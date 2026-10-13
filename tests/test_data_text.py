#!/usr/bin/env python3
"""A game's text as text, found by who reads it, edited and seen in the game.

Builds a small game with a font, a routine that prints, two strings it
prints and one it never does, and a map drawn from blocks of four tiles by
four instructions that each read every fourth byte. Then:

- a run with `--data-use`, and `gb_data.py uses`, have to leave the manifest
  saying enough for `glyphs` and `text` to find the instruction that prints,
  and not the four that draw the map, though every byte those read reaches
  the tile map too;
- `glyphs` has to draw the tile each byte draws out of a save state, the
  bytes the strings use dark and the rest faint, and write a charmap that
  lists those bytes with no letters;
- `text` has to write the two strings that were printed, a byte the charmap
  has no letter for as `<XX>`, and the tree unedited has to be the ROM still;
- a string edited to the same length, and one made shorter and ended, have
  to be in the image `build` makes and on the screen of the game run with it;
- a string that grew, one with a letter nothing draws, one cut short, a line
  that is gone and a text and a file that disagree have to be refused, each
  by name; text only saved again, with other line ends or after the charmap
  learnt a letter, has to change nothing;
- a picture and text of the same region have to keep out of each other's
  bytes.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"
sys.path.insert(0, str(ROOT / "tools"))
import gb_data  # noqa: E402  (its PNG reader)
import gb_data_text  # noqa: E402
import gb_discover  # noqa: E402  (the save state reader)

START = 0x0150
PRINT = 0x0300
STRING1, STRING3, STRING2, BLOCKS, FONT = 0x0400, 0x0414, 0x0420, 0x0480, 0x0500
LINE, END, SPACE, ODD = 0x4F, 0x50, 0x7F, 0x60
FRAMES = 20


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def letters_of(text: str) -> bytes:
    """A to P are the bytes 80 to 8F, as the font has them."""
    return bytes(SPACE if letter == " " else 0x80 + ord(letter) - ord("A") for letter in text)


TEXT1 = letters_of("BAD CAFE") + bytes([LINE]) + letters_of("A FAB BEAD") + bytes([END])
TEXT2 = letters_of("DEAF") + bytes([ODD]) + letters_of("CAB") + bytes([END])
TEXT3 = letters_of("FEED") + bytes([END])
# Two more the game never prints, away from every other string: one long enough to be taken for a string by
# itself, and one that is not.
ALONE, SHORT = 0x0440, 0x0460
TEXT4 = letters_of("DEAD BEEF") + bytes([END])
TEXT5 = letters_of("AB") + bytes([END])


def make_rom() -> tuple[bytearray, dict[str, int]]:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"DATA TEXT"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    readers: dict[str, int] = {"print": PRINT}
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF
    # The screen off while video memory is written, so that every write lands:
    # wait: ldh a,[$44] / cp 144 / jr nz,wait / xor a / ldh [$40],a / ld a,$E4 / ldh [$47],a
    code += [0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xAF, 0xE0, 0x40, 0x3E, 0xE4, 0xE0, 0x47]
    # The font, to where the tiles of bytes 80 to 8F are:
    # ld hl,Font / ld de,$8800 / ld b,0 / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop
    code += [0x21, *word(FONT), 0x11, *word(0x8800), 0x06, 0x00, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]
    code += [0x21, *word(STRING1), 0x11, *word(0x9800), 0xCD, *word(PRINT)]
    code += [0x21, *word(STRING2), 0x11, *word(0x9880), 0xCD, *word(PRINT)]
    # A map of sixteen blocks, four tiles across each, one under another:
    # ld hl,Blocks / ld de,$9900 / ld b,16
    # row: four times (ld a,[hli] / ld [de],a / inc de) / ld a,e / add a,28 / ld e,a / jr nc,+1 / inc d
    #      dec b / jr nz,row
    code += [0x21, *word(BLOCKS), 0x11, *word(0x9900), 0x06, 0x10]
    for column in range(4):
        readers[f"block{column}"] = START + len(code)
        code += [0x2A, 0x12, 0x13]
    code += [0x7B, 0xC6, 0x1C, 0x5F, 0x30, 0x01, 0x14, 0x05, 0x20, 0xEA]
    code += [0x3E, 0x91, 0xE0, 0x40, 0x18, 0xFE]                           # ld a,$91 / ldh [$40],a / jr to itself
    assert START + len(code) <= PRINT
    rom[START:START + len(code)] = bytes(code)
    rom[PRINT:PRINT + 20] = bytes([
        0x2A,                   # Print: ld a,[hli]      the instruction that reads every string
        0xFE, END, 0xC8,        #        cp End / ret z
        0xFE, LINE, 0x28, 0x04,  # cp Line / jr z,line
        0x12, 0x13, 0x18, 0xF4,  # ld [de],a / inc de / jr Print
        0x7B, 0xE6, 0xE0, 0xC6, 0x20, 0x5F,   # line: ld a,e / and $E0 / add a,$20 / ld e,a
        0x18, 0xEC])            #        jr Print
    rom[STRING1:STRING1 + len(TEXT1)] = TEXT1
    rom[STRING3:STRING3 + len(TEXT3)] = TEXT3
    rom[ALONE:ALONE + len(TEXT4)] = TEXT4
    rom[SHORT:SHORT + len(TEXT5)] = TEXT5
    rom[STRING2:STRING2 + len(TEXT2)] = TEXT2
    rom[BLOCKS:BLOCKS + 64] = bytes(0x80 + index % 16 for index in range(64))
    for tile in range(16):                  # A and P all of colour 3, the rest each its own pattern
        rom[FONT + tile * 16:FONT + tile * 16 + 16] = bytes([0xFF] * 16) if tile in (0, 15) else \
            bytes((tile * 37 + row * 11) & 0xFF for row in range(16))
    return rom, readers


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def tool(*arguments: object, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_DATA), *map(str, arguments)], expect=expect)


def play(executable: Path, root: Path, name: str, *flags: str) -> gb_discover.Savestate:
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    state = root / f"{name}.state"
    run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES), "--save-dir", str(saves),
         "--save-state-file", str(state), *flags], timeout=180)
    return gb_discover.Savestate.load(state)


def row(state: gb_discover.Savestate, address: int, count: int) -> bytes:
    return bytes(state.read(address + index) for index in range(count))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]

    with tempfile.TemporaryDirectory(prefix="gbtext-") as scratch:
        root = Path(scratch)
        rom, readers = make_rom()
        rom_path = root / "text.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        run([str(args.gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "text_game"])
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
             "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
        run(["ninja", "-C", str(project / "build")])
        executable = next(path for path in (project / "build" / "text_game.exe", project / "build" / "text_game")
                          if path.exists())

        # The game prints its two strings, and draws its map.
        record_path = root / "use.json"
        state_path = root / "recorded.state"
        state = play(executable, root, "recorded", "--data-use", str(record_path))
        assert row(state, 0x9800, 8) == letters_of("BAD CAFE"), row(state, 0x9800, 8).hex()
        assert row(state, 0x9820, 10) == letters_of("A FAB BEAD")
        assert row(state, 0x9880, 8) == letters_of("DEAF") + bytes([ODD]) + letters_of("CAB")
        assert row(state, 0x9900, 4) == bytes([0x80, 0x81, 0x82, 0x83]) and state.read(0x9AE3) == 0x8F

        tree = root / "tree"
        tool("split", export_path, "--rom", rom_path, "-o", tree)
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        region = next(region for region in manifest["regions"]
                      if region["offset"] <= STRING1 < region["offset"] + region["size"])
        assert region["kind"] == "data" and region["offset"] + region["size"] >= FONT + 256, region
        base = region["offset"]
        name = region["file"]
        text_name = name[:-len(".bin")] + ".text.txt"
        text_path = tree / text_name

        # Nothing to go by before the game has been seen to read anything.
        assert "nothing says yet what the game reads" in tool("text", tree, expect=(2,)).stderr
        tool("uses", tree, record_path)
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        by_reader = {(entry["bank"], entry["address"]): entry for entry in manifest["readers"]}
        assert by_reader[(0, PRINT)] == {"bank": 0, "address": PRINT, "bytes": 29,
                                         "uses": {"tile_map": 26, "nothing": 3}}, by_reader[(0, PRINT)]
        for column in range(4):         # all of them drawn, and none of them text
            assert by_reader[(0, readers[f"block{column}"])]["uses"] == {"tile_map": 16}

        # And none to write it in before somebody has read the letters.
        stderr = tool("text", tree, expect=(2,)).stderr
        assert "cannot read the charmap" in stderr and "`glyphs` draws the letters" in stderr, stderr

        # The letters, as pictures out of a save state.
        assert "cannot read the save state" in tool("glyphs", tree, rom_path, expect=(2,)).stderr
        assert not (tree / "charmap.json").exists()
        said = tool("glyphs", tree, state_path).stdout
        used = set(TEXT1) | set(TEXT2)
        assert f"The {len(used)} bytes the strings use are drawn dark" in said, said
        charmap = json.loads((tree / "charmap.json").read_text(encoding="utf-8"))
        assert charmap == {"schema": "gbrecomp.charmap", "version": 1,
                           "letters": {f"{byte:02X}": "" for byte in sorted(used)}}, charmap
        width, height, grey = gb_data.read_png("glyphs.png", (tree / "glyphs.png").read_bytes())
        assert (width, height) == (415, 415), (width, height)

        def cell(byte: int) -> int:     # a pixel inside the square of the tile that byte draws
            return grey[(14 + (byte >> 4) * 25 + 2) * width + 14 + (byte & 15) * 25 + 2]

        # A is black and in a string; P is as black and in none; a space draws nothing.
        assert (cell(0x80), cell(0x8F), cell(SPACE)) == (0, 200, 255), (cell(0x80), cell(0x8F), cell(SPACE))

        # Somebody reads the sheet. The odd byte is left without a letter.
        letters = {f"{0x80 + index:02X}": chr(ord("A") + index) for index in range(16)}
        letters.update({f"{SPACE:02X}": " ", f"{LINE:02X}": "{line}", f"{END:02X}": "{end}", f"{ODD:02X}": ""})

        def write_charmap() -> None:
            (tree / "charmap.json").write_text(json.dumps(
                {"schema": "gbrecomp.charmap", "version": 1, "letters": letters}), encoding="utf-8")

        write_charmap()
        # A string in bytes a picture has is left to the picture: two views of the same bytes could disagree.
        tool("tiles", tree, name, "--offset", STRING3 - base, "--tiles", 1)
        said = tool("text", tree).stdout
        assert f"1 string, 20 bytes, in 1 region, read at 00:{PRINT:04X}:" in said, said
        assert "1 more lies in bytes a picture has, and is left to it" in said, said
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        entry = next(entry for entry in manifest["regions"] if entry["file"] == name)
        picture = next(view for view in entry["views"] if view["kind"] == "tiles")
        entry["views"].remove(picture)
        (tree / picture["file"]).unlink()
        (tree / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        said = tool("text", tree).stdout
        assert f"2 strings, 29 bytes, in 1 region, read at 00:{PRINT:04X}:" in said, said
        assert "1 of the bytes have no letter in charmap.json and are written <XX>: <60> 1" in said, said
        line1 = f"+{STRING1 - base:04X} 20 |BAD CAFE{{line}}A FAB BEAD{{end}}|"
        line2 = f"+{STRING2 - base:04X} 9 |DEAF<60>CAB{{end}}|"
        written = text_path.read_text(encoding="utf-8")
        assert [line for line in written.splitlines() if line.startswith("+")] == [line1, line2], written
        assert "FEED" not in written                    # never printed, so not known to be text
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        views = next(entry for entry in manifest["regions"] if entry["file"] == name)["views"]
        assert len(views) == 1 and views[0]["kind"] == "text" and views[0]["file"] == text_name, views
        assert views[0]["strings"] == [[STRING1 - base, 20], [STRING2 - base, 9]], views[0]["strings"]
        said = tool("status", tree).stdout
        assert "and 1 file of text" in said and "nothing edited" in said, said
        original = root / "original.gb"
        tool("build", tree, "-o", original, "--expect-original")
        assert original.read_bytes() == bytes(rom)
        # Text nobody touched is not even read: the tree is the ROM with its charmap gone.
        (tree / "charmap.json").unlink()
        tool("build", tree, "-o", original, "--expect-original")
        write_charmap()
        # Naming the instruction says the same as finding it.
        again = tool("text", tree, "--reader", f"00:{PRINT:04X}").stdout
        assert f"2 strings, 29 bytes, in 1 region, read at 00:{PRINT:04X}:" in again, again
        assert "takes BANK:ADDRESS" in tool("text", tree, "--reader", "print", expect=(2,)).stderr

        def edit(*changes: tuple[str, str]) -> None:
            text = written
            for old, new in changes:
                assert text.count(old) == 1, (old, text)
                text = text.replace(old, new)
            text_path.write_text(text, encoding="utf-8", newline="\n")

        def refused(words: str) -> None:
            stderr = tool("build", tree, "-o", root / "no.gb", expect=(2,)).stderr
            assert words in stderr, (words, stderr)
            assert not (root / "no.gb").exists()

        # One string to the same length, one shorter and ended as it was.
        edit(("BAD CAFE", "BAD FACE"), ("DEAF<60>CAB{end}", "ACE{end}"))
        said = tool("status", tree).stdout
        assert f"edited: {text_name} (00:{STRING1:04X}) |BAD FACE{{line}}A FAB BEAD{{end}}|" in said, said
        assert f"edited: {text_name} (00:{STRING2:04X}) |ACE{{end}}|" in said and "<-" not in said, said
        image_path = root / "edited.gb"
        said = tool("build", tree, "-o", image_path).stdout
        assert f"1 of {len(manifest['regions']) + 1} files edited" in said, said
        image = image_path.read_bytes()
        expected = bytearray(rom)
        expected[STRING1:STRING1 + 8] = letters_of("BAD FACE")
        expected[STRING2:STRING2 + 4] = letters_of("ACE") + bytes([END])
        assert image == bytes(expected), [index for index in range(len(rom)) if image[index] != expected[index]][:8]
        assert (tree / name).read_bytes() == bytes(rom[base:base + region["size"]])    # the file is not written to
        state = play(executable, root, "edited", "--rom-image", str(image_path))
        assert row(state, 0x9800, 8) == letters_of("BAD FACE"), row(state, 0x9800, 8).hex()
        assert row(state, 0x9820, 10) == letters_of("A FAB BEAD")
        assert row(state, 0x9880, 8) == letters_of("ACE") + bytes(5), row(state, 0x9880, 8).hex()

        # Shorter and not ended: built, and said.
        edit(("DEAF<60>CAB{end}", "ACE"))
        said = tool("build", tree, "-o", root / "unended.gb").stdout
        assert "<- shorter, and it does not end with {end} as the old one did" in said, said
        assert (root / "unended.gb").read_bytes()[STRING2:STRING2 + 9] == letters_of("ACEF") + TEXT2[4:]

        # What cannot be written back.
        edit(("DEAF<60>CAB{end}", "DEAF<60>CABBAGE{end}"))
        refused(f"the string at +{STRING2 - base:04X} is 13 bytes now and there is room for 9: a string cannot grow")
        edit(("BAD CAFE", "BAD CAFZ"))
        refused("nothing in the charmap draws 'Z'")
        edit(("DEAF<60>CAB{end}", "DEAF"))
        refused("was cut short and not ended")
        edit((line2 + "\n", ""))
        refused(f"the line of the string at +{STRING2 - base:04X} is gone")
        edit((line2, line2 + "\n+0001 2 |AB|"))
        refused("there is no string at +0001")
        edit((line2, line2.replace(" 9 |", " 8 |")))
        refused("is 9 bytes where it lies, not 8")
        edit((line2, "DEAF"))
        refused("not '+OFFSET SIZE |text|'")
        text_path.unlink()
        refused(f"cannot read {text_name}")

        # Saved again and saying the same: with other line ends and a mark in front, nothing is edited.
        text_path.write_bytes(b"\xef\xbb\xbf" + written.replace("\n", "\r\n").encode("utf-8"))
        assert "nothing edited" in tool("status", tree).stdout
        tool("build", tree, "-o", root / "resaved.gb", "--expect-original")
        # Nor when the charmap has learnt the odd byte's letter since; and the letter can then be written.
        letters[f"{ODD:02X}"] = "~"
        write_charmap()
        tool("build", tree, "-o", root / "learnt.gb", "--expect-original")
        edit(("DEAF<60>CAB{end}", "DEAF~BAD{end}"))
        tool("build", tree, "-o", root / "learnt.gb")
        assert (root / "learnt.gb").read_bytes()[STRING2:STRING2 + 9] == \
            letters_of("DEAF") + bytes([ODD]) + letters_of("BAD") + bytes([END])

        # What ends a string: a byte that ends two or more, and ends one more often than it stands inside one.
        made_up = [letters_of("BAD") + bytes([END]), letters_of("CAFE") + bytes([END]), letters_of("FEE"),
                   letters_of("ACE"), letters_of("EEL") + bytes([LINE])]
        body, enders, words = gb_data_text.what_strings_are(made_up, {int(key, 16): letter
                                                                      for key, letter in letters.items() if letter})
        assert enders == {END}, enders              # E ends two and stands inside four; a line break ends one
        assert 0x84 in body and LINE in body and END not in body and SPACE in words and LINE not in words

        # The strings nobody saw printed, found by reading. FEED is made of letters the others have, ends as
        # they end, and lies right after one of them. DEAD BEEF lies by itself and has letters enough. AB lies
        # by itself and has not. The blocks of the map are letters too, and end with nothing: they are left
        # alone, and so is the font, which the game was seen to draw. Asked for again without --read, they are
        # gone again.
        text_path.write_text(written, encoding="utf-8", newline="\n")
        letters.pop(f"{ODD:02X}")
        write_charmap()
        said = tool("text", tree, "--read").stdout
        assert f"4 strings, 44 bytes, in 1 region, read at 00:{PRINT:04X}:" in said, said
        assert "2 of them, 15 bytes, nobody saw printed" in said, said
        view = next(entry for entry in json.loads((tree / "manifest.json").read_text(encoding="utf-8"))["regions"]
                    if entry["file"] == name)["views"][0]
        assert view["strings"] == [[STRING1 - base, 20], [STRING3 - base, 5], [STRING2 - base, 9],
                                   [ALONE - base, 10]], view["strings"]
        assert view["read"] == [[STRING3 - base, 5], [ALONE - base, 10]], view
        read_text = text_path.read_text(encoding="utf-8")
        assert f"+{STRING3 - base:04X} 5 |FEED{{end}}|" in read_text, read_text
        assert f"+{ALONE - base:04X} 10 |DEAD BEEF{{end}}|" in read_text and "|AB{end}|" not in read_text, read_text
        tool("build", tree, "-o", root / "read.gb", "--expect-original")
        text_path.write_text(read_text.replace("|FEED{end}|", "|FADE{end}|"), encoding="utf-8", newline="\n")
        tool("build", tree, "-o", root / "read.gb")
        expected = bytearray(rom)
        expected[STRING3:STRING3 + 4] = letters_of("FADE")
        assert (root / "read.gb").read_bytes() == bytes(expected), "the string that was read did not build"
        assert "has been edited: --force writes it again" in tool("text", tree, "--read", expect=(2,)).stderr
        text_path.write_text(read_text, encoding="utf-8", newline="\n")
        said = tool("text", tree).stdout
        assert f"2 strings, 29 bytes, in 1 region, read at 00:{PRINT:04X}:" in said and "nobody saw" not in said, said
        assert text_path.read_text(encoding="utf-8") == written
        letters[f"{ODD:02X}"] = "~"                                         # and things as they were
        write_charmap()
        edit(("DEAF<60>CAB{end}", "DEAF~BAD{end}"))

        # The text and the file both edited. `text` does not write over an edit unasked.
        assert "has been edited: --force writes it again" in tool("text", tree, expect=(2,)).stderr
        data = bytearray((tree / name).read_bytes())
        data[STRING1 - base] = 0x85                                         # BAD -> FAD, in the file
        (tree / name).write_bytes(data)
        said = tool("status", tree).stdout
        assert "have both been edited, and disagree about 2 of the 2 strings" in said, said
        refused("have both been edited")
        # Text that was only saved again leaves the file's edit standing.
        text_path.write_bytes(written.replace("\n", "\r\n").encode("utf-8"))
        tool("build", tree, "-o", root / "file.gb")
        assert (root / "file.gb").read_bytes()[STRING1:STRING1 + 3] == letters_of("FAD")
        # And `text` again writes what the file says now.
        tool("text", tree)
        assert "|FAD CAFE{line}A FAB BEAD{end}|" in text_path.read_text(encoding="utf-8")
        data[STRING1 - base] = TEXT1[0]
        (tree / name).write_bytes(data)
        tool("text", tree)
        letters[f"{ODD:02X}"] = ""
        write_charmap()
        tool("text", tree)
        assert text_path.read_text(encoding="utf-8") == written

        # Two bytes spelt alike: a line nobody changed keeps the bytes it has, whichever they are.
        letters[f"{ODD:02X}"] = "A"
        write_charmap()
        tool("text", tree)
        alike = text_path.read_text(encoding="utf-8")
        assert "|DEAFACAB{end}|" in alike, alike
        text_path.write_text(alike.replace("BAD CAFE", "BAD FACE"), encoding="utf-8", newline="\n")
        tool("build", tree, "-o", root / "alike.gb")
        assert (root / "alike.gb").read_bytes()[STRING2:STRING2 + 9] == TEXT2
        # The line that was changed is written with the lowest byte that is spelt A, which is the odd one.
        assert (root / "alike.gb").read_bytes()[STRING1:STRING1 + 8] == \
            letters_of("BAD FACE").replace(b"\x80", bytes([ODD]))
        # A spelling that starts as another does, as a font with one tile for two letters has: the longest is taken.
        letters[f"{ODD:02X}"] = "AB"
        write_charmap()
        tool("text", tree, "--force")
        longer = text_path.read_text(encoding="utf-8")
        assert "|DEAFABCAB{end}|" in longer, longer
        text_path.write_text(longer.replace("DEAFABCAB{end}", "DEAFAB{end}"), encoding="utf-8", newline="\n")
        tool("build", tree, "-o", root / "longest.gb")
        assert (root / "longest.gb").read_bytes()[STRING2:STRING2 + 6] == letters_of("DEAF") + bytes([ODD, END])
        letters[f"{ODD:02X}"] = ""
        write_charmap()
        tool("text", tree, "--force")
        assert text_path.read_text(encoding="utf-8") == written

        # A picture and text of one region: the font drawn, and no tiles where the strings are.
        tool("tiles", tree, name, "--offset", FONT - base, "--tiles", 16)
        stderr = tool("tiles", tree, name, "--offset", STRING1 - base, "--tiles", 1, expect=(2,)).stderr
        assert "have strings in them" in stderr, stderr
        said = tool("status", tree).stdout
        assert "and 1 picture of tiles and 1 file of text" in said and "nothing edited" in said, said
        tool("text", tree, "--force")
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        views = next(entry for entry in manifest["regions"] if entry["file"] == name)["views"]
        assert [view["kind"] for view in views] == ["text", "tiles"], views
        tool("build", tree, "-o", root / "last.gb", "--expect-original")

        # A manifest that says something else of a text view is refused.
        good = (tree / "manifest.json").read_text(encoding="utf-8")
        at = next(index for index, entry in enumerate(manifest["regions"]) if entry["file"] == name)
        for change, words in (
                (lambda view: view.update(strings=[[STRING1 - base, 40000]]), "are not inside"),
                (lambda view: view.update(strings=[[STRING2 - base, 9], [STRING1 - base, 20]]), "one after another"),
                (lambda view: view.update(strings=[[FONT - base, 4]]), "another view has"),
                (lambda view: view.update(charmap="../charmap.json"), "is not a file inside"),
                (lambda view: view.update(strings="all"), "a field of the wrong type"),
                (lambda view: view.pop("data_sha256"), "lacks the field 'data_sha256'"),
                (lambda view: view.update(letters={}), "no such field as 'letters'")):
            altered = json.loads(good)
            change(altered["regions"][at]["views"][0])
            (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
            refused(words)
        (tree / "manifest.json").write_text(good, encoding="utf-8", newline="\n")
        tool("build", tree, "-o", root / "last.gb", "--expect-original")

    print("data text end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
