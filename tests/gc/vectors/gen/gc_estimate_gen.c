/*
 * Measure the reciprocal estimate tables.
 *
 * `fres` and `frsqrte` do not compute; they look up. The manual says so and
 * then declines to print the tables - "the value placed into register frD
 * may vary between implementations" - so the only way to have them is to
 * measure them, the same way the instruction vectors are measured
 * (docs/GC-PLAN.md, "The vector pipeline").
 *
 * The estimate is piecewise linear: the top bits of the operand's mantissa
 * choose a segment, and within a segment the result falls off linearly with
 * the bits below. This program sweeps both instructions across every
 * segment and prints what came out; tools/gc/fit_estimate_tables.py turns
 * that into the table, and checks that a linear fit reproduces every
 * measured point exactly rather than approximately.
 */

#include <stdio.h>
#include <string.h>

#include <gccore.h>

__attribute__((weak, noinline)) void OSReport(const char *fmt, ...) { (void)fmt; }

#define SEGMENTS 32
#define SAMPLES 24

static char line[256];

static void emit(const char *text) { OSReport("%s\n", text); }

static double from_bits(unsigned long long bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static unsigned long long to_bits(double value) {
  unsigned long long bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static double do_fres(double x) {
  double result;
  asm volatile("fres %0, %1" : "=f"(result) : "f"(x));
  return result;
}

static double do_frsqrte(double x) {
  double result;
  asm volatile("frsqrte %0, %1" : "=f"(result) : "f"(x));
  return result;
}

/* One sweep over the 52-bit mantissa, at a fixed exponent. */
static void sweep(const char *name, double (*op)(double), unsigned long long exponent_bits) {
  unsigned segment, sample;
  for (segment = 0; segment < SEGMENTS; segment++) {
    for (sample = 0; sample < SAMPLES; sample++) {
      /* Spread the samples across the segment, and include both ends. */
      const unsigned long long span = 1ull << 47;
      const unsigned long long low =
          (sample == SAMPLES - 1) ? (span - 1ull) : (span / SAMPLES) * sample;
      const unsigned long long mantissa = ((unsigned long long)segment << 47) | low;
      const unsigned long long input = exponent_bits | mantissa;
      const unsigned long long output = to_bits(op(from_bits(input)));
      sprintf(line, "V|%s %016llx %016llx", name, input, output);
      emit(line);
    }
  }
}

int main(void) {
  VIDEO_Init();

  sprintf(line, "V|SEGMENTS %d SAMPLES %d", SEGMENTS, SAMPLES);
  emit(line);

  /* fres takes its segment from the mantissa alone, so one exponent is
   * enough; the exponent of the result is the negated exponent of the
   * operand, which needs no table. */
  sweep("fres", do_fres, 0x3ff0000000000000ull);

  /* frsqrte's segment depends on whether the exponent is odd or even as
   * well, because halving it is part of the operation. Both parities are
   * swept. */
  sweep("frsqrte_even", do_frsqrte, 0x3ff0000000000000ull); /* 1.0 <= x < 2.0 */
  sweep("frsqrte_odd", do_frsqrte, 0x4000000000000000ull);  /* 2.0 <= x < 4.0 */

  /* A few values outside the sweep, to pin the exponent handling. */
  {
    static const unsigned long long probes[] = {
        0x3fe0000000000000ull, 0x4008000000000000ull, 0x4024000000000000ull,
        0x0010000000000000ull, 0x7fe0000000000000ull, 0xbff0000000000000ull,
        0x0000000000000001ull, 0x000fffffffffffffull,
        0x0000000000000000ull, 0x8000000000000000ull, /* +0 and -0 */
        0x7ff0000000000000ull, 0xfff0000000000000ull, /* +inf and -inf */
        0x37f0000000000000ull, 0x3810000000000000ull, /* around the single boundary */
    };
    unsigned i;
    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
      sprintf(line, "V|probe_fres %016llx %016llx", probes[i], to_bits(do_fres(from_bits(probes[i]))));
      emit(line);
      sprintf(line, "V|probe_frsqrte %016llx %016llx", probes[i],
              to_bits(do_frsqrte(from_bits(probes[i]))));
      emit(line);
    }
  }

  emit("V|END");
  SYS_ResetSystem(SYS_POWEROFF, 0, 0);
  return 0;
}
