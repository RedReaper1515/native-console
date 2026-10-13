/*
 * The DS sound unit (SPU): sixteen channels, the stereo mixer, the two
 * capture units and the 10-bit output stage (GBATEK "DS Sound").
 *
 * The unit is self-contained. The machine gives it register accesses for
 * 0x04000400-0x0400051F on the ARM7's bus and calls nds_spu_sample() once
 * per mixer period; the unit reads sample data and writes captured data
 * through the NdsSpuHost it is handed on each call. The sound power bit
 * (POWCNT2 bit 0, "Sound Speakers") is the machine's business. GBATEK only
 * names it; in melonDS it silences the output while the unit runs on. So
 * the machine keeps calling while the bit is clear and plays silence in
 * place of the frame.
 *
 * Time. One mixer period is NDS_CYCLES_PER_SAMPLE (1024) system cycles,
 * 32728.5 Hz. The channel and capture timers count at half the system
 * clock (16.756991 MHz), up from their reload value, and step their unit
 * each time they pass 0xFFFF: a call to nds_spu_sample() gives every timer
 * 512 ticks, runs the steps that fall due, and then mixes what each channel
 * is holding at the end of the period. A register write takes effect at
 * the next call, as if it had been made at the start of that period.
 *
 * Memory. A channel reads through an 8-word FIFO that it keeps at least
 * half full, in bursts of up to four words; a capture unit writes through a
 * 4-word FIFO that it empties when full and at the end of its buffer. All
 * bus traffic is therefore whole 32-bit words at word-aligned addresses
 * below 0x08000000: host->read(user, addr, 4) and host->write(user, addr,
 * value, 4), nothing else. Reads happen in nds_spu_sample() and, for the
 * first two bursts (32 bytes) of a sound, in the nds_spu_write() that sets
 * a channel's start bit. Writes happen only in nds_spu_sample(). Within one
 * call every channel reads before any capture unit writes. At the rates
 * games use (up to the mixer's own) a playing channel makes one burst of
 * four reads every few calls. The bound is set by the fastest timer value,
 * 512 samples in one call: 264 words read per channel, 259 written per
 * capture unit. See nds_spu.c for where the FIFO sizes come from and why
 * they matter.
 *
 * A sound or a capture buffer can lie anywhere in those 27 bits, I/O
 * included. If the bus sends such an access back here, to nds_spu_read()
 * or nds_spu_write() on the same NdsSpu from inside a callback, that is
 * allowed.
 *
 * Output. The hardware's result is a 10-bit level per side, the mixer
 * output after master volume plus SOUNDBIAS, clipped to 0..0x3FF. That
 * level is kept for nds_spu_dac(). The 16-bit host sample is that level
 * and nothing more:
 *
 *     out = (level - 0x200) * 64          -32768 .. +32704
 *
 * 0x200 is the SOUNDBIAS every game runs with, so silence is 0. Nothing is
 * filtered here: a different SOUNDBIAS shifts the host sample as it shifts
 * the level on the console (where the output is AC-coupled, so a frontend
 * that wants that applies its own high-pass). After nds_spu_reset()
 * SOUNDBIAS is 0, the power-on value; the firmware leaves 0x200, so a
 * direct boot has to write it.
 *
 * State. NdsSpu is plain data with no padding and no pointers: copy it
 * with memcpy to save and restore. nds_spu_reset() defines every byte.
 */
#ifndef NDS_SPU_H
#define NDS_SPU_H

#include "nds_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NDS_SPU_IO_BASE 0x04000400u
#define NDS_SPU_IO_END 0x04000520u /* one past the last register */

#define NDS_SPU_CHANNELS 16
#define NDS_SPU_CAPTURES 2
#define NDS_SPU_REG_WORDS ((NDS_SPU_IO_END - NDS_SPU_IO_BASE) / 4u) /* 72 */
#define NDS_SPU_CHANNEL_FIFO 32 /* bytes */
#define NDS_SPU_CAPTURE_FIFO 16 /* bytes */

/* Timer ticks in one mixer period. */
#define NDS_SPU_TICKS_PER_SAMPLE (NDS_CYCLES_PER_SAMPLE / 2)

/* Sample data is read from, and capture data written to, the ARM7's bus.
 * The unit only ever passes size 4. A NULL host, or a NULL callback, is
 * memory that reads 0 and takes no writes. */
typedef struct NdsSpuHost {
    uint32_t (*read)(void *user, uint32_t addr, int size); /* size 1, 2 or 4; little-endian value */
    void (*write)(void *user, uint32_t addr, uint32_t value, int size);
    void *user;
} NdsSpuHost;

/* What a channel is doing (NdsSpuChannel.phase). */
enum {
    NDS_SPU_OFF = 0, /* stopped; `sample` is 0 unless a one-shot's last sample is being held */
    NDS_SPU_ON = 1,  /* playing: the start/busy bit is set */
    NDS_SPU_TAIL = 2 /* the last sample period of a one-shot: busy already reads 0 */
};

typedef struct NdsSpuChannel {
    uint32_t timer; /* counts up from SOUNDxTMR while the channel runs; a step at 0x10000 */
    uint32_t pos;   /* the next sample: bytes (PCM8), halfwords (PCM16), nibbles counted from the
                       header (ADPCM) or duty steps (PSG) since the start */
    uint32_t fetch; /* byte offset from SOUNDxSAD of the next word the FIFO reads */
    int16_t sample; /* what the channel outputs now, before its volume stage */
    int16_t adpcm_value;
    int16_t adpcm_loop_value; /* decoder state as it stood on reaching the loop start */
    uint16_t lfsr;            /* the noise generator's shift register */
    uint8_t phase;
    uint8_t delay; /* sample periods still to pass before the first sample; 0 once playing */
    uint8_t adpcm_index;
    uint8_t adpcm_loop_index;
    uint8_t adpcm_byte; /* the data byte whose high nibble comes next */
    uint8_t fifo_read;  /* index of the oldest byte in fifo[] */
    uint8_t fifo_level; /* bytes waiting in fifo[] */
    uint8_t reserved;
    uint8_t fifo[NDS_SPU_CHANNEL_FIFO];
} NdsSpuChannel;

typedef struct NdsSpuCapture {
    uint32_t timer; /* counts up from SOUND1TMR (unit 0) or SOUND3TMR (unit 1) */
    uint32_t pos;   /* bytes taken since the start of the buffer */
    uint32_t flush; /* byte offset from SNDCAPxDAD of the next word the FIFO writes */
    uint8_t fifo_level;
    uint8_t reserved[3];
    uint8_t fifo[NDS_SPU_CAPTURE_FIFO];
} NdsSpuCapture;

typedef struct NdsSpu {
    /* The register file, 0x04000400-0x0400051F as little-endian 32-bit
     * words, holding only the bits that exist. The start/busy bits in
     * SOUNDxCNT and SNDCAPxCNT are kept here too. */
    uint32_t reg[NDS_SPU_REG_WORDS];
    NdsSpuChannel channel[NDS_SPU_CHANNELS];
    NdsSpuCapture capture[NDS_SPU_CAPTURES];
    uint16_t dac[2]; /* the last frame's 10-bit levels, left and right */
} NdsSpu;

/* Power-on state: every register 0, nothing playing. */
void nds_spu_reset(NdsSpu *spu);

/* Register access: addr is the full I/O address (0x04000400-0x0400051F),
 * size is 1, 2 or 4; any size at any offset. Bytes outside the range read
 * 0 and ignore writes, as do write-only registers when read. A write uses
 * the host only when it sets a channel's start bit. */
uint32_t nds_spu_read(const NdsSpu *spu, uint32_t addr, int size);
void nds_spu_write(NdsSpu *spu, const NdsSpuHost *host, uint32_t addr, uint32_t value, int size);

/* Advance by one mixer period (NDS_CYCLES_PER_SAMPLE = 1024 system cycles,
 * 32728.5 Hz) and produce one stereo frame. out[0] is left, out[1] right,
 * signed 16-bit, silence = 0. With out NULL the unit advances all the
 * same and the frame is only kept for nds_spu_dac(). */
void nds_spu_sample(NdsSpu *spu, const NdsSpuHost *host, int16_t out[2]);

/* The exact 10-bit level (0..0x3FF) the last frame put on the left
 * (side 0) or right (side 1) output, after bias and clipping. */
static inline uint32_t nds_spu_dac(const NdsSpu *spu, int side)
{
    return spu->dac[side & 1];
}

/* What a channel is outputting, before its volume stage: a signed 16-bit
 * sample, 0 while it is silent. For tests and channel viewers. */
static inline int nds_spu_channel_sample(const NdsSpu *spu, int channel)
{
    return spu->channel[channel & (NDS_SPU_CHANNELS - 1)].sample;
}

#ifdef __cplusplus
}
#endif

#endif /* NDS_SPU_H */
