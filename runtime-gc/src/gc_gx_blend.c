/*
 * The end of the pipeline: what happens between the texture environment
 * producing a colour and that colour reaching the framebuffer.
 *
 * Four things, in this order: the alpha test can throw the pixel away, the
 * depth test can throw it away (or already has, if the pixel format asks
 * for it early), the colour is combined with what is already there, and
 * two separate enables decide whether the colour and the alpha are written
 * at all.
 *
 * Two details that are easy to get wrong:
 *
 *   - **the blend factors are not symmetric.** The same numeric value means
 *     "the destination's colour" when it is the source's factor and "the
 *     source's colour" when it is the destination's. libogc gives the two
 *     names and the same number.
 *   - **the depth test's position depends on a bit in the pixel format
 *     register.** With early depth testing on, a pixel the alpha test would
 *     have killed has still updated the depth buffer; with it off, it has
 *     not. Games rely on both.
 *
 * The register layouts are in docs/gc-bp-registers.md sections 5 and 6.
 */

#include "gc.h"
#include "gc_gx_blend.h"

#define BP_BLEND_MODE 0x41
#define BP_DESTINATION_ALPHA 0x42
#define BP_PE_CONTROL 0x43
#define BP_ALPHA_COMPARE 0xf3

static int clamp_byte(int value) { return value < 0 ? 0 : (value > 255 ? 255 : value); }

/* The same comparison table the depth test uses. */
static int compares(unsigned function, int a, int b) {
  switch (function & 7u) {
    case 0: return 0;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return 1;
  }
}

int gc_gx_alpha_test(const GcMachine *m, int alpha) {
  const uint32_t reg = m->gx.bp[BP_ALPHA_COMPARE];
  const int first = compares((reg >> 16) & 7u, alpha, (int)(reg & 0xffu));
  const int second = compares((reg >> 19) & 7u, alpha, (int)((reg >> 8) & 0xffu));
  switch ((reg >> 22) & 3u) {
    case 0: return first && second;
    case 1: return first || second;
    case 2: return first ^ second;
    default: return !(first ^ second);
  }
}

int gc_gx_early_depth_test(const GcMachine *m) {
  return (m->gx.bp[BP_PE_CONTROL] >> 6) & 1u;
}

/* One blend factor. Which colour a factor names depends on which side it
 * is the factor for: value 2 is "the other one's colour" and 3 is one
 * minus it, which is why libogc has two names for each of those numbers. */
static int blend_factor(unsigned factor, int channel, const int source[4],
                        const int destination[4], int for_source) {
  const int *other = for_source ? destination : source;
  switch (factor & 7u) {
    case 0: return 0;
    case 1: return 255;
    case 2: return other[channel];
    case 3: return 255 - other[channel];
    case 4: return source[3];
    case 5: return 255 - source[3];
    case 6: return destination[3];
    default: return 255 - destination[3];
  }
}

static int logic_op(unsigned op, int source, int destination) {
  const int s = source & 255, d = destination & 255;
  switch (op & 15u) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return s & ~d;
    case 3: return s;
    case 4: return ~s & d;
    case 5: return d;
    case 6: return s ^ d;
    case 7: return s | d;
    case 8: return ~(s | d);
    case 9: return ~(s ^ d);
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~(s & d);
    default: return 255;
  }
}

/* A blend factor as the blender multiplies by it: 0 to 255 with its top bit
 * added, so 0 to 256, and "one" is one. The combiner scales its own
 * interpolation weight the same way (gc_gx_tev.c: c + (c >> 7)).
 *
 * This was 0 to 255 until 2026-10-06, which makes "one" 255/256: every
 * blended layer then costs the destination a level whether or not it adds
 * anything, and a scene built from many faint layers - Shrek 2's light
 * shafts are about two hundred thousand triangles a cutscene - comes out
 * several levels dark. Neither scale is measured by a test available here
 * (hwtests never blends). Dolphin's software renderer, read as a behaviour
 * reference, uses this one, and it is the one that agrees with the
 * combiner. docs/GC-PLAN.md, "The output stage". */
static int blend_scale(int factor) { return factor + (factor >> 7); }

/* The dither, which exists to hide the banding of the six-bit pixel format
 * and does nothing in the others: with eight bits a channel stored there is
 * nothing to hide, and perturbing the value would be visible error rather
 * than dither. libogc turns it on in GX_Init and most programs never turn
 * it off.
 *
 * A two-by-two ordered matrix of 0 to 3, added before the low two bits are
 * dropped. The value's own top two bits come off first: a six-bit value is
 * stored widened to eight by repeating its top two bits below it
 * (gc_efb.c), and taking them off again leaves exactly four times the
 * six-bit value - so a pixel that is already a six-bit value passes through
 * unchanged whatever the matrix adds, and blending a layer that changes
 * nothing changes nothing.
 *
 * Until 2026-10-06 this was a four-by-four matrix with an offset of -2 to
 * +1, applied to the five-six-five format as well. Its negative half took
 * four levels off half the pixels of every blended layer in a six-bit
 * buffer, on top of the blend's own loss above.
 *
 * **Unverified against hardware:** hwtests has a six-bit case and it is
 * switched off as not working. The matrix, the subtraction and the format
 * it applies to are Dolphin's software renderer's, read as a behaviour
 * reference. */
static int dithers_at_all(const GcMachine *m) {
  return (m->gx.bp[BP_PE_CONTROL] & 7u) == 1u; /* six bits a channel */
}

static int dither(int value, unsigned x, unsigned y) {
  static const int matrix[2][2] = {{0, 2}, {3, 1}};
  return ((value - (value >> 6)) + matrix[y & 1u][x & 1u]) & 0xfc;
}

void gc_gx_write_pixel(GcMachine *m, unsigned x, unsigned y, const uint8_t source[4]) {
  const uint32_t mode = m->gx.bp[BP_BLEND_MODE];
  const int blending = mode & 1u;
  const int logic = (mode >> 1) & 1u;
  const int dithering = (mode >> 2) & 1u;
  const int write_colour = (mode >> 3) & 1u;
  const int write_alpha = (mode >> 4) & 1u;
  const int subtract = (mode >> 11) & 1u;
  const uint32_t constant = m->gx.bp[BP_DESTINATION_ALPHA];
  const uint32_t existing = gc_efb_get_color(m, x, y);
  int in[4], out[4], was[4], channel;

  for (channel = 0; channel < 4; channel++) in[channel] = source[channel];
  was[0] = (int)((existing >> 16) & 0xffu);
  was[1] = (int)((existing >> 8) & 0xffu);
  was[2] = (int)(existing & 0xffu);
  was[3] = (int)((existing >> 24) & 0xffu);

  if (subtract) {
    /* Not a weighted difference: the factors are ignored entirely. */
    for (channel = 0; channel < 4; channel++) out[channel] = clamp_byte(was[channel] - in[channel]);
  } else if (blending) {
    const unsigned destination_factor = (mode >> 5) & 7u;
    const unsigned source_factor = (mode >> 8) & 7u;
    for (channel = 0; channel < 4; channel++) {
      const int s = in[channel] * blend_scale(blend_factor(source_factor, channel, in, was, 1));
      const int d = was[channel] * blend_scale(blend_factor(destination_factor, channel, in, was, 0));
      out[channel] = clamp_byte((s + d) >> 8);
    }
  } else if (logic) {
    const unsigned op = (mode >> 12) & 15u;
    for (channel = 0; channel < 4; channel++) out[channel] = logic_op(op, in[channel], was[channel]);
  } else {
    for (channel = 0; channel < 4; channel++) out[channel] = in[channel];
  }

  if (dithering && dithers_at_all(m)) {
    for (channel = 0; channel < 3; channel++) out[channel] = dither(out[channel], x, y);
  }

  /* A constant alpha replaces whatever the pipeline produced. */
  if ((constant >> 8) & 1u) out[3] = (int)(constant & 0xffu);

  /* The two enables are separate: a pass that writes only alpha, or only
   * colour, is an ordinary thing for a game to do. */
  if (!write_colour) {
    out[0] = was[0];
    out[1] = was[1];
    out[2] = was[2];
  }
  if (!write_alpha) out[3] = was[3];

  gc_efb_set_color(m, x, y,
                   ((uint32_t)out[3] << 24) | ((uint32_t)out[0] << 16) |
                       ((uint32_t)out[1] << 8) | (uint32_t)out[2]);
}
