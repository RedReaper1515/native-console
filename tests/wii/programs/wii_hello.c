/*
 * The smallest Wii program that proves the machine is one: it gets through
 * libogc's start-up (which opens IOS's devices and waits for their answers),
 * finds MEM2, reads the globals a Wii leaves, and talks to IOS itself.
 *
 * Verdicts are printed in the GameCube programs' format
 * (tests/gc/programs/gc_test_report.h), so one runner reads both.
 */
#include <gccore.h>
#include <ogc/ipc.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <stdlib.h>
#include <string.h>

#include "gc_test_report.h"

static char line[256];

/* The globals a game reads to find out what it is running on. */
static void test_globals(void) {
  gc_test_begin("what the machine says it is");
  GC_CHECK_EQ(*(u32 *)0x800000f8, 243000000u, "the bus clock");
  GC_CHECK_EQ(*(u32 *)0x800000fc, 729000000u, "the core clock");
  GC_CHECK_EQ(*(u32 *)0x80000028, 0x01800000u, "MEM1's size");
  GC_CHECK_EQ(*(u32 *)0x80003118, 0x04000000u, "MEM2's size");
  GC_CHECK_EQ(mfpvr(), 0x00087102u, "the processor version (Broadway)");
  snprintf(line, sizeof(line), "an IOS is running (version word %08x)", (unsigned)*(u32 *)0x80003140);
  GC_CHECK((*(u32 *)0x80003140 >> 16) != 0, line);
  gc_test_end();
}

/* A word of MEM2 through the cached view and back through the uncached
 * one: the same bytes, and not MEM1's. */
static void test_mem2(void) {
  volatile u32 *cached = (volatile u32 *)0x90100000;
  volatile u32 *uncached = (volatile u32 *)0xd0100000;
  volatile u32 *mem1 = (volatile u32 *)0x80100000;
  const u32 before = *mem1;
  u8 *block;

  gc_test_begin("MEM2");
  *cached = 0x12345678;
  DCFlushRange((void *)cached, 32);
  GC_CHECK_EQ(*uncached, 0x12345678u, "a word written at 0x90100000, read at 0xd0100000");
  *uncached = 0xcafef00d;
  DCFlushRange((void *)cached, 32);
  GC_CHECK_EQ(*cached, 0xcafef00du, "and written at 0xd0100000, read at 0x90100000");
  GC_CHECK_EQ(*mem1, before, "MEM1 at the same offset is untouched");

  snprintf(line, sizeof(line), "the MEM2 arena has %u bytes", (unsigned)SYS_GetArena2Size());
  GC_CHECK(SYS_GetArena2Size() > 0x02000000u, line);

  /* The very last bytes of each block, where an off-by-one in the map
   * shows. */
  block = (u8 *)0x817fffff;
  *block = 0x5a;
  GC_CHECK_EQ(*block, 0x5au, "the last byte of MEM1");
  block = (u8 *)0x933dffff;
  *block = 0xa5;
  GC_CHECK_EQ(*block, 0xa5u, "the last byte of MEM2 a game may use");
  gc_test_end();
}

/* IOS: a device that is there opens and closes, one that is not says so,
 * and a request is answered with the right shape. */
static void test_ios(void) {
  static u64 title ATTRIBUTE_ALIGN(32);
  static ioctlv vector[1] ATTRIBUTE_ALIGN(32);
  static char directory[32] ATTRIBUTE_ALIGN(32);
  static ioctlv pair[2] ATTRIBUTE_ALIGN(32);
  s32 fd, again, result;

  gc_test_begin("IOS answers");
  fd = IOS_Open("/dev/es", 0);
  snprintf(line, sizeof(line), "/dev/es opens (%d)", (int)fd);
  GC_CHECK(fd >= 0, line);
  again = IOS_Open("/dev/es", 0);
  snprintf(line, sizeof(line), "and again as another descriptor (%d, %d)", (int)fd, (int)again);
  GC_CHECK(again >= 0 && again != fd, line);

  title = 0;
  vector[0].data = &title;
  vector[0].len = sizeof(title);
  result = IOS_Ioctlv(fd, 0x20, 0, 1, vector);
  GC_CHECK_EQ(result, 0, "ES's title id request");
  GC_CHECK_EQ(title >> 32, 0x00010000u, "a disc title's upper half");

  /* Where the title's saves go: /title/00010000/xxxxxxxx/data. */
  memset(directory, 0, sizeof(directory));
  pair[0].data = &title;
  pair[0].len = sizeof(title);
  pair[1].data = directory;
  pair[1].len = 30;
  result = IOS_Ioctlv(fd, 0x1d, 1, 1, pair);
  GC_CHECK_EQ(result, 0, "ES's data directory request");
  snprintf(line, sizeof(line), "the data directory is \"%s\"", directory);
  GC_CHECK(strncmp(directory, "/title/00010000/", 16) == 0 && strcmp(directory + 24, "/data") == 0, line);

  GC_CHECK_EQ(IOS_Close(again), 0, "a descriptor closes");
  GC_CHECK_EQ(IOS_Close(fd), 0, "and the other");
  GC_CHECK(IOS_Close(fd) < 0, "a closed descriptor does not close twice");
  GC_CHECK_EQ(IOS_Open("/dev/no/such/device", 0), -6, "a device that does not exist");
  gc_test_end();
}

/* Time: the timebase against the retrace. A field is 1/59.94 of a second
 * whatever the processor's speed, so sixty of them are a second of
 * timebase, give or take a field. */
static void test_time(void) {
  u64 start;
  u32 elapsed;
  int i;
  gc_test_begin("the clocks agree");
  VIDEO_WaitVSync();
  start = gettime();
  for (i = 0; i < 60; i++) VIDEO_WaitVSync();
  elapsed = (u32)diff_usec(start, gettime());
  snprintf(line, sizeof(line), "sixty fields take %u microseconds by the timebase", (unsigned)elapsed);
  GC_CHECK(elapsed > 980000u && elapsed < 1020000u, line);
  gc_test_end();
}

int main(void) {
  /* An ordinary interlaced NTSC picture, said outright: what a retrace is
   * counted in depends on the mode, and a mode left to the console's
   * settings would make the clock test below about those instead. */
  GXRModeObj *mode = &TVNtsc480IntDf;
  void *frame;
  VIDEO_Init();
  frame = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
  VIDEO_Configure(mode);
  VIDEO_SetNextFramebuffer(frame);
  VIDEO_SetBlack(TRUE);
  VIDEO_Flush();
  VIDEO_WaitVSync();
  VIDEO_WaitVSync();
  test_globals();
  test_mem2();
  test_ios();
  test_time();
  gc_test_finish();
  return 0;
}
