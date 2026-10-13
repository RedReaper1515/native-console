#!/usr/bin/env python3
"""Data that grows by moving: the pointers a game reads through, recorded,
and a string made longer put into free space with its pointers turned.

Builds a small game on a cartridge of four banks that reads its data every
way the recorder tells apart:

- a table of pointers, each read through an instruction's operand;
- a string reached through a pointer that lies in a script, itself reached
  through the table, with a string of its own behind the pointer: a pointer
  inside something that can move, and grow;
- a string right after it, with a pointer of its own;
- a record read through a pointer with a constant added, with a number out
  of RAM added, and with a constant added a byte at a time;
- a string whose pointer goes through RAM and the stack on its way;
- a string printed on past where another pointer leads: one thing with two
  pointers, one of them into its middle;
- two strings in a switched bank, with their pointers in the first;
- a byte read through a pointer that the game then jumps through;
- four bytes each read through a pointer the game also reckons with: takes
  a register from, compares, shifts, masks;
- a byte read through a pointer with a byte of the ROM added to it, which is
  through neither;
- a byte in each of two banks, read through one pointer with one bank
  mapped and then the other, and a byte whose pointer lies in bytes the
  analysis decoded as code;
- a string an instruction's operand leads to, a routine copied into high
  RAM, and a page of the ROM the hardware copies to the sprite table, after
  which a stub of the runtime's has run several instructions unseen.

A game run with `--data-use-pointers` has to end as it does without, and
write the record it writes without, with the pairs of bytes and how each
byte was reached added. `gb_data.py pointers` has to make of that the things
and say of each whether it can move, and why not; `free` has to propose the
empty ends of the banks; a string made longer has to be in the image `build`
makes, in declared free space, with its pointers turned and the strings
after it where they were, and on the screen of the game given the image;
what cannot move has to be refused, each for its reason; and `build
--move-all` has to make an image the game plays the same from, which it does
not once one pointer is left unturned.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_DATA = ROOT / "tools" / "gb_data.py"
sys.path.insert(0, str(ROOT / "tools"))
import gb_discover  # noqa: E402  (the save state reader)

BANK = 0x4000
START, PRINT = 0x0150, 0x0300
END, SPACE = 0x50, 0x7F
FRAMES = 20


def word(value: int) -> list[int]:
    return [value & 0xFF, value >> 8]


def letters_of(text: str) -> bytes:
    """A to P are the bytes 80 to 8F."""
    return bytes(SPACE if letter == " " else 0x80 + ord(letter) - ord("A") for letter in text)


# The data, one thing after another from 0400, in the order it lies.
PIECES = [
    ("TABLE", bytes(32)),                                   # sixteen pointers, filled in below
    ("SCRIPT", bytes(2) + letters_of("ED") + bytes([END])),     # a pointer to STR_A, and a string of its own
    ("STR_A", letters_of("BAD CAFE") + bytes([END])),
    ("STR_B", letters_of("DEAF") + bytes([END])),
    ("REC", bytes(range(0x10, 0x18))),
    ("NUM1", bytes([0x41])), ("NUM2", bytes([0x42])), ("NUM3", bytes([0x43])), ("NUM4", bytes([0x44])),
    ("RAMSTR", letters_of("A BEAD") + bytes([END])),
    ("WALK1", letters_of("FEED")),                          # not ended: printing it prints WALK2 too
    ("WALK2", letters_of("CAB") + bytes([END])),
    ("JUMPED", bytes([0x77, 0xC9])),                        # a byte to read, and a ret to jump to
    ("COMBO", bytes(range(0x20, 0x24))),
    ("IDX", bytes([2])),
    ("IMM_STR", letters_of("FACE") + bytes([END])),
    # ldh [$46],a / ld a,$28 / wait: dec a / jr nz,wait / ret: called with the page in A
    ("ROUTINE", bytes([0xE0, 0x46, 0x3E, 0x28, 0x3D, 0x20, 0xFD, 0xC9])),
    ("DMAPG", bytes([0x07])),
    # A string the analysis is made to decode the end of: add a,h / add a,l / ld d,b / ret.
    ("PINSTR", letters_of("BEEF") + bytes([END, 0xC9])),
]
AT: dict[str, int] = {}
_cursor = 0x0400
for _name, _content in PIECES:
    AT[_name] = _cursor
    _cursor += len(_content)
DATA_END = _cursor
PAGE = 0x0700                                               # what the hardware copies to the sprite table
FAR_A, FAR_B = BANK + 0x0100, BANK + 0x0104                 # 01:4100 and 01:4104
FAR_C = BANK + 0x00F0                                       # 01:40F0, and 02:40F0 a bank further on
CODED, CODEPTR = 0x0580, 0x05A0                             # a byte, and the pointer to it that is decoded as code
FAR = letters_of("ACE") + bytes([END]) + letters_of("BED") + bytes([END])
# What each entry of the table leads to.
ENTRIES = ["SCRIPT", "STR_B", "REC", "COMBO", "RAMSTR", "JUMPED", "WALK1", "WALK2"]
ROWS = ["BAD CAFE", "DEAF", "FACE", "A BEAD", "FEEDCAB", "CAB", "ACE", "BED", "BEEF", "ED"]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    """The ROM, and where the operand of each instruction that has an address for one lies."""
    rom = bytearray(4 * BANK)
    rom[0x100:0x104] = bytes([0x00, 0xC3, *word(START)])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"DATA GROW"
    rom[0x147], rom[0x148] = 0x01, 0x01                     # a mapper, and four banks
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    operands: dict[str, int] = {}
    code: list[int] = [0xF3, 0x31, 0xFF, 0xDF]              # di / ld sp,$DFFF
    # wait: ldh a,[$44] / cp 144 / jr nz,wait / xor a / ldh [$40],a: the screen off while video memory is written
    code += [0xF0, 0x44, 0xFE, 0x90, 0x20, 0xFA, 0xAF, 0xE0, 0x40]
    code += [0x3E, 0x01, 0xEA, 0x00, 0x20]                  # ld a,1 / ld [$2000],a: bank 1 at 4000

    def operand(name: str) -> None:
        operands[name] = START + len(code) + 1

    def entry(number: int, name: str) -> None:
        """ld hl,Table+2n / ld a,[hli] / ld h,[hl] / ld l,a: HL is the table's pointer."""
        nonlocal code
        operand(name)
        code += [0x21, *word(AT["TABLE"] + 2 * number), 0x2A, 0x66, 0x6F]

    def show(row: int) -> None:
        nonlocal code
        code += [0x11, *word(0x9800 + 0x20 * row), 0xCD, *word(PRINT)]

    entry(0, "script")
    code += [0x2A, 0x66, 0x6F]                              # the script's pointer, through the table's
    show(0)
    entry(0, "script2")
    code += [0x23, 0x23]                                    # inc hl / inc hl: the script's own string
    show(9)
    entry(1, "str_b")
    show(1)
    entry(2, "rec1")
    code += [0x11, 0x05, 0x00, 0x19, 0x7E, 0xE0, 0x80]      # ld de,5 / add hl,de / ld a,[hl] / ldh [$80],a
    code += [0x3E, 0x03, 0xEA, 0x00, 0xC0]                  # ld a,3 / ld [$C000],a: further back than the first
    entry(2, "rec2")
    # ld a,[$C000] / ld e,a / ld d,0 / add hl,de / ld a,[hl] / ldh [$81],a
    code += [0xFA, 0x00, 0xC0, 0x5F, 0x16, 0x00, 0x19, 0x7E, 0xE0, 0x81]
    entry(2, "rec3")
    # ld a,l / add a,6 / ld l,a / ld a,h / adc a,0 / ld h,a / ld a,[hl] / ldh [$82],a
    code += [0x7D, 0xC6, 0x06, 0x6F, 0x7C, 0xCE, 0x00, 0x67, 0x7E, 0xE0, 0x82]
    operand("imm_str")
    code += [0x21, *word(AT["IMM_STR"])]
    show(2)
    entry(3, "combo")
    code += [0x7E, 0xE0, 0x85, 0xE5]                        # ld a,[hl] / ldh [$85],a / push hl
    operand("idx")
    # ld a,[Idx] / ld e,a / ld d,0 / pop hl / add hl,de / ld a,[hl] / ldh [$83],a: a byte of the ROM added
    code += [0xFA, *word(AT["IDX"]), 0x5F, 0x16, 0x00, 0xE1, 0x19, 0x7E, 0xE0, 0x83]
    entry(4, "ramstr")
    # ld a,l / ld [$C010],a / ld a,h / ld [$C011],a / ld hl,$C010 / ld a,[hli] / ld h,[hl] / ld l,a
    code += [0x7D, 0xEA, 0x10, 0xC0, 0x7C, 0xEA, 0x11, 0xC0, 0x21, 0x10, 0xC0, 0x2A, 0x66, 0x6F]
    code += [0xE5, 0xC1, 0x60, 0x69]                        # push hl / pop bc / ld h,b / ld l,c
    show(3)
    entry(5, "jumped")
    code += [0x7E, 0xE0, 0x84, 0x23]                        # ld a,[hl] / ldh [$84],a / inc hl
    back = START + len(code) + 5
    code += [0x11, *word(back), 0xD5, 0xE9]                 # ld de,back / push de / jp hl, to the ret behind it
    assert START + len(code) == back
    entry(6, "walk1")
    show(4)
    entry(7, "walk2")
    show(5)
    entry(8, "far_a")
    show(6)
    entry(9, "far_b")
    show(7)
    entry(10, "pinstr")
    show(8)
    # Pointers that are numbers to the game as well: each is read through, and then reckoned with.
    entry(11, "num1")
    code += [0x7E, 0x0E, 0x01, 0x7D, 0x91]                  # ld a,[hl] / ld c,1 / ld a,l / sub c
    entry(12, "num2")
    code += [0x7E, 0x7C, 0xFE, 0x7F]                        # ld a,[hl] / ld a,h / cp $7F
    entry(13, "num3")
    code += [0x7E, 0xCB, 0x3C]                              # ld a,[hl] / srl h
    entry(14, "num4")
    code += [0x7E, 0x7D, 0xE6, 0x0F]                        # ld a,[hl] / ld a,l / and $0F
    # One pointer with one bank mapped and then with another: ld a,[hl] / ld a,2 / ld [$2000],a, and back.
    entry(15, "far_c1")
    code += [0x7E, 0x3E, 0x02, 0xEA, 0x00, 0x20]
    entry(15, "far_c2")
    code += [0x7E, 0x3E, 0x01, 0xEA, 0x00, 0x20]
    # A pointer in bytes the analysis decodes as code: ld hl,CodePtr / ld a,[hli] / ld h,[hl] / ld l,a / ld a,[hl]
    operand("codeptr")
    code += [0x21, *word(CODEPTR), 0x2A, 0x66, 0x6F, 0x7E]
    # The routine into high RAM, and a call of it with the page to copy, itself read from the ROM:
    # the hardware copies a page of the ROM. A stub of the runtime's runs the routine in one go, and
    # A comes back a count that ran down, no byte of the ROM: added to a pointer it is a plain number.
    operand("routine")
    code += [0x21, *word(AT["ROUTINE"]), 0x11, 0x90, 0xFF, 0x06, 0x08, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA]
    operand("dmapg")
    code += [0xFA, *word(AT["DMAPG"]), 0xCD, 0x90, 0xFF, 0x5F, 0x16, 0x00]     # ld a,[Page] / call / ld e,a / ld d,0
    entry(1, "str_b2")
    code += [0x19, 0x7E]                                    # add hl,de / ld a,[hl]
    code += [0x3E, 0x91, 0xE0, 0x40, 0x18, 0xFE]            # ld a,$91 / ldh [$40],a / jr to itself
    assert START + len(code) <= PRINT, len(code)
    rom[START:START + len(code)] = bytes(code)
    # Print: ld a,[hli] / cp End / ret z / ld [de],a / inc de / jr Print
    rom[PRINT:PRINT + 8] = bytes([0x2A, 0xFE, END, 0xC8, 0x12, 0x13, 0x18, 0xF8])
    for name, content in PIECES:
        rom[AT[name]:AT[name] + len(content)] = content
    targets = [AT[name] for name in ENTRIES] + [FAR_A, FAR_B] + [AT[name] for name in
                                                                  ("PINSTR", "NUM1", "NUM2", "NUM3", "NUM4")]
    targets.append(FAR_C)
    for number, target in enumerate(targets):
        address = target if target < BANK else BANK + target % BANK
        rom[AT["TABLE"] + 2 * number:AT["TABLE"] + 2 * number + 2] = bytes(word(address))
    rom[AT["SCRIPT"]:AT["SCRIPT"] + 2] = bytes(word(AT["STR_A"]))
    rom[PAGE:PAGE + 160] = bytes(0x30 + index % 16 for index in range(160))
    rom[FAR_A:FAR_A + len(FAR)] = FAR
    rom[FAR_C], rom[FAR_C + BANK] = 0x99, 0x98
    rom[FAR_A - 1] = 0x21                                   # ld hl,nn, were it run: see the edges file
    rom[CODED] = 0x66
    rom[CODEPTR:CODEPTR + 3] = bytes([*word(CODED), 0xC9])  # add a,b / dec b / ret, to the analysis
    return rom, operands


def run(command: list[str], expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def tool(*arguments: object, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_DATA), *map(str, arguments)], expect=expect)


def play(executable: Path, root: Path, name: str, *flags: str,
         expect: tuple[int, ...] = (0,)) -> tuple[dict, gb_discover.Savestate | None, str]:
    """The machine's state as the game stops, the same as a save state, and what the game said."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump, state = root / f"{name}.json", root / f"{name}.state"
    done = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(FRAMES), "--save-dir",
                str(saves), "--dump-state", str(dump), "--save-state-file", str(state), *flags], expect=expect,
               timeout=180)
    if done.returncode:
        return {}, None, done.stdout + done.stderr
    return json.loads(dump.read_text(encoding="utf-8")), gb_discover.Savestate.load(state), done.stdout + done.stderr


def shown(state: gb_discover.Savestate) -> list[str]:
    """What is on each of the ten rows the game prints to, and the six bytes it keeps in high RAM."""
    rows = []
    for row in range(10):
        tiles = bytes(state.read(0x9800 + 0x20 * row + index) for index in range(14))
        rows.append("".join(" " if tile == SPACE else chr(ord("A") + tile - 0x80) if 0x80 <= tile < 0x90 else "."
                            for tile in tiles).rstrip("."))
    return rows + [bytes(state.read(0xFF80 + index) for index in range(6)).hex()]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]

    with tempfile.TemporaryDirectory(prefix="gbgrow-") as scratch:
        root = Path(scratch)
        rom, operands = make_rom()
        rom_path = root / "grow.gb"
        rom_path.write_bytes(rom)
        # The table is declared data, and an entry point is asked for inside it: no instruction
        # is decoded there, and the game must not hold the image to anything for it.
        annotations = root / "grow.annotations"
        annotations.write_text(f"data 00:{AT['TABLE']:04X} 32\ndata 01:{FAR_A % BANK + BANK:04X} 8\n",
                               encoding="utf-8")
        # And a recorded run's calls are said to have arrived at the byte in front of declared data,
        # which is the first of an instruction three bytes long: it would run on into the data, so
        # it is not decoded, and there is a function with no instruction in it. That is no code for
        # an image to match, whatever the block before it, the routine in high RAM, ended at.
        edges = root / "grow.edges.json"
        edges.write_text(json.dumps({"schema": "gbrecomp.lift-edges", "version": 1,
                                     "rom": {"sha256": hashlib.sha256(rom).hexdigest(), "size": len(rom)},
                                     "edges": [], "entered": [[1, FAR_A % BANK + BANK - 1]]}), encoding="utf-8")
        analysis += ["--lift-edges", str(edges)]
        analysis += ["--annotations", str(annotations), "--add-entry-point", f"0:{AT['TABLE'] + 4:04X}"]
        # And another in the middle of a string, which makes code of the string's end: the tree has
        # that part in a file of code, and the string is still one thing.
        analysis += ["--add-entry-point", f"0:{AT['PINSTR'] + 2:04X}", "--add-entry-point", f"0:{CODEPTR:04X}"]
        export_path = root / "export.json"
        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        run([str(args.gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "grow_game"])
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
             "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
        run(["ninja", "-C", str(project / "build")])
        executable = next(path for path in (project / "build" / "grow_game.exe", project / "build" / "grow_game")
                          if path.exists())

        # The game, plain: every string where it belongs, and the bytes it read through its pointers.
        plain, state, _ = play(executable, root, "plain")
        original = ROWS + [bytes([0x15, 0x13, 0x16, 0x22, 0x77, 0x20]).hex()]
        assert shown(state) == original, shown(state)

        # Recording which bytes made the addresses changes nothing about what the game does, and
        # nothing about the rest of the record.
        plain_record, record_path = root / "plain.use.json", root / "use.json"
        for name, path, flags in (("recorded", plain_record, []), ("pointers", record_path, ["--data-use-pointers"])):
            ended, _, _ = play(executable, root, name, "--data-use", str(path), *flags)
            differ = [key for key in plain if key != "dispatch_fallbacks" and plain[key] != ended.get(key)]
            assert not differ, (name, differ)
        text = record_path.read_text(encoding="utf-8")
        cut = text.index(',\n "pointers": {')
        assert text[:cut] + "\n}\n" == plain_record.read_text(encoding="utf-8")
        _, _, said = play(executable, root, "alone", "--data-use-pointers", expect=(1,))
        assert "--data-use-pointers needs --data-use FILE" in said, said

        record = json.loads(text)
        got = {(link["low"], link["operand"], link["bank"]): link for link in record["pointers"]["links"]}
        table = AT["TABLE"]

        def link(low: int, operand: bool, first: int, last: int, least: int, most: int, reads: int,
                 bank: int = 0) -> dict:
            return {"low": low, "high": low + 1, "operand": operand, "bank": bank, "first": first, "last": last,
                    "least": least, "most": most, "reads": reads}

        expected = {}
        for name, number in (("script", 0), ("script2", 0), ("str_b", 1), ("rec1", 2), ("rec2", 2), ("rec3", 2),
                             ("combo", 3),
                             ("ramstr", 4), ("jumped", 5), ("walk1", 6), ("walk2", 7), ("far_a", 8), ("far_b", 9),
                             ("pinstr", 10), ("num1", 11), ("num2", 12), ("num3", 13), ("num4", 14),
                             ("far_c1", 15), ("far_c2", 15), ("str_b2", 1)):
            # Each use of the table is an operand of its own, and reads the entry's two bytes.
            expected[(operands[name], True)] = link(operands[name], True, table + 2 * number,
                                                    table + 2 * number + 1, 0, 1, 2)
        expected[(operands["imm_str"], True)] = link(operands["imm_str"], True, AT["IMM_STR"], AT["IMM_STR"] + 4, 0,
                                                     4, 5)
        expected[(operands["idx"], True)] = link(operands["idx"], True, AT["IDX"], AT["IDX"], 0, 0, 1)
        expected[(operands["routine"], True)] = link(operands["routine"], True, AT["ROUTINE"], AT["ROUTINE"] + 7,
                                                     0, 7, 8)
        expected[(operands["dmapg"], True)] = link(operands["dmapg"], True, AT["DMAPG"], AT["DMAPG"], 0, 0, 1)
        expected[(operands["codeptr"], True)] = link(operands["codeptr"], True, CODEPTR, CODEPTR + 1, 0, 1, 2)
        expected[(CODEPTR, False)] = link(CODEPTR, False, CODED, CODED, 0, 0, 1)
        expected[(table, False)] = link(table, False, AT["SCRIPT"], AT["SCRIPT"] + 4, 0, 4, 5)
        expected[(AT["SCRIPT"], False)] = link(AT["SCRIPT"], False, AT["STR_A"], AT["STR_A"] + 8, 0, 8, 9)
        # Once more after the stub ran, with what A came back as added: still the pointer's.
        expected[(table + 2, False)] = link(table + 2, False, AT["STR_B"], AT["STR_B"] + 4, 0, 4, 6)
        # A constant added, a number out of RAM added, and a constant added a byte at a time: all the record's,
        # and the lowest byte read is not the first that was.
        expected[(table + 4, False)] = link(table + 4, False, AT["REC"] + 3, AT["REC"] + 6, 3, 6, 3)
        # The byte read with a byte of the ROM added to the pointer is not in this.
        expected[(table + 6, False)] = link(table + 6, False, AT["COMBO"], AT["COMBO"], 0, 0, 1)
        # Through RAM and the stack, it is still the table's two bytes.
        expected[(table + 8, False)] = link(table + 8, False, AT["RAMSTR"], AT["RAMSTR"] + 6, 0, 6, 7)
        expected[(table + 10, False)] = link(table + 10, False, AT["JUMPED"], AT["JUMPED"], 0, 0, 1)
        expected[(table + 12, False)] = link(table + 12, False, AT["WALK1"], AT["WALK2"] + 3, 0, 7, 8)
        expected[(table + 14, False)] = link(table + 14, False, AT["WALK2"], AT["WALK2"] + 3, 0, 3, 4)
        expected[(table + 16, False)] = link(table + 16, False, FAR_A, FAR_A + 3, 0, 3, 4, bank=1)
        expected[(table + 18, False)] = link(table + 18, False, FAR_B, FAR_B + 3, 0, 3, 4, bank=1)
        expected[(table + 20, False)] = link(table + 20, False, AT["PINSTR"], AT["PINSTR"] + 4, 0, 4, 5)
        for number in range(4):
            at = AT[f"NUM{number + 1}"]
            expected[(table + 22 + 2 * number, False)] = link(table + 22 + 2 * number, False, at, at, 0, 0, 1)
        expected = {(low, operand, entry["bank"]): entry for (low, operand), entry in expected.items()}
        # One pair with two banks is two: what was read through it is in each.
        expected[(table + 30, False, 1)] = link(table + 30, False, FAR_C, FAR_C, 0, 0, 1, bank=1)
        expected[(table + 30, False, 2)] = link(table + 30, False, FAR_C + BANK, FAR_C + BANK, 0, 0, 1, bank=2)
        assert got == expected, {key: (got.get(key), expected.get(key)) for key in set(got) | set(expected)
                                 if got.get(key) != expected.get(key)}
        assert len(record["pointers"]["links"]) == len(expected)
        assert record["pointers"]["links"] == sorted(record["pointers"]["links"],
                                                     key=lambda link: (link["low"], link["high"], link["bank"]))
        reached = {offset: tuple(entry["through"]) for entry in record["pointers"]["reached"]
                   for offset in range(entry["offset"], entry["offset"] + entry["size"])}
        wanted: dict[int, tuple] = {}
        for first, count, how in ((table, 32, "operand"), (AT["PINSTR"], 5, "data"), (CODED, 1, "data"),
                                  (CODEPTR, 2, "operand"), (FAR_C, 1, "data"), (FAR_C + BANK, 1, "data"),
                                  (AT["SCRIPT"], 5 + 9 + 5, "data"), (AT["REC"] + 3, 1, "data"),
                                  (AT["REC"] + 5, 2, "data"), (AT["NUM1"], 4 + 7 + 4 + 4 + 1, "data"),
                                  (AT["COMBO"], 1, "data"),
                                  (AT["COMBO"] + 2, 1, "other"), (AT["IDX"], 1 + 5 + 8 + 1, "operand"),
                                  (PAGE, 160, "other"), (FAR_A, 8, "data")):
            wanted.update({offset: (how,) for offset in range(first, first + count)})
        assert reached == wanted, sorted(set(reached.items()) ^ set(wanted.items()))[:8]
        # The bytes that were numbers to the game: compared, or reckoned with. Of the table's, the byte
        # another byte of the ROM was added to, and the four that had a register taken from them, were
        # compared, shifted and masked. Not the ones that only had numbers added, a byte at a time or two.
        numbers = {offset for entry in record["pointers"]["numbers"]
                   for offset in range(entry["offset"], entry["offset"] + entry["size"])}
        assert {offset for offset in numbers if table <= offset < table + 32} == \
            {table + 6, table + 22, table + 25, table + 27, table + 28}, sorted(numbers)[:40]
        assert AT["IDX"] in numbers and AT["STR_A"] in numbers and AT["REC"] + 3 not in numbers
        assert AT["SCRIPT"] not in numbers and AT["SCRIPT"] + 1 not in numbers

        # Into a tree. The pointers need a record that has them.
        tree = root / "tree"
        tool("split", export_path, "--rom", rom_path, "-o", tree)
        assert "recorded without --data-use-pointers" in tool("pointers", tree, plain_record, expect=(2,)).stderr
        assert "nothing says yet what the game reads" in tool("free", tree, expect=(2,)).stderr
        tool("uses", tree, record_path)
        said = tool("pointers", tree, record_path).stdout
        assert "1 record: 45 pairs of bytes made addresses the game read its ROM through, 19 read as data and 26 " \
               "an instruction's operand" in said, said
        assert "39 things reached in data files, 267 bytes of them read" in said, said
        assert "9 can move (" in said and "every read of them came through 10 pointers that lie in data" in said, said
        assert "20 are reached through an instruction's operand" in said, said
        assert "2 were read through an address that no two bytes of the ROM made" in said, said
        assert "5 have a pointer to them that the game uses for something else as well" in said, said
        assert "1 have a pointer to them in a file of code, which no image may change" in said, said
        assert "2 have a pointer to them that shares a byte with another, or is read with two banks" in said, said
        assert "2 bytes more were read in files of code or the header" in said, said
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        things = {thing["offset"]: thing for thing in manifest["things"]}
        assert len(things) == 39 and list(things) == sorted(things), list(things)

        def pointer(low: int, at: int = 0, **more: object) -> dict:
            return dict({"low": low, "high": low + 1, "value": rom[low] | (rom[low + 1] << 8), "at": at}, **more)

        def thing(name: str, size: int, read: list, pointers: list, **more: object) -> dict:
            offset = AT.get(name, {"FAR_A": FAR_A, "FAR_B": FAR_B, "CODED": CODED}.get(name))
            bank = offset // BANK
            return dict({"offset": offset, "size": size, "bank": bank, "address": offset if bank == 0 else
                         BANK + offset % BANK, "read": read, "pointers": pointers, "moves": True}, **more)

        # What can move: each ends where the next pointer leads.
        for name, size, read, pointers in (
                ("SCRIPT", 5, [[0, 5]], [pointer(table)]),
                ("STR_A", 9, [[0, 9]], [pointer(AT["SCRIPT"])]),
                ("STR_B", 5, [[0, 5]], [pointer(table + 2)]),
                ("REC", 8, [[3, 1], [5, 2]], [pointer(table + 4)]),
                ("RAMSTR", 7, [[0, 7]], [pointer(table + 8)]),
                # Printed on through from the first pointer past where the second leads: one thing.
                ("WALK1", 8, [[0, 8]], [pointer(table + 12), pointer(table + 14, at=4)]),
                ("FAR_A", 4, [[0, 4]], [pointer(table + 16)])):
            wanted_thing = thing(name, size, read, pointers)
            assert things[wanted_thing["offset"]] == wanted_thing, things[wanted_thing["offset"]]
        # The last thing of a file runs to the file's end, which is the bank's.
        assert things[FAR_B] == thing("FAR_B", 2 * BANK - FAR_B, [[0, 4]], [pointer(table + 18)]), things[FAR_B]
        # And what cannot, each for its reason.
        for number in range(16):
            entry = things[table + 2 * number]
            assert not entry["moves"] and entry["size"] == 2 and all(one["operand"] for one in entry["pointers"])
            assert "it is reached through an instruction's operand, at 00:" in entry["why"], entry["why"]
        assert [one["low"] for one in things[table + 4]["pointers"]] == [operands[name] for name in
                                                                          ("rec1", "rec2", "rec3")]
        # A pointer the game also jumps through leads to code as well, and the code stays where it is.
        assert things[AT["JUMPED"]] == thing("JUMPED", 1, [[0, 1]], [pointer(table + 10)], moves=False,
                                             why=(f"the pointer to it at 00:{table + 10:04X} is not only a "
                                                  f"pointer: the game also used it for jump")), things[AT["JUMPED"]]
        # A pointer the game reckons with is a number as well, and turning it would change the number.
        for number in range(4):
            assert things[AT[f"NUM{number + 1}"]] == thing(
                f"NUM{number + 1}", 1, [[0, 1]], [pointer(table + 22 + 2 * number)], moves=False,
                why=(f"the pointer to it at 00:{table + 22 + 2 * number:04X} is not only a pointer: the game also "
                     f"compared it or reckoned with it as a number")), things[AT[f"NUM{number + 1}"]]
        assert things[AT["COMBO"]]["why"] == ("1 of its bytes was read through an address that no two bytes of "
                                              "the ROM made"), things[AT["COMBO"]]
        assert things[AT["COMBO"]]["pointers"] == [pointer(table + 6)] and things[AT["COMBO"]]["size"] == 4
        assert "an instruction's operand, at 00:" in things[AT["IMM_STR"]]["why"]
        assert "an instruction's operand, at 00:" in things[AT["ROUTINE"]]["why"]
        assert "an instruction's operand, at 00:" in things[AT["DMAPG"]]["why"] and things[AT["DMAPG"]]["size"] == 1
        # A pointer that lies in a file of code cannot be turned: no image may change it.
        assert things[CODED] == thing("CODED", CODEPTR - CODED, [[0, 1]], [pointer(CODEPTR)], moves=False, why=(
            f"the pointer to it at 00:{CODEPTR:04X} lies in a file of code, which no image may change")), things[CODED]
        # Nor one that was read through with two banks mapped: it leads to two things.
        for offset, size in ((FAR_C, FAR_A - FAR_C), (FAR_C + BANK, 3 * BANK - FAR_C - BANK)):
            assert things[offset]["size"] == size and things[offset]["why"] == (
                f"the pointer to it at 00:{table + 30:04X} was read through with 2 different banks mapped")
        # The string whose end is in a file of code is one thing, and can move: a copy changes no code.
        assert things[AT["PINSTR"]] == thing("PINSTR", 6, [[0, 5]], [pointer(table + 20)], pinned=[[2, 4]])
        # What the hardware copied has no pointer: it is part of what its file starts with.
        assert "160 of its bytes were read through an address" in things[CODEPTR + 3]["why"]
        tool("build", tree, "-o", root / "same.gb", "--expect-original")

        # The empty ends of the banks: all of the last two, and what is behind the data of the first two.
        said = tool("free", tree).stdout
        free_0, free_1 = PAGE + 160 + 16, FAR_B + 4 + 16
        assert f"4 of 4 banks end in a run of one byte value" in said, said
        assert f"bank 00: {BANK - free_0:6,d} bytes of 00 from 00:{free_0:04X}" in said, said
        assert f"bank 01: {2 * BANK - free_1:6,d} bytes of 00 from 01:{free_1 - BANK + BANK:04X}" in said, said
        assert f"bank 03: {BANK:6,d} bytes of 00 from 03:4000" in said, said
        assert not (tree / "free.json").exists()
        tool("free", tree, "-o", tree / "free.json")
        proposed = json.loads((tree / "free.json").read_text(encoding="utf-8"))
        assert proposed == {"schema": "gbrecomp.free-space", "version": 1, "free": [
            {"bank": 0, "address": free_0, "size": BANK - free_0},
            {"bank": 1, "address": free_1, "size": 2 * BANK - free_1},
            # The byte the game read in the third bank is not free, nor the sixteen behind it.
            {"bank": 2, "address": FAR_C + 17, "size": 2 * BANK - FAR_C - 17},
            {"bank": 3, "address": BANK, "size": BANK}]}, proposed
        assert "is already there: --force writes over it" in tool("free", tree, "-o", tree / "free.json",
                                                                  expect=(2,)).stderr
        tool("build", tree, "-o", root / "same.gb", "--expect-original")

        def declare(*runs: tuple[int, int, int]) -> None:
            (tree / "free.json").write_text(json.dumps({"schema": "gbrecomp.free-space", "version": 1, "free": [
                {"bank": bank, "address": address, "size": size} for bank, address, size in runs]}),
                encoding="utf-8")

        # The text, to edit.
        tool("glyphs", tree, root / "plain.state")
        letters = {f"{0x80 + index:02X}": chr(ord("A") + index) for index in range(16)}
        letters.update({f"{SPACE:02X}": " ", f"{END:02X}": "{end}"})
        (tree / "charmap.json").write_text(json.dumps({"schema": "gbrecomp.charmap", "version": 1,
                                                       "letters": letters}), encoding="utf-8")
        tool("text", tree)
        region = next(region for region in manifest["regions"]
                      if region["offset"] <= AT["STR_A"] < region["offset"] + region["size"])
        far_region = next(region for region in manifest["regions"]
                          if region["offset"] <= FAR_A < region["offset"] + region["size"])
        # The string an operand leads to is behind the ret the game jumps to, in the next file.
        imm_region = next(region for region in manifest["regions"]
                          if region["offset"] <= AT["IMM_STR"] < region["offset"] + region["size"])
        assert imm_region is not region
        paths = {key: tree / (entry["file"][:-len(".bin")] + ".text.txt")
                 for key, entry in (("near", region), ("imm", imm_region), ("far", far_region))}
        written = {key: path.read_text(encoding="utf-8") for key, path in paths.items()}
        # Strings that lie one after the other are one line: a line may be of several things.
        assert "|ED{end}BAD CAFE{end}DEAF{end}|" in written["near"] and "|A BEAD{end}FEEDCAB{end}|" in written["near"]
        assert "|FACE{end}|" in written["imm"] and "|BE|" in written["imm"], written["imm"]
        assert "|ACE{end}BED{end}|" in written["far"], written["far"]

        def edit(**changes: list[tuple[str, str]]) -> None:
            """The files of text as they were written, but for these changes: for a file, pairs of old and new."""
            for key, path in paths.items():
                text = written[key]
                for old, new in changes.get(key, ()):
                    assert text.count(old) == 1, (old, text)
                    text = text.replace(old, new)
                path.write_text(text, encoding="utf-8", newline="\n")

        def refused(words: str) -> None:
            stderr = tool("build", tree, "-o", root / "no.gb", expect=(2,)).stderr
            assert words in stderr, (words, stderr)
            assert not (root / "no.gb").exists()
            assert "`build` will refuse this: " in tool("status", tree).stdout

        # A string made longer, with another right behind it: what it is part of goes into the free
        # space, the pointer that leads to it is turned, and nothing else changes.
        spot_0, spot_1 = 0x3F00, 0x7F80
        declare((0, spot_0, 64), (1, spot_1, 32))
        edit(near=[("BAD CAFE{end}", "BAD CAFFEE{end}")])
        said = tool("status", tree).stdout
        assert "|ED{end}BAD CAFFEE{end}DEAF{end}|   <- 2 bytes longer: it has to be moved" in said, said
        assert "`build` will have moved 1 thing into free space, 11 bytes, 2 more than they were, and turned 1 " \
               "pointer to it" in said, said
        image_path = root / "grown.gb"
        said = tool("build", tree, "-o", image_path).stdout
        one_moved = "moved 1 thing into free space, 11 bytes, 2 more than they were, and turned 1 pointer to it"
        assert one_moved in said, said
        assert f"00:{AT['STR_A']:04X} (9 bytes) is at 00:{spot_0:04X}, and 2 longer" in said, said
        assert f"the pointer at 00:{AT['SCRIPT']:04X} said {AT['STR_A']:04X} and says {spot_0:04X}" in said, said
        assert "bank 00 has 53 free bytes left" in said, said
        expected_image = bytearray(rom)
        expected_image[spot_0:spot_0 + 11] = letters_of("BAD CAFFEE") + bytes([END])
        expected_image[AT["SCRIPT"]:AT["SCRIPT"] + 2] = bytes(word(spot_0))
        image = image_path.read_bytes()
        assert image == bytes(expected_image), [hex(index) for index in range(len(rom))
                                                if image[index] != expected_image[index]][:8]
        _, state, _ = play(executable, root, "grown", "--rom-image", str(image_path))
        assert shown(state) == ["BAD CAFFEE"] + original[1:], shown(state)

        # Four at once: the script, whose pointer in front of its string leads to another thing that
        # moves; that thing; one with a pointer into its middle behind what grew; and one in a
        # switched bank, with bytes put in at its very start.
        edit(near=[("ED{end}", "EDGE{end}"), ("BAD CAFE{end}", "BAD CAFFEE{end}"),
                   ("FEEDCAB{end}", "FEEEEDCAB{end}")], far=[("ACE{end}", "PEACE{end}")])
        said = tool("build", tree, "-o", image_path).stdout
        assert "moved 4 things into free space, 34 bytes, 8 more than they were, and turned 5 pointers to them" \
            in said, said
        expected_image = bytearray(rom)
        new_script, new_a, new_walk = spot_0, spot_0 + 7, spot_0 + 18
        expected_image[new_script:new_script + 7] = bytes(word(new_a)) + letters_of("EDGE") + bytes([END])
        expected_image[table:table + 2] = bytes(word(new_script))
        expected_image[AT["SCRIPT"]:AT["SCRIPT"] + 2] = bytes(word(new_a))     # turned where it lies as well
        expected_image[new_a:new_a + 11] = letters_of("BAD CAFFEE") + bytes([END])
        expected_image[new_walk:new_walk + 10] = letters_of("FEEEEDCAB") + bytes([END])
        expected_image[table + 12:table + 14] = bytes(word(new_walk))
        expected_image[table + 14:table + 16] = bytes(word(new_walk + 6))      # into the middle, behind what grew
        expected_image[BANK + spot_1 - BANK:BANK + spot_1 - BANK + 6] = letters_of("PEACE") + bytes([END])
        expected_image[table + 16:table + 18] = bytes(word(spot_1))            # still the start of the string
        image = image_path.read_bytes()
        assert image == bytes(expected_image), [hex(index) for index in range(len(rom))
                                                if image[index] != expected_image[index]][:8]
        _, state, _ = play(executable, root, "grown4", "--rom-image", str(image_path))
        assert shown(state) == ["BAD CAFFEE", "DEAF", "FACE", "A BEAD", "FEEEEDCAB", "CAB", "PEACE", "BED", "BEEF",
                                "EDGE", original[10]], shown(state)
        # The files of the tree are as they were: a build writes an image, and nothing else.
        assert (tree / region["file"]).read_bytes() == bytes(rom[region["offset"]:region["offset"] + region["size"]])

        # What cannot be moved is refused, with the reason.
        edit(imm=[("FACE{end}", "FACADE{end}")])
        refused(f"the string at +{AT['IMM_STR'] - imm_region['offset']:04X} is 7 bytes now and there is room for 5: a "
                f"string cannot grow where it is, and the 5 bytes at 00:{AT['IMM_STR']:04X} it is part of cannot "
                f"be moved: it is reached through an instruction's operand, at 00:{operands['imm_str']:04X}")
        # A change that runs from one thing into the next is of neither.
        edit(near=[("ED{end}BAD", "EDGEFAD")])
        refused(f"what was changed in it runs over the end of one thing into the next, at 00:{AT['STR_A']:04X}")
        edit(near=[("BAD CAFE{end}", "BAD CAFFEE{end}")])
        declare((0, spot_0, 10), (1, spot_1, 32))
        refused(f"there is no room for it: the 9 bytes at 00:{AT['STR_A']:04X} it is part of are 11 with it, and "
                f"the free space declared in bank 00 has 10 in one piece at most")
        declare((1, spot_1, 32))
        refused("the free space declared in bank 00 has none")
        (tree / "free.json").unlink()
        refused("no free space is declared for it to move to")
        # Free space is only what is declared, and only what nothing uses.
        for runs, words in ((((0, AT["STR_B"], 4),), "is bytes a picture or a file of text has"),
                            (((0, START, 16),), "is code or the header"),
                            (((0, AT["REC"], 2),), "is something a pointer leads to"),
                            (((0, table + 2, 2),), "is a pointer"),
                            (((0, 0x3FF0, 32),), "are not inside one bank"),
                            (((0, spot_0, 32), (0, spot_0 + 16, 32)), "free twice")):
            declare(*runs)
            refused(words)
        declare((0, AT["REC"] + 1, 2))
        refused(f"the game read on behind them, as far as 00:{AT['REC'] + 6:04X}")
        declare((0, spot_0, 64), (1, spot_1, 32))
        # A pointer that was edited since is not turned.
        data = bytearray((tree / region["file"]).read_bytes())
        data[AT["SCRIPT"] - region["offset"]] ^= 1
        (tree / region["file"]).write_bytes(data)
        refused(f"the pointer at 00:{AT['SCRIPT']:04X} said {AT['STR_A']:04X} when `pointers` was run and says "
                f"{AT['STR_A'] ^ 1:04X} now")
        # And `pointers`, run on the tree as it is then, has it lead nowhere.
        tool("pointers", tree, record_path)
        strayed = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))["things"]
        assert next(thing for thing in strayed if thing["offset"] == AT["SCRIPT"])["why"] == (
            f"it was read through the pointer at 00:{AT['SCRIPT']:04X}, which does not say now what it said when "
            f"the game was recorded"), strayed
        data[AT["SCRIPT"] - region["offset"]] ^= 1
        (tree / region["file"]).write_bytes(data)
        tool("pointers", tree, record_path)
        assert json.loads((tree / "manifest.json").read_text(encoding="utf-8"))["things"] == manifest["things"]
        # Without the things, nothing says what leads to the string.
        kept = (tree / "manifest.json").read_text(encoding="utf-8")
        bare = json.loads(kept)
        del bare["things"]
        (tree / "manifest.json").write_text(json.dumps(bare), encoding="utf-8")
        refused("a string cannot grow where it is, and no record says what pointers lead to it")
        # Nor is what a record of `uses` shows the game reading free, whether or not a pointer leads to it.
        declare((0, AT["REC"] + 3, 1))
        refused("is bytes the game was seen to read")
        declare((0, spot_0, 64), (1, spot_1, 32))
        edit()
        assert "nothing says what can move" in tool("build", tree, "-o", root / "no.gb", "--move-all",
                                                    expect=(2,)).stderr
        edit(near=[("BAD CAFE{end}", "BAD CAFFEE{end}")])
        # And a manifest that says something else of a thing is refused.
        for change, words in ((lambda thing: thing.update(moves="yes"), "a field of the wrong type"),
                              (lambda thing: thing.update(size=3 * BANK), "are not inside the ROM"),
                              (lambda thing: thing.pop("read"), "lacks the field 'read'"),
                              (lambda thing: thing.update(name="x"), "no such field as 'name'"),
                              (lambda thing: thing["pointers"][0].update(at=99), "that is not where it can be"),
                              (lambda thing: thing.update(pointers=[]), "has no pointer in data to turn")):
            altered = json.loads(kept)
            change(next(thing for thing in altered["things"] if thing["offset"] == AT["STR_A"]))
            (tree / "manifest.json").write_text(json.dumps(altered), encoding="utf-8")
            refused(words)
        (tree / "manifest.json").write_text(kept, encoding="utf-8", newline="\n")
        edit()
        tool("build", tree, "-o", root / "same.gb", "--expect-original")
        assert (root / "same.gb").read_bytes() == bytes(rom)

        # The proof: everything that can move moved, and where it was spoilt. The game shows the same.
        declare((0, spot_0, 64), (1, spot_1, 32))
        moved_path = root / "moved.gb"
        said = tool("build", tree, "-o", moved_path, "--move-all").stdout
        assert "moved 8 things into free space, 52 bytes, and turned 9 pointers to them" in said, said
        assert "the 43 bytes the game was seen to read at the old places are each turned into their opposite" in said
        assert "3 more of them lie in files of code, which no image may change, and are as they were" in said, said
        # What had no room is counted as far as the free space that starts inside it.
        assert f"1 thing that can move had no room in the free space of its bank, {spot_1 - FAR_B % BANK - BANK:,} " \
               f"bytes, and stayed" in said, said
        moved = moved_path.read_bytes()
        expected_image = bytearray(rom)
        # The smallest first: the script, the far string, the string behind it, and so on.
        places = {}
        cursor = spot_0
        for name, size in (("SCRIPT", 5), ("STR_B", 5), ("PINSTR", 6), ("RAMSTR", 7), ("REC", 8), ("WALK1", 8),
                           ("STR_A", 9)):
            places[name] = cursor
            cursor += size
        for name, low, at in (("SCRIPT", table, 0), ("STR_A", AT["SCRIPT"], 0), ("STR_B", table + 2, 0),
                              ("REC", table + 4, 0), ("RAMSTR", table + 8, 0), ("WALK1", table + 12, 0),
                              ("WALK1", table + 14, 4), ("PINSTR", table + 20, 0)):
            expected_image[low:low + 2] = bytes(word(places[name] + at))
        expected_image[table + 16:table + 18] = bytes(word(spot_1))
        sizes = dict((name, len(content)) for name, content in PIECES)
        sizes["WALK1"] = 8
        for name, target in places.items():
            # A copy is of the thing as it is now: the script's has the pointer in it turned.
            expected_image[target:target + sizes[name]] = expected_image[AT[name]:AT[name] + sizes[name]]
        expected_image[BANK + spot_1 - BANK:BANK + spot_1 - BANK + 4] = FAR[:4]
        for first, count in ((AT["SCRIPT"], 19), (AT["REC"] + 3, 1), (AT["REC"] + 5, 2), (AT["RAMSTR"], 15),
                             (AT["PINSTR"], 2), (FAR_A, 4)):
            for offset in range(first, first + count):
                expected_image[offset] = rom[offset] ^ 0xFF
        assert moved == bytes(expected_image), [hex(index) for index in range(len(rom))
                                                if moved[index] != expected_image[index]][:8]
        _, state, _ = play(executable, root, "moved", "--rom-image", str(moved_path))
        assert shown(state) == original, shown(state)
        # With one pointer not turned, the game reads the old place through it, and shows it.
        missed = json.loads(kept)
        walk = next(thing for thing in missed["things"] if thing["offset"] == AT["WALK1"])
        del walk["pointers"][1]
        (tree / "manifest.json").write_text(json.dumps(missed), encoding="utf-8")
        tool("build", tree, "-o", root / "missed.gb", "--move-all")
        _, state, _ = play(executable, root, "missed", "--rom-image", str(root / "missed.gb"))
        assert shown(state) != original and shown(state)[4] == "FEEDCAB" and shown(state)[5] != "CAB", shown(state)
        (tree / "manifest.json").write_text(kept, encoding="utf-8", newline="\n")
        tool("build", tree, "-o", root / "same.gb", "--expect-original")

    print("data pointers end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
