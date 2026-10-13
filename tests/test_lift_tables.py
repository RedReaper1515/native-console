#!/usr/bin/env python3
"""Tables of functions somebody has read the code for, end to end.

Run by test_lift_edges.py (the `lift_far_call_edges_end_to_end` test), or
alone with --gbrecomp.

Builds a four-bank MBC1 ROM whose functions in banks 2 and 3 are named only
by tables nothing in the code can be followed through: a table of pointers
to records, each in the bank a second table gives, with the function's
address five bytes in; a list of three-byte entries that ends with a byte;
lists of records that name their own bank; and a plain table a helper calls
through, with nothing after it to say where it ends. Then:

- the first export has to have none of those functions, and one of them
  inside another function, which runs into it;
- `gb_lift.py edges --unread` has to write the plain table out as a
  declaration to finish, and refuse it back unfinished;
- `gb_lift.py edges --tables` has to read every table, skip what is declared
  as no entry, and say how many of the places are functions already;
- exporting with `--lift-edges` and looking again has to settle, on an export
  where each is a function of the kind `declared_table` that says which
  entry of which table it is, and the plain table's are `table_call`
  functions on the call that goes through it;
- the mechanical lift of every function of that export has to match the ROM;
- a generated project has to compile them only when told about them;
- a declaration that reads past a table, points at what is no code, leaves
  the bank unsaid or is misspelt has to be refused, and so has an edges
  file whose declared table is not in the ROM.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_LIFT = ROOT / "tools" / "gb_lift.py"

MAIN = 0x0150
CALLED = 0x0180               # one instruction, then it runs into...
INSIDE = CALLED + 1           # ...a function only a record names
HEADERS = 0x0200              # three pointers to records
HEADER_BANKS = 0x0210         # and the bank of each
CALL_TABLE = 0x0240           # a helper: table in HL, number in A
TABLE_JUMP = CALL_TABLE + 13
PLAIN_0 = 0x0290
PLAIN_1 = 0x0292
PLAIN = 0x02A0                # two entries, then bytes that are no address
RECORD = 0x4100               # a record: a byte, a pointer to data, the script
SCRIPT = 0x4180
OTHER = 0x4190
ITEMS = 0x4200                # in bank 2: a byte and a handler, until a byte FF
HANDLER_0 = 0x4240
HANDLER_1 = 0x4250
HANDLER_2 = 0x4260
LISTS = 0x4200                # in bank 3: two pointers to lists of records
LIST_A = 0x4220
LIST_B = 0x4240
NO_CODE = 0x4300              # bytes that are no instruction
BAD_TABLE = 0x4310
RET = 0xC9


def low_high(address: int) -> list[int]:
    return [address & 0xFF, address >> 8]


def make_rom() -> tuple[bytearray, int]:
    rom = bytearray(4 * 0x4000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *low_high(MAIN)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13F] = b"LIFT TABLES"
    rom[0x147] = 0x01  # MBC1
    rom[0x148] = 0x01  # 64 KiB
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF

    def home(address: int, body: list[int]) -> None:
        rom[address:address + len(body)] = bytes(body)

    def banked(bank: int, address: int, body: list[int]) -> None:
        offset = bank * 0x4000 + (address - 0x4000)
        rom[offset:offset + len(body)] = bytes(body)

    code = [0xCD, *low_high(CALLED)]                     # call Called
    code += [0xF0, 0x91, 0x21, *low_high(PLAIN)]         # ldh a, [$FF91] / ld hl, Plain
    site = MAIN + len(code)
    code += [0xCD, *low_high(CALL_TABLE), RET]           # call CallTable / ret
    home(MAIN, code)
    home(CALLED, [0x04])                                 # inc b, and on into Inside
    home(INSIDE, [0x0C, RET])                            # inc c

    table_helper = [
        0xE5,                          # push hl
        0x87,                          # add a
        0x16, 0x00,                    # ld d, 0
        0x5F,                          # ld e, a
        0x19,                          # add hl, de
        0x2A, 0x66, 0x6F,              # ld a, [hli] / ld h, [hl] / ld l, a
        0x11, *low_high(TABLE_JUMP + 1),  # ld de, .return
        0xD5,                          # push de
    ]
    assert CALL_TABLE + len(table_helper) == TABLE_JUMP
    home(CALL_TABLE, table_helper + [0xE9, 0xE1, RET])   # jp hl / .return: pop hl / ret
    home(PLAIN_0, [0x04, RET])
    home(PLAIN_1, [0x05, RET])
    home(PLAIN, [*low_high(PLAIN_0), *low_high(PLAIN_1), 0xFF, 0xFF])

    # Records the game would copy to RAM and jump through from there.
    home(HEADERS, [*low_high(RECORD), *low_high(RECORD + 0x10), *low_high(RECORD)])
    home(HEADER_BANKS, [2, 2, 3])
    banked(2, RECORD, [7, 0x34, 0x12, *low_high(SCRIPT)])
    banked(2, RECORD + 0x10, [7, 0x34, 0x12, *low_high(INSIDE)])   # its script is in the home bank
    banked(3, RECORD, [7, 0x34, 0x12, *low_high(SCRIPT)])
    banked(2, SCRIPT, [0x14, RET])                       # inc d
    banked(3, SCRIPT, [0x15, RET])                       # dec d

    # A byte and a handler for each item; the second item has none.
    banked(2, ITEMS, [1, *low_high(HANDLER_0), 2, 0x00, 0x00, 3, *low_high(HANDLER_1), 0xFF])
    banked(2, HANDLER_0, [0x1C, RET])                    # inc e
    banked(2, HANDLER_1, [0x1D, RET])                    # dec e
    banked(2, HANDLER_2, [0x24, RET])                    # inc h

    # Lists of records that say which bank their handler is in.
    banked(3, LISTS, [*low_high(LIST_A), *low_high(LIST_B)])
    banked(3, LIST_A, [9, 2, *low_high(HANDLER_2), 9, 3, *low_high(OTHER), 0xFF])
    banked(3, LIST_B, [9, 2, *low_high(HANDLER_0), 0xFF])
    banked(3, OTHER, [0x2C, RET])                        # inc l

    banked(2, NO_CODE, [0xD3, 0xD3])
    banked(2, BAD_TABLE, [*low_high(NO_CODE)])
    return rom, site


def tables_document(*tables: dict) -> dict:
    return {"schema": "gbrecomp.lift-tables", "version": 1, "tables": list(tables)}


ROOMS = {"name": "room scripts", "bank": 0, "address": HEADERS, "entries": 3,
         "target_bank": {"table": {"bank": 0, "address": HEADER_BANKS}},
         "leads_to": {"entries": 1, "entry_size": 5, "pointer_at": 3},
         "why": "the room's record is copied to RAM, and its last two bytes jumped to from there"}
ITEM_HANDLERS = {"name": "item handlers", "bank": 2, "address": ITEMS, "ends_with_byte": 255,
                 "entry_size": 3, "pointer_at": 1, "skip": [0]}
EVENTS = {"name": "event lists", "bank": 3, "address": LISTS, "entries": 2,
          "leads_to": {"ends_with_byte": 255, "entry_size": 4, "pointer_at": 2, "target_bank": {"at": 1}}}
PLAIN_TABLE = {"bank": 0, "address": PLAIN, "entries": 2}

DECLARED = {
    (2, SCRIPT): ("room scripts", 0, HEADERS, 0),
    (0, INSIDE): ("room scripts", 0, HEADERS, 1),
    (3, SCRIPT): ("room scripts", 0, HEADERS, 2),
    (2, HANDLER_0): ("item handlers", 2, ITEMS, 0),   # the event lists name it too; the first table to does
    (2, HANDLER_1): ("item handlers", 2, ITEMS, 2),
    (2, HANDLER_2): ("event lists", 3, LISTS, 0),
    (3, OTHER): ("event lists", 3, LISTS, 0),
}


def run(command: list[str], expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout}\n{result.stderr}")
    return result


def gb_lift(*arguments: str, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_LIFT), *arguments], expect)


def function_id(bank: int, address: int) -> str:
    return f"gbfn:v1:{bank:04x}:{address:04x}"


def check(gbrecomp: Path) -> None:
    analysis = ["--no-scan", "--pointer-scan", "off"]
    with tempfile.TemporaryDirectory(prefix="gblift-tables-") as scratch:
        root = Path(scratch)
        rom, site = make_rom()
        rom_path = root / "tables.gb"
        rom_path.write_bytes(rom)
        tables_path = root / "tables.json"
        tables_path.write_text(json.dumps(tables_document(ROOMS, ITEM_HANDLERS, EVENTS, PLAIN_TABLE)),
                               encoding="utf-8")

        # Without the tables: none of what they name is a function.
        export_path = root / "export-0.json"
        run([str(gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        first = json.loads(export_path.read_text(encoding="utf-8"))
        by_id = {function["id"]: function for function in first["functions"]}
        for bank, address in list(DECLARED) + [(0, PLAIN_0), (0, PLAIN_1)]:
            assert function_id(bank, address) not in by_id, f"{bank:02X}:{address:04X} was found without its table"
        inside = by_id[function_id(0, CALLED)]
        assert INSIDE in [i["address"] for block in inside["blocks"] for i in block["instructions"]], \
            "the function a record names is not inside the one that runs into it"
        unread_path = root / "unread.json"
        alone = gb_lift("edges", str(export_path), "--rom", str(rom_path), "--unread", str(unread_path),
                        "--work", str(root / "work-edges"))
        assert "1 go through a table that does not end where code is known to begin" in alone.stdout, alone.stdout
        assert "somebody read" not in alone.stdout, alone.stdout
        # The table it could not read, as a declaration to finish: where it is, and who calls through it.
        unread = json.loads(unread_path.read_text(encoding="utf-8"))
        assert unread["schema"] == "gbrecomp.lift-tables" and len(unread["tables"]) == 1, unread
        assert {key: unread["tables"][0][key] for key in ("bank", "address", "entries")} == \
            {"bank": 0, "address": PLAIN, "entries": 0}, unread
        assert f"at {site:04X}" in unread["tables"][0]["why"], unread
        # Handed back as it is, it is not a declaration yet.
        unfinished = gb_lift("edges", str(export_path), "--rom", str(rom_path), "--tables", str(unread_path),
                             "--work", str(root / "work-edges"), expect=(2,))
        assert "'entries' is a number from 1 to" in unfinished.stderr, unfinished.stderr
        # Finished, it is the plain table's declaration.
        unread["tables"][0]["entries"] = 2
        assert {key: unread["tables"][0][key] for key in PLAIN_TABLE} == PLAIN_TABLE

        # With them: read, handed to the next export, and looked at again until nothing is new.
        edges_path = root / "edges.json"
        for round_number in range(1, 6):
            found = gb_lift("edges", str(export_path), "--rom", str(rom_path), "--tables", str(tables_path),
                            "-o", str(edges_path), "--work", str(root / "work-edges"))
            if round_number == 1:
                assert "not in this export" in found.stdout, found.stdout
                assert "4 tables somebody read the code for name 7 places (1 of them a call above goes through)" \
                    in found.stdout, found.stdout
                assert "room scripts (00:0200): 3 places, 0 of them functions in this export" in found.stdout, \
                    found.stdout
                assert "is as long as somebody says: 1 tables, 2 entries" in found.stdout, found.stdout
            if "this export has every one of them" in found.stdout:
                break
            export_path = root / f"export-{round_number}.json"
            loaded = run([str(gbrecomp), str(rom_path), *analysis, "--lift-edges", str(edges_path),
                          "--lift-export", str(export_path)])
            assert "8 of them entries of declared tables" in loaded.stdout, loaded.stdout
        else:
            raise AssertionError("looking with the tables did not settle\n" + found.stdout)
        assert "room scripts (00:0200): 3 places, 3 of them functions in this export" in found.stdout, found.stdout

        document = json.loads(edges_path.read_text(encoding="utf-8"))
        declared = {table["name"]: table for table in document["declared_tables"]}
        assert sorted(declared) == ["event lists", "item handlers", "room scripts"], sorted(declared)
        assert declared["room scripts"]["entries"] == [
            {"index": 0, "bank": 2, "address": SCRIPT}, {"index": 1, "bank": 0, "address": INSIDE},
            {"index": 2, "bank": 3, "address": SCRIPT}], declared["room scripts"]
        # The item with no handler is skipped, and keeps its number.
        assert declared["item handlers"]["entries"] == [
            {"index": 0, "bank": 2, "address": HANDLER_0}, {"index": 2, "bank": 2, "address": HANDLER_1}]
        # Each record of a list says its bank; the index is the list's.
        assert declared["event lists"]["entries"] == [
            {"index": 0, "bank": 2, "address": HANDLER_2}, {"index": 0, "bank": 3, "address": OTHER},
            {"index": 1, "bank": 2, "address": HANDLER_0}], declared["event lists"]
        # The plain table is on the call that goes through it.
        through = {(edge["site"], edge["table"]["address"], edge["table"]["index"]): (edge["bank"], edge["address"])
                   for edge in document["edges"] if "table" in edge}
        assert through == {(site, PLAIN, 0): (0, PLAIN_0), (site, PLAIN, 1): (0, PLAIN_1)}, through

        final = json.loads(export_path.read_text(encoding="utf-8"))
        by_id = {function["id"]: function for function in final["functions"]}
        for (bank, address), (name, table_bank, table, index) in DECLARED.items():
            function = by_id[function_id(bank, address)]
            assert function["kind"] == "declared_table", (function["id"], function["kind"])
            assert "declared_table" in function["entered_by"], function["entered_by"]
            assert function["declared_table"] == {"name": name, "bank": table_bank, "address": table,
                                                  "index": index}, function["declared_table"]
        for address in (PLAIN_0, PLAIN_1):
            function = by_id[function_id(0, address)]
            assert function["kind"] == "table_call" and "declared_table" not in function, function["kind"]
        # What ran into the function a record names now stops where it begins.
        called = by_id[function_id(0, CALLED)]
        assert [i["address"] for block in called["blocks"] for i in block["instructions"]] == [CALLED], called
        dossier = gb_lift("function", str(export_path), f"02:{SCRIPT:04x}").stdout
        assert "found as: declared_table" in dossier and \
            "entry 0 of room scripts, a table at 00:0200 somebody read the code for" in dossier, dossier

        # The functions found this way lift and match like any other.
        lifted = root / "lifted"
        gb_lift("baseline", str(export_path), "--all", "-o", str(lifted))
        report_path = root / "report.json"
        gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(lifted),
                "--trials", "200", "--work", str(root / "work-check"), "--json", str(report_path))
        report = {entry["function"]: entry for entry in
                  json.loads(report_path.read_text(encoding="utf-8"))["results"]}
        for bank, address in list(DECLARED) + [(0, PLAIN_0), (0, PLAIN_1), (0, CALLED)]:
            assert report[function_id(bank, address)]["verdict"] == "match", report[function_id(bank, address)]
        assert not [entry for entry in report.values() if entry["verdict"] == "mismatch"]

        # The generated game compiles them only when it is told about them.
        for name, extra, expected in (("plain", [], False), ("edges", ["--lift-edges", str(edges_path)], True)):
            project = root / f"project-{name}"
            run([str(gbrecomp), str(rom_path), *analysis, *extra, "-o", str(project), "--output-prefix", "game"])
            generated = "".join(path.read_text(encoding="utf-8", errors="replace") for path in project.glob("*.c"))
            assert (f"func_02_{SCRIPT:04x}" in generated) == expected, (name, expected)
            assert (f"func_03_{OTHER:04x}" in generated) == expected, (name, expected)

        # Declarations that cannot be right are refused, with the entry they go wrong at.
        def refused(table: dict, words: str) -> None:
            bad = root / "bad.json"
            bad.write_text(json.dumps(tables_document(table)), encoding="utf-8")
            result = gb_lift("edges", str(export_path), "--rom", str(rom_path), "--tables", str(bad),
                             "--work", str(root / "work-edges"), expect=(2,))
            assert words in result.stderr, (words, result.stderr)

        refused({**ITEM_HANDLERS, "skip": []}, "entry 1 is 0000, which is not an address in the ROM")
        refused({**PLAIN_TABLE, "entries": 3}, "entry 2 is FFFF, which is not an address in the ROM")
        refused({key: value for key, value in ROOMS.items() if key != "target_bank"},
                "say which bank with 'target_bank'")
        refused({"bank": 2, "address": BAD_TABLE, "entries": 1}, "which is not code (the byte at 4300")
        refused({"bank": 2, "address": ITEMS, "entry_size": 3, "pointer_at": 1}, "say how long it is")
        refused({**ITEM_HANDLERS, "entrys": 3}, "no such field as 'entrys'")
        refused({**ITEM_HANDLERS, "pointer_at": 2}, "does not fit in an entry")
        refused({**PLAIN_TABLE, "address": 0x8000}, "not in the ROM")
        refused({**EVENTS, "leads_to": {**EVENTS["leads_to"], "target_bank": {"at": 0}}},
                "is in bank 09, which the ROM does not have")
        not_tables = root / "not-tables.json"
        not_tables.write_text(json.dumps({"schema": "gbrecomp.lift-edges", "version": 1}), encoding="utf-8")
        result = gb_lift("edges", str(export_path), "--rom", str(rom_path), "--tables", str(not_tables),
                         "--work", str(root / "work-edges"), expect=(2,))
        assert "is not a version 1 gbrecomp.lift-tables file" in result.stderr, result.stderr

        # And the recompiler takes no declared table from outside the ROM.
        document["declared_tables"][0]["entries"][0]["address"] = 0x9000
        broken = root / "broken-edges.json"
        broken.write_text(json.dumps(document), encoding="utf-8")
        result = run([str(gbrecomp), str(rom_path), *analysis, "--lift-edges", str(broken),
                      "--lift-export", str(root / "broken.json")], expect=(1,))
        assert "is not a function in the ROM" in result.stdout + result.stderr, result.stderr

    print("lift declared tables end to end: ok")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    check(parser.parse_args().gbrecomp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
