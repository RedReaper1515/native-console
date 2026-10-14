/*
 * The DS's two 2D graphics engines: a scanline renderer.
 *
 * Each engine is the GBA's video controller with more of everything
 * (GBATEK "DS Video"; the GBA chapters it builds on are "LCD I/O ...",
 * "LCD OBJ ..." and "LCD Color Special Effects"), and this file is
 * runtimes/runtime-gba/src/gba_ppu.c grown the same way. A visible line is rendered
 * at the start of its HBlank from the registers and memory as they are at
 * that moment: backgrounds and sprites go into per-layer line buffers, and
 * are then composed per pixel through the window masks, the priorities and
 * the colour special effects.
 *
 * What the DS adds: 512 KiB of BG VRAM and 256 KiB of OBJ VRAM behind a
 * bank mapping (handed in as 16 KiB pages), extended affine and large
 * bitmap backgrounds, extended palettes, bitmap sprites, the 3D engine's
 * picture as BG0, a display capture unit that writes the finished line back
 * into VRAM, raw bitmap display modes, and a master brightness stage.
 *
 * Colours are 6 bits a channel from the layer buffers on: the 3D layer is
 * 18-bit, and a 15-bit 2D colour takes part with each channel doubled.
 *
 * Where GBATEK does not say what the hardware does, the behaviour was
 * learnt from reading melonDS (src/GPU2D*.cpp, GPU_ColorOp.h; GPL-3.0, read
 * as a behaviour reference, nothing copied). Each such place says so.
 */
#include <string.h>

#include "nds_2d.h"

#define W NDS_SCREEN_WIDTH

_Static_assert(sizeof(Nds2d) == 656, "Nds2d must have no padding: it is saved with memcpy");

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

enum {
    REG_DISPCNT = 0x00,
    REG_BG0CNT = 0x08,
    REG_BG0HOFS = 0x10,
    REG_BG0VOFS = 0x12,
    REG_BG2PA = 0x20, /* PA, PB, PC, PD, then X and Y */
    REG_BG2X = 0x28,
    REG_BG2Y = 0x2C,
    REG_BG3PA = 0x30,
    REG_BG3X = 0x38,
    REG_BG3Y = 0x3C,
    REG_WIN0H = 0x40,
    REG_WIN0V = 0x44,
    REG_WININ = 0x48,
    REG_WINOUT = 0x4A,
    REG_MOSAIC = 0x4C,
    REG_BLDCNT = 0x50,
    REG_BLDALPHA = 0x52,
    REG_BLDY = 0x54,
    REG_DISPCAPCNT = 0x64,
    REG_DISP_MMEM_FIFO = 0x68,
    REG_MASTER_BRIGHT = 0x6C
};

enum { ENGINE_A = 1, ENGINE_B = 2, BOTH = 3 };

/* What a write does besides being stored. */
enum {
    HOOK_NONE,
    HOOK_REF_X, /* BGxX: reload that background's internal X reference point */
    HOOK_REF_Y, /* BGxY: likewise for Y */
    HOOK_FIFO   /* DISP_MMEM_FIFO: each halfword completed is one pixel */
};

typedef struct {
    uint8_t offset;      /* from the engine's base */
    uint8_t size;        /* bytes */
    uint8_t engines;     /* which engines have it */
    uint8_t hook;
    uint32_t read_mask;  /* bits that read back; the rest read as zero */
    uint32_t write_mask; /* bits a write stores */
} RegDesc;

/*
 * The register file (GBATEK "GBA I/O Map" for which registers read back,
 * "DS Video BG Modes / Control" and "DS Video Capture and Main Memory
 * Display Mode" for the DS's bits). Write-only registers read as zero.
 *
 * Engine B's DISPCNT lacks the bits the DISPCNT table marks "A": BG0 3D
 * (3), display modes 2-3 (17), the VRAM block (18-19), the bitmap OBJ
 * boundary (22) and the 64K character and screen base offsets (24-29).
 */
static const RegDesc k_registers[] = {
    { REG_DISPCNT, 4, ENGINE_A, HOOK_NONE, 0xFFFFFFFFu, 0xFFFFFFFFu },
    { REG_DISPCNT, 4, ENGINE_B, HOOK_NONE, 0xC0B1FFF7u, 0xC0B1FFF7u },
    { 0x08, 2, BOTH, HOOK_NONE, 0xFFFFu, 0xFFFFu }, /* BG0CNT */
    { 0x0A, 2, BOTH, HOOK_NONE, 0xFFFFu, 0xFFFFu }, /* BG1CNT */
    { 0x0C, 2, BOTH, HOOK_NONE, 0xFFFFu, 0xFFFFu }, /* BG2CNT */
    { 0x0E, 2, BOTH, HOOK_NONE, 0xFFFFu, 0xFFFFu }, /* BG3CNT */
    { 0x10, 2, BOTH, HOOK_NONE, 0, 0x01FFu },       /* BG0HOFS */
    { 0x12, 2, BOTH, HOOK_NONE, 0, 0x01FFu },       /* BG0VOFS */
    { 0x14, 2, BOTH, HOOK_NONE, 0, 0x01FFu },       /* BG1HOFS */
    { 0x16, 2, BOTH, HOOK_NONE, 0, 0x01FFu },       /* BG1VOFS */
    { 0x18, 2, BOTH, HOOK_NONE, 0, 0x01FFu },       /* BG2HOFS */
    { 0x1A, 2, BOTH, HOOK_NONE, 0, 0x01FFu },       /* BG2VOFS */
    { 0x1C, 2, BOTH, HOOK_NONE, 0, 0x01FFu },       /* BG3HOFS */
    { 0x1E, 2, BOTH, HOOK_NONE, 0, 0x01FFu },       /* BG3VOFS */
    { 0x20, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* BG2PA */
    { 0x22, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* BG2PB */
    { 0x24, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* BG2PC */
    { 0x26, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* BG2PD */
    { REG_BG2X, 4, BOTH, HOOK_REF_X, 0, 0x0FFFFFFFu },
    { REG_BG2Y, 4, BOTH, HOOK_REF_Y, 0, 0x0FFFFFFFu },
    { 0x30, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* BG3PA */
    { 0x32, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* BG3PB */
    { 0x34, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* BG3PC */
    { 0x36, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* BG3PD */
    { REG_BG3X, 4, BOTH, HOOK_REF_X, 0, 0x0FFFFFFFu },
    { REG_BG3Y, 4, BOTH, HOOK_REF_Y, 0, 0x0FFFFFFFu },
    { 0x40, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* WIN0H */
    { 0x42, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* WIN1H */
    { 0x44, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* WIN0V */
    { 0x46, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },       /* WIN1V */
    { REG_WININ, 2, BOTH, HOOK_NONE, 0x3F3Fu, 0x3F3Fu },
    { REG_WINOUT, 2, BOTH, HOOK_NONE, 0x3F3Fu, 0x3F3Fu },
    { REG_MOSAIC, 2, BOTH, HOOK_NONE, 0, 0xFFFFu },
    { REG_BLDCNT, 2, BOTH, HOOK_NONE, 0x3FFFu, 0x3FFFu },
    { REG_BLDALPHA, 2, BOTH, HOOK_NONE, 0x1F1Fu, 0x1F1Fu },
    { REG_BLDY, 2, BOTH, HOOK_NONE, 0, 0x001Fu },
    { REG_DISPCAPCNT, 4, ENGINE_A, HOOK_NONE, 0xEF3F1F1Fu, 0xEF3F1F1Fu },
    { REG_DISP_MMEM_FIFO, 4, ENGINE_A, HOOK_FIFO, 0, 0xFFFFFFFFu },
    { REG_MASTER_BRIGHT, 2, BOTH, HOOK_NONE, 0xC01Fu, 0xC01Fu },
};

/* The register that holds byte `addr` of this engine's register space. */
static const RegDesc *reg_at(const Nds2d *e, uint32_t addr)
{
    const uint32_t who = e->engine ? ENGINE_B : ENGINE_A;
    for (size_t i = 0; i < sizeof k_registers / sizeof k_registers[0]; i++) {
        const RegDesc *d = &k_registers[i];
        if ((d->engines & who) && addr - d->offset < d->size) return d;
    }
    return NULL;
}

static inline uint32_t reg16(const Nds2d *e, uint32_t offset)
{
    return nds_get_le16(&e->reg[offset]);
}

static inline uint32_t reg32(const Nds2d *e, uint32_t offset)
{
    return nds_get_le32(&e->reg[offset]);
}

/* A 16-bit register as a signed number (the affine parameters). */
static inline int32_t s16(uint32_t v)
{
    return (int32_t)(v & 0xFFFFu) - (int32_t)((v & 0x8000u) << 1);
}

/* A 28-bit signed reference point, sign-extended. */
static inline int32_t ref_point(uint32_t raw)
{
    raw &= 0x0FFFFFFFu;
    return (int32_t)raw - (int32_t)((raw & 0x08000000u) << 1);
}

/* One pixel arrives at the main memory display FIFO. The engine keeps a
 * line of them; see nds_2d_render_line(). */
static void fifo_push(Nds2d *e, uint32_t pixel)
{
    e->mmem_line[e->mmem_pos & (W - 1u)] = (uint16_t)pixel;
    e->mmem_pos = (uint16_t)((e->mmem_pos + 1u) & (W - 1u));
}

uint32_t nds_2d_read(const Nds2d *e, uint32_t offset, int size)
{
    uint32_t value = 0;
    if ((size != 1 && size != 2 && size != 4) || offset >= NDS_2D_REG_SIZE) return 0;
    for (int i = 0; i < size; i++) {
        const uint32_t addr = offset + (uint32_t)i;
        if (addr >= NDS_2D_REG_SIZE) break;
        const RegDesc *d = reg_at(e, addr);
        if (!d) continue;
        const uint32_t mask = (d->read_mask >> (8u * (addr - d->offset))) & 0xFFu;
        value |= (e->reg[addr] & mask) << (8 * i);
    }
    return value;
}

void nds_2d_write(Nds2d *e, uint32_t offset, uint32_t value, int size)
{
    if ((size != 1 && size != 2 && size != 4) || offset >= NDS_2D_REG_SIZE) return;
    for (int i = 0; i < size; i++) {
        const uint32_t addr = offset + (uint32_t)i;
        if (addr >= NDS_2D_REG_SIZE) break;
        const RegDesc *d = reg_at(e, addr);
        if (!d) continue;
        const uint32_t mask = (d->write_mask >> (8u * (addr - d->offset))) & 0xFFu;
        e->reg[addr] = (uint8_t)((e->reg[addr] & ~mask) | ((value >> (8 * i)) & mask));

        switch (d->hook) {
        case HOOK_REF_X:
        case HOOK_REF_Y: {
            /* "Writing to a reference point register by software outside of
             * the Vblank period does immediately copy the new value to the
             * corresponding internal register" (GBATEK "LCD I/O BG
             * Rotation/Scaling"). Inside VBlank the copy at the start of
             * the frame makes this one moot. */
            const int bg = d->offset >= REG_BG3PA;
            const int32_t point = ref_point(reg32(e, d->offset));
            if (d->hook == HOOK_REF_X) e->ref_x[bg] = point;
            else e->ref_y[bg] = point;
            break;
        }
        case HOOK_FIFO:
            /* A pixel is a halfword; it is complete when its high byte is in. */
            if (addr & 1u) fifo_push(e, reg16(e, addr - 1u));
            break;
        default:
            break;
        }
    }
}

void nds_2d_reset(Nds2d *e, int engine)
{
    memset(e, 0, sizeof *e);
    e->engine = engine ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Line buffers                                                        */
/* ------------------------------------------------------------------ */

/*
 * A pixel in a layer buffer or on its way through the composition:
 *
 *   bits 0-17   colour: red, green, blue, 6 bits each
 *   bits 18-23  the layer it belongs to, one bit, in BLDCNT's order: BG0-BG3, OBJ, backdrop
 *   bits 24-28  3D pixel: its alpha (1-31). Bitmap sprite pixel: its EVA (1-16)
 *   bit 29      a pixel of the 3D layer
 *   bit 30      a semi-transparent sprite pixel
 *   bit 31      a bitmap sprite pixel
 *
 * Every pixel that is there has its layer bit, so 0 is "no pixel".
 */
#define PIX_COLOR 0x0003FFFFu
#define PIX_LAYER_SHIFT 18
#define PIX_BG0 (1u << 18)
#define PIX_OBJ (1u << 22)
#define PIX_BD (1u << 23)
#define PIX_ALPHA_SHIFT 24
#define PIX_3D (1u << 29)
#define PIX_SEMI (1u << 30)
#define PIX_BITMAP (1u << 31)

#define WHITE PIX_COLOR

/* A 15-bit colour as the engine handles it from here on: 6 bits a channel,
 * each the 5-bit value doubled (white is 62, not 63; known from melonDS). */
static inline uint32_t rgb18(uint32_t c)
{
    return ((c & 0x001Fu) << 1) | ((c & 0x03E0u) << 2) | ((c & 0x7C00u) << 3);
}

/* And back, for the capture unit: "Capture data is 15bit color depth" (the
 * low bit of each channel is dropped). */
static inline uint32_t rgb15(uint32_t c)
{
    return ((c >> 1) & 0x001Fu) | ((c >> 2) & 0x03E0u) | ((c >> 3) & 0x7C00u);
}

static inline uint32_t pal18(const uint8_t *palette, uint32_t index)
{
    return rgb18(nds_get_le16(palette + index * 2u));
}

/* Stands in for every page that is not mapped: it reads as zero. Large
 * enough to stand in for a palette or an extended palette slot too. */
static const uint8_t k_zero_page[0x4000] = { 0 };

/* The memory one line is drawn from: the caller's view with the holes
 * filled in, and what DISPCNT says about it. */
typedef struct {
    const uint8_t *bg[32];
    const uint8_t *obj[16];
    const uint8_t *bg_ext_pal[4];
    const uint8_t *obj_ext_pal;
    const uint8_t *palette;
    const uint8_t *oam;  /* NULL: no sprites */
    uint32_t bg_mask;    /* BG VRAM wraps at 512 KiB (engine A) or 128 KiB (engine B) */
    uint32_t obj_mask;   /* OBJ VRAM wraps at 256 KiB or 128 KiB */
    uint32_t dispcnt;
    uint32_t y;
} Scene;

static void scene_init(Scene *s, const Nds2d *e, const Nds2dVram *vram, int y)
{
    const size_t bg_pages = e->engine ? 8 : 32, obj_pages = e->engine ? 8 : 16;
    for (size_t i = 0; i < 32; i++) s->bg[i] = (vram && i < bg_pages && vram->bg[i]) ? vram->bg[i] : k_zero_page;
    for (size_t i = 0; i < 16; i++) s->obj[i] = (vram && i < obj_pages && vram->obj[i]) ? vram->obj[i] : k_zero_page;
    for (size_t i = 0; i < 4; i++) s->bg_ext_pal[i] = (vram && vram->bg_ext_pal[i]) ? vram->bg_ext_pal[i] : k_zero_page;
    s->obj_ext_pal = (vram && vram->obj_ext_pal) ? vram->obj_ext_pal : k_zero_page;
    s->palette = (vram && vram->palette) ? vram->palette : k_zero_page;
    s->oam = vram ? vram->oam : NULL;
    s->bg_mask = e->engine ? 0x1FFFFu : 0x7FFFFu;
    s->obj_mask = e->engine ? 0x1FFFFu : 0x3FFFFu;
    s->dispcnt = reg32(e, REG_DISPCNT);
    s->y = (uint32_t)y;
}

/* VRAM reads. Every address is wrapped into its region first; the wider
 * reads are aligned, as everything the engine fetches is, so they stay
 * inside one page. */
static inline uint32_t bg8(const Scene *s, uint32_t addr)
{
    addr &= s->bg_mask;
    return s->bg[addr >> 14][addr & 0x3FFFu];
}

static inline uint32_t bg16(const Scene *s, uint32_t addr)
{
    addr &= s->bg_mask & ~1u;
    return nds_get_le16(&s->bg[addr >> 14][addr & 0x3FFFu]);
}

static inline uint32_t bg32(const Scene *s, uint32_t addr)
{
    addr &= s->bg_mask & ~3u;
    return nds_get_le32(&s->bg[addr >> 14][addr & 0x3FFFu]);
}

static inline uint32_t obj8(const Scene *s, uint32_t addr)
{
    addr &= s->obj_mask;
    return s->obj[addr >> 14][addr & 0x3FFFu];
}

static inline uint32_t obj16(const Scene *s, uint32_t addr)
{
    addr &= s->obj_mask & ~1u;
    return nds_get_le16(&s->obj[addr >> 14][addr & 0x3FFFu]);
}

/* ------------------------------------------------------------------ */
/* Affine reference points, mosaic counters, window flip-flops         */
/* ------------------------------------------------------------------ */

/* "The internal registers are then incremented by dmx and dmy after each
 * scanline" (GBATEK "LCD I/O BG Rotation/Scaling"). They are 28 bits wide. */
static void advance_refs(Nds2d *e)
{
    for (int i = 0; i < 2; i++) {
        const uint32_t base = i ? REG_BG3PA : REG_BG2PA;
        e->ref_x[i] = ref_point((uint32_t)e->ref_x[i] + (uint32_t)s16(reg16(e, base + 2u))); /* PB */
        e->ref_y[i] = ref_point((uint32_t)e->ref_y[i] + (uint32_t)s16(reg16(e, base + 6u))); /* PD */
    }
}

/*
 * Vertical mosaic (GBATEK "LCD I/O Mosaic Function"): a block of size
 * lines shows its first line throughout. GBATEK does not say what a change
 * of MOSAIC in the middle of a block does. As melonDS has it: the BG block
 * keeps the size it started with, while the OBJ block ends on the line its
 * 4-bit counter equals the size field, which a smaller size written
 * mid-block makes it miss until the counter has wrapped. Both restart with
 * the frame here (melonDS leaves the OBJ counter running across frames).
 */
static void advance_mosaic(Nds2d *e, uint32_t y)
{
    const uint32_t mosaic = reg16(e, REG_MOSAIC);
    if (e->bg_mosaic_count >= e->bg_mosaic_max) {
        e->bg_mosaic_count = 0;
        e->bg_mosaic_max = (uint8_t)((mosaic >> 4) & 15u);
    } else {
        e->bg_mosaic_count++;
    }
    if (e->obj_mosaic_count == ((mosaic >> 12) & 15u)) {
        e->obj_mosaic_count = 0;
        e->obj_mosaic_line = (uint8_t)(y + 1u);
    } else {
        e->obj_mosaic_count = (uint8_t)((e->obj_mosaic_count + 1u) & 15u);
    }
}

/*
 * Windows 0 and 1 are not rectangles that are tested, but flip-flops: the
 * vertical one is set on the line whose number equals Y1 and cleared on the
 * line that equals Y2, the horizontal one likewise at X1 and X2 along each
 * line, clearing wins when both match, and the window is where both are
 * set. That is what makes "X2=00h is treated as 100h", "the window is not
 * displayed if X1=X2=00h" and the Y1 = 0..6 glitch of GBATEK "DS Video
 * Stuff" (DS Window Glitches) true: only the low 8 bits of the line counter
 * (0-262) are compared, on every line of the frame, so lines 256-262 match
 * coordinates 0-6. The flip-flop model itself is melonDS's, and what
 * roms/nds/striker-tests/window/window-midframe was written to show.
 */
static void window_line_start(Nds2d *e, uint32_t line)
{
    for (uint32_t i = 0; i < 2; i++) {
        const uint32_t v = reg16(e, REG_WIN0V + 2u * i); /* Y1 in the high byte, Y2 in the low */
        if ((line & 0xFFu) == (v & 0xFFu)) e->win_v[i] = 0;
        else if ((line & 0xFFu) == (v >> 8)) e->win_v[i] = 1;
    }
}

/* Where a whole line leaves the horizontal flip-flops: X1 and X2 are both
 * passed on the way, so only the later of the two counts. */
static void window_line_end(Nds2d *e)
{
    for (uint32_t i = 0; i < 2; i++) {
        const uint32_t h = reg16(e, REG_WIN0H + 2u * i); /* X1 in the high byte, X2 in the low */
        e->win_h[i] = (uint8_t)((h >> 8) > (h & 0xFFu));
    }
}

/* Bring the flip-flops to the start of `line`, through any lines the engine
 * was not called for. */
static void window_catch_up(Nds2d *e, uint32_t line)
{
    while (e->win_line <= line) {
        window_line_start(e, e->win_line);
        if (e->win_line < line) window_line_end(e);
        e->win_line++;
    }
}

void nds_2d_begin_frame(Nds2d *e)
{
    /* "The above reference points are automatically copied to internal
     * registers during each vblank, specifying the origin for the first
     * scanline." */
    e->ref_x[0] = ref_point(reg32(e, REG_BG2X));
    e->ref_y[0] = ref_point(reg32(e, REG_BG2Y));
    e->ref_x[1] = ref_point(reg32(e, REG_BG3X));
    e->ref_y[1] = ref_point(reg32(e, REG_BG3Y));

    /* The rest of the old frame, as far as the windows have not seen it. */
    if (e->win_line < NDS_LINES_PER_FRAME) {
        window_catch_up(e, NDS_LINES_PER_FRAME - 1);
        window_line_end(e);
    }
    e->win_line = 0;

    e->bg_mosaic_count = 0;
    e->bg_mosaic_max = (uint8_t)((reg16(e, REG_MOSAIC) >> 4) & 15u);
    e->obj_mosaic_count = 0;
    e->obj_mosaic_line = 0;
    e->mmem_pos = 0;

    /* "After setting the Capture Enable bit, capture starts at next line 0"
     * (GBATEK "DS Video Capture and Main Memory Display Mode"). */
    e->capture_active = (uint8_t)(!e->engine && (e->reg[REG_DISPCAPCNT + 3] & 0x80u) != 0);
}

void nds_2d_end_frame(Nds2d *e)
{
    /* "...and the capture enable/busy bit is then automatically cleared (in
     * line 192, regardless of the capture size)". A bit set after line 0
     * waits for the next frame, and stays set until that one ends. */
    if (e->capture_active) {
        e->reg[REG_DISPCAPCNT + 3] &= 0x7Fu;
        e->capture_active = 0;
    }
}

void nds_2d_blank_line(Nds2d *e, int line)
{
    if (line < NDS_VISIBLE_LINES || line >= NDS_LINES_PER_FRAME || e->win_line > (uint32_t)line) return;
    window_catch_up(e, (uint32_t)line);
    window_line_end(e);
}

/* ------------------------------------------------------------------ */
/* Backgrounds                                                         */
/* ------------------------------------------------------------------ */

/* Horizontal mosaic: "pixels 0-5 of each display row are colorized as pixel
 * 0, pixels 6-11 as pixel 6" for a size of 6. Transparency repeats too. */
static void mosaic_h(uint32_t *line, uint32_t size)
{
    if (size <= 1) return;
    for (uint32_t x = 0; x < W; x += size) {
        const uint32_t first = line[x];
        for (uint32_t i = 1; i < size && x + i < W; i++) line[x + i] = first;
    }
}

/* Character and screen base of a tile background: the fields of BGxCNT in
 * 16K and 2K steps, plus on engine A the 64K steps of DISPCNT bits 24-26
 * and 27-29 (GBATEK "DS Video BG Modes / Control", BGxCNT). Engine B's
 * DISPCNT does not have those bits, so they are zero there. */
static inline uint32_t char_base_of(const Scene *s, uint32_t cnt)
{
    return ((cnt >> 2) & 15u) * 0x4000u + ((s->dispcnt >> 24) & 7u) * 0x10000u;
}

static inline uint32_t screen_base_of(const Scene *s, uint32_t cnt)
{
    return ((cnt >> 8) & 31u) * 0x800u + ((s->dispcnt >> 27) & 7u) * 0x10000u;
}

/*
 * A text background: 16-bit map entries, 16-colour or 256-colour tiles,
 * one to four 32x32-tile screens (GBATEK "LCD I/O BG Control", "LCD VRAM BG
 * Screen Data Format"). With extended palettes on (DISPCNT bit 30), the
 * 256-colour tiles take their colours from the map entry's palette number
 * in the background's extended palette slot; "for BG0CNT, BG1CNT only:
 * bit13 selects extended palette slot (BG0: 0=Slot0, 1=Slot2, BG1: 0=Slot1,
 * 1=Slot3)". 16-colour tiles always use the standard palette.
 */
static void render_text_bg(const Nds2d *e, const Scene *s, uint32_t bg, uint32_t *out)
{
    const uint32_t cnt = reg16(e, REG_BG0CNT + 2u * bg);
    const uint32_t char_base = char_base_of(s, cnt);
    const uint32_t screen_base = screen_base_of(s, cnt);
    const bool bpp8 = (cnt & 0x80u) != 0;
    const uint32_t width = (cnt & 0x4000u) ? 512u : 256u;
    const uint32_t height = (cnt & 0x8000u) ? 512u : 256u;
    const uint32_t hofs = reg16(e, REG_BG0HOFS + 4u * bg) & 0x1FFu;
    const uint32_t vofs = reg16(e, REG_BG0VOFS + 4u * bg) & 0x1FFu;
    const uint32_t tag = PIX_BG0 << bg;

    const uint8_t *ext_pal = NULL;
    if (bpp8 && (s->dispcnt & (1u << 30))) {
        ext_pal = s->bg_ext_pal[(bg < 2 && (cnt & 0x2000u)) ? bg + 2u : bg];
    }

    uint32_t line = s->y;
    if (cnt & 0x40u) line -= e->bg_mosaic_count;

    const uint32_t y = (line + vofs) & (height - 1u);
    const uint32_t map_row = screen_base + (y >> 8) * (width >> 8) * 0x800u + ((y & 255u) >> 3) * 64u;
    /* One map entry and one row of tile data per 8 pixels. */
    for (uint32_t x = 0; x < W;) {
        const uint32_t px = (x + hofs) & (width - 1u);
        const uint32_t entry = bg16(s, map_row + (px >> 8) * 0x800u + ((px & 255u) >> 3) * 2u);
        const uint32_t tile = entry & 0x3FFu;
        const uint32_t ty = (entry & 0x800u) ? 7u - (y & 7u) : y & 7u;
        const uint32_t flip = (entry & 0x400u) ? 7u : 0u;
        uint32_t run = 8u - (px & 7u);
        if (run > W - x) run = W - x;

        if (bpp8) {
            const uint32_t row = char_base + tile * 64u + ty * 8u;
            const uint64_t bits = bg32(s, row) | ((uint64_t)bg32(s, row + 4u) << 32);
            const uint8_t *palette = ext_pal ? ext_pal + (entry >> 12) * 512u : s->palette;
            for (uint32_t i = 0; i < run; i++) {
                const uint32_t tx = ((px + i) & 7u) ^ flip;
                const uint32_t index = (uint32_t)(bits >> (tx * 8u)) & 0xFFu;
                out[x + i] = index ? pal18(palette, index) | tag : 0;
            }
        } else {
            const uint32_t bits = bg32(s, char_base + tile * 32u + ty * 4u);
            const uint8_t *palette = s->palette + (entry >> 12) * 32u;
            for (uint32_t i = 0; i < run; i++) {
                const uint32_t tx = ((px + i) & 7u) ^ flip;
                const uint32_t index = (bits >> (tx * 4u)) & 15u;
                out[x + i] = index ? pal18(palette, index) | tag : 0;
            }
        }
        x += run;
    }
    if (cnt & 0x40u) mosaic_h(out, (reg16(e, REG_MOSAIC) & 15u) + 1u);
}

/*
 * The walk of an affine background along a line: it starts at the internal
 * reference point and moves by PA, PC per pixel (GBATEK "LCD I/O BG
 * Rotation/Scaling"). Coordinates are 20.8 fixed point; they are kept
 * unsigned so that they wrap, with negative ones far above every size.
 */
typedef struct {
    uint32_t x, y, dx, dy;
} Walk;

static Walk walk_of(const Nds2d *e, uint32_t bg, uint32_t cnt)
{
    const uint32_t base = bg == 2 ? REG_BG2PA : REG_BG3PA;
    Walk w;
    w.dx = (uint32_t)s16(reg16(e, base));      /* PA */
    w.dy = (uint32_t)s16(reg16(e, base + 4u)); /* PC */
    w.x = (uint32_t)e->ref_x[bg - 2u];
    w.y = (uint32_t)e->ref_y[bg - 2u];
    if (cnt & 0x40u) {
        /* Vertical mosaic: the block's first line again, which is this many
         * steps of PB, PD back. */
        w.x -= e->bg_mosaic_count * (uint32_t)s16(reg16(e, base + 2u));
        w.y -= e->bg_mosaic_count * (uint32_t)s16(reg16(e, base + 6u));
    }
    return w;
}

/*
 * A tile background that rotates and scales. `wide` selects the map entry:
 * 8 bits, a tile number (the GBA's affine background, "LCD VRAM BG Screen
 * Data Format": 256 tiles, 256 colours, standard palette), or 16 bits as in
 * a text map with flips and a palette number (the first of the DS's
 * extended affine forms, "rot/scal with 16bit bgmap entries"; its tiles are
 * 256-colour, and the palette number counts only with extended palettes,
 * where each background uses the slot of its own number).
 */
static void render_affine_bg(const Nds2d *e, const Scene *s, uint32_t bg, bool wide, uint32_t *out)
{
    const uint32_t cnt = reg16(e, REG_BG0CNT + 2u * bg);
    const uint32_t char_base = char_base_of(s, cnt);
    const uint32_t screen_base = screen_base_of(s, cnt);
    const uint32_t size = 128u << ((cnt >> 14) & 3u);
    const bool wrap = (cnt & 0x2000u) != 0;
    const uint8_t *ext_pal = (wide && (s->dispcnt & (1u << 30))) ? s->bg_ext_pal[bg] : NULL;
    const uint32_t tag = PIX_BG0 << bg;
    Walk w = walk_of(e, bg, cnt);

    for (uint32_t x = 0; x < W; x++, w.x += w.dx, w.y += w.dy) {
        uint32_t tx = w.x >> 8, ty = w.y >> 8;
        if (wrap) {
            tx &= size - 1u;
            ty &= size - 1u;
        } else if ((tx | ty) >= size) {
            out[x] = 0;
            continue;
        }
        const uint32_t cell = (ty >> 3) * (size >> 3) + (tx >> 3);
        uint32_t index;
        const uint8_t *palette = s->palette;
        if (wide) {
            const uint32_t entry = bg16(s, screen_base + cell * 2u);
            const uint32_t px = (entry & 0x400u) ? 7u - (tx & 7u) : tx & 7u;
            const uint32_t py = (entry & 0x800u) ? 7u - (ty & 7u) : ty & 7u;
            index = bg8(s, char_base + (entry & 0x3FFu) * 64u + py * 8u + px);
            if (ext_pal) palette = ext_pal + (entry >> 12) * 512u;
        } else {
            index = bg8(s, char_base + bg8(s, screen_base + cell) * 64u + (ty & 7u) * 8u + (tx & 7u));
        }
        out[x] = index ? pal18(palette, index) | tag : 0;
    }
    if (cnt & 0x40u) mosaic_h(out, (reg16(e, REG_MOSAIC) & 15u) + 1u);
}

/*
 * A bitmap that rotates and scales: 256 colours through the standard
 * palette (colour 0 transparent), or direct colour, where "the upper bit
 * (Bit15) is used as Alpha flag. That is, Alpha=0=Transparent". "Unlike GBA
 * bitmap modes, NDS bitmap modes are supporting the Area Overflow bit"
 * (GBATEK "DS Video BG Modes / Control").
 */
static void render_bitmap_bg(const Nds2d *e, const Scene *s, uint32_t bg, uint32_t base, uint32_t width,
                             uint32_t height, bool direct, uint32_t *out, const uint8_t **direct_row)
{
    const uint32_t cnt = reg16(e, REG_BG0CNT + 2u * bg);
    const bool wrap = (cnt & 0x2000u) != 0;
    const uint32_t tag = PIX_BG0 << bg;
    Walk w = walk_of(e, bg, cnt);

    /* A line of direct colour, a texel a pixel, 256 of them in a row in
     * video memory: say where (Nds2dLayers.direct_row). */
    *direct_row = NULL;
    if (direct && w.dx == 0x100u && w.dy == 0 && !(cnt & 0x40u)) {
        uint32_t tx = w.x >> 8, ty = w.y >> 8;
        if (wrap) {
            tx &= width - 1u;
            ty &= height - 1u;
        }
        if (tx < width && ty < height && tx + W <= width) {
            const uint32_t addr = (base + (ty * width + tx) * 2u) & s->bg_mask & ~1u;
            if ((addr & 0x3FFFu) + W * 2u <= 0x4000u) *direct_row = &s->bg[addr >> 14][addr & 0x3FFFu];
        }
    }

    for (uint32_t x = 0; x < W; x++, w.x += w.dx, w.y += w.dy) {
        uint32_t tx = w.x >> 8, ty = w.y >> 8;
        if (wrap) {
            tx &= width - 1u;
            ty &= height - 1u;
        } else if (tx >= width || ty >= height) {
            out[x] = 0;
            continue;
        }
        const uint32_t texel = ty * width + tx;
        if (direct) {
            const uint32_t color = bg16(s, base + texel * 2u);
            out[x] = (color & 0x8000u) ? rgb18(color) | tag : 0;
        } else {
            const uint32_t index = bg8(s, base + texel);
            out[x] = index ? pal18(s->palette, index) | tag : 0;
        }
    }
    if (cnt & 0x40u) mosaic_h(out, (reg16(e, REG_MOSAIC) & 15u) + 1u);
}

/*
 * An "extended" background (BG modes 3-5), "sub-selected by BGxCNT bits":
 *
 *   bit 7  bit 2
 *   0      -      rot/scal with 16bit bgmap entries
 *   1      0      rot/scal 256 color bitmap
 *   1      1      rot/scal direct color bitmap
 *
 * The bitmaps are 128x128, 256x256, 512x256 or 512x512, and start at the
 * screen base field times 16K, "without DISPCNT.bits*64K".
 */
static void render_extended_bg(const Nds2d *e, const Scene *s, uint32_t bg, uint32_t *out,
                               const uint8_t **direct_row)
{
    static const uint16_t k_size[4][2] = { { 128, 128 }, { 256, 256 }, { 512, 256 }, { 512, 512 } };
    const uint32_t cnt = reg16(e, REG_BG0CNT + 2u * bg);
    *direct_row = NULL;
    if (!(cnt & 0x80u)) {
        render_affine_bg(e, s, bg, true, out);
        return;
    }
    const uint32_t size = (cnt >> 14) & 3u;
    render_bitmap_bg(e, s, bg, ((cnt >> 8) & 31u) * 0x4000u, k_size[size][0], k_size[size][1], (cnt & 4u) != 0, out,
                     direct_row);
}

/*
 * The large bitmap of BG mode 6: BG2, 256 colours, 512x1024 or 1024x512,
 * "using all 512K of 2D VRAM"; "screen base however NOT used at all".
 * GBATEK leaves sizes 2 and 3 blank; melonDS makes them 512x256 and 512x512.
 */
static void render_large_bg(const Nds2d *e, const Scene *s, uint32_t *out)
{
    static const uint16_t k_size[4][2] = { { 512, 1024 }, { 1024, 512 }, { 512, 256 }, { 512, 512 } };
    const uint32_t size = (reg16(e, REG_BG0CNT + 4u) >> 14) & 3u;
    const uint8_t *none;
    render_bitmap_bg(e, s, 2, 0, k_size[size][0], k_size[size][1], false, out, &none);
}

/*
 * The 3D engine's picture, as it reaches the 2D engine: "the scroll region
 * is 512 pixels, consisting of 256 pixels for the 3D image, followed by 256
 * transparent pixels, and then wrapped to the 3D image again", moved by
 * BG0HOFS; "vertical scrolling (and rotation/scaling) cannot be used"
 * (GBATEK "DS 3D Final 2D Output"). The capture unit's "3D screen" input is
 * scrolled with it, as in melonDS. Returns the line to use.
 */
static const uint32_t *scroll_3d(const Nds2d *e, const uint32_t *line, uint32_t *scrolled)
{
    const uint32_t hofs = reg16(e, REG_BG0HOFS) & 0x1FFu;
    if (!hofs) return line;
    for (uint32_t x = 0; x < W; x++) {
        const uint32_t from = (x + hofs) & 0x1FFu;
        scrolled[x] = from < W ? line[from] : 0;
    }
    return scrolled;
}

/* The 3D layer as BG0. A pixel with alpha 0 has nothing in it; "mosaic
 * cannot be used on the 3D layer". */
static void render_3d_bg(const uint32_t *line_3d, uint32_t *out)
{
    for (uint32_t x = 0; x < W; x++) {
        const uint32_t alpha = (line_3d[x] >> 18) & 31u;
        out[x] = alpha ? (line_3d[x] & PIX_COLOR) | PIX_BG0 | PIX_3D | (alpha << PIX_ALPHA_SHIFT) : 0;
    }
}

/* ------------------------------------------------------------------ */
/* Sprites                                                             */
/* ------------------------------------------------------------------ */

/* Width and height by shape and size (GBATEK "LCD OBJ - OAM Attributes").
 * Shape 3 is "prohibited"; melonDS draws it 8x8. */
static const uint8_t k_obj_size[4][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } }, /* square */
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } }, /* horizontal */
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } }, /* vertical */
    { { 8, 8 }, { 8, 8 }, { 8, 8 }, { 8, 8 } },
};

#define OBJ_TOUCHED 1u /* a sprite's rectangle covers the pixel */
#define OBJ_MOSAIC 2u  /* and that sprite has the mosaic attribute */

typedef struct {
    uint32_t pix[W];   /* the frontmost sprite pixel, or 0 */
    uint8_t prio[W];   /* its priority relative to the backgrounds */
    uint8_t flags[W];  /* OBJ_* */
    uint8_t window[W]; /* inside an OBJ-window sprite */
    bool any_blend;    /* some pixel is semi-transparent or a bitmap pixel */
    /* Where a pixel of pix[] lies in video memory, when it is a pixel of a
     * bitmap sprite drawn as it is stored (Nds2dLayers.obj_src). */
    const uint8_t *src[W];
} ObjLine;

/* Where a sprite's picture comes from. */
enum { OBJ_TILE4, OBJ_TILE8, OBJ_BITMAP };

typedef struct {
    uint32_t kind;
    uint32_t base;          /* tiles: the first tile, in 32-byte units; bitmap: its byte address */
    uint32_t row;           /* tiles: units from one row of tiles to the next; bitmap: pixels per row */
    const uint8_t *palette; /* tiles */
} ObjSource;

/* The texel at (tx, ty) of a sprite's picture: a 15-bit colour with bit 15
 * set, or 0 where it is transparent. */
static inline uint32_t obj_texel(const Scene *s, const ObjSource *src, uint32_t tx, uint32_t ty)
{
    if (src->kind == OBJ_BITMAP) {
        const uint32_t color = obj16(s, src->base + (ty * src->row + tx) * 2u);
        return (color & 0x8000u) ? color : 0;
    }
    const uint32_t unit = src->base + (ty >> 3) * src->row;
    uint32_t index;
    if (src->kind == OBJ_TILE8) {
        index = obj8(s, (unit + (tx >> 3) * 2u) * 32u + (ty & 7u) * 8u + (tx & 7u));
    } else {
        const uint32_t byte = obj8(s, (unit + (tx >> 3)) * 32u + (ty & 7u) * 4u + ((tx & 7u) >> 1));
        index = (tx & 1u) ? byte >> 4 : byte & 15u;
    }
    return index ? nds_get_le16(src->palette + index * 2u) | 0x8000u : 0;
}

static void draw_sprite(const Nds2d *e, const Scene *s, uint32_t number, ObjLine *out)
{
    const uint8_t *oam = s->oam + number * 8u;
    const uint32_t a0 = nds_get_le16(oam), a1 = nds_get_le16(oam + 2), a2 = nds_get_le16(oam + 4);
    const bool affine = (a0 & 0x100u) != 0;
    const uint32_t dbl = (affine && (a0 & 0x200u)) ? 1u : 0u;
    const uint32_t mode = (a0 >> 10) & 3u; /* 0 normal, 1 semi-transparent, 2 window, 3 bitmap */
    const uint32_t w = k_obj_size[a0 >> 14][a1 >> 14][0], h = k_obj_size[a0 >> 14][a1 >> 14][1];
    const uint32_t bw = w << dbl, bh = h << dbl; /* the rectangle on screen */

    /* The Y coordinate wraps at 256, and "on the NDS, the OBJ appears BOTH
     * at the top and bottom of the screen" (GBATEK "DS Video OBJs", OBJ
     * Vertical Wrap). */
    const uint32_t oy = a0 & 0xFFu;
    uint32_t dy = (s->y - oy) & 0xFFu;
    if (dy >= bh) return;
    int ox = (int)(a1 & 0x1FFu);
    if (ox >= W) ox -= 512;
    if (ox + (int)bw <= 0) return;

    /* The window has no mosaic (melonDS). A mosaic sprite shows the row of
     * the mosaic block's first line; where that line is above the sprite,
     * this keeps the row of the line itself, as the GBA renderer does. */
    const bool mosaic = (a0 & 0x1000u) && mode != 2;
    if (mosaic) {
        const uint32_t block_dy = (e->obj_mosaic_line - oy) & 0xFFu;
        if (block_dy < bh) dy = block_dy;
    }

    const uint32_t tile = a2 & 0x3FFu;
    uint32_t tag = PIX_OBJ;
    ObjSource src;
    if (mode == 3) {
        /*
         * A bitmap sprite: 15-bit pixels with bit 15 as "alpha flag", and
         * attribute 2's palette field "used as Alpha-OAM value" (GBATEK "DS
         * Video OBJs"). How the value is used is known from melonDS only:
         * 0 hides the sprite, and otherwise it blends as a semi-transparent
         * one does, with EVA = value + 1 and EVB = 16 - EVA.
         *
         *   Bit6 Bit5 Bit22 Dimension    Boundary
         *   0    0    x     2D/128 dots  8x8 dots
         *   0    1    x     2D/256 dots  8x8 dots
         *   1    0    0     1D           128 bytes
         *   1    0    1     1D           256 bytes
         *   1    1    x     Reserved (nothing is drawn; melonDS)
         */
        const uint32_t alpha = a2 >> 12;
        if (!alpha) return;
        tag |= PIX_BITMAP | ((alpha + 1u) << PIX_ALPHA_SHIFT);
        src.kind = OBJ_BITMAP;
        src.palette = NULL;
        if (s->dispcnt & 0x40u) {
            if (s->dispcnt & 0x20u) return;
            src.base = tile * (128u << ((s->dispcnt >> 22) & 1u));
            src.row = w; /* "Source Width = Target Width" */
        } else {
            /* "2D_BitmapVramAddress = (TileNo AND MaskX)*10h + (TileNo AND NOT MaskX)*80h" */
            const uint32_t mask_x = (s->dispcnt & 0x20u) ? 0x1Fu : 0x0Fu;
            src.base = (tile & mask_x) * 0x10u + (tile & ~mask_x) * 0x80u;
            src.row = (s->dispcnt & 0x20u) ? 256u : 128u;
        }
    } else {
        /*
         * A sprite of 8x8 tiles, 32 bytes each with 16 colours and 64 with
         * 256 (GBATEK "LCD OBJ - VRAM Character (Tile) Mapping", "DS Video
         * OBJs"). Two-dimensional mapping (DISPCNT bit 4 clear) sees a
         * matrix 32 tiles wide. One-dimensional mapping lays the sprite's
         * tiles end to end from "TileNumber * BoundaryValue", the boundary
         * being 32 bytes shifted by DISPCNT bits 20-21.
         */
        const bool bpp8 = (a0 & 0x2000u) != 0;
        src.kind = bpp8 ? OBJ_TILE8 : OBJ_TILE4;
        if (s->dispcnt & 0x10u) {
            src.base = tile << ((s->dispcnt >> 20) & 3u);
            src.row = (w >> 3) << (bpp8 ? 1 : 0);
        } else {
            /* 256 colours: "in 2-dimensional mapping mode, the [lower] bit
             * is completely ignored". */
            src.base = bpp8 ? tile & ~1u : tile;
            src.row = 32;
        }
        /* 256 colours x 16 palettes come from the extended palette when
         * DISPCNT bit 31 is set (GBATEK "DS Video Extended Palettes"). */
        if (!bpp8) src.palette = s->palette + 0x200u + (a2 >> 12) * 32u;
        else if (s->dispcnt & (1u << 31)) src.palette = s->obj_ext_pal + (a2 >> 12) * 512u;
        else src.palette = s->palette + 0x200u;
        if (mode == 1) tag |= PIX_SEMI;
    }

    const uint8_t prio = (uint8_t)((a2 >> 10) & 3u);
    const uint8_t flags = (uint8_t)(OBJ_TOUCHED | (mosaic ? OBJ_MOSAIC : 0u));
    const int first = ox < 0 ? -ox : 0;
    const int last = ox + (int)bw > W ? W - ox : (int)bw;

    /* Rotation and scaling is about the rectangle's centre, onto the
     * picture's centre (GBATEK "LCD OBJ - OAM Rotation/Scaling
     * Parameters"). Texture coordinates are 8.8 fixed point, unsigned, so
     * that one comparison rejects both sides. */
    uint32_t ux = 0, uy = 0, dux = 0, duy = 0;
    if (affine) {
        const uint8_t *group = s->oam + ((a1 >> 9) & 31u) * 32u;
        const int32_t pa = s16(nds_get_le16(group + 6)), pb = s16(nds_get_le16(group + 14));
        const int32_t pc = s16(nds_get_le16(group + 22)), pd = s16(nds_get_le16(group + 30));
        const int32_t cx = first - (int32_t)(bw >> 1), cy = (int32_t)dy - (int32_t)(bh >> 1);
        ux = (uint32_t)(pa * cx + pb * cy + (int32_t)(w << 7));
        uy = (uint32_t)(pc * cx + pd * cy + (int32_t)(h << 7));
        dux = (uint32_t)pa;
        duy = (uint32_t)pc;
    }
    const uint32_t row = (a1 & 0x2000u) ? h - 1u - dy : dy; /* unused by affine sprites, whose dy can exceed h */
    /* A bitmap sprite shown as it is stored: its pixels can be found again. */
    const bool stored = mode == 3 && !affine && !(a1 & 0x3000u) && !mosaic;

    for (int sx = first; sx < last; sx++) {
        const int x = ox + sx;
        uint32_t tx, ty;
        if (affine) {
            tx = ux >> 8;
            ty = uy >> 8;
            ux += dux;
            uy += duy;
            if (tx >= w || ty >= h) continue;
        } else {
            tx = (a1 & 0x1000u) ? w - 1u - (uint32_t)sx : (uint32_t)sx;
            ty = row;
        }
        const uint32_t color = obj_texel(s, &src, tx, ty);
        if (mode == 2) {
            /* "Any non-transparent dots of any such OBJs are marked as OBJ
             * Window area. The OBJ itself is not displayed." */
            if (color) out->window[x] = 1;
        } else if (color) {
            out->pix[x] = rgb18(color) | tag;
            out->prio[x] = prio;
            out->flags[x] = flags;
            out->src[x] = NULL;
            if (stored) {
                const uint32_t addr = (src.base + (ty * src.row + tx) * 2u) & s->obj_mask & ~1u;
                out->src[x] = &s->obj[addr >> 14][addr & 0x3FFFu];
            }
        } else if (!out->flags[x]) {
            /* A transparent pixel of the first sprite to cover this place
             * still says whether the place takes part in the mosaic. */
            out->flags[x] = flags;
        }
    }
    if (mode != 2 && (tag & (PIX_SEMI | PIX_BITMAP))) out->any_blend = true;
}

/*
 * The sprite layer of one line: for each pixel the frontmost sprite pixel.
 *
 * Which is frontmost: "The GBA has been assigning OBJ priority in respect
 * to the 7bit OAM entry number, regardless of the OBJs 2bit BG-priority
 * attribute [...] That problem has been fixed in DS mode by combining the
 * above two values into a 9bit priority value" (GBATEK "DS Video OBJs").
 * So sprites are painted from the back: priority 3 to 0, and within one
 * priority OAM entry 127 to 0.
 */
static void render_sprites(const Nds2d *e, const Scene *s, ObjLine *out)
{
    memset(out, 0, sizeof *out);
    if (!s->oam) return;

    uint8_t order[4][128];
    uint32_t count[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < 128; i++) {
        const uint32_t a0 = nds_get_le16(s->oam + i * 8u);
        if ((a0 & 0x300u) == 0x200u) continue; /* "OBJ Disable" of a sprite that does not rotate */
        const uint32_t prio = (nds_get_le16(s->oam + i * 8u + 4u) >> 10) & 3u;
        order[prio][count[prio]] = (uint8_t)i;
        count[prio]++;
    }
    for (uint32_t p = 4; p-- > 0;) {
        for (uint32_t k = count[p]; k-- > 0;) draw_sprite(e, s, order[p][k], out);
    }

    /*
     * Horizontal mosaic, after all sprites are in (the rule is melonDS's):
     * going right, the engine holds on to the last pixel it let through,
     * and inside a mosaic block repeats it in place of the next one, as
     * long as both are pixels of mosaic sprites. A pixel that starts a
     * block, or either one not being mosaic, lets the new pixel through.
     */
    const uint32_t size = ((reg16(e, REG_MOSAIC) >> 8) & 15u) + 1u;
    if (size > 1) {
        uint32_t held = 0, phase = 0;
        for (uint32_t x = 1; x < W; x++) {
            if (++phase == size) phase = 0;
            if (phase && (out->flags[held] & out->flags[x] & OBJ_MOSAIC)) {
                out->pix[x] = out->pix[held];
                out->prio[x] = out->prio[held];
                out->src[x] = NULL;
            } else {
                held = x;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Composition                                                         */
/* ------------------------------------------------------------------ */

/*
 * Which layers show at each pixel of the line (bits 0-3 BG0-BG3, bit 4
 * OBJ) and whether colour special effects do (bit 5): WININ and WINOUT
 * (GBATEK "LCD I/O Window Feature"). "Window 0 is having highest priority,
 * Window 1 medium, and Obj Window lowest priority. Outside of Window is
 * having zero priority". With no window enabled everything shows.
 */
static void window_mask(const Nds2d *e, uint32_t dispcnt, const uint8_t *obj_window, uint8_t *enabled)
{
    if (!(dispcnt & 0xE000u)) {
        memset(enabled, 0x3F, W);
        return;
    }
    const uint32_t winin = reg16(e, REG_WININ), winout = reg16(e, REG_WINOUT);
    memset(enabled, (int)(winout & 0x3Fu), W);
    if (dispcnt & 0x8000u) {
        const uint8_t inside = (uint8_t)((winout >> 8) & 0x3Fu);
        for (uint32_t x = 0; x < W; x++) {
            if (obj_window[x]) enabled[x] = inside;
        }
    }
    for (uint32_t i = 2; i-- > 0;) {
        if (!(dispcnt & (0x2000u << i)) || !e->win_v[i]) continue;
        const uint32_t h = reg16(e, REG_WIN0H + 2u * i);
        const uint32_t x1 = h >> 8, x2 = h & 0xFFu;
        const int inside = (int)((winin >> (8u * i)) & 0x3Fu);
        /* The horizontal flip-flop: still set from the line before until it
         * meets X2, set from X1 on. See window_line_start(). */
        if (e->win_h[i]) memset(enabled, inside, x2);
        if (x1 < x2) memset(enabled + x1, inside, x2 - x1);
        else if (x1 > x2) memset(enabled + x1, inside, W - x1);
    }
}

/*
 * Colour special effects (GBATEK "LCD I/O Color Special Effects"), on 6-bit
 * channels. GBATEK gives the formulas; that the DS computes them on the
 * 6-bit colour and how each one rounds is known from melonDS:
 *
 *   alpha blending        I = MIN(63, (I1st*EVA + I2nd*EVB + 8) / 16)
 *   brightness increase   I = I1st + ((63-I1st)*EVY + 8) / 16
 *   brightness decrease   I = I1st - (I1st*EVY + 7) / 16
 *   3D over a 2nd target  I = (I1st*(A+1) + I2nd*(31-A) + 16) / 32
 */
static inline uint32_t blend_16(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb)
{
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 18; shift += 6) {
        const uint32_t c = (((a >> shift) & 63u) * eva + ((b >> shift) & 63u) * evb + 8u) >> 4;
        out |= (c > 63u ? 63u : c) << shift;
    }
    return out;
}

static inline uint32_t blend_32(uint32_t a, uint32_t b, uint32_t eva)
{
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 18; shift += 6) {
        out |= ((((a >> shift) & 63u) * eva + ((b >> shift) & 63u) * (32u - eva) + 16u) >> 5) << shift;
    }
    return out;
}

/* A brightness change is the same function of every channel: tabulate it
 * for the line, then look the three channels up. */
static inline uint32_t map_channels(const uint8_t *table, uint32_t c)
{
    return (uint32_t)table[c & 63u] | ((uint32_t)table[(c >> 6) & 63u] << 6) | ((uint32_t)table[(c >> 12) & 63u] << 12);
}

static void brightness_table(uint8_t *table, bool up, uint32_t factor, uint32_t bias)
{
    for (uint32_t c = 0; c < 64; c++) {
        table[c] = (uint8_t)(up ? c + (((63u - c) * factor + bias) >> 4) : c - ((c * factor + bias) >> 4));
    }
}

/* Which kind of background each BG is in each BG mode (GBATEK "DS Video BG
 * Modes / Control"). Mode 7 is reserved; melonDS shows BG0 and BG1 as text
 * there. BG0 is the 3D layer instead when DISPCNT bit 3 says so. */
enum { BG_NONE, BG_TEXT, BG_AFFINE, BG_EXTENDED, BG_LARGE };

static const uint8_t k_bg_kind[8][4] = {
    { BG_TEXT, BG_TEXT, BG_TEXT, BG_TEXT },
    { BG_TEXT, BG_TEXT, BG_TEXT, BG_AFFINE },
    { BG_TEXT, BG_TEXT, BG_AFFINE, BG_AFFINE },
    { BG_TEXT, BG_TEXT, BG_TEXT, BG_EXTENDED },
    { BG_TEXT, BG_TEXT, BG_AFFINE, BG_EXTENDED },
    { BG_TEXT, BG_TEXT, BG_EXTENDED, BG_EXTENDED },
    { BG_NONE, BG_NONE, BG_LARGE, BG_NONE },
    { BG_TEXT, BG_TEXT, BG_NONE, BG_NONE },
};

/* The colour effects of a line (BLDCNT, BLDALPHA, BLDY), ready to apply to
 * the two frontmost layers of a pixel. */
typedef struct Effects {
    uint32_t effect;           /* BLDCNT bits 6-7 */
    uint32_t eva, evb;
    uint32_t first_targets, second_targets;
    uint8_t bright[64];
} Effects;

static void effects_init(Effects *fx, uint32_t bldcnt, uint32_t bldalpha, uint32_t bldy)
{
    fx->effect = (bldcnt >> 6) & 3u;
    fx->eva = bldalpha & 31u;
    fx->evb = (bldalpha >> 8) & 31u;
    uint32_t evy = bldy & 31u;
    if (fx->eva > 16) fx->eva = 16; /* "0..16 = 0/16..16/16, 17..31=16/16" */
    if (fx->evb > 16) fx->evb = 16;
    if (evy > 16) evy = 16;
    if (fx->effect >= 2) brightness_table(fx->bright, fx->effect == 2, evy, fx->effect == 2 ? 8u : 7u);
    fx->first_targets = bldcnt & 0x3Fu;
    fx->second_targets = (bldcnt >> 8) & 0x3Fu;
}

/* One pixel: `a` in front of `b`, where the windows allow `enabled`. */
static inline uint32_t apply_effects(const Effects *fx, uint32_t a, uint32_t b, uint8_t enabled)
{
    const bool over_target = ((b >> PIX_LAYER_SHIFT) & fx->second_targets) != 0;
    uint32_t color = a & PIX_COLOR;
    if ((a & (PIX_SEMI | PIX_BITMAP)) && over_target) {
        /* "OBJs that are defined as 'Semi-Transparent' in OAM memory
         * are always selected as 1st Target (regardless of BLDCNT Bit
         * 4), and are always using Alpha Blending mode (regardless of
         * BLDCNT Bit 6-7)", and regardless of the window's effect bit.
         * A bitmap sprite does the same with its own coefficients. */
        if (a & PIX_BITMAP) {
            const uint32_t own = (a >> PIX_ALPHA_SHIFT) & 31u;
            color = blend_16(a, b, own, 16u - own);
        } else {
            color = blend_16(a, b, fx->eva, fx->evb);
        }
    } else if ((a & PIX_3D) && over_target) {
        /* "Blending with BG0 as 1st Target via 3D Alpha-values (unlike
         * as for 2D)", "regardless of the Window Control register's
         * color effect enable flag" (GBATEK "DS 3D Final 2D Output").
         * As melonDS has it, this needs neither BG0 as 1st target nor
         * the blending effect selected, and the weights are A+1 and
         * 31-A out of 32. */
        color = blend_32(a, b, ((a >> PIX_ALPHA_SHIFT) & 31u) + 1u);
    } else if (((a >> PIX_LAYER_SHIFT) & fx->first_targets) && (enabled & 0x20u)) {
        if (fx->effect == 1) {
            if (over_target) color = blend_16(a, b, fx->eva, fx->evb);
        } else if (fx->effect >= 2) {
            color = map_channels(fx->bright, color);
        }
    }
    return color;
}

/* A pixel of the 3D engine's picture as BG0's. */
static inline uint32_t pixel_from_3d(uint32_t c)
{
    const uint32_t alpha = (c >> 18) & 31u;
    return alpha ? (c & PIX_COLOR) | PIX_BG0 | PIX_3D | (alpha << PIX_ALPHA_SHIFT) : 0;
}

/*
 * The picture of the "Layering and Special Effects" box of GBATEK's "DS
 * Video Display System Block Diagram" for one line: what display mode 1
 * shows and what the capture unit takes as its graphics input. 18-bit
 * colours, before master brightness.
 *
 * The layers it stacked are left in `l` (all but the 3D picture, which the
 * caller has), with l->composed clear when the line is forced blank.
 */
static void compose(const Nds2d *e, const Nds2dVram *vram, int y, const uint32_t *line_3d, uint32_t *dst,
                    Nds2dLayers *l)
{
    Scene s;
    scene_init(&s, e, vram, y);
    const uint32_t dispcnt = s.dispcnt;

    l->composed = 0;
    l->has_3d = 0;
    for (int i = 0; i < 4; i++) l->direct_row[i] = NULL;
    /* "Setting Forced Blank (Bit 7) causes the video controller to display
     * white lines". */
    if (dispcnt & 0x80u) {
        for (uint32_t x = 0; x < W; x++) dst[x] = WHITE;
        return;
    }
    l->composed = 1;

    const uint32_t *bg_line[4] = { l->bg[0], l->bg[1], l->bg[2], l->bg[3] };
    bool bg_on[4] = { false, false, false, false };
    const uint32_t mode = dispcnt & 7u;
    for (uint32_t bg = 0; bg < 4; bg++) {
        if (!(dispcnt & (0x100u << bg))) continue;
        if (bg == 0 && !e->engine && (dispcnt & 8u)) {
            if (line_3d) {
                render_3d_bg(line_3d, l->bg[0]);
                bg_on[0] = true;
                l->has_3d = 1;
            }
            continue;
        }
        switch (k_bg_kind[mode][bg]) {
        case BG_TEXT:
            render_text_bg(e, &s, bg, l->bg[bg]);
            break;
        case BG_AFFINE:
            render_affine_bg(e, &s, bg, false, l->bg[bg]);
            break;
        case BG_EXTENDED:
            render_extended_bg(e, &s, bg, l->bg[bg], &l->direct_row[bg]);
            break;
        case BG_LARGE:
            /* "Engine B: [...] Mode 6 is reserved (no Large screen bitmap)". */
            if (e->engine) continue;
            render_large_bg(e, &s, l->bg[bg]);
            break;
        default:
            continue;
        }
        bg_on[bg] = true;
    }

    /* "Both DISPCNT Bits 12 and 15 must be set when defining OBJ Window
     * region(s)": without the OBJ layer there are no sprites of any kind. */
    ObjLine obj;
    const bool obj_on = (dispcnt & 0x1000u) != 0;
    if (obj_on) render_sprites(e, &s, &obj);
    else memset(obj.window, 0, sizeof obj.window);

    uint8_t *enabled = l->enabled;
    window_mask(e, dispcnt, obj.window, enabled);

    /* The enabled BGs in drawing order: by priority, lower-numbered first
     * ("In case that some or all BGs are set to same priority then BG0 is
     * having the highest, and BG3 the lowest priority"). */
    uint32_t order[4], layers = 0;
    uint8_t order_prio[4];
    for (uint32_t p = 0; p < 4; p++) {
        for (uint32_t bg = 0; bg < 4; bg++) {
            if (bg_on[bg] && (reg16(e, REG_BG0CNT + 2u * bg) & 3u) == p) {
                order[layers] = bg;
                order_prio[layers] = (uint8_t)p;
                layers++;
            }
        }
    }

    const uint32_t backdrop = pal18(s.palette, 0) | PIX_BD;
    const uint32_t bldcnt = reg16(e, REG_BLDCNT);
    const uint32_t bldalpha = reg16(e, REG_BLDALPHA);
    const uint32_t bldy = reg16(e, REG_BLDY);

    /* What a second stacking of this line needs (nds_2d_stack_scaled). */
    l->obj_on = obj_on ? 1 : 0;
    l->layers = (uint8_t)layers;
    for (uint32_t k = 0; k < layers; k++) {
        l->order[k] = (uint8_t)order[k];
        l->order_prio[k] = order_prio[k];
    }
    l->hofs = (uint16_t)(reg16(e, REG_BG0HOFS) & 0x1FFu);
    l->bldcnt = (uint16_t)bldcnt;
    l->bldalpha = (uint16_t)bldalpha;
    l->bldy = (uint16_t)bldy;
    l->backdrop = backdrop;
    if (obj_on) {
        memcpy(l->obj, obj.pix, sizeof l->obj);
        memcpy(l->obj_prio, obj.prio, sizeof l->obj_prio);
        for (uint32_t x = 0; x < W; x++) l->obj_src[x] = obj.src[x];
    }

    /* The top two layers of each pixel, the backdrop behind all ("Color 0
     * of BG Palette 0 is used as backdrop color"): paint the BGs back to
     * front, keeping what each new pixel covers as the second layer. */
    uint32_t top[W], second[W];
    uint8_t top_prio[W], second_prio[W];
    for (uint32_t x = 0; x < W; x++) {
        top[x] = second[x] = backdrop;
        top_prio[x] = second_prio[x] = 4;
    }
    for (uint32_t k = layers; k-- > 0;) {
        const uint32_t *src = bg_line[order[k]];
        const uint8_t bit = (uint8_t)(1u << order[k]), prio = order_prio[k];
        for (uint32_t x = 0; x < W; x++) {
            if (!src[x] || !(enabled[x] & bit)) continue;
            second[x] = top[x];
            second_prio[x] = top_prio[x];
            top[x] = src[x];
            top_prio[x] = prio;
        }
    }
    /* A sprite pixel goes in front of the BGs of its own priority and
     * lower, behind those of a higher one. */
    if (obj_on) {
        for (uint32_t x = 0; x < W; x++) {
            if (!obj.pix[x] || !(enabled[x] & 0x10u)) continue;
            if (obj.prio[x] <= top_prio[x]) {
                second[x] = top[x];
                top[x] = obj.pix[x];
            } else if (obj.prio[x] <= second_prio[x]) {
                second[x] = obj.pix[x];
            }
        }
    }

    const uint32_t effect = (bldcnt >> 6) & 3u;
    if (effect == 0 && !(obj_on && obj.any_blend) && !(bg_on[0] && !e->engine && (dispcnt & 8u))) {
        for (uint32_t x = 0; x < W; x++) dst[x] = top[x] & PIX_COLOR;
        return;
    }

    Effects fx;
    effects_init(&fx, bldcnt, bldalpha, bldy);
    for (uint32_t x = 0; x < W; x++) dst[x] = apply_effects(&fx, top[x], second[x], enabled[x]);
}

void nds_2d_stack_scaled(const Nds2dLayers *l, int layer, const uint32_t *const *finer, size_t finer_pitch,
                         int scale, uint32_t *out, size_t pitch)
{
    if (!l || !finer || !out || scale < 1 || layer < 0 || layer > NDS_2D_LAYER_OBJ) return;
    const uint32_t n = (uint32_t)scale;
    const bool sprites = layer == NDS_2D_LAYER_OBJ;
    /* A background: the 3D picture, or a bitmap's line. */
    const bool is_3d = layer == 0 && l->has_3d;
    const uint32_t tag = sprites ? 0u : PIX_BG0 << layer;
    const uint8_t bit = sprites ? 0x10u : (uint8_t)(1u << layer);

    /* Where the finer background stands among the others: those drawn
     * after it are in front of it. With the sprites finer, every
     * background is the screen's own and none is. */
    uint32_t at = l->layers;
    for (uint32_t k = 0; k < l->layers && !sprites; k++) {
        if (l->order[k] == (uint8_t)layer) at = k;
    }
    const uint8_t prio_mine = at < l->layers ? l->order_prio[at] : 4;
    /* A layer that is switched off has nothing finer either. */
    const bool shown = sprites ? l->obj_on != 0 : at < l->layers;

    /* The two frontmost of the screen's own backgrounds in front of it at
     * each pixel, and the two frontmost behind it: whatever its finer
     * pixels turn out to be, the top two layers are among these and it. */
    uint32_t front[2][W], behind[2][W];
    uint8_t front_prio[2][W], behind_prio[2][W];
    for (uint32_t x = 0; x < W; x++) {
        front[0][x] = front[1][x] = 0;
        front_prio[0][x] = front_prio[1][x] = 4;
        behind[0][x] = behind[1][x] = l->backdrop;
        behind_prio[0][x] = behind_prio[1][x] = 4;
    }
    for (uint32_t k = l->layers; k-- > 0;) {
        if (k == at) continue;
        const uint32_t bg = l->order[k];
        const uint32_t *src = l->bg[bg];
        const uint8_t other = (uint8_t)(1u << bg), prio = l->order_prio[k];
        uint32_t (*pix)[W] = k < at ? front : behind;
        uint8_t (*pri)[W] = k < at ? front_prio : behind_prio;
        for (uint32_t x = 0; x < W; x++) {
            if (!src[x] || !(l->enabled[x] & other)) continue;
            pix[1][x] = pix[0][x];
            pri[1][x] = pri[0][x];
            pix[0][x] = src[x];
            pri[0][x] = prio;
        }
    }

    Effects fx;
    effects_init(&fx, l->bldcnt, l->bldalpha, l->bldy);

    for (uint32_t x = 0; x < W; x++) {
        const uint8_t enabled = l->enabled[x];
        const uint32_t *fine = shown && (enabled & bit) ? finer[x] : NULL;
        /* The screen's own pixel of each layer that can be finer. */
        const uint32_t own_bg = !sprites && at < l->layers && (enabled & bit) ? l->bg[layer][x] : 0;
        const uint32_t own_obj = l->obj_on && (enabled & 0x10u) ? l->obj[x] : 0;
        const uint8_t obj_prio = l->obj_prio[x];
        for (uint32_t j = 0; j < n; j++) {
            uint32_t *dst = out + (size_t)j * pitch + (size_t)x * n;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t mine = own_bg, obj = own_obj;
                if (fine) {
                    const uint32_t c = fine[(size_t)j * finer_pitch + i];
                    const bool there = ((c >> 18) & 31u) != 0;
                    if (sprites) obj = there && own_obj ? (own_obj & ~PIX_COLOR) | (c & PIX_COLOR) : 0;
                    else if (is_3d) mine = pixel_from_3d(c);
                    else mine = there ? (c & PIX_COLOR) | tag : 0;
                }
                /* The top two, front to back: the backgrounds in front,
                 * this pixel of the finer one, the backgrounds behind. */
                uint32_t top, second;
                uint8_t top_prio, second_prio;
                if (front[0][x]) {
                    top = front[0][x];
                    top_prio = front_prio[0][x];
                    if (front[1][x]) {
                        second = front[1][x];
                        second_prio = front_prio[1][x];
                    } else if (mine) {
                        second = mine;
                        second_prio = prio_mine;
                    } else {
                        second = behind[0][x];
                        second_prio = behind_prio[0][x];
                    }
                } else if (mine) {
                    top = mine;
                    top_prio = prio_mine;
                    second = behind[0][x];
                    second_prio = behind_prio[0][x];
                } else {
                    top = behind[0][x];
                    top_prio = behind_prio[0][x];
                    second = behind[1][x];
                    second_prio = behind_prio[1][x];
                }
                if (obj) {
                    if (obj_prio <= top_prio) {
                        second = top;
                        top = obj;
                    } else if (obj_prio <= second_prio) {
                        second = obj;
                    }
                }
                dst[i] = apply_effects(&fx, top, second, enabled);
            }
        }
    }
}

void nds_2d_brighten_scaled(const Nds2dLayers *l, uint32_t *pixels, size_t count)
{
    /* MASTER_BRIGHT, as master_brightness() applies it. */
    const uint32_t mode = l->master_bright >> 14;
    uint32_t factor = l->master_bright & 31u;
    if (factor > 16) factor = 16;
    if ((mode != 1 && mode != 2) || !factor) return;
    uint8_t table[64];
    brightness_table(table, mode == 1, factor, mode == 1 ? 0u : 15u);
    for (size_t i = 0; i < count; i++) pixels[i] = map_channels(table, pixels[i]);
}

void nds_2d_capture_scaled(const uint32_t *in, size_t count, int from_3d, uint32_t *out)
{
    for (size_t i = 0; i < count; i++) {
        const uint32_t there = !from_3d || ((in[i] >> 18) & 31u) ? 31u << 18 : 0u;
        out[i] = rgb18(rgb15(in[i])) | there;
    }
}

/* ------------------------------------------------------------------ */
/* Display capture                                                     */
/* ------------------------------------------------------------------ */

static const uint16_t k_capture_size[4][2] = { { 128, 128 }, { 256, 64 }, { 256, 128 }, { 256, 192 } };

/*
 * "Capture A+B (blending):
 *   Dest_Intensity = ( (SrcA_Intensitity * SrcA_Alpha * EVA)
 *                    + (SrcB_Intensitity * SrcB_Alpha * EVB) ) / 16
 *   Dest_Alpha = (SrcA_Alpha AND (EVA>0)) OR (SrcB_Alpha AND EVB>0))"
 * on 5-bit channels. melonDS rounds the division to nearest (adds 8) and
 * limits the result to 31; GBATEK mentions neither.
 */
static inline uint32_t capture_blend(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb)
{
    const uint32_t weight_a = (a >> 15) * eva, weight_b = (b >> 15) * evb;
    uint32_t out = (weight_a || weight_b) ? 0x8000u : 0u;
    for (uint32_t shift = 0; shift < 15; shift += 5) {
        const uint32_t c = (((a >> shift) & 31u) * weight_a + ((b >> shift) & 31u) * weight_b + 8u) >> 4;
        out |= (c > 31u ? 31u : c) << shift;
    }
    return out;
}

/*
 * The capture unit, for one line of the capture's size (GBATEK "DS Video
 * Capture and Main Memory Display Mode", DISPCAPCNT). It writes 15-bit
 * pixels plus the alpha bit into the destination block, which must be
 * mapped as LCDC memory (so the caller passes NULL when it is not; then,
 * as in melonDS, nothing is captured).
 *
 * Source A is the composed picture (alpha always 1) or the 3D engine's
 * line (alpha where its alpha is not 0). Source B is the VRAM block that
 * DISPCNT bits 18-19 select, read as a 256-pixel wide bitmap from the read
 * offset ("ignored (zero) in VRAM Display Mode"), or the main memory
 * display FIFO. Offsets "wrap to 00000h when exceeding 1FFFFh". A source
 * block that is not mapped reads as zero.
 */
static void capture_line(const Nds2d *e, const Nds2dVram *vram, uint32_t y, const uint32_t *composed,
                         const uint32_t *line_3d)
{
    const uint32_t dispcnt = reg32(e, REG_DISPCNT), capcnt = reg32(e, REG_DISPCAPCNT);
    uint8_t *dst = vram->capture[(capcnt >> 16) & 3u];
    if (!dst) return;
    const uint32_t width = k_capture_size[(capcnt >> 20) & 3u][0];
    const uint32_t source = (capcnt >> 29) & 3u; /* 0 = A, 1 = B, 2 and 3 = both blended */
    const bool a_is_3d = (capcnt & (1u << 24)) != 0;
    const bool b_is_fifo = (capcnt & (1u << 25)) != 0;
    const uint8_t *src_b = vram->lcdc[(dispcnt >> 18) & 3u];
    uint32_t eva = capcnt & 31u, evb = (capcnt >> 8) & 31u;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;

    /* Positions in 16-bit units within the 128 KiB block. */
    uint32_t to = (((capcnt >> 18) & 3u) * 0x4000u + y * width) & 0xFFFFu;
    uint32_t from = y * 256u;
    if (((dispcnt >> 16) & 3u) != 2u) from += ((capcnt >> 26) & 3u) * 0x4000u;
    from &= 0xFFFFu;

    for (uint32_t x = 0; x < width; x++) {
        uint32_t a = 0, b = 0;
        if (source != 1) {
            if (!a_is_3d) a = rgb15(composed[x]) | 0x8000u;
            else if (line_3d) a = rgb15(line_3d[x]) | (((line_3d[x] >> 18) & 31u) ? 0x8000u : 0u);
        }
        if (source != 0) {
            if (b_is_fifo) b = e->mmem_line[x];
            else if (src_b) b = nds_get_le16(src_b + from * 2u);
        }
        const uint32_t pixel = source == 0 ? a : source == 1 ? b : capture_blend(a, b, eva, evb);
        nds_put_le16(dst + to * 2u, (uint16_t)pixel);
        to = (to + 1u) & 0xFFFFu;
        from = (from + 1u) & 0xFFFFu;
    }
}

/* ------------------------------------------------------------------ */
/* The line                                                            */
/* ------------------------------------------------------------------ */

/*
 * "400006Ch - MASTER_BRIGHT":
 *   Brightness up:   New = Old + (63-Old) * Factor/16
 *   Brightness down: New = Old - Old      * Factor/16
 * with "values >16 same as 16", on the 6-bit colour. Mode 3 is reserved and
 * does nothing here. Rounding, from melonDS: up rounds its step down, down
 * rounds its step up.
 */
static void master_brightness(const Nds2d *e, uint32_t *line)
{
    const uint32_t reg = reg16(e, REG_MASTER_BRIGHT);
    const uint32_t mode = reg >> 14;
    uint32_t factor = reg & 31u;
    if (factor > 16) factor = 16;
    if ((mode != 1 && mode != 2) || !factor) return;
    uint8_t table[64];
    brightness_table(table, mode == 1, factor, mode == 1 ? 0u : 15u);
    for (uint32_t x = 0; x < W; x++) line[x] = map_channels(table, line[x]);
}

void nds_2d_render_line(Nds2d *e, const Nds2dVram *vram, int y, const uint32_t *line_3d, uint32_t *out)
{
    nds_2d_render_line_layers(e, vram, y, line_3d, out, NULL);
}

void nds_2d_render_line_layers(Nds2d *e, const Nds2dVram *vram, int y, const uint32_t *line_3d, uint32_t *out,
                               Nds2dLayers *layers)
{
    Nds2dLayers own;
    Nds2dLayers *l = layers ? layers : &own;
    l->composed = 0;
    l->has_3d = 0;
    l->capture = 0;
    l->capture_3d = 0;
    l->capture_block = 0;
    l->capture_at = NULL;
    for (int i = 0; i < 4; i++) l->direct_row[i] = NULL;
    l->hofs = (uint16_t)(reg16(e, REG_BG0HOFS) & 0x1FFu);
    if (y < 0 || y >= NDS_VISIBLE_LINES) {
        if (out) {
            for (uint32_t x = 0; x < W; x++) out[x] = WHITE;
        }
        return;
    }
    window_catch_up(e, (uint32_t)y);

    const uint32_t dispcnt = reg32(e, REG_DISPCNT);
    const uint32_t display = (dispcnt >> 16) & 3u;
    const uint32_t capcnt = reg32(e, REG_DISPCAPCNT);
    const bool capture = e->capture_active && vram && (uint32_t)y < k_capture_size[(capcnt >> 20) & 3u][1];
    const bool capture_a = capture && ((capcnt >> 29) & 3u) != 1;
    const bool capture_3d = capture_a && (capcnt & (1u << 24));
    /* The layers are composed whenever something takes the result: the
     * capture unit's graphics input is fed in every display mode. */
    const bool want_2d = display == 1 || (capture_a && !capture_3d);
    const bool bg0_3d = !e->engine && (dispcnt & 0x108u) == 0x108u;

    uint32_t scrolled[W];
    if (line_3d && !e->engine && ((want_2d && bg0_3d) || capture_3d)) line_3d = scroll_3d(e, line_3d, scrolled);
    else line_3d = NULL;

    uint32_t composed[W];
    if (want_2d) compose(e, vram, y, line_3d, composed, l);
    /* The layers are the line's picture only in display mode 1. */
    if (display != 1) l->composed = 0;
    l->master_bright = (uint16_t)reg16(e, REG_MASTER_BRIGHT);

    /*
     * Display mode (DISPCNT bits 16-17; engine B has only 0 and 1):
     *   0  Display off (screen becomes white)
     *   1  Graphics Display (normal BG and OBJ layers)
     *   2  Engine A only: VRAM Display (Bitmap from block selected in DISPCNT.18-19)
     *   3  Engine A only: Main Memory Display
     * Modes 2 and 3 show "a raw direct color bitmap (15bit RGB values, the
     * upper bit in each halfword is unused)". A block that is not mapped as
     * LCDC memory shows black (melonDS).
     */
    if (out) {
        switch (display) {
        case 1:
            memcpy(out, composed, sizeof composed);
            break;
        case 2: {
            const uint8_t *block = vram ? vram->lcdc[(dispcnt >> 18) & 3u] : NULL;
            for (uint32_t x = 0; x < W; x++) {
                out[x] = block ? rgb18(nds_get_le16(block + ((uint32_t)y * W + x) * 2u)) : 0;
            }
            break;
        }
        case 3:
            /*
             * An approximation: the hardware shows what a "main memory
             * display" DMA feeds the FIFO, eight pixels at a time, as the
             * line is drawn. Here the line is whatever was written to
             * DISP_MMEM_FIFO since the line before was rendered.
             */
            for (uint32_t x = 0; x < W; x++) out[x] = rgb18(e->mmem_line[x]);
            break;
        default:
            for (uint32_t x = 0; x < W; x++) out[x] = WHITE;
            break;
        }
    }

    /* The capture taps the picture before master brightness, and after
     * VRAM display has read the line it may be about to replace. */
    if (capture) capture_line(e, vram, (uint32_t)y, composed, line_3d);
    l->capture = 0;
    l->capture_3d = 0;
    l->capture_block = (uint8_t)((capcnt >> 16) & 3u);
    l->capture_at = NULL;
    if (capture && vram->capture[l->capture_block]) {
        const uint32_t width = k_capture_size[(capcnt >> 20) & 3u][0];
        const uint32_t to = (((capcnt >> 18) & 3u) * 0x4000u + (uint32_t)y * width) & 0xFFFFu;
        l->capture = 2;
        /* Source A alone, a whole line, in one piece: see capture_line(). */
        if (((capcnt >> 29) & 3u) == 0 && width == W && to + W <= 0x10000u && (!capture_3d || line_3d)) {
            l->capture = 1;
            l->capture_3d = capture_3d ? 1 : 0;
            l->capture_at = vram->capture[l->capture_block] + to * 2u;
        }
    }

    /* Master brightness comes last, behind the display mode selection
     * ("Select Video Input and Master Bright" in the block diagram), so it
     * applies to every mode. GBATEK says so for modes 2 and 3; for the white
     * of mode 0, current melonDS applies it and melonDS 1.1 did not. */
    if (out) master_brightness(e, out);

    advance_refs(e);
    advance_mosaic(e, (uint32_t)y);
    window_line_end(e);
    e->mmem_pos = 0;
}
