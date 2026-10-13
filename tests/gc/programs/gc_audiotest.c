/*
 * The engine that plays sound out, and the interrupt that paces a game.
 *
 * There is nothing to hear in this program. What it checks is the clock:
 * a buffer is handed to the audio DMA, and the DMA is expected to walk
 * through it at the sample rate, loop back to the start, and raise an
 * interrupt each time it gets there. A game's audio thread does its work in
 * that interrupt, so the rate it arrives at is the rate the game's sound
 * runs at - and a machine that raises it at the wrong rate does not sound
 * wrong, it *is* wrong, in ways that show up as a game running fast or
 * waiting for a buffer that never fills.
 *
 * Three things are worth checking separately because they fail separately:
 *
 *  - The interrupt arrives at all. Without it a game reaches gameplay and
 *    its mixer starves, which looks like a game with no sound rather than
 *    like a missing interrupt.
 *
 *  - It arrives at the right rate. A buffer of 160 frames at 32 kHz is five
 *    milliseconds, so it should come round two hundred times a second. Half
 *    that, or twice, is what a wrong sample rate gives - and the rate bit is
 *    inverted in the register, so getting it backwards is easy.
 *
 *  - The remaining-bytes register falls. A game reads it to find out how far
 *    through the buffer the DMA is, and a register that sits at either end
 *    is one a game can wait on for ever.
 */

#include <string.h>

#include <gccore.h>

#include "gc_test_report.h"

/* 160 stereo frames - five milliseconds at 32 kHz - which is the size the
 * sound processor's own output buffer is, and therefore the size a game
 * hands over. Thirty-two byte alignment is the DMA's requirement. */
#define FRAMES 160
static u8 buffer[FRAMES * 4] ATTRIBUTE_ALIGN(32);

static volatile u32 callbacks;

static void dma_done(void) {
  callbacks++;
  /* What a game does here is hand over the next buffer. Handing over the
   * same one keeps the engine running without needing a mixer. */
  AUDIO_InitDMA((u32)buffer, sizeof(buffer));
}

/* How long a field is, as the machine counts. Waiting on the video retrace
 * is the only clock a test program has that does not depend on the thing
 * being measured. */
static void wait_fields(unsigned fields) {
  unsigned i;
  for (i = 0; i < fields; i++) VIDEO_WaitVSync();
}

static void test_interrupt_rate(void) {
  u32 before, after, seen;

  gc_test_begin("the audio DMA's interrupt, and how often it arrives");

  memset(buffer, 0, sizeof(buffer));
  AUDIO_Init(NULL);
  AUDIO_SetDSPSampleRate(AI_SAMPLERATE_32KHZ);
  AUDIO_RegisterDMACallback(dma_done);
  AUDIO_InitDMA((u32)buffer, sizeof(buffer));

  callbacks = 0;
  AUDIO_StartDMA();
  before = callbacks;
  wait_fields(30);
  after = callbacks;
  AUDIO_StopDMA();

  seen = after - before;
  /* Thirty fields is half a second, and five milliseconds a buffer, so a
   * hundred of them. The window is wide enough that the odd field boundary
   * landing early or late cannot matter and narrow enough that a factor of
   * two - which is what the wrong sample rate gives - cannot hide in it. */
  snprintf(gc_test_line, sizeof(gc_test_line),
           "about a hundred buffers in half a second: got %lu", (unsigned long)seen);
  gc_test_check(seen >= 85 && seen <= 115, gc_test_line);
  gc_test_end();
}

/* The engine stops when it is told to, and does not when it is not. A game
 * that pauses its sound and finds the interrupt still arriving has a mixer
 * running when it believed it was not. */
static void test_stop(void) {
  u32 after_stop;

  gc_test_begin("stopping it");
  AUDIO_StopDMA();
  wait_fields(2);
  after_stop = callbacks;
  wait_fields(10);
  snprintf(gc_test_line, sizeof(gc_test_line), "nothing arrives once it is stopped: got %lu more",
           (unsigned long)(callbacks - after_stop));
  gc_test_check(callbacks == after_stop, gc_test_line);
  gc_test_end();
}

/* How far through the buffer the engine is. This is a register a game polls,
 * so what matters is that it moves and stays inside the buffer - not any
 * particular value at any particular moment. */
static void test_bytes_left(void) {
  u32 first, second, in_range = 1, moved = 0;
  unsigned i;

  gc_test_begin("how far through the buffer it is");

  AUDIO_InitDMA((u32)buffer, sizeof(buffer));
  callbacks = 0;
  AUDIO_StartDMA();

  /* Long enough to be sure.
   *
   * One thirty-two byte block is a quarter of a millisecond, which is tens
   * of thousands of instructions - so a short poll loop reads the same value
   * every time and says the register is stuck when it is only being asked
   * faster than it changes. The budget below covers several blocks, and the
   * loop stops as soon as it has its answer. */
  first = second = AUDIO_GetDMABytesLeft();
  for (i = 0; i < 2000000u && !moved; i++) {
    second = AUDIO_GetDMABytesLeft();
    if (second > sizeof(buffer)) in_range = 0;
    if (second != first) moved = 1;
  }
  AUDIO_StopDMA();

  snprintf(gc_test_line, sizeof(gc_test_line), "it stays inside the buffer: last was %lu of %u",
           (unsigned long)second, (unsigned)sizeof(buffer));
  gc_test_check(in_range, gc_test_line);
  gc_test_check(moved, "and it moves");
  gc_test_end();
}

int main(void) {
  VIDEO_Init();
  /* A test that waits on the retrace needs the video interface actually
   * running, which the default mode gives without anything being displayed. */
  VIDEO_Configure(VIDEO_GetPreferredMode(NULL));
  VIDEO_SetBlack(TRUE);
  VIDEO_Flush();
  VIDEO_WaitVSync();

  test_interrupt_rate();
  test_stop();
  test_bytes_left();
  gc_test_finish();
  return 0;
}
