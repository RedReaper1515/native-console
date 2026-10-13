/*
 * RSP instruction semantics: one force-inlined handler per instruction,
 * each taking the RSP and the 32-bit instruction word. The interpreter in
 * n64_rsp.c decodes an instruction and calls one of these; a recompiler can
 * call the same handlers with the instruction word as a constant and let the
 * compiler fold the field extraction away, so there is one source of
 * instruction semantics.
 *
 * Contract for a handler: it runs the instruction at rsp->pc. It may write
 * r[0] (the step clears it afterwards). A taken branch or jump sets
 * branch_pending and branch_target and nothing else about the PC; the step
 * turns that into the delay slot's next_pc. Handlers do no timing.
 *
 * Every computation is done in unsigned or wide signed arithmetic and
 * converted with the helpers below, so nothing depends on signed overflow,
 * on shifting negative numbers or on implementation-defined narrowing.
 *
 * The vector unit's loads, stores and computational instructions are here
 * as n64_rsp_ref_*: portable C, one lane or one byte at a time, and the
 * reference for what each does. n64_rsp_fast.h, included at the end, gives
 * each its n64_rsp_op_* handler: SSE2 or a whole-word copy where there is
 * one, the reference otherwise. tests/n64/n64_rsp_fast_test.c runs both on
 * the same inputs and compares every output.
 *
 * Ported from ares (https://github.com/ares-emulator/ares) ares/n64/rsp
 * (interpreter-ipu.cpp, interpreter-scc.cpp and the portable scalar paths
 * of interpreter-vpu.cpp). Copyright (c) 2004-2025 ares team, Near et al.
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */
#ifndef N64_RSP_IMPL_H
#define N64_RSP_IMPL_H

#include "n64_rsp.h"

#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#include <stdlib.h>
#endif

/* Define N64_RSP_REFERENCE_ONLY to build the interpreter from the portable
 * reference handlers alone, as a host with no fast path gets it. */

/* A little-endian host can move DMEM's big-endian bytes a word at a time. */
#if defined(_MSC_VER)
#define N64_RSP_HOST_LE 1
#define N64_RSP_BSWAP16(v) _byteswap_ushort(v)
#define N64_RSP_BSWAP32(v) _byteswap_ulong(v)
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define N64_RSP_HOST_LE 1
#define N64_RSP_BSWAP16(v) __builtin_bswap16(v)
#define N64_RSP_BSWAP32(v) __builtin_bswap32(v)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* n64_rsp_tables.c */
extern const uint16_t n64_rsp_rcp_table[512];
extern const uint16_t n64_rsp_rsq_table[512];

/* Instruction fields. */
#define N64_RSP_RS(op) (((op) >> 21) & 31u)
#define N64_RSP_RT(op) (((op) >> 16) & 31u)
#define N64_RSP_RD(op) (((op) >> 11) & 31u)
#define N64_RSP_SA(op) (((op) >> 6) & 31u)
#define N64_RSP_VT(op) (((op) >> 16) & 31u)
#define N64_RSP_VS(op) (((op) >> 11) & 31u)
#define N64_RSP_VD(op) (((op) >> 6) & 31u)
#define N64_RSP_VE(op) (((op) >> 21) & 15u) /* computational element */
#define N64_RSP_VDE(op) (((op) >> 11) & 7u) /* VRCP/VMOV destination lane */
#define N64_RSP_LE(op) (((op) >> 7) & 15u)  /* load/store/move element */

/* Sign-extended 16-bit immediate as a 32-bit two's complement pattern. */
N64_FORCEINLINE uint32_t n64_rsp_simm(uint32_t op) { return ((op & 0xFFFFu) ^ 0x8000u) - 0x8000u; }

/* Sign-extended 7-bit vector load/store offset. */
N64_FORCEINLINE int32_t n64_rsp_voff(uint32_t op) { return (int32_t)(op & 0x3Fu) - (int32_t)(op & 0x40u); }

N64_FORCEINLINE int32_t n64_rsp_s16(uint32_t v)
{
    v &= 0xFFFFu;
    return (int32_t)v - (int32_t)((v & 0x8000u) << 1);
}

N64_FORCEINLINE int32_t n64_rsp_s32(uint32_t v)
{
    return (v & 0x80000000u) ? -(int32_t)(~v) - 1 : (int32_t)v;
}

N64_FORCEINLINE int64_t n64_rsp_s48(uint64_t v)
{
    v &= 0xFFFFFFFFFFFFull;
    return (v & 0x800000000000ull) ? (int64_t)v - (int64_t)0x1000000000000ll : (int64_t)v;
}

N64_FORCEINLINE int16_t n64_rsp_i16(uint32_t v) { return (int16_t)n64_rsp_s16(v); }

N64_FORCEINLINE uint16_t n64_rsp_clamp16(int64_t v)
{
    if (v > 32767) return 0x7FFFu;
    if (v < -32768) return 0x8000u;
    return (uint16_t)((uint64_t)v & 0xFFFFu);
}

N64_FORCEINLINE uint32_t n64_rsp_asr32(uint32_t v, unsigned s)
{
    return (v & 0x80000000u) ? ~(~v >> s) : v >> s;
}

N64_FORCEINLINE unsigned n64_rsp_clz32(uint32_t v)
{
    if (v == 0) return 32;
#if defined(_MSC_VER)
    {
        unsigned long top;
        _BitScanReverse(&top, v);
        return 31u - (unsigned)top;
    }
#elif defined(__GNUC__)
    return (unsigned)__builtin_clz(v);
#else
    {
        unsigned n = 0;
        while (!(v & 0x80000000u)) {
            v <<= 1;
            n++;
        }
        return n;
    }
#endif
}

/* Big-endian words at any alignment. */
N64_FORCEINLINE uint32_t n64_rsp_ld16(const uint8_t *p)
{
#ifdef N64_RSP_HOST_LE
    uint16_t v;
    memcpy(&v, p, 2);
    return N64_RSP_BSWAP16(v);
#else
    return n64_be16(p);
#endif
}

N64_FORCEINLINE uint32_t n64_rsp_ld32(const uint8_t *p)
{
#ifdef N64_RSP_HOST_LE
    uint32_t v;
    memcpy(&v, p, 4);
    return N64_RSP_BSWAP32(v);
#else
    return n64_be32(p);
#endif
}

N64_FORCEINLINE void n64_rsp_st16(uint8_t *p, uint32_t v)
{
#ifdef N64_RSP_HOST_LE
    uint16_t x = N64_RSP_BSWAP16((uint16_t)v);
    memcpy(p, &x, 2);
#else
    n64_put_be16(p, (uint16_t)v);
#endif
}

N64_FORCEINLINE void n64_rsp_st32(uint8_t *p, uint32_t v)
{
#ifdef N64_RSP_HOST_LE
    v = N64_RSP_BSWAP32(v);
    memcpy(p, &v, 4);
#else
    n64_put_be32(p, v);
#endif
}

/* DMEM is addressed with wrap-around and no alignment requirement. */
N64_FORCEINLINE uint32_t n64_rsp_rd8(const N64Rsp *rsp, uint32_t a) { return rsp->dmem[a & 0xFFFu]; }
N64_FORCEINLINE void n64_rsp_wr8(N64Rsp *rsp, uint32_t a, uint32_t v) { rsp->dmem[a & 0xFFFu] = (uint8_t)v; }

/* A vector register as 16 big-endian bytes: byte 0 is the high byte of
 * lane 0. */
N64_FORCEINLINE uint32_t n64_rsp_vbyte(const uint16_t *v, unsigned i)
{
    i &= 15u;
    return (i & 1u) ? (v[i >> 1] & 0xFFu) : (uint32_t)(v[i >> 1] >> 8);
}

N64_FORCEINLINE void n64_rsp_set_vbyte(uint16_t *v, unsigned i, uint32_t b)
{
    i &= 15u;
    if (i & 1u)
        v[i >> 1] = (uint16_t)((v[i >> 1] & 0xFF00u) | (b & 0xFFu));
    else
        v[i >> 1] = (uint16_t)((v[i >> 1] & 0x00FFu) | ((b & 0xFFu) << 8));
}

/* The element field's lane selection: 0-1 the whole vector, 2-3 pairs
 * (0q, 1q), 4-7 quarters (0h-3h), 8-15 one lane broadcast. */
static const uint8_t n64_rsp_elem_lane[16][8] = {
    {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7}, {0, 0, 2, 2, 4, 4, 6, 6}, {1, 1, 3, 3, 5, 5, 7, 7},
    {0, 0, 0, 0, 4, 4, 4, 4}, {1, 1, 1, 1, 5, 5, 5, 5}, {2, 2, 2, 2, 6, 6, 6, 6}, {3, 3, 3, 3, 7, 7, 7, 7},
    {0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 2}, {3, 3, 3, 3, 3, 3, 3, 3},
    {4, 4, 4, 4, 4, 4, 4, 4}, {5, 5, 5, 5, 5, 5, 5, 5}, {6, 6, 6, 6, 6, 6, 6, 6}, {7, 7, 7, 7, 7, 7, 7, 7},
};

/* Copies the operands of a computational op: vs as it is, vt with the
 * element selection applied. Copying first makes vd aliasing either source
 * harmless. */
N64_FORCEINLINE void n64_rsp_vops(const N64Rsp *rsp, uint32_t op, uint16_t vs[8], uint16_t vt[8])
{
    const uint16_t *s = rsp->vu.v[N64_RSP_VS(op)];
    const uint16_t *t = rsp->vu.v[N64_RSP_VT(op)];
    const uint8_t *sel = n64_rsp_elem_lane[N64_RSP_VE(op)];
    unsigned n;
    for (n = 0; n < 8; n++) {
        vs[n] = s[n];
        vt[n] = t[sel[n]];
    }
}

N64_FORCEINLINE void n64_rsp_vstore(N64Rsp *rsp, uint32_t op, const uint16_t vd[8])
{
    uint16_t *d = rsp->vu.v[N64_RSP_VD(op)];
    unsigned n;
    for (n = 0; n < 8; n++) d[n] = vd[n];
}

N64_FORCEINLINE unsigned n64_rsp_flag(uint32_t mask, unsigned n) { return (mask >> n) & 1u; }

N64_FORCEINLINE uint16_t n64_rsp_setflag(uint16_t mask, unsigned n, unsigned v)
{
    return (uint16_t)((mask & ~(1u << n)) | ((v ? 1u : 0u) << n));
}

N64_FORCEINLINE uint64_t n64_rsp_acc(const N64RspVu *vu, unsigned n)
{
    return ((uint64_t)vu->acc_h[n] << 32) | ((uint64_t)vu->acc_m[n] << 16) | vu->acc_l[n];
}

N64_FORCEINLINE void n64_rsp_set_acc(N64RspVu *vu, unsigned n, uint64_t v)
{
    vu->acc_h[n] = (uint16_t)(v >> 32);
    vu->acc_m[n] = (uint16_t)(v >> 16);
    vu->acc_l[n] = (uint16_t)v;
}

/* Clamps the accumulator to a 16-bit result: when bits 47..31 (or 47..15)
 * are not a sign extension, the result saturates to `neg` or `pos`;
 * otherwise it is the middle (`mid`) or low slice. */
N64_FORCEINLINE uint16_t n64_rsp_acc_sat(const N64RspVu *vu, unsigned n, int mid, uint16_t neg, uint16_t pos)
{
    uint32_t h = vu->acc_h[n], m = vu->acc_m[n];
    if (h & 0x8000u) {
        if (h != 0xFFFFu || !(m & 0x8000u)) return neg;
    } else {
        if (h != 0 || (m & 0x8000u)) return pos;
    }
    return mid ? (uint16_t)m : vu->acc_l[n];
}

N64_FORCEINLINE void n64_rsp_take(N64Rsp *rsp, uint32_t target)
{
    rsp->branch_pending = 1;
    rsp->branch_target = target & 0xFFCu;
}

N64_FORCEINLINE uint32_t n64_rsp_branch_target(const N64Rsp *rsp, uint32_t op)
{
    return rsp->pc + 4u + (n64_rsp_simm(op) << 2);
}

/* Scalar unit. */

N64_FORCEINLINE void n64_rsp_op_sll(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RT(op)] << N64_RSP_SA(op);
}

N64_FORCEINLINE void n64_rsp_op_srl(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RT(op)] >> N64_RSP_SA(op);
}

N64_FORCEINLINE void n64_rsp_op_sra(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = n64_rsp_asr32(rsp->r[N64_RSP_RT(op)], N64_RSP_SA(op));
}

N64_FORCEINLINE void n64_rsp_op_sllv(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RT(op)] << (rsp->r[N64_RSP_RS(op)] & 31u);
}

N64_FORCEINLINE void n64_rsp_op_srlv(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RT(op)] >> (rsp->r[N64_RSP_RS(op)] & 31u);
}

N64_FORCEINLINE void n64_rsp_op_srav(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = n64_rsp_asr32(rsp->r[N64_RSP_RT(op)], rsp->r[N64_RSP_RS(op)] & 31u);
}

/* The SPECIAL functions the RSP lacks (multiply, divide, the 64-bit
 * shifts, traps, SYSCALL...) are not ignored: ares models them, from
 * hardware observation, as rd = rs >> (rs & 31). */
N64_FORCEINLINE void n64_rsp_op_special_invalid(N64Rsp *rsp, uint32_t op)
{
    uint32_t s = rsp->r[N64_RSP_RS(op)];
    rsp->r[N64_RSP_RD(op)] = s >> (s & 31u);
}

N64_FORCEINLINE void n64_rsp_op_jr(N64Rsp *rsp, uint32_t op) { n64_rsp_take(rsp, rsp->r[N64_RSP_RS(op)]); }

N64_FORCEINLINE void n64_rsp_op_jalr(N64Rsp *rsp, uint32_t op)
{
    n64_rsp_take(rsp, rsp->r[N64_RSP_RS(op)]);
    rsp->r[N64_RSP_RD(op)] = (rsp->pc + 8u) & 0xFFFu;
}

N64_FORCEINLINE void n64_rsp_op_break(N64Rsp *rsp, uint32_t op)
{
    (void)op;
    rsp->status |= N64_SP_STATUS_HALT | N64_SP_STATUS_BROKE;
    if ((rsp->status & N64_SP_STATUS_INTR_BREAK) && rsp->links.interrupt) rsp->links.interrupt(rsp->links.user, 1);
}

/* ADD and SUB never trap on the RSP; they are ADDU and SUBU. */
N64_FORCEINLINE void n64_rsp_op_addu(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RS(op)] + rsp->r[N64_RSP_RT(op)];
}

N64_FORCEINLINE void n64_rsp_op_subu(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RS(op)] - rsp->r[N64_RSP_RT(op)];
}

N64_FORCEINLINE void n64_rsp_op_and(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RS(op)] & rsp->r[N64_RSP_RT(op)];
}

N64_FORCEINLINE void n64_rsp_op_or(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RS(op)] | rsp->r[N64_RSP_RT(op)];
}

N64_FORCEINLINE void n64_rsp_op_xor(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RS(op)] ^ rsp->r[N64_RSP_RT(op)];
}

N64_FORCEINLINE void n64_rsp_op_nor(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = ~(rsp->r[N64_RSP_RS(op)] | rsp->r[N64_RSP_RT(op)]);
}

N64_FORCEINLINE void n64_rsp_op_slt(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = n64_rsp_s32(rsp->r[N64_RSP_RS(op)]) < n64_rsp_s32(rsp->r[N64_RSP_RT(op)]) ? 1u : 0u;
}

N64_FORCEINLINE void n64_rsp_op_sltu(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RD(op)] = rsp->r[N64_RSP_RS(op)] < rsp->r[N64_RSP_RT(op)] ? 1u : 0u;
}

N64_FORCEINLINE void n64_rsp_op_bltz(N64Rsp *rsp, uint32_t op)
{
    if (rsp->r[N64_RSP_RS(op)] & 0x80000000u) n64_rsp_take(rsp, n64_rsp_branch_target(rsp, op));
}

N64_FORCEINLINE void n64_rsp_op_bgez(N64Rsp *rsp, uint32_t op)
{
    if (!(rsp->r[N64_RSP_RS(op)] & 0x80000000u)) n64_rsp_take(rsp, n64_rsp_branch_target(rsp, op));
}

/* The link register is written whether or not the branch is taken, after
 * the condition has read rs. */
N64_FORCEINLINE void n64_rsp_op_bltzal(N64Rsp *rsp, uint32_t op)
{
    n64_rsp_op_bltz(rsp, op);
    rsp->r[31] = (rsp->pc + 8u) & 0xFFFu;
}

N64_FORCEINLINE void n64_rsp_op_bgezal(N64Rsp *rsp, uint32_t op)
{
    n64_rsp_op_bgez(rsp, op);
    rsp->r[31] = (rsp->pc + 8u) & 0xFFFu;
}

N64_FORCEINLINE void n64_rsp_op_j(N64Rsp *rsp, uint32_t op) { n64_rsp_take(rsp, (op & 0x03FFFFFFu) << 2); }

N64_FORCEINLINE void n64_rsp_op_jal(N64Rsp *rsp, uint32_t op)
{
    rsp->r[31] = (rsp->pc + 8u) & 0xFFFu;
    n64_rsp_take(rsp, (op & 0x03FFFFFFu) << 2);
}

N64_FORCEINLINE void n64_rsp_op_beq(N64Rsp *rsp, uint32_t op)
{
    if (rsp->r[N64_RSP_RS(op)] == rsp->r[N64_RSP_RT(op)]) n64_rsp_take(rsp, n64_rsp_branch_target(rsp, op));
}

N64_FORCEINLINE void n64_rsp_op_bne(N64Rsp *rsp, uint32_t op)
{
    if (rsp->r[N64_RSP_RS(op)] != rsp->r[N64_RSP_RT(op)]) n64_rsp_take(rsp, n64_rsp_branch_target(rsp, op));
}

N64_FORCEINLINE void n64_rsp_op_blez(N64Rsp *rsp, uint32_t op)
{
    if (n64_rsp_s32(rsp->r[N64_RSP_RS(op)]) <= 0) n64_rsp_take(rsp, n64_rsp_branch_target(rsp, op));
}

N64_FORCEINLINE void n64_rsp_op_bgtz(N64Rsp *rsp, uint32_t op)
{
    if (n64_rsp_s32(rsp->r[N64_RSP_RS(op)]) > 0) n64_rsp_take(rsp, n64_rsp_branch_target(rsp, op));
}

N64_FORCEINLINE void n64_rsp_op_addiu(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RT(op)] = rsp->r[N64_RSP_RS(op)] + n64_rsp_simm(op);
}

N64_FORCEINLINE void n64_rsp_op_slti(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RT(op)] = n64_rsp_s32(rsp->r[N64_RSP_RS(op)]) < n64_rsp_s16(op) ? 1u : 0u;
}

/* The immediate is sign-extended and then compared unsigned. */
N64_FORCEINLINE void n64_rsp_op_sltiu(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RT(op)] = rsp->r[N64_RSP_RS(op)] < n64_rsp_simm(op) ? 1u : 0u;
}

N64_FORCEINLINE void n64_rsp_op_andi(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RT(op)] = rsp->r[N64_RSP_RS(op)] & (op & 0xFFFFu);
}

N64_FORCEINLINE void n64_rsp_op_ori(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RT(op)] = rsp->r[N64_RSP_RS(op)] | (op & 0xFFFFu);
}

N64_FORCEINLINE void n64_rsp_op_xori(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RT(op)] = rsp->r[N64_RSP_RS(op)] ^ (op & 0xFFFFu);
}

N64_FORCEINLINE void n64_rsp_op_lui(N64Rsp *rsp, uint32_t op) { rsp->r[N64_RSP_RT(op)] = (op & 0xFFFFu) << 16; }

/* Scalar loads and stores reach DMEM only. Any alignment works: each byte
 * wraps within the 4 KiB on its own, so only an access that ends inside
 * DMEM is done as one word. */
N64_FORCEINLINE uint32_t n64_rsp_ea(const N64Rsp *rsp, uint32_t op) { return rsp->r[N64_RSP_RS(op)] + n64_rsp_simm(op); }

N64_FORCEINLINE uint32_t n64_rsp_rd16(const N64Rsp *rsp, uint32_t a)
{
    a &= 0xFFFu;
    if (a <= 0xFFEu) return n64_rsp_ld16(rsp->dmem + a);
    return (n64_rsp_rd8(rsp, a) << 8) | n64_rsp_rd8(rsp, a + 1u);
}

N64_FORCEINLINE uint32_t n64_rsp_rd32(const N64Rsp *rsp, uint32_t a)
{
    a &= 0xFFFu;
    if (a <= 0xFFCu) return n64_rsp_ld32(rsp->dmem + a);
    return (n64_rsp_rd8(rsp, a) << 24) | (n64_rsp_rd8(rsp, a + 1u) << 16) | (n64_rsp_rd8(rsp, a + 2u) << 8) |
           n64_rsp_rd8(rsp, a + 3u);
}

N64_FORCEINLINE void n64_rsp_wr16(N64Rsp *rsp, uint32_t a, uint32_t v)
{
    a &= 0xFFFu;
    if (a <= 0xFFEu) {
        n64_rsp_st16(rsp->dmem + a, v);
        return;
    }
    n64_rsp_wr8(rsp, a, v >> 8);
    n64_rsp_wr8(rsp, a + 1u, v);
}

N64_FORCEINLINE void n64_rsp_wr32(N64Rsp *rsp, uint32_t a, uint32_t v)
{
    a &= 0xFFFu;
    if (a <= 0xFFCu) {
        n64_rsp_st32(rsp->dmem + a, v);
        return;
    }
    n64_rsp_wr8(rsp, a, v >> 24);
    n64_rsp_wr8(rsp, a + 1u, v >> 16);
    n64_rsp_wr8(rsp, a + 2u, v >> 8);
    n64_rsp_wr8(rsp, a + 3u, v);
}

N64_FORCEINLINE void n64_rsp_op_lb(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RT(op)] = (n64_rsp_rd8(rsp, n64_rsp_ea(rsp, op)) ^ 0x80u) - 0x80u;
}

N64_FORCEINLINE void n64_rsp_op_lbu(N64Rsp *rsp, uint32_t op) { rsp->r[N64_RSP_RT(op)] = n64_rsp_rd8(rsp, n64_rsp_ea(rsp, op)); }

N64_FORCEINLINE void n64_rsp_op_lh(N64Rsp *rsp, uint32_t op)
{
    rsp->r[N64_RSP_RT(op)] = (n64_rsp_rd16(rsp, n64_rsp_ea(rsp, op)) ^ 0x8000u) - 0x8000u;
}

N64_FORCEINLINE void n64_rsp_op_lhu(N64Rsp *rsp, uint32_t op) { rsp->r[N64_RSP_RT(op)] = n64_rsp_rd16(rsp, n64_rsp_ea(rsp, op)); }

/* LW and LWU are the same instruction with 32-bit registers. */
N64_FORCEINLINE void n64_rsp_op_lw(N64Rsp *rsp, uint32_t op) { rsp->r[N64_RSP_RT(op)] = n64_rsp_rd32(rsp, n64_rsp_ea(rsp, op)); }

N64_FORCEINLINE void n64_rsp_op_sb(N64Rsp *rsp, uint32_t op) { n64_rsp_wr8(rsp, n64_rsp_ea(rsp, op), rsp->r[N64_RSP_RT(op)]); }

N64_FORCEINLINE void n64_rsp_op_sh(N64Rsp *rsp, uint32_t op)
{
    n64_rsp_wr16(rsp, n64_rsp_ea(rsp, op), rsp->r[N64_RSP_RT(op)]);
}

N64_FORCEINLINE void n64_rsp_op_sw(N64Rsp *rsp, uint32_t op)
{
    n64_rsp_wr32(rsp, n64_rsp_ea(rsp, op), rsp->r[N64_RSP_RT(op)]);
}

/* COP0: registers 0-7 are the SP registers (reads and writes have the same
 * side effects as the CPU's, so reading 7 takes the semaphore and writing
 * 2 or 3 starts a DMA), 8-15 the RDP's command registers. */
N64_FORCEINLINE void n64_rsp_op_mfc0(N64Rsp *rsp, uint32_t op)
{
    unsigned reg = N64_RSP_RD(op) & 15u;
    uint32_t v;
    if (reg & 8u)
        v = rsp->links.dp_read ? rsp->links.dp_read(rsp->links.user, reg & 7u) : 0;
    else
        v = n64_rsp_reg_read(rsp, reg);
    rsp->r[N64_RSP_RT(op)] = v;
}

N64_FORCEINLINE void n64_rsp_op_mtc0(N64Rsp *rsp, uint32_t op)
{
    unsigned reg = N64_RSP_RD(op) & 15u;
    uint32_t v = rsp->r[N64_RSP_RT(op)];
    if (reg & 8u) {
        if (rsp->links.dp_write) rsp->links.dp_write(rsp->links.user, reg & 7u, v);
    } else {
        n64_rsp_reg_write(rsp, reg, v);
    }
}

/* Vector unit: moves. */

/* MFC2 reads the two bytes at the element, the second one wrapping to byte
 * 0 for element 15, and sign-extends them. */
N64_FORCEINLINE void n64_rsp_op_mfc2(N64Rsp *rsp, uint32_t op)
{
    const uint16_t *v = rsp->vu.v[N64_RSP_RD(op)];
    unsigned e = N64_RSP_LE(op);
    uint32_t h = (n64_rsp_vbyte(v, e) << 8) | n64_rsp_vbyte(v, e + 1u);
    rsp->r[N64_RSP_RT(op)] = (h ^ 0x8000u) - 0x8000u;
}

/* MTC2 writes two bytes at the element; at element 15 only the first. */
N64_FORCEINLINE void n64_rsp_op_mtc2(N64Rsp *rsp, uint32_t op)
{
    uint16_t *v = rsp->vu.v[N64_RSP_RD(op)];
    unsigned e = N64_RSP_LE(op);
    uint32_t x = rsp->r[N64_RSP_RT(op)];
    n64_rsp_set_vbyte(v, e, x >> 8);
    if (e != 15u) n64_rsp_set_vbyte(v, e + 1u, x);
}

/* CFC2/CTC2 control register numbers: 0 VCO, 1 VCC, 2 VCE (3 also VCE,
 * which ares marks unverified). The 16-bit value is sign-extended. */
N64_FORCEINLINE void n64_rsp_op_cfc2(N64Rsp *rsp, uint32_t op)
{
    uint32_t v;
    switch (N64_RSP_RD(op) & 3u) {
    case 0: v = ((uint32_t)(rsp->vu.vco_hi & 0xFFu) << 8) | (rsp->vu.vco_lo & 0xFFu); break;
    case 1: v = ((uint32_t)(rsp->vu.vcc_hi & 0xFFu) << 8) | (rsp->vu.vcc_lo & 0xFFu); break;
    default: v = rsp->vu.vce; break;
    }
    rsp->r[N64_RSP_RT(op)] = (v ^ 0x8000u) - 0x8000u;
}

N64_FORCEINLINE void n64_rsp_op_ctc2(N64Rsp *rsp, uint32_t op)
{
    uint32_t v = rsp->r[N64_RSP_RT(op)];
    switch (N64_RSP_RD(op) & 3u) {
    case 0:
        rsp->vu.vco_lo = (uint16_t)(v & 0xFFu);
        rsp->vu.vco_hi = (uint16_t)((v >> 8) & 0xFFu);
        break;
    case 1:
        rsp->vu.vcc_lo = (uint16_t)(v & 0xFFu);
        rsp->vu.vcc_hi = (uint16_t)((v >> 8) & 0xFFu);
        break;
    default: rsp->vu.vce = (uint8_t)v; break;
    }
}

/* Vector unit: loads (LWC2). */

/* The byte-sequential loads (LBV, LSV, LLV, LDV) copy `size` bytes from the
 * address into the register starting at byte `e`, stopping at byte 15. */
N64_FORCEINLINE void n64_rsp_vload_seq(N64Rsp *rsp, uint32_t op, unsigned size)
{
    uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * (int32_t)size);
    unsigned e = N64_RSP_LE(op), end = e + size, i;
    if (end > 16u) end = 16u;
    for (i = e; i < end; i++) n64_rsp_set_vbyte(v, i, n64_rsp_rd8(rsp, a++));
}

N64_FORCEINLINE void n64_rsp_ref_lbv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_seq(rsp, op, 1); }
N64_FORCEINLINE void n64_rsp_ref_lsv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_seq(rsp, op, 2); }
N64_FORCEINLINE void n64_rsp_ref_llv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_seq(rsp, op, 4); }
N64_FORCEINLINE void n64_rsp_ref_ldv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_seq(rsp, op, 8); }

/* LQV loads from the address up to the end of its 16-byte line. */
N64_FORCEINLINE void n64_rsp_ref_lqv(N64Rsp *rsp, uint32_t op)
{
    uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    unsigned e = N64_RSP_LE(op), end = 16u + e - (a & 15u), i;
    if (end > 16u) end = 16u;
    for (i = e; i < end; i++) n64_rsp_set_vbyte(v, i, n64_rsp_rd8(rsp, a++));
}

/* LRV loads the part of the 16-byte line before the address into the end
 * of the register. */
N64_FORCEINLINE void n64_rsp_ref_lrv(N64Rsp *rsp, uint32_t op)
{
    uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    int32_t start = 16 - ((int32_t)(a & 15u) - (int32_t)N64_RSP_LE(op));
    int32_t i;
    a &= ~15u;
    for (i = start; i < 16; i++) n64_rsp_set_vbyte(v, (unsigned)i, n64_rsp_rd8(rsp, a++));
}

/* LPV, LUV and LHV load bytes into the top of each lane (shifted by 8, 7
 * and 7), reading within the aligned 16 bytes with a rotation set by the
 * address's low bits less the element. */
N64_FORCEINLINE void n64_rsp_vload_packed(N64Rsp *rsp, uint32_t op, int32_t scale, unsigned stride, unsigned shift)
{
    uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * scale);
    uint32_t index = (a & 7u) - N64_RSP_LE(op);
    unsigned n;
    a &= ~7u;
    for (n = 0; n < 8; n++) v[n] = (uint16_t)(n64_rsp_rd8(rsp, a + ((index + n * stride) & 15u)) << shift);
}

N64_FORCEINLINE void n64_rsp_ref_lpv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_packed(rsp, op, 8, 1, 8); }
N64_FORCEINLINE void n64_rsp_ref_luv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_packed(rsp, op, 8, 1, 7); }
N64_FORCEINLINE void n64_rsp_ref_lhv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_packed(rsp, op, 16, 2, 7); }

/* LFV loads every fourth byte into four lanes' tops (shifted by 7); only
 * bytes e..e+7 of the result reach the register. */
N64_FORCEINLINE void n64_rsp_ref_lfv(N64Rsp *rsp, uint32_t op)
{
    uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint16_t tmp[8];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    unsigned e = N64_RSP_LE(op), end = e + 8u, i;
    uint32_t index = (a & 7u) - e;
    a &= ~7u;
    for (i = 0; i < 4; i++) {
        tmp[i] = (uint16_t)(n64_rsp_rd8(rsp, a + ((index + i * 4u) & 15u)) << 7);
        tmp[i + 4] = (uint16_t)(n64_rsp_rd8(rsp, a + ((index + i * 4u + 8u) & 15u)) << 7);
    }
    if (end > 16u) end = 16u;
    for (i = e; i < end; i++) n64_rsp_set_vbyte(v, i, n64_rsp_vbyte(tmp, i));
}

/* LWV is listed in the RSP manuals but the console's RSP does nothing for
 * it (ares leaves the opcode unimplemented on hardware evidence). */
N64_FORCEINLINE void n64_rsp_ref_lwv(N64Rsp *rsp, uint32_t op)
{
    (void)rsp;
    (void)op;
}

/* LTV loads a transposed column: byte pair i goes to register
 * (vt & ~7) + ((e / 2 + i) & 7), lane i, read from the aligned 16 bytes
 * starting at byte e + (address & 8). */
N64_FORCEINLINE void n64_rsp_ref_ltv(N64Rsp *rsp, uint32_t op)
{
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    unsigned e = N64_RSP_LE(op), base = N64_RSP_VT(op) & ~7u, off = e >> 1, i;
    uint32_t begin = a & ~7u;
    a = begin + ((e + (a & 8u)) & 15u);
    for (i = 0; i < 8; i++) {
        uint16_t *v = rsp->vu.v[base + off];
        n64_rsp_set_vbyte(v, i * 2u, n64_rsp_rd8(rsp, a++));
        if (a == begin + 16u) a = begin;
        n64_rsp_set_vbyte(v, i * 2u + 1u, n64_rsp_rd8(rsp, a++));
        if (a == begin + 16u) a = begin;
        off = (off + 1u) & 7u;
    }
}

/* Vector unit: stores (SWC2). */

/* SBV, SSV, SLV, SDV store `size` bytes starting at byte e, wrapping round
 * the register. */
N64_FORCEINLINE void n64_rsp_vstore_seq(N64Rsp *rsp, uint32_t op, unsigned size)
{
    const uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * (int32_t)size);
    unsigned e = N64_RSP_LE(op), i;
    for (i = e; i < e + size; i++) n64_rsp_wr8(rsp, a++, n64_rsp_vbyte(v, i));
}

N64_FORCEINLINE void n64_rsp_ref_sbv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_seq(rsp, op, 1); }
N64_FORCEINLINE void n64_rsp_ref_ssv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_seq(rsp, op, 2); }
N64_FORCEINLINE void n64_rsp_ref_slv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_seq(rsp, op, 4); }
N64_FORCEINLINE void n64_rsp_ref_sdv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_seq(rsp, op, 8); }

/* SQV stores from the address up to the end of its 16-byte line. */
N64_FORCEINLINE void n64_rsp_ref_sqv(N64Rsp *rsp, uint32_t op)
{
    const uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    unsigned e = N64_RSP_LE(op), end = e + (16u - (a & 15u)), i;
    for (i = e; i < end; i++) n64_rsp_wr8(rsp, a++, n64_rsp_vbyte(v, i));
}

/* SRV stores the end of the register into the line before the address. */
N64_FORCEINLINE void n64_rsp_ref_srv(N64Rsp *rsp, uint32_t op)
{
    const uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    unsigned e = N64_RSP_LE(op), end = e + (a & 15u), base = 16u - (a & 15u), i;
    a &= ~15u;
    for (i = e; i < end; i++) n64_rsp_wr8(rsp, a++, n64_rsp_vbyte(v, i + base));
}

/* SPV and SUV store each lane's top byte (bits 15..8 for SPV, 14..7 for
 * SUV); for register bytes 8-15 of the walk they swap roles. */
N64_FORCEINLINE void n64_rsp_vstore_packed(N64Rsp *rsp, uint32_t op, int unsigned_first)
{
    const uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 8);
    unsigned e = N64_RSP_LE(op), i;
    for (i = e; i < e + 8u; i++) {
        int low_half = (i & 15u) < 8u;
        if (low_half != unsigned_first)
            n64_rsp_wr8(rsp, a++, n64_rsp_vbyte(v, (i & 7u) << 1));
        else
            n64_rsp_wr8(rsp, a++, (uint32_t)v[i & 7u] >> 7);
    }
}

N64_FORCEINLINE void n64_rsp_ref_spv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_packed(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_ref_suv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_packed(rsp, op, 1); }

/* SHV stores bits 14..7 of each lane (read across the byte boundary from
 * element e) to every other byte of the aligned 16 bytes. */
N64_FORCEINLINE void n64_rsp_ref_shv(N64Rsp *rsp, uint32_t op)
{
    const uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    unsigned e = N64_RSP_LE(op), i;
    uint32_t index = a & 7u;
    a &= ~7u;
    for (i = 0; i < 8; i++) {
        unsigned b = e + i * 2u;
        uint32_t value = (n64_rsp_vbyte(v, b) << 1) | (n64_rsp_vbyte(v, b + 1u) >> 7);
        n64_rsp_wr8(rsp, a + ((index + i * 2u) & 15u), value);
    }
}

/* SFV stores bits 14..7 of four lanes to every fourth byte. Only some
 * elements pick a lane order; the rest (-1) store zeroes (as ares has it). */
static const int8_t n64_rsp_sfv_order[16][4] = {
    {0, 1, 2, 3}, {6, 7, 4, 5}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {1, 2, 3, 0}, {7, 4, 5, 6},
    {-1, -1, -1, -1}, {-1, -1, -1, -1}, {4, 5, 6, 7}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {3, 0, 1, 2},
    {5, 6, 7, 4}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {0, 1, 2, 3},
};

N64_FORCEINLINE void n64_rsp_ref_sfv(N64Rsp *rsp, uint32_t op)
{
    const uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    const int8_t *ord = n64_rsp_sfv_order[N64_RSP_LE(op)];
    uint32_t base = a & 7u;
    unsigned i;
    a &= ~7u;
    for (i = 0; i < 4; i++) {
        uint32_t value = ord[i] < 0 ? 0u : ((uint32_t)v[ord[i]] >> 7);
        n64_rsp_wr8(rsp, a + ((base + i * 4u) & 15u), value);
    }
}

/* SWV stores the register rotated by e into the aligned 16 bytes,
 * starting at the address's offset within them. */
N64_FORCEINLINE void n64_rsp_ref_swv(N64Rsp *rsp, uint32_t op)
{
    const uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    unsigned e = N64_RSP_LE(op), i;
    uint32_t base = a & 7u;
    a &= ~7u;
    for (i = e; i < e + 16u; i++) n64_rsp_wr8(rsp, a + (base++ & 15u), n64_rsp_vbyte(v, i));
}

/* STV stores a transposed column: the inverse of LTV. */
N64_FORCEINLINE void n64_rsp_ref_stv(N64Rsp *rsp, uint32_t op)
{
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    unsigned e = N64_RSP_LE(op) & ~1u, start = N64_RSP_VT(op) & ~7u, r;
    unsigned element = 16u - e;
    uint32_t base = (a & 7u) - e;
    a &= ~7u;
    for (r = start; r < start + 8u; r++) {
        const uint16_t *v = rsp->vu.v[r];
        n64_rsp_wr8(rsp, a + (base++ & 15u), n64_rsp_vbyte(v, element++));
        n64_rsp_wr8(rsp, a + (base++ & 15u), n64_rsp_vbyte(v, element++));
    }
}

/* Vector unit: multiplies. */

/* VMULF/VMULU: acc = vs * vt * 2 + 0x8000 (signed fractions, rounded). */
N64_FORCEINLINE void n64_rsp_vmulf_common(N64Rsp *rsp, uint32_t op, int is_unsigned)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t p = (int64_t)n64_rsp_s16(vs[n]) * n64_rsp_s16(vt[n]) * 2 + 0x8000;
        n64_rsp_set_acc(&rsp->vu, n, (uint64_t)p);
        if (!is_unsigned) {
            vd[n] = n64_rsp_acc_sat(&rsp->vu, n, 1, 0x8000u, 0x7FFFu);
        } else {
            uint32_t h = rsp->vu.acc_h[n], m = rsp->vu.acc_m[n];
            vd[n] = (h & 0x8000u) ? 0u : ((h ^ m) & 0x8000u) ? 0xFFFFu : (uint16_t)m;
        }
    }
    n64_rsp_vstore(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_ref_vmulf(N64Rsp *rsp, uint32_t op) { n64_rsp_vmulf_common(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_ref_vmulu(N64Rsp *rsp, uint32_t op) { n64_rsp_vmulf_common(rsp, op, 1); }

/* VMACF/VMACU: acc += vs * vt * 2. */
N64_FORCEINLINE void n64_rsp_vmacf_common(N64Rsp *rsp, uint32_t op, int is_unsigned)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t p = (int64_t)n64_rsp_s16(vs[n]) * n64_rsp_s16(vt[n]) * 2;
        n64_rsp_set_acc(&rsp->vu, n, n64_rsp_acc(&rsp->vu, n) + (uint64_t)p);
        if (!is_unsigned) {
            vd[n] = n64_rsp_acc_sat(&rsp->vu, n, 1, 0x8000u, 0x7FFFu);
        } else {
            uint32_t h = rsp->vu.acc_h[n], m = rsp->vu.acc_m[n];
            vd[n] = (h & 0x8000u) ? 0u : (h != 0 || (m & 0x8000u)) ? 0xFFFFu : (uint16_t)m;
        }
    }
    n64_rsp_vstore(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_ref_vmacf(N64Rsp *rsp, uint32_t op) { n64_rsp_vmacf_common(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_ref_vmacu(N64Rsp *rsp, uint32_t op) { n64_rsp_vmacf_common(rsp, op, 1); }

/* VRNDP/VRNDN add vt (shifted up 16 when the vs field is odd) to the
 * accumulator when it is non-negative (P) or negative (N). */
N64_FORCEINLINE void n64_rsp_vrnd_common(N64Rsp *rsp, uint32_t op, int positive)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t product = n64_rsp_s16(vt[n]);
        int64_t acc = n64_rsp_s48(n64_rsp_acc(&rsp->vu, n));
        if (N64_RSP_VS(op) & 1u) product *= 65536;
        if (positive ? acc >= 0 : acc < 0) acc = n64_rsp_s48((uint64_t)(acc + product));
        n64_rsp_set_acc(&rsp->vu, n, (uint64_t)acc);
        vd[n] = n64_rsp_clamp16(n64_rsp_s32((uint32_t)((uint64_t)acc >> 16)));
    }
    n64_rsp_vstore(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_ref_vrndp(N64Rsp *rsp, uint32_t op) { n64_rsp_vrnd_common(rsp, op, 1); }
N64_FORCEINLINE void n64_rsp_ref_vrndn(N64Rsp *rsp, uint32_t op) { n64_rsp_vrnd_common(rsp, op, 0); }

/* VMULQ: MPEG dequantisation multiply; negative products round up by 31,
 * the result keeps bits 31..20 of the product and clamps. */
N64_FORCEINLINE void n64_rsp_ref_vmulq(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int32_t product = n64_rsp_s16(vs[n]) * n64_rsp_s16(vt[n]);
        if (product < 0) product += 31;
        rsp->vu.acc_h[n] = (uint16_t)((uint32_t)product >> 16);
        rsp->vu.acc_m[n] = (uint16_t)(uint32_t)product;
        rsp->vu.acc_l[n] = 0;
        vd[n] = (uint16_t)(n64_rsp_clamp16(n64_rsp_s32(n64_rsp_asr32((uint32_t)product, 1))) & ~15u);
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMACQ: oddifies bits 47..16 of the accumulator towards zero by 32 when
 * bit 21 is clear, then stores like VMULQ. */
N64_FORCEINLINE void n64_rsp_ref_vmacq(N64Rsp *rsp, uint32_t op)
{
    uint16_t vd[8];
    unsigned n;
    for (n = 0; n < 8; n++) {
        uint32_t bits = ((uint32_t)rsp->vu.acc_h[n] << 16) | rsp->vu.acc_m[n];
        int32_t product = n64_rsp_s32(bits);
        if (product < 0 && !(bits & 32u))
            bits += 32u;
        else if (product >= 32 && !(bits & 32u))
            bits -= 32u;
        rsp->vu.acc_h[n] = (uint16_t)(bits >> 16);
        rsp->vu.acc_m[n] = (uint16_t)bits;
        vd[n] = (uint16_t)(n64_rsp_clamp16(n64_rsp_s32(n64_rsp_asr32(bits, 1))) & ~15u);
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMUDL: acc = (vs * vt) >> 16, both unsigned; result is the low slice. */
N64_FORCEINLINE void n64_rsp_ref_vmudl(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        n64_rsp_set_acc(&rsp->vu, n, ((uint32_t)vs[n] * (uint32_t)vt[n]) >> 16);
        vd[n] = rsp->vu.acc_l[n];
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMUDM: acc = signed vs * unsigned vt; result is the middle slice. */
N64_FORCEINLINE void n64_rsp_ref_vmudm(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t p = (int64_t)n64_rsp_s16(vs[n]) * vt[n];
        n64_rsp_set_acc(&rsp->vu, n, (uint64_t)p);
        vd[n] = rsp->vu.acc_m[n];
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMUDN: acc = unsigned vs * signed vt; result is the low slice. */
N64_FORCEINLINE void n64_rsp_ref_vmudn(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t p = (int64_t)vs[n] * n64_rsp_s16(vt[n]);
        n64_rsp_set_acc(&rsp->vu, n, (uint64_t)p);
        vd[n] = rsp->vu.acc_l[n];
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMUDH: acc = (vs * vt) << 16, both signed; result clamps the middle. */
N64_FORCEINLINE void n64_rsp_ref_vmudh(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t p = (int64_t)n64_rsp_s16(vs[n]) * n64_rsp_s16(vt[n]);
        n64_rsp_set_acc(&rsp->vu, n, (uint64_t)p << 16);
        vd[n] = n64_rsp_acc_sat(&rsp->vu, n, 1, 0x8000u, 0x7FFFu);
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMADL: acc += (vs * vt) >> 16 unsigned; result clamps the low slice
 * unsigned. */
N64_FORCEINLINE void n64_rsp_ref_vmadl(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        uint64_t p = ((uint32_t)vs[n] * (uint32_t)vt[n]) >> 16;
        n64_rsp_set_acc(&rsp->vu, n, n64_rsp_acc(&rsp->vu, n) + p);
        vd[n] = n64_rsp_acc_sat(&rsp->vu, n, 0, 0x0000u, 0xFFFFu);
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMADM: acc += signed vs * unsigned vt; result clamps the middle. */
N64_FORCEINLINE void n64_rsp_ref_vmadm(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t p = (int64_t)n64_rsp_s16(vs[n]) * vt[n];
        n64_rsp_set_acc(&rsp->vu, n, n64_rsp_acc(&rsp->vu, n) + (uint64_t)p);
        vd[n] = n64_rsp_acc_sat(&rsp->vu, n, 1, 0x8000u, 0x7FFFu);
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMADN: acc += unsigned vs * signed vt; result clamps the low slice
 * unsigned. */
N64_FORCEINLINE void n64_rsp_ref_vmadn(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t p = (int64_t)vs[n] * n64_rsp_s16(vt[n]);
        n64_rsp_set_acc(&rsp->vu, n, n64_rsp_acc(&rsp->vu, n) + (uint64_t)p);
        vd[n] = n64_rsp_acc_sat(&rsp->vu, n, 0, 0x0000u, 0xFFFFu);
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VMADH: acc += (vs * vt) << 16; result clamps the middle. */
N64_FORCEINLINE void n64_rsp_ref_vmadh(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int64_t p = (int64_t)n64_rsp_s16(vs[n]) * n64_rsp_s16(vt[n]);
        n64_rsp_set_acc(&rsp->vu, n, n64_rsp_acc(&rsp->vu, n) + ((uint64_t)p << 16));
        vd[n] = n64_rsp_acc_sat(&rsp->vu, n, 1, 0x8000u, 0x7FFFu);
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* Vector unit: add, subtract, select. */

/* VADD and VSUB fold in the VCO carry bits and clear VCO; the accumulator
 * keeps the unclamped low 16 bits. */
N64_FORCEINLINE void n64_rsp_ref_vadd(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int32_t r = n64_rsp_s16(vs[n]) + n64_rsp_s16(vt[n]) + (int32_t)n64_rsp_flag(rsp->vu.vco_lo, n);
        rsp->vu.acc_l[n] = (uint16_t)(uint32_t)r;
        vd[n] = n64_rsp_clamp16(r);
    }
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    n64_rsp_vstore(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_ref_vsub(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int32_t r = n64_rsp_s16(vs[n]) - n64_rsp_s16(vt[n]) - (int32_t)n64_rsp_flag(rsp->vu.vco_lo, n);
        rsp->vu.acc_l[n] = (uint16_t)(uint32_t)r;
        vd[n] = n64_rsp_clamp16(r);
    }
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    n64_rsp_vstore(rsp, op, vd);
}

/* VABS: vt negated, kept or zeroed by the sign of vs; -0x8000 gives
 * 0x7FFF in vd but 0x8000 in the accumulator. */
N64_FORCEINLINE void n64_rsp_ref_vabs(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int32_t s = n64_rsp_s16(vs[n]);
        if (s < 0) {
            if (vt[n] == 0x8000u) {
                rsp->vu.acc_l[n] = 0x8000u;
                vd[n] = 0x7FFFu;
            } else {
                rsp->vu.acc_l[n] = vd[n] = (uint16_t)(0u - vt[n]);
            }
        } else if (s > 0) {
            rsp->vu.acc_l[n] = vd[n] = vt[n];
        } else {
            rsp->vu.acc_l[n] = vd[n] = 0;
        }
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VADDC: unsigned add, carry out to VCO low; VCO high cleared. */
N64_FORCEINLINE void n64_rsp_ref_vaddc(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    uint16_t carry = 0;
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        uint32_t r = (uint32_t)vs[n] + vt[n];
        rsp->vu.acc_l[n] = vd[n] = (uint16_t)r;
        carry = n64_rsp_setflag(carry, n, r >> 16);
    }
    rsp->vu.vco_lo = carry;
    rsp->vu.vco_hi = 0;
    n64_rsp_vstore(rsp, op, vd);
}

/* VSUBC: unsigned subtract, borrow to VCO low, not-equal to VCO high. */
N64_FORCEINLINE void n64_rsp_ref_vsubc(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    uint16_t lo = 0, hi = 0;
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        uint32_t r = (uint32_t)vs[n] - vt[n];
        rsp->vu.acc_l[n] = vd[n] = (uint16_t)r;
        lo = n64_rsp_setflag(lo, n, (r >> 16) != 0);
        hi = n64_rsp_setflag(hi, n, r != 0);
    }
    rsp->vu.vco_lo = lo;
    rsp->vu.vco_hi = hi;
    n64_rsp_vstore(rsp, op, vd);
}

/* The unimplemented computational opcodes (VSUT, VADDB, VSUBB, VACCB,
 * VSUCB, VSAD, VSAC, VSUM, 0x1E-0x1F, 0x2E-0x2F, VEXTT, VEXTQ, VEXTN,
 * 0x3B, VINST, VINSQ, VINSN) still run the adder: the accumulator's low
 * slice gets vs + vt and vd is zeroed. Flags are untouched. */
N64_FORCEINLINE void n64_rsp_ref_vzero(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        rsp->vu.acc_l[n] = (uint16_t)((uint32_t)vs[n] + vt[n]);
        vd[n] = 0;
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* VSAR reads an accumulator slice: element 8 high, 9 middle, 10 low; any
 * other element reads zero. The accumulator is not written. */
N64_FORCEINLINE void n64_rsp_ref_vsar(N64Rsp *rsp, uint32_t op)
{
    uint16_t vd[8];
    unsigned n;
    for (n = 0; n < 8; n++) {
        switch (N64_RSP_VE(op)) {
        case 8: vd[n] = rsp->vu.acc_h[n]; break;
        case 9: vd[n] = rsp->vu.acc_m[n]; break;
        case 10: vd[n] = rsp->vu.acc_l[n]; break;
        default: vd[n] = 0; break;
        }
    }
    n64_rsp_vstore(rsp, op, vd);
}

/* The compares select vs where VCC low ends up set and vt elsewhere, clear
 * VCC high and VCO. On equal lanes, VCO decides: VLT takes vs when both
 * carry and not-equal were set, VGE when they were not both set, VEQ
 * requires not-equal clear, VNE takes vs when not-equal was set. */
N64_FORCEINLINE void n64_rsp_vcompare(N64Rsp *rsp, uint32_t op, unsigned kind)
{
    uint16_t vs[8], vt[8], vd[8];
    uint16_t cc = 0;
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int32_t s = n64_rsp_s16(vs[n]), t = n64_rsp_s16(vt[n]);
        unsigned co_lo = n64_rsp_flag(rsp->vu.vco_lo, n), co_hi = n64_rsp_flag(rsp->vu.vco_hi, n);
        unsigned sel;
        switch (kind) {
        case 0: sel = s < t || (s == t && co_lo && co_hi); break;      /* VLT */
        case 1: sel = !co_hi && s == t; break;                         /* VEQ */
        case 2: sel = s != t || co_hi; break;                          /* VNE */
        default: sel = s > t || (s == t && !(co_lo && co_hi)); break; /* VGE */
        }
        cc = n64_rsp_setflag(cc, n, sel);
        rsp->vu.acc_l[n] = vd[n] = sel ? vs[n] : vt[n];
    }
    rsp->vu.vcc_lo = cc;
    rsp->vu.vcc_hi = 0;
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    n64_rsp_vstore(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_ref_vlt(N64Rsp *rsp, uint32_t op) { n64_rsp_vcompare(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_ref_veq(N64Rsp *rsp, uint32_t op) { n64_rsp_vcompare(rsp, op, 1); }
N64_FORCEINLINE void n64_rsp_ref_vne(N64Rsp *rsp, uint32_t op) { n64_rsp_vcompare(rsp, op, 2); }
N64_FORCEINLINE void n64_rsp_ref_vge(N64Rsp *rsp, uint32_t op) { n64_rsp_vcompare(rsp, op, 3); }

/* VCH: first half of a double-precision clip test. Signs differing (VCO
 * low) compare vs against -vt, otherwise against vt; VCC low and high are
 * the <= and >= results, VCO high is "neither equal nor off by one", VCE
 * marks vs + vt == -1. */
N64_FORCEINLINE void n64_rsp_ref_vch(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    uint16_t cc_lo = 0, cc_hi = 0, co_lo = 0, co_hi = 0, ce = 0;
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int32_t s = n64_rsp_s16(vs[n]), t = n64_rsp_s16(vt[n]);
        unsigned ne_hi;
        if ((s < 0) != (t < 0)) {
            int32_t r = n64_rsp_s16((uint32_t)(s + t));
            unsigned le = r <= 0;
            rsp->vu.acc_l[n] = le ? (uint16_t)(0u - vt[n]) : vs[n];
            cc_lo = n64_rsp_setflag(cc_lo, n, le);
            cc_hi = n64_rsp_setflag(cc_hi, n, t < 0);
            co_lo = n64_rsp_setflag(co_lo, n, 1);
            ne_hi = r != 0 && vs[n] != (vt[n] ^ 0xFFFFu);
            ce = n64_rsp_setflag(ce, n, r == -1);
        } else {
            int32_t r = s - t;
            unsigned ge = r >= 0;
            rsp->vu.acc_l[n] = ge ? vt[n] : vs[n];
            cc_lo = n64_rsp_setflag(cc_lo, n, t < 0);
            cc_hi = n64_rsp_setflag(cc_hi, n, ge);
            ne_hi = r != 0 && vs[n] != (vt[n] ^ 0xFFFFu);
        }
        co_hi = n64_rsp_setflag(co_hi, n, ne_hi);
        vd[n] = rsp->vu.acc_l[n];
    }
    rsp->vu.vcc_lo = cc_lo;
    rsp->vu.vcc_hi = cc_hi;
    rsp->vu.vco_lo = co_lo;
    rsp->vu.vco_hi = co_hi;
    rsp->vu.vce = (uint8_t)ce;
    n64_rsp_vstore(rsp, op, vd);
}

/* VCL: second half of the clip test, on the low words, using the flags VCH
 * left. Lanes whose VCO high is set keep their previous VCC result. */
N64_FORCEINLINE void n64_rsp_ref_vcl(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    uint16_t cc_lo = rsp->vu.vcc_lo, cc_hi = rsp->vu.vcc_hi;
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        unsigned co_lo = n64_rsp_flag(rsp->vu.vco_lo, n), co_hi = n64_rsp_flag(rsp->vu.vco_hi, n);
        uint16_t r;
        if (co_lo) {
            if (co_hi) {
                r = n64_rsp_flag(cc_lo, n) ? (uint16_t)(0u - vt[n]) : vs[n];
            } else {
                uint32_t sum32 = (uint32_t)vs[n] + vt[n];
                unsigned sum0 = (sum32 & 0xFFFFu) == 0, carry = sum32 > 0xFFFFu;
                unsigned le = n64_rsp_flag(rsp->vu.vce, n) ? (sum0 || !carry) : (sum0 && !carry);
                cc_lo = n64_rsp_setflag(cc_lo, n, le);
                r = le ? (uint16_t)(0u - vt[n]) : vs[n];
            }
        } else {
            if (co_hi) {
                r = n64_rsp_flag(cc_hi, n) ? vt[n] : vs[n];
            } else {
                unsigned ge = vs[n] >= vt[n];
                cc_hi = n64_rsp_setflag(cc_hi, n, ge);
                r = ge ? vt[n] : vs[n];
            }
        }
        rsp->vu.acc_l[n] = vd[n] = r;
    }
    rsp->vu.vcc_lo = cc_lo;
    rsp->vu.vcc_hi = cc_hi;
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    rsp->vu.vce = 0;
    n64_rsp_vstore(rsp, op, vd);
}

/* VCR: single-precision clip test with one's-complement negation. */
N64_FORCEINLINE void n64_rsp_ref_vcr(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    uint16_t cc_lo = 0, cc_hi = 0;
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        int32_t s = n64_rsp_s16(vs[n]), t = n64_rsp_s16(vt[n]);
        if ((s < 0) != (t < 0)) {
            unsigned le = s + t + 1 <= 0;
            cc_hi = n64_rsp_setflag(cc_hi, n, t < 0);
            cc_lo = n64_rsp_setflag(cc_lo, n, le);
            vd[n] = le ? (uint16_t)~vt[n] : vs[n];
        } else {
            unsigned ge = s - t >= 0;
            cc_lo = n64_rsp_setflag(cc_lo, n, t < 0);
            cc_hi = n64_rsp_setflag(cc_hi, n, ge);
            vd[n] = ge ? vt[n] : vs[n];
        }
        rsp->vu.acc_l[n] = vd[n];
    }
    rsp->vu.vcc_lo = cc_lo;
    rsp->vu.vcc_hi = cc_hi;
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    rsp->vu.vce = 0;
    n64_rsp_vstore(rsp, op, vd);
}

/* VMRG: vs where VCC low is set, vt elsewhere; clears VCO. */
N64_FORCEINLINE void n64_rsp_ref_vmrg(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) rsp->vu.acc_l[n] = vd[n] = n64_rsp_flag(rsp->vu.vcc_lo, n) ? vs[n] : vt[n];
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    n64_rsp_vstore(rsp, op, vd);
}

/* The logical ops write their result to the accumulator's low slice too.
 * kind: 0 AND, 1 NAND, 2 OR, 3 NOR, 4 XOR, 5 NXOR. */
N64_FORCEINLINE void n64_rsp_vlogic(N64Rsp *rsp, uint32_t op, unsigned kind)
{
    uint16_t vs[8], vt[8], vd[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) {
        uint32_t r;
        switch (kind >> 1) {
        case 0: r = (uint32_t)vs[n] & vt[n]; break;
        case 1: r = (uint32_t)vs[n] | vt[n]; break;
        default: r = (uint32_t)vs[n] ^ vt[n]; break;
        }
        if (kind & 1u) r = ~r;
        rsp->vu.acc_l[n] = vd[n] = (uint16_t)r;
    }
    n64_rsp_vstore(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_ref_vand(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_ref_vnand(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic(rsp, op, 1); }
N64_FORCEINLINE void n64_rsp_ref_vor(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic(rsp, op, 2); }
N64_FORCEINLINE void n64_rsp_ref_vnor(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic(rsp, op, 3); }
N64_FORCEINLINE void n64_rsp_ref_vxor(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic(rsp, op, 4); }
N64_FORCEINLINE void n64_rsp_ref_vnxor(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic(rsp, op, 5); }

/* Vector unit: divide unit and moves. */

/* VRCP/VRCPL and VRSQ/VRSQL. The input is lane (e & 7) of vt, sign-
 * extended, or for the L forms after a VRCPH/VRSQH the 32-bit value
 * div_in:lane. Negative inputs are made positive (one's complement below
 * -32768), normalised, looked up in the ROM and shifted back; 0 gives
 * 0x7FFFFFFF and -32768 gives 0xFFFF0000. The low half goes to vd lane de,
 * the high half to div_out; the accumulator's low slice gets vt with the
 * element selection applied. n64_rsp_divide is the divide unit itself: it
 * takes the lane and returns the 32-bit result. */
N64_FORCEINLINE uint32_t n64_rsp_divide(N64RspVu *vu, uint32_t lane, int is_low, int is_rsq)
{
    int32_t input;
    uint32_t mask, data, result;
    if (is_low && vu->div_dp)
        input = n64_rsp_s32(((uint32_t)(uint16_t)vu->div_in << 16) | lane);
    else
        input = n64_rsp_s16(lane);
    mask = input < 0 ? 0xFFFFFFFFu : 0u;
    data = (uint32_t)input ^ mask;
    if (input > -32768) data -= mask;
    if (data == 0) {
        result = 0x7FFFFFFFu;
    } else if (input == -32768) {
        result = 0xFFFF0000u;
    } else {
        unsigned shift = n64_rsp_clz32(data);
        uint32_t index = (uint32_t)((((uint64_t)data << shift) & 0x7FC00000u) >> 22);
        if (is_rsq) {
            result = (0x10000u | n64_rsp_rsq_table[(index & 0x1FEu) | (shift & 1u)]) << 14;
            result = (result >> ((31u - shift) >> 1)) ^ mask;
        } else {
            result = (0x10000u | n64_rsp_rcp_table[index]) << 14;
            result = (result >> (31u - shift)) ^ mask;
        }
    }
    vu->div_dp = 0;
    vu->div_out = n64_rsp_i16(result >> 16);
    return result;
}

N64_FORCEINLINE void n64_rsp_vdivide(N64Rsp *rsp, uint32_t op, int is_low, int is_rsq)
{
    uint16_t vs[8], vt[8];
    unsigned n;
    uint32_t result;
    n64_rsp_vops(rsp, op, vs, vt);
    result = n64_rsp_divide(&rsp->vu, rsp->vu.v[N64_RSP_VT(op)][N64_RSP_VE(op) & 7u], is_low, is_rsq);
    for (n = 0; n < 8; n++) rsp->vu.acc_l[n] = vt[n];
    rsp->vu.v[N64_RSP_VD(op)][N64_RSP_VDE(op)] = (uint16_t)result;
}

N64_FORCEINLINE void n64_rsp_ref_vrcp(N64Rsp *rsp, uint32_t op) { n64_rsp_vdivide(rsp, op, 0, 0); }
N64_FORCEINLINE void n64_rsp_ref_vrcpl(N64Rsp *rsp, uint32_t op) { n64_rsp_vdivide(rsp, op, 1, 0); }
N64_FORCEINLINE void n64_rsp_ref_vrsq(N64Rsp *rsp, uint32_t op) { n64_rsp_vdivide(rsp, op, 0, 1); }
N64_FORCEINLINE void n64_rsp_ref_vrsql(N64Rsp *rsp, uint32_t op) { n64_rsp_vdivide(rsp, op, 1, 1); }

/* VRCPH/VRSQH latch the high half of a 32-bit input and return the high
 * half of the previous result. */
N64_FORCEINLINE void n64_rsp_ref_vrcph(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) rsp->vu.acc_l[n] = vt[n];
    rsp->vu.div_dp = 1;
    rsp->vu.div_in = n64_rsp_i16(rsp->vu.v[N64_RSP_VT(op)][N64_RSP_VE(op) & 7u]);
    rsp->vu.v[N64_RSP_VD(op)][N64_RSP_VDE(op)] = (uint16_t)rsp->vu.div_out;
}

N64_FORCEINLINE void n64_rsp_ref_vrsqh(N64Rsp *rsp, uint32_t op) { n64_rsp_ref_vrcph(rsp, op); }

/* VMOV copies lane de of vt (after element selection) to lane de of vd. */
N64_FORCEINLINE void n64_rsp_ref_vmov(N64Rsp *rsp, uint32_t op)
{
    uint16_t vs[8], vt[8];
    unsigned n;
    n64_rsp_vops(rsp, op, vs, vt);
    for (n = 0; n < 8; n++) rsp->vu.acc_l[n] = vt[n];
    rsp->vu.v[N64_RSP_VD(op)][N64_RSP_VDE(op)] = vt[N64_RSP_VDE(op)];
}

N64_FORCEINLINE void n64_rsp_ref_vnop(N64Rsp *rsp, uint32_t op)
{
    (void)rsp;
    (void)op;
}

#ifdef __cplusplus
}
#endif

#include "n64_rsp_fast.h"

#endif /* N64_RSP_IMPL_H */
