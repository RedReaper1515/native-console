#!/usr/bin/env python3
"""Packed data edited as what the game's own routine unpacks it to, end to end.

Builds a small game with a routine that unpacks run-length streams into a
buffer of sixteen bytes, one tile, and a main that has it unpack three
streams, each of them a second way as well (each byte with its halves
changed about, when a byte of RAM says so), one of them through a routine
that ends by jumping to the unpacker. After each the buffer is copied to
video memory. Another stream is never unpacked.

- With `unpackers.json` in the tree, `gb_data.py build` has to write a pack
  beside the image, and a game given the pack and `--unpacked-harvest` has to
  end as it does without, and write out the three different things made.
- `gb_data.py unpacked` has to put them into the tree as pictures, and find
  which are one stream unpacked two ways, each the other with the bits of
  every byte moved about. Given a harvest that lacks one stream's second
  way, it has to work that out from the way the other two streams show, and
  what it works out has to be what the game does make.
- With one picture edited, `build` has to leave the image the ROM, put that
  picture and its twin into the pack, and the game given the pack has to
  show both, and the stream nobody edited as it was, recompiled and lifted.
- From that tree, with its picture edited, `gb_data.py watch-pack` has to
  write the pack `build` wrote before anything was edited, byte for byte: a
  pack that only watches. A run recorded with no pack has to replay with it,
  and be swept with it and `--unpacked-harvest` for what the game unpacked,
  where with the pack `build` writes from that tree it is not the same run.
- Where an image changes what the routine makes of a stream, the game has to
  leave it alone: a replacement is for what it was drawn from.
- A pack of another ROM, a pack cut short, a harvest of an edited image and
  a picture that changed size have to be refused, each by name.
- A run recorded (`--record-run`) while a pack replaces something is a run of
  the game with those replacements: its record has to say which they are, and
  it has to replay with them, however the pack lists them, and be refused by
  name without them or with others. A pack that only watches, and a harvest,
  change nothing, so a recorded run can be swept for what the game unpacks.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"
GB_LIFT = ROOT / "tools" / "gb_lift.py"
RUN_RECORD = ROOT / "tools" / "run_record.py"
sys.path.insert(0, str(ROOT / "tools"))
import gb_data  # noqa: E402  (its PNG reader and writer)
import gb_discover  # noqa: E402  (the save state reader)
import gb_unpacked  # noqa: E402

START = 0x0150
UNPACK, TAIL = 0x0300, 0x0340
STREAM1, STREAM2, STREAM3, STREAM4 = 0x4000, 0x4010, 0x4020, 0x4030
BUFFER, SECOND_WAY = 0xC100, 0xC0F0
FRAMES = 20
RUN_FRAMES = 70   # of a recorded run: a checkpoint as it starts, one at frame 60, and its end
# Run-length pairs, a count and a byte, to a count of nought or of 255: the routine returns
# one way for the one and another way for the other. The four bytes of the first are chosen so
# that every bit of a byte goes its own way through them.
PACKED1 = bytes([4, 0x55, 4, 0x66, 4, 0x78, 4, 0x80, 0])
PACKED2 = bytes([8, 0xF0, 8, 0x0F, 0xFF])
PACKED3 = bytes([16, 0xAA, 0])
PACKED4 = bytes([4, 0x80, 4, 0x78, 4, 0x66, 4, 0x55, 0])
# What the game unpacks, in order: which stream, the second way or not, and how the routine is entered.
STEPS = ((1, False, "call"), (2, False, "tail"), (1, True, "call"), (2, True, "call"), (4, True, "call"),
         (4, False, "call"), (1, False, "call"))


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def copy(source: int, target: int, count: int) -> list[int]:
    """ld hl,source / ld de,target / ld b,count / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop"""
    return [0x21, *word(source), 0x11, *word(target), 0x06, count, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]


def unpacked(packed: bytes) -> bytes:
    out = bytearray()
    at = 0
    while packed[at] not in (0, 0xFF):
        out += bytes([packed[at + 1]]) * packed[at]
        at += 2
    return bytes(out)


def halves_changed(data: bytes) -> bytes:
    return bytes(((value << 4) | (value >> 4)) & 0xFF for value in data)


def make_rom() -> bytearray:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13F] = b"DATA PACKED"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF
    # The screen off while video memory is written: wait for line 144, then xor a / ldh [$40],a
    code += [0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xAF, 0xE0, 0x40]
    streams = {1: STREAM1, 2: STREAM2, 4: STREAM4}
    for tile, (stream, second_way, entered) in enumerate(STEPS):
        code += [0x3E, int(second_way), 0xEA, *word(SECOND_WAY)]           # ld a,way / ld [SecondWay],a
        if entered == "tail":
            code += [0x21, *word(streams[stream]), 0xCD, *word(TAIL)]      # ld hl,Stream / call Tail
        else:
            code += [0x3E, 0x01, 0x21, *word(streams[stream]), 0xCD, *word(UNPACK)]  # ld a,1 / ld hl,.. / call Unpack
        code += copy(BUFFER, 0x8000 + 16 * tile, 16)
    code += [0x3E, 0x91, 0xE0, 0x40, 0x18, 0xFE]                           # ld a,$91 / ldh [$40],a / jr to itself
    assert START + len(code) <= UNPACK
    rom[START:START + len(code)] = bytes(code)
    rom[UNPACK:UNPACK + 30] = bytes([
        0x11, *word(BUFFER),            # Unpack: ld de,Buffer        bank in A, stream in HL
        0x2A, 0xB7, 0xC8,               # next:   ld a,[hli] / or a / ret z      one way out
        0xFE, 0xFF, 0x20, 0x01, 0xC9,   #         cp $FF / jr nz,go / ret        and the other
        0x47, 0x2A, 0x4F,               # go:     ld b,a / ld a,[hli] / ld c,a
        0xFA, *word(SECOND_WAY),        # fill:   ld a,[SecondWay]
        0xB7, 0x79, 0x28, 0x02,         #         or a / ld a,c / jr z,plain
        0xCB, 0x37,                     #         swap a
        0x12, 0x13, 0x05, 0x20, 0xF2,   # plain:  ld [de],a / inc de / dec b / jr nz,fill
        0x18, 0xE5])                    #         jr next
    rom[TAIL:TAIL + 5] = bytes([0x3E, 0x01, 0xC3, *word(UNPACK)])          # Tail: ld a,1 / jp Unpack
    rom[STREAM1:STREAM1 + len(PACKED1)] = PACKED1
    rom[STREAM2:STREAM2 + len(PACKED2)] = PACKED2
    rom[STREAM3:STREAM3 + len(PACKED3)] = PACKED3
    rom[STREAM4:STREAM4 + len(PACKED4)] = PACKED4
    return rom


DECLARATION = {"schema": "gbrecomp.unpackers", "version": 1, "unpackers": [
    {"name": "tiles", "function": {"bank": 0, "address": UNPACK},
     "stream": {"bank": {"register": "a"}, "address": {"register": "hl"}},
     "made": [{"address": BUFFER, "size": 16}],
     "as": {"kind": "tiles", "bpp": 2, "width": 16},
     "why": "the test's own routine"}]}


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def tool(*arguments: object, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_DATA), *map(str, arguments)], expect=expect)


def build_game(gbrecomp: Path, rom_path: Path, project: Path, cmake_args: list[str], *more: str) -> Path:
    run([str(gbrecomp), str(rom_path), "--no-scan", "--pointer-scan", "off", *more, "-o", str(project),
         "--output-prefix", "packed_game"])
    run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"), "-DCMAKE_BUILD_TYPE=Release",
         *cmake_args])
    run(["ninja", "-C", str(project / "build")])
    return next(path for path in (project / "build" / "packed_game.exe", project / "build" / "packed_game")
                if path.exists())


def play(executable: Path, root: Path, name: str, *flags: object,
         expect: tuple[int, ...] = (0,)) -> tuple[dict, list[bytes], str]:
    """A run of the game: how it ends, the tiles it drew, and what it said."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump, state = root / f"{name}.json", root / f"{name}.state"
    result = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES), "--save-dir",
                  str(saves), "--dump-state", str(dump), "--save-state-file", str(state), *map(str, flags)],
                 expect=expect, timeout=180)
    said = result.stdout + result.stderr
    if result.returncode:
        return {}, [], said
    machine = gb_discover.Savestate.load(state)
    tiles = [bytes(machine.read(0x8000 + 16 * tile + index) for index in range(16))
             for tile in range(len(STEPS))]
    return json.loads(dump.read_text(encoding="utf-8")), tiles, said


def recorded_runs(game: Path, root: Path, watching: Path, edited: Path, relisted: Path, others: Path) -> None:
    """Runs recorded with and without replacements, and each played again with every pack.
    `watching` replaces nothing, `edited` and `relisted` hold the same two replacements, listed
    differently, and `others` holds four."""
    saves = root / "saves-replays"
    saves.mkdir()

    def record(name: str, *flags: object) -> tuple[Path, list[str], str]:
        """A run recorded: its folder, its record's header, and what the game said."""
        folder = root / f"run-{name}"
        result = run([str(game), "--headless", "--record-run", str(folder), "--limit-frames", str(RUN_FRAMES),
                      "--report-unpacked", *map(str, flags)], timeout=180)
        text = (folder / "segment-0001.run").read_text(encoding="ascii")
        return folder, text[:text.index("\nbegin\n")].splitlines(), result.stdout + result.stderr

    def replay(folder: Path, *flags: object, expect: int = 0) -> tuple[dict[str, str], str]:
        """A run's one segment played again: its result line, if it got that far, and what was said."""
        result = run([str(game), "--headless", "--replay-run", str(folder / "segment-0001.run"), "--save-dir",
                      str(saves), *map(str, flags)], expect=(expect,), timeout=180)
        said = result.stdout + result.stderr
        found = re.search(r"^\[RUN\] result (.*)$", said, re.M)
        return dict(word.split("=", 1) for word in found.group(1).split()) if found else {}, said

    def replays(folder: Path, *flags: object) -> str:
        result, said = replay(folder, *flags)
        assert result["end"] == "matched" and result["matched"] == result["checkpoints"] == "2", (flags, result)
        return said

    def refused(folder: Path, words: str, *flags: object) -> str:
        """Told apart before anything runs: no result, and nothing about where the replay left the record."""
        result, said = replay(folder, *flags, expect=1)
        assert not result and words in said and "the replay left the record" not in said, (flags, said)
        return said

    def record_tool(*arguments: object, expect: int = 0) -> str:
        result = run([sys.executable, str(RUN_RECORD), *map(str, arguments)], expect=(expect,))
        return result.stdout + result.stderr

    def things_made(path: Path) -> int:
        return len(json.loads(path.read_text(encoding="utf-8"))["made"])

    # Played with the two pictures replaced. The record says how many replacements, and which.
    with_edits, header, said = record("edited", "--unpacked", edited)
    assert "replaced=3" in said, said
    line = next(line for line in header if line.startswith("unpacked "))
    assert re.fullmatch(r"unpacked 2 [0-9a-f]{64}", line), header
    identity = line.split()[2]
    replays(with_edits, "--unpacked", edited)
    replays(with_edits, "--unpacked", relisted)     # another file, the same replacements
    for flags in ((), ("--unpacked", watching)):
        said = refused(with_edits, "the record was played with a pack of unpacked data that replaces", *flags)
        assert f"record 2 replacements, {identity}" in said and "replay nothing replaced" in said, said
    said = refused(with_edits, "the record was played with another pack of unpacked data", "--unpacked", others)
    assert f"record 2 replacements, {identity}" in said and re.search(r"replay 4 replacements, [0-9a-f]{64}", said), said
    said = record_tool("info", with_edits)
    assert f"played with a pack of unpacked data, and replays only with it (--unpacked): 2 replacements, " \
           f"{identity}" in said, said
    said = record_tool("verify", with_edits, "--game", game, expect=1)
    assert "the record was played with a pack of unpacked data" in said and "1 segment(s) did not replay" in said, said
    states = root / "states-edited"
    said = record_tool("verify", with_edits, "--game", game, "--states", states, "--state-frames", 60, "--",
                       "--unpacked", edited)
    assert "ended where the recording did" in said and "the run replays" in said, said
    # In pieces, from the states: each piece is the same run, and wants the same pack.
    said = record_tool("sweep", with_edits, "--game", game, "--states", states, "--out", root / "pieces-edited",
                       "--", "--unpacked", edited)
    assert "2 of 2 pieces replayed" in said, said
    said = record_tool("sweep", with_edits, "--game", game, "--states", states, "--out", root / "pieces-none",
                       expect=1)
    assert "0 of 2 pieces replayed" in said and "the record was played with a pack of unpacked data" in said, said

    # Played with nothing replaced: with no pack, and with one that only watches, harvesting.
    plain, header, _ = record("plain")
    assert "unpacked 0 -" in header, header
    harvested = root / "run-harvest.json"
    watched, header, said = record("watched", "--unpacked", watching, "--unpacked-harvest", harvested)
    assert "unpacked 0 -" in header and "replaced=0" in said and things_made(harvested) == 6, (header, said)
    for folder in (plain, watched):
        replays(folder)
        replays(folder, "--unpacked", watching)
        harvested.unlink()
        replays(folder, "--unpacked", watching, "--unpacked-harvest", harvested)
        assert things_made(harvested) == 6
        said = refused(folder, "the record was played with nothing replaced", "--unpacked", edited)
        assert "record nothing replaced" in said and f"replay 2 replacements, {identity}" in said, said
    # Which is what a recorded run is swept with, to have what the game unpacked while it was played.
    states, pieces = root / "states-plain", root / "pieces-plain"
    record_tool("verify", plain, "--game", game, "--states", states, "--state-frames", 60)
    said = record_tool("sweep", plain, "--game", game, "--states", states, "--out", pieces, "--", "--unpacked",
                       watching, "--unpacked-harvest", "{out}.json")
    assert "2 of 2 pieces replayed" in said, said
    assert [things_made(pieces / f"segment-0001-f{frame:09d}.json") for frame in (0, 60)] == [6, 0]

    # A record from before records had the line cannot be told: it is replayed as it is given, the
    # game says what it does not know, and the checkpoints find a replay that is not the run.
    def with_line(name: str, line: str | None) -> Path:
        folder = root / f"run-{name}"
        folder.mkdir()
        text = (with_edits / "segment-0001.run").read_text(encoding="ascii")
        old = next(each for each in text.splitlines() if each.startswith("unpacked "))
        (folder / "segment-0001.run").write_text(text.replace(old + "\n", f"{line}\n" if line else ""),
                                                 encoding="ascii", newline="")
        return folder

    older = with_line("older", None)
    assert "the record does not say what pack of unpacked data it was played with" in \
        replays(older, "--unpacked", edited)
    result, said = replay(older, expect=1)
    assert result["end"] == "diverged" and "the replay left the record in frame 60" in said, said
    for name, line in (("no-hash", "unpacked 2 -"), ("no-count", f"unpacked 0 {identity}"), ("short", "unpacked 2")):
        refused(with_line(name, line), "the record's line about a pack of unpacked data cannot be read",
                "--unpacked", edited)


def planes_by_themselves() -> None:
    """A picture in bit planes, by columns or by rows, drawn and read back, is the same bytes;
    its size can come from the stream; and two things are twins only where the bytes say how."""
    noise = random.Random(7)
    for order in ("columns", "rows"):
        shape = {"kind": "planes", "order": order, "tiles_wide": 3, "tiles_high": 2}
        sizes = [60, 60]
        made = noise.randbytes(120)
        across, down, colours = gb_unpacked.to_colours(made, sizes, shape)
        assert (across, down, len(colours)) == (24, 16, 24 * 16) and max(colours) == 3
        assert gb_unpacked.from_colours(colours, made, sizes, shape) == made
        blank = gb_unpacked.from_colours(bytes(len(colours)), made, sizes, shape)
        # What the picture does not show, the last twelve bytes of each plane, stays as it was made.
        assert blank[:48] == bytes(48) and blank[48:60] == made[48:60] and blank[108:] == made[108:]
    one = gb_unpacked.to_colours(bytes([0x80]) + bytes(15), [16], {"kind": "planes", "order": "columns",
                                                                  "tiles_wide": 2, "tiles_high": 1})
    assert one[2][0] == 1 and sum(one[2]) == 1          # the first byte's high bit is the top left pixel
    by_rows = gb_unpacked.to_colours(bytes([0, 0x01]) + bytes(14), [16], {"kind": "planes", "order": "rows",
                                                                          "tiles_wide": 2, "tiles_high": 1})
    assert by_rows[2][15] == 1 and sum(by_rows[2]) == 1  # by rows the second byte is the top right
    by_columns = gb_unpacked.to_colours(bytes([0, 0x01]) + bytes(14), [16], {"kind": "planes", "order": "columns",
                                                                             "tiles_wide": 2, "tiles_high": 1})
    assert by_columns[2][16 + 7] == 1                    # by columns it is the second line of the first column
    unpacker = {"name": "x", "made": [{"address": 0xC000, "size": 392}],
                "as": {"kind": "planes", "order": "columns", "tiles_wide": {"stream": 0, "shift": 4, "mask": 15},
                       "tiles_high": {"stream": 0, "mask": 15}}}
    rom = bytearray(0x8000)
    rom[0x4321] = 0x57
    shape = gb_unpacked.resolve_shape(unpacker, bytes(rom), 1, 0x4321)
    assert (shape["tiles_wide"], shape["tiles_high"]) == (5, 7), shape
    rom[0x4321] = 0x88                                    # eight tiles by eight is more than a plane holds
    try:
        gb_unpacked.resolve_shape(unpacker, bytes(rom), 1, 0x4321)
        raise AssertionError("a picture bigger than what is made was drawn")
    except gb_unpacked.UnpackedError:
        pass
    plain = unpacked(PACKED1)
    assert gb_unpacked.bit_moves(plain, halves_changed(plain)) == [4, 5, 6, 7, 0, 1, 2, 3]
    assert gb_unpacked.move_bits(plain, [4, 5, 6, 7, 0, 1, 2, 3]) == halves_changed(plain)
    assert gb_unpacked.bit_moves(plain, bytes(value ^ 0xFF for value in plain)) is None      # not a move of bits
    assert gb_unpacked.bit_moves(unpacked(PACKED2), halves_changed(unpacked(PACKED2))) is None  # which bit is which?


def harvested_over_a_run(game: Path, root: Path, watch_pack: Path, replacing: Path) -> None:
    """A run recorded with no pack, played again and swept with a pack that only watches, for what
    the game unpacked while it was played. `replacing` is the pack `build` writes from the same
    tree, which has an edited picture in it: with that the replay is not the run recorded."""
    saves = root / "saves-over-a-run"
    saves.mkdir()
    folder = root / "run-over"
    run([str(game), "--headless", "--record-run", str(folder), "--limit-frames", str(RUN_FRAMES)], timeout=180)

    def replay(*flags: object, expect: int = 0) -> str:
        result = run([str(game), "--headless", "--replay-run", str(folder / "segment-0001.run"), "--save-dir",
                      str(saves), *map(str, flags)], expect=(expect,), timeout=180)
        return result.stdout + result.stderr

    def record_tool(*arguments: object) -> str:
        result = run([sys.executable, str(RUN_RECORD), *map(str, arguments)])
        return result.stdout + result.stderr

    def things_made(path: Path) -> int:
        return len(json.loads(path.read_text(encoding="utf-8"))["made"])

    assert " end=matched " in replay()
    harvest = root / "over-a-run.json"
    said = replay("--unpacked", watch_pack, "--unpacked-harvest", harvest, "--report-unpacked")
    assert " end=matched " in said and " checkpoints=2 matched=2 " in said and "replaced=0" in said, said
    assert things_made(harvest) == 6
    said = replay("--unpacked", replacing, expect=1)
    assert "[RUN] " in said and " end=matched " not in said, said
    # In pieces, from save states, as a long run is swept.
    states, pieces = root / "states-over-a-run", root / "pieces-over-a-run"
    record_tool("verify", folder, "--game", game, "--states", states, "--state-frames", 60)
    said = record_tool("sweep", folder, "--game", game, "--states", states, "--out", pieces, "--", "--unpacked",
                       watch_pack, "--unpacked-harvest", "{out}.json")
    assert "2 of 2 pieces replayed" in said, said
    assert [things_made(pieces / f"segment-0001-f{frame:09d}.json") for frame in (0, 60)] == [6, 0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    planes_by_themselves()
    plain1, plain2, plain4 = unpacked(PACKED1), unpacked(PACKED2), unpacked(PACKED4)
    other1, other2, other4 = halves_changed(plain1), halves_changed(plain2), halves_changed(plain4)
    made_as = {(1, False): plain1, (1, True): other1, (2, False): plain2, (2, True): other2, (4, False): plain4,
               (4, True): other4}

    def drawn(**instead: bytes) -> list[bytes]:
        """The tiles a run should draw, with what the routine makes replaced where it is said to be."""
        names = {(1, False): "plain1", (1, True): "other1", (2, False): "plain2", (2, True): "other2"}
        return [instead.get(names.get((stream, way), ""), made_as[stream, way]) for stream, way, _ in STEPS]

    with tempfile.TemporaryDirectory(prefix="gbpacked-") as scratch:
        root = Path(scratch)
        rom = make_rom()
        rom_path = root / "packed.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        run([str(args.gbrecomp), str(rom_path), "--no-scan", "--pointer-scan", "off", "--lift-export",
             str(export_path)])
        game = build_game(args.gbrecomp, rom_path, root / "game", args.cmake_arg)
        end, tiles, _ = play(game, root, "plain")
        assert tiles == drawn(), [tile.hex() for tile in tiles]

        # The tree, the declaration in it, and the pack `build` writes beside the image.
        tree = root / "tree"
        tool("split", export_path, "--rom", rom_path, "-o", tree)
        stderr = tool("unpacked", tree, root / "none.json", expect=(2,)).stderr
        assert "unpackers.json is not there" in stderr, stderr
        watch_path = root / "watch.gbup"
        stderr = tool("watch-pack", tree, "-o", watch_path, expect=(2,)).stderr
        assert "unpackers.json is not there" in stderr and not watch_path.exists(), stderr
        (tree / "unpackers.json").write_text(json.dumps(DECLARATION), encoding="utf-8")
        image_path, pack_path = root / "image.gb", root / "image.gbup"
        said = tool("build", tree, "-o", image_path, "--expect-original").stdout
        assert "image.gbup: 1 routine to watch, and 0 replacements" in said, said
        assert image_path.read_bytes() == bytes(rom)
        watching = pack_path.read_bytes()
        # With nothing edited, the pack that only watches is the pack `build` writes.
        tool("watch-pack", tree, "-o", watch_path)
        assert watch_path.read_bytes() == watching
        watch_path.unlink()

        # Watching changes nothing, and says what the routine made.
        harvest_path = root / "harvest.json"
        watched, tiles, said = play(game, root, "watched", "--unpacked", pack_path, "--unpacked-harvest",
                                    harvest_path, "--report-unpacked")
        assert tiles == drawn()
        differ = [key for key in end if key != "dispatch_fallbacks" and end[key] != watched.get(key)]
        assert not differ, differ
        assert "tiles at 00:0300: entered=7 returned=7 replaced=0 made_something_else=0 not_in_reach=0" in said, said
        harvest = json.loads(harvest_path.read_text(encoding="utf-8"))
        assert harvest["image_changed"] == 0 and harvest["dropped"] == 0
        assert harvest["unpackers"] == [{"name": "tiles", "bank": 0, "address": UNPACK, "entered": 7,
                                         "returned": 7, "not_in_reach": 0}], harvest["unpackers"]
        made = sorted((thing["bank"], thing["address"], thing["bytes"], thing["times"]) for thing in harvest["made"])
        assert made == sorted([(1, STREAM1, plain1.hex(), 2), (1, STREAM1, other1.hex(), 1),
                               (1, STREAM2, plain2.hex(), 1), (1, STREAM2, other2.hex(), 1),
                               (1, STREAM4, plain4.hex(), 1), (1, STREAM4, other4.hex(), 1)]), made
        # The same harvest as somebody would have it who never saw the second stream unpacked the second way.
        partial = json.loads(harvest_path.read_text(encoding="utf-8"))
        partial["made"] = [thing for thing in partial["made"] if thing["bytes"] != other2.hex()]
        partial_path = root / "partial.json"
        partial_path.write_text(json.dumps(partial), encoding="utf-8")
        _, _, said = play(game, root, "no-pack", "--unpacked-harvest", harvest_path, expect=(1,))
        assert "--unpacked-harvest needs --unpacked FILE too" in said, said
        _, _, said = play(game, root, "differential", "--unpacked", pack_path, "--differential", "100", expect=(1,))
        assert "--unpacked is for normal execution" in said, said

        # Into the tree: five pictures, of three streams, two of them unpacked two ways. And the
        # third stream's second way, which is in no harvest, worked out from the other two.
        said = tool("unpacked", tree, partial_path).stdout
        assert "1 harvest: 5 different things made, of 3 streams" in said and "5 new in" in said, said
        assert "2 streams are unpacked two ways" in said and "1 more was worked out" in said, said
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        things = {(tree / thing["made"]).read_bytes(): thing for thing in manifest["unpacked"]}
        assert set(things) == {plain1, other1, plain2, other2, plain4, other4}
        assert sorted(thing["file"] for thing in things.values()) == \
            ["unpacked/tiles/01_4000.png", "unpacked/tiles/01_4000_2.png", "unpacked/tiles/01_4010.png",
             "unpacked/tiles/01_4010_2.png", "unpacked/tiles/01_4030.png", "unpacked/tiles/01_4030_2.png"]
        assert [made for made, thing in things.items() if thing.get("worked_out")] == [other2]
        assert things[other2]["file"] == "unpacked/tiles/01_4010_2.png"
        assert things[plain2]["twin"]["made_sha256"] == things[other2]["made_sha256"]
        assert things[plain1]["twin"]["made_sha256"] == things[other1]["made_sha256"]
        assert gb_unpacked.move_bits(other1, things[plain1]["twin"]["bits"]) == plain1
        assert gb_unpacked.move_bits(plain1, things[other1]["twin"]["bits"]) == other1
        picture = tree / things[plain1]["file"]
        width, height, grey = gb_data.read_png(picture.name, picture.read_bytes())
        assert (width, height) == (8, 8) and gb_data.read_tiles(grey, 1, 2, 1) == plain1
        said = tool("status", tree).stdout
        assert "and 6 things the game unpacks" in said and "nothing edited" in said, said
        # A folder of harvests is the harvests in it.
        folder = root / "harvests"
        folder.mkdir()
        for name in ("one.json", "two.json"):
            (folder / name).write_bytes(partial_path.read_bytes())
        again = tool("unpacked", tree, folder).stdout
        assert "2 harvests: 5 different things made" in again and "0 new in" in again and \
            "5 there already and left as they are" in again and "worked out" not in again, again
        (root / "empty").mkdir()
        stderr = tool("unpacked", tree, root / "empty", expect=(2,)).stderr
        assert "there is no harvest in what was given" in stderr, stderr
        said = tool("build", tree, "-o", image_path, "--expect-original").stdout
        assert "and 0 replacements" in said and pack_path.read_bytes() == watching, said

        # One picture edited: every colour to its opposite. Its twin follows it.
        picture.write_bytes(gb_data.write_png(width, height, bytes(255 - level for level in grey)))
        wanted = bytes(value ^ 0xFF for value in plain1)
        said = tool("status", tree).stdout
        assert "no file of the ROM edited" in said and f"edited: {things[plain1]['file']} (unpacked by tiles from " \
            f"01:4000, 16 of 16 bytes changed)" in said, said
        said = tool("build", tree, "-o", image_path, "--expect-original").stdout
        assert "and 2 replacements" in said and "it follows its twin" in said, said
        assert image_path.read_bytes() == bytes(rom)
        # A pack that only watches, from this tree with its picture edited: the pack `build` wrote
        # before anything was edited, byte for byte, and said to replace nothing. The pack `build`
        # wrote just now is left as it is. A run recorded with nothing replaced is harvested with
        # the first, and is another run with the second.
        said = tool("watch-pack", tree, "-o", watch_path).stdout
        assert "watch.gbup: a pack that only watches. 1 routine to watch, and it replaces nothing" in said, said
        assert watch_path.read_bytes() == watching != pack_path.read_bytes()
        harvested_over_a_run(game, root, watch_path, pack_path)
        edited, tiles, said = play(game, root, "edited", "--unpacked", pack_path, "--report-unpacked")
        assert tiles == drawn(plain1=wanted, other1=halves_changed(wanted)), [tile.hex() for tile in tiles]
        assert "entered=7 returned=7 replaced=3 made_something_else=0" in said, said
        # Nothing else is different: the same registers and counters, and of memory only the buffer.
        differ = {key: [index for index, (old, new) in enumerate(zip(end[key], edited[key])) if old != new]
                  if isinstance(end[key], list) else None
                  for key in end if key != "dispatch_fallbacks" and end[key] != edited.get(key)}
        assert list(differ) == ["wram_bank_0_c000_cfff"] and \
            differ["wram_bank_0_c000_cfff"] == list(range(0x100, 0x110)), differ

        edited_pack = pack_path.read_bytes()

        # The same with lifted source, which calls, jumps and returns by other ways.
        lifted = root / "lifted"
        run([sys.executable, str(GB_LIFT), "baseline", str(export_path), "--all", "-o", str(lifted)])
        package = root / "package"
        run([sys.executable, str(GB_LIFT), "package", str(export_path), "--source", str(lifted), "-o",
             str(package)])
        lifted_game = build_game(args.gbrecomp, rom_path, root / "lifted-game", args.cmake_arg, "--lift",
                                 str(package / "lift.json"))
        _, tiles, said = play(lifted_game, root, "lifted", "--unpacked", pack_path, "--report-unpacked",
                              "--report-lift")
        assert tiles == drawn(plain1=wanted, other1=halves_changed(wanted)), [tile.hex() for tile in tiles]
        assert "entered=7 returned=7 replaced=3" in said and "enabled=1" in said and "calls=0 " not in said, said
        _, tiles, _ = play(lifted_game, root, "lifted-plain")
        assert tiles == drawn()

        # An image that changes what the routine makes of a stream: its replacement is not for that.
        region = next(entry for entry in manifest["regions"]
                      if entry["offset"] <= STREAM1 < entry["offset"] + entry["size"])
        assert region["kind"] == "data", region
        data = bytearray((tree / region["file"]).read_bytes())
        data[STREAM1 - region["offset"] + 1] = 0x99                         # the first run's byte
        (tree / region["file"]).write_bytes(data)
        changed_image = root / "changed.gb"
        tool("build", tree, "-o", changed_image)
        changed_harvest = root / "changed-harvest.json"
        _, tiles, said = play(game, root, "changed", "--rom-image", changed_image, "--unpacked",
                              root / "changed.gbup", "--report-unpacked", "--unpacked-harvest", changed_harvest)
        mine = bytes([0x99]) * 4 + plain1[4:]
        assert tiles == drawn(plain1=mine, other1=halves_changed(mine)), [tile.hex() for tile in tiles]
        assert "replaced=0 made_something_else=3" in said, said
        stderr = tool("unpacked", tree, changed_harvest, expect=(2,)).stderr
        assert "was harvested from a game given an edited image" in stderr, stderr
        data[STREAM1 - region["offset"] + 1] = PACKED1[1]
        (tree / region["file"]).write_bytes(data)
        tool("build", tree, "-o", image_path, "--expect-original")

        # Packs the game will not take.
        pack = pack_path.read_bytes()

        def refused(name: str, content: bytes, words: str, *flags: object) -> None:
            path = root / f"{name}.gbup"
            path.write_bytes(content)
            _, _, said = play(game, root, name, "--unpacked", path, *flags, expect=(1,))
            assert words in said, (name, said)

        refused("other-rom", pack[:12] + bytes([pack[12] ^ 1]) + pack[13:], "it is of another ROM")
        refused("short", pack[:-1], "it is cut short")
        refused("long", pack + b"\x00", "there is more in it than it says")
        refused("not-a-pack", b"GBTP" + pack[4:], "it is not a pack of unpacked data")
        refused("recorded", pack, "--data-use records the game as it is", "--data-use", root / "use.json")
        _, _, said = play(game, root, "watched-and-recorded", "--unpacked", root / "none.gbup", expect=(1,))
        assert "cannot open it" in said, said

        # What the tool will not take.
        def tool_refuses(words: str, *arguments: object) -> None:
            stderr = tool(*arguments, expect=(2,)).stderr
            assert words in stderr, (words, stderr)

        other = json.loads(harvest_path.read_text(encoding="utf-8"))
        other["rom"]["sha256"] = "00" * 32
        (root / "other.json").write_text(json.dumps(other), encoding="utf-8")
        tool_refuses("was harvested from another ROM", "unpacked", tree, root / "other.json")
        edited_png = picture.read_bytes()
        picture.write_bytes(gb_data.write_png(16, 8, bytes(128)))
        tool_refuses("a picture keeps its size", "build", tree, "-o", root / "no.gb")
        watch_path.unlink()
        tool("watch-pack", tree, "-o", watch_path)       # which reads no picture, so this one is not in its way
        assert watch_path.read_bytes() == watching
        picture.write_bytes(edited_png)
        made_path = tree / things[plain1]["made"]
        made_path.write_bytes(bytes(16))
        tool_refuses("is not what the game's routine made", "build", tree, "-o", root / "no.gb")
        made_path.write_bytes(plain1)
        good = (tree / "manifest.json").read_text(encoding="utf-8")
        for change, words in (
                (lambda thing: thing.update(colour=1), "has no such field as 'colour'"),
                (lambda thing: thing.pop("made_sha256"), "lacks the field 'made_sha256'"),
                (lambda thing: thing.update(file="../x.png"), "is not a file inside"),
                (lambda thing: thing.update(twin={"made_sha256": "", "bits": [0] * 8}), "has a twin that is not one"),
                (lambda thing: thing.update(bank="1"), "a field of the wrong type")):
            altered = json.loads(good)
            change(altered["unpacked"][0])
            (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
            tool_refuses(words, "status", tree)
        (tree / "manifest.json").write_text(good, encoding="utf-8", newline="\n")
        (tree / "unpackers.json").rename(tree / "kept.json")
        tool_refuses("unpackers.json is gone", "status", tree)
        (tree / "kept.json").rename(tree / "unpackers.json")
        bad = json.loads(json.dumps(DECLARATION))
        bad["unpackers"][0]["made"][0]["address"] = 0x4000
        (tree / "unpackers.json").write_text(json.dumps(bad), encoding="utf-8")
        tool_refuses("which is not all RAM", "build", tree, "-o", root / "no.gb")
        bad["unpackers"][0]["made"][0]["address"] = {"register": "ix"}
        (tree / "unpackers.json").write_text(json.dumps(bad), encoding="utf-8")
        tool_refuses("it has to be a number, or one of", "build", tree, "-o", root / "no.gb")
        (tree / "unpackers.json").write_text(json.dumps(DECLARATION), encoding="utf-8")

        # The other stream, which the routine leaves by its other return, edited as well: the left
        # half of the tile black and the right half white. Its second way, which was worked out and
        # never harvested, follows it, and is what the game makes.
        second = tree / things[plain2]["file"]
        second_png = second.read_bytes()
        second.write_bytes(gb_data.write_png(8, 8, bytes([0, 0, 0, 0, 255, 255, 255, 255]) * 8))
        halved = bytes([0xF0]) * 16
        said = tool("build", tree, "-o", image_path, "--expect-original").stdout
        assert "and 4 replacements" in said, said
        _, tiles, said = play(game, root, "both", "--unpacked", pack_path, "--report-unpacked")
        assert tiles == drawn(plain1=wanted, other1=halves_changed(wanted), plain2=halved,
                              other2=bytes([0x0F]) * 16), [tile.hex() for tile in tiles]
        assert "replaced=5" in said, said
        second.write_bytes(second_png)

        # Runs recorded with these packs. The same two replacements in a pack that lists them the
        # other way round, for a routine of another name, after a routine it only watches, are the
        # same replacements to a record.
        rom_sha256 = hashlib.sha256(rom).hexdigest()
        entries = [(0, 1, STREAM1, hashlib.sha256(made).digest(), instead)
                   for made, instead in ((plain1, wanted), (other1, halves_changed(wanted)))]
        assert gb_unpacked.pack_bytes(len(rom), rom_sha256, DECLARATION["unpackers"], entries) == edited_pack
        idle = {"name": "idle", "function": {"bank": 0, "address": 0x0200},
                "stream": {"bank": 1, "address": {"register": "de"}}, "made": [{"address": 0xC200, "size": 4}]}
        relisted = gb_unpacked.pack_bytes(len(rom), rom_sha256, [idle, dict(DECLARATION["unpackers"][0], name="other")],
                                          [(1, *entry[1:]) for entry in entries])
        one = 6 + 32 + 4 + 16                             # an entry: which stream, the hash, the size, the bytes
        packs = {"watching": watching, "edited": edited_pack, "others": pack_path.read_bytes(),
                 "relisted": relisted[:-2 * one] + relisted[-one:] + relisted[-2 * one:-one]}
        for name, content in packs.items():
            (root / f"{name}.gbup").write_bytes(content)
        recorded_runs(game, root, *(root / f"{name}.gbup" for name in ("watching", "edited", "relisted", "others")))

        # An edit stays where `unpacked` is run again, and goes where it is forced. And what was
        # worked out is, once a harvest has it, something the game was seen to make.
        said = tool("unpacked", tree, harvest_path).stdout
        assert "6 different things made" in said and "0 new in" in said and "worked out" not in said, said
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        assert not any(thing.get("worked_out") for thing in manifest["unpacked"])
        assert picture.read_bytes() == edited_png
        tool("unpacked", tree, harvest_path, "--force")
        said = tool("status", tree).stdout
        assert "nothing edited" in said, said
        said = tool("build", tree, "-o", image_path, "--expect-original").stdout
        assert "and 0 replacements" in said and pack_path.read_bytes() == watching, said

    print("data unpacked end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
