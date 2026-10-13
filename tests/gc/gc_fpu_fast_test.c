/*
 * The floating-point fast path against the full one.
 *
 * gc_fp_binary and gc_fp_madd work out the host's exception flags from the
 * numbers when they can (gc_fpu_impl.h says when and why that is exact), and
 * otherwise call gc_fp_binary_full and gc_fp_madd_full, which ask the host.
 * This holds them to each other: the same result bits and the same FPSCR,
 * for operands at every boundary the fast path's conditions draw - zeros,
 * denormals, the exponent window's edges, twice the smallest normal single,
 * the largest single, NaNs and infinities - and for random ones, in every
 * rounding mode, with non-IEEE mode on and off, and with the host's own
 * rounding mode moved underneath.
 *
 * The rest holds the newer shortcuts to the code they replaced, which is
 * kept here as the reference: the paired arithmetic forms trying both lanes
 * at once, against both lanes the full way; FPSCR's summary bits computed
 * without branches, and a single's class read from the double's bits, against
 * the branches and the conversion; the quantised conversions without the
 * round trip through a single, against it; and psq_l/psq_st reading and
 * writing both lanes straight from the fast map, against the bus - with
 * spans that run off the end of memory, reservations held, and every GQR.
 */

#include <stdio.h>
#include <string.h>

static unsigned long fast_taken;
#define GC_FPU_FAST_TAKEN() (fast_taken++)

#include "gc_bus.h"
#include "gc_cpu.h"
#include "gc_cpu_impl.h"
#include "gc_fpu_impl.h"
#include "gc_test_bus.h"

static uint64_t state = 0x9e3779b97f4a7c15ull;

static uint64_t next(void) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

static uint64_t f64_bits(double v) {
  uint64_t b;
  memcpy(&b, &v, 8);
  return b;
}

static uint64_t single_bits(float v) { return f64_bits((double)v); }

enum { POOL = 4096 };
static uint64_t pool[POOL];
static unsigned pool_size;

static void add(uint64_t bits) {
  if (pool_size < POOL) pool[pool_size++] = bits;
}

static void with_signs(uint64_t bits) {
  add(bits & 0x7fffffffffffffffull);
  add(bits | 0x8000000000000000ull);
}

static void build_pool(void) {
  static const uint32_t exponents[] = {0x000, 0x001, 0x2ff, 0x300, 0x301, 0x380, 0x381, 0x382,
                                       0x3fe, 0x3ff, 0x400, 0x47e, 0x47f, 0x4fe, 0x4ff, 0x500,
                                       0x7fe, 0x7ff};
  unsigned i, k;
  with_signs(0);
  with_signs(0x7ff0000000000000ull);                /* infinity */
  with_signs(0x7ff8000000000000ull);                /* quiet NaN */
  with_signs(0x7ff0000000000001ull);                /* signalling NaN */
  with_signs(0x0000000000000001ull);                /* smallest denormal */
  with_signs(single_bits(1.17549435e-38f));          /* smallest normal single */
  with_signs(f64_bits(2.0 * 1.17549435082228751e-38)); /* twice it */
  with_signs(f64_bits(2.0 * 1.17549435082228751e-38) - 1u);
  with_signs(f64_bits(2.0 * 1.17549435082228751e-38) + 1u);
  with_signs(single_bits(3.40282347e38f));           /* largest single */
  with_signs(single_bits(3.40282347e38f) + 1u);
  with_signs(single_bits(1.0f));
  with_signs(single_bits(0.5f));
  with_signs(single_bits(3.0f));
  with_signs(0x3ff0000010000000ull);                  /* halfway between two singles */
  with_signs(0x3ff0000008000000ull);                  /* bit 27, for the 25-bit round */
  with_signs(0x3feffffff8000000ull);
  for (i = 0; i < sizeof(exponents) / sizeof(exponents[0]); i++) {
    for (k = 0; k < 6; k++) {
      uint64_t mantissa = next() & 0x000fffffffffffffull;
      if (k == 1) mantissa &= ~0x0000000003ffffffull;  /* multiply-exact frA */
      if (k == 2) mantissa &= ~0x000000001fffffffull;  /* a single */
      if (k == 3) mantissa = 0;
      if (k == 4) mantissa = 0x000fffffffffffffull;
      with_signs(((uint64_t)exponents[i] << 52) | mantissa);
    }
  }
  while (pool_size < POOL - 2) {
    const uint64_t r = next();
    switch (r & 3u) {
      case 0: with_signs(single_bits((float)((double)(int32_t)(uint32_t)(r >> 8) / 65536.0))); break;
      case 1: with_signs(r >> 2); break; /* any double */
      case 2: {
        /* a single in the fast window */
        const uint64_t e = 0x300u + (r >> 20) % 0x200u;
        with_signs((e << 52) | ((r >> 12) & 0x000fffffe0000000ull));
        break;
      }
      default: with_signs(single_bits((float)(r >> 40) * 0.001f)); break;
    }
  }
}

static uint64_t operand(void) {
  const uint64_t r = next();
  if (r & 1u) return pool[(r >> 8) % pool_size];
  return single_bits((float)((int64_t)(r >> 16) - (int64_t)(1ll << 47)) / (float)(1u << ((r >> 1) & 31u)));
}

static int failures;

static void compare(const char *what, const GcCpu *start, uint64_t a, uint64_t b, uint64_t cc, uint64_t fast,
                    const GcCpu *fast_cpu, uint64_t full, const GcCpu *full_cpu) {
  if (fast != full || fast_cpu->fpscr != full_cpu->fpscr) {
    if (failures < 20) {
      printf("FAIL %s: a %016llx b %016llx c %016llx fpscr %08x hid2 %08x: fast %016llx/%08x full %016llx/%08x\n",
             what, (unsigned long long)a, (unsigned long long)b, (unsigned long long)cc, start->fpscr, start->hid2,
             (unsigned long long)fast, fast_cpu->fpscr, (unsigned long long)full, full_cpu->fpscr);
    }
    failures++;
  }
}

/* ------------------------------------------------------------------ */
/* The implementations before the paired and quantised fast paths,     */
/* kept here as the reference the new ones answer to.                  */

static void ref_update_vx(GcCpu *c) {
  const uint32_t invalid = GC_FPSCR_VXSNAN | GC_FPSCR_VXISI | GC_FPSCR_VXIDI | GC_FPSCR_VXZDZ |
                           GC_FPSCR_VXIMZ | GC_FPSCR_VXVC | GC_FPSCR_VXSOFT | GC_FPSCR_VXSQRT |
                           GC_FPSCR_VXCVI;
  uint32_t fex = 0;
  if (c->fpscr & invalid) {
    c->fpscr |= GC_FPSCR_VX;
  } else {
    c->fpscr &= ~GC_FPSCR_VX;
  }
  if ((c->fpscr & GC_FPSCR_VX) && (c->fpscr & GC_FPSCR_VE)) fex = 1;
  if ((c->fpscr & GC_FPSCR_OX) && (c->fpscr & GC_FPSCR_OE)) fex = 1;
  if ((c->fpscr & GC_FPSCR_UX) && (c->fpscr & GC_FPSCR_UE)) fex = 1;
  if ((c->fpscr & GC_FPSCR_ZX) && (c->fpscr & GC_FPSCR_ZE)) fex = 1;
  if ((c->fpscr & GC_FPSCR_XX) && (c->fpscr & GC_FPSCR_XE)) fex = 1;
  if (fex) {
    c->fpscr |= GC_FPSCR_FEX;
  } else {
    c->fpscr &= ~GC_FPSCR_FEX;
  }
}

static uint32_t ref_fprf_of_single(uint64_t bits) {
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

static uint64_t ref_dequantize(uint32_t raw, unsigned type, int scale) {
  double value;
  switch (type) {
    case GC_QUANT_U8: value = (double)(uint8_t)raw; break;
    case GC_QUANT_S8: value = (double)(int8_t)(uint8_t)raw; break;
    case GC_QUANT_U16: value = (double)(uint16_t)raw; break;
    case GC_QUANT_S16: value = (double)(int16_t)(uint16_t)raw; break;
    default: return gc_single_to_double(raw);
  }
  return f64_bits((double)(float)(value * ldexp(1.0, -scale)));
}

static uint32_t ref_quantize(uint64_t bits, unsigned type, int scale) {
  double value;
  if (type == GC_QUANT_FLOAT || type < GC_QUANT_U8) {
    if ((bits & 0x7ff0000000000000ull) == 0 && (bits & 0x000fffffffffffffull) != 0) {
      return (uint32_t)((bits >> 32) & 0x80000000u);
    }
    return gc_store_single(bits);
  }
  value = gc_f64(bits) * ldexp(1.0, scale);
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

/* A paired arithmetic instruction the way it was computed before: each lane
 * the full way, then VX, the write, FPRF from ps0 and CR1. */
enum { PS_ADD, PS_SUB, PS_MUL, PS_MULS0, PS_MULS1, PS_MADD, PS_MSUB, PS_NMADD, PS_NMSUB, PS_MADDS0, PS_MADDS1,
       PS_KINDS };

static void ref_paired(GcCpu *c, unsigned kind, uint32_t insn) {
  const unsigned a = GC_F_frA(insn), b = GC_F_frB(insn), cc = GC_F_frC(insn);
  uint64_t ps0, ps1;
  switch (kind) {
    case PS_ADD:
    case PS_SUB: {
      const GcFpKind k = kind == PS_ADD ? GC_FP_ADD : GC_FP_SUB;
      ps0 = gc_fp_binary_full(c, k, c->fpr[a].ps0, c->fpr[b].ps0, 1);
      ps1 = gc_fp_binary_full(c, k, c->fpr[a].ps1, c->fpr[b].ps1, 1);
      break;
    }
    case PS_MUL:
      ps0 = gc_fp_binary_full(c, GC_FP_MUL, c->fpr[a].ps0, c->fpr[cc].ps0, 1);
      ps1 = gc_fp_binary_full(c, GC_FP_MUL, c->fpr[a].ps1, c->fpr[cc].ps1, 1);
      break;
    case PS_MULS0:
    case PS_MULS1: {
      const uint64_t scale = kind == PS_MULS0 ? c->fpr[cc].ps0 : c->fpr[cc].ps1;
      ps0 = gc_fp_binary_full(c, GC_FP_MUL, c->fpr[a].ps0, scale, 1);
      ps1 = gc_fp_binary_full(c, GC_FP_MUL, c->fpr[a].ps1, scale, 1);
      break;
    }
    default: {
      const int negate_add = kind == PS_MSUB || kind == PS_NMSUB;
      const int negate_result = kind == PS_NMADD || kind == PS_NMSUB;
      const uint64_t c0 = kind == PS_MADDS1 ? c->fpr[cc].ps1 : c->fpr[cc].ps0;
      const uint64_t c1 = kind == PS_MADDS0 ? c->fpr[cc].ps0 : c->fpr[cc].ps1;
      ps0 = gc_fp_madd_full(c, c->fpr[a].ps0, c->fpr[b].ps0, c0, negate_add, negate_result, 1);
      ps1 = gc_fp_madd_full(c, c->fpr[a].ps1, c->fpr[b].ps1, c1, negate_add, negate_result, 1);
      break;
    }
  }
  ref_update_vx(c);
  c->fpr[GC_F_frD(insn)].ps0 = ps0;
  c->fpr[GC_F_frD(insn)].ps1 = ps1;
  c->fpscr = (c->fpscr & ~GC_FPSCR_FPRF) | (ref_fprf_of_single(ps0) << 12);
  if (GC_M_Rc(insn)) gc_cr_set_field(c, 1, (c->fpscr >> 28) & 0xfu);
}

static void new_paired(GcCpu *c, unsigned kind, uint32_t insn) {
  switch (kind) {
    case PS_ADD: gc_op_ps_add(c, insn); break;
    case PS_SUB: gc_op_ps_sub(c, insn); break;
    case PS_MUL: gc_op_ps_mul(c, insn); break;
    case PS_MULS0: gc_op_ps_muls0(c, insn); break;
    case PS_MULS1: gc_op_ps_muls1(c, insn); break;
    case PS_MADD: gc_op_ps_madd(c, insn); break;
    case PS_MSUB: gc_op_ps_msub(c, insn); break;
    case PS_NMADD: gc_op_ps_nmadd(c, insn); break;
    case PS_NMSUB: gc_op_ps_nmsub(c, insn); break;
    case PS_MADDS0: gc_op_ps_madds0(c, insn); break;
    default: gc_op_ps_madds1(c, insn); break;
  }
}

/* Primary opcode 4 with frD 4, frA 1, frB 2, frC 3 and each form's XO. */
static const uint32_t paired_words[PS_KINDS] = {
    0x1081102au, 0x10811028u, 0x108100f2u, 0x108100d8u, 0x108100dau, 0x108110fau,
    0x108110f8u, 0x108110feu, 0x108110fcu, 0x108110dcu, 0x108110deu};

static const char *paired_names[PS_KINDS] = {"ps_add", "ps_sub", "ps_mul", "ps_muls0", "ps_muls1", "ps_madd",
                                             "ps_msub", "ps_nmadd", "ps_nmsub", "ps_madds0", "ps_madds1"};

/* The machine as the guest could see it: FPSCR settled first (it can be a
 * note behind, gc_fpu_impl.h), on copies so that a comparison changes
 * nothing. */
static int same_cpu(const GcCpu *x, const GcCpu *y) {
  static GcCpu sx, sy;
  sx = *x;
  sy = *y;
  gc_fp_settle(&sx);
  gc_fp_settle(&sy);
  return memcmp(sx.fpr, sy.fpr, sizeof(sx.fpr)) == 0 && sx.fpscr == sy.fpscr && sx.cr == sy.cr &&
         memcmp(sx.gpr, sy.gpr, sizeof(sx.gpr)) == 0 && sx.pending_vector == sy.pending_vector;
}

/* What reads FPSCR after a run of paired instructions: nothing (the
 * comparison settles), or an instruction that reads or changes it. */
enum { USE_NONE, USE_MFFS, USE_FCMPU, USE_FADDS, USE_PAIRED_RC, USE_MTFSB0_FI, USE_MCRFS, USE_FCTIW, USE_MERGE_RC,
       USE_PS_DIV, USES };

static void use_fpscr(GcCpu *c, unsigned use, uint32_t r) {
  switch (use) {
    case USE_MFFS: gc_op_mffs(c, 0xfca0048eu); break;                  /* mffs f5 */
    case USE_FCMPU: gc_op_fcmpu(c, 0xfc811000u); break;                /* fcmpu cr1, f1, f2 */
    case USE_FADDS: gc_op_fadds(c, 0xeca1102au | (r & 1u)); break;     /* fadds f5, f1, f2 */
    case USE_PAIRED_RC: gc_op_ps_add(c, 0x10a1102bu); break;           /* ps_add. f5, f1, f2 */
    case USE_MTFSB0_FI: gc_op_mtfsb0(c, 0xfdc0008cu); break;           /* mtfsb0 14 (FI) */
    case USE_MCRFS: gc_op_mcrfs(c, (0xfd800080u | ((r >> 8) & 7u) << 18)); break; /* mcrfs cr3, crN */
    case USE_FCTIW: gc_op_fctiw(c, 0xfca0101cu); break;                /* fctiw f5, f2 */
    case USE_MERGE_RC: gc_op_ps_merge00(c, 0x10a11421u); break;        /* ps_merge00. f5, f1, f2 */
    case USE_PS_DIV: gc_op_ps_div(c, 0x10a11024u); break;              /* ps_div f5, f1, f2 */
    default: break;
  }
}

static void test_paired(unsigned rounds) {
  static GcCpu base, one, two;
  unsigned i, n, k;
  unsigned long taken_before = fast_taken;
  static const int modes[4] = {GC_HOST_ROUND_NEAREST, GC_HOST_ROUND_ZERO, GC_HOST_ROUND_UP, GC_HOST_ROUND_DOWN};
  for (i = 0; i < rounds; i++) {
    const uint64_t r = next();
    const unsigned count = 1u + (unsigned)((r >> 58) % 3u);
    const unsigned use = (unsigned)((r >> 52) % USES);
    const int host_mode = ((r >> 40) & 15u) == 0 ? (int)((r >> 44) & 3u) : 0;
    const int use_mode = ((r >> 48) & 7u) == 0 ? (int)((r >> 61) & 3u) : 0;
    memset(&base, 0, sizeof(base));
    base.fpscr = (uint32_t)(r >> 9) & ~(GC_FPSCR_FEX | GC_FPSCR_VX | GC_FPSCR_RESERVED);
    if ((r >> 32) & 3u) base.fpscr &= ~GC_FPSCR_RN;
    if ((r >> 34) & 1u) {
      /* The steady state a running game sits in: FX and XX set, FR clear, no
       * enables - most of the time, and now and then one bit off it. */
      base.fpscr = (base.fpscr | GC_FPSCR_FX | GC_FPSCR_XX) & ~(GC_FPSCR_FR | 0xf8u);
      if (((r >> 35) & 7u) == 0) base.fpscr ^= 1u << (unsigned)((r >> 38) % 32u);
    }
    if (((r >> 50) & 7u) == 0) base.fpscr ^= GC_FPSCR_FEX | GC_FPSCR_VX; /* summary bits left stale */
    base.hid2 = GC_HID2_PSE | ((r >> 36) & 1u ? 0x08000000u : 0u);
    base.msr = ((r >> 37) & 15u) == 0 ? (((r >> 41) & 1u) ? GC_MSR_FE0 : GC_MSR_FE1) : 0u;
    base.cr = (uint32_t)(r >> 21);
    one = base;
    two = base;
    for (k = 0; k < count; k++) {
      const uint64_t q = next();
      const unsigned kind = (unsigned)((q >> 8) % PS_KINDS);
      const uint32_t insn = paired_words[kind] | (((q >> 20) & 7u) == 0 ? 1u : 0u);
      for (n = 1; n <= 3; n++) {
        const uint64_t v0 = operand(), v1 = operand();
        one.fpr[n].ps0 = two.fpr[n].ps0 = v0;
        one.fpr[n].ps1 = two.fpr[n].ps1 = v1;
      }
      if (((q >> 54) & 3u) == 0) {
        /* A product-sum that lands exactly between two singles, in one lane. */
        const double pr = gc_f64(one.fpr[1].ps0) * gc_f64(gc_force_25bit(one.fpr[3].ps0));
        one.fpr[2].ps0 = two.fpr[2].ps0 = f64_bits((double)(float)pr + ldexp(1.0, -30) - pr);
      }
      gc_host_setround(modes[host_mode]);
      new_paired(&one, kind, insn);
      gc_host_setround(modes[host_mode]);
      ref_paired(&two, kind, insn);
    }
    gc_host_setround(modes[use_mode]);
    use_fpscr(&one, use, (uint32_t)r);
    gc_host_setround(modes[use_mode]);
    use_fpscr(&two, use, (uint32_t)r);
    gc_host_setround(GC_HOST_ROUND_NEAREST);
    if (!same_cpu(&one, &two)) {
      if (failures < 20) {
        printf("FAIL paired run of %u then use %u: start fpscr %08x msr %08x hid2 %08x host %d/%d: "
               "new f4 %016llx/%016llx fpscr %08x cr %08x, reference %016llx/%016llx %08x cr %08x\n",
               count, use, base.fpscr, base.msr, base.hid2, host_mode, use_mode, (unsigned long long)one.fpr[4].ps0,
               (unsigned long long)one.fpr[4].ps1, one.fpscr, one.cr, (unsigned long long)two.fpr[4].ps0,
               (unsigned long long)two.fpr[4].ps1, two.fpscr, two.cr);
      }
      failures++;
    }
  }
  printf("paired: %u runs, %lu lanes answered by the fast path\n", rounds, fast_taken - taken_before);
  if (fast_taken - taken_before < rounds / 10u) {
    printf("FAIL: the paired fast path answered too few lanes for this to have tested it\n");
    failures++;
  }
}

static void test_summaries(unsigned rounds) {
  unsigned i;
  for (i = 0; i < rounds; i++) {
    GcCpu x, y;
    const uint64_t r = next();
    memset(&x, 0, sizeof(x));
    x.fpscr = (uint32_t)r ^ (uint32_t)(r >> 32);
    if (i < 1u << 16) x.fpscr = (i & 0x1fu) << 25 | (i >> 5 & 0x1fu) << 3 | (i >> 10 & 1u ? 0x01f80700u : 0u) |
                                (i >> 11 & 1u ? GC_FPSCR_FEX : 0u) | (i >> 12 & 1u ? GC_FPSCR_VX : 0u);
    y = x;
    gc_update_vx(&x);
    ref_update_vx(&y);
    if (x.fpscr != y.fpscr) {
      if (failures < 20) printf("FAIL update_vx: %08x gives %08x, reference %08x\n", (uint32_t)(r ^ (r >> 32)), x.fpscr, y.fpscr);
      failures++;
    }
  }
  for (i = 0; i < rounds; i++) {
    static const int modes[4] = {GC_HOST_ROUND_NEAREST, GC_HOST_ROUND_ZERO, GC_HOST_ROUND_UP, GC_HOST_ROUND_DOWN};
    const uint64_t r = next();
    uint64_t bits = (r & 1u) ? pool[(r >> 8) % pool_size] : next();
    if (((r >> 4) & 3u) == 0) bits = (bits & 0x800fffffffffffffull) | ((0x37eull + (r >> 20) % 12u) << 52);
    if (((r >> 4) & 3u) == 1) bits = (bits & 0x800fffffffffffffull) | ((0x47aull + (r >> 20) % 12u) << 52);
    if (((r >> 6) & 3u) == 0) bits &= ~0x000000001fffffffull;
    gc_host_setround(modes[(r >> 2) & 3u]);
    if (gc_fprf_of_single(bits) != ref_fprf_of_single(bits)) {
      if (failures < 20) printf("FAIL fprf of single: %016llx mode %d\n", (unsigned long long)bits, (int)((r >> 2) & 3u));
      failures++;
    }
    gc_host_setround(GC_HOST_ROUND_NEAREST);
  }
  printf("summaries: %u FPSCR updates and %u single classifications\n", rounds, rounds);
}

static void test_quantised(unsigned rounds) {
  static GcCpu base, one, two;
  unsigned i;
  unsigned long fast_loads = 0;
  for (i = 0; i < rounds; i++) {
    static const int modes[4] = {GC_HOST_ROUND_NEAREST, GC_HOST_ROUND_ZERO, GC_HOST_ROUND_UP, GC_HOST_ROUND_DOWN};
    const uint64_t r = next();
    const unsigned type = (unsigned)(r & 7u);
    const int scale = (int)((r >> 3) & 63u) - 32;
    const uint32_t raw = (uint32_t)(r >> 16);
    const uint64_t bits = ((r >> 9) & 1u) ? operand() : pool[(r >> 20) % pool_size];
    const int mode = ((r >> 48) & 7u) == 0 ? (int)((r >> 51) & 3u) : 0;
    gc_host_setround(modes[mode]);
    if (gc_dequantize_raw(raw, type, scale) != ref_dequantize(raw, type, scale)) {
      if (failures < 20) printf("FAIL dequantize: raw %08x type %u scale %d\n", raw, type, scale);
      failures++;
    }
    if (gc_quantize_raw(bits, type, scale) != ref_quantize(bits, type, scale)) {
      if (failures < 20)
        printf("FAIL quantize: %016llx type %u scale %d mode %d\n", (unsigned long long)bits, type, scale, mode);
      failures++;
    }
    gc_host_setround(GC_HOST_ROUND_NEAREST);

    /* The instructions with and without the fast map, near both ends of
     * memory so that some spans leave it. */
    {
      const unsigned form = (unsigned)((r >> 56) & 7u); /* l, lu, st, stu; the rest D-form l/st */
      const unsigned w = (unsigned)((r >> 59) & 1u);
      const uint32_t offset = (uint32_t)((r >> 12) & 0xfffu);
      const uint32_t where = ((r >> 60) & 1u) ? GC_TEST_BASE + GC_TEST_SIZE - 0x1000u + ((uint32_t)(r >> 24) & 0xfffu)
                                              : GC_TEST_BASE + ((uint32_t)(r >> 24) & 0xfffffu);
      static const uint32_t opcodes[4] = {0xe0000000u, 0xe4000000u, 0xf0000000u, 0xf4000000u};
      const uint32_t insn = opcodes[form & 3u] | (4u << 21) | (5u << 16) | (w << 15) | (((uint32_t)(r >> 61) & 7u) << 12) |
                            offset;
      unsigned k;
      memset(&base, 0, sizeof(base));
      for (k = 0; k < 8; k++) base.gqr[k] = (uint32_t)next();
      base.gpr[5] = where;
      base.fpr[4].ps0 = operand();
      base.fpr[4].ps1 = operand();
      base.reserve = ((r >> 52) & 15u) == 0;
      one = base;
      two = base;
      one.fast[0].base[GC_TEST_BASE >> 24] = gc_test_memory;
      one.fast[0].limit[GC_TEST_BASE >> 24] = GC_TEST_SIZE;
      for (k = 0; k < 64; k++) gc_test_memory[(where + offset - 16u + k - GC_TEST_BASE) & (GC_TEST_SIZE - 1u)] = (uint8_t)next();
      {
        static uint8_t before[GC_TEST_SIZE];
        static uint8_t after_one[GC_TEST_SIZE];
        uint32_t lo = (where + offset - GC_TEST_BASE) & (GC_TEST_SIZE - 1u);
        const uint32_t from = lo > 64u ? lo - 64u : 0u;
        const uint32_t to = lo + 128u < GC_TEST_SIZE ? lo + 128u : GC_TEST_SIZE;
        memcpy(before + from, gc_test_memory + from, to - from);
        switch (form & 3u) {
          case 0: gc_op_psq_l(&one, insn); break;
          case 1: gc_op_psq_lu(&one, insn); break;
          case 2: gc_op_psq_st(&one, insn); break;
          default: gc_op_psq_stu(&one, insn); break;
        }
        if (!one.pending_vector && GC_BUS_FAST_OK(&one, one.gpr[5], 1u)) fast_loads++;
        memcpy(after_one + from, gc_test_memory + from, to - from);
        memcpy(gc_test_memory + from, before + from, to - from);
        switch (form & 3u) {
          case 0: gc_op_psq_l(&two, insn); break;
          case 1: gc_op_psq_lu(&two, insn); break;
          case 2: gc_op_psq_st(&two, insn); break;
          default: gc_op_psq_stu(&two, insn); break;
        }
        one.fast[0].base[GC_TEST_BASE >> 24] = NULL;
        one.fast[0].limit[GC_TEST_BASE >> 24] = 0;
        one.dar = two.dar;
        if (!same_cpu(&one, &two) || memcmp(after_one + from, gc_test_memory + from, to - from) != 0) {
          if (failures < 20)
            printf("FAIL quantised form %u w %u at %08x+%03x gqr %08x: fast map and bus differ (vector %x/%x)\n",
                   form & 3u, w, where, offset, base.gqr[(insn >> 12) & 7u], one.pending_vector, two.pending_vector);
          failures++;
        }
      }
    }
  }
  printf("quantised: %u conversions and instructions, %lu with the fast map\n", rounds, fast_loads);
}

int main(void) {
  static GcCpu base, one, two;
  const unsigned rounds = 3000000u;
  unsigned i;
  build_pool();
  gc_test_reset_memory();
  test_summaries(1000000u);
  test_quantised(400000u);
  test_paired(1500000u);
  fast_taken = 0;
  for (i = 0; i < rounds; i++) {
    const uint64_t r = next();
    const uint64_t a = operand(), b = operand(), cc = operand();
    const int host_mode = ((r >> 40) & 15u) == 0 ? (int)((r >> 44) & 3u) : 0;
    uint64_t fast, full;
    memset(&base, 0, sizeof(base));
    /* Mostly round to nearest, the mode the fast path takes. */
    base.fpscr = (uint32_t)(r >> 8) & ~(GC_FPSCR_FEX | GC_FPSCR_VX | GC_FPSCR_RESERVED);
    if ((r >> 32) & 3u) base.fpscr &= ~GC_FPSCR_RN;
    base.hid2 = GC_HID2_PSE | ((r >> 36) & 1u ? 0x08000000u : 0u);
    gc_update_vx(&base);
    {
      static const int modes[4] = {GC_HOST_ROUND_NEAREST, GC_HOST_ROUND_ZERO, GC_HOST_ROUND_UP,
                                   GC_HOST_ROUND_DOWN};
      gc_host_setround(modes[host_mode]);
    }

    one = base;
    two = base;
    switch (r & 7u) {
      case 0:
        fast = gc_fp_binary(&one, GC_FP_ADD, a, b, 1);
        full = gc_fp_binary_full(&two, GC_FP_ADD, a, b, 1);
        compare("add", &base, a, b, 0, fast, &one, full, &two);
        break;
      case 1:
        fast = gc_fp_binary(&one, GC_FP_SUB, a, b, 1);
        full = gc_fp_binary_full(&two, GC_FP_SUB, a, b, 1);
        compare("sub", &base, a, b, 0, fast, &one, full, &two);
        break;
      case 2:
      case 3:
        fast = gc_fp_binary(&one, GC_FP_MUL, a, b, 1);
        full = gc_fp_binary_full(&two, GC_FP_MUL, a, b, 1);
        compare("mul", &base, a, b, 0, fast, &one, full, &two);
        break;
      case 4:
        fast = gc_fp_binary(&one, GC_FP_ADD, a, b, 0);
        full = gc_fp_binary_full(&two, GC_FP_ADD, a, b, 0);
        compare("add double", &base, a, b, 0, fast, &one, full, &two);
        break;
      default: {
        const int negate_add = (r >> 3) & 1u, negate_result = (r >> 4) & 1u;
        /* Now and then a product-sum that lands exactly between two singles. */
        uint64_t addend = b;
        if (((r >> 50) & 7u) == 0) {
          const double p = gc_f64(a) * gc_f64(gc_force_25bit(cc));
          addend = f64_bits((double)(float)p + ldexp(1.0, -30) - p);
          if (negate_add) addend ^= 0x8000000000000000ull;
        }
        fast = gc_fp_madd(&one, a, addend, cc, negate_add, negate_result, 1);
        full = gc_fp_madd_full(&two, a, addend, cc, negate_add, negate_result, 1);
        compare("madd", &base, a, addend, cc, fast, &one, full, &two);
        break;
      }
    }
    gc_host_setround(GC_HOST_ROUND_NEAREST);
  }
  printf("gc_fpu_fast_test: %u cases, %lu answered by the fast path, %d failures\n", rounds, fast_taken,
         failures);
  if (fast_taken < rounds / 10u) {
    printf("FAIL: the fast path answered too few cases for this to have tested it\n");
    return 1;
  }
  return failures ? 1 : 0;
}
