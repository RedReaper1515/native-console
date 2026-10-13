#!/usr/bin/env python3
"""Calls the export cannot see: through helpers that take their target in registers.

`gb_lift.py edges` uses this. A function is "far-called" when the game reaches
it by loading its bank and address into registers and calling a helper that
switches bank and jumps there, or by handing a helper the number of an entry
in a table. The analysis does not follow that, so such a function has no
caller in the export, is often not a function in it at all (it is merged
into whatever decoded code runs into it), and sometimes was never decoded.

Where the call to the helper is made, though, the registers it reads usually
hold constants, and the export records them (`known`). So for each such call
the helper is run, from the ROM, in the checker's interpreter, with those
constants and everything else made up, several times over
(lift/checker/lift_probe.c). If it leaves for the same address in the same
bank every time, that is where this call goes.

A helper that looks its target up in a table goes somewhere different each
time, because the index is not a constant where it is called. But the probe
sees where the address came from: two bytes of the ROM, a few entries on
from a constant the call did load. That constant is the table, and its
entries can be read straight from the ROM, as far as the table goes. How far
is the one thing the ROM does not say, and reading past the end turns
whatever follows into "functions". So a table is taken only when it ends
exactly where code is known to begin: at the lowest address one of its own
entries points to, or at a function something calls.

What the ROM cannot say, playing can. A game run with `--evidence FILE`
records every jump it makes that its code does not spell out
(runtime/include/gbrt_evidence.h): the instruction, where it went, and the
CALL that entered the function it happened in. Those are merged in here.
They are facts about one run: what was not played is not in them, and a call
seen going to three places may go to a fourth.

And what neither says, a reader can. A game keeps most of its functions in
tables nothing here can follow: the script of each room, in the room's
header, which is copied to RAM and jumped through from there; what each item
does, in a table indexed by the item. Where such a table is, how long, and
where in each entry the address sits, is a few lines to whoever has read
the code that uses it, and hundreds of functions. `--tables` takes those
lines (a `gbrecomp.lift-tables` file) and reads the tables out of the ROM.

The result is a `gbrecomp.lift-edges` file. `gbrecomp --lift-edges` reads it:
the targets become entry points for the analysis and functions of their own
in the next export, with the calls that reach them listed.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

EDGES_SCHEMA = "gbrecomp.lift-edges"
PROBE_SOURCES = ("lift_machine.c", "lift_cpu.c", "lift_probe.c")
REGISTERS = ("a", "b", "c", "d", "e", "h", "l")


class EdgesError(Exception):
    pass


def function_id(bank: int, address: int) -> str:
    return f"gbfn:v1:{bank:04x}:{address:04x}"


def find_sites(export: dict) -> list[dict]:
    """Calls and jumps, with a constant in some register, to a function the
    export cannot follow to the end: it, or something it calls, jumps where a
    register or the stack says."""
    index = {function["id"]: function for function in export["functions"]}
    sites = []
    for function in export["functions"]:
        candidates = [(call, "call") for call in function["calls"]]
        candidates += [(jump, "jump") for jump in function["jumps_out"] if jump["kind"] == "jump"]
        for site, kind in candidates:
            known = site.get("known")
            helper = index.get(site["function"]) if site["function"] else None
            if not known or helper is None or not helper["summary_incomplete"]:
                continue
            sites.append({
                "from": function["id"],
                # The bank mapped where the call is made: the caller's own,
                # if it is in the switchable area.
                "bank": function["bank"] if site["site"] >= 0x4000 else 0,
                "site": site["site"],
                "kind": kind,
                "helper": helper["id"],
                "entry": helper["address"],
                "known": known,
            })
    return sites


def build_probe(compiler: str, checker_dir: Path, include_dir: Path, work: Path) -> Path:
    exe = work / ("lift_probe.exe" if os.name == "nt" else "lift_probe")
    inputs = [str(checker_dir / name) for name in PROBE_SOURCES]
    if Path(compiler).stem.lower() == "cl":
        command = [compiler, "/nologo", "/std:c11", "/W3", "/wd4201", "/D_CRT_SECURE_NO_WARNINGS",
                   "/O2", f"/I{include_dir}", f"/I{checker_dir}", *inputs, f"/Fe:{exe}"]
    else:
        command = [compiler, "-std=c11", "-O2", "-w", "-I", str(include_dir), "-I", str(checker_dir),
                   *inputs, "-o", str(exe)]
    result = subprocess.run(command, cwd=work, capture_output=True, text=True)
    if result.returncode != 0:
        raise EdgesError("the probe does not compile:\n" + (result.stdout + result.stderr).strip()[-4000:])
    return exe


def run_probes(exe: Path, rom: Path, requests: list[tuple[dict, dict]], trials: int, steps: int) -> list[str]:
    """Run the helper each site calls with the given register constants (a
    site and the constants to use, for each request). One answer each, in
    order."""
    if not requests:
        return []
    lines = []
    for site, known in requests:
        registers = " ".join(str(known.get(name, -1)) for name in REGISTERS)
        # The made-up part of the states depends on where the call is and on
        # nothing else, so a call is tried from the same states whichever
        # export it was found in, and whatever constants it is given.
        seed = (site["bank"] << 16) | site["site"]
        # Called from the home bank, any bank may be mapped; one has to be.
        lines.append(f"{seed} {site['bank'] or 1} {site['entry']} {int(site['kind'] == 'jump')} "
                     f"{registers} {trials} {steps}")
    result = subprocess.run([str(exe), str(rom)], input="\n".join(lines) + "\n", capture_output=True, text=True)
    outcomes = [line.partition(" ")[2] for line in result.stdout.splitlines()]
    if result.returncode != 0 or len(outcomes) != len(requests):
        raise EdgesError("the probe failed:\n" + result.stderr.strip()[-2000:])
    return outcomes


TRUSTED_KINDS = ("call", "far_call", "table_call", "declared_table", "entry", "interrupt")
TABLE_LIMIT = 256


def rom_word(rom: bytes, bank: int, address: int) -> int | None:
    offset = bank * 0x4000 + (address - 0x4000) if address >= 0x4000 else address
    if address >= 0x8000 or offset + 1 >= len(rom):
        return None
    return rom[offset] | (rom[offset + 1] << 8)


TABLE_NUDGE = 16


def table_slots(outcome: str) -> list[int] | None:
    """Where in the ROM each state's target was read from, for a `table` answer."""
    words = outcome.split()
    if not words or words[0] != "table":
        return None
    count = int(words[4])
    return [int(word) for word in words[5:5 + 3 * count:3]]


def table_bases(sites: list[dict], outcomes: list[str], probe) -> dict[int, int]:
    """For each site (by its number) whose helper took its target from a
    table: the constant of the call that the table starts at. Which of the
    constants that is, is found by trying: move one a little, run the same
    states again, and see whether every entry read moved with it."""
    requests = []
    moved = []
    for number, (site, outcome) in enumerate(zip(sites, outcomes)):
        slots = table_slots(outcome)
        if slots is None:
            continue
        known = site["known"]
        for high, low in (("h", "l"), ("d", "e"), ("b", "c")):
            if high not in known or low not in known:
                continue
            value = (known[high] << 8) | known[low]
            if not all(0 <= slot - value < 2 * TABLE_LIMIT and (slot - value) % 2 == 0 for slot in slots):
                continue
            nudged = (value + TABLE_NUDGE) & 0xFFFF
            requests.append((site, {**known, high: nudged >> 8, low: nudged & 0xFF}))
            moved.append((number, value, slots))
    found: dict[int, list[int]] = {}
    for (number, value, slots), answer in zip(moved, probe(requests)):
        after = table_slots(answer)
        if after is not None and len(after) == len(slots) and \
                all(new - old == TABLE_NUDGE for old, new in zip(slots, after)):
            found.setdefault(number, []).append(value)
    return {number: values[0] for number, values in found.items() if len(values) == 1}


def read_table(rom: bytes, bank: int, base: int, target_bank: int,
               begins: set[int]) -> list[tuple[int, int]] | None:
    """The entries of the table at bank:base, as (bank, address), or None if
    it does not end where something is known to begin. `begins` are the
    addresses in the table's bank where a function something calls starts."""
    later = [address for address in begins if address > base]
    limit = min(later) if later else None
    entries: list[tuple[int, int]] = []
    for index in range(TABLE_LIMIT):
        slot = base + 2 * index
        if limit is not None and slot + 2 > limit:
            # Ended exactly at code, or with a byte left over, which no
            # table of words has.
            return entries if slot == limit and entries else None
        target = rom_word(rom, bank, slot)
        if target is None or not 0x0150 <= target < 0x8000 or base <= target < slot + 2:
            return None
        entries.append((target_bank if target >= 0x4000 else 0, target))
        # A table does not run into what it points to.
        if (target >= 0x4000) == (base >= 0x4000) and target > base:
            limit = target if limit is None else min(limit, target)
    return None


def resolve(export: dict, sites: list[dict], outcomes: list[str], unprompted: list[str],
            bases: dict[int, int], rom: bytes,
            declared: dict[tuple[int, int], int] | None = None,
            unread: list[dict] | None = None) -> tuple[list[dict], dict[str, int]]:
    """Edges from the probe's answers, and a count of every kind of answer.

    `unprompted` is what the same helpers did with nothing known in the
    registers. One that goes to the same place then goes there by itself: it
    is a function that makes a far call of its own, which is found where it
    makes it, and the call to it is an ordinary call.

    `declared` is how many entries somebody says the table at (bank,
    address) has: taken where the ROM does not mark the table's end. The
    tables left unread for want of that are added to `unread`, each with a
    call that goes through it."""
    banks = export["rom"]["rom_banks"]
    begins: dict[int, set[int]] = {}
    for function in export["functions"]:
        if function["kind"] in TRUSTED_KINDS:
            begins.setdefault(function["bank"], set()).add(function["address"])
    counts: dict[str, int] = {}
    edges = []
    for number, (site, outcome, control) in enumerate(zip(sites, outcomes, unprompted)):
        words = outcome.split()
        kind = words[0] if words else "no answer"
        if kind == "to" and control == outcome:
            kind = "same without the constants"
        elif kind == "table":
            left_at, returns_to, table_bank, count = (int(word) for word in words[1:5])
            seen = [int(word) for word in words[5:5 + 3 * count]]
            base = bases.get(number)
            # The bank the entries are in: the one mapped when the helper
            # jumped to one of them.
            target_banks = {bank for bank, target in zip(seen[1::3], seen[2::3]) if target >= 0x4000}
            target_bank = target_banks.pop() if len(target_banks) == 1 else table_bank
            entries = None
            if base is None:
                kind = "table that does not start at a constant of the call"
            elif base >= 0x4000 and not 0 < table_bank < banks:
                kind = "to a bank the ROM does not have"
            else:
                home = table_bank if base >= 0x4000 else 0
                entries = read_table(rom, home, base, target_bank, begins.get(home, set()))
                if entries is None and declared and (home, base) in declared:
                    words = [rom_word(rom, home, base + 2 * index) for index in range(declared[(home, base)])]
                    entries = [(target_bank if word >= 0x4000 else 0, word) for word in words]
                    kind = "table as long as somebody says"
                if entries is None:
                    kind = "table whose end the ROM does not mark"
                    if unread is not None and not any((table["bank"], table["address"]) == (home, base)
                                                      for table in unread):
                        unread.append({"bank": home, "address": base, "from": site["from"], "site": site["site"]})
            for index, (bank, address) in enumerate(entries or ()):
                edges.append({
                    "from": site["from"], "site": site["site"], "kind": site["kind"],
                    "helper": site["helper"], "known": site["known"],
                    "bank": bank, "address": address,
                    "returns_to": returns_to, "left_at": left_at,
                    "table": {"bank": table_bank if base >= 0x4000 else 0, "address": base, "index": index},
                })
        elif kind == "to":
            bank, address, returns_to, left_at = (int(word) for word in words[1:5])
            if address >= 0x8000:
                kind = "to RAM"
            elif address >= 0x4000 and not 0 < bank < banks:
                kind = "to a bank the ROM does not have"
            elif 0x0100 <= address < 0x0150:
                kind = "to the cartridge header"
            else:
                edges.append({
                    "from": site["from"], "site": site["site"], "kind": site["kind"],
                    "helper": site["helper"], "known": site["known"],
                    "bank": bank if address >= 0x4000 else 0, "address": address,
                    "returns_to": returns_to, "left_at": left_at,
                })
        counts[kind] = counts.get(kind, 0) + 1
    return edges, counts


TABLES_SCHEMA = "gbrecomp.lift-tables"
DECLARED_LIMIT = 4096   # entries of one declared table, however it says it ends
SHAPE_FIELDS = ("entries", "ends_with_byte", "entry_size", "pointer_at", "target_bank", "skip", "unused",
                "leads_to")
# Bytes that are no instruction, and how long the instructions that are, are.
NO_INSTRUCTION = frozenset((0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD))
TWO_BYTES = frozenset((0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x36, 0x3E, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
                       0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6, 0xFE, 0xE0, 0xF0, 0xE8, 0xF8, 0xCB))
THREE_BYTES = frozenset((0x01, 0x11, 0x21, 0x31, 0x08, 0xC2, 0xCA, 0xD2, 0xDA, 0xC3, 0xC4, 0xCC, 0xD4, 0xDC,
                         0xCD, 0xEA, 0xFA))
LEAVES = frozenset((0xC3, 0xC9, 0xD9, 0xE9, 0x18))   # jp, ret, reti, jp hl, jr
LOOK_AHEAD = 24         # instructions looked at from where an entry points


def rom_byte(rom: bytes, bank: int, address: int) -> int | None:
    offset = bank * 0x4000 + (address - 0x4000) if address >= 0x4000 else address
    if not 0 <= address < 0x8000 or offset >= len(rom):
        return None
    return rom[offset]


def no_instruction_at(rom: bytes, bank: int, address: int) -> int | None:
    """The address, within a few instructions of bank:address, of a byte that
    is no instruction, or None. Most data decodes as something, so this only
    catches a table read from the wrong place by a wide margin; it is a net
    under the reader, not a judge of the table."""
    end = 0x4000 if address < 0x4000 else 0x8000
    for _ in range(LOOK_AHEAD):
        opcode = rom_byte(rom, bank, address)
        if opcode is None or opcode in NO_INSTRUCTION:
            return address
        length = 3 if opcode in THREE_BYTES else 2 if opcode in TWO_BYTES else 1
        if address + length > end:
            return address
        if opcode in LEAVES:
            return None
        address += length
    return None


def check_shape(shape: dict, where: str) -> None:
    unknown = sorted(set(shape) - set(SHAPE_FIELDS) - {"name", "bank", "address", "why"})
    if unknown:
        raise EdgesError(f"{where}: no such field as '{unknown[0]}'")
    if ("entries" in shape) == ("ends_with_byte" in shape):
        raise EdgesError(f"{where}: say how long it is with 'entries', or with 'ends_with_byte', one of the two")
    numbers = {"entries": (1, DECLARED_LIMIT), "ends_with_byte": (0, 255), "entry_size": (1, 255),
               "pointer_at": (0, 253)}
    for name, (low, high) in numbers.items():
        if name in shape and not (isinstance(shape[name], int) and low <= shape[name] <= high):
            raise EdgesError(f"{where}: '{name}' is a number from {low} to {high}")
    if shape.get("pointer_at", 0) + 2 > shape.get("entry_size", 2):
        raise EdgesError(f"{where}: the address at 'pointer_at' does not fit in an entry of 'entry_size' bytes")
    if not all(isinstance(value, int) and 0 <= value <= 0xFFFF for value in shape.get("skip", ())):
        raise EdgesError(f"{where}: 'skip' is a list of addresses that mean no entry")
    if not all(isinstance(value, int) and 0 <= value < DECLARED_LIMIT for value in shape.get("unused", ())):
        raise EdgesError(f"{where}: 'unused' is a list of the numbers of entries the game never uses")
    bank = shape.get("target_bank", "same")
    if isinstance(bank, dict):
        table = bank.get("table")
        good = (set(bank) == {"at"} and isinstance(bank["at"], int) and
                0 <= bank["at"] < shape.get("entry_size", 2)) or \
               (set(bank) == {"table"} and isinstance(table, dict) and set(table) == {"bank", "address"} and
                all(isinstance(table[key], int) for key in table))
        if not good:
            raise EdgesError(f"{where}: 'target_bank' is \"same\", a bank, {{\"at\": a byte of the entry}} or "
                             f"{{\"table\": {{\"bank\": .., \"address\": ..}}}} with a byte for each entry")
    elif bank != "same" and not (isinstance(bank, int) and not isinstance(bank, bool) and bank >= 0):
        raise EdgesError(f"{where}: 'target_bank' is \"same\", a bank, {{\"at\": ..}} or {{\"table\": ..}}")
    if "leads_to" in shape:
        if not isinstance(shape["leads_to"], dict):
            raise EdgesError(f"{where}: 'leads_to' describes what each address points to")
        if set(shape["leads_to"]) & {"name", "bank", "address", "why"}:
            raise EdgesError(f"{where}: what 'leads_to' describes is wherever the address points, and has no "
                             f"name or address of its own")
        check_shape(shape["leads_to"], where + ", what it leads to")


def read_shape(rom: bytes, shape: dict, bank: int, address: int, where: str) -> list[tuple[int, int, int]]:
    """The functions a table of this shape at bank:address names, as (index,
    bank, address), following `leads_to`. The index is the entry's own."""
    banks = (len(rom) + 0x3FFF) // 0x4000
    size, at = shape.get("entry_size", 2), shape.get("pointer_at", 0)
    skip = set(shape.get("skip", ()))
    unused = set(shape.get("unused", ()))
    spec = shape.get("target_bank", "same")
    found: list[tuple[int, int, int]] = []
    count = shape.get("entries")
    for index in range(count if count is not None else DECLARED_LIMIT + 1):
        entry = address + index * size
        first = rom_byte(rom, bank, entry)
        if first is None or rom_byte(rom, bank, entry + size - 1) is None or \
                (entry < 0x4000) != (entry + size - 1 < 0x4000):
            raise EdgesError(f"{where}: entry {index} is not in the ROM (bank {bank:02X})")
        if count is None:
            if first == shape["ends_with_byte"]:
                break
            if index == DECLARED_LIMIT:
                raise EdgesError(f"{where}: no byte {shape['ends_with_byte']:02X} ends it within "
                                 f"{DECLARED_LIMIT} entries")
        target = rom_word(rom, bank, entry + at)
        if target in skip or index in unused:
            continue
        if target is None or not 0x0150 <= target < 0x8000:
            raise EdgesError(f"{where}: entry {index} is {0 if target is None else target:04X}, which is not an "
                             f"address in the ROM (give 'skip' the values that mean no entry, or 'unused' the "
                             f"numbers of entries the game never uses)")
        if isinstance(spec, dict) and "at" in spec:
            target_bank = rom_byte(rom, bank, entry + spec["at"])
        elif isinstance(spec, dict):
            target_bank = rom_byte(rom, spec["table"]["bank"], spec["table"]["address"] + index)
            if target_bank is None:
                raise EdgesError(f"{where}: the table of banks has no entry {index}")
        elif spec == "same":
            target_bank = bank
            if bank == 0 and target >= 0x4000:
                raise EdgesError(f"{where}: it is in the home bank and entry {index} points above it, at "
                                 f"{target:04X}: say which bank with 'target_bank'")
        else:
            target_bank = spec
        if target < 0x4000:
            target_bank = 0
        elif not 0 < target_bank < banks:
            raise EdgesError(f"{where}: entry {index} is in bank {target_bank:02X}, which the ROM does not have")
        if "leads_to" in shape:
            inner = f"{where}, entry {index}, at {target_bank:02X}:{target:04X}"
            found += [(index, there_bank, there) for _, there_bank, there in
                      read_shape(rom, shape["leads_to"], target_bank, target, inner)]
            continue
        if target_bank == bank and address <= target < address + (index + 1) * size:
            raise EdgesError(f"{where}: entry {index} points into the table itself, at {target:04X}")
        bad = no_instruction_at(rom, target_bank, target)
        if bad is not None:
            raise EdgesError(f"{where}: entry {index} points to {target_bank:02X}:{target:04X}, which is not code "
                             f"(the byte at {bad:04X} is no instruction)")
        found.append((index, target_bank, target))
    return found


def load_tables(paths: list, rom: bytes) -> list[dict]:
    """Tables of functions somebody has read the code for: where each is, how
    long, and where in an entry the address sits. Each comes back with the
    functions it names (`functions`: index, bank, address).

        {"schema": "gbrecomp.lift-tables", "version": 1, "tables": [
            {"name": "what an item does", "bank": 3, "address": 21583, "entries": 97,
             "why": "UseItem doubles the item's number and adds it to this"},
            {"name": "the script of each room", "bank": 0, "address": 430, "entries": 248,
             "target_bank": {"table": {"bank": 3, "address": 16957}},
             "leads_to": {"entries": 1, "entry_size": 9, "pointer_at": 7},
             "why": "the room's header, copied to RAM when it is entered"}]}

    An entry is `entry_size` bytes (2), with the address `pointer_at` bytes
    into it (0). The table has `entries` of them, or goes on until an entry
    that starts with `ends_with_byte`. What an address points to is in the
    table's own bank ("same"), in a bank given as a number, in the bank a
    byte of the entry names ({"at": n}), or in the bank a second table names,
    one byte an entry ({"table": ...}); below 0x4000 it is in the home bank
    whatever that says. `skip` lists addresses that mean no entry, and
    `unused` the numbers of entries the game never uses, whatever is in
    them. With `leads_to`, the address is not of a function but of another
    table, of the shape given there: one record, or a list."""
    import json

    banks = (len(rom) + 0x3FFF) // 0x4000
    tables: list[dict] = []
    seen: set[tuple[int, int]] = set()
    for path in paths:
        try:
            document = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise EdgesError(f"cannot read the tables in {path}: {error}") from error
        if not isinstance(document, dict) or document.get("schema") != TABLES_SCHEMA or document.get("version") != 1:
            raise EdgesError(f"{path} is not a version 1 {TABLES_SCHEMA} file")
        if not isinstance(document.get("tables"), list):
            raise EdgesError(f"{path}: 'tables' is a list")
        for number, table in enumerate(document["tables"]):
            if not isinstance(table, dict) or not all(isinstance(table.get(key), int) for key in ("bank", "address")):
                raise EdgesError(f"{path}: table {number} needs the 'bank' and 'address' it is at")
            bank, address = table["bank"], table["address"]
            name = table.get("name", f"the table at {bank:02X}:{address:04X}")
            if not isinstance(name, str) or not 0 < len(name) <= 80 or not name.isprintable():
                raise EdgesError(f"{path}: table {number}: 'name' is a line of text")
            where = f"{path}: {name}" + (f" ({bank:02X}:{address:04X})" if "name" in table else "")
            if not (0x0150 <= address < 0x8000 and 0 <= bank < banks and (address < 0x4000) == (bank == 0)):
                raise EdgesError(f"{where}: not in the ROM")
            if (bank, address) in seen:
                raise EdgesError(f"{where}: declared twice")
            seen.add((bank, address))
            check_shape(table, where)
            functions = read_shape(rom, table, bank, address, where)
            tables.append({"name": name, "bank": bank, "address": address, "functions": functions,
                           "where": name + (f" ({bank:02X}:{address:04X})" if "name" in table else ""),
                           # A plain list of addresses can also be one a helper is seen to call through.
                           "plain": bool(functions) and table.get("entry_size", 2) == 2 and
                           not ({"leads_to", "skip", "unused", "ends_with_byte"} & set(table)) and
                           not isinstance(table.get("target_bank"), dict),
                           "entries": table.get("entries")})
    return tables


def classify_targets(export: dict, edges: list[dict],
                     also: list[tuple[int, int]] = ()) -> dict[str, list[tuple[int, int]]]:
    """Each distinct target, by what the export has at that address now."""
    entries = {(function["bank"], function["address"]): function for function in export["functions"]}
    decoded = set()
    for function in export["functions"]:
        for block in function["blocks"]:
            for instruction in block["instructions"]:
                decoded.add((function["bank"], instruction["address"]))
    groups: dict[str, list[tuple[int, int]]] = {"function": [], "inside": [], "undecoded": []}
    for target in sorted({(edge["bank"], edge["address"]) for edge in edges} | set(also)):
        if target in entries:
            groups["function"].append(target)
        elif target in decoded:
            groups["inside"].append(target)
        else:
            groups["undecoded"].append(target)
    return groups


EVIDENCE_SCHEMA = "gbrt.evidence"


def load_evidence(paths: list[Path], rom: bytes) -> tuple[list[dict], set[tuple[int, int]]]:
    """The jumps recorded in some runs of the game, and every address a call
    or such a jump was seen to arrive at."""
    import json

    transfers: dict[tuple, dict] = {}
    entered: set[tuple[int, int]] = set()
    checksum = (rom[0x14E] << 8) | rom[0x14F] if len(rom) > 0x14F else 0
    for path in paths:
        try:
            document = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise EdgesError(f"cannot read evidence {path}: {error}") from error
        if document.get("schema") != EVIDENCE_SCHEMA or document.get("version") != 1:
            raise EdgesError(f"{path} is not a version 1 {EVIDENCE_SCHEMA} file")
        if document["rom"]["size"] != len(rom) or document["rom"]["global_checksum"] != checksum:
            raise EdgesError(f"{path} was recorded from another ROM")
        for transfer in document["transfers"]:
            source = transfer["from"] or {}
            key = (transfer["kind"], transfer["bank"], transfer["at"], transfer["target_bank"],
                   transfer["target"], transfer["calls"], transfer["returns_to"],
                   source.get("bank"), source.get("site"), source.get("entered_bank"), source.get("entered"))
            if key in transfers:
                transfers[key]["count"] += transfer["count"]
            else:
                transfers[key] = dict(transfer)
        entered.update((bank, address) for bank, address in document["entered"])
    return list(transfers.values()), entered


def jump_returns(export: dict) -> set[tuple[int, int]]:
    """Where a function's own `JP HL` comes back: the function goes on
    there, and its callee returns there."""
    places: set[tuple[int, int]] = set()
    for function in export["functions"]:
        for block in function["blocks"]:
            places.update((function["bank"] if address >= 0x4000 else 0, address)
                          for address in block.get("jump_returns_to", ()))
    return places


def evidence_edges(export: dict, transfers: list[dict],
                   found: list[dict]) -> tuple[list[dict], dict[str, int]]:
    """Edges for the jumps a run of the game made, beside those `found` by
    asking the ROM. Each is put on the CALL that entered the function it
    happened in, when the export has that call; otherwise on the jump itself."""
    banks = export["rom"]["rom_banks"]
    came_back = jump_returns(export)
    call_sites: dict[tuple[int, int], str] = {}
    instructions: dict[tuple[int, int], str] = {}
    for function in export["functions"]:
        for call in function["calls"]:
            call_sites[(function["bank"] if call["site"] >= 0x4000 else 0, call["site"])] = function["id"]
        for block in function["blocks"]:
            for instruction in block["instructions"]:
                where = (function["bank"] if instruction["address"] >= 0x4000 else 0, instruction["address"])
                instructions[where] = function["id"]
    known: dict[int, set[tuple[int, int]]] = {}
    tables: set[int] = set()
    for edge in found:
        known.setdefault(edge["site"], set()).add((edge["bank"], edge["address"]))
        if "table" in edge:
            tables.add(edge["site"])

    counts = {"seen": len(transfers), "agree": 0, "disagree": 0, "added": 0, "elsewhere": 0, "no site": 0,
              "came back": 0}
    edges: list[dict] = []
    emitted: set[tuple] = set()
    for transfer in sorted(transfers, key=lambda item: (item["bank"], item["at"], item["target_bank"],
                                                        item["target"])):
        bank, address = transfer["target_bank"], transfer["target"]
        if address >= 0x8000 or 0x0100 <= address < 0x0150 or (address >= 0x4000 and not 0 < bank < banks):
            counts["elsewhere"] += 1
            continue
        if transfer["kind"] == "return" and (bank, address) in came_back:
            # A RET to where a function's own JP HL comes back is the call
            # it made by hand returning. A recording that starts inside the
            # function, from a save state, did not see the push, and wrote
            # the RET down as one to an address no CALL pushed.
            counts["came back"] += 1
            continue
        # What the target returns to counts as that only if it is code.
        returns_to = transfer["returns_to"] if transfer["calls"] else 0
        source = transfer["from"]
        site_key = (source["bank"] if source["site"] >= 0x4000 else 0, source["site"]) if source else None
        if site_key in call_sites:
            edge = {
                "from": call_sites[site_key], "site": source["site"], "kind": "call",
                "helper": function_id(source["entered_bank"], source["entered"]),
                "bank": bank, "address": address, "returns_to": returns_to, "left_at": transfer["at"],
                "seen": True,
            }
        else:
            at_key = (transfer["bank"] if transfer["at"] >= 0x4000 else 0, transfer["at"])
            if at_key not in instructions:
                # The jump is in code the export does not have; where it
                # went is code all the same.
                counts["no site"] += 1
                edge = {"from": None, "site": transfer["at"], "kind": "computed", "bank": bank,
                        "address": address, "returns_to": returns_to, "left_at": transfer["at"], "seen": True}
            else:
                edge = {
                    "from": instructions[at_key], "site": transfer["at"], "kind": "computed",
                    "bank": bank, "address": address, "returns_to": returns_to, "left_at": transfer["at"],
                    "seen": True,
                }
        said = known.get(edge["site"]) if edge["kind"] == "call" else None
        if said is not None:
            if (bank, address) in said:
                counts["agree"] += 1
                continue
            if edge["site"] not in tables:
                # The ROM said this call goes to one place and the game went
                # to another: kept, and worth a look.
                counts["disagree"] += 1
        key = (edge["from"], edge["site"], edge["kind"], bank, address)
        if key in emitted:
            continue
        emitted.add(key)
        edges.append(edge)
        counts["added"] += 1
    return edges, counts


def edges_document(export: dict, edges: list[dict],
                   entered: set[tuple[int, int]] | None = None,
                   tables: list[dict] | None = None) -> dict:
    document = {
        "schema": EDGES_SCHEMA,
        "version": 1,
        "rom": {"sha256": export["rom"]["sha256"], "size": export["rom"]["size"]},
        "edges": edges,
    }
    if tables:
        # Tables somebody declared, with the functions each names. Nothing
        # here says who calls through them.
        document["declared_tables"] = [
            {"name": table["name"], "bank": table["bank"], "address": table["address"],
             "entries": [{"index": index, "bank": bank, "address": address}
                         for index, bank, address in table["functions"]]}
            for table in tables]
    if entered:
        # Every address a run of the game arrived at by a call or a jump the
        # code does not spell out: what is known to have run.
        document["entered"] = [[bank, address] for bank, address in sorted(entered)]
    return document


def reachable(export: dict) -> int:
    """How many functions the entry point and the interrupt vectors reach
    through the calls and jumps the export lists."""
    index = {function["id"]: function for function in export["functions"]}
    seen: set[str] = set()
    stack = [function["id"] for function in export["functions"] if function["kind"] in ("entry", "interrupt")]
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        function = index[current]
        for jump in function.get("seen_jumps", ()):
            for entry in jump["targets"]:
                if entry["function"] in index and entry["function"] not in seen:
                    stack.append(entry["function"])
        for edge in function["calls"] + function["jumps_out"]:
            targets = [edge["function"], (edge.get("reaches") or {}).get("function")]
            targets += [entry["function"] for entry in (edge.get("reaches_table") or {}).get("entries", ())]
            targets += [entry["function"] for entry in edge.get("reaches_seen", ())]
            for target in targets:
                if target and target in index and target not in seen:
                    stack.append(target)
    return len(seen)
