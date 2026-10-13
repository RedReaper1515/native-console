/*
 * Helpers shared by snes_ppu.c (registers) and snes_ppu_render.c.
 * Private to the PPU library.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#ifndef SNES_PPU_INTERNAL_H
#define SNES_PPU_INTERNAL_H

#include "snes_ppu.h"

/* VRAM is 32K words; addresses wrap. */
static inline uint16_t ppu_vram_word(const SnesPpu *p, uint32_t word_addr)
{
    uint32_t a = (word_addr & 0x7FFFu) << 1;
    return (uint16_t)(p->vram[a] | (p->vram[a + 1u] << 8));
}

static inline uint16_t ppu_cgram_color(const SnesPpu *p, uint8_t index)
{
    uint32_t a = (uint32_t)index << 1;
    return (uint16_t)((p->cgram[a] | (p->cgram[a + 1u] << 8)) & 0x7FFFu);
}

static inline bool ppu_forced_blank(const SnesPpu *p) { return (p->regs[0x00] & 0x80u) != 0; }
static inline unsigned ppu_bg_mode(const SnesPpu *p) { return p->regs[0x05] & 7u; }
static inline bool ppu_interlace_reg(const SnesPpu *p) { return (p->regs[0x33] & 0x01u) != 0; }
static inline bool ppu_obj_interlace(const SnesPpu *p) { return (p->regs[0x33] & 0x02u) != 0; }
static inline bool ppu_overscan_reg(const SnesPpu *p) { return (p->regs[0x33] & 0x04u) != 0; }
static inline bool ppu_pseudo_hires(const SnesPpu *p) { return (p->regs[0x33] & 0x08u) != 0; }
static inline bool ppu_extbg(const SnesPpu *p) { return (p->regs[0x33] & 0x40u) != 0; }

/* First line after the picture (225, or 240 with overscan). */
static inline unsigned ppu_vdisp(const SnesPpu *p) { return ppu_overscan_reg(p) ? 240u : 225u; }

static inline unsigned ppu_mosaic_size(const SnesPpu *p) { return (p->regs[0x06] >> 4) + 1u; }
static inline bool ppu_mosaic_any(const SnesPpu *p) { return (p->regs[0x06] & 0x0Fu) != 0; }

/* snes_ppu_render.c: the larger picture's frame edges (snes_ppu.h,
 * snes_ppu_set_mode7_scale). `rows` is the finished frame's height. */
void ppu_large_frame_start(const SnesPpu *p);
void ppu_large_frame_end(const SnesPpu *p, unsigned rows);

#endif /* SNES_PPU_INTERNAL_H */
