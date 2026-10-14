/*
 * The machine: creation, ROM loading, power-on, the event scheduler, the
 * RSP catch-up and the run loop.
 *
 * Time is the CPU's cycle counter. Every device keeps the absolute cycle of
 * its next event in m->events; m->next_event is the earliest. The CPU runs
 * until it reaches a sync point (n64_cpu_decode.h) at or after
 * c->next_sync, which is the next event, or 0 when an interrupt may be
 * taken. Interpreted and recompiled code stop at the same sync points, so
 * events land on the same instructions either way.
 *
 * The RSP runs at 2/3 of the CPU clock. It is caught up (n64_sync_rsp)
 * before the CPU touches anything the RSP owns, and by a periodic event
 * while it runs, so the CPU never sees its future and its interrupts arrive
 * within N64_RSP_SLICE cycles of when they happen.
 */
#include "n64.h"

#include <stdlib.h>
#include <string.h>

#include "n64_bus.h"
#include "n64_coverage.h"
#include "n64_io.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t n64_host_ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

uint64_t n64_host_ticks_per_second(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (uint64_t)f.QuadPart;
}
#else
#include <time.h>

uint64_t n64_host_ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

uint64_t n64_host_ticks_per_second(void) { return 1000000000ull; }
#endif

/* How often a running RSP is caught up, in CPU cycles. */
#define N64_RSP_SLICE 512u

static void rsp_interrupt(void *user, int level)
{
    N64Machine *m = user;
    if (level) n64_mi_raise(m, N64_MI_SP);
    else n64_mi_lower(m, N64_MI_SP);
}

static uint32_t rsp_dp_read(void *user, unsigned reg)
{
    N64Machine *m = user;
    return n64_rdp_reg_read(&m->rdp, reg & 7u);
}

static void rsp_dp_write(void *user, unsigned reg, uint32_t value)
{
    N64Machine *m = user;
    if (!m->perf_on) {
        n64_rdp_reg_write(&m->rdp, reg & 7u, value);
        return;
    }
    const uint64_t t0 = n64_host_ticks();
    n64_rdp_reg_write(&m->rdp, reg & 7u, value);
    const uint64_t dt = n64_host_ticks() - t0;
    m->perf_rdp += dt;
    m->perf_rdp_in_rsp += dt;
}

static void rdp_interrupt(void *user, int level)
{
    N64Machine *m = user;
    if (level) n64_mi_raise(m, N64_MI_DP);
    else n64_mi_lower(m, N64_MI_DP);
}

static void dma_ram_written(void *user, uint32_t addr, uint32_t size) { n64_ram_written(user, addr, size); }

static void link_devices(N64Machine *m)
{
    m->cpu.links.rdram = m->rdram;
    m->cpu.links.rdram_size = m->rdram_size;
    m->cpu.links.code_map = m->rc_code_map;
    m->cpu.links.machine = m;

    m->rsp.links.rdram = m->rdram;
    m->rsp.links.rdram_size = m->rdram_size;
    m->rsp.links.user = m;
    m->rsp.links.interrupt = rsp_interrupt;
    m->rsp.links.dp_read = rsp_dp_read;
    m->rsp.links.dp_write = rsp_dp_write;
    m->rsp.links.ram_written = dma_ram_written;

    m->rdp.links.user = m;
    m->rdp.links.interrupt = rdp_interrupt;
}

N64Machine *n64_create(void)
{
    N64Machine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->rdram_size = N64_RDRAM_MAX;
    m->rdram = calloc(1, m->rdram_size);
    m->sram = calloc(1, 0x20000u);
    if (!m->rdram || !m->sram || !n64_rdp_init(&m->rdp, m->rdram, m->rdram_size, m->rsp.dmem)) {
        n64_destroy(m);
        return NULL;
    }
    link_devices(m);
    m->stop_instructions = UINT64_MAX;
    return m;
}

bool n64_set_internal_scale(N64Machine *m, int scale)
{
    if (!m) return false;
    if (scale < 1) scale = 1;
    free(m->large_frame);
    m->large_frame = NULL;
    m->large_scale = 0;
    if (!n64_rdp_set_scale(&m->rdp, scale)) {
        n64_rdp_set_scale(&m->rdp, 1);
        return false;
    }
    if (scale == 1) return true;
    /* Room for the largest picture: PAL's 576 lines, a display pixel a
     * framebuffer pixel. */
    m->large_frame = calloc((size_t)N64_FB_WIDTH * N64_FB_HEIGHT * (size_t)scale * (size_t)scale, sizeof(uint32_t));
    if (!m->large_frame) {
        n64_rdp_set_scale(&m->rdp, 1);
        return false;
    }
    return true;
}

int n64_internal_scale(const N64Machine *m)
{
    return m ? n64_rdp_scale(&m->rdp) : 1;
}

const uint32_t *n64_large_frame(const N64Machine *m, int *width, int *height)
{
    if (!m || !m->large_frame || m->large_scale < 1) return NULL;
    if (width) *width = m->out_width * m->large_scale;
    if (height) *height = m->out_height * m->large_scale;
    return m->large_frame;
}

void n64_destroy(N64Machine *m)
{
    if (!m) return;
    n64_coverage_free(m);
    free(m->large_frame);
    n64_rdp_free(&m->rdp);
    free(m->rdram);
    free(m->rom);
    free(m->sram);
    free(m->isv_log);
    free(m->rc_hash);
    free(m->rc_code_map);
    free(m->rc_granule_fns);
    free(m->rc_granule_list);
    free(m->rc_verified);
    free(m);
}

bool n64_load_rom(N64Machine *m, const uint8_t *data, size_t size)
{
    if (size < 0x1000u || size > 0x10000000u) return false;
    uint8_t *rom = malloc(size);
    if (!rom) return false;
    memcpy(rom, data, size);
    /* z64 is big-endian (0x80371240); v64 swaps bytes in pairs
     * (0x37804012); n64 is little-endian words (0x40123780). */
    const uint32_t magic = n64_be32(rom);
    if (magic == 0x37804012u) {
        for (size_t i = 0; i + 1 < size; i += 2) {
            uint8_t t = rom[i];
            rom[i] = rom[i + 1];
            rom[i + 1] = t;
        }
    } else if (magic == 0x40123780u) {
        for (size_t i = 0; i + 3 < size; i += 4) {
            uint8_t a = rom[i], b = rom[i + 1];
            rom[i] = rom[i + 3];
            rom[i + 1] = rom[i + 2];
            rom[i + 2] = b;
            rom[i + 3] = a;
        }
    }
    if (!n64_cart_parse(&m->cart, rom, size)) {
        free(rom);
        return false;
    }
    free(m->rom);
    m->rom = rom;
    m->rom_size = (uint32_t)size;
    return true;
}

void n64_schedule(N64Machine *m, int ev, uint64_t at)
{
    m->events[ev] = at;
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < N64_EV_COUNT; i++) {
        if (m->events[i] < next) next = m->events[i];
    }
    m->next_event = next;
    n64_update_sync(m);
}

void n64_update_sync(N64Machine *m)
{
    N64Cpu *c = &m->cpu;
    const uint32_t sr = c->cp0_status;
    bool irq = (c->cp0_cause & sr & N64_SR_IM) && (sr & N64_SR_IE) && !(sr & (N64_SR_EXL | N64_SR_ERL));
    c->next_sync = irq ? 0 : m->next_event;
}

void n64_mi_raise(N64Machine *m, uint32_t bits)
{
    m->mi_intr |= bits;
    n64_cpu_set_irq(&m->cpu, N64_CAUSE_IP_MI, (m->mi_intr & m->mi_mask) != 0);
    n64_update_sync(m);
}

void n64_mi_lower(N64Machine *m, uint32_t bits)
{
    m->mi_intr &= ~bits;
    n64_cpu_set_irq(&m->cpu, N64_CAUSE_IP_MI, (m->mi_intr & m->mi_mask) != 0);
    n64_update_sync(m);
}

void n64_sync_rsp(N64Machine *m)
{
    const uint64_t now = m->cpu.cycles;
    if (now <= m->rsp_synced) return;
    if (n64_rsp_halted(&m->rsp)) {
        m->rsp_synced = now;
        m->rsp_frac = 0;
        return;
    }
    /* RSP cycles = CPU cycles * 2 / 3, the remainder carried. */
    uint64_t total = (now - m->rsp_synced) * 2u + m->rsp_frac;
    uint64_t budget = total / 3u;
    m->rsp_frac = (uint32_t)(total % 3u);
    m->rsp_synced = now;
    if (budget) {
        const uint64_t t0 = m->perf_on ? n64_host_ticks() : 0;
        (void)n64_rsp_run(&m->rsp, budget);
        if (m->perf_on) m->perf_rsp += n64_host_ticks() - t0;
    }
    if (n64_rsp_halted(&m->rsp)) m->rsp_frac = 0;
}

/* The RSP was started (SP_STATUS cleared HALT): make sure it gets time. */
void n64_rsp_started(N64Machine *m)
{
    m->rsp_synced = m->cpu.cycles;
    m->rsp_frac = 0;
    n64_schedule(m, N64_EV_RSP, m->cpu.cycles + N64_RSP_SLICE);
}

void n64_process_events(N64Machine *m)
{
    const uint64_t now = m->cpu.cycles;
    while (m->next_event <= now) {
        int ev = 0;
        for (int i = 1; i < N64_EV_COUNT; i++) {
            if (m->events[i] < m->events[ev]) ev = i;
        }
        uint64_t at = m->events[ev];
        m->events[ev] = UINT64_MAX;
        switch (ev) {
        case N64_EV_VI: n64_vi_line(m, at); break;
        case N64_EV_AI: n64_ai_sample(m, at); break;
        case N64_EV_PI: n64_pi_dma_done(m); break;
        case N64_EV_SI: n64_si_dma_done(m); break;
        case N64_EV_TIMER:
            n64_cpu_set_irq(&m->cpu, N64_CAUSE_IP_TIMER, true);
            m->events[N64_EV_TIMER] = n64_cpu_timer_due(&m->cpu);
            break;
        case N64_EV_RSP:
            n64_sync_rsp(m);
            if (!n64_rsp_halted(&m->rsp)) m->events[N64_EV_RSP] = at + N64_RSP_SLICE;
            break;
        case N64_EV_EEPROM: m->eeprom_busy = false; break;
        case N64_EV_FLASH: n64_flash_done(m); break;
        default: break;
        }
        uint64_t next = UINT64_MAX;
        for (int i = 0; i < N64_EV_COUNT; i++) {
            if (m->events[i] < next) next = m->events[i];
        }
        m->next_event = next;
    }
    n64_update_sync(m);
}

/* ---- the bus hooks the CPU core calls ---- */

void n64_bus_timer_changed(N64Cpu *c)
{
    N64Machine *m = N64_MACHINE(c);
    n64_schedule(m, N64_EV_TIMER, n64_cpu_timer_due(c));
}

void n64_bus_interrupt_check(N64Cpu *c) { n64_update_sync(N64_MACHINE(c)); }

void n64_power(N64Machine *m)
{
    N64Cpu *c = &m->cpu;
    memset(c, 0, sizeof *c);
    n64_cpu_reset(c);
    n64_rsp_reset(&m->rsp);
    n64_rdp_reset(&m->rdp);
    link_devices(m);
    memset(m->rdram, 0, m->rdram_size);
    memset(m->pif_ram, 0, sizeof m->pif_ram);
    m->mi_mode = m->mi_intr = m->mi_mask = 0;
    memset(m->vi_regs, 0, sizeof m->vi_regs);
    m->vi_line = m->vi_field = m->vi_frac = 0;
    m->ai_count = 0;
    m->ai_enable = false;
    m->ai_dacrate = m->ai_bitrate = 0;
    m->ai_frac = 0;
    m->ai_left = m->ai_right = 0;
    m->ai_phase = 0;
    m->audio_head = m->audio_tail = 0;
    m->pi_dram_addr = m->pi_cart_addr = m->pi_rd_len = m->pi_wr_len = m->pi_status = 0;
    memset(m->pi_bsd, 0, sizeof m->pi_bsd);
    n64_rdram_power(m);
    m->pif_state = N64_PIF_RUN;
    m->pif_rom_lockout = false;
    m->si_dram_addr = m->si_status = 0;
    m->si_pending = 0;
    m->eeprom_busy = false;
    memset(&m->flash, 0, sizeof m->flash);
    m->isv_len = 0;
    if (m->isv_log) m->isv_log[0] = '\0';
    m->rsp_synced = 0;
    m->rsp_frac = 0;
    m->frame_count = 0;
    m->frame_ready = false;
    m->out_width = 640;
    m->out_height = m->cart.pal ? 576 : 480;
    memset(m->framebuffer, 0, sizeof m->framebuffer);
    for (int i = 0; i < N64_EV_COUNT; i++) m->events[i] = UINT64_MAX;
    m->next_event = UINT64_MAX;
    if (!m->controllers) m->controllers = 1;
    n64_flash_reset(m);

    n64_boot(m);

    n64_schedule(m, N64_EV_VI, c->cycles + n64_vi_line_cycles(m));
    n64_schedule(m, N64_EV_AI, c->cycles + n64_ai_period(m));
    n64_schedule(m, N64_EV_TIMER, n64_cpu_timer_due(c));
}

/* ---- the run loop ---- */

#include "n64_recomp.h"

/* The interpreter's side of n64_rc_idle: called at a sync point where the
 * delay slot just run returned to the branch before it. Held to
 * stop_instructions, which the differential runner sets. */
static void idle_skip(N64Machine *m)
{
    N64Cpu *c = &m->cpu;
    uint32_t pa;
    uint64_t n = n64_idle_turns(c);
    /* Not n64_fast_ram: coverage turns the CPU's RDRAM shortcut off, and a
     * profile is where an idle loop left to the interpreter costs most. */
    if (!n || !n64_cpu_translate_quiet(c, c->pc, &pa) || (uint64_t)pa + 8u > m->rdram_size) return;
    if (!n64_is_idle_loop(c->pc, n64_be32(m->rdram + pa), n64_be32(m->rdram + pa + 4u))) return;
    if (m->stop_instructions != UINT64_MAX) {
        const uint64_t room = m->stop_instructions > c->instructions ? (m->stop_instructions - c->instructions) / 2u : 0u;
        if (n > room) n = room;
    }
    c->cycles += 2u * n;
    c->instructions += 2u * n;
    c->idled += 2u * n;
}

void n64_run_slice(N64Machine *m, uint64_t limit)
{
    N64Cpu *c = &m->cpu;
    if (c->cycles >= m->next_event) n64_process_events(m);
    if (n64_cpu_poll_interrupt(c)) {
        n64_update_sync(m);
        return;
    }
    if (m->rc_program && !c->delay_pending) {
        N64RcFunc fn = n64_rc_lookup(m, c->pc);
        if (fn) {
            fn(c, (uint32_t)c->pc);
            return;
        }
    }
    const uint64_t stop = limit == UINT64_MAX ? UINT64_MAX : c->cycles + limit;
    bool entered = true;
    for (;;) {
        if (m->trace) m->trace(m);
        if (m->coverage && entered && !c->delay_pending) n64_coverage_entry(m);
        /* The next instruction is an entry when this one ends a transfer:
         * a delay slot, an exception or an ERET. */
        const uint64_t at = c->pc, next = at + 4u;
        const bool sync = n64_cpu_step(c);
        entered = c->delay_pending != 0 || c->pc != next;
        /* Due events run in place at a sync point, exactly as generated code
         * runs them (n64_rc_sync); the slice ends when an interrupt can be
         * taken or a frame is complete. */
        if (sync && c->cycles >= c->next_sync) {
            if (n64_rc_sync(c) || c->cycles >= stop) break;
        }
        if (sync && c->pc + 4u == at) idle_skip(m);
        if (c->instructions >= m->stop_instructions && !c->delay_pending) break;
        /* Recompiled code may take over at the next branch target. */
        if (sync && m->rc_program) break;
    }
}

void n64_run_frame(N64Machine *m)
{
    m->frame_ready = false;
    while (!m->frame_ready) n64_run_slice(m, UINT64_MAX);
}

void n64_set_buttons(N64Machine *m, int port, uint16_t buttons, int8_t x, int8_t y)
{
    if (port < 0 || port > 3) return;
    m->buttons[port] = buttons;
    m->stick_x[port] = x;
    m->stick_y[port] = y;
}

size_t n64_read_audio(N64Machine *m, int16_t *out, size_t max)
{
    size_t n = 0;
    while (n < max && m->audio_tail != m->audio_head) {
        out[2 * n] = m->audio_ring[2 * m->audio_tail];
        out[2 * n + 1] = m->audio_ring[2 * m->audio_tail + 1];
        m->audio_tail = (m->audio_tail + 1u) % N64_AUDIO_RING;
        n++;
    }
    return n;
}

/* ---- saves ---- */

static size_t battery_size(const N64Machine *m)
{
    if (m->cart.flash) return 0x20000u;
    if (m->cart.sram_size) return m->cart.sram_size;
    if (m->cart.eeprom_kbit) return m->cart.eeprom_kbit == 16 ? 2048u : 512u;
    return 0;
}

size_t n64_save_size(const N64Machine *m) { return battery_size(m) + sizeof m->mempak; }

void n64_save_export(const N64Machine *m, uint8_t *out)
{
    size_t b = battery_size(m);
    if (m->cart.flash || m->cart.sram_size) memcpy(out, m->sram, b);
    else if (b) memcpy(out, m->eeprom, b);
    memcpy(out + b, m->mempak, sizeof m->mempak);
}

void n64_save_import(N64Machine *m, const uint8_t *data, size_t size)
{
    size_t b = battery_size(m);
    if (size >= b && b) {
        if (m->cart.flash || m->cart.sram_size) memcpy(m->sram, data, b);
        else memcpy(m->eeprom, data, b);
    }
    if (size >= b + sizeof m->mempak) memcpy(m->mempak, data + b, sizeof m->mempak);
}
