#!/usr/bin/env python3
"""The game unpacking streams nobody played as far as, end to end.

Builds a small game on a cartridge with banks. Its routine unpacks run-length
streams from the bank that is mapped into a buffer of 32 bytes, two bit
planes of a picture whose size is the stream's first byte, and each byte with
its halves changed about when a byte of RAM says so. Play unpacks two of the
streams at once and two more after thirty frames. The others are never
unpacked: they lie after the played ones, in other places a list names, and
behind two tables of pointers.

- A game given `--unpacked-call` has to enter the function, stop as the
  routine returns, and keep what it made: recompiled, interpreted and lifted,
  in a save state and in the game as it is switched on. It has to say so when
  the function comes back without the routine, when nothing returns in the
  cycles given, and when the call cannot be made. A game not given the option
  ends as it always did.
- `gb_data.py unpack` has to have the game unpack the streams of a list and of
  the tables declared, each way the declaration has, and write harvests that
  `unpacked` takes as they are: what a called routine made of a stream is
  what the played game made of it.
- Walking on, it has to find the streams that follow known ones, tell the
  ones whose chain arrives at another known stream from the ones that are
  only guessed, and end each chain for the reason there is: a picture of no
  size, bytes play read as something else, a known stream run over, a routine
  that does not return, padding, the end of the bank.
- A picture that was only ever unpacked this way, edited, has to be in the
  game when play gets as far as it.
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
GB_LIFT = ROOT / "tools" / "gb_lift.py"
sys.path.insert(0, str(ROOT / "tools"))
import gb_data  # noqa: E402  (its PNG reader and writer)
import gb_data_unpack  # noqa: E402
import gb_discover  # noqa: E402  (the save state reader)
import gb_unpacked  # noqa: E402

START = 0x0150
UNPACK, CALLER, RETURNS, LOOPS, DARK, COUNTS, HANDLER, TABLE = 0x0300, 0x0340, 0x0350, 0x0358, 0x0360, 0x0368, \
    0x0370, 0x0380
BUFFER, SECOND_WAY, COUNTED = 0xC100, 0xC0F0, 0xC0F8
FRAMES, LATER = 20, 70          # play that stops before the game unpacks its later pictures, and play that does not
CYCLES = 30000                  # what a call waits for the routine: a stream takes a tenth of that


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def packed(header: int, *runs: tuple[int, int]) -> bytes:
    """A stream: the picture's size in tiles, then a count and a byte for each run, to a count of nought."""
    return bytes([header, *[value for run in runs for value in run], 0])


def unpacked(stream: bytes) -> bytes:
    out = bytearray()
    at = 1
    while stream[at]:
        out += bytes([stream[at + 1]]) * stream[at]
        at += 2
    assert len(out) == 32, stream.hex()
    return bytes(out)


def halves_changed(data: bytes) -> bytes:
    return bytes(((value << 4) | (value >> 4)) & 0xFF for value in data)


# Where the streams lie: (bank, address) -> its bytes. Then what lies after some of them.
S1, S2, S3, S4, S5, S6 = (2, 0x4000), (2, 0x400A), (2, 0x4014), (2, 0x401A), (2, 0x4022), (2, 0x402C)
T1, U1, U2, W1, Y1, P1, V1, V2, X1 = (2, 0x4100), (2, 0x4200), (2, 0x420A), (2, 0x4300), (2, 0x4400), \
    (2, 0x4480), (2, 0x7FF8), (2, 0x7FFC), (3, 0x4000)
STREAMS = {
    S1: packed(0x21, (8, 0x55), (8, 0x66), (8, 0x78), (8, 0x80)),       # play unpacks it at once
    S2: packed(0x21, (8, 0x80), (8, 0x78), (8, 0x66), (8, 0x55)),       # and this one later
    S3: packed(0x12, (16, 0x0F), (16, 0xF1)),
    S4: packed(0x21, (4, 0x12), (12, 0x34), (16, 0x56)),                # at once
    S5: packed(0x11, (8, 0x9A), (8, 0xBC), (8, 0x55), (8, 0x66)),       # later
    S6: packed(0x21, (32, 0xE7)),
    T1: packed(0x21, (16, 0xA5), (16, 0x5A)),
    U1: packed(0x12, (16, 0x13), (16, 0x24)),
    U2: packed(0x21, (4, 0x45), (28, 0x76)),
    W1: packed(0x21, (32, 0x3C)),
    Y1: packed(0x21, (32, 0xC3)),
    P1: packed(0x21, (32, 0x9B)),
    V1: packed(0x21, (32, 0x18)),
    V2: packed(0x12, (32, 0x81)),                                       # its last byte is the bank's last
    X1: packed(0x21, (16, 0x01), (16, 0xFE)),                           # and nothing but noughts after this one
}
NO_PICTURE = (2, 0x4030)        # after S6: a size no picture has
TEXT = (2, 0x4106)              # after T1: a stream to look at, which play copies to the screen as it is
RUN_OVER = (2, 0x4206)          # after U1: bytes that unpack on into U2
LONG = (2, 0x4404)              # after Y1: more than the routine unpacks in the cycles it is given
TABLE2 = (2, 0x4484)            # after P1: a table of pointers, which has no size of a picture either
OTHER = {
    NO_PICTURE: bytes([0x99, 0x01, 0x02, 0x00]),
    TEXT: packed(0x11, (8, 0x3C), (8, 0x42), (16, 0x7E)),
    RUN_OVER: bytes([0x11, 0x02, 0x33, 0x02]),
    (2, 0x4304): bytes([0xFF]) * 20,                                    # after W1: padding
    LONG: bytes([0x21, 0xFF, 0x01, 0xFF, 0x02, 0xFF, 0x03, 0xFF, 0x04, 0xFF, 0x05, 0x00]),
    TABLE2: bytes([*word(U1[1]), *word(P1[1]), *word(T1[1])]),
}
LISTED = (T1, U1, U2, W1, Y1, V1)
# What play draws, in order: the stream, and whether it is before the thirty frames or after.
PLAYED = ((S1, False), (S4, False), (S2, True), (S5, True))


def offset(place: tuple[int, int]) -> int:
    return place[0] * 0x4000 + place[1] - 0x4000


def copy(source: int, target: int, count: int) -> list[int]:
    """ld hl,source / ld de,target / ld b,count / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop"""
    return [0x21, *word(source), 0x11, *word(target), 0x06, count, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]


def make_rom() -> bytearray:
    rom = bytearray(0x10000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13F] = b"DATA UNPACK"
    rom[0x147], rom[0x148] = 0x01, 0x01                                    # a cartridge with four banks
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    screen_off = [0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xAF, 0xE0, 0x40]    # wait for line 144, then xor a / ldh [$40],a
    screen_on = [0x3E, 0x91, 0xE0, 0x40]                                   # ld a,$91 / ldh [$40],a
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF
    code += screen_off
    code += [0x3E, 0x02, 0xEA, *word(0x2000), 0xAF, 0xEA, *word(SECOND_WAY)]  # bank 2 mapped, the plain way
    for tile, (stream, later) in enumerate(PLAYED):
        if later and not PLAYED[tile - 1][1]:
            make_rom.text_reader = START + len(code) + 8                   # the ld a,[hli] of this copy
            code += copy(TEXT[1], 0x8100, len(OTHER[TEXT])) + screen_on
            # Thirty frames: ld b,30 / wait for line 144 / wait for it to pass / dec b / jr nz
            code += [0x06, 30, 0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xF0, 0x44, 0xFE, 0x90, 0x28, 0xFA, 0x05, 0x20,
                     0xF1]
            code += screen_off
        if stream == S4:
            code += [0x11, *word(stream[1]), 0xCD, *word(CALLER)]          # ld de,Stream / call Caller
        else:
            code += [0x21, *word(stream[1]), 0xCD, *word(UNPACK)]          # ld hl,Stream / call Unpack
        code += copy(BUFFER, 0x8000 + 32 * tile, 32)
    code += screen_on + [0x76, 0x18, 0xFD]                                 # halt, for good: nothing wakes it
    assert START + len(code) <= UNPACK
    rom[START:START + len(code)] = bytes(code)
    rom[UNPACK:UNPACK + 26] = bytes([
        0x11, *word(BUFFER),            # Unpack: ld de,Buffer        the stream in HL, in the bank that is mapped
        0x2A,                           #         ld a,[hli]          the picture's size, which it has no use for
        0x2A, 0xB7, 0xC8,               # next:   ld a,[hli] / or a / ret z
        0x47, 0x2A, 0x4F,               #         ld b,a / ld a,[hli] / ld c,a
        0xFA, *word(SECOND_WAY),        # fill:   ld a,[SecondWay]
        0xB7, 0x79, 0x28, 0x02,         #         or a / ld a,c / jr z,plain
        0xCB, 0x37,                     #         swap a
        0x12, 0x13, 0x05, 0x20, 0xF2,   # plain:  ld [de],a / inc de / dec b / jr nz,fill
        0x18, 0xEA])                    #         jr next
    rom[CALLER:CALLER + 5] = bytes([0x62, 0x6B, 0xC3, *word(UNPACK)])      # Caller: ld h,d / ld l,e / jp Unpack
    rom[RETURNS] = 0xC9                                                    # a function that unpacks nothing
    rom[LOOPS:LOOPS + 2] = bytes([0x18, 0xFE])                             # and one that never comes back
    rom[DARK:DARK + 5] = bytes([0xAF, 0xE0, 0x40, 0x18, 0xFE])             # nor this, which turns the screen off first
    # One that lets the frame's interrupt in and waits: ld a,1 / ldh [$FF],a / jr to itself. Its
    # handler counts: push af / ld a,[Counted] / inc a / ld [Counted],a / pop af / reti
    rom[COUNTS:COUNTS + 6] = bytes([0x3E, 0x01, 0xE0, 0xFF, 0x18, 0xFE])
    rom[0x40:0x43] = bytes([0xC3, *word(HANDLER)])
    rom[HANDLER:HANDLER + 10] = bytes([0xF5, 0xFA, *word(COUNTED), 0x3C, 0xEA, *word(COUNTED), 0xF1, 0xD9])
    # A table of pointers with the bank in front of each: X1, S1, and an entry that points at nothing.
    rom[TABLE:TABLE + 9] = bytes([X1[0], *word(X1[1]), S1[0], *word(S1[1]), 0xFF, 0xFF, 0xFF])
    for place, content in {**STREAMS, **OTHER}.items():
        assert not any(rom[offset(place):offset(place) + len(content)]), place
        rom[offset(place):offset(place) + len(content)] = content
    for first, second in ((S1, S2), (S2, S3), (S3, S4), (S4, S5), (S5, S6), (S6, NO_PICTURE), (T1, TEXT),
                          (U1, RUN_OVER), (RUN_OVER, U2), (Y1, LONG), (P1, TABLE2), (V1, V2)):
        content = STREAMS.get(first, OTHER.get(first))
        assert offset(first) + len(content) == offset(second), (first, second)
    assert offset(V2) + len(STREAMS[V2]) == 0xC000
    return rom


DECLARATION = {"schema": "gbrecomp.unpackers", "version": 1, "unpackers": [
    {"name": "tiles", "function": {"bank": 0, "address": UNPACK},
     "stream": {"bank": {"mapped_bank": True}, "address": {"register": "hl"}},
     "made": [{"address": BUFFER, "size": 16}, {"address": BUFFER + 16, "size": 16}],
     "as": {"kind": "planes", "order": "rows", "tiles_wide": {"stream": 0, "shift": 4, "mask": 15},
            "tiles_high": {"stream": 0, "mask": 15}},
     "call": {"function": {"bank": 0, "address": CALLER},
              "stream": {"bank": {"mapped_bank": True}, "address": {"register": "de"}},
              "set": [{"cartridge": 0x2000, "value": {"stream": "bank"}}, {"ram8": SECOND_WAY, "value": 0}],
              "variants": [{"name": "other", "set": [{"ram8": SECOND_WAY, "value": 1}]}],
              "cycles": CYCLES},
     "tables": [{"bank": 0, "address": TABLE, "entries": 3, "entry_size": 3, "pointer": 1,
                 "stream_bank": {"entry": 0}},
                {"bank": TABLE2[0], "address": TABLE2[1], "entries": 3}],
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
         "--output-prefix", "unpack_game"])
    run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"), "-DCMAKE_BUILD_TYPE=Release",
         *cmake_args])
    run(["ninja", "-C", str(project / "build")])
    return next(path for path in (project / "build" / "unpack_game.exe", project / "build" / "unpack_game")
                if path.exists())


def play(executable: Path, root: Path, name: str, *flags: object, frames: int = FRAMES,
         expect: tuple[int, ...] = (0,)) -> tuple[dict, list[bytes], str]:
    """A run of the game: how it ends, the pictures it drew, and what it said."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump, state = root / f"{name}.json", root / f"{name}.state"
    result = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(frames), "--save-dir",
                  str(saves), "--dump-state", str(dump), "--save-state-file", str(state), *map(str, flags)],
                 expect=expect, timeout=180)
    said = result.stdout + result.stderr
    if not dump.exists():
        return {}, [], said
    machine = gb_discover.Savestate.load(state)
    drawn = [bytes(machine.read(0x8000 + 32 * tile + index) for index in range(32)) for tile in range(len(PLAYED))]
    return json.loads(dump.read_text(encoding="utf-8")), drawn, said


def harvested(path: Path) -> list[tuple[int, int, str]]:
    """The things a harvest has: the stream's bank and address, and the bytes made, in hexadecimal."""
    harvest = json.loads(Path(path).read_text(encoding="utf-8"))
    assert harvest["schema"] == "gbrt.unpacked-harvest" and harvest["image_changed"] == 0, harvest
    return [(thing["bank"], thing["address"], thing["bytes"]) for thing in harvest["made"]]


def name_of(stream: tuple[int, int], variant: str = "") -> str:
    return f"{stream[0]:02X}_{stream[1]:04X}" + (f".{variant}" if variant else "") + ".json"


def by_themselves() -> None:
    """What the game is given for a call, the streams of a list and of a table, and where a stream ends."""
    unpacker = json.loads(json.dumps(DECLARATION["unpackers"][0]))
    argument = gb_data_unpack.call_argument
    assert argument(unpacker, S3) == "00:0340,de=4014,2000=02,C0F0=00", argument(unpacker, S3)
    assert argument(unpacker, X1, unpacker["call"]["variants"][0]) == "00:0340,de=4000,2000=03,C0F0=00,C0F0=01"
    # With no `call` the routine itself is entered, with the stream where its `stream` says it is found.
    plain = {"name": "x", "function": {"bank": 0, "address": 0x24FD},
             "stream": {"bank": {"register": "a"}, "address": {"ram16": 0xD0AB}}}
    assert argument(plain, (0x0D, 0x4123)) == "00:24FD,a=0D,D0AB=23,D0AC=41", argument(plain, (0x0D, 0x4123))
    plain["call"] = {"stream": {"bank": {"ram8": 0xFFB8}, "address": {"register": "bc"}}, "interrupts": True,
                     "set": [{"register": "hl", "value": {"stream": "address"}}, {"register": "e", "value": 7},
                             {"ram16": 0xC000, "value": 0x1234}, {"ram8": 0xC010, "value": {"stream": "bank"}}]}
    assert argument(plain, (0x0D, 0x4123)) == \
        "00:24FD,FFB8=0D,bc=4123,hl=4123,e=07,C000=34,C001=12,C010=0D,ime=1", argument(plain, (0x0D, 0x4123))
    plain["call"] = {"interrupts": False}
    assert argument(plain, (1, 0x4000)).endswith(",ime=0")
    for stream, call, words in (
            ({"bank": {"mapped_bank": True}, "address": {"register": "hl"}}, {}, "how this game maps one"),
            ({"bank": 5, "address": {"register": "hl"}}, {}, "is declared as 5"),
            ({"bank": {"register": "a"}, "address": {"register": "c"}}, {}, "does not fit in one"),
            ({"bank": {"register": "a"}, "address": {"ram8": 0xC000}}, {}, "does not fit in one")):
        try:
            argument({"name": "x", "function": {"bank": 0, "address": 0x100}, "stream": stream, "call": call},
                     (1, 0x4000))
            raise AssertionError(f"a call was made up that cannot be: {stream}")
        except gb_data_unpack.UnpackError as error:
            assert words in str(error), error
    assert argument({"name": "x", "function": {"bank": 0, "address": 0x100},
                     "stream": {"bank": 5, "address": {"register": "hl"}}}, (5, 0x4000)) == "00:0100,hl=4000"
    # What a declaration may say, and what it may not.
    gb_data_unpack.check_declared(unpacker, "x")
    for change, words in (
            (lambda call, tables: call.update(colour=1), "has no such field as 'colour'"),
            (lambda call, tables: call.update(function={"bank": 1, "address": 0x100}), "a function in the ROM"),
            (lambda call, tables: call.update(stream={"bank": 1}), "where the function takes the stream's bank"),
            (lambda call, tables: call["set"].append({"ram8": 0x100, "value": 0}), "no address of its kind"),
            (lambda call, tables: call["set"].append({"register": "ix", "value": 0}), "no register"),
            (lambda call, tables: call["set"].append({"ram8": 0xC000, "value": 256}), "its value does not fit"),
            (lambda call, tables: call["set"].append({"ram8": 0xC000, "value": {"stream": "address"}}),
             "an address does not fit in one byte"),
            (lambda call, tables: call["set"].append({"ram8": 0xC000}), "a thing to set is one of"),
            (lambda call, tables: call["set"].append({"cartridge": 0x8000, "value": 1}), "no address of its kind"),
            (lambda call, tables: call["variants"].append({"name": "other", "set": []}), "a name of its own"),
            (lambda call, tables: call.update(interrupts=1), "is true or false"),
            (lambda call, tables: call.update(cycles=0), "how many cycles to wait"),
            (lambda call, tables: tables.append({"bank": 0, "address": 0x100}), "'bank', 'address' and 'entries'"),
            (lambda call, tables: tables[0].update(pointer=2), "its pointer does not lie in an entry"),
            (lambda call, tables: tables[0].update(stream_bank={"entry": 3}), "for a byte of the entry"),
            (lambda call, tables: tables[1].update(address=0x100), "is not in the ROM")):
        altered = json.loads(json.dumps(unpacker))
        change(altered["call"], altered["tables"])
        try:
            gb_data_unpack.check_declared(altered, "x")
            raise AssertionError(f"a declaration was taken that says: {words}")
        except gb_data_unpack.UnpackError as error:
            assert words in str(error), (words, error)
    # A table's streams, with the bank from the entry, from the table's own bank, or given.
    rom = bytes(make_rom())
    assert gb_data_unpack.tabled_streams(rom, unpacker["tables"][0]) == ([X1, S1], 1)
    assert gb_data_unpack.tabled_streams(rom, unpacker["tables"][1]) == ([U1, P1, T1], 0)
    assert gb_data_unpack.tabled_streams(rom, {**unpacker["tables"][1], "stream_bank": 3})[0] == \
        [(3, U1[1]), (3, P1[1]), (3, T1[1])]
    # An address below 4000 is in bank 0, whatever bank the table's streams are said to be in.
    low = bytearray(rom)
    low[0x390:0x394] = bytes([*word(0x3000), *word(0x4000)])
    assert gb_data_unpack.tabled_streams(bytes(low), {"bank": 0, "address": 0x390, "entries": 2,
                                                      "stream_bank": 2}) == ([(0, 0x3000), (2, 0x4000)], 0)
    try:
        gb_data_unpack.tabled_streams(rom, {"bank": 3, "address": 0x7FFE, "entries": 2})
        raise AssertionError("a table that runs out of its bank was read")
    except gb_data_unpack.UnpackError as error:
        assert "does not lie in its bank" in str(error), error
    # Where a stream ends: the run of bytes read from its first on, whoever read them.
    runs = [(10, 12, (0, 1)), (12, 20, (0, 2)), (21, 30, (0, 3)), (40, 50, (0, 4))]
    assert gb_data_unpack.read_from(runs, 10) == (20, {(0, 1), (0, 2)})
    assert gb_data_unpack.read_from(runs, 15) == (20, {(0, 2)})
    assert gb_data_unpack.read_from(runs, 20) == (20, set()) and gb_data_unpack.read_from(runs, 21)[0] == 30
    # How many bytes of a stretch lie in files of code.
    manifest = {"regions": [{"offset": 0, "size": 100, "kind": "code"}, {"offset": 100, "size": 50, "kind": "data"},
                            {"offset": 150, "size": 10, "kind": "header"}, {"offset": 160, "size": 40, "kind": "data"}]}
    walk = gb_data_unpack.Walk(None, bytes(200), manifest, {}, {}, 1, 1)
    assert [walk.in_code(*stretch) for stretch in ((90, 110), (100, 150), (0, 200), (149, 161), (160, 200))] == \
        [10, 0, 110, 10, 0]
    # A guess the game could not unpack, one whose first byte the routine did not read, and one
    # it read on from out of the bank: none is a stream.
    assert walk.refuse((1, 0x4000), {"why": "the game said why"}) == "the game said why"
    assert walk.refuse((1, 0x4000), {"read": [(0x4001, 0x4004, (0, 1))]}) == "the routine did not read its first byte"
    assert walk.refuse((0, 0x3FFE), {"read": [(0x3FFE, 0x4002, (0, 1))]}) == "the routine read on out of its bank"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    by_themselves()
    made = {stream: unpacked(content) for stream, content in STREAMS.items()}

    def drawn(later: bool, **instead: bytes) -> list[bytes]:
        """The pictures a run should have drawn: nothing yet where the game unpacks later and stopped before."""
        return [instead.get(f"{stream[0]}_{stream[1]:04X}", made[stream]) if later or not after else bytes(32)
                for stream, after in PLAYED]

    with tempfile.TemporaryDirectory(prefix="gbunpack-") as scratch:
        root = Path(scratch)
        rom = make_rom()
        rom_path = root / "unpack.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        run([str(args.gbrecomp), str(rom_path), "--no-scan", "--pointer-scan", "off", "--lift-export",
             str(export_path)])
        game = build_game(args.gbrecomp, rom_path, root / "game", args.cmake_arg)
        end, pictures, _ = play(game, root, "plain")
        assert pictures == drawn(False), [picture.hex() for picture in pictures]
        state = root / "plain.state"
        later_end, pictures, _ = play(game, root, "later", frames=LATER)
        assert pictures == drawn(True), [picture.hex() for picture in pictures]

        # The tree, with what play shows: two streams harvested, and who read which bytes.
        tree = root / "tree"
        tool("split", export_path, "--rom", rom_path, "-o", tree)
        (tree / "unpackers.json").write_text(json.dumps(DECLARATION), encoding="utf-8")
        image_path, pack_path = root / "image.gb", root / "image.gbup"
        tool("build", tree, "-o", image_path, "--expect-original")
        watching = pack_path.read_bytes()
        # The pack is what it was before a declaration could say how to call: `call` and `tables` are not in it.
        bare = json.loads(json.dumps(DECLARATION))
        del bare["unpackers"][0]["call"], bare["unpackers"][0]["tables"]
        (tree / "unpackers.json").write_text(json.dumps(bare), encoding="utf-8")
        tool("build", tree, "-o", image_path, "--expect-original")
        assert pack_path.read_bytes() == watching
        (tree / "unpackers.json").write_text(json.dumps(DECLARATION), encoding="utf-8")
        played_harvest = root / "played.json"
        watched, _, said = play(game, root, "watched", "--unpacked", pack_path, "--unpacked-harvest", played_harvest)
        assert not [key for key in end if key != "dispatch_fallbacks" and end[key] != watched.get(key)]
        assert sorted(harvested(played_harvest)) == sorted([(*S1, made[S1].hex()), (*S4, made[S4].hex())])
        said = tool("unpacked", tree, played_harvest).stdout
        assert "2 different things made, of 2 streams" in said, said

        # The call, by itself. In the save state, with the stream where the routine takes it: the
        # game stops as the routine returns and has kept what it made.
        def call(name: str, *flags: object, executable: Path = game, expect: tuple[int, ...] = (0,),
                 frames: int = 5) -> tuple[list, str]:
            harvest = root / f"{name}-harvest.json"
            harvest.unlink(missing_ok=True)
            _, _, said = play(executable, root, name, "--unpacked", pack_path, "--unpacked-harvest", harvest,
                              *flags, frames=frames, expect=expect)
            return (harvested(harvest) if harvest.exists() else []), said

        things, said = call("call", "--load-state-file", state, "--unpacked-call", "00:0300,hl=4014",
                            "--report-unpacked")
        assert things == [(*S3, made[S3].hex())], things
        assert "[UNPACKED] call of 00:0300: tiles returned after" in said and "entered=1 returned=1" in said, said
        # Through the function that leads to it, with another bank mapped first and the other way asked for.
        things, said = call("call-other", "--load-state-file", state, "--unpacked-call",
                            "00:0340,de=4000,2000=03,C0F0=01")
        assert things == [(*X1, halves_changed(made[X1]).hex())] and "call of 00:0340: tiles returned" in said, said
        # In the game as it is switched on, and interpreted, which is how a record is kept.
        things, said = call("call-cold", "--unpacked-call", "00:0340,de=7FFC,2000=02", "--data-use",
                            root / "call.use.json")
        assert things == [(*V2, made[V2].hex())] and "tiles returned" in said, said
        record = json.loads((root / "call.use.json").read_text(encoding="utf-8"))
        read = gb_data_unpack.read_from(gb_data_unpack.read_runs(root / "call.use.json"), offset(V2))
        assert read == (0xC000, {(0, UNPACK + 3), (0, UNPACK + 4), (0, UNPACK + 8)}), (read, record["ranges"])
        # The cycles it took are the routine's own, and the game is where the return left it.
        took = int(said.split("tiles returned after ")[1].split()[0])
        assert 2000 < took < 4000, took
        machine = gb_discover.Savestate.load(root / "call-cold.state")
        assert bytes(machine.read(BUFFER + index) for index in range(32)) == made[V2]
        stopped = json.loads((root / "call-cold.json").read_text(encoding="utf-8"))
        assert (stopped["pc"], stopped["sp"]) == (0x0100, 0xFFFE), (stopped["pc"], stopped["sp"])
        # In a state the game was saved in halted, the function still runs.
        things, said = call("call-halted", "--load-state-file", root / "later.state", "--unpacked-call",
                            "00:0300,hl=4014")
        assert things == [(*S3, made[S3].hex())] and "tiles returned" in said, said
        # Registers, a word of RAM and interrupts, set in the order given: a later one wins.
        things, said = call("call-sets", "--unpacked-call",
                            "00:0300,hl=7FFC,2000=02,C0F0=01,C0F0=00,a=12,bc=3456,ime=1")
        assert things == [(*V2, made[V2].hex())], (things, said)
        # A function that comes back without the routine, one that never does, and a wait that is too short.
        things, said = call("came-back", "--unpacked-call", f"00:{RETURNS:04X}", expect=(1,))
        assert not things and "it came back after" in said and "no routine of the pack had returned" in said, said
        things, said = call("gave-up", "--unpacked-call", f"00:{LOOPS:04X}", "--unpacked-call-cycles", "5000",
                            expect=(1,))
        assert not things and "no routine of the pack returned in the 5000 cycles it was given" in said, said
        # A function that turns the screen off never ends a frame: the cycles are still counted.
        things, said = call("gave-up-dark", "--unpacked-call", f"00:{DARK:04X}", "--unpacked-call-cycles", "5000",
                            expect=(1,))
        assert not things and "no routine of the pack returned in the 5000 cycles it was given" in said, said
        # Interrupts on or off while the function runs, as the call says.
        for ime, low, high in ((1, 3, 6), (0, 0, 0)):
            call(f"counts-{ime}", "--unpacked-call", f"00:{COUNTS:04X},ime={ime}", "--unpacked-call-cycles",
                 str(4 * 70224), expect=(1,), frames=50)
            counted = gb_discover.Savestate.load(root / f"counts-{ime}.state").read(COUNTED)
            assert low <= counted <= high, (ime, counted)
        things, said = call("too-short", "--load-state-file", state, "--unpacked-call", "00:0300,hl=4014",
                            "--unpacked-call-cycles", "1000", expect=(1,))
        assert not things and "no routine of the pack returned in the 1000 cycles it was given" in said, said
        things, said = call("frames-first", "--unpacked-call", f"00:{LOOPS:04X}", expect=(1,), frames=2)
        assert not things and "no routine of the pack had returned when the game stopped" in said, said
        # Calls that cannot be made, each said by name.
        for name, words, flags in (
                ("bad-call", "--unpacked-call takes the BANK:ADDRESS of a function", ("--unpacked-call", "0300")),
                ("bad-set", "--unpacked-call takes the BANK:ADDRESS of a function",
                 ("--unpacked-call", "00:0300,ix=1")),
                ("bad-value", "--unpacked-call takes the BANK:ADDRESS of a function",
                 ("--unpacked-call", "00:0300,a=100")),
                ("other-bank", "the function is in bank 03, and bank 02 is mapped",
                 ("--load-state-file", state, "--unpacked-call", "03:4000")),
                ("not-ram", "E000 is not a byte of RAM", ("--unpacked-call", "00:0300,E000=01")),
                ("benchmark", "--unpacked-call is for normal execution",
                 ("--unpacked-call", "00:0300,hl=4014", "--benchmark"))):
            things, said = call(name, *flags, expect=(1,))
            assert words in said and not things, (name, said)
        _, _, said = play(game, root, "no-pack", "--unpacked-call", "00:0300,hl=4014", expect=(1,))
        assert "--unpacked-call needs --unpacked FILE too" in said, said
        run_dir = root / "run-record"
        result = run([str(game), "--headless", "--limit-frames", "5", "--record-run", str(run_dir), "--unpacked",
                      str(pack_path), "--unpacked-call", "00:0300,hl=4014"], expect=(1,), timeout=180)
        assert "A run cannot be recorded or replayed with --unpacked-call" in result.stderr, result.stderr

        # A list written by hand, and the game unpacking each of its streams both ways.
        listed = root / "streams.txt"
        listed.write_text("# found by hand\n\n" + "\n".join(
            f"{bank:X}:{address:04x}" + ("   ; and a comment" if index % 2 else "")
            for index, (bank, address) in enumerate(LISTED)) + "\n", encoding="utf-8")
        out = root / "listed"
        said = tool("unpack", tree, "--game", game, "--state", state, "--streams", listed, "-o", out).stdout
        assert "tiles: calling 00:0340 in" in said and "once for each stream and each of 1 variant" in said, said
        assert "6 streams known (6 from a list): 6 unpacked" in said and f"12 harvests in {out}" in said, said
        assert sorted(path.name for path in out.glob("*.json")) == \
            sorted(name_of(stream, variant) for stream in LISTED for variant in ("", "other")), list(out.iterdir())
        for stream in LISTED:
            assert harvested(out / name_of(stream)) == [(*stream, made[stream].hex())], stream
            assert harvested(out / name_of(stream, "other")) == [(*stream, halves_changed(made[stream]).hex())]
        assert not (out / "guessed").exists() and not (out / "run" / "saves").exists()
        report = json.loads((out / "run" / "report.json").read_text(encoding="utf-8"))
        assert report["schema"] == "gbrecomp.unpack-report" and report["unpacker"] == "tiles" and not report["ends"]
        assert [(entry["bank"], entry["address"], entry["found"], entry["harvests"]) for entry in report["streams"]] \
            == [(*stream, "list", [name_of(stream), name_of(stream, "other")]) for stream in sorted(LISTED)]
        # What a called routine made of a stream is what the played game made of it.
        one = root / "one.txt"
        one.write_text(f"{S1[0]:02X}:{S1[1]:04X}\n", encoding="utf-8")
        said = tool("unpack", tree, "--game", game, "-o", root / "one", "--streams", one, "--workers", 1).stdout
        assert "in the game as it is switched on" in said, said
        called = json.loads((root / "one" / name_of(S1)).read_text(encoding="utf-8"))["made"]
        assert called == [thing for thing in json.loads(played_harvest.read_text(encoding="utf-8"))["made"]
                          if thing["address"] == S1[1]], called
        # With no function declared, the routine itself is entered.
        direct = json.loads(json.dumps(DECLARATION))
        del direct["unpackers"][0]["call"]["function"], direct["unpackers"][0]["call"]["stream"]
        (tree / "unpackers.json").write_text(json.dumps(direct), encoding="utf-8")
        said = tool("unpack", tree, "--game", game, "--state", state, "--streams", listed, "-o", root / "direct").stdout
        assert "tiles: calling 00:0300 in" in said and "6 unpacked" in said, said
        assert all((root / "direct" / path.name).read_bytes() == path.read_bytes() for path in out.glob("*.json"))
        (tree / "unpackers.json").write_text(json.dumps(DECLARATION), encoding="utf-8")

        # The tables declared: five entries name four streams, one of them listed twice, and one points nowhere.
        out = root / "tabled"
        said = tool("unpack", tree, "--game", game, "--state", state, "--tables", "-o", out).stdout
        assert "5 streams known (5 from the tables): 5 unpacked" in said, said
        assert "1 entry of the tables points at nothing in the ROM" in said, said
        assert sorted(path.name for path in out.glob("*.json")) == \
            sorted(name_of(stream, variant) for stream in (X1, S1, U1, P1, T1) for variant in ("", "other"))
        assert harvested(out / name_of(X1)) == [(*X1, made[X1].hex())]

        # Walking on, before the tree says what play read: what follows T1 unpacks like a stream.
        out = root / "walked-blind"
        said = tool("unpack", tree, "--game", game, "--state", state, "--streams", listed, "--tables", "--walk",
                    "-o", out, "--keep-records").stdout
        report = json.loads((out / "run" / "report.json").read_text(encoding="utf-8"))
        assert (2, TEXT[1], "walk") in [(entry["bank"], entry["address"], entry["found"])
                                        for entry in report["streams"]], report["streams"]
        assert (out / "guessed" / name_of(TEXT)).exists() and (out / "run" / "records" / name_of(TEXT)).exists()
        # And after: a record of play, written into the tree, has those bytes read by another
        # instruction. The record is of longer play than the harvest was: it has two of the streams
        # to be guessed at read as well, by the routine, which is no reason to drop them.
        use_path = root / "play.use.json"
        recorded, _, _ = play(game, root, "recorded", "--data-use", use_path, frames=LATER)
        assert not [key for key in later_end if key != "dispatch_fallbacks" and later_end[key] != recorded.get(key)]
        tool("uses", tree, use_path)
        out = root / "walked"
        said = tool("unpack", tree, "--game", game, "--state", state, "--streams", listed, "--tables", "--walk",
                    "-o", out).stdout
        assert "10 streams known (6 from a list, 3 from the tables, 1 the tree has): 10 unpacked" in said, said
        assert "walking on from where they end: 5 more streams guessed at" in said, said
        assert "2 in chains that arrive exactly at the start of another known stream" in said, said
        assert f"3 in chains that do not: guesses, in {out / 'guessed'}" in said, said
        assert f"24 harvests in {out}, for `unpacked` to take, and 6 of guesses in {out / 'guessed'}" in said, said
        assert "lie wholly in files of code" not in said and not (out / "run" / "records").exists(), said
        report = json.loads((out / "run" / "report.json").read_text(encoding="utf-8"))
        found = {(entry["bank"], entry["address"]): entry for entry in report["streams"]}
        assert {stream: entry["found"] for stream, entry in found.items()} == {
            S1: "table", S4: "tree", T1: "list", U1: "list", U2: "list", W1: "list", Y1: "list", V1: "list",
            X1: "table", P1: "table", S2: "walk", S3: "walk", S5: "walk", S6: "walk", V2: "walk"}, found
        assert all(entry["read"] == len(STREAMS[stream]) for stream, entry in found.items()), found
        assert found[S2]["after"] == list(S1) and found[S3]["after"] == list(S2)
        assert found[S2]["meets"] == list(S4) and found[S3]["meets"] == list(S4)
        assert found[S5]["after"] == list(S4) and found[S6]["after"] == list(S5) and found[V2]["after"] == list(V1)
        assert not any("meets" in found[stream] for stream in (S5, S6, V2))
        assert all(found[stream]["in_code"] == 0 for stream in (S2, S3, S5, S6, V2))
        for stream in (S2, S3):
            assert found[stream]["harvests"] == [name_of(stream), name_of(stream, "other")]
            assert harvested(out / name_of(stream)) == [(*stream, made[stream].hex())]
            assert harvested(out / name_of(stream, "other")) == [(*stream, halves_changed(made[stream]).hex())]
        for stream in (S5, S6, V2):
            assert found[stream]["harvests"] == ["guessed/" + name_of(stream), "guessed/" + name_of(stream, "other")]
            assert harvested(out / "guessed" / name_of(stream)) == [(*stream, made[stream].hex())]
            assert not (out / name_of(stream)).exists()
        # A guess has a picture beside its harvest, of the size its stream says: one tile, and two.
        picture = out / "guessed" / f"{S5[0]:02X}_{S5[1]:04X}.png"
        assert gb_data.read_png(picture.name, picture.read_bytes())[:2] == (8, 8)
        picture = out / "guessed" / f"{V2[0]:02X}_{V2[1]:04X}.png"
        assert gb_data.read_png(picture.name, picture.read_bytes())[:2] == (8, 16)
        # Each chain ended for the reason there is.
        ends = {(tuple(end.get("after", ())), tuple(end.get("at", ()))): end["why"] for end in report["ends"]}
        for after, at, words in (
                (S6, NO_PICTURE, "what was made cannot be drawn the way it is declared (a picture 9 tiles by 9"),
                (T1, TEXT, f"a record of play has those bytes read at 00:{make_rom.text_reader:04X}, which reads "
                           f"no known stream"),
                (U1, RUN_OVER, "the routine read on past the start of a known stream, 02:420A"),
                (U2, (), "padding follows it (16 bytes of 00 or more)"),
                (W1, (), "padding follows it (16 bytes of FF or more)"),
                (Y1, LONG, f"no routine of the pack returned in the {CYCLES} cycles it was given"),
                (P1, TABLE2, "what was made cannot be drawn the way it is declared (a picture 0 tiles by 0"),
                (V2, (), "its bank ends there"),
                (X1, (), f"only padding is left in its bank ({0x4000 - len(STREAMS[X1])} bytes of 00)")):
            assert words in ends.get((after, at), ""), (after, at, ends)
        assert len(ends) == 9, ends
        assert "2 chains went no further: what was made cannot be drawn the way it is declared" in said, said
        assert "2 chains went no further: padding follows it" in said, said
        assert "1 chain went no further: a record of play has those bytes read at BB:AAAA" in said, said
        # A walk that may make two guesses stops there, and says so.
        said = tool("unpack", tree, "--game", game, "--state", state, "--walk", "-o", root / "short",
                    "--walk-limit", 2).stdout
        assert "2 streams known (2 the tree has)" in said and "the walk was stopped after 2 guesses" in said, said

        # Into the tree, the guesses with the rest. And a picture nobody saw in play, edited: play
        # that gets as far as it shows it.
        said = tool("unpacked", tree, out, out / "guessed").stdout
        assert "30 harvests: 30 different things made, of 15 streams" in said and "28 new in" in said, said
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        things = {(thing["bank"], thing["address"], (tree / thing["made"]).read_bytes()): thing
                  for thing in manifest["unpacked"]}
        assert len(things) == 30 and not any(thing.get("worked_out") for thing in things.values())
        wanted = {}
        for stream in (S2, S5):
            thing = things[*stream, made[stream]]
            target = tree / thing["file"]
            width, height, grey = gb_data.read_png(target.name, target.read_bytes())
            assert (width, height) == ((16, 8) if stream == S2 else (8, 8)), (stream, width, height)
            target.write_bytes(gb_data.write_png(width, height, bytes(255 - level for level in grey)))
            # Every pixel to its opposite: of S5, whose picture is one tile, the first half of each plane.
            wanted[stream] = bytes(value ^ 0xFF if stream == S2 or index % 16 < 8 else value
                                   for index, value in enumerate(made[stream]))
        said = tool("build", tree, "-o", image_path, "--expect-original").stdout
        assert "and 3 replacements" in said and "it follows its twin" in said, said
        assert image_path.read_bytes() == bytes(rom)
        edited = {f"{stream[0]}_{stream[1]:04X}": content for stream, content in wanted.items()}
        _, pictures, said = play(game, root, "edited", "--unpacked", pack_path, "--report-unpacked", frames=LATER)
        assert pictures == drawn(True, **edited), [picture.hex() for picture in pictures]
        assert "entered=4 returned=4 replaced=2" in said, said
        _, pictures, _ = play(game, root, "edited-early", "--unpacked", pack_path)
        assert pictures == drawn(False)

        # The same from lifted source, which calls, jumps and returns by other ways.
        lifted = root / "lifted"
        run([sys.executable, str(GB_LIFT), "baseline", str(export_path), "--all", "-o", str(lifted)])
        package = root / "package"
        run([sys.executable, str(GB_LIFT), "package", str(export_path), "--source", str(lifted), "-o",
             str(package)])
        lifted_game = build_game(args.gbrecomp, rom_path, root / "lifted-game", args.cmake_arg, "--lift",
                                 str(package / "lift.json"))
        out = root / "lifted-listed"
        said = tool("unpack", tree, "--game", lifted_game, "--state", state, "--streams", listed, "-o", out).stdout
        assert "6 unpacked" in said, said
        assert all((out / path.name).read_bytes() == path.read_bytes() for path in (root / "listed").glob("*.json"))
        things, said = call("lifted-call", "--unpacked-call", "00:0340,de=4014,2000=02", "--report-lift",
                            executable=lifted_game)
        assert things == [(*S3, made[S3].hex())] and "tiles returned" in said and "enabled=1" in said, said

        # What the tool will not do, each said by name.
        def tool_refuses(words: str, *arguments: object, code: int = 2) -> None:
            result = tool(*arguments, expect=(code,))
            assert words in result.stderr + result.stdout, (words, result.stderr, result.stdout)

        common = ("unpack", tree, "--game", game, "--state", state, "-o", root / "no")
        tool_refuses("say where the streams are", *common)
        tool_refuses("there is no game at", "unpack", tree, "--game", root / "none.exe", "-o", root / "no", "--walk")
        tool_refuses("there is no save state at", "unpack", tree, "--game", game, "--state", root / "none.state",
                     "-o", root / "no", "--walk")
        tool_refuses("declares no unpacker 'pictures'", *common, "--walk", "--unpacker", "pictures")
        bad = root / "bad.txt"
        for line, words in (("4000", "line 2: '4000' is not the BANK:ADDRESS"), ("01:8000", "line 2: '01:8000'"),
                            ("00:4000", "line 2: '00:4000'"), ("05:4000", "line 2: '05:4000'")):
            bad.write_text(f"02:4000\n{line}\n", encoding="utf-8")
            tool_refuses(words, *common, "--streams", bad)
        tool_refuses("cannot read the list", *common, "--streams", root / "none.txt")
        # A stream the routine makes nothing of in the cycles it is given: the others are still unpacked.
        bad.write_text(f"02:4000\n{LONG[0]:02X}:{LONG[1]:04X}\n", encoding="utf-8")
        result = tool(*common[:-1], root / "partly", "--streams", bad, expect=(1,))
        assert "2 streams known (2 from a list): 1 unpacked" in result.stdout and \
            f"not unpacked: 02:4404: no routine of the pack returned in the {CYCLES} cycles" in result.stdout, \
            result.stdout
        assert not (root / "partly" / name_of(LONG)).exists() and (root / "partly" / name_of(S1)).exists()
        # Something that is no game of this kind, and a state of another game.
        tool_refuses("the game stopped:", "unpack", tree, "--game", sys.executable, "-o", root / "no", "--streams",
                     listed)
        (root / "other.state").write_bytes(bytes(64))
        tool_refuses("02:4100: the game stopped:", "unpack", tree, "--game", game, "--state", root / "other.state",
                     "-o", root / "no", "--streams", listed)
        # Declarations that cannot be called, or that say something `unpack` does not know.
        for change, words in (
                (lambda unpacker: unpacker["call"].update(set=[]), "how this game maps one"),
                (lambda unpacker: unpacker["call"].update(colour=1), "'call' of tiles has no such field as 'colour'"),
                (lambda unpacker: unpacker.update(tables=[{"bank": 0}]), "a table of tiles is 'bank', 'address'"),
                (lambda unpacker: unpacker["call"].update(stream={"bank": {"mapped_bank": True},
                                                                  "address": {"register": "hl"}}),
                 "the routine was given 02:8108 and not 02:4100")):
            altered = json.loads(json.dumps(DECLARATION))
            change(altered["unpackers"][0])
            (tree / "unpackers.json").write_text(json.dumps(altered), encoding="utf-8")
            if "was given" in words:
                # The stream put where the function does not take it: every call unpacks something else.
                result = tool(*common[:-1], root / "wrong", "--streams", listed, expect=(1,))
                assert "0 unpacked" in result.stdout and words in result.stdout, result.stdout
                assert not list((root / "wrong").glob("*.json"))
            else:
                tool_refuses(words, *common, "--streams", listed)
        altered = json.loads(json.dumps(DECLARATION))
        del altered["unpackers"][0]["tables"]
        (tree / "unpackers.json").write_text(json.dumps(altered), encoding="utf-8")
        tool_refuses("declares no 'tables' for tiles", *common, "--tables")
        altered["unpackers"].append({**altered["unpackers"][0], "name": "second"})
        (tree / "unpackers.json").write_text(json.dumps(altered), encoding="utf-8")
        tool_refuses("declares 2 unpackers: say which with --unpacker (tiles, second)", *common, "--streams", listed)
        said = tool(*common[:-1], root / "second", "--streams", listed, "--unpacker", "second").stdout
        assert "second: calling 00:0340" in said and "6 unpacked" in said, said
        (tree / "unpackers.json").unlink()
        tool_refuses("unpackers.json is gone", *common, "--streams", listed)

    print("data unpack end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
