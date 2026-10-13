/*
 * The texture environment: sixteen stages of a fixed-function combiner that
 * decide what colour a pixel ends up.
 *
 * Every stage computes the same shape - interpolate between two inputs by a
 * third, add a fourth - and the whole difficulty is that the hardware does
 * it with integers in a particular order, with three constants nobody would
 * guess. The full derivation is in docs/gc-tev.md, measured by
 * dolphin-emu/hwtests' tev program; the three that catch implementations
 * out are worth repeating here because the code below looks wrong without
 * them:
 *
 *   - the interpolation weight is widened as `c + (c >> 7)`, so it spans
 *     0 to 256 and *skips* 128. There is no weight of exactly one half, and
 *     the built-in "half" constant is a weight of 129/256.
 *   - subtract rounds with 127, not 128, and rounds *before* it negates, so
 *     it is not the mirror image of add.
 *   - divide-by-two is not "scale by one, then halve": it also switches the
 *     rounding bias off entirely, and its final shift is arithmetic, so a
 *     negative result floors towards minus infinity.
 *
 * And one asymmetry that matters more than any of them: a, b and c are
 * truncated to eight bits, while d keeps the register's whole signed
 * eleven. The same register read as d and as a gives two different numbers,
 * and both are right.
 */

#include <string.h>

#include "gc.h"
#include "gc_gx_tev.h"
#include "gc_gx_indirect.h"
#include "gc_gx_texture.h"
#include "gc_gx_tev_plan.h"

/* The per-pixel helpers below are called a channel a stage a pixel, and
 * MSVC left them as calls even with whole-program optimisation. */
#if defined(_MSC_VER)
#define GC_TEV_INLINE static __forceinline
#elif defined(__GNUC__)
#define GC_TEV_INLINE static inline __attribute__((always_inline))
#else
#define GC_TEV_INLINE static inline
#endif

#define BP_GEN_MODE 0x00
#define BP_TEV_ORDER 0x28
#define BP_TEV_COLOR 0xc0 /* + 2n */
#define BP_TEV_ALPHA 0xc1 /* + 2n */
#define BP_TEV_REGISTER 0xe0
#define BP_TEV_KSEL 0xf6
#define BP_INDIRECT_COMMAND 0x10 /* .. 0x1f, one a stage */
#define BP_INDIRECT_ORDER 0x27

/* ------------------------------------------------------------------ */
/* The registers a stage can write, and read back                      */

static int clamp_range(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

/* The eight constants the konstant selector's first values produce. They
 * are round-half-up of 255 * (8 - n) / 8, which makes one half 128 and not
 * 127 - the one gap of 31 in an otherwise even descent of 32. */
static int konstant_fraction(unsigned n) { return (int)((255u * (8u - (n & 7u)) + 4u) / 8u); }

static void konstant_colour(const GcGxTev *tev, unsigned selector, int out[3]) {
  unsigned i;
  if (selector < 8u) {
    for (i = 0; i < 3; i++) out[i] = konstant_fraction(selector);
    return;
  }
  if (selector >= 12u && selector < 16u) {
    /* A whole konstant register's three channels. */
    const unsigned reg = selector - 12u;
    for (i = 0; i < 3; i++) out[i] = tev->konstant[reg][i];
    return;
  }
  if (selector >= 16u) {
    /* One channel of one konstant register, in every channel. The sixteen
     * values are channel-major and register-minor - the register is the low
     * two bits and the channel the next two, so 16 is k0.r, 17 is k1.r and
     * 28 is k0.a (docs/gc-tev.md 6.4, confirmed on hardware by loading each
     * register with different values and reading all sixteen back). Taking
     * the register from the high bits instead reads a different register's
     * different channel for every selector but two, which is how the
     * swamp's water came out fully transparent: the game asks for k0.a and
     * got k3.r, which was zero, and the alpha test threw every pixel away. */
    const unsigned reg = (selector - 16u) & 3u;
    const unsigned channel = (selector - 16u) >> 2;
    for (i = 0; i < 3; i++) out[i] = tev->konstant[reg][channel & 3u];
    return;
  }
  for (i = 0; i < 3; i++) out[i] = 0; /* 8 to 11 are not valid selections */
}

static int konstant_alpha(const GcGxTev *tev, unsigned selector) {
  if (selector < 8u) return konstant_fraction(selector);
  if (selector >= 16u) {
    /* Register in the low two bits, channel in the next two (above). */
    const unsigned reg = (selector - 16u) & 3u;
    const unsigned channel = (selector - 16u) >> 2;
    return tev->konstant[reg][channel & 3u];
  }
  return 0;
}

/* One stage's konstant, colour and alpha together. It depends on nothing
 * but register state, which is what lets the OpenGL path compute it once a
 * draw instead of once a pixel. */
void gc_gx_tev_konstant(const GcMachine *m, unsigned stage, int out[4]) {
  const GcGxTev *tev = &m->gx.tev;
  konstant_colour(tev, tev->konstant_color[stage & 15u], out);
  out[3] = konstant_alpha(tev, tev->konstant_alpha[stage & 15u]);
}

/* One of the sixteen colour inputs. The register selectors are the only
 * ones that can be outside 0 to 255. */
GC_TEV_INLINE void colour_input(const GcGxTev *tev, const GcGxTevPixel *pixel, unsigned selector,
                         int out[3]) {
  unsigned i;
  if (selector < 8u) {
    const unsigned reg = selector >> 1;
    if (selector & 1u) {
      for (i = 0; i < 3; i++) out[i] = tev->reg[reg][3]; /* alpha, in every channel */
    } else {
      for (i = 0; i < 3; i++) out[i] = tev->reg[reg][i];
    }
    return;
  }
  switch (selector) {
    case 8: for (i = 0; i < 3; i++) out[i] = pixel->texture[i]; return;
    case 9: for (i = 0; i < 3; i++) out[i] = pixel->texture[3]; return;
    case 10: for (i = 0; i < 3; i++) out[i] = pixel->raster[i]; return;
    case 11: for (i = 0; i < 3; i++) out[i] = pixel->raster[3]; return;
    case 12: for (i = 0; i < 3; i++) out[i] = 255; return;
    case 13: for (i = 0; i < 3; i++) out[i] = 128; return;
    case 14: konstant_colour(tev, pixel->konstant_color, out); return;
    default: for (i = 0; i < 3; i++) out[i] = 0; return;
  }
}

GC_TEV_INLINE int alpha_input(const GcGxTev *tev, const GcGxTevPixel *pixel, unsigned selector) {
  switch (selector & 7u) {
    case 0: return tev->reg[0][3];
    case 1: return tev->reg[1][3];
    case 2: return tev->reg[2][3];
    case 3: return tev->reg[3][3];
    case 4: return pixel->texture[3];
    case 5: return pixel->raster[3];
    case 6: return konstant_alpha(tev, pixel->konstant_alpha);
    default: return 0;
  }
}

/* ------------------------------------------------------------------ */
/* One channel of one stage                                            */

/* The arithmetic of docs/gc-tev.md section 2.3, step by numbered step. */
GC_TEV_INLINE int combine(int a, int b, int c, int d, unsigned bias, int subtract, unsigned scale,
                   int clamp) {
  static const int bias_term[4] = {0, 128, -128, 0};
  const unsigned left = (scale == 3u) ? 0u : scale;      /* x1, x2, x4; divide does not shift up */
  const unsigned right = (scale == 3u) ? 1u : 0u;        /* only divide shifts down */
  const int rounding = (scale == 3u) ? 0 : (subtract ? 127 : 128);
  int weight, interpolated, result;

  a &= 255;
  b &= 255;
  c &= 255;
  weight = c + (c >> 7); /* 0 to 256, skipping 128 */

  interpolated = ((a * (256 - weight) + b * weight) << left) + rounding;
  interpolated >>= 8;

  result = (d << left) + (subtract ? -interpolated : interpolated) + (bias_term[bias & 3] << left);
  /* An arithmetic shift, which floors: -173 halves to -87, not -86. */
  result >>= right;

  return clamp ? clamp_range(result, 0, 255) : clamp_range(result, -1024, 1023);
}

/* Bias 3 is not a bias at all: the stage is a comparison. */
GC_TEV_INLINE int compare(int a, int b, int c, int d, unsigned mode, int equal) {
  int left = a, right = b;
  switch (mode) {
    case 0: break;                                 /* eight bits, per channel */
    case 1: left = a & 255; right = b & 255; break; /* sixteen bits, assembled by the caller */
    case 2: break;
    default: break;
  }
  if (equal) return d + ((left == right) ? c : 0);
  return d + ((left > right) ? c : 0);
}

/* ------------------------------------------------------------------ */
/* What the command stream loads                                       */

/* Eleven bits, signed, out of a register half. */
static int signed11(uint32_t value) {
  const int raw = (int)(value & 0x7ffu);
  return (raw & 0x400) ? (raw - 0x800) : raw;
}

int gc_gx_tev_write(GcMachine *m, unsigned reg, uint32_t payload) {
  GcGxTev *tev = &m->gx.tev;

  /* The colour and konstant registers share eight numbers; bit 23 says
   * which file, and red travels with alpha while blue travels with
   * green. */
  if (reg >= BP_TEV_REGISTER && reg <= BP_TEV_REGISTER + 7u) {
    const unsigned which = (reg - BP_TEV_REGISTER) >> 1;
    const int is_konstant = (payload >> 23) & 1;
    int *target = is_konstant ? tev->konstant[which] : tev->reg[which];
    if ((reg - BP_TEV_REGISTER) & 1u) {
      target[2] = signed11(payload);        /* blue */
      target[1] = signed11(payload >> 12);  /* green */
    } else {
      target[0] = signed11(payload);        /* red */
      target[3] = signed11(payload >> 12);  /* alpha */
    }
    return 1;
  }

  /* One register file doing two jobs: four bits of a swap table, and the
   * konstant selections for two stages. */
  if (reg >= BP_TEV_KSEL && reg <= BP_TEV_KSEL + 7u) {
    const unsigned k = reg - BP_TEV_KSEL;
    const unsigned table = k >> 1;
    if (k & 1u) {
      tev->swap[table][2] = (uint8_t)(payload & 3u);        /* blue */
      tev->swap[table][3] = (uint8_t)((payload >> 2) & 3u); /* alpha */
    } else {
      tev->swap[table][0] = (uint8_t)(payload & 3u);        /* red */
      tev->swap[table][1] = (uint8_t)((payload >> 2) & 3u); /* green */
    }
    tev->konstant_color[2u * k] = (uint8_t)((payload >> 4) & 0x1fu);
    tev->konstant_alpha[2u * k] = (uint8_t)((payload >> 9) & 0x1fu);
    tev->konstant_color[2u * k + 1u] = (uint8_t)((payload >> 14) & 0x1fu);
    tev->konstant_alpha[2u * k + 1u] = (uint8_t)((payload >> 19) & 0x1fu);
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */

void gc_gx_tev_reset(GcGxTev *tev) {
  unsigned i;
  memset(tev, 0, sizeof(*tev));
  /* A swap table that has never been written passes the channels through,
   * which is what an untouched table does on hardware. */
  for (i = 0; i < 4; i++) {
    tev->swap[i][0] = 0;
    tev->swap[i][1] = 1;
    tev->swap[i][2] = 2;
    tev->swap[i][3] = 3;
  }
}

/* A stage's swap table turns four two-bit fields into a channel order. */
GC_TEV_INLINE void swap_channels(const GcGxTev *tev, unsigned table, const int in[4], int out[4]) {
  unsigned i;
  for (i = 0; i < 4; i++) out[i] = in[tev->swap[table & 3u][i] & 3u];
}

/* Where the pixel's texture coordinates came out. Kept here rather than
 * passed through the combiner's own structure because a stage picks one of
 * them by register, not by position. */
static float tev_coordinate[8][2];
static float tev_gradient[8][4];

void gc_gx_tev_set_texture(GcMachine *m, const float coordinate[8][2],
                           const float gradient[8][4]) {
  (void)m;
  memcpy(tev_coordinate, coordinate, sizeof(tev_coordinate));
  memcpy(tev_gradient, gradient, sizeof(tev_gradient));
}

/* One stage's order register: which texture map, which coordinate, which
 * colour channel, and whether the texture is used at all. Two stages share
 * a register, the odd one twelve bits up. */
static uint32_t stage_order(const GcGx *gx, unsigned stage) {
  const uint32_t reg = gx->bp[BP_TEV_ORDER + (stage >> 1)];
  return (stage & 1u) ? (reg >> 12) : reg;
}

void gc_gx_tev_prepare(GcMachine *m, GcGxTevPlan *plan) {
  GcGx *gx = &m->gx;
  const unsigned stages = ((gx->bp[BP_GEN_MODE] >> 10) & 0xfu) + 1u;
  unsigned stage;

  plan->stages = stages < 16u ? stages : 16u;
  plan->coordinates_used = 0;
  plan->gradients_used = 0;
  for (stage = 0; stage < plan->stages; stage++) {
    GcGxTevStagePlan *p = &plan->stage[stage];
    const uint32_t order = stage_order(gx, stage);
    p->colour_env = gx->bp[BP_TEV_COLOR + 2u * stage];
    p->alpha_env = gx->bp[BP_TEV_ALPHA + 2u * stage];
    p->textured = (order >> 6) & 1u;
    p->map = order & 7u;
    p->coordinate = (order >> 3) & 7u;
    p->indirect = 0;
    if (p->textured) {
      const uint32_t command = gx->bp[BP_INDIRECT_COMMAND + stage];
      gc_gx_texcoord_scale(m, p->coordinate, p->map, p->scale);
      gc_gx_texture_bind(m, p->map, stage, &p->texture);
      plan->coordinates_used |= 1u << p->coordinate;
      if (p->texture.texture.max_lod > p->texture.texture.min_lod) plan->gradients_used |= 1u << p->coordinate;
      /* gc_gx_indirect_coordinate does nothing at all for a matrix index of
       * zero. Otherwise it may read the generated set its indirect stage
       * is ordered to use. */
      if (((command >> 9) & 3u) != 0u) {
        const unsigned indirect_stage = command & 3u;
        const uint32_t indirect_order = gx->bp[BP_INDIRECT_ORDER];
        p->indirect = 1;
        plan->coordinates_used |= 1u << ((indirect_order >> (indirect_stage * 6u + 3u)) & 7u);
      }
    }
  }
}

int gc_gx_tev_plan_independent(const GcGxTevPlan *plan) {
  /* Two groups a register: its colour channels, which a colour stage
   * writes together, and its alpha. Bit 2k is register k's colour, bit
   * 2k+1 its alpha. A stage reads its colour inputs, writes its colour
   * destination, then reads its alpha inputs and writes its alpha
   * destination. */
  unsigned ever_written = 0, written = 0, stage, i;
  for (stage = 0; stage < plan->stages; stage++) {
    ever_written |= 1u << (2u * ((plan->stage[stage].colour_env >> 22) & 3u));
    ever_written |= 2u << (2u * ((plan->stage[stage].alpha_env >> 22) & 3u));
  }
  for (stage = 0; stage < plan->stages; stage++) {
    const uint32_t colour_env = plan->stage[stage].colour_env;
    const uint32_t alpha_env = plan->stage[stage].alpha_env;
    unsigned reads = 0;
    for (i = 0; i < 4u; i++) {
      const unsigned selector = (colour_env >> (12u - 4u * i)) & 0xfu;
      if (selector < 8u) reads |= ((selector & 1u) ? 2u : 1u) << (2u * (selector >> 1));
    }
    if (reads & ever_written & ~written) return 0;
    written |= 1u << (2u * ((colour_env >> 22) & 3u));
    reads = 0;
    for (i = 0; i < 4u; i++) {
      const unsigned selector = (alpha_env >> (13u - 3u * i)) & 7u;
      if (selector < 4u) reads |= 2u << (2u * selector);
    }
    if (reads & ever_written & ~written) return 0;
    written |= 2u << (2u * ((alpha_env >> 22) & 3u));
  }
  return 1;
}

void gc_gx_tev_run_planned(GcMachine *m, GcGxTevPlan *plan, GcGxTev *tev,
                           const float coordinate[8][2], const float gradient_in[8][4],
                           GcGxTevPixel *pixel) {
  unsigned stage;

  /* The registers a stage reads are the ones the command stream loaded;
   * `prev` is register zero and is also where a stage writes by default. */
  for (stage = 0; stage < plan->stages; stage++) {
    GcGxTevStagePlan *p = &plan->stage[stage];
    const uint32_t colour_env = p->colour_env;
    const uint32_t alpha_env = p->alpha_env;
    const unsigned destination = (colour_env >> 22) & 3u;
    const unsigned alpha_destination = (alpha_env >> 22) & 3u;
    const unsigned raster_swap = alpha_env & 3u;
    const unsigned texture_swap = (alpha_env >> 2) & 3u;
    GcGxTevPixel swapped = *pixel;
    int a[3], b[3], c[3], d[3];
    int channel;

    /* The stage's own texture sample, if it uses one. */
    if (p->textured) {
      const unsigned n = p->coordinate;
      uint8_t texel[4];
      float normal[2], gradient[4], sample_at[2];
      unsigned i;
      /* From the transform's units into this map's nought-to-one range,
       * through the coordinate's own scale registers. */
      normal[0] = coordinate[n][0] * p->scale[0];
      normal[1] = coordinate[n][1] * p->scale[1];
      gradient[0] = gradient_in[n][0] * p->scale[0];
      gradient[1] = gradient_in[n][1] * p->scale[1];
      gradient[2] = gradient_in[n][2] * p->scale[0];
      gradient[3] = gradient_in[n][3] * p->scale[1];
      /* An indirect stage, if this one names one, perturbs the
       * coordinate before the map is read. With no indirection - which
       * is what GX_Init leaves everywhere - the coordinate is used as it
       * is. */
      if (p->indirect) {
        gc_gx_indirect_coordinate(m, stage, p->map, coordinate, normal, sample_at);
      } else {
        sample_at[0] = normal[0];
        sample_at[1] = normal[1];
      }
      /* The gradient is the one the generator produced, not the one an
       * indirect offset left behind: the perturbed coordinate's own rate
       * of change is not tracked. The hardware has a bit for choosing
       * between the two and this is the side it defaults away from, so
       * an indirect stage picks a slightly sharper level than it should. */
      gc_gx_texture_sample_bound(m, &p->texture, sample_at[0], sample_at[1], gradient, texel);
      for (i = 0; i < 4; i++) pixel->texture[i] = texel[i];
    }

    swap_channels(tev, raster_swap, pixel->raster, swapped.raster);
    swap_channels(tev, texture_swap, pixel->texture, swapped.texture);
    swapped.konstant_color = tev->konstant_color[stage];
    swapped.konstant_alpha = tev->konstant_alpha[stage];

    colour_input(tev, &swapped, (colour_env >> 12) & 0xfu, a);
    colour_input(tev, &swapped, (colour_env >> 8) & 0xfu, b);
    colour_input(tev, &swapped, (colour_env >> 4) & 0xfu, c);
    colour_input(tev, &swapped, colour_env & 0xfu, d);

    {
      const unsigned bias = (colour_env >> 16) & 3u;
      const int subtract = (colour_env >> 18) & 1;
      const int clamp = (colour_env >> 19) & 1;
      const unsigned scale = (colour_env >> 20) & 3u;
      int result[3];
      for (channel = 0; channel < 3; channel++) {
        result[channel] = (bias == 3u)
                              ? compare(a[channel], b[channel], c[channel], d[channel], scale,
                                        subtract)
                              : combine(a[channel], b[channel], c[channel], d[channel], bias,
                                        subtract, scale, clamp);
      }
      for (channel = 0; channel < 3; channel++) tev->reg[destination][channel] = result[channel];
    }

    {
      const unsigned bias = (alpha_env >> 16) & 3u;
      const int subtract = (alpha_env >> 18) & 1;
      const int clamp = (alpha_env >> 19) & 1;
      const unsigned scale = (alpha_env >> 20) & 3u;
      const int aa = alpha_input(tev, &swapped, (alpha_env >> 13) & 7u);
      const int ab = alpha_input(tev, &swapped, (alpha_env >> 10) & 7u);
      const int ac = alpha_input(tev, &swapped, (alpha_env >> 7) & 7u);
      const int ad = alpha_input(tev, &swapped, (alpha_env >> 4) & 7u);
      tev->reg[alpha_destination][3] =
          (bias == 3u) ? compare(aa, ab, ac, ad, scale, subtract)
                       : combine(aa, ab, ac, ad, bias, subtract, scale, clamp);
    }
  }

  /* What reaches the pixel engine is register zero, narrowed to eight
   * bits - and the narrowing is a *wrap*, not a clamp. The extra range only
   * ever existed to be carried between stages, and a stage that wanted its
   * result clamped has a bit for that. The hardware test relies on the
   * wrap: it reads an eleven-bit value back through an eight-bit
   * framebuffer by halving it in three extra stages, which only works if
   * -128 arrives as 128. */
  for (stage = 0; stage < 4u; stage++) {
    pixel->result[stage] = (uint8_t)(tev->reg[0][stage] & 255);
  }
}

void gc_gx_tev_run(GcMachine *m, GcGxTevPixel *pixel) {
  /* On the heap of this file rather than the stack: a plan binds up to
   * sixteen textures and is several kilobytes. */
  static GcGxTevPlan plan;
  gc_gx_tev_prepare(m, &plan);
  gc_gx_tev_run_planned(m, &plan, &m->gx.tev, (const float(*)[2])tev_coordinate,
                        (const float(*)[4])tev_gradient, pixel);
}
