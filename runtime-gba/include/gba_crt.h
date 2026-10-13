/*
 * The CRT display filter's picture (gba_crt.c): a frame drawn as a CRT
 * television shows it, with glowing scanlines, a phosphor mask and a soft
 * halo around bright areas. Nothing in it is GBA-specific; any frontend
 * with an opaque ARGB frame can use it.
 */
#ifndef GBA_CRT_H
#define GBA_CRT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Picture columns gba_crt_render draws per source pixel. */
enum { GBA_CRT_COLUMNS = 4 };
/* Picture rows per source line that gba_crt_render accepts. */
enum { GBA_CRT_MIN_LINE = 2, GBA_CRT_MAX_LINE = 32 };

typedef struct gba_crt gba_crt;

/* NULL when out of memory. */
gba_crt *gba_crt_create(void);
void gba_crt_free(gba_crt *crt);

/* Draws src (w x h opaque ARGB pixels) as a CRT's picture of
 * (w * GBA_CRT_COLUMNS) x (h * line_px) ARGB pixels into dst. Each source
 * line becomes a beam line_px rows tall, blurred a little along the line;
 * a beam is thin where it is dark and swells as it brightens, so dark
 * areas show black gaps between lines and bright ones nearly close them.
 * The picture is brightened to make up for the mask drawn over it
 * (gba_crt_mask). Returns 0, leaving dst unchanged, when line_px is out of
 * range or the work buffers cannot be allocated. */
int gba_crt_render(gba_crt *crt, const uint32_t *src, int w, int h, int line_px, uint32_t *dst);

/* The halo: a blurred copy of src (w x h) into dst (w x h), to be scaled
 * to the picture's size and added over it at GBA_CRT_GLOW_ALPHA of 255.
 * Returns 0 when its buffer cannot be allocated. */
int gba_crt_glow(gba_crt *crt, const uint32_t *src, int w, int h, uint32_t *dst);
enum { GBA_CRT_GLOW_ALPHA = 72 };

/* The phosphor mask's multiplier for screen column x (an aperture grille
 * of red, green and blue stripes, one screen pixel each), as ARGB, to be
 * drawn multiplied over the finished picture. */
uint32_t gba_crt_mask(int x);

#ifdef __cplusplus
}
#endif

#endif
