#!/usr/bin/env python3
"""The commands of music and sound as text, end to end: `gb_data.py streams`.

Builds a small ROM around a sound engine that is made up for this test: a
table of four entries, each a channel and the address of its stream, and
streams of commands in two grammars, one for tunes and one for drums. No
game is compiled. A tree of the ROM is given a `streams.json` that declares
the two grammars and where the streams start, and then:

- `streams` has to follow every start through its commands and through what
  they call, jump and loop to, and write each region's passages as the text
  this test expects, with a passage cut where something points and where a
  command changes the grammar;
- a byte no command starts with, a target outside the ROM, a command that
  would leave its bank and a stream still going where its bank ends have to
  stop a stream with its address, and an entry of the table whose channel
  has no grammar has to be named; a table can say which bank its streams
  are in;
- the tree unedited, and with the text only saved again another way, has to
  build to the ROM byte for byte;
- an edited note, tempo, call and hexadecimal operand have to build to the
  bytes meant, and no others;
- a passage made shorter has to be written where its last command ends the
  stream, with what was after it left, and refused where it does not; one
  made longer has to be refused with how many bytes too many;
- every line that says something a grammar cannot (a command, a name, a
  value, an address out of reach, a first byte that is another command's, a
  passage's own line changed) has to be refused with the file and the line;
- a declaration that would not go back into the same bytes has to be
  refused when it is read;
- text and the region's file both edited have to be a disagreement, an
  edited file has to be left by `streams` and written again with `--force`;
- against a record of what the game read, the instructions that read
  commands have to be found, and the bytes they read that were and were not
  decoded counted;
- with the export cut so that some of the commands are code and some lie in
  two files, those have to be counted and left out, and the rest written;
- `--annotations` has to write the commands' bytes as lines `gbrecomp`
  takes; `names` has to rename the file with its region and put the names
  over the passages; `split --keep` has to carry the file where the region
  is the same bytes and leave the edit in the bytes where it is not; and
  `tiles` has to refuse to draw over commands.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"

START = 0x0150
HOME = 0x0200            # a stream in the home bank, which calls another there
HOME_CALLED = 0x0260
HOME_LOST = 0x0280       # and one whose call points where the home bank cannot see
HOME_EDGE = 0x3FFE       # and one whose command would run on into bank 1
HOME_TABLE, HOME_TABLE_BANKED = 0x02A0, 0x02B0  # two tables in the home bank of streams that are not
TABLE = 0x4000           # four entries: a channel, and where its stream is
CH1, CH2, CALLED, DRUMS, DRUMS_CALLED = 0x4100, 0x4140, 0x4160, 0x4180, 0x41A0
BROKEN = 0x41C0          # a stream with a byte no command has
SWITCH = 0x41E0          # and one that changes its grammar half way
EDGE = 0x7FFF            # and one that is still going when its bank ends
NOT_READ = 0x4120        # three bytes the record says the engine read, and nothing decodes
ALSO_COPIED = 0x4010     # and four it says another instruction read
READS, COPIES = (0, 0x0160), (0, 0x0170)       # the instructions the record says read

STREAMS = {
    HOME: [0xF0, 0x60, 0x02, 0xFF],                     # call 00:0260 / end
    HOME_CALLED: [0x41, 0xFF],                          # note G 2 / end
    HOME_LOST: [0xF0, 0x00, 0x41],                      # call 4100, which bank 0 cannot name
    HOME_EDGE: [0xF0, 0x00],                            # a call whose third byte is bank 1's first
    HOME_TABLE: [0x60, 0x41],                           # a table of one address, of a stream in bank 1
    HOME_TABLE_BANKED: [0x01, 0x60, 0x41],              # and one whose entry says the bank first
    # The channel is the low half of an entry's first byte; the high half is something else's.
    TABLE: [0x40, 0x00, 0x41, 0x01, 0x40, 0x41, 0x03, 0x80, 0x41, 0x09, 0x00, 0x41],
    CH1: [0xE0, 0x00, 0x90,                             # tempo 144
          0xE1, 0x77,                                   # volume 7 7
          0xD4,                                         # octave 4
          0x03,                                         # note C 4          <- the loop comes back here
          0x21,                                         # note E 2
          0xF0, 0x60, 0x41,                             # call 01:4160
          0x47,                                         # note G 8          <- the second channel jumps here
          0xF1, 0x00, 0x06, 0x41],                      # loop 0 01:4106
    CH2: [0x50,                                         # note A 1
          0xF1, 0x02, 0x40, 0x41,                       # loop 2 01:4140
          0x82,                                         # rest 3
          0xF0, 0x60, 0x02,                             # call 00:0260, in the home bank
          0xE2, 0x3C,                                   # wave $3C
          0xF2, 0x0B, 0x41],                            # jump 01:410B
    CALLED: [0x6F, 0xFF],                               # note B 16 / end
    DRUMS: [0x11, 0x40,                                 # hit snare 2 $40
            0x71,                                       # rest 2, which is another byte for the drums
            0xF0, 0xA0, 0x41,                           # call 01:41A0
            0xFF],                                      # end
    DRUMS_CALLED: [0x03, 0x7F, 0xFF],                   # hit kick 4 $7F / end
    BROKEN: [0x00, 0x80, 0x9B],                         # note C 1 / rest 1 / and no command starts with 9B
    SWITCH: [0x11, 0xF8, 0x20, 0x7F, 0xFF],             # note D 2 / drums_from_here / hit 2 1 $7F / end
    EDGE: [0x00],                                       # note C 1, and the ROM ends
}

DECLARATION = {
    "schema": "gbrecomp.streams", "version": 1,
    "grammars": {
        "tune": {"commands": [
            {"name": "note", "first": "00-6F", "operands": [
                {"name": "pitch", "bits": [4, 7], "names": ["C", "D", "E", "F", "G", "A", "B"]},
                {"name": "length", "bits": [0, 3], "add": 1}]},
            {"name": "rest", "first": "80-8F", "operands": [{"name": "length", "bits": [0, 3], "add": 1}]},
            {"name": "octave", "first": "D0-D7", "operands": [{"name": "octave", "bits": [0, 2], "from": 8}]},
            {"name": "tempo", "first": "E0", "operands": [{"name": "tempo", "byte": 1, "word": "big"}]},
            {"name": "volume", "first": "E1", "operands": [
                {"name": "left", "byte": 1, "bits": [4, 7]}, {"name": "right", "byte": 1, "bits": [0, 3]}]},
            {"name": "wave", "first": 0xE2, "operands": [{"name": "wave", "byte": 1, "hex": True}]},
            {"name": "call", "first": "F0", "operands": [
                {"name": "to", "byte": 1, "word": "little", "target": "call"}]},
            {"name": "loop", "first": "F1", "ends": {"times": 0}, "operands": [
                {"name": "times", "byte": 1}, {"name": "to", "byte": 2, "word": "little", "target": "loop"}]},
            {"name": "jump", "first": "F2", "operands": [
                {"name": "to", "byte": 1, "word": "little", "target": "jump"}]},
            {"name": "drums_from_here", "first": "F8", "then": "drums"},
            {"name": "end", "first": "FF", "ends": True}]},
        # A drum is two bytes where a note is one, and a rest is other bytes; what is left is the tune's.
        "drums": {"like": "tune", "commands": [
            {"name": "hit", "first": "00-6F", "operands": [
                {"name": "kind", "bits": [4, 7], "names": {"0": "kick", "1": "snare"}},
                {"name": "length", "bits": [0, 3], "add": 1},
                {"name": "strength", "byte": 1, "hex": True}]},
            {"name": "rest", "first": "70-7F", "operands": [{"name": "length", "bits": [0, 3], "add": 1}]}]}},
    "starts": [
        {"table": {"bank": 1, "address": "4000", "entries": 4, "entry_size": 3, "pointer": 1},
         "grammar": {"byte": 0, "mask": "0F", "is": {"0": "tune", "1": "tune", "3": "drums"}}},
        {"at": {"bank": 1, "address": "41C0"}, "grammar": "tune"},
        {"at": {"bank": 1, "address": "41E0"}, "grammar": "tune"},
        {"at": {"bank": 0, "address": "0200"}, "grammar": "tune"},
        {"at": {"bank": 0, "address": 0x0280}, "grammar": "tune"},
        {"at": {"bank": 0, "address": "3FFE"}, "grammar": "tune"},
        {"at": {"bank": 1, "address": "7FFF"}, "grammar": "tune"}],
}

# What `streams` has to write of bank 1, after the lines that say what the file is.
BANK_1 = """\
01:4100 tune 6
  tempo 144
  volume 7 7
  octave 4
01:4106 tune 5
  note C 4
  note E 2
  call 01:4160
01:410B tune 5
  note G 8
  loop 0 01:4106
01:4140 tune 14
  note A 1
  loop 2 01:4140
  rest 3
  call 00:0260
  wave $3C
  jump 01:410B
01:4160 tune 2
  note B 16
  end
01:4180 drums 7
  hit snare 2 $40
  rest 2
  call 01:41A0
  end
01:41A0 drums 3
  hit kick 4 $7F
  end
01:41C0 tune 2
  note C 1
  rest 1
# The stream stops after this, at 01:41C2: no command of the grammar 'tune' starts with 9B (the stream from the \
start at 01:41C0).
01:41E0 tune 2
  note D 2
  drums_from_here
01:41E2 drums 3
  hit 2 1 $7F
  end
01:7FFF tune 1
  note C 1
# The stream stops after this, at 02:4000: the stream from the start at 01:7FFF runs off the end of its bank.
"""
HOME_BANK = """\
00:0200 tune 4
  call 00:0260
  end
00:0260 tune 2
  note G 2
  end
"""


def make_rom() -> bytes:
    noise = random.Random(1989)
    rom = bytearray(noise.randbytes(0x8000))
    rom[0:START + 16] = bytes(START + 16)
    rom[0x100:0x104] = bytes([0x00, 0xC3, START & 0xFF, START >> 8])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13C] = b"STREAMS\0"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    rom[START:START + 3] = bytes([0xF3, 0x18, 0xFE])        # di / jr to itself
    for address, stream in STREAMS.items():
        rom[address:address + len(stream)] = bytes(stream)
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


def manifest_of(tree: Path) -> dict:
    return json.loads((tree / "manifest.json").read_text(encoding="utf-8"))


def region_at(tree: Path, offset: int) -> dict:
    return next(region for region in manifest_of(tree)["regions"]
                if region["offset"] <= offset < region["offset"] + region["size"])


def stream_view(tree: Path, offset: int) -> dict:
    return next(view for view in region_at(tree, offset).get("views", ()) if view["kind"] == "stream")


def body(text: str) -> str:
    """A file of passages without the comment lines it starts with."""
    lines = text.split("\n")
    first = next(index for index, line in enumerate(lines) if not line.startswith("#"))
    assert first == 5 and all(line.startswith("# ") for line in lines[:first]), lines[:6]
    return "\n".join(lines[first:])


def cut(export: dict, pieces: dict[int, list[tuple[int, int]]]) -> dict:
    """The export with some of its data regions cut into the given pieces, as (first offset, one
    past the last): what lies between two pieces is then code to `split`."""
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

    with tempfile.TemporaryDirectory(prefix="gbstreams-") as scratch:
        root = Path(scratch)
        rom = make_rom()
        rom_path = root / "streams.gb"
        rom_path.write_bytes(rom)
        analysis = ["--no-scan", "--pointer-scan", "off"]
        export_path = root / "export.json"
        run([args.gbrecomp, rom_path, *analysis, "--lift-export", export_path])
        export = json.loads(export_path.read_text(encoding="utf-8"))
        tree = root / "tree"
        gb_data("split", export_path, "--rom", rom_path, "-o", tree)
        bank, home = region_at(tree, TABLE), region_at(tree, HOME)
        assert (bank["offset"], bank["size"], bank["kind"]) == (0x4000, 0x4000, "data"), bank
        assert home["kind"] == "data" and home["offset"] <= HOME and home["offset"] + home["size"] == 0x4000, home
        image_path = root / "built.gb"

        def built(of: Path = tree, expect: int = 0, *more) -> bytes | str:
            """The image `build` makes of a tree, or what it says when it refuses."""
            said = gb_data("build", of, "-o", image_path, *more, expect=expect)
            return said if expect else image_path.read_bytes()

        def differs(image: bytes) -> dict[int, int]:
            assert len(image) == len(rom)
            return {offset: image[offset] for offset in range(len(rom)) if image[offset] != rom[offset]}

        def declare(of: Path, change=None) -> None:
            document = copy.deepcopy(DECLARATION)
            if change:
                change(document)
            (of / "streams.json").write_text(json.dumps(document, indent=1), encoding="utf-8")

        # --- Nothing declared: the command says what is missing, and the tree is as it was. ---
        said = gb_data("streams", tree, expect=2)
        assert "streams.json is not there" in said and "docs/DATA_FILES.md" in said, said
        assert built() == rom

        # --- Declared: every start followed, and the passages written as text. ---
        declare(tree)
        said = gb_data("streams", tree)
        assert "9 starts in 9 places: 5 read to where their commands end them, 4 not. 33 commands, 56 bytes" in said, \
            said
        assert "13 passages, 56 bytes, in 2 regions" in said, said
        assert "drums             3 passages       13 bytes" in said and \
            "tune             10 passages       43 bytes" in said, said
        assert "4 streams stopped before their end" in said, said
        assert "01:41C2: no command of the grammar 'tune' starts with 9B (the stream from the start at 01:41C0)" \
            in said, said
        assert "00:0280: the command 'call' here points at 4100, which is not in the ROM as bank 00 sees it" in said, \
            said
        assert "00:3FFE: the command 'call' here would leave its bank (the stream from the start at 00:3FFE)" in \
            said, said
        assert "02:4000: the stream from the start at 01:7FFF runs off the end of its bank" in said, said
        assert "and 3 more (--stops N lists more)" in gb_data("streams", tree, "--stops", "1")
        assert "entry 4 of the table at 01:4000 (01:4009) has 09 where its grammar is told" in said, said
        assert "nothing says yet what the game reads" in said, said
        assert "files of code" not in said and "two files" not in said and "two grammars" not in said, said
        view, home_view = stream_view(tree, CH1), stream_view(tree, HOME)
        name = view["file"]
        assert name == bank["file"][:-len(".bin")] + ".stream.txt" and view["declaration"] == "streams.json", view
        assert view["passages"] == [[0x100, 6, "tune"], [0x106, 5, "tune"], [0x10B, 5, "tune"], [0x140, 14, "tune"],
                                    [0x160, 2, "tune"], [0x180, 7, "drums"], [0x1A0, 3, "drums"], [0x1C0, 2, "tune"],
                                    [0x1E0, 2, "tune"], [0x1E2, 3, "drums"], [0x3FFF, 1, "tune"]], view["passages"]
        written = (tree / name).read_text(encoding="utf-8")
        assert body(written) == BANK_1, body(written)
        assert bank["file"] in written.split("\n")[0], written.split("\n")[0]
        assert body((tree / home_view["file"]).read_text(encoding="utf-8")) == HOME_BANK
        assert view["sha256"] == hashlib.sha256((tree / name).read_bytes()).hexdigest()
        assert view["data_sha256"] == hashlib.sha256(b"".join(
            rom[0x4000 + offset:0x4000 + offset + size] for offset, size, _ in view["passages"])).hexdigest()

        # --- Unedited, and only saved again, it is the ROM. ---
        assert built(tree, 0, "--expect-original") == rom
        status = gb_data("status", tree)
        assert "2 files of commands" in status and "nothing edited" in status, status
        (tree / name).write_bytes(b"\xef\xbb\xbf" + written.replace("\n", "\r\n").encode("utf-8") +
                                  b"\r\n# a comment, and a line with nothing on it\r\n\r\n")
        assert built(tree, 0, "--expect-original") == rom
        assert "nothing edited" in gb_data("status", tree)

        def edit(*changes: tuple[str, str], of: Path = tree, file: str = name, text: str = written) -> None:
            """The file of passages with some of its lines changed: (a line, what it becomes)."""
            for old, new in changes:
                assert text.count(old + "\n") == 1, old
                text = text.replace(old + "\n", new + "\n" if new else "")
            (of / file).write_text(text, encoding="utf-8", newline="\n")

        # --- An edit that keeps a passage's length is written, and nothing else changes. ---
        edit(("  note E 2", "  note G 3"), ("  tempo 144", "  tempo 300"), ("  call 01:41A0", "  call 01:4160"),
             ("  wave $3C", "  wave 0x40"), ("  hit kick 4 $7F", "  hit snare 16 255"), ("  octave 4", "  octave 8"),
             ("  rest 2", "  rest 3"), ("  call 00:0260", "  call 00:0200"))
        assert differs(built()) == {0x4107: 0x42, 0x4101: 0x01, 0x4102: 0x2C, 0x4184: 0x60, 0x414A: 0x40,
                                    0x41A0: 0x1F, 0x41A1: 0xFF, 0x4105: 0xD0, 0x4182: 0x72, 0x4147: 0x00}, \
            differs(built())
        status = gb_data("status", tree)
        for line in (f"{name} (01:4100, 3 of 6 bytes changed)", f"{name} (01:4106, 1 of 5 bytes changed)",
                     f"{name} (01:4140, 2 of 14 bytes changed)", f"{name} (01:4180, 2 of 7 bytes changed)",
                     f"{name} (01:41A0, 2 of 3 bytes changed)"):
            assert f"edited: {line}" in status, status
        assert "<-" not in status, status
        assert "expected to be the original ROM" in gb_data("build", tree, "-o", image_path, "--expect-original",
                                                            expect=1)
        # `streams` again leaves a file that was edited, and --force writes it from the region's file.
        said = gb_data("streams", tree)
        assert "1 of the files has been edited and is left as it is" in said, said
        assert differs(built())[0x4107] == 0x42 and stream_view(tree, CH1) == view
        said = gb_data("streams", tree, "--force")
        assert "edited" not in said and built(tree, 0, "--expect-original") == rom, said
        assert (tree / name).read_text(encoding="utf-8") == written and stream_view(tree, CH1) == view

        # --- Shorter, where the last command ends the stream: written, and what was after it stays. ---
        edit(("  note G 8", ""), ("  rest 3", ""))
        assert differs(built()) == {0x410B: 0xF1, 0x410C: 0x00, 0x410D: 0x06, 0x410E: 0x41,
                                    0x4145: 0xF0, 0x4146: 0x60, 0x4147: 0x02, 0x4148: 0xE2, 0x4149: 0x3C,
                                    0x414A: 0xF2, 0x414B: 0x0B, 0x414C: 0x41}, differs(built())
        status = gb_data("status", tree)
        assert f"{name} (01:410B, 4 of 5 bytes changed)   <- 1 byte shorter" in status, status
        # And refused where it does not: a call, and a loop that comes to an end, go on to what follows.
        for changes, passage, less, last in (((("  note E 2", ""),), "01:4106", 1, "call"),
                                             ((("  note G 8", ""), ("  loop 0 01:4106", "  loop 2 01:4106")),
                                              "01:410B", 1, "loop"),
                                             ((("  rest 2", ""),), "01:4180", 1, "end")):
            edit(*changes)
            if last == "end":
                assert differs(built()) == {0x4182: 0xF0, 0x4183: 0xA0, 0x4184: 0x41, 0x4185: 0xFF}
                continue
            said = built(tree, 2)
            assert f"{name}: the passage at {passage} is {less} byte shorter than the 5 it had, and its last " \
                   f"command, '{last}', does not end the stream" in said, said
        # --- Longer: refused, with how many bytes too many. ---
        edit(("  note E 2", "  note E 2\n  note E 2\n  rest 1"))
        said = built(tree, 2)
        assert f"{name}: the passage at 01:4106 is 7 bytes now and there is room for 5: 2 too many" in said, said
        assert "2 too many" in gb_data("status", tree, expect=2)

        # --- A line that says what the grammar cannot is refused, with where it is. ---
        line_of = {line: number for number, line in enumerate(written.split("\n"), 1)}
        for old, new, why in (
                ("  note E 2", "  note H 2", "'H' is no number, and no name 'pitch' has (C, D, E, F, G, A, B)"),
                ("  note E 2", "  note E 17", "'length' is 17, and it goes from 1 to 16"),
                ("  note E 2", "  note E 0", "'length' is 0, and it goes from 1 to 16"),
                ("  note E 2", "  note E", "'note' takes pitch length, and the line has 1 word after it"),
                ("  note E 2", "  chord E 2", "the grammar 'tune' has no command 'chord'"),
                ("  note E 2", "  hit kick 2 $40", "the grammar 'tune' has no command 'hit'"),
                ("  note E 2", "  note 7 2", "'note 7 2' would start with the byte 71, which is no command in the "
                                              "grammar 'tune'"),
                ("  octave 4", "  octave 0", "'octave' is 0, and it goes from 1 to 8"),
                ("  call 01:4160", "  call 00:4160", "00:4160 cannot be reached from here"),
                ("  call 01:4160", "  call 02:4160", "02:4160 cannot be reached from here"),
                ("  call 01:4160", "  call 4160", "'to' is where the stream goes on, an address written BB:AAAA"),
                ("  hit snare 2 $40", "  hit snare 2 $100", "'strength' is $100, and it goes from 0 to 255"),
                ("  hit snare 2 $40", "  note C 2", "the grammar 'drums' has no command 'note'")):
            edit((old, new))
            said = built(tree, 2)
            assert f"{name}, line {line_of[old]}: " in said and why in said, (new, said)
        edit(("  drums_from_here", "  drums_from_here\n  end"))
        assert "'drums_from_here' on the line before changes the grammar" in built(tree, 2)
        (tree / name).write_text(written.replace("  note B 16\n  end\n", ""), encoding="utf-8", newline="\n")
        assert "the passage at 01:4160 has no command left" in built(tree, 2)
        for old, new in (("01:4106 tune 5", "01:4106 tune 6"), ("01:4106 tune 5", "01:4106 drums 5"),
                         ("01:4106 tune 5", "01:4107 tune 5"), ("01:4106 tune 5", "")):
            edit((old, new))
            said = built(tree, 2)
            assert "the lines that passages start with are not to be edited" in said and \
                "where the passage '01:4106 tune 5' was" in said, said
        (tree / name).write_text("  note C 1\n" + written, encoding="utf-8", newline="\n")
        assert "line 1: 'note C 1' is before the first passage's line" in built(tree, 2)
        (tree / name).write_bytes(written.encode("utf-8") + b"\xff")
        assert "is not text in UTF-8" in built(tree, 2)

        # --- The text and the region's file both edited: a disagreement, unless they say the same. ---
        edit(("  note E 2", "  note G 3"))
        content = bytearray((tree / bank["file"]).read_bytes())
        content[0x107] = 0x33
        (tree / bank["file"]).write_bytes(content)
        said = built(tree, 2)
        assert f"{name} and {bank['file']} have both been edited, and disagree about 1 of the 11 passages" in said, \
            said
        content[0x107] = 0x42
        (tree / bank["file"]).write_bytes(content)
        assert differs(built()) == {0x4107: 0x42}
        # Written again, the text says what the file says now.
        gb_data("streams", tree, "--force")
        assert "  note G 3\n" in (tree / name).read_text(encoding="utf-8") and differs(built()) == {0x4107: 0x42}
        (tree / bank["file"]).write_bytes(rom[0x4000:0x8000])
        gb_data("streams", tree, "--force")
        assert (tree / name).read_text(encoding="utf-8") == written and built() == rom
        # A file the manifest does not know is not written over, and a view's file that is gone is asked for.
        (tree / name).rename(tree / "kept.txt")
        assert "`streams` writes it again" in built(tree, 2)
        gb_data("streams", tree)
        assert (tree / name).read_text(encoding="utf-8") == written
        other = root / "other"
        gb_data("split", export_path, "--rom", rom_path, "-o", other)
        declare(other)
        (other / name).write_text("mine\n", encoding="utf-8")
        assert "is already there and the manifest does not know it" in gb_data("streams", other, expect=2)
        assert (other / name).read_text(encoding="utf-8") == "mine\n"
        # Bytes that are commands are not drawn as tiles, and a picture's bytes are left to it.
        said = gb_data("tiles", tree, bank["file"], "--offset", "0x141", "--tiles", "1", "--bpp", "1", expect=2)
        assert f"are commands of music or sound, which {name} has" in said, said
        gb_data("tiles", other, bank["file"], "--offset", "0x1D8", "--tiles", "1")
        gb_data("streams", other, "--force")
        said = gb_data("streams", other)
        assert "5 more bytes lie in bytes a picture or a file of text has" in said and \
            "11 passages, 51 bytes, in 2 regions" in said, said
        assert [0x1E0, 2, "tune"] not in stream_view(other, CH1)["passages"] and built(other) == rom

        # --- A declaration that could not go back into the same bytes is refused when it is read. ---
        def tune(document: dict) -> list:
            return document["grammars"]["tune"]["commands"]

        for change, why in (
                (lambda d: tune(d).append({"name": "x", "first": "A0", "operands": [
                    {"name": "v", "byte": 1, "bits": [0, 3]}]}), "no operand says what bits 11110000 of its byte 1"),
                (lambda d: tune(d).append({"name": "x", "first": "A0-AF"}), "no operand says how they differ"),
                (lambda d: tune(d).append({"name": "x", "first": "A0", "operands": [
                    {"name": "v", "byte": 1}, {"name": "w", "byte": 1, "bits": [7, 7]}]}),
                 "two operands of the same bits of its byte 1"),
                (lambda d: tune(d).append({"name": "x", "first": "A0", "operands": [
                    {"name": "to", "byte": 1, "target": "call"}]}), "is a target, which is an address: a word"),
                (lambda d: tune(d).append({"name": "x", "first": "A0", "then": "brass"}),
                 "is followed by the grammar 'brass'"),
                (lambda d: tune(d).append({"name": "note", "first": "A0"}), "two commands of one name"),
                (lambda d: tune(d).append({"name": "x", "first": "A0", "ends": {"times": 0}}),
                 "ends its stream where 'times' is 0, which is no operand of it"),
                (lambda d: tune(d).append({"name": "x", "first": "A0", "size": 2}), "has no such field as 'size'"),
                (lambda d: tune(d).append({"name": "x", "first": "G0"}), "a number, or a string of hexadecimal"),
                (lambda d: tune(d)[0]["operands"][0].update(names=["C", "C"]), "give two values the same name"),
                (lambda d: tune(d)[0]["operands"][0].update(names=["12"]), "a name is a word"),
                (lambda d: d["grammars"]["drums"].update(like="brass"), "is like 'brass', which"),
                (lambda d: d["grammars"]["tune"].update(like="drums"), "is like itself"),
                (lambda d: d["starts"][1].update(grammar="brass"), "is in the grammar 'brass', which"),
                (lambda d: d["starts"][0]["table"].update(pointer=2), "room in it for the pointer's two bytes"),
                (lambda d: d["starts"][1]["at"].update(bank=0), "is not an address of the ROM"),
                (lambda d: d.update(schema="gbrecomp.unpackers"), "is not a version 1 gbrecomp.streams file")):
            declare(other, change)
            said = gb_data("streams", other, "--force", expect=2)
            assert why in said, (why, said)
        # A table that runs out of its bank leads nowhere, and is said; so is a start outside the ROM.
        declare(other, lambda d: d["starts"][0]["table"].update(address="7FFA"))
        assert "the table at 01:7FFA is not in the ROM, or does not end in its bank" in \
            gb_data("streams", other, "--force")
        declare(other, lambda d: d["starts"][1]["at"].update(bank=2))
        assert "the start at 02:41C0 is not in the ROM" in gb_data("streams", other, "--force")
        # A command that is every first byte of another takes its place, and one read two ways is said.
        declare(other, lambda d: d["starts"].append({"at": {"bank": 1, "address": "4186"}, "grammar": "tune"}))
        said = gb_data("streams", other, "--force")
        assert "1 place is read in two grammars, and written in the first: 01:4186 as 'drums' and as 'tune'" in said, \
            said
        assert [0x186, 1, "drums"] in stream_view(other, CH1)["passages"]
        # A table can say which bank its streams are in: by a number, or by a byte of each entry.
        declare(other, lambda d: d["starts"].extend([
            {"table": {"bank": 0, "address": "02A0", "entries": 1, "stream_bank": 1}, "grammar": "tune"},
            {"table": {"bank": 0, "address": "02B0", "entries": 1, "entry_size": 3, "pointer": 1,
                       "stream_bank": {"byte": 0}}, "grammar": "tune"}]))
        said = gb_data("streams", other, "--force")
        assert "11 starts in 10 places: 7 read to where their commands end them, 4 not" in said and \
            "of the table at 00:02" not in said, said
        declare(other, lambda d: d["starts"].append({"at": {"bank": 1, "address": "4109"}, "grammar": "tune"}))
        said = gb_data("streams", other, "--force")
        assert "01:4109: the stream from the start at 01:4109 leads into the middle of a command" in said, said

        # --- Against what the game was seen to read. ---
        record = root / "use.json"
        ranges = [{"offset": first, "size": size, "as_it_is": [], "worked_out": ["sound"], "read_at": list(reader),
                   "one_reader": True}
                  for first, size, reader in ((CH1, 16, READS), (CALLED, 2, READS), (NOT_READ, 3, READS),
                                              (TABLE, 12, COPIES), (ALSO_COPIED, 4, COPIES), (DRUMS, 2, COPIES))]
        record.write_text(json.dumps({"schema": "gbrt.data-use", "version": 1,
                                      "rom": {"size": len(rom), "global_checksum": (rom[0x14E] << 8) | rom[0x14F]},
                                      "ranges": sorted(ranges, key=lambda entry: entry["offset"])}), encoding="utf-8")
        gb_data("uses", tree, record)
        said = gb_data("streams", tree)
        assert "against the game played, whose commands are read at 00:0160 (found):" in said, said
        assert "of the 21 bytes of data files that those instructions read first, 18 are in decoded commands and 3 " \
               "are not" in said and "not decoded: 3 bytes at 01:4120" in said, said
        assert "of the 56 decoded bytes in data files, those instructions read 18, another instruction read 2 first " \
               "(00:0170 2), and no record saw 36 read" in said, said
        said = gb_data("streams", tree, "--reader", "00:0170")
        assert "are read at 00:0170 (named)" in said and "of the 18 bytes of data files that those instructions " \
            "read first, 2 are in decoded commands and 16 are not" in said and \
            "not decoded: 12 bytes at 01:4000" in said and "not decoded: 4 bytes at 01:4010" in said, said
        declare(tree, lambda d: d.update(readers=[{"bank": 0, "address": "0160"}, {"bank": 0, "address": "0170"}]))
        said = gb_data("streams", tree)
        assert "are read at 00:0160, 00:0170 (streams.json says)" in said and \
            "first, 20 are in decoded commands and 19 are not" in said and "no record saw 36 read" in said, said
        assert "--reader takes BANK:ADDRESS" in gb_data("streams", tree, "--reader", "0160", expect=2)
        declare(tree)
        assert (tree / name).read_text(encoding="utf-8") == written and built(tree, 0, "--expect-original") == rom

        # --- Commands in files of code, and commands that lie in two files. ---
        by_hand = root / "by-hand.json"
        by_hand.write_text(json.dumps(cut(export, {0x4000: [(0x4000, 0x4108), (0x410A, CALLED), (CALLED + 2, 0x4183),
                                                            (0x4184, 0x8000)]})), encoding="utf-8")
        pieces = root / "pieces"
        gb_data("split", by_hand, "--rom", rom_path, "-o", pieces)
        declare(pieces)
        notes = root / "streams.annotations"
        said = gb_data("streams", pieces, "--annotations", notes)
        assert "5 read to where their commands end them, 4 not. 33 commands, 56 bytes" in said, said
        assert "13 passages, 48 bytes, in 5 regions" in said, said
        assert "2 more bytes of commands are in files of code" in said and \
            "6 more bytes are of commands that lie in two files" in said, said
        texts = {region["address"]: body((pieces / view["file"]).read_text(encoding="utf-8"))
                 for region in manifest_of(pieces)["regions"] if region["bank"] == 1
                 for view in region.get("views", ())}
        assert sorted(texts) == [0x4000, 0x410A, 0x4162, 0x4184], sorted(texts)
        assert texts[0x4000] == "01:4100 tune 6\n  tempo 144\n  volume 7 7\n  octave 4\n" \
                                "01:4106 tune 2\n  note C 4\n  note E 2\n", texts[0x4000]
        assert texts[0x410A].startswith("01:410B tune 5\n  note G 8\n  loop 0 01:4106\n01:4140 tune 14\n"), \
            texts[0x410A]
        assert texts[0x4162] == "01:4180 drums 3\n  hit snare 2 $40\n  rest 2\n", texts[0x4162]
        assert texts[0x4184].startswith("01:4186 drums 1\n  end\n01:41A0 drums 3\n"), texts[0x4184]
        assert built(pieces, 0, "--expect-original") == rom
        first_piece = next(view["file"] for view in region_at(pieces, CH1)["views"])
        edit(("  note E 2", "  note B 1"), of=pieces, file=first_piece,
             text=(pieces / first_piece).read_text(encoding="utf-8"))
        assert differs(built(pieces)) == {0x4107: 0x60}
        # What `--annotations` wrote is every run of commands, and the analysis takes it.
        lines = [line for line in notes.read_text(encoding="utf-8").split("\n") if line and not line.startswith("#")]
        assert lines == ["data 00:0200 4 ; commands", "data 00:0260 2 ; commands", "data 01:4100 16 ; commands",
                         "data 01:4140 14 ; commands", "data 01:4160 2 ; commands", "data 01:4180 7 ; commands",
                         "data 01:41A0 3 ; commands", "data 01:41C0 2 ; commands", "data 01:41E0 5 ; commands",
                         "data 01:7FFF 1 ; commands"], lines
        told = root / "told.json"
        run([args.gbrecomp, rom_path, *analysis, "--annotations", notes, "--lift-export", told])
        assert any(region["rom_offset"] <= CH1 and CH1 + 16 <= region["rom_offset"] + region["size"]
                   for region in json.loads(told.read_text(encoding="utf-8"))["data_regions"])

        # --- Names: the file goes by its region's name, and a passage has its name over it. ---
        (tree / "names.sym").write_text("01:4000 SoundBank\n01:4100 Tune_Ch1\n01:4180 Drums ; the fourth channel\n"
                                        "01:4186 Drums.end\n", encoding="utf-8", newline="\n")
        gb_data("names", tree)
        renamed = stream_view(tree, CH1)["file"]
        assert renamed != name and renamed.endswith(".stream.txt") and "SoundBank" in renamed and \
            (tree / renamed).exists() and not (tree / name).exists(), renamed
        assert renamed == region_at(tree, CH1)["file"][:-len(".bin")] + ".stream.txt"
        assert built(tree, 0, "--expect-original") == rom
        gb_data("streams", tree)
        named = (tree / renamed).read_text(encoding="utf-8")
        assert "# 01:4100 Tune_Ch1\n01:4100 tune 6\n" in named and "# 01:4180 Drums\n01:4180 drums 7\n" in named and \
            "Drums.end" not in named and region_at(tree, CH1)["file"] in named.split("\n")[0], named
        assert stream_view(tree, CH1)["file"] == renamed and built(tree, 0, "--expect-original") == rom
        edit(("  note E 2", "  note G 3"), file=renamed, text=named)
        assert differs(built()) == {0x4107: 0x42}

        # --- Split again: the file goes with a region that is the same bytes, and the edit is never lost. ---
        again = root / "again"
        gb_data("split", export_path, "--rom", rom_path, "-o", again, "--keep", tree)
        assert stream_view(again, CH1)["passages"] == view["passages"]
        assert (again / stream_view(again, CH1)["file"]).read_text(encoding="utf-8") == named.replace(
            "  note E 2", "  note G 3")
        assert (again / "streams.json").read_bytes() == (tree / "streams.json").read_bytes()
        assert differs(built(again)) == {0x4107: 0x42}
        cut_again = root / "cut-again"
        said = gb_data("split", by_hand, "--rom", rom_path, "-o", cut_again, "--keep", tree)
        assert f"{renamed}, a view of kind 'stream'" in said and "An edit made through it is in the bytes of the " \
            "new files" in said, said
        assert differs(built(cut_again)) == {0x4107: 0x42}
        assert not any(view["kind"] == "stream" and region["bank"] == 1
                       for region in manifest_of(cut_again)["regions"] for view in region.get("views", ()))
        gb_data("streams", cut_again)
        shown = (cut_again / next(view["file"] for view in region_at(cut_again, CH1)["views"])).read_text(
            encoding="utf-8")
        assert "  note C 4\n  note G 3\n" in shown and differs(built(cut_again)) == {0x4107: 0x42}, shown
        # And `split --force` does not write over a tree whose commands were edited.
        said = gb_data("split", export_path, "--rom", rom_path, "-o", tree, "--force", expect=2)
        assert "has edits" in said and renamed in said, said

    print("data streams: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
