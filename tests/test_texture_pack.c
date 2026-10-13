#include "gbrt.h"
#include "gbrt_hash.h"
#include "gbrt_texture_pack.h"
#include "ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCALE 2u
#define SIDE (8u * SCALE)
#define IMAGE_BYTES (SIDE * SIDE * 4u)

typedef struct {
    uint8_t tile[16];
    uint8_t kind;
    uint8_t palette;
    uint8_t rgba[IMAGE_BYTES];
} TestEntry;

static uint8_t g_rom[32u * 1024u];

static void put_le32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static int compare_keys(const void* a, const void* b) {
    const uint8_t* ka = (const uint8_t*)a;
    const uint8_t* kb = (const uint8_t*)b;
    return memcmp(ka, kb, 34);
}

/* Builds a pack in `out` (caller frees). Entries are sorted here unless asked not to. */
static uint8_t* build_pack(const TestEntry* entries, uint32_t count, bool sort, size_t* size) {
    const size_t total = GB_TEXTURE_PACK_HEADER_SIZE +
                         count * (GB_TEXTURE_PACK_ENTRY_SIZE + IMAGE_BYTES);
    uint8_t* data = (uint8_t*)calloc(1, total);
    put_le32(data, GB_TEXTURE_PACK_MAGIC);
    put_le32(data + 4, GB_TEXTURE_PACK_VERSION);
    gbrt_sha256(g_rom, sizeof(g_rom), data + 8);
    put_le32(data + 40, sizeof(g_rom));
    put_le32(data + 44, SCALE);
    put_le32(data + 48, count);
    strcpy((char*)data + 56, "org.example.test-pack");
    strcpy((char*)data + 120, "CC0-1.0");

    /* Sort (key, index) records, then lay out entries and images in that order. */
    uint8_t (*records)[36] = calloc(count, 36);
    for (uint32_t i = 0; i < count; ++i) {
        gbrt_sha256(entries[i].tile, 16, records[i]);
        records[i][32] = entries[i].kind;
        records[i][33] = entries[i].palette;
        records[i][34] = (uint8_t)i; /* stash the source index past the key */
    }
    if (sort) {
        qsort(records, count, 36, compare_keys);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t source = records[i][34];
        uint8_t* entry = data + GB_TEXTURE_PACK_HEADER_SIZE + i * GB_TEXTURE_PACK_ENTRY_SIZE;
        memcpy(entry, records[i], 34);
        memcpy(data + GB_TEXTURE_PACK_HEADER_SIZE + count * GB_TEXTURE_PACK_ENTRY_SIZE +
                   i * IMAGE_BYTES,
               entries[source].rgba, IMAGE_BYTES);
    }
    free(records);
    *size = total;
    return data;
}

static void fill_image(TestEntry* entry, uint8_t r, uint8_t g, uint8_t b) {
    for (uint32_t i = 0; i < SIDE * SIDE; ++i) {
        entry->rgba[i * 4] = r;
        entry->rgba[i * 4 + 1] = g;
        entry->rgba[i * 4 + 2] = b;
        entry->rgba[i * 4 + 3] = 255;
    }
}

static void set_image_pixel(TestEntry* entry, uint32_t x, uint32_t y, uint32_t argb) {
    uint8_t* p = entry->rgba + (y * SIDE + x) * 4;
    p[0] = (uint8_t)(argb >> 16);
    p[1] = (uint8_t)(argb >> 8);
    p[2] = (uint8_t)argb;
    p[3] = 255;
}

static GBContext* make_context(void) {
    GBConfig config;
    memset(&config, 0, sizeof(config));
    config.model = GB_MODEL_DMG;
    GBContext* ctx = gb_context_create(&config);
    if (!ctx || !gb_context_load_rom(ctx, g_rom, sizeof(g_rom))) {
        gb_context_destroy(ctx);
        return NULL;
    }
    gb_context_reset(ctx, true);
    return ctx;
}

static void run_frame(GBContext* ctx) {
    ctx->frame_done = 0;
    ((GBPPU*)ctx->ppu)->frame_ready = false;
    for (int guard = 0; guard < 1000 && !ctx->frame_done; ++guard) {
        gb_tick(ctx, 456);
    }
}

static int fail(const char* message) {
    fprintf(stderr, "texture pack: %s\n", message);
    return 1;
}

static int test_loader(void) {
    GBContext* ctx = make_context();
    if (!ctx) return fail("context");
    static TestEntry entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].tile[0] = 1;
    entries[1].tile[0] = 2;
    entries[1].kind = GB_TEXTURE_PALETTE_DMG;
    entries[1].palette = 0xE4;

    size_t size = 0;
    uint8_t* pack = build_pack(entries, 2, true, &size);
    int failed = 0;

    failed |= gbrt_texture_pack_load_memory(ctx, pack, size) != GB_TEXTURE_PACK_OK;
    failed |= !gbrt_texture_pack_is_active() || gbrt_texture_pack_scale() != SCALE ||
              gbrt_texture_pack_entry_count() != 2 || gbrt_pixel_sources == NULL ||
              strcmp(gbrt_texture_pack_id(), "org.example.test-pack") != 0;
    if (failed) fprintf(stderr, "valid pack rejected\n");

    /* Each corruption sets one byte: bad magic, version, ROM hash, scale 0,
     * nonzero reserved, empty pack ID, unknown palette kind, reserved entry byte. */
    struct {
        size_t offset;
        uint8_t value;
        GBTexturePackStatus expected;
    } corruptions[] = {
        {0, 'X', GB_TEXTURE_PACK_ERROR_FORMAT},
        {4, 9, GB_TEXTURE_PACK_ERROR_VERSION},
        {8, 0, GB_TEXTURE_PACK_ERROR_ROM_MISMATCH},
        {44, 0, GB_TEXTURE_PACK_ERROR_LIMIT},
        {52, 1, GB_TEXTURE_PACK_ERROR_FORMAT},
        {56, 0, GB_TEXTURE_PACK_ERROR_FORMAT},
        {GB_TEXTURE_PACK_HEADER_SIZE + 32, 7, GB_TEXTURE_PACK_ERROR_FORMAT},
        {GB_TEXTURE_PACK_HEADER_SIZE + 35, 1, GB_TEXTURE_PACK_ERROR_FORMAT},
    };
    for (size_t i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); ++i) {
        uint8_t* copy = (uint8_t*)malloc(size);
        memcpy(copy, pack, size);
        copy[corruptions[i].offset] = corruptions[i].value;
        if (corruptions[i].offset == 8 && pack[8] == 0) {
            copy[8] = 1;
        }
        GBTexturePackStatus status = gbrt_texture_pack_load_memory(ctx, copy, size);
        if (status != corruptions[i].expected) {
            fprintf(stderr, "corruption %zu gave %s\n", i, gbrt_texture_pack_status_string(status));
            failed = 1;
        }
        free(copy);
    }
    if (gbrt_texture_pack_load_memory(ctx, pack, size - 1) != GB_TEXTURE_PACK_ERROR_FORMAT) {
        fprintf(stderr, "truncated pack accepted\n");
        failed = 1;
    }
    free(pack);

    /* A failed load keeps the previously active pack. */
    failed |= !gbrt_texture_pack_is_active();

    /* Unsorted and duplicate entries. */
    pack = build_pack(entries, 2, false, &size);
    uint8_t* first = pack + GB_TEXTURE_PACK_HEADER_SIZE;
    uint8_t* second = first + GB_TEXTURE_PACK_ENTRY_SIZE;
    if (memcmp(first, second, 34) < 0) {
        uint8_t tmp[GB_TEXTURE_PACK_ENTRY_SIZE];
        memcpy(tmp, first, sizeof(tmp));
        memcpy(first, second, sizeof(tmp));
        memcpy(second, tmp, sizeof(tmp));
    }
    failed |= gbrt_texture_pack_load_memory(ctx, pack, size) != GB_TEXTURE_PACK_ERROR_ORDER;
    memcpy(second, first, GB_TEXTURE_PACK_ENTRY_SIZE);
    failed |= gbrt_texture_pack_load_memory(ctx, pack, size) != GB_TEXTURE_PACK_ERROR_ORDER;
    free(pack);

    gbrt_texture_pack_unload();
    failed |= gbrt_texture_pack_is_active() || gbrt_pixel_sources != NULL;
    gb_context_destroy(ctx);
    return failed;
}

static int test_ppu_composition(void) {
    GBContext* ctx = make_context();
    if (!ctx) return fail("context");
    GBPPU* ppu = (GBPPU*)ctx->ppu;

    /* Tile 1: left column color 3, rest color 1. Tile 2: sprite, left column 3.
     * Tile 3: solid color 2, no replacement. */
    static const uint8_t tile1[16] = {0xFF, 0x80, 0xFF, 0x80, 0xFF, 0x80, 0xFF, 0x80,
                                      0xFF, 0x80, 0xFF, 0x80, 0xFF, 0x80, 0xFF, 0x80};
    static const uint8_t tile2[16] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
    static const uint8_t tile3[16] = {0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
                                      0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF};

    gb_write8(ctx, 0xFF40, 0x00);
    memcpy(ctx->vram + 1 * 16, tile1, 16);
    memcpy(ctx->vram + 2 * 16, tile2, 16);
    memcpy(ctx->vram + 3 * 16, tile3, 16);
    memset(ctx->vram + 0x1800, 1, 32 * 32);        /* BG map: tile 1 everywhere */
    memset(ctx->vram + 0x1800 + 32, 3, 32);         /* map row 1: tile 3 */
    memset(ctx->oam, 0, OAM_SIZE);
    ctx->oam[0] = 16 + 32;                           /* screen y 32 */
    ctx->oam[1] = 8 + 40;                            /* screen x 40 */
    ctx->oam[2] = 2;
    ctx->oam[3] = 0x20;                              /* X flip, OBP0 */
    gb_write8(ctx, 0xFF42, 0);
    gb_write8(ctx, 0xFF43, 0);
    gb_write8(ctx, 0xFF47, 0xE4);
    gb_write8(ctx, 0xFF48, 0xD2);
    gb_write8(ctx, 0xFF40, 0x93);                    /* LCD, OBJ, BG on */

    run_frame(ctx);
    run_frame(ctx);
    uint32_t reference[GB_FRAMEBUFFER_SIZE];
    memcpy(reference, ppu->rgb_framebuffer, sizeof(reference));

    static TestEntry entries[3];
    memset(entries, 0, sizeof(entries));
    memcpy(entries[0].tile, tile1, 16);
    fill_image(&entries[0], 200, 0, 0);
    set_image_pixel(&entries[0], 0, 0, 0x00FF00);     /* top-left HD pixel of tile 1 */
    memcpy(entries[1].tile, tile2, 16);
    entries[1].kind = GB_TEXTURE_PALETTE_DMG;
    entries[1].palette = 0xD2;
    fill_image(&entries[1], 0, 0, 200);
    set_image_pixel(&entries[1], 0, 0, 0x00FFFF);     /* top-left HD pixel of tile 2 */
    memcpy(entries[2].tile, tile2, 16);
    entries[2].kind = GB_TEXTURE_PALETTE_DMG;
    entries[2].palette = 0x00;                        /* wrong palette: never chosen */
    fill_image(&entries[2], 255, 255, 255);

    size_t size = 0;
    uint8_t* pack = build_pack(entries, 3, true, &size);
    int failed = 0;
    if (gbrt_texture_pack_load_memory(ctx, pack, size) != GB_TEXTURE_PACK_OK) {
        free(pack);
        gb_context_destroy(ctx);
        return fail("load for composition");
    }
    free(pack);

    run_frame(ctx);
    if (memcmp(reference, ppu->rgb_framebuffer, sizeof(reference)) != 0) {
        fprintf(stderr, "an active pack changed the accurate framebuffer\n");
        failed = 1;
    }

    const uint32_t bg = gbrt_pixel_sources[0];
    if (GB_PIXEL_SOURCE_LAYER(bg) != GB_PIXEL_LAYER_BG || GB_PIXEL_SOURCE_SLOT(bg) != 1 ||
        GB_PIXEL_SOURCE_TX(bg) != 0 || GB_PIXEL_SOURCE_TY(bg) != 0 ||
        GB_PIXEL_SOURCE_PALETTE(bg) != 0xE4 || GB_PIXEL_SOURCE_RAW(bg) != 3) {
        fprintf(stderr, "background source %08X\n", bg);
        failed = 1;
    }
    /* Sprite at x 40..47: X flip puts tile column 0 (color 3) at screen x 47. */
    const uint32_t obj = gbrt_pixel_sources[32 * GB_SCREEN_WIDTH + 47];
    if (GB_PIXEL_SOURCE_LAYER(obj) != GB_PIXEL_LAYER_OBJ || GB_PIXEL_SOURCE_SLOT(obj) != 2 ||
        GB_PIXEL_SOURCE_TX(obj) != 0 || !GB_PIXEL_SOURCE_FLIP_X(obj) ||
        GB_PIXEL_SOURCE_PALETTE(obj) != 0xD2) {
        fprintf(stderr, "object source %08X\n", obj);
        failed = 1;
    }

    const uint32_t width = GB_SCREEN_WIDTH * SCALE;
    uint32_t* output = (uint32_t*)malloc(sizeof(uint32_t) * width * GB_SCREEN_HEIGHT * SCALE);
    if (!gbrt_texture_pack_compose(ppu->rgb_framebuffer, output)) {
        failed = 1;
        fprintf(stderr, "compose returned false\n");
    } else {
        /* BG pixel (0,0) is tile 1 column 0: top-left HD pixel is the marker. */
        failed |= output[0] != 0xFF00FF00u || output[1] != 0xFFC80000u;
        /* Row 8 (map row 1) is tile 3 with no entry: original pixel, scaled. */
        const uint32_t original = ppu->rgb_framebuffer[8 * GB_SCREEN_WIDTH + 5];
        failed |= output[16 * width + 10] != original || output[17 * width + 11] != original;
        /* Flipped sprite column 0 lands at screen x 47; within its block the HD
         * columns run right to left, so the marker is the block's right pixel. */
        const size_t block = (size_t)(32 * SCALE) * width + 47 * SCALE;
        failed |= output[block + 1] != 0xFF00FFFFu || output[block] != 0xFF0000C8u;
        GBTexturePackStats stats = gbrt_texture_pack_stats();
        failed |= stats.tiles_matched != 2 || stats.frames_composed != 1 ||
                  stats.pixels_replaced != GB_FRAMEBUFFER_SIZE - 8u * GB_SCREEN_WIDTH;
        if (failed) {
            fprintf(stderr, "compose output %08X %08X block %08X %08X matched %u replaced %llu\n",
                    output[0], output[1], output[block], output[block + 1], stats.tiles_matched,
                    (unsigned long long)stats.pixels_replaced);
        }
    }
    free(output);
    gbrt_texture_pack_unload();
    gb_context_destroy(ctx);
    return failed;
}

int main(void) {
    for (size_t i = 0; i < sizeof(g_rom); ++i) {
        g_rom[i] = (uint8_t)(i * 7u);
    }
    int failed = test_loader();
    failed |= test_ppu_composition();
    if (!failed) {
        printf("texture pack tests passed\n");
    }
    return failed;
}
