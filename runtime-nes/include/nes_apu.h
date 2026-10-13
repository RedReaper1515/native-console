/*
 * NES audio unit (Ricoh 2A03, NTSC): two pulse channels, triangle, noise,
 * the delta modulation channel (DMC), the frame counter, the non-linear
 * mixer and an output stage that resamples to the host rate.
 *
 * The APU does not depend on the machine or the CPU. The machine runs it
 * lazily (nes_apu_run) and catches it up before every register access.
 *
 * Time
 * ----
 * `cycle` counts CPU cycles since power. A register access is made while
 * `cycle` equals the CPU's own count *including* the cycle whose bus access
 * it is (the machine's bus functions count first, then access), so the
 * n-th CPU cycle is the one whose access happens at cycle == n. One step of
 * nes_apu_run is the APU's work that follows that access: the step that
 * advances cycle from n to n + 1 comes after the access of CPU cycle n and
 * before the access of cycle n + 1.
 *
 * The APU's own clock is half the CPU's. An access made while `cycle` is
 * odd is on a "get" cycle (the first half of an APU cycle, the half on
 * which DMAs read), an access at an even `cycle` is on a "put" cycle. The
 * machine must use the same parity for OAM DMA.
 *
 * Frame counter
 * -------------
 * The frame counter takes a $4017 write, and drops its IRQ flag after a
 * $4015 read, in put steps only (even ones: the step that follows a put
 * access). A $4017 write at cycle A restarts the sequence in step A + 2 if
 * A is even (put) and A + 3 if A is odd (get); with bit 7 set that step
 * also clocks the quarter and half frame units. This phase against the
 * DMA's get and put cycles is pinned by blargg's
 * cpu_interrupts_v2/4-irq_and_dma (after its sync_apu, a $4014 write of
 * the parity that restarts the frame counter without the extra cycle
 * starts a 514-cycle OAM DMA: it was a put cycle) and by AccuracyCoin's
 * three Frame Counter tests. Counting steps from the restart step as 0:
 *
 *   4-step: 7457 quarter, 14913 quarter + half, 22371 quarter,
 *           29828 IRQ flag, 29829 quarter + half + IRQ flag,
 *           29830 IRQ flag; this step is step 0 of the next sequence
 *   5-step: 7457 quarter, 14913 quarter + half, 22371 quarter,
 *           37281 quarter + half, 37282 = step 0 of the next sequence
 *
 * (the cycle numbers of the nesdev wiki's table, in CPU cycles). The flag
 * is raised on the first two of its three steps whatever the inhibit bit
 * says and takes the value "not inhibited" on the third; the IRQ line only
 * follows it while the inhibit bit is clear. A $4015 read returns the flag
 * and clears it in the first put step from the read on, before that step
 * raises it again, so a flag that is being set as it is read stays set.
 *
 * A change to a length counter's halt bit, or a reload of the counter,
 * written in the cycle whose step clocks the length counters, is applied
 * after the clock: the clock uses the old halt bit, and the reload is
 * dropped if the clock decremented the counter.
 *
 * DMC sample fetches
 * ------------------
 * See nes_apu_dmc_read below for the exact contract.
 *
 * Output
 * ------
 * Signed 16-bit mono samples at the rate set by nes_apu_set_rate (48000 Hz
 * unless changed) accumulate in a ring inside the state; the frontend
 * drains it with nes_apu_read_samples at least once per frame. When the
 * ring is full the oldest sample is dropped and counted in `overruns`.
 * Samples depend on emulated time only, and every stage (DAC tables, the
 * averaging decimator, the three first-order filters of the console, the
 * FIR decimator) is integer arithmetic, so the stream is bit-identical
 * across compilers and across any split of nes_apu_run into chunks.
 * The DAC's full range (0..1 in the wiki's formulas) spans 32768 output
 * units; one pulse channel at full volume is a square of about +-2450.
 *
 * The state holds no host pointers except `user` (the owner, for the DMC
 * callback): the machine is saved as one block. Tables are constants
 * outside the state.
 *
 * The frame counter's write delay and IRQ flag pipeline are ported from
 * ares (ISC license); see nes_apu.c.
 */
#ifndef NES_APU_H
#define NES_APU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The NTSC CPU clock is 39375000 / 22 Hz (1789772.72...). */
#define NES_APU_CPU_HZ_NUM 39375000u
#define NES_APU_CPU_HZ_DEN 22u

#define NES_APU_DEFAULT_RATE 48000u
#define NES_APU_MIN_RATE 8000u
#define NES_APU_MAX_RATE 192000u

#define NES_APU_RING_SAMPLES 8192u /* power of two */
#define NES_APU_FIR_TAPS 64u       /* power of two */

/* The largest value nes_apu_cycles_until_event returns. */
#define NES_APU_EVENT_MAX 0x100000u

/* Channels, for nes_apu_set_mute (a bit each) and nes_apu_channel_level. */
enum {
    NES_APU_PULSE1 = 0,
    NES_APU_PULSE2 = 1,
    NES_APU_TRIANGLE = 2,
    NES_APU_NOISE = 3,
    NES_APU_DMC = 4,
    NES_APU_CHANNELS = 5
};

/* Envelope generator (pulse 1, pulse 2, noise). */
typedef struct NesApuEnvelope {
    uint8_t start;    /* start flag: the next quarter frame reloads */
    uint8_t loop;     /* also the length counter halt bit */
    uint8_t constant; /* output `volume` instead of the decay level */
    uint8_t volume;   /* the 4-bit parameter: volume, or the divider period */
    uint8_t divider;
    uint8_t decay;    /* 15..0 */
} NesApuEnvelope;

/* Length counter (pulse 1, pulse 2, triangle, noise, in that order). */
typedef struct NesApuLength {
    uint8_t counter;
    uint8_t halt;
    uint8_t enabled;       /* $4015 bit */
    uint8_t halt_deferred; /* halt_next waits for the end of the step */
    uint8_t halt_next;
    uint8_t reload_deferred;
    uint8_t reload_index;
    uint8_t pad_;
} NesApuLength;

typedef struct NesApuPulse {
    uint16_t timer;  /* counts APU cycles, period..0 */
    uint16_t period; /* 11 bits; the sweep unit writes it too */
    uint8_t duty;
    uint8_t step;    /* sequencer position, counts down */
    uint8_t sweep_enable, sweep_period, sweep_negate, sweep_shift;
    uint8_t sweep_divider, sweep_reload;
    NesApuEnvelope env;
    uint8_t pad_[2];
} NesApuPulse;

typedef struct NesApuTriangle {
    uint16_t timer;  /* counts CPU cycles, period..0 */
    uint16_t period;
    uint8_t step;    /* 0..31: output 15..0 then 0..15 */
    uint8_t control; /* $4008 bit 7 (also the length counter halt bit) */
    uint8_t linear_reload;
    uint8_t linear;
    uint8_t reload_flag;
    uint8_t pad_[3];
} NesApuTriangle;

typedef struct NesApuNoise {
    uint16_t timer; /* counts APU cycles */
    uint16_t lfsr;  /* 15 bits */
    uint8_t mode;   /* 1 = short (feedback from bit 6) */
    uint8_t rate;
    NesApuEnvelope env;
} NesApuNoise;

typedef struct NesApuDmc {
    uint16_t timer;       /* counts APU cycles */
    uint16_t sample_addr; /* $C000 + $4012 * 64 */
    uint16_t sample_len;  /* $4013 * 16 + 1 */
    uint16_t addr;        /* memory reader: next byte */
    uint16_t remaining;   /* memory reader: bytes remaining */
    uint8_t rate, irq_enable, loop;
    uint8_t level;        /* 7-bit output level */
    uint8_t shift, bits, silence;
    uint8_t buffer, buffer_full;
    uint8_t irq;
    uint8_t fetch_delay; /* steps until the load fetch (0: none pending) */
    uint8_t pad_[3];
} NesApuDmc;

typedef struct NesApu {
    uint64_t cycle; /* CPU cycles since power */
    void *user;     /* owner; not touched after power (which clears it) */

    /* Output stage. */
    uint64_t rs_acc;   /* level x phase units inside the current window */
    int64_t hp1, hp2;  /* high-pass trackers (90 Hz, 440 Hz), Q30 */
    int64_t lp;        /* low-pass state (14 kHz), Q30 */
    int64_t fir[NES_APU_FIR_TAPS];
    uint32_t rs_phase; /* position inside the window, in phase units */
    uint32_t rs_inc;   /* phase units per CPU cycle */
    uint32_t rate;     /* output rate in Hz */
    uint32_t hp1_k, hp2_k, lp_k; /* filter coefficients, Q24 */
    uint32_t fir_pos;   /* next slot in fir[] */
    uint32_t fir_phase; /* 0..3: position in the 4:1 decimation */
    uint32_t ring_read, ring_count;
    uint32_t overruns;  /* samples dropped because the ring was full */
    uint32_t samples;   /* samples produced in all (wraps) */

    /* Frame counter. */
    uint32_t fc_pos;    /* steps since the sequence restarted */
    uint8_t fc_mode;    /* $4017 bit 7: 1 = 5-step */
    uint8_t fc_inhibit; /* $4017 bit 6 */
    uint8_t fc_reg;     /* last value written to $4017 */
    uint8_t fc_restart[2];     /* the write delay: [1] new, [0] one put step old */
    uint8_t fc_restart_now;    /* power or reset: restart in the next step */
    uint8_t frame_irq;         /* the frame interrupt flag */
    uint8_t irq_clear_pending; /* $4015 was read: clear on the next put step */
    uint8_t len_deferred;      /* some length counter has a deferred write */

    uint8_t mute; /* bit per channel: left out of the mix (debug/mod switch) */
    uint8_t pad_[6];

    NesApuLength len[4];
    NesApuPulse pulse[2];
    NesApuTriangle tri;
    NesApuNoise noise;
    NesApuDmc dmc;

    int16_t ring[NES_APU_RING_SAMPLES];
} NesApu;

/* Power: every byte of the state is initialised (user = NULL, 48000 Hz, no
 * mutes; set those again afterwards), registers as after writing $00 to
 * $4000-$4013, $4015 and $4017, noise LFSR = 1, DMC level 0, the frame
 * sequence restarting in the first step.
 *
 * Reset: $4015 is cleared (length counters zero, DMC stopped, DMC IRQ
 * clear), the frame IRQ flag is cleared, the last value written to $4017 is
 * written again (mode and inhibit kept; the sequence restarts in the next
 * step, clocking the units once in 5-step mode), the triangle's sequencer
 * returns to its first step (output 15), the DMC level keeps only bit 0.
 * Other registers, `cycle`, the rate, the mutes and the queued samples are
 * kept.
 *
 * Either way the sequence restarts as if $4017 had been written 10 or 11
 * cycles before the first instruction, given the CPU's 7-cycle reset
 * sequence (hardware: 9 to 12). */
void nes_apu_power(NesApu *a);
void nes_apu_reset(NesApu *a);

/* Advance by `cpu_cycles` CPU cycles. May call nes_apu_dmc_read. */
void nes_apu_run(NesApu *a, uint32_t cpu_cycles);

/* Registers. Only $4015 can be read; other addresses return 0 (the caller
 * supplies open bus). $4015 bit 5 is open bus too: it is returned as 0 and
 * the caller ORs in bit 5 of its data bus. The read clears the frame IRQ
 * flag (see above); nes_apu_peek returns the same value with no effect.
 * Writes outside $4000-$4013, $4015 and $4017 are ignored ($4014 and
 * $4016 belong to the machine). */
uint8_t nes_apu_read(NesApu *a, uint16_t addr);
uint8_t nes_apu_peek(const NesApu *a, uint16_t addr);
void nes_apu_write(NesApu *a, uint16_t addr, uint8_t value);

/* The IRQ line: (frame IRQ flag and not inhibited) or DMC IRQ flag. */
bool nes_apu_irq(const NesApu *a);
/* The frame counter's part of it. */
static inline bool nes_apu_frame_irq(const NesApu *a) { return a->frame_irq && !a->fc_inhibit; }

/* How far the APU may be run before anything it does can matter to the
 * machine: running fewer cycles than the returned number makes no
 * nes_apu_dmc_read call and leaves nes_apu_irq unchanged. The value is
 * exact or an underestimate, never an overestimate, at least 1 and at most
 * NES_APU_EVENT_MAX. It is valid until the next call that changes the APU
 * (run, read, write, reset), so ask again after each of them. */
uint32_t nes_apu_cycles_until_event(const NesApu *a);

/* The part of nes_apu_cycles_until_event that is the DMC's next fetch: the
 * nes_apu_dmc_read call is made in the n-th step from now, n being the
 * value returned (exact while no register is written; NES_APU_EVENT_MAX
 * when no fetch is in sight). */
uint32_t nes_apu_cycles_until_dmc(const NesApu *a);

/* The DMC's sample fetch, provided by the final program (the machine).
 * It returns the byte at `addr` ($8000-$FFFF) on the CPU's bus and sees to
 * the DMA's cycles; it must not call back into the APU.
 *
 * The call is made inside the step that advances `cycle` from s to s + 1
 * (`a->cycle` reads s during the call). It is the moment the DMA is
 * requested: the first CPU cycle that can be halted is cycle s + 1, the
 * one whose access the machine is about to make if it ran the APU no
 * further than nes_apu_cycles_until_event allowed. Three cases:
 *
 *   Reload (the usual one). The output unit emptied the sample buffer into
 *     its shift register. s is odd: the step of the DMC timer clock that
 *     ends an 8-bit output cycle (the timer clocks on every second step,
 *     always the odd ones). An unobstructed DMA is 4 cycles: halt at
 *     s + 1 (put), dummy at s + 2, alignment at s + 3, read at s + 4 (get).
 *   Load. $4015 was written at cycle A with bit 4 set while no bytes
 *     remained and the buffer was empty. s is even: A + 2 if A is even
 *     (put), A + 3 if A is odd (get). An unobstructed DMA is 3 cycles:
 *     halt at s + 1 (get), dummy at s + 2, read at s + 3 (get).
 *   Reload held back. $4015 started a sample while the buffer still held
 *     a byte, and the output unit took that byte before the step a load
 *     would have been made in: the reload waits for that step (the same
 *     s as a load), so it is 1 to 3 cycles late and on a get cycle.
 *
 * nes_apu_dmc_stall() gives that 4 or 3. A CPU that is writing cannot be
 * halted, which delays the halt and moves the alignment, and a fetch that
 * falls inside OAM DMA costs 1 to 3 cycles; the machine works those out
 * cycle by cycle (dma() in nes_bus.c) and only needs the request's step
 * from here.
 *
 * The APU takes the byte at once, in step s: buffer filled, address + 1
 * with $FFFF wrapping to $8000, bytes remaining - 1 and, at zero, the loop
 * restart or the IRQ. On hardware that happens on the DMA's read cycle,
 * but the CPU is halted from s + 1 until then, so its next access sees the
 * same state either way. The machine keeps the IRQ the last byte raises
 * off the line until the DMA's read cycle (nes_update_irq in nes.c). Only
 * a halt that CPU write cycles hold off shows a difference: a $4015 or
 * $4010 write in those cycles meets the state after the fetch, and a
 * sample stopped in them cannot abort the DMA as it does on hardware. */
uint8_t nes_apu_dmc_read(NesApu *a, uint16_t addr);

/* Inside nes_apu_dmc_read: the length of the unobstructed DMA. */
static inline unsigned nes_apu_dmc_stall(const NesApu *a) { return (a->cycle & 1u) ? 4u : 3u; }

/* Output rate in Hz (clamped to NES_APU_MIN_RATE..NES_APU_MAX_RATE). A
 * change restarts the output stage and drops the queued samples. */
void nes_apu_set_rate(NesApu *a, uint32_t hz);

/* Channels left out of the mix: bit NES_APU_PULSE1 .. NES_APU_DMC. The
 * channels keep running; only the samples change. */
void nes_apu_set_mute(NesApu *a, unsigned mask);

/* Drain up to `max` samples; returns the number written. */
size_t nes_apu_read_samples(NesApu *a, int16_t *dst, size_t max);
size_t nes_apu_samples_available(const NesApu *a);

/* What a channel is feeding the DAC right now (0..15, DMC 0..127),
 * ignoring the mute switches. For meters, mods and tests. */
unsigned nes_apu_channel_level(const NesApu *a, unsigned channel);

/* Tables. DAC outputs are fractions of full scale times 65536; the mixer's
 * output is pulse + tnd (nes_apu_tables.c describes how they are made). */
extern const uint8_t nes_apu_length_table[32];
extern const uint16_t nes_apu_noise_period[16]; /* CPU cycles */
extern const uint16_t nes_apu_dmc_period[16];   /* CPU cycles per bit */
extern const uint16_t nes_apu_pulse_dac[31];    /* [pulse1 + pulse2] */
extern const uint16_t nes_apu_tnd_dac[32768];   /* [dmc << 8 | triangle << 4 | noise] */
extern const int16_t nes_apu_fir[NES_APU_FIR_TAPS];

#ifdef __cplusplus
}
#endif

#endif /* NES_APU_H */
