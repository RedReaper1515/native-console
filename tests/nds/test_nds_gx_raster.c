/*
 * The DS rasteriser against hardware recordings.
 *
 * Two sets of captures, both made by programs that draw with the 3D engine
 * and read the picture back through the display capture unit. Each is fed
 * to the module through its register interface, exactly as its program
 * sent it, and the rendered lines are compared with what hardware drew.
 *
 *   polyrastertest   Jaklyy/polyrastertest v1.0.2-b (MIT): 77 scenes, each a
 *                    polygon or two chosen to show one rule of the
 *                    rasteriser (fill rules, swapped edges, lines, clipped
 *                    polygons, edge marking, anti-aliasing), with the
 *                    pixels a New 3DS XL drew. The scenes and the recording
 *                    are in gx/polyrastertest_data.h.
 *   nds-interp       StrikerX3/nds-interp (MIT): every line from each
 *                    corner of the screen to every point (0..256, 0..192),
 *                    198,404 lines, as a DS drew them. The four data files
 *                    are not in the repository: tools/nds/gx_unpack_interp.py
 *                    unpacks them, and --interp DIR says where they are.
 *
 *   test_nds_gx_raster [--interp DIR | --skip-interp] [--full] [--hash]
 *                      [--scene N] [--line CORNER X Y] [--bench [FRAMES]]
 *
 * Without --full every 16th nds-interp line is drawn (plus all the short
 * ones, where the special cases are); with it, all of them. --scene N
 * prints the differences for one polyrastertest scene, and --line draws one
 * nds-interp line (corner 0-3: top left, top right, bottom left, bottom
 * right), for looking into a failure.
 *
 * Before the recordings come scenes that check the rendering rules the
 * recordings do not reach (textures, blending, depth, fog, shadows) against
 * GBATEK's definitions; see "Rendering rules" below.
 *
 * --hash prints a hash of every frame the run rendered: two builds of the
 * module that print the same hash drew the same pixels. --bench times the
 * rendering of a frame of 1,024 textured triangles instead of testing.
 *
 * A run with neither recording to hand fails: no check, no pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nds_gx.h"

#include "gx/polyrastertest_data.h"

static NdsGx g_gx;
static long g_checks, g_failures;
/* Texture memory for the scenes that use it; the hardware scenes have none. */
static const NdsGxVram *g_vram;
/* --hash: FNV-1a over every rendered frame. */
static int g_hashing;
static uint64_t g_hash = 0xCBF29CE484222325u;
static long g_hashed_frames;

static void render(void)
{
    nds_gx_render(&g_gx, g_vram);
    if (!g_hashing) return;
    for (int y = 0; y < NDS_GX_HEIGHT; y++) {
        const uint32_t *line = nds_gx_line(&g_gx, y);
        for (int x = 0; x < NDS_GX_WIDTH; x++) {
            for (int byte = 0; byte < 4; byte++) {
                g_hash ^= (line[x] >> (8 * byte)) & 0xFFu;
                g_hash *= 0x100000001B3u;
            }
        }
    }
    g_hashed_frames++;
}

/* ------------------------------------------------------------------ */
/* Driving the module the way a program does                           */
/* ------------------------------------------------------------------ */

/* Let the geometry engine finish what it has. */
static void drain(void)
{
    uint32_t cycles;
    while ((cycles = nds_gx_cycles_until_event(&g_gx)) != 0) nds_gx_run(&g_gx, cycles);
}

/* A write to a geometry command port. A full FIFO stalls the CPU. */
static void command(uint32_t port, uint32_t value)
{
    if (nds_gx_fifo_full(&g_gx)) drain();
    nds_gx_write(&g_gx, 0x04000000u + port, value, 4);
}

static void vertex16(int x, int y, int z)
{
    command(0x48C, ((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
    command(0x48C, (uint16_t)z);
}

/* SWAP_BUFFERS, the VBlank that carries it out, and the frame after. */
static void swap_and_render(uint32_t swap)
{
    command(0x540, swap);
    drain();
    nds_gx_vblank(&g_gx);
    drain();
    render();
}

/* What the display capture unit stores for a pixel of the 3D layer: the
 * top five bits of each 6-bit channel, as BGR555. */
static unsigned captured(uint32_t pixel)
{
    return ((pixel >> 1) & 31u) | (((pixel >> 7) & 31u) << 5) | (((pixel >> 13) & 31u) << 10);
}

/* ------------------------------------------------------------------ */
/* polyrastertest                                                      */
/* ------------------------------------------------------------------ */

/* The recording is a bit stream, most significant bit first. */
static size_t g_bit;

static unsigned take(unsigned count)
{
    unsigned value = 0;
    for (unsigned i = 0; i < count; i++, g_bit++) {
        unsigned byte = (g_bit >> 3) < PRT_DATA_SIZE ? prt_data[g_bit >> 3] : 0;
        value = (value << 1) | ((byte >> (7 - (g_bit & 7))) & 1u);
    }
    return value;
}

/* main.cpp: a scene's lines list every run of pixels, not only the first,
 * when its first polygon is wireframe or it has an extended polygon. */
static int multispan(const PrtTest *t)
{
    return (t->first.attr & (31u << 16)) == 0 || t->extended != 0;
}

/* What the recording says about one scene: per pixel, whether hardware
 * drew something there and, where the scene records colour, which. */
typedef struct Expected {
    unsigned char lit[192][256];     /* 1: a pixel that is not black */
    unsigned char checked[192][256]; /* 1: the recording covers this pixel */
    unsigned short color[192][256];  /* its colour, where has_color */
    unsigned char has_color[192][256];
} Expected;

static void decode_scene(const PrtTest *t, Expected *e)
{
    unsigned bits = (t->color_mode > 3 ? 3u : t->color_mode) * 5u;
    memset(e, 0, sizeof *e);
    for (int y = 0; y < 192; y++) {
        int covered_to = -1; /* the recording vouches for columns up to here */
        while (take(1)) {
            unsigned start = take(8), end = take(8);
            for (unsigned x = start; x <= end; x++) {
                e->lit[y][x] = 1;
                /* record() left out the colours of a run that reaches the
                 * last column. */
                if (bits && end != 255) {
                    e->color[y][x] = (unsigned short)take(bits);
                    e->has_color[y][x] = 1;
                }
            }
            covered_to = (int)end;
            if (!multispan(t)) break;
        }
        /* Everything outside the runs is black: the whole line when every
         * run is listed. When only the first is, the program still demands
         * black after it, having drawn nothing else. */
        (void)covered_to;
        memset(e->checked[y], 1, 256);
    }
}

/* drawPoly() of main.cpp. The extended polygon's own PolyAttr is in the
 * table but this release never sends it: glBegin() latches the first
 * polygon's attributes again. */
static void draw_polygon(const PrtPolygon *p)
{
    int quad = p->vertices[3][0] != -512;
    command(0x500, quad ? 1 : 0); /* BEGIN_VTXS */
    for (int i = 0; i < (quad ? 4 : 3); i++) {
        command(0x480, p->colors[i]); /* COLOR */
        vertex16(p->vertices[i][0], p->vertices[i][1], p->vertices[i][2]);
    }
}

static void draw_scene(const PrtTest *t)
{
    /* glClearColor(0,0,0,31), glClearPolyID, glClearDepth */
    nds_gx_write(&g_gx, 0x04000350, (31u << 16) | ((uint32_t)t->rear_id << 24), 4);
    nds_gx_write(&g_gx, 0x04000354, t->rear_depth, 2);
    for (uint32_t i = 0; i < 8; i++) nds_gx_write(&g_gx, 0x04000330 + 2 * i, t->outline[i], 2);
    command(0x580, t->viewport);
    command(0x440, 0); /* glMatrixMode(GL_PROJECTION) */
    for (int i = 0; i < 16; i++) command(0x458, (uint32_t)t->projection[i]);
    command(0x440, 2); /* glMatrixMode(GL_MODELVIEW) */
    command(0x454, 0); /* glLoadIdentity */
    command(0x4A4, t->first.attr);
    nds_gx_write(&g_gx, 0x04000060, t->disp3dcnt, 2);
    draw_polygon(&t->first);
    if (t->extended) draw_polygon(&prt_extended[t->extended - 1]);
    swap_and_render(0);
}

/* Compare the rendered frame with the recording as the program's test()
 * does. Returns the number of pixels that differ; prints them if asked. */
static long compare_scene(const PrtTest *t, const Expected *e, int show)
{
    unsigned mask = t->color_mode == 1 ? 0x1Fu : t->color_mode == 2 ? 0x3FFu : 0x7FFFu;
    long wrong = 0;
    for (int y = 0; y < 192; y++) {
        const uint32_t *line = nds_gx_line(&g_gx, y);
        for (int x = 0; x < 256; x++) {
            unsigned color = captured(line[x]);
            const char *what = NULL;
            if (!e->checked[y][x]) continue;
            if (!e->lit[y][x]) {
                if (color != 0) what = "drawn, hardware has nothing";
            } else if (color == 0) {
                what = "missing";
            } else if (e->has_color[y][x] && (color & mask) != e->color[y][x]) {
                what = "wrong colour";
            }
            if (!what) continue;
            wrong++;
            if (show && wrong <= 400) {
                printf("    (%3d,%3d) %s: ours %04X", x, y, what, color & mask);
                if (e->has_color[y][x]) printf(", hardware %04X", e->color[y][x]);
                printf("\n");
            }
        }
    }
    return wrong;
}

/* The rendered frame as runs of lit pixels with their colours, in the form
 * tools/nds/gx_polyraster_convert.py --show prints the recording. */
static void print_frame(void)
{
    for (int y = 0; y < 192; y++) {
        const uint32_t *line = nds_gx_line(&g_gx, y);
        int any = 0;
        for (int x = 0; x < 256; x++) {
            if (captured(line[x]) == 0) continue;
            int end = x;
            while (end < 255 && captured(line[end + 1]) != 0) end++;
            if (!any) printf("    ours y=%3d:", y);
            printf("  %d-%d[", x, end);
            for (int i = x; i <= end; i++)
                if (i == x || captured(line[i]) != captured(line[i - 1])) printf("%s%d:%04X", i == x ? "" : " ", i, captured(line[i]));
            printf("]");
            any = 1;
            x = end;
        }
        if (any) printf("\n");
    }
}

static void reset_engine(void)
{
    nds_gx_reset(&g_gx);
}

static void run_polyrastertest(int show_scene)
{
    static Expected expected;
    const char *category = "";
    int category_total = 0, category_passed = 0, total_passed = 0;

    printf("polyrastertest v1.0.2-b: %d scenes\n", PRT_NUM_TESTS);
    reset_engine();
    g_bit = 0;
    if (take(16) != PRT_DATA_VERSION) {
        printf("  the recording has an unexpected version\n");
        g_checks++;
        g_failures++;
        return;
    }
    for (int i = 0; i < PRT_NUM_TESTS; i++) {
        const PrtTest *t = &prt_tests[i];
        if (strcmp(category, t->category) != 0) {
            if (category_total) printf("  %-28s %d / %d\n", category, category_passed, category_total);
            category = t->category;
            category_total = category_passed = 0;
        }
        decode_scene(t, &expected);
        draw_scene(t);
        long wrong = compare_scene(t, &expected, 0);
        g_checks++;
        category_total++;
        if (wrong == 0) {
            category_passed++;
            total_passed++;
        } else {
            g_failures++;
            printf("  FAIL scene %d (main.h line %d), %s / %s / %s: %ld pixels differ\n", i + 1, t->line,
                   t->category, t->sub, t->name, wrong);
        }
        if (show_scene == i + 1) {
            printf("  scene %d: %s / %s / %s\n", i + 1, t->category, t->sub, t->name);
            compare_scene(t, &expected, 1);
            print_frame();
        }
    }
    if (category_total) printf("  %-28s %d / %d\n", category, category_passed, category_total);
    printf("  polyrastertest: %d of %d scenes match hardware\n", total_passed, PRT_NUM_TESTS);
}

/* ------------------------------------------------------------------ */
/* nds-interp                                                          */
/* ------------------------------------------------------------------ */

/* nds-interp/data/src/source/main.cpp: the frame the capture program set
 * up once, and the line it drew each frame: a wireframe triangle with two
 * vertices on the corner and the third on the target point. Its projection
 * maps x, y of -0x3000..0x3000 onto the screen, W being 0x3000. */
#define X_STEP (0x3000 / 128)
#define Y_STEP (0x3000 / 96)

static void interp_setup(void)
{
    static const int32_t clip[16] = {0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x3000};
    reset_engine();
    nds_gx_write(&g_gx, 0x04000350, (31u << 16) | (63u << 24), 4); /* black, opaque, ID 63 */
    nds_gx_write(&g_gx, 0x04000354, 0x7FFF, 2);
    nds_gx_write(&g_gx, 0x04000060, 1u << 3, 2); /* glDisable(GL_ANTIALIAS); glEnable(GL_BLEND) */
    command(0x580, (255u << 16) | (191u << 24)); /* glViewport(0,0,255,191) */
    command(0x440, 0);
    for (int i = 0; i < 16; i++) command(0x458, (uint32_t)clip[i]);
    command(0x440, 2);
    command(0x454, 0);
    command(0x4A4, 3u << 6); /* POLY_ALPHA(0) | POLY_CULL_NONE */
}

static void interp_draw(int corner, int x, int y)
{
    int origin_x = -128 * X_STEP + ((corner & 1) ? 256 * X_STEP : 0);
    int origin_y = 96 * Y_STEP - ((corner & 2) ? 192 * Y_STEP : 0);
    command(0x500, 0);
    command(0x480, 0x7FFF);
    vertex16(origin_x, origin_y, 0);
    vertex16(origin_x, origin_y, 0);
    vertex16(-128 * X_STEP + x * X_STEP, 96 * Y_STEP - y * Y_STEP, 0);
    swap_and_render(0);
}

/* The capture program's countAndWrite(): the first run of lit pixels in
 * columns first_x..last_x of a line. */
static void interp_scan(int y, int first_x, int last_x, unsigned char out[3])
{
    const uint32_t *line = nds_gx_line(&g_gx, y);
    int found = 0, first = first_x, last = last_x;
    for (int x = first_x; x <= last_x; x++) {
        unsigned color = captured(line[x]);
        if (!found) {
            if (color != 0) {
                found = 1;
                first = x;
            }
        } else if (color == 0) {
            last = x - 1;
            break;
        }
    }
    out[0] = (unsigned char)found;
    out[1] = (unsigned char)first;
    out[2] = (unsigned char)last;
}

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

/* Check one line against its record: 3 bytes for each scanline the
 * capture program looked at. Returns the number of scanlines that differ. */
static int interp_check(int corner, int x, int y, const unsigned char *record, int show)
{
    int first_x = (corner & 1) ? min_int(x, 255) : 0, last_x = (corner & 1) ? 255 : min_int(x, 255);
    int first_y = (corner & 2) ? min_int(y, 191) : 0, last_y = (corner & 2) ? 191 : min_int(y, 191);
    int wrong = 0;
    interp_draw(corner, x, y);
    for (int line = first_y; line <= last_y; line++, record += 3) {
        unsigned char ours[3];
        interp_scan(line, first_x, last_x, ours);
        if (memcmp(ours, record, 3) != 0) {
            wrong++;
            if (show)
                printf("    line %3d: ours %s %d-%d, hardware %s %d-%d\n", line, ours[0] ? "lit" : "dark", ours[1],
                       ours[2], record[0] ? "lit" : "dark", record[1], record[2]);
        } else if (show && ours[0]) {
            printf("    line %3d: %d-%d\n", line, ours[1], ours[2]);
        }
    }
    return wrong;
}

static unsigned char *load_file(const char *dir, const char *name, long *size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (unsigned char *)malloc((size_t)*size);
    if (data && fread(data, 1, (size_t)*size, f) != (size_t)*size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

/* A capture file (nds-interp/main.cpp, readFile): a 7-byte header, then for
 * each target point in row order a 2-byte tag and the record of the line
 * drawn the frame before, and at the end the record of the last line. A
 * line's record is thus tagged with its own target; the very first record,
 * taken before anything was drawn, is replaced by the second. Returns an
 * index of where each line's record starts. */
static const unsigned char **interp_index(const unsigned char *data, long size, int corner)
{
    const unsigned char **index = (const unsigned char **)calloc(257 * 193, sizeof *index);
    if (!index || size < 7 || data[0] != corner || data[1] != 0 || data[2] != 0 || data[3] != 0 || data[4] != 1 ||
        data[5] != 0 || data[6] != 192) {
        free(index);
        return NULL;
    }
    const unsigned char *at = data + 7, *end = data + size;
    int prev_x = 0, prev_y = 0;
    for (int n = 0; n <= 257 * 193; n++) {
        if (n < 257 * 193) {
            if (end - at < 2 || at[0] != (unsigned char)prev_x || at[1] != (unsigned char)prev_y) break;
            at += 2;
        }
        int lines = (corner & 2) ? 192 - min_int(prev_y, 191) : min_int(prev_y, 191) + 1;
        if (end - at < lines * 3) break;
        index[prev_y * 257 + prev_x] = at;
        at += lines * 3;
        prev_x = n % 257;
        prev_y = n / 257;
        if (n == 257 * 193) return at == end ? index : NULL;
    }
    free(index);
    return NULL;
}

static void run_interp(const char *dir, int full)
{
    static const char *const names[4] = {"TL.bin", "TR.bin", "BL.bin", "BR.bin"};
    static const char *const corners[4] = {"top left", "top right", "bottom left", "bottom right"};
    long all_lines = 0, all_matching = 0;

    printf("nds-interp captures in %s (%s)\n", dir, full ? "every line" : "every 16th line and the short ones");
    for (int corner = 0; corner < 4; corner++) {
        long size = 0;
        unsigned char *data = load_file(dir, names[corner], &size);
        const unsigned char **index = data ? interp_index(data, size, corner) : NULL;
        if (!index) {
            printf("  %s: missing or not a capture file\n", names[corner]);
            g_checks++;
            g_failures++;
            free(data);
            continue;
        }
        interp_setup();
        long lines = 0, matching = 0, reported = 0;
        for (int y = 0; y <= 192; y++) {
            for (int x = 0; x <= 256; x++) {
                /* Distances from the corner, so that the short lines of all
                 * four corners are in the default set. */
                int dx = (corner & 1) ? 256 - x : x, dy = (corner & 2) ? 192 - y : y;
                if (!full && (x + y * 257) % 16 != 0 && dx > 3 && dy > 3) continue;
                int wrong = interp_check(corner, x, y, index[y * 257 + x], 0);
                lines++;
                g_checks++;
                if (wrong == 0) {
                    matching++;
                } else {
                    g_failures++;
                    if (reported++ < 10) printf("  FAIL %s to (%d,%d): %d scanlines differ\n", corners[corner], x, y, wrong);
                }
            }
        }
        printf("  %-12s %ld of %ld lines match hardware\n", corners[corner], matching, lines);
        all_lines += lines;
        all_matching += matching;
        free((void *)index);
        free(data);
    }
    printf("  nds-interp: %ld of %ld lines match hardware\n", all_matching, all_lines);
}

static void show_interp_line(const char *dir, int corner, int x, int y)
{
    static const char *const names[4] = {"TL.bin", "TR.bin", "BL.bin", "BR.bin"};
    long size = 0;
    unsigned char *data = load_file(dir, names[corner & 3], &size);
    const unsigned char **index = data ? interp_index(data, size, corner & 3) : NULL;
    if (!index || x < 0 || x > 256 || y < 0 || y > 192) {
        printf("no such capture\n");
        return;
    }
    interp_setup();
    printf("corner %d to (%d,%d):\n", corner, x, y);
    interp_check(corner & 3, x, y, index[y * 257 + x], 1);
    free((void *)index);
    free(data);
}

/* ------------------------------------------------------------------ */
/* Rendering rules from the documentation                              */
/* ------------------------------------------------------------------ */

/*
 * The recordings have no texture in them, no fog, no shadow, and no two
 * polygons at different depths. These scenes check those parts of the
 * rendering engine against GBATEK's definitions ("DS 3D Texture Formats",
 * "DS 3D Texture Blending", "DS 3D Toon, Edge, Fog, Alpha-Blending,
 * Anti-Aliasing", "DS 3D Shadow Polygons", "DS 3D Rear-Plane"), with the
 * expected pixels worked out here from its equations. Where GBATEK and
 * melonDS differ and the module follows melonDS, the check says so; no
 * hardware capture stands behind any of these.
 */

static uint8_t g_texture[4][0x20000];
static uint8_t g_palette[6][0x4000];
static NdsGxVram g_memory;
static const char *g_rule = "";
static long g_rule_checks, g_rule_failures;

static void rule(const char *name, long got, long want, int line)
{
    g_checks++;
    g_rule_checks++;
    if (got == want) return;
    g_failures++;
    g_rule_failures++;
    printf("  FAIL %s: %s (line %d): got %ld (0x%lX), want %ld (0x%lX)\n", g_rule, name, line, got,
           (unsigned long)got, want, (unsigned long)want);
}

#define RULE(name, got, want) rule(name, (long)(got), (long)(want), __LINE__)

#define RGB15(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 5) | ((uint32_t)(b) << 10))
/* A pixel as nds_gx_line() gives it: 6 bits a channel, 5 of alpha. */
#define PIXEL(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 6) | ((uint32_t)(b) << 12) | ((uint32_t)(a) << 18))

/* POLYGON_ATTR */
#define BOTH_SIDES (3u << 6)
#define MODE(n) ((uint32_t)(n) << 4)
#define NEW_DEPTH (1u << 11)
#define DEPTH_EQUAL (1u << 14)
#define FOGGED (1u << 15)
#define POLY_ALPHA(a) ((uint32_t)(a) << 16)
#define POLY_ID(n) ((uint32_t)(n) << 24)
#define SOLID (BOTH_SIDES | POLY_ALPHA(31))
/* DISP3DCNT */
#define TEXTURES (1u << 0)
#define HIGHLIGHT (1u << 1)
#define ALPHA_TEST (1u << 2)
#define BLENDING (1u << 3)
#define ANTIALIAS (1u << 4)
#define EDGE_MARKING (1u << 5)
#define FOG_ALPHA_ONLY (1u << 6)
#define FOG (1u << 7)
#define REAR_BITMAP (1u << 14)

/* "The 5bit RGB values are internally expanded to 6bit RGB as follows:
 * X=X*2+(X+31)/32" */
static uint32_t six(uint32_t c)
{
    return c * 2 + (c + 31) / 32;
}

static uint32_t pixel_of(uint32_t rgb15, uint32_t alpha)
{
    return PIXEL(six(rgb15 & 31u), six((rgb15 >> 5) & 31u), six((rgb15 >> 10) & 31u), alpha);
}

static uint32_t at(int x, int y)
{
    return nds_gx_line(&g_gx, y)[x];
}

static unsigned red(uint32_t pixel) { return pixel & 63u; }
static unsigned green(uint32_t pixel) { return (pixel >> 6) & 63u; }
static unsigned blue(uint32_t pixel) { return (pixel >> 12) & 63u; }
static unsigned alpha_of(uint32_t pixel) { return (pixel >> 18) & 31u; }

/*
 * A frame in which vertex coordinates are pixels: x 0..256 from the left,
 * y 0..192 from the top, and z -0x3000 (near) to 0x3000 (far). W is 0x3000
 * for every vertex, so attributes are interpolated linearly. The rear plane
 * is black, opaque, polygon ID 0 and as far as can be.
 */
static void space(uint32_t disp3dcnt)
{
    static const int32_t projection[16] = {
        96 * 0x1000, 0, 0, 0, 0, -128 * 0x1000, 0, 0, 0, 0, 0x1000, 0, -0x3000, 0x3000, 0, 0x3000,
    };
    reset_engine();
    g_memory.texture[0] = g_texture[0];
    g_memory.texture[1] = g_texture[1];
    g_memory.texture[2] = g_texture[2];
    g_memory.texture[3] = g_texture[3];
    for (int i = 0; i < 6; i++) g_memory.palette[i] = g_palette[i];
    g_vram = &g_memory;
    nds_gx_write(&g_gx, 0x04000350, 31u << 16, 4);
    nds_gx_write(&g_gx, 0x04000354, 0x7FFF, 2);
    nds_gx_write(&g_gx, 0x04000060, disp3dcnt, 2);
    command(0x580, (255u << 16) | (191u << 24));
    command(0x440, 0);
    for (int i = 0; i < 16; i++) command(0x458, (uint32_t)projection[i]);
    command(0x440, 2);
    command(0x454, 0);
}

static void corner(int x, int y, int z, uint32_t color, int s, int t)
{
    command(0x480, color);
    command(0x488, ((uint32_t)(uint16_t)(t * 16) << 16) | (uint16_t)(s * 16)); /* TEXCOORD, in texels */
    vertex16(x, y, z);
}

/* A rectangle over pixels x0..x1-1, y0..y1-1, showing texels (s0, t0) to
 * (s1, t1). It faces the viewer. */
static void textured_rect(uint32_t attr, int x0, int y0, int x1, int y1, int z, uint32_t color, int s0, int t0,
                          int s1, int t1)
{
    command(0x4A4, attr);
    command(0x500, 1);
    corner(x0, y0, z, color, s0, t0);
    corner(x0, y1, z, color, s0, t1);
    corner(x1, y1, z, color, s1, t1);
    corner(x1, y0, z, color, s1, t0);
}

static void rect(uint32_t attr, int x0, int y0, int x1, int y1, int z, uint32_t color)
{
    textured_rect(attr, x0, y0, x1, y1, z, color, 0, 0, 0, 0);
}

static void show(void)
{
    swap_and_render(0);
}

#define WHITE RGB15(31, 31, 31)

static void rules_rear_plane(void)
{
    g_rule = "rear plane";

    /* CLEAR_COLOR: colour in bits 0-14, alpha in bits 16-20. */
    space(0);
    nds_gx_write(&g_gx, 0x04000350, RGB15(31, 0, 16) | (20u << 16), 4);
    show();
    RULE("clear colour", at(0, 0), PIXEL(63, 0, 33, 20));
    RULE("clear colour, last pixel", at(255, 191), PIXEL(63, 0, 33, 20));
    nds_gx_write(&g_gx, 0x04000350, RGB15(31, 31, 31), 4);
    show();
    RULE("alpha 0: nothing there", alpha_of(at(100, 100)), 0);

    /* The rendering registers are sampled at VBlank: a write after it
     * shows a frame later (melonDS). */
    nds_gx_write(&g_gx, 0x04000350, RGB15(0, 31, 0) | (31u << 16), 4);
    render();
    RULE("written after VBlank: not yet", alpha_of(at(100, 100)), 0);
    nds_gx_vblank(&g_gx);
    render();
    RULE("the frame after", at(100, 100), PIXEL(0, 63, 0, 31));

    /* CLEAR_DEPTH: 15 bits, expanded to 24. GBATEK has
     * "X=(X*200h)+((X+1)/8000h)*1FFh"; melonDS found the low nine bits set
     * for every value, and that is what is checked: a polygon at depth
     * 0x7FFE00 (z = 0 here) is in front of a rear plane at 0x3FFF, which is
     * then 0x7FFFFF, and behind one at 0x3FFE. */
    space(0);
    nds_gx_write(&g_gx, 0x04000354, 0x3FFF, 2);
    rect(SOLID, 10, 10, 20, 20, 0, WHITE);
    show();
    RULE("in front of the rear plane", at(15, 15), PIXEL(63, 63, 63, 31));
    nds_gx_write(&g_gx, 0x04000354, 0x3FFE, 2);
    rect(SOLID, 10, 10, 20, 20, 0, WHITE);
    show();
    RULE("behind the rear plane", at(15, 15), PIXEL(0, 0, 0, 31));

    /* DISP3DCNT bit 14: the rear plane is a 256x256 bitmap of colours in
     * texture slot 2 (bit 15: alpha) and of depths in slot 3 (bit 15: fog),
     * scrolled by CLRIMAGE_OFFSET; "The bitmap wraps". */
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned y = 0; y < 256; y++) {
        for (unsigned x = 0; x < 256; x++) {
            unsigned color = RGB15(x & 31u, y & 31u, (x >> 5) & 7u) | (((x + y) & 3u) ? 0x8000u : 0u);
            unsigned depth = x < 128 ? 0x7FFFu : 0x1000u;
            nds_put_le16(&g_texture[2][(y * 256 + x) * 2], (uint16_t)color);
            nds_put_le16(&g_texture[3][(y * 256 + x) * 2], (uint16_t)depth);
        }
    }
    space(REAR_BITMAP);
    nds_gx_write(&g_gx, 0x04000356, 0, 2);
    rect(SOLID, 100, 50, 160, 60, 0, WHITE);
    show();
    RULE("bitmap pixel", at(5, 9), pixel_of(RGB15(5, 9, 0), 31));
    RULE("bitmap pixel, far side", at(250, 190), pixel_of(RGB15(250 & 31, 190 & 31, 7), (250 + 190) & 3 ? 31 : 0));
    RULE("bitmap pixel, far side, solid", at(251, 190), pixel_of(RGB15(251 & 31, 190 & 31, 7), 31));
    RULE("bitmap alpha bit clear", alpha_of(at(4, 8)), 0);
    RULE("polygon in front of the bitmap's depth", at(110, 55), PIXEL(63, 63, 63, 31));
    RULE("polygon behind the bitmap's depth", at(150, 55), pixel_of(RGB15(150 & 31, 55 & 31, 4), (150 + 55) & 3 ? 31 : 0));
    nds_gx_write(&g_gx, 0x04000356, 3 | (250u << 8), 2);
    show();
    RULE("bitmap scrolled", at(5, 9), pixel_of(RGB15(8, (9 + 250) & 31, 0), 31));
    RULE("bitmap scrolled, wrapped", at(254, 9), pixel_of(RGB15(1, (9 + 250) & 31, 0), ((257 + 259) & 3) ? 31 : 0));
    memset(g_texture, 0, sizeof g_texture);
}

static void rules_polygons(void)
{
    g_rule = "polygons";

    /* A rectangle covers its left and top edges and not its right and
     * bottom ones (polyrastertest shows as much on hardware); its colour is
     * the vertex colour, 5 bits to 6. */
    space(0);
    rect(SOLID, 10, 20, 50, 40, 0, RGB15(31, 16, 1));
    show();
    RULE("inside", at(30, 30), PIXEL(63, 33, 3, 31));
    RULE("top left corner", at(10, 20), PIXEL(63, 33, 3, 31));
    RULE("bottom right corner", at(49, 39), PIXEL(63, 33, 3, 31));
    RULE("left of it", at(9, 30), PIXEL(0, 0, 0, 31));
    RULE("right of it", at(50, 30), PIXEL(0, 0, 0, 31));
    RULE("above it", at(30, 19), PIXEL(0, 0, 0, 31));
    RULE("below it", at(30, 40), PIXEL(0, 0, 0, 31));

    /* Colours between vertices: linear here (every W is the same), on the
     * 9-bit colours the engine draws with, a 5-bit colour followed by four
     * ones (melonDS). Across 64 pixels from red to blue: the first pixel is
     * the left colour, the middle one half of each. */
    space(0);
    command(0x4A4, SOLID);
    command(0x500, 1);
    corner(64, 50, 0, RGB15(31, 0, 0), 0, 0);
    corner(64, 114, 0, RGB15(31, 0, 0), 0, 0);
    corner(128, 114, 0, RGB15(0, 0, 31), 0, 0);
    corner(128, 50, 0, RGB15(0, 0, 31), 0, 0);
    show();
    RULE("gradient: left", at(64, 80), PIXEL(63, 0, 0, 31));
    RULE("gradient: middle", at(96, 80), PIXEL(511 / 2 / 8, 0, 511 / 2 / 8, 31));
    RULE("gradient: right", at(127, 80), PIXEL(511 / 64 / 8, 0, 511 * 63 / 64 / 8, 31));
    {
        int steady = 1;
        for (int x = 65; x < 128; x++)
            if (red(at(x, 80)) > red(at(x - 1, 80)) || blue(at(x, 80)) < blue(at(x - 1, 80))) steady = 0;
        RULE("gradient: steady", steady, 1);
    }
    /* And down the edges. */
    space(0);
    command(0x4A4, SOLID);
    command(0x500, 1);
    corner(64, 50, 0, RGB15(0, 31, 0), 0, 0);
    corner(64, 114, 0, RGB15(0, 0, 0), 0, 0);
    corner(128, 114, 0, RGB15(0, 0, 0), 0, 0);
    corner(128, 50, 0, RGB15(0, 31, 0), 0, 0);
    show();
    RULE("vertical gradient: top", at(100, 50), PIXEL(0, 63, 0, 31));
    RULE("vertical gradient: middle", at(100, 82), PIXEL(0, 511 / 2 / 8, 0, 31));
    RULE("vertical gradient: bottom", at(100, 113), PIXEL(0, 511 / 64 / 8, 0, 31));

    /* "The interior of Wire-frame polygons is transparent (Alpha=0), and
     * only the lines at the polygon edges are rendered, using a fixed Alpha
     * value of 31." */
    space(0);
    rect(BOTH_SIDES | POLY_ALPHA(0), 10, 20, 50, 40, 0, WHITE);
    show();
    RULE("wireframe: top edge", at(30, 20), PIXEL(63, 63, 63, 31));
    RULE("wireframe: bottom edge", at(30, 39), PIXEL(63, 63, 63, 31));
    RULE("wireframe: left edge", at(10, 30), PIXEL(63, 63, 63, 31));
    RULE("wireframe: right edge", at(49, 30), PIXEL(63, 63, 63, 31));
    RULE("wireframe: inside", at(30, 30), PIXEL(0, 0, 0, 31));
    RULE("wireframe: inside, by the edge", at(11, 30), PIXEL(0, 0, 0, 31));

    /* "For Line Segments, use Triangles with twice the same vertex". */
    space(0);
    command(0x4A4, BOTH_SIDES | POLY_ALPHA(31));
    command(0x500, 0);
    corner(20, 20, 0, WHITE, 0, 0);
    corner(20, 20, 0, WHITE, 0, 0);
    corner(60, 60, 0, WHITE, 0, 0);
    show();
    RULE("line: start", at(20, 20), PIXEL(63, 63, 63, 31));
    RULE("line: middle", at(40, 40), PIXEL(63, 63, 63, 31));
    RULE("line: beside it", at(41, 40), PIXEL(0, 0, 0, 31));
    RULE("line: its end point is not drawn", at(60, 60), PIXEL(0, 0, 0, 31));
}

/* TEXIMAGE_PARAM */
#define TEX_REPEAT_S (1u << 16)
#define TEX_REPEAT_T (1u << 17)
#define TEX_FLIP_S (1u << 18)
#define TEX_FLIP_T (1u << 19)
#define TEX_COLOR0_CLEAR (1u << 29)

static uint32_t tex_param(unsigned format, unsigned s_size, unsigned t_size, uint32_t address)
{
    return (address >> 3) | ((uint32_t)s_size << 20) | ((uint32_t)t_size << 23) | ((uint32_t)format << 26);
}

/* An 8x8 texture drawn texel for pixel at (16, 16), white vertices. */
static void show_texture(uint32_t disp3dcnt, uint32_t param, uint32_t palette_base, uint32_t attr)
{
    space(disp3dcnt);
    command(0x4A8, param);
    command(0x4AC, palette_base);
    textured_rect(attr, 16, 16, 24, 24, 0, WHITE, 0, 0, 8, 8);
    show();
}

/* The colour the tests put at a palette index. */
static uint32_t palette_color(unsigned index)
{
    return RGB15(index & 31u, (index * 3u) & 31u, (index * 7u + 5u) & 31u);
}

static void fill_palette(uint32_t address, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        nds_put_le16(&g_palette[(address + 2 * i) >> 14][(address + 2 * i) & 0x3FFFu], (uint16_t)palette_color(i));
}

static void rules_texture_formats(void)
{
    g_rule = "texture formats";
    memset(g_texture, 0, sizeof g_texture);
    memset(g_palette, 0, sizeof g_palette);

    /* Format 4, 256 colours: "Each Texel occupies 8bit". In slot 1, with
     * the palette at 0x6000 (PLTT_BASE in 16-byte steps), in palette slot
     * 1. Texel (x, y) has index 1 + x + 8 y. With white vertices,
     * modulation leaves the texel's colour: ((Rt+1)*64-1)/64 = Rt. */
    for (unsigned i = 0; i < 64; i++) g_texture[1][0x1230 + i] = (uint8_t)(i + 1);
    fill_palette(0x6000, 256);
    show_texture(TEXTURES, tex_param(4, 0, 0, 0x21230), 0x6000 >> 4, SOLID);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) RULE("256 colours", at(16 + x, 16 + y), pixel_of(palette_color(1u + (unsigned)(x + 8 * y)), 31));
    RULE("beside the texture", at(24, 16), PIXEL(0, 0, 0, 31));
    /* DISP3DCNT bit 0 off, or texture format 0: the vertex colour. */
    show_texture(0, tex_param(4, 0, 0, 0x21230), 0x6000 >> 4, SOLID);
    RULE("texture mapping off", at(18, 18), PIXEL(63, 63, 63, 31));
    show_texture(TEXTURES, tex_param(0, 0, 0, 0x21230), 0x6000 >> 4, SOLID);
    RULE("format 0: no texture", at(18, 18), PIXEL(63, 63, 63, 31));
    /* Bit 29: "Color 0 of 4/16/256-Color Palettes (0=Displayed, 1=Made
     * Transparent)". */
    g_texture[1][0x1230 + 9] = 0;
    show_texture(TEXTURES, tex_param(4, 0, 0, 0x21230), 0x6000 >> 4, SOLID);
    RULE("colour 0 displayed", at(17, 17), pixel_of(palette_color(0), 31));
    show_texture(TEXTURES, tex_param(4, 0, 0, 0x21230) | TEX_COLOR0_CLEAR, 0x6000 >> 4, SOLID);
    RULE("colour 0 transparent", at(17, 17), PIXEL(0, 0, 0, 31));
    RULE("the other colours stay", at(18, 17), pixel_of(palette_color(11), 31));

    /* Format 3, 16 colours: "Each Texel occupies 4bit, the 1st Texel is
     * located in LSBs of 1st byte". Texel (x, y) has index (x + 3 y) & 15. */
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) g_texture[0][0x400 + (y * 8 + x) / 2] |= (uint8_t)(((x + 3 * y) & 15u) << (4 * (x & 1u)));
    show_texture(TEXTURES, tex_param(3, 0, 0, 0x400), 0x6000 >> 4, SOLID);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) RULE("16 colours", at(16 + x, 16 + y), pixel_of(palette_color((unsigned)(x + 3 * y) & 15u), 31));

    /* Format 2, 4 colours: "Each Texel occupies 2bit, the first Texel is
     * located in LSBs of 1st byte. In this format, the Palette Base is
     * specified in 8-byte steps". Palette at 0x6008. */
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) g_texture[0][0x400 + (y * 8 + x) / 4] |= (uint8_t)(((x + y) & 3u) << (2 * (x & 3u)));
    show_texture(TEXTURES, tex_param(2, 0, 0, 0x400), 0x6008 >> 3, SOLID);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) RULE("4 colours", at(16 + x, 16 + y), pixel_of(palette_color(4u + ((unsigned)(x + y) & 3u)), 31));

    /* Format 7, direct colour: "Each Texel occupies 16bit ... Bit15:
     * Alpha"; a texel without it is not drawn. In slot 3. */
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned i = 0; i < 64; i++) nds_put_le16(&g_texture[3][0x800 + 2 * i], (uint16_t)(palette_color(i + 40) | ((i & 1u) ? 0x8000u : 0u)));
    show_texture(TEXTURES, tex_param(7, 0, 0, 0x60800), 0, SOLID);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            unsigned i = (unsigned)(x + 8 * y);
            RULE("direct colour", at(16 + x, 16 + y), (i & 1u) ? pixel_of(palette_color(i + 40), 31) : PIXEL(0, 0, 0, 31));
        }
    }

    /* Format 1, A3I5: "Bit0-4: Color Index ... Bit5-7: Alpha ...
     * Alpha=(Alpha*4)+(Alpha/2)"; format 6, A5I3: "Bit0-2: Color Index ...
     * Bit3-7: Alpha". The texel's alpha times the polygon's (31) is the
     * pixel's: "A = ((At+1)*(Av+1)-1)/64" in 6 bits, which for a solid
     * polygon is the texel's own. Over the black rear plane with blending
     * on: "FrameBuf[R] = (Poly[R]*(Poly[A]+1) + FrameBuf[R]*(31-(Poly[A]))
     * / 32"; alpha 31 is solid, alpha 0 not drawn. */
    for (int format = 1; format <= 6; format += 5) {
        memset(g_texture, 0, sizeof g_texture);
        for (unsigned i = 0; i < 64; i++) {
            /* every alpha, and a few indexes */
            if (format == 1) g_texture[0][0x400 + i] = (uint8_t)(((i & 7u) << 5) | (i >> 1));
            else g_texture[0][0x400 + i] = (uint8_t)(((i >> 1) << 3) | (i & 7u));
        }
        show_texture(TEXTURES | BLENDING, tex_param((unsigned)format, 0, 0, 0x400), 0x6000 >> 4, SOLID);
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                unsigned i = (unsigned)(x + 8 * y);
                unsigned index = format == 1 ? i >> 1 : i & 7u;
                unsigned a = format == 1 ? (i & 7u) * 4 + (i & 7u) / 2 : i >> 1;
                uint32_t texel = pixel_of(palette_color(index), 0);
                uint32_t want;
                if (a == 0) want = PIXEL(0, 0, 0, 31);
                else if (a == 31) want = texel | (31u << 18);
                else want = PIXEL(red(texel) * (a + 1) / 32, green(texel) * (a + 1) / 32, blue(texel) * (a + 1) / 32, 31);
                RULE(format == 1 ? "A3I5" : "A5I3", at(16 + x, 16 + y), want);
            }
        }
    }

    /* Format 5, 4x4-texel compressed: "32bit per block, 2bit per Texel" in
     * slot 0 (or 2), rows from the top, texels from the low bits; and per
     * block a word in slot 1 at "slot0_addr / 2": "Bit0-13 Palette Offset
     * in 4-byte steps; Addr=(PLTT_BASE*10h)+(Offset*4)", "Bit14-15
     * Transparent/Interpolation Mode":
     *
     *   Texel  Mode 0       Mode 1             Mode 2    Mode 3
     *   0      Color 0      Color0             Color 0   Color 0
     *   1      Color 1      Color1             Color 1   Color 1
     *   2      Color 2      (Color0+Color1)/2  Color 2   (Color0*5+Color1*3)/8
     *   3      Transparent  Transparent        Color 3   (Color0*3+Color1*5)/8
     *
     * Four blocks, one of each mode; in each, column x has texel value x.
     * Block b uses palette offset 2 b. Slot 0 at 0x800, then slot 2. */
    for (int slot = 0; slot <= 2; slot += 2) {
        uint32_t address = (uint32_t)slot * 0x20000u + 0x800u;
        uint32_t info = (slot ? 0x10000u : 0u) + 0x800u / 2;
        memset(g_texture, 0, sizeof g_texture);
        for (unsigned b = 0; b < 4; b++) {
            for (unsigned row = 0; row < 4; row++) g_texture[slot][0x800 + 4 * b + row] = 0xE4;
            nds_put_le16(&g_texture[1][info + 2 * b], (uint16_t)((b << 14) | (2 * b)));
        }
        show_texture(TEXTURES, tex_param(5, 0, 0, address), 0x6000 >> 4, SOLID);
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                unsigned b = (unsigned)(x / 4 + 2 * (y / 4)), value = (unsigned)x & 3u;
                uint32_t c0 = palette_color(4 * b), c1 = palette_color(4 * b + 1);
                uint32_t want;
                if (value < 2 || b == 2 || (b == 0 && value == 2)) {
                    want = pixel_of(palette_color(4 * b + value), 31);
                } else if (value == 3 && b < 2) {
                    want = PIXEL(0, 0, 0, 31); /* transparent: the rear plane */
                } else {
                    unsigned f0 = b == 1 ? 4 : (value == 2 ? 5 : 3), f1 = 8 - f0;
                    uint32_t mixed = 0;
                    for (unsigned shift = 0; shift < 15; shift += 5)
                        mixed |= ((((c0 >> shift) & 31u) * f0 + ((c1 >> shift) & 31u) * f1) / 8) << shift;
                    want = pixel_of(mixed, 31);
                }
                RULE(slot ? "compressed, slot 2" : "compressed, slot 0", at(16 + x, 16 + y), want);
            }
        }
    }

    /* Sizes: "Size=(8 SHL N)" each way. A 32x16 texture, texel (x, y)
     * index x + 32 y (so the row length must be right), drawn whole. */
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned i = 0; i < 32 * 16; i++) g_texture[0][0x400 + i] = (uint8_t)(i & 255u);
    space(TEXTURES);
    command(0x4A8, tex_param(4, 2, 1, 0x400));
    command(0x4AC, 0x6000 >> 4);
    textured_rect(SOLID, 16, 16, 48, 32, 0, WHITE, 0, 0, 32, 16);
    show();
    RULE("32x16: first texel", at(16, 16), pixel_of(palette_color(0), 31));
    RULE("32x16: end of first row", at(47, 16), pixel_of(palette_color(31), 31));
    RULE("32x16: second row", at(16, 17), pixel_of(palette_color(32), 31));
    RULE("32x16: last texel", at(47, 31), pixel_of(palette_color((32 * 16 - 1) & 255), 31));

    /* Unmapped texture and palette memory reads as zero. */
    g_memory.texture[0] = NULL;
    render();
    RULE("unmapped texture slot: index 0", at(20, 20), pixel_of(palette_color(0), 31));
    g_memory.palette[1] = NULL;
    render();
    RULE("unmapped palette slot: black", at(20, 20), PIXEL(0, 0, 0, 31));
    g_memory.texture[0] = g_texture[0];
    g_memory.palette[1] = g_palette[1];
}

static void rules_texture_wrap(void)
{
    g_rule = "texture wrapping";
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned i = 0; i < 64; i++) g_texture[0][0x400 + i] = (uint8_t)(i + 1);
    fill_palette(0x6000, 256);

    /* TEXIMAGE_PARAM bits 16-19: repeat and flip per direction. "With
     * 'Clamp', the texture coordinates are clipped to MinMax(0,Size-1)";
     * flip is "Flip each 2nd Texture (requires Repeat)". A strip 40 texels
     * long from coordinate -16 to 24 over an 8-texel texture, each way. */
    static const struct {
        uint32_t flags;
        const char *name;
    } modes[4] = {{0, "clamp"},
                  {TEX_REPEAT_S | TEX_REPEAT_T, "repeat"},
                  {TEX_REPEAT_S | TEX_REPEAT_T | TEX_FLIP_S | TEX_FLIP_T, "repeat and flip"},
                  {TEX_FLIP_S | TEX_FLIP_T, "flip without repeat: clamp"}};
    for (int mode = 0; mode < 4; mode++) {
        for (int vertical = 0; vertical < 2; vertical++) {
            space(TEXTURES);
            command(0x4A8, tex_param(4, 0, 0, 0x400) | modes[mode].flags);
            command(0x4AC, 0x6000 >> 4);
            if (vertical) textured_rect(SOLID, 16, 16, 24, 56, 0, WHITE, 0, -16, 8, 24);
            else textured_rect(SOLID, 16, 16, 56, 24, 0, WHITE, -16, 0, 24, 8);
            show();
            for (int i = 0; i < 40; i++) {
                int c = i - 16, texel_at;
                if (mode == 0 || mode == 3) texel_at = c < 0 ? 0 : (c > 7 ? 7 : c);
                else if (mode == 1) texel_at = c & 7;
                else texel_at = (c & 8) ? 7 - (c & 7) : c & 7;
                /* the other coordinate is at texel 3 */
                unsigned index = vertical ? 1u + 3u + 8u * (unsigned)texel_at : 1u + (unsigned)texel_at + 8u * 3u;
                uint32_t got = vertical ? at(19, 16 + i) : at(16 + i, 19);
                RULE(modes[mode].name, got, pixel_of(palette_color(index), 31));
            }
        }
    }
}

static void rules_texture_blending(void)
{
    g_rule = "texture blending";
    memset(g_texture, 0, sizeof g_texture);
    memset(g_palette, 0, sizeof g_palette);
    /* A direct-colour texture of one colour, (10, 20, 31) solid, and an
     * A5I3 one of one palette colour at every alpha. */
    for (unsigned i = 0; i < 64; i++) nds_put_le16(&g_texture[0][0x400 + 2 * i], (uint16_t)(RGB15(10, 20, 31) | 0x8000u));
    for (unsigned i = 0; i < 64; i++) g_texture[0][0x800 + i] = (uint8_t)((i >> 1) << 3);
    nds_put_le16(&g_palette[0][0], (uint16_t)RGB15(10, 20, 31));
    const uint32_t vertex = RGB15(16, 8, 31); /* 33, 17, 63 in 6 bits */
    const unsigned tr = 21, tg = 41, tb = 63, vr = 33, vg = 17, vb = 63;

    /* Polygon mode 0, modulation: "R = ((Rt+1)*(Rv+1)-1)/64". */
    space(TEXTURES);
    command(0x4A8, tex_param(7, 0, 0, 0x400));
    textured_rect(SOLID | MODE(0), 16, 16, 24, 24, 0, vertex, 0, 0, 8, 8);
    show();
    RULE("modulation", at(20, 20), PIXEL(((tr + 1) * (vr + 1) - 1) / 64, ((tg + 1) * (vg + 1) - 1) / 64, ((tb + 1) * (vb + 1) - 1) / 64, 31));

    /* Polygon mode 1, decal: "R = (Rt*At + Rv*(63-At))/64 ;except, when
     * At=0: R=Rv, when At=31: R=Rt" and "A = Av". The two ends are checked
     * exactly; in between the mix is done with 5-bit weights,
     * (Rt*At + Rv*(31-At))/32 (melonDS), and must lie between the ends. */
    space(TEXTURES | BLENDING);
    command(0x4A8, tex_param(6, 0, 0, 0x800));
    command(0x4AC, 0);
    textured_rect(SOLID | MODE(1), 16, 16, 24, 24, 0, vertex, 0, 0, 8, 8);
    show();
    RULE("decal, texel alpha 0: vertex colour", at(16, 16), PIXEL(vr, vg, vb, 31));
    RULE("decal, texel alpha 31: texel colour", at(22, 23), PIXEL(tr, tg, tb, 31));
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            unsigned a = (unsigned)(x + 8 * y) >> 1;
            if (a == 0 || a == 31) continue;
            RULE("decal, in between", at(16 + x, 16 + y), PIXEL((tr * a + vr * (31 - a)) / 32, (tg * a + vg * (31 - a)) / 32, (tb * a + vb * (31 - a)) / 32, 31));
        }
    }
    /* Decal keeps the polygon's alpha: a translucent polygon stays so
     * whatever the texel's alpha. Alpha 15 over black: colour * 16 / 32. */
    space(TEXTURES | BLENDING);
    command(0x4A8, tex_param(7, 0, 0, 0x400));
    textured_rect(BOTH_SIDES | POLY_ALPHA(15) | MODE(1), 16, 16, 24, 24, 0, vertex, 0, 0, 8, 8);
    show();
    RULE("decal, translucent polygon", at(20, 20), PIXEL(tr * 16 / 32, tg * 16 / 32, tb * 16 / 32, 31));
    /* Modulation multiplies the alphas: "A = ((At+1)*(Av+1)-1)/64" in 6
     * bits; in the engine's 5 bits ((At+1)*(Av+1)-1)/32. Texel alpha 20,
     * polygon alpha 15: (21 * 16 - 1) / 32 = 10. */
    space(TEXTURES | BLENDING);
    command(0x4A8, tex_param(6, 0, 0, 0x800));
    command(0x4AC, 0);
    textured_rect(BOTH_SIDES | POLY_ALPHA(15) | MODE(0), 16, 16, 24, 24, 0, WHITE, 0, 0, 8, 8);
    show();
    RULE("modulation of alphas", at(16, 21), PIXEL(tr * 11 / 32, tg * 11 / 32, tb * 11 / 32, 31));

    /* Polygon mode 2 (GBATEK "Shading Table Colors"): "the red component of
     * the Vertex Color (Rv) is mis-used as an index in the Shading Table".
     * Toon: "R = ((Rt+1)*(Rs+1)-1)/64 ;Rs=ToonTableRed[Rv]"; without a
     * texture, the table's colour. */
    for (unsigned i = 0; i < 32; i++) nds_gx_write(&g_gx, 0x04000380 + 2 * i, RGB15(i, 31 - i, (i * 5) & 31), 2);
    space(TEXTURES);
    for (unsigned i = 0; i < 32; i++) nds_gx_write(&g_gx, 0x04000380 + 2 * i, RGB15(i, 31 - i, (i * 5) & 31), 2);
    rect(SOLID | MODE(2), 16, 16, 24, 24, 0, RGB15(20, 3, 7));
    command(0x4A8, tex_param(7, 0, 0, 0x400));
    textured_rect(SOLID | MODE(2), 32, 16, 40, 24, 0, RGB15(20, 3, 7), 0, 0, 8, 8);
    show();
    {
        const unsigned sr = 41, sg = 23, sb = 9; /* table entry 20: (20, 11, 4) */
        RULE("toon, no texture", at(20, 20), PIXEL(sr, sg, sb, 31));
        RULE("toon, textured", at(36, 20), PIXEL(((tr + 1) * (sr + 1) - 1) / 64, ((tg + 1) * (sg + 1) - 1) / 64, ((tb + 1) * (sb + 1) - 1) / 64, 31));
    }
    /* Highlight (DISP3DCNT bit 1): "R = ((Rt+1)*(Rs+1)-1)/64+Rs ;truncated
     * to MAX=63" says GBATEK; hardware modulates the texel with the vertex
     * red in all three channels and then adds the table's colour
     * (melonDS). Vertex red 20 is 41 in 6 bits. */
    space(TEXTURES | HIGHLIGHT);
    for (unsigned i = 0; i < 32; i++) nds_gx_write(&g_gx, 0x04000380 + 2 * i, RGB15(i, 31 - i, (i * 5) & 31), 2);
    rect(SOLID | MODE(2), 16, 16, 24, 24, 0, RGB15(20, 3, 7));
    command(0x4A8, tex_param(7, 0, 0, 0x400));
    textured_rect(SOLID | MODE(2), 32, 16, 40, 24, 0, RGB15(20, 3, 7), 0, 0, 8, 8);
    show();
    {
        const unsigned sr = 41, sg = 23, sb = 9, shade = 41;
        unsigned r = shade + sr, g = shade + sg, b = shade + sb;
        RULE("highlight, no texture", at(20, 20), PIXEL(r > 63 ? 63 : r, g > 63 ? 63 : g, b > 63 ? 63 : b, 31));
        r = ((tr + 1) * (shade + 1) - 1) / 64 + sr;
        g = ((tg + 1) * (shade + 1) - 1) / 64 + sg;
        b = ((tb + 1) * (shade + 1) - 1) / 64 + sb;
        RULE("highlight, textured", at(36, 20), PIXEL(r > 63 ? 63 : r, g > 63 ? 63 : g, b > 63 ? 63 : b, 31));
    }
}

/* "FrameBuf[R] = (Poly[R]*(Poly[A]+1) + FrameBuf[R]*(31-(Poly[A])) / 32" */
static unsigned mix(unsigned poly, unsigned frame, unsigned alpha)
{
    return (poly * (alpha + 1) + frame * (31 - alpha)) / 32;
}

static void rules_alpha(void)
{
    g_rule = "alpha";
    const uint32_t rear = RGB15(10, 20, 30); /* 21, 41, 61 */

    /* GBATEK "Alpha-Blending (Polygon vs FrameBuffer)". */
    space(BLENDING);
    nds_gx_write(&g_gx, 0x04000350, rear | (31u << 16), 4);
    rect(BOTH_SIDES | POLY_ALPHA(15), 16, 16, 24, 24, 0, WHITE);
    rect(BOTH_SIDES | POLY_ALPHA(1), 32, 16, 40, 24, 0, WHITE);
    rect(BOTH_SIDES | POLY_ALPHA(30), 48, 16, 56, 24, 0, WHITE);
    show();
    RULE("alpha 15", at(20, 20), PIXEL(mix(63, 21, 15), mix(63, 41, 15), mix(63, 61, 15), 31));
    RULE("alpha 1", at(36, 20), PIXEL(mix(63, 21, 1), mix(63, 41, 1), mix(63, 61, 1), 31));
    RULE("alpha 30", at(52, 20), PIXEL(mix(63, 21, 30), mix(63, 41, 30), mix(63, 61, 30), 31));
    /* "FrameBuf[A] = max(Poly[A],FrameBuf[A])" */
    nds_gx_write(&g_gx, 0x04000350, rear | (5u << 16), 4);
    rect(BOTH_SIDES | POLY_ALPHA(15), 16, 16, 24, 24, 0, WHITE);
    show();
    RULE("alpha is the larger", alpha_of(at(20, 20)), 15);
    RULE("and the colours still mix", red(at(20, 20)), mix(63, 21, 15));
    /* Bypassed when "The old framebuffer value is totally transparent
     * (FrameBuf[A]=0)": the polygon's colour and alpha as they are. */
    nds_gx_write(&g_gx, 0x04000350, rear, 4);
    rect(BOTH_SIDES | POLY_ALPHA(15), 16, 16, 24, 24, 0, WHITE);
    show();
    RULE("over a transparent rear plane", at(20, 20), PIXEL(63, 63, 63, 15));
    /* Bypassed when "Alpha-Blending is disabled (DISP3DCNT.Bit3=0)": the
     * polygon's colour. (GBATEK has the alpha overwritten too; melonDS
     * keeps the larger, and only the colour is checked.) */
    space(0);
    nds_gx_write(&g_gx, 0x04000350, rear | (31u << 16), 4);
    rect(BOTH_SIDES | POLY_ALPHA(15), 16, 16, 24, 24, 0, WHITE);
    show();
    RULE("blending off: red", red(at(20, 20)), 63);
    RULE("blending off: green", green(at(20, 20)), 63);

    /* A translucent pixel is not drawn over one of the same polygon ID
     * (POLYGON_ATTR bits 24-29: "used for translucent ..."): two
     * overlapping squares blend once where they share an ID, twice where
     * they do not. White at alpha 15 over black is 31; red at alpha 15
     * over that is (47, 15, 15). */
    for (unsigned second_id = 5; second_id <= 6; second_id++) {
        space(BLENDING);
        rect(BOTH_SIDES | POLY_ALPHA(15) | POLY_ID(5), 16, 16, 32, 32, 0, WHITE);
        rect(BOTH_SIDES | POLY_ALPHA(15) | POLY_ID(second_id), 24, 16, 40, 32, 0, RGB15(31, 0, 0));
        show();
        RULE("first square alone", at(20, 20), PIXEL(31, 31, 31, 31));
        RULE("second square alone", at(36, 20), PIXEL(mix(63, 0, 15), 0, 0, 31));
        if (second_id == 5) RULE("overlap, same ID: blended once", at(28, 20), PIXEL(31, 31, 31, 31));
        else RULE("overlap, other ID: blended twice", at(28, 20), PIXEL(mix(63, 31, 15), mix(0, 31, 15), mix(0, 31, 15), 31));
    }

    /* POLYGON_ATTR bit 11: "Depth-value for Translucent Pixels (0=Keep Old,
     * 1=Set New Depth)". A near translucent square, then a far one over the
     * same pixels: the far one is blended on top unless the near one set
     * the depth. */
    for (int set = 0; set < 2; set++) {
        space(BLENDING);
        rect(BOTH_SIDES | POLY_ALPHA(15) | POLY_ID(1) | (set ? NEW_DEPTH : 0), 16, 16, 32, 32, -0x300, WHITE);
        rect(BOTH_SIDES | POLY_ALPHA(15) | POLY_ID(2), 16, 16, 32, 32, 0x300, RGB15(31, 0, 0));
        show();
        if (set) RULE("depth set: the far one is hidden", at(20, 20), PIXEL(31, 31, 31, 31));
        else RULE("depth kept: the far one is drawn", at(20, 20), PIXEL(mix(63, 31, 15), mix(0, 31, 15), mix(0, 31, 15), 31));
    }

    /* Opaque polygons are drawn before translucent ones whatever the order
     * they were sent in. */
    space(BLENDING);
    rect(BOTH_SIDES | POLY_ALPHA(15) | POLY_ID(1), 16, 16, 32, 32, -0x300, WHITE);
    rect(SOLID, 16, 16, 32, 32, 0, RGB15(0, 0, 31));
    show();
    RULE("translucent over opaque sent later", at(20, 20), PIXEL(mix(63, 0, 15), mix(63, 0, 15), mix(63, 63, 15), 31));

    /* ALPHA_TEST_REF (GBATEK "DS 3D Display Control"): "pixels are rendered
     * only if their Alpha value is GREATER than ALPHA_TEST_REF", when
     * DISP3DCNT bit 2 is set; "Value 1Fh hides all polygons, including
     * opaque ones". */
    static const struct {
        uint32_t control;
        unsigned ref, alpha;
        int drawn;
    } tests[] = {
        {BLENDING | ALPHA_TEST, 15, 15, 0}, {BLENDING | ALPHA_TEST, 15, 16, 1}, {BLENDING | ALPHA_TEST, 15, 31, 1},
        {BLENDING | ALPHA_TEST, 31, 31, 0}, {BLENDING | ALPHA_TEST, 0, 1, 1},   {BLENDING, 31, 31, 1},
        {BLENDING, 31, 1, 1},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        space(tests[i].control);
        nds_gx_write(&g_gx, 0x04000340, tests[i].ref, 1);
        rect(BOTH_SIDES | POLY_ALPHA(tests[i].alpha), 16, 16, 32, 32, 0, WHITE);
        show();
        RULE("alpha test", at(20, 20) != PIXEL(0, 0, 0, 31), tests[i].drawn);
    }
    /* "Alpha Test is performed on the final polygon pixels (ie. after
     * texture blending)": an A5I3 texel of alpha 8 on a solid polygon. */
    memset(g_texture, 0, sizeof g_texture);
    memset(g_palette, 0, sizeof g_palette);
    for (unsigned i = 0; i < 64; i++) g_texture[0][0x800 + i] = (uint8_t)((i >> 1) << 3);
    nds_put_le16(&g_palette[0][0], (uint16_t)WHITE);
    show_texture(TEXTURES | BLENDING | ALPHA_TEST, tex_param(6, 0, 0, 0x800), 0, SOLID);
    nds_gx_write(&g_gx, 0x04000340, 8, 1);
    textured_rect(SOLID, 16, 16, 24, 24, 0, WHITE, 0, 0, 8, 8);
    show();
    RULE("alpha test on the texel's alpha: 8 is not above 8", at(16, 18), PIXEL(0, 0, 0, 31));
    RULE("alpha test on the texel's alpha: 9 is", at(18, 18) != PIXEL(0, 0, 0, 31), 1);
}

static void rules_depth(void)
{
    g_rule = "depth";
    const uint32_t red_c = RGB15(31, 0, 0), blue_c = RGB15(0, 0, 31);

    /* GBATEK "Depth Test": "The new pixels are drawn if the new depth is
     * Less (closer to the camera)". The nearer of two overlapping squares
     * shows, whichever is drawn first (the one that ends higher up is). */
    for (int near_first = 0; near_first < 2; near_first++) {
        space(0);
        rect(SOLID, 16, near_first ? 16 : 24, 48, near_first ? 40 : 48, 0, red_c);
        rect(SOLID, 32, near_first ? 24 : 16, 64, near_first ? 48 : 40, 0x300, blue_c);
        show();
        RULE("nearer square on top", at(40, 30), PIXEL(63, 0, 0, 31));
        RULE("farther square beside it", at(56, 30), PIXEL(0, 0, 63, 31));
    }
    /* At the same depth, the one drawn first stays. */
    space(0);
    rect(SOLID, 16, 16, 48, 40, 0, red_c);
    rect(SOLID, 32, 24, 64, 48, 0, blue_c);
    show();
    RULE("same depth: first stays", at(40, 30), PIXEL(63, 0, 0, 31));
    /* Unless the second faces the viewer and the first faces away: then
     * "less" is "less or equal" (melonDS). The first is sent clockwise. */
    space(0);
    command(0x4A4, SOLID);
    command(0x500, 1);
    corner(16, 16, 0, red_c, 0, 0);
    corner(48, 16, 0, red_c, 0, 0);
    corner(48, 40, 0, red_c, 0, 0);
    corner(16, 40, 0, red_c, 0, 0);
    rect(SOLID, 32, 24, 64, 48, 0, blue_c);
    show();
    RULE("same depth, front over back: drawn", at(40, 30), PIXEL(0, 0, 63, 31));

    /* POLYGON_ATTR bit 14: "Depth Test, Draw Pixels with Depth (0=Less,
     * 1=Equal)", and "Equal" has "a tolerance of +/-200h". One unit of z is
     * here 4/3 of 0x200 of depth, truncated: z = 1 is 0x200 away, z = 2 is
     * 0x400. */
    static const struct {
        int z, drawn;
    } equal[] = {{0, 1}, {1, 1}, {-1, 1}, {2, 0}, {-2, 0}, {0x300, 0}, {-0x300, 0}};
    for (size_t i = 0; i < sizeof equal / sizeof equal[0]; i++) {
        space(0);
        rect(SOLID, 16, 16, 48, 40, 0, red_c);
        rect(SOLID | DEPTH_EQUAL, 32, 24, 64, 48, equal[i].z, blue_c);
        show();
        RULE("equal: over the first square", at(40, 30), equal[i].drawn ? PIXEL(0, 0, 63, 31) : PIXEL(63, 0, 0, 31));
        RULE("equal: over the rear plane, never", at(56, 44), PIXEL(0, 0, 0, 31));
    }

    /* SWAP_BUFFERS bit 1: "Depth Buffering (0=With Z-value, 1=With
     * W-value)", for what is sent after the swap. A projection that puts
     * z into W and leaves clip z at 0: with Z-buffering every pixel has the
     * same depth and the first polygon drawn stays; with W-buffering the
     * one with the smaller W shows. The far square is smaller on screen and
     * ends higher up, so it is drawn first. */
    for (uint32_t w_buffer = 0; w_buffer < 2; w_buffer++) {
        static const int32_t projection[16] = {0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0x2000};
        reset_engine();
        nds_gx_write(&g_gx, 0x04000350, 31u << 16, 4);
        command(0x580, (255u << 16) | (191u << 24));
        swap_and_render(w_buffer << 1);
        command(0x440, 0);
        for (int i = 0; i < 16; i++) command(0x458, (uint32_t)projection[i]);
        command(0x4A4, SOLID);
        command(0x500, 1);
        for (int i = 0; i < 2; i++) {
            int z = i ? 0x1000 : 0; /* W = 2.0 for the first, 3.0 for the second */
            uint32_t color = i ? blue_c : red_c;
            corner(-0x1000, 0x1000, z, color, 0, 0);
            corner(-0x1000, -0x1000, z, color, 0, 0);
            corner(0x1000, -0x1000, z, color, 0, 0);
            corner(0x1000, 0x1000, z, color, 0, 0);
        }
        swap_and_render(w_buffer << 1);
        RULE("the near square is the larger", at(70, 96), PIXEL(63, 0, 0, 31));
        if (w_buffer) RULE("W-buffering: the smaller W shows", at(128, 96), PIXEL(63, 0, 0, 31));
        else RULE("Z-buffering: equal depths, the first drawn shows", at(128, 96), PIXEL(0, 0, 63, 31));
    }
}

/* "FrameBuffer[R] = (FogColor[R]*Density + FrameBuffer[R]*(128-Density)) /
 * 128" */
static unsigned fogged(unsigned fog, unsigned frame, unsigned density)
{
    return (fog * density + frame * (128 - density)) / 128;
}

static void fog_registers(uint32_t color, unsigned offset)
{
    nds_gx_write(&g_gx, 0x04000358, color, 4);
    nds_gx_write(&g_gx, 0x0400035C, offset, 2);
    /* FOG_TABLE: density n is 8 + 3 n, except the last, which is 127. */
    for (uint32_t i = 0; i < 32; i++) nds_gx_write(&g_gx, 0x04000360 + i, i == 31 ? 127 : 8 + 3 * i, 1);
}

static void rules_fog(void)
{
    g_rule = "fog";
    const uint32_t fog_color = RGB15(0, 31, 0) | (31u << 16);

    /* GBATEK FOG_OFFSET: "FogDepthBoundary[n] = FOG_OFFSET + FOG_STEP*(n+1)"
     * with "FOG_STEP=400h shr FOG_SHIFT", in 15-bit depth; and FOG_TABLE:
     * "FogDensity[0] is used for all pixels closer than FogDepthBoundary[0]
     * ... Density is linear interpolated for pixels that are between two
     * Density depth boundaries."
     *
     * In this frame a vertex at z has depth (0x3FFF + 4 z / 3) * 0x200, so
     * with FOG_OFFSET = 0x3FFF and no shift, z = 0x300 n is boundary n - 1:
     * density n - 1 exactly. A red square with POLYGON_ATTR bit 15. */
    static const struct {
        int z;
        unsigned density;
        const char *name;
    } at_depth[] = {
        {0x300 * 9, 8 + 3 * 8, "on a boundary"},
        {0x300 * 9 + 0x180, (8 + 3 * 8 + 8 + 3 * 9) / 2, "between two boundaries"},
        {0x300 * 1, 8, "on the first boundary"},
        {-0x600, 8, "closer than the first boundary"},
        {0, 8, "at the offset"},
    };
    for (size_t i = 0; i < sizeof at_depth / sizeof at_depth[0]; i++) {
        space(FOG);
        fog_registers(fog_color, 0x3FFF);
        rect(SOLID | FOGGED, 16, 16, 32, 32, at_depth[i].z, RGB15(31, 0, 0));
        rect(SOLID, 48, 16, 64, 32, at_depth[i].z, RGB15(31, 0, 0));
        show();
        unsigned d = at_depth[i].density;
        RULE(at_depth[i].name, at(20, 20), PIXEL(fogged(0, 63, d), fogged(63, 0, d), 0, 31));
        RULE("POLYGON_ATTR bit 15 clear: no fog", at(52, 20), PIXEL(63, 0, 0, 31));
        RULE("rear plane without its fog bit", at(100, 100), PIXEL(0, 0, 0, 31));
    }
    /* DISP3DCNT bit 7 off: no fog at all. */
    space(0);
    fog_registers(fog_color, 0x3FFF);
    rect(SOLID | FOGGED, 16, 16, 32, 32, 0x300 * 9, RGB15(31, 0, 0));
    show();
    RULE("fog off", at(20, 20), PIXEL(63, 0, 0, 31));

    /* The shift halves the step: with shift 1, z = 0x180 n is the
     * boundary; with shift 2, z = 0xC0 n. */
    for (unsigned shift = 1; shift <= 2; shift++) {
        space(FOG | (shift << 8));
        fog_registers(fog_color, 0x3FFF);
        rect(SOLID | FOGGED, 16, 16, 32, 32, (0x300 >> shift) * 9, RGB15(31, 0, 0));
        show();
        RULE("fog shift", at(20, 20), PIXEL(fogged(0, 63, 32), fogged(63, 0, 32), 0, 31));
    }
    /* The offset moves the boundaries: 0x100 further in 15-bit depth is
     * z = 0xC0 further. */
    space(FOG);
    fog_registers(fog_color, 0x3FFF + 0x100);
    rect(SOLID | FOGGED, 16, 16, 32, 32, 0x300 * 9 + 0xC0, RGB15(31, 0, 0));
    show();
    RULE("fog offset", at(20, 20), PIXEL(fogged(0, 63, 32), fogged(63, 0, 32), 0, 31));

    /* CLEAR_COLOR bit 15: "Fog (enables Fog to the rear-plane)". The rear
     * plane is at depth 0x7FFF, 0x4000 past the offset. Without a shift
     * that is boundary 15 (16 steps of 0x400): density 15. With shift 1 it
     * is past the last boundary: "FogDensity[31] is used for all pixels
     * more distant", and "Density=127 is handled as if Density=128": all
     * fog. */
    space(FOG);
    fog_registers(fog_color, 0x3FFF);
    nds_gx_write(&g_gx, 0x04000350, RGB15(31, 0, 31) | 0x8000u | (31u << 16), 4);
    show();
    RULE("rear plane fogged", at(100, 100), PIXEL(fogged(0, 63, 8 + 3 * 15), fogged(63, 0, 8 + 3 * 15), fogged(0, 63, 8 + 3 * 15), 31));
    space(FOG | (1u << 8));
    fog_registers(fog_color, 0x3FFF);
    nds_gx_write(&g_gx, 0x04000350, RGB15(31, 0, 31) | 0x8000u | (31u << 16), 4);
    show();
    RULE("rear plane past the last boundary: density 127 is all fog", at(100, 100), PIXEL(0, 63, 0, 31));

    /* DISP3DCNT bit 6: "Fog Color/Alpha Mode (0=Alpha and Color, 1=Only
     * Alpha)". Fog alpha 10, density 32, over a solid pixel. */
    for (int alpha_only = 0; alpha_only < 2; alpha_only++) {
        space(FOG | (alpha_only ? FOG_ALPHA_ONLY : 0));
        fog_registers(RGB15(0, 31, 0) | (10u << 16), 0x3FFF);
        rect(SOLID | FOGGED, 16, 16, 32, 32, 0x300 * 9, RGB15(31, 0, 0));
        show();
        if (alpha_only) RULE("alpha only", at(20, 20), PIXEL(63, 0, 0, fogged(10, 31, 32)));
        else RULE("colour and alpha", at(20, 20), PIXEL(fogged(0, 63, 32), fogged(63, 0, 32), 0, fogged(10, 31, 32)));
    }

    /* "When rendering translucent pixels, the old flag in the framebuffer
     * gets ANDed with PolygonAttr.Bit15": a translucent square without the
     * bit over a fogged opaque one takes the fog away; with it, the fog
     * stays (on the blended colour, at the opaque one's depth). */
    for (int keep = 0; keep < 2; keep++) {
        space(FOG | BLENDING);
        fog_registers(fog_color, 0x3FFF);
        rect(SOLID | FOGGED, 16, 16, 32, 32, 0x300 * 9, RGB15(31, 0, 0));
        rect(BOTH_SIDES | POLY_ALPHA(15) | POLY_ID(1) | (keep ? FOGGED : 0), 16, 16, 32, 32, 0, RGB15(0, 0, 31));
        show();
        unsigned r = mix(0, 63, 15), b = mix(63, 0, 15);
        if (keep) RULE("translucent with the bit: fog stays", at(20, 20), PIXEL(fogged(0, r, 32), fogged(63, 0, 32), fogged(0, b, 32), 31));
        else RULE("translucent without the bit: fog cleared", at(20, 20), PIXEL(r, 0, b, 31));
    }
}

static void rules_edge_marking(void)
{
    g_rule = "edge marking";
    const uint32_t edge1 = PIXEL(63, 0, 0, 31), edge2 = PIXEL(0, 0, 63, 31), white = PIXEL(63, 63, 63, 31);

    /* GBATEK EDGE_COLOR: "Edge Color 0 is used for Polygon ID 00h..07h,
     * Color 1 for ID 08h..0Fh, and so on"; an edge pixel is marked when "At
     * least one of the four surrounding pixels (up, down, left, right) must
     * have different polygon_id than the edge, and, the edge depth must be
     * LESS than the depth of that surrounding pixel". */
    for (unsigned id = 0; id < 64; id += 9) {
        space(EDGE_MARKING);
        for (uint32_t i = 0; i < 8; i++) nds_gx_write(&g_gx, 0x04000330 + 2 * i, RGB15(i * 4, 31 - i * 4, i), 2);
        nds_gx_write(&g_gx, 0x04000350, (31u << 16) | (63u << 24), 4); /* rear plane ID 63 */
        rect(SOLID | POLY_ID(id), 16, 16, 32, 32, 0, WHITE);
        show();
        uint32_t want = id == 63 ? white : pixel_of(RGB15((id >> 3) * 4, 31 - (id >> 3) * 4, id >> 3), 31);
        RULE("edge colour by polygon ID", at(16, 20), want);
        RULE("inside is not marked", at(20, 20), white);
    }

    space(EDGE_MARKING);
    nds_gx_write(&g_gx, 0x04000332, RGB15(31, 0, 0), 2);
    nds_gx_write(&g_gx, 0x04000334, RGB15(0, 0, 31), 2);
    rect(SOLID | POLY_ID(8), 16, 16, 32, 32, 0, WHITE);
    show();
    RULE("left edge", at(16, 24), edge1);
    RULE("right edge", at(31, 24), edge1);
    RULE("top edge", at(24, 16), edge1);
    RULE("bottom edge", at(24, 31), edge1);
    RULE("one pixel in", at(17, 17), white);
    RULE("outside", at(15, 24), PIXEL(0, 0, 0, 31));

    /* No edge between two polygons of one ID; one where the IDs differ, on
     * the nearer polygon only (the farther one's edge pixel is not "LESS"). */
    space(EDGE_MARKING);
    nds_gx_write(&g_gx, 0x04000332, RGB15(31, 0, 0), 2);
    nds_gx_write(&g_gx, 0x04000334, RGB15(0, 0, 31), 2);
    rect(SOLID | POLY_ID(8), 16, 16, 32, 32, 0, WHITE);
    rect(SOLID | POLY_ID(8), 32, 16, 48, 32, 0, WHITE);
    rect(SOLID | POLY_ID(8), 64, 16, 80, 32, 0, WHITE);
    rect(SOLID | POLY_ID(16), 80, 16, 96, 32, 0x300, WHITE);
    show();
    RULE("same ID: no edge on the left one", at(31, 24), white);
    RULE("same ID: no edge on the right one", at(32, 24), white);
    RULE("same ID: the outer edge is marked", at(47, 24), edge1);
    RULE("other ID: the nearer one's edge is marked", at(79, 24), edge1);
    RULE("other ID: the farther one's is not", at(80, 24), white);
    RULE("other ID: the farther one's outer edge is", at(95, 24), edge2);

    /* "Edge Marking is applied ONLY to opaque polygons". */
    space(EDGE_MARKING | BLENDING);
    nds_gx_write(&g_gx, 0x04000332, RGB15(31, 0, 0), 2);
    rect(BOTH_SIDES | POLY_ALPHA(15) | POLY_ID(8), 16, 16, 32, 32, 0, WHITE);
    show();
    RULE("translucent: no edge", at(16, 24), PIXEL(31, 31, 31, 31));

    /* At the screen's border the neighbour is the rear plane. */
    space(EDGE_MARKING);
    nds_gx_write(&g_gx, 0x04000332, RGB15(31, 0, 0), 2);
    rect(SOLID | POLY_ID(8), 0, 0, 256, 192, 0, WHITE);
    show();
    RULE("screen corner", at(0, 0), edge1);
    RULE("screen's last pixel", at(255, 191), edge1);
    RULE("screen's inside", at(128, 96), white);
}

static void rules_shadow(void)
{
    g_rule = "shadow";
    const uint32_t grey = RGB15(20, 20, 20); /* 41 */
    const unsigned shade = mix(0, 41, 16);

    /* GBATEK "DS 3D Shadow Polygons". Step 1, the mask: "Mode=Shadow,
     * PolygonID=00h ... the polygon [is] NOT ... drawn to the Color Buffer -
     * instead, flags are set in the Stencil Buffer". Step 2: "Mode=Shadow,
     * PolygonID=01h..3Fh ... drawn to the Color Buffer as usually, but only"
     * where the stencil says so, and "only if its Polygon ID differs from
     * the ID in the Attribute Buffer".
     *
     * A floor, the back of a shadow volume behind it (the mask) and its
     * front before it: where both cover the floor, the floor is shaded.
     * (Which pixels the mask flags is as melonDS has it: those where the
     * mask is hidden by what is there.) */
    static const struct {
        int mask_z;
        unsigned shadow_id;
        int shaded;
        const char *name;
    } cases[] = {
        {0x300, 2, 1, "floor between the mask and the front"},
        {-0x600, 2, 0, "mask in front of the floor: no shadow"},
        {0x300, 1, 0, "shadow with the floor's own ID: no shadow"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        space(BLENDING);
        swap_and_render(1); /* "Auto-sorting must have been disabled in the previous Swap_Buffers command" */
        rect(SOLID | POLY_ID(1), 40, 40, 120, 120, 0, grey);
        rect(BOTH_SIDES | MODE(3) | POLY_ALPHA(16) | POLY_ID(0), 60, 60, 100, 100, cases[i].mask_z, 0);
        rect(BOTH_SIDES | MODE(3) | POLY_ALPHA(16) | POLY_ID(cases[i].shadow_id), 50, 50, 110, 110, -0x300, 0);
        show();
        RULE(cases[i].name, at(80, 80), cases[i].shaded ? PIXEL(shade, shade, shade, 31) : PIXEL(41, 41, 41, 31));
        RULE("inside the front but not the mask", at(55, 55), PIXEL(41, 41, 41, 31));
        RULE("the mask's edge", at(60, 60), cases[i].shaded ? PIXEL(shade, shade, shade, 31) : PIXEL(41, 41, 41, 31));
        RULE("just outside the mask", at(100, 80), PIXEL(41, 41, 41, 31));
        RULE("floor outside both", at(45, 45), PIXEL(41, 41, 41, 31));
        RULE("the mask draws nothing off the floor", at(30, 30), PIXEL(0, 0, 0, 31));
    }
    /* A second volume starts from a clear stencil. */
    space(BLENDING);
    swap_and_render(1);
    rect(SOLID | POLY_ID(1), 40, 40, 200, 120, 0, grey);
    rect(BOTH_SIDES | MODE(3) | POLY_ALPHA(16) | POLY_ID(0), 60, 60, 100, 100, 0x300, 0);
    rect(BOTH_SIDES | MODE(3) | POLY_ALPHA(16) | POLY_ID(2), 50, 50, 110, 110, -0x300, 0);
    rect(BOTH_SIDES | MODE(3) | POLY_ALPHA(16) | POLY_ID(0), 140, 60, 160, 100, 0x300, 0);
    rect(BOTH_SIDES | MODE(3) | POLY_ALPHA(16) | POLY_ID(3), 50, 50, 190, 110, -0x300, 0);
    /* The second front covers the first mask's area too, but that mask is
     * gone, and its ID differs from the first shadow's (so it could draw). */
    show();
    RULE("first volume", at(80, 80), PIXEL(shade, shade, shade, 31));
    RULE("second volume", at(150, 80), PIXEL(shade, shade, shade, 31));
    RULE("between them", at(120, 80), PIXEL(41, 41, 41, 31));
}

static void rules_antialiasing(void)
{
    g_rule = "anti-aliasing";

    /* GBATEK "Anti-Aliasing": "the edges of opaque polygons will be
     * anti-aliased (ie. the pixels at the edges may become translucent)";
     * not the inside, and not translucent polygons. How much each edge
     * pixel is covered is melonDS's model, with only the swapped-edge
     * scenes of polyrastertest behind it; what is checked here is what
     * GBATEK promises: a slanted edge of white over black has pixels in
     * between, getting lighter towards the inside, and the inside is
     * untouched. */
    space(ANTIALIAS);
    command(0x4A4, SOLID);
    command(0x500, 0);
    corner(20, 20, 0, WHITE, 0, 0);
    corner(40, 120, 0, WHITE, 0, 0);
    corner(120, 120, 0, WHITE, 0, 0);
    show();
    {
        int between = 0, inside_ok = 1;
        for (int y = 30; y < 110; y++) {
            int x = 0;
            while (x < 255 && red(at(x, y)) == 0) x++;
            if (red(at(x, y)) < 63) between++;
            if (red(at(x + 2, y)) != 63) inside_ok = 0;
        }
        RULE("the left edge has pixels in between", between > 40, 1);
        RULE("the inside is solid", inside_ok, 1);
    }
    RULE("far from the polygon", at(200, 60), PIXEL(0, 0, 0, 31));
    /* An edge over a transparent rear plane: "the pixels at the edges may
     * become translucent". */
    space(ANTIALIAS);
    nds_gx_write(&g_gx, 0x04000350, 0, 4);
    command(0x4A4, SOLID);
    command(0x500, 0);
    corner(20, 20, 0, WHITE, 0, 0);
    corner(40, 120, 0, WHITE, 0, 0);
    corner(120, 120, 0, WHITE, 0, 0);
    show();
    {
        int translucent = 0;
        for (int y = 30; y < 110; y++) {
            int x = 0;
            while (x < 255 && alpha_of(at(x, y)) == 0) x++;
            if (alpha_of(at(x, y)) < 31) translucent++;
        }
        RULE("edge pixels are translucent", translucent > 40, 1);
        RULE("the inside is opaque", alpha_of(at(60, 100)), 31);
    }
}

static void rules_perspective(void)
{
    g_rule = "perspective";
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 64; x++) g_texture[0][0x400 + y * 64 + x] = (uint8_t)x;
    for (unsigned i = 0; i < 64; i++) nds_put_le16(&g_palette[0][2 * i], (uint16_t)RGB15(i & 31u, i >> 5, 31));

    /* A quad seen at an angle: W is 1.5 at its left edge and 2.5 at its
     * right. Texture coordinates across it must follow perspective: halfway
     * across on screen is not halfway across the texture, but
     *
     *   s = (s0 / w0 * (1 - t) + s1 / w1 * t) / ((1 - t) / w0 + t / w1)
     *
     * which for s 0..64 and t = 1/2 is 64 * (1/2.5) / (1/1.5 + 1/2.5) = 24.
     * The hardware gets there with an 8-bit weight (melonDS), so a texel
     * either side is allowed. */
    static const int32_t projection[16] = {0x1000, 0, 0, 0x800, 0, 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x2000};
    reset_engine();
    g_vram = &g_memory;
    nds_gx_write(&g_gx, 0x04000350, 31u << 16, 4);
    nds_gx_write(&g_gx, 0x04000060, TEXTURES, 2);
    command(0x580, (255u << 16) | (191u << 24));
    command(0x440, 0);
    for (int i = 0; i < 16; i++) command(0x458, (uint32_t)projection[i]);
    command(0x4A8, tex_param(4, 3, 0, 0x400) | TEX_REPEAT_T);
    command(0x4AC, 0);
    command(0x4A4, SOLID);
    command(0x500, 1);
    corner(-0x1000, 0x800, 0, WHITE, 0, 0);
    corner(-0x1000, -0x800, 0, WHITE, 0, 8);
    corner(0x1000, -0x800, 0, WHITE, 64, 8);
    corner(0x1000, 0x800, 0, WHITE, 64, 0);
    show();
    {
        /* On line 96 the quad runs from x = 42 to 178 (W 1.5: x = -1/1.5;
         * W 2.5: x = 1/2.5, each mapped to 0..256). */
        int left = 0, right = 255;
        while (left < 255 && at(left, 96) == PIXEL(0, 0, 0, 31)) left++;
        while (right > 0 && at(right, 96) == PIXEL(0, 0, 0, 31)) right--;
        RULE("left end", left, 42);
        RULE("right end", right, 178);
        int middle = (left + right + 1) / 2;
        /* palette entry i is (i & 31, i >> 5, 31): 6-bit red is 2 r + 1, or 0 */
        unsigned texel = red(at(middle, 96)) / 2 + 32 * (green(at(middle, 96)) / 2);
        RULE("the middle of the screen span is texel 24, not 32", texel >= 23 && texel <= 25, 1);
        RULE("the left end is texel 0", red(at(left, 96)) / 2 + 32 * (green(at(left, 96)) / 2), 0);
        RULE("the right end is texel 63", red(at(right, 96)) / 2 + 32 * (green(at(right, 96)) / 2), 63);
    }
}

/* ------------------------------------------------------------------ */
/* The larger picture                                                  */
/* ------------------------------------------------------------------ */

/*
 * nds_gx_render_scaled(): the same frame drawn two to four times as large.
 * No hardware drew these, so there is no recording to hold them to. What
 * can be held:
 *
 *  - the screen's picture and the engine's state are not touched by it;
 *  - a picture made of whole pixels (rectangles on pixel boundaries, a texel
 *    a pixel, which is how 2D is drawn with the 3D engine) comes out as that
 *    picture with every pixel repeated;
 *  - a vertex is put where the viewport division put it, not at the corner
 *    of its pixel, which is the point of drawing larger;
 *  - a slanted edge covers the area it should;
 *  - the picture does not depend on how many threads drew it.
 */

/* nds_gx_workers.h, which is the runtime's own and not on a test's path. */
void nds_gx_workers_set_threads(int threads);

static void bench_scene(void);

static uint32_t *g_large;

static const uint32_t *larger(int scale)
{
    if (!g_large) g_large = malloc((size_t)NDS_GX_WIDTH * NDS_GX_HEIGHT * NDS_GX_SCALE_MAX * NDS_GX_SCALE_MAX * 4u);
    if (!g_large) {
        printf("no memory for the larger picture\n");
        exit(1);
    }
    RULE("the larger picture is drawn", nds_gx_render_scaled(&g_gx, g_vram, scale, g_large), 1);
    return g_large;
}

static uint32_t large_at(const uint32_t *large, int scale, int x, int y)
{
    return large[(size_t)y * NDS_GX_WIDTH * (size_t)scale + (size_t)x];
}

/* How many pixels of the larger picture are not the pixel of the screen
 * they lie in. */
static long not_repeated(const uint32_t *large, int scale)
{
    long wrong = 0;
    for (int y = 0; y < NDS_GX_HEIGHT * scale; y++)
        for (int x = 0; x < NDS_GX_WIDTH * scale; x++)
            if (large_at(large, scale, x, y) != at(x / scale, y / scale)) wrong++;
    return wrong;
}

static void rules_larger(void)
{
    g_rule = "larger picture";
    static NdsGx before;

    /* A texture with a different colour in each of its 256 texels. */
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned i = 0; i < 256; i++) g_texture[0][0x400 + i] = (uint8_t)i;
    fill_palette(0x6000, 256);

    for (int scale = 1; scale <= NDS_GX_SCALE_MAX; scale++) {
        /* Whole pixels: plain rectangles over one another, a translucent
         * one, and a texture drawn a texel a pixel, on a rear plane of its
         * own colour. */
        space(TEXTURES | BLENDING);
        nds_gx_write(&g_gx, 0x04000350, RGB15(3, 9, 20) | (31u << 16), 4);
        rect(SOLID, 10, 20, 200, 150, 0x1000, RGB15(31, 0, 0));
        rect(SOLID, 100, 5, 250, 60, 0, RGB15(0, 31, 0));
        rect(SOLID, 0, 100, 256, 192, -0x1000, RGB15(0, 0, 31));
        command(0x4A8, tex_param(4, 1, 1, 0x400));
        command(0x4AC, 0x6000 >> 4);
        textured_rect(SOLID, 120, 80, 136, 96, -0x2000, WHITE, 0, 0, 16, 16);
        command(0x4A8, 0);
        rect(BOTH_SIDES | POLY_ALPHA(12), 60, 40, 112, 130, -0x2800, RGB15(31, 31, 0));
        show();
        before = g_gx;
        const uint32_t *large = larger(scale);
        RULE("drawing it leaves the engine and the screen's picture alone", memcmp(&before, &g_gx, sizeof g_gx), 0);
        RULE("a picture of whole pixels is that picture, each pixel repeated", not_repeated(large, scale), 0);
        RULE("the texture's first texel is where it was", large_at(large, scale, 120 * scale, 80 * scale),
             pixel_of(palette_color(0), 31));
        RULE("... and its last", large_at(large, scale, 136 * scale - 1, 96 * scale - 1),
             pixel_of(palette_color(255), 31));
    }

    /* The rear plane's bitmap has a pixel for each of the screen's. */
    for (unsigned y = 0; y < 256; y++) {
        for (unsigned x = 0; x < 256; x++) {
            nds_put_le16(&g_texture[2][(y * 256 + x) * 2], (uint16_t)(RGB15(x & 31u, y & 31u, (x >> 5) + (y >> 5)) | 0x8000u));
            nds_put_le16(&g_texture[3][(y * 256 + x) * 2], 0x7FFF);
        }
    }
    space(REAR_BITMAP);
    nds_gx_write(&g_gx, 0x04000356, 0x0503, 2); /* CLRIMAGE_OFFSET: 3 across, 5 down */
    show();
    for (int scale = 2; scale <= NDS_GX_SCALE_MAX; scale++) {
        RULE("the rear plane's bitmap is repeated", not_repeated(larger(scale), scale), 0);
    }
    memset(g_texture, 0, sizeof g_texture);

    /* Sub-pixel positions. With the vertices halved by the position matrix,
     * an odd coordinate is half a pixel: the screen's picture puts it at the
     * pixel's corner, and the larger one where it is. */
    for (int scale = 2; scale <= NDS_GX_SCALE_MAX; scale++) {
        space(0);
        command(0x46C, 0x800); /* MTX_SCALE: x, y, z */
        command(0x46C, 0x800);
        command(0x46C, 0x1000);
        rect(SOLID, 2 * 40 + 1, 2 * 30 + 1, 2 * 90 + 1, 2 * 70 + 1, 0, WHITE);
        show();
        RULE("on the screen the rectangle starts at pixel 40", alpha_of(at(39, 50)) != 0 && red(at(39, 50)) == 63, 0);
        RULE("... and covers it", red(at(40, 50)), 63);
        RULE("... and stops before pixel 90", red(at(90, 50)), 0);
        const uint32_t *large = larger(scale);
        int left = -1, right = -1, top = -1, bottom = -1;
        for (int x = 0; x < NDS_GX_WIDTH * scale; x++) {
            if (red(large_at(large, scale, x, 50 * scale)) != 63) continue;
            if (left < 0) left = x;
            right = x;
        }
        for (int y = 0; y < NDS_GX_HEIGHT * scale; y++) {
            if (red(large_at(large, scale, 60 * scale, y)) != 63) continue;
            if (top < 0) top = y;
            bottom = y;
        }
        /* 40.5 and 90.5 pixels, 30.5 and 70.5 lines, to whole larger pixels. */
        RULE("larger, its left edge is half a pixel further on", left, (40 * 2 + 1) * scale / 2);
        RULE("... and its right edge", right, (90 * 2 + 1) * scale / 2 - 1);
        RULE("... and its top", top, (30 * 2 + 1) * scale / 2);
        RULE("... and its bottom", bottom, (70 * 2 + 1) * scale / 2 - 1);
    }

    /* A triangle with no edge along the grid: (20, 10), (230, 60), (90,
     * 180). Its area is 16,100 pixels; drawn larger it covers that many
     * times the square of the scale, to within the pixels along its edges. */
    for (int scale = 1; scale <= NDS_GX_SCALE_MAX; scale++) {
        space(0);
        command(0x4A4, SOLID);
        command(0x500, 0);
        corner(20, 10, 0, WHITE, 0, 0);
        corner(90, 180, 0, WHITE, 0, 0);
        corner(230, 60, 0, WHITE, 0, 0);
        show();
        const uint32_t *large = larger(scale);
        long covered = 0, outside = 0;
        for (int y = 0; y < NDS_GX_HEIGHT * scale; y++) {
            for (int x = 0; x < NDS_GX_WIDTH * scale; x++) {
                if (red(large_at(large, scale, x, y)) != 63) continue;
                covered++;
                if (x < 20 * scale || x > 230 * scale || y < 10 * scale || y > 180 * scale) outside++;
            }
        }
        const long area = 16100L * scale * scale, edge = 700L * scale;
        RULE("a triangle covers its area", covered > area - edge && covered < area + edge, 1);
        RULE("... and nothing outside its corners", outside, 0);
    }

    /* A busy frame - perspective, textures, fog, edge marking, 500 polygons -
     * drawn by one thread and by several. */
    bench_scene();
    for (int scale = 2; scale <= NDS_GX_SCALE_MAX; scale++) {
        const size_t bytes = (size_t)NDS_GX_WIDTH * NDS_GX_HEIGHT * (size_t)scale * (size_t)scale * 4u;
        uint32_t *alone = malloc(bytes);
        if (!alone) {
            printf("no memory for the larger picture\n");
            exit(1);
        }
        nds_gx_workers_set_threads(1);
        memcpy(alone, larger(scale), bytes);
        long drawn = 0;
        for (size_t i = 0; i < bytes / 4u; i++) drawn += alone[i] != alone[0];
        RULE("the busy frame has a picture", drawn > 1000, 1);
        static const int threads[3] = {2, 5, 16};
        for (int t = 0; t < 3; t++) {
            nds_gx_workers_set_threads(threads[t]);
            RULE("several threads draw what one does", memcmp(alone, larger(scale), bytes), 0);
        }
        free(alone);
    }
    nds_gx_workers_set_threads(0);
}

static void run_rules(void)
{
    printf("rendering rules from GBATEK\n");
    rules_rear_plane();
    rules_polygons();
    rules_texture_formats();
    rules_texture_wrap();
    rules_texture_blending();
    rules_alpha();
    rules_depth();
    rules_fog();
    rules_edge_marking();
    rules_shadow();
    rules_antialiasing();
    rules_perspective();
    rules_larger();
    g_vram = NULL;
    printf("  rules: %ld of %ld checks pass\n", g_rule_checks - g_rule_failures, g_rule_checks);
}

/* ------------------------------------------------------------------ */
/* Speed                                                               */
/* ------------------------------------------------------------------ */

/* A frame of the kind a game draws: a floor running to the horizon, a wall
 * across the back and a nearer wall in front of both, 1,024 triangles in
 * all, in perspective, each square with a 64x64 256-colour texture, with
 * fog and edge marking on. The whole screen is covered, the middle of it
 * two or three times; the floor is cut by the near plane and the screen's
 * edges. */
static void bench_scene(void)
{
    /* 60 degrees high, 4:3, near 0.5, far 16. */
    static const int32_t projection[16] = {
        0x14C9, 0, 0, 0, 0, 0x1BB6, 0, 0, 0, 0, -0x1108, -0x1000, 0, 0, -0x1084, 0,
    };
    reset_engine();
    memset(g_texture, 0, sizeof g_texture);
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 64; x++)
            g_texture[0][y * 64 + x] = (uint8_t)(1u + ((x ^ y) & 63u) + ((x >> 3) + (y >> 3)) % 3u * 64u);
    fill_palette(0, 256);
    g_memory.texture[0] = g_texture[0];
    g_memory.texture[1] = g_texture[1];
    g_memory.texture[2] = g_texture[2];
    g_memory.texture[3] = g_texture[3];
    for (int i = 0; i < 6; i++) g_memory.palette[i] = g_palette[i];
    g_vram = &g_memory;

    nds_gx_write(&g_gx, 0x04000350, RGB15(12, 18, 31) | 0x8000u | (31u << 16) | (63u << 24), 4);
    nds_gx_write(&g_gx, 0x04000354, 0x7FFF, 2);
    nds_gx_write(&g_gx, 0x04000060, TEXTURES | BLENDING | EDGE_MARKING | FOG | (2u << 8), 2);
    fog_registers(RGB15(20, 24, 31) | (31u << 16), 0x7800);
    for (uint32_t i = 0; i < 8; i++) nds_gx_write(&g_gx, 0x04000330 + 2 * i, RGB15(2, 2, 4), 2);
    command(0x580, (255u << 16) | (191u << 24));
    command(0x440, 0);
    for (int i = 0; i < 16; i++) command(0x458, (uint32_t)projection[i]);
    command(0x440, 2);
    command(0x454, 0);
    command(0x4A8, tex_param(4, 3, 3, 0) | TEX_REPEAT_S | TEX_REPEAT_T);
    command(0x4AC, 0);

    /* Each part is a grid of squares: where its first corner is, and the
     * step to the next square across (u) and along or up (v). */
    static const struct {
        int columns, rows;
        int origin[3], across[3], along[3];
    } parts[3] = {
        {16, 16, {-0x4000, -0x0C00, -0x0400}, {0x800, 0, 0}, {0, 0, -0x800}},  /* the floor */
        {16, 8, {-0x6000, -0x0C00, -0x7000}, {0xC00, 0, 0}, {0, 0x980, 0}},     /* a wall across the back */
        {16, 8, {-0x2000, -0x0C00, -0x4000}, {0x400, 0, 0x40}, {0, 0x580, 0}}, /* a nearer one, at a slant */
    };
    for (int part = 0; part < 3; part++) {
        command(0x4A4, BOTH_SIDES | POLY_ALPHA(31) | FOGGED | POLY_ID(1 + 8 * part));
        command(0x500, 0);
        for (int j = 0; j < parts[part].rows; j++) {
            for (int i = 0; i < parts[part].columns; i++) {
                static const int order[6] = {0, 1, 2, 0, 2, 3};
                for (int k = 0; k < 6; k++) {
                    int c = order[k];
                    int u = i + (c == 1 || c == 2), v = j + (c >= 2);
                    corner(parts[part].origin[0] + u * parts[part].across[0] + v * parts[part].along[0],
                           parts[part].origin[1] + u * parts[part].across[1] + v * parts[part].along[1],
                           parts[part].origin[2] + u * parts[part].across[2] + v * parts[part].along[2],
                           RGB15(31 - (i & 7), 31, 24 + (j & 7)), u * 32, v * 32);
                }
            }
        }
    }
    command(0x540, 0);
    drain();
    nds_gx_vblank(&g_gx);
    drain();
}

static void run_bench(int frames)
{
    bench_scene();
    render();
    long covered = 0;
    for (int y = 0; y < NDS_GX_HEIGHT; y++)
        for (int x = 0; x < NDS_GX_WIDTH; x++)
            if (at(x, y) != at(255, 0)) covered++;
    clock_t start = clock();
    for (int i = 0; i < frames; i++) nds_gx_render(&g_gx, g_vram);
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("%d polygons from 1024 triangles sent, %ld of %d pixels covered\n", g_gx.render_count, covered,
           NDS_GX_WIDTH * NDS_GX_HEIGHT);
    printf("%d frames in %.3f s: %.3f ms a frame, %.0f frames a second\n", frames, seconds,
           seconds * 1000.0 / frames, seconds > 0 ? frames / seconds : 0.0);

    /* The geometry engine's share: the same frame's commands, sent and run. */
    start = clock();
    for (int i = 0; i < 50; i++) bench_scene();
    seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("geometry: %.3f ms a frame\n", seconds * 1000.0 / 50);
}

int main(int argc, char **argv)
{
    const char *interp_dir = NULL;
    int skip_interp = 0, full = 0, scene = 0, bench = 0;
    int line_corner = -1, line_x = 0, line_y = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interp") && i + 1 < argc) interp_dir = argv[++i];
        else if (!strcmp(argv[i], "--skip-interp")) skip_interp = 1;
        else if (!strcmp(argv[i], "--full")) full = 1;
        else if (!strcmp(argv[i], "--hash")) g_hashing = 1;
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bench")) {
            bench = 200;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0) bench = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--line") && i + 3 < argc) {
            line_corner = atoi(argv[++i]);
            line_x = atoi(argv[++i]);
            line_y = atoi(argv[++i]);
        } else {
            printf("usage: test_nds_gx_raster [--interp DIR | --skip-interp] [--full] [--hash] [--scene N] "
                   "[--line CORNER X Y] [--bench [FRAMES]]\n");
            return 2;
        }
    }

    if (bench) {
        run_bench(bench);
        if (g_hashing) printf("frame hash %016llX over %ld frames\n", (unsigned long long)g_hash, g_hashed_frames);
        return 0;
    }

    if (line_corner >= 0) {
        if (!interp_dir) {
            printf("--line needs --interp DIR\n");
            return 2;
        }
        show_interp_line(interp_dir, line_corner, line_x, line_y);
        return 0;
    }

    run_rules();
    run_polyrastertest(scene);
    if (interp_dir) {
        run_interp(interp_dir, full);
    } else if (!skip_interp) {
        printf("nds-interp: no --interp DIR (run tools/nds/gx_unpack_interp.py; --skip-interp to leave it out)\n");
        g_checks++;
        g_failures++;
    }

    if (g_hashing) printf("frame hash %016llX over %ld frames\n", (unsigned long long)g_hash, g_hashed_frames);
    printf("%ld checks, %ld failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
