/*
 * Rotated and scaled layers at a higher resolution (gba.h,
 * gba_ppu_set_affine_scale): the larger picture against the console's
 * formulas in exact arithmetic, sample by sample.
 *
 * A scene is written straight into the machine's registers and memory and
 * a frame's lines are rendered as the machine renders them. For each scale
 * the console's frame must be what it is without one, a sample at a whole
 * pixel must be the console's pixel, and every sample must be the layer at
 * its own place. --time prints what a frame costs at each scale instead.
 */
#include "gba.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W GBA_SCREEN_WIDTH
#define H GBA_SCREEN_HEIGHT

static GbaMachine *m;
static int failures, checks;
static const char *current = "";

#define CHECK(cond, ...)                          \
    do {                                          \
        checks++;                                 \
        if (!(cond)) {                            \
            failures++;                           \
            printf("FAIL %s: ", current);         \
            printf(__VA_ARGS__);                  \
            printf("\n");                         \
        }                                         \
    } while (0)

static void io16(uint32_t off, uint32_t v)
{
    m->io[off] = (uint8_t)v;
    m->io[off + 1] = (uint8_t)(v >> 8);
}

static void io32(uint32_t off, uint32_t v)
{
    io16(off, v & 0xFFFFu);
    io16(off + 2, v >> 16);
}

static uint16_t pal16(uint32_t index) { return (uint16_t)((m->palette[index * 2] | m->palette[index * 2 + 1] << 8) & 0x7FFF); }

static uint32_t argb(uint16_t c)
{
    uint32_t r = (c & 0x1Fu) << 3, g = ((c >> 5) & 0x1Fu) << 3, b = ((c >> 10) & 0x1Fu) << 3;
    return 0xFF000000u | ((r | (r >> 5)) << 16) | ((g | (g >> 5)) << 8) | (b | (b >> 5));
}

static int64_t floor_div(int64_t v, int64_t d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

/* A frame: the reference points are loaded at VBlank, then the lines. The
 * VBlank after them completes the larger picture. */
static void frame(void (*per_line)(int))
{
    gba_ppu_frame_start(m);
    for (int line = 0; line < H; line++) {
        if (per_line) per_line(line);
        gba_ppu_render_line(m, line);
    }
    gba_ppu_frame_start(m);
}

static void clear_scene(void)
{
    memset(m->io, 0, 0x60);
    memset(m->vram, 0, sizeof m->vram);
    memset(m->oam, 0, sizeof m->oam);
    for (int i = 0; i < 128; i++) m->oam[i * 8 + 1] = 0x02; /* every sprite off */
    for (uint32_t c = 0; c < 512; c++) {
        const uint16_t color = (uint16_t)((c & 31u) | ((c >> 3) & 31u) << 5 | ((c * 5u) & 31u) << 10);
        m->palette[c * 2] = (uint8_t)color;
        m->palette[c * 2 + 1] = (uint8_t)(color >> 8);
    }
}

/* Mode 2, BG2: a 128 x 128 wrapping layer of four tiles, every texel its
 * own colour. Map at 0x4000, tiles at 0. */
static void tile_scene(void)
{
    clear_scene();
    io16(0x00, 0x0402);
    io16(0x0C, 0x2800);
    for (uint32_t ty = 0; ty < 16; ty++)
        for (uint32_t tx = 0; tx < 16; tx++) m->vram[0x4000u + ty * 16u + tx] = (uint8_t)(1u + ((tx * 3u + ty * 5u) & 3u));
    for (uint32_t t = 1; t <= 4; t++)
        for (uint32_t i = 0; i < 64; i++) m->vram[t * 64u + i] = (uint8_t)(1u + (t * 40u + (i * 7u) % 37u) % 200u);
}

static uint32_t tile_texel(int32_t tx, int32_t ty)
{
    tx &= 127;
    ty &= 127;
    const uint32_t tile = m->vram[0x4000u + ((uint32_t)ty >> 3) * 16u + ((uint32_t)tx >> 3)];
    return argb(pal16(m->vram[tile * 64u + ((uint32_t)ty & 7u) * 8u + ((uint32_t)tx & 7u)]));
}

/* Mode 3: a 240 x 160 picture of 15-bit colours. */
static void bitmap_scene(void)
{
    clear_scene();
    io16(0x00, 0x0403);
    for (uint32_t i = 0; i < (uint32_t)W * H; i++) {
        const uint16_t c = (uint16_t)(((i % W) * 7u + (i / W) * 13u) & 0x7FFFu);
        m->vram[i * 2] = (uint8_t)c;
        m->vram[i * 2 + 1] = (uint8_t)(c >> 8);
    }
}

static uint32_t bitmap_texel(int32_t tx, int32_t ty)
{
    if (tx < 0 || ty < 0 || tx >= W || ty >= H) return argb(pal16(0));
    const uint32_t i = (uint32_t)(ty * W + tx);
    return argb((uint16_t)((m->vram[i * 2] | m->vram[i * 2 + 1] << 8) & 0x7FFF));
}

static void matrix(int pa, int pb, int pc, int pd, int32_t x, int32_t y)
{
    io16(0x20, (uint32_t)pa & 0xFFFFu);
    io16(0x22, (uint32_t)pb & 0xFFFFu);
    io16(0x24, (uint32_t)pc & 0xFFFFu);
    io16(0x26, (uint32_t)pd & 0xFFFFu);
    io32(0x28, (uint32_t)x & 0x0FFFFFFFu);
    io32(0x2C, (uint32_t)y & 0x0FFFFFFFu);
}

static uint32_t console[W * H];

/* One background through one matrix, at every scale. */
static void check_background(const char *name, uint32_t (*texel)(int32_t, int32_t), int pa, int pb, int pc, int pd,
                             int32_t x, int32_t y)
{
    static char label[96];
    int width = 0, height = 0;
    matrix(pa, pb, pc, pd, x, y);
    gba_ppu_set_affine_scale(m, 1);
    frame(NULL);
    memcpy(console, m->framebuffer, sizeof console);
    for (int n = 2; n <= GBA_AFFINE_SCALE_MAX; n++) {
        snprintf(label, sizeof label, "%s at %dx", name, n);
        current = label;
        CHECK(gba_ppu_set_affine_scale(m, n) && gba_ppu_affine_scale(m) == n, "the scale was refused");
        frame(NULL);
        CHECK(memcmp(console, m->framebuffer, sizeof console) == 0, "the console's frame changed");
        const uint32_t *large = gba_ppu_large_frame(m, &width, &height);
        CHECK(large && width == W * n && height == H * n, "the larger picture is %d x %d", width, height);
        if (!large || width != W * n || height != H * n) continue;
        unsigned wrong = 0, not_console = 0;
        for (int ys = 0; ys < height; ys++)
            for (int xs = 0; xs < width; xs++) {
                /* The reference point in 256ths, counted in n-ths. */
                const int64_t rx = (int64_t)x * n + (int64_t)pb * ys + (int64_t)pa * xs;
                const int64_t ry = (int64_t)y * n + (int64_t)pd * ys + (int64_t)pc * xs;
                const uint32_t got = large[(size_t)ys * (size_t)width + (size_t)xs];
                wrong += got != texel((int32_t)floor_div(rx, 256 * n), (int32_t)floor_div(ry, 256 * n));
                if (ys % n == 0 && xs % n == 0) not_console += got != console[(ys / n) * W + xs / n];
            }
        CHECK(wrong == 0, "%u samples are not the layer's", wrong);
        CHECK(not_console == 0, "%u whole-pixel samples are not the console's", not_console);
    }
}

/* A floor in perspective: every line the game sets the reference point
 * (1000 further along) and PA (2 more). */
static void tilt_line(int line)
{
    io16(0x20, (uint32_t)(256 + 2 * line));
    io32(0x28, (uint32_t)(1000 * line));
    gba_ppu_reference_written(m, 0x28);
}

static void check_tilt(void)
{
    int width = 0, height = 0;
    current = "a matrix set every line";
    tile_scene();
    matrix(256, 0, 0, 256, 0, 0x300);
    gba_ppu_set_affine_scale(m, 4);
    frame(tilt_line);
    const uint32_t *large = gba_ppu_large_frame(m, &width, &height);
    CHECK(large != NULL, "no larger picture");
    if (!large) return;
    unsigned wrong = 0;
    for (int line = 0; line < H; line++) {
        /* The first two lines have no steady movement behind them and go by
         * PB and PD; after that the reference point goes on as it went. */
        const int64_t step_x = line >= 2 ? 1000 : 0, step_y = line >= 2 ? 0 : 256, turn = line >= 2 ? 2 : 0;
        for (int k = 0; k < 4; k++)
            for (int xs = 0; xs < width; xs++) {
                const int64_t rx = ((int64_t)1000 * line * 4 + step_x * k) * 4 + ((int64_t)(256 + 2 * line) * 4 + turn * k) * xs;
                const int64_t ry = ((int64_t)0x300 * 4 + step_y * k) * 4;
                wrong += large[((size_t)line * 4u + (size_t)k) * (size_t)width + (size_t)xs] !=
                         tile_texel((int32_t)floor_div(rx, 4096), (int32_t)floor_div(ry, 4096));
            }
    }
    CHECK(wrong == 0, "%u samples are not the layer's", wrong);
}

/* A 16 x 16 sprite in a 32 x 32 box at (100, 60), turned and enlarged. */
static void check_sprite(void)
{
    enum { OX = 100, OY = 60, PA = 150, PB = -90, PC = 90, PD = 150 };
    static char label[64];
    int width = 0, height = 0;
    clear_scene();
    io16(0x00, 0x1040); /* mode 0, sprites, one-dimensional tiles */
    for (uint32_t t = 0; t < 4; t++)
        for (uint32_t i = 0; i < 64; i++) {
            const uint32_t tx = (t % 2u) * 8u + i % 8u, ty = (t / 2u) * 8u + i / 8u;
            const uint32_t index = 1u + (tx + ty * 3u) % 15u, at = 0x10000u + t * 32u + (i / 8u) * 4u + (i % 8u) / 2u;
            m->vram[at] = (uint8_t)((i & 1u) ? (m->vram[at] & 0x0Fu) | index << 4 : (m->vram[at] & 0xF0u) | index);
        }
    m->oam[0] = OY;
    m->oam[1] = 0x03; /* rotated and scaled, double size */
    m->oam[2] = OX;
    m->oam[3] = 0x40; /* 16 x 16 */
    m->oam[6] = (uint8_t)((unsigned)PA & 0xFFu);
    m->oam[7] = (uint8_t)(((unsigned)PA >> 8) & 0xFFu);
    m->oam[14] = (uint8_t)((unsigned)PB & 0xFFu);
    m->oam[15] = (uint8_t)(((unsigned)PB >> 8) & 0xFFu);
    m->oam[22] = (uint8_t)((unsigned)PC & 0xFFu);
    m->oam[23] = (uint8_t)(((unsigned)PC >> 8) & 0xFFu);
    m->oam[30] = (uint8_t)((unsigned)PD & 0xFFu);
    m->oam[31] = (uint8_t)(((unsigned)PD >> 8) & 0xFFu);

    gba_ppu_set_affine_scale(m, 1);
    frame(NULL);
    memcpy(console, m->framebuffer, sizeof console);
    for (int n = 2; n <= GBA_AFFINE_SCALE_MAX; n++) {
        snprintf(label, sizeof label, "a sprite at %dx", n);
        current = label;
        gba_ppu_set_affine_scale(m, n);
        frame(NULL);
        CHECK(memcmp(console, m->framebuffer, sizeof console) == 0, "the console's frame changed");
        const uint32_t *large = gba_ppu_large_frame(m, &width, &height);
        CHECK(large != NULL, "no larger picture");
        if (!large) continue;
        unsigned wrong = 0, drawn = 0, not_console = 0;
        for (int ys = 0; ys < height; ys++)
            for (int xs = 0; xs < width; xs++) {
                uint32_t want = argb(pal16(0));
                if (xs >= OX * n && xs < (OX + 32) * n && ys >= OY * n && ys < (OY + 32) * n) {
                    const int64_t cx = xs - (OX + 16) * n, cy = ys - (OY + 16) * n;
                    const int64_t tx = floor_div(PA * cx + PB * cy, 256 * n) + 8, ty = floor_div(PC * cx + PD * cy, 256 * n) + 8;
                    if (tx >= 0 && ty >= 0 && tx < 16 && ty < 16) {
                        want = argb(pal16(256u + 1u + (uint32_t)(tx + ty * 3) % 15u));
                        drawn++;
                    }
                }
                const uint32_t got = large[(size_t)ys * (size_t)width + (size_t)xs];
                wrong += got != want;
                if (ys % n == 0 && xs % n == 0) not_console += got != console[(ys / n) * W + xs / n];
            }
        CHECK(wrong == 0, "%u samples are not the sprite's", wrong);
        CHECK(not_console == 0, "%u whole-pixel samples are not the console's", not_console);
        CHECK(drawn > (unsigned)(400 * n * n), "only %u samples of the sprite", drawn);
    }
}

/* --time: milliseconds a frame of a turned full-screen layer takes to
 * render at each scale (the PPU alone). */
static void time_scales(void)
{
    tile_scene();
    matrix(200, 60, -60, 200, 0x1234, -0x0800);
    for (int n = 1; n <= GBA_AFFINE_SCALE_MAX; n++) {
        gba_ppu_set_affine_scale(m, n);
        const clock_t start = clock();
        for (int i = 0; i < 300; i++) frame(NULL);
        printf("%dx: %.3f ms a frame\n", n, 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC / 300.0);
    }
}

int main(int argc, char **argv)
{
    int width = 0, height = 0;
    m = gba_create();
    if (!m) {
        printf("out of memory\n");
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--time")) {
        time_scales();
        gba_destroy(m);
        return 0;
    }

    current = "scales";
    CHECK(gba_ppu_affine_scale(m) == 1 && !gba_ppu_large_frame(m, &width, &height), "a larger picture at a scale of 1");
    CHECK(!gba_ppu_set_affine_scale(m, GBA_AFFINE_SCALE_MAX + 1) && gba_ppu_affine_scale(m) == 1, "a scale too large was taken");

    tile_scene();
    check_background("a turned layer", tile_texel, 200, 60, -60, 200, 0x1234, -0x0800);
    check_background("a reduced layer", tile_texel, 512, 0, 0, 512, 0, 0);
    check_background("a sheared layer", tile_texel, 256, 100, 0, 256, 0x80, 0x40);
    bitmap_scene();
    check_background("a turned bitmap", bitmap_texel, 230, -40, 40, 230, 0x2000, 0x1000);
    check_tilt();
    check_sprite();

    /* A layer drawn one texel to a pixel has no larger picture; nor has a
     * line with a mosaic. */
    current = "nothing to draw larger";
    tile_scene();
    matrix(256, 0, 0, 256, 0x500, 0x300);
    gba_ppu_set_affine_scale(m, 4);
    frame(NULL);
    CHECK(!gba_ppu_large_frame(m, &width, &height), "a larger picture of a layer that is not turned or scaled");
    matrix(200, 60, -60, 200, 0, 0);
    frame(NULL);
    CHECK(gba_ppu_large_frame(m, &width, &height) != NULL, "no larger picture of a turned layer");
    io16(0x4C, 0x0003);
    frame(NULL);
    CHECK(!gba_ppu_large_frame(m, &width, &height), "a larger picture with a mosaic set");
    io16(0x4C, 0);
    gba_ppu_set_affine_scale(m, 1);
    CHECK(gba_ppu_affine_scale(m) == 1 && !gba_ppu_large_frame(m, &width, &height), "the scale goes back to 1");

    gba_destroy(m);
    printf("gba_ppu_large: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
