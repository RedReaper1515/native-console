#!/usr/bin/env python3
"""Data the source names, end to end: an instruction's operand that leads to
something in the ROM goes by a name in the lifted source, and what it leads
to can then be moved.

A thing an operand leads to could not move: the operand is code, and no
image may change it. Lifted source can have the operand by name, and a name
is turned where the source is compiled. This makes a small game with five
tables, each read through `ld hl, table`, and holds the two tools to what
docs/DATA_FILES.md and docs/LIFTING.md say of the step:

- `gb_data.py pointers` on a record says of all five that an operand leads
  to them, and that they stay;
- `gb_lift.py source --data-tree` writes the operands the record saw as
  names (`ctx->hl = TableA;`), each defined as the address the ROM has, and
  an operand no record saw at work stays a number;
- `gb_lift.py build` says which operands the game has by name, and which
  other operands spell an address;
- `pointers --named` then lets TableA move; TableB stays, because a function
  that never ran spells its address too; TableE stays, because the game also
  compares the operand that leads to it; TableJ stays, because the game
  jumps to what that operand made; TableC stays, because the game copies the
  function that reads it into RAM and runs the copy, which no source turns;
- `build --move-all` refuses to move a thing the source names without being
  told where to write the header, and with it writes where the name leads;
- the functions still match the ROM's code with that header there (the
  checker compares with the ROM as it is);
- the game built with the header ends as the original does on the image in
  which the table has moved and its old place is spoilt, and the game built
  without it does not;
- a thing is copied as far as the source says its name runs, where that is
  further than the records read.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import os
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_LIFT = ROOT / "tools" / "gb_lift.py"
GB_DATA = ROOT / "tools" / "gb_data.py"
FRAMES = 60
PRESET = "x64-release" if os.name == "nt" else "release"

# Results live in HRAM so that `--dump-state` shows them (hram_ff80_ff90).
VBLANK_FLAG, LOOPS, SUM_A, SUM_B, SUM_E, SUM_J, WAITS, SUM_C = range(0x80, 0x88)
TABLE_A, TABLE_B, TABLE_E, TABLE_C = [1, 2, 3, 4], [10, 20], [7], [9]
COPY = 0xC100           # where the game copies ReadC to, and runs it


def assemble(origin: int, items: list) -> tuple[bytes, dict[str, int]]:
    """Bytes, ("label", name), ("abs", name) and ("rel", name) items to code."""
    labels: dict[str, int] = {}
    for emit in (False, True):
        address = origin
        out = bytearray()
        for item in items:
            if isinstance(item, (bytes, bytearray, list)):
                data = bytes(item)
            elif item[0] == "label":
                labels[item[1]] = address
                continue
            elif item[0] == "abs":
                target = labels.get(item[1], 0)
                data = bytes([target & 0xFF, target >> 8])
            else:
                target = labels.get(item[1], address + 1)
                offset = target - (address + 1)
                if emit and not -128 <= offset <= 127:
                    raise AssertionError(f"relative jump to {item[1]} out of range")
                data = bytes([offset & 0xFF])
            out += data
            address += len(data)
    return bytes(out), labels


def bump(register: int) -> list[int]:
    """ldh a,[r] / inc a / ldh [r],a"""
    return [0xF0, register, 0x3C, 0xE0, register]


PROGRAM = [
    ("label", "Start"),
    [0xF3],                                  # di
    [0x31, 0xFF, 0xDF],                      # ld sp, $DFFF
    [0xAF, 0xE0, 0x0F],                      # IF = 0
    [0x21, 0x80, 0xFF, 0x06, 0x20],          # ld hl, $FF80 / ld b, 32
    ("label", "Clear"),
    [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli], a / dec b / jr nz
    [0x21], ("abs", "ReadC"),                # ld hl, ReadC: the seven bytes of it, copied into RAM
    [0x11, COPY & 0xFF, COPY >> 8, 0x06, 0x07],
    ("label", "Copy"),
    [0x2A, 0x12, 0x13, 0x05, 0x20], ("rel", "Copy"),  # ld a, [hli] / ld [de], a / inc de / dec b / jr nz
    [0x3E, 0x01, 0xE0, 0xFF],                # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    [0xCD], ("abs", "ReadA"),
    [0xCD], ("abs", "ReadB"),
    [0xAF, 0xC4], ("abs", "UnseenB"),        # xor a / call nz: a call that is never made
    [0xCD], ("abs", "ReadE"),
    [0xCD], ("abs", "ReadJ"),
    [0xCD], ("abs", "ReadC"),
    [0xCD, COPY & 0xFF, COPY >> 8],          # and the copy of it
    [0xCD], ("abs", "WaitFrame"),
    bump(LOOPS),
    [0x18], ("rel", "MainLoop"),

    ("label", "VBlank"),
    [0xF5, 0x3E, 0x01, 0xE0, VBLANK_FLAG, 0xF1, 0xD9],  # push af / flag = 1 / pop af / reti

    # The four bytes of TableA, added up.
    ("label", "ReadA"),
    [0x21], ("abs", "TableA"),               # ld hl, TableA
    [0x2A, 0x86, 0x23, 0x86, 0x23, 0x86],    # ld a, [hli] / add a, [hl] / inc hl / add a, [hl] / inc hl / add a, [hl]
    [0xE0, SUM_A, 0xC9],

    ("label", "ReadB"),
    [0x21], ("abs", "TableB"),               # ld hl, TableB
    [0x2A, 0x86, 0xE0, SUM_B, 0xC9],         # ld a, [hli] / add a, [hl]

    # Reads TableB too, and never runs: no record sees this operand at work.
    ("label", "UnseenB"),
    [0x21], ("abs", "TableB"),
    [0x7E, 0xE0, SUM_B, 0xC9],               # ld a, [hl]

    # Reads TableE, and compares half of the address it read it through.
    ("label", "ReadE"),
    [0x21], ("abs", "TableE"),
    [0x7D, 0xFE, 0x00],                      # ld a, l / cp 0
    [0x7E, 0xE0, SUM_E, 0xC9],               # ld a, [hl]

    # Reads TableJ, and then jumps to what the operand has become: the code
    # that follows the table's one byte.
    ("label", "ReadJ"),
    [0x21], ("abs", "TableJ"),
    [0x2A, 0xE0, SUM_J, 0xE9],               # ld a, [hli] / ldh [SUM_J], a / jp hl

    # Reads TableC, here and from the copy of these seven bytes the game runs in RAM.
    ("label", "ReadC"),
    [0x21], ("abs", "TableC"),
    [0x7E, 0xE0, SUM_C, 0xC9],               # ld a, [hl]

    ("label", "WaitFrame"),
    [0xAF, 0xE0, VBLANK_FLAG],
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    [0xF0, VBLANK_FLAG, 0xA7, 0x28], ("rel", "WaitFrameLoop"),
    bump(WAITS),
    [0xC9],

    ("label", "TableJ"),
    [0x05],
    ("label", "AfterJ"),
    [0xC9],                                  # ret: where ReadJ's jump lands
    ("label", "TableA"),
    TABLE_A,
    ("label", "TableB"),
    TABLE_B,
    ("label", "TableE"),
    TABLE_E,
    ("label", "TableC"),
    TABLE_C,
    ("label", "End"),
]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble(0x150, PROGRAM)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13E] = b"LIFT NAMES"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


def run(command: list, cwd: Path | None = None, timeout: int = 900,
        expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    result = subprocess.run([str(part) for part in command], cwd=cwd, capture_output=True, text=True,
                            timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-4000:]}\n"
                             f"{result.stderr[-4000:]}")
    return result


def lift(*arguments, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, GB_LIFT, *arguments], expect=expect)


def data(*arguments, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, GB_DATA, *arguments], expect=expect)


def play(executable: Path, root: Path, name: str, *flags) -> list[int]:
    """Run the game headless for FRAMES frames; its results."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    run([executable, "--headless", "--no-audio", "--limit-frames", str(FRAMES),
         "--save-dir", saves, "--dump-state", dump, *flags], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8"))["hram_ff80_ff90"]


def as_far_as_a_name_runs() -> None:
    """`move` copies a thing as far as the source says its name runs."""
    sys.path.insert(0, str(ROOT / "tools"))
    import gb_data_pointers

    image = bytearray(bytes(range(64)) + bytes(0x4000 - 64))
    image[5:7] = bytes([16, 0])
    image = bytes(image)
    # Two bytes the records read, at 16, through the operand at 5; the source's name runs six.
    thing = {"offset": 16, "size": 2, "bank": 0, "address": 16, "read": [[0, 2]], "moves": True,
             "pointers": [{"low": 5, "high": 6, "value": 16, "at": 0, "operand": True, "named": "Table", "runs": 6}]}
    gb_data_pointers.checked_things({"things": [thing], "rom": {"size": len(image)}})
    moved, done = gb_data_pointers.move(image, [thing], [(0x3000, 64)], [], True)
    assert done["moved"] == [(16, 6, 0x3000, 6)], done["moved"]
    assert moved[0x3000:0x3006] == image[16:22] and done["named"] == [("Table", 16, 0x3000)], done
    assert moved[5:7] == image[5:7], "an operand the source names was turned in the image"
    assert moved[16:18] == bytes([16 ^ 0xFF, 17 ^ 0xFF]) and moved[18:22] == image[18:22], "what was spoilt"
    short = dict(thing, pointers=[dict(thing["pointers"][0])])
    del short["pointers"][0]["runs"]
    assert gb_data_pointers.move(image, [short], [(0x3000, 64)], [], True)[1]["moved"] == [(16, 2, 0x3000, 2)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()
    cmake_args = [f"--cmake-arg={item}" for item in args.cmake_arg]

    scratch = tempfile.TemporaryDirectory(prefix="gblift-names-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        as_far_as_a_name_runs()
        rom, labels = make_rom()
        rom_path = root / "lift_names.gb"
        rom_path.write_bytes(rom)
        at = {name: f"00:{address:04X}" for name, address in labels.items()}

        def build(*more: str, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
            return lift("build", folder, "--gbrecomp", args.gbrecomp, "--trials", "200", *cmake_args, *more,
                        expect=expect)

        def things() -> dict[int, dict]:
            manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
            return {thing["address"]: thing for thing in manifest["things"]}

        plain = ["--no-scan", "--pointer-scan", "off"]
        export = root / "export.json"
        run([args.gbrecomp, rom_path, *plain, "--lift-export", export])
        symbols = root / "names.sym"
        symbols.write_text("".join(f"{at[name].lower()} {name}\n" for name in (
            "ReadA", "ReadB", "UnseenB", "ReadE", "ReadJ", "ReadC", "TableJ", "AfterJ", "TableA", "TableB", "TableE",
            "TableC", "End")),
            encoding="utf-8")
        folder = root / "source"
        lift("source", export, "-o", folder, "--symbols", symbols, "--rom", rom_path, "--", *plain)
        bank = folder / "src" / "lifted_bank_00.c"
        assert f"ctx->hl = 0x{labels['TableA']:04X};" in bank.read_text(encoding="utf-8")
        executable = folder / "build" / PRESET / "game" / ("game.exe" if os.name == "nt" else "game")

        # ---- The game as it is, and a record of what it reads through.
        build()
        original = play(executable, root, "original")
        loops = original[LOOPS - 0x80]
        assert loops >= FRAMES - 4 and original[SUM_A - 0x80] == sum(TABLE_A) and \
            original[SUM_B - 0x80] == sum(TABLE_B) and original[SUM_E - 0x80] == TABLE_E[0] and \
            original[SUM_J - 0x80] == 5 and original[SUM_C - 0x80] == TABLE_C[0], original
        record = root / "record.json"
        play(executable, root, "recorded", "--data-use", record, "--data-use-pointers")

        tree = root / "tree"
        data("split", export, "--rom", rom_path, "-o", tree)
        data("uses", tree, record)
        said = data("pointers", tree, record).stdout
        assert "5 are reached through an instruction's operand that no source names" in said, said
        assert not any(thing["moves"] for thing in things().values())
        data("free", tree, "-o", tree / "free.json")

        # ---- The source, with the operands the record saw as names.
        lift("source", export, "-o", folder, "--data-tree", tree)
        text = bank.read_text(encoding="utf-8")
        for name in ("TableA", "TableB", "TableC", "TableE", "TableJ"):
            assert f"ctx->hl = {name};" in text, f"{name} is not named in the source"
        # UnseenB's operand says the same as ReadB's, and no record saw what it is: a number.
        assert text.count("ctx->hl = TableB;") == 1 and f"ctx->hl = 0x{labels['TableB']:04X};" in text, "UnseenB"
        header = (folder / "src" / "lifted_data.h").read_text(encoding="utf-8")
        assert f"#define TableA 0x{labels['TableA']:04X}" in header and '#include "lifted_moved.h"' in header, header
        assert '#include "lifted_data.h"' in (folder / "src" / "lifted_names.h").read_text(encoding="utf-8")
        built = build().stdout
        assert ", 0 differ" in built, built
        assert play(executable, root, "named") == original
        named = json.loads((folder / ".lift" / "named-operands.json").read_text(encoding="utf-8"))
        assert sorted(entry["name"] for entry in named["operands"]) == ["TableA", "TableB", "TableC", "TableE", "TableJ"], named
        runs = {entry["name"]: entry["size"] for entry in named["operands"]}
        assert runs == {"TableA": len(TABLE_A), "TableB": len(TABLE_B), "TableE": len(TABLE_E), "TableJ": 1, "TableC": len(TABLE_C)}, runs
        assert any(entry["value"] == labels["TableB"] for entry in named["unnamed"]), named["unnamed"]

        # ---- What can move now.
        said = data("pointers", tree, record, "--named", folder / ".lift" / "named-operands.json").stdout
        found = things()
        table_a, table_b = found[labels["TableA"]], found[labels["TableB"]]
        table_e, table_j = found[labels["TableE"]], found[labels["TableJ"]]
        assert table_a["moves"] and table_a["pointers"][0]["named"] == "TableA" and \
            table_a["pointers"][0]["runs"] == len(TABLE_A), table_a
        assert not table_b["moves"] and "an operand that no source names says" in table_b["why"], table_b
        assert not table_e["moves"] and "also compared it" in table_e["why"], table_e
        assert not table_j["moves"] and "jumped to what it made" in table_j["why"], table_j
        table_c = found[labels["TableC"]]
        assert not table_c["moves"] and "also reads it as data" in table_c["why"], table_c
        assert "1 have their address spelled by an operand that no source names" in said, said

        # ---- Moved. The source has to be told where to.
        moved_image = root / "moved.gb"
        refused = data("build", tree, "-o", moved_image, "--move-all", expect=(2,)).stderr
        assert "--moved-header" in refused and "TableA" in refused, refused
        moved_header = folder / "src" / "lifted_moved.h"
        said = data("build", tree, "-o", moved_image, "--move-all", "--moved-header", moved_header).stdout
        assert "moved 1 thing" in said and "TableA was" in said, said
        listed = json.loads((root / "moved.gb.moved.json").read_text(encoding="utf-8"))
        assert [entry["name"] for entry in listed["named"]] == ["TableA"] and not listed["turned"], listed
        new_place = listed["named"][0]["says"]
        assert f"#define TableA 0x{new_place:04X}" in moved_header.read_text(encoding="utf-8")
        image = moved_image.read_bytes()
        assert image[new_place:new_place + 4] == bytes(TABLE_A)
        assert image[labels["TableA"]:labels["TableA"] + 4] == bytes(value ^ 0xFF for value in TABLE_A), "not spoilt"
        assert image[labels["ReadA"] + 1:labels["ReadA"] + 3] == rom[labels["ReadA"] + 1:labels["ReadA"] + 3], \
            "the operand was turned in the image, which is code"

        # The functions are still the ROM's code: the checker compares with the ROM as it is.
        built = build("--recheck").stdout
        assert ", 0 differ" in built, built
        # The game built with the header, on the image the header is of: as the original.
        assert play(executable, root, "moved", "--rom-image", moved_image) == original
        # On the ROM as it is, that game reads where nothing is.
        astray = play(executable, root, "astray")
        assert astray[SUM_A - 0x80] != original[SUM_A - 0x80], astray

        # ---- And without the header, the game reads the old place, which is spoilt.
        data("build", tree, "-o", root / "plain.gb", "--moved-header", moved_header)
        assert "#define TableA" not in moved_header.read_text(encoding="utf-8")
        build()
        assert play(executable, root, "plain") == original
        stale = play(executable, root, "stale", "--rom-image", moved_image)
        assert stale[SUM_A - 0x80] == sum(value ^ 0xFF for value in TABLE_A) % 256, stale
        assert [x for i, x in enumerate(stale) if i != SUM_A - 0x80] == \
            [x for i, x in enumerate(original) if i != SUM_A - 0x80], (original, stale)
    finally:
        if scratch:
            scratch.cleanup()

    print("an operand went by a name, what it leads to moved, and the game built with where it went plays the same")
    return 0


if __name__ == "__main__":
    sys.exit(main())
