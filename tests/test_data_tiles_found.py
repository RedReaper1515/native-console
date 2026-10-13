#!/usr/bin/env python3
"""Pictures of a game's tiles, found from where the game put them, end to end.

Builds a small game that copies tiles out of its ROM every way the recorder
tells apart, and some things that are not tiles:

- A: three tiles at 2 bits a pixel, by a loop told where they are by a
  constant; and the second of them again by itself, which begins no picture;
- F: a font of four tiles at 1 bit a pixel, each byte written twice; and O,
  two tiles at 1 bit a pixel written to every other address, the odd ones;
- J: a tile at 2 bits a pixel right after the font, read at an address worked
  out from a word of the ROM: it lies where a picture could go on, and is
  not in step with the font;
- P: two tiles read with POP, the stack pointer in the ROM; and Q, two more,
  with the stack pointer loaded from a word of a table;
- T: a sheet of four tiles in two halves, the first read at the address a
  table has and the second at that address plus 32; and K after it, read at
  that address plus 64 and landing half way down a tile;
- U and V: two sheets one after the other, each told by a word of the table,
  V also read once at where U is and 32, and a copy of both in one go that
  runs on from U into V;
- W: two tiles, copied with the sixteen bytes after them, which the game
  also writes to a tile map;
- X1 and X2: two tiles with sixteen bytes between them that nothing reads;
  and Z, a tile sixteen bytes further on, read at where X1 is and 64: it is
  in step with X2, and does not lie where X2's picture could go on;
- M: sixteen bytes that land at two different places in a tile;
- R: twenty-eight bytes that land eight bytes into a tile;
- Y: twenty-four bytes across the end of the home bank, the last sixteen a
  tile at the very start of the next bank's data;
- sixteen bytes of the game's own code, two bytes side by side that each
  land by themselves, one that reaches the tile area with one added to it,
  and four that go to a tile map and nowhere else.

A game run with `--data-use FILE --data-use-tiles` has to end as it does
without, and its record has to have every one of those as a run
(`tile_runs`) and otherwise be exactly the record made without the option,
which has what POP read in it too. `gb_data.py uses` has to write the runs
into the tree and `tiles DIR` draw the sixteen pictures they make and no
other, at the place, the count and the bits a pixel a person would have told
it, list them, and put them on one sheet. Then: a second run changes nothing; a view
told by hand and the strings of a file of text keep their bytes; a picture
that has been edited is left alone, and changes exactly its own bytes of the
image; a picture the runs no longer make is taken away unless it was edited;
`--force` draws afresh; unedited, the tree is the ROM throughout; a record
made without the option, alone or among others, is accepted; and where two
records say different things of a byte, the stronger is kept.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

from fixtures.make_mapper_rom import NINTENDO_LOGO
from test_data_tiles import grey_png, read_grey, repaint, sheet

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"

START = 0x0150
TABLE = 0x0F00                      # words: T, U, V, F, Y, Q, X1
A, F, J, P, T, K, U, V, W, MAP = 0x1000, 0x1100, 0x1120, 0x1200, 0x1300, 0x1340, 0x1400, 0x1420, 0x1500, 0x1520
X1, X2, M, R, O, ALONE, ADDED = 0x1600, 0x1620, 0x1700, 0x1800, 0x1900, 0x1A00, 0x1A10
Q, Y, NEXT_BANK, Z, MAP_ONLY = 0x1240, 0x3FF8, 0x4000, 0x1640, 0x1A20
FRAMES = 20
SCALE = 2                           # how many times across the sheet draws a pixel


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def loop(target: int, count: int) -> list[int]:
    """ld de,target / ld b,count / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop"""
    return [0x11, *word(target), 0x06, count, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]


def copy(source: int, target: int, count: int) -> list[int]:
    """The same from an address written out in the code."""
    return [0x21, *word(source), *loop(target, count)]


def copy_at(pointer: int, target: int, count: int, add: int = 0) -> list[int]:
    """The same from the address a word of the ROM has, or that address and `add`:
    ld a,[pointer] / (add a,add) / ld l,a / ld a,[pointer+1] / (adc a,0) / ld h,a"""
    more, carry = ([0xC6, add], [0xCE, 0x00]) if add else ([], [])
    return [0xFA, *word(pointer), *more, 0x6F, 0xFA, *word(pointer + 1), *carry, 0x67, *loop(target, count)]


def make_rom() -> tuple[bytearray, int, int]:
    """The ROM, and the addresses of the instructions that point the stack pointer at P and at Q."""
    import random
    noise = random.Random(1996)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13E] = b"TILES SEEN"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    # Whatever comes, so that the data is one region and no two tiles are alike.
    rom[0x0800:0x8000] = bytes(noise.randrange(1, 256) for _ in range(0x7800))
    for at, value in enumerate([T, U, V, F, Y, Q, X1]):
        rom[TABLE + 2 * at:TABLE + 2 * at + 2] = bytes(word(value))
    # The screen stays on: the frames the test runs for are counted by it.
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF
    code += copy(A, 0x8000, 48)
    code += copy(A + 16, 0x8F00, 16)
    # ld de,F / ld hl,$8800 / ld b,32 / loop: ld a,[de] / inc de / ld [hli],a / ld [hli],a / dec b / jr nz,loop
    code += [0x11, *word(F), 0x21, *word(0x8800), 0x06, 32, 0x1A, 0x13, 0x22, 0x22, 0x05, 0x20, 0xF9]
    code += copy_at(TABLE + 6, 0x8B00, 16, add=0x20)                       # J, by where F is and 32
    # ld sp,P / ld hl,$9000 / ld b,16 / loop: pop de / ld [hl],e / inc l / ld [hl],d / inc l / dec b / jr nz,loop
    pointed = START + len(code)
    code += [0x31, *word(P), 0x21, *word(0x9000), 0x06, 16, 0xD1, 0x73, 0x2C, 0x72, 0x2C, 0x05, 0x20, 0xF8]
    # The same with the stack pointer from the table: ld a,[..] / ld l,a / ld a,[..] / ld h,a / ld sp,hl
    code += [0xFA, *word(TABLE + 10), 0x6F, 0xFA, *word(TABLE + 11), 0x67]
    pointed_q = START + len(code)
    code += [0xF9, 0x21, *word(0x9100), 0x06, 16, 0xD1, 0x73, 0x2C, 0x72, 0x2C, 0x05, 0x20, 0xF8]
    code += [0x31, 0xFF, 0xDF]                                             # ld sp,$DFFF
    code += copy_at(TABLE, 0x8100, 32)
    code += copy_at(TABLE, 0x8900, 32, add=0x20)
    code += copy_at(TABLE, 0x8D08, 24, add=0x40)                           # K
    code += copy_at(TABLE + 2, 0x8200, 32)
    code += copy_at(TABLE + 4, 0x8A00, 32)
    code += copy_at(TABLE + 2, 0x8A40, 32, add=0x20)                       # V again, by where U is and 32
    code += copy_at(TABLE + 2, 0x8400, 64)                                 # U, and on into V
    code += copy(W, 0x8500, 48)
    code += copy(MAP, 0x9800, 16)
    code += copy(X1, 0x8600, 16)
    code += copy(X2, 0x8700, 16)
    code += copy_at(TABLE + 12, 0x8780, 16, add=0x40)                      # Z, by where X1 is and 64
    code += copy(M, 0x8300, 16)
    code += copy(M, 0x8303, 16)
    code += copy(R, 0x8308, 28)
    # ld de,O / ld hl,$8C00 / ld b,16 / loop: xor a / ld [hli],a / ld a,[de] / inc de / ld [hli],a / dec b / jr nz
    code += [0x11, *word(O), 0x21, *word(0x8C00), 0x06, 16, 0xAF, 0x22, 0x1A, 0x13, 0x22, 0x05, 0x20, 0xF8]
    code += copy_at(TABLE + 8, 0x8F18, 24)                                 # Y, across the end of the bank
    code += copy(START, 0x8E00, 16)
    code += [0xFA, *word(ALONE), 0xEA, *word(0x8FF0)]                      # ld a,[Alone] / ld [$8FF0],a
    code += [0xFA, *word(ALONE + 1), 0xEA, *word(0x8FD3)]                  # and the byte after it, elsewhere
    code += [0xFA, *word(ADDED), 0x3C, 0xEA, *word(0x8FE0)]                # ld a,[Added] / inc a / ld [$8FE0],a
    code += copy(MAP_ONLY, 0x9904, 4)
    code += [0x3E, 0x01, 0xE0, 0x87, 0x18, 0xFE]                           # ld a,1 / ldh [$87],a / jr to itself
    assert START + len(code) <= 0x0800
    rom[START:START + len(code)] = bytes(code)
    return rom, pointed, pointed_q


# What the record has to say of where bytes landed, and the pictures that makes.
RUNS = [
    (START, 16, 1, 0x8E00, "unknown"), (A, 48, 1, 0x8000, "unknown"), (F, 32, 2, 0x8800, "unknown"),
    (J, 16, 1, 0x8B00, "worked_out"), (P, 32, 1, 0x9000, "unknown"), (Q, 32, 1, 0x9100, "told"),
    (T, 32, 1, 0x8100, "told"),
    (T + 32, 32, 1, 0x8900, "worked_out"), (K, 24, 1, 0x8D08, "worked_out"), (U, 32, 1, 0x8400, "told"),
    (V, 32, 1, 0x8420, "told"), (W, 48, 1, 0x8500, "unknown"), (X1, 16, 1, 0x8600, "unknown"),
    (X2, 16, 1, 0x8700, "unknown"), (Z, 16, 1, 0x8780, "worked_out"), (M, 16, None, None, None),
    (R, 28, 1, 0x8308, "unknown"),
    (O, 16, 2, 0x8C01, "unknown"), (ALONE, 1, 0, 0x8FF0, "unknown"),
    (ALONE + 1, 1, 0, 0x8FD3, "unknown"), (Y, 8, 1, 0x8F18, "told"),
    (NEXT_BANK, 16, 1, 0x8F20, "worked_out"),
]
# The pictures in the home bank's data. There is one more, of the tile the next bank's data starts with.
PICTURES = [(A, 3, 2), (F, 4, 1), (J, 1, 2), (P, 2, 2), (Q, 2, 2), (T, 4, 2), (K + 8, 1, 2), (U, 2, 2), (V, 2, 2),
            (W, 2, 2), (X1, 1, 2), (X2, 1, 2), (Z, 1, 2), (R + 8, 1, 2), (O, 2, 1)]
ALL = len(PICTURES) + 1
# Between two runs of tiles, the bytes nothing was seen to use. Not before R, nor after O: M and
# the byte by itself lie there, and were seen.
GAPS = [(A + 48, F), (J + 16, P), (P + 32, Q), (Q + 32, T), (K + 24, U), (V + 32, W), (W + 48, X1), (X1 + 16, X2),
        (X2 + 16, Z), (R + 28, O)]


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
    done = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES), "--save-dir", str(saves),
                "--dump-state", str(dump), *flags], expect, timeout=180)
    return (json.loads(dump.read_text(encoding="utf-8")) if dump.exists() else {}), done.stdout + done.stderr


def greys(data: bytes, bpp: int = 2) -> list[bytes]:
    """The rows of a picture of tiles as the plain greys of its colour numbers. `tiles` draws a
    picture in greys, or, where a record says what the screen showed of its tiles, as an indexed
    picture whose pixels are the colour numbers and whose palette is the game's."""
    width, height, depth, colour = struct.unpack(">IIBB", data[16:26])
    if colour != 3:
        return read_grey(data)
    packed, position = b"", 8
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        if kind == b"IDAT":
            packed += data[position + 8:position + 8 + length]
        position += 12 + length
    raw = zlib.decompress(packed)
    stride = (width * depth + 7) // 8
    assert len(raw) == height * (stride + 1) and all(raw[y * (stride + 1)] == 0 for y in range(height))
    shades = {2: (255, 170, 85, 0), 1: (255, 0)}[bpp]
    rows = []
    for y in range(height):
        line = raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        rows.append(bytes(shades[(line[x * depth >> 3] >> (8 - depth - (x * depth & 7))) & ((1 << depth) - 1)]
                          for x in range(width)))
    return rows


def on_sheet(rows: list[bytes], lines: list[bytes]) -> bool:
    """Whether a picture is somewhere on the sheet, each of its pixels SCALE across."""
    big = [bytes(level for level in line for _ in range(SCALE)) for line in lines for _ in range(SCALE)]
    for top in range(len(rows) - len(big) + 1):
        left = rows[top].find(big[0])
        while left >= 0:
            if all(rows[top + y][left:left + len(big[y])] == big[y] for y in range(len(big))):
                return True
            left = rows[top].find(big[0], left + 1)
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]

    with tempfile.TemporaryDirectory(prefix="gbfound-") as scratch:
        root = Path(scratch)
        rom, pointed, pointed_q = make_rom()
        rom_path = root / "found.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        run([str(args.gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "found_game"])
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
             "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
        run(["ninja", "-C", str(project / "build")])
        executable = next(path for path in (project / "build" / "found_game.exe", project / "build" / "found_game")
                          if path.exists())

        # Recording changes nothing about what the game does, with the option and without.
        plain, _ = play(executable, root, "plain")
        assert plain["hram_ff80_fffe"][7] == 1, plain["hram_ff80_fffe"][:8]
        without_path, with_path = root / "use-without.json", root / "use-with.json"
        without, _ = play(executable, root, "without", "--data-use", str(without_path))
        recorded, _ = play(executable, root, "recorded", "--data-use", str(with_path), "--data-use-tiles")
        for other in (without, recorded):
            differ = [key for key in plain if key != "dispatch_fallbacks" and plain[key] != other.get(key)]
            assert not differ, differ
        _, log = play(executable, root, "alone", "--data-use-tiles", expect=(1,))
        assert "--data-use-tiles needs --data-use FILE" in log, log

        # The record: exactly what it is without the option, and where bytes landed. Nothing else.
        before = json.loads(without_path.read_text(encoding="utf-8"))
        record = json.loads(with_path.read_text(encoding="utf-8"))
        assert "tile_runs" not in before and list(record)[-1] == "tile_runs", list(record)
        assert {key: value for key, value in record.items() if key != "tile_runs"} == before
        # What POP read is in it either way, put down to the instruction that pointed the stack pointer there.
        for sheet_at, reader in ((P, pointed), (Q, pointed_q)):
            assert next(entry for entry in before["ranges"] if entry["offset"] == sheet_at) == {
                "offset": sheet_at, "size": 32, "bank": 0, "address": sheet_at, "as_it_is": ["tiles"],
                "worked_out": [], "read_at": [0, reader], "one_reader": True}
        added = next(entry for entry in record["ranges"] if entry["offset"] == ADDED)
        assert (added["as_it_is"], added["worked_out"]) == ([], ["tiles"]), added

        def place(offset: int) -> dict:
            return {"offset": offset, "bank": offset // 0x4000, "address": offset}

        expected = [dict(place(offset), size=size, mixed=True) if step is None else
                    dict(place(offset), size=size, step=step, to=to, begun=begun)
                    for offset, size, step, to, begun in RUNS]
        assert record["tile_runs"] == expected, [run for run in record["tile_runs"] if run not in expected]

        # Into the tree.
        tree = root / "tree"
        gb_data("split", str(export_path), "--rom", str(rom_path), "-o", str(tree))
        image = root / "image.gb"

        def manifest(directory: Path = tree) -> dict:
            return json.loads((directory / "manifest.json").read_text(encoding="utf-8"))

        def region_of(offset: int, directory: Path = tree) -> dict:
            return next(region for region in manifest(directory)["regions"]
                        if region["offset"] <= offset < region["offset"] + region["size"])

        def original() -> None:
            said = gb_data("build", str(tree), "-o", str(image), "--expect-original").stdout
            assert "the original ROM, byte for byte" in said, said
            assert image.read_bytes() == bytes(rom)

        def changed() -> dict[int, int]:
            gb_data("build", str(tree), "-o", str(image))
            made = image.read_bytes()
            return {offset: made[offset] for offset in range(len(rom)) if made[offset] != rom[offset]}

        def refused(*arguments: str) -> str:
            return gb_data(*arguments, expect=(2,)).stderr

        def files() -> dict[Path, bytes]:
            return {path: path.read_bytes() for path in tree.rglob("*") if path.is_file()}

        data, code, next_bank = region_of(A), region_of(START), region_of(NEXT_BANK)
        assert (data["kind"], code["kind"], next_bank["kind"]) == ("data", "code", "data")
        assert data["offset"] <= TABLE and data["offset"] + data["size"] == NEXT_BANK == next_bank["offset"], data
        base, stem = data["offset"], data["file"][:-len(".bin")]
        beyond = next_bank["file"][:-len(".bin")] + ".tiles.png"       # the picture at the very start of a file

        def name(offset: int) -> str:
            return f"{stem}.tiles_{offset:04X}.png"

        def runs_of(region: dict, runs: list[tuple]) -> list[dict]:
            start, end = region["offset"], region["offset"] + region["size"]
            return [{"offset": offset - start, "size": size, "mixed": True} if step is None else
                    {"offset": offset - start, "size": size, "step": step, "to": to, "begun": begun}
                    for offset, size, step, to, begun in runs if start <= offset < end]

        # A record made without the option says nothing of where tiles are, and is taken as before.
        said = gb_data("uses", str(tree), str(without_path)).stdout
        assert "landed" not in said and "tile_runs" not in json.dumps(manifest()), said
        said = refused("tiles", str(tree))
        assert "nothing says yet where the game puts its tiles" in said and "--data-use-tiles" in said, said
        plain_seen = region_of(A)["seen"]

        said = gb_data("uses", str(tree), str(with_path)).stdout
        landed = sum(size for _, size, *_ in RUNS)
        assert f"{landed} bytes landed in the tile area as they are, in {len(RUNS)} runs" in said, said
        assert "made without" not in said, said
        for region in (data, code, next_bank):
            assert region_of(region["offset"])["tile_runs"] == runs_of(region, RUNS), region["file"]
        # What was seen of the bytes is what it was.
        assert region_of(A)["seen"] == plain_seen
        original()

        # No file named: where the tiles are comes from what was seen, and nothing else may be said.
        assert "go with a file" in refused("tiles", str(tree), "--offset", "16")
        assert "go with a file" in refused("tiles", str(tree), "--tiles", "2")
        assert "go with a file" in refused("tiles", str(tree), "--bpp", "1")
        assert "at least one tile" in refused("tiles", str(tree), "--width", "0")
        assert not list(tree.rglob("*.png")) and "views" not in json.dumps(manifest())

        def check_views(pictures: list[tuple[int, int, int]], width: int = 16) -> list[dict]:
            views = region_of(A)["views"]
            assert [view["file"] for view in views] == [name(offset) for offset, _, _ in pictures], views
            for view, (offset, tiles, bpp) in zip(views, pictures):
                size = tiles * 8 * bpp
                path = tree / view["file"]
                assert view == {"kind": "tiles", "file": view["file"], "offset": offset - base, "tiles": tiles,
                                "width": min(width, tiles), "bpp": bpp, "from": "seen",
                                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                "data_sha256": hashlib.sha256(rom[offset:offset + size]).hexdigest()}, view
                assert greys(path.read_bytes(), bpp) == sheet(bytes(rom[offset:offset + size]), bpp, min(width, tiles))
            return views

        def drew(said: str, pictures: int, wrote: int) -> None:
            assert f"{pictures} pictures of the tiles the game was seen to draw: " in said and \
                f"({wrote} written, {pictures - wrote} there already and the same)" in said, said

        said = gb_data("tiles", str(tree)).stdout
        tiles = sum(count for _, count, _ in PICTURES) + 1
        size = sum(count * 8 * bpp for _, count, bpp in PICTURES) + 16
        drew(said, ALL, ALL)
        assert f"{ALL} pictures of the tiles the game was seen to draw: {tiles} tiles, {size} bytes (" in said, said
        assert "14 pictures at 2 bits a pixel, 24 tiles" in said and "2 pictures at 1 bit a pixel, 6 tiles" in said
        gap = sum(end - start for start, end in GAPS)
        assert f"{len(GAPS)} stretches between two runs of tiles in one file, {gap:,} bytes, are in no record" in said
        assert "62 bytes that landed in the tile area are in no picture: 28 at the ends of a run that are not a " \
            "whole tile, 16 from the first byte of a run that the game also used for something else, 16 that " \
            "landed at more than one place in a tile, 2 that landed alone" in said, said
        assert "16 bytes that landed in the tile area as tiles are in 1 file of code, which cannot be drawn: " \
            "`annotations` writes them" in said and f"00:{START:04X}    16 bytes in {code['file']}" in said, said
        assert f"the {ALL} pictures of tiles in the tree, to look through" in said, said
        assert "told `tiles` of" not in said and "edited" not in said and "taken away" not in said, said
        assert "do not say where" not in said, said
        views = check_views(PICTURES)
        beyond_png = tree / beyond
        assert region_of(NEXT_BANK)["views"] == [{
            "kind": "tiles", "file": beyond, "offset": 0, "tiles": 1, "width": 1, "bpp": 2, "from": "seen",
            "sha256": hashlib.sha256(beyond_png.read_bytes()).hexdigest(),
            "data_sha256": hashlib.sha256(rom[NEXT_BANK:NEXT_BANK + 16]).hexdigest()}]
        assert greys(beyond_png.read_bytes()) == sheet(bytes(rom[NEXT_BANK:NEXT_BANK + 16]), 2, 1)
        assert "views" not in region_of(START)
        original()
        assert f"and {ALL} pictures of tiles" in gb_data("status", str(tree)).stdout

        # Something to look through: a line for each picture, and all of them on one sheet.
        listed = [line for line in (tree / "tiles.txt").read_text(encoding="utf-8").splitlines()
                  if line and not line.startswith("#")]
        assert listed == [f"00:{offset:04X}  {count:4d} tiles  {bpp} bpp  {min(count, 16) * 8:3d}x"
                          f"{-(-count // 16) * 8:<4d} {name(offset)}" for offset, count, bpp in PICTURES] + \
            [f"01:4000     1 tiles  2 bpp    8x8    {beyond}"], listed
        rows = read_grey((tree / "tiles.png").read_bytes())
        for view in views + region_of(NEXT_BANK)["views"]:
            assert on_sheet(rows, greys((tree / view["file"]).read_bytes())), view["file"]
        assert not on_sheet(rows, sheet(bytes(rom[M:M + 16]), 2, 1))

        # Again: nothing changes.
        kept = files()

        def restore() -> None:
            for path in tree.rglob("*.png"):
                if path not in kept:
                    path.unlink()
            for path, content in kept.items():
                path.write_bytes(content)

        drew(gb_data("tiles", str(tree)).stdout, ALL, 0)
        assert files() == kept
        # A picture that is gone is drawn again.
        (tree / views[0]["file"]).unlink()
        drew(gb_data("tiles", str(tree)).stdout, ALL, 1)
        assert files() == kept
        # Fewer to a row is another picture of the same tiles: of A, F and T, which have more than two.
        said = gb_data("tiles", str(tree), "--width", "2").stdout
        drew(said, ALL, 3)
        assert "taken away" not in said, said
        check_views(PICTURES, width=2)
        gb_data("tiles", str(tree))
        assert files() == kept

        # An edited picture is left alone, and changes exactly its own bytes of the image.
        t_png = tree / name(T)
        at_t = [offset for offset, _, _ in PICTURES].index(T)
        drawn = greys(t_png.read_bytes())
        low = T + 2 * 16 + 2 * 3                                # the third tile's fourth row, its low bits
        was = (rom[low] >> 7 & 1) + 2 * (rom[low + 1] >> 7 & 1)
        edit = grey_png(repaint(drawn, 16, 3, 0 if was != 3 else 255))
        t_png.write_bytes(edit)
        expected_edit = {low: rom[low] | 0x80, low + 1: rom[low + 1] | 0x80} if was != 3 else \
            {low: rom[low] & 0x7F, low + 1: rom[low + 1] & 0x7F}
        expected_edit = {offset: value for offset, value in expected_edit.items() if value != rom[offset]}
        assert changed() == expected_edit and len(expected_edit) in (1, 2), (changed(), expected_edit)
        said = gb_data("tiles", str(tree)).stdout
        drew(said, ALL - 1, 0)
        assert "1 more has been edited, and is left as it is" in said, said
        assert t_png.read_bytes() == edit and region_of(A)["views"] == views
        assert changed() == expected_edit
        said = gb_data("status", str(tree)).stdout
        assert said.count("edited: ") == 1 and name(T) in said, said

        # What the runs no longer make: an unedited picture goes, and an edited one stays with its bytes.
        good = (tree / "manifest.json").read_text(encoding="utf-8")

        def with_runs(change) -> None:
            altered = json.loads(good)
            target = next(region for region in altered["regions"] if region["file"] == data["file"])
            change(target["tile_runs"])
            (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")

        def shorter(runs: list[dict]) -> None:
            # The second half of T is not seen any more, and A is seen to be two tiles.
            runs[:] = [run for run in runs if run["offset"] != T + 32 - base]
            next(run for run in runs if run["offset"] == A - base)["size"] = 32

        with_runs(shorter)
        said = gb_data("tiles", str(tree)).stdout
        assert "1 picture that has been edited is not what the runs make any more, and is left with the bytes it " \
            "has: --force draws afresh" in said and name(T) in said, said
        drew(said, ALL - 1, 1)
        assert t_png.read_bytes() == edit
        now = region_of(A)["views"]
        assert now[at_t] == views[at_t] and now[0]["tiles"] == 2 and len(now) == len(PICTURES)
        assert changed() == expected_edit
        # With --force the edit is lost, and the picture is what the runs make now: T's first half.
        said = gb_data("tiles", str(tree), "--force").stdout
        drew(said, ALL, 1)
        assert "edited" not in said, said
        assert region_of(A)["views"][at_t]["tiles"] == 2 and greys(t_png.read_bytes()) == \
            sheet(bytes(rom[T:T + 32]), 2, 2)
        original()
        # A run that is gone altogether takes its picture with it.
        restore()
        with_runs(lambda runs: runs.remove(next(run for run in runs if run["offset"] == X2 - base)))
        said = gb_data("tiles", str(tree)).stdout
        drew(said, ALL - 1, 0)
        assert "1 picture that the runs no longer make was taken away" in said, said
        assert not (tree / name(X2)).exists() and len(region_of(A)["views"]) == len(PICTURES) - 1
        restore()
        gb_data("tiles", str(tree))
        assert files() == kept

        # Told by hand. A picture found from what was seen makes room for one somebody tells of, unless
        # it has been edited; and from then on what the runs make of those bytes is not drawn.
        u_png, v_png = tree / name(U), tree / name(V)
        both = ("tiles", str(tree), data["file"], "--offset", str(U - base), "--tiles", "4", "--width", "4")
        v_png.write_bytes(grey_png(repaint(greys(v_png.read_bytes()), 0, 0, 255 if rom[V] & 0x80 else 0)))
        said = refused(*both)
        assert "are already drawn in" in said and name(V) in said, said
        assert u_png.read_bytes() == kept[u_png] and v_png.exists() and region_of(A)["views"] == views
        v_png.write_bytes(kept[v_png])
        gb_data(*both)
        gb_data("tiles", str(tree), data["file"], "--offset", str(P - base), "--tiles", "2")
        assert not v_png.exists()
        by_hand = [view for view in region_of(A)["views"] if "from" not in view]
        assert [(view["offset"] + base, view["tiles"], view["width"]) for view in by_hand] == [(P, 2, 2), (U, 4, 4)]
        assert len(region_of(A)["views"]) == len(PICTURES) - 1
        said = gb_data("tiles", str(tree)).stdout
        drew(said, ALL - 3, 0)
        assert f"1 more is a picture somebody told `tiles` of already, of the same tiles: {name(P)}" in said, said
        assert "2 runs of tiles are not drawn: a picture somebody told `tiles` of has bytes of each" in said, said
        for offset in (U, V):
            assert f"2 tiles at 00:{offset:04X}: {name(U)} has bytes of them" in said, said
        assert "taken away" not in said, said
        after = region_of(A)["views"]
        assert [view for view in after if "from" not in view] == by_hand and len(after) == len(PICTURES) - 1
        assert not v_png.exists() and greys(u_png.read_bytes()) == sheet(bytes(rom[U:U + 64]), 2, 4)
        assert f"the {ALL - 1} pictures of tiles in the tree" in said
        assert f"{name(U)}   told by hand" in (tree / "tiles.txt").read_text(encoding="utf-8")
        original()
        # And a file of text: its strings keep their bytes. `text` leaves a picture's bytes alone, so the
        # strings are there first: the picture of X1 is taken out of the tree by hand.
        text = f"{stem}.text.txt"
        (tree / text).write_bytes(b"as it was written\n")
        altered = manifest()
        target = next(region for region in altered["regions"] if region["file"] == data["file"])
        target["views"] = sorted([view for view in target["views"] if view["offset"] != X1 - base] + [{
            "kind": "text", "file": text, "charmap": "charmap.json", "strings": [[W + 40 - base, 4], [X1 + 3 - base, 2]],
            "sha256": hashlib.sha256(b"as it was written\n").hexdigest(),
            "data_sha256": hashlib.sha256(bytes(rom[W + 40:W + 44] + rom[X1 + 3:X1 + 5])).hexdigest()}],
            key=lambda view: view["strings"][0][0] if view["kind"] == "text" else view["offset"])
        (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
        (tree / name(X1)).unlink()
        said = gb_data("tiles", str(tree)).stdout
        drew(said, ALL - 4, 0)
        assert "1 run of tiles is not drawn: a file of text has strings in each" in said, said
        assert f"1 tiles at 00:{X1:04X}: {text} has bytes of them" in said, said
        after = region_of(A)["views"]
        assert len(after) == len(PICTURES) - 1 and not (tree / name(X1)).exists()
        assert (tree / name(W)).exists()                        # W's two tiles end before the string after them
        original()

        # A file in the way that is nobody's is not written over, and nothing is written before it is seen.
        fresh = root / "fresh"
        gb_data("split", str(export_path), "--rom", str(rom_path), "-o", str(fresh))
        gb_data("uses", str(fresh), str(with_path))
        stray = fresh / name(O)
        stray.write_bytes(b"mine")
        said = refused("tiles", str(fresh))
        assert "is already there and the manifest does not know it: --force writes over it" in said, said
        assert stray.read_bytes() == b"mine" and len(list(fresh.rglob("*.png"))) == 1
        assert "views" not in (fresh / "manifest.json").read_text(encoding="utf-8")
        drew(gb_data("tiles", str(fresh), "--force").stdout, ALL, ALL)
        # Runs this tool did not write are not drawn from.
        good = (fresh / "manifest.json").read_text(encoding="utf-8")
        for change, words in ((lambda run: run.update(step=3), "is not one `uses` wrote"),
                              (lambda run: run.update(begun="guessed"), "is not one `uses` wrote"),
                              (lambda run: run.update(size=0x8000), "are not inside it, one after another"),
                              (lambda run: run.pop("to"), "is not one `uses` wrote")):
            altered = json.loads(good)
            target = next(region for region in altered["regions"] if region["file"] == data["file"])
            change(target["tile_runs"][2])
            (fresh / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
            assert words in refused("tiles", str(fresh)), words
        altered = json.loads(good)
        next(region for region in altered["regions"] if region["file"] == data["file"])["views"][0]["from"] = "me"
        (fresh / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
        assert "`from` can only be 'seen'" in refused("status", str(fresh))
        (fresh / "manifest.json").write_text(good, encoding="utf-8", newline="\n")

        # Records add up. One made without the option among them says so, and takes nothing away.
        def added_up(*records: Path) -> tuple[list[dict], str]:
            said = gb_data("uses", str(fresh), *map(str, records)).stdout
            return region_of(A, fresh)["tile_runs"], said

        def altered_record(file: str, change) -> Path:
            other = json.loads(with_path.read_text(encoding="utf-8"))
            change(other)
            (root / file).write_text(json.dumps(other), encoding="utf-8")
            return root / file

        def run_at(record: dict, offset: int) -> dict:
            return next(run for run in record["tile_runs"] if run["offset"] == offset)

        runs = runs_of(data, RUNS)
        got, said = added_up(with_path, without_path)
        assert "1 of the records was made without --data-use-tiles, and does not say where" in said and got == runs
        # Where two records put a byte at different places in a tile, it is at no one place.
        elsewhere = altered_record("elsewhere.json", lambda other: run_at(other, A).update(to=0x8004))
        for order in ((with_path, elsewhere), (elsewhere, with_path)):
            got, _ = added_up(*order)
            assert got == [{"offset": A - base, "size": 48, "mixed": True}] + runs[1:], got[:2]
        # How a copy came by its address: what one record knows, another that does not takes nothing from.
        weaker = altered_record("weaker.json", lambda other: (run_at(other, T + 32).update(begun="unknown"),
                                                             run_at(other, J).update(begun="told")))
        for order in ((with_path, weaker), (weaker, with_path)):
            got, _ = added_up(*order)
            assert got == [dict(run, begun="told") if run["offset"] == J - base else run for run in runs], order

        # A copy seen to run on through a byte that another record shows a copy was told to begin with.
        def through(other: dict) -> None:
            run_at(other, U).update(size=64)
            other["tile_runs"].remove(run_at(other, V))

        ran_on = altered_record("ran-on.json", through)
        got, _ = added_up(ran_on)
        assert got == [dict(run, size=64) if run["offset"] == U - base else run
                       for run in runs if run["offset"] != V - base]
        for order in ((with_path, ran_on), (ran_on, with_path)):
            assert added_up(*order)[0] == runs, order
        # Bytes that went to the tile area in a record that does not say where are counted, and not drawn.
        silent = altered_record("silent.json", lambda other: other["tile_runs"].remove(run_at(other, R)))
        added_up(silent)
        said = gb_data("tiles", str(fresh)).stdout
        drew(said, ALL - 1, 0)
        assert "28 bytes went to the tile area in records that do not say where: record with --data-use-tiles" \
            in said and "1 picture that the runs no longer make was taken away" in said, said
        # A record that says something this tool does not read is refused.
        odd = altered_record("odd.json", lambda other: run_at(other, J).update(step=5))
        assert "is not one this gb_data.py reads" in refused("uses", str(fresh), str(odd))
        odd = altered_record("odd.json", lambda other: run_at(other, J).update(offset=len(rom)))
        assert "is not inside the ROM" in refused("uses", str(fresh), str(odd))

    print("data tiles found: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
