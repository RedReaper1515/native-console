/*
 * ARM7TDMI instruction handlers, one per encoding class.
 *
 * These are the only implementation of instruction semantics. The
 * interpreter (gba_cpu_arm.c, gba_cpu_thumb.c) calls them after decoding;
 * recompiled code calls them directly with a constant opcode, and because
 * they are force-inlined the C compiler folds the field extraction and
 * branches on opcode bits away. SingleStepTests validates them through the
 * interpreter.
 *
 * Generated projects define GBA_RC_OUTLINE_HANDLERS (gbarecomp's default):
 * each generated file then calls one out-of-line copy of each handler
 * instead of folding it in (with gba_recomp.h's helpers, builds are about
 * 7x faster and native code about 12% slower; gbarecomp --inline-handlers
 * restores inlining).
 *
 * Contract: a handler executes one instruction whose condition already
 * passed, with r[15] = instruction address + 8 (ARM) or + 4 (Thumb). It
 * leaves r[15] unchanged unless it branches (gba_cpu_branch), in which case
 * cpu->branched is set; the caller advances r[15] otherwise.
 */
#ifndef GBA_CPU_IMPL_H
#define GBA_CPU_IMPL_H

#include "gba_cpu_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define GBA_OP_INLINE static __forceinline
#else
#define GBA_OP_INLINE static inline __attribute__((always_inline))
#endif
#if defined(GBA_RC_OUTLINE_HANDLERS) && defined(_MSC_VER)
#define GBA_OP static __declspec(noinline)
#elif defined(GBA_RC_OUTLINE_HANDLERS)
#define GBA_OP static __attribute__((noinline, unused))
#else
#define GBA_OP GBA_OP_INLINE
#endif

/* ------------------------------------------------------------------ */
/* Data processing                                                     */
/* ------------------------------------------------------------------ */

GBA_OP void gba_arm_dp(GbaCpu *c, uint32_t op)
{
    const bool s = (op >> 20) & 1u;
    const uint32_t opcode = (op >> 21) & 15u;
    const uint32_t rn = (op >> 16) & 15u;
    const uint32_t rd = (op >> 12) & 15u;

    uint8_t carry = c->flag_c;
    uint32_t op1 = c->r[rn];
    uint32_t op2;

    if (op & (1u << 25)) {
        op2 = gba_rotated_imm(op & 0xFFu, (op >> 8) & 15u, &carry);
    } else {
        const uint32_t type = (op >> 5) & 3u;
        const uint32_t rm = op & 15u;
        uint32_t rm_value = c->r[rm];
        if (op & 0x10u) {
            /* Register-specified shift: one internal cycle, during which
             * the PC has advanced another word. */
            uint32_t amount = c->r[(op >> 8) & 15u];
            gba_bus_idle(c, 1);
            if (rn == 15) op1 += 4;
            if (rm == 15) rm_value += 4;
            op2 = gba_shift_reg(type, rm_value, amount, &carry);
        } else {
            op2 = gba_shift_imm(type, rm_value, (op >> 7) & 31u, &carry);
        }
    }

    uint32_t result = 0;
    bool write = true;
    switch (opcode) {
    case 0x0: result = gba_logic(c, op1 & op2, carry, s); break;          /* AND */
    case 0x1: result = gba_logic(c, op1 ^ op2, carry, s); break;          /* EOR */
    case 0x2: result = gba_sub(c, op1, op2, s); break;                    /* SUB */
    case 0x3: result = gba_sub(c, op2, op1, s); break;                    /* RSB */
    case 0x4: result = gba_add(c, op1, op2, s); break;                    /* ADD */
    case 0x5: result = gba_adc(c, op1, op2, s); break;                    /* ADC */
    case 0x6: result = gba_sbc(c, op1, op2, s); break;                    /* SBC */
    case 0x7: result = gba_sbc(c, op2, op1, s); break;                    /* RSC */
    case 0x8: gba_logic(c, op1 & op2, carry, true); write = false; break; /* TST */
    case 0x9: gba_logic(c, op1 ^ op2, carry, true); write = false; break; /* TEQ */
    case 0xA: gba_sub(c, op1, op2, true); write = false; break;           /* CMP */
    case 0xB: gba_add(c, op1, op2, true); write = false; break;           /* CMN */
    case 0xC: result = gba_logic(c, op1 | op2, carry, s); break;          /* ORR */
    case 0xD: result = gba_logic(c, op2, carry, s); break;                /* MOV */
    case 0xE: result = gba_logic(c, op1 & ~op2, carry, s); break;         /* BIC */
    default: result = gba_logic(c, ~op2, carry, s); break;                /* MVN */
    }

    if (write) c->r[rd] = result;
    if (rd == 15) {
        /* S with Rd = R15 restores CPSR from SPSR (exception return). */
        if (s) gba_cpu_set_cpsr(c, gba_cpu_read_spsr(c));
        if (write) gba_cpu_branch(c, result);
    }
}

/* ------------------------------------------------------------------ */
/* PSR transfer                                                        */
/* ------------------------------------------------------------------ */

GBA_OP void gba_arm_mrs(GbaCpu *c, uint32_t op)
{
    const uint32_t rd = (op >> 12) & 15u;
    c->r[rd] = (op & (1u << 22)) ? gba_cpu_read_spsr(c) : gba_cpu_get_cpsr(c);
}

GBA_OP void gba_arm_msr(GbaCpu *c, uint32_t op)
{
    uint32_t value;
    if (op & (1u << 25)) {
        uint8_t unused = 0;
        value = gba_rotated_imm(op & 0xFFu, (op >> 8) & 15u, &unused);
    } else {
        value = c->r[op & 15u];
    }

    uint32_t mask = 0;
    if (op & (1u << 16)) mask |= 0x000000FFu;
    if (op & (1u << 17)) mask |= 0x0000FF00u;
    if (op & (1u << 18)) mask |= 0x00FF0000u;
    if (op & (1u << 19)) mask |= 0xFF000000u;

    if (op & (1u << 22)) {
        if (gba_cpu_has_spsr(c)) {
            uint32_t *spsr = gba_cpu_current_spsr(c);
            *spsr = (*spsr & ~mask) | (value & mask);
        }
        return;
    }

    if (gba_cpu_mode(c) == GBA_MODE_USR) mask &= 0xFF000000u;
    /* Mode bit 4 is hardwired to 1: the 26-bit modes do not exist. */
    if (mask & 0xFFu) value |= 0x10u;
    uint32_t cpsr = gba_cpu_get_cpsr(c);
    gba_cpu_set_cpsr(c, (cpsr & ~mask) | (value & mask));
}

/* ------------------------------------------------------------------ */
/* Multiply                                                            */
/* ------------------------------------------------------------------ */

/* Registers read after the first cycle see R15 one word further on. */
GBA_OP_INLINE uint32_t gba_late_reg(const GbaCpu *c, uint32_t reg)
{
    return c->r[reg] + (reg == 15 ? 4u : 0u);
}

GBA_OP void gba_arm_mul(GbaCpu *c, uint32_t op)
{
    const bool s = (op >> 20) & 1u;
    const bool accumulate = (op >> 21) & 1u;
    const uint32_t rd = (op >> 16) & 15u;
    const uint32_t rn = (op >> 12) & 15u;
    const uint32_t rs = (op >> 8) & 15u;
    const uint32_t rm = op & 15u;

    uint32_t multiplicand = gba_late_reg(c, rm);
    uint32_t multiplier = gba_late_reg(c, rs);
    uint32_t acc = accumulate ? gba_late_reg(c, rn) : 0u;
    uint32_t result = multiplicand * multiplier + acc;

    gba_bus_idle(c, gba_mul_cycles(multiplier, true) + (accumulate ? 1 : 0));
    if (s) {
        gba_set_nz(c, result);
        c->flag_c = gba_mul_carry(GBA_MUL_SHORT, multiplicand, multiplier, acc);
    }
    if (rd == 15) gba_cpu_branch(c, result);
    else c->r[rd] = result;
}

GBA_OP void gba_arm_mull(GbaCpu *c, uint32_t op)
{
    const bool s = (op >> 20) & 1u;
    const bool accumulate = (op >> 21) & 1u;
    const bool is_signed = (op >> 22) & 1u;
    const uint32_t rdhi = (op >> 16) & 15u;
    const uint32_t rdlo = (op >> 12) & 15u;
    const uint32_t rs = (op >> 8) & 15u;
    const uint32_t rm = op & 15u;

    uint32_t multiplicand = gba_late_reg(c, rm);
    uint32_t multiplier = gba_late_reg(c, rs);
    uint64_t acc = accumulate ? (((uint64_t)gba_late_reg(c, rdhi) << 32) | gba_late_reg(c, rdlo)) : 0u;
    uint64_t result;
    if (is_signed) {
        result = (uint64_t)((int64_t)(int32_t)multiplicand * (int64_t)(int32_t)multiplier);
    } else {
        result = (uint64_t)multiplicand * multiplier;
    }
    result += acc;

    gba_bus_idle(c, gba_mul_cycles(multiplier, is_signed) + 1 + (accumulate ? 1 : 0));
    if (s) {
        c->flag_n = (uint8_t)(result >> 63);
        c->flag_z = (uint8_t)(result == 0);
        c->flag_c = gba_mul_carry(is_signed ? GBA_MUL_LONG_SIGNED : GBA_MUL_LONG_UNSIGNED,
                                  multiplicand, multiplier, acc);
    }
    c->r[rdlo] = (uint32_t)result;
    c->r[rdhi] = (uint32_t)(result >> 32);
    if (rdlo == 15 || rdhi == 15) gba_cpu_branch(c, c->r[15]);
}

/* ------------------------------------------------------------------ */
/* Loads and stores                                                    */
/* ------------------------------------------------------------------ */

GBA_OP void gba_arm_swp(GbaCpu *c, uint32_t op)
{
    const bool byte = (op >> 22) & 1u;
    const uint32_t rn = (op >> 16) & 15u;
    const uint32_t rd = (op >> 12) & 15u;
    const uint32_t rm = op & 15u;
    uint32_t addr = gba_late_reg(c, rn);
    uint32_t source = gba_late_reg(c, rm);
    uint32_t loaded;

    if (byte) {
        loaded = gba_bus_read8(c, addr, GBA_ACCESS_NSEQ);
        gba_bus_write8(c, addr, (uint8_t)source, GBA_ACCESS_NSEQ | GBA_ACCESS_LOCK);
    } else {
        loaded = gba_load_word(c, addr, GBA_ACCESS_NSEQ);
        gba_bus_write32(c, addr, source, GBA_ACCESS_NSEQ | GBA_ACCESS_LOCK);
    }
    gba_bus_idle(c, 1);
    if (rd == 15) gba_cpu_branch(c, loaded);
    else c->r[rd] = loaded;
}

/* Base writeback for LDR/STR/LDRH/STRH. Writing back into R15 (an
 * unpredictable encoding) matches the reference: the base is re-read a word
 * later and the pipeline is refilled. */
GBA_OP_INLINE void gba_transfer_writeback(GbaCpu *c, uint32_t rn, uint32_t value)
{
    if (rn == 15) gba_cpu_branch(c, value + 4u);
    else c->r[rn] = value;
}

GBA_OP void gba_arm_sdt(GbaCpu *c, uint32_t op)
{
    const bool pre = (op >> 24) & 1u;
    const bool up = (op >> 23) & 1u;
    const bool byte = (op >> 22) & 1u;
    const bool writeback = (op >> 21) & 1u;
    const bool load = (op >> 20) & 1u;
    const uint32_t rn = (op >> 16) & 15u;
    const uint32_t rd = (op >> 12) & 15u;

    uint32_t offset;
    if (op & (1u << 25)) {
        uint8_t unused = c->flag_c;
        offset = gba_shift_imm((op >> 5) & 3u, c->r[op & 15u], (op >> 7) & 31u, &unused);
    } else {
        offset = op & 0xFFFu;
    }

    uint32_t base = c->r[rn];
    uint32_t moved = up ? base + offset : base - offset;
    uint32_t addr = pre ? moved : base;

    if (load) {
        uint32_t value = byte ? gba_bus_read8(c, addr, GBA_ACCESS_NSEQ)
                              : gba_load_word(c, addr, GBA_ACCESS_NSEQ);
        gba_bus_idle(c, 1);
        if (!pre || writeback) gba_transfer_writeback(c, rn, moved);
        if (rd == 15) gba_cpu_branch(c, value);
        else c->r[rd] = value;
    } else {
        uint32_t value = c->r[rd] + (rd == 15 ? 4u : 0u);
        if (byte) gba_bus_write8(c, addr, (uint8_t)value, GBA_ACCESS_NSEQ);
        else gba_bus_write32(c, addr, value, GBA_ACCESS_NSEQ);
        if (!pre || writeback) gba_transfer_writeback(c, rn, moved);
    }
}

GBA_OP void gba_arm_half(GbaCpu *c, uint32_t op)
{
    const bool pre = (op >> 24) & 1u;
    const bool up = (op >> 23) & 1u;
    const bool immediate = (op >> 22) & 1u;
    const bool writeback = (op >> 21) & 1u;
    const bool load = (op >> 20) & 1u;
    const uint32_t rn = (op >> 16) & 15u;
    const uint32_t rd = (op >> 12) & 15u;
    const uint32_t kind = (op >> 5) & 3u;

    uint32_t offset = immediate ? (((op >> 4) & 0xF0u) | (op & 0xFu)) : c->r[op & 15u];
    uint32_t base = c->r[rn];
    uint32_t moved = up ? base + offset : base - offset;
    uint32_t addr = pre ? moved : base;

    if (load) {
        uint32_t value;
        switch (kind) {
        case 1: value = gba_load_half(c, addr, GBA_ACCESS_NSEQ); break;
        case 2: value = gba_load_signed_byte(c, addr, GBA_ACCESS_NSEQ); break;
        default: value = gba_load_signed_half(c, addr, GBA_ACCESS_NSEQ); break;
        }
        gba_bus_idle(c, 1);
        if (!pre || writeback) gba_transfer_writeback(c, rn, moved);
        if (rd == 15) gba_cpu_branch(c, value);
        else c->r[rd] = value;
    } else {
        /* Only STRH exists on ARMv4; the signed forms with L=0 are the
         * ARMv5 LDRD/STRD encodings and do nothing here beyond writeback. */
        if (kind == 1) {
            uint32_t value = c->r[rd] + (rd == 15 ? 4u : 0u);
            gba_bus_write16(c, addr, (uint16_t)value, GBA_ACCESS_NSEQ);
        }
        if (!pre || writeback) gba_transfer_writeback(c, rn, moved);
    }
}

GBA_OP void gba_arm_block(GbaCpu *c, uint32_t op)
{
    gba_op_block_transfer(c, (op >> 16) & 15u, op & 0xFFFFu, (op >> 24) & 1u, (op >> 23) & 1u,
                          (op >> 22) & 1u, (op >> 21) & 1u, (op >> 20) & 1u, 4u, 0xFFFFFFFFu);
}

/* ------------------------------------------------------------------ */
/* Branches and exceptions                                             */
/* ------------------------------------------------------------------ */

GBA_OP void gba_arm_b(GbaCpu *c, uint32_t op)
{
    uint32_t offset = (uint32_t)((int32_t)(op << 8) >> 6);
    if (op & (1u << 24)) c->r[14] = c->r[15] - 4u;
    gba_cpu_branch(c, c->r[15] + offset);
}

GBA_OP void gba_arm_bx(GbaCpu *c, uint32_t op)
{
    uint32_t target = c->r[op & 15u];
    if (target & 1u) c->cpsr |= GBA_CPSR_T;
    else c->cpsr &= ~GBA_CPSR_T;
    gba_cpu_branch(c, target & ~1u);
}

/* ------------------------------------------------------------------ */
/* Thumb                                                               */
/* ------------------------------------------------------------------ */

/* Format 1: LSL/LSR/ASR Rd, Rs, #imm5. */
GBA_OP void gba_thumb_shift_imm(GbaCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t rs = (op >> 3) & 7u;
    uint8_t carry = c->flag_c;
    uint32_t result = gba_shift_imm((op >> 11) & 3u, c->r[rs], (op >> 6) & 31u, &carry);
    c->r[rd] = gba_logic(c, result, carry, true);
}

/* Format 2: ADD/SUB Rd, Rs, Rn|#imm3. */
GBA_OP void gba_thumb_add_sub(GbaCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t rs = (op >> 3) & 7u;
    uint32_t field = (op >> 6) & 7u;
    uint32_t operand = (op & (1u << 10)) ? field : c->r[field];
    if (op & (1u << 9)) c->r[rd] = gba_sub(c, c->r[rs], operand, true);
    else c->r[rd] = gba_add(c, c->r[rs], operand, true);
}

/* Format 3: MOV/CMP/ADD/SUB Rd, #imm8. */
GBA_OP void gba_thumb_imm8(GbaCpu *c, uint16_t op)
{
    uint32_t rd = (op >> 8) & 7u;
    uint32_t imm = op & 0xFFu;
    switch ((op >> 11) & 3u) {
    case 0: c->r[rd] = imm; gba_set_nz(c, imm); break;
    case 1: gba_sub(c, c->r[rd], imm, true); break;
    case 2: c->r[rd] = gba_add(c, c->r[rd], imm, true); break;
    default: c->r[rd] = gba_sub(c, c->r[rd], imm, true); break;
    }
}

/* Format 4: ALU operations on low registers. */
GBA_OP void gba_thumb_alu(GbaCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t rs = (op >> 3) & 7u;
    uint32_t a = c->r[rd];
    uint32_t b = c->r[rs];
    uint8_t carry = c->flag_c;
    /* Shift first: it writes carry and argument order is unspecified. */
    uint32_t shifted;

    switch ((op >> 6) & 15u) {
    case 0x0: c->r[rd] = gba_logic(c, a & b, carry, true); break; /* AND */
    case 0x1: c->r[rd] = gba_logic(c, a ^ b, carry, true); break; /* EOR */
    case 0x2: /* LSL */
        gba_bus_idle(c, 1);
        shifted = gba_shift_reg(GBA_SHIFT_LSL, a, b, &carry);
        c->r[rd] = gba_logic(c, shifted, carry, true);
        break;
    case 0x3: /* LSR */
        gba_bus_idle(c, 1);
        shifted = gba_shift_reg(GBA_SHIFT_LSR, a, b, &carry);
        c->r[rd] = gba_logic(c, shifted, carry, true);
        break;
    case 0x4: /* ASR */
        gba_bus_idle(c, 1);
        shifted = gba_shift_reg(GBA_SHIFT_ASR, a, b, &carry);
        c->r[rd] = gba_logic(c, shifted, carry, true);
        break;
    case 0x5: c->r[rd] = gba_adc(c, a, b, true); break;
    case 0x6: c->r[rd] = gba_sbc(c, a, b, true); break;
    case 0x7: /* ROR */
        gba_bus_idle(c, 1);
        shifted = gba_shift_reg(GBA_SHIFT_ROR, a, b, &carry);
        c->r[rd] = gba_logic(c, shifted, carry, true);
        break;
    case 0x8: gba_logic(c, a & b, carry, true); break;            /* TST */
    case 0x9: c->r[rd] = gba_sub(c, 0, b, true); break;           /* NEG */
    case 0xA: gba_sub(c, a, b, true); break;                      /* CMP */
    case 0xB: gba_add(c, a, b, true); break;                      /* CMN */
    case 0xC: c->r[rd] = gba_logic(c, a | b, carry, true); break; /* ORR */
    case 0xD: {                                                   /* MUL */
        /* MULS Rd, Rs, Rd: Rs is the multiplicand, Rd the multiplier. */
        uint32_t result = b * a;
        gba_bus_idle(c, gba_mul_cycles(a, true));
        gba_set_nz(c, result);
        c->flag_c = gba_mul_carry(GBA_MUL_SHORT, b, a, 0);
        c->r[rd] = result;
        break;
    }
    case 0xE: c->r[rd] = gba_logic(c, a & ~b, carry, true); break; /* BIC */
    default: c->r[rd] = gba_logic(c, ~b, carry, true); break;      /* MVN */
    }
}

/* Format 5: ADD/CMP/MOV on high registers, and BX. */
GBA_OP void gba_thumb_hi_reg(GbaCpu *c, uint16_t op)
{
    uint32_t rd = (op & 7u) | ((op >> 4) & 8u);
    uint32_t rs = (op >> 3) & 15u;
    uint32_t value = c->r[rs];

    switch ((op >> 8) & 3u) {
    case 0: {
        uint32_t result = c->r[rd] + value;
        if (rd == 15) gba_cpu_branch(c, result & ~1u);
        else c->r[rd] = result;
        break;
    }
    case 1:
        gba_sub(c, c->r[rd], value, true);
        break;
    case 2:
        if (rd == 15) gba_cpu_branch(c, value & ~1u);
        else c->r[rd] = value;
        break;
    default:
        if (value & 1u) c->cpsr |= GBA_CPSR_T;
        else c->cpsr &= ~GBA_CPSR_T;
        gba_cpu_branch(c, value & ~1u);
        break;
    }
}

/* Format 6: LDR Rd, [PC, #imm8*4]. */
GBA_OP void gba_thumb_ldr_pc(GbaCpu *c, uint16_t op)
{
    uint32_t addr = (c->r[15] & ~2u) + ((op & 0xFFu) << 2);
    c->r[(op >> 8) & 7u] = gba_load_word(c, addr, GBA_ACCESS_NSEQ);
    gba_bus_idle(c, 1);
}

/* Format 7: LDR/STR/LDRB/STRB Rd, [Rb, Ro]. */
GBA_OP void gba_thumb_ldst_reg(GbaCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t addr = c->r[(op >> 3) & 7u] + c->r[(op >> 6) & 7u];
    switch ((op >> 10) & 3u) {
    case 0: gba_bus_write32(c, addr, c->r[rd], GBA_ACCESS_NSEQ); break;
    case 1: gba_bus_write8(c, addr, (uint8_t)c->r[rd], GBA_ACCESS_NSEQ); break;
    case 2: c->r[rd] = gba_load_word(c, addr, GBA_ACCESS_NSEQ); gba_bus_idle(c, 1); break;
    default: c->r[rd] = gba_bus_read8(c, addr, GBA_ACCESS_NSEQ); gba_bus_idle(c, 1); break;
    }
}

/* Format 8: STRH/LDSB/LDRH/LDSH Rd, [Rb, Ro]. */
GBA_OP void gba_thumb_ldst_sign(GbaCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t addr = c->r[(op >> 3) & 7u] + c->r[(op >> 6) & 7u];
    switch ((op >> 10) & 3u) {
    case 0: gba_bus_write16(c, addr, (uint16_t)c->r[rd], GBA_ACCESS_NSEQ); break;
    case 1: c->r[rd] = gba_load_signed_byte(c, addr, GBA_ACCESS_NSEQ); gba_bus_idle(c, 1); break;
    case 2: c->r[rd] = gba_load_half(c, addr, GBA_ACCESS_NSEQ); gba_bus_idle(c, 1); break;
    default: c->r[rd] = gba_load_signed_half(c, addr, GBA_ACCESS_NSEQ); gba_bus_idle(c, 1); break;
    }
}

/* Format 9: LDR/STR/LDRB/STRB Rd, [Rb, #imm5]. */
GBA_OP void gba_thumb_ldst_imm(GbaCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t base = c->r[(op >> 3) & 7u];
    uint32_t imm = (op >> 6) & 31u;
    switch ((op >> 11) & 3u) {
    case 0: gba_bus_write32(c, base + (imm << 2), c->r[rd], GBA_ACCESS_NSEQ); break;
    case 1: c->r[rd] = gba_load_word(c, base + (imm << 2), GBA_ACCESS_NSEQ); gba_bus_idle(c, 1); break;
    case 2: gba_bus_write8(c, base + imm, (uint8_t)c->r[rd], GBA_ACCESS_NSEQ); break;
    default: c->r[rd] = gba_bus_read8(c, base + imm, GBA_ACCESS_NSEQ); gba_bus_idle(c, 1); break;
    }
}

/* Format 10: LDRH/STRH Rd, [Rb, #imm5*2]. */
GBA_OP void gba_thumb_ldst_half(GbaCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t addr = c->r[(op >> 3) & 7u] + (((op >> 6) & 31u) << 1);
    if (op & (1u << 11)) {
        c->r[rd] = gba_load_half(c, addr, GBA_ACCESS_NSEQ);
        gba_bus_idle(c, 1);
    } else {
        gba_bus_write16(c, addr, (uint16_t)c->r[rd], GBA_ACCESS_NSEQ);
    }
}

/* Format 11: LDR/STR Rd, [SP, #imm8*4]. */
GBA_OP void gba_thumb_ldst_sp(GbaCpu *c, uint16_t op)
{
    uint32_t rd = (op >> 8) & 7u;
    uint32_t addr = c->r[13] + ((op & 0xFFu) << 2);
    if (op & (1u << 11)) {
        c->r[rd] = gba_load_word(c, addr, GBA_ACCESS_NSEQ);
        gba_bus_idle(c, 1);
    } else {
        gba_bus_write32(c, addr, c->r[rd], GBA_ACCESS_NSEQ);
    }
}

/* Format 12: ADD Rd, PC|SP, #imm8*4. */
GBA_OP void gba_thumb_add_pc_sp(GbaCpu *c, uint16_t op)
{
    uint32_t imm = (op & 0xFFu) << 2;
    uint32_t base = (op & (1u << 11)) ? c->r[13] : (c->r[15] & ~2u);
    c->r[(op >> 8) & 7u] = base + imm;
}

/* Format 13: ADD SP, #+-imm7*4. */
GBA_OP void gba_thumb_add_sp(GbaCpu *c, uint16_t op)
{
    uint32_t imm = (op & 0x7Fu) << 2;
    if (op & 0x80u) c->r[13] -= imm;
    else c->r[13] += imm;
}

/* Format 14: PUSH/POP {Rlist, LR|PC}. */
GBA_OP void gba_thumb_push_pop(GbaCpu *c, uint16_t op)
{
    uint32_t rlist = op & 0xFFu;
    if (op & (1u << 11)) {
        if (op & 0x100u) rlist |= 1u << 15;
        gba_op_block_transfer(c, 13, rlist, false, true, false, true, true, 2u, ~1u);
    } else {
        if (op & 0x100u) rlist |= 1u << 14;
        gba_op_block_transfer(c, 13, rlist, true, false, false, true, false, 2u, ~1u);
    }
}

/* Format 15: LDMIA/STMIA Rb!, {Rlist}. */
GBA_OP void gba_thumb_ldm_stm(GbaCpu *c, uint16_t op)
{
    gba_op_block_transfer(c, (op >> 8) & 7u, op & 0xFFu, false, true, false, true,
                          (op >> 11) & 1u, 2u, ~1u);
}

/* Format 16/17: conditional branch, SWI, and the undefined condition 14. */
GBA_OP void gba_thumb_bcc(GbaCpu *c, uint16_t op)
{
    uint32_t cond = (op >> 8) & 15u;
    if (cond == 15) {
        gba_op_swi(c);
    } else if (gba_cond(c, cond)) {
        /* Condition 14 is architecturally undefined; the ARM7TDMI treats
         * it as "always". */
        uint32_t offset = (uint32_t)((int32_t)(int8_t)(op & 0xFFu) * 2);
        gba_cpu_branch(c, c->r[15] + offset);
    }
}

/* Format 18: B #imm11. */
GBA_OP void gba_thumb_b(GbaCpu *c, uint16_t op)
{
    uint32_t offset = (uint32_t)(((int32_t)((uint32_t)op << 21)) >> 20);
    gba_cpu_branch(c, c->r[15] + offset);
}

/* Format 19: the two halves of BL. */
GBA_OP void gba_thumb_bl(GbaCpu *c, uint16_t op)
{
    uint32_t imm = op & 0x7FFu;
    if (op & (1u << 11)) {
        uint32_t next = (c->r[15] - 2u) | 1u;
        gba_cpu_branch(c, (c->r[14] + (imm << 1)) & ~1u);
        c->r[14] = next;
    } else {
        uint32_t offset = (uint32_t)(((int32_t)(imm << 21)) >> 9);
        c->r[14] = c->r[15] + offset;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* GBA_CPU_IMPL_H */
