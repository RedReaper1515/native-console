/*
 * DMA channels 0-3.
 *
 * A transfer runs to completion when it starts, going through the normal
 * bus so it pays the same waitstates as the CPU would (the CPU is stalled
 * meanwhile). Immediate transfers start after a short delay; VBlank and
 * HBlank transfers are started by the video timing events.
 */
#include "gba.h"
#include "gba_bus.h"

static inline uint32_t reg32(const GbaMachine *m, uint32_t off)
{
    return (uint32_t)m->io[off] | ((uint32_t)m->io[off + 1] << 8) | ((uint32_t)m->io[off + 2] << 16) |
           ((uint32_t)m->io[off + 3] << 24);
}

/* fifo: sound FIFO mode (DMA1/2, special timing): four words to a fixed
 * destination, whatever the count and width bits say. */
static void transfer(GbaMachine *m, int ch, bool fifo)
{
    GbaDma *d = &m->dma[ch];
    const bool word = fifo || (d->control & 0x0400u) != 0;
    const uint32_t size = word ? 4u : 2u;
    const uint32_t dst_ctrl = fifo ? 2u : (d->control >> 5) & 3u;
    uint32_t src_ctrl = (d->control >> 7) & 3u;

    /* Game Pak sources always increment. */
    if (d->src >= 0x08000000u && d->src < 0x0E000000u) src_ctrl = 0;

    int prev_active = m->dma_active;
    m->dma_active = ch;
    m->cpu.cycles += 2; /* internal cycles to start */

    int access = GBA_ACCESS_NSEQ | GBA_ACCESS_DMA;
    for (uint32_t n = fifo ? 4u : d->remaining; n; n--) {
        uint32_t src = d->src & ~(size - 1u);
        uint32_t dst = d->dst & ~(size - 1u);
        if (word) {
            /* Sources below EWRAM (the BIOS) return the DMA latch. */
            if (src >= 0x02000000u) d->latch = gba_bus_read32(&m->cpu, src, access);
            else m->cpu.cycles++;
            gba_bus_write32(&m->cpu, dst, d->latch, access);
        } else {
            if (src >= 0x02000000u) {
                uint32_t half = gba_bus_read16(&m->cpu, src, access);
                d->latch = half * 0x00010001u;
            } else {
                m->cpu.cycles++;
            }
            gba_bus_write16(&m->cpu, dst, (uint16_t)(d->latch >> ((dst & 2u) * 8u)), access);
        }
        access = GBA_ACCESS_SEQ | GBA_ACCESS_DMA;

        if (src_ctrl == 0) d->src += size;
        else if (src_ctrl == 1) d->src -= size;
        if (dst_ctrl == 0 || dst_ctrl == 3) d->dst += size;
        else if (dst_ctrl == 1) d->dst -= size;
    }
    m->dma_active = prev_active;
    if (ch == 3 && gba_eeprom_address(m, d->dad)) gba_eeprom_request_done(m);

    if (d->control & 0x4000u) gba_raise_irq(m, (uint16_t)(GBA_IRQ_DMA0 << ch));

    uint32_t timing = (d->control >> 12) & 3u;
    if (fifo) {
        if (!(d->control & 0x0200u)) d->control &= 0x7FFFu;
    } else if ((d->control & 0x0200u) && timing != 0) {
        /* Repeat: reload the count, and the destination if asked. */
        uint32_t count = d->count & (ch == 3 ? 0xFFFFu : 0x3FFFu);
        d->remaining = count ? count : (ch == 3 ? 0x10000u : 0x4000u);
        if (dst_ctrl == 3) d->dst = reg32(m, 0x0B4 + 12u * (uint32_t)ch);
    } else {
        d->control &= 0x7FFFu;
    }
}

void gba_dma_write_control(GbaMachine *m, int ch, uint16_t value)
{
    GbaDma *d = &m->dma[ch];
    bool was_enabled = (d->control & 0x8000u) != 0;
    d->control = value;
    if (!(value & 0x8000u)) {
        d->pending = false;
        return;
    }
    if (was_enabled) return;

    /* Rising edge: latch addresses and count from the registers. */
    uint32_t off = 0x0B0 + 12u * (uint32_t)ch;
    d->sad = reg32(m, off) & (ch == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu);
    d->dad = reg32(m, off + 4) & (ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    d->count = (uint16_t)(m->io[off + 8] | (m->io[off + 9] << 8));
    d->src = d->sad;
    d->dst = d->dad;
    uint32_t count = d->count & (ch == 3 ? 0xFFFFu : 0x3FFFu);
    d->remaining = count ? count : (ch == 3 ? 0x10000u : 0x4000u);

    if (((value >> 12) & 3u) == 0) {
        d->pending = true;
        d->start_at = m->cpu.cycles + 2;
        gba_schedule(m, GBA_EVENT_DMA, d->start_at);
    }
}

void gba_dma_run_pending(GbaMachine *m)
{
    uint64_t next = UINT64_MAX;
    for (int ch = 0; ch < 4; ch++) {
        GbaDma *d = &m->dma[ch];
        if (!d->pending) continue;
        if (d->start_at <= m->cpu.cycles) {
            d->pending = false;
            transfer(m, ch, false);
        } else if (d->start_at < next) {
            next = d->start_at;
        }
    }
    gba_schedule(m, GBA_EVENT_DMA, next);
}

void gba_dma_trigger(GbaMachine *m, int timing)
{
    for (int ch = 0; ch < 4; ch++) {
        GbaDma *d = &m->dma[ch];
        if ((d->control & 0x8000u) && ((d->control >> 12) & 3u) == (uint32_t)timing) {
            /* Sound FIFO (special timing on DMA1/2) and video capture (DMA3)
             * are handled by the APU and PPU when they exist. */
            if (timing == 3) continue;
            transfer(m, ch, false);
        }
    }
}

/* A DirectSound FIFO dropped to half full: the DMA channel (1 or 2) set to
 * special timing with that FIFO as its destination refills it. */
void gba_dma_sound_request(GbaMachine *m, int fifo)
{
    uint32_t fifo_addr = 0x040000A0u + 4u * (uint32_t)fifo;
    for (int ch = 1; ch <= 2; ch++) {
        GbaDma *d = &m->dma[ch];
        if ((d->control & 0x8000u) && ((d->control >> 12) & 3u) == 3u && (d->dad & ~3u) == fifo_addr) {
            d->dst = fifo_addr;
            transfer(m, ch, true);
            return;
        }
    }
}
