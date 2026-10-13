/*
 * Machine lifecycle, event scheduler, video timing, interrupts, the APU
 * clock, and the run loop.
 *
 * Interrupt timing: the 65C816 samples its interrupt inputs when the last
 * bus cycle of an instruction begins (c->last_cycle_at, with the I flag of
 * that moment in c->last_i). An NMI edge or IRQ level that arrives later
 * is taken one instruction later. This is why CLI lets a pending IRQ in
 * only after the following instruction, and SEI lets one more through.
 */
#include "snes.h"

#include <stdlib.h>
#include <string.h>

#include "snes_bus.h"
#include "snes_recomp.h"

_Static_assert(offsetof(SnesMachine, cpu) == 0, "SnesCpu must be the first member of SnesMachine");

/* Where events sit in a line (master clocks from the line start). */
enum {
    LINE_HDMA_INIT = 12,
    LINE_JOYPAD = 130,
    LINE_REFRESH = 538,
    LINE_RENDER = 1096, /* end of active display, before H-blank HDMA */
    LINE_HDMA = 1104,
};

SnesMachine *snes_create(void)
{
    SnesMachine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cpu.user = m;
    snes_ppu_power(&m->ppu);
    snes_apu_power(&m->apu);
    return m;
}

void snes_destroy(SnesMachine *m)
{
    if (!m) return;
    snes_ppu_set_mode7_scale(&m->ppu, 1);
    snes_rc_install(m, NULL);
    snes_coverage_free(m);
    free((void *)m->rom);
    free(m);
}

bool snes_load_rom(SnesMachine *m, const uint8_t *data, size_t size)
{
    /* A 512-byte copier header in front of the image. */
    if (size % 1024u == 512u) {
        data += 512;
        size -= 512;
    }
    if (!data || size < 0x8000u || size > SNES_ROM_MAX) return false;
    SnesCartInfo info;
    if (!snes_cart_parse(data, size, &info)) return false;
    uint8_t *copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, data, size);
    free((void *)m->rom);
    m->rom = copy;
    m->rom_size = (uint32_t)size;
    m->cart = info;
    memset(m->sram, 0xFF, sizeof m->sram);
    m->sram_dirty = false;
    snes_cart_map(m);
    return true;
}

/* ------------------------------------------------------------------ */
/* Scheduler                                                           */
/* ------------------------------------------------------------------ */

/* Generated code checks the machine when c->cycles reaches next_sync: at
 * the next event, or at once while an interrupt or sleep is pending (an
 * NMI raised by a register write, for instance, arrives between events). */
static void update_next_sync(SnesMachine *m)
{
    m->cpu.next_sync = (m->attention & 3u) ? 0 : m->next_event;
}

void snes_schedule(SnesMachine *m, int event, uint64_t at)
{
    m->event_at[event] = at;
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < SNES_EVENT_COUNT; i++) {
        if (m->event_at[i] < next) next = m->event_at[i];
    }
    m->next_event = next;
    update_next_sync(m);
}

uint16_t snes_hcounter(const SnesMachine *m)
{
    uint64_t h = m->cpu.cycles > m->line_start ? (m->cpu.cycles - m->line_start) >> 2 : 0;
    return (uint16_t)(h > 339 ? 339 : h);
}

static void set_irq_line(SnesMachine *m, uint64_t at)
{
    bool asserted = (m->timeup && (m->nmitimen & 0x30u)) || m->coproc_irq;
    if (!asserted) m->irq_since = UINT64_MAX;
    else if (m->irq_since == UINT64_MAX) m->irq_since = at;
    snes_update_attention(m);
}

void snes_update_irq(SnesMachine *m) { set_irq_line(m, m->cpu.cycles); }

void snes_set_coproc_irq(SnesMachine *m, bool level, uint64_t at)
{
    if (m->coproc_irq == level) return;
    m->coproc_irq = level;
    set_irq_line(m, at);
}

void snes_update_attention(SnesMachine *m)
{
    uint8_t a = (uint8_t)(m->attention & SNES_ATTENTION_FRAME);
    if (m->nmi_transition || m->irq_since != UINT64_MAX) a |= 1u;
    if (m->cpu.wai || m->cpu.stp) a |= 2u;
    m->attention = a;
    update_next_sync(m);
}

/* The H/V timer IRQ for the current line: V-only fires near the start of
 * line VTIME, H (and H+V) about 14 clocks after dot HTIME. A time that has
 * already passed does not fire (the timer matches on an edge). */
static void schedule_irq_from(SnesMachine *m, uint64_t now)
{
    uint64_t at = UINT64_MAX;
    const bool h = (m->nmitimen & 0x10u) != 0, v = (m->nmitimen & 0x20u) != 0;
    if ((h || v) && (!v || m->vcounter == m->vtime)) {
        uint64_t offset = h ? (m->htime <= 339 ? (uint64_t)m->htime * 4u + 14u : UINT64_MAX) : 10u;
        if (offset < m->line_length && m->line_start + offset >= now) at = m->line_start + offset;
    }
    snes_schedule(m, SNES_EVENT_IRQ, at);
}

void snes_schedule_irq(SnesMachine *m) { schedule_irq_from(m, m->cpu.cycles); }

static void set_ppu_position(SnesMachine *m, uint16_t h)
{
    m->ppu.hcounter = h;
    m->ppu.vcounter = m->vcounter;
    m->ppu.vblank = m->vcounter >= m->vdisp;
}

static void start_line(SnesMachine *m)
{
    const uint64_t t = m->line_start;
    /* The odd non-interlaced field has one short line. */
    m->line_length = (m->vcounter == 240 && m->field && !snes_ppu_interlace(&m->ppu)) ? 1360u : SNES_CLOCKS_PER_LINE;
    if (m->vcounter < 225) m->vdisp = snes_ppu_overscan(&m->ppu) ? 240 : 225;
    snes_schedule(m, SNES_EVENT_LINE_END, t + m->line_length);
    snes_schedule(m, SNES_EVENT_REFRESH, t + LINE_REFRESH);
    snes_schedule(m, SNES_EVENT_RENDER, (m->vcounter >= 1 && m->vcounter < m->vdisp) ? t + LINE_RENDER : UINT64_MAX);
    snes_schedule(m, SNES_EVENT_HDMA, m->vcounter < m->vdisp ? t + LINE_HDMA : UINT64_MAX);
    if (m->vcounter == 0) snes_schedule(m, SNES_EVENT_HDMA_INIT, t + LINE_HDMA_INIT);
    if (m->vcounter == m->vdisp) snes_schedule(m, SNES_EVENT_JOYPAD, t + LINE_JOYPAD);
    schedule_irq_from(m, t);
}

static void on_line_end(SnesMachine *m)
{
    m->line_start += m->line_length;
    m->vcounter++;
    if (m->vcounter >= m->lines_this_frame) {
        m->vcounter = 0;
        m->field = !m->field;
        /* Interlaced: the even field has an extra line. */
        m->lines_this_frame = (snes_ppu_interlace(&m->ppu) && !m->field) ? 263u : SNES_LINES_NTSC;
        m->rdnmi = false;
        set_ppu_position(m, 0);
        snes_ppu_frame_start(&m->ppu);
    }
    if (m->vcounter == m->vdisp) {
        m->rdnmi = true;
        if (m->nmitimen & 0x80u) {
            m->nmi_transition = true;
            m->nmi_at = m->line_start;
        }
        set_ppu_position(m, 0);
        snes_ppu_vblank_start(&m->ppu);
        m->frame_ready = true;
        m->attention |= SNES_ATTENTION_FRAME;
        m->frame_count++;
        snes_update_attention(m);
    }
    snes_sync_apu(m);
    start_line(m);
}

void snes_process_events(SnesMachine *m)
{
    /* Not reentrant: DMA started by an event reads I/O, whose catch-up
     * would otherwise run the same event again. */
    if (m->in_events) return;
    m->in_events = true;
    /* The SA-1 keeps pace with the S-CPU at least event by event, so an
     * IRQ it raises reaches the S-CPU within a fraction of a line. */
    snes_sa1_catch_up(m, &m->sa1, m->cpu.cycles);
    while (m->next_event <= m->cpu.cycles) {
        int event = 0;
        for (int i = 1; i < SNES_EVENT_COUNT; i++) {
            if (m->event_at[i] < m->event_at[event]) event = i;
        }
        const uint64_t at = m->event_at[event];
        switch (event) {
        case SNES_EVENT_LINE_END: on_line_end(m); break;
        case SNES_EVENT_RENDER:
            snes_schedule(m, SNES_EVENT_RENDER, UINT64_MAX);
            set_ppu_position(m, LINE_RENDER / 4);
            snes_ppu_render_line(&m->ppu, m->vcounter);
            break;
        case SNES_EVENT_HDMA:
            snes_schedule(m, SNES_EVENT_HDMA, UINT64_MAX);
            snes_hdma_line(m);
            break;
        case SNES_EVENT_HDMA_INIT:
            snes_schedule(m, SNES_EVENT_HDMA_INIT, UINT64_MAX);
            snes_hdma_init(m);
            break;
        case SNES_EVENT_REFRESH:
            /* DRAM refresh: the CPU stops for 40 clocks (5 multiplier steps). */
            snes_schedule(m, SNES_EVENT_REFRESH, UINT64_MAX);
            m->cpu.cycles += 40;
            m->alu_edges += 5;
            break;
        case SNES_EVENT_IRQ:
            snes_schedule(m, SNES_EVENT_IRQ, UINT64_MAX);
            m->timeup = true;
            set_irq_line(m, at);
            break;
        case SNES_EVENT_JOYPAD: snes_joypad_event(m); break;
        default: snes_schedule(m, event, UINT64_MAX); break;
        }
    }
    m->in_events = false;
}

/* ------------------------------------------------------------------ */
/* APU clock                                                           */
/* ------------------------------------------------------------------ */

/* Catch the APU up to the S-CPU. SMP time owed is kept exactly, in units
 * of 1/SNES_MASTER_HZ SMP cycles, so the two clocks never drift. */
void snes_sync_apu(SnesMachine *m)
{
    const uint64_t now = m->cpu.cycles;
    if (now <= m->apu_synced_at) return;
    m->apu_debt += (int64_t)(now - m->apu_synced_at) * (int64_t)SNES_SMP_HZ;
    m->apu_synced_at = now;
    if (m->apu_debt >= (int64_t)SNES_MASTER_HZ) {
        uint32_t want = (uint32_t)(m->apu_debt / (int64_t)SNES_MASTER_HZ);
        uint32_t ran = snes_apu_run(&m->apu, want);
        m->apu_debt -= (int64_t)ran * (int64_t)SNES_MASTER_HZ;
    }
}

/* ------------------------------------------------------------------ */
/* Power and the run loop                                              */
/* ------------------------------------------------------------------ */

void snes_power(SnesMachine *m)
{
    /* Work RAM powers up with unpredictable contents; a fixed pattern keeps
     * runs reproducible. */
    memset(m->wram, 0x55, sizeof m->wram);
    memset(&m->cpu, 0, sizeof m->cpu);
    m->cpu.user = m;
    m->cpu.s = 0x01FF;

    m->wram_port = 0;
    m->nmitimen = 0;
    m->wrio = 0xFF;
    m->wrmpya = m->wrmpyb = 0xFF;
    m->wrdiva = 0xFFFF;
    m->wrdivb = 0xFF;
    m->htime = m->vtime = 0x1FF;
    m->memsel = 0;
    m->rddiv = m->rdmpy = 0;
    m->alu_shift = 0;
    m->alu_mul_left = m->alu_div_left = 0;
    m->alu_edges = m->alu_synced = 0;
    memset(m->joy, 0, sizeof m->joy);
    memset(m->shift, 0, sizeof m->shift);
    m->strobe = false;
    m->joypad_busy = false;
    m->rdnmi = m->nmi_transition = m->timeup = false;
    m->nmi_at = 0;
    m->irq_since = UINT64_MAX;
    m->coproc_irq = false;
    m->attention = 0;
    for (int n = 0; n < 8; n++) {
        SnesDmaChannel *ch = &m->dma[n];
        memset(ch, 0, sizeof *ch);
        ch->control = 0xFF;
        ch->b_addr = 0xFF;
        ch->a_addr = 0xFFFF;
        ch->a_bank = 0xFF;
        ch->size = 0xFFFF;
        ch->ind_bank = 0xFF;
        ch->hdma_addr = 0xFFFF;
        ch->line_counter = 0xFF;
        ch->unused_b = 0xFF;
    }
    m->in_dma = false;

    snes_ppu_power(&m->ppu);
    snes_apu_power(&m->apu);
    m->apu_synced_at = 0;
    m->apu_debt = 0;

    m->vcounter = 0;
    m->line_start = 0;
    m->field = false;
    m->lines_this_frame = SNES_LINES_NTSC;
    m->vdisp = 225;
    m->frame_count = 0;
    m->frame_ready = false;
    for (int i = 0; i < SNES_EVENT_COUNT; i++) m->event_at[i] = UINT64_MAX;
    m->next_event = UINT64_MAX;
    m->in_events = false;
    start_line(m);

    snes_sa1_power(m);

    /* The reset sequence takes 186 clocks before the vector is read. */
    m->cpu.cycles = 170;
    snes_cpu_reset(&m->cpu);
}

void snes_set_buttons(SnesMachine *m, int port, uint16_t buttons)
{
    if (port >= 0 && port < 2) m->buttons[port] = (uint16_t)(buttons & 0xFFF0u);
}

void snes_run_clocks(SnesMachine *m, uint64_t clocks)
{
    uint64_t target = m->cpu.cycles + clocks;
    while (m->cpu.cycles < target) snes_run_slice(m, target);
}

/* One iteration of the run loop: sleep through WAI/STP, take a sampled
 * interrupt, then one instruction, then due events. */
void snes_run_slice(SnesMachine *m, uint64_t limit)
{
    SnesCpu *c = &m->cpu;
    if (c->stp || c->wai) {
        bool wake = m->nmi_transition || m->irq_since != UINT64_MAX;
        if (c->wai && wake) {
            /* WAI ends on any interrupt, even a masked IRQ; it is sampled now. */
            c->wai = 0;
            c->last_cycle_at = c->cycles;
            c->last_i = c->flag_i;
            snes_update_attention(m);
        } else {
            uint64_t until = m->next_event < limit ? m->next_event : limit;
            c->cycles = until > c->cycles ? until : c->cycles + 6u;
            snes_process_events(m);
            return;
        }
    }

    const bool nmi = m->nmi_transition && m->nmi_at <= c->last_cycle_at;
    if (nmi || (m->irq_since <= c->last_cycle_at && !c->last_i)) {
        /* The SA-1 may have switched the S-CPU's vectors (SNV/SIV) since it
         * was last caught up. */
        snes_sa1_catch_up(m, &m->sa1, c->cycles);
    }
    if (nmi) {
        m->nmi_transition = false;
        snes_update_attention(m);
        snes_cpu_interrupt(c, true);
        if (m->coverage) snes_coverage_branched(m);
    } else if (m->irq_since <= c->last_cycle_at && !c->last_i) {
        snes_cpu_interrupt(c, false);
        if (m->coverage) snes_coverage_branched(m);
    }

    const uint32_t key = SNES_RC_KEY(snes_cpu_pc24(c), snes_cpu_mode(c));
    SnesRcFunc native = m->rc_program ? snes_rc_lookup(m, key) : NULL;
    if (native) {
        /* Recompiled code runs until it leaves its known code or yields. */
        c->branched = 0;
        m->in_native = true;
        native(c, key);
        m->in_native = false;
        c->branched = 0;
        m->attention = (uint8_t)(m->attention & ~SNES_ATTENTION_FRAME);
        if (m->coverage) snes_coverage_entry(m, SNES_RC_KEY(snes_cpu_pc24(c), snes_cpu_mode(c)));
    } else {
        if (m->trace) m->trace(m);
        if (m->coverage) {
            snes_coverage_note(m, key);
            c->branched = 0;
            snes_cpu_step(c);
            if (c->branched) snes_coverage_branched(m);
            c->branched = 0;
        } else {
            snes_cpu_step(c);
        }
    }
    if (c->wai || c->stp) snes_update_attention(m);
    snes_catch_up(m);
}

void snes_run_frame(SnesMachine *m)
{
    m->frame_ready = false;
    while (!m->frame_ready) snes_run_clocks(m, SNES_CLOCKS_PER_LINE);
}
