/*
 * The disc drive: the commands, the filesystem, and reading files off it.
 *
 * The third of the P2 programs (docs/GC-PLAN.md). It runs against the image
 * tools/gc/make_test_disc.py builds - a real GameCube disc, with the same
 * header, the same filesystem table and the same 32-byte alignment, holding
 * three files instead of four thousand.
 *
 * Everything goes through libogc's DVD driver rather than through the
 * registers directly, because the point is that a game's own driver works:
 * DVD_Init installs the interrupt handler, DVD_Mount spins the drive up,
 * and DVD_ReadPrio waits on the interrupt the drive raises. A machine that
 * models the registers but not the interrupt passes a register test and
 * hangs here.
 */

#include <string.h>

#include <gccore.h>
#include <ogc/dvd.h>

#include "gc_test_report.h"

#define ALIGN 32u

static dvdcmdblk block;
static dvddrvinfo drive ATTRIBUTE_ALIGN(32);
static u8 buffer[2048] ATTRIBUTE_ALIGN(32);

/* The filesystem table, read off the disc the way an apploader reads it:
 * the header says where it is and how big it is.
 *
 * A game finds it already in memory, at the address the apploader recorded
 * at 0x80000038 - which gc_run writes when it puts a disc in the drive.
 * This program cannot check that: libogc is a loader in its own right and
 * writes the low-memory globals for itself on the way up, so by the time
 * main runs the apploader's are gone. tests/gc/gc_disc_test.c checks them
 * instead, on the host, where nothing has overwritten them. */
static u8 fst_data[512] ATTRIBUTE_ALIGN(32);
static const u8 *fst;
static u32 fst_entries;
static const char *fst_strings;

static u32 be32(const u8 *p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 be24(const u8 *p) { return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; }

/* The same rule tools/gc/make_test_disc.py generates the pattern with. */
static u8 expected_byte(u32 i) { return (u8)((i * 7u + 13u) & 0xffu); }

/* ------------------------------------------------------------------ */

static void test_drive(void) {
  gc_test_begin("the drive answers");

  DVD_Init();
  GC_CHECK(DVD_Mount() >= 0, "the disc mounts");
  GC_CHECK_EQ(DVD_Inquiry(&block, &drive), 0x20u, "DVD_Inquiry transfers 32 bytes");
  GC_CHECK_EQ(drive.rev_level, 0x0002u, "the drive's revision level");
  GC_CHECK_EQ(drive.dev_code, 0x0001u, "the drive's device code");
  GC_CHECK_EQ(drive.rel_date, 0x20010608u, "the drive's release date");

  gc_test_end();
}

static void test_header(void) {
  gc_test_begin("the disc header");

  /* The whole header, because the offsets at 0x420 are what the test after
   * this one needs. */
  GC_CHECK(DVD_ReadPrio(&block, buffer, 0x440u, 0, 2) >= 0, "a read at the start of the disc");
  GC_CHECK_EQ(be32(buffer + 0x1c), 0xc2339f3du, "the disc magic at 0x1c");
  GC_CHECK(memcmp(buffer, "GCTE01", 6) == 0, "the game code");
  GC_CHECK(memcmp(buffer + 0x20, "native console disc test", 24) == 0, "the game name");

  gc_test_end();
}

static void test_filesystem(void) {
  u32 fst_offset, fst_size, i;
  u32 files = 0, directories = 0;

  gc_test_begin("the filesystem, read off the disc");

  /* The header was read into the buffer by the test before this one. */
  fst_offset = be32(buffer + 0x424);
  fst_size = be32(buffer + 0x428);
  GC_CHECK(fst_offset >= 0x440u, "the filesystem starts after the header");
  GC_CHECK(fst_size > 0u && fst_size <= sizeof(fst_data), "its size is one this can hold");
  if (fst_size == 0u || fst_size > sizeof(fst_data)) {
    gc_test_end();
    return;
  }

  GC_CHECK(DVD_ReadPrio(&block, fst_data, (fst_size + ALIGN - 1u) & ~(ALIGN - 1u),
                        (s64)fst_offset, 2) >= 0,
           "the filesystem reads");
  fst = fst_data;

  fst_entries = be32(fst + 8); /* the root entry's "next" is the total */
  fst_strings = (const char *)fst + fst_entries * 12u;
  GC_CHECK_EQ(fst_entries, 5u, "the number of entries: a root, a directory and three files");
  GC_CHECK_EQ(fst[0], 1u, "the first entry is a directory");

  for (i = 1; i < fst_entries; i++) {
    const u8 *entry = fst + i * 12u;
    if (entry[0]) {
      directories++;
    } else {
      files++;
    }
  }
  GC_CHECK_EQ(directories, 1u, "one directory");
  GC_CHECK_EQ(files, 3u, "three files");

  gc_test_end();
}

/* An entry by name, at the top level or inside the one directory. The real
 * DVDOpen walks parents to build a path; three files do not need that. */
static const u8 *entry_named(const char *name) {
  u32 i;
  if (!fst) return NULL;
  for (i = 1; i < fst_entries; i++) {
    const u8 *entry = fst + i * 12u;
    if (entry[0]) continue;
    if (strcmp(fst_strings + be24(entry + 1), name) == 0) return entry;
  }
  return NULL;
}

static void read_and_check(const char *name, u32 size, int is_pattern) {
  const u8 *entry = entry_named(name);
  u32 rounded, i;

  snprintf(gc_test_line, sizeof(gc_test_line), "%s is in the filesystem", name);
  gc_test_check(entry != NULL, gc_test_line);
  if (!entry) return;

  snprintf(gc_test_line, sizeof(gc_test_line), "%s is %u bytes, not %u", name,
           (unsigned)be32(entry + 8), (unsigned)size);
  gc_test_check(be32(entry + 8) == size, gc_test_line);

  rounded = (size + ALIGN - 1u) & ~(ALIGN - 1u);
  memset(buffer, 0, rounded);
  snprintf(gc_test_line, sizeof(gc_test_line), "%s reads", name);
  gc_test_check(DVD_ReadPrio(&block, buffer, rounded, (s64)be32(entry + 4), 2) >= 0,
                gc_test_line);

  if (is_pattern) {
    u32 wrong = 0;
    for (i = 0; i < size; i++) {
      if (buffer[i] != expected_byte(i)) wrong++;
    }
    snprintf(gc_test_line, sizeof(gc_test_line), "%s holds the pattern (%u bytes differ)", name,
             (unsigned)wrong);
    gc_test_check(wrong == 0, gc_test_line);
  }
}

static void test_files(void) {
  gc_test_begin("reading files off the disc");

  read_and_check("pattern.bin", 2048, 1);
  read_and_check("second.bin", 32, 0);
  {
    const u8 *entry = entry_named("second.bin");
    if (entry) {
      u32 i, wrong = 0;
      for (i = 0; i < 32; i++) {
        if (buffer[i] != 0xa5u) wrong++;
      }
      GC_CHECK_EQ(wrong, 0u, "second.bin is thirty-two bytes of 0xa5");
    }
  }
  read_and_check("hello.txt", 32, 0);
  GC_CHECK(memcmp(buffer, "Hello from the disc.\n", 21) == 0, "hello.txt says what it says");

  gc_test_end();
}

static void test_past_the_end(void) {
  gc_test_begin("a read past the end of the disc fails");

  /* Far past the end of a four-kilobyte image, and past the end of a real
   * disc too. The drive raises its error interrupt instead of completing,
   * and the driver turns that into a negative result. */
  GC_CHECK(DVD_ReadPrio(&block, buffer, ALIGN, (s64)0x30000000, 2) < 0,
           "a read at 0x30000000 does not succeed");

  gc_test_end();
}

/* ------------------------------------------------------------------ */

int main(void) {
  VIDEO_Init();

  test_drive();
  test_header();
  test_filesystem();
  test_files();
  test_past_the_end();

  gc_test_finish();
  return 0;
}
