/*
 * The DS sound unit (GBATEK "DS Sound"): sixteen channels that play PCM8,
 * PCM16 or IMA-ADPCM from memory, rectangular waves on channels 8-13 and
 * noise on 14-15; a stereo mixer; two capture units that record a mixer or
 * a channel back into memory; and the 10-bit output stage.
 *
 * Each channel and each capture unit has a 16-bit timer that counts up at
 * half the system clock from its reload value (SOUNDxTMR) and steps the
 * unit when it passes 0xFFFF. nds_spu_sample() gives every timer the 512
 * ticks of one mixer period, runs the steps that fall due, and mixes the
 * sample each channel is left holding. The mixer's arithmetic is GBATEK's
 * table "Channel/Mixer Bit-Widths", kept in the same fixed-point formats so
 * that the 10-bit result is the hardware's bit for bit.
 *
 * Memory. GBATEK's block diagrams ("DS Sound Block Diagrams") put a FIFO
 * between memory and every channel and capture unit but give no sizes. The
 * sizes here are melonDS's model of them: a channel holds 8 words, reads
 * its first 8 when it is started and 4 more whenever it is down to 4; a
 * capture unit holds 4 words and writes them out when it has 4, and at the
 * end of its buffer. The read-ahead is audible. Games play a capture
 * buffer back with channel 1 or 3 while the capture unit is still filling
 * it (reverb, surround): the channel has to fetch each part of the buffer
 * before the capture unit overwrites it, so that what it plays is one
 * whole buffer old. With the FIFOs it does: started together at one rate
 * in PCM16, the channel fetches each part two samples after the unit
 * wrote it on the lap before and 14 samples ahead of the unit writing it
 * again.
 *
 * Where GBATEK is silent, or the references disagree, the comment at that
 * spot names the reference that decided. In short:
 *   - repeat mode 0 plays on past the end and mode 3 loops (melonDS;
 *     DeSmuME stops in both);
 *   - write-only registers read 0 (melonDS);
 *   - clearing the master enable freezes channels and capture units
 *     (melonDS, DeSmuME);
 *   - a capture unit has its own counter and no start delay (melonDS);
 *   - the busy bit of a one-shot clears when its last sample starts,
 *     SOUNDxLEN is 22 bits, an ADPCM index above 88 steps by 0, captured
 *     data is rounded, channel addition and the capture bugs exist: all
 *     GBATEK, where melonDS differs or has nothing.
 *
 * Timing that is not modelled: a register write acts at the next mixer
 * period rather than at its own cycle, and the mixer is sampled once per
 * period (the hardware mixes at 1.05 MHz and then reduces that to the
 * output rate, GBATEK "DS Sound Control Registers").
 */
#include <string.h>

#include "nds_spu.h"

/* The state is saved, restored and compared as bytes: it has no padding.
 * The FIFOs are indexed modulo their size. */
_Static_assert(sizeof(NdsSpuChannel) == 28 + NDS_SPU_CHANNEL_FIFO, "NdsSpuChannel has padding");
_Static_assert(sizeof(NdsSpuCapture) == 16 + NDS_SPU_CAPTURE_FIFO, "NdsSpuCapture has padding");
_Static_assert(sizeof(NdsSpu) == NDS_SPU_REG_WORDS * 4 + NDS_SPU_CHANNELS * sizeof(NdsSpuChannel) +
                                     NDS_SPU_CAPTURES * sizeof(NdsSpuCapture) + 4,
               "NdsSpu has padding");
_Static_assert((NDS_SPU_CHANNEL_FIFO & (NDS_SPU_CHANNEL_FIFO - 1)) == 0, "channel FIFO size is not a power of two");
_Static_assert((NDS_SPU_CAPTURE_FIFO & (NDS_SPU_CAPTURE_FIFO - 1)) == 0, "capture FIFO size is not a power of two");

/* ------------------------------------------------------------------ */
/* The register file                                                   */
/* ------------------------------------------------------------------ */

/* Which bits of a 32-bit word of the register file read back, and which a
 * write stores. Bits outside `writable` do not exist and stay 0; registers
 * GBATEK marks (W) have nothing readable. GBATEK does not say what a
 * write-only sound register reads as on a DS: 0, as in melonDS. */
typedef struct SpuRegWord {
    uint32_t readable;
    uint32_t writable;
} SpuRegWord;

/* One channel, 0x040004x0-0x040004xF (GBATEK "DS Sound Channels 0..15"). */
static const SpuRegWord k_channel_words[4] = {
    /* x0 SOUNDxCNT (R/W): 0-6 volume, 8-9 divider, 15 hold, 16-22 pan,
     *    24-26 duty, 27-28 repeat, 29-30 format, 31 start/busy */
    { 0xFF7F837Fu, 0xFF7F837Fu },
    /* x4 SOUNDxSAD (W): 27-bit source address, word aligned */
    { 0x00000000u, 0x07FFFFFCu },
    /* x8 SOUNDxTMR (W): timer reload; xA SOUNDxPNT (W): loop start in words */
    { 0x00000000u, 0xFFFFFFFFu },
    /* xC SOUNDxLEN (W): length in words, bits 0-21 (melonDS keeps 21 bits) */
    { 0x00000000u, 0x003FFFFFu },
};

/* 0x04000500-0x0400051F (GBATEK "DS Sound Control Registers" and "DS Sound
 * Capture"). */
static const SpuRegWord k_control_words[8] = {
    /* 500 SOUNDCNT (R/W): 0-6 master volume, 8-9 left output from, 10-11
     *     right output from, 12 channel 1 not to mixer, 13 channel 3 not to
     *     mixer, 15 master enable */
    { 0x0000BF7Fu, 0x0000BF7Fu },
    /* 504 SOUNDBIAS (R/W): 0-9 */
    { 0x000003FFu, 0x000003FFu },
    /* 508 SNDCAP0CNT, 509 SNDCAP1CNT (R/W), a byte each: 0 add channel 1/3
     *     to channel 0/2, 1 source, 2 one-shot, 3 PCM8, 7 start/busy */
    { 0x00008F8Fu, 0x00008F8Fu },
    /* 50C nothing */
    { 0x00000000u, 0x00000000u },
    /* 510 SNDCAP0DAD (R/W): 27-bit destination, word aligned. It reads
     *     back as written, not as a running address (melonDS, DeSmuME). */
    { 0x07FFFFFCu, 0x07FFFFFCu },
    /* 514 SNDCAP0LEN (W): buffer length in words */
    { 0x00000000u, 0x0000FFFFu },
    /* 518 SNDCAP1DAD (R/W) */
    { 0x07FFFFFCu, 0x07FFFFFCu },
    /* 51C SNDCAP1LEN (W) */
    { 0x00000000u, 0x0000FFFFu },
};

/* Word indexes into NdsSpu.reg. Channel n's four words are 4n..4n+3. */
enum {
    REG_CHANNEL_WORDS = NDS_SPU_CHANNELS * 4,
    REG_SOUNDCNT = REG_CHANNEL_WORDS,
    REG_SOUNDBIAS = REG_CHANNEL_WORDS + 1,
    REG_SNDCAPCNT = REG_CHANNEL_WORDS + 2,
    REG_SNDCAP0DAD = REG_CHANNEL_WORDS + 4, /* + 2 for unit 1 */
    REG_SNDCAP0LEN = REG_CHANNEL_WORDS + 5  /* + 2 for unit 1 */
};

#define CNT_HOLD 0x00008000u
#define CNT_LOOP 0x08000000u     /* repeat mode bit 0 */
#define CNT_ONE_SHOT 0x10000000u /* repeat mode bit 1 */
#define CNT_BUSY 0x80000000u

enum { FORMAT_PCM8 = 0, FORMAT_PCM16 = 1, FORMAT_ADPCM = 2, FORMAT_PSG = 3 };

#define SOUNDCNT_NO_CH1 0x1000u /* channel 3's bit is this shifted left by one */
#define SOUNDCNT_ENABLE 0x8000u

#define CAP_ADD 0x01u
#define CAP_FROM_CHANNEL 0x02u
#define CAP_ONE_SHOT 0x04u
#define CAP_PCM8 0x08u
#define CAP_BUSY 0x80u

#define ADDRESS_MASK 0x07FFFFFCu
#define TIMER_OVERFLOW 0x10000u

static const SpuRegWord *reg_word(uint32_t index)
{
    return index < (uint32_t)REG_CHANNEL_WORDS ? &k_channel_words[index & 3u]
                                              : &k_control_words[(index - (uint32_t)REG_CHANNEL_WORDS) & 7u];
}

static inline uint32_t ch_cnt(const NdsSpu *spu, int n)
{
    return spu->reg[n * 4];
}

static inline uint32_t ch_sad(const NdsSpu *spu, int n)
{
    return spu->reg[n * 4 + 1];
}

static inline uint32_t ch_tmr(const NdsSpu *spu, int n)
{
    return spu->reg[n * 4 + 2] & 0xFFFFu;
}

/* Loop start and length are registers in words; the unit works in bytes. */
static inline uint32_t ch_loop_bytes(const NdsSpu *spu, int n)
{
    return (spu->reg[n * 4 + 2] >> 16) * 4u;
}

static inline uint32_t ch_end_bytes(const NdsSpu *spu, int n)
{
    return ch_loop_bytes(spu, n) + spu->reg[n * 4 + 3] * 4u;
}

static inline uint32_t cap_cnt(const NdsSpu *spu, int x)
{
    return (spu->reg[REG_SNDCAPCNT] >> (8 * x)) & 0xFFu;
}

/* GBATEK "7bit Volume and Panning Values": settings 0..126 stand for
 * 0..126 and 127 stands for 128, in 128ths. */
static inline uint32_t level7(uint32_t field)
{
    field &= 0x7Fu;
    return field == 127u ? 128u : field;
}

/* v / 2^n rounded down: an arithmetic shift right, spelled so that it does
 * not depend on what the compiler makes of >> on a negative value. */
static inline int64_t floor_shift(int64_t v, unsigned n)
{
    return v < 0 ? ~(~v >> n) : v >> n;
}

/* The 16-bit two's complement value in the low half of `v`. */
static inline int16_t signed16(uint32_t v)
{
    v &= 0xFFFFu;
    return (int16_t)(v >= 0x8000u ? (int32_t)v - 0x10000 : (int32_t)v);
}

static uint32_t bus_read_word(const NdsSpuHost *host, uint32_t addr)
{
    return host && host->read ? host->read(host->user, addr, 4) : 0u;
}

static void bus_write_word(const NdsSpuHost *host, uint32_t addr, uint32_t value)
{
    if (host && host->write) host->write(host->user, addr, value, 4);
}

/* ------------------------------------------------------------------ */
/* Channels: reading memory                                            */
/* ------------------------------------------------------------------ */

/* Read the next burst of a sound into the FIFO: up to four words, fewer
 * when the sound ends first. Past the end the reads go on at the loop
 * start, stop (one-shot), or simply continue into whatever follows the
 * sound in memory (repeat mode 0; see channel_step). */
static void fifo_fill(NdsSpu *spu, const NdsSpuHost *host, int n)
{
    NdsSpuChannel *ch = &spu->channel[n];
    uint32_t cnt = ch_cnt(spu, n);
    uint32_t end = ch_end_bytes(spu, n);
    uint32_t at = ch->fetch;
    uint32_t count = 16;

    if (ch->fifo_level > 16u) return; /* no room for a burst */
    /* GBATEK "DS Sound Channels 0..15": with a length (PNT+LEN) under 4
     * words the busy bit stays set and nothing is ever output. */
    if (end < 16u) return;
    if (at >= end) {
        if (cnt & CNT_LOOP) {
            at = ch_loop_bytes(spu, n);
            if (at >= end) return; /* a loop of no length: nothing to read */
        } else if (cnt & CNT_ONE_SHOT) {
            return;
        }
    }
    if (at < end && end - at < count) count = end - at;

    for (uint32_t i = 0; i < count; i += 4u) {
        uint32_t word = bus_read_word(host, (ch_sad(spu, n) + at + i) & ADDRESS_MASK);
        for (unsigned b = 0; b < 4u; b++) {
            ch->fifo[(ch->fifo_read + ch->fifo_level) & (NDS_SPU_CHANNEL_FIFO - 1u)] = (uint8_t)(word >> (8u * b));
            ch->fifo_level++;
        }
    }
    ch->fetch = at + count;
}

static uint32_t fifo_pop(NdsSpuChannel *ch)
{
    uint32_t byte = ch->fifo[ch->fifo_read & (NDS_SPU_CHANNEL_FIFO - 1u)];
    ch->fifo_read = (uint8_t)((ch->fifo_read + 1u) & (NDS_SPU_CHANNEL_FIFO - 1u));
    ch->fifo_level--;
    return byte;
}

/* ------------------------------------------------------------------ */
/* Channels: IMA-ADPCM, PSG, noise                                     */
/* ------------------------------------------------------------------ */

/* GBATEK "DS Sound Notes", IMA-ADPCM Format. */
static const uint16_t k_adpcm_step[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015,
    0x0017, 0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1,
    0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812,
    0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
    0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF,
};

static const int8_t k_adpcm_index_step[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* Decode one 4-bit value. The difference is built from shifted table
 * entries, each rounded down on its own ("in practice ... with
 * rounding-error"); adding clips at +7FFFh only and subtracting at -7FFFh
 * only, so a value of -8000h (possible from the header) survives an
 * addition. The header's index is 7 bits: GBATEK gives table entries
 * 89..127 as 0 (melonDS limits the header's index to 88 instead). */
static void adpcm_decode(NdsSpuChannel *ch, uint32_t nibble)
{
    int32_t step = ch->adpcm_index <= 88u ? (int32_t)k_adpcm_step[ch->adpcm_index] : 0;
    int32_t diff = step / 8;
    int32_t value = ch->adpcm_value;
    int32_t index = (int32_t)ch->adpcm_index + k_adpcm_index_step[nibble & 7u];

    if (nibble & 1u) diff += step / 4;
    if (nibble & 2u) diff += step / 2;
    if (nibble & 4u) diff += step;
    if (nibble & 8u) {
        value -= diff;
        if (value < -0x7FFF) value = -0x7FFF;
    } else {
        value += diff;
        if (value > 0x7FFF) value = 0x7FFF;
    }
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    ch->adpcm_value = (int16_t)value;
    ch->adpcm_index = (uint8_t)index;
}

/* GBATEK "DS Sound Notes", PSG Wave Duty: a cycle is eight steps, low
 * first. Duty N is high for the last N+1 steps, that is from step 7-N on;
 * duty 7 is never high. */
static const uint8_t k_duty_high_from[8] = { 7, 6, 5, 4, 3, 2, 1, 8 };

#define PSG_HIGH 0x7FFF
#define PSG_LOW (-0x7FFF)

/* GBATEK "DS Sound Notes", PSG Noise:
 *   X=X SHR 1, IF carry THEN Out=LOW, X=X XOR 6000h ELSE Out=HIGH */
static int16_t noise_step(NdsSpuChannel *ch)
{
    uint32_t x = ch->lfsr;
    bool carry = (x & 1u) != 0;

    x >>= 1;
    if (carry) x ^= 0x6000u;
    ch->lfsr = (uint16_t)x;
    return (int16_t)(carry ? PSG_LOW : PSG_HIGH);
}

/* The rectangular wave at step `pos` of its cycle. */
static int16_t duty_step(uint32_t cnt, uint32_t pos)
{
    return (int16_t)((pos & 7u) >= (uint32_t)k_duty_high_from[(cnt >> 24) & 7u] ? PSG_HIGH : PSG_LOW);
}

/* ------------------------------------------------------------------ */
/* Channels: playing                                                   */
/* ------------------------------------------------------------------ */

/* The start bit went from 0 to 1 (GBATEK "DS Sound Notes", Sound delayed
 * Start/Restart): PSG and noise start after 1 sample period, PCM after 3,
 * ADPCM after 11, which is the same 3 and then the 8 positions the header
 * occupies. The duty cycle restarts at the beginning of its low part and
 * the noise register at 7FFFh. A level still held from a one-shot stays
 * for the first of those periods. */
static void channel_start(NdsSpu *spu, const NdsSpuHost *host, int n)
{
    NdsSpuChannel *ch = &spu->channel[n];
    uint32_t cnt = ch_cnt(spu, n);

    ch->phase = NDS_SPU_ON;
    ch->timer = ch_tmr(spu, n);
    ch->pos = 0;
    ch->fetch = 0;
    ch->fifo_read = 0;
    ch->fifo_level = 0;
    ch->lfsr = 0x7FFF;
    ch->adpcm_value = 0;
    ch->adpcm_index = 0;
    ch->adpcm_loop_value = 0;
    ch->adpcm_loop_index = 0;
    ch->adpcm_byte = 0;
    if (!(cnt & CNT_HOLD)) ch->sample = 0;

    if (((cnt >> 29) & 3u) == FORMAT_PSG) {
        ch->delay = 1; /* "The FIFO isn't used in PSG/Noise modes" */
    } else {
        ch->delay = 3;
        fifo_fill(spu, host, n);
        fifo_fill(spu, host, n);
    }
}

/* One timer overflow of a running channel: it moves on to its next sample. */
static void channel_step(NdsSpu *spu, const NdsSpuHost *host, int n)
{
    NdsSpuChannel *ch = &spu->channel[n];
    uint32_t cnt = ch_cnt(spu, n);
    uint32_t format = (cnt >> 29) & 3u;
    uint32_t loop = ch_loop_bytes(spu, n);
    uint32_t end = ch_end_bytes(spu, n);
    uint32_t pos = ch->pos;
    uint32_t need;
    bool looped = false;

    if (ch->phase == NDS_SPU_TAIL) {
        /* GBATEK "DS Sound Notes", Sound Stop: the last sample of a
         * one-shot is output to the end of its period, or for as long as
         * the hold bit stays set. */
        ch->phase = NDS_SPU_OFF;
        if (!(cnt & CNT_HOLD)) ch->sample = 0;
        return;
    }
    if (ch->delay > 1u) {
        /* A start-up period. The output is zero from the second one on,
         * whatever was held before ("Hold Flag (appears useless/bugged)"). */
        ch->delay--;
        ch->sample = 0;
        return;
    }

    if (format == FORMAT_PSG) {
        /* Rectangular wave on channels 8-13, noise on 14-15, nothing on
         * the rest. "PSG sound is always Infinite": length and repeat mode
         * are not looked at. */
        if (n >= 14) ch->sample = noise_step(ch);
        else if (n >= 8) ch->sample = duty_step(cnt, pos);
        else ch->sample = 0;
        ch->pos = pos + 1u;
        ch->delay = 0;
        return;
    }

    /* PCM and ADPCM. Positions count samples: a byte is one PCM8 sample,
     * half a PCM16 sample, two ADPCM samples ("the first value in the
     * lower 4bits"), and ADPCM's positions include the 8 of its header. */
    if (end < 16u) return; /* under 4 words: busy for ever, no output (see fifo_fill) */
    if (format == FORMAT_PCM16) {
        loop /= 2u;
        end /= 2u;
    } else if (format == FORMAT_ADPCM) {
        loop *= 2u;
        end *= 2u;
    }

    if (pos >= end) {
        /* The end of the sound: "the first part (PNT) is played once, the
         * second part (LEN) is repeated infinitely", or the sound is over.
         * GBATEK names repeat mode 0 "Manual" and 3 "Prohibited" without
         * describing them. As in melonDS, the two bits act separately: bit
         * 0 loops, otherwise bit 1 stops, and with neither the channel
         * plays on through the memory that follows until it is stopped by
         * hand. (DeSmuME stops in modes 0 and 3.) */
        if (cnt & CNT_LOOP) {
            pos = loop;
            looped = true;
        } else if (cnt & CNT_ONE_SHOT) {
            /* Only reached when the length shrank under a playing sound;
             * otherwise the tail below has ended it already. */
            spu->reg[n * 4] = cnt & ~CNT_BUSY;
            ch->phase = NDS_SPU_OFF;
            if (!(cnt & CNT_HOLD)) ch->sample = 0;
            return;
        }
    }

    if (format == FORMAT_ADPCM) need = pos == 0u ? 4u : (pos >= 8u && !(pos & 1u)) ? 1u : 0u;
    else need = format == FORMAT_PCM16 ? 2u : 1u;
    if (ch->fifo_level < need) {
        fifo_fill(spu, host, n);
        if (ch->fifo_level < need) return; /* nothing has arrived: the channel waits where it is */
    }

    if (format == FORMAT_PCM8) {
        /* "The output volume of PCM8=NNh is equal to PCM16=NN00h." */
        ch->sample = signed16(fifo_pop(ch) << 8);
    } else if (format == FORMAT_PCM16) {
        uint32_t low = fifo_pop(ch);
        uint32_t high = fifo_pop(ch);
        ch->sample = signed16(low | (high << 8));
    } else if (pos == 0u) {
        /* The 32-bit header: bits 0-15 the initial value, 16-22 the
         * initial index. The channel stays silent for its 8 positions. */
        uint32_t header = fifo_pop(ch);
        header |= fifo_pop(ch) << 8;
        header |= fifo_pop(ch) << 16;
        header |= fifo_pop(ch) << 24;
        ch->adpcm_value = signed16(header);
        ch->adpcm_index = (uint8_t)((header >> 16) & 0x7Fu);
        ch->adpcm_loop_value = ch->adpcm_value;
        ch->adpcm_loop_index = ch->adpcm_index;
    } else if (pos >= 8u) {
        uint32_t nibble;

        if (looped) {
            /* "At the loop end, the SAD value is reloaded to the loop
             * start location, additionally index and pcm16 values are
             * reloaded to the values that have originally appeared at that
             * location." */
            ch->adpcm_value = ch->adpcm_loop_value;
            ch->adpcm_index = ch->adpcm_loop_index;
        }
        if (pos & 1u) {
            nibble = (uint32_t)ch->adpcm_byte >> 4;
        } else {
            ch->adpcm_byte = (uint8_t)fifo_pop(ch);
            nibble = ch->adpcm_byte & 15u;
        }
        if (pos == loop) {
            ch->adpcm_loop_value = ch->adpcm_value;
            ch->adpcm_loop_index = ch->adpcm_index;
        }
        adpcm_decode(ch, nibble);
        ch->sample = ch->adpcm_value;
    }
    if (need && ch->fifo_level <= 16u) fifo_fill(spu, host, n);

    ch->pos = pos + 1u;
    ch->delay = 0;

    /* GBATEK "DS Sound Notes", Sound Stop: "In one-shot mode, the Busy bit
     * gets cleared automatically at the BEGIN of the last sample period".
     * (melonDS clears it one period later, when the sample ends.) */
    if ((cnt & (CNT_LOOP | CNT_ONE_SHOT)) == CNT_ONE_SHOT && pos + 1u >= end) {
        spu->reg[n * 4] = cnt & ~CNT_BUSY;
        ch->phase = NDS_SPU_TAIL;
    }
}

/* Give a channel's timer one mixer period. */
static void channel_run(NdsSpu *spu, const NdsSpuHost *host, int n)
{
    NdsSpuChannel *ch = &spu->channel[n];
    uint32_t reload = ch_tmr(spu, n);

    if (ch->phase == NDS_SPU_OFF) return;
    ch->timer += NDS_SPU_TICKS_PER_SAMPLE;
    while (ch->timer >= TIMER_OVERFLOW) {
        ch->timer = ch->timer - TIMER_OVERFLOW + reload;
        channel_step(spu, host, n);
        if (ch->phase == NDS_SPU_OFF) break;
    }
}

/* GBATEK "Channel/Mixer Bit-Widths", steps 1 and 2: the volume divider
 * (1, 2, 4 or 16) leaves 4 fraction bits and the volume factor in 128ths
 * 7 more. The result is the channel after its volume stage, 16.11. */
static const uint8_t k_divider_scale[4] = { 16, 8, 4, 1 };

static int32_t channel_volume(const NdsSpu *spu, int n)
{
    uint32_t cnt = ch_cnt(spu, n);
    int32_t sample = spu->channel[n].sample;

    return sample * (int32_t)k_divider_scale[(cnt >> 8) & 3u] * (int32_t)level7(cnt);
}

/* Steps 3 and 4: panning in 128ths (left = 128 - N, right = N) and
 * "Rounding Down (strip 10bit)". The results are 16.8. */
static void channel_pan(const NdsSpu *spu, int n, int32_t level, int32_t side[2])
{
    int64_t right = (int64_t)level7(ch_cnt(spu, n) >> 16);

    side[0] = (int32_t)floor_shift((int64_t)level * (128 - right), 10);
    side[1] = (int32_t)floor_shift((int64_t)level * right, 10);
}

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

/* Write the FIFO's whole words to memory. */
static void capture_flush(NdsSpu *spu, const NdsSpuHost *host, int x)
{
    NdsSpuCapture *cap = &spu->capture[x];
    uint32_t dad = spu->reg[REG_SNDCAP0DAD + 2 * x];
    uint32_t words = cap->fifo_level / 4u;

    if (words > NDS_SPU_CAPTURE_FIFO / 4u) words = NDS_SPU_CAPTURE_FIFO / 4u;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t value = nds_get_le32(&cap->fifo[i * 4u]);
        bus_write_word(host, (dad + cap->flush) & ADDRESS_MASK, value);
        cap->flush += 4u;
    }
    cap->fifo_level = 0;
}

/* Take one sample of `bytes` bytes. Returns false if that ended a one-shot
 * capture. The buffer is SNDCAPxLEN words, 0 meaning 1 (GBATEK "DS Sound
 * Capture"); at its end the unit stops or starts again at SNDCAPxDAD. */
static bool capture_store(NdsSpu *spu, const NdsSpuHost *host, int x, uint32_t value, uint32_t bytes)
{
    NdsSpuCapture *cap = &spu->capture[x];
    uint32_t length = spu->reg[REG_SNDCAP0LEN + 2 * x];
    uint32_t total = (length ? length : 1u) * 4u;

    for (uint32_t i = 0; i < bytes; i++) {
        cap->fifo[cap->fifo_level & (NDS_SPU_CAPTURE_FIFO - 1u)] = (uint8_t)(value >> (8u * i));
        cap->fifo_level++;
        cap->pos++;
        if (cap->pos >= total) {
            capture_flush(spu, host, x);
            cap->pos = 0;
            cap->flush = 0;
            if (cap_cnt(spu, x) & CAP_ONE_SHOT) {
                spu->reg[REG_SNDCAPCNT] &= ~((uint32_t)CAP_BUSY << (8 * x));
                return false;
            }
        } else if (cap->fifo_level >= NDS_SPU_CAPTURE_FIFO) {
            capture_flush(spu, host, x);
        }
    }
    return true;
}

/* Give a capture unit one mixer period. `mixer` is its mixer's output
 * (20.8); `a` and `b` are channels 0 and 1 (unit 0) or 2 and 3 (unit 1)
 * after their volume stages (16.11).
 *
 * The unit has no frequency register of its own: it runs at channel 1's or
 * channel 3's SOUNDxTMR (GBATEK "DS Sound Capture"), here on its own
 * counter, started with the unit and without a start delay, as in melonDS.
 * GBATEK describes neither. */
static void capture_run(NdsSpu *spu, const NdsSpuHost *host, int x, int32_t mixer, int32_t a, int32_t b)
{
    NdsSpuCapture *cap = &spu->capture[x];
    uint32_t cnt = cap_cnt(spu, x);
    uint32_t reload = ch_tmr(spu, 2 * x + 1);
    int32_t level;
    unsigned fraction;
    uint32_t bytes = 2;

    if (!(cnt & CAP_BUSY)) return;
    cap->timer += NDS_SPU_TICKS_PER_SAMPLE;
    if (cap->timer < TIMER_OVERFLOW) return;

    if (cnt & CAP_FROM_CHANNEL) {
        /* The channel after its volume stage, before panning ("DS Sound
         * Block Diagrams"), with the two faults of "Capture Bugs": with
         * channel addition on, the sum keeps only 16 integer bits, so an
         * overflow loses the sign; with it off, the result is -8000h
         * whenever both channels are negative. */
        fraction = 11;
        if (cnt & CAP_ADD) {
            uint32_t sum = (uint32_t)(a + b) & 0x07FFFFFFu;
            level = sum & 0x04000000u ? (int32_t)sum - 0x08000000 : (int32_t)sum;
        } else {
            level = a < 0 && b < 0 ? -0x8000 * 0x800 : a;
        }
    } else {
        /* "Incoming mixer data (20.8bits) is clipped to 16.8bits". */
        fraction = 8;
        level = mixer < -0x800000 ? -0x800000 : mixer > 0x7FFFFF ? 0x7FFFFF : mixer;
    }
    if (cnt & CAP_PCM8) {
        /* "the 16.8 bits are divided by 100h" */
        fraction += 8;
        bytes = 1;
    }
    /* GBATEK "DS Sound Notes", Capture Clipping/Rounding: "If the MSB of
     * the fractional part is set, then data is rounded towards zero" and
     * the fraction is then dropped. That leaves a positive value rounded
     * down, and moves a negative one up when its fraction is a half or
     * more. (melonDS always rounds down.) */
    if (level < 0) level += (int32_t)1 << (fraction - 1u);
    level = (int32_t)floor_shift(level, fraction);

    while (cap->timer >= TIMER_OVERFLOW) {
        cap->timer = cap->timer - TIMER_OVERFLOW + reload;
        if (!capture_store(spu, host, x, (uint32_t)level, bytes)) break;
    }
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

/* SOUNDxCNT was written. `bytes` has FFh for each byte the access covered. */
static void channel_control_written(NdsSpu *spu, const NdsSpuHost *host, int n, uint32_t old, uint32_t bytes)
{
    NdsSpuChannel *ch = &spu->channel[n];
    uint32_t cnt = ch_cnt(spu, n);

    if ((cnt & CNT_BUSY) && !(old & CNT_BUSY)) {
        /* "A sound will be started/restarted when changing its start bit
         * from 0 to 1". The bit also reads 0 during the last sample of a
         * one-shot, so a sound can be restarted from there. */
        channel_start(spu, host, n);
        return;
    }
    if ((bytes & CNT_BUSY) && !(cnt & CNT_BUSY) && ch->phase != NDS_SPU_OFF) {
        /* Stopped by hand. The hold bit is for "after one-shot sound": it
         * keeps the level only if the sound had reached its last sample. */
        if (ch->phase == NDS_SPU_ON) ch->sample = 0;
        ch->phase = NDS_SPU_OFF;
    }
    /* A held level lasts "until Hold gets cleared". */
    if (ch->phase == NDS_SPU_OFF && !(cnt & CNT_HOLD)) ch->sample = 0;
}

/* SNDCAP0CNT or SNDCAP1CNT was written: a unit starts when its bit 7 goes
 * from 0 to 1. Stopping one needs nothing: what its FIFO still holds is
 * dropped by the next start. */
static void capture_control_written(NdsSpu *spu, uint32_t old)
{
    uint32_t now = spu->reg[REG_SNDCAPCNT];

    for (int x = 0; x < NDS_SPU_CAPTURES; x++) {
        uint32_t busy = (uint32_t)CAP_BUSY << (8 * x);
        NdsSpuCapture *cap = &spu->capture[x];

        if (!(now & busy) || (old & busy)) continue;
        cap->timer = ch_tmr(spu, 2 * x + 1);
        cap->pos = 0;
        cap->flush = 0;
        cap->fifo_level = 0;
    }
}

/* Store the bytes of one word of the register file that an access covers. */
static void write_word(NdsSpu *spu, const NdsSpuHost *host, uint32_t index, uint32_t value, uint32_t bytes)
{
    uint32_t mask = bytes & reg_word(index)->writable;
    uint32_t old = spu->reg[index];

    spu->reg[index] = (old & ~mask) | (value & mask);
    if (index < (uint32_t)REG_CHANNEL_WORDS && (index & 3u) == 0u) {
        channel_control_written(spu, host, (int)(index / 4u), old, bytes);
    } else if (index == (uint32_t)REG_SNDCAPCNT) {
        capture_control_written(spu, old);
    }
}

void nds_spu_write(NdsSpu *spu, const NdsSpuHost *host, uint32_t addr, uint32_t value, int size)
{
    int i = 0;

    if (size > 4) size = 4;
    /* One access can cover parts of two words of the register file. */
    while (i < size) {
        uint32_t word_addr = (addr + (uint32_t)i) & ~3u;
        uint32_t word = 0;
        uint32_t bytes = 0;

        do {
            uint32_t shift = ((addr + (uint32_t)i) & 3u) * 8u;
            word |= ((value >> (8 * i)) & 0xFFu) << shift;
            bytes |= 0xFFu << shift;
            i++;
        } while (i < size && ((addr + (uint32_t)i) & 3u) != 0u);

        if (word_addr >= NDS_SPU_IO_BASE && word_addr < NDS_SPU_IO_END) {
            write_word(spu, host, (word_addr - NDS_SPU_IO_BASE) / 4u, word, bytes);
        }
    }
}

uint32_t nds_spu_read(const NdsSpu *spu, uint32_t addr, int size)
{
    uint32_t value = 0;

    if (size > 4) size = 4;
    for (int i = 0; i < size; i++) {
        uint32_t byte_addr = addr + (uint32_t)i;
        uint32_t index;

        if (byte_addr < NDS_SPU_IO_BASE || byte_addr >= NDS_SPU_IO_END) continue;
        index = (byte_addr - NDS_SPU_IO_BASE) / 4u;
        value |= (((spu->reg[index] & reg_word(index)->readable) >> ((byte_addr & 3u) * 8u)) & 0xFFu) << (8 * i);
    }
    return value;
}

void nds_spu_reset(NdsSpu *spu)
{
    memset(spu, 0, sizeof *spu);
}

/* ------------------------------------------------------------------ */
/* Mixing                                                              */
/* ------------------------------------------------------------------ */

/* GBATEK "Channel/Mixer Bit-Widths", steps 6 to 9: master volume in
 * 128ths and a further division by 64 (14.21), the fraction stripped,
 * SOUNDBIAS added, and the result clipped to the 10 bits of the output. */
static uint16_t dac_level(int32_t line, uint32_t master, uint32_t bias)
{
    int64_t level = floor_shift((int64_t)line * (int64_t)master, 21) + (int64_t)bias;

    return (uint16_t)(level < 0 ? 0 : level > 0x3FF ? 0x3FF : level);
}

void nds_spu_sample(NdsSpu *spu, const NdsSpuHost *host, int16_t out[2])
{
    uint32_t soundcnt = spu->reg[REG_SOUNDCNT];
    uint32_t bias = spu->reg[REG_SOUNDBIAS];
    int32_t line[2] = { 0, 0 }; /* what goes to the master volume, 16.8 or wider */

    /* "BIAS output is always enabled, even when Master Enable
     * (SOUNDCNT.15) is off." GBATEK does not say what the channels do
     * meanwhile: they and the capture units stand still, as in melonDS and
     * DeSmuME. */
    if (soundcnt & SOUNDCNT_ENABLE) {
        int32_t level[NDS_SPU_CHANNELS];   /* each channel after its volume stage, 16.11 */
        int32_t side[NDS_SPU_CHANNELS][2]; /* after panning, 16.8 */
        int32_t mixer[2] = { 0, 0 };       /* the two mixers, 20.8 */
        int32_t pair[NDS_SPU_CAPTURES][2]; /* channels 0,1 and 2,3 as their capture unit sees them */

        for (int n = 0; n < NDS_SPU_CHANNELS; n++) channel_run(spu, host, n);
        for (int n = 0; n < NDS_SPU_CHANNELS; n++) level[n] = channel_volume(spu, n);

        /* GBATEK "DS Sound Capture", ch(a)+ch(b) Channel Addition: with
         * bits 0 and 7 of SNDCAPxCNT both set, channel 1 (3) is not output
         * as such but added to channel 0 (2) ahead of that channel's
         * panning. Towards the mixer the added channel is muted while the
         * other is not running, and by its own "not to mixer" bit. */
        for (int x = 0; x < NDS_SPU_CAPTURES; x++) {
            int a = 2 * x, b = 2 * x + 1;

            pair[x][0] = level[a];
            pair[x][1] = level[b];
            if ((cap_cnt(spu, x) & (CAP_ADD | CAP_BUSY)) != (CAP_ADD | CAP_BUSY)) continue;
            if (spu->channel[a].phase != NDS_SPU_OFF && !(soundcnt & (SOUNDCNT_NO_CH1 << x))) level[a] += level[b];
            level[b] = 0;
        }

        /* Step 5: the mixers add the panned channels. SOUNDCNT bits 12 and
         * 13 keep channels 1 and 3 out of both. */
        for (int n = 0; n < NDS_SPU_CHANNELS; n++) {
            channel_pan(spu, n, level[n], side[n]);
            if (n == 1 && (soundcnt & SOUNDCNT_NO_CH1)) continue;
            if (n == 3 && (soundcnt & (SOUNDCNT_NO_CH1 << 1))) continue;
            mixer[0] += side[n][0];
            mixer[1] += side[n][1];
        }

        /* Capture unit 0 hears the left mixer or channel 0, unit 1 the
         * right mixer or channel 2. They run after every channel has read
         * its samples for this period. */
        for (int x = 0; x < NDS_SPU_CAPTURES; x++) capture_run(spu, host, x, mixer[x], pair[x][0], pair[x][1]);

        /* SOUNDCNT bits 8-9 (left) and 10-11 (right): the output is the
         * mixer, channel 1, channel 3 or both of those. The block diagram
         * takes the two channels from ahead of the "not to mixer" switch,
         * so they can be heard here without being in the mixer. */
        for (int s = 0; s < 2; s++) {
            switch ((soundcnt >> (8 + 2 * s)) & 3u) {
            case 0: line[s] = mixer[s]; break;
            case 1: line[s] = side[1][s]; break;
            case 2: line[s] = side[3][s]; break;
            default: line[s] = side[1][s] + side[3][s]; break;
            }
        }
    }

    for (int s = 0; s < 2; s++) {
        spu->dac[s] = dac_level(line[s], level7(soundcnt), bias);
        /* The host sample is the 10-bit level around 0x200, the bias in
         * normal use, scaled to 16 bits. */
        if (out) out[s] = (int16_t)(((int32_t)spu->dac[s] - 0x200) * 64);
    }
}
