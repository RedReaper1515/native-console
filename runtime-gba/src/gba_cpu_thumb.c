/*
 * Thumb (16-bit) instruction interpreter for the ARM7TDMI: classify, then
 * call the handler from gba_cpu_impl.h.
 */
#include "gba_cpu_decode.h"
#include "gba_cpu_impl.h"

void gba_cpu_execute_thumb(GbaCpu *c, uint16_t op)
{
    c->branched = false;
    switch (gba_thumb_classify(op)) {
    case GBA_THUMB_SHIFT: gba_thumb_shift_imm(c, op); break;
    case GBA_THUMB_ADD_SUB: gba_thumb_add_sub(c, op); break;
    case GBA_THUMB_IMM8: gba_thumb_imm8(c, op); break;
    case GBA_THUMB_ALU: gba_thumb_alu(c, op); break;
    case GBA_THUMB_HI_REG: gba_thumb_hi_reg(c, op); break;
    case GBA_THUMB_LDR_PC: gba_thumb_ldr_pc(c, op); break;
    case GBA_THUMB_LDST_REG: gba_thumb_ldst_reg(c, op); break;
    case GBA_THUMB_LDST_SIGN: gba_thumb_ldst_sign(c, op); break;
    case GBA_THUMB_LDST_IMM: gba_thumb_ldst_imm(c, op); break;
    case GBA_THUMB_LDST_HALF: gba_thumb_ldst_half(c, op); break;
    case GBA_THUMB_LDST_SP: gba_thumb_ldst_sp(c, op); break;
    case GBA_THUMB_ADD_PC_SP: gba_thumb_add_pc_sp(c, op); break;
    case GBA_THUMB_ADD_SP: gba_thumb_add_sp(c, op); break;
    case GBA_THUMB_PUSH_POP: gba_thumb_push_pop(c, op); break;
    case GBA_THUMB_LDM_STM: gba_thumb_ldm_stm(c, op); break;
    case GBA_THUMB_BCC: gba_thumb_bcc(c, op); break;
    case GBA_THUMB_SWI: gba_op_swi(c); break;
    case GBA_THUMB_B: gba_thumb_b(c, op); break;
    case GBA_THUMB_BL: gba_thumb_bl(c, op); break;
    default: gba_op_undefined(c); break;
    }
    if (!c->branched) c->r[15] += 2;
}
