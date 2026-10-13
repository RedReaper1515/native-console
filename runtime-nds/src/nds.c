/*
 * Machine lifecycle, the event scheduler, video timing and the run loop
 * that takes the two CPUs through time in turns.
 */
#include "nds.h"

#include <stdlib.h>
#include <string.h>

#include "nds_bios_data.h"
#include "nds_prof.h"
#include "nds_recomp.h"

NdsMachine *nds_create(void)
{
    NdsMachine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->arm9.user = m;
    m->arm7.user = m;
    /* The replacement BIOSes, until nds_load_bios9/7 put something else there. */
    memcpy(m->bios9, nds_bios_arm9, sizeof m->bios9);
    memcpy(m->bios7, nds_bios_arm7, sizeof m->bios7);
    nds_user_settings_default(&m->settings);
    nds_firmware_build(m->firmware, &m->settings);
    m->rtc_base = NDS_RTC_DEFAULT_BASE;
    m->running = -1;
    m->vcount_write = -1;
    for (int i = 0; i < NDS_EVENT_COUNT; i++) m->event_at[i] = UINT64_MAX;
    m->next_event = UINT64_MAX;
    nds_vram_remap(m);
    nds_wram_remap(m);
    nds_tcm_remap(m);
    return m;
}

void nds_destroy(NdsMachine *m)
{
    if (!m) return;
    nds_rc_install(m, NULL);
    nds_set_internal_scale(m, 1);
    free(m->save_data);
    free(m->rom);
    free(m);
}

static uint32_t next_pow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v && p < 0x80000000u) p <<= 1;
    return p;
}

bool nds_load_rom(NdsMachine *m, const uint8_t *data, size_t size)
{
    if (!data || size < 0x200 || size > NDS_ROM_MAX) return false;

    /* Both binaries must lie inside the file and fit where they load. */
    uint32_t arm9_offset = nds_get_le32(data + 0x20), arm9_size = nds_get_le32(data + 0x2C);
    uint32_t arm7_offset = nds_get_le32(data + 0x30), arm7_size = nds_get_le32(data + 0x3C);
    if (arm9_offset > size || arm9_size > size - arm9_offset) return false;
    if (arm7_offset > size || arm7_size > size - arm7_offset) return false;
    if (arm9_size > NDS_MAIN_RAM_SIZE || arm7_size > NDS_MAIN_RAM_SIZE) return false;

    uint32_t padded = next_pow2((uint32_t)size);
    uint8_t *copy = malloc(padded);
    if (!copy) return false;
    memcpy(copy, data, size);
    memset(copy + size, 0xFF, padded - size);
    free(m->rom);
    m->rom = copy;
    m->rom_size = (uint32_t)size;
    m->rom_mask = padded - 1u;

    /* The chip ID: a manufacturer byte, then the size in megabytes less one
     * (GBATEK, "DS Cartridge Protocol"). Bit 31 marks a 1T-ROM card, which
     * the header's ROMCTRL setting gives away. */
    uint32_t megabytes = padded >> 20;
    m->chip_id = 0xC2u | ((megabytes ? megabytes - 1u : 0u) << 8);
    if ((nds_get_le32(data + 0x60) & 0x00FFFFFFu) == 0x00416657u) m->chip_id |= 0x80000000u;

    /* The save chip: the table knows it for most games; for the rest it is
     * found out from the first commands the program sends, and until then
     * the memory is that of the largest chip it could be, erased. */
    NdsSaveType type = NDS_SAVE_UNKNOWN;
    uint32_t save_size = 0;
    if (!nds_save_lookup((const char *)data + 0x0C, &type, &save_size)) {
        type = NDS_SAVE_UNKNOWN;
        save_size = 0;
    }
    uint32_t capacity = save_size ? save_size : 0x100000u;
    free(m->save_data);
    m->save_data = NULL;
    m->save_capacity = 0;
    if (type != NDS_SAVE_NONE && type != NDS_SAVE_NAND) {
        m->save_data = malloc(capacity);
        if (!m->save_data) return false;
        memset(m->save_data, 0xFF, capacity);
        m->save_capacity = capacity;
    }
    m->save_type = (uint8_t)type;
    m->save_size = save_size;
    m->save_dirty = false;
    /* The chip is this card's from now on, whatever runs before a reset. */
    nds_save_init(&m->save, type, m->save_capacity);
    nds_save_set_infrared(&m->save, nds_save_has_infrared((const char *)data + 0x0C));
    return true;
}

const uint8_t *nds_save_memory(const NdsMachine *m, uint32_t *size)
{
    /* An unknown chip has no size yet: writing the whole buffer out would
     * make the next run take it for the largest FLASH. */
    *size = m->save_data && m->save.type != NDS_SAVE_UNKNOWN ? m->save.size : 0;
    return m->save_data;
}

bool nds_load_save(NdsMachine *m, const uint8_t *data, size_t size)
{
    if (!m->save_data || !data || size > m->save_capacity) return false;
    if (m->save.type == NDS_SAVE_UNKNOWN) {
        /* An unknown chip with a save already: its length says what it is,
         * or it is not a save. */
        NdsSaveType type;
        if (!nds_save_type_for_size((uint32_t)size, &type)) return false;
        m->save_type = (uint8_t)type;
        m->save_size = (uint32_t)size;
        nds_save_init(&m->save, type, (uint32_t)size);
        if (m->rom) nds_save_set_infrared(&m->save, nds_save_has_infrared((const char *)m->rom + 0x0C));
    } else if (size > m->save.size) {
        return false;
    }
    memset(m->save_data, 0xFF, m->save_capacity);
    memcpy(m->save_data, data, size);
    m->save_dirty = false;
    return true;
}

const uint8_t *nds_firmware_image(const NdsMachine *m)
{
    return m->firmware;
}

bool nds_load_firmware(NdsMachine *m, const uint8_t *data, size_t size)
{
    if (!data || size != NDS_FIRMWARE_SIZE) return false;
    memcpy(m->firmware, data, NDS_FIRMWARE_SIZE);
    /* The owner's settings in force are the image's, calibration included. */
    uint32_t offset;
    if (nds_firmware_current_settings(m->firmware, &offset)) {
        nds_user_settings_from_block(&m->settings, m->firmware + offset);
    }
    m->firmware_dirty = false;
    return true;
}

void nds_set_user_settings(NdsMachine *m, const NdsUserSettings *settings)
{
    m->settings = *settings;
    nds_firmware_build(m->firmware, &m->settings);
    m->firmware_dirty = false;
}

void nds_set_rtc_base(NdsMachine *m, int64_t seconds)
{
    m->rtc_base = seconds;
}

int64_t nds_rtc_now(const NdsMachine *m)
{
    return m->rtc_base + (int64_t)(m->now / NDS_CLOCK_HZ);
}

bool nds_load_bios9(NdsMachine *m, const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > NDS_BIOS9_SIZE) return false;
    memset(m->bios9, 0, sizeof m->bios9);
    memcpy(m->bios9, data, size);
    nds_rc_invalidate(m);
    return true;
}

bool nds_load_bios7(NdsMachine *m, const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > NDS_BIOS7_SIZE) return false;
    memset(m->bios7, 0, sizeof m->bios7);
    memcpy(m->bios7, data, size);
    nds_rc_invalidate(m);
    return true;
}

/* Where the running slice ends, in system time and in each CPU's own
 * cycles (the ARM9's clock is twice as fast). */
static void set_slice_end(NdsMachine *m, uint64_t end)
{
    m->slice_end = end;
    m->cpu_limit[NDS_ARM9] = end * 2u;
    m->cpu_limit[NDS_ARM7] = end;
}

void nds_reset(NdsMachine *m)
{
    memset(m->main_ram, 0, sizeof m->main_ram);
    memset(m->shared_wram, 0, sizeof m->shared_wram);
    memset(m->arm7_wram, 0, sizeof m->arm7_wram);
    memset(m->itcm, 0, sizeof m->itcm);
    memset(m->dtcm, 0, sizeof m->dtcm);
    memset(m->vram, 0, sizeof m->vram);
    memset(m->palette, 0, sizeof m->palette);
    memset(m->oam, 0, sizeof m->oam);
    memset(m->io9, 0, sizeof m->io9);
    memset(m->io7, 0, sizeof m->io7);
    memset(m->wifi, 0, sizeof m->wifi);
    memset(&m->card, 0, sizeof m->card);
    memset(m->vramcnt, 0, sizeof m->vramcnt);
    memset(m->timer, 0, sizeof m->timer);
    memset(m->dma, 0, sizeof m->dma);
    memset(m->ipc_fifo, 0, sizeof m->ipc_fifo);
    memset(m->framebuffer, 0, sizeof m->framebuffer);

    m->arm9.user = m;
    m->arm7.user = m;
    nds9_cpu_reset(&m->arm9);
    nds7_cpu_reset(&m->arm7);

    m->wramcnt = 0;
    m->exmemcnt[0] = m->exmemcnt[1] = 0;
    m->postflg[0] = m->postflg[1] = 0;
    m->powcnt1 = 0;
    m->powcnt2 = 0;
    m->biosprot = 0;
    for (int cpu = 0; cpu < 2; cpu++) {
        m->ie[cpu] = m->if_[cpu] = 0;
        m->ime[cpu] = 0;
        m->irq_line[cpu] = false;
        m->halted[cpu] = false;
        m->dispstat[cpu] = 0;
        m->ipcsync[cpu] = 0;
        m->ipcfifocnt[cpu] = 0x0101; /* both FIFOs empty */
        m->keycnt[cpu] = 0;
        m->instructions[cpu] = 0;
        m->interpreted[cpu] = 0;
        m->rc_calls[cpu] = 0;
        m->pipe_stale[cpu] = false;
    }
    m->vcount = 0;
    m->vcount_write = -1;
    m->line_start = 0;
    m->frame_count = 0;
    m->frame_ready = false;
    m->divcnt = m->sqrtcnt = 0;
    m->div_numer = m->div_denom = m->div_result = m->div_remainder = 0;
    m->sqrt_param = 0;
    m->sqrt_result = 0;
    m->keyinput = 0x03FF;
    m->extkeyin = 0x007F;
    m->touch_down = false;
    m->touch_x = m->touch_y = 0;
    m->lid_closed = false;

    nds_spi_reset(&m->spi);
    nds_rtc_reset(&m->rtc);
    /* The save chip keeps what it is and what it holds; it only comes up
     * between commands. */
    nds_save_reset(&m->save);
    m->touch_calibration = m->settings;
    m->powered_off = false;

    m->now = 0;
    set_slice_end(m, 0);
    m->running = -1;
    for (int i = 0; i < NDS_EVENT_COUNT; i++) m->event_at[i] = UINT64_MAX;
    m->next_event = UINT64_MAX;
    nds_schedule(m, NDS_EVENT_HBLANK, NDS_HBLANK_CYCLES);
    nds_schedule(m, NDS_EVENT_LINE_END, NDS_CYCLES_PER_LINE);
    nds_sound_reset(m);

    nds_vram_remap(m);
    nds_wram_remap(m);
    nds_tcm_remap(m);
    nds_video_reset(m);

    if (m->rom) nds_boot_direct(m);
    /* Memory was just filled without going through the bus. */
    nds_rc_invalidate(m);

    nds9_cpu_refill(&m->arm9);
    nds7_cpu_refill(&m->arm7);
    m->arm9.cycles = 0;
    m->arm7.cycles = 0;
}

/* ------------------------------------------------------------------ */
/* Scheduler                                                           */
/* ------------------------------------------------------------------ */

void nds_schedule(NdsMachine *m, int event, uint64_t at)
{
    m->event_at[event] = at;
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < NDS_EVENT_COUNT; i++) {
        if (m->event_at[i] < next) next = m->event_at[i];
    }
    m->next_event = next;
    /* An event due before the slice ends shortens the slice, but never to
     * before what has already been run. */
    if (m->running >= 0 && at < m->slice_end) {
        uint64_t here = nds_cpu_now(m, m->running);
        uint64_t end = at > here ? at : here;
        set_slice_end(m, end < m->now ? m->now : end);
    }
}

void nds_sync(NdsMachine *m)
{
    if (m->running < 0) return;
    uint64_t here = nds_cpu_now(m, m->running);
    if (here < m->now) here = m->now;
    if (here < m->slice_end) set_slice_end(m, here);
}

void nds_update_irq(NdsMachine *m, int cpu)
{
    m->irq_line[cpu] = (m->ime[cpu] & 1u) && (m->ie[cpu] & m->if_[cpu]);
}

void nds_raise_irq(NdsMachine *m, int cpu, uint32_t bits)
{
    m->if_[cpu] |= bits;
    nds_update_irq(m, cpu);
    /* The other CPU just got something to react to: let it catch up. */
    if (m->running >= 0 && m->running != cpu && (m->ie[cpu] & m->if_[cpu])) nds_sync(m);
}

/* ------------------------------------------------------------------ */
/* Video timing                                                        */
/* ------------------------------------------------------------------ */

/* The match line is nine bits: DISPSTAT bits 8-15 and, as bit 8, bit 7. */
static void vcount_compare(NdsMachine *m)
{
    for (int cpu = 0; cpu < 2; cpu++) {
        uint16_t line = (uint16_t)((m->dispstat[cpu] >> 8) | ((m->dispstat[cpu] & 0x80u) << 1));
        if (m->vcount == line) {
            if (!(m->dispstat[cpu] & 4u) && (m->dispstat[cpu] & 0x20u)) nds_raise_irq(m, cpu, NDS_IRQ_VCOUNT);
            m->dispstat[cpu] |= 4u;
        } else {
            m->dispstat[cpu] &= 0xFFFBu;
        }
    }
}

static void on_hblank(NdsMachine *m)
{
    for (int cpu = 0; cpu < 2; cpu++) {
        m->dispstat[cpu] |= 2u;
        if (m->dispstat[cpu] & 0x10u) nds_raise_irq(m, cpu, NDS_IRQ_HBLANK);
    }
    if (m->vcount < NDS_VISIBLE_LINES) {
        nds_video_render_line(m, m->vcount);
        nds_dma_trigger(m, NDS_ARM9, NDS_DMA_HBLANK);
    }
    nds_schedule(m, NDS_EVENT_HBLANK, UINT64_MAX);
}

static void on_line_end(NdsMachine *m)
{
    m->line_start += NDS_CYCLES_PER_LINE;
    if (m->vcount_write >= 0) {
        m->vcount = (uint16_t)m->vcount_write;
        m->vcount_write = -1;
    } else {
        m->vcount = (uint16_t)((m->vcount + 1u) % NDS_LINES_PER_FRAME);
    }

    for (int cpu = 0; cpu < 2; cpu++) m->dispstat[cpu] &= 0xFFFDu;
    nds_video_line_start(m, m->vcount);
    if (m->vcount == NDS_VISIBLE_LINES) {
        for (int cpu = 0; cpu < 2; cpu++) {
            m->dispstat[cpu] |= 1u;
            if (m->dispstat[cpu] & 0x08u) nds_raise_irq(m, cpu, NDS_IRQ_VBLANK);
            nds_dma_trigger(m, cpu, NDS_DMA_VBLANK);
        }
        m->frame_ready = true;
        m->frame_count++;
        /* The clock's alarms come due by themselves; once a frame is often
         * enough to notice. */
        if (nds_rtc_update(&m->rtc, nds_rtc_now(m))) nds_rtc_line_changed(m);
    } else if (m->vcount == NDS_LINES_PER_FRAME - 1) {
        /* The flag is clear on the last line. */
        for (int cpu = 0; cpu < 2; cpu++) m->dispstat[cpu] &= 0xFFFEu;
    }
    vcount_compare(m);

    nds_schedule(m, NDS_EVENT_HBLANK, m->line_start + NDS_HBLANK_CYCLES);
    nds_schedule(m, NDS_EVENT_LINE_END, m->line_start + NDS_CYCLES_PER_LINE);
}

void nds_process_events(NdsMachine *m)
{
    NDS_PROF_BEGIN(NDS_PROF_EVENTS);
    while (m->next_event <= m->now) {
        int event = 0;
        for (int i = 1; i < NDS_EVENT_COUNT; i++) {
            if (m->event_at[i] < m->event_at[event]) event = i;
        }
        switch (event) {
        case NDS_EVENT_HBLANK: on_hblank(m); break;
        case NDS_EVENT_LINE_END: on_line_end(m); break;
        case NDS_EVENT_TIMER9_0:
        case NDS_EVENT_TIMER9_1:
        case NDS_EVENT_TIMER9_2:
        case NDS_EVENT_TIMER9_3:
            nds_timer_overflow_event(m, NDS_ARM9, event - NDS_EVENT_TIMER9_0);
            break;
        case NDS_EVENT_TIMER7_0:
        case NDS_EVENT_TIMER7_1:
        case NDS_EVENT_TIMER7_2:
        case NDS_EVENT_TIMER7_3:
            nds_timer_overflow_event(m, NDS_ARM7, event - NDS_EVENT_TIMER7_0);
            break;
        case NDS_EVENT_CARD: nds_card_event(m); break;
        case NDS_EVENT_GX:
            nds_schedule(m, NDS_EVENT_GX, UINT64_MAX);
            nds_video_gx_event(m);
            break;
        case NDS_EVENT_SOUND: nds_sound_event(m); break;
        default: nds_schedule(m, event, UINT64_MAX); break;
        }
    }
    NDS_PROF_END();
}

/* ------------------------------------------------------------------ */
/* Run loop                                                            */
/* ------------------------------------------------------------------ */

void nds_prime_pipeline(NdsMachine *m, int cpu)
{
    NdsCpu *c = cpu == NDS_ARM9 ? &m->arm9 : &m->arm7;
    uint32_t pc = nds_cpu_pc(c);
    int size = (c->cpsr & NDS_CPSR_T) ? 2 : 4;
    if (cpu == NDS_ARM9) {
        c->pipe[0] = nds9_peek_code(m, pc, size);
        c->pipe[1] = nds9_peek_code(m, pc + (uint32_t)size, size);
    } else {
        c->pipe[0] = nds7_peek_code(m, pc, size);
        c->pipe[1] = nds7_peek_code(m, pc + (uint32_t)size, size);
    }
    m->pipe_stale[cpu] = false;
}

/* Does a halted CPU go on? When an interrupt it has enabled is requested:
 * IE & IF, whatever IME says. The ARM9, halted by CP15's wait for
 * interrupt, also needs CPSR.I clear: with interrupts masked in the CPU it
 * never wakes. The ARM7, halted through HALTCNT, wakes either way. (The
 * table in BlocksDS's tests/system/irq_handling, taken on a console;
 * melonDS and GBATEK make the ARM9 depend on IME instead.) */
static bool halt_ends(const NdsMachine *m, int cpu)
{
    if (!(m->ie[cpu] & m->if_[cpu])) return false;
    return cpu == NDS_ARM7 || !(m->arm9.cpsr & NDS_CPSR_I);
}

/* Run one CPU to the end of the slice. The end can move earlier while it
 * runs, so it is read again for every instruction. A halted CPU with nothing
 * to wake it just arrives at the end. Each turn of the loop runs either one
 * call into recompiled code, which comes back for exactly the reasons this
 * loop would stop or do something between two instructions, or one
 * interpreted instruction. */
static void run_arm9(NdsMachine *m)
{
    NdsCpu *cpu = &m->arm9;
    m->running = NDS_ARM9;
    while (cpu->cycles < m->cpu_limit[NDS_ARM9]) {
        if (m->stalled[NDS_ARM9]) {
            /* Held by the geometry FIFO: only the engine can release it. */
            cpu->cycles = m->cpu_limit[NDS_ARM9];
            break;
        }
        if (m->halted[NDS_ARM9]) {
            if (!halt_ends(m, NDS_ARM9)) {
                cpu->cycles = m->cpu_limit[NDS_ARM9];
                break;
            }
            m->halted[NDS_ARM9] = false;
        }
        if (m->irq_line[NDS_ARM9] && !(cpu->cpsr & NDS_CPSR_I)) {
            nds9_cpu_irq(cpu);
            m->pipe_stale[NDS_ARM9] = false;
            m->rc_resume[NDS_ARM9] = NULL;
        }
        if (m->rc_program) {
            NDS_PROF_BEGIN(NDS_PROF_NATIVE9);
            bool ran = nds_rc_run(m, NDS_ARM9);
            NDS_PROF_END();
            if (ran) continue;
        }
        NDS_PROF_BEGIN(NDS_PROF_INTERPRET9);
        if (m->pipe_stale[NDS_ARM9]) nds_prime_pipeline(m, NDS_ARM9);
        if (m->step_hook) m->step_hook(m, NDS_ARM9);
        nds9_cpu_step(cpu);
        m->instructions[NDS_ARM9]++;
        m->interpreted[NDS_ARM9]++;
        NDS_PROF_END();
    }
    m->running = -1;
}

static void run_arm7(NdsMachine *m)
{
    NdsCpu *cpu = &m->arm7;
    m->running = NDS_ARM7;
    while (cpu->cycles < m->cpu_limit[NDS_ARM7]) {
        if (m->halted[NDS_ARM7]) {
            if (!halt_ends(m, NDS_ARM7)) {
                cpu->cycles = m->cpu_limit[NDS_ARM7];
                break;
            }
            m->halted[NDS_ARM7] = false;
        }
        if (m->irq_line[NDS_ARM7] && !(cpu->cpsr & NDS_CPSR_I)) {
            nds7_cpu_irq(cpu);
            m->pipe_stale[NDS_ARM7] = false;
            m->rc_resume[NDS_ARM7] = NULL;
        }
        if (m->rc_program) {
            NDS_PROF_BEGIN(NDS_PROF_NATIVE7);
            bool ran = nds_rc_run(m, NDS_ARM7);
            NDS_PROF_END();
            if (ran) continue;
        }
        NDS_PROF_BEGIN(NDS_PROF_INTERPRET7);
        if (m->pipe_stale[NDS_ARM7]) nds_prime_pipeline(m, NDS_ARM7);
        if (m->step_hook) m->step_hook(m, NDS_ARM7);
        nds7_cpu_step(cpu);
        m->instructions[NDS_ARM7]++;
        m->interpreted[NDS_ARM7]++;
        NDS_PROF_END();
    }
    m->running = -1;
}

static bool can_run(const NdsMachine *m, int cpu)
{
    if (m->stalled[cpu]) return false;
    return !m->halted[cpu] || halt_ends(m, cpu);
}

/* A CPU that was asleep when the slice began arrives at its end without
 * running. If the other CPU woke it on the way, the slice was cut short at
 * that moment (nds_raise_irq), so it starts from there in the next slice and
 * not from back where this one began. */
static void sleep_through(NdsMachine *m, int cpu)
{
    NdsCpu *c = cpu == NDS_ARM9 ? &m->arm9 : &m->arm7;
    if (c->cycles < m->cpu_limit[cpu]) c->cycles = m->cpu_limit[cpu];
}

/* One slice: to the next event, or `limit`, or a short step when both CPUs
 * are running so neither gets far ahead of the other. A CPU that is asleep
 * goes second, so the one that is awake sets where the slice ends. */
void nds_run_slice(NdsMachine *m, uint64_t limit)
{
    if (m->next_event <= m->now) nds_process_events(m);

    uint64_t end = m->next_event < limit ? m->next_event : limit;
    bool run9 = can_run(m, NDS_ARM9);
    bool run7 = can_run(m, NDS_ARM7);
    if (run9 && run7 && end - m->now > NDS_SYNC_CYCLES) end = m->now + NDS_SYNC_CYCLES;
    set_slice_end(m, end);

    if (run9) {
        run_arm9(m);
        if (run7) run_arm7(m);
        else sleep_through(m, NDS_ARM7);
    } else {
        if (run7) run_arm7(m);
        else sleep_through(m, NDS_ARM7);
        sleep_through(m, NDS_ARM9);
    }
    m->now = m->slice_end;
    if (m->now >= m->next_event) nds_process_events(m);
}

void nds_run_until(NdsMachine *m, uint64_t until)
{
    while (m->now < until) nds_run_slice(m, until);
}

void nds_run_frame(NdsMachine *m)
{
    m->frame_ready = false;
    while (!m->frame_ready) nds_run_slice(m, m->now + NDS_CYCLES_PER_LINE);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* KEYCNT: an interrupt when any (or, with bit 15, all) of the selected
 * buttons are down. */
static void check_keypad_irq(NdsMachine *m)
{
    uint16_t pressed = (uint16_t)(~m->keyinput & 0x3FFu);
    for (int cpu = 0; cpu < 2; cpu++) {
        uint16_t keycnt = m->keycnt[cpu];
        if (!(keycnt & 0x4000u)) continue;
        uint16_t want = keycnt & 0x3FFu;
        bool hit = (keycnt & 0x8000u) ? (want && (pressed & want) == want) : (pressed & want) != 0;
        if (hit) nds_raise_irq(m, cpu, NDS_IRQ_KEYPAD);
    }
}

static void update_extkeyin(NdsMachine *m, uint32_t pressed)
{
    /* Bits 2, 4 and 5 always read set; X, Y, DEBUG and the pen are active
     * low; bit 7 is set while the lid is closed (GBATEK, "DS Keypad"). The
     * pen bit is the touch controller's interrupt output, which the program
     * can switch off. */
    uint16_t value = 0x007Fu;
    if (pressed & NDS_KEY_X) value &= 0xFFFEu;
    if (pressed & NDS_KEY_Y) value &= 0xFFFDu;
    if (pressed & NDS_KEY_DEBUG) value &= 0xFFF7u;
    if (nds_spi_pen_irq(&m->spi)) value &= 0xFFBFu;
    if (m->lid_closed) value |= 0x80u;
    m->extkeyin = value;
}

static uint32_t pressed_now(const NdsMachine *m)
{
    uint32_t pressed = ~(uint32_t)m->keyinput & 0x3FFu;
    if (!(m->extkeyin & 0x01u)) pressed |= NDS_KEY_X;
    if (!(m->extkeyin & 0x02u)) pressed |= NDS_KEY_Y;
    if (!(m->extkeyin & 0x08u)) pressed |= NDS_KEY_DEBUG;
    return pressed;
}

void nds_set_keys(NdsMachine *m, uint32_t pressed)
{
    m->keyinput = (uint16_t)(~pressed & 0x3FFu);
    update_extkeyin(m, pressed);
    check_keypad_irq(m);
}

void nds_set_touch(NdsMachine *m, bool down, int x, int y)
{
    if (x < 0) x = 0;
    if (x > NDS_SCREEN_WIDTH - 1) x = NDS_SCREEN_WIDTH - 1;
    if (y < 0) y = 0;
    if (y > NDS_SCREEN_HEIGHT - 1) y = NDS_SCREEN_HEIGHT - 1;
    m->touch_down = down;
    m->touch_x = (uint16_t)x;
    m->touch_y = (uint16_t)y;
    /* What the controller's converter reads for that pixel, by the
     * calibration the program will apply to turn it back. */
    uint16_t adc_x = 0, adc_y = 0;
    nds_touch_adc_from_screen(&m->touch_calibration, x, y, &adc_x, &adc_y);
    nds_spi_set_touch(&m->spi, down, adc_x, adc_y);
    update_extkeyin(m, pressed_now(m));
}

void nds_update_extkeyin(NdsMachine *m)
{
    update_extkeyin(m, pressed_now(m));
}

void nds_set_microphone(NdsMachine *m, uint16_t sample12)
{
    nds_spi_set_microphone(&m->spi, sample12);
}

void nds_set_lid(NdsMachine *m, bool closed)
{
    bool was_closed = m->lid_closed;
    m->lid_closed = closed;
    update_extkeyin(m, pressed_now(m));
    /* Opening the lid interrupts the ARM7. */
    if (was_closed && !closed) nds_raise_irq(m, NDS_ARM7, NDS_IRQ_LID);
}
