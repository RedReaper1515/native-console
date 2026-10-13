/*
 * The interpreter for one DS processor: classify, then call the handler
 * from nds_cpu_impl.inc. Also the out-of-line operations the handlers share
 * (block transfer, exception entry), the fetch/execute step and IRQ entry.
 *
 * A template like nds_cpu_impl.inc: nds_cpu9.c includes it with NDS_ARCH 5
 * and nds_cpu7.c with NDS_ARCH 4.
 */
#include "nds_cpu_arch.inc"

/* ------------------------------------------------------------------ */
/* Block transfer (shared with Thumb PUSH/POP/LDMIA/STMIA)              */
/* ------------------------------------------------------------------ */

#if NDS_V5

/*
 * The ARM9's LDM/STM. Where it differs from the ARM7TDMI's (GBATEK, "ARM
 * opcodes: memory: block data transfer", and rockwrestler's LDM/STM tests,
 * which pass on hardware):
 *
 * - An empty register list transfers nothing, but the base still moves by
 *   0x40.
 * - STM always stores the old base.
 * - LDM with the base in the list writes the new base back, over the loaded
 *   value, when the base is the only register or is not the last one in the
 *   list. When it is the last of several, the loaded value stays. That is
 *   the ARM-state instruction: Thumb LDMIA with the base in its list never
 *   writes it back (GBATEK, THUMB.15), which picolibc's gmtime_r relies on
 *   with "ldmia r0!, {r0, r1}".
 * - Writeback goes to the current mode's base register even when the S bit
 *   transfers the user bank.
 * - A loaded R15 selects the instruction set by its bit 0.
 */
void nds9_op_block_transfer(NdsCpu *c, uint32_t rn, uint32_t rlist, bool pre, bool up, bool s,
                            bool writeback, bool load, uint32_t pc_store_bias, bool thumb)
{
    uint32_t base = c->r[rn];
    uint32_t count = 0;
    for (uint32_t list = rlist; list; list &= list - 1) count++;
    uint32_t bytes = count ? count * 4u : 0x40u;

    uint32_t addr;
    if (up) addr = pre ? base + 4u : base;
    else addr = pre ? base - bytes : base - bytes + 4u;
    uint32_t new_base = up ? base + bytes : base - bytes;

    /* With S set and no R15 load, the user bank is transferred. */
    bool user_bank = s && !(load && (rlist & 0x8000u));
    int access = NDS_ACCESS_NSEQ;

    if (load) {
        uint32_t pc_value = 0;
        for (int reg = 0; reg < 16; reg++) {
            if (!(rlist & (1u << reg))) continue;
            uint32_t value = nds9_bus_read32(c, addr, access);
            access = NDS_ACCESS_SEQ;
            addr += 4;
            if (reg == 15) pc_value = value;
            else if (user_bank) nds_cpu_set_user_reg(c, reg, value);
            else c->r[reg] = value;
        }
        if (count) nds9_bus_idle(c, 1);

        if (writeback) {
            bool base_listed = (rlist >> rn) & 1u;
            bool base_only = rlist == (1u << rn);
            bool base_last = (rlist >> rn) == 1u;
            if (!base_listed || (!thumb && (base_only || !base_last))) {
                if (rn == 15) nds9_branch(c, new_base);
                else c->r[rn] = new_base;
            }
        }
        if (rlist & 0x8000u) {
            if (s) {
                /* Exception return: the instruction set comes from SPSR. */
                nds_cpu_set_cpsr(c, nds_cpu_read_spsr(c));
                nds9_branch(c, pc_value);
            } else {
                nds9_branch_loaded(c, pc_value);
            }
        }
    } else {
        for (int reg = 0; reg < 16; reg++) {
            if (!(rlist & (1u << reg))) continue;
            uint32_t value;
            if (reg == 15) value = c->r[15] + pc_store_bias;
            else value = user_bank ? nds_cpu_get_user_reg(c, reg) : c->r[reg];
            nds9_bus_write32(c, addr, value, access);
            access = NDS_ACCESS_SEQ;
            addr += 4;
        }
        if (writeback) {
            if (rn == 15) nds9_branch(c, new_base);
            else c->r[rn] = new_base;
        }
    }
}

#else /* ARMv4T */

static void nds7_block_writeback(NdsCpu *c, uint32_t rn, uint32_t value, bool user_bank)
{
    if (rn == 15) nds7_branch(c, value);
    else if (user_bank) nds_cpu_set_user_reg(c, (int)rn, value);
    else c->r[rn] = value;
}

/* The ARM7TDMI's LDM/STM, as the Game Boy Advance core has it. */
void nds7_op_block_transfer(NdsCpu *c, uint32_t rn, uint32_t rlist, bool pre, bool up, bool s,
                            bool writeback, bool load, uint32_t pc_store_bias, bool thumb)
{
    /* Applied to a loaded R15: Thumb POP aligns, ARM LDM does not. */
    uint32_t pc_load_mask = thumb ? ~1u : 0xFFFFFFFFu;
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
    int access = NDS_ACCESS_NSEQ;

    if (load) {
        /* Writeback happens after the first access, so a loaded base
         * register overrides the written-back value. */
        if (writeback) nds7_block_writeback(c, rn, new_base, user_bank);
        for (int reg = 0; reg < 16; reg++) {
            if (!(rlist & (1u << reg))) continue;
            uint32_t value = nds7_bus_read32(c, addr, access);
            access = NDS_ACCESS_SEQ;
            addr += 4;
            if (reg == 15) {
                nds7_bus_idle(c, 1);
                if (s) nds_cpu_set_cpsr(c, nds_cpu_read_spsr(c));
                nds7_branch(c, value & pc_load_mask);
                return;
            }
            if (user_bank) nds_cpu_set_user_reg(c, reg, value);
            else c->r[reg] = value;
        }
        nds7_bus_idle(c, 1);
    } else {
        bool first = true;
        for (int reg = 0; reg < 16; reg++) {
            if (!(rlist & (1u << reg))) continue;
            uint32_t value;
            /* The base is written back after the first store, so a base
             * register that is not first in the list stores the new base. */
            if ((uint32_t)reg == rn && writeback && !first) value = new_base;
            else if (reg == 15) value = c->r[15] + pc_store_bias;
            else value = user_bank ? nds_cpu_get_user_reg(c, reg) : c->r[reg];
            nds7_bus_write32(c, addr, value, access);
            access = NDS_ACCESS_SEQ;
            addr += 4;
            first = false;
        }
        if (writeback) nds7_block_writeback(c, rn, new_base, user_bank);
    }
}

#endif

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */

void NDS_FN(op_undefined)(NdsCpu *c)
{
    uint32_t next = c->r[15] - ((c->cpsr & NDS_CPSR_T) ? 2u : 4u);
    nds_cpu_exception(c, NDS_VECTOR_UNDEFINED, NDS_MODE_UND, next);
}

void NDS_FN(op_swi)(NdsCpu *c)
{
    uint32_t next = c->r[15] - ((c->cpsr & NDS_CPSR_T) ? 2u : 4u);
    nds_cpu_exception(c, NDS_VECTOR_SWI, NDS_MODE_SVC, next);
}

#if NDS_V5
/* BKPT. LR_abt is the address of the instruction plus 4 in either state. */
void nds9_op_prefetch_abort(NdsCpu *c)
{
    uint32_t link = c->r[15] - ((c->cpsr & NDS_CPSR_T) ? 0u : 4u);
    nds_cpu_exception(c, NDS_VECTOR_PREFETCH_ABORT, NDS_MODE_ABT, link);
}
#endif

/* ------------------------------------------------------------------ */
/* Interpreter                                                         */
/* ------------------------------------------------------------------ */

void NDS_FN(cpu_execute_arm)(NdsCpu *c, uint32_t op)
{
    c->branched = false;
#if NDS_V5
    if ((op >> 28) == 15u) {
        /* Condition 1111 is the unconditional instruction space. */
        switch (nds_arm_classify(op, 1)) {
        case NDS_ARM_BLX_IMM: nds9_arm_blx_imm(c, op); break;
        case NDS_ARM_PLD: break;
        default: nds9_op_undefined(c); break;
        }
        if (!c->branched) c->r[15] += 4;
        return;
    }
#endif
    if (!nds_cond(c, op >> 28)) {
        c->r[15] += 4;
        return;
    }
    switch (nds_arm_classify(op, NDS_V5)) {
    case NDS_ARM_BX: NDS_FN(arm_bx)(c, op); break;
    case NDS_ARM_SWP: NDS_FN(arm_swp)(c, op); break;
    case NDS_ARM_MUL: NDS_FN(arm_mul)(c, op); break;
    case NDS_ARM_MULL: NDS_FN(arm_mull)(c, op); break;
    case NDS_ARM_HALF: NDS_FN(arm_half)(c, op); break;
    case NDS_ARM_MRS: NDS_FN(arm_mrs)(c, op); break;
    case NDS_ARM_MSR: NDS_FN(arm_msr)(c, op); break;
    case NDS_ARM_DP: NDS_FN(arm_dp)(c, op); break;
    case NDS_ARM_SDT: NDS_FN(arm_sdt)(c, op); break;
    case NDS_ARM_BLOCK: NDS_FN(arm_block)(c, op); break;
    case NDS_ARM_B: NDS_FN(arm_b)(c, op); break;
    case NDS_ARM_SWI: NDS_FN(op_swi)(c); break;
#if NDS_V5
    case NDS_ARM_BLX_REG: nds9_arm_blx_reg(c, op); break;
    case NDS_ARM_BKPT: nds9_op_prefetch_abort(c); break;
    case NDS_ARM_CLZ: nds9_arm_clz(c, op); break;
    case NDS_ARM_QARITH: nds9_arm_qarith(c, op); break;
    case NDS_ARM_DSP_MUL: nds9_arm_dsp_mul(c, op); break;
    case NDS_ARM_DOUBLE: nds9_arm_double(c, op); break;
    case NDS_ARM_CP_REG: nds9_arm_cp_reg(c, op); break;
#endif
    default: NDS_FN(op_undefined)(c); break;
    }
    if (!c->branched) c->r[15] += 4;
}

void NDS_FN(cpu_execute_thumb)(NdsCpu *c, uint16_t op)
{
    c->branched = false;
    switch (nds_thumb_classify(op, NDS_V5)) {
    case NDS_THUMB_SHIFT: NDS_FN(thumb_shift_imm)(c, op); break;
    case NDS_THUMB_ADD_SUB: NDS_FN(thumb_add_sub)(c, op); break;
    case NDS_THUMB_IMM8: NDS_FN(thumb_imm8)(c, op); break;
    case NDS_THUMB_ALU: NDS_FN(thumb_alu)(c, op); break;
    case NDS_THUMB_HI_REG: NDS_FN(thumb_hi_reg)(c, op); break;
    case NDS_THUMB_LDR_PC: NDS_FN(thumb_ldr_pc)(c, op); break;
    case NDS_THUMB_LDST_REG: NDS_FN(thumb_ldst_reg)(c, op); break;
    case NDS_THUMB_LDST_SIGN: NDS_FN(thumb_ldst_sign)(c, op); break;
    case NDS_THUMB_LDST_IMM: NDS_FN(thumb_ldst_imm)(c, op); break;
    case NDS_THUMB_LDST_HALF: NDS_FN(thumb_ldst_half)(c, op); break;
    case NDS_THUMB_LDST_SP: NDS_FN(thumb_ldst_sp)(c, op); break;
    case NDS_THUMB_ADD_PC_SP: NDS_FN(thumb_add_pc_sp)(c, op); break;
    case NDS_THUMB_ADD_SP: NDS_FN(thumb_add_sp)(c, op); break;
    case NDS_THUMB_PUSH_POP: NDS_FN(thumb_push_pop)(c, op); break;
    case NDS_THUMB_LDM_STM: NDS_FN(thumb_ldm_stm)(c, op); break;
    case NDS_THUMB_BCC: NDS_FN(thumb_bcc)(c, op); break;
    case NDS_THUMB_SWI: NDS_FN(op_swi)(c); break;
    case NDS_THUMB_B: NDS_FN(thumb_b)(c, op); break;
    case NDS_THUMB_BL: NDS_FN(thumb_bl)(c, op); break;
#if NDS_V5
    case NDS_THUMB_BKPT: nds9_op_prefetch_abort(c); break;
#endif
    default: NDS_FN(op_undefined)(c); break;
    }
    if (!c->branched) c->r[15] += 2;
}

/* ------------------------------------------------------------------ */
/* Fetch, step, interrupt                                              */
/* ------------------------------------------------------------------ */

void NDS_FN(cpu_refill)(NdsCpu *cpu)
{
    uint32_t pc = nds_cpu_pc(cpu);
    if (cpu->cpsr & NDS_CPSR_T) {
        cpu->pipe[0] = NDS_FN(bus_read16)(cpu, pc, NDS_ACCESS_CODE | NDS_ACCESS_NSEQ);
        cpu->pipe[1] = NDS_FN(bus_read16)(cpu, pc + 2, NDS_ACCESS_CODE | NDS_ACCESS_SEQ);
    } else {
        cpu->pipe[0] = NDS_FN(bus_read32)(cpu, pc, NDS_ACCESS_CODE | NDS_ACCESS_NSEQ);
        cpu->pipe[1] = NDS_FN(bus_read32)(cpu, pc + 4, NDS_ACCESS_CODE | NDS_ACCESS_SEQ);
    }
}

void NDS_FN(cpu_step)(NdsCpu *cpu)
{
    uint32_t op = cpu->pipe[0];
    cpu->pipe[0] = cpu->pipe[1];
    if (cpu->cpsr & NDS_CPSR_T) {
        cpu->pipe[1] = NDS_FN(bus_read16)(cpu, cpu->r[15] & ~1u, NDS_ACCESS_CODE | NDS_ACCESS_SEQ);
        NDS_FN(cpu_execute_thumb)(cpu, (uint16_t)op);
    } else {
        cpu->pipe[1] = NDS_FN(bus_read32)(cpu, cpu->r[15] & ~3u, NDS_ACCESS_CODE | NDS_ACCESS_SEQ);
        NDS_FN(cpu_execute_arm)(cpu, op);
    }
    if (cpu->branched) NDS_FN(cpu_refill)(cpu);
}

bool NDS_FN(cpu_irq)(NdsCpu *cpu)
{
    if (cpu->cpsr & NDS_CPSR_I) return false;
    /* LR_irq = address of the next instruction + 4, so SUBS PC, LR, #4
     * resumes it in either state. */
    nds_cpu_exception(cpu, NDS_VECTOR_IRQ, NDS_MODE_IRQ, nds_cpu_pc(cpu) + 4);
    NDS_FN(cpu_refill)(cpu);
    return true;
}

#include "nds_cpu_arch_end.inc"
