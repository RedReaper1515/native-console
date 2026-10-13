/*
 * WDC 65C816 CPU state, shared by the S-CPU (the SNES main CPU) and the SA-1.
 *
 * Every register and flag is an explicit field; the core never keeps guest
 * state in C locals across a bus access. Flags are unpacked into bytes so
 * that setting and testing them is cheap in generated code; the packed P
 * byte is assembled only where the CPU pushes or transfers it.
 *
 * Width modes: M (accumulator) and X (index) are flags in P. In emulation
 * mode (E = 1) both are forced to 1 and the stack pointer's high byte is
 * forced to 0x01. The handlers in snes_cpu_impl.h take the mode as an
 * argument (SNES_MODE_* bits) so that recompiled code, which knows the mode
 * of every instruction statically, compiles one width path only.
 */
#ifndef SNES_CPU_H
#define SNES_CPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mode bits: which widths are 8-bit, and emulation mode. E implies M8|X8. */
enum {
    SNES_MODE_X8 = 1u,
    SNES_MODE_M8 = 2u,
    SNES_MODE_E = 4u,
    SNES_MODE_EMULATION = 7u,
};

/* Interrupt vectors (bank 0). */
enum {
    SNES_VEC_COP_N = 0xFFE4,
    SNES_VEC_BRK_N = 0xFFE6,
    SNES_VEC_ABORT_N = 0xFFE8,
    SNES_VEC_NMI_N = 0xFFEA,
    SNES_VEC_IRQ_N = 0xFFEE,
    SNES_VEC_COP_E = 0xFFF4,
    SNES_VEC_ABORT_E = 0xFFF8,
    SNES_VEC_NMI_E = 0xFFFA,
    SNES_VEC_RESET = 0xFFFC,
    SNES_VEC_IRQ_E = 0xFFFE, /* also BRK in emulation mode */
};

typedef struct SnesCpu {
    uint16_t a, x, y, s, d, pc;
    uint8_t dbr, pbr;
    /* Processor status, one byte per flag (0 or 1). */
    uint8_t flag_c, flag_z, flag_i, flag_d, flag_x, flag_m, flag_v, flag_n, flag_e;

    uint8_t mdr;         /* last value on the data bus (open bus) */
    uint8_t branched;    /* the instruction transferred control (see snes_cpu_impl.h) */
    uint8_t wai;         /* WAI: sleeping until an interrupt */
    uint8_t stp;         /* STP: stopped until reset */
    uint8_t int_pending; /* an interrupt will be taken at the next instruction boundary */
    uint8_t last_i;      /* the I flag when the current instruction's last bus cycle began */
    uint8_t id;          /* 0 = S-CPU, 1 = SA-1 (bus implementations dispatch on it) */

    uint64_t cycles;        /* master clocks (21.477 MHz) this CPU has consumed */
    uint64_t last_cycle_at; /* cycles when the current instruction's last bus cycle began */
    uint64_t next_sync;     /* generated code yields once cycles reach this */
    uint64_t instructions;  /* executed, native or interpreted */
    uint64_t interpreted;   /* executed by the interpreter */

    void *user; /* owner (machine) */
} SnesCpu;

static inline unsigned snes_cpu_mode(const SnesCpu *c)
{
    return c->flag_e ? (unsigned)SNES_MODE_EMULATION
                     : ((c->flag_m ? (unsigned)SNES_MODE_M8 : 0u) | (c->flag_x ? (unsigned)SNES_MODE_X8 : 0u));
}

static inline uint8_t snes_cpu_get_p(const SnesCpu *c)
{
    return (uint8_t)(c->flag_c | c->flag_z << 1 | c->flag_i << 2 | c->flag_d << 3 | c->flag_x << 4 |
                     c->flag_m << 5 | c->flag_v << 6 | c->flag_n << 7);
}

/* Load P as PLP/RTI/REP/SEP do: emulation mode forces M and X to 1, and an
 * 8-bit index width clears the high bytes of X and Y. */
static inline void snes_cpu_set_p(SnesCpu *c, uint8_t p)
{
    c->flag_c = p & 1u;
    c->flag_z = (p >> 1) & 1u;
    c->flag_i = (p >> 2) & 1u;
    c->flag_d = (p >> 3) & 1u;
    c->flag_x = (p >> 4) & 1u;
    c->flag_m = (p >> 5) & 1u;
    c->flag_v = (p >> 6) & 1u;
    c->flag_n = (p >> 7) & 1u;
    if (c->flag_e) c->flag_x = c->flag_m = 1;
    if (c->flag_x) {
        c->x &= 0xFFu;
        c->y &= 0xFFu;
    }
}

static inline uint32_t snes_cpu_pc24(const SnesCpu *c) { return ((uint32_t)c->pbr << 16) | c->pc; }

/* Reset: emulation mode, 8-bit registers, stack in page 1, I set, D clear,
 * DBR/PBR/D zero, PC from the reset vector (read through the bus). */
void snes_cpu_reset(SnesCpu *c);

/* Interpret one instruction (the interrupt check is the caller's job). */
void snes_cpu_step(SnesCpu *c);

/* Take a hardware interrupt now: NMI (nmi = true) or IRQ. Pushes state
 * through the bus and loads PC from the vector. */
void snes_cpu_interrupt(SnesCpu *c, bool nmi);

/* Execute one already-fetched opcode (the interpreter's dispatch). */
void snes_cpu_execute(SnesCpu *c, uint8_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* SNES_CPU_H */
