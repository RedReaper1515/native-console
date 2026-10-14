/*
 * 6502 instruction handlers for the Ricoh 2A03: the only implementation of
 * instruction semantics.
 *
 * The interpreter (nes_cpu.c) fetches an opcode and calls nes_op_XX(c).
 * Recompiled code makes the same opcode fetch at a known address and calls
 * the same nes_op_XX, so both make the same bus cycles in the same order.
 * The SingleStepTests nes6502 vectors validate the handlers through the
 * interpreter (tests/nes/sst_6502.c): registers, flags, memory and every
 * bus cycle of all 256 opcodes.
 *
 * All 256 opcodes are implemented, the unofficial ones included. Games use
 * the stable ones (SLO, RLA, SRE, RRA, SAX, LAX, DCP, ISC, ANC, ALR, ARR,
 * SBX, the NOPs with operands, SBC #imm at $EB). The unstable ones depend on
 * analogue effects inside the chip; they behave here as the vectors record
 * them (see "Unstable opcodes" below).
 *
 * References: the NESdev wiki ("CPU interrupts", "CPU unofficial opcodes")
 * and, for the reset and interrupt sequences, ares's mos6502 (ISC license,
 * Near et al.); see THIRD_PARTY.md. The code is our own.
 *
 * Contract: c->pc points after the opcode byte (the caller fetched it).
 * Handlers that transfer control set c->branched and leave the target in
 * c->pc: JMP, JSR, RTS, RTI, BRK, a JAM opcode, and a conditional branch
 * when it is taken (a branch that falls through leaves it alone). Nothing
 * here clears it; recompiled code tests and clears it, then leaves or jumps
 * to a known label. NES_POLL marks the instruction's interrupt poll
 * (c->poll_at, c->poll_i; nes_cpu.h explains how the machine uses them).
 *
 * What the vectors pin (and games rely on):
 *  - Zero-page indexing wraps within page 0, and so does the +1 of a
 *    (zp,X) or (zp),Y pointer's high byte.
 *  - JMP ($xxFF) takes its high byte from $xx00.
 *  - abs,X, abs,Y and (zp),Y first read from the address without the carry
 *    into the high byte: a read instruction only when there is a carry,
 *    a store or read-modify-write always.
 *  - A read-modify-write writes the unmodified value back, then the new
 *    one. Hardware registers see both writes.
 *  - The stack wraps within page 1.
 *  - ADC and SBC are binary whatever D says.
 */
#ifndef NES_CPU_IMPL_H
#define NES_CPU_IMPL_H

#include "nes_bus.h"
#include "nes_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define NES_OP static __forceinline
#else
#define NES_OP static inline __attribute__((always_inline))
#endif

/* The interrupt poll: called just before the bus cycle that follows it, so
 * c->cycles counts the cycles completed when the chip samples its lines.
 * For every instruction but a taken branch that is the last cycle. */
#define NES_POLL(c) ((c)->poll_at = (c)->cycles, (c)->poll_i = (c)->flag_i)

/* BRK, JAM and the interrupt and reset sequences do not poll. */
#define NES_POLL_NONE(c) ((c)->poll_at = 0, (c)->poll_i = 1)

/* ------------------------------------------------------------------ */
/* Bus helpers                                                         */
/* ------------------------------------------------------------------ */

NES_OP uint8_t nes_fetch(NesCpu *c)
{
    uint8_t v = nes_bus_read(c, c->pc);
    c->pc = (uint16_t)(c->pc + 1u);
    return v;
}

NES_OP uint8_t nes_rd(NesCpu *c, uint16_t addr) { return nes_bus_read(c, addr); }
NES_OP void nes_wr(NesCpu *c, uint16_t addr, uint8_t v) { nes_bus_write(c, addr, v); }

/* A cycle with nothing to transfer: the chip reads anyway and drops the
 * byte. Which address it reads is part of each instruction's behaviour. */
NES_OP void nes_dummy(NesCpu *c, uint16_t addr) { (void)nes_bus_read(c, addr); }

NES_OP void nes_push(NesCpu *c, uint8_t v)
{
    nes_bus_write(c, (uint16_t)(0x0100u | c->s), v);
    c->s = (uint8_t)(c->s - 1u);
}

NES_OP uint8_t nes_pull(NesCpu *c)
{
    c->s = (uint8_t)(c->s + 1u);
    return nes_bus_read(c, (uint16_t)(0x0100u | c->s));
}

NES_OP void nes_nz(NesCpu *c, uint8_t v)
{
    c->flag_z = v == 0;
    c->flag_n = v >> 7;
}

/* ------------------------------------------------------------------ */
/* Effective addresses                                                 */
/* ------------------------------------------------------------------ */

/* Each of these makes the operand fetches and the cycles that only compute
 * the address, and returns the address of the instruction's data. The data
 * access itself is the caller's, after the interrupt poll. */

enum { NES_IX_X, NES_IX_Y };

/* Indexed modes: skip the uncorrected read when the index does not carry
 * (read instructions), or always make it (stores, read-modify-write). */
enum { NES_PAGE_CROSS, NES_PAGE_ALWAYS };

NES_OP uint8_t nes_index(const NesCpu *c, int ix) { return ix == NES_IX_X ? c->x : c->y; }

/* #imm: the data is the next program byte. */
NES_OP uint16_t nes_ea_imm(NesCpu *c)
{
    uint16_t ea = c->pc;
    c->pc = (uint16_t)(c->pc + 1u);
    return ea;
}

NES_OP uint16_t nes_ea_zp(NesCpu *c) { return nes_fetch(c); }

/* zp,X and zp,Y: the adder needs a cycle, spent reading the unindexed
 * address. The sum wraps within page 0. */
NES_OP uint16_t nes_ea_zpi(NesCpu *c, int ix)
{
    uint8_t zp = nes_fetch(c);
    nes_dummy(c, zp);
    return (uint8_t)(zp + nes_index(c, ix));
}

NES_OP uint16_t nes_ea_abs(NesCpu *c)
{
    unsigned ea = nes_fetch(c);
    ea |= (unsigned)nes_fetch(c) << 8;
    return (uint16_t)ea;
}

/* The uncorrected address of an indexed access: the low byte already has
 * the index added, the high byte does not have the carry yet. */
NES_OP uint16_t nes_uncarried(unsigned base, unsigned ea) { return (uint16_t)((base & 0xFF00u) | (ea & 0x00FFu)); }

/* abs,X and abs,Y. */
NES_OP uint16_t nes_ea_absi(NesCpu *c, int ix, int page)
{
    unsigned base = nes_fetch(c);
    base |= (unsigned)nes_fetch(c) << 8;
    unsigned ea = (base + nes_index(c, ix)) & 0xFFFFu;
    if (page == NES_PAGE_ALWAYS || ((base ^ ea) & 0xFF00u)) nes_dummy(c, nes_uncarried(base, ea));
    return (uint16_t)ea;
}

/* (zp,X): the pointer is at zp + X, wrapped within page 0. */
NES_OP uint16_t nes_ea_indx(NesCpu *c)
{
    uint8_t zp = nes_fetch(c);
    nes_dummy(c, zp);
    zp = (uint8_t)(zp + c->x);
    unsigned ea = nes_rd(c, zp);
    ea |= (unsigned)nes_rd(c, (uint8_t)(zp + 1u)) << 8;
    return (uint16_t)ea;
}

/* (zp),Y. */
NES_OP uint16_t nes_ea_indy(NesCpu *c, int page)
{
    uint8_t zp = nes_fetch(c);
    unsigned base = nes_rd(c, zp);
    base |= (unsigned)nes_rd(c, (uint8_t)(zp + 1u)) << 8;
    unsigned ea = (base + c->y) & 0xFFFFu;
    if (page == NES_PAGE_ALWAYS || ((base ^ ea) & 0xFF00u)) nes_dummy(c, nes_uncarried(base, ea));
    return (uint16_t)ea;
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */
/* ------------------------------------------------------------------ */

/* Operations on a byte that was read: the official ones, then the
 * unofficial ones that have this shape. */
enum {
    NES_ALU_ORA, NES_ALU_AND, NES_ALU_EOR, NES_ALU_ADC, NES_ALU_SBC, NES_ALU_CMP, NES_ALU_CPX, NES_ALU_CPY,
    NES_ALU_BIT, NES_ALU_LDA, NES_ALU_LDX, NES_ALU_LDY,
    NES_ALU_NOP, /* the operand is read and dropped */
    NES_ALU_LAX, NES_ALU_ANC, NES_ALU_ALR, NES_ALU_ARR, NES_ALU_SBX, NES_ALU_LAS, NES_ALU_ANE, NES_ALU_LXA,
};

/* Read-modify-write operations; the second row also feeds the result to the
 * accumulator (SLO = ASL then ORA, and so on). */
enum {
    NES_RMW_ASL, NES_RMW_ROL, NES_RMW_LSR, NES_RMW_ROR, NES_RMW_INC, NES_RMW_DEC,
    NES_RMW_SLO, NES_RMW_RLA, NES_RMW_SRE, NES_RMW_RRA, NES_RMW_ISC, NES_RMW_DCP,
};

/* What a store puts on the bus. */
enum { NES_REG_A, NES_REG_X, NES_REG_Y, NES_REG_AX /* SAX: A AND X */ };

/* The constant ANE and LXA OR into A before their ANDs. On the chip it is
 * whatever the floating internal bus settles to, and it is not the same
 * for the two.
 *
 * ANE ($8B): $EE, the common value and the one the vectors were recorded
 * with. No hardware-verified test ROM pins it (blargg lists it as
 * unreliable and leaves it out).
 *
 * LXA ($AB): $FF, which makes the instruction a plain "A = X = operand".
 * That is what blargg's NES did: the checksums of "ATX #n" in
 * instr_test-v5/03-immediate, nes_instr_test/02-immediate and
 * blargg_nes_cpu_test5 only come out with $FF (not with $EE, the vectors'
 * value, nor with $00). AccuracyCoin accepts either. The vectors of
 * opcode $AB that depend on the constant are counted as deviations by
 * tests/nes/sst_6502.c and checked against this rule. */
#define NES_ANE_MAGIC 0xEEu
#define NES_LXA_MAGIC 0xFFu

/* The 2A03 has no decimal mode: the D flag is not connected to the adder. */
NES_OP void nes_adc(NesCpu *c, unsigned data)
{
    const unsigned a = c->a;
    const unsigned sum = a + data + c->flag_c;
    c->flag_v = (uint8_t)((~(a ^ data) & (a ^ sum) & 0x80u) >> 7);
    c->flag_c = (uint8_t)(sum >> 8);
    c->a = (uint8_t)sum;
    nes_nz(c, c->a);
}

NES_OP void nes_compare(NesCpu *c, unsigned reg, unsigned data)
{
    c->flag_c = reg >= data;
    nes_nz(c, (uint8_t)(reg - data));
}

NES_OP void nes_alu(NesCpu *c, int alu, uint8_t data)
{
    switch (alu) {
    case NES_ALU_ORA:
        c->a |= data;
        nes_nz(c, c->a);
        break;
    case NES_ALU_AND:
        c->a &= data;
        nes_nz(c, c->a);
        break;
    case NES_ALU_EOR:
        c->a ^= data;
        nes_nz(c, c->a);
        break;
    case NES_ALU_ADC: nes_adc(c, data); break;
    case NES_ALU_SBC: nes_adc(c, data ^ 0xFFu); break;
    case NES_ALU_CMP: nes_compare(c, c->a, data); break;
    case NES_ALU_CPX: nes_compare(c, c->x, data); break;
    case NES_ALU_CPY: nes_compare(c, c->y, data); break;
    case NES_ALU_BIT:
        c->flag_z = (c->a & data) == 0;
        c->flag_v = (data >> 6) & 1u;
        c->flag_n = data >> 7;
        break;
    case NES_ALU_LDA:
        c->a = data;
        nes_nz(c, data);
        break;
    case NES_ALU_LDX:
        c->x = data;
        nes_nz(c, data);
        break;
    case NES_ALU_LDY:
        c->y = data;
        nes_nz(c, data);
        break;
    case NES_ALU_NOP: break;
    case NES_ALU_LAX:
        c->a = c->x = data;
        nes_nz(c, data);
        break;
    case NES_ALU_ANC: /* AND, with C copied from N as if the result were shifted out */
        c->a &= data;
        nes_nz(c, c->a);
        c->flag_c = c->flag_n;
        break;
    case NES_ALU_ALR: /* AND, then LSR A */
        c->a &= data;
        c->flag_c = c->a & 1u;
        c->a >>= 1;
        nes_nz(c, c->a);
        break;
    case NES_ALU_ARR: /* AND, then ROR A, but C and V come from the adder: bit 6, and bit 6 XOR bit 5 */
        c->a = (uint8_t)(((c->a & data) >> 1) | (c->flag_c << 7));
        nes_nz(c, c->a);
        c->flag_c = (c->a >> 6) & 1u;
        c->flag_v = ((c->a >> 6) ^ (c->a >> 5)) & 1u;
        break;
    case NES_ALU_SBX: { /* X = (A AND X) - data, flags as CMP */
        const unsigned ax = c->a & c->x;
        nes_compare(c, ax, data);
        c->x = (uint8_t)(ax - data);
        break;
    }
    case NES_ALU_LAS: /* A = X = S = data AND S */
        c->a = c->x = c->s = c->s & data;
        nes_nz(c, c->a);
        break;
    case NES_ALU_ANE: /* unstable, see below */
        c->a = (uint8_t)((c->a | NES_ANE_MAGIC) & c->x & data);
        nes_nz(c, c->a);
        break;
    default: /* LXA, unstable */
        c->a = c->x = (uint8_t)((c->a | NES_LXA_MAGIC) & data);
        nes_nz(c, c->a);
        break;
    }
}

/* Read-modify-write: returns the new value. */
NES_OP uint8_t nes_rmw(NesCpu *c, int op, uint8_t data)
{
    uint8_t r;
    switch (op) {
    case NES_RMW_ASL:
    case NES_RMW_SLO:
        c->flag_c = data >> 7;
        r = (uint8_t)(data << 1);
        break;
    case NES_RMW_ROL:
    case NES_RMW_RLA:
        r = (uint8_t)((data << 1) | c->flag_c);
        c->flag_c = data >> 7;
        break;
    case NES_RMW_LSR:
    case NES_RMW_SRE:
        c->flag_c = data & 1u;
        r = data >> 1;
        break;
    case NES_RMW_ROR:
    case NES_RMW_RRA:
        r = (uint8_t)((data >> 1) | (c->flag_c << 7));
        c->flag_c = data & 1u;
        break;
    case NES_RMW_INC:
    case NES_RMW_ISC: r = (uint8_t)(data + 1u); break;
    default: r = (uint8_t)(data - 1u); break; /* DEC, DCP */
    }
    switch (op) {
    case NES_RMW_SLO: nes_alu(c, NES_ALU_ORA, r); break;
    case NES_RMW_RLA: nes_alu(c, NES_ALU_AND, r); break;
    case NES_RMW_SRE: nes_alu(c, NES_ALU_EOR, r); break;
    case NES_RMW_RRA: nes_alu(c, NES_ALU_ADC, r); break; /* with the carry ROR just produced */
    case NES_RMW_ISC: nes_alu(c, NES_ALU_SBC, r); break;
    case NES_RMW_DCP: nes_alu(c, NES_ALU_CMP, r); break;
    default: nes_nz(c, r); break;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Instruction shapes                                                  */
/* ------------------------------------------------------------------ */

/* Read: the data access is the last cycle. */
NES_OP void nes_r(NesCpu *c, int alu, uint16_t ea)
{
    NES_POLL(c);
    nes_alu(c, alu, nes_rd(c, ea));
}

/* Store. The register is read when it is driven, not before the address
 * cycles. */
NES_OP void nes_w(NesCpu *c, int reg, uint16_t ea)
{
    NES_POLL(c);
    nes_wr(c, ea, reg == NES_REG_A ? c->a : reg == NES_REG_X ? c->x : reg == NES_REG_Y ? c->y : (uint8_t)(c->a & c->x));
}

/* Read-modify-write on memory: while the ALU works, the chip writes back
 * the byte it just read. */
NES_OP void nes_m(NesCpu *c, int op, uint16_t ea)
{
    uint8_t data = nes_rd(c, ea);
    nes_wr(c, ea, data);
    data = nes_rmw(c, op, data);
    NES_POLL(c);
    nes_wr(c, ea, data);
}

/* One-byte instructions that only touch registers: the second cycle reads
 * the next opcode and drops it. The caller applies the effect afterwards,
 * so CLI and SEI are polled with the old I. */
NES_OP void nes_implied(NesCpu *c)
{
    NES_POLL(c);
    nes_dummy(c, c->pc);
}

/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

enum { NES_FLAG_C, NES_FLAG_Z, NES_FLAG_V, NES_FLAG_N };

/* Branch when `flag` equals `want`. Not taken: 2 cycles. Taken: a third
 * cycle reads the next opcode and drops it while the adder forms the new
 * PCL. A carry or borrow into PCH costs a fourth, reading from the new PCL
 * in the old page.
 *
 * Interrupt polls (see nes_cpu.h): always before the operand fetch; not
 * again before the third cycle; again before the fourth. */
NES_OP void nes_branch(NesCpu *c, int flag, unsigned want)
{
    NES_POLL(c);
    const uint8_t offset = nes_fetch(c);
    const unsigned value =
        flag == NES_FLAG_C ? c->flag_c : flag == NES_FLAG_Z ? c->flag_z : flag == NES_FLAG_V ? c->flag_v : c->flag_n;
    if (value != want) return;
    const uint16_t target = (uint16_t)(c->pc + (int8_t)offset);
    nes_dummy(c, c->pc);
    if ((target ^ c->pc) & 0xFF00u) {
        NES_POLL(c);
        nes_dummy(c, (uint16_t)((c->pc & 0xFF00u) | (target & 0x00FFu)));
    }
    c->pc = target;
    c->branched = 1;
}

NES_OP void nes_jmp_abs(NesCpu *c)
{
    unsigned target = nes_fetch(c);
    NES_POLL(c);
    target |= (unsigned)nes_fetch(c) << 8;
    c->pc = (uint16_t)target;
    c->branched = 1;
}

/* JMP (a): the pointer's high byte comes from the same page as its low
 * byte, so JMP ($12FF) reads $12FF and $1200. */
NES_OP void nes_jmp_ind(NesCpu *c)
{
    unsigned ptr = nes_fetch(c);
    ptr |= (unsigned)nes_fetch(c) << 8;
    unsigned target = nes_rd(c, (uint16_t)ptr);
    NES_POLL(c);
    target |= (unsigned)nes_rd(c, (uint16_t)((ptr & 0xFF00u) | ((ptr + 1u) & 0x00FFu))) << 8;
    c->pc = (uint16_t)target;
    c->branched = 1;
}

/* JSR: pushes the address of its own last byte, and fetches that byte only
 * after the pushes (the low byte waits in the stack pointer's adder, which
 * is why the third cycle reads the stack). */
NES_OP void nes_jsr(NesCpu *c)
{
    unsigned target = nes_fetch(c);
    nes_dummy(c, (uint16_t)(0x0100u | c->s));
    nes_push(c, (uint8_t)(c->pc >> 8));
    nes_push(c, (uint8_t)c->pc);
    NES_POLL(c);
    target |= (unsigned)nes_fetch(c) << 8;
    c->pc = (uint16_t)target;
    c->branched = 1;
}

/* RTS: the pulled address is JSR's last byte; a final cycle reads it and
 * steps past it. */
NES_OP void nes_rts(NesCpu *c)
{
    nes_dummy(c, c->pc);
    nes_dummy(c, (uint16_t)(0x0100u | c->s));
    unsigned target = nes_pull(c);
    target |= (unsigned)nes_pull(c) << 8;
    c->pc = (uint16_t)target;
    NES_POLL(c);
    nes_dummy(c, c->pc);
    c->pc = (uint16_t)(c->pc + 1u);
    c->branched = 1;
}

/* RTI: P is back before the poll, so the restored I decides. */
NES_OP void nes_rti(NesCpu *c)
{
    nes_dummy(c, c->pc);
    nes_dummy(c, (uint16_t)(0x0100u | c->s));
    nes_cpu_set_p(c, nes_pull(c));
    unsigned target = nes_pull(c);
    NES_POLL(c);
    target |= (unsigned)nes_pull(c) << 8;
    c->pc = (uint16_t)target;
    c->branched = 1;
}

/* BRK: a two-byte instruction (the byte after the opcode is read and
 * skipped) that enters the interrupt sequence with B set in the pushed P.
 * D is left alone on this chip. The vector is chosen once PC is pushed, so
 * an NMI that arrived during the first four cycles takes it (nes_bus.h);
 * P is pushed with B set all the same. No interrupt poll: the handler's
 * first instruction always runs. */
NES_OP void nes_brk(NesCpu *c)
{
    (void)nes_fetch(c);
    nes_push(c, (uint8_t)(c->pc >> 8));
    nes_push(c, (uint8_t)c->pc);
    const uint16_t vector = nes_bus_interrupt_vector(c, NES_VEC_IRQ);
    nes_push(c, (uint8_t)(nes_cpu_get_p(c) | NES_P_B));
    c->flag_i = 1;
    unsigned target = nes_rd(c, vector);
    target |= (unsigned)nes_rd(c, (uint16_t)(vector + 1u)) << 8;
    c->pc = (uint16_t)target;
    c->branched = 1;
    NES_POLL_NONE(c);
}

/* ------------------------------------------------------------------ */
/* Stack                                                               */
/* ------------------------------------------------------------------ */

/* PHA, PHP. */
NES_OP void nes_push_op(NesCpu *c, bool status)
{
    nes_dummy(c, c->pc);
    NES_POLL(c);
    nes_push(c, status ? (uint8_t)(nes_cpu_get_p(c) | NES_P_B) : c->a);
}

/* PLA. The third cycle reads the stack before S is incremented. */
NES_OP void nes_pla(NesCpu *c)
{
    nes_dummy(c, c->pc);
    nes_dummy(c, (uint16_t)(0x0100u | c->s));
    NES_POLL(c);
    c->a = nes_pull(c);
    nes_nz(c, c->a);
}

/* PLP: polled before the pull, so with the old I. */
NES_OP void nes_plp(NesCpu *c)
{
    nes_dummy(c, c->pc);
    nes_dummy(c, (uint16_t)(0x0100u | c->s));
    NES_POLL(c);
    nes_cpu_set_p(c, nes_pull(c));
}

/* ------------------------------------------------------------------ */
/* Unstable opcodes                                                    */
/* ------------------------------------------------------------------ */

/* ANE ($8B) and LXA ($AB) are in nes_alu: A is ORed with a "magic"
 * constant first.
 *
 * SHA ($93, $9F), SHX ($9E), SHY ($9C) and TAS ($9B) store a register ANDed
 * with the high byte of the operand's base address plus one: the chip
 * drives the register and the address adder's output onto the same lines.
 * When the index carries into the high byte, the corrected high byte never
 * reaches the address bus: the write goes to the page given by that same
 * AND. TAS also loads S with A AND X.
 *
 * The AND with the high byte drops out of the stored value when the CPU is
 * stalled (RDY low: a DMC or sprite DMA) in the cycle before the write:
 * SHY then stores Y like STY. The bus reports a halted read in c->stalled
 * (nes_bus.h). AccuracyCoin tests it for all five opcodes (error codes 7
 * to C of "Unofficial Instructions: SH*"); what the address does when the
 * index also carries is not covered there and is left as without a
 * stall. */
enum { NES_SH_SHA, NES_SH_SHX, NES_SH_SHY, NES_SH_TAS };

NES_OP void nes_sh_store(NesCpu *c, int which, unsigned base, unsigned index)
{
    unsigned ea = (base + index) & 0xFFFFu;
    c->stalled = 0;
    nes_dummy(c, nes_uncarried(base, ea));
    if (which == NES_SH_TAS) c->s = c->a & c->x;
    const unsigned reg = which == NES_SH_SHX ? c->x : which == NES_SH_SHY ? c->y : (unsigned)(c->a & c->x);
    const uint8_t anded = (uint8_t)(reg & ((base >> 8) + 1u));
    if ((base ^ ea) & 0xFF00u) ea = ((unsigned)anded << 8) | (ea & 0x00FFu);
    NES_POLL(c);
    nes_wr(c, (uint16_t)ea, c->stalled ? (uint8_t)reg : anded);
}

NES_OP void nes_sh_absi(NesCpu *c, int which, int ix)
{
    unsigned base = nes_fetch(c);
    base |= (unsigned)nes_fetch(c) << 8;
    nes_sh_store(c, which, base, nes_index(c, ix));
}

/* SHA (zp),Y. */
NES_OP void nes_sh_indy(NesCpu *c)
{
    uint8_t zp = nes_fetch(c);
    unsigned base = nes_rd(c, zp);
    base |= (unsigned)nes_rd(c, (uint8_t)(zp + 1u)) << 8;
    nes_sh_store(c, NES_SH_SHA, base, c->y);
}

/* JAM (twelve opcodes): the instruction decoder locks up. The chip reads
 * the byte after the opcode, then $FFFF, $FFFE, $FFFE, and from then on
 * holds $FFFF on the address bus until it is reset; interrupts no longer
 * get in. The cycles made here are the ones the vectors record; after
 * that c->jammed makes nes_cpu_step read $FFFF once per call. PC stays
 * after the opcode. */
NES_OP void nes_jam(NesCpu *c)
{
    nes_dummy(c, c->pc);
    nes_dummy(c, 0xFFFFu);
    nes_dummy(c, 0xFFFEu);
    nes_dummy(c, 0xFFFEu);
    for (int i = 0; i < 6; i++) nes_dummy(c, 0xFFFFu);
    c->jammed = 1;
    c->branched = 1;
    NES_POLL_NONE(c);
}

/* ------------------------------------------------------------------ */
/* Opcode table                                                        */
/* ------------------------------------------------------------------ */

/* X(opcode, body). `c` is in scope in the body. nes_cpu_decode.h has the
 * matching classification; the vector harness checks the two against each
 * other (lengths, control flow, which opcodes write). */
#define NES_OPCODES(X)                                                                                                 \
    X(00, nes_brk(c))                                                                                                  \
    X(01, nes_r(c, NES_ALU_ORA, nes_ea_indx(c)))                                                                       \
    X(02, nes_jam(c))                                                                                                  \
    X(03, nes_m(c, NES_RMW_SLO, nes_ea_indx(c)))                                                                       \
    X(04, nes_r(c, NES_ALU_NOP, nes_ea_zp(c)))                                                                         \
    X(05, nes_r(c, NES_ALU_ORA, nes_ea_zp(c)))                                                                         \
    X(06, nes_m(c, NES_RMW_ASL, nes_ea_zp(c)))                                                                         \
    X(07, nes_m(c, NES_RMW_SLO, nes_ea_zp(c)))                                                                         \
    X(08, nes_push_op(c, true))                                                                                        \
    X(09, nes_r(c, NES_ALU_ORA, nes_ea_imm(c)))                                                                        \
    X(0A, (nes_implied(c), c->a = nes_rmw(c, NES_RMW_ASL, c->a)))                                                      \
    X(0B, nes_r(c, NES_ALU_ANC, nes_ea_imm(c)))                                                                        \
    X(0C, nes_r(c, NES_ALU_NOP, nes_ea_abs(c)))                                                                        \
    X(0D, nes_r(c, NES_ALU_ORA, nes_ea_abs(c)))                                                                        \
    X(0E, nes_m(c, NES_RMW_ASL, nes_ea_abs(c)))                                                                        \
    X(0F, nes_m(c, NES_RMW_SLO, nes_ea_abs(c)))                                                                        \
    X(10, nes_branch(c, NES_FLAG_N, 0))                                                                                \
    X(11, nes_r(c, NES_ALU_ORA, nes_ea_indy(c, NES_PAGE_CROSS)))                                                       \
    X(12, nes_jam(c))                                                                                                  \
    X(13, nes_m(c, NES_RMW_SLO, nes_ea_indy(c, NES_PAGE_ALWAYS)))                                                      \
    X(14, nes_r(c, NES_ALU_NOP, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(15, nes_r(c, NES_ALU_ORA, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(16, nes_m(c, NES_RMW_ASL, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(17, nes_m(c, NES_RMW_SLO, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(18, (nes_implied(c), c->flag_c = 0))                                                                             \
    X(19, nes_r(c, NES_ALU_ORA, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(1A, nes_implied(c))                                                                                              \
    X(1B, nes_m(c, NES_RMW_SLO, nes_ea_absi(c, NES_IX_Y, NES_PAGE_ALWAYS)))                                            \
    X(1C, nes_r(c, NES_ALU_NOP, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(1D, nes_r(c, NES_ALU_ORA, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(1E, nes_m(c, NES_RMW_ASL, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(1F, nes_m(c, NES_RMW_SLO, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(20, nes_jsr(c))                                                                                                  \
    X(21, nes_r(c, NES_ALU_AND, nes_ea_indx(c)))                                                                       \
    X(22, nes_jam(c))                                                                                                  \
    X(23, nes_m(c, NES_RMW_RLA, nes_ea_indx(c)))                                                                       \
    X(24, nes_r(c, NES_ALU_BIT, nes_ea_zp(c)))                                                                         \
    X(25, nes_r(c, NES_ALU_AND, nes_ea_zp(c)))                                                                         \
    X(26, nes_m(c, NES_RMW_ROL, nes_ea_zp(c)))                                                                         \
    X(27, nes_m(c, NES_RMW_RLA, nes_ea_zp(c)))                                                                         \
    X(28, nes_plp(c))                                                                                                  \
    X(29, nes_r(c, NES_ALU_AND, nes_ea_imm(c)))                                                                        \
    X(2A, (nes_implied(c), c->a = nes_rmw(c, NES_RMW_ROL, c->a)))                                                      \
    X(2B, nes_r(c, NES_ALU_ANC, nes_ea_imm(c)))                                                                        \
    X(2C, nes_r(c, NES_ALU_BIT, nes_ea_abs(c)))                                                                        \
    X(2D, nes_r(c, NES_ALU_AND, nes_ea_abs(c)))                                                                        \
    X(2E, nes_m(c, NES_RMW_ROL, nes_ea_abs(c)))                                                                        \
    X(2F, nes_m(c, NES_RMW_RLA, nes_ea_abs(c)))                                                                        \
    X(30, nes_branch(c, NES_FLAG_N, 1))                                                                                \
    X(31, nes_r(c, NES_ALU_AND, nes_ea_indy(c, NES_PAGE_CROSS)))                                                       \
    X(32, nes_jam(c))                                                                                                  \
    X(33, nes_m(c, NES_RMW_RLA, nes_ea_indy(c, NES_PAGE_ALWAYS)))                                                      \
    X(34, nes_r(c, NES_ALU_NOP, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(35, nes_r(c, NES_ALU_AND, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(36, nes_m(c, NES_RMW_ROL, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(37, nes_m(c, NES_RMW_RLA, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(38, (nes_implied(c), c->flag_c = 1))                                                                             \
    X(39, nes_r(c, NES_ALU_AND, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(3A, nes_implied(c))                                                                                              \
    X(3B, nes_m(c, NES_RMW_RLA, nes_ea_absi(c, NES_IX_Y, NES_PAGE_ALWAYS)))                                            \
    X(3C, nes_r(c, NES_ALU_NOP, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(3D, nes_r(c, NES_ALU_AND, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(3E, nes_m(c, NES_RMW_ROL, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(3F, nes_m(c, NES_RMW_RLA, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(40, nes_rti(c))                                                                                                  \
    X(41, nes_r(c, NES_ALU_EOR, nes_ea_indx(c)))                                                                       \
    X(42, nes_jam(c))                                                                                                  \
    X(43, nes_m(c, NES_RMW_SRE, nes_ea_indx(c)))                                                                       \
    X(44, nes_r(c, NES_ALU_NOP, nes_ea_zp(c)))                                                                         \
    X(45, nes_r(c, NES_ALU_EOR, nes_ea_zp(c)))                                                                         \
    X(46, nes_m(c, NES_RMW_LSR, nes_ea_zp(c)))                                                                         \
    X(47, nes_m(c, NES_RMW_SRE, nes_ea_zp(c)))                                                                         \
    X(48, nes_push_op(c, false))                                                                                       \
    X(49, nes_r(c, NES_ALU_EOR, nes_ea_imm(c)))                                                                        \
    X(4A, (nes_implied(c), c->a = nes_rmw(c, NES_RMW_LSR, c->a)))                                                      \
    X(4B, nes_r(c, NES_ALU_ALR, nes_ea_imm(c)))                                                                        \
    X(4C, nes_jmp_abs(c))                                                                                              \
    X(4D, nes_r(c, NES_ALU_EOR, nes_ea_abs(c)))                                                                        \
    X(4E, nes_m(c, NES_RMW_LSR, nes_ea_abs(c)))                                                                        \
    X(4F, nes_m(c, NES_RMW_SRE, nes_ea_abs(c)))                                                                        \
    X(50, nes_branch(c, NES_FLAG_V, 0))                                                                                \
    X(51, nes_r(c, NES_ALU_EOR, nes_ea_indy(c, NES_PAGE_CROSS)))                                                       \
    X(52, nes_jam(c))                                                                                                  \
    X(53, nes_m(c, NES_RMW_SRE, nes_ea_indy(c, NES_PAGE_ALWAYS)))                                                      \
    X(54, nes_r(c, NES_ALU_NOP, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(55, nes_r(c, NES_ALU_EOR, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(56, nes_m(c, NES_RMW_LSR, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(57, nes_m(c, NES_RMW_SRE, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(58, (nes_implied(c), c->flag_i = 0))                                                                             \
    X(59, nes_r(c, NES_ALU_EOR, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(5A, nes_implied(c))                                                                                              \
    X(5B, nes_m(c, NES_RMW_SRE, nes_ea_absi(c, NES_IX_Y, NES_PAGE_ALWAYS)))                                            \
    X(5C, nes_r(c, NES_ALU_NOP, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(5D, nes_r(c, NES_ALU_EOR, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(5E, nes_m(c, NES_RMW_LSR, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(5F, nes_m(c, NES_RMW_SRE, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(60, nes_rts(c))                                                                                                  \
    X(61, nes_r(c, NES_ALU_ADC, nes_ea_indx(c)))                                                                       \
    X(62, nes_jam(c))                                                                                                  \
    X(63, nes_m(c, NES_RMW_RRA, nes_ea_indx(c)))                                                                       \
    X(64, nes_r(c, NES_ALU_NOP, nes_ea_zp(c)))                                                                         \
    X(65, nes_r(c, NES_ALU_ADC, nes_ea_zp(c)))                                                                         \
    X(66, nes_m(c, NES_RMW_ROR, nes_ea_zp(c)))                                                                         \
    X(67, nes_m(c, NES_RMW_RRA, nes_ea_zp(c)))                                                                         \
    X(68, nes_pla(c))                                                                                                  \
    X(69, nes_r(c, NES_ALU_ADC, nes_ea_imm(c)))                                                                        \
    X(6A, (nes_implied(c), c->a = nes_rmw(c, NES_RMW_ROR, c->a)))                                                      \
    X(6B, nes_r(c, NES_ALU_ARR, nes_ea_imm(c)))                                                                        \
    X(6C, nes_jmp_ind(c))                                                                                              \
    X(6D, nes_r(c, NES_ALU_ADC, nes_ea_abs(c)))                                                                        \
    X(6E, nes_m(c, NES_RMW_ROR, nes_ea_abs(c)))                                                                        \
    X(6F, nes_m(c, NES_RMW_RRA, nes_ea_abs(c)))                                                                        \
    X(70, nes_branch(c, NES_FLAG_V, 1))                                                                                \
    X(71, nes_r(c, NES_ALU_ADC, nes_ea_indy(c, NES_PAGE_CROSS)))                                                       \
    X(72, nes_jam(c))                                                                                                  \
    X(73, nes_m(c, NES_RMW_RRA, nes_ea_indy(c, NES_PAGE_ALWAYS)))                                                      \
    X(74, nes_r(c, NES_ALU_NOP, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(75, nes_r(c, NES_ALU_ADC, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(76, nes_m(c, NES_RMW_ROR, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(77, nes_m(c, NES_RMW_RRA, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(78, (nes_implied(c), c->flag_i = 1))                                                                             \
    X(79, nes_r(c, NES_ALU_ADC, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(7A, nes_implied(c))                                                                                              \
    X(7B, nes_m(c, NES_RMW_RRA, nes_ea_absi(c, NES_IX_Y, NES_PAGE_ALWAYS)))                                            \
    X(7C, nes_r(c, NES_ALU_NOP, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(7D, nes_r(c, NES_ALU_ADC, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(7E, nes_m(c, NES_RMW_ROR, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(7F, nes_m(c, NES_RMW_RRA, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(80, nes_r(c, NES_ALU_NOP, nes_ea_imm(c)))                                                                        \
    X(81, nes_w(c, NES_REG_A, nes_ea_indx(c)))                                                                         \
    X(82, nes_r(c, NES_ALU_NOP, nes_ea_imm(c)))                                                                        \
    X(83, nes_w(c, NES_REG_AX, nes_ea_indx(c)))                                                                        \
    X(84, nes_w(c, NES_REG_Y, nes_ea_zp(c)))                                                                           \
    X(85, nes_w(c, NES_REG_A, nes_ea_zp(c)))                                                                           \
    X(86, nes_w(c, NES_REG_X, nes_ea_zp(c)))                                                                           \
    X(87, nes_w(c, NES_REG_AX, nes_ea_zp(c)))                                                                          \
    X(88, (nes_implied(c), c->y = nes_rmw(c, NES_RMW_DEC, c->y)))                                                      \
    X(89, nes_r(c, NES_ALU_NOP, nes_ea_imm(c)))                                                                        \
    X(8A, (nes_implied(c), c->a = c->x, nes_nz(c, c->a)))                                                              \
    X(8B, nes_r(c, NES_ALU_ANE, nes_ea_imm(c)))                                                                        \
    X(8C, nes_w(c, NES_REG_Y, nes_ea_abs(c)))                                                                          \
    X(8D, nes_w(c, NES_REG_A, nes_ea_abs(c)))                                                                          \
    X(8E, nes_w(c, NES_REG_X, nes_ea_abs(c)))                                                                          \
    X(8F, nes_w(c, NES_REG_AX, nes_ea_abs(c)))                                                                         \
    X(90, nes_branch(c, NES_FLAG_C, 0))                                                                                \
    X(91, nes_w(c, NES_REG_A, nes_ea_indy(c, NES_PAGE_ALWAYS)))                                                        \
    X(92, nes_jam(c))                                                                                                  \
    X(93, nes_sh_indy(c))                                                                                              \
    X(94, nes_w(c, NES_REG_Y, nes_ea_zpi(c, NES_IX_X)))                                                                \
    X(95, nes_w(c, NES_REG_A, nes_ea_zpi(c, NES_IX_X)))                                                                \
    X(96, nes_w(c, NES_REG_X, nes_ea_zpi(c, NES_IX_Y)))                                                                \
    X(97, nes_w(c, NES_REG_AX, nes_ea_zpi(c, NES_IX_Y)))                                                               \
    X(98, (nes_implied(c), c->a = c->y, nes_nz(c, c->a)))                                                              \
    X(99, nes_w(c, NES_REG_A, nes_ea_absi(c, NES_IX_Y, NES_PAGE_ALWAYS)))                                              \
    X(9A, (nes_implied(c), c->s = c->x))                                                                               \
    X(9B, nes_sh_absi(c, NES_SH_TAS, NES_IX_Y))                                                                        \
    X(9C, nes_sh_absi(c, NES_SH_SHY, NES_IX_X))                                                                        \
    X(9D, nes_w(c, NES_REG_A, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                              \
    X(9E, nes_sh_absi(c, NES_SH_SHX, NES_IX_Y))                                                                        \
    X(9F, nes_sh_absi(c, NES_SH_SHA, NES_IX_Y))                                                                        \
    X(A0, nes_r(c, NES_ALU_LDY, nes_ea_imm(c)))                                                                        \
    X(A1, nes_r(c, NES_ALU_LDA, nes_ea_indx(c)))                                                                       \
    X(A2, nes_r(c, NES_ALU_LDX, nes_ea_imm(c)))                                                                        \
    X(A3, nes_r(c, NES_ALU_LAX, nes_ea_indx(c)))                                                                       \
    X(A4, nes_r(c, NES_ALU_LDY, nes_ea_zp(c)))                                                                         \
    X(A5, nes_r(c, NES_ALU_LDA, nes_ea_zp(c)))                                                                         \
    X(A6, nes_r(c, NES_ALU_LDX, nes_ea_zp(c)))                                                                         \
    X(A7, nes_r(c, NES_ALU_LAX, nes_ea_zp(c)))                                                                         \
    X(A8, (nes_implied(c), c->y = c->a, nes_nz(c, c->y)))                                                              \
    X(A9, nes_r(c, NES_ALU_LDA, nes_ea_imm(c)))                                                                        \
    X(AA, (nes_implied(c), c->x = c->a, nes_nz(c, c->x)))                                                              \
    X(AB, nes_r(c, NES_ALU_LXA, nes_ea_imm(c)))                                                                        \
    X(AC, nes_r(c, NES_ALU_LDY, nes_ea_abs(c)))                                                                        \
    X(AD, nes_r(c, NES_ALU_LDA, nes_ea_abs(c)))                                                                        \
    X(AE, nes_r(c, NES_ALU_LDX, nes_ea_abs(c)))                                                                        \
    X(AF, nes_r(c, NES_ALU_LAX, nes_ea_abs(c)))                                                                        \
    X(B0, nes_branch(c, NES_FLAG_C, 1))                                                                                \
    X(B1, nes_r(c, NES_ALU_LDA, nes_ea_indy(c, NES_PAGE_CROSS)))                                                       \
    X(B2, nes_jam(c))                                                                                                  \
    X(B3, nes_r(c, NES_ALU_LAX, nes_ea_indy(c, NES_PAGE_CROSS)))                                                       \
    X(B4, nes_r(c, NES_ALU_LDY, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(B5, nes_r(c, NES_ALU_LDA, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(B6, nes_r(c, NES_ALU_LDX, nes_ea_zpi(c, NES_IX_Y)))                                                              \
    X(B7, nes_r(c, NES_ALU_LAX, nes_ea_zpi(c, NES_IX_Y)))                                                              \
    X(B8, (nes_implied(c), c->flag_v = 0))                                                                             \
    X(B9, nes_r(c, NES_ALU_LDA, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(BA, (nes_implied(c), c->x = c->s, nes_nz(c, c->x)))                                                              \
    X(BB, nes_r(c, NES_ALU_LAS, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(BC, nes_r(c, NES_ALU_LDY, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(BD, nes_r(c, NES_ALU_LDA, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(BE, nes_r(c, NES_ALU_LDX, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(BF, nes_r(c, NES_ALU_LAX, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(C0, nes_r(c, NES_ALU_CPY, nes_ea_imm(c)))                                                                        \
    X(C1, nes_r(c, NES_ALU_CMP, nes_ea_indx(c)))                                                                       \
    X(C2, nes_r(c, NES_ALU_NOP, nes_ea_imm(c)))                                                                        \
    X(C3, nes_m(c, NES_RMW_DCP, nes_ea_indx(c)))                                                                       \
    X(C4, nes_r(c, NES_ALU_CPY, nes_ea_zp(c)))                                                                         \
    X(C5, nes_r(c, NES_ALU_CMP, nes_ea_zp(c)))                                                                         \
    X(C6, nes_m(c, NES_RMW_DEC, nes_ea_zp(c)))                                                                         \
    X(C7, nes_m(c, NES_RMW_DCP, nes_ea_zp(c)))                                                                         \
    X(C8, (nes_implied(c), c->y = nes_rmw(c, NES_RMW_INC, c->y)))                                                      \
    X(C9, nes_r(c, NES_ALU_CMP, nes_ea_imm(c)))                                                                        \
    X(CA, (nes_implied(c), c->x = nes_rmw(c, NES_RMW_DEC, c->x)))                                                      \
    X(CB, nes_r(c, NES_ALU_SBX, nes_ea_imm(c)))                                                                        \
    X(CC, nes_r(c, NES_ALU_CPY, nes_ea_abs(c)))                                                                        \
    X(CD, nes_r(c, NES_ALU_CMP, nes_ea_abs(c)))                                                                        \
    X(CE, nes_m(c, NES_RMW_DEC, nes_ea_abs(c)))                                                                        \
    X(CF, nes_m(c, NES_RMW_DCP, nes_ea_abs(c)))                                                                        \
    X(D0, nes_branch(c, NES_FLAG_Z, 0))                                                                                \
    X(D1, nes_r(c, NES_ALU_CMP, nes_ea_indy(c, NES_PAGE_CROSS)))                                                       \
    X(D2, nes_jam(c))                                                                                                  \
    X(D3, nes_m(c, NES_RMW_DCP, nes_ea_indy(c, NES_PAGE_ALWAYS)))                                                      \
    X(D4, nes_r(c, NES_ALU_NOP, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(D5, nes_r(c, NES_ALU_CMP, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(D6, nes_m(c, NES_RMW_DEC, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(D7, nes_m(c, NES_RMW_DCP, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(D8, (nes_implied(c), c->flag_d = 0))                                                                             \
    X(D9, nes_r(c, NES_ALU_CMP, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(DA, nes_implied(c))                                                                                              \
    X(DB, nes_m(c, NES_RMW_DCP, nes_ea_absi(c, NES_IX_Y, NES_PAGE_ALWAYS)))                                            \
    X(DC, nes_r(c, NES_ALU_NOP, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(DD, nes_r(c, NES_ALU_CMP, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(DE, nes_m(c, NES_RMW_DEC, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(DF, nes_m(c, NES_RMW_DCP, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(E0, nes_r(c, NES_ALU_CPX, nes_ea_imm(c)))                                                                        \
    X(E1, nes_r(c, NES_ALU_SBC, nes_ea_indx(c)))                                                                       \
    X(E2, nes_r(c, NES_ALU_NOP, nes_ea_imm(c)))                                                                        \
    X(E3, nes_m(c, NES_RMW_ISC, nes_ea_indx(c)))                                                                       \
    X(E4, nes_r(c, NES_ALU_CPX, nes_ea_zp(c)))                                                                         \
    X(E5, nes_r(c, NES_ALU_SBC, nes_ea_zp(c)))                                                                         \
    X(E6, nes_m(c, NES_RMW_INC, nes_ea_zp(c)))                                                                         \
    X(E7, nes_m(c, NES_RMW_ISC, nes_ea_zp(c)))                                                                         \
    X(E8, (nes_implied(c), c->x = nes_rmw(c, NES_RMW_INC, c->x)))                                                      \
    X(E9, nes_r(c, NES_ALU_SBC, nes_ea_imm(c)))                                                                        \
    X(EA, nes_implied(c))                                                                                              \
    X(EB, nes_r(c, NES_ALU_SBC, nes_ea_imm(c)))                                                                        \
    X(EC, nes_r(c, NES_ALU_CPX, nes_ea_abs(c)))                                                                        \
    X(ED, nes_r(c, NES_ALU_SBC, nes_ea_abs(c)))                                                                        \
    X(EE, nes_m(c, NES_RMW_INC, nes_ea_abs(c)))                                                                        \
    X(EF, nes_m(c, NES_RMW_ISC, nes_ea_abs(c)))                                                                        \
    X(F0, nes_branch(c, NES_FLAG_Z, 1))                                                                                \
    X(F1, nes_r(c, NES_ALU_SBC, nes_ea_indy(c, NES_PAGE_CROSS)))                                                       \
    X(F2, nes_jam(c))                                                                                                  \
    X(F3, nes_m(c, NES_RMW_ISC, nes_ea_indy(c, NES_PAGE_ALWAYS)))                                                      \
    X(F4, nes_r(c, NES_ALU_NOP, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(F5, nes_r(c, NES_ALU_SBC, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(F6, nes_m(c, NES_RMW_INC, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(F7, nes_m(c, NES_RMW_ISC, nes_ea_zpi(c, NES_IX_X)))                                                              \
    X(F8, (nes_implied(c), c->flag_d = 1))                                                                             \
    X(F9, nes_r(c, NES_ALU_SBC, nes_ea_absi(c, NES_IX_Y, NES_PAGE_CROSS)))                                             \
    X(FA, nes_implied(c))                                                                                              \
    X(FB, nes_m(c, NES_RMW_ISC, nes_ea_absi(c, NES_IX_Y, NES_PAGE_ALWAYS)))                                            \
    X(FC, nes_r(c, NES_ALU_NOP, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(FD, nes_r(c, NES_ALU_SBC, nes_ea_absi(c, NES_IX_X, NES_PAGE_CROSS)))                                             \
    X(FE, nes_m(c, NES_RMW_INC, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))                                            \
    X(FF, nes_m(c, NES_RMW_ISC, nes_ea_absi(c, NES_IX_X, NES_PAGE_ALWAYS)))

/* nes_op_XX(c): one function per opcode, each file calling one out-of-line
 * copy. That is what the interpreter's switch and, by default, recompiled
 * code call: force-inlining a handler after every recompiled instruction
 * is most of the C compiler's work on a generated project and bought the
 * SNES backend no speed (see snes_cpu_impl.h). Projects that want them
 * inlined anyway define NES_RC_INLINE_OPS before including this file. */
#if defined(NES_RC_INLINE_OPS)
#define NES_OP_ENTRY NES_OP
#elif defined(_MSC_VER)
#define NES_OP_ENTRY static __declspec(noinline)
#else
#define NES_OP_ENTRY static __attribute__((noinline, unused))
#endif
#define NES_DEFINE_OP(hex, body)                                                                                       \
    NES_OP_ENTRY void nes_op_##hex(NesCpu *c)                                                                          \
    {                                                                                                                  \
        body;                                                                                                          \
    }
NES_OPCODES(NES_DEFINE_OP)
#undef NES_DEFINE_OP

#ifdef __cplusplus
}
#endif

#endif /* NES_CPU_IMPL_H */
