/*
 * The texture environment's registers, read once a triangle.
 *
 * Private for the same reason as gc_gx_texture_bound.h. A triangle cannot
 * write a register, so what gc_gx_tev_run read for every pixel is the same
 * for every pixel of one triangle; this reads it once and the per-pixel run
 * does the arithmetic alone.
 */
#ifndef GC_GX_TEV_PLAN_H
#define GC_GX_TEV_PLAN_H

#include "gc_gx_tev.h"
#include "gc_gx_texture_bound.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcGxTevStagePlan {
  uint32_t colour_env;
  uint32_t alpha_env;
  int textured;
  int indirect; /* the stage names an indirect matrix */
  unsigned map;
  unsigned coordinate;
  float scale[2];
  GcGxBoundTexture texture; /* bound only when textured */
} GcGxTevStagePlan;

typedef struct GcGxTevPlan {
  unsigned stages;
  /* Which of the eight coordinate sets any stage reads, directly or through
   * an indirect lookup. A set outside this mask is never looked at. */
  unsigned coordinates_used;
  /* Of those, the sets some stage samples a texture through whose level of
   * detail can vary (max_lod above min_lod). A texture whose range is one
   * level reads that level whatever the gradient, so the other sets' gradients
   * are never looked at, and may be left zero. */
  unsigned gradients_used;
  GcGxTevStagePlan stage[16];
} GcGxTevPlan;

void gc_gx_tev_prepare(GcMachine *m, GcGxTevPlan *plan);

/* gc_gx_tev_run, with the registers already read and the coordinates
 * handed over directly. Only the sets in `coordinates_used` need be
 * filled in. The colour registers a stage reads and writes are `tev`'s:
 * the machine's own, or a copy a helper thread works in. */
void gc_gx_tev_run_planned(GcMachine *m, GcGxTevPlan *plan, GcGxTev *tev,
                           const float coordinate[8][2], const float gradient[8][4],
                           GcGxTevPixel *pixel);

/* Whether every pixel's combiner is independent of the pixel before it:
 * no stage reads a colour register group (a register's colour channels,
 * or its alpha) that some stage writes, unless an earlier write in the same
 * pixel already replaced it. Then what a pixel computes, and the registers
 * it leaves behind, depend on that pixel alone. */
int gc_gx_tev_plan_independent(const GcGxTevPlan *plan);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_TEV_PLAN_H */
