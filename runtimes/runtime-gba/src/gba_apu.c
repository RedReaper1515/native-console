/*
 * Audio: the four Game Boy-style PSG channels and the two DirectSound FIFOs
 * (GBATEK "GBA Sound Controller").
 *
 * The APU produces one stereo sample every 512 cycles (32768 Hz, the
 * hardware's default sampling rate) from a scheduled event. PSG channels
 * are advanced by elapsed cycles and point-sampled. DirectSound FIFOs are
 * popped by timer 0/1 overflows; when a FIFO drops to half full it requests
 * DMA1 or DMA2 in sound FIFO mode. The mix is clipped to 10 bits around
 * SOUNDBIAS like the hardware's, then AC-coupled (a DC blocker) and goes
 * to a ring buffer the frontend drains and resamples.
 */
#include <stdio.h>
#include <string.h>

#include "gba.h"

#define APU_SAMPLE_CYCLES 512u
#define FRAME_SEQ_CYCLES 32768u /* 512 Hz */

static inline uint16_t reg(const GbaMachine *m, uint32_t off)
{
    return (uint16_t)(m->io[off] | (m->io[off + 1] << 8));
}

static inline void set_reg(GbaMachine *m, uint32_t off, uint16_t v)
{
    m->io[off] = (uint8_t)v;
    m->io[off + 1] = (uint8_t)(v >> 8);
}

static const uint8_t k_duty[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
};

/* ------------------------------------------------------------------ */
/* PSG                                                                 */
/* ------------------------------------------------------------------ */

static void envelope_start(GbaApuChannel *ch, uint16_t env)
{
    ch->volume = (uint8_t)(env >> 12);
    ch->env_period = (uint8_t)((env >> 8) & 7u);
    ch->env_up = (env & 0x0800u) != 0;
    ch->env_timer = ch->env_period;
    /* A channel whose DAC is off (initial volume 0, decreasing) is silent. */
    if ((env & 0xF800u) == 0) ch->enabled = false;
}

static void envelope_clock(GbaApuChannel *ch)
{
    if (!ch->env_period) return;
    if (ch->env_timer && --ch->env_timer) return;
    ch->env_timer = ch->env_period;
    if (ch->env_up && ch->volume < 15) ch->volume++;
    else if (!ch->env_up && ch->volume > 0) ch->volume--;
}

static uint32_t square_period(uint16_t freq)
{
    return (2048u - (freq & 0x7FFu)) * 16u; /* cycles per duty step */
}

static void trigger(GbaMachine *m, int n)
{
    GbaApu *a = &m->apu;
    GbaApuChannel *ch = &a->ch[n];
    ch->enabled = true;
    switch (n) {
    case 0:
    case 1: {
        uint16_t env = reg(m, n == 0 ? 0x62 : 0x68);
        uint16_t x = reg(m, n == 0 ? 0x64 : 0x6C);
        if (!ch->length) ch->length = 64;
        ch->period = square_period(x);
        ch->timer = ch->period;
        envelope_start(ch, env);
        if (n == 0) {
            uint16_t sweep = reg(m, 0x60);
            a->sweep_freq = x & 0x7FFu;
            a->sweep_period = (uint8_t)((sweep >> 4) & 7u);
            a->sweep_timer = a->sweep_period ? a->sweep_period : 8;
        }
        break;
    }
    case 2: {
        uint16_t x = reg(m, 0x74);
        if (!ch->length) ch->length = 256;
        ch->period = (2048u - (x & 0x7FFu)) * 8u;
        ch->timer = ch->period;
        ch->pos = 0;
        if (!(reg(m, 0x70) & 0x80u)) ch->enabled = false;
        break;
    }
    default: {
        uint16_t env = reg(m, 0x78);
        uint16_t h = reg(m, 0x7C);
        if (!ch->length) ch->length = 64;
        uint32_t r = h & 7u, s = (h >> 4) & 15u;
        ch->period = ((r ? r * 16u : 8u) << s) * 4u;
        ch->timer = ch->period;
        ch->lfsr = (h & 8u) ? 0x7Fu : 0x7FFFu;
        envelope_start(ch, env);
        break;
    }
    }
}

static void sweep_clock(GbaMachine *m)
{
    GbaApu *a = &m->apu;
    if (a->sweep_timer && --a->sweep_timer) return;
    uint16_t sweep = reg(m, 0x60);
    a->sweep_period = (uint8_t)((sweep >> 4) & 7u);
    a->sweep_timer = a->sweep_period ? a->sweep_period : 8;
    uint32_t shift = sweep & 7u;
    if (!a->sweep_period || !shift || !a->ch[0].enabled) return;
    uint32_t delta = a->sweep_freq >> shift;
    uint32_t next = (sweep & 8u) ? a->sweep_freq - delta : a->sweep_freq + delta;
    if (next > 2047) {
        a->ch[0].enabled = false;
        return;
    }
    a->sweep_freq = next;
    set_reg(m, 0x64, (uint16_t)((reg(m, 0x64) & 0xF800u) | next));
    a->ch[0].period = square_period((uint16_t)next);
}

static void frame_sequencer(GbaMachine *m)
{
    GbaApu *a = &m->apu;
    uint32_t step = a->seq_step++ & 7u;
    static const uint32_t length_regs[4] = { 0x64, 0x6C, 0x74, 0x7C };
    if ((step & 1u) == 0) {
        for (int n = 0; n < 4; n++) {
            GbaApuChannel *ch = &a->ch[n];
            if ((reg(m, length_regs[n]) & 0x4000u) && ch->length && --ch->length == 0) ch->enabled = false;
        }
    }
    if (step == 2 || step == 6) sweep_clock(m);
    if (step == 7) {
        envelope_clock(&a->ch[0]);
        envelope_clock(&a->ch[1]);
        envelope_clock(&a->ch[3]);
    }
}

/* Advance a channel's waveform generator by `cycles`. */
static void advance(GbaMachine *m, int n, uint32_t cycles)
{
    GbaApuChannel *ch = &m->apu.ch[n];
    if (!ch->enabled || !ch->period) return;
    while (cycles >= ch->timer) {
        cycles -= ch->timer;
        ch->timer = ch->period;
        if (n == 3) {
            uint32_t bit = (ch->lfsr ^ (ch->lfsr >> 1)) & 1u;
            ch->lfsr >>= 1;
            if (reg(m, 0x7C) & 8u) ch->lfsr = (ch->lfsr & ~0x40u) | (bit << 6);
            else ch->lfsr = (ch->lfsr & ~0x4000u) | (bit << 14);
        } else {
            ch->pos++;
        }
    }
    ch->timer -= cycles;
}

static int channel_output(GbaMachine *m, int n)
{
    GbaApu *a = &m->apu;
    GbaApuChannel *ch = &a->ch[n];
    if (!ch->enabled) return 0;
    switch (n) {
    case 0:
    case 1: {
        uint32_t duty = (reg(m, n == 0 ? 0x62 : 0x68) >> 6) & 3u;
        return k_duty[duty][ch->pos & 7u] ? ch->volume : 0;
    }
    case 2: {
        uint16_t l = reg(m, 0x70);
        uint16_t h = reg(m, 0x72);
        bool two_banks = (l & 0x20u) != 0;
        uint32_t bank = (l >> 6) & 1u;
        uint32_t index = two_banks ? (ch->pos & 63u) : (ch->pos & 31u);
        uint32_t byte_index = (two_banks ? (bank ^ (index >> 5)) : bank) * 16u + (index & 31u) / 2u;
        uint8_t byte = a->wave[byte_index];
        int sample = (index & 1u) ? (byte & 15) : (byte >> 4);
        if (h & 0x8000u) return sample * 3 / 4;
        switch ((h >> 13) & 3u) {
        case 0: return 0;
        case 1: return sample;
        case 2: return sample >> 1;
        default: return sample >> 2;
        }
    }
    default:
        return (ch->lfsr & 1u) ? 0 : ch->volume;
    }
}

/* ------------------------------------------------------------------ */
/* DirectSound FIFOs                                                   */
/* ------------------------------------------------------------------ */

void gba_apu_fifo_write(GbaMachine *m, int fifo, uint32_t value, int bytes)
{
    GbaFifo *f = &m->apu.fifo[fifo];
    for (int i = 0; i < bytes; i++) {
        if (f->count >= 32) {
            f->overflows += (uint32_t)(bytes - i);
            break;
        }
        f->data[(f->read + f->count) & 31u] = (int8_t)(value >> (8 * i));
        f->count++;
    }
}

static void fifo_reset(GbaFifo *f)
{
    f->count = 0;
    f->read = 0;
    f->sample = 0;
}

void gba_apu_report(const GbaMachine *m)
{
    for (int n = 0; n < 2; n++) {
        const GbaFifo *f = &m->apu.fifo[n];
        if (f->underflows || f->overflows) {
            printf("audio: FIFO %c %u pops from empty, %u bytes dropped when full\n", 'A' + n, f->underflows,
                   f->overflows);
        }
    }
}

void gba_apu_timer_overflow(GbaMachine *m, int timer)
{
    uint16_t h = reg(m, 0x82);
    if (!(m->io[0x84] & 0x80u)) return;
    for (int fifo = 0; fifo < 2; fifo++) {
        uint32_t timer_bit = fifo == 0 ? 10u : 14u;
        if ((int)((h >> timer_bit) & 1u) != timer) continue;
        GbaFifo *f = &m->apu.fifo[fifo];
        if (f->count) {
            f->sample = f->data[f->read & 31u];
            f->read = (f->read + 1u) & 31u;
            f->count--;
        } else if (h & (fifo == 0 ? 0x0300u : 0x3000u)) {
            f->underflows++; /* only counted while routed to a speaker */
        }
        if (f->count <= 16) gba_dma_sound_request(m, fifo);
    }
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

uint16_t gba_apu_read16(GbaMachine *m, uint32_t off)
{
    GbaApu *a = &m->apu;
    if (off >= 0x90 && off < 0xA0) {
        /* The CPU sees the wave bank that is not selected for playback. */
        uint32_t bank = ((reg(m, 0x70) >> 6) & 1u) ^ 1u;
        uint32_t i = bank * 16u + (off - 0x90u);
        return (uint16_t)(a->wave[i] | (a->wave[i + 1] << 8));
    }
    if (off == 0x84) {
        uint16_t v = m->io[0x84] & 0x80u;
        for (int n = 0; n < 4; n++) {
            if (a->ch[n].enabled) v |= (uint16_t)(1u << n);
        }
        return v;
    }
    return reg(m, off);
}

void gba_apu_write16(GbaMachine *m, uint32_t off, uint16_t value, uint16_t byte_mask)
{
    GbaApu *a = &m->apu;
    if (off >= 0x90 && off < 0xA0) {
        uint32_t bank = ((reg(m, 0x70) >> 6) & 1u) ^ 1u;
        uint32_t i = bank * 16u + (off - 0x90u);
        if (byte_mask & 0x00FFu) a->wave[i] = (uint8_t)value;
        if (byte_mask & 0xFF00u) a->wave[i + 1] = (uint8_t)(value >> 8);
        return;
    }
    if (off >= 0xA0 && off < 0xA8) {
        int fifo = off >= 0xA4 ? 1 : 0;
        if (byte_mask == 0xFFFFu) gba_apu_fifo_write(m, fifo, value, 2);
        else gba_apu_fifo_write(m, fifo, byte_mask & 0xFFu ? value : (uint32_t)(value >> 8), 1);
        return;
    }
    if (!(m->io[0x84] & 0x80u) && off >= 0x60 && off < 0x80) return; /* powered off */

    /* Length counters load when their register is written. */
    switch (off) {
    case 0x62: a->ch[0].length = (uint16_t)(64u - (value & 63u)); break;
    case 0x68: a->ch[1].length = (uint16_t)(64u - (value & 63u)); break;
    case 0x72: a->ch[2].length = (uint16_t)(256u - (value & 255u)); break;
    case 0x78: a->ch[3].length = (uint16_t)(64u - (value & 63u)); break;
    default: break;
    }
    if (off == 0x64 && (value & 0x8000u) && (byte_mask & 0xFF00u)) trigger(m, 0);
    if (off == 0x6C && (value & 0x8000u) && (byte_mask & 0xFF00u)) trigger(m, 1);
    if (off == 0x74 && (value & 0x8000u) && (byte_mask & 0xFF00u)) trigger(m, 2);
    if (off == 0x7C && (value & 0x8000u) && (byte_mask & 0xFF00u)) trigger(m, 3);
    if (off == 0x64 && !(value & 0x8000u)) a->ch[0].period = square_period(value);
    if (off == 0x6C && !(value & 0x8000u)) a->ch[1].period = square_period(value);
    if (off == 0x70 && !(value & 0x80u)) a->ch[2].enabled = false;
    if (off == 0x82) {
        if (value & 0x0800u) fifo_reset(&a->fifo[0]);
        if (value & 0x8000u) fifo_reset(&a->fifo[1]);
    }
    if (off == 0x84 && (byte_mask & 0xFFu) && !(value & 0x80u)) {
        /* Master off: PSG registers clear and channels stop. */
        for (uint32_t r = 0x60; r < 0x82; r++) m->io[r] = 0;
        for (int n = 0; n < 4; n++) a->ch[n].enabled = false;
    }
}

/* ------------------------------------------------------------------ */
/* Mixing                                                              */
/* ------------------------------------------------------------------ */

/* y = x - x_prev + R*y with R = 65410/65536 (cutoff about 10 Hz at
 * 32768 Hz), in fixed point so output is reproducible. `level` is the
 * clipped 10-bit mixer output; the result is scaled to 16 bits. */
static int16_t high_pass(GbaDcBlock *f, int level)
{
    int32_t x = level * 64;
    f->out = ((f->out * 65410) >> 16) + ((int64_t)(x - f->prev_in) << 16);
    f->prev_in = x;
    int64_t y = f->out >> 16;
    return (int16_t)(y < -32768 ? -32768 : y > 32767 ? 32767 : y);
}

static void push_sample(GbaApu *a, int16_t left, int16_t right)
{
    uint32_t w = a->out_write & (GBA_APU_RING - 1u);
    a->out[w * 2] = left;
    a->out[w * 2 + 1] = right;
    a->out_write++;
    if (a->out_write - a->out_read > GBA_APU_RING) a->out_read = a->out_write - GBA_APU_RING;
}

void gba_apu_sample_event(GbaMachine *m)
{
    GbaApu *a = &m->apu;
    uint64_t at = m->event_at[GBA_EVENT_APU];

    a->seq_cycles += APU_SAMPLE_CYCLES;
    while (a->seq_cycles >= FRAME_SEQ_CYCLES) {
        a->seq_cycles -= FRAME_SEQ_CYCLES;
        frame_sequencer(m);
    }
    for (int n = 0; n < 4; n++) advance(m, n, APU_SAMPLE_CYCLES);

    int left = 0, right = 0;
    if (m->io[0x84] & 0x80u) {
        uint16_t l = reg(m, 0x80), h = reg(m, 0x82);
        /* PSG channels mix digitally as unsigned 0..15 (a silent or
         * disabled channel adds nothing); the DC a playing channel adds is
         * removed by the analog output stage below. */
        int psg_l = 0, psg_r = 0;
        for (int n = 0; n < 4; n++) {
            int s = channel_output(m, n);
            if (l & (0x1000u << n)) psg_l += s;
            if (l & (0x0100u << n)) psg_r += s;
        }
        psg_l *= (int)((l >> 4) & 7u) + 1;
        psg_r *= (int)(l & 7u) + 1;
        static const int psg_shift[4] = { 2, 1, 0, 0 }; /* 25%, 50%, 100% */
        psg_l >>= psg_shift[h & 3u];
        psg_r >>= psg_shift[h & 3u];

        int fa = a->fifo[0].sample << ((h & 4u) ? 2 : 1);
        int fb = a->fifo[1].sample << ((h & 8u) ? 2 : 1);
        left = psg_l + ((h & 0x0200u) ? fa : 0) + ((h & 0x2000u) ? fb : 0);
        right = psg_r + ((h & 0x0100u) ? fa : 0) + ((h & 0x1000u) ? fb : 0);
    }
    /* The mixer output is a 10-bit value around SOUNDBIAS, clipped there.
     * The console's output is AC-coupled: a first-order high-pass (about
     * 10 Hz) removes the bias and the PSG's DC, so channels starting and
     * stopping do not step the output level. */
    int bias = (int)(reg(m, 0x88) & 0x3FEu);
    int ol = bias + left, orr = bias + right;
    ol = ol < 0 ? 0 : ol > 0x3FF ? 0x3FF : ol;
    orr = orr < 0 ? 0 : orr > 0x3FF ? 0x3FF : orr;
    push_sample(a, high_pass(&a->dc[0], ol), high_pass(&a->dc[1], orr));

    gba_schedule(m, GBA_EVENT_APU, at + APU_SAMPLE_CYCLES);
}

void gba_apu_reset(GbaMachine *m)
{
    memset(&m->apu, 0, sizeof m->apu);
    /* Start settled at the default bias so there is no thump at power-on. */
    m->apu.dc[0].prev_in = m->apu.dc[1].prev_in = 0x200 * 64;
    gba_schedule(m, GBA_EVENT_APU, m->cpu.cycles + APU_SAMPLE_CYCLES);
}

size_t gba_apu_read_samples(GbaMachine *m, int16_t *out, size_t max_frames)
{
    GbaApu *a = &m->apu;
    size_t n = 0;
    while (n < max_frames && a->out_read != a->out_write) {
        uint32_t r = a->out_read & (GBA_APU_RING - 1u);
        out[n * 2] = a->out[r * 2];
        out[n * 2 + 1] = a->out[r * 2 + 1];
        a->out_read++;
        n++;
    }
    return n;
}
