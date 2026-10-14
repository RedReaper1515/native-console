/*
 * The texture environment: sixteen combiner stages between the rasteriser
 * and the framebuffer.
 *
 * Four colour registers carry values between stages at eleven signed bits
 * a channel, which is wider than anything that reaches the framebuffer -
 * the extra range exists only so a stage can hand an intermediate to the
 * next one. Four konstant registers hold values a stage can select but not
 * write.
 *
 * The arithmetic is in docs/gc-tev.md, measured on a console by
 * dolphin-emu/hwtests; gc_gx_tev.c implements it step by numbered step.
 */
#ifndef GC_GX_TEV_H
#define GC_GX_TEV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;

/* The state the command stream loads: four colour registers, four konstant
 * registers, four swap tables, and the konstant selection per stage. The
 * registers are signed because a stage can write a negative intermediate. */
typedef struct GcGxTev {
  int reg[4][4];       /* prev, and three more; red, green, blue, alpha */
  int konstant[4][4];
  uint8_t swap[4][4];  /* four tables of four two-bit channel selectors */
  uint8_t konstant_color[16];
  uint8_t konstant_alpha[16];
} GcGxTev;

/* What one pixel brings into the combiner, and what it leaves with. */
typedef struct GcGxTevPixel {
  int raster[4];  /* the interpolated vertex colour */
  int texture[4]; /* this stage's texture sample */
  unsigned konstant_color;
  unsigned konstant_alpha;
  uint8_t result[4]; /* red, green, blue, alpha, as the pixel engine sees them */
} GcGxTevPixel;

void gc_gx_tev_reset(GcGxTev *tev);

/* A blitting-processor write that belongs to the texture environment: the
 * colour and konstant registers, and the konstant selection and swap
 * tables. Returns non-zero when it was one. */
int gc_gx_tev_write(GcMachine *m, unsigned reg, uint32_t payload);

/* The texture coordinates this pixel interpolated, one pair per generator,
 * and how fast each is changing across the screen - ds/dx, dt/dx, ds/dy,
 * dt/dy. A stage samples with whichever of them its order register names,
 * and the gradient is what chooses the mipmap level. */
void gc_gx_tev_set_texture(GcMachine *m, const float coordinate[8][2],
                           const float gradient[8][4]);

/* One stage's konstant colour and alpha, which are a function of the
 * register file alone. */
void gc_gx_tev_konstant(const GcMachine *m, unsigned stage, int out[4]);

/* Every stage, in order, on one pixel. */
void gc_gx_tev_run(GcMachine *m, GcGxTevPixel *pixel);

#ifdef __cplusplus
}
#endif

#endif /* GC_GX_TEV_H */
