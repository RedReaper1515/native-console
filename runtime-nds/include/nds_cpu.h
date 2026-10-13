/*
 * CPU state for the Nintendo DS's two processors: the ARM9 (ARM946E-S,
 * ARMv5TE) and the ARM7 (ARM7TDMI, ARMv4T). Both use this struct. The
 * instruction handlers are written once (nds_cpu_impl.inc) and compiled
 * twice, as nds9_* and nds7_*, so each copy has its architecture's
 * differences folded in at compile time and calls its own bus.
 *
 * The interpreter and recompiled code share the struct. R15 follows the
 * pipeline-visible convention: while an instruction executes, and between
 * instructions, r[15] holds the address of the instruction being executed
 * plus 8 (ARM) or plus 4 (Thumb). An instruction that does not branch
 * advances r[15] by its own size; one that branches sets r[15] to the target
 * plus 8 or 4. This matches the SingleStepTests vectors directly.
 *
 * NZCV are kept as separate bytes because nearly every instruction touches
 * them; cpsr holds the remaining bits (mode, T, F, I, the ARMv5TE sticky
 * overflow flag Q in bit 27, and the reserved bits).
 */
#ifndef NDS_CPU_H
#define NDS_CPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NDS_MODE_USR = 0x10,
    NDS_MODE_FIQ = 0x11,
    NDS_MODE_IRQ = 0x12,
    NDS_MODE_SVC = 0x13,
    NDS_MODE_ABT = 0x17,
    NDS_MODE_UND = 0x1B,
    NDS_MODE_SYS = 0x1F,
};

/* Register banks. The order of 1..5 matches the SPSR order used by the
 * SingleStepTests vectors (fiq, svc, abt, irq, und). */
enum {
    NDS_BANK_NONE = 0, /* usr and sys: no SPSR */
    NDS_BANK_FIQ = 1,
    NDS_BANK_SVC = 2,
    NDS_BANK_ABT = 3,
    NDS_BANK_IRQ = 4,
    NDS_BANK_UND = 5,
    NDS_BANK_COUNT = 6,
};

#define NDS_CPSR_T (1u << 5)
#define NDS_CPSR_F (1u << 6)
#define NDS_CPSR_I (1u << 7)
#define NDS_CPSR_Q (1u << 27)
#define NDS_CPSR_FLAGS_MASK 0xF0000000u

/* Exception vectors, as offsets from NdsCpu.vector_base. */
#define NDS_VECTOR_RESET 0x00u
#define NDS_VECTOR_UNDEFINED 0x04u
#define NDS_VECTOR_SWI 0x08u
#define NDS_VECTOR_PREFETCH_ABORT 0x0Cu
#define NDS_VECTOR_DATA_ABORT 0x10u
#define NDS_VECTOR_IRQ 0x18u
#define NDS_VECTOR_FIQ 0x1Cu

/* CP15, the ARM9's system control coprocessor. The ARM7 has none. */
#define NDS_CP15_PU_ENABLE (1u << 0)
#define NDS_CP15_DCACHE_ENABLE (1u << 2)
#define NDS_CP15_BIG_ENDIAN (1u << 7)
#define NDS_CP15_ICACHE_ENABLE (1u << 12)
#define NDS_CP15_HIGH_VECTORS (1u << 13)
#define NDS_CP15_PRE_ARMV5 (1u << 15) /* loads into R15 do not change T */
#define NDS_CP15_DTCM_ENABLE (1u << 16)
#define NDS_CP15_DTCM_LOAD_MODE (1u << 17)
#define NDS_CP15_ITCM_ENABLE (1u << 18)
#define NDS_CP15_ITCM_LOAD_MODE (1u << 19)

typedef struct NdsCp15 {
    uint32_t control;         /* C1,C0,0 */
    uint32_t dcache_regions;  /* C2,C0,0: cacheable bits for data */
    uint32_t icache_regions;  /* C2,C0,1: cacheable bits for instructions */
    uint32_t write_buffer;    /* C3,C0,0 */
    uint32_t data_access;     /* C5,C0,2: four bits per region */
    uint32_t code_access;     /* C5,C0,3 */
    uint32_t region[8];       /* C6,C0-C7,0: protection unit regions */
    uint32_t dcache_lockdown; /* C9,C0,0 */
    uint32_t icache_lockdown; /* C9,C0,1 */
    uint32_t dtcm;            /* C9,C1,0: base and size */
    uint32_t itcm;            /* C9,C1,1: size (the base is always 0) */
    uint32_t process_id;      /* C13,C0,1 and C13,C1,1 */
} NdsCp15;

typedef struct NdsCpu {
    uint32_t r[16];

    /* CPSR without NZCV (bits 0-27), and NZCV as 0/1 bytes. */
    uint32_t cpsr;
    uint8_t flag_n;
    uint8_t flag_z;
    uint8_t flag_c;
    uint8_t flag_v;

    /* Banked copies. r8-r12 have two banks (FIQ and everything else);
     * r13-r14 and SPSR have one slot per bank. spsr[NDS_BANK_NONE] is a
     * scratch slot so user-mode SPSR accesses have somewhere to go. */
    uint32_t bank_r8_r12[2][5];
    uint32_t bank_r13[NDS_BANK_COUNT];
    uint32_t bank_r14[NDS_BANK_COUNT];
    uint32_t spsr[NDS_BANK_COUNT];

    /* The two prefetched opcodes: pipe[0] executes next, pipe[1] was
     * fetched from the address after it. */
    uint32_t pipe[2];

    /* Set by any instruction that writes R15; cleared before each step. */
    bool branched;

    /* Where the exception vectors are: 0 on the ARM7; on the ARM9, 0 or
     * 0xFFFF0000 as CP15 control bit 13 says. */
    uint32_t vector_base;

    /* Cycles consumed, advanced by the bus and by internal cycles. */
    uint64_t cycles;

    /* The ARM9's coprocessor. Unused on the ARM7. */
    NdsCp15 cp15;

    /* Owner of this CPU (the machine), for bus callbacks. */
    void *user;
} NdsCpu;

/* Bank index for a mode value; invalid modes map to NDS_BANK_NONE. */
int nds_cpu_bank_for_mode(uint32_t mode);

/* Full CPSR including NZCV. */
static inline uint32_t nds_cpu_get_cpsr(const NdsCpu *cpu)
{
    return cpu->cpsr | ((uint32_t)cpu->flag_n << 31) | ((uint32_t)cpu->flag_z << 30) |
           ((uint32_t)cpu->flag_c << 29) | ((uint32_t)cpu->flag_v << 28);
}

static inline bool nds_cpu_thumb(const NdsCpu *cpu)
{
    return (cpu->cpsr & NDS_CPSR_T) != 0;
}

static inline uint32_t nds_cpu_mode(const NdsCpu *cpu)
{
    return cpu->cpsr & 0x1Fu;
}

/* Write the whole CPSR, switching register banks if the mode changes. */
void nds_cpu_set_cpsr(NdsCpu *cpu, uint32_t value);

/* Switch register banks to a new mode without touching the other bits. */
void nds_cpu_switch_mode(NdsCpu *cpu, uint32_t new_mode);

/* SPSR of the current mode (the scratch slot in usr/sys). */
uint32_t *nds_cpu_current_spsr(NdsCpu *cpu);
bool nds_cpu_has_spsr(const NdsCpu *cpu);

/* SPSR of the current mode. User and system mode have none, and reads
 * return CPSR there. */
uint32_t nds_cpu_read_spsr(const NdsCpu *cpu);

/* Read or write a register as seen from user mode (LDM/STM with the S bit). */
uint32_t nds_cpu_get_user_reg(const NdsCpu *cpu, int reg);
void nds_cpu_set_user_reg(NdsCpu *cpu, int reg, uint32_t value);

/* Address of the instruction the next step executes. */
static inline uint32_t nds_cpu_pc(const NdsCpu *cpu)
{
    return (cpu->cpsr & NDS_CPSR_T) ? ((cpu->r[15] - 4u) & ~1u) : ((cpu->r[15] - 8u) & ~3u);
}

/* Exception entry. vector is an offset from vector_base; return_addr is
 * what LR of the new mode receives. */
void nds_cpu_exception(NdsCpu *cpu, uint32_t vector, uint32_t mode, uint32_t return_addr);

/* Power-on state: supervisor mode, interrupts off, about to fetch the reset
 * vector. The ARM9's vectors start at 0xFFFF0000, where its BIOS is. */
void nds9_cpu_reset(NdsCpu *cpu);
void nds7_cpu_reset(NdsCpu *cpu);

/* CP15 register access. id is (CRn << 8) | (CRm << 4) | opcode2. */
uint32_t nds9_cp15_read(const NdsCpu *cpu, uint32_t id);
void nds9_cp15_write(NdsCpu *cpu, uint32_t id, uint32_t value);

/* Each of these exists once per processor.
 *
 * execute_arm / execute_thumb run one already-fetched instruction; r[15]
 * must already hold the pipeline value (instruction address + 8 or + 4).
 *
 * step executes pipe[0], prefetching the next opcode first (the hardware
 * fetches in the instruction's first cycle), and refills after a branch.
 *
 * refill reloads the pipeline from nds_cpu_pc() after R15 was written.
 *
 * irq takes an IRQ now if CPSR.I is clear, and returns true if it did. */
void nds9_cpu_execute_arm(NdsCpu *cpu, uint32_t opcode);
void nds9_cpu_execute_thumb(NdsCpu *cpu, uint16_t opcode);
void nds9_cpu_step(NdsCpu *cpu);
void nds9_cpu_refill(NdsCpu *cpu);
bool nds9_cpu_irq(NdsCpu *cpu);

void nds7_cpu_execute_arm(NdsCpu *cpu, uint32_t opcode);
void nds7_cpu_execute_thumb(NdsCpu *cpu, uint16_t opcode);
void nds7_cpu_step(NdsCpu *cpu);
void nds7_cpu_refill(NdsCpu *cpu);
bool nds7_cpu_irq(NdsCpu *cpu);

#ifdef __cplusplus
}
#endif

#endif /* NDS_CPU_H */
