#!/usr/bin/env python3
"""Create a small, repository-owned NES ROM with battery-backed PRG RAM.

A 16 KiB NROM image (iNES mapper 0, battery flag set, CHR RAM) whose program
turns NMI and rendering off, writes $A5 and $5A to PRG RAM at $6000 and
$6001, fills the palette with one colour, turns the background on and idles
in a jump to itself. Used where a test needs a game that writes a battery
save and draws a frame that is not black.

What a test can check:

    $6000 = $A5, $6001 = $5A        the battery save (8 KiB at $6000)
    every pixel is palette colour $2A (green): all 32 palette entries hold
    BACKDROP, so the picture does not depend on what the CHR RAM powers up as
    the CPU ends in JMP * at $C04B  (--until-idle)

    python tests/fixtures/make_nes_sram_rom.py --output build-ci/nes_sram.nes
"""

from __future__ import annotations

import argparse
from pathlib import Path

BACKDROP = 0x2A  # NES palette index: green
SAVE = (0xA5, 0x5A)

PROGRAM = bytes(
    [
        0x78,              # C000 SEI
        0xD8,              # C001 CLD
        0xA2, 0xFF,        # C002 LDX #$FF
        0x9A,              # C004 TXS
        0xA9, 0x00,        # C005 LDA #$00
        0x8D, 0x00, 0x20,  # C007 STA $2000   NMI off
        0x8D, 0x01, 0x20,  # C00A STA $2001   rendering off
        0xA9, SAVE[0],     # C00D LDA #$A5
        0x8D, 0x00, 0x60,  # C00F STA $6000
        0xA9, SAVE[1],     # C012 LDA #$5A
        0x8D, 0x01, 0x60,  # C014 STA $6001
        # The PPU ignores its address and control registers for about a
        # frame after power-on: wait for two vertical blanks.
        0x2C, 0x02, 0x20,  # C017 BIT $2002   clear a stale flag
        0x2C, 0x02, 0x20,  # C01A BIT $2002
        0x10, 0xFB,        # C01D BPL $C01A
        0x2C, 0x02, 0x20,  # C01F BIT $2002
        0x10, 0xFB,        # C022 BPL $C01F
        0xA9, 0x3F,        # C024 LDA #$3F
        0x8D, 0x06, 0x20,  # C026 STA $2006
        0xA9, 0x00,        # C029 LDA #$00
        0x8D, 0x06, 0x20,  # C02B STA $2006   PPU address $3F00: the palette
        0xA9, BACKDROP,    # C02E LDA #$2A
        0xA2, 0x20,        # C030 LDX #$20
        0x8D, 0x07, 0x20,  # C032 STA $2007   all 32 entries
        0xCA,              # C035 DEX
        0xD0, 0xFA,        # C036 BNE $C032
        0xA9, 0x00,        # C038 LDA #$00
        0x8D, 0x06, 0x20,  # C03A STA $2006
        0x8D, 0x06, 0x20,  # C03D STA $2006   PPU address back to $0000
        0x8D, 0x05, 0x20,  # C040 STA $2005
        0x8D, 0x05, 0x20,  # C043 STA $2005   no scroll
        0xA9, 0x0A,        # C046 LDA #$0A
        0x8D, 0x01, 0x20,  # C048 STA $2001   background on, left column too
        0x4C, 0x4B, 0xC0,  # C04B JMP $C04B
    ]
)
IDLE = 0xC04B


def make_rom() -> bytes:
    prg = bytearray([0xFF]) * 0x4000
    prg[0 : len(PROGRAM)] = PROGRAM
    assert PROGRAM[-3:] == bytes([0x4C, IDLE & 0xFF, IDLE >> 8]) and 0xC000 + len(PROGRAM) - 3 == IDLE
    # NMI, reset, IRQ: all at the program (only reset is used).
    for vector in range(0x3FFA, 0x4000, 2):
        prg[vector] = 0x00
        prg[vector + 1] = 0xC0
    header = bytearray(16)
    header[0:4] = b"NES\x1a"
    header[4] = 1     # 16 KiB PRG ROM, mirrored at $8000 and $C000
    header[5] = 0     # no CHR ROM: 8 KiB CHR RAM
    header[6] = 0x02  # mapper 0, horizontal mirroring, battery
    return bytes(header) + bytes(prg)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(make_rom())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
