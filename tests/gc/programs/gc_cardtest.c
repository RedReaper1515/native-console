/*
 * The memory card, through the library a game would actually use.
 *
 * There is almost nothing to be gained from testing the card's commands
 * directly: a game never issues them, and an implementation that answers
 * every command correctly can still be unusable if the image underneath it
 * is not one the save library will mount. So this drives the library - probe,
 * mount, create, write, read back - and what it really tests is the two
 * layers together.
 *
 * The interesting failures all look like something else:
 *
 *  - A status byte returned in the wrong byte of the register reads as a
 *    locked card, and the library answers by uploading a decryption
 *    microcode to the sound processor. The symptom is a mount that takes a
 *    very long time and then fails.
 *
 *  - A write that never raises the card's interrupt looks like a write that
 *    worked, until a watchdog fires a tenth of a second later. A test that
 *    only creates a file and stops would pass.
 *
 *  - A formatted image with a wrong checksum, or a free-block count that
 *    disagrees with its own table, fails at the *mount*, so it is read as a
 *    broken card rather than as a bad format.
 *
 * Each of those is why the checks below go as far as reading data back
 * rather than stopping at "the call returned zero".
 */

#include <string.h>

#include <gccore.h>

#include "gc_test_report.h"

/* The library wants its work area 32-byte aligned, and it is five blocks
 * of eight kilobytes whatever the card reports. */
static u8 work_area[CARD_WORKAREA_SIZE] ATTRIBUTE_ALIGN(32);

/* One sector's worth: the library refuses any write whose length or offset
 * is not a multiple of the card's sector size, so a save is written a whole
 * block at a time whether or not it has that much to say. */
static u8 payload[8192] ATTRIBUTE_ALIGN(32);
static u8 readback[8192] ATTRIBUTE_ALIGN(32);

#define TEST_FILE "gc_cardtest"

static void test_present(void) {
  s32 memsize = 0, sectsize = 0;
  s32 result;

  gc_test_begin("a card in the first slot");

  CARD_Init("TEST", "00");
  /* The first probe after boot legitimately says no: the bus debounces a
   * newly seen device for about thirty microseconds and reports nothing
   * until that has passed. Software is expected to ask again, and a test
   * that asks once would be testing the debounce. */
  result = CARD_ProbeEx(CARD_SLOTA, &memsize, &sectsize);
  while (result == CARD_ERROR_BUSY) {
    result = CARD_ProbeEx(CARD_SLOTA, &memsize, &sectsize);
  }

  snprintf(gc_test_line, sizeof(gc_test_line), "the slot answers: got %ld", (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);
  /* Megabits, not blocks - the library reports the identifier's size field
   * as it stands. Four megabits is sixty-four blocks of eight kilobytes, of
   * which fifty-nine are a game's. */
  snprintf(gc_test_line, sizeof(gc_test_line), "and it is a 4 Mbit card: got %ld", (long)memsize);
  gc_test_check(memsize == 4, gc_test_line);
  snprintf(gc_test_line, sizeof(gc_test_line), "with 8 KB sectors: got %ld", (long)sectsize);
  gc_test_check(sectsize == 8192, gc_test_line);
  gc_test_end();
}

/* Mounting is where a badly formatted image fails, and it fails with an
 * error about the card rather than about the format. Getting here means the
 * header, both directory copies and both allocation tables all checksummed
 * correctly and the free-block count agreed with its own table. */
static void test_mount(void) {
  s32 result;

  gc_test_begin("mounting it");
  result = CARD_Mount(CARD_SLOTA, work_area, NULL);
  snprintf(gc_test_line, sizeof(gc_test_line), "the card mounts: got %ld", (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);
  gc_test_end();
}

/* What an earlier run left behind.
 *
 * This is the check that the image on disk is a memory card and not just
 * something this machine can read: it says nothing about the run it is in,
 * and everything about the run before it. On a card that has just been
 * formatted there is nothing to find, which is not a failure - but it is
 * reported in so many words, because a check that passes by finding nothing
 * is a check that would also pass if the card were never written at all.
 *
 * tests/gc/gc_card_restart.py runs this program twice against one image and
 * insists on seeing the two different answers in the right order. */
static void test_earlier_run(void) {
  card_file file;
  s32 result;
  unsigned i;
  int same = 1;

  gc_test_begin("what an earlier run left on it");

  for (i = 0; i < sizeof(payload); i++) payload[i] = (u8)(i * 7u + (i >> 8));
  memset(readback, 0, sizeof(readback));

  /* Both answers are printed rather than only the failing one, because the
   * checking script reads them: the verdict counts say whether this run went
   * well, and these two lines say which of the two situations it was in. */
  result = CARD_Open(CARD_SLOTA, TEST_FILE, &file);
  if (result != CARD_ERROR_READY) {
    OSReport("  there was no file from an earlier run\n");
    gc_test_check(1, "an empty card is an answer, not a failure");
    gc_test_end();
    return;
  }

  result = CARD_Read(&file, readback, sizeof(readback), 0);
  CARD_Close(&file);
  snprintf(gc_test_line, sizeof(gc_test_line), "the earlier file reads back: got %ld",
           (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);

  for (i = 0; i < sizeof(payload); i++) {
    if (readback[i] != payload[i]) {
      snprintf(gc_test_line, sizeof(gc_test_line), "byte %u is %02x, wanted %02x", i, readback[i],
               payload[i]);
      same = 0;
      break;
    }
  }
  if (same) OSReport("  the file from an earlier run is intact\n");
  gc_test_check(same, gc_test_line);
  gc_test_end();
}

/* Create, write, close, reopen, read. The reopen is the point: a write that
 * only reached the library's copy of the directory would pass everything up
 * to it. */
static void test_write_and_read(void) {
  card_file file;
  s32 result;
  unsigned i;
  int same = 1;

  gc_test_begin("writing a file and reading it back");

  for (i = 0; i < sizeof(payload); i++) payload[i] = (u8)(i * 7u + (i >> 8));
  memset(readback, 0, sizeof(readback));

  /* A file that is already there from an earlier run is deleted first, so
   * the test says the same thing on a fresh card and on one it has already
   * written to. */
  CARD_Delete(CARD_SLOTA, TEST_FILE);

  result = CARD_Create(CARD_SLOTA, TEST_FILE, sizeof(payload), &file);
  snprintf(gc_test_line, sizeof(gc_test_line), "the file is created: got %ld", (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);
  if (result != CARD_ERROR_READY) {
    gc_test_end();
    return;
  }

  result = CARD_Write(&file, payload, sizeof(payload), 0);
  snprintf(gc_test_line, sizeof(gc_test_line), "and written: got %ld", (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);
  CARD_Close(&file);

  /* Put the card away and pick it up again.
   *
   * Without this the test would pass against a card that never wrote
   * anything: the library keeps the directory and the allocation table in
   * its own work area and answers out of them, so a file it created and
   * wrote is findable and readable whether or not a single byte reached the
   * chip. Unmounting throws that away, and mounting reads all five system
   * blocks back off the card - so everything after this point is the card's
   * own account of what happened. */
  CARD_Unmount(CARD_SLOTA);
  result = CARD_Mount(CARD_SLOTA, work_area, NULL);
  snprintf(gc_test_line, sizeof(gc_test_line), "the card mounts again: got %ld", (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);
  if (result != CARD_ERROR_READY) {
    gc_test_end();
    return;
  }

  result = CARD_Open(CARD_SLOTA, TEST_FILE, &file);
  snprintf(gc_test_line, sizeof(gc_test_line), "the file is still there: got %ld", (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);
  if (result != CARD_ERROR_READY) {
    gc_test_end();
    return;
  }

  result = CARD_Read(&file, readback, sizeof(readback), 0);
  snprintf(gc_test_line, sizeof(gc_test_line), "and reads back: got %ld", (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);
  CARD_Close(&file);

  for (i = 0; i < sizeof(payload); i++) {
    if (readback[i] != payload[i]) {
      snprintf(gc_test_line, sizeof(gc_test_line), "byte %u is %02x, wanted %02x", i, readback[i],
               payload[i]);
      same = 0;
      break;
    }
  }
  if (same) snprintf(gc_test_line, sizeof(gc_test_line), "every byte came back");
  gc_test_check(same, gc_test_line);
  gc_test_end();
}

/* How much room is left, and whether the file just written accounts for it.
 * A card whose allocation table is written but not read back correctly
 * passes everything above and gets this wrong. */
static void test_free_space(void) {
  u16 free_blocks = 0;
  s32 result;

  gc_test_begin("what is left on it");
  result = CARD_GetFreeBlocks(CARD_SLOTA, &free_blocks);
  snprintf(gc_test_line, sizeof(gc_test_line), "the card reports its free space: got %ld",
           (long)result);
  gc_test_check(result == CARD_ERROR_READY, gc_test_line);

  /* Fifty-nine usable blocks, one of them now holding the file above. */
  snprintf(gc_test_line, sizeof(gc_test_line), "58 blocks are free: got %u", free_blocks);
  gc_test_check(free_blocks == 58u, gc_test_line);
  gc_test_end();
}

int main(void) {
  VIDEO_Init();
  test_present();
  test_mount();
  test_earlier_run();
  test_write_and_read();
  test_free_space();
  CARD_Unmount(CARD_SLOTA);
  gc_test_finish();
  return 0;
}
