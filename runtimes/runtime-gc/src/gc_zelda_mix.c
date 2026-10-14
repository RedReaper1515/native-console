/*
 * The "Zelda" sound microcode's mixer (gc_zelda.h): what a frame sounds
 * like.
 *
 * Written from docs/gc-dsp-zelda.md, parts "A voice" to "The reverb", which
 * were compiled from Dolphin's DSP-HLE read as a description of behaviour;
 * nothing here is its code. There is no hardware oracle for it in this
 * repository. The arithmetic is kept in the widths the document gives -
 * sixteen-bit buffers that wrap when a sum overflows them, thirty-two-bit
 * volumes that step - because the microcode is fixed-point throughout and a
 * mixer that is "nearly" it is a different instrument. Where the document
 * says the reference is unsure, or where this departs from it, the place
 * says so.
 */
#include <stdio.h>
#include <string.h>

#include "gc.h"
#include "gc_zelda.h"

#define FRAME GC_ZELDA_FRAME

/* Traits that change the mixing (gc_zelda.c has the table). */
#define FLAG_MAKE_DOLBY_LOUDER GC_ZELDA_MAKE_DOLBY_LOUDER
#define FLAG_FOUR_MIXING_DESTS GC_ZELDA_FOUR_MIXING_DESTS
#define FLAG_VOLUME_EXPLICIT_STEP GC_ZELDA_VOLUME_EXPLICIT_STEP

/* The buffers a voice or the reverb can name. A game names them by where
 * they are in the real microcode's data memory, which is why the numbers
 * in buffer_for() look like addresses: they are. */
enum {
  FRONT_LEFT,
  FRONT_RIGHT,
  BACK_LEFT,
  BACK_RIGHT,
  FRONT_LEFT_REVERB,
  FRONT_RIGHT_REVERB,
  BACK_LEFT_REVERB,
  BACK_RIGHT_REVERB,
  REVERB_0,
  REVERB_1,
  OTHER_0,
  OTHER_1,
  OTHER_2
};

static int16_t *buffer_for(GcZelda *z, unsigned id) {
  switch (id) {
    case 0x0d00: return z->buffer[FRONT_LEFT];
    case 0x0d60: return z->buffer[FRONT_RIGHT];
    case 0x0f40: return z->buffer[BACK_LEFT];
    case 0x0ca0: return z->buffer[BACK_RIGHT];
    case 0x0e80: return z->buffer[FRONT_LEFT_REVERB];
    case 0x0ee0: return z->buffer[FRONT_RIGHT_REVERB];
    case 0x0c00: return z->buffer[BACK_LEFT_REVERB];
    case 0x0c50: return z->buffer[BACK_RIGHT_REVERB];
    case 0x0dc0: return z->buffer[REVERB_0];
    case 0x0e20: return z->buffer[REVERB_1];
    case 0x09a0: return z->buffer[OTHER_0];
    case 0x0fa0: return z->buffer[OTHER_1];
    case 0x0b00: return z->buffer[OTHER_2];
    default: return NULL;
  }
}

/* A voice's parameter block, as an index into its sixteen-bit words. The
 * first 0x80 words are the voice's own and are written back; the rest the
 * game sets and the microcode only reads. */
enum {
  V_ENABLED = 0x00,
  V_DONE = 0x01,
  V_RATIO = 0x02,        /* 4.12: raw samples to one output sample */
  V_RESET = 0x04,        /* start from the beginning */
  V_END_REACHED = 0x05,
  V_USE_CONSTANT = 0x06, /* play V_CONSTANT, nothing else */
  V_CHANNELS = 0x08,     /* six of {buffer, target volume, volume now, unused} */
  V_DOLBY_POSITION = 0x28,
  V_DOLBY_REVERB = 0x29,
  V_DOLBY_VOLUME = 0x2a,
  V_DOLBY_TARGET = 0x2b,
  V_USE_DOLBY = 0x2c,
  V_FRACTION = 0x30,     /* 0.12: how far between two raw samples */
  V_AFC_CACHED = 0x32,   /* decoded samples not yet given out */
  V_CONSTANT = 0x33,
  V_POSITION = 0x34,     /* two words */
  V_BEFORE_LOOP = 0x36,
  V_ADDRESS = 0x38,      /* two words: where in sample memory it has got to */
  V_REMAINING = 0x3a,    /* two words */
  V_RESAMPLE = 0x3c,     /* the four samples before the next frame's first */
  V_BIQUAD = 0x54,       /* x1, x2, y1, y2 */
  V_AFC_SAMPLES = 0x58,  /* sixteen: the last block decoded */
  V_LOW_PASS_Y = 0x68,
  V_LOW_PASS_X = 0x69,
  V_SOURCE = 0x80,
  V_LOOPING = 0x81,
  V_LOOP_Y1 = 0x82,
  V_LOOP_Y2 = 0x83,
  V_FILTERS = 0x84,      /* bit 5: the biquad is on */
  V_END_REQUESTED = 0x85,
  V_LOOP_ADDRESS = 0x88, /* two words */
  V_LOOP_START = 0x8a,   /* two words */
  V_BASE = 0x8c,         /* two words */
  V_BIQUAD_COEFFICIENTS = 0xa4, /* b1, b2, a1, a2 */
  V_LOW_PASS = 0xa8,
  V_WORDS = 0xc0,
  V_WRITTEN_BACK = 0x80
};

static int16_t s16(uint16_t v) { return (int16_t)v; }
static uint32_t pair(const uint16_t *v, unsigned at) { return ((uint32_t)v[at] << 16) | v[at + 1u]; }
static void set_pair(uint16_t *v, unsigned at, uint32_t value) {
  v[at] = (uint16_t)(value >> 16);
  v[at + 1u] = (uint16_t)value;
}

static int32_t clamp16(int64_t v) { return v < -0x8000 ? -0x8000 : (v > 0x7fff ? 0x7fff : (int32_t)v); }

/* ------------------------------------------------------------------ */
/* Memory                                                              */

/* Sixteen-bit words in main memory, which keeps them big-endian. */
static int load_words(GcMachine *m, uint32_t address, void *out, unsigned count) {
  const uint8_t *p = gc_ram_at_register(m, address, count * 2u);
  uint16_t *words = (uint16_t *)out;
  unsigned i;
  if (!p) {
    memset(out, 0, count * 2u);
    return 0;
  }
  for (i = 0; i < count; i++) words[i] = (uint16_t)((p[i * 2u] << 8) | p[i * 2u + 1u]);
  return 1;
}

static void store_words(GcMachine *m, uint32_t address, const void *in, unsigned count) {
  uint8_t *p = gc_ram_at_register(m, address, count * 2u);
  const uint16_t *words = (const uint16_t *)in;
  unsigned i;
  if (!p) return;
  for (i = 0; i < count; i++) {
    p[i * 2u] = (uint8_t)(words[i] >> 8);
    p[i * 2u + 1u] = (uint8_t)words[i];
  }
}

/* Where a voice's samples are. On a GameCube that is audio memory, by
 * offset. A Wii has none: the game keeps the same data in main memory and
 * says where (command 0x0e), and the offsets are from there. */
static const uint8_t *sample_memory(GcMachine *m, const GcZelda *z, uint32_t offset, uint32_t size) {
  if (z->flags & GC_ZELDA_NO_ARAM) return gc_ram_at_register(m, z->sample_base + offset, size);
  if (!m->aram || offset >= GC_ARAM_SIZE || size > GC_ARAM_SIZE - offset) return NULL;
  return m->aram + offset;
}

/* ------------------------------------------------------------------ */
/* Adding one buffer to another                                        */

/* A buffer scaled where it stands, the volume having `bits` whole bits. */
static void scale(int16_t *buffer, unsigned count, uint16_t volume, unsigned bits) {
  unsigned i;
  for (i = 0; i < count; i++) {
    const int32_t product = (int32_t)((uint32_t)(int32_t)buffer[i] * (uint32_t)volume);
    buffer[i] = (int16_t)clamp16(product >> (16u - bits));
  }
}

/* `source` at a fixed 1.15 volume, each contribution clamped and the sum
 * left to wrap. */
static void add_scaled(int16_t *destination, const int16_t *source, unsigned count, uint16_t volume) {
  unsigned i;
  for (i = 0; i < count; i++) {
    const int32_t part = clamp16(((int32_t)source[i] * (int32_t)volume) >> 15);
    destination[i] = (int16_t)(destination[i] + part);
  }
}

/* `source` at a volume that moves by `step` a sample. The volume is 16.16
 * and only its whole part multiplies. Returns where the volume ended. */
static int32_t add_ramped(int16_t *destination, const int16_t *source, int32_t volume, int32_t step) {
  unsigned i;
  if (!volume && !step) return volume;
  for (i = 0; i < FRAME; i++) {
    destination[i] = (int16_t)(destination[i] + (((volume >> 16) * (int32_t)source[i]) >> 16));
    volume = (int32_t)((uint32_t)volume + (uint32_t)step);
  }
  return volume;
}

static int32_t as_volume(int16_t whole) { return (int32_t)((uint32_t)(int32_t)whole << 16); }

/* ------------------------------------------------------------------ */
/* The tables                                                          */

void gc_zelda_mix_setup(GcMachine *m, GcZelda *z) {
  (void)load_words(m, z->tables, z->resampling, 0x100);
  (void)load_words(m, z->tables + 0x200u, z->patterns, 0x100);
  (void)load_words(m, z->tables + 0x400u, z->sine, 0x80);
  (void)load_words(m, z->afc_coefficients, z->afc, 0x20);
}

/* ------------------------------------------------------------------ */
/* The reverb                                                          */

/* Four delay lines, each a ring of frames in the game's memory described by
 * a block of sixteen words: whether it is on (and when its filter runs),
 * how many frames long the ring is, where it is, two destinations with a
 * volume each, and eight filter coefficients.
 *
 * Before the voices: the frame the ring is at is read back, filtered across
 * the eight samples that came before it, and added to its destinations.
 * After them: what the voices sent to the line this frame is written into
 * the ring in its place, and the ring moves on. So what comes out is what
 * went in a whole ring ago. */
static void reverb(GcMachine *m, GcZelda *z, int after) {
  static const unsigned LINE[4] = {REVERB_0, REVERB_1, FRONT_LEFT_REVERB, FRONT_RIGHT_REVERB};
  unsigned line;
  if (!z->reverb_blocks) return;

  for (line = 0; line < 4u; line++) {
    uint16_t block[16];
    uint32_t at;
    if (!load_words(m, z->reverb_blocks + line * 32u, block, 16) || !block[0]) continue;
    at = pair(block, 2) + (uint32_t)z->reverb_frame[line] * FRAME * 2u;

    if (after) {
      z->reverb_frames++;
      store_words(m, at, z->buffer[LINE[line]], FRAME);
      z->reverb_frame[line] = block[1] ? (uint16_t)((z->reverb_frame[line] + 1u) % block[1]) : 0u;
    } else {
      int16_t samples[8 + FRAME];
      unsigned pass, i, j, d;
      memcpy(samples, z->reverb_last8[line], sizeof(z->reverb_last8[line]));
      (void)load_words(m, at, samples + 8, FRAME);
      memcpy(z->reverb_last8[line], samples + FRAME, sizeof(z->reverb_last8[line]));

      /* Bit 0 filters before the destinations are fed, bit 1 after. */
      for (pass = 0; pass < 2u; pass++) {
        if (block[0] & (1u << pass)) {
          for (i = 0; i < FRAME; i++) {
            int32_t sum = 0;
            for (j = 0; j < 8u; j++) sum += (int32_t)samples[i + j] * (int32_t)s16(block[8u + j]);
            samples[i] = (int16_t)clamp16(sum >> 15);
          }
        }
        if (pass == 0u) {
          for (d = 0; d < 2u; d++) {
            int16_t *destination = block[4u + d * 2u] ? buffer_for(z, block[4u + d * 2u]) : NULL;
            if (destination) add_scaled(destination, samples, FRAME, block[5u + d * 2u]);
          }
        }
      }
      memcpy(z->buffer[LINE[line]], samples, FRAME * sizeof(int16_t));
    }
  }
}

/* ------------------------------------------------------------------ */
/* The start of a frame                                                */

void gc_zelda_mix_prepare(GcMachine *m, GcZelda *z) {
  if (z->prepared) return;

  memset(z->buffer[FRONT_LEFT], 0, sizeof(z->buffer[FRONT_LEFT]));
  memset(z->buffer[FRONT_RIGHT], 0, sizeof(z->buffer[FRONT_RIGHT]));
  /* The back pair decays rather than being cleared. The real microcode
   * then filters it and folds it into the front pair; that fold is not
   * here (the reference does not have it either), so what a voice sends
   * to the back is not heard. */
  scale(z->buffer[BACK_LEFT], FRAME, 0x6784u, 1);
  scale(z->buffer[BACK_RIGHT], FRAME, 0x6784u, 1);

  /* Last frame's reverb, and the back reverb pair spread onto the front
   * pair: the left whole and a little over onto the right, and the second
   * half of the right the other way. */
  reverb(m, z, 0);
  add_scaled(z->buffer[FRONT_LEFT_REVERB], z->buffer[BACK_LEFT_REVERB], FRAME, 0x7fffu);
  add_scaled(z->buffer[FRONT_RIGHT_REVERB], z->buffer[BACK_LEFT_REVERB], FRAME, 0xb820u);
  add_scaled(z->buffer[FRONT_LEFT_REVERB], z->buffer[BACK_RIGHT_REVERB] + 0x28, 0x28, 0xb820u);
  add_scaled(z->buffer[FRONT_RIGHT_REVERB], z->buffer[BACK_RIGHT_REVERB] + 0x28, 0x28, 0x7fffu);
  memset(z->buffer[BACK_LEFT_REVERB], 0, sizeof(z->buffer[BACK_LEFT_REVERB]));
  memset(z->buffer[BACK_RIGHT_REVERB], 0, sizeof(z->buffer[BACK_RIGHT_REVERB]));

  /* Two of the four wave patterns are not waves but generators: each frame
   * makes the next sixty-four of a noise-like sequence from the last two. */
  {
    int16_t *p = z->patterns + 0x80;
    int32_t y2 = p[0x3e], y1 = p[0x3f];
    uint32_t v;
    unsigned i;
    for (i = 0; i < 0x40u; i += 2u) {
      v = (uint32_t)(y2 * y1) - ((uint32_t)(int32_t)p[i] << 16);
      y2 = y1;
      y1 = p[i];
      p[i] = (int16_t)((int32_t)v >> 16);

      v = 2u * ((uint32_t)(y2 * y1) + ((uint32_t)(int32_t)p[i + 1u] << 16));
      y2 = y1;
      y1 = p[i + 1u];
      p[i + 1u] = (int16_t)((int32_t)v >> 16);
    }
  }
  {
    int16_t *p = z->patterns + 0xc0;
    const int32_t y2 = p[0x3e], y1 = p[0x3f];
    const int16_t start = (int16_t)y1;
    int16_t step = (int16_t)(p[0] + (int16_t)((int32_t)((uint32_t)(y1 * y2) + ((uint32_t)y2 << 16) + (uint32_t)y1) >> 16));
    unsigned i;
    step = (int16_t)((step & 0x1ff) | 0x2000);
    for (i = 0; i < 0x40u; i++) p[i] = (int16_t)(start + (int32_t)(i + 1u) * step);
  }

  z->prepared = 1;
}

/* ------------------------------------------------------------------ */
/* A voice's samples                                                   */

/* The most raw samples a frame can ask for: eighty at the highest ratio,
 * and the sixteen a block decoder rounds up to; four before them carried
 * from the last frame. */
#define RAW_MAX (4u + 0x500u + 0x10u + 0x10u)

static void zero(int16_t *out, unsigned count) {
  if (count) memset(out, 0, count * sizeof(int16_t));
}

/* Plain samples in sample memory, eight or sixteen bits each. A voice has a
 * length (to the loop's end), plays that, then either stops or goes back to
 * its loop's start and plays to the end again. */
static void fetch_pcm(GcMachine *m, GcZelda *z, uint16_t *v, int16_t *out, unsigned wanted, unsigned bytes) {
  unsigned laps = 0;
  if (v[V_DONE]) {
    zero(out, wanted);
    return;
  }
  if (v[V_RESET]) {
    set_pair(v, V_REMAINING, pair(v, V_LOOP_START) - pair(v, V_POSITION));
    set_pair(v, V_ADDRESS, pair(v, V_BASE) + pair(v, V_POSITION) * bytes);
  }
  v[V_END_REACHED] = 0;
  while (wanted) {
    unsigned count, i;
    const uint8_t *source;
    if (v[V_END_REACHED]) {
      v[V_END_REACHED] = 0;
      /* A loop of no length would go round for ever; the real microcode
       * presumably does. Here it ends the voice. */
      if (!v[V_LOOPING] || ++laps > 64u) {
        zero(out, wanted);
        v[V_DONE] = 1;
        break;
      }
      set_pair(v, V_POSITION, pair(v, V_LOOP_ADDRESS));
      set_pair(v, V_REMAINING, pair(v, V_LOOP_START) - pair(v, V_POSITION));
      set_pair(v, V_ADDRESS, pair(v, V_BASE) + pair(v, V_POSITION) * bytes);
    }
    count = pair(v, V_REMAINING) < wanted ? (unsigned)pair(v, V_REMAINING) : wanted;
    source = sample_memory(m, z, pair(v, V_ADDRESS), count * bytes);
    for (i = 0; i < count; i++) {
      if (!source) out[i] = 0;
      else if (bytes == 1u) out[i] = (int16_t)((int8_t)source[i] * 256);
      else out[i] = (int16_t)((source[i * 2u] << 8) | source[i * 2u + 1u]);
    }
    out += count;
    set_pair(v, V_REMAINING, pair(v, V_REMAINING) - count);
    set_pair(v, V_ADDRESS, pair(v, V_ADDRESS) + count * bytes);
    wanted -= count;
    if (!pair(v, V_REMAINING)) v[V_END_REACHED] = 1;
  }
}

/* Blocks of the block-coded format: sixteen samples in nine bytes (four
 * bits each) or five (two bits each). A header byte gives a shift and one of
 * sixteen coefficient pairs; each sample is the coded step, shifted, plus
 * the pair applied to the two samples before it. The two before the first
 * are the last two of the block before, which the voice keeps. */
static void decode_afc(GcMachine *m, GcZelda *z, uint16_t *v, int16_t *out, unsigned blocks) {
  const unsigned block_bytes = v[V_SOURCE]; /* 9 or 5: the source's number is the size */
  const uint32_t address = pair(v, V_ADDRESS);
  const uint8_t *source = sample_memory(m, z, address, blocks * block_bytes);
  int32_t y1 = s16(v[V_AFC_SAMPLES + 15]), y2 = s16(v[V_AFC_SAMPLES + 14]);
  unsigned b, i;
  set_pair(v, V_ADDRESS, address + blocks * block_bytes);
  if (!source) {
    zero(out, blocks * 16u);
    return;
  }
  for (b = 0; b < blocks; b++) {
    int16_t steps[16];
    const int16_t delta = (int16_t)(1u << (source[0] >> 4));
    const unsigned pick = source[0] & 0xfu;
    source++;
    if (block_bytes == 9u) {
      for (i = 0; i < 16u; i += 2u) {
        steps[i] = (int16_t)((int16_t)((source[0] >> 4) << 12) >> 1);
        steps[i + 1u] = (int16_t)((int16_t)((source[0] & 0xfu) << 12) >> 1);
        source++;
      }
    } else {
      for (i = 0; i < 16u; i += 4u) {
        unsigned k;
        for (k = 0; k < 4u; k++) steps[i + k] = (int16_t)((int16_t)(((source[0] >> (6u - 2u * k)) & 3u) << 14) >> 1);
        source++;
      }
    }
    for (i = 0; i < 16u; i++) {
      const int32_t sample =
          clamp16(((int32_t)delta * steps[i] + y1 * z->afc[pick * 2u] + y2 * z->afc[pick * 2u + 1u]) >> 11);
      *out++ = (int16_t)sample;
      y2 = y1;
      y1 = sample;
    }
  }
  v[V_AFC_SAMPLES + 14] = (uint16_t)y2;
  v[V_AFC_SAMPLES + 15] = (uint16_t)y1;
}

/* A block-coded voice: what is left of the last block decoded first, then
 * whole blocks, keeping the last for next time; at the end of the samples,
 * silence and done, or the loop - whose start need not be at the start of a
 * block, so its block is decoded from the two samples the game recorded
 * for that point and the part of it before the loop is skipped. */
static void fetch_afc(GcMachine *m, GcZelda *z, uint16_t *v, int16_t *out, unsigned wanted) {
  unsigned laps = 0;
  if (v[V_RESET]) {
    v[V_AFC_SAMPLES + 14] = 0;
    v[V_AFC_SAMPLES + 15] = 0;
    v[V_AFC_CACHED] = 0;
    set_pair(v, V_REMAINING, pair(v, V_LOOP_START));
    set_pair(v, V_ADDRESS, pair(v, V_BASE));
  }
  if (v[V_DONE]) {
    zero(out, wanted);
    return;
  }
  if (v[V_AFC_CACHED] > 0x10u) v[V_AFC_CACHED] = 0x10u;

  for (;;) {
    const unsigned cached = v[V_AFC_CACHED] < wanted ? v[V_AFC_CACHED] : wanted;
    unsigned i;
    for (i = 0; i < cached; i++) out[i] = s16(v[V_AFC_SAMPLES + 0x10u - v[V_AFC_CACHED] + i]);
    out += cached;
    v[V_AFC_CACHED] = (uint16_t)(v[V_AFC_CACHED] - cached);
    wanted -= cached;
    if (!wanted) return;

    if (wanted <= pair(v, V_REMAINING)) {
      const unsigned blocks = (wanted + 0xfu) >> 4;
      const unsigned decoded = blocks << 4;
      if (decoded < pair(v, V_REMAINING)) {
        v[V_AFC_CACHED] = (uint16_t)(decoded - wanted);
        set_pair(v, V_REMAINING, pair(v, V_REMAINING) - decoded);
      } else {
        v[V_AFC_CACHED] = (uint16_t)(pair(v, V_REMAINING) - wanted);
        set_pair(v, V_REMAINING, 0);
      }
      decode_afc(m, z, v, out, blocks);
      if (v[V_AFC_CACHED]) {
        for (i = 0; i < 0x10u; i++) v[V_AFC_SAMPLES + i] = (uint16_t)out[decoded - 0x10u + i];
        if (!pair(v, V_REMAINING) && pair(v, V_LOOP_START)) {
          /* The samples kept are the end of the voice, which need not be
           * the end of a block: they are moved up so that the last kept is
           * the voice's last. */
          const int last = (int)((pair(v, V_LOOP_START) + 0xfu) & 0xfu);
          for (i = 0; i < v[V_AFC_CACHED] && last - (int)i >= 0; i++) {
            v[V_AFC_SAMPLES + 0xfu - i] = v[V_AFC_SAMPLES + (unsigned)(last - (int)i)];
          }
        }
      }
      return;
    }

    /* More wanted than there is. What there is, then the end or the loop. */
    if (pair(v, V_REMAINING)) {
      const unsigned left = (unsigned)pair(v, V_REMAINING);
      wanted -= left;
      decode_afc(m, z, v, out, (left + 0xfu) >> 4);
      out += left;
    }
    if (!v[V_LOOPING] || ++laps > 64u) {
      v[V_DONE] = 1;
      zero(out, wanted);
      return;
    }
    {
      const uint32_t loop = pair(v, V_LOOP_ADDRESS);
      int16_t block[16];
      set_pair(v, V_ADDRESS, pair(v, V_BASE) + (loop >> 4) * v[V_SOURCE]);
      v[V_AFC_SAMPLES + 14] = v[V_LOOP_Y2];
      v[V_AFC_SAMPLES + 15] = v[V_LOOP_Y1];
      decode_afc(m, z, v, block, 1);
      for (i = 0; i < 0x10u; i++) v[V_AFC_SAMPLES + i] = (uint16_t)block[i];
      v[V_AFC_CACHED] = (uint16_t)(0x10u - (loop & 0xfu));
      set_pair(v, V_REMAINING, pair(v, V_LOOP_START) - v[V_AFC_CACHED] - loop);
    }
  }
}

/* Sixteen-bit samples in main memory, by a position in samples. At the end
 * the last sample is held and the voice is done; a loop is another address
 * the base moves to. */
static void fetch_memory(GcMachine *m, uint16_t *v, int16_t *out, unsigned wanted) {
  const uint32_t address = pair(v, V_BASE) + (uint32_t)v[V_POSITION] * 2u;
  const uint32_t remaining = pair(v, V_REMAINING);
  if (wanted > remaining) {
    int16_t last = 0;
    unsigned i;
    if (remaining) {
      (void)load_words(m, address, out, (unsigned)remaining);
      last = out[remaining - 1u];
    }
    /* The reference writes the held sample over the start of what it has
     * just read and leaves the end as it found it; what it means to do is
     * plain, and is done here. */
    for (i = (unsigned)remaining; i < wanted; i++) out[i] = last;
    v[V_POSITION] = (uint16_t)(v[V_POSITION] + remaining);
    set_pair(v, V_REMAINING, 0);
    v[V_DONE] = 1;
    return;
  }
  set_pair(v, V_REMAINING, remaining - wanted);
  v[V_BEFORE_LOOP] = (uint16_t)(v[V_LOOP_START] - v[V_POSITION]);
  if (wanted <= v[V_BEFORE_LOOP]) {
    (void)load_words(m, address, out, wanted);
    v[V_POSITION] = (uint16_t)(v[V_POSITION] + wanted);
  } else {
    const unsigned before = v[V_BEFORE_LOOP];
    (void)load_words(m, address, out, before);
    set_pair(v, V_BASE, pair(v, V_LOOP_ADDRESS));
    v[V_POSITION] = (uint16_t)(wanted - before);
    (void)load_words(m, pair(v, V_LOOP_ADDRESS), out + before, v[V_POSITION]);
  }
}

/* Raw samples to the frame's eighty. The ratio is raw samples for each
 * output sample, 4.12. Up to four to one each output sample is four raw
 * ones weighted by the coefficients for how far between two of them the
 * position is, in sixty-four steps; beyond that the nearest is taken. */
static void resample(const GcZelda *z, uint16_t *v, const int16_t *raw, int16_t *out) {
  const uint32_t ratio = v[V_RATIO];
  uint32_t position = v[V_FRACTION];
  unsigned i, k;
  if ((ratio >> 12) >= 4u) {
    for (i = 0; i < FRAME; i++) {
      position += ratio;
      out[i] = raw[position >> 12];
    }
  } else {
    for (i = 0; i < FRAME; i++) {
      const int16_t *weights = &z->resampling[((position & 0xfffu) >> 6) * 4u];
      const int16_t *in = &raw[position >> 12];
      int64_t sum = 0;
      for (k = 0; k < 4u; k++) sum += (int64_t)2 * weights[k] * in[k];
      out[i] = (int16_t)clamp16(sum >> 16);
      position += ratio;
    }
  }
  for (k = 0; k < 4u; k++) v[V_RESAMPLE + k] = (uint16_t)raw[(position >> 12) + k];
  v[V_CONSTANT] = (uint16_t)out[FRAME - 1u];
  v[V_FRACTION] = (uint16_t)(position & 0xfffu);
}

/* A frame of the voice's own sound, before its volumes. */
static void voice_samples(GcMachine *m, GcZelda *z, uint16_t *v, int16_t *out) {
  int16_t raw[RAW_MAX];
  const unsigned source = v[V_SOURCE];
  /* How many raw samples eighty output samples take from here. */
  const unsigned wanted = (unsigned)((v[V_FRACTION] + FRAME * (uint32_t)v[V_RATIO]) >> 12);
  unsigned i;

  if (v[V_USE_CONSTANT]) {
    for (i = 0; i < FRAME; i++) out[i] = s16(v[V_CONSTANT]);
    return;
  }
  for (i = 0; i < 4u; i++) raw[i] = s16(v[V_RESAMPLE + i]);
  zero(raw + 4, RAW_MAX - 4u);

  switch (source) {
    case 0:   /* a square wave */
    case 3: { /* a quarter-duty one */
      const unsigned shift = source == 0u ? 1u : 2u;
      const uint32_t mask = (1u << shift) - 1u, step = (uint32_t)v[V_RATIO] << (shift - 1u);
      uint32_t position = (uint32_t)v[V_FRACTION] << shift;
      for (i = 0; i < FRAME; i++) {
        out[i] = ((position >> 16) & mask) ? (int16_t)-0x4000 : (int16_t)0x4000;
        position += step;
      }
      v[V_FRACTION] = (uint16_t)(position >> shift);
      return;
    }
    case 1: { /* a sawtooth */
      uint32_t position = v[V_FRACTION];
      for (i = 0; i < FRAME; i++) {
        out[i] = (int16_t)position;
        position += (uint32_t)v[V_RATIO] >> 1;
      }
      v[V_FRACTION] = (uint16_t)position;
      return;
    }
    case 7:    /* the first pattern */
    case 10:   /* the same, its pitch bent by the back right buffer */
    case 4:    /* the second */
    case 11:   /* the third, a generator */
    case 12: { /* the fourth, another */
      const unsigned which = source == 4u ? 1u : source == 11u ? 2u : source == 12u ? 3u : 0u;
      const int16_t *pattern = z->patterns + which * 0x40u;
      const uint32_t step = (uint32_t)v[V_RATIO] << 5;
      uint32_t position = (uint32_t)v[V_FRACTION] << 6;
      for (i = 0; i < FRAME; i++) {
        out[i] = pattern[(position >> 16) & 0x3fu];
        position = (position + step) % (0x40u << 16);
        if (source == 10u) {
          position = ((position << 10) + (uint32_t)((int32_t)z->buffer[BACK_RIGHT][i] * (int32_t)v[V_RATIO])) >> 10;
        }
      }
      v[V_FRACTION] = (uint16_t)(position >> 6);
      return;
    }
    case 8: fetch_pcm(m, z, v, raw + 4, wanted, 1); break;
    case 16: fetch_pcm(m, z, v, raw + 4, wanted, 2); break;
    case 5:
    case 9: fetch_afc(m, z, v, raw + 4, wanted); break;
    case 33: fetch_memory(m, v, raw + 4, wanted); break;
    default:
      /* 2 and 6 exist and nobody has described them. */
      z->unknown_sources++;
      if (m->log && z->unknown_sources_said < 4u) {
        z->unknown_sources_said++;
        fprintf(m->log, "gc: a voice with sample source %u, which is not written: it is silent\n", source);
      }
      zero(out, FRAME);
      return;
  }
  resample(z, v, raw, out);
}

/* ------------------------------------------------------------------ */
/* A voice                                                             */

void gc_zelda_mix_voice(GcMachine *m, GcZelda *z, unsigned voice) {
  uint16_t v[V_WORDS];
  int16_t samples[FRAME];
  const uint32_t address = z->voice_blocks + voice * (V_WORDS * 2u);
  const int loud = (z->flags & FLAG_MAKE_DOLBY_LOUDER) != 0;
  unsigned i;

  if (!load_words(m, address, v, V_WORDS) || !v[V_ENABLED] || v[V_DONE]) return;
  voice_samples(m, z, v, samples);
  z->voices_mixed++;
  z->sources_used |= (uint64_t)1 << (v[V_SOURCE] & 63u);
  if (v[V_USE_DOLBY]) z->voices_placed++;

  /* A one-pole low-pass, when the voice has a coefficient for it. */
  if (v[V_LOW_PASS]) {
    int32_t y1 = v[V_RESET] ? 0 : s16(v[V_LOW_PASS_Y]), x1 = v[V_RESET] ? 0 : s16(v[V_LOW_PASS_X]);
    for (i = 0; i < FRAME; i++) {
      const int32_t x0 = samples[i];
      const int32_t y0 = clamp16((((int64_t)(x0 - x1) * v[V_LOW_PASS]) >> 7) + y1);
      samples[i] = (int16_t)y0;
      y1 = y0;
      x1 = x0;
    }
    v[V_LOW_PASS_Y] = (uint16_t)y1;
    v[V_LOW_PASS_X] = (uint16_t)x1;
  }
  /* A two-pole filter over the two samples before and the two results
   * before, when it is on and is not the one that changes nothing. (The
   * variable filter in the same word's low five bits is not written.) */
  if (((v[V_FILTERS] >> 5) & 1u) && (v[V_BIQUAD_COEFFICIENTS + 3] || v[V_BIQUAD_COEFFICIENTS + 2] ||
                                     v[V_BIQUAD_COEFFICIENTS + 1] || v[V_BIQUAD_COEFFICIENTS] != 0x7fffu)) {
    int32_t x1 = s16(v[V_BIQUAD]), x2 = s16(v[V_BIQUAD + 1]), y1 = s16(v[V_BIQUAD + 2]), y2 = s16(v[V_BIQUAD + 3]);
    for (i = 0; i < FRAME; i++) {
      const int32_t x0 = samples[i];
      const int64_t sum = (int64_t)s16(v[V_BIQUAD_COEFFICIENTS]) * x1 + (int64_t)s16(v[V_BIQUAD_COEFFICIENTS + 1]) * x2 +
                          (int64_t)s16(v[V_BIQUAD_COEFFICIENTS + 2]) * y1 + (int64_t)s16(v[V_BIQUAD_COEFFICIENTS + 3]) * y2;
      const int32_t y0 = clamp16(sum >> 15);
      samples[i] = (int16_t)y0;
      x2 = x1;
      x1 = x0;
      y2 = y1;
      y1 = y0;
    }
    v[V_BIQUAD] = (uint16_t)x1;
    v[V_BIQUAD + 1] = (uint16_t)x2;
    v[V_BIQUAD + 2] = (uint16_t)y1;
    v[V_BIQUAD + 3] = (uint16_t)y2;
  }

  if (v[V_USE_DOLBY]) {
    /* Placed: one volume, and a position left to right and front to back,
     * each 0 to 127. A quarter sine turns each into two gains that keep
     * the power the same wherever the sound is, their products are the four
     * corners, and each corner has a reverb send beside it. The volume
     * ramps from what it was to its target across the frame. */
    static const unsigned CORNER[4] = {FRONT_LEFT, BACK_LEFT, FRONT_RIGHT, BACK_RIGHT};
    static const unsigned CORNER_REVERB[4] = {FRONT_LEFT_REVERB, BACK_LEFT_REVERB, FRONT_RIGHT_REVERB,
                                              BACK_RIGHT_REVERB};
    const unsigned shift = loud ? 15u : 16u;
    const unsigned x = (v[V_DOLBY_POSITION] >> 8) & 0x7fu, y = v[V_DOLBY_POSITION] & 0x7fu;
    const int32_t right = z->sine[x], back = z->sine[y], left = z->sine[x ^ 0x7fu], front = z->sine[y ^ 0x7fu];
    int16_t corner[4], corner_step[4], delta;
    if (v[V_END_REQUESTED]) {
      /* Asked to stop: the volume halves each frame, and at nothing the
       * voice is done. */
      v[V_DOLBY_TARGET] = (uint16_t)(int16_t)(s16(v[V_DOLBY_VOLUME]) / 2);
      if (!v[V_DOLBY_TARGET]) v[V_DONE] = 1;
    }
    corner[0] = (int16_t)((left * front) >> shift);
    corner[1] = (int16_t)((left * back) >> shift);
    corner[2] = (int16_t)((right * front) >> shift);
    corner[3] = (int16_t)((right * back) >> shift);
    delta = (int16_t)(v[V_DOLBY_TARGET] - v[V_DOLBY_VOLUME]);
    for (i = 0; i < 4u; i++) corner_step[i] = (int16_t)(((int32_t)(uint16_t)corner[i] * delta) >> shift);
    for (i = 0; i < 4u; i++) corner[i] = (int16_t)(((int32_t)corner[i] * s16(v[V_DOLBY_VOLUME])) >> shift);
    for (i = 0; i < 4u; i++) {
      (void)add_ramped(z->buffer[CORNER[i]], samples, as_volume(corner[i]), as_volume(corner_step[i]) / (int32_t)FRAME);
    }
    for (i = 0; i < 4u; i++) {
      const int16_t send = (int16_t)(((int32_t)corner[i] * s16(v[V_DOLBY_REVERB])) >> shift);
      const int16_t send_step = (int16_t)(((int32_t)corner_step[i] * s16(v[V_DOLBY_REVERB])) >> shift);
      (void)add_ramped(z->buffer[CORNER_REVERB[i]], samples, as_volume(send), as_volume(send_step) / (int32_t)FRAME);
    }
    v[V_DOLBY_VOLUME] = v[V_DOLBY_TARGET];
  } else {
    /* Sent: up to six destinations, each a buffer and a volume that ramps
     * to its target (or, on some members, by the step given in its place). */
    const unsigned channels = (z->flags & FLAG_FOUR_MIXING_DESTS) ? 4u : 6u;
    if (v[V_END_REQUESTED]) {
      int silent = 1;
      for (i = 0; i < channels; i++) {
        uint16_t *channel = &v[V_CHANNELS + i * 4u];
        channel[1] = (uint16_t)(int16_t)(s16(channel[2]) / 2);
        if (channel[1]) silent = 0;
      }
      if (silent) v[V_DONE] = 1;
    }
    for (i = 0; i < channels; i++) {
      uint16_t *channel = &v[V_CHANNELS + i * 4u];
      int16_t delta, *destination;
      int32_t step;
      if (!channel[0]) continue;
      delta = (z->flags & FLAG_VOLUME_EXPLICIT_STEP) ? s16(channel[1]) : (int16_t)(channel[1] - channel[2]);
      step = as_volume(delta) / (int32_t)FRAME;
      if (!channel[2] && !step) continue;
      destination = buffer_for(z, channel[0]);
      if (!destination) continue;
      channel[2] = (uint16_t)(add_ramped(destination, samples, as_volume(s16(channel[2])), step) >> 16);
    }
  }

  /* The voice has started, unless it is only holding a sample. */
  if (!v[V_USE_CONSTANT]) v[V_RESET] = 0;
  if (v[V_DONE]) z->voices_finished++;
  store_words(m, address, v, V_WRITTEN_BACK);
}

/* ------------------------------------------------------------------ */
/* The end of a frame                                                  */

void gc_zelda_mix_finish(GcMachine *m, GcZelda *z) {
  /* The front pair at the output volume (4.12) is the frame: eighty
   * samples for the left and eighty for the right, where the render
   * command said, which then moves on by a frame. */
  scale(z->buffer[FRONT_LEFT], FRAME, (uint16_t)z->output_volume, 4);
  scale(z->buffer[FRONT_RIGHT], FRAME, (uint16_t)z->output_volume, 4);
  store_words(m, z->output_left, z->buffer[FRONT_LEFT], FRAME);
  store_words(m, z->output_right, z->buffer[FRONT_RIGHT], FRAME);
  z->output_left += FRAME * 2u;
  z->output_right += FRAME * 2u;
  reverb(m, z, 1);
  z->prepared = 0;
  z->frames++;
}
