/*
 * NES PPU (NTSC RP2C02G): registers $2000-$2007, the dot clock, the
 * background fetch pipeline, sprite evaluation and output, VBlank and NMI.
 * The RGB palette is in nes_ppu_palette.c.
 *
 * Hardware reference: the nesdev wiki (PPU rendering, PPU scrolling, PPU
 * sprite evaluation, PPU registers, PPU frame timing, PPU power up state).
 *
 * Parts are ported from ares (ISC license, Near et al.),
 * https://github.com/ares-emulator/ares at
 * a776c509, ares/fc/ppu:
 *   - sprite.cpp: the sprite evaluation state machine (dots 65-256, with
 *     the sprite overflow bug and misaligned OAMADDR), the OAM data latch
 *     at dots 257-340, and the copy of 8 OAM bytes when rendering starts
 *     with OAMADDR >= 8;
 *   - render.cpp: the sprite output units (X counters, shifters, priority
 *     and the sprite 0 hit condition), the sprite pattern address, and the
 *     backdrop taken from the palette entry v points at when rendering is
 *     off;
 *   - scroll.cpp: the delayed v <- t copy after the second $2006 write and
 *     its AND with v when it lands on a scroll increment;
 *   - memory.cpp: the $2004 access rules during rendering, the $2007
 *     access during rendering (bus address, both increments), and the
 *     ignored $2007 read on the CPU cycle after another;
 *   - ppu.hpp: the power-on palette RAM values.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 *
 * Written from the wiki, not ares: the dot loop and its position
 * convention, VBlank/NMI and the $2002 race, the odd-frame skip, the
 * background pipeline placement (pixel x is produced at dot x + 2, where
 * the wiki puts the first shift and the earliest sprite 0 hit; shifters
 * reload at dots 9, 17, .., 257 and 329, 337), the garbage nametable
 * fetches of the sprite slots, the I/O latch with decay, reset, and the
 * address bus returning to v when rendering stops.
 *
 * Not modelled: the 2-4 dot delay before a PPUMASK write takes effect; the
 * dot-257 bus conflicts of $2000/$2005/$2006 writes (they need the CPU's
 * open bus); OAM decay, and the OAM corruption caused by switching
 * rendering off in mid-line or by writing OAMADDR; the 2C02G's output
 * delay (a pixel leaves the chip two dots after it is produced here).
 *
 * PAL and Dendy are not supported. Besides the line counts (nes_ppu.h)
 * they need: 3.2 dots per CPU cycle (the machine), sprite evaluation and
 * OAM refresh on the extra VBlank lines, the forced border, the swapped
 * red/green emphasis bits, and a PAL palette.
 */
#include "nes_ppu.h"

#include <stddef.h>
#include <string.h>

/* The struct is saved and compared as one block: no padding anywhere.
 * (Checked for 64-bit pointers; `user` is the only field that varies.) */
_Static_assert(sizeof(NesPpuSprite) == 8, "NesPpuSprite has padding");
_Static_assert(sizeof(void *) != 8 || offsetof(NesPpu, frame) == 42, "NesPpu has padding before frame");
_Static_assert(sizeof(void *) != 8 || offsetof(NesPpu, frame_ready) == 42 + 2 * NES_PPU_WIDTH * NES_PPU_HEIGHT,
               "NesPpu has padding after frame");
_Static_assert(sizeof(void *) != 8 || offsetof(NesPpu, reserved) == 123345, "NesPpu has padding in its byte fields");
_Static_assert(sizeof(void *) != 8 || sizeof(NesPpu) == 123352, "NesPpu has tail padding");

enum {
    DOTS = NES_PPU_DOTS_PER_LINE,
    VISIBLE_LINES = NES_PPU_HEIGHT,
    POST_RENDER_LINE = NES_PPU_HEIGHT,

    CTRL_INCREMENT_32 = 0x04,
    CTRL_SPRITE_TABLE = 0x08,
    CTRL_BG_TABLE = 0x10,
    CTRL_SPRITE_16 = 0x20,
    CTRL_NMI = 0x80,

    MASK_GREYSCALE = 0x01,
    MASK_BG_LEFT = 0x02,
    MASK_SPRITE_LEFT = 0x04,
    MASK_BG = 0x08,
    MASK_SPRITES = 0x10,
    MASK_RENDER = MASK_BG | MASK_SPRITES,

    /* v/t fields. */
    V_COARSE_X = 0x001F,
    V_COARSE_Y = 0x03E0,
    V_NT_X = 0x0400,
    V_NT_Y = 0x0800,
    V_FINE_Y = 0x7000,
    V_HORIZONTAL = V_COARSE_X | V_NT_X,
    V_VERTICAL = V_COARSE_Y | V_NT_Y | V_FINE_Y,

    EMPTY_SLOT = 64,

    /* Dots from the second $2006 write to v changing (ares). */
    TRANSFER_DELAY = 3,
    /* A $2007 read on the CPU cycle after another one is ignored (ares). */
    READ_BLOCK_DOTS = 6,
    /* The I/O latch holds a bit for roughly 600 ms. */
    LATCH_DECAY_FRAMES = 36
};

/* ---------------------------------------------------------------- helpers */

static inline unsigned pre_render_line(const NesPpu *p) { return (unsigned)p->lines - 1u; }

static inline bool render_enabled(const NesPpu *p) { return (p->mask & MASK_RENDER) != 0; }

static inline bool on_render_line(const NesPpu *p)
{
    return p->scanline < VISIBLE_LINES || p->scanline == pre_render_line(p);
}

/* Rendering: the PPU is driving its address bus and running the scroll
 * and sprite logic. */
static inline bool rendering(const NesPpu *p) { return render_enabled(p) && on_render_line(p); }

static inline uint8_t bus_read(NesPpu *p, unsigned addr)
{
    p->bus_addr = (uint16_t)addr;
    return nes_ppu_bus_read(p, (uint16_t)addr);
}

static inline void bus_set(NesPpu *p, unsigned addr)
{
    if (p->bus_addr != addr) {
        p->bus_addr = (uint16_t)addr;
        nes_ppu_bus_address(p, (uint16_t)addr);
    }
}

/* Palette RAM: 32 entries, of which $10/$14/$18/$1C are $00/$04/$08/$0C. */
static inline uint8_t palette_read(const NesPpu *p, unsigned index)
{
    index &= 0x1Fu;
    if (!(index & 3u)) index &= 0x0Fu;
    uint8_t c = p->palette[index];
    if (p->mask & MASK_GREYSCALE) c &= 0x30u;
    return c;
}

static inline void palette_write(NesPpu *p, unsigned index, uint8_t value)
{
    index &= 0x1Fu;
    value &= 0x3Fu;
    p->palette[index] = value;
    if (!(index & 3u)) p->palette[index ^ 0x10u] = value;
}

static inline void latch_refresh(NesPpu *p, uint8_t value, uint8_t bits)
{
    p->io_latch = (uint8_t)((p->io_latch & ~bits) | (value & bits));
    for (unsigned i = 0; i < 8; i++)
        if (bits & (1u << i)) p->io_decay[i] = LATCH_DECAY_FRAMES;
}

static void latch_decay(NesPpu *p)
{
    for (unsigned i = 0; i < 8; i++)
        if (p->io_decay[i] && --p->io_decay[i] == 0) p->io_latch &= (uint8_t)~(1u << i);
}

/* ----------------------------------------------------------------- scroll */

static inline void increment_x(NesPpu *p)
{
    if ((p->v & V_COARSE_X) == 31u)
        p->v = (uint16_t)((p->v & ~V_COARSE_X) ^ V_NT_X);
    else
        p->v++;
}

static inline void increment_y(NesPpu *p)
{
    if ((p->v & V_FINE_Y) != V_FINE_Y) {
        p->v = (uint16_t)(p->v + 0x1000u);
        return;
    }
    p->v &= (uint16_t)~V_FINE_Y;
    unsigned y = (p->v & V_COARSE_Y) >> 5;
    if (y == 29u) {
        y = 0;
        p->v ^= V_NT_Y;
    } else {
        y = (y + 1u) & 31u; /* rows 30 and 31 wrap without switching nametable */
    }
    p->v = (uint16_t)((p->v & ~V_COARSE_Y) | (y << 5));
}

static inline void copy_horizontal(NesPpu *p)
{
    p->v = (uint16_t)((p->v & ~V_HORIZONTAL) | (p->t & V_HORIZONTAL));
}

static inline void copy_vertical(NesPpu *p)
{
    p->v = (uint16_t)((p->v & ~V_VERTICAL) | (p->t & V_VERTICAL));
}

/* The scroll counters, on render lines with rendering enabled. */
static inline void scroll_dot(NesPpu *p, unsigned d, bool pre_render)
{
    if (d == 0) return;
    if (d <= 256) {
        if (d & 7u) return;
        increment_x(p);
        if (d == 256) increment_y(p);
    } else if (d == 257) {
        copy_horizontal(p);
    } else if (d > 320 && d <= 336) {
        if (!(d & 7u)) increment_x(p);
    } else if (pre_render && d >= 280 && d <= 304) {
        copy_vertical(p);
    }
}

/* The second $2006 write reaches v a few dots later. If that dot is also
 * a scroll increment, the incremented fields get written AND current. */
static void transfer_dot(NesPpu *p, unsigned d)
{
    if (--p->transfer_delay) return;

    const bool active = rendering(p);
    if (active && d != 0 && !(d & 7u) && (d <= 256 || d > 320))
        p->t &= (uint16_t)(p->v | ~V_HORIZONTAL);
    if (active && d == 256) p->t &= (uint16_t)(p->v | ~V_VERTICAL);

    p->v = p->t;
    if (!active) bus_set(p, p->v & 0x3FFFu);
}

/* ------------------------------------------------------------- background */

static inline unsigned bg_pattern_address(const NesPpu *p)
{
    return ((unsigned)(p->ctrl & CTRL_BG_TABLE) << 8) | ((unsigned)p->nt_latch << 4) | ((p->v >> 12) & 7u);
}

/* One of the four fetches of a tile; phase is 0, 2, 4 or 6 on the first
 * dot of each (odd phases are the second dot of an access). */
static inline void bg_fetch(NesPpu *p, unsigned phase)
{
    switch (phase) {
    case 0:
        p->nt_latch = bus_read(p, 0x2000u | (p->v & 0x0FFFu));
        break;
    case 2: {
        unsigned at = bus_read(p, 0x23C0u | (p->v & 0x0C00u) | ((p->v >> 4) & 0x38u) | ((p->v >> 2) & 0x07u));
        if (p->v & 0x0040u) at >>= 4;
        if (p->v & 0x0002u) at >>= 2;
        p->at_latch = (uint8_t)(at & 3u);
        break;
    }
    case 4:
        p->pt_lo_latch = bus_read(p, bg_pattern_address(p));
        break;
    case 6:
        p->pt_hi_latch = bus_read(p, bg_pattern_address(p) | 8u);
        break;
    default:
        break;
    }
}

static inline void bg_shift(NesPpu *p)
{
    p->bg_lo = (uint16_t)(p->bg_lo << 1);
    p->bg_hi = (uint16_t)((p->bg_hi << 1) | 1u); /* the high plane shifts in ones */
    p->at_lo = (uint16_t)(p->at_lo << 1);
    p->at_hi = (uint16_t)(p->at_hi << 1);
}

static inline void bg_reload(NesPpu *p)
{
    p->bg_lo = (uint16_t)((p->bg_lo & 0xFF00u) | p->pt_lo_latch);
    p->bg_hi = (uint16_t)((p->bg_hi & 0xFF00u) | p->pt_hi_latch);
    p->at_lo = (uint16_t)((p->at_lo & 0xFF00u) | ((p->at_latch & 1u) ? 0xFFu : 0x00u));
    p->at_hi = (uint16_t)((p->at_hi & 0xFF00u) | ((p->at_latch & 2u) ? 0xFFu : 0x00u));
}

/* ---------------------------------------------------------------- sprites */

static inline uint8_t reverse8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

/* Sprite evaluation and the OAM data latch, dots 1-340 of a visible line
 * with rendering enabled; the pre-render line runs only dots 257-340 of
 * it. oam_addr is the evaluation's main counter, as on
 * hardware: bits 7-2 select the sprite (n) and bits 1-0 the byte (m). */
static void sprite_evaluation_dot(NesPpu *p, unsigned line, unsigned d)
{
    if (d <= 64) {
        /* Secondary OAM is cleared: reads give $FF, writes store them. */
        if (d & 1u)
            p->oam_data = 0xFF;
        else
            p->soam[(d - 1u) >> 1] = p->oam_data;
        if (d == 64) p->eval_temp = 0;
        return;
    }

    if (d <= 256) {
        if (d & 1u) {
            p->oam_data = p->oam[p->oam_addr];
            return;
        }

        if (p->eval_main_overflow) {
            /* Every sprite was looked at: the copy keeps failing. */
            p->oam_addr = (uint8_t)(p->oam_addr + 4u);
            if (p->eval_temp_overflow) p->oam_data = p->soam[p->eval_temp];
            return;
        }

        const unsigned height = (p->ctrl & CTRL_SPRITE_16) ? 16u : 8u;
        const bool in_range = ((line - p->oam_data) & 0x1FFu) < height;

        if (p->eval_temp_overflow)
            p->oam_data = p->soam[p->eval_temp]; /* writes are disabled: it reads */
        else
            p->soam[p->eval_temp] = p->oam_data;

        if (!(p->eval_temp & 3u)) {
            /* This byte is taken as a Y coordinate. */
            if (p->eval_temp_overflow) {
                if (!in_range) {
                    /* The bug: n and m both step, m without carry. */
                    const unsigned n = (p->oam_addr + 4u) & 0xFCu;
                    const unsigned m = (p->oam_addr + 1u) & 0x03u;
                    p->oam_addr = (uint8_t)(n | m);
                    if (!n) p->eval_main_overflow = true;
                    return;
                }
                p->status |= NES_PPU_STATUS_OVERFLOW;
            } else {
                if (!in_range) {
                    p->oam_addr = (uint8_t)((p->oam_addr + 4u) & 0xFCu);
                    if (!p->oam_addr) p->eval_main_overflow = true;
                    return;
                }
                /* The first sprite evaluated is sprite zero, whatever
                 * OAMADDR pointed at. */
                p->eval_id[p->eval_temp >> 2] = (d == 66) ? 0 : 1;
            }
            p->eval_temp = (uint8_t)((p->eval_temp + 1u) & 31u);
            if (++p->oam_addr == 0) p->eval_main_overflow = true;
        } else {
            /* Bytes 1-3 of a sprite being copied. */
            p->eval_temp = (uint8_t)((p->eval_temp + 1u) & 31u);
            if (++p->oam_addr == 0) p->eval_main_overflow = true;
            if (p->eval_temp == 0) p->eval_temp_overflow = true;
            if (!(p->eval_temp & 3u) && !in_range) p->oam_addr &= 0xFCu;
        }
        return;
    }

    if (d <= 320) {
        /* Sprite fetches: OAMADDR is held at 0; the latch shows the four
         * bytes of each slot, then its X four more times. */
        const unsigned i = d - 257u;
        const unsigned byte = (i & 7u) < 3u ? (i & 7u) : 3u;
        p->oam_addr = 0;
        p->eval_temp = 0;
        p->eval_main_overflow = false;
        p->eval_temp_overflow = false;
        p->oam_data = p->soam[((i >> 3) << 2) + byte];
        return;
    }

    p->oam_data = p->soam[0];
}

/* Dot 257: secondary OAM is handed to the output units. Slots that
 * evaluation did not fill keep id 64 and stay transparent, although their
 * (dummy, tile $FF) patterns are still fetched. */
static void sprite_latch_units(NesPpu *p)
{
    uint8_t active = 0;
    for (unsigned n = 0; n < 8; n++) {
        NesPpuSprite *u = &p->unit[n];
        u->id = p->eval_id[n];
        u->y = p->soam[4u * n + 0u];
        u->tile = p->soam[4u * n + 1u];
        u->attr = p->soam[4u * n + 2u];
        u->x = p->soam[4u * n + 3u];
        if (u->id != EMPTY_SLOT) active = 1;
    }
    p->units_active = active;
}

static inline unsigned sprite_pattern_address(const NesPpu *p, const NesPpuSprite *u, unsigned line)
{
    unsigned addr, row;
    if (p->ctrl & CTRL_SPRITE_16) {
        row = (line - u->y) & 15u;
        if (u->attr & 0x80u) row ^= 15u;
        addr = ((unsigned)(u->tile & 0x01u) << 12) | ((unsigned)(u->tile & 0xFEu) << 4);
        addr += row + (row & 8u); /* rows 8-15 are the next tile */
    } else {
        row = (line - u->y) & 7u;
        if (u->attr & 0x80u) row ^= 7u;
        addr = ((unsigned)(p->ctrl & CTRL_SPRITE_TABLE) << 9) | ((unsigned)u->tile << 4);
        addr += row;
    }
    return addr;
}

/* Dots 257-320 with rendering enabled: eight dots per slot. */
static inline void sprite_fetch(NesPpu *p, unsigned line, unsigned d)
{
    const unsigned i = d - 257u;
    NesPpuSprite *u = &p->unit[i >> 3];
    switch (i & 7u) {
    case 0:
        if (d == 257)
            /* The first garbage fetch sees the high address bits already
             * reloaded from t, the low eight not yet. */
            bus_read(p, 0x2000u | (p->v & 0x0BFFu) | (p->t & V_NT_X));
        else
            bus_read(p, 0x2000u | (p->v & 0x0FFFu));
        break;
    case 2:
        bus_read(p, 0x2000u | (p->v & 0x0FFFu));
        break;
    case 4: {
        uint8_t b = bus_read(p, sprite_pattern_address(p, u, line));
        u->lo = (u->attr & 0x40u) ? reverse8(b) : b;
        break;
    }
    case 6: {
        uint8_t b = bus_read(p, sprite_pattern_address(p, u, line) | 8u);
        u->hi = (u->attr & 0x40u) ? reverse8(b) : b;
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ pixel */

/* Produce picture pixel x of visible line `line`. */
static inline void pixel(NesPpu *p, unsigned line, unsigned x, bool enabled)
{
    unsigned bg = 0;
    if (enabled) {
        const unsigned bit = 15u - p->fine_x;
        bg = ((p->bg_lo >> bit) & 1u) | (((p->bg_hi >> bit) & 1u) << 1) | (((p->at_lo >> bit) & 1u) << 2) |
             (((p->at_hi >> bit) & 1u) << 3);
        bg_shift(p);
        if (!(bg & 3u) || !(p->mask & MASK_BG) || (x < 8u && !(p->mask & MASK_BG_LEFT))) bg = 0;
    }

    unsigned sprite = 0;
    bool behind = false;
    if (p->units_active) {
        const bool show = (p->mask & MASK_SPRITES) && (x >= 8u || (p->mask & MASK_SPRITE_LEFT));
        /* Highest slot first, so that the lowest one wins. */
        for (int n = 7; n >= 0; n--) {
            NesPpuSprite *u = &p->unit[n];
            if (u->id == EMPTY_SLOT) continue;
            if (!u->x) u->counting = 0;
            if (u->counting) {
                u->x--;
                continue;
            }
            if (!enabled) continue;

            const unsigned c = ((u->lo >> 7) & 1u) | (((u->hi >> 7) & 1u) << 1);
            u->lo = (uint8_t)(u->lo << 1);
            u->hi = (uint8_t)(u->hi << 1);
            if (!show || !c) continue;

            if (u->id == 0 && bg && x != 255u) p->status |= NES_PPU_STATUS_SPRITE0;
            sprite = 0x10u | ((u->attr & 3u) << 2) | c;
            behind = (u->attr & 0x20u) != 0;
        }
    }

    unsigned index = bg;
    if (sprite && (!bg || !behind)) index = sprite;

    /* With rendering off the backdrop is shown, unless v points into the
     * palette: then that entry is. */
    if (!enabled && (p->v & 0x3F00u) == 0x3F00u) index = p->v & 0x1Fu;

    p->frame[line * NES_PPU_WIDTH + x] = (uint16_t)(palette_read(p, index) | ((unsigned)(p->mask >> 5) << 6));
}

/* ------------------------------------------------------------------- dots */

/* The fetches, shifters and pixel of dot d on a render line. */
static inline void render_dot(NesPpu *p, unsigned line, unsigned d, bool pre_render, bool enabled)
{
    if (d == 0) {
        /* Nothing is in range for the next line until evaluation says so. */
        memset(p->eval_id, EMPTY_SLOT, sizeof p->eval_id);
        /* The idle dot shows the pattern address of the tile whose
         * nametable byte was read at dots 337 and 339. When an odd frame
         * skipped a dot, this dot is the end of that nametable fetch
         * instead. (advance() decided the skip with the same PPUMASK.)
         * Not on the pre-render line: its dot 0 follows a VBlank line,
         * which made no fetches, and the pins still show v. That is where
         * blargg's mmc3_test_2/4-scanline_timing puts the first A12 rise
         * of a frame with the background at $1000: at the first pattern
         * fetch, dot 5, not at dot 0. */
        if (enabled && !pre_render && !(line == 0 && p->odd_skip && !p->odd_frame)) bus_set(p, bg_pattern_address(p));
        return;
    }

    /* Memory accesses. */
    if (enabled) {
        if (d <= 256 || (d >= 321 && d <= 336)) {
            if (d & 1u) bg_fetch(p, (d - 1u) & 7u);
        } else if (d == 337 || d == 339) {
            p->nt_latch = bus_read(p, 0x2000u | (p->v & 0x0FFFu));
        }
    }

    /* Pixel x leaves the shifters at dot x + 2. The shifters reload after
     * every eighth shift: dots 9, 17, .., 257, and 329 and 337. */
    if (d >= 2 && d <= 257) {
        if (!pre_render)
            pixel(p, line, d - 2u, enabled);
        else if (enabled)
            bg_shift(p);
        if (enabled && (d & 7u) == 1u) bg_reload(p);
    } else if (enabled && d >= 322 && d <= 337) {
        bg_shift(p);
        if ((d & 7u) == 1u) bg_reload(p);
    }

    if (d >= 257 && d <= 320) {
        if (d == 257) sprite_latch_units(p);
        if (enabled && (d & 1u)) sprite_fetch(p, line, d);
    } else if (d == 339 && enabled) {
        for (unsigned n = 0; n < 8; n++) p->unit[n].counting = 1;
    }
}

/* The work of the dot the PPU has just entered. */
static void dot_work(NesPpu *p)
{
    const unsigned line = p->scanline;
    const unsigned d = p->dot;
    const unsigned pre = pre_render_line(p);

    if (p->read_block) p->read_block--;
    if (p->warmup_dots) p->warmup_dots--;

    if (line < VISIBLE_LINES || line == pre) {
        const bool pre_render = line == pre;
        const bool enabled = render_enabled(p);

        render_dot(p, line, d, pre_render, enabled);
        if (p->transfer_delay) transfer_dot(p, d);

        if (enabled && d) {
            if (!pre_render) {
                sprite_evaluation_dot(p, line, d);
            } else if (d >= 257) {
                sprite_evaluation_dot(p, line, d);
            } else if (d <= 8 && p->oam_addr >= 8) {
                /* Rendering starts with OAMADDR >= 8: the eight bytes at
                 * OAMADDR & $F8 replace the first eight of OAM. */
                p->oam[d - 1u] = p->oam[(p->oam_addr & 0xF8u) + d - 1u];
            }
            scroll_dot(p, d, pre_render);
        }

        if (pre_render && d == 1)
            p->status = 0; /* VBlank, sprite 0 hit and overflow */
        return;
    }

    if (p->transfer_delay) transfer_dot(p, d);

    if (line == POST_RENDER_LINE) {
        if (d == 0) {
            p->frame_ready = true;
            p->frame_count++;
            latch_decay(p);
        } else if (d == 1) {
            /* Rendering is over: the address pins show v again. */
            bus_set(p, p->v & 0x3FFFu);
        }
    }
    if (line == p->vblank_line && d == 1) {
        if (p->vbl_suppress)
            p->vbl_suppress = false;
        else
            p->status |= NES_PPU_STATUS_VBLANK;
    }
}

static inline void advance(NesPpu *p)
{
    unsigned d = p->dot + 1u;
    if (d == DOTS - 1u && p->odd_frame && p->odd_skip && p->scanline == pre_render_line(p) && render_enabled(p))
        d = DOTS; /* (339, pre-render) is followed by (0, 0) */
    if (d >= DOTS) {
        d = 0;
        if (++p->scanline >= p->lines) {
            p->scanline = 0;
            p->odd_frame = !p->odd_frame;
        }
    }
    p->dot = (uint16_t)d;
}

/* On the lines between the picture and the pre-render line only three
 * dots do anything. Returns how many dots ahead need no work at all. */
static inline uint32_t idle_dots(const NesPpu *p)
{
    const uint32_t here = (uint32_t)p->scanline * DOTS + p->dot;
    const uint32_t post = (uint32_t)POST_RENDER_LINE * DOTS;
    const uint32_t vbl = (uint32_t)p->vblank_line * DOTS + 1u;
    uint32_t next = (uint32_t)pre_render_line(p) * DOTS;
    if (here < vbl) next = vbl;
    if (here < post + 1u) next = post + 1u;
    return next - here - 1u;
}

void nes_ppu_run(NesPpu *p, uint32_t dots)
{
    while (dots) {
        if (p->scanline >= POST_RENDER_LINE && p->scanline < pre_render_line(p) && !p->transfer_delay) {
            uint32_t skip = idle_dots(p);
            if (skip) {
                if (skip > dots) skip = dots;
                const uint32_t here = (uint32_t)p->scanline * DOTS + p->dot + skip;
                p->scanline = (uint16_t)(here / DOTS);
                p->dot = (uint16_t)(here % DOTS);
                p->read_block = (uint8_t)(p->read_block > skip ? p->read_block - skip : 0u);
                p->warmup_dots = p->warmup_dots > skip ? p->warmup_dots - skip : 0u;
                dots -= skip;
                continue;
            }
        }
        advance(p);
        dot_work(p);
        dots--;
    }
}

uint32_t nes_ppu_dots_until_event(const NesPpu *p)
{
    const uint32_t here = (uint32_t)p->scanline * DOTS + p->dot;
    const uint32_t ready = (uint32_t)POST_RENDER_LINE * DOTS;
    const uint32_t vbl = (uint32_t)p->vblank_line * DOTS + 1u;
    const uint32_t first = ready < vbl ? ready : vbl;
    const uint32_t second = ready < vbl ? vbl : ready;

    if (here < first) return first - here;
    if (here < second) return second - here;

    /* The next one is in the next frame. If this frame is odd and its
     * skipped dot is still ahead, assume it will be skipped. */
    uint32_t n = (uint32_t)p->lines * DOTS - here + first;
    if (p->odd_skip && p->odd_frame && here <= (uint32_t)pre_render_line(p) * DOTS + (DOTS - 2u)) n--;
    return n;
}

bool nes_ppu_nmi_line(const NesPpu *p)
{
    return (p->ctrl & CTRL_NMI) && (p->status & NES_PPU_STATUS_VBLANK);
}

/* -------------------------------------------------------------- registers */

/* v after a $2007 access: outside rendering it steps by 1 or 32; during
 * rendering it gets a coarse X and a Y increment at once. */
static void data_port_increment(NesPpu *p)
{
    if (rendering(p)) {
        increment_x(p);
        increment_y(p);
    } else {
        p->v = (uint16_t)((p->v + ((p->ctrl & CTRL_INCREMENT_32) ? 32u : 1u)) & 0x7FFFu);
        bus_set(p, p->v & 0x3FFFu);
    }
}

uint8_t nes_ppu_read(NesPpu *p, unsigned reg)
{
    uint8_t value = p->io_latch;

    switch (reg & 7u) {
    case 2: /* PPUSTATUS */
        value = (uint8_t)((value & 0x1Fu) | (p->status & 0xE0u));
        /* Read on the dot before the flag is set: it reads clear and is
         * not set this frame. On the two dots after, it reads set and the
         * clear below takes the NMI line down again before the CPU has
         * sampled it (the machine samples once per CPU cycle). */
        if (p->scanline == p->vblank_line && p->dot == 0) p->vbl_suppress = true;
        p->status = (uint8_t)(p->status & ~NES_PPU_STATUS_VBLANK);
        p->w = 0;
        latch_refresh(p, value, 0xE0);
        break;

    case 4: /* OAMDATA */
        value = rendering(p) ? p->oam_data : p->oam[p->oam_addr];
        latch_refresh(p, value, 0xFF);
        break;

    case 7: { /* PPUDATA */
        if (p->read_block) break;
        const unsigned addr = rendering(p) ? p->bus_addr : (p->v & 0x3FFFu);
        value = p->read_buffer;
        p->read_buffer = bus_read(p, addr);
        if (addr >= 0x3F00u) {
            /* The palette answers at once, on six bits. The buffer gets
             * the nametable byte the cartridge maps underneath. */
            value = (uint8_t)(palette_read(p, addr) | (p->io_latch & 0xC0u));
            latch_refresh(p, value, 0x3F);
        } else {
            latch_refresh(p, value, 0xFF);
        }
        data_port_increment(p);
        p->read_block = READ_BLOCK_DOTS;
        break;
    }

    default: /* write-only: the latch */
        break;
    }
    return value;
}

void nes_ppu_write(NesPpu *p, unsigned reg, uint8_t value)
{
    latch_refresh(p, value, 0xFF);

    switch (reg & 7u) {
    case 0: /* PPUCTRL */
        if (p->warmup_dots) break;
        p->ctrl = value;
        p->t = (uint16_t)((p->t & ~(V_NT_X | V_NT_Y)) | ((unsigned)(value & 3u) << 10));
        break;

    case 1: { /* PPUMASK */
        if (p->warmup_dots) break;
        const bool was = rendering(p);
        p->mask = value;
        if (was && !rendering(p)) bus_set(p, p->v & 0x3FFFu);
        break;
    }

    case 3: /* OAMADDR */
        p->oam_addr = value;
        break;

    case 4: /* OAMDATA */
        if (rendering(p)) {
            /* No write; only the sprite number steps. */
            p->oam_addr = (uint8_t)((p->oam_addr + 4u) & 0xFCu);
            break;
        }
        if ((p->oam_addr & 3u) == 2u) value &= 0xE3u; /* attribute bits 2-4 do not exist */
        p->oam[p->oam_addr++] = value;
        break;

    case 5: /* PPUSCROLL */
        if (p->warmup_dots) break;
        if (!p->w) {
            p->t = (uint16_t)((p->t & ~V_COARSE_X) | (value >> 3));
            p->fine_x = value & 7u;
        } else {
            p->t = (uint16_t)((p->t & ~(V_FINE_Y | V_COARSE_Y)) | ((unsigned)(value & 0x07u) << 12) |
                              ((unsigned)(value & 0xF8u) << 2));
        }
        p->w ^= 1u;
        break;

    case 6: /* PPUADDR */
        if (p->warmup_dots) break;
        if (!p->w) {
            p->t = (uint16_t)((p->t & 0x00FFu) | ((unsigned)(value & 0x3Fu) << 8)); /* bit 14 cleared */
        } else {
            p->t = (uint16_t)((p->t & 0x7F00u) | value);
            p->transfer_delay = TRANSFER_DELAY;
        }
        p->w ^= 1u;
        break;

    case 7: { /* PPUDATA */
        const unsigned addr = rendering(p) ? p->bus_addr : (p->v & 0x3FFFu);
        if (addr >= 0x3F00u) {
            palette_write(p, addr, value);
        } else {
            p->bus_addr = (uint16_t)addr;
            nes_ppu_bus_write(p, (uint16_t)addr, value);
        }
        data_port_increment(p);
        break;
    }

    default: /* PPUSTATUS is read-only */
        break;
    }
}

/* ------------------------------------------------------------------ power */

/* Palette RAM contents observed at power-on (ares, after blargg's
 * power_up_palette test). */
static const uint8_t palette_power_on[32] = {
    0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2C,
    0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08,
};

void nes_ppu_power(NesPpu *p)
{
    void *user = p->user;
    memset(p, 0, sizeof *p);
    p->user = user;

    /* The timing model; the only place the NTSC numbers are used. */
    p->lines = NES_PPU_NTSC_LINES;
    p->vblank_line = NES_PPU_NTSC_VBLANK_LINE;
    p->odd_skip = true;

    memcpy(p->palette, palette_power_on, sizeof p->palette);
    memset(p->eval_id, EMPTY_SLOT, sizeof p->eval_id);
    for (unsigned n = 0; n < 8; n++) p->unit[n].id = EMPTY_SLOT;
}

void nes_ppu_reset(NesPpu *p)
{
    p->ctrl = 0;
    p->mask = 0;
    p->t = 0;
    p->fine_x = 0;
    p->w = 0;
    p->read_buffer = 0;
    p->odd_frame = false;
    p->scanline = 0;
    p->dot = 0;
    p->transfer_delay = 0;
    p->read_block = 0;
    p->vbl_suppress = false;
    p->warmup_dots = 0;
    /* Rendering is off, so the pins show v. Not reported to the cartridge:
     * a reset must not call out. */
    p->bus_addr = p->v & 0x3FFFu;
}
