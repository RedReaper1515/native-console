#!/usr/bin/env python3
"""A data region's tiles as a picture, and the picture read back, end to end.

Builds a small ROM whose bank 1 is 1,024 tiles at 2 bits a pixel, the first
few known and the rest random bytes, with a font of 40 tiles at 1 bit a pixel
in the middle of the home bank's data. No game is compiled. Then:

- a tree without pictures has to split, build and say what was edited as
  before;
- `gb_data.py tiles` has to draw each run of tiles as a PNG of the size and
  the pixels the bytes say, and record it in the manifest; with nothing
  edited, `build --expect-original` still has to make the ROM;
- the same pictures saved as RGB, RGBA, greyscale with alpha and indexed
  PNGs, with every PNG row filter, and in greys that are a little off, have
  to be read back as the same bytes;
- one pixel changed has to change exactly the bytes of the image that hold
  that pixel, and `status` has to name the picture;
- an edit to the region's own file goes with the picture's where they are of
  different bytes; where both are of the same tiles and say different things
  `build` has to stop and name both files; a picture that is as `tiles` wrote
  it leaves the file's edit standing;
- a picture of the wrong size, an interlaced or 16-bit one, something that is
  not a PNG, a missing one, a file of code or the header, a file that is not
  a whole number of tiles, bytes already drawn, a picture drawn over an edit,
  and a manifest with a view this tool cannot read back are refused;
- any bytes at all survive being drawn and read back, at both depths.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import zlib

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"

START = 0x0150
FONT = 0x0200            # 40 tiles at 1 bit a pixel, inside the home bank's data
FONT_TILES = 40
SPARE = 0x0400           # three more tiles at 2 bits a pixel, further on in the same region
TILES = 0x4000           # all of bank 1: 1,024 tiles at 2 bits a pixel
SHADES = {2: (255, 170, 85, 0), 1: (255, 0)}
# Pan Docs' example of a tile: its first row of pixels is colours 0 2 3 3 3 3 2 0.
PAN_DOCS_TILE = bytes.fromhex("3C7E4242424242427E5E7E0A7C56387C")
SIGNATURE = b"\x89PNG\r\n\x1a\n"


def make_rom() -> bytearray:
    noise = random.Random(1989)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, START & 0xFF, START >> 8])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"DATA TILE"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    rom[START:START + 3] = bytes([0xF3, 0x18, 0xFE])                  # di / jr to itself
    # A box, a diagonal and a row of single pixels, then whatever comes.
    font = bytes([0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF,
                  0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,
                  0xAA, 0x00, 0x55, 0x00, 0x01, 0x00, 0x80, 0x00])
    rom[FONT:FONT + FONT_TILES * 8] = font + noise.randbytes(FONT_TILES * 8 - len(font))
    rom[SPARE:SPARE + 48] = noise.randbytes(48)
    # Pan Docs' tile, one tile of each colour, then whatever comes.
    known = PAN_DOCS_TILE + bytes(16) + bytes([0xFF, 0x00] * 8) + bytes([0x00, 0xFF] * 8) + bytes([0xFF] * 16)
    rom[TILES:TILES + 0x4000] = known + noise.randbytes(0x4000 - len(known))
    return rom


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 300) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def gb_data(*arguments: str, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_DATA), *arguments], expect)


# ---------------------------------------------------------------------------
# This test's own reading of tile bytes and of PNG, apart from the tool's


def sheet(data: bytes, bpp: int, width: int) -> list[bytes]:
    """The rows of grey pixels a run of tiles has to be drawn as."""
    count = len(data) // (8 * bpp)
    rows = [bytearray([255] * width * 8) for _ in range(-(-count // width) * 8)]
    for tile in range(count):
        for y in range(8):
            low = data[tile * 8 * bpp + y * bpp]
            high = data[tile * 8 * bpp + y * bpp + 1] if bpp == 2 else 0
            for x in range(8):
                colour = (low >> (7 - x) & 1) + 2 * (high >> (7 - x) & 1)
                rows[tile // width * 8 + y][tile % width * 8 + x] = SHADES[bpp][colour]
    return [bytes(row) for row in rows]


def paeth(left: int, up: int, up_left: int) -> int:
    guess = left + up - up_left
    return min((left, up, up_left), key=lambda value: abs(guess - value))


def png(width: int, height: int, colour: int, depth: int, lines: list[bytes], back: int = 1, palette: bytes = b"",
        interlace: int = 0) -> bytes:
    """A PNG of those rows of samples, each row put through the next of the
    five PNG filters, as a picture editor's would be. `back` is the bytes in
    a pixel."""
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    raw = bytearray()
    above = bytes(len(lines[0]))
    for y, line in enumerate(lines):
        raw.append(y % 5)
        for i, value in enumerate(line):
            left, up_left = (line[i - back], above[i - back]) if i >= back else (0, 0)
            guess = (0, left, above[i], (left + above[i]) // 2, paeth(left, above[i], up_left))[y % 5]
            raw.append((value - guess) & 0xFF)
        above = line
    return (SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, colour, 0, 0, interlace))
            + (chunk(b"PLTE", palette) if palette else b"")
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def read_grey(data: bytes) -> list[bytes]:
    """The rows of a PNG as `tiles` writes one: 8-bit greyscale, no row filtered."""
    assert data.startswith(SIGNATURE), data[:8]
    chunks: dict[bytes, bytes] = {}
    position = len(SIGNATURE)
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        chunks[kind] = chunks.get(kind, b"") + data[position + 8:position + 8 + length]
        position += 12 + length
    width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    assert (depth, colour, interlace) == (8, 0, 0), (depth, colour, interlace)
    raw = zlib.decompress(chunks[b"IDAT"])
    assert len(raw) == height * (width + 1), (len(raw), width, height)
    assert all(raw[y * (width + 1)] == 0 for y in range(height))
    return [raw[y * (width + 1) + 1:(y + 1) * (width + 1)] for y in range(height)]


def pack(values: list[int], depth: int) -> bytes:
    """Samples of fewer than 8 bits, the first in the high bits of each byte."""
    out = bytearray((len(values) * depth + 7) // 8)
    for x, value in enumerate(values):
        out[x * depth >> 3] |= value << (8 - depth - (x * depth & 7))
    return bytes(out)


def grey_png(lines: list[bytes]) -> bytes:
    return png(len(lines[0]), len(lines), 0, 8, lines)


def saved_as(kind: str, lines: list[bytes], bpp: int) -> bytes | None:
    """The picture as another program might save it, or None where that kind
    of PNG cannot hold it."""
    width, height = len(lines[0]), len(lines)
    if kind == "rgb":
        return png(width, height, 2, 8, [bytes(v for grey in line for v in (grey,) * 3) for line in lines], 3)
    if kind in ("off greys", "colours"):
        # Greys that are a little off, and colours as light as each grey: green counts for most and blue for least.
        near = {"off greys": {255: (250, 255, 240), 170: (180, 165, 170), 85: (70, 90, 100), 0: (20, 0, 30)},
                "colours": {255: (160, 255, 255), 170: (0, 255, 0), 85: (0, 120, 255), 0: (0, 0, 255)}}[kind]
        return png(width, height, 2, 8, [bytes(v for grey in line for v in near[grey]) for line in lines], 3)
    if kind in ("lightest", "darkest"):
        # The lightest and the darkest grey that is still nearest to each of the four, or of the two.
        edge = {("lightest", 2): {255: 255, 170: 212, 85: 127, 0: 42}, ("lightest", 1): {255: 255, 0: 127},
                ("darkest", 2): {255: 213, 170: 128, 85: 43, 0: 0}, ("darkest", 1): {255: 128, 0: 0}}[kind, bpp]
        return grey_png([bytes(edge[grey] for grey in line) for line in lines])
    if kind in ("rgba", "grey alpha"):     # with every alpha there is, none of which matters
        greys = 3 if kind == "rgba" else 1
        return png(width, height, 6 if kind == "rgba" else 4, 8,
                   [bytes(v for x, grey in enumerate(line) for v in (grey,) * greys + (x * 37 & 255,))
                    for line in lines], greys + 1)
    words = kind.split()
    depth = int(words[1])
    if words[0] == "indexed":  # a palette darkest first, so a pixel's value is not its colour, and one entry over
        if (1 << depth) < len(SHADES[bpp]):
            return None
        order = sorted(SHADES[bpp])
        palette = bytes(v for grey in order for v in (grey,) * 3) + (bytes([200, 0, 0]) if depth > bpp else b"")
        return png(width, height, 3, depth, [pack([order.index(grey) for grey in line], depth) for line in lines],
                   palette=palette)
    if words[0] == "grey":     # greyscale of 1, 2 or 4 bits: 2 bits is how pokered keeps its pictures
        top = (1 << depth) - 1
        if any(grey * top % 255 for grey in SHADES[bpp]):
            return None
        return png(width, height, 0, depth, [pack([grey * top // 255 for grey in line], depth) for line in lines])
    raise AssertionError(kind)


KINDS = ("rgb", "off greys", "colours", "lightest", "darkest", "rgba", "grey alpha", "indexed 8", "indexed 4",
         "indexed 2", "indexed 1", "grey 4", "grey 2", "grey 1")


def repaint(lines: list[bytes], x: int, y: int, grey: int) -> list[bytes]:
    row = bytearray(lines[y])
    assert row[x] != grey
    row[x] = grey
    return lines[:y] + [bytes(row)] + lines[y + 1:]


def any_bytes_survive() -> None:
    """Drawn and read back, any bytes are the same bytes: every colour of
    every pixel has a grey of its own."""
    spec = importlib.util.spec_from_file_location("gb_data", GB_DATA)
    assert spec and spec.loader
    tool = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(GB_DATA.parent))             # the tool imports the modules beside it
    spec.loader.exec_module(tool)
    noise = random.Random(4)
    for bpp in (2, 1):
        for count, width in ((1, 1), (3, 16), (16, 16), (37, 16), (64, 5), (128, 16)):
            data = noise.randbytes(count * 8 * bpp)
            wide, high, grey = tool.draw_tiles(data, bpp, min(width, count))
            assert (wide, high) == (min(width, count) * 8, -(-count // min(width, count)) * 8)
            assert [grey[y * wide:(y + 1) * wide] for y in range(high)] == sheet(data, bpp, min(width, count))
            again_wide, again_high, again = tool.read_png("drawn.png", tool.write_png(wide, high, grey))
            assert (again_wide, again_high, again) == (wide, high, grey)
            assert tool.read_tiles(again, count, bpp, min(width, count)) == data, (bpp, count, width)
    # Every byte a row of a tile can be, and every pair of them.
    every = bytes(range(256))
    assert tool.read_tiles(tool.draw_tiles(every, 1, 16)[2], 32, 1, 16) == every
    pairs = bytes(value for low in range(256) for high in range(256) for value in (low, high))
    assert tool.read_tiles(tool.draw_tiles(pairs, 2, 16)[2], len(pairs) // 16, 2, 16) == pairs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    args = parser.parse_args()
    any_bytes_survive()

    with tempfile.TemporaryDirectory(prefix="gbtiles-") as scratch:
        root = Path(scratch)
        rom = make_rom()
        rom_path = root / "tiles.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        run([str(args.gbrecomp), str(rom_path), "--no-scan", "--pointer-scan", "off",
             "--lift-export", str(export_path)])
        tree = root / "tree"
        gb_data("split", str(export_path), "--rom", str(rom_path), "-o", str(tree))
        image = root / "image.gb"

        def manifest() -> dict:
            return json.loads((tree / "manifest.json").read_text(encoding="utf-8"))

        def region_of(offset: int) -> dict:
            return next(region for region in manifest()["regions"]
                        if region["offset"] <= offset < region["offset"] + region["size"])

        def original() -> None:
            said = gb_data("build", str(tree), "-o", str(image), "--expect-original").stdout
            assert "the original ROM, byte for byte" in said, said
            assert image.read_bytes() == bytes(rom)
            assert "nothing edited" in gb_data("status", str(tree)).stdout

        def changed() -> dict[int, int]:
            """Build, and the bytes of the image that are not the ROM's."""
            said = gb_data("build", str(tree), "-o", str(image)).stdout
            made = image.read_bytes()
            assert len(made) == len(rom), len(made)
            differ = {offset: made[offset] for offset in range(len(rom)) if made[offset] != rom[offset]}
            assert ("the original ROM, byte for byte" in said) == (not differ), said
            return differ

        def refused(*arguments: str) -> str:
            return gb_data(*arguments, expect=(2,)).stderr

        home, bank = region_of(FONT), region_of(TILES)
        code, header = region_of(START), region_of(0x134)
        assert (home["kind"], bank["kind"], code["kind"], header["kind"]) == ("data", "data", "code", "header")
        assert region_of(SPARE)["file"] == home["file"] and home["offset"] < FONT
        assert (bank["offset"], bank["size"]) == (TILES, 0x4000)
        assert home["size"] % 8, "the home bank's data is meant not to be a whole number of tiles"
        files = len(manifest()["regions"])
        home_file, bank_file = tree / home["file"], tree / bank["file"]

        # A tree without pictures is as it was: the original, then one file edited.
        original()
        assert "views" not in json.dumps(manifest())
        kept = bank_file.read_bytes()
        bank_file.write_bytes(kept[:9] + bytes([kept[9] ^ 0xFF]) + kept[10:])
        assert changed() == {TILES + 9: rom[TILES + 9] ^ 0xFF}
        said = gb_data("status", str(tree)).stdout
        assert said.count("edited: ") == 1 and bank["file"] in said and "picture" not in said, said
        assert f"1 of {files} files edited" in gb_data("build", str(tree), "-o", str(image)).stdout
        bank_file.write_bytes(kept)
        original()

        # What is not tiles, or not said well enough to draw.
        assert "is code, and only data can be drawn as tiles" in refused("tiles", str(tree), code["file"])
        assert "is the cartridge header, and only data" in refused("tiles", str(tree), header["file"])
        said = refused("tiles", str(tree), home["file"])
        assert "not a whole number of 16-byte tiles" in said and "--tiles" in said, said
        assert "not a whole number of 8-byte tiles" in refused("tiles", str(tree), home["file"], "--bpp", "1")
        assert "has room for" in refused("tiles", str(tree), bank["file"], "--tiles", "1025")
        assert "has room for 1023" in refused("tiles", str(tree), bank["file"], "--offset", "16", "--tiles", "1024")
        assert "at least one tile" in refused("tiles", str(tree), bank["file"], "--width", "0")
        assert "it has no offset" in refused("tiles", str(tree), bank["file"], "--offset", "0x4000")
        assert "is not a file of the manifest" in refused("tiles", str(tree), "bank_07/4000_unknown.bin")
        assert "views" not in json.dumps(manifest()) and not list(tree.rglob("*.png"))

        # Drawn: all of bank 1 by default, the font from where it starts, and three tiles on their own.
        font_at = FONT - home["offset"]
        said = gb_data("tiles", str(tree), bank["file"]).stdout
        assert "1024 tiles from 01:4000" in said and "128x512 pixels" in said, said
        said = gb_data("tiles", str(tree), home["file"], "--offset", hex(font_at), "--tiles", str(FONT_TILES),
                       "--bpp", "1").stdout
        assert f"{FONT_TILES} tiles from 00:0200" in said and "128x24 pixels" in said, said
        # The last named by a path to its file, as a shell completes one.
        gb_data("tiles", str(tree), str(tree / home["file"]), "--offset", str(SPARE - home["offset"]),
                "--tiles", "3")
        home, bank = region_of(FONT), region_of(TILES)
        stem = home["file"][:-len(".bin")]
        assert [view["file"] for view in home["views"]] == [f"{stem}.tiles_0200.png", f"{stem}.tiles_0400.png"]
        assert [view["file"] for view in bank["views"]] == [bank["file"][:-len(".bin")] + ".tiles.png"]
        font_view, spare_view = home["views"]
        bank_view = bank["views"][0]
        font_png, spare_png, bank_png = (tree / view["file"] for view in (font_view, spare_view, bank_view))
        for view, path, start, bpp, width in ((bank_view, bank_png, TILES, 2, 16),
                                              (font_view, font_png, FONT, 1, 16),
                                              (spare_view, spare_png, SPARE, 2, 3)):
            size = view["tiles"] * 8 * bpp
            assert view == {"kind": "tiles", "file": view["file"], "offset": start - region_of(start)["offset"],
                            "tiles": size // (8 * bpp), "width": width, "bpp": bpp,
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                            "data_sha256": hashlib.sha256(rom[start:start + size]).hexdigest()}, view
            # The picture is the bytes, pixel for pixel; where a last row has no tile it is white.
            assert read_grey(path.read_bytes()) == sheet(bytes(rom[start:start + size]), bpp, width), view["file"]
        assert (bank_view["tiles"], font_view["tiles"], spare_view["tiles"]) == (1024, FONT_TILES, 3)
        bank_drawn, font_drawn = read_grey(bank_png.read_bytes()), read_grey(font_png.read_bytes())
        assert (len(bank_drawn[0]), len(bank_drawn)) == (128, 512)
        assert (len(font_drawn[0]), len(font_drawn)) == (128, 24)      # 40 tiles: two rows and half of a third
        assert len(read_grey(spare_png.read_bytes())[0]) == 24         # a row is no wider than the tiles there are
        assert bank_drawn[0][:8] == bytes([255, 85, 0, 0, 0, 0, 85, 255])                   # Pan Docs' first row
        assert [bank_drawn[0][8 * tile] for tile in range(1, 5)] == [255, 170, 85, 0]       # each colour's grey
        assert font_drawn[1][:8] == bytes([0, 255, 255, 255, 255, 255, 255, 0])             # 0x81: bit 7 is left
        assert all(font_drawn[y][64:] == bytes([255] * 64) for y in range(16, 24))          # where no tile is
        as_written = {path: path.read_bytes() for path in (bank_png, font_png, spare_png)}

        def restore() -> None:
            for path, content in as_written.items():
                path.write_bytes(content)
            home_file.write_bytes(bytes(rom[home["offset"]:home["offset"] + home["size"]]))
            bank_file.write_bytes(kept)
            original()

        # Nothing edited: the pictures are there and the image is still the ROM.
        original()
        assert "and 3 pictures of tiles" in gb_data("status", str(tree)).stdout

        # Saved by something else, the same picture is the same bytes.
        read = 0
        for kind in KINDS:
            saved = {path: saved_as(kind, lines, bpp)
                     for path, lines, bpp in ((bank_png, bank_drawn, 2), (font_png, font_drawn, 1))}
            for path, content in saved.items():
                if content is not None:
                    assert content != as_written[path]
                    path.write_bytes(content)
                    read += 1
            assert any(saved.values()), kind
            original()
        assert read == 2 * len(KINDS) - 2, read     # all but the two of one bit a pixel, which hold no four greys
        restore()

        # One pixel of tile 300 repainted: its low bit, its high bit, and both.
        tile, x, y = 300, 3, 2
        low = TILES + tile * 16 + y * 2
        was = (rom[low] >> (7 - x) & 1) + 2 * (rom[low + 1] >> (7 - x) & 1)
        at = (tile % 16 * 8 + x, tile // 16 * 8 + y)
        assert bank_drawn[at[1]][at[0]] == SHADES[2][was]
        for flip, expected in ((1, {low: rom[low] ^ 0x10}), (2, {low + 1: rom[low + 1] ^ 0x10}),
                               (3, {low: rom[low] ^ 0x10, low + 1: rom[low + 1] ^ 0x10})):
            bank_png.write_bytes(grey_png(repaint(bank_drawn, *at, SHADES[2][was ^ flip])))
            assert changed() == expected, (flip, expected)
        said = gb_data("status", str(tree)).stdout
        assert said.count("edited: ") == 1, said
        assert f"edited: {bank_view['file']} (01:4000, 1 of 1024 tiles changed)" in said, said
        said = gb_data("build", str(tree), "-o", str(image), "--expect-original", expect=(1,)).stdout
        assert f"1 of {files + 3} files edited" in said and bank_view["file"] in said, said
        # The file itself is not written to: the picture is read each time.
        assert bank_file.read_bytes() == kept
        # And one pixel of the font: one bit of one byte.
        glyph = FONT + 7 * 8 + 5
        font_at_pixel = (7 * 8 + 6, 5)
        assert font_drawn[5][7 * 8 + 6] == (0 if rom[glyph] & 0x02 else 255)
        font_png.write_bytes(grey_png(repaint(font_drawn, *font_at_pixel, 255 if rom[glyph] & 0x02 else 0)))
        assert changed() == {low: rom[low] ^ 0x10, low + 1: rom[low + 1] ^ 0x10, glyph: rom[glyph] ^ 0x02}
        # A pixel of the last row's empty half is no tile's.
        font_png.write_bytes(grey_png(repaint(font_drawn, 127, 23, 0)))
        bank_png.write_bytes(as_written[bank_png])
        original()
        restore()

        # The file and the picture, both edited.
        def poke(path: Path, offset: int) -> None:
            content = bytearray(path.read_bytes())
            content[offset] ^= 0xFF
            path.write_bytes(content)

        font_edit = grey_png(repaint(font_drawn, *font_at_pixel, 255 if rom[glyph] & 0x02 else 0))
        # Of different bytes: both count.
        elsewhere = home["offset"] + home["size"] - 1
        poke(home_file, home["size"] - 1)
        font_png.write_bytes(font_edit)
        assert changed() == {glyph: rom[glyph] ^ 0x02, elsewhere: rom[elsewhere] ^ 0xFF}
        said = gb_data("status", str(tree)).stdout
        assert said.count("edited: ") == 2, said
        assert home["file"] + " (" in said and font_view["file"] + " (" in said, said
        restore()
        # Of the same tiles, the picture as it was written: the file's edit stands.
        poke(home_file, font_at + 3)
        assert changed() == {FONT + 3: rom[FONT + 3] ^ 0xFF}
        # So it does with the picture only saved again.
        font_png.write_bytes(saved_as("rgb", font_drawn, 1))
        assert changed() == {FONT + 3: rom[FONT + 3] ^ 0xFF}
        # Saying different things: neither is taken.
        font_png.write_bytes(font_edit)
        said = refused("build", str(tree), "-o", str(root / "no.gb"))
        assert f"{font_view['file']} and {home['file']} have both been edited, and disagree about 2 of the 40 " \
            "tiles at 00:0200" in said, said
        assert not (root / "no.gb").exists()
        said = gb_data("status", str(tree)).stdout
        assert "`build` will refuse this" in said and font_view["file"] in said, said
        # Saying the same thing: taken.
        edited_font = bytearray(rom[FONT:FONT + FONT_TILES * 8])
        edited_font[3] ^= 0xFF
        font_png.write_bytes(grey_png(sheet(bytes(edited_font), 1, 16)))
        assert changed() == {FONT + 3: rom[FONT + 3] ^ 0xFF}
        # Drawn again: not over an edit, unless told to, and then from the file.
        font_png.write_bytes(font_edit)
        again = ("tiles", str(tree), home["file"], "--offset", str(font_at), "--tiles", str(FONT_TILES),
                 "--bpp", "1")
        assert "has been edited: --force draws it again" in refused(*again)
        assert font_png.read_bytes() == font_edit
        gb_data(*again, "--force")
        assert read_grey(font_png.read_bytes()) == sheet(bytes(edited_font), 1, 16)
        assert changed() == {FONT + 3: rom[FONT + 3] ^ 0xFF}
        assert [view["file"] for view in region_of(FONT)["views"]] == [font_view["file"], spare_view["file"]]
        # With the file put back that picture is out of date, but it is as written: the file stands.
        home_file.write_bytes(bytes(rom[home["offset"]:home["offset"] + home["size"]]))
        original()
        gb_data(*again)
        assert region_of(FONT)["views"][0] == font_view and font_png.read_bytes() == as_written[font_png]
        original()

        # Pictures that cannot be read back.
        wide = [line + bytes([255] * 8) for line in font_drawn]
        font_png.write_bytes(grey_png(wide))
        said = refused("build", str(tree), "-o", str(root / "no.gb"))
        assert "is 136x24 pixels" in said and "are 128x24: a picture keeps its size" in said, said
        assert "a picture keeps its size" in refused("status", str(tree))
        font_png.write_bytes(grey_png(font_drawn[:16]))
        assert "is 128x16 pixels" in refused("build", str(tree), "-o", str(root / "no.gb"))
        font_png.write_bytes(png(128, 24, 0, 8, font_drawn, interlace=1))
        assert "interlaced" in refused("build", str(tree), "-o", str(root / "no.gb"))
        font_png.write_bytes(png(128, 24, 0, 16, [bytes(v for grey in line for v in (grey, grey))
                                                  for line in font_drawn], 2))
        assert "16 bits a sample" in refused("build", str(tree), "-o", str(root / "no.gb"))
        font_png.write_bytes(b"GIF89a" + bytes(40))
        assert "is not a PNG" in refused("build", str(tree), "-o", str(root / "no.gb"))
        font_png.write_bytes(as_written[font_png][:-20])
        assert "is cut short" in refused("build", str(tree), "-o", str(root / "no.gb"))
        font_png.unlink()
        said = refused("build", str(tree), "-o", str(root / "no.gb"))
        assert "cannot read" in said and font_view["file"] in said and "`tiles` draws it again" in said, said
        gb_data(*again)
        assert font_png.read_bytes() == as_written[font_png]
        assert not (root / "no.gb").exists()
        original()

        # Bytes are drawn once: two pictures of the same ones could disagree.
        said = refused("tiles", str(tree), bank["file"], "--offset", "32", "--tiles", "2")
        assert "already drawn in" in said and bank_view["file"] in said, said
        said = refused("tiles", str(tree), home["file"], "--offset", str(font_at + 8 * (FONT_TILES - 1)),
                       "--tiles", "2", "--bpp", "1")
        assert "already drawn in" in said and font_view["file"] in said, said
        # A file that is in the way and is nobody's is not written over.
        stray = tree / f"{stem}.tiles_0800.png"
        stray.write_bytes(b"mine")
        assert "the manifest does not know it" in refused("tiles", str(tree), home["file"], "--offset",
                                                          str(0x800 - home["offset"]), "--tiles", "1")
        assert stray.read_bytes() == b"mine"
        stray.unlink()
        assert region_of(FONT)["views"] == [font_view, spare_view]

        # A manifest that asks for what this tool cannot read back is not built from.
        good = (tree / "manifest.json").read_text(encoding="utf-8")

        def with_manifest(change, words: str) -> None:
            altered = json.loads(good)
            change(altered["regions"])
            (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
            said = refused("build", str(tree), "-o", str(root / "no.gb"))
            assert words in said, (words, said)
            (tree / "manifest.json").write_text(good, encoding="utf-8", newline="\n")

        index = {region["file"]: number for number, region in enumerate(manifest()["regions"])}
        at_home, at_bank, at_code = index[home["file"]], index[bank["file"]], index[code["file"]]
        with_manifest(lambda regions: regions[at_bank]["views"][0].update(kind="sound"), "reads 'tiles' and 'text'")
        with_manifest(lambda regions: regions[at_code].update(views=[dict(bank_view)]), "only data has them")
        with_manifest(lambda regions: regions[at_bank]["views"][0].update(tiles=1025), "are not inside")
        with_manifest(lambda regions: regions[at_bank]["views"][0].update(bpp=4), "at 4 bits a pixel")
        with_manifest(lambda regions: regions[at_bank]["views"][0].update(file="../t.png"), "is not a file inside")
        with_manifest(lambda regions: regions[at_bank]["views"][0].update(colours=4), "no such field as 'colours'")
        with_manifest(lambda regions: regions[at_bank]["views"][0].pop("sha256"), "lacks the field 'sha256'")
        with_manifest(lambda regions: regions[at_home]["views"][1].update(offset=font_at + 8), "another view has")
        with_manifest(lambda regions: regions[at_home].update(pictures=[]), "has no such field as 'pictures'")
        original()
        assert not (root / "no.gb").exists()

    print("data tiles as png: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
