/*
 * The CRT display filter's picture (snes_crt.c): a frame drawn as a CRT
 * television shows it, with glowing scanlines, a phosphor mask and a soft
 * halo around bright areas. A copy of the Game Boy Advance runtime's
 * (runtimes/runtime-gba/src/gba_crt.c) that also takes a row stride and a column
 * count, for frames 256 or 512 pixels wide.
 */
#ifndef SNES_CRT_H
#define SNES_CRT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Picture columns per source pixel that snes_crt_render accepts. */
enum { SNES_CRT_MAX_COLUMNS = 8 };
/* Picture rows per source line that snes_crt_render accepts. */
enum { SNES_CRT_MIN_LINE = 2, SNES_CRT_MAX_LINE = 32 };

typedef struct snes_crt snes_crt;

/* NULL when out of memory. */
snes_crt *snes_crt_create(void);
void snes_crt_free(snes_crt *crt);

/* Draws src (w x h opaque ARGB pixels, stride pixels from one row to the
 * next) as a CRT's picture of (w * columns) x (h * line_px) ARGB pixels
 * into dst. Each source line becomes a beam line_px rows tall, blurred a
 * little along the line; a beam is thin where it is dark and swells as it brightens, so dark
 * areas show black gaps between lines and bright ones nearly close them.
 * The picture is brightened to make up for the mask drawn over it
 * (snes_crt_mask). Returns 0, leaving dst unchanged, when columns or
 * line_px is out of range or the work buffers cannot be allocated. */
int snes_crt_render(snes_crt *crt, const uint32_t *src, int w, int h, int stride, int columns, int line_px,
                    uint32_t *dst);

/* The halo: a blurred copy of src (w x h, stride as above) into dst
 * (w x h), to be scaled to the picture's size and added over it at
 * SNES_CRT_GLOW_ALPHA of 255.
 * Returns 0 when its buffer cannot be allocated. */
int snes_crt_glow(snes_crt *crt, const uint32_t *src, int w, int h, int stride, uint32_t *dst);
enum { SNES_CRT_GLOW_ALPHA = 72 };

/* The phosphor mask's multiplier for screen column x (an aperture grille
 * of red, green and blue stripes, one screen pixel each), as ARGB, to be
 * drawn multiplied over the finished picture. */
uint32_t snes_crt_mask(int x);

#ifdef __cplusplus
}
#endif

#endif
