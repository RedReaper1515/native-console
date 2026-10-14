/*
 * Gekko floating point: scalar, paired singles and the quantised load/store
 * forms. The other half of the single semantics source (gc_cpu_impl.h).
 *
 * Three things here are Gekko-specific and are where an emulator usually
 * goes wrong:
 *
 *  - **A register holds two lanes.** Every FPR is a pair. Scalar
 *    instructions write ps0; `lfs` and `fres` write both; paired-single
 *    instructions work on both. ps1 is architectural state, so it is part
 *    of every state dump and every differential comparison.
 *
 *  - **Single-precision values live in double format.** Arithmetic is done
 *    in double and rounded back to single. That is exact for add, subtract,
 *    multiply, divide and fused multiply-add whenever the operands are
 *    themselves single-representable, which the architecture requires of
 *    the single-precision forms.
 *
 *  - **Quantised load/store convert as they go.** psq_l and psq_st read the
 *    scale and type out of one of eight GQRs, so the same instruction word
 *    means something different depending on a register written long before.
 *    The recompiler cannot fold that away; it reads the GQR at run time.
 *
 * NaN selection, the FPSCR result class (FPRF) and the exception bits are
 * written explicitly rather than inherited from the host, because the host
 * makes no promises about which NaN survives an operation.
 */
#ifndef GC_FPU_IMPL_H
#define GC_FPU_IMPL_H

#include <fenv.h>
#include <math.h>
#include <string.h>

#include "gc_bus.h"
#include "gc_cpu.h"
#include "gc_cpu_impl.h"
#include "gc_estimate_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The full paths are called only where the fast one below declines, so
 * each is one shared copy a file rather than inlined at every floating-point
 * instruction: a generated game has thousands of those, and inlining both
 * paths everywhere cost its build about seven seconds. */
#if defined(_MSC_VER)
#define GC_FP_SLOW static __declspec(noinline)
#elif defined(__GNUC__)
#define GC_FP_SLOW static __attribute__((noinline, unused))
#else
#define GC_FP_SLOW static
#endif

/* FPSCR can be behind (see "FPSCR, a little behind" below): anything that
 * reads or changes it other than its rounding and non-IEEE bits settles it
 * first. */
GC_FP_SLOW void gc_fp_settle_slow(GcCpu *c);

GC_FN void gc_fp_settle(GcCpu *c) {
  if (c->fp_lazy) gc_fp_settle_slow(c);
}

#define GC_QNAN_DEFAULT 0x7ff8000000000000ull
#define GC_SIGN_BIT 0x8000000000000000ull

/* ------------------------------------------------------------------ */
/* Bit patterns and classification                                     */

GC_FN double gc_f64(uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

GC_FN uint64_t gc_f64_bits(double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

GC_FN float gc_f32(uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/* Converting a double to single for a store is defined bit by bit, not as
 * a host cast: values below the single-precision normal range are
 * denormalized by shifting the significand, and the architecture leaves the
 * far end (an exponent below 874) undefined, where the hardware keeps the
 * same field-packing it uses for the normal case. A host cast would flush
 * those to zero or infinity, which is what the vectors caught. */
GC_FN uint32_t gc_store_single(uint64_t bits) {
  const uint32_t exponent = (uint32_t)((bits >> 52) & 0x7ffu);
  if (exponent > 896 || (bits & 0x7fffffffffffffffull) == 0) {
    return (uint32_t)(((bits >> 32) & 0xc0000000u) | ((bits >> 29) & 0x3fffffffu));
  }
  if (exponent >= 874) {
    uint32_t significand = 0x80000000u | (uint32_t)((bits & 0x000fffffffffffffull) >> 21);
    significand >>= (905 - exponent);
    significand |= (uint32_t)((bits >> 32) & 0x80000000u);
    return significand;
  }
  return (uint32_t)(((bits >> 32) & 0xc0000000u) | ((bits >> 29) & 0x3fffffffu));
}

GC_FN uint32_t gc_f32_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/* Widening a single to a double, without the host's help for NaNs: a C
 * conversion quiets a signalling NaN, and the hardware does not. A load is
 * a conversion, not an operation, so the bits come through as they are -
 * which hwtests/cputest/nan.cpp checks through psq_l. */
GC_FN uint64_t gc_single_to_double(uint32_t single) {
  if ((single & 0x7f800000u) == 0x7f800000u && (single & 0x007fffffu) != 0) {
    return ((uint64_t)(single & 0x80000000u) << 32) | 0x7ff0000000000000ull |
           ((uint64_t)(single & 0x007fffffu) << 29);
  }
  {
    float value;
    memcpy(&value, &single, sizeof(value));
    {
      const double wide = (double)value;
      uint64_t bits;
      memcpy(&bits, &wide, sizeof(bits));
      return bits;
    }
  }
}

/* isinf, as a bit test. Under MSVC the macro is a call into the C library
 * that classifies the value, several times a guest floating-point
 * instruction. */
GC_FN int gc_isinf(double value) {
  return (gc_f64_bits(value) & 0x7fffffffffffffffull) == 0x7ff0000000000000ull;
}

/* ldexp(1.0, k) for the exponents a GQR scale gives (-32 to 31, either
 * sign): an exact power of two, so building its bits is the same double. */
GC_FN double gc_power_of_two(int k) { return gc_f64((uint64_t)(1023 + k) << 52); }

GC_FN int gc_is_nan(uint64_t bits) {
  return (bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
         (bits & 0x000fffffffffffffull) != 0;
}

GC_FN int gc_is_snan(uint64_t bits) {
  return gc_is_nan(bits) && (bits & 0x0008000000000000ull) == 0;
}

GC_FN uint64_t gc_quiet(uint64_t bits) { return bits | 0x0008000000000000ull; }

/* FPSCR[FPRF]: the class and sign of a result, as the architecture tables
 * it (C, FL, FG, FE, FU). hwtests/cputest/fprf.cpp checks every case. */
GC_FN uint32_t gc_fprf_of(uint64_t bits) {
  const int sign = (bits & GC_SIGN_BIT) != 0;
  const uint64_t exponent = bits & 0x7ff0000000000000ull;
  const uint64_t mantissa = bits & 0x000fffffffffffffull;
  if (exponent == 0x7ff0000000000000ull) {
    if (mantissa) return 0x11u;                   /* QNaN */
    return sign ? 0x09u : 0x05u;                  /* -Inf, +Inf */
  }
  if (exponent == 0) {
    if (mantissa) return sign ? 0x18u : 0x14u;    /* denormal */
    return sign ? 0x12u : 0x02u;                  /* zero */
  }
  return sign ? 0x08u : 0x04u;                    /* normal */
}

/* A single-precision instruction reports the class of its result as a
 * single, not as the double it is stored in: a value that is denormal in
 * single precision is a perfectly ordinary double, and calling it "normal"
 * is what makes an underflow invisible to the guest. */
GC_FN uint32_t gc_fprf_of_single(uint64_t bits) {
  /* The common answers without the conversion. A double whose exponent is
   * within a single's normal range and below its top (0x381 to 0x47d, so
   * 2^-126 <= |v| < 2^127) narrows to a normal single in any rounding mode:
   * 2^-126 is itself a single, so rounding cannot take a value at or above
   * it below it, and nothing under 2^127 rounds past the largest single. At
   * the top exponent the same holds when the low 29 bits are clear, because
   * then nothing is rounded at all. A zero narrows to itself. */
  const uint32_t biased = (uint32_t)(bits >> 52) & 0x7ffu;
  if (biased - 0x381u <= 0x47du - 0x381u || (biased == 0x47eu && (bits & 0x000000001fffffffull) == 0)) {
    return (bits & GC_SIGN_BIT) ? 0x08u : 0x04u;
  }
  if ((bits & ~GC_SIGN_BIT) == 0) return (bits & GC_SIGN_BIT) ? 0x12u : 0x02u;
  {
  const uint32_t single = gc_f32_bits((float)gc_f64(bits));
  const int sign = (single & 0x80000000u) != 0;
  const uint32_t exponent = single & 0x7f800000u;
  const uint32_t mantissa = single & 0x007fffffu;
  if (exponent == 0x7f800000u) {
    if (mantissa) return 0x11u;
    return sign ? 0x09u : 0x05u;
  }
  if (exponent == 0) {
    if (mantissa) return sign ? 0x18u : 0x14u;
    return sign ? 0x12u : 0x02u;
  }
  return sign ? 0x08u : 0x04u;
  }
}

GC_FN void gc_set_fprf(GcCpu *c, uint64_t bits) {
  gc_fp_settle(c);
  c->fpscr = (c->fpscr & ~GC_FPSCR_FPRF) | (gc_fprf_of(bits) << 12);
}

GC_FN void gc_set_fprf_single(GcCpu *c, uint64_t bits) {
  gc_fp_settle(c);
  c->fpscr = (c->fpscr & ~GC_FPSCR_FPRF) | (gc_fprf_of_single(bits) << 12);
}

/* Setting any exception bit sets the summary bit FX. VX is the OR of every
 * invalid-operation bit; the architecture keeps it derived, so it is
 * recomputed here rather than stored twice. */
GC_FN void gc_raise_fpscr(GcCpu *c, uint32_t bits) {
  uint32_t before;
  gc_fp_settle(c);
  before = c->fpscr;
  c->fpscr |= bits;
  if ((c->fpscr & ~before) != 0) c->fpscr |= GC_FPSCR_FX;
}

GC_FN void gc_update_vx(GcCpu *c) {
  const uint32_t invalid = GC_FPSCR_VXSNAN | GC_FPSCR_VXISI | GC_FPSCR_VXIDI | GC_FPSCR_VXZDZ |
                           GC_FPSCR_VXIMZ | GC_FPSCR_VXVC | GC_FPSCR_VXSOFT | GC_FPSCR_VXSQRT |
                           GC_FPSCR_VXCVI;
  uint32_t f;
  gc_fp_settle(c);
  f = c->fpscr;
  f = (f & ~GC_FPSCR_VX) | ((f & invalid) ? GC_FPSCR_VX : 0u);
  /* FEX is the summary of the exceptions that are enabled to trap. Like VX
   * it is derived, not stored: writing it has no effect. Each of the five
   * exception bits (VX, OX, UX, ZX, XX: bits 29 to 25) sits exactly 22 bits
   * above its enable (VE, OE, UE, ZE, XE: bits 7 to 3), so one shift pairs
   * them all - an instruction's worth of branches otherwise, on every
   * floating-point instruction. */
  f = (f & ~GC_FPSCR_FEX) | (((f >> 22) & f & 0xf8u) ? GC_FPSCR_FEX : 0u);
  c->fpscr = f;
}

/* Every write to FPSCR goes through here: the two summary bits are derived
 * from the rest, so a write cannot set them directly. The vector harness
 * uses it too, because loading a state is a write. */
GC_FN void gc_fpscr_write(GcCpu *c, uint32_t value, uint32_t mask) {
  const uint32_t writable = ~(GC_FPSCR_FEX | GC_FPSCR_VX | GC_FPSCR_RESERVED);
  gc_fp_settle(c);
  c->fpscr = (c->fpscr & ~mask) | (value & mask & writable);
  gc_update_vx(c);
}

/* ------------------------------------------------------------------ */
/* Rounding and the host's exception flags                             */

/* The host's rounding mode and exception flags.
 *
 * On x86-64 every double and float operation here is an SSE operation, so
 * the whole of the host's floating-point environment that matters is one
 * register, MXCSR: the rounding field in bits 13-14 and the sticky flags in
 * bits 0-5. The C library's fegetround, fesetround, feclearexcept and
 * fetestexcept read and write that same register, but as calls - four or
 * five a guest floating-point instruction, and under MSVC each of them
 * rebuilds a whole abstract environment. A game computing in non-IEEE mode
 * spent 60 % of its loading screen in them. Reading the register directly
 * is the same state through a shorter path.
 *
 * Elsewhere (ARM64, or an x86 build using x87 arithmetic) the C library is
 * the only portable way in, and it stays. */
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#include <xmmintrin.h>
#define GC_HOST_ROUND_NEAREST 0x0000
#define GC_HOST_ROUND_DOWN 0x2000
#define GC_HOST_ROUND_UP 0x4000
#define GC_HOST_ROUND_ZERO 0x6000
#define GC_HOST_OVERFLOW 0x08
#define GC_HOST_UNDERFLOW 0x10
#define GC_HOST_INEXACT 0x20
/* FE_ALL_EXCEPT: invalid, divide by zero, overflow, underflow, inexact -
 * not the denormal-operand flag, which C has no name for. */
#define GC_HOST_ALL_FLAGS 0x3d

GC_FN int gc_host_getround(void) { return (int)(_mm_getcsr() & 0x6000u); }

GC_FN void gc_host_setround(int mode) {
  const unsigned csr = _mm_getcsr();
  const unsigned wanted = (csr & ~0x6000u) | (unsigned)mode;
  if (wanted != csr) _mm_setcsr(wanted);
}

/* Rounding mode and clear flags in one write. Returns the previous mode. */
GC_FN int gc_host_enter(int mode) {
  const unsigned csr = _mm_getcsr();
  const unsigned wanted = (csr & ~(0x6000u | GC_HOST_ALL_FLAGS)) | (unsigned)mode;
  if (wanted != csr) _mm_setcsr(wanted);
  return (int)(csr & 0x6000u);
}

GC_FN int gc_host_flags(void) {
  return (int)(_mm_getcsr() & (GC_HOST_OVERFLOW | GC_HOST_UNDERFLOW | GC_HOST_INEXACT));
}
#else
#define GC_HOST_ROUND_NEAREST FE_TONEAREST
#define GC_HOST_ROUND_DOWN FE_DOWNWARD
#define GC_HOST_ROUND_UP FE_UPWARD
#define GC_HOST_ROUND_ZERO FE_TOWARDZERO
#define GC_HOST_OVERFLOW FE_OVERFLOW
#define GC_HOST_UNDERFLOW FE_UNDERFLOW
#define GC_HOST_INEXACT FE_INEXACT

GC_FN int gc_host_getround(void) { return fegetround(); }

GC_FN void gc_host_setround(int mode) {
  if (mode != fegetround()) fesetround(mode);
}

GC_FN int gc_host_enter(int mode) {
  const int previous = fegetround();
  if (mode != previous) fesetround(mode);
  feclearexcept(FE_ALL_EXCEPT);
  return previous;
}

GC_FN int gc_host_flags(void) { return fetestexcept(FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT); }
#endif

/* FPSCR[RN] maps onto the host rounding mode. Round-to-nearest is left
 * alone because it is both the host default and what games use; the other
 * three modes cost a mode switch, which is the price of getting
 * `hwtests/cputest/frsp.cpp` right in every mode. */
GC_FN int gc_host_round_mode(const GcCpu *c) {
  switch (c->fpscr & GC_FPSCR_RN) {
    case 0: return GC_HOST_ROUND_NEAREST;
    case 1: return GC_HOST_ROUND_ZERO;
    case 2: return GC_HOST_ROUND_UP;
    default: return GC_HOST_ROUND_DOWN;
  }
}

/* Settling first: settling computes, and must not do it between an
 * operation and the read of the host's flags that follows it. */
GC_FN int gc_round_enter(GcCpu *c) {
  gc_fp_settle(c);
  return gc_host_enter(gc_host_round_mode(c));
}

GC_FN int gc_round_leave(GcCpu *c, int previous) {
  const int raised = gc_host_flags();
  gc_fp_settle(c); /* already settled by gc_round_enter */
  uint32_t bits = 0;
  if (raised & GC_HOST_OVERFLOW) bits |= GC_FPSCR_OX;
  if (raised & GC_HOST_UNDERFLOW) bits |= GC_FPSCR_UX;
  if (raised & GC_HOST_INEXACT) bits |= GC_FPSCR_XX;
  /* FI says the result was inexact; FR says it was rounded up. FR needs the
   * exact residue, which the host does not expose, so it is left clear and
   * listed as a known deviation (docs/GC-PLAN.md). */
  if (raised & GC_HOST_INEXACT) {
    c->fpscr |= GC_FPSCR_FI;
  } else {
    c->fpscr &= ~(GC_FPSCR_FI | GC_FPSCR_FR);
  }
  if (bits) gc_raise_fpscr(c, bits);
  gc_host_setround(previous);
  return raised;
}

/* Non-IEEE mode flushes denormal *results* to zero of the same sign. It
 * does not touch the operands, whatever the manual's Table 2-21 says:
 * hwtests/cputest/ni.cpp checks a multiply of a denormal by a large number
 * and expects the real product, in both modes, and that test was written
 * against hardware.
 *
 * The flush follows underflow, not just a denormal result. Multiplying the
 * smallest normal by one-minus-an-ulp rounds back up to the smallest
 * normal, so the result is not denormal - and hardware still returns zero,
 * because the exact result was below the normal range. */
GC_FN int gc_non_ieee(const GcCpu *c) {
  return (c->fpscr & GC_FPSCR_NI) != 0 || (c->hid2 & 0x08000000u) != 0;
}

/* The multiplier is 25 bits wide for single-precision work: frC is rounded
 * to 25 significand bits before a single-precision multiply, so fmuls and
 * the paired multiplies can differ from a plain double multiply by an ulp.
 * Games are built on this - it is what makes a recompiled physics step land
 * on the same numbers - and the vectors show it. */
GC_FN uint64_t gc_force_25bit(uint64_t bits) {
  return (bits & 0xfffffffff8000000ull) + (bits & 0x0000000008000000ull);
}

/* A result that is denormal as a single flushes to a zero of the same sign
 * when non-IEEE mode is on (hwtests/cputest/ni.cpp). The double-denormal
 * rule below is not enough: 2^-149 is a perfectly good double. */
/* The smallest normal number of each precision, as bit patterns. */
#define GC_SMALLEST_NORMAL_SINGLE 0x3810000000000000ull
#define GC_SMALLEST_NORMAL_DOUBLE 0x0010000000000000ull

/* Whether the exact result was below the normal range - what the
 * architecture calls tininess, judged *before* rounding.
 *
 * The host cannot be asked: x86 decides tininess after rounding, so a value
 * a hair under the smallest normal that rounds up to it raises nothing at
 * all. Recomputing the same operation toward zero settles which side of the
 * boundary the exact value fell on, because truncation never crosses it
 * upwards. */
GC_FN int gc_tiny(double toward_zero, int single) {
  const double threshold = gc_f64(single ? GC_SMALLEST_NORMAL_SINGLE : GC_SMALLEST_NORMAL_DOUBLE);
  const double magnitude = toward_zero < 0.0 ? -toward_zero : toward_zero;
  return magnitude != 0.0 && magnitude < threshold;
}

/* Whether gc_tiny could say yes, judged from the result already rounded in
 * the guest's mode - so that a result nowhere near the bottom of the range
 * does not pay for a second computation in another rounding mode.
 *
 * T, the smallest normal of the precision, is representable in it. If the
 * exact value v had |v| < T, then whichever way it was rounded - to double,
 * then to single, in any mode, nudged by one double toward v as the fused
 * single path does - its magnitude could not pass T, the representable
 * number just above it. So a result of magnitude at least 2T had |v| >= T,
 * and truncating such a v toward zero, to double and then to single, stops
 * at T or above: gc_tiny would say no. Anything smaller, or a NaN, takes
 * the full test. */
GC_FN int gc_maybe_tiny(double result, int single) {
  const double twice = 2.0 * gc_f64(single ? GC_SMALLEST_NORMAL_SINGLE : GC_SMALLEST_NORMAL_DOUBLE);
  const double magnitude = result < 0.0 ? -result : result;
  return !(magnitude >= twice);
}

GC_FN uint64_t gc_flush_single_denormal(const GcCpu *c, uint64_t bits) {
  const uint64_t exponent = bits & 0x7ff0000000000000ull;
  if (!gc_non_ieee(c)) return bits;
  if (exponent != 0 && exponent < 0x3810000000000000ull) return bits & GC_SIGN_BIT;
  return bits;
}

/* Anything that leaves the paired-single datapath is a single, even a lane
 * that was only copied: ps_sum0 passes frC's ps1 through, and hardware
 * rounds it on the way (Interpreter_Paired's ForceSingle, and the vectors).
 * A double that is too small to be a single comes out as zero. */
GC_FN uint64_t gc_force_single(GcCpu *c, uint64_t bits) {
  uint64_t rounded;
  int raised;
  int previous;
  if (gc_is_nan(bits)) {
    /* Narrowing a NaN drops the mantissa bits a single does not have. It
     * does not quiet it: only an operation does that. */
    return bits & ~0x000000001fffffffull;
  }
  previous = gc_round_enter(c);
  rounded = gc_f64_bits((double)(float)gc_f64(bits));
  raised = gc_round_leave(c, previous);
  if (gc_non_ieee(c) && (raised & GC_HOST_UNDERFLOW)) return rounded & GC_SIGN_BIT;
  return gc_flush_single_denormal(c, rounded);
}

GC_FN uint64_t gc_flush_denormal(const GcCpu *c, uint64_t bits) {
  if (!gc_non_ieee(c)) return bits;
  if ((bits & 0x7ff0000000000000ull) == 0 && (bits & 0x000fffffffffffffull) != 0) {
    return bits & GC_SIGN_BIT;
  }
  return bits;
}

/* ------------------------------------------------------------------ */
/* NaN propagation                                                     */

/* Which NaN comes out is decided by position, not by kind: the first NaN in
 * operand order wins, quieted if it was signalling, and a signalling NaN
 * anywhere raises VXSNAN. A quiet frA beats a signalling frB
 * (hwtests/cputest/nan.cpp) - which is the opposite of the "signalling
 * first" rule it is easy to assume.
 *
 * The order is frA, frB, frC even for multiply-add, whose arithmetic is
 * frA * frC + frB. */
GC_FN int gc_nan_result(GcCpu *c, uint64_t a, uint64_t b, uint64_t cc, int count,
                        uint64_t *result) {
  const uint64_t operands[3] = {a, b, cc};
  int i;
  for (i = 0; i < count; i++) {
    if (gc_is_snan(operands[i])) {
      gc_raise_fpscr(c, GC_FPSCR_VXSNAN);
      break;
    }
  }
  for (i = 0; i < count; i++) {
    if (gc_is_nan(operands[i])) {
      *result = gc_quiet(operands[i]);
      return 1;
    }
  }
  return 0;
}

GC_FN uint64_t gc_invalid(GcCpu *c, uint32_t which) {
  /* Where a NaN is born: an operation on numbers that has no answer. A
   * run can ask to be told (gc.h, gc_fp_invalid_trace). */
  if (gc_fp_invalid_trace) gc_fp_invalid_report(c, which);
  gc_raise_fpscr(c, which);
  return GC_QNAN_DEFAULT;
}

/* ------------------------------------------------------------------ */
/* Register access                                                     */

GC_FN uint64_t gc_ps0(const GcCpu *c, unsigned reg) { return c->fpr[reg].ps0; }
GC_FN uint64_t gc_ps1(const GcCpu *c, unsigned reg) { return c->fpr[reg].ps1; }

/* CR1 is a copy of the FPSCR summary bits, written by the Rc form of every
 * floating-point instruction. */
GC_FN void gc_fp_rc(GcCpu *c, uint32_t insn) {
  if (GC_M_Rc(insn)) {
    gc_fp_settle(c);
    gc_cr_set_field(c, 1, (c->fpscr >> 28) & 0xfu);
  }
}

GC_FN void gc_write_frd(GcCpu *c, uint32_t insn, uint64_t ps0, uint64_t ps1) {
  c->fpr[GC_F_frD(insn)].ps0 = ps0;
  c->fpr[GC_F_frD(insn)].ps1 = ps1;
}

/* A double-precision result goes to ps0; ps1 keeps whatever it held. */
GC_FN void gc_write_frd_scalar(GcCpu *c, uint32_t insn, uint64_t ps0) {
  c->fpr[GC_F_frD(insn)].ps0 = ps0;
  gc_set_fprf(c, ps0);
  gc_fp_rc(c, insn);
}

/* A single-precision result goes to both lanes once paired singles are
 * enabled. The manual is explicit: "In paired single mode, all the
 * single-precision floating-point instructions (fadds, fsubs, fmuls, fdivs,
 * fmadds, fmsubs, fnmadds, fnmsubs, fres, frsp) are valid, and operate on
 * the ps0 operand ... The ps1 value in the destination register is
 * duplicated from the ps0 result." Code that then reads ps1 with a paired
 * instruction sees the same value, and missing this leaves stale data in
 * half of every vector a game computes. */
GC_FN void gc_write_frd_single(GcCpu *c, uint32_t insn, uint64_t ps0) {
  c->fpr[GC_F_frD(insn)].ps0 = ps0;
  if (c->hid2 & GC_HID2_PSE) c->fpr[GC_F_frD(insn)].ps1 = ps0;
  gc_set_fprf_single(c, ps0);
  gc_fp_rc(c, insn);
}

/* ------------------------------------------------------------------ */
/* The arithmetic kernels, shared by the scalar and paired forms        */

typedef enum GcFpKind { GC_FP_ADD, GC_FP_SUB, GC_FP_MUL, GC_FP_DIV } GcFpKind;

GC_FP_SLOW uint64_t gc_fp_binary_full(GcCpu *c, GcFpKind kind, uint64_t a, uint64_t b, int single) {
  uint64_t nan;
  double x, y, result;
  int previous, raised;

  if (gc_nan_result(c, a, b, 0, 2, &nan)) return nan;

  x = gc_f64(a);
  y = gc_f64(b);

  /* The invalid cases the architecture names, checked before the host gets
   * a chance to produce its own NaN. */
  switch (kind) {
    case GC_FP_ADD:
      if (gc_isinf(x) && gc_isinf(y) && ((a ^ b) & GC_SIGN_BIT)) return gc_invalid(c, GC_FPSCR_VXISI);
      break;
    case GC_FP_SUB:
      if (gc_isinf(x) && gc_isinf(y) && !((a ^ b) & GC_SIGN_BIT)) return gc_invalid(c, GC_FPSCR_VXISI);
      break;
    case GC_FP_MUL:
      if ((gc_isinf(x) && y == 0.0) || (x == 0.0 && gc_isinf(y))) return gc_invalid(c, GC_FPSCR_VXIMZ);
      if (single) y = gc_f64(gc_force_25bit(b));
      break;
    case GC_FP_DIV:
      if (gc_isinf(x) && gc_isinf(y)) return gc_invalid(c, GC_FPSCR_VXIDI);
      if (x == 0.0 && y == 0.0) return gc_invalid(c, GC_FPSCR_VXZDZ);
      if (y == 0.0) {
        gc_raise_fpscr(c, GC_FPSCR_ZX);
        return ((a ^ b) & GC_SIGN_BIT) ? 0xfff0000000000000ull : 0x7ff0000000000000ull;
      }
      break;
  }

  previous = gc_round_enter(c);
  switch (kind) {
    case GC_FP_ADD: result = x + y; break;
    case GC_FP_SUB: result = x - y; break;
    case GC_FP_MUL: result = x * y; break;
    default: result = x / y; break;
  }
  if (single) result = (double)(float)result;
  raised = gc_round_leave(c, previous);
  (void)raised;
  if (gc_non_ieee(c) && gc_maybe_tiny(result, single)) {
    double toward_zero;
    const int mode = gc_host_getround();
    gc_host_setround(GC_HOST_ROUND_ZERO);
    switch (kind) {
      case GC_FP_ADD: toward_zero = x + y; break;
      case GC_FP_SUB: toward_zero = x - y; break;
      case GC_FP_MUL: toward_zero = x * y; break;
      default: toward_zero = x / y; break;
    }
    if (single) toward_zero = (double)(float)toward_zero;
    gc_host_setround(mode);
    if (gc_tiny(toward_zero, single)) return gc_f64_bits(result) & GC_SIGN_BIT;
  }
  if (single) return gc_flush_single_denormal(c, gc_f64_bits(result));
  return gc_flush_denormal(c, gc_f64_bits(result));
}

/* Multiply-add is fused: the product is not rounded before the add. Doing
 * it any other way changes results by an ulp all over a game's physics. */
GC_FP_SLOW uint64_t gc_fp_madd_full(GcCpu *c, uint64_t a, uint64_t b, uint64_t cc, int negate_add,
                               int negate_result, int single) {
  uint64_t nan;
  double x, y, z, result;
  int previous, raised;

  if (gc_nan_result(c, a, b, cc, 3, &nan)) return nan;

  x = gc_f64(a);                                        /* frA */
  y = gc_f64(single ? gc_force_25bit(cc) : cc);         /* frC, the multiplicand */
  z = gc_f64(b);                                        /* frB, the addend */

  if ((gc_isinf(x) && y == 0.0) || (x == 0.0 && gc_isinf(y))) return gc_invalid(c, GC_FPSCR_VXIMZ);
  if (negate_add) z = -z;
  if (gc_isinf(x * y) && gc_isinf(z) && ((gc_f64_bits(x * y) ^ gc_f64_bits(z)) & GC_SIGN_BIT)) {
    return gc_invalid(c, GC_FPSCR_VXISI);
  }

  previous = gc_round_enter(c);
  result = fma(x, y, z);
  if (single) {
    /* Hardware rounds the product-sum straight to single. Rounding the
     * double result a second time gives a different answer when the double
     * lands exactly halfway between two singles and the true value did not:
     * round-to-nearest-even then breaks the tie the wrong way. Recovering
     * the exact residue and nudging the double one ulp toward it puts the
     * single rounding back on the right side. Dolphin found this in a
     * shipped game; it is the sort of one-ulp difference that sends a
     * recompiled physics step somewhere else entirely. */
    const uint64_t bits = gc_f64_bits(result);
    if ((bits & 0x000000001fffffffull) == 0x0000000010000000ull) {
      const double product = x * y;
      const double product_error = fma(x, y, -product);
      const double sum = product + z;
      const double split = sum - product;
      const double sum_error = (product - (sum - split)) + (z - split);
      const double residue = ((sum - result) + sum_error) + product_error;
      if (residue != 0.0) {
        const uint64_t nudged =
            ((residue > 0.0) == ((bits & GC_SIGN_BIT) == 0)) ? bits + 1u : bits - 1u;
        result = gc_f64(nudged);
      }
    }
    result = (double)(float)result;
  }
  raised = gc_round_leave(c, previous);
  (void)raised;
  if (negate_result) result = -result;
  if (gc_non_ieee(c) && gc_maybe_tiny(result, single)) {
    double toward_zero;
    const int mode = gc_host_getround();
    gc_host_setround(GC_HOST_ROUND_ZERO);
    toward_zero = fma(x, y, z);
    if (single) toward_zero = (double)(float)toward_zero;
    gc_host_setround(mode);
    if (gc_tiny(toward_zero, single)) return gc_f64_bits(result) & GC_SIGN_BIT;
  }
  if (single) return gc_flush_single_denormal(c, gc_f64_bits(result));
  return gc_flush_denormal(c, gc_f64_bits(result));
}


/* ------------------------------------------------------------------ */
/* The same arithmetic without the host's flags, where they can be known */

/* gc_fp_binary_full and gc_fp_madd_full ask the host for the exception
 * flags: clear them, compute, read them back. For the single-precision
 * add, subtract, multiply and multiply-add a game spends its time in, the
 * flags can instead be worked out from the numbers, which is several times
 * cheaper - and gives the same answer, the same FPSCR and the same result,
 * on every input it accepts. Everything else goes the full way.
 *
 * What it accepts, and why each condition is there:
 *
 *  - Round to nearest, in the guest (FPSCR[RN] = 0) and in the host (the
 *    rounding field of MXCSR): the full path computes in the guest's mode,
 *    this one in the host's, so they must be the same mode.
 *  - Every operand zero, or of magnitude between 2^-255 and 2^256
 *    (biased exponent 0x300 to 0x4ff). No NaN or infinity, so none of the
 *    invalid cases; and no intermediate below can overflow or lose bits to
 *    underflow, which the exactness arguments need.
 *  - For a multiply, frA with its low 26 significand bits clear. frC has
 *    been rounded to 25 bits (26 significant), so the product has at most
 *    53 significant bits and the double product is exact.
 *  - The result, rounded to single, zero from an exactly zero double, or
 *    between twice the smallest normal single and the largest single. So
 *    neither rounding overflowed or underflowed - the host would have
 *    raised neither flag - and non-IEEE mode's tininess test (gc_maybe_tiny)
 *    and denormal flush leave it alone.
 *
 * Then the only flag is inexact, raised by the host exactly when the double
 * operation or the narrowing to single was inexact:
 *
 *  - An add is exact when its rounding error is zero, and the error of
 *    s = x + y is exactly (x - (s - (s - x))) + (y - (s - x)) in round to
 *    nearest without overflow (Knuth's TwoSum).
 *  - A multiply is exact (above). A multiply-add is the exact product plus
 *    the addend, so it is that add: fma(x, y, z) == p + z, and its error is
 *    TwoSum's. The one case the full path adjusts - a double result exactly
 *    halfway between two singles, nudged towards the exact value - goes
 *    the full way.
 *  - Narrowing to single is exact when it gives back the same double.
 *
 * A zero double result from an add is exact (a sum of doubles that is not
 * zero is at least the smallest denormal), and from a multiply the product
 * is exact anyway. tests/gc/gc_fpu_fast_test.c holds the two paths to each
 * other over edge cases and random operands. */
/* A test counts how often the fast path answers (tests/gc/gc_fpu_fast_test.c). */
#ifndef GC_FPU_FAST_TAKEN
#define GC_FPU_FAST_TAKEN() ((void)0)
#endif

GC_FN int gc_fp_moderate(uint64_t bits) {
  const uint32_t exponent = (uint32_t)(bits >> 52) & 0x7ffu;
  return (exponent >= 0x300u && exponent <= 0x4ffu) || (bits & 0x7fffffffffffffffull) == 0;
}

#if defined(_M_X64) || defined(__x86_64__)
GC_FN int gc_host_nearest(void) { return (_mm_getcsr() & 0x6000u) == 0; }
#else
GC_FN int gc_host_nearest(void) { return fegetround() == FE_TONEAREST; }
#endif

/* The fast path in two halves: a kernel that computes a lane's result and
 * whether it was inexact, touching nothing - no FPSCR, no host state - and
 * gc_fp_fast_flags, which folds the flags in as gc_round_leave would. Since
 * a kernel changes nothing, a caller can try every lane of an instruction
 * first and apply the lanes' flags in order only once all of them were
 * accepted, and otherwise run the instruction the ordinary way, lane by
 * lane, as if the kernels had never been called. That is what lets a paired
 * instruction ask the host's rounding mode once rather than once a lane:
 * neither a kernel nor a full path leaves the host's mode changed.
 *
 * The narrowing of a double s to the single r, with the condition on r
 * described above: 0 when outside it. When s already is a single inside the
 * range - its low 29 bits clear and its exponent from 0x382 (twice the
 * smallest normal single) to 0x47e (the largest single's) - r is s and the
 * narrowing is exact, in any rounding mode, without converting; a zero
 * likewise. Anything else converts, which is what the host would do. */
GC_FN int gc_fp_fast_narrow(double s, int inexact, uint64_t *result, int *inexact_out) {
  const uint64_t bits = gc_f64_bits(s);
  const uint32_t biased = (uint32_t)(bits >> 52) & 0x7ffu;
  if ((bits & ~GC_SIGN_BIT) == 0 ||
      ((bits & 0x000000001fffffffull) == 0 && biased - 0x382u <= 0x47eu - 0x382u)) {
    *result = bits;
    *inexact_out = inexact;
    return 1;
  }
  {
    const double r = (double)(float)s;
    const double magnitude = r < 0.0 ? -r : r;
    /* 0x3820000000000000 is twice the smallest normal single. */
    if (!(magnitude >= gc_f64(0x3820000000000000ull) && magnitude <= 3.4028234663852886e38)) return 0;
    *result = gc_f64_bits(r);
    *inexact_out = inexact || r != s;
    return 1;
  }
}

GC_FN void gc_fp_fast_flags(GcCpu *c, int inexact) {
  gc_fp_settle(c);
  if (inexact) {
    c->fpscr |= GC_FPSCR_FI;
    gc_raise_fpscr(c, GC_FPSCR_XX);
  } else {
    c->fpscr &= ~(GC_FPSCR_FI | GC_FPSCR_FR);
  }
  GC_FPU_FAST_TAKEN();
}

/* Whether the modes allow the fast path: round to nearest in the guest and
 * in the host. */
GC_FN int gc_fp_fast_mode(const GcCpu *c) {
  return (c->fpscr & GC_FPSCR_RN) == 0 && gc_host_nearest();
}

/* a + b, or a - b: TwoSum's error says whether the double sum was exact. */
GC_FN int gc_fp_fast_add(uint64_t a, uint64_t b, int subtract, uint64_t *result, int *inexact) {
  if (!gc_fp_moderate(a) || !gc_fp_moderate(b)) return 0;
  {
    const double x = gc_f64(a);
    const double y = subtract ? -gc_f64(b) : gc_f64(b);
    const double s = x + y;
    const double yv = s - x;
    const double error = (x - (s - yv)) + (y - yv);
    return gc_fp_fast_narrow(s, error != 0.0, result, inexact);
  }
}

/* a * b, with b rounded to 25 bits and a short enough that the product is
 * exact. */
GC_FN int gc_fp_fast_mul(uint64_t a, uint64_t b, uint64_t *result, int *inexact) {
  const uint64_t forced = gc_force_25bit(b);
  if (!gc_fp_moderate(a) || !gc_fp_moderate(b) || (a & 0x0000000003ffffffull) != 0 ||
      !gc_fp_moderate(forced)) {
    return 0;
  }
  return gc_fp_fast_narrow(gc_f64(a) * gc_f64(forced), 0, result, inexact);
}

GC_FN int gc_fp_fast_madd(uint64_t a, uint64_t b, uint64_t cc, int negate_add, int negate_result,
                          uint64_t *result, int *inexact) {
  const uint64_t forced = gc_force_25bit(cc);
  if ((a & 0x0000000003ffffffull) != 0 || !gc_fp_moderate(a) || !gc_fp_moderate(b) ||
      !gc_fp_moderate(cc) || !gc_fp_moderate(forced)) {
    return 0;
  }
  {
    const double x = gc_f64(a);
    const double p = x * gc_f64(forced);
    const double z = negate_add ? -gc_f64(b) : gc_f64(b);
    const double s = p + z;
    const double zv = s - p;
    const double error = (p - (s - zv)) + (z - zv);
    uint64_t r;
    if ((gc_f64_bits(s) & 0x000000001fffffffull) == 0x0000000010000000ull) return 0;
    if (!gc_fp_fast_narrow(s, error != 0.0, &r, inexact)) return 0;
    *result = negate_result ? r ^ GC_SIGN_BIT : r;
    return 1;
  }
}

GC_FN uint64_t gc_fp_binary(GcCpu *c, GcFpKind kind, uint64_t a, uint64_t b, int single) {
  if (single && kind != GC_FP_DIV && gc_fp_fast_mode(c)) {
    uint64_t r;
    int inexact;
    if (kind == GC_FP_MUL ? gc_fp_fast_mul(a, b, &r, &inexact)
                          : gc_fp_fast_add(a, b, kind == GC_FP_SUB, &r, &inexact)) {
      gc_fp_fast_flags(c, inexact);
      return r;
    }
  }
  return gc_fp_binary_full(c, kind, a, b, single);
}

GC_FN uint64_t gc_fp_madd(GcCpu *c, uint64_t a, uint64_t b, uint64_t cc, int negate_add,
                          int negate_result, int single) {
  if (single && gc_fp_fast_mode(c)) {
    uint64_t r;
    int inexact;
    if (gc_fp_fast_madd(a, b, cc, negate_add, negate_result, &r, &inexact)) {
      gc_fp_fast_flags(c, inexact);
      return r;
    }
  }
  return gc_fp_madd_full(c, a, b, cc, negate_add, negate_result, single);
}

/* Both lanes of a paired add, subtract or multiply at once: 1 when both
 * kernels accept, with the results and whether each was inexact, and
 * nothing changed either way. */
GC_FN int gc_ps_fast_binary(const GcCpu *c, GcFpKind kind, uint64_t a0, uint64_t b0, uint64_t a1, uint64_t b1,
                            uint64_t *ps0, uint64_t *ps1, int *inexact0, int *inexact1) {
  if (kind == GC_FP_DIV || !gc_fp_fast_mode(c)) return 0;
  if (kind == GC_FP_MUL) {
    return gc_fp_fast_mul(a0, b0, ps0, inexact0) && gc_fp_fast_mul(a1, b1, ps1, inexact1);
  }
  return gc_fp_fast_add(a0, b0, kind == GC_FP_SUB, ps0, inexact0) &&
         gc_fp_fast_add(a1, b1, kind == GC_FP_SUB, ps1, inexact1);
}

GC_FN int gc_ps_fast_madd(const GcCpu *c, uint64_t a0, uint64_t b0, uint64_t c0, uint64_t a1, uint64_t b1,
                          uint64_t c1, int negate_add, int negate_result, uint64_t *ps0, uint64_t *ps1,
                          int *inexact0, int *inexact1) {
  return gc_fp_fast_mode(c) && gc_fp_fast_madd(a0, b0, c0, negate_add, negate_result, ps0, inexact0) &&
         gc_fp_fast_madd(a1, b1, c1, negate_add, negate_result, ps1, inexact1);
}

/* The rest of a paired arithmetic instruction whose lanes were both fast,
 * in one pass over a copy of FPSCR: what gc_fp_fast_flags for lane 0 and
 * then lane 1, gc_update_vx, the write, gc_set_fprf_single and gc_fp_rc
 * leave, in that order, written out.
 *
 *  - The two lanes' flags. Either inexact sets XX, and FX with it if XX was
 *    clear. FI ends as lane 1's inexact, since lane 1 either sets it or
 *    clears it. FR is cleared by whichever lane was exact, so it survives
 *    only when both were inexact.
 *  - VX and FEX as gc_update_vx derives them.
 *  - FPRF. A fast result is a zero (the narrowing's shortcut) or a normal
 *    single (every other accepted result has a magnitude from twice the
 *    smallest normal single to the largest single), so its class is
 *    0x02 or 0x04, with the sign folded in as 0x12 or 0x08 - which is what
 *    gc_fprf_of_single answers for those.
 *
 * Written without branches: FPSCR is carried from one floating-point
 * instruction to the next, and the loading screen's decoder runs a million
 * of these a field. */
GC_FN void gc_ps_fast_commit(GcCpu *c, uint32_t insn, uint64_t ps0, uint64_t ps1, int inexact0, int inexact1) {
  const uint32_t invalid = GC_FPSCR_VXSNAN | GC_FPSCR_VXISI | GC_FPSCR_VXIDI | GC_FPSCR_VXZDZ |
                           GC_FPSCR_VXIMZ | GC_FPSCR_VXVC | GC_FPSCR_VXSOFT | GC_FPSCR_VXSQRT |
                           GC_FPSCR_VXCVI;
  const uint32_t any = (uint32_t)((inexact0 | inexact1) & 1);
  const uint32_t both = (uint32_t)((inexact0 & inexact1) & 1);
  const uint32_t negative = (uint32_t)(ps0 >> 63);
  const uint32_t zero = (ps0 << 1) == 0 ? 1u : 0u;
  const uint32_t fprf = zero ? (0x02u | (negative << 4)) : (0x04u << negative);
  uint32_t f;
  gc_fp_settle(c);
  f = c->fpscr;
  f |= (any & ~(f >> 25)) << 31; /* FX, when XX is newly set */
  f |= any << 25;                /* XX */
  f &= ~(GC_FPSCR_FI | ((1u - both) << 18));
  f |= ((uint32_t)inexact1 & 1u) << 17;
  f = (f & ~GC_FPSCR_VX) | ((f & invalid) ? GC_FPSCR_VX : 0u);
  f = (f & ~GC_FPSCR_FEX) | (((f >> 22) & f & 0xf8u) ? GC_FPSCR_FEX : 0u);
  f = (f & ~GC_FPSCR_FPRF) | (fprf << 12);
  c->fpscr = f;
  c->fpr[GC_F_frD(insn)].ps0 = ps0;
  c->fpr[GC_F_frD(insn)].ps1 = ps1;
  gc_fp_rc(c, insn);
  GC_FPU_FAST_TAKEN();
  GC_FPU_FAST_TAKEN();
}

/* When the lanes cannot both be fast: the whole instruction as it always
 * was, lane 0 and then lane 1, each still trying its own fast path. One
 * shared copy a file, like the full paths, so that every paired instruction
 * does not carry the arithmetic three times over. */
GC_FP_SLOW void gc_ps_binary_slow(GcCpu *c, uint32_t insn, GcFpKind kind, uint64_t a0, uint64_t b0, uint64_t a1,
                                  uint64_t b1) {
  const uint64_t ps0 = gc_fp_binary(c, kind, a0, b0, 1);
  const uint64_t ps1 = gc_fp_binary(c, kind, a1, b1, 1);
  gc_update_vx(c);
  gc_write_frd(c, insn, ps0, ps1);
  gc_set_fprf_single(c, ps0);
  gc_fp_rc(c, insn);
}

GC_FP_SLOW void gc_ps_madd_slow(GcCpu *c, uint32_t insn, uint64_t a0, uint64_t b0, uint64_t c0, uint64_t a1,
                                uint64_t b1, uint64_t c1, int negate_add, int negate_result) {
  const uint64_t ps0 = gc_fp_madd(c, a0, b0, c0, negate_add, negate_result, 1);
  const uint64_t ps1 = gc_fp_madd(c, a1, b1, c1, negate_add, negate_result, 1);
  gc_update_vx(c);
  gc_write_frd(c, insn, ps0, ps1);
  gc_set_fprf_single(c, ps0);
  gc_fp_rc(c, insn);
}

/* FPSCR, a little behind.
 *
 * A paired add, subtract, multiply or multiply-add on the fast path writes
 * FPSCR's inexact bits, its summary bits and FPRF, and the inexact bits
 * need TwoSum's error term: arithmetic that, in a decoder's inner loop,
 * costs more than the result it describes - and the next instruction
 * overwrites all of it. So in the steady state a running game sits in,
 * the instruction leaves a note instead, and FPSCR is brought up to date
 * from the note by whatever reads or changes it next (gc_fp_settle).
 *
 * The steady state is FX and XX already set, FR clear, no floating-point
 * exception enabled in FPSCR (VE, OE, UE, ZE, XE) or MSR (FE0, FE1), and no
 * Rc. Then what gc_ps_fast_commit would do comes down to:
 *
 *  - XX stays set and FX with it: they are set already.
 *  - FR stays clear: it is cleared unless both lanes were inexact, and it
 *    was clear.
 *  - FI becomes lane 1's inexact.
 *  - VX and FEX are derived from the rest, and nothing above changed the
 *    bits they are derived from, so deriving them later gives the same.
 *  - FPRF becomes the class of ps0.
 *
 * None of that depends on FPSCR's other bits, so the note holds lane 1's
 * operands (to recompute its inexact bit with the same kernel, in round to
 * nearest as when it was taken) and ps0. A second instruction in the steady
 * state before anything reads FPSCR replaces the note: its FI and FPRF
 * overwrite the first's, and deriving VX and FEX twice is deriving them
 * once. The steady state's own bits are not among those an instruction
 * leaves behind, so a pending note does not change whether the next
 * instruction is in it.
 *
 * Everything that reads or changes FPSCR beyond RN and NI settles first:
 * the helpers every handler goes through (gc_set_fprf, gc_raise_fpscr,
 * gc_update_vx, gc_fpscr_write, gc_round_enter, gc_fp_rc, the fast path's
 * flags and commit), the compares, mffs, mcrfs and the conversions, and
 * outside this file the differential comparison, the state dump and the
 * tests. Settling happens before an instruction computes, never between
 * its arithmetic and its reading of the host's flags. */
#define GC_FP_LAZY_STEADY_MASK                                                          \
  (GC_FPSCR_FX | GC_FPSCR_XX | GC_FPSCR_FR | GC_FPSCR_VE | GC_FPSCR_OE | GC_FPSCR_UE | \
   GC_FPSCR_ZE | GC_FPSCR_XE)
#define GC_FP_LAZY_ADD 1u
#define GC_FP_LAZY_SUB 2u
#define GC_FP_LAZY_MUL 3u
#define GC_FP_LAZY_MADD 4u /* plus 2 for negate_add, plus 1 for negate_result */

GC_FN int gc_fp_lazy_ok(const GcCpu *c, uint32_t insn) {
  return !GC_M_Rc(insn) && (c->fpscr & GC_FP_LAZY_STEADY_MASK) == (GC_FPSCR_FX | GC_FPSCR_XX) &&
         (c->msr & (GC_MSR_FE0 | GC_MSR_FE1)) == 0;
}

GC_FN void gc_fp_lazy_note(GcCpu *c, uint32_t insn, unsigned kind, uint64_t a1, uint64_t b1, uint64_t c1,
                           uint64_t ps0, uint64_t ps1) {
  c->fp_lazy = (uint8_t)kind;
  c->fp_lazy_a = a1;
  c->fp_lazy_b = b1;
  c->fp_lazy_c = c1;
  c->fp_lazy_result = ps0;
  c->fpr[GC_F_frD(insn)].ps0 = ps0;
  c->fpr[GC_F_frD(insn)].ps1 = ps1;
  GC_FPU_FAST_TAKEN();
  GC_FPU_FAST_TAKEN();
}

GC_FP_SLOW void gc_fp_settle_slow(GcCpu *c) {
  const unsigned kind = c->fp_lazy;
  const int mode = gc_host_getround();
  const uint64_t ps0 = c->fp_lazy_result;
  const uint32_t negative = (uint32_t)(ps0 >> 63);
  const uint32_t fprf = (ps0 << 1) == 0 ? (0x02u | (negative << 4)) : (0x04u << negative);
  uint64_t r;
  int inexact = 0;
  c->fp_lazy = 0;
  if (mode != GC_HOST_ROUND_NEAREST) gc_host_setround(GC_HOST_ROUND_NEAREST);
  if (kind == GC_FP_LAZY_ADD || kind == GC_FP_LAZY_SUB) {
    (void)gc_fp_fast_add(c->fp_lazy_a, c->fp_lazy_b, kind == GC_FP_LAZY_SUB, &r, &inexact);
  } else if (kind == GC_FP_LAZY_MUL) {
    (void)gc_fp_fast_mul(c->fp_lazy_a, c->fp_lazy_b, &r, &inexact);
  } else {
    (void)gc_fp_fast_madd(c->fp_lazy_a, c->fp_lazy_b, c->fp_lazy_c, (kind - GC_FP_LAZY_MADD) >> 1 & 1u,
                          (kind - GC_FP_LAZY_MADD) & 1u, &r, &inexact);
  }
  if (mode != GC_HOST_ROUND_NEAREST) gc_host_setround(mode);
  c->fpscr = (c->fpscr & ~(GC_FPSCR_FI | GC_FPSCR_FPRF)) | (inexact ? GC_FPSCR_FI : 0u) | (fprf << 12);
  gc_update_vx(c);
}

/* The steady state's whole instruction, as a step of its own: both lanes on
 * the fast path and a note left, or 0 having changed nothing. This is the
 * first thing the paired handlers below try - it gives what the fast path
 * followed by gc_fp_lazy_ok gave, because the kernels are pure and the
 * steady state's test does not depend on them - and recompiled code tries it
 * inline before calling the handler. The inexactness the kernels also
 * compute is not needed here (settling recomputes lane 1's), and an
 * optimiser that inlines this drops it. */
#if defined(_M_X64) || defined(__x86_64__)
/* The same tests on both lanes at once, in SSE2.
 *
 * gc_sse_moderate: which of two 64-bit lanes gc_fp_moderate accepts (bit 0
 * for the low lane, bit 1 for the high one) - an exponent from 0x300 to
 * 0x4ff, or a zero - from the exponent and the two halves, 32 bits at a
 * time. gc_sse_narrow: gc_fp_fast_narrow on both lanes, the result always
 * the converted one. That is the same result: where gc_fp_fast_narrow takes
 * its shortcut (a zero, or a single already inside the range) the conversion
 * gives back the same bits, and where it converts it is the same conversion
 * under the same rounding mode. It accepts a lane whose converted magnitude
 * lies from twice the smallest normal single to the largest single, or whose
 * double was a zero: the shortcut's singles are all inside that range, and a
 * NaN compares false. The host's sticky flags can differ from the scalar
 * kernel's (a shortcut converts nothing), which nothing reads: every full
 * path clears them first (gc_host_enter). */
static inline int gc_sse_moderate(__m128i v) {
  const __m128i hi = _mm_shuffle_epi32(_mm_srli_epi64(v, 32), _MM_SHUFFLE(2, 0, 2, 0));
  const __m128i lo = _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 0, 2, 0));
  const __m128i exponent = _mm_and_si128(_mm_srli_epi32(hi, 20), _mm_set1_epi32(0x7ff));
  const __m128i t = _mm_sub_epi32(exponent, _mm_set1_epi32(0x300));
  const __m128i outside = _mm_or_si128(_mm_cmpgt_epi32(t, _mm_set1_epi32(0x1ff)), _mm_cmplt_epi32(t, _mm_setzero_si128()));
  const __m128i zero = _mm_and_si128(_mm_cmpeq_epi32(_mm_and_si128(hi, _mm_set1_epi32(0x7fffffff)), _mm_setzero_si128()),
                                     _mm_cmpeq_epi32(lo, _mm_setzero_si128()));
  const __m128i ok = _mm_or_si128(_mm_andnot_si128(outside, _mm_set1_epi32(-1)), zero);
  return _mm_movemask_ps(_mm_castsi128_ps(ok)) & 3;
}

static inline int gc_sse_narrow(__m128d s, __m128d *out) {
  const __m128d r = _mm_cvtps_pd(_mm_cvtpd_ps(s));
  const __m128d magnitude = _mm_and_pd(r, _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffll)));
  const __m128d inside = _mm_and_pd(_mm_cmpge_pd(magnitude, _mm_castsi128_pd(_mm_set1_epi64x(0x3820000000000000ll))),
                                    _mm_cmple_pd(magnitude, _mm_set1_pd(3.4028234663852886e38)));
  const __m128d zero = _mm_cmpeq_pd(s, _mm_setzero_pd());
  *out = r;
  return _mm_movemask_pd(_mm_or_pd(inside, zero));
}

/* frC rounded to 25 bits, both lanes (gc_force_25bit), and whether a lane of
 * frA has its low 26 significand bits clear. */
static inline __m128i gc_sse_force_25bit(__m128i v) {
  return _mm_add_epi64(_mm_and_si128(v, _mm_set1_epi64x((long long)0xfffffffff8000000ull)),
                       _mm_and_si128(v, _mm_set1_epi64x(0x0000000008000000ll)));
}

static inline int gc_sse_short(__m128i v) {
  const __m128i lo = _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 0, 2, 0));
  return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(lo, _mm_set1_epi32(0x3ffffff)),
                                                          _mm_setzero_si128()))) & 3;
}
#endif

#if defined(_M_X64) || defined(__x86_64__)
/* gc_fp_fast_add and gc_fp_fast_mul on both lanes (see gc_sse_narrow): 1 with
 * the result in *out when both lanes are accepted. */
static inline int gc_sse_ps_binary(__m128i a, __m128i b, GcFpKind kind, __m128d *out) {
  __m128d s;
  if ((gc_sse_moderate(a) & gc_sse_moderate(b)) != 3) return 0;
  if (kind == GC_FP_MUL) {
    const __m128i forced = gc_sse_force_25bit(b);
    if ((gc_sse_short(a) & gc_sse_moderate(forced)) != 3) return 0;
    s = _mm_mul_pd(_mm_castsi128_pd(a), _mm_castsi128_pd(forced));
  } else if (kind == GC_FP_SUB) {
    s = _mm_sub_pd(_mm_castsi128_pd(a), _mm_castsi128_pd(b));
  } else {
    s = _mm_add_pd(_mm_castsi128_pd(a), _mm_castsi128_pd(b));
  }
  return gc_sse_narrow(s, out) == 3;
}

/* gc_fp_fast_madd on both lanes: the product of frA and frC rounded to 25
 * bits, frB added (or subtracted), a result exactly halfway between two
 * singles declined, narrowed, and negated when asked. */
static inline int gc_sse_ps_madd(__m128i a, __m128i b, __m128i cc, int negate_add, int negate_result, __m128d *out) {
  const __m128i forced = gc_sse_force_25bit(cc);
  __m128d p, s;
  if ((gc_sse_short(a) & gc_sse_moderate(a) & gc_sse_moderate(b) & gc_sse_moderate(cc) & gc_sse_moderate(forced)) != 3) {
    return 0;
  }
  p = _mm_mul_pd(_mm_castsi128_pd(a), _mm_castsi128_pd(forced));
  s = negate_add ? _mm_add_pd(p, _mm_xor_pd(_mm_castsi128_pd(b), _mm_set1_pd(-0.0))) : _mm_add_pd(p, _mm_castsi128_pd(b));
  {
    const __m128i low = _mm_shuffle_epi32(_mm_castpd_si128(s), _MM_SHUFFLE(2, 0, 2, 0));
    const __m128i half = _mm_cmpeq_epi32(_mm_and_si128(low, _mm_set1_epi32(0x1fffffff)), _mm_set1_epi32(0x10000000));
    if (_mm_movemask_ps(_mm_castsi128_ps(half)) & 3) return 0;
  }
  if (gc_sse_narrow(s, out) != 3) return 0;
  if (negate_result) *out = _mm_xor_pd(*out, _mm_set1_pd(-0.0));
  return 1;
}
#endif

GC_FN int gc_ps_binary_lazy(GcCpu *c, uint32_t insn, GcFpKind kind, uint64_t a0, uint64_t b0, uint64_t a1,
                            uint64_t b1) {
  uint64_t ps0, ps1;
  if (kind == GC_FP_DIV || !gc_fp_lazy_ok(c, insn) || !gc_fp_fast_mode(c)) return 0;
#if defined(_M_X64) || defined(__x86_64__)
  {
    __m128d r;
    uint64_t lanes[2];
    if (!gc_sse_ps_binary(_mm_set_epi64x((long long)a1, (long long)a0), _mm_set_epi64x((long long)b1, (long long)b0),
                          kind, &r)) {
      return 0;
    }
    _mm_storeu_si128((__m128i *)(void *)lanes, _mm_castpd_si128(r));
    ps0 = lanes[0];
    ps1 = lanes[1];
  }
#else
  {
    int unused0, unused1;
    if (kind == GC_FP_MUL) {
      if (!gc_fp_fast_mul(a0, b0, &ps0, &unused0) || !gc_fp_fast_mul(a1, b1, &ps1, &unused1)) return 0;
    } else {
      if (!gc_fp_fast_add(a0, b0, kind == GC_FP_SUB, &ps0, &unused0) ||
          !gc_fp_fast_add(a1, b1, kind == GC_FP_SUB, &ps1, &unused1)) {
        return 0;
      }
    }
  }
#endif
  gc_fp_lazy_note(c, insn, GC_FP_LAZY_ADD + (unsigned)kind, a1, b1, 0, ps0, ps1);
  return 1;
}

GC_FN int gc_ps_madd_lazy(GcCpu *c, uint32_t insn, uint64_t a0, uint64_t b0, uint64_t c0, uint64_t a1, uint64_t b1,
                          uint64_t c1, int negate_add, int negate_result) {
  uint64_t ps0, ps1;
  if (!gc_fp_lazy_ok(c, insn) || !gc_fp_fast_mode(c)) return 0;
#if defined(_M_X64) || defined(__x86_64__)
  {
    __m128d r;
    uint64_t lanes[2];
    if (!gc_sse_ps_madd(_mm_set_epi64x((long long)a1, (long long)a0), _mm_set_epi64x((long long)b1, (long long)b0),
                        _mm_set_epi64x((long long)c1, (long long)c0), negate_add, negate_result, &r)) {
      return 0;
    }
    _mm_storeu_si128((__m128i *)(void *)lanes, _mm_castpd_si128(r));
    ps0 = lanes[0];
    ps1 = lanes[1];
  }
#else
  {
    int unused0, unused1;
    if (!gc_fp_fast_madd(a0, b0, c0, negate_add, negate_result, &ps0, &unused0) ||
        !gc_fp_fast_madd(a1, b1, c1, negate_add, negate_result, &ps1, &unused1)) {
      return 0;
    }
  }
#endif
  gc_fp_lazy_note(c, insn, GC_FP_LAZY_MADD + 2u * (unsigned)negate_add + (unsigned)negate_result, a1, b1, c1, ps0,
                  ps1);
  return 1;
}

/* A paired instruction whose lanes were both fast, outside the steady
 * state: the flags computed and committed now. One shared copy a file. */
GC_FP_SLOW void gc_ps_binary_eager(GcCpu *c, uint32_t insn, GcFpKind kind, uint64_t a0, uint64_t b0, uint64_t a1,
                                   uint64_t b1) {
  uint64_t ps0, ps1;
  int inexact0, inexact1;
  (void)gc_ps_fast_binary(c, kind, a0, b0, a1, b1, &ps0, &ps1, &inexact0, &inexact1);
  gc_ps_fast_commit(c, insn, ps0, ps1, inexact0, inexact1);
}

GC_FP_SLOW void gc_ps_madd_eager(GcCpu *c, uint32_t insn, uint64_t a0, uint64_t b0, uint64_t c0, uint64_t a1,
                                 uint64_t b1, uint64_t c1, int negate_add, int negate_result) {
  uint64_t ps0, ps1;
  int inexact0, inexact1;
  (void)gc_ps_fast_madd(c, a0, b0, c0, a1, b1, c1, negate_add, negate_result, &ps0, &ps1, &inexact0, &inexact1);
  gc_ps_fast_commit(c, insn, ps0, ps1, inexact0, inexact1);
}

/* ------------------------------------------------------------------ */
/* Scalar arithmetic                                                   */

/* Which writer an arithmetic form uses depends on whether it is a
 * single-precision instruction, which is a compile-time constant in both
 * the interpreter and generated code. */
#define GC_WRITE_RESULT(single, c, insn, value)      \
  ((single) ? gc_write_frd_single((c), (insn), (value)) \
            : gc_write_frd_scalar((c), (insn), (value)))

#define GC_FP_BINOP(name, kind, single)                                            \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                    \
    const uint64_t result = gc_fp_binary(c, kind, gc_ps0(c, GC_F_frA(insn)),       \
                                         gc_ps0(c, GC_F_frB(insn)), single);       \
    gc_update_vx(c);                                                               \
    GC_WRITE_RESULT(single, c, insn, result);                                         \
  }

GC_FP_BINOP(fadd, GC_FP_ADD, 0)
GC_FP_BINOP(fsub, GC_FP_SUB, 0)
GC_FP_BINOP(fdiv, GC_FP_DIV, 0)
GC_FP_BINOP(fadds, GC_FP_ADD, 1)
GC_FP_BINOP(fsubs, GC_FP_SUB, 1)
GC_FP_BINOP(fdivs, GC_FP_DIV, 1)

/* The multiply forms take their second operand from frC, not frB. */
#define GC_FP_MULOP(name, single)                                                  \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                    \
    const uint64_t result = gc_fp_binary(c, GC_FP_MUL, gc_ps0(c, GC_F_frA(insn)),  \
                                         gc_ps0(c, GC_F_frC(insn)), single);       \
    gc_update_vx(c);                                                               \
    GC_WRITE_RESULT(single, c, insn, result);                                         \
  }

GC_FP_MULOP(fmul, 0)
GC_FP_MULOP(fmuls, 1)

#define GC_FP_MADDOP(name, negate_add, negate_result, single)                      \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                    \
    const uint64_t result =                                                        \
        gc_fp_madd(c, gc_ps0(c, GC_F_frA(insn)), gc_ps0(c, GC_F_frB(insn)),        \
                   gc_ps0(c, GC_F_frC(insn)), negate_add, negate_result, single);  \
    gc_update_vx(c);                                                               \
    GC_WRITE_RESULT(single, c, insn, result);                                         \
  }

GC_FP_MADDOP(fmadd, 0, 0, 0)
GC_FP_MADDOP(fmsub, 1, 0, 0)
GC_FP_MADDOP(fnmadd, 0, 1, 0)
GC_FP_MADDOP(fnmsub, 1, 1, 0)
GC_FP_MADDOP(fmadds, 0, 0, 1)
GC_FP_MADDOP(fmsubs, 1, 0, 1)
GC_FP_MADDOP(fnmadds, 0, 1, 1)
GC_FP_MADDOP(fnmsubs, 1, 1, 1)

/* The sign-manipulating moves are bit operations: they do not look at the
 * value, do not raise anything and do not touch FPRF. */
GC_OP gc_op_fmr(GcCpu *c, uint32_t insn) {
  c->fpr[GC_F_frD(insn)].ps0 = gc_ps0(c, GC_F_frB(insn));
  gc_fp_rc(c, insn);
}
GC_OP gc_op_fneg(GcCpu *c, uint32_t insn) {
  c->fpr[GC_F_frD(insn)].ps0 = gc_ps0(c, GC_F_frB(insn)) ^ GC_SIGN_BIT;
  gc_fp_rc(c, insn);
}
GC_OP gc_op_fabs(GcCpu *c, uint32_t insn) {
  c->fpr[GC_F_frD(insn)].ps0 = gc_ps0(c, GC_F_frB(insn)) & ~GC_SIGN_BIT;
  gc_fp_rc(c, insn);
}
GC_OP gc_op_fnabs(GcCpu *c, uint32_t insn) {
  c->fpr[GC_F_frD(insn)].ps0 = gc_ps0(c, GC_F_frB(insn)) | GC_SIGN_BIT;
  gc_fp_rc(c, insn);
}

/* frsp rounds a double to single precision and leaves it in double format.
 * It is the instruction a compiler emits before storing a float, so its
 * rounding is visible in every saved game and every sent packet. */
GC_OP gc_op_frsp(GcCpu *c, uint32_t insn) {
  const uint64_t b = gc_ps0(c, GC_F_frB(insn));
  uint64_t result;
  if (gc_nan_result(c, b, 0, 0, 1, &result)) {
    /* The result is a single, so only the mantissa bits a single has
     * survive - a NaN included. */
    result &= ~0x000000001fffffffull;
    gc_update_vx(c);
  } else {
    const int previous = gc_round_enter(c);
    result = gc_f64_bits((double)(float)gc_f64(b));
    (void)gc_round_leave(c, previous);
    /* The operand is the exact value, so tininess needs no recomputation. */
    if (gc_non_ieee(c) && gc_tiny(gc_f64(b), 1)) {
      result &= GC_SIGN_BIT;
    } else {
      result = gc_flush_single_denormal(c, result);
    }
  }
  gc_write_frd_single(c, insn, result);
}

/* Converting to an integer: out-of-range and NaN operands give the
 * saturated value and set VXCVI, and the high word of the result is not
 * architecturally defined - the 750 leaves 0xfff80000 there, which games
 * have been seen to depend on (hwtests/cputest/fctiw.cpp). */
GC_FN uint64_t gc_convert_to_int(GcCpu *c, uint64_t b, int round_to_zero) {
  const double value = gc_f64(b);
  int32_t result;
  gc_fp_settle(c);
  if (gc_is_nan(b)) {
    if (gc_is_snan(b)) gc_raise_fpscr(c, GC_FPSCR_VXSNAN);
    gc_raise_fpscr(c, GC_FPSCR_VXCVI);
    result = (int32_t)0x80000000;
  } else if (value > 2147483647.0) {
    gc_raise_fpscr(c, GC_FPSCR_VXCVI);
    result = 2147483647;
  } else if (value < -2147483648.0) {
    gc_raise_fpscr(c, GC_FPSCR_VXCVI);
    result = (int32_t)0x80000000;
  } else {
    const int previous = round_to_zero ? gc_host_getround() : gc_round_enter(c);
    if (round_to_zero) {
      result = (int32_t)trunc(value);
    } else {
      result = (int32_t)nearbyint(value);
      gc_round_leave(c, previous);
    }
  }
  gc_update_vx(c);
  if (!gc_is_nan(b) && (double)result != gc_f64(b)) {
    /* The conversion dropped a fraction: that is the inexact exception. */
    gc_raise_fpscr(c, GC_FPSCR_XX);
    c->fpscr |= GC_FPSCR_FI;
  }
  /* The high word is architecturally undefined. The hardware leaves
   * 0xfff80000 there, except that a zero result from a negative operand
   * carries the sign into bit 32 - a quirk hwtests/cputest/fctiw.cpp
   * found, and one the vectors show on every negative operand that
   * truncates to zero. */
  return 0xfff8000000000000ull | ((result == 0 && (b & GC_SIGN_BIT)) ? 0x100000000ull : 0ull) |
         (uint32_t)result;
}

GC_OP gc_op_fctiw(GcCpu *c, uint32_t insn) {
  c->fpr[GC_F_frD(insn)].ps0 = gc_convert_to_int(c, gc_ps0(c, GC_F_frB(insn)), 0);
  gc_fp_rc(c, insn);
}

GC_OP gc_op_fctiwz(GcCpu *c, uint32_t insn) {
  c->fpr[GC_F_frD(insn)].ps0 = gc_convert_to_int(c, gc_ps0(c, GC_F_frB(insn)), 1);
  gc_fp_rc(c, insn);
}

/* fsel is a branchless select on "greater than or equal to zero", and -0.0
 * counts as greater than or equal. It is how the SDK avoids branches, so it
 * is everywhere in game code. */
GC_OP gc_op_fsel(GcCpu *c, uint32_t insn) {
  const uint64_t a = gc_ps0(c, GC_F_frA(insn));
  const int take_c = !gc_is_nan(a) && gc_f64(a) >= 0.0;
  c->fpr[GC_F_frD(insn)].ps0 = take_c ? gc_ps0(c, GC_F_frC(insn)) : gc_ps0(c, GC_F_frB(insn));
  gc_fp_rc(c, insn);
}

/* fres and frsqrte do not compute; they look up. The tables in
 * gc_estimate_tables.h are measured, because the manual declines to print
 * them ("the value placed into register frD may vary between
 * implementations"), and a game that iterates on the estimate converges
 * either way - but one that stores the raw estimate does not.
 *
 * The result is a single: a segment of the operand's mantissa chooses a
 * line, and the bits below it interpolate along it. */
GC_FN uint64_t gc_estimate_mantissa(const GcEstimateTable *table, uint64_t mantissa) {
  const unsigned segment = (unsigned)(mantissa >> GC_ESTIMATE_SEGMENT_SHIFT);
  const uint64_t low = mantissa & ((1ull << GC_ESTIMATE_SEGMENT_SHIFT) - 1);
  const uint32_t index = (uint32_t)(low >> GC_ESTIMATE_INDEX_SHIFT);
  const GcEstimateEntry *entry = &table->entries[segment];
  const uint32_t result =
      entry->base - ((entry->dec * index + table->round_add) >> table->round_shift);
  return (uint64_t)result << 29;
}

/* The largest single, which is what fres answers for an operand too small
 * to have a reciprocal in range. */
#define GC_LARGEST_SINGLE 0x47efffffe0000000ull

GC_FN uint64_t gc_estimate_reciprocal(GcCpu *c, uint64_t b) {
  const uint64_t sign = b & GC_SIGN_BIT;
  const uint32_t exponent = (uint32_t)((b >> 52) & 0x7ffu);
  const uint64_t mantissa = b & 0x000fffffffffffffull;
  uint64_t nan;
  int32_t result_exponent;

  if (gc_nan_result(c, b, 0, 0, 1, &nan)) return nan;
  if (exponent == 0x7ffu) return sign; /* infinity reciprocates to zero */
  if (exponent == 0 && mantissa == 0) {
    gc_raise_fpscr(c, GC_FPSCR_ZX);
    return sign | 0x7ff0000000000000ull;
  }
  result_exponent = 2045 - (int32_t)exponent;
  /* A reciprocal that would land above the largest single saturates to it
   * rather than overflowing to infinity, and one that lands below the
   * bottom of the range becomes zero. Both are measured: the reciprocal of
   * the smallest normal double comes back as the largest single. */
  if (result_exponent > 0x47e) return sign | GC_LARGEST_SINGLE;
  if (result_exponent <= 0) return sign;
  return sign | ((uint64_t)(uint32_t)result_exponent << 52) |
         gc_estimate_mantissa(&gc_fres_table, mantissa);
}

GC_FN uint64_t gc_estimate_reciprocal_sqrt(GcCpu *c, uint64_t b) {
  const uint64_t sign = b & GC_SIGN_BIT;
  uint32_t exponent = (uint32_t)((b >> 52) & 0x7ffu);
  uint64_t mantissa = b & 0x000fffffffffffffull;
  int32_t biased = (int32_t)exponent;
  uint64_t nan;
  int32_t result_exponent;

  if (gc_nan_result(c, b, 0, 0, 1, &nan)) return nan;
  if (exponent == 0 && mantissa == 0) {
    gc_raise_fpscr(c, GC_FPSCR_ZX);
    return sign | 0x7ff0000000000000ull;
  }
  if (sign) return gc_invalid(c, GC_FPSCR_VXSQRT); /* no root of a negative */
  if (exponent == 0x7ffu) return 0; /* infinity */
  if (exponent == 0) {
    /* A denormal is normalised first, which is what the measurement shows:
     * the exponent goes below zero rather than the operand being flushed. */
    while ((mantissa & 0x0010000000000000ull) == 0) {
      mantissa <<= 1;
      biased--;
    }
    mantissa &= 0x000fffffffffffffull;
    biased++;
  }
  result_exponent = (3068 - biased) >> 1;
  if (result_exponent <= 0 || result_exponent >= 0x7ff) return 0;
  return ((uint64_t)(uint32_t)result_exponent << 52) |
         gc_estimate_mantissa((biased & 1) ? &gc_frsqrte_table_odd_exponent
                                           : &gc_frsqrte_table_even_exponent,
                              mantissa);
}

GC_OP gc_op_fres(GcCpu *c, uint32_t insn) {
  const uint64_t result = gc_estimate_reciprocal(c, gc_ps0(c, GC_F_frB(insn)));
  gc_update_vx(c);
  gc_write_frd_single(c, insn, result);
}

GC_OP gc_op_frsqrte(GcCpu *c, uint32_t insn) {
  const uint64_t result = gc_estimate_reciprocal_sqrt(c, gc_ps0(c, GC_F_frB(insn)));
  gc_update_vx(c);
  gc_write_frd_scalar(c, insn, result);
}

/* ------------------------------------------------------------------ */
/* Compare                                                             */

GC_FN void gc_fp_compare(GcCpu *c, uint32_t insn, uint64_t a, uint64_t b, int ordered) {
  uint32_t bits;
  gc_fp_settle(c);
  if (gc_is_nan(a) || gc_is_nan(b)) {
    bits = 1u; /* FU */
    if (gc_is_snan(a) || gc_is_snan(b)) {
      gc_raise_fpscr(c, GC_FPSCR_VXSNAN);
      if (ordered) gc_raise_fpscr(c, GC_FPSCR_VXVC);
    } else if (ordered) {
      gc_raise_fpscr(c, GC_FPSCR_VXVC);
    }
    gc_update_vx(c);
  } else {
    const double x = gc_f64(a), y = gc_f64(b);
    bits = (x < y) ? 8u : (x > y) ? 4u : 2u;
  }
  gc_cr_set_field(c, GC_F_crfD(insn), bits);
  /* A compare writes the four condition bits of FPRF and leaves the class
   * bit above them alone: it is the class of the last arithmetic result,
   * not of anything this instruction did. hwtests/cputest/fprf.cpp sets
   * that bit on purpose beforehand and checks it survives. */
  c->fpscr = (c->fpscr & ~0x0000f000u) | (bits << 12);
}

GC_OP gc_op_fcmpu(GcCpu *c, uint32_t insn) {
  gc_fp_compare(c, insn, gc_ps0(c, GC_F_frA(insn)), gc_ps0(c, GC_F_frB(insn)), 0);
}
GC_OP gc_op_fcmpo(GcCpu *c, uint32_t insn) {
  gc_fp_compare(c, insn, gc_ps0(c, GC_F_frA(insn)), gc_ps0(c, GC_F_frB(insn)), 1);
}

/* ------------------------------------------------------------------ */
/* FPSCR access                                                        */

GC_OP gc_op_mffs(GcCpu *c, uint32_t insn) {
  gc_fp_settle(c);
  c->fpr[GC_F_frD(insn)].ps0 = 0xfff8000000000000ull | c->fpscr;
  gc_fp_rc(c, insn);
}

GC_OP gc_op_mtfsf(GcCpu *c, uint32_t insn) {
  const uint32_t fields = GC_F_mtfsf_FM(insn);
  const uint32_t value = (uint32_t)gc_ps0(c, GC_F_frB(insn));
  uint32_t mask = 0;
  unsigned i;
  for (i = 0; i < 8; i++) {
    if (fields & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
  }
  gc_fpscr_write(c, value, mask);
  gc_fp_rc(c, insn);
}

GC_OP gc_op_mtfsfi(GcCpu *c, uint32_t insn) {
  const unsigned shift = 28 - 4 * GC_F_crfD(insn);
  gc_fpscr_write(c, (uint32_t)GC_F_mtfsf_IMM(insn) << shift, 0xfu << shift);
  gc_fp_rc(c, insn);
}

GC_OP gc_op_mtfsb0(GcCpu *c, uint32_t insn) {
  gc_fpscr_write(c, 0, 0x80000000u >> GC_F_crbD(insn));
  gc_fp_rc(c, insn);
}

GC_OP gc_op_mtfsb1(GcCpu *c, uint32_t insn) {
  /* Setting a bit by hand does not set the FX summary: FX means "an
   * operation raised an exception", and this is not an operation. Bits 1
   * and 2 (FEX and VX) are derived and cannot be set this way at all. */
  const uint32_t bit = 0x80000000u >> GC_F_crbD(insn);
  gc_fpscr_write(c, bit, bit);
  gc_fp_rc(c, insn);
}

GC_OP gc_op_mcrfs(GcCpu *c, uint32_t insn) {
  const unsigned field = GC_F_crfS(insn);
  const unsigned shift = 28 - 4 * field;
  uint32_t value;
  gc_fp_settle(c);
  value = (c->fpscr >> shift) & 0xfu;
  gc_cr_set_field(c, GC_F_crfD(insn), value);
  /* Reading a field clears its exception bits, but not the summary bits or
   * the mode bits. */
  if (field != 0 && field != 6 && field != 7) c->fpscr &= ~(0xfu << shift);
  if (field == 0) c->fpscr &= ~(GC_FPSCR_FX | GC_FPSCR_OX);
  gc_update_vx(c);
}

/* ------------------------------------------------------------------ */
/* Floating-point loads and stores                                     */

/* lfs loads a single and fills both lanes: code that then reads ps1 with a
 * paired instruction sees the same value, which is what makes `lfs` the
 * usual way to splat a scalar. */
#define GC_FLOAD(name, ea_expr, single, update)                                  \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                  \
    const uint32_t ea = ea_expr(c, insn);                                        \
    uint64_t value;                                                              \
    if (single) {                                                                \
      const uint32_t raw = gc_bus_fast_read32(c, ea);                                 \
      if (c->pending_vector) return;                                             \
      value = gc_single_to_double(raw);                                  \
    } else {                                                                     \
      value = gc_bus_fast_read64(c, ea);                                              \
      if (c->pending_vector) return;                                             \
    }                                                                            \
    c->fpr[GC_F_frD(insn)].ps0 = value;                                          \
    if (single) c->fpr[GC_F_frD(insn)].ps1 = value;                              \
    if (update) gc_update_ra(c, insn, ea);                                       \
  }

GC_FLOAD(lfs, gc_ea_d, 1, 0)
GC_FLOAD(lfsu, gc_ea_du, 1, 1)
GC_FLOAD(lfsx, gc_ea_x, 1, 0)
GC_FLOAD(lfsux, gc_ea_xu, 1, 1)
GC_FLOAD(lfd, gc_ea_d, 0, 0)
GC_FLOAD(lfdu, gc_ea_du, 0, 1)
GC_FLOAD(lfdx, gc_ea_x, 0, 0)
GC_FLOAD(lfdux, gc_ea_xu, 0, 1)

#define GC_FSTORE(name, ea_expr, single, update)                                 \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                  \
    const uint32_t ea = ea_expr(c, insn);                                        \
    if (single) {                                                                \
      gc_bus_fast_write32(c, ea, gc_store_single(gc_ps0(c, GC_F_frS(insn))));         \
    } else {                                                                     \
      gc_bus_fast_write64(c, ea, gc_ps0(c, GC_F_frS(insn)));                          \
    }                                                                            \
    if (update) gc_update_ra(c, insn, ea);                                       \
  }

GC_FSTORE(stfs, gc_ea_d, 1, 0)
GC_FSTORE(stfsu, gc_ea_du, 1, 1)
GC_FSTORE(stfsx, gc_ea_x, 1, 0)
GC_FSTORE(stfsux, gc_ea_xu, 1, 1)
GC_FSTORE(stfd, gc_ea_d, 0, 0)
GC_FSTORE(stfdu, gc_ea_du, 0, 1)
GC_FSTORE(stfdx, gc_ea_x, 0, 0)
GC_FSTORE(stfdux, gc_ea_xu, 0, 1)

/* stfiwx stores the low word of the register without converting it: the
 * other half of the fctiw/stfiwx pair every float-to-int cast compiles to. */
GC_OP gc_op_stfiwx(GcCpu *c, uint32_t insn) {
  gc_bus_fast_write32(c, gc_ea_x(c, insn), (uint32_t)gc_ps0(c, GC_F_frS(insn)));
}

/* ------------------------------------------------------------------ */
/* Paired singles                                                      */

/* The paired-single datapath cannot carry a full double. A value that goes
 * through it comes out with only the mantissa bits a single has - and the
 * two lanes do it differently: what lands in ps0 is rounded under the
 * current rounding mode, what lands in ps1 is truncated.
 *
 * hwtests/cputest/pairedmove.cpp shows it plainly: move the double nearest
 * to a third through ps_merge and the halves come back as the two singles
 * either side of it. Any move that ignores this quietly keeps precision the
 * hardware does not have, and a game's paired arithmetic then drifts away
 * from what it computed on the console.
 *
 * The rule is about the lane, not the instruction: anything written to ps0
 * is rounded and anything written to ps1 is truncated, which is why
 * ps_sum0 truncates the lane it passes through and ps_sum1 rounds it.
 * Arithmetic results are already single-representable, so it only shows on
 * a value that came in as a double.
 *
 * A NaN or an infinity is only truncated: rounding one could carry into the
 * exponent. */
GC_FN uint64_t gc_ps_truncate(uint64_t bits) { return bits & ~0x000000001fffffffull; }

GC_FN uint64_t gc_ps_round(const GcCpu *c, uint64_t bits) {
  const uint64_t low = bits & 0x000000001fffffffull;
  const uint64_t rest = bits & ~0x000000001fffffffull;
  const uint64_t half = 0x0000000010000000ull;
  const uint64_t ulp = 0x0000000020000000ull;
  const int negative = (bits & GC_SIGN_BIT) != 0;
  if (!low) return bits;
  if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) return rest;
  switch (c->fpscr & GC_FPSCR_RN) {
    case 0: /* nearest, ties to even */
      if (low > half || (low == half && (rest & ulp))) return rest + ulp;
      return rest;
    case 1: return rest;                          /* toward zero */
    case 2: return negative ? rest : rest + ulp;  /* toward +infinity */
    default: return negative ? rest + ulp : rest; /* toward -infinity */
  }
}



/* Each paired arithmetic form tries both lanes on the fast path at once
 * (gc_ps_fast_binary, gc_ps_fast_commit) and otherwise runs as it always
 * did (gc_ps_binary_slow). */
#define GC_PS_BINOP(name, kind, a_lane, b_lane)                                        \
  GC_FN int gc_lazy_##name(GcCpu *c, uint32_t insn) {                                  \
    return gc_ps_binary_lazy(c, insn, kind, a_lane(c, GC_F_frA(insn)), b_lane(c, GC_F_frB(insn)), \
                             gc_ps1(c, GC_F_frA(insn)), gc_ps1(c, GC_F_frB(insn)));    \
  }                                                                                    \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                        \
    const uint64_t a0 = a_lane(c, GC_F_frA(insn)), b0 = b_lane(c, GC_F_frB(insn));     \
    const uint64_t a1 = gc_ps1(c, GC_F_frA(insn)), b1 = gc_ps1(c, GC_F_frB(insn));     \
    uint64_t ps0, ps1;                                                                 \
    if (gc_ps_binary_lazy(c, insn, kind, a0, b0, a1, b1)) return;                      \
    int inexact0, inexact1;                                                            \
    if (gc_ps_fast_binary(c, kind, a0, b0, a1, b1, &ps0, &ps1, &inexact0, &inexact1)) { \
      if (gc_fp_lazy_ok(c, insn)) {                                                    \
        gc_fp_lazy_note(c, insn, GC_FP_LAZY_ADD + (unsigned)kind, a1, b1, 0, ps0, ps1); \
      } else {                                                                         \
        gc_ps_binary_eager(c, insn, kind, a0, b0, a1, b1);                             \
      }                                                                                \
    } else {                                                                           \
      gc_ps_binary_slow(c, insn, kind, a0, b0, a1, b1);                                \
    }                                                                                  \
  }

GC_PS_BINOP(ps_add, GC_FP_ADD, gc_ps0, gc_ps0)
GC_PS_BINOP(ps_sub, GC_FP_SUB, gc_ps0, gc_ps0)
GC_PS_BINOP(ps_div, GC_FP_DIV, gc_ps0, gc_ps0)

/* The multiply forms take frC, and the s0/s1 variants use one lane of frC
 * for both lanes of the result - the cheap way to scale a vector. */
#define GC_PS_MULOP(name, c0_lane, c1_lane)                                            \
  GC_FN int gc_lazy_##name(GcCpu *c, uint32_t insn) {                                  \
    return gc_ps_binary_lazy(c, insn, GC_FP_MUL, gc_ps0(c, GC_F_frA(insn)), c0_lane(c, GC_F_frC(insn)), \
                             gc_ps1(c, GC_F_frA(insn)), c1_lane(c, GC_F_frC(insn)));   \
  }                                                                                    \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                        \
    const uint64_t a0 = gc_ps0(c, GC_F_frA(insn)), b0 = c0_lane(c, GC_F_frC(insn));    \
    const uint64_t a1 = gc_ps1(c, GC_F_frA(insn)), b1 = c1_lane(c, GC_F_frC(insn));    \
    uint64_t ps0, ps1;                                                                 \
    if (gc_ps_binary_lazy(c, insn, GC_FP_MUL, a0, b0, a1, b1)) return;                 \
    int inexact0, inexact1;                                                            \
    if (gc_ps_fast_binary(c, GC_FP_MUL, a0, b0, a1, b1, &ps0, &ps1, &inexact0, &inexact1)) { \
      if (gc_fp_lazy_ok(c, insn)) {                                                    \
        gc_fp_lazy_note(c, insn, GC_FP_LAZY_MUL, a1, b1, 0, ps0, ps1);                 \
      } else {                                                                         \
        gc_ps_binary_eager(c, insn, GC_FP_MUL, a0, b0, a1, b1);                        \
      }                                                                                \
    } else {                                                                           \
      gc_ps_binary_slow(c, insn, GC_FP_MUL, a0, b0, a1, b1);                           \
    }                                                                                  \
  }

GC_PS_MULOP(ps_mul, gc_ps0, gc_ps1)
GC_PS_MULOP(ps_muls0, gc_ps0, gc_ps0)
GC_PS_MULOP(ps_muls1, gc_ps1, gc_ps1)

#define GC_PS_MADD(name, c0_lane, c1_lane, negate_add, negate_result)                  \
  GC_FN int gc_lazy_##name(GcCpu *c, uint32_t insn) {                                  \
    return gc_ps_madd_lazy(c, insn, gc_ps0(c, GC_F_frA(insn)), gc_ps0(c, GC_F_frB(insn)),       \
                           c0_lane(c, GC_F_frC(insn)), gc_ps1(c, GC_F_frA(insn)),      \
                           gc_ps1(c, GC_F_frB(insn)), c1_lane(c, GC_F_frC(insn)), negate_add, negate_result); \
  }                                                                                    \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                        \
    const uint64_t a0 = gc_ps0(c, GC_F_frA(insn)), b0 = gc_ps0(c, GC_F_frB(insn));     \
    const uint64_t a1 = gc_ps1(c, GC_F_frA(insn)), b1 = gc_ps1(c, GC_F_frB(insn));     \
    const uint64_t c0 = c0_lane(c, GC_F_frC(insn)), c1 = c1_lane(c, GC_F_frC(insn));   \
    uint64_t ps0, ps1;                                                                 \
    if (gc_ps_madd_lazy(c, insn, a0, b0, c0, a1, b1, c1, negate_add, negate_result)) return; \
    int inexact0, inexact1;                                                            \
    if (gc_ps_fast_madd(c, a0, b0, c0, a1, b1, c1, negate_add, negate_result, &ps0, &ps1, &inexact0, \
                        &inexact1)) {                                                  \
      if (gc_fp_lazy_ok(c, insn)) {                                                    \
        gc_fp_lazy_note(c, insn, GC_FP_LAZY_MADD + 2u * (negate_add) + (negate_result), a1, b1, c1, ps0, ps1); \
      } else {                                                                         \
        gc_ps_madd_eager(c, insn, a0, b0, c0, a1, b1, c1, negate_add, negate_result);  \
      }                                                                                \
    } else {                                                                           \
      gc_ps_madd_slow(c, insn, a0, b0, c0, a1, b1, c1, negate_add, negate_result);     \
    }                                                                                  \
  }

GC_PS_MADD(ps_madd, gc_ps0, gc_ps1, 0, 0)
GC_PS_MADD(ps_msub, gc_ps0, gc_ps1, 1, 0)
GC_PS_MADD(ps_nmadd, gc_ps0, gc_ps1, 0, 1)
GC_PS_MADD(ps_nmsub, gc_ps0, gc_ps1, 1, 1)
GC_PS_MADD(ps_madds0, gc_ps0, gc_ps0, 0, 0)
GC_PS_MADD(ps_madds1, gc_ps1, gc_ps1, 0, 0)

/* ps_sum0 puts a cross-lane sum in ps0 and copies frC's ps1 through;
 * ps_sum1 is the mirror image. Together they are how a dot product ends. */
GC_OP gc_op_ps_sum0(GcCpu *c, uint32_t insn) {
  const uint64_t sum = gc_fp_binary(c, GC_FP_ADD, gc_ps0(c, GC_F_frA(insn)),
                                    gc_ps1(c, GC_F_frB(insn)), 1);
  gc_update_vx(c);
  gc_write_frd(c, insn, sum, gc_ps_truncate(gc_ps1(c, GC_F_frC(insn))));
  gc_set_fprf(c, sum);
  gc_fp_rc(c, insn);
}

GC_OP gc_op_ps_sum1(GcCpu *c, uint32_t insn) {
  const uint64_t sum = gc_fp_binary(c, GC_FP_ADD, gc_ps0(c, GC_F_frA(insn)),
                                    gc_ps1(c, GC_F_frB(insn)), 1);
  gc_update_vx(c);
  gc_write_frd(c, insn, gc_ps_round(c, gc_ps0(c, GC_F_frC(insn))), sum);
  gc_set_fprf(c, sum);
  gc_fp_rc(c, insn);
}

#define GC_PS_MOVE(name, ps0_expr, ps1_expr)                                   GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                  const uint64_t ps0 = gc_ps_round(c, (ps0_expr));                             const uint64_t ps1 = gc_ps_truncate(ps1_expr);                               gc_write_frd(c, insn, ps0, ps1);                                             gc_fp_rc(c, insn);                                                         }

GC_PS_MOVE(ps_neg, gc_ps0(c, GC_F_frB(insn)) ^ GC_SIGN_BIT,
           gc_ps1(c, GC_F_frB(insn)) ^ GC_SIGN_BIT)
GC_PS_MOVE(ps_abs, gc_ps0(c, GC_F_frB(insn)) & ~GC_SIGN_BIT,
           gc_ps1(c, GC_F_frB(insn)) & ~GC_SIGN_BIT)
GC_PS_MOVE(ps_nabs, gc_ps0(c, GC_F_frB(insn)) | GC_SIGN_BIT,
           gc_ps1(c, GC_F_frB(insn)) | GC_SIGN_BIT)
GC_PS_MOVE(ps_mr, gc_ps0(c, GC_F_frB(insn)), gc_ps1(c, GC_F_frB(insn)))

/* The merges are the lane shuffles: every vector swizzle in a game is built
 * from these four. */
GC_PS_MOVE(ps_merge00, gc_ps0(c, GC_F_frA(insn)), gc_ps0(c, GC_F_frB(insn)))
GC_PS_MOVE(ps_merge01, gc_ps0(c, GC_F_frA(insn)), gc_ps1(c, GC_F_frB(insn)))
GC_PS_MOVE(ps_merge10, gc_ps1(c, GC_F_frA(insn)), gc_ps0(c, GC_F_frB(insn)))
GC_PS_MOVE(ps_merge11, gc_ps1(c, GC_F_frA(insn)), gc_ps1(c, GC_F_frB(insn)))

GC_OP gc_op_ps_sel(GcCpu *c, uint32_t insn) {
  const uint64_t a0 = gc_ps0(c, GC_F_frA(insn));
  const uint64_t a1 = gc_ps1(c, GC_F_frA(insn));
  const int take0 = !gc_is_nan(a0) && gc_f64(a0) >= 0.0;
  const int take1 = !gc_is_nan(a1) && gc_f64(a1) >= 0.0;
  gc_write_frd(c, insn,
               take0 ? gc_ps0(c, GC_F_frC(insn)) : gc_ps0(c, GC_F_frB(insn)),
               take1 ? gc_ps1(c, GC_F_frC(insn)) : gc_ps1(c, GC_F_frB(insn)));
  gc_fp_rc(c, insn);
}

GC_OP gc_op_ps_res(GcCpu *c, uint32_t insn) {
  const uint64_t ps0 = gc_estimate_reciprocal(c, gc_ps0(c, GC_F_frB(insn)));
  const uint64_t ps1 = gc_estimate_reciprocal(c, gc_ps1(c, GC_F_frB(insn)));
  gc_update_vx(c);
  gc_write_frd(c, insn, ps0, ps1);
  gc_set_fprf_single(c, ps0);
  gc_fp_rc(c, insn);
}

GC_OP gc_op_ps_rsqrte(GcCpu *c, uint32_t insn) {
  const uint64_t ps0 = gc_estimate_reciprocal_sqrt(c, gc_ps0(c, GC_F_frB(insn)));
  const uint64_t ps1 = gc_estimate_reciprocal_sqrt(c, gc_ps1(c, GC_F_frB(insn)));
  gc_update_vx(c);
  gc_write_frd(c, insn, ps0, ps1);
  gc_set_fprf_single(c, ps0);
  gc_fp_rc(c, insn);
}

#define GC_PS_COMPARE(name, a_lane, b_lane, ordered)                                   \
  GC_OP gc_op_##name(GcCpu *c, uint32_t insn) {                                        \
    gc_fp_compare(c, insn, a_lane(c, GC_F_frA(insn)), b_lane(c, GC_F_frB(insn)),       \
                  ordered);                                                            \
  }

GC_PS_COMPARE(ps_cmpu0, gc_ps0, gc_ps0, 0)
GC_PS_COMPARE(ps_cmpo0, gc_ps0, gc_ps0, 1)
GC_PS_COMPARE(ps_cmpu1, gc_ps1, gc_ps1, 0)
GC_PS_COMPARE(ps_cmpo1, gc_ps1, gc_ps1, 1)

/* ------------------------------------------------------------------ */
/* Quantised load and store                                            */

/* GQR layout, from the manual's Table 2-16 (bits numbered from the most
 * significant end): LD_SCALE 2-7, LD_TYPE 13-15, ST_SCALE 18-23,
 * ST_TYPE 29-31. The scales are six-bit two's complement. */
#define GC_GQR_LD_SCALE(gqr) gc_sext32(((gqr) >> 24) & 0x3fu, 6)
#define GC_GQR_LD_TYPE(gqr) (((gqr) >> 16) & 0x7u)
#define GC_GQR_ST_SCALE(gqr) gc_sext32(((gqr) >> 8) & 0x3fu, 6)
#define GC_GQR_ST_TYPE(gqr) ((gqr) & 0x7u)

enum { GC_QUANT_FLOAT = 0, GC_QUANT_U8 = 4, GC_QUANT_U16 = 5, GC_QUANT_S8 = 6, GC_QUANT_S16 = 7 };

GC_FN unsigned gc_quant_size(unsigned type) {
  switch (type) {
    case GC_QUANT_U8:
    case GC_QUANT_S8: return 1;
    case GC_QUANT_U16:
    case GC_QUANT_S16: return 2;
    default: return 4;
  }
}

/* F = I * 2**(-S), from the raw big-endian value of the type's size. A
 * reserved type (1..3) behaves as the float type.
 *
 * For the integer types the product is already a single: at most 16
 * significant bits, and a magnitude between 2^-31 and 2^48 (or zero) since
 * the scale is -32 to 31. So it is exact as a double and narrowing it to
 * single and back gives the same double in every rounding mode, and there
 * is no need to do that round trip. */
GC_FN uint64_t gc_dequantize_raw(uint32_t raw, unsigned type, int scale) {
  double value;
  switch (type) {
    case GC_QUANT_U8: value = (double)(uint8_t)raw; break;
    case GC_QUANT_S8: value = (double)(int8_t)(uint8_t)raw; break;
    case GC_QUANT_U16: value = (double)(uint16_t)raw; break;
    case GC_QUANT_S16: value = (double)(int16_t)(uint16_t)raw; break;
    default: return gc_single_to_double(raw);
  }
  return gc_f64_bits(value * gc_power_of_two(-scale));
}

GC_FN uint32_t gc_quant_load_bytes(const uint8_t *p, unsigned size) {
  switch (size) {
    case 1: return p[0];
    case 2: return ((uint32_t)p[0] << 8) | p[1];
    default: {
      uint32_t v;
      memcpy(&v, p, 4u);
      return GC_BSWAP32(v);
    }
  }
}

GC_FN uint64_t gc_dequantize(GcCpu *c, uint32_t ea, unsigned type, int scale) {
  uint32_t raw;
  switch (gc_quant_size(type)) {
    case 1: raw = gc_bus_fast_read8(c, ea); break;
    case 2: raw = gc_bus_fast_read16(c, ea); break;
    default: raw = gc_bus_fast_read32(c, ea); break;
  }
  return gc_dequantize_raw(raw, type, scale);
}

/* I = ROUND(F * 2**S), clamped to the target type, as the raw value of the
 * type's size. The rounding is toward zero; that, and what a NaN stores,
 * are pinned by the vectors. */
GC_FN uint32_t gc_quantize_raw(uint64_t bits, unsigned type, int scale) {
  double value;
  if (type == GC_QUANT_FLOAT || type < GC_QUANT_U8) {
    /* The float type stores the value with no conversion beyond the
     * double-to-single repacking - which truncates rather than rounds, so
     * 0.1 goes out as 3dcccccc and not 3dcccccd - except that a denormal is
     * stored as zero, which the manual states explicitly. */
    if ((bits & 0x7ff0000000000000ull) == 0 && (bits & 0x000fffffffffffffull) != 0) {
      return (uint32_t)((bits >> 32) & 0x80000000u);
    }
    return gc_store_single(bits);
  }
  value = gc_f64(bits) * gc_power_of_two(scale);
  if (gc_is_nan(bits)) value = 0.0;
  switch (type) {
    case GC_QUANT_U8:
      value = value < 0.0 ? 0.0 : (value > 255.0 ? 255.0 : value);
      return (uint8_t)value;
    case GC_QUANT_S8:
      value = value < -128.0 ? -128.0 : (value > 127.0 ? 127.0 : value);
      return (uint8_t)(int8_t)value;
    case GC_QUANT_U16:
      value = value < 0.0 ? 0.0 : (value > 65535.0 ? 65535.0 : value);
      return (uint16_t)value;
    default:
      value = value < -32768.0 ? -32768.0 : (value > 32767.0 ? 32767.0 : value);
      return (uint16_t)(int16_t)value;
  }
}

GC_FN void gc_quant_store_bytes(uint8_t *p, unsigned size, uint32_t raw) {
  switch (size) {
    case 1: p[0] = (uint8_t)raw; break;
    case 2:
      p[0] = (uint8_t)(raw >> 8);
      p[1] = (uint8_t)raw;
      break;
    default:
      p[0] = (uint8_t)(raw >> 24);
      p[1] = (uint8_t)(raw >> 16);
      p[2] = (uint8_t)(raw >> 8);
      p[3] = (uint8_t)raw;
      break;
  }
}

GC_FN void gc_quantize(GcCpu *c, uint32_t ea, uint64_t bits, unsigned type, int scale) {
  const uint32_t raw = gc_quantize_raw(bits, type, scale);
  switch (gc_quant_size(type)) {
    case 1: gc_bus_fast_write8(c, ea, (uint8_t)raw); break;
    case 2: gc_bus_fast_write16(c, ea, (uint16_t)raw); break;
    default: gc_bus_fast_write32(c, ea, raw); break;
  }
}

/* A quantised load or store lane by lane through the bus, with the fault
 * check between: one shared copy a file. */
GC_FP_SLOW void gc_psq_load_lanes(GcCpu *c, uint32_t insn, uint32_t ea, unsigned type, int scale, unsigned w) {
  const unsigned size = gc_quant_size(type);
  uint64_t ps0, ps1;
  ps0 = gc_dequantize(c, ea, type, scale);
  if (c->pending_vector) return;
  if (w) {
    ps1 = 0x3ff0000000000000ull; /* 1.0 */
  } else {
    ps1 = gc_dequantize(c, ea + size, type, scale);
    if (c->pending_vector) return;
  }
  gc_write_frd(c, insn, ps0, ps1);
}

GC_FP_SLOW void gc_psq_store_lanes(GcCpu *c, uint32_t insn, uint32_t ea, unsigned type, int scale, unsigned w) {
  const unsigned size = gc_quant_size(type);
  gc_quantize(c, ea, gc_ps0(c, GC_F_frS(insn)), type, scale);
  if (c->pending_vector || w) return;
  gc_quantize(c, ea + size, gc_ps1(c, GC_F_frS(insn)), type, scale);
}

/* W = 1 means one value only: ps1 is loaded with 1.0, or not stored.
 *
 * When the fast map covers every byte both lanes touch, neither access can
 * fault or do anything but read or write main memory, so both are done
 * straight from the map; otherwise lane by lane through the bus, with the
 * fault check between. The span [ea, ea + 2 * size) fitting under the
 * piece's limit keeps ea + size in the same piece.
 *
 * gc_psq_load_in and gc_psq_store_in are that first half on its own, through
 * a map the caller chose (null for none): 1 when they did the whole
 * instruction, 0 - having changed nothing - when the map does not cover it.
 * gc_psq_load_fast and gc_psq_store_fast take the map as the bus would;
 * recompiled code passes the one its block chose, and takes the slow half
 * itself with the machine brought up to date first. */
GC_FN int gc_psq_load_in(GcCpu *c, const GcFastMap *map, uint32_t insn, uint32_t ea, unsigned gqr_index,
                         unsigned w) {
  const uint32_t gqr = c->gqr[gqr_index];
  const unsigned type = GC_GQR_LD_TYPE(gqr);
  const int scale = GC_GQR_LD_SCALE(gqr);
  const unsigned size = gc_quant_size(type);
  if (map && GC_BUS_MAP_OK(map, ea, w ? size : 2u * size)) {
    /* gc_dequantize_raw for both lanes, with the type decided once. */
    const uint8_t *p = GC_BUS_MAP_AT(map, ea);
    uint64_t ps0, ps1 = 0x3ff0000000000000ull;
    if (size == 4u) {
      /* Both lanes' bytes in one load when there are two. */
      if (w) {
        ps0 = gc_single_to_double(gc_quant_load_bytes(p, 4u));
      } else {
        uint64_t both;
        memcpy(&both, p, 8u);
        both = GC_BSWAP64(both);
        ps0 = (uint32_t)(both >> 32);
        ps1 = (uint32_t)both;
#if defined(_M_X64) || defined(__x86_64__)
        /* Neither lane a NaN or an infinity: a widening conversion of both at
         * once gives what gc_single_to_double gives each (it converts too,
         * and only keeps a NaN's bits by hand). */
        if (((ps0 & 0x7f800000u) != 0x7f800000u) & ((ps1 & 0x7f800000u) != 0x7f800000u)) {
          double out[2];
          _mm_storeu_pd(out, _mm_cvtps_pd(_mm_castsi128_ps(_mm_set_epi32(0, 0, (int)(uint32_t)ps1, (int)(uint32_t)ps0))));
          memcpy(&c->fpr[GC_F_frD(insn)], out, sizeof(out));
          return 1;
        }
#endif
        ps0 = gc_single_to_double((uint32_t)ps0);
        ps1 = gc_single_to_double((uint32_t)ps1);
      }
    } else if (!w) {
#if defined(_M_X64) || defined(__x86_64__)
      /* Both lanes as one vector: the same conversions and products,
       * element by element, as the scalar code below. */
      __m128i raw;
      double out[2];
      switch (type) {
        case GC_QUANT_U8: raw = _mm_setr_epi32(p[0], p[1], 0, 0); break;
        case GC_QUANT_S8: raw = _mm_setr_epi32((int8_t)p[0], (int8_t)p[1], 0, 0); break;
        case GC_QUANT_U16:
          raw = _mm_setr_epi32((int)(((unsigned)p[0] << 8) | p[1]), (int)(((unsigned)p[2] << 8) | p[3]), 0, 0);
          break;
        default:
          raw = _mm_setr_epi32((int16_t)(uint16_t)(((unsigned)p[0] << 8) | p[1]),
                               (int16_t)(uint16_t)(((unsigned)p[2] << 8) | p[3]), 0, 0);
          break;
      }
      _mm_storeu_pd(out, _mm_mul_pd(_mm_cvtepi32_pd(raw), _mm_set1_pd(gc_power_of_two(-scale))));
      memcpy(&c->fpr[GC_F_frD(insn)], out, sizeof(out));
      return 1;
#else
      const double k = gc_power_of_two(-scale);
      double v0, v1;
      switch (type) {
        case GC_QUANT_U8: v0 = (double)p[0]; v1 = (double)p[1]; break;
        case GC_QUANT_S8: v0 = (double)(int8_t)p[0]; v1 = (double)(int8_t)p[1]; break;
        case GC_QUANT_U16:
          v0 = (double)(uint16_t)(((unsigned)p[0] << 8) | p[1]);
          v1 = (double)(uint16_t)(((unsigned)p[2] << 8) | p[3]);
          break;
        default:
          v0 = (double)(int16_t)(uint16_t)(((unsigned)p[0] << 8) | p[1]);
          v1 = (double)(int16_t)(uint16_t)(((unsigned)p[2] << 8) | p[3]);
          break;
      }
      ps0 = gc_f64_bits(v0 * k);
      ps1 = gc_f64_bits(v1 * k);
#endif
    } else {
      const double k = gc_power_of_two(-scale);
      double v0, v1 = 0.0;
      switch (type) {
        case GC_QUANT_U8:
          v0 = (double)p[0];
          if (!w) v1 = (double)p[1];
          break;
        case GC_QUANT_S8:
          v0 = (double)(int8_t)p[0];
          if (!w) v1 = (double)(int8_t)p[1];
          break;
        case GC_QUANT_U16:
          v0 = (double)(uint16_t)(((unsigned)p[0] << 8) | p[1]);
          if (!w) v1 = (double)(uint16_t)(((unsigned)p[2] << 8) | p[3]);
          break;
        default:
          v0 = (double)(int16_t)(uint16_t)(((unsigned)p[0] << 8) | p[1]);
          if (!w) v1 = (double)(int16_t)(uint16_t)(((unsigned)p[2] << 8) | p[3]);
          break;
      }
      ps0 = gc_f64_bits(v0 * k);
      if (!w) ps1 = gc_f64_bits(v1 * k);
    }
    gc_write_frd(c, insn, ps0, ps1);
    return 1;
  }
  return 0;
}

GC_FN int gc_psq_load_fast(GcCpu *c, uint32_t insn, uint32_t ea, unsigned gqr_index, unsigned w) {
  return gc_psq_load_in(c, (c->msr & GC_MSR_PR) ? NULL : GC_BUS_FAST_MAP(c), insn, ea, gqr_index, w);
}

GC_FN void gc_psq_load(GcCpu *c, uint32_t insn, uint32_t ea, unsigned gqr_index, unsigned w) {
  if (!gc_psq_load_fast(c, insn, ea, gqr_index, w)) {
    const uint32_t gqr = c->gqr[gqr_index];
    gc_psq_load_lanes(c, insn, ea, GC_GQR_LD_TYPE(gqr), GC_GQR_LD_SCALE(gqr), w);
  }
}

GC_FN int gc_psq_store_in(GcCpu *c, const GcFastMap *map, uint32_t insn, uint32_t ea, unsigned gqr_index,
                          unsigned w) {
  const uint32_t gqr = c->gqr[gqr_index];
  const unsigned type = GC_GQR_ST_TYPE(gqr);
  const int scale = GC_GQR_ST_SCALE(gqr);
  const unsigned size = gc_quant_size(type);
  if (map && GC_BUS_MAP_OK(map, ea, w ? size : 2u * size)) {
    /* gc_quantize_raw for both lanes, with the type decided once. */
    uint8_t *p = GC_BUS_MAP_AT(map, ea);
    const uint64_t b0 = gc_ps0(c, GC_F_frS(insn));
    const uint64_t b1 = gc_ps1(c, GC_F_frS(insn));
    if (size == 4u) {
      gc_quant_store_bytes(p, 4u, gc_quantize_raw(b0, type, scale));
      if (!w) gc_quant_store_bytes(p + 4, 4u, gc_quantize_raw(b1, type, scale));
#if defined(_M_X64) || defined(__x86_64__)
    } else if (!w) {
      /* Both lanes as one vector. A NaN becomes +0 (the scalar code's 0.0);
       * the clamp is max then min, which gives the scalar code's bound for a
       * value beyond it and the value itself otherwise - a -0 against a +0
       * bound may come out as either zero, and both truncate to 0; and the
       * truncation is the conversion the scalar code's cast does. */
      static const double bounds[4][2] = {{0.0, 255.0}, {0.0, 65535.0}, {-128.0, 127.0}, {-32768.0, 32767.0}};
      const unsigned which = type - GC_QUANT_U8;
      double in[2];
      __m128d v;
      __m128i r;
      uint32_t r0, r1;
      memcpy(in, &c->fpr[GC_F_frS(insn)], sizeof(in));
      v = _mm_loadu_pd(in);
      v = _mm_andnot_pd(_mm_cmpunord_pd(v, v), _mm_mul_pd(v, _mm_set1_pd(gc_power_of_two(scale))));
      v = _mm_min_pd(_mm_max_pd(v, _mm_set1_pd(bounds[which][0])), _mm_set1_pd(bounds[which][1]));
      r = _mm_cvttpd_epi32(v);
      r0 = (uint32_t)_mm_cvtsi128_si32(r);
      r1 = (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(r, 4));
      if (size == 1u) {
        p[0] = (uint8_t)r0;
        p[1] = (uint8_t)r1;
      } else {
        p[0] = (uint8_t)(r0 >> 8);
        p[1] = (uint8_t)r0;
        p[2] = (uint8_t)(r1 >> 8);
        p[3] = (uint8_t)r1;
      }
#endif
    } else {
      const double k = gc_power_of_two(scale);
      double v0 = gc_is_nan(b0) ? 0.0 : gc_f64(b0) * k;
      double v1 = (w || gc_is_nan(b1)) ? 0.0 : gc_f64(b1) * k;
      switch (type) {
        case GC_QUANT_U8:
          p[0] = (uint8_t)(v0 < 0.0 ? 0.0 : (v0 > 255.0 ? 255.0 : v0));
          if (!w) p[1] = (uint8_t)(v1 < 0.0 ? 0.0 : (v1 > 255.0 ? 255.0 : v1));
          break;
        case GC_QUANT_S8:
          p[0] = (uint8_t)(int8_t)(v0 < -128.0 ? -128.0 : (v0 > 127.0 ? 127.0 : v0));
          if (!w) p[1] = (uint8_t)(int8_t)(v1 < -128.0 ? -128.0 : (v1 > 127.0 ? 127.0 : v1));
          break;
        case GC_QUANT_U16: {
          const uint16_t r0 = (uint16_t)(v0 < 0.0 ? 0.0 : (v0 > 65535.0 ? 65535.0 : v0));
          p[0] = (uint8_t)(r0 >> 8);
          p[1] = (uint8_t)r0;
          if (!w) {
            const uint16_t r1 = (uint16_t)(v1 < 0.0 ? 0.0 : (v1 > 65535.0 ? 65535.0 : v1));
            p[2] = (uint8_t)(r1 >> 8);
            p[3] = (uint8_t)r1;
          }
          break;
        }
        default: {
          const uint16_t r0 = (uint16_t)(int16_t)(v0 < -32768.0 ? -32768.0 : (v0 > 32767.0 ? 32767.0 : v0));
          p[0] = (uint8_t)(r0 >> 8);
          p[1] = (uint8_t)r0;
          if (!w) {
            const uint16_t r1 = (uint16_t)(int16_t)(v1 < -32768.0 ? -32768.0 : (v1 > 32767.0 ? 32767.0 : v1));
            p[2] = (uint8_t)(r1 >> 8);
            p[3] = (uint8_t)r1;
          }
          break;
        }
      }
    }
    return 1;
  }
  return 0;
}

GC_FN int gc_psq_store_fast(GcCpu *c, uint32_t insn, uint32_t ea, unsigned gqr_index, unsigned w) {
  return gc_psq_store_in(c, (c->reserve || (c->msr & GC_MSR_PR)) ? NULL : GC_BUS_FAST_MAP(c), insn, ea,
                         gqr_index, w);
}

GC_FN void gc_psq_store(GcCpu *c, uint32_t insn, uint32_t ea, unsigned gqr_index, unsigned w) {
  if (!gc_psq_store_fast(c, insn, ea, gqr_index, w)) {
    const uint32_t gqr = c->gqr[gqr_index];
    gc_psq_store_lanes(c, insn, ea, GC_GQR_ST_TYPE(gqr), GC_GQR_ST_SCALE(gqr), w);
  }
}

/* The D-form quantised instructions trade eight bits of displacement for
 * the W and I fields, so their reach is 12 bits, not 16. */
GC_FN uint32_t gc_ea_ps(const GcCpu *c, uint32_t insn) {
  return gc_ra_or_zero(c, insn) + (uint32_t)GC_F_ps_offset(insn);
}

GC_FN uint32_t gc_ea_ps_u(const GcCpu *c, uint32_t insn) {
  return c->gpr[GC_F_rA(insn)] + (uint32_t)GC_F_ps_offset(insn);
}

GC_OP gc_op_psq_l(GcCpu *c, uint32_t insn) {
  gc_psq_load(c, insn, gc_ea_ps(c, insn), GC_F_ps_I(insn), GC_F_ps_W(insn));
}

GC_OP gc_op_psq_lu(GcCpu *c, uint32_t insn) {
  const uint32_t ea = gc_ea_ps_u(c, insn);
  gc_psq_load(c, insn, ea, GC_F_ps_I(insn), GC_F_ps_W(insn));
  gc_update_ra(c, insn, ea);
}

GC_OP gc_op_psq_lx(GcCpu *c, uint32_t insn) {
  gc_psq_load(c, insn, gc_ea_x(c, insn), GC_F_ps_IX(insn), GC_F_ps_WX(insn));
}

GC_OP gc_op_psq_lux(GcCpu *c, uint32_t insn) {
  const uint32_t ea = gc_ea_xu(c, insn);
  gc_psq_load(c, insn, ea, GC_F_ps_IX(insn), GC_F_ps_WX(insn));
  gc_update_ra(c, insn, ea);
}

GC_OP gc_op_psq_st(GcCpu *c, uint32_t insn) {
  gc_psq_store(c, insn, gc_ea_ps(c, insn), GC_F_ps_I(insn), GC_F_ps_W(insn));
}

GC_OP gc_op_psq_stu(GcCpu *c, uint32_t insn) {
  const uint32_t ea = gc_ea_ps_u(c, insn);
  gc_psq_store(c, insn, ea, GC_F_ps_I(insn), GC_F_ps_W(insn));
  gc_update_ra(c, insn, ea);
}

GC_OP gc_op_psq_stx(GcCpu *c, uint32_t insn) {
  gc_psq_store(c, insn, gc_ea_x(c, insn), GC_F_ps_IX(insn), GC_F_ps_WX(insn));
}

GC_OP gc_op_psq_stux(GcCpu *c, uint32_t insn) {
  const uint32_t ea = gc_ea_xu(c, insn);
  gc_psq_store(c, insn, ea, GC_F_ps_IX(insn), GC_F_ps_WX(insn));
  gc_update_ra(c, insn, ea);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */

/* The interpreter's switch takes these cases; the list lives here so that
 * adding a handler cannot be forgotten in gc_cpu.c. */
#define GC_FPU_DISPATCH_CASES                                    \
  case GC_OP_FADD: gc_op_fadd(c, insn); break;                   \
  case GC_OP_FADDS: gc_op_fadds(c, insn); break;                 \
  case GC_OP_FSUB: gc_op_fsub(c, insn); break;                   \
  case GC_OP_FSUBS: gc_op_fsubs(c, insn); break;                 \
  case GC_OP_FMUL: gc_op_fmul(c, insn); break;                   \
  case GC_OP_FMULS: gc_op_fmuls(c, insn); break;                 \
  case GC_OP_FDIV: gc_op_fdiv(c, insn); break;                   \
  case GC_OP_FDIVS: gc_op_fdivs(c, insn); break;                 \
  case GC_OP_FMADD: gc_op_fmadd(c, insn); break;                 \
  case GC_OP_FMADDS: gc_op_fmadds(c, insn); break;               \
  case GC_OP_FMSUB: gc_op_fmsub(c, insn); break;                 \
  case GC_OP_FMSUBS: gc_op_fmsubs(c, insn); break;               \
  case GC_OP_FNMADD: gc_op_fnmadd(c, insn); break;               \
  case GC_OP_FNMADDS: gc_op_fnmadds(c, insn); break;             \
  case GC_OP_FNMSUB: gc_op_fnmsub(c, insn); break;               \
  case GC_OP_FNMSUBS: gc_op_fnmsubs(c, insn); break;             \
  case GC_OP_FMR: gc_op_fmr(c, insn); break;                     \
  case GC_OP_FNEG: gc_op_fneg(c, insn); break;                   \
  case GC_OP_FABS: gc_op_fabs(c, insn); break;                   \
  case GC_OP_FNABS: gc_op_fnabs(c, insn); break;                 \
  case GC_OP_FRSP: gc_op_frsp(c, insn); break;                   \
  case GC_OP_FCTIW: gc_op_fctiw(c, insn); break;                 \
  case GC_OP_FCTIWZ: gc_op_fctiwz(c, insn); break;               \
  case GC_OP_FSEL: gc_op_fsel(c, insn); break;                   \
  case GC_OP_FRES: gc_op_fres(c, insn); break;                   \
  case GC_OP_FRSQRTE: gc_op_frsqrte(c, insn); break;             \
  case GC_OP_FCMPU: gc_op_fcmpu(c, insn); break;                 \
  case GC_OP_FCMPO: gc_op_fcmpo(c, insn); break;                 \
  case GC_OP_MFFS: gc_op_mffs(c, insn); break;                   \
  case GC_OP_MTFSF: gc_op_mtfsf(c, insn); break;                 \
  case GC_OP_MTFSFI: gc_op_mtfsfi(c, insn); break;               \
  case GC_OP_MTFSB0: gc_op_mtfsb0(c, insn); break;               \
  case GC_OP_MTFSB1: gc_op_mtfsb1(c, insn); break;               \
  case GC_OP_MCRFS: gc_op_mcrfs(c, insn); break;                 \
  case GC_OP_LFS: gc_op_lfs(c, insn); break;                     \
  case GC_OP_LFSU: gc_op_lfsu(c, insn); break;                   \
  case GC_OP_LFSX: gc_op_lfsx(c, insn); break;                   \
  case GC_OP_LFSUX: gc_op_lfsux(c, insn); break;                 \
  case GC_OP_LFD: gc_op_lfd(c, insn); break;                     \
  case GC_OP_LFDU: gc_op_lfdu(c, insn); break;                   \
  case GC_OP_LFDX: gc_op_lfdx(c, insn); break;                   \
  case GC_OP_LFDUX: gc_op_lfdux(c, insn); break;                 \
  case GC_OP_STFS: gc_op_stfs(c, insn); break;                   \
  case GC_OP_STFSU: gc_op_stfsu(c, insn); break;                 \
  case GC_OP_STFSX: gc_op_stfsx(c, insn); break;                 \
  case GC_OP_STFSUX: gc_op_stfsux(c, insn); break;               \
  case GC_OP_STFD: gc_op_stfd(c, insn); break;                   \
  case GC_OP_STFDU: gc_op_stfdu(c, insn); break;                 \
  case GC_OP_STFDX: gc_op_stfdx(c, insn); break;                 \
  case GC_OP_STFDUX: gc_op_stfdux(c, insn); break;               \
  case GC_OP_STFIWX: gc_op_stfiwx(c, insn); break;               \
  case GC_OP_PS_ADD: gc_op_ps_add(c, insn); break;               \
  case GC_OP_PS_SUB: gc_op_ps_sub(c, insn); break;               \
  case GC_OP_PS_MUL: gc_op_ps_mul(c, insn); break;               \
  case GC_OP_PS_DIV: gc_op_ps_div(c, insn); break;               \
  case GC_OP_PS_MULS0: gc_op_ps_muls0(c, insn); break;           \
  case GC_OP_PS_MULS1: gc_op_ps_muls1(c, insn); break;           \
  case GC_OP_PS_MADD: gc_op_ps_madd(c, insn); break;             \
  case GC_OP_PS_MSUB: gc_op_ps_msub(c, insn); break;             \
  case GC_OP_PS_NMADD: gc_op_ps_nmadd(c, insn); break;           \
  case GC_OP_PS_NMSUB: gc_op_ps_nmsub(c, insn); break;           \
  case GC_OP_PS_MADDS0: gc_op_ps_madds0(c, insn); break;         \
  case GC_OP_PS_MADDS1: gc_op_ps_madds1(c, insn); break;         \
  case GC_OP_PS_SUM0: gc_op_ps_sum0(c, insn); break;             \
  case GC_OP_PS_SUM1: gc_op_ps_sum1(c, insn); break;             \
  case GC_OP_PS_NEG: gc_op_ps_neg(c, insn); break;               \
  case GC_OP_PS_ABS: gc_op_ps_abs(c, insn); break;               \
  case GC_OP_PS_NABS: gc_op_ps_nabs(c, insn); break;             \
  case GC_OP_PS_MR: gc_op_ps_mr(c, insn); break;                 \
  case GC_OP_PS_MERGE00: gc_op_ps_merge00(c, insn); break;       \
  case GC_OP_PS_MERGE01: gc_op_ps_merge01(c, insn); break;       \
  case GC_OP_PS_MERGE10: gc_op_ps_merge10(c, insn); break;       \
  case GC_OP_PS_MERGE11: gc_op_ps_merge11(c, insn); break;       \
  case GC_OP_PS_SEL: gc_op_ps_sel(c, insn); break;               \
  case GC_OP_PS_RES: gc_op_ps_res(c, insn); break;               \
  case GC_OP_PS_RSQRTE: gc_op_ps_rsqrte(c, insn); break;         \
  case GC_OP_PS_CMPU0: gc_op_ps_cmpu0(c, insn); break;           \
  case GC_OP_PS_CMPO0: gc_op_ps_cmpo0(c, insn); break;           \
  case GC_OP_PS_CMPU1: gc_op_ps_cmpu1(c, insn); break;           \
  case GC_OP_PS_CMPO1: gc_op_ps_cmpo1(c, insn); break;           \
  case GC_OP_PSQ_L: gc_op_psq_l(c, insn); break;                 \
  case GC_OP_PSQ_LU: gc_op_psq_lu(c, insn); break;               \
  case GC_OP_PSQ_LX: gc_op_psq_lx(c, insn); break;               \
  case GC_OP_PSQ_LUX: gc_op_psq_lux(c, insn); break;             \
  case GC_OP_PSQ_ST: gc_op_psq_st(c, insn); break;               \
  case GC_OP_PSQ_STU: gc_op_psq_stu(c, insn); break;             \
  case GC_OP_PSQ_STX: gc_op_psq_stx(c, insn); break;             \
  case GC_OP_PSQ_STUX: gc_op_psq_stux(c, insn); break;

#ifdef __cplusplus
}
#endif

#endif /* GC_FPU_IMPL_H */
