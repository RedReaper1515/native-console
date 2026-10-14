#!/usr/bin/env python3
"""Generate the vector case table the guest generator runs.

Reads recompilers/recompiler-gc/data/isa.yaml - the same table the decoder is generated
from - and writes tests/gc/vectors/gen/gc_cases.h: one entry per instruction
encoding to be measured on an oracle.

    python tools/gc/gen_vector_cases.py
    python tools/gc/gen_vector_cases.py --check

Operands are assigned from the small register set the harness can see
(r3-r10, f1-f8; see tests/gc/vectors/gen/gc_vector_state.h), with r10 always
pointing at the scratch buffer so a memory form has somewhere legal to
point. Immediate and field operands get a few values each, and the Rc and OE
forms are separate cases, because those are exactly the bits an emitter gets
wrong.

What is deliberately not measured here, and why:

  - branches and `sc`, `rfi`, `tw`: they change the flow of control, which
    the harness cannot capture around a single instruction.
  - `mtspr`, `mfspr`, `mtmsr`, the segment registers, `tlbie`: they change
    machine state the harness would have to restore, and several would stop
    the oracle dead.
  - `lwarx` and `stwcx.`: the reservation is hidden state carried between
    cases, so results would depend on case order.
  - `lmw`, `lswi`, `lswx`, `stmw`: they reach registers the harness neither
    sets nor captures - the loads write r11 upward, and `stmw` stores
    everything from rS to r31, so its result depends on whatever the
    generator happened to be holding. `stswi` and `stswx` are measured,
    because a short byte count keeps them inside the visible registers and
    their effect lands in the scratch buffer.
  - `dcbz_l`: it needs the locked cache enabled, which is a machine
    configuration, not an instruction property.

Each of those is covered by tests/gc/gc_cpu_test.c and, from P2, by
hwtests/cputest.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gen_decode import EXTENSIONS, ISA_PATH, YamlError, load_isa  # noqa: E402

OUT_PATH = REPO_ROOT / "tests" / "gc" / "vectors" / "gen" / "gc_cases.h"

SKIP = {
    "b", "bc", "bclr", "bcctr", "sc", "rfi", "tw", "twi",
    "mtmsr", "mfmsr", "mtsr", "mtsrin", "mfsr", "mfsrin",
    "mtspr", "mfspr", "mftb", "tlbie", "tlbsync",
    "isync", "sync", "eieio", "icbi",
    "dcbi", "dcbf", "dcbst", "dcbt", "dcbtst", "dcbz_l",
    "eciwx", "ecowx",
    "lwarx", "stwcx.",
    "lmw", "lswi", "lswx", "stmw",
}

# Registers the harness can see. r10 holds the scratch address; r6 is given a
# small value for the indexed memory forms so that rA + rB stays inside it.
REG = {"rD": 3, "rS": 3, "rA": 4, "rB": 5}
MEM_REG = {"rD": 3, "rS": 3, "rA": 10, "rB": 6}
FREG = {"frD": 1, "frS": 1, "frA": 2, "frB": 3, "frC": 4}

# Values tried for each non-register operand.
FIELD_VALUES = {
    "simm": [0x0008, 0xFFF8],
    "uimm": [0x00FF, 0x8001],
    "offset": [0x0008, 0x0010],
    "SH": [1, 17],
    "MB": [0, 24],
    "ME": [7, 31],
    "NB": [4],
    "crm": [0x81, 0xFF],
    "crfD": [0, 5],
    "crfS": [1],
    "crbD": [2, 31],
    "crbA": [0],
    "crbB": [5],
    "L": [0],
    "mtfsf_FM": [0xFF, 0x09],
    "mtfsf_IMM": [0x5],
    "ps_offset": [0x008, 0x010],
    "ps_I": [0, 1],
    "ps_W": [0, 1],
    "ps_IX": [0, 1],
    "ps_WX": [0, 1],
    "sync_L": [0],
    "TO": [0],
}

MAX_VARIANTS = 4

FLAG_FP = 1
FLAG_MEM = 2
FLAG_STRING = 4
FLAG_QUANTISED = 8


def is_memory(name: str) -> bool:
    return name.startswith(("l", "st", "psq_", "dcbz"))


def is_float(name: str, args: list[str]) -> bool:
    return any(a in FREG for a in args) or name.startswith(("ps_", "psq_", "mtfs", "mffs", "mcrfs"))


def field_positions(isa: dict) -> dict[str, tuple[int, int]]:
    """name -> (shift, width) for the single-range fields we place."""
    positions = {}
    for name, field in isa["fields"].items():
        spec = field.get("bits")
        if not spec or "," in str(spec):
            continue
        start, _, end = str(spec).partition("..")
        start, end = int(start), int(end)
        positions[name] = (32 - end, end - start)
    return positions


def place(insn: int, positions: dict, name: str, value: int) -> int:
    shift, width = positions[name]
    mask = ((1 << width) - 1) << shift
    return (insn & ~mask) | ((value << shift) & mask)


def variants_for(entry: dict, isa: dict, positions: dict) -> list[tuple[int, int]]:
    """(instruction word, flags) for one opcode."""
    name = entry["name"]
    args = entry.get("args") or []
    modifiers = entry.get("modifiers") or []
    memory = is_memory(name)
    registers = MEM_REG if memory else REG

    flags = 0
    if is_float(name, args):
        flags |= FLAG_FP
    if memory:
        flags |= FLAG_MEM
    if name.startswith(("stsw", "lsw")):
        flags |= FLAG_STRING
    if name.startswith("psq_"):
        flags |= FLAG_QUANTISED

    base = entry["pattern"]

    # Every operand that is not a register takes a few values; the product is
    # capped so that one instruction cannot dominate the run.
    choices: list[list[tuple[str, int]]] = []
    for arg in args:
        if arg in registers:
            base = place(base, positions, arg, registers[arg])
        elif arg in FREG:
            base = place(base, positions, arg, FREG[arg])
        elif arg in FIELD_VALUES and arg in positions:
            choices.append([(arg, value) for value in FIELD_VALUES[arg]])
        elif arg in positions:
            choices.append([(arg, 0)])

    combinations: list[list[tuple[str, int]]] = [[]]
    for options in choices:
        combinations = [c + [o] for c in combinations for o in options][:MAX_VARIANTS]

    modifier_sets: list[list[str]] = [[]]
    if "Rc" in modifiers:
        modifier_sets += [["Rc"]]
    if "OE" in modifiers:
        modifier_sets += [["OE"], ["OE", "Rc"]] if "Rc" in modifiers else [["OE"]]

    out = []
    for combination in combinations:
        word = base
        for arg, value in combination:
            word = place(word, positions, arg, value)
        for mods in modifier_sets:
            variant = word
            for mod in mods:
                bit = isa["modifiers"][mod]["bit"]
                variant |= 1 << (31 - bit)
            out.append((variant, flags))
    return out


def generate(isa: dict) -> str:
    positions = field_positions(isa)
    lines: list[str] = []
    w = lines.append
    w("/* Generated by tools/gc/gen_vector_cases.py from recompilers/recompiler-gc/data/isa.yaml.")
    w(" * Do not edit. One entry per instruction encoding measured on the oracle;")
    w(" * see the generator for what is deliberately left out. */")
    w("#ifndef GC_CASES_H")
    w("#define GC_CASES_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("#define GC_CASE_FP 1u")
    w("#define GC_CASE_MEM 2u")
    w("#define GC_CASE_STRING 4u")
    w("#define GC_CASE_QUANTISED 8u")
    w("")
    w("typedef struct GcCase {")
    w("  const char *name;")
    w("  uint32_t insn;")
    w("  uint8_t flags;")
    w("} GcCase;")
    w("")
    w("static const GcCase gc_cases[] = {")
    count = 0
    for entry in isa["opcodes"]:
        name = entry["name"]
        if name in SKIP:
            continue
        for word, flags in variants_for(entry, isa, positions):
            w(f'    {{"{name}", 0x{word:08x}u, {flags}u}},')
            count += 1
    w("};")
    w("")
    w(f"#define GC_CASE_COUNT {count}")
    w("")
    w("#endif /* GC_CASES_H */")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--isa", type=Path, default=ISA_PATH)
    parser.add_argument("--out", type=Path, default=OUT_PATH)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    try:
        isa = load_isa(args.isa)
        header = generate(isa)
    except YamlError as error:
        print(f"gen_vector_cases: {error}", file=sys.stderr)
        return 1

    if args.check:
        if not args.out.exists() or args.out.read_text(encoding="utf-8").replace("\r\n", "\n") != header:
            print(f"gen_vector_cases: {args.out} is out of date", file=sys.stderr)
            return 1
        print(f"gen_vector_cases: {args.out.name} is up to date")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(header, encoding="utf-8", newline="\n")
    print(f"gen_vector_cases: {header.count(chr(10) + '    {')} cases -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
