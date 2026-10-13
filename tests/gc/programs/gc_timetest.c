/*
 * Time: the timebase, the decrementer, the interrupt they drive, and the
 * video interface that paces everything a game does.
 *
 * The other half of the P2 gate (docs/GC-PLAN.md). A CPU test cannot see
 * any of this: it needs a machine with a clock running at a known rate and
 * an interrupt arriving on time.
 *
 * Two clocks are checked against each other rather than against a number
 * chosen here. The timebase runs at the bus clock over four - libogc's own
 * PPC_TIMER_CLOCK - and NTSC video runs at 60000/1001 fields a second,
 * which is the same 59.94 every NTSC device has used since colour. A
 * machine that gets either wrong runs a game at the wrong speed, and the
 * usual symptom is audio that drifts out of sync over a few minutes rather
 * than anything that looks broken.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "gc_test_report.h"

/* NTSC: 60000/1001 fields a second, so this many timebase ticks each. */
#define TICKS_PER_FIELD ((u64)PPC_TIMER_CLOCK * 1001ull / 60000ull)

static void *framebuffer;
static GXRModeObj *mode;

static volatile u32 alarm_count;
static volatile u64 alarm_first;
static volatile u64 alarm_last;

/* A relative difference, in parts per million, for the messages below. */
static s32 ppm(u64 got, u64 want) {
  const s64 difference = (s64)got - (s64)want;
  if (!want) return 0;
  return (s32)((difference * 1000000ll) / (s64)want);
}

static void check_within(u64 got, u64 want, s32 tolerance_ppm, const char *what) {
  const s32 off = ppm(got, want);
  snprintf(gc_test_line, sizeof(gc_test_line), "%s: %llu against %llu, off by %d ppm (allowed %d)",
           what, (unsigned long long)got, (unsigned long long)want, (int)off, (int)tolerance_ppm);
  gc_test_check(off <= tolerance_ppm && off >= -tolerance_ppm, gc_test_line);
}

/* ------------------------------------------------------------------ */

static void test_timebase(void) {
  u64 first, second;
  u32 spins;

  gc_test_begin("the timebase");

  first = gettime();
  second = gettime();
  GC_CHECK(second >= first, "it does not go backwards");

  /* It has to be running: a machine where nothing advances the timebase
   * looks fine until the first program that waits for a deadline. */
  for (spins = 0; spins < 1000000u && gettime() == first; spins++) {
  }
  GC_CHECK(gettime() > first, "it advances");

  /* The 64-bit read is a pair of 32-bit reads with a retry around the
   * carry, so it is worth watching across one. */
  {
    u32 i;
    u64 previous = gettime();
    int monotonic = 1;
    for (i = 0; i < 20000u; i++) {
      const u64 now = gettime();
      if (now < previous) monotonic = 0;
      previous = now;
    }
    GC_CHECK(monotonic, "twenty thousand reads in a row are monotonic");
  }

  /* mftb and mfspr 268 are the same register. */
  {
    u32 a = 0, b = 0;
    asm volatile("mftb %0" : "=r"(a));
    asm volatile("mfspr %0, 268" : "=r"(b));
    GC_CHECK(b >= a && (u32)(b - a) < 100000u, "mftb and mfspr 268 read one register");
  }

  gc_test_end();
}

static void test_decrementer(void) {
  u32 first, second;
  u64 tb_before, tb_after;
  u32 dec_before, dec_after;

  gc_test_begin("the decrementer runs at the timebase rate");

  asm volatile("mtdec %0" : : "r"(0x40000000u));
  asm volatile("mfdec %0" : "=r"(first));
  GC_CHECK(first <= 0x40000000u && first > 0x3ff00000u, "mtdec then mfdec");

  asm volatile("mfdec %0" : "=r"(second));
  GC_CHECK(second <= first, "it counts down");

  /* Over the same stretch of time the decrementer must fall by what the
   * timebase rises: they are the same 40.5 MHz. */
  tb_before = gettime();
  asm volatile("mfdec %0" : "=r"(dec_before));
  {
    u32 i;
    for (i = 0; i < 200000u; i++) asm volatile("" ::: "memory");
  }
  asm volatile("mfdec %0" : "=r"(dec_after));
  tb_after = gettime();

  {
    const u64 elapsed = tb_after - tb_before;
    const u64 counted = (u64)(dec_before - dec_after);
    snprintf(gc_test_line, sizeof(gc_test_line),
             "the timebase rose by %llu while the decrementer fell by %llu",
             (unsigned long long)elapsed, (unsigned long long)counted);
    /* The two reads bracket each other, so a handful of ticks of slack. */
    gc_test_check(counted + 64ull >= elapsed && elapsed + 64ull >= counted, gc_test_line);
  }

  /* Leave it where libogc expects it: its alarms own the decrementer, and
   * a huge value only delays the next one. */
  asm volatile("mtdec %0" : : "r"(0x7fffffffu));

  gc_test_end();
}

static void alarm_fired(syswd_t which, void *argument) {
  (void)which;
  (void)argument;
  if (!alarm_count) alarm_first = gettime();
  alarm_last = gettime();
  alarm_count++;
}

static void test_alarm(void) {
  syswd_t alarm;
  struct timespec start, period;
  u32 waited;

  gc_test_begin("a decrementer alarm arrives when it was asked for");

  alarm_count = 0;
  start.tv_sec = 0;
  start.tv_nsec = 20 * 1000 * 1000;  /* 20 ms */
  period.tv_sec = 0;
  period.tv_nsec = 10 * 1000 * 1000; /* then every 10 ms */

  GC_CHECK_EQ(SYS_CreateAlarm(&alarm), 0u, "SYS_CreateAlarm");
  GC_CHECK_EQ(SYS_SetPeriodicAlarm(alarm, &start, &period, alarm_fired, NULL), 0u,
              "SYS_SetPeriodicAlarm");

  /* Ten periods is 120 ms; give it a second of spinning before calling it
   * lost, so that a slow interpreter is not mistaken for a broken one. */
  for (waited = 0; waited < 200000000u && alarm_count < 11u; waited++) {
    asm volatile("" ::: "memory");
  }
  SYS_CancelAlarm(alarm);
  SYS_RemoveAlarm(alarm);

  GC_CHECK(alarm_count >= 11u, "eleven alarms arrived");
  if (alarm_count >= 11u) {
    /* Ten periods between the first and the eleventh. */
    check_within(alarm_last - alarm_first, millisecs_to_ticks(100), 20000,
                 "ten ten-millisecond periods");
  }

  gc_test_end();
}

static void test_video_rate(void) {
  u64 before, after;
  u32 field;
  const u32 fields = 300;

  gc_test_begin("the video interface runs at the NTSC field rate");

  /* Two fields to settle: the first wait after configuring the interface
   * returns as soon as the interrupt that was already pending arrives. */
  VIDEO_WaitVSync();
  VIDEO_WaitVSync();

  before = gettime();
  for (field = 0; field < fields; field++) VIDEO_WaitVSync();
  after = gettime();

  /* 500 ppm is twenty times what a crystal drifts and a fiftieth of the
   * gap between 60 Hz and 59.94, so this passes on a console and fails on
   * a machine that rounded the field rate to sixty. */
  check_within(after - before, TICKS_PER_FIELD * fields, 500, "three hundred fields");

  /* The line counter has to move within a field as well as between them:
   * code that waits for a particular line is code that waits forever
   * otherwise. */
  {
    u32 i;
    u32 seen = 0;
    u32 previous = VIDEO_GetCurrentLine();
    for (i = 0; i < 20000u; i++) {
      const u32 line = VIDEO_GetCurrentLine();
      if (line != previous) seen++;
      previous = line;
    }
    GC_CHECK(seen > 0u, "the current line advances within a field");
  }

  gc_test_end();
}

static void test_sleep(void) {
  u64 before, after;

  gc_test_begin("sleeping for a while takes about that long");

  before = gettime();
  usleep(20000); /* 20 ms, through the thread scheduler and its alarm */
  after = gettime();
  check_within(after - before, millisecs_to_ticks(20), 100000, "a 20 ms sleep");

  /* And a deadline loop, which is the shape of every wait libogc does
   * that is too short to sleep through. It has to end. */
  before = gettime();
  {
    const u64 deadline = before + millisecs_to_ticks(5);
    while (gettime() < deadline) {
    }
  }
  after = gettime();
  check_within(after - before, millisecs_to_ticks(5), 50000, "a 5 ms deadline loop");

  gc_test_end();
}

/* ------------------------------------------------------------------ */

/* Fields alternate.
 *
 * An interlaced mode draws the odd lines and then the even ones, and a
 * game that renders one field at a time asks which one is coming next
 * before it draws. VIDEO_GetNextField answers from the beam position
 * register, so a beam that restarts its count every field makes the answer
 * a constant - and a constant is not obviously wrong: the retrace still
 * arrives at the right rate, the frame counter still advances, and nothing
 * else complains. Only a game that actually renders fields notices, and by
 * then it is drawing every frame into the same half of the screen.
 *
 * This looks at the answer across eight consecutive fields and asks for
 * both values to appear. */
static void test_fields_alternate(void) {
  u32 seen_first = 0, seen_second = 0;
  unsigned i;

  gc_test_begin("the two fields alternate");

  for (i = 0; i < 8u; i++) {
    VIDEO_WaitVSync();
    if (VIDEO_GetNextField() == VI_FIELD_BELOW) seen_second++;
    else seen_first++;
  }

  snprintf(gc_test_line, sizeof(gc_test_line),
           "over eight fields, both come up: %lu and %lu", (unsigned long)seen_first,
           (unsigned long)seen_second);
  gc_test_check(seen_first > 0u && seen_second > 0u, gc_test_line);

  /* And they take turns rather than arriving in a clump. */
  {
    u32 changes = 0, previous = VIDEO_GetNextField();
    for (i = 0; i < 8u; i++) {
      u32 now;
      VIDEO_WaitVSync();
      now = VIDEO_GetNextField();
      if (now != previous) changes++;
      previous = now;
    }
    snprintf(gc_test_line, sizeof(gc_test_line), "and swap every field: %lu changes in eight",
             (unsigned long)changes);
    gc_test_check(changes >= 7u, gc_test_line);
  }
  gc_test_end();
}

/* A display interrupt fires where it was aimed.
 *
 * The four display interrupt registers are position matches: each names a
 * line and a column, both counting from one, and fires when the beam
 * reaches them. The SDK aims two of them at the top of the frame and the
 * top of the second field and uses the pair as its retrace, which means a
 * machine that simply raises every armed interrupt at each field boundary
 * still produces exactly the right retrace rate. Everything above would
 * pass. What such a machine gets wrong is the other two, which exist for
 * raster effects: a game aiming one at the middle of the screen gets it at
 * the top, and whatever register it wanted to change mid-frame changes
 * about two hundred and forty lines early.
 *
 * So this aims one at a line in the middle and asks where the beam is when
 * it arrives. The SDK has no interface for these - its position callback
 * is a file-static with nothing to set it - so the registers are written
 * directly, and interrupts are held off while the status bit is polled,
 * because the SDK's own retrace handler acknowledges whichever interrupts
 * it finds pending and would clear this one before the loop saw it. */
#define VI_DISPLAY_INTERRUPT_2 ((vu32 *)0xcc002038)

static void test_raster_interrupt(void) {
  const u32 aimed_at = 200u; /* a line, counting from one */
  u32 level, line = 0, spins = 0;
  int arrived = 0;

  gc_test_begin("a display interrupt arrives where it was aimed");

  level = IRQ_Disable();
  /* Enable, vertical position 200, horizontal position 1. */
  *VI_DISPLAY_INTERRUPT_2 = (1u << 28) | (aimed_at << 16) | 1u;
  /* Bounded, so a machine that never raises it fails rather than hangs. */
  while (spins++ < 20000000u) {
    if (*VI_DISPLAY_INTERRUPT_2 & 0x80000000u) {
      line = VIDEO_GetCurrentLine();
      arrived = 1;
      break;
    }
  }
  /* Disarm and acknowledge before letting the SDK have its interrupts
   * back, so nothing is left pending on its behalf. */
  *VI_DISPLAY_INTERRUPT_2 = 0;
  IRQ_Restore(level);

  gc_test_check(arrived, "it arrives at all");
  if (arrived) {
    /* The register counts lines of the whole frame from one and
     * VIDEO_GetCurrentLine counts lines of the field from zero, so a match
     * at 200 is read back as 199. A few lines of slack for the polling
     * loop; two hundred would be the wrong answer and so would zero. */
    snprintf(gc_test_line, sizeof(gc_test_line),
             "and at line 199, not at the top of the field: got %lu", (unsigned long)line);
    gc_test_check(line >= 195u && line <= 215u, gc_test_line);
  }
  gc_test_end();
}

int main(void) {
  VIDEO_Init();
  mode = VIDEO_GetPreferredMode(NULL);
  framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
  VIDEO_ClearFrameBuffer(mode, framebuffer, COLOR_BLACK);
  VIDEO_Configure(mode);
  VIDEO_SetNextFramebuffer(framebuffer);
  VIDEO_SetBlack(FALSE);
  VIDEO_Flush();

  test_timebase();
  test_decrementer();
  test_alarm();
  test_video_rate();
  test_fields_alternate();
  test_raster_interrupt();
  test_sleep();

  gc_test_finish();
  return 0;
}
