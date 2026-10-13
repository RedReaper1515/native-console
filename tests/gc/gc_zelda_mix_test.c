/*
 * The "Zelda" sound microcode's mixer: what a frame holds.
 *
 * gc_zelda_test plays the conversation and checks where a frame is written.
 * This one checks what is in it, by handing the mixer voices small enough
 * to work out on paper: every number expected here was, from
 * docs/gc-dsp-zelda.md ("A voice" onwards), and none was copied from what
 * the code printed.
 *
 *  - a held sample sent to two destinations, one at a steady volume and one
 *    ramping, and the output volume;
 *  - eight-bit samples played to their end, and the voice reporting it;
 *  - sixteen-bit samples going round a loop;
 *  - block-coded samples at full and at half speed (where a decoded block
 *    outlasts the frame that asked for it), and their end;
 *  - a placed voice: the corner it lands in, its reverb send, and fading out
 *    when asked to stop;
 *  - a reverb line: what goes in comes out a ring later, filtered, and is
 *    fed back.
 *
 * What stands behind the document's arithmetic is a second implementation
 * and a retail game: Super Mario Galaxy 2's title music, mixed here and by
 * Dolphin, is the same to the bit (docs/gc-dsp-zelda.md, "Evidence";
 * tools/wii/compare_audio.py). That needs the game. This needs nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_zelda.h"

static unsigned failures, checks;

#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(cond)) {                                                                                 \
      failures++;                                                                                  \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
    }                                                                                              \
  } while (0)

/* Where things are put in the machine's memory. */
#define VOICES 0x00400000u
#define TABLES 0x00410000u
#define AFC 0x00420000u
#define REVERB 0x00430000u
#define RING 0x00440000u
#define SAMPLES 0x00500000u
#define LEFT 0x00300000u
#define RIGHT 0x00301000u
#define FRAME 0x50u

/* Super Mario Galaxy's member: no audio memory, the louder placed voice. */
#define GALAXY_FLAGS (GC_ZELDA_NO_ARAM | GC_ZELDA_MAKE_DOLBY_LOUDER | GC_ZELDA_COMBINED_CMD_0D)

static void put16(GcMachine *m, uint32_t at, unsigned value) {
  uint8_t *p = gc_machine_ram(m, at, 2);
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static int get16(GcMachine *m, uint32_t at) {
  const uint8_t *p = gc_machine_ram(m, at, 2);
  return (int16_t)((p[0] << 8) | p[1]);
}

static void put32(GcMachine *m, uint32_t at, uint32_t value) {
  put16(m, at, value >> 16);
  put16(m, at + 2u, value & 0xffffu);
}

/* A word of voice `voice`'s parameter block. */
static void vput(GcMachine *m, unsigned voice, unsigned word, unsigned value) {
  put16(m, VOICES + voice * 0x180u + word * 2u, value);
}
static int vget(GcMachine *m, unsigned voice, unsigned word) { return get16(m, VOICES + voice * 0x180u + word * 2u); }
static void vput32(GcMachine *m, unsigned voice, unsigned word, uint32_t value) {
  put32(m, VOICES + voice * 0x180u + word * 2u, value);
}
/* Destination `channel` of a voice: a buffer, a target volume, the volume now. */
static void destination(GcMachine *m, unsigned voice, unsigned channel, unsigned buffer, unsigned target, unsigned now) {
  vput(m, voice, 0x08u + channel * 4u, buffer);
  vput(m, voice, 0x09u + channel * 4u, target);
  vput(m, voice, 0x0au + channel * 4u, now);
}

/* A machine with the mixer set up and nothing in its tables but a
 * resampler that gives, at every position, half the newest of its four
 * samples: so at one raw sample to one output sample, output i is half of
 * raw sample i - 1, and output 0 is half of the last sample of the frame
 * before. */
static GcMachine *machine(GcZelda **z_out) {
  GcMachine *m = gc_machine_create();
  GcZelda *z;
  unsigned i;
  if (!m || !(z = gc_ax_zelda(m))) {
    printf("gc_zelda_mix_test: no machine\n");
    exit(2);
  }
  memset(z, 0, sizeof(*z));
  z->flags = GALAXY_FLAGS;
  z->voice_blocks = VOICES;
  z->tables = TABLES;
  z->afc_coefficients = AFC;
  z->reverb_blocks = 0;
  z->sample_base = SAMPLES;
  z->output_volume = 0x1000u; /* 4.12: one */
  for (i = 0; i < 64u; i++) put16(m, TABLES + i * 8u + 6u, 0x4000u);
  /* The decoder's second pair of coefficients: the sample before, whole. */
  put16(m, AFC + 4u, 0x0800u);
  gc_zelda_mix_setup(m, z);
  *z_out = z;
  return m;
}

/* One frame of voices 0 to `voices` - 1, written to LEFT and RIGHT. */
static void frame(GcMachine *m, GcZelda *z, unsigned voices) {
  unsigned v;
  z->output_left = LEFT;
  z->output_right = RIGHT;
  gc_zelda_mix_prepare(m, z);
  for (v = 0; v < voices; v++) gc_zelda_mix_voice(m, z, v);
  gc_zelda_mix_finish(m, z);
}

static int left(GcMachine *m, unsigned i) { return get16(m, LEFT + i * 2u); }
static int right(GcMachine *m, unsigned i) { return get16(m, RIGHT + i * 2u); }

/* Every sample of a channel is `value`. */
static int all(GcMachine *m, uint32_t at, int value) {
  unsigned i;
  for (i = 0; i < FRAME; i++) {
    if (get16(m, at + i * 2u) != value) return 0;
  }
  return 1;
}

/* ------------------------------------------------------------ the tests */

static void held_sample(void) {
  GcZelda *z;
  GcMachine *m = machine(&z);
  unsigned i, bad = 0;

  CHECK(z->resampling[3] == 0x4000 && z->resampling[0xff] == 0x4000 && z->resampling[2] == 0 && z->afc[2] == 0x0800,
        "the tables are copied at set-up, big-endian");

  frame(m, z, 1);
  CHECK(all(m, LEFT, 0) && all(m, RIGHT, 0), "a frame whose voice is not enabled is silence");
  CHECK(z->output_left == LEFT + 0xa0u && z->output_right == RIGHT + 0xa0u, "and the output moves on by a frame");
  CHECK(z->voices_mixed == 0u, "a voice that is not enabled is not mixed");

  vput(m, 0, 0x00, 1);      /* enabled */
  vput(m, 0, 0x04, 1);      /* reset */
  vput(m, 0, 0x06, 1);      /* hold the sample */
  vput(m, 0, 0x33, 0x1000); /* the sample */
  destination(m, 0, 0, 0x0d00, 0x4000, 0x4000); /* front left, a quarter throughout (16.16's top half) */
  destination(m, 0, 1, 0x0d60, 0x5000, 0x0000); /* front right, from nothing to 0x5000 */
  frame(m, z, 1);
  /* 0x1000 at 0x4000 of 0x10000 is 0x400. */
  CHECK(all(m, LEFT, 0x0400), "a held sample at a steady volume (left[0] is %04x)", left(m, 0) & 0xffff);
  /* 0x5000 in eighty steps is 0x100 a sample, the first sample at nothing:
   * sample i at i * 0x100 of 0x10000 is i * 0x10. */
  for (i = 0; i < FRAME; i++) bad += right(m, i) != (int)(i * 0x10u);
  CHECK(!bad, "a held sample at a ramping volume (%u wrong; right[1] is %04x, right[79] is %04x)", bad,
        right(m, 1) & 0xffff, right(m, 79) & 0xffff);
  CHECK(vget(m, 0, 0x0e) == 0x5000, "the volume the ramp reached is written back (%04x)", vget(m, 0, 0x0e) & 0xffff);
  CHECK(vget(m, 0, 0x04) == 1, "a voice holding a sample stays in reset");
  CHECK(z->voices_mixed == 1u && z->voices_placed == 0u, "one voice mixed, by destination");

  /* The frame as a whole at half: 4.12. */
  z->output_volume = 0x0800u;
  frame(m, z, 1);
  CHECK(all(m, LEFT, 0x0200), "the output volume (left[0] is %04x)", left(m, 0) & 0xffff);
  /* The right has arrived at 0x5000: 0x1000 at 0x5000 of 0x10000 is 0x500, halved. */
  CHECK(all(m, RIGHT, 0x0280), "a ramp that has arrived stays (right[0] is %04x)", right(m, 0) & 0xffff);

  /* A destination that is no buffer is passed over. */
  destination(m, 0, 2, 0x1234, 0x4000, 0x4000);
  z->output_volume = 0x1000u;
  frame(m, z, 1);
  CHECK(all(m, LEFT, 0x0400), "a destination that names no buffer changes nothing");
  gc_machine_destroy(m);
}

static void eight_bit(void) {
  GcZelda *z;
  GcMachine *m = machine(&z);
  uint8_t *data = gc_machine_ram(m, SAMPLES + 0x100u, 128);
  unsigned i, bad;

  for (i = 0; i < 100u; i++) data[i] = (uint8_t)(i + 1u); /* sample k is (k + 1) * 256 */
  vput(m, 0, 0x00, 1);
  vput(m, 0, 0x04, 1);
  vput(m, 0, 0x02, 0x1000);      /* one raw sample to one output sample */
  vput(m, 0, 0x80, 8);           /* eight-bit samples */
  vput32(m, 0, 0x8a, 100);       /* a hundred samples long */
  vput32(m, 0, 0x8c, 0x100);     /* from here in sample memory */
  destination(m, 0, 0, 0x0d00, 0x4000, 0x4000);

  frame(m, z, 1);
  /* Output i is half of sample i - 1 (which is i * 256), at a quarter. */
  for (bad = 0, i = 0; i < FRAME; i++) bad += left(m, i) != (int)(i * 32u);
  CHECK(!bad, "eight-bit samples, first frame (%u wrong; left[1] is %d)", bad, left(m, 1));
  CHECK(vget(m, 0, 0x04) == 0, "a voice that has played is no longer in reset");
  CHECK(vget(m, 0, 0x01) == 0, "and is not done with twenty samples left");
  CHECK(vget(m, 0, 0x3f) == 80 * 256, "the last raw sample is kept for the next frame's resampling (%d)", vget(m, 0, 0x3f));

  frame(m, z, 1);
  for (bad = 0, i = 0; i < FRAME; i++) bad += left(m, i) != (i <= 20u ? (int)((80u + i) * 32u) : 0);
  CHECK(!bad, "eight-bit samples, the frame they end in (%u wrong; left[0] %d, left[20] %d, left[21] %d)", bad,
        left(m, 0), left(m, 20), left(m, 21));
  CHECK(vget(m, 0, 0x01) == 1, "a voice that has played its samples says it is done");
  CHECK(z->voices_finished == 1u, "and is counted");

  frame(m, z, 1);
  CHECK(all(m, LEFT, 0), "a voice that is done is silent");
  CHECK(z->voices_mixed == 2u, "and is not mixed (%llu)", (unsigned long long)z->voices_mixed);
  CHECK((z->sources_used >> 8) & 1u, "the source is noted");
  gc_machine_destroy(m);
}

static void sixteen_bit_loop(void) {
  GcZelda *z;
  GcMachine *m = machine(&z);
  unsigned i;

  for (i = 0; i < 100u; i++) put16(m, SAMPLES + 0x200u + i * 2u, i * 100u); /* sample k is k * 100 */
  vput(m, 0, 0x00, 1);
  vput(m, 0, 0x04, 1);
  vput(m, 0, 0x02, 0x1000);
  vput(m, 0, 0x80, 16);      /* sixteen-bit samples */
  vput(m, 0, 0x81, 1);       /* looping */
  vput32(m, 0, 0x88, 40);    /* back to sample 40 */
  vput32(m, 0, 0x8a, 100);   /* from the end, sample 100 */
  vput32(m, 0, 0x8c, 0x200);
  destination(m, 0, 0, 0x0d00, 0x4000, 0x4000);

  frame(m, z, 1);
  /* Output i is half of sample i - 1, at a quarter: (i - 1) * 100 / 8, rounded down. */
  CHECK(left(m, 0) == 0 && left(m, 1) == 0 && left(m, 2) == 12 && left(m, 79) == 975,
        "sixteen-bit samples (left[2] %d, left[79] %d)", left(m, 2), left(m, 79));
  frame(m, z, 1);
  /* Samples 80 to 99, then 40 onwards: output 0 is sample 79, output 20
   * sample 99, output 21 sample 40, output 79 sample 98. */
  CHECK(left(m, 0) == 987 && left(m, 20) == 1237 && left(m, 21) == 500 && left(m, 79) == 1225,
        "a loop goes back to its start (left[0] %d, left[20] %d, left[21] %d, left[79] %d)", left(m, 0), left(m, 20),
        left(m, 21), left(m, 79));
  CHECK(vget(m, 0, 0x01) == 0, "and the voice is not done");
  gc_machine_destroy(m);
}

/* Ten blocks of nine bytes: a header choosing a step of 128 and the second
 * coefficient pair (the sample before, whole), and sixteen codes of 1. So
 * sample n (from 1) is n * 128, to 160 * 128. */
static void coded_blocks(GcMachine *m) {
  uint8_t *data = gc_machine_ram(m, SAMPLES + 0x400u, 90);
  unsigned b;
  for (b = 0; b < 10u; b++) {
    data[b * 9u] = 0x71u;
    memset(data + b * 9u + 1u, 0x11, 8);
  }
}

static void block_coded(void) {
  GcZelda *z;
  GcMachine *m = machine(&z);
  unsigned i, bad;

  coded_blocks(m);
  vput(m, 0, 0x00, 1);
  vput(m, 0, 0x04, 1);
  vput(m, 0, 0x02, 0x1000);
  vput(m, 0, 0x80, 9);         /* block-coded, four bits a sample */
  vput32(m, 0, 0x8a, 160);     /* a hundred and sixty samples long */
  vput32(m, 0, 0x8c, 0x400);
  destination(m, 0, 0, 0x0d00, 0x4000, 0x4000);

  frame(m, z, 1);
  /* Output i is half of sample i (n * 128), at a quarter: i * 16. */
  for (bad = 0, i = 0; i < FRAME; i++) bad += left(m, i) != (int)(i * 16u);
  CHECK(!bad, "block-coded samples, first frame (%u wrong; left[1] %d, left[79] %d)", bad, left(m, 1), left(m, 79));
  frame(m, z, 1);
  for (bad = 0, i = 0; i < FRAME; i++) bad += left(m, i) != (int)((80u + i) * 16u);
  CHECK(!bad, "block-coded samples carry on from the two before (%u wrong; left[0] %d)", bad, left(m, 0));
  CHECK(vget(m, 0, 0x01) == 0, "a voice whose samples ran out exactly is not done until more are asked for");
  frame(m, z, 1);
  CHECK(left(m, 0) == 160 * 16 && left(m, 1) == 0 && left(m, 79) == 0, "then silence (left[0] %d, left[1] %d)",
        left(m, 0), left(m, 1));
  CHECK(vget(m, 0, 0x01) == 1, "and done");
  gc_machine_destroy(m);

  /* At half speed a frame takes forty samples, and blocks come sixteen at
   * a time: the first frame decodes forty-eight and leaves eight for the
   * second. */
  m = machine(&z);
  coded_blocks(m);
  vput(m, 0, 0x00, 1);
  vput(m, 0, 0x04, 1);
  vput(m, 0, 0x02, 0x0800);
  vput(m, 0, 0x80, 9);
  vput32(m, 0, 0x8a, 160);
  vput32(m, 0, 0x8c, 0x400);
  destination(m, 0, 0, 0x0d00, 0x4000, 0x4000);
  frame(m, z, 1);
  for (bad = 0, i = 0; i < FRAME; i++) bad += left(m, i) != (int)((i / 2u) * 16u);
  CHECK(!bad, "block-coded samples at half speed (%u wrong; left[2] %d, left[79] %d)", bad, left(m, 2), left(m, 79));
  CHECK(vget(m, 0, 0x32) == 8, "eight decoded samples are left over (%d)", vget(m, 0, 0x32));
  frame(m, z, 1);
  for (bad = 0, i = 0; i < FRAME; i++) bad += left(m, i) != (int)((40u + i / 2u) * 16u);
  CHECK(!bad, "and are played first in the next frame (%u wrong; left[0] %d, left[2] %d)", bad, left(m, 0), left(m, 2));
  frame(m, z, 1);
  for (bad = 0, i = 0; i < FRAME; i++) bad += left(m, i) != (int)((80u + i / 2u) * 16u);
  CHECK(!bad, "and again (%u wrong; left[0] %d)", bad, left(m, 0));
  gc_machine_destroy(m);
}

static void placed(void) {
  GcZelda *z;
  GcMachine *m = machine(&z);
  unsigned frames;

  /* A quarter sine reduced to its ends: nothing at 0, everything at 127.
   * A voice at the far left and the very front is then all in one corner. */
  put16(m, TABLES + 0x400u + 0x7fu * 2u, 0x7fff);
  gc_zelda_mix_setup(m, z);

  vput(m, 0, 0x00, 1);
  vput(m, 0, 0x04, 1);
  vput(m, 0, 0x06, 1);
  vput(m, 0, 0x33, 0x1000);
  vput(m, 0, 0x2c, 1);      /* placed */
  vput(m, 0, 0x28, 0x0000); /* far left, very front */
  vput(m, 0, 0x29, 0x4000); /* half of it to the reverb as well */
  vput(m, 0, 0x2a, 0x4000); /* volume now */
  vput(m, 0, 0x2b, 0x4000); /* and its target */

  z->output_left = LEFT;
  z->output_right = RIGHT;
  gc_zelda_mix_prepare(m, z);
  gc_zelda_mix_voice(m, z, 0);
  /* The corner's gain is 0x7fff squared, 0x7ffe of 0x8000 on this member;
   * at volume 0x4000 of 0x8000 that is 0x3fff, and 0x1000 at 0x3fff of
   * 0x10000 is 0x3ff. Half of 0x3fff to the reverb is 0x1fff: 0x1ff. */
  CHECK(z->buffer[4][0] == 0x01ff && z->buffer[4][79] == 0x01ff, "a placed voice's reverb send (%04x)",
        z->buffer[4][0] & 0xffff);
  CHECK(z->buffer[2][0] == 0 && z->buffer[3][0] == 0, "and nothing behind");
  gc_zelda_mix_finish(m, z);
  CHECK(all(m, LEFT, 0x03ff) && all(m, RIGHT, 0), "a placed voice in the front left corner (left[0] %04x, right[0] %04x)",
        left(m, 0) & 0xffff, right(m, 0) & 0xffff);
  CHECK(z->voices_placed == 1u, "counted as placed");

  /* Far right: the other corner. */
  vput(m, 0, 0x28, 0x7f00);
  frame(m, z, 1);
  CHECK(all(m, LEFT, 0) && all(m, RIGHT, 0x03ff), "and in the front right (left[0] %04x, right[0] %04x)",
        left(m, 0) & 0xffff, right(m, 0) & 0xffff);

  /* Asked to stop: the target is half the volume, each frame, and the
   * frame ramps to it. The corner's step is -0x2000 over the frame, so the
   * last sample is at 0x3fff0000 - 79 * (0x20000000 / 80) = 0x2065.... of
   * 0x10000: 0x1000 at 0x2065 is 0x206. */
  vput(m, 0, 0x85, 1);
  frame(m, z, 1);
  CHECK(right(m, 0) == 0x03ff && right(m, 79) == 0x0206, "a voice asked to stop fades (right[0] %04x, right[79] %04x)",
        right(m, 0) & 0xffff, right(m, 79) & 0xffff);
  CHECK(vget(m, 0, 0x2a) == 0x2000, "to half its volume in a frame (%04x)", vget(m, 0, 0x2a) & 0xffff);
  CHECK(vget(m, 0, 0x01) == 0, "and is not done yet");
  /* 0x2000 halves to nothing in fourteen more frames. */
  for (frames = 1; frames < 40u && !vget(m, 0, 0x01); frames++) frame(m, z, 1);
  CHECK(frames == 15u && vget(m, 0, 0x01) == 1, "and is done when its volume reaches nothing (%u frames)", frames);
  CHECK(z->voices_finished == 1u, "which is counted");
  gc_machine_destroy(m);
}

static void reverb_line(void) {
  GcZelda *z;
  GcMachine *m = machine(&z);

  /* The first line: on, its filter run before its destinations; a ring of
   * two frames; to the front left at a half. The filter is the sample
   * before, whole. */
  put16(m, REVERB + 0u, 1);
  put16(m, REVERB + 2u, 2);
  put32(m, REVERB + 4u, RING);
  put16(m, REVERB + 8u, 0x0d00);
  put16(m, REVERB + 10u, 0x4000);
  put16(m, REVERB + 16u + 7u * 2u, 0x7fff);
  z->reverb_blocks = REVERB;

  /* A held sample sent to the line and nowhere else. */
  vput(m, 0, 0x00, 1);
  vput(m, 0, 0x06, 1);
  vput(m, 0, 0x33, 0x1000);
  destination(m, 0, 0, 0x0dc0, 0x4000, 0x4000);

  frame(m, z, 1);
  CHECK(all(m, LEFT, 0), "what is sent to a reverb line is not heard at once");
  CHECK(get16(m, RING) == 0x0400 && get16(m, RING + 79u * 2u) == 0x0400 && get16(m, RING + 80u * 2u) == 0,
        "it is written into the line's ring (%04x)", get16(m, RING) & 0xffff);
  frame(m, z, 1);
  CHECK(all(m, LEFT, 0), "nor a frame later, in a ring of two");
  CHECK(get16(m, RING + 80u * 2u) == 0x0400, "the second frame is the ring's second");
  frame(m, z, 1);
  /* The first frame comes back: through the filter it is 0x400 * 0x7fff /
   * 0x8000 = 0x3ff, a sample late; at a half, 0x1ff. */
  CHECK(left(m, 0) == 0 && left(m, 1) == 0x01ff && left(m, 79) == 0x01ff,
        "and comes out a ring later, filtered (left[0] %04x, left[1] %04x)", left(m, 0) & 0xffff, left(m, 1) & 0xffff);
  /* And what came out is what the voice's new frame is added to. */
  CHECK(get16(m, RING + 2u) == 0x07ff, "what comes out goes round again (%04x)", get16(m, RING + 2u) & 0xffff);
  CHECK(z->reverb_frames == 3u, "three frames of one line (%llu)", (unsigned long long)z->reverb_frames);
  gc_machine_destroy(m);
}

int main(void) {
  held_sample();
  eight_bit();
  sixteen_bit_loop();
  block_coded();
  placed();
  reverb_line();
  if (failures) {
    printf("gc_zelda_mix_test: %u of %u checks failed\n", failures, checks);
    return 1;
  }
  printf("gc_zelda_mix_test: %u checks: the Zelda microcode's mixer plays held, eight-bit, sixteen-bit and "
         "block-coded samples, ramps and places them, ends them, and delays them through a reverb line as "
         "docs/gc-dsp-zelda.md says\n",
         checks);
  return 0;
}
