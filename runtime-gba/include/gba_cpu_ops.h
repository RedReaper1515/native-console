/*
 * ARM7TDMI instruction semantics shared by the interpreter and by recompiled
 * code. Everything here is static inline and operates on GbaCpu, so the
 * emitter can call these helpers with constant arguments and let the C
 * compiler fold them. The SingleStepTests harness validates these helpers
 * through the interpreter; the emitter must not reimplement them.
 *
 * All arithmetic is on uint32_t/uint64_t. Signed values are only formed by
 * explicit casts where the hardware sign-extends.
 */
#ifndef GBA_CPU_OPS_H
#define GBA_CPU_OPS_H

#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_cpu_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Conditions                                                          */
/* ------------------------------------------------------------------ */

static inline bool gba_cond(const GbaCpu *c, uint32_t cond)
{
    switch (cond & 15u) {
    case GBA_COND_EQ: return c->flag_z;
    case GBA_COND_NE: return !c->flag_z;
    case GBA_COND_CS: return c->flag_c;
    case GBA_COND_CC: return !c->flag_c;
    case GBA_COND_MI: return c->flag_n;
    case GBA_COND_PL: return !c->flag_n;
    case GBA_COND_VS: return c->flag_v;
    case GBA_COND_VC: return !c->flag_v;
    case GBA_COND_HI: return c->flag_c && !c->flag_z;
    case GBA_COND_LS: return !c->flag_c || c->flag_z;
    case GBA_COND_GE: return c->flag_n == c->flag_v;
    case GBA_COND_LT: return c->flag_n != c->flag_v;
    case GBA_COND_GT: return !c->flag_z && c->flag_n == c->flag_v;
    case GBA_COND_LE: return c->flag_z || c->flag_n != c->flag_v;
    case GBA_COND_AL: return true;
    default: return false; /* NV */
    }
}

/* ------------------------------------------------------------------ */
/* Barrel shifter                                                      */
/* ------------------------------------------------------------------ */

enum { GBA_SHIFT_LSL, GBA_SHIFT_LSR, GBA_SHIFT_ASR, GBA_SHIFT_ROR };

static inline uint32_t gba_ror32(uint32_t value, uint32_t amount)
{
    amount &= 31u;
    return amount ? (value >> amount) | (value << (32u - amount)) : value;
}

/* Shift by an immediate amount (0-31) as encoded in the instruction, where
 * LSR #0 and ASR #0 mean a shift by 32 and ROR #0 means RRX. *carry holds
 * the current C on entry and the shifter carry-out on return. */
static inline uint32_t gba_shift_imm(uint32_t type, uint32_t value, uint32_t amount, uint8_t *carry)
{
    switch (type) {
    case GBA_SHIFT_LSL:
        if (amount == 0) return value;
        *carry = (uint8_t)((value >> (32u - amount)) & 1u);
        return value << amount;
    case GBA_SHIFT_LSR:
        if (amount == 0) {
            *carry = (uint8_t)(value >> 31);
            return 0;
        }
        *carry = (uint8_t)((value >> (amount - 1u)) & 1u);
        return value >> amount;
    case GBA_SHIFT_ASR:
        if (amount == 0) {
            *carry = (uint8_t)(value >> 31);
            return (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        }
        *carry = (uint8_t)((value >> (amount - 1u)) & 1u);
        return (uint32_t)((int32_t)value >> amount);
    default: /* ROR, RRX for #0 */
        if (amount == 0) {
            uint32_t result = (value >> 1) | ((uint32_t)*carry << 31);
            *carry = (uint8_t)(value & 1u);
            return result;
        }
        *carry = (uint8_t)((value >> (amount - 1u)) & 1u);
        return gba_ror32(value, amount);
    }
}

/* Shift by a register amount (bottom byte of Rs, 0-255). An amount of 0
 * leaves both value and carry unchanged. */
static inline uint32_t gba_shift_reg(uint32_t type, uint32_t value, uint32_t amount, uint8_t *carry)
{
    amount &= 0xFFu;
    if (amount == 0) return value;
    switch (type) {
    case GBA_SHIFT_LSL:
        if (amount < 32) {
            *carry = (uint8_t)((value >> (32u - amount)) & 1u);
            return value << amount;
        }
        *carry = amount == 32 ? (uint8_t)(value & 1u) : 0;
        return 0;
    case GBA_SHIFT_LSR:
        if (amount < 32) {
            *carry = (uint8_t)((value >> (amount - 1u)) & 1u);
            return value >> amount;
        }
        *carry = amount == 32 ? (uint8_t)(value >> 31) : 0;
        return 0;
    case GBA_SHIFT_ASR:
        if (amount < 32) {
            *carry = (uint8_t)((value >> (amount - 1u)) & 1u);
            return (uint32_t)((int32_t)value >> amount);
        }
        *carry = (uint8_t)(value >> 31);
        return (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
    default: /* ROR */
        amount &= 31u;
        if (amount == 0) {
            *carry = (uint8_t)(value >> 31);
            return value;
        }
        *carry = (uint8_t)((value >> (amount - 1u)) & 1u);
        return gba_ror32(value, amount);
    }
}

/* Rotated 8-bit immediate. The carry-out is bit 31 of the result when the
 * rotation is non-zero and unchanged otherwise. */
static inline uint32_t gba_rotated_imm(uint32_t imm8, uint32_t rotate, uint8_t *carry)
{
    uint32_t value = gba_ror32(imm8, rotate * 2u);
    if (rotate) *carry = (uint8_t)(value >> 31);
    return value;
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */
/* ------------------------------------------------------------------ */

static inline void gba_set_nz(GbaCpu *c, uint32_t result)
{
    c->flag_n = (uint8_t)(result >> 31);
    c->flag_z = (uint8_t)(result == 0);
}

/* Logical operations: N and Z from the result, C from the shifter. */
static inline uint32_t gba_logic(GbaCpu *c, uint32_t result, uint8_t shifter_carry, bool s)
{
    if (s) {
        gba_set_nz(c, result);
        c->flag_c = shifter_carry;
    }
    return result;
}

static inline uint32_t gba_add(GbaCpu *c, uint32_t a, uint32_t b, bool s)
{
    uint32_t result = a + b;
    if (s) {
        gba_set_nz(c, result);
        c->flag_c = (uint8_t)(result < a);
        c->flag_v = (uint8_t)((~(a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

static inline uint32_t gba_adc(GbaCpu *c, uint32_t a, uint32_t b, bool s)
{
    uint64_t wide = (uint64_t)a + b + c->flag_c;
    uint32_t result = (uint32_t)wide;
    if (s) {
        gba_set_nz(c, result);
        c->flag_c = (uint8_t)(wide >> 32);
        c->flag_v = (uint8_t)((~(a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

static inline uint32_t gba_sub(GbaCpu *c, uint32_t a, uint32_t b, bool s)
{
    uint32_t result = a - b;
    if (s) {
        gba_set_nz(c, result);
        c->flag_c = (uint8_t)(a >= b);
        c->flag_v = (uint8_t)(((a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

static inline uint32_t gba_sbc(GbaCpu *c, uint32_t a, uint32_t b, bool s)
{
    uint32_t borrow = c->flag_c ? 0u : 1u;
    uint32_t result = a - b - borrow;
    if (s) {
        gba_set_nz(c, result);
        c->flag_c = (uint8_t)((uint64_t)a >= (uint64_t)b + borrow);
        c->flag_v = (uint8_t)(((a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* Multiply                                                            */
/* ------------------------------------------------------------------ */

/* Number of 8-bit multiplier chunks the ARM7TDMI's Booth array processes
 * before terminating early (1-4). Signed multiplies stop when the remaining
 * bits are all zeros or all ones; unsigned ones only on all zeros. */
static inline int gba_mul_cycles(uint32_t multiplier, bool is_signed)
{
    int m = 1;
    for (int shift = 8; shift < 32; shift += 8, m++) {
        uint32_t rest = multiplier >> shift;
        if (rest == 0) return m;
        if (is_signed && rest == (0xFFFFFFFFu >> shift)) return m;
    }
    return 4;
}

enum { GBA_MUL_SHORT, GBA_MUL_LONG_SIGNED, GBA_MUL_LONG_UNSIGNED };

/* Carry flag left by MUL/MLA/UMULL/... with S set. The ARM7TDMI does not
 * leave C unchanged: it holds a carry-out of the Booth multiplier array.
 * multiplicand is Rm, multiplier is Rs, accumulator is Rn (MLA) or
 * RdHi:RdLo (MLAL) and 0 otherwise. See gba_mul_carry.c. */
uint8_t gba_mul_carry(int flavor, uint32_t multiplicand, uint32_t multiplier, uint64_t accumulator);

/* ------------------------------------------------------------------ */
/* Out-of-line operations (gba_cpu_arm.c)                              */
/* ------------------------------------------------------------------ */

/* LDM/STM, also used for Thumb PUSH/POP/LDMIA/STMIA. pc_store_bias is
 * added to r[15] when R15 is stored: 4 in ARM state (instruction + 12),
 * 2 in Thumb state. pc_load_mask is applied to a loaded R15 (~1 for POP;
 * ARM LDM does not align). */
void gba_op_block_transfer(GbaCpu *c, uint32_t rn, uint32_t rlist, bool pre, bool up, bool s,
                           bool writeback, bool load, uint32_t pc_store_bias, uint32_t pc_load_mask);

/* Undefined instruction and SWI exception entry from either state. */
void gba_op_undefined(GbaCpu *c);
void gba_op_swi(GbaCpu *c);

/* ------------------------------------------------------------------ */
/* Loads with the ARM7TDMI misaligned-access behaviour                 */
/* ------------------------------------------------------------------ */

/* LDR/SWP: the word at the aligned address, rotated so the addressed byte
 * lands in bits 0-7. */
static inline uint32_t gba_load_word(GbaCpu *c, uint32_t addr, int access)
{
    uint32_t value = gba_bus_read32(c, addr, access);
    return gba_ror32(value, (addr & 3u) * 8u);
}

/* LDRH: the halfword at the aligned address, rotated by 8 when odd. */
static inline uint32_t gba_load_half(GbaCpu *c, uint32_t addr, int access)
{
    uint32_t value = gba_bus_read16(c, addr, access);
    return gba_ror32(value, (addr & 1u) * 8u);
}

/* LDRSH: an odd address still performs a halfword access but
 * sign-extends only the addressed (high) byte. */
static inline uint32_t gba_load_signed_half(GbaCpu *c, uint32_t addr, int access)
{
    uint32_t value = gba_bus_read16(c, addr, access);
    if (addr & 1u) return (uint32_t)(int32_t)(int8_t)(uint8_t)(value >> 8);
    return (uint32_t)(int32_t)(int16_t)(uint16_t)value;
}

static inline uint32_t gba_load_signed_byte(GbaCpu *c, uint32_t addr, int access)
{
    return (uint32_t)(int32_t)(int8_t)gba_bus_read8(c, addr, access);
}

#ifdef __cplusplus
}
#endif

#endif /* GBA_CPU_OPS_H */
