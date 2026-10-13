#!/usr/bin/env python3
"""Write the N64 recompiler's own test ROM (repository-owned, no Nintendo code).

The hardware test suites test the console; this ROM tests what a recompiler
gets wrong (docs/ADDING-A-CONSOLE.md, "From NES"): delay slots taken,
skipped and faulting, calls and returns, exceptions and ERET, a timer
interrupt landing in compiled code, the FPU, and code in RDRAM that changes
under it - two overlays DMA'd from the cartridge to the same address in turn,
and an instruction patched by a store. Expected values come from the MIPS
architecture and the VR4300 manual, not from this project.

The verdict is in r30 (Dillon Beliveau's n64-tests convention): -1 when
every case passed, otherwise the number of the first case that failed. It
also prints "N64 FIXTURE PASS" or "N64 FIXTURE FAIL <n>" through the
ISViewer port.

The boot code area (0x40-0xFFF) is left empty: the runtime boots at high
level and treats an unknown boot code as CIC 6102.

    python3 tests/fixtures/make_n64_rom.py --output output/fixture.z64
"""
import argparse
import struct
import sys

REG = {f"r{i}": i for i in range(32)}
REG.update({"zero": 0, "at": 1, "v0": 2, "v1": 3, "a0": 4, "a1": 5, "a2": 6, "a3": 7,
            "t0": 8, "t1": 9, "t2": 10, "t3": 11, "t4": 12, "t5": 13, "t6": 14, "t7": 15,
            "s0": 16, "s1": 17, "s2": 18, "s3": 19, "s4": 20, "s5": 21, "s6": 22, "s7": 23,
            "t8": 24, "t9": 25, "k0": 26, "k1": 27, "gp": 28, "sp": 29, "fp": 30, "ra": 31})


class Asm:
    """Just enough of a MIPS III assembler for this ROM."""

    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
        self.fixups = []  # (index, label, kind)

    @property
    def pc(self):
        return self.base + 4 * len(self.words)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def word(self, w):
        self.words.append(w & 0xFFFFFFFF)

    def r(self, rs, rt, rd, sa, fn):
        self.word((REG[rs] << 21) | (REG[rt] << 16) | (REG[rd] << 11) | (sa << 6) | fn)

    def i(self, op, rs, rt, imm):
        self.word((op << 26) | (REG[rs] << 21) | (REG[rt] << 16) | (imm & 0xFFFF))

    # ALU
    def addu(self, d, s, t): self.r(s, t, d, 0, 0x21)
    def add(self, d, s, t): self.r(s, t, d, 0, 0x20)
    def subu(self, d, s, t): self.r(s, t, d, 0, 0x23)
    def and_(self, d, s, t): self.r(s, t, d, 0, 0x24)
    def or_(self, d, s, t): self.r(s, t, d, 0, 0x25)
    def xor(self, d, s, t): self.r(s, t, d, 0, 0x26)
    def slt(self, d, s, t): self.r(s, t, d, 0, 0x2A)
    def sltu(self, d, s, t): self.r(s, t, d, 0, 0x2B)
    def sll(self, d, t, sa): self.r("zero", t, d, sa, 0x00)
    def srl(self, d, t, sa): self.r("zero", t, d, sa, 0x02)
    def sra(self, d, t, sa): self.r("zero", t, d, sa, 0x03)
    def multu(self, s, t): self.r(s, t, "zero", 0, 0x19)
    def div(self, s, t): self.r(s, t, "zero", 0, 0x1A)
    def mflo(self, d): self.r("zero", "zero", d, 0, 0x12)
    def mfhi(self, d): self.r("zero", "zero", d, 0, 0x10)
    def daddu(self, d, s, t): self.r(s, t, d, 0, 0x2D)
    def dsll32(self, d, t, sa): self.r("zero", t, d, sa, 0x3C)
    def addiu(self, t, s, imm): self.i(0x09, s, t, imm)
    def slti(self, t, s, imm): self.i(0x0A, s, t, imm)
    def andi(self, t, s, imm): self.i(0x0C, s, t, imm)
    def ori(self, t, s, imm): self.i(0x0D, s, t, imm)
    def lui(self, t, imm): self.i(0x0F, "zero", t, imm)
    def nop(self): self.word(0)

    def li(self, t, value):
        value &= 0xFFFFFFFF
        self.lui(t, value >> 16)
        self.ori(t, t, value & 0xFFFF)

    def la(self, t, name):
        self.fixups.append((len(self.words), name, "hi"))
        self.lui(t, 0)
        self.fixups.append((len(self.words), name, "lo"))
        self.ori(t, t, 0)

    # memory
    def lw(self, t, off, s): self.i(0x23, s, t, off)
    def lb(self, t, off, s): self.i(0x20, s, t, off)
    def lbu(self, t, off, s): self.i(0x24, s, t, off)
    def lh(self, t, off, s): self.i(0x21, s, t, off)
    def lwl(self, t, off, s): self.i(0x22, s, t, off)
    def lwr(self, t, off, s): self.i(0x26, s, t, off)
    def ld(self, t, off, s): self.i(0x37, s, t, off)
    def sw(self, t, off, s): self.i(0x2B, s, t, off)
    def sh(self, t, off, s): self.i(0x29, s, t, off)
    def sb(self, t, off, s): self.i(0x28, s, t, off)
    def sd(self, t, off, s): self.i(0x3F, s, t, off)

    # control
    def _branch(self, op, rs, rt, name):
        self.fixups.append((len(self.words), name, "b"))
        self.i(op, rs, rt, 0)

    def beq(self, s, t, name): self._branch(0x04, s, t, name)
    def bne(self, s, t, name): self._branch(0x05, s, t, name)
    def beql(self, s, t, name): self._branch(0x14, s, t, name)
    def bnel(self, s, t, name): self._branch(0x15, s, t, name)
    def blez(self, s, name): self._branch(0x06, s, "zero", name)
    def bgtz(self, s, name): self._branch(0x07, s, "zero", name)

    def bltzal(self, s, name):
        self.fixups.append((len(self.words), name, "b"))
        self.word((1 << 26) | (REG[s] << 21) | (0x10 << 16))

    def j(self, name):
        self.fixups.append((len(self.words), name, "j"))
        self.word(0x02 << 26)

    def jal(self, name):
        self.fixups.append((len(self.words), name, "j"))
        self.word(0x03 << 26)

    def jr(self, s): self.r(s, "zero", "zero", 0, 0x08)
    def jalr(self, d, s): self.r(s, "zero", d, 0, 0x09)
    def syscall(self): self.word(0x0000000C)
    def eret(self): self.word(0x42000018)
    def mfc0(self, t, rd): self.word((0x10 << 26) | (0 << 21) | (REG[t] << 16) | (rd << 11))
    def mtc0(self, t, rd): self.word((0x10 << 26) | (4 << 21) | (REG[t] << 16) | (rd << 11))

    # FPU (FR=1)
    def mtc1(self, t, fs): self.word((0x11 << 26) | (4 << 21) | (REG[t] << 16) | (fs << 11))
    def mfc1(self, t, fs): self.word((0x11 << 26) | (0 << 21) | (REG[t] << 16) | (fs << 11))
    def cvt_d_w(self, fd, fs): self.word((0x11 << 26) | (20 << 21) | (fs << 11) | (fd << 6) | 0x21)
    def add_d(self, fd, fs, ft): self.word((0x11 << 26) | (17 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x00)
    def mul_d(self, fd, fs, ft): self.word((0x11 << 26) | (17 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x02)
    def trunc_w_d(self, fd, fs): self.word((0x11 << 26) | (17 << 21) | (fs << 11) | (fd << 6) | 0x0D)
    def c_lt_d(self, fs, ft): self.word((0x11 << 26) | (17 << 21) | (ft << 16) | (fs << 11) | 0x3C)

    def bc1t(self, name):
        self.fixups.append((len(self.words), name, "b"))
        self.word((0x11 << 26) | (8 << 21) | (1 << 16))

    def resolve(self, extern=None):
        labels = dict(extern or {})
        labels.update(self.labels)
        for index, name, kind in self.fixups:
            target = labels[name]
            pc = self.base + 4 * index
            w = self.words[index]
            if kind == "b":
                off = (target - (pc + 4)) >> 2
                assert -32768 <= off < 32768, name
                w |= off & 0xFFFF
            elif kind == "j":
                assert (target & 0xF0000000) == ((pc + 4) & 0xF0000000)
                w |= (target >> 2) & 0x03FFFFFF
            elif kind == "hi":
                w |= (target >> 16) & 0xFFFF
            elif kind == "lo":
                w |= target & 0xFFFF
            self.words[index] = w

    def blob(self):
        return b"".join(struct.pack(">I", w) for w in self.words)


ISV = 0xB3FF0000      # ISViewer base (uncached)
OVERLAY = 0x80200000  # where both overlays run
OVL_A_ROM = 0x00100000 + 0x1000  # cartridge offsets, past the boot megabyte
OVL_B_ROM = OVL_A_ROM + 0x100


def build_main():
    a = Asm(0x80001000)

    def check(reg, value, case):
        """Fail with `case` unless reg == value (uses t9)."""
        a.li("t9", value)
        a.bne(reg, "t9", f"fail_{case}")
        a.nop()

    fails = []

    def case_fail(case):
        fails.append(case)

    a.label("start")
    a.li("sp", 0x80300000)
    a.ori("fp", "zero", 0)  # r30: the verdict
    a.ori("s1", "zero", 0)  # syscalls taken
    a.ori("s2", "zero", 0)  # timer interrupts taken
    a.ori("s3", "zero", 0)  # overflows taken
    a.ori("s4", "zero", 0)  # where the timer handler resumes, if not at EPC

    # Install the exception handler at 0x80000180 (32 words).
    a.la("t0", "handler")
    a.li("t1", 0x80000180)
    a.ori("t2", "zero", 32)
    a.label("copy_handler")
    a.lw("t3", 0, "t0")
    a.sw("t3", 0, "t1")
    a.addiu("t0", "t0", 4)
    a.addiu("t2", "t2", -1)
    a.bne("t2", "zero", "copy_handler")
    a.addiu("t1", "t1", 4)  # delay slot

    # 1: arithmetic
    a.li("t0", 0x12345678)
    a.li("t1", 0x00010001)
    a.addu("t2", "t0", "t1")
    check("t2", 0x12355679, 1)
    a.multu("t0", "t1")
    a.mflo("t3")
    check("t3", (0x12345678 * 0x00010001) & 0xFFFFFFFF, 1)
    a.li("t4", 100)
    a.ori("t5", "zero", 7)
    a.div("t4", "t5")
    a.mflo("t6")
    a.mfhi("t7")
    check("t6", 14, 1)
    check("t7", 2, 1)

    # 2: delay slots: a taken branch runs its slot, a likely branch not
    # taken skips it.
    a.ori("t0", "zero", 0)
    a.beq("zero", "zero", "b2_taken")
    a.ori("t0", "zero", 5)  # slot: runs
    a.ori("t0", "zero", 9)  # skipped
    a.label("b2_taken")
    check("t0", 5, 2)
    a.ori("t1", "zero", 0)
    a.bnel("zero", "zero", "fail_2")
    a.ori("t1", "zero", 7)  # nullified
    check("t1", 0, 2)
    a.ori("t2", "zero", 1)
    a.bltzal("zero", "fail_2")  # not taken, but links
    a.nop()
    a.label("after_bltzal")

    # 3: a loop and a call
    a.ori("t0", "zero", 0)
    a.ori("t1", "zero", 100)
    a.label("sum_loop")
    a.addu("t0", "t0", "t1")
    a.addiu("t1", "t1", -1)
    a.bne("t1", "zero", "sum_loop")
    a.nop()
    check("t0", 5050, 3)
    a.ori("a0", "zero", 21)
    a.jal("double")
    a.nop()
    check("v0", 42, 3)
    a.la("t0", "double")
    a.ori("a0", "zero", 50)
    a.jalr("ra", "t0")
    a.nop()
    check("v0", 100, 3)

    # 4: loads and stores
    a.li("t0", 0x80280000)
    a.li("t1", 0x8899AABB)
    a.sw("t1", 0, "t0")
    a.lb("t2", 0, "t0")
    check("t2", 0xFFFFFF88, 4)
    a.lbu("t2", 1, "t0")
    check("t2", 0x99, 4)
    a.lh("t2", 2, "t0")
    check("t2", 0xFFFFAABB, 4)
    a.li("t3", 0x11223344)
    a.sw("t3", 4, "t0")
    a.ori("t4", "zero", 0)
    a.lwl("t4", 2, "t0")  # AABB....
    a.lwr("t4", 5, "t0")  # ....1122
    check("t4", 0xAABB1122, 4)
    a.sd("t1", 8, "t0")
    a.ld("t5", 8, "t0")
    a.dsll32("t6", "t1", 0)
    a.daddu("t6", "t6", "zero")
    check("t5", 0x8899AABB, 4)  # sd of a sign-extended register: the upper word is 0xFFFFFFFF
    a.lw("t7", 8, "t0")
    check("t7", 0xFFFFFFFF, 4)

    # 5: SYSCALL goes to the handler and comes back after it
    a.syscall()
    check("s1", 1, 5)

    # 6: overflow: the handler counts it and the destination is untouched
    a.li("t0", 0x7FFFFFFF)
    a.ori("t1", "zero", 1)
    a.ori("t2", "zero", 77)
    a.add("t2", "t0", "t1")
    check("s3", 1, 6)
    check("t2", 77, 6)

    # 7: the FPU
    a.li("t0", 0x34000000)  # CU0, CU1, FR
    a.mtc0("t0", 12)
    a.ori("t1", "zero", 3)
    a.mtc1("t1", 1)
    a.cvt_d_w(2, 1)          # f2 = 3.0
    a.add_d(4, 2, 2)         # f4 = 6.0
    a.mul_d(6, 4, 2)         # f6 = 18.0
    a.trunc_w_d(8, 6)
    a.mfc1("t2", 8)
    check("t2", 18, 7)
    a.c_lt_d(2, 4)           # 3 < 6
    a.bc1t("fpu_ok")
    a.nop()
    a.j("fail_7")
    a.nop()
    a.label("fpu_ok")

    # 8: overlays: DMA overlay A to OVERLAY and call it, then overlay B to
    # the same address and call it again. Native code compiled from A must
    # not run for B.
    for case, rom_off, expect in ((8, OVL_A_ROM, 0x1111), (9, OVL_B_ROM, 0x2222)):
        a.li("t0", 0xA4600000)            # PI
        a.li("t1", OVERLAY & 0x1FFFFFFF)
        a.sw("t1", 0, "t0")               # PI_DRAM_ADDR
        a.li("t1", 0x10000000 + rom_off)
        a.sw("t1", 4, "t0")               # PI_CART_ADDR
        a.ori("t1", "zero", 0x7F)
        a.sw("t1", 12, "t0")              # PI_WR_LEN: 128 bytes
        a.label(f"pi_wait_{case}")
        a.lw("t2", 16, "t0")              # PI_STATUS
        a.andi("t2", "t2", 1)
        a.bne("t2", "zero", f"pi_wait_{case}")
        a.nop()
        a.ori("t2", "zero", 2)
        a.sw("t2", 16, "t0")              # clear the PI interrupt
        a.li("t3", OVERLAY)
        a.jalr("ra", "t3")
        a.nop()
        check("v0", expect, case)

    # 10: a store patches an instruction of code that already ran (the
    # overlay's delay slot)
    a.li("t0", OVERLAY)
    a.li("t1", 0x24023333)               # addiu v0, zero, 0x3333
    a.sw("t1", 4, "t0")
    a.jalr("ra", "t0")
    a.nop()
    check("v0", 0x3333, 10)

    # 11: a timer interrupt lands while compiled code spins
    a.mfc0("t0", 9)                     # Count
    a.addiu("t0", "t0", 2000)
    a.mtc0("t0", 11)                    # Compare
    a.li("t1", 0x34008001)              # CU0|CU1|FR, IM7, IE
    a.mtc0("t1", 12)
    a.li("t2", 1000000)
    a.label("spin")
    a.bne("s2", "zero", "spun")
    a.addiu("t2", "t2", -1)
    a.bne("t2", "zero", "spin")
    a.nop()
    a.j("fail_11")
    a.nop()
    a.label("spun")
    a.li("t1", 0x34000000)
    a.mtc0("t1", 12)

    # 12: ERET to code nothing else reaches (the rest of the program): it
    # is compiled only if a profile records where ERET went.
    a.la("t0", "after_eret")
    a.mtc0("t0", 14)                    # EPC
    a.li("t1", 0x34000002)              # EXL
    a.mtc0("t1", 12)
    a.eret()
    a.j("fail_12")
    a.nop()
    a.label("after_eret")
    a.mfc0("t1", 12)
    a.andi("t1", "t1", 2)
    check("t1", 0, 12)                  # ERET cleared EXL

    # 13: the idle loop a game's lowest thread sits in (a branch to itself)
    # until the timer interrupt, whose handler resumes after the loop (s4).
    # The run loop and generated code take its turns at once; the interrupt
    # must still land on the same turn. An odd distance, so the loop's two
    # cycles step over Compare.
    a.la("s4", "idle_done")
    a.mfc0("t0", 9)                     # Count
    a.addiu("t0", "t0", 1501)
    a.mtc0("t0", 11)                    # Compare
    a.li("t1", 0x34008001)              # CU0|CU1|FR, IM7, IE
    a.mtc0("t1", 12)
    a.label("idle")
    a.beq("zero", "zero", "idle")
    a.nop()
    a.label("idle_done")
    a.li("t1", 0x34000000)
    a.mtc0("t1", 12)
    check("s2", 2, 13)                  # the second timer interrupt
    check("s4", 0, 13)                  # taken through the handler's resume

    # 14: a jump into a branch the recompiler leaves to the interpreter
    # (jump14, below, into never14). Nothing here runs it; it only has to
    # compile, and the call has to come back.
    a.jal("jump14")
    a.nop()
    a.beq("zero", "zero", "past14")
    a.nop()
    a.label("never14")
    a.bne("t0", "t1", "past14")
    a.syscall()                         # a delay slot that cannot be compiled
    a.label("past14")
    check("v0", 0x1414, 14)

    # Done: print and report.
    a.la("a0", "msg_pass")
    a.ori("a1", "zero", 17)
    a.jal("isv_print")
    a.nop()
    a.addiu("fp", "zero", -1)
    a.label("done")
    a.j("done")
    a.nop()

    for case in range(1, 15):
        a.label(f"fail_{case}")
        a.ori("fp", "zero", case)
        a.la("a0", "msg_fail")
        a.ori("a1", "zero", 17)
        a.jal("isv_print")
        a.nop()
        a.j("done_fail")
        a.nop()
    a.label("done_fail")
    a.j("done_fail")
    a.nop()

    # double(a0) -> v0
    a.label("double")
    a.jr("ra")
    a.addu("v0", "a0", "a0")  # in the delay slot

    # jump14() -> v0 = 0x1414: a function of its own whose untaken path
    # jumps into the middle of the main one, at never14.
    a.label("jump14")
    a.beq("zero", "zero", "jump14_out")
    a.ori("v0", "zero", 0x1414)         # in the delay slot
    a.j("never14")
    a.nop()
    a.label("jump14_out")
    a.jr("ra")
    a.nop()

    # isv_print(a0 = string, a1 = length): copy it to the ISViewer buffer
    # word by word, then write the length.
    a.label("isv_print")
    a.li("t0", ISV + 0x20)
    a.addiu("t1", "a1", 3)
    a.srl("t1", "t1", 2)
    a.label("isv_loop")
    a.lw("t2", 0, "a0")
    a.sw("t2", 0, "t0")
    a.addiu("a0", "a0", 4)
    a.addiu("t1", "t1", -1)
    a.bne("t1", "zero", "isv_loop")
    a.addiu("t0", "t0", 4)
    a.li("t0", ISV + 0x14)
    a.sw("a1", 0, "t0")
    a.jr("ra")
    a.nop()

    # The exception handler (copied to 0x80000180): SYSCALL and overflow
    # resume after the instruction; the timer interrupt is acknowledged by
    # writing Compare. Anything else fails with case 99.
    a.label("handler")
    h = Asm(0x80000180)
    h.mfc0("k0", 13)
    h.andi("k1", "k0", 0x7C)
    h.ori("at", "zero", 8 << 2)
    h.beq("k1", "at", "h_skip_sys")
    h.nop()
    h.ori("at", "zero", 12 << 2)
    h.beq("k1", "at", "h_skip_ov")
    h.nop()
    h.bne("k1", "zero", "h_unknown")
    h.nop()
    h.mfc0("k1", 11)
    h.mtc0("k1", 11)  # acknowledge the timer
    h.addiu("s2", "s2", 1)
    h.beq("s4", "zero", "h_eret")  # s4: where to resume instead (case 13)
    h.nop()
    h.mtc0("s4", 14)
    h.ori("s4", "zero", 0)
    h.label("h_eret")
    h.eret()
    h.label("h_skip_sys")
    h.addiu("s1", "s1", 1)
    h.j("h_resume")
    h.nop()
    h.label("h_skip_ov")
    h.addiu("s3", "s3", 1)
    h.label("h_resume")
    h.mfc0("k1", 14)
    h.addiu("k1", "k1", 4)
    h.mtc0("k1", 14)
    h.eret()
    h.label("h_unknown")
    h.ori("fp", "zero", 99)
    h.label("h_hang")
    h.j("h_hang")
    h.nop()
    h.resolve()
    assert len(h.words) <= 32
    for w in h.words + [0] * (32 - len(h.words)):
        a.word(w)

    a.label("msg_pass")
    for chunk in struct.iter_unpack(">I", b"N64 FIXTURE PASS\n\0\0\0"):
        a.word(chunk[0])
    a.label("msg_fail")
    for chunk in struct.iter_unpack(">I", b"N64 FIXTURE FAIL\n\0\0\0"):
        a.word(chunk[0])
    a.resolve()
    return a


def build_overlay(value):
    o = Asm(OVERLAY)
    o.jr("ra")
    o.ori("v0", "zero", value)
    o.resolve()
    return o.blob()


def build_rom():
    main = build_main()
    rom = bytearray(0x101000 + 0x1000)
    header = struct.pack(">IIII", 0x80371240, 0x0000000F, 0x80001000, 0x0000144C)
    rom[0:16] = header
    title = b"N64 RECOMP FIXTURE  "
    rom[0x20:0x20 + len(title)] = title
    rom[0x3B:0x3F] = b"NRCE"
    code = main.blob()
    assert len(code) < 0x100000
    rom[0x1000:0x1000 + len(code)] = code
    a = build_overlay(0x1111)
    b = build_overlay(0x2222)
    rom[OVL_A_ROM:OVL_A_ROM + len(a)] = a
    rom[OVL_B_ROM:OVL_B_ROM + len(b)] = b
    return bytes(rom)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    data = build_rom()
    with open(args.output, "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
