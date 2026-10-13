#!/usr/bin/env python3
"""What a game shows to be data, told to the analysis, end to end.

Builds a small game with data where the analysis walks into it: a tile, and
a table of one address, each right after a branch the game always takes, so
that the way not taken runs on through them. The byte in front of the tile
is the start of a three-byte instruction, which takes the tile's first two
bytes for its operand. The game copies the tile to video memory and jumps
through the table. It also copies one of its own routines into RAM, and adds
a constant to a byte of it and draws that: bytes it reads without using them
as they are.

- From the first analysis the tile and the table are in files of code:
  `tiles` will not draw them, and no image may change them.
- `gb_data.py annotations` has to write, from a record of the game played,
  the tile, the table and four bytes drawn from a data file, each with what
  it was used for, and not a byte of the routine.
- An export and a game made with `gbrecomp --annotations` that file have to
  have the tile whole in a data file, the instruction that ran into it gone
  with the rest, and the routine still a function. The game has to end as
  the first one does.
- In a tree split from that export the tile can be drawn, and the game made
  with the annotations shows an edited one, where the game made without them
  refuses the image.
- Told again, the tool has to write the same file and find nothing in code.
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
GB_DATA = ROOT / "tools" / "gb_data.py"
sys.path.insert(0, str(ROOT / "tools"))
import gb_data  # noqa: E402  (its PNG reader and writer)
import gb_discover  # noqa: E402  (the save state reader)

START = 0x0150
HELPER, HANDLER, DRAWN = 0x02E0, 0x0300, 0x0400
# Sixteen bytes that are a tile to the game and sixteen one-byte instructions to the analysis.
TILE = bytes([0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x3C, 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x3D, 0x04, 0x0C])
FRAMES = 20


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def copy(source: int, target: int, count: int) -> list[int]:
    """ld hl,source / ld de,target / ld b,count / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop"""
    return [0x21, *word(source), 0x11, *word(target), 0x06, count, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"DATA TOLD"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    at: dict[str, int] = {}
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF
    # The screen off while video memory is written: wait for line 144, then xor a / ldh [$40],a
    code += [0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xAF, 0xE0, 0x40]

    def here() -> int:
        return START + len(code)

    code += [0x3E, 0x01, 0xB7, 0x20, 0x11]                                 # ld a,1 / or a / jr nz,Over
    at["before"] = here()
    code += [0x21]                                  # never run, never read: `ld hl,nn` to whoever decodes it
    at["tile"] = here()
    code += list(TILE)
    code += copy(at["tile"], 0x8000, 16)                                   # Over: the tile, to video memory
    code += [0xCD, *word(HELPER)]                                          # call Helper
    code += copy(HELPER, 0xC000, 5)                                        # and a copy of it into RAM
    code += [0x21, *word(HELPER), 0x7E, 0xC6, 0x80, 0xEA, 0x40, 0x98]      # ld a,[Helper] / add a,$80 / ld [$9840],a
    code += [0x3E, 0x01, 0xB7, 0x20, 0x02]                                 # ld a,1 / or a / jr nz,Past
    at["table"] = here()
    code += word(HANDLER)                                                  # 00 03: nop / inc bc, decoded
    # Past: ld hl,Table / ld a,[hli] / ld h,[hl] / ld l,a / ld de,back / push de / jp hl
    code += [0x21, *word(at["table"]), 0x2A, 0x66, 0x6F]
    back = here() + 5
    code += [0x11, *word(back), 0xD5, 0xE9]
    assert here() == back
    code += copy(DRAWN, 0x9800, 4)
    code += [0x3E, 0x91, 0xE0, 0x40, 0x18, 0xFE]                           # ld a,$91 / ldh [$40],a / jr to itself
    assert here() <= HELPER
    rom[START:START + len(code)] = bytes(code)
    rom[HELPER:HELPER + 5] = bytes([0x3E, 0x5A, 0xE0, 0x85, 0xC9])         # ld a,$5A / ldh [$85],a / ret
    rom[HANDLER:HANDLER + 5] = bytes([0x3E, 0x77, 0xE0, 0x86, 0xC9])       # ld a,$77 / ldh [$86],a / ret
    rom[DRAWN:DRAWN + 4] = bytes([0x41, 0x42, 0x43, 0x44])
    return rom, at


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def tool(*arguments: object, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_DATA), *map(str, arguments)], expect=expect)


def generate(gbrecomp: Path, rom_path: Path, root: Path, name: str, cmake_args: list[str],
             *more: str) -> tuple[dict, Path]:
    """The export and the game of one analysis."""
    analysis = ["--no-scan", "--pointer-scan", "off", *more]
    export_path = root / f"{name}.json"
    project = root / name
    run([str(gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
    run([str(gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "told_game"])
    run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"), "-DCMAKE_BUILD_TYPE=Release",
         *cmake_args])
    run(["ninja", "-C", str(project / "build")])
    executable = next(path for path in (project / "build" / "told_game.exe", project / "build" / "told_game")
                      if path.exists())
    return json.loads(export_path.read_text(encoding="utf-8")), executable


def play(executable: Path, root: Path, name: str, *flags: str,
         expect: tuple[int, ...] = (0,)) -> tuple[dict, gb_discover.Savestate | None, str]:
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump, state = root / f"{name}.json", root / f"{name}.state"
    result = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES), "--save-dir",
                  str(saves), "--dump-state", str(dump), "--save-state-file", str(state), *flags],
                 expect=expect, timeout=180)
    if result.returncode:
        return {}, None, result.stdout + result.stderr
    return json.loads(dump.read_text(encoding="utf-8")), gb_discover.Savestate.load(state), \
        result.stdout + result.stderr


def data_region(export: dict, offset: int) -> dict | None:
    return next((region for region in export["data_regions"]
                 if region["rom_offset"] <= offset < region["rom_offset"] + region["size"]), None)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="gbtold-") as scratch:
        root = Path(scratch)
        rom, at = make_rom()
        rom_path = root / "told.gb"
        rom_path.write_bytes(rom)

        # As the analysis has it by itself: the tile and the table are instructions.
        first, game = generate(args.gbrecomp, rom_path, root, "first", args.cmake_arg)
        assert data_region(first, at["tile"]) is None and data_region(first, at["table"]) is None
        assert any(function["address"] == HELPER for function in first["functions"])
        record_path = root / "use.json"
        plain, state, _ = play(game, root, "plain", "--data-use", str(record_path))
        assert plain["hram_ff80_fffe"][5:7] == [0x5A, 0x77], plain["hram_ff80_fffe"][:8]
        assert bytes(state.read(0x8000 + index) for index in range(16)) == TILE
        tree = root / "tree"
        tool("split", root / "first.json", "--rom", rom_path, "-o", tree)
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        code = next(region for region in manifest["regions"]
                    if region["offset"] <= at["tile"] < region["offset"] + region["size"])
        assert code["kind"] == "code", code
        stderr = tool("tiles", tree, code["file"], "--offset", at["tile"] - code["offset"], "--tiles", 1,
                      expect=(2,)).stderr
        assert "is code, and only data can be drawn as tiles" in stderr, stderr
        said = tool("uses", tree, record_path).stdout
        assert "bytes the game read as data are in files of code" in said, said

        # What the game showed to be data: what it used as it is, and not what it only read.
        told = root / "told.annotations"
        said = tool("annotations", tree, record_path, "-o", told).stdout
        assert "1 record: the game used 22 bytes as they are, in 3 runs" in said, said
        assert "18 of them, in 2 of the runs, are in files of code" in said, said
        lines = [line for line in told.read_text(encoding="utf-8").splitlines() if line and not line.startswith("#")]
        assert lines == [f"data 00:{at['tile']:04X} 16 ; tiles", f"data 00:{at['table']:04X} 2 ; jump",
                         f"data 00:{DRAWN:04X} 4 ; tile_map"], lines

        # Told, the analysis leaves them alone, and the instruction that ran on into the tile with them.
        second, told_game = generate(args.gbrecomp, rom_path, root, "second", args.cmake_arg,
                                     "--annotations", str(told))
        region = data_region(second, at["tile"])
        assert region and (region["rom_offset"], region["size"]) == (at["before"], 17), region
        region = data_region(second, at["table"])
        assert region and (region["rom_offset"], region["size"]) == (at["table"], 2), region
        assert any(function["address"] == HELPER for function in second["functions"])
        assert data_region(second, HELPER) is None and data_region(second, START) is None
        again, _, _ = play(told_game, root, "told")
        differ = [key for key in plain if key != "dispatch_fallbacks" and plain[key] != again.get(key)]
        assert not differ, differ

        # The tile is a data file's now: drawn, edited, and seen in the game that was told.
        told_tree = root / "told-tree"
        tool("split", root / "second.json", "--rom", rom_path, "-o", told_tree)
        manifest = json.loads((told_tree / "manifest.json").read_text(encoding="utf-8"))
        data = next(entry for entry in manifest["regions"]
                    if entry["offset"] <= at["tile"] < entry["offset"] + entry["size"])
        assert data["kind"] == "data" and data["offset"] == at["before"], data
        tool("tiles", told_tree, data["file"], "--offset", 1, "--tiles", 1)
        tool("build", told_tree, "-o", root / "same.gb", "--expect-original")
        picture = told_tree / (data["file"][:-len(".bin")] + f".tiles_{at['tile']:04X}.png")
        width, height, grey = gb_data.read_png(picture.name, picture.read_bytes())
        picture.write_bytes(gb_data.write_png(width, height, bytes(255 - level for level in grey)))
        image_path = root / "edited.gb"
        tool("build", told_tree, "-o", image_path)
        image = image_path.read_bytes()
        turned = bytes(byte ^ 0xFF for byte in TILE)                       # every colour to its opposite
        assert image[at["tile"]:at["tile"] + 16] == turned
        assert image[:at["tile"]] == bytes(rom[:at["tile"]]) and image[at["tile"] + 16:] == bytes(rom[at["tile"] + 16:])
        _, state, _ = play(told_game, root, "edited", "--rom-image", str(image_path))
        assert bytes(state.read(0x8000 + index) for index in range(16)) == turned
        # The game that was not told has those bytes in its code, and will not have them changed.
        _, _, log = play(game, root, "refused", "--rom-image", str(image_path), expect=(1,))
        assert f"the byte at offset 0x{at['tile']:05X} is not the one this game's code" in log, log

        # Told again there is nothing new, and nothing of it left in code.
        told_again = root / "again.annotations"
        said = tool("annotations", told_tree, record_path, "-o", told_again).stdout
        assert "none of them is in a file of code" in said, said
        assert told_again.read_bytes() == told.read_bytes()

        # A record is of one ROM.
        record = json.loads(record_path.read_text(encoding="utf-8"))
        record["rom"]["global_checksum"] ^= 1
        (root / "other.json").write_text(json.dumps(record), encoding="utf-8")
        stderr = tool("annotations", told_tree, root / "other.json", "-o", root / "no.annotations", expect=(2,)).stderr
        assert "was recorded from another ROM" in stderr and not (root / "no.annotations").exists(), stderr

    print("data annotations end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
