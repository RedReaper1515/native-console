#!/usr/bin/env python3
"""Tile pictures in the colours the game showed them in, end to end.

Builds a small game twice, as a Game Boy game and as a Game Boy Color game.
It copies tiles out of its ROM (a font of two bits a pixel, background tiles,
a font of one bit a pixel written to both bit planes, two sprites, a tile no
map uses and a tile that is changed on the way), lays out a screen from the
ROM, shows two sprites, and changes the background's palette after a few
frames. Then:

- a run with `--data-use` has to end as a plain one does, and its record has
  to say, for the tiles that were on the screen and for no others, which
  layer drew them and through which palette, in how many frames: the palette
  register on a Game Boy, the palette's four colours on a Game Boy Color,
  where a tile in the second bank of video memory is told from the one at
  the same address in the first;
- `gb_data.py uses` has to carry that into the manifest, add records up, and
  take a record that has none of it;
- `tiles` has to draw a picture in the colours most seen, with colour 0 clear
  for tiles shown as sprites, and in plain greys when asked or when nothing
  was seen; unedited, however it is saved, the tree has to be the ROM still;
- an edit has to be read back right from an indexed, an RGB and an RGBA
  save, where two colours share a shade too, and to show in the game.

And tile maps. The game lays out a screen of five tiles by three, two blocks
of two by two (one twice, the other three times and once of them under the
first), a bar in the window, two lines of text and three tiles out of sight:

- the record has to say, for each ROM byte in a cell that was on the screen,
  in how many cells it was, how it lay to the byte before it, and which ROM
  tile its number drew, counting cells and not frames;
- `uses` has to make rows of that, and `maps` grids of the rows: the screen,
  each block and the bar, and not the text, in a file of tile numbers that
  is a view; unedited, the tree has to be the ROM still;
- its picture has to draw each number as the tile it stood for, in that
  tile's colours, from the tree as it is: an edited number and an edited
  tile both show, and a number never seen is hatched;
- an edited number has to be in the image and on the game's screen; a file
  that lost a row, a grid or its shape, or disagrees with the region's file,
  has to be refused by name, and one only saved again has to change nothing.

The cases of reading a picture back need no game, and come first.
"""
from __future__ import annotations

import argparse
import hashlib
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
sys.path.insert(0, str(ROOT / "tools"))
import gb_discover  # noqa: E402  (the save state reader)

START = 0x0150
PRINT = 0x0500
# In the home bank, what is laid out on the screen; in bank 1, the tiles.
STRING, SCREEN, BLOCKS, BAR, OFFSCREEN, SPRITE_TABLE = 0x0600, 0x0620, 0x0630, 0x0640, 0x0650, 0x0658
PALETTES, PALETTE_LATER, ATTRIBUTE, SHIFTED = 0x0660, 0x0680, 0x0690, 0x0698
FONT, BG_TILES, FONT_1BPP, SPRITES, UNSHOWN, CHANGED, BANK1_TILE = 0x4000, 0x4100, 0x4140, 0x4150, 0x4170, 0x4180, 0x4190
DROPPED = 0x41A0
LINE, END, SPACE = 0x4F, 0x50, 0x7F
FRAMES = 20
BEFORE = 12             # frames before the background's palette changes
BGP, BGP_LATER, OBP0, OBP1 = 0xE4, 0x1B, 0xD0, 0x90
# The Game Boy Color palettes, four colours of 15 bits each: two for the background, two for
# sprites (the first of those with colours 0 and 1 alike), and what the first becomes later.
BG0, BG1 = (0x7FFF, 0x56B5, 0x294A, 0x0000), (0x7FFF, 0x03FF, 0x001F, 0x0000)
OBJ0, OBJ1 = (0x7FFF, 0x7FFF, 0x5294, 0x0000), (0x0000, 0x001F, 0x03E0, 0x7C00)
BG0_LATER = (0x0000, 0x294A, 0x56B5, 0x7FFF)
# Where the pieces of the screen go, as (column, row) of the background's map.
SCREEN_AT, SCREEN_WIDE, SCREEN_HIGH = (1, 4), 5, 3
BLOCK_A_AT, BLOCK_B_AT = ((8, 4), (12, 8)), ((8, 6), (10, 4), (15, 10))
OFFSCREEN_AT = (25, 10)
EDGE_ROWS = (8, 10)             # block A twice more, with only its right half on the screen
SHIFTED_AT = (2, 14)            # a row the game draws with the screen moved a tile along
DROPPED_AT = (18, 2)            # where the tile is that the hardware does not let the game write
BAR_WIDE, BAR_HIGH = 6, 2
SIGNATURE = b"\x89PNG\r\n\x1a\n"


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def letters_of(text: str) -> bytes:
    """A to P are the bytes 80 to 8F, as the font has them."""
    return bytes(SPACE if letter == " " else 0x80 + ord(letter) - ord("A") for letter in text)


TEXT = letters_of("BAD CAFE") + bytes([LINE]) + letters_of("A FAB BEAD") + bytes([END])
SCREEN_TILES = bytes([0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0])
BLOCK_A, BLOCK_B = bytes([1, 2, 3, 0]), bytes([4, 5, 6, 1])
BAR_TILES = bytes([0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0])
OFFSCREEN_TILES = bytes([1, 2, 3])
SHIFTED_TILES = bytes([1, 2, 3])


def cell(at: tuple[int, int], map_base: int = 0x9800) -> int:
    return map_base + at[1] * 32 + at[0]


def make_rom(colour: bool) -> tuple[bytearray, dict[str, int]]:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13F] = b"DATA COLOUR"
    rom[0x143] = 0x80 if colour else 0x00
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    readers: dict[str, int] = {"print": PRINT}
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF

    def here() -> int:
        return START + len(code)

    def copy(name: str, source: int, target: int, count: int) -> None:
        """ld hl,source / ld de,target / ld b,count / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop"""
        nonlocal code
        readers.setdefault(name, here() + 8)
        code += [0x21, *word(source), 0x11, *word(target), 0x06, count & 0xFF, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]

    def grid(name: str, source: int, at: tuple[int, int], wide: int, high: int, map_base: int = 0x9800) -> None:
        for line in range(high):
            copy(name, source + line * wide, cell((at[0], at[1] + line), map_base), wide)

    def high_register(register: int, value: int) -> None:
        nonlocal code
        code += [0x3E, value, 0xE0, register]                              # ld a,value / ldh [register],a

    # The screen off while video memory is written, so that every write lands:
    # wait: ldh a,[$44] / cp 144 / jr nz,wait / xor a / ldh [$40],a
    code += [0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xAF, 0xE0, 0x40]
    high_register(0x47, BGP)
    high_register(0x48, OBP0)
    high_register(0x49, OBP1)
    if colour:
        for register, source in ((0x68, PALETTES), (0x6A, PALETTES + 16)):
            high_register(register, 0x80)                                  # from the first colour, stepping on
            # ld hl,source / ld b,16 / loop: ld a,[hli] / ldh [register + 1],a / dec b / jr nz,loop
            code += [0x21, *word(source), 0x06, 16, 0x2A, 0xE0, register + 1, 0x05, 0x20, 0xFA]
        high_register(0x42, 8)                                             # and the screen a tile down and across
        high_register(0x43, 8)
    # The tiles. With bit 4 of LCDC clear the background's tiles 0 to 7F are at 9000 and 80 to FF at 8800.
    copy("font", FONT, 0x8800, 256)
    copy("bg", BG_TILES, 0x9000, 64)
    # A font of one bit a pixel, each byte to both bit planes:
    # ld hl,Font1 / ld de,$9040 / ld b,16 / loop: ld a,[hli] / ld [de],a / inc de / ld [de],a / inc de / dec b / jr nz
    code += [0x21, *word(FONT_1BPP), 0x11, *word(0x9040), 0x06, 16, 0x2A, 0x12, 0x13, 0x12, 0x13, 0x05, 0x20, 0xF8]
    copy("sprites", SPRITES, 0x8000, 32)
    copy("unshown", UNSHOWN, 0x8100, 16)
    # A tile with every bit turned over on the way: ... loop: ld a,[hli] / cpl / ld [de],a / inc de / dec b / jr nz
    code += [0x21, *word(CHANGED), 0x11, *word(0x9060), 0x06, 16, 0x2A, 0x2F, 0x12, 0x13, 0x05, 0x20, 0xF9]
    # What is on the screen: two lines of text, a screen, blocks of two tiles by two, and three tiles out of sight.
    code += [0x21, *word(STRING), 0x11, *word(cell((1, 1))), 0xCD, *word(PRINT)]
    grid("screen", SCREEN, SCREEN_AT, SCREEN_WIDE, SCREEN_HIGH)
    for at in BLOCK_A_AT:
        grid("block", BLOCKS, at, 2, 2)
    for at in BLOCK_B_AT:
        grid("block", BLOCKS + 4, at, 2, 2)
    copy("offscreen", OFFSCREEN, cell(OFFSCREEN_AT), 3)
    # (the scrolled screen is a tile further down the map, and so are the lines that are drawn shifted)
    grid("shifted", SHIFTED, (SHIFTED_AT[0], SHIFTED_AT[1] + (1 if colour else 0)), 3, 1)
    # Block A at the screen's left edge: its right column in the first column that shows, its left out of sight.
    edge = 1 if colour else 0
    for line in EDGE_ROWS:
        copy("block", BLOCKS + 1, cell((edge, line)), 1)
        copy("block", BLOCKS + 3, cell((edge, line + 1)), 1)
    code += [0x3E, 0x07, 0xEA, *word(cell(DROPPED_AT))]                    # ld a,7 / ld [cell],a: tile 7, at 9070
    grid("bar", BAR, (0, 0), BAR_WIDE, BAR_HIGH, 0x9C00)                   # the window's map
    copy("oam", SPRITE_TABLE, 0xFE00, 8)
    if colour:
        high_register(0x4F, 1)                                             # the second bank of video memory
        copy("bank1", BANK1_TILE, 0x9000, 16)
        copy("attribute", ATTRIBUTE, cell(SCREEN_AT), 1)                   # the screen's first tile: bank 1, palette 1
        for column, line in ((0, 0), (1, 0), (0, 1), (1, 1)):              # the second block A: palette 1
            code += [0x3E, 0x01, 0xEA, *word(cell((BLOCK_A_AT[1][0] + column, BLOCK_A_AT[1][1] + line)))]
        high_register(0x4F, 0)
    high_register(0x4A, 128)                                               # the window over the last two rows
    high_register(0x4B, 7)
    high_register(0x40, 0xE3)                                              # on: window from 9C00, sprites, background
    # A tile written while the screen is being drawn, a byte in each line's drawing: the hardware drops the writes.
    # ld hl,Dropped / ld de,$9070 / ld b,16
    # out: ldh a,[$41] / and 3 / cp 3 / jr z,out / in: ldh a,[$41] / and 3 / cp 3 / jr nz,in
    #      ld a,[hli] / ld [de],a / inc de / dec b / jr nz,out
    code += [0x21, *word(DROPPED), 0x11, *word(0x9070), 0x06, 16]
    code += [0xF0, 0x41, 0xE6, 0x03, 0xFE, 0x03, 0x28, 0xF8, 0xF0, 0x41, 0xE6, 0x03, 0xFE, 0x03, 0x20, 0xF8]
    readers["dropped"] = here()
    code += [0x2A, 0x12, 0x13, 0x05, 0x20, 0xEA]
    # Every frame, the lines from 108 to 123 are drawn with the screen a tile further along, and the scroll
    # put back before the frame ends: what is drawn there is not where the registers say at the frame's end.
    # split: ldh a,[$44] / cp 108 / jr nz,split / ld a,There / ldh [$43],a
    #    w2: ldh a,[$44] / cp 124 / jr nz,w2 / ld a,Here / ldh [$43],a
    scroll = 8 if colour else 0
    split = [0xF0, 0x44, 0xFE, 108, 0x20, 0xFA, 0x3E, scroll + 8, 0xE0, 0x43,
             0xF0, 0x44, 0xFE, 124, 0x20, 0xFA, 0x3E, scroll, 0xE0, 0x43]
    # Some frames, then another palette for the background:
    # ld b,Before / frame: the split / w3: ldh a,[$44] / cp 144 / jr nz,w3 / dec b / jr nz,frame
    code += [0x06, BEFORE, *split, 0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0x05, 0x20, 0xE3]
    if colour:
        high_register(0x68, 0x80)
        code += [0x21, *word(PALETTE_LATER), 0x06, 8, 0x2A, 0xE0, 0x69, 0x05, 0x20, 0xFA]
    else:
        high_register(0x47, BGP_LATER)
    code += [*split, 0x18, 0xEA]                                           # the split, for ever
    assert here() <= PRINT, hex(here())
    rom[START:START + len(code)] = bytes(code)
    rom[PRINT:PRINT + 20] = bytes([
        0x2A,                   # Print: ld a,[hli]      the instruction that reads every string
        0xFE, END, 0xC8,        #        cp End / ret z
        0xFE, LINE, 0x28, 0x04,  # cp Line / jr z,line
        0x12, 0x13, 0x18, 0xF4,  # ld [de],a / inc de / jr Print
        0x7B, 0xE6, 0xE0, 0xC6, 0x20, 0x5F,   # line: ld a,e / and $E0 / add a,$20 / ld e,a
        0x18, 0xEC])            #        jr Print

    noise = random.Random(1996)
    rom[STRING:STRING + len(TEXT)] = TEXT
    rom[SCREEN:SCREEN + len(SCREEN_TILES)] = SCREEN_TILES
    rom[BLOCKS:BLOCKS + 8] = BLOCK_A + BLOCK_B
    rom[BAR:BAR + len(BAR_TILES)] = BAR_TILES
    rom[OFFSCREEN:OFFSCREEN + 3] = OFFSCREEN_TILES
    rom[SHIFTED:SHIFTED + 3] = SHIFTED_TILES
    # Two sprites side by side in the middle of the screen: y + 16, x + 8, tile, flags. The second
    # is drawn through the second palette: bit 4 says so on a Game Boy, the low bits on a Game Boy Color.
    rom[SPRITE_TABLE:SPRITE_TABLE + 8] = bytes([16 + 96, 8 + 80, 0, 0x00, 16 + 96, 8 + 100, 1, 0x01 if colour else 0x10])
    for index, palette in enumerate((BG0, BG1, OBJ0, OBJ1)):
        rom[PALETTES + index * 8:PALETTES + index * 8 + 8] = struct.pack("<4H", *palette)
    rom[PALETTE_LATER:PALETTE_LATER + 8] = struct.pack("<4H", *BG0_LATER)
    rom[ATTRIBUTE] = 0x09
    for tile in range(16):                  # A and P all of colour 3, the rest each its own pattern
        rom[FONT + tile * 16:FONT + tile * 16 + 16] = bytes([0xFF] * 16) if tile in (0, 15) else \
            bytes((tile * 37 + row * 11) & 0xFF for row in range(16))
    for start, size in ((BG_TILES, 64), (FONT_1BPP, 16), (SPRITES, 32), (UNSHOWN, 16), (CHANGED, 16),
                        (BANK1_TILE, 16), (DROPPED, 16)):
        rom[start:start + size] = noise.randbytes(size)
    assert 0 not in rom[DROPPED:DROPPED + 16]
    return rom, readers


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def tool(*arguments: object, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_DATA), *map(str, arguments)], expect=expect)


# ---------------------------------------------------------------------------
# This test's own writing and reading of PNG and of tiles, apart from the tool's


def chunk(kind: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def paeth(left: int, up: int, up_left: int) -> int:
    guess = left + up - up_left
    return min((left, up, up_left), key=lambda value: abs(guess - value))


def png(width: int, height: int, colour: int, depth: int, lines: list[bytes], back: int = 1, palette: bytes = b"",
        clear: bytes = b"") -> bytes:
    """A PNG of those rows of samples, each row put through the next of the
    five PNG filters, as a picture editor's would be. `back` is the bytes in
    a pixel, `palette` its PLTE and `clear` its tRNS."""
    raw = bytearray()
    above = bytes(len(lines[0]))
    for y, line in enumerate(lines):
        raw.append(y % 5)
        for i, value in enumerate(line):
            left, up_left = (line[i - back], above[i - back]) if i >= back else (0, 0)
            guess = (0, left, above[i], (left + above[i]) // 2, paeth(left, above[i], up_left))[y % 5]
            raw.append((value - guess) & 0xFF)
        above = line
    return (SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, colour, 0, 0, 0))
            + (chunk(b"PLTE", palette) if palette else b"") + (chunk(b"tRNS", clear) if clear else b"")
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def pack(values, depth: int) -> bytes:
    """Samples of fewer than 8 bits, the first in the high bits of each byte."""
    out = bytearray((len(values) * depth + 7) // 8)
    for x, value in enumerate(values):
        out[x * depth >> 3] |= value << (8 - depth - (x * depth & 7))
    return bytes(out)


def as_written(data: bytes) -> dict:
    """A PNG as `tiles` writes one, taken apart: no row is filtered."""
    assert data.startswith(SIGNATURE), data[:8]
    chunks: dict[bytes, bytes] = {}
    position = len(SIGNATURE)
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        chunks[kind] = chunks.get(kind, b"") + data[position + 8:position + 8 + length]
        position += 12 + length
    width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    assert interlace == 0 and colour in (0, 3), (colour, interlace)
    raw = zlib.decompress(chunks[b"IDAT"])
    stride = (width * depth + 7) // 8
    assert len(raw) == height * (stride + 1) and all(raw[y * (stride + 1)] == 0 for y in range(height))
    rows = []
    for y in range(height):
        line = raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        rows.append(bytes((line[x * depth >> 3] >> (8 - depth - (x * depth & 7))) & ((1 << depth) - 1)
                          for x in range(width)))
    return {"size": (width, height), "depth": depth, "colour": colour, "palette": chunks.get(b"PLTE"),
            "clear": chunks.get(b"tRNS"), "rows": rows}


def numbers_of(data: bytes, bpp: int, width: int) -> list[bytes]:
    """The rows of colour numbers a run of tiles is, `width` tiles to a row."""
    count = len(data) // (8 * bpp)
    rows = [bytearray(width * 8) for _ in range(-(-count // width) * 8)]
    for tile in range(count):
        for y in range(8):
            low = data[tile * 8 * bpp + y * bpp]
            high = data[tile * 8 * bpp + y * bpp + 1] if bpp == 2 else 0
            for x in range(8):
                rows[tile // width * 8 + y][tile % width * 8 + x] = (low >> (7 - x) & 1) + 2 * (high >> (7 - x) & 1)
    return [bytes(row) for row in rows]


def tiles_of(rows: list[bytes], count: int, width: int) -> bytes:
    """And back, at two bits a pixel."""
    data = bytearray()
    for tile in range(count):
        for y in range(8):
            line = rows[tile // width * 8 + y][tile % width * 8:tile % width * 8 + 8]
            data.append(sum((number & 1) << (7 - x) for x, number in enumerate(line)))
            data.append(sum((number >> 1) << (7 - x) for x, number in enumerate(line)))
    return bytes(data)


KINDS = ("indexed 2", "indexed 8", "indexed bare", "indexed turned", "indexed turned bare", "rgba", "rgb",
         "rgb keyed", "grey", "grey alpha")


def saved_as(kind: str, rows: list[bytes], entries: list[tuple], pixels: dict | None = None) -> bytes | None:
    """A picture of those colour numbers through that palette as another
    program might save it, or None where that kind of PNG cannot hold it.
    `pixels` paints over some: (x, y) -> red, green, blue and how solid."""
    width, height = len(rows[0]), len(rows)
    painted = [[(pixels or {}).get((x, y), entries[number]) for x, number in enumerate(row)]
               for y, row in enumerate(rows)]
    alphas = bytes(entry[3] for entry in entries)
    words = kind.split()
    if words[0] == "indexed":
        assert not pixels
        order = list(range(len(entries)))
        if "turned" in words:
            order.reverse()         # the last colour first, so a pixel's value is not its number
        depth = 8 if "8" in words else 2
        plte = b"".join(bytes(entries[number][:3]) for number in order)
        clear = b"" if "bare" in words else bytes(alphas[number] for number in order)
        return png(width, height, 3, depth, [pack([order.index(number) for number in row], depth) for row in rows],
                   palette=plte, clear=clear)
    if kind == "rgba":
        return png(width, height, 6, 8, [bytes(v for pixel in line for v in pixel) for line in painted], 4)
    if kind in ("rgb", "rgb keyed"):        # what is clear dropped, or one colour said to be clear wherever it is
        if kind == "rgb keyed" and alphas[0]:
            return None
        key = bytes(v for sample in entries[0][:3] for v in (0, sample)) if kind == "rgb keyed" else b""
        return png(width, height, 2, 8, [bytes(v for pixel in line for v in pixel[:3]) for line in painted], 3,
                   clear=key)
    if any(len(set(pixel[:3])) != 1 for line in painted for pixel in line):
        return None                         # no grey holds a colour
    if kind == "grey":
        return png(width, height, 0, 8, [bytes(pixel[0] for pixel in line) for line in painted])
    if kind == "grey alpha":
        return png(width, height, 4, 8, [bytes(v for pixel in line for v in (pixel[0], pixel[3])) for line in painted],
                   2)
    raise AssertionError(kind)


# Palettes a picture can be drawn through: four greys; a sprite's, with colours 0 and 1 both
# white and 0 clear; a background's caught fading, with colours 1 and 2 alike; and colours.
GREYS = [(255, 255, 255, 255), (170, 170, 170, 255), (85, 85, 85, 255), (0, 0, 0, 255)]
SPRITE = [(255, 255, 255, 0), (255, 255, 255, 255), (170, 170, 170, 255), (0, 0, 0, 255)]
FADING = [(255, 255, 255, 255), (170, 170, 170, 255), (170, 170, 170, 255), (0, 0, 0, 255)]
SPECTRUM = [(0, 0, 0, 0), (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255)]
# The ways of saving that say every pixel's number without knowing the tile, for each.
TOLD = {"greys": set(KINDS) - {"rgb keyed"},
        "sprite": {"indexed 2", "indexed 8", "indexed bare", "indexed turned", "rgba", "grey alpha"},
        "fading": {"indexed 2", "indexed 8", "indexed bare"},
        "spectrum": set(KINDS) - {"grey", "grey alpha"}}


def reading_back() -> None:
    """A picture drawn in colours, saved every way, read back: no game."""
    import gb_data
    import gb_data_colours

    def read(data: bytes, entries: list[tuple], before: bytes | None = None) -> bytes:
        width, _, depth, colour, plte, clear, lines = gb_data.png_lines("p.png", data)
        return gb_data_colours.numbers_shown("p.png", width, depth, colour, plte, clear, lines,
                                             gb_data_colours.written(entries), before)

    def refused(data: bytes, entries: list[tuple], before: bytes | None, *words: str) -> None:
        try:
            read(data, entries, before)
        except gb_data_colours.ColourError as error:
            assert all(word in str(error) for word in words), (words, str(error))
        else:
            raise AssertionError(f"read, where it could not be told: {words}")

    noise = random.Random(7)
    wide, high = 16, 8
    numbers = bytes(noise.randrange(4) for _ in range(wide * high))
    rows = [numbers[y * wide:(y + 1) * wide] for y in range(high)]
    at = {number: numbers.index(number) for number in range(4)}             # a pixel of each colour
    where = {number: (at[number] % wide, at[number] // wide) for number in range(4)}

    # What a palette register and a Game Boy Color palette draw each colour number in.
    assert gb_data_colours.rgb_of({"palette": 0xE4}) == [(255,) * 3, (170,) * 3, (85,) * 3, (0,) * 3]
    assert gb_data_colours.rgb_of({"palette": 0xD0}) == [(255,) * 3, (255,) * 3, (170,) * 3, (0,) * 3]
    assert gb_data_colours.rgb_of({"colours": [0x7FFF, 0x001F, 0x03E0, 0x7C00]}) == \
        [(255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    assert gb_data_colours.rgb_of({"colours": [0x5294, 0, 0, 0]})[0] == (164, 164, 164)

    # As `tiles` writes it: the numbers themselves, the palette, and colour 0 clear where it is.
    for entries in (GREYS, SPRITE, FADING, SPECTRUM):
        drawn = as_written(gb_data_colours.indexed_png(wide, high, numbers, entries))
        assert (drawn["size"], drawn["depth"], drawn["colour"], drawn["rows"]) == ((wide, high), 2, 3, rows)
        assert drawn["palette"] == b"".join(bytes(entry[:3]) for entry in entries), drawn["palette"]
        assert drawn["clear"] == (b"\x00" if entries[0][3] == 0 else None), drawn["clear"]
        assert read(gb_data_colours.indexed_png(wide, high, numbers, entries), entries) == numbers
    bits = bytes(number & 1 for number in numbers)
    drawn = as_written(gb_data_colours.indexed_png(wide, high, bits, SPRITE[:1] + SPRITE[3:]))
    assert (drawn["depth"], drawn["palette"], drawn["clear"]) == (1, bytes([255] * 3 + [0] * 3), b"\x00")
    assert b"".join(drawn["rows"]) == bits
    assert read(gb_data_colours.indexed_png(wide, high, bits, SPRITE[:1] + SPRITE[3:]), SPRITE[:1] + SPRITE[3:]) == bits

    # Saved by something else. Where the picture says each pixel's number, the tile is not asked;
    # where two numbers share a colour and nothing tells them apart, the tile settles it, or nothing does.
    read_alone = 0
    for name, entries in (("greys", GREYS), ("sprite", SPRITE), ("fading", FADING), ("spectrum", SPECTRUM)):
        for kind in KINDS:
            saved = saved_as(kind, rows, entries)
            if saved is None:
                continue
            assert read(saved, entries, numbers) == numbers, (name, kind)
            if kind in TOLD[name]:
                assert read(saved, entries) == numbers, (name, kind)
                assert read(saved, entries, bytes(len(numbers))) == numbers, (name, kind)
                read_alone += 1
            else:
                refused(saved, entries, None, "alike in this picture", "Nothing says which is meant")
    assert read_alone == 9 + 6 + 3 + 8, read_alone

    # A sprite's picture, edited. With what is clear in it, a white pixel that is solid is colour 1 and a
    # clear one colour 0, whatever is under it, and a pixel is clear below half solid.
    x3, y3 = where[3]
    for pixel, number in (((255, 255, 255, 255), 1), ((255, 255, 255, 0), 0), ((0, 0, 0, 0), 0), ((0, 0, 0, 127), 0),
                          ((0, 0, 0, 128), 3), ((170, 170, 170, 255), 2), ((250, 250, 240, 255), 1),
                          ((160, 175, 172, 200), 2), ((90, 60, 70, 255), 3)):
        expected = bytearray(numbers)
        expected[at[3]] = number
        assert read(saved_as("rgba", rows, SPRITE, {(x3, y3): pixel}), SPRITE, numbers) == bytes(expected), pixel
        assert read(saved_as("rgba", rows, SPRITE, {(x3, y3): pixel}), SPRITE) == bytes(expected), pixel
    # Without it, white is colour 0 or colour 1: it stays what the tile has, if that is one of them...
    for number in (0, 1):
        assert read(saved_as("rgb", rows, SPRITE, {where[number]: (255, 255, 255, 255)}), SPRITE, numbers) == numbers
        expected = bytearray(numbers)
        expected[at[number]] = 3
        assert read(saved_as("rgb", rows, SPRITE, {where[number]: (0, 0, 0, 255)}), SPRITE, numbers) == bytes(expected)
    # ...and where the tile has another colour, nothing says which white is meant: the pixel is named.
    for kind in ("rgb", "rgb keyed", "grey"):
        refused(saved_as(kind, rows, SPRITE, {(x3, y3): (255, 255, 255, 255)}), SPRITE, numbers,
                f"the pixel {x3} across and {y3} down is #ffffff, which is colour 0 and colour 1 alike",
                "the tile has colour 3 there", "save the picture with what is clear in it")
    # One colour said to be clear wherever it is makes the solid white clear too: the same doubt.
    assert read(saved_as("rgb keyed", rows, SPRITE), SPRITE, numbers) == numbers
    # A palette with no such pair has no such doubt, saved any way.
    expected = bytearray(numbers)
    expected[at[3]] = 0
    assert read(saved_as("rgb keyed", rows, SPECTRUM, {(x3, y3): (0, 0, 0, 255)}), SPECTRUM) == bytes(expected)
    expected[at[3]] = 2
    assert read(saved_as("rgb", rows, SPECTRUM, {(x3, y3): (10, 250, 30, 255)}), SPECTRUM) == bytes(expected)
    # Two solid colours alike: only the tile, or the picture's own numbers, tell them apart.
    x0, y0 = where[0]
    for kind in ("rgba", "rgb", "grey", "grey alpha"):
        refused(saved_as(kind, rows, FADING, {(x0, y0): (170, 170, 170, 255)}), FADING, numbers,
                f"the pixel {x0} across and {y0} down is #aaaaaa, which is colour 1 and colour 2 alike",
                "the tile has colour 0 there", "keep the picture indexed with the palette it was written with")
        expected = bytearray(numbers)
        expected[at[1]] = 3
        assert read(saved_as(kind, rows, FADING, {where[1]: (0, 0, 0, 255)}), FADING, numbers) == bytes(expected)

    # An indexed picture that kept its palette and got a colour more: that one is read by its colour.
    more = bytearray(numbers)
    more[at[0]] = 4
    extra = png(wide, high, 3, 8, [bytes(more[y * wide:(y + 1) * wide]) for y in range(high)],
                palette=b"".join(bytes(entry[:3]) for entry in SPRITE) + bytes([200, 30, 30]), clear=b"\x00")
    expected = bytearray(numbers)
    expected[at[0]] = 2
    assert read(extra, SPRITE) == bytes(expected)
    # And what is damaged is said to be.
    refused(png(wide, high, 3, 8, rows), SPRITE, None, "an indexed PNG without a palette")
    refused(png(wide, high, 3, 8, rows, palette=bytes(9)), SPRITE, None, "a colour its palette does not")

    # Any bytes drawn through any of these and read back are the same bytes.
    for bpp, palettes in ((2, (GREYS, SPRITE, FADING, SPECTRUM)), (1, (SPRITE[:1] + SPRITE[3:], GREYS[:2]))):
        for entries in palettes:
            for count, width in ((1, 1), (5, 16), (37, 16)):
                data = noise.randbytes(count * 8 * bpp)
                across, down, grey = gb_data.draw_tiles(data, bpp, min(width, count))
                drawn = gb_data_colours.indexed_png(across, down, grey.translate(gb_data.NEAREST[bpp]), entries)
                view = {"file": "p.png", "tiles": count, "width": min(width, count), "bpp": bpp,
                        "palette": gb_data_colours.written(entries)}
                assert gb_data.tiles_shown(view, drawn) == data, (bpp, count, entries)

    # Which palette a picture gets: the one on the screen longest, a frame counting for each byte.
    background = {"by": "background", "palette": 0xE4, "frames": 10}
    shown = [{"offset": 0, "size": 48, "as": [background, {"by": "background", "palette": 0x1B, "frames": 4}]},
             {"offset": 48, "size": 16, "as": [{"by": "objects", "colours": [1, 2, 3, 4], "frames": 20}]},
             {"offset": 64, "size": 8, "planes": 1, "as": [background]}]
    assert gb_data_colours.most_seen(shown, 80, 16, 2) is None
    found = gb_data_colours.most_seen(shown, 0, 64, 2)
    assert (found["by"], found["palette"], found["frames"], found["entries"]) == ("background", 0xE4, 10, GREYS)
    tie = [{"offset": 0, "size": 32, "as": [background]},           # 32 bytes for 10 frames, or 16 for 20: a tie
           {"offset": 32, "size": 16, "as": [{"by": "objects", "palette": 0xD0, "frames": 20}]}]
    assert gb_data_colours.most_seen(tie, 0, 48, 2)["by"] == "background"
    assert gb_data_colours.most_seen(tie, 16, 32, 2)["by"] == "objects"
    # A palette that draws every colour alike, or fewer of them apart, is passed over however long
    # it was seen; with nothing else seen it is what there is. A sprite's colour 0 does not count.
    blank = [{"offset": 0, "size": 16, "as": [{"by": "background", "palette": 0x00, "frames": 90},
                                              {"by": "background", "palette": 0xFE, "frames": 50},
                                              {"by": "background", "palette": 0x1B, "frames": 3}]},
             {"offset": 16, "size": 16, "as": [{"by": "background", "palette": 0x00, "frames": 9}]},
             {"offset": 32, "size": 16, "as": [{"by": "objects", "palette": 0xD0, "frames": 9},
                                               {"by": "objects", "palette": 0xE4, "frames": 2},
                                               {"by": "objects", "palette": 0x03, "frames": 30}]}]
    found = gb_data_colours.most_seen(blank, 0, 16, 2)
    assert (found["palette"], found["frames"]) == (0x1B, 3), found
    assert gb_data_colours.most_seen(blank, 16, 16, 2)["palette"] == 0x00
    assert gb_data_colours.most_seen(blank, 32, 16, 2)["palette"] == 0xD0
    found = gb_data_colours.most_seen(shown, 32, 32, 2)
    assert (found["by"], found["colours"], found["frames"]) == ("objects", [1, 2, 3, 4], 20), found
    assert found["entries"][0][3] == 0 and all(entry[3] == 255 for entry in found["entries"][1:])
    # One bit a pixel: colour 0, and the colour the bytes make where they are set.
    assert gb_data_colours.most_seen(shown, 64, 8, 1)["entries"] == [GREYS[0], GREYS[1]]
    for planes, number in ((1, 1), (2, 2), (3, 3), (None, 3)):
        run = {"offset": 0, "size": 8, "as": [background]}
        if planes:
            run["planes"] = planes
        assert gb_data_colours.most_seen([run], 0, 8, 1)["entries"] == [GREYS[0], GREYS[number]], planes


def generate(gbrecomp: Path, cmake_args: list[str], root: Path, name: str, colour: bool):
    """The game made and built, and its export: the ROM, its path, the executable, the export's path."""
    analysis = ["--no-scan", "--pointer-scan", "off"]
    rom, _ = make_rom(colour)
    rom_path = root / f"{name}.gb"
    rom_path.write_bytes(rom)
    export_path = root / f"{name}-export.json"
    project = root / f"{name}-game"
    run([str(gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
    run([str(gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", name])
    run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
         "-DCMAKE_BUILD_TYPE=Release", *cmake_args])
    run(["ninja", "-C", str(project / "build")])
    executable = next(path for path in (project / "build" / f"{name}.exe", project / "build" / name)
                      if path.exists())
    return rom, rom_path, executable, export_path


def play(executable: Path, root: Path, name: str, *flags: str, frames: int = FRAMES) -> tuple[dict, Path]:
    """Run the game: what it ended as, and the save state it left."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump, state = root / f"{name}.json", root / f"{name}.state"
    run([str(executable), "--headless", "--no-audio", "--limit-frames", str(frames), "--save-dir", str(saves),
         "--dump-state", str(dump), "--save-state-file", str(state), *flags], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8")), state


def seen_as(by: str, colour: bool, palette, frames: int) -> dict:
    """An entry of `shown`, as the record of either game has it."""
    return {"by": by, "colours": list(palette), "frames": frames} if colour else \
        {"by": by, "palette": palette, "frames": frames}


def rgb(palette) -> list[tuple[int, int, int]]:
    """The colours of a palette register, or of four colours of 15 bits."""
    if isinstance(palette, int):
        return [((255, 170, 85, 0)[palette >> 2 * number & 3],) * 3 for number in range(4)]
    return [tuple((value >> shift & 31) * 255 // 31 for shift in (0, 5, 10)) for value in palette]


def colours_of_a_game(args: argparse.Namespace, root: Path, name: str, colour: bool) -> None:
    rom, rom_path, executable, export_path = generate(args.gbrecomp, args.cmake_arg, root, name, colour)
    later = FRAMES - 1 - BEFORE     # the first frame is of the screen before the game has drawn anything
    bg_first, bg_later = (BG0, BG0_LATER) if colour else (BGP, BGP_LATER)
    sprite0, sprite1 = (OBJ0, OBJ1) if colour else (OBP0, OBP1)

    # Recording changes nothing about what the game does.
    plain, _ = play(executable, root, f"{name}-plain")
    record_path = root / f"{name}.use.json"
    recorded, _ = play(executable, root, f"{name}-recorded", "--data-use", str(record_path))
    differ = [key for key in plain if key != "dispatch_fallbacks" and plain[key] != recorded.get(key)]
    assert not differ, differ

    # What the record says the screen showed: the tiles that were on it, and no others.
    record = json.loads(record_path.read_text(encoding="utf-8"))
    shown = record["shown"]
    assert shown["frames"] == FRAMES and shown["dropped"] is False, shown["frames"]
    background = [seen_as("background", colour, bg_first, BEFORE), seen_as("background", colour, bg_later, later)]
    expected = [
        {"offset": FONT, "size": 6 * 16, "as": background},                     # the letters A to F, and no others
        {"offset": BG_TILES, "size": 64, "as": background},
        {"offset": FONT_1BPP, "size": 16, "planes": 3, "as": background},       # each byte in both bit planes
        {"offset": SPRITES, "size": 16, "as": [seen_as("objects", colour, sprite0, FRAMES - 1)]},
        {"offset": SPRITES + 16, "size": 16, "as": [seen_as("objects", colour, sprite1, FRAMES - 1)]},
    ]
    if colour:
        # Four background tiles were drawn through the second palette as well, all the while; and
        # the tile at the same address in the other bank of video memory is another tile of the ROM.
        expected[1]["as"] = [seen_as("background", True, BG1, FRAMES - 1)] + background
        expected.append({"offset": BANK1_TILE, "size": 16, "as": [seen_as("background", True, BG1, FRAMES - 1)]})
    assert shown["tiles"] == expected, [(got, wanted) for got, wanted in zip(shown["tiles"], expected) if got != wanted]
    # The record is otherwise what it was: every byte read, and what became of it.
    used = {entry["offset"]: entry for entry in record["ranges"]}
    assert used[UNSHOWN]["as_it_is"] == ["tiles"] and used[CHANGED]["worked_out"] == ["tiles"], used[UNSHOWN]
    # A tile on the screen that the game wrote while the hardware was drawing: the writes were
    # dropped. Its bytes were sent to the tile area all the same, and the screen never showed them.
    assert used[DROPPED]["as_it_is"] == ["tiles"] and used[DROPPED]["size"] == 16, used[DROPPED]
    state = gb_discover.Savestate.load(root / f"{name}-recorded.state")
    assert state.read(cell(DROPPED_AT)) == 7 and not any(state.read(0x9070 + index) for index in range(16))

    # Into the tree.
    tree = root / f"{name}-tree"
    tool("split", export_path, "--rom", rom_path, "-o", tree)

    def region_of(offset: int) -> dict:
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        return next(region for region in manifest["regions"]
                    if region["offset"] <= offset < region["offset"] + region["size"])

    # A record made before the recorder looked at the screen is taken, and says nothing of it.
    bare = dict(record)
    del bare["shown"]
    bare_path = root / f"{name}-bare.json"
    bare_path.write_text(json.dumps(bare), encoding="utf-8")
    said = tool("uses", tree, bare_path).stdout
    assert "shown on the screen" not in said and "shown" not in region_of(FONT), said
    stem = region_of(FONT)["file"][:-len(".bin")]
    base = region_of(FONT)["offset"]
    assert region_of(BANK1_TILE)["file"] == region_of(FONT)["file"] and region_of(FONT)["kind"] == "data"
    said = tool("tiles", tree, f"{stem}.bin", "--offset", SPRITES - base, "--tiles", 1).stdout
    assert "in the colours most seen" not in said, said
    grey_view = region_of(FONT)["views"][0]
    assert "palette" not in grey_view
    grey = (tree / grey_view["file"]).read_bytes()
    assert as_written(grey)["colour"] == 0 and as_written(grey)["depth"] == 8

    said = tool("uses", tree, record_path).stdout
    total = 6 * 16 + 64 + 16 + (16 if colour else 0)
    assert f"shown on the screen as the ROM has them: {len(expected)} runs of tile bytes, {total} bytes drawn by " \
        f"the background and 32 by sprites" in said, said
    region = region_of(FONT)
    assert region["shown"] == [dict(run, offset=run["offset"] - base) for run in expected], region["shown"]
    assert "shown" not in region_of(STRING) and region["views"] == [grey_view]
    # Two records are two runs of the game: their frames add up.
    tool("uses", tree, record_path, record_path)
    twice = region_of(FONT)["shown"]
    assert [[entry["frames"] for entry in run["as"]] for run in twice] == \
        [[2 * entry["frames"] for entry in run["as"]] for run in expected], twice
    tool("uses", tree, record_path)
    image = root / f"{name}-image.gb"

    def original() -> None:
        said = tool("build", tree, "-o", image, "--expect-original").stdout
        assert "the original ROM, byte for byte" in said and image.read_bytes() == bytes(rom), said
        assert "nothing edited" in tool("status", tree).stdout

    def draw(start: int, tiles: int, *flags: object) -> tuple[dict, Path, str]:
        said = tool("tiles", tree, f"{stem}.bin", "--offset", start - base, "--tiles", tiles, *flags).stdout
        view = next(view for view in region_of(FONT)["views"] if view["offset"] == start - base)
        return view, tree / view["file"], said

    # The first sprite, drawn again now that a record shows it: in its palette, colour 0 clear.
    view, sprite_png, said = draw(SPRITES, 1)
    entries = [(*colour_of, 255) for colour_of in rgb(sprite0)]
    entries[0] = (*entries[0][:3], 0)
    through = "the colours " + " ".join(f"{value:04X}" for value in sprite0) if colour else f"palette {sprite0:02X}"
    assert f"in the colours most seen: drawn by sprites through {through}, for {FRAMES - 1} frames. Colour 0 is " \
        f"clear, as a sprite leaves it (--greys draws the plain greys)" in said, said
    assert view["palette"] == ["%02x%02x%02x%02x" % entry for entry in entries], view["palette"]
    assert entries[0][:3] == entries[1][:3] == (255, 255, 255), entries        # two colours, one shade
    drawn = as_written(sprite_png.read_bytes())
    sprite_rows = numbers_of(bytes(rom[SPRITES:SPRITES + 16]), 2, 1)
    assert (drawn["size"], drawn["depth"], drawn["colour"]) == ((8, 8), 2, 3), drawn
    assert drawn["rows"] == sprite_rows and drawn["clear"] == b"\x00"
    assert drawn["palette"] == b"".join(bytes(entry[:3]) for entry in entries), drawn["palette"]
    assert {number for row in sprite_rows for number in row} == {0, 1, 2, 3}

    # The background's tiles: in the palette that was on the screen longest. The font of one bit a
    # pixel has two colours, 0 and the 3 its bytes make of both bit planes; a tile never shown, greys.
    bg_view, bg_png, said = draw(BG_TILES, 4)
    bg_most = BG1 if colour else BGP
    assert "drawn by the background through" in said and "clear" not in said, said
    assert bg_view["palette"] == ["%02x%02x%02xff" % colour_of for colour_of in rgb(bg_most)], bg_view["palette"]
    drawn = as_written(bg_png.read_bytes())
    assert drawn["rows"] == numbers_of(bytes(rom[BG_TILES:BG_TILES + 64]), 2, 4) and drawn["clear"] is None
    bg_rows, bg_written = numbers_of(bytes(rom[BG_TILES:BG_TILES + 64]), 2, 4), bg_png.read_bytes()

    def tile_edit(on: bool = True) -> bytes:
        """Paint over the third background tile in its picture, or put the picture back: that tile's bytes."""
        rows = [bytearray(row) for row in bg_rows]
        for y in range(8 if on else 0):
            for x in range(16, 24):
                rows[y][x] = (x + y) % 4
        bg_png.write_bytes(saved_as("indexed 8", [bytes(row) for row in rows],
                                    [(*colour_of, 255) for colour_of in rgb(bg_most)]) if on else bg_written)
        return tiles_of([bytes(row[16:24]) for row in rows], 1, 1)

    font_view, font_png, _ = draw(FONT_1BPP, 2, "--bpp", 1)
    first = rgb(bg_first)
    assert font_view["palette"] == ["%02x%02x%02xff" % first[0], "%02x%02x%02xff" % first[3]], font_view["palette"]
    drawn = as_written(font_png.read_bytes())
    assert (drawn["depth"], drawn["rows"]) == (1, numbers_of(bytes(rom[FONT_1BPP:FONT_1BPP + 16]), 1, 2))
    unshown_view, unshown_png, said = draw(UNSHOWN, 1)
    assert "palette" not in unshown_view and "most seen" not in said, said
    assert as_written(unshown_png.read_bytes())["colour"] == 0
    # The plain greys are still to be had, and are what they were.
    plain_view, plain_png, said = draw(SPRITES + 16, 1, "--greys")
    assert "palette" not in plain_view and "most seen" not in said, said
    drawn = as_written(plain_png.read_bytes())
    assert (drawn["colour"], drawn["depth"]) == (0, 8)
    assert drawn["rows"] == [bytes((255, 170, 85, 0)[number] for number in row)
                             for row in numbers_of(bytes(rom[SPRITES + 16:SPRITES + 32]), 2, 1)]
    original()
    assert "and 5 pictures of tiles" in tool("status", tree).stdout

    # Saved again by something else, every way, with nothing painted: still the ROM.
    written = sprite_png.read_bytes()
    for kind in KINDS:
        saved = saved_as(kind, sprite_rows, entries)
        if saved is not None:
            assert saved != written
            sprite_png.write_bytes(saved)
            original()

    # Edited, and saved with what is clear in it: a solid white pixel is colour 1, a clear one colour 0.
    def find(number: int) -> tuple[int, int]:
        return next((x, y) for y, row in enumerate(sprite_rows) for x, value in enumerate(row) if value == number)

    edits = {find(3): ((255, 255, 255, 255), 1), find(2): ((255, 255, 255, 0), 0), find(0): ((0, 0, 0, 255), 3),
             find(1): ((255, 255, 255, 0), 0)}
    edited_rows = [bytearray(row) for row in sprite_rows]
    for (x, y), (_, number) in edits.items():
        edited_rows[y][x] = number
    wanted = tiles_of([bytes(row) for row in edited_rows], 1, 1)
    assert sum(1 for old, new in zip(rom[SPRITES:SPRITES + 16], wanted) if old != new) >= 2
    sprite_png.write_bytes(saved_as("rgba", sprite_rows, entries, {at: pixel for at, (pixel, _) in edits.items()}))
    said = tool("status", tree).stdout
    assert f"edited: {view['file']} (01:{SPRITES:04X}, 1 of 1 tiles changed)" in said, said
    tool("build", tree, "-o", image)
    made = image.read_bytes()
    assert made[SPRITES:SPRITES + 16] == wanted, (made[SPRITES:SPRITES + 16].hex(), wanted.hex())
    assert made[:SPRITES] == bytes(rom[:SPRITES]) and made[SPRITES + 16:] == bytes(rom[SPRITES + 16:])
    # And the game shows it.
    _, state_path = play(executable, root, f"{name}-edited", "--rom-image", str(image))
    state = gb_discover.Savestate.load(state_path)
    assert bytes(state.read(0x8000 + index) for index in range(16)) == wanted
    # The same picture flattened: a white painted over colour 2 or 3 could be either white, and the
    # first of them is refused by name.
    x, y = min(find(3), find(2), key=lambda at: (at[1], at[0]))
    sprite_png.write_bytes(saved_as("rgb", sprite_rows, entries, {at: pixel for at, (pixel, _) in edits.items()}))
    for command in (("build", tree, "-o", root / "no.gb"), ("status", tree)):
        stderr = tool(*command, expect=(2,)).stderr
        assert f"{view['file']}: the pixel {x} across and {y} down is #ffffff, which is colour 0 and colour 1 " \
            f"alike in this picture, and the tile has colour {sprite_rows[y][x]} there" in stderr, stderr
    assert not (root / "no.gb").exists()
    # The region's file edited instead, and the picture as it was written: the file's edit stands.
    sprite_png.write_bytes(written)
    region_file = tree / f"{stem}.bin"
    kept = region_file.read_bytes()
    region_file.write_bytes(kept[:SPRITES - base] + wanted + kept[SPRITES - base + 16:])
    tool("build", tree, "-o", image)
    assert image.read_bytes()[SPRITES:SPRITES + 16] == wanted
    # Drawn again from the edited file, the picture shows the edit; put back, the tree is the ROM.
    assert as_written(draw(SPRITES, 1)[1].read_bytes())["rows"] == [bytes(row) for row in edited_rows]
    region_file.write_bytes(kept)
    draw(SPRITES, 1)
    assert sprite_png.read_bytes() == written
    original()

    # A manifest whose palette is not one is refused.
    good = (tree / "manifest.json").read_text(encoding="utf-8")
    index = next(number for number, entry in enumerate(json.loads(good)["regions"]) if entry["file"] == f"{stem}.bin")
    for palette, words in ((["ffffff00"] * 3, "is not a colour for each colour of a tile"),
                           (["ffffff00", "ffffff80", "aaaaaaff", "000000ff"], "is not a colour for each colour"),
                           (["ffffff00", "ffffffff", "aaaaaaff", "00000g"], "is not a colour for each colour"),
                           ("greys", "is not a colour for each colour")):
        altered = json.loads(good)
        next(entry for entry in altered["regions"][index]["views"] if entry["file"] == view["file"])["palette"] = palette
        (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
        assert words in tool("build", tree, "-o", root / "no.gb", expect=(2,)).stderr
    altered = json.loads(good)
    altered["regions"][index]["views"][0]["colours"] = 4
    (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
    assert "no such field as 'colours'" in tool("build", tree, "-o", root / "no.gb", expect=(2,)).stderr
    (tree / "manifest.json").write_text(good, encoding="utf-8", newline="\n")
    original()
    maps_of_a_game(root, name, colour, rom, record, record_path, tree, executable, export_path, rom_path, tile_edit)


def rgb_rows(data: bytes) -> list[list[tuple[int, int, int]]]:
    """An RGB PNG as `maps` writes its picture, taken apart: no row is filtered."""
    assert data.startswith(SIGNATURE), data[:8]
    chunks: dict[bytes, bytes] = {}
    position = len(SIGNATURE)
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        chunks[kind] = chunks.get(kind, b"") + data[position + 8:position + 8 + length]
        position += 12 + length
    width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    assert (depth, colour, interlace) == (8, 2, 0), (depth, colour, interlace)
    raw = zlib.decompress(chunks[b"IDAT"])
    stride = width * 3 + 1
    assert len(raw) == height * stride and all(raw[y * stride] == 0 for y in range(height))
    return [[tuple(raw[y * stride + 1 + x * 3:y * stride + 4 + x * 3]) for x in range(width)] for y in range(height)]


def maps_of_a_game(root: Path, name: str, colour: bool, rom: bytearray, record: dict, record_path: Path, tree: Path,
                   executable: Path, export_path: Path, rom_path: Path, tile_edit) -> None:
    """Tile maps: what the record says of the bytes on the screen, the file of
    tile numbers `maps` writes of them, its picture, and an edit read back."""
    shown = FRAMES - 1              # frames the game's own screen was on
    bg_first = BG0 if colour else BGP
    image = root / f"{name}-image.gb"

    def region_of(offset: int) -> dict:
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        return next(region for region in manifest["regions"]
                    if region["offset"] <= offset < region["offset"] + region["size"])

    def original() -> None:
        said = tool("build", tree, "-o", image, "--expect-original").stdout
        assert "the original ROM, byte for byte" in said and image.read_bytes() == bytes(rom), said
        assert "nothing edited" in tool("status", tree).stdout

    def tile_of(number: int, first: bool = False) -> list:
        """The ROM tile a tile number of this game draws, as the record has it: where it is and how many bytes."""
        if colour and first:
            return [BANK1_TILE, 16]             # the screen's first cell draws from the other bank
        return [BG_TILES + 16 * number, 16] if number < 4 else [FONT_1BPP + 8 * (number - 4), 8] if number < 6 else []

    # The record: each byte that was in a cell on the screen, how it lay to its neighbours, and its tile.
    seen = {entry["offset"]: entry for entry in record["shown"]["maps"]}

    def said(offset: int, cells: int, left: int, after: int, above: int, under: list, tile: list) -> None:
        assert seen[offset] == {"offset": offset, "cells": cells, "left": left, "after": after, "above": above,
                                "under": under, "tiles": [tile + [cells]] if tile else []}, seen[offset]

    for index, number in enumerate(SCREEN_TILES):
        # The scrolled screen starts at the map's second column, where the screen's first tiles are:
        # the cell to their left is out of sight, and says nothing.
        first = index % SCREEN_WIDE == 0
        said(SCREEN + index, shown, 0 if first and colour else shown, 0 if first else shown, shown,
             [[SCREEN_WIDE, shown]] if index >= SCREEN_WIDE else [], tile_of(number, index == 0))
    # Block A is on the screen twice and block B three times, once of them under an A: cells count, not frames.
    # Another B is right beside an A, whose bytes in the cells to its left are not the ones before B's: no row.
    # A's right column is there twice more, at the screen's edge with the cell to its left out
    # of sight, which is counted in none of `left`: a neighbour that cannot be seen says nothing.
    for index in range(8):
        whole = (2 if index < 4 else 3) * shown
        times = whole + (len(EDGE_ROWS) * shown if index in (1, 3) else 0)
        under = [[2, times]] if index % 4 >= 2 else [[2, shown]] if index >= 4 else []
        said(BLOCKS + index, times, whole, whole if index % 2 else 0, times, under,
             tile_of((BLOCK_A + BLOCK_B)[index]))
    for index, number in enumerate(BAR_TILES):  # the window's map, from its first cell: nothing left of or above that
        first, top = index % BAR_WIDE == 0, index < BAR_WIDE
        said(BAR + index, shown, 0 if first else shown, 0 if first else shown, 0 if top else shown,
             [] if top else [[BAR_WIDE, shown]], tile_of(number))
    assert not any(OFFSCREEN <= offset < OFFSCREEN + 3 for offset in seen) and ATTRIBUTE not in seen
    # The row drawn with the screen moved along: the cell the registers give for each of its pixels
    # at the frame's end holds another tile number than the one that pixel was drawn from, so none is kept.
    assert not any(SHIFTED <= offset < SHIFTED + 3 for offset in seen), sorted(seen)
    assert STRING in seen and STRING + 8 not in seen            # a letter is a tile number; a line's end draws nothing

    # Into the tree: rows. A record without any of it leaves none.
    home = region_of(SCREEN)
    base, stem = home["offset"], home["file"][:-len(".bin")]
    assert home["kind"] == "data" and region_of(BAR)["file"] == home["file"] == region_of(STRING)["file"]
    bare = dict(record, shown={key: value for key, value in record["shown"].items() if key != "maps"})
    bare_path = root / f"{name}-nomaps.json"
    bare_path.write_text(json.dumps(bare), encoding="utf-8")
    said = tool("uses", tree, bare_path).stdout
    assert "in a tile map" not in said and "mapped" not in region_of(SCREEN) and "shown" in region_of(FONT), said
    line2 = 10 if not colour else 9             # the second line's first letter is off the scrolled screen
    said = tool("uses", tree, record_path, record_path).stdout
    assert f"shown on the screen in a tile map as the ROM has them: {35 + 8 + line2} bytes, in 11 rows" in said, said
    rows = {row["offset"] + base: row for row in region_of(SCREEN)["mapped"]}
    wanted = {SCREEN: (5, None), SCREEN + 5: (5, 5), SCREEN + 10: (5, 5), BLOCKS: (2, None), BLOCKS + 2: (2, 2),
              BLOCKS + 4: (2, None), BLOCKS + 6: (2, 2), BAR: (6, None), BAR + 6: (6, 6), STRING: (8, None),
              STRING + 9 + (10 - line2): (line2, 10)}       # the second line, ten bytes on from the letters above it
    assert {offset: (row["size"], row.get("under")) for offset, row in rows.items()} == wanted, rows
    assert rows[SCREEN]["tiles"] == [tile_of(number, index == 0) or None
                                     for index, number in enumerate(SCREEN_TILES[:5])], rows[SCREEN]
    assert rows[SCREEN + 5]["tiles"][1] is None and rows[BLOCKS + 4]["tiles"] == [tile_of(4), tile_of(5)]

    # Nothing to go by in a tree no record was written into.
    fresh = root / f"{name}-fresh"
    tool("split", export_path, "--rom", rom_path, "-o", fresh)
    assert "nothing says yet what the game shows" in tool("maps", fresh, expect=(2,)).stderr

    # The file of tile numbers: the screen, the two blocks and the window's bar, and not the text.
    said = tool("maps", tree).stdout
    hidden = 3 + 3 + (1 if colour else 0)       # out of sight; drawn shifted; and the byte for the other bank's map
    assert "4 grids of tile numbers, 35 bytes, in 1 region" in said, said
    assert f"text is left alone: what 00:{PRINT:04X} read" in said, said
    assert f"{hidden} more bytes reached a tile map as they are and were in no cell of the screen" in said, said
    map_name = f"{stem}.map.txt"
    map_path, picture = tree / map_name, tree / f"{stem}.map.png"
    written = map_path.read_text(encoding="utf-8")
    grids = [[SCREEN - base, 5, 3], [BLOCKS - base, 2, 2], [BLOCKS + 4 - base, 2, 2], [BAR - base, 6, 2]]
    body = []
    for offset, width, down in grids:
        body.append(f"+{offset:04X} {width}x{down}")
        body += [" ".join(f"{value:02X}" for value in rom[base + offset + line * width:base + offset + (line + 1) * width])
                 for line in range(down)]
    assert [line for line in written.splitlines() if line and not line.startswith("#")] == body, written
    view = next(view for view in region_of(SCREEN)["views"] if view["kind"] == "map")
    numbers = b"".join(bytes(rom[base + offset:base + offset + width * down]) for offset, width, down in grids)
    assert view == {"kind": "map", "file": map_name, "grids": grids,
                    "sha256": hashlib.sha256(map_path.read_bytes()).hexdigest(),
                    "data_sha256": hashlib.sha256(numbers).hexdigest()}, view
    assert "and 1 file of tile maps" in tool("status", tree).stdout
    original()

    # Its picture: the grids side by side, each number as the tile it drew, in the colours those were seen in.
    def tile_pixels(start: int, size: int, colours: list) -> list[list[tuple]]:
        data = bytes(rom[start:start + size])
        return [[colours[number * (3 if size == 8 else 1)] for number in row] for row in numbers_of(data, size // 8, 1)]

    def cell_of(drawn: list, grid: int, index: int) -> list[list[tuple]]:
        left = 4 + sum(width * 8 + 4 for _, width, _ in grids[:grid])
        x, y = left + index % grids[grid][1] * 8, 4 + index // grids[grid][1] * 8
        return [line[x:x + 8] for line in drawn[y:y + 8]]

    drawn = rgb_rows(picture.read_bytes())
    assert (len(drawn[0]), len(drawn)) == (4 + (40 + 4) + (16 + 4) + (16 + 4) + (48 + 4), 4 + 24 + 4)
    assert drawn[0][0] == (72, 88, 120) and drawn[5][45] == (72, 88, 120)       # around the grids, and between them
    tiles_palette = rgb(BG1 if colour else BGP)     # the background tiles' palette, as `tiles` chose it too
    assert cell_of(drawn, 0, 1) == tile_pixels(BG_TILES + 16, 16, tiles_palette)
    assert cell_of(drawn, 0, 0) == tile_pixels(BANK1_TILE if colour else BG_TILES, 16, tiles_palette)
    assert cell_of(drawn, 0, 4) == tile_pixels(FONT_1BPP, 8, rgb(bg_first))     # one bit a pixel: colours 0 and 3
    assert cell_of(drawn, 3, 6) == tile_pixels(FONT_1BPP + 8, 8, rgb(bg_first))
    assert {pixel for line in cell_of(drawn, 0, 6) for pixel in line} == {(208, 80, 160), (120, 40, 96)}   # not the ROM's
    assert cell_of(drawn, 1, 3) == tile_pixels(BG_TILES, 16, tiles_palette)

    # Edited: two numbers of the screen and one of a block.
    def edit(*changes: tuple[str, str]) -> None:
        text = written
        for old, new in changes:
            assert text.count(old) == 1, (old, text)
            text = text.replace(old, new)
        map_path.write_text(text, encoding="utf-8", newline="\n")

    def refused(words: str) -> None:
        stderr = tool("build", tree, "-o", root / "no.gb", expect=(2,)).stderr
        assert words in stderr, (words, stderr)
        assert not (root / "no.gb").exists()

    edit(("00 01 02 03 04\n", "00 03 02 03 7E\n"), ("01 02\n03 00", "02 02\n03 00"),
         ("00 01 02 03 04 05\n", "00 00 02 03 04 05\n"))
    said = tool("status", tree).stdout
    assert f"edited: {map_name} (00:{SCREEN:04X}, 4 of 35 tile numbers changed)" in said, said
    tool("build", tree, "-o", image)
    made = image.read_bytes()
    expected = bytearray(rom)
    expected[SCREEN + 1], expected[SCREEN + 4], expected[BLOCKS], expected[BAR + 1] = 3, 0x7E, 2, 0
    assert made == bytes(expected), [offset for offset in range(len(rom)) if made[offset] != expected[offset]][:8]
    assert (tree / f"{stem}.bin").read_bytes() == bytes(rom[base:base + home["size"]])     # the file is not written to
    _, state_path = play(executable, root, f"{name}-map", "--rom-image", str(image))
    state = gb_discover.Savestate.load(state_path)
    assert [state.read(cell(SCREEN_AT) + index) for index in range(5)] == [0, 3, 2, 3, 0x7E]
    assert [state.read(cell(at)) for at in BLOCK_A_AT] == [2, 2] and state.read(0x9C01) == 0
    # `maps` again leaves the edit alone and draws the picture from it: a number seen elsewhere in the
    # grid draws the tile it drew there, and one never seen is hatched. An edited tile shows too.
    changed_tile = tile_edit()
    said = tool("maps", tree).stdout
    assert "1 of the files has been edited and is left as it is, the picture drawn from the edit" in said, said
    assert map_path.read_text(encoding="utf-8").count("00 03 02 03 7E\n") == 1
    assert next(entry for entry in region_of(SCREEN)["views"] if entry["kind"] == "map") == view
    drawn = rgb_rows(picture.read_bytes())
    assert cell_of(drawn, 0, 1) == cell_of(drawn, 0, 3) == tile_pixels(BG_TILES + 48, 16, tiles_palette)
    assert {pixel for line in cell_of(drawn, 0, 4) for pixel in line} == {(208, 80, 160), (120, 40, 96)}
    assert cell_of(drawn, 1, 0) == cell_of(drawn, 1, 1) == [
        [tiles_palette[number] for number in row] for row in numbers_of(changed_tile, 2, 1)]
    assert cell_of(drawn, 0, 2) == cell_of(drawn, 1, 0)             # the same tile, wherever a number draws it
    # The bar's second number is 0 now. On the Game Boy Color 0 drew two tiles in this region: the
    # other bank's in the screen's first cell, and the first background tile everywhere else. The
    # nearest byte that had a 0 is the bar's own first, so it is the background tile.
    assert cell_of(drawn, 3, 1) == cell_of(drawn, 3, 0) == tile_pixels(BG_TILES, 16, tiles_palette)
    assert (cell_of(drawn, 0, 0) == cell_of(drawn, 3, 0)) == (not colour)
    tile_edit(False)
    # Written again from the region's file when told to: the edit is gone, and the tree is the ROM.
    tool("maps", tree, "--force")
    assert map_path.read_text(encoding="utf-8") == written
    original()

    # What cannot be read back, each by name.
    edit(("00 01 02 03 04\n", "00 01 02 03\n"))
    refused(f"a row of 4 tile numbers in the grid at +{SCREEN - base:04X}, which is 5 across")
    edit(("00 01 02 03 04\n", "00 01 02 03 ZZ\n"))
    refused("'ZZ' is not a tile number")
    edit(("00 01 02 03 04\n", "00 01 02 03 100\n"))
    refused("'100' is not a tile number")
    edit((f"+{SCREEN - base:04X} 5x3\n00 01 02 03 04\n05 06 00 01 02\n03 04 05 06 00",
          f"+{SCREEN - base:04X} 3x5\n00 01 02\n03 04 05\n06 00 01\n02 03 04\n05 06 00"))
    refused(f"the grid at +{SCREEN - base:04X} is 5x3 where it lies, not 3x5: a grid keeps its size")
    edit(("05 06 00 01 02\n", ""))
    refused(f"the grid at +{SCREEN - base:04X} has 2 rows and is 3 down")
    edit((f"+{BLOCKS - base:04X} 2x2\n01 02\n03 00\n", ""))
    refused(f"the grid at +{BLOCKS - base:04X} is gone")
    edit((f"+{BLOCKS - base:04X} 2x2", f"+0001 1x1\n00\n\n+{BLOCKS - base:04X} 2x2"))
    refused("there is no grid at +0001")
    block_b = f"+{BLOCKS + 4 - base:04X} 2x2\n04 05\n06 01\n"
    edit((block_b, block_b + "\n" + block_b))
    refused(f"the grid at +{BLOCKS + 4 - base:04X} is there twice")
    edit((f"+{BLOCKS - base:04X} 2x2", "the blocks"))
    refused("not '+OFFSET ACROSSxDOWN'")
    map_path.unlink()
    refused(f"cannot read {map_name}")
    # Saved again and saying the same, with other line ends, a mark in front and small letters: nothing is edited.
    map_path.write_bytes(b"\xef\xbb\xbf" + written.replace("\n", "\r\n").replace("05 06", "5  06").encode("utf-8"))
    original()
    # The file and the numbers both edited. Saying different things, neither is taken; the same thing, it is.
    region_file = tree / f"{stem}.bin"
    kept = region_file.read_bytes()
    region_file.write_bytes(kept[:SCREEN - base + 1] + b"\x02" + kept[SCREEN - base + 2:])
    edit(("00 01 02 03 04\n", "00 03 02 03 04\n"))
    refused(f"{map_name} and {stem}.bin have both been edited, and disagree about 1 of the 35 tile numbers")
    assert "`build` will refuse this" in tool("status", tree).stdout
    edit(("00 01 02 03 04\n", "00 02 02 03 04\n"))
    tool("build", tree, "-o", image)
    assert image.read_bytes()[SCREEN + 1] == 2
    map_path.write_text(written, encoding="utf-8", newline="\n")        # as it was written: the file's edit stands
    tool("build", tree, "-o", image)
    assert image.read_bytes()[SCREEN + 1] == 2
    region_file.write_bytes(kept)
    original()

    # Other views keep out of these bytes, and these out of theirs.
    stderr = tool("tiles", tree, f"{stem}.bin", "--offset", BLOCKS - base, "--tiles", 1, expect=(2,)).stderr
    assert "are tile numbers of a map" in stderr and map_name in stderr, stderr
    # Every byte for a tile number, text too: its two lines are rows.
    said = tool("maps", tree, "--reader", "none", "--force").stdout
    assert f"6 grids of tile numbers, {35 + 8 + line2} bytes" in said and "what no instruction read" in said, said
    assert f"+{STRING - base:04X} 8x1" in map_path.read_text(encoding="utf-8")
    # Named, the instruction that prints is what was found.
    said = tool("maps", tree, "--reader", f"00:{PRINT:04X}", "--force").stdout
    assert "4 grids of tile numbers, 35 bytes" in said and map_path.read_text(encoding="utf-8") == written, said
    assert "takes BANK:ADDRESS" in tool("maps", tree, "--reader", "print", expect=(2,)).stderr
    # Bytes a picture has are left to it: sixteen from where the text starts, which both of its lines are in.
    tool("tiles", tree, f"{stem}.bin", "--offset", STRING - base, "--tiles", 1)
    said = tool("maps", tree, "--reader", "none", "--force").stdout
    assert "4 grids of tile numbers, 35 bytes" in said, said
    assert f"{8 + line2} more bytes lie in bytes a picture or a file of text has, and are left to it" in said, said
    original()

    # A manifest that says something else of a map view is refused.
    good = (tree / "manifest.json").read_text(encoding="utf-8")
    at = next(index for index, each in enumerate(json.loads(good)["regions"]) if each["file"] == f"{stem}.bin")
    for change, words in (
            (lambda each: each.update(grids=[[SCREEN - base, 5, 40000]]), "are not inside"),
            (lambda each: each.update(grids=[[BAR - base, 6, 2], [SCREEN - base, 5, 3]]), "one after another"),
            (lambda each: each.update(grids=[[STRING - base, 4, 2]]), "another view has"),
            (lambda each: each.update(grids=[[SCREEN - base, 0, 3]]), "are not inside"),
            (lambda each: each.update(grids="all"), "a field of the wrong type"),
            (lambda each: each.update(file="../maps.txt"), "is not a file inside"),
            (lambda each: each.pop("data_sha256"), "lacks the field 'data_sha256'"),
            (lambda each: each.update(tiles=[]), "no such field as 'tiles'"),
            (lambda each: each.update(kind="sound"), "reads 'tiles' and 'text', and 'map'")):
        altered = json.loads(good)
        change(next(each for each in altered["regions"][at]["views"] if each["kind"] == "map"))
        (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
        refused(words)
    (tree / "manifest.json").write_text(good, encoding="utf-8", newline="\n")
    original()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work in this folder and leave it there, to look at")
    args = parser.parse_args()
    reading_back()

    with tempfile.TemporaryDirectory(prefix="gbcolours-") as scratch:
        root = args.keep or Path(scratch)
        root.mkdir(parents=True, exist_ok=True)
        colours_of_a_game(args, root, "plain", False)
        colours_of_a_game(args, root, "colour", True)

    print("data colours end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
