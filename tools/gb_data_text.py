#!/usr/bin/env python3
"""A game's text as text: the strings in a data tree, found by who reads them.

`gb_data.py` uses this for its `glyphs` and `text` commands and when it puts
a tree together again.

Nothing in a ROM says which bytes are text, or what letter a byte is. Two
things a game was seen to do say both (`--data-use`, `gb_data.py uses`):

- one instruction, in the routine that prints, reads every string, a byte
  after a byte, and nearly all the bytes it reads arrive in a tile map as
  they are. So an instruction whose bytes mostly end up there, and that
  reads them in runs, is a reader of text, and a run of bytes it read is a
  string, control bytes and all;
- a byte that reaches a tile map draws the tile of that number, and the
  tiles are in video memory while the text is on the screen. So a save state
  from then has the game's alphabet in it, as pictures.

Turning the pictures into letters is the one step left to a reader: `glyphs`
draws the sheet, and whoever looks at it, a person or a model, writes the
charmap, a few dozen lines. After that `text` writes each region's strings
to a file, and `build` puts an edited string back where it was.

A string keeps its place and cannot grow there: the bytes after it belong
to something else, and nothing here can move them. A shorter one is written
over the start of the old one and the rest of the old one is left as it
was, which is right when the new string ends itself the way the old one
did, and `build` says so when it does not. A longer one is handed on: where
the game was recorded reading it through pointers that lie in data, `build`
moves what it is part of into free space (gb_data_pointers.py).

A game prints only what somebody plays as far as, and most of its text is
said by people nobody talked to. The strings that were seen say what a
string is made of, though: which bytes occur inside one and which byte ends
one. With `text --read` the data nobody saw read is gone through for more of
the same (`strings_read`): a stretch of such bytes with such an end, with
letters enough in it to be words and not chance. They are strings like the
others, edited and built the same way; what they are not is proven, so the
view says which they are (`read`), and a string that was seen printed is
never replaced by one that was read.
"""
from __future__ import annotations

import json
from pathlib import Path
import re

CHARMAP_SCHEMA = "gbrecomp.charmap"
ESCAPE = re.compile(r"<([0-9A-Fa-f]{2})>")
LINE = re.compile(r"\+([0-9A-Fa-f]+) (\d+) \|(.*)\|\s*$")
TEXT_SHARE = 0.75       # of an instruction's bytes, how many reach a tile map as they are, for it to read text
TEXT_LEAST = 16         # how many bytes it has to have read at all
TEXT_RUN = 2            # and how many, on average, one after another
READ_ALONE = 4          # letters a string nobody saw printed has to have to be taken for one by itself
READ_GAP = 4            # how near a string it may lie to be taken with any letters at all: a command and
                        # what the command takes may lie between the two
SCALE = 3               # the sheet draws each pixel of a tile this many times across
# Hex digits three pixels wide and five high, a row of each in three bits, for the sheet's edges.
DIGITS = {
    "0": (7, 5, 5, 5, 7), "1": (2, 6, 2, 2, 7), "2": (7, 1, 7, 4, 7), "3": (7, 1, 7, 1, 7),
    "4": (5, 5, 7, 1, 1), "5": (7, 4, 7, 1, 7), "6": (7, 4, 7, 5, 7), "7": (7, 1, 1, 1, 1),
    "8": (7, 5, 7, 5, 7), "9": (7, 5, 7, 1, 7), "A": (7, 5, 7, 5, 5), "B": (6, 5, 6, 5, 6),
    "C": (7, 4, 4, 4, 7), "D": (6, 5, 5, 5, 6), "E": (7, 4, 7, 4, 7), "F": (7, 4, 7, 4, 4),
}


class TextError(Exception):
    pass


def text_readers(manifest: dict) -> list[tuple[int, int]]:
    """The instructions that read text, from what `uses` wrote into the
    manifest: those most of whose bytes reached a tile map as they were, and
    that read their bytes in runs. The second half tells a routine that
    prints from one that draws a map out of blocks of tiles, where each of
    four instructions reads every fourth byte."""
    runs: dict[tuple[int, int], int] = {}
    for region in manifest["regions"]:
        last = None
        for seen in region.get("seen", ()):
            reader = (seen["read_at"][0], seen["read_at"][1])
            if last != (reader, seen["offset"]):
                runs[reader] = runs.get(reader, 0) + 1
            last = (reader, seen["offset"] + seen["size"])
    found = []
    for reader in manifest.get("readers", ()):
        key = (reader["bank"], reader["address"])
        drawn = sum(count for what, count in reader["uses"].items()
                    if "tile_map" in what.split("+") and not what.startswith("("))
        if reader["bytes"] >= TEXT_LEAST and drawn >= TEXT_SHARE * reader["bytes"] and \
                reader["bytes"] >= TEXT_RUN * runs.get(key, reader["bytes"]):
            found.append(key)
    return found


def strings_of(region: dict, readers: set[tuple[int, int]]) -> list[tuple[int, int]]:
    """The strings in a region, as (offset in the region, size): each run of
    bytes a text reader read, whatever became of each byte, since the bytes
    that end a string or break a line are read by it too and drawn nowhere."""
    strings: list[list[int]] = []
    for seen in region.get("seen", ()):
        if (seen["read_at"][0], seen["read_at"][1]) not in readers:
            continue
        if strings and strings[-1][0] + strings[-1][1] == seen["offset"]:
            strings[-1][1] += seen["size"]
        else:
            strings.append([seen["offset"], seen["size"]])
    return [(offset, size) for offset, size in strings]


def what_strings_are(seen: list[bytes], letters: dict[int, str]) -> tuple[set[int], set[int], set[int]]:
    """What the strings a game was seen to print say a string is: the bytes
    that occur inside one, the bytes that end one, and of the first those
    that are letters of a word, or a space between two.

    A byte ends strings if it ends two of them or more and does so more often than
    it stands inside one: the last letter of a name that fills its room is
    the last byte of a string too, and ends nothing. What can stand inside
    one is every byte the charmap has a letter for, seen inside a string or
    not: the strings that were printed are a few hundred, and a letter
    none of them has is a letter all the same. A string with a byte in it
    that nobody has read off the sheet is left for when somebody has."""
    inside: dict[int, int] = {}
    last: dict[int, int] = {}
    for string in seen:
        for byte in string[:-1]:
            inside[byte] = inside.get(byte, 0) + 1
        if string:
            last[string[-1]] = last.get(string[-1], 0) + 1
    enders = {byte for byte, count in last.items() if count >= 2 and count > inside.get(byte, 0)}
    body = set(letters) - enders
    words = {byte for byte in body if (len(letters[byte]) == 1 and letters[byte].isalpha()) or letters[byte] == " "}
    return body, enders, words


def strings_read(content: bytes, taken: bytes, stand: list[tuple[int, int]], body: set[int], enders: set[int],
                 words: set[int], letters: dict[int, str]) -> list[tuple[int, int]]:
    """The strings of a region nobody saw printed, as (offset, size), found
    by reading it for what strings are made of (`what_strings_are`).

    `taken` has a byte for each of the region's, not zero where something
    is known of it that is not text, or where a string already stands;
    `stand` is those strings. A stretch of bytes that occur inside strings,
    ended by a byte that ends them, is a candidate. It is taken for a string
    where at least half of it is letters and spaces and it has READ_ALONE
    letters, which chance seldom gives; or whatever it has, where it lies
    within READ_GAP bytes of a string that stands or of one taken so: the
    short names of a list lie one after another, and a sentence with a name
    put into it goes on after the name with as little as a full stop."""
    candidates: list[tuple[int, int, int]] = []
    start = None
    for at, byte in enumerate(content):
        if taken[at]:
            start = None
        elif byte in body:
            if start is None:
                start = at
        else:
            if byte in enders and start is not None:
                run = content[start:at]
                spelled = sum(1 for value in run if value in words and letters[value] != " ")
                if 2 * sum(1 for value in run if value in words) < len(run):
                    spelled = 0             # too little of it is words to stand by itself
                candidates.append((start, at + 1 - start, spelled))
            start = None
    spans = [(offset, offset + size) for offset, size in stand]
    found = []
    left = []
    for offset, size, spelled in candidates:
        if spelled >= READ_ALONE:
            found.append((offset, size))
            spans.append((offset, offset + size))
        else:
            left.append((offset, size))
    grew = True
    while grew and left:
        grew = False
        for offset, size in list(left):
            if any(0 <= offset - stop <= READ_GAP or 0 <= first - (offset + size) <= READ_GAP for first, stop in spans):
                found.append((offset, size))
                spans.append((offset, offset + size))
                left.remove((offset, size))
                grew = True
    return sorted(found)


def load_charmap(path: Path) -> dict[int, str]:
    """What each byte draws, as somebody read it off the sheet."""
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise TextError(f"cannot read the charmap {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != CHARMAP_SCHEMA or document.get("version") != 1 \
            or not isinstance(document.get("letters"), dict):
        raise TextError(f"{path} is not a version 1 {CHARMAP_SCHEMA} file")
    letters: dict[int, str] = {}
    for key, letter in document["letters"].items():
        if not re.fullmatch(r"[0-9A-Fa-f]{2}", key):
            raise TextError(f"{path}: '{key}' is not a byte in two hexadecimal digits")
        if not isinstance(letter, str):
            raise TextError(f"{path}: what byte {key} draws has to be text")
        if letter == "":
            continue                      # not read yet
        if any(c in letter for c in "<>\n\r"):
            raise TextError(f"{path}: byte {key} cannot be written '{letter}': <, > and the end of a line are "
                            f"taken")
        letters[int(key, 16)] = letter
    return letters


def decode(raw: bytes, letters: dict[int, str]) -> str:
    return "".join(letters.get(byte) or f"<{byte:02X}>" for byte in raw)


def encode(text: str, letters: dict[int, str]) -> bytes:
    """The bytes that draw `text`: `<XX>` is the byte XX, and otherwise the
    longest thing the charmap can draw at each place, by the lowest byte
    that draws it."""
    by_letter: dict[str, int] = {}
    for byte, letter in sorted(letters.items()):
        by_letter.setdefault(letter, byte)
    longest = max((len(letter) for letter in by_letter), default=0)
    out = bytearray()
    at = 0
    while at < len(text):
        escape = ESCAPE.match(text, at)
        if escape:
            out.append(int(escape.group(1), 16))
            at = escape.end()
            continue
        for length in range(min(longest, len(text) - at), 0, -1):
            byte = by_letter.get(text[at:at + length])
            if byte is not None:
                out.append(byte)
                at += length
                break
        else:
            raise TextError(f"nothing in the charmap draws '{text[at]}' (a byte can be written <XX>)")
    return bytes(out)


def strings_text(region_file: str, content: bytes, strings: list[tuple[int, int]], letters: dict[int, str]) -> str:
    """The file of text for a region's strings."""
    lines = [f"# The strings of {region_file}, one to a line: where it starts in the file, how many",
             "# bytes it has, and between the bars what it says. <XX> is the byte XX, for the bytes",
             "# the charmap has no letter for. Edit between the bars. A string made longer has to be",
             "# moved, which `build` does where `pointers` found what leads to it.", ""]
    for offset, size in strings:
        lines.append(f"+{offset:04X} {size} |{decode(content[offset:offset + size], letters)}|")
    return "\n".join(lines) + "\n"


def read_strings(name: str, text: str) -> dict[int, tuple[int, str]]:
    """What a file of text says now: offset -> (size, what is between the bars)."""
    found: dict[int, tuple[int, str]] = {}
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        match = LINE.match(line)
        if not match:
            raise TextError(f"{name}, line {number}: not '+OFFSET SIZE |text|'")
        offset = int(match.group(1), 16)
        if offset in found:
            raise TextError(f"{name}, line {number}: the string at +{offset:04X} is there twice")
        found[offset] = (int(match.group(2)), match.group(3))
    return found


def said_bytes(name: str, text: str, strings: list[tuple[int, int]], letters: dict[int, str]) -> bytes | None:
    """The bytes a file of text says its strings are, one after another, or
    None where it does not say them all."""
    said = read_strings(name, text)
    out = bytearray()
    for offset, _ in strings:
        if offset not in said:
            return None
        try:
            out += encode(said[offset][1], letters)
        except TextError:
            return None
    return bytes(out)


def edited_strings(name: str, text: str, content: bytes, strings: list[tuple[int, int]],
                   letters: dict[int, str], longer: list | None = None) -> list[tuple[int, bytes, str, str]]:
    """The strings a file of text says differently from the region's bytes,
    as (offset, the new bytes, the new text, a note about it or nothing).

    A line that still says what its bytes say is not an edit, whatever the
    charmap has learnt since: a string is only ever rewritten whole, from a
    line somebody changed.

    A string that has more bytes than it had cannot be written where it
    lies. Given a list in `longer`, it is put there as (offset, the new
    bytes, the new text), for whoever can move it (gb_data_pointers.py);
    without one it is refused."""
    said = read_strings(name, text)
    edits = []
    for offset, size in strings:
        if offset not in said:
            raise TextError(f"{name}: the line of the string at +{offset:04X} is gone")
        said_size, line = said.pop(offset)
        if said_size != size:
            raise TextError(f"{name}: the string at +{offset:04X} is {size} bytes where it lies, not {said_size}")
        old = content[offset:offset + size]
        if line == decode(old, letters):
            continue
        try:
            new = encode(line, letters)
        except TextError as error:
            raise TextError(f"{name}, the string at +{offset:04X}: {error}") from error
        if new == old:
            continue
        if len(new) > size and longer is not None:
            longer.append((offset, new, line))
            continue
        if len(new) > size:
            raise TextError(f"{name}: the string at +{offset:04X} is {len(new)} bytes now and there is room for "
                            f"{size}: a string cannot grow (moving what follows it is not built yet)")
        if new == old[:len(new)]:
            raise TextError(f"{name}: the string at +{offset:04X} was cut short and not ended, so the rest of the "
                            f"old one would follow it as before: end it as the old one ends, with "
                            f"{decode(old[-1:], letters)}")
        note = ""
        if len(new) < size and new[-1] != old[-1]:
            note = (f"   <- shorter, and it does not end with {decode(old[-1:], letters)} as the old one did: "
                    f"the rest of the old one follows it")
        edits.append((offset, new, line, note))
    if said:
        raise TextError(f"{name}: there is no string at +{min(said):04X}")
    return edits


def glyph_sheet(state, used: set[int]) -> tuple[int, int, bytes]:
    """A picture of the tile each tile-map byte draws in a save state
    (`gb_discover.Savestate`), as (width, height, RGBA): sixteen to a row,
    the byte's first digit down the side and its second along the top. The
    bytes not in `used` are drawn faint."""
    lcdc = state.read(0xFF40)
    palette = state.read(0xFF47)
    shades = [255 - 85 * ((palette >> (2 * index)) & 3) for index in range(4)]
    if len(set(shades)) < 2:
        shades = [255, 170, 85, 0]        # the screen is faded out: draw the tiles as they are
    cell = 8 * SCALE + 1
    edge = 5 * 2 + 4
    width = height = edge + 16 * cell + 1
    pixels = bytearray(b"\xC8\xC8\xC8\xFF" * (width * height))

    def dot(x: int, y: int, grey: int) -> None:
        pixels[(y * width + x) * 4:(y * width + x) * 4 + 3] = bytes((grey, grey, grey))

    def digit(letter: str, x: int, y: int) -> None:
        for row, bits in enumerate(DIGITS[letter]):
            for column in range(3):
                if bits & (4 >> column):
                    for dy in range(2):
                        for dx in range(2):
                            dot(x + column * 2 + dx, y + row * 2 + dy, 0)

    for index in range(16):
        digit("0123456789ABCDEF"[index], edge + index * cell + (cell - 6) // 2, 2)
        digit("0123456789ABCDEF"[index], 4, edge + index * cell + (cell - 10) // 2)
    for value in range(256):
        # Which tiles the background draws from: the low table, or the high one with the numbers signed.
        base = 0x8000 + value * 16 if lcdc & 0x10 else 0x9000 + value * 16 if value < 0x80 else 0x8000 + value * 16
        left, top = edge + (value & 15) * cell + 1, edge + (value >> 4) * cell + 1
        for row in range(8):
            low, high = state.read(base + row * 2), state.read(base + row * 2 + 1)
            for column in range(8):
                bit = 7 - column
                grey = shades[((high >> bit) & 1) << 1 | ((low >> bit) & 1)]
                if value not in used:
                    grey = 200 + grey * 40 // 255
                for dy in range(SCALE):
                    for dx in range(SCALE):
                        dot(left + column * SCALE + dx, top + row * SCALE + dy, grey)
    return width, height, bytes(pixels)
