#!/usr/bin/env python3
"""Disassemble 65C816 code from a LoROM/HiROM image (development aid).

    python tools/snes/disasm65816.py ROM ADDR [COUNT] [--mode M] [--hirom]

ADDR is a 24-bit SNES address (hex); --mode is the SNES_MODE_* value the
code runs in (0-3 native widths, 7 emulation; default 3 = 8-bit A and X).
REP/SEP with immediate operands update the mode as the listing goes. The
opcode table is the one in runtimes/runtime-snes/include/snes_cpu_decode.h.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TABLE = {}
for line in (REPO / "runtimes" / "runtime-snes" / "include" / "snes_cpu_decode.h").read_text().splitlines():
    m = re.search(r'X\((0x[0-9A-Fa-f]{2}), "(\w+)", SNES_AM_(\w+), SNES_FLOW_(\w+)\)', line)
    if m:
        TABLE[int(m.group(1), 16)] = (m.group(2), m.group(3))

FORMATS = {
    "IMP": "", "IMM8": "#${v:02X}", "IMM_M": "#${v}", "IMM_X": "#${v}", "DP": "${v:02X}", "DPX": "${v:02X},x",
    "DPY": "${v:02X},y", "IND": "(${v:02X})", "INDX": "(${v:02X},x)", "INDY": "(${v:02X}),y", "INDL": "[${v:02X}]",
    "INDLY": "[${v:02X}],y", "SR": "${v:02X},s", "SRIY": "(${v:02X},s),y", "ABS": "${v:04X}", "ABSX": "${v:04X},x",
    "ABSY": "${v:04X},y", "LONG": "${v:06X}", "LONGX": "${v:06X},x", "ABSIND": "(${v:04X})",
    "ABSINDX": "(${v:04X},x)", "ABSINDL": "[${v:04X}]", "REL8": "", "REL16": "", "BLOCK": "",
}


def length(op: int, mode: int) -> int:
    am = TABLE[op][1]
    if am == "IMP":
        return 1
    if am == "IMM_M":
        return 2 if mode & 2 else 3
    if am == "IMM_X":
        return 2 if mode & 1 else 3
    if am in ("ABS", "ABSX", "ABSY", "ABSIND", "ABSINDX", "ABSINDL", "REL16", "BLOCK"):
        return 3
    if am in ("LONG", "LONGX"):
        return 4
    return 2


def offset(addr: int, hirom: bool) -> int:
    bank, off = (addr >> 16) & 0x7F, addr & 0xFFFF
    if hirom:
        return ((bank & 0x3F) << 16) | off
    return (bank << 15) | (off & 0x7FFF)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("rom", type=Path)
    parser.add_argument("addr", type=lambda s: int(s.replace(":", ""), 16))
    parser.add_argument("count", type=int, nargs="?", default=32)
    parser.add_argument("--mode", type=int, default=3)
    parser.add_argument("--hirom", action="store_true")
    args = parser.parse_args()
    data = args.rom.read_bytes()
    if len(data) % 1024 == 512:
        data = data[512:]
    pc, mode = args.addr, args.mode
    for _ in range(args.count):
        op = data[offset(pc, args.hirom) % len(data)]
        mn, am = TABLE[op]
        n = length(op, mode)
        raw = [data[offset((pc & 0xFF0000) | ((pc + i) & 0xFFFF), args.hirom) % len(data)] for i in range(n)]
        v = int.from_bytes(bytes(raw[1:]), "little") if n > 1 else 0
        if am == "REL8":
            text = f"${(pc + 2 + (v - 256 if v & 0x80 else v)) & 0xFFFF:04X}"
        elif am == "REL16":
            text = f"${(pc + 3 + v) & 0xFFFF:04X}"
        elif am == "BLOCK":
            text = f"${raw[2]:02X},${raw[1]:02X}"
        elif am in ("IMM_M", "IMM_X"):
            text = f"#${v:0{2 * (n - 1)}X}"
        else:
            text = FORMATS[am].format(v=v).replace("{v}", str(v))
        print(f"{pc >> 16:02X}:{pc & 0xFFFF:04X}  {' '.join(f'{b:02X}' for b in raw):12s} {mn.lower()} {text}")
        if mn in ("REP", "SEP") and mode != 7:
            mode = (mode & ~((v >> 4) & 3)) if mn == "REP" else (mode | ((v >> 4) & 3))
        pc = (pc & 0xFF0000) | ((pc + n) & 0xFFFF)


if __name__ == "__main__":
    main()
