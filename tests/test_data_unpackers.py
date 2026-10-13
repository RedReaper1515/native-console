#!/usr/bin/env python3
"""Finding the routine that unpacks, from a played game, end to end.

Builds a small game that unpacks four run-length streams out of two ROM
banks, the way a game unpacks its pictures:

- `ReadByte` reads the next byte of a stream: its `ld a,[hli]` is the one
  instruction every packed byte is read through;
- `Unpack` takes a bank in A and a stream in HL, switches to the bank and
  unpacks the stream into a buffer in RAM, 32 bytes whatever the stream. It
  keeps a count for itself and waits a frame in the middle of every run;
- `Literal`, which `Unpack` calls for a literal byte, calls `Inner`, which
  notes the byte in RAM, moves the stack pointer past both return addresses
  and jumps back into `Unpack`'s loop: two calls that never return;
- `LoadPicture` notes the stream in a word of RAM, calls `Unpack`, and
  copies the buffer to the tile area;
- `Main` loads two streams through `LoadPicture`, a third through a function
  that ends in `jp Unpack`, and a fourth through one that runs on into it;
- the frame interrupt comes in the middle of `Unpack`, stores a count of its
  own and reads a byte through `ReadByte` itself.

A game run with `--data-use-follow` on that instruction has to end as it
does without; its record has to say that `ReadByte` reads pieces, that
`Unpack` and `LoadPicture` read whole streams and `Main` several, that
`Literal` and `Inner` left, how each was entered, and what `Unpack` stored to, less the
stack and less what the interrupt stored; and `gb_data.py unpackers` has to propose `Unpack`, the
tightest, with the register pair, the bank register and the buffer, and
write that as a declaration. The rules the proposal follows are then tried
on records made up for the purpose.
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"
sys.path.insert(0, str(ROOT / "tools"))
import gb_data_unpackers  # noqa: E402  (the rules, tried on records made up here)

START = 0x0150
TUNE = 0x0300
BUFFER, SIZE = 0xC100, 32
LAST_COUNT, REMEMBERED, POINTER, TICKS = 0xC180, 0xC1C0, 0xC1E0, 0xC1F0
BANK_KEPT = 0xFF90
# (bank, address, the stream): pairs of a count and a byte, $80 and a byte for a literal, 0 to end.
STREAMS = [
    (1, 0x4000, [8, 0x11, 8, 0x22, 16, 0x33, 0]),
    (2, 0x4010, [0x80, 0xAA, 15, 0x44, 16, 0x55, 0]),
    (1, 0x4040, [32, 0x66, 0]),
    (2, 0x4080, [32, 0x77, 0]),
]
FRAMES = 20


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def assemble(origin: int, items: list) -> tuple[bytes, dict[str, int]]:
    """Bytes, with labels (a string), addresses of labels (`to`) and relative jumps to them (`by`)."""
    labels: dict[str, int] = {}
    size = 0
    for item in items:
        if isinstance(item, str):
            labels[item] = origin + size
        else:
            size += 2 if isinstance(item, tuple) and item[0] == "to" else 1
    out: list[int] = []
    for item in items:
        if isinstance(item, str):
            continue
        if isinstance(item, tuple) and item[0] == "to":
            out += word(labels[item[1]])
        elif isinstance(item, tuple):
            offset = labels[item[1]] - (origin + len(out) + 1)
            assert -128 <= offset <= 127, item
            out.append(offset & 0xFF)
        else:
            out.append(item)
    return bytes(out), labels


def unpacked(stream: list[int]) -> bytes:
    out = bytearray()
    at = 0
    while stream[at]:
        if stream[at] == 0x80:
            out.append(stream[at + 1])
        else:
            out += bytes([stream[at + 1]]) * stream[at]
        at += 2
    return bytes(out)


def make_rom() -> tuple[bytearray, dict[str, int]]:
    rom = bytearray(0x10000)                                              # four banks, MBC1
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"UNPACKERS"
    rom[0x147], rom[0x148] = 0x01, 0x01
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    code, at = assemble(START, [
        "Start",
        0xF3, 0x31, 0xFF, 0xDF,                                           # di / ld sp,$DFFF
        0xAF, 0xE0, 0x0F, 0x3E, 0x01, 0xE0, 0xFF, 0xFB,                   # xor a / ldh [$0F],a / ld a,1 / ldh [$FF],a / ei
        0xCD, ("to", "Run"),                                              # call Run
        "Run",
        0xCD, ("to", "Main"),                                             # call Main
        "Forever",
        0x18, ("by", "Forever"),                                          # jr Forever: Run never returns
        "Main",
        0x3E, STREAMS[0][0], 0x21, *word(STREAMS[0][1]), 0xCD, ("to", "LoadPicture"),
        0x3E, STREAMS[1][0], 0x21, *word(STREAMS[1][1]), 0xCD, ("to", "LoadPicture"),
        "CallThird",
        0xCD, ("to", "LoadThird"),
        "CallFourth",
        0xCD, ("to", "LoadFourth"),
        0x3E, 0x01, 0xE0, 0x87,                                           # ld a,1 / ldh [$87],a
        0xC9,
        "LoadPicture",                                                    # A: the bank, HL: the stream
        0xF5, 0x7D, 0xEA, *word(POINTER), 0x7C, 0xEA, *word(POINTER + 1), 0xF1,   # the stream, noted in RAM
        "CallUnpack",
        0xCD, ("to", "Unpack"),
        0x21, *word(BUFFER), 0x11, 0x00, 0x80, 0x06, SIZE,                # ld hl,Buffer / ld de,$8000 / ld b,32
        "Copy",
        0x2A, 0x12, 0x13, 0x05, 0x20, ("by", "Copy"),                     # ld a,[hli] / ld [de],a / inc de / dec b / jr nz
        0xC9,
        "LoadThird",
        0x3E, STREAMS[2][0], 0x21, *word(STREAMS[2][1]),
        "JumpUnpack",
        0xC3, ("to", "Unpack"),                                           # jp Unpack
        "Handler",                                                        # the frame interrupt
        0xF5, 0xE5, 0x21, *word(TUNE), 0xCD, ("to", "ReadByte"),          # push af / push hl / ld hl,Tune / call ReadByte
        0x21, *word(TICKS), 0x34, 0xE1, 0xF1, 0xD9,                       # ld hl,Ticks / inc [hl] / pop hl / pop af / reti
        "ReadByte",
        0x2A, 0xC9,                                                       # ld a,[hli] / ret
        "LoadFourth",
        0x3E, STREAMS[3][0],
        "LastOfFourth",
        0x21, *word(STREAMS[3][1]),                                       # and on into Unpack
        "Unpack",                                                         # A: the bank, HL: the stream
        0xE0, BANK_KEPT & 0xFF, 0xEA, 0x00, 0x20, 0x11, *word(BUFFER),    # ldh [$90],a / ld [$2000],a / ld de,Buffer
        "Next",
        0xCD, ("to", "ReadByte"), 0xB7, 0xC8,                             # call ReadByte / or a / ret z
        0xFE, 0x80, 0xCC, ("to", "Literal"),                              # cp $80 / call z,Literal: no way back
        0x47, 0xEA, *word(LAST_COUNT), 0x76,                              # ld b,a / ld [LastCount],a / halt
        0xCD, ("to", "ReadByte"),
        "Fill",
        0x12, 0x13, 0x05, 0x20, ("by", "Fill"),                           # ld [de],a / inc de / dec b / jr nz,Fill
        0x18, ("by", "Next"),
        "Literal",
        0xCD, ("to", "Inner"),                                            # call Inner: neither comes back
        "Inner",
        0xCD, ("to", "ReadByte"), 0xEA, *word(REMEMBERED), 0x12, 0x13,    # the byte: noted, and stored once
        0xE8, 0x04, 0x18, ("by", "Next"),                                 # add sp,4 / jr Next: on with the loop
    ])
    assert START + len(code) <= TUNE
    rom[START:START + len(code)] = code
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x40:0x43] = bytes([0xC3, *word(at["Handler"])])                  # jp Handler
    rom[TUNE] = 0x5A
    for bank, address, stream in STREAMS:
        assert len(unpacked(stream)) == SIZE
        offset = bank * 0x4000 + address - 0x4000
        rom[offset:offset + len(stream)] = bytes(stream)
    return rom, at


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def play(executable: Path, root: Path, name: str, *flags: str, expect: tuple[int, ...] = (0,)) -> tuple[dict, str]:
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    result = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES), "--save-dir",
                  str(saves), "--dump-state", str(dump), *flags], expect=expect, timeout=180)
    return (json.loads(dump.read_text(encoding="utf-8")) if dump.exists() else {}), result.stdout + result.stderr


def made_up_rules() -> None:
    """The rules of the proposal, on records that say what a game could."""
    def sample(bank: int, address: int, **more: object) -> dict:
        said = {"by": "call", "from": {"bank": 0, "address": 0x200},
                "registers": {"a": 0, "bc": 0, "de": 0, "hl": 0, "sp": 0xDFF0}, "mapped_bank": bank,
                "read": "whole",
                "streams": [{"offset": bank * 0x4000 + address % 0x4000, "size": 40, "bank": bank,
                             "address": address, "from_start": True}], "streams_dropped": False,
                "held": {"registers": [], "ram16": [0xD100], "ram16_dropped": False}, "bank_in": [],
                "wrote": [{"area": "cart_ram", "bank": 0, "address": 0xA100, "size": 800},
                          {"area": "wram", "bank": 0, "address": 0xD100, "size": 20}],
                "wrote_dropped": {"runs": 0, "bytes": 0}}
        said.update(more)
        return said

    def function(samples: list[dict], **more: int) -> dict:
        said = {"entered": len(samples), "by": {**dict.fromkeys(gb_data_unpackers.ENTERED, 0), "call": len(samples)},
                "read": len(samples), **dict.fromkeys(gb_data_unpackers.READS, 0), "whole": len(samples),
                "distance": 1, "inside": {}, "samples_dropped": 0, "samples": samples}
        said.update(more)
        return said

    # No register pair held the stream's address and a word of RAM did; no register held the bank
    # and it was the one mapped. The 20 bytes beside 800 are scratch.
    two = [sample(9, 0x4000), sample(12, 0x5000)]
    declaration, lines = gb_data_unpackers.propose((0, 0x0200), (0, 0x0180), function(two))
    assert declaration == {
        "name": "unpacker_00_0180", "function": {"bank": 0, "address": 0x0180},
        "stream": {"bank": {"mapped_bank": True}, "address": {"ram16": 0xD100}},
        "made": [{"address": 0xA100, "size": 800}], "why": declaration["why"]}, declaration
    assert any("D100, 20 bytes (work RAM), in all 2 samples: small beside 800, so scratch" in line
               for line in lines), lines
    # A stream below 4000 is in bank 0 whatever is mapped, and one stream alone is said to be no proof.
    low = [sample(0, 0x3000, mapped_bank=5, held={"registers": ["de"], "ram16": [], "ram16_dropped": False})]
    declaration, lines = gb_data_unpackers.propose((0, 0x0200), (0, 0x0180), function(low))
    assert declaration["stream"] == {"bank": 0, "address": {"register": "de"}}, declaration
    assert any("one stream is no proof" in line for line in lines), lines
    # Nothing held the address in every sample: no declaration, and the report says why.
    lost = [sample(9, 0x4000), sample(12, 0x5000, held={"registers": [], "ram16": [], "ram16_dropped": False})]
    declaration, lines = gb_data_unpackers.propose((0, 0x0200), (0, 0x0180), function(lost))
    assert declaration is None and any("no register pair and no word of RAM" in line for line in lines), lines
    # Unless that sample held it in more words than a record keeps: then it says nothing either way.
    full = [sample(9, 0x4000), sample(12, 0x5000, held={"registers": [], "ram16": [0xC000], "ram16_dropped": True})]
    declaration, lines = gb_data_unpackers.propose((0, 0x0200), (0, 0x0180), function(full))
    assert declaration["stream"]["address"] == {"ram16": 0xD100}, declaration
    assert any("1 sample held it in more words than a record keeps" in line for line in lines), lines
    # Neither a register nor the mapping gave the bank of two different banks.
    unbanked = [sample(9, 0x4000, mapped_bank=1, bank_in=["c"]), sample(12, 0x5000, mapped_bank=1)]
    declaration, lines = gb_data_unpackers.propose((0, 0x0200), (0, 0x0180), function(unbanked))
    assert declaration is None and any("no register held the bank" in line for line in lines), lines
    # Output of another size every time is not declared: a declaration has one size.
    sized = [sample(9, 0x4000), sample(12, 0x5000, wrote=[{"area": "cart_ram", "bank": 0, "address": 0xA100,
                                                           "size": 500}])]
    declaration, lines = gb_data_unpackers.propose((0, 0x0200), (0, 0x0180), function(sized))
    assert declaration is None and any("of 500 to 800 bytes: a declaration needs one size" in line
                                       for line in lines), lines
    # A buffer the caller names: 64 bytes from wherever DE pointed.
    named = [sample(9, 0x4000, registers={"a": 0, "bc": 0, "de": 0xC400, "hl": 0, "sp": 0xDFF0},
                    wrote=[{"area": "wram", "bank": 0, "address": 0xC400, "size": 64}]),
             sample(12, 0x5000, registers={"a": 0, "bc": 0, "de": 0xC800, "hl": 0, "sp": 0xDFF0},
                    wrote=[{"area": "wram", "bank": 0, "address": 0xC800, "size": 64}])]
    declaration, _ = gb_data_unpackers.propose((0, 0x0200), (0, 0x0180), function(named))
    assert declaration["made"] == [{"address": {"register": "de"}, "size": 64}], declaration
    # Reading whole allows for a few calls that joined, and for none that read several or never came back.
    assert gb_data_unpackers.reads_whole(function(two, whole=19, joined=1))
    assert not gb_data_unpackers.reads_whole(function(two, whole=5, joined=1))
    assert not gb_data_unpackers.reads_whole(function(two, whole=19, several=1))
    assert not gb_data_unpackers.reads_whole(function(two, whole=19, left=1))
    assert not gb_data_unpackers.reads_whole(function(two, whole=0, undecided=3))
    # The tightest that can be declared: the nearer function makes nothing of a size, so the next out is taken.
    inner = function([sample(9, 0x4000, wrote=[]), sample(12, 0x5000, wrote=[])])
    outer = function(copy.deepcopy(two), distance=2)
    reader = {"bytes": 80, "streams": 2, "records": 1, "too_deep": 0, "functions_dropped": 0,
              "functions": {(0, 0x01C0): inner, (0, 0x0180): outer}}
    proposed, lines = gb_data_unpackers.report((0, 0x0200), reader)
    assert [entry["name"] for entry in proposed] == ["unpacker_00_0180"], proposed
    assert any("00:01C0, not proposed" in line for line in lines) and \
        any("so the next further out" in line for line in lines), lines
    assert not any("one way to the routine among others" in line for line in lines), lines
    # And when that one is not in every call of the nearer, the report says it is one way in among others.
    reader["functions"][(0, 0x0180)] = function(copy.deepcopy(two[:1]), distance=2)
    proposed, lines = gb_data_unpackers.report((0, 0x0200), reader)
    assert len(proposed) == 1 and any("00:01C0, nearer, read in 2 calls and this in 1" in line for line in lines), lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]
    made_up_rules()

    with tempfile.TemporaryDirectory(prefix="gbunpack-") as scratch:
        root = Path(scratch)
        rom, at = make_rom()
        rom_path = root / "unpack.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        run([str(args.gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "unpack_game"])
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
             "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
        run(["ninja", "-C", str(project / "build")])
        executable = next(path for path in (project / "build" / "unpack_game.exe", project / "build" / "unpack_game")
                          if path.exists())
        reader = f"00:{at['ReadByte']:04X}"

        # Following changes nothing about what the game does, and neither does recording.
        plain, _ = play(executable, root, "plain")
        assert plain["hram_ff80_fffe"][7] == 1, plain["hram_ff80_fffe"][:8]
        buffer = plain["wram_bank_0_c000_cfff"][BUFFER - 0xC000:][:SIZE]
        assert bytes(buffer) == unpacked(STREAMS[3][2]), buffer
        ticks = plain["wram_bank_0_c000_cfff"][TICKS - 0xC000]
        assert ticks >= 7, ticks                                          # a frame for each run of the four streams
        recorded, _ = play(executable, root, "recorded", "--data-use", str(root / "plain-use.json"))
        record_path = root / "use.json"
        followed_state, _ = play(executable, root, "followed", "--data-use", str(record_path),
                                 "--data-use-follow", reader)
        for name, state in (("recorded", recorded), ("followed", followed_state)):
            differ = [key for key in plain if key != "dispatch_fallbacks" and plain[key] != state.get(key)]
            assert not differ, (name, differ)
        # What is followed is written into a record, and is an instruction's address.
        _, log = play(executable, root, "alone", "--data-use-follow", reader, expect=(1,))
        assert "--data-use-follow needs --data-use FILE" in log, log
        _, log = play(executable, root, "wrong", "--data-use", str(root / "no.json"), "--data-use-follow", "ReadByte",
                      expect=(1,))
        assert "takes the BANK:ADDRESS of an instruction that reads" in log and not (root / "no.json").exists(), log

        # The record is the one a plain recording makes, and more.
        record = json.loads(record_path.read_text(encoding="utf-8"))
        without = json.loads((root / "plain-use.json").read_text(encoding="utf-8"))
        assert "followed" not in without
        assert {key: value for key, value in record.items() if key != "followed"} == without
        followed = record["followed"]
        assert followed["too_deep"] == 0 and len(followed["readers"]) == 1, followed["too_deep"]
        entry = followed["readers"][0]
        packed = sum(len(stream) for _, _, stream in STREAMS)
        assert (entry["bank"], entry["address"]) == (0, at["ReadByte"]) and entry["functions_dropped"] == 0
        assert entry["bytes"] == packed + ticks and entry["streams"] == len(STREAMS) + 1, (entry["bytes"], ticks)
        functions = {said["address"]: said for said in entry["functions"]}
        assert set(functions) == {at[name] for name in ("ReadByte", "Unpack", "Literal", "Inner", "LoadPicture",
                                                         "LoadThird", "LoadFourth", "Main", "Run")} | {0x40}, \
            sorted(functions)

        def reads(name: str) -> dict:
            said = functions[at[name]] if name in at else functions[int(name, 16)]
            return {kind: said[kind] for kind in ("read", *gb_data_unpackers.READS) if said[kind]}

        def entered(name: str) -> dict:
            said = functions[at[name]] if name in at else functions[int(name, 16)]
            assert said["entered"] == sum(said["by"].values()), said
            return {kind: count for kind, count in said["by"].items() if count}

        # ReadByte reads a byte a call, each joined to the last; and the interrupt's byte again and again.
        assert reads("ReadByte") == {"read": packed + ticks, "joined": packed + ticks}, reads("ReadByte")
        assert entered("ReadByte") == {"call": packed + ticks} and functions[at["ReadByte"]]["distance"] == 0
        # Unpack reads a stream whole every call, however it was entered: the last when the record ended.
        assert reads("Unpack") == {"read": 4, "whole": 3, "undecided": 1}, reads("Unpack")
        assert entered("Unpack") == {"call": 2, "jump": 1, "fall": 1}, entered("Unpack")
        assert functions[at["Unpack"]]["distance"] == 1
        assert {(said["address"], said["times"]) for said in functions[at["Unpack"]]["inside"]} == \
            {(at["LoadPicture"], 2), (at["LoadThird"], 1), (at["LoadFourth"], 1)}, functions[at["Unpack"]]["inside"]
        # The two calls for a literal never came back: the stack pointer was moved past both return
        # addresses at once. Each was made once, though the instruction that calls Literal ran for
        # every count.
        assert reads("Literal") == {"read": 1, "left": 1} and entered("Literal") == {"call": 1}, reads("Literal")
        assert reads("Inner") == {"read": 1, "left": 1} and entered("Inner") == {"call": 1}, reads("Inner")
        assert functions[at["Inner"]]["distance"] == 1 and functions[at["Inner"]]["inside"] == [
            {"bank": 0, "address": at["Literal"], "times": 1}], functions[at["Inner"]]
        assert functions[at["Literal"]]["distance"] == 2 and functions[at["Literal"]]["inside"] == [
            {"bank": 0, "address": at["Unpack"], "times": 1}], functions[at["Literal"]]
        # So do the functions that call it once each.
        assert reads("LoadPicture") == {"read": 2, "whole": 2} and entered("LoadPicture") == {"call": 2}
        assert reads("LoadThird") == {"read": 1, "whole": 1} and reads("LoadFourth") == {"read": 1, "undecided": 1}
        assert all(functions[at[name]]["distance"] == 2 for name in ("LoadPicture", "LoadThird", "LoadFourth"))
        # Main reads four, and Run never returned.
        assert reads("Main") == {"read": 1, "several": 1} and functions[at["Main"]]["distance"] == 3
        assert reads("Run") == {"unfinished": 1} and entered("Run") == {"call": 1}
        # The interrupt's routine reads its own byte: one run, on and on, and none of Unpack's.
        assert reads("40") == {"read": ticks, "joined": ticks} and entered("40") == {"interrupt": ticks}, reads("40")

        samples = functions[at["Unpack"]]["samples"]
        assert len(samples) == 4 and functions[at["Unpack"]]["samples_dropped"] == 0
        for number, (sample, (bank, address, stream)) in enumerate(zip(samples, STREAMS)):
            assert sample["streams"] == [{"offset": bank * 0x4000 + address - 0x4000, "size": len(stream),
                                          "bank": bank, "address": address, "from_start": True}], sample["streams"]
            assert sample["registers"]["a"] == bank and sample["registers"]["hl"] == address, sample["registers"]
            assert sample["held"]["registers"] == ["hl"] and sample["bank_in"] == ["a"], sample
            # The word LoadPicture notes the stream in held it for the calls LoadPicture made, and no other.
            assert sample["held"]["ram16"] == ([POINTER] if number < 2 else []), sample["held"]
            assert sample["held"]["ram16_dropped"] is False
            assert sample["read"] == ("whole" if number < 3 else "undecided"), sample["read"]
            # What it stored to: the buffer, its count, the bank it keeps, and the literal where there is
            # one. Not the stack, and not the count the interrupt keeps, which came in between.
            wrote = [(run["area"], run["address"], run["size"]) for run in sample["wrote"]]
            assert wrote == [("wram", BUFFER, SIZE), ("wram", LAST_COUNT, 1),
                             *([("wram", REMEMBERED, 1)] if number == 1 else []), ("hram", BANK_KEPT, 1)], wrote
            assert sample["wrote_dropped"] == {"runs": 0, "bytes": 0}
        assert [(sample["by"], sample["from"]["address"]) for sample in samples] == [
            ("call", at["CallUnpack"]), ("call", at["CallUnpack"]), ("jump", at["JumpUnpack"]),
            ("fall", at["LastOfFourth"])], [(sample["by"], sample["from"]) for sample in samples]
        assert samples[0]["mapped_bank"] == 1 and samples[1]["mapped_bank"] == 1 and samples[2]["mapped_bank"] == 2
        # LoadPicture's calls stored to the tile area as well, and to the word it notes the stream in:
        # which it wrote after it was entered, so that word did not hold the address then. HL did.
        for sample in functions[at["LoadPicture"]]["samples"]:
            assert sample["held"] == {"registers": ["hl"], "ram16": [], "ram16_dropped": False}, sample["held"]
            assert sample["read"] == "whole", sample["read"]
        wrote = [(run["area"], run["address"], run["size"]) for run in functions[at["LoadPicture"]]["samples"][0]["wrote"]]
        assert wrote == [("vram", 0x8000, SIZE), ("wram", BUFFER, SIZE), ("wram", LAST_COUNT, 1),
                         ("wram", POINTER, 2), ("hram", BANK_KEPT, 1)], wrote
        # And the interrupt's routine stored its own count.
        wrote = [(run["area"], run["address"], run["size"]) for run in functions[0x40]["samples"][0]["wrote"]]
        assert wrote == [("wram", TICKS, 1)], wrote

        # The record is still one `uses` reads.
        tree = root / "tree"
        run([sys.executable, str(GB_DATA), "split", str(export_path), "--rom", str(rom_path), "-o", str(tree)])
        said = run([sys.executable, str(GB_DATA), "uses", str(tree), str(record_path)]).stdout
        assert f"bytes read at {reader}" in said, said

        # The proposal: Unpack, the tightest that reads whole, with what its samples agree on.
        declared = root / "declared" / "unpackers.json"
        said = run([sys.executable, str(GB_DATA), "unpackers", str(tree), str(record_path), "-o", str(declared)]).stdout
        assert f"followed {reader}: it read {packed + ticks} bytes in 5 runs, in 1 record" in said, said
        listed = [line.split()[:2] for line in said.splitlines() if line.startswith("    ") and "entered" in line
                  and line.split()[0].isdigit()]
        assert listed == [[str(functions[address]["distance"]), f"00:{address:04X}"] for address in
                          sorted(functions, key=lambda address: (functions[address]["distance"], address))], listed
        assert listed[0] == ["0", reader] and ["1", f"00:{at['Unpack']:04X}"] in listed[1:3], listed
        whole = [line.split()[1] for line in said.splitlines() if "<- reads whole" in line]
        assert whole == [f"00:{at[name]:04X}" for name in sorted(("Unpack", "LoadPicture", "LoadThird"),
                                                                  key=lambda name: (name != "Unpack", at[name]))], whole
        assert f"00:{at['Unpack']:04X}, proposed:" in said and "not proposed" not in said, said
        assert "3 of its 3 calls that read one run read it whole; 1 more had read one when their record ended" in said
        assert "entered 4 times: 2 by a call, 1 by a jump, 1 by running on into it" in said, said
        assert "stream address: register hl held it in all 3 samples, of 3 different streams" in said, said
        assert "stream bank: register a held it in all 3 samples, of 2 different banks" in said, said
        assert f"made: {BUFFER:04X}, 32 bytes (work RAM), in all 3 samples" in said, said
        assert f"{BANK_KEPT:04X}, 1 byte (high RAM), in all 3 samples: small beside 32, so scratch" in said, said
        assert f"{REMEMBERED:04X}, 1 byte (work RAM), in 1 of 3 samples: not every call writes it" in said, said
        assert "the 3 streams of its samples, 17 bytes, lie in" in said, said
        document = json.loads(declared.read_text(encoding="utf-8"))
        assert document == {"schema": "gbrecomp.unpackers", "version": 1, "unpackers": [{
            "name": f"unpacker_00_{at['Unpack']:04X}",
            "function": {"bank": 0, "address": at["Unpack"]},
            "stream": {"bank": {"register": "a"}, "address": {"register": "hl"}},
            "made": [{"address": BUFFER, "size": SIZE}],
            "why": document["unpackers"][0]["why"]}]}, document
        why = document["unpackers"][0]["why"]
        assert reader in why and "3 of 3 did" in why and "\n" not in why, why
        # Two records add up, and without -o the declaration is printed.
        said = run([sys.executable, str(GB_DATA), "unpackers", str(tree), str(record_path), str(record_path)]).stdout
        assert "in 2 records" in said and "6 of its 6 calls that read one run read it whole" in said, said
        assert json.loads(said[said.index("{"):]) == {**document, "unpackers": [
            {**document["unpackers"][0], "why": json.loads(said[said.index("{"):])["unpackers"][0]["why"]}]}, said

        # A record that follows nothing, and the record of another ROM, are refused.
        refused = run([sys.executable, str(GB_DATA), "unpackers", str(tree), str(root / "plain-use.json")], expect=(2,))
        assert "follows no instruction" in refused.stderr, refused.stderr
        record["rom"]["global_checksum"] ^= 1
        (root / "other.json").write_text(json.dumps(record), encoding="utf-8")
        refused = run([sys.executable, str(GB_DATA), "unpackers", str(tree), str(root / "other.json")], expect=(2,))
        assert "was recorded from another ROM" in refused.stderr, refused.stderr

    print("data unpackers end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
