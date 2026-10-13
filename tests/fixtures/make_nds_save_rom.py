#!/usr/bin/env python3
"""Create a small, repository-owned Nintendo DS ROM that writes a save.

A 128 KiB image whose ARM9 program takes the card slot, writes A5 5A C3 3C
to the start of the save chip over AUXSPI (write enable, then a write
command with a two-byte address), waits for the chip, reads the first two
bytes back, stores them in main RAM and shows the verdict as the backdrop
colour of both screens; then it idles in a branch to itself. The ARM7
program is a branch to itself. Used where a test needs a game that writes a
save and draws a frame that is not black.

The game code is "####", which is in no table of save chips, so the runtime
has to recognise the chip from the write: two address bytes and four data
bytes make it a 64 KiB EEPROM (runtime-nds/src/nds_save.c).

What a test can check:

    the save starts A5 5A C3 3C
    0x02000400 holds 0x5AA5         (the two bytes read back, little-endian)
    every pixel of both screens is green (0x03E0); red (0x001F) if the bytes
    read back were wrong
    the ARM9 ends in a branch to itself at IDLE_ADDRESS

There is no logo in the header (it is Nintendo's), so the logo checksum is
wrong; the runtime starts programs directly and never looks at it.

With --relocate-arm7 the ARM7 program is one that copies its second half to
work RAM at RELOCATED_ADDRESS and runs it there, as homebrew start-up code
does: a loop of RELOCATED_INSTRUCTIONS different additions to r4. A
recompiler finds that code only by seeing where it ran.

    python tests/fixtures/make_nds_save_rom.py --output build-ci/nds_save.nds
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct

ROM_SIZE = 0x20000
ARM9_OFFSET, ARM9_ADDRESS = 0x200, 0x02000000
ARM7_OFFSET, ARM7_ADDRESS = 0x400, 0x02380000
SAVE = (0xA5, 0x5A, 0xC3, 0x3C)
VERDICT_ADDRESS = 0x02000400
GREEN, RED = 0x03E0, 0x001F
RELOCATED_ADDRESS = 0x037F8000
RELOCATED_INSTRUCTIONS = 20

AL, EQ, NE = 0xE, 0x0, 0x1


def rotated(value: int) -> int:
    """An ARM immediate: eight bits rotated right by an even amount."""
    for rotation in range(16):
        byte = ((value << (2 * rotation)) | (value >> (32 - 2 * rotation))) & 0xFFFFFFFF
        if byte < 0x100:
            return (rotation << 8) | byte
    raise ValueError(f"{value:#x} is not an ARM immediate")


class Program:
    """Just enough of an ARM assembler for the program below."""

    def __init__(self, base: int) -> None:
        self.base = base
        self.words: list[int] = []
        self.labels: dict[str, int] = {}
        self.branches: list[tuple[int, str]] = []

    def label(self, name: str) -> None:
        self.labels[name] = len(self.words)

    def address(self, name: str) -> int:
        return self.base + 4 * self.labels[name]

    def data(self, opcode: int, rd: int, rn: int, value: int, cond: int = AL) -> None:
        """A data-processing instruction with an immediate operand."""
        self.words.append((cond << 28) | 0x02000000 | (opcode << 21) | (rn << 16) | (rd << 12) | rotated(value))

    def mov(self, rd: int, value: int, cond: int = AL) -> None:
        self.data(0xD, rd, 0, value, cond)

    def add(self, rd: int, rn: int, value: int) -> None:
        self.data(0x4, rd, rn, value)

    def orr(self, rd: int, rn: int, value: int) -> None:
        self.data(0xC, rd, rn, value)

    def bic(self, rd: int, rn: int, value: int) -> None:
        self.data(0xE, rd, rn, value)

    def and_(self, rd: int, rn: int, value: int) -> None:
        self.data(0x0, rd, rn, value)

    def tst(self, rn: int, value: int) -> None:
        self.words.append(0xE3100000 | (rn << 16) | rotated(value))

    def mov_reg(self, rd: int, rm: int) -> None:
        self.words.append(0xE1A00000 | (rd << 12) | rm)

    def orr_lsl(self, rd: int, rn: int, rm: int, shift: int) -> None:
        self.words.append(0xE1800000 | (rn << 16) | (rd << 12) | (shift << 7) | rm)

    def cmp_reg(self, rn: int, rm: int) -> None:
        self.words.append(0xE1500000 | (rn << 16) | rm)

    def strh(self, rd: int, rn: int, offset: int = 0) -> None:
        self.words.append(0xE1C000B0 | (rn << 16) | (rd << 12) | ((offset >> 4) << 8) | (offset & 15))

    def ldrh(self, rd: int, rn: int, offset: int = 0) -> None:
        self.words.append(0xE1D000B0 | (rn << 16) | (rd << 12) | ((offset >> 4) << 8) | (offset & 15))

    def str_(self, rd: int, rn: int, offset: int = 0) -> None:
        self.words.append(0xE5800000 | (rn << 16) | (rd << 12) | offset)

    def branch(self, name: str, cond: int = AL, link: bool = False) -> None:
        self.branches.append((len(self.words), name))
        self.words.append((cond << 28) | (0x0B000000 if link else 0x0A000000))

    def bx_lr(self) -> None:
        self.words.append(0xE12FFF1E)

    def word(self, word: int) -> None:
        self.words.append(word)

    def code(self) -> bytes:
        words = list(self.words)
        for index, name in self.branches:
            words[index] |= (self.labels[name] - index - 2) & 0xFFFFFF
        return b"".join(struct.pack("<I", word) for word in words)


def arm9_program() -> Program:
    p = Program(ARM9_ADDRESS)

    def send(byte: int, last: bool = False) -> None:
        """Exchange one byte with the save chip; r0 = the byte it returned."""
        p.mov(0, byte)
        p.mov_reg(1, 7 if last else 8)  # the last byte of a command releases chip select
        p.branch("exchange", link=True)

    p.mov(4, 0x04000000)             # r4 = the I/O registers
    p.add(5, 4, 0x200)
    p.ldrh(0, 5, 4)                  # EXMEMCNT: the ARM9 owns the card slot
    p.bic(0, 0, 0x800)
    p.strh(0, 5, 4)
    p.add(6, 4, 0x1A0)               # r6 = AUXSPICNT, AUXSPIDATA at +2
    p.mov(7, 0xA000)                 # r7 = port on, serial mode
    p.orr(8, 7, 0x40)                # r8 = the same, chip select held
    send(0x06, last=True)            # write enable
    send(0x02)                       # write, at address 0x0000
    send(0x00)
    send(0x00)
    for index, byte in enumerate(SAVE):
        send(byte, last=index == len(SAVE) - 1)
    p.label("wait")                  # until the status register's busy bit clears
    send(0x05)
    send(0x00, last=True)
    p.tst(0, 0x01)
    p.branch("wait", NE)
    send(0x03)                       # read, from address 0x0000
    send(0x00)
    send(0x00)
    send(0x00)
    p.mov_reg(9, 0)
    send(0x00, last=True)
    p.orr_lsl(9, 9, 0, 8)            # r9 = the two bytes read back
    p.mov(10, 0x02000000)
    p.str_(9, 10, VERDICT_ADDRESS - 0x02000000)
    p.add(0, 4, 0x300)               # POWCNT1: both screens and both 2D engines on
    p.mov(1, 0x8200)
    p.orr(1, 1, 0x03)
    p.strh(1, 0, 4)
    p.mov(1, 0x10000)                # DISPCNT: display on, no layers, so the backdrop shows
    p.str_(1, 4, 0)
    p.add(0, 4, 0x1000)
    p.str_(1, 0, 0)
    p.mov(0, (SAVE[1] << 8))
    p.orr(0, 0, SAVE[0])
    p.cmp_reg(9, 0)
    p.mov(3, GREEN & 0xFF00, EQ)     # 0x03E0 in two steps: it is not one immediate
    p.data(0xC, 3, 3, GREEN & 0x00FF, EQ)
    p.mov(3, RED, NE)
    p.mov(2, 0x05000000)             # the backdrop: palette entry 0 of each engine
    p.strh(3, 2, 0)
    p.add(2, 2, 0x400)
    p.strh(3, 2, 0)
    p.label("idle")
    p.branch("idle")

    p.label("exchange")              # r0 = byte to send, r1 = AUXSPICNT value
    p.strh(1, 6, 0)
    p.strh(0, 6, 2)
    p.label("busy")
    p.ldrh(2, 6, 0)
    p.tst(2, 0x80)
    p.branch("busy", NE)
    p.ldrh(0, 6, 2)
    p.and_(0, 0, 0xFF)
    p.bx_lr()
    return p


def arm7_relocating_program() -> Program:
    p = Program(ARM7_ADDRESS)
    p.mov(0, ARM7_ADDRESS)           # r0 = where the second half was loaded: fixed up below
    p.word(0)
    p.mov(1, 0x03800000)             # r1 = where it runs
    p.data(0x2, 1, 1, 0x03800000 - RELOCATED_ADDRESS)
    p.mov(2, RELOCATED_INSTRUCTIONS + 1)
    p.label("copy")
    p.word(0xE4903004)               # ldr r3, [r0], #4
    p.word(0xE4813004)               # str r3, [r1], #4
    p.word(0xE2522001)               # subs r2, r2, #1
    p.branch("copy", NE)
    p.mov(1, 0x03800000)
    p.data(0x2, 1, 1, 0x03800000 - RELOCATED_ADDRESS)
    p.word(0xE12FFF11)               # bx r1
    p.label("payload")               # position-independent: runs at RELOCATED_ADDRESS
    for index in range(RELOCATED_INSTRUCTIONS):
        p.add(4, 4, index + 1)
    p.branch("payload")
    p.words[1] = 0xE2800000 | rotated(4 * p.labels["payload"])  # add r0, r0, #payload
    return p


ARM9 = arm9_program()
IDLE_ADDRESS = ARM9.address("idle")


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_rom(relocate_arm7: bool = False) -> bytes:
    arm9 = ARM9.code()
    arm7 = arm7_relocating_program().code() if relocate_arm7 else struct.pack("<I", 0xEAFFFFFE)  # b .
    assert ARM9_OFFSET + len(arm9) <= ARM7_OFFSET

    rom = bytearray(b"\xFF" * ROM_SIZE)
    rom[0:0x200] = bytes(0x200)
    rom[0x000:0x00A] = b"NCSAVETEST"
    rom[0x00C:0x010] = b"####"
    rom[0x010:0x012] = b"00"
    struct.pack_into("<IIII", rom, 0x020, ARM9_OFFSET, ARM9_ADDRESS, ARM9_ADDRESS, len(arm9))
    struct.pack_into("<IIII", rom, 0x030, ARM7_OFFSET, ARM7_ADDRESS, ARM7_ADDRESS, len(arm7))
    struct.pack_into("<II", rom, 0x080, ARM7_OFFSET + len(arm7), 0x200)  # bytes in use, header size
    struct.pack_into("<H", rom, 0x15E, crc16(bytes(rom[0:0x15E])))
    rom[ARM9_OFFSET:ARM9_OFFSET + len(arm9)] = arm9
    rom[ARM7_OFFSET:ARM7_OFFSET + len(arm7)] = arm7
    return bytes(rom)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--relocate-arm7", action="store_true", help="an ARM7 program that runs from work RAM")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_rom(args.relocate_arm7))
    print(f"wrote {args.output} ({ROM_SIZE} bytes, ARM9 idles at {IDLE_ADDRESS:#010x})")


if __name__ == "__main__":
    main()
