/*
 * 65C816 instruction handlers: the only implementation of instruction
 * semantics.
 *
 * The interpreter (snes_cpu.c) fetches an opcode and calls snes_op_XX(c,
 * mode) with the mode read from the flags. Recompiled code calls the same
 * snes_op_XX with a constant mode, and because every handler is
 * force-inlined the C compiler folds the 8/16-bit choices away. The
 * SingleStepTests vectors validate these handlers through the interpreter
 * (tests/snes/sst_65816.c): registers, flags, memory and the ordered bus
 * cycles, including idle cycles.
 *
 * Bus cycle order follows ares (ISC license, Near et al.), which is
 * hardware-verified; see THIRD_PARTY.md. The code is our own.
 *
 * Contract: c->pc points after the opcode byte (the caller fetched it).
 * Handlers that transfer control set c->branched; recompiled code then
 * leaves or jumps to a known label. SNES_LAST marks the start of an
 * instruction's last bus cycle, where the S-CPU samples its interrupt
 * lines (c->last_cycle_at, c->last_i).
 *
 * Wrap rules the vectors pin (and games rely on):
 *  - Absolute and long data addresses are 24-bit: an index or a 16-bit
 *    operand's second byte carries into the next bank.
 *  - Direct page and stack-relative addresses wrap at 16 bits in bank 0.
 *  - In emulation mode with D's low byte zero, direct page wraps within
 *    the page. (dp,X) with D's low byte non-zero reads the pointer's high
 *    byte with the +1 wrapped within the page.
 *  - Instructions new on the 65816 (PEA, PEI, PER, PHD, PLD, PLB, JSL, RTL,
 *    JSR (a,X)) move S as a 16-bit value even in emulation mode, then force
 *    S's high byte back to 0x01; the older ones wrap S within page 1.
 *  - Branches and JMP/JSR wrap within the program bank.
 */
#ifndef SNES_CPU_IMPL_H
#define SNES_CPU_IMPL_H

#include "snes_bus.h"
#include "snes_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define SNES_OP static __forceinline
#else
#define SNES_OP static inline __attribute__((always_inline))
#endif

/* Width tests on the mode argument. */
#define SNES_M16(mode) (((mode) & SNES_MODE_M8) == 0)
#define SNES_X16(mode) (((mode) & SNES_MODE_X8) == 0)
#define SNES_EMU(mode) (((mode) & SNES_MODE_E) != 0)

/* Interrupts are sampled when the last bus cycle of an instruction starts,
 * with the I flag as it was then (so CLI lets an IRQ in only after the next
 * instruction, and SEI still lets one in). */
#define SNES_LAST(c) ((c)->last_cycle_at = (c)->cycles, (c)->last_i = (c)->flag_i)

/* ------------------------------------------------------------------ */
/* Bus helpers                                                         */
/* ------------------------------------------------------------------ */

SNES_OP uint8_t snes_fetch(SnesCpu *c)
{
    uint8_t v = snes_bus_read(c, ((uint32_t)c->pbr << 16) | c->pc);
    c->pc = (uint16_t)(c->pc + 1u);
    return v;
}

SNES_OP uint8_t snes_rd(SnesCpu *c, uint32_t addr) { return snes_bus_read(c, addr & 0xFFFFFFu); }
SNES_OP void snes_wr(SnesCpu *c, uint32_t addr, uint8_t v) { snes_bus_write(c, addr & 0xFFFFFFu, v); }
SNES_OP void snes_io(SnesCpu *c) { snes_bus_idle(c); }

/* An implied instruction's internal cycle turns into a read of the next
 * opcode byte when an interrupt will be taken right after it. */
SNES_OP void snes_io_irq(SnesCpu *c)
{
    SNES_LAST(c);
    if (c->int_pending) (void)snes_rd(c, snes_cpu_pc24(c));
    else snes_io(c);
}

/* The SA-1 (c->id 1) spends a cycle after a jump or return into ROM, and
 * after a taken branch to an odd address in ROM; the S-CPU does not. */
SNES_OP void snes_io_jump(SnesCpu *c)
{
    if (c->id) snes_bus_idle_jump(c);
}

SNES_OP void snes_io_branch(SnesCpu *c)
{
    if (c->id && (c->pc & 1u)) snes_bus_idle_jump(c);
}

/* Extra cycle when the direct page register's low byte is non-zero. */
SNES_OP void snes_io_dl(SnesCpu *c)
{
    if (c->d & 0xFFu) snes_io(c);
}

/* Extra cycle for indexed addressing: always with 16-bit index registers,
 * else when the index crosses a page. */
SNES_OP void snes_io_index(SnesCpu *c, unsigned mode, uint32_t base, uint32_t indexed)
{
    if (SNES_X16(mode) || ((base >> 8) & 0xFFu) != ((indexed >> 8) & 0xFFu)) snes_io(c);
}

SNES_OP uint32_t snes_dp_addr(const SnesCpu *c, unsigned mode, uint32_t offset)
{
    if (SNES_EMU(mode) && !(c->d & 0xFFu)) return (uint32_t)c->d | (offset & 0xFFu);
    return (uint32_t)((c->d + offset) & 0xFFFFu);
}
SNES_OP uint8_t snes_rd_dp(SnesCpu *c, unsigned mode, uint32_t offset) { return snes_rd(c, snes_dp_addr(c, mode, offset)); }
SNES_OP void snes_wr_dp(SnesCpu *c, unsigned mode, uint32_t offset, uint8_t v) { snes_wr(c, snes_dp_addr(c, mode, offset), v); }

/* Direct page without the emulation-mode page wrap ([dp], PEI). */
SNES_OP uint8_t snes_rd_dpn(SnesCpu *c, uint32_t offset) { return snes_rd(c, (c->d + offset) & 0xFFFFu); }

/* (dp,X) pointer bytes: in emulation mode with DL != 0 the high byte's
 * +1 wraps within the page of D + dp + X. */
SNES_OP uint8_t snes_rd_dpx(SnesCpu *c, unsigned mode, uint32_t address, uint32_t offset)
{
    if (SNES_EMU(mode) && (c->d & 0xFFu)) {
        uint32_t base = (c->d + address) & 0xFFFFu;
        return snes_rd(c, (base & 0xFF00u) | ((base + offset) & 0xFFu));
    }
    return snes_rd_dp(c, mode, address + offset);
}

SNES_OP uint32_t snes_bank_addr(const SnesCpu *c, uint32_t offset) { return (((uint32_t)c->dbr << 16) + offset) & 0xFFFFFFu; }
SNES_OP uint8_t snes_rd_bank(SnesCpu *c, uint32_t offset) { return snes_rd(c, snes_bank_addr(c, offset)); }
SNES_OP void snes_wr_bank(SnesCpu *c, uint32_t offset, uint8_t v) { snes_wr(c, snes_bank_addr(c, offset), v); }

SNES_OP uint8_t snes_rd_stack(SnesCpu *c, uint32_t offset) { return snes_rd(c, (c->s + offset) & 0xFFFFu); }
SNES_OP void snes_wr_stack(SnesCpu *c, uint32_t offset, uint8_t v) { snes_wr(c, (c->s + offset) & 0xFFFFu, v); }

/* Stack push/pull wrapping within page 1 in emulation mode... */
SNES_OP void snes_push(SnesCpu *c, unsigned mode, uint8_t v)
{
    snes_wr(c, c->s, v);
    if (SNES_EMU(mode)) c->s = (uint16_t)((c->s & 0xFF00u) | ((c->s - 1u) & 0xFFu));
    else c->s = (uint16_t)(c->s - 1u);
}
SNES_OP uint8_t snes_pull(SnesCpu *c, unsigned mode)
{
    if (SNES_EMU(mode)) c->s = (uint16_t)((c->s & 0xFF00u) | ((c->s + 1u) & 0xFFu));
    else c->s = (uint16_t)(c->s + 1u);
    return snes_rd(c, c->s);
}
/* ...and the 65816-only forms that move all 16 bits (see snes_fix_s). */
SNES_OP void snes_push_n(SnesCpu *c, uint8_t v)
{
    snes_wr(c, c->s, v);
    c->s = (uint16_t)(c->s - 1u);
}
SNES_OP uint8_t snes_pull_n(SnesCpu *c)
{
    c->s = (uint16_t)(c->s + 1u);
    return snes_rd(c, c->s);
}
SNES_OP void snes_fix_s(SnesCpu *c, unsigned mode)
{
    if (SNES_EMU(mode)) c->s = (uint16_t)(0x0100u | (c->s & 0xFFu));
}

SNES_OP void snes_nz8(SnesCpu *c, uint32_t v)
{
    c->flag_z = (v & 0xFFu) == 0;
    c->flag_n = (v >> 7) & 1u;
}
SNES_OP void snes_nz16(SnesCpu *c, uint32_t v)
{
    c->flag_z = (v & 0xFFFFu) == 0;
    c->flag_n = (v >> 15) & 1u;
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */
/* ------------------------------------------------------------------ */

/* Operations that read an operand (A or an index register is the target). */
enum {
    SNES_ALU_ORA, SNES_ALU_AND, SNES_ALU_EOR, SNES_ALU_ADC, SNES_ALU_BIT, SNES_ALU_LDA, SNES_ALU_CMP, SNES_ALU_SBC,
    SNES_ALU_LDX, SNES_ALU_LDY, SNES_ALU_CPX, SNES_ALU_CPY,
};

/* Read-modify-write operations. */
enum { SNES_RMW_ASL, SNES_RMW_ROL, SNES_RMW_LSR, SNES_RMW_ROR, SNES_RMW_INC, SNES_RMW_DEC, SNES_RMW_TSB, SNES_RMW_TRB };

/* Which width an ALU operation uses: accumulator ops follow M, index ops X. */
SNES_OP bool snes_alu_wide(int alu, unsigned mode)
{
    return alu >= SNES_ALU_LDX ? SNES_X16(mode) : SNES_M16(mode);
}

/* ADC/SBC: binary, or BCD in decimal mode (per nibble, with N, V and Z
 * from the adjusted result as the 65C816 does). */
SNES_OP void snes_adc(SnesCpu *c, bool wide, uint32_t data)
{
    int32_t result;
    if (!wide) {
        uint32_t a = c->a & 0xFFu;
        if (!c->flag_d) {
            result = (int32_t)(a + data + c->flag_c);
        } else {
            result = (int32_t)((a & 0x0Fu) + (data & 0x0Fu) + c->flag_c);
            if (result > 0x09) result += 0x06;
            c->flag_c = result > 0x0F;
            result = (int32_t)((a & 0xF0u) + (data & 0xF0u) + ((uint32_t)c->flag_c << 4) + ((uint32_t)result & 0x0Fu));
        }
        c->flag_v = ((~(a ^ data) & (a ^ (uint32_t)result)) >> 7) & 1u;
        if (c->flag_d && result > 0x9F) result += 0x60;
        c->flag_c = result > 0xFF;
        snes_nz8(c, (uint32_t)result);
        c->a = (uint16_t)((c->a & 0xFF00u) | ((uint32_t)result & 0xFFu));
    } else {
        uint32_t a = c->a;
        if (!c->flag_d) {
            result = (int32_t)(a + data + c->flag_c);
        } else {
            result = (int32_t)((a & 0x000Fu) + (data & 0x000Fu) + c->flag_c);
            if (result > 0x0009) result += 0x0006;
            c->flag_c = result > 0x000F;
            result = (int32_t)((a & 0x00F0u) + (data & 0x00F0u) + ((uint32_t)c->flag_c << 4) + ((uint32_t)result & 0x000Fu));
            if (result > 0x009F) result += 0x0060;
            c->flag_c = result > 0x00FF;
            result = (int32_t)((a & 0x0F00u) + (data & 0x0F00u) + ((uint32_t)c->flag_c << 8) + ((uint32_t)result & 0x00FFu));
            if (result > 0x09FF) result += 0x0600;
            c->flag_c = result > 0x0FFF;
            result = (int32_t)((a & 0xF000u) + (data & 0xF000u) + ((uint32_t)c->flag_c << 12) + ((uint32_t)result & 0x0FFFu));
        }
        c->flag_v = ((~(a ^ data) & (a ^ (uint32_t)result)) >> 15) & 1u;
        if (c->flag_d && result > 0x9FFF) result += 0x6000;
        c->flag_c = result > 0xFFFF;
        snes_nz16(c, (uint32_t)result);
        c->a = (uint16_t)result;
    }
}

SNES_OP void snes_sbc(SnesCpu *c, bool wide, uint32_t data)
{
    int32_t result;
    if (!wide) {
        uint32_t a = c->a & 0xFFu;
        data = ~data & 0xFFu;
        if (!c->flag_d) {
            result = (int32_t)(a + data + c->flag_c);
        } else {
            result = (int32_t)((a & 0x0Fu) + (data & 0x0Fu) + c->flag_c);
            if (result <= 0x0F) result -= 0x06;
            c->flag_c = result > 0x0F;
            result = (int32_t)((a & 0xF0u) + (data & 0xF0u) + ((uint32_t)c->flag_c << 4) + ((uint32_t)result & 0x0Fu));
        }
        c->flag_v = ((~(a ^ data) & (a ^ (uint32_t)result)) >> 7) & 1u;
        if (c->flag_d && result <= 0xFF) result -= 0x60;
        c->flag_c = result > 0xFF;
        snes_nz8(c, (uint32_t)result);
        c->a = (uint16_t)((c->a & 0xFF00u) | ((uint32_t)result & 0xFFu));
    } else {
        uint32_t a = c->a;
        data = ~data & 0xFFFFu;
        if (!c->flag_d) {
            result = (int32_t)(a + data + c->flag_c);
        } else {
            result = (int32_t)((a & 0x000Fu) + (data & 0x000Fu) + c->flag_c);
            if (result <= 0x000F) result -= 0x0006;
            c->flag_c = result > 0x000F;
            result = (int32_t)((a & 0x00F0u) + (data & 0x00F0u) + ((uint32_t)c->flag_c << 4) + ((uint32_t)result & 0x000Fu));
            if (result <= 0x00FF) result -= 0x0060;
            c->flag_c = result > 0x00FF;
            result = (int32_t)((a & 0x0F00u) + (data & 0x0F00u) + ((uint32_t)c->flag_c << 8) + ((uint32_t)result & 0x00FFu));
            if (result <= 0x0FFF) result -= 0x0600;
            c->flag_c = result > 0x0FFF;
            result = (int32_t)((a & 0xF000u) + (data & 0xF000u) + ((uint32_t)c->flag_c << 12) + ((uint32_t)result & 0x0FFFu));
        }
        c->flag_v = ((~(a ^ data) & (a ^ (uint32_t)result)) >> 15) & 1u;
        if (c->flag_d && result <= 0xFFFF) result -= 0x6000;
        c->flag_c = result > 0xFFFF;
        snes_nz16(c, (uint32_t)result);
        c->a = (uint16_t)result;
    }
}

SNES_OP void snes_compare(SnesCpu *c, bool wide, uint32_t reg, uint32_t data)
{
    if (!wide) {
        int32_t r = (int32_t)(reg & 0xFFu) - (int32_t)data;
        c->flag_c = r >= 0;
        snes_nz8(c, (uint32_t)r);
    } else {
        int32_t r = (int32_t)reg - (int32_t)data;
        c->flag_c = r >= 0;
        snes_nz16(c, (uint32_t)r);
    }
}

/* Apply a read-type ALU operation to `data` (8 or 16 bits). */
SNES_OP void snes_alu(SnesCpu *c, int alu, bool wide, uint32_t data)
{
    switch (alu) {
    case SNES_ALU_ORA:
        if (wide) { c->a = (uint16_t)(c->a | data); snes_nz16(c, c->a); }
        else { c->a = (uint16_t)(c->a | (data & 0xFFu)); snes_nz8(c, c->a); }
        break;
    case SNES_ALU_AND:
        if (wide) { c->a = (uint16_t)(c->a & data); snes_nz16(c, c->a); }
        else { c->a = (uint16_t)(c->a & (0xFF00u | data)); snes_nz8(c, c->a); }
        break;
    case SNES_ALU_EOR:
        if (wide) { c->a = (uint16_t)(c->a ^ data); snes_nz16(c, c->a); }
        else { c->a = (uint16_t)(c->a ^ (data & 0xFFu)); snes_nz8(c, c->a); }
        break;
    case SNES_ALU_ADC: snes_adc(c, wide, data); break;
    case SNES_ALU_SBC: snes_sbc(c, wide, data); break;
    case SNES_ALU_BIT:
        if (wide) {
            c->flag_z = (data & c->a) == 0;
            c->flag_v = (data >> 14) & 1u;
            c->flag_n = (data >> 15) & 1u;
        } else {
            c->flag_z = (data & c->a & 0xFFu) == 0;
            c->flag_v = (data >> 6) & 1u;
            c->flag_n = (data >> 7) & 1u;
        }
        break;
    case SNES_ALU_LDA:
        if (wide) { c->a = (uint16_t)data; snes_nz16(c, data); }
        else { c->a = (uint16_t)((c->a & 0xFF00u) | data); snes_nz8(c, data); }
        break;
    case SNES_ALU_CMP: snes_compare(c, wide, c->a, data); break;
    case SNES_ALU_LDX:
        if (wide) { c->x = (uint16_t)data; snes_nz16(c, data); }
        else { c->x = (uint16_t)data; snes_nz8(c, data); }
        break;
    case SNES_ALU_LDY:
        if (wide) { c->y = (uint16_t)data; snes_nz16(c, data); }
        else { c->y = (uint16_t)data; snes_nz8(c, data); }
        break;
    case SNES_ALU_CPX: snes_compare(c, wide, c->x, data); break;
    default: snes_compare(c, wide, c->y, data); break; /* CPY */
    }
}

/* Read-modify-write: returns the new value (8 or 16 bits). */
SNES_OP uint32_t snes_rmw(SnesCpu *c, int op, bool wide, uint32_t data)
{
    const uint32_t top = wide ? 0x8000u : 0x80u;
    const uint32_t mask = wide ? 0xFFFFu : 0xFFu;
    uint32_t r;
    switch (op) {
    case SNES_RMW_ASL:
        c->flag_c = (data & top) != 0;
        r = (data << 1) & mask;
        break;
    case SNES_RMW_ROL:
        r = ((data << 1) | c->flag_c) & mask;
        c->flag_c = (data & top) != 0;
        break;
    case SNES_RMW_LSR:
        c->flag_c = data & 1u;
        r = data >> 1;
        break;
    case SNES_RMW_ROR:
        r = (data >> 1) | (c->flag_c ? top : 0u);
        c->flag_c = data & 1u;
        break;
    case SNES_RMW_INC: r = (data + 1u) & mask; break;
    case SNES_RMW_DEC: r = (data - 1u) & mask; break;
    case SNES_RMW_TSB:
        c->flag_z = (data & c->a & mask) == 0;
        return (data | c->a) & mask;
    default: /* TRB */
        c->flag_z = (data & c->a & mask) == 0;
        return data & ~(uint32_t)c->a & mask;
    }
    if (wide) snes_nz16(c, r);
    else snes_nz8(c, r);
    return r;
}

/* ------------------------------------------------------------------ */
/* Read instructions by addressing mode                                */
/* ------------------------------------------------------------------ */

SNES_OP void snes_r_imm(SnesCpu *c, unsigned mode, int alu)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t w;
    if (!wide) {
        SNES_LAST(c);
        w = snes_fetch(c);
    } else {
        w = snes_fetch(c);
        SNES_LAST(c);
        w |= (uint32_t)snes_fetch(c) << 8;
    }
    snes_alu(c, alu, wide, w);
}

/* Finish a read: one or two bytes at `a0` and `a1` (the second address is
 * given, because each mode wraps it differently). */
#define SNES_READ_BODY(c, wide, rd0, rd1, alu)                                                                        \
    do {                                                                                                               \
        uint32_t w_;                                                                                                   \
        if (!(wide)) {                                                                                                 \
            SNES_LAST(c);                                                                                              \
            w_ = rd0;                                                                                                  \
        } else {                                                                                                       \
            w_ = rd0;                                                                                                  \
            SNES_LAST(c);                                                                                              \
            w_ |= (uint32_t)(rd1) << 8;                                                                                \
        }                                                                                                              \
        snes_alu(c, alu, wide, w_);                                                                                    \
    } while (0)

SNES_OP void snes_r_abs(SnesCpu *c, unsigned mode, int alu)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    SNES_READ_BODY(c, wide, snes_rd_bank(c, v), snes_rd_bank(c, v + 1u), alu);
}

SNES_OP void snes_r_absi(SnesCpu *c, unsigned mode, int alu, uint32_t index)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    snes_io_index(c, mode, v, v + index);
    SNES_READ_BODY(c, wide, snes_rd_bank(c, v + index), snes_rd_bank(c, v + index + 1u), alu);
}

SNES_OP void snes_r_long(SnesCpu *c, unsigned mode, int alu, uint32_t index)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    v |= (uint32_t)snes_fetch(c) << 16;
    SNES_READ_BODY(c, wide, snes_rd(c, v + index), snes_rd(c, v + index + 1u), alu);
}

SNES_OP void snes_r_dp(SnesCpu *c, unsigned mode, int alu)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    SNES_READ_BODY(c, wide, snes_rd_dp(c, mode, u), snes_rd_dp(c, mode, u + 1u), alu);
}

SNES_OP void snes_r_dpi(SnesCpu *c, unsigned mode, int alu, uint32_t index)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    snes_io(c);
    SNES_READ_BODY(c, wide, snes_rd_dp(c, mode, u + index), snes_rd_dp(c, mode, u + index + 1u), alu);
}

/* (dp) */
SNES_OP void snes_r_ind(SnesCpu *c, unsigned mode, int alu)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    uint32_t v = snes_rd_dp(c, mode, u);
    v |= (uint32_t)snes_rd_dp(c, mode, u + 1u) << 8;
    SNES_READ_BODY(c, wide, snes_rd_bank(c, v), snes_rd_bank(c, v + 1u), alu);
}

/* (dp,X) */
SNES_OP void snes_r_indx(SnesCpu *c, unsigned mode, int alu)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    snes_io(c);
    uint32_t v = snes_rd_dpx(c, mode, u + c->x, 0);
    v |= (uint32_t)snes_rd_dpx(c, mode, u + c->x, 1) << 8;
    SNES_READ_BODY(c, wide, snes_rd_bank(c, v), snes_rd_bank(c, v + 1u), alu);
}

/* (dp),Y */
SNES_OP void snes_r_indy(SnesCpu *c, unsigned mode, int alu)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    uint32_t v = snes_rd_dp(c, mode, u);
    v |= (uint32_t)snes_rd_dp(c, mode, u + 1u) << 8;
    snes_io_index(c, mode, v, v + c->y);
    SNES_READ_BODY(c, wide, snes_rd_bank(c, v + c->y), snes_rd_bank(c, v + c->y + 1u), alu);
}

/* [dp] and [dp],Y */
SNES_OP void snes_r_indl(SnesCpu *c, unsigned mode, int alu, uint32_t index)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    uint32_t v = snes_rd_dpn(c, u);
    v |= (uint32_t)snes_rd_dpn(c, u + 1u) << 8;
    v |= (uint32_t)snes_rd_dpn(c, u + 2u) << 16;
    SNES_READ_BODY(c, wide, snes_rd(c, v + index), snes_rd(c, v + index + 1u), alu);
}

/* sr,S */
SNES_OP void snes_r_sr(SnesCpu *c, unsigned mode, int alu)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t u = snes_fetch(c);
    snes_io(c);
    SNES_READ_BODY(c, wide, snes_rd_stack(c, u), snes_rd_stack(c, u + 1u), alu);
}

/* (sr,S),Y */
SNES_OP void snes_r_sriy(SnesCpu *c, unsigned mode, int alu)
{
    const bool wide = snes_alu_wide(alu, mode);
    uint32_t u = snes_fetch(c);
    snes_io(c);
    uint32_t v = snes_rd_stack(c, u);
    v |= (uint32_t)snes_rd_stack(c, u + 1u) << 8;
    snes_io(c);
    SNES_READ_BODY(c, wide, snes_rd_bank(c, v + c->y), snes_rd_bank(c, v + c->y + 1u), alu);
}

/* BIT #imm only changes Z. */
SNES_OP void snes_bit_imm(SnesCpu *c, unsigned mode)
{
    uint32_t u;
    if (!SNES_M16(mode)) {
        SNES_LAST(c);
        u = snes_fetch(c);
        c->flag_z = (u & c->a & 0xFFu) == 0;
    } else {
        u = snes_fetch(c);
        SNES_LAST(c);
        u |= (uint32_t)snes_fetch(c) << 8;
        c->flag_z = (u & c->a) == 0;
    }
}

/* ------------------------------------------------------------------ */
/* Write instructions                                                  */
/* ------------------------------------------------------------------ */

/* Store `value` (8 or 16 bits) through two addresses, low byte first. */
#define SNES_WRITE_BODY(c, wide, wr0, wr1)                                                                             \
    do {                                                                                                               \
        if (!(wide)) {                                                                                                 \
            SNES_LAST(c);                                                                                              \
            wr0;                                                                                                       \
        } else {                                                                                                       \
            wr0;                                                                                                       \
            SNES_LAST(c);                                                                                              \
            wr1;                                                                                                       \
        }                                                                                                              \
    } while (0)

SNES_OP void snes_w_abs(SnesCpu *c, bool wide, uint32_t value)
{
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    SNES_WRITE_BODY(c, wide, snes_wr_bank(c, v, (uint8_t)value), snes_wr_bank(c, v + 1u, (uint8_t)(value >> 8)));
}

SNES_OP void snes_w_absi(SnesCpu *c, bool wide, uint32_t value, uint32_t index)
{
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    snes_io(c);
    SNES_WRITE_BODY(c, wide, snes_wr_bank(c, v + index, (uint8_t)value),
                    snes_wr_bank(c, v + index + 1u, (uint8_t)(value >> 8)));
}

SNES_OP void snes_w_long(SnesCpu *c, bool wide, uint32_t index)
{
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    v |= (uint32_t)snes_fetch(c) << 16;
    SNES_WRITE_BODY(c, wide, snes_wr(c, v + index, (uint8_t)c->a), snes_wr(c, v + index + 1u, (uint8_t)(c->a >> 8)));
}

SNES_OP void snes_w_dp(SnesCpu *c, unsigned mode, bool wide, uint32_t value)
{
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    SNES_WRITE_BODY(c, wide, snes_wr_dp(c, mode, u, (uint8_t)value), snes_wr_dp(c, mode, u + 1u, (uint8_t)(value >> 8)));
}

SNES_OP void snes_w_dpi(SnesCpu *c, unsigned mode, bool wide, uint32_t value, uint32_t index)
{
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    snes_io(c);
    SNES_WRITE_BODY(c, wide, snes_wr_dp(c, mode, u + index, (uint8_t)value),
                    snes_wr_dp(c, mode, u + index + 1u, (uint8_t)(value >> 8)));
}

SNES_OP void snes_w_ind(SnesCpu *c, unsigned mode, bool wide)
{
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    uint32_t v = snes_rd_dp(c, mode, u);
    v |= (uint32_t)snes_rd_dp(c, mode, u + 1u) << 8;
    SNES_WRITE_BODY(c, wide, snes_wr_bank(c, v, (uint8_t)c->a), snes_wr_bank(c, v + 1u, (uint8_t)(c->a >> 8)));
}

SNES_OP void snes_w_indx(SnesCpu *c, unsigned mode, bool wide)
{
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    snes_io(c);
    uint32_t v = snes_rd_dpx(c, mode, u + c->x, 0);
    v |= (uint32_t)snes_rd_dpx(c, mode, u + c->x, 1) << 8;
    SNES_WRITE_BODY(c, wide, snes_wr_bank(c, v, (uint8_t)c->a), snes_wr_bank(c, v + 1u, (uint8_t)(c->a >> 8)));
}

SNES_OP void snes_w_indy(SnesCpu *c, unsigned mode, bool wide)
{
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    uint32_t v = snes_rd_dp(c, mode, u);
    v |= (uint32_t)snes_rd_dp(c, mode, u + 1u) << 8;
    snes_io(c);
    SNES_WRITE_BODY(c, wide, snes_wr_bank(c, v + c->y, (uint8_t)c->a), snes_wr_bank(c, v + c->y + 1u, (uint8_t)(c->a >> 8)));
}

SNES_OP void snes_w_indl(SnesCpu *c, bool wide, uint32_t index)
{
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    uint32_t v = snes_rd_dpn(c, u);
    v |= (uint32_t)snes_rd_dpn(c, u + 1u) << 8;
    v |= (uint32_t)snes_rd_dpn(c, u + 2u) << 16;
    SNES_WRITE_BODY(c, wide, snes_wr(c, v + index, (uint8_t)c->a), snes_wr(c, v + index + 1u, (uint8_t)(c->a >> 8)));
}

SNES_OP void snes_w_sr(SnesCpu *c, bool wide)
{
    uint32_t u = snes_fetch(c);
    snes_io(c);
    SNES_WRITE_BODY(c, wide, snes_wr_stack(c, u, (uint8_t)c->a), snes_wr_stack(c, u + 1u, (uint8_t)(c->a >> 8)));
}

SNES_OP void snes_w_sriy(SnesCpu *c, bool wide)
{
    uint32_t u = snes_fetch(c);
    snes_io(c);
    uint32_t v = snes_rd_stack(c, u);
    v |= (uint32_t)snes_rd_stack(c, u + 1u) << 8;
    snes_io(c);
    SNES_WRITE_BODY(c, wide, snes_wr_bank(c, v + c->y, (uint8_t)c->a), snes_wr_bank(c, v + c->y + 1u, (uint8_t)(c->a >> 8)));
}

/* ------------------------------------------------------------------ */
/* Read-modify-write                                                   */
/* ------------------------------------------------------------------ */

/* ASL A, INC A, DEX, INY, ... on a register. */
SNES_OP uint16_t snes_m_reg(SnesCpu *c, int op, bool wide, uint16_t reg)
{
    snes_io_irq(c);
    if (wide) return (uint16_t)snes_rmw(c, op, true, reg);
    return (uint16_t)((reg & 0xFF00u) | snes_rmw(c, op, false, reg & 0xFFu));
}

/* 16-bit RMW writes the high byte first. In emulation mode the modify
 * cycle is a write of the unmodified value (as on the 6502), so an RMW on
 * a hardware register writes it twice. */
SNES_OP void snes_m_abs(SnesCpu *c, unsigned mode, int op)
{
    const bool wide = SNES_M16(mode);
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    uint32_t w = snes_rd_bank(c, v);
    if (wide) w |= (uint32_t)snes_rd_bank(c, v + 1u) << 8;
    if (SNES_EMU(mode)) snes_wr_bank(c, v, (uint8_t)w);
    else snes_io(c);
    w = snes_rmw(c, op, wide, w);
    if (wide) snes_wr_bank(c, v + 1u, (uint8_t)(w >> 8));
    SNES_LAST(c);
    snes_wr_bank(c, v, (uint8_t)w);
}

SNES_OP void snes_m_absx(SnesCpu *c, unsigned mode, int op)
{
    const bool wide = SNES_M16(mode);
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    snes_io(c);
    v += c->x;
    uint32_t w = snes_rd_bank(c, v);
    if (wide) w |= (uint32_t)snes_rd_bank(c, v + 1u) << 8;
    if (SNES_EMU(mode)) snes_wr_bank(c, v, (uint8_t)w);
    else snes_io(c);
    w = snes_rmw(c, op, wide, w);
    if (wide) snes_wr_bank(c, v + 1u, (uint8_t)(w >> 8));
    SNES_LAST(c);
    snes_wr_bank(c, v, (uint8_t)w);
}

SNES_OP void snes_m_dp(SnesCpu *c, unsigned mode, int op)
{
    const bool wide = SNES_M16(mode);
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    uint32_t w = snes_rd_dp(c, mode, u);
    if (wide) w |= (uint32_t)snes_rd_dp(c, mode, u + 1u) << 8;
    if (SNES_EMU(mode)) snes_wr_dp(c, mode, u, (uint8_t)w);
    else snes_io(c);
    w = snes_rmw(c, op, wide, w);
    if (wide) snes_wr_dp(c, mode, u + 1u, (uint8_t)(w >> 8));
    SNES_LAST(c);
    snes_wr_dp(c, mode, u, (uint8_t)w);
}

SNES_OP void snes_m_dpx(SnesCpu *c, unsigned mode, int op)
{
    const bool wide = SNES_M16(mode);
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    snes_io(c);
    u += c->x;
    uint32_t w = snes_rd_dp(c, mode, u);
    if (wide) w |= (uint32_t)snes_rd_dp(c, mode, u + 1u) << 8;
    if (SNES_EMU(mode)) snes_wr_dp(c, mode, u, (uint8_t)w);
    else snes_io(c);
    w = snes_rmw(c, op, wide, w);
    if (wide) snes_wr_dp(c, mode, u + 1u, (uint8_t)(w >> 8));
    SNES_LAST(c);
    snes_wr_dp(c, mode, u, (uint8_t)w);
}

/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

SNES_OP void snes_branch(SnesCpu *c, unsigned mode, bool take)
{
    if (!take) {
        SNES_LAST(c);
        (void)snes_fetch(c);
        return;
    }
    uint8_t u = snes_fetch(c);
    uint16_t target = (uint16_t)(c->pc + (int8_t)u);
    /* Emulation mode adds a cycle when the branch crosses a page. */
    if (SNES_EMU(mode) && (c->pc >> 8) != (target >> 8)) snes_io(c);
    SNES_LAST(c);
    snes_io(c);
    c->pc = target;
    snes_io_branch(c);
    c->branched = 1;
}

SNES_OP void snes_brl(SnesCpu *c)
{
    uint32_t u = snes_fetch(c);
    u |= (uint32_t)snes_fetch(c) << 8;
    SNES_LAST(c);
    snes_io(c);
    c->pc = (uint16_t)(c->pc + u);
    snes_io_branch(c);
    c->branched = 1;
}

SNES_OP void snes_jmp_abs(SnesCpu *c)
{
    uint32_t w = snes_fetch(c);
    SNES_LAST(c);
    w |= (uint32_t)snes_fetch(c) << 8;
    c->pc = (uint16_t)w;
    snes_io_jump(c);
    c->branched = 1;
}

SNES_OP void snes_jml_long(SnesCpu *c)
{
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    SNES_LAST(c);
    v |= (uint32_t)snes_fetch(c) << 16;
    c->pc = (uint16_t)v;
    c->pbr = (uint8_t)(v >> 16);
    snes_io_jump(c);
    c->branched = 1;
}

/* JMP (a): pointer in bank 0. */
SNES_OP void snes_jmp_ind(SnesCpu *c)
{
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    uint32_t w = snes_rd(c, v & 0xFFFFu);
    SNES_LAST(c);
    w |= (uint32_t)snes_rd(c, (v + 1u) & 0xFFFFu) << 8;
    c->pc = (uint16_t)w;
    snes_io_jump(c);
    c->branched = 1;
}

/* JMP (a,X): pointer in the program bank. */
SNES_OP void snes_jmp_indx(SnesCpu *c)
{
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    snes_io(c);
    const uint32_t bank = (uint32_t)c->pbr << 16;
    uint32_t w = snes_rd(c, bank | ((v + c->x) & 0xFFFFu));
    SNES_LAST(c);
    w |= (uint32_t)snes_rd(c, bank | ((v + c->x + 1u) & 0xFFFFu)) << 8;
    c->pc = (uint16_t)w;
    snes_io_jump(c);
    c->branched = 1;
}

/* JML [a]: 24-bit pointer in bank 0. */
SNES_OP void snes_jml_ind(SnesCpu *c)
{
    uint32_t u = snes_fetch(c);
    u |= (uint32_t)snes_fetch(c) << 8;
    uint32_t v = snes_rd(c, u & 0xFFFFu);
    v |= (uint32_t)snes_rd(c, (u + 1u) & 0xFFFFu) << 8;
    SNES_LAST(c);
    v |= (uint32_t)snes_rd(c, (u + 2u) & 0xFFFFu) << 16;
    c->pc = (uint16_t)v;
    c->pbr = (uint8_t)(v >> 16);
    snes_io_jump(c);
    c->branched = 1;
}

/* JSR a: pushes the address of its last byte. */
SNES_OP void snes_jsr_abs(SnesCpu *c, unsigned mode)
{
    uint32_t w = snes_fetch(c);
    w |= (uint32_t)snes_fetch(c) << 8;
    snes_io(c);
    uint16_t ret = (uint16_t)(c->pc - 1u);
    snes_push(c, mode, (uint8_t)(ret >> 8));
    SNES_LAST(c);
    snes_push(c, mode, (uint8_t)ret);
    c->pc = (uint16_t)w;
    snes_io_jump(c);
    c->branched = 1;
}

/* JSL al: pushes PBR, then the address of its last byte. */
SNES_OP void snes_jsl(SnesCpu *c, unsigned mode)
{
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    snes_push_n(c, c->pbr);
    snes_io(c);
    v |= (uint32_t)snes_fetch(c) << 16;
    uint16_t ret = (uint16_t)(c->pc - 1u);
    snes_push_n(c, (uint8_t)(ret >> 8));
    SNES_LAST(c);
    snes_push_n(c, (uint8_t)ret);
    c->pc = (uint16_t)v;
    c->pbr = (uint8_t)(v >> 16);
    snes_fix_s(c, mode);
    snes_io_jump(c);
    c->branched = 1;
}

/* JSR (a,X): pushes the return address between the operand bytes. */
SNES_OP void snes_jsr_indx(SnesCpu *c, unsigned mode)
{
    uint32_t v = snes_fetch(c);
    snes_push_n(c, (uint8_t)(c->pc >> 8));
    snes_push_n(c, (uint8_t)c->pc);
    v |= (uint32_t)snes_fetch(c) << 8;
    snes_io(c);
    const uint32_t bank = (uint32_t)c->pbr << 16;
    uint32_t w = snes_rd(c, bank | ((v + c->x) & 0xFFFFu));
    SNES_LAST(c);
    w |= (uint32_t)snes_rd(c, bank | ((v + c->x + 1u) & 0xFFFFu)) << 8;
    c->pc = (uint16_t)w;
    snes_fix_s(c, mode);
    snes_io_jump(c);
    c->branched = 1;
}

SNES_OP void snes_rti(SnesCpu *c, unsigned mode)
{
    snes_io(c);
    snes_io(c);
    snes_cpu_set_p(c, snes_pull(c, mode));
    uint32_t pc = snes_pull(c, mode);
    if (SNES_EMU(mode)) {
        SNES_LAST(c);
        pc |= (uint32_t)snes_pull(c, mode) << 8;
    } else {
        pc |= (uint32_t)snes_pull(c, mode) << 8;
        SNES_LAST(c);
        c->pbr = snes_pull(c, mode);
    }
    c->pc = (uint16_t)pc;
    snes_io_jump(c);
    c->branched = 1;
}

SNES_OP void snes_rts(SnesCpu *c, unsigned mode)
{
    snes_io(c);
    snes_io(c);
    uint32_t w = snes_pull(c, mode);
    w |= (uint32_t)snes_pull(c, mode) << 8;
    SNES_LAST(c);
    snes_io(c);
    c->pc = (uint16_t)(w + 1u);
    snes_io_jump(c);
    c->branched = 1;
}

SNES_OP void snes_rtl(SnesCpu *c, unsigned mode)
{
    snes_io(c);
    snes_io(c);
    uint32_t v = snes_pull_n(c);
    v |= (uint32_t)snes_pull_n(c) << 8;
    SNES_LAST(c);
    v |= (uint32_t)snes_pull_n(c) << 16;
    c->pc = (uint16_t)(v + 1u);
    c->pbr = (uint8_t)(v >> 16);
    snes_fix_s(c, mode);
    snes_io_jump(c);
    c->branched = 1;
}

/* BRK and COP: signature byte, then the interrupt sequence. In emulation
 * mode the pushed P has B (bit 4) set, since X reads as 1. */
SNES_OP void snes_soft_interrupt(SnesCpu *c, unsigned mode, uint16_t vector)
{
    (void)snes_fetch(c);
    if (!SNES_EMU(mode)) snes_push(c, mode, c->pbr);
    snes_push(c, mode, (uint8_t)(c->pc >> 8));
    snes_push(c, mode, (uint8_t)c->pc);
    snes_push(c, mode, snes_cpu_get_p(c));
    c->flag_i = 1;
    c->flag_d = 0;
    uint32_t pc = snes_rd(c, vector);
    SNES_LAST(c);
    pc |= (uint32_t)snes_rd(c, vector + 1u) << 8;
    c->pc = (uint16_t)pc;
    c->pbr = 0;
    c->branched = 1;
}

/* ------------------------------------------------------------------ */
/* Other instructions                                                  */
/* ------------------------------------------------------------------ */

SNES_OP void snes_xba(SnesCpu *c)
{
    snes_io(c);
    SNES_LAST(c);
    snes_io(c);
    c->a = (uint16_t)((c->a >> 8) | (c->a << 8));
    snes_nz8(c, c->a);
}

/* MVN/MVP: one byte per execution; the instruction repeats itself (PC
 * steps back) until A underflows. DBR becomes the destination bank. */
SNES_OP void snes_block_move(SnesCpu *c, unsigned mode, int step)
{
    uint8_t dst = snes_fetch(c);
    uint8_t src = snes_fetch(c);
    c->dbr = dst;
    uint8_t v = snes_rd(c, ((uint32_t)src << 16) | c->x);
    snes_wr(c, ((uint32_t)dst << 16) | c->y, v);
    snes_io(c);
    if (SNES_X16(mode)) {
        c->x = (uint16_t)(c->x + step);
        c->y = (uint16_t)(c->y + step);
    } else {
        c->x = (uint16_t)((c->x + step) & 0xFFu);
        c->y = (uint16_t)((c->y + step) & 0xFFu);
    }
    SNES_LAST(c);
    snes_io(c);
    if (c->a-- != 0) {
        c->pc = (uint16_t)(c->pc - 3u);
        c->branched = 1; /* re-executes itself */
    }
}

SNES_OP void snes_rep_sep(SnesCpu *c, bool set)
{
    uint8_t w = snes_fetch(c);
    SNES_LAST(c);
    snes_io(c);
    uint8_t p = snes_cpu_get_p(c);
    snes_cpu_set_p(c, set ? (uint8_t)(p | w) : (uint8_t)(p & ~w));
}

SNES_OP void snes_xce(SnesCpu *c)
{
    snes_io_irq(c);
    uint8_t carry = c->flag_c;
    c->flag_c = c->flag_e;
    c->flag_e = carry;
    if (c->flag_e) {
        c->flag_x = c->flag_m = 1;
        c->x &= 0xFFu;
        c->y &= 0xFFu;
        c->s = (uint16_t)(0x0100u | (c->s & 0xFFu));
    }
}

/* Register transfers (TAX, TXA, TYX, ...). Width is the destination's. */
SNES_OP uint16_t snes_transfer(SnesCpu *c, bool wide, uint16_t from, uint16_t to)
{
    snes_io_irq(c);
    if (wide) {
        snes_nz16(c, from);
        return from;
    }
    snes_nz8(c, from);
    return (uint16_t)((to & 0xFF00u) | (from & 0xFFu));
}

SNES_OP void snes_push_reg(SnesCpu *c, unsigned mode, bool wide, uint16_t v)
{
    snes_io(c);
    if (wide) snes_push(c, mode, (uint8_t)(v >> 8));
    SNES_LAST(c);
    snes_push(c, mode, (uint8_t)v);
}

SNES_OP uint16_t snes_pull_reg(SnesCpu *c, unsigned mode, bool wide, uint16_t old)
{
    snes_io(c);
    snes_io(c);
    if (!wide) {
        SNES_LAST(c);
        uint8_t v = snes_pull(c, mode);
        snes_nz8(c, v);
        return (uint16_t)((old & 0xFF00u) | v);
    }
    uint32_t v = snes_pull(c, mode);
    SNES_LAST(c);
    v |= (uint32_t)snes_pull(c, mode) << 8;
    snes_nz16(c, v);
    return (uint16_t)v;
}

SNES_OP void snes_phd(SnesCpu *c, unsigned mode)
{
    snes_io(c);
    snes_push_n(c, (uint8_t)(c->d >> 8));
    SNES_LAST(c);
    snes_push_n(c, (uint8_t)c->d);
    snes_fix_s(c, mode);
}

SNES_OP void snes_pld(SnesCpu *c, unsigned mode)
{
    snes_io(c);
    snes_io(c);
    uint32_t v = snes_pull_n(c);
    SNES_LAST(c);
    v |= (uint32_t)snes_pull_n(c) << 8;
    c->d = (uint16_t)v;
    snes_nz16(c, v);
    snes_fix_s(c, mode);
}

SNES_OP void snes_plb(SnesCpu *c, unsigned mode)
{
    snes_io(c);
    snes_io(c);
    SNES_LAST(c);
    c->dbr = snes_pull_n(c);
    snes_nz8(c, c->dbr);
    snes_fix_s(c, mode);
}

SNES_OP void snes_plp(SnesCpu *c, unsigned mode)
{
    snes_io(c);
    snes_io(c);
    SNES_LAST(c);
    snes_cpu_set_p(c, snes_pull(c, mode));
}

SNES_OP void snes_pea(SnesCpu *c, unsigned mode)
{
    uint32_t w = snes_fetch(c);
    w |= (uint32_t)snes_fetch(c) << 8;
    snes_push_n(c, (uint8_t)(w >> 8));
    SNES_LAST(c);
    snes_push_n(c, (uint8_t)w);
    snes_fix_s(c, mode);
}

SNES_OP void snes_pei(SnesCpu *c, unsigned mode)
{
    uint32_t u = snes_fetch(c);
    snes_io_dl(c);
    uint32_t w = snes_rd_dpn(c, u);
    w |= (uint32_t)snes_rd_dpn(c, u + 1u) << 8;
    snes_push_n(c, (uint8_t)(w >> 8));
    SNES_LAST(c);
    snes_push_n(c, (uint8_t)w);
    snes_fix_s(c, mode);
}

SNES_OP void snes_per(SnesCpu *c, unsigned mode)
{
    uint32_t v = snes_fetch(c);
    v |= (uint32_t)snes_fetch(c) << 8;
    snes_io(c);
    uint16_t w = (uint16_t)(c->pc + v);
    snes_push_n(c, (uint8_t)(w >> 8));
    SNES_LAST(c);
    snes_push_n(c, (uint8_t)w);
    snes_fix_s(c, mode);
}

/* WDM: a two-byte no-op whose second cycle is internal (the operand byte
 * is skipped, not read). */
SNES_OP void snes_wdm(SnesCpu *c)
{
    c->pc = (uint16_t)(c->pc + 1u);
    SNES_LAST(c);
    snes_io(c);
}

/* WAI and STP only raise a state here; the run loop sleeps. */
SNES_OP void snes_wai(SnesCpu *c)
{
    snes_io(c);
    SNES_LAST(c);
    snes_io(c);
    c->wai = 1;
    c->branched = 1;
}

SNES_OP void snes_stp(SnesCpu *c)
{
    snes_io(c);
    SNES_LAST(c);
    snes_io(c);
    c->stp = 1;
    c->branched = 1;
}

/* ------------------------------------------------------------------ */
/* Opcode table                                                        */
/* ------------------------------------------------------------------ */

/* X(opcode, body). `c` and `mode` are in scope in the body. */
#define SNES_M16_ (SNES_M16(mode))
#define SNES_X16_ (SNES_X16(mode))
#define SNES_OPCODES(X)                                                                                                \
    X(00, snes_soft_interrupt(c, mode, (uint16_t)(SNES_EMU(mode) ? SNES_VEC_IRQ_E : SNES_VEC_BRK_N)))                 \
    X(01, snes_r_indx(c, mode, SNES_ALU_ORA))                                                                           \
    X(02, snes_soft_interrupt(c, mode, (uint16_t)(SNES_EMU(mode) ? SNES_VEC_COP_E : SNES_VEC_COP_N)))                 \
    X(03, snes_r_sr(c, mode, SNES_ALU_ORA))                                                                             \
    X(04, snes_m_dp(c, mode, SNES_RMW_TSB))                                                                             \
    X(05, snes_r_dp(c, mode, SNES_ALU_ORA))                                                                             \
    X(06, snes_m_dp(c, mode, SNES_RMW_ASL))                                                                             \
    X(07, snes_r_indl(c, mode, SNES_ALU_ORA, 0))                                                                        \
    X(08, snes_push_reg(c, mode, false, snes_cpu_get_p(c)))                                                             \
    X(09, snes_r_imm(c, mode, SNES_ALU_ORA))                                                                            \
    X(0A, c->a = snes_m_reg(c, SNES_RMW_ASL, SNES_M16_, c->a))                                                          \
    X(0B, snes_phd(c, mode))                                                                                            \
    X(0C, snes_m_abs(c, mode, SNES_RMW_TSB))                                                                            \
    X(0D, snes_r_abs(c, mode, SNES_ALU_ORA))                                                                            \
    X(0E, snes_m_abs(c, mode, SNES_RMW_ASL))                                                                            \
    X(0F, snes_r_long(c, mode, SNES_ALU_ORA, 0))                                                                        \
    X(10, snes_branch(c, mode, !c->flag_n))                                                                             \
    X(11, snes_r_indy(c, mode, SNES_ALU_ORA))                                                                           \
    X(12, snes_r_ind(c, mode, SNES_ALU_ORA))                                                                            \
    X(13, snes_r_sriy(c, mode, SNES_ALU_ORA))                                                                           \
    X(14, snes_m_dp(c, mode, SNES_RMW_TRB))                                                                             \
    X(15, snes_r_dpi(c, mode, SNES_ALU_ORA, c->x))                                                                      \
    X(16, snes_m_dpx(c, mode, SNES_RMW_ASL))                                                                            \
    X(17, snes_r_indl(c, mode, SNES_ALU_ORA, c->y))                                                                     \
    X(18, (snes_io_irq(c), c->flag_c = 0))                                                                              \
    X(19, snes_r_absi(c, mode, SNES_ALU_ORA, c->y))                                                                     \
    X(1A, c->a = snes_m_reg(c, SNES_RMW_INC, SNES_M16_, c->a))                                                          \
    X(1B, (snes_io_irq(c), c->s = SNES_EMU(mode) ? (uint16_t)(0x0100u | (c->a & 0xFFu)) : c->a))                      \
    X(1C, snes_m_abs(c, mode, SNES_RMW_TRB))                                                                            \
    X(1D, snes_r_absi(c, mode, SNES_ALU_ORA, c->x))                                                                     \
    X(1E, snes_m_absx(c, mode, SNES_RMW_ASL))                                                                           \
    X(1F, snes_r_long(c, mode, SNES_ALU_ORA, c->x))                                                                     \
    X(20, snes_jsr_abs(c, mode))                                                                                        \
    X(21, snes_r_indx(c, mode, SNES_ALU_AND))                                                                           \
    X(22, snes_jsl(c, mode))                                                                                            \
    X(23, snes_r_sr(c, mode, SNES_ALU_AND))                                                                             \
    X(24, snes_r_dp(c, mode, SNES_ALU_BIT))                                                                             \
    X(25, snes_r_dp(c, mode, SNES_ALU_AND))                                                                             \
    X(26, snes_m_dp(c, mode, SNES_RMW_ROL))                                                                             \
    X(27, snes_r_indl(c, mode, SNES_ALU_AND, 0))                                                                        \
    X(28, snes_plp(c, mode))                                                                                            \
    X(29, snes_r_imm(c, mode, SNES_ALU_AND))                                                                            \
    X(2A, c->a = snes_m_reg(c, SNES_RMW_ROL, SNES_M16_, c->a))                                                          \
    X(2B, snes_pld(c, mode))                                                                                            \
    X(2C, snes_r_abs(c, mode, SNES_ALU_BIT))                                                                            \
    X(2D, snes_r_abs(c, mode, SNES_ALU_AND))                                                                            \
    X(2E, snes_m_abs(c, mode, SNES_RMW_ROL))                                                                            \
    X(2F, snes_r_long(c, mode, SNES_ALU_AND, 0))                                                                        \
    X(30, snes_branch(c, mode, c->flag_n))                                                                              \
    X(31, snes_r_indy(c, mode, SNES_ALU_AND))                                                                           \
    X(32, snes_r_ind(c, mode, SNES_ALU_AND))                                                                            \
    X(33, snes_r_sriy(c, mode, SNES_ALU_AND))                                                                           \
    X(34, snes_r_dpi(c, mode, SNES_ALU_BIT, c->x))                                                                      \
    X(35, snes_r_dpi(c, mode, SNES_ALU_AND, c->x))                                                                      \
    X(36, snes_m_dpx(c, mode, SNES_RMW_ROL))                                                                            \
    X(37, snes_r_indl(c, mode, SNES_ALU_AND, c->y))                                                                     \
    X(38, (snes_io_irq(c), c->flag_c = 1))                                                                              \
    X(39, snes_r_absi(c, mode, SNES_ALU_AND, c->y))                                                                     \
    X(3A, c->a = snes_m_reg(c, SNES_RMW_DEC, SNES_M16_, c->a))                                                          \
    X(3B, c->a = snes_transfer(c, true, c->s, c->a))                                                                    \
    X(3C, snes_r_absi(c, mode, SNES_ALU_BIT, c->x))                                                                     \
    X(3D, snes_r_absi(c, mode, SNES_ALU_AND, c->x))                                                                     \
    X(3E, snes_m_absx(c, mode, SNES_RMW_ROL))                                                                           \
    X(3F, snes_r_long(c, mode, SNES_ALU_AND, c->x))                                                                     \
    X(40, snes_rti(c, mode))                                                                                            \
    X(41, snes_r_indx(c, mode, SNES_ALU_EOR))                                                                           \
    X(42, snes_wdm(c))                                                                                                  \
    X(43, snes_r_sr(c, mode, SNES_ALU_EOR))                                                                             \
    X(44, snes_block_move(c, mode, -1))                                                                                 \
    X(45, snes_r_dp(c, mode, SNES_ALU_EOR))                                                                             \
    X(46, snes_m_dp(c, mode, SNES_RMW_LSR))                                                                             \
    X(47, snes_r_indl(c, mode, SNES_ALU_EOR, 0))                                                                        \
    X(48, snes_push_reg(c, mode, SNES_M16_, c->a))                                                                      \
    X(49, snes_r_imm(c, mode, SNES_ALU_EOR))                                                                            \
    X(4A, c->a = snes_m_reg(c, SNES_RMW_LSR, SNES_M16_, c->a))                                                          \
    X(4B, snes_push_reg(c, mode, false, c->pbr))                                                                        \
    X(4C, snes_jmp_abs(c))                                                                                              \
    X(4D, snes_r_abs(c, mode, SNES_ALU_EOR))                                                                            \
    X(4E, snes_m_abs(c, mode, SNES_RMW_LSR))                                                                            \
    X(4F, snes_r_long(c, mode, SNES_ALU_EOR, 0))                                                                        \
    X(50, snes_branch(c, mode, !c->flag_v))                                                                             \
    X(51, snes_r_indy(c, mode, SNES_ALU_EOR))                                                                           \
    X(52, snes_r_ind(c, mode, SNES_ALU_EOR))                                                                            \
    X(53, snes_r_sriy(c, mode, SNES_ALU_EOR))                                                                           \
    X(54, snes_block_move(c, mode, 1))                                                                                  \
    X(55, snes_r_dpi(c, mode, SNES_ALU_EOR, c->x))                                                                      \
    X(56, snes_m_dpx(c, mode, SNES_RMW_LSR))                                                                            \
    X(57, snes_r_indl(c, mode, SNES_ALU_EOR, c->y))                                                                     \
    X(58, (snes_io_irq(c), c->flag_i = 0))                                                                              \
    X(59, snes_r_absi(c, mode, SNES_ALU_EOR, c->y))                                                                     \
    X(5A, snes_push_reg(c, mode, SNES_X16_, c->y))                                                                      \
    X(5B, c->d = snes_transfer(c, true, c->a, c->d))                                                                    \
    X(5C, snes_jml_long(c))                                                                                             \
    X(5D, snes_r_absi(c, mode, SNES_ALU_EOR, c->x))                                                                     \
    X(5E, snes_m_absx(c, mode, SNES_RMW_LSR))                                                                           \
    X(5F, snes_r_long(c, mode, SNES_ALU_EOR, c->x))                                                                     \
    X(60, snes_rts(c, mode))                                                                                            \
    X(61, snes_r_indx(c, mode, SNES_ALU_ADC))                                                                           \
    X(62, snes_per(c, mode))                                                                                            \
    X(63, snes_r_sr(c, mode, SNES_ALU_ADC))                                                                             \
    X(64, snes_w_dp(c, mode, SNES_M16_, 0))                                                                             \
    X(65, snes_r_dp(c, mode, SNES_ALU_ADC))                                                                             \
    X(66, snes_m_dp(c, mode, SNES_RMW_ROR))                                                                             \
    X(67, snes_r_indl(c, mode, SNES_ALU_ADC, 0))                                                                        \
    X(68, c->a = snes_pull_reg(c, mode, SNES_M16_, c->a))                                                               \
    X(69, snes_r_imm(c, mode, SNES_ALU_ADC))                                                                            \
    X(6A, c->a = snes_m_reg(c, SNES_RMW_ROR, SNES_M16_, c->a))                                                          \
    X(6B, snes_rtl(c, mode))                                                                                            \
    X(6C, snes_jmp_ind(c))                                                                                              \
    X(6D, snes_r_abs(c, mode, SNES_ALU_ADC))                                                                            \
    X(6E, snes_m_abs(c, mode, SNES_RMW_ROR))                                                                            \
    X(6F, snes_r_long(c, mode, SNES_ALU_ADC, 0))                                                                        \
    X(70, snes_branch(c, mode, c->flag_v))                                                                              \
    X(71, snes_r_indy(c, mode, SNES_ALU_ADC))                                                                           \
    X(72, snes_r_ind(c, mode, SNES_ALU_ADC))                                                                            \
    X(73, snes_r_sriy(c, mode, SNES_ALU_ADC))                                                                           \
    X(74, snes_w_dpi(c, mode, SNES_M16_, 0, c->x))                                                                      \
    X(75, snes_r_dpi(c, mode, SNES_ALU_ADC, c->x))                                                                      \
    X(76, snes_m_dpx(c, mode, SNES_RMW_ROR))                                                                            \
    X(77, snes_r_indl(c, mode, SNES_ALU_ADC, c->y))                                                                     \
    X(78, (snes_io_irq(c), c->flag_i = 1))                                                                              \
    X(79, snes_r_absi(c, mode, SNES_ALU_ADC, c->y))                                                                     \
    X(7A, c->y = snes_pull_reg(c, mode, SNES_X16_, c->y))                                                               \
    X(7B, c->a = snes_transfer(c, true, c->d, c->a))                                                                    \
    X(7C, snes_jmp_indx(c))                                                                                             \
    X(7D, snes_r_absi(c, mode, SNES_ALU_ADC, c->x))                                                                     \
    X(7E, snes_m_absx(c, mode, SNES_RMW_ROR))                                                                           \
    X(7F, snes_r_long(c, mode, SNES_ALU_ADC, c->x))                                                                     \
    X(80, snes_branch(c, mode, true))                                                                                   \
    X(81, snes_w_indx(c, mode, SNES_M16_))                                                                              \
    X(82, snes_brl(c))                                                                                                  \
    X(83, snes_w_sr(c, SNES_M16_))                                                                                      \
    X(84, snes_w_dp(c, mode, SNES_X16_, c->y))                                                                          \
    X(85, snes_w_dp(c, mode, SNES_M16_, c->a))                                                                          \
    X(86, snes_w_dp(c, mode, SNES_X16_, c->x))                                                                          \
    X(87, snes_w_indl(c, SNES_M16_, 0))                                                                                 \
    X(88, c->y = snes_m_reg(c, SNES_RMW_DEC, SNES_X16_, c->y))                                                          \
    X(89, snes_bit_imm(c, mode))                                                                                        \
    X(8A, c->a = snes_transfer(c, SNES_M16_, c->x, c->a))                                                              \
    X(8B, snes_push_reg(c, mode, false, c->dbr))                                                                        \
    X(8C, snes_w_abs(c, SNES_X16_, c->y))                                                                               \
    X(8D, snes_w_abs(c, SNES_M16_, c->a))                                                                               \
    X(8E, snes_w_abs(c, SNES_X16_, c->x))                                                                               \
    X(8F, snes_w_long(c, SNES_M16_, 0))                                                                                 \
    X(90, snes_branch(c, mode, !c->flag_c))                                                                             \
    X(91, snes_w_indy(c, mode, SNES_M16_))                                                                              \
    X(92, snes_w_ind(c, mode, SNES_M16_))                                                                               \
    X(93, snes_w_sriy(c, SNES_M16_))                                                                                    \
    X(94, snes_w_dpi(c, mode, SNES_X16_, c->y, c->x))                                                                   \
    X(95, snes_w_dpi(c, mode, SNES_M16_, c->a, c->x))                                                                   \
    X(96, snes_w_dpi(c, mode, SNES_X16_, c->x, c->y))                                                                   \
    X(97, snes_w_indl(c, SNES_M16_, c->y))                                                                              \
    X(98, c->a = snes_transfer(c, SNES_M16_, c->y, c->a))                                                              \
    X(99, snes_w_absi(c, SNES_M16_, c->a, c->y))                                                                        \
    X(9A, (snes_io_irq(c), c->s = SNES_EMU(mode) ? (uint16_t)(0x0100u | (c->x & 0xFFu)) : c->x))                      \
    X(9B, c->y = snes_transfer(c, SNES_X16_, c->x, c->y))                                                              \
    X(9C, snes_w_abs(c, SNES_M16_, 0))                                                                                  \
    X(9D, snes_w_absi(c, SNES_M16_, c->a, c->x))                                                                        \
    X(9E, snes_w_absi(c, SNES_M16_, 0, c->x))                                                                           \
    X(9F, snes_w_long(c, SNES_M16_, c->x))                                                                              \
    X(A0, snes_r_imm(c, mode, SNES_ALU_LDY))                                                                            \
    X(A1, snes_r_indx(c, mode, SNES_ALU_LDA))                                                                           \
    X(A2, snes_r_imm(c, mode, SNES_ALU_LDX))                                                                            \
    X(A3, snes_r_sr(c, mode, SNES_ALU_LDA))                                                                             \
    X(A4, snes_r_dp(c, mode, SNES_ALU_LDY))                                                                             \
    X(A5, snes_r_dp(c, mode, SNES_ALU_LDA))                                                                             \
    X(A6, snes_r_dp(c, mode, SNES_ALU_LDX))                                                                             \
    X(A7, snes_r_indl(c, mode, SNES_ALU_LDA, 0))                                                                        \
    X(A8, c->y = snes_transfer(c, SNES_X16_, c->a, c->y))                                                              \
    X(A9, snes_r_imm(c, mode, SNES_ALU_LDA))                                                                            \
    X(AA, c->x = snes_transfer(c, SNES_X16_, c->a, c->x))                                                              \
    X(AB, snes_plb(c, mode))                                                                                            \
    X(AC, snes_r_abs(c, mode, SNES_ALU_LDY))                                                                            \
    X(AD, snes_r_abs(c, mode, SNES_ALU_LDA))                                                                            \
    X(AE, snes_r_abs(c, mode, SNES_ALU_LDX))                                                                            \
    X(AF, snes_r_long(c, mode, SNES_ALU_LDA, 0))                                                                        \
    X(B0, snes_branch(c, mode, c->flag_c))                                                                              \
    X(B1, snes_r_indy(c, mode, SNES_ALU_LDA))                                                                           \
    X(B2, snes_r_ind(c, mode, SNES_ALU_LDA))                                                                            \
    X(B3, snes_r_sriy(c, mode, SNES_ALU_LDA))                                                                           \
    X(B4, snes_r_dpi(c, mode, SNES_ALU_LDY, c->x))                                                                      \
    X(B5, snes_r_dpi(c, mode, SNES_ALU_LDA, c->x))                                                                      \
    X(B6, snes_r_dpi(c, mode, SNES_ALU_LDX, c->y))                                                                      \
    X(B7, snes_r_indl(c, mode, SNES_ALU_LDA, c->y))                                                                     \
    X(B8, (snes_io_irq(c), c->flag_v = 0))                                                                              \
    X(B9, snes_r_absi(c, mode, SNES_ALU_LDA, c->y))                                                                     \
    X(BA, c->x = snes_transfer(c, SNES_X16_, c->s, c->x))                                                              \
    X(BB, c->x = snes_transfer(c, SNES_X16_, c->y, c->x))                                                              \
    X(BC, snes_r_absi(c, mode, SNES_ALU_LDY, c->x))                                                                     \
    X(BD, snes_r_absi(c, mode, SNES_ALU_LDA, c->x))                                                                     \
    X(BE, snes_r_absi(c, mode, SNES_ALU_LDX, c->y))                                                                     \
    X(BF, snes_r_long(c, mode, SNES_ALU_LDA, c->x))                                                                     \
    X(C0, snes_r_imm(c, mode, SNES_ALU_CPY))                                                                            \
    X(C1, snes_r_indx(c, mode, SNES_ALU_CMP))                                                                           \
    X(C2, snes_rep_sep(c, false))                                                                                       \
    X(C3, snes_r_sr(c, mode, SNES_ALU_CMP))                                                                             \
    X(C4, snes_r_dp(c, mode, SNES_ALU_CPY))                                                                             \
    X(C5, snes_r_dp(c, mode, SNES_ALU_CMP))                                                                             \
    X(C6, snes_m_dp(c, mode, SNES_RMW_DEC))                                                                             \
    X(C7, snes_r_indl(c, mode, SNES_ALU_CMP, 0))                                                                        \
    X(C8, c->y = snes_m_reg(c, SNES_RMW_INC, SNES_X16_, c->y))                                                          \
    X(C9, snes_r_imm(c, mode, SNES_ALU_CMP))                                                                            \
    X(CA, c->x = snes_m_reg(c, SNES_RMW_DEC, SNES_X16_, c->x))                                                          \
    X(CB, snes_wai(c))                                                                                                  \
    X(CC, snes_r_abs(c, mode, SNES_ALU_CPY))                                                                            \
    X(CD, snes_r_abs(c, mode, SNES_ALU_CMP))                                                                            \
    X(CE, snes_m_abs(c, mode, SNES_RMW_DEC))                                                                            \
    X(CF, snes_r_long(c, mode, SNES_ALU_CMP, 0))                                                                        \
    X(D0, snes_branch(c, mode, !c->flag_z))                                                                             \
    X(D1, snes_r_indy(c, mode, SNES_ALU_CMP))                                                                           \
    X(D2, snes_r_ind(c, mode, SNES_ALU_CMP))                                                                            \
    X(D3, snes_r_sriy(c, mode, SNES_ALU_CMP))                                                                           \
    X(D4, snes_pei(c, mode))                                                                                            \
    X(D5, snes_r_dpi(c, mode, SNES_ALU_CMP, c->x))                                                                      \
    X(D6, snes_m_dpx(c, mode, SNES_RMW_DEC))                                                                            \
    X(D7, snes_r_indl(c, mode, SNES_ALU_CMP, c->y))                                                                     \
    X(D8, (snes_io_irq(c), c->flag_d = 0))                                                                              \
    X(D9, snes_r_absi(c, mode, SNES_ALU_CMP, c->y))                                                                     \
    X(DA, snes_push_reg(c, mode, SNES_X16_, c->x))                                                                      \
    X(DB, snes_stp(c))                                                                                                  \
    X(DC, snes_jml_ind(c))                                                                                              \
    X(DD, snes_r_absi(c, mode, SNES_ALU_CMP, c->x))                                                                     \
    X(DE, snes_m_absx(c, mode, SNES_RMW_DEC))                                                                           \
    X(DF, snes_r_long(c, mode, SNES_ALU_CMP, c->x))                                                                     \
    X(E0, snes_r_imm(c, mode, SNES_ALU_CPX))                                                                            \
    X(E1, snes_r_indx(c, mode, SNES_ALU_SBC))                                                                           \
    X(E2, snes_rep_sep(c, true))                                                                                        \
    X(E3, snes_r_sr(c, mode, SNES_ALU_SBC))                                                                             \
    X(E4, snes_r_dp(c, mode, SNES_ALU_CPX))                                                                             \
    X(E5, snes_r_dp(c, mode, SNES_ALU_SBC))                                                                             \
    X(E6, snes_m_dp(c, mode, SNES_RMW_INC))                                                                             \
    X(E7, snes_r_indl(c, mode, SNES_ALU_SBC, 0))                                                                        \
    X(E8, c->x = snes_m_reg(c, SNES_RMW_INC, SNES_X16_, c->x))                                                          \
    X(E9, snes_r_imm(c, mode, SNES_ALU_SBC))                                                                            \
    X(EA, snes_io_irq(c))                                                                                               \
    X(EB, snes_xba(c))                                                                                                  \
    X(EC, snes_r_abs(c, mode, SNES_ALU_CPX))                                                                            \
    X(ED, snes_r_abs(c, mode, SNES_ALU_SBC))                                                                            \
    X(EE, snes_m_abs(c, mode, SNES_RMW_INC))                                                                            \
    X(EF, snes_r_long(c, mode, SNES_ALU_SBC, 0))                                                                        \
    X(F0, snes_branch(c, mode, c->flag_z))                                                                              \
    X(F1, snes_r_indy(c, mode, SNES_ALU_SBC))                                                                           \
    X(F2, snes_r_ind(c, mode, SNES_ALU_SBC))                                                                            \
    X(F3, snes_r_sriy(c, mode, SNES_ALU_SBC))                                                                           \
    X(F4, snes_pea(c, mode))                                                                                            \
    X(F5, snes_r_dpi(c, mode, SNES_ALU_SBC, c->x))                                                                      \
    X(F6, snes_m_dpx(c, mode, SNES_RMW_INC))                                                                            \
    X(F7, snes_r_indl(c, mode, SNES_ALU_SBC, c->y))                                                                     \
    X(F8, (snes_io_irq(c), c->flag_d = 1))                                                                              \
    X(F9, snes_r_absi(c, mode, SNES_ALU_SBC, c->y))                                                                     \
    X(FA, c->x = snes_pull_reg(c, mode, SNES_X16_, c->x))                                                               \
    X(FB, snes_xce(c))                                                                                                  \
    X(FC, snes_jsr_indx(c, mode))                                                                                       \
    X(FD, snes_r_absi(c, mode, SNES_ALU_SBC, c->x))                                                                     \
    X(FE, snes_m_absx(c, mode, SNES_RMW_INC))                                                                           \
    X(FF, snes_r_long(c, mode, SNES_ALU_SBC, c->x))

/* snes_op_XX(c, mode): one function per opcode, each file calling one
 * out-of-line copy. Force-inlining them after every recompiled instruction
 * and into the interpreter's switch was almost all of the C compiler's work
 * (Kirby's Dream Land 3: 55 s -> 6.6 s to build) and bought nothing: the
 * recompiled game runs as fast, since the PPU, APU and coprocessors dominate
 * its time, and the interpreter runs about 1.5x faster without the giant
 * inlined switch. Projects generated with snesrecomp --inline-handlers
 * define SNES_RC_INLINE_OPS to inline them again. */
#if defined(SNES_RC_INLINE_OPS)
#define SNES_OP_ENTRY SNES_OP
#elif defined(_MSC_VER)
#define SNES_OP_ENTRY static __declspec(noinline)
#else
#define SNES_OP_ENTRY static __attribute__((noinline, unused))
#endif
#define SNES_DEFINE_OP(hex, body)                                                                                      \
    SNES_OP_ENTRY void snes_op_##hex(SnesCpu *c, unsigned mode)                                                        \
    {                                                                                                                  \
        (void)mode;                                                                                                    \
        body;                                                                                                          \
    }
SNES_OPCODES(SNES_DEFINE_OP)
#undef SNES_DEFINE_OP

#ifdef __cplusplus
}
#endif

#endif /* SNES_CPU_IMPL_H */
