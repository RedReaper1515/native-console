/*
 * Texture packs (gba_texture_pack.c) on a hand-built mode 0 screen:
 *
 * - A tile with an image is replaced by that image at the pack's scale,
 *   mirrored when the map entry flips it; other tiles and the backdrop keep
 *   their original colour, scaled.
 * - A palette the pack has no image for, but which is a per-channel linear
 *   change of one it has (a fade), shows the image recoloured.
 * - A sprite in front has its image drawn, and brightness fades apply to
 *   replaced pixels.
 * - Packs with a bad size, unsorted entries, or another ROM's hash are refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"
#include "gba_sha256.h"
#include "gba_texture_pack.h"

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

enum { SCALE = 2, SIDE = 16, W = 240, H = 160 };

static uint8_t rom[0x400];
static uint32_t out[W * SCALE * H * SCALE];

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t argb555(uint16_t c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}

/* One entry: a tile's bytes, a palette's bytes, an RGBA image. */
typedef struct {
    uint8_t key[64];
    uint8_t palette[32];
    uint8_t image[SIDE * SIDE * 4];
} Entry;

static int cmp_entry(const void *a, const void *b)
{
    return memcmp(((const Entry *)a)->key, ((const Entry *)b)->key, 64);
}

static uint8_t *build_pack(Entry *entries, int n, const uint8_t *rom_sha, size_t *size, bool sort)
{
    if (sort) qsort(entries, (size_t)n, sizeof *entries, cmp_entry);
    size_t total = 192 + (size_t)n * (76 + 32 + SIDE * SIDE * 4);
    uint8_t *d = calloc(1, total);
    put32(d, 0x50544147u);
    put32(d + 4, 1);
    if (rom_sha) {
        memcpy(d + 8, rom_sha, 32);
        put32(d + 40, sizeof rom);
    }
    put32(d + 44, SCALE);
    put32(d + 48, (uint32_t)n);
    put32(d + 52, (uint32_t)n * 32u);
    memcpy(d + 56, "test-pack", 9);
    memcpy(d + 120, "CC0-1.0", 7);
    uint8_t *e = d + 192, *pal = d + 192 + (size_t)n * 76, *img = pal + (size_t)n * 32;
    for (int i = 0; i < n; i++, e += 76) {
        memcpy(e, entries[i].key, 64);
        e[64] = 4;
        put32(e + 68, (uint32_t)i * 32u);
        memcpy(pal + i * 32, entries[i].palette, 32);
        memcpy(img + (size_t)i * SIDE * SIDE * 4, entries[i].image, SIDE * SIDE * 4);
    }
    *size = total;
    return d;
}

static void render_frame(GbaMachine *m)
{
    for (int line = 0; line < H; line++) gba_ppu_render_line(m, line);
    gba_texture_pack_frame_complete(m);
}

/* Output pixel (x, y) of the composed frame. */
static uint32_t at(int x, int y) { return out[y * W * SCALE + x]; }

int main(void)
{
    GbaMachine *m = gba_create();
    if (!m || !gba_load_rom(m, rom, sizeof rom)) {
        printf("cannot create machine\n");
        return 2;
    }
    gba_reset(m, true);

    /* Mode 0, BG0 on, char base 0, screen base block 8, 4bpp. */
    put16(m->io + 0x00, 0x0100);
    put16(m->io + 0x08, 0x0800);
    /* Palette 0: backdrop blue; palette 1: colours 1-15 ramp; palette 2 = palette 1 darkened. */
    put16(m->palette + 0, 0x7C00);
    for (int i = 1; i < 16; i++) {
        put16(m->palette + (16 + i) * 2, (uint16_t)(i * 2 | (i << 5)));
        put16(m->palette + (32 + i) * 2, (uint16_t)(i | ((i / 2) << 5)));
    }
    /* Tile 1: pattern of colours 1..15 with transparency in one corner. */
    for (int p = 0; p < 64; p++) {
        int v = (p % 15) + 1;
        if (p == 0) v = 0;
        int byte = 32 + p / 2;
        if (p & 1) m->vram[byte] |= (uint8_t)(v << 4);
        else m->vram[byte] |= (uint8_t)v;
    }
    /* Tile 2: all colour 3 (no image in the pack). */
    memset(m->vram + 64, 0x33, 32);
    /* Map (block 8 = VRAM 0x4000): (0,0) tile 1 pal 1, (1,0) tile 1 pal 1 hflip,
     * (2,0) tile 2 pal 1, (3,0) tile 1 pal 2 (the darkened palette). */
    put16(m->vram + 0x4000, 0x1001);
    put16(m->vram + 0x4002, 0x1401);
    put16(m->vram + 0x4004, 0x1002);
    put16(m->vram + 0x4006, 0x2001);

    Entry *entries = calloc(3, sizeof *entries);
    gba_sha256(m->vram + 32, 32, entries[0].key);
    memcpy(entries[0].palette, m->palette + 32, 32);
    gba_sha256(entries[0].palette, 32, entries[0].key + 32);
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            uint8_t *p = entries[0].image + (y * SIDE + x) * 4;
            p[0] = (uint8_t)(x * 16);
            p[1] = (uint8_t)(y * 16);
            p[2] = 200;
            p[3] = 255;
        }
    }

    size_t size;
    uint8_t *pack = build_pack(entries, 1, NULL, &size, true);
    GbaTexturePackStatus st = gba_texture_pack_load_memory(m, pack, size);
    CHECK(st == GBA_TEXTURE_PACK_OK, "load: %s", gba_texture_pack_status_string(st));
    CHECK(gba_texture_pack_scale() == SCALE && gba_texture_pack_entry_count() == 1, "pack info");

    render_frame(m);
    CHECK(gba_texture_pack_compose(out), "compose");
    /* Tile (0,0): the image as is. */
    CHECK(at(5, 3) == (0xFF000000u | (5u * 16u << 16) | (3u * 16u << 8) | 200u), "image pixel %08X", at(5, 3));
    /* Tile (1,0) flipped: screen column 0 of that tile shows image column 15. */
    CHECK(at(16, 3) == (0xFF000000u | (15u * 16u << 16) | (3u * 16u << 8) | 200u), "flipped pixel %08X", at(16, 3));
    /* Tile (2,0) has no image: its colour 3, scaled. */
    CHECK(at(33, 1) == argb555((uint16_t)(3 * 2 | (3 << 5))), "original pixel %08X", at(33, 1));
    /* Past the map row: backdrop. */
    CHECK(at(2, 40) == argb555(0x7C00), "backdrop %08X", at(2, 40));
    /* Tile (3,0), darkened palette: recoloured, so not the plain image colour. */
    GbaTexturePackStats stats = gba_texture_pack_stats();
    CHECK(stats.tiles_matched == 1 && stats.tiles_recoloured == 1, "stats matched %u recoloured %u missing %u",
          stats.tiles_matched, stats.tiles_recoloured, stats.tiles_missing);
    uint32_t dark = at(48 + 5, 3);
    CHECK(dark != at(5, 3) && ((dark >> 16) & 255) <= ((at(5, 3) >> 16) & 255), "recoloured %08X", dark);

    /* Brightness fade to black at 8/16 on BG0: replaced pixels fade too. */
    put16(m->io + 0x50, 0x00C1);
    put16(m->io + 0x54, 8);
    render_frame(m);
    gba_texture_pack_compose(out);
    uint32_t faded = at(5, 3);
    CHECK(((faded >> 16) & 255) == (5u * 16u) - ((5u * 16u * 8u) >> 4), "faded red %u", (faded >> 16) & 255);
    put16(m->io + 0x50, 0);

    /* Many fade steps: three tiles recoloured with new palettes every frame
     * for 1,000 frames fill the recolour cache past the point where it is
     * cleared. Clearing it in the middle of a frame freed images that frame
     * still used (the game crashed during Minish Cap's intro fade); every
     * recoloured pixel must stay exact. Tile 3 (all colours set), drawn at
     * row 1 with palettes 7, 8 and 9; its pack image uses palette 6, grey
     * ramp i, and palettes 7-9 add per-channel offsets, so the fit is exact. */
    for (int p = 0; p < 64; p++) {
        int v = ((p + 7) % 15) + 1;
        m->vram[96 + p / 2] |= (uint8_t)((p & 1) ? v << 4 : v);
    }
    for (int i = 1; i < 16; i++) put16(m->palette + (96 + i) * 2, (uint16_t)(i | (i << 5) | (i << 10)));
    Entry *stress = calloc(2, sizeof *stress);
    stress[0] = entries[0];
    gba_sha256(m->vram + 96, 32, stress[1].key);
    memcpy(stress[1].palette, m->palette + 96 * 2, 32);
    gba_sha256(stress[1].palette, 32, stress[1].key + 32);
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            uint8_t *p = stress[1].image + (y * SIDE + x) * 4;
            p[0] = (uint8_t)(x * 8);
            p[1] = (uint8_t)(y * 8);
            p[2] = 100;
            p[3] = 255;
        }
    }
    size_t stress_size;
    uint8_t *stress_pack = build_pack(stress, 2, NULL, &stress_size, true);
    CHECK(gba_texture_pack_load_memory(m, stress_pack, stress_size) == GBA_TEXTURE_PACK_OK, "stress pack");
    for (int j = 0; j < 3; j++) put16(m->vram + 0x4040 + j * 2, (uint16_t)(((7 + j) << 12) | 3));
    int wrong = 0;
    for (int k = 0; k < 1000; k++) {
        int offs[3][3];
        for (int j = 0; j < 3; j++) {
            int c = k * 3 + j;
            offs[j][0] = c % 17;
            offs[j][1] = (c / 17) % 17;
            offs[j][2] = (c / 289) % 17;
            for (int i = 1; i < 16; i++) {
                put16(m->palette + ((7 + j) * 16 + i) * 2,
                      (uint16_t)((i + offs[j][0]) | ((i + offs[j][1]) << 5) | ((i + offs[j][2]) << 10)));
            }
        }
        render_frame(m);
        gba_texture_pack_compose(out);
        for (int j = 0; j < 3; j++) {
            /* Screen pixel (2,3) of the tile, second sub-pixel: image (5,7) = (40, 56, 100). */
            static const int base[3] = { 40, 56, 100 };
            uint32_t want = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                double v = base[ch] + offs[j][ch] * 255.0 / 31.0;
                want |= (uint32_t)(uint8_t)(v > 255 ? 255 : v + 0.5) << (16 - 8 * ch);
            }
            if (at(16 * j + 5, 23) != want && wrong++ < 5)
                printf("frame %d tile %d: %08X, want %08X\n", k, j, at(16 * j + 5, 23), want);
        }
    }
    CHECK(wrong == 0, "%d recoloured pixels wrong over 1,000 fade frames", wrong);
    put16(m->vram + 0x4040, 0);
    put16(m->vram + 0x4042, 0);
    put16(m->vram + 0x4044, 0);
    free(stress_pack);
    free(stress);

    gba_texture_pack_unload();
    CHECK(!gba_texture_pack_active() && gba_tex_record == NULL, "unload");

    /* Refused packs. */
    GbaTexturePackStatus bad = gba_texture_pack_load_memory(m, pack, size - 1);
    CHECK(bad == GBA_TEXTURE_PACK_ERROR_FORMAT, "short pack: %d", bad);
    memcpy(&entries[1], &entries[0], sizeof *entries);
    entries[1].key[63] ^= 1;
    Entry swap = entries[0];
    entries[0] = entries[1];
    entries[1] = swap;
    size_t size2;
    uint8_t *unsorted = build_pack(entries, 2, NULL, &size2, false);
    if (memcmp(entries[0].key, entries[1].key, 64) > 0) {
        bad = gba_texture_pack_load_memory(m, unsorted, size2);
        CHECK(bad == GBA_TEXTURE_PACK_ERROR_ORDER, "unsorted pack: %d", bad);
    }
    uint8_t other_rom[32];
    memset(other_rom, 0xAB, sizeof other_rom);
    size_t size3;
    uint8_t *wrong_rom = build_pack(entries, 1, other_rom, &size3, true);
    bad = gba_texture_pack_load_memory(m, wrong_rom, size3);
    CHECK(bad == GBA_TEXTURE_PACK_ERROR_ROM_MISMATCH, "wrong ROM: %d", bad);
    uint8_t rom_sha[32];
    gba_sha256(rom, sizeof rom, rom_sha);
    size_t size4;
    uint8_t *right_rom = build_pack(entries, 1, rom_sha, &size4, true);
    bad = gba_texture_pack_load_memory(m, right_rom, size4);
    CHECK(bad == GBA_TEXTURE_PACK_OK, "matching ROM: %d", bad);
    gba_texture_pack_unload();

    free(pack);
    free(unsorted);
    free(wrong_rom);
    free(right_rom);
    free(entries);
    gba_destroy(m);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gba_texture_pack: all passed\n");
    return 0;
}
