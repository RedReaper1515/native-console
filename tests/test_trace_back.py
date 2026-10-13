#!/usr/bin/env python3
"""A backward trace from a RAM address, recorded and read back, end to end.

Builds a small game that writes watched variables every way the trace tells
apart, each in a function of its own so that what a function decides on is
that function's:

- an entry of a table in a switched bank, picked by another variable;
- a value with a constant added;
- a product from a shift-and-add multiply that leaves it in a register,
  where the multiplier only decides whether an addition happens;
- a value pushed and popped;
- a value that went through a scratch byte which was overwritten before the
  watched write was made: the trace has to name the write that was read;
- a hardware register, one that only the hardware sets and one that holds
  what was written to it;
- a constant chosen by a comparison in a function that returns it;
- a sum of two bytes with the carry of the low halves in the high half;
- a value made of nothing that was there before (`xor a`);
- a store inside two calls, made or not by a test, of a register and of a
  constant no register holds;
- stores that a test guards, that follow a loop, and that follow code a jump
  skipped: only the first is decided by anything;
- a store decided by a value that a test in another function chose: by that
  value, and not by what the other function tested;
- a sum of eight variables made under a test, which is more than a value
  keeps: the eight stay, and what decided is dropped and said to be;
- a carry kept across PUSH AF and POP AF;
- a byte of cartridge RAM;
- a byte set, stepped, shifted and rotated where it lies in memory;
- a byte read with POP while the stack pointer is pointed at data above the
  stack, as a fast copy does: the call it is made in is still in progress;
- bytes copied from the ROM to RAM by a loop, and from there to the sprite
  table by the hardware's own copy, started by a routine that was itself
  copied into high RAM;
- a counter the interrupt routine steps every frame, which starts from RAM
  nothing wrote, a byte that a program playing the game changes from
  outside, and a byte written once, long after what it is made of was.

A game run with `--trace-back` has to end as it does without, alone and
beside `--data-use` and `--evidence`, whose records it must not change; from
a save state; over a recorded run; and under `--control`. The record has to
say exactly the sources of each write, and `tools/gb_trace_back.py` has to
print them.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "gb_trace_back.py"
sys.path.insert(0, str(ROOT / "tools"))

FRAMES = 12

# Variables the game sets from constants
INDEX, MULTIPLIER, MULTIPLICAND, PUSHED_FROM, FIRST, SECOND, SELECT = 0xC000, 0xC002, 0xC003, 0xC004, 0xC005, 0xC006, 0xC007
X_LOW, X_HIGH, Y_LOW, Y_HIGH, SCRATCH, POKED, LATE_FROM = 0xC008, 0xC009, 0xC00A, 0xC00B, 0xC001, 0xC00C, 0xC00D
WORK = 0xC00E
# ...and the watched ones
OUT_TABLE, OUT_CONSTANT, OUT_PRODUCT, OUT_PUSHED, OUT_STALE = 0xC010, 0xC011, 0xC012, 0xC014, 0xC015
OUT_HARDWARE, OUT_HELD, OUT_CHOSEN, OUT_SUM, OUT_ZEROED, OUT_NESTED = 0xC016, 0xC017, 0xC018, 0xC019, 0xC01B, 0xC01C
OUT_CARTRIDGE, COUNTER, OUT_POKED, OUT_LATE, OUT_WORKED = 0xC01D, 0xC01E, 0xC01F, 0xC020, 0xC021
OUT_POPPED, OUT_DIRECT, OUT_GUARDED, OUT_AFTER, OUT_SECOND = 0xC022, 0xC023, 0xC024, 0xC025, 0xC026
OUT_CROWDED, OUT_FLAGS = 0xC027, 0xC028
SAVED_STACK, POPPED_FROM = 0xC030, 0xFFA0
LATE_FRAME = 150
SPRITE_PAGE = 0xC200
CARTRIDGE = 0xA123
HIGH_ROUTINE = 0xFF90
TABLE_BANK, TABLE = 3, 0x4100
SPRITES, ROUTINE = 0x3000, 0x3010
WATCHED = ["C010-C028", "C200-C203", "FE00-FE03"]


class Code:
    """Bytes at an address, with names for places in them."""

    def __init__(self, origin: int) -> None:
        self.origin = origin
        self.items: list = []
        self.labels: dict[str, int] = {}

    def here(self) -> int:
        return self.origin + len(self.items)

    def label(self, name: str) -> None:
        assert name not in self.labels, name
        self.labels[name] = self.here()

    def op(self, name: str | None, *items) -> None:
        """An instruction, named if anything is asked about it."""
        if name:
            self.label(name)
        for item in items:
            if isinstance(item, int):
                self.items.append(item)
            elif item[0] == "word":
                self.items += [("low", item[1]), ("high", item[1])]
            else:
                self.items.append(item)

    def link(self) -> bytes:
        out = bytearray()
        for at, item in enumerate(self.items):
            if isinstance(item, int):
                out.append(item)
                continue
            kind, name = item
            target = name if isinstance(name, int) else self.labels[name]
            if kind == "low":
                out.append(target & 0xFF)
            elif kind == "high":
                out.append(target >> 8)
            else:
                offset = target - (self.origin + at + 1)
                assert -128 <= offset <= 127, (name, offset)
                out.append(offset & 0xFF)
        return bytes(out)


def word(value) -> tuple:
    return ("word", value)


def to(name: str) -> tuple:
    return ("relative", name)


# Eight variables summed into one byte: as many sources as a value keeps.
CROWD = (INDEX, MULTIPLIER, MULTIPLICAND, PUSHED_FROM, FIRST, SECOND, X_LOW, X_HIGH)


def make_rom() -> tuple[bytearray, dict[str, int]]:
    rom = bytearray(0x10000)
    c = Code(0x0150)
    load_a = lambda name, address: c.op(name, 0xFA, word(address))        # ld a,[address]
    store_a = lambda name, address: c.op(name, 0xEA, word(address))       # ld [address],a
    constant_a = lambda name, value: c.op(name, 0x3E, value)              # ld a,value
    call = lambda name, target: c.op(name, 0xCD, word(target))

    c.op(None, 0xF3)                                                      # di
    c.op(None, 0x31, word(0xDFFF))                                        # ld sp,$DFFF
    constant_a(None, TABLE_BANK)
    store_a(None, 0x2000)                                                 # the table's bank
    constant_a(None, 0x0A)
    store_a(None, 0x0000)                                                 # cartridge RAM on
    for name in ("Init", "TableEntry", "AddConstant", "Product", "Pushed", "Stale", "Hardware", "Chosen", "Sum",
                 "Zeroed", "Outer", "Cartridge", "Worked", "Popped", "Joined", "Second", "Crowded", "Flags",
                 "CopySprites", "CopyRoutine"):
        call(f"call_{name}", name)
    call("call_high", HIGH_ROUTINE)
    constant_a(None, 0x01)
    c.op(None, 0xE0, 0xFF)                                                # ldh [$FF],a: the vertical blank's
    c.op(None, 0xAF)                                                      # xor a
    c.op(None, 0xE0, 0x0F)                                                # ldh [$0F],a
    c.op(None, 0xFB)                                                      # ei
    c.label("Forever")
    c.op(None, 0x76)                                                      # halt
    c.op(None, 0x18, to("Forever"))                                       # jr Forever

    c.label("Init")
    for name, address, value in (("index", INDEX, 2), ("multiplier", MULTIPLIER, 11), ("multiplicand", MULTIPLICAND, 7),
                                 ("pushed", PUSHED_FROM, 0x5C), ("first", FIRST, 0x31), ("second", SECOND, 0x42),
                                 ("select", SELECT, 7), ("x_low", X_LOW, 0xF0), ("x_high", X_HIGH, 0x01),
                                 ("y_low", Y_LOW, 0x20), ("y_high", Y_HIGH, 0x02), ("poked", POKED, 0x33),
                                 ("late", LATE_FROM, 0x58)):
        constant_a(f"set_{name}", value)
        store_a(f"init_{name}", address)
    c.op(None, 0xC9)

    c.label("TableEntry")
    load_a(None, INDEX)
    c.op(None, 0x5F)                                                      # ld e,a
    c.op("table_d", 0x16, 0x00)                                           # ld d,0
    c.op("table_hl", 0x21, word(TABLE))                                   # ld hl,Table
    c.op(None, 0x19)                                                      # add hl,de
    c.op(None, 0x7E)                                                      # ld a,[hl]
    store_a("store_table", OUT_TABLE)
    c.op(None, 0xC9)

    c.label("AddConstant")
    load_a(None, OUT_TABLE)
    c.op("add_five", 0xC6, 0x05)                                          # add a,5
    store_a("store_constant", OUT_CONSTANT)
    c.op(None, 0xC9)

    c.label("Product")
    load_a(None, MULTIPLICAND)
    c.op(None, 0x5F)                                                      # ld e,a
    load_a(None, MULTIPLIER)
    call(None, "Multiply")
    c.op(None, 0x7D)                                                      # ld a,l
    store_a("store_product", OUT_PRODUCT)
    c.op(None, 0x7C)                                                      # ld a,h
    store_a("store_product_high", OUT_PRODUCT + 1)
    c.op(None, 0xC9)
    c.label("Multiply")                                                   # hl = a * e
    c.op("multiply_hl", 0x21, word(0x0000))                               # ld hl,0
    c.op("multiply_d", 0x16, 0x00)                                        # ld d,0
    c.op("multiply_b", 0x06, 0x08)                                        # ld b,8
    c.label("MultiplyLoop")
    c.op(None, 0x29)                                                      # add hl,hl
    c.op(None, 0x17)                                                      # rla
    c.op(None, 0x30, to("MultiplySkip"))                                  # jr nc,MultiplySkip
    c.op(None, 0x19)                                                      # add hl,de
    c.label("MultiplySkip")
    c.op(None, 0x05)                                                      # dec b
    c.op(None, 0x20, to("MultiplyLoop"))                                  # jr nz,MultiplyLoop
    c.op(None, 0xC9)

    c.label("Pushed")
    load_a(None, PUSHED_FROM)
    c.op(None, 0x47)                                                      # ld b,a
    c.op(None, 0x0E, 0x99)                                                # ld c,$99
    c.op("push", 0xC5)                                                    # push bc
    c.op(None, 0x06, 0x00)                                                # ld b,0
    c.op(None, 0xD1)                                                      # pop de
    c.op(None, 0x7A)                                                      # ld a,d
    store_a("store_pushed", OUT_PUSHED)
    c.op(None, 0xC9)

    c.label("Stale")
    load_a(None, FIRST)
    store_a("scratch_first", SCRATCH)
    load_a(None, SCRATCH)
    c.op(None, 0x47)                                                      # ld b,a
    load_a(None, SECOND)
    store_a("scratch_second", SCRATCH)                                    # the scratch byte is used again
    c.op(None, 0x78)                                                      # ld a,b
    store_a("store_stale", OUT_STALE)
    c.op(None, 0xC9)

    c.label("Hardware")
    c.op("read_divider", 0xF0, 0x04)                                      # ldh a,[$04]
    store_a("store_hardware", OUT_HARDWARE)
    constant_a("set_palette", 0xE4)
    c.op("store_palette", 0xE0, 0x47)                                     # ldh [$47],a
    c.op("read_palette", 0xF0, 0x47)                                      # ldh a,[$47]
    store_a("store_held", OUT_HELD)
    c.op(None, 0xC9)

    c.label("Chosen")
    call(None, "Choose")
    store_a("store_chosen", OUT_CHOSEN)
    c.op(None, 0xC9)
    c.label("Choose")
    load_a(None, SELECT)
    c.op("compare", 0xFE, 0x07)                                           # cp 7
    c.op(None, 0x28, to("ChooseYes"))                                     # jr z,ChooseYes
    constant_a("chosen_no", 0x11)
    c.op(None, 0xC9)
    c.label("ChooseYes")
    constant_a("chosen_yes", 0x22)
    c.op(None, 0xC9)

    c.label("Sum")
    load_a(None, X_LOW)
    c.op(None, 0x47)                                                      # ld b,a
    load_a(None, Y_LOW)
    c.op(None, 0x80)                                                      # add a,b
    store_a("store_sum", OUT_SUM)
    load_a(None, X_HIGH)
    c.op(None, 0x47)                                                      # ld b,a
    load_a(None, Y_HIGH)
    c.op(None, 0x88)                                                      # adc a,b
    store_a("store_sum_high", OUT_SUM + 1)
    c.op(None, 0xC9)

    c.label("Zeroed")
    load_a(None, FIRST)
    c.op(None, 0xAF)                                                      # xor a
    c.op("add_nine", 0xC6, 0x09)                                          # add a,9
    store_a("store_zeroed", OUT_ZEROED)
    c.op(None, 0xC9)

    c.label("Outer")
    call("call_inner", "Inner")
    c.op("after_inner", 0xC9)
    c.label("Inner")
    load_a(None, SELECT)
    c.op(None, 0xA7)                                                      # and a
    c.op(None, 0x28, to("InnerNone"))                                     # jr z,InnerNone
    constant_a("nested_value", 0x44)
    store_a("store_nested", OUT_NESTED)
    c.op(None, 0x21, word(OUT_DIRECT))                                    # ld hl,OutDirect
    c.op("store_direct", 0x36, 0x55)                                      # ld [hl],$55: no register holds it
    c.label("InnerNone")
    c.op(None, 0xC9)

    c.label("Cartridge")
    constant_a("cartridge_value", 0x6B)
    store_a("store_cartridge", CARTRIDGE)
    c.op(None, 0xAF)                                                      # xor a
    load_a(None, CARTRIDGE)
    store_a("store_from_cartridge", OUT_CARTRIDGE)
    c.op(None, 0xC9)

    c.label("Worked")
    c.op("worked_hl", 0x21, word(WORK))                                   # ld hl,Work
    c.op("worked_set", 0x36, 0x81)                                        # ld [hl],$81
    c.op("worked_inc", 0x34)                                              # inc [hl]
    c.op("worked_srl", 0xCB, 0x3E)                                        # srl [hl]
    c.op("worked_rl", 0xCB, 0x16)                                         # rl [hl]
    c.op(None, 0x7E)                                                      # ld a,[hl]
    store_a("store_worked", OUT_WORKED)
    c.op(None, 0xC9)

    c.label("Joined")
    c.op(None, 0x06, 0x02)                                                # ld b,2
    c.label("JoinedLoop")
    c.op(None, 0x05)                                                      # dec b
    c.op(None, 0x20, to("JoinedLoop"))                                    # jr nz,JoinedLoop: a loop that ends
    c.op(None, 0x21, word(SELECT))                                        # ld hl,Select
    c.op(None, 0x7E)                                                      # ld a,[hl]
    c.op("guard_compare", 0xFE, 0x07)                                     # cp 7
    c.op(None, 0x20, to("JoinedOver"))                                    # jr nz,JoinedOver: not taken
    constant_a("guarded_value", 0x11)
    store_a("store_guarded", OUT_GUARDED)                                 # runs because of the test
    c.label("JoinedOver")
    load_a(None, SELECT)
    c.op(None, 0xFE, 0x09)                                                # cp 9
    c.op(None, 0x20, to("JoinedSkip"))                                    # jr nz,JoinedSkip: taken
    constant_a(None, 0x12)
    store_a(None, OUT_GUARDED)                                            # skipped; it runs on into JoinedSkip
    c.label("JoinedSkip")
    constant_a("after_value", 0x66)
    store_a("store_after", OUT_AFTER)                                     # runs whatever the tests said
    c.op(None, 0xC9)

    c.label("Second")
    call(None, "Choose")                                                  # a holds what a test chose
    c.op(None, 0xA7)                                                      # and a
    c.op(None, 0x28, to("SecondNone"))                                    # jr z,SecondNone
    constant_a("second_value", 0x77)
    store_a("store_second", OUT_SECOND)
    c.label("SecondNone")
    c.op(None, 0xC9)

    c.label("Crowded")
    load_a(None, SELECT)
    c.op("crowded_compare", 0xFE, 0x07)                                   # cp 7
    c.op(None, 0xC0)                                                      # ret nz: the rest runs because of it
    load_a(None, INDEX)
    c.op(None, 0x47)                                                      # ld b,a
    for address in CROWD[1:]:
        load_a(None, address)
        c.op(None, 0x80)                                                  # add a,b
        c.op(None, 0x47)                                                  # ld b,a
    store_a("store_crowded", OUT_CROWDED)                                 # eight values, and two decisions
    c.op(None, 0xC9)

    c.label("Flags")
    load_a(None, X_LOW)
    c.op(None, 0x47)                                                      # ld b,a
    load_a(None, Y_LOW)
    c.op(None, 0x80)                                                      # add a,b: with a carry
    c.op("push_flags", 0xF5)                                              # push af
    c.op(None, 0xAF)                                                      # xor a: the carry is gone
    c.op(None, 0xF1)                                                      # pop af: and back
    constant_a("flags_zero", 0x00)
    c.op("flags_carry", 0xCE, 0x00)                                       # adc a,0
    store_a("store_flags", OUT_FLAGS)
    c.op(None, 0xC9)

    c.label("Popped")
    constant_a("popped_value", 0x9D)
    c.op("popped_store", 0xE0, POPPED_FROM & 0xFF)                        # ldh [PoppedFrom],a
    c.op(None, 0x08, word(SAVED_STACK))                                   # ld [SavedStack],sp
    c.op(None, 0x21, word(POPPED_FROM))                                   # ld hl,PoppedFrom
    c.op(None, 0xF9)                                                      # ld sp,hl: above every call in progress
    c.op(None, 0xD1)                                                      # pop de
    c.op(None, 0x21, word(SAVED_STACK))                                   # ld hl,SavedStack
    c.op(None, 0x2A)                                                      # ld a,[hli]
    c.op(None, 0x66)                                                      # ld h,[hl]
    c.op(None, 0x6F)                                                      # ld l,a
    c.op(None, 0xF9)                                                      # ld sp,hl: back
    c.op(None, 0x7B)                                                      # ld a,e
    store_a("store_popped", OUT_POPPED)
    c.op(None, 0xC9)

    def copying(name: str, source: int, target: int, count: int) -> None:
        c.label(name)
        c.op(f"{name}_hl", 0x21, word(source))                           # ld hl,source
        c.op(None, 0x11, word(target))                                    # ld de,target
        c.op(f"{name}_b", 0x06, count)                                    # ld b,count
        c.label(f"{name}Loop")
        c.op(None, 0x2A)                                                  # ld a,[hli]
        c.op(f"{name}_store", 0x12)                                       # ld [de],a
        c.op(None, 0x13)                                                  # inc de
        c.op(None, 0x05)                                                  # dec b
        c.op(None, 0x20, to(f"{name}Loop"))                               # jr nz,loop
        c.op(None, 0xC9)

    copying("CopySprites", SPRITES, SPRITE_PAGE, 4)
    copying("CopyRoutine", ROUTINE, HIGH_ROUTINE, 10)

    c.label("Handler")
    c.op(None, 0xF5)                                                      # push af
    load_a(None, COUNTER)
    c.op(None, 0x3C)                                                      # inc a
    store_a("store_counter", COUNTER)
    load_a(None, POKED)
    store_a("store_poked", OUT_POKED)
    load_a(None, COUNTER)
    c.op("compare_late", 0xFE, LATE_FRAME)                                # cp LATE_FRAME
    c.op(None, 0x20, to("HandlerDone"))                                   # jr nz,HandlerDone
    load_a(None, LATE_FROM)
    store_a("store_late", OUT_LATE)                                       # once, long after LATE_FROM was set
    c.label("HandlerDone")
    c.op(None, 0xF1)                                                      # pop af
    c.op(None, 0xD9)                                                      # reti

    code = c.link()
    assert 0x0150 + len(code) <= SPRITES
    rom[0x0150:0x0150 + len(code)] = code
    rom[0x0040:0x0043] = bytes([0xC3, c.labels["Handler"] & 0xFF, c.labels["Handler"] >> 8])
    rom[0x0100:0x0104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x0104:0x0134] = NINTENDO_LOGO
    rom[0x0134:0x013E] = b"TRACE BACK"
    rom[0x0147] = 0x03                                                    # MBC1, memory, battery
    rom[0x0148] = 0x01                                                    # four banks
    rom[0x0149] = 0x02                                                    # 8 KiB of memory
    rom[0x014D] = (-sum(rom[0x0134:0x014D]) - 25) & 0xFF
    rom[SPRITES:SPRITES + 4] = bytes([0x20, 0x30, 0x05, 0x00])
    # ld a,$C2 / ldh [$46],a / ld a,$28 / wait: dec a / jr nz,wait / ret
    rom[ROUTINE:ROUTINE + 10] = bytes([0x3E, SPRITE_PAGE >> 8, 0xE0, 0x46, 0x3E, 0x28, 0x3D, 0x20, 0xFD, 0xC9])
    table = TABLE_BANK * 0x4000 + (TABLE & 0x3FFF)
    rom[table:table + 4] = bytes([0x10, 0x20, 0x30, 0x40])
    # The same place in the other banks holds something else: a wrong bank shows.
    for bank in (1, 2):
        rom[bank * 0x4000 + 0x100:bank * 0x4000 + 0x104] = bytes([0xE1, 0xE2, 0xE3, 0xE4])
    return rom, c.labels


def run(command: list, expect: tuple[int, ...] = (0,), timeout: int = 900) -> subprocess.CompletedProcess:
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-3000:]}\n"
                             f"{result.stderr[-3000:]}")
    return result


def play(executable: Path, root: Path, name: str, *flags, frames: int = FRAMES) -> dict:
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    run([executable, "--headless", "--no-audio", "--limit-frames", frames, "--save-dir", saves,
         "--dump-state", dump, *flags], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8"))


def same_state(one: dict, other: dict) -> list[str]:
    return [key for key in one if key != "dispatch_fallbacks" and one[key] != other.get(key)]


def tracing(path: Path, *more) -> list:
    flags: list = ["--trace-back", path]
    for watched in WATCHED:
        flags += ["--trace-back-of", watched]
    return flags + list(more)


class Record:
    """A record, asked what it says."""

    def __init__(self, path: Path) -> None:
        self.data = json.loads(path.read_text(encoding="utf-8"))
        self.events = {event["id"]: event for event in self.data["events_kept"]}

    def writes(self, address: int) -> list[dict]:
        return [write for write in self.data["writes"] if write["address"] == address]

    def write(self, address: int) -> dict:
        found = self.writes(address)
        assert len(found) == 1, (hex(address), found)
        return found[0]

    def root(self, address: int, tree: int = 0) -> dict:
        return self.events[self.write(address)["trees"][tree]["event"]]

    def sources(self, event: dict) -> dict[str, set]:
        """An event's sources by role, each event it names as the address it wrote and where from."""
        by_role: dict[str, set] = {"value": set(), "address": set(), "decision": set()}
        for source in event["sources"]:
            kind, role, *rest = source
            if kind == "ram":
                there = self.events[rest[0]]
                rest = [there["address"], there["at"][1], there["what"]]
            by_role[role].add((kind, *rest))
        return by_role

    def through(self, event: dict, address: int) -> dict:
        """The event among `event`'s sources that wrote `address`."""
        found = [self.events[source[2]] for source in event["sources"]
                 if source[0] == "ram" and self.events[source[2]].get("address") == address]
        assert len(found) == 1, (event, hex(address))
        return found[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work in this folder and leave it: for looking at what was made")
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]

    with tempfile.TemporaryDirectory(prefix="gbback-") as scratch:
        root = args.keep if args.keep else Path(scratch)
        root.mkdir(parents=True, exist_ok=True)
        rom, at = make_rom()
        rom_path = root / "back.gb"
        rom_path.write_bytes(rom)
        project = root / "game"
        run([args.gbrecomp, rom_path, *analysis, "-o", project, "--output-prefix", "back_game"])
        run(["cmake", "-G", "Ninja", "-S", project, "-B", project / "build", "-DCMAKE_BUILD_TYPE=Release",
             *args.cmake_arg])
        run(["ninja", "-C", project / "build"])
        executable = next(path for path in (project / "build" / "back_game.exe", project / "build" / "back_game")
                          if path.exists())

        def constant(name: str, size: int = 1) -> tuple:
            """The operand of the instruction called `name`, as the record has a constant."""
            offset = at[name] + 1
            value = rom[offset] if size == 1 else rom[offset] | (rom[offset + 1] << 8)
            return ("constant", offset, size, value)

        def stored(address: int, name: str, what: str = "store") -> tuple:
            """The event that wrote `address` at the instruction called `name`."""
            return ("ram", address, at[name], what)

        # ---- Tracing changes nothing about what the game does -----------------------------
        plain = play(executable, root, "plain")
        memory = plain["wram_bank_0_c000_cfff"]
        assert memory[OUT_TABLE - 0xC000:OUT_HARDWARE - 0xC000] == [0x30, 0x35, 77, 0, 0x5C, 0x31], memory[16:32]
        assert memory[OUT_HELD - 0xC000:COUNTER - 0xC000] == [0xE4, 0x22, 0x10, 0x04, 9, 0x44, 0x6B], memory[16:32]
        assert memory[COUNTER - 0xC000] == FRAMES - 1 and memory[OUT_POKED - 0xC000] == 0x33, memory[16:33]
        assert memory[OUT_LATE - 0xC000] == 0, memory[16:40]
        assert memory[OUT_WORKED - 0xC000:OUT_FLAGS - 0xBFFF] == [0x82, 0x9D, 0x55, 0x11, 0x66, 0x77, 0xD4, 0x01], \
            memory[16:42]
        record_path = root / "back.json"
        traced = play(executable, root, "traced", *tracing(record_path, "--trace-back-each", 2))
        assert not same_state(plain, traced), same_state(plain, traced)
        use_alone, use_beside = root / "use-alone.json", root / "use-beside.json"
        evidence_alone, evidence_beside = root / "evidence-alone.json", root / "evidence-beside.json"
        alone = play(executable, root, "alone", "--data-use", use_alone, "--evidence", evidence_alone)
        beside = play(executable, root, "beside", "--data-use", use_beside, "--evidence", evidence_beside,
                      *tracing(root / "back-beside.json", "--trace-back-each", 2))
        assert not same_state(plain, alone) and not same_state(plain, beside)
        assert use_alone.read_bytes() == use_beside.read_bytes(), "the data-use record changed beside a trace"
        assert evidence_alone.read_bytes() == evidence_beside.read_bytes(), "the evidence changed beside a trace"
        # ...and the trace is the same trace beside them.
        assert (root / "back-beside.json").read_text(encoding="utf-8").replace("back-beside.json", "back.json") == \
            record_path.read_text(encoding="utf-8"), "the trace changed beside other records"

        record = Record(record_path)
        data = record.data
        assert data["schema"] == "gbrt.trace-back" and data["version"] == 1
        assert data["rom"] == {"size": len(rom), "global_checksum": (rom[0x14E] << 8) | rom[0x14F]}, data["rom"]
        assert data["instructions"] > 300 and data["events"] > 100, (data["instructions"], data["events"])
        assert data["watched"] == [{"first": 0xC010, "last": 0xC028}, {"first": 0xC200, "last": 0xC203},
                                   {"first": 0xFE00, "last": 0xFE03}], data["watched"]
        assert data["state_loads"] == 0 and not data["out_of_memory"] and not data["memory"]["forgetting"]
        assert data["writes_without_site"] == 0 and data["calls_too_deep"] == 0, data
        none: set = set()

        # ---- A table's entry, picked by a variable -----------------------------------------
        write = record.write(OUT_TABLE)
        assert write["at"] == [0, at["store_table"]] and write["count"] == 1 and write["first_value"] == 0x30, write
        assert [call["entered"] for call in write["calls"]] == [[0, at["TableEntry"]]], write["calls"]
        assert write["calls"][0]["returns_to"] == at["call_TableEntry"] + 3, write["calls"]
        assert write["calls"][0]["from"] == [0, at["call_TableEntry"]] and not write["calls"][0]["interrupt"]
        entry = TABLE_BANK * 0x4000 + (TABLE & 0x3FFF) + 2
        event = record.root(OUT_TABLE)
        assert event["value"] == 0x30 and event["what"] == "store" and event["address"] == OUT_TABLE, event
        assert record.sources(event) == {
            "value": {("rom", entry, 0x30)},
            "address": {constant("table_hl", 2), constant("table_d"), stored(INDEX, "init_index")},
            "decision": none}, record.sources(event)
        index = record.through(event, INDEX)
        assert record.sources(index) == {"value": {constant("set_index")}, "address": none, "decision": none}

        # ---- A constant added ---------------------------------------------------------------
        event = record.root(OUT_CONSTANT)
        assert event["value"] == 0x35 and record.sources(event) == {
            "value": {stored(OUT_TABLE, "store_table"), constant("add_five")}, "address": none,
            "decision": none}, record.sources(event)

        # ---- A product: the multiplicand is in it, the multiplier decided ------------------
        low, high = record.root(OUT_PRODUCT), record.root(OUT_PRODUCT + 1)
        assert (low["value"], high["value"]) == (77, 0), (low, high)
        multiplicand = stored(MULTIPLICAND, "init_multiplicand")
        multiplier = stored(MULTIPLIER, "init_multiplier")
        assert record.sources(low) == {
            "value": {constant("multiply_hl", 2), multiplicand},
            "address": none,
            "decision": {multiplier, constant("multiply_b"), constant("multiply_d")}}, record.sources(low)
        assert record.sources(high) == {
            "value": {constant("multiply_hl", 2), multiplicand, constant("multiply_d")},
            "address": none,
            "decision": {multiplier, constant("multiply_b")}}, record.sources(high)
        assert [call["entered"] for call in record.write(OUT_PRODUCT)["calls"]] == [[0, at["Product"]]]

        # ---- Pushed and popped --------------------------------------------------------------
        event = record.root(OUT_PUSHED)
        sources = record.sources(event)
        assert event["value"] == 0x5C and len(sources["value"]) == 1 and not sources["address"] and \
            not sources["decision"], sources
        pushed = record.events[next(source[2] for source in event["sources"])]
        assert pushed["what"] == "push" and pushed["at"] == [0, at["push"]] and pushed["value"] == 0x5C, pushed
        assert record.sources(pushed)["value"] == {stored(PUSHED_FROM, "init_pushed")}, record.sources(pushed)

        # ---- A scratch byte used again: the write that was read, not the last one ----------
        event = record.root(OUT_STALE)
        assert event["value"] == 0x31 and record.sources(event) == {
            "value": {stored(SCRATCH, "scratch_first")}, "address": none, "decision": none}, record.sources(event)
        scratch = record.through(event, SCRATCH)
        assert scratch["value"] == 0x31 and record.sources(scratch)["value"] == {stored(FIRST, "init_first")}, scratch

        # ---- Hardware -----------------------------------------------------------------------
        event = record.root(OUT_HARDWARE)
        assert record.sources(event) == {"value": {("hardware", 0xFF04, 0, at["read_divider"])}, "address": none,
                                         "decision": none}, record.sources(event)
        event = record.root(OUT_HELD)
        assert event["value"] == 0xE4 and record.sources(event) == {
            "value": {("hardware", 0xFF47, 0, at["read_palette"]), stored(0xFF47, "store_palette")},
            "address": none, "decision": none}, record.sources(event)

        # ---- A constant a comparison chose --------------------------------------------------
        event = record.root(OUT_CHOSEN)
        assert event["value"] == 0x22 and record.sources(event) == {
            "value": {constant("chosen_yes")}, "address": none,
            "decision": {stored(SELECT, "init_select"), constant("compare")}}, record.sources(event)

        # ---- A sum: the carry of the low halves is in the high half ------------------------
        low_halves = {stored(X_LOW, "init_x_low"), stored(Y_LOW, "init_y_low")}
        high_halves = {stored(X_HIGH, "init_x_high"), stored(Y_HIGH, "init_y_high")}
        low, high = record.root(OUT_SUM), record.root(OUT_SUM + 1)
        assert (low["value"], high["value"]) == (0x10, 0x04), (low, high)
        assert record.sources(low) == {"value": low_halves, "address": none, "decision": none}, record.sources(low)
        assert record.sources(high) == {"value": low_halves | high_halves, "address": none,
                                        "decision": none}, record.sources(high)

        # ---- Nought, and a constant ---------------------------------------------------------
        event = record.root(OUT_ZEROED)
        assert event["value"] == 9 and record.sources(event) == {
            "value": {constant("add_nine")}, "address": none, "decision": none}, record.sources(event)

        # ---- Inside two calls, after a test -------------------------------------------------
        write = record.write(OUT_NESTED)
        assert [call["entered"] for call in write["calls"]] == [[0, at["Outer"]], [0, at["Inner"]]], write["calls"]
        assert [call["returns_to"] for call in write["calls"]] == [at["call_Outer"] + 3, at["after_inner"]]
        assert write["calls_in_progress"] == 2, write
        event = record.root(OUT_NESTED)
        assert record.sources(event) == {
            "value": {constant("nested_value")}, "address": none,
            "decision": {stored(SELECT, "init_select")}}, record.sources(event)

        event = record.root(OUT_DIRECT)
        assert event["value"] == 0x55 and record.sources(event) == {
            "value": {constant("store_direct")}, "address": none,
            "decision": {stored(SELECT, "init_select")}}, record.sources(event)

        # ---- How far a decision reaches -----------------------------------------------------
        # The code a jump not taken runs into is decided by what the jump tested: the variable and the
        # number it was compared with, and not the address the variable was loaded through.
        event = record.root(OUT_GUARDED)
        assert record.write(OUT_GUARDED)["count"] == 1 and event["value"] == 0x11 and record.sources(event) == {
            "value": {constant("guarded_value")}, "address": none,
            "decision": {stored(SELECT, "init_select"), constant("guard_compare")}}, record.sources(event)
        # What follows the loop, the guarded code and the code a taken jump skipped is decided by none of them.
        event = record.root(OUT_AFTER)
        assert event["value"] == 0x66 and record.sources(event) == {
            "value": {constant("after_value")}, "address": none, "decision": none}, record.sources(event)
        # A test of a value that another function's test chose: decided by the value, not by that test.
        event = record.root(OUT_SECOND)
        assert event["value"] == 0x77 and record.sources(event) == {
            "value": {constant("second_value")}, "address": none,
            "decision": {constant("chosen_yes")}}, record.sources(event)

        # ---- More sources than a value keeps: what decided goes first, and the event says so ----
        event = record.root(OUT_CROWDED)
        names = {INDEX: "index", MULTIPLIER: "multiplier", MULTIPLICAND: "multiplicand", PUSHED_FROM: "pushed",
                 FIRST: "first", SECOND: "second", X_LOW: "x_low", X_HIGH: "x_high"}
        assert event["value"] == sum(memory[address - 0xC000] for address in CROWD) & 0xFF == 0xD4, event
        assert record.sources(event) == {
            "value": {stored(address, f"init_{names[address]}") for address in CROWD}, "address": none,
            "decision": none}, record.sources(event)
        assert event.get("more_decisions") is True and "more" not in event, event
        assert "more_decisions" not in record.root(OUT_GUARDED) and "more" not in record.root(OUT_GUARDED)

        # ---- The flags over the stack: the carry of a sum, pushed, lost, popped and added in ----
        event = record.root(OUT_FLAGS)
        assert event["value"] == 1 and record.sources(event) == {
            "value": {constant("flags_zero"), constant("flags_carry"), stored(0xDFFB, "push_flags", "push")},
            "address": none, "decision": none}, record.sources(event)
        assert record.sources(record.through(event, 0xDFFB)) == {
            "value": {stored(X_LOW, "init_x_low"), stored(Y_LOW, "init_y_low")}, "address": none,
            "decision": none}, record.sources(record.through(event, 0xDFFB))

        # ---- Cartridge RAM ------------------------------------------------------------------
        event = record.root(OUT_CARTRIDGE)
        assert event["value"] == 0x6B and record.sources(event) == {
            "value": {stored(CARTRIDGE, "store_cartridge")}, "address": none, "decision": none}, record.sources(event)

        # ---- Worked on in memory: each write is made of the one before ----------------------
        through_hl = {constant("worked_hl", 2)}
        event = record.root(OUT_WORKED)
        assert event["value"] == 0x82 and record.sources(event) == {
            "value": {stored(WORK, "worked_rl")}, "address": through_hl, "decision": none}, record.sources(event)
        rotated = record.through(event, WORK)
        # The rotate takes in the carry the shift put out, which was a bit of the byte the shift read.
        assert rotated["value"] == 0x82 and record.sources(rotated) == {
            "value": {stored(WORK, "worked_srl"), stored(WORK, "worked_inc")}, "address": through_hl,
            "decision": none}, record.sources(rotated)
        shifted = next(record.events[source[2]] for source in rotated["sources"]
                       if source[0] == "ram" and record.events[source[2]]["at"][1] == at["worked_srl"])
        assert shifted["value"] == 0x41 and record.sources(shifted) == {
            "value": {stored(WORK, "worked_inc")}, "address": through_hl, "decision": none}, record.sources(shifted)
        stepped = record.through(shifted, WORK)
        assert stepped["value"] == 0x82 and record.sources(stepped) == {
            "value": {stored(WORK, "worked_set")}, "address": through_hl, "decision": none}, record.sources(stepped)
        assert record.sources(record.through(stepped, WORK)) == {
            "value": {constant("worked_set")}, "address": none, "decision": none}

        # ---- Read with POP, the stack pointer away from the stack: the call is not over ----
        write = record.write(OUT_POPPED)
        assert [call["entered"] for call in write["calls"]] == [[0, at["Popped"]]], write["calls"]
        event = record.root(OUT_POPPED)
        assert event["value"] == 0x9D and record.sources(event) == {
            "value": {stored(POPPED_FROM, "popped_store")}, "address": none, "decision": none}, record.sources(event)

        # ---- Copied by a loop, then by the hardware, started from code in RAM --------------
        for index in range(4):
            copied = record.root(SPRITE_PAGE + index)
            assert copied["at"] == [0, at["CopySprites_store"]], copied
            assert record.sources(copied) == {
                "value": {("rom", SPRITES + index, rom[SPRITES + index])},
                "address": {constant("CopySprites_hl", 2)},
                "decision": {constant("CopySprites_b")} if index else none}, (index, record.sources(copied))
            sprite = record.root(0xFE00 + index)
            assert sprite["what"] == "oam_copy" and sprite["at"] == [0, HIGH_ROUTINE + 2], sprite
            assert sprite["value"] == rom[SPRITES + index], sprite
            assert record.sources(sprite) == {
                "value": {("ram", SPRITE_PAGE + index, at["CopySprites_store"], "store")},
                "address": {("constant_in_ram", HIGH_ROUTINE + 1, 1, SPRITE_PAGE >> 8)},
                "decision": none}, record.sources(sprite)
            assert [call["entered"] for call in record.write(0xFE00 + index)["calls"]] == [[0, HIGH_ROUTINE]]

        # ---- The interrupt's counter: RAM nothing wrote, then its own last write -----------
        write = record.write(COUNTER)
        assert write["count"] >= FRAMES - 2 and write["first_value"] == 1, write
        assert len(write["calls"]) == 1 and write["calls"][0]["interrupt"], write["calls"]
        assert write["calls"][0]["entered"] == [0, 0x0040], write["calls"]
        assert len(write["trees"]) == 2, write["trees"]
        first, second = record.root(COUNTER, 0), record.root(COUNTER, 1)
        assert record.sources(first)["value"] == {("start", COUNTER, first["cell"])}, record.sources(first)
        assert record.sources(second)["value"] == {stored(COUNTER, "store_counter")}, record.sources(second)
        assert second["frame"] == first["frame"] + 1, (first, second)
        event = record.root(OUT_POKED)
        assert event["value"] == 0x33 and record.sources(event)["value"] == {stored(POKED, "init_poked")}

        # ---- Bounded: fewer trees than writes, and a ring that forgets ----------------------
        small_path = root / "small-record.json"
        small_state = play(executable, root, "small", *tracing(small_path, "--trace-back-trees", 3,
                                                               "--trace-back-nodes", 2, "--trace-back-depth", 1))
        assert not same_state(plain, small_state)
        small = Record(small_path)
        assert sum(len(write["trees"]) for write in small.data["writes"]) == 3, small.data["writes"]
        assert small.data["writes_without_tree"] == small.data["watched_writes"] - 3, small.data
        assert small.data["watched_writes"] == data["watched_writes"]
        cut = {address: small.write(address)["trees"] for address in (OUT_TABLE, OUT_CONSTANT, OUT_PRODUCT)}
        # The table's entry has one event behind it; the sum has one behind that; the product has two.
        assert [(tree["nodes"], tree["cut_by_depth"], tree["cut_by_nodes"]) for tree in cut[OUT_TABLE]] == [(2, 0, 0)]
        assert [(tree["nodes"], tree["cut_by_depth"], tree["cut_by_nodes"]) for tree in cut[OUT_CONSTANT]] == [(2, 1, 0)]
        assert [(tree["nodes"], tree["cut_by_depth"], tree["cut_by_nodes"]) for tree in cut[OUT_PRODUCT]] == [(2, 0, 1)]
        unkept = small.write(OUT_PRODUCT + 1)
        assert unkept["trees"] == [] and unkept["count"] == 1 and unkept["first_value"] == 0, unkept
        # A ring of 16 KiB is written over long before the late write is made: what that is made of was
        # stored in the first frame, and the trace has forgotten it, and says so. What decided it is recent.
        assert not record.writes(OUT_LATE)
        long_plain = play(executable, root, "long-plain", frames=LATE_FRAME + 10)
        assert long_plain["wram_bank_0_c000_cfff"][OUT_LATE - 0xC000] == 0x58
        for megabytes, forgets in (("16k", True), ("1", False)):
            ring_path = root / f"ring-{megabytes}-record.json"
            long_traced = play(executable, root, f"long-{megabytes}",
                               *tracing(ring_path, "--trace-back-memory", megabytes), frames=LATE_FRAME + 10)
            assert not same_state(long_plain, long_traced)
            ring = Record(ring_path)
            words = 2048 if forgets else 131072
            assert ring.data["memory"]["kilobytes"] * 128 == ring.data["memory"]["words"] == words, ring.data["memory"]
            assert ring.data["memory"]["forgetting"] == forgets and ring.data["memory"]["words_written"] > 2048
            late = ring.write(OUT_LATE)
            assert late["count"] == 1 and late["first_value"] == 0x58 and late["first_frame"] == LATE_FRAME, late
            # Two ends are lost: the value's one source, and the far end of the counter's own chain of writes.
            assert late["trees"][0]["forgotten"] == (2 if forgets else 0), late
            event = ring.root(OUT_LATE)
            behind = [ring.events[source[2]] for source in event["sources"] if source[0] == "ram"]
            assert [there.get("forgotten", False) for there in behind if "address" not in there] == [True] * forgets
            assert [there["address"] for there in behind if "address" in there] == [LATE_FROM] * (not forgets) + [COUNTER]
            assert ("constant", "decision", at["compare_late"] + 1, 1, LATE_FRAME) in map(tuple, event["sources"])
            # ...and the first trees, taken when the ring was fresh, are whole.
            assert ring.sources(ring.root(OUT_TABLE)) == record.sources(record.root(OUT_TABLE))
        refused = run([executable, "--headless", "--trace-back", root / "no.json", "--trace-back-of", "C010",
                       "--trace-back-memory", "8k"], expect=(1,))
        assert "the options of --trace-back are" in refused.stderr, refused.stderr

        # ---- From a save state: what was in RAM is "as it was" -----------------------------
        state = root / "at-six.state"
        play(executable, root, "to-state", "--save-state-file", state, frames=6)
        resumed_plain = play(executable, root, "resumed-plain", "--load-state-file", state, frames=5)
        resumed_path = root / "resumed-record.json"
        resumed_state = play(executable, root, "resumed", "--load-state-file", state, *tracing(resumed_path),
                             frames=5)
        assert not same_state(resumed_plain, resumed_state), same_state(resumed_plain, resumed_state)
        resumed = Record(resumed_path)
        assert resumed.data["state_loads"] == 1, resumed.data["state_loads"]
        assert {write["address"] for write in resumed.data["writes"]} == {COUNTER, OUT_POKED}, resumed.data["writes"]
        event = resumed.root(COUNTER)
        assert resumed.sources(event)["value"] == {("start", COUNTER, event["cell"])}, resumed.sources(event)
        event = resumed.root(OUT_POKED)
        assert resumed.sources(event)["value"] == {("start", POKED, event["cell"] - (OUT_POKED - POKED))}

        # ---- Over a recorded run: the replay still ends where the recording did ------------
        run_dir = root / "run"
        shutil.rmtree(run_dir, ignore_errors=True)       # a kept folder may hold another game's run
        run([executable, "--headless", "--record-run", run_dir, "--limit-frames", FRAMES])
        replay_saves = root / "saves-replay"
        replay_saves.mkdir(exist_ok=True)
        replayed_path = root / "replayed.json"
        replayed = run([executable, "--headless", "--replay-run", run_dir / "segment-0001.run", "--save-dir",
                        replay_saves, *tracing(replayed_path, "--trace-back-each", 2)])
        said = re.search(r"^\[RUN\] result (.*)$", replayed.stdout + replayed.stderr, re.M)
        assert said, replayed.stderr[-2000:]
        result = dict(part.split("=", 1) for part in said.group(1).split())
        assert result["end"] == "matched" and result["late"] == "0", result
        assert Record(replayed_path).sources(Record(replayed_path).root(OUT_TABLE)) == \
            record.sources(record.root(OUT_TABLE))

        # ---- Played by a program: a byte changed from outside is told from one the game wrote
        from gb_control import Game
        control_saves = root / "saves-control"
        control_saves.mkdir(exist_ok=True)
        controlled_path = root / "controlled.json"
        with Game([executable, "--headless", "--no-audio", "--save-dir", control_saves,
                   *tracing(controlled_path, "--trace-back-each", 16, "--trace-back-trees", 200)], log=root / "control.log") as game:
            game.step(frames=4)
            assert game.read(OUT_POKED)[0] == 0x33
            game.write(POKED, 0x77)
            game.step(frames=3)
            assert game.read(OUT_POKED)[0] == 0x77
            # A state loaded half way: what the trace knew of memory is gone with the machine it knew.
            game.save_state(root / "half-way.state")
            game.step(frames=2)
            game.load_state(root / "half-way.state")
            game.step(frames=2)
        controlled = Record(controlled_path)
        assert controlled.data["state_loads"] == 1, controlled.data["state_loads"]
        trees = controlled.write(OUT_POKED)["trees"]
        kinds = []
        for tree in trees:
            event = controlled.events[tree["event"]]
            (kind, _role, *rest), = event["sources"][:1]
            kinds.append((event["value"], kind))
        assert (0x33, "ram") in kinds and (0x77, "unseen") in kinds, kinds
        assert kinds.index((0x77, "unseen")) > kinds.index((0x33, "ram")), kinds
        assert kinds[-2:] == [(0x77, "start"), (0x77, "start")], kinds

        # ---- What is asked for wrongly is refused -------------------------------------------
        refused = run([executable, "--headless", "--trace-back", root / "no.json"], expect=(1,))
        assert "go together" in refused.stderr, refused.stderr
        refused = run([executable, "--headless", "--trace-back-of", "C010"], expect=(1,))
        assert "go together" in refused.stderr, refused.stderr
        refused = run([executable, "--headless", "--trace-back", root / "no.json", "--trace-back-of", "0100"],
                      expect=(1,))
        assert "from 8000 up" in refused.stderr, refused.stderr
        refused = run([executable, "--headless", "--differential", "100", *tracing(root / "no.json")], expect=(1,))
        assert "--trace-back is for normal execution" in refused.stderr, refused.stderr

        # ---- The tool ------------------------------------------------------------------------
        symbols = root / "names.sym"
        symbols.write_text("\n".join(f"00:{address:04X} {name}" for name, address in at.items()
                                     if name[0].isupper() and "_" not in name) +
                           f"\n00:{OUT_TABLE:04X} wOutTable\n00:{INDEX:04X} wIndex\n00:{OUT_PRODUCT:04X} wProduct\n"
                           f"00:{MULTIPLIER:04X} wMultiplier\n00:{MULTIPLICAND:04X} wMultiplicand\n"
                           f"{TABLE_BANK:02X}:{TABLE:04X} Table\n", encoding="utf-8")
        shown = run([sys.executable, TOOL, "show", record_path, "--sym", symbols, "--rom", rom_path,
                     "--of", f"{OUT_TABLE:04X}"]).stdout
        assert f"wOutTable ({OUT_TABLE:04X}) = $30" in shown, shown
        assert f"00:{at['store_table']:04X} TableEntry+" in shown, shown
        assert "ROM 03:4102 Table+2 = $30" in shown, shown
        assert "wIndex" in shown and "constant $4100" in shown and "(Table)" in shown, shown
        assert "ld [$C010], a" in shown, shown
        product = run([sys.executable, TOOL, "show", record_path, "--sym", symbols,
                       "--of", f"{OUT_PRODUCT:04X}-{OUT_PRODUCT + 1:04X}"]).stdout
        assert f"wProduct ({OUT_PRODUCT:04X}-{OUT_PRODUCT + 1:04X}) = $4D $00" in product, product
        assert "wMultiplicand" in product and "wMultiplier" in product, product
        assert re.search(r"decided by.*wMultiplier", product), product
        values_only = run([sys.executable, TOOL, "show", record_path, "--sym", symbols, "--no-decisions",
                           "--of", f"{OUT_PRODUCT:04X}-{OUT_PRODUCT + 1:04X}"]).stdout
        assert "wMultiplicand" in values_only and "wMultiplier" not in values_only, values_only
        for count, same_as in ((0, values_only), (1, product), (5, product)):
            limited = run([sys.executable, TOOL, "show", record_path, "--sym", symbols, "--decisions", count,
                           "--of", f"{OUT_PRODUCT:04X}-{OUT_PRODUCT + 1:04X}"]).stdout
            assert limited == same_as, (count, limited)
        # An export and a data tree beside the record: the export's functions and instructions, and the
        # file of the tree a ROM byte is in.
        export_path = root / "export.json"
        run([args.gbrecomp, rom_path, *analysis, "--lift-export", export_path])
        tree = root / "tree"
        shutil.rmtree(tree, ignore_errors=True)
        run([sys.executable, ROOT / "tools" / "gb_data.py", "split", export_path, "--rom", rom_path, "-o", tree])
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
        region = next(region for region in manifest["regions"]
                      if region["offset"] <= entry < region["offset"] + region["size"])
        told = run([sys.executable, TOOL, "show", record_path, "--export", export_path, "--tree", tree,
                    "--of", f"{OUT_TABLE:04X}"]).stdout
        assert f"written at 00:{at['store_table']:04X} func_{at['TableEntry']:04x}+11: ld [$C010], a" in told, told
        assert f"called from 00:{at['call_TableEntry']:04X} func_0150+" in told, told
        assert f"ROM 03:4102 = $30  [{region['file']}+{entry - region['offset']}]" in told, told
        as_json = json.loads(run([sys.executable, TOOL, "show", record_path, "--sym", symbols, "--json",
                                  "--of", f"{OUT_TABLE:04X}"]).stdout)
        assert as_json["writes"][0]["summary"]["rom"] == [
            {"offset": entry, "size": 1, "bank": TABLE_BANK, "address": TABLE + 2, "name": "Table+2",
             "roles": ["value"], "decisions": 0}], as_json["writes"][0]["summary"]
        assert [step["name"] for step in as_json["writes"][0]["summary"]["functions"]] == ["TableEntry", "Init"]
        summary = run([sys.executable, TOOL, "summary", record_path, small_path, "--sym", symbols]).stdout
        assert f"wOutTable ({OUT_TABLE:04X}): 2 writes in 2 records" in summary, summary
        assert f"00:{at['store_table']:04X} TableEntry+" in summary and "03:4102 Table+2" in summary, summary
        other = json.loads(record_path.read_text(encoding="utf-8"))
        other["schema"] = "gbrt.data-use"
        (root / "other.json").write_text(json.dumps(other), encoding="utf-8")
        refused = run([sys.executable, TOOL, "show", root / "other.json"], expect=(2,))
        assert "is not a trace-back record" in refused.stderr, refused.stderr
    print("trace back end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
