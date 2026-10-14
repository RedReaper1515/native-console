/*
 * S-DSP: cycle-scheduled voice pipeline, BRR decoding, Gaussian
 * interpolation, envelopes, noise and echo.
 *
 * Ported from ares (ISC license, Near et al.), ares/sfc/dsp/{dsp,brr,
 * counter,echo,envelope,gaussian,memory,misc,voice}.cpp at commit
 * af4cbb04f067682a8a3cf42695ff78bed634b38d: the 32-step per-sample
 * schedule, the voice steps V1-V9, echo steps 22-30, misc steps 27-30,
 * the envelope/BRR/noise/echo arithmetic and the rate counter tables.
 * Rewritten in C with explicit wrap/clamp helpers in place of ares's
 * fixed-width integer types; no undefined signed shifts.
 *
 * The Gaussian table is hardware data, transcribed from fullsnes ("SNES
 * APU DSP BRR Pitch"); ares computes the identical table at run time.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#include "snes_dsp.h"

#include <string.h>

/* ---- Integer helpers --------------------------------------------------- */

/* Arithmetic shift right, defined for negative values. */
static inline int32_t asr(int32_t v, unsigned n) { return v < 0 ? ~(~v >> n) : v >> n; }

static inline int32_t clamp16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

/* Keep the low 16 (17) bits as a signed value. */
static inline int32_t wrap16(int32_t v)
{
    int32_t u = (int32_t)((uint32_t)v & 0xFFFFu);
    return u >= 0x8000 ? u - 0x10000 : u;
}

static inline int32_t wrap17(int32_t v)
{
    int32_t u = (int32_t)((uint32_t)v & 0x1FFFFu);
    return u >= 0x10000 ? u - 0x20000 : u;
}

/* Clear bit 0 (x & ~1 on a two's-complement value). */
static inline int32_t even(int32_t v) { return v - (int32_t)((uint32_t)v & 1u); }

/* ---- Tables -------------------------------------------------------------- */

/* The S-DSP's 512-entry Gaussian interpolation table (hardware data). */
static const int16_t gauss_table[512] = {
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x002, 0x002,
    0x002, 0x002, 0x003, 0x003, 0x003, 0x003, 0x003, 0x004, 0x004, 0x004, 0x004, 0x004, 0x005, 0x005, 0x005, 0x005,
    0x006, 0x006, 0x006, 0x006, 0x007, 0x007, 0x007, 0x008, 0x008, 0x008, 0x009, 0x009, 0x009, 0x00A, 0x00A, 0x00A,
    0x00B, 0x00B, 0x00B, 0x00C, 0x00C, 0x00D, 0x00D, 0x00E, 0x00E, 0x00F, 0x00F, 0x00F, 0x010, 0x010, 0x011, 0x011,
    0x012, 0x013, 0x013, 0x014, 0x014, 0x015, 0x015, 0x016, 0x017, 0x017, 0x018, 0x018, 0x019, 0x01A, 0x01B, 0x01B,
    0x01C, 0x01D, 0x01D, 0x01E, 0x01F, 0x020, 0x020, 0x021, 0x022, 0x023, 0x024, 0x024, 0x025, 0x026, 0x027, 0x028,
    0x029, 0x02A, 0x02B, 0x02C, 0x02D, 0x02E, 0x02F, 0x030, 0x031, 0x032, 0x033, 0x034, 0x035, 0x036, 0x037, 0x038,
    0x03A, 0x03B, 0x03C, 0x03D, 0x03E, 0x040, 0x041, 0x042, 0x043, 0x045, 0x046, 0x047, 0x049, 0x04A, 0x04C, 0x04D,
    0x04E, 0x050, 0x051, 0x053, 0x054, 0x056, 0x057, 0x059, 0x05A, 0x05C, 0x05E, 0x05F, 0x061, 0x063, 0x064, 0x066,
    0x068, 0x06A, 0x06B, 0x06D, 0x06F, 0x071, 0x073, 0x075, 0x076, 0x078, 0x07A, 0x07C, 0x07E, 0x080, 0x082, 0x084,
    0x086, 0x089, 0x08B, 0x08D, 0x08F, 0x091, 0x093, 0x096, 0x098, 0x09A, 0x09C, 0x09F, 0x0A1, 0x0A3, 0x0A6, 0x0A8,
    0x0AB, 0x0AD, 0x0AF, 0x0B2, 0x0B4, 0x0B7, 0x0BA, 0x0BC, 0x0BF, 0x0C1, 0x0C4, 0x0C7, 0x0C9, 0x0CC, 0x0CF, 0x0D2,
    0x0D4, 0x0D7, 0x0DA, 0x0DD, 0x0E0, 0x0E3, 0x0E6, 0x0E9, 0x0EC, 0x0EF, 0x0F2, 0x0F5, 0x0F8, 0x0FB, 0x0FE, 0x101,
    0x104, 0x107, 0x10B, 0x10E, 0x111, 0x114, 0x118, 0x11B, 0x11E, 0x122, 0x125, 0x129, 0x12C, 0x130, 0x133, 0x137,
    0x13A, 0x13E, 0x141, 0x145, 0x148, 0x14C, 0x150, 0x153, 0x157, 0x15B, 0x15F, 0x162, 0x166, 0x16A, 0x16E, 0x172,
    0x176, 0x17A, 0x17D, 0x181, 0x185, 0x189, 0x18D, 0x191, 0x195, 0x19A, 0x19E, 0x1A2, 0x1A6, 0x1AA, 0x1AE, 0x1B2,
    0x1B7, 0x1BB, 0x1BF, 0x1C3, 0x1C8, 0x1CC, 0x1D0, 0x1D5, 0x1D9, 0x1DD, 0x1E2, 0x1E6, 0x1EB, 0x1EF, 0x1F3, 0x1F8,
    0x1FC, 0x201, 0x205, 0x20A, 0x20F, 0x213, 0x218, 0x21C, 0x221, 0x226, 0x22A, 0x22F, 0x233, 0x238, 0x23D, 0x241,
    0x246, 0x24B, 0x250, 0x254, 0x259, 0x25E, 0x263, 0x267, 0x26C, 0x271, 0x276, 0x27B, 0x280, 0x284, 0x289, 0x28E,
    0x293, 0x298, 0x29D, 0x2A2, 0x2A6, 0x2AB, 0x2B0, 0x2B5, 0x2BA, 0x2BF, 0x2C4, 0x2C9, 0x2CE, 0x2D3, 0x2D8, 0x2DC,
    0x2E1, 0x2E6, 0x2EB, 0x2F0, 0x2F5, 0x2FA, 0x2FF, 0x304, 0x309, 0x30E, 0x313, 0x318, 0x31D, 0x322, 0x326, 0x32B,
    0x330, 0x335, 0x33A, 0x33F, 0x344, 0x349, 0x34E, 0x353, 0x357, 0x35C, 0x361, 0x366, 0x36B, 0x370, 0x374, 0x379,
    0x37E, 0x383, 0x388, 0x38C, 0x391, 0x396, 0x39B, 0x39F, 0x3A4, 0x3A9, 0x3AD, 0x3B2, 0x3B7, 0x3BB, 0x3C0, 0x3C5,
    0x3C9, 0x3CE, 0x3D2, 0x3D7, 0x3DC, 0x3E0, 0x3E5, 0x3E9, 0x3ED, 0x3F2, 0x3F6, 0x3FB, 0x3FF, 0x403, 0x408, 0x40C,
    0x410, 0x415, 0x419, 0x41D, 0x421, 0x425, 0x42A, 0x42E, 0x432, 0x436, 0x43A, 0x43E, 0x442, 0x446, 0x44A, 0x44E,
    0x452, 0x455, 0x459, 0x45D, 0x461, 0x465, 0x468, 0x46C, 0x470, 0x473, 0x477, 0x47A, 0x47E, 0x481, 0x485, 0x488,
    0x48C, 0x48F, 0x492, 0x496, 0x499, 0x49C, 0x49F, 0x4A2, 0x4A6, 0x4A9, 0x4AC, 0x4AF, 0x4B2, 0x4B5, 0x4B7, 0x4BA,
    0x4BD, 0x4C0, 0x4C3, 0x4C5, 0x4C8, 0x4CB, 0x4CD, 0x4D0, 0x4D2, 0x4D5, 0x4D7, 0x4D9, 0x4DC, 0x4DE, 0x4E0, 0x4E3,
    0x4E5, 0x4E7, 0x4E9, 0x4EB, 0x4ED, 0x4EF, 0x4F1, 0x4F3, 0x4F5, 0x4F6, 0x4F8, 0x4FA, 0x4FB, 0x4FD, 0x4FF, 0x500,
    0x502, 0x503, 0x504, 0x506, 0x507, 0x508, 0x50A, 0x50B, 0x50C, 0x50D, 0x50E, 0x50F, 0x510, 0x511, 0x511, 0x512,
    0x513, 0x514, 0x514, 0x515, 0x516, 0x516, 0x517, 0x517, 0x517, 0x518, 0x518, 0x518, 0x518, 0x518, 0x519, 0x519,
};

/* Samples per event for the 32 envelope/noise rates (0 never fires), and
 * the counter phase offset per rate. The counter runs from 0x77FF down
 * and every rate divides 0x7800. */
static const uint16_t counter_rate[32] = {
    0,    2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64,   48,   40,   32,   24,   20,  16,  12,  10,  8,   6,   5,   4,   3,   2,  1,
};
static const uint16_t counter_offset[32] = {
    0,   0,    1040, 536, 0,    1040, 536, 0,    1040, 536, 0,    1040, 536, 0,    1040, 536,
    0,   1040, 536,  0,   1040, 536,  0,   1040, 536,  0,   1040, 536,  0,   1040, 0,    0,
};

static inline void counter_tick(SnesDsp *d)
{
    if (!d->counter) d->counter = 0x7800;
    d->counter--;
}

static inline int counter_poll(const SnesDsp *d, unsigned rate)
{
    if (rate == 0) return 0;
    return ((uint32_t)d->counter + counter_offset[rate]) % counter_rate[rate] == 0;
}

/* ---- Envelope -------------------------------------------------------------- */

static void envelope_run(SnesDsp *d, SnesDspVoice *v)
{
    int32_t env = v->envelope;

    if (v->envelope_mode == SNES_DSP_ENV_RELEASE) {
        env -= 0x8;
        if (env < 0) env = 0;
        v->envelope = (uint16_t)env;
        return;
    }

    int32_t rate;
    int32_t data = v->adsr1;
    if (d->latch_adsr0 & 0x80u) { /* ADSR */
        if (v->envelope_mode >= SNES_DSP_ENV_DECAY) {
            env--;
            env -= asr(env, 8);
            rate = data & 0x1F;
            if (v->envelope_mode == SNES_DSP_ENV_DECAY) rate = (int32_t)((d->latch_adsr0 >> 4) & 7u) * 2 + 16;
        } else { /* attack */
            rate = (int32_t)(d->latch_adsr0 & 0x0Fu) * 2 + 1;
            env += rate < 31 ? 0x20 : 0x400;
        }
    } else { /* GAIN */
        data = v->gain;
        int32_t mode = data >> 5;
        if (mode < 4) { /* direct */
            env = data << 4;
            rate = 31;
        } else {
            rate = data & 0x1F;
            if (mode == 4) { /* linear decrease */
                env -= 0x20;
            } else if (mode < 6) { /* exponential decrease */
                env--;
                env -= asr(env, 8);
            } else { /* linear increase; mode 7 bends at 0x600 */
                env += 0x20;
                if (mode > 6 && (uint32_t)v->envelope_latch >= 0x600u) env += 0x8 - 0x20;
            }
        }
    }

    /* Sustain level. */
    if (asr(env, 8) == (data >> 5) && v->envelope_mode == SNES_DSP_ENV_DECAY) v->envelope_mode = SNES_DSP_ENV_SUSTAIN;
    v->envelope_latch = env;

    /* Unsigned compare: a linear decrease below zero also lands here. */
    if ((uint32_t)env > 0x7FFu) {
        env = env < 0 ? 0 : 0x7FF;
        if (v->envelope_mode == SNES_DSP_ENV_ATTACK) v->envelope_mode = SNES_DSP_ENV_DECAY;
    }

    if (counter_poll(d, (unsigned)rate)) v->envelope = (uint16_t)env;
}

/* ---- BRR ------------------------------------------------------------------- */

/* Decode four samples: the byte cached in V3b and the next one. */
static void brr_decode(SnesDsp *d, SnesDspVoice *v, const uint8_t *aram)
{
    uint32_t nybbles = ((uint32_t)d->brr_byte_l << 8) | aram[(uint16_t)(v->brr_address + v->brr_offset + 1u)];
    const int filter = (d->brr_header_l >> 2) & 3;
    const int scale = d->brr_header_l >> 4;

    for (int n = 0; n < 4; n++) {
        /* Top nybble, sign-extended. */
        int32_t s = asr(wrap16((int32_t)(nybbles & 0xFFFFu)), 12);
        nybbles <<= 4;

        if (scale <= 12) {
            s = s * (1 << scale);
            s = asr(s, 1);
        } else {
            s = s < 0 ? -2048 : 0;
        }

        int off = v->buffer_offset;
        if (--off < 0) off = 11;
        const int32_t p1 = v->buffer[off];
        if (--off < 0) off = 11;
        const int32_t p2 = asr(v->buffer[off], 1);

        switch (filter) {
        case 0:
            break;
        case 1: /* s += p1 * 0.46875 */
            s += asr(p1, 1);
            s += asr(-p1, 5);
            break;
        case 2: /* s += p1 * 0.953125 - p2 * 0.46875 */
            s += p1;
            s -= p2;
            s += asr(p2, 4);
            s += asr(p1 * -3, 6);
            break;
        case 3: /* s += p1 * 0.8984375 - p2 * 0.40625 */
            s += p1;
            s -= p2;
            s += asr(p1 * -13, 7);
            s += asr(p2 * 3, 4);
            break;
        }

        s = clamp16(s);
        s = wrap16(s * 2);
        v->buffer[v->buffer_offset] = (int16_t)s;
        if (++v->buffer_offset >= 12) v->buffer_offset = 0;
    }
}

/* ---- Gaussian interpolation ------------------------------------------------ */

static int32_t gaussian_interpolate(const SnesDspVoice *v)
{
    const unsigned i = (v->gaussian_offset >> 4) & 0xFFu;
    const int16_t *fwd = gauss_table + 255 - i;
    const int16_t *rev = gauss_table + i;

    unsigned o = (v->buffer_offset + (v->gaussian_offset >> 12)) % 12u;
    int32_t out;
    out = asr(fwd[0] * v->buffer[o], 11);
    if (++o >= 12) o = 0;
    out += asr(fwd[256] * v->buffer[o], 11);
    if (++o >= 12) o = 0;
    out += asr(rev[256] * v->buffer[o], 11);
    if (++o >= 12) o = 0;
    out = wrap16(out);
    out += asr(rev[0] * v->buffer[o], 11);
    return even(clamp16(out));
}

/* ---- Voice pipeline -------------------------------------------------------- */

static inline void voice_output(SnesDsp *d, const SnesDspVoice *v, int ch)
{
    int32_t amp = asr((int32_t)d->latch_output * v->volume[ch], 7);
    d->main_output[ch] = clamp16(wrap17(d->main_output[ch] + amp));
    if (v->l_echo) d->echo_output[ch] = clamp16(wrap17(d->echo_output[ch] + amp));
}

static void voice1(SnesDsp *d, SnesDspVoice *v)
{
    d->brr_address_l = (uint16_t)(((uint32_t)d->brr_bank_l << 8) + ((uint32_t)d->brr_source_l << 2));
    d->brr_source_l = v->source;
}

static void voice2(SnesDsp *d, SnesDspVoice *v, const uint8_t *aram)
{
    /* Sample pointer (start on KON, else loop address). */
    uint16_t addr = d->brr_address_l;
    if (!v->keyon_delay) addr = (uint16_t)(addr + 2u);
    uint16_t lo = aram[addr];
    uint16_t hi = aram[(uint16_t)(addr + 1u)];
    d->brr_next_address_l = (uint16_t)(lo | (hi << 8));
    d->latch_adsr0 = v->adsr0;
    /* Pitch is read over two clocks. */
    d->latch_pitch = (uint16_t)(v->pitch & 0xFFu);
}

static void voice3a(SnesDsp *d, SnesDspVoice *v) { d->latch_pitch = (uint16_t)(d->latch_pitch | (v->pitch & 0x3F00u)); }

static void voice3b(SnesDsp *d, SnesDspVoice *v, const uint8_t *aram)
{
    d->brr_byte_l = aram[(uint16_t)(v->brr_address + v->brr_offset)];
    d->brr_header_l = aram[v->brr_address];
}

static void voice3c(SnesDsp *d, SnesDspVoice *v)
{
    /* Pitch modulation by the previous voice's output. */
    if (v->l_modulate) {
        int32_t p = d->latch_pitch;
        p += asr(asr(d->latch_output, 5) * p, 10);
        d->latch_pitch = (uint16_t)((uint32_t)p & 0x7FFFu);
    }

    if (v->keyon_delay) {
        /* Start BRR decoding on the next sample. */
        if (v->keyon_delay == 5) {
            v->brr_address = d->brr_next_address_l;
            v->brr_offset = 1;
            v->buffer_offset = 0;
            d->brr_header_l = 0; /* the header is ignored on this sample */
        }
        /* The envelope does not run during KON. */
        v->envelope = 0;
        v->envelope_latch = 0;
        /* Decoding is held off until the last three samples. */
        v->gaussian_offset = 0;
        v->keyon_delay--;
        if (v->keyon_delay & 3u) v->gaussian_offset = 0x4000;
        /* Pitch is not added during KON. */
        d->latch_pitch = 0;
    }

    int32_t output = gaussian_interpolate(v);
    if (v->l_noise) output = wrap16((int32_t)((uint32_t)d->noise_lfsr << 1));

    /* Envelope. */
    d->latch_output = (int16_t)wrap16(even(asr(output * (int32_t)v->envelope, 11)));
    v->envx = (uint8_t)(v->envelope >> 4);

    /* Immediate silence: end-without-loop block, or soft reset. */
    if (d->soft_reset || (d->brr_header_l & 3u) == 1u) {
        v->envelope_mode = SNES_DSP_ENV_RELEASE;
        v->envelope = 0;
    }

    if (d->sample) {
        if (v->l_keyoff) v->envelope_mode = SNES_DSP_ENV_RELEASE;
        if (v->l_keyon) {
            v->keyon_delay = 5;
            v->envelope_mode = SNES_DSP_ENV_ATTACK;
        }
    }

    if (!v->keyon_delay) envelope_run(d, v);
}

static void voice3(SnesDsp *d, SnesDspVoice *v, const uint8_t *aram)
{
    voice3a(d, v);
    voice3b(d, v, aram);
    voice3c(d, v);
}

static void voice4(SnesDsp *d, SnesDspVoice *v, const uint8_t *aram)
{
    /* Decode BRR. */
    v->l_looped = 0;
    if (v->gaussian_offset >= 0x4000u) {
        brr_decode(d, v, aram);
        v->brr_offset = (uint8_t)(v->brr_offset + 2u);
        if (v->brr_offset >= 9u) {
            /* Next block; the end flag jumps to the loop address. */
            v->brr_address = (uint16_t)(v->brr_address + 9u);
            if (d->brr_header_l & 1u) {
                v->brr_address = d->brr_next_address_l;
                v->l_looped = 1;
            }
            v->brr_offset = 1;
        }
    }

    /* Pitch. */
    uint32_t g = (uint32_t)(v->gaussian_offset & 0x3FFFu) + d->latch_pitch;
    if (g > 0x7FFFu) g = 0x7FFFu; /* pitch modulation can overshoot */
    v->gaussian_offset = (uint16_t)g;

    voice_output(d, v, 0);
}

static void voice5(SnesDsp *d, SnesDspVoice *v)
{
    voice_output(d, v, 1);
    /* ENDX, OUTX and ENVX do not update if written 1-2 clocks earlier. */
    v->l_end = (uint8_t)(v->l_end | v->l_looped);
    /* KON clears the voice's ENDX bit. */
    if (v->keyon_delay == 5) v->l_end = 0;
}

static void voice6(SnesDsp *d) { d->latch_outx = (uint8_t)asr(d->latch_output, 8); }

static void voice7(SnesDsp *d, SnesDspVoice *v)
{
    uint8_t endx = 0;
    for (int n = 0; n < 8; n++) endx = (uint8_t)(endx | (d->voice[n].l_end << n));
    d->regs[0x7C] = endx;
    d->latch_envx = v->envx;
}

static void voice8(SnesDsp *d, SnesDspVoice *v) { d->regs[v->index | 0x09u] = d->latch_outx; }

static void voice9(SnesDsp *d, SnesDspVoice *v) { d->regs[v->index | 0x08u] = d->latch_envx; }

/* ---- Echo ------------------------------------------------------------------ */

static inline int32_t fir_tap(const SnesDsp *d, int ch, int index)
{
    int32_t s = d->history[ch][(d->history_offset + index + 1) & 7];
    return asr(s * d->fir[index], 6);
}

static inline int32_t echo_output_ch(const SnesDsp *d, int ch)
{
    int32_t main = wrap16(asr(d->main_output[ch] * d->main_volume[ch], 7));
    int32_t echo = wrap16(asr(d->echo_input[ch] * d->echo_volume[ch], 7));
    return clamp16(main + echo);
}

static void echo_read(SnesDsp *d, int ch, const uint8_t *aram)
{
    uint16_t addr = (uint16_t)(d->echo_address + ch * 2);
    uint32_t lo = aram[addr];
    uint32_t hi = aram[(uint16_t)(addr + 1u)];
    int32_t s = wrap16((int32_t)((hi << 8) | lo));
    d->history[ch][d->history_offset] = (int16_t)asr(s, 1);
}

static void echo_write(SnesDsp *d, int ch, uint8_t *aram)
{
    if (!d->echo_readonly_l) {
        uint16_t addr = (uint16_t)(d->echo_address + ch * 2);
        uint32_t s = (uint32_t)d->echo_output[ch];
        aram[addr] = (uint8_t)(s & 0xFFu);
        aram[(uint16_t)(addr + 1u)] = (uint8_t)((s >> 8) & 0xFFu);
    }
    d->echo_output[ch] = 0;
}

static void echo22(SnesDsp *d, const uint8_t *aram)
{
    d->history_offset = (uint8_t)((d->history_offset + 1u) & 7u);
    d->echo_address = (uint16_t)(((uint32_t)d->echo_page_l << 8) + d->echo_offset);
    echo_read(d, 0, aram);
    d->echo_input[0] = wrap17(fir_tap(d, 0, 0));
    d->echo_input[1] = wrap17(fir_tap(d, 1, 0));
}

static void echo23(SnesDsp *d, const uint8_t *aram)
{
    int32_t l = fir_tap(d, 0, 1) + fir_tap(d, 0, 2);
    int32_t r = fir_tap(d, 1, 1) + fir_tap(d, 1, 2);
    d->echo_input[0] = wrap17(d->echo_input[0] + l);
    d->echo_input[1] = wrap17(d->echo_input[1] + r);
    echo_read(d, 1, aram);
}

static void echo24(SnesDsp *d)
{
    int32_t l = fir_tap(d, 0, 3) + fir_tap(d, 0, 4) + fir_tap(d, 0, 5);
    int32_t r = fir_tap(d, 1, 3) + fir_tap(d, 1, 4) + fir_tap(d, 1, 5);
    d->echo_input[0] = wrap17(d->echo_input[0] + l);
    d->echo_input[1] = wrap17(d->echo_input[1] + r);
}

static void echo25(SnesDsp *d)
{
    int32_t l = d->echo_input[0] + fir_tap(d, 0, 6);
    int32_t r = d->echo_input[1] + fir_tap(d, 1, 6);
    l = wrap16(l);
    r = wrap16(r);
    l += wrap16(fir_tap(d, 0, 7));
    r += wrap16(fir_tap(d, 1, 7));
    d->echo_input[0] = even(clamp16(l));
    d->echo_input[1] = even(clamp16(r));
}

static void echo26(SnesDsp *d)
{
    /* Left output (kept for the next clock, output together). */
    d->main_output[0] = echo_output_ch(d, 0);
    /* Feedback. */
    int32_t l = d->echo_output[0] + wrap16(asr(d->echo_input[0] * d->echo_feedback, 7));
    int32_t r = d->echo_output[1] + wrap16(asr(d->echo_input[1] * d->echo_feedback, 7));
    d->echo_output[0] = even(clamp16(l));
    d->echo_output[1] = even(clamp16(r));
}

static void emit_sample(SnesDsp *d, int32_t l, int32_t r)
{
    const uint32_t mask = SNES_DSP_RING_FRAMES - 1u;
    if (d->ring_count == SNES_DSP_RING_FRAMES) {
        d->ring_read = (d->ring_read + 1u) & mask;
        d->ring_count--;
        d->overruns++;
    }
    uint32_t w = (d->ring_read + d->ring_count) & mask;
    d->ring[w * 2u] = (int16_t)l;
    d->ring[w * 2u + 1u] = (int16_t)r;
    d->ring_count++;
    d->frames++;
}

static void echo27(SnesDsp *d)
{
    int32_t outl = d->main_output[0];
    int32_t outr = echo_output_ch(d, 1);
    d->main_output[0] = 0;
    d->main_output[1] = 0;
    if (d->mute) { /* FLG bit 6 mutes the amplifier only */
        outl = 0;
        outr = 0;
    }
    emit_sample(d, outl, outr);
}

static void echo28(SnesDsp *d) { d->echo_readonly_l = d->echo_readonly; }

static void echo29(SnesDsp *d, uint8_t *aram)
{
    d->echo_page_l = d->echo_page;
    if (!d->echo_offset) d->echo_length = (uint16_t)(d->echo_delay << 11);
    d->echo_offset = (uint16_t)(d->echo_offset + 4u);
    if (d->echo_offset >= d->echo_length) d->echo_offset = 0;
    echo_write(d, 0, aram);
    d->echo_readonly_l = d->echo_readonly;
}

static void echo30(SnesDsp *d, uint8_t *aram) { echo_write(d, 1, aram); }

/* ---- Misc ------------------------------------------------------------------ */

static void misc27(SnesDsp *d)
{
    for (int n = 0; n < 8; n++) d->voice[n].l_modulate = d->voice[n].modulate;
}

static void misc28(SnesDsp *d)
{
    for (int n = 0; n < 8; n++) {
        d->voice[n].l_noise = d->voice[n].noise;
        d->voice[n].l_echo = d->voice[n].echo;
    }
    d->brr_bank_l = d->brr_bank;
}

static void misc29(SnesDsp *d)
{
    d->sample ^= 1u;
    if (d->sample) { /* KON is cleared 63 clocks after it was last read */
        for (int n = 0; n < 8; n++) d->voice[n].l_keylatch &= (uint8_t)!d->voice[n].l_keyon;
    }
}

static void misc30(SnesDsp *d)
{
    if (d->sample) {
        for (int n = 0; n < 8; n++) {
            d->voice[n].l_keyon = d->voice[n].l_keylatch;
            d->voice[n].l_keyoff = d->voice[n].keyoff;
        }
    }
    counter_tick(d);
    if (counter_poll(d, d->noise_frequency)) {
        uint32_t lfsr = d->noise_lfsr;
        uint32_t feedback = (lfsr << 13) ^ (lfsr << 14);
        d->noise_lfsr = (uint16_t)((feedback & 0x4000u) | (lfsr >> 1));
    }
}

/* ---- The 32-clock schedule ------------------------------------------------- */

void snes_dsp_clock(SnesDsp *d, uint8_t *aram)
{
    SnesDspVoice *v = d->voice;
    switch (d->phase) {
    case 0: voice5(d, &v[0]); voice2(d, &v[1], aram); break;
    case 1: voice6(d); voice3(d, &v[1], aram); break;
    case 2: voice7(d, &v[0]); voice4(d, &v[1], aram); voice1(d, &v[3]); break;
    case 3: voice8(d, &v[0]); voice5(d, &v[1]); voice2(d, &v[2], aram); break;
    case 4: voice9(d, &v[0]); voice6(d); voice3(d, &v[2], aram); break;
    case 5: voice7(d, &v[1]); voice4(d, &v[2], aram); voice1(d, &v[4]); break;
    case 6: voice8(d, &v[1]); voice5(d, &v[2]); voice2(d, &v[3], aram); break;
    case 7: voice9(d, &v[1]); voice6(d); voice3(d, &v[3], aram); break;
    case 8: voice7(d, &v[2]); voice4(d, &v[3], aram); voice1(d, &v[5]); break;
    case 9: voice8(d, &v[2]); voice5(d, &v[3]); voice2(d, &v[4], aram); break;
    case 10: voice9(d, &v[2]); voice6(d); voice3(d, &v[4], aram); break;
    case 11: voice7(d, &v[3]); voice4(d, &v[4], aram); voice1(d, &v[6]); break;
    case 12: voice8(d, &v[3]); voice5(d, &v[4]); voice2(d, &v[5], aram); break;
    case 13: voice9(d, &v[3]); voice6(d); voice3(d, &v[5], aram); break;
    case 14: voice7(d, &v[4]); voice4(d, &v[5], aram); voice1(d, &v[7]); break;
    case 15: voice8(d, &v[4]); voice5(d, &v[5]); voice2(d, &v[6], aram); break;
    case 16: voice9(d, &v[4]); voice6(d); voice3(d, &v[6], aram); break;
    case 17: voice1(d, &v[0]); voice7(d, &v[5]); voice4(d, &v[6], aram); break;
    case 18: voice8(d, &v[5]); voice5(d, &v[6]); voice2(d, &v[7], aram); break;
    case 19: voice9(d, &v[5]); voice6(d); voice3(d, &v[7], aram); break;
    case 20: voice1(d, &v[1]); voice7(d, &v[6]); voice4(d, &v[7], aram); break;
    case 21: voice8(d, &v[6]); voice5(d, &v[7]); voice2(d, &v[0], aram); break;
    case 22: voice3a(d, &v[0]); voice9(d, &v[6]); voice6(d); echo22(d, aram); break;
    case 23: voice7(d, &v[7]); echo23(d, aram); break;
    case 24: voice8(d, &v[7]); echo24(d); break;
    case 25: voice3b(d, &v[0], aram); voice9(d, &v[7]); echo25(d); break;
    case 26: echo26(d); break;
    case 27: misc27(d); echo27(d); break;
    case 28: misc28(d); echo28(d); break;
    case 29: misc29(d); echo29(d, aram); break;
    case 30: misc30(d); voice3c(d, &v[0]); echo30(d, aram); break;
    default: voice4(d, &v[0], aram); voice1(d, &v[2]); break; /* 31 */
    }
    d->phase = (uint8_t)((d->phase + 1u) & 31u);
}

/* ---- Registers ------------------------------------------------------------- */

uint8_t snes_dsp_read(const SnesDsp *d, uint8_t addr) { return d->regs[addr & 0x7Fu]; }

void snes_dsp_write(SnesDsp *d, uint8_t addr, uint8_t data)
{
    addr &= 0x7Fu;
    d->regs[addr] = data;

    /* Signed register values: (int8_t) of a byte, done portably. */
    const int8_t sdata = (int8_t)((int)data - ((data & 0x80u) ? 256 : 0));

    switch (addr) {
    case 0x0C: d->main_volume[0] = sdata; break; /* MVOLL */
    case 0x1C: d->main_volume[1] = sdata; break; /* MVOLR */
    case 0x2C: d->echo_volume[0] = sdata; break; /* EVOLL */
    case 0x3C: d->echo_volume[1] = sdata; break; /* EVOLR */
    case 0x4C: /* KON */
        for (int n = 0; n < 8; n++) d->voice[n].l_keylatch = (uint8_t)((data >> n) & 1u);
        break;
    case 0x5C: /* KOFF */
        for (int n = 0; n < 8; n++) d->voice[n].keyoff = (uint8_t)((data >> n) & 1u);
        break;
    case 0x6C: /* FLG */
        d->noise_frequency = (uint8_t)(data & 0x1Fu);
        d->echo_readonly = (uint8_t)((data >> 5) & 1u);
        d->mute = (uint8_t)((data >> 6) & 1u);
        d->soft_reset = (uint8_t)((data >> 7) & 1u);
        break;
    case 0x7C: /* ENDX: any write clears every bit */
        for (int n = 0; n < 8; n++) d->voice[n].l_end = 0;
        d->regs[0x7C] = 0;
        break;
    case 0x0D: d->echo_feedback = sdata; break; /* EFB */
    case 0x2D: /* PMON (voice 0 cannot be modulated) */
        for (int n = 0; n < 8; n++) d->voice[n].modulate = (uint8_t)((data >> n) & 1u);
        d->voice[0].modulate = 0;
        break;
    case 0x3D: /* NON */
        for (int n = 0; n < 8; n++) d->voice[n].noise = (uint8_t)((data >> n) & 1u);
        break;
    case 0x4D: /* EON */
        for (int n = 0; n < 8; n++) d->voice[n].echo = (uint8_t)((data >> n) & 1u);
        break;
    case 0x5D: d->brr_bank = data; break;                         /* DIR */
    case 0x6D: d->echo_page = data; break;                        /* ESA */
    case 0x7D: d->echo_delay = (uint8_t)(data & 0x0Fu); break;    /* EDL */
    default: break;
    }

    SnesDspVoice *v = &d->voice[(addr >> 4) & 7u];
    switch (addr & 0x0Fu) {
    case 0x00: v->volume[0] = sdata; break;                                              /* VxVOLL */
    case 0x01: v->volume[1] = sdata; break;                                              /* VxVOLR */
    case 0x02: v->pitch = (uint16_t)((v->pitch & 0x3F00u) | data); break;                /* VxPITCHL */
    case 0x03: v->pitch = (uint16_t)((v->pitch & 0x00FFu) | ((data & 0x3Fu) << 8)); break; /* VxPITCHH */
    case 0x04: v->source = data; break;                                                  /* VxSRCN */
    case 0x05: v->adsr0 = data; break;                                                   /* VxADSR1 */
    case 0x06: v->adsr1 = data; break;                                                   /* VxADSR2 */
    case 0x07: v->gain = data; break;                                                    /* VxGAIN */
    case 0x08: d->latch_envx = data; break; /* VxENVX: written value is latched */
    case 0x09: d->latch_outx = data; break; /* VxOUTX */
    case 0x0F: d->fir[(addr >> 4) & 7u] = sdata; break;                                  /* FIRx */
    default: break;
    }
}

/* ---- Power, reset, output -------------------------------------------------- */

static void reset_internal(SnesDsp *d)
{
    /* The register file and the output ring (with its counters) are kept;
     * everything else is cleared. */
    memset(d->voice, 0, sizeof d->voice);
    memset(d->main_output, 0, sizeof d->main_output);
    memset(d->echo_input, 0, sizeof d->echo_input);
    memset(d->echo_output, 0, sizeof d->echo_output);
    memset(d->history, 0, sizeof d->history);
    d->echo_address = d->echo_offset = d->echo_length = 0;
    d->counter = 0;
    d->noise_lfsr = 0x4000; /* noise level starts at -0x4000 */
    d->brr_address_l = d->brr_next_address_l = 0;
    d->latch_pitch = 0;
    d->latch_output = 0;
    memset(d->main_volume, 0, sizeof d->main_volume);
    memset(d->echo_volume, 0, sizeof d->echo_volume);
    d->echo_feedback = 0;
    memset(d->fir, 0, sizeof d->fir);
    d->echo_page = d->echo_delay = 0;
    d->echo_readonly = 1;
    d->echo_page_l = 0;
    d->echo_readonly_l = 0;
    d->history_offset = 0;
    d->mute = 1;
    d->soft_reset = 1;
    d->noise_frequency = 0;
    d->brr_bank = d->brr_bank_l = d->brr_source_l = d->brr_header_l = d->brr_byte_l = 0;
    d->latch_adsr0 = d->latch_envx = d->latch_outx = 0;
    d->sample = 1;
    d->phase = 0;
    memset(d->pad_, 0, sizeof d->pad_);
    for (int n = 0; n < 8; n++) {
        d->voice[n].index = (uint8_t)(n << 4);
        d->voice[n].brr_offset = 1;
    }
}

void snes_dsp_power(SnesDsp *d)
{
    memset(d, 0, sizeof *d);
    reset_internal(d);
    d->regs[0x6C] = 0xE0; /* FLG: soft reset, mute, echo writes disabled */
}

void snes_dsp_reset(SnesDsp *d)
{
    reset_internal(d);
    /* Re-apply the kept register values to the voice and echo units. */
    for (unsigned r = 0; r < 128; r++) {
        if (r == 0x4C || r == 0x7C || (r & 0x0Fu) == 0x08u || (r & 0x0Fu) == 0x09u) continue;
        snes_dsp_write(d, (uint8_t)r, d->regs[r]);
    }
    d->latch_envx = d->latch_outx = 0;
    snes_dsp_write(d, 0x6C, 0xE0);
    snes_dsp_write(d, 0x7C, 0x00);
}

size_t snes_dsp_read_samples(SnesDsp *d, int16_t *out, size_t max_frames)
{
    const uint32_t mask = SNES_DSP_RING_FRAMES - 1u;
    size_t n = 0;
    while (n < max_frames && d->ring_count) {
        out[n * 2u] = d->ring[d->ring_read * 2u];
        out[n * 2u + 1u] = d->ring[d->ring_read * 2u + 1u];
        d->ring_read = (d->ring_read + 1u) & mask;
        d->ring_count--;
        n++;
    }
    return n;
}
