/*
 * The disc reader, on the host: the header, the filesystem, lookups by
 * path, and the state putting a disc in the drive leaves in low memory.
 *
 * The image is the one tools/gc/make_test_disc.py builds - a real GameCube
 * disc with three files instead of four thousand - and its path is the
 * first argument. CMake builds it at configure time, so this test needs no
 * devkitPPC and runs everywhere CI does.
 *
 * The other half of the drive, the registers and the interrupt, is tested
 * by tests/gc/programs/gc_disctest.c running libogc's own DVD driver. This
 * half is what that one cannot reach: the low-memory globals the apploader
 * leaves behind, which libogc overwrites for itself on the way up.
 */

#include <stdio.h>
#include <string.h>

#include "gc.h"
#include "gc_disc.h"

static int failures;
static const char *current;

#define CHECK(condition, ...)                                 \
  do {                                                        \
    if (!(condition)) {                                       \
      printf("FAIL [%s] %s:%d: ", current, __FILE__, __LINE__); \
      printf(__VA_ARGS__);                                    \
      printf("\n");                                           \
      failures++;                                             \
    }                                                         \
  } while (0)

#define CHECK_U32(got, want, what)                                 \
  do {                                                             \
    const uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);    \
    CHECK(g_ == w_, "%s = 0x%08x, expected 0x%08x", what, g_, w_); \
  } while (0)

/* The rule tools/gc/make_test_disc.py fills pattern.bin with. */
static uint8_t expected_byte(uint32_t i) { return (uint8_t)((i * 7u + 13u) & 0xffu); }

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void test_header(GcDisc *disc) {
  current = "the header";
  CHECK(strcmp(disc->game_code, "GCTE01") == 0, "the game code is %s", disc->game_code);
  CHECK(strcmp(disc->name, "native console disc test") == 0, "the game name is %s", disc->name);
  CHECK(gc_disc_fst_offset(disc) >= GC_DISC_HEADER_SIZE, "the filesystem is after the header");
  CHECK(disc->fst_size > 0 && disc->fst_size <= disc->fst_max_size,
        "the filesystem's size, %u, fits in its maximum, %u", disc->fst_size,
        disc->fst_max_size);
}

static void test_filesystem(GcDisc *disc) {
  uint32_t i, files = 0, directories = 0;
  current = "the filesystem";

  CHECK_U32(disc->entry_count, 5u, "the entry count");
  CHECK(disc->entries[0].is_directory, "the first entry is the root");
  for (i = 1; i < disc->entry_count; i++) {
    if (disc->entries[i].is_directory) {
      directories++;
    } else {
      files++;
    }
  }
  CHECK_U32(directories, 1u, "one directory");
  CHECK_U32(files, 3u, "three files");
}

static void test_lookup(GcDisc *disc) {
  const GcDiscEntry *entry;
  current = "looking a file up by path";

  entry = gc_disc_find(disc, "hello.txt");
  CHECK(entry != NULL, "hello.txt is there");
  if (entry) CHECK_U32(entry->size, 32u, "hello.txt's size");

  entry = gc_disc_find(disc, "data/pattern.bin");
  CHECK(entry != NULL, "data/pattern.bin is there");
  if (entry) CHECK_U32(entry->size, 2048u, "pattern.bin's size");

  entry = gc_disc_find(disc, "/data/second.bin");
  CHECK(entry != NULL, "a leading slash makes no difference");

  CHECK(gc_disc_find(disc, "pattern.bin") == NULL,
        "a file inside a directory is not at the top level");
  CHECK(gc_disc_find(disc, "data") == NULL, "a directory is not a file");
  CHECK(gc_disc_find(disc, "data/missing.bin") == NULL, "a name that is not there");
  CHECK(gc_disc_find(disc, "hello.txt/inner") == NULL, "a file is not a directory");
  CHECK(gc_disc_find(disc, "") == NULL, "the empty path");
}

static void test_reading(GcDisc *disc) {
  const GcDiscEntry *entry = gc_disc_find(disc, "data/pattern.bin");
  static uint8_t buffer[2048];
  uint32_t i, wrong = 0;

  current = "reading a file";
  if (!entry) return;

  CHECK(gc_disc_read(disc, gc_disc_entry_offset(disc, entry), buffer, entry->size) == 0,
        "pattern.bin reads");
  for (i = 0; i < entry->size; i++) {
    if (buffer[i] != expected_byte(i)) wrong++;
  }
  CHECK_U32(wrong, 0u, "the bytes that do not match the pattern");

  entry = gc_disc_find(disc, "hello.txt");
  if (entry) {
    CHECK(gc_disc_read(disc, gc_disc_entry_offset(disc, entry), buffer, entry->size) == 0,
          "hello.txt reads");
    CHECK(memcmp(buffer, "Hello from the disc.\n", 21) == 0, "hello.txt says what it says");
  }

  /* Past the end is a failure, not a short read filled with zeroes: a game
   * that asks for more than is there has a bug, and hiding it makes the bug
   * turn up somewhere else. */
  CHECK(gc_disc_read(disc, disc->size - 8, buffer, 64) != 0, "a read past the end fails");
  CHECK(gc_disc_read(disc, 0x40000000u, buffer, 32) != 0, "a read far past the end fails");
}

/* Putting a disc in the drive is also the apploader's job: the filesystem
 * goes into memory and the globals say where. */
static void test_attach(GcDisc *disc) {
  GcMachine *m = gc_machine_create();
  const uint8_t *low;
  uint8_t *fst;
  uint32_t address;

  current = "attaching a disc";
  if (!m) {
    CHECK(0, "the machine would not start");
    return;
  }
  m->log = NULL;
  gc_install_boot_state(m);
  CHECK(gc_attach_disc(m, disc) == 0, "the disc attaches");

  low = gc_machine_ram(m, 0, 0x100);
  CHECK(low != NULL, "low memory is there");
  if (!low) {
    gc_machine_destroy(m);
    return;
  }

  address = be32(low + 0x38);
  CHECK_U32(address, 0x81700000u, "0x80000038, where the filesystem went");
  CHECK_U32(be32(low + 0x3c), disc->fst_max_size, "0x8000003c, its maximum size");
  CHECK_U32(be32(low + 0x34), address, "the arena ends below the filesystem");
  CHECK(memcmp(low, "GCTE01", 6) == 0, "the disc id is in low memory");

  fst = gc_machine_ram(m, address & 0x03ffffffu, disc->fst_size);
  CHECK(fst != NULL, "the filesystem landed inside memory");
  if (fst) {
    CHECK(memcmp(fst, disc->fst, disc->fst_size) == 0,
          "and it is the table the disc holds");
  }

  gc_machine_destroy(m);
}

int main(int argc, char **argv) {
  GcDisc *disc;

  if (argc < 2) {
    printf("usage: gc_disc_test IMAGE.gcm\n");
    return 2;
  }
  disc = gc_disc_open(argv[1], stdout);
  if (!disc) {
    printf("FAIL: %s did not open as a disc\n", argv[1]);
    return 1;
  }

  test_header(disc);
  test_filesystem(disc);
  test_lookup(disc);
  test_reading(disc);
  test_attach(disc);

  gc_disc_close(disc);
  printf("gc_disc_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
