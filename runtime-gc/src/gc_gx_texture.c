/*
 * The texture unit: a coordinate in, a colour out.
 *
 * Textures are tiled, and every format but one stores a tile in exactly
 * thirty-two bytes - the cache line - which is why an eight-bit format has
 * an eight-by-four tile and a sixteen-bit one four-by-four. The address
 * arithmetic is the same shape the framebuffer copy writes, so a copied
 * texture and a texture loaded from a disc read identically.
 *
 * The decode rules are in docs/gc-texture-formats.md. They came out of
 * permissively licensed decoders - noclip.website, aurora, gclib and
 * GRRLIB, all MIT - rather than from libogc, which never touches a texel
 * byte, and the document says which rules are settled, which are disputed
 * and which are still open. Two worth knowing here:
 *
 *   - alpha is the *high* half of an IA texel in both IA4 and IA8. A
 *     decoder that puts intensity first is wrong the same way in both.
 *   - the two endpoint colours of a compressed block are compared as
 *     unsigned sixteen-bit numbers, and equal endpoints select the mode
 *     with a transparent fourth entry rather than the four-colour one.
 */

#include <limits.h>
#include <math.h>
#include <string.h>

#include "gc.h"
#include "gc_gx_texture.h"
#include "gc_gx_texture_bound.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define GC_TEXTURE_SSE2 1
#else
#define GC_TEXTURE_SSE2 0
#endif

/* setmode0 through setimage3, for texture units 0 to 3 at 0x80 and 4 to 7
 * at 0xa0. */
#define BP_TEXTURE_LOW 0x80
#define BP_TEXTURE_HIGH 0xa0
#define BP_TEXCOORD_SCALE 0x30 /* .. 0x3f, an s and a t register a coordinate */

enum {
  GC_TF_I4 = 0x0,
  GC_TF_I8 = 0x1,
  GC_TF_IA4 = 0x2,
  GC_TF_IA8 = 0x3,
  GC_TF_RGB565 = 0x4,
  GC_TF_RGB5A3 = 0x5,
  GC_TF_RGBA8 = 0x6,
  GC_TF_C4 = 0x8,
  GC_TF_C8 = 0x9,
  GC_TF_C14X2 = 0xa,
  GC_TF_CMPR = 0xe
};

/* Tile geometry and size, by format. */
typedef struct GcTexFormat {
  unsigned tile_width;
  unsigned tile_height;
  unsigned bytes_per_tile;
  unsigned bits_per_texel;
} GcTexFormat;

static GcTexFormat format_of(unsigned format) {
  GcTexFormat f;
  switch (format) {
    case GC_TF_I4:
    case GC_TF_C4: f.tile_width = 8; f.tile_height = 8; f.bits_per_texel = 4; break;
    case GC_TF_CMPR: f.tile_width = 8; f.tile_height = 8; f.bits_per_texel = 4; break;
    case GC_TF_I8:
    case GC_TF_IA4:
    case GC_TF_C8: f.tile_width = 8; f.tile_height = 4; f.bits_per_texel = 8; break;
    case GC_TF_RGBA8: f.tile_width = 4; f.tile_height = 4; f.bits_per_texel = 32; break;
    default: f.tile_width = 4; f.tile_height = 4; f.bits_per_texel = 16; break;
  }
  f.bytes_per_tile = (format == GC_TF_RGBA8) ? 64u : 32u;
  return f;
}

/* Widening a narrow channel: the top bits are repeated into the bottom, so
 * that the largest value becomes 255 and not 248. */
static uint8_t widen3(unsigned v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); }
static uint8_t widen4(unsigned v) { return (uint8_t)((v << 4) | v); }
static uint8_t widen5(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t widen6(unsigned v) { return (uint8_t)((v << 2) | (v >> 4)); }

/* gc_machine_ram's own test, written out here so the compiler can see it:
 * that function is in another file, and a texel read is the innermost loop
 * of the rasteriser. A byte outside main memory reads as zero, as before. */
static uint8_t texture_byte(const GcMachine *m, uint32_t address) {
  return (address < gc_ram_size) ? m->mem1[address] : 0u;
}

static uint16_t texture_halfword(const GcMachine *m, uint32_t address) {
  return (uint16_t)((texture_byte(m, address) << 8) | texture_byte(m, address + 1u));
}

/* ------------------------------------------------------------------ */
/* The palette                                                         */

/* A lookup table is loaded, not addressed in place: software points one
 * register at the palette in main memory and another at where in the
 * texture cache it should go, and writing the second one performs the
 * copy. A texture unit then names an offset into that memory rather than
 * an address. */
int gc_gx_tlut_write(GcMachine *m, unsigned reg, uint32_t payload) {
  if (reg == 0x64u) {
    m->gx.tlut_source = gc_ram_from_register((payload & 0x00ffffffu) << 5);
    return 1;
  }
  if (reg == 0x65u) {
    /* The destination is in 512-byte units, which is sixteen entries of
     * the table's own sixteen bits. The count code is in entries. */
    const uint32_t offset = (payload & 0x3ffu) * GC_GX_TLUT_UNIT_ENTRIES;
    const uint32_t count = (payload >> 10) & 0x3ffu;
    uint32_t entries = count * 16u;
    uint32_t i;
    if (!entries) entries = 16u;
    for (i = 0; i < entries && offset + i < GC_GX_TLUT_ENTRIES; i++) {
      m->gx.tlut.entry[offset + i] = texture_halfword(m, m->gx.tlut_source + i * 2u);
    }
    return 1;
  }
  return 0;
}

static void from_palette_entry(unsigned format, uint16_t v, uint8_t out[4]) {
  switch (format & 3u) {
    case 0: /* intensity and alpha, alpha in the high byte */
      out[0] = out[1] = out[2] = (uint8_t)(v & 0xffu);
      out[3] = (uint8_t)(v >> 8);
      return;
    case 1: /* 565 */
      out[0] = widen5((v >> 11) & 0x1fu);
      out[1] = widen6((v >> 5) & 0x3fu);
      out[2] = widen5(v & 0x1fu);
      out[3] = 0xff;
      return;
    default: /* 5a3 */
      if (v & 0x8000u) {
        out[0] = widen5((v >> 10) & 0x1fu);
        out[1] = widen5((v >> 5) & 0x1fu);
        out[2] = widen5(v & 0x1fu);
        out[3] = 0xff;
      } else {
        out[0] = widen4((v >> 8) & 0xfu);
        out[1] = widen4((v >> 4) & 0xfu);
        out[2] = widen4(v & 0xfu);
        out[3] = widen3((v >> 12) & 7u);
      }
      return;
  }
}

/* One entry of the table this unit is bound to. */
static uint16_t palette_entry(const GcMachine *m, const GcGxTexture *tex, unsigned index) {
  const unsigned at = tex->palette_offset * GC_GX_TLUT_UNIT_ENTRIES + index;
  return (at < GC_GX_TLUT_ENTRIES) ? m->gx.tlut.entry[at] : 0u;
}

/* ------------------------------------------------------------------ */
/* One texel                                                           */

/* A compressed block: two endpoint colours and two bits an entry. The
 * comparison between the endpoints is unsigned, and the weights are three
 * and five eighths rather than exact thirds - which is where this format
 * differs from the one it was derived from.
 *
 * The four colours an entry can select are a function of the two endpoint
 * words and nothing else, so they are remembered keyed on those words: a
 * bilinear sample reads four texels, usually of one block, and adjacent
 * pixels read the same blocks again. The key is the bytes themselves, read
 * afresh every time, so there is nothing to invalidate - a block whose
 * memory changed has different bytes or the same colours. */
typedef struct GcCompressedColours {
  uint32_t key; /* c0 << 16 | c1 */
  int valid;
  uint8_t colour[4][4];
} GcCompressedColours;

/* A few blocks rather than one: a bilinear footprint straddles two or four
 * blocks along every block edge, which is a quarter of the texels in each
 * direction, and one entry thrashed there. Where an entry lives is only a
 * hash of its key, and a hit still compares the whole key. */
#define GC_CMPR_MEMO 64u
/* One memo a thread: the rasteriser samples on helper threads too
 * (gc_gx_raster.c), and a slot half rewritten by one thread while another
 * reads it would hand back colours that belong to neither key. */
#if defined(_MSC_VER)
static __declspec(thread) GcCompressedColours compressed_memo[GC_CMPR_MEMO];
#else
static __thread GcCompressedColours compressed_memo[GC_CMPR_MEMO];
#endif

static void compressed_palette(uint16_t c0, uint16_t c1, uint8_t colours[4][4]) {
  uint8_t *const a = colours[0], *const b = colours[1];
  uint8_t *const third = colours[2], *const fourth = colours[3];

  from_palette_entry(1, c0, a);
  from_palette_entry(1, c1, b);

  if (c0 > c1) {
    third[0] = (uint8_t)((5u * a[0] + 3u * b[0]) >> 3);
    third[1] = (uint8_t)((5u * a[1] + 3u * b[1]) >> 3);
    third[2] = (uint8_t)((5u * a[2] + 3u * b[2]) >> 3);
    fourth[0] = (uint8_t)((3u * a[0] + 5u * b[0]) >> 3);
    fourth[1] = (uint8_t)((3u * a[1] + 5u * b[1]) >> 3);
    fourth[2] = (uint8_t)((3u * a[2] + 5u * b[2]) >> 3);
    fourth[3] = 0xff;
  } else {
    third[0] = (uint8_t)((a[0] + b[0]) >> 1);
    third[1] = (uint8_t)((a[1] + b[1]) >> 1);
    third[2] = (uint8_t)((a[2] + b[2]) >> 1);
    /* The midpoint again, and transparent. This is the one rule the
     * permissive sources disagree about: the tools that treat this as
     * an ordinary compressed texture say black instead. */
    fourth[0] = third[0];
    fourth[1] = third[1];
    fourth[2] = third[2];
    fourth[3] = 0;
  }
  third[3] = 0xff;
}

static void decode_compressed(const GcMachine *m, uint32_t base, unsigned s, unsigned t,
                              uint8_t out[4]) {
  const unsigned sub = ((t & 4u) >> 1) | ((s & 4u) >> 2); /* four blocks to a tile */
  const uint32_t at = base + sub * 8u;
  const uint16_t c0 = texture_halfword(m, at);
  const uint16_t c1 = texture_halfword(m, at + 2u);
  const uint32_t key = ((uint32_t)c0 << 16) | c1;
  const unsigned row = t & 3u, column = s & 3u;
  const uint8_t indices = texture_byte(m, at + 4u + row);
  /* The first texel of a row is the most significant pair. */
  const unsigned index = (indices >> (6u - 2u * column)) & 3u;

  GcCompressedColours *memo = &compressed_memo[(key ^ (key >> 13) ^ (key >> 22)) & (GC_CMPR_MEMO - 1u)];
  if (!memo->valid || memo->key != key) {
    compressed_palette(c0, c1, memo->colour);
    memo->key = key;
    memo->valid = 1;
  }
  memcpy(out, memo->colour[index], 4);
}

/* Tile sides are powers of two, so the division and remainder that find a
 * texel's tile are shifts and masks - the same numbers for unsigned
 * operands. */
static unsigned tile_shift(unsigned side) { return side == 8u ? 3u : 2u; }

static void decode_at(const GcMachine *m, const GcGxTexture *tex, uint32_t base, unsigned k,
                      unsigned s, unsigned t, uint8_t out[4]);

static void decode_texel(GcMachine *m, const GcGxTexture *tex, unsigned s, unsigned t,
                         uint8_t out[4]) {
  const GcTexFormat f = format_of(tex->format);
  const unsigned tiles_per_row = (tex->width + f.tile_width - 1u) / f.tile_width;
  const unsigned tile = (t / f.tile_height) * tiles_per_row + (s / f.tile_width);
  const uint32_t base = tex->address + tile * f.bytes_per_tile;
  const unsigned k = (t % f.tile_height) * f.tile_width + (s % f.tile_width);
  decode_at(m, tex, base, k, s, t, out);
}

/* One texel of a tile, given where the tile starts. `tex` supplies only the
 * format and the palette binding. */
static void decode_at(const GcMachine *m, const GcGxTexture *tex, uint32_t base, unsigned k,
                      unsigned s, unsigned t, uint8_t out[4]) {
  out[0] = out[1] = out[2] = 0;
  out[3] = 0xff;

  switch (tex->format) {
    case GC_TF_I4: {
      const uint8_t byte = texture_byte(m, base + (k >> 1));
      /* The high nibble is the left texel. */
      const unsigned v = (k & 1u) ? (byte & 0xfu) : (byte >> 4);
      out[0] = out[1] = out[2] = out[3] = widen4(v);
      return;
    }
    case GC_TF_I8: {
      const uint8_t v = texture_byte(m, base + k);
      out[0] = out[1] = out[2] = out[3] = v;
      return;
    }
    case GC_TF_IA4: {
      const uint8_t byte = texture_byte(m, base + k);
      out[0] = out[1] = out[2] = widen4(byte & 0xfu);
      out[3] = widen4(byte >> 4); /* alpha is the high nibble */
      return;
    }
    case GC_TF_IA8: {
      const uint16_t v = texture_halfword(m, base + k * 2u);
      out[0] = out[1] = out[2] = (uint8_t)(v & 0xffu);
      out[3] = (uint8_t)(v >> 8); /* alpha is the high byte */
      return;
    }
    case GC_TF_RGB565:
      from_palette_entry(1, texture_halfword(m, base + k * 2u), out);
      return;
    case GC_TF_RGB5A3:
      from_palette_entry(2, texture_halfword(m, base + k * 2u), out);
      return;
    case GC_TF_RGBA8:
      /* Two planes: alpha and red in the first thirty-two bytes, green and
       * blue in the second. */
      out[3] = texture_byte(m, base + k * 2u);
      out[0] = texture_byte(m, base + k * 2u + 1u);
      out[1] = texture_byte(m, base + 32u + k * 2u);
      out[2] = texture_byte(m, base + 32u + k * 2u + 1u);
      return;
    case GC_TF_C4: {
      const uint8_t byte = texture_byte(m, base + (k >> 1));
      const unsigned index = (k & 1u) ? (byte & 0xfu) : (byte >> 4);
      from_palette_entry(tex->palette_format, palette_entry(m, tex, index), out);
      return;
    }
    case GC_TF_C8:
      from_palette_entry(tex->palette_format, palette_entry(m, tex, texture_byte(m, base + k)),
                         out);
      return;
    case GC_TF_C14X2: {
      const uint16_t index = texture_halfword(m, base + k * 2u) & 0x3fffu;
      from_palette_entry(tex->palette_format, palette_entry(m, tex, index), out);
      return;
    }
    case GC_TF_CMPR:
      decode_compressed(m, base, s, t, out);
      return;
    default:
      return;
  }
}

/* ------------------------------------------------------------------ */
/* Wrapping                                                            */

static unsigned wrap(int value, unsigned size, unsigned mode) {
  if (!size) return 0;
  /* Inside the texture every mode leaves a coordinate where it is, and that
   * is nearly every texel a triangle reads. */
  if ((unsigned)value < size) return (unsigned)value;
  switch (mode) {
    case 1: { /* repeat */
      const int wrapped = value % (int)size;
      return (unsigned)(wrapped < 0 ? wrapped + (int)size : wrapped);
    }
    case 2: { /* mirror */
      const int period = (int)size * 2;
      int wrapped = value % period;
      if (wrapped < 0) wrapped += period;
      return (unsigned)(wrapped < (int)size ? wrapped : period - 1 - wrapped);
    }
    default: /* clamp */
      if (value < 0) return 0;
      return (unsigned)value >= size ? size - 1u : (unsigned)value;
  }
}

/* ------------------------------------------------------------------ */

void gc_gx_texture_read(GcMachine *m, unsigned unit, GcGxTexture *out) {
  const unsigned block = (unit & 4u) ? BP_TEXTURE_HIGH : BP_TEXTURE_LOW;
  const unsigned n = unit & 3u;
  const uint32_t mode0 = m->gx.bp[block + n];
  const uint32_t size = m->gx.bp[block + 8u + n];
  const uint32_t image = m->gx.bp[block + 0x14u + n];

  memset(out, 0, sizeof(*out));
  out->width = (size & 0x3ffu) + 1u;
  out->height = ((size >> 10) & 0x3ffu) + 1u;
  out->format = (size >> 20) & 0xfu;
  out->address = gc_ram_from_register((image & 0x00ffffffu) << 5); /* an offset: a Wii keeps textures in either block */
  out->wrap_s = mode0 & 3u;
  out->wrap_t = (mode0 >> 2) & 3u;
  out->magnify_linear = (mode0 >> 4) & 1u;
  out->mip_mode = (mode0 >> 5) & 3u;
  out->minify_linear = (mode0 >> 7) & 1u;
  /* Bit 8 is the *inverse* of the SDK's "edge LOD" parameter: set means
   * the diagonal reduction. */
  out->diagonal_lod = (mode0 >> 8) & 1u;
  /* Signed eight bits in thirty-seconds of a level. */
  out->lod_bias = (int)((mode0 >> 9) & 0xffu) - (int)(((mode0 >> 16) & 1u) << 8);

  {
    const uint32_t mode1 = m->gx.bp[block + 4u + n];
    out->min_lod = mode1 & 0xffu;
    out->max_lod = (mode1 >> 8) & 0xffu;
  }

  {
    /* Which lookup table, and in what format. */
    const uint32_t binding = m->gx.bp[block + 0x18u + n];
    out->palette_offset = binding & 0x3ffu;
    out->palette_format = (binding >> 10) & 3u;
  }
}

/* ------------------------------------------------------------------ */
/* Mipmaps                                                             */

/* How many bytes one level occupies.
 *
 * The ceiling is not optional and it is where garbage comes from. A level
 * narrower than a tile still costs a whole tile - a one-by-one level is 32
 * bytes, not two - and a level whose width is not a multiple of the tile
 * width is padded to one, with the padding occupying real bytes that the
 * next level's offset has to step over. Get either wrong and every level
 * past the first reads from the middle of the one before it. */
static uint32_t level_bytes(unsigned format, unsigned width, unsigned height) {
  const GcTexFormat f = format_of(format);
  const unsigned across = (width + f.tile_width - 1u) / f.tile_width;
  const unsigned down = (height + f.tile_height - 1u) / f.tile_height;
  return (uint32_t)across * (uint32_t)down * f.bytes_per_tile;
}

/* The texture as one of its levels sees it. There is exactly one address
 * register a unit and it points at level zero; every other level is
 * derived, because the chain is contiguous and that is the whole of the
 * addressing scheme. Each axis halves independently and floors at one, so
 * the tail of a chain is not square - a 64 by 8 texture has a 4 by 1 level
 * before it has a 1 by 1 one. */
static void level_of(const GcGxTexture *texture, unsigned level, GcGxTexture *out) {
  unsigned i;
  *out = *texture;
  for (i = 0; i < level; i++) {
    out->address += level_bytes(out->format, out->width, out->height);
    out->width = (out->width > 1u) ? (out->width >> 1) : 1u;
    out->height = (out->height > 1u) ? (out->height >> 1) : 1u;
    if (out->width == 1u && out->height == 1u) break;
  }
}

/* One level of a chain, as a texture in its own right: the OpenGL path
 * needs each level's address and size so it can hand the whole chain to
 * the card in one go. */
void gc_gx_texture_level(const GcGxTexture *texture, unsigned level, GcGxTexture *out) {
  level_of(texture, level, out);
}

/* How many levels the chain holds before both axes reach one texel. The
 * registers do not say, so this is derived the same way the addressing
 * is: halve until there is nothing left to halve. */
unsigned gc_gx_texture_levels(const GcGxTexture *texture) {
  unsigned width = texture->width, height = texture->height, count = 1;
  while (width > 1u || height > 1u) {
    if (width > 1u) width >>= 1;
    if (height > 1u) height >>= 1;
    count++;
  }
  return count;
}

/* The bytes of main memory a pixel sampling this texture can read: every
 * level up to the highest the level of detail can reach, as the OpenGL
 * path's cache hashes them (gc_gl.c, top_level). A texture that starts one
 * texel square has a distinct second level, which the level walk steps onto
 * before it notices. Cut to main memory; *size is 0 when none of it is. */
void gc_gx_texture_span(const GcGxTexture *texture, uint32_t *first, uint32_t *size) {
  const unsigned levels = gc_gx_texture_levels(texture) + ((texture->width == 1u && texture->height == 1u) ? 1u : 0u);
  const unsigned range = (texture->min_lod > texture->max_lod ? texture->min_lod : texture->max_lod) >> 4;
  const unsigned top = range + 1u < levels ? range + 1u : levels - 1u;
  GcGxTexture last;
  uint64_t end;
  level_of(texture, top, &last);
  end = (uint64_t)last.address + level_bytes(texture->format, last.width, last.height);
  *first = texture->address;
  *size = 0;
  if (texture->address >= gc_ram_size) return;
  if (end > gc_ram_size) end = gc_ram_size;
  if (end > texture->address) *size = (uint32_t)(end - texture->address);
}

/* One texel of one level, decoded, with no filtering and no wrapping -
 * the caller is walking the level in order and already knows the texel is
 * inside it. */
void gc_gx_texture_texel(GcMachine *m, const GcGxTexture *level, unsigned s, unsigned t,
                         uint8_t out[4]) {
  decode_texel(m, level, s, t, out);
}

/* Two to the what, in sixteenths of a level.
 *
 * **Unverified:** whether the hardware's log2 is this approximation or an
 * accurate one. This is the classic trick - take the float's biased
 * exponent as the integer part and the top of its mantissa *directly* as
 * the fraction, so 2^e * (1+m) reads as e + m rather than e + log2(1+m).
 * It sags between the octave boundaries, under-reading by up to about
 * 0.086 of a level near m = 0.44, which is a band of slightly-too-sharp
 * texture in the middle of each octave.
 *
 * The reason to pick it rather than a real log2 is that the reference
 * emulator's own two implementations disagree - its software rasteriser
 * uses this and its shader path calls the GPU's log2 - so the distinction
 * is evidently not settled there either, and this is the one that is
 * nearly free in silicon. A hardware test would settle it. */
static int fixed_log2(float value) {
  uint32_t bits;
  if (!(value > 0.0f)) return -0x7fff; /* a coordinate that does not move at all */
  memcpy(&bits, &value, sizeof(bits));
  /* Sixteen times the exponent, plus the top four mantissa bits. */
  return (int)((bits >> 19) & 0x1fffu) - (127 * 16);
}

/* fabsf, which strict floating point leaves as a call: the sign bit
 * cleared, which is all fabsf does, NaNs included. */
static float magnitude(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  bits &= 0x7fffffffu;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/* The level of detail for a pixel, in sixteenths, already biased and
 * clamped. */
static int level_of_detail(const GcGxTexture *texture, const float gradient[4]) {
  const float du_dx = gradient[0] * (float)texture->width;
  const float dv_dx = gradient[1] * (float)texture->height;
  const float du_dy = gradient[2] * (float)texture->width;
  const float dv_dy = gradient[3] * (float)texture->height;
  const float su = magnitude(du_dx), sv = magnitude(dv_dx);
  const float tu = magnitude(du_dy), tv = magnitude(dv_dy);
  /* Two cheap stand-ins for the length of the pixel's footprint. Neither
   * is a Euclidean length; there is no square root anywhere in this
   * pipeline. The diagonal form is never smaller than the edge form, so it
   * leans towards blurrier levels. */
  const float s_delta = texture->diagonal_lod ? (su + tu) : (su > tu ? su : tu);
  const float t_delta = texture->diagonal_lod ? (sv + tv) : (sv > tv ? sv : tv);
  const float delta = (s_delta > t_delta) ? s_delta : t_delta;
  int lod = fixed_log2(delta);

  /* The bias is in thirty-seconds and the level of detail in sixteenths,
   * so it arrives halved - an arithmetic shift, rounding towards minus
   * infinity. */
  lod += texture->lod_bias >> 1;
  if (lod > (int)texture->max_lod) lod = (int)texture->max_lod;
  if (lod < (int)texture->min_lod) lod = (int)texture->min_lod;
  return lod;
}

/* floorf, for a value about to become an int. (int)floorf(v) is a library
 * call under strict floating point; this is the same number for every
 * input - including the ones the conversion cannot represent, which come
 * out as INT_MIN either way. */
static int floor_to_int(float value) {
  const int truncated = (int)value;
  if ((float)truncated > value && truncated != INT_MIN) return truncated - 1;
  return truncated;
}

/* ------------------------------------------------------------------ */
/* A bound unit                                                        */

void gc_gx_texture_bind(GcMachine *m, unsigned unit, unsigned owner, GcGxBoundTexture *out) {
  GcTexFormat f;
  unsigned k;
  int reached_one = 0;

  gc_gx_texture_read(m, unit, &out->texture);
  f = format_of(out->texture.format);
  out->tile_shift_s = tile_shift(f.tile_width);
  out->tile_shift_t = tile_shift(f.tile_height);
  out->bytes_per_tile = f.bytes_per_tile;
  out->owner = owner;

  /* Every level level_of can be asked for, by the same steps: level k is
   * level k-1 halved, until a halving has produced one texel by one, after
   * which it stays put. */
  for (k = 0; k < GC_GX_TEX_LEVELS; k++) {
    GcGxTexLevel *level = &out->level[k];
    if (k == 0) {
      level->address = out->texture.address;
      level->width = out->texture.width;
      level->height = out->texture.height;
    } else {
      *level = out->level[k - 1u];
      if (!reached_one) {
        level->address += level_bytes(out->texture.format, level->width, level->height);
        level->width = (level->width > 1u) ? (level->width >> 1) : 1u;
        level->height = (level->height > 1u) ? (level->height >> 1) : 1u;
        reached_one = (level->width == 1u && level->height == 1u);
      }
    }
    level->tiles_per_row = (level->width + f.tile_width - 1u) / f.tile_width;
    out->slot[k] = NULL;
  }
}

/* One texel of one level of a bound unit: wrap, find the tile, decode. */
static void bound_texel(GcMachine *m, GcGxBoundTexture *bound, unsigned level_index, int s, int t,
                        uint8_t out[4]) {
  const GcGxTexLevel *level = &bound->level[level_index];
  const unsigned ws = wrap(s, level->width, bound->texture.wrap_s);
  const unsigned wt = wrap(t, level->height, bound->texture.wrap_t);
  const unsigned tile = (wt >> bound->tile_shift_t) * level->tiles_per_row + (ws >> bound->tile_shift_s);
  const uint32_t base = level->address + tile * bound->bytes_per_tile;
  const unsigned k = ((wt & ((1u << bound->tile_shift_t) - 1u)) << bound->tile_shift_s) +
                     (ws & ((1u << bound->tile_shift_s) - 1u));
  decode_at(m, &bound->texture, base, k, ws, wt, out);
}

/* The four texels of a bilinear footprint in a compressed level: what four
 * bound_texel calls would give, with each axis wrapped once and each of the
 * (at most four, usually one) blocks it touches read once.
 *
 * bound_texel on a compressed texture finds the 8x8 tile, then decode_at
 * finds the 4x4 block within it from the wrapped coordinates, reads the
 * block's endpoint words and the row's index byte, and looks the colours up.
 * Here the block's address is the same arithmetic grouped by block, and a
 * block whose eight bytes lie inside main memory is read directly; anything
 * else takes the byte-at-a-time route that reads outside memory as zero. */
static const uint8_t *compressed_block(const GcMachine *m, uint32_t at) {
  uint16_t c0, c1;
  uint32_t key;
  GcCompressedColours *memo;
  if (at < gc_ram_size - 8u) {
    const uint8_t *p = m->mem1 + at;
    c0 = (uint16_t)((p[0] << 8) | p[1]);
    c1 = (uint16_t)((p[2] << 8) | p[3]);
  } else {
    c0 = texture_halfword(m, at);
    c1 = texture_halfword(m, at + 2u);
  }
  key = ((uint32_t)c0 << 16) | c1;
  memo = &compressed_memo[(key ^ (key >> 13) ^ (key >> 22)) & (GC_CMPR_MEMO - 1u)];
  if (!memo->valid || memo->key != key) {
    compressed_palette(c0, c1, memo->colour);
    memo->key = key;
    memo->valid = 1;
  }
  return (const uint8_t *)memo->colour;
}

static void compressed_texel(const GcMachine *m, uint32_t at, const uint8_t *colours, unsigned s,
                             unsigned t, uint8_t out[4]) {
  const uint8_t indices = texture_byte(m, at + 4u + (t & 3u));
  const unsigned index = (indices >> (6u - 2u * (s & 3u))) & 3u;
  memcpy(out, colours + 4u * index, 4);
}

static void compressed_footprint(GcMachine *m, GcGxBoundTexture *bound, unsigned level_index,
                                 int s0, int t0, uint8_t a[4], uint8_t b[4], uint8_t c[4],
                                 uint8_t d[4]) {
  const GcGxTexLevel *level = &bound->level[level_index];
  const unsigned ws[2] = {wrap(s0, level->width, bound->texture.wrap_s),
                          wrap(s0 + 1, level->width, bound->texture.wrap_s)};
  const unsigned wt[2] = {wrap(t0, level->height, bound->texture.wrap_t),
                          wrap(t0 + 1, level->height, bound->texture.wrap_t)};
  uint8_t *const out[4] = {a, b, c, d};
  uint32_t seen_at[4];
  /* Copied out of the memo, because looking up the next block may reuse
   * the slot this one came from. */
  uint8_t seen_colours[4][16];
  unsigned seen = 0, i, k;
  /* The tile shift for this format is three on both axes (8x8 tiles of
   * thirty-two bytes), and a block is a quarter of a tile. Written the way
   * bound_texel and decode_compressed write it. */
  const unsigned shift_s = bound->tile_shift_s, shift_t = bound->tile_shift_t;
  if ((ws[0] >> 2) == (ws[1] >> 2) && (wt[0] >> 2) == (wt[1] >> 2)) {
    /* All four texels in one block - most footprints: a block is a column
     * of four texels (s >> 2) in a row of four (t >> 2), which fixes both
     * its tile and which quarter of the tile it is. One lookup, and nothing
     * after it that could reuse the memo slot, so no copy. */
    const unsigned tile = (wt[0] >> shift_t) * level->tiles_per_row + (ws[0] >> shift_s);
    const uint32_t at = level->address + tile * bound->bytes_per_tile + (((wt[0] & 4u) >> 1) | ((ws[0] & 4u) >> 2)) * 8u;
    const uint8_t *const colours = compressed_block(m, at);
    for (i = 0; i < 4u; i++) compressed_texel(m, at, colours, ws[i & 1u], wt[i >> 1], out[i]);
    return;
  }
  for (i = 0; i < 4u; i++) {
    const unsigned s = ws[i & 1u], t = wt[i >> 1];
    const unsigned tile = (t >> shift_t) * level->tiles_per_row + (s >> shift_s);
    const uint32_t base = level->address + tile * bound->bytes_per_tile;
    const unsigned sub = ((t & 4u) >> 1) | ((s & 4u) >> 2);
    const uint32_t at = base + sub * 8u;
    const uint8_t *colours = NULL;
    for (k = 0; k < seen; k++) {
      if (seen_at[k] == at) {
        colours = seen_colours[k];
        break;
      }
    }
    if (!colours) {
      memcpy(seen_colours[seen], compressed_block(m, at), 16);
      seen_at[seen] = at;
      colours = seen_colours[seen];
      seen++;
    }
    compressed_texel(m, at, colours, s, t, out[i]);
  }
}

/* One level, point-sampled or bilinear. */
static void sample_level(GcMachine *m, GcGxBoundTexture *bound, unsigned level_index, float s,
                         float t, int linear, uint8_t out[4]) {
  const GcGxTexLevel *level = &bound->level[level_index];

  if (!linear) {
    /* Point sampling: the texel the coordinate lands in. */
    bound_texel(m, bound, level_index, floor_to_int(s * (float)level->width),
                floor_to_int(t * (float)level->height), out);
    return;
  }

  /* Four texels, weighted by how far the sample is from each. The
   * half-texel offset is what makes a coordinate at the centre of a texel
   * return that texel exactly rather than a blend of it with its
   * neighbour.
   *
   * **Unverified:** the weights' precision. The hardware interpolates in
   * fixed point with seven bits of weight; this uses the full float and
   * rounds, which can differ by a step. */
  {
    const float fs = s * (float)level->width - 0.5f;
    const float ft = t * (float)level->height - 0.5f;
    const int s0 = floor_to_int(fs), t0 = floor_to_int(ft);
    const float ws = fs - (float)s0, wt = ft - (float)t0;
    uint8_t a[4], b[4], c[4], d[4];
    unsigned i;

    if (bound->texture.format == GC_TF_CMPR) {
      compressed_footprint(m, bound, level_index, s0, t0, a, b, c, d);
    } else {
      bound_texel(m, bound, level_index, s0, t0, a);
      bound_texel(m, bound, level_index, s0 + 1, t0, b);
      bound_texel(m, bound, level_index, s0, t0 + 1, c);
      bound_texel(m, bound, level_index, s0 + 1, t0 + 1, d);
    }

#if GC_TEXTURE_SSE2
    /* The same four channels as the loop below, side by side: the same
     * single-precision operations in the same order, so the same bits. The
     * clamp agrees too, for a NaN included - maxps hands back its second
     * operand, zero, where the comparisons below fall through to a
     * conversion that also comes out as zero in the low byte. */
    (void)i;
    {
      const __m128i zero_i = _mm_setzero_si128();
      uint32_t wa, wb, wc, wd, packed;
      __m128 A, B, C, D, top, bottom, value;
      const __m128 WS = _mm_set1_ps(ws), WT = _mm_set1_ps(wt);
      memcpy(&wa, a, 4);
      memcpy(&wb, b, 4);
      memcpy(&wc, c, 4);
      memcpy(&wd, d, 4);
#define GC_WIDEN(word) \
  _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)(word)), zero_i), zero_i))
      A = GC_WIDEN(wa);
      B = GC_WIDEN(wb);
      C = GC_WIDEN(wc);
      D = GC_WIDEN(wd);
#undef GC_WIDEN
      top = _mm_add_ps(A, _mm_mul_ps(WS, _mm_sub_ps(B, A)));
      bottom = _mm_add_ps(C, _mm_mul_ps(WS, _mm_sub_ps(D, C)));
      value = _mm_add_ps(_mm_add_ps(top, _mm_mul_ps(WT, _mm_sub_ps(bottom, top))), _mm_set1_ps(0.5f));
      value = _mm_min_ps(_mm_max_ps(value, _mm_setzero_ps()), _mm_set1_ps(255.0f));
      packed = (uint32_t)_mm_cvtsi128_si32(
          _mm_packus_epi16(_mm_packs_epi32(_mm_cvttps_epi32(value), zero_i), zero_i));
      memcpy(out, &packed, 4);
    }
#else
    for (i = 0; i < 4; i++) {
      const float top = (float)a[i] + ws * ((float)b[i] - (float)a[i]);
      const float bottom = (float)c[i] + ws * ((float)d[i] - (float)c[i]);
      const float value = top + wt * (bottom - top) + 0.5f;
      out[i] = (uint8_t)(value < 0.0f ? 0.0f : (value > 255.0f ? 255.0f : value));
    }
#endif
  }
}

/* The whole of it: pick a level, or two, and read them.
 *
 * Which filter applies is one comparison on the *final* level of detail,
 * after the bias and the clamp: above zero minifies, zero or below
 * magnifies. The tie at exactly zero magnifying is what the reference
 * emulator does; the SDK's prose puts a texel-to-pixel ratio of exactly
 * one in the other bucket. **Unverified**, one texel wide, noted rather
 * than agonised over.
 *
 * The magnifying filter has no mip mode, so a magnifying pixel reads one
 * level and that level is the minimum the range allows. There is no such
 * thing as magnifying level three. */
static void sample_with_lod(GcMachine *m, GcGxBoundTexture *bound, float s, float t, int lod,
                            uint8_t out[4]) {
  const GcGxTexture *texture = &bound->texture;

  if (lod <= 0) {
    sample_level(m, bound, texture->min_lod >> 4, s, t, (int)texture->magnify_linear, out);
    return;
  }

  {
    const unsigned base = (unsigned)(lod >> 4);
    const unsigned fraction = (unsigned)(lod & 15);
    const int linear = (int)texture->minify_linear;

    if (texture->mip_mode == 0u) {
      sample_level(m, bound, base, s, t, linear, out);
      return;
    }
    if (texture->mip_mode == 1u) {
      /* One level, rounded to nearest rather than down, so a level of two
       * and a half reads level three. */
      sample_level(m, bound, base + (fraction >= 8u ? 1u : 0u), s, t, linear, out);
      return;
    }
    {
      /* Two levels, blended by the fraction. */
      uint8_t near_texel[4], far_texel[4];
      unsigned i;
      sample_level(m, bound, base, s, t, linear, near_texel);
      sample_level(m, bound, base + 1u, s, t, linear, far_texel);
      for (i = 0; i < 4; i++) {
        const unsigned mixed =
            ((unsigned)near_texel[i] * (16u - fraction) + (unsigned)far_texel[i] * fraction) >> 4;
        out[i] = (uint8_t)mixed;
      }
    }
  }
}

void gc_gx_texcoord_scale(const GcMachine *m, unsigned coordinate, unsigned map, float out[2]) {
  const unsigned block = (map & 4u) ? BP_TEXTURE_HIGH : BP_TEXTURE_LOW;
  const uint32_t size = m->gx.bp[block + 8u + (map & 3u)];
  const unsigned width = (size & 0x3ffu) + 1u;
  const unsigned height = ((size >> 10) & 0x3ffu) + 1u;
  const uint32_t s_size = m->gx.bp[BP_TEXCOORD_SCALE + 2u * (coordinate & 7u)];
  const uint32_t t_size = m->gx.bp[BP_TEXCOORD_SCALE + 2u * (coordinate & 7u) + 1u];
  out[0] = (float)((s_size & 0xffffu) + 1u) / (float)width;
  out[1] = (float)((t_size & 0xffffu) + 1u) / (float)height;
}

void gc_gx_texture_sample_bound(GcMachine *m, GcGxBoundTexture *bound, float s, float t,
                                const float gradient[4], uint8_t out[4]) {
  if (!bound->texture.width || !bound->texture.height) {
    out[0] = out[1] = out[2] = out[3] = 0;
    return;
  }
  /* level_of_detail clamps to max_lod and then to min_lod, so a range of one
   * level, or none, gives min_lod whatever the gradient says. */
  sample_with_lod(m, bound, s, t,
                  bound->texture.max_lod <= bound->texture.min_lod ? (int)bound->texture.min_lod
                                                                  : level_of_detail(&bound->texture, gradient),
                  out);
}

void gc_gx_texture_sample_gradient(GcMachine *m, unsigned unit, float s, float t,
                                   const float gradient[4], uint8_t out[4]) {
  GcGxBoundTexture bound;
  gc_gx_texture_bind(m, unit, GC_GX_TEX_OWNERS, &bound);
  gc_gx_texture_sample_bound(m, &bound, s, t, gradient, out);
}

void gc_gx_texture_sample(GcMachine *m, unsigned unit, float s, float t, uint8_t out[4]) {
  /* No gradient, so nothing to compute a level from: the clamp lifts the
   * result to the minimum of the range, which is the level a magnifying
   * pixel would have read anyway. */
  static const float still[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  gc_gx_texture_sample_gradient(m, unit, s, t, still, out);
}
