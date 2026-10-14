#!/usr/bin/env python3
"""Generate the Gekko decode classifier from the ISA table.

Reads recompilers/recompiler-gc/data/isa.yaml (encounter/powerpc-rs, MIT OR Apache-2.0)
and writes runtimes/runtime-gc/include/gc_cpu_decode.h: the opcode enum, the field
accessors, a table-driven decoder and the per-opcode metadata the analyzer
and the disassembler read.

The header is checked in. Regenerate it and commit the result whenever the
table changes; tests/gc checks that the checked-in header matches what this
script produces, so CI needs no YAML parser.

    python tools/gc/gen_decode.py            # rewrite the header
    python tools/gc/gen_decode.py --check    # fail if it is out of date

Only the base PowerPC opcodes and the PairedSingles extension are taken.
Gekko is a 32-bit 750CXe: it has no AltiVec, no VMX128 and no 64-bit forms,
so those extensions are dropped here rather than decoded and rejected later.

This file carries a small YAML reader instead of depending on PyYAML: the
table is regular, and the reader fails closed on anything it does not
recognise rather than guessing.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
ISA_PATH = REPO_ROOT / "recompilers" / "recompiler-gc" / "data" / "isa.yaml"
HEADER_PATH = REPO_ROOT / "runtimes" / "runtime-gc" / "include" / "gc_cpu_decode.h"

# Extensions we take. Everything else in the table is for CPUs that are not
# Gekko.
EXTENSIONS = ["PairedSingles"]

PRIMARY_MASK = 0xFC000000
# Selector bits that a per-primary jump table can index directly. Every
# PowerPC extended opcode field lives inside bits 21..31 (MSB numbering).
TABLE_SELECTOR = 0x000007FF
# Below this many opcodes a primary group is decoded by an ordered mask test.
TABLE_MIN_MEMBERS = 6


# --------------------------------------------------------------------------
# A YAML reader for exactly the shape of isa.yaml.


class YamlError(RuntimeError):
    pass


def strip_comment(line: str) -> str:
    out, quote = [], None
    for i, ch in enumerate(line):
        if quote:
            out.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
            out.append(ch)
            continue
        if ch == "#" and (i == 0 or line[i - 1] in " \t"):
            break
        out.append(ch)
    return "".join(out).rstrip()


def parse_scalar(text: str):
    text = text.strip()
    if not text:
        return None
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        return text[1:-1]
    if text.startswith("[") and text.endswith("]"):
        inner = text[1:-1].strip()
        if not inner:
            return []
        return [parse_scalar(part) for part in split_flow(inner)]
    if text in ("true", "false"):
        return text == "true"
    if re.fullmatch(r"-?0x[0-9a-fA-F]+", text):
        return int(text, 16)
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    return text


def split_flow(inner: str) -> list[str]:
    parts, depth, quote, current = [], 0, None, []
    for ch in inner:
        if quote:
            current.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
            current.append(ch)
            continue
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(ch)
    parts.append("".join(current))
    return [part.strip() for part in parts if part.strip()]


class Reader:
    """Indentation-driven reader for block maps and block sequences."""

    def __init__(self, text: str):
        self.lines: list[tuple[int, str, int]] = []
        for number, raw in enumerate(text.splitlines(), 1):
            line = strip_comment(raw)
            if not line.strip():
                continue
            indent = len(line) - len(line.lstrip(" "))
            if "\t" in line:
                raise YamlError(f"line {number}: tabs are not supported")
            self.lines.append((indent, line.strip(), number))
        self.pos = 0

    def peek(self):
        return self.lines[self.pos] if self.pos < len(self.lines) else None

    def parse(self):
        value = self.parse_block(0)
        if self.pos != len(self.lines):
            indent, line, number = self.lines[self.pos]
            raise YamlError(f"line {number}: unexpected {line!r}")
        return value

    def parse_block(self, indent: int):
        item = self.peek()
        if item is None:
            return None
        if item[1].startswith("- "):
            return self.parse_sequence(indent)
        return self.parse_map(indent)

    def parse_sequence(self, indent: int) -> list:
        items = []
        while True:
            item = self.peek()
            if item is None or item[0] < indent or not item[1].startswith("- "):
                return items
            if item[0] != indent:
                raise YamlError(f"line {item[2]}: bad sequence indent")
            self.pos += 1
            head = item[1][2:]
            entry_indent = indent + 2
            if ":" in head and not head.startswith("["):
                entry = {}
                self.consume_pair(entry, head, item[2])
                entry.update(self.parse_map(entry_indent, allow_missing=True) or {})
                items.append(entry)
            else:
                items.append(parse_scalar(head))

    def parse_map(self, indent: int, allow_missing: bool = False) -> dict:
        result: dict = {}
        while True:
            item = self.peek()
            if item is None or item[0] < indent or item[1].startswith("- "):
                if not result and not allow_missing:
                    raise YamlError(f"expected a mapping at indent {indent}")
                return result
            if item[0] != indent:
                raise YamlError(f"line {item[2]}: bad mapping indent")
            self.pos += 1
            self.consume_pair(result, item[1], item[2])

    def consume_pair(self, target: dict, text: str, number: int) -> None:
        if ":" not in text:
            raise YamlError(f"line {number}: expected 'key: value', got {text!r}")
        key, _, rest = text.partition(":")
        key = key.strip()
        rest = rest.strip()
        if rest:
            target[key] = parse_scalar(rest)
            return
        nested = self.peek()
        if nested is None:
            target[key] = None
            return
        target[key] = self.parse_block(nested[0])


# --------------------------------------------------------------------------
# The ISA table.


def load_isa(path: Path) -> dict:
    table = Reader(path.read_text(encoding="utf-8")).parse()
    for key in ("fields", "modifiers", "opcodes", "extensions"):
        if key not in table:
            raise YamlError(f"{path}: no '{key}' section")
    fields = {entry["name"]: entry for entry in table["fields"]}
    modifiers = {entry["name"]: entry for entry in table["modifiers"]}
    opcodes = [dict(entry, extension=None) for entry in table["opcodes"]]
    for name in EXTENSIONS:
        extension = table["extensions"].get(name)
        if extension is None:
            raise YamlError(f"{path}: no extension {name!r}")
        for entry in extension.get("fields") or []:
            fields.setdefault(entry["name"], entry)
        for entry in extension.get("modifiers") or []:
            modifiers.setdefault(entry["name"], entry)
        for entry in extension.get("opcodes") or []:
            opcodes.append(dict(entry, extension=name))
    return {"fields": fields, "modifiers": modifiers, "opcodes": opcodes}


def field_pieces(field: dict) -> list[tuple[int, int]]:
    """Return (shift, width) pieces, most significant first.

    Ranges are PowerPC MSB-0 and half open: 16..32 is the low 16 bits.
    """
    spec = field.get("bits")
    if spec is None:
        return []
    pieces = []
    for part in str(spec).split(","):
        start, _, end = part.partition("..")
        start, end = int(start), int(end)
        if not 0 <= start < end <= 32:
            raise YamlError(f"field {field['name']}: bad range {part!r}")
        pieces.append((32 - end, end - start))
    return pieces


def field_expr(field: dict) -> str:
    pieces = field_pieces(field)
    if not pieces:
        raise YamlError(f"field {field['name']}: no bits")
    terms, consumed = [], 0
    for shift, width in reversed(pieces):  # least significant piece first
        term = f"(((insn) >> {shift}) & 0x{(1 << width) - 1:x}u)"
        terms.append(term if consumed == 0 else f"({term} << {consumed})")
        consumed += width
    expr = " | ".join(reversed(terms))
    if len(terms) > 1:
        expr = f"({expr})"
    if field.get("signed"):
        total = sum(width for _, width in pieces)
        expr = f"gc_sext32({expr}, {total})"
    shift_left = field.get("shift_left")
    if shift_left:
        expr = f"((int32_t)({expr}) << {shift_left})" if field.get("signed") else f"(({expr}) << {shift_left})"
    return expr


def enum_name(name: str) -> str:
    ident = re.sub(r"[^0-9A-Za-z]", "_", name).upper()
    return f"GC_OP_{ident}"


def primary(pattern: int) -> int:
    return (pattern & PRIMARY_MASK) >> 26


def check_opcodes(opcodes: list[dict]) -> None:
    seen = {}
    for entry in opcodes:
        name = entry["name"]
        if enum_name(name) in seen:
            raise YamlError(f"opcode {name!r} collides with {seen[enum_name(name)]!r}")
        seen[enum_name(name)] = name
        mask, pattern = entry["bitmask"], entry["pattern"]
        if mask & PRIMARY_MASK != PRIMARY_MASK:
            raise YamlError(f"opcode {name!r}: primary opcode is not fixed")
        if pattern & ~mask & 0xFFFFFFFF:
            raise YamlError(f"opcode {name!r}: pattern has bits outside its mask")
    # Two opcodes must never accept the same encoding.
    by_primary: dict[int, list[dict]] = {}
    for entry in opcodes:
        by_primary.setdefault(primary(entry["pattern"]), []).append(entry)
    for group in by_primary.values():
        for i, a in enumerate(group):
            for b in group[i + 1 :]:
                common = a["bitmask"] & b["bitmask"]
                if a["pattern"] & common == b["pattern"] & common:
                    raise YamlError(f"opcodes {a['name']!r} and {b['name']!r} overlap")


def decoder_groups(opcodes: list[dict]) -> list[tuple[int, list[dict], bool]]:
    """(primary, members, needs_table) groups for the decoder.

    Dispatch is on the primary opcode and the extended-opcode field only,
    which is what the hardware decodes on. The bits some encodings fix
    outside that field are reserved fields that must be written as zero -
    rA for `fabs`, rB for `cntlzw`, and so on - and an implementation is
    free to ignore them. The full mask stays in gc_op_info so that a
    disassembler or the analyzer can still tell a canonical encoding from a
    sloppy one (gc_decode_is_canonical).
    """
    by_primary: dict[int, list[dict]] = {}
    for entry in opcodes:
        by_primary.setdefault(primary(entry["pattern"]), []).append(entry)
    groups = []
    for key in sorted(by_primary):
        members = by_primary[key]
        selector = 0
        for entry in members:
            selector |= entry["bitmask"] & TABLE_SELECTOR
        if selector == 0:
            if len(members) != 1:
                raise YamlError(
                    f"primary {key:#x}: {len(members)} members share one encoding"
                )
            groups.append((key, members, False))
            continue
        # Two members must not agree on the extended-opcode field, or the
        # table below could not tell them apart.
        for i, a in enumerate(members):
            for b in members[i + 1 :]:
                common = a["bitmask"] & b["bitmask"] & TABLE_SELECTOR
                if a["pattern"] & common == b["pattern"] & common:
                    raise YamlError(
                        f"opcodes {a['name']!r} and {b['name']!r} share an extended opcode"
                    )
        # A jump table costs 4 KB; below a handful of members an ordered mask
        # test is smaller and just as fast.
        groups.append((key, members, len(members) >= TABLE_MIN_MEMBERS))
    return groups


def decode_table(key: int, members: list[dict]) -> list:
    """table[extended opcode bits] -> opcode entry, or 0 for illegal."""
    table = [0] * (TABLE_SELECTOR + 1)
    for index in range(TABLE_SELECTOR + 1):
        for entry in members:
            mask = entry["bitmask"] & TABLE_SELECTOR
            if index & mask == entry["pattern"] & mask:
                table[index] = entry
                break
    return table


# --------------------------------------------------------------------------
# Header generation.


def generate(isa: dict) -> str:
    opcodes = isa["opcodes"]
    check_opcodes(opcodes)
    groups = decoder_groups(opcodes)

    out: list[str] = []
    w = out.append
    w("/* Generated by tools/gc/gen_decode.py from recompilers/recompiler-gc/data/isa.yaml.")
    w(" * Do not edit. Run the generator and commit the result instead.")
    w(" *")
    w(" * The ISA table comes from encounter/powerpc-rs (MIT OR Apache-2.0);")
    w(" * see THIRD_PARTY.md. Only the base PowerPC opcodes and the paired-single")
    w(" * extension are taken, because that is what a Gekko implements.")
    w(" *")
    w(" * One decode classifier is shared by the interpreter, the analyzer and the")
    w(" * emitter, so that the three cannot drift apart (ADDING-A-CONSOLE.md, P1).")
    w(" */")
    w("#ifndef GC_CPU_DECODE_H")
    w("#define GC_CPU_DECODE_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("#ifdef __cplusplus")
    w('extern "C" {')
    w("#endif")
    w("")
    w("/* Sign-extend the low `bits` of `value`. */")
    w("static inline int32_t gc_sext32(uint32_t value, int bits) {")
    w("  const uint32_t sign = 1u << (bits - 1);")
    w("  return (int32_t)((value ^ sign) - sign);")
    w("}")
    w("")

    w("typedef enum GcOp {")
    w("  GC_OP_ILLEGAL = 0,")
    for entry in opcodes:
        w(f"  {enum_name(entry['name'])},")
    w("  GC_OP_COUNT")
    w("} GcOp;")
    w("")

    w("/* Field accessors. Ranges are PowerPC MSB-0 numbering, as in the manual. */")
    for name, field in isa["fields"].items():
        if not field.get("bits"):
            continue
        ident = re.sub(r"[^0-9A-Za-z_]", "_", name)
        w(f"#define GC_F_{ident}(insn) ({field_expr(field)})")
    w("")

    w("/* Modifier bits. */")
    for name, modifier in isa["modifiers"].items():
        bit = modifier.get("bit")
        if bit is None:
            continue
        ident = re.sub(r"[^0-9A-Za-z_]", "_", name)
        w(f"#define GC_M_{ident}(insn) (((insn) >> {31 - bit}) & 1u)")
    w("")

    w("/* Opcode metadata, indexed by GcOp. */")
    w("typedef struct GcOpInfo {")
    w("  const char *name;   /* mnemonic without modifier suffixes */")
    w("  uint32_t bitmask;   /* bits the encoding fixes */")
    w("  uint32_t pattern;   /* value of those bits */")
    w("  uint8_t paired;     /* 1 when the opcode is part of the paired-single set */")
    w("} GcOpInfo;")
    w("")
    w("extern const GcOpInfo gc_op_info[GC_OP_COUNT];")
    w("")

    # The decoder.
    w("/* Decode dispatches on the primary opcode and the extended-opcode field,")
    w(" * as the hardware does. Reserved fields an encoding is supposed to write")
    w(" * as zero do not take part; gc_decode_is_canonical() checks those. */")
    for key, members, needs_table in groups:
        if not needs_table:
            continue
        table = decode_table(key, members)
        w(f"static const uint16_t gc_decode_table_{key:02x}[{TABLE_SELECTOR + 1}] = {{")
        row = []
        for index, entry in enumerate(table):
            row.append("0" if entry == 0 else enum_name(entry["name"]))
            if len(row) == 6 or index == TABLE_SELECTOR:
                w("  " + ", ".join(row) + ",")
                row = []
        w("};")
        w("")

    w("static inline GcOp gc_decode(uint32_t insn) {")
    w("  switch (insn >> 26) {")
    for key, members, needs_table in groups:
        w(f"  case 0x{key:02x}:")
        if not needs_table:
            ordered = sorted(members, key=lambda e: -bin(e["bitmask"] & TABLE_SELECTOR).count("1"))
            for entry in ordered:
                mask = entry["bitmask"] & TABLE_SELECTOR
                if mask == 0:
                    w(f"    return {enum_name(entry['name'])};")
                    break
                pattern = entry["pattern"] & TABLE_SELECTOR
                w(f"    if ((insn & 0x{mask:03x}u) == 0x{pattern:03x}u) return {enum_name(entry['name'])};")
            else:
                w("    return GC_OP_ILLEGAL;")
        else:
            w(f"    return (GcOp)gc_decode_table_{key:02x}[insn & 0x{TABLE_SELECTOR:03x}u];")
    w("  default:")
    w("    return GC_OP_ILLEGAL;")
    w("  }")
    w("}")
    w("")
    w("/* True when every bit the encoding fixes really holds its fixed value.")
    w(" * A canonical encoding is what an assembler emits; a non-canonical one")
    w(" * still executes, but it is a good signal that the analyzer has wandered")
    w(" * into data. */")
    w("static inline int gc_decode_is_canonical(GcOp op, uint32_t insn) {")
    w("  const GcOpInfo *info = &gc_op_info[op];")
    w("  return op != GC_OP_ILLEGAL && (insn & info->bitmask) == info->pattern;")
    w("}")
    w("")

    w("#ifdef GC_CPU_DECODE_IMPLEMENTATION")
    w("const GcOpInfo gc_op_info[GC_OP_COUNT] = {")
    w('  { "illegal", 0u, 0u, 0 },')
    for entry in opcodes:
        paired = 1 if entry.get("extension") == "PairedSingles" else 0
        w(f'  {{ "{entry["name"]}", 0x{entry["bitmask"]:08x}u, 0x{entry["pattern"]:08x}u, {paired} }},')
    w("};")
    w("#endif /* GC_CPU_DECODE_IMPLEMENTATION */")
    w("")
    w("#ifdef __cplusplus")
    w("}")
    w("#endif")
    w("")
    w("#endif /* GC_CPU_DECODE_H */")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--isa", type=Path, default=ISA_PATH)
    parser.add_argument("--out", type=Path, default=HEADER_PATH)
    parser.add_argument("--check", action="store_true", help="fail if the header is out of date")
    args = parser.parse_args()

    try:
        isa = load_isa(args.isa)
        header = generate(isa)
    except YamlError as error:
        print(f"gen_decode: {error}", file=sys.stderr)
        return 1

    if args.check:
        if not args.out.exists():
            print(f"gen_decode: {args.out} does not exist", file=sys.stderr)
            return 1
        if args.out.read_text(encoding="utf-8").replace("\r\n", "\n") != header:
            print(f"gen_decode: {args.out} is out of date; run tools/gc/gen_decode.py", file=sys.stderr)
            return 1
        print(f"gen_decode: {args.out.name} is up to date")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(header, encoding="utf-8", newline="\n")
    count = sum(1 for _ in isa["opcodes"])
    print(f"gen_decode: {count} opcodes -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
