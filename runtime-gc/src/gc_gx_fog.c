/*
 * Fog. See gc_gx_fog.h for what this is, and docs/gc-fog-indirect.md part
 * A for where every constant here comes from and which of them are not
 * settled.
 */
#include <math.h>
#include <string.h>

#include "gc.h"
#include "gc_gx.h"
#include "gc_gx_fog.h"

#define BP_FOG_RANGE 0xe8      /* enable and centre */
#define BP_FOG_RANGE_TABLE 0xe9 /* .. 0xed, two entries a register */
#define BP_FOG_A 0xee
#define BP_FOG_B_MAGNITUDE 0xef
#define BP_FOG_B_SHIFT 0xf0
#define BP_FOG_C 0xf1
#define BP_FOG_COLOR 0xf2

/* A and C are stored as the top twenty bits of the raw float word, with the
 * bottom twelve mantissa bits thrown away rather than rounded. Rebuilding
 * the float from those twenty bits is what reproduces the hardware; keeping
 * the caller's original float would drift. */
static double fog_float(uint32_t twenty_bits) {
  const uint32_t word = (twenty_bits & 0x000fffffu) << 12;
  float value;
  memcpy(&value, &word, sizeof(value));
  return (double)value;
}

/* The horizontal range adjustment: eye-space Z is the distance along the
 * view axis, and the distance to the eye is longer than that by 1/cos of
 * the horizontal angle off it. Ten table entries cover 32 pixels each,
 * mirrored about a centre that carries the same +342 bias the scissor
 * registers do. Vertical angle is not compensated, on the hardware or
 * here.
 *
 * **Unverified:** the indexing and the interpolation. That entry k stands
 * for a distance of 32(k+1) from the centre follows from the spacing the
 * SDK's table generator uses, but how the hardware splits the distance
 * into an index and a weight, what it uses inside the first 32 pixels
 * where there is no entry, and whether it interpolates at all are all
 * inference. This reads entry floor(|dx|/32) with no blending and treats
 * everything past the table as its last entry, which is the simplest rule
 * consistent with the table's shape. A guest program that fogs a flat wall
 * and reads a row of pixels would settle it. */
static double range_factor(const GcMachine *m, uint32_t x) {
  const uint32_t control = m->gx.bp[BP_FOG_RANGE];
  uint32_t centre, distance, index, entry;

  if (!((control >> 10) & 1u)) return 1.0;

  centre = control & 0x3ffu;
  /* Screen coordinates keep the 342 bias, so both sides of this are
   * biased and the difference is the real one. */
  distance = (x + 342u > centre) ? (x + 342u - centre) : (centre - x - 342u);
  index = distance >> 5;
  if (index >= 10u) index = 9u;

  /* Two twelve-bit entries to a register, low half first. */
  entry = m->gx.bp[BP_FOG_RANGE_TABLE + (index >> 1)];
  entry = (index & 1u) ? ((entry >> 12) & 0xfffu) : (entry & 0xfffu);
  /* u4.8: 256 is a factor of one. */
  return (double)entry / 256.0;
}

double gc_gx_fog_factor(const GcMachine *m, uint32_t depth, uint32_t x) {
  const uint32_t c_reg = m->gx.bp[BP_FOG_C];
  const unsigned select = (c_reg >> 21) & 7u;
  const unsigned orthographic = (c_reg >> 20) & 1u;
  const double a = fog_float(m->gx.bp[BP_FOG_A]);
  const double c = fog_float(c_reg & 0x000fffffu);
  double range, z;

  if (select == 0u) return -1.0;

  if (orthographic) {
    /* No division: A is already the whole scale. The normalised depth is
     * the 24-bit value over its full range. */
    range = a * ((double)(depth & 0x00ffffffu) / 16777215.0);
  } else {
    /* A/(B - zs), which recovers something proportional to eye-space Z.
     * The hardware does this as one 24-bit subtract and one reciprocal:
     * the B shift pre-aligns the raw depth against the raw B magnitude, so
     * the two powers of two cancel to exactly 2^24. Doing it that way
     * rather than in floats is what keeps the shift meaningful. */
    const uint32_t magnitude = m->gx.bp[BP_FOG_B_MAGNITUDE] & 0x00ffffffu;
    const unsigned shift = m->gx.bp[BP_FOG_B_SHIFT] & 0x1fu;
    const int32_t denominator = (int32_t)magnitude - (int32_t)((depth & 0x00ffffffu) >> shift);
    if (denominator == 0) {
      /* The far plane of the projection the constants were built for.
       * Saturating is the only sane answer and matches what a divider
       * that clamps would give. */
      range = (a >= 0.0) ? 1e30 : -1e30;
    } else {
      range = (a * 16777216.0) / (double)denominator;
    }
  }

  /* The range adjustment multiplies the recovered eye-space Z, before C is
   * subtracted - it is the term proportional to Z, and the final result is
   * not. On the orthographic path there is no such term, so the hardware's
   * own documentation only describes this for perspective; it is applied
   * to both here because the multiply is harmless when the table is all
   * ones, which is what GX_Init leaves. */
  range *= range_factor(m, x);
  z = range - c;

  /* **Unverified:** where fog clamps. Some clamp must exist - the
   * expression goes negative closer than the start distance and above one
   * beyond the end - but nothing read says where or at what precision. */
  if (z < 0.0) z = 0.0;
  if (z > 1.0) z = 1.0;

  switch (select) {
    case 2: return z; /* linear */
    /* **Unverified: all four exponential curves.** The SDK's header is a
     * bare list of enumerators with no prose and its source passes the
     * three-bit value straight through, so nothing establishes the base,
     * the constant 8, or where the square goes. These are the shapes every
     * reimplementation uses and they are at least self-consistent: each
     * reverse form is its forward form reflected in both axes, and all
     * four run from about 0 at z = 0 to about 1 at z = 1. If one thing in
     * the fog unit gets a hardware test, it should be this. */
    case 4: return 1.0 - pow(2.0, -8.0 * z);
    case 5: return 1.0 - pow(2.0, -8.0 * z * z);
    case 6: return pow(2.0, -8.0 * (1.0 - z));
    case 7: return pow(2.0, -8.0 * (1.0 - z) * (1.0 - z));
    default:
      /* 1 and 3 are not assigned by any enumerator the SDK defines. */
      return 0.0;
  }
}

void gc_gx_fog_apply(const GcMachine *m, uint32_t depth, uint32_t x, uint8_t rgba[4]) {
  const double f = gc_gx_fog_factor(m, depth, x);
  const uint32_t colour = m->gx.bp[BP_FOG_COLOR];
  /* Blue at 7:0, green at 15:8, red at 23:16, and no alpha field at all -
   * which is why alpha comes out of here untouched. */
  const int fog[3] = {(int)((colour >> 16) & 0xffu), (int)((colour >> 8) & 0xffu),
                      (int)(colour & 0xffu)};
  unsigned channel;

  if (f < 0.0) return;

  for (channel = 0; channel < 3u; channel++) {
    const double mixed = (double)rgba[channel] * (1.0 - f) + (double)fog[channel] * f;
    const int value = (int)(mixed + 0.5);
    rgba[channel] = (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
  }
}
