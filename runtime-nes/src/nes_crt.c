/*
 * The CRT display filter's picture.
 *
 * Light is worked in linear terms (12 bits, 0-4095) and turned back into
 * screen values at the end. Each source line is first blurred along the
 * line into the requested number of columns per pixel. Every picture row then lies
 * between two source lines, and takes light from both through the beam
 * profile: a Gaussian across the line whose width grows with the light's
 * brightness, per colour channel. The profile depends only on the row's
 * place within its line and on the brightness, so it is a table lookup;
 * the tables are made again when the line height changes. A copy of
 * runtime-snes/src/snes_crt.c (itself runtime-gba/src/gba_crt.c with a row
 * stride and a column count).
 */
#include "nes_crt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { LIN_MAX = 4095, TAPS = 3 };

/* The look. */
#define GAMMA 2.2           /* screen values to light and back */
#define BLUR_SIGMA 0.50     /* along a line, in source pixels */
#define BEAM_THIN 0.14      /* a dark beam's half-width (sigma), in lines */
#define BEAM_WIDE 0.30      /* a white beam's */
#define BRIGHTNESS 1.70     /* makes up for the gaps and the mask */
#define MASK_DARK 0.72      /* a stripe's other two colours, screen value */
#define GLOW_SIGMA 1.6      /* the halo's blur, in source pixels */

struct nes_crt {
    uint16_t to_linear[256];
    uint8_t to_screen[LIN_MAX + 1];
    int columns;                                  /* the blur weights' column count */
    int16_t blur_weight[NES_CRT_MAX_COLUMNS][TAPS]; /* taps x-1, x, x+1; sum 1024 */
    int line_px;                                /* the beam tables' line height */
    uint16_t *beam;       /* [line_px][2][LIN_MAX + 1]: light from the upper, lower line */
    int8_t *row_offset;   /* [line_px]: the upper line's offset from the row's own */
    uint16_t *blurred;    /* the blurred lines, 3 channels */
    size_t blurred_size;
    uint16_t *glow_work;
    size_t glow_size;
};

static double beam_sigma(double v)
{
    return BEAM_THIN + (BEAM_WIDE - BEAM_THIN) * v;
}

nes_crt *nes_crt_create(void)
{
    nes_crt *c = (nes_crt *)calloc(1, sizeof *c);
    if (!c) return NULL;
    for (int i = 0; i < 256; i++) c->to_linear[i] = (uint16_t)(pow(i / 255.0, GAMMA) * LIN_MAX + 0.5);
    for (int i = 0; i <= LIN_MAX; i++) c->to_screen[i] = (uint8_t)(pow(i / (double)LIN_MAX, 1.0 / GAMMA) * 255.0 + 0.5);
    return c;
}

/* Blur weights for each of columns columns across a source pixel. */
static void make_blur_weights(nes_crt *c, int columns)
{
    if (c->columns == columns) return;
    for (int q = 0; q < columns; q++) {
        const double at = (q + 0.5) / columns - 0.5; /* the column's place from its pixel's centre */
        double w[TAPS], sum = 0.0;
        for (int t = 0; t < TAPS; t++) {
            const double d = (t - 1) - at;
            w[t] = exp(-0.5 * d * d / (BLUR_SIGMA * BLUR_SIGMA));
            sum += w[t];
        }
        int total = 0;
        for (int t = 0; t < TAPS; t++) total += c->blur_weight[q][t] = (int16_t)(w[t] / sum * 1024.0 + 0.5);
        c->blur_weight[q][1] += (int16_t)(1024 - total);
    }
    c->columns = columns;
}

void nes_crt_free(nes_crt *c)
{
    if (!c) return;
    free(c->beam);
    free(c->row_offset);
    free(c->blurred);
    free(c->glow_work);
    free(c);
}

static int make_beam_tables(nes_crt *c, int line_px)
{
    if (c->line_px == line_px) return 1;
    uint16_t *beam = (uint16_t *)malloc(sizeof *beam * (size_t)line_px * 2 * (LIN_MAX + 1));
    int8_t *offset = (int8_t *)malloc((size_t)line_px);
    if (!beam || !offset) {
        free(beam);
        free(offset);
        return 0;
    }
    for (int p = 0; p < line_px; p++) {
        /* The row's centre in source lines, from its own line's centre. */
        const double at = (p + 0.5) / line_px - 0.5;
        offset[p] = (int8_t)(at < 0 ? -1 : 0);
        const double from_upper = at < 0 ? at + 1.0 : at;
        for (int k = 0; k < 2; k++) {
            const double d = k ? 1.0 - from_upper : from_upper;
            uint16_t *out = beam + ((size_t)p * 2 + k) * (LIN_MAX + 1);
            for (int i = 0; i <= LIN_MAX; i++) {
                const double v = i / (double)LIN_MAX, s = beam_sigma(v);
                const double light = v * BRIGHTNESS * exp(-0.5 * d * d / (s * s));
                out[i] = (uint16_t)(light >= 1.0 ? LIN_MAX : light * LIN_MAX + 0.5);
            }
        }
    }
    free(c->beam);
    free(c->row_offset);
    c->beam = beam;
    c->row_offset = offset;
    c->line_px = line_px;
    return 1;
}

static int reserve(uint16_t **buf, size_t *size, size_t want)
{
    if (*size >= want) return 1;
    uint16_t *b = (uint16_t *)realloc(*buf, want * sizeof *b);
    if (!b) return 0;
    *buf = b;
    *size = want;
    return 1;
}

int nes_crt_render(nes_crt *c, const uint32_t *src, int w, int h, int stride, int columns, int line_px,
                    uint32_t *dst)
{
    if (!c || w < 1 || h < 1 || stride < w || columns < 1 || columns > NES_CRT_MAX_COLUMNS ||
        line_px < NES_CRT_MIN_LINE || line_px > NES_CRT_MAX_LINE)
        return 0;
    const int cols = w * columns;
    if (!make_beam_tables(c, line_px) || !reserve(&c->blurred, &c->blurred_size, (size_t)cols * 3 * h)) return 0;
    make_blur_weights(c, columns);

    for (int y = 0; y < h; y++) {
        const uint32_t *line = src + (size_t)y * stride;
        uint16_t *out = c->blurred + (size_t)y * cols * 3;
        for (int x = 0; x < w; x++) {
            const uint32_t px[TAPS] = { line[x > 0 ? x - 1 : 0], line[x], line[x < w - 1 ? x + 1 : w - 1] };
            for (int q = 0; q < columns; q++) {
                const int16_t *wt = c->blur_weight[q];
                for (int ch = 0; ch < 3; ch++) {
                    const int shift = 16 - 8 * ch;
                    int sum = 0;
                    for (int t = 0; t < TAPS; t++) sum += wt[t] * c->to_linear[(px[t] >> shift) & 0xFF];
                    *out++ = (uint16_t)(sum >> 10);
                }
            }
        }
    }

    const size_t table = LIN_MAX + 1;
    for (int oy = 0; oy < h * line_px; oy++) {
        const int p = oy % line_px, upper = oy / line_px + c->row_offset[p];
        const uint16_t *from_upper = c->beam + (size_t)p * 2 * table, *from_lower = from_upper + table;
        const uint16_t *a = upper >= 0 ? c->blurred + (size_t)upper * cols * 3 : NULL;
        const uint16_t *b = upper + 1 < h ? c->blurred + (size_t)(upper + 1) * cols * 3 : NULL;
        uint32_t *out = dst + (size_t)oy * cols;
        for (int x = 0; x < cols; x++) {
            uint32_t px = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                unsigned light = (a ? from_upper[a[x * 3 + ch]] : 0u) + (b ? from_lower[b[x * 3 + ch]] : 0u);
                if (light > LIN_MAX) light = LIN_MAX;
                px |= (uint32_t)c->to_screen[light] << (16 - 8 * ch);
            }
            out[x] = px;
        }
    }
    return 1;
}

int nes_crt_glow(nes_crt *c, const uint32_t *src, int w, int h, int stride, uint32_t *dst)
{
    enum { R = 4 };
    if (!c || w < 1 || h < 1 || stride < w || !reserve(&c->glow_work, &c->glow_size, (size_t)w * h * 3)) return 0;
    int k[2 * R + 1], ksum = 0;
    for (int i = -R; i <= R; i++) ksum += k[i + R] = (int)(exp(-0.5 * i * i / (GLOW_SIGMA * GLOW_SIGMA)) * 256.0 + 0.5);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int ch = 0; ch < 3; ch++) {
                int sum = 0;
                for (int i = -R; i <= R; i++) {
                    const int sx = x + i < 0 ? 0 : x + i >= w ? w - 1 : x + i;
                    sum += k[i + R] * c->to_linear[(src[(size_t)y * stride + sx] >> (16 - 8 * ch)) & 0xFF];
                }
                c->glow_work[((size_t)y * w + x) * 3 + ch] = (uint16_t)(sum / ksum);
            }
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                int sum = 0;
                for (int i = -R; i <= R; i++) {
                    const int sy = y + i < 0 ? 0 : y + i >= h ? h - 1 : y + i;
                    sum += k[i + R] * c->glow_work[((size_t)sy * w + x) * 3 + ch];
                }
                px |= (uint32_t)c->to_screen[sum / ksum] << (16 - 8 * ch);
            }
            dst[(size_t)y * w + x] = px;
        }
    }
    return 1;
}

uint32_t nes_crt_mask(int x)
{
    const uint32_t dark = (uint32_t)(MASK_DARK * 255.0 + 0.5);
    const int stripe = ((x % 3) + 3) % 3; /* red, green, blue */
    uint32_t px = 0xFF000000u;
    for (int ch = 0; ch < 3; ch++) px |= (ch == stripe ? 255u : dark) << (16 - 8 * ch);
    return px;
}
