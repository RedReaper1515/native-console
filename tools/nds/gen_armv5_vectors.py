#!/usr/bin/env python3
"""Generate per-instruction ARMv5TE test vectors for the DS's ARM9.

There is no public per-instruction vector set for ARMv5TE (SingleStepTests
covers the ARM7TDMI only), so this script makes one. It runs each test
instruction once on unicorn's ARM946 model, which is QEMU's ARM front end,
and records the state before and after, and the data accesses in order. The
files are in the SingleStepTests ARM7TDMI .json.bin format with a different
magic (D33DBAE5) that says "data accesses only": tests/nds/sst_arm.c reads
them with --cpu 9.

    python tools/nds/gen_armv5_vectors.py                  # roms/nds/armv5-vectors/
    python tools/nds/gen_armv5_vectors.py --count 2000 --only arm_ldm
    python tools/nds/gen_armv5_vectors.py --list

Needs unicorn (pip install unicorn==2.1.4). unicorn is GPL-2.0: it is run
here as a tool, and only its outputs are used. The output is deterministic
for a given unicorn version, seed and count.

What the vectors are evidence of. QEMU implements the architecture, not the
ARM946E-S: where the architecture manual says UNPREDICTABLE, QEMU's answer
says nothing about the DS. So every class below stays inside what ARMv5TE
defines: aligned addresses, no R15 where the manual forbids it, no base
register in a written-back register list, no reserved PSR bits. What the DS
does outside that (and it is deterministic there) is pinned by
tests/nds/test_nds_cpu.c from GBATEK and hardware-verified test ROMs.

Three things unicorn cannot show and the vectors therefore leave out: taking
an exception (SWI, BKPT; unicorn stops instead of entering the handler),
CP15 (QEMU's generic ARM946 has different cache and TCM ID values), and the
two halves of a Thumb BL as a pair (the harness steps one halfword).

The "undefined" classes are different in kind: the encodings come from the
manual's list of what ARMv5TE leaves undefined, a test is kept only if
unicorn also rejects the encoding, and the expected state is the
architectural undefined-instruction entry.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import struct
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_OUT = REPO_ROOT / "roms" / "nds" / "armv5-vectors"

MAGIC = 0xD33DBAE5
GENERATOR_VERSION = 1

M32 = 0xFFFFFFFF

# Everything a test can touch is inside one mapping, so no test faults.
#
# The layout works around two faults in unicorn 2.1.4, both found while
# building this generator and both able to corrupt vectors silently:
#
# 1. A memory write does not invalidate code already translated from those
#    bytes. Every code write here goes through Oracle.put_code, which drops
#    the translations.
# 2. unicorn translates the block at a branch target, and the rest of the
#    block after the instruction under test, without running them. If such
#    a block contains a Thumb IT instruction and ends inside the IT block,
#    unicorn stops generating its stop checks, and later runs execute whole
#    blocks instead of one instruction. So nothing but the instruction under
#    test and FILL is ever translated: the whole mapping is filled with an
#    instruction that ends a block at once in either instruction set, every
#    test puts back what it wrote, and the data window, which holds random
#    bytes, is beyond the reach of any branch a test can make.
#
# Oracle.canary checks both, and generation stops if either ever shows.
MAP_BASE = 0x00000000
MAP_SIZE = 0x04800000
CODE_LO = 0x02000000  # instructions under test live in [CODE_LO, CODE_HI)
CODE_HI = 0x02100000
DATA_LO = 0x04400000  # more than a B/BL away from the code window
DATA_HI = 0x04500000
DATA_MID = 0x04480000  # data accesses land within +-0x8000 of here
VECTOR_BASE = 0xFFFF0000

# "b ." in ARM state. Read as Thumb it is a BL suffix and a BLX suffix:
# each ends a translation block, and neither is an IT instruction.
FILL = 0xEAFFFFFE
THUMB_HALT = 0xE7FE  # "b ." in Thumb state

MODE_USR, MODE_FIQ, MODE_IRQ, MODE_SVC = 0x10, 0x11, 0x12, 0x13
MODE_ABT, MODE_UND, MODE_SYS = 0x17, 0x1B, 0x1F
PRIVILEGED_WITH_SPSR = (MODE_FIQ, MODE_IRQ, MODE_SVC, MODE_ABT, MODE_UND)
ALL_MODES = (MODE_USR, MODE_FIQ, MODE_IRQ, MODE_SVC, MODE_ABT, MODE_UND, MODE_SYS)
MODE_WEIGHTS = (15, 10, 10, 15, 10, 10, 30)

CPSR_T = 1 << 5
CPSR_F = 1 << 6
CPSR_I = 1 << 7
CPSR_Q = 1 << 27
CPSR_C = 1 << 29

INTERESTING = (0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
               0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF,
               0x55555555, 0xAAAAAAAA, 0x3FFFFFFF, 0x40000000, 0xC0000000, 0xBFFFFFFF)


class Discard(Exception):
    """The generated case turned out not to be usable; make another."""


# ----------------------------------------------------------------------
# CPU state in the vectors' canonical layout
# ----------------------------------------------------------------------

class State:
    """R holds the user-mode view; every banked slot is separate."""

    __slots__ = ("r", "fiq", "svc", "abt", "irq", "und", "cpsr", "spsr")

    def __init__(self):
        self.r = [0] * 16
        self.fiq = [0] * 7   # r8-r14
        self.svc = [0] * 2   # r13, r14
        self.abt = [0] * 2
        self.irq = [0] * 2
        self.und = [0] * 2
        self.cpsr = MODE_SYS
        self.spsr = [MODE_USR] * 5  # fiq, svc, abt, irq, und

    def copy(self) -> "State":
        other = State()
        other.r = list(self.r)
        other.fiq = list(self.fiq)
        other.svc = list(self.svc)
        other.abt = list(self.abt)
        other.irq = list(self.irq)
        other.und = list(self.und)
        other.cpsr = self.cpsr
        other.spsr = list(self.spsr)
        return other

    def slot(self, reg: int):
        """The list and index holding register reg in the current mode."""
        mode = self.cpsr & 0x1F
        if reg <= 7 or reg == 15:
            return self.r, reg
        if mode == MODE_FIQ:
            return self.fiq, reg - 8
        if reg <= 12:
            return self.r, reg
        bank = {MODE_SVC: self.svc, MODE_ABT: self.abt, MODE_IRQ: self.irq, MODE_UND: self.und}.get(mode)
        return (bank, reg - 13) if bank is not None else (self.r, reg)

    def get(self, reg: int) -> int:
        lst, index = self.slot(reg)
        return lst[index]

    def set(self, reg: int, value: int) -> None:
        lst, index = self.slot(reg)
        lst[index] = value & M32

    def words(self, pipe0: int, pipe1: int) -> list:
        """The 40 words of a state block."""
        return (self.r + self.fiq + self.svc + self.abt + self.irq + self.und +
                [self.cpsr] + self.spsr + [pipe0, pipe1, 0])


SPSR_INDEX = {MODE_FIQ: 0, MODE_SVC: 1, MODE_ABT: 2, MODE_IRQ: 3, MODE_UND: 4}


def random_value(rng: random.Random) -> int:
    if rng.randrange(6) == 0:
        return rng.choice(INTERESTING)
    return rng.getrandbits(32)


def random_psr(rng: random.Random, mode: int | None = None, thumb: bool | None = None) -> int:
    """A PSR value using only the bits ARMv5TE has."""
    if mode is None:
        mode = rng.choices(ALL_MODES, MODE_WEIGHTS)[0]
    value = mode | (rng.getrandbits(5) << 27) | (rng.getrandbits(2) << 6)
    if thumb is None:
        thumb = bool(rng.getrandbits(1))
    return value | (CPSR_T if thumb else 0)


def random_state(rng: random.Random, thumb: bool) -> State:
    st = State()
    st.r = [random_value(rng) for _ in range(16)]
    st.fiq = [random_value(rng) for _ in range(7)]
    st.svc = [random_value(rng) for _ in range(2)]
    st.abt = [random_value(rng) for _ in range(2)]
    st.irq = [random_value(rng) for _ in range(2)]
    st.und = [random_value(rng) for _ in range(2)]
    st.cpsr = random_psr(rng, thumb=thumb)
    st.spsr = [random_psr(rng) for _ in range(5)]
    return st


# ----------------------------------------------------------------------
# One test case under construction
# ----------------------------------------------------------------------

class Case:
    def __init__(self, rng: random.Random, thumb: bool):
        self.rng = rng
        self.thumb = thumb
        self.st = random_state(rng, thumb)
        step = 2 if thumb else 4
        self.addr = rng.randrange(CODE_LO + 0x1000, CODE_HI - 0x1000, step)
        self.mem: list = []   # (address, bytes) written to the data window before the run
        self.code: list = []  # (address, bytes) written to the code window, and put back after

    # -- registers and flags ------------------------------------------------
    @property
    def pc(self) -> int:
        """What R15 reads as during the instruction."""
        return (self.addr + (4 if self.thumb else 8)) & M32

    def get(self, reg: int) -> int:
        return self.pc if reg == 15 else self.st.get(reg)

    def set(self, reg: int, value: int) -> None:
        assert reg != 15
        self.st.set(reg, value)

    def mode(self, *choices: int) -> int:
        """Put the CPU in one of the given modes. Call before set()."""
        mode = self.rng.choice(choices)
        self.st.cpsr = (self.st.cpsr & ~0x1F) | mode
        return mode

    @property
    def carry(self) -> int:
        return (self.st.cpsr >> 29) & 1

    def reg(self, exclude=(), lo: int = 0, hi: int = 14) -> int:
        while True:
            r = self.rng.randint(lo, hi)
            if r not in exclude:
                return r

    def cond(self) -> int:
        """A condition field: usually one that passes, sometimes any of 0-14."""
        return self.rng.randrange(15) << 28 if self.rng.randrange(4) == 0 else 0xE << 28

    # -- memory ------------------------------------------------------------
    def data_addr(self, align: int, span: int = 0) -> int:
        """An address in the data window with room for span bytes after it."""
        base = DATA_MID + self.rng.randrange(-0x8000, 0x8000 - span)
        return base & ~(align - 1)

    def poke32(self, addr: int, value: int) -> None:
        self.mem.append((addr, struct.pack("<I", value & M32)))

    def literal(self, addr: int) -> None:
        """Put a random word where a PC-relative load will read."""
        self.code.append((addr & ~3, struct.pack("<I", random_value(self.rng))))

    def jump_target(self, arm_only: bool = False, thumb_only: bool = False) -> int:
        """A value to load into R15: bit 0 picks the instruction set, and an
        ARM target is word-aligned (anything else is unpredictable). Never
        the instruction under test: only filler may be translated."""
        while True:
            target = self.rng.randrange(CODE_LO, CODE_HI, 4)
            if abs(target - self.addr) > 64:
                break
        to_thumb = thumb_only or (not arm_only and self.rng.getrandbits(1))
        if to_thumb:
            return (target + self.rng.choice((0, 2))) | 1
        return target


def shift_imm(kind: int, value: int, amount: int, carry: int) -> int:
    """The value a shifter operand with an immediate shift produces."""
    if kind == 0:
        return (value << amount) & M32
    if kind == 1:
        return 0 if amount == 0 else value >> amount
    if kind == 2:
        signed = value - (1 << 32) if value & 0x80000000 else value
        return (signed >> (amount or 32)) & M32
    if amount == 0:
        return (value >> 1) | (carry << 31)
    return ((value >> amount) | (value << (32 - amount))) & M32


def shift_amount_byte(rng: random.Random) -> int:
    """A register shift amount, weighted towards the interesting ones."""
    pick = rng.randrange(8)
    if pick == 0:
        return 0
    if pick == 1:
        return 32
    if pick == 2:
        return rng.randint(33, 255)
    return rng.randint(1, 31)


# ----------------------------------------------------------------------
# ARM classes
# ----------------------------------------------------------------------

DP_TEST = (8, 9, 10, 11)  # TST TEQ CMP CMN: S is set and Rd is zero
DP_NO_RN = (13, 15)       # MOV MVN: Rn is zero


def arm_dp_fields(c: Case):
    opcode = c.rng.randrange(16)
    s = 1 if opcode in DP_TEST else c.rng.getrandbits(1)
    return opcode, s


def arm_dp_imm(c: Case) -> int:
    opcode, s = arm_dp_fields(c)
    rd = 0 if opcode in DP_TEST else c.reg()
    rn = 0 if opcode in DP_NO_RN else c.rng.randint(0, 15)
    return c.cond() | (1 << 25) | (opcode << 21) | (s << 20) | (rn << 16) | (rd << 12) | c.rng.getrandbits(12)


def arm_dp_imm_shift(c: Case) -> int:
    opcode, s = arm_dp_fields(c)
    rd = 0 if opcode in DP_TEST else c.reg()
    rn = 0 if opcode in DP_NO_RN else c.rng.randint(0, 15)
    rm = c.rng.randint(0, 15)
    amount = c.rng.choice((0, 0, 1, 31)) if c.rng.getrandbits(1) else c.rng.randrange(32)
    return (c.cond() | (opcode << 21) | (s << 20) | (rn << 16) | (rd << 12) |
            (amount << 7) | (c.rng.randrange(4) << 5) | rm)


def arm_dp_reg_shift(c: Case) -> int:
    # R15 is unpredictable in every position here.
    opcode, s = arm_dp_fields(c)
    rd = 0 if opcode in DP_TEST else c.reg()
    rn = 0 if opcode in DP_NO_RN else c.reg()
    rm = c.reg()
    rs = c.reg()
    c.set(rs, (c.rng.getrandbits(24) << 8) | shift_amount_byte(c.rng))
    return (c.cond() | (opcode << 21) | (s << 20) | (rn << 16) | (rd << 12) |
            (rs << 8) | (c.rng.randrange(4) << 5) | 0x10 | rm)


# Opcodes that keep a result near their operands, so the branch target stays
# inside the mapping: AND EOR SUB ADD ADC SBC ORR MOV BIC.
DP_PC_OPCODES = (0, 1, 2, 4, 5, 6, 12, 13, 14)


def arm_dp_pc_operands(c: Case, s: int) -> int:
    # With S, QEMU implements the exception return for SUBS and MOVS only
    # (all ARMv7 allows) and treats the other opcodes as plain flag-setting
    # branches. ARMv5 defines it for every opcode, and the handler makes no
    # distinction; tests/nds/test_nds_cpu.c checks the others.
    opcode = c.rng.choice((2, 13) if s else DP_PC_OPCODES)
    rn = 0 if opcode in DP_NO_RN else c.reg()
    if opcode not in DP_NO_RN:
        c.set(rn, c.rng.randrange(CODE_LO, CODE_HI))
    if c.rng.getrandbits(1):
        operand = (1 << 25) | c.rng.getrandbits(8)  # an unrotated 8-bit immediate
    else:
        rm = c.reg(exclude=(rn,) if opcode not in DP_NO_RN else ())
        small = opcode not in DP_NO_RN
        c.set(rm, c.rng.getrandbits(12) if small else c.rng.randrange(CODE_LO, CODE_HI))
        operand = rm  # LSL #0
    return c.cond() | (opcode << 21) | (s << 20) | (rn << 16) | (15 << 12) | operand


def arm_dp_pc(c: Case) -> int:
    """Rd = R15 without S: a branch that keeps the instruction set."""
    return arm_dp_pc_operands(c, 0)


def arm_dp_pc_s(c: Case) -> int:
    """Rd = R15 with S: exception return, CPSR from SPSR."""
    c.mode(*PRIVILEGED_WITH_SPSR)
    return arm_dp_pc_operands(c, 1)


def arm_mrs(c: Case) -> int:
    spsr = c.rng.getrandbits(1)
    if spsr:
        c.mode(*PRIVILEGED_WITH_SPSR)
    return c.cond() | 0x010F0000 | (spsr << 22) | (c.reg() << 12)


def arm_msr_reg(c: Case) -> int:
    spsr = c.rng.getrandbits(1)
    if spsr:
        c.mode(*PRIVILEGED_WITH_SPSR)
    rm = c.reg()
    c.set(rm, random_psr(c.rng))
    return c.cond() | 0x0120F000 | (spsr << 22) | (c.rng.getrandbits(4) << 16) | rm


def arm_msr_imm(c: Case) -> int:
    spsr = c.rng.getrandbits(1)
    if spsr:
        c.mode(*PRIVILEGED_WITH_SPSR)
    if c.rng.getrandbits(1):
        # The flags byte: an 8-bit value rotated to the top, reserved bits clear.
        imm = (4 << 8) | (c.rng.getrandbits(5) << 3)
        mask = 8 | (c.rng.getrandbits(2) << 1)
    else:
        # The control byte, which has to name a real mode.
        imm = random_psr(c.rng) & 0xFF
        mask = 1 | (c.rng.getrandbits(3) << 1)
    return c.cond() | 0x0320F000 | (spsr << 22) | (mask << 16) | imm


def arm_mul_mla(c: Case) -> int:
    accumulate = c.rng.getrandbits(1)
    rd = c.reg()
    rm = c.reg(exclude=(rd,))
    rs = c.reg()
    rn = c.reg() if accumulate else 0
    return (c.cond() | (accumulate << 21) | (c.rng.getrandbits(1) << 20) | (rd << 16) | (rn << 12) |
            (rs << 8) | 0x90 | rm)


def arm_mull_mlal(c: Case) -> int:
    rdlo = c.reg()
    rdhi = c.reg(exclude=(rdlo,))
    rm = c.reg(exclude=(rdlo, rdhi))
    rs = c.reg()
    return (c.cond() | (1 << 23) | (c.rng.getrandbits(3) << 20) | (rdhi << 16) | (rdlo << 12) |
            (rs << 8) | 0x90 | rm)


def arm_dsp_mul(c: Case) -> int:
    kind = c.rng.randrange(4)
    rd = c.reg()
    rn = c.reg(exclude=(rd,))
    rs = c.reg()
    rm = c.reg()
    xy = c.rng.getrandbits(2) << 5
    if kind in (1, 3) and (kind == 3 or (xy & 0x20)):
        rn = 0  # SMULxy and SMULWy have no accumulator
    return c.cond() | 0x01000080 | (kind << 21) | (rd << 16) | (rn << 12) | (rs << 8) | xy | rm


def arm_qarith(c: Case) -> int:
    return (c.cond() | 0x01000050 | (c.rng.randrange(4) << 21) | (c.reg() << 16) |
            (c.reg() << 12) | c.reg())


def arm_clz(c: Case) -> int:
    rm = c.reg()
    if c.rng.getrandbits(1):
        c.set(rm, c.rng.getrandbits(32) >> c.rng.randrange(33))
    return c.cond() | 0x016F0F10 | (c.reg() << 12) | rm


def sdt_common(c: Case, load: int, byte: int, rd: int | None):
    """Shared by the LDR/STR classes: returns (flags, rn, rd, pre, up, target)."""
    pre = c.rng.getrandbits(1)
    up = c.rng.getrandbits(1)
    writeback = c.rng.getrandbits(1) if pre else 0  # post-indexed with W is LDRT/STRT
    rn = c.reg()
    if rd is None:
        rd = c.reg(exclude=(rn,) if (writeback or not pre) else ())
    target = c.data_addr(1 if byte else 4)
    flags = (pre << 24) | (up << 23) | (byte << 22) | (writeback << 21) | (load << 20)
    return flags, rn, rd, pre, up, target


def arm_ldr_str_imm(c: Case) -> int:
    load = c.rng.getrandbits(1)
    byte = c.rng.getrandbits(1)
    if load and c.rng.randrange(6) == 0:
        # A literal load: [pc, #+-imm].
        imm = c.rng.getrandbits(12) & (0xFFF if byte else 0xFFC)
        up = c.rng.getrandbits(1)
        c.literal(c.pc + imm if up else c.pc - imm)
        return (c.cond() | 0x05000000 | (up << 23) | (byte << 22) | (1 << 20) |
                (15 << 16) | (c.reg() << 12) | imm)
    flags, rn, rd, pre, up, target = sdt_common(c, load, byte, None)
    imm = c.rng.getrandbits(12)
    c.set(rn, (target - imm if up else target + imm) if pre else target)
    return c.cond() | 0x04000000 | flags | (rn << 16) | (rd << 12) | imm


def arm_ldr_str_reg(c: Case) -> int:
    load = c.rng.getrandbits(1)
    byte = c.rng.getrandbits(1)
    flags, rn, rd, pre, up, target = sdt_common(c, load, byte, None)
    rm = c.reg(exclude=(rn,))
    kind = c.rng.randrange(4)
    amount = c.rng.randrange(32)
    offset = shift_imm(kind, c.get(rm), amount, c.carry)
    c.set(rn, (target - offset if up else target + offset) if pre else target)
    return c.cond() | 0x06000000 | flags | (rn << 16) | (rd << 12) | (amount << 7) | (kind << 5) | rm


def arm_ldr_pc(c: Case) -> int:
    """LDR into R15: bit 0 of the loaded word picks the instruction set."""
    flags, rn, rd, pre, up, target = sdt_common(c, 1, 0, 15)
    c.poke32(target, c.jump_target())
    if c.rng.getrandbits(1):
        imm = c.rng.getrandbits(12)
        c.set(rn, (target - imm if up else target + imm) if pre else target)
        return c.cond() | 0x04000000 | flags | (rn << 16) | (15 << 12) | imm
    rm = c.reg(exclude=(rn,))
    kind = c.rng.randrange(4)
    amount = c.rng.randrange(32)
    offset = shift_imm(kind, c.get(rm), amount, c.carry)
    c.set(rn, (target - offset if up else target + offset) if pre else target)
    return c.cond() | 0x06000000 | flags | (rn << 16) | (15 << 12) | (amount << 7) | (kind << 5) | rm


def arm_half(c: Case, kinds) -> int:
    """LDRH/STRH/LDRSB/LDRSH. kinds is a tuple of (L, SH)."""
    load, sh = c.rng.choice(kinds)
    align = 1 if sh == 2 else 2
    pre = c.rng.getrandbits(1)
    up = c.rng.getrandbits(1)
    writeback = c.rng.getrandbits(1) if pre else 0
    immediate = c.rng.getrandbits(1)
    if load and immediate and c.rng.randrange(8) == 0:
        imm = c.rng.getrandbits(8) & ~(align - 1)
        up = c.rng.getrandbits(1)
        c.literal(c.pc + imm if up else c.pc - imm)
        return (c.cond() | 0x01400090 | (up << 23) | (1 << 20) | (15 << 16) |
                (c.reg() << 12) | ((imm & 0xF0) << 4) | (sh << 5) | (imm & 0xF))
    rn = c.reg()
    rd = c.reg(exclude=(rn,) if (writeback or not pre) else ())
    target = c.data_addr(align)
    op = (c.cond() | (pre << 24) | (up << 23) | (immediate << 22) | (writeback << 21) | (load << 20) |
          (rn << 16) | (rd << 12) | 0x90 | (sh << 5))
    if immediate:
        offset = c.rng.getrandbits(8)
        op |= ((offset & 0xF0) << 4) | (offset & 0xF)
    else:
        rm = c.reg(exclude=(rn,))
        offset = c.get(rm)
        op |= rm
    c.set(rn, (target - offset if up else target + offset) if pre else target)
    return op


def arm_ldrh_strh(c: Case) -> int:
    return arm_half(c, ((0, 1), (1, 1)))


def arm_ldrsb_ldrsh(c: Case) -> int:
    return arm_half(c, ((1, 2), (1, 3)))


def arm_ldrd_strd(c: Case) -> int:
    store = c.rng.getrandbits(1)
    pre = c.rng.getrandbits(1)
    up = c.rng.getrandbits(1)
    writeback = c.rng.getrandbits(1) if pre else 0
    immediate = c.rng.getrandbits(1)
    rd = c.rng.randrange(0, 13, 2)  # even, and not r14
    rn = c.reg(exclude=(rd, rd + 1))
    target = c.data_addr(8, 8)
    op = (c.cond() | (pre << 24) | (up << 23) | (immediate << 22) | (writeback << 21) |
          (rn << 16) | (rd << 12) | 0xD0 | (store << 5))
    if immediate:
        offset = c.rng.getrandbits(8)
        op |= ((offset & 0xF0) << 4) | (offset & 0xF)
    else:
        rm = c.reg(exclude=(rn, rd, rd + 1))
        offset = c.get(rm)
        op |= rm
    c.set(rn, (target - offset if up else target + offset) if pre else target)
    return op


def block_base(pre: int, up: int, count: int, lowest: int) -> int:
    """The base register value that makes the lowest transfer address `lowest`."""
    if up:
        return lowest - 4 if pre else lowest
    return lowest + 4 * count if pre else lowest + 4 * count - 4


def random_rlist(c: Case, candidates) -> int:
    rlist = 0
    while not rlist:
        for reg in candidates:
            if c.rng.randrange(3) == 0:
                rlist |= 1 << reg
    return rlist


def arm_ldm_stm(c: Case) -> int:
    load = c.rng.getrandbits(1)
    pre = c.rng.getrandbits(1)
    up = c.rng.getrandbits(1)
    writeback = c.rng.getrandbits(1)
    rn = c.reg()
    # A base register in the list is only defined without writeback.
    rlist = random_rlist(c, [r for r in range(15) if not (writeback and r == rn)])
    count = bin(rlist).count("1")
    lowest = c.data_addr(4, 64)
    c.set(rn, block_base(pre, up, count, lowest))
    return (c.cond() | 0x08000000 | (pre << 24) | (up << 23) | (writeback << 21) | (load << 20) |
            (rn << 16) | rlist)


def arm_ldm_stm_user(c: Case) -> int:
    """S set and no R15: the user bank is transferred. No writeback."""
    c.mode(*PRIVILEGED_WITH_SPSR)
    load = c.rng.getrandbits(1)
    pre = c.rng.getrandbits(1)
    up = c.rng.getrandbits(1)
    rn = c.reg()
    rlist = random_rlist(c, [r for r in range(15) if r != rn])
    count = bin(rlist).count("1")
    lowest = c.data_addr(4, 64)
    c.set(rn, block_base(pre, up, count, lowest))
    return c.cond() | 0x08400000 | (pre << 24) | (up << 23) | (load << 20) | (rn << 16) | rlist


def arm_ldm_pc_common(c: Case, s: int) -> int:
    pre = c.rng.getrandbits(1)
    up = c.rng.getrandbits(1)
    writeback = c.rng.getrandbits(1)
    rn = c.reg()
    rlist = (1 << 15)
    for reg in range(15):
        if reg != rn and c.rng.randrange(4) == 0:
            rlist |= 1 << reg
    count = bin(rlist).count("1")
    lowest = c.data_addr(4, 64)
    c.set(rn, block_base(pre, up, count, lowest))
    c.poke32(lowest + 4 * (count - 1), c.jump_target())
    return (c.cond() | 0x08100000 | (pre << 24) | (up << 23) | (s << 22) | (writeback << 21) |
            (rn << 16) | rlist)


def arm_ldm_pc(c: Case) -> int:
    """LDM with R15: bit 0 of the loaded word picks the instruction set."""
    return arm_ldm_pc_common(c, 0)


def arm_ldm_pc_s(c: Case) -> int:
    """LDM with R15 and S: exception return."""
    c.mode(*PRIVILEGED_WITH_SPSR)
    return arm_ldm_pc_common(c, 1)


def arm_swp(c: Case) -> int:
    byte = c.rng.getrandbits(1)
    rn = c.reg()
    rd = c.reg(exclude=(rn,))
    rm = c.reg(exclude=(rn,))
    c.set(rn, c.data_addr(1 if byte else 4))
    return c.cond() | 0x01000090 | (byte << 22) | (rn << 16) | (rd << 12) | rm


def arm_b_bl(c: Case) -> int:
    return c.cond() | 0x0A000000 | (c.rng.getrandbits(1) << 24) | c.rng.getrandbits(24)


def arm_bx_blx_reg(c: Case) -> int:
    link = c.rng.getrandbits(1)
    rm = c.reg()
    c.set(rm, c.jump_target())
    return c.cond() | 0x012FFF10 | (link << 5) | rm


def arm_blx_imm(c: Case) -> int:
    return 0xFA000000 | (c.rng.getrandbits(1) << 24) | c.rng.getrandbits(24)


def arm_pld(c: Case) -> int:
    if c.rng.getrandbits(1):
        return 0xF550F000 | (c.rng.getrandbits(1) << 23) | (c.reg() << 16) | c.rng.getrandbits(12)
    return (0xF750F000 | (c.rng.getrandbits(1) << 23) | (c.reg() << 16) | (c.rng.randrange(32) << 7) |
            (c.rng.randrange(4) << 5) | c.reg())


# ----------------------------------------------------------------------
# Thumb classes
# ----------------------------------------------------------------------

def lo(c: Case, exclude=()) -> int:
    return c.reg(exclude=exclude, hi=7)


def thumb_shift_imm(c: Case) -> int:
    return (c.rng.randrange(3) << 11) | (c.rng.randrange(32) << 6) | (lo(c) << 3) | lo(c)


def thumb_add_sub(c: Case) -> int:
    return 0x1800 | (c.rng.getrandbits(2) << 9) | (c.rng.randrange(8) << 6) | (lo(c) << 3) | lo(c)


def thumb_imm8(c: Case) -> int:
    return 0x2000 | (c.rng.randrange(4) << 11) | (lo(c) << 8) | c.rng.getrandbits(8)


def thumb_alu(c: Case) -> int:
    opcode = c.rng.randrange(16)
    rs = lo(c)
    rd = lo(c)
    if opcode in (2, 3, 4, 7):  # shifts by register
        c.set(rs, (c.rng.getrandbits(24) << 8) | shift_amount_byte(c.rng))
    return 0x4000 | (opcode << 6) | (rs << 3) | rd


def thumb_hi_reg(c: Case) -> int:
    opcode = c.rng.randrange(3)  # ADD, CMP, MOV
    if opcode == 1:
        rd = c.reg()
        rs = c.reg()
    else:
        rd = c.rng.randint(0, 14)
        rs = c.rng.randint(0, 15)
    return 0x4400 | (opcode << 8) | ((rd & 8) << 4) | (rs << 3) | (rd & 7)


def thumb_hi_reg_pc(c: Case) -> int:
    """ADD pc, Rs and MOV pc, Rs: branches that stay in Thumb state."""
    rs = c.reg()
    if c.rng.getrandbits(1):
        c.set(rs, c.rng.randrange(-0x800, 0x800) & M32)
        return 0x4487 | (rs << 3)
    c.set(rs, c.rng.randrange(CODE_LO, CODE_HI))
    return 0x4687 | (rs << 3)


def thumb_bx_blx(c: Case) -> int:
    link = c.rng.getrandbits(1)
    if not link and c.rng.randrange(8) == 0 and c.addr % 4 == 0:
        return 0x4778  # bx pc, from a word-aligned address
    rs = c.reg()
    c.set(rs, c.jump_target())
    return 0x4700 | (link << 7) | (rs << 3)


def thumb_ldr_pc(c: Case) -> int:
    imm = c.rng.getrandbits(8)
    c.literal((c.pc & ~3) + imm * 4)
    return 0x4800 | (lo(c) << 8) | imm


def thumb_two_reg_addr(c: Case, align: int):
    rb = lo(c)
    ro = lo(c, exclude=(rb,))
    target = c.data_addr(align)
    c.set(rb, target - c.get(ro))
    return rb, ro


def thumb_ldst_reg(c: Case) -> int:
    opcode = c.rng.randrange(4)  # STR STRB LDR LDRB
    rb, ro = thumb_two_reg_addr(c, 1 if opcode & 1 else 4)
    return 0x5000 | (opcode << 10) | (ro << 6) | (rb << 3) | lo(c)


def thumb_ldst_sign(c: Case) -> int:
    opcode = c.rng.randrange(4)  # STRH LDSB LDRH LDSH
    rb, ro = thumb_two_reg_addr(c, 1 if opcode == 1 else 2)
    return 0x5200 | (opcode << 10) | (ro << 6) | (rb << 3) | lo(c)


def thumb_ldst_imm(c: Case) -> int:
    byte = c.rng.getrandbits(1)
    load = c.rng.getrandbits(1)
    imm = c.rng.randrange(32)
    rb = lo(c)
    c.set(rb, c.data_addr(1 if byte else 4) - (imm if byte else imm * 4))
    return 0x6000 | (byte << 12) | (load << 11) | (imm << 6) | (rb << 3) | lo(c)


def thumb_ldst_half(c: Case) -> int:
    imm = c.rng.randrange(32)
    rb = lo(c)
    c.set(rb, c.data_addr(2) - imm * 2)
    return 0x8000 | (c.rng.getrandbits(1) << 11) | (imm << 6) | (rb << 3) | lo(c)


def thumb_ldst_sp(c: Case) -> int:
    imm = c.rng.getrandbits(8)
    c.set(13, c.data_addr(4) - imm * 4)
    return 0x9000 | (c.rng.getrandbits(1) << 11) | (lo(c) << 8) | imm


def thumb_add_pc_sp(c: Case) -> int:
    return 0xA000 | (c.rng.getrandbits(1) << 11) | (lo(c) << 8) | c.rng.getrandbits(8)


def thumb_add_sp(c: Case) -> int:
    return 0xB000 | c.rng.getrandbits(8)


def thumb_push_pop(c: Case) -> int:
    """PUSH {rlist[, lr]} and POP {rlist}."""
    pop = c.rng.getrandbits(1)
    rlist = c.rng.getrandbits(8)
    extra = 0 if pop else c.rng.getrandbits(1)
    if not rlist and not extra:
        rlist = 1 << c.rng.randrange(8)
    count = bin(rlist).count("1") + extra
    lowest = c.data_addr(4, 64)
    c.set(13, lowest if pop else lowest + 4 * count)
    return 0xB400 | (pop << 11) | (extra << 8) | rlist


def thumb_pop_pc(c: Case) -> int:
    """POP {rlist, pc}: bit 0 of the loaded word picks the instruction set."""
    rlist = c.rng.getrandbits(8)
    count = bin(rlist).count("1") + 1
    lowest = c.data_addr(4, 64)
    c.set(13, lowest)
    c.poke32(lowest + 4 * (count - 1), c.jump_target())
    return 0xBD00 | rlist


def thumb_ldm_stm(c: Case) -> int:
    rb = lo(c)
    rlist = random_rlist(c, [r for r in range(8) if r != rb])
    c.set(rb, c.data_addr(4, 64))
    return 0xC000 | (c.rng.getrandbits(1) << 11) | (rb << 8) | rlist


def thumb_bcc(c: Case) -> int:
    return 0xD000 | (c.rng.randrange(14) << 8) | c.rng.getrandbits(8)


def thumb_b(c: Case) -> int:
    return 0xE000 | c.rng.getrandbits(11)


def thumb_bl_prefix(c: Case) -> int:
    # QEMU pairs a prefix with whatever follows it unless it is the last
    # halfword of a page, so that is where a lone prefix goes.
    c.addr = (c.addr & ~0x3FF) | 0x3FE
    return 0xF000 | c.rng.getrandbits(11)


def thumb_bl_suffix(c: Case) -> int:
    # LR as a prefix leaves it: even. (With an odd LR QEMU's sum has bit 0
    # clear and it switches to ARM state, which is its own invention.)
    c.set(14, c.rng.randrange(CODE_LO, CODE_HI, 2))
    return 0xF800 | c.rng.getrandbits(11)


def thumb_blx_suffix(c: Case) -> int:
    c.set(14, c.rng.randrange(CODE_LO, CODE_HI, 2))
    return 0xE800 | (c.rng.getrandbits(10) << 1)


# ----------------------------------------------------------------------
# Encodings ARMv5TE leaves undefined
# ----------------------------------------------------------------------

def arm_undefined(c: Case) -> int:
    rng = c.rng
    cond = 0xE0000000
    pick = rng.randrange(10)
    if pick == 0:  # the undefined instruction space: 011x with bit 4 set
        return cond | 0x06000010 | (rng.getrandbits(25) & 0x01FFFFEF)
    if pick == 1:  # LDC/STC and the two-register coprocessor transfers
        return cond | 0x0C000000 | rng.getrandbits(25)
    if pick == 2:  # CDP
        return cond | 0x0E000000 | (rng.getrandbits(24) & 0x00FFFFEF)
    if pick == 3:  # MRC/MCR to a coprocessor that is not there
        return cond | 0x0E000010 | (rng.getrandbits(24) & 0x00FFF0EF) | (rng.randrange(14) << 8)
    if pick == 4:  # the unconditional space, other than BLX, PLD and coprocessors
        while True:
            op = 0xF0000000 | (rng.randrange(5) << 25) | rng.getrandbits(25)
            if (op & 0xFD70F000) != 0xF550F000:
                return op
    if pick == 5:  # multiplies ARMv6 added: UMAAL, MLS
        return cond | 0x00400090 | (rng.getrandbits(22) & 0x003FFF0F)
    if pick == 6:  # the rest of the SWP space: LDREX/STREX and friends
        while True:
            op = cond | 0x01000090 | (rng.getrandbits(24) & 0x00FFFF0F)
            if op & 0x00B00000:
                return op
    if pick == 7:  # the control space, where only BX, BLX, CLZ, BKPT, Q* and SMUL* exist
        while True:
            op = cond | 0x01000000 | (rng.getrandbits(2) << 21) | (rng.choice((1, 2, 3, 4, 6, 7)) << 4)
            op |= rng.getrandbits(20) & 0x000FFF0F
            key = op & 0x0FF000F0
            if key not in (0x01200010, 0x01600010, 0x01200030, 0x01200070):
                return op
    if pick == 8:  # opcodes 8 and 10 with an immediate and no S: MOVW/MOVT later
        return cond | 0x03000000 | (rng.getrandbits(1) << 22) | rng.getrandbits(20)
    # LDRD/STRD with an odd Rd
    return (cond | (rng.getrandbits(4) << 21) | (rng.randrange(15) << 16) | (rng.randrange(1, 16, 2) << 12) |
            (rng.getrandbits(4) << 8) | 0xD0 | (rng.getrandbits(1) << 5) | rng.getrandbits(4))


def thumb_undefined(c: Case) -> int:
    rng = c.rng
    pick = rng.randrange(4)
    if pick == 0:  # condition 14
        return 0xDE00 | rng.getrandbits(8)
    if pick == 1:  # the holes around PUSH, POP, ADD SP and BKPT (not 0xBFxx: see FILL)
        return 0xB000 | (rng.choice((1, 2, 3, 6, 7, 8, 9, 10, 11)) << 8) | rng.getrandbits(8)
    if pick == 2:
        return 0xB600 | rng.getrandbits(9)
    return 0xBA00 | rng.getrandbits(8)


CLASSES = [
    # name, generator, thumb, kind
    ("arm_dp_imm", arm_dp_imm, False, "normal"),
    ("arm_dp_imm_shift", arm_dp_imm_shift, False, "normal"),
    ("arm_dp_reg_shift", arm_dp_reg_shift, False, "normal"),
    ("arm_dp_pc", arm_dp_pc, False, "normal"),
    ("arm_dp_pc_s", arm_dp_pc_s, False, "normal"),
    ("arm_mrs", arm_mrs, False, "normal"),
    ("arm_msr_reg", arm_msr_reg, False, "normal"),
    ("arm_msr_imm", arm_msr_imm, False, "normal"),
    ("arm_mul_mla", arm_mul_mla, False, "normal"),
    ("arm_mull_mlal", arm_mull_mlal, False, "normal"),
    ("arm_dsp_mul", arm_dsp_mul, False, "normal"),
    ("arm_qarith", arm_qarith, False, "normal"),
    ("arm_clz", arm_clz, False, "normal"),
    ("arm_ldr_str_imm", arm_ldr_str_imm, False, "normal"),
    ("arm_ldr_str_reg", arm_ldr_str_reg, False, "normal"),
    ("arm_ldr_pc", arm_ldr_pc, False, "normal"),
    ("arm_ldrh_strh", arm_ldrh_strh, False, "normal"),
    ("arm_ldrsb_ldrsh", arm_ldrsb_ldrsh, False, "normal"),
    ("arm_ldrd_strd", arm_ldrd_strd, False, "normal"),
    ("arm_ldm_stm", arm_ldm_stm, False, "normal"),
    ("arm_ldm_stm_user", arm_ldm_stm_user, False, "normal"),
    ("arm_ldm_pc", arm_ldm_pc, False, "normal"),
    ("arm_ldm_pc_s", arm_ldm_pc_s, False, "normal"),
    ("arm_swp", arm_swp, False, "normal"),
    ("arm_b_bl", arm_b_bl, False, "normal"),
    ("arm_bx_blx_reg", arm_bx_blx_reg, False, "normal"),
    ("arm_blx_imm", arm_blx_imm, False, "normal"),
    ("arm_pld", arm_pld, False, "normal"),
    ("arm_undefined", arm_undefined, False, "undefined"),
    ("thumb_shift_imm", thumb_shift_imm, True, "normal"),
    ("thumb_add_sub", thumb_add_sub, True, "normal"),
    ("thumb_imm8", thumb_imm8, True, "normal"),
    ("thumb_alu", thumb_alu, True, "normal"),
    ("thumb_hi_reg", thumb_hi_reg, True, "normal"),
    ("thumb_hi_reg_pc", thumb_hi_reg_pc, True, "normal"),
    ("thumb_bx_blx", thumb_bx_blx, True, "normal"),
    ("thumb_ldr_pc", thumb_ldr_pc, True, "normal"),
    ("thumb_ldst_reg", thumb_ldst_reg, True, "normal"),
    ("thumb_ldst_sign", thumb_ldst_sign, True, "normal"),
    ("thumb_ldst_imm", thumb_ldst_imm, True, "normal"),
    ("thumb_ldst_half", thumb_ldst_half, True, "normal"),
    ("thumb_ldst_sp", thumb_ldst_sp, True, "normal"),
    ("thumb_add_pc_sp", thumb_add_pc_sp, True, "normal"),
    ("thumb_add_sp", thumb_add_sp, True, "normal"),
    ("thumb_push_pop", thumb_push_pop, True, "normal"),
    ("thumb_pop_pc", thumb_pop_pc, True, "normal"),
    ("thumb_ldm_stm", thumb_ldm_stm, True, "normal"),
    ("thumb_bcc", thumb_bcc, True, "normal"),
    ("thumb_b", thumb_b, True, "normal"),
    ("thumb_bl_prefix", thumb_bl_prefix, True, "normal"),
    ("thumb_bl_suffix", thumb_bl_suffix, True, "normal"),
    ("thumb_blx_suffix", thumb_blx_suffix, True, "normal"),
    ("thumb_undefined", thumb_undefined, True, "undefined"),
]


# ----------------------------------------------------------------------
# The oracle
# ----------------------------------------------------------------------

class Oracle:
    def __init__(self):
        import unicorn
        from unicorn import arm_const as ac
        self.unicorn = unicorn
        self.ac = ac
        self.version = unicorn.__version__
        self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(ac.UC_CPU_ARM_946)
        self.uc.mem_map(MAP_BASE, MAP_SIZE)
        self.transactions: list = []
        self.interrupts: list = []
        self.uc.hook_add(unicorn.UC_HOOK_MEM_READ, self._on_read)
        self.uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, self._on_write)
        self.uc.hook_add(unicorn.UC_HOOK_INTR, self._on_interrupt)
        self.R = [ac.UC_ARM_REG_R0 + i for i in range(13)]
        self.SP, self.LR, self.PC = ac.UC_ARM_REG_SP, ac.UC_ARM_REG_LR, ac.UC_ARM_REG_PC
        self.CPSR, self.SPSR = ac.UC_ARM_REG_CPSR, ac.UC_ARM_REG_SPSR
        # Filler everywhere, then deterministic random bytes in the data window.
        self.uc.mem_write(MAP_BASE, struct.pack("<I", FILL) * (MAP_SIZE // 4))
        self.uc.mem_write(DATA_LO, random.Random(0x4E445339).randbytes(DATA_HI - DATA_LO))
        self.canary()

    def _on_read(self, uc, access, address, size, value, user):
        data = int.from_bytes(uc.mem_read(address, size), "little")
        self.transactions.append((1, size, address, data))

    def _on_write(self, uc, access, address, size, value, user):
        self.transactions.append((2, size, address, value & ((1 << (size * 8)) - 1)))

    def _on_interrupt(self, uc, number, user):
        self.interrupts.append(number)
        uc.emu_stop()

    def put_code(self, addr: int, data: bytes) -> None:
        """Write code and drop any translation of it. This unicorn build
        does not invalidate translated code on a memory write."""
        self.uc.mem_write(addr, data)
        self.uc.ctl_remove_cache(addr & ~1, (addr & ~1) + len(data))

    def restore(self, addr: int, length: int) -> None:
        """Put filler back over [addr, addr + length)."""
        start = addr & ~3
        end = (addr + length + 3) & ~3
        self.put_code(start, struct.pack("<I", FILL) * ((end - start) // 4))

    def canary(self) -> None:
        """Check the two things the layout guards against: that a rewritten
        instruction takes effect, and that one step runs one instruction."""
        uc, ac = self.uc, self.ac
        addr = CODE_HI - 0x100
        for value in (0x11, 0x22):
            # mov r0, #value ; mov r1, #0x77
            self.put_code(addr, struct.pack("<III", 0xE3A00000 | value, 0xE3A01077, FILL))
            uc.reg_write(self.CPSR, MODE_SYS)
            uc.reg_write(ac.UC_ARM_REG_R0, 0)
            uc.reg_write(ac.UC_ARM_REG_R1, 0)
            uc.emu_start(addr, 0xFFFFFFF0, count=1)
            if uc.reg_read(ac.UC_ARM_REG_R0) != value:
                raise RuntimeError("unicorn executed a stale translation; vectors would be wrong")
            if uc.reg_read(ac.UC_ARM_REG_R1) != 0 or uc.reg_read(self.PC) != addr + 4:
                raise RuntimeError("unicorn ran more than one instruction; vectors would be wrong")
        self.restore(addr, 12)

    def program(self, st: State) -> None:
        seq = [(self.CPSR, MODE_SYS)]
        seq += [(self.R[i], st.r[i]) for i in range(13)]
        seq += [(self.SP, st.r[13]), (self.LR, st.r[14])]
        seq += [(self.CPSR, MODE_FIQ)]
        seq += [(self.R[8 + i], st.fiq[i]) for i in range(5)]
        seq += [(self.SP, st.fiq[5]), (self.LR, st.fiq[6]), (self.SPSR, st.spsr[0])]
        for mode, bank, index in ((MODE_SVC, st.svc, 1), (MODE_ABT, st.abt, 2),
                                  (MODE_IRQ, st.irq, 3), (MODE_UND, st.und, 4)):
            seq += [(self.CPSR, mode), (self.SP, bank[0]), (self.LR, bank[1]), (self.SPSR, st.spsr[index])]
        seq.append((self.CPSR, st.cpsr))
        self.uc.reg_write_batch(seq)

    def capture(self) -> State:
        uc = self.uc
        st = State()
        st.cpsr = uc.reg_read(self.CPSR)
        pc = uc.reg_read(self.PC) & ~1
        uc.reg_write(self.CPSR, MODE_SYS)
        values = uc.reg_read_batch(self.R + [self.SP, self.LR])
        st.r = list(values) + [(pc + (4 if st.cpsr & CPSR_T else 8)) & M32]
        uc.reg_write(self.CPSR, MODE_FIQ)
        values = uc.reg_read_batch(self.R[8:13] + [self.SP, self.LR, self.SPSR])
        st.fiq = list(values[:7])
        st.spsr[0] = values[7]
        for mode, name, index in ((MODE_SVC, "svc", 1), (MODE_ABT, "abt", 2),
                                  (MODE_IRQ, "irq", 3), (MODE_UND, "und", 4)):
            uc.reg_write(self.CPSR, mode)
            values = uc.reg_read_batch([self.SP, self.LR, self.SPSR])
            setattr(st, name, list(values[:2]))
            st.spsr[index] = values[2]
        return st

    def run(self, case: Case, opcode: int):
        """Returns ("ok", final state, transactions), ("invalid", ...) or
        raises Discard."""
        uc = self.uc
        UcError = self.unicorn.UcError
        for addr, data in case.mem:
            uc.mem_write(addr, data)
        # Literals first, so the instruction wins where they overlap. The
        # instruction is followed by one that ends the block.
        for addr, data in case.code:
            self.put_code(addr, data)
        if case.thumb:
            code = struct.pack("<HHH", opcode, THUMB_HALT, THUMB_HALT)
        else:
            code = struct.pack("<II", opcode, FILL)
        self.put_code(case.addr, code)
        self.program(case.st)
        self.transactions = []
        self.interrupts = []
        try:
            uc.emu_start(case.addr | (1 if case.thumb else 0), 0xFFFFFFF0, count=1)
        except UcError as error:
            if error.errno == self.unicorn.UC_ERR_INSN_INVALID:
                return "invalid", None, []
            raise Discard(f"unicorn: {error}") from None
        finally:
            self.restore(case.addr, len(code))
            for addr, data in case.code:
                self.restore(addr, len(data))
        if self.interrupts:
            raise Discard(f"exception {self.interrupts[0]}")
        final = self.capture()
        for kind, size, address, data in self.transactions:
            if address & (size - 1):
                raise Discard("misaligned access")
        if not (final.cpsr & CPSR_T) and (final.r[15] & 3):
            raise Discard("ARM state at an address that is not word-aligned")
        return "ok", final, self.transactions


def undefined_entry(case: Case) -> State:
    """The state after the undefined-instruction trap is taken."""
    final = case.st.copy()
    final.und[1] = (case.addr + (2 if case.thumb else 4)) & M32
    final.spsr[4] = case.st.cpsr
    final.cpsr = (case.st.cpsr & ~0x3F) | MODE_UND | CPSR_I
    final.r[15] = VECTOR_BASE + 0x04 + 8
    return final


# ----------------------------------------------------------------------
# Output
# ----------------------------------------------------------------------

def pack_state(st: State, pipe0: int, kind: int) -> bytes:
    words = st.words(pipe0, 0)
    return struct.pack("<ii40I", 168, kind, *words)


def pack_test(case: Case, opcode: int, final: State, transactions) -> bytes:
    body = pack_state(case.st, opcode, 1) + pack_state(final, 0, 2)
    tx = struct.pack("<iii", 12 + 24 * len(transactions), 3, len(transactions))
    for kind, size, address, data in transactions:
        tx += struct.pack("<6I", kind, size, address, data, 0, 0)
    body += tx
    body += struct.pack("<iiII", 16, 4, opcode, case.addr)
    return struct.pack("<i", 4 + len(body)) + body


def generate_class(oracle: Oracle, name: str, gen, thumb: bool, kind: str, count: int, seed: int):
    rng = random.Random(f"{seed}:{name}")
    tests = []
    discards: dict = {}
    attempts = 0
    while len(tests) < count:
        attempts += 1
        if attempts > count * 40 + 1000:
            raise RuntimeError(f"{name}: too many discarded cases: {discards}")
        if attempts % 256 == 0:
            oracle.canary()
        case = Case(rng, thumb)
        opcode = gen(case)
        case.st.r[15] = case.pc
        try:
            status, final, transactions = oracle.run(case, opcode)
        except Discard as reason:
            discards[str(reason)] = discards.get(str(reason), 0) + 1
            continue
        if kind == "undefined":
            if status != "invalid":
                key = "oracle executes it"
                discards[key] = discards.get(key, 0) + 1
                continue
            final, transactions = undefined_entry(case), []
        elif status != "ok":
            key = "oracle rejects it"
            discards[key] = discards.get(key, 0) + 1
            continue
        tests.append(pack_test(case, opcode, final, transactions))
    oracle.canary()
    return struct.pack("<II", MAGIC, len(tests)) + b"".join(tests), discards


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--count", type=int, default=10000, help="tests per class")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--only", action="append", help="generate classes whose name contains this")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        for name, gen, thumb, kind in CLASSES:
            doc = (gen.__doc__ or "").strip().splitlines()[0] if gen.__doc__ else ""
            print(f"{name:20} {'thumb' if thumb else 'arm':5} {kind:9} {doc}")
        return 0

    try:
        oracle = Oracle()
    except ImportError:
        print("unicorn is not installed: pip install unicorn==2.1.4", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    manifest_path = args.out / "MANIFEST.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {"classes": {}}
    manifest.update({
        "generator": "tools/nds/gen_armv5_vectors.py",
        "generator_version": GENERATOR_VERSION,
        "oracle": f"unicorn {oracle.version} (QEMU), UC_CPU_ARM_946",
        "oracle_license": "GPL-2.0 (run as a tool; these files are its output)",
        "format": "SingleStepTests ARM7TDMI .json.bin, magic D33DBAE5: data accesses only",
    })

    started = time.time()
    selected = [entry for entry in CLASSES if not args.only or any(s in entry[0] for s in args.only)]
    if not selected:
        print("no class matches", file=sys.stderr)
        return 1
    for name, gen, thumb, kind in selected:
        t0 = time.time()
        data, discards = generate_class(oracle, name, gen, thumb, kind, args.count, args.seed)
        (args.out / f"{name}.json.bin").write_bytes(data)
        manifest["classes"][name] = {
            "tests": args.count,
            "seed": args.seed,
            "kind": kind,
            "sha256": hashlib.sha256(data).hexdigest(),
            "discarded": discards,
        }
        note = f"  discarded {sum(discards.values())}: {discards}" if discards else ""
        print(f"{name:20} {args.count} tests  {time.time() - t0:5.1f} s{note}", flush=True)

    # The index lists every class that has a file, in the order above.
    names = [name for name, *_ in CLASSES if (args.out / f"{name}.json.bin").exists()]
    (args.out / "index.txt").write_text("".join(f"{name}\n" for name in names))
    manifest["classes"] = {name: manifest["classes"][name] for name in names if name in manifest["classes"]}
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"{len(selected)} classes in {time.time() - started:.0f} s -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
