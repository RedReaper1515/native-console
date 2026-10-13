/*
 * The RDP's command interface and reference rasteriser, checked byte for
 * byte in RDRAM.
 *
 * Each test builds a command list, runs it through n64_rdp_execute() (or
 * the DPC registers), and compares RDRAM with values derived by hand from
 * the hardware's rules: edge equations and coverage samples, the
 * attribute interpolators, TMEM layout, texel decoding, the combiner
 * equation (A - B) * C + D, the blender (P * A + M * B) and the depth
 * format. Where a value is not derivable by hand (the noise hash), the
 * test computes it from parallel-rdp's shader math and says so.
 *
 * The last two tests hold the rasteriser's faster paths, its helper
 * threads and its remembered TMEM loads to the pixel-by-pixel path and the
 * full gather, on generated command lists.
 */
#include "n64_rdp.h"
#include "n64_rdp_state.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RDRAM_SIZE 0x800000u
#define FB_ADDR 0x100000u
#define Z_ADDR 0x200000u
#define TEX_ADDR 0x300000u
#define PAL_ADDR 0x310000u
#define CMD_ADDR 0x400000u
#define FB_WIDTH 16u

static uint8_t *rdram;
static uint8_t dmem[4096];
static N64Rdp rdp;
static int failures;
static int checks;
static int interrupts;
static const char *current_test = "";
static char test_name[96]; /* for generated test names */

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

static void on_interrupt(void *user, int level)
{
    (void)user;
    if (level)
        interrupts++;
}

static void begin(const char *name)
{
    current_test = name;
    memset(rdram, 0, RDRAM_SIZE);
    n64_rdp_reset(&rdp);
}

/* RDRAM access. */
static uint16_t rd16(uint32_t a) { return n64_be16(rdram + a); }
static uint32_t rd32(uint32_t a) { return n64_be32(rdram + a); }
static void wr16(uint32_t a, uint16_t v) { n64_put_be16(rdram + a, v); }
static void wr32(uint32_t a, uint32_t v) { n64_put_be32(rdram + a, v); }
static uint16_t px16(unsigned x, unsigned y) { return rd16(FB_ADDR + 2 * (y * FB_WIDTH + x)); }
static uint32_t px32(unsigned x, unsigned y) { return rd32(FB_ADDR + 4 * (y * FB_WIDTH + x)); }
static uint8_t px8(unsigned x, unsigned y) { return rdram[FB_ADDR + y * FB_WIDTH + x]; }

/* Command builders. Coordinates are 10.2 fixed point unless named. */
#define OP(x) ((uint64_t)(x) << 56)
static uint64_t c_color_image(unsigned fmt, unsigned size, unsigned width, uint32_t addr)
{
    return OP(0x3f) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)(width - 1) << 32 | addr;
}
static uint64_t c_tex_image(unsigned fmt, unsigned size, unsigned width, uint32_t addr)
{
    return OP(0x3d) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)(width - 1) << 32 | addr;
}
static uint64_t c_z_image(uint32_t addr) { return OP(0x3e) | addr; }
static uint64_t c_scissor(unsigned xh, unsigned yh, unsigned xl, unsigned yl)
{
    return OP(0x2d) | (uint64_t)xh << 44 | (uint64_t)yh << 32 | (uint64_t)xl << 12 | yl;
}
static uint64_t c_other_modes(uint32_t hi, uint32_t lo) { return OP(0x2f) | (uint64_t)(hi & 0xffffff) << 32 | lo; }
static uint64_t c_fill_color(uint32_t c) { return OP(0x37) | c; }
static uint64_t c_fog_color(uint32_t c) { return OP(0x38) | c; }
static uint64_t c_blend_color(uint32_t c) { return OP(0x39) | c; }
static uint64_t c_prim_color(unsigned min_level, unsigned lod_frac, uint32_t c)
{
    return OP(0x3a) | (uint64_t)min_level << 40 | (uint64_t)lod_frac << 32 | c;
}
static uint64_t c_env_color(uint32_t c) { return OP(0x3b) | c; }
static uint64_t c_prim_depth(unsigned z, unsigned dz) { return OP(0x2e) | (uint64_t)z << 16 | dz; }
static uint64_t c_rect(unsigned op, unsigned xl, unsigned yl, unsigned xh, unsigned yh)
{
    return OP(op) | (uint64_t)xl << 44 | (uint64_t)yl << 32 | (uint64_t)xh << 12 | yh;
}
static uint64_t c_fill_rect(unsigned xl, unsigned yl, unsigned xh, unsigned yh) { return c_rect(0x36, xl, yl, xh, yh); }
static uint64_t c_tile(unsigned fmt, unsigned size, unsigned line, unsigned tmem, unsigned tile, unsigned palette,
                       unsigned cmt_t, unsigned mask_t, unsigned shift_t, unsigned cms_s, unsigned mask_s,
                       unsigned shift_s)
{
    return OP(0x35) | (uint64_t)fmt << 53 | (uint64_t)size << 51 | (uint64_t)line << 41 | (uint64_t)tmem << 32 |
           (uint64_t)tile << 24 | (uint64_t)palette << 20 | (uint64_t)cmt_t << 18 | (uint64_t)mask_t << 14 |
           (uint64_t)shift_t << 10 | (uint64_t)cms_s << 8 | (uint64_t)mask_s << 4 | shift_s;
}
static uint64_t c_simple_tile(unsigned fmt, unsigned size, unsigned line, unsigned tmem, unsigned tile)
{
    return c_tile(fmt, size, line, tmem, tile, 0, 0, 0, 0, 0, 0, 0);
}
static uint64_t c_tile_op(unsigned op, unsigned tile, unsigned sl, unsigned tl, unsigned sh, unsigned th)
{
    return OP(op) | (uint64_t)sl << 44 | (uint64_t)tl << 32 | (uint64_t)tile << 24 | (uint64_t)sh << 12 | th;
}
static uint64_t c_load_tile(unsigned tile, unsigned sl, unsigned tl, unsigned sh, unsigned th)
{
    return c_tile_op(0x34, tile, sl, tl, sh, th);
}
static uint64_t c_load_block(unsigned tile, unsigned sl, unsigned tl, unsigned sh, unsigned dxt)
{
    return c_tile_op(0x33, tile, sl, tl, sh, dxt);
}
static uint64_t c_load_tlut(unsigned tile, unsigned sl, unsigned sh) { return c_tile_op(0x30, tile, sl, 0, sh, 0); }
static uint64_t c_tile_size(unsigned tile, unsigned sl, unsigned tl, unsigned sh, unsigned th)
{
    return c_tile_op(0x32, tile, sl, tl, sh, th);
}
/* SET_CONVERT with K0-K5 as 9-bit fields. */
static uint64_t c_convert(unsigned k0, unsigned k1, unsigned k2, unsigned k3, unsigned k4, unsigned k5)
{
    return OP(0x2c) | (uint64_t)k0 << 45 | (uint64_t)k1 << 36 | (uint64_t)k2 << 27 | (uint64_t)k3 << 18 |
           (uint64_t)k4 << 9 | k5;
}
/* SET_COMBINE from the eight selectors of each cycle: RGB a, b, c, d then
 * alpha a, b, c, d. */
static uint64_t c_combine(const unsigned c0[8], const unsigned c1[8])
{
    uint64_t hi = (uint64_t)c0[0] << 20 | (uint64_t)c0[2] << 15 | (uint64_t)c0[4] << 12 | (uint64_t)c0[6] << 9 |
                  (uint64_t)c1[0] << 5 | c1[2];
    uint64_t lo = (uint64_t)c0[1] << 28 | (uint64_t)c1[1] << 24 | (uint64_t)c1[4] << 21 | (uint64_t)c1[6] << 18 |
                  (uint64_t)c0[3] << 15 | (uint64_t)c0[5] << 12 | (uint64_t)c0[7] << 9 | (uint64_t)c1[3] << 6 |
                  (uint64_t)c1[5] << 3 | c1[7];
    return OP(0x3c) | hi << 32 | lo;
}
static uint64_t c_combine1(const unsigned c[8]) { return c_combine(c, c); }

/* Other-modes bits (high word, then low word). */
#define M_CYCLE1 (0u << 20)
#define M_CYCLE2 (1u << 20)
#define M_COPY (2u << 20)
#define M_FILL (3u << 20)
#define M_PERSP (1u << 19)
#define M_TLUT (1u << 15)
#define M_TLUT_IA (1u << 14)
#define M_SAMPLE_QUAD (1u << 13)
#define M_BILERP0 (1u << 11)
#define M_BILERP1 (1u << 10)
#define M_RGB_DITHER(m) ((uint32_t)(m) << 6)
#define M_ALPHA_DITHER(m) ((uint32_t)(m) << 4)
#define M_NO_DITHER (M_RGB_DITHER(3) | M_ALPHA_DITHER(3))
#define L_BLEND(p0, a0, m0, b0, p1, a1, m1, b1)                                                                 \
    ((uint32_t)(p0) << 30 | (uint32_t)(p1) << 28 | (uint32_t)(a0) << 26 | (uint32_t)(a1) << 24 |                \
     (uint32_t)(m0) << 22 | (uint32_t)(m1) << 20 | (uint32_t)(b0) << 18 | (uint32_t)(b1) << 16)
#define L_FORCE_BLEND (1u << 14)
#define L_Z_MODE(m) ((uint32_t)(m) << 10)
#define L_IMAGE_READ (1u << 6)
#define L_Z_UPDATE (1u << 5)
#define L_Z_COMPARE (1u << 4)
#define L_AA (1u << 3)
#define L_Z_PRIM (1u << 2)
#define L_ALPHA_DITHER_TEST (1u << 1)
#define L_ALPHA_TEST (1u << 0)

/* Combiner selector lists used throughout. */
static const unsigned CC_PRIM[8] = {8, 8, 16, 3, 7, 7, 7, 3};
static const unsigned CC_SHADE[8] = {8, 8, 16, 4, 7, 7, 7, 4};
static const unsigned CC_TEXEL0[8] = {8, 8, 16, 1, 7, 7, 7, 1};
/* RGB = 1.0 * texel0 alpha. */
static const unsigned CC_TEXEL0_ALPHA[8] = {6, 8, 8, 7, 7, 7, 7, 1};

#define MAX_CMDS 8192
static uint64_t cmds[MAX_CMDS];
static size_t ncmds;
static void emit(uint64_t w)
{
    if (ncmds < MAX_CMDS)
        cmds[ncmds++] = w;
}
static void run(void)
{
    n64_rdp_execute(&rdp, cmds, ncmds);
    ncmds = 0;
}

/* A triangle. Y values are s11.2, X values and slopes s15.16 (per
 * scanline). lft is the major-edge-on-the-left flag. */
typedef struct Tri {
    unsigned op, lft, level, tile;
    int32_t yl, ym, yh;
    int32_t xl, dxldy, xh, dxhdy, xm, dxmdy;
} Tri;

static void emit_tri(const Tri *t)
{
    emit(OP(t->op) | (uint64_t)t->lft << 55 | (uint64_t)t->level << 51 | (uint64_t)t->tile << 48 |
         (uint64_t)((uint32_t)t->yl & 0x3fff) << 32 | (uint64_t)((uint32_t)t->ym & 0x3fff) << 16 |
         ((uint32_t)t->yh & 0x3fff));
    emit((uint64_t)(uint32_t)t->xl << 32 | (uint32_t)t->dxldy);
    emit((uint64_t)(uint32_t)t->xh << 32 | (uint32_t)t->dxhdy);
    emit((uint64_t)(uint32_t)t->xm << 32 | (uint32_t)t->dxmdy);
}

/* The shade or texture coefficient block: four s15.16 attributes with
 * their x, edge and y derivatives, integer halves then fractions. */
static void emit_attr_block(const int32_t v[4], const int32_t dx[4], const int32_t de[4], const int32_t dy[4])
{
#define HI4(a) ((uint64_t)((uint32_t)(a)[0] >> 16) << 48 | (uint64_t)((uint32_t)(a)[1] >> 16) << 32 |                 \
                (uint64_t)((uint32_t)(a)[2] >> 16) << 16 | ((uint32_t)(a)[3] >> 16))
#define LO4(a) ((uint64_t)((uint32_t)(a)[0] & 0xffff) << 48 | (uint64_t)((uint32_t)(a)[1] & 0xffff) << 32 |          \
                (uint64_t)((uint32_t)(a)[2] & 0xffff) << 16 | ((uint32_t)(a)[3] & 0xffff))
    emit(HI4(v));
    emit(HI4(dx));
    emit(LO4(v));
    emit(LO4(dx));
    emit(HI4(de));
    emit(HI4(dy));
    emit(LO4(de));
    emit(LO4(dy));
#undef HI4
#undef LO4
}

#define FX(v) ((int32_t)((v) * 65536.0)) /* s15.16 from a constant */

/* parallel-rdp's noise hash (shaders/noise.h): not a hardware value, so
 * the tests that use noise compute it the same way. */
static uint32_t ref_noise(uint32_t x, uint32_t y, uint32_t prim)
{
    const uint32_t p = 1103515245u;
    uint32_t a = x, b = y, c = prim;
    int i;
    for (i = 0; i < 3; i++) {
        uint32_t na = ((a >> 8) ^ b) * p, nb = ((b >> 8) ^ c) * p, nc = ((c >> 8) ^ a) * p;
        a = na;
        b = nb;
        c = nc;
    }
    return (a >> 16) & 0xffff;
}

/* 1. Fill mode: rectangles in 16, 32 and 8-bit images, the fill colour's
 * per-pixel halves, scissor, and x rounding. */
static void test_fill_rectangle(void)
{
    unsigned x, y;
    begin("fill_rectangle_16");
    memset(rdram + FB_ADDR, 0xaa, 2 * FB_WIDTH * 16);
    emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, 16 << 2, 16 << 2));
    /* Rgb dither on: fill mode ignores it. */
    emit(c_other_modes(M_FILL | M_RGB_DITHER(0), 0));
    emit(c_fill_color(0x12345679u));
    /* xh = 2.75, yh = 1.5, xl = 10.25, yl = 5.25. Fill mode covers
     * floor(xh)..floor(xl) inclusive (x snaps to eighths, start = xleft / 8,
     * end = xright / 8), and the bottom scanline: yl is or'ed with 3, so
     * lines 1 (sub-lines 6, 7 inside) to 5. */
    emit(c_fill_rect(41, 21, 11, 6));
    run();
    for (y = 0; y < 8; y++)
        for (x = 0; x < 14; x++) {
            int inside = x >= 2 && x <= 10 && y >= 1 && y <= 5;
            /* Even pixels take the high half of the fill colour. */
            uint16_t want = inside ? ((x & 1) ? 0x5679 : 0x1234) : 0xaaaa;
            CHECK(px16(x, y) == want, "(%u,%u) = %04x, want %04x", x, y, px16(x, y), want);
        }
    /* The hidden bits follow the colour's coverage: bit 0 set gives 3. */
    CHECK(rdp.links.hidden[(FB_ADDR + 2 * (1 * FB_WIDTH + 3)) >> 1] == 3, "hidden bits of an odd pixel");
    CHECK(rdp.links.hidden[(FB_ADDR + 2 * (1 * FB_WIDTH + 2)) >> 1] == 0, "hidden bits of an even pixel");

    begin("fill_scissor");
    memset(rdram + FB_ADDR, 0, 2 * FB_WIDTH * 16);
    emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
    /* Scissor (4, 2)-(8, 4): xright clamps to 8 * 8, which fill mode
     * includes; y stops before line 4. */
    emit(c_scissor(4 << 2, 2 << 2, 8 << 2, 4 << 2));
    emit(c_other_modes(M_FILL, 0));
    emit(c_fill_color(0xffffffffu));
    emit(c_fill_rect(15 << 2, 15 << 2, 0, 0));
    run();
    for (y = 0; y < 8; y++)
        for (x = 0; x < 16; x++) {
            int inside = x >= 4 && x <= 8 && y >= 2 && y <= 3;
            CHECK(px16(x, y) == (inside ? 0xffff : 0), "(%u,%u) = %04x", x, y, px16(x, y));
        }

    begin("fill_rectangle_32");
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, 16 << 2, 16 << 2));
    emit(c_other_modes(M_FILL, 0));
    emit(c_fill_color(0x11223344u));
    emit(c_fill_rect(2 << 2, 1 << 2, 1 << 2, 1 << 2));
    run();
    CHECK(px32(0, 1) == 0, "left of the rectangle");
    CHECK(px32(1, 1) == 0x11223344u && px32(2, 1) == 0x11223344u, "%08x %08x", px32(1, 1), px32(2, 1));
    CHECK(px32(3, 1) == 0 && px32(1, 0) == 0 && px32(1, 2) == 0, "outside the rectangle");

    begin("fill_rectangle_8");
    emit(c_color_image(4, 1, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, 16 << 2, 16 << 2));
    emit(c_other_modes(M_FILL, 0));
    emit(c_fill_color(0x11223344u));
    emit(c_fill_rect(7 << 2, 0, 0, 0));
    run();
    /* Each byte takes the fill colour's byte at its address modulo 4. */
    for (x = 0; x < 8; x++)
        CHECK(px8(x, 0) == (uint8_t)(0x11223344u >> (24 - 8 * (x & 3))), "x=%u: %02x", x, px8(x, 0));
    CHECK(px8(8, 0) == 0, "past the rectangle");
}

/* 2. SET_COLOR_IMAGE, SET_TEXTURE_IMAGE, SET_Z_IMAGE, SET_SCISSOR and
 * SET_TILE field decoding. */
static void test_image_decoding(void)
{
    const N64RdpState *st = rdp.state;
    begin("image_decoding");
    emit(c_color_image(3, 2, 320, 0x12345678u & 0xffffffu));
    emit(OP(0x3f) | (uint64_t)(0 << 21 | 2 << 19 | 319) << 32 | 0xff345678u); /* upper address bits dropped */
    run();
    CHECK(st->fb_fmt == N64_RDP_FB_RGBA5551 && st->fb_width == 320 && st->fb_addr == 0x345678u,
          "colour image %u %u %06x", st->fb_fmt, st->fb_width, st->fb_addr);
    emit(c_color_image(3, 2, 64, 0x1000));
    run();
    CHECK(st->fb_fmt == N64_RDP_FB_IA88 && st->fb_width == 64, "IA 16-bit colour image");
    emit(c_color_image(0, 3, 1024, 0x2000));
    run();
    CHECK(st->fb_fmt == N64_RDP_FB_RGBA8888 && st->fb_width == 1024, "32-bit colour image");
    emit(c_color_image(2, 1, 1, 0));
    run();
    CHECK(st->fb_fmt == N64_RDP_FB_I8 && st->fb_width == 1, "8-bit colour image");
    emit(c_color_image(4, 0, 8, 0));
    run();
    CHECK(st->fb_fmt == N64_RDP_FB_I4, "4-bit colour image");

    emit(c_tex_image(2, 1, 64, 0xabcdef));
    emit(c_z_image(0xfedcba98u & 0xffffffu));
    emit(OP(0x3e) | 0xff123458u);
    run();
    CHECK(st->tex_fmt == 2 && st->tex_size == 1 && st->tex_width == 64 && st->tex_addr == 0xabcdefu,
          "texture image %u %u %u %06x", st->tex_fmt, st->tex_size, st->tex_width, st->tex_addr);
    CHECK(st->depth_addr == 0x123458u, "z image %06x", st->depth_addr);

    emit(c_scissor(5, 6, 0x123, 0x456));
    run();
    CHECK(st->scissor_xlo == 5 && st->scissor_ylo == 6 && st->scissor_xhi == 0x123 && st->scissor_yhi == 0x456,
          "scissor");

    emit(c_tile(3, 1, 0x1ff, 0x1ab, 5, 9, 1, 12, 7, 2, 0, 3));
    emit(c_tile_size(5, 0x123, 0x456, 0x789, 0xabc));
    run();
    {
        const N64RdpTile *t = &st->tiles[5];
        CHECK(t->fmt == 3 && t->size == 1 && t->stride == 0x1ff * 8 && t->offset == 0x1ab * 8 && t->palette == 9,
              "tile fields");
        /* mask_t 12 behaves as 10; mask_s 0 forces clamp. */
        CHECK(t->mask_t == 10 && t->shift_t == 7 && t->mask_s == 0 && t->shift_s == 3, "tile masks");
        CHECK(t->flags == (N64_RDP_TILE_MIRROR_T | N64_RDP_TILE_CLAMP_S | 0), "tile flags %x", t->flags);
        CHECK(t->slo == 0x123 && t->tlo == 0x456 && t->shi == 0x789 && t->thi == 0xabc, "tile size");
    }
}

/* Common setup for copy-mode texture tests: a colour image of the given
 * size and a copy-mode TEXTURE_RECTANGLE of w x h pixels at the origin. */
static void copy_rect(unsigned w, unsigned h, unsigned tile, unsigned dsdx)
{
    emit(c_rect(0x24, (w - 1) << 2, (h - 1) << 2, 0, 0) | (uint64_t)tile << 24);
    emit((uint64_t)0 << 48 | (uint64_t)0 << 32 | (uint64_t)dsdx << 16 | 0x400);
}

static void copy_setup(unsigned fb_size, uint32_t extra_modes)
{
    emit(c_color_image(0, fb_size, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_COPY | extra_modes, 0));
}

/* 3. TMEM loads checked through copy mode, which writes the texel words
 * TMEM holds: LOAD_TILE for 16-bit and 8-bit images (the load swaps the
 * 32-bit halves of odd lines and the sampler swaps them back), 4-bit
 * textures loaded as 8-bit, LOAD_TLUT with CI4 and CI8, LOAD_BLOCK with
 * and without dxt, and mirrored wrapping. */
static void test_copy_16bit_tiles(void)
{
    static const unsigned fmts[2] = {0, 3}; /* RGBA16, IA16 */
    unsigned f, x, y;
    for (f = 0; f < 2; f++) {
        begin(f ? "copy_ia16" : "copy_rgba16");
        for (y = 0; y < 2; y++)
            for (x = 0; x < 4; x++)
                wr16(TEX_ADDR + 2 * (y * 4 + x), (uint16_t)(0xa001 + y * 0x1000 + x * 0x0102 + f * 0x10));
        copy_setup(2, 0);
        emit(c_tex_image(fmts[f], 2, 4, TEX_ADDR));
        emit(c_simple_tile(fmts[f], 2, 1, 0, 0));
        emit(c_load_tile(0, 0, 0, 3 << 2, 1 << 2));
        copy_rect(4, 2, 0, 0x1000);
        run();
        for (y = 0; y < 2; y++)
            for (x = 0; x < 4; x++) {
                uint16_t want = (uint16_t)(0xa001 + y * 0x1000 + x * 0x0102 + f * 0x10);
                CHECK(px16(x, y) == want, "(%u,%u) %04x want %04x", x, y, px16(x, y), want);
            }
        CHECK(px16(4, 0) == 0 && px16(0, 2) == 0, "outside the rectangle");
    }
}

static void load_palette(unsigned entries)
{
    unsigned i;
    for (i = 0; i < entries; i++)
        wr16(PAL_ADDR + 2 * i, (uint16_t)(0x8001u ^ (i * 0x0123u)));
    emit(c_tex_image(0, 2, 1, PAL_ADDR));
    /* The TLUT tile: 4-bit, at TMEM 0x800 (256 64-bit words). */
    emit(c_simple_tile(0, 0, 0, 256, 7));
    emit(c_load_tlut(7, 0, (entries - 1) << 2));
}

static uint16_t pal(unsigned i) { return (uint16_t)(0x8001u ^ (i * 0x0123u)); }

static void test_copy_tlut(void)
{
    static const uint8_t ci8[2][4] = {{3, 200, 0, 255}, {17, 1, 128, 64}};
    static const uint8_t ci4[2][2] = {{0x1f, 0x3c}, {0x52, 0xa7}};
    unsigned x, y;

    begin("copy_ci8_tlut");
    for (y = 0; y < 2; y++)
        for (x = 0; x < 4; x++)
            rdram[TEX_ADDR + y * 4 + x] = ci8[y][x];
    copy_setup(2, M_TLUT);
    load_palette(256);
    emit(c_tex_image(2, 1, 4, TEX_ADDR));
    emit(c_simple_tile(2, 1, 1, 0, 0));
    emit(c_load_tile(0, 0, 0, 3 << 2, 1 << 2));
    copy_rect(4, 2, 0, 0x1000);
    run();
    /* TLUT entries are replicated four times in the high half of TMEM. */
    CHECK(n64_be16(rdp.state->tmem + 0x800 + 8 * 200 + 6) == pal(200), "TLUT replication");
    for (y = 0; y < 2; y++)
        for (x = 0; x < 4; x++)
            CHECK(px16(x, y) == pal(ci8[y][x]), "(%u,%u) %04x want %04x", x, y, px16(x, y), pal(ci8[y][x]));

    begin("copy_ci4_tlut");
    for (y = 0; y < 2; y++)
        for (x = 0; x < 2; x++)
            rdram[TEX_ADDR + y * 2 + x] = ci4[y][x];
    copy_setup(2, M_TLUT);
    load_palette(256);
    /* 4-bit textures load as 8-bit through a separate load tile. */
    emit(c_tex_image(2, 1, 2, TEX_ADDR));
    emit(c_simple_tile(2, 1, 1, 0, 7));
    emit(c_load_tile(7, 0, 0, 1 << 2, 1 << 2));
    emit(c_tile(2, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0)); /* CI4, palette 2 */
    emit(c_tile_size(0, 0, 0, 3 << 2, 1 << 2));
    copy_rect(4, 2, 0, 0x1000);
    run();
    for (y = 0; y < 2; y++)
        for (x = 0; x < 4; x++) {
            unsigned nib = (ci4[y][x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
            CHECK(px16(x, y) == pal(32 + nib), "(%u,%u) %04x want %04x", x, y, px16(x, y), pal(32 + nib));
        }
}

static void test_copy_8bit_and_4bit(void)
{
    static const unsigned fmts8[3] = {4, 3, 2}; /* I8, IA8, CI8 */
    static const char *names8[3] = {"copy_i8", "copy_ia8", "copy_ci8"};
    static const char *names4[3] = {"copy_i4", "copy_ia4", "copy_ci4"};
    unsigned f, x, y;
    for (f = 0; f < 3; f++) {
        begin(names8[f]);
        for (y = 0; y < 2; y++)
            for (x = 0; x < 4; x++)
                rdram[TEX_ADDR + y * 4 + x] = (uint8_t)(0x10 * (y * 4 + x) + 3 + f);
        copy_setup(1, 0);
        emit(c_tex_image(fmts8[f], 1, 4, TEX_ADDR));
        emit(c_simple_tile(fmts8[f], 1, 1, 0, 0));
        emit(c_load_tile(0, 0, 0, 3 << 2, 1 << 2));
        copy_rect(4, 2, 0, 0x1000);
        run();
        /* An 8-bit colour image takes one texel per byte. */
        for (y = 0; y < 2; y++)
            for (x = 0; x < 4; x++) {
                uint8_t want = (uint8_t)(0x10 * (y * 4 + x) + 3 + f);
                CHECK(px8(x, y) == want, "(%u,%u) %02x want %02x", x, y, px8(x, y), want);
            }
        CHECK(px8(4, 0) == 0, "outside the rectangle");
    }
    for (f = 0; f < 3; f++) {
        begin(names4[f]);
        rdram[TEX_ADDR + 0] = 0x1f;
        rdram[TEX_ADDR + 1] = 0x3c;
        rdram[TEX_ADDR + 2] = 0x52;
        rdram[TEX_ADDR + 3] = 0xa7;
        copy_setup(1, 0);
        emit(c_tex_image(fmts8[f], 1, 2, TEX_ADDR));
        emit(c_simple_tile(fmts8[f], 1, 1, 0, 7));
        emit(c_load_tile(7, 0, 0, 1 << 2, 1 << 2));
        emit(c_simple_tile(fmts8[f], 0, 1, 0, 0));
        emit(c_tile_size(0, 0, 0, 3 << 2, 1 << 2));
        copy_rect(4, 2, 0, 0x1000);
        run();
        /* The copy pipe replicates each nibble into a byte. */
        for (y = 0; y < 2; y++)
            for (x = 0; x < 4; x++) {
                unsigned nib = (rdram[TEX_ADDR + 2 * y + (x >> 1)] >> ((x & 1) ? 0 : 4)) & 15;
                CHECK(px8(x, y) == nib * 0x11, "(%u,%u) %02x want %02x", x, y, px8(x, y), nib * 0x11);
            }
    }
}

static void test_load_block(void)
{
    unsigned x, y, pass;
    for (pass = 0; pass < 2; pass++) {
        begin(pass ? "load_block_no_dxt" : "load_block_dxt");
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                wr16(TEX_ADDR + 2 * (y * 4 + x), (uint16_t)(0x0101u * (y * 4 + x + 1)));
        copy_setup(2, 0);
        emit(c_tex_image(0, 2, 4, TEX_ADDR)); /* width is irrelevant to LOAD_BLOCK */
        emit(c_simple_tile(0, 2, 0, 0, 7));
        /* 16 texels; dxt = 2048 / (one 64-bit word per line) advances T
         * every word, so odd words are stored with their halves swapped. */
        emit(c_load_block(7, 0, 0, 15, pass ? 0 : 0x800));
        emit(c_simple_tile(0, 2, 1, 0, 0));
        emit(c_tile_size(0, 0, 0, 3 << 2, 3 << 2));
        copy_rect(4, 4, 0, 0x1000);
        run();
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                /* Without dxt nothing is swapped on load, but the sampler
                 * still swaps odd lines: texel x ^ 2 appears there. */
                unsigned sx = (pass && (y & 1)) ? x ^ 2 : x;
                uint16_t want = (uint16_t)(0x0101u * (y * 4 + sx + 1));
                CHECK(px16(x, y) == want, "(%u,%u) %04x want %04x", x, y, px16(x, y), want);
            }
    }
}

static void test_copy_mirror(void)
{
    unsigned x;
    begin("copy_mirror_mask");
    for (x = 0; x < 4; x++)
        wr16(TEX_ADDR + 2 * x, (uint16_t)(0x1111u * (x + 1)));
    copy_setup(2, 0);
    emit(c_tex_image(0, 2, 4, TEX_ADDR));
    /* mask_s 2 (4 texels), mirror_s. */
    emit(c_tile(0, 2, 1, 0, 0, 0, 0, 0, 0, 1, 2, 0));
    emit(c_load_tile(0, 0, 0, 3 << 2, 0));
    copy_rect(8, 1, 0, 0x1000);
    run();
    /* s 4..7 mirrors to 3..0. */
    for (x = 0; x < 8; x++) {
        unsigned s = x < 4 ? x : 7 - x;
        CHECK(px16(x, 0) == 0x1111u * (s + 1), "x=%u %04x", x, px16(x, 0));
    }
}

/* One-cycle texture rectangle of w x h pixels at the origin, one texel per
 * pixel, into a 32-bit colour image. */
static void cycle1_tex_setup(uint32_t extra_hi, const unsigned *combine)
{
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_CYCLE1 | M_NO_DITHER | extra_hi, 0));
    emit(c_combine1(combine));
}

static void tex_rect_1cycle(unsigned w, unsigned h, unsigned tile, unsigned s)
{
    emit(c_rect(0x24, w << 2, h << 2, 0, 0) | (uint64_t)tile << 24);
    emit((uint64_t)s << 48 | (uint64_t)0x400 << 16 | 0x400);
}

/* 4. RGBA32 (split across the TMEM halves) and YUV16 (chroma low, luma
 * high), rendered in one-cycle mode. */
static void test_rgba32_and_yuv(void)
{
    unsigned x, y;
    begin("rgba32_1cycle");
    for (y = 0; y < 2; y++)
        for (x = 0; x < 4; x++)
            wr32(TEX_ADDR + 4 * (y * 4 + x), 0x10203040u + 0x01020304u * (y * 4 + x));
    cycle1_tex_setup(M_BILERP0 | M_BILERP1, CC_TEXEL0);
    emit(c_tex_image(0, 3, 4, TEX_ADDR));
    emit(c_simple_tile(0, 3, 1, 0, 0));
    emit(c_load_tile(0, 0, 0, 3 << 2, 1 << 2));
    tex_rect_1cycle(4, 2, 0, 0);
    run();
    /* RGB from the texel; a 32-bit image stores coverage (7 << 5) in
     * alpha. */
    for (y = 0; y < 2; y++)
        for (x = 0; x < 4; x++) {
            uint32_t t = 0x10203040u + 0x01020304u * (y * 4 + x);
            CHECK(px32(x, y) == ((t & 0xffffff00u) | 0xe0u), "(%u,%u) %08x want %08x", x, y, px32(x, y),
                  (t & 0xffffff00u) | 0xe0u);
        }
    /* The alpha half, through the combiner as RGB. */
    emit(c_combine1(CC_TEXEL0_ALPHA));
    tex_rect_1cycle(4, 2, 0, 0);
    run();
    for (y = 0; y < 2; y++)
        for (x = 0; x < 4; x++) {
            uint32_t a = (0x10203040u + 0x01020304u * (y * 4 + x)) & 0xff;
            CHECK(px32(x, y) == (a * 0x01010100u | 0xe0u), "(%u,%u) alpha %08x", x, y, px32(x, y));
        }

    begin("yuv16_1cycle");
    /* UYVY: texels 0, 1 have U = 0x90, V = 0x70, Y = 0x50, 0x60; texels
     * 2, 3 have no chroma and Y = 0x30, 0xff. */
    wr32(TEX_ADDR, 0x90507060u);
    wr32(TEX_ADDR + 4, 0x803080ffu);
    /* bi_lerp0 off: the cycle converts. K0 = 64, K1 = -16, K2 = -32,
     * K3 = 80 give factors 2K + 1 = 129, -31, -63, 161. */
    cycle1_tex_setup(0, CC_TEXEL0);
    emit(c_convert(0x40, 0x1f0, 0x1e0, 0x50, 0, 0));
    emit(c_tex_image(1, 2, 4, TEX_ADDR));
    emit(c_simple_tile(1, 2, 1, 0, 0));
    emit(c_load_tile(0, 0, 0, 3 << 2, 0));
    tex_rect_1cycle(4, 1, 0, 0);
    run();
    /* U' = 16, V' = -16:
     *   R = Y + (129 * -16 + 128) >> 8 = Y - 8
     *   G = Y + (-31 * 16 + -63 * -16 + 128) >> 8 = Y + 2
     *   B = Y + (161 * 16 + 128) >> 8 = Y + 10 */
    CHECK(px32(0, 0) == 0x48525ae0u, "Y0 %08x", px32(0, 0));
    CHECK(px32(1, 0) == 0x58626ae0u, "Y1 %08x", px32(1, 0));
    CHECK(px32(2, 0) == 0x303030e0u, "Y2 %08x", px32(2, 0));
    CHECK(px32(3, 0) == 0xffffffe0u, "Y3 %08x", px32(3, 0));
}

/* 5. Texel decoding for each format in one-cycle mode: the colour and the
 * alpha of four texels. */
typedef struct DecodeCase {
    const char *name;
    unsigned fmt, size;     /* the render tile */
    unsigned load_size;     /* the size the image is loaded at */
    unsigned load_width;    /* texels of load_size in the line */
    uint8_t bytes[8];       /* the image */
    uint32_t want_rgb[4];   /* 0xRRGGBB */
    uint8_t want_a[4];
    uint32_t extra_hi;
} DecodeCase;

static void test_texel_decoding(void)
{
    static const DecodeCase cases[] = {
        /* RGBA16: 5-bit channels expand as (c << 3) | (c >> 2). */
        {"decode_rgba16", 0, 2, 2, 4, {0xf8, 0x01, 0x07, 0xc0, 0x00, 0x3e, 0x84, 0x21},
         {0xff0000, 0x00ff00, 0x0000ff, 0x848484}, {0xff, 0x00, 0x00, 0xff}, 0},
        /* IA16: intensity byte, alpha byte. */
        {"decode_ia16", 3, 2, 2, 4, {0x80, 0xc0, 0x12, 0x34, 0xff, 0x00, 0x00, 0xff},
         {0x808080, 0x121212, 0xffffff, 0x000000}, {0xc0, 0x34, 0x00, 0xff}, 0},
        /* IA8: 4-bit intensity and alpha, each replicated. */
        {"decode_ia8", 3, 1, 1, 4, {0x5a, 0xf0, 0x0f, 0x81, 0, 0, 0, 0},
         {0x555555, 0xffffff, 0x000000, 0x888888}, {0xaa, 0x00, 0xff, 0x11}, 0},
        /* IA4: 3-bit intensity (iii -> iii iii ii), 1-bit alpha. */
        {"decode_ia4", 3, 0, 1, 2, {0xb6, 0xf0, 0, 0, 0, 0, 0, 0},
         {0xb6b6b6, 0x6d6d6d, 0xffffff, 0x000000}, {0xff, 0x00, 0xff, 0x00}, 0},
        /* I4 and I8: intensity in every channel and alpha. */
        {"decode_i4", 4, 0, 1, 2, {0x7a, 0x0f, 0, 0, 0, 0, 0, 0},
         {0x777777, 0xaaaaaa, 0x000000, 0xffffff}, {0x77, 0xaa, 0x00, 0xff}, 0},
        {"decode_i8", 4, 1, 1, 4, {0x3c, 0x00, 0xff, 0x81, 0, 0, 0, 0},
         {0x3c3c3c, 0x000000, 0xffffff, 0x818181}, {0x3c, 0x00, 0xff, 0x81}, 0},
        /* CI4 without a TLUT: palette << 4 | index in every channel
         * (palette 0 here). */
        {"decode_ci4_raw", 2, 0, 1, 2, {0x5f, 0x09, 0, 0, 0, 0, 0, 0},
         {0x050505, 0x0f0f0f, 0x000000, 0x090909}, {0x05, 0x0f, 0x00, 0x09}, 0},
        /* CI8 through an IA16 TLUT: entry i is 0x8001 ^ i * 0x123. */
        {"decode_ci8_tlut_ia16", 2, 1, 1, 4, {0, 1, 2, 255, 0, 0, 0, 0},
         {0x808080, 0x818181, 0x828282, 0xa1a1a1}, {0x01, 0x22, 0x47, 0xdc}, M_TLUT | M_TLUT_IA},
    };
    unsigned i, x;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const DecodeCase *c = &cases[i];
        begin(c->name);
        memcpy(rdram + TEX_ADDR, c->bytes, 8);
        cycle1_tex_setup(M_BILERP0 | M_BILERP1 | c->extra_hi, CC_TEXEL0);
        if (c->extra_hi & M_TLUT)
            load_palette(256);
        emit(c_tex_image(c->fmt, c->load_size, c->load_width, TEX_ADDR));
        emit(c_simple_tile(c->fmt, c->load_size, 1, 0, 7));
        emit(c_load_tile(7, 0, 0, (c->load_width - 1) << 2, 0));
        emit(c_simple_tile(c->fmt, c->size, 1, 0, 0));
        emit(c_tile_size(0, 0, 0, 3 << 2, 0));
        tex_rect_1cycle(4, 1, 0, 0);
        run();
        for (x = 0; x < 4; x++)
            CHECK(px32(x, 0) == (c->want_rgb[x] << 8 | 0xe0u), "texel %u: %08x want %06x", x, px32(x, 0),
                  c->want_rgb[x]);
        emit(c_combine1(CC_TEXEL0_ALPHA));
        tex_rect_1cycle(4, 1, 0, 0);
        run();
        for (x = 0; x < 4; x++)
            CHECK(px32(x, 0) == (c->want_a[x] * 0x01010100u | 0xe0u), "alpha %u: %08x want %02x", x, px32(x, 0),
                  c->want_a[x]);
    }
}

/* Bilinear filtering half way between texels: t00 + ((t10 - t00) * 16 +
 * 16) >> 5. */
static void test_bilinear(void)
{
    begin("bilinear");
    rdram[TEX_ADDR + 0] = 0x20;
    rdram[TEX_ADDR + 1] = 0x60;
    rdram[TEX_ADDR + 2] = 0xa0;
    rdram[TEX_ADDR + 3] = 0xe0;
    cycle1_tex_setup(M_BILERP0 | M_BILERP1 | M_SAMPLE_QUAD, CC_TEXEL0);
    emit(c_tex_image(4, 1, 4, TEX_ADDR));
    emit(c_simple_tile(4, 1, 1, 0, 0));
    emit(c_load_tile(0, 0, 0, 3 << 2, 0));
    tex_rect_1cycle(4, 1, 0, 16); /* s starts at 0.5 */
    run();
    CHECK(px32(0, 0) == 0x404040e0u, "x0 %08x", px32(0, 0));
    CHECK(px32(1, 0) == 0x808080e0u, "x1 %08x", px32(1, 0));
    CHECK(px32(2, 0) == 0xc0c0c0e0u, "x2 %08x", px32(2, 0));
    /* s = 3.5 clamps to the tile's last texel (frac 0). */
    CHECK(px32(3, 0) == 0xe0e0e0e0u, "x3 %08x", px32(3, 0));
}

/* 6. A flat triangle's edges. Left (major) edge: x = 1.25 + 0.5 y;
 * middle edge: x = 2.5 + 0.75 y from y = 0 to 3; low edge: x = 4.75 -
 * 0.5 (y - 3) from y = 3 to 6. Without AA a pixel is drawn when its
 * first sample (its top-left corner, at sub-scanline 0) lies in
 * [left, right), with edges snapped to eighths of a pixel:
 *   y0: left 1.25 -> x >= 2, right 2.5  -> x <= 2        {2}
 *   y1: left 1.75 -> x >= 2, right 3.25 -> x <= 3        {2, 3}
 *   y2: left 2.25 -> x >= 3, right 4.0  -> x <= 3        {3}
 *   y3: left 2.75 -> x >= 3, right 4.75 -> x <= 4        {3, 4}
 *   y4: left 3.25 -> x >= 4, right 4.25 -> x <= 4        {4}
 *   y5: left 3.75 -> x >= 4, right 3.75 -> x <= 3        {} */
static void test_flat_triangle(void)
{
    static const uint8_t rows[6][2] = {{2, 2}, {2, 3}, {3, 3}, {3, 4}, {4, 4}, {1, 0}};
    Tri t = {0x08, 1, 0, 0, 24, 12, 0, FX(4.75), FX(-0.5), FX(1.25), FX(0.5), FX(2.5), FX(0.75)};
    unsigned x, y;
    begin("flat_triangle_edges");
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, 0));
    emit(c_combine1(CC_PRIM));
    emit(c_prim_color(0, 0, 0x336699ffu));
    emit_tri(&t);
    run();
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            int inside = y < 6 && x >= rows[y][0] && x <= rows[y][1];
            CHECK(((px32(x, y) & 0xffffff00u) == (inside ? 0x33669900u : 0)), "(%u,%u) %08x", x, y, px32(x, y));
        }
    /* Pixel (2, 0)'s coverage: sub-scanline edges (left, right) in
     * eighths are (10, 20), (11, 21), (12, 23), (13, 25) (21 and 25 carry a
     * sticky bit); its samples at 16, 20 / 18, 22 / 16, 20 / 18, 22 hit 6
     * of 8. Coverage clamp mode without blending stores (6 - 1) << 5. */
    CHECK((px32(2, 0) & 0xff) == 0xa0, "coverage of (2,0): %02x", px32(2, 0) & 0xff);
}

/* 7. Gouraud shading: R = 16 + 1.5 x along x, G = 32 + 2 y down the
 * edge, B constant. RGBA interpolate to 9 bits and truncate. */
static void test_shaded_triangle(void)
{
    static const int32_t v[4] = {FX(16), FX(32), FX(100), FX(255)};
    static const int32_t dx[4] = {FX(1.5), 0, 0, 0};
    static const int32_t de[4] = {0, FX(2), 0, 0};
    static const int32_t dy[4] = {0, FX(2), 0, 0};
    Tri t = {0x0c, 1, 0, 0, 32, 32, 0, FX(8), 0, FX(0), 0, FX(8), 0};
    unsigned x, y;
    begin("gouraud_triangle");
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, 0));
    emit(c_combine1(CC_SHADE));
    emit_tri(&t);
    emit_attr_block(v, dx, de, dy);
    run();
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            uint32_t r = 16 + (3 * x) / 2, g = 32 + 2 * y;
            uint32_t want = r << 24 | g << 16 | 100u << 8 | 0xe0u;
            CHECK(px32(x, y) == want, "(%u,%u) %08x want %08x", x, y, px32(x, y), want);
        }
    CHECK(px32(8, 0) == 0, "right edge is exclusive");

    /* With the major edge on the left sloping left (do_offset), the
     * attributes are latched at the scanline's last sub-scanline: on line
     * 1 the left edge there is 8 - 1 - 0.75 = 6.25, so the span starts
     * at x 6 with a fraction of 0.25 and R = 16 + 4 (x - 6.25). The first
     * pixel drawn is 7 (the edge at sub-scanline 0 is 7.0). */
    {
        static const int32_t v2[4] = {FX(16), 0, 0, FX(255)};
        static const int32_t dx2[4] = {FX(4), 0, 0, 0};
        static const int32_t zero[4] = {0, 0, 0, 0};
        Tri t2 = {0x0c, 1, 0, 0, 16, 16, 0, FX(12), 0, FX(8), FX(-1), FX(12), 0};
        begin("gouraud_do_offset");
        emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
        emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
        emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, 0));
        emit(c_combine1(CC_SHADE));
        emit_tri(&t2);
        emit_attr_block(v2, dx2, zero, zero);
        run();
        CHECK(px32(6, 1) == 0, "(6,1) not covered: %08x", px32(6, 1));
        CHECK(px32(7, 1) >> 24 == 19, "(7,1) R %u", px32(7, 1) >> 24);
        CHECK(px32(11, 1) >> 24 == 35, "(11,1) R %u", px32(11, 1) >> 24);
        CHECK(px32(12, 1) == 0, "(12,1) not covered");
    }
}

/* 8. The depth buffer with primitive depth: z = depth << 3 as 18 bits,
 * stored as a 14-bit float (exponent = leading ones, 11-bit mantissa)
 * << 2 with the compressed dz in the low bits and the hidden bits. */
static void z_rect(uint32_t lo_modes, unsigned z, unsigned x0, unsigned x1, uint32_t color)
{
    emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, L_Z_PRIM | lo_modes));
    emit(c_prim_depth(z, 1));
    emit(c_prim_color(0, 0, color));
    emit(c_rect(0x36, x1 << 2, 2 << 2, x0 << 2, 0)); /* 1-cycle: right and bottom exclusive */
}

static void test_depth(void)
{
    unsigned x;
    begin("depth_compare_update");
    for (x = 0; x < 16; x++)
        wr16(Z_ADDR + 2 * x, 0xfffc), wr16(Z_ADDR + 2 * (FB_WIDTH + x), 0xfffc);
    emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
    emit(c_z_image(Z_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_combine1(CC_PRIM));
    /* A at depth 0x4000 over the maximum: z = 0x20000 compresses to
     * exponent 1, mantissa 0 = 0x800, stored as 0x2000 (dz 1 compresses
     * to 0). */
    z_rect(L_Z_COMPARE | L_Z_UPDATE, 0x4000, 0, 4, 0xf800f8ffu);
    /* B farther (0x5000) loses everywhere it overlaps A. */
    z_rect(L_Z_COMPARE | L_Z_UPDATE, 0x5000, 0, 4, 0x07c0ffffu);
    /* C nearer (0x3000) wins over A and over the cleared buffer: z =
     * 0x18000 is exponent 0, mantissa 0x600, stored 0x1800. */
    z_rect(L_Z_COMPARE | L_Z_UPDATE, 0x3000, 2, 6, 0x0000f8ffu);
    run();
    /* RGBA5551 of 0xf8,0x00,0xf8 with full coverage: 0xf83f. */
    CHECK(px16(0, 0) == 0xf83f && px16(1, 1) == 0xf83f, "A kept: %04x %04x", px16(0, 0), px16(1, 1));
    CHECK(rd16(Z_ADDR) == 0x2000 && rd16(Z_ADDR + 2 * FB_WIDTH + 2) == 0x2000, "A depth %04x", rd16(Z_ADDR));
    CHECK(rdp.links.hidden[Z_ADDR >> 1] == 0, "A dz hidden bits");
    for (x = 2; x < 6; x++) {
        CHECK(px16(x, 0) == 0x003f, "C at x=%u: %04x", x, px16(x, 0));
        CHECK(rd16(Z_ADDR + 2 * x) == 0x1800, "C depth at x=%u: %04x", x, rd16(Z_ADDR + 2 * x));
    }
    CHECK(rd16(Z_ADDR + 2 * 6) == 0xfffc && px16(6, 0) == 0, "past C untouched");

    /* Update without compare always writes: 0x5000 -> z 0x28000 ->
     * exponent 1, mantissa 0x400 -> 0xc00 -> 0x3000. */
    z_rect(L_Z_UPDATE, 0x5000, 0, 1, 0xffffffffu);
    run();
    CHECK(rd16(Z_ADDR) == 0x3000 && px16(0, 0) == 0xffff, "update only: %04x %04x", rd16(Z_ADDR), px16(0, 0));

    /* Decal passes within the combined dz of the stored depth (here
     * 8 << 3 = 64 around 0x20000) and fails outside. */
    begin("depth_decal");
    emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
    emit(c_z_image(Z_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_combine1(CC_PRIM));
    z_rect(L_Z_UPDATE, 0x4000, 0, 2, 0x000000ffu);
    z_rect(L_Z_COMPARE | L_Z_MODE(3), 0x4000, 0, 1, 0xf800f8ffu);
    z_rect(L_Z_COMPARE | L_Z_MODE(3), 0x4100, 1, 2, 0xf800f8ffu);
    run();
    CHECK(px16(0, 0) == 0xf83f, "decal at equal depth drawn: %04x", px16(0, 0));
    CHECK(px16(1, 0) == 0x0001, "decal 0x800 farther rejected: %04x", px16(1, 0));
}

/* Every depth mode. With coverage times alpha at alpha 0x80 the coverage
 * drops to 4 and, reading memory coverage 0, does not overflow: then
 * opaque passes anything within dz of the stored depth, transparent only
 * what is strictly in front, decal only what is within dz, and
 * interpenetrating (without overflow) behaves as opaque. Stored depth is
 * 0x4000 (z 0x20000, dz 0, so the combined dz is 8 << 3 = 64). */
#define L_CVG_TIMES_ALPHA (1u << 12)
static void depth_mode_draw(unsigned mode, uint32_t extra_lo, unsigned z, unsigned prim_alpha)
{
    unsigned x;
    begin("depth_modes");
    for (x = 0; x < 4; x++) {
        wr16(Z_ADDR + 2 * x, 0x2000);
        rdp.links.hidden[(Z_ADDR >> 1) + x] = 0;
        rdp.links.hidden[(FB_ADDR >> 1) + x] = 0;
    }
    emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
    emit(c_z_image(Z_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_combine1(CC_PRIM));
    emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, L_Z_PRIM | L_Z_COMPARE | L_Z_MODE(mode) | extra_lo));
    emit(c_prim_depth(z, 1));
    emit(c_prim_color(0, 0, 0xf8000000u | prim_alpha));
    emit(c_rect(0x36, 1 << 2, 1 << 2, 0, 0));
    run();
}

static void test_depth_modes(void)
{
    static const int pass_equal[4] = {1, 1, 0, 1};
    static const int pass_nearer[4] = {1, 1, 1, 0};
    unsigned mode;
    for (mode = 0; mode < 4; mode++) {
        depth_mode_draw(mode, L_CVG_TIMES_ALPHA | L_IMAGE_READ, 0x4000, 0x80);
        /* Drawn: red with coverage (4 - 1) = 3: bit 0 clear, hidden 3. */
        CHECK(px16(0, 0) == (pass_equal[mode] ? 0xf800 : 0), "mode %u at equal depth: %04x", mode, px16(0, 0));
        if (pass_equal[mode])
            CHECK(rdp.links.hidden[FB_ADDR >> 1] == 3, "mode %u coverage", mode);
        depth_mode_draw(mode, L_CVG_TIMES_ALPHA | L_IMAGE_READ, 0x3000, 0x80);
        CHECK(px16(0, 0) == (pass_nearer[mode] ? 0xf800 : 0), "mode %u nearer: %04x", mode, px16(0, 0));
    }
    /* Interpenetrating, overflowing, in front but within dz: z 0x1fff8
     * against 0x20000. Coverage scales by ((0x20000 >> 3) - (0x1fff8 >>
     * 3)) & 15 = 1 to 8 / 8 = 1 and is stored as (1 - 1) = 0. Opaque keeps
     * full coverage (7). */
    depth_mode_draw(1, 0, 0x3fff, 0xff);
    CHECK(px16(0, 0) == 0xf800 && rdp.links.hidden[FB_ADDR >> 1] == 0, "interpenetrating: %04x hidden %u",
          px16(0, 0), rdp.links.hidden[FB_ADDR >> 1]);
    depth_mode_draw(0, 0, 0x3fff, 0xff);
    CHECK(px16(0, 0) == 0xf801 && rdp.links.hidden[FB_ADDR >> 1] == 3, "opaque: %04x hidden %u", px16(0, 0),
          rdp.links.hidden[FB_ADDR >> 1]);
}

/* 9. The combiner: every selector of every slot in both cycles. Each
 * input has its own value; the test sets one slot and probes with the
 * others so the output is (A - B) * C / 256 + D of known numbers. */
static const uint8_t IN_T0[4] = {0x21, 0x22, 0x23, 0x24};
static const uint8_t IN_T1[4] = {0x31, 0x32, 0x33, 0x34};
static const uint8_t IN_SH[4] = {0x41, 0x42, 0x43, 0x44};
static const uint8_t IN_PR[4] = {0x51, 0x52, 0x53, 0x54};
static const uint8_t IN_EN[4] = {0x61, 0x62, 0x63, 0x64};
static const uint8_t IN_KC[3] = {0x71, 0x72, 0x73};
static const uint8_t IN_KS[3] = {0x81, 0x82, 0x83};
#define IN_K4 0x19au /* 9 bits, so the top bit is exercised */
#define IN_K5 0x1b5u
#define IN_LOD 0xff /* two-cycle, LOD < 0 with max level 0: 0xff */

static int32_t sext9(int32_t v) { return (v & 0x100) ? (v & 0xff) - 0x100 : (v & 0xff); }
static int32_t expand9(int32_t v) { return sext9(v - 0x80) + 0x80; }
static int32_t clamp9(int32_t v)
{
    int32_t r = expand9(v);
    return r < 0 ? 0 : (r > 255 ? 255 : r);
}
static int32_t asr8(int32_t v) { return v >= 0 ? v >> 8 : -((-v + 255) >> 8); }
static int32_t equation(int32_t a, int32_t b, int32_t c, int32_t d)
{
    return asr8((expand9(a) - expand9(b)) * sext9(c) + 0x80) + expand9(d);
}

typedef struct CombState {
    const int32_t *combined; /* this cycle's COMBINED input */
    int cycle;               /* 0 or 1; cycle 1 swaps the texels */
    int32_t plf;             /* primitive LOD fraction */
    int32_t noise;
} CombState;

static int32_t in_texel(const CombState *s, int which, int ch)
{
    const uint8_t *t = (which ^ s->cycle) ? IN_T1 : IN_T0;
    return t[ch];
}

static int32_t ref_rgb(const CombState *s, int slot, unsigned sel, int ch)
{
    switch (sel) {
    case 0: return s->combined[ch];
    case 1: return in_texel(s, 0, ch);
    case 2: return in_texel(s, 1, ch);
    case 3: return IN_PR[ch];
    case 4: return IN_SH[ch];
    case 5: return IN_EN[ch];
    default: break;
    }
    switch (slot) {
    case 0: return sel == 6 ? 0x100 : sel == 7 ? s->noise : 0;
    case 1: return sel == 6 ? IN_KC[ch] : sel == 7 ? (int32_t)IN_K4 : 0;
    case 2:
        switch (sel) {
        case 6: return IN_KS[ch];
        case 7: return s->combined[3];
        case 8: return in_texel(s, 0, 3);
        case 9: return in_texel(s, 1, 3);
        case 10: return IN_PR[3];
        case 11: return IN_SH[3];
        case 12: return IN_EN[3];
        case 13: return IN_LOD;
        case 14: return s->plf;
        case 15: return (int32_t)IN_K5;
        default: return 0;
        }
    default: return sel == 6 ? 0x100 : 0;
    }
}

static int32_t ref_alpha(const CombState *s, int slot, unsigned sel)
{
    if (slot == 2) {
        switch (sel) {
        case 0: return IN_LOD;
        case 6: return s->plf;
        case 7: return 0;
        default: break;
        }
    }
    switch (sel) {
    case 0: return s->combined[3];
    case 1: return in_texel(s, 0, 3);
    case 2: return in_texel(s, 1, 3);
    case 3: return IN_PR[3];
    case 4: return IN_SH[3];
    case 5: return IN_EN[3];
    case 6: return 0x100;
    default: return 0;
    }
}

static void ref_cycle(const CombState *s, const unsigned sel[8], int32_t out[4])
{
    int ch;
    for (ch = 0; ch < 3; ch++)
        out[ch] = equation(ref_rgb(s, 0, sel[0], ch), ref_rgb(s, 1, sel[1], ch), ref_rgb(s, 2, sel[2], ch),
                           ref_rgb(s, 3, sel[3], ch));
    out[3] = equation(ref_alpha(s, 0, sel[4]), ref_alpha(s, 1, sel[5]), ref_alpha(s, 2, sel[6]),
                      ref_alpha(s, 3, sel[7]));
}

/* The textures, shade and constants every combiner and blender test
 * draws with: a 4x4 shaded, textured triangle pair covering (0,0)-(4,4). */
static void combiner_scene_setup(uint32_t hi_modes, uint32_t lo_modes, unsigned plf)
{
    unsigned i;
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(hi_modes | M_NO_DITHER | M_BILERP0 | M_BILERP1, lo_modes));
    /* RGBA32 textures: 2x2 of IN_T0 in tile 0 at TMEM 0, of IN_T1 in tile
     * 1 at TMEM 64. */
    for (i = 0; i < 4; i++) {
        wr32(TEX_ADDR + 4 * i, n64_be32(IN_T0));
        wr32(TEX_ADDR + 0x100 + 4 * i, n64_be32(IN_T1));
    }
    emit(c_tex_image(0, 3, 2, TEX_ADDR));
    emit(c_simple_tile(0, 3, 1, 0, 0));
    emit(c_load_tile(0, 0, 0, 1 << 2, 1 << 2));
    emit(c_tex_image(0, 3, 2, TEX_ADDR + 0x100));
    emit(c_simple_tile(0, 3, 1, 8, 1));
    emit(c_load_tile(1, 0, 0, 1 << 2, 1 << 2));
    emit(c_prim_color(0, plf, n64_be32(IN_PR)));
    emit(c_env_color(n64_be32(IN_EN)));
    emit(OP(0x2b) | (uint64_t)IN_KC[0] << 8 | IN_KS[0]);
    emit(OP(0x2a) | (uint64_t)IN_KC[1] << 24 | (uint64_t)IN_KS[1] << 16 | (uint64_t)IN_KC[2] << 8 | IN_KS[2]);
    emit(c_convert(0, 0, 0, 0, IN_K4, IN_K5));
    emit(c_fog_color(0xffffffffu));
    emit(c_blend_color(0));
}

static void combiner_scene_draw(void)
{
    static const int32_t sh[4] = {FX(0x41), FX(0x42), FX(0x43), FX(0x44)};
    static const int32_t zero[4] = {0, 0, 0, 0};
    Tri t = {0x0e, 1, 0, 0, 16, 16, 0, FX(4), 0, FX(0), 0, FX(4), 0};
    emit_tri(&t);
    emit_attr_block(sh, zero, zero, zero); /* shade */
    emit_attr_block(zero, zero, zero, zero); /* s, t, w = 0 */
}

static unsigned combiner_draw(const unsigned c0[8], const unsigned c1[8], uint32_t lo_modes, unsigned plf,
                              uint32_t *prim_index)
{
    begin("combiner");
    combiner_scene_setup(M_CYCLE2, lo_modes, plf);
    emit(c_combine(c0, c1));
    run();
    *prim_index = rdp.state->primitive_count;
    combiner_scene_draw();
    run();
    return 0;
}

static void test_combiner(void)
{
    static const unsigned slot_values[4] = {16, 16, 32, 8};
    static const unsigned alpha_zero[4] = {7, 7, 7, 7};
    static const unsigned rgb_zero[4] = {8, 8, 16, 7};
    static const char *slot_names[4] = {"a", "b", "c", "d"};
    int cycle, slot, alpha;
    unsigned v;

    for (alpha = 0; alpha < 2; alpha++)
        for (cycle = 0; cycle < 2; cycle++)
            for (slot = 0; slot < 4; slot++)
                for (v = 0; v < (alpha ? 8u : slot_values[slot]); v++) {
                    unsigned c[2][8];
                    unsigned *t = c[cycle];
                    unsigned plf = slot == 2 ? 0xa5 : 0xff;
                    uint32_t lo = 0, prim_index, got;
                    int32_t comb0[4] = {0, 0, 0, 0}, out0[4], out1[4], want[3];
                    CombState s;
                    int ch;

                    /* The tested cycle: the slot under test and probes. */
                    memcpy(t, rgb_zero, sizeof rgb_zero);
                    memcpy(t + 4, alpha_zero, sizeof alpha_zero);
                    {
                        unsigned *p = alpha ? t + 4 : t;
                        static const unsigned rgb_probe[4][4] = {
                            {0, 8, 14, 7}, {6, 0, 14, 7}, {6, 8, 0, 7}, {8, 8, 16, 0}};
                        static const unsigned alpha_probe[4][4] = {
                            {0, 7, 6, 7}, {6, 0, 6, 7}, {6, 7, 0, 7}, {7, 7, 7, 0}};
                        memcpy(p, alpha ? alpha_probe[slot] : rgb_probe[slot], 4 * sizeof(unsigned));
                        p[slot] = v;
                    }
                    /* The other cycle. */
                    if (cycle == 1) {
                        /* Cycle 0 passes PRIM colour and ENV alpha on. */
                        static const unsigned pass[8] = {8, 8, 16, 3, 7, 7, 7, 5};
                        memcpy(c[0], pass, sizeof pass);
                    } else if (!alpha) {
                        /* Cycle 1 passes COMBINED on. */
                        static const unsigned pass[8] = {8, 8, 16, 0, 7, 7, 7, 7};
                        memcpy(c[1], pass, sizeof pass);
                    } else {
                        /* Cycle 1 shows COMBINED alpha as RGB: 1.0 * it. */
                        static const unsigned pass[8] = {6, 8, 7, 7, 7, 7, 7, 7};
                        memcpy(c[1], pass, sizeof pass);
                    }
                    if (alpha && cycle == 1) {
                        /* The final alpha shows through the blender: P = fog
                         * (white), A = pixel alpha, M = blend colour (black),
                         * B = 1 - A, forced. Blender cycle 0 passes through
                         * (P = M = pixel, A = 0, B = 1). */
                        lo = L_BLEND(0, 3, 0, 2, 3, 0, 2, 0) | L_FORCE_BLEND;
                    }

                    combiner_draw(c[0], c[1], lo, plf, &prim_index);
                    got = px32(1, 1);

                    s.plf = (int32_t)plf;
                    s.noise = (int32_t)(((ref_noise(1, 1, prim_index) & 7) << 6) | 0x20);
                    s.cycle = 0;
                    s.combined = comb0;
                    ref_cycle(&s, c[0], out0);
                    s.cycle = 1;
                    s.combined = out0;
                    ref_cycle(&s, c[1], out1);
                    for (ch = 0; ch < 3; ch++)
                        want[ch] = clamp9(out1[ch]);
                    if (alpha && cycle == 1) {
                        int32_t a = clamp9(out1[3]);
                        int32_t shown = a == 0xff ? 0xff : (0xff * (a >> 3)) >> 5;
                        want[0] = want[1] = want[2] = shown;
                    }
                    snprintf(test_name, sizeof test_name, "combiner %s cycle %d slot %s = %u", alpha ? "alpha" : "rgb",
                             cycle, slot_names[slot], v);
                    current_test = test_name;
                    CHECK((got >> 8) == ((uint32_t)want[0] << 16 | (uint32_t)want[1] << 8 | (uint32_t)want[2]),
                          "got %06x want %02x%02x%02x", got >> 8, want[0], want[1], want[2]);
                }
}

/* 10. The blender: every P, A, M and B selector in the final cycle with
 * force blend, and a two-cycle chain. Inputs: pixel = PRIM (0x51,0x52,
 * 0x53; alpha 0x54), memory = (0x10, 0x20, 0x30) with coverage 5,
 * blend colour (0x91..0x94), fog (0xa1..0xa4), shade alpha 0x44. */
static const uint8_t BL_FOG[4] = {0xa1, 0xa2, 0xa3, 0xa4};
static const uint8_t BL_BLEND[4] = {0x91, 0x92, 0x93, 0x94};
static const uint8_t BL_MEM[4] = {0x10, 0x20, 0x30, 0xa0};

static const uint8_t *blend_rgb(unsigned sel, const uint8_t *pixel)
{
    switch (sel) {
    case 0: return pixel;
    case 1: return BL_MEM;
    case 2: return BL_BLEND;
    default: return BL_FOG;
    }
}

/* The blender's sum with the final-cycle shortcuts off: (P * a0 + M *
 * (a1 + 1)) >> 5 on 5-bit alphas; with B = memory alpha the alphas are
 * shifted by the dz-dependent amounts (0 and 4 without depth). */
static void ref_blend(const unsigned m[4], const uint8_t *pixel, uint8_t pixel_alpha, uint8_t out[3])
{
    int32_t a0, a1;
    int c;
    switch (m[1]) {
    case 0: a0 = pixel_alpha; break;
    case 1: a0 = BL_FOG[3]; break;
    case 2: a0 = 0x44; break;
    default: a0 = 0; break;
    }
    switch (m[3]) {
    case 0: a1 = ~a0 & 0xff; break;
    case 1: a1 = BL_MEM[3]; break;
    case 2: a1 = 0xff; break;
    default: a1 = 0; break;
    }
    a0 >>= 3;
    a1 >>= 3;
    if (m[3] == 1) {
        a0 = a0 & 0x3c;
        a1 = (a1 >> 4) | 3;
    }
    for (c = 0; c < 3; c++)
        out[c] = (uint8_t)(((blend_rgb(m[0], pixel)[c] * a0 + blend_rgb(m[2], pixel)[c] * (a1 + 1)) >> 5) & 0xff);
}

static void blender_draw(uint32_t hi, uint32_t lo)
{
    begin("blender");
    combiner_scene_setup(hi, lo | L_IMAGE_READ | L_FORCE_BLEND, 0xff);
    emit(c_combine1(CC_PRIM));
    emit(c_fog_color(n64_be32(BL_FOG)));
    emit(c_blend_color(n64_be32(BL_BLEND)));
    wr32(FB_ADDR + 4 * (FB_WIDTH + 1), n64_be32(BL_MEM));
    combiner_scene_draw();
    run();
}

static void test_blender(void)
{
    static const unsigned base[4] = {0, 0, 1, 0};
    unsigned slot, v;
    for (slot = 0; slot < 4; slot++)
        for (v = 0; v < 4; v++) {
            unsigned m[4];
            uint8_t want[3];
            uint32_t got;
            memcpy(m, base, sizeof m);
            m[slot] = v;
            blender_draw(M_CYCLE1, L_BLEND(m[0], m[1], m[2], m[3], m[0], m[1], m[2], m[3]));
            got = px32(1, 1);
            ref_blend(m, IN_PR, IN_PR[3], want);
            snprintf(test_name, sizeof test_name, "blender %s = %u", slot == 0 ? "P" : slot == 1 ? "A" : slot == 2 ? "M" : "B",
                     v);
            current_test = test_name;
            /* Coverage: min(7, memory 5 + 8) << 5. */
            CHECK(got == ((uint32_t)want[0] << 24 | (uint32_t)want[1] << 16 | (uint32_t)want[2] << 8 | 0xe0u),
                  "got %08x want %02x%02x%02xe0", got, want[0], want[1], want[2]);
        }

    /* Two cycles: cycle 0 blends memory with the blend colour by fog
     * alpha; cycle 1 blends that with fog by shade alpha. */
    {
        static const unsigned m0[4] = {1, 1, 2, 2};
        static const unsigned m1[4] = {0, 2, 3, 3};
        static const unsigned pass[8] = {8, 8, 16, 0, 7, 7, 7, 0};
        uint8_t first[3], want[3];
        uint32_t got;
        begin("blender_2cycle");
        combiner_scene_setup(M_CYCLE2, L_BLEND(m0[0], m0[1], m0[2], m0[3], m1[0], m1[1], m1[2], m1[3]) |
                                           L_IMAGE_READ | L_FORCE_BLEND, 0xff);
        emit(c_combine(CC_PRIM, pass));
        emit(c_fog_color(n64_be32(BL_FOG)));
        emit(c_blend_color(n64_be32(BL_BLEND)));
        wr32(FB_ADDR + 4 * (FB_WIDTH + 1), n64_be32(BL_MEM));
        combiner_scene_draw();
        run();
        got = px32(1, 1);
        ref_blend(m0, IN_PR, IN_PR[3], first);
        ref_blend(m1, first, IN_PR[3], want);
        current_test = "blender_2cycle";
        CHECK(got == ((uint32_t)want[0] << 24 | (uint32_t)want[1] << 16 | (uint32_t)want[2] << 8 | 0xe0u),
              "got %08x want %02x%02x%02xe0", got, want[0], want[1], want[2]);
    }
}

/* 11. Alpha compare against the blend colour's alpha, against noise, and
 * in copy mode (bit 0 of a 16-bit texel). */
static void test_alpha_compare(void)
{
    unsigned x, y;
    uint32_t prim_index;
    begin("alpha_compare");
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, L_ALPHA_TEST));
    emit(c_combine1(CC_PRIM));
    emit(c_prim_color(0, 0, 0x40404080u));
    emit(c_blend_color(0x00000081u));
    emit(c_fill_rect(2 << 2, 1 << 2, 0, 0)); /* alpha 0x80 < 0x81: rejected */
    emit(c_blend_color(0x00000080u));
    emit(c_fill_rect(4 << 2, 1 << 2, 2 << 2, 0)); /* 0x80 >= 0x80: drawn */
    run();
    CHECK(px32(0, 0) == 0 && px32(1, 0) == 0, "below threshold rejected: %08x", px32(0, 0));
    CHECK(px32(2, 0) == 0x404040e0u && px32(3, 0) == 0x404040e0u, "at threshold drawn: %08x", px32(2, 0));

    begin("alpha_compare_dither");
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, L_ALPHA_TEST | L_ALPHA_DITHER_TEST));
    emit(c_combine1(CC_PRIM));
    emit(c_prim_color(0, 0, 0x40404080u));
    run();
    prim_index = rdp.state->primitive_count;
    emit(c_fill_rect(8 << 2, 8 << 2, 0, 0));
    run();
    /* The threshold is the low byte of parallel-rdp's noise hash. */
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            int drawn = 0x80 >= (ref_noise(x, y, prim_index) & 0xff);
            CHECK(px32(x, y) == (drawn ? 0x404040e0u : 0), "(%u,%u) %08x", x, y, px32(x, y));
        }

    begin("alpha_compare_copy");
    for (x = 0; x < 4; x++)
        wr16(TEX_ADDR + 2 * x, (uint16_t)(0x1230 + x)); /* bit 0 set on odd texels */
    emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_COPY, L_ALPHA_TEST));
    emit(c_tex_image(0, 2, 4, TEX_ADDR));
    emit(c_simple_tile(0, 2, 1, 0, 0));
    emit(c_load_tile(0, 0, 0, 3 << 2, 0));
    copy_rect(4, 1, 0, 0x1000);
    run();
    for (x = 0; x < 4; x++)
        CHECK(px16(x, 0) == ((x & 1) ? 0x1230 + x : 0), "x=%u %04x", x, px16(x, 0));
}

/* 12. RGB dither into RGBA5551. A channel rounds up to the next multiple
 * of 8 where the dither value is below its low three bits; red, green and
 * blue have low bits 1, 3 and 6, so every threshold position of the magic
 * square and the Bayer matrix shows. */
static void test_dither(void)
{
    static const uint8_t magic[16] = {0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0};
    static const uint8_t bayer[16] = {0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2};
    static const unsigned low[3] = {1, 3, 6};
    unsigned mode, x, y;
    for (mode = 0; mode < 4; mode++) {
        uint32_t prim_index;
        static const char *names[4] = {"dither_magic", "dither_bayer", "dither_noise", "dither_off"};
        begin(names[mode]);
        emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
        emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
        emit(c_other_modes(M_CYCLE1 | M_RGB_DITHER(mode) | M_ALPHA_DITHER(3), 0));
        emit(c_combine1(CC_PRIM));
        emit(c_prim_color(0, 0, 0x818386ffu));
        run();
        prim_index = rdp.state->primitive_count;
        emit(c_fill_rect(4 << 2, 4 << 2, 0, 0));
        run();
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                unsigned c, d;
                uint16_t want = 1; /* full coverage */
                for (c = 0; c < 3; c++) {
                    if (mode == 0)
                        d = magic[y * 4 + x];
                    else if (mode == 1)
                        d = bayer[y * 4 + x];
                    else if (mode == 2)
                        d = ((ref_noise(x, y, prim_index) & 0x1ff) >> (3 * c)) & 7;
                    else
                        d = 8;
                    want |= (uint16_t)((d < low[c] ? 17u : 16u) << (11 - 5 * c));
                }
                CHECK(px16(x, y) == want, "(%u,%u) %04x want %04x", x, y, px16(x, y), want);
            }
    }
}

/* LOD_FRAC in one-cycle mode, without texture LOD: the LOD unit still runs
 * when the combiner reads it (Peter Lemon's Cycle1Texture* ROMs scale
 * texel alpha by it). A texture triangle stepping 1.5 texels per pixel in
 * s and none in t, with max level 3, has LOD log2(1.5): mip level 0 and a
 * fraction of (48 << 3) & 0xff = 0x80, shown here as RGB = 1.0 * LOD_FRAC.
 * A magnified texture (0.5 texels per pixel) at max level 0 reads 0xff. */
static void test_lod_frac_one_cycle(void)
{
    static const unsigned cc_lod[8] = {6, 8, 13, 7, 7, 7, 7, 6};
    static const int32_t zero[4] = {0, 0, 0, 0};
    unsigned pass, x;
    for (pass = 0; pass < 2; pass++) {
        int32_t dx[4] = {pass ? FX(16) : FX(48), 0, 0, 0};
        Tri t = {0x0a, 1, pass ? 0u : 3u, 0, 4, 4, 0, FX(4), 0, FX(0), 0, FX(4), 0};
        begin(pass ? "lod_frac_one_cycle_magnified" : "lod_frac_one_cycle");
        cycle1_tex_setup(M_BILERP0 | M_BILERP1, cc_lod);
        emit_tri(&t);
        emit_attr_block(zero, dx, zero, zero);
        run();
        for (x = 0; x < 4; x++) {
            uint32_t want = pass ? 0xffffffe0u : 0x808080e0u;
            CHECK(px32(x, 0) == want, "x=%u %08x want %08x", x, px32(x, 0), want);
        }
    }
}

/* In two-cycle mode the second cycle's TEXEL1 is the next pixel's texel0:
 * the RDP fetches it while finishing this pixel. At the end of a span of
 * three or more pixels the next pixel is the next scanline's first; on the
 * last scanline it is simply the next texel (clamped here). The span ends
 * at the pixel holding the right edge, so the rectangle's right edge is
 * 3.5: pixel 3 is covered and is the span's last (an edge at 4.0 would end
 * the span at the uncovered pixel 4). I8 texture, row 0 = 11 22 33 44,
 * row 1 = 55 66 77 88, one texel per pixel. */
static void test_two_cycle_next_texel(void)
{
    static const uint8_t tex[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    static const uint8_t want[2][4] = {{0x22, 0x33, 0x44, 0x55}, {0x66, 0x77, 0x88, 0x88}};
    static const unsigned c0[8] = {8, 8, 16, 1, 7, 7, 7, 6};
    static const unsigned c1[8] = {8, 8, 16, 2, 7, 7, 7, 6};
    unsigned x, y;
    begin("two_cycle_next_texel");
    memcpy(rdram + TEX_ADDR, tex, sizeof tex);
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_CYCLE2 | M_NO_DITHER | M_BILERP0 | M_BILERP1, 0));
    emit(c_combine(c0, c1));
    emit(c_tex_image(4, 1, 4, TEX_ADDR));
    emit(c_simple_tile(4, 1, 1, 0, 0));
    emit(c_load_tile(0, 0, 0, 3 << 2, 1 << 2));
    emit(c_rect(0x24, 14, 2 << 2, 0, 0));
    emit((uint64_t)0x400 << 16 | 0x400);
    run();
    for (y = 0; y < 2; y++)
        for (x = 0; x < 4; x++)
            /* Pixel 3 holds 4 of 8 samples: coverage (4 - 1) << 5. */
            CHECK(px32(x, y) == (want[y][x] * 0x01010100u | (x == 3 ? 0x60u : 0xe0u)), "(%u,%u) %08x want %02x", x, y,
                  px32(x, y), want[y][x]);
}

/* Perspective correction: s / w with the reciprocal table. w = 0.5
 * (0x4000) doubles s exactly; w = 1.0 (0x7fff) reads 0x2000 from the table
 * and leaves s alone. S = 1 + x texels across three pixels. */
static void test_perspective(void)
{
    static const int32_t dx[4] = {FX(32), 0, 0, 0};
    static const int32_t zero[4] = {0, 0, 0, 0};
    unsigned pass, x;
    for (pass = 0; pass < 3; pass++) {
        int32_t stw[4] = {FX(32), 0, pass == 1 ? FX(0x7fff) : FX(0x4000), 0};
        Tri t = {0x0a, 1, 0, 0, 4, 4, 0, FX(3), 0, FX(0), 0, FX(3), 0};
        static const char *names[3] = {"perspective_half_w", "perspective_unit_w", "perspective_off"};
        begin(names[pass]);
        for (x = 0; x < 8; x++)
            rdram[TEX_ADDR + x] = (uint8_t)(0x10 * x + 1);
        cycle1_tex_setup(M_BILERP0 | M_BILERP1 | (pass < 2 ? M_PERSP : 0), CC_TEXEL0);
        emit(c_tex_image(4, 1, 8, TEX_ADDR));
        emit(c_simple_tile(4, 1, 1, 0, 0));
        emit(c_load_tile(0, 0, 0, 7 << 2, 0));
        emit_tri(&t);
        emit_attr_block(stw, dx, zero, zero);
        run();
        for (x = 0; x < 3; x++) {
            unsigned texel = pass == 0 ? 2 * (1 + x) : 1 + x;
            uint32_t want = (0x10u * texel + 1) * 0x01010100u | 0xe0u;
            CHECK(px32(x, 0) == want, "x=%u %08x want %08x", x, px32(x, 0), want);
        }
    }
}

/* 13. SYNC_FULL raises the DP interrupt; the other syncs, NOP and the
 * undefined opcodes take one word and do nothing. */
static void test_syncs_and_unknown(void)
{
    begin("sync_full");
    interrupts = 0;
    emit(OP(0x26));
    emit(OP(0x27));
    emit(OP(0x28));
    emit(OP(0x29));
    run();
    CHECK(interrupts == 1, "interrupts %d", interrupts);

    begin("unknown_commands");
    emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(OP(0x00));
    emit(OP(0x01) | 0x123456789ull);
    emit(OP(0x07));
    emit(OP(0x10));
    emit(OP(0x23));
    emit(OP(0x31));
    emit(c_other_modes(M_FILL, 0));
    emit(c_fill_color(0xabcdabcdu));
    emit(c_fill_rect(0, 0, 0, 0));
    run();
    CHECK(px16(0, 0) == 0xabcd, "fill after unknown commands: %04x", px16(0, 0));
}

/* 14. The DPC registers: commands from RDRAM and from DMEM (XBUS, with
 * the 4 KiB wrap), a command split across two END writes, and freeze. */
static size_t fill_list(uint64_t *out, uint32_t color)
{
    size_t n = 0;
    out[n++] = c_color_image(0, 2, FB_WIDTH, FB_ADDR);
    out[n++] = c_scissor(0, 0, FB_WIDTH << 2, 16 << 2);
    out[n++] = c_other_modes(M_FILL, 0);
    out[n++] = c_fill_color(color);
    out[n++] = c_fill_rect(1 << 2, 0, 0, 0);
    out[n++] = OP(0x29);
    return n;
}

static void test_dpc_registers(void)
{
    uint64_t list[16];
    size_t n, i;

    begin("dpc_rdram");
    interrupts = 0;
    n = fill_list(list, 0x11111111u);
    for (i = 0; i < n; i++)
        n64_put_be64(rdram + CMD_ADDR + 8 * i, list[i]);
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_STATUS) & N64_DPC_STATUS_CBUF_READY, "idle status");
    n64_rdp_reg_write(&rdp, N64_DPC_REG_START, CMD_ADDR | 0x3000000u);
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_STATUS) & N64_DPC_STATUS_START_VALID, "start valid");
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_START) == CMD_ADDR, "start masked to 24 bits");
    n64_rdp_reg_write(&rdp, N64_DPC_REG_END, CMD_ADDR + 8 * (uint32_t)n);
    CHECK(px16(0, 0) == 0x1111 && px16(1, 0) == 0x1111, "drawn from RDRAM: %04x", px16(0, 0));
    CHECK(interrupts == 1, "sync full from RDRAM");
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_CURRENT) == CMD_ADDR + 8 * n, "current at end");
    CHECK(!(n64_rdp_reg_read(&rdp, N64_DPC_REG_STATUS) & N64_DPC_STATUS_START_VALID), "start consumed");

    begin("dpc_xbus");
    interrupts = 0;
    n = fill_list(list, 0x22222222u);
    /* The list starts in the last word of DMEM and wraps to its start. */
    n64_put_be64(dmem + 0xff8, list[0]);
    for (i = 1; i < n; i++)
        n64_put_be64(dmem + 8 * (i - 1), list[i]);
    n64_rdp_reg_write(&rdp, N64_DPC_REG_STATUS, 0x002);
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_STATUS) & N64_DPC_STATUS_XBUS, "xbus set");
    n64_rdp_reg_write(&rdp, N64_DPC_REG_START, 0xff8);
    n64_rdp_reg_write(&rdp, N64_DPC_REG_END, 0xff8 + 8 * (uint32_t)n);
    CHECK(px16(0, 0) == 0x2222, "drawn from DMEM: %04x", px16(0, 0));
    CHECK(interrupts == 1, "sync full from DMEM");
    n64_rdp_reg_write(&rdp, N64_DPC_REG_STATUS, 0x001);
    CHECK(!(n64_rdp_reg_read(&rdp, N64_DPC_REG_STATUS) & N64_DPC_STATUS_XBUS), "xbus cleared");

    begin("dpc_split_command");
    {
        /* A fill-mode triangle (four words) split after its second. */
        Tri t = {0x08, 1, 0, 0, 32, 32, 0, FX(4), 0, FX(0), 0, FX(4), 0};
        uint32_t a = CMD_ADDR;
        ncmds = 0;
        emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
        emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
        emit(c_other_modes(M_FILL, 0));
        emit(c_fill_color(0x33333333u));
        emit_tri(&t);
        for (i = 0; i < ncmds; i++)
            n64_put_be64(rdram + a + 8 * i, cmds[i]);
        n = ncmds;
        ncmds = 0;
        n64_rdp_reg_write(&rdp, N64_DPC_REG_START, a);
        n64_rdp_reg_write(&rdp, N64_DPC_REG_END, a + 8 * (uint32_t)(n - 2));
        CHECK(px16(0, 0) == 0, "half a triangle draws nothing");
        CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_CURRENT) == a + 8 * (n - 2), "current after the first part");
        n64_rdp_reg_write(&rdp, N64_DPC_REG_END, a + 8 * (uint32_t)n);
        /* Fill mode spans are inclusive: x 0..4 on lines 0..7. */
        CHECK(px16(0, 0) == 0x3333 && px16(4, 7) == 0x3333 && px16(5, 0) == 0 && px16(0, 8) == 0,
              "completed triangle: %04x %04x %04x", px16(0, 0), px16(4, 7), px16(5, 0));
    }

    begin("dpc_freeze");
    n = fill_list(list, 0x44444444u);
    for (i = 0; i < n; i++)
        n64_put_be64(rdram + CMD_ADDR + 8 * i, list[i]);
    n64_rdp_reg_write(&rdp, N64_DPC_REG_STATUS, 0x008);
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_STATUS) & N64_DPC_STATUS_FREEZE, "frozen");
    n64_rdp_reg_write(&rdp, N64_DPC_REG_START, CMD_ADDR);
    n64_rdp_reg_write(&rdp, N64_DPC_REG_END, CMD_ADDR + 8 * (uint32_t)n);
    CHECK(px16(0, 0) == 0, "frozen: nothing runs");
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_CURRENT) == CMD_ADDR, "frozen: current at start");
    n64_rdp_reg_write(&rdp, N64_DPC_REG_STATUS, 0x004);
    CHECK(px16(0, 0) == 0x4444, "unfrozen: commands ran: %04x", px16(0, 0));

    begin("dpc_counters");
    n64_rdp_reg_write(&rdp, N64_DPC_REG_STATUS, 0x3c0 | 0x020);
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_CLOCK) == 0 && n64_rdp_reg_read(&rdp, N64_DPC_REG_BUFBUSY) == 0 &&
              n64_rdp_reg_read(&rdp, N64_DPC_REG_PIPEBUSY) == 0 && n64_rdp_reg_read(&rdp, N64_DPC_REG_TMEM) == 0,
          "counters cleared");
    CHECK(n64_rdp_reg_read(&rdp, N64_DPC_REG_STATUS) & N64_DPC_STATUS_FLUSH, "flush set");
    n64_rdp_reg_write(&rdp, N64_DPC_REG_STATUS, 0x010);
    CHECK(!(n64_rdp_reg_read(&rdp, N64_DPC_REG_STATUS) & N64_DPC_STATUS_FLUSH), "flush cleared");
}

/* 15. Same commands, same bytes: two machines in one process agree. */
static void test_determinism(void)
{
    uint8_t *copy = (uint8_t *)malloc(RDRAM_SIZE);
    begin("determinism");
    if (!copy) {
        CHECK(0, "out of memory");
        return;
    }
    test_combiner(); /* leaves the last combiner frame in RDRAM */
    memcpy(copy, rdram, RDRAM_SIZE);
    test_combiner();
    current_test = "determinism";
    CHECK(memcmp(copy, rdram, RDRAM_SIZE) == 0, "repeat run differs");
    free(copy);
}

/* 16. The same bytes however they are drawn. Generated command lists -
 * every cycle type, mode bit, combiner and blender selector, tile format
 * and load, on colour images of every size, with the depth image apart
 * from, on top of and overlapping the colour image, in and out of RDRAM -
 * are run through the pixel-by-pixel reference path on one thread, and
 * then through the rasteriser's shortcuts on one thread and on several.
 * RDRAM, the hidden bits and the RDP's state must match byte for byte.
 * The lists are random but seeded, so a failure names a scene that can be
 * run again. */
static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static uint32_t rnd_below(uint32_t n) { return rnd() % n; }
static int chance(unsigned percent) { return rnd_below(100) < percent; }
static int32_t rnd_between(int32_t lo, int32_t hi) { return lo + (int32_t)rnd_below((uint32_t)(hi - lo) + 1u); }

typedef struct Scene {
    unsigned width, height;
    uint32_t fb_addr;
} Scene;

static void scene_color_image(Scene *sc)
{
    static const unsigned widths[6] = {320, 320, 320, 64, 640, 200};
    static const unsigned heights[6] = {240, 240, 120, 48, 100, 150};
    unsigned shape = rnd_below(6), kind = rnd_below(20);
    sc->width = widths[shape];
    sc->height = heights[shape];
    /* Mostly in RDRAM; sometimes across its end, or across the end of the
     * 24-bit address space. */
    sc->fb_addr = chance(88) ? FB_ADDR + 2 * rnd_below(64) : chance(50) ? RDRAM_SIZE - 0x9000u : 0xfff000u;
    if (kind < 12)
        emit(c_color_image(0, 2, sc->width, sc->fb_addr)); /* RGBA5551 */
    else if (kind < 15)
        emit(c_color_image(0, 3, sc->width, sc->fb_addr)); /* RGBA8888 */
    else if (kind < 17)
        emit(c_color_image(3, 2, sc->width, sc->fb_addr)); /* IA88 */
    else if (kind < 19)
        emit(c_color_image(0, 1, sc->width, sc->fb_addr + rnd_below(2))); /* 8-bit */
    else
        emit(c_color_image(0, 0, sc->width, sc->fb_addr)); /* 4-bit */
    switch (rnd_below(12)) {
    case 0: emit(c_z_image(sc->fb_addr)); break;                          /* on the colour image */
    case 1: emit(c_z_image(sc->fb_addr + 2 * rnd_below(0x2000))); break;  /* overlapping it */
    case 2: emit(c_z_image(RDRAM_SIZE - 0x4000u)); break;                 /* across the end of RDRAM */
    default: emit(c_z_image(Z_ADDR + 2 * rnd_below(16))); break;
    }
}

static void scene_scissor(const Scene *sc)
{
    unsigned xh = 0, yh = 0, xl = sc->width << 2, yl = sc->height << 2;
    if (chance(30)) {
        xh = rnd_below(sc->width << 1);
        yh = rnd_below(sc->height << 1);
        xl = xh + rnd_below((sc->width << 2) - xh + 40);
        yl = yh + rnd_below((sc->height << 2) - yh + 40);
    }
    /* Bits 25 and 24: draw one field only. */
    emit(c_scissor(xh, yh, xl & 0xfff, yl & 0xfff) | (chance(8) ? (uint64_t)(2 + rnd_below(2)) << 24 : 0));
}

static void scene_modes(void)
{
    static const uint32_t cycles[12] = {M_CYCLE1, M_CYCLE1, M_CYCLE1, M_CYCLE1, M_CYCLE2, M_CYCLE2,
                                        M_CYCLE2, M_CYCLE2, M_CYCLE2, M_CYCLE2, M_COPY,   M_FILL};
    uint32_t hi = cycles[rnd_below(12)] | (rnd() & 0x0ffef0u), lo = rnd();
    unsigned kind = rnd_below(10);
    if (kind < 3) {
        /* What games draw with: filtered texels, one or two cycles, with
         * or without perspective, depth, AA and blending. */
        hi = (chance(60) ? M_CYCLE2 : M_CYCLE1) | M_SAMPLE_QUAD | M_BILERP0 | M_BILERP1 | (rnd() & (M_PERSP | 0xf0u));
        lo = (rnd() & 0xffff0000u) |
             (rnd() & (L_FORCE_BLEND | L_IMAGE_READ | L_Z_UPDATE | L_Z_COMPARE | L_AA | 0xf00u));
    } else if (kind < 6) {
        /* Modes most pixels survive: no alpha compare, no
         * colour-on-coverage, and no palette lookups of texels that are
         * not indices. */
        hi &= ~(M_TLUT | M_TLUT_IA);
        lo &= ~(L_ALPHA_TEST | L_ALPHA_DITHER_TEST | (1u << 7) | (1u << 13) | (1u << 12));
    }
    emit(c_other_modes(hi, lo));
}

static void scene_combine(void)
{
    /* Texel0 times shade; texel1 alone (in one cycle, the next pixel's
     * texel); a blend of the two texels by the LOD fraction, by the
     * environment colour and by texel1's alpha; and the second cycle of
     * each: combined times shade, texel1, or combined alone. */
    static const unsigned first[5][8] = {{1, 8, 4, 7, 1, 7, 4, 7},
                                         {8, 8, 16, 2, 7, 7, 7, 2},
                                         {2, 1, 13, 1, 2, 1, 0, 1},
                                         {2, 1, 5, 1, 2, 1, 5, 1},
                                         {1, 2, 9, 2, 7, 7, 7, 1}};
    static const unsigned second[3][8] = {{0, 8, 4, 7, 0, 7, 4, 7},
                                          {8, 8, 16, 2, 7, 7, 7, 2},
                                          {8, 8, 16, 0, 7, 7, 7, 0}};
    unsigned c[2][8], i;
    if (chance(35)) {
        /* One cycle draws with the second set of selectors. */
        emit(c_combine(first[rnd_below(5)], chance(50) ? first[rnd_below(5)] : second[rnd_below(3)]));
        return;
    }
    for (i = 0; i < 2; i++) {
        c[i][0] = rnd_below(16);
        c[i][1] = rnd_below(16);
        c[i][2] = rnd_below(32);
        c[i][3] = rnd_below(8);
        c[i][4] = rnd_below(8);
        c[i][5] = rnd_below(8);
        c[i][6] = rnd_below(8);
        c[i][7] = rnd_below(8);
    }
    emit(c_combine(c[0], c[1]));
}

static void scene_colors(void)
{
    switch (rnd_below(8)) {
    case 0: emit(c_fill_color(rnd())); break;
    case 1: emit(c_fog_color(rnd())); break;
    case 2: emit(c_blend_color(rnd())); break;
    case 3: emit(c_prim_color(rnd_below(32), rnd_below(256), rnd())); break;
    case 4: emit(c_env_color(rnd())); break;
    case 5: emit(c_prim_depth(rnd_below(0x8000), chance(50) ? 1u << rnd_below(16) : rnd_below(0x10000))); break;
    case 6:
        emit(c_convert(rnd_below(512), rnd_below(512), rnd_below(512), rnd_below(512), rnd_below(512),
                       rnd_below(512)));
        break;
    default:
        emit(OP(0x2a) | (uint64_t)rnd_below(0x1000000) << 32 | rnd()); /* SET_KEY_GB */
        emit(OP(0x2b) | (rnd() & 0x0fffffffu));                        /* SET_KEY_R */
        break;
    }
}

static void scene_tile(void)
{
    unsigned tile = rnd_below(8), fmt = chance(95) ? rnd_below(5) : 5 + rnd_below(3), size = rnd_below(4);
    unsigned sl = rnd_below(64), tl = rnd_below(64);
    emit(c_tile(fmt, size, rnd_below(chance(80) ? 24 : 512), rnd_below(chance(70) ? 128 : 512), tile, rnd_below(16),
                rnd_below(4), rnd_below(chance(80) ? 8 : 16), chance(70) ? 0 : rnd_below(16), rnd_below(4),
                rnd_below(chance(80) ? 8 : 16), chance(70) ? 0 : rnd_below(16)));
    if (chance(60))
        emit(c_tile_size(tile, sl, tl, sl + rnd_below(1024), tl + rnd_below(1024)));
}

/* A load, from the texture area or (to read back what was just drawn)
 * from the colour or depth image. */
static void scene_load(const Scene *sc)
{
    unsigned tile = rnd_below(8), size = 1 + rnd_below(3), width = 1 + rnd_below(chance(80) ? 64 : 1024);
    uint32_t addr = chance(75)   ? TEX_ADDR + rnd_below(0x8000)
                    : chance(50) ? sc->fb_addr + rnd_below(sc->width * sc->height)
                                 : Z_ADDR + rnd_below(0x8000);
    if (chance(90))
        addr &= ~1u;
    if (chance(5))
        size = 0;
    emit(c_tex_image(rnd_below(5), size, width, addr));
    switch (rnd_below(5)) {
    case 0: {
        unsigned sl = rnd_below(64);
        emit(c_load_tlut(tile, sl << 2, (sl + rnd_below(256)) << 2));
        break;
    }
    case 1:
    case 2: {
        unsigned sl = rnd_below(32), tl = rnd_below(32);
        emit(c_load_tile(tile, sl << 2, tl << 2, (sl + rnd_below(chance(80) ? 64 : 400)) << 2,
                         (tl + rnd_below(chance(80) ? 64 : 400)) << 2));
        break;
    }
    default: {
        unsigned texels = 1 + rnd_below(chance(80) ? 1024 : 4096);
        unsigned dxt = chance(60) ? 0x800u / (1 + rnd_below(64)) : rnd_below(0x1000);
        emit(c_load_block(tile, rnd_below(4), rnd_below(4), (texels - 1) & 0xfff, dxt));
        break;
    }
    }
}

static void scene_blocks(unsigned op, int wild)
{
    int32_t v[4], dx[4], de[4], dy[4];
    unsigned i;
    if (op & 4) {
        for (i = 0; i < 4; i++) {
            v[i] = wild ? (int32_t)rnd() : rnd_between(-16, 272) * 65536 + (int32_t)rnd_below(65536);
            dx[i] = wild ? (int32_t)rnd() : rnd_between(-0x30000, 0x30000);
            de[i] = wild ? (int32_t)rnd() : rnd_between(-0x30000, 0x30000);
            dy[i] = wild ? (int32_t)rnd() : rnd_between(-0x30000, 0x30000);
        }
        emit_attr_block(v, dx, de, dy);
    }
    if (op & 2) {
        /* s, t and w: w is positive and large unless the list is wild, so
         * that the perspective divide stays in range. */
        /* A texel is 32 units of s or t: from a fraction of a texel a pixel
         * to several. */
        int32_t scale = 1 << rnd_below(8);
        for (i = 0; i < 4; i++) {
            v[i] = wild ? (int32_t)rnd() : rnd_between(-0x400, 0x2000) * 65536 + (int32_t)rnd_below(65536);
            dx[i] = wild ? (int32_t)rnd() : rnd_between(-0x80000, 0x80000) * scale;
            de[i] = wild ? (int32_t)rnd() : rnd_between(-0x80000, 0x80000) * scale;
            dy[i] = wild ? (int32_t)rnd() : rnd_between(-0x80000, 0x80000) * scale;
        }
        if (!wild) {
            v[2] = rnd_between(0x1000, 0x7fff) * 65536;
            dx[2] = rnd_between(-0x80000, 0x80000);
            de[2] = rnd_between(-0x80000, 0x80000);
            dy[2] = rnd_between(-0x80000, 0x80000);
        }
        emit_attr_block(v, dx, de, dy);
    }
    if (op & 1) {
        uint32_t z = wild ? rnd() : rnd_below(0x7fff) << 16 | rnd_below(65536);
        uint32_t dzdx = wild ? rnd() : (uint32_t)rnd_between(-0x200000, 0x200000);
        emit((uint64_t)z << 32 | dzdx);
        emit((uint64_t)(wild ? rnd() : (uint32_t)rnd_between(-0x200000, 0x200000)) << 32 |
             (wild ? rnd() : (uint32_t)rnd_between(-0x200000, 0x200000)));
    }
}

/* A triangle through three points on or a little off the image, with the
 * edge coefficients a display list would give it. */
static void scene_triangle(const Scene *sc)
{
    Tri t;
    int32_t x[3], y[3], i, j, major;
    unsigned op = 0x08 + rnd_below(8);
    int wild = chance(6);
    for (i = 0; i < 3; i++) {
        x[i] = rnd_between(-80, (int32_t)sc->width * 4 + 80); /* quarter pixels */
        y[i] = rnd_between(-40, (int32_t)sc->height * 4 + 40);
    }
    for (i = 0; i < 3; i++)
        for (j = i + 1; j < 3; j++)
            if (y[j] < y[i]) {
                int32_t tx = x[i], ty = y[i];
                x[i] = x[j];
                y[i] = y[j];
                x[j] = tx;
                y[j] = ty;
            }
    memset(&t, 0, sizeof t);
    t.op = op;
    t.level = rnd_below(8);
    t.tile = rnd_below(8);
    t.yh = y[0];
    t.ym = y[1];
    t.yl = y[2];
    /* Slopes in s15.16 pixels per scanline; x in quarter pixels is << 14. */
    t.dxhdy = (int32_t)(((int64_t)(x[2] - x[0]) << 16) / (y[2] - y[0] ? y[2] - y[0] : 1));
    t.dxmdy = (int32_t)(((int64_t)(x[1] - x[0]) << 16) / (y[1] - y[0] ? y[1] - y[0] : 1));
    t.dxldy = (int32_t)(((int64_t)(x[2] - x[1]) << 16) / (y[2] - y[1] ? y[2] - y[1] : 1));
    t.xh = x[0] * 16384 - (y[0] & 3) * (t.dxhdy / 4);
    t.xm = x[0] * 16384 - (y[0] & 3) * (t.dxmdy / 4);
    t.xl = x[1] * 16384;
    major = x[0] + (int32_t)(((int64_t)(x[2] - x[0]) * (y[1] - y[0])) / (y[2] - y[0] ? y[2] - y[0] : 1));
    t.lft = major < x[1];
    if (wild) {
        t.xh = (int32_t)rnd();
        t.dxhdy = (int32_t)rnd() / (1 << rnd_below(16));
        t.lft = rnd_below(2);
    }
    emit_tri(&t);
    scene_blocks(op, wild);
}

static void scene_rectangle(const Scene *sc)
{
    unsigned xh = rnd_below(sc->width << 2), yh = rnd_below(sc->height << 2);
    unsigned xl = xh + rnd_below((sc->width << 2) - xh + 16), yl = yh + rnd_below((sc->height << 2) - yh + 16);
    unsigned kind = rnd_below(4);
    if (kind == 0) {
        emit(c_fill_rect(xl & 0xfff, yl & 0xfff, xh, yh));
    } else {
        emit(c_rect(kind == 1 ? 0x25 : 0x24, xl & 0xfff, yl & 0xfff, xh, yh) | (uint64_t)rnd_below(8) << 24);
        /* s, t, dsdx, dtdy: mostly one texel a pixel (or four, as the copy
         * pipe wants). */
        emit((uint64_t)rnd_below(0x10000) << 48 | (uint64_t)rnd_below(0x10000) << 32 |
             (uint64_t)(chance(70) ? 0x400u << (2 * rnd_below(2)) : rnd_below(0x10000)) << 16 |
             (chance(70) ? 0x400u : rnd_below(0x10000)));
    }
}

/* Magnified, mirrored textures, stepped across the scanline (s) and, in the
 * flipped rectangle, t across it: neighbouring pixels share their texels
 * until the coordinate turns round at a mirror edge, where the texel
 * beside the base one changes sides while the base stays. */
static void scene_mirror_strips(void)
{
    unsigned tile = rnd_below(8);
    emit(c_color_image(0, 2, 320, FB_ADDR));
    emit(c_z_image(Z_ADDR));
    emit(c_scissor(0, 0, 320 << 2, 240 << 2));
    emit(c_other_modes(M_CYCLE1 | M_SAMPLE_QUAD | M_BILERP0 | M_BILERP1 | M_NO_DITHER, 0));
    emit(c_combine1(CC_TEXEL0));
    emit(c_tex_image(0, 2, 8, TEX_ADDR + 2 * rnd_below(256)));
    emit(c_tile(0, 2, 2, 0, tile, 0, 1, 3, 0, 1, 3, 0));
    emit(c_load_tile(tile, 0, 0, 7 << 2, 7 << 2));
    emit(c_rect(0x24, 300 << 2, 40 << 2, 20 << 2, 20 << 2) | (uint64_t)tile << 24);
    emit((uint64_t)rnd_below(0x400) << 48 | (uint64_t)rnd_below(0x400) << 32 | (uint64_t)0x100 << 16 | 0x100);
    emit(c_rect(0x25, 300 << 2, 80 << 2, 20 << 2, 60 << 2) | (uint64_t)tile << 24);
    emit((uint64_t)rnd_below(0x400) << 48 | (uint64_t)rnd_below(0x400) << 32 | (uint64_t)0x100 << 16 | 0x100);
}

static void build_scene(uint32_t seed)
{
    Scene sc;
    unsigned i, primitives = 24 + (seed % 5) * 6;
    rng_state = seed * 2654435761u + 1u;
    if (seed % 8 == 0)
        scene_mirror_strips();
    scene_color_image(&sc);
    scene_scissor(&sc);
    scene_modes();
    scene_combine();
    for (i = 0; i < 8; i++)
        scene_colors();
    for (i = 0; i < 12; i++)
        scene_tile();
    for (i = 0; i < 4; i++)
        scene_load(&sc);
    for (i = 0; i < primitives; i++) {
        if (chance(45))
            scene_modes();
        if (chance(45))
            scene_combine();
        if (chance(40))
            scene_colors();
        if (chance(25))
            scene_tile();
        if (chance(30))
            scene_load(&sc);
        if (chance(6))
            scene_scissor(&sc);
        if (chance(3)) {
            scene_color_image(&sc);
            scene_scissor(&sc);
        }
        if (chance(2))
            emit(OP(0x29)); /* SYNC_FULL */
        if (chance(70))
            scene_triangle(&sc);
        else
            scene_rectangle(&sc);
    }
}

/* Memory the scenes draw on and load from: not zero, so that the blender
 * and the depth test have something to read. */
static void scene_memory(uint32_t seed)
{
    static const uint32_t areas[5][2] = {{FB_ADDR, 0x60000},
                                         {Z_ADDR, 0x30000},
                                         {TEX_ADDR, 0x10000},
                                         {RDRAM_SIZE - 0x9000u, 0x9000},
                                         {0, 0}};
    unsigned a;
    uint32_t i;
    rng_state = seed ^ 0x9e3779b9u;
    for (a = 0; areas[a][1]; a++)
        for (i = 0; i < areas[a][1]; i += 4)
            wr32(areas[a][0] + i, rnd());
}

/* 17. A load's layout is remembered and replayed (n64_rdp_tmem.c): the
 * same tile and load parameters come back with other texture addresses,
 * odd ones included, and other bytes in RDRAM, and TMEM must match the
 * gather over every word, which the reference path does each time. */
static void load_round(uint32_t round, int reference)
{
    unsigned i;
    begin(test_name);
    scene_memory(round);
    n64_rdp_set_reference(reference);
    rng_state = round * 0x9e3779b1u + 7u;
    for (i = 0; i < 160; i++) {
        /* One of 96 layouts, with whatever address this round gives it. */
        uint32_t addr = TEX_ADDR + rnd_below(0x8000), keep = rnd();
        unsigned tile, fmt, size, texsize, width;
        rng_state = (keep % 96u) * 0x85ebca6bu + 3u;
        tile = rnd_below(8);
        fmt = rnd_below(5);
        size = rnd_below(4);
        texsize = chance(5) ? 0 : 1 + rnd_below(3);
        width = 1 + rnd_below(chance(80) ? 64 : 1024);
        emit(c_tile(fmt, size, rnd_below(chance(80) ? 24 : 512), rnd_below(chance(70) ? 128 : 512), tile, rnd_below(16),
                    0, 0, 0, 0, 0, 0));
        emit(c_tex_image(rnd_below(5), texsize, width, chance(85) ? addr & ~1u : addr));
        switch (rnd_below(5)) {
        case 0: {
            unsigned sl = rnd_below(64);
            emit(c_load_tlut(tile, sl << 2, (sl + rnd_below(256)) << 2));
            break;
        }
        case 1:
        case 2: {
            unsigned sl = rnd_below(32), tl = rnd_below(32);
            emit(c_load_tile(tile, sl << 2, tl << 2, (sl + rnd_below(chance(80) ? 64 : 400)) << 2,
                             (tl + rnd_below(chance(80) ? 64 : 400)) << 2));
            break;
        }
        default: {
            unsigned texels = 1 + rnd_below(chance(80) ? 1024 : 4096);
            unsigned dxt = chance(60) ? 0x800u / (1 + rnd_below(64)) : rnd_below(0x1000);
            emit(c_load_block(tile, rnd_below(4), rnd_below(4), (texels - 1) & 0xfff, dxt));
            break;
        }
        }
        rng_state = keep;
    }
    run();
}

static void test_load_plans(void)
{
    N64RdpState *want = (N64RdpState *)malloc(sizeof *want);
    uint32_t round;
    if (!want) {
        begin("load_plans");
        CHECK(0, "out of memory");
        return;
    }
    for (round = 1; round <= 24; round++) {
        unsigned word = 0;
        snprintf(test_name, sizeof test_name, "load_plans round %u", (unsigned)round);
        load_round(round, 1);
        memcpy(want, rdp.state, sizeof *want);
        load_round(round, 0);
        while (word < 2048 && memcmp(want->tmem + 2 * word, rdp.state->tmem + 2 * word, 2) == 0)
            word++;
        CHECK(word == 2048, "TMEM word %u differs from the gather", word);
        CHECK(memcmp(want->tiles, rdp.state->tiles, sizeof want->tiles) == 0, "tiles differ");
    }
    n64_rdp_set_reference(0);
    free(want);
}

static void test_paths_agree(void)
{
    /* Threads for each run after the reference: the shortcuts alone, then
     * with helpers, then with more threads than most hosts have cores. */
    static const int threads[3] = {1, 4, 16};
    uint8_t *want = (uint8_t *)malloc(RDRAM_SIZE), *want_hidden = (uint8_t *)malloc(RDRAM_SIZE / 2);
    N64RdpState *want_state = (N64RdpState *)malloc(sizeof *want_state);
    uint32_t seed;
    unsigned run_index;

    if (!want || !want_hidden || !want_state) {
        begin("paths_agree");
        CHECK(0, "out of memory");
        return;
    }
    for (seed = 1; seed <= 40; seed++) {
        snprintf(test_name, sizeof test_name, "paths_agree scene %u", (unsigned)seed);
        for (run_index = 0; run_index < 4; run_index++) {
            uint32_t at = 0;
            begin(test_name);
            scene_memory(seed);
            build_scene(seed);
            n64_rdp_set_reference(run_index == 0);
            n64_rdp_set_threads(run_index == 0 ? 1 : threads[run_index - 1]);
            run();
            if (run_index == 0) {
                memcpy(want, rdram, RDRAM_SIZE);
                memcpy(want_hidden, rdp.links.hidden, RDRAM_SIZE / 2);
                memcpy(want_state, rdp.state, sizeof *want_state);
                continue;
            }
            if (memcmp(want, rdram, RDRAM_SIZE) != 0)
                while (want[at] == rdram[at])
                    at++;
            CHECK(memcmp(want, rdram, RDRAM_SIZE) == 0, "%d threads: RDRAM differs from the reference path at %06x",
                  threads[run_index - 1], (unsigned)at);
            CHECK(memcmp(want_hidden, rdp.links.hidden, RDRAM_SIZE / 2) == 0,
                  "%d threads: hidden bits differ from the reference path", threads[run_index - 1]);
            CHECK(memcmp(want_state, rdp.state, offsetof(N64RdpState, spans)) == 0,
                  "%d threads: RDP state differs from the reference path", threads[run_index - 1]);
        }
    }
    n64_rdp_set_reference(0);
    n64_rdp_set_threads(0);
    free(want);
    free(want_hidden);
    free(want_state);
}

/* ---- the larger picture (n64_rdp_set_scale) ---- */

/* Sample (sx, sy) of the `bytes`-byte pixel (x, y) of the image at FB_ADDR,
 * or NULL where the larger picture is out of step with RDRAM. */
static const uint8_t *larger_at(unsigned x, unsigned y, unsigned sx, unsigned sy, unsigned bytes)
{
    size_t stride = 0;
    const uint8_t *p = n64_rdp_larger_pixel(&rdp, FB_ADDR + bytes * (y * FB_WIDTH + x), bytes, &stride);
    return p ? p + ((size_t)sy * (size_t)n64_rdp_scale(&rdp) + sx) * stride : NULL;
}

/* How many of pixel (x, y)'s samples hold `rgb` in a 32-bit image; -1 where
 * the pixel is out of step. */
static int larger_count(unsigned x, unsigned y, uint32_t rgb)
{
    const unsigned scale = (unsigned)n64_rdp_scale(&rdp);
    unsigned sx, sy;
    int count = 0;
    if (!larger_at(x, y, 0, 0, 4))
        return -1;
    for (sy = 0; sy < scale; sy++)
        for (sx = 0; sx < scale; sx++)
            count += (n64_be32(larger_at(x, y, sx, sy, 4)) >> 8) == rgb;
    return count;
}

static void flat_triangle_scene(void)
{
    Tri t = {0x08, 1, 0, 0, 24, 12, 0, FX(4.75), FX(-0.5), FX(1.25), FX(0.5), FX(2.5), FX(0.75)};
    emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, 0));
    emit(c_combine1(CC_PRIM));
    emit(c_prim_color(0, 0, 0x336699ffu));
    emit_tri(&t);
    run();
}

/* 18. The larger picture by its rules: which scales there are; a
 * triangle's edge at the finer grain; attributes stepped by parts of a
 * pixel; rectangles as the console draws them; and pixels the game wrote,
 * which have no larger picture until the RDP draws on them. */
static void test_larger_rules(void)
{
    unsigned x, y, sx, sy, scale;

    begin("larger_scales");
    CHECK(n64_rdp_scale(&rdp) == 1, "the scale starts at %d", n64_rdp_scale(&rdp));
    CHECK(!larger_at(0, 0, 0, 0, 2), "no larger picture at a scale of 1");
    CHECK(n64_rdp_set_scale(&rdp, 2) && n64_rdp_scale(&rdp) == 2, "2 is a scale");
    CHECK(n64_rdp_set_scale(&rdp, 4) && n64_rdp_scale(&rdp) == 4, "4 is a scale");
    CHECK(!n64_rdp_set_scale(&rdp, 3) && n64_rdp_scale(&rdp) == 1, "3 is not, and leaves none");
    CHECK(n64_rdp_set_scale(&rdp, 4), "4 again");
    n64_rdp_reset(&rdp);
    CHECK(n64_rdp_scale(&rdp) == 4, "a reset keeps the scale: %d", n64_rdp_scale(&rdp));
    CHECK(n64_rdp_set_scale(&rdp, 1) && n64_rdp_scale(&rdp) == 1, "and 1 is none");

    /* The triangle of flat_triangle_edges: 6.875 pixels. Without
     * anti-aliasing a pixel is drawn when its first sample, at its top
     * left, is inside; a sample of the larger picture is such a pixel a
     * fraction of the way along, so sample (0, 0) is drawn exactly where
     * the console's pixel is, and the samples drawn come to the area. */
    for (scale = 2; scale <= 4; scale += 2) {
        static const uint8_t rows[6][2] = {{2, 2}, {2, 3}, {3, 3}, {3, 4}, {4, 4}, {1, 0}};
        int total = 0, partial = 0;
        snprintf(test_name, sizeof test_name, "larger_flat_edges %ux", scale);
        n64_rdp_set_scale(&rdp, (int)scale);
        begin(test_name);
        flat_triangle_scene();
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                int inside = y < 6 && x >= rows[y][0] && x <= rows[y][1];
                int count = larger_count(x, y, 0x336699u);
                CHECK(((px32(x, y) & 0xffffff00u) == (inside ? 0x33669900u : 0)), "the console's (%u,%u) %08x", x, y,
                      px32(x, y));
                if (count < 0) {
                    CHECK(!inside, "(%u,%u) is drawn and out of step", x, y);
                    continue;
                }
                CHECK(((n64_be32(larger_at(x, y, 0, 0, 4)) >> 8) == 0x336699u) == inside,
                      "(%u,%u): sample (0,0) is not the console's pixel", x, y);
                total += count;
                partial += count > 0 && count < (int)(scale * scale);
            }
        CHECK(total >= (int)(6.875 * scale * scale) - (int)(2 * scale) &&
                  total <= (int)(6.875 * scale * scale) + (int)(2 * scale),
              "%d samples drawn, for an area of %.1f", total, 6.875 * scale * scale);
        CHECK(partial >= 4, "%d pixels are partly drawn: the edge is no finer", partial);
    }

    /* Gouraud shading (gouraud_triangle): R = 16 + 1.5 x and G = 32 + 2 y,
     * now at quarters of a pixel. */
    {
        static const int32_t v[4] = {FX(16), FX(32), FX(100), FX(255)};
        static const int32_t dx[4] = {FX(1.5), 0, 0, 0};
        static const int32_t de[4] = {0, FX(2), 0, 0};
        static const int32_t dy[4] = {0, FX(2), 0, 0};
        Tri t = {0x0c, 1, 0, 0, 32, 32, 0, FX(8), 0, FX(0), 0, FX(8), 0};
        n64_rdp_set_scale(&rdp, 4);
        begin("larger_gouraud");
        emit(c_color_image(0, 3, FB_WIDTH, FB_ADDR));
        emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
        emit(c_other_modes(M_CYCLE1 | M_NO_DITHER, 0));
        emit(c_combine1(CC_SHADE));
        emit_tri(&t);
        emit_attr_block(v, dx, de, dy);
        run();
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                for (sy = 0; sy < 4; sy++)
                    for (sx = 0; sx < 4; sx++) {
                        const uint8_t *p = larger_at(x, y, sx, sy, 4);
                        uint32_t r = 16 + (3 * (4 * x + sx)) / 8, g = 32 + (4 * y + sy) / 2;
                        uint32_t want = r << 24 | g << 16 | 100u << 8 | 0xe0u;
                        CHECK(p && n64_be32(p) == want, "(%u,%u) sample (%u,%u) %08x want %08x", x, y, sx, sy,
                              p ? n64_be32(p) : 0u, want);
                    }
        CHECK(px32(3, 5) == (20u << 24 | 42u << 16 | 100u << 8 | 0xe0u), "the console's (3,5) %08x", px32(3, 5));
    }

    /* Rectangles are the console's own in every sample, and a pixel the
     * game writes has no larger picture until the RDP draws on it. */
    n64_rdp_set_scale(&rdp, 2);
    begin("larger_out_of_step");
    emit(c_color_image(0, 2, FB_WIDTH, FB_ADDR));
    emit(c_scissor(0, 0, FB_WIDTH << 2, 16 << 2));
    emit(c_other_modes(M_FILL, 0));
    emit(c_fill_color(0x12351235u));
    emit(c_fill_rect(7 << 2, 7 << 2, 0, 0));
    run();
    for (sy = 0; sy < 2; sy++)
        for (sx = 0; sx < 2; sx++) {
            const uint8_t *p = larger_at(3, 2, sx, sy, 2);
            CHECK(p && n64_be16(p) == 0x1235, "the fill in sample (%u,%u): %04x", sx, sy, p ? n64_be16(p) : 0u);
        }
    CHECK(!larger_at(9, 2, 0, 0, 2) || n64_be16(larger_at(9, 2, 0, 0, 2)) == 0, "beyond the rectangle");
    wr16(FB_ADDR + 2 * (2 * FB_WIDTH + 3), 0xf801);
    CHECK(!larger_at(3, 2, 0, 0, 2), "a pixel the game wrote has a larger picture");
    CHECK(larger_at(4, 2, 0, 0, 2) != NULL, "and so its neighbour has none");
    emit(c_fill_color(0x07c107c1u));
    emit(c_fill_rect(7 << 2, 2 << 2, 0, 2 << 2));
    run();
    for (sy = 0; sy < 2; sy++)
        for (sx = 0; sx < 2; sx++) {
            const uint8_t *p = larger_at(3, 2, sx, sy, 2);
            CHECK(p && n64_be16(p) == 0x07c1, "drawn on again, sample (%u,%u): %04x", sx, sy, p ? n64_be16(p) : 0u);
        }

    /* A primitive that covers part of such a pixel leaves the game's
     * colour in the samples it does not draw. */
    n64_rdp_set_scale(&rdp, 4);
    begin("larger_step_in");
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            wr32(FB_ADDR + 4 * (y * FB_WIDTH + x), 0xabcdef00u);
    flat_triangle_scene();
    {
        int partial = 0;
        for (y = 0; y < 6; y++)
            for (x = 0; x < 8; x++) {
                int drawn = larger_count(x, y, 0x336699u), kept = larger_count(x, y, 0xabcdefu);
                if (drawn < 0)
                    continue;
                CHECK(drawn + kept == 16, "(%u,%u): %d samples drawn and %d the game's", x, y, drawn, kept);
                partial += drawn > 0 && kept > 0;
            }
        CHECK(partial >= 4, "%d pixels hold both", partial);
        CHECK(larger_count(7, 7, 0xabcdefu) == -1, "a pixel the triangle is nowhere near is in step");
    }
    n64_rdp_set_scale(&rdp, 1);
}

/* Everything the larger picture holds, as one number. */
static uint64_t larger_hash(void)
{
    N64RdpLargerView view;
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *parts[3];
    size_t sizes[3], i;
    unsigned part;
    if (!n64_rdp_larger_view(&rdp, &view))
        return 0;
    parts[0] = view.rdram;
    sizes[0] = (size_t)RDRAM_SIZE << (2 * view.log2);
    parts[1] = view.hidden;
    sizes[1] = (size_t)(RDRAM_SIZE / 2) << (2 * view.log2);
    parts[2] = view.reference;
    sizes[2] = RDRAM_SIZE;
    for (part = 0; part < 3; part++)
        for (i = 0; i < sizes[part]; i += 8) {
            uint64_t word;
            memcpy(&word, parts[part] + i, 8);
            h = (h ^ word) * 0x100000001b3ull;
            h ^= h >> 29;
        }
    return h;
}

/* 19. At a scale the console's own picture is what it is without one, and
 * the larger picture is the same whichever path draws it and however many
 * threads: over the scenes of paths_agree. */
static void test_larger_paths_agree(void)
{
    uint8_t *want = (uint8_t *)malloc(RDRAM_SIZE), *want_hidden = (uint8_t *)malloc(RDRAM_SIZE / 2);
    N64RdpState *want_state = (N64RdpState *)malloc(sizeof *want_state);
    uint32_t seed;
    unsigned run_index, scale;

    if (!want || !want_hidden || !want_state) {
        begin("larger_paths_agree");
        CHECK(0, "out of memory");
        return;
    }
    for (scale = 2; scale <= 4; scale += 2)
        for (seed = 1; seed <= 12; seed++) {
            /* The console alone; then at the scale through the reference
             * path, the shortcuts, and the shortcuts on many threads. */
            static const char *const names[4] = {"", "reference", "one thread", "16 threads"};
            uint64_t larger = 0;
            snprintf(test_name, sizeof test_name, "larger_paths_agree %ux scene %u", scale, (unsigned)seed);
            for (run_index = 0; run_index < 4; run_index++) {
                n64_rdp_set_scale(&rdp, run_index == 0 ? 1 : (int)scale);
                begin(test_name);
                scene_memory(seed);
                build_scene(seed);
                n64_rdp_set_reference(run_index == 1);
                n64_rdp_set_threads(run_index == 3 ? 16 : 1);
                run();
                if (run_index == 0) {
                    memcpy(want, rdram, RDRAM_SIZE);
                    memcpy(want_hidden, rdp.links.hidden, RDRAM_SIZE / 2);
                    memcpy(want_state, rdp.state, sizeof *want_state);
                    continue;
                }
                CHECK(memcmp(want, rdram, RDRAM_SIZE) == 0, "%s: RDRAM is not the console's", names[run_index]);
                CHECK(memcmp(want_hidden, rdp.links.hidden, RDRAM_SIZE / 2) == 0,
                      "%s: the hidden bits are not the console's", names[run_index]);
                CHECK(memcmp(want_state, rdp.state, offsetof(N64RdpState, spans)) == 0,
                      "%s: the RDP's state is not the console's", names[run_index]);
                if (run_index == 1)
                    larger = larger_hash();
                else
                    CHECK(larger_hash() == larger, "%s: the larger picture differs from the reference path's",
                          names[run_index]);
            }
        }
    n64_rdp_set_reference(0);
    n64_rdp_set_threads(0);
    n64_rdp_set_scale(&rdp, 1);
    free(want);
    free(want_hidden);
    free(want_state);
}

int main(void)
{
    rdram = (uint8_t *)calloc(1, RDRAM_SIZE);
    if (!rdram || !n64_rdp_init(&rdp, rdram, RDRAM_SIZE, dmem)) {
        printf("out of memory\n");
        return 1;
    }
    rdp.links.interrupt = on_interrupt;

    test_fill_rectangle();
    test_image_decoding();
    test_copy_16bit_tiles();
    test_copy_tlut();
    test_copy_8bit_and_4bit();
    test_load_block();
    test_copy_mirror();
    test_rgba32_and_yuv();
    test_texel_decoding();
    test_bilinear();
    test_flat_triangle();
    test_shaded_triangle();
    test_depth();
    test_depth_modes();
    test_combiner();
    test_blender();
    test_alpha_compare();
    test_dither();
    test_perspective();
    test_lod_frac_one_cycle();
    test_two_cycle_next_texel();
    test_syncs_and_unknown();
    test_dpc_registers();
    test_determinism();
    test_paths_agree();
    test_load_plans();
    test_larger_rules();
    test_larger_paths_agree();

    n64_rdp_free(&rdp);
    free(rdram);
    printf("n64_rdp: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
