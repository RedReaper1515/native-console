/*
 * Picture processing unit: a scanline renderer.
 *
 * Each visible line is rendered at the start of its HBlank from the
 * registers and memory as they are at that moment. Backgrounds and sprites
 * are drawn into per-layer line buffers, then composed per pixel through
 * the window masks, priorities and colour special effects (GBATEK "LCD
 * Video Controller").
 *
 * The affine backgrounds (BG2/BG3, also used by the bitmap modes) keep
 * internal reference points: reloaded from BGxX/BGxY at the start of each
 * frame and whenever the game writes those registers, and advanced by
 * PB/PD after every line.
 *
 * While a texture pack is active (gba_tex_record != NULL) the text BGs and
 * regular sprites also note which tile pixel made each pixel, and the
 * composition notes the top two layers and the effect (gba_texture_pack.h).
 */
#include <stdlib.h>
#include <string.h>

#include "gba.h"
#include "gba_texture_pack.h"

#define W GBA_SCREEN_WIDTH
#define TRANSPARENT 0x8000u /* bit 15 of a line-buffer entry: no pixel */

enum { LAYER_BG0, LAYER_BG1, LAYER_BG2, LAYER_BG3, LAYER_OBJ, LAYER_BD };

typedef struct {
    uint16_t color[W]; /* BGR555, or TRANSPARENT */
} BgLine;

typedef struct {
    uint16_t color[W];
    uint8_t prio[W];
    uint8_t semi[W];   /* semi-transparent sprite pixel */
    uint8_t window[W]; /* inside an OBJ-window sprite */
    uint8_t mosaic[W];
    bool any_semi;     /* some pixel is semi-transparent */
} ObjLine;

static inline uint16_t io16(const GbaMachine *m, uint32_t off)
{
    return (uint16_t)(m->io[off] | (m->io[off + 1] << 8));
}

static inline uint32_t io32(const GbaMachine *m, uint32_t off)
{
    return io16(m, off) | ((uint32_t)io16(m, off + 2) << 16);
}

static inline uint16_t pal(const GbaMachine *m, uint32_t index)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint16_t v = (uint16_t)(m->palette[index * 2] | (m->palette[index * 2 + 1] << 8));
#else
    uint16_t v;
    memcpy(&v, &m->palette[index * 2], 2);
#endif
    return (uint16_t)(v & 0x7FFFu);
}

static inline uint16_t vram16(const GbaMachine *m, uint32_t off)
{
    return (uint16_t)(m->vram[off] | (m->vram[off + 1] << 8));
}

/* n (4 or 8) bytes of BG tile data as a little-endian integer; bytes past
 * the 64 KiB BG area read as 0. */
static inline uint64_t tile_row(const GbaMachine *m, uint32_t off, uint32_t n)
{
    uint64_t v = 0;
    if (off + n <= 0x10000u) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        for (uint32_t i = 0; i < n; i++) v |= (uint64_t)m->vram[off + i] << (8 * i);
#else
        memcpy(&v, &m->vram[off], n);
#endif
        return v;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (off + i < 0x10000u) v |= (uint64_t)m->vram[off + i] << (8 * i);
    }
    return v;
}

static inline uint32_t bgr555_to_argb(uint16_t c)
{
    uint32_t r = (c & 0x1Fu) << 3;
    uint32_t g = ((c >> 5) & 0x1Fu) << 3;
    uint32_t b = ((c >> 10) & 0x1Fu) << 3;
    return 0xFF000000u | ((r | (r >> 5)) << 16) | ((g | (g >> 5)) << 8) | (b | (b >> 5));
}

/* 28-bit signed reference point register, sign-extended. */
static inline int32_t ref_point(uint32_t raw)
{
    return (int32_t)(raw << 4) >> 4;
}

/* ------------------------------------------------------------------ */
/* Affine reference points                                             */
/* ------------------------------------------------------------------ */

static void reload_ref(GbaMachine *m, int bg)
{
    uint32_t base = bg == 2 ? 0x28u : 0x38u;
    m->ppu_ref_x[bg - 2] = ref_point(io32(m, base));
    m->ppu_ref_y[bg - 2] = ref_point(io32(m, base + 4));
}

void gba_ppu_reference_written(GbaMachine *m, uint32_t offset)
{
    if (offset >= 0x28 && offset < 0x30) reload_ref(m, 2);
    else if (offset >= 0x38 && offset < 0x40) reload_ref(m, 3);
}

static void large_frame_end(const GbaMachine *m);

void gba_ppu_frame_start(GbaMachine *m)
{
    large_frame_end(m);
    reload_ref(m, 2);
    reload_ref(m, 3);
}

static void advance_refs(GbaMachine *m)
{
    for (int i = 0; i < 2; i++) {
        uint32_t base = i == 0 ? 0x20u : 0x30u;
        m->ppu_ref_x[i] += (int16_t)io16(m, base + 2); /* PB */
        m->ppu_ref_y[i] += (int16_t)io16(m, base + 6); /* PD */
    }
}

/* ------------------------------------------------------------------ */
/* Backgrounds                                                         */
/* ------------------------------------------------------------------ */

static void mosaic_h(BgLine *line, int size)
{
    if (size <= 1) return;
    for (int x = 0; x < W; x++) line->color[x] = line->color[x - x % size];
}

static void render_text_bg(GbaMachine *m, int bg, int y_line, BgLine *out, GbaTexSource *src)
{
    uint16_t cnt = io16(m, 0x08 + 2u * (uint32_t)bg);
    uint32_t char_base = ((cnt >> 2) & 3u) * 0x4000u;
    uint32_t screen_base = ((cnt >> 8) & 31u) * 0x800u;
    bool bpp8 = (cnt & 0x80u) != 0;
    uint32_t size = (cnt >> 14) & 3u;
    uint32_t width = (size & 1u) ? 512u : 256u;
    uint32_t height = (size & 2u) ? 512u : 256u;
    uint32_t hofs = io16(m, 0x10 + 4u * (uint32_t)bg) & 0x1FFu;
    uint32_t vofs = io16(m, 0x12 + 4u * (uint32_t)bg) & 0x1FFu;

    uint32_t mosaic = io16(m, 0x4C);
    int line = y_line;
    if (cnt & 0x40u) line -= line % (int)(((mosaic >> 4) & 15u) + 1u);

    uint32_t y = ((uint32_t)line + vofs) & (height - 1u);
    uint32_t map_row = screen_base + (y >> 8) * (width >> 8) * 0x800u + ((y & 255u) >> 3) * 64u;
    /* One map entry and one row of tile data per 8 pixels. */
    for (int x = 0; x < W;) {
        uint32_t px = ((uint32_t)x + hofs) & (width - 1u);
        uint32_t entry_off = map_row + (px >> 8) * 0x800u + ((px & 255u) >> 3) * 2u;
        uint16_t entry = entry_off < 0x10000u ? vram16(m, entry_off) : 0;
        uint32_t tile = entry & 0x3FFu;
        uint32_t ty = (entry & 0x800u) ? 7u - (y & 7u) : y & 7u;
        uint32_t flip = (entry & 0x400u) ? 7u : 0u;
        int run = 8 - (int)(px & 7u);
        if (run > W - x) run = W - x;

        if (src) {
            uint32_t tile_off = char_base + tile * (bpp8 ? 64u : 32u);
            uint8_t flags = (uint8_t)((tile_off + (bpp8 ? 64u : 32u) <= 0x10000u ? GBA_TEX_VALID : 0u) |
                                      (bpp8 ? GBA_TEX_BPP8 : 0u) | ((entry & 0x400u) ? GBA_TEX_HFLIP : 0u) |
                                      ((entry & 0x800u) ? GBA_TEX_VFLIP : 0u));
            for (int i = 0; i < run; i++) {
                GbaTexSource *s = &src[x + i];
                s->tile_off = tile_off;
                s->pal_base = (uint16_t)(bpp8 ? 0u : (uint32_t)(entry >> 12) * 16u);
                s->tx = (uint8_t)(((px + (uint32_t)i) & 7u) ^ flip);
                s->ty = (uint8_t)ty;
                s->flags = flags;
            }
        }
        if (bpp8) {
            uint64_t bits = tile_row(m, char_base + tile * 64u + ty * 8u, 8);
            for (int i = 0; i < run; i++) {
                uint32_t tx = ((px + (uint32_t)i) & 7u) ^ flip;
                uint32_t color_index = (uint32_t)(bits >> (tx * 8u)) & 0xFFu;
                out->color[x + i] = color_index ? pal(m, color_index) : TRANSPARENT;
            }
        } else {
            uint32_t bits = (uint32_t)tile_row(m, char_base + tile * 32u + ty * 4u, 4);
            uint32_t bank = (uint32_t)(entry >> 12) * 16u;
            for (int i = 0; i < run; i++) {
                uint32_t tx = ((px + (uint32_t)i) & 7u) ^ flip;
                uint32_t color_index = (bits >> (tx * 4u)) & 15u;
                out->color[x + i] = color_index ? pal(m, bank + color_index) : TRANSPARENT;
            }
        }
        x += run;
    }
    if (cnt & 0x40u) {
        mosaic_h(out, (int)(mosaic & 15u) + 1);
        /* A mosaic larger than 1x1 keeps its original pixels. */
        if (src && (mosaic & 0xFFu) != 0) memset(src, 0, sizeof(GbaTexSource) * W);
    }
}

static void render_affine_bg(GbaMachine *m, int bg, BgLine *out)
{
    uint16_t cnt = io16(m, 0x08 + 2u * (uint32_t)bg);
    uint32_t char_base = ((cnt >> 2) & 3u) * 0x4000u;
    uint32_t screen_base = ((cnt >> 8) & 31u) * 0x800u;
    uint32_t size = 128u << ((cnt >> 14) & 3u);
    bool wrap = (cnt & 0x2000u) != 0;
    uint32_t pbase = bg == 2 ? 0x20u : 0x30u;
    int32_t pa = (int16_t)io16(m, pbase);
    int32_t pc = (int16_t)io16(m, pbase + 4);
    int32_t rx = m->ppu_ref_x[bg - 2], ry = m->ppu_ref_y[bg - 2];

    for (int x = 0; x < W; x++, rx += pa, ry += pc) {
        int32_t tx = rx >> 8, ty = ry >> 8;
        if (wrap) {
            tx &= (int32_t)size - 1;
            ty &= (int32_t)size - 1;
        } else if (tx < 0 || ty < 0 || tx >= (int32_t)size || ty >= (int32_t)size) {
            out->color[x] = TRANSPARENT;
            continue;
        }
        uint32_t map_off = screen_base + ((uint32_t)ty >> 3) * (size >> 3) + ((uint32_t)tx >> 3);
        uint32_t tile = map_off < 0x10000u ? m->vram[map_off] : 0;
        uint32_t off = char_base + tile * 64u + ((uint32_t)ty & 7u) * 8u + ((uint32_t)tx & 7u);
        uint32_t color_index = off < 0x10000u ? m->vram[off] : 0;
        out->color[x] = color_index ? pal(m, color_index) : TRANSPARENT;
    }
    if (cnt & 0x40u) mosaic_h(out, (int)(io16(m, 0x4C) & 15u) + 1);
}

static void render_bitmap_bg(GbaMachine *m, uint32_t mode, BgLine *out)
{
    uint16_t dispcnt = io16(m, 0);
    uint32_t page = (dispcnt & 0x10u) ? 0xA000u : 0;
    int32_t pa = (int16_t)io16(m, 0x20);
    int32_t pc = (int16_t)io16(m, 0x24);
    int32_t rx = m->ppu_ref_x[0], ry = m->ppu_ref_y[0];
    int32_t width = mode == 5 ? 160 : 240;
    int32_t height = mode == 5 ? 128 : 160;

    for (int x = 0; x < W; x++, rx += pa, ry += pc) {
        int32_t tx = rx >> 8, ty = ry >> 8;
        if (tx < 0 || ty < 0 || tx >= width || ty >= height) {
            out->color[x] = TRANSPARENT;
            continue;
        }
        uint32_t index = (uint32_t)(ty * width + tx);
        if (mode == 4) {
            uint32_t c = m->vram[page + index];
            out->color[x] = c ? pal(m, c) : TRANSPARENT;
        } else {
            uint32_t base = mode == 5 ? page : 0;
            out->color[x] = vram16(m, base + index * 2u) & 0x7FFFu;
        }
    }
    uint16_t cnt = io16(m, 0x0C);
    if (cnt & 0x40u) mosaic_h(out, (int)(io16(m, 0x4C) & 15u) + 1);
}

/* ------------------------------------------------------------------ */
/* Sprites                                                             */
/* ------------------------------------------------------------------ */

static const uint8_t k_obj_size[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } }, /* square */
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } }, /* horizontal */
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } }, /* vertical */
};

static inline uint16_t oam16(const GbaMachine *m, uint32_t off)
{
    return (uint16_t)(m->oam[off] | (m->oam[off + 1] << 8));
}

static void render_sprites(GbaMachine *m, int line, ObjLine *out, GbaTexSource *tex)
{
    uint16_t dispcnt = io16(m, 0);
    bool map_1d = (dispcnt & 0x40u) != 0;
    bool bitmap_mode = (dispcnt & 7u) >= 3;
    uint32_t mosaic = io16(m, 0x4C);
    int mos_h = (int)((mosaic >> 8) & 15u) + 1;
    int mos_v = (int)((mosaic >> 12) & 15u) + 1;

    out->any_semi = false;
    for (int x = 0; x < W; x++) {
        out->color[x] = TRANSPARENT;
        out->prio[x] = 4;
        out->semi[x] = 0;
        out->window[x] = 0;
        out->mosaic[x] = 0;
    }
    if (tex) memset(tex, 0, sizeof(GbaTexSource) * W);

    for (int i = 0; i < 128; i++) {
        uint16_t a0 = oam16(m, (uint32_t)i * 8u);
        uint16_t a1 = oam16(m, (uint32_t)i * 8u + 2u);
        uint16_t a2 = oam16(m, (uint32_t)i * 8u + 4u);
        bool affine = (a0 & 0x100u) != 0;
        if (!affine && (a0 & 0x200u)) continue; /* disabled */
        uint32_t mode = (a0 >> 10) & 3u;
        if (mode == 3) continue;
        uint32_t shape = (a0 >> 14) & 3u;
        if (shape == 3) continue;
        int w = k_obj_size[shape][a1 >> 14][0];
        int h = k_obj_size[shape][a1 >> 14][1];
        bool dbl = affine && (a0 & 0x200u);
        int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;

        int oy = a0 & 0xFF;
        int dy = (line - oy) & 0xFF;
        if (dy >= bh) continue;
        bool obj_mosaic = (a0 & 0x1000u) != 0;
        if (obj_mosaic) {
            /* Vertical mosaic samples the row of the first line of each
             * mosaic block (counted from the top of the screen). */
            int mdy = (line - line % mos_v - oy) & 0xFF;
            if (mdy < bh) dy = mdy;
        }

        int ox = a1 & 0x1FF;
        if (ox >= 240) ox -= 512;
        bool bpp8 = (a0 & 0x2000u) != 0;
        uint32_t tile_base = a2 & 0x3FFu;
        if (bitmap_mode && tile_base < 512) continue; /* those tiles hold the bitmap */
        int prio = (a2 >> 10) & 3;
        uint32_t palette = bpp8 ? 0 : (uint32_t)(a2 >> 12) * 16u;

        int32_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            uint32_t group = ((a1 >> 9) & 31u) * 32u;
            pa = (int16_t)oam16(m, group + 6);
            pb = (int16_t)oam16(m, group + 14);
            pc = (int16_t)oam16(m, group + 22);
            pd = (int16_t)oam16(m, group + 30);
        }

        for (int sx = 0; sx < bw; sx++) {
            int x = ox + sx;
            if (x < 0 || x >= W) continue;
            int tx, ty;
            if (affine) {
                int cx = sx - bw / 2, cy = dy - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000u) ? w - 1 - sx : sx;
                ty = (a1 & 0x2000u) ? h - 1 - dy : dy;
            }

            uint32_t tile;
            uint32_t tiles_per_row = map_1d ? (uint32_t)(w / 8) * (bpp8 ? 2u : 1u) : 32u;
            tile = tile_base + (uint32_t)(ty / 8) * tiles_per_row + (uint32_t)(tx / 8) * (bpp8 ? 2u : 1u);
            uint32_t color_index;
            if (bpp8) {
                uint32_t off = 0x10000u + ((tile & 0x3FFu) * 32u) + (uint32_t)(ty & 7) * 8u + (uint32_t)(tx & 7);
                color_index = off < GBA_VRAM_SIZE ? m->vram[off] : 0;
            } else {
                uint32_t off = 0x10000u + ((tile & 0x3FFu) * 32u) + (uint32_t)(ty & 7) * 4u + (uint32_t)(tx & 7) / 2u;
                uint32_t byte = off < GBA_VRAM_SIZE ? m->vram[off] : 0;
                color_index = (tx & 1) ? byte >> 4 : byte & 15u;
            }
            if (!color_index) continue;

            if (mode == 2) {
                out->window[x] = 1;
                continue;
            }
            if (out->color[x] != TRANSPARENT && prio >= out->prio[x]) continue;
            out->color[x] = pal(m, 256u + palette + color_index);
            out->prio[x] = (uint8_t)prio;
            out->semi[x] = mode == 1;
            out->any_semi |= mode == 1;
            out->mosaic[x] = obj_mosaic;
            if (tex) {
                GbaTexSource *s = &tex[x];
                if (affine || (obj_mosaic && (mos_h > 1 || mos_v > 1))) {
                    s->flags = 0; /* rotated, scaled and mosaic sprites keep their original pixels */
                } else {
                    s->tile_off = 0x10000u + (tile & 0x3FFu) * 32u;
                    s->pal_base = (uint16_t)(256u + palette);
                    s->tx = (uint8_t)(tx & 7);
                    s->ty = (uint8_t)(ty & 7);
                    s->flags = (uint8_t)((s->tile_off + (bpp8 ? 64u : 32u) <= GBA_VRAM_SIZE ? GBA_TEX_VALID : 0u) |
                                         (bpp8 ? GBA_TEX_BPP8 : 0u) | ((a1 & 0x1000u) ? GBA_TEX_HFLIP : 0u) |
                                         ((a1 & 0x2000u) ? GBA_TEX_VFLIP : 0u));
                }
            }
        }
    }

    if (mos_h > 1) {
        for (int x = 0; x < W; x++) {
            int src = x - x % mos_h;
            if (out->mosaic[x] && out->mosaic[src]) {
                out->color[x] = out->color[src];
                out->prio[x] = out->prio[src];
                out->semi[x] = out->semi[src];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Composition                                                         */
/* ------------------------------------------------------------------ */

static inline bool in_range(int v, int lo, int hi, int limit)
{
    if (hi > limit || lo > hi) {
        /* Hardware wraps: inside means past the start or before the end. */
        if (hi > limit) hi = limit;
        return lo > hi ? (v >= lo || v < hi) : (v >= lo && v < hi);
    }
    return v >= lo && v < hi;
}

static inline uint16_t blend_alpha(uint16_t a, uint16_t b, uint32_t eva, uint32_t evb)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 15; shift += 5) {
        uint32_t c = (((a >> shift) & 31u) * eva + ((b >> shift) & 31u) * evb) >> 4;
        out |= (c > 31 ? 31 : c) << shift;
    }
    return (uint16_t)out;
}

static inline uint16_t blend_bright(uint16_t a, uint32_t evy, bool up)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 15; shift += 5) {
        uint32_t c = (a >> shift) & 31u;
        c = up ? c + (((31u - c) * evy) >> 4) : c - ((c * evy) >> 4);
        out |= c << shift;
    }
    return (uint16_t)out;
}

/* ------------------------------------------------------------------ */
/* Rotated and scaled layers, drawn larger                             */
/* ------------------------------------------------------------------ */

/*
 * gba_ppu_set_affine_scale(): the rotated and scaled layers sampled `scale`
 * times a pixel each way. The console's line is drawn first and as always;
 * a line that shows such a layer is then drawn again, `scale` rows of 240 *
 * scale samples, composed by the same rules.
 *
 * A background sample is the layer at a fraction of a pixel: the console's
 * reference point and PA/PC with the screen position in scale-ths, in exact
 * integers, so a sample at a whole pixel is the console's pixel. Down the
 * screen the reference point moves by what it moved since the line above
 * (PB/PD, unless the game rewrote it for the line, as a floor in
 * perspective does), and PA/PC by their change, where those are small
 * enough to be one surface. A sprite sample is the sprite's own matrix at
 * a fraction of a pixel. Everything else is the console's pixels repeated.
 *
 * The picture lives here, not in GbaMachine, which a save state copies byte
 * for byte: one machine at a time has one.
 */
#define FINE_MAX (W * GBA_AFFINE_SCALE_MAX)

typedef struct {
    int line;                 /* the line these are of, or -2 */
    int32_t rx, ry, pa, pc;
    int32_t step_x, step_y;   /* how far the reference point had moved */
    bool has_step;
} AffineLine;

static struct {
    const GbaMachine *owner;
    int scale;
    uint32_t *frame;          /* 240 * scale by 160 * scale */
    uint8_t fine_row[GBA_SCREEN_HEIGHT];
    bool any;
    bool valid;               /* the last complete frame has a larger picture */
    AffineLine prev[2];       /* BG2 and BG3 on the line before */
} large;

bool gba_ppu_set_affine_scale(GbaMachine *m, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > GBA_AFFINE_SCALE_MAX) return false;
    if (scale == 1) {
        if (large.owner == m) {
            free(large.frame);
            memset(&large, 0, sizeof large);
        }
        return true;
    }
    if (large.owner == m && large.scale == scale) return true;
    uint32_t *frame = (uint32_t *)calloc((size_t)W * (size_t)scale * GBA_SCREEN_HEIGHT * (size_t)scale, sizeof(uint32_t));
    if (!frame) return false;
    free(large.frame);
    memset(&large, 0, sizeof large);
    large.owner = m;
    large.scale = scale;
    large.frame = frame;
    large.prev[0].line = large.prev[1].line = -2;
    return true;
}

int gba_ppu_affine_scale(const GbaMachine *m) { return large.owner == m ? large.scale : 1; }

const uint32_t *gba_ppu_large_frame(const GbaMachine *m, int *width, int *height)
{
    if (large.owner != m || !large.valid) return NULL;
    if (width) *width = W * large.scale;
    if (height) *height = GBA_SCREEN_HEIGHT * large.scale;
    return large.frame;
}

/* The frame is complete: its other rows are the console's, repeated. */
static void large_frame_end(const GbaMachine *m)
{
    if (large.owner != m) return;
    large.valid = large.any;
    if (large.valid) {
        const int n = large.scale;
        const size_t pitch = (size_t)W * (size_t)n;
        for (int row = 0; row < GBA_SCREEN_HEIGHT; row++) {
            if (large.fine_row[row]) continue;
            const uint32_t *src = &m->framebuffer[row * W];
            uint32_t *out = large.frame + (size_t)row * (size_t)n * pitch;
            for (size_t x = 0; x < pitch; x++) out[x] = src[x / (size_t)n];
            for (int k = 1; k < n; k++) memcpy(out + (size_t)k * pitch, out, pitch * sizeof out[0]);
        }
    }
    memset(large.fine_row, 0, sizeof large.fine_row);
    large.any = false;
    large.prev[0].line = large.prev[1].line = -2;
}

/* floor(v / d) for the three scales' divisors, each a constant so that it
 * is a multiplication: `per` is 256 (texels from 256ths in n-ths) or 256 * n
 * (the same in n-ths twice over). */
static inline int32_t fine_floor(int64_t v, int n, bool twice)
{
#define FINE_FLOOR(D) (int32_t)(v >= 0 ? v / (D) : -((-v + (D) - 1) / (D)))
    if (twice) {
        switch (n) {
        case 2: return FINE_FLOOR(1024);
        case 3: return FINE_FLOOR(2304);
        default: return FINE_FLOOR(4096);
        }
    }
    switch (n) {
    case 2: return FINE_FLOOR(512);
    case 3: return FINE_FLOOR(768);
    default: return FINE_FLOOR(1024);
    }
#undef FINE_FLOOR
}

static inline bool fine_near(int32_t now, int32_t before, int32_t slack)
{
    int32_t change = now > before ? now - before : before - now;
    return change <= slack;
}

static inline int32_t fine_size(int32_t v) { return v < 0 ? -v : v; }

/* Row k of n of an affine background's line: render_affine_bg() and
 * render_bitmap_bg() at n samples a pixel. `x0` and `y0` are the reference
 * point on this row and `pa`, `pc` the step along it, all in n-ths twice
 * over. */
static void fine_affine_bg(const GbaMachine *m, int bg, uint32_t mode, int n, int64_t x0, int64_t y0, int64_t pa,
                           int64_t pc, uint16_t *out)
{
    const uint16_t cnt = io16(m, 0x08 + 2u * (uint32_t)bg);
    const uint16_t dispcnt = io16(m, 0);
    const uint32_t char_base = ((cnt >> 2) & 3u) * 0x4000u;
    const uint32_t screen_base = ((cnt >> 8) & 31u) * 0x800u;
    const uint32_t size = 128u << ((cnt >> 14) & 3u);
    const bool wrap = (cnt & 0x2000u) != 0;
    const uint32_t page = (dispcnt & 0x10u) ? 0xA000u : 0;
    const int32_t width = mode == 5 ? 160 : 240, height = mode == 5 ? 128 : 160;
    int32_t held_x = INT32_MIN, held_y = INT32_MIN;
    uint16_t held = TRANSPARENT;

    for (int xs = 0; xs < W * n; xs++, x0 += pa, y0 += pc) {
        int32_t tx = fine_floor(x0, n, true), ty = fine_floor(y0, n, true);
        if (tx == held_x && ty == held_y) {
            out[xs] = held;
            continue;
        }
        held_x = tx;
        held_y = ty;
        held = TRANSPARENT;
        if (mode >= 3) {
            if (tx >= 0 && ty >= 0 && tx < width && ty < height) {
                const uint32_t index = (uint32_t)(ty * width + tx);
                if (mode == 4) {
                    const uint32_t c = m->vram[page + index];
                    held = c ? pal(m, c) : TRANSPARENT;
                } else {
                    held = vram16(m, (mode == 5 ? page : 0) + index * 2u) & 0x7FFFu;
                }
            }
        } else {
            if (wrap) {
                tx &= (int32_t)size - 1;
                ty &= (int32_t)size - 1;
            }
            if (tx >= 0 && ty >= 0 && tx < (int32_t)size && ty < (int32_t)size) {
                const uint32_t map_off = screen_base + ((uint32_t)ty >> 3) * (size >> 3) + ((uint32_t)tx >> 3);
                const uint32_t tile = map_off < 0x10000u ? m->vram[map_off] : 0;
                const uint32_t off = char_base + tile * 64u + ((uint32_t)ty & 7u) * 8u + ((uint32_t)tx & 7u);
                const uint32_t color_index = off < 0x10000u ? m->vram[off] : 0;
                held = color_index ? pal(m, color_index) : TRANSPARENT;
            }
        }
        out[xs] = held;
    }
}

static inline bool obj_matrix_plain(const GbaMachine *m, uint16_t a1)
{
    const uint32_t group = ((a1 >> 9) & 31u) * 32u;
    return (int16_t)oam16(m, group + 6) == 256 && (int16_t)oam16(m, group + 14) == 0 &&
           (int16_t)oam16(m, group + 22) == 0 && (int16_t)oam16(m, group + 30) == 256;
}

/* Whether a rotated or scaled sprite is on the line. */
static bool fine_sprites_wanted(const GbaMachine *m, int line)
{
    for (int i = 0; i < 128; i++) {
        const uint16_t a0 = oam16(m, (uint32_t)i * 8u), a1 = oam16(m, (uint32_t)i * 8u + 2u);
        if (!(a0 & 0x100u) || ((a0 >> 10) & 3u) >= 2u || ((a0 >> 14) & 3u) == 3u) continue;
        int h = k_obj_size[(a0 >> 14) & 3u][a1 >> 14][1];
        if (a0 & 0x200u) h *= 2;
        if (((line - (a0 & 0xFF)) & 0xFF) >= h) continue;
        if (!obj_matrix_plain(m, a1)) return true;
    }
    return false;
}

/* Row k of n of the sprites' line: render_sprites() at n samples a pixel,
 * without the OBJ window (which keeps the console's outline) and without
 * mosaic (such a line is not drawn larger). */
static void fine_sprites(const GbaMachine *m, int line, int n, int k, uint16_t *color, uint8_t *prio_out,
                         uint8_t *semi)
{
    const uint16_t dispcnt = io16(m, 0);
    const bool map_1d = (dispcnt & 0x40u) != 0;
    const bool bitmap_mode = (dispcnt & 7u) >= 3;

    for (int xs = 0; xs < W * n; xs++) {
        color[xs] = TRANSPARENT;
        prio_out[xs] = 4;
        semi[xs] = 0;
    }
    for (int i = 0; i < 128; i++) {
        const uint16_t a0 = oam16(m, (uint32_t)i * 8u);
        const uint16_t a1 = oam16(m, (uint32_t)i * 8u + 2u);
        const uint16_t a2 = oam16(m, (uint32_t)i * 8u + 4u);
        const bool affine = (a0 & 0x100u) != 0;
        if (!affine && (a0 & 0x200u)) continue;
        const uint32_t mode = (a0 >> 10) & 3u;
        if (mode >= 2) continue;
        const uint32_t shape = (a0 >> 14) & 3u;
        if (shape == 3) continue;
        const int w = k_obj_size[shape][a1 >> 14][0], h = k_obj_size[shape][a1 >> 14][1];
        const bool dbl = affine && (a0 & 0x200u);
        const int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        const int dy = (line - (a0 & 0xFF)) & 0xFF;
        if (dy >= bh) continue;
        int ox = a1 & 0x1FF;
        if (ox >= 240) ox -= 512;
        const bool bpp8 = (a0 & 0x2000u) != 0;
        const uint32_t tile_base = a2 & 0x3FFu;
        if (bitmap_mode && tile_base < 512) continue;
        const int prio = (a2 >> 10) & 3;
        const uint32_t palette = bpp8 ? 0 : (uint32_t)(a2 >> 12) * 16u;
        const uint32_t tiles_per_row = map_1d ? (uint32_t)(w / 8) * (bpp8 ? 2u : 1u) : 32u;

        int32_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            const uint32_t group = ((a1 >> 9) & 31u) * 32u;
            pa = (int16_t)oam16(m, group + 6);
            pb = (int16_t)oam16(m, group + 14);
            pc = (int16_t)oam16(m, group + 22);
            pd = (int16_t)oam16(m, group + 30);
        }
        const int64_t cy = (int64_t)(dy - bh / 2) * n + k;

        for (int sx = 0; sx < bw; sx++) {
            const int x = ox + sx;
            if (x < 0 || x >= W) continue;
            for (int j = 0; j < n; j++) {
                int tx, ty;
                if (affine) {
                    const int64_t cx = (int64_t)(sx - bw / 2) * n + j;
                    tx = fine_floor(pa * cx + pb * cy, n, false) + w / 2;
                    ty = fine_floor(pc * cx + pd * cy, n, false) + h / 2;
                    if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
                } else {
                    tx = (a1 & 0x1000u) ? w - 1 - sx : sx;
                    ty = (a1 & 0x2000u) ? h - 1 - dy : dy;
                }
                const uint32_t tile = tile_base + (uint32_t)(ty / 8) * tiles_per_row + (uint32_t)(tx / 8) * (bpp8 ? 2u : 1u);
                uint32_t color_index;
                if (bpp8) {
                    const uint32_t off = 0x10000u + ((tile & 0x3FFu) * 32u) + (uint32_t)(ty & 7) * 8u + (uint32_t)(tx & 7);
                    color_index = off < GBA_VRAM_SIZE ? m->vram[off] : 0;
                } else {
                    const uint32_t off = 0x10000u + ((tile & 0x3FFu) * 32u) + (uint32_t)(ty & 7) * 4u + (uint32_t)(tx & 7) / 2u;
                    const uint32_t byte = off < GBA_VRAM_SIZE ? m->vram[off] : 0;
                    color_index = (tx & 1) ? byte >> 4 : byte & 15u;
                }
                if (!color_index) continue;
                const int xs = x * n + j;
                if (color[xs] != TRANSPARENT && prio >= prio_out[xs]) continue;
                color[xs] = pal(m, 256u + palette + color_index);
                prio_out[xs] = (uint8_t)prio;
                semi[xs] = mode == 1;
            }
        }
    }
}

/* Line `line` again, larger, after the console's own and before its
 * reference points move on. The layers, their order and the window masks
 * are the ones the console's line was composed from. */
static void large_line(GbaMachine *m, int line, uint32_t mode, const bool bg_on[4], const int order[4],
                       const int order_prio[4], int layers, const BgLine bgs[4], const ObjLine *obj,
                       const uint8_t enabled[W], bool obj_on)
{
    static uint16_t bg_fine[4][FINE_MAX], obj_color[FINE_MAX];
    static uint8_t obj_prio[FINE_MAX], obj_semi[FINE_MAX];
    const int n = large.scale;

    /* The backgrounds drawn through a matrix, and what the matrix is on
     * this line and the rows below it. */
    bool affine_bg[4] = { false, false, false, false }, wanted = false;
    int64_t step_x[2] = { 0, 0 }, step_y[2] = { 0, 0 }, turn_a[2] = { 0, 0 }, turn_c[2] = { 0, 0 };
    AffineLine now[2];
    for (int i = 0; i < 2; i++) {
        const int bg = 2 + i;
        const uint32_t base = i == 0 ? 0x20u : 0x30u;
        const int32_t pb = (int16_t)io16(m, base + 2), pd = (int16_t)io16(m, base + 6);
        const AffineLine *was = &large.prev[i];
        now[i].line = -2;
        if (!bg_on[bg] || !(mode == 2 || (mode == 1 && bg == 2) || (mode >= 3 && bg == 2))) continue;
        affine_bg[bg] = true;
        now[i].line = line;
        now[i].rx = m->ppu_ref_x[i];
        now[i].ry = m->ppu_ref_y[i];
        now[i].pa = (int16_t)io16(m, base);
        now[i].pc = (int16_t)io16(m, base + 4);
        now[i].has_step = was->line == line - 1;
        now[i].step_x = now[i].has_step ? now[i].rx - was->rx : pb;
        now[i].step_y = now[i].has_step ? now[i].ry - was->ry : pd;
        /* Down the screen: as the reference point has been moving, if it
         * moved the same way the line before; otherwise as PB and PD say. */
        step_x[i] = pb;
        step_y[i] = pd;
        if (now[i].has_step) {
            const bool steady = !was->has_step
                                    ? now[i].step_x == pb && now[i].step_y == pd
                                    : fine_near(now[i].step_x, was->step_x, (fine_size(was->step_x) >> 2) + 256) &&
                                          fine_near(now[i].step_y, was->step_y, (fine_size(was->step_y) >> 2) + 256);
            if (steady) {
                step_x[i] = now[i].step_x;
                step_y[i] = now[i].step_y;
                if (fine_near(now[i].pa, was->pa, (fine_size(now[i].pa) >> 3) + 2) &&
                    fine_near(now[i].pc, was->pc, (fine_size(now[i].pc) >> 3) + 2)) {
                    turn_a[i] = now[i].pa - was->pa;
                    turn_c[i] = now[i].pc - was->pc;
                }
            }
        }
        /* A layer drawn one texel to a pixel, square to the screen, is the
         * console's pixels however many samples it is given. */
        if (now[i].pa != 256 || now[i].pc != 0 || step_x[i] != 0 || step_y[i] != 256 || ((now[i].rx | now[i].ry) & 0xFF))
            wanted = true;
    }
    large.prev[0] = now[0];
    large.prev[1] = now[1];

    const bool sprites = obj_on && fine_sprites_wanted(m, line);
    if (!wanted && !sprites) return;
    if (io16(m, 0x4C) != 0 || (io16(m, 0x02) & 1u) || gba_tex_record) return;

    const uint16_t bldcnt = io16(m, 0x50), bldalpha = io16(m, 0x52);
    uint32_t eva = bldalpha & 31u, evb = (bldalpha >> 8) & 31u, evy = io16(m, 0x54) & 31u;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    const uint32_t effect = (bldcnt >> 6) & 3u;
    const uint16_t backdrop = pal(m, 0);
    const size_t pitch = (size_t)W * (size_t)n;

    for (int k = 0; k < n; k++) {
        for (int bg = 0; bg < 4; bg++) {
            if (!bg_on[bg]) continue;
            if (affine_bg[bg]) {
                const int i = bg - 2;
                fine_affine_bg(m, bg, mode, n, ((int64_t)now[i].rx * n + step_x[i] * k) * n,
                               ((int64_t)now[i].ry * n + step_y[i] * k) * n, (int64_t)now[i].pa * n + turn_a[i] * k,
                               (int64_t)now[i].pc * n + turn_c[i] * k, bg_fine[bg]);
            } else if (k == 0) {
                for (int xs = 0; xs < W * n; xs++) bg_fine[bg][xs] = bgs[bg].color[xs / n];
            }
        }
        if (sprites) {
            fine_sprites(m, line, n, k, obj_color, obj_prio, obj_semi);
        } else if (k == 0) {
            for (int xs = 0; xs < W * n; xs++) {
                obj_color[xs] = obj->color[xs / n];
                obj_prio[xs] = obj->prio[xs / n];
                obj_semi[xs] = obj->semi[xs / n];
            }
        }

        /* The composition of gba_ppu_render_line(), a sample at a time. */
        uint32_t *out = large.frame + ((size_t)line * (size_t)n + (size_t)k) * pitch;
        for (int xs = 0; xs < W * n; xs++) {
            const uint8_t on = enabled[xs / n];
            int top = LAYER_BD, second = LAYER_BD, top_prio = 4, second_prio = 4;
            uint16_t top_color = backdrop, second_color = backdrop;
            for (int at = layers - 1; at >= 0; at--) {
                const uint16_t c = bg_fine[order[at]][xs];
                if (c == TRANSPARENT || !(on & (1u << order[at]))) continue;
                second = top;
                second_color = top_color;
                second_prio = top_prio;
                top = order[at];
                top_color = c;
                top_prio = order_prio[at];
            }
            bool semi = false;
            if (obj_on && (on & 0x10u) && obj_color[xs] != TRANSPARENT) {
                if (obj_prio[xs] <= top_prio) {
                    second = top;
                    second_color = top_color;
                    top = LAYER_OBJ;
                    top_color = obj_color[xs];
                    semi = obj_semi[xs] != 0;
                } else if (obj_prio[xs] <= second_prio) {
                    second = LAYER_OBJ;
                    second_color = obj_color[xs];
                }
            }
            uint16_t color = top_color;
            const bool second_target = (bldcnt & (0x100u << second)) != 0;
            if (top == LAYER_OBJ && semi && second_target) {
                color = blend_alpha(top_color, second_color, eva, evb);
            } else if ((on & 0x20u) && (bldcnt & (1u << top))) {
                if (effect == 1 && second_target) color = blend_alpha(top_color, second_color, eva, evb);
                else if (effect == 2) color = blend_bright(top_color, evy, true);
                else if (effect == 3) color = blend_bright(top_color, evy, false);
            }
            out[xs] = bgr555_to_argb(color);
        }
    }
    large.fine_row[line] = 1;
    large.any = true;
}

void gba_ppu_render_line(GbaMachine *m, int line)
{
    uint32_t *fb = &m->framebuffer[line * W];
    uint16_t dispcnt = io16(m, 0);
    uint32_t mode = dispcnt & 7u;

    GbaTexRecord *rec = gba_tex_record;
    if (dispcnt & 0x80u) {
        for (int x = 0; x < W; x++) fb[x] = 0xFFFFFFFFu;
        if (rec) {
            memset(&rec->top[line * W], 0, sizeof(GbaTexSource) * W);
            memset(&rec->effect[line * W], GBA_TEX_EFFECT_NONE, W);
            memset(&rec->depth[line * W], 255, W);
            for (int x = 0; x < W; x++) rec->top_color[line * W + x] = 0x7FFF;
        }
        advance_refs(m);
        return;
    }

    static BgLine bgs[4];
    static ObjLine obj;
    static GbaTexSource bg_src[4][W], obj_src[W];
    bool bg_on[4] = { false, false, false, false };

    for (int bg = 0; bg < 4; bg++) {
        if (!(dispcnt & (0x100u << bg))) continue;
        switch (mode) {
        case 0:
            bg_on[bg] = true;
            render_text_bg(m, bg, line, &bgs[bg], rec ? bg_src[bg] : NULL);
            break;
        case 1:
            if (bg == 3) break;
            bg_on[bg] = true;
            if (bg == 2) render_affine_bg(m, 2, &bgs[bg]);
            else render_text_bg(m, bg, line, &bgs[bg], rec ? bg_src[bg] : NULL);
            if (bg == 2 && rec) memset(bg_src[bg], 0, sizeof bg_src[bg]);
            break;
        case 2:
            if (bg < 2) break;
            bg_on[bg] = true;
            render_affine_bg(m, bg, &bgs[bg]);
            if (rec) memset(bg_src[bg], 0, sizeof bg_src[bg]);
            break;
        case 3: case 4: case 5:
            if (bg != 2) break;
            bg_on[bg] = true;
            render_bitmap_bg(m, mode, &bgs[bg]);
            if (rec) memset(bg_src[bg], 0, sizeof bg_src[bg]);
            break;
        default:
            break;
        }
    }
    bool obj_on = (dispcnt & 0x1000u) != 0;
    if (obj_on || (dispcnt & 0x8000u)) render_sprites(m, line, &obj, rec ? obj_src : NULL);

    /* The enabled BGs in drawing order: by priority, lower-numbered first. */
    int order[4], order_prio[4], layers = 0;
    for (int p = 0; p < 4; p++) {
        for (int bg = 0; bg < 4; bg++) {
            if (bg_on[bg] && (io16(m, 0x08 + 2u * (uint32_t)bg) & 3u) == (uint32_t)p) {
                order[layers] = bg;
                order_prio[layers++] = p;
            }
        }
    }

    bool windows = (dispcnt & 0xE000u) != 0;
    uint16_t winin = io16(m, 0x48), winout = io16(m, 0x4A);
    uint16_t win0h = io16(m, 0x40), win1h = io16(m, 0x42);
    uint16_t win0v = io16(m, 0x44), win1v = io16(m, 0x46);
    bool in_win0_v = (dispcnt & 0x2000u) && in_range(line, win0v >> 8, win0v & 0xFF, 160);
    bool in_win1_v = (dispcnt & 0x4000u) && in_range(line, win1v >> 8, win1v & 0xFF, 160);

    uint16_t bldcnt = io16(m, 0x50);
    uint16_t bldalpha = io16(m, 0x52);
    uint32_t eva = bldalpha & 31u, evb = (bldalpha >> 8) & 31u, evy = io16(m, 0x54) & 31u;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    uint32_t effect = (bldcnt >> 6) & 3u;
    uint16_t backdrop = pal(m, 0);
    /* Only alpha blending looks at the layer under the top one. */
    const int want = (effect == 1 || (obj_on && obj.any_semi) || rec) ? 2 : 1;

    /* Per pixel: which layers (bits 0-4) and whether effects (bit 5) show. */
    static uint8_t enabled[W];
    if (windows) {
        for (int x = 0; x < W; x++) {
            if (in_win0_v && in_range(x, win0h >> 8, win0h & 0xFF, 240)) enabled[x] = winin & 0x3Fu;
            else if (in_win1_v && in_range(x, win1h >> 8, win1h & 0xFF, 240)) enabled[x] = (winin >> 8) & 0x3Fu;
            else if ((dispcnt & 0x8000u) && obj.window[x]) enabled[x] = (winout >> 8) & 0x3Fu;
            else enabled[x] = winout & 0x3Fu;
        }
    } else {
        memset(enabled, 0x3F, sizeof enabled);
    }

    /* The top two opaque layers of each pixel (the backdrop behind all):
     * paint the BGs back to front, lower-numbered BGs last since they win
     * ties, keeping what each new pixel covers as the second layer. Only
     * alpha blending needs the second layer. */
    static uint16_t top_c[W], sec_c[W], out_c[W];
    static uint8_t top_l[W], sec_l[W], top_p[W], sec_p[W];
    for (int x = 0; x < W; x++) {
        top_c[x] = sec_c[x] = backdrop;
        top_l[x] = sec_l[x] = LAYER_BD;
        top_p[x] = sec_p[x] = 4;
    }
    for (int k = layers - 1; k >= 0; k--) {
        const uint16_t *src = bgs[order[k]].color;
        const uint8_t bit = (uint8_t)(1u << order[k]), layer = (uint8_t)order[k], prio = (uint8_t)order_prio[k];
        if (want == 1) {
            for (int x = 0; x < W; x++) {
                if (src[x] == TRANSPARENT || !(enabled[x] & bit)) continue;
                top_c[x] = src[x];
                top_l[x] = layer;
                top_p[x] = prio;
            }
        } else {
            for (int x = 0; x < W; x++) {
                if (src[x] == TRANSPARENT || !(enabled[x] & bit)) continue;
                sec_c[x] = top_c[x];
                sec_l[x] = top_l[x];
                sec_p[x] = top_p[x];
                top_c[x] = src[x];
                top_l[x] = layer;
                top_p[x] = prio;
            }
        }
    }

    for (int x = 0; x < W; x++) {
        int top = top_l[x], second = sec_l[x];
        uint16_t top_color = top_c[x], second_color = sec_c[x];
        /* A sprite pixel goes in front of the BGs of its own priority and
         * lower, behind those of a higher one. */
        if (obj_on && (enabled[x] & 0x10u) && obj.color[x] != TRANSPARENT) {
            if (obj.prio[x] <= top_p[x]) {
                second = top;
                second_color = top_color;
                top = LAYER_OBJ;
                top_color = obj.color[x];
            } else if (obj.prio[x] <= sec_p[x]) {
                second = LAYER_OBJ;
                second_color = obj.color[x];
            }
        }

        uint16_t color = top_color;
        uint8_t applied = GBA_TEX_EFFECT_NONE;
        bool second_target = (bldcnt & (0x100u << second)) != 0;
        if (top == LAYER_OBJ && obj.semi[x] && second_target) {
            color = blend_alpha(top_color, second_color, eva, evb);
            applied = GBA_TEX_EFFECT_ALPHA;
        } else if ((enabled[x] & 0x20u) && (bldcnt & (1u << top))) {
            if (effect == 1 && second_target) {
                color = blend_alpha(top_color, second_color, eva, evb);
                applied = GBA_TEX_EFFECT_ALPHA;
            } else if (effect == 2) {
                color = blend_bright(top_color, evy, true);
                applied = GBA_TEX_EFFECT_BRIGHTEN;
            } else if (effect == 3) {
                color = blend_bright(top_color, evy, false);
                applied = GBA_TEX_EFFECT_DARKEN;
            }
        }
        out_c[x] = color;
        if (rec) {
            static const GbaTexSource none = { 0 };
            int i = line * W + x;
            rec->top[i] = top == LAYER_BD ? none : top == LAYER_OBJ ? obj_src[x] : bg_src[top][x];
            rec->second[i] = second == LAYER_BD ? none : second == LAYER_OBJ ? obj_src[x] : bg_src[second][x];
            rec->top_color[i] = top_color;
            rec->second_color[i] = second_color;
            rec->effect[i] = applied;
            /* Front to back: a sprite before the BGs of its priority, lower BGs before higher. */
            rec->depth[i] = top == LAYER_BD ? 255
                          : top == LAYER_OBJ ? (uint8_t)(obj.prio[x] * 8u)
                                             : (uint8_t)(top_p[x] * 8u + 1u + (uint32_t)top);
        }
    }
    for (int x = 0; x < W; x++) fb[x] = bgr555_to_argb(out_c[x]);
    if (rec) {
        rec->eva[line] = (uint8_t)eva;
        rec->evb[line] = (uint8_t)evb;
        rec->evy[line] = (uint8_t)evy;
    }

    if (io16(m, 0x02) & 1u) {
        /* Green swap: adjacent pixel pairs exchange their green channel. */
        for (int x = 0; x < W; x += 2) {
            uint32_t a = fb[x], b = fb[x + 1];
            fb[x] = (a & 0xFFFF00FFu) | (b & 0x0000FF00u);
            fb[x + 1] = (b & 0xFFFF00FFu) | (a & 0x0000FF00u);
        }
    }

    if (large.owner == m && line < GBA_SCREEN_HEIGHT)
        large_line(m, line, mode, bg_on, order, order_prio, layers, bgs, &obj, enabled, obj_on);

    advance_refs(m);
}
