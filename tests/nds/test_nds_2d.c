/*
 * Tests for the DS's two 2D graphics engines (runtime-nds/src/nds_2d.c).
 *
 * Each test builds a small scene in VRAM, palette RAM and OAM, renders one
 * or a few lines, and checks pixels against values worked out by hand from
 * the documentation:
 *
 *   GBATEK    problemkaputt.de/gbatek.htm, "DS Video" and the GBA chapters
 *             it builds on ("LCD I/O ...", "LCD OBJ ...", "LCD Color Special
 *             Effects"), "DS 3D Final 2D Output"
 *   melonDS   src/GPU2D*.cpp, read as a behaviour reference where GBATEK
 *             does not say what the hardware does: how each effect rounds,
 *             the window flip-flops, the bitmap sprite's alpha, sprite mosaic
 *
 * An output pixel is 18 bits: red in bits 0-5, green in 6-11, blue in
 * 12-17. A 15-bit colour comes out with each channel doubled.
 *
 * Every line any test renders with the 3D layer showing is also stacked a
 * second time around a finer 3D picture (nds_2d_stack_scaled), and held to
 * the engine itself: see check_stack_scaled(). So is every line with a
 * direct-colour bitmap shown pixel for pixel, as a background or as sprites,
 * around finer pixels for the bitmap: see check_stack_bitmap().
 *
 * Usage: test_nds_2d            run the checks
 *        test_nds_2d --bench    also time a busy line
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nds_2d.h"

/* ------------------------------------------------------------------ */
/* The machine around the engine                                       */
/* ------------------------------------------------------------------ */

static uint8_t g_bg[0x80000];       /* BG VRAM: 512 KiB (engine B is given all of it and must use 128 KiB) */
static uint8_t g_obj[0x40000];      /* OBJ VRAM: 256 KiB */
static uint8_t g_pal[0x400];        /* standard palettes: BG at 0, OBJ at 0x200 */
static uint8_t g_oam[0x400];
static uint8_t g_bg_ext[4][0x2000]; /* BG extended palette slots */
static uint8_t g_obj_ext[0x2000];   /* OBJ extended palette */
static uint8_t g_lcdc[4][0x20000];  /* VRAM blocks A-D as LCDC memory */
static uint32_t g_3d_line[256];
static const uint32_t *g_3d;        /* the 3D line handed to the engine, or NULL */
static Nds2dVram g_vram;
static Nds2d g_e;
static uint32_t g_out[256];

static int g_checks;
static int g_failures;
static const char *g_section = "";

static void check_eq(const char *what, uint32_t got, uint32_t want, int line)
{
    g_checks++;
    if (got == want) return;
    g_failures++;
    if (g_failures <= 100) {
        printf("FAIL %s: %s = %05" PRIX32 ", expected %05" PRIX32 " (line %d)\n", g_section, what, got, want, line);
    }
}

#define EQ(what, got, want) check_eq(what, (uint32_t)(got), (uint32_t)(want), __LINE__)

static void check_px(int x, uint32_t want, int line)
{
    char what[32];
    snprintf(what, sizeof what, "pixel %d", x);
    check_eq(what, g_out[x], want, line);
}

#define PX(x, want) check_px((x), (uint32_t)(want), __LINE__)

/* Colours. */
#define RGB15(r, g, b) ((uint32_t)((r) | ((g) << 5) | ((b) << 10)))
#define RGB18(r, g, b) ((uint32_t)((r) | ((g) << 6) | ((b) << 12)))
#define WHITE18 RGB18(63, 63, 63)

/* How a 15-bit colour comes out when nothing is done to it. */
static uint32_t out15(uint32_t c)
{
    return RGB18((c & 31u) * 2u, ((c >> 5) & 31u) * 2u, ((c >> 10) & 31u) * 2u);
}

/* A different 15-bit colour for every i in 1..255, none of them black. */
static uint32_t shade(uint32_t i)
{
    return (i * 97u) & 0x7FFFu;
}

#define C_BACK RGB15(5, 10, 15) /* the usual backdrop */
#define C_A RGB15(31, 0, 0)
#define C_B RGB15(0, 31, 0)
#define C_C RGB15(0, 0, 31)
#define C_D RGB15(31, 31, 0)
#define C_E RGB15(0, 31, 31)

static void put16(uint8_t *mem, uint32_t addr, uint32_t value)
{
    nds_put_le16(mem + addr, (uint16_t)value);
}

static void pal_bg(uint32_t index, uint32_t color)
{
    put16(g_pal, index * 2u, color);
}

static void pal_obj(uint32_t index, uint32_t color)
{
    put16(g_pal, 0x200u + index * 2u, color);
}

/* A 16-colour tile (32 bytes) at addr, every pixel `index`. */
static void solid4(uint8_t *mem, uint32_t addr, uint32_t index)
{
    memset(mem + addr, (int)(index * 0x11u), 32);
}

/* A 256-colour tile (64 bytes) at addr, every pixel `index`. */
static void solid8(uint8_t *mem, uint32_t addr, uint32_t index)
{
    memset(mem + addr, (int)index, 64);
}

/* One pixel of a 16-colour tile: "the lower 4 bits define the color for the
 * left (!) dot, the upper 4 bits the color for the right dot". */
static void dot4(uint8_t *mem, uint32_t addr, uint32_t x, uint32_t y, uint32_t index)
{
    uint8_t *p = mem + addr + y * 4u + x / 2u;
    *p = (uint8_t)((x & 1u) ? (*p & 0x0Fu) | (index << 4) : (*p & 0xF0u) | index);
}

static void w8(uint32_t offset, uint32_t value) { nds_2d_write(&g_e, offset, value, 1); }
static void w16(uint32_t offset, uint32_t value) { nds_2d_write(&g_e, offset, value, 2); }
static void w32(uint32_t offset, uint32_t value) { nds_2d_write(&g_e, offset, value, 4); }
static uint32_t r8(uint32_t offset) { return nds_2d_read(&g_e, offset, 1); }
static uint32_t r16(uint32_t offset) { return nds_2d_read(&g_e, offset, 2); }
static uint32_t r32(uint32_t offset) { return nds_2d_read(&g_e, offset, 4); }

/* Register offsets and bits. */
enum {
    DISPCNT = 0x00, BG0CNT = 0x08, BG1CNT = 0x0A, BG2CNT = 0x0C, BG3CNT = 0x0E,
    BG0HOFS = 0x10, BG0VOFS = 0x12, BG1HOFS = 0x14,
    BG2PA = 0x20, BG2PB = 0x22, BG2PC = 0x24, BG2PD = 0x26, BG2X = 0x28, BG2Y = 0x2C,
    BG3PA = 0x30, BG3PB = 0x32, BG3PC = 0x34, BG3PD = 0x36, BG3X = 0x38, BG3Y = 0x3C,
    WIN0H = 0x40, WIN1H = 0x42, WIN0V = 0x44, WIN1V = 0x46, WININ = 0x48, WINOUT = 0x4A,
    MOSAIC = 0x4C, BLDCNT = 0x50, BLDALPHA = 0x52, BLDY = 0x54,
    DISPCAPCNT = 0x64, DISP_MMEM_FIFO = 0x68, MASTER_BRIGHT = 0x6C
};

#define DISP_ON 0x00010000u    /* display mode 1: graphics */
#define DISP_BG(n) (0x100u << (n))
#define DISP_OBJ 0x1000u
#define DISP_WIN0 0x2000u
#define DISP_WIN1 0x4000u
#define DISP_OBJWIN 0x8000u
#define DISP_3D 0x0008u
#define DISP_OBJ_1D 0x0010u
#define DISP_BG_EXTPAL (1u << 30)
#define DISP_OBJ_EXTPAL (1u << 31)

/* A machine with everything mapped and zeroed, every sprite disabled, and
 * the engine fresh out of reset. */
static void reset_scene(int engine)
{
    memset(g_bg, 0, sizeof g_bg);
    memset(g_obj, 0, sizeof g_obj);
    memset(g_pal, 0, sizeof g_pal);
    memset(g_bg_ext, 0, sizeof g_bg_ext);
    memset(g_obj_ext, 0, sizeof g_obj_ext);
    memset(g_lcdc, 0, sizeof g_lcdc);
    memset(g_3d_line, 0, sizeof g_3d_line);
    for (uint32_t n = 0; n < 128; n++) {
        memset(g_oam + n * 8u, 0, 8);
        put16(g_oam, n * 8u, 0x0200); /* OBJ disable */
    }
    memset(&g_vram, 0, sizeof g_vram);
    for (uint32_t i = 0; i < 32; i++) g_vram.bg[i] = g_bg + i * 0x4000u;
    for (uint32_t i = 0; i < 16; i++) g_vram.obj[i] = g_obj + i * 0x4000u;
    for (uint32_t i = 0; i < 4; i++) g_vram.bg_ext_pal[i] = g_bg_ext[i];
    g_vram.obj_ext_pal = g_obj_ext;
    g_vram.palette = g_pal;
    g_vram.oam = g_oam;
    for (uint32_t i = 0; i < 4; i++) {
        g_vram.lcdc[i] = g_lcdc[i];
        g_vram.capture[i] = g_lcdc[i];
    }
    g_3d = NULL;
    nds_2d_reset(&g_e, engine);
}

/*
 * nds_2d_stack_scaled() against the engine. It stacks a line's layers again
 * around a 3D picture two or three times as fine, and what each of its
 * pixels should be has a definition that needs no second opinion: the pixel
 * the engine itself would have drawn there had the screen's 3D pixel been
 * that finer one. So for each position within a pixel, the engine draws the
 * line once more from where it stood, with a 3D line made of the finer
 * pixels at that position, and the two must agree at all 256 pixels.
 *
 * The finer pixels are the 3D line's own at the first position and made up
 * at the others: any colour, with nothing there, opaque or translucent.
 * This runs for every line a test renders with the 3D layer among the
 * layers, so it sees every blend, window, priority, scroll and brightness
 * the tests below set up.
 */
static long g_stacked_lines, g_stacked_wrong;

static uint32_t finer_pixel(uint32_t own, uint32_t x, int i, int j, int n)
{
    if (i == 0 && j == 0) return own;
    uint32_t h = (x + 1u) * 2654435761u ^ (uint32_t)(i * 40503 + j * 9973 + n * 31) ^ own;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const uint32_t kind = (h >> 20) % 3u;
    const uint32_t alpha = kind == 0 ? 0u : (kind == 1 ? 31u : 1u + ((h >> 23) % 30u));
    return (h & 0x3FFFFu) | (alpha << 18);
}

static void check_stack_scaled(const Nds2d *before, int y, const Nds2dLayers *layers)
{
    static uint32_t rows[3 * 256 * 3], fine[3 * 256 * 3];
    /* The engine's second and later goes must not capture again. */
    Nds2dVram quiet = g_vram;
    for (int i = 0; i < 4; i++) quiet.capture[i] = NULL;

    for (int n = 2; n <= 3; n++) {
        const size_t wide = 256u * (size_t)n;
        for (int j = 0; j < n; j++)
            for (uint32_t x = 0; x < 256; x++)
                for (int i = 0; i < n; i++) rows[(size_t)j * wide + x * (size_t)n + (size_t)i] = finer_pixel(g_3d[x], x, i, j, n);
        memset(fine, 0xA5, sizeof fine);
        /* The 3D picture's pixels, scrolled as BG0 is. */
        const uint32_t *finer[256];
        for (uint32_t x = 0; x < 256; x++) {
            const uint32_t from = (x + layers->hofs) & 0x1FFu;
            finer[x] = from < 256 ? rows + from * (size_t)n : NULL;
        }
        nds_2d_stack_scaled(layers, 0, finer, wide, n, fine, wide);
        nds_2d_brighten_scaled(layers, fine, (size_t)n * wide);
        long wrong = 0;
        for (int j = 0; j < n; j++) {
            for (int i = 0; i < n; i++) {
                uint32_t line[256], want[256];
                for (uint32_t x = 0; x < 256; x++) line[x] = rows[(size_t)j * wide + x * (size_t)n + (size_t)i];
                Nds2d again = *before;
                nds_2d_render_line(&again, &quiet, y, line, want);
                for (uint32_t x = 0; x < 256; x++) {
                    if (fine[(size_t)j * wide + x * (size_t)n + (size_t)i] != want[x]) wrong++;
                }
            }
        }
        g_checks++;
        g_stacked_lines++;
        if (wrong) {
            g_failures++;
            g_stacked_wrong++;
            if (g_stacked_wrong <= 12)
                printf("FAIL %s: line %d stacked %d times as fine differs from the engine at %ld pixels\n", g_section,
                       y, n, wrong);
        }
    }
}

/*
 * The same for a bitmap shown pixel for pixel: a direct-colour background
 * (Nds2dLayers.direct_row) or bitmap sprites (obj_src). Here the finer
 * pixels stand for what the bitmap's own pixels would be, so the engine's
 * word on each is had by writing them into the bitmap in video memory and
 * drawing the line again, once for each position within a pixel; the bitmap
 * is put back afterwards.
 *
 * A finer pixel depends on the address it stands for and not on where it is
 * shown, so two places showing the same memory agree. A background's can be
 * there or not. A sprite's are always there: where a finer sprite pixel is
 * missing the engine might show another sprite underneath, and
 * nds_2d_stack_scaled() only has the frontmost one.
 */
static long g_bitmap_bg_lines, g_bitmap_obj_lines, g_bitmap_wrong;

static uint32_t finer_texel(const uint8_t *where, int i, int j, int n, int may_be_absent)
{
    uint32_t h = (uint32_t)(uintptr_t)where * 2654435761u ^ (uint32_t)(i * 40503 + j * 9973 + n * 31);
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const uint32_t there = may_be_absent && (h >> 28) < 4u ? 0u : 0x8000u;
    return (h & 0x7FFFu) | there;
}

static void check_stack_bitmap(const Nds2d *before, int y, const Nds2dLayers *layers)
{
    static uint32_t blocks[256 * 3 * 3], fine[3 * 256 * 3];
    /* Which layer shows a bitmap as it is stored, and where each of the
     * line's pixels is read from. */
    const uint8_t *where[256];
    int layer = -1;
    for (int k = 0; k < layers->layers && layer < 0; k++) {
        const int bg = layers->order[k];
        if (!layers->direct_row[bg]) continue;
        for (uint32_t x = 0; x < 256; x++) where[x] = layers->direct_row[bg] + x * 2u;
        layer = bg;
    }
    if (layer < 0 && layers->obj_on) {
        int any = 0;
        for (uint32_t x = 0; x < 256; x++) {
            where[x] = layers->obj_src[x];
            any = any || where[x];
        }
        if (any) layer = NDS_2D_LAYER_OBJ;
    }
    if (layer < 0) return;
    const int sprites = layer == NDS_2D_LAYER_OBJ;

    Nds2dVram quiet = g_vram;
    for (int i = 0; i < 4; i++) quiet.capture[i] = NULL;
    uint16_t saved[256];
    for (uint32_t x = 0; x < 256; x++) {
        if (where[x]) memcpy(&saved[x], where[x], 2);
    }

    for (int n = 2; n <= 3; n++) {
        const size_t wide = 256u * (size_t)n;
        const uint32_t *finer[256];
        for (uint32_t x = 0; x < 256; x++) {
            finer[x] = NULL;
            if (!where[x]) continue;
            uint32_t *block = &blocks[x * 9u];
            for (int j = 0; j < n; j++) {
                for (int i = 0; i < n; i++) {
                    const uint32_t c = finer_texel(where[x], i, j, n, !sprites);
                    /* 15 bits to line_3d's format: each channel doubled. */
                    block[j * 3 + i] = ((c & 0x1Fu) << 1) | ((c & 0x3E0u) << 2) | ((c & 0x7C00u) << 3) |
                                       ((c & 0x8000u) ? 31u << 18 : 0u);
                }
            }
            finer[x] = block;
        }
        memset(fine, 0xA5, sizeof fine);
        nds_2d_stack_scaled(layers, layer, finer, 3, n, fine, wide);
        nds_2d_brighten_scaled(layers, fine, (size_t)n * wide);

        long wrong = 0;
        for (int j = 0; j < n; j++) {
            for (int i = 0; i < n; i++) {
                for (uint32_t x = 0; x < 256; x++) {
                    if (!where[x]) continue;
                    const uint16_t texel = (uint16_t)finer_texel(where[x], i, j, n, !sprites);
                    memcpy((uint8_t *)(uintptr_t)where[x], &texel, 2);
                }
                uint32_t want[256];
                Nds2d again = *before;
                nds_2d_render_line(&again, &quiet, y, g_3d, want);
                for (uint32_t x = 0; x < 256; x++) {
                    /* Where the bitmap is not what shows, the screen's own. */
                    const uint32_t expect = where[x] ? want[x] : g_out[x];
                    if (fine[(size_t)j * wide + x * (size_t)n + (size_t)i] != expect) wrong++;
                }
            }
        }
        for (uint32_t x = 0; x < 256; x++) {
            if (where[x]) memcpy((uint8_t *)(uintptr_t)where[x], &saved[x], 2);
        }
        g_checks++;
        if (sprites) g_bitmap_obj_lines++;
        else g_bitmap_bg_lines++;
        if (wrong) {
            g_failures++;
            g_bitmap_wrong++;
            if (g_bitmap_wrong <= 12)
                printf("FAIL %s: line %d with its %s %d times as fine differs from the engine at %ld pixels\n",
                       g_section, y, sprites ? "bitmap sprites" : "bitmap background", n, wrong);
        }
    }
}

static void render(int y)
{
    static Nds2d before;
    Nds2dLayers layers;
    before = g_e;
    nds_2d_render_line_layers(&g_e, &g_vram, y, g_3d, g_out, &layers);
    if (g_3d && layers.composed && layers.has_3d) check_stack_scaled(&before, y, &layers);
    else if (layers.composed) check_stack_bitmap(&before, y, &layers);
}

/* A new frame, rendered down to line y; g_out holds that line. */
static void frame_line(int y)
{
    nds_2d_begin_frame(&g_e);
    for (int line = 0; line <= y; line++) render(line);
}

/* Sprite n's three attributes. */
static void obj_set(uint32_t n, uint32_t a0, uint32_t a1, uint32_t a2)
{
    put16(g_oam, n * 8u, a0);
    put16(g_oam, n * 8u + 2u, a1);
    put16(g_oam, n * 8u + 4u, a2);
}

/* Rotation/scaling parameter group g: "PA=07000006, PB=0700000E,
 * PC=07000016, PD=0700001E" for the first, the next 0x20 on. */
static void obj_matrix(uint32_t group, uint32_t pa, uint32_t pb, uint32_t pc, uint32_t pd)
{
    put16(g_oam, group * 32u + 6u, pa);
    put16(g_oam, group * 32u + 14u, pb);
    put16(g_oam, group * 32u + 22u, pc);
    put16(g_oam, group * 32u + 30u, pd);
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

/* What word `offset` of the register space reads after ones were written
 * to all of it: the bits GBATEK lists that are readable. */
static uint32_t readable_bits(int engine, uint32_t offset)
{
    switch (offset) {
    case 0x00: return engine ? 0xC0B1FFF7u : 0xFFFFFFFFu; /* DISPCNT: engine B lacks bits 3, 17-19, 22, 24-29 */
    case 0x08: return 0xFFFFFFFFu;                        /* BG0CNT, BG1CNT */
    case 0x0C: return 0xFFFFFFFFu;                        /* BG2CNT, BG3CNT */
    case 0x48: return 0x3F3F3F3Fu;                        /* WININ, WINOUT */
    case 0x50: return 0x1F1F3FFFu;                        /* BLDCNT, BLDALPHA */
    case 0x64: return engine ? 0u : 0xEF3F1F1Fu;          /* DISPCAPCNT, engine A only */
    case 0x6C: return 0x0000C01Fu;                        /* MASTER_BRIGHT */
    default: return 0; /* write-only, unused, or another module's (0x04, 0x60) */
    }
}

static void test_registers(void)
{
    g_section = "registers";
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        for (uint32_t offset = 0; offset < 0x70; offset += 4) EQ("after reset", r32(offset), 0);
        for (uint32_t offset = 0; offset < 0x70; offset += 4) w32(offset, 0xFFFFFFFFu);
        for (uint32_t offset = 0; offset < 0x70; offset += 4) {
            const uint32_t want = readable_bits(engine, offset);
            EQ("word", r32(offset), want);
            EQ("low half", r16(offset), want & 0xFFFFu);
            EQ("high half", r16(offset + 2), want >> 16);
            for (uint32_t i = 0; i < 4; i++) EQ("byte", r8(offset + i), (want >> (8 * i)) & 0xFFu);
        }
        for (uint32_t offset = 0; offset < 0x70; offset += 4) w32(offset, 0);
        for (uint32_t offset = 0; offset < 0x70; offset += 4) EQ("cleared", r32(offset), 0);
    }

    /* The same register through every access size and alignment. */
    reset_scene(0);
    w32(DISPCNT, 0x12345678u);
    EQ("DISPCNT", r32(DISPCNT), 0x12345678u);
    EQ("DISPCNT byte 0", r8(0), 0x78);
    EQ("DISPCNT byte 1", r8(1), 0x56);
    EQ("DISPCNT byte 2", r8(2), 0x34);
    EQ("DISPCNT byte 3", r8(3), 0x12);
    EQ("DISPCNT halfword 0", r16(0), 0x5678);
    EQ("DISPCNT halfword 2", r16(2), 0x1234);
    EQ("DISPCNT halfword at 1", r16(1), 0x3456);
    EQ("DISPCNT word at 1", r32(1), 0x00123456u); /* its fourth byte is DISPSTAT's: zero from here */
    EQ("DISPCNT word at 3", r32(3), 0x00000012u);
    w8(2, 0xAB);
    EQ("DISPCNT after byte write", r32(DISPCNT), 0x12AB5678u);
    w16(1, 0xCDEF);
    EQ("DISPCNT after halfword write at 1", r32(DISPCNT), 0x12CDEF78u);
    w16(2, 0x0001);
    EQ("DISPCNT after halfword write at 2", r32(DISPCNT), 0x0001EF78u);
    w32(2, 0xFFFF4321u); /* the upper half lands on DISPSTAT and is dropped */
    EQ("DISPCNT after word write at 2", r32(DISPCNT), 0x4321EF78u);

    w8(BLDALPHA, 0xFF);
    EQ("BLDALPHA low", r16(BLDALPHA), 0x001F);
    w8(BLDALPHA + 1, 0xFF);
    EQ("BLDALPHA", r16(BLDALPHA), 0x1F1F);
    w16(BLDALPHA, 0x0A05);
    EQ("BLDALPHA bytes", r8(BLDALPHA) | (r8(BLDALPHA + 1) << 8), 0x0A05);

    /* A word that straddles a gap and a register. */
    w16(BLDCNT, 0);
    w32(0x4E, 0xFFFFFFFFu);
    EQ("BLDCNT via 0x4E", r16(BLDCNT), 0x3FFF);
    EQ("BLDALPHA untouched", r16(BLDALPHA), 0x0A05);
    EQ("word at 0x4E", r32(0x4E), 0x3FFF0000u);

    /* Offsets and sizes the engine does not have. */
    EQ("beyond the end", r32(0x70), 0);
    EQ("far beyond", r32(0xFFFFFFFFu), 0);
    EQ("word ending past the end", r32(0x6E), 0);
    EQ("size 3", nds_2d_read(&g_e, 0, 3), 0);
    w32(0x70, 0xFFFFFFFFu);
    w32(0xFFFFFFFEu, 0xFFFFFFFFu);
    nds_2d_write(&g_e, 0, 0, 3);
    EQ("unchanged by them", r32(DISPCNT), 0x4321EF78u);
    EQ("MASTER_BRIGHT unchanged", r32(MASTER_BRIGHT), 0);

    /* Write-only registers hold what was written (the picture tests show
     * that), and read as zero. */
    w16(BG0HOFS, 0x01FF);
    w32(BG2X, 0x0FFFFFFFu);
    w16(MOSAIC, 0xFFFF);
    w16(BLDY, 0x1F);
    w16(WIN0H, 0xFFFF);
    EQ("BG0HOFS", r16(BG0HOFS), 0);
    EQ("BG2X", r32(BG2X), 0);
    EQ("MOSAIC", r16(MOSAIC), 0);
    EQ("BLDY", r16(BLDY), 0);
    EQ("WIN0H", r16(WIN0H), 0);

    /* Engine B has no capture unit and no main memory display. */
    reset_scene(1);
    w32(DISPCAPCNT, 0xFFFFFFFFu);
    w32(DISP_MMEM_FIFO, 0x12345678u);
    EQ("engine B DISPCAPCNT", r32(DISPCAPCNT), 0);
    EQ("engine B takes no FIFO pixels", g_e.mmem_pos, 0);
    EQ("engine B keeps no FIFO pixels", g_e.mmem_line[0] | g_e.mmem_line[1], 0);
    nds_2d_begin_frame(&g_e);
    EQ("engine B captures nothing", g_e.capture_active, 0);
    reset_scene(0);
    w32(DISP_MMEM_FIFO, 0x12345678u);
    EQ("engine A takes FIFO pixels", g_e.mmem_pos, 2);
    EQ("engine A FIFO pixel 0", g_e.mmem_line[0], 0x5678);
    EQ("engine A FIFO pixel 1", g_e.mmem_line[1], 0x1234);

    /* Two engines given the same calls are the same bytes. */
    static Nds2d a, b;
    memset(&a, 0xAA, sizeof a);
    memset(&b, 0x55, sizeof b);
    nds_2d_reset(&a, 0);
    nds_2d_reset(&b, 0);
    EQ("reset is complete", memcmp(&a, &b, sizeof a), 0);
}

/* ------------------------------------------------------------------ */
/* Display modes, backdrop, forced blank                               */
/* ------------------------------------------------------------------ */

static void test_display_modes(void)
{
    g_section = "display modes";
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, RGB15(1, 2, 3));

        /* "0  Display off (screen becomes white)" */
        frame_line(0);
        PX(0, WHITE18);
        PX(255, WHITE18);

        /* "1  Graphics Display": with no layer on, the backdrop, "Color 0 of
         * BG Palette 0". */
        w32(DISPCNT, DISP_ON);
        frame_line(5);
        PX(0, RGB18(2, 4, 6));
        PX(128, RGB18(2, 4, 6));
        PX(255, RGB18(2, 4, 6));
        pal_bg(0, RGB15(31, 0, 0));
        frame_line(0);
        PX(7, RGB18(62, 0, 0));
        pal_bg(0, RGB15(0, 31, 0));
        frame_line(0);
        PX(7, RGB18(0, 62, 0));
        pal_bg(0, RGB15(0, 0, 31));
        frame_line(0);
        PX(7, RGB18(0, 0, 62));
        pal_bg(0, 0xFFFF); /* bit 15 of a palette entry is not used */
        frame_line(0);
        PX(7, RGB18(62, 62, 62));

        /* "Setting Forced Blank (Bit 7) causes the video controller to
         * display white lines". */
        w32(DISPCNT, DISP_ON | 0x80u);
        frame_line(0);
        PX(0, WHITE18);
        PX(255, WHITE18);
    }

    /* "2  Engine A only: VRAM Display (Bitmap from block selected in
     * DISPCNT.18-19)": a 256x192 bitmap of 15-bit colours at the start of
     * the block, "the upper bit in each halfword is unused". */
    reset_scene(0);
    for (uint32_t block = 0; block < 4; block++) {
        put16(g_lcdc[block], (3u * 256u + 5u) * 2u, shade(10 + block));
        put16(g_lcdc[block], (3u * 256u + 6u) * 2u, shade(20 + block) | 0x8000u);
        put16(g_lcdc[block], (191u * 256u + 255u) * 2u, shade(30 + block));
    }
    for (uint32_t block = 0; block < 4; block++) {
        w32(DISPCNT, (2u << 16) | (block << 18));
        frame_line(3);
        PX(4, 0);
        PX(5, out15(shade(10 + block)));
        PX(6, out15(shade(20 + block)));
        frame_line(191);
        PX(255, out15(shade(30 + block)));
    }
    /* A block that is not mapped as LCDC memory shows black (melonDS). */
    g_vram.lcdc[2] = NULL;
    w32(DISPCNT, (2u << 16) | (2u << 18));
    frame_line(3);
    PX(5, 0);

    /* Engine B has display modes 0 and 1 only: bit 17 does not exist. */
    reset_scene(1);
    pal_bg(0, C_A);
    put16(g_lcdc[0], 0, C_B);
    w32(DISPCNT, 2u << 16);
    EQ("engine B mode 2 reads back", r32(DISPCNT), 0);
    frame_line(0);
    PX(0, WHITE18);
    w32(DISPCNT, 3u << 16);
    EQ("engine B mode 3 reads back", r32(DISPCNT), 1u << 16);
    frame_line(0);
    PX(0, out15(C_A));

    /* "3  Engine A only: Main Memory Display": the pixels sent to
     * DISP_MMEM_FIFO, two to a word. Here a line is what was written since
     * the line before. */
    reset_scene(0);
    w32(DISPCNT, 3u << 16);
    nds_2d_begin_frame(&g_e);
    for (uint32_t x = 0; x < 256; x += 2) w32(DISP_MMEM_FIFO, shade(x + 1) | (shade(x + 2) << 16));
    render(0);
    for (uint32_t x = 0; x < 256; x++) PX((int)x, out15(shade(x + 1)));
    /* Halfword and byte writes deliver the same pixels. */
    w16(DISP_MMEM_FIFO, C_A);
    w16(DISP_MMEM_FIFO + 2, C_B);
    w8(DISP_MMEM_FIFO, C_C & 0xFF);
    w8(DISP_MMEM_FIFO + 1, C_C >> 8);
    w8(DISP_MMEM_FIFO + 2, C_D & 0xFF);
    w8(DISP_MMEM_FIFO + 3, C_D >> 8);
    render(1);
    PX(0, out15(C_A));
    PX(1, out15(C_B));
    PX(2, out15(C_C));
    PX(3, out15(C_D));
    PX(4, out15(shade(5))); /* what the line before left there */
    EQ("the FIFO reads as zero", r32(DISP_MMEM_FIFO), 0);
    /* Each line fills from its left edge again, and so does each frame. */
    w16(DISP_MMEM_FIFO, C_E);
    w16(DISP_MMEM_FIFO + 2, C_A);
    render(2);
    PX(0, out15(C_E));
    PX(1, out15(C_A));
    PX(2, out15(C_C));
    for (int i = 0; i < 3; i++) w16(DISP_MMEM_FIFO, C_B);
    nds_2d_begin_frame(&g_e);
    w16(DISP_MMEM_FIFO, C_D);
    render(0);
    PX(0, out15(C_D));
    PX(1, out15(C_B));
    PX(2, out15(C_B));
    PX(3, out15(C_D));

    /* Lines outside the picture are not rendered: white, and no state moves. */
    reset_scene(0);
    w32(DISPCNT, DISP_ON);
    nds_2d_begin_frame(&g_e);
    Nds2d before = g_e;
    render(192);
    PX(0, WHITE18);
    render(-1);
    PX(255, WHITE18);
    EQ("state untouched", memcmp(&before, &g_e, sizeof g_e), 0);
}

/* ------------------------------------------------------------------ */
/* Text backgrounds                                                    */
/* ------------------------------------------------------------------ */

static void test_text_bg(void)
{
    g_section = "text BG";
    reset_scene(0);
    pal_bg(0, C_BACK);
    pal_bg(1, C_A);
    pal_bg(2, C_B);
    pal_bg(3, C_C);
    /* Tile 1: solid colour 1. Tile 2: colour 1 top left, 2 top right, 3
     * bottom left. */
    solid4(g_bg, 1 * 32, 1);
    dot4(g_bg, 2 * 32, 0, 0, 1);
    dot4(g_bg, 2 * 32, 7, 0, 2);
    dot4(g_bg, 2 * 32, 0, 7, 3);
    w16(BG0CNT, 1u << 8); /* character block 0, screen block 1 (0x800), 256x256, 16 colours */
    w32(DISPCNT, DISP_ON | DISP_BG(0));

    /* Map entry: "0-9 Tile Number, 10 Horizontal Flip, 11 Vertical Flip". */
    static const struct {
        uint32_t flags;
        uint8_t top_left, top_right, bottom_left, bottom_right; /* palette index seen at each corner */
    } k_flips[4] = {
        { 0x000, 1, 2, 3, 0 },
        { 0x400, 2, 1, 0, 3 },
        { 0x800, 3, 0, 1, 2 },
        { 0xC00, 0, 3, 2, 1 },
    };
    static const uint32_t k_corner[4] = { C_BACK, C_A, C_B, C_C };
    for (int i = 0; i < 4; i++) {
        put16(g_bg, 0x800, 2u | k_flips[i].flags);
        frame_line(0);
        PX(0, out15(k_corner[k_flips[i].top_left]));
        PX(7, out15(k_corner[k_flips[i].top_right]));
        PX(3, out15(C_BACK));
        PX(8, out15(C_BACK)); /* the next map entry is tile 0, which is empty */
        frame_line(7);
        PX(0, out15(k_corner[k_flips[i].bottom_left]));
        PX(7, out15(k_corner[k_flips[i].bottom_right]));
    }

    /* "12-15 Palette Number": all sixteen 16-colour palettes. */
    put16(g_bg, 0x802, 1);
    for (uint32_t p = 0; p < 16; p++) pal_bg(p * 16u + 1u, shade(100 + p));
    for (uint32_t p = 0; p < 16; p++) {
        put16(g_bg, 0x800, 1u | (p << 12));
        frame_line(0);
        PX(0, out15(shade(100 + p)));
        PX(8, out15(shade(100))); /* its neighbour stays in palette 0 */
    }

    /* "0-9 Tile Number (0-1023)". */
    solid4(g_bg, 0x200 * 32, 2);
    solid4(g_bg, 0x3FF * 32, 3);
    put16(g_bg, 0x804, 0x200);
    put16(g_bg, 0x806, 0x3FF);
    frame_line(0);
    PX(16, out15(C_B));
    PX(24, out15(C_C));
    put16(g_bg, 0x804, 0);
    put16(g_bg, 0x806, 0);

    /* Scrolling: "the coordinate of the upperleft first visible dot". */
    pal_bg(1, C_A);
    put16(g_bg, 0x800, 2);         /* (0,0): the marked tile */
    put16(g_bg, 0x802, 1);         /* (1,0): solid */
    put16(g_bg, 0x800 + 31 * 2, 1); /* (31,0): solid */
    w16(BG0HOFS, 7);
    frame_line(0);
    PX(0, out15(C_B));  /* the marked tile's pixel 7 */
    PX(1, out15(C_A));  /* then the solid tile */
    PX(8, out15(C_A));
    PX(9, out15(C_BACK));
    w16(BG0HOFS, 0);
    w16(BG0VOFS, 7);
    frame_line(0);
    PX(0, out15(C_C));  /* row 7 of the marked tile */
    PX(1, out15(C_BACK));
    PX(8, out15(C_A));
    frame_line(1);      /* line 1 is row 8: the second row of tiles, empty */
    PX(8, out15(C_BACK));
    /* It wraps: 256 pixels wide, so one pixel left of 0 is column 255. */
    w16(BG0VOFS, 0);
    w16(BG0HOFS, 255);
    frame_line(0);
    PX(0, out15(C_A));
    PX(1, out15(C_A)); /* then column 0 again */
    PX(8, out15(C_B));
    /* "0-8 Offset (0-511), 9-15 Not used". */
    w16(BG0HOFS, 0xFE07);
    frame_line(0);
    PX(0, out15(C_B));
    w16(BG0HOFS, 0);

    /* 256 colours, one palette: one byte a pixel, the palette number unused. */
    solid8(g_bg, 0x4000 + 1 * 64, 200);
    g_bg[0x4000 + 2 * 64 + 9] = 201; /* tile 2: pixel (1,1) */
    pal_bg(200, C_D);
    pal_bg(201, C_E);
    put16(g_bg, 0x800, 1u | 0xF000u);
    put16(g_bg, 0x802, 2u);
    put16(g_bg, 0x804, 2u | 0x400u);
    put16(g_bg, 0x806, 2u | 0x800u);
    w16(BG0CNT, (1u << 8) | (1u << 2) | 0x80u); /* character block 1 */
    frame_line(1);
    PX(0, out15(C_D));
    PX(8, out15(C_BACK));
    PX(9, out15(C_E));
    PX(16 + 6, out15(C_E)); /* mirrored */
    PX(16 + 1, out15(C_BACK));
    PX(24 + 1, out15(C_BACK)); /* turned over: its row 6 is on line 6 */
    frame_line(6);
    PX(24 + 1, out15(C_E));
    PX(9, out15(C_BACK));

    /*
     * Screen size: "Value Text Mode: 0 256x256, 1 512x256, 2 256x512,
     * 3 512x512", made of screens SC0-SC3 at the base, +2K, +4K, +6K: "when
     * Size=1 or Size=2: two areas (SC0,SC1 either horizontally or vertically
     * arranged next to each other), when Size=3: four areas (SC0,SC1 in
     * upper row, SC2,SC3 in lower row)". Each screen's first tile is its own
     * colour; scroll to each quadrant and see which screen is there.
     */
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t n = 0; n < 4; n++) {
        pal_bg(n + 1, shade(40 + n));
        solid4(g_bg, (n + 1) * 32, n + 1);
        put16(g_bg, 0x800 + n * 0x800, n + 1);
    }
    w32(DISPCNT, DISP_ON | DISP_BG(0));
    static const uint8_t k_screen[4][4] = {
        /* scrolled to (0,0), (256,0), (0,256), (256,256) */
        { 0, 0, 0, 0 },
        { 0, 1, 0, 1 },
        { 0, 0, 1, 1 },
        { 0, 1, 2, 3 },
    };
    for (uint32_t size = 0; size < 4; size++) {
        w16(BG0CNT, (1u << 8) | (size << 14));
        for (uint32_t quadrant = 0; quadrant < 4; quadrant++) {
            w16(BG0HOFS, (quadrant & 1u) ? 256 : 0);
            w16(BG0VOFS, (quadrant & 2u) ? 256 : 0);
            frame_line(0);
            PX(0, out15(shade(40 + k_screen[size][quadrant])));
            PX(8, out15(C_BACK));
        }
    }
    /* 512 wide: column 511 is the last of SC1, then SC0 again. */
    w16(BG0CNT, (1u << 8) | (3u << 14));
    put16(g_bg, 0x800 + 0x800 + 31 * 2, 4);
    w16(BG0HOFS, 511);
    w16(BG0VOFS, 0);
    frame_line(0);
    PX(0, out15(shade(43)));
    PX(1, out15(shade(40)));

    /* Every BG has its own control and scroll registers, and its own enable
     * bit (DISPCNT bits 8-11). */
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t bg = 0; bg < 4; bg++) {
        pal_bg(bg + 1, shade(50 + bg));
        solid4(g_bg, (bg + 1) * 32, bg + 1);
        put16(g_bg, 0x800 + bg * 0x800 + (2 * 32 + 1) * 2, bg + 1); /* screen block 1 + bg, tile (1,2) */
        w16(BG0CNT + 2 * bg, (1u + bg) << 8);
        w16(BG0HOFS + 4 * bg, 8);
        w16(BG0VOFS + 4 * bg, 16);
    }
    for (uint32_t bg = 0; bg < 4; bg++) {
        w32(DISPCNT, DISP_ON | DISP_BG(bg));
        frame_line(0);
        PX(0, out15(shade(50 + bg)));
        PX(8, out15(C_BACK));
        frame_line(8);
        PX(0, out15(C_BACK));
    }
    w32(DISPCNT, DISP_ON);
    frame_line(0);
    PX(0, out15(C_BACK));
    /* "Color 0 of all BG and OBJ palettes is transparent": BG1 shows through
     * an empty tile of BG0 in front of it. */
    w16(BG0HOFS, 0);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1));
    frame_line(0);
    PX(0, out15(shade(51)));
    PX(8, out15(shade(50)));
    PX(16, out15(C_BACK));
}

/* BGxCNT's character base (bits 2-5, 16K steps) and screen base (bits 8-12,
 * 2K steps), and on engine A "char base: BGxCNT.bits*16K + DISPCNT.bits*64K"
 * (DISPCNT bits 24-26) and "screen base: BGxCNT.bits*2K + DISPCNT.bits*64K"
 * (bits 27-29). */
static void test_bg_bases(void)
{
    g_section = "BG bases";

    /* All 16 character blocks: tile 1 of each is its own colour. */
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 64; i++) pal_bg(i, shade(i));
    for (uint32_t n = 0; n < 16; n++) solid8(g_bg, n * 0x4000u + 64u, n + 1);
    put16(g_bg, 0xF800, 1);
    w32(DISPCNT, DISP_ON | DISP_BG(0));
    for (uint32_t n = 0; n < 16; n++) {
        w16(BG0CNT, (31u << 8) | (n << 2) | 0x80u);
        frame_line(0);
        PX(0, out15(shade(n + 1)));
    }

    /* All 8 DISPCNT character offsets, on top of character block 1. */
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 64; i++) pal_bg(i, shade(i));
    for (uint32_t k = 0; k < 8; k++) solid8(g_bg, k * 0x10000u + 0x4000u + 64u, 20 + k);
    put16(g_bg, 0xF800, 1);
    w16(BG0CNT, (31u << 8) | (1u << 2) | 0x80u);
    for (uint32_t k = 0; k < 8; k++) {
        w32(DISPCNT, DISP_ON | DISP_BG(0) | (k << 24));
        frame_line(0);
        PX(0, out15(shade(20 + k)));
    }
    /* The sum wraps inside the 512 KiB: block 15 + 7*64K = 0xAC000 -> 0x2C000. */
    solid8(g_bg, 0x2C000u + 64u, 40);
    w16(BG0CNT, (31u << 8) | (15u << 2) | 0x80u);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | (7u << 24));
    frame_line(0);
    PX(0, out15(shade(40)));

    /* All 32 screen blocks: the first entry of each names its own tile. */
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 64; i++) pal_bg(i, shade(i));
    for (uint32_t n = 0; n < 32; n++) {
        solid8(g_bg, 0x10000u + (n + 1) * 64u, n + 1);
        put16(g_bg, n * 0x800u, n + 1);
    }
    w32(DISPCNT, DISP_ON | DISP_BG(0));
    for (uint32_t n = 0; n < 32; n++) {
        w16(BG0CNT, (n << 8) | (4u << 2) | 0x80u);
        frame_line(0);
        PX(0, out15(shade(n + 1)));
    }

    /* All 8 DISPCNT screen offsets. */
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 64; i++) pal_bg(i, shade(i));
    for (uint32_t k = 0; k < 8; k++) {
        solid8(g_bg, 0x8000u + (40 + k) * 64u, 40 + k);
        put16(g_bg, k * 0x10000u, 40 + k);
    }
    w16(BG0CNT, (2u << 2) | 0x80u);
    for (uint32_t k = 0; k < 8; k++) {
        w32(DISPCNT, DISP_ON | DISP_BG(0) | (k << 27));
        frame_line(0);
        PX(0, out15(shade(40 + k)));
    }

    /* "engine B screen base: BGxCNT.bits*2K + 0", "engine B char base:
     * BGxCNT.bits*16K + 0": the DISPCNT fields are not there. */
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, C_BACK);
        pal_bg(5, C_A);
        solid8(g_bg, 64, 5);
        put16(g_bg, 0xF800, 1);
        w16(BG0CNT, (31u << 8) | 0x80u);
        w32(DISPCNT, DISP_ON | DISP_BG(0) | (7u << 24) | (7u << 27));
        frame_line(0);
        PX(0, out15(engine ? C_A : C_BACK));
    }

    /* Engine B's BG VRAM is 128 KiB: character block 15 (0x3C000) is 0x1C000
     * there, and what the caller maps above 128 KiB is never looked at. */
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, C_BACK);
        pal_bg(6, C_A);
        pal_bg(7, C_B);
        solid8(g_bg, 0x1C000u + 64u, 6);
        solid8(g_bg, 0x3C000u + 64u, 7);
        put16(g_bg, 0x800, 1);
        w16(BG0CNT, (1u << 8) | (15u << 2) | 0x80u);
        w32(DISPCNT, DISP_ON | DISP_BG(0));
        frame_line(0);
        PX(0, out15(engine ? C_A : C_B));
    }
}

/*
 * BG extended palettes (GBATEK "DS Video Extended Palettes"): with DISPCNT
 * bit 30 set, "256-color tiles (with 16bit bgmap entries)" take their colours
 * from 16 palettes of 256 in the BG's slot, selected by the map entry's
 * palette number; "normally BG0..3 are using Slot 0..3, however BG0 and BG1
 * can be optionally changed to BG0=Slot2, and BG1=Slot3 via BG0CNT and
 * BG1CNT" (bit 13).
 */
static void test_bg_ext_palettes(void)
{
    g_section = "BG extended palettes";
    reset_scene(0);
    pal_bg(0, C_BACK);
    pal_bg(5, C_A); /* the standard palette's colour 5 */
    solid8(g_bg, 64, 5);
    for (uint32_t slot = 0; slot < 4; slot++) {
        for (uint32_t p = 0; p < 16; p++) put16(g_bg_ext[slot], (p * 256u + 5u) * 2u, shade(1 + slot * 16 + p));
    }
    for (uint32_t bg = 0; bg < 4; bg++) {
        for (uint32_t alt = 0; alt < 2; alt++) {
            /* Bit 13 moves BG0 and BG1 two slots up; a text BG2 or BG3 ignores it. */
            const uint32_t slot = (alt && bg < 2) ? bg + 2 : bg;
            w16(BG0CNT + 2 * bg, ((1u + bg) << 8) | 0x80u | (alt ? 0x2000u : 0u));
            for (uint32_t p = 0; p < 16; p++) {
                put16(g_bg, 0x800 + bg * 0x800, 1u | (p << 12));
                w32(DISPCNT, DISP_ON | DISP_BG(bg) | DISP_BG_EXTPAL);
                frame_line(0);
                PX(0, out15(shade(1 + slot * 16 + p)));
                PX(8, out15(C_BACK)); /* colour 0 of every palette is transparent */
                w32(DISPCNT, DISP_ON | DISP_BG(bg)); /* extended palettes off: one palette of 256 */
                frame_line(0);
                PX(0, out15(C_A));
            }
        }
    }

    /* "standard palette --> 16-color tiles (with 16bit bgmap entries)":
     * they keep their sixteen standard palettes. */
    solid4(g_bg, 0x4000 + 32, 3);
    pal_bg(2 * 16 + 3, C_D);
    put16(g_bg, 0x800, 1u | (2u << 12));
    w16(BG0CNT, (1u << 8) | (1u << 2));
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG_EXTPAL);
    frame_line(0);
    PX(0, out15(C_D));

    /* A slot with no VRAM behind it reads as zero: black, but there. */
    g_vram.bg_ext_pal[1] = NULL;
    put16(g_bg, 0x1000, 1u | (4u << 12));
    w16(BG1CNT, (2u << 8) | 0x80u);
    w32(DISPCNT, DISP_ON | DISP_BG(1) | DISP_BG_EXTPAL);
    frame_line(0);
    PX(0, 0);
    PX(8, out15(C_BACK));
}

/*
 * Which kind of background each BG is in each BG mode (GBATEK "DS Video BG
 * Modes / Control"):
 *
 *   Mode  BG0      BG1      BG2      BG3
 *   0     Text/3D  Text     Text     Text
 *   1     Text/3D  Text     Text     Affine
 *   2     Text/3D  Text     Affine   Affine
 *   3     Text/3D  Text     Text     Extended
 *   4     Text/3D  Text     Affine   Extended
 *   5     Text/3D  Text     Extended Extended
 *   6     3D       -        Large    -
 *
 * "Engine B: Same as above, except that: Mode 6 is reserved (no Large screen
 * bitmap), and BG0 is always Text". Mode 7 is reserved; as melonDS has it,
 * BG0 and BG1 are text there and BG2 and BG3 nothing.
 *
 * One piece of VRAM reads differently as each kind: the map entry 0x0102 at
 * the screen base is tile 0x102 of 16 colours to a text BG (data at 0x2040),
 * tile 2 of 256 colours to an affine BG, which takes only the byte (0x0080),
 * tile 0x102 of 256 colours to an extended BG (0x4080), and the large bitmap
 * starts at 0.
 */
static void test_bg_modes(void)
{
    g_section = "BG modes";
    static const char k_kind[2][8][5] = {
        { "TTTT", "TTTA", "TTAA", "TTTE", "TTAE", "TTEE", "--L-", "TT--" },
        { "TTTT", "TTTA", "TTAA", "TTTE", "TTAE", "TTEE", "----", "TT--" },
    };
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, C_BACK);
        pal_bg(1, C_A); /* seen as text */
        pal_bg(2, C_B); /* as affine */
        pal_bg(3, C_C); /* as extended */
        pal_bg(4, C_D); /* as the large bitmap */
        put16(g_bg, 0xF800, 0x0102);
        g_bg[0x2040] = 0x01;
        g_bg[0x0080] = 2;
        g_bg[0x4080] = 3;
        g_bg[0x0000] = 4;
        for (uint32_t bg = 0; bg < 4; bg++) w16(BG0CNT + 2 * bg, 31u << 8);
        w16(BG2PA, 0x100);
        w16(BG2PD, 0x100);
        w16(BG3PA, 0x100);
        w16(BG3PD, 0x100);
        for (uint32_t mode = 0; mode < 8; mode++) {
            for (uint32_t bg = 0; bg < 4; bg++) {
                w32(DISPCNT, DISP_ON | mode | DISP_BG(bg));
                frame_line(0);
                uint32_t want = C_BACK;
                switch (k_kind[engine][mode][bg]) {
                case 'T': want = C_A; break;
                case 'A': want = C_B; break;
                case 'E': want = C_C; break;
                case 'L': want = C_D; break;
                default: break;
                }
                PX(0, out15(want));
            }
        }
    }
}

/* Texel (x, y) of the test bitmap: a palette index, never 0. */
static uint32_t pattern(uint32_t x, uint32_t y)
{
    return 1u + (x * 7u + y * 13u) % 255u;
}

#define TEXEL(x, y) out15(shade(pattern((uint32_t)(x), (uint32_t)(y))))

/* A 256x256 bitmap of 256 colours at the start of BG VRAM, every texel its
 * own function of where it is, and the palette that shows it. */
static void pattern_bitmap(void)
{
    for (uint32_t y = 0; y < 256; y++) {
        for (uint32_t x = 0; x < 256; x++) g_bg[y * 256u + x] = (uint8_t)pattern(x, y);
    }
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 256; i++) pal_bg(i, shade(i));
}

/*
 * Affine tile backgrounds (GBATEK "LCD I/O BG Control", "LCD I/O BG
 * Rotation/Scaling"): a map of one byte per tile, "16x16 tiles (128x128
 * pixels), 32x32 tiles (256x256 pixels), 64x64 tiles (512x512 pixels), or
 * 128x128 tiles (1024x1024 pixels)", 256-colour tiles, and outside it
 * "either displayed as transparent or wraparound (Bit 13 of BGxCNT)".
 */
static void test_affine_bg(void)
{
    g_section = "affine BG";
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 256; i++) {
        pal_bg(i, shade(i));
        solid8(g_bg, i * 64u, i); /* tile i is all colour i */
    }
    /* For a map n tiles wide: its last tile of row 1 is tile 10 + size, its
     * first of row 1 tile 20 + size. A wrong width looks somewhere else. */
    for (uint32_t size = 0; size < 4; size++) {
        const uint32_t n = 16u << size;
        g_bg[0x8000 + 2 * n - 1] = (uint8_t)(10 + size);
        g_bg[0x8000 + n] = (uint8_t)(20 + size);
    }
    g_bg[0x8000 + 17] = 30; /* size 0: tile (1,1) */
    w16(BG2PA, 0x100);
    w16(BG2PD, 0x100);
    w32(DISPCNT, DISP_ON | 2u | DISP_BG(2));

    for (uint32_t size = 0; size < 4; size++) {
        const uint32_t pixels = 128u << size;
        const uint32_t cnt = (16u << 8) | (size << 14);
        w32(BG2Y, 8u << 8);

        w16(BG2CNT, cnt);
        w32(BG2X, (pixels - 8u) << 8);
        frame_line(0);
        PX(0, out15(shade(10 + size)));
        PX(7, out15(shade(10 + size)));
        PX(8, out15(C_BACK)); /* past the right edge */
        w16(BG2CNT, cnt | 0x2000u);
        frame_line(0);
        PX(7, out15(shade(10 + size)));
        PX(8, out15(shade(20 + size))); /* wrapped to the left edge */

        /* One pixel left of the map: "27 Sign". */
        w16(BG2CNT, cnt);
        w32(BG2X, 0x0FFFFF00u);
        frame_line(0);
        PX(0, out15(C_BACK));
        PX(1, out15(shade(20 + size)));
        w16(BG2CNT, cnt | 0x2000u);
        frame_line(0);
        PX(0, out15(shade(10 + size)));
        PX(1, out15(shade(20 + size)));
    }

    /* PA: "dx" along the line. Twice the step shows every other texel. */
    w16(BG2CNT, 16u << 8);
    w32(BG2X, 0);
    w32(BG2Y, 8u << 8);
    w16(BG2PA, 0x200);
    frame_line(0);
    PX(3, out15(shade(20))); /* texel 6 */
    PX(4, out15(shade(30))); /* texel 8 */
    PX(7, out15(shade(30))); /* texel 14 */
    PX(8, out15(C_BACK));    /* texel 16: tile (2,1) is empty */
    w16(BG2PA, 0x80);
    frame_line(0);
    PX(15, out15(shade(20))); /* texel 7 */
    PX(16, out15(shade(30))); /* texel 8 */
    PX(31, out15(shade(30)));
    PX(32, out15(C_BACK));
    /* PC: "dy" along the line. With both 1.0 the line runs down the diagonal. */
    w16(BG2PA, 0x100);
    w16(BG2PC, 0x100);
    w32(BG2Y, 0);
    frame_line(0);
    PX(7, out15(C_BACK));     /* (7,7): tile (0,0), empty */
    PX(8, out15(shade(30)));  /* (8,8): tile (1,1) */
    PX(15, out15(shade(30)));
    PX(16, out15(C_BACK));    /* (16,16): tile (2,2), empty */
    /* A negative PC climbs instead. */
    w16(BG2PC, 0xFF00);
    w32(BG2Y, 23u << 8);
    frame_line(0);
    PX(7, out15(shade(21)));  /* (7,16): tile (0,2), which is map byte 32 */
    PX(8, out15(shade(30)));  /* (8,15): tile (1,1) */
    PX(15, out15(shade(30))); /* (15,8) */
    PX(16, out15(C_BACK));    /* (16,7): tile (2,0), empty */
    w16(BG2PC, 0);

    /* Below and above the map: transparent, or wrapped round. Rows 136 and
     * -120 of a 128-pixel map are both row 8. (Map byte 17*16+1, where a
     * map that went on downwards would have row 136, is tile 31.) */
    g_bg[0x8000 + 17 * 16 + 1] = 31;
    w32(BG2X, 8u << 8);
    w32(BG2Y, 136u << 8);
    frame_line(0);
    PX(0, out15(C_BACK));
    w16(BG2CNT, (16u << 8) | 0x2000u);
    frame_line(0);
    PX(0, out15(shade(30)));
    w32(BG2Y, 0x0FFF8800u);
    frame_line(0);
    PX(0, out15(shade(30)));
    w16(BG2CNT, 16u << 8);
    frame_line(0);
    PX(0, out15(C_BACK));

    /* BG3 has its own parameters and reference point (0x30-0x3F). */
    w16(BG3CNT, 16u << 8);
    w16(BG3PA, 0x100);
    w16(BG3PD, 0x100);
    w32(BG3X, 8u << 8);
    w32(BG3Y, 8u << 8);
    w32(BG2X, 0);
    w32(BG2Y, 0);
    w32(DISPCNT, DISP_ON | 2u | DISP_BG(3));
    frame_line(0);
    PX(0, out15(shade(30)));
    PX(7, out15(shade(30)));
    PX(8, out15(C_BACK));
    /* Colour 0 is transparent: BG3 shows through the empty tile (0,0) of BG2
     * in front of it. */
    w32(DISPCNT, DISP_ON | 2u | DISP_BG(2) | DISP_BG(3));
    frame_line(0);
    PX(0, out15(shade(30)));
    PX(8, out15(C_BACK));

    /* Engine A adds DISPCNT's 64K steps to both bases here too. */
    solid8(g_bg, 0x10000u + 5u * 64u, 5);
    g_bg[0x18000] = 5;
    w32(DISPCNT, DISP_ON | 2u | DISP_BG(2) | (1u << 24) | (1u << 27));
    frame_line(0);
    PX(0, out15(shade(5)));
    w32(DISPCNT, DISP_ON | 2u | DISP_BG(2));
    frame_line(0);
    PX(0, out15(C_BACK));
}

/*
 * The internal reference points (GBATEK "LCD I/O BG Rotation/Scaling"):
 * "The above reference points are automatically copied to internal registers
 * during each vblank, specifying the origin for the first scanline. The
 * internal registers are then incremented by dmx and dmy after each
 * scanline. Caution: Writing to a reference point register by software
 * outside of the Vblank period does immediately copy the new value to the
 * corresponding internal register, that means: in the current frame, the new
 * value specifies the origin of the <current> scanline".
 */
static void test_reference_points(void)
{
    g_section = "affine reference points";
    reset_scene(0);
    pattern_bitmap();
    w16(BG3CNT, 0x4080); /* an extended BG as a 256x256 bitmap of 256 colours at 0 */
    w32(DISPCNT, DISP_ON | 3u | DISP_BG(3));
    w16(BG3PA, 0x100);
    w16(BG3PD, 0x100);

    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 6; y++) {
        render(y);
        PX(0, TEXEL(0, y));
        PX(100, TEXEL(100, y));
        PX(255, TEXEL(255, y));
    }

    /* PB moves the start of each line sideways, PD down. */
    w16(BG3PB, 0x100);
    w16(BG3PD, 0x200);
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 6; y++) {
        render(y);
        PX(0, TEXEL(y, 2 * y));
        PX(50, TEXEL(50 + y, 2 * y));
    }

    /* The origin. */
    w16(BG3PB, 0);
    w16(BG3PD, 0x100);
    w32(BG3X, 10u << 8);
    w32(BG3Y, 20u << 8);
    frame_line(0);
    PX(0, TEXEL(10, 20));
    PX(5, TEXEL(15, 20));
    frame_line(3);
    PX(0, TEXEL(10, 23));

    /* A write in the middle of the frame is the origin of the next line
     * rendered. Each register reloads its own coordinate only. */
    w16(BG3PB, 0x100);
    nds_2d_begin_frame(&g_e);
    render(0);
    PX(0, TEXEL(10, 20));
    render(1);
    PX(0, TEXEL(11, 21));
    w32(BG3Y, 100u << 8);
    render(2);
    PX(0, TEXEL(12, 100)); /* Y from the register, X where it had got to */
    render(3);
    PX(0, TEXEL(13, 101));
    w32(BG3X, 40u << 8);
    render(4);
    PX(0, TEXEL(40, 102));
    render(5);
    PX(0, TEXEL(41, 103));
    /* The next frame starts from the registers again. */
    nds_2d_begin_frame(&g_e);
    render(0);
    PX(0, TEXEL(40, 100));
    render(1);
    render(2);
    PX(0, TEXEL(42, 102));
    /* Writing the value a register already holds reloads all the same. */
    w32(BG3Y, 100u << 8);
    render(3);
    PX(0, TEXEL(43, 100));

    /* Halves and bytes of the 28-bit value. */
    w16(BG3PB, 0);
    w32(BG3X, 0);
    w32(BG3Y, 0);
    w16(BG3X, 0x0A00); /* 10.0 */
    frame_line(0);
    PX(0, TEXEL(10, 0));
    w16(BG3X + 2, 0x0001); /* + 256.0: off the bitmap */
    frame_line(0);
    PX(0, out15(C_BACK));
    w16(BG3CNT, 0x4080 | 0x2000); /* "NDS bitmap modes are supporting the Area Overflow bit" */
    frame_line(0);
    PX(0, TEXEL(10, 0));
    w16(BG3CNT, 0x4080);
    w32(BG3X, 0);
    w8(BG3X + 1, 0x14);
    frame_line(0);
    PX(0, TEXEL(20, 0));
    w8(BG3Y + 1, 0x07);
    frame_line(0);
    PX(0, TEXEL(20, 7));
    nds_2d_begin_frame(&g_e);
    render(0);
    w8(BG3Y + 1, 0x30); /* mid-frame, one byte: still a reload */
    render(1);
    PX(0, TEXEL(20, 48));

    /* "8-26 Integer portion, 27 Sign, 28-31 Not used". */
    w32(BG3Y, 0);
    w32(BG3X, 0x0FFFFF00u); /* -1.0 */
    frame_line(0);
    PX(0, out15(C_BACK));
    PX(1, TEXEL(0, 0));
    PX(2, TEXEL(1, 0));
    w32(BG3X, 0xFFFFFF00u);
    frame_line(0);
    PX(0, out15(C_BACK));
    PX(1, TEXEL(0, 0));
    w32(BG3X, 0xF0000A00u);
    frame_line(0);
    PX(0, TEXEL(10, 0));
    w32(BG3X, 0x08000000u); /* the most negative value there is */
    frame_line(0);
    PX(0, out15(C_BACK));
    PX(255, out15(C_BACK));
    w32(BG3X, 0x07FFFF00u); /* and the most positive */
    frame_line(0);
    PX(0, out15(C_BACK));
    PX(255, out15(C_BACK));

    /* Negative and fractional steps: "0-7 Fractional portion (8 bits)". */
    w32(BG3X, 0);
    w32(BG3Y, 50u << 8);
    w16(BG3PD, 0xFF00); /* -1.0 */
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 4; y++) {
        render(y);
        PX(3, TEXEL(3, 50 - y));
    }
    w32(BG3Y, 0);
    w16(BG3PD, 0x80); /* 0.5 */
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 6; y++) {
        render(y);
        PX(3, TEXEL(3, y / 2));
    }
    w16(BG3PD, 0x100);
    w16(BG3PA, 0x180); /* 1.5 */
    frame_line(0);
    PX(0, TEXEL(0, 0));
    PX(1, TEXEL(1, 0));
    PX(2, TEXEL(3, 0));
    PX(3, TEXEL(4, 0));
    PX(4, TEXEL(6, 0));
    w32(BG3X, 0x80); /* from 0.5 */
    frame_line(0);
    PX(0, TEXEL(0, 0));
    PX(1, TEXEL(2, 0));
    PX(2, TEXEL(3, 0));
    w32(BG3X, 0);
    w16(BG3PA, 0x100);
    /* PC: down the diagonal. */
    w16(BG3PC, 0x100);
    frame_line(2);
    PX(0, TEXEL(0, 2));
    PX(9, TEXEL(9, 11));
    w16(BG3PC, 0);

    /* The step is taken after every line, displayed or not (GBATEK's "after
     * each scanline"; melonDS steps only on lines that draw the BG). */
    w32(DISPCNT, DISP_ON | 3u);
    nds_2d_begin_frame(&g_e);
    render(0);
    render(1);
    render(2);
    w32(DISPCNT, DISP_ON | 3u | DISP_BG(3));
    render(3);
    PX(0, TEXEL(0, 3));

    /* BG2 is the same machine at 0x20-0x2F. */
    w16(BG2CNT, 0x4080);
    w16(BG2PA, 0x100);
    w16(BG2PB, 0x200);
    w16(BG2PD, 0x100);
    w32(BG2X, 5u << 8);
    w32(BG2Y, 9u << 8);
    w32(DISPCNT, DISP_ON | 5u | DISP_BG(2));
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 4; y++) {
        render(y);
        PX(0, TEXEL(5 + 2 * y, 9 + y));
        PX(30, TEXEL(35 + 2 * y, 9 + y));
    }
    w32(BG2X, 60u << 8);
    render(4);
    PX(0, TEXEL(60, 13));
    w32(BG2Y, 70u << 8);
    render(5);
    PX(0, TEXEL(62, 70));
    nds_2d_begin_frame(&g_e);
    render(0);
    PX(0, TEXEL(60, 70));
    render(1);
    PX(0, TEXEL(62, 71));
}

/*
 * The DS's "extended" backgrounds (GBATEK "DS Video BG Modes / Control"):
 *
 *   BGxCNT.Bit7 BGxCNT.Bit2 Extended Affine Mode Selection
 *   0           CharBaseLsb rot/scal with 16bit bgmap entries (Text+Affine mixup)
 *   1           0           rot/scal 256 color bitmap
 *   1           1           rot/scal direct color bitmap
 *
 *   bgcnt size  text     rotscal    bitmap   large bmp
 *   0           256x256  128x128    128x128  512x1024
 *   1           512x256  256x256    256x256  1024x512
 *   2           256x512  512x512    512x256  -
 *   3           512x512  1024x1024  512x512  -
 */
static void test_extended_bg(void)
{
    static const uint32_t k_size[4][2] = { { 128, 128 }, { 256, 256 }, { 512, 256 }, { 512, 512 } };

    /* 16-bit map entries: a text map's tile number, flips and palette
     * number, on a rotating background of 256-colour tiles. */
    g_section = "extended BG, 16-bit map";
    for (uint32_t bg = 2; bg < 4; bg++) {
        const uint32_t cnt = BG0CNT + 2 * bg, pa = bg == 2 ? BG2PA : BG3PA;
        reset_scene(0);
        pal_bg(0, C_BACK);
        for (uint32_t i = 1; i < 32; i++) {
            pal_bg(i, shade(i));
            if (i >= 10) solid8(g_bg, i * 64u, i);
        }
        /* Tile 0x155: colour 1 top left, 2 top right, 3 bottom left. */
        g_bg[0x155 * 64 + 0] = 1;
        g_bg[0x155 * 64 + 7] = 2;
        g_bg[0x155 * 64 + 56] = 3;
        for (uint32_t slot = 0; slot < 4; slot++) {
            for (uint32_t p = 0; p < 16; p++) put16(g_bg_ext[slot], (p * 256u + 1u) * 2u, shade(100 + slot * 16 + p));
        }
        w16(pa, 0x100);
        w16(pa + 6, 0x100);
        w16(cnt, 16u << 8);
        w32(DISPCNT, DISP_ON | 5u | DISP_BG(bg));

        static const struct {
            uint32_t flags;
            uint8_t top_left, top_right, bottom_left, bottom_right;
        } k_flips[4] = {
            { 0x000, 1, 2, 3, 0 },
            { 0x400, 2, 1, 0, 3 },
            { 0x800, 3, 0, 1, 2 },
            { 0xC00, 0, 3, 2, 1 },
        };
        const uint32_t corner[4] = { C_BACK, shade(1), shade(2), shade(3) };
        for (int i = 0; i < 4; i++) {
            put16(g_bg, 0x8000, 0x155u | k_flips[i].flags);
            frame_line(0);
            PX(0, out15(corner[k_flips[i].top_left]));
            PX(7, out15(corner[k_flips[i].top_right]));
            PX(8, out15(C_BACK));
            frame_line(7);
            PX(0, out15(corner[k_flips[i].bottom_left]));
            PX(7, out15(corner[k_flips[i].bottom_right]));
        }

        /* The palette number counts only with extended palettes, and then
         * the slot is the BG's own number; bit 13 is the overflow bit here. */
        for (uint32_t p = 0; p < 16; p++) {
            put16(g_bg, 0x8000, 0x155u | (p << 12));
            w32(DISPCNT, DISP_ON | 5u | DISP_BG(bg));
            frame_line(0);
            PX(0, out15(shade(1)));
            w32(DISPCNT, DISP_ON | 5u | DISP_BG(bg) | DISP_BG_EXTPAL);
            frame_line(0);
            PX(0, out15(shade(100 + bg * 16 + p)));
            w16(cnt, (16u << 8) | 0x2000u);
            frame_line(0);
            PX(0, out15(shade(100 + bg * 16 + p)));
            w16(cnt, 16u << 8);
        }
        w32(DISPCNT, DISP_ON | 5u | DISP_BG(bg));

        /* The four sizes, as for the affine BG, with two bytes an entry. */
        for (uint32_t size = 0; size < 4; size++) {
            const uint32_t n = 16u << size;
            put16(g_bg, 0x8000 + (2 * n - 1) * 2, 10 + size);
            put16(g_bg, 0x8000 + n * 2, 20 + size);
        }
        for (uint32_t size = 0; size < 4; size++) {
            w32(pa + 8, ((128u << size) - 8u) << 8);
            w32(pa + 12, 8u << 8);
            w16(cnt, (16u << 8) | (size << 14));
            frame_line(0);
            PX(0, out15(shade(10 + size)));
            PX(7, out15(shade(10 + size)));
            PX(8, out15(C_BACK));
            w16(cnt, (16u << 8) | (size << 14) | 0x2000u);
            frame_line(0);
            PX(8, out15(shade(20 + size)));
        }
    }

    /* Bitmaps of 256 colours: one byte a texel, through the standard palette. */
    g_section = "extended BG, 256-colour bitmap";
    for (int engine = 0; engine < 2; engine++) {
        for (uint32_t bg = 2; bg < 4; bg++) {
            const uint32_t cnt = BG0CNT + 2 * bg, pa = bg == 2 ? BG2PA : BG3PA;
            reset_scene(engine);
            pal_bg(0, C_BACK);
            for (uint32_t i = 1; i < 64; i++) pal_bg(i, shade(i));
            /* Texel (w-1, 1) of each width, texel (0, h-1) of each size,
             * texel (0, 256), and texel (0, 0). */
            g_bg[255] = 10;
            g_bg[511] = 11;
            g_bg[1023] = 12;
            g_bg[127 * 128] = 14;
            g_bg[255 * 256] = 15;
            g_bg[255 * 512] = 16;
            g_bg[511 * 512] = 17;
            g_bg[256 * 512] = 13;
            g_bg[0] = 18;
            w16(pa, 0x100);
            w16(pa + 6, 0x100);
            w32(DISPCNT, DISP_ON | 5u | DISP_BG(bg));
            for (uint32_t size = 0; size < 4; size++) {
                const uint32_t w = k_size[size][0], h = k_size[size][1];
                /* Engine B's BG VRAM ends at 128 KiB; what lies beyond it in
                 * a 512x512 bitmap is the start again. */
                const bool whole = !engine || size < 3;
                w16(cnt, 0x80u | (size << 14));
                w32(pa + 8, (w - 1u) << 8);
                w32(pa + 12, 1u << 8);
                frame_line(0);
                PX(0, out15(shade(size < 2 ? 10 + size : 12)));
                PX(1, out15(C_BACK));
                w32(pa + 8, 0);
                w32(pa + 12, (h - 1u) << 8);
                frame_line(0);
                PX(0, out15(whole ? shade(14 + size) : shade(16))); /* row 511 is row 255 there */
                w32(pa + 12, h << 8);
                frame_line(0);
                PX(0, out15(C_BACK));
                w16(cnt, 0x80u | (size << 14) | 0x2000u);
                frame_line(0);
                PX(0, out15(shade(18)));
                w16(cnt, 0x80u | (size << 14));
                w32(pa + 12, 256u << 8);
                frame_line(0);
                PX(0, out15(size < 3 ? C_BACK : whole ? shade(13) : shade(18)));
            }
        }
        /* Colour 0 is transparent: BG3 at texel (0,0) shows through BG2 at
         * texel (1,0), which is 0. */
        w16(BG2CNT, 0x80u);
        w16(BG3CNT, 0x80u | 1u);
        w32(BG2X, 1u << 8);
        w32(BG2Y, 0);
        w32(BG3X, 0);
        w32(BG3Y, 0);
        w32(DISPCNT, DISP_ON | 5u | DISP_BG(2) | DISP_BG(3));
        frame_line(0);
        PX(0, out15(shade(18)));
        w32(BG2X, 0);
        w32(BG3X, 1u << 8);
        frame_line(0);
        PX(0, out15(shade(18)));
    }

    /* "screen base used in bitmap modes as BGxCNT.bits*16K, without
     * DISPCNT.bits*64K": all 32 of them. */
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, C_BACK);
        for (uint32_t i = 1; i < 64; i++) pal_bg(i, shade(i));
        for (uint32_t n = 0; n < 32; n++) g_bg[n * 0x4000u] = (uint8_t)(n + 1);
        w16(BG3PA, 0x100);
        w16(BG3PD, 0x100);
        for (uint32_t n = 0; n < 32; n++) {
            const uint32_t want = engine ? (n & 7u) + 1u : n + 1u; /* engine B: 128 KiB */
            w16(BG3CNT, 0x80u | (n << 8));
            w32(DISPCNT, DISP_ON | 3u | DISP_BG(3));
            frame_line(0);
            PX(0, out15(shade(want)));
            w32(DISPCNT, DISP_ON | 3u | DISP_BG(3) | (7u << 24) | (7u << 27));
            frame_line(0);
            PX(0, out15(shade(want)));
        }
    }

    /* Direct colour: two bytes a texel, and "the upper bit (Bit15) is used
     * as Alpha flag. That is, Alpha=0=Transparent, Alpha=1=Normal (ie. on
     * the NDS, Direct Color values 0..7FFFh are NOT displayed)". */
    g_section = "extended BG, direct colour bitmap";
    for (uint32_t bg = 2; bg < 4; bg++) {
        const uint32_t cnt = BG0CNT + 2 * bg, pa = bg == 2 ? BG2PA : BG3PA;
        reset_scene(0);
        pal_bg(0, C_BACK);
        put16(g_bg, 255 * 2, 0x8000u | shade(10));
        put16(g_bg, 511 * 2, 0x8000u | shade(11));
        put16(g_bg, 1023 * 2, 0x8000u | shade(12));
        put16(g_bg, 127 * 128 * 2, 0x8000u | shade(14));
        put16(g_bg, 255 * 256 * 2, 0x8000u | shade(15));
        put16(g_bg, 255 * 512 * 2, 0x8000u | shade(16));
        put16(g_bg, 511 * 512 * 2, 0x8000u | shade(17));
        put16(g_bg, 256 * 512 * 2, 0x8000u | shade(13));
        put16(g_bg, 0x7FFFE, 0x8000u | shade(19)); /* the last texel of 512x512 */
        put16(g_bg, 0, 0x8000u | shade(18));
        w16(pa, 0x100);
        w16(pa + 6, 0x100);
        w32(DISPCNT, DISP_ON | 5u | DISP_BG(bg));
        for (uint32_t size = 0; size < 4; size++) {
            const uint32_t w = k_size[size][0], h = k_size[size][1];
            w16(cnt, 0x84u | (size << 14));
            w32(pa + 8, (w - 1u) << 8);
            w32(pa + 12, 1u << 8);
            frame_line(0);
            PX(0, out15(shade(size < 2 ? 10 + size : 12)));
            PX(1, out15(C_BACK));
            w32(pa + 8, 0);
            w32(pa + 12, (h - 1u) << 8);
            frame_line(0);
            PX(0, out15(shade(14 + size)));
            w32(pa + 12, h << 8);
            frame_line(0);
            PX(0, out15(C_BACK));
            w16(cnt, 0x84u | (size << 14) | 0x2000u);
            frame_line(0);
            PX(0, out15(shade(18)));
            w16(cnt, 0x84u | (size << 14));
            w32(pa + 12, 256u << 8);
            frame_line(0);
            PX(0, out15(size < 3 ? C_BACK : shade(13)));
        }
        w32(pa + 8, 511u << 8);
        w32(pa + 12, 511u << 8);
        frame_line(0);
        PX(0, out15(shade(19)));

        /* The alpha bit, and black that is there. */
        put16(g_bg, 0, shade(40));
        put16(g_bg, 2, 0x8000u | shade(41));
        put16(g_bg, 4, 0x8000u);
        put16(g_bg, 6, 0x7FFFu);
        put16(g_bg, 8, 0xFFFFu);
        w32(pa + 8, 0);
        w32(pa + 12, 0);
        frame_line(0);
        PX(0, out15(C_BACK));
        PX(1, out15(shade(41)));
        PX(2, 0);
        PX(3, out15(C_BACK));
        PX(4, RGB18(62, 62, 62));

        /* The base is the screen base field times 16K here as well. */
        put16(g_bg, 5 * 0x4000, 0x8000u | shade(42));
        w16(cnt, 0x84u | (5u << 8));
        frame_line(0);
        PX(0, out15(shade(42)));
    }
    /* On engine B a 512x256 direct colour bitmap is twice its VRAM: row 128
     * is row 0 again. */
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, C_BACK);
        put16(g_bg, 0, 0x8000u | shade(42));
        w16(BG3PA, 0x100);
        w16(BG3PD, 0x100);
        w32(BG3Y, 128u << 8);
        w16(BG3CNT, 0x84u | (2u << 14));
        w32(DISPCNT, DISP_ON | 3u | DISP_BG(3));
        frame_line(0);
        PX(0, out15(engine ? shade(42) : C_BACK));
    }
}

/*
 * "Large Screen Bitmap = rot/scal 256 color bitmap (using all 512K of 2D
 * VRAM)": BG2 in BG mode 6, engine A only, 512x1024 or 1024x512; "screen
 * base however NOT used at all for Large screen bitmap mode". GBATEK gives
 * no size for 2 and 3; melonDS has 512x256 and 512x512.
 */
static void test_large_bg(void)
{
    g_section = "large bitmap BG";
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 16; i++) pal_bg(i, shade(i));
    g_bg[0x7FFFF] = 9;     /* the last texel of either shape */
    g_bg[0] = 8;
    g_bg[255 * 512] = 7;
    g_bg[256 * 512] = 6;
    w16(BG2PA, 0x100);
    w16(BG2PD, 0x100);
    w32(DISPCNT, DISP_ON | 6u | DISP_BG(2));

    w16(BG2CNT, 0u << 14); /* 512x1024 */
    w32(BG2X, 511u << 8);
    w32(BG2Y, 1023u << 8);
    frame_line(0);
    PX(0, out15(shade(9)));
    PX(1, out15(C_BACK));
    w32(BG2X, 1023u << 8);
    w32(BG2Y, 511u << 8);
    frame_line(0);
    PX(0, out15(C_BACK));

    w16(BG2CNT, 1u << 14); /* 1024x512 */
    frame_line(0);
    PX(0, out15(shade(9)));
    PX(1, out15(C_BACK));
    w32(BG2X, 511u << 8);
    w32(BG2Y, 1023u << 8);
    frame_line(0);
    PX(0, out15(C_BACK));

    /* The overflow bit wraps it, at its own size. */
    w16(BG2CNT, 0x2000u);
    w32(BG2X, 512u << 8);
    w32(BG2Y, 1024u << 8);
    frame_line(0);
    PX(0, out15(shade(8)));
    w16(BG2CNT, 0x2000u | (1u << 14));
    w32(BG2X, 1024u << 8);
    w32(BG2Y, 512u << 8);
    frame_line(0);
    PX(0, out15(shade(8)));

    /* Sizes 2 and 3. */
    w32(BG2X, 0);
    w16(BG2CNT, 2u << 14);
    w32(BG2Y, 255u << 8);
    frame_line(0);
    PX(0, out15(shade(7)));
    w32(BG2Y, 256u << 8);
    frame_line(0);
    PX(0, out15(C_BACK));
    w16(BG2CNT, 3u << 14);
    frame_line(0);
    PX(0, out15(shade(6)));
    w32(BG2Y, 512u << 8);
    frame_line(0);
    PX(0, out15(C_BACK));

    /* Neither base field moves it, nor do extended palettes change its colours. */
    put16(g_bg_ext[2], 8 * 2, C_A);
    w32(BG2Y, 0);
    w16(BG2CNT, (31u << 8) | (15u << 2));
    w32(DISPCNT, DISP_ON | 6u | DISP_BG(2) | DISP_BG_EXTPAL | (7u << 24) | (7u << 27));
    frame_line(0);
    PX(0, out15(shade(8)));

    /* BG1 and BG3, which do not exist in this mode, do not disturb it when
     * their enable bits are set (test_bg_modes shows they draw nothing). */
    w32(DISPCNT, DISP_ON | 6u | DISP_BG(1) | DISP_BG(2) | DISP_BG(3));
    w16(BG1CNT, 0);
    w16(BG3CNT, 0);
    frame_line(0);
    PX(0, out15(shade(8)));
    PX(1, out15(C_BACK));
}

/* ------------------------------------------------------------------ */
/* Mosaic and priorities                                               */
/* ------------------------------------------------------------------ */

/* A text BG whose pixel (x, y), for x < 64 and y < 48, has palette index
 * pattern(x, y): 48 tiles of 256 colours in character block 0, mapped from
 * screen block 8. */
static void pattern_text_bg(void)
{
    for (uint32_t cy = 0; cy < 6; cy++) {
        for (uint32_t cx = 0; cx < 8; cx++) {
            const uint32_t tile = 1 + cy * 8 + cx;
            for (uint32_t py = 0; py < 8; py++) {
                for (uint32_t px = 0; px < 8; px++) {
                    g_bg[tile * 64 + py * 8 + px] = (uint8_t)pattern(cx * 8 + px, cy * 8 + py);
                }
            }
            put16(g_bg, 0x4000 + (cy * 32 + cx) * 2, tile);
        }
    }
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 256; i++) pal_bg(i, shade(i));
}

/*
 * "400004Ch - MOSAIC: 0-3 BG Mosaic H-Size (minus 1), 4-7 BG Mosaic V-Size
 * (minus 1)", for the BGs that have bit 6 of their BGxCNT set. "When setting
 * H-Size to 5, then pixels 0-5 of each display row are colorized as pixel 0,
 * pixels 6-11 as pixel 6"; rows likewise.
 */
static void test_bg_mosaic(void)
{
    g_section = "BG mosaic, text";
    reset_scene(0);
    pattern_text_bg();
    w32(DISPCNT, DISP_ON | DISP_BG(0));
    w16(BG0CNT, (8u << 8) | 0x80u | 0x40u);
    for (uint32_t s = 0; s < 16; s++) {
        w16(MOSAIC, s);
        frame_line(0);
        for (uint32_t x = 0; x < 48; x++) PX((int)x, TEXEL(x - x % (s + 1), 0));
    }
    for (uint32_t s = 0; s < 16; s++) {
        w16(MOSAIC, s << 4);
        nds_2d_begin_frame(&g_e);
        for (uint32_t y = 0; y < 40; y++) {
            render((int)y);
            PX(5, TEXEL(5, y - y % (s + 1)));
            PX(40, TEXEL(40, y - y % (s + 1)));
        }
    }
    w16(MOSAIC, 2u | (3u << 4)); /* 3 wide, 4 high */
    frame_line(5);
    PX(4, TEXEL(3, 4));
    PX(5, TEXEL(3, 4));
    PX(6, TEXEL(6, 4));
    /* The block is counted on the screen; the scroll comes on top. */
    w16(BG0VOFS, 3);
    w16(BG0HOFS, 2);
    frame_line(5);
    PX(4, TEXEL(3 + 2, 4 + 3));
    w16(BG0VOFS, 0);
    w16(BG0HOFS, 0);
    /* The sprites' sizes are not the BGs'. */
    w16(MOSAIC, 0xFF00);
    frame_line(5);
    PX(4, TEXEL(4, 5));
    /* Without bit 6 of BGxCNT the BG has no mosaic. */
    w16(MOSAIC, 0x00FF);
    w16(BG0CNT, (8u << 8) | 0x80u);
    frame_line(5);
    PX(4, TEXEL(4, 5));
    PX(17, TEXEL(17, 5));

    /* A size written in the middle of a block (as melonDS has it): the
     * block under way keeps its height, the next one has the new height. */
    w16(BG0CNT, (8u << 8) | 0x80u | 0x40u);
    w16(MOSAIC, 3u << 4);
    nds_2d_begin_frame(&g_e);
    render(0);
    render(1);
    PX(5, TEXEL(5, 0));
    w16(MOSAIC, 0);
    render(2);
    PX(5, TEXEL(5, 0));
    render(3);
    PX(5, TEXEL(5, 0));
    render(4);
    PX(5, TEXEL(5, 4));
    render(5);
    PX(5, TEXEL(5, 5));

    /* Bitmaps and affine BGs: the same, with the reference point of the
     * block's first line. */
    g_section = "BG mosaic, bitmap";
    reset_scene(0);
    pattern_bitmap();
    w16(BG3CNT, 0x4080 | 0x40);
    w16(BG3PA, 0x100);
    w16(BG3PD, 0x100);
    w32(DISPCNT, DISP_ON | 3u | DISP_BG(3));
    for (uint32_t s = 0; s < 16; s++) {
        w16(MOSAIC, s);
        frame_line(0);
        for (uint32_t x = 0; x < 48; x++) PX((int)x, TEXEL(x - x % (s + 1), 0));
    }
    for (uint32_t s = 0; s < 16; s++) {
        w16(MOSAIC, s << 4);
        nds_2d_begin_frame(&g_e);
        for (uint32_t y = 0; y < 40; y++) {
            render((int)y);
            PX(5, TEXEL(5, y - y % (s + 1)));
        }
    }
    /* Sheared by PB: the whole block shows the first line's start. */
    w16(BG3PB, 0x100);
    w16(MOSAIC, 3u << 4);
    nds_2d_begin_frame(&g_e);
    for (uint32_t y = 0; y < 12; y++) {
        const uint32_t first = y - y % 4;
        render((int)y);
        PX(5, TEXEL(5 + first, first));
    }
    w16(BG3PB, 0);
    /* Scaled by PA: the block repeats the texel its first pixel shows. */
    w16(BG3PA, 0x200);
    w16(MOSAIC, 2u);
    frame_line(0);
    PX(3, TEXEL(6, 0));
    PX(4, TEXEL(6, 0));
    PX(5, TEXEL(6, 0));
    PX(6, TEXEL(12, 0));

    /* The affine tile BG. */
    g_section = "BG mosaic, affine";
    reset_scene(0);
    pal_bg(0, C_BACK);
    pal_bg(20, shade(20));
    pal_bg(30, shade(30));
    solid8(g_bg, 20 * 64, 20);
    solid8(g_bg, 30 * 64, 30);
    g_bg[0x8000 + 16] = 20; /* tile (0,1) */
    g_bg[0x8000 + 17] = 30; /* tile (1,1) */
    w16(BG2CNT, (16u << 8) | 0x40u);
    w16(BG2PA, 0x100);
    w16(BG2PD, 0x100);
    w32(BG2Y, 8u << 8);
    w32(DISPCNT, DISP_ON | 2u | DISP_BG(2));
    w16(MOSAIC, 15u);
    frame_line(0);
    PX(7, out15(shade(20)));
    PX(8, out15(shade(20))); /* still the block's first pixel */
    PX(15, out15(shade(20)));
    PX(16, out15(C_BACK));   /* the next block starts on tile (2,1), empty */
    w16(MOSAIC, 0);
    frame_line(0);
    PX(8, out15(shade(30)));
    w32(BG2Y, 0);
    w16(MOSAIC, 15u << 4);
    frame_line(8); /* row 8 is tile (0,1), but line 8 repeats line 0 */
    PX(0, out15(C_BACK));
    w16(MOSAIC, 0);
    frame_line(8);
    PX(0, out15(shade(20)));
}

/*
 * Layer order (GBATEK "LCD I/O BG Control": "0-1 BG Priority (0-3,
 * 0=Highest)", "In case that some or all BGs are set to same priority then
 * BG0 is having the highest, and BG3 the lowest priority"; "LCD OBJ - OAM
 * Attributes": "In case that the 'Priority relative to BG' is the same than
 * the priority of one of the background layers, then the OBJ becomes higher
 * priority and is displayed on top of that BG layer").
 */
static void test_priorities(void)
{
    g_section = "BG priorities";
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t bg = 0; bg < 4; bg++) {
        pal_bg(bg + 1, shade(50 + bg));
        solid4(g_bg, (bg + 1) * 32, bg + 1);
        put16(g_bg, 0x800 + bg * 0x800, bg + 1);
    }
    w32(DISPCNT, DISP_ON | 0xF00u);
    for (uint32_t all = 0; all < 256; all++) { /* every assignment of four priorities */
        uint32_t top = 0, top_prio = 4;
        for (uint32_t bg = 0; bg < 4; bg++) {
            const uint32_t prio = (all >> (2 * bg)) & 3u;
            w16(BG0CNT + 2 * bg, ((1u + bg) << 8) | prio);
            if (prio < top_prio) {
                top = bg;
                top_prio = prio;
            }
        }
        frame_line(0);
        PX(0, out15(shade(50 + top)));
    }

    g_section = "OBJ against BG";
    reset_scene(0);
    pal_bg(0, C_BACK);
    pal_bg(1, C_A);
    solid4(g_bg, 32, 1);
    put16(g_bg, 0x800, 1);
    pal_obj(1, C_B);
    solid4(g_obj, 0, 1);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_OBJ | DISP_OBJ_1D);
    for (uint32_t bg_prio = 0; bg_prio < 4; bg_prio++) {
        for (uint32_t obj_prio = 0; obj_prio < 4; obj_prio++) {
            w16(BG0CNT, (1u << 8) | bg_prio);
            obj_set(0, 0, 0, obj_prio << 10);
            frame_line(0);
            PX(4, out15(obj_prio <= bg_prio ? C_B : C_A));
            PX(8, out15(C_BACK));
        }
    }

    /* "DS OBJ Priority: The GBA has been assigning OBJ priority in respect
     * to the 7bit OAM entry number, regardless of the OBJs 2bit BG-priority
     * attribute [...] That problem has been fixed in DS mode by combining the
     * above two values into a 9bit priority value." */
    g_section = "OBJ against OBJ";
    reset_scene(0);
    pal_bg(0, C_BACK);
    pal_obj(1, C_B);
    pal_obj(2, C_C);
    solid4(g_obj, 0, 1);
    solid4(g_obj, 32, 2);
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);
    for (uint32_t p3 = 0; p3 < 4; p3++) {
        for (uint32_t p9 = 0; p9 < 4; p9++) {
            obj_set(3, 0, 0, 0u | (p3 << 10));
            obj_set(9, 0, 4, 1u | (p9 << 10));
            frame_line(0);
            PX(2, out15(C_B));                    /* sprite 3 alone */
            PX(6, out15(p9 < p3 ? C_C : C_B));    /* both: the lower priority value, then the lower number */
            PX(10, out15(C_C));                   /* sprite 9 alone */
            PX(12, out15(C_BACK));
        }
    }
    /* With a BG between them: sprite 3 (priority 3) is behind the BG
     * (priority 1), sprite 9 (priority 0) in front of both. */
    pal_bg(1, C_A);
    solid4(g_bg, 32, 1);
    put16(g_bg, 0x800, 1);
    put16(g_bg, 0x802, 1);
    w16(BG0CNT, (1u << 8) | 1u);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_OBJ | DISP_OBJ_1D);
    obj_set(3, 0, 0, 0u | (3u << 10));
    obj_set(9, 0, 4, 1u | (0u << 10));
    frame_line(0);
    PX(2, out15(C_A));
    PX(6, out15(C_C));
    PX(10, out15(C_C));
    PX(12, out15(C_A));
}

/* ------------------------------------------------------------------ */
/* Sprites                                                             */
/* ------------------------------------------------------------------ */

/* Attribute 0. */
#define A0_AFFINE 0x0100u
#define A0_DOUBLE 0x0300u /* with rotation/scaling: double size */
#define A0_HIDE 0x0200u   /* without: not displayed */
#define A0_SEMI 0x0400u
#define A0_WINDOW 0x0800u
#define A0_BITMAP 0x0C00u
#define A0_MOSAIC 0x1000u
#define A0_256 0x2000u
/* Attribute 1. */
#define A1_HFLIP 0x1000u
#define A1_VFLIP 0x2000u

/* "OBJ Size (0..3, depends on OBJ Shape)":
 *   Size  Square   Horizontal  Vertical
 *   0     8x8      16x8        8x16
 *   1     16x16    32x8        8x32
 *   2     32x32    32x16       16x32
 *   3     64x64    64x32       32x64
 * Shape 3 is "Prohibited"; melonDS draws it 8x8. */
static const uint8_t k_obj_dim[4][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
    { { 8, 8 }, { 8, 8 }, { 8, 8 }, { 8, 8 } },
};

static void test_sprite_shapes(void)
{
    g_section = "sprite shapes";
    reset_scene(0);
    pal_bg(0, C_BACK);
    pal_obj(1, C_A);
    memset(g_obj, 0x11, 64 * 32); /* 64 solid tiles: enough for 64x64 */
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);
    for (uint32_t shape = 0; shape < 4; shape++) {
        for (uint32_t size = 0; size < 4; size++) {
            const int w = k_obj_dim[shape][size][0], h = k_obj_dim[shape][size][1];
            obj_set(0, 20u | (shape << 14), 10u | (size << 14), 0);
            nds_2d_begin_frame(&g_e);
            for (int y = 0; y <= 20 + h; y++) {
                render(y);
                if (y == 19 || y == 20 + h) {
                    PX(10, out15(C_BACK));
                    PX(10 + w - 1, out15(C_BACK));
                } else if (y == 20 || y == 20 + h - 1) {
                    PX(9, out15(C_BACK));
                    PX(10, out15(C_A));
                    PX(10 + w - 1, out15(C_A));
                    PX(10 + w, out15(C_BACK));
                }
            }
        }
    }

    /* "9 OBJ Disable (0=Normal, 1=Not displayed)" when not rotating; with
     * rotation/scaling the same bit is the double-size flag. */
    obj_set(0, 20u | A0_HIDE, 10, 0);
    frame_line(20);
    PX(10, out15(C_BACK));
    obj_matrix(0, 0x100, 0, 0, 0x100);
    obj_set(0, 20u | A0_DOUBLE, 10, 0);
    frame_line(24);
    PX(13, out15(C_BACK)); /* an 8x8 picture in the middle of 16x16 */
    PX(14, out15(C_A));
    PX(21, out15(C_A));
    PX(22, out15(C_BACK));

    /* Flips: a 16x16 sprite of tiles 4-7, each its own colour, and one
     * marked pixel at (1,2). */
    g_section = "sprite flips";
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i <= 5; i++) pal_obj(i, shade(i));
    for (uint32_t k = 0; k < 4; k++) solid4(g_obj, (4 + k) * 32, k + 1);
    dot4(g_obj, 4 * 32, 1, 2, 5);
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);
    static const struct {
        uint32_t flags;
        uint8_t quadrant[4]; /* colour at (0,0), (8,0), (0,8), (8,8) */
        uint8_t mark_x, mark_y;
    } k_flips[4] = {
        { 0, { 1, 2, 3, 4 }, 1, 2 },
        { A1_HFLIP, { 2, 1, 4, 3 }, 14, 2 },
        { A1_VFLIP, { 3, 4, 1, 2 }, 1, 13 },
        { A1_HFLIP | A1_VFLIP, { 4, 3, 2, 1 }, 14, 13 },
    };
    for (int i = 0; i < 4; i++) {
        obj_set(0, 30, 50u | (1u << 14) | k_flips[i].flags, 4);
        frame_line(30);
        PX(50, out15(shade(k_flips[i].quadrant[0])));
        PX(58, out15(shade(k_flips[i].quadrant[1])));
        frame_line(38);
        PX(50, out15(shade(k_flips[i].quadrant[2])));
        PX(58, out15(shade(k_flips[i].quadrant[3])));
        frame_line(30 + k_flips[i].mark_y);
        PX(50 + k_flips[i].mark_x, out15(shade(5)));
    }

    /* The X coordinate is 9 bits and wraps: 256-511 are -256 to -1. */
    g_section = "sprite position";
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i <= 64; i++) pal_obj(i, shade(i));
    /* A 64x64 picture of 256 colours: column x of row 0 is colour x + 1,
     * and every other row r is colour r + 1 throughout. */
    for (uint32_t ty = 0; ty < 64; ty++) {
        for (uint32_t tx = 0; tx < 64; tx++) {
            g_obj[((ty >> 3) * 8 + (tx >> 3)) * 64 + (ty & 7) * 8 + (tx & 7)] = (uint8_t)(ty ? ty + 1 : tx + 1);
        }
    }
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);
    obj_set(0, 0u | A0_256, 510u, 0); /* 8x8 at x = -2 */
    frame_line(0);
    PX(0, out15(shade(3)));
    PX(5, out15(shade(8)));
    PX(6, out15(C_BACK));
    obj_set(0, 0u | A0_256, 255u, 0);
    frame_line(0);
    PX(254, out15(C_BACK));
    PX(255, out15(shade(1)));
    obj_set(0, 0u | A0_256, 256u, 0); /* -256: nowhere */
    frame_line(0);
    PX(0, out15(C_BACK));
    PX(255, out15(C_BACK));
    obj_set(0, 0u | A0_256, 449u | (3u << 14), 0); /* 64 wide at -63 */
    frame_line(0);
    PX(0, out15(shade(64)));
    PX(1, out15(C_BACK));
    obj_set(0, 0u | A0_256, 448u | (3u << 14), 0); /* at -64 */
    frame_line(0);
    PX(0, out15(C_BACK));

    /* The Y coordinate is 8 bits and wraps at 256. */
    obj_set(0, 250u | A0_256, 0, 0); /* 8 high: rows 6 and 7 come round to lines 0 and 1 */
    frame_line(0);
    PX(0, out15(shade(7)));
    frame_line(1);
    PX(0, out15(shade(8)));
    frame_line(2);
    PX(0, out15(C_BACK));
    frame_line(191);
    PX(0, out15(C_BACK));
    obj_set(0, 190u | A0_256, 0, 0);
    frame_line(191);
    PX(0, out15(shade(2)));
    frame_line(0);
    PX(0, out15(C_BACK));
    /* "On the GBA, a large OBJ (with 64pix height, scaled into double-size
     * region of 128pix height) located near the bottom of the screen has
     * been wrapped to the top [...] on the NDS, the OBJ appears BOTH at the
     * top and bottom of the screen." At half scale, row = line in the
     * region / 2. */
    obj_matrix(0, 0x80, 0, 0, 0x80);
    obj_set(0, 160u | A0_DOUBLE | A0_256, 0u | (3u << 14), 0);
    frame_line(191);
    PX(10, out15(shade(15 + 1))); /* line 31 of the region: row 15 */
    frame_line(0);
    PX(10, out15(shade(48 + 1))); /* line 96 of the region: row 48 */
}

/*
 * Tile mapping (GBATEK "LCD OBJ - VRAM Character (Tile) Mapping", "DS Video
 * OBJs"):
 *
 *   Bit4  Bit20-21  Dimension Boundary Total
 *   0     x         2D        32       32K
 *   1     0         1D        32       32K
 *   1     1         1D        64       64K
 *   1     2         1D        128      128K
 *   1     3         1D        256      256K  ;Engine B: 128K max
 */
static void test_sprite_mapping(void)
{
    g_section = "sprite mapping";

    /* Two-dimensional, 16 colours: "when displaying a 16x16 pixel OBJ, with
     * tile number set to 04h; The upper row of the OBJ will consist of tile
     * 04h and 05h, the next row of 24h and 25h." */
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 32; i++) pal_obj(i, shade(i));
    solid4(g_obj, 0x04 * 32, 1);
    solid4(g_obj, 0x05 * 32, 2);
    solid4(g_obj, 0x24 * 32, 3);
    solid4(g_obj, 0x25 * 32, 4);
    solid4(g_obj, 0x06 * 32, 5); /* where one-dimensional mapping has its second row */
    solid4(g_obj, 0x07 * 32, 6);
    obj_set(0, 0, 0u | (1u << 14), 4);
    w32(DISPCNT, DISP_ON | DISP_OBJ);
    frame_line(0);
    PX(0, out15(shade(1)));
    PX(8, out15(shade(2)));
    frame_line(8);
    PX(0, out15(shade(3)));
    PX(8, out15(shade(4)));
    /* One-dimensional: "the upper row of the OBJ will consist of tile 04h
     * and 05h, the next row of tile 06h and 07h." */
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);
    frame_line(0);
    PX(0, out15(shade(1)));
    PX(8, out15(shade(2)));
    frame_line(8);
    PX(0, out15(shade(5)));
    PX(8, out15(shade(6)));

    /* 256 colours, a tile being two tile numbers long. 2D: "04h and 06h,
     * 24h and 26h". 1D: "04h and 06h, 08h and 0Ah". */
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i < 32; i++) pal_obj(i, shade(i));
    solid8(g_obj, 0x04 * 32, 11);
    solid8(g_obj, 0x06 * 32, 12);
    solid8(g_obj, 0x24 * 32, 13);
    solid8(g_obj, 0x26 * 32, 14);
    solid8(g_obj, 0x08 * 32, 15);
    solid8(g_obj, 0x0A * 32, 16);
    obj_set(0, 0u | A0_256, 0u | (1u << 14), 4);
    w32(DISPCNT, DISP_ON | DISP_OBJ);
    frame_line(0);
    PX(0, out15(shade(11)));
    PX(8, out15(shade(12)));
    frame_line(8);
    PX(0, out15(shade(13)));
    PX(8, out15(shade(14)));
    /* "the lower bit of the tile number should be zero (in 2-dimensional
     * mapping mode, the bit is completely ignored)": tile number 5 is tile
     * number 4, not the 64 bytes that start half a tile later. */
    memset(g_obj + 0x04 * 32 + 32, 17, 32); /* the lower half of that tile */
    obj_set(0, 0u | A0_256, 0u | (1u << 14), 5);
    frame_line(0);
    PX(0, out15(shade(11)));
    PX(8, out15(shade(12)));
    frame_line(4);
    PX(0, out15(shade(17)));
    obj_set(0, 0u | A0_256, 0u | (1u << 14), 4);
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);
    frame_line(0);
    PX(0, out15(shade(11)));
    PX(8, out15(shade(12)));
    frame_line(8);
    PX(0, out15(shade(15)));
    PX(8, out15(shade(16)));

    /* "TileVramAddress = TileNumber * BoundaryValue": tile 3 is at 96, 192,
     * 384 or 768. "Even if the boundary gets changed, OBJs are kept composed
     * of 8x8 tiles": the tiles that follow are 32 (or 64) bytes on. */
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, C_BACK);
        for (uint32_t i = 1; i < 32; i++) pal_obj(i, shade(i));
        for (uint32_t b = 0; b < 4; b++) {
            solid4(g_obj, 3u * (32u << b), 1 + b);
            solid4(g_obj, 3u * (32u << b) + 32u, 5 + b);
        }
        /* Above the tiles of boundaries 0-3: a 256-colour 16x16 picture at tile 8, boundary 3. */
        for (uint32_t k = 0; k < 4; k++) solid8(g_obj, 8u * 256u + k * 64u, 20 + k);
        for (uint32_t b = 0; b < 4; b++) {
            w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D | (b << 20));
            obj_set(0, 0u | (1u << 14), 0, 3); /* 16x8: two tiles */
            frame_line(0);
            PX(0, out15(shade(1 + b)));
            PX(8, out15(shade(5 + b)));
        }
        obj_set(0, 0u | A0_256, 0u | (1u << 14), 8);
        frame_line(0);
        PX(0, out15(shade(20)));
        PX(8, out15(shade(21)));
        frame_line(8);
        PX(0, out15(shade(22)));
        PX(8, out15(shade(23)));

        /* The last tile number at the largest boundary is at 0x3FF00, which
         * engine B, with 128 KiB of OBJ VRAM, has at 0x1FF00. */
        solid4(g_obj, 0x3FF00, 9);
        solid4(g_obj, 0x1FF00, 10);
        obj_set(0, 0, 0, 0x3FF);
        frame_line(0);
        PX(0, out15(shade(engine ? 10 : 9)));
        /* Without one-dimensional mapping the boundary bits mean nothing. */
        solid4(g_obj, 3 * 32, 1);
        obj_set(0, 0, 0, 3);
        w32(DISPCNT, DISP_ON | DISP_OBJ | (3u << 20));
        frame_line(0);
        PX(0, out15(shade(1)));
    }
}

/* Sprite colours (GBATEK "LCD OBJ - OAM Attributes": "13 Colors/Palettes
 * (0=16/16, 1=256/1)", "12-15 Palette Number (0-15) (Not used in 256
 * color/1 palette mode)"; "DS Video Extended Palettes": "OBJ Extended
 * Palette enabled in DISPCNT Bit 31, when enabled, 16 colors x 16 palettes
 * --> standard palette memory, 256 colors x 16 palettes --> extended palette
 * memory"). */
static void test_sprite_palettes(void)
{
    g_section = "sprite palettes";
    reset_scene(0);
    pal_bg(0, C_BACK);
    pal_bg(1, C_E); /* the BG palette is not the sprites' */
    pal_bg(200, C_E);
    solid4(g_obj, 0, 1);
    solid8(g_obj, 64, 200); /* tile number 2 */
    for (uint32_t p = 0; p < 16; p++) {
        pal_obj(p * 16 + 1, shade(60 + p));
        put16(g_obj_ext, (p * 256u + 200u) * 2u, shade(80 + p));
    }
    pal_obj(200, C_D);
    for (uint32_t p = 0; p < 16; p++) {
        for (uint32_t ext = 0; ext < 2; ext++) {
            w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D | (ext ? DISP_OBJ_EXTPAL : 0u));
            obj_set(0, 0, 0, 0u | (p << 12));
            obj_set(1, 0u | A0_256, 16, 2u | (p << 12));
            frame_line(0);
            PX(0, out15(shade(60 + p)));                 /* 16 colours: its palette, either way */
            PX(16, out15(ext ? shade(80 + p) : C_D));    /* 256 colours */
        }
    }
    /* The BG's extended palette enable is not the sprites'. */
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D | DISP_BG_EXTPAL);
    frame_line(0);
    PX(16, out15(C_D));
    /* No VRAM behind the extended palette: black, but there. */
    g_vram.obj_ext_pal = NULL;
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D | DISP_OBJ_EXTPAL);
    frame_line(0);
    PX(16, 0);
    PX(24, out15(C_BACK));
}

/*
 * Rotation and scaling (GBATEK "LCD OBJ - OAM Rotation/Scaling Parameters"):
 * "The OBJ Rotation Center is always [...] in the middle of the object", and
 * PA-PD work as for a BG: the texel under screen offset (x, y) from the
 * centre is the picture's centre + (PA*x + PB*y, PC*x + PD*y).
 */
static void test_sprite_affine(void)
{
    g_section = "sprite rotation/scaling";
    reset_scene(0);
    pal_bg(0, C_BACK);
    /* An 8x8 picture of 256 colours whose texel (x, y) is colour 1 + x + 8y.
     * Around it, where texels just outside the picture would be read from
     * (the tiles after it, and the end of OBJ VRAM before it), colour 60. */
    for (uint32_t i = 0; i < 64; i++) {
        g_obj[i] = (uint8_t)(i + 1);
        pal_obj(i + 1, shade(i + 1));
    }
    memset(g_obj + 64, 60, 3 * 64);
    memset(g_obj + sizeof g_obj - 0x400, 60, 0x400);
#define AT(x, y) out15(shade(1 + (x) + 8 * (y)))
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);

    /* The identity. */
    obj_matrix(0, 0x100, 0, 0, 0x100);
    obj_set(0, 30u | A0_AFFINE | A0_256, 40, 0);
    frame_line(30);
    PX(39, out15(C_BACK));
    PX(40, AT(0, 0));
    PX(47, AT(7, 0));
    PX(48, out15(C_BACK));
    frame_line(37);
    PX(40, AT(0, 7));
    PX(47, AT(7, 7));

    /* PA = PD = 0.5: twice the size. In the sprite's own 8x8 rectangle only
     * the middle of the picture fits: screen x 0..7 are texels 2,2,3,3,4,4,5,5. */
    obj_matrix(0, 0x80, 0, 0, 0x80);
    frame_line(30);
    PX(40, AT(2, 2));
    PX(41, AT(2, 2));
    PX(42, AT(3, 2));
    PX(47, AT(5, 2));
    PX(48, out15(C_BACK));
    frame_line(33);
    PX(43, AT(3, 3));
    frame_line(34);
    PX(44, AT(4, 4));
    frame_line(37);
    PX(47, AT(5, 5));
    frame_line(38);
    PX(44, out15(C_BACK));

    /* "When Double-Size is set: The sprite is rotated, and then display
     * inside of the double-sized (not rotated) rectangular area": 16x16,
     * which shows all of it. */
    obj_set(0, 30u | A0_DOUBLE | A0_256, 40, 0);
    frame_line(30);
    PX(39, out15(C_BACK));
    PX(40, AT(0, 0));
    PX(41, AT(0, 0));
    PX(42, AT(1, 0));
    PX(55, AT(7, 0));
    PX(56, out15(C_BACK));
    frame_line(35);
    PX(42, AT(1, 2));
    frame_line(45);
    PX(55, AT(7, 7));
    frame_line(46);
    PX(48, out15(C_BACK));
    /* At full size the picture sits in the middle of the double rectangle. */
    obj_matrix(0, 0x100, 0, 0, 0x100);
    frame_line(34);
    PX(43, out15(C_BACK));
    PX(44, AT(0, 0));
    PX(51, AT(7, 0));
    PX(52, out15(C_BACK));
    frame_line(33);
    PX(44, out15(C_BACK));
    frame_line(41);
    PX(51, AT(7, 7));
    frame_line(42);
    PX(51, out15(C_BACK));

    /* A quarter turn: PB = 1, PC = -1. Texel = (y, 8 - x) for screen (x, y)
     * of the rectangle; column 0 falls off the picture. */
    obj_matrix(0, 0, 0x100, 0xFF00, 0);
    obj_set(0, 30u | A0_AFFINE | A0_256, 40, 0);
    frame_line(30);
    PX(40, out15(C_BACK));
    PX(41, AT(0, 7));
    PX(47, AT(0, 1));
    frame_line(32);
    PX(47, AT(2, 1));
    PX(44, AT(2, 4));

    /* "9-13 Rotation/Scaling Parameter Selection (0-31)": every group. With
     * all four parameters zero, every pixel is the centre texel (4,4). */
    for (uint32_t g = 0; g < 32; g++) {
        for (uint32_t other = 0; other < 32; other++) obj_matrix(other, 0, 0, 0, 0);
        obj_matrix(g, 0x100, 0, 0, 0x100);
        obj_set(0, 30u | A0_AFFINE | A0_256, 40u | (g << 9), 0);
        frame_line(30);
        PX(40, AT(0, 0));
        obj_set(0, 30u | A0_AFFINE | A0_256, 40u | (((g + 1u) & 31u) << 9), 0);
        frame_line(30);
        PX(40, AT(4, 4));
    }
#undef AT
}

/*
 * Sprite mosaic: "8-11 OBJ Mosaic H-Size (minus 1), 12-15 OBJ Mosaic V-Size
 * (minus 1)" for the sprites with bit 12 of attribute 0. How the blocks
 * treat sprite edges and transparent pixels is known from melonDS only.
 */
static void test_sprite_mosaic(void)
{
    g_section = "sprite mosaic";
    reset_scene(0);
    pal_bg(0, C_BACK);
    for (uint32_t i = 1; i <= 64; i++) pal_obj(i, shade(i));
    /* 64x64, 256 colours: texel (x, y) is colour 1 + (x + y) % 64. */
    for (uint32_t ty = 0; ty < 64; ty++) {
        for (uint32_t tx = 0; tx < 64; tx++) {
            g_obj[((ty >> 3) * 8 + (tx >> 3)) * 64 + (ty & 7) * 8 + (tx & 7)] = (uint8_t)(1 + (tx + ty) % 64);
        }
    }
#define AT(x, y) out15(shade(1 + ((uint32_t)(x) + (uint32_t)(y)) % 64))
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);
    obj_set(0, 0u | A0_MOSAIC | A0_256, 0u | (3u << 14), 0);
    for (uint32_t s = 0; s < 16; s++) {
        w16(MOSAIC, s << 8);
        frame_line(0);
        for (uint32_t x = 0; x < 64; x++) PX((int)x, AT(x - x % (s + 1), 0));
    }
    for (uint32_t s = 0; s < 16; s++) {
        w16(MOSAIC, s << 12);
        nds_2d_begin_frame(&g_e);
        for (uint32_t y = 0; y < 48; y++) {
            render((int)y);
            PX(5, AT(5, y - y % (s + 1)));
        }
    }
    /* The BGs' sizes are not the sprites', and a sprite without the bit
     * has no mosaic. */
    w16(MOSAIC, 0x00FF);
    frame_line(5);
    PX(7, AT(7, 5));
    w16(MOSAIC, 0xFF00);
    obj_set(0, 0u | A0_256, 0u | (3u << 14), 0);
    frame_line(5);
    PX(7, AT(7, 5));

    /* A sprite that does not start on a block boundary: its first pixels
     * repeat its own first pixel, since the pixel left of it is no mosaic
     * pixel; from the next boundary on it follows the screen's blocks. */
    w16(MOSAIC, 3u << 8); /* 4 wide */
    obj_set(0, 0u | A0_MOSAIC | A0_256, 2u, 0); /* 8x8 at x = 2 */
    frame_line(0);
    PX(1, out15(C_BACK));
    PX(2, AT(0, 0));
    PX(3, AT(0, 0));
    PX(4, AT(2, 0));
    PX(7, AT(2, 0));
    PX(8, AT(6, 0));
    PX(9, AT(6, 0));
    PX(10, out15(C_BACK));

    /* A block whose first pixel is a transparent pixel of the sprite is
     * transparent throughout. */
    g_obj[0] = 0; /* texel (0,0) */
    obj_set(0, 0u | A0_MOSAIC | A0_256, 0u, 0);
    frame_line(0);
    PX(0, out15(C_BACK));
    PX(1, out15(C_BACK));
    PX(3, out15(C_BACK));
    PX(4, AT(4, 0));
    PX(7, AT(4, 0));
    frame_line(1);
    PX(0, AT(0, 1));
    PX(3, AT(0, 1));
    /* A sprite without the bit next to it keeps its own pixels. */
    obj_set(1, 0u | A0_256, 8u, 0);
    frame_line(1);
    PX(7, AT(4, 1));
    PX(8, AT(0, 1));
    PX(9, AT(1, 1));
    PX(11, AT(3, 1));
    obj_set(1, A0_HIDE, 0, 0);
    g_obj[0] = 1;

    /* A transparent pixel of a mosaic sprite in front of a plain sprite
     * leaves that sprite's pixel alone: the place was taken already. */
    g_obj[0x1000] = 40; /* tile number 128: one pixel at (0,0), the rest transparent */
    obj_set(0, 0u | A0_MOSAIC | A0_256, 0u, 128);
    obj_set(5, 0u | A0_256, 0u, 0);
    frame_line(0);
    PX(0, out15(shade(40)));
    PX(1, AT(1, 0));
    PX(2, AT(2, 0));
    PX(3, AT(3, 0));
    PX(4, AT(4, 0));
    obj_set(5, A0_HIDE, 0, 0);
    /* And a plain sprite in front of a mosaic sprite covers it, mosaic and
     * all: its pixels are its own. (Tile number 2 is the picture from
     * column 8 on.) */
    obj_set(0, 0u | A0_256, 0u, 2);
    obj_set(5, 0u | A0_MOSAIC | A0_256, 0u, 0);
    frame_line(0);
    PX(0, AT(8, 0));
    PX(1, AT(9, 0));
    PX(3, AT(11, 0));
    obj_set(0, A0_HIDE, 0, 0);
    frame_line(0);
    PX(1, AT(0, 0)); /* the sprite behind, by itself */
    obj_set(5, A0_HIDE, 0, 0);

    /* What a block repeats is the whole pixel, its priority included: over
     * a BG at priority 1, a sprite at priority 2 that repeats the pixel of
     * a sprite at priority 0 is in front of the BG there. */
    pal_bg(1, C_A);
    solid4(g_bg, 32, 1);
    for (uint32_t i = 0; i < 32; i++) put16(g_bg, 0x800 + i * 2, 1);
    w16(BG0CNT, (1u << 8) | 1u);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_OBJ | DISP_OBJ_1D);
    obj_set(0, 0u | A0_MOSAIC | A0_256, 0u, 0);
    obj_set(1, 0u | A0_MOSAIC | A0_256, 8u, 0u | (2u << 10));
    w16(MOSAIC, 0);
    frame_line(0);
    PX(7, AT(7, 0));
    PX(8, out15(C_A));
    w16(MOSAIC, 15u << 8); /* 16 wide */
    frame_line(0);
    PX(7, AT(0, 0));
    PX(8, AT(0, 0));
    PX(15, AT(0, 0));
    PX(16, out15(C_A));
    obj_set(1, A0_HIDE, 0, 0);
    w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);

    /* Vertically, a sprite that begins inside a block: the lines before
     * the next boundary show their own rows (as this project's GBA renderer
     * has it), the blocks after that the row of their first line. */
    w16(MOSAIC, 3u << 12); /* 4 high */
    obj_set(0, 2u | A0_MOSAIC | A0_256, 0u, 0); /* 8x8 at y = 2 */
    static const uint8_t k_row[10] = { 0, 0, 0, 1, 2, 2, 2, 2, 6, 6 }; /* row shown on lines 2-9 */
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 10; y++) {
        render(y);
        PX(3, y < 2 ? out15(C_BACK) : AT(3, k_row[y]));
    }

    /* OBJ-window sprites take no part in the mosaic (melonDS), in either
     * direction. */
    w16(BG0CNT, 1u << 8);
    w16(MOSAIC, 3u << 8);
    w16(WINOUT, 0x0001); /* outside: BG0; inside the OBJ window: nothing */
    g_obj[0] = 0;        /* its first pixel is transparent: the window starts at x = 1 */
    obj_set(0, 0u | A0_WINDOW | A0_MOSAIC | A0_256, 0u, 0);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_OBJ | DISP_OBJ_1D | DISP_OBJWIN);
    frame_line(0);
    PX(0, out15(C_A));
    PX(1, out15(C_BACK));
    PX(7, out15(C_BACK));
    PX(8, out15(C_A));
    w16(MOSAIC, 3u << 12);
    frame_line(1); /* row 1 has no hole: the window is whole on line 1 */
    PX(0, out15(C_BACK));
    PX(7, out15(C_BACK));
    PX(8, out15(C_A));
#undef AT
}

/*
 * Bitmap sprites (GBATEK "DS Video OBJs"): OBJ mode 3. "Bitmap OBJs are
 * 15bit Direct Color data, plus 1bit Alpha flag (in bit15)."
 *
 *   Bit6 Bit5 Bit22 Dimension    Boundary   Total
 *   0    0    x     2D/128 dots  8x8 dots   128K  ;Source Bitmap width 128 dots
 *   0    1    x     2D/256 dots  8x8 dots   128K  ;Source Bitmap width 256 dots
 *   1    0    0     1D           128 bytes  128K  ;Source Width = Target Width
 *   1    0    1     1D           256 bytes  256K  ;Engine A only
 *   1    1    x     Reserved
 *
 *   1D_BitmapVramAddress = TileNumber(0..3FFh) * BoundaryValue(128..256)
 *   2D_BitmapVramAddress = (TileNo AND MaskX)*10h + (TileNo AND NOT MaskX)*80h
 */
static void test_bitmap_sprites(void)
{
    g_section = "bitmap sprites";
    /* Where texels (0,0) and (2,3) of tile number 0x13 are in each
     * configuration, by the formulas above: */
    static const struct {
        uint32_t dispcnt;
        uint32_t origin, texel_2_3; /* byte addresses */
    } k_config[4] = {
        { 0, 0x830, 0x830 + 3 * 256 + 4 },                 /* X = 3, Y = 1; rows of 128 dots */
        { 0x20, 0x130, 0x130 + 3 * 512 + 4 },              /* X = 19, Y = 0; rows of 256 dots */
        { 0x40, 0x13 * 128, 0x13 * 128 + 3 * 16 + 4 },     /* rows of the sprite's 8 dots */
        { 0x40 | (1u << 22), 0x13 * 256, 0x13 * 256 + 3 * 16 + 4 },
    };
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, C_BACK);
        for (uint32_t k = 0; k < 4; k++) {
            put16(g_obj, k_config[k].origin, 0x8000u | shade(1 + k));
            put16(g_obj, k_config[k].texel_2_3, 0x8000u | shade(11 + k));
        }
        obj_set(0, 20u | A0_BITMAP, 30, 0x13u | (15u << 12));
        for (uint32_t k = 0; k < 4; k++) {
            /* Engine B has no bit 22: its one-dimensional boundary is 128 bytes. */
            const uint32_t seen = (engine && k == 3) ? 2 : k;
            w32(DISPCNT, DISP_ON | DISP_OBJ | k_config[k].dispcnt);
            frame_line(20);
            PX(30, out15(shade(1 + seen)));
            PX(31, out15(C_BACK));
            frame_line(23);
            PX(32, out15(shade(11 + seen)));
            PX(30, out15(C_BACK));
        }
        /* "1 1 x Reserved": nothing is drawn (melonDS). */
        w32(DISPCNT, DISP_ON | DISP_OBJ | 0x60u);
        frame_line(20);
        PX(30, out15(C_BACK));
        /* Bit 4, the tile sprites' mapping, is not the bitmap sprites'. */
        w32(DISPCNT, DISP_ON | DISP_OBJ | DISP_OBJ_1D);
        frame_line(20);
        PX(30, out15(shade(1)));
    }

    reset_scene(0);
    pal_bg(0, C_BACK);
    /* The highest tile number in each two-dimensional layout:
     * 0x3FF = X 15, Y 63 (128 dots wide) or X 31, Y 31 (256 dots wide). */
    put16(g_obj, 15 * 0x10 + 0x3F0 * 0x80, 0x8000u | shade(5));
    put16(g_obj, 31 * 0x10 + 0x3E0 * 0x80, 0x8000u | shade(6));
    obj_set(0, 0u | A0_BITMAP, 0, 0x3FFu | (15u << 12));
    w32(DISPCNT, DISP_ON | DISP_OBJ);
    frame_line(0);
    PX(0, out15(shade(5)));
    w32(DISPCNT, DISP_ON | DISP_OBJ | 0x20u);
    frame_line(0);
    PX(0, out15(shade(6)));

    /* One-dimensional: "Source Width = Target Width", so the second row of
     * a 16x16 sprite is 16 pixels on, of an 8x8 one 8 pixels. */
    reset_scene(0);
    pal_bg(0, C_BACK);
    put16(g_obj, 0x200, 0x8000u | shade(1));          /* tile 4: (0,0) */
    put16(g_obj, 0x200 + 16 * 2, 0x8000u | shade(2)); /* 16 pixels on */
    put16(g_obj, 0x200 + 8 * 2, 0x8000u | shade(3));  /* 8 pixels on */
    w32(DISPCNT, DISP_ON | DISP_OBJ | 0x40u);
    obj_set(0, 0u | A0_BITMAP, 0u | (1u << 14), 4u | (15u << 12)); /* 16x16 */
    frame_line(0);
    PX(0, out15(shade(1)));
    PX(8, out15(shade(3)));
    frame_line(1);
    PX(0, out15(shade(2)));
    obj_set(0, 0u | A0_BITMAP, 0u, 4u | (15u << 12)); /* 8x8 */
    frame_line(1);
    PX(0, out15(shade(3)));

    /* The alpha flag, flips, rotation, priority: as for any sprite; bit 13
     * of attribute 0 ("should be set to zero") changes nothing. */
    reset_scene(0);
    pal_bg(0, C_BACK);
    put16(g_obj, 0, 0x8000u | shade(1));
    put16(g_obj, 2, shade(2));  /* alpha flag clear: not displayed */
    put16(g_obj, 4, 0x8000u);   /* black, displayed */
    put16(g_obj, 14, 0x8000u | shade(4));
    w32(DISPCNT, DISP_ON | DISP_OBJ | 0x40u);
    obj_set(0, 0u | A0_BITMAP, 0, 0u | (15u << 12));
    frame_line(0);
    PX(0, out15(shade(1)));
    PX(1, out15(C_BACK));
    PX(2, 0);
    PX(7, out15(shade(4)));
    obj_set(0, 0u | A0_BITMAP | A0_256, 0, 0u | (15u << 12));
    frame_line(0);
    PX(0, out15(shade(1)));
    PX(7, out15(shade(4)));
    obj_set(0, 0u | A0_BITMAP, 0u | A1_HFLIP, 0u | (15u << 12));
    frame_line(0);
    PX(0, out15(shade(4)));
    PX(7, out15(shade(1)));
    obj_matrix(0, 0x100, 0, 0, 0x100);
    obj_set(0, 0u | A0_BITMAP | A0_AFFINE, 0, 0u | (15u << 12));
    frame_line(0);
    PX(0, out15(shade(1)));
    PX(7, out15(shade(4)));

    /*
     * "the color bits (Attr 2, Bit 12-15) are used as Alpha-OAM value". As
     * melonDS has it: 0 hides the sprite; otherwise it is blended, like a
     * semi-transparent one, over a 2nd-target pixel, with EVA = value + 1
     * and EVB = 16 - EVA (BLDALPHA is not used), and is plain elsewhere.
     * Red (62,0,0) over a green backdrop (0,62,0):
     *   I = MIN(63, (I1st*EVA + I2nd*EVB + 8) / 16)
     */
    reset_scene(0);
    pal_bg(0, C_B);
    for (uint32_t i = 0; i < 8; i++) put16(g_obj, i * 2, 0x8000u | C_A);
    w32(DISPCNT, DISP_ON | DISP_OBJ | 0x40u);
    w16(BLDALPHA, 0x1010); /* would saturate if it were used */
    for (uint32_t alpha = 0; alpha < 16; alpha++) {
        obj_set(0, 0u | A0_BITMAP, 0, 0u | (alpha << 12));
        w16(BLDCNT, 0);
        frame_line(0);
        PX(0, alpha ? RGB18(62, 0, 0) : RGB18(0, 62, 0));
        w16(BLDCNT, 0x2000); /* the backdrop is a 2nd target */
        frame_line(0);
        const uint32_t eva = alpha + 1, evb = 15 - alpha;
        PX(0, alpha ? RGB18((62 * eva + 8) / 16, (62 * evb + 8) / 16, 0) : RGB18(0, 62, 0));
    }
    obj_set(0, 0u | A0_BITMAP, 0, 0u | (7u << 12)); /* EVA 8, EVB 8 */
    frame_line(0);
    PX(0, RGB18(31, 31, 0));
    obj_set(0, 0u | A0_BITMAP, 0, 0u | (1u << 12)); /* EVA 2, EVB 14: (124+8)/16, (868+8)/16 */
    frame_line(0);
    PX(0, RGB18(8, 54, 0));
    obj_set(0, 0u | A0_BITMAP, 0, 0u | (15u << 12)); /* EVA 16, EVB 0 */
    frame_line(0);
    PX(0, RGB18(62, 0, 0));
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

/* Four text BGs that each cover the whole screen in one colour, BG n at
 * priority n, over the usual backdrop. */
static uint32_t bg_color(uint32_t bg)
{
    return shade(50 + bg);
}

static void four_solid_bgs(void)
{
    pal_bg(0, C_BACK);
    for (uint32_t bg = 0; bg < 4; bg++) {
        pal_bg(bg + 1, bg_color(bg));
        solid4(g_bg, (bg + 1) * 32, bg + 1);
        for (uint32_t i = 0; i < 1024; i++) put16(g_bg, 0x800 + bg * 0x800 + i * 2, bg + 1);
        w16(BG0CNT + 2 * bg, ((1u + bg) << 8) | bg);
    }
}

#define WIN(first, second) ((uint32_t)(((first) << 8) | (second)))

/*
 * Windows (GBATEK "LCD I/O Window Feature"): WININ and WINOUT say which of
 * BG0-BG3 (bits 0-3), OBJ (bit 4) and the colour special effects (bit 5)
 * show inside window 0, window 1, the OBJ window and outside all of them.
 * "Window 0 is having highest priority, Window 1 medium, and Obj Window
 * lowest priority. Outside of Window is having zero priority".
 */
static void test_windows(void)
{
    g_section = "windows";
    reset_scene(0);
    four_solid_bgs();
    pal_obj(1, C_D);
    solid4(g_obj, 0, 1);

    /* Window 0: X1..X2-1 by Y1..Y2-1 ("X2, Rightmost coordinate of window,
     * plus 1"). Inside BG0 only, outside nothing: the backdrop. */
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_WIN0);
    w16(WIN0H, WIN(10, 20));
    w16(WIN0V, WIN(15, 18));
    w16(WININ, 0x0001);
    w16(WINOUT, 0x0000);
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 20; y++) {
        const bool rows = y >= 15 && y < 18;
        render(y);
        PX(9, out15(C_BACK));
        PX(10, out15(rows ? bg_color(0) : C_BACK));
        PX(19, out15(rows ? bg_color(0) : C_BACK));
        PX(20, out15(C_BACK));
    }
    /* Window 1 has the other half of each register. */
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_WIN1);
    w16(WIN0H, 0);
    w16(WIN0V, 0);
    w16(WIN0H + 2, WIN(30, 40));
    w16(WIN0V + 2, WIN(15, 18));
    w16(WININ, 0x0200);
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 20; y++) {
        const bool rows = y >= 15 && y < 18;
        render(y);
        PX(29, out15(C_BACK));
        PX(30, out15(rows ? bg_color(1) : C_BACK));
        PX(39, out15(rows ? bg_color(1) : C_BACK));
        PX(40, out15(C_BACK));
    }
    /* A window that is not enabled in DISPCNT is not there. */
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_WIN0);
    frame_line(16);
    PX(35, out15(C_BACK));

    /* Where they overlap, window 0 wins; the OBJ window comes after both;
     * the rest is outside. The OBJ window is the non-transparent pixels of
     * OBJ-window sprites, which are not displayed themselves. */
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_OBJ | DISP_OBJ_1D | DISP_WIN0 | DISP_WIN1 | DISP_OBJWIN);
    w16(WIN0H, WIN(10, 20));
    w16(WIN0V, WIN(20, 30));
    w16(WIN0H + 2, WIN(15, 44));
    w16(WIN0V + 2, WIN(20, 30));
    obj_set(4, 20u | A0_WINDOW, 40, 0); /* 8x8 at (40, 20) */
    w16(WININ, 0x0201);  /* window 0: BG0. Window 1: BG1 */
    w16(WINOUT, 0x0408); /* OBJ window: BG2. Outside: BG3 */
    frame_line(22);
    PX(12, out15(bg_color(0))); /* window 0 */
    PX(17, out15(bg_color(0))); /* both: window 0 */
    PX(25, out15(bg_color(1))); /* window 1 */
    PX(42, out15(bg_color(1))); /* window 1 and the OBJ window: window 1 */
    PX(45, out15(bg_color(2))); /* the OBJ window */
    PX(48, out15(bg_color(3))); /* outside */
    PX(5, out15(bg_color(3)));
    frame_line(30); /* below all three */
    PX(12, out15(bg_color(3)));
    PX(45, out15(bg_color(3)));
    /* "Both DISPCNT Bits 12 and 15 must be set when defining OBJ Window
     * region(s)." */
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_OBJ_1D | DISP_WIN0 | DISP_WIN1 | DISP_OBJWIN);
    frame_line(22);
    PX(45, out15(bg_color(3)));
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_OBJ | DISP_OBJ_1D | DISP_WIN0 | DISP_WIN1);
    frame_line(22);
    PX(45, out15(bg_color(3)));
    /* The OBJ window alone enables the outside region as well ("if any of
     * these regions is enabled then the Outside of Windows region is
     * automatically enabled, too"). */
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_OBJ | DISP_OBJ_1D | DISP_OBJWIN);
    frame_line(22);
    PX(45, out15(bg_color(2)));
    PX(12, out15(bg_color(3)));
    /* With no window enabled, WININ and WINOUT mean nothing. */
    w16(WININ, 0);
    w16(WINOUT, 0);
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_OBJ | DISP_OBJ_1D);
    frame_line(22);
    PX(12, out15(bg_color(0)));

    /*
     * Each of the six bits of each of the four regions. A sprite sits at
     * each region's test pixel, and brightness increase at full strength is
     * selected for every layer, which turns a pixel white wherever the
     * effect bit lets it.
     */
    g_section = "window enable bits";
    static const int k_at[4] = { 12, 32, 62, 100 }; /* in window 0, window 1, the OBJ window, outside */
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_OBJ | DISP_OBJ_1D | DISP_WIN0 | DISP_WIN1 | DISP_OBJWIN);
    w16(WIN0H, WIN(10, 20));
    w16(WIN0V, WIN(20, 30));
    w16(WIN0H + 2, WIN(30, 40));
    w16(WIN0V + 2, WIN(20, 30));
    obj_set(0, 20, 10, 0);
    obj_set(1, 20, 30, 0);
    obj_set(2, 20, 60, 0);
    obj_set(3, 20, 98, 0);
    obj_set(4, 20u | A0_WINDOW, 60, 0);
    w16(BLDCNT, 0x00BF); /* brightness increase, every layer a 1st target */
    w16(BLDY, 16);
    for (uint32_t region = 0; region < 4; region++) {
        for (uint32_t bit = 0; bit < 6; bit++) {
            static const uint8_t k_shift[4] = { 0, 8, 24, 16 }; /* in WININ (low word) and WINOUT (high word) */
            w32(WININ, (1u << bit) << k_shift[region]);
            frame_line(22);
            for (uint32_t other = 0; other < 4; other++) {
                uint32_t want = out15(C_BACK); /* nothing is enabled in the other regions */
                if (other == region) {
                    want = bit < 4 ? out15(bg_color(bit)) : bit == 4 ? out15(C_D) : WHITE18;
                }
                PX(k_at[other], want);
            }
        }
    }
    /* All six bits on, everywhere: the sprite, white. */
    w32(WININ, 0x3F3F3F3Fu);
    frame_line(22);
    PX(12, WHITE18);
    PX(100, WHITE18);
    w16(BLDCNT, 0);
    for (uint32_t n = 0; n < 5; n++) obj_set(n, A0_HIDE, 0, 0);

    /*
     * The horizontal edges (GBATEK "DS Video Stuff", DS Window Glitches):
     * "X1=00h is treated as 0 (left-most), X2=00h is treated as 100h
     * (right-most). However, the window is not displayed if X1=X2=00h; the
     * window width can be max 255 pixels." The window is a flip-flop, set
     * at X1 and cleared at X2 (melonDS), so with X1 > X2 it is the two ends
     * of the line.
     */
    g_section = "window edges";
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_WIN0);
    w16(WININ, 0x0001);
    w16(WINOUT, 0x0000);
    w16(WIN0V, WIN(0, 192));
#define INSIDE out15(bg_color(0))
#define OUTSIDE out15(C_BACK)
    w16(WIN0H, WIN(100, 0));
    frame_line(6);
    PX(0, OUTSIDE);
    PX(99, OUTSIDE);
    PX(100, INSIDE);
    PX(255, INSIDE);
    w16(WIN0H, WIN(0, 0));
    frame_line(6);
    PX(0, OUTSIDE);
    PX(128, OUTSIDE);
    PX(255, OUTSIDE);
    w16(WIN0H, WIN(50, 50));
    frame_line(6);
    PX(49, OUTSIDE);
    PX(50, OUTSIDE);
    PX(51, OUTSIDE);
    w16(WIN0H, WIN(0, 255));
    frame_line(6);
    PX(0, INSIDE);
    PX(254, INSIDE);
    PX(255, OUTSIDE);
    w16(WIN0H, WIN(0, 1));
    frame_line(6);
    PX(0, INSIDE);
    PX(1, OUTSIDE);
    w16(WIN0H, WIN(255, 0));
    frame_line(6);
    PX(254, OUTSIDE);
    PX(255, INSIDE);
    w16(WIN0H, WIN(200, 50));
    frame_line(6);
    PX(0, INSIDE);
    PX(49, INSIDE);
    PX(50, OUTSIDE);
    PX(199, OUTSIDE);
    PX(200, INSIDE);
    PX(255, INSIDE);
    /* The flip-flop is not reset between lines: set at the end of a line
     * with X1 > X2, it stays set into the next line until that line's X2,
     * whatever X1 has become (melonDS). */
    w16(WIN0H, WIN(100, 150));
    render(7);
    PX(0, INSIDE);
    PX(99, INSIDE);
    PX(149, INSIDE);
    PX(150, OUTSIDE);
    render(8);
    PX(0, OUTSIDE);
    PX(99, OUTSIDE);
    PX(100, INSIDE);
    PX(149, INSIDE);
    PX(150, OUTSIDE);
    /* A line the engine is not asked to render happens all the same: line
     * 20 is drawn with X1 > X2, line 21 (not rendered) has X1 < X2 and
     * clears the flip-flop, so line 22 starts clear. */
    w16(WIN0H, WIN(200, 50));
    frame_line(20);
    w16(WIN0H, WIN(100, 150));
    render(22);
    PX(0, OUTSIDE);
    PX(100, INSIDE);
    /* And the vertical comparisons of lines not rendered are made. */
    w16(WIN0H, WIN(0, 255));
    w16(WIN0V, WIN(10, 20));
    frame_line(191);
    nds_2d_begin_frame(&g_e);
    render(15);
    PX(10, INSIDE);
    render(25);
    PX(10, OUTSIDE);
    w16(WIN0V, WIN(0, 192));

    /*
     * The vertical edges. The flip-flop is set on the line that equals Y1
     * and cleared on the line that equals Y2, on every line of the frame,
     * comparing only eight bits of the line number: "Y1 coordinates 00h..06h
     * will be triggered in scanlines 100h-106h by mistake. That means, the
     * window gets activated within VBlank period, and will be active in
     * scanline 0 and up (that is no problem with Y1=0, but Y1=1..6 will
     * appear as if Y1 would be 0)."
     */
    w16(WIN0H, WIN(0, 255));
    static const struct {
        uint8_t y1, y2;
        uint8_t line[6];
        uint8_t inside[6];
    } k_rows[] = {
        { 50, 50, { 0, 49, 50, 51, 191, 0 }, { 0, 0, 0, 0, 0, 0 } },       /* Y1 = Y2: clearing wins */
        { 50, 200, { 0, 49, 50, 100, 191, 0 }, { 0, 0, 1, 1, 1, 0 } },     /* Y2 below the screen */
        { 0, 192, { 0, 1, 100, 190, 191, 0 }, { 1, 1, 1, 1, 1, 1 } },      /* the whole screen */
        { 0, 0, { 0, 1, 100, 191, 0, 0 }, { 0, 0, 0, 0, 0, 0 } },
        { 100, 50, { 0, 49, 50, 99, 100, 191 }, { 1, 1, 0, 0, 1, 1 } },    /* Y1 > Y2: it wraps through VBlank */
        { 3, 100, { 0, 2, 3, 99, 100, 191 }, { 1, 1, 1, 1, 0, 0 } },       /* set on line 259 = 0x103 */
        { 6, 100, { 0, 5, 6, 99, 100, 191 }, { 1, 1, 1, 1, 0, 0 } },       /* set on line 262 = 0x106 */
        { 7, 100, { 0, 6, 7, 99, 100, 191 }, { 0, 0, 1, 1, 0, 0 } },       /* there is no line 263 */
        { 100, 3, { 0, 2, 3, 99, 100, 191 }, { 0, 0, 0, 0, 1, 1 } },       /* cleared on line 259: no wrap */
        { 100, 7, { 0, 6, 7, 99, 100, 191 }, { 1, 1, 0, 0, 1, 1 } },
    };
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0]; i++) {
        w16(WIN0V, WIN(k_rows[i].y1, k_rows[i].y2));
        /* Two frames: the first leaves the flip-flop as a frame with these
         * values leaves it. */
        for (int frame = 0; frame < 2; frame++) {
            nds_2d_begin_frame(&g_e);
            for (int y = 0; y < 192; y++) {
                render(y);
                for (int k = 0; k < 6; k++) {
                    if (frame == 1 && k_rows[i].line[k] == y) PX(10, k_rows[i].inside[k] ? INSIDE : OUTSIDE);
                }
            }
            nds_2d_end_frame(&g_e);
        }
    }

    /*
     * Y1 moved while the frame is drawn (what the window-midframe test of
     * roms/nds/striker-tests shows on hardware): the comparison is made
     * line by line, so a Y1 that is moved to a line already passed never
     * sets the flip-flop, and one that has set it does not need to stay.
     */
    w16(WIN0V, WIN(140, 141));
    frame_line(191);
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 192; y++) {
        render(y);
        if (y == 80) {
            PX(10, OUTSIDE);
            w8(WIN0V + 1, 70); /* Y1 = 70: gone by already */
        }
        if (y == 90 || y == 100) PX(10, OUTSIDE);
        if (y == 100) w8(WIN0V + 1, 101);
        if (y == 101) {
            PX(10, INSIDE);
            w8(WIN0V + 1, 110); /* stays set without it */
        }
        if (y == 102 || y == 109 || y == 110 || y == 140) PX(10, INSIDE);
        if (y == 141 || y == 191) PX(10, OUTSIDE);
    }

    /* Lines 192-262 compare too. Told about them one by one, the engine
     * uses the registers as they are on each; not told, it catches up at
     * the start of the next frame with the registers as they are then. */
    for (int told = 0; told < 2; told++) {
        w16(WIN0V, WIN(10, 150));
        frame_line(191);
        nds_2d_end_frame(&g_e);
        w8(WIN0V + 1, 200); /* Y1 = 200 from line 192 on */
        if (told) {
            for (int line = 192; line <= 200; line++) nds_2d_blank_line(&g_e, line);
        }
        w8(WIN0V + 1, 10); /* and back to 10 from line 201 on */
        if (told) {
            for (int line = 201; line <= 262; line++) nds_2d_blank_line(&g_e, line);
        }
        nds_2d_begin_frame(&g_e);
        render(0);
        PX(10, told ? INSIDE : OUTSIDE);
        for (int y = 1; y <= 150; y++) render(y);
        PX(10, OUTSIDE); /* line 150 */
    }
    /* Only lines 192-262 are told that way. */
    Nds2d before = g_e;
    nds_2d_blank_line(&g_e, 160);
    nds_2d_blank_line(&g_e, 263);
    nds_2d_blank_line(&g_e, -1);
    EQ("not a blank line", memcmp(&before, &g_e, sizeof g_e), 0);
#undef INSIDE
#undef OUTSIDE
}

/* ------------------------------------------------------------------ */
/* Colour special effects                                              */
/* ------------------------------------------------------------------ */

static uint32_t channel(uint32_t color, int i)
{
    return (color >> (6 * i)) & 63u;
}

/* GBATEK "LCD I/O Color Special Effects": "I = MIN ( 31, I1st*EVA +
 * I2nd*EVB )" with the coefficients in sixteenths, "0..16 = 0/16..16/16,
 * 17..31=16/16". On the DS (melonDS) the intensities are the 6-bit ones,
 * the limit is 63, and the division rounds to nearest. */
static uint32_t ref_alpha(uint32_t first, uint32_t second, uint32_t eva, uint32_t evb)
{
    uint32_t out = 0;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    for (int i = 0; i < 3; i++) {
        uint32_t v = (channel(first, i) * eva + channel(second, i) * evb + 8u) / 16u;
        if (v > 63) v = 63;
        out |= v << (6 * i);
    }
    return out;
}

/* "I = I1st + (31-I1st)*EVY" and "I = I1st - (I1st)*EVY", likewise on 6
 * bits: the step of an increase is rounded to nearest (+8), the step of a
 * decrease with +7 (melonDS). Master brightness is the same pair with the
 * steps rounded down (+0) and up (+15). */
static uint32_t ref_brighter(uint32_t color, uint32_t factor, uint32_t bias)
{
    uint32_t out = 0;
    if (factor > 16) factor = 16;
    for (int i = 0; i < 3; i++) {
        const uint32_t c = channel(color, i);
        out |= (c + ((63u - c) * factor + bias) / 16u) << (6 * i);
    }
    return out;
}

static uint32_t ref_darker(uint32_t color, uint32_t factor, uint32_t bias)
{
    uint32_t out = 0;
    if (factor > 16) factor = 16;
    for (int i = 0; i < 3; i++) {
        const uint32_t c = channel(color, i);
        out |= (c - (c * factor + bias) / 16u) << (6 * i);
    }
    return out;
}

#define COL_BG0 RGB15(31, 0, 16)
#define COL_BG1 RGB15(0, 31, 16)
#define COL_BG2 RGB15(10, 20, 30)
#define COL_BG3 RGB15(7, 3, 25)
#define COL_OBJ RGB15(20, 5, 9)
#define COL_BD RGB15(4, 12, 28)

/* Four BGs that fill line 0, BG n at priority n, and an 8x8 sprite at the
 * left end at priority 0. */
static void blend_scene(int engine)
{
    static const uint32_t k_color[4] = { COL_BG0, COL_BG1, COL_BG2, COL_BG3 };
    reset_scene(engine);
    pal_bg(0, COL_BD);
    for (uint32_t bg = 0; bg < 4; bg++) {
        pal_bg(bg + 1, k_color[bg]);
        solid4(g_bg, (bg + 1) * 32, bg + 1);
        for (uint32_t i = 0; i < 32; i++) put16(g_bg, 0x800 + bg * 0x800 + i * 2, bg + 1);
        w16(BG0CNT + 2 * bg, ((1u + bg) << 8) | bg);
    }
    pal_obj(1, COL_OBJ);
    solid4(g_obj, 0, 1);
    obj_set(0, 0, 0, 0);
}

static void test_blending(void)
{
    static const uint32_t k_layer[6] = { COL_BG0, COL_BG1, COL_BG2, COL_BG3, COL_OBJ, COL_BD };
    const uint32_t top = out15(COL_BG0), under = out15(COL_BG1); /* (62, 0, 32) and (0, 62, 32) */

    /* BG0 over BG1, worked by hand. */
    g_section = "alpha blending";
    blend_scene(0);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1));
    w16(BLDCNT, 0x0241); /* 1st: BG0. Alpha blending. 2nd: BG1 */
    static const struct {
        uint32_t eva, evb, r, g, b;
    } k_alpha[] = {
        { 8, 8, 31, 31, 32 },    /* (496+8)/16, (496+8)/16, (256+256+8)/16 */
        { 16, 16, 62, 62, 63 },  /* (992+8)/16 = 62, and (512+512+8)/16 = 64, limited to 63 */
        { 1, 0, 4, 0, 2 },       /* (62+8)/16, 8/16, (32+8)/16 */
        { 0, 0, 0, 0, 0 },
        { 16, 0, 62, 0, 32 },
        { 31, 0, 62, 0, 32 },    /* 17..31 = 16/16 */
        { 0, 31, 0, 62, 32 },
        { 3, 5, 12, 19, 16 },    /* (186+8)/16, (310+8)/16, (96+160+8)/16 */
    };
    for (size_t i = 0; i < sizeof k_alpha / sizeof k_alpha[0]; i++) {
        w16(BLDALPHA, k_alpha[i].eva | (k_alpha[i].evb << 8));
        frame_line(0);
        PX(20, RGB18(k_alpha[i].r, k_alpha[i].g, k_alpha[i].b));
    }
    /* Every pair of coefficients. */
    for (uint32_t eva = 0; eva < 32; eva++) {
        for (uint32_t evb = 0; evb < 32; evb++) {
            w16(BLDALPHA, eva | (evb << 8));
            frame_line(0);
            PX(20, ref_alpha(top, under, eva, evb));
        }
    }

    /* "6-7 Color Special Effect: 0 = None, 1 = Alpha Blending, 2 =
     * Brightness Increase, 3 = Brightness Decrease", by hand for (62, 0, 32). */
    g_section = "brightness effects";
    w16(BLDALPHA, 0x0808);
    static const struct {
        uint32_t effect, evy, r, g, b;
    } k_effect[] = {
        { 0, 8, 62, 0, 32 },
        { 1, 8, 31, 31, 32 },
        { 2, 8, 63, 32, 48 },   /* 62 + (1*8+8)/16, 0 + (63*8+8)/16, 32 + (31*8+8)/16 */
        { 3, 8, 31, 0, 16 },    /* 62 - (62*8+7)/16, 0, 32 - (32*8+7)/16 */
        { 2, 16, 63, 63, 63 },
        { 3, 16, 0, 0, 0 },
        { 2, 1, 62, 4, 34 },    /* 62 + 9/16, 0 + 71/16, 32 + 39/16 */
        { 3, 1, 58, 0, 30 },    /* 62 - 69/16, 0 - 7/16, 32 - 39/16 */
        { 2, 0, 62, 0, 32 },
        { 3, 0, 62, 0, 32 },
        { 2, 31, 63, 63, 63 },  /* 17..31 = 16/16 */
        { 3, 17, 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof k_effect / sizeof k_effect[0]; i++) {
        w16(BLDCNT, 0x0201u | (k_effect[i].effect << 6));
        w16(BLDY, k_effect[i].evy);
        frame_line(0);
        PX(20, RGB18(k_effect[i].r, k_effect[i].g, k_effect[i].b));
    }
    for (uint32_t evy = 0; evy < 32; evy++) {
        w16(BLDY, evy);
        w16(BLDCNT, 0x0081);
        frame_line(0);
        PX(20, ref_brighter(top, evy, 8));
        w16(BLDCNT, 0x00C1);
        frame_line(0);
        PX(20, ref_darker(top, evy, 7));
    }

    /* "0-5 1st Target Pixel": each layer on top, each target bit. Full
     * brightness increase turns the pixel white if it is a 1st target. */
    g_section = "1st targets";
    w16(BLDY, 16);
    for (uint32_t layer = 0; layer < 6; layer++) {
        /* Show just that layer: one BG, or the sprite, or nothing at all. */
        w32(DISPCNT, DISP_ON | DISP_OBJ_1D | (layer < 4 ? DISP_BG(layer) : layer == 4 ? DISP_OBJ : 0u));
        for (uint32_t bit = 0; bit < 6; bit++) {
            w16(BLDCNT, 0x0080u | (1u << bit));
            frame_line(0);
            PX(4, bit == layer ? WHITE18 : out15(k_layer[layer]));
        }
    }

    /* "8-13 2nd Target Pixel": each layer under another, each target bit. */
    g_section = "2nd targets";
    w16(BLDALPHA, 0x0808);
    for (uint32_t layer = 0; layer < 6; layer++) {
        uint32_t over = 0; /* the layer on top: BG0, or BG1 when BG0 is the one underneath */
        if (layer == 0) {
            w16(BG0CNT, (1u << 8) | 1u);
            w16(BG1CNT, (2u << 8) | 0u);
            w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1));
            over = 1;
        } else if (layer < 4) {
            w16(BG0CNT, (1u << 8) | 0u);
            w16(BG1CNT, (2u << 8) | 1u);
            w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(layer));
        } else if (layer == 4) {
            obj_set(0, 0, 0, 1u << 10); /* the sprite behind BG0 */
            w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_OBJ | DISP_OBJ_1D);
        } else {
            w32(DISPCNT, DISP_ON | DISP_BG(0));
        }
        for (uint32_t bit = 0; bit < 6; bit++) {
            w16(BLDCNT, 0x0040u | (1u << over) | (0x100u << bit));
            frame_line(0);
            PX(4, bit == layer ? ref_alpha(out15(k_layer[over]), out15(k_layer[layer]), 8, 8) : out15(k_layer[over]));
        }
    }
    obj_set(0, 0, 0, 0);
    w16(BG0CNT, (1u << 8) | 0u);
    w16(BG1CNT, (2u << 8) | 1u);

    /* "if a non-transparent non-2nd-target pixel is moved between the two
     * targets, or if 2nd target has higher display priority than 1st target
     * - then only the top-most pixel is displayed". */
    g_section = "blending rules";
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1) | DISP_BG(2));
    w16(BLDCNT, 0x0441); /* 1st: BG0. 2nd: BG2. BG1 lies between */
    frame_line(0);
    PX(20, top);
    w16(BLDCNT, 0x0142); /* 1st: BG1. 2nd: BG0, which is in front */
    frame_line(0);
    PX(20, top);
    /* "all layers may be selected as both 1st+2nd target, in that case the
     * top-most pixel will be used as 1st target, and the next lower pixel as
     * 2nd target." */
    w16(BLDCNT, 0x3F7F);
    frame_line(0);
    PX(20, ref_alpha(top, under, 8, 8));
    w32(DISPCNT, DISP_ON | 0xF00u | DISP_OBJ | DISP_OBJ_1D);
    frame_line(0);
    PX(4, ref_alpha(out15(COL_OBJ), top, 8, 8));

    /* Which layer is "the next lower pixel" when a sprite is among them: at
     * the priority of BG1 the sprite is in front of BG1, so under BG0 it is
     * the sprite; one priority lower it is BG1. */
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1) | DISP_OBJ | DISP_OBJ_1D);
    obj_set(0, 0, 0, 1u << 10);
    w16(BLDCNT, 0x1041); /* 1st: BG0. 2nd: OBJ */
    frame_line(0);
    PX(4, ref_alpha(top, out15(COL_OBJ), 8, 8));
    PX(20, top);
    w16(BLDCNT, 0x0241); /* 1st: BG0. 2nd: BG1 */
    frame_line(0);
    PX(4, top);
    PX(20, ref_alpha(top, under, 8, 8));
    obj_set(0, 0, 0, 2u << 10);
    frame_line(0);
    PX(4, ref_alpha(top, under, 8, 8));
    w16(BLDCNT, 0x1041);
    frame_line(0);
    PX(4, top);
    obj_set(0, 0, 0, 0);

    /* A sprite, not semi-transparent, as 1st target of alpha blending. */
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_OBJ | DISP_OBJ_1D);
    w16(BLDCNT, 0x0150);
    frame_line(0);
    PX(4, ref_alpha(out15(COL_OBJ), top, 8, 8));
    PX(20, top);
    w16(BLDCNT, 0x0140);
    frame_line(0);
    PX(4, out15(COL_OBJ));

    /*
     * "OBJs that are defined as 'Semi-Transparent' in OAM memory are always
     * selected as 1st Target (regardless of BLDCNT Bit 4), and are always
     * using Alpha Blending mode (regardless of BLDCNT Bit 6-7)."
     */
    g_section = "semi-transparent sprites";
    obj_set(0, 0u | A0_SEMI, 0, 0);
    w16(BLDALPHA, 0x0C06); /* EVA 6, EVB 12 */
    const uint32_t semi = ref_alpha(out15(COL_OBJ), top, 6, 12);
    /* (40*6+62*12+8)/16 = 62, (10*6+0+8)/16 = 4, (18*6+32*12+8)/16 = 31 */
    EQ("semi-transparent by hand", semi, RGB18(62, 4, 31));
    for (uint32_t effect = 0; effect < 4; effect++) {
        w16(BLDCNT, 0x0100u | (effect << 6)); /* 2nd: BG0. No 1st target at all */
        frame_line(0);
        PX(4, semi);
        PX(20, top);
    }
    /* Over something that is no 2nd target it is an ordinary sprite... */
    w16(BLDCNT, 0x0040);
    frame_line(0);
    PX(4, out15(COL_OBJ));
    /* ...to which "the BLDCNT register may be used to perform Brightness
     * effects". */
    w16(BLDY, 16);
    w16(BLDCNT, 0x0090);
    frame_line(0);
    PX(4, WHITE18);
    /* "However, if a semi-transparent OBJ pixel does overlap a 2nd target
     * pixel, then semi-transparency becomes priority, and the brightness
     * effect will not take place". */
    w16(BLDCNT, 0x0190);
    frame_line(0);
    PX(4, semi);
    /* A window that switches the effects off does not switch this off
     * (melonDS), though it does switch off BG0 over BG1 next to it. */
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1) | DISP_OBJ | DISP_OBJ_1D | DISP_WIN0);
    w16(WIN0H, WIN(0, 255));
    w16(WIN0V, WIN(0, 192));
    w16(WININ, 0x001F);
    w16(BLDCNT, 0x0341); /* 1st: BG0. Alpha. 2nd: BG0, BG1 */
    frame_line(0);
    PX(4, semi);
    PX(20, top);
    w16(WININ, 0x003F);
    frame_line(0);
    PX(4, semi);
    PX(20, ref_alpha(top, under, 6, 12));
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1) | DISP_OBJ | DISP_OBJ_1D);

    /* A semi-transparent sprite behind a BG is the BG's 2nd target like any
     * other sprite. */
    obj_set(0, 0u | A0_SEMI, 0, 1u << 10);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_OBJ | DISP_OBJ_1D);
    w16(BLDCNT, 0x1041);
    frame_line(0);
    PX(4, ref_alpha(top, out15(COL_OBJ), 6, 12));

    /* The backdrop alone, 1st and 2nd target at once: blended with itself,
     * as on the GBA (this project's GBA renderer and melonDS agree). */
    w32(DISPCNT, DISP_ON);
    w16(BLDCNT, 0x2060);
    w16(BLDALPHA, 0x0808);
    frame_line(0);
    PX(20, ref_alpha(out15(COL_BD), out15(COL_BD), 8, 8));

    /* Engine B blends the same way. */
    blend_scene(1);
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1));
    w16(BLDCNT, 0x0241);
    w16(BLDALPHA, 0x0808);
    frame_line(0);
    PX(20, RGB18(31, 31, 32));
}

/* ------------------------------------------------------------------ */
/* The 3D layer                                                        */
/* ------------------------------------------------------------------ */

#define PIX3D(r, g, b, a) ((uint32_t)((r) | ((g) << 6) | ((b) << 12) | ((uint32_t)(a) << 18)))

/* GBATEK "DS 3D Final 2D Output": "The final 3D image [...] is passed to 2D
 * Engine A as BG0 layer (provided that DISPCNT is configured to use 3D as
 * BG0)". */
static void test_3d_layer(void)
{
    g_section = "3D layer";
    blend_scene(0);
    obj_set(0, A0_HIDE, 0, 0);
    /* Pixel 0: opaque, with odd intensities no 15-bit colour has. Pixel 1:
     * a colour but alpha 0, so nothing. Pixels 2-33: one colour at alpha
     * 0-31. Pixels 34-38: nothing. Pixel 39: that colour, opaque. From 40
     * on: opaque, each its own colour. */
    g_3d_line[0] = PIX3D(63, 1, 33, 31);
    g_3d_line[1] = PIX3D(40, 20, 10, 0);
    for (uint32_t a = 0; a < 32; a++) g_3d_line[2 + a] = PIX3D(40, 20, 10, a);
    g_3d_line[39] = PIX3D(40, 20, 10, 31);
    for (uint32_t x = 40; x < 256; x++) g_3d_line[x] = PIX3D(x & 63, 63 - (x & 63), x >> 2, 31);
    g_3d = g_3d_line;
    const uint32_t bg1 = out15(COL_BG1), color = RGB18(40, 20, 10);

    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(0) | DISP_BG(1));
    frame_line(0);
    PX(0, RGB18(63, 1, 33));
    PX(1, bg1);       /* alpha 0: BG1 shows through */
    PX(2, bg1);
    PX(3, color);     /* with no 2nd target under them, translucent pixels are plain */
    PX(17, color);
    PX(33, color);
    PX(40, RGB18(40, 23, 10));
    PX(255, RGB18(63, 0, 63));

    /* "Blending with BG0 as 1st Target via 3D Alpha-values (unlike as for
     * 2D)". Over a 2nd target a 3D pixel of alpha A is mixed with it A+1 to
     * 31-A (melonDS), whatever BLDCNT's effect and 1st targets are:
     *   I = (I1st*(A+1) + I2nd*(31-A) + 16) / 32
     * (40, 20, 10) over (0, 62, 32) by hand: */
    static const struct {
        uint32_t alpha, r, g, b;
    } k_mix[] = {
        { 15, 20, 41, 21 }, /* (640+16)/32, (320+992+16)/32, (160+512+16)/32 */
        { 1, 3, 59, 31 },   /* (80+16)/32, (40+1860+16)/32, (20+960+16)/32 */
        { 30, 39, 21, 11 }, /* (1240+16)/32, (620+62+16)/32, (310+32+16)/32 */
        { 31, 40, 20, 10 },
    };
    for (uint32_t effect = 0; effect < 4; effect++) {
        w16(BLDCNT, 0x0200u | (effect << 6)); /* 2nd: BG1 */
        frame_line(0);
        for (size_t i = 0; i < sizeof k_mix / sizeof k_mix[0]; i++) {
            PX(2 + (int)k_mix[i].alpha, RGB18(k_mix[i].r, k_mix[i].g, k_mix[i].b));
        }
        for (uint32_t a = 1; a < 32; a++) {
            uint32_t want = 0;
            for (int i = 0; i < 3; i++) {
                want |= ((channel(color, i) * (a + 1) + channel(bg1, i) * (31 - a) + 16u) / 32u) << (6 * i);
            }
            PX(2 + (int)a, want);
        }
        PX(0, RGB18(63, 1, 33)); /* opaque: itself */
        PX(1, bg1);
        PX(2, bg1);
    }
    /* Over the backdrop. */
    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(0));
    w16(BLDCNT, 0x2000);
    frame_line(0);
    /* (40*16+8*16+16)/32, (20*16+24*16+16)/32, (10*16+56*16+16)/32 */
    PX(17, RGB18(24, 22, 33));
    PX(1, out15(COL_BD));
    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(0) | DISP_BG(1));

    /* "If the 3D screen has highest priority, then alpha-blending is always
     * enabled, regardless of the Window Control register's color effect
     * enable flag". */
    w16(BLDCNT, 0x0200);
    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(0) | DISP_BG(1) | DISP_WIN0);
    w16(WIN0H, WIN(0, 255));
    w16(WIN0V, WIN(0, 192));
    w16(WININ, 0x0003); /* BG0 and BG1, no effects */
    frame_line(0);
    PX(17, RGB18(20, 41, 21));
    /* The window's BG0 bit is the 3D layer's. */
    w16(WININ, 0x0002);
    frame_line(0);
    PX(0, bg1);
    PX(17, bg1);
    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(0) | DISP_BG(1));

    /* "Brightness up/down with BG0 as 1st Target via EVY (as for 2D)", where
     * nothing under the pixel is a 2nd target; where something is, the
     * mixing above happens instead (melonDS), which leaves an opaque pixel
     * as it is. */
    w16(BLDY, 16);
    w16(BLDCNT, 0x0081);
    frame_line(0);
    PX(0, WHITE18);
    PX(17, WHITE18);
    PX(1, bg1);
    w16(BLDCNT, 0x00C1);
    frame_line(0);
    PX(0, 0);
    w16(BLDCNT, 0x0281);
    frame_line(0);
    PX(0, RGB18(63, 1, 33));
    PX(17, RGB18(20, 41, 21));

    /* "Blending with BG0 as 2nd Target via EVA/EVB (as for 2D)": BG1 in
     * front of the 3D layer. The 3D pixel's own alpha plays no part. */
    w16(BG0CNT, 1);
    w16(BG1CNT, (2u << 8) | 0u);
    w16(BLDCNT, 0x0142);
    w16(BLDALPHA, 0x0808);
    frame_line(0);
    PX(0, RGB18(32, 32, 33));  /* (0+63*8+8)/16, (62*8+1*8+8)/16, (32*8+33*8+8)/16 */
    PX(17, RGB18(20, 41, 21)); /* (0+40*8+8)/16, (62*8+20*8+8)/16, (32*8+10*8+8)/16 */
    PX(1, bg1);                /* nothing of the 3D layer there: BG1 over the backdrop */
    w16(BG0CNT, 1u << 8);
    w16(BG1CNT, (2u << 8) | 1u);

    /* Under a semi-transparent sprite. */
    obj_set(0, 0u | A0_SEMI, 0, 0);
    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(0) | DISP_OBJ | DISP_OBJ_1D);
    w16(BLDCNT, 0x0100);
    frame_line(0);
    PX(0, ref_alpha(out15(COL_OBJ), RGB18(63, 1, 33), 8, 8));
    obj_set(0, A0_HIDE, 0, 0);
    w16(BLDCNT, 0);
    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(0) | DISP_BG(1));

    /* "The lower 2bit of the BG0CNT register control the priority relative
     * to other BGs and OBJs [...] All other bits in BG0CNT have no effect on
     * 3D, namely, mosaic cannot be used on the 3D layer." */
    for (uint32_t prio = 0; prio < 4; prio++) {
        w16(BG0CNT, prio);
        frame_line(0);
        PX(0, prio <= 1 ? RGB18(63, 1, 33) : bg1); /* BG1 is at priority 1; BG0 wins the tie */
    }
    w16(BG0CNT, 0xFFFC);
    w16(MOSAIC, 0xFFFF);
    w16(BG0VOFS, 100); /* "Vertical scrolling [...] cannot be used on the 3D layer" */
    frame_line(0);
    PX(40, RGB18(40, 23, 10));
    PX(41, RGB18(41, 22, 10));
    PX(42, RGB18(42, 21, 10));
    w16(BG0CNT, 1u << 8);
    w16(MOSAIC, 0);
    w16(BG0VOFS, 0);

    /* "The BG0HOFS register can be used the scroll the 3D layer
     * horizontally, the scroll region is 512 pixels, consisting of 256
     * pixels for the 3D image, followed by 256 transparent pixels, and then
     * wrapped to the 3D image again." */
    w16(BG0HOFS, 40);
    frame_line(0);
    PX(0, RGB18(40, 23, 10));  /* pixel 40 of the 3D line */
    PX(215, RGB18(63, 0, 63)); /* pixel 255 */
    PX(216, bg1);              /* 256: transparent */
    PX(255, bg1);
    w16(BG0HOFS, 256 + 40);
    frame_line(0);
    PX(0, bg1);
    PX(215, bg1);              /* 511 */
    PX(216, RGB18(63, 1, 33)); /* 512 = pixel 0 again */
    PX(254, bg1);              /* pixel 38: nothing */
    PX(255, color);            /* pixel 39 */
    w16(BG0HOFS, 511);
    frame_line(0);
    PX(0, bg1);
    PX(1, RGB18(63, 1, 33));
    w16(BG0HOFS, 0);

    /* Bits above the alpha are not part of the pixel. */
    g_3d_line[0] |= 0xFF800000u;
    frame_line(0);
    PX(0, RGB18(63, 1, 33));
    g_3d_line[0] = PIX3D(63, 1, 33, 31);

    /* Without a 3D line BG0 has nothing; without DISPCNT bit 3 it is the
     * text BG; without its enable bit it is not there. */
    g_3d = NULL;
    frame_line(0);
    PX(0, bg1);
    g_3d = g_3d_line;
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1));
    frame_line(0);
    PX(0, out15(COL_BG0));
    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(1));
    frame_line(0);
    PX(0, bg1);

    /* BG mode 6: "3D | - | Large | -". */
    w32(DISPCNT, DISP_ON | 6u | DISP_3D | DISP_BG(0));
    frame_line(0);
    PX(0, RGB18(63, 1, 33));
    PX(1, out15(COL_BD));

    /* "Engine B: [...] BG0 is always Text (no 3D support)." */
    blend_scene(1);
    g_3d = g_3d_line;
    w32(DISPCNT, DISP_ON | DISP_3D | DISP_BG(0));
    EQ("engine B has no bit 3", r32(DISPCNT) & DISP_3D, 0);
    frame_line(0);
    PX(0, out15(COL_BG0));
    g_3d = NULL;
}

/* ------------------------------------------------------------------ */
/* Master brightness                                                   */
/* ------------------------------------------------------------------ */

/* GBATEK "DS Video Stuff", MASTER_BRIGHT: "0-4 Factor used for 6bit R,G,B
 * Intensities (0-16, values >16 same as 16). Brightness up: New = Old +
 * (63-Old) * Factor/16. Brightness down: New = Old - Old * Factor/16.
 * 14-15 Mode (0=Disable, 1=Up, 2=Down, 3=Reserved)". */
static void test_master_brightness(void)
{
    g_section = "master brightness";
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        pal_bg(0, RGB15(31, 16, 0)); /* (62, 32, 0) */
        w32(DISPCNT, DISP_ON);
        static const struct {
            uint32_t value, r, g, b;
        } k_cases[] = {
            { 0x0000, 62, 32, 0 },
            { 0x0010, 62, 32, 0 },      /* a factor without a mode */
            { 0x4008, 62, 47, 31 },     /* up 8: 62 + 8/16, 32 + 248/16, 0 + 504/16 */
            { 0x8008, 31, 16, 0 },      /* down 8: 62 - (496+15)/16, 32 - (256+15)/16, 0 */
            { 0x4010, 63, 63, 63 },
            { 0x8010, 0, 0, 0 },
            { 0x4001, 62, 33, 3 },      /* up 1: 62 + 1/16, 32 + 31/16, 0 + 63/16 */
            { 0x8001, 58, 30, 0 },      /* down 1: 62 - (62+15)/16, 32 - (32+15)/16, 0 */
            { 0x401F, 63, 63, 63 },     /* "values >16 same as 16" */
            { 0x8011, 0, 0, 0 },
            { 0xC010, 62, 32, 0 },      /* mode 3, reserved: nothing */
            { 0x4000 | 0x3FE0 | 8, 62, 47, 31 }, /* "5-13 Not used" */
        };
        for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
            w16(MASTER_BRIGHT, k_cases[i].value);
            frame_line(0);
            PX(100, RGB18(k_cases[i].r, k_cases[i].g, k_cases[i].b));
        }
        EQ("unused bits read back as zero", r16(MASTER_BRIGHT), 0x4008);
        const uint32_t color = RGB18(62, 32, 0);
        for (uint32_t mode = 0; mode < 4; mode++) {
            for (uint32_t factor = 0; factor < 32; factor++) {
                w16(MASTER_BRIGHT, (mode << 14) | factor);
                frame_line(0);
                PX(100, mode == 1 ? ref_brighter(color, factor, 0) : mode == 2 ? ref_darker(color, factor, 15) : color);
            }
        }

        /* It comes after the colour special effects: BLDY brightens (62, 32,
         * 0) by 8/16 to (63, 48, 32), and master brightness takes 8/16 off
         * that: 63 - (504+15)/16, 48 - (384+15)/16, 32 - (256+15)/16. */
        w16(BLDCNT, 0x00A0);
        w16(BLDY, 8);
        w16(MASTER_BRIGHT, 0x8008);
        frame_line(0);
        PX(100, RGB18(31, 24, 16));
        w16(BLDCNT, 0);

        /* And after forced blank's white, and the white of display mode 0
         * (as current melonDS has it; melonDS 1.1 left that one white). */
        w32(DISPCNT, DISP_ON | 0x80u);
        frame_line(0);
        PX(100, RGB18(31, 31, 31)); /* 63 - (504+15)/16 */
        w32(DISPCNT, 0);
        frame_line(0);
        PX(100, RGB18(31, 31, 31));
        w16(MASTER_BRIGHT, 0x4010);
        frame_line(0);
        PX(100, WHITE18);
    }

    /* "Mode 2-3 display a raw direct color bitmap [...] however the Master
     * Brightness effect can be applied to these modes." */
    reset_scene(0);
    put16(g_lcdc[1], 100 * 2, RGB15(31, 16, 0));
    w32(DISPCNT, (2u << 16) | (1u << 18));
    w16(MASTER_BRIGHT, 0x8008);
    frame_line(0);
    PX(100, RGB18(31, 16, 0));
    PX(101, 0);
    w16(MASTER_BRIGHT, 0x4008);
    frame_line(0);
    PX(100, RGB18(62, 47, 31));
    PX(101, RGB18(31, 31, 31)); /* black, 8/16 up: 63*8/16 */
    w32(DISPCNT, 3u << 16);
    nds_2d_begin_frame(&g_e);
    w16(DISP_MMEM_FIFO, RGB15(31, 16, 0));
    render(0);
    PX(0, RGB18(62, 47, 31));
}

/* ------------------------------------------------------------------ */
/* Display capture                                                     */
/* ------------------------------------------------------------------ */

/* DISPCAPCNT (GBATEK "DS Video Capture and Main Memory Display Mode"). */
#define CAP_ENABLE (1u << 31)
#define CAP_SOURCE(n) ((uint32_t)(n) << 29)  /* 0 = A, 1 = B, 2 and 3 = A and B blended */
#define CAP_READ_OFFSET(n) ((uint32_t)(n) << 26)
#define CAP_B_FIFO (1u << 25)                /* source B: 0 = VRAM, 1 = main memory display FIFO */
#define CAP_A_3D (1u << 24)                  /* source A: 0 = the graphics picture, 1 = the 3D picture */
#define CAP_SIZE(n) ((uint32_t)(n) << 20)    /* 0 = 128x128, 1 = 256x64, 2 = 256x128, 3 = 256x192 */
#define CAP_WRITE_OFFSET(n) ((uint32_t)(n) << 18)
#define CAP_BLOCK(n) ((uint32_t)(n) << 16)
#define CAP_EVB(n) ((uint32_t)(n) << 8)
#define CAP_EVA(n) ((uint32_t)(n))

/* One whole frame. */
static void whole_frame(void)
{
    nds_2d_begin_frame(&g_e);
    for (int y = 0; y < 192; y++) render(y);
    nds_2d_end_frame(&g_e);
}

/* Halfword `index` of VRAM block `block`. */
static uint32_t captured(uint32_t block, uint32_t index)
{
    return nds_get_le16(g_lcdc[block] + index * 2u);
}

static void fill_blocks(uint32_t value)
{
    for (uint32_t block = 0; block < 4; block++) {
        for (uint32_t i = 0; i < 0x10000; i++) put16(g_lcdc[block], i * 2, value);
    }
}

/* The picture the captures take: the test bitmap on BG3, whose pixel (x, y)
 * is the 15-bit colour shade(pattern(x, y)). */
static void capture_scene(void)
{
    reset_scene(0);
    pattern_bitmap();
    w16(BG3CNT, 0x4080);
    w16(BG3PA, 0x100);
    w16(BG3PD, 0x100);
    w32(DISPCNT, DISP_ON | 3u | DISP_BG(3));
}

#define SEEN(x, y) (0x8000u | shade(pattern((uint32_t)(x), (uint32_t)(y))))

static void test_capture(void)
{
    /* "After setting the Capture Enable bit, capture starts at next line 0,
     * and the capture enable/busy bit is then automatically cleared (in line
     * 192, regardless of the capture size)." */
    g_section = "capture enable";
    capture_scene();
    fill_blocks(0x5555);
    const uint32_t control = CAP_SIZE(3) | CAP_BLOCK(0);
    nds_2d_begin_frame(&g_e);
    render(0);
    w32(DISPCAPCNT, CAP_ENABLE | control); /* set during line 0: too late for this frame */
    for (int y = 1; y < 192; y++) render(y);
    EQ("not started", captured(0, 256 + 5), 0x5555);
    EQ("still set at line 191", r32(DISPCAPCNT), CAP_ENABLE | control);
    nds_2d_end_frame(&g_e);
    EQ("still set after a frame it did not start in", r32(DISPCAPCNT), CAP_ENABLE | control);
    nds_2d_begin_frame(&g_e);
    EQ("busy", r32(DISPCAPCNT), CAP_ENABLE | control);
    for (int y = 0; y < 192; y++) render(y);
    EQ("busy at line 191", r8(DISPCAPCNT + 3), 0x80);
    EQ("captured (0,0)", captured(0, 0), SEEN(0, 0));
    EQ("captured (5,1)", captured(0, 256 + 5), SEEN(5, 1));
    EQ("captured (255,191)", captured(0, 191 * 256 + 255), SEEN(255, 191));
    nds_2d_end_frame(&g_e);
    EQ("cleared at line 192", r32(DISPCAPCNT), control);
    fill_blocks(0x5555);
    whole_frame();
    EQ("one frame only", captured(0, 0), 0x5555);
    /* Clearing the bit by hand does not stop a capture that has started
     * (melonDS), and setting it again in VBlank starts the next one. */
    w32(DISPCAPCNT, CAP_ENABLE | control);
    nds_2d_begin_frame(&g_e);
    render(0);
    w32(DISPCAPCNT, control);
    render(1);
    EQ("line 1 after the bit was cleared", captured(0, 256 + 5), SEEN(5, 1));
    nds_2d_end_frame(&g_e);
    EQ("stays cleared", r32(DISPCAPCNT), control);

    /* "20-21 Capture Size (0=128x128, 1=256x64, 2=256x128, 3=256x192 dots)";
     * "Capture Sizes less than 256x192 capture the upper-left portion of the
     * screen." Line y is written at y * width. */
    g_section = "capture size";
    static const uint32_t k_dim[4][2] = { { 128, 128 }, { 256, 64 }, { 256, 128 }, { 256, 192 } };
    for (uint32_t size = 0; size < 4; size++) {
        const uint32_t w = k_dim[size][0], h = k_dim[size][1];
        fill_blocks(0x5555);
        w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(size) | CAP_BLOCK(0));
        whole_frame();
        EQ("first pixel", captured(0, 0), SEEN(0, 0));
        EQ("end of the first line", captured(0, w - 1), SEEN(w - 1, 0));
        EQ("start of the second line", captured(0, w), SEEN(0, 1));
        EQ("a pixel", captured(0, 9 * w + 77), SEEN(77, 9));
        EQ("last pixel", captured(0, h * w - 1), SEEN(w - 1, h - 1));
        EQ("nothing after it", captured(0, h * w), 0x5555);
        EQ("nothing far after it", captured(0, 0xFFFF), 0x5555);
    }

    /* "16-17 VRAM Write Block (0..3 = VRAM A..D)", "18-19 VRAM Write Offset
     * (0=00000h, 0=08000h, 0=10000h, 0=18000h)". */
    g_section = "capture destination";
    for (uint32_t block = 0; block < 4; block++) {
        for (uint32_t offset = 0; offset < 4; offset++) {
            fill_blocks(0x5555);
            w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(1) | CAP_BLOCK(block) | CAP_WRITE_OFFSET(offset));
            whole_frame();
            for (uint32_t other = 0; other < 4; other++) {
                for (uint32_t at = 0; at < 4; at++) {
                    const bool here = other == block && at == offset;
                    EQ("line 0", captured(other, at * 0x4000u + 3), here ? SEEN(3, 0) : 0x5555u);
                    EQ("line 63", captured(other, at * 0x4000u + 63 * 256 + 255), here ? SEEN(255, 63) : 0x5555u);
                }
            }
        }
    }
    /* "VRAM Read/Write Offsets wrap to 00000h when exceeding 1FFFFh (max
     * 128K)": from offset 3, line 64 of a full screen is at the start. */
    fill_blocks(0x5555);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(3) | CAP_BLOCK(2) | CAP_WRITE_OFFSET(3));
    whole_frame();
    EQ("line 63 at the end", captured(2, 0xFFFF), SEEN(255, 63));
    EQ("line 64 at the start", captured(2, 0), SEEN(0, 64));
    EQ("line 191", captured(2, 127 * 256 + 255), SEEN(255, 191));
    EQ("beyond line 191", captured(2, 128 * 256), 0x5555);
    /* A destination that is not mapped as LCDC memory takes nothing, and
     * the capture still ends. */
    fill_blocks(0x5555);
    g_vram.capture[1] = NULL;
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(3) | CAP_BLOCK(1));
    whole_frame();
    EQ("not written", captured(1, 0), 0x5555);
    EQ("ended", r32(DISPCAPCNT), CAP_SIZE(3) | CAP_BLOCK(1));
    g_vram.capture[1] = g_lcdc[1];

    /*
     * Source A, the graphics picture: the composed line, "15bit color depth
     * (even when capturing 18bit 3D-images)", alpha 1. It is taken before
     * master brightness, in every display mode.
     */
    g_section = "capture source A";
    capture_scene();
    pal_bg(0, RGB15(31, 0, 16));
    w32(DISPCNT, DISP_ON | DISP_BG(0) | DISP_BG(1));
    /* Two BGs blended to (31, 31, 32) of 63: 15 bits keep (15, 15, 16). */
    pal_bg(1, RGB15(31, 0, 16));
    pal_bg(2, RGB15(0, 31, 16));
    memset(g_bg, 0, 0x4000);
    solid4(g_bg, 32, 1);
    solid4(g_bg, 64, 2);
    put16(g_bg, 0x800, 1);
    put16(g_bg, 0x1000, 2);
    w16(BG0CNT, (1u << 8) | 0u);
    w16(BG1CNT, (2u << 8) | 1u);
    w16(BLDCNT, 0x0241);
    w16(BLDALPHA, 0x0808);
    w16(MASTER_BRIGHT, 0x8010);
    fill_blocks(0x5555);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(3) | CAP_BLOCK(0));
    nds_2d_begin_frame(&g_e);
    render(0);
    PX(0, 0); /* the screen is black */
    EQ("blended pixel", captured(0, 0), 0x8000u | RGB15(15, 15, 16));
    EQ("backdrop", captured(0, 8), 0x8000u | RGB15(31, 0, 16));
    nds_2d_end_frame(&g_e);
    w16(BLDCNT, 0);
    w16(MASTER_BRIGHT, 0);
    for (uint32_t display = 0; display < 4; display++) {
        fill_blocks(0x5555);
        put16(g_lcdc[0], 0, RGB15(1, 2, 3));
        w32(DISPCNT, (display << 16) | DISP_BG(0));
        w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(3) | CAP_BLOCK(0));
        nds_2d_begin_frame(&g_e);
        render(0);
        EQ("captured in every display mode", captured(0, 0), 0x8000u | RGB15(31, 0, 16));
        EQ("backdrop in every display mode", captured(0, 8), 0x8000u | RGB15(31, 0, 16));
        /* Display mode 2 shows the block the capture is replacing: the line
         * as it was. */
        if (display == 2) PX(0, RGB18(2, 4, 6));
        nds_2d_end_frame(&g_e);
    }
    /* Forced blank is white in the capture too. */
    fill_blocks(0x5555);
    w32(DISPCNT, DISP_ON | 0x80u);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(3) | CAP_BLOCK(0));
    nds_2d_begin_frame(&g_e);
    render(0);
    EQ("forced blank", captured(0, 0), 0xFFFF);
    nds_2d_end_frame(&g_e);

    /* "24 Source A (0=Graphics Screen BG+3D+OBJ, 1=3D Screen)": the 3D
     * engine's line itself, whether or not BG0 shows it, with "Dest_Alpha =
     * SrcA_Alpha": set where the 3D pixel has any alpha. */
    g_3d_line[0] = PIX3D(63, 1, 33, 31);
    g_3d_line[1] = PIX3D(40, 20, 10, 0);
    g_3d_line[2] = PIX3D(40, 20, 10, 1);
    g_3d_line[6] = PIX3D(8, 60, 2, 31);
    g_3d = g_3d_line;
    for (uint32_t as_bg0 = 0; as_bg0 < 2; as_bg0++) {
        fill_blocks(0x5555);
        w32(DISPCNT, DISP_ON | DISP_BG(0) | (as_bg0 ? DISP_3D : 0u));
        w32(DISPCAPCNT, CAP_ENABLE | CAP_A_3D | CAP_SIZE(3) | CAP_BLOCK(0));
        nds_2d_begin_frame(&g_e);
        render(0);
        EQ("opaque 3D pixel", captured(0, 0), 0x8000u | RGB15(31, 0, 16));
        EQ("3D pixel of alpha 0", captured(0, 1), RGB15(20, 10, 5));
        EQ("3D pixel of alpha 1", captured(0, 2), 0x8000u | RGB15(20, 10, 5));
        EQ("empty 3D pixel", captured(0, 3), 0);
        nds_2d_end_frame(&g_e);
    }
    /* It is the line as BG0HOFS scrolls it (melonDS). */
    w16(BG0HOFS, 4);
    fill_blocks(0x5555);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_A_3D | CAP_SIZE(3) | CAP_BLOCK(0));
    nds_2d_begin_frame(&g_e);
    render(0);
    EQ("scrolled 3D", captured(0, 2), 0x8000u | RGB15(4, 30, 1));
    EQ("past the end of the scrolled 3D line", captured(0, 252), 0);
    nds_2d_end_frame(&g_e);
    w16(BG0HOFS, 0);
    /* No 3D line at all: nothing, alpha 0. */
    g_3d = NULL;
    fill_blocks(0x5555);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_A_3D | CAP_SIZE(3) | CAP_BLOCK(0));
    nds_2d_begin_frame(&g_e);
    render(0);
    EQ("no 3D line", captured(0, 0), 0);
    nds_2d_end_frame(&g_e);

    /*
     * Source B: "25 Source B (0=VRAM, 1=Main Memory Display FIFO)". "VRAM
     * Read Block (VRAM A..D) is selected in DISPCNT Bits 18-19", read as a
     * 256-pixel wide bitmap from "26-27 VRAM Read Offset (0=00000h,
     * 0=08000h, 0=10000h, 0=18000h)". "Capture B: Dest_Intensity =
     * SrcB_Intensitity ; Dest_Alpha=SrcB_Alpha": a copy.
     */
    g_section = "capture source B";
    capture_scene();
    for (uint32_t block = 0; block < 4; block++) {
        for (uint32_t offset = 0; offset < 4; offset++) {
            const uint32_t to = (block + 1u) & 3u;
            fill_blocks(0x5555);
            /* Pixel (7, 2) as each read offset sees it, in every block. */
            for (uint32_t b = 0; b < 4; b++) {
                for (uint32_t o = 0; o < 4; o++) {
                    put16(g_lcdc[b], (o * 0x4000u + 2 * 256 + 7) * 2u, 0x1000u + b * 16u + o);
                }
            }
            put16(g_lcdc[block], (offset * 0x4000u + 2 * 256 + 8) * 2u, 0x8123);
            w32(DISPCNT, DISP_ON | (block << 18));
            w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(1) | CAP_READ_OFFSET(offset) | CAP_SIZE(2) | CAP_BLOCK(to));
            nds_2d_begin_frame(&g_e);
            for (int y = 0; y < 3; y++) render(y);
            EQ("copied", captured(to, 2 * 256 + 7), 0x1000u + block * 16u + offset);
            EQ("copied with its alpha bit", captured(to, 2 * 256 + 8), 0x8123);
            EQ("copied filler", captured(to, 2 * 256 + 9), 0x5555);
            nds_2d_end_frame(&g_e);
        }
    }
    /* The read offset wraps like the write offset. */
    fill_blocks(0x5555);
    put16(g_lcdc[0], 5 * 2, 0x0777);
    w32(DISPCNT, DISP_ON);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(1) | CAP_READ_OFFSET(3) | CAP_SIZE(3) | CAP_BLOCK(1));
    whole_frame();
    EQ("line 64 reads the start", captured(1, 64 * 256 + 5), 0x0777);
    /* "VRAM Read Offset is ignored (zero) in VRAM Display Mode". */
    fill_blocks(0x5555);
    put16(g_lcdc[0], 5 * 2, 0x0777);
    w32(DISPCNT, 2u << 16);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(1) | CAP_READ_OFFSET(3) | CAP_SIZE(3) | CAP_BLOCK(1));
    whole_frame();
    EQ("line 0 reads the start", captured(1, 5), 0x0777);
    EQ("line 64 reads its own line", captured(1, 64 * 256 + 5), 0x5555);
    /* A 128-pixel wide capture still reads 256-pixel wide lines. */
    fill_blocks(0x5555);
    put16(g_lcdc[0], (3 * 256 + 4) * 2, 0x0321);
    w32(DISPCNT, DISP_ON);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(1) | CAP_SIZE(0) | CAP_BLOCK(1));
    whole_frame();
    EQ("128 wide", captured(1, 3 * 128 + 4), 0x0321);
    /* A source block that is not mapped reads as zero. */
    fill_blocks(0x5555);
    g_vram.lcdc[0] = NULL;
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(1) | CAP_SIZE(3) | CAP_BLOCK(1));
    whole_frame();
    EQ("unmapped source", captured(1, 77), 0);
    g_vram.lcdc[0] = g_lcdc[0];
    /* The main memory display FIFO: the line that was sent. */
    fill_blocks(0x5555);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(1) | CAP_B_FIFO | CAP_SIZE(3) | CAP_BLOCK(1));
    nds_2d_begin_frame(&g_e);
    for (uint32_t x = 0; x < 256; x += 2) w32(DISP_MMEM_FIFO, (0x8000u | x) | ((x + 1u) << 16));
    render(0);
    EQ("FIFO pixel 0", captured(1, 0), 0x8000);
    EQ("FIFO pixel 1", captured(1, 1), 0x0001);
    EQ("FIFO pixel 254", captured(1, 254), 0x80FE);
    EQ("FIFO pixel 255", captured(1, 255), 0x00FF);
    nds_2d_end_frame(&g_e);

    /*
     * "29-30 Capture Source (0=Source A, 1=Source B, 2/3=Sources A+B
     * blended)":
     *   Dest_Intensity = (  (SrcA_Intensitity * SrcA_Alpha * EVA)
     *                     + (SrcB_Intensitity * SrcB_Alpha * EVB) ) / 16
     *   Dest_Alpha = (SrcA_Alpha AND (EVA>0)) OR (SrcB_Alpha AND EVB>0))
     * on 5-bit intensities; melonDS rounds to nearest and limits to 31.
     * A is the backdrop (31, 0, 16), B (0, 31, 16) with and without alpha.
     */
    g_section = "capture blending";
    reset_scene(0);
    pal_bg(0, RGB15(31, 0, 16));
    w32(DISPCNT, DISP_ON | (1u << 18)); /* source B: block B */
    static const struct {
        uint32_t eva, evb, b_alpha, want;
    } k_blend[] = {
        { 8, 8, 1, 0x8000u | RGB15(16, 16, 16) },  /* (248+8)/16, (248+8)/16, (128+128+8)/16 */
        { 8, 8, 0, 0x8000u | RGB15(16, 0, 8) },    /* (248+8)/16, 8/16, (128+8)/16 */
        { 0, 8, 0, 0 },                            /* neither source counts: black, alpha 0 */
        { 0, 16, 1, 0x8000u | RGB15(0, 31, 16) },
        { 16, 0, 1, 0x8000u | RGB15(31, 0, 16) },
        { 16, 0, 0, 0x8000u | RGB15(31, 0, 16) },
        { 16, 16, 1, 0x8000u | RGB15(31, 31, 31) }, /* (496+8)/16, (496+8)/16, (256+256+8)/16 = 32 -> 31 */
        { 0, 0, 1, 0 },
        { 1, 1, 1, 0x8000u | RGB15(2, 2, 2) },      /* (31+8)/16, (31+8)/16, (16+16+8)/16 */
        { 31, 31, 1, 0x8000u | RGB15(31, 31, 31) }, /* above 16 is 16 */
    };
    for (uint32_t source = 2; source < 4; source++) {
        for (size_t i = 0; i < sizeof k_blend / sizeof k_blend[0]; i++) {
            fill_blocks(0x5555);
            put16(g_lcdc[1], 0, (k_blend[i].b_alpha ? 0x8000u : 0u) | RGB15(0, 31, 16));
            w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(source) | CAP_SIZE(3) | CAP_BLOCK(0) | CAP_EVA(k_blend[i].eva) |
                                CAP_EVB(k_blend[i].evb));
            nds_2d_begin_frame(&g_e);
            render(0);
            EQ("blend", captured(0, 0), k_blend[i].want);
            nds_2d_end_frame(&g_e);
        }
    }
    /* Every pair of coefficients, for a B with and without alpha. */
    for (uint32_t eva = 0; eva < 32; eva++) {
        for (uint32_t evb = 0; evb < 32; evb++) {
            const uint32_t a = RGB15(31, 0, 16), b = RGB15(9, 31, 22);
            const uint32_t fa = eva > 16 ? 16 : eva, fb = evb > 16 ? 16 : evb;
            put16(g_lcdc[1], 0, 0x8000u | b);
            put16(g_lcdc[1], 2, b);
            w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(2) | CAP_SIZE(3) | CAP_BLOCK(0) | CAP_EVA(eva) | CAP_EVB(evb));
            nds_2d_begin_frame(&g_e);
            render(0);
            nds_2d_end_frame(&g_e);
            uint32_t both = (fa || fb) ? 0x8000u : 0u, a_only = fa ? 0x8000u : 0u;
            for (int i = 0; i < 3; i++) {
                const uint32_t ia = (a >> (5 * i)) & 31u, ib = (b >> (5 * i)) & 31u;
                uint32_t v = (ia * fa + ib * fb + 8u) / 16u;
                both |= (v > 31 ? 31 : v) << (5 * i);
                v = (ia * fa + 8u) / 16u;
                a_only |= (v > 31 ? 31 : v) << (5 * i);
            }
            EQ("blend with B's alpha set", captured(0, 0), both);
            EQ("blend with B's alpha clear", captured(0, 1), a_only);
        }
    }
    /* Blending the 3D picture: its alpha is source A's. */
    g_3d_line[0] = PIX3D(62, 0, 32, 31);
    g_3d_line[1] = PIX3D(62, 0, 32, 0);
    g_3d = g_3d_line;
    put16(g_lcdc[1], 0, 0x8000u | RGB15(0, 31, 16));
    put16(g_lcdc[1], 2, 0x8000u | RGB15(0, 31, 16));
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(2) | CAP_A_3D | CAP_SIZE(3) | CAP_BLOCK(0) | CAP_EVA(8) | CAP_EVB(8));
    nds_2d_begin_frame(&g_e);
    render(0);
    EQ("3D blended", captured(0, 0), 0x8000u | RGB15(16, 16, 16));
    EQ("3D of alpha 0 blended", captured(0, 1), 0x8000u | RGB15(0, 16, 8)); /* B alone: 8/16, (248+8)/16, (128+8)/16 */
    nds_2d_end_frame(&g_e);
    g_3d = NULL;

    /* The picture a game makes with it: capture, then show the block. */
    g_section = "capture round trip";
    capture_scene();
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(3) | CAP_BLOCK(3));
    whole_frame();
    w32(DISPCNT, (2u << 16) | (3u << 18));
    frame_line(100);
    for (int x = 0; x < 256; x += 5) PX(x, TEXEL(x, 100));
}

/* ------------------------------------------------------------------ */
/* Holes in the memory, and arbitrary contents                         */
/* ------------------------------------------------------------------ */

static uint32_t g_seed = 12345;

static uint32_t rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

static uint32_t rnd32(void)
{
    const uint32_t low = rnd() & 0xFFFFu;
    return low | (rnd() << 16);
}

static void test_robustness(void)
{
    g_section = "unmapped memory";
    /* Everything on, nothing mapped: the backdrop, and no sprite. */
    for (int engine = 0; engine < 2; engine++) {
        for (uint32_t mode = 0; mode < 8; mode++) {
            reset_scene(engine);
            pal_bg(0, C_BACK);
            memset(g_oam, 0, sizeof g_oam); /* 128 sprites at (0,0) */
            for (uint32_t i = 0; i < 32; i++) g_vram.bg[i] = NULL;
            for (uint32_t i = 0; i < 16; i++) g_vram.obj[i] = NULL;
            for (uint32_t i = 0; i < 4; i++) g_vram.bg_ext_pal[i] = NULL;
            g_vram.obj_ext_pal = NULL;
            for (uint32_t i = 0; i < 4; i++) g_vram.lcdc[i] = g_vram.capture[i] = NULL;
            for (uint32_t bg = 0; bg < 4; bg++) w16(BG0CNT + 2 * bg, 0xFFFF);
            w32(DISPCNT, DISP_ON | mode | 0x1F00u | DISP_BG_EXTPAL | DISP_OBJ_EXTPAL | DISP_OBJ_1D);
            w32(DISPCAPCNT, CAP_ENABLE | CAP_SOURCE(2) | CAP_SIZE(3));
            nds_2d_begin_frame(&g_e);
            for (int y = 0; y < 192; y += 37) {
                render(y);
                PX(0, out15(C_BACK));
                PX(255, out15(C_BACK));
            }
            nds_2d_end_frame(&g_e);
            /* No palette either: black. No OAM: no sprites. */
            g_vram.palette = NULL;
            g_vram.oam = NULL;
            frame_line(0);
            PX(0, 0);
            /* No memory at all. */
            nds_2d_begin_frame(&g_e);
            nds_2d_render_line(&g_e, NULL, 0, NULL, g_out);
            PX(0, 0);
            /* Nowhere to put the line. */
            nds_2d_render_line(&g_e, &g_vram, 1, NULL, NULL);
            nds_2d_render_line(&g_e, NULL, 2, NULL, NULL);
        }
    }
    /* Display modes 2 and 3 and a capture with no memory behind them. */
    reset_scene(0);
    w32(DISPCNT, 2u << 16);
    w32(DISPCAPCNT, CAP_ENABLE | CAP_SIZE(3));
    nds_2d_begin_frame(&g_e);
    nds_2d_render_line(&g_e, NULL, 0, NULL, g_out);
    PX(0, 0);
    nds_2d_end_frame(&g_e);
    EQ("capture without memory ends", r32(DISPCAPCNT), CAP_SIZE(3));

    /*
     * Arbitrary registers, OAM and VRAM: whatever a game writes, every
     * pixel is 18 bits and nothing outside the engine's memory is touched
     * (the sanitizer builds check the second part).
     */
    g_section = "arbitrary contents";
    for (int engine = 0; engine < 2; engine++) {
        reset_scene(engine);
        for (size_t i = 0; i < sizeof g_bg; i++) g_bg[i] = (uint8_t)rnd();
        for (size_t i = 0; i < sizeof g_obj; i++) g_obj[i] = (uint8_t)rnd();
        for (size_t i = 0; i < sizeof g_pal; i++) g_pal[i] = (uint8_t)rnd();
        for (size_t i = 0; i < sizeof g_bg_ext; i++) g_bg_ext[i / 0x2000][i % 0x2000] = (uint8_t)rnd();
        for (size_t i = 0; i < sizeof g_obj_ext; i++) g_obj_ext[i] = (uint8_t)rnd();
        for (uint32_t x = 0; x < 256; x++) g_3d_line[x] = rnd32();
        for (int round = 0; round < 400; round++) {
            for (size_t i = 0; i < sizeof g_oam; i++) g_oam[i] = (uint8_t)rnd();
            for (int n = 0; n < 60; n++) {
                static const int k_sizes[3] = { 1, 2, 4 };
                const uint32_t offset = rnd() % 0x78u, value = rnd32();
                nds_2d_write(&g_e, offset, value, k_sizes[rnd() % 3u]);
            }
            /* Mostly with the picture on, so that the layers are drawn. */
            if (round & 3) w8(DISPCNT + 2, (r8(DISPCNT + 2) & 0xFCu) | 1u);
            if (round & 1) w8(DISPCNT, r8(DISPCNT) & 0x7Fu);
            g_3d = (round & 4) ? g_3d_line : NULL;
            for (uint32_t i = 0; i < 4; i++) { /* some pages moved, some taken away */
                const uint32_t slot = rnd() % 32u, page = rnd() % 32u;
                g_vram.bg[slot] = (round & 8) ? NULL : g_bg + page * 0x4000u;
            }
            nds_2d_begin_frame(&g_e);
            uint32_t bits = 0;
            for (int y = 0; y < 192; y += 1 + (int)(rnd() % 13u)) {
                render(y);
                for (int x = 0; x < 256; x++) bits |= g_out[x];
            }
            nds_2d_end_frame(&g_e);
            EQ("18-bit pixels", bits & ~0x3FFFFu, 0);
            for (int line = 192; line < 263; line += 1 + (int)(rnd() % 40u)) nds_2d_blank_line(&g_e, line);
        }
    }
}

/* ------------------------------------------------------------------ */

static double bench(int busy);

int main(int argc, char **argv)
{
    test_registers();
    test_display_modes();
    test_text_bg();
    test_bg_bases();
    test_bg_ext_palettes();
    test_bg_modes();
    test_affine_bg();
    test_reference_points();
    test_extended_bg();
    test_large_bg();
    test_bg_mosaic();
    test_priorities();
    test_sprite_shapes();
    test_sprite_mapping();
    test_sprite_palettes();
    test_sprite_affine();
    test_sprite_mosaic();
    test_bitmap_sprites();
    test_windows();
    test_blending();
    test_3d_layer();
    test_master_brightness();
    test_capture();
    test_robustness();

    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        printf("busy line (4 BGs, 128 sprites, windows, blending): %.0f lines per second\n", bench(1));
        printf("game-like line (3D layer over 3 BGs, 24 sprites): %.0f lines per second\n", bench(2));
        printf("plain line (1 BG, no sprites): %.0f lines per second\n", bench(0));
    }

    printf("%ld lines stacked again around a finer 3D picture, %ld of them wrong\n", g_stacked_lines, g_stacked_wrong);
    printf("%ld around a finer bitmap background and %ld around finer bitmap sprites, %ld of them wrong\n",
           g_bitmap_bg_lines, g_bitmap_obj_lines, g_bitmap_wrong);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return (g_failures || !g_checks || !g_stacked_lines || !g_bitmap_bg_lines || !g_bitmap_obj_lines) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Speed                                                               */
/* ------------------------------------------------------------------ */

/*
 * Lines per second for one engine. The busy scene: BG mode 0 with four text
 * BGs (two of 16 colours, two of 256 with extended palettes, three sizes,
 * all scrolled), 128 sprites that all cross the lines measured (64 16x16 of
 * 16 colours, 32 32x32 of 256, 16 32x32 rotated, 8 semi-transparent, 4
 * OBJ-window, 4 bitmap), window 0 and the OBJ window, and alpha blending
 * between the BGs. The game-like scene: the 3D picture as BG0, a quarter of
 * it translucent, mixed over three of those BGs, and 24 16x16 sprites on
 * the line. The plain scene: one BG.
 */
static double bench(int busy)
{
    reset_scene(0);
    for (size_t i = 0; i < sizeof g_bg; i++) g_bg[i] = (uint8_t)rnd();
    for (size_t i = 0; i < sizeof g_obj; i++) g_obj[i] = (uint8_t)rnd();
    for (size_t i = 0; i < sizeof g_pal; i++) g_pal[i] = (uint8_t)rnd();
    for (size_t i = 0; i < sizeof g_bg_ext; i++) g_bg_ext[i / 0x2000][i % 0x2000] = (uint8_t)rnd();
    for (size_t i = 0; i < sizeof g_obj_ext; i++) g_obj_ext[i] = (uint8_t)rnd();
    w16(BG0CNT, (16u << 8) | 0u);                           /* 16 colours, 256x256 */
    w16(BG1CNT, (18u << 8) | 0x80u | (1u << 2) | 1u | (3u << 14)); /* 256 colours, 512x512 */
    w16(BG2CNT, (22u << 8) | 2u | (1u << 14));              /* 16 colours, 512x256 */
    w16(BG3CNT, (26u << 8) | 0x80u | (2u << 2) | 3u);       /* 256 colours, 256x256 */
    for (uint32_t bg = 0; bg < 4; bg++) {
        w16(BG0HOFS + 4 * bg, 3 + 37 * bg);
        w16(BG0VOFS + 4 * bg, 5 + 11 * bg);
    }
    if (busy == 2) {
        for (uint32_t x = 0; x < 256; x++) {
            const uint32_t alpha = (x & 3u) ? 31u : (x >> 2) & 31u;
            g_3d_line[x] = (rnd32() & 0x3FFFFu) | (alpha << 18);
        }
        g_3d = g_3d_line;
        for (uint32_t n = 0; n < 24; n++) {
            const uint32_t x = (n * 11u) % 240u, tile = (n * 7u) & 0x3FFu;
            obj_set(n, 84u + (n % 8u), x | (1u << 14), tile | ((n % 4u) << 10) | ((n & 15u) << 12));
        }
        w16(BLDCNT, 0x3E00); /* everything under the 3D layer is a 2nd target */
        w32(DISPCNT, DISP_ON | DISP_3D | 0x1F00u | DISP_OBJ_1D | DISP_BG_EXTPAL);
    } else if (busy) {
        for (uint32_t n = 0; n < 128; n++) {
            const uint32_t x = (n * 29u) % 240u, y = 90u - (n % 4u), tile = (n * 7u) & 0x3FFu, prio = (n % 4u) << 10;
            if (n < 64) obj_set(n, y, x | (1u << 14), tile | prio | ((n & 15u) << 12));
            else if (n < 96) obj_set(n, (y - 16u) | A0_256, x | (2u << 14), tile | prio);
            else if (n < 112) {
                obj_set(n, (y - 16u) | A0_AFFINE | A0_256, x | (2u << 14) | ((n & 31u) << 9), tile | prio);
            }
            else if (n < 120) obj_set(n, y | A0_SEMI, x | (1u << 14), tile | prio);
            else if (n < 124) obj_set(n, y | A0_WINDOW, x | (1u << 14), tile | prio);
            else obj_set(n, y | A0_BITMAP, x | (1u << 14), tile | prio | (9u << 12));
        }
        for (uint32_t g = 0; g < 32; g++) obj_matrix(g, 0xE0 + g, 0x30, 0xFFD0, 0xE0 + g);
        w16(WIN0H, WIN(40, 200));
        w16(WIN0V, WIN(0, 192));
        w16(WININ, 0x003F);
        w16(WINOUT, 0x3F1F);
        w16(BLDCNT, 0x3E41);
        w16(BLDALPHA, 0x0A06);
        w32(DISPCNT, DISP_ON | 0x1F00u | DISP_WIN0 | DISP_OBJWIN | DISP_OBJ_1D | DISP_BG_EXTPAL | DISP_OBJ_EXTPAL);
    } else {
        w32(DISPCNT, DISP_ON | DISP_BG(0));
    }

    uint32_t lines = 0, sum = 0;
    const clock_t start = clock();
    clock_t now = start;
    while (now - start < CLOCKS_PER_SEC) {
        for (int k = 0; k < 20; k++) {
            nds_2d_begin_frame(&g_e);
            for (int y = 86; y < 96; y++) {
                render(y);
                sum += g_out[y];
                lines++;
            }
        }
        now = clock();
    }
    if (sum == 0xFFFFFFFFu) printf(" "); /* keep the rendering alive */
    return (double)lines * CLOCKS_PER_SEC / (double)(now - start);
}
