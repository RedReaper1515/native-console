/*
 * AX, the sound microcode, done on this side. See gc_ax.h for what this is
 * and docs/gc-dsp-ax.md parts B and C for where every number came from.
 *
 * The shape of a five-millisecond frame:
 *
 *   SETUP            nine 32-bit mixing buffers of 160 samples, primed from
 *                    a table in memory - not necessarily with silence
 *   SET_VOICE_LIST   the address of a linked list of voices
 *   PROCESS          decode, resample, shape and mix every voice into them
 *   COMPRESSOR       hold the main mix back when it gets too loud
 *   OUTPUT           write the finished mix where the audio DMA will find it
 *   END
 *
 * and everything else in the command table moves buffers between the mix and
 * the processor, which is how the library does reverb and streamed material.
 *
 * Three things in here would each produce something that runs and sounds
 * wrong, and all three are consequences of the format rather than choices:
 *
 *  - **A sample address is not a byte address.** It counts units of the
 *    access size: nibbles for ADPCM, bytes for 8-bit, halfwords for 16-bit.
 *    Treat them all as bytes and ADPCM plays at twice the rate and 16-bit
 *    PCM at half.
 *
 *  - **There is no fixed ADPCM coefficient table.** The sixteen coefficients
 *    are per-voice, carried in the parameter block, fitted to that sound when
 *    it was encoded. A decoder written against a constant table - which is
 *    what the handheld consoles have - decodes noise.
 *
 *  - **A command's argument count is not encoded anywhere.** The walker has
 *    to know the table, and one wrong count turns the rest of the list into
 *    garbage opcodes. That is why an unknown command stops the walk rather
 *    than being skipped.
 *
 * And four more that the first version had wrong, found by measuring Shrek 2
 * (docs/GC-PLAN.md, "Clean sound"). Each is a place where the behaviour
 * reference - Dolphin's DSP-HLE (`UCodes/AX.cpp`, `AXVoice.h`, `AXStructs.h`)
 * and its sample accelerator (`DSP/DSPAccelerator.cpp`), GPL-2.0, read for
 * behaviour and restated here, no code taken - says something the first
 * reading of it did not carry over:
 *
 *  - **Send volumes are unsigned.** 0x8000 is unity gain, and it is the value
 *    games use. Read as signed it is -1.0: every voice sent at unity played
 *    inverted against every voice sent a little below it.
 *
 *  - **The buffers the processor hands over are 32-bit**, like the mixing
 *    buffers - SET_MAIN_LR, UPLOAD_LRS and the auxiliary sends all move 160
 *    `int`s, not 160 halfwords.
 *
 *  - **A one-shot sound that ends stops producing samples** for the rest of
 *    its millisecond. The accelerator raises its end condition and returns
 *    nothing until it is restarted; decoding the last nibble again and again
 *    instead ran the predictor on its own output - a buzz at the end of every
 *    sound effect.
 *
 *  - **The de-pop values are in L, L, L / R, R, R / S, S, S order**, not in
 *    send order. The library reads them back when a voice stops and ramps the
 *    remainder out through SETUP; in the wrong order it ramped main right's
 *    last value into the left channel and so on - a step, heard as a click.
 */
#include "gc_ax.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_zelda.h"

/* Five milliseconds at 32 kHz, which is the unit everything here works in:
 * the mixing buffers are this long, a PROCESS mixes this many samples, and
 * OUTPUT writes this many frames. */
#define AX_SAMPLES 160u
#define AX_SUBFRAMES 5u
#define AX_SUBFRAME_SAMPLES 32u
#define AX_BUFFERS 9u
#define AX_UPDATES 32u

/* The parameter block, in halfwords.
 *
 * The doc gives the field order rather than the offsets, so these are the
 * running sum of the field widths - and the check that the sum is right is
 * that it closes at 122 halfwords, which is the 244 bytes the block is known
 * to be. A field of the wrong width would not land there. */
#define AX_PB_NEXT 0          /* 2: the next voice, or zero to end the list */
#define AX_PB_THIS 2          /* 2 */
#define AX_PB_SRC_TYPE 4
#define AX_PB_COEF_SELECT 5
#define AX_PB_MIXER_CONTROL 6
#define AX_PB_RUNNING 7
#define AX_PB_IS_STREAM 8
#define AX_PB_SENDS 9         /* 9 pairs of (volume, delta) */
#define AX_PB_DELAY 27        /* 7: the initial time delay, not implemented */
#define AX_PB_UPDATES 34      /* 5 counts, then a 32-bit address */
#define AX_PB_DPOP 41         /* 9 */
#define AX_PB_CUR_VOLUME 50
#define AX_PB_VOLUME_DELTA 51
/* 52..54 reserved */
#define AX_PB_LOOPING 55
#define AX_PB_FORMAT 56
#define AX_PB_LOOP_ADDR 57    /* 2 */
#define AX_PB_END_ADDR 59     /* 2 */
#define AX_PB_CUR_ADDR 61     /* 2 */
#define AX_PB_COEFS 63        /* 16 */
#define AX_PB_GAIN 79
#define AX_PB_PRED_SCALE 80
#define AX_PB_YN1 81
#define AX_PB_YN2 82
#define AX_PB_RATIO 83        /* 2, as 16.16 */
#define AX_PB_FRAC 85
#define AX_PB_LAST 86         /* 4 */
#define AX_PB_LOOP_PRED_SCALE 90
#define AX_PB_LOOP_YN1 91
#define AX_PB_LOOP_YN2 92
#define AX_PB_LPF_ON 93
#define AX_PB_LPF_YN1 94
#define AX_PB_LPF_A0 95
#define AX_PB_LPF_B0 96
#define AX_PB_LOOP_COUNTER 97
#define AX_PB_HALFWORDS 122

enum { AX_MAIN_L, AX_MAIN_R, AX_MAIN_S, AX_AUXA_L, AX_AUXA_R, AX_AUXA_S, AX_AUXB_L, AX_AUXB_R, AX_AUXB_S };

typedef struct GcAx {
  /* Main L, main R, main S, auxA L, auxA R, auxA S, auxB L, auxB R, auxB S,
   * in the order SETUP's table gives them. Thirty-two bits because they sum
   * many voices; each voice's own contribution is clamped to sixteen before
   * it is added, so the headroom is for the sum and not for one loud voice. */
  int32_t buffer[AX_BUFFERS][AX_SAMPLES];
  uint32_t voice_list;
  uint64_t lists;
  uint64_t bad_lists;
  /* How many voices were walked and how many of those were actually
   * playing. A game whose lists parse and whose voices are all stopped is a
   * different problem from one whose voices play the wrong thing, and the
   * two look identical from the far end. */
  uint64_t voices;
  uint64_t voices_running;
  /* The compressor's release position: how many frames of release are left
   * after the frame that last went over the threshold. Kept last, so a state
   * saved before it existed still loads (gc_ax_state_load). */
  uint32_t compressor_pos;
  uint32_t reserved;
  /* The other sound microcode's state (gc_zelda.h). It is here because this
   * block is the one piece of sound state a save state carries and the
   * machine's own structure cannot grow; it is last for the same reason
   * the compressor was. */
  GcZelda zelda;
} GcAx;

/* How big GcAx was before the compressor, and before the other microcode,
 * for loading older states. */
#define AX_STATE_SIZE_V1 (offsetof(GcAx, compressor_pos))
#define AX_STATE_SIZE_V2 (offsetof(GcAx, zelda))

static GcAx *ax_of(struct GcMachine *m) {
  if (!m) return NULL;
  if (!m->ax) m->ax = calloc(1, sizeof(GcAx));
  return (GcAx *)m->ax;
}

GcZelda *gc_ax_zelda(struct GcMachine *m) {
  GcAx *ax = ax_of(m);
  return ax ? &ax->zelda : NULL;
}

uint64_t gc_ax_lists(const struct GcMachine *m) {
  const GcAx *ax = m ? (const GcAx *)m->ax : NULL;
  return ax ? ax->lists : 0u;
}

uint64_t gc_ax_bad_lists(const struct GcMachine *m) {
  const GcAx *ax = m ? (const GcAx *)m->ax : NULL;
  return ax ? ax->bad_lists : 0u;
}

uint64_t gc_ax_voices(const struct GcMachine *m) {
  const GcAx *ax = m ? (const GcAx *)m->ax : NULL;
  return ax ? ax->voices : 0u;
}

uint64_t gc_ax_voices_running(const struct GcMachine *m) {
  const GcAx *ax = m ? (const GcAx *)m->ax : NULL;
  return ax ? ax->voices_running : 0u;
}

void gc_ax_reset(struct GcMachine *m) {
  if (!m) return;
  free(m->ax);
  m->ax = NULL;
}

/* --------------------------------------------------------------- memory */

static uint16_t read16(struct GcMachine *m, uint32_t address) {
  const uint8_t *p = gc_ram_at_register(m, address & ~1u, 2);
  return p ? (uint16_t)(((uint16_t)p[0] << 8) | p[1]) : 0u;
}

static void write16(struct GcMachine *m, uint32_t address, uint16_t value) {
  uint8_t *p = gc_ram_at_register(m, address & ~1u, 2);
  if (!p) return;
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static int32_t read32(struct GcMachine *m, uint32_t address) {
  return (int32_t)(((uint32_t)read16(m, address) << 16) | read16(m, address + 2u));
}

static void write32(struct GcMachine *m, uint32_t address, int32_t value) {
  write16(m, address, (uint16_t)((uint32_t)value >> 16));
  write16(m, address + 2u, (uint16_t)(uint32_t)value);
}

/* Sample data is not in main memory. The parameter blocks are - a voice list
 * is a linked list the game walks - but the sample addresses in them are
 * addresses in **audio memory**, which the DSP's sample accelerator reads and
 * the processor can only reach by DMA. Reading them out of main memory
 * instead decodes whatever the game keeps at those offsets, which is code
 * and data: full-scale static, the same on every build, with every command
 * list parsing perfectly.
 *
 * The decode is the ARAM DMA's (gc_io.c): 64 MB that repeats, of which the
 * first 16 MB are audio memory and the rest the empty expansion port. */
static uint8_t aram8(const struct GcMachine *m, uint32_t address) {
  const uint32_t at = address & 0x03ffffffu;
  return (m->aram && at < GC_ARAM_SIZE) ? m->aram[at] : 0u;
}

static uint32_t pair32(const uint16_t *h, unsigned index) {
  return ((uint32_t)h[index] << 16) | h[index + 1u];
}

static void set_pair32(uint16_t *h, unsigned index, uint32_t value) {
  h[index] = (uint16_t)(value >> 16);
  h[index + 1u] = (uint16_t)value;
}

static int16_t clamp16(int64_t v) {
  return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* ------------------------------------------------------ the accelerator */

/* What the sample format says. The three values AX actually uses are 0x0000
 * (4-bit ADPCM), 0x000a (16-bit PCM) and 0x0019 (8-bit PCM); the fields are
 * decoded rather than matched so that anything else at least behaves
 * consistently. */
#define AX_ACCESS(fmt) ((fmt) & 3u)
#define AX_DECODER(fmt) (((fmt) >> 2) & 3u)
#define AX_GAIN_SHIFT(fmt) (((fmt) >> 4) & 3u)

static unsigned gain_shift(uint16_t format) {
  switch (AX_GAIN_SHIFT(format)) {
    case 0: return 11u; /* divide by 2048 */
    case 1: return 0u;  /* divide by 1 */
    default: return 16u; /* divide by 65536 */
  }
}

/* The sample accelerator, loaded from a parameter block for one millisecond
 * and written back after it. It is a piece of hardware with registers - a
 * start (the loop address), an end, a current address, the predictor state -
 * and the microcode programs it from the block each millisecond, which is
 * why its state lives here rather than in the block directly. */
typedef struct Accel {
  uint32_t start, end, cur;
  uint16_t format, pred_scale;
  int16_t yn1, yn2, gain;
  /* Set by the end-of-sample condition and cleared by a write to yn2: until
   * then the accelerator returns zero and does not move. */
  int stopped;
} Accel;

static void accel_load(Accel *a, const uint16_t *h) {
  a->start = pair32(h, AX_PB_LOOP_ADDR) & 0x3fffffffu;
  a->end = pair32(h, AX_PB_END_ADDR) & 0x3fffffffu;
  /* Bit 31 of the current address selects writing, which voices never do;
   * it is kept so a block reads back as it was written. */
  a->cur = pair32(h, AX_PB_CUR_ADDR) & 0xbfffffffu;
  a->format = h[AX_PB_FORMAT];
  a->pred_scale = (uint16_t)(h[AX_PB_PRED_SCALE] & 0x7fu);
  a->yn1 = (int16_t)h[AX_PB_YN1];
  a->yn2 = (int16_t)h[AX_PB_YN2];
  a->gain = (int16_t)h[AX_PB_GAIN];
  a->stopped = 0; /* loading yn2 is a write to it */
}

static void accel_store(const Accel *a, uint16_t *h) {
  set_pair32(h, AX_PB_CUR_ADDR, a->cur);
  h[AX_PB_YN1] = (uint16_t)a->yn1;
  h[AX_PB_YN2] = (uint16_t)a->yn2;
  h[AX_PB_PRED_SCALE] = a->pred_scale;
}

/* The raw sample at the current address, in the access size. Unsigned: an
 * 8-bit sample is widened without its sign, and it is the truncation of the
 * scaled result to sixteen bits below that makes it signed again. */
static uint16_t accel_raw(const struct GcMachine *m, const Accel *a) {
  switch (AX_ACCESS(a->format)) {
    case 0: return (uint16_t)((aram8(m, a->cur >> 1) >> ((a->cur & 1u) ? 0 : 4)) & 0xfu);
    case 1: return aram8(m, a->cur);
    case 2: return (uint16_t)(((uint16_t)aram8(m, a->cur * 2u) << 8) | aram8(m, a->cur * 2u + 1u));
    default: return 0u;
  }
}

/* One sample out of the accelerator, advancing the current address and
 * handling the end of the sample the way the voice asks. */
static int16_t accel_read(struct GcMachine *m, Accel *a, uint16_t *h) {
  int16_t out;
  unsigned step = 2u;
  const int32_t coef_a = (int16_t)h[AX_PB_COEFS + ((a->pred_scale >> 4) & 7u) * 2u];
  const int32_t coef_b = (int16_t)h[AX_PB_COEFS + ((a->pred_scale >> 4) & 7u) * 2u + 1u];

  if (a->stopped) return 0;

  if (AX_DECODER(a->format) == 0u) {
    /* ADPCM. The 0x400 is half of the 2048 the shift divides by: a rounding
     * constant, and leaving it out biases every sample downwards. The result
     * is clamped rather than truncated, and it is the *clamped* value that
     * becomes the next history sample. */
    int32_t nibble = (int32_t)(accel_raw(m, a) & 0xfu);
    const int32_t scale = (int32_t)1 << (a->pred_scale & 0xfu);
    if (nibble >= 8) nibble -= 16;
    out = clamp16((int64_t)scale * nibble + ((0x400 + coef_a * a->yn1 + coef_b * a->yn2) >> 11));
    a->yn2 = a->yn1;
    a->yn1 = out;
    a->cur++;
    /* A frame is sixteen nibble addresses: a header byte and fourteen
     * samples. The header is read as soon as the address reaches a frame
     * boundary, and stepped over - so a block written back between frames
     * holds the new frame's predictor and an address past its header.
     *
     * Two end positions that land on a header behave differently in the
     * reference's account of the hardware, and it calls both hardware
     * behaviour: an end on the first header nibble loops one past the start
     * and an end on the second loops to the start one read early, and in
     * neither is the header read. A tool-chain sound never ends on a header,
     * so neither should come up. */
    if ((a->end & 15u) == 0u && a->cur == a->end) {
      a->cur = a->start + 1u;
    } else if ((a->end & 15u) == 1u && a->cur == a->end - 1u) {
      a->cur = a->start;
    } else if ((a->cur & 15u) == 0u) {
      a->pred_scale = (uint16_t)(aram8(m, (a->cur & ~15u) >> 1) & 0x7fu);
      a->cur += 2u;
      step += 2u;
    }
  } else {
    /* PCM, through the same datapath - which is why a format that appears
     * not to need a predictor still has one. With the coefficients zeroed
     * this is a plain gain stage. Truncated to sixteen bits, not clamped. */
    const unsigned shift = gain_shift(a->format);
    const int32_t raw = (int16_t)accel_raw(m, a);
    out = (int16_t)(((a->gain * raw) >> shift) + ((coef_a * a->yn1) >> shift) + ((coef_b * a->yn2) >> shift));
    a->yn2 = a->yn1;
    a->yn1 = out;
    if (AX_DECODER(a->format) != 1u) a->cur++;
  }

  /* The end of the sample: the read just made was the one at `end`, which
   * is inclusive. The accelerator goes back to the start address, raises its
   * end condition and stops; the microcode's handler decides what that
   * means. An address already past the end is not an end - the comparison
   * is for equality - which matters to a stream whose end the game moves. */
  if (a->cur == a->end + step - 1u) {
    a->cur = a->start;
    a->stopped = 1;
    if (!h[AX_PB_LOOPING]) {
      /* Stopped for good: nothing more this millisecond, and the voice is
       * skipped from the next one until the game starts it again. */
      h[AX_PB_RUNNING] = 0;
    } else {
      /* A loop reloads the predictor from the values the encoder computed
       * for the loop point - which is what makes the join seamless. A stream
       * keeps its history, because the game is refilling the buffer behind
       * the playhead and the waveform really is continuous, and counts the
       * wrap. Either way yn2 is written, which restarts the reads. */
      a->pred_scale = (uint16_t)(h[AX_PB_LOOP_PRED_SCALE] & 0x7fu);
      if (h[AX_PB_IS_STREAM] != 1u) {
        a->yn1 = (int16_t)h[AX_PB_LOOP_YN1];
        a->yn2 = (int16_t)h[AX_PB_LOOP_YN2];
      } else {
        h[AX_PB_LOOP_COUNTER] = (uint16_t)(h[AX_PB_LOOP_COUNTER] + 1u);
      }
      a->stopped = 0;
    }
  }
  return out;
}

/* --------------------------------------------------------------- a voice */

/* Which mixing buffer each of the nine sends feeds, and which de-pop slot
 * records it. The sends are ordered main L, main R, auxA L, auxA R, auxB L,
 * auxB R, auxB S, main S, auxA S - the last three out of pattern so that the
 * microcode can run one two-channel routine over adjacent pairs. The buffers
 * are in the plain order SETUP's table gives; the de-pop values are grouped
 * by channel: main, auxA, auxB left, then right, then surround. */
static const unsigned k_send_buffer[AX_BUFFERS] = {AX_MAIN_L, AX_MAIN_R, AX_AUXA_L, AX_AUXA_R, AX_AUXB_L,
                                                   AX_AUXB_R, AX_AUXB_S, AX_MAIN_S, AX_AUXA_S};
static const unsigned k_send_dpop[AX_BUFFERS] = {0u, 3u, 1u, 4u, 2u, 5u, 8u, 6u, 7u};

/* Which sends `mixer_control` turns on, and which of them ramp. This is the
 * modern GameCube encoding; the oldest microcode revision uses a completely
 * different and much smaller one, which is not implemented here - a machine
 * that applied this table to that revision would route almost everything
 * wrongly, so it is better to be wrong in a way that is written down. */
static void send_enables(uint16_t control, int on[AX_BUFFERS], int ramp[AX_BUFFERS]) {
  unsigned i;
  for (i = 0; i < AX_BUFFERS; i++) on[i] = ramp[i] = 0;
  on[0] = (control & 0x0001u) != 0;  /* main L */
  on[1] = (control & 0x0002u) != 0;  /* main R */
  on[7] = (control & 0x0004u) != 0;  /* main S */
  ramp[0] = ramp[1] = ramp[7] = (control & 0x0008u) != 0;
  on[2] = (control & 0x0010u) != 0;  /* auxA L */
  on[3] = (control & 0x0020u) != 0;  /* auxA R */
  ramp[2] = ramp[3] = (control & 0x0040u) != 0;
  on[8] = (control & 0x0080u) != 0;  /* auxA S */
  ramp[8] = (control & 0x0100u) != 0;
  on[4] = (control & 0x0200u) != 0;  /* auxB L */
  on[5] = (control & 0x0400u) != 0;  /* auxB R */
  ramp[4] = ramp[5] = (control & 0x0800u) != 0;
  on[6] = (control & 0x1000u) != 0;  /* auxB S */
  ramp[6] = (control & 0x2000u) != 0;
}

/* The updates a game can ask for part-way through a frame. The block is read
 * once and five milliseconds are mixed, so a game that needs finer timing
 * than that - note timing, mostly - leaves a list of (field, value) pairs to
 * be applied between them. The offsets are raw halfword indices into the
 * block, which is why its field order is load-bearing.
 *
 * The array is read once, when the block is, from the address the block had
 * then. Millisecond n's entries start after the counts of the ones before
 * it; a count that would run past the 32 entries applies none of them. */
static void apply_updates(uint16_t *h, const uint16_t counts[AX_SUBFRAMES],
                          const uint16_t updates[AX_UPDATES * 2u], unsigned subframe) {
  unsigned start = 0, i;
  for (i = 0; i < subframe; i++) start += counts[i];
  if (start >= AX_UPDATES || counts[subframe] > AX_UPDATES - start) return;
  for (i = start; i < start + counts[subframe]; i++) {
    if (updates[i * 2u] < AX_PB_HALFWORDS) h[updates[i * 2u]] = updates[i * 2u + 1u];
  }
}

/* One voice, one millisecond: thirty-two samples fetched, shaped and sent. */
static void mix_subframe(struct GcMachine *m, GcAx *ax, uint16_t *h, unsigned at) {
  const uint32_t ratio = pair32(h, AX_PB_RATIO);
  int on[AX_BUFFERS], ramp[AX_BUFFERS];
  int16_t shaped[AX_SUBFRAME_SAMPLES];
  unsigned i, s;
  Accel acc;

  accel_load(&acc, h);

  if (h[AX_PB_SRC_TYPE] == 2u) {
    /* Nearest: no conversion at all. One input sample per output sample,
     * whatever the ratio says, and the history keeps the last four. */
    for (i = 0; i < AX_SUBFRAME_SAMPLES; i++) shaped[i] = accel_read(m, &acc, h);
    for (i = 0; i < 4u; i++) h[AX_PB_LAST + i] = (uint16_t)shaped[AX_SUBFRAME_SAMPLES - 4u + i];
  } else {
    /* Linear. The ratio is 16.16, input rate over output rate. For each
     * output sample the position steps first, every whole input sample it
     * crosses is fetched into the four-sample history, and then the output
     * is weighed from the two oldest of the four by the fraction.
     *
     * `src_type` 0 is a four-tap polyphase filter whose coefficients live in
     * the sound processor's own data ROM, which this machine does not have
     * and will not ship; the reference falls back to linear without the ROM
     * and so does this. Shrek 2 asks for linear itself. */
    uint32_t pos = h[AX_PB_FRAC];
    for (i = 0; i < AX_SUBFRAME_SAMPLES; i++) {
      uint32_t frac;
      pos += ratio;
      while (pos >= 0x10000u) {
        h[AX_PB_LAST + 0u] = h[AX_PB_LAST + 1u];
        h[AX_PB_LAST + 1u] = h[AX_PB_LAST + 2u];
        h[AX_PB_LAST + 2u] = h[AX_PB_LAST + 3u];
        h[AX_PB_LAST + 3u] = (uint16_t)accel_read(m, &acc, h);
        pos -= 0x10000u;
      }
      frac = pos & 0xffffu;
      if (frac) {
        const int32_t s0 = (int16_t)h[AX_PB_LAST + 0u];
        const int32_t s1 = (int16_t)h[AX_PB_LAST + 1u];
        shaped[i] = (int16_t)(((int64_t)s0 * (int64_t)(0x10000u - frac) + (int64_t)s1 * frac) >> 16);
      } else {
        shaped[i] = (int16_t)h[AX_PB_LAST + 0u];
      }
    }
    h[AX_PB_FRAC] = (uint16_t)pos;
  }
  accel_store(&acc, h);

  /* The volume envelope, a per-sample linear ramp over the whole voice
   * before any send. Signed on this console - the one volume that is. */
  {
    int16_t volume = (int16_t)h[AX_PB_CUR_VOLUME];
    const int16_t delta = (int16_t)h[AX_PB_VOLUME_DELTA];
    for (i = 0; i < AX_SUBFRAME_SAMPLES; i++) {
      shaped[i] = clamp16(((int32_t)shaped[i] * volume) >> 15);
      volume = (int16_t)(uint16_t)((uint16_t)volume + (uint16_t)delta);
    }
    h[AX_PB_CUR_VOLUME] = (uint16_t)volume;
  }

  /* A one-pole low-pass. The coefficient on the input is unsigned. */
  if (h[AX_PB_LPF_ON]) {
    const int32_t a0 = h[AX_PB_LPF_A0];
    const int32_t b0 = (int16_t)h[AX_PB_LPF_B0];
    int16_t yn1 = (int16_t)h[AX_PB_LPF_YN1];
    for (i = 0; i < AX_SUBFRAME_SAMPLES; i++) {
      yn1 = clamp16(((int64_t)a0 * shaped[i] + (int64_t)b0 * yn1) >> 15);
      shaped[i] = yn1;
    }
    h[AX_PB_LPF_YN1] = (uint16_t)yn1;
  }

  send_enables(h[AX_PB_MIXER_CONTROL], on, ramp);

  /* The sends. The volume is **unsigned**, 0x8000 being unity, and the delta
   * wraps with it. Each sample is clamped to sixteen bits before it is added,
   * so the thirty-two bits of headroom are for summing voices rather than
   * for one loud one; the last value added is the send's de-pop value, which
   * the library reads back to ramp out a voice it stops. */
  for (s = 0; s < AX_BUFFERS; s++) {
    uint16_t send_volume = h[AX_PB_SENDS + s * 2u];
    const uint16_t send_delta = ramp[s] ? h[AX_PB_SENDS + s * 2u + 1u] : 0u;
    int16_t last = 0;
    int32_t *out = ax->buffer[k_send_buffer[s]] + at;
    if (!on[s]) continue;
    for (i = 0; i < AX_SUBFRAME_SAMPLES; i++) {
      last = clamp16(((int64_t)shaped[i] * send_volume) >> 15);
      out[i] += last;
      send_volume = (uint16_t)(send_volume + send_delta);
    }
    h[AX_PB_SENDS + s * 2u] = send_volume;
    h[AX_PB_DPOP + k_send_dpop[s]] = (uint16_t)last;
  }
}

/* Every voice in the list, five milliseconds each. A block is read once and
 * written back once; the five milliseconds in between are separate because
 * the per-millisecond updates go between them - and every millisecond's
 * updates are applied whether or not the voice is running, since an update
 * can be what starts it. */
static void process_voices(struct GcMachine *m, GcAx *ax) {
  uint32_t address = ax->voice_list;
  unsigned guard = 0;

  while (address && guard++ < 256u) {
    uint16_t h[AX_PB_HALFWORDS];
    uint16_t counts[AX_SUBFRAMES];
    uint16_t updates[AX_UPDATES * 2u];
    uint32_t updates_at;
    unsigned i;
    int ran = 0;
    for (i = 0; i < AX_PB_HALFWORDS; i++) h[i] = read16(m, address + i * 2u);
    for (i = 0; i < AX_SUBFRAMES; i++) counts[i] = h[AX_PB_UPDATES + i];
    updates_at = pair32(h, AX_PB_UPDATES + 5u);
    for (i = 0; i < AX_UPDATES * 2u; i++) updates[i] = updates_at ? read16(m, updates_at + i * 2u) : 0u;

    ax->voices++;
    for (i = 0; i < AX_SUBFRAMES; i++) {
      apply_updates(h, counts, updates, i);
      if (h[AX_PB_RUNNING] != 1u) continue;
      ran = 1;
      mix_subframe(m, ax, h, i * AX_SUBFRAME_SAMPLES);
    }
    if (ran) ax->voices_running++;

    for (i = 0; i < AX_PB_HALFWORDS; i++) write16(m, address + i * 2u, h[i]);
    address = pair32(h, AX_PB_NEXT);
  }
}

/* ----------------------------------------------------------- the commands */

/* SETUP: nine triples of (value_hi, value_lo, delta). A zero 32-bit value
 * zeroes the buffer; anything else starts it at that value and adds the
 * signed delta per sample - so a frame does not necessarily begin from
 * silence. This is where the library's de-pop goes: the last values of the
 * voices it stopped, ramped towards zero across the next frame. */
static void command_setup(struct GcMachine *m, GcAx *ax, uint32_t address) {
  unsigned b, i;
  for (b = 0; b < AX_BUFFERS; b++) {
    const int32_t value = read32(m, address + b * 6u);
    const int32_t delta = (int16_t)read16(m, address + b * 6u + 4u);
    if (!value) {
      memset(ax->buffer[b], 0, sizeof(ax->buffer[b]));
      continue;
    }
    for (i = 0; i < AX_SAMPLES; i++) ax->buffer[b][i] = (int32_t)((uint32_t)value + (uint32_t)((int32_t)i * delta));
  }
}

/* Three buffers out to memory, and three back in and added: the auxiliary
 * busses. The library takes a bus's mix, runs its reverb over it on the
 * processor, and hands the result back to be added into the main mix a frame
 * later. `write_at` zero is the variant that only reads. */
static void command_mix_aux(struct GcMachine *m, GcAx *ax, unsigned bus, uint32_t write_at, uint32_t read_at) {
  const unsigned first = bus ? AX_AUXB_L : AX_AUXA_L;
  unsigned b, i;
  if (write_at) {
    for (b = 0; b < 3u; b++) {
      for (i = 0; i < AX_SAMPLES; i++) write32(m, write_at + (b * AX_SAMPLES + i) * 4u, ax->buffer[first + b][i]);
    }
  }
  for (b = 0; b < 3u; b++) {
    for (i = 0; i < AX_SAMPLES; i++) {
      ax->buffer[AX_MAIN_L + b][i] += read32(m, read_at + (b * AX_SAMPLES + i) * 4u);
    }
  }
}

/* DOWNLOAD_AND_MIX: nine buffers from memory, each group of three scaled by
 * its own unsigned volume and added into main, auxA and auxB. */
static void command_download_and_mix(struct GcMachine *m, GcAx *ax, uint32_t address, const uint16_t volumes[3]) {
  unsigned g, b, i;
  for (g = 0; g < 3u; g++) {
    for (b = 0; b < 3u; b++) {
      for (i = 0; i < AX_SAMPLES; i++) {
        const int64_t sample = read32(m, address + ((g * 3u + b) * AX_SAMPLES + i) * 4u);
        ax->buffer[g * 3u + b][i] += (int32_t)((sample * volumes[g]) >> 15);
      }
    }
  }
}

/* MIX_AUXB_LR: auxB left and right out, then two buffers in that replace
 * them and are added into main left and right. */
static void command_mix_auxb_lr(struct GcMachine *m, GcAx *ax, uint32_t up, uint32_t down) {
  unsigned c, i;
  for (c = 0; c < 2u; c++) {
    for (i = 0; i < AX_SAMPLES; i++) write32(m, up + (c * AX_SAMPLES + i) * 4u, ax->buffer[AX_AUXB_L + c][i]);
  }
  for (c = 0; c < 2u; c++) {
    for (i = 0; i < AX_SAMPLES; i++) {
      const int32_t sample = read32(m, down + (c * AX_SAMPLES + i) * 4u);
      ax->buffer[AX_AUXB_L + c][i] = sample;
      ax->buffer[AX_MAIN_L + c][i] += sample;
    }
  }
}

/* SEND_AUX_AND_MIX: auxA's three buffers and auxB surround out; then four in,
 * added into main L, main R, auxB L and auxB R. */
static void command_send_aux_and_mix(struct GcMachine *m, GcAx *ax, const uint32_t addr[6]) {
  static const unsigned k_down[4] = {AX_MAIN_L, AX_MAIN_R, AX_AUXB_L, AX_AUXB_R};
  unsigned b, i;
  for (b = 0; b < 3u; b++) {
    for (i = 0; i < AX_SAMPLES; i++) write32(m, addr[0] + (b * AX_SAMPLES + i) * 4u, ax->buffer[AX_AUXA_L + b][i]);
  }
  for (i = 0; i < AX_SAMPLES; i++) write32(m, addr[1] + i * 4u, ax->buffer[AX_AUXB_S][i]);
  for (b = 0; b < 4u; b++) {
    for (i = 0; i < AX_SAMPLES; i++) ax->buffer[k_down[b]][i] += read32(m, addr[2u + b] + i * 4u);
  }
}

/* COMPRESSOR: a limiter over main L and R, with no look-ahead, driven by a
 * table of gain ramps the game supplies.
 *
 * If any sample of either channel in this frame is louder than the threshold,
 * this frame is scaled by an *attack* ramp chosen by where the release had
 * got to, and the release starts over at its full length. Otherwise, while a
 * release is running, it steps one frame closer to its end and the frame is
 * scaled by that step's *release* ramp. A ramp is 160 unsigned gains, 0x8000
 * being unity, one per sample; the table holds eleven attack ramps and the
 * release ramps after them. Nothing happens at all when neither applies.
 *
 * Behaviour as the reference describes it (Dolphin, `AXUCode::RunCompressor`,
 * read for behaviour only). **Unverified** beyond that: what an attack does
 * when the release position is past the eleven attack ramps - here, as
 * there, it simply reads that far into the table. */
static void command_compressor(struct GcMachine *m, GcAx *ax, uint16_t threshold, uint16_t release_frames,
                               uint32_t table) {
  unsigned i, entry;
  int triggered = 0;
  for (i = 0; i < AX_SAMPLES && !triggered; i++) {
    const int64_t l = ax->buffer[AX_MAIN_L][i], r = ax->buffer[AX_MAIN_R][i];
    if ((l < 0 ? -l : l) > threshold || (r < 0 ? -r : r) > threshold) triggered = 1;
  }
  if (triggered) {
    entry = ax->compressor_pos;
    ax->compressor_pos = release_frames;
  } else if (ax->compressor_pos) {
    ax->compressor_pos--;
    entry = 11u + ax->compressor_pos;
  } else {
    return;
  }
  for (i = 0; i < AX_SAMPLES; i++) {
    const int64_t gain = read16(m, table + (entry * AX_SAMPLES + i) * 2u);
    ax->buffer[AX_MAIN_L][i] = (int32_t)((ax->buffer[AX_MAIN_L][i] * gain) >> 15);
    ax->buffer[AX_MAIN_R][i] = (int32_t)((ax->buffer[AX_MAIN_R][i] * gain) >> 15);
  }
}

/* OUTPUT: the surround buffer as 160 unclamped 32-bit values, and the stereo
 * buffer as 160 frames of two 16-bit samples - **right first, then left**,
 * which is the one order nobody guesses. **Unverified:** that channel order
 * rests on a second implementation rather than a measurement, and getting it
 * backwards gives a perfectly plausible stereo image with the channels
 * swapped. docs/gc-dsp-ax.md D.2 says what would settle it. */
static void command_output(struct GcMachine *m, GcAx *ax, uint32_t surround, uint32_t stereo) {
  unsigned i;
  for (i = 0; i < AX_SAMPLES; i++) write32(m, surround + i * 4u, ax->buffer[AX_MAIN_S][i]);
  for (i = 0; i < AX_SAMPLES; i++) {
    write16(m, stereo + i * 4u, (uint16_t)clamp16(ax->buffer[AX_MAIN_R][i]));      /* right */
    write16(m, stereo + i * 4u + 2u, (uint16_t)clamp16(ax->buffer[AX_MAIN_L][i])); /* left */
  }
}

/* SET_MAIN_LR and SET_OPPOSITE_LR: the game's own material, arriving
 * already mixed, as one buffer of 160 32-bit samples that *replaces* main L
 * and R (the opposite form negates the left) and zeroes main S. A game that
 * streams its music can decode it on the processor and hand it over here.
 * Shrek 2 issues this every frame with a buffer of zeros - which a 16-bit
 * reading of it would also have got right, and nothing else would. */
static void command_set_main_lr(struct GcMachine *m, GcAx *ax, uint32_t address, int opposite) {
  unsigned i;
  for (i = 0; i < AX_SAMPLES; i++) {
    const int32_t sample = read32(m, address + i * 4u);
    ax->buffer[AX_MAIN_L][i] = opposite ? (int32_t)(0u - (uint32_t)sample) : sample;
    ax->buffer[AX_MAIN_R][i] = sample;
    ax->buffer[AX_MAIN_S][i] = 0;
  }
}

/* UPLOAD_LRS: the main mix back out to memory, 32 bits a sample. */
static void command_upload_lrs(struct GcMachine *m, GcAx *ax, uint32_t address) {
  unsigned b, i;
  for (b = 0; b < 3u; b++) {
    for (i = 0; i < AX_SAMPLES; i++) write32(m, address + (b * AX_SAMPLES + i) * 4u, ax->buffer[AX_MAIN_L + b][i]);
  }
}

/* How many argument halfwords each command takes. A command list has no
 * length fields, so this table is the only thing keeping the walk in step -
 * and an opcode this does not know has an unknown length, which is why the
 * walk stops rather than guessing. */
static int argument_count(uint16_t command, unsigned *count) {
  static const signed char k_args[] = {
      2,  /* 0x00 SETUP */
      5,  /* 0x01 DOWNLOAD_AND_MIX */
      2,  /* 0x02 SET_VOICE_LIST */
      0,  /* 0x03 PROCESS */
      4,  /* 0x04 MIX_AUXA */
      4,  /* 0x05 MIX_AUXB */
      2,  /* 0x06 UPLOAD_LRS */
      2,  /* 0x07 SET_MAIN_LR */
      10, /* 0x08 unimplemented; the count is the only thing known about it */
      2,  /* 0x09 MIX_AUXB_NOWRITE */
      0,  /* 0x0a */
      0,  /* 0x0b */
      0,  /* 0x0c */
      3,  /* 0x0d CHAIN */
      4,  /* 0x0e OUTPUT */
      0,  /* 0x0f END */
      4,  /* 0x10 MIX_AUXB_LR */
      2,  /* 0x11 SET_OPPOSITE_LR */
      4,  /* 0x12 COMPRESSOR */
      12, /* 0x13 SEND_AUX_AND_MIX */
  };
  if (command >= sizeof(k_args) / sizeof(k_args[0])) return 0;
  *count = (unsigned)k_args[command];
  return 1;
}

#define ARG32(i) (((uint32_t)a[i] << 16) | a[(i) + 1])

int gc_ax_run_list(struct GcMachine *m, uint32_t address, uint32_t halfwords) {
  GcAx *ax = ax_of(m);
  uint32_t at = 0;
  unsigned lists_followed = 0;
  if (!ax) return -1;
  ax->lists++;

  while (at < halfwords) {
    const uint16_t command = read16(m, address + at * 2u);
    unsigned args = 0;
    uint16_t a[12];
    unsigned i;
    if (!argument_count(command, &args) || at + 1u + args > halfwords) break;
    at++;
    for (i = 0; i < args; i++) a[i] = read16(m, address + (at + i) * 2u);
    at += args;

    switch (command) {
      case 0x00: command_setup(m, ax, ARG32(0)); break;
      case 0x01: {
        const uint16_t volumes[3] = {a[2], a[3], a[4]};
        command_download_and_mix(m, ax, ARG32(0), volumes);
        break;
      }
      case 0x02: ax->voice_list = ARG32(0); break;
      case 0x03: process_voices(m, ax); break;
      case 0x04: command_mix_aux(m, ax, 0u, ARG32(0), ARG32(2)); break;
      case 0x05: command_mix_aux(m, ax, 1u, ARG32(0), ARG32(2)); break;
      case 0x06: command_upload_lrs(m, ax, ARG32(0)); break;
      case 0x07: command_set_main_lr(m, ax, ARG32(0), 0); break;
      case 0x09: command_mix_aux(m, ax, 1u, 0u, ARG32(0)); break;
      case 0x0e: command_output(m, ax, ARG32(0), ARG32(2)); break;
      case 0x0f: return 0; /* END, and the only way out that is not a failure */
      case 0x10: command_mix_auxb_lr(m, ax, ARG32(0), ARG32(2)); break;
      case 0x11: command_set_main_lr(m, ax, ARG32(0), 1); break;
      case 0x12: command_compressor(m, ax, a[0], a[1], ARG32(2)); break;
      case 0x13: {
        const uint32_t addr[6] = {ARG32(0), ARG32(2), ARG32(4), ARG32(6), ARG32(8), ARG32(10)};
        command_send_aux_and_mix(m, ax, addr);
        break;
      }
      case 0x0d:
        /* CHAIN replaces the list rather than continuing it. A walker that
         * treated it as "skip three halfwords" would read the rest of the
         * old list as commands. */
        if (lists_followed++ >= 8u) break;
        address = ARG32(0);
        halfwords = a[2];
        at = 0;
        continue;
      default:
        /* Known length, no effect: 0x08, whose meaning is unknown, and the
         * three no-ops. */
        break;
    }
  }
  ax->bad_lists++;
  return -1;
}

/* ------------------------------------------------------------ save states */

#include "gc_state.h"

void gc_ax_state_save(struct GcMachine *m, GcStateBuffer *b) {
  /* The other microcode's state is written only once a game has loaded
   * that microcode. A game that never does - every game there was before -
   * saves exactly the bytes it always saved, so its states still load in
   * an older build and its state hash is the number it was. */
  const GcAx *ax = (const GcAx *)m->ax;
  const uint32_t bytes = (ax && ax->zelda.hash) ? (uint32_t)sizeof(GcAx) : (uint32_t)AX_STATE_SIZE_V2;
  gc_state_put_u32(b, ax ? 1u : 0u);
  gc_state_put_u32(b, bytes);
  if (ax) gc_state_put(b, ax, bytes);
}

/* A state from before the compressor carries the struct without its last
 * fields; it loads with the compressor idle, which is what it was. One from
 * before the other microcode loads with that microcode not running, which
 * is also what it was: no game that uses it could get that far. */
int gc_ax_state_load(struct GcMachine *m, const uint8_t *payload, size_t size, int apply) {
  uint32_t present, bytes;
  if (size < 8u) return -1;
  memcpy(&present, payload, 4u);
  memcpy(&bytes, payload + 4u, 4u);
  if (present > 1u || (bytes != sizeof(GcAx) && bytes != AX_STATE_SIZE_V1 && bytes != AX_STATE_SIZE_V2) ||
      size != 8u + (present ? bytes : 0u)) {
    return -1;
  }
  if (!apply) return 0;
  if (!present) {
    gc_ax_reset(m);
    return 0;
  }
  if (!m->ax) m->ax = calloc(1, sizeof(GcAx));
  if (!m->ax) return -1;
  memset(m->ax, 0, sizeof(GcAx));
  memcpy(m->ax, payload + 8u, bytes);
  return 0;
}
