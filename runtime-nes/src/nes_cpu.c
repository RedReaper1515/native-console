/*
 * 6502 interpreter: fetch, dispatch to the shared handlers, reset and
 * hardware interrupt entry.
 *
 * The cycle order of the reset and interrupt sequences follows ares's
 * mos6502 (ISC license, Near et al.); see THIRD_PARTY.md. The code is our
 * own.
 */
#include "nes_cpu_impl.h"

void nes_cpu_execute(NesCpu *c, uint8_t opcode)
{
    switch (opcode) {
#define NES_CASE(hex, body)                                                                                            \
    case 0x##hex: nes_op_##hex(c); break;
        NES_OPCODES(NES_CASE)
#undef NES_CASE
    }
}

void nes_cpu_step(NesCpu *c)
{
    if (c->jammed) {
        /* The address bus is stuck at $FFFF; time still passes. */
        nes_dummy(c, 0xFFFFu);
        return;
    }
    uint8_t opcode = nes_fetch(c);
    nes_cpu_execute(c, opcode);
    c->instructions++;
    c->interpreted++;
}

/* Reset is the interrupt sequence with the read/write line held at read:
 * the three pushes move S but store nothing. */
void nes_cpu_reset(NesCpu *c)
{
    c->jammed = 0;
    c->branched = 0;
    nes_dummy(c, c->pc);
    nes_dummy(c, c->pc);
    for (int i = 0; i < 3; i++) {
        nes_dummy(c, (uint16_t)(0x0100u | c->s));
        c->s = (uint8_t)(c->s - 1u);
    }
    c->flag_i = 1;
    unsigned pc = nes_rd(c, NES_VEC_RESET);
    pc |= (unsigned)nes_rd(c, NES_VEC_RESET + 1u) << 8;
    c->pc = (uint16_t)pc;
    NES_POLL_NONE(c);
}

/* Hardware interrupt: the opcode fetch of the instruction that will not run
 * happens twice and is discarded (PC does not advance), then the pushes.
 * The pushed P has B clear, the only way a handler can tell an IRQ from a
 * BRK. Which vector is used is decided once PC is pushed (nes_bus.h): an
 * NMI that arrived during the first four cycles of an IRQ's entry takes the
 * sequence over. The sequence does not poll for interrupts, so the
 * handler's first instruction always runs. */
void nes_cpu_interrupt(NesCpu *c, bool nmi)
{
    nes_dummy(c, c->pc);
    nes_dummy(c, c->pc);
    nes_push(c, (uint8_t)(c->pc >> 8));
    nes_push(c, (uint8_t)c->pc);
    const uint16_t vector = nes_bus_interrupt_vector(c, nmi ? NES_VEC_NMI : NES_VEC_IRQ);
    nes_push(c, nes_cpu_get_p(c));
    c->flag_i = 1;
    unsigned pc = nes_rd(c, vector);
    pc |= (unsigned)nes_rd(c, (uint16_t)(vector + 1u)) << 8;
    c->pc = (uint16_t)pc;
    c->branched = 1;
    NES_POLL_NONE(c);
}
