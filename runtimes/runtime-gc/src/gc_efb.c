/*
 * The embedded framebuffer and the copy out of it.
 *
 * The register layouts are in docs/gc-bp-registers.md, derived from
 * libogc's GX (zlib). The order of the copy's steps, and the two places it
 * does something other than the obvious thing, are from
 * dolphin-emu/hwtests' copyfilter and intensity programs - which is to say
 * from somebody who measured them on a console:
 *
 *   - the weighted sum is shifted down by six and then *masked to nine
 *     bits* before it is clamped, so a large enough weight sum wraps round
 *     to a dark value instead of saturating to white
 *   - alpha does not go through the filter or the gamma at all; it comes
 *     straight from the row being copied
 *
 * Both are recorded in docs/gc-gxtest-survey-pixel.md with the reasoning.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_gl.h"
#include "gc_efb.h"
#include "gc_gx_thread.h"
#include "gc_gx_workers.h"
#include "gc_memory.h"

/* Helpers called once a pixel or once a channel, which MSVC otherwise left
 * as calls. */
#if defined(_MSC_VER)
#define GC_EFB_INLINE static __forceinline
#elif defined(__GNUC__)
#define GC_EFB_INLINE static inline __attribute__((always_inline))
#else
#define GC_EFB_INLINE static inline
#endif

/* The copy's registers. */
#define BP_PE_CONTROL 0x43
#define BP_EFB_TOP_LEFT 0x49
#define BP_EFB_SIZE 0x4a
#define BP_EFB_ADDRESS 0x4b
#define BP_EFB_STRIDE 0x4d
#define BP_COPY_CLEAR_AR 0x4f
#define BP_COPY_CLEAR_GB 0x50
#define BP_COPY_CLEAR_Z 0x51
#define BP_COPY_FILTER_LOW 0x53
#define BP_COPY_FILTER_HIGH 0x54

/* The copy trigger's fields. */
#define COPY_CLAMP_TOP 0x000001u
#define COPY_CLAMP_BOTTOM 0x000002u
/* Halve the picture on the way out, each way.
 *
 * A game copying the whole screen into a texture it will only ever draw
 * small does not want a 640-wide texture, and the hardware will downsample
 * for it: every pixel written is four of the rectangle's, two across and two
 * down. The destination is half as wide, which is why the pitch register on
 * such a copy reads as half what the rectangle would suggest - and a machine
 * that ignores the bit writes twice as many pixels into each row as belong
 * there, so every row overruns into the next and the picture comes out as
 * bands.
 *
 * It is half as tall too, and for a long time this halved only the width:
 * twice the rows the game had made room for, written over whatever its heap
 * kept after the texture. Super Mario Galaxy 2's pause screen is the first
 * thing that asked (320 by 228 into a 160 by 114 texture); the three
 * textures after that one went, and the game stopped in its own exception
 * handler a dozen fields later (docs/WII-PLAN.md). */
#define COPY_HALF_SCALE 0x000200u
#define COPY_CLEAR 0x000800u
#define COPY_TO_XFB 0x004000u
#define COPY_INTENSITY 0x008000u
#define COPY_AUTO_CONVERT 0x010000u

/* The destination format the four test programs, and most games, use. */
/* What a copy writes into memory.
 *
 * These are the *copy's* format numbers, which overlap the texture formats
 * without being them: a copy can also write single channels, which no
 * texture format does, and the compressed format is not among them because
 * nothing compresses on the way out.
 *
 * The tiling is the texture sampler's, because the whole point of a copy to
 * memory is that a later draw samples it: 4x4 tiles of 32 bytes for the
 * sixteen-bit formats, two of those for the thirty-two-bit one, 8x4 for the
 * eight-bit ones and 8x8 for the four-bit ones. */
#define COPY_FORMAT_R4 0x0u
#define COPY_FORMAT_R8_LOW 0x1u
#define COPY_FORMAT_RA4 0x2u
#define COPY_FORMAT_RA8 0x3u
#define COPY_FORMAT_RGB565 0x4u
#define COPY_FORMAT_RGB5A3 0x5u
#define COPY_FORMAT_RGBA8 0x6u
#define COPY_FORMAT_A8 0x7u
#define COPY_FORMAT_R8 0x8u
#define COPY_FORMAT_G8 0x9u
#define COPY_FORMAT_B8 0xau
#define COPY_FORMAT_RG8 0xbu
#define COPY_FORMAT_GB8 0xcu

int gc_efb_create(GcEfb *efb) {
  const size_t pixels = (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT;
  efb->color = (uint32_t *)calloc(pixels, sizeof(uint32_t));
  efb->depth = (uint32_t *)calloc(pixels, sizeof(uint32_t));
  efb->copies = 0;
  efb->unsupported_copies = 0;
  return (efb->color && efb->depth) ? 0 : -1;
}

void gc_efb_destroy(GcEfb *efb) {
  free(efb->color);
  free(efb->depth);
  efb->color = NULL;
  efb->depth = NULL;
}

void gc_efb_clear(GcEfb *efb) {
  const size_t pixels = (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT;
  if (efb->color) memset(efb->color, 0, pixels * sizeof(uint32_t));
  if (efb->depth) memset(efb->depth, 0, pixels * sizeof(uint32_t));
  efb->copies = 0;
  efb->unsupported_copies = 0;
}

/* ------------------------------------------------------------------ */
/* How many bits the EFB really holds                                  */

/* Five and six bit channels come back with their top bits repeated into
 * the bottom, which is what makes the largest value 255 rather than 248. */
static uint8_t six_bit(uint8_t v) { return (uint8_t)((v & 0xfcu) | ((v & 0xc0u) >> 6)); }
static uint8_t five_bit(uint8_t v) { return (uint8_t)((v & 0xf8u) | ((v & 0xe0u) >> 5)); }

static unsigned efb_format(const GcMachine *m) { return m->gx.bp[BP_PE_CONTROL] & 7u; }

/* A colour as the framebuffer will hold it once it has been written. */
GC_EFB_INLINE uint32_t quantise(unsigned format, uint32_t argb) {
  const uint8_t a = (uint8_t)(argb >> 24), r = (uint8_t)(argb >> 16);
  const uint8_t g = (uint8_t)(argb >> 8), b = (uint8_t)argb;
  switch (format) {
    case GC_EFB_RGBA6_Z24:
      return ((uint32_t)six_bit(a) << 24) | ((uint32_t)six_bit(r) << 16) |
             ((uint32_t)six_bit(g) << 8) | six_bit(b);
    case GC_EFB_RGB565_Z16:
      return 0xff000000u | ((uint32_t)five_bit(r) << 16) | ((uint32_t)six_bit(g) << 8) |
             five_bit(b);
    default:
      /* Eight bits a channel, and no alpha to store. */
      return 0xff000000u | (argb & 0x00ffffffu);
  }
}

void gc_efb_set_color(GcMachine *m, uint32_t x, uint32_t y, uint32_t argb) {
  if (x >= GC_EFB_WIDTH || y >= GC_EFB_HEIGHT) return;
  m->efb.color[(size_t)y * GC_EFB_WIDTH + x] = quantise(efb_format(m), argb);
}

uint32_t gc_efb_get_color(const GcMachine *m, uint32_t x, uint32_t y) {
  if (x >= GC_EFB_WIDTH || y >= GC_EFB_HEIGHT) return 0;
  return m->efb.color[(size_t)y * GC_EFB_WIDTH + x];
}

/* ------------------------------------------------------------------ */
/* The address window                                                  */

/* x is at bits 2 to 11, y at bits 12 to 21, and bit 22 chooses the plane.
 * A coordinate outside the real framebuffer is not an error - the window
 * is 1024 square and the framebuffer is not - but there is nothing there. */
static uint32_t *pixel_at(GcMachine *m, uint32_t physical, int *is_depth) {
  const uint32_t offset = physical - GC_PHYS_EFB;
  const uint32_t x = (offset >> 2) & 0x3ffu;
  const uint32_t y = (offset >> 12) & 0x3ffu;
  *is_depth = (offset >> 22) & 1;
  if (x >= GC_EFB_WIDTH || y >= GC_EFB_HEIGHT) return NULL;
  return (*is_depth ? m->efb.depth : m->efb.color) + (size_t)y * GC_EFB_WIDTH + x;
}

int gc_efb_read(GcMachine *m, uint32_t physical, unsigned size, uint32_t *value) {
  int is_depth;
  const uint32_t *p;
  if (physical < GC_PHYS_EFB || physical >= GC_PHYS_EFB + GC_EFB_WINDOW) return 0;
  /* A read of the framebuffer window has to see what the card drew - and
   * only a read of the window: the bus asks here on every access that is
   * not main memory, and bringing the framebuffer back for each of those
   * cost the OpenGL path most of its time. Only the pixel read comes back
   * (gc_gl_fetch_pixel), after whatever a graphics thread still has to draw
   * (the planes are the same buffers). */
  {
    const uint32_t offset = physical - GC_PHYS_EFB;
    const uint32_t x = (offset >> 2) & 0x3ffu, y = (offset >> 12) & 0x3ffu;
    const int depth = (int)((offset >> 22) & 1u);
    /* The fast OpenGL path answers from the frame before, with nothing
     * waited for, when it can (gc_gl_peek_late). Otherwise a graphics thread
     * draws everything queued first, with or without the OpenGL path (the
     * planes are shared, so the pixel is read in place). */
    if (!gc_gl_peek_late(m, x, y, depth)) {
      m = gc_gx_thread_target(m);
      gc_gl_fetch_pixel(m, x, y, depth);
    }
  }
  p = pixel_at(m, physical, &is_depth);
  *value = p ? *p : 0u;
  if (size < 4u) *value >>= 8u * (4u - size); /* the window is read as words */
  return 1;
}

int gc_efb_write(GcMachine *m, uint32_t physical, unsigned size, uint32_t value) {
  int is_depth;
  uint32_t *p;
  if (physical < GC_PHYS_EFB || physical >= GC_PHYS_EFB + GC_EFB_WINDOW) return 0;
  /* Written from the outside, so whatever the card holds is stale; and
   * whatever a graphics thread still has to draw lands before the poke. */
  m = gc_gx_thread_target(m);
  gc_gl_invalidate(m);
  if (size < 4u) return 1; /* a partial write to a pixel is not something that happens */
  p = pixel_at(m, physical, &is_depth);
  if (!p) return 1;
  *p = is_depth ? (value & 0x00ffffffu) : quantise(efb_format(m), value);
  return 1;
}

/* ------------------------------------------------------------------ */
/* The copy                                                           */

/* The seven filter weights make three row sums, and not three-one-three:
 * the first two are the row above, the middle three the row itself and the
 * last two the row below. libogc's own default of 0, 0, 21, 22, 21, 0, 0 is
 * therefore an exact pass-through, which is what a copy that is not meant
 * to blur relies on. */
typedef GcEfbCopyFilter GcCopyFilter;

static void copy_filter(const GcMachine *m, GcCopyFilter *out) {
  const uint32_t low = m->gx.bp[BP_COPY_FILTER_LOW];
  const uint32_t high = m->gx.bp[BP_COPY_FILTER_HIGH];
  const uint32_t w[7] = {low & 0x3fu,        (low >> 6) & 0x3fu,  (low >> 12) & 0x3fu,
                         (low >> 18) & 0x3fu, high & 0x3fu,       (high >> 6) & 0x3fu,
                         (high >> 12) & 0x3fu};
  out->previous = w[0] + w[1];
  out->current = w[2] + w[3] + w[4];
  out->next = w[5] + w[6];
}

/* One channel through the filter. The mask to nine bits before the clamp is
 * the hardware's, and it is the whole reason a large weight sum comes out
 * dark rather than white. */
static uint8_t filter_channel(uint32_t previous, uint32_t current, uint32_t next,
                              const GcCopyFilter *w) {
  uint32_t sum = previous * w->previous + current * w->current + next * w->next;
  sum >>= 6;
  sum &= 0x1ffu;
  return (uint8_t)(sum > 255u ? 255u : sum);
}

static uint8_t gamma_curve(uint8_t value, unsigned gamma) {
  double exponent;
  switch (gamma) {
    case 1: exponent = 1.0 / 1.7; break;
    /* Three is not a documented setting and behaves as 2.2 on hardware. */
    case 2:
    case 3: exponent = 1.0 / 2.2; break;
    default: return value;
  }
  return (uint8_t)(pow((double)value / 255.0, exponent) * 255.0 + 0.5);
}

/* The curve above for every byte, worked out the first time a copy asks:
 * it was a pow() a channel a pixel. */
GC_EFB_INLINE uint8_t apply_gamma(uint8_t value, unsigned gamma) {
  static uint8_t table[4][256];
  static int ready;
  if (!gamma) return value;
  if (!ready) {
    unsigned g, v;
    for (g = 0; g < 4u; g++) {
      for (v = 0; v < 256u; v++) table[g][v] = gamma_curve((uint8_t)v, g);
    }
    ready = 1;
  }
  return table[gamma & 3u][value];
}

/* Studio-swing BT.601. The narrowing at the end is a wrap and not a clamp,
 * which is most of what the 262,144-point sweep in the hardware test is
 * checking. */
static void to_intensity(uint8_t *r, uint8_t *g, uint8_t *b) {
  const uint16_t y = (uint16_t)(66u * *r + 129u * *g + 25u * *b);
  const int16_t u = (int16_t)(-38 * *r + -74 * *g + 112 * *b);
  const int16_t v = (int16_t)(112 * *r + -94 * *g + -18 * *b);
  *r = (uint8_t)((y >> 8) + ((y >> 7) & 1) + 16);
  *g = (uint8_t)((u >> 8) + ((u >> 7) & 1) + 128);
  *b = (uint8_t)((v >> 8) + ((v >> 7) & 1) + 128);
}

/* The colour a copy sees at one pixel: what the framebuffer holds, read
 * back the way the pixel format stores it. */
static uint32_t source_pixel(const GcEfbCopyJob *j, unsigned format, uint32_t x, uint32_t y) {
  const size_t at = (size_t)y * GC_EFB_WIDTH + x;
  if (format == GC_EFB_Z24) {
    /* A depth copy reads the depth as if it were three colour channels. */
    return 0xff000000u | (j->depth[at] & 0x00ffffffu);
  }
  return j->color[at];
}

/* The display buffer is not RGB and it is not tiled. It is YUV 4:2:2: two
 * horizontally adjacent pixels share one pair of chroma samples, packed as
 * Y0, Cb, Y1, Cr - which read as a big-endian word is 0xY0CbY1Cr, and is
 * the layout everything else calls YUY2.
 *
 * The conversion is studio-swing BT.601, so black is luma 16 and white is
 * 235 rather than 0 and 255. That is checked: libogc ships a table of
 * pre-converted fill values for twenty named colours, and eighteen of them
 * come back byte-exact from these coefficients.
 *
 * The chroma reduction is a three-tap filter, not an average of the pair:
 * see store_macropixel. */
/* Each product in the conversion, worked out once for every byte value.
 * The sums below are the same operations on the same doubles in the same
 * order as `16.0 + 0.257 * r + 0.504 * g + ...` - a product looked up is
 * the product computed - so the results are the same to the bit. */
typedef struct GcYcbcrTables {
  int ready;
  double y_r[256], y_g[256], y_b[256];
  double cb_r[256], cb_g[256], cb_b[256];
  double cr_r[256], cr_g[256], cr_b[256];
  /* And the way back, for the display. */
  double luma[256], r_v[256], g_u[256], g_v[256], b_u[256];
} GcYcbcrTables;

static GcYcbcrTables ycbcr;

static void ycbcr_fill(void) {
  {
    int i;
    for (i = 0; i < 256; i++) {
      ycbcr.y_r[i] = 16.0 + 0.257 * i;
      ycbcr.y_g[i] = 0.504 * i;
      ycbcr.y_b[i] = 0.098 * i;
      ycbcr.cb_r[i] = 128.0 - 0.148 * i;
      ycbcr.cb_g[i] = 0.291 * i;
      ycbcr.cb_b[i] = 0.439 * i;
      ycbcr.cr_r[i] = 128.0 + 0.439 * i;
      ycbcr.cr_g[i] = 0.368 * i;
      ycbcr.cr_b[i] = 0.071 * i;
      ycbcr.luma[i] = 1.164 * (double)(i - 16);
      ycbcr.r_v[i] = 1.596 * (double)(i - 128);
      ycbcr.g_u[i] = 0.392 * (double)(i - 128);
      ycbcr.g_v[i] = 0.813 * (double)(i - 128);
      ycbcr.b_u[i] = 2.017 * (double)(i - 128);
    }
    ycbcr.ready = 1;
  }
}

GC_EFB_INLINE const GcYcbcrTables *ycbcr_tables(void) {
  if (!ycbcr.ready) ycbcr_fill();
  return &ycbcr;
}

GC_EFB_INLINE void to_ycbcr(uint8_t r, uint8_t g, uint8_t b, int out[3]) {
  const GcYcbcrTables *t = ycbcr_tables();
  out[0] = (int)(t->y_r[r] + t->y_g[g] + t->y_b[b] + 0.5);
  out[1] = (int)(t->cb_r[r] - t->cb_g[g] + t->cb_b[b] + 0.5);
  out[2] = (int)(t->cr_r[r] - t->cr_g[g] - t->cr_b[b] + 0.5);
}

static uint8_t clamp_byte(int value, int low, int high) {
  return (uint8_t)(value < low ? low : (value > high ? high : value));
}

/* Two finished pixels into one four-byte macropixel.
 *
 * Luma is per pixel. The pair shares one Cb and one Cr, and those are not
 * the average of the pair: they are a three-tap filter over the pixel
 * *before* the pair and the pair's own two, weighted one, two, one, and
 * divided by four with the remainder thrown away. In other words the
 * chroma sample sits on the pair's even pixel and reaches one pixel either
 * side of it, which is where a 4:2:2 sample is supposed to sit.
 *
 * Averaging the pair instead is wrong only at a sharp colour edge, and
 * wrong there by a lot: on a test frame of four solid vertical bands it
 * left the first pair of every band fully saturated where a reference
 * renderer had it a quarter of the way back towards the band before -
 * about one percent of the picture, every one of those pixels on a
 * boundary and none anywhere else. Fitting the reference's colours back to
 * chroma gives exactly one part in four, which is this filter and is not
 * any two-tap rule.
 *
 * **Evidence: differential against a second implementation, not hardware.**
 * The weights and the truncation come from matching Dolphin's software
 * renderer, because libogc's table of pre-converted fill colours is all
 * solid colours and a solid colour cannot tell any of these rules apart.
 * A hardware test that copies an edge would settle it. */
/* gc_machine_ram's bounds, over whichever view of main memory a copy
 * writes through. */
GC_EFB_INLINE uint8_t *ram_at(uint8_t *ram, uint32_t physical, uint32_t size) {
  if (physical < gc_ram_size && physical + size <= gc_ram_size) return ram + physical;
  return NULL;
}

static void store_macropixel(uint8_t *ram, uint32_t address, const uint8_t before[3],
                             const uint8_t left[3], const uint8_t right[3]) {
  int a[3], b[3], c[3];
  uint8_t *p = ram_at(ram, address, 4);
  if (!p) return;
  to_ycbcr(before[0], before[1], before[2], c);
  to_ycbcr(left[0], left[1], left[2], a);
  to_ycbcr(right[0], right[1], right[2], b);
  p[0] = clamp_byte(a[0], 16, 235);
  p[1] = clamp_byte((c[1] + 2 * a[1] + b[1]) / 4, 16, 240);
  p[2] = clamp_byte(b[0], 16, 235);
  p[3] = clamp_byte((c[2] + 2 * a[2] + b[2]) / 4, 16, 240);
}

/* The destination is a texture, and a texture is tiled: four by four
 * pixels to a tile, with a tile's alpha and red bytes in its first half and
 * its green and blue in the second. */
static void store_rgba8(uint8_t *ram, uint32_t base, uint32_t tiles_per_row, uint32_t s,
                        uint32_t t, uint32_t argb) {
  const uint32_t tile = ((t >> 2) * tiles_per_row + (s >> 2)) << 5;
  const uint32_t within = ((t & 3u) << 2) + (s & 3u);
  const uint32_t offset = (tile + within) << 1;
  uint8_t *p = ram_at(ram, base + offset, 1);
  uint8_t *q = ram_at(ram, base + offset + 32u, 1);
  if (!p || !q) return;
  p[0] = (uint8_t)(argb >> 24); /* alpha */
  p[1] = (uint8_t)(argb >> 16); /* red */
  q[0] = (uint8_t)(argb >> 8);  /* green */
  q[1] = (uint8_t)argb;         /* blue */
}

/* The other destination formats, tiled the way the texture sampler expects
 * to find them.
 *
 * A copy to memory is how a game makes a texture out of something it just
 * drew - a reflection, a shadow map, a page of a storybook - and it picks
 * the narrowest format that will hold what it needs. Writing only the
 * thirty-two-bit one means every such texture is read back as whatever was
 * in that memory before, which is not blank; it is stale.
 */
static void store_tiled16(uint8_t *ram, uint32_t base, uint32_t tiles_per_row, uint32_t s,
                          uint32_t t, uint16_t value) {
  const uint32_t tile = ((t >> 2) * tiles_per_row + (s >> 2)) << 5;
  const uint32_t within = ((t & 3u) << 2) + (s & 3u);
  uint8_t *p = ram_at(ram, base + tile + (within << 1), 2);
  if (!p) return;
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static void store_tiled8(uint8_t *ram, uint32_t base, uint32_t tiles_per_row, uint32_t s,
                         uint32_t t, uint8_t value) {
  /* Eight-bit texels are 8x4 tiles of thirty-two bytes. */
  const uint32_t tile = ((t >> 2) * tiles_per_row + (s >> 3)) << 5;
  const uint32_t within = ((t & 3u) << 3) + (s & 7u);
  uint8_t *p = ram_at(ram, base + tile + within, 1);
  if (p) *p = value;
}

static void store_tiled4(uint8_t *ram, uint32_t base, uint32_t tiles_per_row, uint32_t s,
                         uint32_t t, uint8_t value) {
  /* Four-bit texels are 8x8 tiles of thirty-two bytes, two to a byte, the
   * left one in the high nibble. A read-modify-write, because the
   * neighbour's nibble is somebody else's pixel. */
  const uint32_t tile = ((t >> 3) * tiles_per_row + (s >> 3)) << 5;
  const uint32_t within = ((t & 7u) << 3) + (s & 7u);
  uint8_t *p = ram_at(ram, base + tile + (within >> 1), 1);
  if (!p) return;
  if (within & 1u) *p = (uint8_t)((*p & 0xf0u) | (value & 0x0fu));
  else *p = (uint8_t)((*p & 0x0fu) | ((value & 0x0fu) << 4));
}

/* Whether a copy in this format writes anything. Asked before the loop
 * rather than inside it, and asked without writing: a probe that stored a
 * pixel to find out would leave one behind in the corner of every copy it
 * refused. */
static int copy_format_known(uint32_t format) {
  switch (format) {
    case COPY_FORMAT_R4:
    case COPY_FORMAT_R8_LOW:
    case COPY_FORMAT_RA4:
    case COPY_FORMAT_RA8:
    case COPY_FORMAT_RGB565:
    case COPY_FORMAT_RGB5A3:
    case COPY_FORMAT_RGBA8:
    case COPY_FORMAT_A8:
    case COPY_FORMAT_R8:
    case COPY_FORMAT_G8:
    case COPY_FORMAT_B8:
    case COPY_FORMAT_RG8:
    case COPY_FORMAT_GB8: return 1;
    default: return 0;
  }
}

/* One pixel, in whichever format the copy asked for. */
static void store_copy_pixel(uint8_t *m, uint32_t format, uint32_t base, uint32_t tiles_per_row,
                            uint32_t s, uint32_t t, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  switch (format) {
    case COPY_FORMAT_RGBA8:
      store_rgba8(m, base, tiles_per_row, s, t,
                  ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
      break;
    case COPY_FORMAT_RGB565:
      store_tiled16(m, base, tiles_per_row, s, t,
                    (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)));
      break;
    case COPY_FORMAT_RGB5A3:
      /* Two layouts in one format, chosen by the top bit: opaque five bits
       * a channel, or three bits of alpha and four bits a channel. A copy
       * whose alpha is full takes the first, which is what makes an opaque
       * render target keep its precision. */
      if (a >= 0xe0u) {
        store_tiled16(m, base, tiles_per_row, s, t,
                      (uint16_t)(0x8000u | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)));
      } else {
        store_tiled16(m, base, tiles_per_row, s, t,
                      (uint16_t)(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)));
      }
      break;
    case COPY_FORMAT_RA8:
      /* Intensity and alpha, a byte each, alpha first - the same order the
       * sampler reads an IA8 texture in. */
      store_tiled16(m, base, tiles_per_row, s, t, (uint16_t)(((uint16_t)a << 8) | r));
      break;
    case COPY_FORMAT_RA4:
      store_tiled8(m, base, tiles_per_row, s, t, (uint8_t)(((a >> 4) << 4) | (r >> 4)));
      break;
    case COPY_FORMAT_R8_LOW:
    case COPY_FORMAT_R8: store_tiled8(m, base, tiles_per_row, s, t, r); break;
    case COPY_FORMAT_G8: store_tiled8(m, base, tiles_per_row, s, t, g); break;
    case COPY_FORMAT_B8: store_tiled8(m, base, tiles_per_row, s, t, b); break;
    case COPY_FORMAT_A8: store_tiled8(m, base, tiles_per_row, s, t, a); break;
    case COPY_FORMAT_RG8:
      store_tiled16(m, base, tiles_per_row, s, t, (uint16_t)(((uint16_t)g << 8) | r));
      break;
    case COPY_FORMAT_GB8:
      store_tiled16(m, base, tiles_per_row, s, t, (uint16_t)(((uint16_t)b << 8) | g));
      break;
    case COPY_FORMAT_R4: store_tiled4(m, base, tiles_per_row, s, t, (uint8_t)(r >> 4)); break;
    default: break;
  }
}

/* Back from luma and chroma to red, green and blue: the inverse of the
 * studio-swing matrix the copy applied. */
GC_EFB_INLINE void from_ycbcr(int y, int cb, int cr, uint8_t out[3]) {
  /* 1.164 * (y - 16), 1.596 * (cr - 128) and the rest, looked up: the same
   * products summed in the same order (ycbcr_tables). */
  const GcYcbcrTables *t = ycbcr_tables();
  const double luma = t->luma[y & 0xff];
  const int r = (int)(luma + t->r_v[cr & 0xff] + 0.5);
  const int g = (int)(luma - t->g_u[cb & 0xff] - t->g_v[cr & 0xff] + 0.5);
  const int b = (int)(luma + t->b_u[cb & 0xff] + 0.5);
  out[0] = clamp_byte(r, 0, 255);
  out[1] = clamp_byte(g, 0, 255);
  out[2] = clamp_byte(b, 0, 255);
}

/* Which buffer is on screen, as fields: a frame line `y` is in the top
 * field's buffer when it is even and the bottom's when it is odd, `y / 2`
 * field lines down.
 *
 * The video interface decides that, so its registers are asked first. The
 * last display copy is the fallback, for a program that copies a frame out
 * but never points the video interface at it - a test program with no
 * retrace handler, say. A copy is one buffer, so its "fields" are the same
 * buffer a line apart. */
static int display_fields(GcMachine *m, GcViDisplay *d) {
  if (gc_vi_display(m, d) == 0) {
    /* The registers hold addresses; what is read here is main memory by
     * offset, and on a Wii the two differ for the second block. */
    d->top = gc_ram_from_register(d->top);
    d->bottom = gc_ram_from_register(d->bottom);
    return 0;
  }
  if (!m->efb.display_width || !m->efb.display_height || !m->efb.display_pitch) return -1;
  d->top = m->efb.display_address;
  d->bottom = m->efb.display_address + m->efb.display_pitch;
  d->field_stride = m->efb.display_pitch * 2u;
  d->width = m->efb.display_width;
  d->height = m->efb.display_height;
  return 0;
}

static uint32_t display_line(const GcViDisplay *d, uint32_t y) {
  return ((y & 1u) ? d->bottom : d->top) + (y >> 1) * d->field_stride;
}

/* The displayed frame as 32-bit pixels, for a host that wants to put it in
 * a window. `pitch` is in pixels, not bytes, so a caller can hand over a
 * texture wider than the frame.
 *
 * What is read is the *external* framebuffer - the one the video interface
 * is scanning out - rather than anything the graphics processor is still
 * working on, so this is the picture as a television would have shown it,
 * including the fact that it is stored two pixels at a time with one shared
 * pair of chroma samples between them. */
/* Rows of the displayed frame, converted. Each row reads its own line of
 * the external framebuffer and writes its own row of the output, so rows
 * can be converted on several threads at once. */
typedef struct GcFrameJob {
  const uint8_t *ram; /* main memory, through whichever view reads without waiting when it can */
  GcViDisplay d;
  uint32_t *out;
  unsigned pitch;
  long chunk_rows, chunks, next_chunk;
} GcFrameJob;

static void frame_row(const uint8_t *ram, const GcViDisplay *d, uint32_t *row, uint32_t y) {
  const uint32_t line = display_line(d, y);
  uint32_t x;
  for (x = 0; x < d->width; x += 2u) {
    const uint32_t at = line + x * 2u;
    const uint8_t *p = (at < gc_ram_size && at + 4u <= gc_ram_size) ? ram + at : NULL;
    uint8_t left[3] = {0, 0, 0}, right[3] = {0, 0, 0};
    if (p) {
      from_ycbcr(p[0], p[1], p[3], left);
      from_ycbcr(p[2], p[1], p[3], right);
    }
    row[x] = 0xff000000u | ((uint32_t)left[0] << 16) | ((uint32_t)left[1] << 8) | left[2];
    if (x + 1u < d->width) {
      row[x + 1u] = 0xff000000u | ((uint32_t)right[0] << 16) | ((uint32_t)right[1] << 8) | right[2];
    }
  }
}

static void frame_chunks(void *argument, unsigned id) {
  GcFrameJob *const j = (GcFrameJob *)argument;
  long chunk;
  (void)id;
  while ((chunk = gc_workers_take(&j->next_chunk)) < j->chunks) {
    const uint32_t first = (uint32_t)(chunk * j->chunk_rows);
    const uint32_t end = (first + (uint32_t)j->chunk_rows < j->d.height) ? first + (uint32_t)j->chunk_rows : j->d.height;
    uint32_t y;
    for (y = first; y < end; y++) frame_row(j->ram, &j->d, j->out + (size_t)y * j->pitch, y);
  }
}

/* The bytes of main memory a display lies in; nonzero when there is no
 * such buffer (nothing to wait for). */
static int display_range(const GcViDisplay *d, uint32_t *first_out, uint32_t *size_out) {
  uint32_t first, last;
  if (!d->height || !d->width) return 1;
  first = d->top < d->bottom ? d->top : d->bottom;
  last = display_line(d, d->height - 1u);
  if (d->height > 1u && display_line(d, d->height - 2u) > last) last = display_line(d, d->height - 2u);
  last += 2u * d->width + 4u;
  if (first >= gc_ram_size || last <= first) return 1;
  *first_out = first;
  *size_out = last - first;
  return 0;
}

int gc_efb_display_place(GcMachine *m, uint32_t *first, uint32_t *width, uint32_t *height) {
  GcViDisplay d;
  if (display_fields(m, &d) != 0 || !d.width || !d.height) return -1;
  *first = d.top < d.bottom ? d.top : d.bottom;
  *width = d.width;
  *height = d.height;
  return 0;
}

int gc_efb_display_ready(GcMachine *m) {
  GcViDisplay d;
  uint32_t first, size;
  if (display_fields(m, &d) != 0 || display_range(&d, &first, &size) != 0) return 1;
  /* By the bytes a pending copy writes, not by its pages: two display
   * buffers side by side share a page, and a copy into one must not make the
   * other look unfinished (gc_efb_frame_rgba reads it through the second
   * view then). */
  return gc_memory_view_for(m->mem1, first, size) != NULL;
}

int gc_efb_display_wait(GcMachine *m, double timeout_ms) {
  GcViDisplay d;
  uint32_t first, size;
  if (display_fields(m, &d) != 0 || display_range(&d, &first, &size) != 0) return 1;
  return gc_memory_wait_released(m->mem1, first, size, timeout_ms);
}

int gc_efb_frame_rgba(GcMachine *m, uint32_t *out, unsigned pitch, unsigned rows,
                      unsigned *width_out, unsigned *height_out) {
  GcFrameJob job;
  uint32_t y;
  unsigned helpers;

  if (display_fields(m, &job.d) != 0) {
    if (width_out) *width_out = 0;
    if (height_out) *height_out = 0;
    return -1;
  }
  if (width_out) *width_out = job.d.width;
  if (height_out) *height_out = job.d.height;
  if (!out) return -1;
  {
    /* A pending copy into the page this buffer shares with another would
     * make a read of the machine's view wait for all of that copy: the
     * second view has the same bytes and does not wait. Bytes a copy is
     * still writing are read through the machine's view, which waits. */
    uint32_t first, size;
    const uint8_t *view = display_range(&job.d, &first, &size) == 0 ? gc_memory_view_for(m->mem1, first, size)
                                                                     : m->mem1;
    job.ram = view ? view : m->mem1;
  }
  /* The video interface can describe a frame bigger than the caller's
   * buffer - a PAL field is taller than NTSC's - and what does not fit is
   * cut off rather than written past the end. */
  if (job.d.width > pitch) job.d.width = pitch;
  if (job.d.height > rows) job.d.height = rows;

  (void)ycbcr_tables(); /* filled on this thread, before any row asks */
  helpers = job.d.height >= 16u ? gc_workers_count() : 0u;
  if (helpers) {
    job.out = out;
    job.pitch = pitch;
    job.chunk_rows = (long)(job.d.height / ((helpers + 1u) * 2u));
    if (job.chunk_rows < 8) job.chunk_rows = 8;
    job.chunks = ((long)job.d.height + job.chunk_rows - 1) / job.chunk_rows;
    job.next_chunk = 0;
    if (GC_WORKERS_RUN_SHARED(frame_chunks, &job) == 0) return 0;
  }
  for (y = 0; y < job.d.height; y++) frame_row(job.ram, &job.d, out + (size_t)y * pitch, y);
  return 0;
}

int gc_efb_write_frame(GcMachine *m, const char *path) {
  GcViDisplay d;
  FILE *file;
  uint32_t x, y;

  if (display_fields(m, &d) != 0) return -1;
  file = fopen(path, "wb");
  if (!file) return -1;
  fprintf(file, "P6\n%u %u\n255\n", d.width, d.height);

  for (y = 0; y < d.height; y++) {
    const uint32_t line = display_line(&d, y);
    for (x = 0; x < d.width; x += 2u) {
      const uint32_t at = line + x * 2u;
      const uint8_t *p = gc_ram_ptr(m, at, 4); /* an offset, as display_fields gives it */
      uint8_t left[3] = {0, 0, 0}, right[3] = {0, 0, 0};
      if (p) {
        from_ycbcr(p[0], p[1], p[3], left);
        from_ycbcr(p[2], p[1], p[3], right);
      }
      fwrite(left, 1, 3, file);
      if (x + 1u < d.width) fwrite(right, 1, 3, file);
    }
  }
  fclose(file);
  return 0;
}

/* Row y of what the copy writes: filtered, gamma corrected, and stored. For
 * a halved copy that is rows 2y and 2y + 1 of the rectangle. */
static void copy_row(const GcEfbCopyJob *j, uint32_t y, uint8_t row_rgb[][3]) {
  uint8_t *const m = j->ram;
  const uint32_t trigger = j->trigger;
  const uint32_t width = j->width, height = j->height, out_width = j->out_width;
  const unsigned efb = j->efb, gamma = j->gamma;
  const int to_display = j->to_display, convert = j->convert, half_scale = j->half_scale;
  const GcCopyFilter *const weights = &j->weights;
  /* One destination pixel, from one source pixel or from a block of four. */
  const unsigned taps = half_scale ? 2u : 1u;
  uint32_t out_x;
  (void)width;

  for (out_x = 0; out_x < out_width; out_x++) {
    unsigned sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
    unsigned tap, down, taken = 0;
    uint8_t r, g, b, a;
    for (down = 0; down < taps; down++) {
      /* A row of the rectangle, and the rows above and below it, clamped to
       * the rectangle when the trigger asks for it. */
      const uint32_t in_y = y * taps + down;
      const uint32_t row = j->top + in_y;
      const uint32_t above = (in_y == 0 && (trigger & COPY_CLAMP_TOP)) ? row : (row ? row - 1u : 0u);
      const uint32_t below = (in_y + 1u == height && (trigger & COPY_CLAMP_BOTTOM)) ? row : row + 1u;
      if (row >= GC_EFB_HEIGHT) break;
      for (tap = 0; tap < taps; tap++) {
        const uint32_t column = j->left + out_x * taps + tap;
        uint32_t previous, current, next;
        uint8_t tr, tg, tb;
        if (column >= GC_EFB_WIDTH) break;

        current = source_pixel(j, efb, column, row);
        previous = source_pixel(j, efb, column, above < GC_EFB_HEIGHT ? above : row);
        next = source_pixel(j, efb, column, below < GC_EFB_HEIGHT ? below : row);

        tr = filter_channel((previous >> 16) & 0xffu, (current >> 16) & 0xffu,
                            (next >> 16) & 0xffu, weights);
        tg = filter_channel((previous >> 8) & 0xffu, (current >> 8) & 0xffu, (next >> 8) & 0xffu,
                            weights);
        tb = filter_channel(previous & 0xffu, current & 0xffu, next & 0xffu, weights);

        tr = apply_gamma(tr, gamma);
        tg = apply_gamma(tg, gamma);
        tb = apply_gamma(tb, gamma);
        if (convert) to_intensity(&tr, &tg, &tb);

        sum_r += tr;
        sum_g += tg;
        sum_b += tb;
        /* Alpha is not filtered and not gamma corrected. */
        sum_a += (current >> 24) & 0xffu;
        taken++;
      }
    }
    if (!taken) break; /* the rectangle ran off the side of the buffer */
    /* **Unverified:** that a halved copy averages its four source pixels
     * rather than taking one of them. Averaging is what a downsample
     * means and what the vertical filter above already does; point
     * sampling would alias, which is the sort of difference that shows up
     * as shimmer on a moving texture and not at all on a still frame. */
    r = (uint8_t)(sum_r / taken);
    g = (uint8_t)(sum_g / taken);
    b = (uint8_t)(sum_b / taken);
    a = (uint8_t)(sum_a / taken);

    if (to_display) {
      row_rgb[out_x][0] = r;
      row_rgb[out_x][1] = g;
      row_rgb[out_x][2] = b;
    } else {
      store_copy_pixel(m, j->format, j->destination, j->tiles_per_row, out_x, y, r, g, b, a);
    }
  }

  /* A row of the display buffer, two pixels at a time. */
  if (to_display) {
    uint32_t pair;
    for (pair = 0; pair + 1u < out_width && pair + 1u < GC_EFB_WIDTH; pair += 2u) {
      /* The tap to the left of the first pair falls outside the
       * rectangle, so it repeats the pixel it sits next to. */
      store_macropixel(m, j->destination + y * j->display_pitch + pair * 2u,
                       row_rgb[pair ? pair - 1u : 0u], row_rgb[pair], row_rgb[pair + 1u]);
    }
  }
}

static void copy_chunks(void *argument, unsigned id) {
  const GcEfbCopyJob *const j = (const GcEfbCopyJob *)argument;
  long chunk;
  (void)id;
  while ((chunk = gc_workers_take(&((GcEfbCopyJob *)argument)->next_chunk)) < j->chunks) {
    const uint32_t first = (uint32_t)chunk * j->chunk_rows;
    const uint32_t end = (first + j->chunk_rows < j->out_rows) ? first + j->chunk_rows : j->out_rows;
    uint8_t row_rgb[GC_EFB_WIDTH][3];
    uint32_t y;
    for (y = first; y < end; y++) copy_row(j, y, row_rgb);
  }
}

/* Whether the rows of this copy may be written by several threads at once.
 *
 * A row reads the framebuffer, which the copy does not change until its
 * clear, after every row is done; the tables it reads are filled before
 * (apply_gamma, ycbcr_tables); and it writes main memory only. So rows can
 * be split exactly when no two rows write the same byte.
 *
 * A display row writes bytes [y * pitch, y * pitch + 2 * width) of the
 * buffer, which are disjoint when two bytes a pixel fit in the pitch. A
 * texture row writes, for each pixel s of row t, the bytes of tile
 * (t >> shift_t) * tiles_per_row + (s >> shift_s) at the offset
 * (t & mask_t, s & mask_s) within it. While s stays below the row of tiles
 * (s >> shift_s < tiles_per_row), that is a different byte for every
 * (s, t) - or, for the four-bit format, a byte shared by s and s ^ 1 of
 * the same row. A rectangle wider than its stride wraps into the next row
 * of tiles, which belongs to another row of the copy; that is drawn in
 * order on one thread. */
static int copy_rows_disjoint(const GcEfbCopyJob *j) {
  if (j->to_display) return j->display_pitch != 0 && 2u * j->out_width <= j->display_pitch;
  {
    uint32_t tile_width;
    switch (j->format) {
      case COPY_FORMAT_R4:
      case COPY_FORMAT_R8_LOW:
      case COPY_FORMAT_RA4:
      case COPY_FORMAT_A8:
      case COPY_FORMAT_R8:
      case COPY_FORMAT_G8:
      case COPY_FORMAT_B8: tile_width = 8u; break;
      default: tile_width = 4u; break;
    }
    return j->tiles_per_row != 0 && j->out_width <= tile_width * j->tiles_per_row;
  }
}

int gc_efb_copy_texture_format(const GcEfbCopyJob *job, int *clamp_top, int *clamp_bottom) {
  if (clamp_top) *clamp_top = (job->trigger & COPY_CLAMP_TOP) != 0;
  if (clamp_bottom) *clamp_bottom = (job->trigger & COPY_CLAMP_BOTTOM) != 0;
  if (job->to_display || !job->writes || !job->out_rows || !job->out_width) return -1;
  /* A rectangle wider than its stride folds over itself in memory; nothing
   * reads that as one picture. */
  if (!copy_rows_disjoint(job)) return -1;
  switch (job->format) {
    case COPY_FORMAT_R4: return 0x0;     /* I4 */
    case COPY_FORMAT_R8_LOW:
    case COPY_FORMAT_A8:
    case COPY_FORMAT_R8:
    case COPY_FORMAT_G8:
    case COPY_FORMAT_B8: return 0x1;     /* I8 */
    case COPY_FORMAT_RA4: return 0x2;    /* IA4 */
    case COPY_FORMAT_RA8:
    case COPY_FORMAT_RG8:
    case COPY_FORMAT_GB8: return 0x3;    /* IA8 */
    case COPY_FORMAT_RGB565: return 0x4;
    case COPY_FORMAT_RGB5A3: return 0x5;
    case COPY_FORMAT_RGBA8: return 0x6;
    default: return -1;
  }
}

int gc_efb_copy_prepare(GcMachine *m, uint32_t trigger, GcEfbCopyJob *job) {
  const uint32_t top_left = m->gx.bp[BP_EFB_TOP_LEFT];
  const uint32_t size = m->gx.bp[BP_EFB_SIZE];
  const uint32_t left = top_left & 0x3ffu;
  const uint32_t top = (top_left >> 10) & 0x3ffu;
  const uint32_t width = (size & 0x3ffu) + 1u;
  const uint32_t height = ((size >> 10) & 0x3ffu) + 1u;
  /* As an offset into main memory. On a Wii the register can name the
   * second block, which is where Super Mario Galaxy 2 keeps the buffers it
   * displays; left as the address it would have been past the end of memory
   * and every frame the game drew would have gone nowhere. */
  const uint32_t destination = gc_ram_from_register((m->gx.bp[BP_EFB_ADDRESS] & 0x00ffffffu) << 5);
  /* The format field is stored rotated one place right. */
  const uint32_t field = (trigger >> 3) & 0xfu;
  const uint32_t format = (field >> 1) | ((field & 1u) << 3);
  /* How far apart two rows of tiles are, counted in tiles.
   *
   * The register counts thirty-two byte units, and a tile is thirty-two
   * bytes in every format except the thirty-two-bit one, where a 4x4 block
   * takes two of them: the alphas and reds in the first, the greens and
   * blues in the second. So the halving that is right for that format is
   * wrong for all the others, and a copy written with it puts every row of
   * tiles at half the pitch it should - which looks like a picture whose top
   * is correct and whose remainder is folded over itself. */
  const uint32_t stride_units = m->gx.bp[BP_EFB_STRIDE] & 0x3ffu;
  const uint32_t tiles_per_row =
      (format == COPY_FORMAT_RGBA8) ? (stride_units >> 1) : stride_units;
  const unsigned gamma = (trigger >> 7) & 3u;
  const int to_display = (trigger & COPY_TO_XFB) != 0;
  const int convert = !to_display && (trigger & COPY_INTENSITY) && (trigger & COPY_AUTO_CONVERT);
  const int half_scale = (trigger & COPY_HALF_SCALE) != 0;
  /* What goes out, which is not what comes in when the width is halved. */
  const uint32_t out_width = half_scale ? (width >> 1) : width;
  /* The display buffer's pitch is in units of sixteen pixels, which for two
   * bytes a pixel is the same thirty-two bytes the tile count counts in. */
  const uint32_t display_pitch = (m->gx.bp[BP_EFB_STRIDE] & 0x3ffu) * 32u;

  memset(job, 0, sizeof(*job));
  m->efb.copies++;

  /* Bit 14 overrides the format field: a display copy is always YUV,
   * whatever the destination format says. */
  job->writes = to_display || (tiles_per_row && copy_format_known(format));
  if (!job->writes) {
    /* A format nothing here writes, or a destination with no width. Counted,
     * so that a game leaning on one is visible rather than silently reading
     * back whatever was in that memory before. */
    m->efb.unsupported_copies++;
    if (m->log && m->efb.unsupported_copies <= 8) {
      fprintf(m->log, "gx: an EFB copy this does not do yet: format %u, stride %u\n", format,
              m->gx.bp[BP_EFB_STRIDE] & 0x3ffu);
    }
    return 0;
  }

  job->trigger = trigger;
  job->left = left;
  job->top = top;
  job->width = width;
  job->height = height;
  job->out_width = out_width;
  job->destination = destination;
  job->format = format;
  job->tiles_per_row = tiles_per_row;
  job->display_pitch = display_pitch;
  job->gamma = gamma;
  job->efb = efb_format(m);
  job->to_display = to_display;
  job->convert = convert;
  job->half_scale = half_scale;
  copy_filter(m, &job->weights);
  /* The rows that exist: a rectangle reaching below the framebuffer stops
   * at its last row. */
  job->rows = (top < GC_EFB_HEIGHT) ? ((height < GC_EFB_HEIGHT - top) ? height : GC_EFB_HEIGHT - top) : 0u;
  /* ... and the rows written: half of them when the copy is halved. */
  job->out_rows = half_scale ? (job->rows >> 1) : job->rows;
  /* Filled here, on this thread, rather than by whichever row asks first. */
  (void)apply_gamma(0, 1);
  (void)ycbcr_tables();

  if (to_display) {
    m->efb.display_address = destination;
    m->efb.display_pitch = display_pitch;
    m->efb.display_width = width;
    m->efb.display_height = height;
  }

  if (trigger & COPY_CLEAR) {
    /* Alpha travels with red and green with blue, each in the high and low
     * byte of its own register. */
    const uint32_t ar = m->gx.bp[BP_COPY_CLEAR_AR];
    const uint32_t gb = m->gx.bp[BP_COPY_CLEAR_GB];
    const uint32_t colour = (((ar >> 8) & 0xffu) << 24) | ((ar & 0xffu) << 16) |
                            (((gb >> 8) & 0xffu) << 8) | (gb & 0xffu);
    job->clears = 1;
    /* One colour for the whole rectangle, so it is narrowed once. */
    job->clear_colour = quantise(job->efb, colour);
    job->clear_depth = m->gx.bp[BP_COPY_CLEAR_Z] & 0x00ffffffu;
  }
  return 1;
}

void gc_efb_copy_bytes(const GcEfbCopyJob *j, uint32_t *first, uint32_t *end) {
  uint64_t last;
  *first = j->destination;
  if (!j->rows || !j->out_width) {
    *end = j->destination;
    return;
  }
  if (j->to_display) {
    /* Row y writes [y * pitch, y * pitch + 2 * width), four bytes a pair. */
    last = (uint64_t)(j->out_rows - 1u) * j->display_pitch + 2u * (uint64_t)j->out_width + 4u;
  } else {
    unsigned shift_s = 2u, shift_t = 2u;
    uint64_t tile_bytes = 32u;
    switch (j->format) {
      case COPY_FORMAT_R4: shift_s = 3u; shift_t = 3u; break;
      case COPY_FORMAT_R8_LOW:
      case COPY_FORMAT_RA4:
      case COPY_FORMAT_A8:
      case COPY_FORMAT_R8:
      case COPY_FORMAT_G8:
      case COPY_FORMAT_B8: shift_s = 3u; break;
      case COPY_FORMAT_RGBA8: tile_bytes = 64u; break;
      default: break;
    }
    last = ((uint64_t)((j->out_rows - 1u) >> shift_t) * j->tiles_per_row + ((j->out_width - 1u) >> shift_s) + 1u) *
           tile_bytes;
  }
  last += j->destination;
  *end = last > 0xffffffffu ? 0xffffffffu : (uint32_t)last;
}

int gc_efb_copy_rows_independent(const GcEfbCopyJob *j) {
  return j->writes && j->left + j->out_width * (j->half_scale ? 2u : 1u) <= GC_EFB_WIDTH && copy_rows_disjoint(j);
}

void gc_efb_copy_row_bytes(const GcEfbCopyJob *j, uint32_t y, uint32_t *first, uint32_t *end) {
  uint64_t start, stop;
  if (j->to_display) {
    start = (uint64_t)j->destination + (uint64_t)y * j->display_pitch;
    stop = start + 2u * (uint64_t)j->out_width + 4u;
  } else {
    unsigned shift_t = 2u;
    uint64_t tile_bytes = 32u;
    if (j->format == COPY_FORMAT_R4) shift_t = 3u;
    if (j->format == COPY_FORMAT_RGBA8) tile_bytes = 64u;
    start = (uint64_t)j->destination + (uint64_t)(y >> shift_t) * j->tiles_per_row * tile_bytes;
    stop = start + (uint64_t)j->tiles_per_row * tile_bytes;
  }
  *first = start > 0xffffffffu ? 0xffffffffu : (uint32_t)start;
  *end = stop > 0xffffffffu ? 0xffffffffu : (uint32_t)stop;
}

void gc_efb_copy_run_rows(GcEfbCopyJob *j, uint32_t first, uint32_t end) {
  uint8_t row_rgb[GC_EFB_WIDTH][3];
  uint32_t y;
  if (!j->writes) return;
  if (end > j->out_rows) end = j->out_rows;
  for (y = first; y < end; y++) copy_row(j, y, row_rgb);
}

void gc_efb_copy_run(GcEfbCopyJob *j, int helpers_allowed) {
  uint8_t row_rgb[GC_EFB_WIDTH][3];
  uint32_t y;
  unsigned helpers = 0;
  if (!j->writes) return;
  /* A rectangle that runs off the side of the framebuffer leaves the
   * rest of a display row to whatever the previous row put in the
   * buffer, so it stays on one thread with one buffer. */
  if (helpers_allowed && j->out_rows >= 8u && (uint64_t)j->out_rows * j->out_width >= 4096u &&
      j->left + j->out_width * (j->half_scale ? 2u : 1u) <= GC_EFB_WIDTH && copy_rows_disjoint(j)) {
    helpers = gc_workers_count();
  }
  if (helpers) {
    const uint32_t per = j->out_rows / ((helpers + 1u) * 4u);
    j->chunk_rows = per < 4u ? 4u : per;
    j->chunks = (long)((j->out_rows + j->chunk_rows - 1u) / j->chunk_rows);
    j->next_chunk = 0;
    if (GC_WORKERS_RUN_SHARED(copy_chunks, j) != 0) helpers = 0;
  }
  if (!helpers) {
    for (y = 0; y < j->out_rows; y++) copy_row(j, y, row_rgb);
  }
}

void gc_efb_copy(GcMachine *m, uint32_t trigger) {
  GcEfbCopyJob job;
  uint32_t x, y;

  /* The OpenGL path makes the copy from the card's framebuffer. */
  if (gc_gl_copy(m, trigger)) return;

  if (!gc_efb_copy_prepare(m, trigger, &job)) return;
  job.color = m->efb.color;
  job.depth = m->efb.depth;
  job.ram = m->mem1;
  gc_efb_copy_run(&job, 1);

  if (job.clears) {
    const uint32_t left = job.left, top = job.top, width = job.width, height = job.height;
    const uint32_t room = (left < GC_EFB_WIDTH) ? GC_EFB_WIDTH - left : 0u;
    const uint32_t across = (width < room) ? width : room;
    for (y = 0; y < height && top + y < GC_EFB_HEIGHT; y++) {
      const size_t row = (size_t)(top + y) * GC_EFB_WIDTH + left;
      for (x = 0; x < across; x++) {
        m->efb.color[row + x] = job.clear_colour;
        m->efb.depth[row + x] = job.clear_depth;
      }
    }
  }
}
