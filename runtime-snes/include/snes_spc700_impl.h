/*
 * SPC700 instruction set, as a template over the bus.
 *
 * Ported from ares (ISC license, Near et al.),
 * ares/component/processor/spc700/{instruction,instructions,algorithms,
 * memory}.cpp at commit af4cbb04f067682a8a3cf42695ff78bed634b38d:
 * the per-instruction bus sequences (reads, dummy reads, idle cycles) and
 * the ALU algorithms, including the DIV overflow behaviour. Rewritten in
 * C with explicit unsigned arithmetic.
 *
 * Include this file once per translation unit after defining:
 *
 *   SPC_FN                 name of the function to define:
 *                            static void SPC_FN(SnesSpc700 *c, SPC_CTX ctx)
 *   SPC_CTX                the bus context type
 *   SPC_READ(ctx, addr)    one bus read cycle, returns uint8_t
 *   SPC_WRITE(ctx, addr, v) one bus write cycle
 *   SPC_IDLE(ctx)          one internal cycle
 *
 * SPC_FN runs one instruction. When the core is halted (SLEEP/STOP) it
 * runs one halted iteration instead: a dummy read of PC and an idle cycle,
 * as the hardware keeps clocking in that state.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#ifndef SPC_FN
#error "define SPC_FN, SPC_CTX, SPC_READ, SPC_WRITE and SPC_IDLE before including snes_spc700_impl.h"
#endif

#include "snes_spc700.h"

#ifndef SNES_SPC700_ALU_DEFINED
#define SNES_SPC700_ALU_DEFINED

static inline uint8_t s7_nz(SnesSpc700 *c, uint8_t v)
{
    c->fz = (uint8_t)(v == 0);
    c->fn = (uint8_t)(v >> 7);
    return v;
}

static inline uint8_t s7_or(SnesSpc700 *c, uint8_t x, uint8_t y) { return s7_nz(c, (uint8_t)(x | y)); }
static inline uint8_t s7_and(SnesSpc700 *c, uint8_t x, uint8_t y) { return s7_nz(c, (uint8_t)(x & y)); }
static inline uint8_t s7_eor(SnesSpc700 *c, uint8_t x, uint8_t y) { return s7_nz(c, (uint8_t)(x ^ y)); }

static inline uint8_t s7_ld(SnesSpc700 *c, uint8_t x, uint8_t y)
{
    (void)x;
    return s7_nz(c, y);
}

static inline uint8_t s7_cmp(SnesSpc700 *c, uint8_t x, uint8_t y)
{
    uint32_t z = (uint32_t)x - (uint32_t)y;
    c->fc = (uint8_t)(x >= y);
    c->fz = (uint8_t)((z & 0xFFu) == 0);
    c->fn = (uint8_t)((z >> 7) & 1u);
    return x;
}

static inline uint8_t s7_adc(SnesSpc700 *c, uint8_t x, uint8_t y)
{
    uint32_t z = (uint32_t)x + (uint32_t)y + (uint32_t)c->fc;
    c->fc = (uint8_t)(z > 0xFFu);
    c->fh = (uint8_t)((((uint32_t)x ^ y ^ z) >> 4) & 1u);
    c->fv = (uint8_t)(((~((uint32_t)x ^ y) & ((uint32_t)x ^ z)) >> 7) & 1u);
    return s7_nz(c, (uint8_t)z);
}

static inline uint8_t s7_sbc(SnesSpc700 *c, uint8_t x, uint8_t y) { return s7_adc(c, x, (uint8_t)~y); }

static inline uint8_t s7_asl(SnesSpc700 *c, uint8_t x)
{
    c->fc = (uint8_t)(x >> 7);
    return s7_nz(c, (uint8_t)(x << 1));
}

static inline uint8_t s7_rol(SnesSpc700 *c, uint8_t x)
{
    uint8_t carry = c->fc;
    c->fc = (uint8_t)(x >> 7);
    return s7_nz(c, (uint8_t)((x << 1) | carry));
}

static inline uint8_t s7_lsr(SnesSpc700 *c, uint8_t x)
{
    c->fc = (uint8_t)(x & 1u);
    return s7_nz(c, (uint8_t)(x >> 1));
}

static inline uint8_t s7_ror(SnesSpc700 *c, uint8_t x)
{
    uint8_t carry = c->fc;
    c->fc = (uint8_t)(x & 1u);
    return s7_nz(c, (uint8_t)((carry << 7) | (x >> 1)));
}

static inline uint8_t s7_inc(SnesSpc700 *c, uint8_t x) { return s7_nz(c, (uint8_t)(x + 1u)); }
static inline uint8_t s7_dec(SnesSpc700 *c, uint8_t x) { return s7_nz(c, (uint8_t)(x - 1u)); }

/* 16-bit YA operations (ADDW, SUBW, CMPW, MOVW YA,dp). */
static inline uint16_t s7_adw(SnesSpc700 *c, uint16_t x, uint16_t y)
{
    c->fc = 0;
    uint16_t lo = s7_adc(c, (uint8_t)x, (uint8_t)y);
    uint16_t hi = s7_adc(c, (uint8_t)(x >> 8), (uint8_t)(y >> 8));
    uint16_t z = (uint16_t)(lo | (hi << 8));
    c->fz = (uint8_t)(z == 0);
    return z;
}

static inline uint16_t s7_sbw(SnesSpc700 *c, uint16_t x, uint16_t y)
{
    c->fc = 1;
    uint16_t lo = s7_sbc(c, (uint8_t)x, (uint8_t)y);
    uint16_t hi = s7_sbc(c, (uint8_t)(x >> 8), (uint8_t)(y >> 8));
    uint16_t z = (uint16_t)(lo | (hi << 8));
    c->fz = (uint8_t)(z == 0);
    return z;
}

static inline void s7_cpw(SnesSpc700 *c, uint16_t x, uint16_t y)
{
    uint32_t z = (uint32_t)x - (uint32_t)y;
    c->fc = (uint8_t)(x >= y);
    c->fz = (uint8_t)((z & 0xFFFFu) == 0);
    c->fn = (uint8_t)((z >> 15) & 1u);
}

static inline void s7_ldw(SnesSpc700 *c, uint16_t y)
{
    c->fz = (uint8_t)(y == 0);
    c->fn = (uint8_t)(y >> 15);
}

/* PC + signed 8-bit displacement, modulo 64 KiB. */
static inline uint16_t s7_rel(uint16_t pc, uint8_t d)
{
    return (uint16_t)((uint32_t)pc + d - ((uint32_t)(d & 0x80u) << 1));
}

#endif /* SNES_SPC700_ALU_DEFINED */

/* Bus helpers (per instantiation). */
#define S7_RD(addr) SPC_READ(ctx, (uint16_t)(addr))
#define S7_WR(addr, v) SPC_WRITE(ctx, (uint16_t)(addr), (uint8_t)(v))
#define S7_IDLE() SPC_IDLE(ctx)
#define S7_FETCH() S7_RD(c->pc++)
#define S7_DP(addr) ((uint16_t)(((uint32_t)c->fp << 8) | (uint8_t)(addr)))
#define S7_LOAD(addr) S7_RD(S7_DP(addr))
#define S7_STORE(addr, v) S7_WR(S7_DP(addr), v)
#define S7_PUSH(v)                               \
    do {                                         \
        S7_WR(0x100u | c->sp, (v));              \
        c->sp = (uint8_t)(c->sp - 1u);           \
    } while (0)
#define S7_ABS(var)                              \
    uint16_t var = S7_FETCH();                   \
    var = (uint16_t)(var | (S7_FETCH() << 8))

/* ---- Addressing-mode groups ------------------------------------------ */

/* A = F(A, operand) for the eight read modes of an ALU column. */
#define S7_ALU_READS(b, F)                                                   \
    case (b) + 0x04: { /* dp */                                              \
        uint8_t ad = S7_FETCH();                                             \
        uint8_t d = S7_LOAD(ad);                                             \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x05: { /* !abs */                                            \
        S7_ABS(ad);                                                          \
        uint8_t d = S7_RD(ad);                                               \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x06: { /* (X) */                                             \
        S7_RD(c->pc);                                                        \
        uint8_t d = S7_LOAD(c->x);                                           \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x07: { /* [dp+X] */                                          \
        uint8_t ind = S7_FETCH();                                            \
        S7_IDLE();                                                           \
        uint16_t ad = S7_LOAD(ind + c->x);                                   \
        ad = (uint16_t)(ad | (S7_LOAD(ind + c->x + 1u) << 8));               \
        uint8_t d = S7_RD(ad);                                               \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x08: { /* #imm */                                            \
        uint8_t d = S7_FETCH();                                              \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x14: { /* dp+X */                                            \
        uint8_t ad = S7_FETCH();                                             \
        S7_IDLE();                                                           \
        uint8_t d = S7_LOAD(ad + c->x);                                      \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x15: { /* !abs+X */                                          \
        S7_ABS(ad);                                                          \
        S7_IDLE();                                                           \
        uint8_t d = S7_RD(ad + c->x);                                        \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x16: { /* !abs+Y */                                          \
        S7_ABS(ad);                                                          \
        S7_IDLE();                                                           \
        uint8_t d = S7_RD(ad + c->y);                                        \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x17: { /* [dp]+Y */                                          \
        uint8_t ind = S7_FETCH();                                            \
        S7_IDLE();                                                           \
        uint16_t ad = S7_LOAD(ind);                                          \
        ad = (uint16_t)(ad | (S7_LOAD(ind + 1u) << 8));                      \
        uint8_t d = S7_RD(ad + c->y);                                        \
        c->a = F(c, c->a, d);                                                \
        break;                                                               \
    }

/* Memory-to-memory forms: dp,dp / dp,#imm / (X),(Y). Compare variants end
 * with an idle cycle instead of the write. */
#define S7_ALU_MODS(b, F, IS_CMP)                                            \
    case (b) + 0x09: { /* dp,dp */                                           \
        uint8_t src = S7_FETCH();                                            \
        uint8_t rhs = S7_LOAD(src);                                          \
        uint8_t dst = S7_FETCH();                                            \
        uint8_t lhs = S7_LOAD(dst);                                          \
        lhs = F(c, lhs, rhs);                                                \
        if (IS_CMP) S7_IDLE();                                               \
        else S7_STORE(dst, lhs);                                             \
        break;                                                               \
    }                                                                        \
    case (b) + 0x18: { /* dp,#imm */                                         \
        uint8_t imm = S7_FETCH();                                            \
        uint8_t ad = S7_FETCH();                                             \
        uint8_t d = S7_LOAD(ad);                                             \
        d = F(c, d, imm);                                                    \
        if (IS_CMP) S7_IDLE();                                               \
        else S7_STORE(ad, d);                                                \
        break;                                                               \
    }                                                                        \
    case (b) + 0x19: { /* (X),(Y) */                                         \
        S7_RD(c->pc);                                                        \
        uint8_t rhs = S7_LOAD(c->y);                                         \
        uint8_t lhs = S7_LOAD(c->x);                                         \
        lhs = F(c, lhs, rhs);                                                \
        if (IS_CMP) S7_IDLE();                                               \
        else S7_STORE(c->x, lhs);                                            \
        break;                                                               \
    }

/* Shift/increment columns: dp, dp+X, !abs, and the register form. */
#define S7_RMW_DP(op, F)                                                     \
    case (op): {                                                             \
        uint8_t ad = S7_FETCH();                                             \
        uint8_t d = S7_LOAD(ad);                                             \
        S7_STORE(ad, F(c, d));                                               \
        break;                                                               \
    }
#define S7_RMW_DPX(op, F)                                                    \
    case (op): {                                                             \
        uint8_t ad = S7_FETCH();                                             \
        S7_IDLE();                                                           \
        uint8_t d = S7_LOAD(ad + c->x);                                      \
        S7_STORE(ad + c->x, F(c, d));                                        \
        break;                                                               \
    }
#define S7_RMW_ABS(op, F)                                                    \
    case (op): {                                                             \
        S7_ABS(ad);                                                          \
        uint8_t d = S7_RD(ad);                                               \
        S7_WR(ad, F(c, d));                                                  \
        break;                                                               \
    }
#define S7_RMW_REG(op, F, R)                                                 \
    case (op): {                                                             \
        S7_RD(c->pc);                                                        \
        c->R = F(c, c->R);                                                   \
        break;                                                               \
    }

/* Register loads/compares with dp, !abs and #imm operands. */
#define S7_REG_DP(op, F, R)                                                  \
    case (op): {                                                             \
        uint8_t ad = S7_FETCH();                                             \
        uint8_t d = S7_LOAD(ad);                                             \
        c->R = F(c, c->R, d);                                                \
        break;                                                               \
    }
#define S7_REG_ABS(op, F, R)                                                 \
    case (op): {                                                             \
        S7_ABS(ad);                                                          \
        uint8_t d = S7_RD(ad);                                               \
        c->R = F(c, c->R, d);                                                \
        break;                                                               \
    }
#define S7_REG_IMM(op, F, R)                                                 \
    case (op): {                                                             \
        uint8_t d = S7_FETCH();                                              \
        c->R = F(c, c->R, d);                                                \
        break;                                                               \
    }
/* dp+index read into a register. */
#define S7_REG_DPI(op, F, R, I)                                              \
    case (op): {                                                             \
        uint8_t ad = S7_FETCH();                                             \
        S7_IDLE();                                                           \
        uint8_t d = S7_LOAD(ad + c->I);                                      \
        c->R = F(c, c->R, d);                                                \
        break;                                                               \
    }

/* Stores (a dummy read of the target precedes the write). */
#define S7_ST_DP(op, R)                                                      \
    case (op): {                                                             \
        uint8_t ad = S7_FETCH();                                             \
        S7_LOAD(ad);                                                         \
        S7_STORE(ad, c->R);                                                  \
        break;                                                               \
    }
#define S7_ST_ABS(op, R)                                                     \
    case (op): {                                                             \
        S7_ABS(ad);                                                          \
        S7_RD(ad);                                                           \
        S7_WR(ad, c->R);                                                     \
        break;                                                               \
    }
#define S7_ST_DPI(op, R, I)                                                  \
    case (op): {                                                             \
        uint8_t ad = S7_FETCH();                                             \
        S7_IDLE();                                                           \
        S7_LOAD(ad + c->I);                                                  \
        S7_STORE(ad + c->I, c->R);                                           \
        break;                                                               \
    }
#define S7_ST_ABSI(op, I)                                                    \
    case (op): {                                                             \
        S7_ABS(ad);                                                          \
        S7_IDLE();                                                           \
        S7_RD(ad + c->I);                                                    \
        S7_WR(ad + c->I, c->a);                                              \
        break;                                                               \
    }

#define S7_BRANCH(op, COND)                                                  \
    case (op): {                                                             \
        uint8_t d = S7_FETCH();                                              \
        if (COND) {                                                          \
            S7_IDLE();                                                       \
            S7_IDLE();                                                       \
            c->pc = s7_rel(c->pc, d);                                        \
        }                                                                    \
        break;                                                               \
    }

/* TCALL n: vectors at $FFDE - 2n. */
#define S7_TCALL(n)                                                          \
    case ((n) << 4) | 0x01: {                                                \
        S7_RD(c->pc);                                                        \
        S7_IDLE();                                                           \
        S7_PUSH(c->pc >> 8);                                                 \
        S7_PUSH(c->pc & 0xFFu);                                              \
        S7_IDLE();                                                           \
        uint16_t v = (uint16_t)(0xFFDEu - ((n) << 1));                       \
        uint16_t t = S7_RD(v);                                               \
        t = (uint16_t)(t | (S7_RD(v + 1u) << 8));                            \
        c->pc = t;                                                           \
        break;                                                               \
    }

/* SET1/CLR1 dp.bit (opcodes x2), BBS/BBC dp.bit,rel (opcodes x3). */
#define S7_BITOPS(bit)                                                       \
    case ((bit) << 5) | 0x02:                                                \
    case ((bit) << 5) | 0x12: {                                              \
        uint8_t ad = S7_FETCH();                                             \
        uint8_t d = S7_LOAD(ad);                                             \
        if (op & 0x10u) d = (uint8_t)(d & ~(1u << (bit)));                   \
        else d = (uint8_t)(d | (1u << (bit)));                               \
        S7_STORE(ad, d);                                                     \
        break;                                                               \
    }                                                                        \
    case ((bit) << 5) | 0x03:                                                \
    case ((bit) << 5) | 0x13: {                                              \
        uint8_t ad = S7_FETCH();                                             \
        uint8_t d = S7_LOAD(ad);                                             \
        S7_IDLE();                                                           \
        uint8_t disp = S7_FETCH();                                           \
        unsigned set = (d >> (bit)) & 1u;                                    \
        unsigned want = (op & 0x10u) ? 0u : 1u;                              \
        if (set == want) {                                                   \
            S7_IDLE();                                                       \
            S7_IDLE();                                                       \
            c->pc = s7_rel(c->pc, disp);                                     \
        }                                                                    \
        break;                                                               \
    }

/* mem.bit operand of the OR1/AND1/EOR1/MOV1/NOT1 family: 13-bit address,
 * bit number in the top three bits. */
#define S7_MBIT(ad, bit)                                                     \
    S7_ABS(ad##_raw);                                                        \
    unsigned bit = (unsigned)(ad##_raw >> 13);                               \
    uint16_t ad = (uint16_t)(ad##_raw & 0x1FFFu)

/* One instantiation per translation unit, so a fixed static name is safe. */
static inline uint8_t s7_pull_(SnesSpc700 *c, SPC_CTX ctx)
{
    c->sp = (uint8_t)(c->sp + 1u);
    return S7_RD(0x100u | c->sp);
}
#define S7_PULL() s7_pull_(c, ctx)

static void SPC_FN(SnesSpc700 *c, SPC_CTX ctx)
{
    if (c->halt) {
        S7_RD(c->pc);
        S7_IDLE();
        return;
    }
    const uint8_t op = S7_FETCH();
    switch (op) {
    /* ---- ALU columns ---- */
    S7_ALU_READS(0x00, s7_or)
    S7_ALU_MODS(0x00, s7_or, 0)
    S7_ALU_READS(0x20, s7_and)
    S7_ALU_MODS(0x20, s7_and, 0)
    S7_ALU_READS(0x40, s7_eor)
    S7_ALU_MODS(0x40, s7_eor, 0)
    S7_ALU_READS(0x60, s7_cmp)
    S7_ALU_MODS(0x60, s7_cmp, 1)
    S7_ALU_READS(0x80, s7_adc)
    S7_ALU_MODS(0x80, s7_adc, 0)
    S7_ALU_READS(0xA0, s7_sbc)
    S7_ALU_MODS(0xA0, s7_sbc, 0)
    S7_ALU_READS(0xE0, s7_ld)

    /* ---- shifts, INC/DEC ---- */
    S7_RMW_DP(0x0B, s7_asl)
    S7_RMW_DPX(0x1B, s7_asl)
    S7_RMW_ABS(0x0C, s7_asl)
    S7_RMW_REG(0x1C, s7_asl, a)
    S7_RMW_DP(0x2B, s7_rol)
    S7_RMW_DPX(0x3B, s7_rol)
    S7_RMW_ABS(0x2C, s7_rol)
    S7_RMW_REG(0x3C, s7_rol, a)
    S7_RMW_DP(0x4B, s7_lsr)
    S7_RMW_DPX(0x5B, s7_lsr)
    S7_RMW_ABS(0x4C, s7_lsr)
    S7_RMW_REG(0x5C, s7_lsr, a)
    S7_RMW_DP(0x6B, s7_ror)
    S7_RMW_DPX(0x7B, s7_ror)
    S7_RMW_ABS(0x6C, s7_ror)
    S7_RMW_REG(0x7C, s7_ror, a)
    S7_RMW_DP(0x8B, s7_dec)
    S7_RMW_DPX(0x9B, s7_dec)
    S7_RMW_ABS(0x8C, s7_dec)
    S7_RMW_REG(0x9C, s7_dec, a)
    S7_RMW_DP(0xAB, s7_inc)
    S7_RMW_DPX(0xBB, s7_inc)
    S7_RMW_ABS(0xAC, s7_inc)
    S7_RMW_REG(0xBC, s7_inc, a)
    S7_RMW_REG(0x1D, s7_dec, x)
    S7_RMW_REG(0x3D, s7_inc, x)
    S7_RMW_REG(0xDC, s7_dec, y)
    S7_RMW_REG(0xFC, s7_inc, y)

    /* ---- X/Y loads and compares ---- */
    S7_REG_ABS(0x1E, s7_cmp, x)
    S7_REG_DP(0x3E, s7_cmp, x)
    S7_REG_IMM(0xC8, s7_cmp, x)
    S7_REG_ABS(0x5E, s7_cmp, y)
    S7_REG_DP(0x7E, s7_cmp, y)
    S7_REG_IMM(0xAD, s7_cmp, y)
    S7_REG_IMM(0x8D, s7_ld, y)
    S7_REG_IMM(0xCD, s7_ld, x)
    S7_REG_ABS(0xE9, s7_ld, x)
    S7_REG_DP(0xEB, s7_ld, y)
    S7_REG_ABS(0xEC, s7_ld, y)
    S7_REG_DP(0xF8, s7_ld, x)
    S7_REG_DPI(0xF9, s7_ld, x, y)
    S7_REG_DPI(0xFB, s7_ld, y, x)

    /* ---- stores ---- */
    S7_ST_DP(0xC4, a)
    S7_ST_DP(0xCB, y)
    S7_ST_DP(0xD8, x)
    S7_ST_ABS(0xC5, a)
    S7_ST_ABS(0xC9, x)
    S7_ST_ABS(0xCC, y)
    S7_ST_DPI(0xD4, a, x)
    S7_ST_DPI(0xD9, x, y)
    S7_ST_DPI(0xDB, y, x)
    S7_ST_ABSI(0xD5, x)
    S7_ST_ABSI(0xD6, y)
    case 0xC6: { /* MOV (X),A */
        S7_RD(c->pc);
        S7_LOAD(c->x);
        S7_STORE(c->x, c->a);
        break;
    }
    case 0xC7: { /* MOV [dp+X],A */
        uint8_t ind = S7_FETCH();
        S7_IDLE();
        uint16_t ad = S7_LOAD(ind + c->x);
        ad = (uint16_t)(ad | (S7_LOAD(ind + c->x + 1u) << 8));
        S7_RD(ad);
        S7_WR(ad, c->a);
        break;
    }
    case 0xD7: { /* MOV [dp]+Y,A */
        uint8_t ind = S7_FETCH();
        uint16_t ad = S7_LOAD(ind);
        ad = (uint16_t)(ad | (S7_LOAD(ind + 1u) << 8));
        S7_IDLE();
        S7_RD(ad + c->y);
        S7_WR(ad + c->y, c->a);
        break;
    }
    case 0x8F: { /* MOV dp,#imm */
        uint8_t imm = S7_FETCH();
        uint8_t ad = S7_FETCH();
        S7_LOAD(ad);
        S7_STORE(ad, imm);
        break;
    }
    case 0xFA: { /* MOV dp,dp */
        uint8_t src = S7_FETCH();
        uint8_t d = S7_LOAD(src);
        uint8_t dst = S7_FETCH();
        S7_STORE(dst, d);
        break;
    }
    case 0xAF: { /* MOV (X)+,A: an idle cycle, not a dummy read */
        S7_RD(c->pc);
        S7_IDLE();
        S7_STORE(c->x, c->a);
        c->x = (uint8_t)(c->x + 1u);
        break;
    }
    case 0xBF: { /* MOV A,(X)+ */
        S7_RD(c->pc);
        uint8_t d = S7_LOAD(c->x);
        c->x = (uint8_t)(c->x + 1u);
        S7_IDLE();
        c->a = s7_nz(c, d);
        break;
    }

    /* ---- register transfers ---- */
    case 0x5D: S7_RD(c->pc); c->x = s7_nz(c, c->a); break;  /* MOV X,A */
    case 0x7D: S7_RD(c->pc); c->a = s7_nz(c, c->x); break;  /* MOV A,X */
    case 0xDD: S7_RD(c->pc); c->a = s7_nz(c, c->y); break;  /* MOV A,Y */
    case 0xFD: S7_RD(c->pc); c->y = s7_nz(c, c->a); break;  /* MOV Y,A */
    case 0x9D: S7_RD(c->pc); c->x = s7_nz(c, c->sp); break; /* MOV X,SP */
    case 0xBD: S7_RD(c->pc); c->sp = c->x; break;           /* MOV SP,X (no flags) */

    /* ---- 16-bit ---- */
    case 0x1A:   /* DECW dp */
    case 0x3A: { /* INCW dp */
        uint8_t ad = S7_FETCH();
        uint32_t d = (uint32_t)S7_LOAD(ad) + (op == 0x3A ? 1u : 0xFFFFu);
        d &= 0xFFFFu;
        S7_STORE(ad, d & 0xFFu);
        d = (d + ((uint32_t)S7_LOAD(ad + 1u) << 8)) & 0xFFFFu;
        S7_STORE(ad + 1u, d >> 8);
        c->fz = (uint8_t)(d == 0);
        c->fn = (uint8_t)((d >> 15) & 1u);
        break;
    }
    case 0x5A: { /* CMPW YA,dp */
        uint8_t ad = S7_FETCH();
        uint16_t d = S7_LOAD(ad);
        d = (uint16_t)(d | (S7_LOAD(ad + 1u) << 8));
        s7_cpw(c, (uint16_t)(c->a | (c->y << 8)), d);
        break;
    }
    case 0x7A:   /* ADDW YA,dp */
    case 0x9A:   /* SUBW YA,dp */
    case 0xBA: { /* MOVW YA,dp */
        uint8_t ad = S7_FETCH();
        uint16_t d = S7_LOAD(ad);
        S7_IDLE();
        d = (uint16_t)(d | (S7_LOAD(ad + 1u) << 8));
        uint16_t ya = (uint16_t)(c->a | (c->y << 8));
        if (op == 0x7A) ya = s7_adw(c, ya, d);
        else if (op == 0x9A) ya = s7_sbw(c, ya, d);
        else {
            s7_ldw(c, d);
            ya = d;
        }
        c->a = (uint8_t)ya;
        c->y = (uint8_t)(ya >> 8);
        break;
    }
    case 0xDA: { /* MOVW dp,YA */
        uint8_t ad = S7_FETCH();
        S7_LOAD(ad);
        S7_STORE(ad, c->a);
        S7_STORE(ad + 1u, c->y);
        break;
    }

    /* ---- mem.bit carry operations ---- */
    case 0x0A: { /* OR1 C,mem.bit */
        S7_MBIT(ad, bit);
        uint8_t d = S7_RD(ad);
        S7_IDLE();
        c->fc = (uint8_t)(c->fc | ((d >> bit) & 1u));
        break;
    }
    case 0x2A: { /* OR1 C,/mem.bit */
        S7_MBIT(ad, bit);
        uint8_t d = S7_RD(ad);
        S7_IDLE();
        c->fc = (uint8_t)(c->fc | (((d >> bit) & 1u) ^ 1u));
        break;
    }
    case 0x4A: { /* AND1 C,mem.bit */
        S7_MBIT(ad, bit);
        uint8_t d = S7_RD(ad);
        c->fc = (uint8_t)(c->fc & ((d >> bit) & 1u));
        break;
    }
    case 0x6A: { /* AND1 C,/mem.bit */
        S7_MBIT(ad, bit);
        uint8_t d = S7_RD(ad);
        c->fc = (uint8_t)(c->fc & (((d >> bit) & 1u) ^ 1u));
        break;
    }
    case 0x8A: { /* EOR1 C,mem.bit */
        S7_MBIT(ad, bit);
        uint8_t d = S7_RD(ad);
        S7_IDLE();
        c->fc = (uint8_t)(c->fc ^ ((d >> bit) & 1u));
        break;
    }
    case 0xAA: { /* MOV1 C,mem.bit */
        S7_MBIT(ad, bit);
        uint8_t d = S7_RD(ad);
        c->fc = (uint8_t)((d >> bit) & 1u);
        break;
    }
    case 0xCA: { /* MOV1 mem.bit,C */
        S7_MBIT(ad, bit);
        uint8_t d = S7_RD(ad);
        S7_IDLE();
        d = (uint8_t)((d & ~(1u << bit)) | ((unsigned)c->fc << bit));
        S7_WR(ad, d);
        break;
    }
    case 0xEA: { /* NOT1 mem.bit */
        S7_MBIT(ad, bit);
        uint8_t d = S7_RD(ad);
        d = (uint8_t)(d ^ (1u << bit));
        S7_WR(ad, d);
        break;
    }

    /* ---- TSET1 / TCLR1 !abs ---- */
    case 0x0E:
    case 0x4E: {
        S7_ABS(ad);
        uint8_t d = S7_RD(ad);
        uint8_t t = (uint8_t)(c->a - d);
        c->fz = (uint8_t)(t == 0);
        c->fn = (uint8_t)(t >> 7);
        S7_RD(ad);
        S7_WR(ad, op == 0x0E ? (uint8_t)(d | c->a) : (uint8_t)(d & ~c->a));
        break;
    }

    /* ---- bit ops, TCALL ---- */
    S7_BITOPS(0)
    S7_BITOPS(1)
    S7_BITOPS(2)
    S7_BITOPS(3)
    S7_BITOPS(4)
    S7_BITOPS(5)
    S7_BITOPS(6)
    S7_BITOPS(7)
    S7_TCALL(0)
    S7_TCALL(1)
    S7_TCALL(2)
    S7_TCALL(3)
    S7_TCALL(4)
    S7_TCALL(5)
    S7_TCALL(6)
    S7_TCALL(7)
    S7_TCALL(8)
    S7_TCALL(9)
    S7_TCALL(10)
    S7_TCALL(11)
    S7_TCALL(12)
    S7_TCALL(13)
    S7_TCALL(14)
    S7_TCALL(15)

    /* ---- branches ---- */
    S7_BRANCH(0x10, !c->fn)
    S7_BRANCH(0x30, c->fn)
    S7_BRANCH(0x50, !c->fv)
    S7_BRANCH(0x70, c->fv)
    S7_BRANCH(0x90, !c->fc)
    S7_BRANCH(0xB0, c->fc)
    S7_BRANCH(0xD0, !c->fz)
    S7_BRANCH(0xF0, c->fz)
    S7_BRANCH(0x2F, 1)
    case 0x2E: { /* CBNE dp,rel */
        uint8_t ad = S7_FETCH();
        uint8_t d = S7_LOAD(ad);
        S7_IDLE();
        uint8_t disp = S7_FETCH();
        if (c->a != d) {
            S7_IDLE();
            S7_IDLE();
            c->pc = s7_rel(c->pc, disp);
        }
        break;
    }
    case 0xDE: { /* CBNE dp+X,rel */
        uint8_t ad = S7_FETCH();
        S7_IDLE();
        uint8_t d = S7_LOAD(ad + c->x);
        S7_IDLE();
        uint8_t disp = S7_FETCH();
        if (c->a != d) {
            S7_IDLE();
            S7_IDLE();
            c->pc = s7_rel(c->pc, disp);
        }
        break;
    }
    case 0x6E: { /* DBNZ dp,rel */
        uint8_t ad = S7_FETCH();
        uint8_t d = S7_LOAD(ad);
        d = (uint8_t)(d - 1u);
        S7_STORE(ad, d);
        uint8_t disp = S7_FETCH();
        if (d != 0) {
            S7_IDLE();
            S7_IDLE();
            c->pc = s7_rel(c->pc, disp);
        }
        break;
    }
    case 0xFE: { /* DBNZ Y,rel */
        S7_RD(c->pc);
        S7_IDLE();
        uint8_t disp = S7_FETCH();
        c->y = (uint8_t)(c->y - 1u);
        if (c->y != 0) {
            S7_IDLE();
            S7_IDLE();
            c->pc = s7_rel(c->pc, disp);
        }
        break;
    }

    /* ---- jumps, calls, returns ---- */
    case 0x5F: { /* JMP !abs */
        S7_ABS(ad);
        c->pc = ad;
        break;
    }
    case 0x1F: { /* JMP [!abs+X] */
        S7_ABS(ad);
        S7_IDLE();
        uint16_t t = S7_RD(ad + c->x);
        t = (uint16_t)(t | (S7_RD(ad + c->x + 1u) << 8));
        c->pc = t;
        break;
    }
    case 0x3F: { /* CALL !abs */
        S7_ABS(ad);
        S7_IDLE();
        S7_PUSH(c->pc >> 8);
        S7_PUSH(c->pc & 0xFFu);
        S7_IDLE();
        S7_IDLE();
        c->pc = ad;
        break;
    }
    case 0x4F: { /* PCALL up */
        uint8_t ad = S7_FETCH();
        S7_IDLE();
        S7_PUSH(c->pc >> 8);
        S7_PUSH(c->pc & 0xFFu);
        S7_IDLE();
        c->pc = (uint16_t)(0xFF00u | ad);
        break;
    }
    case 0x0F: { /* BRK */
        S7_RD(c->pc);
        S7_PUSH(c->pc >> 8);
        S7_PUSH(c->pc & 0xFFu);
        S7_PUSH(snes_spc700_get_psw(c));
        S7_IDLE();
        uint16_t t = S7_RD(0xFFDEu);
        t = (uint16_t)(t | (S7_RD(0xFFDFu) << 8));
        c->pc = t;
        c->fi = 0;
        c->fb = 1;
        break;
    }
    case 0x6F: { /* RET */
        S7_RD(c->pc);
        S7_IDLE();
        uint16_t t = S7_PULL();
        t = (uint16_t)(t | (S7_PULL() << 8));
        c->pc = t;
        break;
    }
    case 0x7F: { /* RETI */
        S7_RD(c->pc);
        S7_IDLE();
        snes_spc700_set_psw(c, S7_PULL());
        uint16_t t = S7_PULL();
        t = (uint16_t)(t | (S7_PULL() << 8));
        c->pc = t;
        break;
    }

    /* ---- stack ---- */
    case 0x0D: S7_RD(c->pc); S7_PUSH(snes_spc700_get_psw(c)); S7_IDLE(); break; /* PUSH PSW */
    case 0x2D: S7_RD(c->pc); S7_PUSH(c->a); S7_IDLE(); break;                   /* PUSH A */
    case 0x4D: S7_RD(c->pc); S7_PUSH(c->x); S7_IDLE(); break;                   /* PUSH X */
    case 0x6D: S7_RD(c->pc); S7_PUSH(c->y); S7_IDLE(); break;                   /* PUSH Y */
    case 0x8E: S7_RD(c->pc); S7_IDLE(); snes_spc700_set_psw(c, S7_PULL()); break; /* POP PSW */
    case 0xAE: S7_RD(c->pc); S7_IDLE(); c->a = S7_PULL(); break;                /* POP A */
    case 0xCE: S7_RD(c->pc); S7_IDLE(); c->x = S7_PULL(); break;                /* POP X */
    case 0xEE: S7_RD(c->pc); S7_IDLE(); c->y = S7_PULL(); break;                /* POP Y */

    /* ---- flags ---- */
    case 0x20: S7_RD(c->pc); c->fp = 0; break;            /* CLRP */
    case 0x40: S7_RD(c->pc); c->fp = 1; break;            /* SETP */
    case 0x60: S7_RD(c->pc); c->fc = 0; break;            /* CLRC */
    case 0x80: S7_RD(c->pc); c->fc = 1; break;            /* SETC */
    case 0xA0: S7_RD(c->pc); S7_IDLE(); c->fi = 1; break; /* EI */
    case 0xC0: S7_RD(c->pc); S7_IDLE(); c->fi = 0; break; /* DI */
    case 0xE0: S7_RD(c->pc); c->fh = 0; c->fv = 0; break; /* CLRV */
    case 0xED: S7_RD(c->pc); S7_IDLE(); c->fc ^= 1u; break; /* NOTC */

    /* ---- arithmetic specials ---- */
    case 0xCF: { /* MUL YA */
        S7_RD(c->pc);
        for (int i = 0; i < 7; i++) S7_IDLE();
        uint16_t ya = (uint16_t)(c->y * c->a);
        c->a = (uint8_t)ya;
        c->y = (uint8_t)(ya >> 8);
        s7_nz(c, c->y); /* flags from Y only */
        break;
    }
    case 0x9E: { /* DIV YA,X */
        S7_RD(c->pc);
        for (int i = 0; i < 10; i++) S7_IDLE();
        const uint32_t ya = (uint32_t)c->a | ((uint32_t)c->y << 8);
        const uint32_t x = c->x;
        c->fh = (uint8_t)((c->y & 15u) >= (x & 15u));
        c->fv = (uint8_t)(c->y >= x);
        if ((uint32_t)c->y < (x << 1)) {
            /* The quotient fits in 9 bits (V + A). */
            c->a = (uint8_t)(ya / x);
            c->y = (uint8_t)(ya % x);
        } else {
            /* The hardware's result when it does not. */
            c->a = (uint8_t)(255u - (ya - (x << 9)) / (256u - x));
            c->y = (uint8_t)(x + (ya - (x << 9)) % (256u - x));
        }
        s7_nz(c, c->a); /* flags from A only */
        break;
    }
    case 0x9F: { /* XCN A */
        S7_RD(c->pc);
        S7_IDLE();
        S7_IDLE();
        S7_IDLE();
        c->a = s7_nz(c, (uint8_t)((c->a >> 4) | (c->a << 4)));
        break;
    }
    case 0xDF: { /* DAA A */
        S7_RD(c->pc);
        S7_IDLE();
        if (c->fc || c->a > 0x99u) {
            c->a = (uint8_t)(c->a + 0x60u);
            c->fc = 1;
        }
        if (c->fh || (c->a & 15u) > 9u) c->a = (uint8_t)(c->a + 6u);
        s7_nz(c, c->a);
        break;
    }
    case 0xBE: { /* DAS A */
        S7_RD(c->pc);
        S7_IDLE();
        if (!c->fc || c->a > 0x99u) {
            c->a = (uint8_t)(c->a - 0x60u);
            c->fc = 0;
        }
        if (!c->fh || (c->a & 15u) > 9u) c->a = (uint8_t)(c->a - 6u);
        s7_nz(c, c->a);
        break;
    }

    /* ---- misc ---- */
    case 0x00: S7_RD(c->pc); break; /* NOP */
    case 0xEF:   /* SLEEP */
    case 0xFF: { /* STOP */
        c->halt = op == 0xEF ? SNES_SPC700_SLEEP : SNES_SPC700_STOP;
        S7_RD(c->pc);
        S7_IDLE();
        break;
    }
    }
}

#undef S7_RD
#undef S7_WR
#undef S7_IDLE
#undef S7_FETCH
#undef S7_DP
#undef S7_LOAD
#undef S7_STORE
#undef S7_PUSH
#undef S7_PULL
#undef S7_ABS
#undef S7_ALU_READS
#undef S7_ALU_MODS
#undef S7_RMW_DP
#undef S7_RMW_DPX
#undef S7_RMW_ABS
#undef S7_RMW_REG
#undef S7_REG_DP
#undef S7_REG_ABS
#undef S7_REG_IMM
#undef S7_REG_DPI
#undef S7_ST_DP
#undef S7_ST_ABS
#undef S7_ST_DPI
#undef S7_ST_ABSI
#undef S7_BRANCH
#undef S7_TCALL
#undef S7_BITOPS
#undef S7_MBIT
