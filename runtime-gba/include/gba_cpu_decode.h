/*
 * ARM7TDMI instruction classification, shared by the interpreter, the
 * recompiler's analyzer and its C emitter so that all three agree on which
 * handler in gba_cpu_impl.h an opcode belongs to. Pure functions of the
 * opcode; valid C and C++.
 */
#ifndef GBA_CPU_DECODE_H
#define GBA_CPU_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Condition field values. */
enum {
    GBA_COND_EQ, GBA_COND_NE, GBA_COND_CS, GBA_COND_CC,
    GBA_COND_MI, GBA_COND_PL, GBA_COND_VS, GBA_COND_VC,
    GBA_COND_HI, GBA_COND_LS, GBA_COND_GE, GBA_COND_LT,
    GBA_COND_GT, GBA_COND_LE, GBA_COND_AL, GBA_COND_NV,
};

typedef enum GbaArmClass {
    GBA_ARM_BX,
    GBA_ARM_SWP,
    GBA_ARM_MUL,
    GBA_ARM_MULL,
    GBA_ARM_HALF,  /* LDRH/STRH/LDRSB/LDRSH */
    GBA_ARM_MRS,
    GBA_ARM_MSR,
    GBA_ARM_DP,    /* data processing */
    GBA_ARM_SDT,   /* LDR/STR/LDRB/STRB */
    GBA_ARM_BLOCK, /* LDM/STM */
    GBA_ARM_B,     /* B/BL */
    GBA_ARM_SWI,
    GBA_ARM_UNDEF, /* undefined, and all coprocessor space */
} GbaArmClass;

typedef enum GbaThumbClass {
    GBA_THUMB_SHIFT,     /* format 1 */
    GBA_THUMB_ADD_SUB,   /* 2 */
    GBA_THUMB_IMM8,      /* 3 */
    GBA_THUMB_ALU,       /* 4 */
    GBA_THUMB_HI_REG,    /* 5, including BX */
    GBA_THUMB_LDR_PC,    /* 6 */
    GBA_THUMB_LDST_REG,  /* 7 */
    GBA_THUMB_LDST_SIGN, /* 8 */
    GBA_THUMB_LDST_IMM,  /* 9 */
    GBA_THUMB_LDST_HALF, /* 10 */
    GBA_THUMB_LDST_SP,   /* 11 */
    GBA_THUMB_ADD_PC_SP, /* 12 */
    GBA_THUMB_ADD_SP,    /* 13 */
    GBA_THUMB_PUSH_POP,  /* 14 */
    GBA_THUMB_LDM_STM,   /* 15 */
    GBA_THUMB_BCC,       /* 16 */
    GBA_THUMB_SWI,       /* 17 */
    GBA_THUMB_B,         /* 18 */
    GBA_THUMB_BL,        /* 19, either half */
    GBA_THUMB_UNDEF,
} GbaThumbClass;

static inline GbaArmClass gba_arm_classify(uint32_t op)
{
    const uint32_t opcode = (op >> 21) & 15u;
    const int psr_class = opcode >= 8 && opcode <= 11 && !(op & (1u << 20));

    switch ((op >> 25) & 7u) {
    case 0:
        if ((op & 0x0FF000F0u) == 0x01200010u) return GBA_ARM_BX;
        if ((op & 0xF0u) == 0x90u) {
            if (op & (1u << 24)) return GBA_ARM_SWP;
            return (op & (1u << 23)) ? GBA_ARM_MULL : GBA_ARM_MUL;
        }
        if ((op & 0x90u) == 0x90u) return GBA_ARM_HALF;
        if (psr_class) return (op & (1u << 21)) ? GBA_ARM_MSR : GBA_ARM_MRS;
        return GBA_ARM_DP;
    case 1:
        if (psr_class) return (op & (1u << 21)) ? GBA_ARM_MSR : GBA_ARM_MRS;
        return GBA_ARM_DP;
    case 2:
        return GBA_ARM_SDT;
    case 3:
        return (op & 0x10u) ? GBA_ARM_UNDEF : GBA_ARM_SDT;
    case 4:
        return GBA_ARM_BLOCK;
    case 5:
        return GBA_ARM_B;
    case 6:
        return GBA_ARM_UNDEF; /* LDC/STC: no coprocessors */
    default:
        return (op & (1u << 24)) ? GBA_ARM_SWI : GBA_ARM_UNDEF; /* CDP/MRC/MCR */
    }
}

static inline GbaThumbClass gba_thumb_classify(uint16_t op)
{
    switch (op >> 13) {
    case 0:
        return (((op >> 11) & 3u) == 3u) ? GBA_THUMB_ADD_SUB : GBA_THUMB_SHIFT;
    case 1:
        return GBA_THUMB_IMM8;
    case 2:
        if ((op >> 10) == 0x10u) return GBA_THUMB_ALU;
        if ((op >> 10) == 0x11u) return GBA_THUMB_HI_REG;
        if ((op >> 11) == 0x09u) return GBA_THUMB_LDR_PC;
        return (op & 0x200u) ? GBA_THUMB_LDST_SIGN : GBA_THUMB_LDST_REG;
    case 3:
        return GBA_THUMB_LDST_IMM;
    case 4:
        return (op & 0x1000u) ? GBA_THUMB_LDST_SP : GBA_THUMB_LDST_HALF;
    case 5:
        if (!(op & 0x1000u)) return GBA_THUMB_ADD_PC_SP;
        if ((op & 0x0F00u) == 0x0000u) return GBA_THUMB_ADD_SP;
        if ((op & 0x0600u) == 0x0400u) return GBA_THUMB_PUSH_POP;
        return GBA_THUMB_UNDEF;
    case 6:
        if (!(op & 0x1000u)) return GBA_THUMB_LDM_STM;
        return (((op >> 8) & 15u) == 15u) ? GBA_THUMB_SWI : GBA_THUMB_BCC;
    default:
        if ((op & 0x1800u) == 0x0000u) return GBA_THUMB_B;
        if (op & 0x1000u) return GBA_THUMB_BL;
        return GBA_THUMB_UNDEF; /* 11101: BLX suffix, ARMv5 only */
    }
}

#ifdef __cplusplus
}
#endif

#endif /* GBA_CPU_DECODE_H */
