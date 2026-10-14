/*
 * DMA and HDMA. Channel logic follows ares (ISC license, Near et al.).
 *
 * DMA is hardware, not a memory copy: it moves bytes between the A-bus
 * (the CPU address space) and the B-bus ($2100-$21FF) in a per-mode
 * register pattern, can hold the A address fixed or count it down, and
 * pauses the CPU (8 master clocks per byte). Channels run in order 0-7.
 * HDMA reloads its tables at the start of each frame and transfers one
 * table entry per visible line, in H-blank, stopping a GP-DMA that is in
 * progress on that channel.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#include "snes.h"

/* B-bus register offsets per transfer mode, for byte index 0-3. */
static const uint8_t mode_offsets[8][4] = {
    { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 1, 1 },
    { 0, 1, 2, 3 }, { 0, 1, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 1, 1 },
};
static const uint8_t hdma_lengths[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };

uint8_t snes_dma_read(SnesMachine *m, uint32_t addr)
{
    const SnesDmaChannel *ch = &m->dma[(addr >> 4) & 7u];
    switch (addr & 0xFF8Fu) {
    case 0x4300: return ch->control;
    case 0x4301: return ch->b_addr;
    case 0x4302: return (uint8_t)ch->a_addr;
    case 0x4303: return (uint8_t)(ch->a_addr >> 8);
    case 0x4304: return ch->a_bank;
    case 0x4305: return (uint8_t)ch->size;
    case 0x4306: return (uint8_t)(ch->size >> 8);
    case 0x4307: return ch->ind_bank;
    case 0x4308: return (uint8_t)ch->hdma_addr;
    case 0x4309: return (uint8_t)(ch->hdma_addr >> 8);
    case 0x430A: return ch->line_counter;
    case 0x430B:
    case 0x430F: return ch->unused_b;
    default: return m->cpu.mdr;
    }
}

void snes_dma_write(SnesMachine *m, uint32_t addr, uint8_t v)
{
    SnesDmaChannel *ch = &m->dma[(addr >> 4) & 7u];
    switch (addr & 0xFF8Fu) {
    case 0x4300: ch->control = v; break;
    case 0x4301: ch->b_addr = v; break;
    case 0x4302: ch->a_addr = (uint16_t)((ch->a_addr & 0xFF00u) | v); break;
    case 0x4303: ch->a_addr = (uint16_t)((ch->a_addr & 0x00FFu) | (v << 8)); break;
    case 0x4304: ch->a_bank = v; break;
    case 0x4305: ch->size = (uint16_t)((ch->size & 0xFF00u) | v); break;
    case 0x4306: ch->size = (uint16_t)((ch->size & 0x00FFu) | (v << 8)); break;
    case 0x4307: ch->ind_bank = v; break;
    case 0x4308: ch->hdma_addr = (uint16_t)((ch->hdma_addr & 0xFF00u) | v); break;
    case 0x4309: ch->hdma_addr = (uint16_t)((ch->hdma_addr & 0x00FFu) | (v << 8)); break;
    case 0x430A: ch->line_counter = v; break;
    case 0x430B:
    case 0x430F: ch->unused_b = v; break;
    default: break;
    }
}

/* The A-bus cannot reach the B-bus or the S-CPU's own registers. */
static bool valid_a(uint32_t addr)
{
    if ((addr & 0x40FF00u) == 0x2100u) return false;
    if ((addr & 0x40FE00u) == 0x4000u) return false;
    if ((addr & 0x40FFE0u) == 0x4200u) return false;
    if ((addr & 0x40FF80u) == 0x4300u) return false;
    return true;
}

static void dma_step(SnesMachine *m, unsigned clocks)
{
    m->cpu.cycles += clocks;
    snes_catch_up(m);
}

/* One byte between A-bus address `a` and B-bus register `b`. */
static void transfer(SnesMachine *m, const SnesDmaChannel *ch, uint32_t a, uint8_t b)
{
    /* WRAM to WRAM through $2180 does not work. */
    bool valid = b != 0x80 || ((a & 0xFE0000u) != 0x7E0000u && (a & 0x40E000u) != 0x0000u);
    dma_step(m, 4);
    if (!(ch->control & 0x80u)) {
        uint8_t v = valid_a(a) ? snes_bus_read_raw(m, a, m->cpu.mdr) : 0;
        m->cpu.mdr = v;
        dma_step(m, 4);
        if (valid) snes_bbus_write(m, b, v);
    } else {
        uint8_t v = valid ? snes_bbus_read(m, b, m->cpu.mdr) : 0;
        m->cpu.mdr = v;
        dma_step(m, 4);
        if (valid_a(a)) snes_bus_write_raw(m, a, v);
    }
}

void snes_dma_run(SnesMachine *m)
{
    if (m->in_dma) return;
    m->in_dma = true;
    dma_step(m, 12); /* align to the DMA clock and start */
    for (int n = 0; n < 8; n++) {
        SnesDmaChannel *ch = &m->dma[n];
        if (!ch->dma_enable) continue;
        dma_step(m, 8);
        unsigned index = 0;
        do {
            uint8_t b = (uint8_t)(ch->b_addr + mode_offsets[ch->control & 7u][index & 3u]);
            transfer(m, ch, ((uint32_t)ch->a_bank << 16) | ch->a_addr, b);
            index++;
            if (!(ch->control & 0x08u)) ch->a_addr = (uint16_t)((ch->control & 0x10u) ? ch->a_addr - 1u : ch->a_addr + 1u);
            ch->size = (uint16_t)(ch->size - 1u);
        } while (ch->dma_enable && ch->size != 0);
        ch->dma_enable = false;
    }
    dma_step(m, 8);
    m->in_dma = false;
}

static bool hdma_active(const SnesDmaChannel *ch) { return ch->hdma_enable && !ch->hdma_completed; }

static bool hdma_finished_after(const SnesMachine *m, int n)
{
    for (int k = n + 1; k < 8; k++) {
        if (hdma_active(&m->dma[k])) return false;
    }
    return true;
}

static uint8_t hdma_read_a(SnesMachine *m, uint32_t a)
{
    dma_step(m, 4);
    uint8_t v = valid_a(a) ? snes_bus_read_raw(m, a, m->cpu.mdr) : 0;
    m->cpu.mdr = v;
    dma_step(m, 4);
    return v;
}

static void hdma_reload(SnesMachine *m, int n)
{
    SnesDmaChannel *ch = &m->dma[n];
    uint8_t data = hdma_read_a(m, ((uint32_t)ch->a_bank << 16) | ch->hdma_addr);
    if ((ch->line_counter & 0x7Fu) == 0) {
        ch->line_counter = data;
        ch->hdma_addr = (uint16_t)(ch->hdma_addr + 1u);
        ch->hdma_completed = ch->line_counter == 0;
        ch->hdma_do_transfer = !ch->hdma_completed;
        if (ch->control & 0x40u) {
            data = hdma_read_a(m, ((uint32_t)ch->a_bank << 16) | ch->hdma_addr);
            ch->hdma_addr = (uint16_t)(ch->hdma_addr + 1u);
            ch->size = (uint16_t)(data << 8);
            if (ch->hdma_completed && hdma_finished_after(m, n)) return;
            data = hdma_read_a(m, ((uint32_t)ch->a_bank << 16) | ch->hdma_addr);
            ch->hdma_addr = (uint16_t)(ch->hdma_addr + 1u);
            ch->size = (uint16_t)((data << 8) | (ch->size >> 8));
        }
    }
}

void snes_hdma_init(SnesMachine *m)
{
    bool any = false;
    for (int n = 0; n < 8; n++) {
        m->dma[n].hdma_completed = false;
        m->dma[n].hdma_do_transfer = false;
        any |= m->dma[n].hdma_enable;
    }
    if (!any) return;
    dma_step(m, 8);
    for (int n = 0; n < 8; n++) {
        SnesDmaChannel *ch = &m->dma[n];
        ch->hdma_do_transfer = true;
        if (!ch->hdma_enable) continue;
        ch->dma_enable = false; /* HDMA stops a DMA in progress */
        ch->hdma_addr = ch->a_addr;
        ch->line_counter = 0;
        hdma_reload(m, n);
    }
}

void snes_hdma_line(SnesMachine *m)
{
    bool any = false;
    for (int n = 0; n < 8; n++) any |= hdma_active(&m->dma[n]);
    if (!any) return;
    dma_step(m, 8);
    for (int n = 0; n < 8; n++) {
        SnesDmaChannel *ch = &m->dma[n];
        if (!hdma_active(ch)) continue;
        ch->dma_enable = false;
        if (!ch->hdma_do_transfer) continue;
        for (unsigned index = 0; index < hdma_lengths[ch->control & 7u]; index++) {
            uint32_t a;
            if (!(ch->control & 0x40u)) {
                a = ((uint32_t)ch->a_bank << 16) | ch->hdma_addr;
                ch->hdma_addr = (uint16_t)(ch->hdma_addr + 1u);
            } else {
                a = ((uint32_t)ch->ind_bank << 16) | ch->size;
                ch->size = (uint16_t)(ch->size + 1u);
            }
            transfer(m, ch, a, (uint8_t)(ch->b_addr + mode_offsets[ch->control & 7u][index]));
        }
    }
    for (int n = 0; n < 8; n++) {
        SnesDmaChannel *ch = &m->dma[n];
        if (!hdma_active(ch)) continue;
        ch->line_counter = (uint8_t)(ch->line_counter - 1u);
        ch->hdma_do_transfer = (ch->line_counter & 0x80u) != 0;
        hdma_reload(m, n);
    }
}
