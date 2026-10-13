#!/usr/bin/env python3
"""A ROM's data taken apart into files and put together again, end to end.

Builds a small game that copies five bytes of data, four from the home bank
and one from bank 1, to high RAM, where a state dump shows them. Then:

- `gb_data.py split` has to write every byte of the ROM to exactly one file,
  code, data and header apart;
- `gb_data.py build` has to make the original ROM from those files, byte for
  byte, and say so; the game given that image has to end as it does without;
- with two data files edited, `status` has to name them, `build` has to make
  an image that differs in those two bytes and fail `--expect-original`, and
  the game given it has to copy the edited bytes, while the same executable
  without the image copies the original ones;
- an edit to a file of code, or one that changes a file's size, has to stop
  `build`; a manifest that points outside the tree has to be refused;
- the game has to refuse an image that differs in its code or its header, or
  is not the size of its ROM.
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

START = 0x0150
DATA = 0x0200            # four bytes in the home bank
BANKED = 0x4100          # one in bank 1
ORIGINAL = [0x11, 0x22, 0x33, 0x44, 0x55]
FRAMES = 30


def make_rom() -> bytearray:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, START & 0xFF, START >> 8])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"DATA TREE"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    code = [0xF3, 0x31, 0xFF, 0xDF, 0x21, DATA & 0xFF, DATA >> 8]     # di / ld sp,$DFFF / ld hl,Data
    for register in range(4):
        code += [0x2A, 0xE0, 0x80 + register]                         # ld a,[hli] / ldh [$80+n],a
    code += [0xFA, BANKED & 0xFF, BANKED >> 8, 0xE0, 0x84]            # ld a,[Banked] / ldh [$84],a
    code += [0x18, 0xFE]                                              # jr to itself
    rom[START:START + len(code)] = bytes(code)
    rom[DATA:DATA + 4] = bytes(ORIGINAL[:4])
    rom[BANKED] = ORIGINAL[4]
    return rom


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def gb_data(*arguments: str, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_DATA), *arguments], expect)


def play(executable: Path, root: Path, name: str, *flags: str, expect: tuple[int, ...] = (0,)) -> tuple[dict, str]:
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    result = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES),
                  "--save-dir", str(saves), "--dump-state", str(dump), *flags], expect, timeout=180)
    state = json.loads(dump.read_text(encoding="utf-8")) if dump.exists() else {}
    return state, result.stdout + result.stderr


def copied(state: dict) -> list[int]:
    return state["hram_ff80_fffe"][:5]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]

    with tempfile.TemporaryDirectory(prefix="gbdata-") as scratch:
        root = Path(scratch)
        rom = make_rom()
        rom_path = root / "data.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        run([str(args.gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "data_game"])
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
             "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
        run(["ninja", "-C", str(project / "build")])
        executable = next(path for path in (project / "build" / "data_game.exe", project / "build" / "data_game")
                          if path.exists())

        # Apart: every byte in exactly one file.
        tree = root / "tree"
        said = gb_data("split", str(export_path), "--rom", str(rom_path), "-o", str(tree)).stdout
        assert "data" in said and "code" in said and "header" in said, said
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["schema"] == "gbrecomp.data-tree" and manifest["version"] == 1
        cursor = 0
        for region in manifest["regions"]:
            assert region["offset"] == cursor and (tree / region["file"]).stat().st_size == region["size"], region
            cursor += region["size"]
        assert cursor == len(rom) == manifest["rom"]["size"]
        kinds = {region["kind"] for region in manifest["regions"]}
        assert kinds == {"code", "data", "header"}, kinds

        def region_of(offset: int) -> dict:
            return next(region for region in manifest["regions"]
                        if region["offset"] <= offset < region["offset"] + region["size"])

        assert region_of(START)["kind"] == "code" and region_of(0x134)["kind"] == "header"
        assert region_of(DATA)["kind"] == "data" and region_of(BANKED)["kind"] == "data"
        assert region_of(BANKED)["bank"] == 1 and region_of(BANKED)["file"].startswith("bank_01/")
        # A second split does not write over a tree, where there may be edits.
        again = gb_data("split", str(export_path), "--rom", str(rom_path), "-o", str(tree), expect=(2,))
        assert "already holds a data tree" in again.stderr, again.stderr

        # Together again: the original, byte for byte.
        image = root / "rebuilt.gb"
        said = gb_data("build", str(tree), "-o", str(image), "--expect-original").stdout
        assert "the original ROM, byte for byte" in said, said
        assert image.read_bytes() == bytes(rom)
        assert "nothing edited" in gb_data("status", str(tree)).stdout
        plain, _ = play(executable, root, "plain")
        assert copied(plain) == ORIGINAL, copied(plain)
        same, log = play(executable, root, "same", "--rom-image", str(image))
        assert "0 bytes differ from the ROM this game was built from" in log, log
        assert same == plain, [key for key in plain if plain[key] != same.get(key)]

        # Two data files edited.
        def poke(offset: int, value: int) -> Path:
            region = region_of(offset)
            path = tree / region["file"]
            content = bytearray(path.read_bytes())
            content[offset - region["offset"]] = value
            path.write_bytes(content)
            return path

        poke(DATA, 0x99)
        poke(BANKED, 0x77)
        said = gb_data("status", str(tree)).stdout
        assert said.count("edited: ") == 2 and region_of(BANKED)["file"] in said, said
        edited = root / "edited.gb"
        said = gb_data("build", str(tree), "-o", str(edited)).stdout
        assert f"2 of {len(manifest['regions'])} files edited" in said, said
        differ = [offset for offset in range(len(rom)) if edited.read_bytes()[offset] != rom[offset]]
        assert differ == [DATA, BANKED], differ
        gb_data("build", str(tree), "-o", str(edited), "--expect-original", expect=(1,))
        changed, log = play(executable, root, "edited", "--rom-image", str(edited))
        assert "2 bytes differ from the ROM this game was built from" in log, log
        assert copied(changed) == [0x99, 0x22, 0x33, 0x44, 0x77], copied(changed)
        # The executable itself is as it was.
        assert copied(play(executable, root, "plain-again")[0]) == ORIGINAL

        # Code is not data: the tool stops, and names the file.
        code_file = poke(START + 4, 0x11)
        refused = gb_data("build", str(tree), "-o", str(root / "no.gb"), expect=(2,))
        assert "code the game's C was compiled from" in refused.stderr and \
            region_of(START)["file"] in refused.stderr, refused.stderr
        assert "not data" in gb_data("status", str(tree)).stdout
        code_file.write_bytes(bytes(rom[region_of(START)["offset"]:][:region_of(START)["size"]]))
        # Nor does a region change its size.
        data_file = tree / region_of(DATA)["file"]
        kept = data_file.read_bytes()
        data_file.write_bytes(kept + b"\x00")
        refused = gb_data("build", str(tree), "-o", str(root / "no.gb"), expect=(2,))
        assert "a region keeps its size" in refused.stderr, refused.stderr
        data_file.write_bytes(kept)
        gb_data("build", str(tree), "-o", str(edited))
        # A manifest that points outside its tree is not followed.
        elsewhere = root / "elsewhere"
        elsewhere.mkdir()
        outside = json.loads(json.dumps(manifest))
        outside["regions"][0]["file"] = "../data.gb"
        (elsewhere / "manifest.json").write_text(json.dumps(outside), encoding="utf-8")
        refused = gb_data("build", str(elsewhere), "-o", str(root / "no.gb"), expect=(2,))
        assert "is not a file inside" in refused.stderr, refused.stderr

        # The game checks for itself: an image made any other way is held to the same.
        def refuses(name: str, content: bytes, words: str) -> None:
            path = root / f"{name}.gb"
            path.write_bytes(content)
            _, log = play(executable, root, name, "--rom-image", str(path), expect=(1,))
            assert words in log, (name, log)

        bad = bytearray(rom)
        bad[START + 4] ^= 0xFF
        refuses("code-changed", bytes(bad), f"the byte at offset 0x{START + 4:05X} is not the one this game's code")
        bad = bytearray(rom)
        bad[0x147] = 0x01
        refuses("header-changed", bytes(bad), "the byte at offset 0x00147")
        refuses("short", bytes(rom[:-1]), "it is shorter than the 32768 bytes")
        refuses("long", bytes(rom) + b"\x00", "it is longer than the 32768 bytes")
        _, log = play(executable, root, "missing", "--rom-image", str(root / "none.gb"), expect=(1,))
        assert "cannot open it" in log, log

    print("data rebuild end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
