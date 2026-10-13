/*
 * VR4300 instruction handlers: the one source of instruction semantics.
 *
 * The interpreter (n64_cpu.c) decodes an instruction and calls the handler
 * for it; generated code calls the same handlers with constant instruction
 * words, so the field extraction folds away. Every handler is
 * force-inlined and takes (N64Cpu *c, uint32_t i).
 *
 * Handlers do not sequence: branches are split into a condition
 * (n64_cond_*) and the link (n64_link), and the caller - the interpreter's
 * delay-slot state or the emitter's straight-line code - decides where
 * execution goes. A handler that raises an exception sets c->exc (via
 * n64_cpu_exception) and leaves the rest of the state as the hardware
 * does; the caller checks c->exc after any handler whose N64_F_FAULT flag
 * is set.
 *
 * Behaviour follows ares (n64/cpu/interpreter-ipu.cpp, interpreter-scc.cpp,
 * ISC) where the VR4300 differs from a textbook MIPS III: MULT is a 64 x 35
 * bit multiply when the operands are not sign-extended, DIV divides the
 * sign-extended low word by all 64 bits of rt, SRA and SRAV shift the
 * whole 64-bit register before truncating, and the 64-bit instructions are
 * reserved in 32-bit user and supervisor mode. Ported from ares
 * (https://github.com/ares-emulator/ares). Copyright (c) 2004-2025 ares
 * team, Near et al. Permission to use, copy, modify, and/or distribute this
 * software for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear in all
 * copies.
 */
#ifndef N64_CPU_IMPL_H
#define N64_CPU_IMPL_H

#include "n64_bus.h"
#include "n64_cpu.h"
#include "n64_cpu_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N64_OP N64_FORCEINLINE void

/* Out-of-line parts (n64_cpu.c). */
void n64_cpu_address_error(N64Cpu *c, uint64_t vaddr, bool store);
void n64_cpu_reserved(N64Cpu *c);
void n64_cpu_unusable(N64Cpu *c, uint32_t cop);

static inline uint64_t n64_sext32(uint64_t v) { return (uint64_t)(int64_t)(int32_t)(uint32_t)v; }
/* Arithmetic right shift without relying on the implementation-defined
 * behaviour of >> on negative values. */
static inline uint64_t n64_sra64(uint64_t v, unsigned s)
{
    s &= 63u;
    if (!s) return v;
    uint64_t r = v >> s;
    if (v >> 63) r |= ~(UINT64_MAX >> s);
    return r;
}

N64_FORCEINLINE void n64_wr(N64Cpu *c, unsigned r, uint64_t v)
{
#ifdef N64_CONSTANT_INSNS
    /* Generated code: r is a constant, so this is one store or none. */
    if (r) c->gpr[r] = v;
#else
    c->gpr[r] = v;
    c->gpr[0] = 0;
#endif
}

#define N64_GS(c, i) ((c)->gpr[N64_RS(i)])
#define N64_GT(c, i) ((c)->gpr[N64_RT(i)])

/* The 64-bit integer instructions are reserved in 32-bit user and
 * supervisor mode. */
#define N64_NEED64(c) \
    do { \
        if (!(c)->kernel && !(c)->addr64) { \
            n64_cpu_reserved(c); \
            return; \
        } \
    } while (0)

/* ---- memory ---- */

/* The shortcut to RDRAM: kernel mode, KSEG0 or KSEG1 (sign-extended), and
 * inside the RDRAM that is present. */
N64_FORCEINLINE bool n64_fast_ram(const N64Cpu *c, uint64_t va, uint32_t size, uint32_t *pa)
{
    if (c->kernel && va - 0xFFFFFFFF80000000ull < 0x40000000ull) {
        uint32_t p = (uint32_t)va & 0x1FFFFFFFu;
        if (p + size <= c->links.rdram_size) {
            *pa = p;
            return true;
        }
    }
    return false;
}

N64_FORCEINLINE void n64_note_store(N64Cpu *c, uint32_t pa, uint32_t size)
{
    if (c->links.code_map && c->links.code_map[pa >> N64_CODE_GRANULE_SHIFT]) n64_bus_code_written(c, pa, size);
}

/* Translate an aligned access; on failure the exception is raised. */
N64_FORCEINLINE bool n64_xlate(N64Cpu *c, uint64_t va, uint32_t size, bool store, uint32_t *pa)
{
    if (va & (size - 1u)) {
        n64_cpu_address_error(c, va, store);
        return false;
    }
    if (n64_fast_ram(c, va, size, pa)) return true;
    return n64_cpu_translate(c, va, store, pa);
}

N64_FORCEINLINE bool n64_in_ram(const N64Cpu *c, uint32_t pa, uint32_t size) { return pa + size <= c->links.rdram_size; }

N64_FORCEINLINE bool n64_load8(N64Cpu *c, uint64_t va, uint8_t *out)
{
    uint32_t pa;
    if (!n64_xlate(c, va, 1, false, &pa)) return false;
    *out = n64_in_ram(c, pa, 1) ? c->links.rdram[pa] : n64_bus_read8(c, pa);
    return true;
}

N64_FORCEINLINE bool n64_load16(N64Cpu *c, uint64_t va, uint16_t *out)
{
    uint32_t pa;
    if (!n64_xlate(c, va, 2, false, &pa)) return false;
    *out = n64_in_ram(c, pa, 2) ? n64_be16(c->links.rdram + pa) : n64_bus_read16(c, pa);
    return true;
}

N64_FORCEINLINE bool n64_load32(N64Cpu *c, uint64_t va, uint32_t *out)
{
    uint32_t pa;
    if (!n64_xlate(c, va, 4, false, &pa)) return false;
    *out = n64_in_ram(c, pa, 4) ? n64_be32(c->links.rdram + pa) : n64_bus_read32(c, pa);
    return true;
}

N64_FORCEINLINE bool n64_load64(N64Cpu *c, uint64_t va, uint64_t *out)
{
    uint32_t pa;
    if (!n64_xlate(c, va, 8, false, &pa)) return false;
    *out = n64_in_ram(c, pa, 8) ? n64_be64(c->links.rdram + pa) : n64_bus_read64(c, pa);
    return true;
}

N64_FORCEINLINE bool n64_store8(N64Cpu *c, uint64_t va, uint8_t v)
{
    uint32_t pa;
    if (!n64_xlate(c, va, 1, true, &pa)) return false;
    if (n64_in_ram(c, pa, 1)) {
        c->links.rdram[pa] = v;
        n64_note_store(c, pa, 1);
    } else {
        n64_bus_write8(c, pa, v);
    }
    return true;
}

N64_FORCEINLINE bool n64_store16(N64Cpu *c, uint64_t va, uint16_t v)
{
    uint32_t pa;
    if (!n64_xlate(c, va, 2, true, &pa)) return false;
    if (n64_in_ram(c, pa, 2)) {
        n64_put_be16(c->links.rdram + pa, v);
        n64_note_store(c, pa, 2);
    } else {
        n64_bus_write16(c, pa, v);
    }
    return true;
}

N64_FORCEINLINE bool n64_store32(N64Cpu *c, uint64_t va, uint32_t v)
{
    uint32_t pa;
    if (!n64_xlate(c, va, 4, true, &pa)) return false;
    if (n64_in_ram(c, pa, 4)) {
        n64_put_be32(c->links.rdram + pa, v);
        n64_note_store(c, pa, 4);
    } else {
        n64_bus_write32(c, pa, v);
    }
    return true;
}

N64_FORCEINLINE bool n64_store64(N64Cpu *c, uint64_t va, uint64_t v)
{
    uint32_t pa;
    if (!n64_xlate(c, va, 8, true, &pa)) return false;
    if (n64_in_ram(c, pa, 8)) {
        n64_put_be64(c->links.rdram + pa, v);
        n64_note_store(c, pa, 8);
    } else {
        n64_bus_write64(c, pa, v);
    }
    return true;
}

/* The bytes of an aligned unit that SWL/SWR/SDL/SDR write: bit k of mask
 * set writes byte k (big-endian order) of `bytes`. */
N64_FORCEINLINE void n64_store_partial(N64Cpu *c, uint32_t pa, const uint8_t *bytes, uint32_t size, uint32_t mask)
{
    for (uint32_t k = 0; k < size; k++) {
        if (!(mask & (1u << k))) continue;
        if (n64_in_ram(c, pa + k, 1)) {
            c->links.rdram[pa + k] = bytes[k];
            n64_note_store(c, pa + k, 1);
        } else {
            n64_bus_write8(c, pa + k, bytes[k]);
        }
    }
}

/* ---- arithmetic and logic ---- */

N64_OP n64_op_sll(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), n64_sext32((uint32_t)N64_GT(c, i) << N64_SA(i))); }
N64_OP n64_op_srl(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), n64_sext32((uint32_t)N64_GT(c, i) >> N64_SA(i))); }
N64_OP n64_op_sra(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), n64_sext32(n64_sra64(N64_GT(c, i), N64_SA(i)))); }
N64_OP n64_op_sllv(N64Cpu *c, uint32_t i)
{
    n64_wr(c, N64_RD(i), n64_sext32((uint32_t)N64_GT(c, i) << (N64_GS(c, i) & 31u)));
}
N64_OP n64_op_srlv(N64Cpu *c, uint32_t i)
{
    n64_wr(c, N64_RD(i), n64_sext32((uint32_t)N64_GT(c, i) >> (N64_GS(c, i) & 31u)));
}
N64_OP n64_op_srav(N64Cpu *c, uint32_t i)
{
    n64_wr(c, N64_RD(i), n64_sext32(n64_sra64(N64_GT(c, i), (unsigned)(N64_GS(c, i) & 31u))));
}
N64_OP n64_op_dsllv(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), N64_GT(c, i) << (N64_GS(c, i) & 63u));
}
N64_OP n64_op_dsrlv(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), N64_GT(c, i) >> (N64_GS(c, i) & 63u));
}
N64_OP n64_op_dsrav(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), n64_sra64(N64_GT(c, i), (unsigned)(N64_GS(c, i) & 63u)));
}
N64_OP n64_op_dsll(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), N64_GT(c, i) << N64_SA(i));
}
N64_OP n64_op_dsrl(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), N64_GT(c, i) >> N64_SA(i));
}
N64_OP n64_op_dsra(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), n64_sra64(N64_GT(c, i), N64_SA(i)));
}
N64_OP n64_op_dsll32(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), N64_GT(c, i) << (N64_SA(i) + 32u));
}
N64_OP n64_op_dsrl32(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), N64_GT(c, i) >> (N64_SA(i) + 32u));
}
N64_OP n64_op_dsra32(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), n64_sra64(N64_GT(c, i), N64_SA(i) + 32u));
}

N64_OP n64_op_mfhi(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), c->hi); }
N64_OP n64_op_mflo(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), c->lo); }
N64_OP n64_op_mthi(N64Cpu *c, uint32_t i) { c->hi = N64_GS(c, i); }
N64_OP n64_op_mtlo(N64Cpu *c, uint32_t i) { c->lo = N64_GS(c, i); }

/* 64 x 64 -> 128 bit products, portably (MSVC has no __int128). */
static inline void n64_mul64u(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (mid << 32) | (uint32_t)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

N64_OP n64_op_mult(N64Cpu *c, uint32_t i)
{
    /* 64 x 35 bits: rt's low 35 bits, sign-extended (ares). */
    uint64_t b = n64_sra64(N64_GT(c, i) << 29, 29);
    uint64_t r = N64_GS(c, i) * b;
    c->lo = n64_sext32(r);
    c->hi = n64_sext32(r >> 32);
}
N64_OP n64_op_multu(N64Cpu *c, uint32_t i)
{
    uint64_t r = (uint64_t)(uint32_t)N64_GS(c, i) * (uint32_t)N64_GT(c, i);
    c->lo = n64_sext32(r);
    c->hi = n64_sext32(r >> 32);
}
N64_OP n64_op_div(N64Cpu *c, uint32_t i)
{
    int64_t a = (int32_t)(uint32_t)N64_GS(c, i);
    int64_t b = (int64_t)N64_GT(c, i);
    if (b) {
        /* |a| < 2^31, so a / b cannot overflow. */
        c->lo = n64_sext32((uint64_t)(a / b));
        c->hi = n64_sext32((uint64_t)(a % b));
    } else {
        c->lo = a < 0 ? 1u : UINT64_MAX;
        c->hi = (uint64_t)a;
    }
}
N64_OP n64_op_divu(N64Cpu *c, uint32_t i)
{
    uint32_t a = (uint32_t)N64_GS(c, i), b = (uint32_t)N64_GT(c, i);
    if (b) {
        c->lo = n64_sext32(a / b);
        c->hi = n64_sext32(a % b);
    } else {
        c->lo = UINT64_MAX;
        c->hi = n64_sext32(a);
    }
}
N64_OP n64_op_dmult(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t a = N64_GS(c, i), b = N64_GT(c, i), hi, lo;
    n64_mul64u(a, b, &hi, &lo);
    if (a >> 63) hi -= b;
    if (b >> 63) hi -= a;
    c->hi = hi;
    c->lo = lo;
}
N64_OP n64_op_dmultu(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_mul64u(N64_GS(c, i), N64_GT(c, i), &c->hi, &c->lo);
}
N64_OP n64_op_ddiv(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    int64_t a = (int64_t)N64_GS(c, i), b = (int64_t)N64_GT(c, i);
    if (b) {
        if (a == INT64_MIN && b == -1) {
            c->lo = (uint64_t)a;
            c->hi = 0;
        } else {
            c->lo = (uint64_t)(a / b);
            c->hi = (uint64_t)(a % b);
        }
    } else {
        c->lo = a < 0 ? 1u : UINT64_MAX;
        c->hi = (uint64_t)a;
    }
}
N64_OP n64_op_ddivu(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t a = N64_GS(c, i), b = N64_GT(c, i);
    if (b) {
        c->lo = a / b;
        c->hi = a % b;
    } else {
        c->lo = UINT64_MAX;
        c->hi = a;
    }
}

N64_OP n64_op_add(N64Cpu *c, uint32_t i)
{
    uint32_t a = (uint32_t)N64_GS(c, i), b = (uint32_t)N64_GT(c, i), r = a + b;
    if (~(a ^ b) & (a ^ r) & 0x80000000u) {
        n64_cpu_exception(c, N64_EXC_OV, 0, false);
        return;
    }
    n64_wr(c, N64_RD(i), n64_sext32(r));
}
N64_OP n64_op_addu(N64Cpu *c, uint32_t i)
{
    n64_wr(c, N64_RD(i), n64_sext32((uint32_t)N64_GS(c, i) + (uint32_t)N64_GT(c, i)));
}
N64_OP n64_op_sub(N64Cpu *c, uint32_t i)
{
    uint32_t a = (uint32_t)N64_GS(c, i), b = (uint32_t)N64_GT(c, i), r = a - b;
    if ((a ^ b) & (a ^ r) & 0x80000000u) {
        n64_cpu_exception(c, N64_EXC_OV, 0, false);
        return;
    }
    n64_wr(c, N64_RD(i), n64_sext32(r));
}
N64_OP n64_op_subu(N64Cpu *c, uint32_t i)
{
    n64_wr(c, N64_RD(i), n64_sext32((uint32_t)N64_GS(c, i) - (uint32_t)N64_GT(c, i)));
}
N64_OP n64_op_dadd(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t a = N64_GS(c, i), b = N64_GT(c, i), r = a + b;
    if (~(a ^ b) & (a ^ r) & 0x8000000000000000ull) {
        n64_cpu_exception(c, N64_EXC_OV, 0, false);
        return;
    }
    n64_wr(c, N64_RD(i), r);
}
N64_OP n64_op_daddu(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), N64_GS(c, i) + N64_GT(c, i));
}
N64_OP n64_op_dsub(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t a = N64_GS(c, i), b = N64_GT(c, i), r = a - b;
    if ((a ^ b) & (a ^ r) & 0x8000000000000000ull) {
        n64_cpu_exception(c, N64_EXC_OV, 0, false);
        return;
    }
    n64_wr(c, N64_RD(i), r);
}
N64_OP n64_op_dsubu(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RD(i), N64_GS(c, i) - N64_GT(c, i));
}
N64_OP n64_op_and(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), N64_GS(c, i) & N64_GT(c, i)); }
N64_OP n64_op_or(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), N64_GS(c, i) | N64_GT(c, i)); }
N64_OP n64_op_xor(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), N64_GS(c, i) ^ N64_GT(c, i)); }
N64_OP n64_op_nor(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), ~(N64_GS(c, i) | N64_GT(c, i))); }
N64_OP n64_op_slt(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), (int64_t)N64_GS(c, i) < (int64_t)N64_GT(c, i)); }
N64_OP n64_op_sltu(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RD(i), N64_GS(c, i) < N64_GT(c, i)); }

N64_OP n64_op_addi(N64Cpu *c, uint32_t i)
{
    uint32_t a = (uint32_t)N64_GS(c, i), b = (uint32_t)N64_IMM(i), r = a + b;
    if (~(a ^ b) & (a ^ r) & 0x80000000u) {
        n64_cpu_exception(c, N64_EXC_OV, 0, false);
        return;
    }
    n64_wr(c, N64_RT(i), n64_sext32(r));
}
N64_OP n64_op_addiu(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RT(i), n64_sext32((uint32_t)N64_GS(c, i) + (uint32_t)N64_IMM(i))); }
N64_OP n64_op_daddi(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t a = N64_GS(c, i), b = (uint64_t)N64_IMM(i), r = a + b;
    if (~(a ^ b) & (a ^ r) & 0x8000000000000000ull) {
        n64_cpu_exception(c, N64_EXC_OV, 0, false);
        return;
    }
    n64_wr(c, N64_RT(i), r);
}
N64_OP n64_op_daddiu(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    n64_wr(c, N64_RT(i), N64_GS(c, i) + (uint64_t)N64_IMM(i));
}
N64_OP n64_op_slti(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RT(i), (int64_t)N64_GS(c, i) < N64_IMM(i)); }
N64_OP n64_op_sltiu(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RT(i), N64_GS(c, i) < (uint64_t)N64_IMM(i)); }
N64_OP n64_op_andi(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RT(i), N64_GS(c, i) & N64_UIMM(i)); }
N64_OP n64_op_ori(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RT(i), N64_GS(c, i) | N64_UIMM(i)); }
N64_OP n64_op_xori(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RT(i), N64_GS(c, i) ^ N64_UIMM(i)); }
N64_OP n64_op_lui(N64Cpu *c, uint32_t i) { n64_wr(c, N64_RT(i), n64_sext32((uint32_t)(i & 0xFFFFu) << 16)); }

/* ---- traps and system ---- */

#define N64_TRAP_IF(cond) \
    do { \
        if (cond) n64_cpu_exception(c, N64_EXC_TR, 0, false); \
    } while (0)

N64_OP n64_op_tge(N64Cpu *c, uint32_t i) { N64_TRAP_IF((int64_t)N64_GS(c, i) >= (int64_t)N64_GT(c, i)); }
N64_OP n64_op_tgeu(N64Cpu *c, uint32_t i) { N64_TRAP_IF(N64_GS(c, i) >= N64_GT(c, i)); }
N64_OP n64_op_tlt(N64Cpu *c, uint32_t i) { N64_TRAP_IF((int64_t)N64_GS(c, i) < (int64_t)N64_GT(c, i)); }
N64_OP n64_op_tltu(N64Cpu *c, uint32_t i) { N64_TRAP_IF(N64_GS(c, i) < N64_GT(c, i)); }
N64_OP n64_op_teq(N64Cpu *c, uint32_t i) { N64_TRAP_IF(N64_GS(c, i) == N64_GT(c, i)); }
N64_OP n64_op_tne(N64Cpu *c, uint32_t i) { N64_TRAP_IF(N64_GS(c, i) != N64_GT(c, i)); }
N64_OP n64_op_tgei(N64Cpu *c, uint32_t i) { N64_TRAP_IF((int64_t)N64_GS(c, i) >= N64_IMM(i)); }
N64_OP n64_op_tgeiu(N64Cpu *c, uint32_t i) { N64_TRAP_IF(N64_GS(c, i) >= (uint64_t)N64_IMM(i)); }
N64_OP n64_op_tlti(N64Cpu *c, uint32_t i) { N64_TRAP_IF((int64_t)N64_GS(c, i) < N64_IMM(i)); }
N64_OP n64_op_tltiu(N64Cpu *c, uint32_t i) { N64_TRAP_IF(N64_GS(c, i) < (uint64_t)N64_IMM(i)); }
N64_OP n64_op_teqi(N64Cpu *c, uint32_t i) { N64_TRAP_IF((int64_t)N64_GS(c, i) == N64_IMM(i)); }
N64_OP n64_op_tnei(N64Cpu *c, uint32_t i) { N64_TRAP_IF((int64_t)N64_GS(c, i) != N64_IMM(i)); }

N64_OP n64_op_syscall(N64Cpu *c, uint32_t i)
{
    (void)i;
    n64_cpu_exception(c, N64_EXC_SYS, 0, false);
}
N64_OP n64_op_break(N64Cpu *c, uint32_t i)
{
    (void)i;
    n64_cpu_exception(c, N64_EXC_BP, 0, false);
}
N64_OP n64_op_sync(N64Cpu *c, uint32_t i) { (void)c; (void)i; }
N64_OP n64_op_invalid(N64Cpu *c, uint32_t i)
{
    (void)i;
    n64_cpu_reserved(c);
}

/* ---- branches and jumps ----
 * Conditions are pure; the caller sequences the delay slot. */

N64_FORCEINLINE bool n64_cond_beq(N64Cpu *c, uint32_t i) { return N64_GS(c, i) == N64_GT(c, i); }
N64_FORCEINLINE bool n64_cond_bne(N64Cpu *c, uint32_t i) { return N64_GS(c, i) != N64_GT(c, i); }
N64_FORCEINLINE bool n64_cond_blez(N64Cpu *c, uint32_t i) { return (int64_t)N64_GS(c, i) <= 0; }
N64_FORCEINLINE bool n64_cond_bgtz(N64Cpu *c, uint32_t i) { return (int64_t)N64_GS(c, i) > 0; }
N64_FORCEINLINE bool n64_cond_bltz(N64Cpu *c, uint32_t i) { return (int64_t)N64_GS(c, i) < 0; }
N64_FORCEINLINE bool n64_cond_bgez(N64Cpu *c, uint32_t i) { return (int64_t)N64_GS(c, i) >= 0; }
#define n64_cond_beql n64_cond_beq
#define n64_cond_bnel n64_cond_bne
#define n64_cond_blezl n64_cond_blez
#define n64_cond_bgtzl n64_cond_bgtz
#define n64_cond_bltzl n64_cond_bltz
#define n64_cond_bgezl n64_cond_bgez
#define n64_cond_bltzal n64_cond_bltz
#define n64_cond_bgezal n64_cond_bgez
#define n64_cond_bltzall n64_cond_bltz
#define n64_cond_bgezall n64_cond_bgez

/* BC1F/BC1T and their likely forms: raise "coprocessor unusable" when
 * COP1 is off (the caller checks c->exc first). */
N64_FORCEINLINE bool n64_cond_bc1(N64Cpu *c, bool value)
{
    if (!(c->cp0_status & N64_SR_CU1)) {
        n64_cpu_unusable(c, 1);
        return false;
    }
    return ((c->fcr31 & N64_FCR31_C) != 0) == value;
}
#define n64_cond_bc1f(c, i) n64_cond_bc1((c), false)
#define n64_cond_bc1t(c, i) n64_cond_bc1((c), true)
#define n64_cond_bc1fl(c, i) n64_cond_bc1((c), false)
#define n64_cond_bc1tl(c, i) n64_cond_bc1((c), true)

/* The return address a linking branch or jump at `pc` writes. JALR writes
 * rd, the rest r31. */
N64_FORCEINLINE void n64_link(N64Cpu *c, uint32_t i, uint64_t pc)
{
    unsigned r = (i >> 26) == 0 ? N64_RD(i) : 31u;
    n64_wr(c, r, pc + 8u);
}

/* ---- loads and stores ---- */

#define N64_EA(c, i) (N64_GS(c, i) + (uint64_t)N64_IMM(i))

N64_OP n64_op_lb(N64Cpu *c, uint32_t i)
{
    uint8_t v;
    if (n64_load8(c, N64_EA(c, i), &v)) n64_wr(c, N64_RT(i), (uint64_t)(int64_t)(int8_t)v);
}
N64_OP n64_op_lbu(N64Cpu *c, uint32_t i)
{
    uint8_t v;
    if (n64_load8(c, N64_EA(c, i), &v)) n64_wr(c, N64_RT(i), v);
}
N64_OP n64_op_lh(N64Cpu *c, uint32_t i)
{
    uint16_t v;
    if (n64_load16(c, N64_EA(c, i), &v)) n64_wr(c, N64_RT(i), (uint64_t)(int64_t)(int16_t)v);
}
N64_OP n64_op_lhu(N64Cpu *c, uint32_t i)
{
    uint16_t v;
    if (n64_load16(c, N64_EA(c, i), &v)) n64_wr(c, N64_RT(i), v);
}
N64_OP n64_op_lw(N64Cpu *c, uint32_t i)
{
    uint32_t v;
    if (n64_load32(c, N64_EA(c, i), &v)) n64_wr(c, N64_RT(i), n64_sext32(v));
}
N64_OP n64_op_lwu(N64Cpu *c, uint32_t i)
{
    uint32_t v;
    if (n64_load32(c, N64_EA(c, i), &v)) n64_wr(c, N64_RT(i), v);
}
N64_OP n64_op_ld(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t v;
    if (n64_load64(c, N64_EA(c, i), &v)) n64_wr(c, N64_RT(i), v);
}
N64_OP n64_op_ll(N64Cpu *c, uint32_t i)
{
    uint64_t va = N64_EA(c, i);
    uint32_t pa, v;
    if (!n64_xlate(c, va, 4, false, &pa)) return;
    v = n64_in_ram(c, pa, 4) ? n64_be32(c->links.rdram + pa) : n64_bus_read32(c, pa);
    n64_wr(c, N64_RT(i), n64_sext32(v));
    c->cp0_ll_addr = pa >> 4;
    c->llbit = 1;
}
N64_OP n64_op_lld(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t va = N64_EA(c, i), v;
    uint32_t pa;
    if (!n64_xlate(c, va, 8, false, &pa)) return;
    v = n64_in_ram(c, pa, 8) ? n64_be64(c->links.rdram + pa) : n64_bus_read64(c, pa);
    n64_wr(c, N64_RT(i), v);
    c->cp0_ll_addr = pa >> 4;
    c->llbit = 1;
}

/* LWL/LWR/LDL/LDR read the aligned unit holding the address and merge the
 * bytes the instruction names into rt (big-endian, the console's mode). */
N64_OP n64_op_lwl(N64Cpu *c, uint32_t i)
{
    uint64_t va = N64_EA(c, i);
    uint32_t word;
    if (!n64_load32(c, va & ~(uint64_t)3u, &word)) return;
    unsigned sh = (unsigned)(va & 3u) * 8u;
    uint32_t keep = sh ? (0xFFFFFFFFu >> (32u - sh)) : 0u;
    uint32_t data = ((uint32_t)N64_GT(c, i) & keep) | (word << sh);
    n64_wr(c, N64_RT(i), n64_sext32(data));
}
N64_OP n64_op_lwr(N64Cpu *c, uint32_t i)
{
    uint64_t va = N64_EA(c, i);
    uint32_t word;
    if (!n64_load32(c, va & ~(uint64_t)3u, &word)) return;
    unsigned sh = (3u - (unsigned)(va & 3u)) * 8u;
    uint32_t keep = sh ? ~(0xFFFFFFFFu >> sh) : 0u;
    uint32_t data = ((uint32_t)N64_GT(c, i) & keep) | (word >> sh);
    /* Only the full-word case sign-extends; the others keep rt's top half. */
    if ((va & 3u) == 3u) n64_wr(c, N64_RT(i), n64_sext32(data));
    else n64_wr(c, N64_RT(i), (N64_GT(c, i) & 0xFFFFFFFF00000000ull) | data);
}
N64_OP n64_op_ldl(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t va = N64_EA(c, i), mem;
    if (!n64_load64(c, va & ~(uint64_t)7u, &mem)) return;
    unsigned sh = (unsigned)(va & 7u) * 8u;
    uint64_t keep = sh ? (UINT64_MAX >> (64u - sh)) : 0u;
    n64_wr(c, N64_RT(i), (N64_GT(c, i) & keep) | (mem << sh));
}
N64_OP n64_op_ldr(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t va = N64_EA(c, i), mem;
    if (!n64_load64(c, va & ~(uint64_t)7u, &mem)) return;
    unsigned sh = (7u - (unsigned)(va & 7u)) * 8u;
    uint64_t keep = sh ? ~(UINT64_MAX >> sh) : 0u;
    n64_wr(c, N64_RT(i), (N64_GT(c, i) & keep) | (mem >> sh));
}

N64_OP n64_op_sb(N64Cpu *c, uint32_t i) { (void)n64_store8(c, N64_EA(c, i), (uint8_t)N64_GT(c, i)); }
N64_OP n64_op_sh(N64Cpu *c, uint32_t i) { (void)n64_store16(c, N64_EA(c, i), (uint16_t)N64_GT(c, i)); }
N64_OP n64_op_sw(N64Cpu *c, uint32_t i) { (void)n64_store32(c, N64_EA(c, i), (uint32_t)N64_GT(c, i)); }
N64_OP n64_op_sd(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    (void)n64_store64(c, N64_EA(c, i), N64_GT(c, i));
}
N64_OP n64_op_sc(N64Cpu *c, uint32_t i)
{
    if (c->llbit) n64_wr(c, N64_RT(i), n64_store32(c, N64_EA(c, i), (uint32_t)N64_GT(c, i)) ? 1u : 0u);
    else n64_wr(c, N64_RT(i), 0);
}
N64_OP n64_op_scd(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    if (c->llbit) n64_wr(c, N64_RT(i), n64_store64(c, N64_EA(c, i), N64_GT(c, i)) ? 1u : 0u);
    else n64_wr(c, N64_RT(i), 0);
}

/* SWL writes rt's top bytes from the address to the end of its word; SWR
 * writes rt's bottom bytes from the start of the word to the address. */
N64_OP n64_op_swl(N64Cpu *c, uint32_t i)
{
    uint64_t va = N64_EA(c, i);
    uint32_t pa;
    if (!n64_xlate(c, va & ~(uint64_t)3u, 4, true, &pa)) return;
    unsigned k = (unsigned)(va & 3u);
    uint8_t bytes[4];
    n64_put_be32(bytes, (uint32_t)N64_GT(c, i) >> (8u * k));
    n64_store_partial(c, pa, bytes, 4, (0xFu << k) & 0xFu);
}
N64_OP n64_op_swr(N64Cpu *c, uint32_t i)
{
    uint64_t va = N64_EA(c, i);
    uint32_t pa;
    if (!n64_xlate(c, va & ~(uint64_t)3u, 4, true, &pa)) return;
    unsigned k = (unsigned)(va & 3u);
    uint8_t bytes[4];
    n64_put_be32(bytes, (uint32_t)N64_GT(c, i) << (8u * (3u - k)));
    n64_store_partial(c, pa, bytes, 4, (1u << (k + 1u)) - 1u);
}
N64_OP n64_op_sdl(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t va = N64_EA(c, i);
    uint32_t pa;
    if (!n64_xlate(c, va & ~(uint64_t)7u, 8, true, &pa)) return;
    unsigned k = (unsigned)(va & 7u);
    uint8_t bytes[8];
    n64_put_be64(bytes, N64_GT(c, i) >> (8u * k));
    n64_store_partial(c, pa, bytes, 8, (0xFFu << k) & 0xFFu);
}
N64_OP n64_op_sdr(N64Cpu *c, uint32_t i)
{
    N64_NEED64(c);
    uint64_t va = N64_EA(c, i);
    uint32_t pa;
    if (!n64_xlate(c, va & ~(uint64_t)7u, 8, true, &pa)) return;
    unsigned k = (unsigned)(va & 7u);
    uint8_t bytes[8];
    n64_put_be64(bytes, N64_GT(c, i) << (8u * (7u - k)));
    n64_store_partial(c, pa, bytes, 8, (1u << (k + 1u)) - 1u);
}

/* CACHE: the caches are not modelled (N64-PLAN.md, known deviations), but
 * the address is still translated, so its TLB exceptions happen. */
N64_OP n64_op_cache(N64Cpu *c, uint32_t i)
{
    uint64_t va = N64_EA(c, i);
    uint32_t pa;
    if (n64_fast_ram(c, va, 1, &pa)) return;
    (void)n64_cpu_translate(c, va, false, &pa);
}

/* ---- coprocessor 0 ---- */

#define N64_NEED_CU0(c) \
    do { \
        if (!(c)->kernel && !((c)->cp0_status & N64_SR_CU0)) { \
            n64_cpu_unusable(c, 0); \
            return; \
        } \
    } while (0)

N64_OP n64_op_mfc0(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU0(c);
    n64_wr(c, N64_RT(i), n64_sext32(n64_cpu_cop0_read(c, N64_RD(i))));
}
N64_OP n64_op_dmfc0(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU0(c);
    N64_NEED64(c);
    n64_wr(c, N64_RT(i), n64_cpu_cop0_read(c, N64_RD(i)));
}
N64_OP n64_op_mtc0(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU0(c);
    n64_cpu_cop0_write(c, N64_RD(i), N64_GT(c, i));
}
N64_OP n64_op_dmtc0(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU0(c);
    N64_NEED64(c);
    n64_cpu_cop0_write(c, N64_RD(i), N64_GT(c, i));
}
N64_OP n64_op_cop0_nop(N64Cpu *c, uint32_t i) { (void)c; (void)i; }
N64_OP n64_op_tlbr(N64Cpu *c, uint32_t i)
{
    (void)i;
    N64_NEED_CU0(c);
    n64_cpu_tlbr(c);
}
N64_OP n64_op_tlbwi(N64Cpu *c, uint32_t i)
{
    (void)i;
    N64_NEED_CU0(c);
    n64_cpu_tlbwi(c);
}
N64_OP n64_op_tlbwr(N64Cpu *c, uint32_t i)
{
    (void)i;
    N64_NEED_CU0(c);
    n64_cpu_tlbwr(c);
}
N64_OP n64_op_tlbp(N64Cpu *c, uint32_t i)
{
    (void)i;
    N64_NEED_CU0(c);
    n64_cpu_tlbp(c);
}
/* ERET: next_pc becomes EPC (or ErrorEPC); the caller continues there. */
N64_OP n64_op_eret(N64Cpu *c, uint32_t i)
{
    (void)i;
    N64_NEED_CU0(c);
    n64_cpu_eret(c);
}

/* ---- the CPU's coprocessor 2 (absent: one latch) ---- */

#define N64_NEED_CU2(c) \
    do { \
        if (!((c)->cp0_status & N64_SR_CU2)) { \
            n64_cpu_unusable(c, 2); \
            return; \
        } \
    } while (0)

N64_OP n64_op_mfc2(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU2(c);
    n64_wr(c, N64_RT(i), n64_sext32(c->cop2_latch));
}
N64_OP n64_op_dmfc2(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU2(c);
    n64_wr(c, N64_RT(i), c->cop2_latch);
}
N64_OP n64_op_cfc2(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU2(c);
    n64_wr(c, N64_RT(i), n64_sext32(c->cop2_latch));
}
N64_OP n64_op_mtc2(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU2(c);
    c->cop2_latch = N64_GT(c, i);
}
N64_OP n64_op_dmtc2(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU2(c);
    c->cop2_latch = N64_GT(c, i);
}
N64_OP n64_op_ctc2(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU2(c);
    c->cop2_latch = N64_GT(c, i);
}
N64_OP n64_op_cop2_invalid(N64Cpu *c, uint32_t i)
{
    (void)i;
    N64_NEED_CU2(c);
    n64_cpu_exception(c, N64_EXC_RI, 2, false);
}

#ifdef __cplusplus
}
#endif

#include "n64_fpu_impl.h"

#endif /* N64_CPU_IMPL_H */
