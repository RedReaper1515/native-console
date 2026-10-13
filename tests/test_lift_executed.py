#!/usr/bin/env python3
"""Which bytes of the ROM a game executed, end to end.

Makes a small ROM in which the analysis is told to decode two stretches of
data as functions: a table the game reads, whose bytes read as `ld a, 5` /
`ret`, and two bytes the game does nothing with. Beside them it has a
function that runs, one that never does, and code nothing names, reached
through a register. Then:

- the generated game, run with `--executed FILE`, has to end in the same
  state as without, alone and beside `--data-use`, whose record has to be
  the same with the map kept beside it; the map has to have exactly the
  bytes of executed instructions, first bytes and later bytes told apart,
  and nothing of the data or of the function that never ran;
- `gb_lift.py executed` has to sort the export's functions: the ones that
  ran, the table as read and never executed, the rest as neither, and
  without a record of what was read the table as neither too; it has to
  count the code the export does not have; a function whose first
  instruction did not run and a function whose bytes ran as other
  instructions have to be told from both; maps add up, and one of another
  ROM is refused.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from test_lift_runtime_e2e import GB_LIFT, NINTENDO_LOGO, assemble, bump, ldh_a, ldh_to, play, run

FRAMES = 60
VBLANK_FLAG, LOOPS, SUM, RAN, NEVER, HIDDEN = range(0x80, 0x86)
BEGAN, WITHIN = 1, 2

PROGRAM = [
    ("label", "Start"),
    [0xF3],                                  # di
    [0x31, 0xFF, 0xDF],                      # ld sp, $DFFF
    [0xAF], ldh_to(0x0F),                    # IF = 0
    [0x21, 0x80, 0xFF, 0x06, 0x20],          # ld hl, $FF80 / ld b, 32
    ("label", "Clear"),
    [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli], a / dec b / jr nz
    [0x21, 0x00, 0xC2, 0x36, 0x3C, 0x23, 0x36, 0xC9],   # code in RAM at $C200: inc a / ret
    [0x3E, 0x01], ldh_to(0xFF),              # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    [0xCD], ("abs", "WaitFrame"),
    [0xCD, 0x00, 0xC2],                      # call $C200
    [0xCD], ("abs", "ReadsTable"),
    [0xCD], ("abs", "Ran"),
    [0x21], ("abs", "Hidden"),               # code nothing names:
    [0xCD], ("abs", "JumpHL"),               # reached through a register
    ldh_a(NEVER), [0xB7],                    # or a
    [0xC4], ("abs", "NeverRan"),             # call nz: never
    bump(LOOPS),
    [0x18], ("rel", "MainLoop"),

    ("label", "VBlank"),
    [0xF5],                                  # push af
    [0x3E, 0x01], ldh_to(VBLANK_FLAG),
    [0xF1, 0xD9],                            # pop af / reti

    ("label", "WaitFrame"),
    [0xAF], ldh_to(VBLANK_FLAG),
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    ldh_a(VBLANK_FLAG), [0xB7, 0x28], ("rel", "WaitFrameLoop"),
    [0xC9],

    ("label", "ReadsTable"),
    [0x21], ("abs", "Table"),
    [0x06, 0x02, 0x0E, 0x00],                # ld b, 2 / ld c, 0
    ("label", "ReadsTableLoop"),
    [0x2A, 0x81, 0x4F, 0x05, 0x20], ("rel", "ReadsTableLoop"),   # ld a, [hli] / add c / ld c, a / dec b / jr nz
    [0x79], ldh_to(SUM),                     # ld a, c
    [0xC9],
    ("label", "Ran"),
    bump(RAN),
    [0xC9],
    ("label", "JumpHL"),
    [0xE9],                                  # jp hl
    ("label", "Hidden"),
    bump(HIDDEN),
    [0xC9],
    ("label", "NeverRan"),
    bump(NEVER),
    [0xC9],
    # Data. To a decoder: ld a, 5 / ret. The game reads the first two bytes.
    ("label", "Table"),
    [0x3E, 0x05, 0xC9],
    # And two bytes it does nothing with: xor a / ret.
    ("label", "Untouched"),
    [0xAF, 0xC9],
]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble(0x150, PROGRAM)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13E] = b"RAN GAME  "
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


def read_map(path: Path) -> tuple[bytes, int]:
    data = path.read_bytes()
    assert data[:8] == b"GBEXECUT" and struct.unpack_from("<I", data, 8)[0] == 1, data[:12]
    size, checksum, zero, instructions = struct.unpack_from("<IHHQ", data, 12)
    assert size == 0x8000 and zero == 0 and len(data) == 28 + size, (size, len(data))
    return data[28:], instructions


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()

    scratch = tempfile.TemporaryDirectory(prefix="gblift-executed-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom, labels = make_rom()
        rom_path = root / "ran_game.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        # The two stretches of data are given to the analysis as places code
        # starts, which is what a pointer scan does to a real game's data.
        analysis = ["--no-scan", "--pointer-scan", "off",
                    "--add-entry-point", f"0:{labels['Table']:04X}",
                    "--add-entry-point", f"0:{labels['Untouched']:04X}"]
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        export = json.loads(export_path.read_text(encoding="utf-8"))
        ids = {function["address"]: function["id"] for function in export["functions"]}
        for name in ("ReadsTable", "Ran", "NeverRan", "Table", "Untouched", "WaitFrame"):
            assert labels[name] in ids, f"the export has no function at {name}"
        assert labels["Hidden"] not in ids, "Hidden is reached through a register only"

        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "ran_game"])
        build = project / "build"
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(build),
             "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
        run(["ninja", "-C", str(build)], timeout=900)
        executable = next(path for path in (build / "ran_game.exe", build / "ran_game") if path.exists())

        # ---- Recording changes nothing -------------------------------------
        def played(name: str, *flags: str, frames: int = FRAMES) -> dict:
            return play(executable, root, name, *flags, frames=frames)[0]

        plain = played("plain")
        hram = plain["hram_ff80_ff90"]
        assert hram[LOOPS - 0x80] > 30 and hram[SUM - 0x80] == 0x3E + 0x05 and hram[NEVER - 0x80] == 0, hram
        assert hram[RAN - 0x80] == hram[HIDDEN - 0x80] > 30, hram
        map_path = root / "game.executed"
        recorded = played("recorded", "--executed", str(map_path))
        data_use = played("data-use", "--data-use", str(root / "use.json"))
        both = played("both", "--executed", str(root / "both.executed"), "--data-use", str(root / "use-both.json"))
        for name, other in (("--executed", recorded), ("--data-use", data_use), ("both", both)):
            differ = [key for key in plain if key != "dispatch_fallbacks" and plain[key] != other.get(key)]
            assert not differ, f"the game ends differently with {name}: {differ}"
        assert map_path.read_bytes() == (root / "both.executed").read_bytes(), \
            "the map differs with a second recorder beside"
        assert json.loads((root / "use.json").read_text(encoding="utf-8")) == \
            json.loads((root / "use-both.json").read_text(encoding="utf-8")), \
            "the record of what the data is used for differs with the map kept beside it"

        # ---- What the map holds ----------------------------------------------
        ran, instructions = read_map(map_path)
        assert instructions > 1000, instructions

        def flags(name: str, length: int) -> list[int]:
            return list(ran[labels[name]:labels[name] + length])

        # ldh a, [n] / inc a / ldh [n], a / ret: operands are not where an instruction begins.
        assert flags("Ran", 6) == [BEGAN, WITHIN, BEGAN, BEGAN, WITHIN, BEGAN], flags("Ran", 6)
        assert flags("Hidden", 6) == [BEGAN, WITHIN, BEGAN, BEGAN, WITHIN, BEGAN], flags("Hidden", 6)
        assert flags("MainLoop", 6) == [BEGAN, WITHIN, WITHIN, BEGAN, WITHIN, WITHIN], flags("MainLoop", 6)
        # The game also runs two instructions it wrote into RAM: they are no bytes of the ROM.
        assert not any(ran[labels["NeverRan"]:]), [hex(i) for i, flag in enumerate(ran) if flag and i >= labels["NeverRan"]]
        assert flags("NeverRan", 6) == [0] * 6 and flags("Table", 3) == [0] * 3 and flags("Untouched", 2) == [0, 0]
        assert list(ran[0x100:0x104]) == [BEGAN, BEGAN, WITHIN, WITHIN] and ran[0x40] == BEGAN
        assert not any(ran[0x104:0x150]), "the header was executed"
        # Every byte up to the function that never ran is code that ran, but
        # for the call to that function, which was not made.
        end = labels["NeverRan"]
        assert all(ran[0x150:end]), [hex(0x150 + i) for i, flag in enumerate(ran[0x150:end]) if not flag]

        # ---- Sorting the functions ---------------------------------------------
        def sort(name: str, *more: str) -> tuple[dict, str]:
            out = root / f"{name}.json"
            printed = run([sys.executable, str(GB_LIFT), "executed", str(export_path), *more,
                           "--json", str(out)]).stdout
            return json.loads(out.read_text(encoding="utf-8")), printed

        verdicts = root / "verdicts.json"
        verdicts.write_text(json.dumps({"results": [
            {"function": ids[labels["Ran"]], "verdict": "match"},
            {"function": ids[labels["Table"]], "verdict": "inconclusive"},
            {"function": ids[labels["Untouched"]], "verdict": "match"}]}), encoding="utf-8")
        sorted_, printed = sort("sorted", "--map", str(map_path), "--data-use", str(root / "use.json"),
                                "--verdicts", str(verdicts))
        classes = {entry["function"]: entry for entry in sorted_["functions"]}

        def of(name: str) -> dict:
            return classes[ids[labels[name]]]

        for name in ("ReadsTable", "Ran", "WaitFrame"):
            assert of(name)["class"] == "ran" and of(name)["bytes_run"] == of(name)["bytes"], of(name)
            assert of(name)["bytes_read"] == 0, of(name)
        assert classes["gbfn:v1:0000:0040"]["class"] == classes["gbfn:v1:0000:0100"]["class"] == "ran"
        # The table: decoded as two instructions of three bytes, of which the
        # game read two and executed none.
        table = of("Table")
        assert (table["class"], table["bytes"], table["instructions"]) == ("read", 3, 2), table
        assert (table["bytes_read"], table["bytes_run"], table["instructions_run"]) == (2, 0, 0), table
        assert of("Untouched")["class"] == of("NeverRan")["class"] == "neither", (of("Untouched"), of("NeverRan"))
        assert of("NeverRan")["bytes_read"] == 0 and of("NeverRan")["bytes_run"] == 0
        assert sorted_["classes"]["read"] == 1 and sum(sorted_["classes"].values()) == len(sorted_["functions"])
        # Hidden ran and the export has no instruction there: six bytes.
        assert sorted_["bytes"]["ran_not_decoded"] == 6, sorted_["bytes"]
        assert sorted_["bytes"]["decoded_read_only"] == 2 and sorted_["bytes"]["ran_and_read"] == 0, sorted_["bytes"]
        assert sorted_["by_verdict"]["inconclusive"]["read"] == 1, sorted_["by_verdict"]
        assert sorted_["by_verdict"]["match"] == {"ran": 1, "ran_inside": 0, "ran_otherwise": 0, "read": 0,
                                                 "neither": 1}, sorted_["by_verdict"]
        assert "6 executed bytes are in no instruction of the export" in printed, printed
        assert "none of it was executed, and the game read bytes of it as data" in printed, printed

        # Without a record of what was read, read is not told from neither.
        blind, printed = sort("blind", "--map", str(map_path))
        assert blind["classes"]["read"] == 0 and blind["classes"]["neither"] == sorted_["classes"]["neither"] + 1
        assert "cannot be told" in printed, printed

        # A function whose first instruction did not run, and one whose bytes
        # ran only as other instructions than the export decoded: maps made
        # by hand from the real one.
        head = map_path.read_bytes()[:28]
        inside = bytearray(ran)
        inside[labels["Ran"]] = 0
        (root / "inside.executed").write_bytes(head + inside)
        otherwise = bytearray(ran)
        otherwise[labels["NeverRan"] + 1] = BEGAN   # the operand of its first instruction, as an instruction
        (root / "otherwise.executed").write_bytes(head + otherwise)
        entered, _ = sort("inside", "--map", str(root / "inside.executed"))
        entry = next(entry for entry in entered["functions"] if entry["function"] == ids[labels["Ran"]])
        assert entry["class"] == "ran_inside" and entry["instructions_run"] == entry["instructions"] - 1, entry
        askew, _ = sort("otherwise", "--map", str(root / "otherwise.executed"))
        entry = next(entry for entry in askew["functions"] if entry["function"] == ids[labels["NeverRan"]])
        assert entry["class"] == "ran_otherwise" and entry["instructions_run"] == 0 and entry["bytes_run"] == 1, entry

        # Maps add up: a run of five frames has less, and with the long run
        # beside it the answer is the long run's.
        played("short", "--executed", str(root / "short.executed"), frames=1)
        short, _ = read_map(root / "short.executed")
        assert sum(1 for flag in short if flag) <= sum(1 for flag in ran if flag)
        for order in ((root / "short.executed", map_path), (map_path, root / "short.executed")):
            added, _ = sort("added", "--map", str(order[0]), "--map", str(order[1]),
                            "--data-use", str(root / "use.json"))
            assert added["classes"] == sorted_["classes"] and added["bytes"] == sorted_["bytes"], order

        # A map of another ROM is refused.
        other = bytearray(map_path.read_bytes())
        struct.pack_into("<I", other, 12, 0x4000)
        (root / "other.executed").write_bytes(bytes(other[:28 + 0x4000]))
        refused = subprocess.run([sys.executable, str(GB_LIFT), "executed", str(export_path), "--map",
                                  str(root / "other.executed")], capture_output=True, text=True, timeout=120)
        assert refused.returncode == 2 and "another ROM" in refused.stderr, (refused.returncode, refused.stderr)
    finally:
        if scratch:
            scratch.cleanup()
    print("executed bytes end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
