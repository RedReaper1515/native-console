/*
 * Ricoh 2A03 CPU state: the NMOS 6502 core of the NES, without decimal mode.
 *
 * Every register and flag is an explicit field; the core never keeps guest
 * state in C locals across a bus access. Flags are unpacked into bytes so
 * that setting and testing them is cheap in generated code; the packed P
 * byte is assembled only where the CPU pushes it.
 *
 * P has six real bits. Bit 5 does not exist and reads as 1; bit 4 (B) is not
 * a flag either: it is the value the CPU drives on the bus when it pushes P,
 * 1 from PHP and BRK and 0 from an IRQ or NMI. D is a normal flag bit (SED,
 * CLD, PLP and RTI change it, PHP and interrupts push it), but the 2A03's
 * adder ignores it.
 *
 * Interrupts. The 6502 looks at its NMI edge detector and its IRQ line once
 * per instruction, while the cycle before the instruction's last cycle
 * completes, and the interrupt sequence then replaces the next instruction.
 * The handlers record that moment (NES_POLL in nes_cpu_impl.h):
 *
 *   poll_at   c->cycles when the poll happened, which is the number of CPU
 *             cycles completed by then. A line that went active during bus
 *             cycle number T (c->cycles == T once that bus call has
 *             returned) is seen by this poll if T <= poll_at.
 *   poll_i    the I flag at that moment. It masks the IRQ, never the NMI.
 *
 * So after an instruction (interpreted or recompiled) the machine takes
 *   an NMI  when its edge is latched and edge_cycle <= c->poll_at,
 *   an IRQ  when the line was active at the poll (active since a cycle
 *           <= c->poll_at, and not released by then) and !c->poll_i,
 * by calling nes_cpu_interrupt(), and otherwise runs the next instruction.
 * The IRQ is a level, and what counts is its state at the poll: an
 * instruction whose last cycle acknowledges it (a read of $4015, say) was
 * still polled with the line active, and is followed by the interrupt.
 *
 * The cases that follow from where the poll sits:
 *  - CLI, SEI and PLP change I in their last cycle, after the poll, so the
 *    old I applies: an IRQ pending at CLI is taken only after the
 *    instruction that follows CLI, and one pending at SEI is still taken
 *    right after SEI (and its handler returns with I set).
 *  - RTI pulls P two cycles before its poll, so the restored I applies at
 *    once.
 *  - A two-cycle instruction is polled at the end of its opcode fetch.
 *  - Branches are polled before the operand fetch. A taken branch that
 *    stays in its page (3 cycles) is not polled again before its third
 *    cycle, so an interrupt that arrives during the operand fetch waits
 *    for the instruction after the branch. A taken branch that crosses a
 *    page (4 cycles) is polled again before the high-byte fixup cycle.
 *  - BRK and the interrupt sequence do not poll (poll_at = 0, poll_i = 1:
 *    no event is at or before cycle 0), so the first instruction of a
 *    handler always runs. An NMI that arrives during the first four cycles
 *    of one of them instead redirects it to the NMI vector; that is the
 *    bus's decision, see nes_bus_interrupt_vector() in nes_bus.h.
 */
#ifndef NES_CPU_H
#define NES_CPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Interrupt vectors. */
enum {
    NES_VEC_NMI = 0xFFFA,
    NES_VEC_RESET = 0xFFFC,
    NES_VEC_IRQ = 0xFFFE, /* also BRK */
};

/* Bits of the packed P byte. */
enum {
    NES_P_C = 0x01,
    NES_P_Z = 0x02,
    NES_P_I = 0x04,
    NES_P_D = 0x08,
    NES_P_B = 0x10, /* only on the stack: 1 from PHP/BRK, 0 from IRQ/NMI */
    NES_P_1 = 0x20, /* always 1 */
    NES_P_V = 0x40,
    NES_P_N = 0x80,
};

typedef struct NesCpu {
    uint16_t pc;
    uint8_t a, x, y;
    uint8_t s; /* the stack is page 1: $0100 | s */
    /* Processor status, one byte per flag (0 or 1). */
    uint8_t flag_c, flag_z, flag_i, flag_d, flag_v, flag_n;

    uint8_t mdr;      /* last value on the data bus (open bus); the bus keeps it */
    uint8_t branched; /* the instruction transferred control (see nes_cpu_impl.h) */
    uint8_t jammed;   /* a JAM opcode halted the CPU; only reset clears it */
    uint8_t poll_i;   /* the I flag at the instruction's interrupt poll */
    uint8_t stalled;  /* the bus sets this when it halts a read (RDY low: a DMA); see nes_sh_store */

    uint64_t cycles;       /* CPU cycles consumed; the bus adds one per call */
    uint64_t poll_at;      /* cycles at the instruction's interrupt poll; 0 = it did not poll */
    uint64_t next_sync;    /* generated code yields once cycles reach this */
    uint64_t instructions; /* executed, native or interpreted */
    uint64_t interpreted;  /* executed by the interpreter */

    void *user; /* owner (machine) */
} NesCpu;

/* P as the CPU pushes it for an IRQ or NMI: bit 5 set, B clear. PHP and BRK
 * push this with NES_P_B added. */
static inline uint8_t nes_cpu_get_p(const NesCpu *c)
{
    return (uint8_t)(c->flag_c | c->flag_z << 1 | c->flag_i << 2 | c->flag_d << 3 | NES_P_1 | c->flag_v << 6 |
                     c->flag_n << 7);
}

/* Load P as PLP and RTI do: bits 4 and 5 of the pulled byte go nowhere. */
static inline void nes_cpu_set_p(NesCpu *c, uint8_t p)
{
    c->flag_c = p & 1u;
    c->flag_z = (p >> 1) & 1u;
    c->flag_i = (p >> 2) & 1u;
    c->flag_d = (p >> 3) & 1u;
    c->flag_v = (p >> 6) & 1u;
    c->flag_n = (p >> 7) & 1u;
}

/* Reset: the seven-cycle sequence of the real chip. It is the interrupt
 * sequence with the three pushes turned into reads, so S drops by 3 and
 * nothing is written; I is set and PC comes from $FFFC. A, X, Y and the
 * other flags keep their values. For power-on, zero the struct first: S
 * then ends at $FD. */
void nes_cpu_reset(NesCpu *c);

/* Interpret one instruction (the interrupt check is the caller's job). A
 * jammed CPU spends one cycle reading $FFFF instead, as the chip does until
 * it is reset. */
void nes_cpu_step(NesCpu *c);

/* Execute one already-fetched opcode (the interpreter's dispatch); c->pc
 * points after the opcode byte. */
void nes_cpu_execute(NesCpu *c, uint8_t opcode);

/* Take a hardware interrupt now: NMI (nmi = true) or IRQ. Seven cycles: two
 * discarded reads of PC, PCH, PCL and P pushed with B clear, then the
 * vector, which the bus chooses (nes_bus_interrupt_vector). Sets I. */
void nes_cpu_interrupt(NesCpu *c, bool nmi);

#ifdef __cplusplus
}
#endif

#endif /* NES_CPU_H */
