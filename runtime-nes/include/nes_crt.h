/*
 * The CRT display filter's picture (nes_crt.c): a frame drawn as a CRT
 * television shows it, with glowing scanlines, a phosphor mask and a soft
 * halo around bright areas. A copy of the Super Nintendo runtime's
 * (runtime-snes/src/snes_crt.c): it takes a row stride and a column count,
 * so the frontend can draw the frame with or without its overscan lines.
 */
#ifndef NES_CRT_H
#define NES_CRT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Picture columns per source pixel that nes_crt_render accepts. */
enum { NES_CRT_MAX_COLUMNS = 8 };
/* Picture rows per source line that nes_crt_render accepts. */
enum { NES_CRT_MIN_LINE = 2, NES_CRT_MAX_LINE = 32 };

typedef struct nes_crt nes_crt;

/* NULL when out of memory. */
nes_crt *nes_crt_create(void);
void nes_crt_free(nes_crt *crt);

/* Draws src (w x h opaque ARGB pixels, stride pixels from one row to the
 * next) as a CRT's picture of (w * columns) x (h * line_px) ARGB pixels
 * into dst. Each source line becomes a beam line_px rows tall, blurred a
 * little along the line; a beam is thin where it is dark and swells as it brightens, so dark
 * areas show black gaps between lines and bright ones nearly close them.
 * The picture is brightened to make up for the mask drawn over it
 * (nes_crt_mask). Returns 0, leaving dst unchanged, when columns or
 * line_px is out of range or the work buffers cannot be allocated. */
int nes_crt_render(nes_crt *crt, const uint32_t *src, int w, int h, int stride, int columns, int line_px,
                    uint32_t *dst);

/* The halo: a blurred copy of src (w x h, stride as above) into dst
 * (w x h), to be scaled to the picture's size and added over it at
 * NES_CRT_GLOW_ALPHA of 255.
 * Returns 0 when its buffer cannot be allocated. */
int nes_crt_glow(nes_crt *crt, const uint32_t *src, int w, int h, int stride, uint32_t *dst);
enum { NES_CRT_GLOW_ALPHA = 72 };

/* The phosphor mask's multiplier for screen column x (an aperture grille
 * of red, green and blue stripes, one screen pixel each), as ARGB, to be
 * drawn multiplied over the finished picture. */
uint32_t nes_crt_mask(int x);

#ifdef __cplusplus
}
#endif

#endif
