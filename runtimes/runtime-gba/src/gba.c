/*
 * Machine lifecycle, event scheduler, video timing and the interpreter run
 * loop.
 */
#include "gba.h"

#include <stdlib.h>
#include <string.h>

#include "gba_bus.h"
#include "gba_recomp.h"
#include "gba_texture_pack.h"

_Static_assert(offsetof(GbaMachine, cpu) == 0, "GbaCpu must be the first member of GbaMachine");

GbaMachine *gba_create(void)
{
    GbaMachine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    gba_bios_install_hle(m);
    m->bios_hle = true;
    gba_reset(m, true);
    return m;
}

void gba_destroy(GbaMachine *m)
{
    if (!m) return;
    gba_ppu_set_affine_scale(m, 1);
    gba_coverage_free(m);
    free(m->rc_verified);
    free(m->rc_index);
    free(m->rom);
    free(m);
}

static uint32_t next_pow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* Save type from the ID string the SDK links into the ROM. */
static GbaSaveType detect_save_type(const uint8_t *rom, size_t size)
{
    static const struct { const char *id; GbaSaveType type; } ids[] = {
        { "EEPROM_V", GBA_SAVE_EEPROM },
        { "SRAM_V", GBA_SAVE_SRAM },
        { "SRAM_F_V", GBA_SAVE_SRAM },
        { "FLASH_V", GBA_SAVE_FLASH64 },
        { "FLASH512_V", GBA_SAVE_FLASH64 },
        { "FLASH1M_V", GBA_SAVE_FLASH128 },
    };
    for (size_t i = 0; i + 12 <= size; i += 4) {
        if (rom[i] != 'E' && rom[i] != 'S' && rom[i] != 'F') continue; /* no ID starts here */
        for (size_t k = 0; k < sizeof ids / sizeof ids[0]; k++) {
            size_t len = strlen(ids[k].id);
            if (i + len <= size && memcmp(rom + i, ids[k].id, len) == 0) return ids[k].type;
        }
    }
    return GBA_SAVE_NONE;
}

bool gba_load_rom(GbaMachine *m, const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > GBA_ROM_MAX) return false;
    uint8_t *copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, data, size);
    free(m->rom);
    m->rom = copy;
    m->rom_size = (uint32_t)size;
    m->rom_mask = next_pow2((uint32_t)size) - 1u;
    m->save_type = detect_save_type(data, size);
    memset(m->save, 0xFF, sizeof m->save);
    /* A cartridge clock that has been running: 24-hour mode, battery good.
     * Headless runs start it at a fixed time; frontends switch to the
     * host clock (rtc.wall_clock). */
    memset(&m->rtc, 0, sizeof m->rtc);
    m->rtc.present = gba_rtc_detect(data, size);
    m->rtc.control = 0x40;
    gba_rtc_parse_time("2026-01-01 12:00:00", &m->rtc.start);
    return true;
}

bool gba_load_bios(GbaMachine *m, const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > GBA_BIOS_SIZE) return false;
    memset(m->bios, 0, sizeof m->bios);
    memcpy(m->bios, data, size);
    m->bios_hle = false;
    return true;
}

void gba_reset(GbaMachine *m, bool skip_bios)
{
    memset(m->ewram, 0, sizeof m->ewram);
    memset(m->iwram, 0, sizeof m->iwram);
    memset(m->io, 0, sizeof m->io);
    memset(m->palette, 0, sizeof m->palette);
    memset(m->vram, 0, sizeof m->vram);
    memset(m->oam, 0, sizeof m->oam);
    memset(m->timer, 0, sizeof m->timer);
    memset(m->dma, 0, sizeof m->dma);
    memset(m->framebuffer, 0, sizeof m->framebuffer);

    m->cpu.user = m;
    gba_cpu_reset(&m->cpu, skip_bios);

    m->ie = m->if_ = m->ime = 0;
    m->halted = m->stopped = false;
    m->intr_wait_active = false;
    m->attention = 0;
    m->vcount = 0;
    m->dispstat = 0;
    m->line_start = 0;
    m->frame_count = 0;
    m->frame_ready = false;
    m->dma_active = -1;
    m->keyinput = 0x3FF;
    m->memcnt = 0x0D000020u;
    m->debug_enabled = false;
    m->instructions = 0;
    m->interpreted_instructions = 0;
    m->pipe_stale = false;
    m->flash_stage = 0;
    m->flash_bank = 0;
    m->flash_id_mode = m->flash_erase_armed = m->flash_program_next = m->flash_bank_next = false;
    memset(&m->eeprom, 0, sizeof m->eeprom);
    m->eeprom.out_pos = 68;
    gba_rtc_reset(m);
    m->bios_latch = 0;

    m->io[0x88] = 0x00; /* SOUNDBIAS low */
    m->io[0x89] = 0x02; /* SOUNDBIAS = 0x200 */
    if (skip_bios) {
        m->io[0x300] = 1;             /* POSTFLG: booted */
        m->bios_latch = 0xE129F000u;  /* last BIOS opcode before the jump */
        /* The BIOS leaves identity affine matrices for BG2 and BG3. */
        m->io[0x21] = 0x01;           /* BG2PA = 0x100 */
        m->io[0x27] = 0x01;           /* BG2PD = 0x100 */
        m->io[0x31] = 0x01;           /* BG3PA = 0x100 */
        m->io[0x37] = 0x01;           /* BG3PD = 0x100 */
    }
    gba_update_waitstates(m);

    for (int i = 0; i < GBA_EVENT_COUNT; i++) m->event_at[i] = UINT64_MAX;
    m->sio_at = UINT64_MAX;
    m->next_event = UINT64_MAX;
    gba_schedule(m, GBA_EVENT_HBLANK, GBA_HDRAW_CYCLES);
    gba_schedule(m, GBA_EVENT_LINE_END, GBA_CYCLES_PER_LINE);

    gba_cpu_refill(&m->cpu);
    m->cpu.cycles = 0;
    m->pf_at = 0;
    gba_apu_reset(m);
    if (m->link) gba_sio_state_loaded(m);
}

/* ------------------------------------------------------------------ */
/* Scheduler                                                           */
/* ------------------------------------------------------------------ */

static void update_next_event(GbaMachine *m)
{
    uint64_t next = m->sio_at;
    for (int i = 0; i < GBA_EVENT_COUNT; i++) {
        if (m->event_at[i] < next) next = m->event_at[i];
    }
    m->next_event = next;
}

void gba_schedule(GbaMachine *m, int event, uint64_t at)
{
    m->event_at[event] = at;
    update_next_event(m);
}

void gba_schedule_sio(GbaMachine *m, uint64_t at)
{
    m->sio_at = at;
    update_next_event(m);
}

void gba_raise_irq(GbaMachine *m, uint16_t bits)
{
    m->if_ |= bits;
    gba_update_attention(m);
}

static void vcount_compare(GbaMachine *m)
{
    if (m->vcount == (m->dispstat >> 8)) {
        if (!(m->dispstat & 4u) && (m->dispstat & 0x20u)) gba_raise_irq(m, GBA_IRQ_VCOUNT);
        m->dispstat |= 4u;
    } else {
        m->dispstat &= 0xFFFBu;
    }
}

static void on_hblank(GbaMachine *m)
{
    m->dispstat |= 2u;
    if (m->dispstat & 0x10u) gba_raise_irq(m, GBA_IRQ_HBLANK);
    if (m->vcount < GBA_SCREEN_HEIGHT) {
        gba_ppu_render_line(m, m->vcount);
        gba_dma_trigger(m, 2);
    }
    gba_schedule(m, GBA_EVENT_HBLANK, UINT64_MAX);
}

static void on_line_end(GbaMachine *m)
{
    m->dispstat &= 0xFFFDu;
    m->line_start += GBA_CYCLES_PER_LINE;
    m->vcount = (uint16_t)((m->vcount + 1u) % GBA_LINES_PER_FRAME);

    if (m->vcount == GBA_SCREEN_HEIGHT) {
        if (gba_tex_record) gba_texture_pack_frame_complete(m);
        m->dispstat |= 1u;
        if (m->dispstat & 0x08u) gba_raise_irq(m, GBA_IRQ_VBLANK);
        gba_dma_trigger(m, 1);
        gba_ppu_frame_start(m);
        m->frame_ready = true;
        m->attention |= GBA_ATTENTION_FRAME;
        m->frame_count++;
    } else if (m->vcount == GBA_LINES_PER_FRAME - 1) {
        m->dispstat &= 0xFFFEu; /* the flag clears on line 227 */
    }
    vcount_compare(m);

    gba_schedule(m, GBA_EVENT_HBLANK, m->line_start + GBA_HDRAW_CYCLES);
    gba_schedule(m, GBA_EVENT_LINE_END, m->line_start + GBA_CYCLES_PER_LINE);
}

void gba_process_events(GbaMachine *m)
{
    /* Not reentrant: a DMA started by an event may read I/O, whose
     * catch-up would otherwise run the same event again. */
    if (m->in_events) return;
    m->in_events = true;
    while (m->next_event <= m->cpu.cycles) {
        int event = 0;
        for (int i = 1; i < GBA_EVENT_COUNT; i++) {
            if (m->event_at[i] < m->event_at[event]) event = i;
        }
        if (m->sio_at < m->event_at[event]) {
            gba_schedule_sio(m, UINT64_MAX); /* the handler sets its next time */
            gba_sio_event(m);
            continue;
        }
        switch (event) {
        case GBA_EVENT_HBLANK: on_hblank(m); break;
        case GBA_EVENT_LINE_END: on_line_end(m); break;
        case GBA_EVENT_TIMER0:
        case GBA_EVENT_TIMER1:
        case GBA_EVENT_TIMER2:
        case GBA_EVENT_TIMER3: gba_timer_overflow_event(m, event - GBA_EVENT_TIMER0); break;
        case GBA_EVENT_DMA: gba_dma_run_pending(m); break;
        case GBA_EVENT_APU: gba_apu_sample_event(m); break;
        default: gba_schedule(m, event, UINT64_MAX); break;
        }
    }
    m->in_events = false;
}

/* ------------------------------------------------------------------ */
/* Run loop                                                            */
/* ------------------------------------------------------------------ */

/* Load the pipeline with the next two opcodes without charging cycles: used
 * when the interpreter takes over from recompiled code mid-stream, where the
 * fetches were already paid for. */
void gba_prime_pipeline(GbaMachine *m)
{
    GbaCpu *cpu = &m->cpu;
    uint32_t pc = gba_cpu_pc(cpu);
    if (cpu->cpsr & GBA_CPSR_T) {
        cpu->pipe[0] = (gba_peek32(m, pc) >> ((pc & 2u) * 8u)) & 0xFFFFu;
        cpu->pipe[1] = (gba_peek32(m, pc + 2) >> (((pc + 2) & 2u) * 8u)) & 0xFFFFu;
    } else {
        cpu->pipe[0] = gba_peek32(m, pc);
        cpu->pipe[1] = gba_peek32(m, pc + 4);
    }
}

void gba_run_cycles(GbaMachine *m, uint64_t cycles)
{
    uint64_t target = m->cpu.cycles + cycles;
    while (m->cpu.cycles < target && !(m->attention & GBA_ATTENTION_STALL)) gba_run_slice(m, target);
}

/* One iteration of the run loop: wake from halt or skip to the next event,
 * take a pending interrupt, then run either one native function call or
 * one interpreted instruction, then due events. */
void gba_run_slice(GbaMachine *m, uint64_t limit)
{
    GbaCpu *cpu = &m->cpu;
    {
        if (m->attention & GBA_ATTENTION_STALL) return; /* waiting for the other players */
        if (m->halted || m->stopped) {
            if (m->ie & m->if_) {
                m->halted = m->stopped = false;
                gba_update_attention(m);
            } else {
                uint64_t until = m->next_event < limit ? m->next_event : limit;
                if (until > cpu->cycles) cpu->cycles = until;
                gba_process_events(m);
                return;
            }
        }

        if ((m->ime & 1u) && (m->ie & m->if_) && !(cpu->cpsr & GBA_CPSR_I)) {
            gba_cpu_irq(cpu);
            m->pipe_stale = false;
        }

        uint32_t pc = gba_cpu_pc(cpu);
        GbaRcFunc native = m->rc_program ? gba_rc_lookup(m, pc | ((cpu->cpsr & GBA_CPSR_T) ? 1u : 0u)) : NULL;
        if (native) {
            /* Recompiled code runs until it leaves its blocks or yields.
             * It charged every fetch; a branch out refills the pipeline
             * exactly as the interpreter would. */
            cpu->branched = false;
            m->in_native = true;
            native(m, pc);
            m->in_native = false;
            m->attention = (uint8_t)(m->attention & ~GBA_ATTENTION_FRAME);
            if (cpu->branched) gba_cpu_refill(cpu);
            else m->pipe_stale = true;
            if (m->coverage) gba_coverage_entry(m, gba_cpu_pc(cpu) | ((cpu->cpsr & GBA_CPSR_T) ? 1u : 0u));
        } else {
            if (m->pipe_stale) {
                gba_prime_pipeline(m);
                m->pipe_stale = false;
            }
            if (m->step_hook) m->step_hook(m);
            if (m->coverage) {
                uint32_t mode = gba_cpu_mode(cpu);
                gba_coverage_note(m, pc | ((cpu->cpsr & GBA_CPSR_T) ? 1u : 0u));
                gba_cpu_step(cpu);
                /* A mode change is an exception entry or return, not a branch
                 * into code (an interrupt returns to any instruction). */
                if (cpu->branched && gba_cpu_mode(cpu) == mode) gba_coverage_branched(m);
            } else {
                gba_cpu_step(cpu);
            }
            m->instructions++;
            m->interpreted_instructions++;
        }

        /* A SWI trapped into the built-in BIOS: perform it in C. */
        if (cpu->branched && m->bios_hle && cpu->r[15] == GBA_VECTOR_SWI + 8 &&
            gba_cpu_mode(cpu) == GBA_MODE_SVC) {
            gba_bios_hle_swi(m);
            gba_cpu_refill(cpu);
        }

        if (cpu->cycles >= m->next_event) gba_process_events(m);
    }
}

void gba_run_frame(GbaMachine *m)
{
    m->frame_ready = false;
    while (!m->frame_ready && !(m->attention & GBA_ATTENTION_STALL)) gba_run_cycles(m, GBA_CYCLES_PER_LINE);
}
