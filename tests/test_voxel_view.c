#include "gbrt.h"
#include "gbrt_hash.h"
#include "gbrt_texture_pack.h"
#include "gbrt_voxel.h"
#include "ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The test drives gbrt_voxel_frame_complete directly with hand-built memory,
 * the way the PPU calls it at the end of a frame. */

static uint8_t g_rom[32u * 1024u];
static uint32_t g_base[160 * 144];
static uint32_t g_out_a[GB_VOXEL_OUTPUT_WIDTH * GB_VOXEL_OUTPUT_HEIGHT];
static uint32_t g_out_b[GB_VOXEL_OUTPUT_WIDTH * GB_VOXEL_OUTPUT_HEIGHT];
static const uint32_t g_shades[4] = {0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820};

#define GROUND_OBJ 0x04
#define BOX_OBJ 0x20

static const char k_profile[] =
    "voxel-profile 1\n"
    "# test profile\n"
    "playfield 160 128\n"
    "grid D711 10 8 16\n"
    "room FFF6\n"
    "require DBA5 00\n"
    "ground 3\n"
    "camera 55 2.0\n"
    "class ground ground\n"
    "class block box height=12\n"
    "object 04 ground\n"
    "object 20 block\n";

static int fail(const char* message) {
    fprintf(stderr, "voxel view: %s\n", message);
    return 1;
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

static void set_wram(GBContext* ctx, uint16_t addr, uint8_t value) {
    if (addr < 0xD000) ctx->wram[addr - 0xC000] = value;
    else ctx->wram[0x1000 + (addr - 0xD000)] = value;
}

/* Room `room` of objects, all ground except a 2x2 block at cells (4..5, 3..4).
 * Every object ID gets its own tile pattern so the tilemap matches the grid. */
static void setup_room(GBContext* ctx, uint8_t room, uint8_t block_obj, int scx) {
    ctx->wram_bank = 1;
    ctx->io[0x40] = 0x81; /* LCD on, BG on, signed tile addressing, map 9800 */
    ctx->io[0x47] = 0xE4;
    ctx->io[0x43] = (uint8_t)scx;
    ctx->io[0x42] = 0;
    ctx->hram[0xF6 - 0x80] = room;
    set_wram(ctx, 0xDBA5, 0);
    /* tile 1: solid color 1; tile 2: solid color 3 */
    memset(ctx->vram + 0x1000 + 1 * 16, 0, 16);
    for (int i = 0; i < 16; i += 2) ctx->vram[0x1000 + 1 * 16 + i] = 0xFF;
    memset(ctx->vram + 0x1000 + 2 * 16, 0xFF, 16);
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 10; ++c) {
            const bool block = c >= 4 && c <= 5 && r >= 3 && r <= 4;
            const uint8_t obj = block ? block_obj : GROUND_OBJ;
            set_wram(ctx, (uint16_t)(0xD711 + r * 16 + c), obj);
            const int cx = ((scx / 16) + c) & 15;
            const uint8_t tile = obj == GROUND_OBJ ? 1 : 2;
            for (int ty = 0; ty < 2; ++ty) {
                for (int tx = 0; tx < 2; ++tx) {
                    ctx->vram[0x1800 + (r * 2 + ty) * 32 + ((cx * 2 + tx) & 31)] = tile;
                }
            }
        }
    }
}

static int test_loader(GBContext* ctx) {
    int line = 0;
    int failed = 0;
    failed |= gbrt_voxel_load_memory(ctx, "grid D711 10 8 16\n", 18, &line) != GB_VOXEL_ERROR_FORMAT;
    static const char bad[] = "voxel-profile 1\ngrid D711 10 8 16\nroom FFF6\nclass x pyramid\n";
    failed |= gbrt_voxel_load_memory(ctx, bad, sizeof(bad) - 1, &line) != GB_VOXEL_ERROR_FORMAT;
    failed |= line != 4;
    static const char unknown_class[] = "voxel-profile 1\ngrid D711 10 8 16\nroom FFF6\nobject 04 nope\n";
    failed |= gbrt_voxel_load_memory(ctx, unknown_class, sizeof(unknown_class) - 1, &line) != GB_VOXEL_ERROR_FORMAT;
    failed |= gbrt_voxel_is_loaded();

    /* `rom` lines restrict a profile to those ROMs; `title` is for launchers. */
    char text[1024];
    snprintf(text, sizeof(text), "%stitle Test view\nrom %064d\n", k_profile, 0);
    failed |= gbrt_voxel_load_memory(ctx, text, strlen(text), &line) != GB_VOXEL_ERROR_ROM_MISMATCH;
    uint8_t digest[32];
    gbrt_sha256(g_rom, sizeof(g_rom), digest);
    int used = snprintf(text, sizeof(text), "%stitle Test view\nrom %064d\nrom ", k_profile, 0);
    for (int i = 0; i < 32; ++i) used += snprintf(text + used, sizeof(text) - (size_t)used, "%02x", digest[i]);
    snprintf(text + used, sizeof(text) - (size_t)used, "\n");
    failed |= gbrt_voxel_load_memory(ctx, text, strlen(text), &line) != GB_VOXEL_OK;
    gbrt_voxel_unload();

    failed |= gbrt_voxel_load_memory(ctx, k_profile, sizeof(k_profile) - 1, &line) != GB_VOXEL_OK;
    failed |= !gbrt_voxel_is_loaded() || !gbrt_voxel_is_enabled();
    failed |= gbrt_pixel_sources == NULL; /* the view records pixel sources */
    return failed ? fail("loader") : 0;
}

static int differs(const uint32_t* a, const uint32_t* b) {
    return memcmp(a, b, sizeof(g_out_a)) != 0;
}

static int test_render_and_gates(GBContext* ctx) {
    int failed = 0;
    for (int i = 0; i < 160 * 144; ++i) g_base[i] = 0xFF102030u + (uint32_t)i;

    /* Nothing composes before a frame completes. */
    failed |= gbrt_voxel_compose(g_base, g_shades, g_out_a);

    setup_room(ctx, 0x92, GROUND_OBJ, 0);
    uint8_t wram_before[0x2000];
    uint8_t vram_before[VRAM_SIZE];
    memcpy(wram_before, ctx->wram, sizeof(wram_before));
    memcpy(vram_before, ctx->vram, sizeof(vram_before));
    gbrt_voxel_frame_complete(ctx);
    failed |= !gbrt_voxel_compose(g_base, g_shades, g_out_a);
    failed |= memcmp(wram_before, ctx->wram, sizeof(wram_before)) != 0;
    failed |= memcmp(vram_before, ctx->vram, sizeof(vram_before)) != 0;
    if (failed) return fail("flat room did not compose, or touched guest memory");

    /* The HUD rows below the playfield are copied flat at 4x. */
    const size_t hud = (size_t)(136 * 4) * GB_VOXEL_OUTPUT_WIDTH + 12u * 4u;
    failed |= g_out_a[hud] != g_base[136 * 160 + 12];
    if (failed) return fail("rows below the playfield are not the flat base image");

    /* A raised block changes the picture; the same room reloaded matches. */
    gbrt_voxel_unload();
    int line = 0;
    gbrt_voxel_load_memory(ctx, k_profile, sizeof(k_profile) - 1, &line);
    setup_room(ctx, 0x92, BOX_OBJ, 0);
    gbrt_voxel_frame_complete(ctx);
    failed |= !gbrt_voxel_compose(g_base, g_shades, g_out_b);
    failed |= !differs(g_out_a, g_out_b);
    const GBVoxelStats with_block = gbrt_voxel_stats();
    if (failed) return fail("a box object did not change the rendered scene");

    /* Indoors (require DBA5 00 fails): no 3D. */
    set_wram(ctx, 0xDBA5, 1);
    gbrt_voxel_frame_complete(ctx);
    failed |= gbrt_voxel_compose(g_base, g_shades, g_out_b);
    set_wram(ctx, 0xDBA5, 0);
    gbrt_voxel_frame_complete(ctx);
    failed |= !gbrt_voxel_compose(g_base, g_shades, g_out_b);
    if (failed) return fail("the require condition does not gate the view");

    /* Disabled view does not compose. */
    gbrt_voxel_set_enabled(false);
    failed |= gbrt_voxel_compose(g_base, g_shades, g_out_b);
    gbrt_voxel_set_enabled(true);

    /* A text box: tiles change under unchanged objects -> those cells draw flat. */
    for (int ty = 12; ty < 16; ++ty) {
        for (int tx = 2; tx < 18; ++tx) ctx->vram[0x1800 + ty * 32 + tx] = 3;
    }
    gbrt_voxel_frame_complete(ctx);
    failed |= !gbrt_voxel_compose(g_base, g_shades, g_out_b);
    const GBVoxelStats boxed = gbrt_voxel_stats();
    failed |= boxed.overlay_cells != 16u;
    const size_t flat = (size_t)((13 * 8) * 4 + 1) * GB_VOXEL_OUTPUT_WIDTH + (size_t)(6 * 8 * 4 + 1);
    failed |= g_out_b[flat] != g_base[(13 * 8) * 160 + 6 * 8];
    if (failed) {
        fprintf(stderr, "overlay cells %u (block frame had %u)\n", boxed.overlay_cells,
                with_block.overlay_cells);
        return fail("changed tiles are not drawn flat");
    }

    /* Covering most of the screen (a menu) falls back to 2D. */
    for (int ty = 0; ty < 16; ++ty) {
        for (int tx = 0; tx < 20; ++tx) ctx->vram[0x1800 + ty * 32 + tx] = 3;
    }
    gbrt_voxel_frame_complete(ctx);
    failed |= gbrt_voxel_compose(g_base, g_shades, g_out_b);
    if (failed) return fail("a mostly replaced screen still composed in 3D");
    return 0;
}

static int test_props(GBContext* ctx) {
    int failed = 0;
    int line = 0;
    static const char bad_quadrant[] =
        "voxel-profile 1\ngrid D711 10 8 16\nroom FFF6\nclass post prop span=1 top_rows=6\nobject 20 post 1 0\n";
    gbrt_voxel_unload();
    failed |= gbrt_voxel_load_memory(ctx, bad_quadrant, sizeof(bad_quadrant) - 1, &line) != GB_VOXEL_ERROR_FORMAT;
    failed |= line != 5;
    if (failed) return fail("a prop quadrant outside its span loaded");

    static const char props[] =
        "voxel-profile 1\nplayfield 160 128\ngrid D711 10 8 16\nroom FFF6\n"
        "class ground ground\nclass tree prop span=2 top_rows=6 canopy=4\n"
        "object 04 ground\nobject 20 tree 0 0\n";
    gbrt_voxel_load_memory(ctx, props, sizeof(props) - 1, &line);
    memset(ctx->vram + 0x1800, 0, 0x400);
    setup_room(ctx, 0x92, GROUND_OBJ, 0);
    gbrt_voxel_frame_complete(ctx);
    failed |= !gbrt_voxel_compose(g_base, g_shades, g_out_a);
    const uint32_t flat_faces = gbrt_voxel_stats().faces_drawn;

    /* The 2x2 block of object 20 is one prop: the four cells share one art square. */
    setup_room(ctx, 0x92, BOX_OBJ, 0);
    gbrt_voxel_frame_complete(ctx);
    failed |= !gbrt_voxel_compose(g_base, g_shades, g_out_b);
    failed |= !differs(g_out_a, g_out_b);
    failed |= gbrt_voxel_stats().faces_drawn <= flat_faces;
    if (failed) return fail("a prop object did not render");
    return 0;
}

static int test_transition(GBContext* ctx) {
    int failed = 0;
    gbrt_voxel_unload();
    int line = 0;
    gbrt_voxel_load_memory(ctx, k_profile, sizeof(k_profile) - 1, &line);
    memset(ctx->vram + 0x1800, 0, 0x400);
    setup_room(ctx, 0x92, GROUND_OBJ, 0);
    gbrt_voxel_frame_complete(ctx);
    failed |= !gbrt_voxel_compose(g_base, g_shades, g_out_a);

    /* Walk right into room 93, whose block cells are drawn as the screen scrolls. */
    setup_room(ctx, 0x93, BOX_OBJ, 160);
    for (int scx = 4; scx <= 160; scx += 4) {
        ctx->io[0x43] = (uint8_t)scx;
        gbrt_voxel_frame_complete(ctx);
        if (!gbrt_voxel_compose(g_base, g_shades, g_out_b)) {
            fprintf(stderr, "transition fell back at scx=%d\n", scx);
            return fail("transition");
        }
        if (gbrt_voxel_stats().overlay_cells != 0) {
            fprintf(stderr, "overlay cells %u at scx=%d\n", gbrt_voxel_stats().overlay_cells, scx);
            return fail("transition cells drawn flat");
        }
    }

    /* After landing, the same view from a fresh load must match exactly. */
    gbrt_voxel_unload();
    gbrt_voxel_load_memory(ctx, k_profile, sizeof(k_profile) - 1, &line);
    gbrt_voxel_frame_complete(ctx);
    failed |= !gbrt_voxel_compose(g_base, g_shades, g_out_a);
    failed |= differs(g_out_a, g_out_b);
    return failed ? fail("landed room differs from a freshly tracked one") : 0;
}

int main(void) {
    GBContext* ctx = make_context();
    if (!ctx) return fail("context");
    int failed = 0;
    failed |= test_loader(ctx);
    if (!failed) failed |= test_render_and_gates(ctx);
    if (!failed) failed |= test_props(ctx);
    if (!failed) failed |= test_transition(ctx);
    gbrt_voxel_unload();
    failed |= gbrt_pixel_sources != NULL;
    gb_context_destroy(ctx);
    if (!failed) printf("voxel view: ok\n");
    return failed;
}
