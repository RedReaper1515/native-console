#!/usr/bin/env python3
"""Score a lift export against a disassembly of the same ROM.

`gb_lift.py score` uses this. A community disassembly gives ground truth the
recompiler never had: an RGBDS `.sym` file says where every label is, and the
`.asm` sources say whether a label starts code or data (by what follows it).
With both, the export's function map can be marked: how many real functions
it found, how many of its own functions are real, and how much of what it
decoded as code is data.

Nothing from the disassembly is copied anywhere; it is only read.
"""

from __future__ import annotations

import re
from pathlib import Path

MNEMONICS = frozenset("""
adc add and bit call ccf cp cpl daa dec di ei halt inc jp jr ld ldh nop or pop push res ret reti
rl rla rlc rlca rr rra rrc rrca rst sbc scf set sla sra srl stop sub swap xor
""".split())

# Macros that expand to instructions in the pret disassemblies. A macro this
# does not know counts as "unknown", which the report shows; it is never
# guessed to be code.
CODE_MACROS = frozenset("""
lb farcall callfar farjp jpfar homecall homecall_sf predef predef_jump tx_pre tx_pre_jump
hlcoord bccoord decoord hlbgcoord debgcoord bcbgcoord hlowcoord deowcoord bcowcoord
ldcoord_a lda_coord ld_hli_a_string jumptable
""".split())
EVENT_MACRO = re.compile(r"^(check|set|reset|toggle)\w*event\w*$", re.IGNORECASE)

DATA_DIRECTIVES = frozenset("""
db dw dl ds dn dbw dwb dba dab dbbw dbww dbwww incbin text text_far text_start line para cont
done prompt next page dex bcd2 bcd3 table_width assert_table_length list_start li
""".split())

LABEL = re.compile(r"^(\.?[A-Za-z_][A-Za-z0-9_#@.]*):{1,2}")
LOCAL_LABEL = re.compile(r"^(\.[A-Za-z_][A-Za-z0-9_#@]*)\b:?")


def load_sym(path: Path) -> dict[tuple[int, int], list[str]]:
    """(bank, address) -> names, ROM labels only."""
    labels: dict[tuple[int, int], list[str]] = {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.split(";", 1)[0].strip()
        match = re.fullmatch(r"([0-9A-Fa-f]{2,4}):([0-9A-Fa-f]{4})\s+(\S+)", line)
        if not match:
            continue
        bank, address = int(match.group(1), 16), int(match.group(2), 16)
        if address < 0x8000:
            labels.setdefault((bank, address), []).append(match.group(3))
    return labels


def classify_token(token: str) -> str:
    token = token.lower()
    if token in MNEMONICS or token in CODE_MACROS or EVENT_MACRO.match(token):
        return "code"
    if token in DATA_DIRECTIVES:
        return "data"
    return "unknown"


def classify_source_labels(source: Path) -> dict[str, str]:
    """Label name -> "code", "data" or "unknown", from what follows it."""
    kinds: dict[str, str] = {}
    for path in sorted(Path(source).rglob("*.asm")):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        pending: list[str] = []
        scope = ""
        for raw in lines:
            line = raw.split(";", 1)[0].rstrip()
            if not line.strip():
                continue
            stripped = line.strip()
            match = LABEL.match(stripped) or LOCAL_LABEL.match(stripped)
            if match and (stripped.startswith(".") or ":" in stripped.split()[0]):
                name = match.group(1)
                if name.startswith("."):
                    name = scope + name
                else:
                    scope = name
                pending.append(name)
                rest = stripped[match.end():].lstrip(":").strip()
                if not rest:
                    continue
                stripped = rest
            token = stripped.split()[0] if stripped.split() else ""
            if not token or token.lower() in ("section", "include", "endc", "if", "else", "elif", "endm",
                                              "macro", "def", "redef", "assert", "endr", "rept", "for",
                                              "export", "purge", "pushc", "popc", "charmap", "newcharmap",
                                              "setcharmap", "opt", "pusho", "popo", "println", "warn",
                                              "fail", "static_assert", "endl", "load", "union", "nextu",
                                              "endu", "shift"):
                continue
            if pending:
                kind = classify_token(token)
                for name in pending:
                    kinds.setdefault(name, kind)
                pending = []
    return kinds


def score(export: dict, sym_path: Path, source: Path | None) -> dict:
    labels = load_sym(sym_path)
    kinds = classify_source_labels(source) if source else {}

    def label_kind(names: list[str]) -> str:
        found = {kinds.get(name, "unknown") for name in names}
        for kind in ("code", "data"):
            if kind in found:
                return kind
        return "unknown"

    is_local = {where: all("." in name for name in names) for where, names in labels.items()}
    kind_at = {where: label_kind(names) for where, names in labels.items()}

    entries = {(f["bank"], f["address"]): f for f in export["functions"]}
    instruction_at: set[tuple[int, int]] = set()
    decoded_bytes: set[tuple[int, int]] = set()
    for function in export["functions"]:
        for block in function["blocks"]:
            for instruction in block["instructions"]:
                where = (function["bank"], instruction["address"])
                instruction_at.add(where)
                for offset in range(len(instruction["bytes"]) // 2):
                    decoded_bytes.add((function["bank"], instruction["address"] + offset))

    result: dict = {"labels": len(labels), "source_classified": bool(source)}

    # Our functions against the labels.
    by_kind: dict[str, dict[str, int]] = {}
    for where, function in entries.items():
        row = by_kind.setdefault(function["kind"], {"total": 0, "function_label": 0, "local_label": 0,
                                                     "data_label": 0, "no_label": 0})
        row["total"] += 1
        if where not in labels:
            row["no_label"] += 1
        elif kind_at[where] == "data":
            row["data_label"] += 1
        elif is_local[where]:
            row["local_label"] += 1
        else:
            row["function_label"] += 1
    result["our_functions"] = by_kind

    # The disassembly's functions against ours.
    if source:
        functions = [where for where in labels if kind_at[where] == "code" and not is_local[where]]
        found = sum(1 for where in functions if where in entries)
        decoded = sum(1 for where in functions if where in instruction_at and where not in entries)
        result["their_functions"] = {
            "total": len(functions),
            "is_our_function": found,
            "decoded_inside_another": decoded,
            "not_decoded": len(functions) - found - decoded,
        }
        result["label_kinds"] = {kind: sum(1 for where in labels if kind_at[where] == kind)
                                 for kind in ("code", "data", "unknown")}

        # Bytes: each label's kind holds until the next label in the bank.
        code_bytes = data_bytes = unknown_bytes = 0
        code_decoded = data_decoded = unknown_decoded = 0
        by_bank: dict[int, list[int]] = {}
        for bank, address in labels:
            by_bank.setdefault(bank, []).append(address)
        for bank, addresses in by_bank.items():
            addresses.sort()
            limit = 0x4000 if bank == 0 else 0x8000
            for index, address in enumerate(addresses):
                end = addresses[index + 1] if index + 1 < len(addresses) else limit
                kind = kind_at[(bank, address)]
                size = end - address
                hit = sum(1 for a in range(address, end) if (bank, a) in decoded_bytes)
                if kind == "code":
                    code_bytes += size
                    code_decoded += hit
                elif kind == "data":
                    data_bytes += size
                    data_decoded += hit
                else:
                    unknown_bytes += size
                    unknown_decoded += hit
        result["bytes"] = {
            "code": code_bytes, "code_decoded": code_decoded,
            "data": data_bytes, "data_decoded_as_code": data_decoded,
            "unknown": unknown_bytes, "unknown_decoded": unknown_decoded,
        }
    return result


def format_score(result: dict) -> str:
    lines = [f"{result['labels']} ROM labels in the symbol file"]
    lines.append("")
    lines.append("our functions, by how they were found, and what the disassembly has at their address:")
    lines.append(f"  {'kind':<14}{'total':>7}{'function':>10}{'local':>8}{'data':>7}{'nothing':>9}")
    for kind, row in sorted(result["our_functions"].items(), key=lambda item: -item[1]["total"]):
        lines.append(f"  {kind:<14}{row['total']:>7}{row['function_label']:>10}{row['local_label']:>8}"
                     f"{row['data_label']:>7}{row['no_label']:>9}")
    if "their_functions" in result:
        theirs = result["their_functions"]
        total = max(theirs["total"], 1)
        lines.append("")
        lines.append(f"the disassembly's {theirs['total']} functions (global labels followed by code):")
        lines.append(f"  {theirs['is_our_function']:>6} ({theirs['is_our_function'] * 100 / total:.1f}%) are functions in the export")
        lines.append(f"  {theirs['decoded_inside_another']:>6} ({theirs['decoded_inside_another'] * 100 / total:.1f}%) are decoded, inside another function")
        lines.append(f"  {theirs['not_decoded']:>6} ({theirs['not_decoded'] * 100 / total:.1f}%) were not decoded at all")
        b = result["bytes"]
        lines.append("")
        lines.append("bytes, by what the label before them starts:")
        lines.append(f"  code    {b['code']:>8}, of which the export decoded {b['code_decoded']} "
                     f"({b['code_decoded'] * 100 / max(b['code'], 1):.1f}%)")
        lines.append(f"  data    {b['data']:>8}, of which the export decoded {b['data_decoded_as_code']} as code "
                     f"({b['data_decoded_as_code'] * 100 / max(b['data'], 1):.1f}%)")
        lines.append(f"  unknown {b['unknown']:>8}, of which the export decoded {b['unknown_decoded']}")
        kinds = result["label_kinds"]
        lines.append(f"labels by kind: {kinds['code']} code, {kinds['data']} data, {kinds['unknown']} not recognised")
    return "\n".join(lines) + "\n"
