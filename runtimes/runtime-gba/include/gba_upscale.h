/*
 * Pixel-art upscaling for the display filters (gba_upscale.c).
 */
#ifndef GBA_UPSCALE_H
#define GBA_UPSCALE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MMPX 2x magnification (McGuire and Gagiu, "MMPX Style-Preserving Pixel
 * Art Magnification", JCGT 2021): src is w x h ARGB pixels, dst receives
 * 2w x 2h. Every output pixel is a copy of a source pixel, so the palette
 * is kept; edges beyond the image repeat the border pixels. src and dst
 * must not overlap. Returns 0 when it cannot allocate its work buffer
 * (dst is then left unchanged). */
int gba_mmpx_scale2x(const uint32_t *src, int w, int h, uint32_t *dst);

#ifdef __cplusplus
}
#endif

#endif
