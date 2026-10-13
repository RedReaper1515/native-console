/*
 * The sound microcode's command list, voices and decoder.
 *
 * There is no hardware oracle for any of this - no test suite, no captured
 * output - so these are not comparisons against hardware. They are checks
 * that the code does what docs/gc-dsp-ax.md says, on cases chosen so that
 * the ways this goes wrong are distinguishable from each other:
 *
 *  - A walker that gets one argument count wrong reads the rest of the list
 *    as opcodes. So there is a list with a command that does nothing but has
 *    a long argument list, and the check is that the END after it is found.
 *
 *  - A decoder that treats a sample address as a byte address reads ADPCM at
 *    twice the rate. So the ADPCM case checks the *values* of consecutive
 *    samples against the arithmetic in the document, worked through by hand,
 *    rather than checking that something came out.
 *
 *  - The channel order in the output buffer is right before left, which is
 *    the one thing here nobody guesses. So the stereo case sends different
 *    volumes to the two channels and checks which one landed where.
 *
 * What none of these can check is whether the document is right about the
 * hardware. That is what the **Unverified:** markers in it are for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_ax.h"

static unsigned failures;

static void check(int condition, const char *what) {
  if (condition) return;
  fprintf(stderr, "gc_ax_test: FAIL %s\n", what);
  failures++;
}

static void check_eq(int32_t got, int32_t want, const char *what) {
  if (got == want) return;
  fprintf(stderr, "gc_ax_test: FAIL %s: got %d, want %d\n", what, (int)got, (int)want);
  failures++;
}

/* Somewhere to put things. The addresses are physical, which is what the
 * sound processor sees. */
#define LIST_AT 0x00100000u
#define VOICE_AT 0x00110000u
#define SETUP_AT 0x00120000u
#define SAMPLES_AT 0x00130000u
#define STEREO_AT 0x00140000u
#define SURROUND_AT 0x00150000u

static void put16(GcMachine *m, uint32_t address, uint16_t value) {
  uint8_t *p = gc_machine_ram(m, address, 2);
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static uint16_t get16(GcMachine *m, uint32_t address) {
  const uint8_t *p = gc_machine_ram(m, address, 2);
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Sample data goes in audio memory, where the sound processor reads it; the
 * lists, blocks and buffers above are in main memory. A test that put its
 * samples in main memory passed against a decoder that read them from there,
 * which is exactly the decoder that played Shrek 2 as static. */
static void aram_put8(GcMachine *m, uint32_t address, uint8_t value) { m->aram[address] = value; }

static void aram_put16(GcMachine *m, uint32_t address, uint16_t value) {
  m->aram[address] = (uint8_t)(value >> 8);
  m->aram[address + 1u] = (uint8_t)value;
}

/* The parameter block's halfword offsets, as gc_ax.c derives them. Repeated
 * here on purpose: a test that imported them from the implementation would
 * agree with it about a layout they were both wrong about. */
enum {
  PB_NEXT = 0, PB_SRC_TYPE = 4, PB_MIXER_CONTROL = 6, PB_RUNNING = 7, PB_IS_STREAM = 8,
  PB_SENDS = 9, PB_CUR_VOLUME = 50, PB_VOLUME_DELTA = 51, PB_LOOPING = 55, PB_FORMAT = 56,
  PB_LOOP_ADDR = 57, PB_END_ADDR = 59, PB_CUR_ADDR = 61, PB_COEFS = 63, PB_GAIN = 79,
  PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82, PB_RATIO = 83, PB_FRAC = 85, PB_LAST = 86,
  PB_LPF_ON = 93, PB_HALFWORDS = 122
};

static void pb_set(GcMachine *m, unsigned field, uint16_t value) {
  put16(m, VOICE_AT + field * 2u, value);
}

static void pb_set32(GcMachine *m, unsigned field, uint32_t value) {
  pb_set(m, field, (uint16_t)(value >> 16));
  pb_set(m, field + 1u, (uint16_t)value);
}

/* A voice that plays what is at SAMPLES_AT and nothing more: no loop, no
 * filter, no ramp, unity everywhere, and one input sample per output. */
static void plain_voice(GcMachine *m, uint16_t format, uint32_t last_address) {
  unsigned i;
  for (i = 0; i < PB_HALFWORDS; i++) pb_set(m, i, 0);
  pb_set32(m, PB_NEXT, 0);
  pb_set(m, PB_SRC_TYPE, 2);      /* nearest: no resampling to reason about */
  pb_set(m, PB_RUNNING, 1);
  pb_set(m, PB_FORMAT, format);
  pb_set32(m, PB_CUR_ADDR, 0);
  pb_set32(m, PB_END_ADDR, last_address);
  pb_set32(m, PB_LOOP_ADDR, 0);
  pb_set(m, PB_LOOPING, 0);
  pb_set32(m, PB_RATIO, 0x00010000u); /* one input sample per output sample */
  pb_set(m, PB_FRAC, 0);              /* nearest ignores both */
  pb_set(m, PB_CUR_VOLUME, 0x7fff);   /* unity, near enough: (x * 0x7fff) >> 15 */
  pb_set(m, PB_VOLUME_DELTA, 0);
  pb_set(m, PB_LPF_ON, 0);
}

/* SETUP, SET_VOICE_LIST, PROCESS, OUTPUT, END - the five that make a mix. */
static uint32_t plain_list(GcMachine *m) {
  unsigned i, at = 0;
  for (i = 0; i < 27u; i++) put16(m, SETUP_AT + i * 2u, 0); /* nine buffers of three */
  put16(m, LIST_AT + at++ * 2u, 0x0000);
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(SETUP_AT >> 16u));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(SETUP_AT & 0xffffu));
  put16(m, LIST_AT + at++ * 2u, 0x0002);
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(VOICE_AT >> 16u));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(VOICE_AT & 0xffffu));
  put16(m, LIST_AT + at++ * 2u, 0x0003);
  put16(m, LIST_AT + at++ * 2u, 0x000e);
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(SURROUND_AT >> 16u));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(SURROUND_AT & 0xffffu));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(STEREO_AT >> 16u));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(STEREO_AT & 0xffffu));
  put16(m, LIST_AT + at++ * 2u, 0x000f);
  return at;
}

/* ------------------------------------------------------------------ */

/* A 16-bit voice, sent hard to one side. This is the case that says which
 * halfword of an output frame is which channel. */
static void test_stereo_order(GcMachine *m) {
  unsigned i;
  uint32_t halfwords;

  for (i = 0; i < 200u; i++) aram_put16(m, SAMPLES_AT + i * 2u, 1000);
  plain_voice(m, 0x000a, 199u); /* PCM16: the address counts halfwords */
  pb_set32(m, PB_CUR_ADDR, SAMPLES_AT / 2u);
  pb_set32(m, PB_END_ADDR, SAMPLES_AT / 2u + 199u);
  pb_set(m, PB_GAIN, 0x0800); /* unity for a gain shift of 11 */
  pb_set(m, PB_MIXER_CONTROL, 0x0003); /* main L and R, no ramping */
  pb_set(m, PB_SENDS + 0u, 0x7fff);    /* main L: full */
  pb_set(m, PB_SENDS + 2u, 0x0000);    /* main R: nothing */

  halfwords = plain_list(m);
  check_eq(gc_ax_run_list(m, LIST_AT, halfwords), 0, "the list ends at its END");

  /* Right first, then left. A voice sent only to the left must therefore
   * leave the *second* halfword of every frame loud and the first silent. */
  check_eq((int16_t)get16(m, STEREO_AT + 0u), 0, "the first halfword of a frame is the right channel");
  check((int16_t)get16(m, STEREO_AT + 2u) > 900, "and the second is the left");
  check((int16_t)get16(m, STEREO_AT + 4u * 159u + 2u) > 900, "for all 160 frames");
}

/* ADPCM, against the arithmetic worked through by hand.
 *
 * One frame: a header byte of 0x00 - scale exponent 0, coefficient pair 0 -
 * followed by nibbles. With both coefficients zero the predictor term is
 * just the rounding constant shifted away, so each output is the scaled
 * residual: nibble 1 gives 1, nibble 0xf gives -1. That is a weak check on
 * its own, so the second frame uses a real coefficient. */
static void test_adpcm(GcMachine *m) {
  unsigned i;
  uint32_t halfwords;

  for (i = 0; i < 64u; i++) aram_put8(m, SAMPLES_AT + i, 0);
  aram_put8(m, SAMPLES_AT + 0u, 0x00);  /* scale 1 << 0, coefficient pair 0 */
  aram_put8(m, SAMPLES_AT + 1u, 0x12);  /* nibbles 1, 2 */
  aram_put8(m, SAMPLES_AT + 2u, 0xf0);  /* nibbles -1, 0 */

  plain_voice(m, 0x0000, 0); /* ADPCM: the address counts nibbles */
  /* Started the way the library starts a sound: past the first header, with
   * that header's value already in the block. */
  pb_set32(m, PB_CUR_ADDR, SAMPLES_AT * 2u + 2u);
  pb_set(m, PB_PRED_SCALE, 0x00);
  pb_set32(m, PB_END_ADDR, SAMPLES_AT * 2u + 1000u);
  for (i = 0; i < 16u; i++) pb_set(m, PB_COEFS + i, 0);
  pb_set(m, PB_MIXER_CONTROL, 0x0003);
  pb_set(m, PB_SENDS + 0u, 0x7fff);
  pb_set(m, PB_SENDS + 2u, 0x7fff);

  halfwords = plain_list(m);
  check_eq(gc_ax_run_list(m, LIST_AT, halfwords), 0, "the ADPCM list ends at its END");

  /* The stepping is what this case is really for, and it is the check that
   * catches an address treated as bytes rather than nibbles.
   *
   * A hundred and sixty samples at fourteen samples to a frame is eleven
   * whole frames and six more. Eleven frames are eleven times sixteen
   * nibbles; the twelfth contributes its two-nibble header and six samples.
   * Read the addresses as bytes instead and this comes out at half. (The
   * start's own header is counted in the first frame.) */
  check_eq((int32_t)(((uint32_t)get16(m, VOICE_AT + PB_CUR_ADDR * 2u) << 16) |
                     get16(m, VOICE_AT + (PB_CUR_ADDR + 1u) * 2u)) -
               (int32_t)(SAMPLES_AT * 2u),
           11 * 16 + 2 + 6, "160 ADPCM samples step eleven frames and six nibbles");
}

/* A louder ADPCM case, so the decoder's arithmetic is actually exercised:
 * scale exponent 8 makes each residual 256 times bigger, which survives the
 * volume stages. */
static void test_adpcm_values(GcMachine *m) {
  unsigned i;
  uint32_t halfwords;

  for (i = 0; i < 64u; i++) aram_put8(m, SAMPLES_AT + i, 0);
  aram_put8(m, SAMPLES_AT + 0u, 0x08);  /* scale 1 << 8 = 256, coefficient pair 0 */
  aram_put8(m, SAMPLES_AT + 1u, 0x1f);  /* nibbles 1, -1 */
  aram_put8(m, SAMPLES_AT + 2u, 0x20);  /* nibbles 2, 0 */

  plain_voice(m, 0x0000, 0);
  pb_set32(m, PB_CUR_ADDR, SAMPLES_AT * 2u + 2u);
  pb_set(m, PB_PRED_SCALE, 0x08);
  pb_set32(m, PB_END_ADDR, SAMPLES_AT * 2u + 1000u);
  for (i = 0; i < 16u; i++) pb_set(m, PB_COEFS + i, 0);
  pb_set(m, PB_MIXER_CONTROL, 0x0003);
  pb_set(m, PB_SENDS + 0u, 0x7fff);
  pb_set(m, PB_SENDS + 2u, 0x7fff);

  halfwords = plain_list(m);
  gc_ax_run_list(m, LIST_AT, halfwords);

  /* The residuals are 1, -1, 2, 0 and the scale is 256, so the decoder's
   * output is 256, -256, 512, 0. Each then passes two (x * 0x7fff) >> 15
   * stages, the volume envelope and the send, which round a positive value
   * down by one each time and leave a negative one where it is. */
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 254, "a residual of 1 at scale 256");
  check_eq((int16_t)get16(m, STEREO_AT + 6u), -256, "a residual of -1");
  check_eq((int16_t)get16(m, STEREO_AT + 10u), 510, "a residual of 2");
  check_eq((int16_t)get16(m, STEREO_AT + 14u), 0, "and a residual of 0");
}

/* A command the walker does nothing with, but whose arguments it must step
 * over exactly. If the count is wrong, the END after it is never found and
 * the list is reported as bad. */
static void test_walker_stays_in_step(GcMachine *m) {
  unsigned at = 0;
  /* 0x13 SEND_AUX_AND_MIX: twelve argument halfwords, no effect here. */
  put16(m, LIST_AT + at++ * 2u, 0x0013);
  while (at < 13u) put16(m, LIST_AT + at++ * 2u, 0xdead);
  put16(m, LIST_AT + at++ * 2u, 0x000f);
  check_eq(gc_ax_run_list(m, LIST_AT, at), 0, "a skipped command's arguments are stepped over");

  /* And a command nobody knows: its length is unknown, so the walk has to
   * stop rather than guess. Reading on would execute its arguments. */
  at = 0;
  put16(m, LIST_AT + at++ * 2u, 0x00ff);
  put16(m, LIST_AT + at++ * 2u, 0x000f);
  check(gc_ax_run_list(m, LIST_AT, at) != 0, "an unknown command stops the walk");
}

/* A voice that is not running contributes nothing, and one that runs off the
 * end of a sample without looping stops itself. */
static void test_running(GcMachine *m) {
  uint32_t halfwords;
  unsigned i;

  for (i = 0; i < 200u; i++) aram_put16(m, SAMPLES_AT + i * 2u, 4000);
  plain_voice(m, 0x000a, 0);
  pb_set32(m, PB_CUR_ADDR, SAMPLES_AT / 2u);
  pb_set32(m, PB_END_ADDR, SAMPLES_AT / 2u + 199u);
  pb_set(m, PB_GAIN, 0x0800);
  pb_set(m, PB_MIXER_CONTROL, 0x0003);
  pb_set(m, PB_SENDS + 0u, 0x7fff);
  pb_set(m, PB_SENDS + 2u, 0x7fff);
  pb_set(m, PB_RUNNING, 0); /* not running */

  halfwords = plain_list(m);
  gc_ax_run_list(m, LIST_AT, halfwords);
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 0, "a voice that is not running is silent");

  /* Running, but with only 200 samples for 160 output samples and no loop:
   * it plays out and stops itself rather than reading past its end. */
  pb_set(m, PB_RUNNING, 1);
  pb_set32(m, PB_CUR_ADDR, SAMPLES_AT / 2u);
  pb_set32(m, PB_END_ADDR, SAMPLES_AT / 2u + 99u);
  gc_ax_run_list(m, LIST_AT, halfwords);
  check_eq(get16(m, VOICE_AT + PB_RUNNING * 2u), 0,
           "a voice that reaches the end without looping stops itself");
}

/* SETUP does not have to start from silence: a non-zero value primes the
 * buffer with a ramp, which is how the CPU feeds its own material in. */
static void test_setup_ramp(GcMachine *m) {
  unsigned i, at = 0;
  for (i = 0; i < 27u; i++) put16(m, SETUP_AT + i * 2u, 0);
  /* Main L: start at 100, add 1 per sample. Main R: zero, so silent. */
  put16(m, SETUP_AT + 0u, 0);
  put16(m, SETUP_AT + 2u, 100);
  put16(m, SETUP_AT + 4u, 1);

  put16(m, LIST_AT + at++ * 2u, 0x0000);
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(SETUP_AT >> 16u));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(SETUP_AT & 0xffffu));
  put16(m, LIST_AT + at++ * 2u, 0x000e);
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(SURROUND_AT >> 16u));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(SURROUND_AT & 0xffffu));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(STEREO_AT >> 16u));
  put16(m, LIST_AT + at++ * 2u, (uint16_t)(STEREO_AT & 0xffffu));
  put16(m, LIST_AT + at++ * 2u, 0x000f);
  check_eq(gc_ax_run_list(m, LIST_AT, at), 0, "the SETUP list ends at its END");

  check_eq((int16_t)get16(m, STEREO_AT + 2u), 100, "a primed buffer starts where it was told");
  check_eq((int16_t)get16(m, STEREO_AT + 4u * 10u + 2u), 110, "and ramps by its delta");
  check_eq((int16_t)get16(m, STEREO_AT + 0u), 0, "a buffer primed with zero is silent");
}

/* ------------------------------------------------------------------ */
/* The four things the first version had wrong, each found by measuring what
 * Shrek 2 sounded like (docs/GC-PLAN.md, "Clean sound"), and the compressor
 * and the auxiliary busses it had left out. */

static void put32(GcMachine *m, uint32_t address, uint32_t value) {
  put16(m, address, (uint16_t)(value >> 16));
  put16(m, address + 2u, (uint16_t)value);
}

static int32_t get32(GcMachine *m, uint32_t address) {
  return (int32_t)(((uint32_t)get16(m, address) << 16) | get16(m, address + 2u));
}

enum { PB_DPOP = 41, PB_LOOP_PRED_SCALE = 90, PB_UPDATES = 34 };

/* A PCM16 voice of a constant 1000, both main sends at `volume`. */
static void constant_voice(GcMachine *m, uint16_t volume) {
  unsigned i;
  for (i = 0; i < 400u; i++) aram_put16(m, SAMPLES_AT + i * 2u, 1000);
  plain_voice(m, 0x000a, 0);
  pb_set32(m, PB_CUR_ADDR, SAMPLES_AT / 2u);
  pb_set32(m, PB_END_ADDR, SAMPLES_AT / 2u + 399u);
  pb_set(m, PB_GAIN, 0x0800);
  pb_set(m, PB_MIXER_CONTROL, 0x0003);
  pb_set(m, PB_SENDS + 0u, volume);
  pb_set(m, PB_SENDS + 2u, volume);
}

/* 0x8000 is unity on a send - and it is the volume Shrek 2 plays its music
 * at. Read as a signed halfword it is -1.0, which inverted every voice sent
 * at unity against the ones sent just below it. */
static void test_send_volume_is_unsigned(GcMachine *m) {
  constant_voice(m, 0x8000);
  gc_ax_run_list(m, LIST_AT, plain_list(m));
  /* 1000 through the envelope at 0x7fff is 999; a unity send keeps it. */
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 999, "a send at 0x8000 is unity, not inversion");
  constant_voice(m, 0xc000);
  gc_ax_run_list(m, LIST_AT, plain_list(m));
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 1498, "and 0xc000 is one and a half");
}

/* A one-shot sound that ends part-way through a millisecond is silent for
 * the rest of it. The first version decoded the last nibble again for every
 * remaining sample, running the predictor on its own output - a buzz at the
 * end of every sound effect. */
static void test_one_shot_end_is_silent(GcMachine *m) {
  unsigned i;
  uint32_t halfwords;
  for (i = 0; i < 64u; i++) aram_put8(m, SAMPLES_AT + i, 0x77);
  aram_put8(m, SAMPLES_AT + 0u, 0x0a); /* scale 1 << 10, coefficient pair 0 */
  plain_voice(m, 0x0000, 0);
  pb_set32(m, PB_CUR_ADDR, SAMPLES_AT * 2u + 2u);
  pb_set(m, PB_PRED_SCALE, 0x0a);
  /* Ten samples: nibble addresses 2 to 11. */
  pb_set32(m, PB_END_ADDR, SAMPLES_AT * 2u + 11u);
  pb_set32(m, PB_LOOP_ADDR, SAMPLES_AT * 2u + 2u);
  pb_set(m, PB_COEFS + 0u, 0x0800); /* yn1 carried at full weight: a runaway if re-read */
  pb_set(m, PB_MIXER_CONTROL, 0x0003);
  pb_set(m, PB_SENDS + 0u, 0x8000);
  pb_set(m, PB_SENDS + 2u, 0x8000);
  halfwords = plain_list(m);
  gc_ax_run_list(m, LIST_AT, halfwords);
  check((int16_t)get16(m, STEREO_AT + 4u * 9u + 2u) != 0, "the last sample of a one-shot plays");
  for (i = 10u; i < 32u; i++) {
    if ((int16_t)get16(m, STEREO_AT + 4u * i + 2u) != 0) {
      check_eq((int16_t)get16(m, STEREO_AT + 4u * i + 2u), 0, "a one-shot is silent after its end");
      break;
    }
  }
  check_eq(get16(m, VOICE_AT + PB_RUNNING * 2u), 0, "and stops");
  check_eq((int32_t)(((uint32_t)get16(m, VOICE_AT + PB_CUR_ADDR * 2u) << 16) |
                     get16(m, VOICE_AT + (PB_CUR_ADDR + 1u) * 2u)),
           (int32_t)(SAMPLES_AT * 2u + 2u), "with the accelerator back at its start");
}

/* The de-pop values are grouped by channel - main, auxA, auxB left, then the
 * rights, then the surrounds - not in send order. */
static void test_dpop_order(GcMachine *m) {
  unsigned i;
  constant_voice(m, 0x8000);
  pb_set(m, PB_MIXER_CONTROL, 0x0002); /* main R only */
  for (i = 0; i < 9u; i++) pb_set(m, PB_DPOP + i, 0x1111);
  gc_ax_run_list(m, LIST_AT, plain_list(m));
  check_eq((int16_t)get16(m, VOICE_AT + (PB_DPOP + 3u) * 2u), 999, "main right's de-pop is the fourth");
  check_eq(get16(m, VOICE_AT + (PB_DPOP + 1u) * 2u), 0x1111, "and the second, auxA left's, is left alone");
}

/* The buffer SET_MAIN_LR takes is 160 32-bit samples. Read as halfwords, a
 * buffer of small values comes out as zeros between the samples. */
#define MAIN_LR_AT 0x00160000u
static void test_set_main_lr_is_32_bit(GcMachine *m) {
  unsigned i, at = 0;
  for (i = 0; i < 27u; i++) put16(m, SETUP_AT + i * 2u, 0);
  for (i = 0; i < 160u; i++) put32(m, MAIN_LR_AT + i * 4u, (uint32_t)(1000 + (int)i));
  put16(m, LIST_AT + at++ * 2u, 0x0000);
  put32(m, LIST_AT + at * 2u, SETUP_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x0011); /* SET_OPPOSITE_LR */
  put32(m, LIST_AT + at * 2u, MAIN_LR_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x000e);
  put32(m, LIST_AT + at * 2u, SURROUND_AT); at += 2u;
  put32(m, LIST_AT + at * 2u, STEREO_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x000f);
  check_eq(gc_ax_run_list(m, LIST_AT, at), 0, "the SET_OPPOSITE_LR list ends at its END");
  check_eq((int16_t)get16(m, STEREO_AT + 4u * 1u), 1001, "SET_OPPOSITE_LR's second sample is the buffer's second int");
  check_eq((int16_t)get16(m, STEREO_AT + 4u * 1u + 2u), -1001, "and the left is its negation");
  check_eq((int16_t)get16(m, STEREO_AT + 4u * 159u), 1159, "for all 160");
}

/* The compressor: a frame over the threshold is scaled by the attack ramp
 * the release position picks, and each quiet frame after it by the next
 * release ramp, until the release has run out. */
#define TABLE_AT 0x00170000u
static void test_compressor(GcMachine *m) {
  unsigned i, entry, at = 0;
  for (entry = 0; entry < 40u; entry++) {
    for (i = 0; i < 160u; i++) put16(m, TABLE_AT + (entry * 160u + i) * 2u, (uint16_t)(0x0400u * (entry + 1u)));
  }
  put16(m, LIST_AT + at++ * 2u, 0x0000);
  put32(m, LIST_AT + at * 2u, SETUP_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x0012);
  put16(m, LIST_AT + at++ * 2u, 10000);  /* threshold */
  put16(m, LIST_AT + at++ * 2u, 4);      /* release frames */
  put32(m, LIST_AT + at * 2u, TABLE_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x000e);
  put32(m, LIST_AT + at * 2u, SURROUND_AT); at += 2u;
  put32(m, LIST_AT + at * 2u, STEREO_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x000f);

  /* Quiet, nothing running: untouched. */
  for (i = 0; i < 27u; i++) put16(m, SETUP_AT + i * 2u, 0);
  put32(m, SETUP_AT + 0u, 8000);
  check_eq(gc_ax_run_list(m, LIST_AT, at), 0, "the COMPRESSOR list ends at its END");
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 8000, "under the threshold the compressor does nothing");

  /* Loud: attack ramp 0 (gain 0x0400, an eighth of unity... times 1). */
  put32(m, SETUP_AT + 0u, 20000);
  gc_ax_run_list(m, LIST_AT, at);
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 20000 * 0x0400 / 0x8000, "a loud frame takes attack ramp 0");

  /* Quiet again: release, position 4 - 1 = 3, ramp 11 + 3. */
  put32(m, SETUP_AT + 0u, 8000);
  gc_ax_run_list(m, LIST_AT, at);
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 8000 * (0x0400 * 15) / 0x8000, "the next quiet frame releases on ramp 14");
  gc_ax_run_list(m, LIST_AT, at);
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 8000 * (0x0400 * 14) / 0x8000, "then ramp 13");

  /* Loud during the release: the attack ramp is the release position (2). */
  put32(m, SETUP_AT + 0u, 20000);
  gc_ax_run_list(m, LIST_AT, at);
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 20000 * (0x0400 * 3) / 0x8000, "a loud frame mid-release attacks from there");

  /* And the release runs out: four quiet frames, then untouched. */
  put32(m, SETUP_AT + 0u, 8000);
  for (i = 0; i < 4u; i++) gc_ax_run_list(m, LIST_AT, at);
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 8000 * (0x0400 * 12) / 0x8000, "the last release frame is ramp 11");
  gc_ax_run_list(m, LIST_AT, at);
  check_eq((int16_t)get16(m, STEREO_AT + 2u), 8000, "and after the release the mix is untouched");
}

/* An auxiliary bus: its three buffers go out to memory, and three come back
 * and are added into the main mix. */
#define AUX_UP_AT 0x00180000u
#define AUX_DOWN_AT 0x00190000u
static void test_aux_bus(GcMachine *m) {
  unsigned i, at = 0;
  constant_voice(m, 0x8000);
  pb_set(m, PB_MIXER_CONTROL, 0x0010); /* auxA L only */
  pb_set(m, PB_SENDS + 4u, 0x8000);    /* the auxA L send */
  for (i = 0; i < 27u; i++) put16(m, SETUP_AT + i * 2u, 0);
  for (i = 0; i < 480u; i++) put32(m, AUX_DOWN_AT + i * 4u, i < 160u ? 7u : 0u);
  put16(m, LIST_AT + at++ * 2u, 0x0000);
  put32(m, LIST_AT + at * 2u, SETUP_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x0002);
  put32(m, LIST_AT + at * 2u, VOICE_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x0003);
  put16(m, LIST_AT + at++ * 2u, 0x0004); /* MIX_AUXA */
  put32(m, LIST_AT + at * 2u, AUX_UP_AT); at += 2u;
  put32(m, LIST_AT + at * 2u, AUX_DOWN_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x000e);
  put32(m, LIST_AT + at * 2u, SURROUND_AT); at += 2u;
  put32(m, LIST_AT + at * 2u, STEREO_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x000f);
  check_eq(gc_ax_run_list(m, LIST_AT, at), 0, "the MIX_AUXA list ends at its END");
  check_eq(get32(m, AUX_UP_AT + 4u * 5u), 999, "auxA left goes out as 32-bit samples");
  check_eq(get32(m, AUX_UP_AT + 4u * 165u), 0, "auxA right after it is silent");
  check_eq((int16_t)get16(m, STEREO_AT + 4u * 5u + 2u), 7, "and what comes back is added into main left");
  check_eq((int16_t)get16(m, STEREO_AT + 4u * 5u), 0, "not main right");
}

int main(void) {
  GcMachine *m = gc_machine_create();
  if (!m) return 1;

  test_stereo_order(m);
  test_adpcm(m);
  test_adpcm_values(m);
  test_walker_stays_in_step(m);
  test_running(m);
  test_setup_ramp(m);
  test_send_volume_is_unsigned(m);
  test_one_shot_end_is_silent(m);
  test_dpop_order(m);
  test_set_main_lr_is_32_bit(m);
  test_compressor(m);
  test_aux_bus(m);

  gc_machine_destroy(m);
  if (failures) {
    fprintf(stderr, "gc_ax_test: %u failures\n", failures);
    return 1;
  }
  printf("gc_ax_test: the command list, the voices and the decoder behave as documented\n");
  return 0;
}
