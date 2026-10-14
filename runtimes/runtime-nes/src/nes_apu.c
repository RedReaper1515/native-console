/*
 * NES APU (Ricoh 2A03, NTSC): channels, frame counter, DMC, mixer and the
 * output stage. See nes_apu.h for the timing conventions.
 *
 * The channels (pulse, sweep, envelope, triangle, noise, DMC output unit
 * and memory reader), the length counters and the mixer are written from
 * the nesdev wiki (APU, APU Pulse, APU Sweep, APU Envelope, APU Triangle,
 * APU Noise, APU DMC, APU Length Counter, APU Frame Counter, APU Mixer,
 * DMA, CPU power up state). The hardware tables (length counter loads,
 * noise and DMC periods) are transcribed from the same pages.
 *
 * Ported from ares (ISC license, Near et al.), ares/fc/apu/framecounter.cpp
 * and length.cpp at commit a776c509: the frame counter's two-stage write
 * delay that advances every second cycle, the delayed clear of the frame IRQ
 * flag after a $4015 read, the three-step IRQ flag whose third step obeys
 * the inhibit bit, and the idea of holding back a length counter's halt
 * and reload writes made in the cycle that clocks it. Rewritten in C
 * around a step position instead of ares's step/period counters.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#include "nes_apu.h"

#include <string.h>

/* ---- Hardware tables --------------------------------------------------- */

const uint8_t nes_apu_length_table[32] = {
    10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
    12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

const uint16_t nes_apu_noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

const uint16_t nes_apu_dmc_period[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};

/* Duty sequences, bit n = the output at sequencer step n. The sequencer
 * starts at step 0 and counts down, which reads the wiki's waveforms
 * 01000000, 01100000, 01111000, 10011111. */
static const uint8_t duty_bits[4] = { 0x80, 0xC0, 0xF0, 0x3F };

/* Frame sequence positions (steps since the restart step). */
enum {
    FC_QUARTER_1 = 7457,
    FC_HALF_1 = 14913,
    FC_QUARTER_3 = 22371,
    FC_IRQ_FIRST = 29828,
    FC_HALF_2_MODE0 = 29829,
    FC_WRAP_MODE0 = 29830,
    FC_HALF_2_MODE1 = 37281,
    FC_WRAP_MODE1 = 37282
};

/* ---- Output stage ------------------------------------------------------ */

/* The decimator works in phase units: one sample at four times the output
 * rate is PHASE_ONE units long and a CPU cycle is 11 * 4 * rate units
 * (the CPU clock is 19687500 / 11 Hz), so the ratio is exact. */
#define PHASE_ONE 19687500u
#define OVERSAMPLE 4u
#define LEVEL_SHIFT 14 /* DAC level (Q16) to filter precision (Q30) */

/* floor(d * k / 2^24) without shifting a negative value. */
static inline int64_t mul_k(int64_t d, uint32_t k)
{
    const int64_t p = d * (int64_t)k;
    return p >= 0 ? p >> 24 : -((-p + 0xFFFFFF) >> 24);
}

/* The coefficient of a first-order filter with corner `hz` at sample rate
 * `fs`: 1 - exp(-x), x = 2 pi hz / fs, in Q24. Integer arithmetic only:
 * 2 pi = 710 / 113 and exp(-x) = (12 - 6x + x^2) / (12 + 6x + x^2), which
 * is within 3e-5 for the 14 kHz filter at 48 kHz output (x = 0.46) and far
 * closer for the two high-pass filters. */
static uint32_t filter_k(uint32_t hz, uint32_t fs)
{
    const uint64_t x = ((uint64_t)(710u * hz) << 28) / ((uint64_t)113u * fs); /* Q28 */
    const uint64_t x2 = (x * x) >> 28;
    return (uint32_t)(((12u * x) << 24) / (x2 + 6u * x + ((uint64_t)12u << 28)));
}

static inline unsigned pulse_level(const NesApuPulse *p, const NesApuLength *l)
{
    if (!l->counter || p->period < 8u || !((duty_bits[p->duty] >> p->step) & 1u)) return 0;
    /* The sweep unit's adder overflowing mutes the channel even when the
     * sweep is disabled (only possible when adding). */
    if (!p->sweep_negate && (unsigned)p->period + ((unsigned)p->period >> p->sweep_shift) > 0x7FFu) return 0;
    return p->env.constant ? p->env.volume : p->env.decay;
}

static inline unsigned triangle_level(const NesApuTriangle *t)
{
    return t->step < 16u ? 15u - t->step : t->step - 16u;
}

static inline unsigned noise_level(const NesApuNoise *n, const NesApuLength *l)
{
    if (!l->counter || (n->lfsr & 1u)) return 0;
    return n->env.constant ? n->env.volume : n->env.decay;
}

/* The DAC's output for the current state, 0..65536. */
static inline uint32_t dac_level(const NesApu *a)
{
    unsigned p1 = pulse_level(&a->pulse[0], &a->len[0]);
    unsigned p2 = pulse_level(&a->pulse[1], &a->len[1]);
    unsigned t = triangle_level(&a->tri);
    unsigned n = noise_level(&a->noise, &a->len[3]);
    unsigned d = a->dmc.level;
    if (a->mute) {
        if (a->mute & (1u << NES_APU_PULSE1)) p1 = 0;
        if (a->mute & (1u << NES_APU_PULSE2)) p2 = 0;
        if (a->mute & (1u << NES_APU_TRIANGLE)) t = 0;
        if (a->mute & (1u << NES_APU_NOISE)) n = 0;
        if (a->mute & (1u << NES_APU_DMC)) d = 0;
    }
    return (uint32_t)nes_apu_pulse_dac[p1 + p2] + nes_apu_tnd_dac[(d << 8) | (t << 4) | n];
}

static void ring_push(NesApu *a, int16_t v)
{
    const uint32_t mask = NES_APU_RING_SAMPLES - 1u;
    if (a->ring_count == NES_APU_RING_SAMPLES) {
        a->ring_read = (a->ring_read + 1u) & mask;
        a->ring_count--;
        a->overruns++;
    }
    a->ring[(a->ring_read + a->ring_count) & mask] = v;
    a->ring_count++;
    a->samples++;
}

/* One sample at four times the output rate (Q30): the console's filters,
 * then the FIR low-pass and the 4:1 decimation. */
static void output_sample(NesApu *a, int64_t x)
{
    /* High-pass 90 Hz and 440 Hz: each subtracts a one-pole tracker,
     * taken halfway through its update so the gain is 1 at the top of
     * the band (and exactly 0 at DC once the tracker has settled). */
    int64_t before = a->hp1;
    a->hp1 += mul_k(x - before, a->hp1_k);
    x -= (before + a->hp1) / 2;
    before = a->hp2;
    a->hp2 += mul_k(x - before, a->hp2_k);
    x -= (before + a->hp2) / 2;
    /* Low-pass 14 kHz. */
    a->lp += mul_k(x - a->lp, a->lp_k);

    const uint32_t mask = NES_APU_FIR_TAPS - 1u;
    a->fir[a->fir_pos] = a->lp;
    a->fir_pos = (a->fir_pos + 1u) & mask;
    if (++a->fir_phase < OVERSAMPLE) return;
    a->fir_phase = 0;

    int64_t sum = 0;
    for (uint32_t k = 0; k < NES_APU_FIR_TAPS; k++) sum += a->fir[(a->fir_pos + k) & mask] * nes_apu_fir[k];
    /* Q30 samples times taps that sum to 2^15; full scale 1.0 = 32768.
     * The division truncates toward zero, so the sub-unit residue the
     * filters settle on is exactly zero in the output. */
    sum /= (int64_t)1 << 30;
    ring_push(a, (int16_t)(sum > 32767 ? 32767 : (sum < -32768 ? -32768 : sum)));
}

/* Add one CPU cycle at the current DAC level to the averaging window. A
 * cycle that straddles the window's end is split in proportion. */
static inline void output_cycle(NesApu *a)
{
    const uint32_t level = dac_level(a);
    uint32_t phase = a->rs_phase + a->rs_inc;
    if (phase < PHASE_ONE) {
        a->rs_acc += (uint64_t)level * a->rs_inc;
        a->rs_phase = phase;
        return;
    }
    a->rs_acc += (uint64_t)level * (PHASE_ONE - a->rs_phase);
    output_sample(a, (int64_t)((a->rs_acc << LEVEL_SHIFT) / PHASE_ONE));
    phase -= PHASE_ONE;
    a->rs_acc = (uint64_t)level * phase;
    a->rs_phase = phase;
}

/* Start the output stage settled on the current level, as if it had been
 * held forever: no power-on thump. */
static void output_restart(NesApu *a)
{
    a->rs_acc = 0;
    a->rs_phase = 0;
    a->hp1 = (int64_t)dac_level(a) << LEVEL_SHIFT;
    a->hp2 = 0;
    a->lp = 0;
    memset(a->fir, 0, sizeof a->fir);
    a->fir_pos = 0;
    a->fir_phase = 0;
    a->ring_read = 0;
    a->ring_count = 0;
}

void nes_apu_set_rate(NesApu *a, uint32_t hz)
{
    if (hz < NES_APU_MIN_RATE) hz = NES_APU_MIN_RATE;
    if (hz > NES_APU_MAX_RATE) hz = NES_APU_MAX_RATE;
    if (hz == a->rate) return;
    a->rate = hz;
    a->rs_inc = 11u * OVERSAMPLE * hz;
    a->hp1_k = filter_k(90u, OVERSAMPLE * hz);
    a->hp2_k = filter_k(440u, OVERSAMPLE * hz);
    a->lp_k = filter_k(14000u, OVERSAMPLE * hz);
    output_restart(a);
}

void nes_apu_set_mute(NesApu *a, unsigned mask) { a->mute = (uint8_t)(mask & ((1u << NES_APU_CHANNELS) - 1u)); }

size_t nes_apu_samples_available(const NesApu *a) { return a->ring_count; }

size_t nes_apu_read_samples(NesApu *a, int16_t *dst, size_t max)
{
    const uint32_t mask = NES_APU_RING_SAMPLES - 1u;
    size_t n = a->ring_count < max ? a->ring_count : max;
    for (size_t i = 0; i < n; i++) dst[i] = a->ring[(a->ring_read + i) & mask];
    a->ring_read = (a->ring_read + (uint32_t)n) & mask;
    a->ring_count -= (uint32_t)n;
    return n;
}

unsigned nes_apu_channel_level(const NesApu *a, unsigned channel)
{
    switch (channel) {
    case NES_APU_PULSE1: return pulse_level(&a->pulse[0], &a->len[0]);
    case NES_APU_PULSE2: return pulse_level(&a->pulse[1], &a->len[1]);
    case NES_APU_TRIANGLE: return triangle_level(&a->tri);
    case NES_APU_NOISE: return noise_level(&a->noise, &a->len[3]);
    case NES_APU_DMC: return a->dmc.level;
    default: return 0;
    }
}

/* ---- Quarter and half frame units -------------------------------------- */

static void envelope_clock(NesApuEnvelope *e)
{
    if (e->start) {
        e->start = 0;
        e->decay = 15;
        e->divider = e->volume;
    } else if (e->divider == 0) {
        e->divider = e->volume;
        if (e->decay) e->decay--;
        else if (e->loop) e->decay = 15;
    } else {
        e->divider--;
    }
}

/* The period the sweep unit would write: pulse 1 subtracts in ones'
 * complement (one more than the shifted value), pulse 2 in two's
 * complement; a negative result is zero. */
static unsigned sweep_target(const NesApuPulse *p, unsigned channel)
{
    const unsigned period = p->period;
    const unsigned change = period >> p->sweep_shift;
    if (!p->sweep_negate) return period + change;
    const unsigned sub = change + (channel == 0 ? 1u : 0u);
    return period > sub ? period - sub : 0;
}

static void sweep_clock(NesApuPulse *p, unsigned channel)
{
    const unsigned target = sweep_target(p, channel);
    if (p->sweep_divider == 0 && p->sweep_enable && p->sweep_shift && p->period >= 8u && target <= 0x7FFu) {
        p->period = (uint16_t)target;
    }
    if (p->sweep_divider == 0 || p->sweep_reload) {
        p->sweep_divider = p->sweep_period;
        p->sweep_reload = 0;
    } else {
        p->sweep_divider--;
    }
}

/* Envelopes and the triangle's linear counter. */
static void clock_quarter(NesApu *a)
{
    envelope_clock(&a->pulse[0].env);
    envelope_clock(&a->pulse[1].env);
    envelope_clock(&a->noise.env);

    NesApuTriangle *t = &a->tri;
    if (t->reload_flag) t->linear = t->linear_reload;
    else if (t->linear) t->linear--;
    if (!t->control) t->reload_flag = 0;
}

/* Length counters and sweep units. A reload written in this cycle is
 * dropped when the clock decrements the counter. */
static void clock_half(NesApu *a)
{
    for (unsigned i = 0; i < 4; i++) {
        NesApuLength *l = &a->len[i];
        if (l->counter && !l->halt) {
            l->counter--;
            l->reload_deferred = 0;
        }
    }
    sweep_clock(&a->pulse[0], 0);
    sweep_clock(&a->pulse[1], 1);
}

/* Whether the step that follows the current access clocks the length
 * counters (the frame counter part of apu_step, looked at in advance). */
static bool half_clock_this_step(const NesApu *a)
{
    if (a->fc_restart_now || (!(a->cycle & 1u) && a->fc_restart[0])) return a->fc_mode != 0;
    const uint32_t pos = a->fc_pos + 1u;
    return pos == FC_HALF_1 || pos == (a->fc_mode ? (uint32_t)FC_HALF_2_MODE1 : (uint32_t)FC_HALF_2_MODE0);
}

static void length_set_halt(NesApu *a, unsigned i, uint8_t halt)
{
    NesApuLength *l = &a->len[i];
    if (half_clock_this_step(a)) {
        l->halt_next = halt;
        l->halt_deferred = 1;
        a->len_deferred = 1;
    } else {
        l->halt = halt;
    }
}

static void length_load(NesApu *a, unsigned i, unsigned index)
{
    NesApuLength *l = &a->len[i];
    if (!l->enabled) return;
    if (half_clock_this_step(a)) {
        l->reload_index = (uint8_t)index;
        l->reload_deferred = 1;
        a->len_deferred = 1;
    } else {
        l->counter = nes_apu_length_table[index];
    }
}

static void length_apply_deferred(NesApu *a)
{
    for (unsigned i = 0; i < 4; i++) {
        NesApuLength *l = &a->len[i];
        if (l->halt_deferred) {
            l->halt = l->halt_next;
            l->halt_deferred = 0;
        }
        if (l->reload_deferred) {
            l->counter = nes_apu_length_table[l->reload_index];
            l->reload_deferred = 0;
        }
    }
    a->len_deferred = 0;
}

/* ---- DMC ---------------------------------------------------------------- */

/* The memory reader: fetch the next sample byte into the buffer. The
 * machine stops the CPU for the DMA; the byte is taken at once, which no
 * CPU read can tell from the hardware (there it arrives on the DMA's last
 * cycle, and the CPU is halted until then). */
static void dmc_fetch(NesApu *a)
{
    NesApuDmc *d = &a->dmc;
    d->buffer = nes_apu_dmc_read(a, d->addr);
    d->buffer_full = 1;
    d->addr = d->addr == 0xFFFFu ? 0x8000u : (uint16_t)(d->addr + 1u);
    if (--d->remaining == 0) {
        if (d->loop) {
            d->addr = d->sample_addr;
            d->remaining = d->sample_len;
        } else if (d->irq_enable) {
            d->irq = 1;
        }
    }
}

/* One clock of the output unit. */
static void dmc_clock(NesApu *a)
{
    NesApuDmc *d = &a->dmc;
    if (!d->silence) {
        if (d->shift & 1u) {
            if (d->level <= 125u) d->level += 2u;
        } else if (d->level >= 2u) {
            d->level -= 2u;
        }
    }
    d->shift >>= 1;
    if (--d->bits) return;
    d->bits = 8;
    if (!d->buffer_full) {
        d->silence = 1;
        return;
    }
    d->silence = 0;
    d->shift = d->buffer;
    d->buffer_full = 0;
    if (d->remaining && !d->fetch_delay) dmc_fetch(a);
}

/* ---- One CPU cycle ------------------------------------------------------ */

static void frame_event(NesApu *a, uint32_t pos)
{
    if (pos == FC_QUARTER_1 || pos == FC_QUARTER_3) {
        clock_quarter(a);
    } else if (pos == FC_HALF_1) {
        clock_quarter(a);
        clock_half(a);
    } else if (!a->fc_mode) {
        if (pos == FC_IRQ_FIRST) {
            a->frame_irq = 1;
        } else if (pos == FC_HALF_2_MODE0) {
            a->frame_irq = 1;
            clock_quarter(a);
            clock_half(a);
        } else if (pos == FC_WRAP_MODE0) {
            a->frame_irq = !a->fc_inhibit;
            a->fc_pos = 0;
        }
    } else if (pos == FC_HALF_2_MODE1) {
        clock_quarter(a);
        clock_half(a);
    }
    /* The 5-step sequence's end; also keeps a position left over from the
     * other mode bounded until the pending restart arrives. */
    if (pos >= FC_WRAP_MODE1) a->fc_pos = 0;
}

static void apu_step(NesApu *a)
{
    const bool get = (a->cycle & 1u) != 0;

    output_cycle(a);

    /* The DMC's load fetch, a few cycles after the $4015 write. */
    NesApuDmc *d = &a->dmc;
    if (d->fetch_delay && --d->fetch_delay == 0 && d->remaining && !d->buffer_full) dmc_fetch(a);

    /* Timers. The triangle's runs at the CPU rate, the others at the APU
     * rate (every second cycle); each counts period..0 and acts on the
     * step after 0. */
    NesApuTriangle *t = &a->tri;
    if (t->timer == 0) {
        t->timer = t->period;
        if (a->len[2].counter && t->linear) t->step = (t->step + 1u) & 31u;
    } else {
        t->timer--;
    }
    if (get) {
        for (unsigned i = 0; i < 2; i++) {
            NesApuPulse *p = &a->pulse[i];
            if (p->timer == 0) {
                p->timer = p->period;
                p->step = (p->step - 1u) & 7u;
            } else {
                p->timer--;
            }
        }
        NesApuNoise *n = &a->noise;
        if (n->timer == 0) {
            n->timer = (uint16_t)(nes_apu_noise_period[n->rate] / 2u - 1u);
            const unsigned feedback = (n->lfsr ^ (n->lfsr >> (n->mode ? 6 : 1))) & 1u;
            n->lfsr = (uint16_t)((n->lfsr >> 1) | (feedback << 14));
        } else {
            n->timer--;
        }
        if (d->timer == 0) {
            d->timer = (uint16_t)(nes_apu_dmc_period[d->rate] / 2u - 1u);
            dmc_clock(a);
        } else {
            d->timer--;
        }
    }

    /* Frame counter: a $4017 write restarts the sequence two put steps
     * later, power and reset in their first step. */
    bool restart = a->fc_restart_now != 0;
    a->fc_restart_now = 0;
    if (!get) {
        if (a->irq_clear_pending) {
            a->irq_clear_pending = 0;
            a->frame_irq = 0;
        }
        restart |= a->fc_restart[0] != 0;
        a->fc_restart[0] = a->fc_restart[1];
        a->fc_restart[1] = 0;
    }
    if (restart) {
        a->fc_pos = 0;
        if (a->fc_mode) {
            clock_quarter(a);
            clock_half(a);
        }
    } else {
        const uint32_t pos = ++a->fc_pos;
        if (pos == FC_QUARTER_1 || pos == FC_HALF_1 || pos == FC_QUARTER_3 || pos >= FC_IRQ_FIRST) frame_event(a, pos);
    }

    if (a->len_deferred) length_apply_deferred(a);

    a->cycle++;
}

void nes_apu_run(NesApu *a, uint32_t cpu_cycles)
{
    while (cpu_cycles--) apu_step(a);
}

/* ---- Registers ---------------------------------------------------------- */

bool nes_apu_irq(const NesApu *a) { return (a->frame_irq && !a->fc_inhibit) || a->dmc.irq; }

static uint8_t status(const NesApu *a)
{
    unsigned v = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (a->len[i].counter) v |= 1u << i;
    }
    if (a->dmc.remaining) v |= 0x10u;
    if (a->frame_irq) v |= 0x40u;
    if (a->dmc.irq) v |= 0x80u;
    return (uint8_t)v;
}

uint8_t nes_apu_peek(const NesApu *a, uint16_t addr) { return addr == 0x4015u ? status(a) : 0; }

uint8_t nes_apu_read(NesApu *a, uint16_t addr)
{
    if (addr != 0x4015u) return 0;
    a->irq_clear_pending = 1;
    return status(a);
}

static void write_status(NesApu *a, uint8_t value)
{
    for (unsigned i = 0; i < 4; i++) {
        NesApuLength *l = &a->len[i];
        l->enabled = (value >> i) & 1u;
        if (!l->enabled) {
            l->counter = 0;
            l->reload_deferred = 0;
        }
    }
    NesApuDmc *d = &a->dmc;
    d->irq = 0;
    if (!(value & 0x10u)) {
        /* Stop: no bytes remain; the buffered byte and the bits already in
         * the shift register still play. A load not yet made is dropped. */
        d->remaining = 0;
        d->fetch_delay = 0;
        return;
    }
    const bool start = d->remaining == 0;
    if (start) {
        d->addr = d->sample_addr;
        d->remaining = d->sample_len;
    }
    if ((start || !d->buffer_full) && !d->fetch_delay) {
        /* The memory reader starts working in the even step that puts a
         * halt on the get cycle of the second APU cycle after this write.
         * With the buffer empty that is the load fetch. With a byte still
         * in the buffer nothing happens then, unless the output unit takes
         * the byte before: the reload it would ask for at once waits for
         * that step (AccuracyCoin, Delta Modulation Channel L to N: a
         * sample restarted 0 to 2 cycles before the output cycle ends). */
        d->fetch_delay = (a->cycle & 1u) ? 4 : 3;
    }
}

static void write_frame_counter(NesApu *a, uint8_t value)
{
    a->fc_reg = value;
    a->fc_mode = value >> 7;
    a->fc_inhibit = (value >> 6) & 1u;
    if (a->fc_inhibit) a->frame_irq = 0;
    a->fc_restart[1] = 1;
}

void nes_apu_write(NesApu *a, uint16_t addr, uint8_t value)
{
    switch (addr) {
    case 0x4000:
    case 0x4004: {
        NesApuPulse *p = &a->pulse[(addr >> 2) & 1u];
        p->duty = value >> 6;
        p->env.loop = (value >> 5) & 1u;
        p->env.constant = (value >> 4) & 1u;
        p->env.volume = value & 15u;
        length_set_halt(a, (addr >> 2) & 1u, (value >> 5) & 1u);
        break;
    }
    case 0x4001:
    case 0x4005: {
        NesApuPulse *p = &a->pulse[(addr >> 2) & 1u];
        p->sweep_enable = value >> 7;
        p->sweep_period = (value >> 4) & 7u;
        p->sweep_negate = (value >> 3) & 1u;
        p->sweep_shift = value & 7u;
        p->sweep_reload = 1;
        break;
    }
    case 0x4002:
    case 0x4006: {
        NesApuPulse *p = &a->pulse[(addr >> 2) & 1u];
        p->period = (uint16_t)((p->period & 0x700u) | value);
        break;
    }
    case 0x4003:
    case 0x4007: {
        NesApuPulse *p = &a->pulse[(addr >> 2) & 1u];
        p->period = (uint16_t)((p->period & 0x0FFu) | ((value & 7u) << 8));
        p->step = 0; /* the sequencer restarts; the timer is left alone */
        p->env.start = 1;
        length_load(a, (addr >> 2) & 1u, value >> 3);
        break;
    }
    case 0x4008:
        a->tri.control = value >> 7;
        a->tri.linear_reload = value & 0x7Fu;
        length_set_halt(a, 2, value >> 7);
        break;
    case 0x400A: a->tri.period = (uint16_t)((a->tri.period & 0x700u) | value); break;
    case 0x400B:
        a->tri.period = (uint16_t)((a->tri.period & 0x0FFu) | ((value & 7u) << 8));
        a->tri.reload_flag = 1;
        length_load(a, 2, value >> 3);
        break;
    case 0x400C:
        a->noise.env.loop = (value >> 5) & 1u;
        a->noise.env.constant = (value >> 4) & 1u;
        a->noise.env.volume = value & 15u;
        length_set_halt(a, 3, (value >> 5) & 1u);
        break;
    case 0x400E:
        a->noise.mode = value >> 7;
        a->noise.rate = value & 15u;
        break;
    case 0x400F:
        a->noise.env.start = 1;
        length_load(a, 3, value >> 3);
        break;
    case 0x4010:
        a->dmc.irq_enable = value >> 7;
        a->dmc.loop = (value >> 6) & 1u;
        a->dmc.rate = value & 15u;
        if (!a->dmc.irq_enable) a->dmc.irq = 0;
        break;
    case 0x4011: a->dmc.level = value & 0x7Fu; break;
    case 0x4012: a->dmc.sample_addr = (uint16_t)(0xC000u | ((unsigned)value << 6)); break;
    case 0x4013: a->dmc.sample_len = (uint16_t)(((unsigned)value << 4) | 1u); break;
    case 0x4015: write_status(a, value); break;
    case 0x4017: write_frame_counter(a, value); break;
    default: break;
    }
}

/* ---- Scheduling --------------------------------------------------------- */

uint32_t nes_apu_cycles_until_event(const NesApu *a)
{
    uint64_t n = NES_APU_EVENT_MAX;

    /* The frame IRQ flag: raised at FC_IRQ_FIRST in 4-step mode (a pending
     * restart only moves that later), dropped in the next put step after a
     * $4015 read. An inhibited flag does not reach the line. */
    if (!a->fc_inhibit) {
        if (a->frame_irq) {
            if (a->irq_clear_pending) n = (a->cycle & 1u) ? 2u : 1u;
        } else if (!a->fc_mode) {
            n = a->fc_pos < FC_IRQ_FIRST ? (uint64_t)FC_IRQ_FIRST - a->fc_pos : 1u;
        }
    }

    /* The DMC's next fetch (the last one of a sample is also where its
     * IRQ is raised). */
    const uint32_t fetch = nes_apu_cycles_until_dmc(a);
    if (fetch < n) n = fetch;
    return (uint32_t)(n ? n : 1u);
}

/* A load counting down, or the output unit emptying the buffer. */
uint32_t nes_apu_cycles_until_dmc(const NesApu *a)
{
    uint64_t n = NES_APU_EVENT_MAX;
    const NesApuDmc *d = &a->dmc;
    if (d->fetch_delay && d->remaining && !d->buffer_full) n = d->fetch_delay;
    if (d->buffer_full && d->remaining) {
        /* The timer acts on odd steps: the first clock is 2 * timer steps
         * after the next odd one, and the output cycle ends on clock
         * number `bits`. A reload asked for while the memory reader is
         * still starting waits for it. */
        const uint64_t until_clock = (a->cycle & 1u ? 0u : 1u) + 2u * (uint64_t)d->timer;
        uint64_t until_empty = until_clock + (uint64_t)(d->bits - 1u) * nes_apu_dmc_period[d->rate] + 1u;
        if (until_empty < d->fetch_delay) until_empty = d->fetch_delay;
        if (until_empty < n) n = until_empty;
    }
    return (uint32_t)n;
}

/* ---- Power and reset ---------------------------------------------------- */

void nes_apu_power(NesApu *a)
{
    memset(a, 0, sizeof *a);
    a->noise.lfsr = 1;
    a->noise.timer = (uint16_t)(nes_apu_noise_period[0] / 2u - 1u);
    a->dmc.timer = (uint16_t)(nes_apu_dmc_period[0] / 2u - 1u);
    a->dmc.bits = 8;
    a->dmc.silence = 1;
    a->dmc.sample_addr = 0xC000u;
    a->dmc.sample_len = 1;
    a->fc_restart_now = 1;
    nes_apu_set_rate(a, NES_APU_DEFAULT_RATE);
}

void nes_apu_reset(NesApu *a)
{
    write_status(a, 0);
    for (unsigned i = 0; i < 4; i++) a->len[i].halt_deferred = 0;
    a->len_deferred = 0;

    /* $4017 as last written: the sequence restarts in the next step,
     * which puts the equivalent write 10 or 11 cycles before the first
     * instruction after the CPU's 7-cycle reset sequence (blargg's
     * apu_reset tests allow 9 to 12). */
    a->frame_irq = 0;
    a->irq_clear_pending = 0;
    a->fc_pos = 0;
    a->fc_restart[0] = 0;
    a->fc_restart[1] = 0;
    a->fc_restart_now = 1;

    a->tri.step = 0;
    a->dmc.level &= 1u;
}
