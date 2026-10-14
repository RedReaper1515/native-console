/*
 * The texture unit.
 *
 * Eight units, each with a format, a size, an address in main memory and a
 * pair of wrap modes, described by four registers apiece - at 0x80 for the
 * first four units and 0xa0 for the second four.
 *
 * The formats and the address arithmetic are in
 * docs/gc-texture-formats.md, which also records which decode rules are
 * settled, which are disputed and which are still open.
 */
#ifndef GC_GX_TEXTURE_H
#define GC_GX_TEXTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;

typedef struct GcGxTexture {
  uint32_t address; /* physical, where the texels are */
  unsigned width;
  unsigned height;
  unsigned format;
  unsigned wrap_s;
  unsigned wrap_t;
  unsigned magnify_linear;

  /* The three filter bits are two independent things. Bit 7 says how the
   * four texels *within* a level are combined; bits 6:5 say how many
   * levels are read and how those are combined. The magnifying filter is
   * a separate single bit and never has a mip mode, which is structural:
   * a magnifying pixel always reads one level. */
  unsigned minify_linear;
  unsigned mip_mode; /* 0 none, 1 nearest level, 2 blend two levels */
  /* The LOD range is unsigned and in sixteenths, so neither end can be
   * negative - and a minimum above zero makes magnification impossible.
   * The bias is signed and in *thirty-seconds*, which is why it is halved
   * before it meets the rest. */
  int lod_bias;  /* 1/32 steps, signed */
  unsigned min_lod; /* 1/16 steps */
  unsigned max_lod; /* 1/16 steps */
  unsigned diagonal_lod;

  /* Which lookup table this unit reads, and in what format. */
  unsigned palette_format;
  unsigned palette_offset; /* in the table memory, in 512-byte units */
} GcGxTexture;

/* The palette memory a lookup-table load fills: the upper half of the
 * texture cache, which is 512 KB. The destination register names it in
 * 512-byte units, and an entry is two bytes, so that is 1024 units of 256
 * entries each. Sizing this by guess rather than by that arithmetic leaves
 * every load falling off the end and every paletted texture black. */
#define GC_GX_TLUT_UNIT_ENTRIES 256
#define GC_GX_TLUT_ENTRIES (1024 * GC_GX_TLUT_UNIT_ENTRIES)

typedef struct GcGxTlut {
  uint16_t entry[GC_GX_TLUT_ENTRIES];
} GcGxTlut;

/* A load, as the two lookup-table registers describe it. Returns non-zero
 * when the register was one of them. */
int gc_gx_tlut_write(GcMachine *m, unsigned reg, uint32_t payload);

/* What the registers say one unit is bound to. */
void gc_gx_texture_read(GcMachine *m, unsigned unit, GcGxTexture *out);

/* The mip chain, piece by piece, for a caller that wants to hand the whole
 * thing to something else - the OpenGL path uploads it to the card. A
 * level knows its own address and size; there is only one address register
 * a unit and it points at level zero, so every other level is derived. */
void gc_gx_texture_level(const GcGxTexture *texture, unsigned level, GcGxTexture *out);
unsigned gc_gx_texture_levels(const GcGxTexture *texture);
/* The bytes of main memory sampling the texture can read (every level the
 * level of detail can reach), cut to main memory: [*first, *first + *size). */
void gc_gx_texture_span(const GcGxTexture *texture, uint32_t *first, uint32_t *size);
void gc_gx_texture_texel(GcMachine *m, const GcGxTexture *level, unsigned s, unsigned t,
                         uint8_t out[4]);

/* What turns a coordinate as the transform produced it into the nought-to-
 * one range of one texture map: `out[0]` multiplies s and `out[1]` t.
 *
 * The transform's coordinates are not normalised to any texture. The
 * rasteriser scales each coordinate by its own pair of size registers
 * (0x30 + 2n and 0x31 + 2n, sixteen bits of size minus one) into texels,
 * and the sampler reads texels. The SDK normally writes the bound texture's
 * size there, which makes the two readings agree; a game that sets the scale
 * itself - GX_SetTexCoordScaleManually - and hands over texel coordinates
 * is where they part. */
void gc_gx_texcoord_scale(const GcMachine *m, unsigned coordinate, unsigned map, float out[2]);

/* One texel, at a coordinate in the usual nought-to-one range, with no
 * idea how fast that coordinate is changing - so no level of detail, and
 * the magnifying filter on the lowest level the range allows. */
void gc_gx_texture_sample(GcMachine *m, unsigned unit, float s, float t, uint8_t out[4]);

/* The same, for a caller that knows how the coordinate changes across the
 * screen. `gradient` is ds/dx, dt/dx, ds/dy, dt/dy in the same nought-to-
 * one units as the coordinate; the texture's own size is folded in here.
 * That gradient is what picks the level. */
void gc_gx_texture_sample_gradient(GcMachine *m, unsigned unit, float s, float t,
                                   const float gradient[4], uint8_t out[4]);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_TEXTURE_H */
