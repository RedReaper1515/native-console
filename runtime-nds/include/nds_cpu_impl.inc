/*
 * ARM and Thumb instruction handlers, one per encoding class, for both DS
 * processors.
 *
 * This file is a template. It is included through nds9_cpu_impl.h with
 * NDS_ARCH 5 (the ARM9, ARMv5TE) and through nds7_cpu_impl.h with
 * NDS_ARCH 4 (the ARM7, ARMv4T), and defines the same handlers under the
 * prefixes nds9_ and nds7_. NDS_V5 is a compile-time constant, so each copy
 * contains only its own architecture's behaviour, and each calls its own
 * bus (nds9_bus_* or nds7_bus_*).
 *
 * These are the only implementation of instruction semantics. The
 * interpreter (nds_cpu_exec.inc) calls them after decoding; recompiled code
 * calls them directly with a constant opcode, and because they are
 * force-inlined the C compiler folds the field extraction and the branches
 * on opcode bits away. The vector harness validates them through the
 * interpreter.
 *
 * The ARMv4T behaviour is the Game Boy Advance core's, which passes the
 * ARM7TDMI SingleStepTests. Where the ARM9 differs, the source is named at
 * the difference: GBATEK, the hardware-verified rockwrestler tests, or
 * melonDS's behaviour where neither says.
 *
 * Generated projects may define NDS_RC_OUTLINE_HANDLERS: each generated
 * file then calls one out-of-line copy of each handler instead of folding
 * it in.
 *
 * Contract: a handler executes one instruction whose condition already
 * passed, with r[15] = instruction address + 8 (ARM) or + 4 (Thumb). It
 * leaves r[15] unchanged unless it branches, in which case cpu->branched is
 * set; the caller advances r[15] otherwise.
 */

#ifndef NDS_ARCH
#error "include nds9_cpu_impl.h or nds7_cpu_impl.h, not this file"
#endif

#include "nds_bus.h"
#include "nds_cpu_ops.h"

#include "nds_cpu_arch.inc"

#ifndef NDS_OP_INLINE
#if defined(_MSC_VER)
#define NDS_OP_INLINE static __forceinline
#else
#define NDS_OP_INLINE static inline __attribute__((always_inline))
#endif
#if defined(NDS_RC_OUTLINE_HANDLERS) && defined(_MSC_VER)
#define NDS_OP static __declspec(noinline)
#elif defined(NDS_RC_OUTLINE_HANDLERS)
#define NDS_OP static __attribute__((noinline, unused))
#else
#define NDS_OP NDS_OP_INLINE
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Out-of-line operations (nds_cpu_exec.inc)                           */
/* ------------------------------------------------------------------ */

/* LDM/STM, also used for Thumb PUSH/POP/LDMIA/STMIA. pc_store_bias is
 * added to r[15] when R15 is stored: 4 in ARM state (instruction + 12),
 * 2 in Thumb state. thumb is set by the Thumb callers: the ARM7 aligns
 * an R15 they load and stays in Thumb state. */
void NDS_FN(op_block_transfer)(NdsCpu *c, uint32_t rn, uint32_t rlist, bool pre, bool up, bool s,
                               bool writeback, bool load, uint32_t pc_store_bias, bool thumb);

/* Exception entry from either state. */
void NDS_FN(op_undefined)(NdsCpu *c);
void NDS_FN(op_swi)(NdsCpu *c);
#if NDS_V5
void NDS_FN(op_prefetch_abort)(NdsCpu *c);
#endif

/* ------------------------------------------------------------------ */
/* Writes to R15                                                       */
/* ------------------------------------------------------------------ */

/* Write R15 and mark the pipeline for a refill in the current instruction
 * set. On the ARM7 the target is not aligned: the fetch unit ignores the
 * low address bits but R15 keeps them, so callers align only where the
 * instruction itself does. The ARM9 always holds an aligned R15. */
NDS_OP_INLINE void NDS_FN(branch)(NdsCpu *c, uint32_t target)
{
#if NDS_V5
    target &= (c->cpsr & NDS_CPSR_T) ? ~1u : ~3u;
#endif
    c->r[15] = target + ((c->cpsr & NDS_CPSR_T) ? 4u : 8u);
    c->branched = true;
}

/* Branch and exchange: bit 0 of the target selects the instruction set. */
NDS_OP_INLINE void NDS_FN(branch_exchange)(NdsCpu *c, uint32_t target)
{
    if (target & 1u) c->cpsr |= NDS_CPSR_T;
    else c->cpsr &= ~NDS_CPSR_T;
    NDS_FN(branch)(c, target & ~1u);
}

#if NDS_V5
/* A word loaded into R15 by LDR, LDM or POP. From ARMv5 on, bit 0 selects
 * the instruction set, unless CP15 control bit 15 asks for the ARMv4
 * behaviour (GBATEK, "CP15 control register"). */
NDS_OP_INLINE void NDS_FN(branch_loaded)(NdsCpu *c, uint32_t value)
{
    if (c->cp15.control & NDS_CP15_PRE_ARMV5) NDS_FN(branch)(c, value & ~1u);
    else NDS_FN(branch_exchange)(c, value);
}
#endif

/* ------------------------------------------------------------------ */
/* Loads                                                               */
/* ------------------------------------------------------------------ */

/* LDR/SWP: the word at the aligned address, rotated so the addressed byte
 * lands in bits 0-7. Both processors do this. */
NDS_OP_INLINE uint32_t NDS_FN(load_word)(NdsCpu *c, uint32_t addr, int access)
{
    uint32_t value = NDS_FN(bus_read32)(c, addr, access);
    return nds_ror32(value, (addr & 3u) * 8u);
}

/* LDRH from an odd address. The ARM7TDMI rotates the aligned halfword by
 * 8; the ARM9 just reads the aligned halfword (GBATEK, "ARM opcodes:
 * memory: halfword, doubleword, and signed data transfer"). */
NDS_OP_INLINE uint32_t NDS_FN(load_half)(NdsCpu *c, uint32_t addr, int access)
{
    uint32_t value = NDS_FN(bus_read16)(c, addr, access);
#if NDS_V5
    return value;
#else
    return nds_ror32(value, (addr & 1u) * 8u);
#endif
}

/* LDRSH from an odd address. The ARM7TDMI sign-extends only the addressed
 * (high) byte; the ARM9 sign-extends the aligned halfword. */
NDS_OP_INLINE uint32_t NDS_FN(load_signed_half)(NdsCpu *c, uint32_t addr, int access)
{
    uint32_t value = NDS_FN(bus_read16)(c, addr, access);
#if !NDS_V5
    if (addr & 1u) return (uint32_t)(int32_t)(int8_t)(uint8_t)(value >> 8);
#endif
    return (uint32_t)(int32_t)(int16_t)(uint16_t)value;
}

NDS_OP_INLINE uint32_t NDS_FN(load_signed_byte)(NdsCpu *c, uint32_t addr, int access)
{
    return (uint32_t)(int32_t)(int8_t)NDS_FN(bus_read8)(c, addr, access);
}

/* ------------------------------------------------------------------ */
/* Data processing                                                     */
/* ------------------------------------------------------------------ */

NDS_OP void NDS_FN(arm_dp)(NdsCpu *c, uint32_t op)
{
    const bool s = (op >> 20) & 1u;
    const uint32_t opcode = (op >> 21) & 15u;
    const uint32_t rn = (op >> 16) & 15u;
    const uint32_t rd = (op >> 12) & 15u;

    uint8_t carry = c->flag_c;
    uint32_t op1 = c->r[rn];
    uint32_t op2;

    if (op & (1u << 25)) {
        op2 = nds_rotated_imm(op & 0xFFu, (op >> 8) & 15u, &carry);
    } else {
        const uint32_t type = (op >> 5) & 3u;
        const uint32_t rm = op & 15u;
        uint32_t rm_value = c->r[rm];
        if (op & 0x10u) {
            /* Register-specified shift: one internal cycle, during which
             * the PC has advanced another word. The ARM7TDMI shows that
             * in both operands; for the ARM9 (where R15 here is
             * unpredictable) melonDS has it in Rm only. */
            uint32_t amount = c->r[(op >> 8) & 15u];
            NDS_FN(bus_idle)(c, 1);
#if !NDS_V5
            if (rn == 15) op1 += 4;
#endif
            if (rm == 15) rm_value += 4;
            op2 = nds_shift_reg(type, rm_value, amount, &carry);
        } else {
            op2 = nds_shift_imm(type, rm_value, (op >> 7) & 31u, &carry);
        }
    }

    uint32_t result = 0;
    bool write = true;
    switch (opcode) {
    case 0x0: result = nds_logic(c, op1 & op2, carry, s); break;          /* AND */
    case 0x1: result = nds_logic(c, op1 ^ op2, carry, s); break;          /* EOR */
    case 0x2: result = nds_sub(c, op1, op2, s); break;                    /* SUB */
    case 0x3: result = nds_sub(c, op2, op1, s); break;                    /* RSB */
    case 0x4: result = nds_add(c, op1, op2, s); break;                    /* ADD */
    case 0x5: result = nds_adc(c, op1, op2, s); break;                    /* ADC */
    case 0x6: result = nds_sbc(c, op1, op2, s); break;                    /* SBC */
    case 0x7: result = nds_sbc(c, op2, op1, s); break;                    /* RSC */
    case 0x8: nds_logic(c, op1 & op2, carry, true); write = false; break; /* TST */
    case 0x9: nds_logic(c, op1 ^ op2, carry, true); write = false; break; /* TEQ */
    case 0xA: nds_sub(c, op1, op2, true); write = false; break;           /* CMP */
    case 0xB: nds_add(c, op1, op2, true); write = false; break;           /* CMN */
    case 0xC: result = nds_logic(c, op1 | op2, carry, s); break;          /* ORR */
    case 0xD: result = nds_logic(c, op2, carry, s); break;                /* MOV */
    case 0xE: result = nds_logic(c, op1 & ~op2, carry, s); break;         /* BIC */
    default: result = nds_logic(c, ~op2, carry, s); break;                /* MVN */
    }

    if (write) c->r[rd] = result;
    if (rd == 15) {
        /* S with Rd = R15 restores CPSR from SPSR (exception return),
         * and the instruction set is then the one SPSR names. Without S
         * the instruction set does not change, on ARMv5 either. */
        if (s) nds_cpu_set_cpsr(c, nds_cpu_read_spsr(c));
        if (write) NDS_FN(branch)(c, result);
    }
}

/* ------------------------------------------------------------------ */
/* PSR transfer                                                        */
/* ------------------------------------------------------------------ */

NDS_OP void NDS_FN(arm_mrs)(NdsCpu *c, uint32_t op)
{
    const uint32_t rd = (op >> 12) & 15u;
    c->r[rd] = (op & (1u << 22)) ? nds_cpu_read_spsr(c) : nds_cpu_get_cpsr(c);
}

NDS_OP void NDS_FN(arm_msr)(NdsCpu *c, uint32_t op)
{
    uint32_t value;
    if (op & (1u << 25)) {
        uint8_t unused = 0;
        value = nds_rotated_imm(op & 0xFFu, (op >> 8) & 15u, &unused);
    } else {
        value = c->r[op & 15u];
    }

    uint32_t mask = 0;
    if (op & (1u << 16)) mask |= 0x000000FFu;
    if (op & (1u << 17)) mask |= 0x0000FF00u;
    if (op & (1u << 18)) mask |= 0x00FF0000u;
    if (op & (1u << 19)) mask |= 0xFF000000u;

    if (op & (1u << 22)) {
        if (nds_cpu_has_spsr(c)) {
            uint32_t *spsr = nds_cpu_current_spsr(c);
            *spsr = (*spsr & ~mask) | (value & mask);
        }
        return;
    }

    if (nds_cpu_mode(c) == NDS_MODE_USR) mask &= 0xFF000000u;
    /* Mode bit 4 is hardwired to 1: the 26-bit modes do not exist. */
    if (mask & 0xFFu) value |= 0x10u;
#if NDS_V5
    /* MSR cannot change the instruction set on ARMv5. */
    mask &= ~NDS_CPSR_T;
#endif
    uint32_t cpsr = nds_cpu_get_cpsr(c);
    nds_cpu_set_cpsr(c, (cpsr & ~mask) | (value & mask));
}

/* ------------------------------------------------------------------ */
/* Multiply                                                            */
/* ------------------------------------------------------------------ */

/* On the ARM7TDMI, registers read after the first cycle see R15 one word
 * further on. On the ARM9 R15 is unpredictable in these instructions and
 * is read as it stands. */
NDS_OP_INLINE uint32_t NDS_FN(late_reg)(const NdsCpu *c, uint32_t reg)
{
#if NDS_V5
    return c->r[reg];
#else
    return c->r[reg] + (reg == 15 ? 4u : 0u);
#endif
}

NDS_OP void NDS_FN(arm_mul)(NdsCpu *c, uint32_t op)
{
    const bool s = (op >> 20) & 1u;
    const bool accumulate = (op >> 21) & 1u;
    const uint32_t rd = (op >> 16) & 15u;
    const uint32_t rn = (op >> 12) & 15u;
    const uint32_t rs = (op >> 8) & 15u;
    const uint32_t rm = op & 15u;

    uint32_t multiplicand = NDS_FN(late_reg)(c, rm);
    uint32_t multiplier = NDS_FN(late_reg)(c, rs);
    uint32_t acc = accumulate ? NDS_FN(late_reg)(c, rn) : 0u;
    uint32_t result = multiplicand * multiplier + acc;

#if NDS_V5
    /* The ARM9's multiplier takes a fixed time, and leaves C alone
     * (GBATEK: "carry flag: ARMv5 and up: not affected"). */
    NDS_FN(bus_idle)(c, s ? 3 : 1);
    if (s) nds_set_nz(c, result);
#else
    NDS_FN(bus_idle)(c, nds_mul_cycles(multiplier, true) + (accumulate ? 1 : 0));
    if (s) {
        nds_set_nz(c, result);
        c->flag_c = nds_mul_carry(NDS_MUL_SHORT, multiplicand, multiplier, acc);
    }
#endif
    if (rd == 15) NDS_FN(branch)(c, result);
    else c->r[rd] = result;
}

NDS_OP void NDS_FN(arm_mull)(NdsCpu *c, uint32_t op)
{
    const bool s = (op >> 20) & 1u;
    const bool accumulate = (op >> 21) & 1u;
    const bool is_signed = (op >> 22) & 1u;
    const uint32_t rdhi = (op >> 16) & 15u;
    const uint32_t rdlo = (op >> 12) & 15u;
    const uint32_t rs = (op >> 8) & 15u;
    const uint32_t rm = op & 15u;

    uint32_t multiplicand = NDS_FN(late_reg)(c, rm);
    uint32_t multiplier = NDS_FN(late_reg)(c, rs);
    uint64_t acc = accumulate ? (((uint64_t)NDS_FN(late_reg)(c, rdhi) << 32) | NDS_FN(late_reg)(c, rdlo)) : 0u;
    uint64_t result;
    if (is_signed) {
        result = (uint64_t)((int64_t)(int32_t)multiplicand * (int64_t)(int32_t)multiplier);
    } else {
        result = (uint64_t)multiplicand * multiplier;
    }
    result += acc;

#if NDS_V5
    NDS_FN(bus_idle)(c, s ? 3 : 1);
    if (s) {
        c->flag_n = (uint8_t)(result >> 63);
        c->flag_z = (uint8_t)(result == 0);
    }
#else
    NDS_FN(bus_idle)(c, nds_mul_cycles(multiplier, is_signed) + 1 + (accumulate ? 1 : 0));
    if (s) {
        c->flag_n = (uint8_t)(result >> 63);
        c->flag_z = (uint8_t)(result == 0);
        c->flag_c = nds_mul_carry(is_signed ? NDS_MUL_LONG_SIGNED : NDS_MUL_LONG_UNSIGNED,
                                  multiplicand, multiplier, acc);
    }
#endif
    c->r[rdlo] = (uint32_t)result;
    c->r[rdhi] = (uint32_t)(result >> 32);
    if (rdlo == 15 || rdhi == 15) NDS_FN(branch)(c, c->r[15]);
}

/* ------------------------------------------------------------------ */
/* Loads and stores                                                    */
/* ------------------------------------------------------------------ */

NDS_OP void NDS_FN(arm_swp)(NdsCpu *c, uint32_t op)
{
    const bool byte = (op >> 22) & 1u;
    const uint32_t rn = (op >> 16) & 15u;
    const uint32_t rd = (op >> 12) & 15u;
    const uint32_t rm = op & 15u;
    uint32_t addr = NDS_FN(late_reg)(c, rn);
    uint32_t source = NDS_FN(late_reg)(c, rm);
    uint32_t loaded;

    if (byte) {
        loaded = NDS_FN(bus_read8)(c, addr, NDS_ACCESS_NSEQ);
        NDS_FN(bus_write8)(c, addr, (uint8_t)source, NDS_ACCESS_NSEQ | NDS_ACCESS_LOCK);
    } else {
        loaded = NDS_FN(load_word)(c, addr, NDS_ACCESS_NSEQ);
        NDS_FN(bus_write32)(c, addr, source, NDS_ACCESS_NSEQ | NDS_ACCESS_LOCK);
    }
    NDS_FN(bus_idle)(c, 1);
    if (rd == 15) NDS_FN(branch)(c, loaded);
    else c->r[rd] = loaded;
}

/* Base writeback for LDR/STR/LDRH/STRH. Writing back into R15 is an
 * unpredictable encoding; the ARM7TDMI re-reads the base a word later and
 * refills the pipeline. */
NDS_OP_INLINE void NDS_FN(transfer_writeback)(NdsCpu *c, uint32_t rn, uint32_t value)
{
#if NDS_V5
    if (rn == 15) NDS_FN(branch)(c, value);
#else
    if (rn == 15) NDS_FN(branch)(c, value + 4u);
#endif
    else c->r[rn] = value;
}

NDS_OP void NDS_FN(arm_sdt)(NdsCpu *c, uint32_t op)
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
        offset = nds_shift_imm((op >> 5) & 3u, c->r[op & 15u], (op >> 7) & 31u, &unused);
    } else {
        offset = op & 0xFFFu;
    }

    uint32_t base = c->r[rn];
    uint32_t moved = up ? base + offset : base - offset;
    uint32_t addr = pre ? moved : base;

    if (load) {
        uint32_t value = byte ? NDS_FN(bus_read8)(c, addr, NDS_ACCESS_NSEQ)
                              : NDS_FN(load_word)(c, addr, NDS_ACCESS_NSEQ);
        NDS_FN(bus_idle)(c, 1);
        if (!pre || writeback) NDS_FN(transfer_writeback)(c, rn, moved);
        if (rd != 15) {
            c->r[rd] = value;
        } else {
#if NDS_V5
            /* LDR into R15 can change the instruction set; LDRB into
             * R15 is unpredictable and stays put. */
            if (byte) NDS_FN(branch)(c, value);
            else NDS_FN(branch_loaded)(c, value);
#else
            NDS_FN(branch)(c, value);
#endif
        }
    } else {
        uint32_t value = c->r[rd] + (rd == 15 ? 4u : 0u);
        if (byte) NDS_FN(bus_write8)(c, addr, (uint8_t)value, NDS_ACCESS_NSEQ);
        else NDS_FN(bus_write32)(c, addr, value, NDS_ACCESS_NSEQ);
        if (!pre || writeback) NDS_FN(transfer_writeback)(c, rn, moved);
    }
}

NDS_OP void NDS_FN(arm_half)(NdsCpu *c, uint32_t op)
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
        case 1: value = NDS_FN(load_half)(c, addr, NDS_ACCESS_NSEQ); break;
        case 2: value = NDS_FN(load_signed_byte)(c, addr, NDS_ACCESS_NSEQ); break;
        default: value = NDS_FN(load_signed_half)(c, addr, NDS_ACCESS_NSEQ); break;
        }
        NDS_FN(bus_idle)(c, 1);
        if (!pre || writeback) NDS_FN(transfer_writeback)(c, rn, moved);
        if (rd == 15) NDS_FN(branch)(c, value);
        else c->r[rd] = value;
    } else {
        /* Only STRH exists on ARMv4; the signed forms with L=0 are the
         * ARMv5 LDRD/STRD encodings (nds9_arm_double), and the ARM7TDMI
         * does nothing with them beyond writeback. */
        if (kind == 1) {
            uint32_t value = c->r[rd] + (rd == 15 ? 4u : 0u);
            NDS_FN(bus_write16)(c, addr, (uint16_t)value, NDS_ACCESS_NSEQ);
        }
        if (!pre || writeback) NDS_FN(transfer_writeback)(c, rn, moved);
    }
}

NDS_OP void NDS_FN(arm_block)(NdsCpu *c, uint32_t op)
{
    NDS_FN(op_block_transfer)(c, (op >> 16) & 15u, op & 0xFFFFu, (op >> 24) & 1u, (op >> 23) & 1u,
                              (op >> 22) & 1u, (op >> 21) & 1u, (op >> 20) & 1u, 4u, false);
}

/* ------------------------------------------------------------------ */
/* Branches and exceptions                                             */
/* ------------------------------------------------------------------ */

NDS_OP void NDS_FN(arm_b)(NdsCpu *c, uint32_t op)
{
    uint32_t offset = (uint32_t)((int32_t)(op << 8) >> 6);
    if (op & (1u << 24)) c->r[14] = c->r[15] - 4u;
    NDS_FN(branch)(c, c->r[15] + offset);
}

NDS_OP void NDS_FN(arm_bx)(NdsCpu *c, uint32_t op)
{
    NDS_FN(branch_exchange)(c, c->r[op & 15u]);
}

/* ------------------------------------------------------------------ */
/* ARMv5TE additions                                                   */
/* ------------------------------------------------------------------ */

#if NDS_V5

/* BLX Rm. The target is read before LR is written, so BLX LR works. */
NDS_OP void NDS_FN(arm_blx_reg)(NdsCpu *c, uint32_t op)
{
    uint32_t target = c->r[op & 15u];
    uint32_t link = c->r[15] - 4u;
    NDS_FN(branch_exchange)(c, target);
    c->r[14] = link;
}

/* BLX label: always to Thumb. Bit 24 (H) supplies bit 1 of the target. */
NDS_OP void NDS_FN(arm_blx_imm)(NdsCpu *c, uint32_t op)
{
    uint32_t offset = (uint32_t)((int32_t)(op << 8) >> 6) + ((op >> 23) & 2u);
    uint32_t target = c->r[15] + offset;
    c->r[14] = c->r[15] - 4u;
    c->cpsr |= NDS_CPSR_T;
    NDS_FN(branch)(c, target);
}

NDS_OP void NDS_FN(arm_clz)(NdsCpu *c, uint32_t op)
{
    const uint32_t rd = (op >> 12) & 15u;
    uint32_t result = nds_clz32(c->r[op & 15u]);
    if (rd == 15) NDS_FN(branch)(c, result);
    else c->r[rd] = result;
}

/* QADD, QSUB, QDADD, QDSUB: Rd = sat(Rm +- [sat(2 *)] Rn). */
NDS_OP void NDS_FN(arm_qarith)(NdsCpu *c, uint32_t op)
{
    const bool doubled = (op >> 22) & 1u;
    const bool subtract = (op >> 21) & 1u;
    const uint32_t rn = (op >> 16) & 15u;
    const uint32_t rd = (op >> 12) & 15u;
    const uint32_t rm = op & 15u;

    uint32_t a = c->r[rm];
    uint32_t b = c->r[rn];
    if (doubled) b = nds_sat_add(c, b, b);
    uint32_t result = subtract ? nds_sat_sub(c, a, b) : nds_sat_add(c, a, b);
    if (rd == 15) NDS_FN(branch)(c, result);
    else c->r[rd] = result;
}

/* The signed 16-bit multiplies. x and y pick the top or bottom half of Rm
 * and Rs. Only the accumulating 32-bit forms can set Q, and none of them
 * saturates or touches NZCV. */
NDS_OP void NDS_FN(arm_dsp_mul)(NdsCpu *c, uint32_t op)
{
    const uint32_t kind = (op >> 21) & 3u;
    const uint32_t rd = (op >> 16) & 15u;
    const uint32_t rn = (op >> 12) & 15u;
    const uint32_t rs = (op >> 8) & 15u;
    const uint32_t rm = op & 15u;
    const bool x = (op >> 5) & 1u;
    const bool y = (op >> 6) & 1u;

    int32_t rs_half = (int16_t)(uint16_t)(y ? c->r[rs] >> 16 : c->r[rs]);
    int32_t rm_half = (int16_t)(uint16_t)(x ? c->r[rm] >> 16 : c->r[rm]);
    uint32_t result;

    switch (kind) {
    case 0: /* SMLAxy: Rd = Rm.x * Rs.y + Rn */
        result = nds_add_set_q(c, (uint32_t)(rm_half * rs_half), c->r[rn]);
        break;
    case 1: { /* SMLAWy (x = 0): Rd = (Rm * Rs.y) >> 16 + Rn; SMULWy (x = 1): no Rn */
        uint32_t product = (uint32_t)(((int64_t)(int32_t)c->r[rm] * rs_half) >> 16);
        result = x ? product : nds_add_set_q(c, product, c->r[rn]);
        break;
    }
    case 2: { /* SMLALxy: RdHi:RdLo += Rm.x * Rs.y */
        uint64_t acc = ((uint64_t)c->r[rd] << 32) | c->r[rn];
        acc += (uint64_t)(int64_t)(rm_half * rs_half);
        NDS_FN(bus_idle)(c, 1);
        c->r[rn] = (uint32_t)acc;
        c->r[rd] = (uint32_t)(acc >> 32);
        if (rd == 15 || rn == 15) NDS_FN(branch)(c, c->r[15]);
        return;
    }
    default: /* SMULxy: Rd = Rm.x * Rs.y */
        result = (uint32_t)(rm_half * rs_half);
        break;
    }
    if (rd == 15) NDS_FN(branch)(c, result);
    else c->r[rd] = result;
}

/* LDRD/STRD: two words to or from Rd and Rd+1. Rd must be even; an odd Rd
 * is undefined. The base is written back before the loads, so a loaded
 * register wins over the writeback. */
NDS_OP void NDS_FN(arm_double)(NdsCpu *c, uint32_t op)
{
    const bool pre = (op >> 24) & 1u;
    const bool up = (op >> 23) & 1u;
    const bool immediate = (op >> 22) & 1u;
    const bool writeback = (op >> 21) & 1u;
    const bool store = (op >> 5) & 1u;
    const uint32_t rn = (op >> 16) & 15u;
    const uint32_t rd = (op >> 12) & 15u;

    if (rd & 1u) {
        NDS_FN(op_undefined)(c);
        return;
    }

    uint32_t offset = immediate ? (((op >> 4) & 0xF0u) | (op & 0xFu)) : c->r[op & 15u];
    uint32_t base = c->r[rn];
    uint32_t moved = up ? base + offset : base - offset;
    uint32_t addr = pre ? moved : base;

    if (store) {
        uint32_t low = c->r[rd];
        uint32_t high = c->r[rd + 1u] + (rd == 14 ? 4u : 0u);
        NDS_FN(bus_write32)(c, addr, low, NDS_ACCESS_NSEQ);
        NDS_FN(bus_write32)(c, addr + 4u, high, NDS_ACCESS_SEQ);
        if (!pre || writeback) NDS_FN(transfer_writeback)(c, rn, moved);
    } else {
        if (!pre || writeback) NDS_FN(transfer_writeback)(c, rn, moved);
        uint32_t low = NDS_FN(bus_read32)(c, addr, NDS_ACCESS_NSEQ);
        uint32_t high = NDS_FN(bus_read32)(c, addr + 4u, NDS_ACCESS_SEQ);
        NDS_FN(bus_idle)(c, 1);
        c->r[rd] = low;
        if (rd == 14) NDS_FN(branch)(c, high);
        else c->r[rd + 1u] = high;
    }
}

/* MRC/MCR. Only CP15 exists, and only privileged code may use it. MRC with
 * Rd = R15 writes the top four bits to NZCV. */
NDS_OP void NDS_FN(arm_cp_reg)(NdsCpu *c, uint32_t op)
{
    const uint32_t rd = (op >> 12) & 15u;
    const uint32_t id = ((op >> 8) & 0xF00u) | ((op & 15u) << 4) | ((op >> 5) & 7u);

    if (((op >> 8) & 15u) != 15u || nds_cpu_mode(c) == NDS_MODE_USR) {
        NDS_FN(op_undefined)(c);
        return;
    }
    if (op & (1u << 20)) {
        uint32_t value = nds9_cp15_read(c, id);
        if (rd == 15) {
            c->flag_n = (uint8_t)((value >> 31) & 1u);
            c->flag_z = (uint8_t)((value >> 30) & 1u);
            c->flag_c = (uint8_t)((value >> 29) & 1u);
            c->flag_v = (uint8_t)((value >> 28) & 1u);
        } else {
            c->r[rd] = value;
        }
    } else {
        nds9_cp15_write(c, id, c->r[rd] + (rd == 15 ? 4u : 0u));
    }
}

#endif /* NDS_V5 */

/* ------------------------------------------------------------------ */
/* Thumb                                                               */
/* ------------------------------------------------------------------ */

/* Format 1: LSL/LSR/ASR Rd, Rs, #imm5. */
NDS_OP void NDS_FN(thumb_shift_imm)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t rs = (op >> 3) & 7u;
    uint8_t carry = c->flag_c;
    uint32_t result = nds_shift_imm((op >> 11) & 3u, c->r[rs], (op >> 6) & 31u, &carry);
    c->r[rd] = nds_logic(c, result, carry, true);
}

/* Format 2: ADD/SUB Rd, Rs, Rn|#imm3. */
NDS_OP void NDS_FN(thumb_add_sub)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t rs = (op >> 3) & 7u;
    uint32_t field = (op >> 6) & 7u;
    uint32_t operand = (op & (1u << 10)) ? field : c->r[field];
    if (op & (1u << 9)) c->r[rd] = nds_sub(c, c->r[rs], operand, true);
    else c->r[rd] = nds_add(c, c->r[rs], operand, true);
}

/* Format 3: MOV/CMP/ADD/SUB Rd, #imm8. */
NDS_OP void NDS_FN(thumb_imm8)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = (op >> 8) & 7u;
    uint32_t imm = op & 0xFFu;
    switch ((op >> 11) & 3u) {
    case 0: c->r[rd] = imm; nds_set_nz(c, imm); break;
    case 1: nds_sub(c, c->r[rd], imm, true); break;
    case 2: c->r[rd] = nds_add(c, c->r[rd], imm, true); break;
    default: c->r[rd] = nds_sub(c, c->r[rd], imm, true); break;
    }
}

/* Format 4: ALU operations on low registers. */
NDS_OP void NDS_FN(thumb_alu)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t rs = (op >> 3) & 7u;
    uint32_t a = c->r[rd];
    uint32_t b = c->r[rs];
    uint8_t carry = c->flag_c;
    uint32_t shifted;

    /* The shifts write carry through a pointer, so each is a statement of
     * its own: as an argument beside `carry` the order would be the
     * compiler's choice, and gcc and MSVC choose differently. */
    switch ((op >> 6) & 15u) {
    case 0x0: c->r[rd] = nds_logic(c, a & b, carry, true); break; /* AND */
    case 0x1: c->r[rd] = nds_logic(c, a ^ b, carry, true); break; /* EOR */
    case 0x2: /* LSL */
        NDS_FN(bus_idle)(c, 1);
        shifted = nds_shift_reg(NDS_SHIFT_LSL, a, b, &carry);
        c->r[rd] = nds_logic(c, shifted, carry, true);
        break;
    case 0x3: /* LSR */
        NDS_FN(bus_idle)(c, 1);
        shifted = nds_shift_reg(NDS_SHIFT_LSR, a, b, &carry);
        c->r[rd] = nds_logic(c, shifted, carry, true);
        break;
    case 0x4: /* ASR */
        NDS_FN(bus_idle)(c, 1);
        shifted = nds_shift_reg(NDS_SHIFT_ASR, a, b, &carry);
        c->r[rd] = nds_logic(c, shifted, carry, true);
        break;
    case 0x5: c->r[rd] = nds_adc(c, a, b, true); break;
    case 0x6: c->r[rd] = nds_sbc(c, a, b, true); break;
    case 0x7: /* ROR */
        NDS_FN(bus_idle)(c, 1);
        shifted = nds_shift_reg(NDS_SHIFT_ROR, a, b, &carry);
        c->r[rd] = nds_logic(c, shifted, carry, true);
        break;
    case 0x8: nds_logic(c, a & b, carry, true); break;            /* TST */
    case 0x9: c->r[rd] = nds_sub(c, 0, b, true); break;           /* NEG */
    case 0xA: nds_sub(c, a, b, true); break;                      /* CMP */
    case 0xB: nds_add(c, a, b, true); break;                      /* CMN */
    case 0xC: c->r[rd] = nds_logic(c, a | b, carry, true); break; /* ORR */
    case 0xD: {                                                   /* MUL */
        /* MULS Rd, Rs, Rd: Rs is the multiplicand, Rd the multiplier. */
        uint32_t result = b * a;
#if NDS_V5
        NDS_FN(bus_idle)(c, 3);
        nds_set_nz(c, result);
#else
        NDS_FN(bus_idle)(c, nds_mul_cycles(a, true));
        nds_set_nz(c, result);
        c->flag_c = nds_mul_carry(NDS_MUL_SHORT, b, a, 0);
#endif
        c->r[rd] = result;
        break;
    }
    case 0xE: c->r[rd] = nds_logic(c, a & ~b, carry, true); break; /* BIC */
    default: c->r[rd] = nds_logic(c, ~b, carry, true); break;      /* MVN */
    }
}

/* Format 5: ADD/CMP/MOV on high registers, BX, and (ARMv5) BLX. None of
 * ADD, CMP or MOV changes the instruction set. */
NDS_OP void NDS_FN(thumb_hi_reg)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = (op & 7u) | ((op >> 4) & 8u);
    uint32_t rs = (op >> 3) & 15u;
    uint32_t value = c->r[rs];

    switch ((op >> 8) & 3u) {
    case 0: {
        uint32_t result = c->r[rd] + value;
        if (rd == 15) NDS_FN(branch)(c, result & ~1u);
        else c->r[rd] = result;
        break;
    }
    case 1:
        nds_sub(c, c->r[rd], value, true);
        break;
    case 2:
        if (rd == 15) NDS_FN(branch)(c, value & ~1u);
        else c->r[rd] = value;
        break;
    default: {
#if NDS_V5
        uint32_t link = (c->r[15] - 2u) | 1u;
        NDS_FN(branch_exchange)(c, value);
        if (op & 0x80u) c->r[14] = link; /* BLX Rs */
#else
        /* The ARM7TDMI ignores bit 7: the BLX encoding is a BX. */
        NDS_FN(branch_exchange)(c, value);
#endif
        break;
    }
    }
}

/* Format 6: LDR Rd, [PC, #imm8*4]. */
NDS_OP void NDS_FN(thumb_ldr_pc)(NdsCpu *c, uint16_t op)
{
    uint32_t addr = (c->r[15] & ~2u) + ((op & 0xFFu) << 2);
    c->r[(op >> 8) & 7u] = NDS_FN(load_word)(c, addr, NDS_ACCESS_NSEQ);
    NDS_FN(bus_idle)(c, 1);
}

/* Format 7: LDR/STR/LDRB/STRB Rd, [Rb, Ro]. */
NDS_OP void NDS_FN(thumb_ldst_reg)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t addr = c->r[(op >> 3) & 7u] + c->r[(op >> 6) & 7u];
    switch ((op >> 10) & 3u) {
    case 0: NDS_FN(bus_write32)(c, addr, c->r[rd], NDS_ACCESS_NSEQ); break;
    case 1: NDS_FN(bus_write8)(c, addr, (uint8_t)c->r[rd], NDS_ACCESS_NSEQ); break;
    case 2: c->r[rd] = NDS_FN(load_word)(c, addr, NDS_ACCESS_NSEQ); NDS_FN(bus_idle)(c, 1); break;
    default: c->r[rd] = NDS_FN(bus_read8)(c, addr, NDS_ACCESS_NSEQ); NDS_FN(bus_idle)(c, 1); break;
    }
}

/* Format 8: STRH/LDSB/LDRH/LDSH Rd, [Rb, Ro]. */
NDS_OP void NDS_FN(thumb_ldst_sign)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t addr = c->r[(op >> 3) & 7u] + c->r[(op >> 6) & 7u];
    switch ((op >> 10) & 3u) {
    case 0: NDS_FN(bus_write16)(c, addr, (uint16_t)c->r[rd], NDS_ACCESS_NSEQ); break;
    case 1: c->r[rd] = NDS_FN(load_signed_byte)(c, addr, NDS_ACCESS_NSEQ); NDS_FN(bus_idle)(c, 1); break;
    case 2: c->r[rd] = NDS_FN(load_half)(c, addr, NDS_ACCESS_NSEQ); NDS_FN(bus_idle)(c, 1); break;
    default: c->r[rd] = NDS_FN(load_signed_half)(c, addr, NDS_ACCESS_NSEQ); NDS_FN(bus_idle)(c, 1); break;
    }
}

/* Format 9: LDR/STR/LDRB/STRB Rd, [Rb, #imm5]. */
NDS_OP void NDS_FN(thumb_ldst_imm)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t base = c->r[(op >> 3) & 7u];
    uint32_t imm = (op >> 6) & 31u;
    switch ((op >> 11) & 3u) {
    case 0: NDS_FN(bus_write32)(c, base + (imm << 2), c->r[rd], NDS_ACCESS_NSEQ); break;
    case 1: c->r[rd] = NDS_FN(load_word)(c, base + (imm << 2), NDS_ACCESS_NSEQ); NDS_FN(bus_idle)(c, 1); break;
    case 2: NDS_FN(bus_write8)(c, base + imm, (uint8_t)c->r[rd], NDS_ACCESS_NSEQ); break;
    default: c->r[rd] = NDS_FN(bus_read8)(c, base + imm, NDS_ACCESS_NSEQ); NDS_FN(bus_idle)(c, 1); break;
    }
}

/* Format 10: LDRH/STRH Rd, [Rb, #imm5*2]. */
NDS_OP void NDS_FN(thumb_ldst_half)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = op & 7u;
    uint32_t addr = c->r[(op >> 3) & 7u] + (((op >> 6) & 31u) << 1);
    if (op & (1u << 11)) {
        c->r[rd] = NDS_FN(load_half)(c, addr, NDS_ACCESS_NSEQ);
        NDS_FN(bus_idle)(c, 1);
    } else {
        NDS_FN(bus_write16)(c, addr, (uint16_t)c->r[rd], NDS_ACCESS_NSEQ);
    }
}

/* Format 11: LDR/STR Rd, [SP, #imm8*4]. */
NDS_OP void NDS_FN(thumb_ldst_sp)(NdsCpu *c, uint16_t op)
{
    uint32_t rd = (op >> 8) & 7u;
    uint32_t addr = c->r[13] + ((op & 0xFFu) << 2);
    if (op & (1u << 11)) {
        c->r[rd] = NDS_FN(load_word)(c, addr, NDS_ACCESS_NSEQ);
        NDS_FN(bus_idle)(c, 1);
    } else {
        NDS_FN(bus_write32)(c, addr, c->r[rd], NDS_ACCESS_NSEQ);
    }
}

/* Format 12: ADD Rd, PC|SP, #imm8*4. */
NDS_OP void NDS_FN(thumb_add_pc_sp)(NdsCpu *c, uint16_t op)
{
    uint32_t imm = (op & 0xFFu) << 2;
    uint32_t base = (op & (1u << 11)) ? c->r[13] : (c->r[15] & ~2u);
    c->r[(op >> 8) & 7u] = base + imm;
}

/* Format 13: ADD SP, #+-imm7*4. */
NDS_OP void NDS_FN(thumb_add_sp)(NdsCpu *c, uint16_t op)
{
    uint32_t imm = (op & 0x7Fu) << 2;
    if (op & 0x80u) c->r[13] -= imm;
    else c->r[13] += imm;
}

/* Format 14: PUSH/POP {Rlist, LR|PC}. */
NDS_OP void NDS_FN(thumb_push_pop)(NdsCpu *c, uint16_t op)
{
    uint32_t rlist = op & 0xFFu;
    if (op & (1u << 11)) {
        if (op & 0x100u) rlist |= 1u << 15;
        NDS_FN(op_block_transfer)(c, 13, rlist, false, true, false, true, true, 2u, true);
    } else {
        if (op & 0x100u) rlist |= 1u << 14;
        NDS_FN(op_block_transfer)(c, 13, rlist, true, false, false, true, false, 2u, true);
    }
}

/* Format 15: LDMIA/STMIA Rb!, {Rlist}. */
NDS_OP void NDS_FN(thumb_ldm_stm)(NdsCpu *c, uint16_t op)
{
    NDS_FN(op_block_transfer)(c, (op >> 8) & 7u, op & 0xFFu, false, true, false, true,
                              (op >> 11) & 1u, 2u, true);
}

/* Format 16: conditional branch. Condition 15 is SWI and has its own
 * class. Condition 14 is undefined: on ARMv5 the decoder never sends it
 * here; the ARM7TDMI treats it as "always". */
NDS_OP void NDS_FN(thumb_bcc)(NdsCpu *c, uint16_t op)
{
    uint32_t cond = (op >> 8) & 15u;
    if (cond == 15) {
        NDS_FN(op_swi)(c);
    } else if (nds_cond(c, cond)) {
        uint32_t offset = (uint32_t)((int32_t)(int8_t)(op & 0xFFu) * 2);
        NDS_FN(branch)(c, c->r[15] + offset);
    }
}

/* Format 18: B #imm11. */
NDS_OP void NDS_FN(thumb_b)(NdsCpu *c, uint16_t op)
{
    uint32_t offset = (uint32_t)(((int32_t)((uint32_t)op << 21)) >> 20);
    NDS_FN(branch)(c, c->r[15] + offset);
}

/* Format 19: the prefix of BL/BLX, the BL suffix, and (ARMv5) the BLX
 * suffix, which goes to ARM state. */
NDS_OP void NDS_FN(thumb_bl)(NdsCpu *c, uint16_t op)
{
    uint32_t imm = op & 0x7FFu;
    if ((op & 0x1800u) == 0x1000u) {
        uint32_t offset = (uint32_t)(((int32_t)(imm << 21)) >> 9);
        c->r[14] = c->r[15] + offset;
        return;
    }
    uint32_t next = (c->r[15] - 2u) | 1u;
    uint32_t target = c->r[14] + (imm << 1);
#if NDS_V5
    if (!(op & 0x1000u)) {
        /* BLX: an odd offset is not a word address and is undefined. */
        if (op & 1u) {
            NDS_FN(op_undefined)(c);
            return;
        }
        c->cpsr &= ~NDS_CPSR_T;
        NDS_FN(branch)(c, target);
        c->r[14] = next;
        return;
    }
#endif
    NDS_FN(branch)(c, target & ~1u);
    c->r[14] = next;
}

#ifdef __cplusplus
}
#endif

#include "nds_cpu_arch_end.inc"
