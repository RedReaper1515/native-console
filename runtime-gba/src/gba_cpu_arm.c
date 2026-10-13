/*
 * ARM (32-bit) instruction interpreter for the ARM7TDMI: classify, then call
 * the handler from gba_cpu_impl.h. Also holds the out-of-line operations the
 * handlers share (block transfer, undefined instruction, SWI).
 */
#include "gba_cpu_decode.h"
#include "gba_cpu_impl.h"

/* ------------------------------------------------------------------ */
/* Block transfer (shared with Thumb PUSH/POP/LDMIA/STMIA)              */
/* ------------------------------------------------------------------ */

static void block_writeback(GbaCpu *c, uint32_t rn, uint32_t value, bool user_bank)
{
    if (rn == 15) gba_cpu_branch(c, value);
    else if (user_bank) gba_cpu_set_user_reg(c, (int)rn, value);
    else c->r[rn] = value;
}

void gba_op_block_transfer(GbaCpu *c, uint32_t rn, uint32_t rlist, bool pre, bool up, bool s,
                           bool writeback, bool load, uint32_t pc_store_bias, uint32_t pc_load_mask)
{
    uint32_t base = c->r[rn];
    uint32_t count = 0;
    for (uint32_t list = rlist; list; list &= list - 1) count++;

    /* ARM7TDMI quirk: an empty list transfers R15 and moves the base by
     * 0x40, as if all sixteen registers were listed. */
    uint32_t bytes = count ? count * 4u : 0x40u;
    if (rlist == 0) {
        rlist = 1u << 15;
        pc_load_mask = 0xFFFFFFFFu;
    }

    uint32_t addr;
    if (up) addr = pre ? base + 4u : base;
    else addr = pre ? base - bytes : base - bytes + 4u;
    uint32_t new_base = up ? base + bytes : base - bytes;

    /* With S set and no R15 load, the user bank is transferred, and the
     * reference implementation also writes the base back into the user
     * bank (the base itself is read from the current mode). */
    bool user_bank = s && !(load && (rlist & 0x8000u));
    int access = GBA_ACCESS_NSEQ;

    if (load) {
        /* Writeback happens after the first access, so a loaded base
         * register overrides the written-back value. */
        if (writeback) block_writeback(c, rn, new_base, user_bank);
        for (int reg = 0; reg < 16; reg++) {
            if (!(rlist & (1u << reg))) continue;
            uint32_t value = gba_bus_read32(c, addr, access);
            access = GBA_ACCESS_SEQ;
            addr += 4;
            if (reg == 15) {
                gba_bus_idle(c, 1);
                if (s) gba_cpu_set_cpsr(c, gba_cpu_read_spsr(c));
                gba_cpu_branch(c, value & pc_load_mask);
                return;
            }
            if (user_bank) gba_cpu_set_user_reg(c, reg, value);
            else c->r[reg] = value;
        }
        gba_bus_idle(c, 1);
    } else {
        bool first = true;
        for (int reg = 0; reg < 16; reg++) {
            if (!(rlist & (1u << reg))) continue;
            uint32_t value;
            /* The base is written back after the first store, so a base
             * register that is not first in the list stores the new base. */
            if ((uint32_t)reg == rn && writeback && !first) value = new_base;
            else if (reg == 15) value = c->r[15] + pc_store_bias;
            else value = user_bank ? gba_cpu_get_user_reg(c, reg) : c->r[reg];
            gba_bus_write32(c, addr, value, access);
            access = GBA_ACCESS_SEQ;
            addr += 4;
            first = false;
        }
        if (writeback) block_writeback(c, rn, new_base, user_bank);
    }
}

void gba_op_undefined(GbaCpu *c)
{
    uint32_t next = c->r[15] - ((c->cpsr & GBA_CPSR_T) ? 2u : 4u);
    gba_cpu_exception(c, GBA_VECTOR_UNDEFINED, GBA_MODE_UND, next);
}

void gba_op_swi(GbaCpu *c)
{
    uint32_t next = c->r[15] - ((c->cpsr & GBA_CPSR_T) ? 2u : 4u);
    gba_cpu_exception(c, GBA_VECTOR_SWI, GBA_MODE_SVC, next);
}

void gba_cpu_execute_arm(GbaCpu *c, uint32_t op)
{
    c->branched = false;
    if (!gba_cond(c, op >> 28)) {
        c->r[15] += 4;
        return;
    }
    switch (gba_arm_classify(op)) {
    case GBA_ARM_BX: gba_arm_bx(c, op); break;
    case GBA_ARM_SWP: gba_arm_swp(c, op); break;
    case GBA_ARM_MUL: gba_arm_mul(c, op); break;
    case GBA_ARM_MULL: gba_arm_mull(c, op); break;
    case GBA_ARM_HALF: gba_arm_half(c, op); break;
    case GBA_ARM_MRS: gba_arm_mrs(c, op); break;
    case GBA_ARM_MSR: gba_arm_msr(c, op); break;
    case GBA_ARM_DP: gba_arm_dp(c, op); break;
    case GBA_ARM_SDT: gba_arm_sdt(c, op); break;
    case GBA_ARM_BLOCK: gba_arm_block(c, op); break;
    case GBA_ARM_B: gba_arm_b(c, op); break;
    case GBA_ARM_SWI: gba_op_swi(c); break;
    default: gba_op_undefined(c); break;
    }
    if (!c->branched) c->r[15] += 4;
}
