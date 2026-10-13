/*
 * S-DSP (Sony CXD1222Q) state: 8 voices of BRR playback with Gaussian
 * interpolation, ADSR/GAIN envelopes, pitch modulation, noise, and the
 * 8-tap FIR echo whose buffer lives in audio RAM.
 *
 * The DSP is clocked once per SMP cycle (snes_dsp_clock); 32 clocks make
 * one stereo output sample (32040 Hz). The work done on each of the 32
 * clocks follows the hardware's pipeline, so writes by the SPC700 land
 * between the right steps.
 *
 * Output frames accumulate in a ring inside the state; the APU drains it
 * through snes_apu_read_samples. When the ring is full the oldest frame
 * is dropped (and counted in `overruns`).
 *
 * The state holds no pointers (the machine is saved as one block).
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#ifndef SNES_DSP_H
#define SNES_DSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_DSP_RING_FRAMES 4096u /* power of two */

enum { SNES_DSP_ENV_RELEASE = 0, SNES_DSP_ENV_ATTACK = 1, SNES_DSP_ENV_DECAY = 2, SNES_DSP_ENV_SUSTAIN = 3 };

typedef struct SnesDspVoice {
    int32_t envelope_latch;   /* last computed envelope (GAIN mode 7 two-slope quirk) */
    int16_t buffer[12];       /* decoded BRR samples, a ring of 12 */
    uint16_t pitch;           /* 14 bits */
    uint16_t gaussian_offset; /* position in the sample (0x1000 = one sample) */
    uint16_t brr_address;     /* current BRR block */
    uint16_t envelope;        /* 0..0x7FF */
    int8_t volume[2];
    uint8_t source, adsr0, adsr1, gain, envx;
    uint8_t index;            /* register base: voice << 4 */
    uint8_t buffer_offset;    /* 0..11: where the next samples are decoded */
    uint8_t brr_offset;       /* 1..8: byte within the BRR block */
    uint8_t keyon_delay;      /* 5..0 during the KON start-up */
    uint8_t envelope_mode;    /* SNES_DSP_ENV_* */
    uint8_t keyoff, modulate, noise, echo;
    /* Latched copies used by the pipeline. */
    uint8_t l_keylatch, l_keyon, l_keyoff, l_modulate, l_noise, l_echo, l_end, l_looped;
    uint8_t pad_[2];
} SnesDspVoice;

typedef struct SnesDsp {
    uint8_t regs[128];
    SnesDspVoice voice[8];

    /* Output ring: interleaved stereo frames. */
    int16_t ring[SNES_DSP_RING_FRAMES * 2u];
    uint32_t ring_read, ring_count;
    uint32_t overruns;
    uint32_t frames; /* total frames produced (wraps) */

    /* Main and echo mixing (17-bit accumulators hold 16-bit values). */
    int32_t main_output[2];
    int32_t echo_input[2];
    int32_t echo_output[2];
    int16_t history[2][8];
    uint16_t echo_address, echo_offset, echo_length;

    uint16_t counter;       /* global rate counter, 15 bits */
    uint16_t noise_lfsr;    /* 15 bits */
    uint16_t brr_address_l, brr_next_address_l;
    uint16_t latch_pitch;   /* 15 bits */
    int16_t latch_output;

    int8_t main_volume[2];
    int8_t echo_volume[2];
    int8_t echo_feedback;
    int8_t fir[8];
    uint8_t echo_page, echo_delay, echo_readonly;
    uint8_t echo_page_l, echo_readonly_l;
    uint8_t history_offset; /* 0..7 */
    uint8_t mute, soft_reset;
    uint8_t noise_frequency;
    uint8_t brr_bank, brr_bank_l, brr_source_l, brr_header_l, brr_byte_l;
    uint8_t latch_adsr0, latch_envx, latch_outx;
    uint8_t sample; /* toggles every sample; KON/KOFF are polled when set */
    uint8_t phase;  /* 0..31: the next clock within the sample */
    uint8_t pad_[3];
} SnesDsp;

/* Power: registers cleared except FLG = $E0 (soft reset, mute, echo
 * writes off), internal state reset. Reset: internal state reset, the
 * register values are kept and re-applied, FLG = $E0, ENDX = 0. */
void snes_dsp_power(SnesDsp *d);
void snes_dsp_reset(SnesDsp *d);

/* Register access ($F2/$F3 on the SPC700; addr is 0..127). */
uint8_t snes_dsp_read(const SnesDsp *d, uint8_t addr);
void snes_dsp_write(SnesDsp *d, uint8_t addr, uint8_t data);

/* One DSP clock (one SMP cycle). `aram` is the 64 KiB audio RAM. */
void snes_dsp_clock(SnesDsp *d, uint8_t *aram);

/* Drain up to max_frames frames from the ring. */
size_t snes_dsp_read_samples(SnesDsp *d, int16_t *out, size_t max_frames);

#ifdef __cplusplus
}
#endif

#endif /* SNES_DSP_H */
