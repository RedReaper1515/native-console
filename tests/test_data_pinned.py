#!/usr/bin/env python3
"""The bytes a generated game holds an image to are the bytes its data tree calls code.

A game given an image with `--rom-image` checks it against the ranges its
code was made from, which the generator writes into `<prefix>_rom.c`. A data
tree split from an export made with the same options says which files are
code and which are data. The two have to be one answer: where the game holds
more than the tree calls code, somebody edits a file of data, `build` takes
it, and the game refuses the image.

Builds a small ROM on a cartridge with banks, generates it three ways (as it
is; with an entry point asked for at the byte in front of declared data;
with a recorded run said to have arrived there) and compares the two for
each. The entry point is the first byte of an instruction three bytes long,
which would run on into the data and so is not decoded: a block without an
instruction, which is no code for an image to match. The ROM also copies a
routine into high RAM and calls it there, which gives the analysis a block
at a high address just before that one. Compiles no game.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"

BANK = 0x4000
START, ROUTINE, HRAM_ROUTINE = 0x0150, 0x0300, 0xFF90
FAR = 0x4100                        # 01:4100, and the offset in the ROM: the byte in front of the declared data
DECLARED = 16


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def make_rom() -> bytearray:
    rom = bytearray(4 * BANK)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13C] = b"PIN TEST"
    rom[0x147], rom[0x148] = 0x01, 0x01                                    # MBC1, four banks
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF
    # ld hl,Routine / ld de,$FF90 / ld b,10 / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop
    code += [0x21, *word(ROUTINE), 0x11, *word(HRAM_ROUTINE), 0x06, 0x0A, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]
    code += [0xCD, *word(HRAM_ROUTINE)]                                    # call the copy of the routine
    code += [0x3E, 0x01, 0xEA, 0x00, 0x20]                                 # ld a,1 / ld [$2000],a
    code += [0xFA, *word(FAR + 1), 0xE0, 0x86]                             # ld a,[Declared] / ldh [$86],a
    code += [0x18, 0xFE]                                                   # jr to itself
    rom[START:START + len(code)] = bytes(code)
    # ld a,$C2 / ldh [$46],a / ld a,$28 / wait: dec a / jr nz,wait / ret
    rom[ROUTINE:ROUTINE + 10] = bytes([0x3E, 0xC2, 0xE0, 0x46, 0x3E, 0x28, 0x3D, 0x20, 0xFD, 0xC9])
    rom[FAR] = 0x21                                                        # `ld hl,nn` to whoever decodes it
    rom[FAR + 1:FAR + 1 + DECLARED] = bytes(range(0x40, 0x40 + DECLARED))
    return rom


def run(command: list[str]) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    if result.returncode:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def joined(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """The same bytes, in as few ranges as say them."""
    out: list[tuple[int, int]] = []
    for first, end in sorted(ranges):
        if out and first <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], end))
        else:
            out.append((first, end))
    return out


def held(project: Path) -> list[tuple[int, int]]:
    """The ranges the generated game holds an image to."""
    text = (project / "pin_rom.c").read_text(encoding="utf-8")
    table = re.search(r"_pinned\[\] = \{(.*?)\};", text, re.S)
    assert table, "the generated ROM file has no table of what an image is held to"
    numbers = [int(number) for number in re.findall(r"(\d+)u", table.group(1))]
    pairs = int(re.search(r"_pinned_pairs = (\d+);", text).group(1))
    assert len(numbers) == 2 * pairs, (len(numbers), pairs)
    return joined(list(zip(numbers[0::2], numbers[1::2])))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="gbpin-") as scratch:
        root = Path(scratch)
        rom = make_rom()
        rom_path = root / "pin.gb"
        rom_path.write_bytes(rom)
        annotations = root / "pin.annotations"
        annotations.write_text(f"data 01:{FAR + 1:04X} {DECLARED}\n", encoding="utf-8")
        edges = root / "pin.edges.json"
        edges.write_text(json.dumps({"schema": "gbrecomp.lift-edges", "version": 1,
                                     "rom": {"sha256": hashlib.sha256(rom).hexdigest(), "size": len(rom)},
                                     "edges": [], "entered": [[1, FAR]]}), encoding="utf-8")
        ways = {"plain": [],
                "asked": ["--add-entry-point", f"1:{FAR:04X}"],
                "arrived": ["--lift-edges", str(edges)]}
        seen: dict[str, list[tuple[int, int]]] = {}
        for name, more in ways.items():
            options = ["--no-scan", "--pointer-scan", "off", "--annotations", str(annotations), *more]
            project, export, tree = root / name, root / f"{name}.json", root / f"{name}-tree"
            run([str(args.gbrecomp), str(rom_path), *options, "-o", str(project), "--output-prefix", "pin"])
            run([str(args.gbrecomp), str(rom_path), *options, "--lift-export", str(export)])
            run([sys.executable, str(GB_DATA), "split", str(export), "--rom", str(rom_path), "-o", str(tree)])
            manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
            code = joined([(region["offset"], region["offset"] + region["size"])
                           for region in manifest["regions"] if region["kind"] != "data"])
            seen[name] = held(project)
            assert seen[name] == code, (name, [(hex(a), hex(b)) for a, b in seen[name]],
                                        [(hex(a), hex(b)) for a, b in code])
            # The declared bytes, and the byte in front of them that no instruction was decoded at, are data.
            for offset in (FAR, FAR + 1, FAR + DECLARED):
                region = next(region for region in manifest["regions"]
                              if region["offset"] <= offset < region["offset"] + region["size"])
                assert region["kind"] == "data", (name, hex(offset), region)
                assert not any(first <= offset < end for first, end in seen[name]), (name, hex(offset), seen[name])
        # The code is the code, whichever way it was asked: an entry point nothing was decoded at adds none.
        assert seen["asked"] == seen["plain"] and seen["arrived"] == seen["plain"], seen
        # And the cartridge header is held as ever.
        assert any(first <= 0x100 and end >= 0x150 for first, end in seen["plain"]), seen["plain"]

    print("what the game holds an image to is the tree's code: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
