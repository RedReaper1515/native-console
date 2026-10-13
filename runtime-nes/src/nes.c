/*
 * Machine lifecycle, the device clocks, interrupts, and the run loop.
 *
 * The PPU and APU are not stepped with the CPU. nes_sync() runs each up to
 * the CPU's time in steps that end exactly where a device could change one
 * of its outputs by itself (the PPU's NMI line and end of frame, the APU's
 * IRQ line and DMC fetches), so every edge is noted with the moment it
 * happened no matter how late the catch-up runs. The bus (nes_bus.c) does
 * the same before a register access.
 *
 * Interrupt timing: the 6502 samples its interrupt inputs during the cycle
 * before an instruction's last one (c->poll_at, with the I flag of that
 * moment in c->poll_i). An NMI edge or IRQ level that arrives later is
 * taken one instruction later. This is why CLI lets a pending IRQ in only
 * after the following instruction, and SEI lets one more through.
 */
#include "nes.h"

#include <stdlib.h>
#include <string.h>

#include "nes_bus.h"
#include "nes_internal.h"
#include "nes_recomp.h"

_Static_assert(offsetof(NesMachine, cpu) == 0, "NesCpu must be the first member of NesMachine");

NesMachine *nes_create(void)
{
    NesMachine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cpu.user = m;
    m->irq_since = m->irq_until = UINT64_MAX;
    m->next_event = UINT64_MAX;
    return m;
}

void nes_destroy(NesMachine *m)
{
    if (!m) return;
    nes_rc_install(m, NULL);
    nes_coverage_free(m);
    free((void *)m->file);
    free(m);
}

bool nes_load_rom(NesMachine *m, const uint8_t *data, size_t size)
{
    NesCartInfo info;
    if (!nes_cart_parse(data, size, &info)) return false;
    m->info = info;
    if (!info.board) return false;
    uint8_t *copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, data, size);
    free((void *)m->file);
    m->file = copy;
    m->file_size = (uint32_t)size;
    m->prg = copy + info.prg_offset;
    m->chr = info.chr_size ? copy + info.chr_offset : NULL;
    memset(m->prg_ram, 0, sizeof m->prg_ram);
    m->prg_ram_dirty = false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Interrupt lines                                                     */
/* ------------------------------------------------------------------ */

void nes_update_attention(NesMachine *m)
{
    uint8_t a = (uint8_t)(m->attention & (NES_ATTENTION_FRAME | NES_ATTENTION_REMAP));
    if (m->nmi_pending || nes_irq_line(m) || nes_irq_sampled(m)) a |= NES_ATTENTION_INTERRUPT;
    if (m->cpu.jammed) a |= NES_ATTENTION_HALT;
    m->attention = a;
    /* Generated code checks the machine when c->cycles reaches next_sync:
     * at the next event, or after every instruction while something needs
     * the run loop. */
    m->cpu.next_sync = (a & (NES_ATTENTION_INTERRUPT | NES_ATTENTION_HALT | NES_ATTENTION_REMAP)) ? 0 : m->next_event;
}

/* The NMI input follows the PPU's output; the CPU latches a falling edge
 * (here: the line becoming active). `dot` is when the PPU drove it. */
void nes_ppu_outputs(NesMachine *m)
{
    const bool line = nes_ppu_nmi_line(&m->ppu);
    if (line && !m->nmi_line) {
        m->nmi_pending = true;
        m->nmi_at = m->ppu_dots;
    }
    m->nmi_line = line;
    if (m->ppu.frame_ready) {
        m->ppu.frame_ready = false;
        m->frame_ready = true;
        m->frame_count++;
        m->attention |= NES_ATTENTION_FRAME;
    }
}

/* The IRQ input is the APU's frame and DMC interrupts and the cartridge's
 * line, wired together. `at` is the CPU cycle a change happened in. */
void nes_update_irq(NesMachine *m, uint64_t at)
{
    bool apu = nes_apu_irq(&m->apu);
    /* The APU takes a DMC byte when it asks for it, but the flag a sample's
     * last byte raises appears on the DMA's read cycle: hold it back until
     * then (nes_bus.c calls again from that cycle). */
    if (apu && m->dmc_pending && !m->dmc_irq_before && !nes_apu_frame_irq(&m->apu)) apu = false;
    const bool line = apu || m->cart.irq;
    if (line == nes_irq_line(m)) return;
    if (line) {
        m->irq_since = at;
        m->irq_until = UINT64_MAX;
    } else {
        m->irq_until = at;
    }
}

/* ------------------------------------------------------------------ */
/* Device clocks                                                       */
/* ------------------------------------------------------------------ */

void nes_sync_ppu(NesMachine *m, uint64_t target_dots)
{
    /* A cartridge that raises its IRQ from the PPU's address lines (MMC3)
     * does so in the middle of a run and cannot say at which dot. While it
     * is armed and the lines can move by themselves, the PPU advances three
     * dots at a time, so the step just run names the CPU cycle.
     *
     * The line reaches the CPU a dot after A12 rises: a rise in the last
     * dot of a cycle belongs to the next one. The steps are cut to match:
     * each ends two dots into a cycle. Measured with blargg's
     * mmc3_test_2/4-scanline_timing, which places the IRQ to the dot
     * against both pattern table arrangements. */
    const bool by_cycle = nes_ppu_address_moves(m);
    while (m->ppu_dots < target_dots) {
        uint64_t n = target_dots - m->ppu_dots;
        const uint32_t until = nes_ppu_dots_until_event(&m->ppu);
        if (n > until) n = until;
        if (by_cycle) {
            const uint64_t in_step = NES_DOTS_PER_CYCLE - (m->ppu_dots + 1u) % NES_DOTS_PER_CYCLE;
            if (n > in_step) n = in_step;
        }
        const uint8_t cart_irq = m->cart.irq;
        nes_ppu_run(&m->ppu, (uint32_t)n);
        m->ppu_dots += n;
        nes_ppu_outputs(m);
        if (m->cart.irq != cart_irq) nes_update_irq(m, m->ppu_dots / NES_DOTS_PER_CYCLE + 1u);
    }
}

void nes_sync_apu(NesMachine *m, uint64_t target_cycles)
{
    while (m->apu_cycles < target_cycles) {
        uint64_t n = target_cycles - m->apu_cycles;
        const uint32_t until = nes_apu_cycles_until_event(&m->apu);
        if (n > until) n = until;
        nes_apu_run(&m->apu, (uint32_t)n);
        m->apu_cycles += n;
        nes_update_irq(m, m->apu_cycles);
    }
}

/* When the next catch-up is due: the earliest moment a device could change
 * an output on its own. A cartridge that raises its IRQ from PPU fetches
 * cannot say when, so the PPU is caught up after every instruction while
 * that interrupt is enabled and the PPU's address lines are moving. */
void nes_schedule(NesMachine *m)
{
    const uint64_t now = m->cpu.cycles;
    const uint64_t ppu_dot = m->ppu_dots + nes_ppu_dots_until_event(&m->ppu);
    uint64_t next = (ppu_dot + NES_DOTS_PER_CYCLE - 1u) / NES_DOTS_PER_CYCLE;
    const uint64_t apu = m->apu_cycles + nes_apu_cycles_until_event(&m->apu);
    if (apu < next) next = apu;
    if (nes_ppu_address_moves(m) && next > now + 1u) next = now + 1u;
    /* The DMC's fetch is asked for in APU step s (the n-th from now runs
     * step apu_cycles + n - 1): the first read it can halt is the one made
     * with s cycles completed. */
    const uint32_t fetch = nes_apu_cycles_until_dmc(&m->apu);
    m->dmc_at = m->dmc_pending ? 0 : fetch >= NES_APU_EVENT_MAX ? UINT64_MAX : m->apu_cycles + fetch - 1u;
    m->dma_at = m->oam_pending ? 0 : m->dmc_at;
    /* A register access in the middle of an instruction comes here with
     * only its own device caught up. If the other one's event is already
     * due, next_event stays at or before now, so the catch-up at the end of
     * the instruction happens: moving it past now would let the next
     * instruction's interrupt poll go by without the edge (an APU IRQ that
     * rises during STA $4014 was noticed one instruction late, behind the
     * 256 PPU writes of the DMA). After nes_sync both devices are at now and
     * their events lie ahead. */
    m->next_event = next;
    nes_update_attention(m);
}

void nes_sync(NesMachine *m)
{
    if (m->in_sync) return;
    m->in_sync = true;
    nes_sync_apu(m, m->cpu.cycles);
    nes_sync_ppu(m, m->cpu.cycles * NES_DOTS_PER_CYCLE);
    m->in_sync = false;
    nes_schedule(m);
}

/* ------------------------------------------------------------------ */
/* Power and the run loop                                              */
/* ------------------------------------------------------------------ */

static void start(NesMachine *m, bool power)
{
    m->shift[0] = m->shift[1] = 0;
    m->strobe = m->strobe_latch = false;
    m->strobe_at = UINT64_MAX;
    m->nmi_line = m->nmi_pending = false;
    m->nmi_at = 0;
    m->irq_since = m->irq_until = UINT64_MAX;
    m->attention = 0;
    m->in_dma = false;
    m->oam_pending = m->dmc_pending = m->dmc_dummy = m->dmc_irq_before = false;
    m->dma_at = m->dmc_at = UINT64_MAX;
    m->in_sync = false;
    m->frame_ready = false;
    if (power) {
        m->ppu_dots = 0;
        m->apu_cycles = 0;
        m->dmc_stolen = 0;
        m->frame_count = 0;
    }
    m->next_event = m->cpu.cycles + 1u;
    nes_cpu_reset(&m->cpu);
    nes_sync(m);
}

void nes_power(NesMachine *m)
{
    /* RAM powers up with unpredictable contents; zeros keep runs
     * reproducible. */
    memset(m->ram, 0, sizeof m->ram);
    memset(m->chr_ram, 0, sizeof m->chr_ram);
    memset(m->nt_ram, 0, sizeof m->nt_ram);
    memset(&m->cpu, 0, sizeof m->cpu);
    m->cpu.user = m;
    nes_cart_power(m);
    nes_ppu_power(&m->ppu);
    m->ppu.user = m;
    nes_apu_power(&m->apu);
    m->apu.user = m;
    start(m, true);
}

void nes_reset(NesMachine *m)
{
    nes_sync(m);
    nes_ppu_reset(&m->ppu);
    nes_apu_reset(&m->apu);
    start(m, false);
}

void nes_set_buttons(NesMachine *m, int port, uint8_t buttons)
{
    if (port >= 0 && port < 2) m->buttons[port] = buttons;
}

void nes_run_cycles(NesMachine *m, uint64_t cycles)
{
    const uint64_t target = m->cpu.cycles + cycles;
    while (m->cpu.cycles < target) nes_run_slice(m);
}

/* One iteration of the run loop: take a sampled interrupt, then one
 * instruction or one call into recompiled code, then due events. */
void nes_run_slice(NesMachine *m)
{
    NesCpu *c = &m->cpu;
    if (c->jammed) {
        /* A jammed CPU stops until reset; time goes on. */
        c->cycles = m->next_event > c->cycles ? m->next_event : c->cycles + 1u;
        nes_sync(m);
        return;
    }

    if (m->nmi_pending && nes_nmi_sampled(m)) {
        m->nmi_pending = false;
        nes_update_attention(m);
        nes_cpu_interrupt(c, true);
        if (m->coverage) nes_coverage_branched(m);
    } else if (nes_irq_sampled(m)) {
        nes_cpu_interrupt(c, false);
        if (m->coverage) nes_coverage_branched(m);
    }

    const uint32_t key = nes_rc_key_at(m, c->pc);
    NesRcFunc native = m->rc_program ? nes_rc_lookup(m, key) : NULL;
    if (native) {
        /* Recompiled code runs until it leaves its known code or yields. */
        c->branched = 0;
        m->in_native = true;
        native(c, key);
        m->in_native = false;
        c->branched = 0;
        if (m->coverage) nes_coverage_entry(m, nes_rc_key_at(m, c->pc));
    } else {
        if (m->trace) m->trace(m);
        if (m->coverage) {
            nes_coverage_note(m, key);
            c->branched = 0;
            nes_cpu_step(c);
            if (c->branched) nes_coverage_branched(m);
            c->branched = 0;
        } else {
            nes_cpu_step(c);
        }
    }
    if (m->attention & (NES_ATTENTION_FRAME | NES_ATTENTION_REMAP)) {
        m->attention = (uint8_t)(m->attention & ~(NES_ATTENTION_FRAME | NES_ATTENTION_REMAP));
        nes_update_attention(m);
    }
    if (c->jammed) nes_update_attention(m);
    nes_catch_up(m);
}

void nes_run_frame(NesMachine *m)
{
    m->frame_ready = false;
    while (!m->frame_ready) nes_run_slice(m);
}

void nes_frame_rgb(const NesMachine *m, uint32_t *out)
{
    static uint32_t palette[512];
    static bool ready;
    if (!ready) {
        nes_ppu_palette(palette);
        ready = true;
    }
    for (size_t i = 0; i < (size_t)NES_WIDTH * NES_HEIGHT; i++) out[i] = palette[m->ppu.frame[i] & 0x1FFu];
}
