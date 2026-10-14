/*
 * DMA channels 0-3 of each CPU (GBATEK, "DS DMA Transfers").
 *
 * DMAxCNT is one 32-bit register: the word count in the low bits (21 on the
 * ARM9; 14, or 16 for channel 3, on the ARM7), then destination control
 * (bits 21-22), source control (23-24), repeat (25), 32-bit units (26), the
 * start mode (27-29 on the ARM9, 28-29 on the ARM7), the interrupt enable
 * (30) and the enable bit (31).
 *
 * A transfer runs to completion when it starts, going through the CPU's bus
 * so it pays the same access times (the CPU is stalled meanwhile). The
 * exceptions are the card, whose DMA moves one word each time the card has
 * one ready, and the geometry command FIFO, whose DMA sends 112 words each
 * time the FIFO is less than half full. DMA goes around the ARM9's TCMs.
 */
#include "nds.h"

/* Units one request moves. */
static uint32_t burst_of(int timing)
{
    if (timing == NDS_DMA_CARD) return 1u;
    if (timing == NDS_DMA_GXFIFO) return 112u;
    return UINT32_MAX;
}

static inline const uint8_t *regs_of(const NdsMachine *m, int cpu, int ch)
{
    return (cpu == NDS_ARM9 ? m->io9 : m->io7) + 0xB0 + 12 * ch;
}

static inline uint32_t timing_of(int cpu, uint32_t control)
{
    if (cpu == NDS_ARM9) return (control >> 27) & 7u;
    static const uint8_t arm7_modes[4] = { NDS_DMA_IMMEDIATE, NDS_DMA_VBLANK, NDS_DMA_CARD, NDS_DMA_WIFI };
    return arm7_modes[(control >> 28) & 3u];
}

static uint32_t count_of(int cpu, int ch, uint32_t control)
{
    if (cpu == NDS_ARM9) {
        uint32_t count = control & 0x1FFFFFu;
        return count ? count : 0x200000u;
    }
    uint32_t mask = ch == 3 ? 0xFFFFu : 0x3FFFu;
    uint32_t count = control & mask;
    return count ? count : mask + 1u;
}

static inline uint32_t dma_read(NdsMachine *m, int cpu, uint32_t addr, bool word)
{
    if (cpu == NDS_ARM9) {
        return word ? nds9_bus_read32(&m->arm9, addr, NDS_ACCESS_DMA) : nds9_bus_read16(&m->arm9, addr, NDS_ACCESS_DMA);
    }
    return word ? nds7_bus_read32(&m->arm7, addr, NDS_ACCESS_DMA) : nds7_bus_read16(&m->arm7, addr, NDS_ACCESS_DMA);
}

static inline void dma_write(NdsMachine *m, int cpu, uint32_t addr, uint32_t value, bool word)
{
    if (cpu == NDS_ARM9) {
        if (word) nds9_bus_write32(&m->arm9, addr, value, NDS_ACCESS_DMA);
        else nds9_bus_write16(&m->arm9, addr, (uint16_t)value, NDS_ACCESS_DMA);
    } else {
        if (word) nds7_bus_write32(&m->arm7, addr, value, NDS_ACCESS_DMA);
        else nds7_bus_write16(&m->arm7, addr, (uint16_t)value, NDS_ACCESS_DMA);
    }
}

/* Move up to `limit` units. When the count runs out the channel raises its
 * interrupt and either reloads (repeat, in a triggered mode) or switches
 * itself off. */
static void transfer(NdsMachine *m, int cpu, int ch, uint32_t limit)
{
    NdsDma *d = &m->dma[cpu][ch];
    const bool word = (d->control & (1u << 26)) != 0;
    const uint32_t size = word ? 4u : 2u;
    const uint32_t dst_ctrl = (d->control >> 21) & 3u;
    const uint32_t src_ctrl = (d->control >> 23) & 3u;

    uint32_t units = d->remaining < limit ? d->remaining : limit;
    d->remaining -= units;
    for (; units; units--) {
        uint32_t value = dma_read(m, cpu, d->src & ~(size - 1u), word);
        dma_write(m, cpu, d->dst & ~(size - 1u), value, word);
        if (src_ctrl == 0) d->src += size;
        else if (src_ctrl == 1) d->src -= size;
        if (dst_ctrl == 0 || dst_ctrl == 3) d->dst += size;
        else if (dst_ctrl == 1) d->dst -= size;
    }
    if (d->remaining) return;

    if (d->control & (1u << 30)) nds_raise_irq(m, cpu, NDS_IRQ_DMA0 << ch);
    if ((d->control & (1u << 25)) && timing_of(cpu, d->control) != NDS_DMA_IMMEDIATE) {
        d->remaining = count_of(cpu, ch, d->control);
        if (dst_ctrl == 3) d->dst = nds_get_le32(regs_of(m, cpu, ch) + 4);
    } else {
        d->control &= 0x7FFFFFFFu;
        /* The enable bit reads back clear. */
        uint8_t *regs = (cpu == NDS_ARM9 ? m->io9 : m->io7) + 0xB0 + 12 * ch;
        regs[11] &= 0x7Fu;
    }
}

void nds_dma_write_control(NdsMachine *m, int cpu, int ch, uint32_t value)
{
    NdsDma *d = &m->dma[cpu][ch];
    bool was_enabled = (d->control & 0x80000000u) != 0;
    d->control = value;
    if (!(value & 0x80000000u) || was_enabled) return;

    /* Rising edge: latch the addresses and the count from the registers. */
    const uint8_t *regs = regs_of(m, cpu, ch);
    uint32_t sad = nds_get_le32(regs), dad = nds_get_le32(regs + 4);
    if (cpu == NDS_ARM9) {
        sad &= 0x0FFFFFFEu;
        dad &= 0x0FFFFFFEu;
    } else {
        sad &= ch == 0 ? 0x07FFFFFEu : 0x0FFFFFFEu;
        dad &= ch == 3 ? 0x0FFFFFFEu : 0x07FFFFFEu;
    }
    d->src = sad;
    d->dst = dad;
    d->remaining = count_of(cpu, ch, value);

    if (timing_of(cpu, value) == NDS_DMA_IMMEDIATE) transfer(m, cpu, ch, UINT32_MAX);
    /* The geometry FIFO's request may already be up. */
    else if (timing_of(cpu, value) == NDS_DMA_GXFIFO) nds_video_dma_enabled(m);
}

bool nds_dma_trigger(NdsMachine *m, int cpu, int timing)
{
    bool moved = false;
    for (int ch = 0; ch < 4; ch++) {
        NdsDma *d = &m->dma[cpu][ch];
        if (!(d->control & 0x80000000u) || timing_of(cpu, d->control) != (uint32_t)timing) continue;
        transfer(m, cpu, ch, burst_of(timing));
        moved = true;
    }
    return moved;
}
