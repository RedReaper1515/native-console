/*
 * The CPU's bus: the memory map, the 2A03's own registers, controllers,
 * OAM DMA, and the functions the PPU and APU reach the cartridge and the
 * CPU's memory through.
 *
 *   $0000-$1FFF  2 KiB of work RAM, mirrored
 *   $2000-$3FFF  the PPU's eight registers, mirrored
 *   $4000-$4017  APU and I/O; $4018-$401F unused (open bus)
 *   $4020-$FFFF  the cartridge: PRG RAM at $6000, PRG ROM from $8000
 *
 * Each call is one CPU cycle. RAM and ROM, where nearly every cycle goes,
 * are plain array accesses; a device is caught up to the CPU only when one
 * of its registers is touched.
 */
#include "nes_bus.h"

#include "nes.h"
#include "nes_internal.h"

static uint8_t ppu_read(NesMachine *m, uint16_t addr)
{
    nes_sync_ppu(m, m->cpu.cycles * NES_DOTS_PER_CYCLE - (NES_DOTS_PER_CYCLE - NES_PPU_READ_DOTS));
    const uint8_t cart_irq = m->cart.irq;
    uint8_t value = nes_ppu_read(&m->ppu, addr & 7u);
    nes_ppu_outputs(m);
    /* A $2007 access moves the PPU's address lines, which a cartridge may
     * be counting (MMC3: A12). */
    if (m->cart.irq != cart_irq) nes_update_irq(m, m->cpu.cycles);
    /* Reading $2002 as the flag rises drops the NMI line again before the
     * CPU's edge detector saw it. */
    if (!m->nmi_line && m->nmi_pending && m->nmi_at + NES_NMI_SETUP_DOTS > (m->cpu.cycles - 1u) * NES_DOTS_PER_CYCLE) {
        m->nmi_pending = false;
    }
    if ((addr & 7u) == 2u) {
        /* $2002 latches the VBlank flag when the read begins, but the
         * sprite 0 hit and overflow flags are not latched: the CPU gets
         * what they are when the read ends, two dots later. All three are
         * cleared at dot 1 of the pre-render line, and a read there can
         * return VBlank alone still set (AccuracyCoin, "$2002 flag
         * timing"). */
        nes_sync_ppu(m, m->cpu.cycles * NES_DOTS_PER_CYCLE);
        const uint8_t sprite_flags = NES_PPU_STATUS_OVERFLOW | NES_PPU_STATUS_SPRITE0;
        value = (uint8_t)((value & ~sprite_flags) | (m->ppu.status & sprite_flags));
        m->ppu.io_latch = (uint8_t)((m->ppu.io_latch & ~sprite_flags) | (m->ppu.status & sprite_flags));
    }
    nes_schedule(m);
    return value;
}

static void ppu_write(NesMachine *m, uint16_t addr, uint8_t value)
{
    /* PPUMASK reaches the rendering logic a dot after the other registers
     * take effect (the odd-frame skip sees a late enable one dot later). */
    const unsigned dots = (addr & 7u) == 1u ? NES_PPU_MASK_WRITE_DOTS : NES_PPU_WRITE_DOTS;
    nes_sync_ppu(m, m->cpu.cycles * NES_DOTS_PER_CYCLE - (NES_DOTS_PER_CYCLE - dots));
    const uint8_t cart_irq = m->cart.irq;
    nes_ppu_write(&m->ppu, addr & 7u, value);
    nes_ppu_outputs(m);
    /* So do $2006 and $2007 writes. */
    if (m->cart.irq != cart_irq) nes_update_irq(m, m->cpu.cycles);
    if (!m->nmi_line && m->nmi_pending && m->nmi_at + NES_NMI_SETUP_DOTS > (m->cpu.cycles - 1u) * NES_DOTS_PER_CYCLE) {
        m->nmi_pending = false;
    }
    nes_schedule(m);
}

/* $4016 bit 0 is a latch inside the 2A03; the OUT0 pin, the controllers'
 * strobe, copies it once per APU cycle, when a get cycle gives way to a
 * put cycle. A 1 that is written in a put cycle and overwritten in the get
 * cycle after it (the two writes of DEC $4016) never reaches the pin;
 * written in a get cycle it does, for two cycles (AccuracyCoin,
 * "Controller Strobing" tests 3 and 4, which line the writes up with OAM
 * DMA). The pin is brought up to date when something looks: `now` is the
 * cycle of the access. */
static void strobe_update(NesMachine *m, uint64_t now)
{
    if (now < m->strobe_at) return;
    m->strobe_at = UINT64_MAX;
    m->strobe = m->strobe_latch;
    if (m->strobe) {
        m->shift[0] = m->buttons[0];
        m->shift[1] = m->buttons[1];
    }
}

/* A controller's data line: the shift register's low bit. While the strobe
 * is high the register keeps reloading, so every read returns the A button
 * and nothing shifts; after eight reads an official controller returns
 * ones. */
static uint8_t controller_read(NesMachine *m, int port)
{
    strobe_update(m, m->cpu.cycles);
    if (m->strobe) {
        m->shift[port] = m->buttons[port];
        return (uint8_t)(m->shift[port] & 1u);
    }
    const uint8_t bit = (uint8_t)(m->shift[port] & 1u);
    m->shift[port] = (uint8_t)((m->shift[port] >> 1) | 0x80u);
    return bit;
}

uint8_t nes_io_read(NesMachine *m, uint16_t addr, uint8_t mdr)
{
    switch (addr) {
    case 0x4015: {
        nes_sync_apu(m, m->cpu.cycles);
        /* Bit 5 is not driven. */
        const uint8_t value = (uint8_t)((nes_apu_read(&m->apu, addr) & ~0x20u) | (mdr & 0x20u));
        nes_update_irq(m, m->cpu.cycles);
        nes_schedule(m);
        return value;
    }
    /* Only the low bits are driven; the rest is open bus. */
    case 0x4016: return (uint8_t)((mdr & 0xE0u) | controller_read(m, 0));
    case 0x4017: return (uint8_t)((mdr & 0xE0u) | controller_read(m, 1));
    default: return mdr;
    }
}

void nes_io_write(NesMachine *m, uint16_t addr, uint8_t value)
{
    if (addr == 0x4014u) {
        /* OAM DMA starts at the CPU's next read (see dma() below). */
        m->oam_pending = true;
        m->oam_page = value;
        m->dma_at = 0;
    } else if (addr == 0x4016u) {
        /* The pin takes the value at the start of the next put cycle (even
         * cycle numbers): the cycle after a get write, two after a put
         * write. A change already due is applied first. */
        const uint64_t now = m->cpu.cycles;
        strobe_update(m, now);
        m->strobe_latch = (value & 1u) != 0;
        m->strobe_at = now + 2u - (now & 1u);
    } else if (addr <= 0x4017u) {
        nes_sync_apu(m, m->cpu.cycles);
        nes_apu_write(&m->apu, addr, value);
        nes_update_irq(m, m->cpu.cycles);
        nes_schedule(m);
    }
}

/* One read cycle. */
static inline uint8_t read_cycle(NesCpu *c, uint16_t addr)
{
    NesMachine *m = NES_MACHINE(c);
    c->cycles++;
    if (addr >= 0x8000u) return c->mdr = m->prg[m->cart.prg_page[(addr >> 13) & 3u] + (addr & 0x1FFFu)];
    if (addr < 0x2000u) return c->mdr = m->ram[addr & 0x7FFu];
    if (addr < 0x4000u) return c->mdr = ppu_read(m, addr);
    /* $4015 is driven inside the 2A03 and does not reach the data bus
     * latch. */
    if (addr == 0x4015u) return nes_io_read(m, addr, c->mdr);
    if (addr < 0x4020u) return c->mdr = nes_io_read(m, addr, c->mdr);
    return c->mdr = nes_cart_read(m, addr, c->mdr);
}

/* ------------------------------------------------------------------ */
/* DMA                                                                 */
/* ------------------------------------------------------------------ */

/*
 * OAM DMA ($4014) and the DMC's sample fetch stop the CPU and use its bus.
 * The CPU can only be stopped on a read cycle (a write goes through and the
 * halt waits), so DMA begins inside nes_bus_read(): the read the CPU wanted
 * is made and thrown away (the halt cycle), the DMA's own cycles follow,
 * and then the CPU's read happens for real. The 2A03 alternates "get" and
 * "put" cycles (odd and even values of c->cycles after the cycle, the
 * APU's convention in nes_apu.h); DMA reads only on get cycles and OAM DMA
 * writes on put cycles, which is where the lengths come from:
 *
 *   OAM DMA   halt, an alignment cycle if the next cycle is a put, then
 *             256 get/put pairs: 513 or 514 cycles.
 *   DMC DMA   halt, a dummy cycle, an alignment cycle if the next cycle is
 *             a put, the read: 4 cycles when the halt fell on a put cycle
 *             (a reload, unless writes held the halt off by an odd number
 *             of cycles), 3 when on a get cycle (a load).
 *   both      the DMC's halt and dummy cycles pass unseen inside OAM DMA;
 *             its read takes a get cycle from OAM DMA, which then needs a
 *             put cycle to realign: 2 extra cycles, or 1 or 3 at the very
 *             end of the page.
 *
 * During the halt, dummy and alignment cycles the CPU's address stays on
 * the bus and the read is repeated, with its side effects: a read of $2007
 * is seen two or three extra times. The controller ports are the
 * exception: their read strobe stays asserted across consecutive cycles,
 * so the shift register sees one extra read whatever the DMA's length
 * (read_joy3 and dmc_dma_during_read4/dma_4016_read: one bit lost).
 *
 * The loop is ported from ares (ISC license, Near et al.), CPU::dma() in
 * ares/fc/cpu/timing.cpp at commit a776c509; see THIRD_PARTY.md.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 *
 * The APU is not stepped with the CPU: nes_schedule() knows the cycle in
 * which the DMC will ask for its next byte (m->dmc_at), and the APU is
 * caught up to that step only once the CPU's cycle count gets there.
 */

/* Whether the DMC has asked for a byte by the cycle about to be made. The
 * request is made in the APU step that follows the previous cycle's
 * access. */
static bool dmc_requested(NesMachine *m)
{
    if (!m->dmc_pending && m->cpu.cycles >= m->dmc_at) {
        nes_sync_apu(m, m->cpu.cycles + 1u);
        nes_schedule(m);
    }
    return m->dmc_pending;
}

/* A cycle in which the halted CPU's read is repeated. */
static void repeat_cycle(NesCpu *c, uint16_t addr, bool strobe_held)
{
    if (strobe_held) c->cycles++;
    else (void)read_cycle(c, addr);
}

static void dma(NesMachine *m, uint16_t addr)
{
    NesCpu *c = &m->cpu;
    if (!dmc_requested(m) && !m->oam_pending) return;

    const uint64_t start = c->cycles;
    const bool strobe_held = addr == 0x4016u || addr == 0x4017u;
    c->stalled = 1;
    bool with_dmc = false, oam_ready = false;
    unsigned oam_index = 0;
    uint8_t oam_value = 0;
    m->in_dma = true;
    (void)read_cycle(c, addr); /* the halt cycle */
    for (;;) {
        const bool dmc = dmc_requested(m);
        if (!dmc && !m->oam_pending) break;
        with_dmc = with_dmc || dmc;
        if (c->cycles & 1u) {
            /* A put cycle. */
            if (m->oam_pending && oam_ready) {
                nes_bus_write(c, 0x2004u, oam_value);
                oam_ready = false;
                if (++oam_index == 256u) m->oam_pending = false;
            } else {
                /* Alignment, for either DMA. */
                m->dmc_dummy = true;
                repeat_cycle(c, addr, strobe_held);
            }
        } else if (dmc && m->dmc_dummy) {
            /* A get cycle: the DMC's read. The APU has had the byte since
             * it asked; the flag the last byte of a sample raises shows
             * from here. */
            (void)read_cycle(c, m->dmc_addr);
            m->dmc_pending = false;
            nes_update_irq(m, c->cycles + 1u);
            nes_schedule(m);
        } else if (m->oam_pending) {
            if (dmc) m->dmc_dummy = true;
            oam_value = read_cycle(c, (uint16_t)((m->oam_page << 8) | oam_index));
            oam_ready = true;
        } else {
            /* The DMC's dummy cycle. */
            m->dmc_dummy = true;
            repeat_cycle(c, addr, strobe_held);
        }
    }
    m->in_dma = false;
    if (with_dmc) m->dmc_stolen += c->cycles - start;
    nes_schedule(m);
}

uint8_t nes_bus_read(NesCpu *c, uint16_t addr)
{
    NesMachine *m = NES_MACHINE(c);
    if (c->cycles >= m->dma_at) dma(m, addr);
    return read_cycle(c, addr);
}

void nes_bus_write(NesCpu *c, uint16_t addr, uint8_t value)
{
    NesMachine *m = NES_MACHINE(c);
    c->cycles++;
    c->mdr = value;
    if (addr < 0x2000u) {
        m->ram[addr & 0x7FFu] = value;
    } else if (addr < 0x4000u) {
        ppu_write(m, addr, value);
    } else if (addr < 0x4020u) {
        nes_io_write(m, addr, value);
    } else if (addr < 0x8000u && m->info.mapper != 34) {
        /* PRG RAM (the one board with registers down here is NINA-001). */
        nes_cart_write(m, addr, value);
    } else {
        /* A mapper register: the PPU must have drawn everything up to now
         * with the old banks, and recompiled code must look its place up
         * again if PRG moved. */
        const uint64_t gen = m->cart.prg_gen;
        const uint8_t irq = m->cart.irq;
        nes_sync_ppu(m, c->cycles * NES_DOTS_PER_CYCLE);
        nes_cart_write(m, addr, value);
        if (m->cart.irq != irq) nes_update_irq(m, c->cycles);
        if (m->cart.prg_gen != gen) m->attention |= NES_ATTENTION_REMAP;
        nes_schedule(m);
    }
}

/* BRK and IRQ choose their vector late: an NMI that has arrived by then
 * takes the sequence over. "Arrived" is what it is for an instruction's
 * poll: the edge detector needs the line's setup time before the end of
 * the cycle (cpu_interrupts_v2/2-nmi_and_brk: an NMI takes a BRK over for
 * five clocks, the cycle before the BRK and its first four). */
uint16_t nes_bus_interrupt_vector(NesCpu *c, uint16_t vector)
{
    NesMachine *m = NES_MACHINE(c);
    if (vector == 0xFFFAu) return vector;
    nes_sync(m);
    if (m->nmi_pending && m->nmi_at + NES_NMI_SETUP_DOTS <= c->cycles * NES_DOTS_PER_CYCLE) {
        m->nmi_pending = false;
        nes_update_attention(m);
        return 0xFFFAu;
    }
    return vector;
}

uint8_t nes_peek(NesMachine *m, uint16_t addr)
{
    if (addr < 0x2000u) return m->ram[addr & 0x7FFu];
    if (addr >= 0x4020u) return nes_cart_read(m, addr, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The PPU's bus and the DMC's sample fetch                            */
/* ------------------------------------------------------------------ */

/* The dot the PPU is on, as a count that only moves forward: the cartridge
 * times the A12 line with it. The PPU counts a frame when its picture is
 * complete (scanline 240), 22 lines before the scanline counter wraps, so
 * the lines from there on still belong to the frame before. (Counting them
 * to the new one made the count jump back at scanline 0, and the MMC3 saw
 * an A12 rise there after an "endless" low: one clock too many per frame
 * with the background at $1000.) */
static uint64_t ppu_now(const NesMachine *m, const NesPpu *p)
{
    (void)m;
    const uint64_t frame = p->frame_count - (p->scanline >= NES_PPU_HEIGHT ? 1u : 0u);
    return frame * NES_PPU_NTSC_FRAME_DOTS + (uint64_t)p->scanline * NES_PPU_DOTS_PER_LINE + p->dot;
}

uint8_t nes_ppu_bus_read(NesPpu *p, uint16_t addr)
{
    NesMachine *m = (NesMachine *)p->user;
    return nes_cart_ppu_read(m, addr, ppu_now(m, p));
}

void nes_ppu_bus_write(NesPpu *p, uint16_t addr, uint8_t value)
{
    NesMachine *m = (NesMachine *)p->user;
    nes_cart_ppu_write(m, addr, value, ppu_now(m, p));
}

void nes_ppu_bus_address(NesPpu *p, uint16_t addr)
{
    NesMachine *m = (NesMachine *)p->user;
    nes_cart_ppu_address(m, addr, ppu_now(m, p));
}

/* The DMC asks for its next sample byte. The APU gets the byte at once:
 * the CPU can only write before the DMA's read cycle, so nothing short of
 * a bank switch in those cycles could change it. The DMA's cycles are
 * spent at the CPU's next read. */
uint8_t nes_apu_dmc_read(NesApu *a, uint16_t addr)
{
    NesMachine *m = (NesMachine *)a->user;
    m->dmc_pending = true;
    m->dmc_dummy = false;
    m->dmc_irq_before = a->dmc.irq != 0;
    m->dmc_addr = addr;
    m->dmc_at = m->dma_at = 0;
    return nes_peek(m, addr);
}
