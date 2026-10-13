/*
 * SNES audio unit: SPC700 CPU, S-DSP, timers, 64 KiB audio RAM and the
 * four communication ports shared with the S-CPU ($2140-$2143).
 *
 * The APU runs on its own clock. The machine (snes.c) converts S-CPU
 * master clocks to SMP cycles and calls snes_apu_run() to catch the APU
 * up, always before the S-CPU touches a port, so the interleaving of the
 * two CPUs is deterministic.
 *
 * The boot ROM at $FFC0-$FFFF is our own program implementing the
 * documented upload protocol (Nintendo's IPL ROM is not used).
 *
 * The struct holds no pointers: the machine is saved as one block.
 */
#ifndef SNES_APU_H
#define SNES_APU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SMP clock (the SPC700 runs one cycle per SMP_HZ tick) and the DSP's
 * output rate (one stereo sample every 32 SMP cycles). The resonator on
 * real consoles runs near 24.607 MHz / 24. */
#define SNES_SMP_HZ 1025280u
#define SNES_APU_RATE 32040u

typedef struct SnesApu SnesApu;

/* Power on / reset. Power fills audio RAM with a fixed pattern (so runs
 * are reproducible) and loads the boot ROM; reset keeps audio RAM. */
void snes_apu_power(SnesApu *apu);
void snes_apu_reset(SnesApu *apu);

/* Run until at least `smp_cycles` more SMP cycles have elapsed; returns
 * the number actually run (an instruction is never split). */
uint32_t snes_apu_run(SnesApu *apu, uint32_t smp_cycles);

/* The S-CPU side of the ports ($2140-$2143; port = address & 3). */
uint8_t snes_apu_cpu_read(const SnesApu *apu, unsigned port);
void snes_apu_cpu_write(SnesApu *apu, unsigned port, uint8_t value);

/* Drain interleaved stereo int16 frames at SNES_APU_RATE. */
size_t snes_apu_read_samples(SnesApu *apu, int16_t *out, size_t max_frames);

/* The 64 KiB audio RAM (for state dumps). */
const uint8_t *snes_apu_ram(const SnesApu *apu);

/* The full definition (snes_apu_state.h) is owned by the APU sources. */
#include "snes_apu_state.h"

#ifdef __cplusplus
}
#endif

#endif /* SNES_APU_H */
