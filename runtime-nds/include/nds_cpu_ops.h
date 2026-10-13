/*
 * Instruction arithmetic shared by both DS processors and by recompiled
 * code: conditions, the barrel shifter, the ALU and saturation. Everything
 * here is static inline, touches only NdsCpu, and is the same on ARMv4T and
 * ARMv5TE. The parts that differ between the two, or that call the bus, are
 * in nds_cpu_impl.inc.
 *
 * All arithmetic is on uint32_t/uint64_t. Signed values are only formed by
 * explicit casts where the hardware sign-extends.
 */
#ifndef NDS_CPU_OPS_H
#define NDS_CPU_OPS_H

#include "nds_cpu.h"
#include "nds_cpu_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Conditions                                                          */
/* ------------------------------------------------------------------ */

static inline bool nds_cond(const NdsCpu *c, uint32_t cond)
{
    switch (cond & 15u) {
    case NDS_COND_EQ: return c->flag_z;
    case NDS_COND_NE: return !c->flag_z;
    case NDS_COND_CS: return c->flag_c;
    case NDS_COND_CC: return !c->flag_c;
    case NDS_COND_MI: return c->flag_n;
    case NDS_COND_PL: return !c->flag_n;
    case NDS_COND_VS: return c->flag_v;
    case NDS_COND_VC: return !c->flag_v;
    case NDS_COND_HI: return c->flag_c && !c->flag_z;
    case NDS_COND_LS: return !c->flag_c || c->flag_z;
    case NDS_COND_GE: return c->flag_n == c->flag_v;
    case NDS_COND_LT: return c->flag_n != c->flag_v;
    case NDS_COND_GT: return !c->flag_z && c->flag_n == c->flag_v;
    case NDS_COND_LE: return c->flag_z || c->flag_n != c->flag_v;
    case NDS_COND_AL: return true;
    default: return false; /* NV */
    }
}

/* ------------------------------------------------------------------ */
/* Barrel shifter                                                      */
/* ------------------------------------------------------------------ */

enum { NDS_SHIFT_LSL, NDS_SHIFT_LSR, NDS_SHIFT_ASR, NDS_SHIFT_ROR };

static inline uint32_t nds_ror32(uint32_t value, uint32_t amount)
{
    amount &= 31u;
    return amount ? (value >> amount) | (value << (32u - amount)) : value;
}

/* Shift by an immediate amount (0-31) as encoded in the instruction, where
 * LSR #0 and ASR #0 mean a shift by 32 and ROR #0 means RRX. *carry holds
 * the current C on entry and the shifter carry-out on return. */
static inline uint32_t nds_shift_imm(uint32_t type, uint32_t value, uint32_t amount, uint8_t *carry)
{
    switch (type) {
    case NDS_SHIFT_LSL:
        if (amount == 0) return value;
        *carry = (uint8_t)((value >> (32u - amount)) & 1u);
        return value << amount;
    case NDS_SHIFT_LSR:
        if (amount == 0) {
            *carry = (uint8_t)(value >> 31);
            return 0;
        }
        *carry = (uint8_t)((value >> (amount - 1u)) & 1u);
        return value >> amount;
    case NDS_SHIFT_ASR:
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
        return nds_ror32(value, amount);
    }
}

/* Shift by a register amount (bottom byte of Rs, 0-255). An amount of 0
 * leaves both value and carry unchanged. */
static inline uint32_t nds_shift_reg(uint32_t type, uint32_t value, uint32_t amount, uint8_t *carry)
{
    amount &= 0xFFu;
    if (amount == 0) return value;
    switch (type) {
    case NDS_SHIFT_LSL:
        if (amount < 32) {
            *carry = (uint8_t)((value >> (32u - amount)) & 1u);
            return value << amount;
        }
        *carry = amount == 32 ? (uint8_t)(value & 1u) : 0;
        return 0;
    case NDS_SHIFT_LSR:
        if (amount < 32) {
            *carry = (uint8_t)((value >> (amount - 1u)) & 1u);
            return value >> amount;
        }
        *carry = amount == 32 ? (uint8_t)(value >> 31) : 0;
        return 0;
    case NDS_SHIFT_ASR:
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
        return nds_ror32(value, amount);
    }
}

/* Rotated 8-bit immediate. The carry-out is bit 31 of the result when the
 * rotation is non-zero and unchanged otherwise. */
static inline uint32_t nds_rotated_imm(uint32_t imm8, uint32_t rotate, uint8_t *carry)
{
    uint32_t value = nds_ror32(imm8, rotate * 2u);
    if (rotate) *carry = (uint8_t)(value >> 31);
    return value;
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */
/* ------------------------------------------------------------------ */

static inline void nds_set_nz(NdsCpu *c, uint32_t result)
{
    c->flag_n = (uint8_t)(result >> 31);
    c->flag_z = (uint8_t)(result == 0);
}

/* Logical operations: N and Z from the result, C from the shifter. */
static inline uint32_t nds_logic(NdsCpu *c, uint32_t result, uint8_t shifter_carry, bool s)
{
    if (s) {
        nds_set_nz(c, result);
        c->flag_c = shifter_carry;
    }
    return result;
}

static inline uint32_t nds_add(NdsCpu *c, uint32_t a, uint32_t b, bool s)
{
    uint32_t result = a + b;
    if (s) {
        nds_set_nz(c, result);
        c->flag_c = (uint8_t)(result < a);
        c->flag_v = (uint8_t)((~(a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

static inline uint32_t nds_adc(NdsCpu *c, uint32_t a, uint32_t b, bool s)
{
    uint64_t wide = (uint64_t)a + b + c->flag_c;
    uint32_t result = (uint32_t)wide;
    if (s) {
        nds_set_nz(c, result);
        c->flag_c = (uint8_t)(wide >> 32);
        c->flag_v = (uint8_t)((~(a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

static inline uint32_t nds_sub(NdsCpu *c, uint32_t a, uint32_t b, bool s)
{
    uint32_t result = a - b;
    if (s) {
        nds_set_nz(c, result);
        c->flag_c = (uint8_t)(a >= b);
        c->flag_v = (uint8_t)(((a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

static inline uint32_t nds_sbc(NdsCpu *c, uint32_t a, uint32_t b, bool s)
{
    uint32_t borrow = c->flag_c ? 0u : 1u;
    uint32_t result = a - b - borrow;
    if (s) {
        nds_set_nz(c, result);
        c->flag_c = (uint8_t)((uint64_t)a >= (uint64_t)b + borrow);
        c->flag_v = (uint8_t)(((a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* Saturating arithmetic (ARMv5TE)                                     */
/* ------------------------------------------------------------------ */

/* Signed saturating add and subtract. Saturation sets the sticky Q flag,
 * which only a write to CPSR clears. */
static inline uint32_t nds_sat_add(NdsCpu *c, uint32_t a, uint32_t b)
{
    uint32_t result = a + b;
    if ((~(a ^ b) & (a ^ result)) >> 31) {
        c->cpsr |= NDS_CPSR_Q;
        return (a >> 31) ? 0x80000000u : 0x7FFFFFFFu;
    }
    return result;
}

static inline uint32_t nds_sat_sub(NdsCpu *c, uint32_t a, uint32_t b)
{
    uint32_t result = a - b;
    if (((a ^ b) & (a ^ result)) >> 31) {
        c->cpsr |= NDS_CPSR_Q;
        return (a >> 31) ? 0x80000000u : 0x7FFFFFFFu;
    }
    return result;
}

/* A 32-bit add that sets Q on signed overflow but does not saturate
 * (SMLAxy, SMLAWy). */
static inline uint32_t nds_add_set_q(NdsCpu *c, uint32_t a, uint32_t b)
{
    uint32_t result = a + b;
    if ((~(a ^ b) & (a ^ result)) >> 31) c->cpsr |= NDS_CPSR_Q;
    return result;
}

static inline uint32_t nds_clz32(uint32_t value)
{
    uint32_t count = 0;
    if (value == 0) return 32;
    while (!(value & 0x80000000u)) {
        value <<= 1;
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Multiply (ARM7TDMI)                                                 */
/* ------------------------------------------------------------------ */

/* Number of 8-bit multiplier chunks the ARM7TDMI's Booth array processes
 * before terminating early (1-4). Signed multiplies stop when the remaining
 * bits are all zeros or all ones; unsigned ones only on all zeros. */
static inline int nds_mul_cycles(uint32_t multiplier, bool is_signed)
{
    int m = 1;
    for (int shift = 8; shift < 32; shift += 8, m++) {
        uint32_t rest = multiplier >> shift;
        if (rest == 0) return m;
        if (is_signed && rest == (0xFFFFFFFFu >> shift)) return m;
    }
    return 4;
}

enum { NDS_MUL_SHORT, NDS_MUL_LONG_SIGNED, NDS_MUL_LONG_UNSIGNED };

/* Carry flag left by MUL/MLA/UMULL/... with S set on the ARM7TDMI, which
 * does not leave C unchanged: it holds a carry-out of the Booth multiplier
 * array. multiplicand is Rm, multiplier is Rs, accumulator is Rn (MLA) or
 * RdHi:RdLo (MLAL) and 0 otherwise. See nds_mul_carry.c. The ARM9 leaves C
 * alone. */
uint8_t nds_mul_carry(int flavor, uint32_t multiplicand, uint32_t multiplier, uint64_t accumulator);

#ifdef __cplusplus
}
#endif

#endif /* NDS_CPU_OPS_H */
