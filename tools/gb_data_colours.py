#!/usr/bin/env python3
"""Tile pictures in the colours the game showed them in.

`gb_data.py` uses this for its `uses` and `tiles` commands and when it puts
a tree together again.

A tile is colour numbers, 0 to 3, and nothing in it says what colour a
number is: the game's palettes do, and they are set while it runs. A game
run with `--data-use` looks at its screen once a frame and notes, for the
tiles that lie in video memory as the ROM has them, which layer drew each
and through which palette: on a Game Boy the value of the palette register
(the background's for the background and the window, one of the two object
palettes for a sprite), on a Game Boy Color the palette's four colours. The
record's `shown` has that; `uses` adds the records up into the manifest.

`tiles` then draws a picture as an indexed PNG whose palette is the one most
seen for those tiles. Colour 0 of a tile a sprite drew is clear, since a
sprite does not draw it. The pixels of the PNG are still the colour numbers,
so nothing is lost by a palette that gives two numbers the same shade.

Read back, a picture that is still indexed with the palette it was written
with says the number of each pixel outright. One saved as something else is
read by colour: a pixel is the colour number whose colour it is nearest to,
a clear pixel is colour 0 where colour 0 is clear, and where two numbers
have that colour the pixel keeps the number the tile has there if that is
one of the two. Otherwise nothing says which is meant, and the picture is
refused with the pixel named.
"""
from __future__ import annotations

import json
from pathlib import Path
import struct
import zlib

USE_SCHEMA = "gbrt.data-use"
LAYERS = ("background", "objects")
GREYS = (255, 170, 85, 0)       # the grey of each shade a Game Boy has, the lightest first
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
CLEAR_BELOW = 128               # a pixel less solid than this is clear


class ColourError(Exception):
    pass


def palette_key(entry: dict) -> tuple:
    """A palette as a record or the manifest has it, as something to count by."""
    if "colours" in entry:
        return ("colours", tuple(entry["colours"]))
    return ("palette", entry["palette"])


def load_shown(paths: list[Path], size: int) -> dict[int, tuple[dict, set]]:
    """What the records say the screen showed of each ROM byte: how many
    frames each layer drew it in through each palette, and which bit planes
    it was. Several records add up. A record made before the recorder looked
    at the screen has no `shown`, and adds nothing."""
    shown: dict[int, tuple[dict, set]] = {}
    for path in paths:
        try:
            record = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise ColourError(f"cannot read the record {path}: {error}") from error
        if not isinstance(record, dict) or record.get("schema") != USE_SCHEMA:
            raise ColourError(f"{path} is not a {USE_SCHEMA} record")
        for run in record.get("shown", {}).get("tiles", ()):
            if not 0 <= run["offset"] <= run["offset"] + run["size"] <= size:
                raise ColourError(f"{path}: a run of tiles shown leaves the ROM at offset {run['offset']:#x}")
            for entry in run["as"]:
                if entry["by"] not in LAYERS:
                    raise ColourError(f"{path}: no such layer as '{entry['by']}'")
            for offset in range(run["offset"], run["offset"] + run["size"]):
                counts, planes = shown.setdefault(offset, ({}, set()))
                for entry in run["as"]:
                    key = (entry["by"], *palette_key(entry))
                    counts[key] = counts.get(key, 0) + entry["frames"]
                planes.add(run.get("planes"))
    return shown


def shown_runs(shown: dict[int, tuple[dict, set]], start: int, size: int) -> list[dict]:
    """The runs of a region's bytes that were shown alike, for the manifest,
    placed from the region's start: the most seen palette first."""
    runs: list[dict] = []
    for offset in range(start, start + size):
        if offset not in shown:
            continue
        counts, planes = shown[offset]
        ranked = sorted(counts.items(), key=lambda item: (-item[1], LAYERS.index(item[0][0]), item[0][1:]))
        seen = [{"by": by, kind: list(value) if kind == "colours" else value, "frames": frames}
                for (by, kind, value), frames in ranked]
        plane = next(iter(planes)) if len(planes) == 1 else None
        last = runs[-1] if runs else None
        if last and last["offset"] + last["size"] == offset - start and last["as"] == seen and \
                last.get("planes") == plane:
            last["size"] += 1
            continue
        run = {"offset": offset - start, "size": 1, "as": seen}
        if plane is not None:
            run["planes"] = plane
        runs.append(run)
    return runs


def rgb_of(entry: dict) -> list[tuple[int, int, int]]:
    """The colour each of the four colour numbers is drawn in through a palette."""
    if "colours" in entry:          # 15 bits: red in the low five, then green, then blue
        return [tuple((value >> shift & 31) * 255 // 31 for shift in (0, 5, 10)) for value in entry["colours"]]
    return [(GREYS[entry["palette"] >> 2 * number & 3],) * 3 for number in range(4)]


def most_seen(shown: list[dict], offset: int, size: int, bpp: int) -> dict | None:
    """The palette most seen for a stretch of a region, from what `uses`
    wrote: the one that was on the screen longest, a frame counting for each
    byte it showed, of those that tell the most colour numbers apart. None
    where the screen showed none of those bytes.

    The answer is what a picture's palette is to be: `entries`, a colour and
    whether it is solid for each colour number the picture has, and `by`,
    `palette` or `colours`, and `frames` as the manifest has them."""
    weights: dict[tuple, int] = {}
    longest: dict[tuple, int] = {}
    planes: set = set()
    for run in shown:
        covered = min(offset + size, run["offset"] + run["size"]) - max(offset, run["offset"])
        if covered <= 0:
            continue
        planes.add(run.get("planes"))
        for entry in run["as"]:
            key = (entry["by"], *palette_key(entry))
            weights[key] = weights.get(key, 0) + covered * entry["frames"]
            longest[key] = max(longest.get(key, 0), entry["frames"])
    if not weights:
        return None
    # The layer that drew them longest, and of its palettes the one seen longest among those that
    # keep the most colour numbers apart: a screen fading out, or all white while the game loads,
    # draws every number alike, and a picture of that shows nothing.
    by = min(LAYERS, key=lambda layer: -sum(weight for key, weight in weights.items() if key[0] == layer))

    def apart(key: tuple) -> int:
        colours = rgb_of({key[1]: key[2]})
        return len(set(colours[1:] if by == "objects" else colours))

    by, kind, value = min((key for key in weights if key[0] == by), key=lambda key: (-apart(key), -weights[key], key[1:]))
    found = {"by": by, kind: list(value) if kind == "colours" else value, "frames": longest[(by, kind, value)]}
    colours = rgb_of(found)
    if bpp == 1:
        # A tile of one bit a pixel has two colours: 0, and the number its bytes make where they
        # are set. Written to both bit planes that is 3, to the low one 1, to the high one 2.
        plane = next(iter(planes)) if len(planes) == 1 else None
        colours = [colours[0], colours[plane if plane in (1, 2) else 3]]
    found["entries"] = [(*colour, 255) for colour in colours]
    if by == "objects":
        found["entries"][0] = (*colours[0], 0)      # a sprite does not draw its colour 0
    return found


def written(entries: list[tuple[int, int, int, int]]) -> list[str]:
    """A palette as a view keeps it: red, green, blue and how solid, two hexadecimal digits each."""
    return ["%02x%02x%02x%02x" % entry for entry in entries]


def entries_of(palette: list[str]) -> list[tuple[int, int, int, int]]:
    return [tuple(bytes.fromhex(word)) for word in palette]


def check_palette(palette, bpp: int) -> bool:
    """Whether a view's palette is one `tiles` writes: a colour for each
    number, all solid but perhaps the first."""
    if not isinstance(palette, list) or len(palette) != 1 << bpp:
        return False
    for number, word in enumerate(palette):
        if type(word) is not str or len(word) != 8 or any(c not in "0123456789abcdef" for c in word) or \
                word[6:] not in (("00", "ff") if number == 0 else ("ff",)):
            return False
    return True


def indexed_png(width: int, height: int, numbers: bytes, entries: list[tuple[int, int, int, int]]) -> bytes:
    """An indexed PNG whose pixels are those colour numbers and whose palette
    is `entries`, at as many bits a pixel as the numbers need."""
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    depth = 1 if len(entries) <= 2 else 2
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        line = numbers[y * width:(y + 1) * width]
        packed = bytearray((width * depth + 7) // 8)
        for x, number in enumerate(line):
            packed[x * depth >> 3] |= number << (8 - depth - (x * depth & 7))
        rows += packed
    clear = bytes(entry[3] for entry in entries).rstrip(b"\xff")    # tRNS: how solid, as far as it is not all solid
    return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, 3, 0, 0, 0))
            + chunk(b"PLTE", b"".join(bytes(entry[:3]) for entry in entries))
            + (chunk(b"tRNS", clear) if clear else b"")
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def colour_said(colour: tuple[int, int, int]) -> str:
    return "#%02x%02x%02x" % colour


def numbers_shown(name: str, width: int, depth: int, colour: int, plte: bytes, trns: bytes, lines: list,
                  palette: list[str], before: bytes | None) -> bytes:
    """The colour number of each pixel of a picture that was drawn through
    `palette`, from the rows of its PNG as they were unpacked (`colour` and
    `depth` as the PNG's header has them).

    `before` is the number each pixel has in the tiles as they are now. It
    settles a pixel whose colour two numbers share, where it is one of the
    two; nothing else does, and such a pixel is refused."""
    entries = entries_of(palette)
    clear = entries[0][3] == 0
    solid = [entry[:3] for entry in entries]
    alpha = colour in (4, 6) or bool(trns)      # whether the PNG says which pixels are clear

    def by_colour(pixel: tuple[int, int, int, int], keyed: bool = False):
        """The number a colour is, or the numbers it may be. `keyed`: the PNG
        has no clearness for each pixel, and calls one colour clear wherever
        it is. A solid colour number that has that colour is then clear with
        colour 0, and the two cannot be told apart."""
        if clear and alpha and pixel[3] < CLEAR_BELOW:
            same = (0,) + tuple(number for number in range(1, len(entries)) if keyed and solid[number] == pixel[:3])
            return 0 if len(same) == 1 else same
        # Where the picture says what is clear, a pixel that is not is no colour 0.
        pool = range(1 if clear and alpha else 0, len(entries))
        nearest = min(pool, key=lambda number: sum((a - b) ** 2 for a, b in zip(solid[number], pixel)))
        same = tuple(number for number in pool if solid[number] == solid[nearest])
        return same[0] if len(same) == 1 else same

    # What each value a pixel can have says, worked out once.
    said: dict = {}
    if colour == 3:
        if not plte or len(plte) % 3:
            raise ColourError(f"{name} is damaged: it is an indexed PNG without a palette")
        table = [(*plte[i:i + 3], trns[i // 3] if i // 3 < len(trns) else 255) for i in range(0, len(plte), 3)]
        # The palette it was written with, colour for colour: a pixel's value is its number.
        kept = len(table) >= len(entries) and all(table[number][:3] == solid[number]
                                                  for number in range(len(entries)))
        for value, pixel in enumerate(table):
            said[value] = value if kept and value < len(entries) else by_colour(pixel)
    numbers = bytearray()
    step = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    key = None
    if trns and colour == 0:
        key = (trns[1],) if len(trns) >= 2 else None
    elif trns and colour == 2:
        key = tuple(trns[1:6:2]) if len(trns) >= 6 else None
    top = (1 << depth) - 1
    for y, line in enumerate(lines):
        if colour in (0, 3) and depth < 8:      # several pixels to a byte, the first in its high bits
            values = [(line[x * depth >> 3] >> (8 - depth - (x * depth & 7))) & top for x in range(width)]
        elif step == 1:
            values = line
        else:
            values = [tuple(line[x * step:(x + 1) * step]) for x in range(width)]
        for x, value in enumerate(values):
            found = said.get(value)
            if found is None:
                if colour == 3:
                    raise ColourError(f"{name} is damaged: a pixel has a colour its palette does not")
                if colour == 0:
                    grey = value * 255 // top
                    pixel = (grey, grey, grey, 0 if key == (value,) else 255)
                elif colour == 4:
                    pixel = (value[0], value[0], value[0], value[1])
                elif colour == 2:
                    pixel = (*value, 0 if key == value else 255)
                else:
                    pixel = value
                found = said[value] = by_colour(pixel, key is not None)
            if type(found) is tuple:
                at = y * width + x
                if before is None or before[at] not in found:
                    shade = colour_said(solid[found[0]])
                    both = " and ".join(f"colour {number}" for number in found)
                    had = "" if before is None else f", and the tile has colour {before[at]} there"
                    if clear and 0 in found:
                        how = ("colour 0 is the one a sprite does not draw: save the picture with what is clear in "
                               "it, as RGBA or as the indexed picture it was, and a clear pixel is colour 0")
                    else:
                        how = ("keep the picture indexed with the palette it was written with, where a pixel's "
                               "value is its colour number, or draw it in plain greys (`tiles --greys --force`)")
                    raise ColourError(f"{name}: the pixel {x} across and {y} down is {shade}, which is {both} "
                                      f"alike in this picture{had}. Nothing says which is meant: {how}")
                found = before[at]
            numbers.append(found)
    return bytes(numbers)
