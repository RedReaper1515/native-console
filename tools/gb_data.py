#!/usr/bin/env python3
"""A Game Boy ROM's data as files, and the ROM put together again from them.

    gb_data.py split EXPORT --rom ROM -o DIR    one file for each region of the ROM, and a manifest
    gb_data.py tiles DIR FILE                   a data file's tiles as a picture that can be edited
    gb_data.py build DIR -o IMAGE               the ROM image those files make
    gb_data.py status DIR                       which files have been edited
    gb_data.py uses DIR RECORD [RECORD ...]     what the game was seen to do with each region
    gb_data.py annotations DIR RECORD ... -o F  what it showed to be data, for the analysis to be told
    gb_data.py glyphs DIR STATE                 a picture of the game's letters, to write the charmap from
    gb_data.py text DIR                         the strings of each region as a file of text to edit
    gb_data.py unpackers DIR RECORD ... -o F    which routine unpacks, proposed from a recording
    gb_data.py unpacked DIR HARVEST ...         what the game's own routine unpacks, as files to edit
    gb_data.py watch-pack DIR -o PACK           a pack that only watches, whatever is edited in the tree
    gb_data.py pointers DIR RECORD [RECORD ...] which pointers lead to each thing the game read, and what can move
    gb_data.py free DIR -o DIR/free.json        the empty ends of banks, proposed as room for what grows
    gb_data.py unpack DIR --game GAME -o OUT    the game unpacking streams nobody played as far as
    gb_data.py names DIR [--sym FILE ...]       the files called by name, as names.sym says
    gb_data.py maps DIR                         the tile maps of each region as tile numbers to edit, and a picture
    gb_data.py tiles DIR                        a picture of every run of tiles the game was seen to draw
    gb_data.py split EXPORT --rom ROM -o NEW --keep OLD   a new export's tree, with what an old tree holds
    gb_data.py streams DIR                      the commands of the game's music and sound as text to edit

`split` takes a lift export (`gbrecomp --lift-export`), which lists every
run of ROM bytes no decoded instruction covers. Each such run is a data
region and becomes a file; so does each run of code between them, and the
cartridge header. Nothing is left out: put back together in the manifest's
order, the files are the ROM, byte for byte. `build` checks that by hash
every time.

A data file can be edited, as long as it keeps its size, and the image built
from the edited files is what a generated game loads with `--rom-image`. Code
and the header cannot: the game's C was compiled from those bytes, so `build`
refuses a change to them, and the game refuses an image that has one. To
change code, lift it and edit the C (LIFTING.md).

`tiles` says that a data file, or a stretch of one, is uncompressed tiles,
and draws them as a PNG beside it. The picture is then another way to edit
those bytes: `build` reads a picture that has been edited back into tile
bytes, and leaves the file as it is where the picture is as `tiles` wrote it.

Make the export with the options the game was generated with (`--lift-edges`
and the rest). What is code is what the analysis decoded, and a game refuses
an image that differs where its own code is.

`uses` takes what a game run with `--data-use FILE` recorded: for every byte
it read from the ROM as data, whether the byte, or a value worked out from
it, went to the tile area of video memory, to a tile map, to the sprite
table, to a sound register or a palette, was written to the cartridge as a
bank number, or was half of an address the game jumped to or read through.
It is written into the manifest region by region (`seen`), which is what
says that this stretch of a region is pictures and that one text.

`annotations` takes the same records and writes the bytes the game showed
to be data, the ones it used as they are (drew, played, sent to the
cartridge as a bank number, or took for half of an address), as a file for
`gbrecomp --annotations`. The analysis decodes some data as
instructions, and such bytes are then in files of code, which cannot be
edited; told that they are data, it leaves them alone, and a tree split from
an export made that way has them in data files.

`glyphs` and `text` make text of the strings, which are found from what
`uses` wrote (gb_data_text.py says how). `glyphs` draws the game's letters
out of a save state, for somebody to write `charmap.json` from; `text` then
writes the strings of each region to a file beside it, and `build` reads an
edited string back into the bytes.

`unpacked` is for data that lies packed in the ROM, which no file of the
tree can show. The game's own routine unpacks it, and gb_unpacked.py says
how that is used: with `unpackers.json` in the tree saying which routine it
is (`unpackers` proposes it, from a game run with --data-use-follow on the
instruction that reads the packed bytes; gb_data_unpackers.py says how), `build` writes a pack beside the image (`IMAGE.gbup`), a game given the
pack writes out what the routine made (`--unpacked-harvest`), `unpacked`
puts that into the tree as pictures, and `build` then puts each edited one
into the pack for the game to have in place of what its routine makes.

`watch-pack` writes the pack with no replacement in it, from a tree in which
pictures have been edited as well as from one in which none has: the pack
`build` wrote before the first edit. That is the pack to harvest with over a
run recorded with nothing replaced (RUN_RECORDS.md), which is not replayed
with a pack that replaces something.

`pointers` and `free` are for data that grows (gb_data_pointers.py says
how). A file keeps its size and its place, so a string made longer has to go
somewhere else, and everything that reads it has to be told. `pointers`
takes records of a game run with --data-use-pointers and writes into the
manifest the things the game read and the pointers that led to each
(`things`); `free` proposes the empty ends of banks as room, which
`free.json` in the tree declares; and `build` then moves the thing a string
grew in into that room and turns its pointers, where every one of them is
data. `build --move-all` moves all that can move and spoils where it was:
a game that still plays the same from that image read none of it there.

`unpack` gets the streams nobody played as far as. The game is started once
for each, enters its own function with the stream's bank and address set,
and stops as the routine returns (`--unpacked-call`), with a harvest
written. The streams are those of a list, of the tables of pointers that
`unpackers.json` declares, or of a walk on from where the known ones end,
which is a guess and is told as one (gb_data_unpack.py says how).

`names` calls the files by name. `names.sym`, at the top of the tree, is an
RGBDS symbol file that anybody can write, and `--sym` brings into it the
labels of a symbol file somebody has that lie on what the tree has. A file
is renamed and nothing in it changes, the manifest follows, and every
command that writes files afterwards keeps the names (gb_data_names.py).

A record also says what the screen showed of the tiles (`shown`): which layer
drew them and through which palette. `uses` writes that into the manifest,
and `tiles` draws a picture of such tiles in the palette most seen, as an
indexed PNG whose pixels are still the colour numbers (gb_data_colours.py
says how one is read back); `tiles --greys` draws the plain greys.

`maps` is for the bytes that reach a tile map as they are and are not text:
the tile numbers of a screen, or of the blocks a map is built from. A record
says where they lay on the screen and which tiles they drew (gb_data_maps.py
says how that makes rows and grids). `maps` writes them to a file of tile
numbers beside the region's file, which `build` reads back, and draws a
picture of them with those tiles, for looking at.

`tiles` with no file named draws the tiles nobody has said the place of: the
runs of bytes that a game recorded with --data-use-tiles copied to the tile
area as they are, each from its first whole tile, as many tiles and at the
bits a pixel the game showed (gb_data_tiles.py says how). It leaves alone
what somebody told it of or edited, and writes a sheet and a list of all the
tree's pictures of tiles to look through.

`split --keep OLD` is for splitting again, which is ordinary work once the
analysis is told more: the new export cuts the ROM into other regions. It
writes that export's tree to a new folder and takes over what was done in the
old one (edits, pictures, text, what the game unpacks), each by the bytes of
the ROM it is of; gb_data_keep.py says how. `split --force`, which writes a
tree afresh where one is, does not write over edits unless told to.

`streams` is for music and sound, which in a ROM are lists of commands for
the game's own sound engine. `streams.json`, at the top of the tree, says
what the commands are and where the lists start; `streams` follows every
start through its commands and what they call and loop to, and writes each
region's passages to a file of text beside it, which `build` reads back.
A passage keeps its place and cannot grow there (gb_data_streams.py).

The files are the game's data: keep them out of the repository, as the ROM is.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import zlib

import gb_data_pointers
import gb_data_names
import gb_data_colours
import gb_data_maps
import gb_data_text
import gb_unpacked
import gb_data_tiles
import gb_data_streams

SCHEMA = "gbrecomp.data-tree"
EXPORT_SCHEMA = "gbrecomp.lift-export"
BANK = 0x4000
PINNED = ("code", "header")
USE_SCHEMA = "gbrt.data-use"
USES = ("tiles", "tile_map", "oam", "sound", "palette", "bank", "jump", "pointer")
REGION_FIELDS = {"offset", "size", "bank", "address", "kind", "guess", "file", "sha256", "referenced_by", "seen",
                 "views", "tile_runs",
                 "labels"}
UNPACKED_FIELDS = {"unpacker", "bank", "address", "made", "made_sha256", "file", "sha256", "shape", "twin",
                   "worked_out"}
VIEW_FIELDS = {"tiles": {"kind", "file", "offset", "tiles", "width", "bpp", "sha256", "data_sha256"},
               "text": {"kind", "file", "charmap", "strings", "sha256", "data_sha256"}}
# What a view may say besides, and the one thing it can say there, or None where it can say anything: `from`,
# on a picture `tiles DIR` made from what was seen and nobody told it of, and `palette`, the colours a picture
# of tiles was drawn in (gb_data_colours.py).
# And `read`, on a view of text: which of its strings nobody saw printed, and `text --read` found by reading.
VIEW_OPTIONAL = {"tiles": {"from": gb_data_tiles.MADE, "palette": None}, "text": {"read": None}}
# The grey each colour of a tile is drawn in, by bits a pixel: colour 0 is white.
SHADES = {2: (255, 170, 85, 0), 1: (255, 0)}
# And back: the colour whose grey a brightness is nearest to.
NEAREST = {bpp: bytes(min(range(len(shades)), key=lambda colour: abs(shades[colour] - level))
                      for level in range(256))
           for bpp, shades in SHADES.items()}
# What `uses` writes on a region besides `seen`: `shown`, the palettes its tiles were drawn through.
REGION_FIELDS |= {"shown"}
# And `mapped`, the rows its tile numbers lay in on the screen; a view of those is a file of tile numbers.
REGION_FIELDS |= {"mapped"}
VIEW_FIELDS["map"] = {"kind", "file", "grids", "sha256", "data_sha256"}
CHARMAP = "charmap.json"          # what each byte of text draws, at the top of the tree
# A view of the commands of music and sound: the passages a region has of them (gb_data_streams.py).
VIEW_FIELDS["stream"] = {"kind", "file", "declaration", "passages", "sha256", "data_sha256"}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# The PNGs `read_png` reads: for each colour type, the bit depths, and the samples in a pixel.
PNG_READ = {0: ((1, 2, 4, 8), 1), 2: ((8,), 3), 3: ((1, 2, 4, 8), 1), 4: ((8,), 2), 6: ((8,), 4)}


class DataError(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def place(offset: int) -> tuple[int, int]:
    """The bank and the address a ROM offset is mapped at."""
    bank = offset // BANK
    return bank, offset if bank == 0 else BANK + offset % BANK


def split_regions(export: dict, rom: bytes) -> list[dict]:
    """Every byte of the ROM in exactly one region, in order: the export's
    data regions, and between them the code, cut where a bank ends."""
    data = sorted(export["data_regions"], key=lambda region: region["rom_offset"])
    regions: list[dict] = []
    cursor = 0

    def code_until(end: int) -> None:
        nonlocal cursor
        while cursor < end:
            stop = min(end, (cursor // BANK + 1) * BANK)
            bank, address = place(cursor)
            regions.append({"offset": cursor, "size": stop - cursor, "bank": bank, "address": address,
                            "kind": "code"})
            cursor = stop

    for region in data:
        offset, size = region["rom_offset"], region["size"]
        if offset < cursor or size <= 0 or offset + size > len(rom):
            raise DataError(f"the export's data region at offset {offset:#x} overlaps another or leaves the ROM")
        code_until(offset)
        kind = "header" if region["guess"] == "header" else "data"
        entry = {"offset": offset, "size": size, "bank": region["bank"], "address": region["address"],
                 "kind": kind, "guess": region["guess"]}
        if region.get("referenced_by"):
            entry["referenced_by"] = list(region["referenced_by"])
        regions.append(entry)
        cursor = offset + size
    code_until(len(rom))
    return regions


def file_name(region: dict) -> str:
    what = region["kind"] if region["kind"] != "data" else region.get("guess", "data")
    return f"bank_{region['bank']:02X}/{region['address']:04X}_{what}.bin"


def write_manifest(directory: Path, manifest: dict) -> dict | None:
    """Whole or not at all: the manifest is the only record of what was split and what was drawn.
    In a tree that has names (gb_data_names.py), every file is first called what names.sym
    calls it, whichever command made it; what that did is returned, for `names` to print."""
    named = gb_data_names.apply(directory, manifest)
    fresh = directory / "manifest.json.new"
    try:
        fresh.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8", newline="\n")
        fresh.replace(directory / "manifest.json")
    except OSError as error:
        raise DataError(f"cannot write {directory / 'manifest.json'}: {error}") from error
    gb_data_names.written(directory)
    return named


def command_split(args: argparse.Namespace) -> int:
    try:
        export = json.loads(Path(args.export).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise DataError(f"cannot read the export {args.export}: {error}") from error
    if export.get("schema") != EXPORT_SCHEMA:
        raise DataError(f"{args.export} is not a {EXPORT_SCHEMA} file")
    rom = Path(args.rom).read_bytes()
    if len(rom) != export["rom"]["size"] or sha256(rom) != export["rom"]["sha256"]:
        raise DataError(f"{args.rom} is not the ROM {args.export} was made from")
    import gb_data_keep         # a tree split again, and what the old one holds: only this command needs it

    out = Path(args.output)
    gone: list[str] = []
    if (out / "manifest.json").exists():
        if not args.force:
            raise DataError(f"{out} already holds a data tree: --force writes over one that has no edits, and "
                            f"`-o NEW --keep {out}` takes what it holds into a new one")
        if not args.discard_edits:
            gone = gb_data_keep.written_over(sys.modules[__name__], out)
    regions = split_regions(export, rom)
    if args.keep:
        return gb_data_keep.split_keeping(sys.modules[__name__], export, rom, regions, out, Path(args.keep),
                                          args.lose_edits_in_code)
    for region in regions:
        content = rom[region["offset"]:region["offset"] + region["size"]]
        region["file"] = file_name(region)
        region["sha256"] = sha256(content)
        target = out / region["file"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    manifest = {
        "schema": SCHEMA, "version": 1,
        "rom": {"size": len(rom), "sha1": hashlib.sha1(rom).hexdigest(), "sha256": sha256(rom),
                "title": export["rom"].get("title", "")},
        "regions": regions,
    }
    write_manifest(out, manifest)
    counts: dict[str, list[int]] = {}
    for region in regions:
        entry = counts.setdefault(region["kind"], [0, 0])
        entry[0] += 1
        entry[1] += region["size"]
    print(f"wrote {len(regions)} files and manifest.json to {out}")
    for kind in ("data", "code", "header"):
        if kind in counts:
            note = {"data": "", "code": "  (not to be edited: the game's C was compiled from it)",
                    "header": "  (not to be edited: it says what the cartridge is)"}[kind]
            print(f"  {kind:6s} {counts[kind][0]:6,d} files, {counts[kind][1]:9,d} bytes{note}")
    if gone:
        print(f"  the tree that was there also held {', '.join(gone)}: the new one does not (`--keep` takes them "
              f"into a new tree)")
    return 0


def inside(directory: Path, name: str) -> Path:
    """A path inside the tree, written with forward slashes: nothing that
    climbs out of it, names a drive, or starts at the root."""
    parts = PurePosixPath(name).parts
    if not parts or ".." in parts or name.startswith("/") or ":" in name or "\\" in name:
        raise DataError(f"{name} is not a file inside {directory}")
    return directory.joinpath(*parts)


def spans(view: dict) -> list[tuple[int, int]]:
    """The bytes of its region a view is of, as (first, one past the last)."""
    if view["kind"] == "text":
        return [(offset, offset + size) for offset, size in view["strings"]]
    if view["kind"] == "map":
        return [(offset, offset + width * rows) for offset, width, rows in view["grids"]]
    if view["kind"] == "stream":
        return [(offset, offset + size) for offset, size, _ in view["passages"]]
    return [(view["offset"], view["offset"] + view["tiles"] * 8 * view["bpp"])]


def check_text_view(directory: Path, region: dict, view: dict) -> None:
    if any(type(view[field]) is not str for field in ("file", "charmap", "sha256", "data_sha256")) or \
            not isinstance(view["strings"], list) or not view["strings"] or \
            any(not isinstance(string, list) or len(string) != 2 or any(type(number) is not int for number in string)
                for string in view["strings"]):
        raise DataError(f"a text view of {region['file']} has a field of the wrong type")
    end = 0
    for offset, size in view["strings"]:
        if offset < end or size < 1 or offset + size > region["size"]:
            raise DataError(f"the strings of {view['file']} are not inside {region['file']}, one after another")
        end = offset + size
    inside(directory, view["file"])
    inside(directory, view["charmap"])


def check_map_view(directory: Path, region: dict, view: dict) -> None:
    if any(type(view[field]) is not str for field in ("file", "sha256", "data_sha256")) or \
            not isinstance(view["grids"], list) or not view["grids"] or \
            any(not isinstance(grid, list) or len(grid) != 3 or any(type(number) is not int for number in grid)
                for grid in view["grids"]):
        raise DataError(f"a map view of {region['file']} has a field of the wrong type")
    end = 0
    for offset, width, rows in view["grids"]:
        if offset < end or width < 1 or rows < 1 or offset + width * rows > region["size"]:
            raise DataError(f"the grids of {view['file']} are not inside {region['file']}, one after another")
        end = offset + width * rows
    inside(directory, view["file"])


def check_stream_view(directory: Path, region: dict, view: dict) -> None:
    if any(type(view[field]) is not str for field in ("file", "declaration", "sha256", "data_sha256")) or \
            not isinstance(view["passages"], list) or not view["passages"] or \
            any(not isinstance(passage, list) or len(passage) != 3 or type(passage[0]) is not int or
                type(passage[1]) is not int or type(passage[2]) is not str for passage in view["passages"]):
        raise DataError(f"a stream view of {region['file']} has a field of the wrong type")
    end = 0
    for offset, size, _ in view["passages"]:
        if offset < end or size < 1 or offset + size > region["size"]:
            raise DataError(f"the passages of {view['file']} are not inside {region['file']}, one after another")
        end = offset + size
    inside(directory, view["file"])
    inside(directory, view["declaration"])


def check_views(directory: Path, region: dict) -> None:
    """A region's views have to be ones this tool can read back, each of bytes
    the region has, and no two of the same bytes."""
    views = region["views"]
    if region["kind"] != "data":
        raise DataError(f"{region['file']} is {region['kind']}, and the manifest has views of it: only data has "
                        f"them")
    if not isinstance(views, list):
        raise DataError(f"the views of {region['file']} are not a list")
    for view in views:
        kind = view.get("kind") if isinstance(view, dict) else None
        if kind not in VIEW_FIELDS:
            raise DataError(f"{region['file']} has a view of kind {kind!r}, and this gb_data.py reads 'tiles' and "
                            f"'text', and 'map' and 'stream'")
        optional = VIEW_OPTIONAL.get(kind, {})
        for field in optional:
            if optional[field] is not None and field in view and view[field] != optional[field]:
                raise DataError(f"a {kind} view of {region['file']} has a `{field}` that is not one: `{field}` can "
                                f"only be '{optional[field]}'")
        if set(view) - set(optional) != VIEW_FIELDS[kind]:
            odd = sorted((set(view) - set(optional)) ^ VIEW_FIELDS[kind])[0]
            raise DataError(f"a {kind} view of {region['file']} "
                            f"{'has no such field as' if odd in view else 'lacks the field'} '{odd}'")
        if kind == "text":
            check_text_view(directory, region, view)
            continue
        if kind == "map":
            check_map_view(directory, region, view)
            continue
        if kind == "stream":
            check_stream_view(directory, region, view)
            continue
        numbers = [view[field] for field in ("offset", "tiles", "width", "bpp")]
        words = [view[field] for field in ("file", "sha256", "data_sha256")]
        if any(type(number) is not int for number in numbers) or any(type(word) is not str for word in words):
            raise DataError(f"a tiles view of {region['file']} has a field of the wrong type")
        if view["bpp"] not in SHADES or view["width"] < 1:
            raise DataError(f"{view['file']} cannot be {view['width']} tiles to a row at {view['bpp']} bits a "
                            f"pixel")
        if view["offset"] < 0 or view["tiles"] < 1 or \
                view["offset"] + view["tiles"] * 8 * view["bpp"] > region["size"]:
            raise DataError(f"the tiles of {view['file']} are not inside {region['file']}")
        if "palette" in view and not gb_data_colours.check_palette(view["palette"], view["bpp"]):
            raise DataError(f"the palette of {view['file']} is not a colour for each colour of a tile")
        inside(directory, view["file"])
    end = 0
    for start, stop, view in sorted(((start, stop, view) for view in views for start, stop in spans(view)),
                                    key=lambda span: span[:2]):
        if start < end:
            raise DataError(f"{view['file']} is of bytes of {region['file']} that another view has")
        end = stop


def check_unpacked(directory: Path, things) -> None:
    """The manifest's list of what the game's routines unpacked has to be one this tool wrote."""
    if not isinstance(things, list):
        raise DataError("the manifest's unpacked things are not a list")
    for thing in things:
        if not isinstance(thing, dict):
            raise DataError("an unpacked thing of the manifest is not an object")
        unknown = sorted(set(thing) - UNPACKED_FIELDS)
        missing = sorted(UNPACKED_FIELDS - {"shape", "twin", "worked_out"} - set(thing))
        if unknown:
            raise DataError(f"an unpacked thing of the manifest has no such field as '{unknown[0]}'")
        if missing:
            raise DataError(f"an unpacked thing of the manifest lacks the field '{missing[0]}'")
        words = [thing[field] for field in ("unpacker", "made", "made_sha256", "file", "sha256")]
        if any(type(word) is not str for word in words) or type(thing["bank"]) is not int or \
                type(thing["address"]) is not int or not gb_unpacked.NAME.fullmatch(thing["unpacker"]) or \
                ("shape" in thing and (not isinstance(thing["shape"], dict) or
                                       thing["shape"].get("kind") not in ("tiles", "planes"))):
            raise DataError("an unpacked thing of the manifest has a field of the wrong type")
        if thing.get("worked_out", True) is not True:
            raise DataError("an unpacked thing of the manifest has a field of the wrong type")
        twin = thing.get("twin", {"made_sha256": "", "bits": list(range(8))})
        if not isinstance(twin, dict) or set(twin) != {"made_sha256", "bits"} or \
                type(twin["made_sha256"]) is not str or not isinstance(twin["bits"], list) or \
                sorted(bit for bit in twin["bits"] if type(bit) is int) != list(range(8)):
            raise DataError("an unpacked thing of the manifest has a twin that is not one")
        inside(directory, thing["made"])
        inside(directory, thing["file"])


def load_tree(directory: Path) -> tuple[dict, list[tuple[dict, bytes]]]:
    """The manifest, and each region with what its file holds now."""
    gb_data_names.recover(directory)        # a renaming that was cut short is taken back first
    try:
        manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise DataError(f"cannot read {directory / 'manifest.json'}: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA or manifest.get("version") != 1:
        raise DataError(f"{directory / 'manifest.json'} is not a version 1 {SCHEMA} manifest")
    unknown = sorted(set(manifest) - {"schema", "version", "rom", "regions", "readers", "unpacked", "names",
                                      "things"})
    if unknown:
        raise DataError(f"the manifest has no such field as '{unknown[0]}'")
    try:
        gb_data_names.check(manifest)
    except gb_data_names.NamesError as error:
        raise DataError(str(error)) from error
    if "unpacked" in manifest:
        check_unpacked(directory, manifest["unpacked"])
    size = manifest["rom"]["size"]
    loaded = []
    cursor = 0
    for region in manifest["regions"]:
        unknown = sorted(set(region) - REGION_FIELDS)
        if unknown:
            raise DataError(f"a region of the manifest has no such field as '{unknown[0]}'")
        if region["offset"] != cursor or region["size"] <= 0:
            raise DataError(f"the manifest's regions do not follow on at offset {cursor:#x}")
        cursor += region["size"]
        try:
            content = inside(directory, region["file"]).read_bytes()
        except OSError as error:
            raise DataError(f"cannot read {region['file']}: {error}") from error
        if len(content) != region["size"]:
            raise DataError(f"{region['file']} is {len(content)} bytes and its region is {region['size']}: a "
                            f"region keeps its size (growing and moving regions is not built yet)")
        if "views" in region:
            check_views(directory, region)
        loaded.append((region, content))
    if cursor != size:
        raise DataError(f"the manifest's regions cover {cursor:#x} bytes of a ROM of {size:#x}")
    return manifest, loaded


def edited(loaded: list[tuple[dict, bytes]]) -> list[tuple[dict, int]]:
    """The regions whose file is not what `split` wrote, with how many bytes differ in size terms."""
    return [(region, len(content)) for region, content in loaded if sha256(content) != region["sha256"]]


def describe(region: dict) -> str:
    return f"{region['file']} ({region['bank']:02X}:{region['address']:04X}, {region['size']} bytes)"


def refuse_pinned(changes: list[tuple[dict, int]]) -> None:
    for region, _ in changes:
        if region["kind"] in PINNED:
            what = ("code the game's C was compiled from: lift the function and edit the C instead"
                    if region["kind"] == "code" else
                    "the cartridge header, which says what the cartridge is")
            raise DataError(f"{describe(region)} has been edited, and it is {what}")


# ---------------------------------------------------------------------------
# PNG, with the standard library only


def write_png(width: int, height: int, grey: bytes) -> bytes:
    """An 8-bit greyscale PNG of those pixels."""
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    rows = b"".join(b"\x00" + grey[y * width:(y + 1) * width] for y in range(height))
    return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def paeth(left: int, up: int, up_left: int) -> int:
    guess = left + up - up_left
    by_left, by_up, by_up_left = abs(guess - left), abs(guess - up), abs(guess - up_left)
    if by_left <= by_up and by_left <= by_up_left:
        return left
    return up if by_up <= by_up_left else up_left


def brightness(red: int, green: int, blue: int) -> int:
    """How light a colour is, 0 to 255, as a greyscale picture of it would have it. A grey is itself."""
    return (299 * red + 587 * green + 114 * blue + 500) // 1000


def png_lines(name: str, data: bytes) -> tuple[int, int, int, int, bytes, bytes, list[bytearray]]:
    """A PNG unpacked: its width and height, its bits a sample and its colour
    type, its palette and what it says is clear (PLTE and tRNS, or nothing),
    and its rows of samples with the row filters undone.

    Reads what a picture editor saves: greyscale and indexed pictures of up to
    8 bits a pixel, and 8-bit greyscale with alpha, RGB and RGBA. Not
    interlaced pictures, and not 16 bits a sample."""
    if not data.startswith(PNG_SIGNATURE):
        raise DataError(f"{name} is not a PNG")
    header = None
    palette = b""
    clear = b""
    packed = bytearray()
    position = len(PNG_SIGNATURE)
    while True:
        if position + 8 > len(data):
            raise DataError(f"{name} is cut short: it is a PNG without its end")
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        body = data[position + 8:position + 8 + length]
        check = data[position + 8 + length:position + 12 + length]
        if len(body) != length or len(check) != 4:
            raise DataError(f"{name} is cut short: it is a PNG without its end")
        if zlib.crc32(kind + body) != struct.unpack(">I", check)[0]:
            raise DataError(f"{name} is damaged: its {kind.decode('latin-1')} chunk fails its check")
        position += 12 + length
        if kind == b"IHDR" and length == 13:
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            clear = body
        elif kind == b"IDAT":
            packed += body
        elif kind == b"IEND":
            break
    if header is None:
        raise DataError(f"{name} is not a PNG: it has no header")
    width, height, depth, colour, _, _, interlace = header
    if interlace:
        raise DataError(f"{name} is an interlaced PNG, which is not read: save it without interlacing")
    if colour not in PNG_READ or depth not in PNG_READ[colour][0]:
        what = {0: "greyscale", 2: "RGB", 3: "indexed", 4: "greyscale with alpha", 6: "RGBA"}.get(colour, "unknown")
        raise DataError(f"{name} is a PNG of a kind that is not read ({what}, {depth} bits a sample): save it as "
                        f"8-bit greyscale, RGB or RGBA, or as an indexed picture")
    samples = PNG_READ[colour][1]
    stride = (width * samples * depth + 7) // 8
    back = max(1, samples * depth // 8)      # how far back the byte a filter calls "left" is
    try:
        raw = zlib.decompress(bytes(packed))
    except zlib.error as error:
        raise DataError(f"{name} is damaged: {error}") from error
    if len(raw) < height * (stride + 1):
        raise DataError(f"{name} is damaged: it has fewer pixels than its {width}x{height}")

    lines = []
    above = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        if kind == 1:
            for i in range(back, stride):
                line[i] = (line[i] + line[i - back]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + above[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = line[i - back] if i >= back else 0
                line[i] = (line[i] + (left + above[i]) // 2) & 0xFF
        elif kind == 4:
            for i in range(stride):
                left, up_left = (line[i - back], above[i - back]) if i >= back else (0, 0)
                line[i] = (line[i] + paeth(left, above[i], up_left)) & 0xFF
        elif kind != 0:
            raise DataError(f"{name} is damaged: there is no PNG filter {kind}")
        lines.append(line)
        above = line
    return width, height, depth, colour, palette, clear, lines


def read_png(name: str, data: bytes) -> tuple[int, int, bytes]:
    """A PNG's width and height, and the brightness of each of its pixels.
    Alpha is ignored."""
    width, height, depth, colour, palette, _, lines = png_lines(name, data)
    samples = PNG_READ[colour][1]
    stride = (width * samples * depth + 7) // 8
    # The brightness of each value a pixel of one sample can have.
    levels: list[int] = []
    if colour == 3:
        if not palette or len(palette) % 3:
            raise DataError(f"{name} is damaged: it is an indexed PNG without a palette")
        levels = [brightness(*palette[i:i + 3]) for i in range(0, len(palette), 3)]
    elif colour == 0:
        top = (1 << depth) - 1
        levels = [value * 255 // top for value in range(top + 1)]
    grey = bytearray()
    for line in lines:
        if colour in (0, 3):
            values = line
            if depth < 8:                    # several pixels to a byte, the first in its high bits
                mask = (1 << depth) - 1
                values = [(line[x * depth >> 3] >> (8 - depth - (x * depth & 7))) & mask for x in range(width)]
            try:
                grey += bytes(levels[value] for value in values)
            except IndexError:
                raise DataError(f"{name} is damaged: a pixel has a colour its palette does not") from None
        elif colour == 4:
            grey += line[0::2]
        else:
            grey += bytes(brightness(line[i], line[i + 1], line[i + 2]) for i in range(0, stride, samples))
    return width, height, bytes(grey)


# ---------------------------------------------------------------------------
# Tiles as pictures


def draw_tiles(data: bytes, bpp: int, width: int) -> tuple[int, int, bytes]:
    """Tiles as a sheet of grey pixels, `width` tiles to a row: the sheet's
    width and height, and its pixels.

    A tile is 8 by 8 pixels. At 2 bits a pixel it is 16 bytes, two for each
    row of pixels: the low bit of every pixel's colour, then the high bit, bit
    7 the leftmost pixel. At 1 bit a pixel it is 8 bytes, one for each row."""
    step = 8 * bpp
    count = len(data) // step
    shades = SHADES[bpp]
    across = width * 8
    sheet = bytearray(b"\xff") * (across * 8 * -(-count // width))
    for tile in range(count):
        corner = tile // width * 8 * across + tile % width * 8
        for y in range(8):
            low = data[tile * step + y * bpp]
            high = data[tile * step + y * bpp + 1] if bpp == 2 else 0
            start = corner + y * across
            for x in range(8):
                sheet[start + x] = shades[((low >> (7 - x)) & 1) | (((high >> (7 - x)) & 1) << 1)]
    return across, len(sheet) // across, bytes(sheet)


def read_tiles(grey: bytes, count: int, bpp: int, width: int) -> bytes:
    """The other way: the bytes of the tiles a sheet of that shape shows, each
    pixel taken as the colour whose grey it is nearest to."""
    nearest = NEAREST[bpp]
    across = width * 8
    data = bytearray()
    for tile in range(count):
        corner = tile // width * 8 * across + tile % width * 8
        for y in range(8):
            low = high = 0
            for level in grey[corner + y * across:corner + y * across + 8]:
                colour = nearest[level]
                low = (low << 1) | (colour & 1)
                high = (high << 1) | (colour >> 1)
            data.append(low)
            if bpp == 2:
                data.append(high)
    return bytes(data)


def sheet_size(view: dict) -> tuple[int, int]:
    return view["width"] * 8, -(-view["tiles"] // view["width"]) * 8


def view_at(region: dict, view: dict) -> str:
    return f"{region['bank']:02X}:{region['address'] + view['offset']:04X}"


def tiles_shown(view: dict, png: bytes, there: bytes | None = None) -> bytes:
    """The tile bytes a view's picture shows now. `there` is the bytes those
    tiles have in the region's file, which a picture drawn in colours may need
    (gb_data_colours.py says when)."""
    # The size first, from the header a PNG starts with: a picture of another size is not worth decoding.
    if not png.startswith(PNG_SIGNATURE) or png[12:16] != b"IHDR" or len(png) < 24:
        raise DataError(f"{view['file']} is not a PNG")
    width, height = struct.unpack(">II", png[16:24])
    if (width, height) != sheet_size(view):
        wide, high = sheet_size(view)
        raise DataError(f"{view['file']} is {width}x{height} pixels, and its {view['tiles']} tiles, "
                        f"{view['width']} to a row, are {wide}x{high}: a picture keeps its size")
    if "palette" in view:
        # Drawn in the colours the game showed: a pixel is a colour number, and is read by its colour.
        bpp = view["bpp"]
        _, _, depth, colour, palette, clear, lines = png_lines(view["file"], png)
        before = None if there is None else draw_tiles(there, bpp, view["width"])[2].translate(NEAREST[bpp])
        try:
            numbers = gb_data_colours.numbers_shown(view["file"], width, depth, colour, palette, clear, lines,
                                                    view["palette"], before)
        except gb_data_colours.ColourError as error:
            raise DataError(str(error)) from error
        return read_tiles(numbers.translate(bytes(SHADES[bpp]).ljust(256, b"\0")), view["tiles"], bpp,
                          view["width"])
    _, _, grey = read_png(view["file"], png)
    return read_tiles(grey, view["tiles"], view["bpp"], view["width"])


def with_views(directory: Path, loaded: list[tuple[dict, bytes]],
               grown: list | None = None) -> tuple[list[bytes], list[str], list[str]]:
    """Each region's bytes, with the pictures and the text that have been
    edited read back into them.

    A string that was made longer cannot be read back into its region. Given
    a list in `grown`, it is put there for `move_things`, as (the region, the
    file of text, its offset in the region, its size there, its new bytes);
    without one it is refused.

    A picture that is as `tiles` wrote it changes nothing. Nor does one saved
    again that still shows the tiles it was drawn from, or the ones its
    region's file has now. Otherwise its tiles take the place of those bytes
    of the region, unless the file was edited there as well since the picture
    was drawn: then the two say different things, and neither is taken.
    Text goes by the same rules, a string at a time (`with_text`).

    Also returns a line for each picture or string that changes its region,
    and a line for each view that disagrees with its region's file."""
    contents, changed, disagree = [], [], []
    charmaps: dict[str, dict[int, str]] = {}
    declared: dict[str, dict] = {}      # the declarations of command streams, by file
    for region, content in loaded:
        for view in region.get("views", ()):
            name = view["file"]
            if view["kind"] == "text":
                content = with_text(directory, region, view, content, charmaps, changed, disagree, grown)
                continue
            if view["kind"] == "map":
                content = with_map(directory, region, view, content, changed, disagree)
                continue
            if view["kind"] == "stream":
                content = with_stream(directory, region, view, content, declared, changed, disagree)
                continue
            try:
                png = inside(directory, name).read_bytes()
            except OSError as error:
                raise DataError(f"cannot read {name}, the picture of the tiles at {view_at(region, view)} in "
                                f"{region['file']} ({error}): `tiles` draws it again") from error
            if sha256(png) == view["sha256"]:
                continue
            step = 8 * view["bpp"]
            start, size = view["offset"], view["tiles"] * step
            there = content[start:start + size]
            shown = tiles_shown(view, png, there)
            if shown == there or sha256(shown) == view["data_sha256"]:
                continue
            differ = sum(1 for i in range(0, size, step) if shown[i:i + step] != there[i:i + step])
            if sha256(there) != view["data_sha256"]:
                disagree.append(f"{name} and {region['file']} have both been edited, and disagree about {differ} "
                                f"of the {view['tiles']} tiles at {view_at(region, view)}")
                continue
            content = content[:start] + shown + content[start + size:]
            changed.append(f"{name} ({view_at(region, view)}, {differ} of {view['tiles']} tiles changed)")
        contents.append(content)
    return contents, changed, disagree


def with_text(directory: Path, region: dict, view: dict, content: bytes, charmaps: dict[str, dict[int, str]],
              changed: list[str], disagree: list[str], grown: list | None = None) -> bytes:
    """A region's bytes with the strings that were edited in its file of text
    written in. The same rules as a picture: a file of text that is as `text`
    wrote it changes nothing, a line that still says what the bytes say
    changes nothing, and where the region's file was edited inside the
    strings as well, a line that says something else is a disagreement."""
    name = view["file"]
    try:
        written = inside(directory, name).read_bytes()
    except OSError as error:
        raise DataError(f"cannot read {name}, the text of {region['file']} ({error}): `text` writes it again") \
            from error
    if sha256(written) == view["sha256"]:
        return content
    try:
        if view["charmap"] not in charmaps:
            charmaps[view["charmap"]] = gb_data_text.load_charmap(inside(directory, view["charmap"]))
        letters = charmaps[view["charmap"]]
        strings = [(offset, size) for offset, size in view["strings"]]
        text = written.decode("utf-8-sig")          # an editor may have put a byte order mark in front
        there = b"".join(content[offset:offset + size] for offset, size in strings)
        file_edited = sha256(there) != view["data_sha256"]
        if file_edited:
            # The region's file was edited inside the strings. Text that still says what it was
            # written from was only saved again, and the file stands.
            said = gb_data_text.said_bytes(name, text, strings, letters)
            if said is not None and sha256(said) == view["data_sha256"]:
                return content
        longer: list | None = None if grown is None else []
        edits = gb_data_text.edited_strings(name, text, content, strings, letters, longer)
    except UnicodeDecodeError as error:
        raise DataError(f"{name} is not text in UTF-8: {error}") from error
    except gb_data_text.TextError as error:
        raise DataError(str(error)) from error
    longer = longer or []
    if not edits and not longer:
        return content
    if file_edited:
        first = min(edit[0] for edit in edits + longer)
        disagree.append(f"{name} and {region['file']} have both been edited, and disagree about "
                        f"{len(edits) + len(longer)} of "
                        f"the {len(strings)} strings, the first at {region['bank']:02X}:"
                        f"{region['address'] + first:04X}")
        return content
    out = bytearray(content)
    for offset, new, text, note in edits:
        out[offset:offset + len(new)] = new
        changed.append(f"{name} ({region['bank']:02X}:{region['address'] + offset:04X}) |{text}|{note}")
    # A longer string stays as it was where it lies: what it is part of is moved, if it can be.
    sizes = dict(strings)
    for offset, new, text in longer:
        changed.append(f"{name} ({region['bank']:02X}:{region['address'] + offset:04X}) |{text}|   <- "
                       f"{len(new) - sizes[offset]} bytes longer: it has to be moved")
        grown.append((region, name, offset, sizes[offset], new))
    return bytes(out)


def with_map(directory: Path, region: dict, view: dict, content: bytes, changed: list[str],
             disagree: list[str]) -> bytes:
    """A region's bytes with the tile numbers that were edited in its file of
    tile maps written in. The same rules as a picture: a file that is as
    `maps` wrote it changes nothing, nor does one that says what it was
    written from or what the region's file says now, and where the region's
    file was edited in those bytes as well and says something else, that is
    a disagreement."""
    name = view["file"]
    try:
        written = inside(directory, name).read_bytes()
    except OSError as error:
        raise DataError(f"cannot read {name}, the tile maps of {region['file']} ({error}): `maps` writes it again") \
            from error
    if sha256(written) == view["sha256"]:
        return content
    grids = [tuple(grid) for grid in view["grids"]]
    try:
        said = gb_data_maps.said_bytes(name, written.decode("utf-8-sig"), grids)
    except UnicodeDecodeError as error:
        raise DataError(f"{name} is not text in UTF-8: {error}") from error
    except gb_data_maps.MapError as error:
        raise DataError(str(error)) from error
    there = b"".join(content[offset:offset + width * rows] for offset, width, rows in grids)
    if said == there or sha256(said) == view["data_sha256"]:
        return content
    differ = sum(1 for old, new in zip(there, said) if old != new)
    if sha256(there) != view["data_sha256"]:
        disagree.append(f"{name} and {region['file']} have both been edited, and disagree about {differ} of the "
                        f"{len(there)} tile numbers of its maps")
        return content
    out = bytearray(content)
    at = 0
    first = None
    for offset, width, rows in grids:
        new = said[at:at + width * rows]
        if first is None and new != content[offset:offset + width * rows]:
            first = offset
        out[offset:offset + width * rows] = new
        at += width * rows
    changed.append(f"{name} ({region['bank']:02X}:{region['address'] + first:04X}, {differ} of {len(there)} tile "
                   f"numbers changed)")
    return bytes(out)


def stream_said(directory: Path, region: dict, view: dict, content: bytes, written: bytes,
                declared: dict[str, dict]) -> list[tuple[bytes, int]]:
    """What a file of passages says its region's bytes are now, a passage at a time, with how
    many bytes shorter each has become."""
    name = view["file"]
    try:
        if view["declaration"] not in declared:
            declared[view["declaration"]] = gb_data_streams.load_declaration(inside(directory, view["declaration"]))
        return gb_data_streams.said(name, written.decode("utf-8-sig"), region,
                                    [tuple(passage) for passage in view["passages"]],
                                    declared[view["declaration"]]["grammars"], content)
    except UnicodeDecodeError as error:
        raise DataError(f"{name} is not text in UTF-8: {error}") from error
    except gb_data_streams.StreamsError as error:
        raise DataError(str(error)) from error


def with_stream(directory: Path, region: dict, view: dict, content: bytes, declared: dict[str, dict],
                changed: list[str], disagree: list[str]) -> bytes:
    """A region's bytes with the commands that were edited in its file of passages written in.
    The same rules as a file of tile numbers: a file that is as `streams` wrote it changes
    nothing, nor does one that says what it was written from or what the region's file says now,
    and where the region's file was edited in those bytes as well and says something else, that
    is a disagreement. A passage keeps its place (gb_data_streams.py says what that allows)."""
    name = view["file"]
    try:
        written = inside(directory, name).read_bytes()
    except OSError as error:
        raise DataError(f"cannot read {name}, the commands of {region['file']} ({error}): `streams` writes it "
                        f"again") from error
    if sha256(written) == view["sha256"]:
        return content
    passages = view["passages"]
    said = stream_said(directory, region, view, content, written, declared)
    there = b"".join(content[offset:offset + size] for offset, size, _ in passages)
    new = b"".join(made for made, _ in said)
    if new == there or sha256(new) == view["data_sha256"]:
        return content
    if sha256(there) != view["data_sha256"]:
        differ = sum(1 for (offset, size, _), (made, _) in zip(passages, said) if made != content[offset:offset + size])
        disagree.append(f"{name} and {region['file']} have both been edited, and disagree about {differ} of the "
                        f"{len(passages)} passages of commands")
        return content
    out = bytearray(content)
    for (offset, size, _), (made, shorter) in zip(passages, said):
        if made == content[offset:offset + size]:
            continue
        differ = sum(1 for old, byte in zip(content[offset:offset + size], made) if old != byte)
        out[offset:offset + size] = made
        changed.append(f"{name} ({region['bank']:02X}:{region['address'] + offset:04X}, {differ} of {size} bytes "
                       f"changed)" + (f"   <- {shorter} {'byte' if shorter == 1 else 'bytes'} shorter: what was after "
                                      f"its commands stays, and is not reached" if shorter else ""))
    return bytes(out)


def pictures(loaded: list[tuple[dict, bytes]]) -> int:
    return sum(len(region.get("views", ())) for region, _ in loaded)


def views_said(loaded: list[tuple[dict, bytes]]) -> str:
    """", and 3 pictures of tiles and 2 files of text", or nothing."""
    kinds = [view["kind"] for region, _ in loaded for view in region.get("views", ())]
    drawn, texts, maps = kinds.count("tiles"), kinds.count("text"), kinds.count("map")
    parts = ([f"{drawn} {'picture' if drawn == 1 else 'pictures'} of tiles"] if drawn else []) + \
            ([f"{texts} {'file' if texts == 1 else 'files'} of text"] if texts else []) + \
            ([f"{maps} {'file' if maps == 1 else 'files'} of tile maps"] if maps else []) + \
            ([f"{kinds.count('stream')} {'file' if kinds.count('stream') == 1 else 'files'} of commands"]
             if "stream" in kinds else [])
    return ", and " + " and ".join(parts) if parts else ""


def find_region(directory: Path, loaded: list[tuple[dict, bytes]], name: str) -> tuple[dict, bytes]:
    """The region a file belongs to: named as the manifest names it, or by a path to it."""
    names = [name.replace("\\", "/")]
    try:
        names.append(Path(name).resolve().relative_to(directory.resolve()).as_posix())
    except (OSError, ValueError):
        pass
    for region, content in loaded:
        if region["file"] in names or file_name(region) in names:       # by its name, or by its address as ever
            return region, content
    like = next((region["file"] for region, _ in loaded if region["kind"] == "data"), loaded[0][0]["file"])
    raise DataError(f"{name} is not a file of the manifest in {directory}: name it as the manifest does, like "
                    f"{like}")


def command_tiles(args: argparse.Namespace) -> int:
    if args.file is None:
        # No file named: the pictures the game was seen to draw, wherever they are.
        try:
            return gb_data_tiles.found(argparse.Namespace(**globals()), args)
        except gb_data_tiles.TilesError as error:
            raise DataError(str(error)) from error
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    region, content = find_region(directory, loaded, args.file)
    name = region["file"]
    if region["kind"] != "data":
        what = "code" if region["kind"] == "code" else "the cartridge header"
        raise DataError(f"{name} is {what}, and only data can be drawn as tiles")
    bpp, step, offset = args.bpp, 8 * args.bpp, args.offset
    if not 0 <= offset < region["size"]:
        raise DataError(f"{name} is {region['size']} bytes: it has no offset {offset}")
    room = region["size"] - offset
    count = args.tiles
    if count is None:
        if room % step:
            raise DataError(f"{name} has {room} bytes from offset {offset}, which is not a whole number of "
                            f"{step}-byte tiles ({room // step}, and {room % step} bytes over): say how many are "
                            f"tiles with --tiles")
        count = room // step
    elif count < 1 or count * step > room:
        raise DataError(f"{name} has room for {room // step} {step}-byte tiles from offset {offset}, not {count}")
    if args.width < 1:
        raise DataError("a row of the picture has at least one tile")
    width = min(args.width, count)

    stem = name[:-len(".bin")] if name.endswith(".bin") else name
    picture = f"{stem}.tiles.png" if offset == 0 else f"{stem}.tiles_{region['address'] + offset:04X}.png"
    picture = gb_data_names.kept(region, "tiles", offset, picture)      # a picture that has a name keeps it
    target = inside(directory, picture)
    kept = []
    gone = []
    drawn_before = False
    for view in region.get("views", []):
        if view["file"] == picture:
            drawn_before = True
            if not args.force and target.exists() and sha256(target.read_bytes()) != view["sha256"]:
                raise DataError(f"{picture} has been edited: --force draws it again from {name}, and the edit is "
                                f"lost")
            continue
        if any(start < offset + count * step and offset < stop for start, stop in spans(view)):
            if gb_data_tiles.gives_way(inside(directory, view["file"]), view, args.force):
                gone.append(inside(directory, view["file"]))      # found from what was seen: it makes room
                continue
            raise DataError(f"those bytes of {name} are already drawn in {view['file']} "
                            f"({view['tiles']} tiles from offset {view['offset']})" if view["kind"] == "tiles" else
                            f"those bytes of {name} are tile numbers of a map, which {view['file']} has"
                            if view["kind"] == "map" else
                            f"those bytes of {name} are commands of music or sound, which {view['file']} has"
                            if view["kind"] == "stream" else
                            f"those bytes of {name} have strings in them, which {view['file']} has")
        kept.append(view)
    if target.exists() and not drawn_before and not args.force:
        raise DataError(f"{picture} is already there and the manifest does not know it: --force writes over it")

    data = content[offset:offset + count * step]
    # In the colours the game showed these tiles in, where a record says (`uses`), and in greys otherwise.
    seen = None if args.greys else gb_data_colours.most_seen(region.get("shown", ()), offset, count * step, bpp)
    across, down, grey = draw_tiles(data, bpp, width)
    png = write_png(across, down, grey) if seen is None else \
        gb_data_colours.indexed_png(across, down, grey.translate(NEAREST[bpp]), seen["entries"])
    target.write_bytes(png)
    for path in gone:
        path.unlink(missing_ok=True)
    view = {"kind": "tiles", "file": picture, "offset": offset, "tiles": count, "width": width, "bpp": bpp,
            "sha256": sha256(png), "data_sha256": sha256(data)}
    if seen is not None:
        view["palette"] = gb_data_colours.written(seen["entries"])
    region["views"] = sorted(kept + [view], key=lambda view: spans(view)[0])
    write_manifest(directory, manifest)
    wide, high = sheet_size(view)
    print(f"wrote {directory / view['file']}: {count} tiles from {view_at(region, view)}, {bpp} "
          f"{'bit' if bpp == 1 else 'bits'} a pixel, {width} to a row ({wide}x{high} pixels)")
    if seen is not None:
        through = (f"palette {seen['palette']:02X}" if "palette" in seen else
                   "the colours " + " ".join(f"{value:04X}" for value in seen["colours"]))
        print(f"  in the colours most seen: drawn by {'sprites' if seen['by'] == 'objects' else 'the background'} "
              f"through {through}, for {seen['frames']:,} {'frame' if seen['frames'] == 1 else 'frames'}"
              + (". Colour 0 is clear, as a sprite leaves it" if seen["by"] == "objects" else "")
              + " (--greys draws the plain greys)")
    return 0


# ---------------------------------------------------------------------------
# What the game's own routine unpacks


def unpacked_picture(made: bytes, sizes: list[int], shape: dict) -> bytes:
    """The PNG of something a routine made, drawn the way its declaration says."""
    if shape["kind"] == "tiles":
        return write_png(*draw_tiles(made, shape["bpp"], min(shape["width"], len(made) // (8 * shape["bpp"]))))
    across, down, colours = gb_unpacked.to_colours(made, sizes, shape)
    shades = SHADES[len(sizes)]
    return write_png(across, down, bytes(shades[colour] for colour in colours))


def unpacked_from_picture(name: str, png: bytes, made: bytes, sizes: list[int], shape: dict) -> bytes:
    """What the routine should have made for its picture to be this one."""
    if shape["kind"] == "tiles":
        count = len(made) // (8 * shape["bpp"])
        view = {"file": name, "tiles": count, "width": min(shape["width"], count), "bpp": shape["bpp"]}
        return tiles_shown(view, png)
    width, height, grey = read_png(name, png)
    wide, high = shape["tiles_wide"] * 8, shape["tiles_high"] * 8
    if (width, height) != (wide, high):
        raise DataError(f"{name} is {width}x{height} pixels, and the picture it was drawn as is {wide}x{high}: a "
                        f"picture keeps its size")
    nearest = NEAREST[len(sizes)]
    return gb_unpacked.from_colours(bytes(nearest[level] for level in grey), made, sizes, shape)


def tree_unpackers(directory: Path, manifest: dict) -> list[dict] | None:
    """The tree's declaration of its unpackers, or None where it has none."""
    path = directory / gb_unpacked.DECLARATION
    if not path.exists():
        if manifest.get("unpacked"):
            raise DataError(f"{path} is gone, and the tree has things unpacked by what it declared")
        return None
    try:
        return gb_unpacked.load_declaration(path)
    except gb_unpacked.UnpackedError as error:
        raise DataError(str(error)) from error


def with_unpacked(directory: Path, manifest: dict, unpackers: list[dict] | None) -> tuple[list[tuple], list[str]]:
    """The replacements the tree's edited unpacked things make, as the pack
    wants them, and a line for each. A file that is as `unpacked` wrote it,
    or that still shows what the routine made, replaces nothing."""
    entries, lines = [], []
    by_name = {unpacker["name"]: (index, unpacker) for index, unpacker in enumerate(unpackers or ())}
    edits: dict[tuple, bytes] = {}      # what each edited thing should have been made as
    followers = []
    for thing in manifest.get("unpacked", ()):
        key = (thing["unpacker"], thing["bank"], thing["address"], thing["made_sha256"])
        if thing["unpacker"] not in by_name:
            raise DataError(f"{thing['file']} was unpacked by '{thing['unpacker']}', which "
                            f"{gb_unpacked.DECLARATION} no longer declares")
        index, unpacker = by_name[thing["unpacker"]]
        sizes = gb_unpacked.made_sizes(unpacker)
        try:
            made = inside(directory, thing["made"]).read_bytes()
            shown = inside(directory, thing["file"]).read_bytes()
        except OSError as error:
            raise DataError(f"cannot read what was unpacked from {thing['bank']:02X}:{thing['address']:04X} "
                            f"({error}): `unpacked --force` writes it again from a harvest") from error
        if sha256(made) != thing["made_sha256"] or len(made) != sum(sizes):
            raise DataError(f"{thing['made']} is not what the game's routine made: it is not to be edited, and "
                            f"`unpacked --force` writes it again from a harvest")
        wanted = made
        if sha256(shown) != thing["sha256"]:
            if "shape" in thing:
                wanted = unpacked_from_picture(thing["file"], shown, made, sizes, thing["shape"])
            elif len(shown) != len(made):
                raise DataError(f"{thing['file']} is {len(shown)} bytes and what the routine makes is {len(made)}: "
                                f"it keeps its size")
            else:
                wanted = shown
        if wanted == made:
            if "twin" in thing:
                followers.append((thing, index, made))
            continue
        edits[key] = wanted
        entries.append((index, thing["bank"], thing["address"], bytes.fromhex(thing["made_sha256"]), wanted))
        differ = sum(1 for old, new in zip(made, wanted) if old != new)
        lines.append(f"{thing['file']} (unpacked by {thing['unpacker']} from {thing['bank']:02X}:"
                     f"{thing['address']:04X}, {differ} of {len(made)} bytes changed)")
    # The same stream unpacked another way, where that is the first with each byte's bits moved
    # about: left as it was written, it follows the edit of its twin.
    for thing, index, made in followers:
        edit = edits.get((thing["unpacker"], thing["bank"], thing["address"], thing["twin"]["made_sha256"]))
        if edit is None:
            continue
        wanted = gb_unpacked.move_bits(edit, thing["twin"]["bits"])
        entries.append((index, thing["bank"], thing["address"], bytes.fromhex(thing["made_sha256"]), wanted))
        lines.append(f"{thing['file']} (the same stream unpacked another way: it follows its twin)")
    return entries, lines


def write_pack(output: Path, manifest: dict, unpackers: list[dict] | None, entries: list[tuple],
               lines: list[str]) -> None:
    """The pack beside the image: what the game is to watch, and to put in place of what."""
    if unpackers is None:
        return
    pack = output.with_suffix(".gbup")
    try:
        pack.write_bytes(gb_unpacked.pack_bytes(manifest["rom"]["size"], manifest["rom"]["sha256"], unpackers,
                                                entries))
    except (OSError, gb_unpacked.UnpackedError) as error:
        raise DataError(f"cannot write {pack}: {error}") from error
    print(f"wrote {pack}: {len(unpackers)} {'routine' if len(unpackers) == 1 else 'routines'} to watch, and "
          f"{len(entries)} {'replacement' if len(entries) == 1 else 'replacements'} for what "
          f"{'it makes' if len(unpackers) == 1 else 'they make'} (the game takes it with --unpacked)")
    for line in lines[:12]:
        print(f"  {line}")
    if len(lines) > 12:
        print(f"  and {len(lines) - 12} more")


def command_watch_pack(args: argparse.Namespace) -> int:
    """A pack of the tree's declaration with no replacement in it, whatever is edited: the pack
    `build` writes for a tree with nothing unpacked edited, byte for byte. What was unpacked is
    not read, so an edited picture that `build` would refuse is not in the way."""
    directory = Path(args.directory)
    manifest, _ = load_tree(directory)
    unpackers = tree_unpackers(directory, manifest)
    if unpackers is None:
        raise DataError(f"{directory / gb_unpacked.DECLARATION} is not there: it says which routine unpacks, and "
                        f"has to be written first (DATA_FILES.md)")
    pack = Path(args.output)
    try:
        pack.parent.mkdir(parents=True, exist_ok=True)
        pack.write_bytes(gb_unpacked.pack_bytes(manifest["rom"]["size"], manifest["rom"]["sha256"], unpackers, []))
    except (OSError, gb_unpacked.UnpackedError) as error:
        raise DataError(f"cannot write {pack}: {error}") from error
    one = len(unpackers) == 1
    print(f"wrote {pack}: a pack that only watches. {len(unpackers)} {'routine' if one else 'routines'} to watch, "
          f"and it replaces nothing, whatever is edited in the tree (the game takes it with --unpacked, and with "
          f"--unpacked-harvest writes out what {'the routine makes' if one else 'the routines make'}; `build` "
          f"writes the pack that has the edits)")
    return 0


def put_unpacked(directory: Path, thing: dict, made: bytes, unpacker: dict, rom: bytes) -> bool:
    """Write a thing a routine made, and the file to edit it by, and say in `thing` what was
    written. False where it could not be drawn the way its unpacker's declaration says."""
    stem = thing["made"][:-len(".made")]
    drawn = True
    try:
        shape = gb_unpacked.resolve_shape(unpacker, rom, thing["bank"], thing["address"])
    except gb_unpacked.UnpackedError:
        shape, drawn = None, False          # not a picture of the shape declared: its bytes are still its bytes
    if shape is None:
        thing.pop("shape", None)
        thing["file"], content = stem + ".bin", made
    else:
        thing["shape"] = shape
        thing["file"], content = stem + ".png", unpacked_picture(made, gb_unpacked.made_sizes(unpacker), shape)
    thing["sha256"] = sha256(content)
    for name, data in ((thing["made"], made), (thing["file"], content)):
        target = inside(directory, name)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return drawn


def command_unpacked(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    unpackers = tree_unpackers(directory, manifest)
    if unpackers is None:
        raise DataError(f"{directory / gb_unpacked.DECLARATION} is not there: it says which routine unpacks, and "
                        f"has to be written first (DATA_FILES.md)")
    by_name = {unpacker["name"]: unpacker for unpacker in unpackers}
    rom = b"".join(content for _, content in loaded)
    # A folder stands for the harvests in it: a game played from hundreds of save states writes
    # more of them than a command line holds.
    harvests = [path for given in args.harvests
                for path in (sorted(Path(given).glob("*.json")) if Path(given).is_dir() else [Path(given)])]
    if not harvests:
        raise DataError("there is no harvest in what was given")
    try:
        harvested = [thing for path in harvests
                     for thing in gb_unpacked.load_harvest(path, manifest["rom"], unpackers)]
    except gb_unpacked.UnpackedError as error:
        raise DataError(str(error)) from error
    things = list(manifest.get("unpacked", ()))
    known = {(thing["unpacker"], thing["bank"], thing["address"], thing["made_sha256"]): thing for thing in things}

    def new_thing(key: tuple) -> dict:
        others = sum(1 for other in things if (other["unpacker"], other["bank"], other["address"]) == key[:3])
        stem = f"unpacked/{key[0]}/{key[1]:02X}_{key[2]:04X}" + (f"_{others + 1}" if others else "")
        thing = {"unpacker": key[0], "bank": key[1], "address": key[2], "made": stem + ".made", "made_sha256": key[3]}
        things.append(thing)
        known[key] = thing
        return thing

    new = again = redrawn = as_bytes = 0
    seen = set()
    for harvest in sorted(harvested, key=lambda thing: (thing["unpacker"], thing["bank"], thing["address"],
                                                        sha256(thing["bytes"]))):
        made = harvest["bytes"]
        key = (harvest["unpacker"], harvest["bank"], harvest["address"], sha256(made))
        if key in seen:
            continue
        seen.add(key)
        thing = known.get(key)
        if thing is not None:
            thing.pop("worked_out", None)   # the game has been seen to make it now
            if not args.force:
                again += 1
                continue
            redrawn += 1
        else:
            thing = new_thing(key)
            new += 1
        as_bytes += not put_unpacked(directory, thing, made, by_name[key[0]], rom)

    def streams_of() -> dict[tuple, list[dict]]:
        things.sort(key=lambda thing: (thing["unpacker"], thing["bank"], thing["address"], thing["made"]))
        by_stream: dict[tuple, list[dict]] = {}
        for thing in things:
            by_stream.setdefault((thing["unpacker"], thing["bank"], thing["address"]), []).append(thing)
        return by_stream

    def made_of(thing: dict) -> bytes:
        return inside(directory, thing["made"]).read_bytes()

    # Two things made of one stream are often one picture and its mirrored form. A twin that
    # is in the manifest stays while it holds: one that was worked out may be of bytes that
    # do not say by themselves which bit went where.
    for group in streams_of().values():
        by_hash = {thing["made_sha256"]: thing for thing in group}
        for thing in group:
            twin = thing.get("twin")
            partner = by_hash.get(twin["made_sha256"]) if twin else None
            if twin and (partner is None or
                         gb_unpacked.move_bits(made_of(partner), twin["bits"]) != made_of(thing)):
                del thing["twin"]
        first = group[0]
        for other in group[1:]:
            if "twin" in other:
                continue
            there = gb_unpacked.bit_moves(made_of(first), made_of(other))
            back = gb_unpacked.bit_moves(made_of(other), made_of(first))
            if there and back:
                other["twin"] = {"made_sha256": first["made_sha256"], "bits": there}
                first.setdefault("twin", {"made_sha256": other["made_sha256"], "bits": back})
    # And where every stream that was unpacked two ways shows the same move of bits, one that
    # undoes itself, the other way of a stream seen only one way can be worked out. It is a
    # guess that costs nothing: its replacement is used only if the game makes exactly that.
    worked = 0
    for name, unpacker in by_name.items():
        groups = [group for key, group in streams_of().items() if key[0] == name]
        moves = {tuple(thing["twin"]["bits"]) for group in groups for thing in group if "twin" in thing}
        paired = sum(1 for group in groups if any("twin" in thing for thing in group))
        if len(moves) != 1 or paired < 2:
            continue
        bits = list(next(iter(moves)))
        if [bits[bit] for bit in bits] != list(range(8)):
            continue
        for group in groups:
            if len(group) != 1:
                continue
            first = group[0]
            other_made = gb_unpacked.move_bits(made_of(first), bits)
            if other_made == made_of(first):
                continue
            other = new_thing((first["unpacker"], first["bank"], first["address"], sha256(other_made)))
            other["worked_out"] = True
            other["twin"] = {"made_sha256": first["made_sha256"], "bits": bits}
            first["twin"] = {"made_sha256": other["made_sha256"], "bits": bits}
            as_bytes += not put_unpacked(directory, other, other_made, unpacker, rom)
            worked += 1
    twins = sum(1 for group in streams_of().values()
                if any("twin" in thing for thing in group) and not any(thing.get("worked_out") for thing in group))
    manifest["unpacked"] = things
    write_manifest(directory, manifest)
    streams = len({key[:3] for key in seen})
    print(f"{len(harvests)} {'harvest' if len(harvests) == 1 else 'harvests'}: {len(seen)} different "
          f"{'thing' if len(seen) == 1 else 'things'} made, of {streams} {'stream' if streams == 1 else 'streams'}")
    print(f"  {new} new in {directory / 'unpacked'}, {again} there already and left as they are"
          + (f", {redrawn} written again" if redrawn else ""))
    if as_bytes:
        print(f"  {as_bytes} could not be drawn the way {gb_unpacked.DECLARATION} says, and are files of bytes")
    if twins:
        print(f"  {twins} {'stream is' if twins == 1 else 'streams are'} unpacked two ways that are each other with "
              f"the bits of every byte moved about, as a mirrored picture is: edit either and the other follows")
    if worked:
        print(f"  {worked} more {'was' if worked == 1 else 'were'} worked out: a stream seen unpacked one way, "
              f"written the other way as well, the way the others show. No harvest has "
              f"{'it' if worked == 1 else 'them'}; if the game makes something else, nothing is replaced")
    return 0


def view_spans(loaded: list[tuple[dict, bytes]]) -> list[tuple[int, int, str]]:
    """The bytes of the ROM the tree's pictures and files of text are of, as (first, one past the last, the file)."""
    return [(region["offset"] + start, region["offset"] + stop, view["file"])
            for region, _ in loaded for view in region.get("views", ()) for start, stop in spans(view)]


def move_things(directory: Path, manifest: dict, loaded: list[tuple[dict, bytes]], image: bytes, grown: list,
                everything: bool) -> tuple[bytes, list[str], dict]:
    """The image with what has to move moved into the free space the tree
    declares, what to say of it, and what was done as `gb_data_pointers.move`
    gives it (`named` there is where each name of the game's source leads
    now): each string that grew, with what it is part of, and with
    `everything` all else that can move (gb_data_pointers.py says what can,
    and how)."""
    try:
        things = gb_data_pointers.checked_things(manifest)
        free = gb_data_pointers.load_free(directory / gb_data_pointers.FREE, [region for region, _ in loaded],
                                          len(image), things, view_spans(loaded))
        strings = [(region["offset"] + offset, size, new,
                    f"{name}: the string at +{offset:04X} is {len(new)} bytes now and there is room for {size}: a "
                    f"string cannot grow where it is, and ")
                   for region, name, offset, size, new in grown]
        image, done = gb_data_pointers.move(image, things, free, strings, everything)
    except gb_data_pointers.PointersError as error:
        raise DataError(str(error)) from error
    return image, gb_data_pointers.moved_said(done), done


def moved_header(image: bytes, named: list[tuple[str, int, int]]) -> str:
    """The header a game's source is compiled with after things it names have
    moved: each name with where it leads in this image."""
    lines = ["/* Where the names of the game's source lead in the image with SHA-1",
             f" * {hashlib.sha1(image).hexdigest()}: written by tools/gb_data.py build, and written again by every",
             " * build that is given --moved-header. It is for a game that runs that image and no",
             " * other (DATA_FILES.md, \"Data the source names\"). "
             + ("Every name not here is where the ROM" if named else "No name is here: nothing the source"),
             " * has it. */" if named else " * names has moved. */",
             f"#define LIFTED_MOVED_IMAGE \"{hashlib.sha1(image).hexdigest()}\""]
    places: dict[str, int] = {}
    for name, _, leads in named:
        if places.setdefault(name, leads) != leads:
            raise DataError(f"the name {name} would lead to two places, {places[name]:04X} and {leads:04X}: the "
                            f"source gives one name to two different things")
    lines += [f"#define {name} 0x{leads:04X}" for name, leads in sorted(places.items())]
    return "\n".join(lines) + "\n"


def command_build(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    changes = edited(loaded)
    refuse_pinned(changes)
    grown: list = []
    contents, redrawn, disagree = with_views(directory, loaded, grown)
    if disagree:
        raise DataError(f"{disagree[0]}: keep one of the two (`tiles --force` draws a picture again from the "
                        f"file, and `text --force` writes the text again)")
    unpackers = tree_unpackers(directory, manifest)
    replacements, replaced = with_unpacked(directory, manifest, unpackers)
    image = b"".join(contents)
    moved: list[str] = []
    named: list[tuple[str, int, int]] = []
    done: dict | None = None
    if grown or args.move_all:
        image, moved, done = move_things(directory, manifest, loaded, image, grown, args.move_all)
        named = sorted(set(done["named"]))
    if named and not args.moved_header:
        raise DataError(f"{len({name for name, _, _ in named})} of the things that move are led to by operands the "
                        f"game's source names ({named[0][0]}, at {named[0][1]:04X}): give --moved-header FILE, the "
                        f"source folder's src/lifted_moved.h, so that the source learns where they are, and build "
                        f"the game again from it")
    if args.moved_header:
        # Always: a header left from another image would send the source to the wrong places.
        Path(args.moved_header).write_text(moved_header(image, named), encoding="utf-8", newline="\n")
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    # What moved where, for whatever has to tell an address of the image from the same address of
    # the ROM: beside the image, and gone when nothing has moved.
    moved_list = output.with_name(output.name + ".moved.json")
    if done and done["moved"]:
        moved_list.write_text(json.dumps({
            "schema": "gbrecomp.data-moved", "version": 1, "image": hashlib.sha1(image).hexdigest(),
            "moved": [{"offset": old, "size": size, "to": new, "new_size": new_size}
                      for old, size, new, new_size in done["moved"]],
            "turned": [{"offset": low, "said": said, "says": says} for low, said, says in done["turned"]],
            "named": [{"name": name, "said": said, "says": says} for name, said, says in named]},
            indent=1) + "\n", encoding="utf-8", newline="\n")
    else:
        moved_list.unlink(missing_ok=True)
    digest = hashlib.sha1(image).hexdigest()
    if digest == manifest["rom"]["sha1"]:
        print(f"wrote {output}: the original ROM, byte for byte (SHA-1 {digest})")
        for line in moved:
            print(line)
        write_pack(output, manifest, unpackers, replacements, replaced)
        return 0
    lines = [describe(region) for region, _ in changes] + redrawn
    files = len({line.split(" (", 1)[0] for line in lines})     # a file of text has a line for each string edited
    print(f"wrote {output}: {files} of {len(loaded) + pictures(loaded)} files edited (SHA-1 {digest}, the "
          f"original's is {manifest['rom']['sha1']})")
    for line in lines[:12]:
        print(f"  {line}")
    if len(lines) > 12:
        print(f"  and {len(lines) - 12} more")
    for line in moved:
        print(line)
    write_pack(output, manifest, unpackers, replacements, replaced)
    if args.expect_original:
        print("gb_data: the image was expected to be the original ROM", file=sys.stderr)
        return 1
    return 0


def command_status(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    changes = edited(loaded)
    grown: list = []
    contents, redrawn, disagree = with_views(directory, loaded, grown)
    _, replaced = with_unpacked(directory, manifest, tree_unpackers(directory, manifest))
    data = sum(1 for region, _ in loaded if region["kind"] == "data")
    things = len(manifest.get("unpacked", ()))
    print(f"{manifest['rom'].get('title') or 'ROM'}: {len(loaded)} files, {data} of them data{views_said(loaded)}"
          + (f", and {things} {'thing' if things == 1 else 'things'} the game unpacks" if things else ""))
    if not changes and not redrawn and not disagree:
        print("nothing edited: `build` makes the original ROM" if not replaced else
              "no file of the ROM edited: `build` makes the original ROM, and a pack with what was unpacked and "
              "edited")
        for line in replaced:
            print(f"  edited: {line}")
        return 0
    for region, _ in changes:
        note = "" if region["kind"] == "data" else "   <- not data: `build` will refuse this"
        print(f"  edited: {describe(region)}{note}")
    for line in redrawn:
        print(f"  edited: {line}")
    for line in disagree:
        print(f"  edited: {line}   <- `build` will refuse this")
    for line in replaced:
        print(f"  edited: {line}")
    if grown and not disagree:
        # What `build` would do with the strings that grew, without doing it.
        try:
            for line in move_things(directory, manifest, loaded, b"".join(contents), grown, False)[1]:  # what build would say
                print(f"  `build` will have {line}" if line.startswith("moved") else f"  {line}")
        except DataError as error:
            print(f"  `build` will refuse this: {error}")
    return 0


def load_uses(paths: list[Path], size: int, checksum: int,
              landed: gb_data_tiles.Landed | None = None) -> dict[int, tuple]:
    """What the records say of each ROM byte that was read as data: the uses
    of the byte as it is, the uses of what was worked out from it, the first
    instruction that read it, and whether only one did. Several records are
    several runs of the game: their uses add up. Where in the tile area
    bytes landed goes into `landed`, if one is given."""
    seen: dict[int, tuple] = {}
    for path in paths:
        try:
            record = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise DataError(f"cannot read the record {path}: {error}") from error
        if not isinstance(record, dict) or record.get("schema") != USE_SCHEMA or record.get("version") != 1:
            raise DataError(f"{path} is not a version 1 {USE_SCHEMA} record")
        if record["rom"]["size"] != size or record["rom"]["global_checksum"] != checksum:
            raise DataError(f"{path} was recorded from another ROM")
        if landed is not None:
            landed.add(record, path)
        for entry in record["ranges"]:
            unknown = sorted((set(entry["as_it_is"]) | set(entry["worked_out"])) - set(USES))
            if unknown:
                raise DataError(f"{path}: no such use as '{unknown[0]}'")
            reader = tuple(entry["read_at"])
            for offset in range(entry["offset"], entry["offset"] + entry["size"]):
                if not 0 <= offset < size:
                    raise DataError(f"{path}: a range leaves the ROM at offset {offset:#x}")
                before = seen.get(offset)
                if before is None:
                    seen[offset] = (frozenset(entry["as_it_is"]), frozenset(entry["worked_out"]), reader,
                                    entry["one_reader"])
                else:
                    seen[offset] = (before[0] | frozenset(entry["as_it_is"]), before[1] | frozenset(entry["worked_out"]),
                                    before[2], before[3] and entry["one_reader"] and before[2] == reader)
    return seen


def records_given(given: list[Path]) -> list[Path]:
    """A folder stands for the records in it, as one does for harvests: a game played from
    hundreds of save states writes more of them than a command line holds."""
    records = [path for one in given
               for path in (sorted(Path(one).glob("*.json")) if Path(one).is_dir() else [Path(one)])]
    if not records:
        raise DataError("there is no record in what was given")
    return records


def command_uses(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    args.records = records_given(args.records)
    image = b"".join(content for _, content in loaded)
    checksum = (image[0x14E] << 8) | image[0x14F] if len(image) > 0x14F else 0
    landed = gb_data_tiles.Landed()
    try:
        seen = load_uses(args.records, len(image), checksum, landed)
    except gb_data_tiles.TilesError as error:
        raise DataError(str(error)) from error
    totals = {use: [0, 0, set()] for use in USES}      # bytes as they are, bytes worked out, regions
    readers: dict[tuple, dict[str, int]] = {}          # the instruction that read a byte -> what its bytes were for
    nothing = 0
    in_code = 0
    for number, (region, _) in enumerate(loaded):
        runs: list[dict] = []
        for offset in range(region["offset"], region["offset"] + region["size"]):
            entry = seen.get(offset)
            if entry is None:
                continue
            if region["kind"] != "data":
                in_code += 1
                continue
            as_it_is, worked_out, reader, one_reader = entry
            for use in as_it_is:
                totals[use][0] += 1
                totals[use][2].add(number)
            for use in worked_out - as_it_is:
                totals[use][1] += 1
                totals[use][2].add(number)
            nothing += not (as_it_is or worked_out)
            what = "+".join(use for use in USES if use in as_it_is) or \
                ("(" + "+".join(use for use in USES if use in worked_out) + ")" if worked_out else "nothing")
            by_reader = readers.setdefault(tuple(reader), {})
            by_reader[what] = by_reader.get(what, 0) + 1
            last = runs[-1] if runs else None
            if last and last["offset"] + last["size"] == offset - region["offset"] and \
                    last["_key"] == (as_it_is, worked_out, reader, one_reader):
                last["size"] += 1
            else:
                runs.append({"offset": offset - region["offset"], "size": 1,
                             "as_it_is": [use for use in USES if use in as_it_is],
                             "worked_out": [use for use in USES if use in worked_out],
                             "read_at": list(reader), "one_reader": one_reader,
                             "_key": (as_it_is, worked_out, reader, one_reader)})
        for run in runs:
            del run["_key"]
        if region["kind"] == "data":
            if runs:
                region["seen"] = runs
            else:
                region.pop("seen", None)
    # One instruction reads one kind of thing: the copying loop reads pictures, the routine that
    # prints reads text, the one that unpacks reads packed pictures whatever becomes of each byte.
    ranked = sorted(readers.items(), key=lambda item: (-sum(item[1].values()), item[0]))
    manifest["readers"] = [{"bank": bank, "address": address, "bytes": sum(uses.values()),
                            "uses": dict(sorted(uses.items(), key=lambda item: -item[1]))}
                           for (bank, address), uses in ranked]
    # And what the screen showed of the tiles: the palettes they were drawn through (`shown`).
    try:
        shown = gb_data_colours.load_shown(args.records, len(image))
    except gb_data_colours.ColourError as error:
        raise DataError(str(error)) from error
    drawn = {"runs": 0, "background": 0, "objects": 0}
    for region, _ in loaded:
        runs = gb_data_colours.shown_runs(shown, region["offset"], region["size"]) if region["kind"] == "data" else []
        if runs:
            region["shown"] = runs
        else:
            region.pop("shown", None)
        drawn["runs"] += len(runs)
        for run in runs:
            for layer in {entry["by"] for entry in run["as"]}:
                drawn[layer] += run["size"]
    # And of the tile maps: the rows their bytes lay in (`mapped`).
    try:
        mapped = gb_data_maps.load_mapped(args.records, len(image))
    except gb_data_maps.MapError as error:
        raise DataError(str(error)) from error
    laid = [0, 0]
    for region, _ in loaded:
        rows = gb_data_maps.mapped_rows(mapped, region["offset"], region["size"]) if region["kind"] == "data" else []
        if rows:
            region["mapped"] = rows
        else:
            region.pop("mapped", None)
        laid[0] += len(rows)
        laid[1] += sum(row["size"] for row in rows)
    tiles_said = landed.into([region for region, _ in loaded])
    write_manifest(directory, manifest)
    data_bytes = sum(region["size"] for region, _ in loaded if region["kind"] == "data")
    read = sum(1 for offset in seen if True) - in_code
    print(f"{len(args.records)} record{'s' if len(args.records) != 1 else ''}: the game read {read:,} of the "
          f"{data_bytes:,} bytes of data as data")
    print(f"  {'':10s} {'as it is':>10s} {'worked out':>11s} {'regions':>8s}")
    for use in USES:
        as_it_is, worked_out, regions = totals[use]
        if as_it_is or worked_out:
            print(f"  {use:10s} {as_it_is:10,d} {worked_out:11,d} {len(regions):8,d}")
    print(f"  {nothing:,} bytes were read and went to none of those")
    for line in tiles_said:
        print(f"  {line}")
    if in_code:
        print(f"  {in_code:,} bytes the game read as data are in files of code or the header: what the analysis "
              f"decoded there may be data")
    if ranked:
        print("  by the instruction that read them (a byte in brackets was worked out from, not copied):")
        for (bank, address), uses in ranked[:args.readers]:
            parts = ", ".join(f"{what} {count:,}" for what, count in sorted(uses.items(), key=lambda item: -item[1])[:4])
            print(f"    {sum(uses.values()):9,d} bytes read at {bank:02X}:{address:04X}: {parts}")
        if len(ranked) > args.readers:
            print(f"    and {len(ranked) - args.readers} more instructions (`readers` in the manifest has them all)")
    if drawn["runs"]:
        print(f"  shown on the screen as the ROM has them: {drawn['runs']:,} "
              f"{'run' if drawn['runs'] == 1 else 'runs'} of tile bytes, {drawn['background']:,} bytes drawn by the "
              f"background and {drawn['objects']:,} by sprites. `tiles` draws them in the palette most seen "
              f"(`shown`, on each region)")
    if laid[0]:
        print(f"  shown on the screen in a tile map as the ROM has them: {laid[1]:,} bytes, in {laid[0]:,} "
              f"{'row' if laid[0] == 1 else 'rows'}. `maps` writes those that are not text as tile numbers "
              f"(`mapped`, on each region)")
    print(f"wrote what was seen into {directory / 'manifest.json'} (`seen`, on each region)")
    return 0


def readers_asked(args: argparse.Namespace, manifest: dict) -> list[tuple[int, int]]:
    """The instructions that read text: the ones named, or the ones found."""
    if "readers" not in manifest:
        raise DataError("nothing says yet what the game reads: run it with --data-use, and `uses` on what it "
                        "records")
    readers = []
    for word in args.reader:
        try:
            bank, address = (int(part, 16) for part in word.split(":"))
        except ValueError:
            raise DataError(f"--reader takes BANK:ADDRESS in hexadecimal, not '{word}'") from None
        readers.append((bank, address))
    readers = readers or gb_data_text.text_readers(manifest)
    if not readers:
        raise DataError("no instruction was seen to read text, which is one that reads bytes one after another "
                        "and most of them reach a tile map as they are: name one with --reader (`uses` lists "
                        "the instructions that read), or record more of the game")
    return readers


def command_glyphs(args: argparse.Namespace) -> int:
    import gb_discover      # the save state reader: only this command needs it

    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    readers = set(readers_asked(args, manifest))
    used: set[int] = set()
    for region, content in loaded:
        for offset, size in gb_data_text.strings_of(region, readers):
            used.update(content[offset:offset + size])
    try:
        state = gb_discover.Savestate.load(args.state)
    except (OSError, ValueError) as error:
        raise DataError(f"cannot read the save state {args.state}: {error}") from error
    sheet = Path(args.output) if args.output else directory / "glyphs.png"
    gb_discover.write_png(sheet, *gb_data_text.glyph_sheet(state, used))
    print(f"wrote {sheet}: the tile each byte draws in {args.state}, the byte's first digit down the side and its "
          f"second along the top. The {len(used)} bytes the strings use are drawn dark")
    charmap = directory / "charmap.json"
    if charmap.exists():
        print(f"{charmap} is there already and is left as it is")
    else:
        document = {"schema": gb_data_text.CHARMAP_SCHEMA, "version": 1,
                    "letters": {f"{value:02X}": "" for value in sorted(used)}}
        charmap.write_text(json.dumps(document, indent=1, ensure_ascii=False) + "\n", encoding="utf-8",
                           newline="\n")
        print(f"wrote {charmap} with those bytes and no letters: write beside each what the sheet shows it draws. "
              f"A byte left empty is written <XX> in the text")
    return 0


def as_written(name: str, there: bytes, view: dict, letters: dict[int, str]) -> bool:
    """Whether a file of text that was saved again still says what `text` wrote it from."""
    try:
        said = gb_data_text.said_bytes(name, there.decode("utf-8-sig"),
                                       [(offset, size) for offset, size in view["strings"]], letters)
    except (UnicodeDecodeError, gb_data_text.TextError):
        return False
    return said is not None and sha256(said) == view["data_sha256"]


def packed_stretches(manifest: dict, planned: list) -> dict[int, list[tuple[int, int]]]:
    """Where a bank holds packed data, as (address, one past the last) by bank: from where each stream a
    routine of the game unpacks starts, up to the next stream, or to the next view or string after the last.

    The manifest knows where a stream starts and not how long it is, and the analysis may have cut it into
    several regions, with stretches it took for code between them: so this goes by the bank's addresses."""
    starts: dict[int, set[int]] = {}
    for thing in manifest.get("unpacked", ()):
        starts.setdefault(thing["bank"], set()).add(thing["address"])
    other: dict[int, set[int]] = {}
    for region, _, kept, _, found, _, _ in planned:
        for first, _ in [span for view in kept for span in spans(view)] + found:
            other.setdefault(region["bank"], set()).add(region["address"] + first)
    stretches = {}
    for bank, addresses in starts.items():
        stops = sorted(addresses | other.get(bank, set()))
        stretches[bank] = [(address, next((stop for stop in stops if stop > address), 0x8000))
                           for address in sorted(addresses)]
    return stretches


def not_text(region: dict, views: list[dict], strings: list[tuple[int, int]],
             packed: list[tuple[int, int]]) -> bytearray:
    """A byte for each byte of a region, not zero where `text --read` is not to look for a string: where a
    string or another view already is, where the game was seen to use the bytes for something that is not
    text, and in packed data (`packed_stretches`).

    Bytes that reached a tile map as they are, or were half of an address, may be text all the same: a name is
    copied into memory before it is printed, by an instruction that prints nothing. Anything else a record
    says of a byte (it was drawn as a tile, played, sent as a bank, or something was worked out from it) is
    not what becomes of a letter."""
    taken = bytearray(region["size"])
    for seen in region.get("seen", ()):
        if set(seen["as_it_is"]) - {"tile_map", "pointer"} or seen["worked_out"]:
            taken[seen["offset"]:seen["offset"] + seen["size"]] = b"\x01" * seen["size"]
    for first, stop in [span for view in views for span in spans(view)] + \
            [(offset, offset + size) for offset, size in strings]:
        taken[first:stop] = b"\x01" * (stop - first)
    for first, stop in packed:
        first = max(first - region["address"], 0)
        stop = min(stop - region["address"], region["size"])
        if first < stop:
            taken[first:stop] = b"\x01" * (stop - first)
    return taken


def command_text(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    readers = set(readers_asked(args, manifest))
    try:
        letters = gb_data_text.load_charmap(directory / CHARMAP)
    except gb_data_text.TextError as error:
        raise DataError(f"{error} (`glyphs` draws the letters and writes a charmap to fill in)") from error
    regions = count = total = drawn_over = edited_kept = 0
    unlettered: dict[int, int] = {}
    # What to write is worked out for every region before anything is written: a refusal half way
    # would leave files of text the manifest does not describe.
    planned = []
    for region, content in loaded:
        if region["kind"] != "data":
            continue
        kept = [view for view in region.get("views", ()) if view["kind"] != "text"]
        before = next((view for view in region.get("views", ()) if view["kind"] == "text"), None)
        taken = [span for view in kept for span in spans(view)]
        found = []
        for offset, size in gb_data_text.strings_of(region, readers):
            if any(start < offset + size and offset < stop for start, stop in taken):
                drawn_over += 1         # a picture has those bytes, and two views of the same bytes could disagree
                continue
            found.append((offset, size))
        name = (region["file"][:-len(".bin")] if region["file"].endswith(".bin") else region["file"]) + ".text.txt"
        target = inside(directory, name)
        if target.exists() and not args.force:
            if before is None:
                raise DataError(f"{name} is already there and the manifest does not know it: --force writes over "
                                f"it")
            there = target.read_bytes()
            if sha256(there) != before["sha256"] and not as_written(name, there, before, letters):
                if args.keep_edited:
                    edited_kept += 1    # the file and its view stay as they are, as `maps` leaves an edited one
                    continue
                raise DataError(f"{name} has been edited: --force writes it again from {region['file']}, and the "
                                f"edit is lost")
        planned.append((region, content, kept, before, found, name, target))
    read_count = read_total = 0
    reads: dict[str, list[tuple[int, int]]] = {}
    if args.read:
        # What the strings seen printed are made of, and the data nobody saw used for anything else read for
        # more of it.
        body, enders, words = gb_data_text.what_strings_are(
            [content[offset:offset + size] for _, content, _, _, found, _, _ in planned for offset, size in found],
            letters)
        packed = packed_stretches(manifest, planned)
        for index, (region, content, kept, before, found, name, target) in enumerate(planned):
            if region.get("guess") == "padding" or not enders:
                continue
            more = gb_data_text.strings_read(content, not_text(region, kept, found, packed.get(region["bank"], ())),
                                             found, body, enders, words, letters)
            if more:
                reads[region["file"]] = more
                read_count += len(more)
                read_total += sum(size for _, size in more)
                planned[index] = (region, content, kept, before, sorted(found + more), name, target)
    for region, content, kept, before, found, name, target in planned:
        if found:
            written = gb_data_text.strings_text(region["file"], content, found, letters).encode("utf-8")
            target.write_bytes(written)
            kept.append({"kind": "text", "file": name, "charmap": CHARMAP,
                         "strings": [[offset, size] for offset, size in found], "sha256": sha256(written),
                         "data_sha256": sha256(b"".join(content[offset:offset + size] for offset, size in found))})
            if region["file"] in reads:
                kept[-1]["read"] = [[offset, size] for offset, size in reads[region["file"]]]
            regions += 1
            count += len(found)
            for offset, size in found:
                total += size
                for byte in content[offset:offset + size]:
                    if byte not in letters:
                        unlettered[byte] = unlettered.get(byte, 0) + 1
        elif before is not None and target.exists():
            target.unlink()
        if kept:
            region["views"] = sorted(kept, key=lambda view: spans(view)[0])
        else:
            region.pop("views", None)
    write_manifest(directory, manifest)
    read_at = ", ".join(f"{bank:02X}:{address:04X}" for bank, address in sorted(readers))
    print(f"{count:,} {'string' if count == 1 else 'strings'}, {total:,} bytes, in {regions} "
          f"{'region' if regions == 1 else 'regions'}, read at "
          f"{read_at}: each region's are in a .text.txt beside its file")
    if args.read:
        print(f"  {read_count:,} of them, {read_total:,} bytes, nobody saw printed: they were found by reading the "
              f"data for what the others are made of, and are among them in the files")
    if drawn_over:
        print(f"  {drawn_over} more {'lies' if drawn_over == 1 else 'lie'} in bytes a picture has, and "
              f"{'is' if drawn_over == 1 else 'are'} left to it")
    if edited_kept:
        print(f"  {edited_kept} of the files of text {'has' if edited_kept == 1 else 'have'} been edited and "
              f"{'is' if edited_kept == 1 else 'are'} left as {'it is' if edited_kept == 1 else 'they are'}, with "
              f"the strings {'it' if edited_kept == 1 else 'they'} had (--force writes "
              f"{'it' if edited_kept == 1 else 'them'} again from the region's file)")
    if unlettered:
        most = ", ".join(f"<{byte:02X}> {times:,}" for byte, times in
                         sorted(unlettered.items(), key=lambda item: (-item[1], item[0]))[:12])
        print(f"  {sum(unlettered.values()):,} of the bytes have no letter in {CHARMAP} and are written <XX>: {most}")
    return 0


def command_maps(args: argparse.Namespace) -> int:
    import bisect               # to find the region a ROM tile lies in: only this command needs it

    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    if "readers" not in manifest:
        raise DataError("nothing says yet what the game shows: run it with --data-use, and `uses` on what it "
                        "records")
    # The instructions that read text, whose bytes are no tile numbers: the ones named, or the ones found.
    if args.reader == ["none"]:
        readers: set[tuple[int, int]] = set()
    elif args.reader:
        readers = set(readers_asked(args, manifest))
    else:
        readers = set(gb_data_text.text_readers(manifest))
    # The ROM as the tree makes it now, for the pictures: with every picture, text and map that was edited.
    contents, _, _ = with_views(directory, loaded)
    image = b"".join(contents)
    starts = [region["offset"] for region, _ in loaded]

    def colours_of(offset: int, size: int) -> list[tuple[int, int, int]]:
        """The colours a ROM tile is drawn in: the palette it was seen through, or the plain greys."""
        region = loaded[bisect.bisect_right(starts, offset) - 1][0]
        seen = gb_data_colours.most_seen(region.get("shown", ()), offset - region["offset"], size, 2)
        return [entry[:3] for entry in seen["entries"]] if seen else [(shade,) * 3 for shade in SHADES[2]]

    # What to write is worked out for every region before anything is written, as `text` does.
    planned = []
    files = count = total = edited_kept = left_out = unseen = 0
    for region, content in loaded:
        if region["kind"] != "data":
            continue
        kept = [view for view in region.get("views", ()) if view["kind"] != "map"]
        before = next((view for view in region.get("views", ()) if view["kind"] == "map"), None)
        grids, taken = gb_data_maps.grids_of(region, readers, [span for view in kept for span in spans(view)])
        left_out += taken
        name = (region["file"][:-len(".bin")] if region["file"].endswith(".bin") else region["file"]) + ".map.txt"
        target = inside(directory, name)
        keep = False
        if target.exists() and not args.force:
            if before is None:
                raise DataError(f"{name} is already there and the manifest does not know it: --force writes over "
                                f"it")
            # A file somebody edited is left as it is, with its view: only its picture is drawn again.
            there = target.read_bytes()
            if sha256(there) != before["sha256"]:
                said = gb_data_maps.said_bytes(name, there.decode("utf-8-sig"),
                                               [tuple(grid) for grid in before["grids"]])
                keep = sha256(said) != before["data_sha256"]
        if keep:
            grids = [list(grid) for grid in before["grids"]]
            edited_kept += 1
        covered = {at for offset, width, rows in grids for at in range(offset, offset + width * rows)}
        text = {at for seen in region.get("seen", ()) if (seen["read_at"][0], seen["read_at"][1]) in readers
                for at in range(seen["offset"], seen["offset"] + seen["size"])}
        unseen += sum(1 for seen in region.get("seen", ()) if "tile_map" in seen["as_it_is"]
                      for at in range(seen["offset"], seen["offset"] + seen["size"])
                      if at not in covered and at not in text)
        planned.append((region, content, kept, before, grids, name, target, keep))
    for region, content, kept, before, grids, name, target, keep in planned:
        picture = inside(directory, name[:-len(".txt")] + ".png")
        if grids:
            if keep:
                kept.append(before)
            else:
                written = gb_data_maps.grids_text(region["file"], content, grids).encode("utf-8")
                target.write_bytes(written)
                kept.append({"kind": "map", "file": name, "grids": grids, "sha256": sha256(written),
                             "data_sha256": sha256(b"".join(content[offset:offset + width * rows]
                                                            for offset, width, rows in grids))})
            picture.write_bytes(gb_data_maps.preview(image, region["offset"], grids, gb_data_maps.tiles_of(region),
                                                     content, colours_of))
            files += 1
            count += len(grids)
            total += sum(width * rows for _, width, rows in grids)
        elif before is not None:
            for stale in (target, picture):
                if stale.exists():
                    stale.unlink()
        if kept:
            region["views"] = sorted(kept, key=lambda view: spans(view)[0])
        else:
            region.pop("views", None)
    write_manifest(directory, manifest)
    read_at = ", ".join(f"{bank:02X}:{address:04X}" for bank, address in sorted(readers)) or "no instruction"
    print(f"{count:,} {'grid' if count == 1 else 'grids'} of tile numbers, {total:,} bytes, in {files} "
          f"{'region' if files == 1 else 'regions'}: each region's are in a .map.txt beside its file, with a "
          f"picture of them as the tree draws them now (.map.png)")
    print(f"  text is left alone: what {read_at} read")
    if edited_kept:
        print(f"  {edited_kept} of the files {'has' if edited_kept == 1 else 'have'} been edited and "
              f"{'is' if edited_kept == 1 else 'are'} left as "
              f"{'it is' if edited_kept == 1 else 'they are'}, the picture drawn from the edit (--force writes "
              f"{'it' if edited_kept == 1 else 'them'} again from the region's file)")
    if left_out:
        print(f"  {left_out:,} more bytes lie in bytes a picture or a file of text has, and are left to it")
    if unseen:
        print(f"  {unseen:,} more bytes reached a tile map as they are and were in no cell of the screen when a "
              f"frame was whole: nothing says how they lie")
    return 0


def command_annotations(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    args.records = records_given(args.records)
    image = b"".join(content for _, content in loaded)
    checksum = (image[0x14E] << 8) | image[0x14F] if len(image) > 0x14F else 0
    seen = load_uses(args.records, len(image), checksum)
    kind_at = bytearray(len(image))             # 1 where the tree has the byte in a file of code
    for region, _ in loaded:
        if region["kind"] == "code":
            kind_at[region["offset"]:region["offset"] + region["size"]] = b"\x01" * region["size"]
    # A byte is shown to be data by having been used as it is: drawn, played, sent to the cartridge
    # as a bank number, or taken for half of an address. One that was only read, or only worked out
    # from, is not shown to be anything: a game reads its own code too, to copy a routine into RAM
    # or to add itself up. Runs of such bytes, each inside one bank, as an annotation is.
    runs: list[list] = []                       # [offset, size, uses, bytes of it in files of code]
    for offset in sorted(seen):
        shown = seen[offset][0]
        if not shown:
            continue
        last = runs[-1] if runs else None
        if last and last[0] + last[1] == offset and offset % BANK:
            last[1] += 1
            last[2] |= shown
            last[3] += kind_at[offset]
        else:
            runs.append([offset, 1, set(shown), kind_at[offset]])
    lines = [f"# What {manifest['rom'].get('title') or 'the game'} showed to be data, from "
             f"{len(args.records)} record{'s' if len(args.records) != 1 else ''} of it played:",
             "# bytes it used as they are, and what for. Written by gb_data.py annotations for",
             "# gbrecomp --annotations: the analysis decodes no instruction in these bytes.", ""]
    for offset, size, uses, _ in runs:
        bank, address = place(offset)
        lines.append(f"data {bank:02X}:{address:04X} {size} ; {'+'.join(use for use in USES if use in uses)}")
    output = Path(args.output)
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    except OSError as error:
        raise DataError(f"cannot write {output}: {error}") from error
    total = sum(run[1] for run in runs)
    in_code = sum(run[3] for run in runs)
    print(f"{len(args.records)} record{'s' if len(args.records) != 1 else ''}: the game used {total:,} bytes as "
          f"they are, in {len(runs):,} {'run' if len(runs) == 1 else 'runs'}")
    if in_code:
        print(f"  {in_code:,} of them, in {sum(1 for run in runs if run[3]):,} of the runs, are in files of code: "
              f"the analysis decoded instructions there")
    else:
        print("  none of them is in a file of code: the analysis already has them as data")
    print(f"wrote {output}: give it to gbrecomp with --annotations, for the export and for the game alike, and "
          f"split the new export")
    return 0


def command_unpackers(args: argparse.Namespace) -> int:
    import gb_data_unpackers    # finding the routine that unpacks: only this command needs it

    _, loaded = load_tree(Path(args.directory))
    image = b"".join(content for _, content in loaded)
    checksum = (image[0x14E] << 8) | image[0x14F] if len(image) > 0x14F else 0
    try:
        return gb_data_unpackers.run(args.records, len(image), checksum, args.output,
                                     [region for region, _ in loaded])
    except gb_data_unpackers.UnpackersError as error:
        raise DataError(str(error)) from error


def command_pointers(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    image = b"".join(content for _, content in loaded)
    checksum = (image[0x14E] << 8) | image[0x14F] if len(image) > 0x14F else 0
    args.records = records_given(args.records)
    uses = load_uses(args.records, len(image), checksum)
    try:
        links, reached, numbers = gb_data_pointers.load_links(args.records, len(image), checksum)
        named = gb_data_pointers.load_named(args.named, manifest["rom"]["sha1"]) if args.named else None
        things, kinds, counts = gb_data_pointers.find_things([region for region, _ in loaded], image, links,
                                                             reached, numbers, uses, named)
    except gb_data_pointers.PointersError as error:
        raise DataError(str(error)) from error
    manifest["things"] = things
    write_manifest(directory, manifest)
    for line in gb_data_pointers.report(things, kinds, counts, len(args.records)):
        print(line)
    print(f"wrote what leads where into {directory / 'manifest.json'} (`things`): `build` moves one that a string "
          f"grew in, into the free space {gb_data_pointers.FREE} declares (`free` proposes it)")
    return 0


def command_free(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    if "readers" not in manifest:
        raise DataError("nothing says yet what the game reads, and so nothing says what it never does: run it with "
                        "--data-use, and `uses` on what it records")
    image = b"".join(content for _, content in loaded)
    try:
        runs = gb_data_pointers.propose_free([region for region, _ in loaded], image,
                                             gb_data_pointers.checked_things(manifest), view_spans(loaded))
    except gb_data_pointers.PointersError as error:
        raise DataError(str(error)) from error
    banks = -(-len(image) // BANK)
    print(f"{len(runs)} of {banks} {'bank ends' if len(runs) == 1 else 'banks end'} in a run of one byte value that "
          f"is data, that no record read and nothing was drawn from: {sum(size for _, size, _ in runs):,} bytes")
    for offset, size, value in runs:
        bank, address = place(offset)
        print(f"  bank {bank:02X}: {size:6,d} bytes of {value:02X} from {bank:02X}:{address:04X}")
    if runs:
        print(f"  the first {gb_data_pointers.FREE_MARGIN} bytes of each run are left out: what lies in front of it "
              f"may end in the same value")
    if args.output is None:
        print(f"-o {directory / gb_data_pointers.FREE} declares them free: it is a proposal, so read it first")
        return 0
    output = Path(args.output)
    if output.exists() and not args.force:
        raise DataError(f"{output} is already there: --force writes over it")
    try:
        output.write_text(json.dumps(gb_data_pointers.free_document(runs), indent=1) + "\n", encoding="utf-8",
                          newline="\n")
    except OSError as error:
        raise DataError(f"cannot write {output}: {error}") from error
    print(f"wrote {output}: `build` writes what has to move there, and nowhere else")


def command_unpack(args: argparse.Namespace) -> int:
    import gb_data_unpack       # having the game unpack by itself: only this command needs it

    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    unpackers = tree_unpackers(directory, manifest)
    if unpackers is None:
        raise DataError(f"{directory / gb_unpacked.DECLARATION} is not there: it says which routine unpacks, and "
                        f"has to be written first (DATA_FILES.md)")
    try:
        return gb_data_unpack.run(args, manifest, b"".join(content for _, content in loaded), unpackers,
                                  unpacked_picture)
    except (gb_data_unpack.UnpackError, gb_unpacked.UnpackedError) as error:
        raise DataError(str(error)) from error


def command_names(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, _ = load_tree(directory)
    try:
        for line in gb_data_names.bring_in(directory, manifest, args.sym, args.seen):
            print(line)
        named = write_manifest(directory, manifest)
        if args.list:
            gb_data_names.write_list(args.list, manifest)
    except gb_data_names.NamesError as error:
        raise DataError(str(error)) from error
    if named["failed"]:
        raise DataError(f"{named['failed']}: nothing was renamed")
    for line in gb_data_names.said(named):
        print(line)
    if args.list:
        print(f"wrote {args.list}: everything the tree has, with its address, its name and its file")
    return 0


def command_streams(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest, loaded = load_tree(directory)
    path = directory / gb_data_streams.DECLARATION
    if not path.exists():
        raise DataError(f"{path} is not there: it says what the commands of the game's sound engine are and where "
                        f"their streams start, and has to be written first (DATA_FILES.md)")
    try:
        declaration = gb_data_streams.load_declaration(path)
    except gb_data_streams.StreamsError as error:
        raise DataError(str(error)) from error
    asked = []
    for word in args.reader:
        try:
            bank, address = (int(part, 16) for part in word.split(":"))
        except ValueError:
            raise DataError(f"--reader takes BANK:ADDRESS in hexadecimal, not '{word}'") from None
        asked.append((bank, address))
    regions = [region for region, _ in loaded]
    found = gb_data_streams.follow(declaration, b"".join(content for _, content in loaded))
    passages, counts = gb_data_streams.passages_of(found, regions)
    # What to write is worked out for every region before anything is written, as `text` does.
    planned = []
    declared = {gb_data_streams.DECLARATION: declaration}
    left_out = 0
    for index, (region, content) in enumerate(loaded):
        if region["kind"] != "data":
            continue
        kept = [view for view in region.get("views", ()) if view["kind"] != "stream"]
        before = next((view for view in region.get("views", ()) if view["kind"] == "stream"), None)
        taken = [span for view in kept for span in spans(view)]
        mine = []
        for passage in passages.get(index, ()):
            if any(start < passage[0] + passage[1] and passage[0] < stop for start, stop in taken):
                left_out += passage[1]      # a picture or a file of text has those bytes
                continue
            mine.append(passage)
        if not mine and before is None:
            continue
        stem = region["file"][:-len(".bin")] if region["file"].endswith(".bin") else region["file"]
        name = gb_data_names.kept(region, "stream", None, stem + ".stream.txt")
        target = inside(directory, name)
        keep = False
        if target.exists() and not args.force:
            if before is None:
                raise DataError(f"{name} is already there and the manifest does not know it: --force writes over "
                                f"it")
            # A file somebody edited is left as it is, with its view, as `maps` leaves one.
            there = target.read_bytes()
            if sha256(there) != before["sha256"]:
                try:
                    new = b"".join(made for made, _ in stream_said(directory, region, before, content, there,
                                                                   declared))
                    keep = sha256(new) != before["data_sha256"]
                except DataError:
                    keep = True
        planned.append((region, content, kept, before, mine, name, target, keep))
    files = count = total = edited_kept = 0
    by_grammar: dict[str, list[int]] = {}
    for region, content, kept, before, mine, name, target, keep in planned:
        if keep:
            kept.append(before)
            edited_kept += 1
        elif mine:
            written = gb_data_streams.passages_text(region, mine, found).encode("utf-8")
            target.write_bytes(written)
            kept.append({"kind": "stream", "file": name, "declaration": gb_data_streams.DECLARATION,
                         "passages": mine, "sha256": sha256(written),
                         "data_sha256": sha256(b"".join(content[offset:offset + size] for offset, size, _ in mine))})
            files += 1
            count += len(mine)
            for _, size, grammar in mine:
                total += size
                entry = by_grammar.setdefault(grammar, [0, 0])
                entry[0] += 1
                entry[1] += size
        elif target.exists():
            target.unlink()
        if kept:
            region["views"] = sorted(kept, key=lambda view: spans(view)[0])
        else:
            region.pop("views", None)
    write_manifest(directory, manifest)

    whole = sum(1 for offset, _, _ in found.starts if found.whole(offset))
    decoded = len(found.covered) - found.covered.count(0)
    print(f"{len(found.starts):,} {'start' if len(found.starts) == 1 else 'starts'} in "
          f"{len({offset for offset, _, _ in found.starts}):,} places: {whole:,} read to where their commands end "
          f"them, {len(found.starts) - whole:,} not. {len(found.commands):,} commands, {decoded:,} bytes")
    print(f"{count:,} {'passage' if count == 1 else 'passages'}, {total:,} bytes, in {files} "
          f"{'region' if files == 1 else 'regions'}: each region's are in a .stream.txt beside its file")
    for grammar, (number_of, size) in sorted(by_grammar.items()):
        print(f"  {grammar:12s} {number_of:6,d} {'passage ' if number_of == 1 else 'passages'} {size:8,d} bytes")
    if counts["pinned"]:
        print(f"  {counts['pinned']:,} more bytes of commands are in files of code: the analysis decoded instructions "
              f"there, and they cannot be edited until it is told they are data (--annotations FILE writes that)")
    if counts["split"]:
        print(f"  {counts['split']:,} more bytes are of commands that lie in two files, and are in neither's text")
    if left_out:
        print(f"  {left_out:,} more bytes lie in bytes a picture or a file of text has, and are left to it")
    if edited_kept:
        print(f"  {edited_kept} of the files {'has' if edited_kept == 1 else 'have'} been edited and "
              f"{'is' if edited_kept == 1 else 'are'} left as {'it is' if edited_kept == 1 else 'they are'} "
              f"(--force writes {'it' if edited_kept == 1 else 'them'} again from the region's file)")
    for line in found.wrong[:args.stops]:
        print(f"  {line}")
    if len(found.wrong) > args.stops:
        print(f"  and {len(found.wrong) - args.stops} more starts that lead nowhere")
    if found.stops:
        print(f"  {len(found.stops):,} {'stream' if len(found.stops) == 1 else 'streams'} stopped before "
              f"{'its' if len(found.stops) == 1 else 'their'} end, and nothing was read past there:")
        for offset in sorted(found.stops)[:args.stops]:
            print(f"    {gb_data_streams.at(offset)}: {found.stops[offset]}")
        if len(found.stops) > args.stops:
            print(f"    and {len(found.stops) - args.stops} more (--stops N lists more)")
    if found.twice:
        first = min(found.twice)
        print(f"  {len(found.twice):,} {'place is' if len(found.twice) == 1 else 'places are'} read in two grammars, "
              f"and written in the first: {gb_data_streams.at(first)} as '{found.twice[first][0]}' and as "
              f"'{found.twice[first][1]}'")
    if args.annotations:
        # The bytes of the commands as lines for the analysis, which then decodes no instruction in them.
        lines = [f"# The commands of music and sound that {gb_data_streams.DECLARATION} declares, as gb_data.py "
                 f"streams read them:", "# for gbrecomp --annotations, so that the analysis decodes no instruction "
                 "in these bytes.", ""]
        runs = 0
        offset = 0
        while offset < len(found.covered):
            if not found.covered[offset]:
                offset += 1
                continue
            end = offset
            while end < len(found.covered) and found.covered[end] and (end == offset or end % BANK):
                end += 1
            bank, address = place(offset)
            lines.append(f"data {bank:02X}:{address:04X} {end - offset} ; commands")
            runs += 1
            offset = end
        try:
            Path(args.annotations).parent.mkdir(parents=True, exist_ok=True)
            Path(args.annotations).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        except OSError as error:
            raise DataError(f"cannot write {args.annotations}: {error}") from error
        print(f"wrote {args.annotations}: {decoded:,} bytes in {runs:,} {'run' if runs == 1 else 'runs'}, for "
              f"gbrecomp --annotations, for the export and for the game alike")
    # The check against the game played: do the instructions that read commands read what was decoded?
    if "readers" not in manifest:
        print("nothing says yet what the game reads (`uses`, from records made with --data-use): the grammars are "
              "not checked against the game played")
        return 0
    readers, how = (asked, "named") if asked else (declaration["readers"], f"{gb_data_streams.DECLARATION} says") \
        if declaration["readers"] else (gb_data_streams.readers_found(regions, found.covered), "found")
    if not readers:
        print("against the game played: no instruction was seen to read mostly decoded commands. Name the ones that "
              "read commands with --reader (`uses` lists the instructions that read), or record more of the game")
        return 0
    result = gb_data_streams.check(regions, found, readers)
    read_at = ", ".join(f"{bank:02X}:{address:04X}" for bank, address in sorted(readers))
    print(f"against the game played, whose commands are read at {read_at} ({how}):")
    print(f"  of the {result['inside'] + result['outside']:,} bytes of data files that those instructions read "
          f"first, {result['inside']:,} are in decoded commands and {result['outside']:,} are not")
    for offset, size in result["missed"][:args.stops]:
        print(f"    not decoded: {size} {'byte' if size == 1 else 'bytes'} at {gb_data_streams.at(offset)}")
    if len(result["missed"]) > args.stops:
        print(f"    and {len(result['missed']) - args.stops} more runs")
    others = sorted(result["others"].items(), key=lambda item: (-item[1], item[0]))[:3]
    print(f"  of the {result['decoded']:,} decoded bytes in data files, those instructions read {result['inside']:,}, "
          f"another instruction read {result['by_others']:,} first"
          + (f" ({', '.join(f'{bank:02X}:{address:04X} {size:,}' for (bank, address), size in others)})"
             if others else "")
          + f", and no record saw {result['unseen']:,} read")
    return 0


def number(text: str) -> int:
    return int(text, 0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    split = commands.add_parser("split", help="write every region of the ROM to a file of its own, with a manifest")
    split.add_argument("export", type=Path, help="the lift export of the ROM (gbrecomp --lift-export)")
    split.add_argument("--rom", required=True, type=Path)
    split.add_argument("-o", "--output", required=True, type=Path)
    split.add_argument("--force", action="store_true", help="write over a data tree that is already there")
    split.add_argument("--keep", type=Path, metavar="OLD",
                       help="a tree split before, to take over into the new one: its edits, its pictures and text, "
                            "what the game unpacks. It is only read")
    split.add_argument("--lose-edits-in-code", action="store_true",
                       help="with --keep: go on where edited bytes are code in the new tree, without them")
    split.add_argument("--discard-edits", action="store_true",
                       help="with --force: write over a tree even if it has edits")
    split.set_defaults(run=command_split)
    tiles = commands.add_parser("tiles", help="draw a data file's uncompressed tiles as a PNG, which `build` reads "
                                              "back once it has been edited")
    tiles.add_argument("directory", type=Path)
    tiles.add_argument("file", nargs="?",
                       help="the data file, as the manifest names it: bank_05/4180_unknown.bin. Without one: a "
                            "picture of every run of tiles the game was seen to draw (`uses`, from records made "
                            "with --data-use-tiles), where they start, as many and at the bits a pixel it showed")
    tiles.add_argument("--offset", type=number, default=0,
                       help="where in the file the tiles start (default 0; 0x for hexadecimal)")
    tiles.add_argument("--tiles", type=number, help="how many tiles (default: the rest of the file)")
    tiles.add_argument("--width", type=number, default=16, help="tiles to a row of the picture (default 16)")
    tiles.add_argument("--bpp", type=int, choices=(2, 1), default=2,
                       help="bits a pixel: 2 (default, 16 bytes a tile) or 1 (8 bytes a tile, as many fonts are)")
    tiles.add_argument("--force", action="store_true", help="draw again over a picture that has been edited")
    tiles.add_argument("--greys", action="store_true",
                       help="draw in the four plain greys, whatever colours the game was seen to show the tiles in")
    tiles.set_defaults(run=command_tiles)
    build = commands.add_parser("build", help="put the files together again into a ROM image")
    build.add_argument("directory", type=Path)
    build.add_argument("-o", "--output", required=True, type=Path)
    build.add_argument("--expect-original", action="store_true",
                       help="exit 1 unless the image is the original ROM: the check that nothing was lost")
    build.add_argument("--move-all", action="store_true",
                       help="move every thing `pointers` says can move into the free space, and spoil the bytes the "
                            "game read at each old place: an image for proving the pointers, not for playing")
    build.add_argument("--moved-header", type=Path, metavar="FILE",
                       help="write where the names of the game's source lead in this image: the source folder's "
                            "src/lifted_moved.h, which the game is then built again with. Needed when a thing "
                            "moves that an operand the source names leads to")
    build.set_defaults(run=command_build)
    status = commands.add_parser("status", help="say which files have been edited")
    status.add_argument("directory", type=Path)
    status.set_defaults(run=command_status)
    uses = commands.add_parser("uses", help="write into the manifest what a game run with --data-use was seen "
                                            "to do with each region")
    uses.add_argument("directory", type=Path)
    uses.add_argument("records", nargs="+", type=Path,
                      help="files a game wrote with --data-use, or a folder of them")
    uses.add_argument("--readers", type=int, default=12, help="how many of the reading instructions to list (12)")
    uses.set_defaults(run=command_uses)
    annotations = commands.add_parser("annotations", help="write the bytes the game showed to be data as a file "
                                                          "for gbrecomp --annotations")
    annotations.add_argument("directory", type=Path)
    annotations.add_argument("records", nargs="+", type=Path,
                             help="files a game wrote with --data-use, or a folder of them")
    annotations.add_argument("-o", "--output", required=True, type=Path)
    annotations.set_defaults(run=command_annotations)
    unpacked = commands.add_parser("unpacked", help="put what the game's own routine unpacked, which a game wrote "
                                                    "with --unpacked-harvest, into the tree as files to edit")
    unpacked.add_argument("directory", type=Path)
    unpacked.add_argument("harvests", nargs="+", type=Path,
                          help="files a game wrote with --unpacked-harvest, or a folder of them (its .json files)")
    unpacked.add_argument("--force", action="store_true",
                          help="write again the things that are in the tree already, edits and all")
    unpacked.set_defaults(run=command_unpacked)
    watch_pack = commands.add_parser("watch-pack", help="write a pack that only watches, with nothing replaced "
                                                        "whatever is edited in the tree: the pack to harvest a "
                                                        "recorded run with")
    watch_pack.add_argument("directory", type=Path)
    watch_pack.add_argument("-o", "--output", required=True, type=Path, help="the pack to write (a .gbup)")
    watch_pack.set_defaults(run=command_watch_pack)
    unpackers = commands.add_parser("unpackers", help="say which function unpacks, from records of a game run "
                                                      "with --data-use-follow, and propose its declaration")
    unpackers.add_argument("directory", type=Path)
    unpackers.add_argument("records", nargs="+", type=Path,
                           help="files a game wrote with --data-use and --data-use-follow")
    unpackers.add_argument("-o", "--output", type=Path, help="write the declaration proposed to this file")
    unpackers.set_defaults(run=command_unpackers)
    glyphs = commands.add_parser("glyphs", help="draw the tile each byte draws in a save state, for the charmap "
                                                "to be written from")
    glyphs.add_argument("directory", type=Path)
    glyphs.add_argument("state", type=Path, help="a save state made while text was on the screen")
    glyphs.add_argument("-o", "--output", type=Path, help="the picture (default DIR/glyphs.png)")
    glyphs.add_argument("--reader", action="append", default=[], metavar="BANK:ADDRESS",
                        help="an instruction that reads text, in place of the ones found; repeatable")
    glyphs.set_defaults(run=command_glyphs)
    text = commands.add_parser("text", help="write the strings of each region to a file of text, which `build` "
                                            "reads back once it has been edited")
    text.add_argument("directory", type=Path)
    text.add_argument("--reader", action="append", default=[], metavar="BANK:ADDRESS",
                      help="an instruction that reads text, in place of the ones found; repeatable")
    text.add_argument("--read", action="store_true",
                      help="also the strings nobody saw printed: the data is read for more of what the strings "
                           "seen are made of")
    text.add_argument("--force", action="store_true", help="write again over text that has been edited")
    text.add_argument("--keep-edited", action="store_true",
                      help="leave a file of text that has been edited as it is, and write the others")
    text.set_defaults(run=command_text)
    pointers = commands.add_parser("pointers", help="write into the manifest which pointers lead to each thing a "
                                                    "game run with --data-use-pointers read, and whether it can "
                                                    "be moved")
    pointers.add_argument("directory", type=Path)
    pointers.add_argument("records", nargs="+", type=Path,
                          help="files a game wrote with --data-use and --data-use-pointers")
    pointers.add_argument("--named", type=Path, metavar="FILE",
                          help="the operands the game's source names: .lift/named-operands.json of a source folder "
                               "made with `gb_lift.py source --data-tree`. A thing such an operand leads to can "
                               "move, since the operand is turned in the source")
    pointers.set_defaults(run=command_pointers)
    free = commands.add_parser("free", help="propose the empty ends of banks as the free space that what grows is "
                                            "moved into")
    free.add_argument("directory", type=Path)
    free.add_argument("-o", "--output", type=Path, help="write the proposal to this file: DIR/free.json declares it")
    free.add_argument("--force", action="store_true", help="write over a file that is already there")
    free.set_defaults(run=command_free)

    unpack = commands.add_parser("unpack", help="have the game unpack streams nobody played as far as, and write "
                                                "a harvest of each for `unpacked` to take")
    unpack.add_argument("directory", type=Path)
    unpack.add_argument("--game", required=True, type=Path, help="the game's executable, generated and built")
    unpack.add_argument("--state", type=Path,
                        help="a save state of it to make the calls in (without one: the game as it is switched on)")
    unpack.add_argument("-o", "--output", required=True, type=Path, help="the folder for the harvests")
    unpack.add_argument("--streams", action="append", default=[], type=Path, metavar="FILE",
                        help="a list of streams, a BANK:ADDRESS in hexadecimal to a line; repeatable")
    unpack.add_argument("--tables", action="store_true",
                        help="the streams the tables of pointers name that unpackers.json declares")
    unpack.add_argument("--walk", action="store_true",
                        help="walk on: try as a stream what follows each stream that is known, the tree's "
                             "included. What this finds is a guess")
    unpack.add_argument("--unpacker", metavar="NAME", help="which unpacker, where unpackers.json declares several")
    unpack.add_argument("--workers", type=int, default=4, help="copies of the game to run at once (4, at most 8)")
    unpack.add_argument("--walk-limit", type=int, default=4096, help="how many guesses a walk makes at most (4096)")
    unpack.add_argument("--keep-records", action="store_true",
                        help="keep the records of what each called routine read (OUTPUT/run/records)")
    unpack.set_defaults(run=command_unpack)

    names = commands.add_parser("names", help="call the files what names.sym, at the top of the tree, calls them")
    names.add_argument("directory", type=Path)
    names.add_argument("--sym", action="append", default=[], type=Path, metavar="FILE",
                       help="an RGBDS symbol file to bring names in from: its labels that lie on what the tree has "
                            "are added to names.sym, where there is no line for the address yet; repeatable")
    names.add_argument("--seen", action=argparse.BooleanOptionalAction,
                       help="also call a data file that has no name by what the game was seen to use most of it as "
                            "(`uses`), from now on; --no-seen stops that")
    names.add_argument("--list", type=Path, metavar="FILE",
                       help="write everything the tree has to this file, with its address, its name and its file")
    names.set_defaults(run=command_names)

    maps = commands.add_parser("maps", help="write the tile numbers each region has of tile maps to a file, which "
                                            "`build` reads back once it has been edited, and draw a picture of them")
    maps.add_argument("directory", type=Path)
    maps.add_argument("--reader", action="append", default=[], metavar="BANK:ADDRESS",
                      help="an instruction that reads text, whose bytes are left alone, in place of the ones found; "
                           "repeatable. `none`: no instruction reads text")
    maps.add_argument("--force", action="store_true", help="write again over tile numbers that have been edited")
    maps.set_defaults(run=command_maps)

    streams = commands.add_parser("streams", help="write the commands of the game's music and sound, as "
                                                  "streams.json declares them, to files of text, which `build` "
                                                  "reads back once they have been edited")
    streams.add_argument("directory", type=Path)
    streams.add_argument("--reader", action="append", default=[], metavar="BANK:ADDRESS",
                         help="an instruction that reads commands, for the check against the game played, in place "
                              "of the ones streams.json names or the ones found; repeatable")
    streams.add_argument("--force", action="store_true", help="write again over commands that have been edited")
    streams.add_argument("--stops", type=int, default=12, help="how many of the places a stream stopped to list (12)")
    streams.add_argument("--annotations", type=Path, metavar="FILE",
                         help="also write the bytes of the commands as `data` lines for gbrecomp --annotations: the "
                              "analysis then decodes no instruction in them")
    streams.set_defaults(run=command_streams)
    args = parser.parse_args()
    # A game's letters are whatever its charmap says, and a console or a pipe may have no way to
    # write one of them: it is then written as its number, where it would have stopped the tool.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="backslashreplace")
    try:
        return args.run(args)
    except DataError as error:
        print(f"gb_data: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
