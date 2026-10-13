/*
 * The CRT display filter's picture (snes_crt.c): black stays black, a flat
 * colour stays flat along the line, beams are brightest at their line's
 * centre and dark between lines, a bright beam fills more of the gap than a
 * dim one, a sharp edge is softened along the line but not smeared far,
 * bad line heights are refused, the mask cycles red, green, blue, and the
 * halo of a flat image is that image; a frame read through a row stride
 * gives the same picture as one packed tightly. Also prints the time for a 256x224 frame at
 * fullscreen 1440p (6 rows per line).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "snes_crt.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

enum { W = 256, H = 224, LINE = 6, COLUMNS = 4, COLS = W * COLUMNS };
static uint32_t src[W * H], dst[COLS * H * LINE], glow[W * H], wide[512 * H], wide_dst[COLS * H * LINE];

static int green(uint32_t px) { return (int)((px >> 8) & 0xFF); }

static void fill(uint32_t c)
{
    for (int i = 0; i < W * H; i++) src[i] = c;
}

/* Green of column x in row p of source line y. */
static int at(int y, int p, int x) { return green(dst[(size_t)(y * LINE + p) * COLS + x]); }

int main(void)
{
    snes_crt *crt = snes_crt_create();
    CHECK(crt != NULL, "create");
    if (!crt) return 1;

    fill(0xFF000000u);
    CHECK(snes_crt_render(crt, src, W, H, W, COLUMNS, LINE, dst), "render black");
    int lit = 0;
    for (size_t i = 0; i < (size_t)COLS * H * LINE; i++) lit |= (int)(dst[i] & 0xFFFFFFu);
    CHECK(lit == 0, "black lights nothing");

    const uint32_t greys[2] = { 0xFF404040u, 0xFFFFFFFFu };
    int gap[2];
    for (int g = 0; g < 2; g++) {
        fill(greys[g]);
        CHECK(snes_crt_render(crt, src, W, H, W, COLUMNS, LINE, dst), "render grey");
        const int centre = at(80, LINE / 2, COLS / 2), edge = at(80, 0, COLS / 2);
        int flat = 1;
        for (int x = 0; x < COLS; x++) flat &= at(80, LINE / 2, x) == centre;
        CHECK(flat, "flat colour %08x stays flat along the line", greys[g]);
        CHECK(centre > edge, "line centre %d brighter than its edge %d", centre, edge);
        gap[g] = edge * 1000 / (centre ? centre : 1);
    }
    CHECK(gap[1] > gap[0] + 150, "bright beam fills more of the gap (%d vs %d per mille)", gap[1], gap[0]);
    CHECK(gap[0] < 500, "dim lines leave a dark gap (%d per mille)", gap[0]);

    /* Left half black, right half white: soft over about a pixel, not more. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) src[y * W + x] = x < W / 2 ? 0xFF000000u : 0xFFFFFFFFu;
    CHECK(snes_crt_render(crt, src, W, H, W, COLUMNS, LINE, dst), "render edge");
    const int e = W / 2 * COLUMNS;
    CHECK(at(80, LINE / 2, e - 1) > 0 && at(80, LINE / 2, e - 1) < at(80, LINE / 2, e + 8), "edge softened");
    CHECK(at(80, LINE / 2, e - 2 * COLUMNS) == 0, "edge not smeared two pixels");

    CHECK(!snes_crt_render(crt, src, W, H, W, COLUMNS, SNES_CRT_MIN_LINE - 1, dst), "line height too small refused");
    CHECK(!snes_crt_render(crt, src, W, H, W, COLUMNS, SNES_CRT_MAX_LINE + 1, dst), "line height too large refused");

    for (int x = 0; x < 6; x++) {
        const uint32_t m = snes_crt_mask(x), full = 0xFFu << (16 - 8 * (x % 3));
        CHECK((m & full) == full && (m & 0xFFFFFFu) != 0xFFFFFFu, "mask column %d is stripe %d", x, x % 3);
    }

    /* The same frame stored 512 apart with junk after each row. */
    for (int i = 0; i < W * H; i++) src[i] = 0xFF000000u | (uint32_t)(i * 2246822519u >> 8);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < 512; x++) wide[y * 512 + x] = x < W ? src[y * W + x] : 0xFFFFFFFFu;
    CHECK(snes_crt_render(crt, src, W, H, W, COLUMNS, LINE, dst), "render packed");
    CHECK(snes_crt_render(crt, wide, W, H, 512, COLUMNS, LINE, wide_dst), "render through stride");
    CHECK(!memcmp(dst, wide_dst, sizeof dst), "stride read the same pixels");
    fill(0xFF6080A0u);
    CHECK(snes_crt_render(crt, src, W, H, W, 2, LINE, dst), "render 2 columns");
    CHECK(dst[(size_t)(100 * LINE + LINE / 2) * W * 2 + 7] == dst[(size_t)(100 * LINE + LINE / 2) * W * 2 + 300],
          "flat colour flat at 2 columns");
    CHECK(!snes_crt_render(crt, src, W, H, W, SNES_CRT_MAX_COLUMNS + 1, LINE, dst), "too many columns refused");

    fill(0xFF6080A0u);
    CHECK(snes_crt_glow(crt, src, W, H, W, glow), "glow");
    int same = 1;
    for (int i = 0; i < W * H; i++) same &= glow[i] == 0xFF6080A0u;
    CHECK(same, "halo of a flat image is the image");

    for (int i = 0; i < W * H; i++) src[i] = 0xFF000000u | (uint32_t)(i * 2654435761u >> 8);
    enum { FRAMES = 60 };
    const clock_t start = clock();
    for (int i = 0; i < FRAMES; i++) {
        snes_crt_render(crt, src, W, H, W, COLUMNS, LINE, dst);
        snes_crt_glow(crt, src, W, H, W, glow);
    }
    printf("CRT %dx%d at %d rows per line: %.2f ms per frame\n", W, H, LINE,
           (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC / FRAMES);

    snes_crt_free(crt);
    if (failures) printf("%d failure(s)\n", failures);
    else printf("snes_crt: all checks passed\n");
    return failures ? 1 : 0;
}
