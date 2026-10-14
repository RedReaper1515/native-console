#!/usr/bin/env python3
"""A small two-pass 6502 assembler for the project's own NES test ROMs.

The opcode table comes from runtimes/runtime-nes/include/nes_cpu_decode.h (the
NES_OPINFO_TABLE lines), the same table the recompiler and the disassembler
use, so the assembler, the analyzer and the interpreter agree on every
encoding.

Syntax (one statement per line, ';' comments):

    label:                  a label; 'name = expr' defines a constant
    .bank N                 continue in PRG bank N (16 KiB unless told otherwise)
    .org $C000              continue at a CPU address inside the current bank
    .rorg $0200 / .rend     assemble what follows as if it ran at $0200 (code
                            that is copied to RAM); .rend returns to ROM addresses
    .pad $DFFF[, value]     fill up to an address (default $EA, NOP)
    .db 1, $ff, "text"      bytes;  .dw little-endian words
    .fill N[, value]        N bytes
    .unofficial / .official allow / refuse the undocumented opcodes
    lda #$12                immediate
    lda $12    lda $12,x    zero page when the operand is a literal or an '='
    lda $1234  lda $1234,y  constant below $100, absolute otherwise (labels
                            are always absolute)
    lda.b / lda.w           force zero page / absolute
    lda ($12,x)  lda ($12),y  jmp ($1234)
    asl a  /  asl           accumulator
    bne label               relative branches
    jne label (jeq jcc ...) the inverse branch over a JMP, for far targets
    brk                     one byte ($00); 'brk #n' also emits the byte the
                            CPU skips
    lo(x) hi(x)             byte selectors; + - * / & | ^ << >> ~ ( );
                            $hex %binary decimal 'c'; '*' alone is the
                            address of the statement

Only the documented opcodes are accepted until '.unofficial' (or
Assembler(unofficial=True)); the undocumented ones use the names of the
decode header (LAX, SAX, DCP, ISC, SLO, ..., and NOP with an operand).
Where one mnemonic and mode has several encodings, the documented one is
used, otherwise the lowest.

Output: PRG ROM, bank after bank. Inside a bank an address's place is
address & (bank size - 1), so a 16 KiB bank assembles the same at $8000 or
$C000. Unused bytes are $FF. Emitting over bytes that were already emitted
is an error. write_ines() wraps PRG (and CHR) in an iNES header.

    python tools/nes/asm6502.py game.asm -o game.nes --banks 2 --mapper 0
    python tools/nes/asm6502.py --selftest
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DECODE_H = REPO / "runtimes" / "runtime-nes" / "include" / "nes_cpu_decode.h"

# Operand bytes per addressing mode.
MODE_BYTES = {"IMP": 0, "ACC": 0, "IMM": 1, "ZP": 1, "ZPX": 1, "ZPY": 1, "ABS": 2, "ABSX": 2, "ABSY": 2,
              "IND": 2, "INDX": 1, "INDY": 1, "REL": 1}


def load_opinfo() -> list[tuple[str, str, str, bool, bool]]:
    """(mnemonic, mode, flow, official, writes) for each of the 256 opcodes."""
    pattern = re.compile(r'X\((0x[0-9A-Fa-f]{2}), "(\w+)", NES_AM_(\w+), NES_FLOW_(\w+), ([^)]*)\)')
    table: dict[int, tuple[str, str, str, bool, bool]] = {}
    for line in DECODE_H.read_text().splitlines():
        m = pattern.search(line)
        if m:
            opcode = int(m.group(1), 16)
            if opcode in table:
                raise SystemExit(f"opcode {opcode:02X} appears twice in {DECODE_H}")
            if m.group(3) not in MODE_BYTES:
                raise SystemExit(f"unknown addressing mode NES_AM_{m.group(3)} in {DECODE_H}")
            flags = m.group(5)
            table[opcode] = (m.group(2), m.group(3), m.group(4), "NES_OPF_OFFICIAL" in flags, "NES_OPF_WRITES" in flags)
    if len(table) != 256:
        raise SystemExit(f"expected 256 opcodes in {DECODE_H}, found {len(table)}")
    return [table[i] for i in range(256)]


OPINFO = load_opinfo()


def build_tables() -> tuple[dict[tuple[str, str], int], dict[tuple[str, str], int]]:
    """(mnemonic, mode) -> opcode: documented only, and all (documented first, then lowest)."""
    official: dict[tuple[str, str], int] = {}
    every: dict[tuple[str, str], int] = {}
    for opcode, (mn, mode, _flow, is_official, _writes) in enumerate(OPINFO):
        if is_official:
            if (mn, mode) in official:
                raise SystemExit(f"two documented encodings of {mn} {mode}")
            official[(mn, mode)] = opcode
    for opcode, (mn, mode, _flow, _official, _writes) in enumerate(OPINFO):
        every.setdefault((mn, mode), official.get((mn, mode), opcode))
    return official, every


OFFICIAL, OPCODES = build_tables()
if len(OFFICIAL) != 151:
    raise SystemExit(f"expected 151 documented opcodes in {DECODE_H}, found {len(OFFICIAL)}")
MNEMONICS = {mn for mn, _ in OPCODES}
BRANCHES = {mn for mn, mode in OPCODES if mode == "REL"}
# Long conditional jumps: mnemonic -> the inverse branch that skips a JMP.
LONG_BRANCHES = {"JNE": "BEQ", "JEQ": "BNE", "JCC": "BCS", "JCS": "BCC",
                 "JMI": "BPL", "JPL": "BMI", "JVC": "BVS", "JVS": "BVC"}

TOKEN = re.compile(r"""\s*(?:
      (?P<hex>\$[0-9A-Fa-f]+)
    | (?P<bin>%[01]+)
    | (?P<num>[0-9]+)
    | (?P<chr>'[^']')
    | (?P<name>[A-Za-z_][A-Za-z0-9_.]*)
    | (?P<op><<|>>|[-+*/&|^~()])
    )""", re.VERBOSE)


class AsmError(Exception):
    pass


class Assembler:
    def __init__(self, banks: int = 1, bank_size: int = 0x4000, unofficial: bool = False):
        if bank_size & (bank_size - 1):
            raise AsmError("the bank size must be a power of two")
        self.banks = banks
        self.bank_size = bank_size
        self.default_unofficial = unofficial
        self.symbols: dict[str, int] = {}
        self.label_bank: dict[str, int] = {}  # labels only: the bank each was defined in
        self.constants: set[str] = set()  # names defined with '=' (not addresses)
        self.image = bytearray([0xFF]) * (banks * bank_size)
        self.used = bytearray(banks * bank_size)

    # ---- expressions -------------------------------------------------
    def value(self, text: str, final: bool) -> int:
        """Evaluate an expression. Undefined names are 0 until the final pass."""
        parts: list[str] = []
        pos, operand = 0, True  # operand: a value is expected next ('*' is then the PC)
        text = text.strip()
        if not text:
            raise AsmError("missing expression")
        while pos < len(text):
            m = TOKEN.match(text, pos)
            if not m:
                raise AsmError(f"bad expression '{text}'")
            pos = m.end()
            kind, tok = m.lastgroup, m.group(m.lastgroup)
            if kind == "hex":
                parts.append(str(int(tok[1:], 16)))
            elif kind == "bin":
                parts.append(str(int(tok[1:], 2)))
            elif kind == "num":
                parts.append(str(int(tok, 10)))
            elif kind == "chr":
                parts.append(str(ord(tok[1])))
            elif kind == "name":
                if tok in ("lo", "hi"):
                    parts.append(tok)
                    operand = True
                    continue
                if tok in self.symbols:
                    parts.append(str(self.symbols[tok]))
                elif final:
                    raise AsmError(f"undefined symbol {tok}")
                else:
                    parts.append("0")
            else:
                if tok == "*" and operand:
                    parts.append(str(self.pc))
                    operand = False
                    continue
                parts.append("//" if tok == "/" else tok)
                operand = tok != ")"
                continue
            operand = False
        try:
            return int(eval(" ".join(parts), {"__builtins__": {}},
                            {"lo": lambda v: v & 0xFF, "hi": lambda v: (v >> 8) & 0xFF}))
        except Exception as error:
            raise AsmError(f"bad expression '{text}': {error}")

    def is_constant(self, text: str) -> bool:
        """The expression's value cannot change between passes: literals and '=' names only."""
        names = {m.group("name") for m in TOKEN.finditer(text) if m.lastgroup == "name"} - {"lo", "hi"}
        stars = any(m.lastgroup == "op" and m.group("op") == "*" for m in TOKEN.finditer(text))
        return not stars and all(n in self.constants for n in names)

    # ---- output ------------------------------------------------------
    def assemble(self, lines: list[str], final: bool) -> None:
        self.bank = 0
        self.pc = 0x8000  # the address labels get
        self.out = 0      # the offset inside the bank that bytes go to
        self.relocated = False
        self.unofficial = self.default_unofficial
        if final:
            self.used = bytearray(len(self.used))
        for number, raw in enumerate(lines, 1):
            try:
                self.statement(raw, final)
            except AsmError as error:
                raise AsmError(f"line {number}: {error}\n    {raw.strip()}")

    def emit(self, data: bytes, final: bool) -> None:
        if self.out + len(data) > self.bank_size:
            raise AsmError(f"bank {self.bank}: {len(data)} byte(s) at {self.pc:04X} run past the end of the bank")
        if final:
            at = self.bank * self.bank_size + self.out
            if any(self.used[at:at + len(data)]):
                raise AsmError(f"bank {self.bank}: {self.pc:04X} was already assembled (overlap)")
            self.image[at:at + len(data)] = data
            self.used[at:at + len(data)] = b"\x01" * len(data)
        self.pc += len(data)
        self.out += len(data)

    @staticmethod
    def split_args(text: str) -> list[str]:
        out, depth, cur, quote = [], 0, "", ""
        for ch in text:
            if quote:
                if ch == quote:
                    quote = ""
            elif ch in "\"'":
                quote = ch
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            if ch == "," and depth == 0 and not quote:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    @staticmethod
    def strip_comment(raw: str) -> str:
        quote = ""
        for i, ch in enumerate(raw):
            if quote:
                if ch == quote:
                    quote = ""
            elif ch in "\"'":
                quote = ch
            elif ch == ";":
                return raw[:i].strip()
        return raw.strip()

    # ---- statements --------------------------------------------------
    def statement(self, raw: str, final: bool) -> None:
        line = self.strip_comment(raw)
        if not line:
            return
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):(.*)$", line)
        if m:
            label = m.group(1)
            if label.lower() in ("a", "x", "y", "lo", "hi"):
                raise AsmError(f"'{label}' cannot be a label")
            if final:
                if self.symbols.get(label) != self.pc or self.label_bank.get(label) != self.bank:
                    raise AsmError(f"label {label} moved between passes")
            elif label in self.symbols:
                raise AsmError(f"{label} is defined twice")
            self.symbols[label] = self.pc
            self.label_bank[label] = self.bank
            line = m.group(2).strip()
            if not line:
                return
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", line)
        if m:
            name = m.group(1)
            if name in self.label_bank:
                raise AsmError(f"{name} is defined twice")
            self.symbols[name] = self.value(m.group(2), final)
            self.constants.add(name)
            return
        parts = line.split(None, 1)
        op, rest = parts[0].lower(), (parts[1].strip() if len(parts) > 1 else "")
        if op.startswith("."):
            return self.directive(op, rest, final)
        size_hint = None
        if "." in op:
            op, size_hint = op.split(".", 1)
            if size_hint not in ("b", "w"):
                raise AsmError(f"unknown size suffix .{size_hint}")
        mn = op.upper()
        if mn in LONG_BRANCHES:
            # jne label: the inverse branch over a JMP.
            target = self.value(rest, final)
            self.emit(bytes([OFFICIAL[(LONG_BRANCHES[mn], "REL")], 3, OFFICIAL[("JMP", "ABS")],
                             target & 0xFF, (target >> 8) & 0xFF]), final)
            return
        if mn not in MNEMONICS:
            raise AsmError(f"unknown instruction {op}")
        self.instruction(mn, rest, size_hint, final)

    def directive(self, op: str, rest: str, final: bool) -> None:
        if op == ".bank":
            bank = self.value(rest, True)
            if not 0 <= bank < self.banks:
                raise AsmError(f"bank {bank} does not exist (the ROM has {self.banks})")
            self.bank = bank
            self.relocated = False
            self.pc = 0x10000 - self.bank_size if self.bank_size <= 0x8000 else 0
            self.out = 0
        elif op == ".org":
            addr = self.value(rest, True)
            if not 0 <= addr <= 0xFFFF:
                raise AsmError(f"address {addr:X} is not a CPU address")
            self.pc = addr
            self.out = addr & (self.bank_size - 1)
            self.relocated = False
        elif op == ".rorg":
            if self.relocated:
                raise AsmError(".rorg inside .rorg")
            self.resume = self.pc - self.out  # the ROM address of offset 0, to come back to
            self.pc = self.value(rest, True)
            self.relocated = True
        elif op == ".rend":
            if not self.relocated:
                raise AsmError(".rend without .rorg")
            self.pc = self.resume + self.out
            self.relocated = False
        elif op == ".pad":
            args = self.split_args(rest)
            target = self.value(args[0], True)
            fill = self.value(args[1], True) if len(args) > 1 else 0xEA
            if target < self.pc:
                raise AsmError(f".pad to {target:04X}, but the code is already at {self.pc:04X}")
            self.emit(bytes([fill & 0xFF]) * (target - self.pc), final)
        elif op in (".db", ".dw"):
            width = 1 if op == ".db" else 2
            out = bytearray()
            for arg in self.split_args(rest):
                if arg.startswith('"'):
                    if len(arg) < 2 or not arg.endswith('"'):
                        raise AsmError("unterminated string")
                    out += arg[1:-1].encode("ascii")
                else:
                    v = self.value(arg, final)
                    if final and not -(1 << (8 * width - 1)) <= v < (1 << (8 * width)):
                        raise AsmError(f"{v} does not fit in {width} byte(s)")
                    out += (v & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
            self.emit(bytes(out), final)
        elif op == ".fill":
            args = self.split_args(rest)
            count = self.value(args[0], True)
            fill = self.value(args[1], True) if len(args) > 1 else 0
            self.emit(bytes([fill & 0xFF]) * count, final)
        elif op == ".unofficial":
            self.unofficial = True
        elif op == ".official":
            self.unofficial = False
        else:
            raise AsmError(f"unknown directive {op}")

    def instruction(self, mn: str, arg: str, hint: str | None, final: bool) -> None:
        table = OPCODES if self.unofficial else OFFICIAL

        def has(mode: str) -> bool:
            return (mn, mode) in table

        def encode(mode: str, operand: int = 0) -> None:
            if not has(mode):
                if (mn, mode) in OPCODES:
                    raise AsmError(f"{mn} {mode} is an undocumented opcode (use .unofficial)")
                raise AsmError(f"{mn} has no {mode} form")
            nbytes = MODE_BYTES[mode]
            if final and nbytes and not -(1 << (8 * nbytes - 1)) <= operand < (1 << (8 * nbytes)):
                raise AsmError(f"operand {operand:X} does not fit in {nbytes} byte(s)")
            self.emit(bytes([table[(mn, mode)]]) + (operand & ((1 << (8 * nbytes)) - 1)).to_bytes(nbytes, "little"),
                      final)

        low = arg.lower().replace(" ", "")
        if not any(m == mn for m, _ in table):
            raise AsmError(f"{mn} is an undocumented opcode (use .unofficial)")
        if not arg or low == "a":
            if mn == "BRK":
                return self.emit(bytes([OFFICIAL[("BRK", "IMM")]]), final)
            if has("ACC"):
                return encode("ACC")
            if not arg and (mn, "IMP") in OPCODES:
                return encode("IMP")
            raise AsmError(f"{mn} needs an operand")
        if mn in BRANCHES:
            target = self.value(arg, final)
            disp = target - (self.pc + 2)
            if final and not -128 <= disp <= 127:
                raise AsmError(f"branch out of range ({disp})")
            return encode("REL", disp & 0xFF)
        if low.startswith("#"):
            return encode("IMM", self.value(arg[1:], final))
        if low.startswith("("):
            m = re.match(r"^\((.+),x\)$", low)
            if m:
                return encode("INDX", self.value(arg[arg.index("(") + 1:arg.rindex(",")], final))
            m = re.match(r"^\((.+)\),y$", low)
            if m:
                return encode("INDY", self.value(arg[arg.index("(") + 1:arg.rindex(")")], final))
            m = re.match(r"^\((.+)\)$", low)
            if m:
                return encode("IND", self.value(arg[arg.index("(") + 1:arg.rindex(")")], final))
            raise AsmError(f"bad indirect operand '{arg}'")
        index = ""
        expr = arg
        m = re.match(r"^(.+),([xy])$", low)
        if m:
            index = m.group(2).upper()
            expr = arg[:arg.rindex(",")]
        v = self.value(expr, final)
        # Operand size from the value only when it cannot change between
        # passes: literals and '=' constants. Labels are absolute unless a
        # .b suffix says otherwise.
        zp_mode, abs_mode = "ZP" + index, "ABS" + index
        if hint == "b":
            return encode(zp_mode, v)
        if hint == "w":
            return encode(abs_mode, v)
        if self.is_constant(expr) and 0 <= v < 0x100 and (mn, zp_mode) in OPCODES:
            return encode(zp_mode, v)
        return encode(abs_mode, v)


def assemble(source: str, banks: int = 1, bank_size: int = 0x4000,
             unofficial: bool = False) -> tuple[bytes, dict[str, int], dict[str, int]]:
    """Assemble source text. Returns (PRG ROM, symbols, the bank of each label)."""
    asm = Assembler(banks, bank_size, unofficial)
    lines = source.splitlines()
    asm.assemble(lines, final=False)
    asm.assemble(lines, final=True)
    return bytes(asm.image), asm.symbols, asm.label_bank


def ines_header(prg_size: int, chr_size: int = 0, mapper: int = 0, mirroring: str = "horizontal",
                battery: bool = False, prg_ram: int | None = None, chr_ram: int | None = None,
                submapper: int = 0, nes20: bool = False) -> bytes:
    """The 16-byte header.

    Plain iNES unless that cannot say what is asked: a mapper above 255, a
    submapper, more ROM than its size bytes count, or a RAM size other than
    the 8 KiB it implies (prg_ram / chr_ram None: whatever the format
    implies). Then NES 2.0.
    """
    if prg_size == 0 or prg_size % 0x4000:
        raise ValueError("PRG ROM must be a non-zero multiple of 16 KiB")
    if chr_size % 0x2000:
        raise ValueError("CHR ROM must be a multiple of 8 KiB")
    flags6 = {"horizontal": 0, "vertical": 1, "four": 8}[mirroring] | (2 if battery else 0)
    prg_units, chr_units = prg_size // 0x4000, chr_size // 0x2000
    implied_chr_ram = 0 if chr_size else 0x2000
    nes20 = (nes20 or mapper > 255 or submapper != 0 or prg_units > 255 or chr_units > 255 or
             prg_ram not in (None, 0x2000) or chr_ram not in (None, implied_chr_ram))
    header = bytearray(16)
    header[0:4] = b"NES\x1a"
    header[4] = prg_units & 0xFF
    header[5] = chr_units & 0xFF
    header[6] = flags6 | ((mapper & 0x0F) << 4)
    header[7] = mapper & 0xF0
    if not nes20:
        return bytes(header)

    def shift(size: int) -> int:
        if size == 0:
            return 0
        count = size.bit_length() - 7  # 64 << count bytes
        if count < 1 or count > 15 or size != 64 << count:
            raise ValueError(f"RAM size {size} is not 64 << n bytes")
        return count

    if mapper > 0xFFF or submapper > 15 or prg_units > 0xEFF or chr_units > 0xEFF:
        raise ValueError("value out of range for a NES 2.0 header")
    header[7] |= 0x08
    header[8] = (mapper >> 8) | (submapper << 4)
    header[9] = (prg_units >> 8) | ((chr_units >> 8) << 4)
    ram = shift(0x2000 if prg_ram is None else prg_ram)
    header[10] = ram << 4 if battery else ram
    header[11] = shift(implied_chr_ram if chr_ram is None else chr_ram)
    return bytes(header)


def write_ines(path: Path | str, prg: bytes, chr: bytes = b"", mapper: int = 0, mirroring: str = "horizontal",
               battery: bool = False, prg_ram: int | None = None, **more) -> None:
    """Write PRG (and CHR) ROM as an iNES file; see ines_header()."""
    header = ines_header(len(prg), len(chr), mapper, mirroring, battery, prg_ram, **more)
    Path(path).write_bytes(header + bytes(prg) + bytes(chr))


# ---------------------------------------------------------------------------
# Self-test: independent of the emulator.
# ---------------------------------------------------------------------------

# The 151 documented opcodes, from the MOS 6502 programming manual's
# instruction tables, written out by mnemonic (not derived from the decode
# header, so this checks the header too).
REFERENCE = """
ADC IMM 69 ZP 65 ZPX 75 ABS 6D ABSX 7D ABSY 79 INDX 61 INDY 71
AND IMM 29 ZP 25 ZPX 35 ABS 2D ABSX 3D ABSY 39 INDX 21 INDY 31
ASL ACC 0A ZP 06 ZPX 16 ABS 0E ABSX 1E
BCC REL 90
BCS REL B0
BEQ REL F0
BIT ZP 24 ABS 2C
BMI REL 30
BNE REL D0
BPL REL 10
BRK IMM 00
BVC REL 50
BVS REL 70
CLC IMP 18
CLD IMP D8
CLI IMP 58
CLV IMP B8
CMP IMM C9 ZP C5 ZPX D5 ABS CD ABSX DD ABSY D9 INDX C1 INDY D1
CPX IMM E0 ZP E4 ABS EC
CPY IMM C0 ZP C4 ABS CC
DEC ZP C6 ZPX D6 ABS CE ABSX DE
DEX IMP CA
DEY IMP 88
EOR IMM 49 ZP 45 ZPX 55 ABS 4D ABSX 5D ABSY 59 INDX 41 INDY 51
INC ZP E6 ZPX F6 ABS EE ABSX FE
INX IMP E8
INY IMP C8
JMP ABS 4C IND 6C
JSR ABS 20
LDA IMM A9 ZP A5 ZPX B5 ABS AD ABSX BD ABSY B9 INDX A1 INDY B1
LDX IMM A2 ZP A6 ZPY B6 ABS AE ABSY BE
LDY IMM A0 ZP A4 ZPX B4 ABS AC ABSX BC
LSR ACC 4A ZP 46 ZPX 56 ABS 4E ABSX 5E
NOP IMP EA
ORA IMM 09 ZP 05 ZPX 15 ABS 0D ABSX 1D ABSY 19 INDX 01 INDY 11
PHA IMP 48
PHP IMP 08
PLA IMP 68
PLP IMP 28
ROL ACC 2A ZP 26 ZPX 36 ABS 2E ABSX 3E
ROR ACC 6A ZP 66 ZPX 76 ABS 6E ABSX 7E
RTI IMP 40
RTS IMP 60
SBC IMM E9 ZP E5 ZPX F5 ABS ED ABSX FD ABSY F9 INDX E1 INDY F1
SEC IMP 38
SED IMP F8
SEI IMP 78
STA ZP 85 ZPX 95 ABS 8D ABSX 9D ABSY 99 INDX 81 INDY 91
STX ZP 86 ZPY 96 ABS 8E
STY ZP 84 ZPX 94 ABS 8C
TAX IMP AA
TAY IMP A8
TSX IMP BA
TXA IMP 8A
TXS IMP 9A
TYA IMP 98
"""

# A few undocumented opcodes real games use, from the NESdev wiki's "CPU
# unofficial opcodes" page.
REFERENCE_UNOFFICIAL = """
LAX ZP A7 ZPY B7 ABS AF ABSY BF INDX A3 INDY B3
SAX ZP 87 ZPY 97 ABS 8F INDX 83
DCP ZP C7 ZPX D7 ABS CF ABSX DF ABSY DB INDX C3 INDY D3
ISC ZP E7 ZPX F7 ABS EF ABSX FF ABSY FB INDX E3 INDY F3
SLO ZP 07 ZPX 17 ABS 0F ABSX 1F ABSY 1B INDX 03 INDY 13
RLA ZP 27 ZPX 37 ABS 2F ABSX 3F ABSY 3B INDX 23 INDY 33
SRE ZP 47 ZPX 57 ABS 4F ABSX 5F ABSY 5B INDX 43 INDY 53
RRA ZP 67 ZPX 77 ABS 6F ABSX 7F ABSY 7B INDX 63 INDY 73
NOP IMM 80 ZP 04 ZPX 14 ABS 0C ABSX 1C
"""

OPERAND_TEXT = {"IMP": "", "ACC": "a", "IMM": "#$5A", "ZP": "$5A", "ZPX": "$5A,x", "ZPY": "$5A,y",
                "ABS": "$1234", "ABSX": "$1234,x", "ABSY": "$1234,y", "IND": "($1234)", "INDX": "($5A,x)",
                "INDY": "($5A),y", "REL": "*+$12"}
OPERAND_BYTES = {"IMP": b"", "ACC": b"", "IMM": b"\x5A", "ZP": b"\x5A", "ZPX": b"\x5A", "ZPY": b"\x5A",
                 "ABS": b"\x34\x12", "ABSX": b"\x34\x12", "ABSY": b"\x34\x12", "IND": b"\x34\x12",
                 "INDX": b"\x5A", "INDY": b"\x5A", "REL": b"\x10"}


def parse_reference(text: str) -> dict[tuple[str, str], int]:
    out = {}
    for line in text.split("\n"):
        words = line.split()
        for mode, code in zip(words[1::2], words[2::2]):
            out[(words[0], mode)] = int(code, 16)
    return out


def selftest() -> int:
    failures: list[str] = []

    def check(what: str, ok: bool) -> None:
        if not ok:
            failures.append(what)

    def one(text: str, unofficial: bool = False) -> bytes | str:
        try:
            image, _, _ = assemble(".org $8000\n" + text + "\n", unofficial=unofficial)
        except AsmError as error:
            return str(error)
        end = len(image)
        while end and image[end - 1] == 0xFF:
            end -= 1
        return image[:end]

    # 1. The decode header against the reference tables.
    reference = parse_reference(REFERENCE)
    check("the reference lists 151 opcodes", len(reference) == 151 and len(set(reference.values())) == 151)
    check("the header's documented opcodes are the reference's", OFFICIAL == reference)
    for key, code in parse_reference(REFERENCE_UNOFFICIAL).items():
        check(f"undocumented {key} is ${code:02X}", OPCODES.get(key) == code and not OPINFO[code][3])

    # 2. Round trip: every opcode of the header, assembled from its
    #    mnemonic and mode, gives the canonical opcode and the operand
    #    bytes; every documented opcode gives exactly itself.
    for opcode, (mn, mode, _flow, official, _writes) in enumerate(OPINFO):
        text = f"{mn.lower()} {OPERAND_TEXT[mode]}".strip()
        if mn == "BRK":
            text = "brk #$5A"
        forced = {"ZP": ".b", "ZPX": ".b", "ZPY": ".b", "ABS": ".w", "ABSX": ".w", "ABSY": ".w"}.get(mode, "")
        want = bytes([OPCODES[(mn, mode)]]) + OPERAND_BYTES[mode]
        for variant in {text, text.replace(mn.lower(), mn.lower() + forced, 1)}:
            got = one(variant, unofficial=True)
            check(f"${opcode:02X} '{variant}' -> {want.hex()} (got {got if isinstance(got, str) else got.hex()})",
                  got == want)
        if official:
            check(f"${opcode:02X} is the canonical encoding of {mn} {mode}", OPCODES[(mn, mode)] == opcode)
            check(f"${opcode:02X} '{text}' assembles without .unofficial", one(text) == want)
        else:
            canonical = OPCODES[(mn, mode)]
            check(f"${opcode:02X} {mn} {mode}: canonical ${canonical:02X} is documented or the lowest",
                  OPINFO[canonical][3] or canonical <= opcode)
            if (mn, mode) not in OFFICIAL:
                check(f"${opcode:02X} '{text}' is refused without .unofficial", isinstance(one(text), str))

    # 3. Known byte sequences (hand-assembled).
    known = [
        ("sei\ncld\nldx #$FF\ntxs", "78 D8 A2 FF 9A"),
        ("lda #$00\nsta $2000\nsta $2001", "A9 00 8D 00 20 8D 01 20"),
        ("l: bit $2002\nbpl l", "2C 02 20 10 FB"),
        ("l: jmp l", "4C 00 80"),
        ("jmp *", "4C 00 80"),
        ("nop\nl: jmp (l+2)", "EA 6C 03 80"),
        ("lda $10\nlda $0010\nlda.w $10\nlda.b $0010", "A5 10 A5 10 AD 10 00 A5 10"),
        ("v = $10\nlda v\nlda v+1,x\nldx v,y\nlda v,y", "A5 10 B5 11 B6 10 B9 10 00"),
        ("lda ($10,x)\nlda ($10),y\nsta (v+1),y\nv = $20", "A1 10 B1 10 91 21"),
        ("lda v\nl: rts\nv = $20", None),  # a constant must be defined before a zero-page use
        ("l: lda l\nlda l,x\nlda lo(l)\nlda.b lo(l)\nlda #hi(l)\nlda #lo(l+$12)",
         "AD 00 80 BD 00 80 AD 00 00 A5 00 A9 80 A9 12"),
        ("asl\nasl a\nlsr a\nrol\nror a\nasl $10\nasl $1234,x", "0A 0A 4A 2A 6A 06 10 1E 34 12"),
        ("brk\nbrk #$EA\nrti", "00 00 EA 40"),
        ("ldx #5\nl: dex\nbne l\nbeq f\nnop\nf: rts", "A2 05 CA D0 FD F0 01 EA 60"),
        ("jne t\njeq t\njcc t\njcs t\njmi t\njpl t\njvc t\njvs t\nt: rts",
         "F0 03 4C 28 80 D0 03 4C 28 80 B0 03 4C 28 80 90 03 4C 28 80 "
         "10 03 4C 28 80 30 03 4C 28 80 70 03 4C 28 80 50 03 4C 28 80 60"),
        ('.db 1, $FF, "AB", \'c\', %101\n.dw $1234, t\n.fill 3, $EE\nt: .db lo(t), hi(t)',
         "01 FF 41 42 63 05 34 12 0D 80 EE EE EE 0D 80"),
        ("lda #(1+2)*3\nlda #$F0>>4\nlda #1<<7|1\nlda #$FF&~$0F\nlda #7/2\nlda #-2&$FF",
         "A9 09 A9 0F A9 81 A9 F0 A9 03 A9 FE"),
        ("stx $10,y\nsty $10,x\nlda $FF,x\nsta $0100,x", "96 10 94 10 B5 FF 9D 00 01"),
        ("jsr s\n.db $12, $34\ns: pla\nrts", "20 05 80 12 34 68 60"),
        (".pad $8003\nrts\n.pad $8006, $00\nrts", "EA EA EA 60 00 00 60"),
        (".rorg $0200\nr: lda r\njmp r\n.rend\nl: jmp l", "AD 00 02 4C 00 02 4C 06 80"),
    ]
    for text, want in known:
        got = one(text)
        if want is None:
            check(f"'{text}' must fail (constant used before its definition)", isinstance(got, str))
        else:
            check(f"'{text}' -> {want} (got {got if isinstance(got, str) else got.hex(' ').upper()})",
                  got == bytes.fromhex(want))
    check("unofficial NOPs and LAX/SAX/DCP/ISC/SLO",
          one("nop #$12\nnop $12\nnop $12,x\nnop $1234\nnop $1234,x\nnop\nlax $12\nsax $12\ndcp $12\nisc $12\nslo $12\n"
              "sbc #$12", True) == bytes.fromhex("8012 0412 1412 0C3412 1C3412 EA A712 8712 C712 E712 0712 E912"))

    # 4. Errors that must be caught.
    for text, why in [("bne far\n.fill 200\nfar: rts", "branch out of range"), ("lda #$100", "immediate too large"),
                      ("lda ($1234),y", "pointer too large"), ("sta #$12", "no such mode"), ("xyz", "unknown mnemonic"),
                      ("l: rts\nl: rts", "duplicate label"), ("lda nowhere", "undefined symbol"),
                      ("rts\n.org $8000\nrts", "overlap"), ("lax $12", "undocumented without .unofficial"),
                      ("jmp ($12),y", "bad mode"), (".org $BFFF\nlda #1", "past the end of the bank")]:
        check(f"'{text}' must fail ({why})", isinstance(one(text), str))

    # 5. Banks: the same address in two banks, and both halves of the map.
    image, symbols, label_bank = assemble(
        ".bank 0\n.org $8000\nb0: lda #$A0\nrts\n.bank 1\n.org $8000\nb1: lda #$B1\nrts\n"
        ".bank 3\n.org $C000\nreset: jmp reset\n.org $FFFA\n.dw reset, reset, reset\n", banks=4)
    check("four 16 KiB banks", len(image) == 0x10000)
    check("bank 0 at $8000", image[0:3] == bytes.fromhex("A9A060"))
    check("bank 1 at $8000", image[0x4000:0x4003] == bytes.fromhex("A9B160"))
    check("bank 3 at $C000 and its vectors", image[0xC000:0xC003] == bytes.fromhex("4C00C0") and
          image[0xFFFA:] == bytes.fromhex("00C000C000C0"))
    check("labels and their banks", symbols["b0"] == symbols["b1"] == 0x8000 and label_bank["b1"] == 1 and
          label_bank["reset"] == 3)
    check("unused bytes are $FF", image[0x8000:0xC000] == b"\xFF" * 0x4000)
    image, _, _ = assemble(".bank 1\n.org $A000\nrts\n", banks=2, bank_size=0x2000)
    check("8 KiB banks", len(image) == 0x4000 and image[0x2000] == 0x60)

    # 6. Headers.
    check("NROM, 16 KiB, battery", ines_header(0x4000, battery=True) == b"NES\x1a\x01\x00\x02" + bytes(9))
    check("MMC1, 64 KiB, CHR RAM", ines_header(0x10000, mapper=1, prg_ram=0x2000) == b"NES\x1a\x04\x00\x10" + bytes(9))
    check("mapper 66, vertical, CHR ROM",
          ines_header(0x8000, 0x4000, mapper=66, mirroring="vertical") == b"NES\x1a\x02\x02\x21\x40" + bytes(8))
    check("NES 2.0 when the PRG RAM is not 8 KiB",
          ines_header(0x8000, mapper=1, battery=True, prg_ram=0x8000) ==
          b"NES\x1a\x02\x00\x12\x08\x00\x00\x90\x07" + bytes(4))
    check("NES 2.0 for mapper 256",
          ines_header(0x4000, 0x2000, mapper=256)[6:12] == bytes([0x00, 0x08, 0x01, 0x00, 0x07, 0x00]))

    for line in failures:
        print("FAIL " + line)
    print(f"asm6502 self-test: {'ok' if not failures else f'{len(failures)} failure(s)'}")
    return 1 if failures else 0


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--banks", type=int, default=1, help="PRG banks (default 1)")
    parser.add_argument("--bank-size", type=lambda s: int(s, 0), default=0x4000)
    parser.add_argument("--unofficial", action="store_true", help="accept the undocumented opcodes")
    parser.add_argument("--raw", action="store_true", help="write PRG ROM only, without an iNES header")
    parser.add_argument("--mapper", type=int, default=0)
    parser.add_argument("--mirroring", choices=["horizontal", "vertical", "four"], default="horizontal")
    parser.add_argument("--battery", action="store_true")
    parser.add_argument("--chr", type=Path, help="CHR ROM file (default: none, CHR RAM)")
    parser.add_argument("--symbols", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if not args.source or not args.output:
        parser.error("source and --output are required")
    try:
        prg, symbols, label_bank = assemble(args.source.read_text(), args.banks, args.bank_size, args.unofficial)
    except AsmError as error:
        print(f"{args.source}: {error}", file=sys.stderr)
        return 1
    if args.raw:
        args.output.write_bytes(prg)
    else:
        write_ines(args.output, prg, args.chr.read_bytes() if args.chr else b"", args.mapper, args.mirroring,
                   args.battery)
    if args.symbols:
        args.symbols.write_text("".join(
            f"{label_bank.get(k, 0):02X}:{v:04X} {k}\n" for k, v in sorted(symbols.items(), key=lambda kv: kv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
