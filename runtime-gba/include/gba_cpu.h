/*
 * ARM7TDMI CPU state for the Game Boy Advance runtime.
 *
 * The interpreter and recompiled code share this struct. R15 follows the
 * hardware's pipeline-visible convention: while an instruction executes, and
 * between instructions, r[15] holds the address of the instruction being
 * executed plus 8 (ARM) or plus 4 (Thumb). An instruction that does not
 * branch advances r[15] by its own size; one that branches sets r[15] to the
 * target plus 8 or 4. This matches the SingleStepTests vectors directly.
 *
 * NZCV are kept as separate bytes because nearly every instruction touches
 * them; cpsr holds the remaining bits (mode, T, F, I and the reserved bits).
 */
#ifndef GBA_CPU_H
#define GBA_CPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GBA_MODE_USR = 0x10,
    GBA_MODE_FIQ = 0x11,
    GBA_MODE_IRQ = 0x12,
    GBA_MODE_SVC = 0x13,
    GBA_MODE_ABT = 0x17,
    GBA_MODE_UND = 0x1B,
    GBA_MODE_SYS = 0x1F,
};

/* Register banks. The order of 1..5 matches the SPSR order used by the
 * SingleStepTests vectors (fiq, svc, abt, irq, und). */
enum {
    GBA_BANK_NONE = 0, /* usr and sys: no SPSR */
    GBA_BANK_FIQ = 1,
    GBA_BANK_SVC = 2,
    GBA_BANK_ABT = 3,
    GBA_BANK_IRQ = 4,
    GBA_BANK_UND = 5,
    GBA_BANK_COUNT = 6,
};

#define GBA_CPSR_T (1u << 5)
#define GBA_CPSR_F (1u << 6)
#define GBA_CPSR_I (1u << 7)
#define GBA_CPSR_FLAGS_MASK 0xF0000000u

typedef struct GbaCpu {
    uint32_t r[16];

    /* CPSR without NZCV (bits 0-27), and NZCV as 0/1 bytes. */
    uint32_t cpsr;
    uint8_t flag_n;
    uint8_t flag_z;
    uint8_t flag_c;
    uint8_t flag_v;

    /* Banked copies. r8-r12 have two banks (FIQ and everything else);
     * r13-r14 and SPSR have one slot per bank. spsr[GBA_BANK_NONE] is a
     * scratch slot so user-mode SPSR accesses have somewhere to go. */
    uint32_t bank_r8_r12[2][5];
    uint32_t bank_r13[GBA_BANK_COUNT];
    uint32_t bank_r14[GBA_BANK_COUNT];
    uint32_t spsr[GBA_BANK_COUNT];

    /* The two prefetched opcodes: pipe[0] executes next, pipe[1] was
     * fetched from the address after it. Code that overwrites the next two
     * instructions does not affect them, as on hardware. */
    uint32_t pipe[2];

    /* Set by any instruction that writes R15; cleared before each step. */
    bool branched;

    /* Cycles consumed, advanced by the bus and by internal cycles. */
    uint64_t cycles;

    /* Owner of this CPU (the machine), for bus callbacks. */
    void *user;
} GbaCpu;

/* Bank index for a mode value; invalid modes map to GBA_BANK_NONE. */
int gba_cpu_bank_for_mode(uint32_t mode);

/* Full CPSR including NZCV. */
static inline uint32_t gba_cpu_get_cpsr(const GbaCpu *cpu)
{
    return cpu->cpsr | ((uint32_t)cpu->flag_n << 31) | ((uint32_t)cpu->flag_z << 30) |
           ((uint32_t)cpu->flag_c << 29) | ((uint32_t)cpu->flag_v << 28);
}

static inline bool gba_cpu_thumb(const GbaCpu *cpu)
{
    return (cpu->cpsr & GBA_CPSR_T) != 0;
}

static inline uint32_t gba_cpu_mode(const GbaCpu *cpu)
{
    return cpu->cpsr & 0x1Fu;
}

/* Write the whole CPSR, switching register banks if the mode changes. */
void gba_cpu_set_cpsr(GbaCpu *cpu, uint32_t value);

/* Switch register banks to a new mode without touching the other bits. */
void gba_cpu_switch_mode(GbaCpu *cpu, uint32_t new_mode);

/* SPSR of the current mode (the scratch slot in usr/sys). */
uint32_t *gba_cpu_current_spsr(GbaCpu *cpu);
bool gba_cpu_has_spsr(const GbaCpu *cpu);

/* Read or write a register as seen from user mode (LDM/STM with the S bit). */
uint32_t gba_cpu_get_user_reg(const GbaCpu *cpu, int reg);
void gba_cpu_set_user_reg(GbaCpu *cpu, int reg, uint32_t value);

/* Put the CPU in the post-BIOS state used by a direct ROM boot. */
void gba_cpu_reset(GbaCpu *cpu, bool skip_bios);

/* Write R15 and refill the pipeline in the current instruction set. The
 * target is not aligned: the fetch unit ignores the low address bits but
 * R15 keeps them, so callers align only where the instruction itself does
 * (BX, Thumb high-register ops, POP, BL). Fetches use gba_cpu_pc(). */
static inline void gba_cpu_branch(GbaCpu *cpu, uint32_t target)
{
    cpu->r[15] = target + ((cpu->cpsr & GBA_CPSR_T) ? 4u : 8u);
    cpu->branched = true;
}

/* Address of the instruction the next step executes. */
static inline uint32_t gba_cpu_pc(const GbaCpu *cpu)
{
    return (cpu->cpsr & GBA_CPSR_T) ? ((cpu->r[15] - 4u) & ~1u) : ((cpu->r[15] - 8u) & ~3u);
}

/* SPSR of the current mode. User and system mode have none, and reads
 * return CPSR there, as on the reference implementation. */
uint32_t gba_cpu_read_spsr(const GbaCpu *cpu);

/* Exception entry. return_addr is what LR of the new mode receives. */
void gba_cpu_exception(GbaCpu *cpu, uint32_t vector, uint32_t mode, uint32_t return_addr);

#define GBA_VECTOR_RESET 0x00u
#define GBA_VECTOR_UNDEFINED 0x04u
#define GBA_VECTOR_SWI 0x08u
#define GBA_VECTOR_IRQ 0x18u
#define GBA_VECTOR_FIQ 0x1Cu

/* Execute one already-fetched instruction. r[15] must already hold the
 * pipeline value (instruction address + 8 or + 4). */
void gba_cpu_execute_arm(GbaCpu *cpu, uint32_t opcode);
void gba_cpu_execute_thumb(GbaCpu *cpu, uint16_t opcode);

/* Execute pipe[0], prefetching the next opcode first (the hardware fetches
 * in the instruction's first cycle), and refill after a branch. */
void gba_cpu_step(GbaCpu *cpu);

/* Refill the pipeline from gba_cpu_pc() after R15 was written. */
void gba_cpu_refill(GbaCpu *cpu);

/* Take an IRQ now if CPSR.I is clear. Returns true if the exception was entered. */
bool gba_cpu_irq(GbaCpu *cpu);

#ifdef __cplusplus
}
#endif

#endif /* GBA_CPU_H */
