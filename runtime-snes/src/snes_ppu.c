/*
 * SNES PPU1 + PPU2: power/reset, the register file $2100-$213F, VRAM /
 * OAM / CGRAM ports, the counter latch and the frame edges. The scanline
 * renderer is in snes_ppu_render.c.
 *
 * Register semantics are ported from ares (ISC license, Near et al.),
 * https://github.com/ares-emulator/ares at
 * af4cbb04f067682a8a3cf42695ff78bed634b38d: ares/sfc/ppu-performance/io.cpp
 * and oam.cpp (register writes and reads, write-twice latches, VRAM address
 * remapping and prefetch, OAM and CGRAM ports, $2134-$213F), with the
 * open-bus, OAM reload and power/reset details of ares/sfc/ppu/io.cpp and
 * ppu.cpp. Hardware reference: fullsnes.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 *
 * Differences from ares: registers are kept as their raw written values
 * and decoded on use; OAM is kept as the raw 544-byte table; there is no
 * random power-on state (everything powers on as zero, forced blank).
 */
#include "snes_ppu_internal.h"

#include <string.h>

enum { PPU1_VERSION = 1, PPU2_VERSION = 3 };

/* ---------------------------------------------------------------- power */

static void reset_ports(SnesPpu *p)
{
    p->regs[0x00] = 0x80; /* forced blank, brightness 0 */
    p->regs[0x05] = 0x00; /* BGMODE */
    p->regs[0x33] &= 0x08; /* SETINI: pseudo-hires survives a reset */
    p->vram_addr = 0;
    p->vram_prefetch = 0;
    p->oam_addr = 0;
    p->oam_base = 0;
    p->oam_latch = 0;
    p->first_sprite = 0;
    p->cgram_addr = 0;
    p->cgram_latch = 0;
    p->cgram_high = false;
    p->bgofs_latch1 = 0;
    p->bgofs_latch2 = 0;
    p->m7_latch = 0;
    p->m7_hofs = p->m7_vofs = 0;
    p->m7a = p->m7b = p->m7c = p->m7d = p->m7x = p->m7y = 0;
    p->latched_h = p->latched_v = 0;
    p->counters_latched = false;
    p->h_flip = p->v_flip = false;
    p->wrio = 0xFF;
    p->ppu1_mdr = 0;
    p->ppu2_mdr = 0;
    p->range_over = p->time_over = false;
    p->mosaic_counter = 0;
}

void snes_ppu_power(SnesPpu *p)
{
    memset(p, 0, sizeof *p);
    reset_ports(p);
    p->out_width = 256;
    p->out_height = 224;
}

void snes_ppu_reset(SnesPpu *p)
{
    /* VRAM, CGRAM, OAM and the layer registers survive a reset. */
    reset_ports(p);
}

/* ----------------------------------------------------------------- VRAM */

static uint16_t vram_step(const SnesPpu *p)
{
    static const uint16_t steps[4] = { 1, 32, 128, 128 };
    return steps[p->regs[0x15] & 3u];
}

/* VMAIN bits 2-3: address translation for 2/4/8bpp bitmap uploads. */
static uint16_t vram_address(const SnesPpu *p)
{
    uint16_t a = p->vram_addr;
    switch ((p->regs[0x15] >> 2) & 3u) {
    case 1: return (uint16_t)((a & 0xFF00u) | ((a & 0x001Fu) << 3) | ((a >> 5) & 7u));
    case 2: return (uint16_t)((a & 0xFE00u) | ((a & 0x003Fu) << 3) | ((a >> 6) & 7u));
    case 3: return (uint16_t)((a & 0xFC00u) | ((a & 0x007Fu) << 3) | ((a >> 7) & 7u));
    default: return a;
    }
}

/* VRAM is only accessible in vblank or forced blank. */
static bool vram_blocked(const SnesPpu *p)
{
    return !ppu_forced_blank(p) && !p->vblank;
}

static uint16_t vram_read(const SnesPpu *p)
{
    if (vram_blocked(p)) return 0x0000;
    return ppu_vram_word(p, vram_address(p));
}

static void vram_write(SnesPpu *p, unsigned byte, uint8_t value)
{
    if (vram_blocked(p)) return;
    uint32_t a = ((uint32_t)vram_address(p) & 0x7FFFu) << 1;
    p->vram[a + byte] = value;
}

/* ------------------------------------------------------------------ OAM */

static void set_first_sprite(SnesPpu *p)
{
    p->first_sprite = (p->regs[0x03] & 0x80u) ? (uint8_t)((p->oam_addr >> 2) & 0x7Fu) : 0;
}

static void oam_address_reset(SnesPpu *p)
{
    p->oam_addr = p->oam_base;
    set_first_sprite(p);
}

/* During active display the PPU owns the OAM address bus: accesses land
 * on an address the sprite logic is using (ares-performance: $218). */
static uint16_t oam_access_address(const SnesPpu *p, uint16_t addr)
{
    if (!ppu_forced_blank(p) && !p->vblank) return 0x218;
    return addr;
}

static uint8_t oam_read(const SnesPpu *p, uint16_t addr)
{
    addr = oam_access_address(p, (uint16_t)(addr & 0x3FFu));
    if (addr & 0x200u) return p->oam[0x200u | (addr & 0x1Fu)];
    return p->oam[addr];
}

static void oam_write(SnesPpu *p, uint16_t addr, uint8_t value)
{
    addr = oam_access_address(p, (uint16_t)(addr & 0x3FFu));
    if (addr & 0x200u) p->oam[0x200u | (addr & 0x1Fu)] = value;
    else p->oam[addr] = value;
}

/* ---------------------------------------------------------------- CGRAM */

/* During the active part of a picture line (H = 22-273 dots) the PPU
 * drives the CGRAM address with the palette index it is outputting. */
static uint8_t cgram_access_address(const SnesPpu *p, uint8_t addr)
{
    if (!ppu_forced_blank(p) && p->vcounter > 0 && !p->vblank && p->hcounter >= 22 && p->hcounter < 274)
        return p->cgram_pixel[(p->hcounter - 22u) & 0xFFu];
    return addr;
}

/* ------------------------------------------------------------- counters */

void snes_ppu_latch_counters(SnesPpu *p)
{
    p->latched_h = p->hcounter;
    p->latched_v = p->vcounter;
    p->counters_latched = true;
}

void snes_ppu_set_wrio(SnesPpu *p, uint8_t wrio) { p->wrio = wrio; }

/* ---------------------------------------------------------------- write */

static void write_hofs(SnesPpu *p, unsigned bg, uint8_t value)
{
    p->bg_hofs[bg] = (uint16_t)((value << 8) | (p->bgofs_latch1 & ~7u) | (p->bgofs_latch2 & 7u));
    p->bgofs_latch1 = value;
    p->bgofs_latch2 = value;
}

static void write_vofs(SnesPpu *p, unsigned bg, uint8_t value)
{
    p->bg_vofs[bg] = (uint16_t)((value << 8) | p->bgofs_latch1);
    p->bgofs_latch1 = value;
}

static uint16_t write_m7(SnesPpu *p, uint8_t value)
{
    uint16_t v = (uint16_t)((value << 8) | p->m7_latch);
    p->m7_latch = value;
    return v;
}

void snes_ppu_write(SnesPpu *p, uint8_t reg, uint8_t value)
{
    if (reg > 0x33) return;
    uint8_t old = p->regs[reg];
    p->regs[reg] = value;

    switch (reg) {
    case 0x00: /* INIDISP */
        if ((old & 0x80u) && p->vcounter == ppu_vdisp(p)) oam_address_reset(p);
        break;
    case 0x02: /* OAMADDL */
        p->oam_base = (uint16_t)((p->oam_base & 0x200u) | (value << 1));
        oam_address_reset(p);
        break;
    case 0x03: /* OAMADDH */
        p->oam_base = (uint16_t)((p->oam_base & 0x1FEu) | ((value & 1u) << 9));
        oam_address_reset(p);
        break;
    case 0x04: { /* OAMDATA */
        unsigned latch_bit = p->oam_addr & 1u;
        uint16_t addr = p->oam_addr;
        p->oam_addr = (uint16_t)((p->oam_addr + 1u) & 0x3FFu);
        if (latch_bit == 0) p->oam_latch = value;
        if (addr & 0x200u) {
            oam_write(p, addr, value);
        } else if (latch_bit == 1) {
            oam_write(p, (uint16_t)((addr & ~1u) + 0u), p->oam_latch);
            oam_write(p, (uint16_t)((addr & ~1u) + 1u), value);
        }
        set_first_sprite(p);
        break;
    }
    case 0x06: /* MOSAIC: the vertical counter reloads when mosaic turns on */
        if (!(old & 0x0Fu) && (value & 0x0Fu)) p->mosaic_counter = (uint8_t)(ppu_mosaic_size(p) + 1u);
        break;
    case 0x0D: /* BG1HOFS, also the mode 7 horizontal offset */
        p->m7_hofs = write_m7(p, value);
        write_hofs(p, 0, value);
        break;
    case 0x0E: /* BG1VOFS, also the mode 7 vertical offset */
        p->m7_vofs = write_m7(p, value);
        write_vofs(p, 0, value);
        break;
    case 0x0F: write_hofs(p, 1, value); break;
    case 0x10: write_vofs(p, 1, value); break;
    case 0x11: write_hofs(p, 2, value); break;
    case 0x12: write_vofs(p, 2, value); break;
    case 0x13: write_hofs(p, 3, value); break;
    case 0x14: write_vofs(p, 3, value); break;
    case 0x16: /* VMADDL */
        p->vram_addr = (uint16_t)((p->vram_addr & 0xFF00u) | value);
        p->vram_prefetch = vram_read(p);
        break;
    case 0x17: /* VMADDH */
        p->vram_addr = (uint16_t)((p->vram_addr & 0x00FFu) | (value << 8));
        p->vram_prefetch = vram_read(p);
        break;
    case 0x18: /* VMDATAL */
        vram_write(p, 0, value);
        if (!(p->regs[0x15] & 0x80u)) p->vram_addr = (uint16_t)(p->vram_addr + vram_step(p));
        break;
    case 0x19: /* VMDATAH */
        vram_write(p, 1, value);
        if (p->regs[0x15] & 0x80u) p->vram_addr = (uint16_t)(p->vram_addr + vram_step(p));
        break;
    case 0x1B: p->m7a = write_m7(p, value); break;
    case 0x1C: p->m7b = write_m7(p, value); break;
    case 0x1D: p->m7c = write_m7(p, value); break;
    case 0x1E: p->m7d = write_m7(p, value); break;
    case 0x1F: p->m7x = write_m7(p, value); break;
    case 0x20: p->m7y = write_m7(p, value); break;
    case 0x21: /* CGADD */
        p->cgram_addr = value;
        p->cgram_high = false;
        break;
    case 0x22: /* CGDATA */
        if (!p->cgram_high) {
            p->cgram_latch = value;
            p->cgram_high = true;
        } else {
            uint8_t a = cgram_access_address(p, p->cgram_addr);
            p->cgram[(unsigned)a * 2u] = p->cgram_latch;
            p->cgram[(unsigned)a * 2u + 1u] = (uint8_t)(value & 0x7Fu);
            p->cgram_addr++;
            p->cgram_high = false;
        }
        break;
    case 0x32: /* COLDATA */
        if (value & 0x20u) p->fixed_r = (uint8_t)(value & 0x1Fu);
        if (value & 0x40u) p->fixed_g = (uint8_t)(value & 0x1Fu);
        if (value & 0x80u) p->fixed_b = (uint8_t)(value & 0x1Fu);
        break;
    default:
        /* Everything else is decoded from regs[] when used. */
        break;
    }
}

/* ----------------------------------------------------------------- read */

static uint8_t mpy_byte(const SnesPpu *p, unsigned byte)
{
    int32_t product = (int32_t)(int16_t)p->m7a * (int32_t)(int8_t)(uint8_t)(p->m7b >> 8);
    uint32_t bits = (uint32_t)product & 0xFFFFFFu;
    return (uint8_t)(bits >> (byte * 8u));
}

uint8_t snes_ppu_read(SnesPpu *p, uint8_t reg, uint8_t mdr)
{
    switch (reg) {
    /* PPU1 drives the bus for $21x4-$21x6 and $21x8-$21xA. */
    case 0x04: case 0x05: case 0x06: case 0x08: case 0x09: case 0x0A:
    case 0x14: case 0x15: case 0x16: case 0x18: case 0x19: case 0x1A:
    case 0x24: case 0x25: case 0x26: case 0x28: case 0x29: case 0x2A:
        return p->ppu1_mdr;

    case 0x34: return p->ppu1_mdr = mpy_byte(p, 0); /* MPYL */
    case 0x35: return p->ppu1_mdr = mpy_byte(p, 1); /* MPYM */
    case 0x36: return p->ppu1_mdr = mpy_byte(p, 2); /* MPYH */

    case 0x37: /* SLHV: the machine calls snes_ppu_latch_counters */
        return mdr;

    case 0x38: /* OAMDATAREAD */
        p->ppu1_mdr = oam_read(p, p->oam_addr);
        p->oam_addr = (uint16_t)((p->oam_addr + 1u) & 0x3FFu);
        set_first_sprite(p);
        return p->ppu1_mdr;

    case 0x39: /* VMDATALREAD */
        p->ppu1_mdr = (uint8_t)(p->vram_prefetch & 0xFFu);
        if (!(p->regs[0x15] & 0x80u)) {
            p->vram_prefetch = vram_read(p);
            p->vram_addr = (uint16_t)(p->vram_addr + vram_step(p));
        }
        return p->ppu1_mdr;

    case 0x3A: /* VMDATAHREAD */
        p->ppu1_mdr = (uint8_t)(p->vram_prefetch >> 8);
        if (p->regs[0x15] & 0x80u) {
            p->vram_prefetch = vram_read(p);
            p->vram_addr = (uint16_t)(p->vram_addr + vram_step(p));
        }
        return p->ppu1_mdr;

    case 0x3B: { /* CGDATAREAD */
        uint8_t a = cgram_access_address(p, p->cgram_addr);
        if (!p->cgram_high) {
            p->ppu2_mdr = p->cgram[(unsigned)a * 2u];
            p->cgram_high = true;
        } else {
            p->ppu2_mdr = (uint8_t)((p->ppu2_mdr & 0x80u) | (p->cgram[(unsigned)a * 2u + 1u] & 0x7Fu));
            p->cgram_addr++;
            p->cgram_high = false;
        }
        return p->ppu2_mdr;
    }

    case 0x3C: /* OPHCT */
        if (!p->h_flip) p->ppu2_mdr = (uint8_t)(p->latched_h & 0xFFu);
        else p->ppu2_mdr = (uint8_t)((p->ppu2_mdr & 0xFEu) | ((p->latched_h >> 8) & 1u));
        p->h_flip = !p->h_flip;
        return p->ppu2_mdr;

    case 0x3D: /* OPVCT */
        if (!p->v_flip) p->ppu2_mdr = (uint8_t)(p->latched_v & 0xFFu);
        else p->ppu2_mdr = (uint8_t)((p->ppu2_mdr & 0xFEu) | ((p->latched_v >> 8) & 1u));
        p->v_flip = !p->v_flip;
        return p->ppu2_mdr;

    case 0x3E: /* STAT77: bit 4 is PPU1 open bus */
        p->ppu1_mdr = (uint8_t)((p->ppu1_mdr & 0x10u) | PPU1_VERSION
                                | (p->range_over ? 0x40u : 0u) | (p->time_over ? 0x80u : 0u));
        return p->ppu1_mdr;

    case 0x3F: { /* STAT78: bit 5 is PPU2 open bus; bit 4 = 0 (NTSC) */
        uint8_t v = (uint8_t)((p->ppu2_mdr & 0x20u) | PPU2_VERSION);
        p->h_flip = p->v_flip = false;
        if (!(p->wrio & 0x80u)) {
            v |= 0x40u; /* latch line held low by WRIO */
        } else {
            if (p->counters_latched) v |= 0x40u;
            p->counters_latched = false;
        }
        if (p->field) v |= 0x80u;
        return p->ppu2_mdr = v;
    }

    default:
        return mdr;
    }
}

/* ----------------------------------------------------------- frame edges */

void snes_ppu_frame_start(SnesPpu *p)
{
    p->field = !p->field;
    p->frame_interlace = ppu_interlace_reg(p);
    p->frame_last_line = 0;
    p->range_over = false;
    p->time_over = false;
    ppu_large_frame_start(p);
}

/* Double a 256-pixel row in place to 512. */
static void widen_row(uint32_t *row)
{
    for (int x = 255; x >= 0; x--) {
        uint32_t c = row[x];
        row[x * 2] = c;
        row[x * 2 + 1] = c;
    }
}

void snes_ppu_vblank_start(SnesPpu *p)
{
    if (!ppu_forced_blank(p)) oam_address_reset(p);

    /* Publish the frame. */
    unsigned lines = p->frame_last_line > 224u || (p->frame_last_line == 0 && ppu_overscan_reg(p)) ? 239u : 224u;
    unsigned rows = p->frame_interlace ? lines * 2u : lines;
    bool wide = false;
    for (unsigned r = 0; r < rows; r++) wide = wide || p->row_wide[r];
    if (wide) {
        for (unsigned r = 0; r < rows; r++) {
            if (p->row_wide[r]) continue;
            widen_row(p->framebuffer + r * SNES_FB_WIDTH);
            p->row_wide[r] = 1;
        }
    }
    p->out_width = (uint16_t)(wide ? 512u : 256u);
    p->out_height = (uint16_t)rows;
    ppu_large_frame_end(p, rows);
}

/* ------------------------------------------------------------ accessors */

bool snes_ppu_overscan(const SnesPpu *p) { return ppu_overscan_reg(p); }
bool snes_ppu_interlace(const SnesPpu *p) { return ppu_interlace_reg(p); }
bool snes_ppu_forced_blank(const SnesPpu *p) { return ppu_forced_blank(p); }
const uint8_t *snes_ppu_vram(const SnesPpu *p) { return p->vram; }
const uint8_t *snes_ppu_cgram(const SnesPpu *p) { return p->cgram; }
const uint8_t *snes_ppu_oam(const SnesPpu *p) { return p->oam; }
uint8_t snes_ppu_reg(const SnesPpu *p, uint8_t reg) { return reg <= 0x33 ? p->regs[reg] : 0; }
bool snes_ppu_field(const SnesPpu *p) { return p->field; }
