/*
 * Fog: the pixel engine's last piece of colour arithmetic, between the
 * texture environment and the framebuffer blend.
 *
 * The hardware does not hold the near, far, start and end distances a game
 * passes to GX_SetFog. It holds three constants A, B and C, a shift, and a
 * three-bit function select, and evaluates one fixed rational expression
 * in the interpolated depth. The change of variables that turns four
 * distances into those constants lives in the SDK, which means the
 * constants are what has to be reproduced here.
 *
 * The algebra, the register encodings and - importantly - which parts of
 * this are guesses are set out in docs/gc-fog-indirect.md, part A. Two
 * things there are worth repeating at the point of use: the exponential
 * curves are **not** established by any source that document's author
 * read, and the range-adjustment table's indexing is inference. Both are
 * marked where they are implemented.
 */
#ifndef GC_GX_FOG_H
#define GC_GX_FOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;

/* Fog one pixel in place. `depth` is the 24-bit screen-space depth the
 * rasteriser interpolated - the same value that reaches the Z buffer - and
 * `x` is its screen column, which the horizontal range adjustment needs.
 * Alpha is never touched: the fog colour register has no alpha field.
 *
 * Does nothing when the function select is zero, which is what GX_Init
 * leaves behind, so the ordinary path costs one register read. */
void gc_gx_fog_apply(const GcMachine *m, uint32_t depth, uint32_t x, uint8_t rgba[4]);

/* The fog factor on its own, 0 for unfogged and 1 for fully fogged, for
 * tests that want to check the curve without a framebuffer. Returns a
 * negative number when fog is off. */
double gc_gx_fog_factor(const GcMachine *m, uint32_t depth, uint32_t x);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_FOG_H */
