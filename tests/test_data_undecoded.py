#!/usr/bin/env python3
"""An address the analysis looked at and decoded nothing at, end to end.

Code seems to go to an address, and what is there is no instruction: no
instruction has that opcode, or the instruction would take for its operand
bytes that were declared to be data (`--annotations`). The analysis decodes
nothing there. Such an address is no block of code: it has no end, no C is
made of it, the game holds no image (`--rom-image`) to its bytes, and a game
that does go there runs it in the interpreter.

Builds a small game of four banks with:

- a branch, never taken, to a byte no instruction starts with, the lowest
  address of its bank;
- one like it to a byte in front of a table, which an analysis not told of
  the table decodes with the table's first bytes for its operand;
- a routine that is called and one that is jumped to, each starting with an
  instruction of three bytes.

Then, for the analysis by itself, told of the table, and told wrongly that
the operands of the two routines' first instructions are data:

- the ranges the game holds an image to, in its `<prefix>_rom.c`, are the
  tree's files of code and its header file, to the byte;
- the game takes an image with every byte of every data file changed, and
  refuses one with the first or the last byte of any other file changed;
- told wrongly, the game ends as it does told right, in every register,
  counter and byte of RAM: the two routines are data to that analysis, and
  the interpreter runs them. A block made of such an address was a function
  with nothing in it, which the game called for ever.
"""
from __future__ import annotations

import argparse
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
START = 0x0150
WRONG, WRONG_TAIL, TINY, HOME_DATA = 0x0200, 0x0220, 0x0240, 0x0300
TRAP, TABLE, ENTRY = 0x4000, 0x4001, 0x4020       # in banks 2 and 3: the trap is the bank's lowest block
FAR = {1: 0x5000, 2: 0x6000}                       # where banks 1 and 2 end, above the next bank's trap
# A table to the game. To an analysis not told so, the operand of the byte in front and then instructions.
TABLE_BYTES = bytes([0x42, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x3C, 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x3D, 0x04, 0xC9])
PREFIX = "undecoded_game"
FRAMES = 12


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def make_rom() -> bytearray:
    rom = bytearray(4 * BANK)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13F] = b"UNDECODED  "
    rom[0x147], rom[0x148] = 0x01, 0x01                                     # MBC1, 64 KiB
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF

    code = [0xF3, 0x31, 0xFF, 0xDF]                                         # di / ld sp,$DFFF
    code += [0xFA, *word(HOME_DATA), 0xE0, 0x80]                            # ld a,[HomeData] / ldh [$80],a
    for bank in (1, 2, 3):
        code += [0x3E, bank, 0xEA, 0x00, 0x21, 0xCD, *word(ENTRY)]          # ld a,bank / ld [$2100],a / call Entry
    code += [0xCD, *word(TINY), 0xCD, *word(WRONG), 0xC3, *word(WRONG_TAIL)]  # call Tiny / call Wrong / jp WrongTail
    assert START + len(code) <= WRONG
    rom[START:START + len(code)] = bytes(code)
    rom[WRONG:WRONG + 7] = bytes([0x21, 0x34, 0x12, 0x7D, 0xE0, 0x87, 0xC9])  # ld hl,$1234 / ld a,l / ldh [$87],a / ret
    rom[WRONG_TAIL:WRONG_TAIL + 8] = bytes([0x01, 0x78, 0x56, 0x79, 0xE0, 0x88,  # ld bc,$5678 / ld a,c / ldh [$88],a
                                            0x18, 0xFE])                         # jr to itself
    rom[TINY:TINY + 2] = bytes([0xAF, 0xC9])                                # xor a / ret
    rom[HOME_DATA:HOME_DATA + 4] = bytes([0x5A, 0x5B, 0x5C, 0x5D])

    def banked(bank: int, address: int, content: list[int]) -> None:
        offset = bank * BANK + address - BANK
        rom[offset:offset + len(content)] = bytes(content)

    # ld a,n / or a / jr z,Trap: never taken, and the analysis cannot know.
    to_trap = [0x3E, 0x01, 0xB7, 0x28, (TRAP - (ENTRY + 5)) & 0xFF]
    banked(1, ENTRY, [0x3E, 0x11, 0xE0, 0x81, 0xC3, *word(FAR[1])])         # ld a,$11 / ldh [$81],a / jp Far
    banked(1, FAR[1], [0x3E, 0x12, 0xE0, 0x82, 0xC9])
    banked(2, TRAP, [0x21])                                                 # `ld hl,nn`, were the table its operand
    banked(2, TABLE, list(TABLE_BYTES))
    banked(2, ENTRY, to_trap + [0xFA, *word(TABLE), 0xE0, 0x83, 0xC3, *word(FAR[2])])
    banked(2, FAR[2], [0x3E, 0x22, 0xE0, 0x84, 0xC9])
    banked(3, TRAP, [0xD3, 0x66, 0x77, 0x88])                               # no instruction has this opcode
    banked(3, ENTRY, to_trap + [0xFA, *word(TRAP + 2), 0xE0, 0x85, 0xC9])
    return rom


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def pinned_by_the_game(project: Path, prefix: str) -> list[tuple[int, int]]:
    """The ranges the generator wrote beside the ROM: first offset, and the one after the last."""
    source = (project / f"{prefix}_rom.c").read_text(encoding="utf-8")
    found = re.search(r"const uint32_t (\w+)_pinned\[\] = \{([^}]*)\};\s*const size_t \1_pinned_pairs = (\d+);", source)
    assert found, "no pinned ranges beside the ROM"
    numbers = [int(number) for number in re.findall(r"(\d+)u", found.group(2))]
    pairs = list(zip(numbers[0::2], numbers[1::2]))
    assert len(pairs) == int(found.group(3)) and all(first < end for first, end in pairs), pairs
    assert all(pairs[index][1] < pairs[index + 1][0] for index in range(len(pairs) - 1)), pairs
    return pairs


def pinned_by_the_tree(manifest: dict) -> list[tuple[int, int]]:
    """The files `build` will not have changed, those that touch joined."""
    ranges: list[list[int]] = []
    for region in manifest["regions"]:
        if region["kind"] == "data":
            continue
        assert region["kind"] in ("code", "header"), region
        if ranges and ranges[-1][1] == region["offset"]:
            ranges[-1][1] += region["size"]
        else:
            ranges.append([region["offset"], region["offset"] + region["size"]])
    return [(first, end) for first, end in ranges]


def difference(game: list[tuple[int, int]], tree: list[tuple[int, int]]) -> str:
    def offsets(ranges: list[tuple[int, int]]) -> set[int]:
        return {offset for first, end in ranges for offset in range(first, end)}

    only_game, only_tree = offsets(game) - offsets(tree), offsets(tree) - offsets(game)
    said = []
    if only_game:
        said.append(f"{len(only_game)} bytes the game will not have changed are in data files, the first at "
                    f"offset {min(only_game):#x}")
    if only_tree:
        said.append(f"{len(only_tree)} bytes of the tree's code and header the game does not keep, the first at "
                    f"offset {min(only_tree):#x}")
    return "; ".join(said)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="gbundecoded-") as scratch:
        root = Path(scratch)
        rom = make_rom()
        rom_path = root / "undecoded.gb"
        rom_path.write_bytes(rom)
        right = f"data 02:{TABLE:04X} {len(TABLE_BYTES)} ; tiles\n"
        (root / "told.annotations").write_text(right, encoding="utf-8")
        # The operand of an instruction the game runs, called data: at a function, and at a jump's target.
        (root / "wrong.annotations").write_text(
            right + f"data 00:{WRONG + 1:04X} 2\ndata 00:{WRONG_TAIL + 1:04X} 2\n", encoding="utf-8")

        def analysed(name: str) -> tuple[dict, dict, Path]:
            """The export, the tree and the game's source of one analysis: the same options to both."""
            options = ["--no-scan", "--pointer-scan", "off"]
            if name != "plain":
                options += ["--annotations", str(root / f"{name}.annotations")]
            run([str(args.gbrecomp), str(rom_path), *options, "--lift-export", str(root / f"{name}.json")])
            run([str(args.gbrecomp), str(rom_path), *options, "-o", str(root / name), "--output-prefix", PREFIX])
            run([sys.executable, str(GB_DATA), "split", str(root / f"{name}.json"), "--rom", str(rom_path),
                 "-o", str(root / f"{name}-tree")])
            return (json.loads((root / f"{name}.json").read_text(encoding="utf-8")),
                    json.loads((root / f"{name}-tree" / "manifest.json").read_text(encoding="utf-8")), root / name)

        def region_of(manifest: dict, offset: int) -> dict:
            return next(region for region in manifest["regions"]
                        if region["offset"] <= offset < region["offset"] + region["size"])

        trees: dict[str, dict] = {}
        for name in ("plain", "told", "wrong"):
            export, manifest, project = analysed(name)
            trees[name] = manifest
            # The game is the one this test means: every bank's function was found, and the traps were looked at.
            functions = {(function["bank"], function["address"]) for function in export["functions"]}
            assert {(1, ENTRY), (2, ENTRY), (3, ENTRY), (0, TINY)} <= functions, sorted(functions)
            assert region_of(manifest, 3 * BANK)["kind"] == "data", name
            assert region_of(manifest, 2 * BANK)["kind"] == ("code" if name == "plain" else "data"), name
            assert region_of(manifest, WRONG)["kind"] == ("data" if name == "wrong" else "code"), name
            game, tree = pinned_by_the_game(project, PREFIX), pinned_by_the_tree(manifest)
            assert game == tree, f"{name}: {difference(game, tree)}"
            assert any(first <= 0x100 and end >= 0x150 for first, end in game), game

        def built(name: str) -> Path:
            project = root / name
            run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
                 "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
            run(["ninja", "-C", str(project / "build")])
            return next(path for path in (project / "build" / f"{PREFIX}.exe", project / "build" / PREFIX)
                        if path.exists())

        def play(executable: Path, name: str, *flags: str, expect: tuple[int, ...] = (0,)) -> tuple[dict, str]:
            saves = root / f"saves-{name}"
            saves.mkdir(exist_ok=True)
            dump = root / f"{name}.state.json"
            try:
                result = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES),
                              "--save-dir", str(saves), "--dump-state", str(dump), *flags], expect, timeout=60)
            except subprocess.TimeoutExpired:
                raise AssertionError(f"the game of {name} did not end") from None
            state = json.loads(dump.read_text(encoding="utf-8")) if dump.exists() else {}
            return state, result.stdout + result.stderr

        # The game, asked: it takes every data file changed, and no other file touched at either end.
        told_game = built("told")
        told, _ = play(told_game, "told")
        assert told["hram_ff80_fffe"][:9] == [0x5A, 0x11, 0x12, TABLE_BYTES[0], 0x22, 0x77, 0x00, 0x34, 0x78], \
            told["hram_ff80_fffe"][:9]
        image = bytearray(rom)
        changed = 0
        for region in trees["told"]["regions"]:
            if region["kind"] == "data":
                for offset in range(region["offset"], region["offset"] + region["size"]):
                    image[offset] ^= 0xFF
                changed += region["size"]
        (root / "all-data.gb").write_bytes(image)
        state, log = play(told_game, "all-data", "--rom-image", str(root / "all-data.gb"))
        assert f"{changed} bytes differ from the ROM this game was built from" in log, log
        assert state["hram_ff80_fffe"][:6] == [0xA5, 0x11, 0x12, TABLE_BYTES[0] ^ 0xFF, 0x22, 0x88], \
            state["hram_ff80_fffe"][:6]
        edges = sorted({edge for first, end in pinned_by_the_tree(trees["told"]) for edge in (first, end - 1)})
        assert len(edges) >= 12, edges
        for offset in edges:
            image = bytearray(rom)
            image[offset] ^= 0xFF
            (root / "edge.gb").write_bytes(image)
            _, log = play(told_game, "edge", "--rom-image", str(root / "edge.gb"), expect=(1,))
            assert f"the byte at offset 0x{offset:05X} is not the one this game's code" in log, (hex(offset), log)

        # Told wrongly, the game is still the game: the two routines are data to this analysis (as was
        # seen above), no C was made of them, and the interpreter runs them to the same end.
        wrong, _ = play(built("wrong"), "wrong")
        differ = [key for key in told if key != "dispatch_fallbacks" and told[key] != wrong.get(key)]
        assert not differ, differ

    print("data undecoded end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
