/*
 * 65C816 interpreter: fetch, dispatch to the shared handlers, reset and
 * hardware interrupt entry.
 */
#include "snes_cpu_impl.h"

void snes_cpu_execute(SnesCpu *c, uint8_t opcode)
{
    const unsigned mode = snes_cpu_mode(c);
    switch (opcode) {
#define SNES_CASE(hex, body)                                                                                           \
    case 0x##hex: snes_op_##hex(c, mode); break;
        SNES_OPCODES(SNES_CASE)
#undef SNES_CASE
    }
}

void snes_cpu_step(SnesCpu *c)
{
    uint8_t opcode = snes_fetch(c);
    snes_cpu_execute(c, opcode);
    c->instructions++;
    c->interpreted++;
}

void snes_cpu_reset(SnesCpu *c)
{
    c->flag_e = 1;
    c->flag_m = c->flag_x = 1;
    c->flag_i = 1;
    c->flag_d = 0;
    c->x &= 0xFFu;
    c->y &= 0xFFu;
    c->s = (uint16_t)(0x0100u | (c->s & 0xFFu));
    c->d = 0;
    c->dbr = 0;
    c->pbr = 0;
    c->wai = c->stp = 0;
    c->int_pending = 0;
    c->branched = 0;
    uint32_t pc = snes_bus_read(c, SNES_VEC_RESET);
    pc |= (uint32_t)snes_bus_read(c, SNES_VEC_RESET + 1u) << 8;
    c->pc = (uint16_t)pc;
}

/* Hardware interrupt: the opcode fetch of the next instruction happens but
 * is discarded (PC does not advance), then an internal cycle, the pushes and
 * the vector. In emulation mode the pushed P has B clear and PBR is not
 * pushed. */
void snes_cpu_interrupt(SnesCpu *c, bool nmi)
{
    const unsigned mode = snes_cpu_mode(c);
    uint16_t vector = SNES_EMU(mode) ? (nmi ? SNES_VEC_NMI_E : SNES_VEC_IRQ_E) : (nmi ? SNES_VEC_NMI_N : SNES_VEC_IRQ_N);
    (void)snes_rd(c, snes_cpu_pc24(c));
    snes_io(c);
    if (!SNES_EMU(mode)) snes_push(c, mode, c->pbr);
    snes_push(c, mode, (uint8_t)(c->pc >> 8));
    snes_push(c, mode, (uint8_t)c->pc);
    uint8_t p = snes_cpu_get_p(c);
    snes_push(c, mode, SNES_EMU(mode) ? (uint8_t)(p & ~0x10u) : p);
    c->flag_i = 1;
    c->flag_d = 0;
    uint32_t pc = snes_rd(c, vector);
    SNES_LAST(c);
    pc |= (uint32_t)snes_rd(c, vector + 1u) << 8;
    c->pc = (uint16_t)pc;
    c->pbr = 0;
    c->wai = 0;
    c->branched = 1;
}
