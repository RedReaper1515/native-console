/*
 * Copyright (c) 2020 Themaister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The VI's scan-out: the framebuffer in RDRAM, filtered, then scaled by
 * X_SCALE and Y_SCALE into the visible window H_START..H_END,
 * V_START..V_END of a 640 x 480 (NTSC) or 640 x 576 (PAL) picture.
 *
 * The filters are parallel-rdp's VI stage with every ScanoutOptions filter
 * on - at native resolution for the console's picture, and on the RDP's
 * larger picture when there is one (n64_set_internal_scale) - ported from
 * parallel-rdp (commit
 * 388d70f5835b352d841d9d9e5a08c5de01470f41):
 *  - video_interface.cpp: scanout (the stage order, and which VI_CONTROL
 *    bits reach which stage), decode_vi_registers (the sample positions)
 *    and need_fetch_bug_emulation;
 *  - shaders/extract_vram.comp (the fetch), shaders/vi_fetch.frag
 *    (anti-aliasing and the dither filter), shaders/vi_divot.frag,
 *    shaders/vi_scale.frag (the bilinear scaler, gamma and gamma dither),
 *    shaders/vi_status.h and shaders/noise.h; luts.hpp's gamma_table is
 *    n64_vi_luts.c.
 *
 * parallel-rdp runs each stage over the whole picture on the GPU. Here a
 * stage's rows are made when the scaler first asks for them and a few are
 * kept, since it walks down the framebuffer; every pixel is the same
 * function of the framebuffer, so the results are bit for bit the
 * shaders'. Where a shader's unsigned arithmetic wraps, a comment says why
 * the form used here gives the same bits.
 *
 * Not taken from parallel-rdp: its per-scanline H_START and X_SCALE
 * (registers are read once, when the field ends), its upscaling, and its
 * choices for a host display (cropping, de-interlacing by scaling a field;
 * a field's lines are woven with the last field's here). Memory beyond
 * RDRAM reads as zero, as it does for the RDP (n64_rdp.h), where
 * parallel-rdp wraps.
 *
 * The windowing follows ares (n64/vi/vi.cpp, VI::refresh; ISC), as it did
 * before the filters: which rows and columns of the picture are drawn, the
 * 8-pixel guard band, and V_START (not the top of the picture) as the
 * line the framebuffer's first row is on. parallel-rdp differs in three
 * places, none of which this file adopts: it drops the right guard band
 * only when H_END is beyond the picture (ares: also when it is exactly the
 * edge), drops the left one when H_START is before the picture (ares:
 * never), and starts an odd V_START one line higher. n64_vi_render's
 * unfiltered path (N64Machine.vi_raw) is ares's nearest-neighbour scan-out
 * unchanged. Ported from ares (https://github.com/ares-emulator/ares).
 * Copyright (c) 2004-2025 ares team, Near et al. Permission to use, copy,
 * modify, and/or distribute this software for any purpose with or without
 * fee is hereby granted, provided that the above copyright notice and this
 * permission notice appear in all copies.
 */
#include "n64_io.h"
#include "n64_rdp_workers.h"
#include "n64_vi_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define VI_RESTRICT __restrict
#else
#define VI_RESTRICT restrict
#endif

/* VI_CONTROL (rdp_common.hpp, VIControlFlagBits). Bits 8-9 are the
 * anti-aliasing mode: 0 and 1 anti-alias and resample, 2 resamples only,
 * 3 does neither. */
#define VI_GAMMA_DITHER 0x00004u
#define VI_GAMMA 0x00008u
#define VI_DIVOT 0x00010u
#define VI_SERRATE 0x00040u
#define VI_DITHER_FILTER 0x10000u

/* The part of the picture the VI draws: columns dx0..dx1 and lines
 * dy0..dy1, counted like H_START and V_START (the picture's top left is
 * hscan_start, vscan_start). */
typedef struct ViWindow {
    int hscan_start, vscan_start;
    int hstart, vstart;
    int dx0, dx1, dy0, dy1;
} ViWindow;

/* ---- the unfiltered scan-out ---- */

static uint32_t rgb5551(uint16_t p)
{
    uint32_t r = (p >> 11) & 31u, g = (p >> 6) & 31u, b = (p >> 1) & 31u;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r << 16 | g << 8 | b;
}

static void vi_scan_raw(N64Machine *m, const ViWindow *w)
{
    const uint32_t *io = m->vi_regs;
    const uint32_t depth = io[0] & 3u;
    const bool serrate = (io[0] & VI_SERRATE) != 0;
    const uint32_t origin = io[1] & 0xFFFFFFu, pitch = io[2] & 0xFFFu;
    const uint32_t xscale = io[12] & 0xFFFu, xsub = (io[12] >> 16) & 0xFFFu;
    const uint32_t yscale = io[13] & 0xFFFu, ysub = (io[13] >> 16) & 0xFFFu;

    uint32_t y0 = ysub + yscale * (uint32_t)(w->dy0 - w->vstart);
    for (int dy = w->dy0; dy < w->dy1; dy++) {
        if (!serrate || (dy & 1) == (int)!m->vi_field) {
            uint32_t *line = m->framebuffer + (size_t)(dy - w->vscan_start) * N64_FB_WIDTH;
            uint32_t x0 = xsub + xscale * (uint32_t)(w->dx0 - w->hstart);
            if (depth == 2) {
                uint32_t row = origin + (y0 >> 11) * pitch * 2u;
                for (int dx = w->dx0; dx < w->dx1; dx++) {
                    uint32_t a = (row + (x0 >> 10) * 2u) & 0xFFFFFEu;
                    uint16_t p = a + 2u <= m->rdram_size ? n64_be16(m->rdram + a) : 0;
                    line[dx - w->hscan_start] = rgb5551(p);
                    x0 += xscale;
                }
            } else {
                uint32_t row = origin + (y0 >> 11) * pitch * 4u;
                for (int dx = w->dx0; dx < w->dx1; dx++) {
                    uint32_t a = (row + (x0 >> 10) * 4u) & 0xFFFFFCu;
                    uint32_t p = a + 4u <= m->rdram_size ? n64_be32(m->rdram + a) : 0;
                    line[dx - w->hscan_start] = p >> 8;
                    x0 += xscale;
                }
            }
        }
        y0 += yscale;
    }
}

/* ---- the larger picture ----
 *
 * The RDP's larger picture (n64_rdp.h, n64_rdp_set_scale), scanned out the
 * way vi_scan_raw() scans the console's: the nearest sample, no filters.
 * A display pixel is `g` by `g` pixels here, and each steps a g-th of
 * X_SCALE and Y_SCALE through the framebuffer; within a framebuffer pixel
 * the fraction left chooses among its scale by scale samples. A pixel the
 * RDP has no larger picture of - the game wrote it itself - is its own
 * colour in each.
 *
 * Gamma is applied where VI_CONTROL asks for it, without the noise gamma
 * dither puts under it. The anti-aliasing, dither and divot filters are
 * not: they are defined on the console's pixels and its coverage bits.
 */
static void vi_scan_larger(N64Machine *m, const ViWindow *w, int g)
{
    const uint32_t *io = m->vi_regs;
    const uint32_t depth = io[0] & 3u;
    const bool serrate = (io[0] & VI_SERRATE) != 0, gamma = (io[0] & VI_GAMMA) != 0;
    const uint32_t origin = io[1] & 0xFFFFFFu, pitch = io[2] & 0xFFFu;
    const uint32_t xscale = io[12] & 0xFFFu, xsub = (io[12] >> 16) & 0xFFFu;
    const uint32_t yscale = io[13] & 0xFFFu, ysub = (io[13] >> 16) & 0xFFFu;
    const uint32_t scale = (uint32_t)n64_rdp_scale(&m->rdp);
    const uint32_t bytes = depth == 2 ? 2u : 4u, mask = depth == 2 ? 0xFFFFFEu : 0xFFFFFCu;
    const size_t wide = (size_t)N64_FB_WIDTH * (size_t)g;
    const uint8_t *table = n64_vi_gamma_table;

    uint32_t y0 = ysub + yscale * (uint32_t)(w->dy0 - w->vstart);
    for (int dy = w->dy0; dy < w->dy1; dy++, y0 += yscale) {
        if (serrate && (dy & 1) != (int)!m->vi_field) continue;
        for (int j = 0; j < g; j++) {
            const uint32_t y = y0 + yscale * (uint32_t)j / (uint32_t)g;
            const uint32_t row = origin + (y >> 11) * pitch * bytes;
            const uint32_t sy = ((y & 0x7FFu) * scale) >> 11;
            uint32_t *line = m->large_frame + ((size_t)(dy - w->vscan_start) * (size_t)g + (size_t)j) * wide;
            uint32_t x0 = xsub + xscale * (uint32_t)(w->dx0 - w->hstart);
            for (int dx = w->dx0; dx < w->dx1; dx++, x0 += xscale) {
                for (int i = 0; i < g; i++) {
                    const uint32_t x = x0 + xscale * (uint32_t)i / (uint32_t)g;
                    const uint32_t a = (row + (x >> 10) * bytes) & mask;
                    const uint32_t sx = ((x & 0x3FFu) * scale) >> 10;
                    uint32_t c = 0;
                    if (a + bytes <= m->rdram_size) {
                        size_t stride = 0;
                        const uint8_t *larger = n64_rdp_larger_pixel(&m->rdp, a, bytes, &stride);
                        const uint8_t *at = larger ? larger + (size_t)(sy * scale + sx) * stride : m->rdram + a;
                        c = depth == 2 ? rgb5551(n64_be16(at)) : n64_be32(at) >> 8;
                    }
                    if (gamma)
                        c = (uint32_t)table[c >> 16] << 16 | (uint32_t)table[(c >> 8) & 0xFFu] << 8 | table[c & 0xFFu];
                    line[(size_t)(dx - w->hscan_start) * (size_t)g + (size_t)i] = c;
                }
            }
        }
    }
}

/* ---- the filters ----
 *
 * A pixel between stages is 0x0CRRGGBB: the colour, and its 3-bit coverage
 * C (7 when the VI is not anti-aliasing) where the shaders keep alpha.
 *
 * Stage rows are indexed by framebuffer column - x0, and hold a few
 * columns either side of the w the scaler reads: the divot filter looks
 * one column left and right, anti-aliasing two beyond that. */
#define VI_PAD 3

typedef struct ViRows {
    const uint8_t *rdram, *hidden;
    uint32_t rdram_size;
    uint32_t origin, pitch; /* in pixels */
    bool rgba32, fetch_aa, dither_filter, divot;
    int x0, w;
    /* Fetched rows y - 1 .. y + 2 at most are in use at once: slot y & 3.
     * lanes is the same row as 5-bit colours for the dither filter; spots
     * lists the row's pixels with coverage below 7 (as indices into fetch,
     * ascending), which are all that anti-aliasing and the divot filter
     * have to visit. */
    uint32_t *fetch[4], *lanes[4];
    uint16_t *spots[4];
    int spot_count[4];
    int fetch_y[4];
    /* Filtered rows y and y + 1, as the scaler reads them: slot y & 1 of
     * [0], or of [1] for the rows a fetch-bugged line reads. */
    uint32_t *out[2][2];
    int out_y[2][2];
    /* The RDP's larger picture (n64_rdp.h): with larger.log2 not 0 the rows
     * are rows of it, x0, w and y counted in its pixels, and origin and
     * pitch still the console's. */
    N64RdpLargerView larger;
    uint32_t larger_lo, larger_hi; /* the bytes of RDRAM it is read for */
} ViRows;

static const uint32_t *vi_fetched(const ViRows *r, int y) { return r->fetch[y & 3] + VI_PAD; }
static const uint32_t *vi_lanes(const ViRows *r, int y) { return r->lanes[y & 3] + VI_PAD; }

/* extract_vram.comp: row y of the framebuffer, columns x0 - VI_PAD up to
 * x0 + w + VI_PAD. Rows above the origin and columns outside the width are
 * simply the memory that is there. */
/* The same row of the RDP's larger picture: pixel (x, y) of it is sample
 * (x mod scale, y mod scale) of the console's pixel (x / scale, y / scale),
 * where the RDP drew that pixel last; where the game wrote it, the
 * console's own pixel and hidden bits. */
static void vi_fetch_row_larger(ViRows *r, int y, uint32_t *VI_RESTRICT px, uint16_t *VI_RESTRICT spots, int n,
                                int *spot_count)
{
    const N64RdpLargerView *l = &r->larger;
    const int log2 = l->log2, mask = (1 << log2) - 1;
    const uint32_t row = r->origin + (uint32_t)(y >> log2) * r->pitch;
    const uint32_t slice_y = (uint32_t)(y & mask) << log2;
    const uint32_t bytes = r->rgba32 ? 4u : 2u, index_mask = r->rgba32 ? 0x3FFFFFu : 0x7FFFFFu;
    const uint32_t limit = r->rdram_size / bytes;
    int count = 0;
    for (int i = 0; i < n; i++) {
        const int x = r->x0 - VI_PAD + i;
        const uint32_t a = (row + (uint32_t)(x >> log2)) & index_mask;
        uint32_t word = 0, cov = r->fetch_aa ? 0u : 7u;
        if (a < limit) {
            const size_t at = (size_t)a * bytes;
            const size_t slice = slice_y + (uint32_t)(x & mask);
            const bool drawn = at >= r->larger_lo && at + bytes <= r->larger_hi &&
                               !memcmp(l->reference + at, r->rdram + at, bytes);
            const uint8_t *p = drawn ? l->rdram + slice * l->rdram_stride + at : r->rdram + at;
            if (r->rgba32) {
                word = n64_be32(p);
                if (r->fetch_aa) cov = (word >> 5) & 7u;
            } else {
                word = n64_be16(p);
                if (r->fetch_aa) {
                    const uint8_t hidden = drawn ? l->hidden[slice * l->hidden_stride + (at >> 1)] : r->hidden[at >> 1];
                    cov = ((word & 1u) << 2) | (hidden & 3u);
                }
            }
        }
        spots[count] = (uint16_t)i;
        count += cov != 7u;
        px[i] = r->rgba32 ? word >> 8 | cov << 24
                          : ((word & 0xF800u) << 8) | ((word & 0x07C0u) << 5) | ((word & 0x003Eu) << 2) | cov << 24;
    }
    *spot_count = count;
}

static void vi_fetch_row(ViRows *r, int y)
{
    const int slot = y & 3;
    if (r->fetch_y[slot] == y) return;
    r->fetch_y[slot] = y;
    uint32_t *VI_RESTRICT px = r->fetch[slot];
    uint16_t *VI_RESTRICT spots = r->spots[slot];
    const int n = r->w + 2 * VI_PAD;
    if (r->larger.log2) {
        vi_fetch_row_larger(r, y, px, spots, n, &r->spot_count[slot]);
        if (r->dither_filter) {
            uint32_t *VI_RESTRICT q = r->lanes[slot];
            for (int i = 0; i < n; i++) q[i] = (px[i] >> 3) & 0x1F1F1Fu;
        }
        return;
    }
    /* linear_coord, wrapping in the 24 bits of a VI address. */
    uint32_t at = r->origin + (uint32_t)y * r->pitch + (uint32_t)(r->x0 - VI_PAD);
    int count = 0;
    if (!r->rgba32) {
        const uint32_t limit = r->rdram_size >> 1;
        at &= 0x7FFFFFu;
        if (at + (uint32_t)n <= limit) {
            const uint8_t *VI_RESTRICT p = r->rdram + 2u * (size_t)at;
            if (r->fetch_aa) {
                const uint8_t *VI_RESTRICT h = r->hidden + at;
                for (int i = 0; i < n; i++) {
                    const uint32_t word = ((uint32_t)p[2 * i] << 8) | p[2 * i + 1];
                    const uint32_t cov = ((word & 1u) << 2) | (h[i] & 3u);
                    spots[count] = (uint16_t)i;
                    count += cov != 7u;
                    px[i] = ((word & 0xF800u) << 8) | ((word & 0x07C0u) << 5) | ((word & 0x003Eu) << 2) | cov << 24;
                }
            } else {
                for (int i = 0; i < n; i++) {
                    const uint32_t word = ((uint32_t)p[2 * i] << 8) | p[2 * i + 1];
                    px[i] = ((word & 0xF800u) << 8) | ((word & 0x07C0u) << 5) | ((word & 0x003Eu) << 2) | 7u << 24;
                }
            }
        } else {
            for (int i = 0; i < n; i++) {
                const uint32_t a = (at + (uint32_t)i) & 0x7FFFFFu;
                const uint32_t word = a < limit ? n64_be16(r->rdram + 2u * (size_t)a) : 0u;
                const uint32_t cov = !r->fetch_aa ? 7u : a < limit ? ((word & 1u) << 2) | (r->hidden[a] & 3u) : 0u;
                spots[count] = (uint16_t)i;
                count += cov != 7u;
                px[i] = ((word & 0xF800u) << 8) | ((word & 0x07C0u) << 5) | ((word & 0x003Eu) << 2) | cov << 24;
            }
        }
    } else {
        const uint32_t limit = r->rdram_size >> 2;
        at &= 0x3FFFFFu;
        for (int i = 0; i < n; i++) {
            const uint32_t a = (at + (uint32_t)i) & 0x3FFFFFu;
            const uint32_t word = a < limit ? n64_be32(r->rdram + 4u * (size_t)a) : 0u;
            const uint32_t cov = r->fetch_aa ? (word >> 5) & 7u : 7u;
            spots[count] = (uint16_t)i;
            count += cov != 7u;
            px[i] = word >> 8 | cov << 24;
        }
    }
    r->spot_count[slot] = count;
    if (r->dither_filter) {
        uint32_t *VI_RESTRICT q = r->lanes[slot];
        for (int i = 0; i < n; i++) q[i] = (px[i] >> 3) & 0x1F1F1Fu;
    }
}

/* vi_fetch.frag, coverage below 7: the pixel is blended towards its
 * surroundings, the less it covers the further. Of the six neighbours with
 * full coverage, each channel's second lowest and second highest values
 * (the pixel's own counted in) stand for the background. For a fetch-bugged
 * line the row below is not read and the left and right neighbours count
 * twice instead.
 *
 * The shader also resets second_lo to lo where the pixel is itself the
 * lowest (and likewise the highest) when it is built for the fetch bug.
 * That changes nothing - no neighbour was below the pixel, so second_lo
 * never left the pixel's value - and is left out.
 *
 * `offset` can be negative, which the shader's unsigned arithmetic wraps;
 * so does this, and the final & 0xff sees the same low bits either way. */
static uint32_t vi_aa_pixel(const uint32_t *up, const uint32_t *mid, const uint32_t *down, int i, bool bug)
{
    const uint32_t c = mid[i];
    uint32_t taps[6] = { up[i - 1],  up[i + 1],  mid[i - 2],
                         mid[i + 2], bug ? mid[i - 2] : down[i - 1], bug ? mid[i + 2] : down[i + 1] };
    /* A neighbour that is not fully covered is not counted. The pixel's
     * own colour stands in for it: counting that changes nothing, lo and
     * second_lo being at or below it already and hi and second_hi at or
     * above. With no neighbour counted the offset is 0. */
    uint32_t counted = 0;
    for (int t = 0; t < 6; t++) {
        const uint32_t full = taps[t] >> 24 == 7u;
        counted |= full;
        taps[t] = full ? taps[t] : c;
    }
    if (!counted) return c;
    const uint32_t coeff = 7u - (c >> 24);
    uint32_t out = c & 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        const uint32_t v = (c >> shift) & 0xFFu;
        uint32_t lo = v, hi = v, second_lo = v, second_hi = v;
        for (int t = 0; t < 6; t++) {
            const uint32_t n = (taps[t] >> shift) & 0xFFu;
            const uint32_t above_lo = n > lo ? n : lo, below_hi = n < hi ? n : hi;
            second_lo = above_lo < second_lo ? above_lo : second_lo;
            second_hi = below_hi > second_hi ? below_hi : second_hi;
            lo = n < lo ? n : lo;
            hi = n > hi ? n : hi;
        }
        const uint32_t offset = second_lo + second_hi - (v << 1);
        out |= ((v + ((offset * coeff + 4u) >> 3)) & 0xFFu) << shift;
    }
    return out;
}

/* vi_fetch.frag, full coverage with the dither filter on: each channel
 * moves by the number of its eight neighbours whose 5-bit value is above
 * it less the number below, which takes the RDP's dither pattern back out.
 *
 * The three channels are counted at once, as 5-bit values in the low bits
 * of each byte. (n | 0x20) - c keeps bit 5 exactly where n >= c and never
 * borrows from the byte above, so VI_STEP is clamp(n - c, -1, 1) + 1 per
 * channel, in units of 0x20. The sum is then eight (or, fetch-bugged,
 * seven) too high in every channel; taking that off again leaves each byte
 * within 0..255 (a channel at 0 has no neighbour below it, one at 31 none
 * above), so no borrow or carry is left over between bytes. */
#define VI_GE(a, b) ((((a) | 0x202020u) - (b)) & 0x202020u)
#define VI_STEP(n, c) (VI_GE(n, c) + (~(((c) | 0x202020u) - (n)) & 0x202020u))

static void vi_dedither_row(uint32_t *VI_RESTRICT out, const uint32_t *VI_RESTRICT mid, const uint32_t *VI_RESTRICT qu,
                            const uint32_t *VI_RESTRICT qm, const uint32_t *VI_RESTRICT qd, int from, int to)
{
    if (qd) {
        for (int i = from; i < to; i++) {
            const uint32_t c = qm[i];
            const uint32_t acc = VI_STEP(qu[i - 1], c) + VI_STEP(qu[i], c) + VI_STEP(qu[i + 1], c) +
                                 VI_STEP(qm[i - 1], c) + VI_STEP(qm[i + 1], c) + VI_STEP(qd[i - 1], c) +
                                 VI_STEP(qd[i], c) + VI_STEP(qd[i + 1], c);
            out[i] = (mid[i] & 0xFFF8F8F8u) + (acc >> 5) - 0x080808u;
        }
    } else {
        /* The fetch bug: the row below is not read; left and right count twice. */
        for (int i = from; i < to; i++) {
            const uint32_t c = qm[i];
            const uint32_t acc = VI_STEP(qu[i - 1], c) + VI_STEP(qu[i], c) + VI_STEP(qu[i + 1], c) +
                                 2u * (VI_STEP(qm[i - 1], c) + VI_STEP(qm[i + 1], c));
            out[i] = (mid[i] & 0xFFF8F8F8u) + (acc >> 5) - 0x070707u;
        }
    }
}

/* Row y of vi_fetch.frag's output (FragColorFetchBug when `bug`), columns
 * -1 .. w, into out. */
static void vi_aa_row(ViRows *r, int y, bool bug, uint32_t *out)
{
    vi_fetch_row(r, y - 1);
    vi_fetch_row(r, y);
    if (!bug) vi_fetch_row(r, y + 1);
    const uint32_t *up = vi_fetched(r, y - 1), *mid = vi_fetched(r, y), *down = bug ? NULL : vi_fetched(r, y + 1);
    if (r->dither_filter)
        vi_dedither_row(out, mid, vi_lanes(r, y - 1), vi_lanes(r, y), bug ? NULL : vi_lanes(r, y + 1), -1, r->w + 1);
    else
        memcpy(out - 1, mid - 1, sizeof(uint32_t) * (size_t)(r->w + 2));
    const uint16_t *spots = r->spots[y & 3];
    for (int k = 0, count = r->spot_count[y & 3]; k < count; k++) {
        const int i = (int)spots[k] - VI_PAD;
        if (i >= -1 && i <= r->w) out[i] = vi_aa_pixel(up, mid, down, i, bug);
    }
}

static uint32_t vi_median(uint32_t a, uint32_t b, uint32_t c)
{
    const uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
    return c < lo ? lo : c > hi ? hi : c;
}

/* Row y as the scaler reads it, columns 0 .. w - 1: vi_fetch.frag's output,
 * through vi_divot.frag when the divot filter is on. Where a pixel or
 * either horizontal neighbour is partly covered, the divot filter takes
 * each channel's median of the three, which removes the one-pixel notches
 * anti-aliasing leaves between two surfaces.
 *
 * Only the columns beside a partly covered pixel change, so the filter
 * walks the row's list of those and works in place: `held` is what the
 * column to the left held before it was filtered. */
static const uint32_t *vi_out_row(ViRows *r, int y, bool bug)
{
    const int slot = y & 1;
    uint32_t *row = r->out[bug][slot] + 1;
    if (r->out_y[bug][slot] == y) return row;
    r->out_y[bug][slot] = y;
    vi_aa_row(r, y, bug, row);
    if (!r->divot) return row;
    const uint16_t *spots = r->spots[y & 3];
    const int last = r->w - 1;
    int next = 0, held_at = INT_MIN;
    uint32_t held = 0;
    for (int k = 0, count = r->spot_count[y & 3]; k < count; k++) {
        const int i = (int)spots[k] - VI_PAD;
        const int end = i + 1 < last ? i + 1 : last;
        for (int j = i - 1 > next ? i - 1 : next; j <= end; j++) {
            const uint32_t left = held_at == j - 1 ? held : row[j - 1], mid = row[j], right = row[j + 1];
            held = mid;
            held_at = j;
            row[j] = (mid & 0xFF000000u) | vi_median(left & 0xFF0000u, mid & 0xFF0000u, right & 0xFF0000u) |
                     vi_median(left & 0xFF00u, mid & 0xFF00u, right & 0xFF00u) |
                     vi_median(left & 0xFFu, mid & 0xFFu, right & 0xFFu);
        }
        if (end >= next) next = end + 1;
    }
    return row;
}

/* vi_scale.frag's vi_lerp, on the three channels of a pixel: a + (((b - a)
 * * l + 16) >> 5) per channel. Written as (a * (32 - l) + b * l + 16) >> 5
 * nothing goes negative: the two are the same number, floor((32a + (b - a)
 * l + 16) / 32), and it is within 0..255, so the shader's & 0xff (and the
 * wrap of its unsigned b - a) changes nothing. Red and blue share a
 * multiplication; each product stays below 2^13 in its own 16 bits. The
 * coverage byte is dropped. */
N64_FORCEINLINE uint32_t vi_lerp(uint32_t a, uint32_t b, uint32_t l)
{
    const uint32_t k = 32u - l;
    const uint32_t rb = (((a & 0xFF00FFu) * k + (b & 0xFF00FFu) * l + 0x100010u) >> 5) & 0xFF00FFu;
    const uint32_t g = (((a & 0x00FF00u) * k + (b & 0x00FF00u) * l + 0x001000u) >> 5) & 0x00FF00u;
    return rb | g;
}

/* noise.h's reseed_noise(x, y, frame): three rounds of
 * seed = ((seed >> 8) ^ seed.yzx) * prime over (x, y, frame), of which
 * only the first component's top 16 bits are used. Followed back, that
 * component needs round one's z (which depends on the column and the frame
 * alone), round one's y (the line and the frame) and round two's x (the
 * line and x >> 8), so a pixel costs two multiplications, and a line is
 * done in the up to three stretches of columns that share x >> 8. */
#define VI_NOISE_PRIME 1103515245u

typedef struct ViNoise {
    uint32_t frame;
    int x0;       /* the first column's x */
    uint32_t *z1; /* per column: ((frame >> 8) ^ x) * prime */
} ViNoise;

/* vi_scale.frag's last step, on one line of 0x00RRGGBB pixels: the gamma
 * table, with six bits of noise a channel under each lookup when gamma
 * dither is on; or, with gamma off and gamma dither on, one bit of noise
 * added to each channel. The hardware's noise is random; parallel-rdp's is
 * this hash of the pixel's position (counted from where the line's
 * sampling starts) and the frame. */
static void vi_gamma_line(uint32_t *VI_RESTRICT line, int cols, uint32_t ctrl, const ViNoise *nz, uint32_t ny)
{
    const uint8_t *table = n64_vi_gamma_table;
    if (!(ctrl & VI_GAMMA_DITHER)) {
        if (!(ctrl & VI_GAMMA)) return;
        for (int j = 0; j < cols; j++) {
            const uint32_t c = line[j];
            line[j] = (uint32_t)table[c >> 16] << 16 | (uint32_t)table[(c >> 8) & 0xFFu] << 8 | table[c & 0xFFu];
        }
        return;
    }
    const uint32_t *VI_RESTRICT z1 = nz->z1;
    const uint32_t y1 = ((ny >> 8) ^ nz->frame) * VI_NOISE_PRIME, y1_high = y1 >> 8;
    for (int from = 0; from < cols;) {
        const uint32_t x_high = (uint32_t)(nz->x0 + from) >> 8;
        const int stop = (int)((x_high + 1u) << 8) - nz->x0, to = stop < cols ? stop : cols;
        const uint32_t x2_high = ((((x_high ^ ny) * VI_NOISE_PRIME) >> 8) ^ y1) * VI_NOISE_PRIME >> 8;
        if (ctrl & VI_GAMMA) {
            for (int j = from; j < to; j++) {
                const uint32_t seed = ((x2_high ^ ((y1_high ^ z1[j]) * VI_NOISE_PRIME)) * VI_NOISE_PRIME) >> 16;
                const uint32_t c = line[j];
                /* noise_get_full_gamma_dither */
                const uint32_t r = ((c >> 16) << 6) + (seed & 0x3Fu) + 256u;
                const uint32_t g = (((c >> 8) & 0xFFu) << 6) + ((seed >> 6) & 0x3Fu) + 256u;
                const uint32_t b = ((c & 0xFFu) << 6) + (((seed >> 9) & 0x38u) | (seed & 7u)) + 256u;
                line[j] = (uint32_t)table[r] << 16 | (uint32_t)table[g] << 8 | table[b];
            }
        } else {
            for (int j = from; j < to; j++) {
                const uint32_t seed = ((x2_high ^ ((y1_high ^ z1[j]) * VI_NOISE_PRIME)) * VI_NOISE_PRIME) >> 16;
                const uint32_t c = line[j];
                /* noise_get_partial_gamma_dither, and min(c + noise, 0xff): a
                 * channel already at 0xff (bit 7 of `full`) takes nothing. */
                const uint32_t add = (seed & 1u) << 16 | (seed & 2u) << 7 | ((seed >> 2) & 1u);
                const uint32_t full = ((c & 0x7F7F7Fu) + 0x010101u) & c & 0x808080u;
                line[j] = c + (add & ~(full >> 7));
            }
        }
        from = to;
    }
}

/* One filtered scan-out: what every line of it is drawn from. */
typedef struct ViScan {
    const N64Machine *m;
    const ViWindow *w;
    ViRows rows; /* without its rows, which each stretch of lines has its own of */
    size_t fetch_n, out_n, spots_n;
    uint32_t ctrl;
    bool serrate, scale_aa, larger, fetch_bug;
    int out_scale, y_add, y_start, k0, cols;
    const uint16_t *xi;
    const uint8_t *xf;
    ViNoise noise;
    uint32_t *out;
    size_t pitch;
    /* For the helper threads: the display lines to a piece, and the next
     * piece. */
    int piece_lines;
    long next_piece;
} ViScan;

/* Display lines dy_first up to dy_end of a scan-out. The lines are drawn
 * from the framebuffer alone, so any stretch of them can be drawn apart
 * from the rest; within one, the rows a line shares with the line above
 * are fetched and filtered once. */
static void vi_scan_lines(const ViScan *s, int dy_first, int dy_end)
{
    const N64Machine *m = s->m;
    const ViWindow *w = s->w;
    const int out_scale = s->out_scale, cols = s->cols, y_add = s->y_add;
    const uint16_t *xi = s->xi;
    const uint8_t *xf = s->xf;
    ViRows r = s->rows;

    /* One block for every row: four fetched (pixels, lanes, and the list
     * of partly covered pixels at two to a word), four filtered, and the
     * scaler's vertically interpolated row. */
    const size_t fetch_n = s->fetch_n, out_n = s->out_n, spots_n = s->spots_n;
    uint32_t *rows = (uint32_t *)malloc(sizeof(uint32_t) * (4 * (2 * fetch_n + spots_n) + 5 * out_n));
    if (!rows) return;
    uint32_t *next = rows;
    for (int i = 0; i < 4; i++) {
        r.fetch[i] = next;
        r.lanes[i] = next + fetch_n;
        r.spots[i] = (uint16_t *)(next + 2 * fetch_n);
        r.fetch_y[i] = INT_MIN;
        next += 2 * fetch_n + spots_n;
    }
    for (int i = 0; i < 4; i++) {
        r.out[i >> 1][i & 1] = next;
        r.out_y[i >> 1][i & 1] = INT_MIN;
        next += out_n;
    }
    uint32_t *const blend = next;

    int drawn = -1;
    const uint32_t *drawn_line = NULL;
    for (int dy = dy_first; dy < dy_end; dy++) {
        if (s->serrate && (dy & 1) != (int)!m->vi_field) continue;
        /* Without serrate a line is drawn in both fields, so each
         * progressive line is two lines of the picture. */
        const int k = (dy - w->vstart) >> 1;
        const int n = k - s->k0;
        for (int t = 0; t < out_scale; t++) {
            uint32_t *line = s->out + ((size_t)(dy - w->vscan_start) * (size_t)out_scale + (size_t)t) * s->pitch +
                             (size_t)(w->dx0 - w->hscan_start) * (size_t)out_scale;
            int y = n * y_add * out_scale + s->y_start;
            if (!s->larger) {
                if (k == drawn) {
                    memcpy(line, drawn_line, sizeof(uint32_t) * (size_t)cols);
                    continue;
                }
                drawn = k;
                drawn_line = line;
            } else {
                /* Where this line lies in its progressive line: one of two
                 * lines' worth, or with serrate of the one line the field
                 * draws. */
                const int u = (s->serrate ? 0 : ((dy - w->vstart) & 1) * out_scale) + t;
                y += u * y_add / 2;
            }

            const int y_base = y >> 10, y_frac = (y >> 5) & 31;
            const uint32_t *top = vi_out_row(&r, y_base, false);
            if (!s->scale_aa) {
                for (int j = 0; j < cols; j++) line[j] = top[xi[j]] & 0xFFFFFFu;
            } else {
                const uint32_t *v = top;
                if (y_frac) {
                    /* The fetch bug: on a line that reads the same two rows as
                     * the line above it but not as the line below, the lower
                     * row comes from the bugged fetch. */
                    const bool bug =
                        s->fetch_bug && n != 0 && y_base == (y - y_add) >> 10 && y_base != (y + y_add) >> 10;
                    const uint32_t *bottom = vi_out_row(&r, y_base + 1, bug);
                    for (int i = 0; i < r.w; i++) blend[i] = vi_lerp(top[i], bottom[i], (uint32_t)y_frac);
                    v = blend;
                }
                for (int j = 0; j < cols; j++) {
                    const uint32_t *p = v + xi[j];
                    line[j] = xf[j] ? vi_lerp(p[0], p[1], xf[j]) : p[0] & 0xFFFFFFu;
                }
            }
            vi_gamma_line(line, cols, s->ctrl, &s->noise, (uint32_t)(n * out_scale + t));
        }
    }
    free(rows);
}

static void vi_scan_piece(void *scan, unsigned thread)
{
    ViScan *s = (ViScan *)scan;
    (void)thread;
    for (;;) {
        const int dy = s->w->dy0 + (int)n64_rdp_workers_take(&s->next_piece) * s->piece_lines;
        if (dy >= s->w->dy1) return;
        vi_scan_lines(s, dy, dy + s->piece_lines < s->w->dy1 ? dy + s->piece_lines : s->w->dy1);
    }
}

/*
 * The filtered picture into `out`, a line every `pitch` pixels.
 *
 * fb_scale 1 and out_scale 1 is the console's: the framebuffer through the
 * VI's filters onto the display's 640 columns. Otherwise it is the RDP's
 * larger picture (fb_scale 2 or 4 of its pixels to one of the console's)
 * through the same filters, which then work on the larger picture's pixels
 * and coverage as parallel-rdp has them do, onto out_scale columns and
 * lines for each of the display's. A line of the larger picture is sampled
 * where it lies; the console's two lines of a progressive line are one line
 * drawn twice, and stay so. The fetch bug, which is about the console
 * stretching a framebuffer row over several lines, is not carried over.
 *
 * The larger picture is several times the work and is shared out between
 * the RDP's threads, a few lines at a time.
 */
static void vi_scan_filtered(N64Machine *m, const ViWindow *w, int fb_scale, int out_scale, uint32_t *out,
                             size_t pitch)
{
    const uint32_t *io = m->vi_regs;
    const uint32_t ctrl = io[0];
    const uint32_t aa_mode = (ctrl >> 8) & 3u;
    const bool larger = fb_scale > 1;
    /* The step from one column or progressive line of `out` to the next,
     * and the start, in 1024ths of the pixels the rows are made of. */
    const int per = fb_scale / out_scale;
    const int x_add = (int)(io[12] & 0xFFFu) * per, y_add = (int)(io[13] & 0xFFFu) * per;
    int x_start = (int)((io[12] >> 16) & 0xFFFu) * fb_scale, y_start = (int)((io[13] >> 16) & 0xFFFu) * fb_scale;
    const int cols = (w->dx1 - w->dx0) * out_scale;
    if (cols <= 0 || w->dy0 >= w->dy1) return;

    /* decode_vi_registers: sampling starts at H_START and V_START, or at
     * the picture's edge when they are outside it. Lines are counted in
     * progressive lines (two of the picture's) from V_START. */
    int h0 = w->hstart, k0 = 0;
    if (h0 < w->hscan_start) {
        x_start += x_add * out_scale * (w->hscan_start - h0);
        h0 = w->hscan_start;
    }
    if (w->vstart < w->vscan_start) {
        k0 = (w->vscan_start - w->vstart) / 2;
        y_start += y_add * out_scale * k0;
    }

    /* Per column: which framebuffer columns it reads, and the fraction. */
    const int cx0 = (w->dx0 - h0) * out_scale;
    const int x_first = (cx0 * x_add + x_start) >> 10, x_last = ((cx0 + cols - 1) * x_add + x_start) >> 10;
    uint16_t *xi = (uint16_t *)malloc((sizeof(uint16_t) + sizeof(uint8_t)) * (size_t)cols);
    if (!xi) return;
    uint8_t *xf = (uint8_t *)(xi + cols);
    for (int j = 0; j < cols; j++) {
        const int x = (cx0 + j) * x_add + x_start;
        xi[j] = (uint16_t)((x >> 10) - x_first);
        xf[j] = (uint8_t)((x >> 5) & 31);
    }

    ViScan s;
    memset(&s, 0, sizeof s);
    s.m = m;
    s.w = w;
    s.rows.rdram = m->rdram;
    s.rows.hidden = m->rdp.links.hidden;
    s.rows.rdram_size = m->rdram_size;
    s.rows.rgba32 = (ctrl & 3u) == 3u;
    s.rows.origin = (io[1] & 0xFFFFFFu) >> (s.rows.rgba32 ? 2 : 1);
    s.rows.pitch = io[2] & 0xFFFu;
    s.rows.fetch_aa = aa_mode < 2u && (s.rows.rgba32 || s.rows.hidden); /* VI_CONTROL_META_AA_BIT */
    s.rows.dither_filter = (ctrl & VI_DITHER_FILTER) != 0;
    s.rows.divot = (ctrl & VI_DIVOT) != 0;
    s.rows.x0 = x_first;
    s.rows.w = x_last - x_first + 2;
    if (larger) {
        /* The bytes of RDRAM the picture's own rows are in: the larger
         * picture is asked for those, and waited for no further than them.
         * The filters also look a row above and below and a few columns
         * either side, which is other memory - often the next picture,
         * still being drawn; there they get the console's pixels. All of
         * RDRAM when the rows run off either end of it. */
        const int log2 = fb_scale == 2 ? 1 : 2;
        const int64_t unit = s.rows.rgba32 ? 4 : 2;
        const int64_t n_first = ((w->dy0 - w->vstart) >> 1) - k0, n_last = ((w->dy1 - 1 - w->vstart) >> 1) - k0;
        const int64_t row_first = ((n_first * y_add * out_scale + y_start) >> 10) >> log2;
        /* The last line's row and the row it is interpolated with. */
        const int64_t row_last = (((((n_last + 1) * y_add * out_scale + y_start - 1) >> 10) + 1) >> log2);
        int64_t lo = ((int64_t)s.rows.origin + row_first * s.rows.pitch) * unit;
        int64_t hi = ((int64_t)s.rows.origin + (row_last + 1) * s.rows.pitch) * unit;
        if (lo < 0 || hi > (int64_t)m->rdram_size || hi <= lo) {
            lo = 0;
            hi = (int64_t)m->rdram_size;
        }
        if (!n64_rdp_larger_view_of(&m->rdp, &s.rows.larger, (uint32_t)lo, (uint32_t)hi)) {
            free(xi);
            return;
        }
        s.rows.larger_lo = (uint32_t)lo;
        s.rows.larger_hi = (uint32_t)hi;
    }
    s.fetch_n = (size_t)(s.rows.w + 2 * VI_PAD);
    s.out_n = (size_t)(s.rows.w + 2);
    s.spots_n = (s.fetch_n + 1) / 2;
    s.ctrl = ctrl;
    s.serrate = (ctrl & VI_SERRATE) != 0;
    s.scale_aa = aa_mode < 3u; /* VI_CONTROL_META_SCALE_BIT */
    s.larger = larger;
    /* need_fetch_bug_emulation */
    s.fetch_bug = !larger && y_add < 1024;
    s.out_scale = out_scale;
    s.y_add = y_add;
    s.y_start = y_start;
    s.k0 = k0;
    s.cols = cols;
    s.xi = xi;
    s.xf = xf;
    s.out = out;
    s.pitch = pitch;

    s.noise.frame = (uint32_t)m->frame_count;
    s.noise.x0 = cx0;
    s.noise.z1 = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)cols);
    if (!s.noise.z1) {
        free(xi);
        return;
    }
    if (ctrl & VI_GAMMA_DITHER) {
        for (int j = 0; j < cols; j++) s.noise.z1[j] = ((s.noise.frame >> 8) ^ (uint32_t)(cx0 + j)) * VI_NOISE_PRIME;
    }

    /* The console's own picture is drawn in one go: a line can be a copy
     * of the line above it there. */
    const unsigned threads = larger ? n64_rdp_workers_threads() : 1u;
    s.piece_lines = 8;
    if (threads < 2u || n64_rdp_workers_run(vi_scan_piece, &s, threads - 1u, 1) != 0)
        vi_scan_lines(&s, w->dy0, w->dy1);
    free(s.noise.z1);
    free(xi);
}

void n64_vi_render(N64Machine *m)
{
    const uint32_t *io = m->vi_regs;
    const bool pal = m->cart.pal;
    const int hscan_start = pal ? 128 : 108, vscan_start = pal ? 44 : 34;
    const int hscan_len = 640, vscan_len = pal ? 576 : 480;
    const int hscan_stop = hscan_start + hscan_len, vscan_stop = vscan_start + vscan_len;
    m->out_width = hscan_len;
    m->out_height = vscan_len;

    const uint32_t depth = io[0] & 3u;
    const bool serrate = (io[0] & VI_SERRATE) != 0;
    const int hstart = (int)((io[9] >> 16) & 0x3FFu), hend = (int)(io[9] & 0x3FFu);
    const int vstart = (int)((io[10] >> 16) & 0x3FFu), vend = (int)(io[10] & 0x3FFu);

    /* Lines this field does not draw keep last field's picture when
     * interlaced; everything else starts black. */
    if (!serrate) memset(m->framebuffer, 0, sizeof m->framebuffer);
    /* The larger picture: a display pixel is one framebuffer pixel at
     * most, so the internal scale's worth of samples each way; the usual
     * picture has a framebuffer pixel to every two, and then half as many
     * tell it all. */
    int g = 0;
    if (m->large_frame && depth >= 2) {
        const uint32_t xscale = io[12] & 0xFFFu, yscale = io[13] & 0xFFFu;
        const int scale = n64_rdp_scale(&m->rdp);
        g = (xscale > 0x200u || yscale > 0x400u) ? scale : scale / 2;
        if (g < 1) g = 1;
        if (!serrate || g != m->large_scale)
            memset(m->large_frame, 0, (size_t)N64_FB_WIDTH * (size_t)g * (size_t)vscan_len * (size_t)g * sizeof(uint32_t));
    }
    m->large_scale = g;
    if (depth < 2) return;

    int dy0 = vstart, dy1 = vend < vstart ? vscan_stop : vend;
    int dx0 = hstart, dx1 = hend;
    if (dy0 < vscan_start) dy0 = vscan_start;
    if (dy1 > vscan_stop) dy1 = vscan_stop;
    if (dx0 < hscan_start) dx0 = hscan_start;
    if (dx1 > hscan_stop) dx1 = hscan_stop;
    /* The VI's guard band (as parallel-RDP and ares draw it). */
    if (dx0 >= hscan_start) dx0 += 8;
    if (dx1 < hscan_stop) dx1 -= 7;

    const ViWindow w = { hscan_start, vscan_start, hstart, vstart, dx0, dx1, dy0, dy1 };
    if (m->vi_raw) vi_scan_raw(m, &w);
    else vi_scan_filtered(m, &w, 1, 1, m->framebuffer, N64_FB_WIDTH);
    if (g) {
        if (m->vi_raw) vi_scan_larger(m, &w, g);
        else vi_scan_filtered(m, &w, n64_rdp_scale(&m->rdp), g, m->large_frame, (size_t)N64_FB_WIDTH * (size_t)g);
    }

    /* With serrate set but an odd V_SYNC the field never alternates, so a
     * television shows each drawn line over two lines of the picture: fill
     * the lines this field skips from the ones it drew. */
    if (serrate && (io[6] & 1u)) {
        for (int dy = dy0; dy < dy1; dy++) {
            if ((dy & 1) == (int)!m->vi_field) continue;
            int src = dy - 1 >= dy0 ? dy - 1 : dy + 1;
            if (src < dy1 && src >= vscan_start && dy >= vscan_start) {
                memcpy(m->framebuffer + (size_t)(dy - vscan_start) * N64_FB_WIDTH,
                       m->framebuffer + (size_t)(src - vscan_start) * N64_FB_WIDTH, sizeof(uint32_t) * N64_FB_WIDTH);
                if (g) {
                    const size_t lines = (size_t)N64_FB_WIDTH * (size_t)g * (size_t)g;
                    memcpy(m->large_frame + (size_t)(dy - vscan_start) * lines,
                           m->large_frame + (size_t)(src - vscan_start) * lines, sizeof(uint32_t) * lines);
                }
            }
        }
    }
}
