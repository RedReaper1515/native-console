/*
 * Gekko instruction handlers: the only implementation of integer, branch,
 * memory and system instruction semantics.
 *
 * The interpreter (gc_cpu.c) decodes an instruction and calls gc_op_<name>(c,
 * insn). Recompiled code calls the same handler with the instruction word as
 * a compile-time constant, so every field extraction folds away and what is
 * left is the arithmetic. The emitter only sequences handlers; it never
 * re-implements one. That is how the Game Boy lesson - an emitter and an
 * interpreter drifting apart - is avoided (ADDING-A-CONSOLE.md, P1).
 *
 * Floating point, paired singles and the quantised load/store forms are in
 * gc_fpu_impl.h.
 *
 * Contract:
 *  - c->pc is the address of this instruction and c->npc has already been
 *    set to c->pc + 4. A handler that changes the flow of control writes
 *    c->npc and sets c->branched.
 *  - Every memory access goes through gc_bus_* (the gc_bus_fast_* forms,
 *    which take the long way whenever the bus has not said the short one is
 *    the same). After an access that can
 *    fault, c->pending_vector is checked before any architectural state is
 *    updated, so a faulting instruction leaves no half-written state.
 *  - Register numbers are read from the instruction each time; no guest
 *    value is cached in a C local across a bus access.
 */
#ifndef GC_CPU_IMPL_H
#define GC_CPU_IMPL_H

#include "gc_bus.h"
#include "gc_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The interpreter wants these inlined into its dispatch, where one copy of
 * each handler costs nothing and the call overhead would be most of the
 * work.
 *
 * Recompiled code wants the opposite, and can say so by defining GC_OP and
 * GC_FN before including this. A generated block file has one call site
 * per guest instruction - tens of thousands of them - and forcing an
 * inline at every one expands tens of thousands of copies of the
 * arithmetic. That trades a dispatch loop which stays in the instruction
 * cache for a working set which does not, and it is why the first
 * recompiled build came out slower than the interpreter it replaced.
 * Letting the compiler decide gets the small handlers inlined and the
 * large ones shared. */
#ifndef GC_OP
#if defined(_MSC_VER)
#define GC_OP static __forceinline void
#define GC_FN static __forceinline
#else
#define GC_OP static inline __attribute__((always_inline)) void
#define GC_FN static inline __attribute__((always_inline))
#endif
#endif

/* ------------------------------------------------------------------ */
/* Condition register, XER                                             */

/* CR field 0 occupies bits 31..28 of c->cr: the architecture numbers bits
 * from the most significant end, and so does this. */
GC_FN uint32_t gc_cr_field(const GcCpu *c, unsigned field) {
  return (c->cr >> (28 - 4 * field)) & 0xfu;
}

GC_FN void gc_cr_set_field(GcCpu *c, unsigned field, uint32_t value) {
  const unsigned shift = 28 - 4 * field;
  c->cr = (c->cr & ~(0xfu << shift)) | ((value & 0xfu) << shift);
}

/* CR bit 0 is CR0[LT], bit 31 is CR7[SO]. */
GC_FN uint32_t gc_cr_bit(const GcCpu *c, unsigned bit) {
  return (c->cr >> (31 - bit)) & 1u;
}

GC_FN void gc_cr_set_bit(GcCpu *c, unsigned bit, uint32_t value) {
  const uint32_t mask = 0x80000000u >> bit;
  c->cr = value ? (c->cr | mask) : (c->cr & ~mask);
}

/* LT, GT, EQ and the sticky overflow copied out of XER. */
GC_FN uint32_t gc_compare_bits(const GcCpu *c, int less, int greater) {
  uint32_t bits = less ? 8u : (greater ? 4u : 2u);
  if (c->xer & GC_XER_SO) bits |= 1u;
  return bits;
}

GC_FN void gc_set_cr0(GcCpu *c, uint32_t result) {
  gc_cr_set_field(c, 0, gc_compare_bits(c, (int32_t)result < 0, (int32_t)result > 0));
}

GC_FN void gc_set_ca(GcCpu *c, int carry) {
  c->xer = carry ? (c->xer | GC_XER_CA) : (c->xer & ~GC_XER_CA);
}

/* OV is written every time an OE-form instruction runs; SO is sticky and is
 * only ever set here, never cleared. */
GC_FN void gc_set_ov(GcCpu *c, int overflow) {
  if (overflow) {
    c->xer |= GC_XER_OV | GC_XER_SO;
  } else {
    c->xer &= ~GC_XER_OV;
  }
}

/* a + b + carry_in, with the carry and overflow both derived from the same
 * sum so that the OE and Rc forms cannot disagree. */
GC_FN uint32_t gc_add(GcCpu *c, uint32_t a, uint32_t b, uint32_t carry_in,
                      int set_ca, uint32_t insn) {
  const uint64_t wide = (uint64_t)a + (uint64_t)b + (uint64_t)carry_in;
  const uint32_t result = (uint32_t)wide;
  if (set_ca) gc_set_ca(c, (wide >> 32) != 0);
  if (GC_M_OE(insn)) {
    gc_set_ov(c, (int32_t)((~(a ^ b) & (a ^ result)) & 0x80000000u) != 0);
  }
  if (GC_M_Rc(insn)) gc_set_cr0(c, result);
  return result;
}

/* rA is read as zero when the field is 0 in the forms that allow it. */
GC_FN uint32_t gc_ra_or_zero(const GcCpu *c, uint32_t insn) {
  const unsigned ra = GC_F_rA(insn);
  return ra ? c->gpr[ra] : 0u;
}

GC_FN void gc_write_rd(GcCpu *c, uint32_t insn, uint32_t value) {
  c->gpr[GC_F_rD(insn)] = value;
}

/* The destination of the logical and rotate forms is rA. */
GC_FN void gc_write_ra(GcCpu *c, uint32_t insn, uint32_t value) {
  c->gpr[GC_F_rA(insn)] = value;
  if (GC_M_Rc(insn)) gc_set_cr0(c, value);
}

/* ------------------------------------------------------------------ */
/* Integer arithmetic                                                  */

GC_OP gc_op_add(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, gc_add(c, c->gpr[GC_F_rA(insn)], c->gpr[GC_F_rB(insn)], 0, 0, insn));
}

GC_OP gc_op_addc(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, gc_add(c, c->gpr[GC_F_rA(insn)], c->gpr[GC_F_rB(insn)], 0, 1, insn));
}

GC_OP gc_op_adde(GcCpu *c, uint32_t insn) {
  const uint32_t carry = (c->xer & GC_XER_CA) ? 1u : 0u;
  gc_write_rd(c, insn, gc_add(c, c->gpr[GC_F_rA(insn)], c->gpr[GC_F_rB(insn)], carry, 1, insn));
}

GC_OP gc_op_addi(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, gc_ra_or_zero(c, insn) + (uint32_t)GC_F_simm(insn));
}

GC_OP gc_op_addis(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, gc_ra_or_zero(c, insn) + (GC_F_uimm(insn) << 16));
}

GC_OP gc_op_addic(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, gc_add(c, c->gpr[GC_F_rA(insn)], (uint32_t)GC_F_simm(insn), 0, 1, 0));
}

/* addic. is a separate opcode, not addic with Rc: the encoding has no Rc
 * bit to spare. */
GC_OP gc_op_addic_(GcCpu *c, uint32_t insn) {
  const uint32_t result = gc_add(c, c->gpr[GC_F_rA(insn)], (uint32_t)GC_F_simm(insn), 0, 1, 0);
  gc_write_rd(c, insn, result);
  gc_set_cr0(c, result);
}

GC_OP gc_op_addme(GcCpu *c, uint32_t insn) {
  const uint32_t carry = (c->xer & GC_XER_CA) ? 1u : 0u;
  gc_write_rd(c, insn, gc_add(c, c->gpr[GC_F_rA(insn)], 0xffffffffu, carry, 1, insn));
}

GC_OP gc_op_addze(GcCpu *c, uint32_t insn) {
  const uint32_t carry = (c->xer & GC_XER_CA) ? 1u : 0u;
  gc_write_rd(c, insn, gc_add(c, c->gpr[GC_F_rA(insn)], 0u, carry, 1, insn));
}

/* subf rD,rA,rB is rB - rA, computed as ~rA + rB + 1 so that carry and
 * overflow come out of the same adder as the add forms. */
GC_OP gc_op_subf(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, gc_add(c, ~c->gpr[GC_F_rA(insn)], c->gpr[GC_F_rB(insn)], 1, 0, insn));
}

GC_OP gc_op_subfc(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, gc_add(c, ~c->gpr[GC_F_rA(insn)], c->gpr[GC_F_rB(insn)], 1, 1, insn));
}

GC_OP gc_op_subfe(GcCpu *c, uint32_t insn) {
  const uint32_t carry = (c->xer & GC_XER_CA) ? 1u : 0u;
  gc_write_rd(c, insn, gc_add(c, ~c->gpr[GC_F_rA(insn)], c->gpr[GC_F_rB(insn)], carry, 1, insn));
}

GC_OP gc_op_subfic(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, gc_add(c, ~c->gpr[GC_F_rA(insn)], (uint32_t)GC_F_simm(insn), 1, 1, 0));
}

GC_OP gc_op_subfme(GcCpu *c, uint32_t insn) {
  const uint32_t carry = (c->xer & GC_XER_CA) ? 1u : 0u;
  gc_write_rd(c, insn, gc_add(c, ~c->gpr[GC_F_rA(insn)], 0xffffffffu, carry, 1, insn));
}

GC_OP gc_op_subfze(GcCpu *c, uint32_t insn) {
  const uint32_t carry = (c->xer & GC_XER_CA) ? 1u : 0u;
  gc_write_rd(c, insn, gc_add(c, ~c->gpr[GC_F_rA(insn)], 0u, carry, 1, insn));
}

GC_OP gc_op_neg(GcCpu *c, uint32_t insn) {
  const uint32_t a = c->gpr[GC_F_rA(insn)];
  const uint32_t result = ~a + 1u;
  /* The only overflow case is negating the most negative value. */
  if (GC_M_OE(insn)) gc_set_ov(c, a == 0x80000000u);
  if (GC_M_Rc(insn)) gc_set_cr0(c, result);
  gc_write_rd(c, insn, result);
}

GC_OP gc_op_mulli(GcCpu *c, uint32_t insn) {
  gc_write_rd(c, insn, (uint32_t)((int32_t)c->gpr[GC_F_rA(insn)] * GC_F_simm(insn)));
}

GC_OP gc_op_mullw(GcCpu *c, uint32_t insn) {
  const int64_t product = (int64_t)(int32_t)c->gpr[GC_F_rA(insn)] *
                          (int64_t)(int32_t)c->gpr[GC_F_rB(insn)];
  const uint32_t result = (uint32_t)product;
  if (GC_M_OE(insn)) gc_set_ov(c, product != (int64_t)(int32_t)result);
  if (GC_M_Rc(insn)) gc_set_cr0(c, result);
  gc_write_rd(c, insn, result);
}

GC_OP gc_op_mulhw(GcCpu *c, uint32_t insn) {
  const int64_t product = (int64_t)(int32_t)c->gpr[GC_F_rA(insn)] *
                          (int64_t)(int32_t)c->gpr[GC_F_rB(insn)];
  const uint32_t result = (uint32_t)((uint64_t)product >> 32);
  if (GC_M_Rc(insn)) gc_set_cr0(c, result);
  gc_write_rd(c, insn, result);
}

GC_OP gc_op_mulhwu(GcCpu *c, uint32_t insn) {
  const uint64_t product = (uint64_t)c->gpr[GC_F_rA(insn)] * (uint64_t)c->gpr[GC_F_rB(insn)];
  const uint32_t result = (uint32_t)(product >> 32);
  if (GC_M_Rc(insn)) gc_set_cr0(c, result);
  gc_write_rd(c, insn, result);
}

/* Division by zero, and the most negative value divided by -1, leave the
 * result architecturally undefined. The value chosen here is the one the
 * 750 leaves behind; it is pinned by the vectors (docs/GC-PLAN.md,
 * "Behaviour pinned by the vectors"). */
GC_OP gc_op_divw(GcCpu *c, uint32_t insn) {
  const int32_t a = (int32_t)c->gpr[GC_F_rA(insn)];
  const int32_t b = (int32_t)c->gpr[GC_F_rB(insn)];
  uint32_t result;
  int overflow = 0;
  if (b == 0 || (a == (int32_t)0x80000000 && b == -1)) {
    overflow = 1;
    result = (a < 0) ? 0xffffffffu : 0u;
  } else {
    result = (uint32_t)(a / b);
  }
  if (GC_M_OE(insn)) gc_set_ov(c, overflow);
  if (GC_M_Rc(insn)) gc_set_cr0(c, result);
  gc_write_rd(c, insn, result);
}

GC_OP gc_op_divwu(GcCpu *c, uint32_t insn) {
  const uint32_t a = c->gpr[GC_F_rA(insn)];
  const uint32_t b = c->gpr[GC_F_rB(insn)];
  const int overflow = (b == 0);
  const uint32_t result = overflow ? 0u : a / b;
  if (GC_M_OE(insn)) gc_set_ov(c, overflow);
  if (GC_M_Rc(insn)) gc_set_cr0(c, result);
  gc_write_rd(c, insn, result);
}

/* ------------------------------------------------------------------ */
/* Compare                                                             */

GC_OP gc_op_cmp(GcCpu *c, uint32_t insn) {
  const int32_t a = (int32_t)c->gpr[GC_F_rA(insn)];
  const int32_t b = (int32_t)c->gpr[GC_F_rB(insn)];
  gc_cr_set_field(c, GC_F_crfD(insn), gc_compare_bits(c, a < b, a > b));
}

GC_OP gc_op_cmpi(GcCpu *c, uint32_t insn) {
  const int32_t a = (int32_t)c->gpr[GC_F_rA(insn)];
  const int32_t b = GC_F_simm(insn);
  gc_cr_set_field(c, GC_F_crfD(insn), gc_compare_bits(c, a < b, a > b));
}

GC_OP gc_op_cmpl(GcCpu *c, uint32_t insn) {
  const uint32_t a = c->gpr[GC_F_rA(insn)];
  const uint32_t b = c->gpr[GC_F_rB(insn)];
  gc_cr_set_field(c, GC_F_crfD(insn), gc_compare_bits(c, a < b, a > b));
}

GC_OP gc_op_cmpli(GcCpu *c, uint32_t insn) {
  const uint32_t a = c->gpr[GC_F_rA(insn)];
  const uint32_t b = GC_F_uimm(insn);
  gc_cr_set_field(c, GC_F_crfD(insn), gc_compare_bits(c, a < b, a > b));
}

/* ------------------------------------------------------------------ */
/* Logical                                                             */

GC_OP gc_op_and(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, c->gpr[GC_F_rS(insn)] & c->gpr[GC_F_rB(insn)]);
}
GC_OP gc_op_andc(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, c->gpr[GC_F_rS(insn)] & ~c->gpr[GC_F_rB(insn)]);
}
GC_OP gc_op_or(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, c->gpr[GC_F_rS(insn)] | c->gpr[GC_F_rB(insn)]);
}
GC_OP gc_op_orc(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, c->gpr[GC_F_rS(insn)] | ~c->gpr[GC_F_rB(insn)]);
}
GC_OP gc_op_xor(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, c->gpr[GC_F_rS(insn)] ^ c->gpr[GC_F_rB(insn)]);
}
GC_OP gc_op_nand(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, ~(c->gpr[GC_F_rS(insn)] & c->gpr[GC_F_rB(insn)]));
}
GC_OP gc_op_nor(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, ~(c->gpr[GC_F_rS(insn)] | c->gpr[GC_F_rB(insn)]));
}
GC_OP gc_op_eqv(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, ~(c->gpr[GC_F_rS(insn)] ^ c->gpr[GC_F_rB(insn)]));
}
GC_OP gc_op_extsb(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, (uint32_t)(int32_t)(int8_t)c->gpr[GC_F_rS(insn)]);
}
GC_OP gc_op_extsh(GcCpu *c, uint32_t insn) {
  gc_write_ra(c, insn, (uint32_t)(int32_t)(int16_t)c->gpr[GC_F_rS(insn)]);
}

GC_OP gc_op_cntlzw(GcCpu *c, uint32_t insn) {
  uint32_t value = c->gpr[GC_F_rS(insn)];
  uint32_t count = 0;
  while (count < 32 && (value & 0x80000000u) == 0) {
    value <<= 1;
    count++;
  }
  gc_write_ra(c, insn, count);
}

/* The immediate logical forms never touch CR except andi./andis., which are
 * their own opcodes with an implied Rc. */
GC_OP gc_op_ori(GcCpu *c, uint32_t insn) {
  c->gpr[GC_F_rA(insn)] = c->gpr[GC_F_rS(insn)] | GC_F_uimm(insn);
}
GC_OP gc_op_oris(GcCpu *c, uint32_t insn) {
  c->gpr[GC_F_rA(insn)] = c->gpr[GC_F_rS(insn)] | (GC_F_uimm(insn) << 16);
}
GC_OP gc_op_xori(GcCpu *c, uint32_t insn) {
  c->gpr[GC_F_rA(insn)] = c->gpr[GC_F_rS(insn)] ^ GC_F_uimm(insn);
}
GC_OP gc_op_xoris(GcCpu *c, uint32_t insn) {
  c->gpr[GC_F_rA(insn)] = c->gpr[GC_F_rS(insn)] ^ (GC_F_uimm(insn) << 16);
}
GC_OP gc_op_andi_(GcCpu *c, uint32_t insn) {
  const uint32_t result = c->gpr[GC_F_rS(insn)] & GC_F_uimm(insn);
  c->gpr[GC_F_rA(insn)] = result;
  gc_set_cr0(c, result);
}
GC_OP gc_op_andis_(GcCpu *c, uint32_t insn) {
  const uint32_t result = c->gpr[GC_F_rS(insn)] & (GC_F_uimm(insn) << 16);
  c->gpr[GC_F_rA(insn)] = result;
  gc_set_cr0(c, result);
}

/* ------------------------------------------------------------------ */
/* Rotate and shift                                                    */

/* The mask runs from bit MB to bit ME inclusive in MSB-0 numbering, and
 * wraps when MB > ME - a wrapped mask is how the architecture expresses
 * "clear the middle", and getting it wrong silently corrupts bitfield code
 * (hwtests/cputest/rlw.cpp). */
GC_FN uint32_t gc_mask(unsigned mb, unsigned me) {
  const uint32_t begin = 0xffffffffu >> mb;
  const uint32_t end = (me >= 31) ? 0xffffffffu : ~(0xffffffffu >> (me + 1));
  return (mb <= me) ? (begin & end) : (begin | end);
}

GC_FN uint32_t gc_rotl32(uint32_t value, unsigned amount) {
  amount &= 31u;
  return amount ? ((value << amount) | (value >> (32 - amount))) : value;
}

GC_OP gc_op_rlwinm(GcCpu *c, uint32_t insn) {
  const uint32_t mask = gc_mask(GC_F_MB(insn), GC_F_ME(insn));
  gc_write_ra(c, insn, gc_rotl32(c->gpr[GC_F_rS(insn)], GC_F_SH(insn)) & mask);
}

GC_OP gc_op_rlwnm(GcCpu *c, uint32_t insn) {
  const uint32_t mask = gc_mask(GC_F_MB(insn), GC_F_ME(insn));
  gc_write_ra(c, insn, gc_rotl32(c->gpr[GC_F_rS(insn)], c->gpr[GC_F_rB(insn)] & 31u) & mask);
}

GC_OP gc_op_rlwimi(GcCpu *c, uint32_t insn) {
  const uint32_t mask = gc_mask(GC_F_MB(insn), GC_F_ME(insn));
  const uint32_t rotated = gc_rotl32(c->gpr[GC_F_rS(insn)], GC_F_SH(insn));
  gc_write_ra(c, insn, (rotated & mask) | (c->gpr[GC_F_rA(insn)] & ~mask));
}

/* Shifts use the low six bits of rB, so a shift of 32 or more clears the
 * register rather than being undefined as it would be in C. */
GC_OP gc_op_slw(GcCpu *c, uint32_t insn) {
  const uint32_t amount = c->gpr[GC_F_rB(insn)] & 0x3fu;
  gc_write_ra(c, insn, amount < 32 ? (c->gpr[GC_F_rS(insn)] << amount) : 0u);
}

GC_OP gc_op_srw(GcCpu *c, uint32_t insn) {
  const uint32_t amount = c->gpr[GC_F_rB(insn)] & 0x3fu;
  gc_write_ra(c, insn, amount < 32 ? (c->gpr[GC_F_rS(insn)] >> amount) : 0u);
}

/* An arithmetic shift right sets the carry when the value was negative and
 * any one bit was shifted out (hwtests/cputest/srawix.cpp). */
GC_OP gc_op_srawi(GcCpu *c, uint32_t insn) {
  const uint32_t value = c->gpr[GC_F_rS(insn)];
  const unsigned amount = GC_F_SH(insn);
  const uint32_t lost = amount ? (value & ((1u << amount) - 1u)) : 0u;
  const uint32_t result = (uint32_t)((int32_t)value >> amount);
  gc_set_ca(c, (value & 0x80000000u) != 0 && lost != 0);
  gc_write_ra(c, insn, result);
}

GC_OP gc_op_sraw(GcCpu *c, uint32_t insn) {
  const uint32_t value = c->gpr[GC_F_rS(insn)];
  const uint32_t amount = c->gpr[GC_F_rB(insn)] & 0x3fu;
  uint32_t result;
  uint32_t lost;
  if (amount >= 32) {
    result = (value & 0x80000000u) ? 0xffffffffu : 0u;
    lost = value;
  } else {
    result = (uint32_t)((int32_t)value >> amount);
    lost = amount ? (value & ((1u << amount) - 1u)) : 0u;
  }
  gc_set_ca(c, (value & 0x80000000u) != 0 && lost != 0);
  gc_write_ra(c, insn, result);
}

/* ------------------------------------------------------------------ */
/* Condition register logic                                            */

GC_FN void gc_cr_logic(GcCpu *c, uint32_t insn, uint32_t value) {
  gc_cr_set_bit(c, GC_F_crbD(insn), value);
}

GC_OP gc_op_crand(GcCpu *c, uint32_t insn) {
  gc_cr_logic(c, insn, gc_cr_bit(c, GC_F_crbA(insn)) & gc_cr_bit(c, GC_F_crbB(insn)));
}
GC_OP gc_op_crandc(GcCpu *c, uint32_t insn) {
  gc_cr_logic(c, insn, gc_cr_bit(c, GC_F_crbA(insn)) & ~gc_cr_bit(c, GC_F_crbB(insn)) & 1u);
}
GC_OP gc_op_cror(GcCpu *c, uint32_t insn) {
  gc_cr_logic(c, insn, gc_cr_bit(c, GC_F_crbA(insn)) | gc_cr_bit(c, GC_F_crbB(insn)));
}
GC_OP gc_op_crorc(GcCpu *c, uint32_t insn) {
  gc_cr_logic(c, insn, (gc_cr_bit(c, GC_F_crbA(insn)) | ~gc_cr_bit(c, GC_F_crbB(insn))) & 1u);
}
GC_OP gc_op_crxor(GcCpu *c, uint32_t insn) {
  gc_cr_logic(c, insn, gc_cr_bit(c, GC_F_crbA(insn)) ^ gc_cr_bit(c, GC_F_crbB(insn)));
}
GC_OP gc_op_crnand(GcCpu *c, uint32_t insn) {
  gc_cr_logic(c, insn, ~(gc_cr_bit(c, GC_F_crbA(insn)) & gc_cr_bit(c, GC_F_crbB(insn))) & 1u);
}
GC_OP gc_op_crnor(GcCpu *c, uint32_t insn) {
  gc_cr_logic(c, insn, ~(gc_cr_bit(c, GC_F_crbA(insn)) | gc_cr_bit(c, GC_F_crbB(insn))) & 1u);
}
GC_OP gc_op_creqv(GcCpu *c, uint32_t insn) {
  gc_cr_logic(c, insn, ~(gc_cr_bit(c, GC_F_crbA(insn)) ^ gc_cr_bit(c, GC_F_crbB(insn))) & 1u);
}

GC_OP gc_op_mcrf(GcCpu *c, uint32_t insn) {
  gc_cr_set_field(c, GC_F_crfD(insn), gc_cr_field(c, GC_F_crfS(insn)));
}

/* mcrxr moves XER's top four bits into a CR field and clears them. */
GC_OP gc_op_mcrxr(GcCpu *c, uint32_t insn) {
  gc_cr_set_field(c, GC_F_crfD(insn), c->xer >> 28);
  c->xer &= ~(GC_XER_SO | GC_XER_OV | GC_XER_CA);
}

GC_OP gc_op_mfcr(GcCpu *c, uint32_t insn) { c->gpr[GC_F_rD(insn)] = c->cr; }

GC_OP gc_op_mtcrf(GcCpu *c, uint32_t insn) {
  const uint32_t fields = GC_F_crm(insn);
  uint32_t mask = 0;
  unsigned i;
  for (i = 0; i < 8; i++) {
    if (fields & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
  }
  c->cr = (c->cr & ~mask) | (c->gpr[GC_F_rS(insn)] & mask);
}

/* ------------------------------------------------------------------ */
/* Branches                                                            */

GC_FN void gc_branch_to(GcCpu *c, uint32_t insn, uint32_t target) {
  if (GC_M_LK(insn)) c->lr = c->pc + 4u;
  c->npc = target;
  c->branched = 1;
}

GC_OP gc_op_b(GcCpu *c, uint32_t insn) {
  const uint32_t offset = (uint32_t)GC_F_LI(insn);
  gc_branch_to(c, insn, GC_M_AA(insn) ? offset : c->pc + offset);
}

/* BO is five bits, numbered from the most significant end:
 *   BO0 ignore the condition, BO1 the value to compare it with,
 *   BO2 do not decrement CTR, BO3 branch when CTR is zero. */
GC_FN int gc_branch_taken(GcCpu *c, uint32_t insn) {
  const uint32_t bo = GC_F_BO(insn);
  int ctr_ok, cond_ok;
  if ((bo & 0x04u) == 0) c->ctr -= 1u;
  ctr_ok = (bo & 0x04u) != 0 || ((c->ctr != 0) != ((bo & 0x02u) != 0));
  cond_ok = (bo & 0x10u) != 0 || (gc_cr_bit(c, GC_F_BI(insn)) == ((bo >> 3) & 1u));
  return ctr_ok && cond_ok;
}

GC_OP gc_op_bc(GcCpu *c, uint32_t insn) {
  const uint32_t offset = (uint32_t)GC_F_BD(insn);
  const uint32_t target = GC_M_AA(insn) ? offset : c->pc + offset;
  if (gc_branch_taken(c, insn)) {
    gc_branch_to(c, insn, target);
  } else if (GC_M_LK(insn)) {
    /* A not-taken bcl still links: this is how position-independent code
     * reads its own address. */
    c->lr = c->pc + 4u;
  }
}

GC_OP gc_op_bclr(GcCpu *c, uint32_t insn) {
  /* The target is read before LK overwrites LR. */
  const uint32_t target = c->lr & ~3u;
  if (gc_branch_taken(c, insn)) {
    gc_branch_to(c, insn, target);
  } else if (GC_M_LK(insn)) {
    c->lr = c->pc + 4u;
  }
}

GC_OP gc_op_bcctr(GcCpu *c, uint32_t insn) {
  /* bcctr never decrements CTR; BO2 is required to be set. */
  const uint32_t bo = GC_F_BO(insn);
  const int cond_ok = (bo & 0x10u) != 0 || (gc_cr_bit(c, GC_F_BI(insn)) == ((bo >> 3) & 1u));
  if (cond_ok) {
    gc_branch_to(c, insn, c->ctr & ~3u);
  } else if (GC_M_LK(insn)) {
    c->lr = c->pc + 4u;
  }
}

/* ------------------------------------------------------------------ */
/* System                                                              */

GC_OP gc_op_sc(GcCpu *c, uint32_t insn) {
  (void)insn;
  gc_bus_syscall(c);
}

GC_OP gc_op_rfi(GcCpu *c, uint32_t insn) {
  (void)insn;
  /* The bits SRR1 restores; the rest of MSR is kept. */
  const uint32_t mask = 0x87c0ff73u;
  c->msr = (c->srr1 & mask) | (c->msr & ~mask);
  c->npc = c->srr0 & ~3u;
  c->branched = 1;
  /* MSR[EE] may have come back on with an interrupt waiting. */
  c->next_event = 0;
}

GC_FN int gc_trap_condition(uint32_t to, int32_t a, int32_t b) {
  const uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
  return ((to & 0x10u) && a < b) || ((to & 0x08u) && a > b) || ((to & 0x04u) && a == b) ||
         ((to & 0x02u) && ua < ub) || ((to & 0x01u) && ua > ub);
}

GC_OP gc_op_tw(GcCpu *c, uint32_t insn) {
  if (gc_trap_condition(GC_F_TO(insn), (int32_t)c->gpr[GC_F_rA(insn)],
                        (int32_t)c->gpr[GC_F_rB(insn)])) {
    c->pending_vector = GC_VEC_PROGRAM;
  }
}

GC_OP gc_op_twi(GcCpu *c, uint32_t insn) {
  if (gc_trap_condition(GC_F_TO(insn), (int32_t)c->gpr[GC_F_rA(insn)], GC_F_simm(insn))) {
    c->pending_vector = GC_VEC_PROGRAM;
  }
}

/* ------------------------------------------------------------------ */
/* Special-purpose registers                                           */

uint32_t gc_spr_read(GcCpu *c, uint32_t spr);
void gc_spr_write(GcCpu *c, uint32_t spr, uint32_t value);

GC_OP gc_op_mfspr(GcCpu *c, uint32_t insn) {
  c->gpr[GC_F_rD(insn)] = gc_spr_read(c, GC_F_spr(insn));
}

GC_OP gc_op_mtspr(GcCpu *c, uint32_t insn) {
  gc_spr_write(c, GC_F_spr(insn), c->gpr[GC_F_rS(insn)]);
}

GC_OP gc_op_mftb(GcCpu *c, uint32_t insn) {
  const uint64_t tb = gc_cpu_timebase(c);
  const uint32_t tbr = GC_F_tbr(insn);
  c->gpr[GC_F_rD(insn)] = (tbr == GC_SPR_TBU_R) ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

GC_OP gc_op_mfmsr(GcCpu *c, uint32_t insn) { c->gpr[GC_F_rD(insn)] = c->msr; }

GC_OP gc_op_mtmsr(GcCpu *c, uint32_t insn) {
  c->msr = c->gpr[GC_F_rS(insn)];
  c->next_event = 0; /* as for rfi */
}

GC_OP gc_op_mfsr(GcCpu *c, uint32_t insn) { c->gpr[GC_F_rD(insn)] = c->sr[GC_F_sr(insn)]; }
GC_OP gc_op_mtsr(GcCpu *c, uint32_t insn) { c->sr[GC_F_sr(insn)] = c->gpr[GC_F_rS(insn)]; }
GC_OP gc_op_mfsrin(GcCpu *c, uint32_t insn) {
  c->gpr[GC_F_rD(insn)] = c->sr[(c->gpr[GC_F_rB(insn)] >> 28) & 0xfu];
}
GC_OP gc_op_mtsrin(GcCpu *c, uint32_t insn) {
  c->sr[(c->gpr[GC_F_rB(insn)] >> 28) & 0xfu] = c->gpr[GC_F_rS(insn)];
}

/* ------------------------------------------------------------------ */
/* Cache and ordering                                                  */

/* The ordering instructions are where our bus model promises to have
 * finished anything it has buffered - the write-gather pipe above all. */
GC_OP gc_op_sync(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_isync(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_eieio(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_tlbsync(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_tlbie(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_dcbt(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_dcbtst(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_dcbst(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_dcbf(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }
GC_OP gc_op_dcbi(GcCpu *c, uint32_t insn) { (void)c; (void)insn; }

GC_OP gc_op_dcbz(GcCpu *c, uint32_t insn) {
  gc_bus_dcbz(c, (gc_ra_or_zero(c, insn) + c->gpr[GC_F_rB(insn)]) & ~31u);
}

GC_OP gc_op_dcbz_l(GcCpu *c, uint32_t insn) {
  gc_bus_dcbz_l(c, (gc_ra_or_zero(c, insn) + c->gpr[GC_F_rB(insn)]) & ~31u);
}

GC_OP gc_op_icbi(GcCpu *c, uint32_t insn) {
  gc_bus_icbi(c, (gc_ra_or_zero(c, insn) + c->gpr[GC_F_rB(insn)]) & ~31u);
}

/* ------------------------------------------------------------------ */
/* Loads and stores                                                    */

GC_FN uint32_t gc_ea_d(const GcCpu *c, uint32_t insn) {
  return gc_ra_or_zero(c, insn) + (uint32_t)GC_F_simm(insn);
}

GC_FN uint32_t gc_ea_x(const GcCpu *c, uint32_t insn) {
  return gc_ra_or_zero(c, insn) + c->gpr[GC_F_rB(insn)];
}

/* The update forms use rA directly (rA == 0 is an invalid form) and write it
 * back only when the access completed. */
GC_FN uint32_t gc_ea_du(const GcCpu *c, uint32_t insn) {
  return c->gpr[GC_F_rA(insn)] + (uint32_t)GC_F_simm(insn);
}

GC_FN uint32_t gc_ea_xu(const GcCpu *c, uint32_t insn) {
  return c->gpr[GC_F_rA(insn)] + c->gpr[GC_F_rB(insn)];
}

GC_FN void gc_update_ra(GcCpu *c, uint32_t insn, uint32_t ea) {
  if (!c->pending_vector) c->gpr[GC_F_rA(insn)] = ea;
}

#define GC_LOAD(name, ea_expr, load_expr, update)                       \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                         \
    const uint32_t ea = ea_expr(c, insn);                               \
    const uint32_t value = (uint32_t)(load_expr);                       \
    if (c->pending_vector) return;                                      \
    c->gpr[GC_F_rD(insn)] = value;                                      \
    if (update) gc_update_ra(c, insn, ea);                              \
  }

GC_LOAD(lbz, gc_ea_d, gc_bus_fast_read8(c, ea), 0)
GC_LOAD(lbzu, gc_ea_du, gc_bus_fast_read8(c, ea), 1)
GC_LOAD(lbzx, gc_ea_x, gc_bus_fast_read8(c, ea), 0)
GC_LOAD(lbzux, gc_ea_xu, gc_bus_fast_read8(c, ea), 1)
GC_LOAD(lhz, gc_ea_d, gc_bus_fast_read16(c, ea), 0)
GC_LOAD(lhzu, gc_ea_du, gc_bus_fast_read16(c, ea), 1)
GC_LOAD(lhzx, gc_ea_x, gc_bus_fast_read16(c, ea), 0)
GC_LOAD(lhzux, gc_ea_xu, gc_bus_fast_read16(c, ea), 1)
GC_LOAD(lha, gc_ea_d, (int32_t)(int16_t)gc_bus_fast_read16(c, ea), 0)
GC_LOAD(lhau, gc_ea_du, (int32_t)(int16_t)gc_bus_fast_read16(c, ea), 1)
GC_LOAD(lhax, gc_ea_x, (int32_t)(int16_t)gc_bus_fast_read16(c, ea), 0)
GC_LOAD(lhaux, gc_ea_xu, (int32_t)(int16_t)gc_bus_fast_read16(c, ea), 1)
GC_LOAD(lwz, gc_ea_d, gc_bus_fast_read32(c, ea), 0)
GC_LOAD(lwzu, gc_ea_du, gc_bus_fast_read32(c, ea), 1)
GC_LOAD(lwzx, gc_ea_x, gc_bus_fast_read32(c, ea), 0)
GC_LOAD(lwzux, gc_ea_xu, gc_bus_fast_read32(c, ea), 1)

/* The byte-reverse forms read a little-endian value out of big-endian
 * memory; the guest uses them for file formats and for hardware that is
 * wired the other way round. */
GC_FN uint32_t gc_bswap16(uint32_t value) {
  return ((value & 0xffu) << 8) | ((value >> 8) & 0xffu);
}

GC_FN uint32_t gc_bswap32(uint32_t value) {
  return (value >> 24) | ((value >> 8) & 0xff00u) | ((value << 8) & 0xff0000u) | (value << 24);
}

GC_LOAD(lhbrx, gc_ea_x, gc_bswap16(gc_bus_fast_read16(c, ea)), 0)
GC_LOAD(lwbrx, gc_ea_x, gc_bswap32(gc_bus_fast_read32(c, ea)), 0)

#define GC_STORE(name, ea_expr, store_stmt, update)                     \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                         \
    const uint32_t ea = ea_expr(c, insn);                               \
    store_stmt;                                                         \
    if (update) gc_update_ra(c, insn, ea);                              \
  }

GC_STORE(stb, gc_ea_d, gc_bus_fast_write8(c, ea, (uint8_t)c->gpr[GC_F_rS(insn)]), 0)
GC_STORE(stbu, gc_ea_du, gc_bus_fast_write8(c, ea, (uint8_t)c->gpr[GC_F_rS(insn)]), 1)
GC_STORE(stbx, gc_ea_x, gc_bus_fast_write8(c, ea, (uint8_t)c->gpr[GC_F_rS(insn)]), 0)
GC_STORE(stbux, gc_ea_xu, gc_bus_fast_write8(c, ea, (uint8_t)c->gpr[GC_F_rS(insn)]), 1)
GC_STORE(sth, gc_ea_d, gc_bus_fast_write16(c, ea, (uint16_t)c->gpr[GC_F_rS(insn)]), 0)
GC_STORE(sthu, gc_ea_du, gc_bus_fast_write16(c, ea, (uint16_t)c->gpr[GC_F_rS(insn)]), 1)
GC_STORE(sthx, gc_ea_x, gc_bus_fast_write16(c, ea, (uint16_t)c->gpr[GC_F_rS(insn)]), 0)
GC_STORE(sthux, gc_ea_xu, gc_bus_fast_write16(c, ea, (uint16_t)c->gpr[GC_F_rS(insn)]), 1)
GC_STORE(stw, gc_ea_d, gc_bus_fast_write32(c, ea, c->gpr[GC_F_rS(insn)]), 0)
GC_STORE(stwu, gc_ea_du, gc_bus_fast_write32(c, ea, c->gpr[GC_F_rS(insn)]), 1)
GC_STORE(stwx, gc_ea_x, gc_bus_fast_write32(c, ea, c->gpr[GC_F_rS(insn)]), 0)
GC_STORE(stwux, gc_ea_xu, gc_bus_fast_write32(c, ea, c->gpr[GC_F_rS(insn)]), 1)
GC_STORE(sthbrx, gc_ea_x, gc_bus_fast_write16(c, ea, (uint16_t)gc_bswap16(c->gpr[GC_F_rS(insn)])), 0)
GC_STORE(stwbrx, gc_ea_x, gc_bus_fast_write32(c, ea, gc_bswap32(c->gpr[GC_F_rS(insn)])), 0)

/* Load/store multiple: rD through r31, one word each, ascending. */
GC_OP gc_op_lmw(GcCpu *c, uint32_t insn) {
  uint32_t ea = gc_ea_d(c, insn);
  unsigned reg;
  for (reg = GC_F_rD(insn); reg < 32; reg++) {
    const uint32_t value = gc_bus_fast_read32(c, ea);
    if (c->pending_vector) return;
    c->gpr[reg] = value;
    ea += 4u;
  }
}

GC_OP gc_op_stmw(GcCpu *c, uint32_t insn) {
  uint32_t ea = gc_ea_d(c, insn);
  unsigned reg;
  for (reg = GC_F_rS(insn); reg < 32; reg++) {
    gc_bus_fast_write32(c, ea, c->gpr[reg]);
    if (c->pending_vector) return;
    ea += 4u;
  }
}

/* The string forms move bytes into and out of the register file, packing
 * four bytes to a register and wrapping past r31 back to r0. */
GC_FN void gc_load_string(GcCpu *c, uint32_t ea, unsigned start, unsigned count) {
  unsigned reg = start;
  unsigned byte = 0;
  /* A register is cleared when its first byte arrives, and not before: the
   * register after the last one filled is not part of the load. Clearing it
   * on the way out - as this did - zeroes one register too many whenever
   * the count is a multiple of four, and a 32-byte load into r25 onwards
   * (which wraps to r0) then takes the stack pointer with it. The 750CL
   * test (tests/wii/programs/wii_cputest.c) found it by not coming back. */
  while (count--) {
    const uint8_t value = gc_bus_fast_read8(c, ea++);
    if (c->pending_vector) return;
    if (byte == 0) c->gpr[reg] = 0;
    c->gpr[reg] |= (uint32_t)value << (24 - 8 * byte);
    if (++byte == 4) {
      byte = 0;
      reg = (reg + 1) & 31u;
    }
  }
}

GC_FN void gc_store_string(GcCpu *c, uint32_t ea, unsigned start, unsigned count) {
  unsigned reg = start;
  unsigned byte = 0;
  while (count--) {
    gc_bus_fast_write8(c, ea++, (uint8_t)(c->gpr[reg] >> (24 - 8 * byte)));
    if (c->pending_vector) return;
    if (++byte == 4) {
      byte = 0;
      reg = (reg + 1) & 31u;
    }
  }
}

GC_OP gc_op_lswi(GcCpu *c, uint32_t insn) {
  const unsigned nb = GC_F_NB(insn);
  gc_load_string(c, gc_ra_or_zero(c, insn), GC_F_rD(insn), nb ? nb : 32u);
}

GC_OP gc_op_stswi(GcCpu *c, uint32_t insn) {
  const unsigned nb = GC_F_NB(insn);
  gc_store_string(c, gc_ra_or_zero(c, insn), GC_F_rS(insn), nb ? nb : 32u);
}

GC_OP gc_op_lswx(GcCpu *c, uint32_t insn) {
  gc_load_string(c, gc_ea_x(c, insn), GC_F_rD(insn), c->xer & GC_XER_BYTES);
}

GC_OP gc_op_stswx(GcCpu *c, uint32_t insn) {
  gc_store_string(c, gc_ea_x(c, insn), GC_F_rS(insn), c->xer & GC_XER_BYTES);
}

/* lwarx takes a reservation; stwcx. succeeds only if it is still held. A
 * store to the reserved block from anywhere - including this CPU - clears
 * it. The block is a physical one, so the bus keeps it. */
GC_OP gc_op_lwarx(GcCpu *c, uint32_t insn) {
  const uint32_t ea = gc_ea_x(c, insn);
  const uint32_t value = gc_bus_fast_read32(c, ea);
  if (c->pending_vector) return;
  c->gpr[GC_F_rD(insn)] = value;
  gc_bus_reserve(c, ea);
}

GC_OP gc_op_stwcx_(GcCpu *c, uint32_t insn) {
  const uint32_t ea = gc_ea_x(c, insn);
  uint32_t bits;
  if (gc_bus_reserve_held(c, ea)) {
    gc_bus_fast_write32(c, ea, c->gpr[GC_F_rS(insn)]);
    if (c->pending_vector) return;
    bits = 2u; /* EQ: the store happened */
  } else {
    bits = 0u;
  }
  if (c->xer & GC_XER_SO) bits |= 1u;
  gc_cr_set_field(c, 0, bits);
  c->reserve = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* GC_CPU_IMPL_H */
