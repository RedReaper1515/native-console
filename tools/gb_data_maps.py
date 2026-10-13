#!/usr/bin/env python3
"""Tile maps as grids of tile numbers to edit, and a picture of each.

`gb_data.py` uses this for its `uses` and `maps` commands and when it puts a
tree together again.

A byte that reaches a tile map as it is and is not text is a tile number: of
a screen, or of one of the blocks of tiles a game builds its maps from.
Nothing in the ROM says how wide such a screen or block is. Where its bytes
lay on the screen does: a game run with `--data-use` looks at its screen
once a frame, and for every ROM byte in a cell of a tile map that was on it
the record's `shown` says in how many cells it was; in how many of those the
cell to its left was on the screen as well, and held the byte before it in
the ROM; in how many the cell above was, and how many bytes before it in the
ROM that cell held, which is how many tiles a row is; and which ROM tile its
number drew.

`uses` adds the records up and writes rows into the manifest: a byte goes on
the row of the byte before it where it lay right of it in more than half of
the cells where that could be seen, and a row is under bytes so many before
it where more than half of the cells above its tiles say so. `maps` then
writes the rows of each region, put together into grids, to a file of tile
numbers beside the region's file, and draws a picture of them from the
tree's files as they are now, edits and all, with the tiles the numbers
stood for.

The file of numbers is a view, read back by `build` like a picture or a file
of text: a grid keeps its place and its size, and a number that was changed
takes the place of its byte. The picture is for looking at, and is not read.
"""
from __future__ import annotations

import bisect
import json
from pathlib import Path
import re
import struct
import zlib

USE_SCHEMA = "gbrt.data-use"
HEAD = re.compile(r"\+([0-9A-Fa-f]+) (\d+)x(\d+)\s*$")
NUMBER = re.compile(r"[0-9A-Fa-f]{1,2}$")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
SHEET = 320                     # how wide the picture of a region's grids gets before they go on a new line
GAP = 4                         # pixels between two grids
BETWEEN = (72, 88, 120)         # what is between them
NO_TILE = ((208, 80, 160), (120, 40, 96))   # a number whose tile did not come from the ROM as it is


class MapError(Exception):
    pass


def load_mapped(paths: list[Path], size: int) -> dict[int, dict]:
    """What the records say of each ROM byte that was in a cell of a tile map
    on the screen: `cells`, `left`, `after`, `above`, `under` (a count for
    each number of bytes back) and `tiles` (a count for each ROM tile it
    stood for). Several records add up, and a record without any of this
    adds nothing."""
    mapped: dict[int, dict] = {}
    for path in paths:
        try:
            record = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise MapError(f"cannot read the record {path}: {error}") from error
        if not isinstance(record, dict) or record.get("schema") != USE_SCHEMA:
            raise MapError(f"{path} is not a {USE_SCHEMA} record")
        for entry in (record.get("shown") or {}).get("maps", ()):
            if not 0 <= entry["offset"] < size:
                raise MapError(f"{path}: a byte of a tile map leaves the ROM at offset {entry['offset']:#x}")
            byte = mapped.setdefault(entry["offset"], {"cells": 0, "left": 0, "after": 0, "above": 0, "under": {},
                                                       "tiles": {}})
            for count in ("cells", "left", "after", "above"):
                byte[count] += entry[count]
            for width, times in entry["under"]:
                byte["under"][width] = byte["under"].get(width, 0) + times
            for offset, count, times in entry["tiles"]:
                byte["tiles"][(offset, count)] = byte["tiles"].get((offset, count), 0) + times
    return mapped


def mapped_rows(mapped: dict[int, dict], start: int, size: int) -> list[dict]:
    """The rows of a region's bytes, for the manifest, placed from the
    region's start: `offset` and `size`, `under` where the tiles above the
    row's were seen to be the bytes so many before them (which is how many
    tiles a whole row is), and `tiles`, for each byte the ROM tile its number
    drew most (where it starts and how many bytes it is: 16, or 8 for a tile
    of one bit a pixel) or null."""
    rows: list[dict] = []
    votes: list[tuple[int, dict]] = []      # for each row: the cells above its tiles, and what they said
    for offset in range(start, start + size):
        byte = mapped.get(offset)
        if byte is None:
            continue
        tile = min(byte["tiles"], key=lambda key: (-byte["tiles"][key], key), default=None)
        tile = list(tile) if tile else None
        last = rows[-1] if rows else None
        # It goes on the row of the byte before it where it lay right of it in most of the cells
        # whose neighbour could be seen.
        if last and last["offset"] + last["size"] == offset - start and byte["after"] * 2 > byte["left"]:
            last["size"] += 1
            last["tiles"].append(tile)
        else:
            rows.append({"offset": offset - start, "size": 1, "tiles": [tile]})
            votes.append((0, {}))
        above, under = votes[-1]
        for back, times in byte["under"].items():
            under[back] = under.get(back, 0) + times
        votes[-1] = (above + byte["above"], under)
    for row, (above, under) in zip(rows, votes):
        back = min(under, key=lambda key: (-under[key], key), default=0)
        if back and under[back] * 2 > above:
            row["under"] = back
    return rows


def grids_of(region: dict, readers: set[tuple[int, int]], taken: list[tuple[int, int]]) -> tuple[list, int]:
    """The grids of a region, as (offset, tiles across, rows): rows that
    follow one another in the file, each as wide as the one before and seen
    to lie under bytes that many before it. Rows with a byte in them that an instruction of
    `readers` read are text and are left out, and so are rows in bytes
    another view has (`taken`); how many bytes those were is returned too."""
    text = [(seen["offset"], seen["offset"] + seen["size"]) for seen in region.get("seen", ())
            if (seen["read_at"][0], seen["read_at"][1]) in readers]
    grids: list[list[int]] = []
    left_out = 0
    for row in region.get("mapped", ()):
        start, end = row["offset"], row["offset"] + row["size"]
        if any(first < end and start < last for first, last in text):
            continue
        if any(first < end and start < last for first, last in taken):
            left_out += row["size"]
            continue
        last = grids[-1] if grids else None
        if last and last[0] + last[1] * last[2] == start and row["size"] == last[1] and row.get("under") == last[1]:
            last[2] += 1
        else:
            grids.append([start, row["size"], 1])
    return grids, left_out


def tiles_of(region: dict) -> dict[int, tuple[int, int]]:
    """For each byte of a region's rows, the ROM tile its number stood for."""
    stood: dict[int, tuple[int, int]] = {}
    for row in region.get("mapped", ()):
        for index, tile in enumerate(row["tiles"]):
            if tile:
                stood[row["offset"] + index] = (tile[0], tile[1])
    return stood


def grids_text(region_file: str, content: bytes, grids: list) -> str:
    """The file of tile numbers for a region's grids."""
    lines = [f"# The tile maps of {region_file}. A grid starts with a line that says where it is in the",
             "# file and how many tiles it is across and down, and has a row of tile numbers to a line,",
             "# in hexadecimal. Edit the numbers. A grid keeps its place and its size.", ""]
    for offset, width, rows in grids:
        lines.append(f"+{offset:04X} {width}x{rows}")
        for row in range(rows):
            lines.append(" ".join(f"{byte:02X}" for byte in content[offset + row * width:offset + (row + 1) * width]))
        lines.append("")
    return "\n".join(lines)


def read_grids(name: str, text: str) -> dict[int, tuple[int, int, bytes]]:
    """What a file of tile numbers says now: offset -> (across, down, the numbers)."""
    found: dict[int, tuple[int, int, bytes]] = {}
    lines = [(number, line.strip()) for number, line in enumerate(text.splitlines(), 1)
             if line.strip() and not line.startswith("#")]
    at = 0
    while at < len(lines):
        number, line = lines[at]
        head = HEAD.match(line)
        if not head:
            raise MapError(f"{name}, line {number}: not '+OFFSET ACROSSxDOWN', which a grid starts with")
        offset, width, rows = int(head.group(1), 16), int(head.group(2)), int(head.group(3))
        if offset in found:
            raise MapError(f"{name}, line {number}: the grid at +{offset:04X} is there twice")
        numbers = bytearray()
        for row in range(rows):
            at += 1
            words = lines[at][1].split() if at < len(lines) else []
            if at >= len(lines) or HEAD.match(lines[at][1]):
                raise MapError(f"{name}: the grid at +{offset:04X} has {row} rows and is {rows} down")
            for word in words:
                if not NUMBER.match(word):
                    raise MapError(f"{name}, line {lines[at][0]}: '{word}' is not a tile number, which is two "
                                   f"hexadecimal digits")
            if len(words) != width:
                raise MapError(f"{name}, line {lines[at][0]}: a row of {len(words)} tile numbers in the grid at "
                               f"+{offset:04X}, which is {width} across")
            numbers += bytes(int(word, 16) for word in words)
        found[offset] = (width, rows, bytes(numbers))
        at += 1
    return found


def said_bytes(name: str, text: str, grids: list) -> bytes:
    """The bytes a file of tile numbers says its grids are, one after
    another. A grid keeps its place and its size: one that is gone, one more,
    or one of another size is refused."""
    said = read_grids(name, text)
    out = bytearray()
    for offset, width, rows in grids:
        if offset not in said:
            raise MapError(f"{name}: the grid at +{offset:04X} is gone")
        across, down, numbers = said.pop(offset)
        if (across, down) != (width, rows):
            raise MapError(f"{name}: the grid at +{offset:04X} is {width}x{rows} where it lies, not {across}x{down}: "
                           f"a grid keeps its size")
        out += numbers
    if said:
        raise MapError(f"{name}: there is no grid at +{min(said):04X}")
    return bytes(out)


def rgb_png(width: int, height: int, pixels: bytes) -> bytes:
    """An RGB PNG of those pixels."""
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    rows = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def placed(grids: list) -> tuple[int, int, list[tuple[int, int]]]:
    """Where each grid goes in the picture, one after another and on a new
    line where the picture would get too wide: the picture's size, and the
    corner of each."""
    corners = []
    x = y = GAP
    across = line = 0
    for _, width, rows in grids:
        if x > GAP and x + width * 8 + GAP > SHEET:
            x, y, line = GAP, y + line + GAP, 0
        corners.append((x, y))
        x += width * 8 + GAP
        across = max(across, x)
        line = max(line, rows * 8)
    return across, y + line + GAP, corners


def preview(image: bytes, base: int, grids: list, stood: dict[int, tuple[int, int]], was: bytes,
            colours_of) -> bytes:
    """A picture of a region's grids: each tile number drawn as the ROM tile
    it stands for, out of `image`, the ROM as the tree makes it now. `base`
    is where the region starts in it, `was` the region's bytes as the game
    was seen to draw them, and `colours_of(offset, size)` gives the four
    colours a ROM tile is drawn in.

    A number that was not changed draws the tile it was seen to draw. One
    that was changed, or whose own tile no record has, draws what the nearest
    byte of the region that had that number was seen to draw: a game's tile
    numbers mean the same tiles from one block of a set to the next. A
    number nothing is known of, or whose tile did not come from the ROM as
    it is, is drawn hatched."""
    width, height, corners = placed(grids)
    pixels = bytearray(bytes(BETWEEN) * (width * height))
    drawn: dict[tuple, list[bytes]] = {}
    # Where each tile number was seen to draw which tile, in the order of the file.
    seen: dict[int, list[tuple[int, tuple[int, int]]]] = {}
    for at in sorted(stood):
        seen.setdefault(was[at], []).append((at, stood[at]))

    def stands_for(at: int) -> tuple[int, int] | None:
        number = image[base + at]
        if number == was[at] and at in stood:
            return stood[at]
        places = seen.get(number)
        if not places:
            return None
        index = bisect.bisect_left(places, (at,))
        return min(places[max(0, index - 1):index + 1], key=lambda place: abs(place[0] - at))[1]

    def tile_rows(tile: tuple[int, int] | None) -> list[bytes]:
        if tile not in drawn:
            if tile is None:
                drawn[tile] = [b"".join(bytes(NO_TILE[(x + y) % 4 == 0]) for x in range(8)) for y in range(8)]
            else:
                offset, size = tile
                colours = [bytes(colour) for colour in colours_of(offset, size)]
                step = size // 8                # 2 bytes to a row of pixels, or 1 where each byte is both bit planes
                rows = []
                for y in range(8):
                    low = image[offset + y * step]
                    high = image[offset + y * step + step - 1]
                    rows.append(b"".join(colours[(low >> (7 - x) & 1) | (high >> (7 - x) & 1) << 1] for x in range(8)))
                drawn[tile] = rows
        return drawn[tile]

    for (offset, across, rows), (left, top) in zip(grids, corners):
        for index in range(across * rows):
            x, y = left + index % across * 8, top + index // across * 8
            for line, row in enumerate(tile_rows(stands_for(offset + index))):
                start = ((y + line) * width + x) * 3
                pixels[start:start + 24] = row
    return rgb_png(width, height, bytes(pixels))
