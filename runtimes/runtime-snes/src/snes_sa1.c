/*
 * SA-1 coprocessor (see snes_sa1.h): both CPUs' views of the SA-1 board,
 * its registers, DMA and character conversion, arithmetic, the H/V timer,
 * interrupts, and the SA-1's run loop.
 *
 * Memory map, S-CPU side (banks 00-3F and 80-BF unless noted):
 *   $2200-$23FF  SA-1 registers        $3000-$37FF  I-RAM (2 KiB)
 *   $6000-$7FFF  BW-RAM, 8 KiB window  $8000-$FFFF  ROM (Super MMC)
 *   banks 40-4F  BW-RAM                banks C0-FF  ROM (Super MMC)
 * SA-1 side: the same ROM, registers and windows, I-RAM also at
 * $0000-$07FF, BW-RAM at 40-5F, and banks 60-6F as a 2 or 4 bpp bitmap view
 * of BW-RAM. The SA-1 has no access to WRAM, the PPU or the APU.
 *
 * Timing: every SA-1 bus cycle is 2 master clocks, BW-RAM 4; a jump into
 * ROM (and a branch to an odd address) costs one more cycle.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#include "snes_sa1.h"

#include <string.h>

#include "snes.h"
#include "snes_cpu_impl.h"
#include "snes_recomp.h"

#define SA1_LINES 262u /* the H/V timer's frame (NTSC) */

enum { DMA_SRC_ROM = 0, DMA_SRC_BWRAM = 1, DMA_SRC_IRAM = 2 };
enum { DMA_DST_IRAM = 0, DMA_DST_BWRAM = 1 };

static inline SnesMachine *machine_of(SnesCpu *c) { return (SnesMachine *)c->user; }

static inline bool is_rom_region(uint32_t addr)
{
    return (addr & 0x408000u) == 0x008000u || (addr & 0xC00000u) == 0xC00000u;
}

/* ------------------------------------------------------------------ */
/* ROM (Super MMC)                                                     */
/* ------------------------------------------------------------------ */

/* The ROM address before mirroring: banks 00-3F/80-BF $8000-$FFFF are
 * LoROM-style 32 KiB halves, banks C0-FF whole banks, and each 1 MiB region
 * follows its bank register (the LoROM halves only when its mode bit is
 * set; otherwise they are linear). */
static uint32_t rom_linear(const SnesSa1 *s, uint32_t addr)
{
    if ((addr & 0x408000u) == 0x008000u) addr = (addr & 0x800000u) >> 2 | (addr & 0x3F0000u) >> 1 | (addr & 0x7FFFu);
    const bool lo = addr < 0x400000u;
    addr &= 0x3FFFFFu;
    const unsigned region = addr >> 20;
    if (lo && !s->mmc_mode[region]) return addr;
    return (uint32_t)s->mmc_bank[region] << 20 | (addr & 0xFFFFFu);
}

/* Any ROM read, including the S-CPU vector override at 00:FFEA/FFEE. */
static uint8_t rom_read(const SnesMachine *m, uint32_t addr)
{
    const SnesSa1 *s = &m->sa1;
    if ((addr & 0xFFFFF0u) == 0x00FFE0u) {
        if (addr == 0x00FFEAu && s->cpu_nvsw) return (uint8_t)s->snv;
        if (addr == 0x00FFEBu && s->cpu_nvsw) return (uint8_t)(s->snv >> 8);
        if (addr == 0x00FFEEu && s->cpu_ivsw) return (uint8_t)s->siv;
        if (addr == 0x00FFEFu && s->cpu_ivsw) return (uint8_t)(s->siv >> 8);
    }
    if (!m->rom_size) return 0;
    return m->rom[snes_rom_mirror(rom_linear(s, addr), m->rom_size)];
}

/* Reads of the page holding the S-CPU vectors go through the SA-1 while
 * either override is on. */
static void update_vector_page(SnesMachine *m)
{
    m->page_type[0x00F] = (m->sa1.cpu_nvsw || m->sa1.cpu_ivsw) ? SNES_PAGE_COPROC : SNES_PAGE_ROM;
}

static void map_rom(SnesMachine *m)
{
    SnesSa1 *s = &m->sa1;
    for (uint32_t page = 0; page < 4096u; page++) {
        const uint32_t addr = page << 12;
        if (!is_rom_region(addr)) continue;
        m->page_type[page] = SNES_PAGE_ROM;
        m->page_offset[page] = m->rom_size ? snes_rom_mirror(rom_linear(s, addr), m->rom_size) : 0;
        m->page_mask[page] = 0xFFFFFFFFu;
    }
    s->mmc_linear = true;
    for (unsigned i = 0; i < 4; i++) {
        if (s->mmc_bank[i] != i) s->mmc_linear = false;
    }
    update_vector_page(m);
}

void snes_sa1_map(SnesMachine *m)
{
    SnesSa1 *s = &m->sa1;
    s->present = true;
    for (unsigned i = 0; i < 4; i++) {
        s->mmc_bank[i] = (uint8_t)i;
        s->mmc_mode[i] = false;
    }
    for (uint32_t bank = 0; bank < 0x100u; bank++) {
        if (!(bank & 0x40u)) {
            m->page_type[(bank << 4) | 6u] = SNES_PAGE_COPROC; /* BW-RAM window */
            m->page_type[(bank << 4) | 7u] = SNES_PAGE_COPROC;
        } else if (bank < 0x50u) {
            for (uint32_t p = 0; p < 16u; p++) m->page_type[(bank << 4) | p] = SNES_PAGE_COPROC;
        }
    }
    map_rom(m);
}

/* ------------------------------------------------------------------ */
/* BW-RAM and I-RAM                                                    */
/* ------------------------------------------------------------------ */

static inline uint8_t bwram_read(const SnesMachine *m, uint32_t addr)
{
    const uint32_t size = m->cart.sram_size;
    return size ? m->sram[addr & (size - 1u)] : 0;
}

static inline void bwram_write(SnesMachine *m, uint32_t addr, uint8_t v)
{
    const uint32_t size = m->cart.sram_size;
    if (!size) return;
    uint8_t *p = &m->sram[addr & (size - 1u)];
    if (*p != v) {
        *p = v;
        m->sram_dirty = true;
    }
}

/* Write protection covers the first 256 << BWPA bytes, and only while both
 * CPUs' write enables are off (Kirby's Dream Land 3 depends on this). */
static inline bool bwram_protected(const SnesSa1 *s, uint32_t addr)
{
    return !s->swen && !s->cwen && (addr & 0x3FFFFu) < (0x100u << s->bwp);
}

/* The bitmap view: each byte of BW-RAM holds two 4-bit or four 2-bit pixels. */
static uint8_t bitmap_read(const SnesMachine *m, uint32_t addr)
{
    addr &= 0xFFFFFu;
    if (!m->sa1.bbf) {
        uint8_t b = bwram_read(m, addr >> 1);
        return (addr & 1u) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0Fu);
    }
    uint8_t b = bwram_read(m, addr >> 2);
    return (uint8_t)((b >> ((addr & 3u) * 2u)) & 3u);
}

static void bitmap_write(SnesMachine *m, uint32_t addr, uint8_t v)
{
    addr &= 0xFFFFFu;
    if (!m->sa1.bbf) {
        const unsigned shift = (addr & 1u) * 4u;
        uint8_t b = bwram_read(m, addr >> 1);
        bwram_write(m, addr >> 1, (uint8_t)((b & ~(0x0Fu << shift)) | ((v & 0x0Fu) << shift)));
    } else {
        const unsigned shift = (addr & 3u) * 2u;
        uint8_t b = bwram_read(m, addr >> 2);
        bwram_write(m, addr >> 2, (uint8_t)((b & ~(0x03u << shift)) | ((v & 0x03u) << shift)));
    }
}

static inline bool iram_writable(uint8_t protect, uint32_t addr) { return (protect >> ((addr >> 8) & 7u)) & 1u; }

/* ------------------------------------------------------------------ */
/* Interrupt lines                                                     */
/* ------------------------------------------------------------------ */

static void set_cpu_irq(SnesMachine *m, bool level, uint64_t at) { snes_set_coproc_irq(m, level, at); }

/* Evaluated between SA-1 instructions (ares evaluates the same conditions in
 * the last cycle of each instruction; the I flag is the one of that cycle). */
static bool poll_interrupts(SnesSa1 *s)
{
    if (s->sa1_nmi && !s->sa1_nmicl) {
        s->vector = s->cnv;
        s->sa1_nmifl = true;
        s->sa1_nmicl = true;
        return true;
    }
    if (s->cpu.last_i) return false;
    if (s->timer_irqen && !s->timer_irqcl) {
        s->timer_irqfl = true;
    } else if (s->dma_irqen && !s->dma_irqcl) {
        s->dma_irqfl = true;
    } else if (s->sa1_irq && !s->sa1_irqcl) {
        s->sa1_irqfl = true;
    } else {
        return false;
    }
    s->vector = s->civ;
    return true;
}

/* An IRQ condition is up (taken as soon as I clears). */
static bool irq_asserted(const SnesSa1 *s)
{
    return (s->timer_irqen && !s->timer_irqcl) || (s->dma_irqen && !s->dma_irqcl) || (s->sa1_irq && !s->sa1_irqcl);
}

/* The SA-1 enters its handler at a vector register: no vector fetch. */
static void take_interrupt(SnesCpu *c, uint16_t vector)
{
    const unsigned mode = snes_cpu_mode(c);
    (void)snes_rd(c, snes_cpu_pc24(c));
    snes_io(c);
    if (!SNES_EMU(mode)) snes_push(c, mode, c->pbr);
    snes_push(c, mode, (uint8_t)(c->pc >> 8));
    snes_push(c, mode, (uint8_t)c->pc);
    const uint8_t p = snes_cpu_get_p(c);
    snes_push(c, mode, SNES_EMU(mode) ? (uint8_t)(p & ~0x10u) : p);
    c->flag_i = 1;
    c->flag_d = 0;
    c->last_i = 1;
    c->pc = vector;
    c->pbr = 0;
    c->wai = 0;
    c->branched = 1;
}

/* ------------------------------------------------------------------ */
/* H/V timer                                                           */
/* ------------------------------------------------------------------ */

/* Counter values at SA-1 time t: H in master clocks. The H/V mode wraps H
 * at 1364 and V at 262; the linear mode is one 20-bit counter (H 11 bits,
 * V 9 bits). */
static void timer_position(const SnesSa1 *s, uint64_t t, uint32_t *h, uint32_t *v)
{
    const uint64_t elapsed = t > s->timer_base ? t - s->timer_base : 0;
    if (!s->hvselb) {
        const uint64_t total = ((uint64_t)s->timer_v0 * 1364u + s->timer_h0 + elapsed) % (1364ull * SA1_LINES);
        *h = (uint32_t)(total % 1364u);
        *v = (uint32_t)(total / 1364u);
    } else {
        const uint64_t total = ((((uint64_t)s->timer_v0 << 11) | s->timer_h0) + elapsed) & 0xFFFFFu;
        *h = (uint32_t)(total & 0x7FFu);
        *v = (uint32_t)(total >> 11);
    }
}

static void timer_rebase(SnesSa1 *s, uint64_t now)
{
    uint32_t h, v;
    timer_position(s, now, &h, &v);
    s->timer_h0 = (uint16_t)h;
    s->timer_v0 = (uint16_t)v;
    s->timer_base = now;
}

/* The first time after `from` at which the counters match HCNT/VCNT. */
static uint64_t timer_next_after(const SnesSa1 *s, uint64_t from)
{
    if (!s->hen && !s->ven) return UINT64_MAX;
    const uint32_t width = s->hvselb ? 2048u : 1364u, lines = s->hvselb ? 512u : SA1_LINES;
    const uint64_t frame = (uint64_t)width * lines;
    const uint32_t hc = (uint32_t)s->hcnt << 2;
    if (s->hen && hc >= width) return UINT64_MAX;
    if (s->ven && s->vcnt >= lines) return UINT64_MAX;
    uint32_t h, v;
    timer_position(s, from, &h, &v);
    uint64_t delta;
    if (!s->ven) {
        delta = (hc + width - h) % width;
        if (!delta) delta = width;
    } else {
        const uint64_t at = (uint64_t)s->vcnt * width + (s->hen ? hc : 0u), pos = (uint64_t)v * width + h;
        delta = (at + frame - pos) % frame;
        if (!delta) delta = frame;
    }
    return from + delta;
}

static void timer_catch_up(SnesSa1 *s, uint64_t now)
{
    while (s->timer_next <= now) {
        s->timer_irqfl = true;
        if (s->timer_irqen) s->timer_irqcl = false;
        s->timer_next = timer_next_after(s, s->timer_next);
    }
}

/* ------------------------------------------------------------------ */
/* DMA and character conversion                                        */
/* ------------------------------------------------------------------ */

static void dma_normal(SnesMachine *m)
{
    SnesSa1 *s = &m->sa1;
    SnesCpu *c = &s->cpu;
    while (s->dtc-- != 0) {
        uint8_t data = c->mdr;
        const uint32_t source = s->dsa, target = s->dda;
        s->dsa = (s->dsa + 1u) & 0xFFFFFFu;
        s->dda = (s->dda + 1u) & 0xFFFFFFu;
        if (s->sd == DMA_SRC_ROM && s->dd == DMA_DST_BWRAM) {
            c->cycles += 4;
            data = rom_read(m, source);
            bwram_write(m, target, data);
        } else if (s->sd == DMA_SRC_ROM && s->dd == DMA_DST_IRAM) {
            c->cycles += 2;
            data = rom_read(m, source);
            s->iram[target & 0x7FFu] = data;
        } else if (s->sd == DMA_SRC_BWRAM && s->dd == DMA_DST_IRAM) {
            c->cycles += 4;
            data = bwram_read(m, source);
            s->iram[target & 0x7FFu] = data;
        } else if (s->sd == DMA_SRC_IRAM && s->dd == DMA_DST_BWRAM) {
            c->cycles += 4;
            data = s->iram[source & 0x7FFu];
            bwram_write(m, target, data);
        }
    }
    s->dma_irqfl = true;
    if (s->dma_irqen) s->dma_irqcl = false;
}

/* Type 1: the S-CPU reads a bitmap in BW-RAM as planar characters; each
 * character is converted into I-RAM when its first byte is read. */
static void dma_cc1(SnesMachine *m, uint64_t at)
{
    SnesSa1 *s = &m->sa1;
    s->bwram_dma = true;
    s->chdma_irqfl = true;
    if (s->chdma_irqen) {
        s->chdma_irqcl = false;
        set_cpu_irq(m, true, at);
    }
}

static uint8_t dma_cc1_read(SnesMachine *m, uint32_t addr)
{
    SnesSa1 *s = &m->sa1;
    const uint32_t charmask = (1u << (6u - s->dmacb)) - 1u;
    if ((addr & charmask) == 0 && m->cart.sram_size) {
        const uint32_t bpp = 2u << (2u - s->dmacb);
        const uint32_t bpl = (8u << s->dmasize) >> s->dmacb;
        const uint32_t bwmask = m->cart.sram_size - 1u;
        const uint32_t tile = ((addr - s->dsa) & bwmask) >> (6u - s->dmacb);
        const uint32_t ty = tile >> s->dmasize, tx = tile & ((1u << s->dmasize) - 1u);
        uint32_t bwaddr = s->dsa + ty * 8u * bpl + tx * bpp;
        for (uint32_t y = 0; y < 8; y++) {
            uint64_t data = 0;
            for (uint32_t byte = 0; byte < bpp; byte++)
                data |= (uint64_t)bwram_read(m, (bwaddr + byte) & bwmask) << (byte << 3);
            bwaddr += bpl;
            uint8_t out[8] = { 0 };
            for (uint32_t x = 0; x < 8; x++) {
                const unsigned bit = 7u - x;
                out[0] |= (uint8_t)((data & 1u) << bit), data >>= 1;
                out[1] |= (uint8_t)((data & 1u) << bit), data >>= 1;
                if (s->dmacb == 2) continue;
                out[2] |= (uint8_t)((data & 1u) << bit), data >>= 1;
                out[3] |= (uint8_t)((data & 1u) << bit), data >>= 1;
                if (s->dmacb == 1) continue;
                out[4] |= (uint8_t)((data & 1u) << bit), data >>= 1;
                out[5] |= (uint8_t)((data & 1u) << bit), data >>= 1;
                out[6] |= (uint8_t)((data & 1u) << bit), data >>= 1;
                out[7] |= (uint8_t)((data & 1u) << bit), data >>= 1;
            }
            for (uint32_t byte = 0; byte < bpp; byte++) {
                const uint32_t p = s->dda + (y << 1) + ((byte & 6u) << 3) + (byte & 1u);
                s->iram[p & 0x7FFu] = out[byte];
            }
        }
    }
    return s->iram[(s->dda + (addr & charmask)) & 0x7FFu];
}

/* Type 2: the SA-1 writes a line of pixels to BRF; each 8 pixels become
 * one line of a planar character in I-RAM. */
static void dma_cc2(SnesSa1 *s)
{
    const uint8_t *brf = &s->brf[(s->dma_line & 1u) << 3];
    const uint32_t bpp = 2u << (2u - s->dmacb);
    uint32_t address = s->dda & 0x7FFu;
    address &= ~((1u << (7u - s->dmacb)) - 1u);
    address += (s->dma_line & 8u) * bpp;
    address += (s->dma_line & 7u) * 2u;
    for (uint32_t byte = 0; byte < bpp; byte++) {
        uint8_t output = 0;
        for (uint32_t bit = 0; bit < 8; bit++) output |= (uint8_t)(((brf[bit] >> byte) & 1u) << (7u - bit));
        s->iram[(address + ((byte & 6u) << 3) + (byte & 1u)) & 0x7FFu] = output;
    }
    s->dma_line = (uint8_t)((s->dma_line + 1u) & 15u);
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

/* $2231-$2237: written by either CPU; `at` is the writer's clock. */
static void write_shared(SnesMachine *m, uint32_t reg, uint8_t v, uint64_t at)
{
    SnesSa1 *s = &m->sa1;
    switch (reg) {
    case 0x2231:
        s->dmacb = v & 3u;
        s->dmasize = (v >> 2) & 7u;
        s->chdend = (v & 0x80u) != 0;
        if (s->dmacb > 2) s->dmacb = 2;
        if (s->dmasize > 5) s->dmasize = 5;
        if (s->chdend) s->bwram_dma = false;
        break;
    case 0x2232: s->dsa = (s->dsa & 0xFFFF00u) | v; break;
    case 0x2233: s->dsa = (s->dsa & 0xFF00FFu) | (uint32_t)v << 8; break;
    case 0x2234: s->dsa = (s->dsa & 0x00FFFFu) | (uint32_t)v << 16; break;
    case 0x2235: s->dda = (s->dda & 0xFFFF00u) | v; break;
    case 0x2236:
        s->dda = (s->dda & 0xFF00FFu) | (uint32_t)v << 8;
        if (s->dmaen) {
            if (!s->cden && s->dd == DMA_DST_IRAM) dma_normal(m);
            else if (s->cden && s->cdsel) dma_cc1(m, at);
        }
        break;
    case 0x2237:
        s->dda = (s->dda & 0x00FFFFu) | (uint32_t)v << 16;
        if (s->dmaen && !s->cden && s->dd == DMA_DST_BWRAM) dma_normal(m);
        break;
    default: break;
    }
}

static uint8_t read_cpu_reg(SnesMachine *m, uint32_t reg, uint8_t mdr)
{
    const SnesSa1 *s = &m->sa1;
    if (reg == 0x2300) {
        return (uint8_t)((s->cmeg & 0x0Fu) | s->cpu_nvsw << 4 | s->chdma_irqfl << 5 | s->cpu_ivsw << 6 |
                         s->cpu_irqfl << 7);
    }
    return mdr; /* write-only, or SA-1 side only */
}

static void write_cpu_reg(SnesMachine *m, uint32_t reg, uint8_t v)
{
    SnesSa1 *s = &m->sa1;
    const uint64_t now = m->cpu.cycles;
    switch (reg) {
    case 0x2200: /* CCNT */
        if (s->sa1_resb && !(v & 0x20u)) {
            /* Leaving reset: the 65C816 reset sequence (emulation mode, 8-bit
             * registers, I set, D clear, D/DBR/PBR zero, out of WAI and STP),
             * with PC from CRV. A and the arithmetic registers survive
             * (absindx SA1RebootTest, measured on hardware). */
            SnesCpu *c = &s->cpu;
            c->flag_e = 1;
            c->flag_m = c->flag_x = 1;
            c->flag_i = c->last_i = 1;
            c->flag_d = 0;
            c->x &= 0xFFu;
            c->y &= 0xFFu;
            c->s = (uint16_t)(0x0100u | (c->s & 0xFFu));
            c->d = 0;
            c->dbr = c->pbr = 0;
            c->wai = c->stp = 0;
            c->pc = s->crv;
            s->int_pending = false;
            s->ciwp = 0;
        }
        s->smeg = v & 0x0Fu;
        s->sa1_nmi = (v & 0x10u) != 0;
        s->sa1_resb = (v & 0x20u) != 0;
        s->sa1_rdyb = (v & 0x40u) != 0;
        s->sa1_irq = (v & 0x80u) != 0;
        if (s->sa1_irq) {
            s->sa1_irqfl = true;
            if (s->sa1_irqen) s->sa1_irqcl = false;
        }
        if (s->sa1_nmi) {
            s->sa1_nmifl = true;
            if (s->sa1_nmien) s->sa1_nmicl = false;
        }
        break;
    case 0x2201: /* SIE */
        if (!s->chdma_irqen && (v & 0x20u) && s->chdma_irqfl) {
            s->chdma_irqcl = false;
            set_cpu_irq(m, true, now);
        }
        if (!s->cpu_irqen && (v & 0x80u) && s->cpu_irqfl) {
            s->cpu_irqcl = false;
            set_cpu_irq(m, true, now);
        }
        s->chdma_irqen = (v & 0x20u) != 0;
        s->cpu_irqen = (v & 0x80u) != 0;
        break;
    case 0x2202: /* SIC */
        s->chdma_irqcl = (v & 0x20u) != 0;
        s->cpu_irqcl = (v & 0x80u) != 0;
        if (s->chdma_irqcl) s->chdma_irqfl = false;
        if (s->cpu_irqcl) s->cpu_irqfl = false;
        if (!s->chdma_irqfl && !s->cpu_irqfl) set_cpu_irq(m, false, now);
        break;
    case 0x2203: s->crv = (uint16_t)((s->crv & 0xFF00u) | v); break;
    case 0x2204: s->crv = (uint16_t)((s->crv & 0x00FFu) | v << 8); break;
    case 0x2205: s->cnv = (uint16_t)((s->cnv & 0xFF00u) | v); break;
    case 0x2206: s->cnv = (uint16_t)((s->cnv & 0x00FFu) | v << 8); break;
    case 0x2207: s->civ = (uint16_t)((s->civ & 0xFF00u) | v); break;
    case 0x2208: s->civ = (uint16_t)((s->civ & 0x00FFu) | v << 8); break;
    case 0x2220:
    case 0x2221:
    case 0x2222:
    case 0x2223: /* CXB-FXB: Super MMC */
        s->mmc_bank[reg - 0x2220u] = v & 7u;
        s->mmc_mode[reg - 0x2220u] = (v & 0x80u) != 0;
        map_rom(m);
        break;
    case 0x2224: s->sbm = v & 0x1Fu; break;
    case 0x2226: s->swen = (v & 0x80u) != 0; break;
    case 0x2228: s->bwp = v & 0x0Fu; break;
    case 0x2229: s->siwp = v; break;
    case 0x2231:
    case 0x2232:
    case 0x2233:
    case 0x2234:
    case 0x2235:
    case 0x2236:
    case 0x2237: write_shared(m, reg, v, now); break;
    default: break;
    }
}

static uint8_t vbr_read(const SnesMachine *m, uint32_t addr)
{
    addr &= 0xFFFFFFu;
    if (is_rom_region(addr)) return rom_read(m, addr);
    if ((addr & 0x40E000u) == 0x006000u || (addr & 0xF00000u) == 0x400000u) return bwram_read(m, addr);
    if ((addr & 0x40F800u) == 0x000000u || (addr & 0x40F800u) == 0x003000u) return m->sa1.iram[addr & 0x7FFu];
    return 0xFF;
}

static uint32_t vbr_bits(const SnesMachine *m)
{
    const SnesSa1 *s = &m->sa1;
    uint32_t data = vbr_read(m, s->va) | (uint32_t)vbr_read(m, s->va + 1u) << 8 | (uint32_t)vbr_read(m, s->va + 2u) << 16;
    return data >> s->vbit;
}

static void vbr_advance(SnesSa1 *s)
{
    s->vbit = (uint8_t)(s->vbit + s->vb);
    s->va = (s->va + (s->vbit >> 3)) & 0xFFFFFFu;
    s->vbit &= 7u;
}

static uint8_t read_sa1_reg(SnesMachine *m, uint32_t reg)
{
    SnesSa1 *s = &m->sa1;
    switch (reg) {
    case 0x2301:
        return (uint8_t)((s->smeg & 0x0Fu) | s->sa1_nmifl << 4 | s->dma_irqfl << 5 | s->timer_irqfl << 6 |
                         s->sa1_irqfl << 7);
    case 0x2302: {
        uint32_t h, v;
        timer_position(s, s->cpu.cycles, &h, &v);
        s->hcr = (uint16_t)(h >> 2);
        s->vcr = (uint16_t)v;
        return (uint8_t)s->hcr;
    }
    case 0x2303: return (uint8_t)(s->hcr >> 8);
    case 0x2304: return (uint8_t)s->vcr;
    case 0x2305: return (uint8_t)(s->vcr >> 8);
    case 0x2306:
    case 0x2307:
    case 0x2308:
    case 0x2309:
    case 0x230A: return (uint8_t)(s->mr >> ((reg - 0x2306u) * 8u));
    case 0x230B: return (uint8_t)(s->overflow << 7);
    case 0x230C: return (uint8_t)vbr_bits(m);
    case 0x230D: {
        const uint32_t data = vbr_bits(m);
        if (s->hl) vbr_advance(s);
        return (uint8_t)(data >> 8);
    }
    default: return 0xFF;
    }
}

static void arithmetic(SnesSa1 *s)
{
    const int32_t a = (int16_t)s->ma, b = (int16_t)s->mb;
    if (!s->acm) {
        if (!s->md) {
            /* Signed multiply. */
            s->mr = (uint32_t)(a * b);
            s->mb = 0;
        } else {
            /* Signed dividend, unsigned divisor; the remainder is never negative. */
            if (s->mb == 0) {
                s->mr = 0;
            } else {
                const int32_t divisor = s->mb;
                const int32_t rem = a >= 0 ? a % divisor : (a % divisor + divisor) % divisor;
                const uint16_t quot = (uint16_t)((a - rem) / divisor);
                s->mr = (uint32_t)((uint32_t)(uint16_t)rem << 16 | quot);
            }
            s->ma = 0;
            s->mb = 0;
        }
    } else {
        /* Cumulative sum of products, 40 bits; bit 40 is the overflow flag. */
        s->mr += (uint64_t)(int64_t)(a * b);
        s->overflow = (s->mr >> 40) != 0;
        s->mr &= 0xFFFFFFFFFFull;
        s->mb = 0;
    }
}

static void write_sa1_reg(SnesMachine *m, uint32_t reg, uint8_t v)
{
    SnesSa1 *s = &m->sa1;
    SnesCpu *c = &s->cpu;
    const uint64_t now = c->cycles;
    switch (reg) {
    case 0x2209: /* SCNT */
        s->cmeg = v & 0x0Fu;
        s->cpu_nvsw = (v & 0x10u) != 0;
        s->cpu_ivsw = (v & 0x40u) != 0;
        s->cpu_irq = (v & 0x80u) != 0;
        update_vector_page(m);
        if (s->cpu_irq) {
            s->cpu_irqfl = true;
            if (s->cpu_irqen) {
                s->cpu_irqcl = false;
                set_cpu_irq(m, true, now);
            }
        }
        break;
    case 0x220A: /* CIE */
        if (!s->sa1_nmien && (v & 0x10u) && s->sa1_nmifl) s->sa1_nmicl = false;
        if (!s->dma_irqen && (v & 0x20u) && s->dma_irqfl) s->dma_irqcl = false;
        if (!s->timer_irqen && (v & 0x40u) && s->timer_irqfl) s->timer_irqcl = false;
        if (!s->sa1_irqen && (v & 0x80u) && s->sa1_irqfl) s->sa1_irqcl = false;
        s->sa1_nmien = (v & 0x10u) != 0;
        s->dma_irqen = (v & 0x20u) != 0;
        s->timer_irqen = (v & 0x40u) != 0;
        s->sa1_irqen = (v & 0x80u) != 0;
        break;
    case 0x220B: /* CIC */
        s->sa1_nmicl = (v & 0x10u) != 0;
        s->dma_irqcl = (v & 0x20u) != 0;
        s->timer_irqcl = (v & 0x40u) != 0;
        s->sa1_irqcl = (v & 0x80u) != 0;
        if (s->sa1_nmicl) s->sa1_nmifl = false;
        if (s->sa1_irqcl) s->sa1_irqfl = false;
        if (s->timer_irqcl) s->timer_irqfl = false;
        if (s->dma_irqcl) s->dma_irqfl = false;
        break;
    case 0x220C: s->snv = (uint16_t)((s->snv & 0xFF00u) | v); break;
    case 0x220D: s->snv = (uint16_t)((s->snv & 0x00FFu) | v << 8); break;
    case 0x220E: s->siv = (uint16_t)((s->siv & 0xFF00u) | v); break;
    case 0x220F: s->siv = (uint16_t)((s->siv & 0x00FFu) | v << 8); break;
    case 0x2210: /* TMC */
        timer_rebase(s, now);
        s->hen = (v & 0x01u) != 0;
        s->ven = (v & 0x02u) != 0;
        s->hvselb = (v & 0x80u) != 0;
        s->timer_next = timer_next_after(s, now);
        break;
    case 0x2211: /* CTR: restart the timer */
        s->timer_base = now;
        s->timer_h0 = s->timer_v0 = 0;
        s->timer_next = timer_next_after(s, now);
        break;
    case 0x2212: s->hcnt = (uint16_t)((s->hcnt & 0xFF00u) | v); s->timer_next = timer_next_after(s, now); break;
    case 0x2213: s->hcnt = (uint16_t)((s->hcnt & 0x00FFu) | v << 8); s->timer_next = timer_next_after(s, now); break;
    case 0x2214: s->vcnt = (uint16_t)((s->vcnt & 0xFF00u) | v); s->timer_next = timer_next_after(s, now); break;
    case 0x2215: s->vcnt = (uint16_t)((s->vcnt & 0x00FFu) | v << 8); s->timer_next = timer_next_after(s, now); break;
    case 0x2225: /* BMAP */
        s->cbm = v & 0x7Fu;
        s->sw46 = (v & 0x80u) != 0;
        break;
    case 0x2227: s->cwen = (v & 0x80u) != 0; break;
    case 0x222A: s->ciwp = v; break;
    case 0x2230: /* DCNT */
        s->sd = v & 3u;
        s->dd = (v & 0x04u) != 0;
        s->cdsel = (v & 0x10u) != 0;
        s->cden = (v & 0x20u) != 0;
        s->dprio = (v & 0x40u) != 0;
        s->dmaen = (v & 0x80u) != 0;
        if (!s->dmaen) s->dma_line = 0;
        break;
    case 0x2231:
    case 0x2232:
    case 0x2233:
    case 0x2234:
    case 0x2235:
    case 0x2236:
    case 0x2237: write_shared(m, reg, v, now); break;
    case 0x2238: s->dtc = (uint16_t)((s->dtc & 0xFF00u) | v); break;
    case 0x2239: s->dtc = (uint16_t)((s->dtc & 0x00FFu) | v << 8); break;
    case 0x223F: s->bbf = (v & 0x80u) != 0; break;
    case 0x2250: /* MCNT */
        s->md = (v & 0x01u) != 0;
        s->acm = (v & 0x02u) != 0;
        if (s->acm) s->mr = 0;
        break;
    case 0x2251: s->ma = (uint16_t)((s->ma & 0xFF00u) | v); break;
    case 0x2252: s->ma = (uint16_t)((s->ma & 0x00FFu) | v << 8); break;
    case 0x2253: s->mb = (uint16_t)((s->mb & 0xFF00u) | v); break;
    case 0x2254:
        s->mb = (uint16_t)((s->mb & 0x00FFu) | v << 8);
        arithmetic(s);
        break;
    case 0x2258: /* VBD */
        s->vb = v & 0x0Fu;
        s->hl = (v & 0x80u) != 0;
        if (s->vb == 0) s->vb = 16;
        if (!s->hl) vbr_advance(s);
        break;
    case 0x2259: s->va = (s->va & 0xFFFF00u) | v; break;
    case 0x225A: s->va = (s->va & 0xFF00FFu) | (uint32_t)v << 8; break;
    case 0x225B:
        s->va = (s->va & 0x00FFFFu) | (uint32_t)v << 16;
        s->vbit = 0;
        break;
    default:
        if (reg >= 0x2240u && reg <= 0x224Fu) {
            s->brf[reg - 0x2240u] = v;
            if ((reg == 0x2247u || reg == 0x224Fu) && s->dmaen && s->cden && !s->cdsel) dma_cc2(s);
        }
        break;
    }
    /* Any register write can raise an SA-1 interrupt or move the timer:
     * recompiled SA-1 code returns to snes_sa1_run to look. */
    c->next_sync = 0;
}

/* ------------------------------------------------------------------ */
/* S-CPU side                                                          */
/* ------------------------------------------------------------------ */

/* BW-RAM through the S-CPU: the $6000-$7FFF window selects an 8 KiB block
 * (BMAPS); character conversion type 1 substitutes converted data. */
static uint8_t bwram_cpu_read(SnesMachine *m, uint32_t addr, uint8_t mdr)
{
    if (m->sa1.bwram_dma) return dma_cc1_read(m, addr);
    return m->cart.sram_size ? bwram_read(m, addr) : mdr;
}

static void bwram_cpu_write(SnesMachine *m, uint32_t addr, uint8_t v)
{
    if (bwram_protected(&m->sa1, addr)) return;
    bwram_write(m, addr, v);
}

uint8_t snes_sa1_cpu_read(SnesMachine *m, uint32_t addr, uint8_t mdr)
{
    SnesSa1 *s = &m->sa1;
    const uint32_t bank = addr >> 16, a = addr & 0xFFFFu;
    if (!(bank & 0x40u)) {
        if (a >= 0x8000u) {
            /* The vector page, while an override is on. */
            if ((addr & 0xFFFFF0u) == 0x00FFE0u) snes_sa1_catch_up(m, s, m->cpu.cycles);
            return rom_read(m, addr);
        }
        if (a >= 0x6000u) {
            snes_sa1_catch_up(m, s, m->cpu.cycles);
            return bwram_cpu_read(m, (uint32_t)s->sbm * 0x2000u + (a & 0x1FFFu), mdr);
        }
        if (a >= 0x3000u && a < 0x3800u) {
            snes_sa1_catch_up(m, s, m->cpu.cycles);
            return s->iram[a & 0x7FFu];
        }
        if (a >= 0x2200u && a < 0x2400u) {
            snes_sa1_catch_up(m, s, m->cpu.cycles);
            return read_cpu_reg(m, 0x2200u | (a & 0x1FFu), mdr);
        }
        return mdr;
    }
    if (bank < 0x50u) {
        snes_sa1_catch_up(m, s, m->cpu.cycles);
        return bwram_cpu_read(m, addr, mdr);
    }
    if (bank >= 0xC0u) return rom_read(m, addr);
    return mdr;
}

void snes_sa1_cpu_write(SnesMachine *m, uint32_t addr, uint8_t v)
{
    SnesSa1 *s = &m->sa1;
    const uint32_t bank = addr >> 16, a = addr & 0xFFFFu;
    if (!(bank & 0x40u)) {
        if (a >= 0x8000u) return;
        if (a >= 0x6000u) {
            snes_sa1_catch_up(m, s, m->cpu.cycles);
            bwram_cpu_write(m, (uint32_t)s->sbm * 0x2000u + (a & 0x1FFFu), v);
        } else if (a >= 0x3000u && a < 0x3800u) {
            snes_sa1_catch_up(m, s, m->cpu.cycles);
            if (iram_writable(s->siwp, a)) s->iram[a & 0x7FFu] = v;
        } else if (a >= 0x2200u && a < 0x2400u) {
            snes_sa1_catch_up(m, s, m->cpu.cycles);
            write_cpu_reg(m, 0x2200u | (a & 0x1FFu), v);
        }
        return;
    }
    if (bank < 0x50u) {
        snes_sa1_catch_up(m, s, m->cpu.cycles);
        bwram_cpu_write(m, addr, v);
    }
}

uint8_t snes_sa1_peek(const SnesMachine *m, uint32_t addr)
{
    const SnesSa1 *s = &m->sa1;
    const uint32_t bank = addr >> 16, a = addr & 0xFFFFu;
    if (!(bank & 0x40u)) {
        if (a >= 0x8000u) return rom_read(m, addr);
        if (a >= 0x6000u) return bwram_read(m, (uint32_t)s->sbm * 0x2000u + (a & 0x1FFFu));
        if (a >= 0x3000u && a < 0x3800u) return s->iram[a & 0x7FFu];
        return 0;
    }
    if (bank < 0x50u) return bwram_read(m, addr);
    if (bank >= 0xC0u) return rom_read(m, addr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SA-1 side                                                           */
/* ------------------------------------------------------------------ */

static inline bool is_bwram(uint32_t addr)
{
    return (addr & 0x40E000u) == 0x006000u || (addr & 0xE00000u) == 0x400000u || (addr & 0xF00000u) == 0x600000u;
}

static inline bool is_iram(uint32_t addr)
{
    return (addr & 0x40F800u) == 0x000000u || (addr & 0x40F800u) == 0x003000u;
}

uint8_t snes_sa1_bus_read(SnesCpu *c, uint32_t addr)
{
    SnesMachine *m = machine_of(c);
    SnesSa1 *s = &m->sa1;
    uint8_t v;
    if ((addr & 0x40FE00u) == 0x002200u) {
        c->cycles += 2;
        v = read_sa1_reg(m, 0x2200u | (addr & 0x1FFu));
    } else if (is_rom_region(addr)) {
        c->cycles += 2;
        const uint32_t page = addr >> 12;
        v = ((addr & 0xFFFFF0u) == 0x00FFE0u || !m->rom_size) ? rom_read(m, addr)
                                                               : m->rom[m->page_offset[page] + (addr & 0xFFFu)];
    } else if (is_bwram(addr)) {
        c->cycles += 4;
        if ((addr & 0x600000u) == 0x600000u) {
            v = bitmap_read(m, addr);
        } else if (addr & 0x400000u) {
            v = bwram_read(m, addr);
        } else if (!s->sw46) {
            v = bwram_read(m, (uint32_t)(s->cbm & 0x1Fu) * 0x2000u + (addr & 0x1FFFu));
        } else {
            v = bitmap_read(m, (uint32_t)s->cbm * 0x2000u + (addr & 0x1FFFu));
        }
    } else if (is_iram(addr)) {
        c->cycles += 2;
        v = s->iram[addr & 0x7FFu];
    } else {
        c->cycles += 2;
        return c->mdr;
    }
    c->mdr = v;
    return v;
}

void snes_sa1_bus_write(SnesCpu *c, uint32_t addr, uint8_t v)
{
    SnesMachine *m = machine_of(c);
    SnesSa1 *s = &m->sa1;
    c->mdr = v;
    if ((addr & 0x40FE00u) == 0x002200u) {
        c->cycles += 2;
        write_sa1_reg(m, 0x2200u | (addr & 0x1FFu), v);
    } else if (is_rom_region(addr)) {
        c->cycles += 2;
    } else if (is_bwram(addr)) {
        c->cycles += 4;
        if ((addr & 0x600000u) == 0x600000u) {
            bitmap_write(m, addr, v);
        } else if (addr & 0x400000u) {
            if (!bwram_protected(s, addr)) bwram_write(m, addr, v);
        } else if (!s->sw46) {
            const uint32_t off = (uint32_t)(s->cbm & 0x1Fu) * 0x2000u + (addr & 0x1FFFu);
            if (!bwram_protected(s, off)) bwram_write(m, off, v);
        } else {
            bitmap_write(m, (uint32_t)s->cbm * 0x2000u + (addr & 0x1FFFu), v);
        }
    } else if (is_iram(addr)) {
        c->cycles += 2;
        if (iram_writable(s->ciwp, addr)) s->iram[addr & 0x7FFu] = v;
    } else {
        c->cycles += 2;
    }
}

void snes_sa1_bus_idle(SnesCpu *c) { c->cycles += 2; }

void snes_sa1_idle_jump(SnesCpu *c)
{
    if (is_rom_region(snes_cpu_pc24(c))) c->cycles += 2;
}

/* ------------------------------------------------------------------ */
/* Power and the run loop                                              */
/* ------------------------------------------------------------------ */

void snes_sa1_power(SnesMachine *m)
{
    SnesSa1 *s = &m->sa1;
    if (!s->present) return;
    memset(s, 0, sizeof *s);
    s->present = true;
    SnesCpu *c = &s->cpu;
    c->user = m;
    c->id = 1;
    c->s = 0x01FF;
    c->flag_e = 1;
    c->flag_m = c->flag_x = 1;
    c->flag_i = c->last_i = 1;
    s->sa1_resb = true; /* held in reset until the S-CPU writes CCNT */
    s->bwp = 0x0F;
    s->vb = 16;
    s->timer_next = UINT64_MAX;
    for (unsigned i = 0; i < 4; i++) s->mmc_bank[i] = (uint8_t)i;
    map_rom(m);
}

/* Asleep (reset, RDYB, STP or WAI): skip to `target`, or to the timer. */
static void sleep_until(SnesSa1 *s, uint64_t target)
{
    uint64_t until = s->timer_next < target ? s->timer_next : target;
    if (until <= s->cpu.cycles) until = s->cpu.cycles + 2u;
    s->cpu.cycles = (until + 1u) & ~(uint64_t)1u; /* SA-1 cycles are 2 clocks */
}

void snes_sa1_run(SnesMachine *m, uint64_t target)
{
    SnesSa1 *s = &m->sa1;
    SnesCpu *c = &s->cpu;
    if (!s->present || s->running) return;
    s->running = true;
    while (c->cycles < target) {
        timer_catch_up(s, c->cycles);
        if (c->wai) {
            if (!poll_interrupts(s)) {
                sleep_until(s, target);
                continue;
            }
            c->wai = 0;
            s->int_pending = true;
            c->cycles += 2; /* WAI's last cycle */
        }
        if (c->stp || s->sa1_rdyb || s->sa1_resb) {
            sleep_until(s, target);
            continue;
        }
        if (s->int_pending || poll_interrupts(s)) {
            s->int_pending = false;
            take_interrupt(c, s->vector);
            if (m->coverage) snes_coverage_sa1_branched(m);
            continue;
        }

        const uint32_t pc = snes_cpu_pc24(c);
        const uint32_t key = SNES_RC_KEY(pc, snes_cpu_mode(c));
        /* Only ROM code is shared with the S-CPU's program: the SA-1's low
         * memory is I-RAM, not WRAM. */
        SnesRcFunc native = (m->rc_program && is_rom_region(pc)) ? snes_rc_lookup(m, key) : NULL;
        if (native) {
            /* Return at the target or the timer, and after every instruction
             * while an IRQ waits for the I flag to clear. */
            const uint64_t until = s->timer_next < target ? s->timer_next : target;
            c->next_sync = irq_asserted(s) ? 0 : until;
            c->branched = 0;
            native(c, key);
            c->branched = 0;
            if (m->coverage) snes_coverage_sa1_entry(m, SNES_RC_KEY(snes_cpu_pc24(c), snes_cpu_mode(c)));
        } else if (m->coverage) {
            snes_coverage_sa1_note(m, key);
            c->branched = 0;
            snes_cpu_step(c);
            if (c->branched) snes_coverage_sa1_branched(m);
            c->branched = 0;
        } else {
            snes_cpu_step(c);
        }
    }
    s->running = false;
}
