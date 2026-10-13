/*
 * ARM and Thumb instruction classification for both DS processors, shared
 * by the interpreter, the recompiler's analyzer and its C emitter so that
 * all three agree on which handler in nds_cpu_impl.inc an opcode belongs
 * to. Pure functions of the opcode and the architecture; valid C and C++.
 *
 * v5 is 0 for the ARM7 (ARMv4T) and non-zero for the ARM9 (ARMv5TE). With
 * v5 == 0 the result is exactly the Game Boy Advance classification, which
 * the ARM7TDMI test vectors pin down, including how that CPU decodes
 * encodings that later architectures gave to new instructions.
 */
#ifndef NDS_CPU_DECODE_H
#define NDS_CPU_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Condition field values. */
enum {
    NDS_COND_EQ, NDS_COND_NE, NDS_COND_CS, NDS_COND_CC,
    NDS_COND_MI, NDS_COND_PL, NDS_COND_VS, NDS_COND_VC,
    NDS_COND_HI, NDS_COND_LS, NDS_COND_GE, NDS_COND_LT,
    NDS_COND_GT, NDS_COND_LE, NDS_COND_AL, NDS_COND_NV,
};

typedef enum NdsArmClass {
    NDS_ARM_BX,
    NDS_ARM_SWP,
    NDS_ARM_MUL,
    NDS_ARM_MULL,
    NDS_ARM_HALF,  /* LDRH/STRH/LDRSB/LDRSH */
    NDS_ARM_MRS,
    NDS_ARM_MSR,
    NDS_ARM_DP,    /* data processing */
    NDS_ARM_SDT,   /* LDR/STR/LDRB/STRB */
    NDS_ARM_BLOCK, /* LDM/STM */
    NDS_ARM_B,     /* B/BL */
    NDS_ARM_SWI,
    NDS_ARM_UNDEF,
    /* ARMv5TE only. */
    NDS_ARM_BLX_REG,
    NDS_ARM_BLX_IMM, /* condition field 1111 */
    NDS_ARM_PLD,     /* condition field 1111 */
    NDS_ARM_BKPT,
    NDS_ARM_CLZ,
    NDS_ARM_QARITH,  /* QADD/QSUB/QDADD/QDSUB */
    NDS_ARM_DSP_MUL, /* SMLAxy/SMLAWy/SMULWy/SMLALxy/SMULxy */
    NDS_ARM_DOUBLE,  /* LDRD/STRD */
    NDS_ARM_CP_REG,  /* MRC/MCR */
} NdsArmClass;

typedef enum NdsThumbClass {
    NDS_THUMB_SHIFT,     /* format 1 */
    NDS_THUMB_ADD_SUB,   /* 2 */
    NDS_THUMB_IMM8,      /* 3 */
    NDS_THUMB_ALU,       /* 4 */
    NDS_THUMB_HI_REG,    /* 5, including BX and (ARMv5) BLX */
    NDS_THUMB_LDR_PC,    /* 6 */
    NDS_THUMB_LDST_REG,  /* 7 */
    NDS_THUMB_LDST_SIGN, /* 8 */
    NDS_THUMB_LDST_IMM,  /* 9 */
    NDS_THUMB_LDST_HALF, /* 10 */
    NDS_THUMB_LDST_SP,   /* 11 */
    NDS_THUMB_ADD_PC_SP, /* 12 */
    NDS_THUMB_ADD_SP,    /* 13 */
    NDS_THUMB_PUSH_POP,  /* 14 */
    NDS_THUMB_LDM_STM,   /* 15 */
    NDS_THUMB_BCC,       /* 16 */
    NDS_THUMB_SWI,       /* 17 */
    NDS_THUMB_B,         /* 18 */
    NDS_THUMB_BL,        /* 19: the prefix, the BL suffix and (ARMv5) the BLX suffix */
    NDS_THUMB_UNDEF,
    /* ARMv5TE only. */
    NDS_THUMB_BKPT,
} NdsThumbClass;

static inline NdsArmClass nds_arm_classify(uint32_t op, int v5)
{
    const uint32_t opcode = (op >> 21) & 15u;
    const int psr_class = opcode >= 8 && opcode <= 11 && !(op & (1u << 20));

    if (v5 && (op >> 28) == 15u) {
        /* The unconditional instructions. Everything else in this space
         * is undefined. */
        if ((op & 0xFE000000u) == 0xFA000000u) return NDS_ARM_BLX_IMM;
        if ((op & 0xFD70F000u) == 0xF550F000u) return NDS_ARM_PLD;
        return NDS_ARM_UNDEF;
    }

    switch ((op >> 25) & 7u) {
    case 0:
        if ((op & 0x0FF000F0u) == 0x01200010u) return NDS_ARM_BX;
        if (v5) {
            if ((op & 0x0FF000F0u) == 0x01200030u) return NDS_ARM_BLX_REG;
            if ((op & 0x0FF000F0u) == 0x01200070u) return NDS_ARM_BKPT;
            if ((op & 0x0FF000F0u) == 0x01600010u) return NDS_ARM_CLZ;
            if ((op & 0x0F9000F0u) == 0x01000050u) return NDS_ARM_QARITH;
            if ((op & 0x0F900090u) == 0x01000080u) return NDS_ARM_DSP_MUL;
        }
        if ((op & 0xF0u) == 0x90u) {
            /* The ARM7TDMI decodes everything here as SWP or a multiply.
             * From ARMv5 the encodings not allocated are undefined: the
             * rest of the SWP space (LDREX/STREX from ARMv6) and the
             * multiplies with bit 22 set and bit 23 clear (UMAAL, MLS). */
            if (op & (1u << 24)) {
                if (v5 && (op & 0x00B00000u)) return NDS_ARM_UNDEF;
                return NDS_ARM_SWP;
            }
            if (op & (1u << 23)) return NDS_ARM_MULL;
            if (v5 && (op & (1u << 22))) return NDS_ARM_UNDEF;
            return NDS_ARM_MUL;
        }
        if ((op & 0x90u) == 0x90u) {
            /* With L clear, the two signed forms are LDRD and STRD. */
            if (v5 && !(op & (1u << 20)) && (op & 0x40u)) return NDS_ARM_DOUBLE;
            return NDS_ARM_HALF;
        }
        if (psr_class) {
            /* The ARM7TDMI ignores bits 4-7 here; on ARMv5 the encodings
             * that are not MRS or MSR are undefined. */
            if (v5 && (op & 0xF0u)) return NDS_ARM_UNDEF;
            return (op & (1u << 21)) ? NDS_ARM_MSR : NDS_ARM_MRS;
        }
        return NDS_ARM_DP;
    case 1:
        if (psr_class) {
            if (op & (1u << 21)) return NDS_ARM_MSR;
            return v5 ? NDS_ARM_UNDEF : NDS_ARM_MRS;
        }
        return NDS_ARM_DP;
    case 2:
        return NDS_ARM_SDT;
    case 3:
        return (op & 0x10u) ? NDS_ARM_UNDEF : NDS_ARM_SDT;
    case 4:
        return NDS_ARM_BLOCK;
    case 5:
        return NDS_ARM_B;
    case 6:
        return NDS_ARM_UNDEF; /* LDC/STC: no coprocessor takes them */
    default:
        if (op & (1u << 24)) return NDS_ARM_SWI;
        /* MRC/MCR reach CP15 on the ARM9. CDP is undefined on both. */
        return (v5 && (op & 0x10u)) ? NDS_ARM_CP_REG : NDS_ARM_UNDEF;
    }
}

static inline NdsThumbClass nds_thumb_classify(uint16_t op, int v5)
{
    switch (op >> 13) {
    case 0:
        return (((op >> 11) & 3u) == 3u) ? NDS_THUMB_ADD_SUB : NDS_THUMB_SHIFT;
    case 1:
        return NDS_THUMB_IMM8;
    case 2:
        if ((op >> 10) == 0x10u) return NDS_THUMB_ALU;
        if ((op >> 10) == 0x11u) return NDS_THUMB_HI_REG;
        if ((op >> 11) == 0x09u) return NDS_THUMB_LDR_PC;
        return (op & 0x200u) ? NDS_THUMB_LDST_SIGN : NDS_THUMB_LDST_REG;
    case 3:
        return NDS_THUMB_LDST_IMM;
    case 4:
        return (op & 0x1000u) ? NDS_THUMB_LDST_SP : NDS_THUMB_LDST_HALF;
    case 5:
        if (!(op & 0x1000u)) return NDS_THUMB_ADD_PC_SP;
        if ((op & 0x0F00u) == 0x0000u) return NDS_THUMB_ADD_SP;
        if ((op & 0x0600u) == 0x0400u) return NDS_THUMB_PUSH_POP;
        if (v5 && (op & 0x0F00u) == 0x0E00u) return NDS_THUMB_BKPT;
        return NDS_THUMB_UNDEF;
    case 6:
        if (!(op & 0x1000u)) return NDS_THUMB_LDM_STM;
        if (((op >> 8) & 15u) == 15u) return NDS_THUMB_SWI;
        /* Condition 14 is undefined. The ARM7TDMI branches anyway (the
         * handler treats it as "always"); ARMv5 takes the trap. */
        if (v5 && ((op >> 8) & 15u) == 14u) return NDS_THUMB_UNDEF;
        return NDS_THUMB_BCC;
    default:
        if ((op & 0x1800u) == 0x0000u) return NDS_THUMB_B;
        if (op & 0x1000u) return NDS_THUMB_BL;
        /* 11101: the BLX suffix, ARMv5 only. */
        return v5 ? NDS_THUMB_BL : NDS_THUMB_UNDEF;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* NDS_CPU_DECODE_H */
