/*
 * The pixel engine's output stage: the alpha test, blending, dithering and
 * the two write enables.
 *
 * Register layouts in docs/gc-bp-registers.md sections 5 and 6.
 */
#ifndef GC_GX_BLEND_H
#define GC_GX_BLEND_H

#include <stdint.h>

#include "gc_efb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Does this pixel's alpha survive the two comparisons and the logic
 * between them? */
int gc_gx_alpha_test(const GcMachine *m, int alpha);

/* Does the depth test happen before the texture stages or after? A pixel
 * the alpha test kills has still updated the depth buffer in the first
 * case and has not in the second. */
int gc_gx_early_depth_test(const GcMachine *m);

/* One finished pixel into the framebuffer, through whatever blending,
 * logic operation and write enables are configured. */
void gc_gx_write_pixel(GcMachine *m, unsigned x, unsigned y, const uint8_t source[4]);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_BLEND_H */
