/*
 * SnesPpu definition. Public fields first (the machine and frontends use
 * them); the rest is private to snes_ppu*.c.
 *
 * No pointers: the machine is saved and compared as one memory block.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#ifndef SNES_PPU_STATE_H
#define SNES_PPU_STATE_H

#include <stdbool.h>
#include <stdint.h>

struct SnesPpu {
    /* Set by the machine before every call (see snes_ppu.h). */
    uint16_t hcounter; /* dot 0-339 */
    uint16_t vcounter; /* line 0-261 (262/263 in interlace) */
    bool vblank;       /* V is past the last picture line */

    /* Output (describes the last completed frame; updated by
     * snes_ppu_vblank_start). */
    uint16_t out_width, out_height;
    uint32_t framebuffer[512 * 480];

    /* ---- Private (snes_ppu*.c). ---- */

    /* Memories, raw as the CPU sees them. VRAM is 32K little-endian words;
     * CGRAM is 256 little-endian 15-bit colors; OAM is the 512-byte low
     * table followed by the 32-byte high table. */
    uint8_t vram[0x10000];
    uint8_t cgram[0x200];
    uint8_t oam[0x220];

    /* Last value written to each register $2100-$2133 (decoded on use). */
    uint8_t regs[0x40];

    /* VRAM port ($2115-$2119, $2139-$213A). */
    uint16_t vram_addr;     /* word address before remapping */
    uint16_t vram_prefetch; /* read latch */

    /* OAM port ($2102-$2104, $2138). */
    uint16_t oam_addr;      /* 10-bit byte address */
    uint16_t oam_base;      /* 10-bit reload address (OAMADD * 2) */
    uint8_t oam_latch;      /* low byte of a pending low-table pair */
    uint8_t first_sprite;   /* priority rotation (OAMADDH bit 7) */

    /* CGRAM port ($2121-$2122, $213B). */
    uint8_t cgram_addr;     /* word address */
    uint8_t cgram_latch;    /* low byte of a pending color */
    bool cgram_high;        /* next data access is the high byte */

    /* Write-twice latches. */
    uint8_t bgofs_latch1;   /* PPU1 BGnxOFS latch */
    uint8_t bgofs_latch2;   /* PPU2 BGnHOFS latch (fine scroll bits) */
    uint8_t m7_latch;       /* mode 7 latch, shared with BG1HOFS/VOFS */

    uint16_t bg_hofs[4], bg_vofs[4];
    uint16_t m7_hofs, m7_vofs;
    uint16_t m7a, m7b, m7c, m7d, m7x, m7y;

    uint8_t fixed_r, fixed_g, fixed_b; /* COLDATA */

    /* Counters and status. */
    uint16_t latched_h, latched_v;
    bool counters_latched;  /* $213F bit 6 */
    bool h_flip, v_flip;    /* $213C/$213D second-read toggles */
    uint8_t wrio;           /* CPU WRIO ($4201) shadow, for $213F bit 6 */
    uint8_t ppu1_mdr, ppu2_mdr;
    bool range_over, time_over;
    bool field;             /* interlace field, toggled at frame_start */
    uint8_t mosaic_counter;

    /* CGRAM index of each main-screen pixel of the last rendered line: the
     * address the PPU drives during active display, where CPU CGRAM
     * accesses land. */
    uint8_t cgram_pixel[256];

    /* Frame bookkeeping. */
    bool frame_interlace;   /* SETINI bit 0 latched at frame_start */
    uint16_t frame_last_line;
    uint8_t row_wide[480];  /* row holds 512 pixels (else 256) */
};

/* Additions to the snes_ppu.h interface (declared here because this header
 * is owned by the PPU implementation; snes_ppu.h includes it). */

/* The CPU's WRIO ($4201) value. Bit 7 low holds the counter latch line, so
 * $213F bit 6 then reads 1. Call on every WRIO write (power default $FF).
 * This does not latch; the machine still calls snes_ppu_latch_counters
 * when bit 7 falls. */
void snes_ppu_set_wrio(struct SnesPpu *p, uint8_t wrio);

/* Current interlace field ($213F bit 7; toggled by snes_ppu_frame_start). */
bool snes_ppu_field(const struct SnesPpu *p);

#endif /* SNES_PPU_STATE_H */
