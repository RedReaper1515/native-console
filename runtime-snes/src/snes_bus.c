/*
 * The S-CPU bus: access timing, the memory map, open bus, and the B-bus
 * ($2100-$21FF: PPU, APU ports, WRAM port).
 *
 * Access speed (master clocks per bus cycle) depends on the address:
 * ROM and banks 40-7F/C0-FF take 8 (6 in banks 80-FF with MEMSEL = 1,
 * "FastROM"), WRAM and $6000-$7FFF 8, most I/O 6, and the controller
 * serial ports at $4000-$41FF 12. Internal cycles take 6.
 *
 * Open bus: a read of an unmapped address returns the last value on the
 * data bus (c->mdr). Reads of the S-CPU's own registers ($4000-$43FF) do
 * not drive the external bus, so they do not change it.
 */
#include "snes.h"
#include "snes_bus.h"

static inline unsigned access_speed(const SnesMachine *m, uint32_t addr)
{
    if (addr & 0x408000u) return (addr & 0x800000u) ? ((m->memsel & 1u) ? 6u : 8u) : 8u;
    if ((addr + 0x6000u) & 0x4000u) return 8u;
    if ((addr - 0x4000u) & 0x7E00u) return 6u;
    return 12u;
}

uint8_t snes_bbus_read(SnesMachine *m, uint8_t reg, uint8_t mdr)
{
    if (reg < 0x40) {
        if (reg >= 0x34) {
            m->ppu.hcounter = snes_hcounter(m);
            m->ppu.vcounter = m->vcounter;
            m->ppu.vblank = m->vcounter >= m->vdisp;
            if (reg == 0x37) {
                /* SLHV: reading latches the counters (when WRIO bit 7 is set). */
                if (m->wrio & 0x80u) snes_ppu_latch_counters(&m->ppu);
                return mdr;
            }
            return snes_ppu_read(&m->ppu, reg, mdr);
        }
        return mdr; /* write-only PPU registers */
    }
    if (reg < 0x80) {
        snes_sync_apu(m);
        uint8_t v = snes_apu_cpu_read(&m->apu, reg & 3u);
        if (m->apu_port_trace) m->apu_port_trace(m, reg & 3u, v, false);
        return v;
    }
    if (reg <= 0x83) return snes_io_read(m, 0x2100u | reg);
    return mdr;
}

void snes_bbus_write(SnesMachine *m, uint8_t reg, uint8_t v)
{
    if (reg < 0x40) {
        m->ppu.hcounter = snes_hcounter(m);
        m->ppu.vcounter = m->vcounter;
        m->ppu.vblank = m->vcounter >= m->vdisp;
        snes_ppu_write(&m->ppu, reg, v);
        return;
    }
    if (reg < 0x80) {
        snes_sync_apu(m);
        if (m->apu_port_trace) m->apu_port_trace(m, reg & 3u, v, true);
        snes_apu_cpu_write(&m->apu, reg & 3u, v);
        return;
    }
    if (reg <= 0x83) snes_io_write(m, 0x2100u | reg, v);
}

/* The memory map without timing. */
uint8_t snes_bus_read_raw(SnesMachine *m, uint32_t addr, uint8_t mdr)
{
    uint32_t page = addr >> 12;
    switch (m->page_type[page]) {
    case SNES_PAGE_ROM: return m->rom[m->page_offset[page] + (addr & 0xFFFu)];
    case SNES_PAGE_WRAM: return m->wram[addr & 0x1FFFFu];
    case SNES_PAGE_SRAM: return m->sram[(m->page_offset[page] + (addr & 0xFFFu)) & m->page_mask[page]];
    case SNES_PAGE_SYSTEM: {
        uint32_t a = addr & 0xFFFFu;
        if (a < 0x2000u) return m->wram[a];
        if (a < 0x2100u) return mdr;
        if (a < 0x2200u) return snes_bbus_read(m, (uint8_t)a, mdr);
        if (a < 0x4000u) return m->sa1.present ? snes_sa1_cpu_read(m, addr, mdr) : mdr; /* SA-1 registers, I-RAM */
        if (a == 0x4016u || a == 0x4017u || (a >= 0x4200u && a < 0x4220u)) return snes_io_read(m, a);
        if (a >= 0x4300u && a < 0x4380u) return snes_dma_read(m, a);
        return mdr;
    }
    case SNES_PAGE_COPROC: return snes_cart_read(m, addr, mdr);
    default: return mdr;
    }
}

void snes_bus_write_raw(SnesMachine *m, uint32_t addr, uint8_t v)
{
    uint32_t page = addr >> 12;
    switch (m->page_type[page]) {
    case SNES_PAGE_WRAM:
        m->wram[addr & 0x1FFFFu] = v;
        snes_wram_written(m, addr & 0x1FFFFu);
        return;
    case SNES_PAGE_SRAM: snes_cart_write(m, addr, v); return;
    case SNES_PAGE_SYSTEM: {
        uint32_t a = addr & 0xFFFFu;
        if (a < 0x2000u) {
            m->wram[a] = v;
            snes_wram_written(m, a);
            return;
        }
        if (a >= 0x2100u && a < 0x2200u) {
            snes_bbus_write(m, (uint8_t)a, v);
            return;
        }
        if (a >= 0x2200u && a < 0x4000u) {
            if (m->sa1.present) snes_sa1_cpu_write(m, addr, v);
            return;
        }
        if (a == 0x4016u || (a >= 0x4200u && a < 0x4220u)) {
            snes_io_write(m, a, v);
            return;
        }
        if (a >= 0x4300u && a < 0x4380u) snes_dma_write(m, a, v);
        return;
    }
    case SNES_PAGE_COPROC: snes_cart_write(m, addr, v); return;
    default: return;
    }
}

/* Does an access to this address touch hardware whose state depends on
 * time (so due events must run first)? */
static inline bool is_io(const SnesMachine *m, uint32_t addr)
{
    uint8_t type = m->page_type[addr >> 12];
    if (type == SNES_PAGE_SYSTEM) return (addr & 0xFFFFu) >= 0x2000u;
    return type == SNES_PAGE_COPROC;
}

uint8_t snes_bus_read(SnesCpu *c, uint32_t addr)
{
    if (c->id) return snes_sa1_bus_read(c, addr);
    SnesMachine *m = SNES_MACHINE(c);
    unsigned speed = access_speed(m, addr);
    c->cycles += speed - 4u;
    uint8_t v;
    if (is_io(m, addr)) {
        snes_catch_up(m);
        v = snes_bus_read_raw(m, addr, c->mdr);
        /* The S-CPU's own registers do not drive the external data bus. */
        if ((addr & 0x40FC00u) != 0x4000u) c->mdr = v;
    } else {
        v = snes_bus_read_raw(m, addr, c->mdr);
        c->mdr = v;
    }
    c->cycles += 4u;
    m->alu_edges++;
    return v;
}

void snes_bus_write(SnesCpu *c, uint32_t addr, uint8_t value)
{
    if (c->id) {
        snes_sa1_bus_write(c, addr, value);
        return;
    }
    SnesMachine *m = SNES_MACHINE(c);
    c->cycles += access_speed(m, addr);
    c->mdr = value;
    m->alu_edges++; /* a write cycle steps the multiplier before the store */
    if (is_io(m, addr)) snes_catch_up(m);
    snes_bus_write_raw(m, addr, value);
}

void snes_bus_idle(SnesCpu *c)
{
    if (c->id) {
        snes_sa1_bus_idle(c);
        return;
    }
    SnesMachine *m = SNES_MACHINE(c);
    c->cycles += 6u;
    m->alu_edges++;
}

void snes_bus_idle_jump(SnesCpu *c) { snes_sa1_idle_jump(c); }

uint8_t snes_peek(SnesMachine *m, uint32_t addr)
{
    addr &= 0xFFFFFFu;
    uint32_t page = addr >> 12;
    switch (m->page_type[page]) {
    case SNES_PAGE_ROM:
    case SNES_PAGE_WRAM:
    case SNES_PAGE_SRAM: return snes_bus_read_raw(m, addr, 0);
    case SNES_PAGE_SYSTEM:
        if ((addr & 0xFFFFu) < 0x2000u) return m->wram[addr & 0x1FFFu];
        return m->sa1.present ? snes_sa1_peek(m, addr) : 0;
    case SNES_PAGE_COPROC: return m->sa1.present ? snes_sa1_peek(m, addr) : 0;
    default: return 0;
    }
}
