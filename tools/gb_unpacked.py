#!/usr/bin/env python3
"""Packed data edited as what the game unpacks it to.

`gb_data.py` uses this for its `unpacked` command and when it builds.

A packed picture cannot be edited where it lies: writing new packed bytes
takes the packer, which no game ships. What every game ships is the routine
that unpacks, and it makes the same bytes of each stream every time. So the
routine is left to do its work, and what it made is what gets edited.

Three things carry that:

- a declaration, `unpackers.json` at the top of the data tree, a few lines a
  game: which routine unpacks, where its stream starts as it is entered, and
  where its output lies as it returns (schema below);
- a harvest: the game run with `--unpacked PACK --unpacked-harvest FILE`
  writes out what the routine made of each stream it was given;
- a pack, which `gb_data.py build` writes beside the image: the declaration
  in the form the game reads, and for each picture that was edited the bytes
  to have in place of what the routine makes, with the hash of what it makes.
  The game puts them there as the routine returns, where the hash matches.

The declaration:

    {"schema": "gbrecomp.unpackers", "version": 1, "unpackers": [
      {"name": "pictures",
       "function": {"bank": 0, "address": 9469},
       "stream": {"bank": {"register": "a"}, "address": {"ram16": 53419}},
       "made": [{"address": 41352, "size": 392}, {"address": 41744, "size": 392}],
       "as": {"kind": "planes", "order": "columns",
              "tiles_wide": {"stream": 0, "shift": 4, "mask": 15},
              "tiles_high": {"stream": 0, "mask": 15}}}]}

Where a number is wanted for `stream` or a `made` address, it is a number,
or how to find it as the routine is entered: {"register": "a"} (a b c d e h
l bc de hl), {"ram8": ADDRESS}, {"ram16": ADDRESS} or {"mapped_bank": true}.

`as` says how to draw what was made, and without it the bytes are edited as
bytes. Two ways of drawing are known:

- {"kind": "tiles", "bpp": 2, "width": 16}: ordinary tiles one after
  another, as `gb_data.py tiles` draws them;
- {"kind": "planes", ...}: each piece of `made` is one bit of every pixel
  (the first piece the low bit), a byte for eight pixels across, and the
  bytes run down the picture a column of eight pixels at a time ("order":
  "columns") or across it a row at a time ("rows"). How many tiles wide and
  high the picture is, is a number, or a byte of the packed stream itself:
  {"stream": OFFSET, "shift": N, "mask": M}.

An unpacker may also say how the game is had to unpack a stream nobody
played as far as (`call`), and where tables of pointers to its streams are
(`tables`). `gb_data.py unpack` uses those, gb_data_unpack.py says what they
are, and the pack has neither.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct

SCHEMA = "gbrecomp.unpackers"
HARVEST_SCHEMA = "gbrt.unpacked-harvest"
DECLARATION = "unpackers.json"
NAME = re.compile(r"[A-Za-z0-9_-]{1,63}")
REGISTERS = {"a": (1, 0), "b": (1, 1), "c": (1, 2), "d": (1, 3), "e": (1, 4), "h": (1, 5), "l": (1, 6),
             "bc": (2, 0), "de": (2, 1), "hl": (2, 2)}
MAX_ROUTINES = 64
MAX_SEGMENTS = 8


class UnpackedError(Exception):
    pass


def expression(value, what: str) -> tuple[int, int]:
    """A number, or how to find one as the routine is entered, as the pack has it: (kind, value)."""
    if type(value) is int and 0 <= value <= 0xFFFF:
        return 0, value
    if isinstance(value, dict) and len(value) == 1:
        (key, inner), = value.items()
        if key == "register" and inner in REGISTERS:
            return REGISTERS[inner]
        if key in ("ram8", "ram16") and type(inner) is int and 0x8000 <= inner <= 0xFFFE:
            return (3 if key == "ram8" else 4), inner
        if key == "mapped_bank" and inner is True:
            return 5, 0
    raise UnpackedError(f"{what} is {json.dumps(value)}: it has to be a number, or one of {{\"register\": \"hl\"}}, "
                        f"{{\"ram8\": ADDRESS}}, {{\"ram16\": ADDRESS}} and {{\"mapped_bank\": true}}")


def tiles_count(value, what: str) -> None:
    if type(value) is int and 1 <= value <= 64:
        return
    if isinstance(value, dict) and set(value) <= {"stream", "shift", "mask"} and "stream" in value and \
            all(type(number) is int and 0 <= number <= 0xFFFF for number in value.values()):
        return
    raise UnpackedError(f"{what} is {json.dumps(value)}: it has to be a number of tiles, or a byte of the stream as "
                        f"{{\"stream\": OFFSET, \"shift\": N, \"mask\": M}}")


def check_shape(shape, unpacker: dict) -> None:
    name = unpacker["name"]
    sizes = [piece["size"] for piece in unpacker["made"]]
    if not isinstance(shape, dict) or shape.get("kind") not in ("tiles", "planes"):
        raise UnpackedError(f"'as' of {name} has to be of kind \"tiles\" or \"planes\"")
    if shape["kind"] == "tiles":
        if set(shape) - {"kind", "bpp", "width"}:
            raise UnpackedError(f"'as' of {name} has no such field as '{sorted(set(shape) - {'kind', 'bpp', 'width'})[0]}'")
        bpp, width = shape.get("bpp", 2), shape.get("width", 16)
        if bpp not in (1, 2) or type(width) is not int or width < 1:
            raise UnpackedError(f"'as' of {name}: tiles are 1 or 2 bits a pixel, some number to a row")
        if sum(sizes) % (8 * bpp):
            raise UnpackedError(f"{name} makes {sum(sizes)} bytes, which is not a whole number of {8 * bpp}-byte tiles")
        return
    if set(shape) - {"kind", "order", "tiles_wide", "tiles_high"}:
        odd = sorted(set(shape) - {"kind", "order", "tiles_wide", "tiles_high"})[0]
        raise UnpackedError(f"'as' of {name} has no such field as '{odd}'")
    if shape.get("order") not in ("columns", "rows"):
        raise UnpackedError(f"'as' of {name}: the bytes of a plane run in \"columns\" or in \"rows\"")
    if len(sizes) > 2:
        raise UnpackedError(f"'as' of {name}: a picture is of one plane or two, and {name} makes {len(sizes)} pieces")
    for field in ("tiles_wide", "tiles_high"):
        if field not in shape:
            raise UnpackedError(f"'as' of {name} lacks '{field}'")
        tiles_count(shape[field], f"'{field}' of {name}")


def load_declaration(path: Path) -> list[dict]:
    """The unpackers a game has, as somebody declared them."""
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise UnpackedError(f"cannot read the declaration {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != SCHEMA or document.get("version") != 1 or \
            not isinstance(document.get("unpackers"), list) or not document["unpackers"]:
        raise UnpackedError(f"{path} is not a version 1 {SCHEMA} file with unpackers in it")
    if len(document["unpackers"]) > MAX_ROUTINES:
        raise UnpackedError(f"{path} has more than {MAX_ROUTINES} unpackers")
    names = set()
    for unpacker in document["unpackers"]:
        if not isinstance(unpacker, dict):
            raise UnpackedError(f"{path}: an unpacker is an object")
        odd = sorted(set(unpacker) - {"name", "function", "stream", "made", "as", "why", "call", "tables"})
        if odd:
            raise UnpackedError(f"{path}: an unpacker has no such field as '{odd[0]}'")
        name = unpacker.get("name")
        if not isinstance(name, str) or not NAME.fullmatch(name) or name in names:
            raise UnpackedError(f"{path}: an unpacker's name is letters, digits, _ and -, and its own: {name!r}")
        names.add(name)
        function = unpacker.get("function")
        if not isinstance(function, dict) or set(function) != {"bank", "address"} or \
                type(function["bank"]) is not int or type(function["address"]) is not int or \
                not 0 <= function["address"] < 0x8000 or not 0 <= function["bank"] < 0x200 or \
                (function["address"] < 0x4000) != (function["bank"] == 0):
            raise UnpackedError(f"{path}: 'function' of {name} is the bank and address its routine is entered at, in "
                                f"the ROM")
        stream = unpacker.get("stream")
        if not isinstance(stream, dict) or set(stream) != {"bank", "address"}:
            raise UnpackedError(f"{path}: 'stream' of {name} is the bank and the address its packed bytes start at")
        expression(stream["bank"], f"the stream's bank of {name}")
        expression(stream["address"], f"the stream's address of {name}")
        made = unpacker.get("made")
        if not isinstance(made, list) or not 1 <= len(made) <= MAX_SEGMENTS:
            raise UnpackedError(f"{path}: 'made' of {name} is the pieces its output lies in, {MAX_SEGMENTS} at most")
        for piece in made:
            if not isinstance(piece, dict) or set(piece) != {"address", "size"} or type(piece["size"]) is not int or \
                    not 1 <= piece["size"] <= 0xFFFF:
                raise UnpackedError(f"{path}: a piece of what {name} makes is an address and a size in bytes")
            kind, value = expression(piece["address"], f"where a piece of what {name} makes lies")
            if kind == 0 and not (0x8000 <= value and value + piece["size"] <= 0xE000 or
                                  0xFF80 <= value and value + piece["size"] <= 0xFFFF):
                raise UnpackedError(f"{path}: {name} makes {piece['size']} bytes at {value:04X}, which is not all "
                                    f"RAM (8000-DFFF or FF80-FFFE)")
        if "as" in unpacker:
            try:
                check_shape(unpacker["as"], unpacker)
            except UnpackedError as error:
                raise UnpackedError(f"{path}: {error}") from error
        if "call" in unpacker or "tables" in unpacker:
            # How the game is had to unpack a stream by itself, and where its streams are:
            # `gb_data.py unpack` uses them, and gb_data_unpack.py says what they are.
            import gb_data_unpack
            try:
                gb_data_unpack.check_declared(unpacker, path)
            except gb_data_unpack.UnpackError as error:
                raise UnpackedError(str(error)) from error
    return document["unpackers"]


def made_sizes(unpacker: dict) -> list[int]:
    return [piece["size"] for piece in unpacker["made"]]


def pack_bytes(rom_size: int, rom_sha256: str, unpackers: list[dict], entries: list[tuple]) -> bytes:
    """The pack the game reads (runtime/src/gbrt_unpacked.c has the layout).
    An entry is (index of its unpacker, the stream's bank and address, the
    SHA-256 of what the routine makes as 32 bytes, the bytes to have instead)."""
    out = bytearray(b"GBUP" + struct.pack("<II", 1, rom_size) + bytes.fromhex(rom_sha256))
    out += struct.pack("<HI", len(unpackers), len(entries))
    for unpacker in unpackers:
        name = unpacker["name"].encode("ascii")
        out += struct.pack("<B", len(name)) + name
        out += struct.pack("<HH", unpacker["function"]["bank"], unpacker["function"]["address"])
        out += struct.pack("<BH", *expression(unpacker["stream"]["bank"], "the stream's bank"))
        out += struct.pack("<BH", *expression(unpacker["stream"]["address"], "the stream's address"))
        out += struct.pack("<B", len(unpacker["made"]))
        for piece in unpacker["made"]:
            out += struct.pack("<BH", *expression(piece["address"], "a piece")) + struct.pack("<H", piece["size"])
    for index, bank, address, made_sha256, replacement in sorted(entries, key=lambda entry: entry[:4]):
        out += struct.pack("<HHH", index, bank, address) + made_sha256 + struct.pack("<I", len(replacement))
        out += replacement
    return bytes(out)


def load_harvest(path: Path, rom: dict, unpackers: list[dict]) -> list[dict]:
    """What a game wrote with --unpacked-harvest: the things made, each with
    its unpacker's name, its stream's bank and address, and its bytes."""
    try:
        harvest = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise UnpackedError(f"cannot read the harvest {path}: {error}") from error
    if not isinstance(harvest, dict) or harvest.get("schema") != HARVEST_SCHEMA or harvest.get("version") != 1:
        raise UnpackedError(f"{path} is not a version 1 {HARVEST_SCHEMA} file")
    if harvest["rom"]["size"] != rom["size"] or harvest["rom"]["sha256"] != rom["sha256"]:
        raise UnpackedError(f"{path} was harvested from another ROM")
    if harvest.get("image_changed"):
        raise UnpackedError(f"{path} was harvested from a game given an edited image (--rom-image, "
                            f"{harvest['image_changed']} bytes changed): what a routine makes of edited data is not "
                            f"the game's own. Harvest without --rom-image")
    declared = {unpacker["name"]: unpacker for unpacker in unpackers}
    for watched in harvest["unpackers"]:
        unpacker = declared.get(watched["name"])
        if unpacker is None or (watched["bank"], watched["address"]) != \
                (unpacker["function"]["bank"], unpacker["function"]["address"]):
            raise UnpackedError(f"{path} was harvested with another declaration: it has '{watched['name']}' at "
                                f"{watched['bank']:02X}:{watched['address']:04X}")
    things = []
    for made in harvest["made"]:
        unpacker = declared[made["unpacker"]]
        content = bytes.fromhex(made["bytes"])
        if len(content) != sum(made_sizes(unpacker)) or hashlib.sha256(content).hexdigest() != made["sha256"]:
            raise UnpackedError(f"{path}: what '{made['unpacker']}' made of {made['bank']:02X}:{made['address']:04X} "
                                f"is not the size or the hash it says")
        things.append({"unpacker": made["unpacker"], "bank": made["bank"], "address": made["address"],
                       "bytes": content, "times": made["times"]})
    return things


def bit_moves(first: bytes, second: bytes) -> list[int] | None:
    """If `second` is `first` with the bits of every byte moved about the
    same way, where each bit of a byte of `second` comes from: bit N of it is
    bit moves[N] of the byte of `first` at the same place. None if it is not,
    or if the bytes do not say which bit is which.

    A game that wants a picture mirrored often unpacks it a second way, and
    what it makes then is the first with each byte's bits turned about."""
    if len(first) != len(second) or first == second:
        return None
    # Each bit of every byte, as one long number: bit N of byte K is bit K of column N.
    columns = [[sum(((byte >> bit) & 1) << place for place, byte in enumerate(data)) for bit in range(8)]
               for data in (first, second)]
    moves = []
    for bit in range(8):
        sources = [source for source in range(8) if columns[0][source] == columns[1][bit]]
        if not sources:
            return None
        moves.append(sources[0])
    # Two bits that go the same way through every byte both find the first of them, and that
    # is no way of moving eight bits about: the bytes do not say which is which.
    return moves if sorted(moves) == list(range(8)) else None


def move_bits(data: bytes, moves: list[int]) -> bytes:
    table = bytes(sum(((value >> moves[bit]) & 1) << bit for bit in range(8)) for value in range(256))
    return data.translate(table)


def stream_offset(bank: int, address: int) -> int | None:
    """Where in the ROM a stream starts, or None if that is not in the ROM."""
    if address < 0x4000:
        return address
    if address < 0x8000:
        return bank * 0x4000 + address - 0x4000
    return None


def resolve_shape(unpacker: dict, rom: bytes, bank: int, address: int) -> dict | None:
    """How one thing made is drawn, with the numbers a stream's own bytes
    give worked out. None if the unpacker's things are not drawn, and an
    error where this one cannot be drawn the way the declaration says."""
    shape = unpacker.get("as")
    if shape is None:
        return None
    if shape["kind"] == "tiles":
        return {"kind": "tiles", "bpp": shape.get("bpp", 2), "width": shape.get("width", 16)}
    where = f"the stream of {unpacker['name']} at {bank:02X}:{address:04X}"
    resolved = {"kind": "planes", "order": shape["order"]}
    for field in ("tiles_wide", "tiles_high"):
        value = shape[field]
        if isinstance(value, dict):
            start = stream_offset(bank, address)
            if start is None or start + value["stream"] >= len(rom):
                raise UnpackedError(f"{where} is not in the ROM, so '{field}' cannot be read from it")
            value = (rom[start + value["stream"]] >> value.get("shift", 0)) & value.get("mask", 0xFF)
        resolved[field] = value
    wide, high = resolved["tiles_wide"], resolved["tiles_high"]
    plane = wide * high * 8
    if wide < 1 or high < 1 or any(plane > size for size in made_sizes(unpacker)):
        raise UnpackedError(f"{where}: a picture {wide} tiles by {high} does not fit the "
                            f"{min(made_sizes(unpacker))} bytes a plane has")
    return resolved


def to_colours(made: bytes, sizes: list[int], shape: dict) -> tuple[int, int, bytes]:
    """A planes picture as (width, height, the colour number of each pixel)."""
    wide, high = shape["tiles_wide"], shape["tiles_high"]
    across, down = wide * 8, high * 8
    colours = bytearray(across * down)
    start = 0
    for plane, size in enumerate(sizes):
        for index in range(wide * down):
            column, y = divmod(index, down) if shape["order"] == "columns" else divmod(index, wide)[::-1]
            byte = made[start + index]
            at = y * across + column * 8
            for bit in range(8):
                colours[at + bit] |= ((byte >> (7 - bit)) & 1) << plane
        start += size
    return across, down, bytes(colours)


def from_colours(colours: bytes, made: bytes, sizes: list[int], shape: dict) -> bytes:
    """What was made, with a planes picture written into it: the bytes the
    picture does not show stay as the routine made them."""
    wide, high = shape["tiles_wide"], shape["tiles_high"]
    across, down = wide * 8, high * 8
    out = bytearray(made)
    start = 0
    for plane, size in enumerate(sizes):
        for index in range(wide * down):
            column, y = divmod(index, down) if shape["order"] == "columns" else divmod(index, wide)[::-1]
            at = y * across + column * 8
            byte = 0
            for bit in range(8):
                byte = (byte << 1) | ((colours[at + bit] >> plane) & 1)
            out[start + index] = byte
        start += size
    return bytes(out)
