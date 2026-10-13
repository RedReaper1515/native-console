/*
 * The disc reader on Wii discs.
 *
 * The discs are the ones tests/wii/wii_test_disc.c builds - a real Wii
 * disc's structure around two made-up keys - and this builds them itself,
 * into the directory named by its first argument, in every container the
 * reader takes: a plain image, a CISO and a WBFS, and the last two again
 * with the game partition past four gigabytes.
 *
 * What is held to account here is everything gc_disc.h promises about a
 * Wii disc: that it opens locked and says so once, that only the right
 * key opens it, from each place a key can come from, that what is read
 * afterwards is the partition's decrypted contents wherever a read starts
 * and however many clusters it crosses, and that the formats it does not
 * read are turned away by name.
 *
 * It says nothing about retail discs. There are none here.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "gc_disc.h"
#include "wii_crypto.h"
#include "wii_test_disc.h"

static int failures;
static const char *current;
static const char *image; /* which disc the failing check was looking at */

#define CHECK(condition, ...)                                              \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL [%s, %s] %s:%d: ", current, image, __FILE__, __LINE__); \
      printf(__VA_ARGS__);                                                 \
      printf("\n");                                                        \
      failures++;                                                          \
    }                                                                      \
  } while (0)

#define CHECK_U64(got, want, what)                                              \
  do {                                                                          \
    const unsigned long long g_ = (unsigned long long)(got);                    \
    const unsigned long long w_ = (unsigned long long)(want);                   \
    CHECK(g_ == w_, "%s = 0x%llx, expected 0x%llx", what, g_, w_);              \
  } while (0)

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------------ */
/* Files, the environment and the log                                  */

static char work[900];

/* A path inside the working directory. Good until four more are asked
 * for. */
static const char *in_work(const char *name) {
  static char paths[4][1024];
  static unsigned next;
  char *path = paths[next++ & 3u];
  snprintf(path, sizeof(paths[0]), "%s/%s", work, name);
  return path;
}

static void make_directory(const char *path) {
#if defined(_WIN32)
  _mkdir(path);
#else
  mkdir(path, 0777);
#endif
}

static void write_file(const char *path, const void *bytes, size_t size) {
  FILE *file = fopen(path, "wb");
  if (!file || fwrite(bytes, 1, size, file) != size) {
    printf("FAIL: cannot write %s\n", path);
    failures++;
  }
  if (file) fclose(file);
}

/* An environment variable set, or with NULL taken away. */
static void set_environment(const char *name, const char *value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value : "");
#else
  if (value) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
#endif
}

/* The machine this runs on may well have a key set up, and a test that
 * passed or failed depending on that would be worse than no test. */
static void no_keys_in_environment(void) {
  set_environment("WII_COMMON_KEY", NULL);
  set_environment("WII_KEY_FILE", NULL);
}

/* The reader's log is a FILE, so the test's is a file: written by the
 * reader, read back here. */
static FILE *log_open(void) {
  FILE *log = fopen(in_work("log.txt"), "w+");
  if (!log) {
    printf("FAIL: cannot create a log in %s\n", work);
    exit(2);
  }
  return log;
}

static const char *log_text(FILE *log) {
  static char text[16384];
  size_t got;
  fflush(log);
  fseek(log, 0, SEEK_SET);
  got = fread(text, 1, sizeof(text) - 1, log);
  text[got] = 0;
  fseek(log, 0, SEEK_END);
  return text;
}

static unsigned count_of(const char *text, const char *needle) {
  unsigned count = 0;
  for (text = strstr(text, needle); text; text = strstr(text + 1, needle)) count++;
  return count;
}

static void key_as_hex(const char *key, char *out, int upper, int spaced) {
  unsigned i;
  for (i = 0; i < 16; i++) {
    out += sprintf(out, upper ? "%02X" : "%02x", (unsigned)(uint8_t)key[i]);
    if (spaced && (i & 3u) == 3u) out += sprintf(out, i == 7 ? "\r\n" : " \t");
  }
}

/* ------------------------------------------------------------------ */
/* What an open disc should hold                                       */

static const uint8_t *common_key(void) { return (const uint8_t *)WII_TEST_COMMON_KEY; }
static const uint8_t *title_key(void) { return (const uint8_t *)WII_TEST_TITLE_KEY; }
static const uint8_t wrong_key[16] = {'n', 'o', 't', ' ', 't', 'h', 'e', ' ',
                                      'r', 'i', 'g', 'h', 't', ' ', 'k', 'y'};

static void check_layout(GcDisc *disc) {
  current = "the header";
  CHECK(disc->is_wii == 1, "it is a Wii disc");
  CHECK(disc->offset_shift == 2u, "its offsets are stored shifted by two (%u)", disc->offset_shift);
  CHECK(!gc_disc_is_locked(disc), "it is open");
  CHECK(strcmp(disc->game_code, WII_TEST_GAME_CODE) == 0, "the game code is %s", disc->game_code);
  CHECK(strcmp(disc->name, WII_TEST_GAME_NAME) == 0, "the game name is %s", disc->name);
  CHECK_U64(disc->dol_offset, WII_TEST_DOL_AT >> 2, "the stored DOL offset");
  CHECK_U64(gc_disc_dol_offset(disc), WII_TEST_DOL_AT, "the DOL's byte offset");
  CHECK_U64(disc->fst_offset, WII_TEST_FST_AT >> 2, "the stored filesystem offset");
  CHECK_U64(gc_disc_fst_offset(disc), WII_TEST_FST_AT, "the filesystem's byte offset");
  /* The sizes are in bytes whatever the disc: six entries and their
   * names, rounded up to the four the stored form can express. */
  CHECK_U64(disc->fst_size, 120u, "the filesystem's size in bytes");
  CHECK_U64(disc->fst_max_size, 120u, "the filesystem's maximum size in bytes");
  CHECK_U64(gc_disc_data_size(disc), WII_TEST_DATA_SIZE, "the decrypted data size");

  current = "the filesystem";
  CHECK_U64(disc->entry_count, 6u, "the entry count");
  if (disc->entry_count != 6u) return;
  CHECK(disc->entries[0].is_directory && disc->entries[1].is_directory, "the root and data/");
  CHECK(strcmp(disc->entries[1].name, "data") == 0, "the directory is called %s", disc->entries[1].name);
  CHECK_U64(gc_disc_entry_offset(disc, &disc->entries[1]), 0u, "a directory has no offset");
  CHECK(disc->fst && memcmp(disc->fst, wii_test_disc_plain() + WII_TEST_FST_AT, disc->fst_size) == 0,
        "the table kept is the table on the disc");
}

/* A file by name, through the filesystem, against the rule that filled
 * it. */
static void check_file(GcDisc *disc, const char *path, uint64_t at, uint32_t size,
                       uint8_t (*rule)(uint32_t)) {
  const GcDiscEntry *entry = gc_disc_find(disc, path);
  uint8_t *bytes;
  uint32_t i, wrong = 0;

  CHECK(entry != NULL, "%s is there", path);
  if (!entry) return;
  CHECK_U64(entry->size, size, "its size");
  CHECK_U64(entry->offset, at >> 2, "its stored offset");
  CHECK_U64(gc_disc_entry_offset(disc, entry), at, "its byte offset");
  bytes = (uint8_t *)malloc(size);
  if (!bytes) return;
  CHECK(gc_disc_read(disc, gc_disc_entry_offset(disc, entry), bytes, entry->size) == 0, "%s reads", path);
  for (i = 0; i < size; i++) {
    if (bytes[i] != (rule ? rule(i) : wii_test_disc_plain()[at + i])) wrong++;
  }
  CHECK(wrong == 0, "%u bytes of %s are not what was written", wrong, path);
  free(bytes);
}

static void check_files(GcDisc *disc) {
  static uint8_t buffer[0x1000];
  void *dol = NULL;
  uint32_t dol_size = 0, i, wrong = 0;

  current = "files by name";
  check_file(disc, "hello.txt", WII_TEST_HELLO_AT, WII_TEST_HELLO_SIZE, NULL);
  check_file(disc, "data/second.bin", WII_TEST_SECOND_AT, WII_TEST_SECOND_SIZE, NULL);
  /* 0x7000 to 0x10000: the end of the first cluster, all of the second,
   * the start of the third. */
  check_file(disc, "/data/pattern.bin", WII_TEST_PATTERN_AT, WII_TEST_PATTERN_SIZE, wii_test_pattern_byte);
  /* Twenty clusters, where sixteen are kept. */
  check_file(disc, "big.bin", WII_TEST_BIG_AT, WII_TEST_BIG_SIZE, wii_test_big_byte);
  CHECK(gc_disc_find(disc, "pattern.bin") == NULL, "a file inside a directory is not at the top level");
  CHECK(gc_disc_find(disc, "data/missing.bin") == NULL, "a name that is not there");
  if (gc_disc_read(disc, WII_TEST_HELLO_AT, buffer, 32) == 0) {
    CHECK(memcmp(buffer, WII_TEST_HELLO, sizeof(WII_TEST_HELLO) - 1u) == 0, "hello.txt says what it says");
  }

  current = "a read across a cluster boundary";
  /* Sixteen bytes, eight either side of the first boundary. */
  CHECK(gc_disc_read(disc, 0x7c00u - 8u, buffer, 16) == 0, "it reads");
  CHECK(memcmp(buffer, wii_test_disc_plain() + 0x7c00u - 8u, 16) == 0, "and both halves are right");

  current = "the apploader";
  /* What gc_run_apploader does: the header at 0x2440, the code after. */
  CHECK(gc_disc_read(disc, 0x2440u, buffer, 0x20) == 0, "its header reads");
  CHECK_U64(be32(buffer + 0x10), WII_TEST_APPLOADER_ENTRY, "its entry point");
  CHECK_U64(be32(buffer + 0x14), WII_TEST_APPLOADER_SIZE, "its size");
  CHECK(gc_disc_read(disc, 0x2460u, buffer, WII_TEST_APPLOADER_SIZE) == 0, "its code reads");
  for (i = 0; i < WII_TEST_APPLOADER_SIZE; i++) {
    if (buffer[i] != wii_test_apploader_byte(i)) wrong++;
  }
  CHECK(wrong == 0, "%u bytes of the apploader are wrong", wrong);

  current = "the DOL";
  CHECK(gc_disc_read_dol(disc, &dol, &dol_size) == 0, "it reads");
  CHECK_U64(dol_size, WII_TEST_DOL_SIZE, "its length, worked out from its sections");
  if (dol && dol_size == WII_TEST_DOL_SIZE) {
    CHECK(memcmp(dol, wii_test_disc_plain() + WII_TEST_DOL_AT, dol_size) == 0, "its bytes");
  }
  free(dol);

  current = "reads that should fail";
  CHECK(gc_disc_read(disc, WII_TEST_DATA_SIZE - 8u, buffer, 8) == 0, "the last eight bytes read");
  CHECK(gc_disc_read(disc, WII_TEST_DATA_SIZE - 8u, buffer, 64) != 0, "a read past the end fails");
  CHECK(gc_disc_read(disc, WII_TEST_DATA_SIZE, buffer, 1) != 0, "a read at the end fails");
  CHECK(gc_disc_read(disc, 0xfffffffffffffff0ull, buffer, 64) != 0,
        "an offset that wraps round sixty-four bits fails");
}

/* The disc underneath: the parts that are not encrypted, and the parts
 * that are, as they are stored. */
static void check_raw(GcDisc *disc, uint64_t partition, int clusters, int encrypted) {
  static uint8_t buffer[0x8000];
  const uint8_t *blob;
  uint32_t size = 0, i, wrong = 0;
  uint64_t data;

  current = "raw reads";
  CHECK_U64(gc_disc_partition_offset(disc), partition, "the game partition's offset");
  CHECK(gc_disc_read_raw(disc, 0, buffer, 0x100) == 0, "the disc header reads");
  CHECK(memcmp(buffer, WII_TEST_GAME_CODE, 6) == 0, "the id at the front of the disc");
  CHECK_U64(be32(buffer + 0x18), WII_DISC_MAGIC, "the magic at 0x18");
  CHECK(gc_disc_read_raw(disc, 0x40000u, buffer, 0x30) == 0, "the partition table reads");
  CHECK_U64(be32(buffer), 2u, "the partition count");
  CHECK_U64(be32(buffer + 0x24), 1u, "the first partition is the update");
  CHECK_U64((uint64_t)be32(buffer + 0x28) << 2, partition, "the second is the game, here");
  CHECK(gc_disc_read_raw(disc, 0x4fffcu, buffer, 4) == 0 && be32(buffer) == 0xc3f81a8eu,
        "the magic that ends the region block");
  /* Between the update partition's header and the game partition there is
   * nothing, which a container leaves out and a read gets as zeroes. */
  CHECK(gc_disc_read_raw(disc, 0x58000u, buffer, 0x1000) == 0, "empty space reads");
  for (i = 0; i < 0x1000u; i++) wrong += buffer[i] != 0;
  CHECK(wrong == 0, "and reads as zeroes (%u bytes do not)", wrong);
  CHECK(gc_disc_read_raw(disc, disc->size - 4u, buffer, 4) == 0, "the disc's last bytes read");
  CHECK(gc_disc_read_raw(disc, disc->size - 4u, buffer, 8) != 0, "a raw read past the end fails");

  current = "the ticket and the metadata";
  blob = gc_disc_ticket(disc, &size);
  CHECK(blob != NULL && size == WII_TICKET_SIZE, "the ticket is %u bytes", size);
  CHECK(gc_disc_read_raw(disc, partition, buffer, WII_TICKET_SIZE) == 0, "the same bytes read raw");
  if (blob) CHECK(memcmp(blob, buffer, WII_TICKET_SIZE) == 0, "and they are the same");
  if (blob) CHECK(memcmp(blob + 0x1dc, "\0\1\0\0RWTE", 8) == 0, "the title id is in it");
  blob = gc_disc_tmd(disc, &size);
  CHECK(blob != NULL && size == WII_TEST_TMD_SIZE, "the title metadata is 0x%x bytes", size);
  if (blob) CHECK(memcmp(blob + 0x18c, "\0\1\0\0RWTE", 8) == 0, "the title id is in that too");
  if (blob && size == WII_TEST_TMD_SIZE) {
    /* The metadata's one content hash is the hash of the H3 table, which
     * sits in the partition's header area, unencrypted. */
    uint8_t *h3 = (uint8_t *)malloc(0x18000u);
    uint8_t digest[WII_SHA1_SIZE];
    CHECK(gc_disc_read_raw(disc, partition, buffer, 0x2c0) == 0, "the partition header reads");
    if (h3 && gc_disc_read_raw(disc, partition + ((uint64_t)be32(buffer + 0x2b4) << 2), h3, 0x18000u) == 0) {
      wii_sha1(h3, 0x18000u, digest);
      CHECK(memcmp(digest, blob + 0x1e4 + 0x10, sizeof(digest)) == 0, "the H3 table has the hash it records");
    } else {
      CHECK(0, "the H3 table does not read");
    }
    free(h3);
  }
  blob = gc_disc_cert_chain(disc, &size);
  CHECK(blob != NULL && size == WII_TEST_CERT_SIZE, "the certificate chain is 0x%x bytes", size);
  for (i = 0, wrong = 0; blob && i < size; i++) wrong += blob[i] != wii_test_cert_byte(i);
  CHECK(wrong == 0, "%u bytes of the chain are wrong", wrong);

  current = "the data as stored";
  CHECK(gc_disc_read_raw(disc, partition, buffer, 0x2c0) == 0, "the partition header reads");
  data = partition + ((uint64_t)be32(buffer + 0x2b8) << 2);
  CHECK_U64(data, partition + WII_TEST_PARTITION_DATA, "where the data starts");
  CHECK(gc_disc_read_raw(disc, data, buffer, sizeof(buffer)) == 0, "the first cluster reads raw");
  if (!clusters) {
    CHECK(memcmp(buffer, wii_test_disc_plain(), sizeof(buffer)) == 0, "and is simply the data");
  } else if (!encrypted) {
    CHECK(memcmp(buffer + 0x400, wii_test_disc_plain(), 0x7c00) == 0, "and holds the data after 0x400 of hashes");
  } else {
    CHECK(memcmp(buffer + 0x400, wii_test_disc_plain(), 0x7c00) != 0, "and is not the decrypted data");
  }
}

/* Every cluster decrypted here, the plain way - read raw, the vector out
 * of the encrypted hashes, one CBC pass - and the reader's answers held
 * to that, for reads at random places and of random lengths. The reader
 * keeps sixteen clusters and there are twenty-four, so this goes through
 * hits, misses and evictions in no particular order. */
static void check_against_straight_decrypt(GcDisc *disc) {
  enum { READS = 3000, LONGEST = 0x12000 };
  const uint64_t data = gc_disc_partition_offset(disc) + WII_TEST_PARTITION_DATA;
  uint8_t *straight = (uint8_t *)malloc(WII_TEST_DATA_SIZE);
  uint8_t *got = (uint8_t *)malloc(LONGEST);
  static uint8_t raw[0x8000];
  uint32_t state = 0x9e3779b9u, c, i;
  unsigned failed = 0, differing = 0;
  WiiAes aes;

  current = "random reads against a straight decrypt";
  if (!straight || !got) {
    CHECK(0, "out of memory");
    return;
  }
  wii_aes_set_key(&aes, title_key());
  for (c = 0; c < WII_TEST_CLUSTERS; c++) {
    static uint8_t hashes[0x400];
    static const uint8_t zero_iv[16] = {0};
    uint8_t digest[WII_SHA1_SIZE];
    if (gc_disc_read_raw(disc, data + (uint64_t)c * 0x8000u, raw, sizeof(raw)) != 0) {
      CHECK(0, "cluster %u does not read raw", c);
      break;
    }
    wii_aes_cbc_decrypt(&aes, raw + 0x3d0, raw + 0x400, straight + c * 0x7c00u, 0x7c00);
    /* And the cluster's own first hash, of its own first block. */
    wii_aes_cbc_decrypt(&aes, zero_iv, raw, hashes, sizeof(hashes));
    wii_sha1(straight + c * 0x7c00u, 0x400, digest);
    CHECK(memcmp(digest, hashes, sizeof(digest)) == 0, "cluster %u's first H0 hash", c);
  }
  CHECK(memcmp(straight, wii_test_disc_plain(), WII_TEST_DATA_SIZE) == 0,
        "the straight decrypt is the data the disc was built from");

  for (i = 0; i < READS; i++) {
    uint32_t at, length;
    state = state * 1664525u + 1013904223u;
    at = (state >> 8) % WII_TEST_DATA_SIZE;
    state = state * 1664525u + 1013904223u;
    /* Mostly the small reads a game makes of a file's header; now and
     * then one that covers several clusters. */
    length = (i % 16u == 0) ? (state >> 8) % LONGEST : 1u + (state >> 8) % 600u;
    if (length > WII_TEST_DATA_SIZE - at) length = WII_TEST_DATA_SIZE - at;
    if (gc_disc_read(disc, at, got, length) != 0) {
      failed++;
    } else if (memcmp(got, straight + at, length) != 0) {
      differing++;
    }
  }
  CHECK(failed == 0, "%u of %d reads failed", failed, (int)READS);
  CHECK(differing == 0, "%u of %d reads returned something else", differing, (int)READS);
  free(straight);
  free(got);
}

/* ------------------------------------------------------------------ */
/* The tests                                                           */

/* One disc, from locked to read through. */
static void test_container(const char *name, const WiiTestDisc *options) {
  static uint8_t buffer[64];
  const char *path = in_work(name);
  FILE *log = log_open();
  const char *text;
  const GcDiscEntry *entries;
  uint8_t key[16];
  void *dol = NULL;
  uint32_t size = 0;
  GcDisc *disc;

  image = name;
  current = "building the disc";
  CHECK(wii_test_disc_write(path, options) == 0, "it is written");

  current = "opening without a key";
  no_keys_in_environment();
  disc = gc_disc_open(path, log);
  CHECK(disc != NULL, "it opens: %s", log_text(log));
  if (!disc) {
    fclose(log);
    return;
  }
  CHECK(disc->is_wii == 1 && disc->offset_shift == 2u, "it is a Wii disc");
  CHECK(gc_disc_is_locked(disc), "it is locked");
  CHECK(strcmp(disc->game_code, WII_TEST_GAME_CODE) == 0, "the id from the front of the disc, %s",
        disc->game_code);
  CHECK(strcmp(disc->name, WII_TEST_GAME_NAME) == 0, "the name from the front of the disc");
  CHECK(disc->entry_count == 0 && disc->entries == NULL && disc->fst == NULL, "no filesystem yet");
  CHECK(gc_disc_find(disc, "hello.txt") == NULL, "so no file by name");
  CHECK(gc_disc_title_key(disc, key) != 0, "and no title key to hand out");
  CHECK(gc_disc_ticket(disc, &size) != NULL && size == WII_TICKET_SIZE, "the ticket needs no key");
  CHECK(strstr(log_text(log), "locked") != NULL, "the log says it is locked");

  current = "reading a locked disc";
  CHECK(count_of(log_text(log), "WII_COMMON_KEY") == 0, "nothing about keys before a read is tried");
  CHECK(gc_disc_read(disc, 0, buffer, sizeof(buffer)) != 0, "a read fails");
  CHECK(gc_disc_read(disc, WII_TEST_HELLO_AT, buffer, 32) != 0, "and another");
  CHECK(gc_disc_read_dol(disc, &dol, &size) != 0, "and the DOL");
  text = log_text(log);
  CHECK(count_of(text, "no key for it was given") == 1, "the log says why, once: %s", text);
  CHECK(strstr(text, "WII_COMMON_KEY") && strstr(text, "WII_KEY_FILE") &&
            strstr(text, "wii-common-key.bin") && strstr(text, "keys.bin") &&
            strstr(text, "gc_disc_set_title_key"),
        "and names every way of supplying one");
  CHECK(strstr(text, gc_disc_key_help()) != NULL, "in the words gc_disc_key_help() gives");
  CHECK(gc_disc_read_raw(disc, 0x18, buffer, 4) == 0 && be32(buffer) == WII_DISC_MAGIC,
        "the raw disc reads all the same");

  current = "the wrong common key";
  CHECK(gc_disc_set_common_key(disc, wrong_key, log) != 0, "it is refused");
  CHECK(gc_disc_is_locked(disc), "the disc stays locked");
  CHECK(gc_disc_read(disc, 0, buffer, sizeof(buffer)) != 0, "and still does not read");
  text = log_text(log);
  CHECK(count_of(text, "wrong key") == 1, "the log calls it the wrong key: %s", text);
  CHECK(strstr(text, options->common_key_index == 1 ? "Korean" : "standard") != NULL,
        "and says which common key the ticket asks for");

  current = "the right common key";
  CHECK(gc_disc_set_common_key(disc, common_key(), log) == 0, "it opens the partition: %s", log_text(log));
  CHECK(gc_disc_title_key(disc, key) == 0, "the title key can be had");
  CHECK(memcmp(key, title_key(), sizeof(key)) == 0, "and is the one the disc was built with");
  check_layout(disc);
  check_files(disc);
  check_raw(disc, options->partition_offset, 1, 1);
  check_against_straight_decrypt(disc);

  /* A program holding entries must not have them moved from under it by
   * someone offering a key again. */
  current = "a key offered to an open disc";
  entries = disc->entries;
  CHECK(gc_disc_set_common_key(disc, common_key(), log) == 0, "the right one is accepted again");
  CHECK(gc_disc_set_title_key(disc, title_key()) == 0, "as is the title key");
  CHECK(disc->entries == entries, "and the filesystem stays where it was");
  CHECK(gc_disc_set_common_key(disc, wrong_key, NULL) != 0, "a wrong one is still refused");
  CHECK(!gc_disc_is_locked(disc), "but does not lock the disc again");
  CHECK(gc_disc_read(disc, WII_TEST_HELLO_AT, buffer, 32) == 0 &&
            memcmp(buffer, WII_TEST_HELLO, sizeof(WII_TEST_HELLO) - 1u) == 0,
        "which goes on reading");

  gc_disc_close(disc);
  fclose(log);
}

/* The title key alone: what a generated game has, having embedded it. */
static void test_title_key(const char *name) {
  static uint8_t buffer[64];
  FILE *log = log_open();
  GcDisc *disc;
  uint8_t key[16];

  image = name;
  current = "the title key alone";
  no_keys_in_environment();
  disc = gc_disc_open(in_work(name), log);
  CHECK(disc != NULL && gc_disc_is_locked(disc), "the disc opens locked");
  if (!disc) {
    fclose(log);
    return;
  }
  CHECK(gc_disc_set_title_key(disc, wrong_key) != 0, "a wrong title key is refused");
  /* The common key is not a title key, however right it is. */
  CHECK(gc_disc_set_title_key(disc, common_key()) != 0, "as is the common key offered as one");
  CHECK(gc_disc_is_locked(disc), "the disc stays locked");
  CHECK(count_of(log_text(log), "wrong key") == 2, "each is called the wrong key: %s", log_text(log));
  CHECK(gc_disc_read(disc, 0, buffer, sizeof(buffer)) != 0, "nothing reads");

  CHECK(gc_disc_set_title_key(disc, title_key()) == 0, "the right one opens the partition");
  CHECK(gc_disc_title_key(disc, key) == 0 && memcmp(key, title_key(), 16) == 0, "and is handed back");
  check_layout(disc);
  check_files(disc);
  gc_disc_close(disc);
  fclose(log);
}

/* One byte from the middle of a cluster, and whether it is the byte the
 * disc was built with. */
static int cluster_reads_right(GcDisc *disc, uint32_t cluster) {
  const uint32_t at = cluster * 0x7c00u + 0x3e00u;
  uint8_t byte = 0;
  return gc_disc_read(disc, at, &byte, 1) == 0 && byte == wii_test_disc_plain()[at];
}

/* The clusters the reader keeps, and which one it lets go of.
 *
 * Whether a read was answered from memory cannot be seen from outside -
 * unless the disc changes underneath. So: read a cluster, then overwrite
 * it in the file. For as long as the reader still has it, it goes on
 * reading right; once the reader has let it go, it reads wrong. That
 * turns "sixteen clusters, the least recently used replaced first" into
 * something a test can hold it to. */
static void test_cluster_cache(void) {
  static const WiiTestDisc options = {WII_TEST_ISO, 0, WII_TEST_NEAR_PARTITION, 0, 0, 0};
  enum { KEPT = 5 };
  const long stored_at = (long)(WII_TEST_NEAR_PARTITION + WII_TEST_PARTITION_DATA + KEPT * 0x8000u);
  static uint8_t spoiled[0x8000];
  static uint8_t raw[16];
  const char *path = in_work("cache.iso");
  uint32_t cluster;
  unsigned wrong = 0;
  GcDisc *disc;
  FILE *file;

  image = "cache.iso";
  current = "the clusters kept";
  no_keys_in_environment();
  CHECK(wii_test_disc_write(path, &options) == 0, "the disc is written");
  disc = gc_disc_open(path, NULL);
  CHECK(disc != NULL && gc_disc_set_title_key(disc, title_key()) == 0, "it opens");
  if (!disc) return;

  CHECK(cluster_reads_right(disc, KEPT), "cluster %d reads", (int)KEPT);
  memset(spoiled, 0xff, sizeof(spoiled));
  file = fopen(path, "r+b");
  CHECK(file != NULL, "the image can be written to while it is open");
  if (file) {
    CHECK(fseek(file, stored_at, SEEK_SET) == 0 && fwrite(spoiled, 1, sizeof(spoiled), file) == sizeof(spoiled),
          "the cluster is overwritten in the file");
    fclose(file);
  }
  CHECK(gc_disc_read_raw(disc, (uint64_t)stored_at, raw, sizeof(raw)) == 0 && raw[0] == 0xff && raw[15] == 0xff,
        "and a raw read sees that it has been");

  /* Opening the disc read the header and the filesystem, both in cluster
   * 0. With cluster 5, and fourteen more read here, that is sixteen. */
  for (cluster = 6; cluster < 20u; cluster++) {
    CHECK(cluster_reads_right(disc, cluster), "cluster %u reads", cluster);
    wrong += !cluster_reads_right(disc, KEPT);
  }
  CHECK(wrong == 0, "the kept cluster was read again from the file %u times while there was room", wrong);

  /* Now every new cluster pushes one out, and it should be the one asked
   * for longest ago - never the one asked for a moment ago. */
  for (cluster = 20; cluster < WII_TEST_CLUSTERS; cluster++) {
    CHECK(cluster_reads_right(disc, cluster), "cluster %u reads", cluster);
    wrong += !cluster_reads_right(disc, KEPT);
  }
  for (cluster = 0; cluster < 5u; cluster++) {
    CHECK(cluster_reads_right(disc, cluster), "cluster %u reads", cluster);
    wrong += !cluster_reads_right(disc, KEPT);
  }
  CHECK(wrong == 0, "the kept cluster was pushed out %u times though it was the most recently used", wrong);

  /* Sixteen others without going back to it, and it has to have gone:
   * what the file holds now does not decrypt to what was there. */
  for (cluster = 6; cluster < 22u; cluster++) {
    CHECK(cluster_reads_right(disc, cluster), "cluster %u reads", cluster);
  }
  CHECK(!cluster_reads_right(disc, KEPT), "after sixteen others the kept cluster is read from the file again");
  gc_disc_close(disc);
}

/* Opens the disc with nothing in the environment and offers it a key
 * file. Returns what the reader said. */
static int offer_key_file(const char *disc_name, const char *key_path, FILE *log, int expect_open) {
  GcDisc *disc;
  int result = -1;

  no_keys_in_environment();
  disc = gc_disc_open(in_work(disc_name), NULL);
  CHECK(disc != NULL && gc_disc_is_locked(disc), "the disc opens locked");
  if (!disc) return -1;
  result = gc_disc_load_key_file(disc, key_path, log);
  CHECK((result == 0) == (expect_open != 0), "loading %s returned %d: %s", key_path, result, log_text(log));
  CHECK(gc_disc_is_locked(disc) == !expect_open, "and the disc is %s", expect_open ? "open" : "locked");
  if (expect_open) check_files(disc);
  gc_disc_close(disc);
  return result;
}

static void test_key_files(const char *name) {
  static uint8_t keys_bin[0x400], otp[0x80], junk[10], noise[0x400];
  char hex[80];
  FILE *log = log_open();
  unsigned i;

  image = name;

  current = "a key file of sixteen bytes";
  write_file(in_work("key-raw.bin"), common_key(), 16);
  offer_key_file(name, in_work("key-raw.bin"), log, 1);

  current = "a key file of hex digits";
  key_as_hex(WII_TEST_COMMON_KEY, hex, 0, 0);
  write_file(in_work("key-hex.txt"), hex, strlen(hex));
  offer_key_file(name, in_work("key-hex.txt"), log, 1);

  current = "a key file of hex digits with white space";
  key_as_hex(WII_TEST_COMMON_KEY, hex, 1, 1);
  strcat(hex, "\n");
  write_file(in_work("key-hex-spaced.txt"), hex, strlen(hex));
  offer_key_file(name, in_work("key-hex-spaced.txt"), log, 1);

  /* BootMii's keys.bin: 0x100 bytes of text, then the one-time-
   * programmable memory, in which the common key follows a twenty-byte
   * hash. Everything but the key is filler here. */
  current = "a BootMii keys.bin";
  for (i = 0; i < sizeof(keys_bin); i++) keys_bin[i] = (uint8_t)(i * 13u + 1u);
  memset(keys_bin, 0, 0x100);
  strcpy((char *)keys_bin, "BackupMii v1, ConsoleID: 00000000");
  memcpy(keys_bin + 0x114, common_key(), 16);
  write_file(in_work("key-bootmii.bin"), keys_bin, sizeof(keys_bin));
  offer_key_file(name, in_work("key-bootmii.bin"), log, 1);

  current = "a dump of the one-time-programmable memory";
  for (i = 0; i < sizeof(otp); i++) otp[i] = (uint8_t)(i * 29u + 3u);
  memcpy(otp + 0x14, common_key(), 16);
  write_file(in_work("key-otp.bin"), otp, sizeof(otp));
  offer_key_file(name, in_work("key-otp.bin"), log, 1);

  fclose(log);
  log = log_open();
  current = "a key file holding the wrong key";
  write_file(in_work("key-wrong.bin"), wrong_key, 16);
  offer_key_file(name, in_work("key-wrong.bin"), log, 0);
  CHECK(count_of(log_text(log), "wrong key") == 1, "the log calls it the wrong key: %s", log_text(log));
  CHECK(strstr(log_text(log), "key-wrong.bin") != NULL, "and names the file");

  fclose(log);
  log = log_open();
  current = "a keys.bin with no right key in it";
  for (i = 0; i < sizeof(noise); i++) noise[i] = (uint8_t)(i * 7u + 5u);
  write_file(in_work("key-noise.bin"), noise, sizeof(noise));
  offer_key_file(name, in_work("key-noise.bin"), log, 0);
  CHECK(count_of(log_text(log), "wrong key") == 1, "the log says so once, not once a candidate: %s",
        log_text(log));

  fclose(log);
  log = log_open();
  current = "a file that is not a key file";
  memset(junk, 0x41, sizeof(junk));
  write_file(in_work("key-junk.bin"), junk, sizeof(junk));
  offer_key_file(name, in_work("key-junk.bin"), log, 0);
  CHECK(strstr(log_text(log), "is not a key file") != NULL, "the log says what it is not: %s", log_text(log));
  /* Thirty-one digits is not a key either. */
  key_as_hex(WII_TEST_COMMON_KEY, hex, 0, 0);
  hex[31] = 0;
  write_file(in_work("key-short.txt"), hex, strlen(hex));
  offer_key_file(name, in_work("key-short.txt"), log, 0);
  CHECK(count_of(log_text(log), "is not a key file") == 2, "nor is one of thirty-one digits");

  fclose(log);
  log = log_open();
  current = "a key file that is not there";
  offer_key_file(name, in_work("key-missing.bin"), log, 0);
  CHECK(strstr(log_text(log), "cannot open the key file") != NULL, "the log says so: %s", log_text(log));
  fclose(log);
}

/* Opens the disc as the environment stands and says whether it came up
 * open. */
static int opens_unlocked(const char *path, FILE *log) {
  GcDisc *disc = gc_disc_open(path, log);
  int unlocked;
  CHECK(disc != NULL, "it opens: %s", log ? log_text(log) : "");
  if (!disc) return 0;
  unlocked = !gc_disc_is_locked(disc);
  if (unlocked) check_files(disc);
  gc_disc_close(disc);
  return unlocked;
}

/* The places gc_disc_open looks on its own. */
static void test_key_sources(const char *name) {
  static const WiiTestDisc beside_iso = {WII_TEST_ISO, 0, WII_TEST_NEAR_PARTITION, 0, 0, 0};
  static const WiiTestDisc beside_wbfs = {WII_TEST_WBFS, 18, WII_TEST_NEAR_PARTITION, 0, 0, 0};
  static uint8_t keys_bin[0x400];
  char hex[80], path[1024];
  FILE *log;

  image = name;
  write_file(in_work("key-raw.bin"), common_key(), 16);

  current = "WII_COMMON_KEY";
  log = log_open();
  no_keys_in_environment();
  key_as_hex(WII_TEST_COMMON_KEY, hex, 0, 0);
  set_environment("WII_COMMON_KEY", hex);
  CHECK(opens_unlocked(in_work(name), log), "the disc opens with the key in the environment");
  CHECK(strstr(log_text(log), "open") != NULL && !strstr(log_text(log), "locked"), "and the log says open");
  fclose(log);

  current = "WII_COMMON_KEY set to something else";
  log = log_open();
  set_environment("WII_COMMON_KEY", "not a key at all");
  CHECK(!opens_unlocked(in_work(name), log), "the disc stays locked");
  CHECK(strstr(log_text(log), "not to 32 hex digits") != NULL, "and the log says what is wrong: %s",
        log_text(log));
  fclose(log);

  current = "WII_COMMON_KEY set to the wrong key";
  log = log_open();
  set_environment("WII_COMMON_KEY", "000102030405060708090a0b0c0d0e0f");
  CHECK(!opens_unlocked(in_work(name), log), "the disc stays locked");
  CHECK(count_of(log_text(log), "wrong key") == 1, "and the log calls it the wrong key: %s", log_text(log));

  current = "WII_KEY_FILE";
  /* The wrong key is still in WII_COMMON_KEY: the next place is tried. */
  set_environment("WII_KEY_FILE", in_work("key-raw.bin"));
  CHECK(opens_unlocked(in_work(name), log), "the disc opens with a key file in the environment");
  no_keys_in_environment();
  set_environment("WII_KEY_FILE", in_work("key-raw.bin"));
  CHECK(opens_unlocked(in_work(name), log), "and with nothing else set");
  fclose(log);

  current = "WII_KEY_FILE naming no file";
  log = log_open();
  set_environment("WII_KEY_FILE", in_work("key-missing.bin"));
  CHECK(!opens_unlocked(in_work(name), log), "the disc stays locked");
  CHECK(strstr(log_text(log), "cannot open the key file") != NULL, "and the log says so: %s", log_text(log));
  fclose(log);
  no_keys_in_environment();

  current = "wii-common-key.bin beside the image";
  log = log_open();
  make_directory(in_work("beside-a"));
  CHECK(wii_test_disc_write(in_work("beside-a/disc.iso"), &beside_iso) == 0, "the disc is written");
  remove(in_work("beside-a/wii-common-key.bin"));
  CHECK(!opens_unlocked(in_work("beside-a/disc.iso"), log), "locked while nothing is beside it");
  write_file(in_work("beside-a/wii-common-key.bin"), common_key(), 16);
  CHECK(opens_unlocked(in_work("beside-a/disc.iso"), log), "open once the key file is");
  /* Windows writes its paths with the other separator. */
  snprintf(path, sizeof(path), "%s/beside-a\\disc.iso", work);
#if defined(_WIN32)
  CHECK(opens_unlocked(path, log), "and with a backslash before the name");
#endif
  fclose(log);

  current = "keys.bin beside the image";
  log = log_open();
  make_directory(in_work("beside-b"));
  CHECK(wii_test_disc_write(in_work("beside-b/disc.wbfs"), &beside_wbfs) == 0, "the disc is written");
  memset(keys_bin, 0, sizeof(keys_bin));
  memcpy(keys_bin + 0x114, common_key(), 16);
  write_file(in_work("beside-b/keys.bin"), keys_bin, sizeof(keys_bin));
  CHECK(opens_unlocked(in_work("beside-b/disc.wbfs"), log), "the disc opens");
  /* A key file beside the image that does not fit is worth a line. */
  keys_bin[0x114] ^= 0xffu;
  write_file(in_work("beside-b/keys.bin"), keys_bin, sizeof(keys_bin));
  CHECK(!opens_unlocked(in_work("beside-b/disc.wbfs"), log), "locked when that key is wrong");
  CHECK(count_of(log_text(log), "wrong key") == 1, "and the log says so: %s", log_text(log));
  fclose(log);
}

/* The two bytes of the disc header that turn hashing and encryption off. */
static void test_header_flags(void) {
  static const WiiTestDisc no_hashes = {WII_TEST_ISO, 0, WII_TEST_NEAR_PARTITION, 0, 1, 0};
  static const WiiTestDisc no_encryption = {WII_TEST_ISO, 0, WII_TEST_NEAR_PARTITION, 0, 0, 1};
  static const WiiTestDisc neither = {WII_TEST_ISO, 0, WII_TEST_NEAR_PARTITION, 0, 1, 1};
  static const WiiTestDisc korean = {WII_TEST_ISO, 0, WII_TEST_NEAR_PARTITION, 1, 0, 0};
  FILE *log = log_open();
  uint8_t key[16];
  GcDisc *disc;

  no_keys_in_environment();

  /* Hash checking off: the hashes on this disc are deliberately wrong,
   * so a key can only be told by the magic word in what it decrypts. */
  image = "no-hashes.iso";
  current = "a disc with hash checking off";
  CHECK(wii_test_disc_write(in_work(image), &no_hashes) == 0, "it is written");
  disc = gc_disc_open(in_work(image), log);
  CHECK(disc != NULL && gc_disc_is_locked(disc), "it opens locked");
  if (disc) {
    CHECK(gc_disc_set_common_key(disc, wrong_key, log) != 0, "the wrong key is still refused");
    CHECK(count_of(log_text(log), "wrong key") == 1, "and called the wrong key");
    CHECK(gc_disc_set_common_key(disc, common_key(), log) == 0, "the right key opens it: %s", log_text(log));
    check_layout(disc);
    check_files(disc);
    check_raw(disc, WII_TEST_NEAR_PARTITION, 1, 1);
    gc_disc_close(disc);
  }

  /* Encryption off: the same clusters, in the clear, and no key at all. */
  image = "no-encryption.iso";
  current = "a disc with encryption off";
  CHECK(wii_test_disc_write(in_work(image), &no_encryption) == 0, "it is written");
  disc = gc_disc_open(in_work(image), log);
  CHECK(disc != NULL && !gc_disc_is_locked(disc), "it opens, and open: %s", log_text(log));
  if (disc) {
    CHECK(strstr(log_text(log), "not encrypted") != NULL, "the log says it is not encrypted");
    CHECK(gc_disc_title_key(disc, key) != 0, "there is no title key to hand out");
    CHECK(gc_disc_set_common_key(disc, wrong_key, log) == 0, "and any key is as good as none");
    check_layout(disc);
    check_files(disc);
    check_raw(disc, WII_TEST_NEAR_PARTITION, 1, 0);
    gc_disc_close(disc);
  }

  /* Both off: no clusters either. */
  image = "neither.iso";
  current = "a disc with neither";
  CHECK(wii_test_disc_write(in_work(image), &neither) == 0, "it is written");
  disc = gc_disc_open(in_work(image), log);
  CHECK(disc != NULL && !gc_disc_is_locked(disc), "it opens, and open: %s", log_text(log));
  if (disc) {
    check_layout(disc);
    check_files(disc);
    check_raw(disc, WII_TEST_NEAR_PARTITION, 0, 0);
    gc_disc_close(disc);
  }
  fclose(log);

  /* A ticket that names the Korean common key. The reader has no keys of
   * its own, so all the index does is make the message more useful. */
  image = "korean.iso";
  current = "a ticket asking for another common key";
  log = log_open();
  CHECK(wii_test_disc_write(in_work(image), &korean) == 0, "it is written");
  disc = gc_disc_open(in_work(image), log);
  CHECK(disc != NULL && gc_disc_is_locked(disc), "it opens locked");
  if (disc) {
    CHECK(gc_disc_set_common_key(disc, wrong_key, log) != 0, "the wrong key is refused");
    CHECK(strstr(log_text(log), "Korean") != NULL, "and the log says which is wanted: %s", log_text(log));
    CHECK(gc_disc_set_common_key(disc, common_key(), log) == 0, "the key it was made with opens it");
    gc_disc_close(disc);
  }
  fclose(log);
}

/* Opens something that should be refused, and returns the log. */
static const char *refused(const char *path, FILE *log) {
  GcDisc *disc = gc_disc_open(path, log);
  CHECK(disc == NULL, "%s is refused", path);
  if (disc) gc_disc_close(disc);
  return log_text(log);
}

static void test_refusals(void) {
  static const struct {
    const char *file;
    uint8_t magic[4];
    const char *name;
  } formats[] = {
      {"image.rvz", {'R', 'V', 'Z', 1}, "an RVZ"},
      {"image.wia", {'W', 'I', 'A', 1}, "a WIA"},
      {"image.gcz", {0x01, 0xc0, 0x0b, 0xb1}, "a GCZ"},
  };
  static uint8_t bytes[0x1000];
  static uint8_t wbfs[0x41000];
  const char *text;
  unsigned i;
  FILE *source;
  size_t got = 0;

  image = "refusals";
  no_keys_in_environment();

  current = "formats that are not read yet";
  for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
    FILE *log = log_open();
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, formats[i].magic, 4);
    /* Even with a disc's magic words further in, as a compressed image
     * of a real disc might happen to have. */
    bytes[0x18] = 0x5d; bytes[0x19] = 0x1c; bytes[0x1a] = 0x9e; bytes[0x1b] = 0xa3;
    write_file(in_work(formats[i].file), bytes, sizeof(bytes));
    text = refused(in_work(formats[i].file), log);
    CHECK(strstr(text, formats[i].name) != NULL, "the log names the format: %s", text);
    CHECK(strstr(text, "convert it to ISO or WBFS") != NULL, "and says what to do about it: %s", text);
    fclose(log);
  }

  current = "a split WBFS";
  {
    /* The first quarter megabyte and a little of a good WBFS: the header
     * and table are whole, and the table points past the end - which is
     * what the first file of a split image looks like. */
    FILE *log = log_open();
    source = fopen(in_work("near.wbfs"), "rb");
    if (source) {
      got = fread(wbfs, 1, sizeof(wbfs), source);
      fclose(source);
    }
    CHECK(got == sizeof(wbfs), "near.wbfs is there to cut short");
    write_file(in_work("split.wbfs"), wbfs, sizeof(wbfs));
    text = refused(in_work("split.wbfs"), log);
    CHECK(strstr(text, "split WBFS") != NULL && strstr(text, ".wbf1") != NULL,
          "the log says it is split: %s", text);
    fclose(log);

    log = log_open();
    write_file(in_work("split.wbf1"), wbfs + 0x40000, 0x1000);
    text = refused(in_work("split.wbf1"), log);
    CHECK(strstr(text, "later part of a split WBFS") != NULL, "a .wbf1 is named for what it is: %s", text);
    fclose(log);
  }

  current = "a WBFS that makes no sense";
  {
    FILE *log = log_open();
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, "WBFS", 4);
    bytes[8] = 9;
    bytes[9] = 3; /* sectors of eight bytes */
    write_file(in_work("bad.wbfs"), bytes, sizeof(bytes));
    text = refused(in_work("bad.wbfs"), log);
    CHECK(strstr(text, "make no sense") != NULL, "the log says so: %s", text);
    fclose(log);

    log = log_open();
    bytes[9] = 21; /* sensible sizes, and no disc in any slot */
    write_file(in_work("empty.wbfs"), bytes, sizeof(bytes));
    text = refused(in_work("empty.wbfs"), log);
    CHECK(strstr(text, "no disc in it") != NULL, "an empty one is called empty: %s", text);
    fclose(log);
  }

  current = "a Wii disc with no game on it";
  {
    /* The magic, and a partition table whose one entry is an update. */
    static uint8_t disc[0x50000];
    FILE *log = log_open();
    disc[0x18] = 0x5d; disc[0x19] = 0x1c; disc[0x1a] = 0x9e; disc[0x1b] = 0xa3;
    disc[0x40003] = 1;
    disc[0x40004] = 0x00; disc[0x40005] = 0x01; disc[0x40006] = 0x00; disc[0x40007] = 0x08;
    disc[0x40027] = 1;
    write_file(in_work("no-game.iso"), disc, sizeof(disc));
    text = refused(in_work("no-game.iso"), log);
    CHECK(strstr(text, "none of them is a game") != NULL, "the log says so: %s", text);
    fclose(log);
  }

  current = "something that is not a disc";
  {
    FILE *log = log_open();
    for (i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i * 3u + 7u);
    write_file(in_work("noise.iso"), bytes, sizeof(bytes));
    text = refused(in_work("noise.iso"), log);
    CHECK(strstr(text, "no GameCube disc magic") != NULL && strstr(text, "no Wii disc magic") != NULL,
          "the log says neither magic is there: %s", text);
    fclose(log);
  }
}

/* The additions to the interface, on a disc that is not a Wii disc: they
 * answer, and the answer is "not applicable". */
static void test_gamecube_disc(void) {
  static uint8_t gcm[0x460];
  static uint8_t a[0x40], b[0x40];
  FILE *log = log_open();
  GcDisc *disc;
  uint32_t size = 123;
  uint8_t key[16];

  image = "gamecube.gcm";
  current = "a GameCube disc";
  no_keys_in_environment();
  memcpy(gcm, "GTST01", 6);
  gcm[0x1c] = 0xc2; gcm[0x1d] = 0x33; gcm[0x1e] = 0x9f; gcm[0x1f] = 0x3d;
  gcm[0x420 + 2] = 0x01;                    /* a DOL at 0x100, were there one */
  gcm[0x424 + 2] = 0x04; gcm[0x424 + 3] = 0x40; /* the filesystem at 0x440 */
  gcm[0x428 + 3] = 12;
  gcm[0x42c + 3] = 12;
  gcm[0x440] = 1;                           /* the root, and nothing in it */
  gcm[0x440 + 11] = 1;
  write_file(in_work(image), gcm, sizeof(gcm));

  disc = gc_disc_open(in_work(image), log);
  CHECK(disc != NULL, "it opens: %s", log_text(log));
  if (!disc) {
    fclose(log);
    return;
  }
  CHECK(disc->is_wii == 0 && disc->offset_shift == 0u && disc->wii == NULL, "it is not a Wii disc");
  CHECK(!gc_disc_is_locked(disc), "and not locked");
  CHECK_U64(gc_disc_dol_offset(disc), 0x100u, "the DOL's offset is the stored one");
  CHECK_U64(gc_disc_fst_offset(disc), 0x440u, "the filesystem's offset is the stored one");
  CHECK_U64(disc->fst_size, 12u, "the filesystem's size is the stored one");
  CHECK_U64(gc_disc_partition_offset(disc), 0u, "it has no partition");
  CHECK_U64(gc_disc_data_size(disc), 0u, "and no partition data");
  CHECK(gc_disc_ticket(disc, &size) == NULL && size == 0, "no ticket");
  size = 123;
  CHECK(gc_disc_tmd(disc, &size) == NULL && size == 0, "no title metadata");
  CHECK(gc_disc_cert_chain(disc, NULL) == NULL, "no certificate chain");
  CHECK(gc_disc_set_title_key(disc, title_key()) != 0, "a title key is not taken");
  CHECK(gc_disc_set_common_key(disc, common_key(), log) != 0, "nor a common key");
  CHECK(gc_disc_load_key_file(disc, in_work("key-raw.bin"), log) != 0, "nor a key file");
  CHECK(gc_disc_title_key(disc, key) != 0, "and there is none to hand out");
  CHECK(gc_disc_read(disc, 0x400, a, sizeof(a)) == 0 && gc_disc_read_raw(disc, 0x400, b, sizeof(b)) == 0 &&
            memcmp(a, b, sizeof(a)) == 0,
        "a raw read is a read");
  CHECK(gc_disc_read_raw(disc, sizeof(gcm) - 8u, a, 64) != 0, "and fails past the end like one");
  gc_disc_close(disc);
  fclose(log);
}

int main(int argc, char **argv) {
  static const struct {
    const char *name;
    WiiTestDisc options;
  } discs[] = {
      {"near.iso", {WII_TEST_ISO, 0, WII_TEST_NEAR_PARTITION, 0, 0, 0}},
      /* The smallest block a CISO can have and a small WBFS sector, so
       * the disc is spread over many of each. */
      {"near.ciso", {WII_TEST_CISO, 15, WII_TEST_NEAR_PARTITION, 0, 0, 0}},
      {"near.wbfs", {WII_TEST_WBFS, 18, WII_TEST_NEAR_PARTITION, 0, 0, 0}},
      /* The sizes real images use, and the partition past 4 GB. */
      {"far.ciso", {WII_TEST_CISO, 20, WII_TEST_FAR_PARTITION, 0, 0, 0}},
      {"far.wbfs", {WII_TEST_WBFS, 21, WII_TEST_FAR_PARTITION, 0, 0, 0}},
  };
  unsigned i;

  if (argc < 2 || strlen(argv[1]) >= sizeof(work)) {
    printf("usage: wii_disc_test WORKING-DIRECTORY\n");
    return 2;
  }
  strcpy(work, argv[1]);
  make_directory(work);
  image = "setup";
  current = "setup";
  /* Left by an earlier run, these would be found beside the images. */
  remove(in_work("wii-common-key.bin"));
  remove(in_work("keys.bin"));
  if (!wii_test_disc_plain()) {
    printf("FAIL: out of memory\n");
    return 2;
  }

  for (i = 0; i < sizeof(discs) / sizeof(discs[0]); i++) test_container(discs[i].name, &discs[i].options);
  for (i = 0; i < sizeof(discs) / sizeof(discs[0]); i++) test_title_key(discs[i].name);
  test_cluster_cache();
  test_key_files("near.iso");
  test_key_files("far.wbfs");
  test_key_sources("near.iso");
  test_header_flags();
  test_refusals();
  test_gamecube_disc();

  no_keys_in_environment();
  printf("wii_disc_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
