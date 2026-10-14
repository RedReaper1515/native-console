/*
 * SNES PPU scanline renderer: backgrounds (modes 0-7, offset-per-tile,
 * hires, mosaic, direct color), sprites, windows, color math and the
 * INIDISP brightness stage, one picture line at a time.
 *
 * Ported from the ares "performance" PPU (ISC license, Near et al.),
 * https://github.com/ares-emulator/ares at
 * af4cbb04f067682a8a3cf42695ff78bed634b38d, ares/sfc/ppu-performance/:
 * background.cpp (render, getTile), mode7.cpp (renderMode7), object.cpp
 * (render), oam.cpp (object sizes), window.cpp, mosaic.cpp, dac.cpp
 * (prepare, render, pixel, blend, directColor) and the priority table of
 * io.cpp (updateVideoMode). Mode 7 vertical mosaic follows ares/sfc/ppu
 * (BG2 uses the BG1 enable).
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 *
 * Differences from ares: output goes to a 512 x 480 XRGB8888 buffer
 * (256 or 512 pixels per row) instead of a 564-wide 19-bit screen; the
 * brightness stage is the linear fullsnes one (0 = black, else
 * c * (N + 1) / 16) followed by the 5-to-8-bit expansion c << 3 | c >> 2;
 * a forced-blank line is black; scroll and offset-per-tile values are
 * taken as the hardware's 10 bits.
 */
#include "snes_ppu_internal.h"

#include <stdlib.h>
#include <string.h>

enum { SRC_BG1, SRC_BG2, SRC_BG3, SRC_BG4, SRC_OBJ1, SRC_OBJ2, SRC_COL };
enum { BPP2, BPP4, BPP8, MODE7, INACTIVE };

typedef struct {
    uint8_t source;
    uint8_t priority;
    uint8_t index;  /* CGRAM index (for the CGRAM address bus) */
    uint16_t color; /* BGR555 */
} Pixel;

/* One line's working set; lives on the stack of snes_ppu_render_line. */
typedef struct {
    int y;
    uint8_t layer_mode[4];
    uint8_t bg_priority[4][2];
    uint8_t obj_priority[4];
    Pixel above[256], below[256];
} Line;

/* ------------------------------------------------------- mode priorities */

typedef struct {
    uint8_t mode[4];
    uint8_t bg[4][2];
    uint8_t obj[4];
} ModeInfo;

/* Index: BG mode 0-7, then 8 = mode 1 with BG3 priority, 9 = mode 7 EXTBG. */
static const ModeInfo mode_table[10] = {
    { { BPP2, BPP2, BPP2, BPP2 }, { { 8, 11 }, { 7, 10 }, { 2, 5 }, { 1, 4 } }, { 3, 6, 9, 12 } },
    { { BPP4, BPP4, BPP2, INACTIVE }, { { 6, 9 }, { 5, 8 }, { 1, 3 }, { 0, 0 } }, { 2, 4, 7, 10 } },
    { { BPP4, BPP4, INACTIVE, INACTIVE }, { { 3, 7 }, { 1, 5 }, { 0, 0 }, { 0, 0 } }, { 2, 4, 6, 8 } },
    { { BPP8, BPP4, INACTIVE, INACTIVE }, { { 3, 7 }, { 1, 5 }, { 0, 0 }, { 0, 0 } }, { 2, 4, 6, 8 } },
    { { BPP8, BPP2, INACTIVE, INACTIVE }, { { 3, 7 }, { 1, 5 }, { 0, 0 }, { 0, 0 } }, { 2, 4, 6, 8 } },
    { { BPP4, BPP2, INACTIVE, INACTIVE }, { { 3, 7 }, { 1, 5 }, { 0, 0 }, { 0, 0 } }, { 2, 4, 6, 8 } },
    { { BPP4, INACTIVE, INACTIVE, INACTIVE }, { { 2, 5 }, { 0, 0 }, { 0, 0 }, { 0, 0 } }, { 1, 3, 4, 6 } },
    { { MODE7, INACTIVE, INACTIVE, INACTIVE }, { { 2, 2 }, { 0, 0 }, { 0, 0 }, { 0, 0 } }, { 1, 3, 4, 5 } },
    { { BPP4, BPP4, BPP2, INACTIVE }, { { 5, 8 }, { 4, 7 }, { 1, 10 }, { 0, 0 } }, { 2, 3, 6, 9 } },
    { { MODE7, MODE7, INACTIVE, INACTIVE }, { { 3, 3 }, { 1, 5 }, { 0, 0 }, { 0, 0 } }, { 2, 4, 6, 7 } },
};

static const ModeInfo *mode_info(const SnesPpu *p)
{
    unsigned mode = ppu_bg_mode(p);
    if (mode == 1 && (p->regs[0x05] & 0x08u)) return &mode_table[8];
    if (mode == 7 && ppu_extbg(p)) return &mode_table[9];
    return &mode_table[mode];
}

static bool bg_hires(const SnesPpu *p)
{
    unsigned mode = ppu_bg_mode(p);
    return mode == 5 || mode == 6;
}

static bool line_hires(const SnesPpu *p)
{
    return ppu_pseudo_hires(p) || bg_hires(p);
}

/* Arithmetic shift right without relying on implementation-defined >>. */
static int32_t asr(int32_t v, unsigned s)
{
    return v < 0 ? ~(~v >> s) : v >> s;
}

/* ---------------------------------------------------------------- window */

/* Layer windows (W12SEL/W34SEL/WOBJSEL nibble `sel`, WBGLOG/WOBJLOG pair
 * `logic`): out[x] = true where the layer is masked. */
static void layer_window(const SnesPpu *p, unsigned sel, unsigned logic, bool enable, bool out[256])
{
    bool one_invert = sel & 1u, one_enable = (sel >> 1) & 1u;
    bool two_invert = (sel >> 2) & 1u, two_enable = (sel >> 3) & 1u;
    int one_left = p->regs[0x26], one_right = p->regs[0x27];
    int two_left = p->regs[0x28], two_right = p->regs[0x29];

    if (!enable || (!one_enable && !two_enable)) {
        memset(out, 0, 256 * sizeof out[0]);
        return;
    }
    if (one_enable && !two_enable) {
        bool set = !one_invert, clear = !set;
        for (int x = 0; x < 256; x++) out[x] = x >= one_left && x <= one_right ? set : clear;
        return;
    }
    if (two_enable && !one_enable) {
        bool set = !two_invert, clear = !set;
        for (int x = 0; x < 256; x++) out[x] = x >= two_left && x <= two_right ? set : clear;
        return;
    }
    for (int x = 0; x < 256; x++) {
        bool one = (x >= one_left && x <= one_right) ^ one_invert;
        bool two = (x >= two_left && x <= two_right) ^ two_invert;
        switch (logic & 3u) {
        case 0: out[x] = one | two; break;
        case 1: out[x] = one & two; break;
        case 2: out[x] = one ^ two; break;
        default: out[x] = !(one ^ two); break;
        }
    }
}

/* Color window with a CGWSEL region `mask` (0 = always ... 3 = never). */
static void color_window(const SnesPpu *p, unsigned mask, bool out[256])
{
    unsigned sel = p->regs[0x25] >> 4, logic = (p->regs[0x2B] >> 2) & 3u;
    bool set = false, clear = false;
    switch (mask & 3u) {
    case 0: memset(out, 1, 256 * sizeof out[0]); return;
    case 1: set = true; clear = false; break;
    case 2: set = false; clear = true; break;
    default: memset(out, 0, 256 * sizeof out[0]); return;
    }

    bool one_invert = sel & 1u, one_enable = (sel >> 1) & 1u;
    bool two_invert = (sel >> 2) & 1u, two_enable = (sel >> 3) & 1u;
    int one_left = p->regs[0x26], one_right = p->regs[0x27];
    int two_left = p->regs[0x28], two_right = p->regs[0x29];

    if (!one_enable && !two_enable) {
        for (int x = 0; x < 256; x++) out[x] = clear;
        return;
    }
    if (one_enable && !two_enable) {
        if (one_invert) { set = !set; clear = !clear; }
        for (int x = 0; x < 256; x++) out[x] = x >= one_left && x <= one_right ? set : clear;
        return;
    }
    if (!one_enable && two_enable) {
        if (two_invert) { set = !set; clear = !clear; }
        for (int x = 0; x < 256; x++) out[x] = x >= two_left && x <= two_right ? set : clear;
        return;
    }
    for (int x = 0; x < 256; x++) {
        bool one = (x >= one_left && x <= one_right) ^ one_invert;
        bool two = (x >= two_left && x <= two_right) ^ two_invert;
        bool in;
        switch (logic) {
        case 0: in = one | two; break;
        case 1: in = one & two; break;
        case 2: in = one ^ two; break;
        default: in = !(one ^ two); break;
        }
        out[x] = in ? set : clear;
    }
}

static void bg_windows(const SnesPpu *p, unsigned id, bool above[256], bool below[256])
{
    unsigned sel = id < 2 ? p->regs[0x23] : p->regs[0x24];
    sel = (id & 1u) ? sel >> 4 : sel & 0x0Fu;
    unsigned logic = (p->regs[0x2A] >> (id * 2u)) & 3u;
    layer_window(p, sel, logic, (p->regs[0x2E] >> id) & 1u, above);
    layer_window(p, sel, logic, (p->regs[0x2F] >> id) & 1u, below);
}

/* ---------------------------------------------------------------- mosaic */

static void mosaic_scanline(SnesPpu *p, int y)
{
    unsigned size = ppu_mosaic_size(p);
    bool enable = ppu_mosaic_any(p);
    if (y == 1) p->mosaic_counter = (uint8_t)(enable ? size + 1u : 0u);
    if (p->mosaic_counter && !--p->mosaic_counter) p->mosaic_counter = (uint8_t)(enable ? size : 0u);
}

/* Lines since the current mosaic block started (wraps like ares' u32 if
 * the size shrinks mid-block). */
static uint32_t mosaic_voffset(const SnesPpu *p)
{
    return (uint32_t)ppu_mosaic_size(p) - p->mosaic_counter;
}

/* ------------------------------------------------------------------- DAC */

static uint16_t fixed_color(const SnesPpu *p)
{
    return (uint16_t)(p->fixed_r | (p->fixed_g << 5) | (p->fixed_b << 10));
}

/* palette = BBGGGRRR, group = bgr -> 0BBb00GG Gg0RRRr0 */
static uint16_t direct_color(unsigned palette, unsigned group)
{
    return (uint16_t)(((palette << 7) & 0x6000u) + ((group << 10) & 0x1000u)
                      + ((palette << 4) & 0x0380u) + ((group << 5) & 0x0040u)
                      + ((palette << 2) & 0x001Cu) + ((group << 1) & 0x0002u));
}

static void plot_above(Line *L, unsigned x, unsigned source, unsigned priority, unsigned index, uint16_t color)
{
    Pixel *px = &L->above[x];
    if (priority > px->priority) {
        px->source = (uint8_t)source;
        px->priority = (uint8_t)priority;
        px->index = (uint8_t)index;
        px->color = color;
    }
}

static void plot_below(Line *L, unsigned x, unsigned source, unsigned priority, unsigned index, uint16_t color)
{
    Pixel *px = &L->below[x];
    if (priority > px->priority) {
        px->source = (uint8_t)source;
        px->priority = (uint8_t)priority;
        px->index = (uint8_t)index;
        px->color = color;
    }
}

static void dac_prepare(const SnesPpu *p, Line *L)
{
    uint16_t above = ppu_cgram_color(p, 0);
    uint16_t below = line_hires(p) ? above : fixed_color(p);
    for (int x = 0; x < 256; x++) {
        L->above[x] = (Pixel){ SRC_COL, 0, 0, above };
        L->below[x] = (Pixel){ SRC_COL, 0, 0, below };
    }
}

static uint16_t blend(const SnesPpu *p, uint32_t x, uint32_t y, bool halve)
{
    if (!(p->regs[0x31] & 0x80u)) { /* add */
        if (!halve) {
            uint32_t sum = x + y;
            uint32_t carry = (sum - ((x ^ y) & 0x0421u)) & 0x8420u;
            return (uint16_t)(((sum - carry) | (carry - (carry >> 5))) & 0x7FFFu);
        }
        return (uint16_t)(((x + y - ((x ^ y) & 0x0421u)) >> 1) & 0x7FFFu);
    }
    /* subtract */
    uint32_t diff = x - y + 0x8420u;
    uint32_t borrow = (diff - ((x ^ y) & 0x8420u)) & 0x8420u;
    if (!halve) return (uint16_t)(((diff - borrow) & (borrow - (borrow >> 5))) & 0x7FFFu);
    return (uint16_t)(((((diff - borrow) & (borrow - (borrow >> 5))) & 0x7BDEu) >> 1) & 0x7FFFu);
}

/* Color math enables (CGADSUB) by source: BG1-4, OBJ palettes 0-3 (never),
 * OBJ palettes 4-7, backdrop. */
static bool color_enable(const SnesPpu *p, unsigned source)
{
    uint8_t r = p->regs[0x31];
    switch (source) {
    case SRC_BG1: case SRC_BG2: case SRC_BG3: case SRC_BG4: return (r >> source) & 1u;
    case SRC_OBJ1: return false;
    case SRC_OBJ2: return (r >> 4) & 1u;
    default: return (r >> 5) & 1u;
    }
}

static uint16_t dac_pixel(const SnesPpu *p, const bool *win_above, const bool *win_below, unsigned x,
                          Pixel above, Pixel below)
{
    bool halve = (p->regs[0x31] & 0x40u) != 0;
    if (!win_above[x]) above.color = 0;
    if (!win_below[x]) return above.color;
    if (!color_enable(p, above.source)) return above.color;
    if (!(p->regs[0x30] & 0x02u)) return blend(p, above.color, fixed_color(p), halve && win_above[x]);
    return blend(p, above.color, below.color, halve && win_above[x] && below.source != SRC_COL);
}

/* BGR555 -> XRGB8888 after the INIDISP brightness stage. */
static uint32_t to_xrgb(uint16_t c, unsigned brightness)
{
    uint32_t r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
    if (brightness == 0) return 0;
    if (brightness != 15) {
        r = r * (brightness + 1u) / 16u;
        g = g * (brightness + 1u) / 16u;
        b = b * (brightness + 1u) / 16u;
    }
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------------------ background */

static uint16_t bg_get_tile(const SnesPpu *p, unsigned id, uint32_t hoffset, uint32_t voffset)
{
    bool hires = bg_hires(p);
    unsigned tile_size = (p->regs[0x05] >> (4u + id)) & 1u;
    unsigned sc = p->regs[0x07 + id];
    unsigned screen_size = sc & 3u;
    uint32_t screen_addr = (uint32_t)(sc & 0xFCu) << 8;
    unsigned tile_height = 3u + tile_size;
    unsigned tile_width = hires ? 4u : tile_height;
    uint32_t screen_x = (screen_size & 1u) ? 32u << 5 : 0u;
    uint32_t screen_y = (screen_size & 2u) ? 32u << (5u + (screen_size & 1u)) : 0u;
    uint32_t tile_x = hoffset >> tile_width;
    uint32_t tile_y = voffset >> tile_height;
    uint32_t offset = (tile_y & 0x1Fu) << 5 | (tile_x & 0x1Fu);
    if (tile_x & 0x20u) offset += screen_x;
    if (tile_y & 0x20u) offset += screen_y;
    return ppu_vram_word(p, screen_addr + offset);
}

static void render_mode7(const SnesPpu *p, Line *L, unsigned id);

static void render_bg(const SnesPpu *p, Line *L, unsigned id)
{
    bool above_en = (p->regs[0x2C] >> id) & 1u, below_en = (p->regs[0x2D] >> id) & 1u;
    if (!above_en && !below_en) return;
    unsigned mode = L->layer_mode[id];
    if (mode == INACTIVE) return;
    if (mode == MODE7) {
        render_mode7(p, L, id);
        return;
    }

    bool win_above[256], win_below[256];
    bg_windows(p, id, win_above, win_below);

    unsigned bgmode = ppu_bg_mode(p);
    bool hires = bgmode == 5 || bgmode == 6;
    bool opt_mode = bgmode == 2 || bgmode == 4 || bgmode == 6;
    bool direct = (p->regs[0x30] & 1u) && id == 0 && (bgmode == 3 || bgmode == 4);
    bool interlace = ppu_interlace_reg(p);
    bool mosaic_en = (p->regs[0x06] >> id) & 1u;
    unsigned color_shift = 3u + mode;
    unsigned tile_size = (p->regs[0x05] >> (4u + id)) & 1u;
    unsigned screen_size = p->regs[0x07 + id] & 3u;
    unsigned nba = p->regs[0x0B + (id >> 1)];
    uint32_t tiledata_addr = (uint32_t)((nba >> ((id & 1u) * 4u)) & 0x0Fu) << 12;

    unsigned tile_height = 3u + tile_size;
    unsigned tile_width = hires ? 4u : tile_height;
    uint32_t tile_mask = 0x0FFFu >> mode;
    uint32_t tiledata_index = tiledata_addr >> (3u + mode);
    uint32_t palette_base = bgmode == 0 ? id << 5 : 0u;
    unsigned palette_shift = 2u << mode;

    uint32_t hscroll = p->bg_hofs[id] & 0x3FFu;
    uint32_t vscroll = p->bg_vofs[id] & 0x3FFu;
    uint32_t hmask = (256u << hires << tile_size << (screen_size & 1u)) - 1u;
    uint32_t vmask = (256u << hires << tile_size << ((screen_size >> 1) & 1u)) - 1u;

    uint32_t y = (uint32_t)L->y;
    if (hires) {
        hscroll <<= 1;
        if (interlace) y = y << 1 | (p->field && !mosaic_en ? 1u : 0u);
    }
    if (mosaic_en) y -= mosaic_voffset(p) << (hires && interlace ? 1u : 0u);

    unsigned mosaic_size = ppu_mosaic_size(p);
    uint32_t mosaic_counter = 1, mosaic_palette = 0, mosaic_priority = 0, mosaic_index = 0;
    uint16_t mosaic_color = 0;

    const int32_t x2 = 256 << hires;
    int32_t x = -(int32_t)(hscroll & 7u);
    while (x < x2) {
        uint32_t hoffset = (uint32_t)x + hscroll;
        uint32_t voffset = y + vscroll;
        if (opt_mode) {
            uint32_t valid_bit = 0x2000u << id;
            uint32_t offset_x = (uint32_t)x + (hscroll & 7u);
            if (offset_x >= (1u << tile_width)) { /* first column is exempt */
                uint32_t bx = (offset_x - (1u << tile_width)) + ((p->bg_hofs[2] & 0x3F8u) << hires);
                uint32_t by = p->bg_vofs[2] & 0x3FFu;
                uint32_t hlookup = bg_get_tile(p, 2, bx, by);
                if (bgmode == 4) {
                    if (hlookup & valid_bit) {
                        if (!(hlookup & 0x8000u)) hoffset = offset_x + (hlookup & 0x3F8u);
                        else voffset = y + (hlookup & 0x3FFu);
                    }
                } else {
                    uint32_t vlookup = bg_get_tile(p, 2, bx, by + 8u);
                    if (hlookup & valid_bit) hoffset = offset_x + (hlookup & 0x3F8u);
                    if (vlookup & valid_bit) voffset = y + (vlookup & 0x3FFu);
                }
            }
        }
        hoffset &= hmask;
        voffset &= vmask;

        uint32_t tile_number = bg_get_tile(p, id, hoffset, voffset);
        uint32_t mirror_y = (tile_number & 0x8000u) ? 7u : 0u;
        uint32_t mirror_x = (tile_number & 0x4000u) ? 7u : 0u;
        uint32_t tile_priority = L->bg_priority[id][(tile_number & 0x2000u) ? 1 : 0];
        uint32_t palette_number = (tile_number >> 10) & 7u;
        uint32_t palette_index = (palette_base + (palette_number << palette_shift)) & 0xFFu;

        if (tile_width == 4u && (((hoffset & 8u) != 0) ^ (mirror_x != 0))) tile_number += 1u;
        if (tile_height == 4u && (((voffset & 8u) != 0) ^ (mirror_y != 0))) tile_number += 16u;
        tile_number = ((tile_number & 0x03FFu) + tiledata_index) & tile_mask;

        uint32_t address = ((tile_number << color_shift) + ((voffset & 7u) ^ mirror_y)) & 0xFFFFu;
        uint64_t data = (uint64_t)ppu_vram_word(p, address + 0u)
                      | (uint64_t)ppu_vram_word(p, address + 8u) << 16
                      | (uint64_t)ppu_vram_word(p, address + 16u) << 32
                      | (uint64_t)ppu_vram_word(p, address + 24u) << 48;

        for (unsigned tile_x = 0; tile_x < 8; tile_x++, x++) {
            if (x < 0 || x >= x2) continue;
            if (--mosaic_counter == 0) {
                unsigned shift = mirror_x ? tile_x : 7u - tile_x;
                uint32_t color = 0;
                color += (uint32_t)(data >> (shift + 0)) & 1u;
                color += (uint32_t)(data >> (shift + 7)) & 2u;
                if (mode >= BPP4) {
                    color += (uint32_t)(data >> (shift + 14)) & 4u;
                    color += (uint32_t)(data >> (shift + 21)) & 8u;
                }
                if (mode >= BPP8) {
                    color += (uint32_t)(data >> (shift + 28)) & 16u;
                    color += (uint32_t)(data >> (shift + 35)) & 32u;
                    color += (uint32_t)(data >> (shift + 42)) & 64u;
                    color += (uint32_t)(data >> (shift + 49)) & 128u;
                }
                mosaic_counter = mosaic_en ? mosaic_size << hires : 1u;
                mosaic_palette = color;
                mosaic_priority = tile_priority;
                mosaic_index = (palette_index + color) & 0xFFu;
                if (direct) mosaic_color = direct_color(color, palette_number);
                else mosaic_color = ppu_cgram_color(p, (uint8_t)mosaic_index);
            }
            if (!mosaic_palette) continue;

            unsigned xp = (unsigned)x;
            if (!hires) {
                if (above_en && !win_above[xp]) plot_above(L, xp, id, mosaic_priority, mosaic_index, mosaic_color);
                if (below_en && !win_below[xp]) plot_below(L, xp, id, mosaic_priority, mosaic_index, mosaic_color);
            } else {
                unsigned half = xp >> 1;
                if (xp & 1u) {
                    if (above_en && !win_above[half]) plot_above(L, half, id, mosaic_priority, mosaic_index, mosaic_color);
                } else {
                    if (below_en && !win_below[half]) plot_below(L, half, id, mosaic_priority, mosaic_index, mosaic_color);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- mode 7 */

static int32_t sext13(uint16_t v)
{
    return (int32_t)((v & 0x1FFFu) ^ 0x1000u) - 0x1000;
}

/* 13-bit sign extend: --s---nnnnnnnnnn -> ssssssnnnnnnnnnn */
static int32_t m7_clip(int32_t n)
{
    return (n & 0x2000) ? (n | ~1023) : (n & 1023);
}

static void render_mode7(const SnesPpu *p, Line *L, unsigned id)
{
    bool above_en = (p->regs[0x2C] >> id) & 1u, below_en = (p->regs[0x2D] >> id) & 1u;
    uint8_t m7sel = p->regs[0x1A];
    bool hflip = m7sel & 1u, vflip = (m7sel >> 1) & 1u;
    unsigned repeat = m7sel >> 6;
    bool mosaic_en = (p->regs[0x06] >> id) & 1u;
    bool direct = (p->regs[0x30] & 1u) && id == 0;

    int32_t Y = L->y;
    if (p->regs[0x06] & 1u) Y -= (int32_t)mosaic_voffset(p); /* BG2 uses the BG1 enable */
    int32_t y = !vflip ? Y : 255 - Y;

    int32_t a = (int16_t)p->m7a, b = (int16_t)p->m7b, c = (int16_t)p->m7c, d = (int16_t)p->m7d;
    int32_t hcenter = sext13(p->m7x), vcenter = sext13(p->m7y);
    int32_t hoffset = sext13(p->m7_hofs), voffset = sext13(p->m7_vofs);

    int32_t hc = m7_clip(hoffset - hcenter), vc = m7_clip(voffset - vcenter);
    int32_t origin_x = ((a * hc) & ~63) + ((b * vc) & ~63) + ((b * y) & ~63) + hcenter * 256;
    int32_t origin_y = ((c * hc) & ~63) + ((d * vc) & ~63) + ((d * y) & ~63) + vcenter * 256;

    bool win_above[256], win_below[256];
    bg_windows(p, id, win_above, win_below);

    unsigned mosaic_size = ppu_mosaic_size(p);
    uint32_t mosaic_counter = 1, mosaic_palette = 0, mosaic_priority = 0;
    uint16_t mosaic_color = 0;

    for (int32_t X = 0; X <= 255; X++) {
        int32_t x = !hflip ? X : 255 - X;
        int32_t pixel_x = asr(origin_x + a * x, 8);
        int32_t pixel_y = asr(origin_y + c * x, 8);
        uint32_t tile_x = (uint32_t)asr(pixel_x, 3) & 127u;
        uint32_t tile_y = (uint32_t)asr(pixel_y, 3) & 127u;
        bool out_of_bounds = ((pixel_x | pixel_y) & ~1023) != 0;
        uint32_t tile_address = tile_y << 7 | tile_x;
        uint32_t palette_address = ((uint32_t)pixel_y & 7u) << 3 | ((uint32_t)pixel_x & 7u);

        uint32_t tile = repeat == 3 && out_of_bounds ? 0u : ppu_vram_word(p, tile_address) & 0xFFu;
        uint32_t palette = repeat == 2 && out_of_bounds ? 0u : ppu_vram_word(p, tile << 6 | palette_address) >> 8;

        uint32_t priority;
        if (id == 0) {
            priority = L->bg_priority[0][0];
        } else {
            priority = L->bg_priority[id][palette >> 7];
            palette &= 0x7Fu;
        }

        if (--mosaic_counter == 0) {
            mosaic_counter = mosaic_en ? mosaic_size : 1u;
            mosaic_palette = palette;
            mosaic_priority = priority;
            if (direct) mosaic_color = direct_color(palette, 0);
            else mosaic_color = ppu_cgram_color(p, (uint8_t)palette);
        }
        if (!mosaic_palette) continue;

        unsigned xp = (unsigned)X;
        if (above_en && !win_above[xp]) plot_above(L, xp, id, mosaic_priority, mosaic_palette, mosaic_color);
        if (below_en && !win_below[xp]) plot_below(L, xp, id, mosaic_priority, mosaic_palette, mosaic_color);
    }
}

/* ---------------------------------------------------------------- objects */

static unsigned obj_width(const SnesPpu *p, bool large)
{
    static const uint8_t small_w[8] = { 8, 8, 8, 16, 16, 32, 16, 16 };
    static const uint8_t large_w[8] = { 16, 32, 64, 32, 64, 64, 32, 32 };
    unsigned base = p->regs[0x01] >> 5;
    return large ? large_w[base] : small_w[base];
}

static unsigned obj_height(const SnesPpu *p, bool large)
{
    static const uint8_t small_h[8] = { 8, 8, 8, 16, 16, 32, 32, 32 };
    static const uint8_t large_h[8] = { 16, 32, 64, 32, 64, 64, 64, 32 };
    unsigned base = p->regs[0x01] >> 5;
    if (!large && ppu_obj_interlace(p) && base >= 6) return 16; /* hardware quirk */
    return large ? large_h[base] : small_h[base];
}

typedef struct {
    uint16_t x;  /* 9-bit */
    uint8_t y;
    uint8_t character;
    uint8_t attr;
    bool large;
} Sprite;

static Sprite sprite_at(const SnesPpu *p, unsigned n)
{
    Sprite s;
    const uint8_t *e = p->oam + n * 4u;
    uint8_t hi = p->oam[0x200u + (n >> 2)];
    unsigned shift = (n & 3u) * 2u;
    s.x = (uint16_t)(e[0] | (((hi >> shift) & 1u) << 8));
    s.y = e[1];
    s.character = e[2];
    s.attr = e[3];
    s.large = (hi >> (shift + 1u)) & 1u;
    return s;
}

typedef struct {
    uint16_t x;
    uint8_t priority;
    uint8_t palette;
    bool hflip;
    uint32_t data;
} ObjTile;

static void render_obj(SnesPpu *p, Line *L)
{
    bool above_en = (p->regs[0x2C] >> 4) & 1u, below_en = (p->regs[0x2D] >> 4) & 1u;
    if (!above_en && !below_en) return;

    bool win_above[256], win_below[256];
    unsigned sel = p->regs[0x25] & 0x0Fu, logic = p->regs[0x2B] & 3u;
    layer_window(p, sel, logic, (p->regs[0x2E] >> 4) & 1u, win_above);
    layer_window(p, sel, logic, (p->regs[0x2F] >> 4) & 1u, win_below);

    bool interlace = ppu_obj_interlace(p);
    uint8_t obsel = p->regs[0x01];
    uint32_t tiledata_base = (uint32_t)(obsel & 7u) << 13;
    unsigned nameselect = (obsel >> 3) & 3u;
    /* Sprites are evaluated on the line before they show: OAM Y = N shows
     * from picture line N + 1 (output row N). */
    unsigned line = (unsigned)(L->y - 1) & 0xFFu;

    uint8_t items[32], item_row[32];
    unsigned item_count = 0;
    for (unsigned n = 0; n < 128; n++) {
        unsigned index = (p->first_sprite + n) & 127u;
        Sprite s = sprite_at(p, index);
        unsigned w = obj_width(p, s.large);
        if (s.x > 256 && s.x + w - 1 < 512) continue;
        unsigned h = obj_height(p, s.large) >> (interlace ? 1 : 0);
        unsigned row = (line - s.y) & 0xFFu;
        if (row >= h) continue;
        if (item_count++ >= 32) break;
        items[item_count - 1] = (uint8_t)index;
        item_row[item_count - 1] = (uint8_t)row;
    }

    ObjTile tiles[34];
    unsigned tile_count = 0;
    unsigned valid_items = item_count > 32 ? 32 : item_count;
    for (int i = (int)valid_items - 1; i >= 0; i--) {
        Sprite s = sprite_at(p, items[i]);
        unsigned w = obj_width(p, s.large), h = obj_height(p, s.large);
        unsigned tile_width = w >> 3;
        int32_t x = s.x;
        int32_t y = item_row[i];
        bool hflip = (s.attr >> 6) & 1u, vflip = (s.attr >> 7) & 1u;
        if (interlace) y <<= 1;

        if (vflip) {
            if (w == h) y = (int32_t)h - 1 - y;
            else if (y < (int32_t)w) y = (int32_t)w - 1 - y;
            else y = (int32_t)w + ((int32_t)w - 1) - (y - (int32_t)w);
        }
        if (interlace) y = !vflip ? y + (p->field ? 1 : 0) : y - (p->field ? 1 : 0);

        x &= 511;
        y &= 255;

        uint32_t tiledata = tiledata_base;
        if (s.attr & 1u) tiledata += (1u + nameselect) << 12;
        tiledata &= 0xFFFFu;
        uint32_t character_x = s.character & 15u;
        uint32_t character_y = ((((uint32_t)s.character >> 4) + ((uint32_t)y >> 3)) & 15u) << 4;

        for (unsigned tx = 0; tx < tile_width; tx++) {
            uint32_t object_x = ((uint32_t)x + (tx << 3)) & 511u;
            if (x != 256 && object_x >= 256 && object_x + 7 < 512) continue;

            unsigned mirror_x = !hflip ? tx : tile_width - 1 - tx;
            uint32_t address = (tiledata + ((character_y + ((character_x + mirror_x) & 15u)) << 4)) & 0xFFFFu;
            address = (address & 0xFFF0u) + ((uint32_t)y & 7u);

            if (tile_count++ >= 34) break;
            ObjTile *t = &tiles[tile_count - 1];
            t->x = (uint16_t)object_x;
            t->priority = (uint8_t)((s.attr >> 4) & 3u);
            t->palette = (uint8_t)(128u + (((s.attr >> 1) & 7u) << 4));
            t->hflip = hflip;
            t->data = (uint32_t)ppu_vram_word(p, address) | (uint32_t)ppu_vram_word(p, address + 8u) << 16;
        }
    }

    p->range_over = p->range_over || item_count > 32;
    p->time_over = p->time_over || tile_count > 34;

    uint8_t palette[256], priority[256];
    memset(palette, 0, sizeof palette);
    memset(priority, 0, sizeof priority);

    unsigned drawn = tile_count > 34 ? 34 : tile_count;
    for (unsigned n = 0; n < drawn; n++) {
        const ObjTile *t = &tiles[n];
        uint32_t tile_x = t->x;
        for (unsigned px = 0; px < 8; px++) {
            tile_x &= 511u;
            if (tile_x < 256) {
                unsigned shift = t->hflip ? px : 7u - px;
                uint32_t color = 0;
                color += (t->data >> (shift + 0)) & 1u;
                color += (t->data >> (shift + 7)) & 2u;
                color += (t->data >> (shift + 14)) & 4u;
                color += (t->data >> (shift + 21)) & 8u;
                if (color) {
                    palette[tile_x] = (uint8_t)(t->palette + color);
                    priority[tile_x] = L->obj_priority[t->priority];
                }
            }
            tile_x++;
        }
    }

    for (unsigned x = 0; x < 256; x++) {
        if (!priority[x]) continue;
        unsigned source = palette[x] < 192 ? SRC_OBJ1 : SRC_OBJ2;
        uint16_t color = ppu_cgram_color(p, palette[x]);
        if (above_en && !win_above[x]) plot_above(L, x, source, priority[x], palette[x], color);
        if (below_en && !win_below[x]) plot_below(L, x, source, priority[x], palette[x], color);
    }
}

/* ------------------------------------------------- mode 7, drawn larger */

/*
 * snes_ppu_set_mode7_scale(): the mode 7 plane sampled `scale` times a
 * pixel each way. The console's line is drawn first and as always; a line
 * that shows the plane is then drawn again, `scale` rows of 256 * scale
 * samples.
 *
 * A sample is the plane at a fraction of a pixel: the same matrix and
 * origin as render_mode7(), with the screen position in scale-ths. Games
 * that tilt the plane give every line its own matrix, so the rows between
 * line y and line y + 1 use the matrix carried on from line y by its
 * change since line y - 1, where that change is small enough to be one
 * surface and not a cut between two. The arithmetic is exact integers
 * (the products are not rounded to 64ths as the console's are), so a
 * sample at a whole pixel is the console's pixel whenever the console's
 * rounding loses nothing, and near it otherwise.
 *
 * Everything else on the line - sprites, the backdrop, windows, colour
 * math - is the console's, per pixel: a sample is put through the same
 * priority and colour stages as the plane's own pixel was.
 *
 * The picture lives here, not in SnesPpu, which a save state copies byte
 * for byte: one PPU at a time has one.
 */
typedef struct {
    int32_t a, b, c, d;           /* the matrix */
    int32_t hc, vc;               /* scroll less centre, clipped */
    int32_t hcenter, vcenter;
} M7;

static struct {
    const SnesPpu *owner;
    int scale;
    uint32_t *frame;       /* 256 * scale by 239 * scale */
    uint8_t fine_row[240]; /* the frame's rows drawn larger so far */
    bool any;
    bool valid;            /* the last complete frame has a larger picture */
    unsigned valid_rows;
    /* The line before, for the matrix between lines. */
    M7 prev;
    int prev_y;
    uint8_t prev_sel;
} large;

bool snes_ppu_set_mode7_scale(SnesPpu *p, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > SNES_MODE7_SCALE_MAX) return false;
    if (scale == 1) {
        if (large.owner == p) {
            free(large.frame);
            memset(&large, 0, sizeof large);
        }
        return true;
    }
    if (large.owner == p && large.scale == scale) return true;
    uint32_t *frame = (uint32_t *)calloc((size_t)256 * (size_t)scale * 239u * (size_t)scale, sizeof(uint32_t));
    if (!frame) return false;
    free(large.frame);
    memset(&large, 0, sizeof large);
    large.owner = p;
    large.scale = scale;
    large.frame = frame;
    large.prev_y = -2;
    return true;
}

int snes_ppu_mode7_scale(const SnesPpu *p) { return large.owner == p ? large.scale : 1; }

const uint32_t *snes_ppu_large_frame(const SnesPpu *p, int *width, int *height)
{
    if (large.owner != p || !large.valid) return NULL;
    if (width) *width = 256 * large.scale;
    if (height) *height = (int)large.valid_rows * large.scale;
    return large.frame;
}

void ppu_large_frame_start(const SnesPpu *p)
{
    if (large.owner != p) return;
    memset(large.fine_row, 0, sizeof large.fine_row);
    large.any = false;
    large.prev_y = -2;
}

/* The frame is complete: its other rows are the console's, repeated. */
void ppu_large_frame_end(const SnesPpu *p, unsigned rows)
{
    if (large.owner != p) return;
    large.valid = large.any && !p->frame_interlace && rows <= 239u;
    if (!large.valid) return;
    large.valid_rows = rows;
    const unsigned n = (unsigned)large.scale, pitch = 256u * n;
    for (unsigned r = 0; r < rows; r++) {
        if (large.fine_row[r]) continue;
        const uint32_t *src = p->framebuffer + r * SNES_FB_WIDTH;
        const unsigned src_width = p->row_wide[r] ? 512u : 256u;
        uint32_t *out = large.frame + (size_t)r * n * pitch;
        for (unsigned x = 0; x < pitch; x++) out[x] = src[x * src_width / pitch];
        for (unsigned k = 1; k < n; k++) memcpy(out + (size_t)k * pitch, out, pitch * sizeof out[0]);
    }
}

static void m7_decode(const SnesPpu *p, M7 *m)
{
    m->a = (int16_t)p->m7a;
    m->b = (int16_t)p->m7b;
    m->c = (int16_t)p->m7c;
    m->d = (int16_t)p->m7d;
    m->hcenter = sext13(p->m7x);
    m->vcenter = sext13(p->m7y);
    m->hc = m7_clip(sext13(p->m7_hofs) - m->hcenter);
    m->vc = m7_clip(sext13(p->m7_vofs) - m->vcenter);
}

static bool m7_near(int32_t now, int32_t before, int32_t slack)
{
    int32_t change = now > before ? now - before : before - now;
    return change <= slack;
}

/* floor(v / (256 * n * n)): a plane coordinate from 256ths of a texel
 * counted in n-ths twice over. The divisors are constants so that each is
 * a multiplication. */
static inline int32_t m7_whole(int64_t v, int n)
{
#define M7_FLOOR(D) (int32_t)(v >= 0 ? v / (D) : -((-v + (D) - 1) / (D)))
    switch (n) {
    case 2: return M7_FLOOR(1024);
    case 3: return M7_FLOOR(2304);
    default: return M7_FLOOR(4096);
    }
#undef M7_FLOOR
}

static inline void plot_pixel(Pixel *px, unsigned source, unsigned priority, unsigned index, uint16_t color)
{
    if (priority > px->priority) {
        px->source = (uint8_t)source;
        px->priority = (uint8_t)priority;
        px->index = (uint8_t)index;
        px->color = color;
    }
}

static void render_obj(SnesPpu *p, Line *L);

/* Line y again, larger, after the console's own: L is that line's working
 * set, and the colour windows and brightness are the ones it used. */
static void large_line(SnesPpu *p, const Line *L, int y, unsigned row, const bool *color_above,
                       const bool *color_below, unsigned brightness)
{
    const int n = large.scale;
    const unsigned ids = L->layer_mode[1] == MODE7 ? 2u : 1u;
    bool above_en[2] = { false, false }, below_en[2] = { false, false }, shown = false;
    for (unsigned id = 0; id < ids; id++) {
        above_en[id] = (p->regs[0x2C] >> id) & 1u;
        below_en[id] = (p->regs[0x2D] >> id) & 1u;
        shown = shown || above_en[id] || below_en[id];
    }
    if (L->layer_mode[0] != MODE7 || !shown || p->frame_interlace || line_hires(p) || row >= 239u) return;
    if ((p->regs[0x06] & 3u) && ppu_mosaic_size(p) > 1u) return;

    /* The line without the plane: the backdrop and the sprites. */
    Line base;
    base.y = y;
    memcpy(base.layer_mode, L->layer_mode, sizeof base.layer_mode);
    memcpy(base.bg_priority, L->bg_priority, sizeof base.bg_priority);
    memcpy(base.obj_priority, L->obj_priority, sizeof base.obj_priority);
    dac_prepare(p, &base);
    render_obj(p, &base);

    bool win_above[2][256], win_below[2][256];
    for (unsigned id = 0; id < ids; id++) bg_windows(p, id, win_above[id], win_below[id]);

    const uint8_t m7sel = p->regs[0x1A];
    const bool hflip = m7sel & 1u, vflip = (m7sel >> 1) & 1u;
    const unsigned repeat = m7sel >> 6;
    const bool direct = (p->regs[0x30] & 1u) != 0;

    /* The matrix a row further down: this line's, carried on by its change
     * since the line above where the two are one surface. */
    M7 now, step;
    m7_decode(p, &now);
    memset(&step, 0, sizeof step);
    if (large.prev_y == y - 1 && large.prev_sel == m7sel) {
        const M7 *was = &large.prev;
        int32_t biggest = 0;
        const int32_t parts[4] = { now.a, now.b, now.c, now.d };
        for (int i = 0; i < 4; i++) {
            int32_t size = parts[i] < 0 ? -parts[i] : parts[i];
            if (size > biggest) biggest = size;
        }
        const int32_t slack = (biggest >> 3) + 2;
        if (m7_near(now.a, was->a, slack) && m7_near(now.b, was->b, slack) && m7_near(now.c, was->c, slack) &&
            m7_near(now.d, was->d, slack) && m7_near(now.hc, was->hc, 8) && m7_near(now.vc, was->vc, 8) &&
            m7_near(now.hcenter, was->hcenter, 8) && m7_near(now.vcenter, was->vcenter, 8)) {
            step.a = now.a - was->a;
            step.b = now.b - was->b;
            step.c = now.c - was->c;
            step.d = now.d - was->d;
            step.hc = now.hc - was->hc;
            step.vc = now.vc - was->vc;
            step.hcenter = now.hcenter - was->hcenter;
            step.vcenter = now.vcenter - was->vcenter;
        }
    }

    const size_t pitch = (size_t)256 * (size_t)n;
    for (int k = 0; k < n; k++) {
        /* Everything in n-ths: the matrix and the offsets at y + k / n. */
        const int64_t a = (int64_t)now.a * n + (int64_t)step.a * k, b = (int64_t)now.b * n + (int64_t)step.b * k;
        const int64_t c = (int64_t)now.c * n + (int64_t)step.c * k, d = (int64_t)now.d * n + (int64_t)step.d * k;
        const int64_t hc = (int64_t)now.hc * n + (int64_t)step.hc * k, vc = (int64_t)now.vc * n + (int64_t)step.vc * k;
        const int64_t hcenter = (int64_t)now.hcenter * n + (int64_t)step.hcenter * k;
        const int64_t vcenter = (int64_t)now.vcenter * n + (int64_t)step.vcenter * k;
        const int64_t ys = !vflip ? (int64_t)y * n + k : (int64_t)255 * n - ((int64_t)y * n + k);
        const int64_t origin_x = a * hc + b * vc + b * ys + hcenter * 256 * n;
        const int64_t origin_y = c * hc + d * vc + d * ys + vcenter * 256 * n;

        uint32_t *out = large.frame + ((size_t)row * (size_t)n + (size_t)k) * pitch;
        int32_t held_x = INT32_MIN, held_y = INT32_MIN;
        uint32_t held_palette = 0;
        for (int X = 0; X < 256; X++) {
            uint32_t done_palette = ~0u, done_color = 0;
            for (int j = 0; j < n; j++) {
                const int64_t xs = !hflip ? (int64_t)X * n + j : (int64_t)255 * n - ((int64_t)X * n + j);
                const int32_t pixel_x = m7_whole(origin_x + a * xs, n), pixel_y = m7_whole(origin_y + c * xs, n);
                /* The plane's byte here, kept while the samples stay on one texel. */
                if (pixel_x != held_x || pixel_y != held_y) {
                    const uint32_t tile_x = (uint32_t)asr(pixel_x, 3) & 127u, tile_y = (uint32_t)asr(pixel_y, 3) & 127u;
                    const bool out_of_bounds = ((pixel_x | pixel_y) & ~1023) != 0;
                    const uint32_t palette_address = ((uint32_t)pixel_y & 7u) << 3 | ((uint32_t)pixel_x & 7u);
                    const uint32_t tile =
                        repeat == 3 && out_of_bounds ? 0u : ppu_vram_word(p, tile_y << 7 | tile_x) & 0xFFu;
                    held_palette = repeat == 2 && out_of_bounds ? 0u : ppu_vram_word(p, tile << 6 | palette_address) >> 8;
                    held_x = pixel_x;
                    held_y = pixel_y;
                }
                if (held_palette != done_palette) {
                    Pixel above = base.above[X], below = base.below[X];
                    for (unsigned id = 0; id < ids; id++) {
                        uint32_t palette = held_palette, priority = L->bg_priority[0][0];
                        if (id == 1) {
                            priority = L->bg_priority[1][palette >> 7];
                            palette &= 0x7Fu;
                        }
                        if (!palette) continue;
                        const uint16_t color =
                            direct && id == 0 ? direct_color(palette, 0) : ppu_cgram_color(p, (uint8_t)palette);
                        if (above_en[id] && !win_above[id][X]) plot_pixel(&above, id, priority, palette, color);
                        if (below_en[id] && !win_below[id][X]) plot_pixel(&below, id, priority, palette, color);
                    }
                    done_palette = held_palette;
                    done_color = to_xrgb(dac_pixel(p, color_above, color_below, (unsigned)X, above, below), brightness);
                }
                out[(size_t)X * (size_t)n + (size_t)j] = done_color;
            }
        }
    }
    large.fine_row[row] = 1;
    large.any = true;
    large.prev = now;
    large.prev_y = y;
    large.prev_sel = m7sel;
}

/* ------------------------------------------------------------------ line */

void snes_ppu_render_line(SnesPpu *p, int y)
{
    if (y < 1 || y > 239) return;
    if ((unsigned)y > p->frame_last_line) p->frame_last_line = (uint16_t)y;

    unsigned row = p->frame_interlace ? (unsigned)(y - 1) * 2u + (p->field ? 1u : 0u) : (unsigned)(y - 1);
    uint32_t *out = p->framebuffer + row * SNES_FB_WIDTH;

    mosaic_scanline(p, y);

    if (ppu_forced_blank(p)) {
        memset(out, 0, 256 * sizeof out[0]);
        p->row_wide[row] = 0;
        memset(p->cgram_pixel, 0, sizeof p->cgram_pixel);
        return;
    }

    Line L;
    const ModeInfo *mi = mode_info(p);
    L.y = y;
    memcpy(L.layer_mode, mi->mode, sizeof L.layer_mode);
    memcpy(L.bg_priority, mi->bg, sizeof L.bg_priority);
    memcpy(L.obj_priority, mi->obj, sizeof L.obj_priority);

    dac_prepare(p, &L);
    for (unsigned id = 0; id < 4; id++) render_bg(p, &L, id);
    render_obj(p, &L);

    bool win_above[256], win_below[256];
    color_window(p, p->regs[0x30] >> 6, win_above);
    color_window(p, (p->regs[0x30] >> 4) & 3u, win_below);

    unsigned brightness = p->regs[0x00] & 0x0Fu;
    if (!line_hires(p)) {
        for (unsigned x = 0; x < 256; x++)
            out[x] = to_xrgb(dac_pixel(p, win_above, win_below, x, L.above[x], L.below[x]), brightness);
        p->row_wide[row] = 0;
    } else {
        for (unsigned x = 0; x < 256; x++) {
            out[x * 2] = to_xrgb(dac_pixel(p, win_above, win_below, x, L.below[x], L.above[x]), brightness);
            out[x * 2 + 1] = to_xrgb(dac_pixel(p, win_above, win_below, x, L.above[x], L.below[x]), brightness);
        }
        p->row_wide[row] = 1;
    }

    for (unsigned x = 0; x < 256; x++) p->cgram_pixel[x] = L.above[x].index;

    if (large.owner == p) large_line(p, &L, y, row, win_above, win_below, brightness);
}
