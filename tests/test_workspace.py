#!/usr/bin/env python3
"""From a ROM to a folder to mod it in, with one command, end to end.

Builds a small game that does its work in three goes, forty frames apart. At
once it loads a font and prints a string. Then it draws a tile that lies
where the analysis walks into it, a map out of blocks and a second string,
and unpacks a stream. Last it prints a third string and unpacks another. A
third stream is never unpacked, and lies behind a table of pointers. So what
`tools/gb_workspace.py` finds depends on how far the play it is given goes.

- With nothing but the ROM it has to build the game, record it from switching
  on, and leave a tree with the font as a picture, a sheet of the letters in
  place of text, the image that is the ROM, and a README that says what was
  found and what was not; every gbrecomp command has to have the options
  given, the export's like the game's. A build environment that is not there
  has to be said before anything is made.
- Run again it has to do nothing: no command, and no file of the tree touched.
- Each thing given later has to turn its steps on and no others: a charmap
  the text, save states the second pass (the game and the export made again
  with what the records show, and the tree split again with what it held),
  an unpackers declaration the pack, the harvest, the table's streams and the
  pictures, a symbol file the names, a recorded run its replay in pieces,
  and lines of one's own for the analysis another pass.
- An edit has to survive all of it: a pixel of a picture, a string, and a
  byte of a file, each in a region that the next split cuts otherwise. The
  image the tree builds has to be the same after every run, and the game
  started the way the README says has to show the edit.
- A run that stops, on a declaration nothing can be made of, has to say so,
  and the next has to go on from that step without doing the earlier ones
  again.
- A whole run in a fresh folder, given everything at once, has to make the
  tree the runs one after another made.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
WORKSPACE = ROOT / "tools" / "gb_workspace.py"
sys.path.insert(0, str(ROOT / "tools"))
import gb_data  # noqa: E402  (its PNG reader and writer)
import gb_discover  # noqa: E402  (the save state reader)

START = 0x0150
SHEET, TEXT1, SPARE = 0x0260, 0x0271, 0x02F0
PRINT, UNPACK, WAIT = 0x0300, 0x0314, 0x0323
TEXT2, EXTRA, TEXT3, BLOCKS, FONT, TABLE = 0x0400, 0x0440, 0x0450, 0x0480, 0x0500, 0x0700
S1, S2, S3 = 0x4000, 0x4010, 0x4020
BUFFER = 0xC100
LINE, END, SPACE = 0x4F, 0x50, 0x7F
APART = 40                      # frames between the game's three goes
BOOT, FROM_STATE = 15, 40       # frames recorded from switching on, and from a save state
# Sixteen bytes that are a tile to the game and sixteen one-byte instructions to the analysis.
TILE = bytes([0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x3C, 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x3D, 0x04, 0x0C])
OPTIONS = ["--no-scan", "--pointer-scan", "off"]


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def letters_of(text: str) -> bytes:
    """A to P are the bytes 80 to 8F, as the font has them."""
    return bytes(SPACE if letter == " " else 0x80 + ord(letter) - ord("A") for letter in text)


STRING1 = letters_of("BAD CAFE") + bytes([LINE]) + letters_of("A FAB BEAD") + bytes([END])
STRING2 = letters_of("DEAF CAB") + bytes([END])
STRING3 = letters_of("FEED") + bytes([END])
STREAMS = {S1: bytes([8, 0x55, 8, 0x66, 0]), S2: bytes([4, 0x12, 12, 0x34, 0]), S3: bytes([16, 0xE7, 0])}


def unpacked(stream: bytes) -> bytes:
    out = bytearray()
    for at in range(0, len(stream) - 1, 2):
        out += bytes([stream[at + 1]]) * stream[at]
    return bytes(out)


def copy(source: int, target: int, count: int) -> list[int]:
    """ld hl,source / ld de,target / ld b,count / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop"""
    return [0x21, *word(source), 0x11, *word(target), 0x06, count & 0xFF, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]


def make_rom() -> bytearray:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"WORKSPACE"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    screen_off = [0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xAF, 0xE0, 0x40]    # wait for line 144, then xor a / ldh [$40],a
    screen_on = [0x3E, 0x91, 0xE0, 0x40]                                   # ld a,$91 / ldh [$40],a
    wait = [0x06, APART, 0xCD, *word(WAIT)]                                # ld b,Apart / call Wait

    def never(target: int) -> list[int]:
        """ld a,1 / or a / jr nz,over / jp target: a jump the game never makes and the analysis follows."""
        return [0x3E, 0x01, 0xB7, 0x20, 0x03, 0xC3, *word(target)]

    def say(string: int, where: int) -> list[int]:
        return [0x21, *word(string), 0x11, *word(where), 0xCD, *word(PRINT)]

    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF
    # At once: the font to where the tiles of bytes 80 to 8F are, and the first string.
    code += screen_off + [0x3E, 0xE4, 0xE0, 0x47]                          # ld a,$E4 / ldh [$47],a
    code += copy(FONT, 0x8800, 256) + say(TEXT1, 0x9800) + screen_on + wait
    # Then: the tile the analysis took for code, a map of eight rows of four, a string, a stream.
    code += screen_off + never(SHEET) + copy(SHEET, 0x8000, 16) + never(EXTRA)
    code += [0x21, *word(BLOCKS), 0x11, *word(0x9900), 0x06, 0x08]
    code += [0x2A, 0x12, 0x13] * 4 + [0x7B, 0xC6, 0x1C, 0x5F, 0x30, 0x01, 0x14, 0x05, 0x20, 0xEA]
    code += say(TEXT2, 0x9880) + [0x21, *word(S1), 0xCD, *word(UNPACK)] + copy(BUFFER, 0x8400, 16)
    code += screen_on + wait
    # Last: another string and another stream, and then nothing more.
    code += screen_off + say(TEXT3, 0x98C0) + [0x21, *word(S2), 0xCD, *word(UNPACK)] + copy(BUFFER, 0x8410, 16)
    code += screen_on + [0x18, 0xFE]
    assert START + len(code) <= SHEET, len(code)
    rom[START:START + len(code)] = bytes(code)
    make_rom.code_end = START + len(code)
    rom[SHEET:SHEET + 17] = TILE + bytes([0xC9])                           # and a `ret` to the analysis
    rom[EXTRA:EXTRA + 5] = bytes([0x04, 0x0C, 0x14, 0x1C, 0xC9])           # which the game never so much as reads
    rom[PRINT:PRINT + 20] = bytes([
        0x2A,                           # Print: ld a,[hli]      the instruction that reads every string
        0xFE, END, 0xC8,                #        cp End / ret z
        0xFE, LINE, 0x28, 0x04,         #        cp Line / jr z,line
        0x12, 0x13, 0x18, 0xF4,         #        ld [de],a / inc de / jr Print
        0x7B, 0xE6, 0xE0, 0xC6, 0x20, 0x5F,   # line: ld a,e / and $E0 / add a,$20 / ld e,a
        0x18, 0xEC])                    #        jr Print
    rom[UNPACK:UNPACK + 15] = bytes([
        0x11, *word(BUFFER),            # Unpack: ld de,Buffer    the stream in HL: a count and a byte, to a count of 0
        0x2A, 0xB7, 0xC8,               # next:   ld a,[hli] / or a / ret z
        0x47, 0x2A,                     #         ld b,a / ld a,[hli]
        0x12, 0x13, 0x05, 0x20, 0xFB,   # fill:   ld [de],a / inc de / dec b / jr nz,fill
        0x18, 0xF4])                    #         jr next
    rom[WAIT:WAIT + 16] = bytes([
        0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA,     # Wait: ldh a,[$44] / cp 144 / jr nz,Wait
        0xF0, 0x44, 0xFE, 0x90, 0x28, 0xFA,     # past: ldh a,[$44] / cp 144 / jr z,past
        0x05, 0x20, 0xF1, 0xC9])                #       dec b / jr nz,Wait / ret
    rom[TEXT1:TEXT1 + len(STRING1)] = STRING1
    rom[TEXT2:TEXT2 + len(STRING2)] = STRING2
    rom[TEXT3:TEXT3 + len(STRING3)] = STRING3
    rom[BLOCKS:BLOCKS + 32] = bytes(0x80 + index % 16 for index in range(32))
    for tile in range(16):
        rom[FONT + tile * 16:FONT + tile * 16 + 16] = bytes((tile * 37 + row * 11) & 0xFF for row in range(16))
    rom[TABLE:TABLE + 6] = bytes([*word(S1), *word(S2), *word(S3)])
    for place, content in STREAMS.items():
        rom[place:place + len(content)] = content
    return rom


DECLARATION = {"schema": "gbrecomp.unpackers", "version": 1, "unpackers": [
    {"name": "tiles", "function": {"bank": 0, "address": UNPACK},
     "stream": {"bank": 1, "address": {"register": "hl"}},
     "made": [{"address": BUFFER, "size": 16}],
     "as": {"kind": "tiles", "bpp": 2, "width": 1},
     "tables": [{"bank": 0, "address": TABLE, "entries": 3, "stream_bank": 1}],
     "why": "the test's own routine"}]}
CHARMAP = {"schema": "gbrecomp.charmap", "version": 1,
           "letters": {"7F": " ", "4F": "{line}", "50": "{end}",
                       **{f"{0x80 + index:02X}": chr(ord("A") + index) for index in range(16)}}}
SYMBOLS = f"00:{FONT:04X} FontGfx\n00:{TEXT1:04X} TextOne\n01:{S1:04X} StreamOne\n00:{START:04X} Start\n"


def run(command: list, expect: tuple[int, ...] = (0,), timeout: int = 600, env: dict | None = None) -> str:
    result = subprocess.run([str(word) for word in command], capture_output=True, text=True, errors="replace",
                            timeout=timeout, env=env)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-4000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result.stdout + result.stderr


def commands(said: str) -> list[str]:
    """The commands a run of the tool printed."""
    return [line[2:] for line in said.splitlines() if line.startswith("$ ")]


def ran(said: str, *words: str) -> list[str]:
    return [command for command in commands(said) if all(word in command for word in words)]


def files_of(folder: Path) -> dict[str, str]:
    return {path.relative_to(folder).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(folder.rglob("*")) if path.is_file()}


def readme_command(directory: Path, title: str) -> list[str]:
    """A command the README gives, as its words."""
    lines = (directory / "README.txt").read_text(encoding="utf-8").splitlines()
    line = lines[lines.index(f"  {title}:") + 1].strip()
    words = [word.strip('"') for word in shlex.split(line, posix=False)] if os.name == "nt" else shlex.split(line)
    return [sys.executable if word in ("python", "python3") else word for word in words]


def readme_of(directory: Path) -> str:
    """What the README says, as one line: it is broken into lines wherever it gets long."""
    return " ".join((directory / "README.txt").read_text(encoding="utf-8").split())


def manifest_of(tree: Path) -> dict:
    return json.loads((tree / "manifest.json").read_text(encoding="utf-8"))


def region_at(manifest: dict, offset: int) -> dict:
    return next(region for region in manifest["regions"]
                if region["offset"] <= offset < region["offset"] + region["size"])


def view_at(manifest: dict, kind: str, offset: int) -> dict | None:
    """The view of a kind that starts at this place in the ROM."""
    region = region_at(manifest, offset)
    for view in region.get("views", ()):
        starts = {"tiles": lambda: view["offset"], "text": lambda: view["strings"][0][0],
                  "map": lambda: view["grids"][0][0]}[view["kind"]]()
        if view["kind"] == kind and region["offset"] + starts == offset:
            return view
    return None


def strings_of(tree: Path, manifest: dict, offset: int) -> str:
    """The file of text of the region this place is in."""
    region = region_at(manifest, offset)
    view = next(view for view in region.get("views", ()) if view["kind"] == "text")
    return (tree / view["file"]).read_text(encoding="utf-8")


def video(state: Path, address: int, count: int) -> bytes:
    machine = gb_discover.Savestate.load(state)
    return bytes(machine.read(address + index) for index in range(count))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    cmake = [f"--cmake-arg={word}" for word in args.cmake_arg]

    with tempfile.TemporaryDirectory(prefix="gbws-") as scratch:
        root = Path(scratch)
        rom = bytes(make_rom())
        rom_path = root / "work space.gb"               # a name with a space in it, as a person's has
        rom_path.write_bytes(rom)
        folder = root / "my game"
        tree = folder / "tree"
        saves = root / "saves"
        saves.mkdir()

        def ws(*arguments: object, expect: int = 0, env: dict | None = None, at: Path = folder) -> str:
            return run([sys.executable, WORKSPACE, rom_path, at, "--timeout", "120", *arguments], expect=(expect,),
                       env=env)

        def play(game: Path, name: str, frames: int, *flags: object) -> Path:
            state = root / f"{name}.state"
            run([game, "--headless", "--save-dir", saves, "--limit-frames", frames, "--save-state-file", state,
                 *flags], timeout=120)
            return state

        # Nothing is made where a game cannot be built, and the first line says what is missing.
        empty = root / "empty"
        empty.mkdir()
        said = ws("--gbrecomp", args.gbrecomp, expect=1, env={**os.environ, "PATH": str(empty), "CC": ""})
        assert "a game cannot be built here: cmake is not on PATH; ninja is not on PATH; no C compiler" in said, said
        assert not folder.exists(), "something was made before the build environment was looked at"
        said = ws("--gbrecomp", root / "none.exe", expect=1)
        assert "gbrecomp is not at" in said and "--gbrecomp" in said and not folder.exists(), said
        # Nor in a folder that is something else, nor from options that are this tool's to give.
        (empty / "notes.txt").write_text("mine", encoding="utf-8")
        assert "is not empty and is no workspace of this tool" in ws("--gbrecomp", args.gbrecomp, expect=1, at=empty)
        for option, words in ((["-o", "x"], "the folder is DIR/game"), (["--lift-export", "x"], "DIR/export.json"),
                              (["--lift", "x"], "lifted source"), (["--output-prefix", "x"], "say --prefix")):
            said = ws("--gbrecomp", args.gbrecomp, "--", *option, expect=1)
            assert f"{option[0]} is not for gbrecomp here" in said and words in said and not folder.exists(), said
        assert "holds no save state" in ws("--states", empty, expect=1)
        assert "--frames is 1 to" in ws("--frames", "99999", expect=1)

        # ---- With nothing but the ROM -------------------------------------------------------
        said = ws("--boot-frames", BOOT, "--prefix", "ws_game", "--gbrecomp", args.gbrecomp, *cmake, "--", *OPTIONS)
        game = next(path for path in (folder / "game" / "build" / "ws_game.exe", folder / "game" / "build" / "ws_game")
                    if path.exists())
        # The game and the export are made with the same options, both times: before anything is
        # played, and again once the records say what is data.
        made, described = ran(said, "gbrecomp", "--output-prefix ws_game"), ran(said, "gbrecomp", "--lift-export")
        assert len(made) == 2 and len(described) == 2, commands(said)
        for command in made + described:
            assert "--no-scan --pointer-scan off" in command, command
        assert ["--annotations" in command for command in made] == [False, True], made
        assert ["--annotations" in command for command in described] == [False, True], described
        assert len(ran(said, "ninja")) == 2 and len(ran(said, "gb_data.py", "split")) == 1, commands(said)
        # Played from switching on, with the recorder noting where tiles land, and a whole Game Boy.
        played = ran(said, "ws_game", "--data-use")
        assert len(played) == 1 and "--data-use-tiles" in played[0] and f"--limit-frames {BOOT}" in played[0] and \
            "--no-audio" not in played[0] and "--save-dir" in played[0] and "--input c" in played[0], played
        assert "and 1 more like it" in said, said
        assert sorted(path.name for path in (folder / "records").glob("*.json")) == ["boot-buttons.json", "boot.json"]
        manifest = manifest_of(tree)
        font = view_at(manifest, "tiles", FONT)
        assert font and font["tiles"] == 16 and font.get("from") == "seen", manifest["regions"]
        # What the game did not get to is as the analysis has it: the tile it walks into is code.
        assert region_at(manifest, SHEET)["kind"] == "code" and region_at(manifest, EXTRA)["kind"] == "code"
        assert not any(view["kind"] == "text" for region in manifest["regions"] for view in region.get("views", ()))
        assert "unpacked" not in manifest and not (tree / "unpackers.json").exists()
        # No charmap: the sheet of letters, and a charmap to fill in.
        assert (tree / "glyphs.png").read_bytes().startswith(b"\x89PNG") and (folder / "glyphs.state").exists()
        letters = json.loads((tree / "charmap.json").read_text(encoding="utf-8"))["letters"]
        assert letters and not any(letters.values()) and "80" in letters, letters
        assert (folder / "game.gb").read_bytes() == rom, "the tree of a ROM nobody edited does not build to it"
        readme = readme_of(folder)
        for words in ("WORKSPACE: a folder to mod it in", "Nobody's play was given", f"left for {BOOT} frames",
                      "No text yet", "tree/glyphs.png", "no unpackers.json says which routine unpacks",
                      "No names from a symbol file", "1 picture of tiles, 16 tiles",
                      "the tree builds to the original ROM, byte for byte", "2 records of the game played"):
            assert words in readme, (words, readme)
        assert "--unpacked" not in readme
        log = (folder / "workspace.log").read_text(encoding="utf-8")
        assert all(command in log for command in commands(said)) and "exit code 0" in log, "the log lacks a command"
        # The log has the run nobody pressed anything in as well: it is the other of the two.
        logged = [line for line in log.splitlines() if line.startswith("$ ") and "--data-use" in line and
                  "ws_game" in line]
        assert len(logged) == 2 and sum("--input" in line for line in logged) == 1, logged

        # ---- Again: nothing ------------------------------------------------------------------
        before = files_of(folder)
        said = ws()
        assert "nothing to do" in said and not commands(said), said
        after = files_of(folder)
        assert {name: after[name] for name in after if name != "workspace.log"} == \
            {name: before[name] for name in before if name != "workspace.log"}, "a run with nothing new changed a file"
        # A folder is of one ROM.
        other = root / "other.gb"
        other.write_bytes(rom[:0x200] + b"\x01" + rom[0x201:])
        assert "was made from another ROM" in run([sys.executable, WORKSPACE, other, folder], expect=(1,))

        # ---- A charmap turns the text on, and nothing else -----------------------------------
        charmap = root / "letters.json"
        charmap.write_text(json.dumps(CHARMAP), encoding="utf-8")
        said = ws("--charmap", charmap)
        assert ran(said, "gb_data.py", "text") and not ran(said, "gbrecomp") and not ran(said, "ninja") and \
            not ran(said, "ws_game") and not ran(said, "split"), commands(said)
        manifest = manifest_of(tree)
        assert "|BAD CAFE{line}A FAB BEAD{end}|" in strings_of(tree, manifest, TEXT1)
        readme = readme_of(folder)
        assert "1 string in 1 file of text" in readme and "No text yet" not in readme, readme
        assert "nothing to do" in ws()

        # ---- Edits: a pixel, a string, a byte ------------------------------------------------
        manifest = manifest_of(tree)
        picture = tree / view_at(manifest, "tiles", FONT)["file"]
        width, height, grey = gb_data.read_png(picture.name, picture.read_bytes())
        painted = bytearray(grey)
        for y in range(8):                              # the second tile of the font, all black
            painted[y * width + 8:y * width + 16] = bytes(8)
        picture.write_bytes(gb_data.write_png(width, height, bytes(painted)))
        text_region = region_at(manifest, TEXT1)
        text_file = tree / next(view for view in text_region["views"] if view["kind"] == "text")["file"]
        text_file.write_text(text_file.read_text(encoding="utf-8").replace("|BAD CAFE{line}", "|FAB DEAD{line}"),
                             encoding="utf-8")
        spare_region = region_at(manifest, SPARE)
        assert spare_region["offset"] > SHEET, "the byte to edit is to lie behind the tile, in a file the split joins"
        spare = bytearray((tree / spare_region["file"]).read_bytes())
        spare[SPARE - spare_region["offset"]] = 0x5A
        (tree / spare_region["file"]).write_bytes(spare)
        build = readme_command(folder, "build the tree")
        assert build[:3] == [sys.executable, str(ROOT / "tools" / "gb_data.py"), "build"], build
        run(build)
        edited = bytearray(rom)
        edited[FONT + 16:FONT + 32] = bytes([0xFF]) * 16
        edited[TEXT1:TEXT1 + 8] = letters_of("FAB DEAD")
        edited[SPARE] = 0x5A
        image = folder / "game.gb"
        assert image.read_bytes() == bytes(edited), "the edits are not what the tree builds to"

        # ---- Save states turn the second pass on ---------------------------------------------
        states = root / "states"
        states.mkdir()
        early = play(game, "early", 15)
        late = play(game, "late", APART + 20, "--no-audio")      # as states of the earlier tools were made
        assert video(early, 0x8000, 16) == bytes(16) and video(early, 0x8810, 16) == rom[FONT + 16:FONT + 32]
        assert video(late, 0x8000, 16) == TILE and video(late, 0x8410, 16) == bytes(16)
        for state in (early, late):
            state.replace(states / state.name)
        early, late = states / "early.state", states / "late.state"
        (states / "junk.state").write_bytes(bytes(64))          # and something that is no save state
        said = ws("--states", states, "--frames", FROM_STATE)
        # Three runs from states and none from switching on: those records are there. What is
        # no state leaves no record, and stops nothing.
        played = ran(said, "ws_game", "--data-use")
        assert len(played) == 1 and "--load-state-file" in played[0] and "and 2 more like it" in said, said
        assert "2 records in" in said and "1 not made: state-junk: the game did not load the state" in said, said
        assert not (folder / "records" / "state-junk.json").exists()
        assert "state-early.json" in os.listdir(folder / "records") and "state-late.json" in \
            os.listdir(folder / "records"), os.listdir(folder / "records")
        # What they showed went back to the analysis: the game and the export again, and the tree
        # split again with what it held, the old one kept.
        assert len(ran(said, "gbrecomp", "--output-prefix ws_game", "--annotations")) == 1 and \
            len(ran(said, "gbrecomp", "--lift-export", "--annotations")) == 1 and len(ran(said, "ninja")) == 1, \
            commands(said)
        again = ran(said, "gb_data.py", "split")
        assert len(again) == 1 and "--keep" in again[0] and "tree.next" in again[0], again
        assert not (folder / "tree.next").exists() and (folder / "old" / "tree-1" / "manifest.json").exists()
        assert "--keep-edited" in ran(said, "gb_data.py", "text")[0]
        manifest = manifest_of(tree)
        joined = region_at(manifest, SHEET)
        assert joined["kind"] == "data" and joined["offset"] == make_rom.code_end and \
            joined["offset"] + joined["size"] == PRINT, joined
        assert region_at(manifest_of(folder / "old" / "tree-1"), SHEET)["kind"] == "code"
        sheet = view_at(manifest, "tiles", SHEET)
        assert sheet and sheet["tiles"] == 1 and view_at(manifest, "map", BLOCKS), joined
        # The edited text is as it was left, and the strings of the other files are there.
        assert "|FAB DEAD{line}A FAB BEAD{end}|" in strings_of(tree, manifest, TEXT1)
        assert "|DEAF CAB{end}|" in strings_of(tree, manifest, TEXT2)
        assert "|FEED{end}|" in strings_of(tree, manifest, TEXT3), "the state made without sound was not played"
        assert image.read_bytes() == bytes(edited), "an edit was lost when the tree was split again"
        readme = readme_of(folder)
        for words in ("the tree is edited: the image it builds differs from the ROM in", "old/",
                      "1 run from save states had the game without its sound hardware", "4 records",
                      "1 piece of play left no record (state-junk: the game did not load the state",
                      "--retry tries them again",
                      "2 pictures of tiles, 17 tiles", "3 strings in 3 files of text"):
            assert words in readme, (words, readme)
        assert "Nobody's play was given" not in readme
        before = files_of(folder)
        said = ws()
        assert "nothing to do" in said and not commands(said), said
        after = files_of(folder)
        assert {name: after[name] for name in after if name != "workspace.log"} == \
            {name: before[name] for name in before if name != "workspace.log"}

        # What left no record is not tried again, until it is asked for.
        said = ws("--retry")
        assert "1 run of 40 frames from save states" in said and "1 not made: state-junk" in said and \
            not ran(said, "gbrecomp") and not ran(said, "gb_data.py", "uses"), said
        assert "nothing to do" in ws()

        # ---- The game started the way the README says shows the edits --------------------------
        # A tile that was code until the records said otherwise can be edited now, and the game
        # that was built again takes the image.
        picture = tree / sheet["file"]
        width, height, grey = gb_data.read_png(picture.name, picture.read_bytes())
        picture.write_bytes(gb_data.write_png(width, height, bytes(8) * 4 + bytes([255]) * 32))
        run(readme_command(folder, "build the tree"))
        edited[SHEET:SHEET + 16] = bytes([0xFF, 0xFF] * 4 + [0x00, 0x00] * 4)
        assert image.read_bytes() == bytes(edited)
        start = readme_command(folder, "start the game with it")
        assert Path(start[0]) == game and start[1:] == ["--rom-image", str(image)], start
        shown = root / "shown.state"
        run([*start, "--headless", "--save-dir", saves, "--limit-frames", 10, "--save-state-file", shown], timeout=120)
        assert video(shown, 0x8810, 16) == bytes([0xFF]) * 16, "the game does not show the edited font"
        assert video(shown, 0x9800, 8) == letters_of("FAB DEAD"), "the game does not print the edited string"
        run([*start, "--headless", "--save-dir", saves, "--load-state-file", early, "--limit-frames", FROM_STATE,
             "--save-state-file", shown], timeout=120)
        assert video(shown, 0x8000, 16) == bytes(edited[SHEET:SHEET + 16]), "the game does not draw the edited tile"

        # ---- A charmap that was worked on in the tree is the tree's own ------------------------
        mine = json.loads((tree / "charmap.json").read_text(encoding="utf-8"))
        mine["letters"]["60"] = "?"
        (tree / "charmap.json").write_text(json.dumps(mine), encoding="utf-8")
        charmap.write_text(json.dumps({**CHARMAP, "letters": {**CHARMAP["letters"], "61": "#"}}), encoding="utf-8")
        said = ws()
        assert "was written in the tree, and stays" in said and ran(said, "gb_data.py", "text"), said
        assert json.loads((tree / "charmap.json").read_text(encoding="utf-8")) == mine
        assert "|FAB DEAD{line}A FAB BEAD{end}|" in strings_of(tree, manifest_of(tree), TEXT1)

        # ---- A run that stops, and the next that goes on ---------------------------------------
        declaration = root / "unpackers.json"
        declaration.write_text(json.dumps({**DECLARATION, "unpackers": [{"name": "tiles"}]}), encoding="utf-8")
        said = ws("--unpackers", declaration, expect=1)
        assert "gb_workspace: gb_data.py stopped with exit code 2" in said and "goes on from here" in said and \
            ran(said, "watch-pack") and not ran(said, "unpacked"), said
        assert "watch-pack" not in json.loads((folder / "workspace.json").read_text(encoding="utf-8"))["done"]
        said = ws(expect=1)
        assert ran(said, "watch-pack") and not ran(said, "gb_data.py", "tiles") and not ran(said, "gbrecomp"), said
        declaration.write_text(json.dumps(DECLARATION), encoding="utf-8")
        symbols = root / "game.sym"
        symbols.write_text(SYMBOLS, encoding="utf-8")
        said = ws("--sym", symbols)
        # What was done holds: only the steps from the pack on, and the names.
        assert not ran(said, "gbrecomp") and not ran(said, "ninja") and not ran(said, "--data-use") and \
            not ran(said, "split"), commands(said)
        assert ran(said, "watch-pack") and ran(said, "gb_data.py", "unpacked"), commands(said)
        harvested = ran(said, "ws_game", "--unpacked-harvest")
        assert len(harvested) == 1 and "watch.gbup" in harvested[0] and "and 4 more like it" in said, said
        tabled = ran(said, "gb_data.py", "unpack", "--tables")
        assert len(tabled) == 1 and "--unpacker tiles" in tabled[0] and f"--state {early}" in tabled[0].replace(
            '"', ""), tabled
        assert len(ran(said, "gb_data.py", "names", "--sym")) == 2 and "--seen" in ran(said, "names")[0], said
        manifest = manifest_of(tree)
        things = {(thing["bank"], thing["address"]): thing for thing in manifest["unpacked"]}
        assert sorted(things) == [(1, S1), (1, S2), (1, S3)], sorted(things)
        for place, thing in things.items():
            assert (tree / thing["made"]).read_bytes() == unpacked(STREAMS[place[1]]), thing
        assert things[(1, S1)]["file"] == "unpacked/tiles/StreamOne.png", things[(1, S1)]
        assert view_at(manifest, "tiles", FONT)["file"].endswith("/FontGfx.tiles.png")
        assert region_at(manifest, S1)["file"] == "bank_01/StreamOne.bin", region_at(manifest, S1)["file"]
        assert "# 00:0271 TextOne" in strings_of(tree, manifest, TEXT1)
        listed = (tree / "tiles.txt").read_text(encoding="utf-8")
        assert "FontGfx.tiles.png" in listed and all((tree / line.split()[-1]).exists() for line in
                                                     listed.splitlines() if line[:2] == "00"), listed
        assert "FontGfx" in (folder / "names.txt").read_text(encoding="utf-8")
        readme = readme_of(folder)
        for words in ("3 things the game unpacks, of 3 streams", "2 of the streams were unpacked in play, and 1 "
                      "more are from the tables declared", "harvests/", "called/", "game.gbup, the pack"):
            assert words in readme, (words, readme)
        assert "No names from a symbol file" not in readme and "no unpackers.json" not in readme
        start = readme_command(folder, "start the game with it")
        assert start[1:] == ["--rom-image", str(image), "--unpacked", str(folder / "game.gbup")], start
        assert image.read_bytes() == bytes(edited), "an edit was lost"
        assert "nothing to do" in ws()

        # ---- A recorded run is replayed in pieces, recorder and pack on -----------------------
        recorded = root / "run"
        run([game, "--headless", "--record-run", recorded, "--limit-frames", 2 * APART + 20], timeout=120)
        said = ws("--run", recorded)
        assert ran(said, "run_record.py", "verify", "--states") and \
            ran(said, "run_record.py", "sweep", "--data-use", "--data-use-tiles") and \
            ran(said, "run_record.py", "sweep", "--unpacked-harvest", "watch.gbup"), commands(said)
        piece = "segment-0001-f000000000"
        assert (folder / "run-states" / f"{piece}.state").exists() and (folder / "records" / f"{piece}.json").exists()
        assert {(thing["bank"], thing["address"]) for thing in
                json.loads((folder / "harvests" / f"{piece}.json").read_text(encoding="utf-8"))["made"]} == \
            {(1, S1), (1, S2)}
        # It showed nothing the states had not: the game is not made again.
        assert not ran(said, "gbrecomp") and not ran(said, "split") and ran(said, "gb_data.py", "uses"), commands(said)
        assert "5 records" in readme_of(folder)
        assert image.read_bytes() == bytes(edited) and "nothing to do" in ws()

        # ---- Lines of one's own for the analysis go in with what the records show -------------
        # One says that bytes nothing was seen to use are data, which joins two files; the other
        # says what the records say too, of part of the font, and gbrecomp takes no line twice.
        own = root / "own.annotations"
        own.write_text(f"data 00:{EXTRA:04X} 5 ; mine\ndata 00:{FONT:04X} 32 ; half the font\n", encoding="utf-8")
        said = ws("--annotations", own)
        assert len(ran(said, "gbrecomp", "--annotations")) == 2 and len(ran(said, "split", "--keep")) == 1, \
            commands(said)
        told = (folder / "game.annotations").read_text(encoding="utf-8")
        assert f"data 00:{EXTRA:04X} 5 ; mine" in told and f"data 00:{FONT:04X} 32 ; half the font" in told and \
            f"data 00:{FONT + 32:04X} 224 ; tiles" in told and f"data 00:{FONT:04X} 256" not in told, told
        manifest = manifest_of(tree)
        joined = region_at(manifest, EXTRA)
        assert joined["kind"] == "data" and joined["offset"] <= TEXT2 and joined["offset"] + joined["size"] == 0x4000
        assert (folder / "old" / "tree-2" / "manifest.json").exists()
        # The picture that was edited went along to the file its tiles are in now, name and all.
        font = view_at(manifest, "tiles", FONT)
        assert font["file"].endswith("/FontGfx.tiles.png") and font["offset"] == FONT - joined["offset"], font
        assert image.read_bytes() == bytes(edited), "an edit was lost when the tree was split a second time"
        assert len(manifest["unpacked"]) == 3 and "|FAB DEAD{line}" in strings_of(tree, manifest, TEXT1)
        assert "nothing to do" in ws()
        run([*readme_command(folder, "start the game with it"), "--headless", "--save-dir", saves, "--limit-frames",
             10, "--save-state-file", shown], timeout=120)
        assert video(shown, 0x8810, 16) == bytes([0xFF]) * 16

        # ---- Everything given at once, in a fresh folder ---------------------------------------
        whole = root / "whole"
        charmap.write_text(json.dumps(CHARMAP), encoding="utf-8")
        said = ws("--states", states, "--frames", FROM_STATE, "--boot-frames", BOOT, "--charmap", charmap,
                  "--unpackers", declaration, "--sym", symbols, "--annotations", own, "--run", recorded,
                  "--gbrecomp", args.gbrecomp, *cmake, "--", *OPTIONS, at=whole)
        assert len(ran(said, "gbrecomp", "--output-prefix game")) == 2 and len(ran(said, "split", "--keep")) == 1, \
            commands(said)
        assert (whole / "game.gb").read_bytes() == rom
        one, other = manifest_of(whole / "tree"), manifest_of(tree)
        assert [(region["offset"], region["size"], region["kind"]) for region in one["regions"]] == \
            [(region["offset"], region["size"], region["kind"]) for region in other["regions"]]

        def views(manifest: dict) -> list:
            return sorted((region["offset"], view["kind"], view["file"]) for region in manifest["regions"]
                          for view in region.get("views", ()))

        assert views(one) == views(other), (views(one), views(other))
        assert sorted((thing["bank"], thing["address"], thing["file"]) for thing in one["unpacked"]) == \
            sorted((thing["bank"], thing["address"], thing["file"]) for thing in other["unpacked"])
        # The strings of the file that was edited in the other folder are all here: nobody edited this one.
        assert "|BAD CAFE{line}A FAB BEAD{end}|" in strings_of(whole / "tree", one, TEXT1)
        said = run([sys.executable, WORKSPACE, rom_path, whole])
        assert "nothing to do" in said and not commands(said), said

        # ---- A tree that is gone is made again, with everything in it --------------------------
        shutil.rmtree(whole / "tree")
        said = run([sys.executable, WORKSPACE, rom_path, whole])
        assert not ran(said, "gbrecomp") and not ran(said, "ninja") and not ran(said, "--data-use"), commands(said)
        again = manifest_of(whole / "tree")
        assert views(again) == views(one) and len(again["unpacked"]) == 3, views(again)
        assert (whole / "game.gb").read_bytes() == rom
        assert "|BAD CAFE{line}A FAB BEAD{end}|" in strings_of(whole / "tree", again, TEXT1)

    print("workspace end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
