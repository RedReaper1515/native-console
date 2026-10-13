#!/usr/bin/env python3
"""A data tree split again with what was done to the old one kept, end to end.

Builds a small ROM with a routine in the middle of its home bank's data, and
makes its export three ways: by itself (the routine is code, and the data on
either side of it two files), told that the routine is data (`--annotations`:
one file), and cut by hand through the middle of things. No game is compiled.
A tree of the first export gets something of every kind done to it: two files
edited, four pictures of tiles drawn and two painted, five strings written out
as text and four changed, a comment put in the text, two things the game
unpacks harvested and one redrawn, a charmap, and files nobody declared. Then
`gb_data.py split ... --keep OLD` has to:

- with the same export, write the same tree: every file and the manifest;
- with the export told more, put the two files' views into the one file, with
  their offsets moved and their files named after it, and say that `uses` has
  to be run again, since the old tree knows nothing of the bytes that were
  code;
- back from that tree to the first export, put them into two again, and carry
  what `uses` wrote exactly: what `uses` writes into a fresh tree from the
  same record. An edit to the routine's bytes, which are code again, has to
  stop it, and `--lose-edits-in-code` has to go on without those bytes only;
- with the export cut by hand, leave behind and name the picture and the
  strings whose bytes are in two files or partly code, with what was changed
  through them in the bytes of the new files, and carry the rest;
- carry what a later gb_data.py may add and this one has no rule for (a kind
  of view, a field of a region, a field of the manifest, a file's own name)
  where its region is the same bytes, and name it where it is not;
- each time, leave the old tree as it was and make a tree from which `build`
  makes the image and the pack it makes from the old one;
- refuse a tree of another ROM, a folder inside the old tree, an old tree that
  does not build, and a new folder that holds a tree.

And `split --force` has to refuse to write over a tree that has edits unless
it is told to discard them, and say what else it wrote over.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import types

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"

START = 0x0150
SPARE = 0x0260           # two tiles in the data in front of the routine
SUB = 0x0300             # the routine: five bytes of code, or of data once the analysis is told
FONT = 0x0400            # 40 tiles at 1 bit a pixel, in the data behind it
TILES = 0x4000           # all of bank 1
TEXT = {0x0240: "HELLO@", 0x0600: "GOOD DAY@", 0x0610: "BYE NOW@", 0x0700: "MIDDLE @", 0x0720: "FAREWEL@"}
SAID = {0x0240: "HOWDY@", 0x0600: "FINE DAY@", 0x0610: "BYE@", 0x0720: "GOODBYE@"}     # what four of them become
LETTERS = {0x7F: " ", 0x50: "@", **{0x80 + index: chr(65 + index) for index in range(26)}}
PRINTS, COPIES, DRAWS = (0, 0x0160), (0, 0x0170), (1, 0x4100)      # the instructions the record says read


def spell(text: str) -> bytes:
    by_letter = {letter: byte for byte, letter in LETTERS.items()}
    return bytes(by_letter[letter] for letter in text)


def make_rom() -> bytes:
    noise = random.Random(2024)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, START & 0xFF, START >> 8])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"DATA KEEP"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    rom[START:START + 6] = bytes([0xF3, 0xCD, SUB & 0xFF, SUB >> 8, 0x18, 0xFE])   # di / call Sub / jr to itself
    rom[SUB:SUB + 5] = bytes([0x3E, 0x12, 0xE0, 0x80, 0xC9])                       # ld a,$12 / ldh [$80],a / ret
    rom[SPARE:SPARE + 32] = noise.randbytes(32)
    rom[FONT:FONT + 320] = noise.randbytes(320)
    rom[TILES:TILES + 0x4000] = noise.randbytes(0x4000)
    for address, text in TEXT.items():
        rom[address:address + len(text)] = spell(text)
    return bytes(rom)


def run(command: list, expect: int = 0, timeout: int = 300) -> subprocess.CompletedProcess:
    result = subprocess.run([str(word) for word in command], capture_output=True, text=True, timeout=timeout)
    if result.returncode != expect:
        raise AssertionError(f"exit {result.returncode}, not {expect}, from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def gb_data(*arguments, expect: int = 0) -> str:
    result = run([sys.executable, GB_DATA, *arguments], expect)
    return result.stderr if expect else result.stdout


def load_tool(later: bool = False) -> types.ModuleType:
    """gb_data.py as a module. `later` is one from after something else was
    added to it: a kind of view, a field of a region and a field of the
    manifest that the step that keeps has no rule for."""
    source = GB_DATA.read_text(encoding="utf-8")
    name = "gb_data_later" if later else "gb_data_now"
    if later:
        source, places = re.subn(r"(set\(manifest\) - \{[^}]*)\}", r'\1, "notes"}', source, count=1)
        assert places == 1, "the manifest's fields are no longer listed where this test adds one"
    module = types.ModuleType(name)
    module.__file__ = str(GB_DATA)
    sys.modules[name] = module              # `split` hands itself on as sys.modules[__name__]
    if str(GB_DATA.parent) not in sys.path:
        sys.path.insert(0, str(GB_DATA.parent))
    exec(compile(source, str(GB_DATA), "exec"), module.__dict__)
    if later:
        module.VIEW_FIELDS["marks"] = set(module.VIEW_FIELDS["tiles"])     # drawn and read back as tiles are
        module.REGION_FIELDS.add("name")
    return module


def files_of(tree: Path) -> dict[str, bytes]:
    return {path.relative_to(tree).as_posix(): path.read_bytes() for path in sorted(tree.rglob("*")) if path.is_file()}


def manifest_of(tree: Path) -> dict:
    return json.loads((tree / "manifest.json").read_text(encoding="utf-8"))


def region_at(tree: Path, offset: int) -> dict:
    return next(region for region in manifest_of(tree)["regions"]
                if region["offset"] <= offset < region["offset"] + region["size"])


def views_of(tree: Path) -> dict[str, tuple[str, int]]:
    """Each view's file -> (its kind, where in the ROM its first byte is)."""
    found = {}
    for region in manifest_of(tree)["regions"]:
        for view in region.get("views", ()):
            first = view["strings"][0][0] if view["kind"] == "text" else view["offset"]
            found[view["file"]] = (view["kind"], region["offset"] + first)
    return found


def cut(export: dict, pieces: dict[int, list[tuple[int, int]]]) -> dict:
    """The export with some of its data regions cut into the given pieces,
    as (first offset, one past the last): what lies between two pieces is
    then code to `split`."""
    out = json.loads(json.dumps(export))
    regions = []
    for region in out["data_regions"]:
        for first, end in pieces.get(region["rom_offset"], [(region["rom_offset"],
                                                             region["rom_offset"] + region["size"])]):
            assert region["rom_offset"] <= first < end <= region["rom_offset"] + region["size"]
            regions.append(dict(region, rom_offset=first, size=end - first,
                                address=region["address"] + first - region["rom_offset"]))
    out["data_regions"] = regions
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    args = parser.parse_args()
    tool = load_tool()

    with tempfile.TemporaryDirectory(prefix="gbkeep-") as scratch:
        root = Path(scratch)
        rom = make_rom()
        rom_path = root / "keep.gb"
        rom_path.write_bytes(rom)
        analysis = ["--no-scan", "--pointer-scan", "off"]
        told = root / "told.annotations"
        told.write_text(f"data 00:{SUB:04X} 5 ; tiles\n", encoding="utf-8", newline="\n")
        exports = {"alone": root / "alone.json", "told": root / "told.json", "by hand": root / "by-hand.json",
                   "in two": root / "in-two.json"}
        run([args.gbrecomp, rom_path, *analysis, "--lift-export", exports["alone"]])
        run([args.gbrecomp, rom_path, *analysis, "--annotations", told, "--lift-export", exports["told"]])
        alone = json.loads(exports["alone"].read_text(encoding="utf-8"))
        behind = next(region for region in alone["data_regions"] if region["rom_offset"] <= FONT <
                      region["rom_offset"] + region["size"])
        assert behind["rom_offset"] == SUB + 5 and behind["rom_offset"] + behind["size"] == TILES, behind
        ahead = next(region for region in alone["data_regions"] if region["rom_offset"] <= SPARE <
                     region["rom_offset"] + region["size"])
        # Cut by hand: the two tiles in front of the routine are code, and so are two bytes of
        # the fourth string; the fifth is in two files, the
        # first picture of bank 1 is in two files and the second further into one than it was.
        exports["by hand"].write_text(json.dumps(cut(alone, {
            ahead["rom_offset"]: [(ahead["rom_offset"], SPARE), (SPARE + 32, SUB)],
            SUB + 5: [(SUB + 5, 0x0702), (0x0704, 0x0724), (0x0724, TILES)],
            TILES: [(TILES, 0x4080), (0x4080, 0x47C0), (0x47C0, 0x6000), (0x6010, 0x8000)]})), encoding="utf-8")
        # And one that only cuts data in two, with no byte of it turned into code.
        exports["in two"].write_text(json.dumps(cut(alone, {TILES: [(TILES, 0x5000), (0x5000, 0x8000)]})),
                                     encoding="utf-8")

        def split(name: str, tree: Path, *more, expect: int = 0) -> str:
            return gb_data("split", exports[name], "--rom", rom_path, "-o", tree, *more, expect=expect)

        def built(tree: Path) -> tuple[bytes, bytes]:
            """The image and the pack `build` makes of a tree."""
            image = root / "built.gb"
            gb_data("build", tree, "-o", image)
            return image.read_bytes(), image.with_suffix(".gbup").read_bytes()

        def kept(name: str, old: Path, new: Path, *more) -> str:
            """Split again, keeping: the old tree is as it was, and the new one builds what it builds."""
            before = files_of(old)
            said = split(name, new, "--keep", old, *more)
            assert files_of(old) == before, f"{old.name} was written to"
            if "--lose-edits-in-code" not in more:
                assert built(new) == built(old), f"{new.name} does not build what {old.name} builds"
                assert "what it makes of" in said and "but for the edits" not in said, said
            return said

        # --- A tree of the first export, with something of every kind done to it. ---
        first = root / "first"
        split("alone", first)
        front, back, bank = region_at(first, SPARE), region_at(first, FONT), region_at(first, TILES)
        assert region_at(first, SUB)["kind"] == "code" and front["file"] != back["file"]
        assert (front["offset"] + front["size"], back["offset"]) == (SUB, SUB + 5)

        def poke(tree: Path, offset: int, value: int) -> None:
            region = region_at(tree, offset)
            content = bytearray((tree / region["file"]).read_bytes())
            content[offset - region["offset"]] = value
            (tree / region["file"]).write_bytes(content)

        def paint(tree: Path, picture: str, tiles: bytes, bpp: int, width: int) -> None:
            (tree / picture).write_bytes(tool.write_png(*tool.draw_tiles(tiles, bpp, width)))

        # What the game was seen to do, as a record says it: one instruction printed the strings,
        # another read across the routine, a third across the middle of bank 1.
        record = root / "use.json"
        ranges = [{"offset": address, "size": len(text), "as_it_is": ["tile_map"], "worked_out": [],
                   "read_at": list(PRINTS), "one_reader": True} for address, text in TEXT.items()]
        ranges.append({"offset": SUB - 2, "size": 10, "as_it_is": ["tiles"], "worked_out": [],
                       "read_at": list(COPIES), "one_reader": True})
        ranges.append({"offset": 0x4FF0, "size": 0x20, "as_it_is": [], "worked_out": ["tiles"],
                       "read_at": list(DRAWS), "one_reader": False})
        record.write_text(json.dumps({"schema": "gbrt.data-use", "version": 1,
                                      "rom": {"size": len(rom), "global_checksum": (rom[0x14E] << 8) | rom[0x14F]},
                                      "ranges": sorted(ranges, key=lambda entry: entry["offset"])}), encoding="utf-8")
        charmap = json.dumps({"schema": "gbrecomp.charmap", "version": 1,
                              "letters": {f"{byte:02X}": letter for byte, letter in LETTERS.items()}})

        gb_data("uses", first, record)
        (first / "charmap.json").write_text(charmap, encoding="utf-8")
        gb_data("text", first)
        gb_data("tiles", first, bank["file"], "--tiles", "16")
        gb_data("tiles", first, bank["file"], "--offset", "0x800", "--tiles", "8", "--width", "4")
        gb_data("tiles", first, back["file"], "--offset", str(FONT - back["offset"]), "--tiles", "40", "--bpp", "1")
        gb_data("tiles", first, front["file"], "--offset", str(SPARE - front["offset"]), "--tiles", "2")
        pictures = {address: name for name, (kind, address) in views_of(first).items() if kind == "tiles"}
        texts = {address: name for name, (kind, address) in views_of(first).items() if kind == "text"}
        assert sorted(pictures) == [SPARE, FONT, TILES, TILES + 0x800] and sorted(texts) == [0x0240, 0x0600]
        # Painted: the first tile of bank 1 black, and the font's first tile a box.
        paint(first, pictures[TILES], bytes([0xFF] * 16) + rom[TILES + 16:TILES + 256], 2, 16)
        box = bytes([0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF])
        paint(first, pictures[FONT], box + rom[FONT + 8:FONT + 320], 1, 16)
        # Written: four strings changed, one of them to fewer bytes, and a comment.
        for address, name in texts.items():
            text = (first / name).read_text(encoding="utf-8")
            for at, said in SAID.items():
                text = text.replace(f"|{TEXT[at]}|", f"|{said}|")
            # One string is spelt another way and says the same bytes: that is no edit, and it stays.
            text = text.replace("|MIDDLE @|", "|<8C>IDDLE @|")
            text = text.replace("+02FB ", "# the greeting at the console\n+02FB ")
            if address == 0x0600:
                text += "# the end of the talk\n"
            (first / name).write_text(text, encoding="utf-8", newline="\n")
        assert "# the greeting" in (first / texts[0x0600]).read_text(encoding="utf-8")
        poke(first, 0x0200, 0xAA)
        poke(first, 0x7000, rom[0x7000] ^ 0xFF)
        # Unpacked: two things a routine made, one of them redrawn.
        (first / "unpackers.json").write_text(json.dumps({"schema": "gbrecomp.unpackers", "version": 1, "unpackers": [
            {"name": "pictures", "function": {"bank": 0, "address": START},
             "stream": {"bank": {"register": "a"}, "address": {"register": "hl"}},
             "made": [{"address": 0xC000, "size": 32}], "as": {"kind": "tiles", "bpp": 2, "width": 2}}]}),
            encoding="utf-8")
        made = [bytes(range(32)), bytes(range(64, 96))]
        harvest = root / "harvest.json"
        harvest.write_text(json.dumps({
            "schema": "gbrt.unpacked-harvest", "version": 1,
            "rom": {"size": len(rom), "sha256": hashlib.sha256(rom).hexdigest()},
            "unpackers": [{"name": "pictures", "bank": 0, "address": START}],
            "made": [{"unpacker": "pictures", "bank": 1, "address": 0x5000 + 0x100 * index, "bytes": thing.hex(),
                      "sha256": hashlib.sha256(thing).hexdigest(), "times": 1} for index, thing in enumerate(made)]}),
            encoding="utf-8")
        gb_data("unpacked", first, harvest)
        paint(first, "unpacked/pictures/01_5000.png", bytes([0x0F] * 32), 2, 2)
        # And what nobody declared: a sheet of letters, notes at the top, a file further down.
        (first / "glyphs.png").write_bytes(tool.write_png(8, 8, bytes(64)))
        (first / "notes.txt").write_text("what I am doing here\n", encoding="utf-8")
        (first / "bank_01" / "mine.txt").write_text("a file of my own\n", encoding="utf-8")

        image, pack = built(first)
        expected = bytearray(rom)
        expected[0x0200], expected[0x7000] = 0xAA, rom[0x7000] ^ 0xFF
        expected[TILES:TILES + 16] = bytes([0xFF] * 16)
        expected[FONT:FONT + 8] = box
        for address, said in SAID.items():
            expected[address:address + len(said)] = spell(said)
        assert image == bytes(expected), "the tree this test edits does not build what it means to"
        assert pack.count(bytes([0x0F] * 32)) == 1, "the pack has no replacement"
        edits = gb_data("status", first)
        assert edits.count("edited: ") == 9, edits       # two files, two pictures, four strings, one thing unpacked

        # --- The same export: the same tree. ---
        same = root / "same"
        said = kept("alone", first, same)
        strays = {"bank_01/mine.txt"}
        assert {name: content for name, content in files_of(same).items() if name != "manifest.json"} == \
            {name: content for name, content in files_of(first).items() if name != "manifest.json" and
             name not in strays}
        assert manifest_of(same) == manifest_of(first)
        assert gb_data("status", same) == edits.replace(str(first), str(same))
        assert "2 edited files" in said and "4 pictures of tiles (2 edited)" in said, said
        assert "2 files of text with 5 strings (4 edited)" in said, said
        assert "2 things the game unpacks (1 replacement in the pack)" in said, said
        assert all(f"  {name}\n" in said for name in ("unpackers.json", "charmap.json", "glyphs.png")), said
        assert "what `uses` wrote" in said.split("not kept:")[0], said
        assert "notes.txt, copied as it is" in said and "of the old tree has to be looked at" not in said, said
        assert "1 file of" in said and "bank_01/mine.txt" in said and "left where it is" in said, said
        differ = sum(1 for offset in range(len(rom)) if image[offset] != rom[offset])
        assert f"an image with {differ} bytes edited, and a pack with 1 replacement" in said, said

        # --- Told more: two files of data and the routine between them are one file. ---
        more = root / "more"
        said = kept("told", first, more)
        whole = region_at(more, SUB)
        assert whole["kind"] == "data" and (whole["offset"], whole["offset"] + whole["size"]) == \
            (front["offset"], TILES)
        assert whole["file"] != back["file"], "the test wants the pictures behind the routine to change their names"
        stem = whole["file"][:-len(".bin")]
        assert views_of(more) == {
            f"{stem}.tiles_{SPARE:04X}.png": ("tiles", SPARE), f"{stem}.tiles_{FONT:04X}.png": ("tiles", FONT),
            f"{stem}.text.txt": ("text", 0x0240), pictures[TILES]: ("tiles", TILES),
            pictures[TILES + 0x800]: ("tiles", TILES + 0x800)}, views_of(more)
        for address in (SPARE, FONT):       # the modder's picture, to the byte, under its new name
            assert (more / f"{stem}.tiles_{address:04X}.png").read_bytes() == (first / pictures[address]).read_bytes()
        text = (more / f"{stem}.text.txt").read_text(encoding="utf-8")
        lines = [line for line in text.splitlines() if line.startswith(("+", "# the greeting"))]
        assert lines == [f"+{0x0240 - whole['offset']:04X} 6 |HOWDY@|", "# the greeting at the console",
                         f"+{0x0600 - whole['offset']:04X} 9 |FINE DAY@|",
                         f"+{0x0610 - whole['offset']:04X} 8 |BYE@|",
                         f"+{0x0700 - whole['offset']:04X} 8 |<8C>IDDLE @|",
                         f"+{0x0720 - whole['offset']:04X} 8 |GOODBYE@|"], lines
        assert whole["file"] in text.splitlines()[0] and text.count("# The strings of") == 1, text
        assert text.endswith("|GOODBYE@|\n# the end of the talk\n"), text
        assert "seen" not in json.dumps(manifest_of(more)) and "readers" not in manifest_of(more)
        assert "not kept:" in said and "what `uses` wrote" in said.split("not kept:")[1], said
        assert "5 bytes that are data in the new tree were in files of code" in said, said
        assert "Run `uses` again with the records" in said, said
        assert "1 file of text with 5 strings (4 edited)" in said and "4 pictures of tiles (2 edited)" in said, said
        assert "of the old tree has to be looked at" in said, said
        assert gb_data("status", more).count("edited: ") == 9
        # The tools go on from there: the text is not as `text` wrote it, a picture is drawn again.
        refused = gb_data("text", more, expect=2)
        assert "nothing says yet what the game reads" in refused, refused
        gb_data("uses", more, record)
        assert "has been edited" in gb_data("text", more, expect=2)
        refused = gb_data("tiles", more, whole["file"], "--offset", str(FONT - whole["offset"]), "--tiles", "40",
                          "--bpp", "1", expect=2)
        assert "has been edited: --force draws it again" in refused, refused

        # --- And back: the routine is code again, and one file two. ---
        again = root / "again"
        said = kept("alone", more, again)
        assert views_of(again) == views_of(first)
        for name, (kind, _) in views_of(first).items():
            assert (again / name).read_bytes() == (first / name).read_bytes(), name
        # What `uses` wrote, carried: what it writes itself into a fresh tree of that export.
        fresh = root / "fresh"
        split("alone", fresh)
        gb_data("uses", fresh, record)
        assert "what `uses` wrote" in said.split("not kept:")[0], said
        assert manifest_of(again)["readers"] == manifest_of(fresh)["readers"]
        assert [region.get("seen") for region in manifest_of(again)["regions"]] == \
            [region.get("seen") for region in manifest_of(fresh)["regions"]]
        assert sum(reader["bytes"] for reader in manifest_of(again)["readers"]) == \
            sum(reader["bytes"] for reader in manifest_of(more)["readers"]) - 5
        assert manifest_of(again) == manifest_of(first)
        # From a tree whose data is only cut in two, the runs that were cut are one again.
        two = root / "two"
        split("in two", two)
        gb_data("uses", two, record)
        halves = [region["seen"] for region in manifest_of(two)["regions"] if region["offset"] in (TILES, 0x5000)]
        assert [(run["offset"], run["size"]) for runs in halves for run in runs] == [(0x0FF0, 0x10), (0, 0x10)]
        one = root / "one"
        kept("alone", two, one)
        assert manifest_of(one) == manifest_of(fresh)

        # An edit where the new tree has code: the routine's bytes, which only the told tree has
        # as data. It stops, names the place, and writes nothing; told to, it goes on without it.
        poke(more, SUB + 1, 0x77)
        poke(more, SUB + 3, 0x66)
        assert built(more)[0][SUB:SUB + 5] == bytes([0x3E, 0x77, 0xE0, 0x66, 0xC9])
        lost = root / "lost"
        refused = split("alone", lost, "--keep", more, expect=2)
        assert f"2 edited bytes of {whole['file']}, from 00:{SUB + 1:04X} on: they are code in the new tree" in \
            refused and "--lose-edits-in-code" in refused and not lost.exists(), refused
        before = files_of(more)
        said = kept("alone", more, lost, "--lose-edits-in-code")
        assert files_of(more) == before
        assert f"from 00:{SUB + 1:04X} on" in said.split("not kept:")[1] and "The edit is lost" in said, said
        assert "but for the edits that were lost" in said, said
        assert built(lost) == (image, pack), "more than the edit to code was lost, or less"
        # In a tree told as much, the edit is carried as any other.
        told_again = root / "told-again"
        kept("told", more, told_again)
        assert built(told_again)[0][SUB:SUB + 5] == bytes([0x3E, 0x77, 0xE0, 0x66, 0xC9])

        # --- Cut by hand, through the middle of things. ---
        by_hand = root / "by-hand"
        said = kept("by hand", first, by_hand)
        left = said.split("not kept:")[1]
        found = views_of(by_hand)
        assert sorted(found.values()) == [("text", 0x0240), ("text", 0x0600), ("tiles", FONT),
                                         ("tiles", TILES + 0x800)], found
        # The picture that is all code now: left behind, and nothing was painted in it.
        assert f"{pictures[SPARE]}, the picture of 2 tiles at 00:{SPARE:04X}: its bytes are code or the header in " \
               f"the new tree\n" in left, left
        moved = next(name for name, where in found.items() if where == ("tiles", TILES + 0x800))
        assert moved == "bank_01/47C0_unknown.tiles_4800.png" and \
            (by_hand / moved).read_bytes() == (first / pictures[TILES + 0x800]).read_bytes(), moved
        view = next(view for view in region_at(by_hand, 0x4800)["views"])
        assert (view["offset"], view["tiles"], view["width"]) == (0x40, 8, 4), view
        # The picture in two files: left behind and named, and what was painted is in the bytes.
        assert f"{pictures[TILES]}, the picture of 16 tiles at 01:4000: its bytes lie in 2 files of the new tree" \
            in left and "What was painted in it is in the bytes of the new files" in left, left
        assert not (by_hand / pictures[TILES]).exists()
        assert (by_hand / region_at(by_hand, TILES)["file"]).read_bytes()[:16] == bytes([0xFF] * 16)
        # The strings: one partly code and never changed, one in two files and changed.
        assert f"2 strings of {texts[0x0600]}, the first at 00:0700: 2 of its bytes are code or the header in the " \
               f"new tree. What 1 of them was changed to is in the bytes of the new files" in left, left
        text = (by_hand / next(name for name, where in found.items() if where == ("text", 0x0600))).read_text(
            encoding="utf-8")
        assert [line.split(" ", 1)[1] for line in text.splitlines() if line.startswith("+")] == \
            ["9 |FINE DAY@|", "8 |BYE@|"] and "# the greeting at the console" in text, text
        # The comment that came after the string left behind is after the last one that was kept.
        assert text.endswith("|BYE@|\n# the end of the talk\n"), text
        assert (by_hand / region_at(by_hand, 0x0720)["file"]).read_bytes()[0x0720 - 0x0704:] == spell("GOOD")
        assert (by_hand / region_at(by_hand, 0x0724)["file"]).read_bytes()[:4] == spell("BYE@")
        # The strings that are carried are still text to edit: the bytes under them are the old file's.
        assert (by_hand / region_at(by_hand, 0x0600)["file"]).read_bytes()[0x0600 - back["offset"]:][:9] == \
            spell("GOOD DAY@")
        assert "what `uses` wrote" in said.split("not kept:")[0], said     # no byte of data that was not data
        # Five files now (two that were, and three that hold what the picture and the string said),
        # one picture, three strings, one thing unpacked.
        status = gb_data("status", by_hand)
        assert status.count("edited: ") == 10 and "2 pictures of tiles and 2 files of text" in status, status

        # --- What a later gb_data.py adds, and this step has no rule for. ---
        later = load_tool(later=True)

        def later_split(name: str, tree: Path, old: Path) -> str:
            import contextlib
            import io
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                code = later.command_split(argparse.Namespace(
                    export=exports[name], rom=rom_path, output=tree, force=False, keep=old,
                    lose_edits_in_code=False, discard_edits=False))
            assert code == 0
            return printed.getvalue()

        def later_built(tree: Path) -> bytes:
            _, loaded = later.load_tree(tree)
            contents, _, disagree = later.with_views(tree, loaded)
            assert not disagree, disagree
            return b"".join(contents)

        odd = root / "odd"
        shutil.copytree(first, odd)
        document = manifest_of(odd)
        document["notes"] = {"who": "somebody"}
        for region in document["regions"]:
            if region["offset"] == bank["offset"]:
                # The file given a name of its own, and its two pictures made views of another kind.
                (odd / region["file"]).rename(odd / "bank_01/tileset.bin")
                region["file"], region["name"] = "bank_01/tileset.bin", "Tileset"
                for view in region["views"]:
                    view["kind"] = "marks"
        (odd / "manifest.json").write_text(json.dumps(document), encoding="utf-8")
        odd_image = later_built(odd)
        assert odd_image == image
        before = files_of(odd)
        carried = root / "odd-carried"
        said = later_split("alone", carried, odd)
        assert files_of(odd) == before
        assert manifest_of(carried) == manifest_of(odd) and later_built(carried) == image
        assert (carried / "bank_01/tileset.bin").read_bytes() == (odd / "bank_01/tileset.bin").read_bytes()
        assert "the names of 1 file that had been given one" in said and "'notes' of the manifest, as it is" in said
        assert "2 views of other kinds, where they were" in said and "not kept:\n  1 file of" in said, said
        dropped = root / "odd-dropped"
        said = later_split("by hand", dropped, odd)
        left = said.split("not kept:")[1]
        assert later_built(dropped) == image and "notes" not in manifest_of(dropped)
        assert not any("name" in region for region in manifest_of(dropped)["regions"])
        assert "the name of bank_01/tileset.bin: its bytes (01:4000, 16384) are cut another way" in left, left
        assert "'name' of bank_01/tileset.bin, which this gb_data.py does not know how to move" in left, left
        assert "'notes' of the manifest, which this gb_data.py does not know" in left, left
        assert left.count("a view of kind 'marks' of bank_01/tileset.bin") == 2 and \
            "An edit made through it is in the bytes of the new files" in left, left
        assert sorted(views_of(dropped).values()) == [("text", 0x0240), ("text", 0x0600), ("tiles", FONT)]

        # --- Text nobody changed, over a file that was changed under it. ---
        # The old tree lets the file stand; the new tree's text is written from the file, and is
        # as `text` writes it.
        stale = root / "stale"
        split("alone", stale)
        gb_data("uses", stale, record)
        (stale / "charmap.json").write_text(charmap, encoding="utf-8")
        gb_data("text", stale)
        poke(stale, 0x0700, spell("R")[0])
        assert built(stale)[0][0x0700:0x0708] == spell("RIDDLE @")
        assert "|MIDDLE @|" in (stale / texts[0x0600]).read_text(encoding="utf-8")
        stood = root / "stood"
        kept("told", stale, stood)
        name, view = next((view["file"], view) for view in region_at(stood, SUB)["views"])
        assert "|RIDDLE @|" in (stood / name).read_text(encoding="utf-8")
        assert hashlib.sha256((stood / name).read_bytes()).hexdigest() == view["sha256"]
        assert gb_data("status", stood).count("edited: ") == 1
        # Without its charmap the text cannot be written again, and is named. It had changed nothing.
        (stale / "charmap.json").unlink()
        said = kept("told", stale, root / "no-charmap")
        assert said.count("cannot read the charmap") == 2 and f"{texts[0x0240]}, the text of {front['file']}: " in \
            said.split("not kept:")[1] and not views_of(root / "no-charmap"), said

        # --- Two files of text with a charmap each, whose strings are in one file now. ---
        clash = root / "clash"
        shutil.copytree(first, clash)
        shutil.copy(clash / "charmap.json", clash / "other.json")
        document = manifest_of(clash)
        next(view for view in region_at(clash, SPARE)["views"] if view["kind"] == "text")
        for region in document["regions"]:
            for view in region.get("views", ()):
                if view["kind"] == "text" and region["offset"] == front["offset"]:
                    view["charmap"] = "other.json"
        (clash / "manifest.json").write_text(json.dumps(document), encoding="utf-8")
        said = kept("told", clash, root / "clashed")
        left = said.split("not kept:")[1]
        assert f"4 strings of {texts[0x0600]}, the first at 00:0600: the text of {whole['file']} is written with " \
               f"another charmap, other.json, and a file of text has one. What 3 of them were changed to is in the " \
               f"bytes of the new files" in left, left
        assert "notes.txt, other.json, copied as they are" in said, said
        assert sorted(views_of(root / "clashed").values())[0] == ("text", 0x0240)

        # --- What is refused. ---
        assert "is a tree of another ROM" in gb_data(
            "split", exports["alone"], "--rom", rom_path, "-o", root / "no", "--keep",
            _other_tree(args.gbrecomp, root, analysis), expect=2)
        for inside in (first, first / "new", first.parent):
            refused = split("alone", inside, "--keep", first, "--force", "--discard-edits", expect=2)
            assert "cannot be one folder or one inside the other" in refused, refused
        assert "already holds a data tree" in split("told", same, "--keep", first, expect=2)
        broken = root / "broken"
        shutil.copytree(first, broken)
        poke(broken, TILES + 3, 0x00)       # the file and its picture now both say something of the first tile
        refused = split("told", root / "no", "--keep", broken, expect=2)
        assert "have both been edited" in refused and "before splitting again" in refused, refused
        shutil.rmtree(broken)
        shutil.copytree(first, broken)
        poke(broken, START, 0x00)
        assert "code the game's C was compiled from" in split("told", root / "no", "--keep", broken, expect=2)
        assert not (root / "no").exists()

        # --- `split --force` and a tree that has edits. ---
        for name, words in (("alone", "has edits"), ("told", "has edits")):
            before = files_of(same)
            refused = split(name, same, "--force", expect=2)
            assert words in refused and "9 more" not in refused and "and 8 more" in refused, refused
            assert "--keep" in refused and "--discard-edits" in refused and files_of(same) == before, refused
        only_unpacked = root / "only-unpacked"      # an edit that is in no file of the ROM is an edit too
        split("alone", only_unpacked)
        shutil.copy(first / "unpackers.json", only_unpacked / "unpackers.json")
        gb_data("unpacked", only_unpacked, harvest)
        paint(only_unpacked, "unpacked/pictures/01_5100.png", bytes([0xF0] * 32), 2, 2)
        assert "has edits (unpacked/pictures/01_5100.png" in split("alone", only_unpacked, "--force", expect=2)
        (same / "manifest.json").write_text("{}", encoding="utf-8")
        refused = split("alone", same, "--force", expect=2)
        assert "cannot be read" in refused and "nothing says whether it has edits" in refused, refused
        said = split("alone", same, "--force", "--discard-edits")
        assert "wrote" in said and "the tree that was there" not in said, said
        assert built(same)[0] == rom and "views" not in json.dumps(manifest_of(same))
        # With no edits it is written over as before, and what else it held is said.
        said = split("alone", fresh, "--force")
        assert "the tree that was there also held what `uses` wrote" in said, said
        gb_data("tiles", fresh, bank["file"], "--tiles", "4")
        said = split("told", fresh, "--force")
        assert "also held 1 view (pictures and text): the new one does not" in said, said
        assert "the tree that was there" not in split("told", fresh, "--force")

    print("data keep: ok")
    return 0


def _other_tree(gbrecomp: Path, root: Path, analysis: list[str]) -> Path:
    """A tree of another ROM: this one with another title."""
    rom = bytearray(make_rom())
    rom[0x134:0x13D] = b"OTHER ROM"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    (root / "other.gb").write_bytes(rom)
    run([gbrecomp, root / "other.gb", *analysis, "--lift-export", root / "other.json"])
    gb_data("split", root / "other.json", "--rom", root / "other.gb", "-o", root / "other")
    return root / "other"


if __name__ == "__main__":
    sys.exit(main())
