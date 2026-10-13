/*
 * SnesApu definition: SPC700 + S-DSP + timers + 64 KiB audio RAM + the
 * four S-CPU communication ports. Owned by snes_apu.c / snes_dsp.c /
 * snes_spc700*.c; included at the end of snes_apu.h.
 *
 * The struct holds no pointers: the machine is saved as one block, and
 * two machines can run side by side.
 */
#ifndef SNES_APU_STATE_H
#define SNES_APU_STATE_H

#include <stdint.h>

#include "snes_dsp.h"
#include "snes_spc700.h"

/* One SMP timer. Timers 0/1 count an 8 kHz clock (every 128 SMP cycles),
 * timer 2 a 64 kHz clock (every 16); each divides by its target
 * ($FA-$FC, 0 = 256) into a 4-bit output ($FD-$FF, cleared on read). */
typedef struct SnesApuTimer {
    uint8_t stage0; /* prescaler, in half SMP cycles (2.048 MHz clocks) */
    uint8_t stage1; /* divided clock level; stage 2 counts its falling edges */
    uint8_t stage2; /* counts up to target */
    uint8_t stage3; /* 4-bit output */
    uint8_t line;   /* last gated stage-1 level (edge detector) */
    uint8_t enable; /* CONTROL bit */
    uint8_t target;
    uint8_t pad_;
} SnesApuTimer;

struct SnesApu {
    /* Audio RAM. Power fills it with 32-byte runs of $00 and $FF
     * alternating ($0000-$001F = $00, $0020-$003F = $FF, ...). */
    uint8_t aram[0x10000];

    SnesSpc700 smp;
    SnesDsp dsp;
    SnesApuTimer timer[3];

    uint8_t cpu_to_apu[4]; /* written by the S-CPU ($2140-3), read by the SPC700 ($F4-7) */
    uint8_t apu_to_cpu[4]; /* written by the SPC700 ($F4-7), read by the S-CPU ($2140-3) */

    uint8_t test;     /* $F0 TEST (power $0A) */
    uint8_t control;  /* $F1 CONTROL as last written (bit 7: boot ROM at $FFC0) */
    uint8_t dsp_addr; /* $F2 */
    uint8_t aux[2];   /* $F8/$F9 */
    uint8_t pad_[3];

    uint64_t cycles; /* SMP cycles since power */
};

#endif /* SNES_APU_STATE_H */
