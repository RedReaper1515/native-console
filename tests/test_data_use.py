#!/usr/bin/env python3
"""What a game does with the data in its ROM, recorded and read back, end to end.

Builds a small game that uses its data every way the recorder tells apart:

- sixteen bytes copied straight to the tile area of video memory;
- four bytes copied to a buffer in RAM and from there to a tile map;
- two bytes that reach a tile map with a constant added to each;
- two bytes loaded into HL and jumped through;
- two bytes loaded into HL and read through, and the byte they point to;
- a byte written to a sound register, and one written to the cartridge;
- four bytes copied to RAM and from there to the sprite table by the
  hardware's own copy, started from a routine that was itself copied out of
  the ROM into high RAM;
- four bytes copied to the tile area by two POPs, the stack pointer pointed
  into the ROM by two bytes of a table, and two taken off it there by a RET;
- a byte that is only compared, and four that are never read.

A game run with `--data-use FILE` has to end as it does without, and its
record has to say each of those and nothing else; `gb_data.py uses` has to
write them into the data tree's manifest and count them; and the record of
another ROM has to be refused.
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

START = 0x0150
HANDLER = 0x02F0
TILES, TEXT, ADDED, JUMPS, POINTER, POINTED = 0x0300, 0x0320, 0x0330, 0x0340, 0x0350, 0x0360
SOUND, BANK, SPRITES, ROUTINE, COMPARED, UNREAD = 0x0370, 0x0380, 0x0390, 0x03A0, 0x03B0, 0x03C0
POPPED, RETURNS, STACKS = 0x03D0, 0x03E0, 0x03F0
RETURNED_TO = 0x02E0
HRAM_ROUTINE = 0xFF90
FRAMES = 20


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def copy(source: int, target: int, count: int) -> list[int]:
    """ld hl,source / ld de,target / ld b,count / loop: ld a,[hli] / ld [de],a / inc de / dec b / jr nz,loop"""
    return [0x21, *word(source), 0x11, *word(target), 0x06, count, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13C] = b"DATA USE"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    readers: dict[str, int] = {}
    # The screen stays on: the frames the test runs for are counted by it. A
    # write to video memory the hardware would drop is still a use.
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]                             # di / ld sp,$DFFF

    def here() -> int:
        return START + len(code)

    def copying(name: str, source: int, target: int, count: int) -> None:
        nonlocal code
        readers[name] = here() + 8                                         # the loop's ld a,[hli]
        code += copy(source, target, count)

    copying("tiles", TILES, 0x8000, 16)
    copying("text", TEXT, 0xC100, 4)
    code += copy(0xC100, 0x9800, 4)
    code += [0x21, *word(ADDED), 0x11, *word(0x9820), 0x06, 0x02]
    readers["added"] = here()
    code += [0x2A, 0xC6, 0x80, 0x12, 0x13, 0x05, 0x20, 0xF8]               # ld a,[hli] / add a,$80 / ld [de],a ...
    # Through a table of code: ld hl,Jumps / ld a,[hli] / ld h,[hl] / ld l,a / ld de,back / push de / jp hl
    code += [0x21, *word(JUMPS)]
    readers["jump_low"] = here()
    code += [0x2A]
    readers["jump_high"] = here()
    code += [0x66, 0x6F]
    back = here() + 5
    code += [0x11, *word(back), 0xD5, 0xE9]
    assert here() == back
    # Through a pointer to data: the same loads, then ld a,[hl] / ldh [$86],a
    code += [0x21, *word(POINTER)]
    readers["pointer_low"] = here()
    code += [0x2A]
    readers["pointer_high"] = here()
    code += [0x66, 0x6F]
    readers["pointed"] = here()
    code += [0x7E, 0xE0, 0x86]
    readers["sound"] = here()
    code += [0xFA, *word(SOUND), 0xE0, 0x11]                               # ld a,[Sound] / ldh [$11],a
    readers["bank"] = here()
    code += [0xFA, *word(BANK), 0xEA, 0x00, 0x20]                          # ld a,[Bank] / ld [$2000],a
    copying("sprites", SPRITES, 0xC200, 4)
    copying("routine", ROUTINE, HRAM_ROUTINE, 10)
    code += [0xCD, *word(HRAM_ROUTINE)]                                    # call the copy of the routine
    # Copied with the stack pointer in the ROM, where a table says: ld hl,Stacks / ld a,[hli] / ld h,[hl] /
    # ld l,a / ld sp,hl / ld hl,$8010 / pop de / ld [hl],e / inc hl / ld [hl],d / inc hl / pop bc /
    # ld [hl],c / inc hl / ld [hl],b. Two POPs and one reader: the instruction that pointed the stack there.
    code += [0x21, *word(STACKS)]
    readers["stack_low"] = here()
    code += [0x2A]
    readers["stack_high"] = here()
    code += [0x66, 0x6F]
    readers["popped"] = here()
    code += [0xF9, 0x21, *word(0x8010), 0xD1, 0x73, 0x23, 0x72, 0x23, 0xC1, 0x71, 0x23, 0x70]
    # And an address taken off it there: ld sp,Returns / ret, to code that puts the stack back and comes back
    readers["returned"] = here()
    code += [0x31, *word(RETURNS), 0xC9]
    after = here()
    # A read after those is the reading instruction's again.
    readers["compared"] = here()
    code += [0xFA, *word(COMPARED), 0xFE, 0x03]                            # ld a,[Compared] / cp 3
    code += [0x3E, 0x01, 0xE0, 0x87, 0x18, 0xFE]                           # ld a,1 / ldh [$87],a / jr to itself
    assert here() <= RETURNED_TO
    rom[START:START + len(code)] = bytes(code)
    # ld a,$A5 / ldh [$88],a / ld sp,$DFFF / jp after
    rom[RETURNED_TO:RETURNED_TO + 10] = bytes([0x3E, 0xA5, 0xE0, 0x88, 0x31, 0xFF, 0xDF, 0xC3, *word(after)])
    rom[HANDLER:HANDLER + 5] = bytes([0x3E, 0x5A, 0xE0, 0x85, 0xC9])       # ld a,$5A / ldh [$85],a / ret

    rom[TILES:TILES + 16] = bytes(range(0x10, 0x20))
    rom[TEXT:TEXT + 4] = bytes([0x41, 0x42, 0x43, 0x44])
    rom[ADDED:ADDED + 2] = bytes([0x01, 0x02])
    rom[JUMPS:JUMPS + 2] = bytes(word(HANDLER))
    rom[POINTER:POINTER + 2] = bytes(word(POINTED))
    rom[POINTED] = 0x77
    rom[SOUND] = 0x80
    rom[BANK] = 0x01
    rom[SPRITES:SPRITES + 4] = bytes([0x20, 0x30, 0x05, 0x00])
    # ld a,$C2 / ldh [$46],a / ld a,$28 / wait: dec a / jr nz,wait / ret
    rom[ROUTINE:ROUTINE + 10] = bytes([0x3E, 0xC2, 0xE0, 0x46, 0x3E, 0x28, 0x3D, 0x20, 0xFD, 0xC9])
    rom[COMPARED] = 0x03
    rom[UNREAD:UNREAD + 4] = bytes([0xAA, 0xBB, 0xCC, 0xDD])
    rom[POPPED:POPPED + 4] = bytes([0x3C, 0x42, 0x81, 0x99])
    rom[RETURNS:RETURNS + 2] = bytes(word(RETURNED_TO))
    rom[STACKS:STACKS + 2] = bytes(word(POPPED))
    return rom, readers


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def play(executable: Path, root: Path, name: str, *flags: str) -> dict:
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES), "--save-dir", str(saves),
         "--dump-state", str(dump), *flags], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]

    with tempfile.TemporaryDirectory(prefix="gbuse-") as scratch:
        root = Path(scratch)
        rom, readers = make_rom()
        rom_path = root / "use.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        run([str(args.gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "use_game"])
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
             "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
        run(["ninja", "-C", str(project / "build")])
        executable = next(path for path in (project / "build" / "use_game.exe", project / "build" / "use_game")
                          if path.exists())

        # Recording changes nothing about what the game does.
        plain = play(executable, root, "plain")
        assert plain["hram_ff80_fffe"][5:9] == [0x5A, 0x77, 0x01, 0xA5], plain["hram_ff80_fffe"][:9]
        record_path = root / "use.json"
        recorded = play(executable, root, "recorded", "--data-use", str(record_path))
        differ = [key for key in plain if key != "dispatch_fallbacks" and plain[key] != recorded.get(key)]
        assert not differ, differ

        record = json.loads(record_path.read_text(encoding="utf-8"))
        assert record["schema"] == "gbrt.data-use" and record["version"] == 1
        assert record["rom"] == {"size": len(rom), "global_checksum": (rom[0x14E] << 8) | rom[0x14F]}, record["rom"]
        assert record["instructions"] > 100, record["instructions"]
        got = {entry["offset"]: (entry["size"], entry["as_it_is"], entry["worked_out"], entry["read_at"],
                                 entry["one_reader"]) for entry in record["ranges"]}
        expected = {
            TILES: (16, ["tiles"], [], [0, readers["tiles"]], True),
            TEXT: (4, ["tile_map"], [], [0, readers["text"]], True),
            ADDED: (2, [], ["tile_map"], [0, readers["added"]], True),
            JUMPS: (1, ["jump"], [], [0, readers["jump_low"]], True),
            JUMPS + 1: (1, ["jump"], [], [0, readers["jump_high"]], True),
            POINTER: (1, ["pointer"], [], [0, readers["pointer_low"]], True),
            POINTER + 1: (1, ["pointer"], [], [0, readers["pointer_high"]], True),
            POINTED: (1, [], [], [0, readers["pointed"]], True),
            SOUND: (1, ["sound"], [], [0, readers["sound"]], True),
            BANK: (1, ["bank"], [], [0, readers["bank"]], True),
            SPRITES: (4, ["oam"], [], [0, readers["sprites"]], True),
            ROUTINE: (10, [], [], [0, readers["routine"]], True),
            COMPARED: (1, [], [], [0, readers["compared"]], True),
            POPPED: (4, ["tiles"], [], [0, readers["popped"]], True),
            RETURNS: (2, ["jump"], [], [0, readers["returned"]], True),
            STACKS: (1, ["pointer"], [], [0, readers["stack_low"]], True),
            STACKS + 1: (1, ["pointer"], [], [0, readers["stack_high"]], True),
        }
        assert got == expected, {offset: (got.get(offset), expected.get(offset))
                                 for offset in set(got) | set(expected) if got.get(offset) != expected.get(offset)}
        for entry in record["ranges"]:
            assert entry["bank"] == 0 and entry["address"] == entry["offset"], entry

        # Into the data tree: what each stretch of a region is for.
        tree = root / "tree"
        run([sys.executable, str(GB_DATA), "split", str(export_path), "--rom", str(rom_path), "-o", str(tree)])
        said = run([sys.executable, str(GB_DATA), "uses", str(tree), str(record_path)]).stdout
        assert "the game read 52 of the" in said, said
        lines = {line.split()[0]: line.split()[1:] for line in said.splitlines() if line.startswith("  ")}
        assert lines["tiles"] == ["20", "0", "1"] and lines["tile_map"] == ["4", "2", "1"], said
        assert lines["jump"] == ["4", "0", "1"] and lines["pointer"] == ["4", "0", "1"], said
        assert lines["oam"] == ["4", "0", "1"] and lines["sound"] == ["1", "0", "1"], said
        assert "12 bytes were read and went to none of those" in said, said
        # The same bytes, by the instruction that read them: the copying loops first.
        assert f"16 bytes read at 00:{readers['tiles']:04X}: tiles 16" in said, said
        assert f"10 bytes read at 00:{readers['routine']:04X}: nothing 10" in said, said
        assert f"2 bytes read at 00:{readers['added']:04X}: (tile_map) 2" in said, said
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["readers"][0] == {"bank": 0, "address": readers["tiles"], "bytes": 16,
                                          "uses": {"tiles": 16}}, manifest["readers"][0]
        assert len(manifest["readers"]) == 17, len(manifest["readers"])
        region = next(region for region in manifest["regions"]
                      if region["offset"] <= TILES < region["offset"] + region["size"])
        assert region["kind"] == "data", region["kind"]
        seen = {entry["offset"] + region["offset"]: entry for entry in region["seen"]}
        assert seen[TILES] == {"offset": TILES - region["offset"], "size": 16, "as_it_is": ["tiles"],
                               "worked_out": [], "read_at": [0, readers["tiles"]], "one_reader": True}, seen[TILES]
        assert seen[ADDED]["worked_out"] == ["tile_map"] and UNREAD not in seen
        # The tree is still the ROM, and two records of the same run add nothing.
        run([sys.executable, str(GB_DATA), "build", str(tree), "-o", str(root / "rebuilt.gb"), "--expect-original"])
        again = run([sys.executable, str(GB_DATA), "uses", str(tree), str(record_path), str(record_path)]).stdout
        assert "2 records: the game read 52 of the" in again, again

        # A record is of one ROM.
        record["rom"]["global_checksum"] ^= 1
        other = root / "other.json"
        other.write_text(json.dumps(record), encoding="utf-8")
        refused = run([sys.executable, str(GB_DATA), "uses", str(tree), str(other)], expect=(2,))
        assert "was recorded from another ROM" in refused.stderr, refused.stderr

    print("data use end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
