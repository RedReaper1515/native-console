/*
 * A texture unit resolved once, for a caller about to sample it many times.
 *
 * Private to the rasteriser, the texture environment and the sampler. It is
 * deliberately not in include/: gc.h pulls in the public texture header, and
 * every generated project would rebuild for a change here.
 *
 * Nothing in this is a new rule. It is the same registers read the same way
 * as gc_gx_texture_read and level_of, worked out once a triangle instead of
 * once a texel - which is only sound because nothing a triangle does can
 * write a register or main memory while it is being drawn.
 */
#ifndef GC_GX_TEXTURE_BOUND_H
#define GC_GX_TEXTURE_BOUND_H

#include "gc_gx_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The level of detail is clamped to max_lod, eight bits of sixteenths, so
 * the deepest level a sample can name is 15 and a blend reads one past it. */
#define GC_GX_TEX_LEVELS 17

typedef struct GcGxTexLevel {
  uint32_t address;
  unsigned width;
  unsigned height;
  unsigned tiles_per_row;
} GcGxTexLevel;

struct GcGxTexCacheSlot;

typedef struct GcGxBoundTexture {
  GcGxTexture texture;
  GcGxTexLevel level[GC_GX_TEX_LEVELS];
  unsigned tile_shift_s; /* log2 of the tile width */
  unsigned tile_shift_t;
  unsigned bytes_per_tile;
  /* The decoded-texel cache for each level, found on first use. Which set of
   * slots is this binding's to use; a stage owns one set. */
  struct GcGxTexCacheSlot *slot[GC_GX_TEX_LEVELS];
  unsigned owner;
} GcGxBoundTexture;

/* Read one unit's registers and work out every level's address and size.
 * `owner` names the cache slots this binding may use, and must be below
 * GC_GX_TEX_OWNERS; two bindings live at once must not share one. */
#define GC_GX_TEX_OWNERS 16
void gc_gx_texture_bind(GcMachine *m, unsigned unit, unsigned owner, GcGxBoundTexture *out);

/* gc_gx_texture_sample_gradient, on a bound unit. Identical results. */
void gc_gx_texture_sample_bound(GcMachine *m, GcGxBoundTexture *bound, float s, float t,
                                const float gradient[4], uint8_t out[4]);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_TEXTURE_BOUND_H */
