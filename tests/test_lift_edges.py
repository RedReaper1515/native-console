#!/usr/bin/env python3
"""Calls made through helpers that take their target in registers, end to end.

Builds a four-bank MBC1 ROM whose code in banks 2 and 3 is only ever reached
the way hand-written games reach it. One helper in the home bank takes the
bank in B and the address in HL, switches bank and jumps there. Another takes
a table in HL and a number in A and jumps to that entry of the table. Then:

- the first export has to record the constants at each call to a helper,
  including one whose loads are in an earlier block;
- `gb_lift.py edges` has to find where each far call goes by running the
  helper, leave out the call whose bank is not a constant, and attribute a
  wrapper's far call to the wrapper and not to whoever calls the wrapper;
- it has to read a table of functions that ends at the first function it
  points to, and one that ends at a function something calls, and leave
  alone one with nothing to say where it ends;
- exporting with `--lift-edges` and looking again has to settle, and the
  export it settles on has to have every target as a function of its own,
  of the kind `far_call` or `table_call`, with the calls that reach it;
- the mechanical lift of every function of that export has to match the ROM;
- a project generated with `--lift-edges` has to compile the far-called code;
- edges found in another ROM have to be refused.

Then, with a ROM of its own, the tables no helper is seen to call through,
which somebody who has read the code declares (test_lift_tables.py).
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO
import test_lift_tables

ROOT = Path(__file__).resolve().parent.parent
GB_LIFT = ROOT / "tools" / "gb_lift.py"

FAR_CALL = 0x0200
FAR_JUMP = FAR_CALL + 13     # its JP HL
FAR_RETURN = FAR_JUMP + 1   # where it tells the target to return
WRAPPER = 0x0220
CALL_TABLE = 0x0240
TABLE_JUMP = CALL_TABLE + 13
TABLE_RETURN = TABLE_JUMP + 1
TABLE_A = 0x0280             # two entries, then the first of them
FUNC_A0 = TABLE_A + 4
FUNC_A1 = FUNC_A0 + 2
TABLE_C = 0x02A0             # entries, then bytes that are no address
TABLE_B = 0x4040             # in bank 2: three entries, then a function that is called
CALLED_B = TABLE_B + 6
FUNC_B0 = CALLED_B + 2
FUNC_B2 = FUNC_B0 + 2
USES_TABLE_B = 0x4050
MAIN = 0x0150
RET = 0xC9


def low_high(address: int) -> list[int]:
    return [address & 0xFF, address >> 8]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    rom = bytearray(4 * 0x4000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *low_high(MAIN)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"LIFT FARS"
    rom[0x147] = 0x01  # MBC1
    rom[0x148] = 0x01  # 64 KiB
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF

    def home(address: int, body: list[int]) -> None:
        rom[address:address + len(body)] = bytes(body)

    def banked(bank: int, address: int, body: list[int]) -> None:
        offset = bank * 0x4000 + (address - 0x4000)
        rom[offset:offset + len(body)] = bytes(body)

    # The far-call helper: bank in B, address in HL. It keeps the bank it
    # was called under in high RAM and maps it back afterwards.
    helper = [
        0xF0, 0x90,                    # ldh a, [$FF90]
        0xF5,                          # push af
        0x78,                          # ld a, b
        0xE0, 0x90,                    # ldh [$FF90], a
        0xEA, 0x00, 0x20,              # ld [$2000], a
        0x11, *low_high(FAR_RETURN),   # ld de, .return
        0xD5,                          # push de
    ]
    assert FAR_CALL + len(helper) == FAR_JUMP
    helper += [0xE9]                                     # jp hl
    helper += [0xF1, 0xE0, 0x90, 0xEA, 0x00, 0x20, RET]  # .return: pop af / restore / ret
    home(FAR_CALL, helper)

    # The table helper: table in HL, number in A.
    table_helper = [
        0xE5,                          # push hl
        0x87,                          # add a
        0x16, 0x00,                    # ld d, 0
        0x5F,                          # ld e, a
        0x19,                          # add hl, de
        0x2A, 0x66, 0x6F,              # ld a, [hli] / ld h, [hl] / ld l, a
        0x11, *low_high(TABLE_RETURN), # ld de, .return
        0xD5,                          # push de
    ]
    assert CALL_TABLE + len(table_helper) == TABLE_JUMP
    table_helper += [0xE9, 0xE1, RET]  # jp hl / .return: pop hl / ret
    home(CALL_TABLE, table_helper)

    sites: dict[str, int] = {}

    # A function that makes one far call of its own, whatever it is handed.
    wrapper = [0x06, 0x02, 0x21, 0x30, 0x40]             # ld b, 2 / ld hl, $4030
    sites["wrapper"] = WRAPPER + len(wrapper)
    wrapper += [0xC3, *low_high(FAR_CALL)]               # jp FarCall
    home(WRAPPER, wrapper)

    code: list[int] = []

    def here() -> int:
        return MAIN + len(code)

    code += [0x06, 0x02, 0x21, 0x00, 0x40]               # ld b, 2 / ld hl, $4000
    sites["plain"] = here()
    code += [0xCD, *low_high(FAR_CALL)]
    # The loads, then a branch that ends the block, then the call.
    code += [0x06, 0x02, 0x21, 0x20, 0x40, 0xA7, 0x28, 0x00]  # ... / and a / jr z, +0
    sites["later_block"] = here()
    code += [0xCD, *low_high(FAR_CALL)]
    # The bank is whatever is in A: not a constant.
    code += [0x47, 0x21, 0x00, 0x40]                     # ld b, a / ld hl, $4000
    sites["unknown_bank"] = here()
    code += [0xCD, *low_high(FAR_CALL)]
    # A call to the wrapper, with a constant in A that has nothing to do with it.
    code += [0x3E, 0x01]                                 # ld a, 1
    sites["calls_wrapper"] = here()
    code += [0xCD, *low_high(WRAPPER)]
    # Through the table helper: the number comes out of memory.
    code += [0xF0, 0x91, 0x21, *low_high(TABLE_A)]       # ldh a, [$FF91] / ld hl, TableA
    sites["table_a"] = here()
    code += [0xCD, *low_high(CALL_TABLE)]
    code += [0xF0, 0x91, 0x21, *low_high(TABLE_C)]       # the same with a table that has no end
    sites["table_c"] = here()
    code += [0xCD, *low_high(CALL_TABLE)]
    code += [0x06, 0x02, 0x21, *low_high(USES_TABLE_B)]  # a far call to the code that uses table B
    sites["far_to_b"] = here()
    code += [0xCD, *low_high(FAR_CALL)]
    # A far jump: the target returns to whoever called this.
    code += [0x06, 0x03, 0x21, 0x10, 0x40]               # ld b, 3 / ld hl, $4010
    sites["jump"] = here()
    code += [0xC3, *low_high(FAR_CALL)]
    assert here() <= FAR_CALL
    home(MAIN, code)

    # Table A ends where its first entry begins.
    home(TABLE_A, [*low_high(FUNC_A0), *low_high(FUNC_A1)])
    home(FUNC_A0, [0x04, RET])                           # inc b
    home(FUNC_A1, [0x05, RET])                           # dec b
    # Table C points back at those and is followed by nothing that says it is over.
    home(TABLE_C, [*low_high(FUNC_A0), *low_high(FUNC_A1), 0xFF, 0xFF])

    banked(1, 0x4000, [0x15, RET])                       # the same address in another bank: dec d
    banked(2, 0x4000, [0x1C, RET])                       # inc e
    banked(2, 0x4020, [0x14, RET])                       # inc d
    banked(2, 0x4030, [0x0C, RET])                       # inc c
    banked(3, 0x4010, [0x1D, RET])                       # dec e
    # Table B is followed by a function that is not one of its entries but
    # is called, and that is where it ends.
    banked(2, TABLE_B, [*low_high(FUNC_B0), *low_high(FUNC_A0), *low_high(FUNC_B2)])
    banked(2, CALLED_B, [0x0C, RET])
    banked(2, FUNC_B0, [0x14, RET])
    banked(2, FUNC_B2, [0x15, RET])
    uses = [0xCD, *low_high(CALLED_B), 0xF0, 0x91, 0x21, *low_high(TABLE_B)]
    sites["table_b"] = USES_TABLE_B + len(uses)
    uses += [0xC3, *low_high(CALL_TABLE)]                # jp CallTable
    banked(2, USES_TABLE_B, uses)
    return rom, sites


def run(command: list[str], expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout}\n{result.stderr}")
    return result


def gb_lift(*arguments: str) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_LIFT), *arguments])


def function_id(bank: int, address: int) -> str:
    return f"gbfn:v1:{bank:04x}:{address:04x}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]
    far_targets = {(2, 0x4000), (2, 0x4020), (2, 0x4030), (3, 0x4010), (2, USES_TABLE_B)}
    table_targets = {(0, FUNC_A0), (0, FUNC_A1), (2, FUNC_B0), (2, FUNC_B2)}

    with tempfile.TemporaryDirectory(prefix="gblift-edges-") as scratch:
        root = Path(scratch)
        rom, sites = make_rom()
        rom_path = root / "far.gb"
        rom_path.write_bytes(rom)

        # Without the edges: the constants are recorded, the targets unknown.
        export_path = root / "export-0.json"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        first = json.loads(export_path.read_text(encoding="utf-8"))
        by_id = {function["id"]: function for function in first["functions"]}
        # The analyzer follows the constants of the first call it meets into
        # the helper and so finds that one target by itself; the others it
        # cannot, because by then the helper has been reached with two
        # different sets of constants.
        unseen = {target for target in far_targets if function_id(*target) not in by_id}
        assert len(unseen) >= 4, sorted(unseen)
        known = {}
        for function in first["functions"]:
            for site in function["calls"] + function["jumps_out"]:
                if "known" in site:
                    known[site["site"]] = site["known"]
        assert known[sites["plain"]] == {"b": 2, "h": 0x40, "l": 0x00}, known
        assert known[sites["later_block"]] == {"b": 2, "h": 0x40, "l": 0x20}, known
        assert known[sites["unknown_bank"]] == {"h": 0x40, "l": 0x00}, known
        assert known[sites["table_a"]] == {"h": TABLE_A >> 8, "l": TABLE_A & 0xFF}, known
        assert known[sites["jump"]] == {"b": 3, "h": 0x40, "l": 0x10}, known
        assert by_id[function_id(0, FAR_CALL)]["summary_incomplete"]

        # Look, export with what was found, look again, until nothing is new.
        # The code that uses table B is only decoded once the far call to it
        # has been found, so its table takes a second look.
        edges_path = root / "edges.json"
        for round_number in range(1, 6):
            found = gb_lift("edges", str(export_path), "--rom", str(rom_path), "-o", str(edges_path),
                            "--work", str(root / "work-edges"))
            if round_number == 1:
                assert "not in this export" in found.stdout, found.stdout
                first_edges = json.loads(edges_path.read_text(encoding="utf-8"))["edges"]
            if "this export has every one of them" in found.stdout:
                break
            export_path = root / f"export-{round_number}.json"
            loaded = run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-edges", str(edges_path),
                          "--lift-export", str(export_path)])
            assert "calls made through helpers" in loaded.stdout, loaded.stdout
        else:
            raise AssertionError("looking for calls through helpers did not settle\n" + found.stdout)
        assert round_number >= 3, "table B's caller was decoded before the far call to it was found"

        # The first look: the far calls, and the table main itself uses.
        got = {(edge["site"], edge["kind"], edge["bank"], edge["address"]) for edge in first_edges
               if "table" not in edge}
        assert got == {
            (sites["plain"], "call", 2, 0x4000),
            (sites["later_block"], "call", 2, 0x4020),
            (sites["far_to_b"], "call", 2, USES_TABLE_B),
            (sites["jump"], "jump", 3, 0x4010),
            (sites["wrapper"], "jump", 2, 0x4030),
        }, sorted(got)
        for edge in first_edges:
            if "table" not in edge:
                assert edge["helper"] == function_id(0, FAR_CALL), edge
                # The helper pushes where its target returns: its own code.
                assert edge["returns_to"] == FAR_RETURN and edge["left_at"] == FAR_JUMP, edge

        document = json.loads(edges_path.read_text(encoding="utf-8"))
        assert document["schema"] == "gbrecomp.lift-edges" and document["version"] == 1
        tables = {}
        for edge in document["edges"]:
            if "table" in edge:
                assert edge["helper"] == function_id(0, CALL_TABLE), edge
                assert edge["returns_to"] == TABLE_RETURN and edge["left_at"] == TABLE_JUMP, edge
                key = (edge["site"], edge["kind"], edge["table"]["bank"], edge["table"]["address"])
                tables.setdefault(key, {})[edge["table"]["index"]] = (edge["bank"], edge["address"])
        assert tables == {
            (sites["table_a"], "call", 0, TABLE_A): {0: (0, FUNC_A0), 1: (0, FUNC_A1)},
            (sites["table_b"], "jump", 2, TABLE_B): {0: (2, FUNC_B0), 1: (0, FUNC_A0), 2: (2, FUNC_B2)},
        }, tables
        assert "1 go through a table that does not end where code is known to begin" in found.stdout, \
            found.stdout

        # The export it settled on: every target is a function, and says who reaches it.
        final = json.loads(export_path.read_text(encoding="utf-8"))
        by_id = {function["id"]: function for function in final["functions"]}
        for bank, address in far_targets:
            function = by_id[function_id(bank, address)]
            assert function["kind"] == "far_call", (function["id"], function["kind"])
            assert function["entered_by"] == ["far_call"], function["entered_by"]
            assert function["callers"], function["id"]
        for bank, address in table_targets:
            function = by_id[function_id(bank, address)]
            assert function["kind"] == "table_call", (function["id"], function["kind"])
            # Its address is a word in the ROM as well, which the export notes
            # of any function: here that word is the table.
            assert function["entered_by"] == ["data_pointer", "table_call"], function["entered_by"]
            assert function["callers"], function["id"]
        # The function after table B is called, which is why the table ends there.
        assert by_id[function_id(2, CALLED_B)]["kind"] == "call"
        # The same address in the bank the helper was not told to map is not
        # what the call reaches.
        assert by_id.get(function_id(1, 0x4000), {}).get("kind") != "far_call"
        assert by_id[function_id(2, 0x4030)]["callers"] == [function_id(0, WRAPPER)]
        assert function_id(2, USES_TABLE_B) in by_id[function_id(2, FUNC_B2)]["callers"]
        reaches = {}
        reaches_table = {}
        for function in final["functions"]:
            for site in function["calls"] + function["jumps_out"]:
                if "reaches" in site:
                    reaches[site["site"]] = site["reaches"]
                if "reaches_table" in site:
                    reaches_table[site["site"]] = site["reaches_table"]
        assert reaches[sites["plain"]] == {"bank": 2, "address": 0x4000,
                                           "function": function_id(2, 0x4000)}, reaches
        assert sites["unknown_bank"] not in reaches and sites["calls_wrapper"] not in reaches, reaches
        assert reaches_table[sites["table_b"]] == {
            "bank": 2, "address": TABLE_B,
            "entries": [{"bank": 2, "address": FUNC_B0, "function": function_id(2, FUNC_B0)},
                        {"bank": 0, "address": FUNC_A0, "function": function_id(0, FUNC_A0)},
                        {"bank": 2, "address": FUNC_B2, "function": function_id(2, FUNC_B2)}]}, reaches_table
        assert sites["table_c"] not in reaches_table, reaches_table
        assert "9 different places in all" in found.stdout, found.stdout
        assert "9 are functions in this export, 0 are inside another function, 0 were not decoded" \
            in found.stdout, found.stdout
        assert "found as: far_call" in gb_lift("function", str(export_path), "02:4000").stdout
        dossier = gb_lift("function", str(export_path), f"02:{USES_TABLE_B:04x}").stdout
        assert f"one of the 3 functions in the table at 02:{TABLE_B:04X}" in dossier, dossier

        # The functions found this way lift and match like any other.
        lifted = root / "lifted"
        gb_lift("baseline", str(export_path), "--all", "-o", str(lifted))
        report_path = root / "report.json"
        gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(lifted),
                "--trials", "200", "--work", str(root / "work-check"), "--json", str(report_path))
        report = {entry["function"]: entry for entry in
                  json.loads(report_path.read_text(encoding="utf-8"))["results"]}
        for bank, address in far_targets | table_targets:
            assert report[function_id(bank, address)]["verdict"] == "match", report[function_id(bank, address)]
        assert not [entry for entry in report.values() if entry["verdict"] == "mismatch"]
        structured = root / "structured"
        gb_lift("baseline", str(export_path), "--all", "--style", "structured", "-o", str(structured))
        text = "".join(path.read_text(encoding="utf-8") for path in structured.glob("*.c"))
        assert "on to func_02_4000 */" in text, "the far call is not named where it is made"
        assert f"on to one of the 3 in the table at 0x{TABLE_B:04X} */" in text, "nor the table"

        # The generated game compiles them only when it is told about them.
        for name, extra, expected in (("plain", [], False), ("edges", ["--lift-edges", str(edges_path)], True)):
            project = root / f"project-{name}"
            run([str(args.gbrecomp), str(rom_path), *analysis, *extra, "-o", str(project),
                 "--output-prefix", "game"])
            generated = "".join(path.read_text(encoding="utf-8", errors="replace")
                                for path in project.glob("*.c"))
            assert ("func_02_4020" in generated) == expected, (name, expected)
            assert (f"func_02_{FUNC_B2:04x}" in generated) == expected, (name, expected)

        # Edges are for the ROM they were found in.
        other = bytearray(rom)
        other[0x3000] ^= 0xFF
        other_path = root / "other.gb"
        other_path.write_bytes(other)
        refused = run([str(args.gbrecomp), str(other_path), *analysis, "--lift-edges", str(edges_path),
                       "--lift-export", str(root / "other.json")], expect=(1,))
        assert "another ROM" in refused.stdout + refused.stderr, refused.stderr

    print("lift edges end to end: ok")
    test_lift_tables.check(args.gbrecomp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
