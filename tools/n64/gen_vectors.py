#!/usr/bin/env python3
"""Generate the VR4300 per-instruction vectors (tests/n64/vectors/vr4300.vectors).

There is no SingleStepTests set for the VR4300, so the vectors are made: each
case loads a register and memory state, runs one instruction (two for a branch
and its delay slot) on an oracle, and records the state after it and the bus
transactions in order. The oracle is Unicorn's MIPS64 R4000 model (QEMU's
MIPS III core, GPL-2.0), run as a tool and never linked: `pip install unicorn`.
Nothing this project ships depends on it; the vector file is checked in and
tests/n64/n64_vectors.c replays it against the interpreter.

The R4000 model is a second implementation of the same architecture, not
hardware. Cases where the architecture leaves the result undefined and the
VR4300 is known to differ (32-bit operations on values that are not
sign-extended, division by zero, denormal or NaN floating-point operands and
results, the unimplemented-operation exception) are not generated here; they
are pinned by tests/n64/n64_cpu_test.c from ares's hardware-tested behaviour
and by the hardware test ROMs (docs/N64-PLAN.md, "The vector pipeline").

    python3 tools/n64/gen_vectors.py            # rewrite the vector file
    python3 tools/n64/gen_vectors.py --check    # fail if it would change
"""
import argparse
import os
import random
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_MIPS, UC_MODE_MIPS64, UC_MODE_BIG_ENDIAN, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_CODE
from unicorn import mips_const as M

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tests", "n64", "vectors", "vr4300.vectors")

CODE_PHYS = 0x10000
DATA_PHYS = 0x20000
CODE = 0xFFFFFFFF80000000 | CODE_PHYS
DATA = 0xFFFFFFFF80000000 | DATA_PHYS
MASK64 = (1 << 64) - 1

GPR = [getattr(M, "UC_MIPS_REG_%d" % i) for i in range(32)]
FPR = [getattr(M, "UC_MIPS_REG_F%d" % i) for i in range(32)]

# QEMU's exception numbers -> MIPS Cause codes.
QEMU_EXC = {12: 4, 13: 5, 17: 8, 18: 9, 19: 11, 20: 10, 21: 12, 22: 13, 23: 15}


def sx32(v):
    v &= 0xFFFFFFFF
    return (v | ~0xFFFFFFFF) & MASK64 if v & 0x80000000 else v


def r_type(rs, rt, rd, sa, fn):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def cop1(fmt, ft, fs, fd, fn):
    return (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn


def f32bits(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def f64bits(x):
    return struct.unpack(">Q", struct.pack(">d", x))[0]


class Oracle:
    def _fresh(self):
        # A new machine per case: Unicorn keeps translated code and
        # exception state between runs, and one case must not see another.
        self.mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_BIG_ENDIAN, M.UC_CPU_MIPS64_R4000)
        self.mu.mem_map(0, 0x100000)
        self.mu.hook_add(UC_HOOK_INTR, self._intr)
        self.mu.hook_add(UC_HOOK_MEM_READ, self._read)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self._write)
        self.mu.hook_add(UC_HOOK_CODE, self._code)

    def _intr(self, uc, intno, _):
        self.exc = QEMU_EXC.get(intno, 100 + intno)
        uc.emu_stop()

    def _code(self, uc, addr, size, _):
        self.bus.append(("F", addr & 0x1FFFFFFF, 4, int.from_bytes(bytes(uc.mem_read(addr & 0x1FFFFFFF, 4)), "big")))

    def _read(self, uc, access, addr, size, value, _):
        pa = addr & 0x1FFFFFFF
        self.bus.append(("R", pa, size, int.from_bytes(bytes(uc.mem_read(pa, size)), "big")))

    def _write(self, uc, access, addr, size, value, _):
        self.bus.append(("W", addr & 0x1FFFFFFF, size, value & ((1 << (8 * size)) - 1)))

    def run(self, case):
        self._fresh()
        mu = self.mu
        mu.mem_write(CODE_PHYS, b"".join(struct.pack(">I", w) for w in case["insns"]) + b"\0" * 16)
        for addr, data in case.get("mem", []):
            mu.mem_write(addr, data)
        regs = case["regs"]
        for i in range(32):
            mu.reg_write(GPR[i], regs.get(i, 0) if i else 0)
        mu.reg_write(M.UC_MIPS_REG_HI, case.get("hi", 0))
        mu.reg_write(M.UC_MIPS_REG_LO, case.get("lo", 0))
        status = 0x20400004 | (0x04000000 if case.get("fr", 1) else 0)
        mu.reg_write(M.UC_MIPS_REG_CP0_STATUS, status)
        for i in range(32):
            mu.reg_write(FPR[i], case.get("fpr", {}).get(i, 0))
        mu.reg_write(M.UC_MIPS_REG_FCSR, case.get("fcr31", 0))
        self.exc = None
        self.bus = []
        count = case.get("count", 1)
        try:
            mu.emu_start(CODE, CODE + 0x1000, count=count)
        except UcError as e:
            if self.exc is None:
                raise RuntimeError("%s: %s" % (case["name"], e))
        out = {"regs": {}, "fpr": {}}
        for i in range(1, 32):
            out["regs"][i] = mu.reg_read(GPR[i]) & MASK64
        out["hi"] = mu.reg_read(M.UC_MIPS_REG_HI) & MASK64
        out["lo"] = mu.reg_read(M.UC_MIPS_REG_LO) & MASK64
        for i in range(32):
            out["fpr"][i] = mu.reg_read(FPR[i]) & MASK64
        out["fcr31"] = mu.reg_read(M.UC_MIPS_REG_FCSR) & 0xFFFFFFFF
        out["pc"] = mu.reg_read(M.UC_MIPS_REG_PC) & MASK64
        out["exc"] = self.exc
        out["bus"] = list(self.bus)
        out["mem"] = bytes(mu.mem_read(DATA_PHYS, 0x100))
        return out


# Values that find the edges, sign-extended for 32-bit operations.
EDGE32 = [0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x00008000, 0x0000FFFF, 0x12345678, 0x89ABCDEF]
EDGE64 = [0, 1, MASK64, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x00000000FFFFFFFF, 0xFFFFFFFF00000000,
          0x0123456789ABCDEF, 0xFEDCBA9876543210, 0x0000000080000000]


def val32(rng):
    if rng.random() < 0.35:
        return sx32(rng.choice(EDGE32))
    return sx32(rng.getrandbits(32))


def val64(rng):
    if rng.random() < 0.35:
        return rng.choice(EDGE64)
    return rng.getrandbits(64)


def regs3(rng):
    rs, rt, rd = rng.sample(range(1, 32), 3)
    return rs, rt, rd


def int_cases(rng):
    cases = []
    r3_32 = {"add": 0x20, "addu": 0x21, "sub": 0x22, "subu": 0x23}
    r3_64 = {"and": 0x24, "or": 0x25, "xor": 0x26, "nor": 0x27, "slt": 0x2A, "sltu": 0x2B,
             "dadd": 0x2C, "daddu": 0x2D, "dsub": 0x2E, "dsubu": 0x2F}
    for name, fn in list(r3_32.items()) + list(r3_64.items()):
        wide = name in r3_64
        for n in range(48):
            rs, rt, rd = regs3(rng)
            if n == 47:
                rd = 0  # writes to r0 are dropped
            a = val64(rng) if wide else val32(rng)
            b = val64(rng) if wide else val32(rng)
            cases.append({"name": name, "insns": [r_type(rs, rt, rd, 0, fn)], "regs": {rs: a, rt: b, rd: val64(rng)}})
    shifts = {"sll": (0x00, False), "srl": (0x02, False), "sra": (0x03, False),
              "dsll": (0x38, True), "dsrl": (0x3A, True), "dsra": (0x3B, True),
              "dsll32": (0x3C, True), "dsrl32": (0x3E, True), "dsra32": (0x3F, True)}
    for name, (fn, wide) in shifts.items():
        for sa in range(32):
            rs, rt, rd = regs3(rng)
            v = val64(rng) if wide else val32(rng)
            cases.append({"name": name, "insns": [r_type(0, rt, rd, sa, fn)], "regs": {rt: v}})
    vshifts = {"sllv": (0x04, False), "srlv": (0x06, False), "srav": (0x07, False),
               "dsllv": (0x14, True), "dsrlv": (0x16, True), "dsrav": (0x17, True)}
    for name, (fn, wide) in vshifts.items():
        for n in range(40):
            rs, rt, rd = regs3(rng)
            v = val64(rng) if wide else val32(rng)
            cases.append({"name": name, "insns": [r_type(rs, rt, rd, 0, fn)], "regs": {rt: v, rs: val64(rng)}})
    for name, fn, wide in [("mult", 0x18, False), ("multu", 0x19, False), ("div", 0x1A, False),
                           ("divu", 0x1B, False), ("dmult", 0x1C, True), ("dmultu", 0x1D, True),
                           ("ddiv", 0x1E, True), ("ddivu", 0x1F, True)]:
        for n in range(48):
            rs, rt, rd = regs3(rng)
            a = val64(rng) if wide else val32(rng)
            b = val64(rng) if wide else val32(rng)
            if fn in (0x1A, 0x1B, 0x1E, 0x1F):
                if (b & (MASK64 if wide else 0xFFFFFFFF)) == 0:
                    b = 3  # division by zero is undefined; see the module comment
                if name == "div" and (a & 0xFFFFFFFF) == 0x80000000 and (b & 0xFFFFFFFF) == 0xFFFFFFFF:
                    b = 5  # undefined in the architecture
                if name == "ddiv" and a == 0x8000000000000000 and b == MASK64:
                    b = 7
            cases.append({"name": name, "insns": [r_type(rs, rt, 0, 0, fn)], "regs": {rs: a, rt: b},
                          "hi": val64(rng), "lo": val64(rng)})
    for name, fn in [("mfhi", 0x10), ("mflo", 0x12)]:
        for n in range(8):
            rd = rng.randrange(1, 32)
            cases.append({"name": name, "insns": [r_type(0, 0, rd, 0, fn)], "regs": {}, "hi": val64(rng), "lo": val64(rng)})
    for name, fn in [("mthi", 0x11), ("mtlo", 0x13)]:
        for n in range(8):
            rs = rng.randrange(1, 32)
            cases.append({"name": name, "insns": [r_type(rs, 0, 0, 0, fn)], "regs": {rs: val64(rng)}})
    imm = {"addi": 0x08, "addiu": 0x09, "slti": 0x0A, "sltiu": 0x0B, "andi": 0x0C, "ori": 0x0D,
           "xori": 0x0E, "lui": 0x0F, "daddi": 0x18, "daddiu": 0x19}
    for name, op in imm.items():
        wide = name in ("daddi", "daddiu", "andi", "ori", "xori", "slti", "sltiu")
        for n in range(40):
            rs, rt, _ = regs3(rng)
            k = rng.choice([0, 1, 0x7FFF, 0x8000, 0xFFFF, rng.getrandbits(16)])
            v = val64(rng) if wide else val32(rng)
            cases.append({"name": name, "insns": [i_type(op, rs, rt, k)], "regs": {rs: v}})
    return cases


def mem_cases(rng):
    cases = []
    loads = {"lb": (0x20, 1), "lh": (0x21, 2), "lw": (0x23, 4), "lbu": (0x24, 1), "lhu": (0x25, 2),
             "lwu": (0x27, 4), "ld": (0x37, 8), "lwl": (0x22, 0), "lwr": (0x26, 0), "ldl": (0x1A, 0), "ldr": (0x1B, 0)}
    stores = {"sb": (0x28, 1), "sh": (0x29, 2), "sw": (0x2B, 4), "sd": (0x3F, 8),
              "swl": (0x2A, 0), "swr": (0x2E, 0), "sdl": (0x2C, 0), "sdr": (0x2D, 0)}
    for name, (op, size) in list(loads.items()) + list(stores.items()):
        for n in range(48):
            base, rt, _ = regs3(rng)
            align = size if size else 1
            off = rng.randrange(0, 0x80 // align) * align
            imm = rng.choice([0, 4, 8, -8, 0x10, -0x10]) if size else rng.randrange(-8, 9)
            addr = DATA + 0x40 + off
            mem = bytes(rng.getrandbits(8) for _ in range(0x100))
            case = {"name": name, "insns": [i_type(op, base, rt, imm)],
                    "regs": {base: (addr - imm) & MASK64, rt: val64(rng)}, "mem": [(DATA_PHYS, mem)],
                    "bus": size != 0}
            if n == 47 and size > 1:
                case["regs"][base] = (addr + 1 - imm) & MASK64  # misaligned: address error
            cases.append(case)
    return cases


def branch_cases(rng):
    cases = []
    marker = i_type(0x09, 0, 1, 0x55)  # addiu r1, r0, 0x55 in the delay slot
    defs = [("beq", 0x04, None), ("bne", 0x05, None), ("blez", 0x06, None), ("bgtz", 0x07, None),
            ("beql", 0x14, None), ("bnel", 0x15, None), ("blezl", 0x16, None), ("bgtzl", 0x17, None),
            ("bltz", 0x01, 0x00), ("bgez", 0x01, 0x01), ("bltzl", 0x01, 0x02), ("bgezl", 0x01, 0x03),
            ("bltzal", 0x01, 0x10), ("bgezal", 0x01, 0x11), ("bltzall", 0x01, 0x12), ("bgezall", 0x01, 0x13)]
    for name, op, rt_field in defs:
        for n in range(16):
            rs, rt, _ = regs3(rng)
            rs = max(rs, 2)
            rt = max(rt, 2)
            if rs == rt:
                rt = rs + 1 if rs < 31 else 2
            a = rng.choice([0, 1, MASK64, 0x8000000000000000, rng.getrandbits(64)])
            b = a if rng.random() < 0.5 else rng.choice([0, 1, MASK64, rng.getrandbits(64)])
            disp = rng.choice([2, 3, 4])
            if name in ("blez", "bgtz", "blezl", "bgtzl"):
                w = i_type(op, rs, 0, disp)  # rt must be zero: the R4000 model reserves the rest
            elif rt_field is None:
                w = i_type(op, rs, rt, disp)
            else:
                w = i_type(op, rs, rt_field, disp)
            cases.append({"name": name, "insns": [w, marker, 0, 0, 0, 0], "regs": {rs: a, rt: b}, "count": 2})
    for name, op in [("j", 0x02), ("jal", 0x03)]:
        for n in range(4):
            target = (CODE + 4 * rng.randrange(2, 6)) & 0x0FFFFFFF
            cases.append({"name": name, "insns": [(op << 26) | (target >> 2), marker, 0, 0, 0, 0],
                          "regs": {}, "count": 2})
    for name, fn in [("jr", 0x08), ("jalr", 0x09)]:
        for n in range(6):
            rs, rt, rd = regs3(rng)
            rs = max(rs, 2)
            rd = max(rd, 2)
            if rd == rs:
                rd = rs + 1 if rs < 31 else 2
            target = CODE + 4 * rng.randrange(2, 6)
            cases.append({"name": name, "insns": [r_type(rs, 0, rd if fn == 9 else 0, 0, fn), marker, 0, 0, 0, 0],
                          "regs": {rs: target}, "count": 2})
    return cases


def trap_cases(rng):
    cases = []
    for name, fn in [("tge", 0x30), ("tgeu", 0x31), ("tlt", 0x32), ("tltu", 0x33), ("teq", 0x34), ("tne", 0x36)]:
        for n in range(12):
            rs, rt, _ = regs3(rng)
            a = val64(rng)
            b = a if rng.random() < 0.4 else val64(rng)
            cases.append({"name": name, "insns": [r_type(rs, rt, 0, 0, fn)], "regs": {rs: a, rt: b}})
    for name, rtf in [("tgei", 0x08), ("tgeiu", 0x09), ("tlti", 0x0A), ("tltiu", 0x0B), ("teqi", 0x0C), ("tnei", 0x0E)]:
        for n in range(12):
            rs = rng.randrange(1, 32)
            k = rng.choice([0, 1, 0xFFFF, 0x8000, rng.getrandbits(16)])
            a = sx32(k | (0xFFFF0000 if k & 0x8000 else 0)) if rng.random() < 0.3 else val64(rng)
            cases.append({"name": name, "insns": [i_type(0x01, rs, rtf, k)], "regs": {rs: a}})
    cases.append({"name": "syscall", "insns": [0x0000000C], "regs": {}})
    cases.append({"name": "break", "insns": [0x0000000D], "regs": {}})
    return cases


# Floating point: normal operands only, results that stay normal (or zero,
# infinity) - see the module comment.
def fnormal32(rng):
    e = rng.randrange(-30, 31)
    return rng.choice([-1, 1]) * rng.uniform(1.0, 2.0) * (2.0 ** e)


def fp_cases(rng):
    cases = []
    fmts = [("s", 16), ("d", 17)]
    binops = [("add", 0x00), ("sub", 0x01), ("mul", 0x02), ("div", 0x03)]
    for fname, fmt in fmts:
        for oname, fn in binops:
            for n in range(40):
                a, b = fnormal32(rng), fnormal32(rng)
                if n < 4:
                    b = a if oname == "sub" else -a
                if n == 4 and oname == "div":
                    b = 0.0
                if n == 5:
                    a = 3.0e38 if fname == "s" else 1.0e308
                    b = a
                bits = f32bits if fname == "s" else f64bits
                rm = rng.randrange(4) if n % 3 == 0 else 0
                fs, ft, fd = rng.sample(range(0, 32), 3)
                cases.append({"name": "%s.%s" % (oname, fname), "insns": [cop1(fmt, ft, fs, fd, fn)],
                              "regs": {}, "fpr": {fs: bits(a), ft: bits(b)}, "fcr31": rm,
                              "fp_result": (fd, fname)})
        for oname, fn in [("sqrt", 0x04), ("abs", 0x05), ("mov", 0x06), ("neg", 0x07)]:
            for n in range(24):
                a = abs(fnormal32(rng)) if oname == "sqrt" and n % 6 else fnormal32(rng)
                bits = f32bits if fname == "s" else f64bits
                fs, fd = rng.sample(range(0, 32), 2)
                cases.append({"name": "%s.%s" % (oname, fname), "insns": [cop1(fmt, 0, fs, fd, fn)],
                              "regs": {}, "fpr": {fs: bits(a)}, "fcr31": rng.randrange(4) if n % 4 == 0 else 0,
                              "fp_result": (fd, fname)})
        # The R4000 model reserves the 64-bit integer formats (.L); those
        # conversions are covered by tests/n64/n64_cpu_test.c instead.
        conv = [("round.w", 0x0C), ("trunc.w", 0x0D), ("ceil.w", 0x0E), ("floor.w", 0x0F), ("cvt.w", 0x24)]
        conv += [("cvt.d", 0x21)] if fname == "s" else [("cvt.s", 0x20)]
        for oname, fn in conv:
            for n in range(24):
                a = rng.choice([0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.0]) if n < 7 else fnormal32(rng) * 1000
                if oname == "cvt.s":
                    a = fnormal32(rng)
                bits = f32bits if fname == "s" else f64bits
                fs, fd = rng.sample(range(0, 32), 2)
                cases.append({"name": "%s.%s" % (oname, fname), "insns": [cop1(fmt, 0, fs, fd, fn)],
                              "regs": {}, "fpr": {fs: bits(a)}, "fcr31": rng.randrange(4), "fp_result": (fd, None)})
        for cond in range(16):
            for n in range(6):
                a = fnormal32(rng)
                b = a if n % 3 == 0 else (fnormal32(rng) if n % 3 == 1 else -a)
                bits = f32bits if fname == "s" else f64bits
                fs, ft = rng.sample(range(0, 32), 2)
                cases.append({"name": "c.%d.%s" % (cond, fname), "insns": [cop1(fmt, ft, fs, 0, 0x30 | cond)],
                              "regs": {}, "fpr": {fs: bits(a), ft: bits(b)}, "fcr31": rng.choice([0, 0x00800000])})
    for fname, fmt in [("w", 20)]:
        for oname, fn in [("cvt.s", 0x20), ("cvt.d", 0x21)]:
            for n in range(24):
                v = sx32(rng.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0x01000001, rng.getrandbits(32)]))
                v &= 0xFFFFFFFF
                fs, fd = rng.sample(range(0, 32), 2)
                cases.append({"name": "%s.%s" % (oname, fname), "insns": [cop1(fmt, 0, fs, fd, fn)],
                              "regs": {}, "fpr": {fs: v}, "fcr31": rng.randrange(4), "fp_result": (fd, None)})
    # Moves. Only FR=1: with FR clear the R4000 model keeps 32 separate
    # 32-bit halves, the VR4300 one register file viewed two ways, so the
    # states are not comparable (tests/n64/n64_cpu_test.c covers FR=0).
    for fr in (1,):
        for name, rsf in [("mfc1", 0), ("dmfc1", 1), ("mtc1", 4), ("dmtc1", 5)]:
            for n in range(12):
                rt = rng.randrange(1, 32)
                fs = rng.randrange(0, 32)
                if fr == 0 and name in ("dmfc1", "dmtc1"):
                    fs &= ~1
                cases.append({"name": "%s.fr%d" % (name, fr), "insns": [(0x11 << 26) | (rsf << 21) | (rt << 16) | (fs << 11)],
                              "regs": {rt: val64(rng)}, "fpr": {k: rng.getrandbits(64) for k in range(32)}, "fr": fr})
    for name, op in [("lwc1", 0x31), ("ldc1", 0x35), ("swc1", 0x39), ("sdc1", 0x3D)]:
        for n in range(16):
            size = 4 if name in ("lwc1", "swc1") else 8
            base = rng.randrange(1, 32)
            ft = rng.randrange(0, 32)
            off = rng.randrange(0, 0x80 // size) * size
            addr = DATA + 0x40 + off
            mem = bytes(rng.getrandbits(8) for _ in range(0x100))
            cases.append({"name": name, "insns": [i_type(op, base, ft, 0)], "regs": {base: addr},
                          "fpr": {k: rng.getrandbits(64) for k in range(32)}, "mem": [(DATA_PHYS, mem)], "bus": True})
    for n in range(12):
        rt = rng.randrange(1, 32)
        cases.append({"name": "ctc1", "insns": [(0x11 << 26) | (6 << 21) | (rt << 16) | (31 << 11)],
                      "regs": {rt: rng.getrandbits(32) & 0x01800003}})
        cases.append({"name": "cfc1", "insns": [(0x11 << 26) | (2 << 21) | (rt << 16) | (31 << 11)],
                      "regs": {}, "fcr31": rng.getrandbits(32) & 0x0183FFFF & ~0x0003F000})
    for name, rtf in [("bc1f", 0), ("bc1t", 1), ("bc1fl", 2), ("bc1tl", 3)]:
        for n in range(8):
            marker = i_type(0x09, 0, 1, 0x55)
            cases.append({"name": name, "insns": [(0x11 << 26) | (8 << 21) | (rtf << 16) | 3, marker, 0, 0, 0, 0],
                          "regs": {}, "fcr31": 0x00800000 if n % 2 else 0, "count": 2})
    return cases


def denormal(bits, kind):
    if kind == "s":
        b = bits & 0xFFFFFFFF
        return (b & 0x7F800000) == 0 and (b & 0x007FFFFF) != 0
    return (bits & 0x7FF0000000000000) == 0 and (bits & 0x000FFFFFFFFFFFFF) != 0


def is_nan(bits, kind):
    if kind == "s":
        b = bits & 0xFFFFFFFF
        return (b & 0x7F800000) == 0x7F800000 and (b & 0x007FFFFF) != 0
    return (bits & 0x7FF0000000000000) == 0x7FF0000000000000 and (bits & 0x000FFFFFFFFFFFFF) != 0


def emit(f, case, out):
    f.write("case %s\n" % case["name"])
    f.write("insn %s\n" % " ".join("%08X" % w for w in case["insns"]))
    f.write("count %d\n" % case.get("count", 1))
    f.write("fr %d\n" % case.get("fr", 1))
    init = " ".join("r%d=%016X" % (k, v & MASK64) for k, v in sorted(case["regs"].items()) if k and v)
    init += " hi=%016X lo=%016X fcr31=%08X" % (case.get("hi", 0), case.get("lo", 0), case.get("fcr31", 0))
    init += "".join(" f%d=%016X" % (k, v) for k, v in sorted(case.get("fpr", {}).items()) if v)
    f.write("init%s\n" % (" " + init.strip()))
    for addr, data in case.get("mem", []):
        f.write("mem %08X %s\n" % (addr, data.hex().upper()))
    regs = " ".join("r%d=%016X" % (k, v) for k, v in sorted(out["regs"].items()) if v)
    regs += " hi=%016X lo=%016X fcr31=%08X" % (out["hi"], out["lo"], out["fcr31"])
    regs += "".join(" f%d=%016X" % (k, v) for k, v in sorted(out["fpr"].items()) if v)
    f.write("final %s\n" % regs.strip())
    if out["exc"] is not None:
        f.write("exc %d\n" % out["exc"])
    else:
        f.write("pc %016X\n" % out["pc"])
    if case.get("mem") or out["mem"] != bytes(0x100):
        f.write("memafter %08X %s\n" % (DATA_PHYS, out["mem"].hex().upper()))
    if case.get("bus"):
        for kind, addr, size, value in out["bus"]:
            f.write("bus %s %08X %d %016X\n" % (kind, addr, size, value))
    f.write("end\n")


def keep(case, out):
    """Drop cases that reach behaviour the R4000 model does not share with
    the VR4300 (see the module comment)."""
    if out["exc"] is not None and out["exc"] >= 100:
        return False
    res = case.get("fp_result")
    if res:
        fd, kind = res
        if out["exc"] == 15 or out["exc"] == 15:
            return False
        if kind and (denormal(out["fpr"][fd], kind) or is_nan(out["fpr"][fd], kind)):
            return False
        if out["fcr31"] & 0x00020000:
            return False
        # Out-of-range conversions: the model returns 0x7FFFFFFF with
        # "invalid"; the VR4300 raises "unimplemented" (n64_cpu_test.c).
        if out["fcr31"] & 0x00010000 and case["name"].split(".")[0] in ("round", "trunc", "ceil", "floor", "cvt"):
            return False
        # Underflow is reported before rounding by the host the model runs
        # on and differently by others; keep only cases without it.
        if out["fcr31"] & 0x00002000:
            return False
    if out["exc"] == 15:
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    rng = random.Random(0x4E3634)
    oracle = Oracle()
    cases = int_cases(rng) + mem_cases(rng) + branch_cases(rng) + trap_cases(rng) + fp_cases(rng)
    import io
    buf = io.StringIO()
    buf.write("# VR4300 per-instruction vectors. Generated by tools/n64/gen_vectors.py from\n")
    buf.write("# Unicorn's MIPS64 R4000 model (a second implementation, not hardware). Do not edit.\n")
    kept = 0
    for case in cases:
        out = oracle.run(case)
        if not keep(case, out):
            continue
        emit(buf, case, out)
        kept += 1
    text = buf.getvalue()
    if args.check:
        try:
            with open(OUT) as f:
                current = f.read()
        except OSError:
            current = ""
        if current != text:
            print("vector file is out of date: run tools/n64/gen_vectors.py")
            return 1
        print("%d cases, up to date" % kept)
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", newline="\n") as f:
        f.write(text)
    print("wrote %d of %d cases to %s" % (kept, len(cases), OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
