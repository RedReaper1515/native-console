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
 * The VI's scan-out (runtime-n64/src/n64_vi.c), checked pixel for pixel in
 * the host frame.
 *
 * Each test writes a small framebuffer into RDRAM - chosen colours, and
 * the coverage a 16-bit pixel keeps in its low bit and its two hidden bits
 * - sets the VI's registers, calls n64_vi_render() and compares the frame
 * with values worked out by hand from parallel-rdp's shader arithmetic
 * (the working is in the comments). Two things are computed rather than
 * written down: gamma dither's noise, a hash (noise.h's reseed_noise,
 * checked here against four values made independently), and the gamma
 * table, which is twice the integer square root of its index.
 *
 * The last test renders random framebuffers under random register settings
 * and compares every pixel of the frame with a transcription of the
 * shaders, one fragment at a time and with none of the runtime's
 * shortcuts. The transcription is of parallel-rdp (commit
 * 388d70f5835b352d841d9d9e5a08c5de01470f41): shaders/extract_vram.comp,
 * vi_fetch.frag, vi_divot.frag, vi_scale.frag and noise.h, whose notice is
 * the one above.
 *
 * n64_vi_test --time prints how long a scan-out takes instead.
 */
#include "n64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VI registers (N64Machine.vi_regs). */
enum { R_CONTROL = 0, R_ORIGIN = 1, R_WIDTH = 2, R_V_SYNC = 6, R_H_VIDEO = 9, R_V_VIDEO = 10, R_X_SCALE = 12, R_Y_SCALE = 13 };

/* VI_CONTROL. */
#define TYPE16 2u
#define TYPE32 3u
#define GAMMA_DITHER 0x4u
#define GAMMA 0x8u
#define DIVOT 0x10u
#define SERRATE 0x40u
#define AA_MODE(n) ((uint32_t)(n) << 8) /* 0, 1: anti-alias and resample; 2: resample; 3: neither */
#define DITHER_FILTER 0x10000u

/* The test framebuffer: PITCH pixels a row at FB, shown one to one from
 * the picture's top left, so that framebuffer pixel (x, y) is the frame's
 * column x on rows 2y and 2y + 1. Columns 8 to 72 are inside the guard
 * band, and ROWS rows are shown. */
#define FB 0x100000u
#define PITCH 80
#define ROWS 24
#define MARKER 0x00ABCDEFu /* what the frame holds before a render */

static N64Machine *m;
static int failures;
static int checks;
static const char *current_test = "";

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            printf("FAIL %s (%s:%d): ", current_test, __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

static uint32_t out(int col, int row) { return m->framebuffer[(size_t)row * N64_FB_WIDTH + (size_t)col]; }

#define EXPECT_PX(col, row, want)                                                                          \
    CHECK(out(col, row) == (uint32_t)(want), "frame (%d, %d) is %06X, expected %06X", (int)(col), (int)(row), \
          (unsigned)out(col, row), (unsigned)(want))

static uint32_t hv(unsigned start, unsigned end) { return start << 16 | end; }

/* A 16-bit pixel: 5 bits a channel, and 3 bits of coverage. */
static void put16(int x, int y, unsigned r, unsigned g, unsigned b, unsigned cov)
{
    const uint32_t a = (uint32_t)((int32_t)FB + 2 * (y * PITCH + x));
    n64_put_be16(m->rdram + a, (uint16_t)(r << 11 | g << 6 | b << 1 | cov >> 2));
    m->rdp.links.hidden[a >> 1] = (uint8_t)(cov & 3u);
}

/* A 32-bit pixel: 8 bits a channel; coverage is the top of the alpha byte. */
static void put32(int x, int y, unsigned r, unsigned g, unsigned b, unsigned cov)
{
    const uint32_t a = (uint32_t)((int32_t)FB + 4 * (y * PITCH + x));
    n64_put_be32(m->rdram + a, r << 24 | g << 16 | b << 8 | cov << 5);
}

static void render(void)
{
    for (size_t i = 0; i < (size_t)N64_FB_WIDTH * N64_FB_HEIGHT; i++) m->framebuffer[i] = MARKER;
    n64_vi_render(m);
}

/* A black, fully covered framebuffer (and the memory around it), shown
 * with no filter on. */
static void begin(const char *name, uint32_t type)
{
    current_test = name;
    memset(m->rdram + FB - 0x4000u, 0, 0x24000u);
    memset(m->rdp.links.hidden + (FB - 0x4000u) / 2u, 3, 0x12000u);
    for (int y = -16; y < 300; y++) {
        for (int x = 0; x < PITCH; x++) {
            if (type == TYPE32) {
                if (y < 100) put32(x, y, 0, 0, 0, 7);
            } else {
                put16(x, y, 0, 0, 0, 7);
            }
        }
    }
    memset(m->vi_regs, 0, sizeof m->vi_regs);
    m->vi_regs[R_CONTROL] = type | AA_MODE(3);
    m->vi_regs[R_ORIGIN] = FB;
    m->vi_regs[R_WIDTH] = PITCH;
    m->vi_regs[R_V_SYNC] = 525;
    m->vi_regs[R_H_VIDEO] = hv(108, 108 + PITCH);
    m->vi_regs[R_V_VIDEO] = hv(34, 34 + 2 * ROWS);
    m->vi_regs[R_X_SCALE] = 0x400;
    m->vi_regs[R_Y_SCALE] = 0x400;
    m->cart.pal = false;
    m->vi_field = 0;
    m->frame_count = 0;
    m->vi_raw = false;
}

/* ---- noise.h and the gamma table, for the expected values ---- */

/* reseed_noise(x, y, frame): the 16 bits the shaders call seeded_noise. */
static uint32_t noise(uint32_t x, uint32_t y, uint32_t frame)
{
    uint32_t s[3] = { x, y, frame };
    for (int round = 0; round < 3; round++) {
        const uint32_t t[3] = { ((s[0] >> 8) ^ s[1]) * 1103515245u, ((s[1] >> 8) ^ s[2]) * 1103515245u,
                                ((s[2] >> 8) ^ s[0]) * 1103515245u };
        memcpy(s, t, sizeof s);
    }
    return s[0] >> 16;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t root = 0;
    while ((root + 1u) * (root + 1u) <= v) root++;
    return root;
}

/* luts.hpp's gamma_table holds 2 * isqrt(colour << 6), then 2 * isqrt(i)
 * for the 14-bit i = (colour << 6) + noise. */
static uint32_t gamma_plain(uint32_t c) { return 2u * isqrt(c << 6); }
static uint32_t gamma_dithered(uint32_t c, uint32_t noise6) { return 2u * isqrt((c << 6) + noise6); }

/* ---- the stages ---- */

static void test_fetch(void)
{
    /* extract_vram.comp, 16 bits: r = (word >> 8) & 0xf8 and so on - the
     * five bits in the top of the byte, the low three zero. */
    begin("fetch 16-bit", TYPE16);
    put16(8, 0, 31, 0, 0, 7);
    put16(9, 0, 0, 31, 0, 7);
    put16(10, 0, 0, 0, 31, 7);
    put16(11, 0, 31, 31, 31, 0); /* coverage is not read in mode 3 */
    put16(12, 0, 1, 2, 3, 3);
    put16(7, 0, 31, 31, 31, 7);  /* in the left guard band */
    put16(72, 0, 31, 0, 31, 7);  /* the last column before the right one */
    put16(73, 0, 31, 31, 31, 7);
    put16(8, ROWS - 1, 0, 31, 31, 7);
    put16(8, ROWS, 31, 31, 31, 7); /* below V_END */
    render();
    CHECK(m->out_width == 640 && m->out_height == 480, "frame is %d x %d", m->out_width, m->out_height);
    EXPECT_PX(8, 0, 0xF80000);
    EXPECT_PX(8, 1, 0xF80000); /* a progressive line is two rows of the frame */
    EXPECT_PX(9, 0, 0x00F800);
    EXPECT_PX(10, 0, 0x0000F8);
    EXPECT_PX(11, 0, 0xF8F8F8);
    EXPECT_PX(12, 0, 0x081018);
    EXPECT_PX(13, 0, 0x000000);
    EXPECT_PX(7, 0, 0x000000);
    EXPECT_PX(72, 0, 0xF800F8);
    EXPECT_PX(73, 0, 0x000000);
    EXPECT_PX(8, 2 * ROWS - 1, 0x00F8F8);
    EXPECT_PX(8, 2 * ROWS, 0x000000);
    EXPECT_PX(8, 2, 0x000000);

    /* 32 bits: the three colour bytes as they are. */
    begin("fetch 32-bit", TYPE32);
    put32(8, 0, 0x12, 0x34, 0x56, 7);
    put32(9, 0, 0xFF, 0xFF, 0xFF, 0);
    put32(8, 1, 0x01, 0x80, 0xFE, 5);
    render();
    EXPECT_PX(8, 0, 0x123456);
    EXPECT_PX(9, 0, 0xFFFFFF);
    EXPECT_PX(8, 2, 0x0180FE);
    EXPECT_PX(8, 3, 0x0180FE);
    EXPECT_PX(10, 0, 0x000000);

    /* Blank and the reserved type show black. */
    for (uint32_t type = 0; type < 2; type++) {
        begin("blank", TYPE16);
        put16(8, 0, 31, 31, 31, 7);
        m->vi_regs[R_CONTROL] = type | AA_MODE(3);
        render();
        EXPECT_PX(8, 0, 0x000000);
        EXPECT_PX(300, 200, 0x000000);
    }
}

static void test_raw(void)
{
    /* The unfiltered path: nearest neighbour, five bits widened to eight by
     * repeating the top three ((v << 3) | (v >> 2)), every filter bit and
     * the coverage ignored. X_SCALE 0x200 shows each pixel twice. */
    begin("raw 16-bit", TYPE16);
    m->vi_raw = true;
    m->vi_regs[R_CONTROL] = TYPE16 | GAMMA_DITHER | GAMMA | DIVOT | DITHER_FILTER | AA_MODE(0);
    m->vi_regs[R_X_SCALE] = 0x200;
    put16(4, 0, 31, 31, 31, 7);
    put16(5, 0, 16, 0, 0, 0);
    put16(6, 0, 1, 2, 3, 7);
    put16(4, 1, 0, 21, 10, 3);
    render();
    EXPECT_PX(8, 0, 0xFFFFFF);
    EXPECT_PX(9, 0, 0xFFFFFF);
    EXPECT_PX(10, 0, 0x840000);
    EXPECT_PX(11, 0, 0x840000);
    EXPECT_PX(12, 0, 0x081018);
    EXPECT_PX(13, 0, 0x081018);
    EXPECT_PX(14, 0, 0x000000);
    EXPECT_PX(8, 1, 0xFFFFFF);
    EXPECT_PX(8, 2, 0x00AD52); /* 21 -> 0xAD, 10 -> 0x52 */
    EXPECT_PX(7, 0, 0x000000);

    begin("raw 32-bit", TYPE32);
    m->vi_raw = true;
    m->vi_regs[R_CONTROL] = TYPE32 | GAMMA | AA_MODE(0);
    put32(8, 0, 0x12, 0x34, 0x56, 2);
    render();
    EXPECT_PX(8, 0, 0x123456);

    /* The same framebuffer through the filters is not the raw picture. */
    begin("raw is not the default", TYPE16);
    put16(8, 0, 31, 31, 31, 7);
    render();
    EXPECT_PX(8, 0, 0xF8F8F8);
}

/* The anti-aliasing test's pixel P at (20, 5) and the six neighbours
 * vi_fetch.frag reads: A (19, 4), B (21, 4), C (18, 5), D (22, 5),
 * E (19, 6), F (21, 6). */
static void aa_scene(uint32_t type, unsigned cov)
{
    static const struct { int x, y; unsigned r, g, b; } px[7] = {
        { 20, 5, 5, 25, 0 },  { 19, 4, 10, 2, 31 }, { 21, 4, 15, 5, 31 }, { 18, 5, 20, 10, 31 },
        { 22, 5, 25, 15, 31 }, { 19, 6, 2, 20, 31 }, { 21, 6, 30, 30, 31 },
    };
    for (int i = 0; i < 7; i++) {
        const unsigned c = i ? 7u : cov;
        if (type == TYPE32) put32(px[i].x, px[i].y, px[i].r << 3, px[i].g << 3, px[i].b << 3, c);
        else put16(px[i].x, px[i].y, px[i].r, px[i].g, px[i].b, c);
    }
}

static void test_aa(void)
{
    /* vi_fetch.frag, coverage below 7:
     *   colour = mid + (((second_lo + second_hi - 2 * mid) * (7 - cov) + 4) >> 3)
     * with second_lo and second_hi the second lowest and second highest of
     * the pixel and its fully covered neighbours.
     *
     * Red: P 40; A 80, B 120, C 160, D 200, E 16, F 240. Sorted with P:
     * 16 40 80 120 160 200 240, so second_lo = 40, second_hi = 200 and the
     * offset is 40 + 200 - 80 = 160.
     * Green: P 200; A 16, B 40, C 80, D 120, E 160, F 240. Sorted:
     * 16 40 80 120 160 200 240: second_lo 40, second_hi 200, offset
     * 240 - 400 = -160 (the shifts below floor).
     * Blue: P 0, every neighbour 248: second_lo 0, second_hi 248, offset 248.
     *
     *   cov  7 - cov   red                 green                 blue
     *    6      1      40 +  20 =  60      200 -  20 = 180       0 +  31 =  31
     *    5      2      40 +  40 =  80      200 -  40 = 160       0 +  62 =  62
     *    4      3      40 +  60 = 100      200 -  60 = 140       0 +  93 =  93
     *    3      4      40 +  80 = 120      200 -  80 = 120       0 + 124 = 124
     *    2      5      40 + 100 = 140      200 - 100 = 100       0 + 155 = 155
     *    1      6      40 + 120 = 160      200 - 120 =  80       0 + 186 = 186
     *    0      7      40 + 140 = 180      200 - 140 =  60       0 + 217 = 217
     * e.g. cov 3, red (160 * 4 + 4) >> 3 = 80; green (-640 + 4) >> 3 = -80;
     * blue (992 + 4) >> 3 = 124. */
    static const uint32_t want[8] = { 0xB43CD9, 0xA050BA, 0x8C649B, 0x78787C, 0x648C5D, 0x50A03E, 0x3CB41F, 0x28C800 };
    for (unsigned cov = 0; cov < 8; cov++) {
        begin("anti-aliasing weights", TYPE16);
        m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0);
        aa_scene(TYPE16, cov);
        render();
        EXPECT_PX(20, 10, want[cov]);
        EXPECT_PX(20, 11, want[cov]);
        EXPECT_PX(19, 8, 0x5010F8); /* A, fully covered, is left alone */
        EXPECT_PX(21, 12, 0xF0F0F8); /* F */
    }

    /* A neighbour that is itself partly covered is not counted. Without
     * F: red 16 40 80 120 160 200, second_hi 160, offset 120, and
     * (120 * 4 + 4) >> 3 = 60, so 100; green and blue as before. */
    begin("anti-aliasing skips partly covered neighbours", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0);
    aa_scene(TYPE16, 3);
    put16(21, 6, 30, 30, 31, 6);
    render();
    EXPECT_PX(20, 10, 0x64787C);

    /* Mode 1 anti-aliases like mode 0; modes 2 and 3 do not read coverage. */
    for (unsigned mode = 1; mode < 4; mode++) {
        begin("anti-aliasing modes", TYPE16);
        m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(mode);
        aa_scene(TYPE16, 3);
        render();
        EXPECT_PX(20, 10, mode == 1 ? 0x78787C : 0x28C800);
    }

    /* 32 bits: the same colours, the coverage in the alpha byte's top bits. */
    begin("anti-aliasing 32-bit", TYPE32);
    m->vi_regs[R_CONTROL] = TYPE32 | AA_MODE(0);
    aa_scene(TYPE32, 3);
    render();
    EXPECT_PX(20, 10, 0x78787C);
    begin("anti-aliasing 32-bit", TYPE32);
    m->vi_regs[R_CONTROL] = TYPE32 | AA_MODE(0);
    aa_scene(TYPE32, 0);
    render();
    EXPECT_PX(20, 10, 0xB43CD9);

    /* The rounding (+ 4, then >> 3 flooring) shows only in 32-bit pictures:
     * 16-bit colours are multiples of 8. P (100, 100, 7), every neighbour
     * (104, 96, 19): offsets 100 + 104 - 200 = 4, 96 + 100 - 200 = -4 and
     * 7 + 19 - 14 = 12.
     *   cov 6: (4 + 4) >> 3 = 1, (-4 + 4) >> 3 = 0, (12 + 4) >> 3 = 2
     *   cov 4: (12 + 4) >> 3 = 2, (-12 + 4) >> 3 = -1, (36 + 4) >> 3 = 5 */
    for (int pass = 0; pass < 2; pass++) {
        begin("anti-aliasing rounding", TYPE32);
        m->vi_regs[R_CONTROL] = TYPE32 | AA_MODE(0);
        put32(20, 5, 100, 100, 7, pass ? 4 : 6);
        put32(19, 4, 104, 96, 19, 7);
        put32(21, 4, 104, 96, 19, 7);
        put32(18, 5, 104, 96, 19, 7);
        put32(22, 5, 104, 96, 19, 7);
        put32(19, 6, 104, 96, 19, 7);
        put32(21, 6, 104, 96, 19, 7);
        render();
        EXPECT_PX(20, 10, pass ? 0x66630C : 0x656409);
    }
}

static void test_dedither(void)
{
    /* vi_fetch.frag, full coverage, dither filter on: each channel becomes
     * (mid & 0xf8) + the sum over the eight neighbours of
     * clamp((neighbour >> 3) - (mid >> 3), -1, 1).
     *
     * A checkerboard of v and v + 1 - what the RDP's dither makes of a
     * colour half way between - has four neighbours one above each v pixel
     * and four one below each v + 1 pixel: 8v + 4 and 8(v + 1) - 4, the
     * same flat colour. Red v = 10: 84; green v = 20: 164; blue v = 0: 4. */
    for (int pass = 0; pass < 2; pass++) {
        begin(pass ? "dither filter off" : "dither filter on a dither pattern", TYPE16);
        m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0) | (pass ? 0u : DITHER_FILTER);
        for (int y = 0; y <= 12; y++) {
            for (int x = 8; x <= 40; x++) {
                const unsigned odd = (unsigned)(x + y) & 1u;
                put16(x, y, 10 + odd, 20 + odd, odd, 7);
            }
        }
        render();
        if (!pass) {
            EXPECT_PX(20, 10, 0x54A404);
            EXPECT_PX(21, 10, 0x54A404);
            EXPECT_PX(20, 12, 0x54A404);
            EXPECT_PX(30, 20, 0x54A404);
            EXPECT_PX(39, 22, 0x54A404);
        } else {
            EXPECT_PX(20, 10, 0x58A808); /* (20, 5): odd */
            EXPECT_PX(21, 10, 0x50A000);
        }
    }

    /* A step from 10 to 11 at x = 30: the last column of 10s has three
     * neighbours above it (80 + 3), the first of 11s three below (88 - 3). */
    begin("dither filter on a step", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0) | DITHER_FILTER;
    for (int y = 0; y <= 6; y++) {
        for (int x = 8; x <= 60; x++) put16(x, y, x < 30 ? 10 : 11, x < 30 ? 10 : 11, x < 30 ? 10 : 11, 7);
    }
    /* The difference is clamped: a 10 among 31s, 0s and 10s moves by 8. */
    for (int y = 10; y <= 14; y++) {
        for (int x = 48; x <= 52; x++) put16(x, y, 31, 0, 10, 7);
    }
    put16(50, 12, 10, 10, 10, 7);
    /* A partly covered pixel is anti-aliased, not filtered: 80 among black
     * with coverage 0 is second_lo 0, second_hi 80, offset -80 and
     * 80 + ((-80 * 7 + 4) >> 3) = 80 - 70 = 10. */
    put16(64, 12, 10, 10, 10, 0);
    render();
    EXPECT_PX(28, 6, 0x505050);
    EXPECT_PX(29, 6, 0x535353);
    EXPECT_PX(30, 6, 0x555555);
    EXPECT_PX(31, 6, 0x585858);
    EXPECT_PX(50, 24, 0x584850);
    EXPECT_PX(64, 24, 0x0A0A0A);

    /* With anti-aliasing off (mode 2) every pixel counts as fully covered,
     * and that one is filtered instead: 80 - 8. */
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2) | DITHER_FILTER;
    render();
    EXPECT_PX(64, 24, 0x484848);
    EXPECT_PX(29, 6, 0x535353);

    /* 32 bits: the comparison is of the top five bits and the result
     * starts from mid & 0xf8, so the filter drops the pixel's low three
     * bits. 87 among (87, 95, 80): red 80 + 0; green 95 >> 3 = 11 is above
     * 87 >> 3 = 10 eight times, 80 + 8; blue 80 >> 3 = 10, 80 + 0. */
    begin("dither filter 32-bit", TYPE32);
    m->vi_regs[R_CONTROL] = TYPE32 | AA_MODE(0) | DITHER_FILTER;
    for (int y = 4; y <= 6; y++) {
        for (int x = 19; x <= 21; x++) put32(x, y, 87, 95, 80, 7);
    }
    put32(20, 5, 87, 87, 87, 7);
    render();
    EXPECT_PX(20, 10, 0x505850);
}

static void divot_scene(unsigned left_cov)
{
    for (int y = 3; y <= 7; y++) {
        for (int x = 8; x <= 40; x++) put16(x, y, 12, 12, 12, 7);
    }
    put16(19, 5, 10, 20, 30, left_cov);
    put16(20, 5, 25, 3, 12, 7); /* the spike */
    put16(21, 5, 5, 25, 20, 7);
}

static void test_divot(void)
{
    /* vi_divot.frag: where a pixel or a horizontal neighbour is partly
     * covered, each channel is the median of the three (after
     * anti-aliasing).
     *
     * On a grey (96) field: (19, 5) is (80, 160, 240) with coverage 6,
     * (20, 5) a one-pixel spike (200, 24, 96), (21, 5) (40, 200, 160).
     *
     * Anti-aliasing first, on (19, 5), whose six neighbours are five greys
     * and (21, 5), coefficient 1:
     *   red   80: second_lo 80 (only the 40 is below), second_hi 96,
     *         offset 16, (16 + 4) >> 3 = 2: 82
     *   green 160: second_lo 96, second_hi 160 (only the 200 is above),
     *         offset -64, (-64 + 4) >> 3 = -8: 152
     *   blue  240: second_lo 96, second_hi 240, offset -144,
     *         (-144 + 4) >> 3 = -18: 222
     * Then the spike: median(82, 200, 40) = 82, median(152, 24, 200) = 152,
     * median(222, 96, 160) = 160. (19, 5) itself, between grey and the
     * spike: median(96, 82, 200), median(96, 152, 24), median(96, 222, 96)
     * - all 96. (21, 5) and its neighbours are fully covered: untouched. */
    begin("divot filter", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0) | DIVOT;
    divot_scene(6);
    render();
    EXPECT_PX(18, 10, 0x606060);
    EXPECT_PX(19, 10, 0x606060);
    EXPECT_PX(20, 10, 0x5298A0);
    EXPECT_PX(20, 11, 0x5298A0);
    EXPECT_PX(21, 10, 0x28C8A0);
    EXPECT_PX(22, 10, 0x606060);

    begin("divot filter off", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0);
    divot_scene(6);
    render();
    EXPECT_PX(19, 10, 0x5298DE);
    EXPECT_PX(20, 10, 0xC81860);

    /* Three fully covered pixels: the spike stays. */
    begin("divot filter needs partial coverage", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0) | DIVOT;
    divot_scene(7);
    render();
    EXPECT_PX(19, 10, 0x50A0F0);
    EXPECT_PX(20, 10, 0xC81860);

    /* The partly covered pixel on the spike's right instead: a grey with
     * coverage 6 at (31, 5), which anti-aliasing leaves grey (its six
     * neighbours are grey; the spike is not one of them). The spike at
     * (30, 5) is the median of grey, itself and grey. */
    begin("divot filter, partly covered pixel on the right", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0) | DIVOT;
    for (int y = 3; y <= 7; y++) {
        for (int x = 8; x <= 40; x++) put16(x, y, 12, 12, 12, 7);
    }
    put16(30, 5, 25, 3, 12, 7);
    put16(31, 5, 12, 12, 12, 6);
    render();
    EXPECT_PX(30, 10, 0x606060);
    EXPECT_PX(31, 10, 0x606060);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0);
    render();
    EXPECT_PX(30, 10, 0xC81860);
}

static void test_edges(void)
{
    /* The filters read past what is shown: the first and last pixels the
     * scaler reads take their divot neighbours, anti-aliased, from the
     * columns outside. X_SCALE 0x200 with H_END one column further: column
     * 8 is pixel 4, and the last column, 73, is half way between pixels 36
     * and 37.
     *
     * On row 2, (3, 2) and (38, 2) are 160 with coverage 3 under two 248s
     * (their upper diagonal neighbours); their other neighbours are black
     * or the 80 two columns in. Sorted: 0 0 0 80 160 248 248, second_lo 0,
     * second_hi 248, offset 248 - 320 = -72, and 160 + ((-72 * 4 + 4) >> 3)
     * = 160 - 36 = 124.
     *
     * Left: (4, 2) = 200, (5, 2) = 80. Pixel 4 is median(124, 200, 80) =
     * 124; column 9 is half way to pixel 5's 80: (124 * 16 + 80 * 16 + 16)
     * >> 5 = 102.
     * Right: (36, 2) = 80, (37, 2) = 200. Pixel 37 is median(80, 200, 124)
     * = 124, and column 73 is (80 * 16 + 124 * 16 + 16) >> 5 = 102. */
    begin("filters at the edges of what is shown", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0) | DIVOT;
    m->vi_regs[R_X_SCALE] = 0x200;
    m->vi_regs[R_H_VIDEO] = hv(108, 108 + PITCH + 1);
    put16(2, 1, 31, 31, 31, 7);
    put16(4, 1, 31, 31, 31, 7);
    put16(3, 2, 20, 20, 20, 3);
    put16(4, 2, 25, 25, 25, 7);
    put16(5, 2, 10, 10, 10, 7);
    put16(37, 1, 31, 31, 31, 7);
    put16(39, 1, 31, 31, 31, 7);
    put16(36, 2, 10, 10, 10, 7);
    put16(37, 2, 25, 25, 25, 7);
    put16(38, 2, 20, 20, 20, 3);
    render();
    EXPECT_PX(8, 4, 0x7C7C7C);
    EXPECT_PX(9, 4, 0x666666);
    EXPECT_PX(10, 4, 0x505050);
    EXPECT_PX(72, 4, 0x505050);
    EXPECT_PX(73, 4, 0x666666);
    EXPECT_PX(74, 4, 0x000000);
}

static void test_scale_horizontal(void)
{
    /* vi_scale.frag: x = X_OFFSET + X_SCALE * column, the pixel is x >> 10
     * and the fraction (x >> 5) & 31; vi_lerp is
     *   a + (((b - a) * frac + 16) >> 5).
     * P = (248, 128, 8) between black pixels.
     *   frac  8: (248 *  8 + 16) >> 5 =  62, (128 *  8 + 16) >> 5 = 32, (8 *  8 + 16) >> 5 = 2
     *   frac 16: (248 * 16 + 16) >> 5 = 124, (128 * 16 + 16) >> 5 = 64, (8 * 16 + 16) >> 5 = 4
     *   frac 24: (248 * 24 + 16) >> 5 = 186, (128 * 24 + 16) >> 5 = 96, (8 * 24 + 16) >> 5 = 6
     * and the same going back down to black, e.g. frac 8 from P:
     * 248 + ((-248 * 8 + 16) >> 5) = 248 - 62 = 186. */
    begin("horizontal interpolation, X_SCALE 0x200", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2);
    m->vi_regs[R_X_SCALE] = 0x200;
    put16(5, 0, 31, 16, 1, 7);
    render();
    EXPECT_PX(8, 0, 0x000000);  /* pixel 4 */
    EXPECT_PX(9, 0, 0x7C4004);  /* 4 and 5, frac 16 */
    EXPECT_PX(10, 0, 0xF88008); /* pixel 5 */
    EXPECT_PX(11, 0, 0x7C4004); /* 5 and 6, frac 16 */
    EXPECT_PX(12, 0, 0x000000);
    EXPECT_PX(9, 1, 0x7C4004);

    /* Mode 3 does not interpolate: the pixel at x >> 10. */
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3);
    render();
    EXPECT_PX(9, 0, 0x000000);
    EXPECT_PX(10, 0, 0xF88008);
    EXPECT_PX(11, 0, 0xF88008);
    EXPECT_PX(12, 0, 0x000000);

    begin("horizontal interpolation, X_SCALE 0x100", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2);
    m->vi_regs[R_X_SCALE] = 0x100;
    put16(3, 0, 31, 16, 1, 7);
    render();
    {
        static const uint32_t want[9] = { 0x000000, 0x3E2002, 0x7C4004, 0xBA6006, 0xF88008,
                                          0xBA6006, 0x7C4004, 0x3E2002, 0x000000 };
        for (int i = 0; i < 9; i++) EXPECT_PX(8 + i, 0, want[i]);
    }

    /* The subpixel offset: x = 0x155 + 0x200 * column.
     *   column 8: 4437, pixel 4, frac (4437 >> 5) & 31 = 10
     *     (248 * 10 + 16) >> 5 = 78, (128 * 10 + 16) >> 5 = 40, (8 * 10 + 16) >> 5 = 3
     *   column 9: 4949, pixel 4, frac 26
     *     (248 * 26 + 16) >> 5 = 202, (128 * 26 + 16) >> 5 = 104, (8 * 26 + 16) >> 5 = 7
     *   column 10: 5461, pixel 5, frac 10, P towards black
     *     248 + ((-2480 + 16) >> 5) = 171, 128 + ((-1280 + 16) >> 5) = 88, 8 + ((-80 + 16) >> 5) = 6
     *   column 11: 5973, pixel 5, frac 26
     *     248 + ((-6448 + 16) >> 5) = 47, 128 + ((-3328 + 16) >> 5) = 24, 8 + ((-208 + 16) >> 5) = 2 */
    begin("horizontal interpolation with an offset", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2);
    m->vi_regs[R_X_SCALE] = 0x155u << 16 | 0x200u;
    put16(5, 0, 31, 16, 1, 7);
    render();
    EXPECT_PX(8, 0, 0x4E2803);
    EXPECT_PX(9, 0, 0xCA6807);
    EXPECT_PX(10, 0, 0xAB5806);
    EXPECT_PX(11, 0, 0x2F1802);
    EXPECT_PX(12, 0, 0x000000);

    /* Odd fractions, and where the + 16 decides: offset 0x175 gives
     * fractions 11 (columns 8 and 10) and 27 (9 and 11). On row 1 a grey
     * of 16, for which 16 * 11 = 176 and 16 * 27 = 432 are both 16 short
     * of a multiple of 32:
     *   P:    (248 * 11 + 16) >> 5 = 85, (128 * 11 + 16) >> 5 = 44, (8 * 11 + 16) >> 5 = 3
     *         (248 * 27 + 16) >> 5 = 209, (128 * 27 + 16) >> 5 = 108, (8 * 27 + 16) >> 5 = 7
     *         248 + ((-2728 + 16) >> 5) = 163, 128 + ((-1408 + 16) >> 5) = 84, 8 + ((-88 + 16) >> 5) = 5
     *         248 + ((-6696 + 16) >> 5) = 39, 128 + ((-3456 + 16) >> 5) = 20, 8 + ((-216 + 16) >> 5) = 1
     *   grey: (176 + 16) >> 5 = 6, (432 + 16) >> 5 = 14,
     *         16 + ((-176 + 16) >> 5) = 11, 16 + ((-432 + 16) >> 5) = 3 */
    m->vi_regs[R_X_SCALE] = 0x175u << 16 | 0x200u;
    put16(5, 1, 2, 2, 2, 7);
    render();
    EXPECT_PX(8, 0, 0x552C03);
    EXPECT_PX(9, 0, 0xD16C07);
    EXPECT_PX(10, 0, 0xA35405);
    EXPECT_PX(11, 0, 0x271401);
    EXPECT_PX(8, 2, 0x060606);
    EXPECT_PX(9, 2, 0x0E0E0E);
    EXPECT_PX(10, 2, 0x0B0B0B);
    EXPECT_PX(11, 2, 0x030303);
}

static void test_scale_vertical(void)
{
    /* y = Y_OFFSET + Y_SCALE * line, lines counted from V_START in
     * progressive lines; the weights are the horizontal ones. Row 1 is
     * P = (248, 128, 8), rows 0 and 2 black. */
    begin("vertical interpolation, Y_SCALE 0x200", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2);
    m->vi_regs[R_Y_SCALE] = 0x200;
    for (int x = 0; x < PITCH; x++) put16(x, 1, 31, 16, 1, 7);
    render();
    EXPECT_PX(20, 0, 0x000000); /* line 0: row 0 */
    EXPECT_PX(20, 1, 0x000000);
    EXPECT_PX(20, 2, 0x7C4004); /* line 1: rows 0 and 1, frac 16 */
    EXPECT_PX(20, 3, 0x7C4004);
    EXPECT_PX(20, 4, 0xF88008); /* line 2: row 1 */
    EXPECT_PX(20, 6, 0x7C4004); /* line 3: rows 1 and 2, frac 16 */
    EXPECT_PX(20, 8, 0x000000);

    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3);
    render();
    EXPECT_PX(20, 2, 0x000000);
    EXPECT_PX(20, 4, 0xF88008);
    EXPECT_PX(20, 6, 0xF88008);
    EXPECT_PX(20, 8, 0x000000);

    /* The offset: frac (0x100 >> 5) & 31 = 8 on every line. */
    begin("vertical interpolation with an offset", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2);
    m->vi_regs[R_Y_SCALE] = 0x100u << 16 | 0x400u;
    for (int x = 0; x < PITCH; x++) put16(x, 1, 31, 16, 1, 7);
    render();
    EXPECT_PX(20, 0, 0x3E2002); /* rows 0 and 1, frac 8 */
    EXPECT_PX(20, 2, 0xBA6006); /* rows 1 and 2, frac 8 */
    EXPECT_PX(20, 4, 0x000000);

    /* An odd fraction: (0x160 >> 5) & 31 = 11, the weights of the
     * horizontal test: (85, 44, 3) going up to P and (163, 84, 5) down. */
    m->vi_regs[R_Y_SCALE] = 0x160u << 16 | 0x400u;
    render();
    EXPECT_PX(20, 0, 0x552C03);
    EXPECT_PX(20, 2, 0xA35405);

    /* Both at once, on one pixel: P at (5, 1). Column 9 of line 0 reads
     * pixels 4 and 5 of rows 0 and 1 with fractions 16 across and 8 down,
     * down first: (248 * 8 + 16) >> 5 = 62, then (62 * 16 + 16) >> 5 = 31;
     * green 32 then 16; blue 2 then 1. */
    begin("interpolation in both directions", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2);
    m->vi_regs[R_X_SCALE] = 0x200;
    m->vi_regs[R_Y_SCALE] = 0x100u << 16 | 0x400u;
    put16(5, 1, 31, 16, 1, 7);
    render();
    EXPECT_PX(9, 0, 0x1F1001);
    EXPECT_PX(10, 0, 0x3E2002);
    EXPECT_PX(10, 2, 0xBA6006);
    /* Column 11 of line 1: rows 1 and 2, frac 8: 186, 96, 6; then with
     * black across at 16: (186 * 16 + 16) >> 5 = 93, 48, 3. */
    EXPECT_PX(11, 2, 0x5D3003);
}

static void test_fetch_bug(void)
{
    /* video_interface.cpp's need_fetch_bug_emulation, vi_fetch.frag's
     * FragColorFetchBug and vi_scale.frag's bug_offset: with Y_SCALE below
     * 0x400, a line that reads the same two rows as the line above it and
     * not as the line below takes its lower row from a fetch that did not
     * read the row under it; the left and right neighbours count twice
     * instead.
     *
     * Dither filter, rows 0 and 1 all 10 (80), row 2 all 11 (88), black
     * above and below. Filtered normally: row 0 = 80 - 3 (three black
     * above) = 77; row 1 = 80 + 3 (three 11s below) = 83; row 2 =
     * 88 - 3 - 3 = 82. Fetch-bugged: row 1 = 80 (the row below is not
     * read), row 2 = 88 - 3 = 85, row 3 (black under 11s) = 0 + 3.
     *
     * Y_SCALE 0x200:
     *   line 0: y 0, row 0                                   77
     *   line 1: y 0x200, rows 0 and 1, frac 16, bugged:
     *           (77 * 16 + 80 * 16 + 16) >> 5                79 (80 unbugged)
     *   line 2: y 0x400, row 1                               83
     *   line 3: rows 1 and 2, bugged: (83 + 85) * 16 + 16    84 (83 unbugged)
     *   line 4: row 2                                        82
     *   line 5: rows 2 and 3, bugged: (82 + 3) * 16 + 16     43 */
    begin("fetch bug, dither filter", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2) | DITHER_FILTER;
    m->vi_regs[R_Y_SCALE] = 0x200;
    for (int x = 0; x < PITCH; x++) {
        put16(x, 0, 10, 10, 10, 7);
        put16(x, 1, 10, 10, 10, 7);
        put16(x, 2, 11, 11, 11, 7);
    }
    /* Left and right counted twice: 12s either side of (31, 1). Its row
     * above, (31, 0), is 80 - 3 + 2 = 79 (two 12s diagonally below); the
     * bugged (31, 1) is 80 + 2 * 2 = 84: (79 * 16 + 84 * 16 + 16) >> 5 = 82. */
    put16(30, 1, 12, 12, 12, 7);
    put16(32, 1, 12, 12, 12, 7);
    render();
    EXPECT_PX(20, 0, 0x4D4D4D);
    EXPECT_PX(20, 2, 0x4F4F4F);
    EXPECT_PX(20, 3, 0x4F4F4F);
    EXPECT_PX(20, 4, 0x535353);
    EXPECT_PX(20, 6, 0x545454);
    EXPECT_PX(20, 8, 0x525252);
    EXPECT_PX(20, 10, 0x2B2B2B);
    EXPECT_PX(31, 2, 0x525252);

    /* Y_SCALE 0x300: only line 1 repeats the rows of the line above it.
     *   line 0: y 0, row 0                                              77
     *   line 1: y 0x300, rows 0 and 1, frac 24, bugged:
     *           (77 * 8 + 80 * 24 + 16) >> 5                            79 (82 unbugged)
     *   line 2: y 0x600, rows 1 and 2, frac 16; the line above read
     *           rows 0 and 1, so not bugged: (83 * 16 + 82 * 16 + 16) >> 5   83 (84 bugged)
     *   line 3: y 0x900, rows 2 and 3, frac 8, not bugged; row 3 is
     *           0 + 3: (82 * 24 + 3 * 8 + 16) >> 5                      62 */
    m->vi_regs[R_Y_SCALE] = 0x300;
    render();
    EXPECT_PX(20, 0, 0x4D4D4D);
    EXPECT_PX(20, 2, 0x4F4F4F);
    EXPECT_PX(20, 4, 0x535353);
    EXPECT_PX(20, 6, 0x3E3E3E);

    /* The same rows at Y_SCALE 0x400 with half a line's offset are not
     * bugged: (77 * 16 + 83 * 16 + 16) >> 5 = 80. */
    m->vi_regs[R_Y_SCALE] = 0x200u << 16 | 0x400u;
    render();
    EXPECT_PX(20, 0, 0x505050);

    /* Anti-aliasing: P (20, 1) = 40 with coverage 3; (19, 0) 80,
     * (21, 0) 120, (18, 1) 160, (22, 1) 200; black below.
     * Normally: 0 0 40 80 120 160 200, second_lo 0, second_hi 160,
     * offset 80, 40 + ((80 * 4 + 4) >> 3) = 80.
     * Bugged: the two blacks below are not read and 160 and 200 count
     * again: second_lo 40, second_hi 200, offset 160: 120.
     *   line 1 (rows 0 and 1, bugged): (0 + 120 * 16 + 16) >> 5 = 60
     *   line 2 (row 1): 80
     *   line 3 (rows 1 and 2, bugged; row 2 is black): (80 * 16 + 16) >> 5 = 40 */
    begin("fetch bug, anti-aliasing", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(0);
    m->vi_regs[R_Y_SCALE] = 0x200;
    put16(19, 0, 10, 10, 10, 7);
    put16(21, 0, 15, 15, 15, 7);
    put16(18, 1, 20, 20, 20, 7);
    put16(20, 1, 5, 5, 5, 3);
    put16(22, 1, 25, 25, 25, 7);
    render();
    EXPECT_PX(20, 2, 0x3C3C3C);
    EXPECT_PX(20, 4, 0x505050);
    EXPECT_PX(20, 6, 0x282828);
}

static void test_gamma(void)
{
    /* Four values of reseed_noise, made independently of this file. */
    current_test = "noise";
    CHECK(noise(8, 0, 0) == 0x03EDu, "noise(8, 0, 0) = %04X", (unsigned)noise(8, 0, 0));
    CHECK(noise(9, 0, 0) == 0x846Bu, "noise(9, 0, 0) = %04X", (unsigned)noise(9, 0, 0));
    CHECK(noise(300, 5, 1234) == 0x1FC7u, "noise(300, 5, 1234) = %04X", (unsigned)noise(300, 5, 1234));
    CHECK(noise(639, 239, 0x12345) == 0xF1F1u, "noise(639, 239, 0x12345) = %04X", (unsigned)noise(639, 239, 0x12345));

    /* Gamma: the table at the colour. 2 * isqrt(c << 6): 8 -> 2 * 22 = 44,
     * 40 -> 2 * 50 = 100, 64 -> 128, 128 -> 2 * 90 = 180, 200 -> 2 * 113 =
     * 226, 248 -> 2 * 125 = 250. */
    begin("gamma", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3) | GAMMA;
    put16(9, 0, 1, 5, 8, 7);
    put16(10, 0, 16, 25, 31, 7);
    render();
    EXPECT_PX(8, 0, 0x000000);
    EXPECT_PX(9, 0, 0x2C6480);
    EXPECT_PX(10, 0, 0xB4E2FA);

    /* Every colour value, through 32-bit pixels: plain, then with gamma
     * dither, which looks the table up at (c << 6) + six bits of noise
     * (noise_get_full_gamma_dither: red seed & 0x3f, green (seed >> 6) &
     * 0x3f, blue ((seed >> 9) & 0x38) | (seed & 7)). The noise is seeded
     * with the column counted from H_START, the progressive line and the
     * frame. */
    for (int pass = 0; pass < 2; pass++) {
        begin(pass ? "gamma with gamma dither" : "gamma, every value", TYPE32);
        m->vi_regs[R_CONTROL] = TYPE32 | AA_MODE(3) | GAMMA | (pass ? GAMMA_DITHER : 0u);
        m->frame_count = 7;
        for (unsigned v = 0; v < 256; v++) put32(8 + (int)(v & 63u), (int)(v >> 6), v, 255u - v, v ^ 0x55u, 7);
        render();
        for (unsigned v = 0; v < 256; v++) {
            const int col = 8 + (int)(v & 63u), line = (int)(v >> 6);
            const uint32_t s = noise((uint32_t)col, (uint32_t)line, 7);
            const uint32_t r = v, g = 255u - v, b = v ^ 0x55u;
            const uint32_t want = pass ? gamma_dithered(r, s & 0x3Fu) << 16 | gamma_dithered(g, (s >> 6) & 0x3Fu) << 8 |
                                             gamma_dithered(b, ((s >> 9) & 0x38u) | (s & 7u))
                                       : gamma_plain(r) << 16 | gamma_plain(g) << 8 | gamma_plain(b);
            EXPECT_PX(col, 2 * line, want);
            EXPECT_PX(col, 2 * line + 1, want);
        }
    }

    /* One of those by hand. noise(8, 0, 0) = 0x3ED: red 0x2D = 45, green
     * (0x3ED >> 6) & 0x3F = 15, blue 5. Colour 8: isqrt(512 + 45) = 23,
     * isqrt(512 + 15) = 22, isqrt(512 + 5) = 22: 46, 44, 44. */
    begin("gamma with gamma dither, by hand", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3) | GAMMA | GAMMA_DITHER;
    put16(8, 0, 1, 1, 1, 7);
    /* And where the table's index must be exact: noise(9, 0, 0) = 0x846B,
     * red 0x2B = 43, green (0x846B >> 6) & 0x3F = 17, blue 3. Colour 8:
     * 512 + 17 = 529 is 23 squared, so green is 46 where an index one lower
     * would give 44; red isqrt(555) = 23, blue isqrt(515) = 22. */
    put16(9, 0, 1, 1, 1, 7);
    render();
    EXPECT_PX(8, 0, 0x2E2C2C);
    EXPECT_PX(9, 0, 0x2E2E2C);

    /* Gamma dither without gamma adds one bit of noise a channel
     * (noise_get_partial_gamma_dither: red bit 0, green bit 1, blue bit 2),
     * stopping at 255. noise(8, 0, 0) = 0x3ED = ...101: red and blue;
     * noise(9, 0, 0) = 0x846B = ...011: red and green. */
    begin("gamma dither alone", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3) | GAMMA_DITHER;
    put16(8, 0, 16, 16, 16, 7);
    put16(9, 0, 16, 16, 16, 7);
    render();
    EXPECT_PX(8, 0, 0x818081);
    EXPECT_PX(8, 1, 0x818081);
    EXPECT_PX(9, 0, 0x818180);
    {
        /* The whole visible part of a line, and the next frame's noise. */
        uint32_t first[65];
        int changed = 0;
        for (int col = 8; col <= 72; col++) {
            const uint32_t s = noise((uint32_t)col, 3, 0);
            const uint32_t want = (s & 1u) << 16 | ((s >> 1) & 1u) << 8 | ((s >> 2) & 1u);
            EXPECT_PX(col, 6, want); /* line 3 is black */
            first[col - 8] = out(col, 6);
        }
        m->frame_count = 1;
        render();
        for (int col = 8; col <= 72; col++) {
            const uint32_t s = noise((uint32_t)col, 3, 1);
            EXPECT_PX(col, 6, (s & 1u) << 16 | ((s >> 1) & 1u) << 8 | ((s >> 2) & 1u));
            changed += out(col, 6) != first[col - 8];
        }
        CHECK(changed > 10, "only %d of 65 pixels changed with the frame", changed);
        m->frame_count = 0;
        render();
        for (int col = 8; col <= 72; col++)
            CHECK(out(col, 6) == first[col - 8], "column %d is not the same for the same frame", col);
    }

    /* Across the whole picture, on both sides of columns 256 and 512
     * (the noise's first round takes x >> 8): a black line 5. */
    begin("gamma dither across the picture", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3) | GAMMA_DITHER;
    m->vi_regs[R_H_VIDEO] = hv(108, 748);
    m->frame_count = 0x1234;
    render();
    {
        static const int cols[] = { 8, 255, 256, 257, 300, 511, 512, 513, 639 };
        for (size_t i = 0; i < sizeof cols / sizeof cols[0]; i++) {
            const uint32_t s = noise((uint32_t)cols[i], 5, 0x1234);
            EXPECT_PX(cols[i], 10, (s & 1u) << 16 | ((s >> 1) & 1u) << 8 | ((s >> 2) & 1u));
        }
    }

    begin("gamma dither alone stops at 255", TYPE32);
    m->vi_regs[R_CONTROL] = TYPE32 | AA_MODE(3) | GAMMA_DITHER;
    put32(8, 0, 255, 255, 254, 7); /* red and blue get a bit: 255, 255, 255 */
    put32(9, 0, 254, 255, 255, 7); /* red and green: 255, 255, 255 */
    render();
    EXPECT_PX(8, 0, 0xFFFFFF);
    EXPECT_PX(9, 0, 0xFFFFFF);

    /* The noise's column counts from H_START: with H_START 8 columns into
     * the picture, column 16 is the first drawn and its noise is
     * noise(8, 0, 0) again. */
    begin("gamma dither counts from H_START", TYPE16);
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3) | GAMMA_DITHER;
    m->vi_regs[R_H_VIDEO] = hv(116, 116 + PITCH);
    put16(8, 0, 16, 16, 16, 7);
    put16(7, 0, 31, 31, 31, 7);
    render();
    EXPECT_PX(15, 0, 0x000000);
    EXPECT_PX(16, 0, 0x818081);
}

static void test_serrate(void)
{
    /* Interlace: a field draws every other row of the frame and leaves
     * the others as the last field drew them. Field 0 has the odd rows.
     * Row y of the framebuffer is red 8 * (y + 1); Y_SCALE 0x800 steps two
     * rows a field line, as a 480-line framebuffer is shown. */
    begin("serrate", TYPE16);
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < PITCH; x++) put16(x, y, (unsigned)y + 1u, 0, 0, 7);
    }
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3) | SERRATE;
    m->vi_regs[R_V_SYNC] = 524;
    m->vi_regs[R_Y_SCALE] = 0x800;
    m->vi_regs[R_V_VIDEO] = hv(35, 35 + 2 * ROWS);
    m->vi_field = 0;
    render();
    EXPECT_PX(8, 0, MARKER);
    EXPECT_PX(8, 1, 0x080000); /* row 0 */
    EXPECT_PX(8, 2, MARKER);
    EXPECT_PX(8, 3, 0x180000); /* row 2 */
    EXPECT_PX(8, 5, 0x280000); /* row 4 */
    EXPECT_PX(8, 29, 0xE80000); /* row 28 */
    EXPECT_PX(8, 47, 0x000000); /* row 46, black: the last line of the field */
    EXPECT_PX(8, 49, MARKER);

    /* The other field, a row further down the framebuffer, into the even
     * rows; the odd ones still hold field 0. */
    m->vi_regs[R_V_VIDEO] = hv(34, 34 + 2 * ROWS);
    m->vi_regs[R_Y_SCALE] = 0x400u << 16 | 0x800u;
    m->vi_field = 1;
    n64_vi_render(m);
    EXPECT_PX(8, 0, 0x100000); /* row 1 */
    EXPECT_PX(8, 1, 0x080000);
    EXPECT_PX(8, 2, 0x200000); /* row 3 */
    EXPECT_PX(8, 3, 0x180000);
    EXPECT_PX(8, 28, 0xF00000); /* row 29 */
    EXPECT_PX(8, 29, 0xE80000);

    /* Interpolation between the two rows a field line covers: Y_OFFSET
     * 0x200 is frac 16. Field line 0: rows 0 and 1, (8 * 16 + 16 * 16 + 16)
     * >> 5 = 12; field line 1: rows 2 and 3, (24 + 32) * 16 + 16 >> 5 = 28. */
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(2) | SERRATE;
    m->vi_regs[R_V_VIDEO] = hv(35, 35 + 2 * ROWS);
    m->vi_regs[R_Y_SCALE] = 0x200u << 16 | 0x800u;
    m->vi_field = 0;
    render();
    EXPECT_PX(8, 1, 0x0C0000);
    EXPECT_PX(8, 3, 0x1C0000);
    EXPECT_PX(8, 2, MARKER);

    /* Serrate with an odd V_SYNC: the field never alternates, and the rows
     * it skips are filled from the ones it drew. */
    m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(3) | SERRATE;
    m->vi_regs[R_V_SYNC] = 525;
    m->vi_regs[R_Y_SCALE] = 0x800;
    render();
    EXPECT_PX(8, 0, MARKER);
    EXPECT_PX(8, 1, 0x080000);
    EXPECT_PX(8, 2, 0x080000);
    EXPECT_PX(8, 3, 0x180000);
    EXPECT_PX(8, 4, 0x180000);
}

static void test_window(void)
{
    /* An odd V_START (37, as libultra's 240-line modes set): the first
     * framebuffer row is on the frame's rows 3 and 4, as in the unfiltered
     * scan-out (parallel-rdp would put it on rows 2 and 3). */
    for (int raw = 0; raw < 2; raw++) {
        begin(raw ? "odd V_START, raw" : "odd V_START", TYPE16);
        for (int y = 0; y < 30; y++) {
            for (int x = 0; x < PITCH; x++) put16(x, y, 0, (unsigned)y + 1u, 0, 7);
        }
        m->vi_raw = raw != 0;
        m->vi_regs[R_V_VIDEO] = hv(37, 37 + 2 * ROWS);
        render();
        EXPECT_PX(8, 2, 0x000000);
        EXPECT_PX(8, 3, 0x000800);
        EXPECT_PX(8, 4, 0x000800);
        EXPECT_PX(8, 5, 0x001000);
        EXPECT_PX(8, 6, 0x001000);
        EXPECT_PX(8, 50, raw ? 0x00C600 : 0x00C000); /* row 23: 24 -> 192, or 198 widened */
        EXPECT_PX(8, 51, 0x000000);
    }

    /* V_START above the picture: the rows before it are skipped, two
     * frame rows to a framebuffer row from V_START on. H_START left of the
     * picture: the columns before it are skipped (and the guard band is
     * still eight columns). */
    begin("V_START and H_START before the picture", TYPE16);
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < PITCH; x++) put16(x, y, (unsigned)x & 31u, (unsigned)y + 1u, 0, 7);
    }
    m->vi_regs[R_V_VIDEO] = hv(30, 30 + 2 * ROWS);
    m->vi_regs[R_H_VIDEO] = hv(100, 100 + PITCH);
    render();
    EXPECT_PX(7, 0, 0x000000);
    EXPECT_PX(8, 0, 0x801800);  /* pixel (16, 2) */
    EXPECT_PX(8, 1, 0x801800);
    EXPECT_PX(9, 0, 0x881800);  /* (17, 2) */
    EXPECT_PX(8, 2, 0x802000);  /* (16, 3) */
    EXPECT_PX(64, 0, 0x401800); /* (72, 2): H_END 180 less the guard band of 7 is column 64 */
    EXPECT_PX(65, 0, 0x000000);
    EXPECT_PX(8, 43, 0x80C000); /* (16, 23): the last line, V_END 78 */
    EXPECT_PX(8, 44, 0x000000);

    /* PAL: the picture starts at H 128, V 44 and is 576 rows; H_END at the
     * picture's edge has no guard band; the bottom row is line 287. */
    begin("PAL", TYPE16);
    m->cart.pal = true;
    m->vi_regs[R_V_SYNC] = 625;
    m->vi_regs[R_H_VIDEO] = hv(128, 768);
    m->vi_regs[R_V_VIDEO] = hv(44, 620);
    put16(8, 0, 31, 0, 0, 7);
    put16(639 - 7 * PITCH, 7, 0, 0, 31, 7); /* column 639 of row 0, in a framebuffer 80 wide */
    put16(8, 287, 0, 31, 0, 7);
    render();
    CHECK(m->out_width == 640 && m->out_height == 576, "frame is %d x %d", m->out_width, m->out_height);
    EXPECT_PX(8, 0, 0xF80000);
    EXPECT_PX(639, 0, 0x0000F8);
    EXPECT_PX(8, 574, 0x00F800);
    EXPECT_PX(8, 575, 0x00F800);
    EXPECT_PX(8, 573, 0x000000);

    /* V_END before V_START draws to the bottom of the picture. */
    begin("V_END before V_START", TYPE16);
    m->vi_regs[R_V_VIDEO] = hv(34, 20);
    put16(8, 239, 31, 31, 0, 7);
    render();
    EXPECT_PX(8, 478, 0xF8F800);
    EXPECT_PX(8, 479, 0xF8F800);
}

static void test_modes(void)
{
    /* The four anti-aliasing modes on one scene: P (5, 0) = (248, 128, 8)
     * with coverage 3 on black, X_SCALE 0x200.
     * Anti-aliased (modes 0 and 1): second_lo 0, second_hi P, offset -P,
     * P + ((-P * 4 + 4) >> 3): 248 - 124, 128 - 64, 8 - 4 = (124, 64, 4);
     * column 9, half way from black: (62, 32, 2).
     * Mode 2 interpolates P as it is; mode 3 shows the pixel at x >> 10. */
    static const uint32_t at9[4] = { 0x3E2002, 0x3E2002, 0x7C4004, 0x000000 };
    static const uint32_t at10[4] = { 0x7C4004, 0x7C4004, 0xF88008, 0xF88008 };
    for (unsigned mode = 0; mode < 4; mode++) {
        begin("anti-aliasing mode", TYPE16);
        m->vi_regs[R_CONTROL] = TYPE16 | AA_MODE(mode);
        m->vi_regs[R_X_SCALE] = 0x200;
        put16(5, 0, 31, 16, 1, 3);
        render();
        EXPECT_PX(9, 0, at9[mode]);
        EXPECT_PX(10, 0, at10[mode]);
    }
}

/* ---- parallel-rdp's shaders, transcribed ----
 *
 * One fragment at a time, in the shaders' own terms: uvec4 pixels,
 * unsigned arithmetic that wraps, and the stages as functions of a
 * framebuffer position. model_render() puts the result where
 * n64_vi_render() must: the window is ares's (see n64_vi.c), which the
 * shaders do not decide. */

typedef struct { uint32_t c[3], a; } Px;

static struct {
    uint32_t status, origin, width;
    bool fetch_aa, scale_aa, fetch_bug;
} sh;

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static int clamp1(int v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

/* extract_vram.comp's fetch_color. Memory past RDRAM reads as zero. */
static Px sh_fetch_color(int x, int y)
{
    Px color;
    memset(&color, 0, sizeof color);
    if ((sh.status & 3u) == TYPE32) {
        const uint32_t linear = ((uint32_t)(y * (int)sh.width + x) + (sh.origin >> 2)) & 0x3FFFFFu;
        const uint32_t word = linear < m->rdram_size / 4u ? n64_be32(m->rdram + 4u * (size_t)linear) : 0u;
        color.c[0] = (word >> 24) & 0xFFu;
        color.c[1] = (word >> 16) & 0xFFu;
        color.c[2] = (word >> 8) & 0xFFu;
        color.a = (word >> 5) & 7u;
    } else {
        const uint32_t linear = ((uint32_t)(y * (int)sh.width + x) + (sh.origin >> 1)) & 0x7FFFFFu;
        const bool inside = linear < m->rdram_size / 2u;
        const uint32_t word = inside ? n64_be16(m->rdram + 2u * (size_t)linear) : 0u;
        const uint32_t hidden_word = inside ? m->rdp.links.hidden[linear] : 0u;
        color.c[0] = (word >> 8) & 0xF8u;
        color.c[1] = (word >> 3) & 0xF8u;
        color.c[2] = (word << 2) & 0xF8u;
        color.a = ((word & 1u) << 2) | hidden_word;
    }
    if (!sh.fetch_aa) color.a = 7u;
    return color;
}

/* ivec3(fetch_color_offset(...).rgb >> 3u), one channel. */
static int sh_fetch_5bit(int x, int y, int channel)
{
    const Px color = sh_fetch_color(x, y);
    return (int)(color.c[channel] >> 3);
}

static void sh_check_neighbor(Px candidate, uint32_t *lo, uint32_t *hi, uint32_t *second_lo, uint32_t *second_hi)
{
    if (candidate.a != 7u) return;
    for (int i = 0; i < 3; i++) {
        second_lo[i] = umin(second_lo[i], umax(candidate.c[i], lo[i]));
        second_hi[i] = umax(second_hi[i], umin(candidate.c[i], hi[i]));
        lo[i] = umin(candidate.c[i], lo[i]);
        hi[i] = umax(candidate.c[i], hi[i]);
    }
}

/* vi_fetch.frag: FragColor, or FragColorFetchBug. */
static Px sh_vi_fetch(int x, int y, bool bug_output)
{
    const Px mid_pixel = sh_fetch_color(x, y);
    Px result = mid_pixel;
    if (mid_pixel.a != 7u) {
        uint32_t lo[3], hi[3], second_lo[3], second_hi[3], lo_bug[3], hi_bug[3], second_lo_bug[3], second_hi_bug[3];
        for (int i = 0; i < 3; i++) lo[i] = hi[i] = second_lo[i] = second_hi[i] = mid_pixel.c[i];
        const Px left_up = sh_fetch_color(x - 1, y - 1), right_up = sh_fetch_color(x + 1, y - 1);
        const Px to_left = sh_fetch_color(x - 2, y), to_right = sh_fetch_color(x + 2, y);
        const Px left_down = sh_fetch_color(x - 1, y + 1), right_down = sh_fetch_color(x + 1, y + 1);
        sh_check_neighbor(left_up, lo, hi, second_lo, second_hi);
        sh_check_neighbor(right_up, lo, hi, second_lo, second_hi);
        sh_check_neighbor(to_left, lo, hi, second_lo, second_hi);
        sh_check_neighbor(to_right, lo, hi, second_lo, second_hi);
        memcpy(lo_bug, lo, sizeof lo);
        memcpy(hi_bug, hi, sizeof hi);
        memcpy(second_lo_bug, second_lo, sizeof lo);
        memcpy(second_hi_bug, second_hi, sizeof hi);
        sh_check_neighbor(left_down, lo, hi, second_lo, second_hi);
        sh_check_neighbor(right_down, lo, hi, second_lo, second_hi);
        if (sh.fetch_bug) {
            sh_check_neighbor(to_left, lo_bug, hi_bug, second_lo_bug, second_hi_bug);
            sh_check_neighbor(to_right, lo_bug, hi_bug, second_lo_bug, second_hi_bug);
            for (int i = 0; i < 3; i++) {
                if (mid_pixel.c[i] == lo[i]) second_lo[i] = lo[i];
                if (mid_pixel.c[i] == hi[i]) second_hi[i] = hi[i];
                if (mid_pixel.c[i] == lo_bug[i]) second_lo_bug[i] = lo_bug[i];
                if (mid_pixel.c[i] == hi_bug[i]) second_hi_bug[i] = hi_bug[i];
            }
        }
        const uint32_t coeff = 7u - mid_pixel.a;
        for (int i = 0; i < 3; i++) {
            const uint32_t offset = bug_output ? second_lo_bug[i] + second_hi_bug[i] - (mid_pixel.c[i] << 1)
                                               : second_lo[i] + second_hi[i] - (mid_pixel.c[i] << 1);
            result.c[i] = (mid_pixel.c[i] + ((offset * coeff + 4u) >> 3)) & 0xFFu;
        }
    } else if (sh.status & DITHER_FILTER) {
        for (int i = 0; i < 3; i++) {
            const int tmp_color = (int)(mid_pixel.c[i] >> 3);
            int tmp_accum = 0;
            for (int dy = -1; dy <= 0; dy++) {
                for (int dx = -1; dx <= 1; dx++) tmp_accum += clamp1(sh_fetch_5bit(x + dx, y + dy, i) - tmp_color);
            }
            if (bug_output) {
                tmp_accum += clamp1(sh_fetch_5bit(x - 1, y, i) - tmp_color);
                tmp_accum += clamp1(sh_fetch_5bit(x + 1, y, i) - tmp_color);
            } else {
                tmp_accum += clamp1(sh_fetch_5bit(x - 1, y + 1, i) - tmp_color);
                tmp_accum += clamp1(sh_fetch_5bit(x + 1, y + 1, i) - tmp_color);
                tmp_accum += clamp1(sh_fetch_5bit(x, y + 1, i) - tmp_color);
            }
            result.c[i] = (mid_pixel.c[i] & 0xF8u) + (uint32_t)tmp_accum;
        }
    }
    return result;
}

static uint32_t sh_median3(uint32_t left, uint32_t center, uint32_t right)
{
    uint32_t t;
    if (left < center) { t = left; left = center; center = t; }
    if (center < right) { t = center; center = right; right = t; }
    if (left < center) { t = left; left = center; center = t; }
    return center;
}

/* vi_divot.frag (its texel x is the framebuffer's x, the fetch stage's
 * image starting one column to the left), or the fetch stage alone when
 * the divot filter is off. */
static Px sh_vi_divot(int x, int y, bool bug_output)
{
    if (!(sh.status & DIVOT)) return sh_vi_fetch(x, y, bug_output);
    const Px left = sh_vi_fetch(x - 1, y, bug_output), mid = sh_vi_fetch(x, y, bug_output);
    const Px right = sh_vi_fetch(x + 1, y, bug_output);
    if ((left.a & mid.a & right.a) == 7u) return mid;
    Px result = mid;
    for (int i = 0; i < 3; i++) result.c[i] = sh_median3(left.c[i], mid.c[i], right.c[i]);
    return result;
}

static uint32_t sh_vi_lerp(uint32_t a, uint32_t b, uint32_t l) { return (a + (((b - a) * l + 16u) >> 5)) & 0xFFu; }

/* vi_scale.frag for the fragment at column coord_x from where the line's
 * sampling starts, on progressive line coord_y. */
static uint32_t sh_vi_scale(int coord_x, int coord_y, int x_start, int x_add, int y_start, int y_add, uint32_t frame)
{
    const int x = coord_x * x_add + x_start, y = coord_y * y_add + y_start;
    const int base_x = x >> 10, base_y = y >> 10;
    Px c00 = sh_vi_divot(base_x, base_y, false);
    bool bug_offset = false;
    if (sh.fetch_bug && coord_y != 0) {
        const int prev_y = (y - y_add) >> 10, next_y = (y + y_add) >> 10;
        if (base_y == prev_y && base_y != next_y) bug_offset = true;
    }
    if (sh.scale_aa) {
        const uint32_t x_frac = (uint32_t)(x >> 5) & 31u, y_frac = (uint32_t)(y >> 5) & 31u;
        Px c10 = sh_vi_divot(base_x + 1, base_y, false);
        const Px c01 = sh_vi_divot(base_x, base_y + 1, bug_offset);
        const Px c11 = sh_vi_divot(base_x + 1, base_y + 1, bug_offset);
        for (int i = 0; i < 3; i++) {
            c00.c[i] = sh_vi_lerp(c00.c[i], c01.c[i], y_frac);
            c10.c[i] = sh_vi_lerp(c10.c[i], c11.c[i], y_frac);
            c00.c[i] = sh_vi_lerp(c00.c[i], c10.c[i], x_frac);
        }
    }
    const uint32_t seed = noise((uint32_t)coord_x, (uint32_t)coord_y, frame);
    if (sh.status & GAMMA) {
        if (sh.status & GAMMA_DITHER) {
            const uint32_t n[3] = { seed & 0x3Fu, (seed >> 6) & 0x3Fu, ((seed >> 9) & 0x38u) | (seed & 7u) };
            for (int i = 0; i < 3; i++) c00.c[i] = gamma_dithered(c00.c[i], n[i]);
        } else {
            for (int i = 0; i < 3; i++) c00.c[i] = gamma_plain(c00.c[i]);
        }
    } else if (sh.status & GAMMA_DITHER) {
        for (int i = 0; i < 3; i++) c00.c[i] = umin(c00.c[i] + ((seed >> i) & 1u), 0xFFu);
    }
    return c00.c[0] << 16 | c00.c[1] << 8 | c00.c[2];
}

/* What n64_vi_render must leave in `frame`, which holds what the frame
 * held before. */
static void model_render(uint32_t *frame)
{
    const uint32_t *io = m->vi_regs;
    const bool pal = m->cart.pal;
    const int hscan_start = pal ? 128 : 108, vscan_start = pal ? 44 : 34;
    const int hscan_stop = hscan_start + 640, vscan_stop = vscan_start + (pal ? 576 : 480);
    const bool serrate = (io[R_CONTROL] & SERRATE) != 0;
    const int hstart = (int)((io[R_H_VIDEO] >> 16) & 0x3FFu), hend = (int)(io[R_H_VIDEO] & 0x3FFu);
    const int vstart = (int)((io[R_V_VIDEO] >> 16) & 0x3FFu), vend = (int)(io[R_V_VIDEO] & 0x3FFu);
    if (!serrate) memset(frame, 0, sizeof(uint32_t) * N64_FB_WIDTH * N64_FB_HEIGHT);
    if ((io[R_CONTROL] & 3u) < 2u) return;

    /* The window (ares). */
    int dy0 = vstart, dy1 = vend < vstart ? vscan_stop : vend, dx0 = hstart, dx1 = hend;
    if (dy0 < vscan_start) dy0 = vscan_start;
    if (dy1 > vscan_stop) dy1 = vscan_stop;
    if (dx0 < hscan_start) dx0 = hscan_start;
    if (dx1 > hscan_stop) dx1 = hscan_stop;
    dx0 += 8;
    if (dx1 < hscan_stop) dx1 -= 7;

    /* The sampling (decode_vi_registers and scanout). */
    const uint32_t aa_mode = (io[R_CONTROL] >> 8) & 3u;
    int x_start = (int)((io[R_X_SCALE] >> 16) & 0xFFFu), y_start = (int)((io[R_Y_SCALE] >> 16) & 0xFFFu);
    const int x_add = (int)(io[R_X_SCALE] & 0xFFFu), y_add = (int)(io[R_Y_SCALE] & 0xFFFu);
    int h0 = hstart, line0 = 0;
    if (h0 < hscan_start) {
        x_start += x_add * (hscan_start - h0);
        h0 = hscan_start;
    }
    if (vstart < vscan_start) {
        line0 = (vscan_start - vstart) / 2;
        y_start += y_add * line0;
    }
    sh.status = io[R_CONTROL];
    sh.origin = io[R_ORIGIN] & 0xFFFFFFu;
    sh.width = io[R_WIDTH] & 0xFFFu;
    sh.fetch_aa = aa_mode < 2u;
    sh.scale_aa = aa_mode < 3u;
    sh.fetch_bug = y_add < 1024;

    for (int dy = dy0; dy < dy1; dy++) {
        if (serrate && (dy & 1) != (int)!m->vi_field) continue;
        const int line = (dy - vstart) / 2 - line0;
        for (int dx = dx0; dx < dx1; dx++) {
            frame[(size_t)(dy - vscan_start) * N64_FB_WIDTH + (size_t)(dx - hscan_start)] =
                sh_vi_scale(dx - h0, line, x_start, x_add, y_start, y_add, (uint32_t)m->frame_count);
        }
    }
    if (serrate && (io[R_V_SYNC] & 1u)) {
        for (int dy = dy0; dy < dy1; dy++) {
            if ((dy & 1) == (int)!m->vi_field) continue;
            const int src = dy - 1 >= dy0 ? dy - 1 : dy + 1;
            if (src < dy1)
                memcpy(frame + (size_t)(dy - vscan_start) * N64_FB_WIDTH,
                       frame + (size_t)(src - vscan_start) * N64_FB_WIDTH, sizeof(uint32_t) * N64_FB_WIDTH);
        }
    }
}

static uint32_t rng_state;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}
static uint32_t pick(const uint32_t *list, size_t n) { return list[rng() % n]; }

static void test_against_shaders(void)
{
    static uint32_t want[N64_FB_WIDTH * N64_FB_HEIGHT];
    static const uint32_t x_scales[] = { 0x400, 0x200, 0x200, 0x155, 0x300, 0x333, 0x800, 0x280, 0x3FF, 0xFFF, 0x001, 0x000 };
    static const uint32_t y_scales[] = { 0x400, 0x400, 0x200, 0x333, 0x3C0, 0x800, 0x600, 0x3FF, 0x401, 0xFFF, 0x001, 0x000 };
    static const uint32_t widths[] = { 320, 320, 640, 80, 33, 1, 0 };
    static char name[160];
    const uint32_t base = 0x200000u, span = 0x100000u;

    for (int scenario = 0; scenario < 72; scenario++) {
        rng_state = 0x1234567u + (uint32_t)scenario * 7919u;
        /* A framebuffer of noise in which most pixels are fully covered
         * (so that all three of anti-aliasing, the dither filter and the
         * divot filter have work), or almost none, or all. */
        const uint32_t full = scenario % 3 == 0 ? 2u : scenario % 3 == 1 ? 13u : 16u; /* of 16 */
        const uint32_t type = scenario % 4 == 3 ? TYPE32 : TYPE16;
        for (uint32_t a = base; a < base + span; a += 4) {
            uint32_t word = rng() << 8 ^ rng();
            /* A smoother picture than white noise: dithered-looking 5-bit steps. */
            if (scenario & 1) word &= 0x18C718C7u | 0x00010001u;
            const bool covered = rng() % 16u < full;
            if (type == TYPE32) {
                word = covered ? word | 0xE0u : word;
            } else if (covered) {
                word |= 0x00010001u;
            }
            n64_put_be32(m->rdram + a, word);
            m->rdp.links.hidden[a >> 1] = (uint8_t)(covered ? 3u : rng() & 3u);
            m->rdp.links.hidden[(a >> 1) + 1] = (uint8_t)(covered ? 3u : rng() & 3u);
        }

        memset(m->vi_regs, 0, sizeof m->vi_regs);
        uint32_t ctrl = type | AA_MODE(rng() & 3u);
        if (rng() & 1u) ctrl |= GAMMA_DITHER;
        if (rng() & 1u) ctrl |= GAMMA;
        if (rng() & 1u) ctrl |= DIVOT;
        if (rng() & 1u) ctrl |= DITHER_FILTER;
        if (rng() % 4u == 0) ctrl |= SERRATE;
        if (scenario < 4) ctrl = type | AA_MODE(0) | GAMMA_DITHER | DIVOT | DITHER_FILTER; /* the usual settings */
        m->cart.pal = rng() % 4u == 0;
        const uint32_t h0 = m->cart.pal ? 128u : 108u, v0 = m->cart.pal ? 44u : 34u;
        /* A window of at most 120 x 60, placed anywhere, sometimes off the
         * picture's edges; the first scenarios are the whole picture. */
        uint32_t hstart = h0 - 20u + rng() % 600u, vstart = v0 - 10u + rng() % 480u;
        uint32_t hend = hstart + 20u + rng() % 100u, vend = vstart + 2u + rng() % 60u;
        if (scenario < 4) {
            hstart = h0;
            hend = h0 + 640u;
            vstart = v0 + (scenario & 1u ? 3u : 0u);
            vend = v0 + (m->cart.pal ? 576u : 480u) - (scenario & 1u ? 3u : 0u);
        } else if (scenario % 9 == 0) {
            vend = rng() % vstart; /* V_END before V_START */
            vstart = v0 + (m->cart.pal ? 520u : 430u) + rng() % 40u;
        }
        m->vi_regs[R_CONTROL] = ctrl;
        m->vi_regs[R_WIDTH] = pick(widths, sizeof widths / sizeof widths[0]);
        m->vi_regs[R_ORIGIN] = base + 0x40000u + rng() % 0x1000u;
        /* The ends of memory: rows that run off RDRAM, or start before 0. */
        if (scenario % 12 == 5) m->vi_regs[R_ORIGIN] = m->rdram_size - 0x300u + rng() % 0x100u;
        if (scenario % 12 == 7) m->vi_regs[R_ORIGIN] = rng() % 0x400u;
        if (scenario % 12 == 9) m->vi_regs[R_ORIGIN] = 0xFFFE00u + rng() % 0x200u;
        m->vi_regs[R_V_SYNC] = (m->cart.pal ? 624u : 524u) + (rng() & 1u);
        m->vi_regs[R_H_VIDEO] = hv(hstart & 0x3FFu, hend & 0x3FFu);
        m->vi_regs[R_V_VIDEO] = hv(vstart & 0x3FFu, vend & 0x3FFu);
        m->vi_regs[R_X_SCALE] = (rng() & 1u ? rng() & 0xFFFu : 0u) << 16 | pick(x_scales, sizeof x_scales / sizeof x_scales[0]);
        m->vi_regs[R_Y_SCALE] = (rng() & 1u ? rng() & 0xFFFu : 0u) << 16 | pick(y_scales, sizeof y_scales / sizeof y_scales[0]);
        if (scenario < 4) {
            m->vi_regs[R_WIDTH] = scenario < 2 ? 320u : 640u;
            m->vi_regs[R_ORIGIN] = base + 0x1000u;
            m->vi_regs[R_X_SCALE] = scenario < 2 ? 0x200u : 0x400u;
            m->vi_regs[R_Y_SCALE] = scenario < 2 ? 0x400u : 0x02000800u;
            if (scenario >= 2) m->vi_regs[R_CONTROL] |= SERRATE;
            m->vi_regs[R_V_SYNC] = scenario >= 2 ? 524u : 525u;
        }
        m->vi_field = rng() & 1u;
        m->frame_count = rng();
        m->vi_raw = false;

        snprintf(name, sizeof name, "shaders, scenario %d (ctrl %05X, x %08X, y %08X, h %08X, v %08X, width %u)", scenario,
                 (unsigned)m->vi_regs[R_CONTROL], (unsigned)m->vi_regs[R_X_SCALE], (unsigned)m->vi_regs[R_Y_SCALE],
                 (unsigned)m->vi_regs[R_H_VIDEO], (unsigned)m->vi_regs[R_V_VIDEO], (unsigned)m->vi_regs[R_WIDTH]);
        current_test = name;
        for (size_t i = 0; i < (size_t)N64_FB_WIDTH * N64_FB_HEIGHT; i++) m->framebuffer[i] = want[i] = MARKER ^ (uint32_t)i;
        n64_vi_render(m);
        model_render(want);
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < (size_t)N64_FB_WIDTH * N64_FB_HEIGHT; i++) {
            if (m->framebuffer[i] != want[i] && !bad++) first = i;
        }
        CHECK(bad == 0, "%u pixels differ; the first is (%u, %u): %06X, expected %06X", (unsigned)bad,
              (unsigned)(first % N64_FB_WIDTH), (unsigned)(first / N64_FB_WIDTH), (unsigned)m->framebuffer[first],
              (unsigned)want[first]);

        /* The same state renders the same frame. */
        if (scenario % 8 == 0 && !(m->vi_regs[R_CONTROL] & SERRATE)) {
            n64_vi_render(m);
            CHECK(memcmp(m->framebuffer, want, sizeof want) == 0, "a second render differs");
        }
    }
}

/* ---- the larger picture (n64_set_internal_scale) ---- */

/* A 320 x 240 picture at 0x201000, doubled across as most games show it. */
static void larger_mode(uint32_t ctrl)
{
    memset(m->vi_regs, 0, sizeof m->vi_regs);
    m->vi_regs[R_CONTROL] = ctrl;
    m->vi_regs[R_ORIGIN] = 0x201000u;
    m->vi_regs[R_WIDTH] = 320;
    m->vi_regs[R_V_SYNC] = 525;
    m->vi_regs[R_H_VIDEO] = 0x006C02ECu;
    m->vi_regs[R_V_VIDEO] = 0x002501FFu;
    m->vi_regs[R_X_SCALE] = 0x200;
    m->vi_regs[R_Y_SCALE] = 0x400;
    m->cart.pal = false;
    m->vi_field = 0;
    m->frame_count = 7;
    m->vi_raw = false;
}

/* The first column of a row that is not black, or -1. */
static int first_lit(const uint32_t *row, int width)
{
    for (int x = 0; x < width; x++)
        if (row[x]) return x;
    return -1;
}

/* How many times the first lit column changes from one row to the next. */
static int edge_steps(const uint32_t *frame, int width, int height)
{
    int steps = 0, last = -1;
    for (int y = 0; y < height; y++) {
        const int at = first_lit(frame + (size_t)y * (size_t)width, width);
        if (at < 0) continue;
        if (last >= 0 && at != last) steps++;
        last = at;
    }
    return steps;
}

/* The larger picture's scan-out. At an internal scale the console's own
 * frame is what it is without one. Pixels the game wrote have no larger
 * picture, so with no filter on they are the console's frame with every
 * pixel repeated; what the RDP drew is shown at the finer grain; and the
 * frame does not depend on how many threads draw it. */
static void test_larger(void)
{
    enum { W = 640, H = 480 };
    static uint32_t console[N64_FB_WIDTH * N64_FB_HEIGHT];
    static uint32_t kept[(size_t)W * 4 * H * 4];
    const size_t frame_bytes = sizeof m->framebuffer;
    int w = 0, h = 0;

    current_test = "larger: none at a scale of 1";
    CHECK(n64_internal_scale(m) == 1 && !n64_large_frame(m, &w, &h), "a larger frame at a scale of 1");
    CHECK(!n64_set_internal_scale(m, 3) && n64_internal_scale(m) == 1, "3 is not a scale");

    rng_state = 99;
    for (uint32_t a = 0x200000u; a < 0x300000u; a += 2) {
        const bool covered = rng() % 16u != 0;
        n64_put_be16(m->rdram + a, (uint16_t)((rng() & 0xF7BCu) | (covered ? 1u : 0u)));
        m->rdp.links.hidden[a >> 1] = (uint8_t)(covered ? 3u : rng() & 3u);
    }

    for (int filtered = 0; filtered < 2; filtered++) {
        /* No filter; then every one, as games have them. */
        larger_mode(filtered ? 0x00013016u : TYPE16 | AA_MODE(3));
        n64_set_internal_scale(m, 1);
        render();
        memcpy(console, m->framebuffer, frame_bytes);
        for (int scale = 2; scale <= 4; scale += 2) {
            static char name[96];
            const int g = scale / 2;
            snprintf(name, sizeof name, "larger: %dx, %s", scale, filtered ? "filtered" : "no filter");
            current_test = name;
            CHECK(n64_set_internal_scale(m, scale) && n64_internal_scale(m) == scale, "the scale was refused");
            n64_rdp_set_threads(1);
            render();
            CHECK(memcmp(console, m->framebuffer, frame_bytes) == 0, "the console's frame changed");
            const uint32_t *large = n64_large_frame(m, &w, &h);
            CHECK(large && w == W * g && h == H * g, "the larger frame is %d x %d", w, h);
            if (!large || w != W * g || h != H * g) continue;
            if (!filtered) {
                size_t bad = 0;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        bad += large[(size_t)y * (size_t)w + (size_t)x] != console[(size_t)(y / g) * N64_FB_WIDTH + (size_t)(x / g)];
                CHECK(bad == 0, "%u pixels are not the console's", (unsigned)bad);
            }
            memcpy(kept, large, sizeof(uint32_t) * (size_t)w * (size_t)h);
            n64_rdp_set_threads(16);
            render();
            large = n64_large_frame(m, &w, &h);
            CHECK(large && memcmp(kept, large, sizeof(uint32_t) * (size_t)w * (size_t)h) == 0, "16 threads draw another frame");
            CHECK(memcmp(console, m->framebuffer, frame_bytes) == 0, "the console's frame changed with 16 threads");
        }
    }
    n64_rdp_set_threads(0);

    /* A triangle from the RDP whose left edge moves half a pixel a line:
     * 90 steps down the console's 180 lines, four times that at 4x. */
    {
        /* SET_COMBINE's selectors for the primitive colour alone, both cycles. */
        static const unsigned cc[8] = {8, 8, 16, 3, 7, 7, 7, 3};
        const uint64_t combine_hi = (uint64_t)cc[0] << 20 | (uint64_t)cc[2] << 15 | (uint64_t)cc[4] << 12 |
                                    (uint64_t)cc[6] << 9 | (uint64_t)cc[0] << 5 | cc[2];
        const uint64_t combine_lo = (uint64_t)cc[1] << 28 | (uint64_t)cc[1] << 24 | (uint64_t)cc[4] << 21 |
                                    (uint64_t)cc[6] << 18 | (uint64_t)cc[3] << 15 | (uint64_t)cc[5] << 12 |
                                    (uint64_t)cc[7] << 9 | (uint64_t)cc[3] << 6 | (uint64_t)cc[5] << 3 | cc[7];
        const uint64_t list[] = {
            (uint64_t)0x3f << 56 | (uint64_t)2 << 51 | (uint64_t)(320 - 1) << 32 | 0x201000u, /* SET_COLOR_IMAGE, RGBA5551 */
            (uint64_t)0x2d << 56 | (uint64_t)(320 << 2) << 12 | (240 << 2),                   /* SET_SCISSOR */
            (uint64_t)0x2f << 56 | (uint64_t)0xf0 << 32, /* SET_OTHER_MODES: one cycle, no dither */
            (uint64_t)0x3c << 56 | combine_hi << 32 | combine_lo,
            (uint64_t)0x3a << 56 | 0xffffffffu, /* SET_PRIM_COLOR white */
            /* A triangle, major edge on the left: lines 20 to 199, the
             * left edge from x 40 by 0.5 a line, the right edge at 300. */
            (uint64_t)0x08 << 56 | (uint64_t)1 << 55 | (uint64_t)(200 << 2) << 32 | (uint64_t)(200 << 2) << 16 | (20 << 2),
            (uint64_t)(300 << 16) << 32,
            (uint64_t)(40 << 16) << 32 | 0x8000u,
            (uint64_t)(300 << 16) << 32,
        };
        current_test = "larger: an RDP edge";
        memset(m->rdram + 0x200000u, 0, 0x100000u);
        memset(m->rdp.links.hidden + 0x100000u, 3, 0x80000u);
        larger_mode(TYPE16 | AA_MODE(3));
        CHECK(n64_set_internal_scale(m, 4), "the scale was refused");
        n64_rdp_reset(&m->rdp);
        n64_rdp_execute(&m->rdp, list, sizeof list / sizeof list[0]);
        render();
        const uint32_t *large = n64_large_frame(m, &w, &h);
        const int console_steps = edge_steps(m->framebuffer, N64_FB_WIDTH, H);
        const int large_steps = large ? edge_steps(large, w, h) : 0;
        CHECK(console_steps >= 85 && console_steps <= 90, "the console's edge has %d steps", console_steps);
        CHECK(large_steps >= 4 * console_steps - 8 && large_steps <= 4 * console_steps + 8,
              "the larger edge has %d steps to the console's %d", large_steps, console_steps);
    }
    n64_set_internal_scale(m, 1);
}

/* --time: milliseconds a scan-out takes, for a 320 x 240 picture as most
 * games show it (16 bits, every filter on, doubled across) and a 640 x 480
 * interlaced one, filtered and raw. One pixel in sixteen is partly
 * covered, about what a scene's edges come to. The best of several
 * batches, since other programs share the machine. */
static void time_scanout(void)
{
    static const struct { const char *name; uint32_t ctrl, width, x_scale, y_scale, v_video, v_sync; } modes[2] = {
        { "320 x 240", 0x00013016u, 320, 0x200, 0x400, 0x002501FFu, 525 },
        { "640 x 480 interlaced", 0x00013056u, 640, 0x400, 0x02000800u, 0x002301FDu, 524 },
    };
    rng_state = 99;
    for (uint32_t a = 0x200000u; a < 0x300000u; a += 2) {
        const bool covered = rng() % 16u != 0;
        n64_put_be16(m->rdram + a, (uint16_t)((rng() & 0x18C6u) | 0x8420u | (covered ? 1u : 0u)));
        m->rdp.links.hidden[a >> 1] = (uint8_t)(covered ? 3u : rng() & 3u);
    }
    for (int mode = 0; mode < 2; mode++) {
        for (int raw = 0; raw < 2; raw++) {
            memset(m->vi_regs, 0, sizeof m->vi_regs);
            m->vi_regs[R_CONTROL] = modes[mode].ctrl;
            m->vi_regs[R_ORIGIN] = 0x201000u;
            m->vi_regs[R_WIDTH] = modes[mode].width;
            m->vi_regs[R_V_SYNC] = modes[mode].v_sync;
            m->vi_regs[R_H_VIDEO] = 0x006C02ECu;
            m->vi_regs[R_V_VIDEO] = modes[mode].v_video;
            m->vi_regs[R_X_SCALE] = modes[mode].x_scale;
            m->vi_regs[R_Y_SCALE] = modes[mode].y_scale;
            m->cart.pal = false;
            m->vi_raw = raw != 0;
            double best = 1e9;
            for (int batch = 0; batch < 9; batch++) {
                const uint64_t t0 = n64_host_ticks();
                for (int i = 0; i < 100; i++) {
                    m->vi_field = (uint32_t)i & 1u;
                    m->frame_count = (uint64_t)i;
                    n64_vi_render(m);
                }
                const double ms = 1000.0 * (double)(n64_host_ticks() - t0) / (double)n64_host_ticks_per_second() / 100.0;
                if (ms < best) best = ms;
            }
            printf("%-22s %-8s %.3f ms a scan-out\n", modes[mode].name, raw ? "raw" : "filtered", best);
        }
    }
}

int main(int argc, char **argv)
{
    m = n64_create();
    if (!m) {
        printf("out of memory\n");
        return 1;
    }
    memset(m->rdp.links.hidden, 3, m->rdram_size / 2u);
    if (argc > 1 && !strcmp(argv[1], "--time")) {
        time_scanout();
        n64_destroy(m);
        return 0;
    }

    test_fetch();
    test_raw();
    test_aa();
    test_dedither();
    test_divot();
    test_edges();
    test_scale_horizontal();
    test_scale_vertical();
    test_fetch_bug();
    test_gamma();
    test_serrate();
    test_window();
    test_modes();
    test_against_shaders();
    test_larger();

    n64_destroy(m);
    printf("n64_vi: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
