#!/usr/bin/env python3
"""A small two-pass 65C816 assembler for the project's own test ROMs.

The opcode table comes from runtimes/runtime-snes/include/snes_cpu_decode.h (the
SNES_OPINFO lines), the same table the recompiler uses, so the assembler,
the analyzer and the interpreter agree on every encoding.

Syntax (one statement per line, ';' comments):

    label:                  a label (global); 'name = expr' defines a constant
    .org $008000            continue at a 24-bit SNES address (LoROM mapping)
    .a8 .a16 .i8 .i16       accumulator / index width for immediates
    .db 1, $ff, "text"      bytes;  .dw words;  .dl 24-bit longs
    .fill N[, value]        N bytes
    lda #$1234              immediates follow .a/.i width (rep/sep with a
                            literal operand also update it)
    lda.b / .w / .l         force direct page / absolute / long operands
    lda (dp),y  lda [dp],y  lda (dp,x)  lda sr,s  lda (sr,s),y  lda long,x
    jmp (abs)  jmp (abs,x)  jml [abs]  jsr (abs,x)  mvn srcbank, dstbank
    bra label  brl label    relative branches
    jne label (jeq jcc ...) inverse branch over a JMP, for far targets
    lo(x) hi(x) bank(x)     byte selectors in expressions (+ - * & | << >>)

The output is a LoROM image: bank b, address a (a >= $8000) is file offset
(b & $7F) * $8000 + (a - $8000). Unused bytes are $FF.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DECODE_H = REPO / "runtimes" / "runtime-snes" / "include" / "snes_cpu_decode.h"

ALIASES = {"IMP": "IMP"}


def load_opcodes() -> dict[tuple[str, str], int]:
    table = {}
    pattern = re.compile(r'X\((0x[0-9A-Fa-f]{2}), "(\w+)", SNES_AM_(\w+), SNES_FLOW_(\w+)\)')
    for line in DECODE_H.read_text().splitlines():
        m = pattern.search(line)
        if m:
            table[(m.group(2), m.group(3))] = int(m.group(1), 16)
    if len(table) != 256:
        raise SystemExit(f"expected 256 opcodes in {DECODE_H}, found {len(table)}")
    return table


OPCODES = load_opcodes()
MNEMONICS = {mn for mn, _ in OPCODES}
BRANCH8 = {"BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ", "BRA"}
# Long conditional jumps: mnemonic -> the inverse branch that skips a JMP.
LONG_BRANCHES = {"JNE": "BEQ", "JEQ": "BNE", "JCC": "BCS", "JCS": "BCC",
                 "JMI": "BPL", "JPL": "BMI", "JVC": "BVS", "JVS": "BVC"}


class AsmError(Exception):
    pass


def lorom_offset(addr: int) -> int:
    bank, off = addr >> 16, addr & 0xFFFF
    if off < 0x8000:
        raise AsmError(f"address {addr:06X} is not in LoROM space")
    return (bank & 0x7F) * 0x8000 + (off - 0x8000)


class Assembler:
    def __init__(self, size: int):
        self.size = size
        self.symbols: dict[str, int] = {}
        self.constants: set[str] = set()  # names defined with '=' (not addresses)
        self.image = bytearray([0xFF]) * size

    # ---- expressions -------------------------------------------------
    def value(self, text: str, final: bool) -> int | None:
        expr = text.strip()
        expr = re.sub(r"\$([0-9A-Fa-f]+)", lambda m: str(int(m.group(1), 16)), expr)
        expr = re.sub(r"%([01]+)", lambda m: str(int(m.group(1), 2)), expr)
        expr = expr.replace(":", "")  # allow $00:8000 style

        def name(m):
            n = m.group(0)
            if n in ("lo", "hi", "bank"):
                return n
            if n in self.symbols:
                return str(self.symbols[n])
            if final:
                raise AsmError(f"undefined symbol {n}")
            return "0"

        expr = re.sub(r"[A-Za-z_][A-Za-z0-9_.]*", name, expr)
        try:
            return int(eval(expr, {"__builtins__": {}},
                            {"lo": lambda v: v & 0xFF, "hi": lambda v: (v >> 8) & 0xFF,
                             "bank": lambda v: (v >> 16) & 0xFF}))
        except Exception as error:
            raise AsmError(f"bad expression '{text}': {error}")

    # ---- statements --------------------------------------------------
    def assemble(self, lines: list[str], final: bool) -> None:
        self.pc = 0x008000
        self.a8 = self.i8 = True
        for number, raw in enumerate(lines, 1):
            try:
                self.statement(raw, final)
            except AsmError as error:
                raise AsmError(f"line {number}: {error}\n    {raw.strip()}")

    def emit(self, data: bytes, final: bool) -> None:
        if final:
            off = lorom_offset(self.pc)
            if off + len(data) > self.size:
                raise AsmError(f"code at {self.pc:06X} is past the end of the ROM")
            self.image[off:off + len(data)] = data
        self.pc += len(data)

    def split_args(self, text: str) -> list[str]:
        out, depth, cur, quote = [], 0, "", False
        for ch in text:
            if ch == '"':
                quote = not quote
            if not quote and ch in "([":
                depth += 1
            if not quote and ch in ")]":
                depth -= 1
            if ch == "," and depth == 0 and not quote:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    def statement(self, raw: str, final: bool) -> None:
        line = raw.split(";", 1)[0].strip()
        if not line:
            return
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):(.*)$", line)
        if m:
            label = m.group(1)
            if final and self.symbols.get(label) != self.pc:
                raise AsmError(f"label {label} moved between passes")
            self.symbols[label] = self.pc
            line = m.group(2).strip()
            if not line:
                return
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", line)
        if m:
            self.symbols[m.group(1)] = self.value(m.group(2), final) or 0
            self.constants.add(m.group(1))
            return
        parts = line.split(None, 1)
        op, rest = parts[0].lower(), (parts[1] if len(parts) > 1 else "")
        if op.startswith("."):
            return self.directive(op, rest, final)
        size_hint = None
        if "." in op:
            op, size_hint = op.split(".", 1)
        mn = op.upper()
        if mn in LONG_BRANCHES:
            # jne label: the inverse branch over a JMP (for targets beyond
            # a relative branch's reach, within the bank).
            inverse = LONG_BRANCHES[mn]
            self.emit(bytes([OPCODES[(inverse, "REL8")], 3]), final)
            target = self.value(rest, final) or 0
            self.emit(bytes([OPCODES[("JMP", "ABS")], target & 0xFF, (target >> 8) & 0xFF]), final)
            return
        if mn not in MNEMONICS:
            raise AsmError(f"unknown instruction {op}")
        self.instruction(mn, rest.strip(), size_hint, final)

    def directive(self, op: str, rest: str, final: bool) -> None:
        if op == ".org":
            self.pc = self.value(rest, True)
        elif op == ".a8":
            self.a8 = True
        elif op == ".a16":
            self.a8 = False
        elif op == ".i8":
            self.i8 = True
        elif op == ".i16":
            self.i8 = False
        elif op in (".db", ".dw", ".dl"):
            width = {".db": 1, ".dw": 2, ".dl": 3}[op]
            out = bytearray()
            for arg in self.split_args(rest):
                if arg.startswith('"'):
                    out += arg.strip('"').encode("ascii")
                else:
                    v = self.value(arg, final) or 0
                    out += (v & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
            self.emit(bytes(out), final)
        elif op == ".fill":
            args = self.split_args(rest)
            count = self.value(args[0], True)
            fill = self.value(args[1], True) if len(args) > 1 else 0
            self.emit(bytes([fill & 0xFF]) * count, final)
        else:
            raise AsmError(f"unknown directive {op}")

    def instruction(self, mn: str, arg: str, hint: str | None, final: bool) -> None:
        def encode(am: str, operand: int, nbytes: int) -> None:
            key = (mn, am)
            if key not in OPCODES:
                raise AsmError(f"{mn} has no {am} form")
            data = bytes([OPCODES[key]]) + (operand & ((1 << (8 * nbytes)) - 1)).to_bytes(nbytes, "little")
            self.emit(data, final)

        low = arg.lower().replace(" ", "")
        if not arg or low == "a":
            if (mn, "IMP") in OPCODES:
                return encode("IMP", 0, 0)
            raise AsmError(f"{mn} needs an operand")
        if mn in ("MVN", "MVP"):
            src, dst = self.split_args(arg)
            s, d = self.value(src, final) or 0, self.value(dst, final) or 0
            return self.emit(bytes([OPCODES[(mn, "BLOCK")], d & 0xFF, s & 0xFF]), final)
        if mn in BRANCH8:
            target = self.value(arg, final) or 0
            disp = target - (self.pc + 2)
            if final and not -128 <= disp <= 127:
                raise AsmError(f"branch out of range ({disp})")
            return encode("REL8", disp & 0xFF, 1)
        if mn in ("BRL", "PER"):
            target = self.value(arg, final) or 0
            return encode("REL16", (target - (self.pc + 3)) & 0xFFFF, 2)
        if low.startswith("#"):
            v = self.value(arg[1:], final) or 0
            if (mn, "IMM8") in OPCODES:
                if final or True:
                    if mn == "REP":
                        if v & 0x20: self.a8 = False
                        if v & 0x10: self.i8 = False
                    elif mn == "SEP":
                        if v & 0x20: self.a8 = True
                        if v & 0x10: self.i8 = True
                return encode("IMM8", v, 1)
            if (mn, "IMM_M") in OPCODES:
                return encode("IMM_M", v, 1 if self.a8 else 2)
            if (mn, "IMM_X") in OPCODES:
                return encode("IMM_X", v, 1 if self.i8 else 2)
            raise AsmError(f"{mn} has no immediate form")
        m = re.match(r"^\((.+),s\),y$", low)
        if m:
            return encode("SRIY", self.value(m.group(1), final) or 0, 1)
        m = re.match(r"^(.+),s$", low)
        if m:
            return encode("SR", self.value(m.group(1), final) or 0, 1)
        m = re.match(r"^\[(.+)\],y$", low)
        if m:
            return encode("INDLY", self.value(m.group(1), final) or 0, 1)
        m = re.match(r"^\[(.+)\]$", low)
        if m:
            v = self.value(m.group(1), final) or 0
            return encode("ABSINDL", v, 2) if mn == "JML" else encode("INDL", v, 1)
        m = re.match(r"^\((.+)\),y$", low)
        if m:
            return encode("INDY", self.value(m.group(1), final) or 0, 1)
        m = re.match(r"^\((.+),x\)$", low)
        if m:
            v = self.value(m.group(1), final) or 0
            return encode("ABSINDX", v, 2) if mn in ("JMP", "JSR") else encode("INDX", v, 1)
        m = re.match(r"^\((.+)\)$", low)
        if m:
            v = self.value(m.group(1), final) or 0
            return encode("ABSIND", v, 2) if mn == "JMP" else encode("IND", v, 1)
        index = None
        m = re.match(r"^(.+),([xy])$", low)
        expr = arg
        if m:
            index = m.group(2)
            expr = arg[: arg.lower().rfind(",")]
        v = self.value(expr, final) or 0
        # Operand size from the value only when it cannot change between
        # passes: literals and '=' constants. Labels are absolute unless a
        # .b/.l suffix says otherwise.
        names = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", re.sub(r"\$[0-9A-Fa-f]+", "", expr))) - {"lo", "hi", "bank"}
        known = all(n in self.constants for n in names)
        if mn in ("JSL", "JML"):
            return encode("LONG", v, 3)
        if mn in ("JMP", "JSR", "PEA"):
            return encode("ABS", v, 2)
        if mn == "PEI":
            return encode("DP", v, 1)
        size = {"b": 1, "w": 2, "l": 3}.get(hint or "")
        if size is None:
            if known and v < 0x100 and (mn, "DP" + (index.upper() if index else "")) in OPCODES:
                size = 1
            elif known and v > 0xFFFF:
                size = 3
            else:
                size = 2
        if size == 1:
            return encode("DP" + (index.upper() if index else ""), v, 1)
        if size == 2:
            return encode("ABS" + (index.upper() if index else ""), v, 2)
        if index == "y":
            raise AsmError(f"{mn} has no long,y form")
        return encode("LONG" + ("X" if index else ""), v, 3)


def assemble_file(source: Path, size: int) -> tuple[bytes, dict[str, int]]:
    lines = source.read_text().splitlines()
    asm = Assembler(size)
    asm.assemble(lines, final=False)
    asm.assemble(lines, final=True)
    return bytes(asm.image), asm.symbols


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--size", type=lambda s: int(s, 0), default=0x20000)
    parser.add_argument("--symbols", type=Path)
    args = parser.parse_args()
    try:
        image, symbols = assemble_file(args.source, args.size)
    except AsmError as error:
        print(f"{args.source}: {error}", file=sys.stderr)
        return 1
    args.output.write_bytes(image)
    if args.symbols:
        args.symbols.write_text("".join(f"{v:06X} {k}\n" for k, v in sorted(symbols.items(), key=lambda kv: kv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
