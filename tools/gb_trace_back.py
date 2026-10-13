#!/usr/bin/env python3
"""Read a backward trace from a RAM address.

A generated Game Boy game run with `--trace-back FILE --trace-back-of ADDRESS`
writes down, for every write to the watched bytes, which instruction wrote
and what the value was made of, followed back through RAM (RUNTIME.md,
DATA_FILES.md). This prints that record for a person:

  show RECORD      each write as a tree: the instruction, the calls it was
                   made in, and under it what the value was made of
  summary RECORD...  per watched address over many records: the instructions
                   that wrote it, the ROM that fed it, the RAM on the way

A tree is drawn by writing instruction, not by event: a loop that stores to
one byte sixty times is one line that says so, with what all sixty were made
of under it. Each line begins with how the thing took part:

  made of      it was copied or reckoned into the value
  through      the value was loaded through an address made of it
  decided by   a conditional tested it earlier in the same call

`--export` (a `gbrecomp --lift-export` file) and `--sym` (RGBDS symbol
files, or a `names.sym`) give addresses their names; `--rom` lets the
instructions be written out; `--tree` says which file of a data tree a ROM
byte is in. None of them is needed.
"""

from __future__ import annotations

import argparse
from collections import deque
import heapq
import json
from pathlib import Path
import sys

SCHEMA = "gbrt.trace-back"
VERSION = 1

VALUE, ADDRESS, DECISION = 0, 1, 2
ROLES = {"value": VALUE, "address": ADDRESS, "decision": DECISION}
ROLE_NAMES = ["value", "address", "decision"]
LABELS = ["made of", "through", "decided by"]


class TraceError(Exception):
    pass


# --------------------------------------------------------------------------
# The record
# --------------------------------------------------------------------------

def load_record(path: Path) -> dict:
    try:
        record = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise TraceError(f"cannot read {path}: {error}") from error
    if not isinstance(record, dict) or record.get("schema") != SCHEMA:
        raise TraceError(f"{path} is not a trace-back record (its schema is not {SCHEMA})")
    if record.get("version") != VERSION:
        raise TraceError(f"{path} is version {record.get('version')} of the record; this tool reads {VERSION}")
    record["by_id"] = {event["id"]: event for event in record["events_kept"]}
    record["path"] = str(path)
    return record


def parse_range(text: str) -> tuple[int, int]:
    first, _, last = text.partition("-")
    try:
        low = int(first, 16)
        high = int(last, 16) if last else low
    except ValueError:
        raise TraceError(f"'{text}' is not ADDRESS or ADDRESS-ADDRESS in hexadecimal") from None
    if not 0 <= low <= high <= 0xFFFF:
        raise TraceError(f"'{text}' is not a range of addresses")
    return low, high


# --------------------------------------------------------------------------
# Names, from an export and from symbol files
# --------------------------------------------------------------------------

class Names:
    def __init__(self, export: dict | None, sym_texts: list[str]) -> None:
        import gb_lift_codegen
        import gb_lift_source

        self.hardware = dict(gb_lift_codegen.HARDWARE_NAMES)
        self.labels: dict[tuple[int, int], list[str]] = {}
        for text in sym_texts:
            for where, names in gb_lift_source.read_names(text).items():
                self.labels.setdefault(where, []).extend(names)
        self.ram: dict[int, str] = {}
        self.by_bank: dict[int, list[int]] = {}
        for (bank, address), names in self.labels.items():
            if address >= 0x8000:
                # A byte with several names goes by the first: of the first file given that names it.
                self.ram.setdefault(address, names[0])
            else:
                self.by_bank.setdefault(bank, []).append(address)
        for addresses in self.by_bank.values():
            addresses.sort()
        self.ram_addresses = sorted(self.ram)
        self.spans: dict[int, list[tuple[int, int, int, str]]] = {}     # bank -> (start, end, entry, name)
        self.asm: dict[tuple[int, int], str] = {}
        if export:
            for entry in export.get("memory", ()):
                if entry.get("name") and entry["address"] >= 0x8000:
                    self.ram.setdefault(entry["address"], entry["name"])
            self.ram_addresses = sorted(self.ram)
            for function in export.get("functions", ()):
                for block in function.get("blocks", ()):
                    self.spans.setdefault(function["bank"], []).append(
                        (block["address"], block["end"], function["address"], function["name"]))
                    for instruction in block.get("instructions", ()):
                        self.asm[(function["bank"], instruction["address"])] = instruction["asm"]
            for spans in self.spans.values():
                spans.sort()

    def label_before(self, bank: int, address: int, whole_names_only: bool) -> tuple[int, str] | None:
        """The nearest label at or before a ROM address, in its bank."""
        addresses = self.by_bank.get(bank if address >= 0x4000 else 0, ())
        low, high = 0, len(addresses)
        while low < high:
            middle = (low + high) // 2
            if addresses[middle] <= address:
                low = middle + 1
            else:
                high = middle
        for index in range(low - 1, -1, -1):
            start = addresses[index]
            if (start < 0x4000) != (address < 0x4000):
                return None
            names = list(self.labels[(bank if address >= 0x4000 else 0, start)])
            if whole_names_only:
                names = [name for name in names if "." not in name]
            if names:
                return start, names[0]
        return None

    def function(self, bank: int, address: int) -> tuple[int, str] | None:
        """The function an instruction is in: where it starts and what it is called."""
        if address >= 0x8000:
            return None
        bank = bank if address >= 0x4000 else 0
        for start, end, entry, name in self.spans.get(bank, ()):
            if start <= address < end:
                given = self.labels.get((bank, entry))
                return entry, (given[0] if given else name)
        return self.label_before(bank, address, whole_names_only=True)

    def code(self, bank: int, address: int) -> str:
        """`BB:AAAA function+offset` for an instruction."""
        if address >= 0x8000:
            return f"{address:04X} (code in RAM)"
        where = f"{bank:02X}:{address:04X}"
        found = self.function(bank, address)
        if not found:
            return where
        start, name = found
        return f"{where} {name}" + (f"+{address - start}" if address != start else "")

    def function_name(self, bank: int, address: int) -> str:
        if address >= 0x8000:
            return f"code in RAM at {address:04X}"
        found = self.function(bank, address)
        return found[1] if found else f"{bank:02X}:{address:04X}"

    def data(self, bank: int, address: int) -> str | None:
        """`Label+offset` for a byte of data in the ROM."""
        found = self.label_before(bank, address, whole_names_only=False)
        if not found:
            return None
        start, name = found
        return name + (f"+{address - start}" if address != start else "")

    def variable(self, address: int) -> str | None:
        """A name for a byte of RAM or a hardware register: its own, or the one before it and how far on."""
        if address in self.hardware:
            return self.hardware[address]
        if address in self.ram:
            return self.ram[address]
        low, high = 0, len(self.ram_addresses)
        while low < high:
            middle = (low + high) // 2
            if self.ram_addresses[middle] <= address:
                low = middle + 1
            else:
                high = middle
        if low and address - self.ram_addresses[low - 1] < 0x40 and address < 0xFF00:
            before = self.ram_addresses[low - 1]
            return f"{self.ram[before]}+{address - before}"
        return None

    def place(self, address: int) -> str:
        """`name (AAAA)` or `AAAA` for a byte of RAM."""
        name = self.variable(address)
        return f"{name} ({address:04X})" if name else f"{address:04X}"


# --------------------------------------------------------------------------
# Instructions written out
# --------------------------------------------------------------------------

R8 = ["b", "c", "d", "e", "h", "l", "[hl]", "a"]
R16 = ["bc", "de", "hl", "sp"]
R16_STACK = ["bc", "de", "hl", "af"]
CONDITIONS = ["nz", "z", "nc", "c"]
ARITHMETIC = ["add a, ", "adc a, ", "sub ", "sbc a, ", "and ", "xor ", "or ", "cp "]
SHIFTS = ["rlc", "rrc", "rl", "rr", "sla", "sra", "swap", "srl"]
PLAIN = {0x00: "nop", 0x07: "rlca", 0x0F: "rrca", 0x10: "stop", 0x17: "rla", 0x1F: "rra", 0x27: "daa", 0x2F: "cpl",
         0x37: "scf", 0x3F: "ccf", 0x76: "halt", 0xC9: "ret", 0xD9: "reti", 0xE9: "jp hl", 0xF3: "di", 0xFB: "ei",
         0xF9: "ld sp, hl", 0x02: "ld [bc], a", 0x12: "ld [de], a", 0x22: "ld [hli], a", 0x32: "ld [hld], a",
         0x0A: "ld a, [bc]", 0x1A: "ld a, [de]", 0x2A: "ld a, [hli]", 0x3A: "ld a, [hld]", 0xE2: "ld [$FF00+c], a",
         0xF2: "ld a, [$FF00+c]"}


def disassemble(code: bytes) -> str:
    """One SM83 instruction, from its first bytes (three are enough)."""
    code = code + bytes(3)
    op = code[0]
    byte = f"${code[1]:02X}"
    word = f"${code[1] | (code[2] << 8):04X}"
    high = f"$FF{code[1]:02X}"
    relative = code[1] - 256 if code[1] >= 128 else code[1]
    low3, mid3 = op & 7, (op >> 3) & 7
    if op in PLAIN:
        return PLAIN[op]
    if op == 0xCB:
        second = code[1]
        operand = R8[second & 7]
        if second < 0x40:
            return f"{SHIFTS[(second >> 3) & 7]} {operand}"
        return f"{['bit', 'res', 'set'][(second >> 6) - 1]} {(second >> 3) & 7}, {operand}"
    if 0x40 <= op < 0x80:
        return f"ld {R8[mid3]}, {R8[low3]}"
    if 0x80 <= op < 0xC0:
        return f"{ARITHMETIC[mid3]}{R8[low3]}"
    if op < 0x40:
        pair = R16[(op >> 4) & 3]
        if low3 == 0:
            return f"ld [{word}], sp" if op == 0x08 else f"jr {relative:+d}" if op == 0x18 else \
                f"jr {CONDITIONS[mid3 - 4]}, {relative:+d}"
        if low3 == 1:
            return f"add hl, {pair}" if op & 8 else f"ld {pair}, {word}"
        if low3 == 3:
            return f"{'dec' if op & 8 else 'inc'} {pair}"
        if low3 == 4:
            return f"inc {R8[mid3]}"
        if low3 == 5:
            return f"dec {R8[mid3]}"
        return f"ld {R8[mid3]}, {byte}"
    if low3 == 0 and op < 0xE0:
        return f"ret {CONDITIONS[mid3]}"
    if low3 == 2 and op < 0xE0:
        return f"jp {CONDITIONS[mid3]}, {word}"
    if low3 == 4 and op < 0xE0:
        return f"call {CONDITIONS[mid3]}, {word}"
    if low3 == 7:
        return f"rst ${op & 0x38:02X}"
    if low3 == 6:
        return f"{ARITHMETIC[mid3]}{byte}"
    if low3 == 1:
        return f"pop {R16_STACK[(op >> 4) & 3]}"
    if low3 == 5 and not op & 8:
        return f"push {R16_STACK[(op >> 4) & 3]}"
    return {0xC3: f"jp {word}", 0xCD: f"call {word}", 0xE0: f"ld [{high}], a", 0xF0: f"ld a, [{high}]",
            0xEA: f"ld [{word}], a", 0xFA: f"ld a, [{word}]", 0xE8: f"add sp, {relative:+d}",
            0xF8: f"ld hl, sp{relative:+d}"}.get(op, f"db ${op:02X}")


class Context:
    """What was given beside the record: names, the ROM, a data tree."""

    def __init__(self, args) -> None:
        import gb_lift

        export = None
        if getattr(args, "export", None):
            try:
                export = gb_lift.load_export(args.export)
            except gb_lift.LiftError as error:
                raise TraceError(str(error)) from error
        texts = []
        for path in getattr(args, "sym", None) or ():
            try:
                texts.append(Path(path).read_text(encoding="utf-8", errors="replace"))
            except OSError as error:
                raise TraceError(f"cannot read the symbol file {path}: {error}") from error
        self.names = Names(export, texts)
        self.rom = None
        if getattr(args, "rom", None):
            try:
                self.rom = Path(args.rom).read_bytes()
            except OSError as error:
                raise TraceError(f"cannot read the ROM {args.rom}: {error}") from error
        self.files: list[tuple[int, int, str]] = []
        if getattr(args, "tree", None):
            try:
                manifest = json.loads((Path(args.tree) / "manifest.json").read_text(encoding="utf-8"))
                self.files = sorted((region["offset"], region["size"], region["file"])
                                    for region in manifest["regions"])
            except (OSError, ValueError, KeyError, TypeError) as error:
                raise TraceError(f"cannot read the data tree {args.tree}: {error}") from error

    def check(self, record: dict) -> None:
        if self.rom is not None and len(self.rom) != record["rom"]["size"]:
            raise TraceError(f"the ROM given is {len(self.rom)} bytes, and {record['path']} was recorded from one "
                             f"of {record['rom']['size']}")

    def instruction(self, bank: int, address: int) -> str:
        """`BB:AAAA function+offset: the instruction`."""
        where = self.names.code(bank, address)
        text = None
        if address < 0x8000:
            if self.rom is not None:
                offset = (bank if address >= 0x4000 else 0) * 0x4000 + (address & 0x3FFF)
                text = disassemble(self.rom[offset:offset + 3])
            else:
                text = self.names.asm.get((bank if address >= 0x4000 else 0, address))
        return f"{where}: {text}" if text else where

    def file_of(self, offset: int) -> str | None:
        for start, size, name in self.files:
            if start <= offset < start + size:
                return f"{name}+{offset - start}" if offset != start else name
        return None


def bank_address(offset: int) -> tuple[int, int]:
    bank = offset // 0x4000
    return bank, (0x4000 if bank else 0) + (offset & 0x3FFF)


# --------------------------------------------------------------------------
# From events to writing instructions
# --------------------------------------------------------------------------

FAR = (1 << 30, 1)


def further(way: tuple[int, int], role: int) -> tuple[int, int]:
    """How a thing took part, reached by `role` from something that took part by `way`: the number of
    decisions on the way, and whether an address was loaded through on it."""
    if role == DECISION:
        return way[0] + 1, way[1]
    return way[0], max(way[1], 1 if role == ADDRESS else 0)


def role_of(way: tuple[int, int]) -> int:
    return DECISION if way[0] else ADDRESS if way[1] else VALUE


def strengths(record: dict, roots: list[int], decisions: bool | int) -> dict[int, tuple[int, int]]:
    """Every event the roots lead back to, with how it took part at best: by the fewest decisions, and
    among those as a value all the way before through an address. `decisions` is whether decisions are
    followed at all, or how many of them one after another."""
    events = record["by_id"]
    most = FAR[0] if decisions is True else int(decisions)
    best: dict[int, tuple[int, int]] = {}
    heap = [((0, 0), root) for root in roots]
    heapq.heapify(heap)
    while heap:
        way, ident = heapq.heappop(heap)
        if ident in best:
            continue
        best[ident] = way
        event = events.get(ident)
        if not event or event.get("forgotten"):
            continue
        for source in event["sources"]:
            if source[0] != "ram" or source[2] in best:
                continue
            role = ROLES[source[1]]
            if role == DECISION and way[0] >= most:
                continue
            heapq.heappush(heap, (further(way, role), source[2]))
    return best


def site_key(event: dict) -> tuple:
    return (event["what"], event["at"][0], event["at"][1], event["address"], event["cell"])


class Site:
    """One instruction's writes to one byte, taken together."""

    def __init__(self, key: tuple) -> None:
        self.key = key
        self.what, self.bank, self.pc, self.address, self.cell = key
        self.events: list[dict] = []
        self.strength = FAR
        self.leaves: dict[tuple, int] = {}      # (kind, ...) -> role at its strongest
        self.edges: dict[tuple, int] = {}       # another site's key -> role at its strongest
        self.itself = 0                         # writes made of this instruction's own earlier ones
        self.cut = 0                            # sources the record did not follow
        self.forgotten = 0                      # ...and ones the trace had written over
        self.more = False
        self.more_decisions = False


def sites_of(record: dict, roots: list[int], decisions: bool | int) -> tuple[dict[tuple, Site], dict]:
    events = record["by_id"]
    best = strengths(record, roots, decisions)
    most = FAR[0] if decisions is True else int(decisions)
    sites: dict[tuple, Site] = {}
    for ident in best:
        event = events.get(ident)
        if not event or event.get("forgotten"):
            continue
        site = sites.setdefault(site_key(event), Site(site_key(event)))
        site.events.append(event)
        site.strength = min(site.strength, best[ident])
        site.more |= bool(event.get("more"))
        site.more_decisions |= bool(event.get("more_decisions"))
        for source in event["sources"]:
            kind, role = source[0], ROLES[source[1]]
            if role == DECISION and best[ident][0] >= most:
                continue
            if kind != "ram":
                leaf = (kind, *source[2:])
                site.leaves[leaf] = min(site.leaves.get(leaf, 3), role)
                continue
            there = events.get(source[2])
            if there is None:
                site.cut += 1
            elif there.get("forgotten"):
                site.forgotten += 1
            elif site_key(there) == site.key:
                site.itself += 1
            else:
                site.edges[site_key(there)] = min(site.edges.get(site_key(there), 3), role)
    for site in sites.values():
        site.events.sort(key=lambda event: event["id"])
    return sites, best


def values_of(events: list[dict]) -> str:
    seen: list[int] = []
    for event in events:
        if event["value"] not in seen:
            seen.append(event["value"])
    text = ", ".join(f"${value:02X}" for value in seen[:4])
    return text + (", ..." if len(seen) > 4 else "")


WHAT = {
    "store": "written at",
    "push": "pushed at",
    "return_address": "the return address of the call at",
    "interrupt": "the return address of an interrupt taken before",
    "oam_copy": "copied into the sprite table by the hardware, started at",
    "block_copy": "copied into video memory by the hardware, while at",
}


def runs(offsets: list[int]) -> list[tuple[int, int]]:
    """Consecutive ROM offsets in one bank as (first, size)."""
    out: list[tuple[int, int]] = []
    for offset in sorted(set(offsets)):
        if out and offset == out[-1][0] + out[-1][1] and offset // 0x4000 == out[-1][0] // 0x4000:
            out[-1] = (out[-1][0], out[-1][1] + 1)
        else:
            out.append((offset, 1))
    return out


class Printer:
    def __init__(self, context: Context, record: dict, depth: int | None) -> None:
        self.context = context
        self.names = context.names
        self.record = record
        self.depth = depth
        self.lines: list[str] = []
        self.number: dict[tuple, int] = {}
        self.parent: dict[tuple, tuple | None] = {}
        self.level: dict[tuple, int] = {}

    def lay_out(self, sites: dict[tuple, "Site"], roots: list[tuple]) -> None:
        """Number the writing instructions nearest first, and put each under the nearest one that leads to it."""
        queue = deque()
        for root in roots:
            if root not in self.number:
                self.number[root] = len(self.number) + 1
                self.parent[root] = None
                self.level[root] = 0
                queue.append(root)
        while queue:
            key = queue.popleft()
            for there in self.edges(sites, sites[key]):
                if there not in self.number:
                    self.number[there] = len(self.number) + 1
                    self.parent[there] = key
                    self.level[there] = self.level[key] + 1
                    queue.append(there)

    @staticmethod
    def edges(sites: dict[tuple, "Site"], site: "Site") -> list[tuple]:
        return sorted(site.edges, key=lambda key: (site.edges[key], sites[key].events[0]["id"]))

    def rom_bytes(self, first: int, size: int, values: dict[int, int]) -> str:
        bank, address = bank_address(first)
        where = f"{bank:02X}:{address:04X}" + (f"-{address + size - 1:04X}" if size > 1 else "")
        name = self.names.data(bank, address)
        said = " ".join(f"${values[offset]:02X}" for offset in range(first, first + size) if offset in values)
        file = self.context.file_of(first)
        return f"ROM {where}" + (f" {name}" if name else "") + (f" = {said}" if said else "") + \
            (f"  [{file}]" if file else "")

    def constant(self, leaf: tuple, read_bank: int | None, short: bool = False) -> str:
        _kind, offset, size, value = leaf
        bank, address = bank_address(offset - 1)
        text = f"constant ${value:0{size * 2}X}"
        if size == 2:
            # A word that is an address has a name: in RAM, in the fixed bank, or in the bank read through it.
            name = None
            if value >= 0x8000:
                name = self.names.variable(value)
            elif value < 0x4000 or read_bank is not None:
                name = self.names.data(read_bank or 0, value)
            if name:
                text += f" ({name})"
        if short:
            return f"{text} at {bank:02X}:{address:04X}"
        return f"{text} at {self.context.instruction(bank, address)}"

    def leaf_lines(self, site: "Site") -> list[tuple[int, str]]:
        """A site's sources that are no events, each with its role. What only decided is said briefly."""
        out: list[tuple[int, str]] = []
        rom = [leaf for leaf in site.leaves if leaf[0] == "rom"]
        values = {leaf[1]: leaf[2] for leaf in rom}
        read_banks = {leaf[1] // 0x4000 for leaf in rom if leaf[1] >= 0x4000}
        read_bank = next(iter(read_banks)) if len(read_banks) == 1 else None
        for role in (VALUE, ADDRESS, DECISION):
            for first, size in runs([leaf[1] for leaf in rom if site.leaves[leaf] == role]):
                out.append((role, self.rom_bytes(first, size, values)))
        for leaf, role in site.leaves.items():
            kind = leaf[0]
            short = role == DECISION
            if kind == "constant":
                out.append((role, self.constant(leaf, read_bank, short)))
            elif kind == "constant_in_ram":
                out.append((role, f"constant ${leaf[3]:0{leaf[2] * 2}X} in code in RAM at {leaf[1]:04X}"))
            elif kind == "hardware":
                out.append((role, f"hardware {self.names.place(leaf[1])}" if short else
                            f"hardware {self.names.place(leaf[1])} read at "
                            f"{self.context.instruction(leaf[2], leaf[3])}"))
            elif kind == "start":
                out.append((role, f"{self.names.place(leaf[1])} as it was" if short else
                            f"{self.names.place(leaf[1])} as it was when the trace began"))
            elif kind == "start_register":
                out.append((role, f"register {leaf[1]} as it was" if short else
                            f"register {leaf[1]} as it was when the trace began"))
            elif kind == "unseen":
                out.append((role, f"{self.names.place(leaf[1])} as something that is no instruction left it "
                                  f"(a write from outside the game, or one the hardware dropped)"))
            elif kind != "rom":
                out.append((role, f"something the tool does not know ({kind})"))
        out.sort(key=lambda entry: entry[0])
        return out

    def site_line(self, site: "Site") -> str:
        count = len(site.events)
        text = f"[{self.number[site.key]}] {self.names.place(site.address)} = {values_of(site.events)}, " \
               f"{WHAT.get(site.what, site.what)} {self.context.instruction(site.bank, site.pc)}"
        if count > 1:
            text += f"  ({count} writes)"
        return text

    def wrapped(self, indent: str, label: str, items: list[str]) -> None:
        """Short things on as few lines as they take."""
        line = f"{indent}{label:<11} "
        start = len(line)
        for index, item in enumerate(items):
            piece = item + ("; " if index + 1 < len(items) else "")
            if len(line) + len(piece) > 150 and len(line) > start:
                self.lines.append(line.rstrip())
                line = " " * start
            line += piece
        self.lines.append(line.rstrip())

    def under(self, sites: dict[tuple, "Site"], site: "Site", indent: str) -> None:
        leaves = self.leaf_lines(site)
        for role, text in leaves:
            if role != DECISION:
                self.lines.append(f"{indent}{LABELS[role]:<11} {text}")
        if site.itself:
            self.lines.append(f"{indent}{LABELS[VALUE]:<11} its own earlier value ({site.itself} times)")
        decided = [text for role, text in leaves if role == DECISION]
        if decided:
            self.wrapped(indent, LABELS[DECISION], decided)
        if site.cut:
            self.lines.append(f"{indent}...         {site.cut} sources the record did not follow (its depth or "
                              f"its number of nodes)")
        if site.forgotten:
            self.lines.append(f"{indent}...         {site.forgotten} sources the trace had forgotten")
        if site.more:
            self.lines.append(f"{indent}...         more sources than a value keeps: some were dropped")
        elif site.more_decisions:
            self.lines.append(f"{indent}...         more decisions than a value keeps: some were dropped")
        elsewhere: dict[int, list[str]] = {VALUE: [], ADDRESS: [], DECISION: []}
        for key in self.edges(sites, site):
            role = site.edges[key]
            there = sites[key]
            if self.parent.get(key) != site.key:
                elsewhere[role].append(f"[{self.number[key]}] {self.names.place(there.address)}")
                continue
            self.lines.append(f"{indent}{LABELS[role]:<11} {self.site_line(there)}")
            if self.depth is not None and self.level[key] > self.depth:
                if there.leaves or there.edges:
                    self.lines.append(f"{indent}  ...")
                continue
            self.under(sites, there, indent + "  ")
        for role in (VALUE, ADDRESS, DECISION):
            if elsewhere[role]:
                self.wrapped(indent, LABELS[role], elsewhere[role])


def summary_of(context: Context, sites: dict[tuple, Site], roots: set[tuple]) -> dict:
    """What a tree comes to: the functions, the ROM, the constants, the hardware and the RAM on the way, each
    with how it took part at best (`role`) and how many decisions lie between it and the write (`decisions`)."""
    names = context.names
    functions: list[dict] = []
    seen_functions: set = set()

    def function(bank: int, address: int) -> None:
        if address >= 0x8000:
            key, start, name = ("ram", address), address, f"code in RAM at {address:04X}"
        else:
            found = names.function(bank, address)
            start, name = found if found else (address, f"{bank:02X}:{address:04X}")
            key = (bank if start >= 0x4000 else 0, start)
        if key not in seen_functions:
            seen_functions.add(key)
            functions.append({"bank": key[0] if key[0] != "ram" else 0, "address": start, "name": name})

    # Nearest first: by the decisions on the way, then by the order the events were made in, latest first.
    order = sorted(sites, key=lambda key: (sites[key].strength, -sites[key].events[-1]["id"]))
    rom: dict[int, tuple[int, int]] = {}
    leaves: dict[tuple, tuple[int, int]] = {}
    ram: dict[int, dict] = {}
    for key in order:
        site = sites[key]
        function(site.bank, site.pc)
        for leaf, role in site.leaves.items():
            way = further(site.strength, role)
            if leaf[0] == "rom":
                rom[leaf[1]] = min(rom.get(leaf[1], FAR), way)
            else:
                leaves[leaf] = min(leaves.get(leaf, FAR), way)
            if leaf[0] == "constant":
                function(*bank_address(leaf[1] - 1))
            elif leaf[0] == "hardware":
                function(leaf[2], leaf[3])
        if key not in roots and site.what in ("store", "oam_copy", "block_copy"):
            known = ram.setdefault(site.address, {"address": site.address, "name": names.variable(site.address),
                                                  "way": site.strength, "writes": 0})
            known["way"] = min(known["way"], site.strength)
            known["writes"] += len(site.events)

    def said(way: tuple[int, int]) -> dict:
        return {"role": ROLE_NAMES[role_of(way)], "decisions": way[0]}

    rom_runs = []
    for way in sorted(set(rom.values())):
        for first, size in runs([offset for offset, how in rom.items() if how == way]):
            bank, address = bank_address(first)
            rom_runs.append({"offset": first, "size": size, "bank": bank, "address": address,
                             "name": names.data(bank, address), "roles": [ROLE_NAMES[role_of(way)]],
                             "decisions": way[0]})
    rom_runs.sort(key=lambda run: (run["decisions"], run["offset"]))
    constants, hardware, start, unseen = [], [], [], []
    for leaf, way in sorted(leaves.items(), key=lambda item: item[1]):
        if leaf[0] == "constant":
            bank, address = bank_address(leaf[1] - 1)
            constants.append({"offset": leaf[1], "size": leaf[2], "value": leaf[3], "bank": bank,
                              "address": address, **said(way)})
        elif leaf[0] == "constant_in_ram":
            constants.append({"ram": leaf[1], "size": leaf[2], "value": leaf[3], **said(way)})
        elif leaf[0] == "hardware":
            known = next((entry for entry in hardware if entry["register"] == leaf[1]), None)
            if known is None:
                hardware.append({"register": leaf[1], "name": names.variable(leaf[1]), "bank": leaf[2],
                                 "address": leaf[3], **said(way)})
        elif leaf[0] == "start":
            start.append({"address": leaf[1], "name": names.variable(leaf[1]), **said(way)})
        elif leaf[0] == "start_register":
            start.append({"register": leaf[1], **said(way)})
        elif leaf[0] == "unseen":
            unseen.append({"address": leaf[1], "name": names.variable(leaf[1]), **said(way)})
    ram_list = [{"address": entry["address"], "name": entry["name"], "writes": entry["writes"], **said(entry["way"])}
                for entry in sorted(ram.values(), key=lambda entry: entry["way"])]
    return {"functions": functions, "rom": rom_runs, "constants": constants, "hardware": hardware, "ram": ram_list,
            "start": start, "unseen": unseen,
            "not_followed": sum(site.cut for site in sites.values()),
            "forgotten": sum(site.forgotten for site in sites.values())}


# --------------------------------------------------------------------------
# show
# --------------------------------------------------------------------------

def grouped(record: dict, chosen: list[tuple[dict, dict]]) -> list[list[tuple[dict, dict]]]:
    """Writes of neighbouring bytes made one after the other in the same calls go together: the two
    halves of a word, the three bytes of a number."""
    def calls(write: dict) -> list:
        return [(call["entered"], call["returns_to"]) for call in write["calls"]]

    groups: list[list[tuple[dict, dict]]] = []
    for write, tree in sorted(chosen, key=lambda pair: pair[1]["event"]):
        for group in groups:
            last_write, last_tree = group[-1]
            addresses = [member[0]["address"] for member in group]
            if (write["address"] in (min(addresses) - 1, max(addresses) + 1) and
                    calls(write) == calls(last_write) and tree["frame"] == last_tree["frame"] and
                    0 < tree["event"] - last_tree["event"] <= 256):
                group.append((write, tree))
                break
        else:
            groups.append([(write, tree)])
    for group in groups:
        group.sort(key=lambda pair: pair[0]["address"])
    return groups


def chosen_trees(record: dict, wanted: list[tuple[int, int]], every: bool) -> list[tuple[dict, dict]]:
    out = []
    for write in record["writes"]:
        if wanted and not any(low <= write["address"] <= high for low, high in wanted):
            continue
        for tree in write["trees"] if every else write["trees"][:1]:
            out.append((write, tree))
    return out


def describe(context: Context, record: dict, group: list[tuple[dict, dict]], decisions: bool,
             depth: int | None) -> tuple[list[str], dict]:
    names = context.names
    printer = Printer(context, record, depth)
    events = record["by_id"]
    roots = [tree["event"] for _write, tree in group]
    sites, _best = sites_of(record, roots, decisions)
    first, last = group[0][0]["address"], group[-1][0]["address"]
    name = names.variable(first)
    span = f"{first:04X}" + (f"-{last:04X}" if last != first else "")
    value = " ".join(f"${tree['value']:02X}" for _write, tree in group)
    head = (f"{name} ({span})" if name else span) + f" = {value}"
    if len(group) > 1:
        head += "  (bytes in the order of their addresses)"
    printer.lines.append(head)
    root_keys = []
    for write, tree in group:
        event = events[tree["event"]]
        root_keys.append(site_key(event))
    root_keys = list(dict.fromkeys(root_keys))
    printer.lay_out(sites, root_keys)
    for write, tree in group:
        event = events[tree["event"]]
        times = f"{write['count']} write" + ("s" if write["count"] != 1 else "")
        frames = f"frame {tree['frame']}" if write["count"] == 1 else \
            f"this one in frame {tree['frame']}, the first in {write['first_frame']}, the last in {write['last_frame']}"
        printer.lines.append(f"  [{printer.number[site_key(event)]}] {write['address']:04X} "
                             f"{WHAT.get(event['what'], event['what'])} "
                             f"{context.instruction(*event['at'])}  ({times}; {frames})")
    write = group[0][0]
    if write["calls"]:
        chain = []
        for call in write["calls"]:
            how = "an interrupt taken before" if call["interrupt"] else "called from"
            chain.append(f"{names.code(*call['entered'])}, {how} {names.code(*call['from'])}")
        missing = write["calls_in_progress"] - len(write["calls"])
        printer.lines.append("  inside, outermost first:" + (f" ({missing} more outside these)" if missing else ""))
        printer.lines += [f"    {entry}" for entry in chain]
    else:
        printer.lines.append("  inside no call the trace saw made")
    for key in root_keys:
        site = sites[key]
        if len(root_keys) > 1:
            printer.lines.append(f"  [{printer.number[key]}] {site.address:04X}:")
        printer.under(sites, site, "    " if len(root_keys) > 1 else "  ")
    summary = summary_of(context, sites, set(root_keys))
    cut = sum(tree["cut_by_depth"] + tree["cut_by_nodes"] for _write, tree in group)
    lines = printer.lines

    def took_part(role: int, away: int, entry: dict) -> bool:
        return ROLES[entry["roles"][0] if "roles" in entry else entry["role"]] == role and entry["decisions"] == away

    furthest = max([entry["decisions"] for kind in ("rom", "constants", "hardware", "ram", "start", "unseen")
                    for entry in summary[kind]] or [0])
    for role, away in [(VALUE, 0), (ADDRESS, 0)] + [(DECISION, count) for count in range(1, furthest + 1)]:
        title = ("made of" if role == VALUE else "loaded through" if role == ADDRESS else
                 "decided by" if away == 1 else f"decided by what decided that ({away} decisions away)")
        rom = [run for run in summary["rom"] if took_part(role, away, run)]
        constants = [entry for entry in summary["constants"] if took_part(role, away, entry)]
        hardware = [entry for entry in summary["hardware"] if took_part(role, away, entry)]
        ram = [entry for entry in summary["ram"] if took_part(role, away, entry)]
        start = [entry for entry in summary["start"] if took_part(role, away, entry)]
        unseen = [entry for entry in summary["unseen"] if took_part(role, away, entry)]
        if not (rom or constants or hardware or ram or start or unseen):
            continue
        lines.append(f"  in all, {title}:")
        for run in rom:
            where = f"{run['bank']:02X}:{run['address']:04X}" + \
                (f"-{run['address'] + run['size'] - 1:04X}" if run["size"] > 1 else "")
            file = context.file_of(run["offset"])
            lines.append(f"    ROM {where}" + (f" {run['name']}" if run["name"] else "") +
                         f"  {run['size']} byte" + ("s" if run["size"] != 1 else "") + (f"  [{file}]" if file else ""))
        for entry in constants:
            where = f"code in RAM at {entry['ram']:04X}" if "ram" in entry else \
                context.instruction(entry["bank"], entry["address"])
            lines.append(f"    constant ${entry['value']:0{entry['size'] * 2}X} at {where}")
        if hardware:
            printer.wrapped("    ", "hardware:", [names.place(entry["register"]) for entry in hardware])
        if ram:
            printer.wrapped("    ", "RAM:", [names.place(entry["address"]) for entry in ram])
        if start:
            printer.wrapped("    ", "as it was:", [names.place(entry["address"]) if "address" in entry else
                                                    f"register {entry['register']}" for entry in start])
        if unseen:
            printer.wrapped("    ", "changed by something that is no instruction:",
                            [names.place(entry["address"]) for entry in unseen])
    if names.labels or names.spans:
        printer.wrapped("  ", "functions on the way:", [entry["name"] for entry in summary["functions"]])
    else:
        lines.append("  functions on the way: not known without --export or --sym")
    if summary["not_followed"] or summary["forgotten"] or cut:
        lines.append(f"  not followed: {summary['not_followed']} sources past the record's depth or number of "
                     f"nodes, {summary['forgotten']} the trace had forgotten")
    as_json = {
        "addresses": [member[0]["address"] for member in group],
        "name": name,
        "values": [member[1]["value"] for member in group],
        "frame": group[0][1]["frame"],
        "written_at": [events[tree["event"]]["at"] for _write, tree in group],
        "counts": [member[0]["count"] for member in group],
        "calls": write["calls"],
        "summary": summary,
        "instructions": [
            {"at": [site.bank, site.pc], "address": site.address, "name": names.variable(site.address),
             "what": site.what, "writes": len(site.events), "role": ROLE_NAMES[role_of(site.strength)],
             "decisions": site.strength[0],
             "function": names.function_name(site.bank, site.pc)} for site in sites.values()],
    }
    return lines, as_json


def how_far(args) -> bool | int:
    """How many decisions one after another are followed: none, so many, or all."""
    if args.no_decisions:
        return 0
    if args.decisions is not None:
        if args.decisions < 0:
            raise TraceError("--decisions takes a number from 0 up")
        return args.decisions
    return True


def command_show(args) -> int:
    record = load_record(args.record)
    context = Context(args)
    context.check(record)
    wanted = [parse_range(text) for text in args.of or ()]
    chosen = chosen_trees(record, wanted, args.all)
    if not chosen:
        seen = ", ".join(sorted({f"{write['address']:04X}" for write in record["writes"]})) or "none"
        raise TraceError(f"{args.record} has no tree for " + (", ".join(args.of) if args.of else "any write") +
                         f" (addresses written: {seen})")
    out = []
    as_json = []
    for group in grouped(record, chosen):
        lines, described = describe(context, record, group, how_far(args), args.depth)
        out.append("\n".join(lines))
        as_json.append(described)
    if args.json:
        print(json.dumps({"schema": "gb-trace-back.show", "version": 1, "record": str(args.record),
                          "writes": as_json}, indent=1))
        return 0
    print("\n\n".join(out))
    notes = []
    if record["memory"]["forgetting"]:
        notes.append(f"the trace's memory ({record['memory']['kilobytes'] / 1024:g} MB) was full: the oldest events were "
                     f"forgotten (--trace-back-memory)")
    if record["writes_without_tree"]:
        notes.append(f"{record['writes_without_tree']} of {record['watched_writes']} watched writes have no tree: "
                     f"a tree is kept for the first {record['limits']['each']} of each instruction and set of "
                     f"calls, and {record['limits']['trees']} in all (--trace-back-each, --trace-back-trees)")
    if record["state_loads"]:
        notes.append("a save state was loaded: what was in memory then is 'as it was when the trace began', and "
                     "the calls in progress then are not known")
    if record["out_of_memory"]:
        notes.append("the game ran out of memory for the record: it is not whole")
    if notes:
        print("\n" + "\n".join(f"note: {note}" for note in notes))
    return 0


# --------------------------------------------------------------------------
# summary
# --------------------------------------------------------------------------

def took(way: tuple[int, int]) -> str:
    """`made of`, `through`, or `decided by` with how many decisions away."""
    if way[0] > 1:
        return f"{LABELS[DECISION]}, {way[0]} away"
    return LABELS[DECISION if way[0] else way[1]]


def command_summary(args) -> int:
    records = [load_record(path) for path in args.records]
    context = Context(args)
    for record in records:
        context.check(record)
        if record["rom"] != records[0]["rom"]:
            raise TraceError(f"{record['path']} was recorded from another ROM than {records[0]['path']}")
    names = context.names
    decisions = how_far(args)
    by_address: dict[int, dict] = {}
    for record in records:
        for write in record["writes"]:
            entry = by_address.setdefault(write["address"], {"writes": 0, "records": set(), "at": {}, "rom": {},
                                                             "ram": {}, "functions": {}, "trees": 0})
            entry["writes"] += write["count"]
            entry["records"].add(record["path"])
            where = tuple(write["at"])
            entry["at"][where] = entry["at"].get(where, 0) + write["count"]
            for tree in write["trees"]:
                entry["trees"] += 1
                sites, _best = sites_of(record, [tree["event"]], decisions)
                root = site_key(record["by_id"][tree["event"]])
                summary = summary_of(context, sites, {root})
                for run in summary["rom"]:
                    for offset in range(run["offset"], run["offset"] + run["size"]):
                        way = (run["decisions"], ROLES[run["roles"][0]])
                        entry["rom"][offset] = min(entry["rom"].get(offset, FAR), way)
                for there in summary["ram"]:
                    way = (there["decisions"], ROLES[there["role"]])
                    entry["ram"][there["address"]] = min(entry["ram"].get(there["address"], FAR), way)
                for function in summary["functions"]:
                    entry["functions"].setdefault((function["bank"], function["address"]), function["name"])
    if args.json:
        out = []
        for address in sorted(by_address):
            entry = by_address[address]
            out.append({
                "address": address, "name": names.variable(address), "writes": entry["writes"],
                "records": len(entry["records"]), "trees": entry["trees"],
                "written_at": [{"at": list(where), "count": count} for where, count in sorted(entry["at"].items())],
                "rom": [{"offset": first, "size": size, "bank": bank_address(first)[0],
                         "address": bank_address(first)[1], "name": names.data(*bank_address(first))}
                        for first, size in runs(list(entry["rom"]))],
                "ram": [{"address": there, "name": names.variable(there), "role": ROLE_NAMES[way[1]],
                         "decisions": way[0]} for there, way in sorted(entry["ram"].items())],
                "functions": [{"bank": bank, "address": start, "name": name}
                              for (bank, start), name in entry["functions"].items()]})
        print(json.dumps({"schema": "gb-trace-back.summary", "version": 1, "addresses": out}, indent=1))
        return 0
    for address in sorted(by_address):
        entry = by_address[address]
        count = len(entry["records"])
        print(f"{names.place(address)}: {entry['writes']} write" + ("s" if entry["writes"] != 1 else "") +
              f" in {count} record" + ("s" if count != 1 else "") + f", {entry['trees']} with a tree")
        for where, times in sorted(entry["at"].items(), key=lambda item: -item[1]):
            print(f"  written at {context.instruction(*where)}  ({times})")
        if entry["functions"]:
            print("  functions on the way: " + ", ".join(entry["functions"].values()))
        for way in sorted(set(entry["rom"].values())):
            for first, size in runs([offset for offset, how in entry["rom"].items() if how == way]):
                bank, start = bank_address(first)
                name = names.data(bank, start)
                where = f"{bank:02X}:{start:04X}" + (f"-{start + size - 1:04X}" if size > 1 else "")
                file = context.file_of(first)
                print(f"  ROM {where}" + (f" {name}" if name else "") + f"  {size} byte" + ("s" if size != 1 else "") +
                      f"  ({took(way)})" + (f"  [{file}]" if file else ""))
        if entry["ram"]:
            print("  RAM on the way: " + ", ".join(
                f"{names.place(there)} ({took(way)})"
                for there, way in sorted(entry["ram"].items(), key=lambda item: (item[1], item[0]))))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def common(command) -> None:
        command.add_argument("--export", type=Path, help="a gbrecomp --lift-export file: its functions and names")
        command.add_argument("--sym", type=Path, action="append",
                             help="an RGBDS .sym file or a names.sym (may be given more than once)")
        command.add_argument("--rom", type=Path, help="the ROM, to write the instructions out")
        command.add_argument("--tree", type=Path, help="a data tree (gb_data.py split): the file each ROM byte is in")
        command.add_argument("--no-decisions", action="store_true",
                             help="leave out what only decided whether a write was made")
        command.add_argument("--decisions", type=int, metavar="N",
                             help="follow at most N decisions one after another (all of them, if not given)")
        command.add_argument("--json", action="store_true", help="for a program")

    show = commands.add_parser("show", help="each write as a tree")
    show.add_argument("record", type=Path)
    show.add_argument("--of", action="append", metavar="ADDRESS[-ADDRESS]",
                      help="only the writes to these addresses, hexadecimal (may be given more than once)")
    show.add_argument("--depth", type=int, help="how many writing instructions deep to print (all, if not given)")
    show.add_argument("--all", action="store_true", help="every tree of a write, not only its first")
    common(show)
    show.set_defaults(run=command_show)

    summary = commands.add_parser("summary", help="per watched address, over many records")
    summary.add_argument("records", type=Path, nargs="+")
    common(summary)
    summary.set_defaults(run=command_summary)

    args = parser.parse_args()
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="backslashreplace")       # a name may hold what the console cannot print
    try:
        return args.run(args)
    except TraceError as error:
        print(f"gb_trace_back: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
