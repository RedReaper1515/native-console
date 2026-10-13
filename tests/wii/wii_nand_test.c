/*
 * The flash filesystem's tree and its one host file (wii_nand.h), and the
 * two settings files (wii_sysconf.h), with no machine anywhere: every
 * operation, every error ISFS can give from a tree in memory, its limits,
 * what a console holds before any game has run, and the file on the host
 * - there and back, cut short, altered, and written when writing fails.
 *
 * SYSCONF and setting.txt are read back by readers written here from the
 * formats' descriptions, sharing no code with what wrote them, and their
 * bytes are held to a recorded hash so that a change to either is seen.
 *
 * The one argument is a directory this may write in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define make_directory(path) _mkdir(path)
#define remove_directory(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0777)
#define remove_directory(path) rmdir(path)
#endif

#include "wii_nand.h"
#include "wii_remote.h"

static int failures;
static int checks;

#define CHECK(condition, ...)                     \
  do {                                            \
    checks++;                                     \
    if (!(condition)) {                           \
      failures++;                                 \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

/* A call that should give exactly this result. */
#define EXPECT(call, want)                                                                  \
  do {                                                                                      \
    const long got_ = (long)(call);                                                         \
    CHECK(got_ == (long)(want), "%s gave %ld, want %ld", #call, got_, (long)(want));        \
  } while (0)

#define RW (WII_NAND_READ | WII_NAND_WRITE)
#define RO WII_NAND_READ
#define WO WII_NAND_WRITE

/* IOS, and three titles: two that share a group and one that shares
 * nothing. */
static const WiiNandCaller ios = {0, 0};
static const WiiNandCaller alice = {0x1001, 0x3031};
static const WiiNandCaller bob = {0x1002, 0x3031};
static const WiiNandCaller carol = {0x1003, 0x3545};

/* The recorded bytes of what a default console is given. A change here is
 * a change to what every game reads at start-up: make it on purpose. */
#define SYSCONF_HASH 0x467032380a124bc2ull     /* FNV-1a, 64 bits, of the 0x4000 bytes */
#define SETTING_TXT_HASH 0x73b7bb98469164f8ull /* of the 0x100 bytes */
#define FRESH_NAND_HASH 0x22c9af2e6ef6a238ull  /* of a freshly formatted console's host file */

static const char *scratch;

static const char *in_scratch(const char *name) {
  static char path[4][512];
  static unsigned next;
  char *out = path[next++ & 3u];
  snprintf(out, sizeof(path[0]), "%s/%s", scratch, name);
  return out;
}

static uint64_t fnv(uint64_t hash, const void *data, size_t size) {
  const uint8_t *p = (const uint8_t *)data;
  size_t i;
  for (i = 0; i < size; i++) hash = (hash ^ p[i]) * 0x100000001b3ull;
  return hash;
}
#define FNV_START 0xcbf29ce484222325ull

static uint8_t *read_whole(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  uint8_t *data;
  long length;
  *size = 0;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  length = ftell(f);
  rewind(f);
  data = (uint8_t *)malloc(length > 0 ? (size_t)length : 1u);
  if (data && length > 0 && fread(data, 1, (size_t)length, f) != (size_t)length) {
    free(data);
    data = NULL;
  }
  fclose(f);
  if (data) *size = (size_t)length;
  return data;
}

static int write_whole(const char *path, const uint8_t *data, size_t size) {
  FILE *f = fopen(path, "wb");
  size_t written;
  if (!f) return -1;
  written = size ? fwrite(data, 1, size, f) : 0;
  return fclose(f) == 0 && written == size ? 0 : -1;
}

/* Everything about a filesystem that a game could find out, in the order
 * it would find it: used to say two filesystems are the same. */
static uint64_t tree_hash_at(WiiNand *nand, const char *path, uint64_t hash) {
  WiiNandAttr attr;
  char names[64][13];
  uint32_t count = 0, i;
  if (wii_nand_get_attr(nand, &ios, path, &attr) != 0) return hash ^ 0xdeadu;
  hash = fnv(hash, path, strlen(path) + 1u);
  hash = fnv(hash, &attr.uid, sizeof(attr.uid));
  hash = fnv(hash, &attr.gid, sizeof(attr.gid));
  hash = fnv(hash, &attr.owner, 1);
  hash = fnv(hash, &attr.group, 1);
  hash = fnv(hash, &attr.other, 1);
  hash = fnv(hash, &attr.attributes, 1);
  hash = fnv(hash, &attr.is_directory, 1);
  if (!attr.is_directory) {
    const uint8_t *data = NULL;
    uint32_t size = 0;
    wii_nand_get(nand, path, &data, &size);
    hash = fnv(hash, &size, sizeof(size));
    return fnv(hash, data, size);
  }
  if (wii_nand_read_dir(nand, &ios, path, names[0], 64, &count) != 0) return hash ^ 0xbeefu;
  hash = fnv(hash, &count, sizeof(count));
  for (i = 0; i < count; i++) {
    char child[80];
    snprintf(child, sizeof(child), "%s%s%s", path, path[1] ? "/" : "", names[i]);
    hash = tree_hash_at(nand, child, hash);
  }
  return hash;
}

static uint64_t tree_hash(WiiNand *nand) { return tree_hash_at(nand, "/", FNV_START); }

/* ------------------------------------------------------------------ */

static void test_paths(void) {
  WiiNand *nand = wii_nand_create();
  WiiNandAttr attr;
  uint32_t count = 99;

  /* What is and is not a path. */
  EXPECT(wii_nand_create_dir(nand, &ios, "", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "a", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "/", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "/a/", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "//a", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "/a\x01", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "/a\x7f", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "/a", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/a//b", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_read_dir(nand, &ios, "/", NULL, 0, &count), 0);
  CHECK(count == 1, "the root has one entry (%u)", (unsigned)count);
  EXPECT(wii_nand_get_attr(nand, &ios, "/", &attr), 0);
  CHECK(attr.is_directory && attr.uid == 0 && attr.owner == RW && attr.group == RW && attr.other == 0,
        "the root is IOS's and nobody else's");

  /* A name: twelve characters and no more. */
  EXPECT(wii_nand_create_file(nand, &ios, "/a/123456789012", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &ios, "/a/1234567890123", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1234567890123/x", 0, RW, RW, RW), WII_FS_EINVAL);
  EXPECT(wii_nand_open(nand, &ios, "/a/1234567890123", RO), WII_FS_EINVAL);
  /* Names are compared exactly. */
  EXPECT(wii_nand_create_file(nand, &ios, "/a/File", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &ios, "/a/file", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &ios, "/a/file", 0, RW, RW, RW), WII_FS_EEXIST);
  EXPECT(wii_nand_create_dir(nand, &ios, "/a/file", 0, RW, RW, RW), WII_FS_EEXIST);

  /* A path: sixty-three characters and no more. Four directories of
   * twelve and a file of ten is exactly that. */
  EXPECT(wii_nand_create_dir(nand, &ios, "/aaaaaaaaaaaa", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/aaaaaaaaaaaa/bbbbbbbbbbbb", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/dddddddddddd", 0, RW, RW, RW), 0);
  {
    const char *exactly = "/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/dddddddddddd/eeeeeeeeee";
    const char *over = "/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/dddddddddddd/eeeeeeeeeee";
    CHECK(strlen(exactly) == WII_NAND_PATH_MAX && strlen(over) == WII_NAND_PATH_MAX + 1u, "the test's own paths");
    EXPECT(wii_nand_create_file(nand, &ios, exactly, 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_file(nand, &ios, over, 0, RW, RW, RW), WII_FS_EINVAL);
    EXPECT(wii_nand_open(nand, &ios, exactly, RO), 0);
    EXPECT(wii_nand_close(nand, exactly), 0);
    EXPECT(wii_nand_open(nand, &ios, over, RO), WII_FS_EINVAL);
    EXPECT(wii_nand_delete(nand, &ios, over), WII_FS_EINVAL);
    EXPECT(wii_nand_get_attr(nand, &ios, over, &attr), WII_FS_EINVAL);
    EXPECT(wii_nand_read_dir(nand, &ios, over, NULL, 0, &count), WII_FS_EINVAL);
    EXPECT(wii_nand_usage(nand, &ios, over, &count, &count), WII_FS_EINVAL);
    EXPECT(wii_nand_rename(nand, &ios, exactly, over), WII_FS_EINVAL);
    EXPECT(wii_nand_rename(nand, &ios, over, exactly), WII_FS_EINVAL);
    EXPECT(wii_nand_set_attr(nand, &ios, over, &attr), WII_FS_EINVAL);
  }

  /* Depth: eight directories, and a file in the eighth. */
  EXPECT(wii_nand_create_dir(nand, &ios, "/1", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4/5", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4/5/6", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4/5/6/7", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4/5/6/7/8", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4/5/6/7/8/9", 0, RW, RW, RW), WII_FS_EDIRDEPTH);
  EXPECT(wii_nand_create_file(nand, &ios, "/1/2/3/4/5/6/7/8/f", 0, RW, RW, RW), 0);
  /* The depth is checked before anything is looked up. */
  EXPECT(wii_nand_create_dir(nand, &ios, "/x/2/3/4/5/6/7/8/9", 0, RW, RW, RW), WII_FS_EDIRDEPTH);

  /* A parent that is missing, and one that is a file. */
  EXPECT(wii_nand_create_file(nand, &ios, "/nowhere/f", 0, RW, RW, RW), WII_FS_ENOENT);
  EXPECT(wii_nand_create_file(nand, &ios, "/a/file/f", 0, RW, RW, RW), WII_FS_ENOENT);
  EXPECT(wii_nand_open(nand, &ios, "/a/file/f", RO), WII_FS_ENOENT);
  EXPECT(wii_nand_delete(nand, &ios, "/nowhere/f"), WII_FS_ENOENT);

  wii_nand_destroy(nand);
}

static void test_files(void) {
  WiiNand *nand = wii_nand_create();
  uint8_t pattern[40000], back[40000];
  uint32_t position = 0, length = 77;
  WiiNandStats before, after;
  unsigned i;
  for (i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i * 7u + (i >> 8));

  EXPECT(wii_nand_create_dir(nand, &ios, "/d", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_open(nand, &ios, "/d/f", RW), WII_FS_ENOENT);
  EXPECT(wii_nand_open(nand, &ios, "/d", RO), WII_FS_EINVAL);
  EXPECT(wii_nand_open(nand, &ios, "/", RO), WII_FS_EINVAL);
  EXPECT(wii_nand_create_file(nand, &ios, "/d/f", 0x5a, RW, RW, RW), 0);
  EXPECT(wii_nand_open(nand, &ios, "/d/f", 4), WII_FS_EINVAL);
  wii_nand_stats(nand, &before);
  EXPECT(wii_nand_open(nand, &ios, "/d/f", RW), 0);
  CHECK(wii_nand_open_count(nand) == 1, "one file is open");

  /* An empty file. */
  EXPECT(wii_nand_length(nand, "/d/f", &length), 0);
  CHECK(length == 0, "a new file is empty");
  EXPECT(wii_nand_read(nand, "/d/f", RW, &position, back, 16), 0);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 0, 2), 0);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 1, 0), WII_FS_EINVAL);

  /* Writing extends it, in pieces that cross a cluster. */
  EXPECT(wii_nand_write(nand, "/d/f", RW, &position, pattern, 100), 100);
  CHECK(position == 100, "a write moves the position (%u)", (unsigned)position);
  EXPECT(wii_nand_write(nand, "/d/f", RW, &position, pattern + 100, 0x4000), 0x4000);
  EXPECT(wii_nand_write(nand, "/d/f", RW, &position, pattern + 100 + 0x4000, 0), 0);
  EXPECT(wii_nand_write(nand, "/d/f", RW, &position, pattern + 100 + 0x4000, 40000 - 100 - 0x4000),
         40000 - 100 - 0x4000);
  EXPECT(wii_nand_length(nand, "/d/f", &length), 0);
  CHECK(length == 40000 && position == 40000, "the file is what was written (%u, %u)", (unsigned)length,
        (unsigned)position);
  wii_nand_stats(nand, &after);
  CHECK(after.used_clusters - before.used_clusters == 3, "40000 bytes are three clusters (%u)",
        (unsigned)(after.used_clusters - before.used_clusters));

  /* Reading: all of it, the end of it, and past the end. */
  EXPECT(wii_nand_read(nand, "/d/f", RW, &position, back, 10), 0);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 0, 0), 0);
  memset(back, 0, sizeof(back));
  EXPECT(wii_nand_read(nand, "/d/f", RW, &position, back, 40000), 40000);
  CHECK(memcmp(back, pattern, 40000) == 0, "it reads back");
  EXPECT(wii_nand_seek(nand, "/d/f", &position, -10, 2), 39990);
  EXPECT(wii_nand_read(nand, "/d/f", RW, &position, back, 100), 10);
  CHECK(memcmp(back, pattern + 39990, 10) == 0 && position == 40000, "a short read at the end");
  EXPECT(wii_nand_read(nand, "/d/f", RW, &position, back, 100), 0);

  /* Seeking: each origin, and neither before the start nor past the end. */
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 1234, 0), 1234);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 100, 1), 1334);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, -1334, 1), 0);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, -1, 1), WII_FS_EINVAL);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, -1, 0), WII_FS_EINVAL);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 40000, 0), 40000);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 40001, 0), WII_FS_EINVAL);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 1, 2), WII_FS_EINVAL);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 0, 3), WII_FS_EINVAL);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 0, -1), WII_FS_EINVAL);
  CHECK(position == 40000, "a refused seek leaves the position alone (%u)", (unsigned)position);

  /* Overwriting in the middle changes those bytes and no length. */
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 5000, 0), 5000);
  EXPECT(wii_nand_write(nand, "/d/f", RW, &position, "hello", 5), 5);
  EXPECT(wii_nand_length(nand, "/d/f", &length), 0);
  EXPECT(wii_nand_seek(nand, "/d/f", &position, 4999, 0), 4999);
  EXPECT(wii_nand_read(nand, "/d/f", RW, &position, back, 7), 7);
  CHECK(length == 40000 && back[0] == pattern[4999] && memcmp(back + 1, "hello", 5) == 0 && back[6] == pattern[5005],
        "an overwrite in place");

  /* The mode a descriptor was opened with is all it may do. */
  EXPECT(wii_nand_read(nand, "/d/f", WO, &position, back, 1), WII_FS_EACCESS);
  EXPECT(wii_nand_write(nand, "/d/f", RO, &position, back, 1), WII_FS_EACCESS);
  EXPECT(wii_nand_read(nand, "/d/f", 0, &position, back, 1), WII_FS_EACCESS);
  EXPECT(wii_nand_read(nand, "/d/f", RO, &position, NULL, 1), WII_FS_EINVAL);
  EXPECT(wii_nand_write(nand, "/d/f", WO, &position, NULL, 1), WII_FS_EINVAL);

  /* The chip is 512 MB less what is not a file's: a write that would need
   * one cluster more than that is refused before anything is allocated,
   * and so is one that would pass four gigabytes. */
  position = (WII_NAND_USABLE_CLUSTERS - 3u) * WII_NAND_CLUSTER + 100u;
  EXPECT(wii_nand_create_file(nand, &ios, "/d/big", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_write(nand, "/d/big", RW, &position, pattern, 1), WII_FS_EFBIG);
  position = 0xfffffff0u;
  EXPECT(wii_nand_write(nand, "/d/big", RW, &position, pattern, 0x20), WII_FS_EFBIG);
  EXPECT(wii_nand_length(nand, "/d/big", &length), 0);
  CHECK(length == 0, "a refused write writes nothing");

  /* Open files cannot be deleted or moved, nor can what holds them. */
  EXPECT(wii_nand_delete(nand, &ios, "/d/f"), WII_FS_EFDOPEN);
  EXPECT(wii_nand_delete(nand, &ios, "/d"), WII_FS_EFDOPEN);
  EXPECT(wii_nand_create_dir(nand, &ios, "/e", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_rename(nand, &ios, "/d/f", "/e/f"), WII_FS_EFDOPEN);
  EXPECT(wii_nand_rename(nand, &ios, "/d", "/e/d"), WII_FS_EFDOPEN);
  EXPECT(wii_nand_create_file(nand, &ios, "/e/f", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_open(nand, &ios, "/e/f", RO), 0);
  EXPECT(wii_nand_close(nand, "/d/f"), 0);
  EXPECT(wii_nand_rename(nand, &ios, "/d/f", "/e/f"), WII_FS_EFDOPEN); /* the destination is open */
  EXPECT(wii_nand_close(nand, "/e/f"), 0);
  EXPECT(wii_nand_close(nand, "/e/f"), WII_FS_EINVAL);
  CHECK(wii_nand_open_count(nand) == 0, "nothing is open");

  /* The same file may be open more than once. */
  EXPECT(wii_nand_open(nand, &ios, "/d/f", RO), 0);
  EXPECT(wii_nand_open(nand, &ios, "/d/f", RW), 0);
  EXPECT(wii_nand_close(nand, "/d/f"), 0);
  EXPECT(wii_nand_delete(nand, &ios, "/d/f"), WII_FS_EFDOPEN);
  EXPECT(wii_nand_close(nand, "/d/f"), 0);

  /* Sixteen descriptors. */
  for (i = 0; i < 17u; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/e/%02u", i);
    EXPECT(wii_nand_create_file(nand, &ios, path, 0, RW, RW, RW), 0);
    EXPECT(wii_nand_open(nand, &ios, path, RO), i < 16u ? 0 : WII_FS_EFDEXHAUSTED);
  }
  CHECK(wii_nand_open_count(nand) == WII_NAND_OPEN_MAX, "sixteen are open (%u)", wii_nand_open_count(nand));
  EXPECT(wii_nand_close(nand, "/e/03"), 0);
  EXPECT(wii_nand_open(nand, &ios, "/e/16", RO), 0);
  /* What a reset or a loaded state does: nothing is open, then some are. */
  wii_nand_forget_open(nand);
  CHECK(wii_nand_open_count(nand) == 0, "forgotten");
  EXPECT(wii_nand_delete(nand, &ios, "/e/00"), 0);
  EXPECT(wii_nand_mark_open(nand, "/e/01"), 0);
  EXPECT(wii_nand_mark_open(nand, "/e/00"), WII_FS_ENOENT);
  EXPECT(wii_nand_delete(nand, &ios, "/e/01"), WII_FS_EFDOPEN);
  EXPECT(wii_nand_delete(nand, &ios, "/e"), WII_FS_EFDOPEN);
  EXPECT(wii_nand_close(nand, "/e/01"), 0);
  EXPECT(wii_nand_delete(nand, &ios, "/e"), 0);

  /* A whole file at once. */
  {
    const uint8_t *data = NULL;
    uint32_t size = 0;
    WiiNandAttr attr;
    EXPECT(wii_nand_put(nand, "/d/whole", "0123456789", 10), 0);
    EXPECT(wii_nand_get(nand, "/d/whole", &data, &size), 0);
    CHECK(size == 10 && memcmp(data, "0123456789", 10) == 0, "put then get");
    EXPECT(wii_nand_put(nand, "/d/whole", "abc", 3), 0);
    EXPECT(wii_nand_get(nand, "/d/whole", &data, &size), 0);
    CHECK(size == 3 && memcmp(data, "abc", 3) == 0, "put replaces");
    EXPECT(wii_nand_get_attr(nand, &ios, "/d/whole", &attr), 0);
    CHECK(attr.uid == 0 && attr.owner == RW && attr.group == RW && attr.other == RW, "a put file is IOS's, open to all");
    EXPECT(wii_nand_put(nand, "/d", "abc", 3), WII_FS_EINVAL);
    EXPECT(wii_nand_put(nand, "/nowhere/whole", "abc", 3), WII_FS_ENOENT);
    EXPECT(wii_nand_get(nand, "/d/none", &data, &size), WII_FS_ENOENT);
  }
  wii_nand_destroy(nand);
}

static void test_permissions(void) {
  WiiNand *nand = wii_nand_create();
  WiiNandAttr attr;
  uint32_t position = 0, count = 0;
  char names[8][13];

  /* A directory that is alice's: she may do anything in it, her group may
   * look, and the rest may do nothing. */
  EXPECT(wii_nand_create_dir(nand, &ios, "/open", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &alice, "/open/hers", 7, RW, RO, 0), 0);
  EXPECT(wii_nand_get_attr(nand, &ios, "/open/hers", &attr), 0);
  CHECK(attr.uid == alice.uid && attr.gid == alice.gid && attr.owner == RW && attr.group == RO && attr.other == 0 &&
            attr.attributes == 7 && attr.is_directory,
        "what is made belongs to who made it");

  /* The root is closed to everyone but IOS. */
  EXPECT(wii_nand_create_dir(nand, &alice, "/mine", 0, RW, RW, RW), WII_FS_EACCESS);
  EXPECT(wii_nand_read_dir(nand, &alice, "/", NULL, 0, &count), WII_FS_EACCESS);
  EXPECT(wii_nand_get_attr(nand, &alice, "/open", &attr), WII_FS_EACCESS);
  EXPECT(wii_nand_get_attr(nand, &alice, "/", &attr), 0);
  EXPECT(wii_nand_delete(nand, &alice, "/open"), WII_FS_EACCESS);

  /* Creating and deleting need write on the directory. */
  EXPECT(wii_nand_create_file(nand, &alice, "/open/hers/f", 0, RW, RO, 0), 0);
  EXPECT(wii_nand_create_file(nand, &bob, "/open/hers/g", 0, RW, RW, RW), WII_FS_EACCESS);
  EXPECT(wii_nand_create_file(nand, &carol, "/open/hers/g", 0, RW, RW, RW), WII_FS_EACCESS);
  EXPECT(wii_nand_create_dir(nand, &carol, "/open/hers/g", 0, RW, RW, RW), WII_FS_EACCESS);
  EXPECT(wii_nand_delete(nand, &bob, "/open/hers/f"), WII_FS_EACCESS);
  EXPECT(wii_nand_delete(nand, &carol, "/open/hers/f"), WII_FS_EACCESS);
  /* Being refused comes before finding out whether it is there. */
  EXPECT(wii_nand_delete(nand, &carol, "/open/hers/none"), WII_FS_EACCESS);
  EXPECT(wii_nand_delete(nand, &alice, "/open/hers/none"), WII_FS_ENOENT);
  EXPECT(wii_nand_create_file(nand, &alice, "/open/hers/f", 0, RW, RO, 0), WII_FS_EEXIST);
  EXPECT(wii_nand_create_file(nand, &carol, "/open/hers/f", 0, RW, RO, 0), WII_FS_EACCESS);

  /* Listing needs read on the directory; attributes need read on the
   * directory the thing is in. */
  EXPECT(wii_nand_read_dir(nand, &alice, "/open/hers", names[0], 8, &count), 0);
  EXPECT(wii_nand_read_dir(nand, &bob, "/open/hers", names[0], 8, &count), 0);
  EXPECT(wii_nand_read_dir(nand, &carol, "/open/hers", names[0], 8, &count), WII_FS_EACCESS);
  EXPECT(wii_nand_get_attr(nand, &bob, "/open/hers/f", &attr), 0);
  EXPECT(wii_nand_get_attr(nand, &carol, "/open/hers/f", &attr), WII_FS_EACCESS);
  EXPECT(wii_nand_get_attr(nand, &carol, "/open/hers", &attr), 0);
  /* Refused is said even of a file: that is how the SDK tells a directory
   * it may not list from a file. */
  EXPECT(wii_nand_read_dir(nand, &carol, "/open/hers/f", NULL, 0, &count), WII_FS_EACCESS);
  EXPECT(wii_nand_read_dir(nand, &alice, "/open/hers/f", NULL, 0, &count), WII_FS_EINVAL);

  /* Opening needs the bits of the mode asked for. */
  EXPECT(wii_nand_open(nand, &alice, "/open/hers/f", RW), 0);
  EXPECT(wii_nand_write(nand, "/open/hers/f", RW, &position, "x", 1), 1);
  EXPECT(wii_nand_close(nand, "/open/hers/f"), 0);
  EXPECT(wii_nand_open(nand, &bob, "/open/hers/f", RO), 0);
  EXPECT(wii_nand_close(nand, "/open/hers/f"), 0);
  EXPECT(wii_nand_open(nand, &bob, "/open/hers/f", WO), WII_FS_EACCESS);
  EXPECT(wii_nand_open(nand, &bob, "/open/hers/f", RW), WII_FS_EACCESS);
  EXPECT(wii_nand_open(nand, &carol, "/open/hers/f", RO), WII_FS_EACCESS);
  EXPECT(wii_nand_open(nand, &carol, "/open/hers/f", 0), 0);
  EXPECT(wii_nand_close(nand, "/open/hers/f"), 0);
  EXPECT(wii_nand_open(nand, &ios, "/open/hers/f", RW), 0);
  EXPECT(wii_nand_close(nand, "/open/hers/f"), 0);

  /* Changing attributes: the owner may, and may not give the thing away;
   * IOS may give away an empty file and not a full one. */
  EXPECT(wii_nand_get_attr(nand, &ios, "/open/hers/f", &attr), 0);
  attr.other = RO;
  attr.attributes = 9;
  EXPECT(wii_nand_set_attr(nand, &bob, "/open/hers/f", &attr), WII_FS_EACCESS);
  EXPECT(wii_nand_set_attr(nand, &carol, "/open/hers/f", &attr), WII_FS_EACCESS);
  EXPECT(wii_nand_set_attr(nand, &alice, "/open/hers/f", &attr), 0);
  EXPECT(wii_nand_get_attr(nand, &ios, "/open/hers/f", &attr), 0);
  CHECK(attr.other == RO && attr.attributes == 9 && attr.uid == alice.uid, "the owner changed them");
  EXPECT(wii_nand_open(nand, &carol, "/open/hers/f", RO), 0);
  EXPECT(wii_nand_close(nand, "/open/hers/f"), 0);
  attr.uid = bob.uid;
  EXPECT(wii_nand_set_attr(nand, &alice, "/open/hers/f", &attr), WII_FS_EACCESS);
  EXPECT(wii_nand_set_attr(nand, &ios, "/open/hers/f", &attr), WII_FS_ENOTEMPTY);
  EXPECT(wii_nand_create_file(nand, &alice, "/open/hers/empty", 0, RW, 0, 0), 0);
  EXPECT(wii_nand_get_attr(nand, &ios, "/open/hers/empty", &attr), 0);
  attr.uid = bob.uid;
  attr.gid = 0x1234;
  EXPECT(wii_nand_set_attr(nand, &ios, "/open/hers/empty", &attr), 0);
  EXPECT(wii_nand_get_attr(nand, &ios, "/open/hers/empty", &attr), 0);
  CHECK(attr.uid == bob.uid && attr.gid == 0x1234, "IOS gave an empty file away");
  EXPECT(wii_nand_open(nand, &alice, "/open/hers/empty", RO), WII_FS_EACCESS);
  EXPECT(wii_nand_open(nand, &bob, "/open/hers/empty", RW), 0);
  EXPECT(wii_nand_close(nand, "/open/hers/empty"), 0);
  /* A directory can be given away with things in it. */
  EXPECT(wii_nand_get_attr(nand, &ios, "/open/hers", &attr), 0);
  attr.uid = carol.uid;
  attr.gid = carol.gid;
  EXPECT(wii_nand_set_attr(nand, &ios, "/open/hers", &attr), 0);
  EXPECT(wii_nand_create_file(nand, &carol, "/open/hers/c", 0, RW, 0, 0), 0);
  EXPECT(wii_nand_create_file(nand, &alice, "/open/hers/a", 0, RW, 0, 0), WII_FS_EACCESS);
  EXPECT(wii_nand_set_attr(nand, &ios, "/open/none", &attr), WII_FS_ENOENT);

  /* Moving needs write on both directories. */
  EXPECT(wii_nand_create_dir(nand, &bob, "/open/his", 0, RW, 0, 0), 0);
  EXPECT(wii_nand_create_file(nand, &bob, "/open/his/b", 0, RW, 0, 0), 0);
  EXPECT(wii_nand_rename(nand, &bob, "/open/his/b", "/open/hers/b"), WII_FS_EACCESS);
  EXPECT(wii_nand_rename(nand, &carol, "/open/his/b", "/open/hers/b"), WII_FS_EACCESS);
  EXPECT(wii_nand_rename(nand, &bob, "/open/his/b", "/open/b"), 0);
  EXPECT(wii_nand_rename(nand, &ios, "/open/b", "/open/hers/b"), 0);

  /* How much a directory uses is nobody's secret (the SDK asks it of
   * directories no game can read), and nor is how full the chip is. */
  EXPECT(wii_nand_usage(nand, &alice, "/open/his", &count, &count), 0);

  /* Only IOS formats. */
  EXPECT(wii_nand_wipe(nand, &alice), WII_FS_EACCESS);
  EXPECT(wii_nand_read_dir(nand, &ios, "/", NULL, 0, &count), 0);
  CHECK(count == 1, "and a refused format removes nothing");

  /* With the checks off everyone is IOS. */
  wii_nand_enforce_permissions(nand, 0);
  EXPECT(wii_nand_create_dir(nand, &alice, "/mine", 0, RW, 0, 0), 0);
  EXPECT(wii_nand_open(nand, &alice, "/open/hers/empty", RW), 0);
  EXPECT(wii_nand_close(nand, "/open/hers/empty"), 0);
  EXPECT(wii_nand_read_dir(nand, &carol, "/", NULL, 0, &count), 0);
  EXPECT(wii_nand_get_attr(nand, &ios, "/open/hers/empty", &attr), 0);
  attr.uid = carol.uid;
  EXPECT(wii_nand_set_attr(nand, &alice, "/open/hers/empty", &attr), 0);
  EXPECT(wii_nand_wipe(nand, &alice), 0);
  EXPECT(wii_nand_read_dir(nand, &ios, "/", NULL, 0, &count), 0);
  CHECK(count == 0, "a format leaves the root and nothing else (%u)", (unsigned)count);
  wii_nand_enforce_permissions(nand, 1);
  EXPECT(wii_nand_create_dir(nand, &alice, "/mine", 0, RW, 0, 0), WII_FS_EACCESS);
  wii_nand_destroy(nand);
}

static void test_names(void) {
  WiiNand *nand = wii_nand_create();
  char names[8][13];
  uint32_t count = 0, clusters = 0, inodes = 0;
  WiiNandStats stats;
  const uint8_t *data;
  uint32_t size;
  static uint8_t block[0x4001];

  EXPECT(wii_nand_create_dir(nand, &ios, "/d", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &ios, "/d/first", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/d/second", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &ios, "/d/third.bin", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &ios, "/d/second/in", 0, RW, RW, RW), 0);

  /* A directory lists its newest entry first. */
  EXPECT(wii_nand_read_dir(nand, &ios, "/d", NULL, 0, &count), 0);
  CHECK(count == 3, "three entries (%u)", (unsigned)count);
  memset(names, 0x55, sizeof(names));
  EXPECT(wii_nand_read_dir(nand, &ios, "/d", names[0], 8, &count), 0);
  CHECK(count == 3 && !strcmp(names[0], "third.bin") && !strcmp(names[1], "second") && !strcmp(names[2], "first"),
        "newest first: %s %s %s", names[0], names[1], names[2]);
  CHECK(names[2][5] == 0 && names[2][12] == 0 && names[0][9] == 0 && names[0][12] == 0, "names are padded with zeroes");
  CHECK((uint8_t)names[3][0] == 0x55, "and nothing past the last is touched");
  /* Room for fewer than there are. */
  memset(names, 0x55, sizeof(names));
  EXPECT(wii_nand_read_dir(nand, &ios, "/d", names[0], 2, &count), 0);
  CHECK(count == 2 && !strcmp(names[1], "second") && (uint8_t)names[2][0] == 0x55, "only as many as fit (%u)",
        (unsigned)count);
  EXPECT(wii_nand_read_dir(nand, &ios, "/d", names[0], 0, &count), 0);
  CHECK(count == 0, "room for none");
  EXPECT(wii_nand_read_dir(nand, &ios, "/d/first", NULL, 0, &count), WII_FS_EINVAL);
  EXPECT(wii_nand_read_dir(nand, &ios, "/d/none", NULL, 0, &count), WII_FS_ENOENT);
  EXPECT(wii_nand_read_dir(nand, &ios, "/d/second", NULL, 0, &count), 0);
  CHECK(count == 1, "the directory inside");

  /* What a directory uses: itself and everything in it, and a cluster for
   * every 16 KB or part of one. */
  EXPECT(wii_nand_usage(nand, &ios, "/d", &clusters, &inodes), 0);
  CHECK(clusters == 0 && inodes == 5, "empty files use entries and no clusters (%u, %u)", (unsigned)clusters,
        (unsigned)inodes);
  EXPECT(wii_nand_put(nand, "/d/first", block, 1), 0);
  EXPECT(wii_nand_put(nand, "/d/third.bin", block, 0x4000), 0);
  EXPECT(wii_nand_put(nand, "/d/second/in", block, 0x4001), 0);
  EXPECT(wii_nand_usage(nand, &ios, "/d", &clusters, &inodes), 0);
  CHECK(clusters == 4 && inodes == 5, "1 + 1 + 2 clusters (%u, %u)", (unsigned)clusters, (unsigned)inodes);
  EXPECT(wii_nand_usage(nand, &ios, "/d/second", &clusters, &inodes), 0);
  CHECK(clusters == 2 && inodes == 2, "the directory inside (%u, %u)", (unsigned)clusters, (unsigned)inodes);
  EXPECT(wii_nand_usage(nand, &ios, "/", &clusters, &inodes), 0);
  CHECK(clusters == 4 && inodes == 6, "the whole filesystem (%u, %u)", (unsigned)clusters, (unsigned)inodes);
  EXPECT(wii_nand_usage(nand, &ios, "/d/first", &clusters, &inodes), WII_FS_EINVAL);
  EXPECT(wii_nand_usage(nand, &ios, "/none", &clusters, &inodes), WII_FS_ENOENT);

  /* The chip's figures add up to the chip. */
  wii_nand_stats(nand, &stats);
  CHECK(stats.cluster_size == 0x4000, "clusters are 16 KB");
  CHECK(stats.used_clusters == 4 && stats.used_inodes == 6, "what is used (%u, %u)", (unsigned)stats.used_clusters,
        (unsigned)stats.used_inodes);
  CHECK(stats.free_clusters + stats.used_clusters + stats.bad_clusters + stats.reserved_clusters == 0x7ec0u,
        "free, used, bad and reserved are the 0x7ec0 clusters the filesystem counts");
  CHECK(stats.reserved_clusters == 0x300u && stats.bad_clusters == 0u, "of which it holds 0x300 back, and none are bad");
  /* With the boot stages' sixty-four and the superblocks' two hundred
   * and fifty-six, that is the chip. */
  CHECK((uint64_t)(0x7ec0u + 0x40u + 0x100u) * stats.cluster_size == 512u * 1024u * 1024u, "which is 512 MB");
  CHECK(stats.free_inodes + stats.used_inodes == WII_NAND_INODES, "and the table's entries are all accounted for");

  /* Renaming. A file moves and keeps its name. */
  EXPECT(wii_nand_rename(nand, &ios, "/d/first", "/d/renamed"), WII_FS_EINVAL);
  EXPECT(wii_nand_rename(nand, &ios, "/d/first", "/d/second/First"), WII_FS_EINVAL);
  EXPECT(wii_nand_rename(nand, &ios, "/d/first", "/d/first"), 0);
  EXPECT(wii_nand_rename(nand, &ios, "/d/first", "/d/second/first"), 0);
  EXPECT(wii_nand_get(nand, "/d/second/first", &data, &size), 0);
  CHECK(size == 1, "it arrived with its contents");
  EXPECT(wii_nand_get(nand, "/d/first", &data, &size), WII_FS_ENOENT);
  EXPECT(wii_nand_read_dir(nand, &ios, "/d/second", names[0], 8, &count), 0);
  CHECK(count == 2 && !strcmp(names[0], "first"), "and is the newest entry where it went");
  EXPECT(wii_nand_rename(nand, &ios, "/d/none", "/d/second/none"), WII_FS_ENOENT);
  EXPECT(wii_nand_rename(nand, &ios, "/d/third.bin", "/nowhere/third.bin"), WII_FS_ENOENT);
  EXPECT(wii_nand_rename(nand, &ios, "/nowhere/x", "/d/x"), WII_FS_ENOENT);
  EXPECT(wii_nand_rename(nand, &ios, "/", "/d/x"), WII_FS_EINVAL);
  EXPECT(wii_nand_rename(nand, &ios, "/d", "/"), WII_FS_EINVAL);
  /* Onto a file that is there: it is replaced. This is every save. */
  EXPECT(wii_nand_create_file(nand, &ios, "/d/first", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_put(nand, "/d/first", "new save", 8), 0);
  wii_nand_stats(nand, &stats);
  EXPECT(wii_nand_rename(nand, &ios, "/d/first", "/d/second/first"), 0);
  EXPECT(wii_nand_get(nand, "/d/second/first", &data, &size), 0);
  CHECK(size == 8 && memcmp(data, "new save", 8) == 0, "the new file is where the old one was");
  EXPECT(wii_nand_read_dir(nand, &ios, "/d/second", NULL, 0, &count), 0);
  CHECK(count == 2, "and there is still one of it");
  {
    WiiNandStats now;
    wii_nand_stats(nand, &now);
    CHECK(now.used_inodes == stats.used_inodes - 1u && now.used_clusters == stats.used_clusters - 1u,
          "the replaced file's entry and cluster came back");
  }
  /* A file onto a directory, and the other way. */
  EXPECT(wii_nand_create_dir(nand, &ios, "/d/in", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_rename(nand, &ios, "/d/second/in", "/d/in"), WII_FS_EINVAL);
  EXPECT(wii_nand_rename(nand, &ios, "/d/in", "/d/second/in"), WII_FS_EINVAL);
  /* A directory may change its name, takes what is in it, and replaces a
   * directory that is there. */
  EXPECT(wii_nand_rename(nand, &ios, "/d/second", "/d/moved"), 0);
  EXPECT(wii_nand_get(nand, "/d/moved/first", &data, &size), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/d/target", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &ios, "/d/target/old", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_rename(nand, &ios, "/d/moved", "/d/target"), 0);
  EXPECT(wii_nand_get(nand, "/d/target/first", &data, &size), 0);
  EXPECT(wii_nand_get(nand, "/d/target/old", &data, &size), WII_FS_ENOENT);
  /* Not into itself, and not deeper than directories go. */
  EXPECT(wii_nand_rename(nand, &ios, "/d", "/d/target/d"), WII_FS_EINVAL);
  EXPECT(wii_nand_rename(nand, &ios, "/d/target", "/d/target/again"), WII_FS_EINVAL);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4/5", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4/5/6", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/1/2/3/4/5/6/7", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/d/target/deeper", 0, RW, RW, RW), 0);
  /* /d/target holds one level; under seven it would be the eighth and its
   * child the ninth. */
  EXPECT(wii_nand_rename(nand, &ios, "/d/target", "/1/2/3/4/5/6/7/8"), WII_FS_EDIRDEPTH);
  EXPECT(wii_nand_rename(nand, &ios, "/d/target", "/1/2/3/4/5/6/t"), 0);
  EXPECT(wii_nand_get(nand, "/1/2/3/4/5/6/t/first", &data, &size), 0);

  /* Deleting takes everything underneath. */
  wii_nand_stats(nand, &stats);
  EXPECT(wii_nand_delete(nand, &ios, "/"), WII_FS_EINVAL);
  EXPECT(wii_nand_delete(nand, &ios, "/none"), WII_FS_ENOENT);
  EXPECT(wii_nand_delete(nand, &ios, "/1"), 0);
  EXPECT(wii_nand_get(nand, "/1/2/3/4/5/6/t/first", &data, &size), WII_FS_ENOENT);
  {
    WiiNandStats now;
    wii_nand_stats(nand, &now);
    CHECK(stats.used_inodes - now.used_inodes == 11u, "eleven entries went (%u)",
          (unsigned)(stats.used_inodes - now.used_inodes));
    CHECK(stats.used_clusters - now.used_clusters == 3u, "and their clusters (%u)",
          (unsigned)(stats.used_clusters - now.used_clusters));
  }
  wii_nand_destroy(nand);
}

/* The file table has 6,143 entries, the root among them. */
static void test_table_full(void) {
  WiiNand *nand = wii_nand_create();
  WiiNandStats stats;
  unsigned i, made = 0;
  int32_t result = 0;
  EXPECT(wii_nand_create_dir(nand, &ios, "/d", 0, RW, RW, RW), 0);
  for (i = 0; i < 7000u; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/d/%05u", i);
    result = wii_nand_create_file(nand, &ios, path, 0, RW, RW, RW);
    if (result != 0) break;
    made++;
  }
  CHECK(result == WII_FS_ENFILE, "the table fills (%d)", (int)result);
  CHECK(made == WII_NAND_INODES - 2u, "after %u files beside the root and one directory", made);
  EXPECT(wii_nand_create_dir(nand, &ios, "/e", 0, RW, RW, RW), WII_FS_ENFILE);
  wii_nand_stats(nand, &stats);
  CHECK(stats.free_inodes == 0 && stats.used_inodes == WII_NAND_INODES, "none are free");
  EXPECT(wii_nand_delete(nand, &ios, "/d/00000"), 0);
  EXPECT(wii_nand_create_dir(nand, &ios, "/e", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_delete(nand, &ios, "/d"), 0);
  wii_nand_stats(nand, &stats);
  CHECK(stats.used_inodes == 2, "and deleting gives them back (%u)", (unsigned)stats.used_inodes);
  wii_nand_destroy(nand);
}

/* ------------------------------------------------------------------ */
/* SYSCONF and setting.txt, read by readers of our own                 */

static uint32_t be16_at(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32_at(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct ConfItem {
  char name[33];
  unsigned type;
  uint32_t length;
  const uint8_t *value;
  uint32_t slot; /* where in the file this item's offset is kept */
} ConfItem;

/* Walk a SYSCONF the way the SDK's parser does, which is the strict way:
 * the first item straight after the offsets, each item straight after the
 * one before, the extra offset where the last one ends. Returns the number
 * of items, or -1. */
static int conf_parse(const uint8_t *file, ConfItem *items, unsigned max, uint32_t *end) {
  unsigned count, i;
  uint32_t at;
  if (memcmp(file, "SCv0", 4) != 0 || memcmp(file + 0x3ffc, "SCed", 4) != 0) return -1;
  count = be16_at(file + 4);
  if (count > max) return -1;
  at = 6u + 2u * (count + 1u);
  for (i = 0; i < count; i++) {
    ConfItem *item = &items[i];
    unsigned name_length;
    if (be16_at(file + 6u + 2u * i) != at || at >= 0x3ffcu) return -1;
    item->slot = 6u + 2u * i;
    item->type = file[at] >> 5;
    name_length = (file[at] & 0x1fu) + 1u;
    memcpy(item->name, file + at + 1u, name_length);
    item->name[name_length] = 0;
    at += 1u + name_length;
    switch (item->type) {
      case 1:
        item->length = be16_at(file + at) + 1u;
        at += 2u;
        break;
      case 2:
        item->length = file[at] + 1u;
        at += 1u;
        break;
      case 3:
      case 7: item->length = 1; break;
      case 4: item->length = 2; break;
      case 5: item->length = 4; break;
      case 6: item->length = 8; break;
      default: return -1;
    }
    item->value = file + at;
    at += item->length;
  }
  if (be16_at(file + 6u + 2u * count) != at) return -1;
  *end = at;
  return (int)count;
}

static const ConfItem *conf_item(const ConfItem *items, int count, const char *name) {
  int i;
  for (i = 0; i < count; i++) {
    if (!strcmp(items[i].name, name)) return &items[i];
  }
  return NULL;
}

/* The settings the system software has numbers for, first number first
 * (WiiBrew's table of the file's last 78 bytes, read from its end). */
static const char *const conf_numbered[39] = {
    "IPL.CB",   "IPL.AR",   "IPL.ARN",  "IPL.CD",   "IPL.CD2",  "IPL.DH",   "IPL.E60",  "IPL.EULA",
    "IPL.FRC",  "IPL.IDL",  "IPL.INC",  "IPL.LNG",  "IPL.NIK",  "IPL.PC",   "IPL.PGS",  "IPL.SSV",
    "IPL.SADR", "IPL.SND",  "IPL.UPT",  "NET.CNF",  "NET.CTPC", "NET.PROF", "NET.WCPC", "NET.WCFG",
    "DEV.BTM",  "DEV.VIM",  "DEV.CTC",  "DEV.DSM",  "BT.DINF",  "BT.CDIF",  "BT.SENS",  "BT.SPKV",
    "BT.MOT",   "BT.BAR",   "DVD.CNF",  "WWW.RST",  "IPL.TID",  "MPLS.MOVIE", "IPL.APD",
};

/* What the file must hold: each item's type and size, which are a real
 * file's, and the value the default settings give it. Arrays are checked
 * separately. */
static const struct {
  const char *name;
  unsigned type;
  uint32_t length;
  uint32_t value;
} conf_expected[] = {
    {"IPL.CB", 5, 4, 0},       {"IPL.AR", 3, 1, 0},     {"IPL.CD", 7, 1, 1},      {"IPL.CD2", 7, 1, 1},
    {"IPL.DH", 3, 1, 0},       {"IPL.E60", 3, 1, 1},    {"IPL.EULA", 7, 1, 1},    {"IPL.FRC", 5, 4, 0x28},
    {"IPL.IDL", 2, 2, 0},      {"IPL.INC", 5, 4, 8},    {"IPL.LNG", 3, 1, 1},     {"IPL.NIK", 2, 0x16, 0},
    {"IPL.PC", 2, 0x4a, 0},    {"IPL.PGS", 3, 1, 0},    {"IPL.SSV", 3, 1, 1},     {"IPL.SADR", 1, 0x1008, 0},
    {"IPL.SND", 3, 1, 1},      {"IPL.UPT", 3, 1, 2},    {"NET.CTPC", 5, 4, 0},    {"NET.WCPC", 7, 1, 0},
    {"NET.WCFG", 5, 4, 0},     {"DEV.BTM", 3, 1, 0},    {"DEV.VIM", 3, 1, 0},     {"DEV.CTC", 3, 1, 1},
    {"DEV.DSM", 3, 1, 0},      {"BT.DINF", 1, 0x461, 0}, {"BT.CDIF", 1, 0x205, 0}, {"BT.SENS", 5, 4, 3},
    {"BT.SPKV", 3, 1, 0x58},   {"BT.MOT", 3, 1, 1},     {"BT.BAR", 3, 1, 0},      {"DVD.CNF", 3, 1, 0},
    {"WWW.RST", 3, 1, 0},      {"MPLS.MOVIE", 3, 1, 1},
};
#define CONF_EXPECTED (sizeof(conf_expected) / sizeof(conf_expected[0]))

/* setting.txt as the SDK reads it: a stored zero is nothing; anything
 * else is a character under the key; a name matches whatever its case. */
static int txt_value(const uint8_t *file, const char *name, char *out, size_t out_size) {
  uint32_t key = 0x73b5dbfau;
  size_t i, matched = 0, length = 0;
  int found = 0;
  for (i = 0; i < 0x100u; i++, key = (key << 1) | (key >> 31)) {
    uint8_t c = file[i];
    if (!c) continue;
    c ^= (uint8_t)key;
    if (found) {
      if (c == '\r' || c == '\n') {
        out[length] = 0;
        return 1;
      }
      if (length + 1u < out_size) out[length++] = (char)c;
    } else if (!name[matched] && c == '=') {
      found = 1;
    } else if (name[matched] && ((c ^ (uint8_t)name[matched]) & 0xdfu) == 0) {
      matched++;
    } else {
      matched = 0;
    }
  }
  return 0;
}

static void test_settings_files(void) {
  static uint8_t sysconf[WII_SYSCONF_SIZE], again[WII_SYSCONF_SIZE];
  uint8_t text[WII_SETTING_TXT_SIZE], plain[WII_SETTING_TXT_SIZE + 1u];
  static ConfItem items[64];
  WiiSettings settings;
  uint32_t end = 0;
  int count;
  unsigned i;
  char value[32];

  wii_settings_default(&settings);
  CHECK(settings.region == WII_REGION_USA && settings.video == WII_VIDEO_NTSC &&
            settings.language == WII_LANGUAGE_ENGLISH && !settings.widescreen && settings.sound == WII_SOUND_STEREO &&
            !settings.progressive && settings.sixty_hertz && !settings.sensor_bar_top && !strcmp(settings.nickname, "Wii"),
        "the defaults are a North American console's");
  memset(sysconf, 0xee, sizeof(sysconf));
  EXPECT(wii_sysconf_build(&settings, sysconf), 0);

  count = conf_parse(sysconf, items, 64, &end);
  CHECK(count == (int)CONF_EXPECTED, "the file parses the strict way, with %d items", count);
  if (count < 0) return;
  /* An early SDK's table at the end starts lower than the newest one's;
   * the items stop long before either. */
  CHECK(end <= 0x3ffcu - 2u * 39u, "the items end at %04x, before the table", (unsigned)end);
  for (i = end; i < 0x3ffcu - 2u * 39u; i++) {
    if (sysconf[i]) break;
  }
  CHECK(i == 0x3ffcu - 2u * 39u, "and the space between is zeroes");

  for (i = 0; i < CONF_EXPECTED; i++) {
    const ConfItem *item = conf_item(items, count, conf_expected[i].name);
    uint32_t got = 0;
    CHECK(item != NULL, "%s is there", conf_expected[i].name);
    if (!item) continue;
    CHECK(item->type == conf_expected[i].type && item->length == conf_expected[i].length,
          "%s is type %u, %u bytes (want %u, %u)", item->name, item->type, (unsigned)item->length, conf_expected[i].type,
          (unsigned)conf_expected[i].length);
    if (item->type >= 3u) {
      got = item->length == 1 ? item->value[0] : item->length == 2 ? be16_at(item->value) : be32_at(item->value);
      CHECK(got == conf_expected[i].value, "%s is %u (want %u)", item->name, (unsigned)got,
            (unsigned)conf_expected[i].value);
    }
    /* The reader the runtime offers agrees with this one. */
    {
      unsigned type = 0;
      uint32_t length = 0;
      const uint8_t *found = wii_sysconf_find(sysconf, sizeof(sysconf), item->name, &type, &length);
      CHECK(found == item->value && type == item->type && length == item->length, "wii_sysconf_find(%s)", item->name);
    }
  }
  CHECK(wii_sysconf_find(sysconf, sizeof(sysconf), "NET.PROF", NULL, NULL) == NULL, "no network profile");
  CHECK(wii_sysconf_find(sysconf, sizeof(sysconf), "IPL", NULL, NULL) == NULL, "a name's beginning is not the name");
  CHECK(wii_sysconf_find(sysconf, 7, "IPL.AR", NULL, NULL) == NULL, "a short buffer is not a file");

  /* The table at the end: each setting's entry, counted back from the
   * last, is where that setting's offset is - or zero. */
  for (i = 0; i < 39u; i++) {
    const ConfItem *item = conf_item(items, count, conf_numbered[i]);
    const uint32_t entry = be16_at(sysconf + 0x3ffau - 2u * i);
    CHECK(entry == (item ? item->slot : 0u), "the table's entry for %s is %04x", conf_numbered[i], (unsigned)entry);
    if (item) CHECK(be16_at(sysconf + entry) + 1u + strlen(item->name) + (item->type == 1 ? 2u : item->type == 2 ? 1u : 0u) ==
                        (uint32_t)(item->value - sysconf),
                    "and leads to it");
  }

  /* The arrays. */
  {
    const ConfItem *nik = conf_item(items, count, "IPL.NIK");
    const ConfItem *dinf = conf_item(items, count, "BT.DINF");
    const ConfItem *sadr = conf_item(items, count, "IPL.SADR");
    const ConfItem *pc = conf_item(items, count, "IPL.PC");
    const ConfItem *cdif = conf_item(items, count, "BT.CDIF");
    unsigned zero = 0;
    if (nik) {
      CHECK(nik->value[0] == 0 && nik->value[1] == 'W' && nik->value[3] == 'i' && nik->value[5] == 'i' &&
                nik->value[6] == 0 && nik->value[7] == 0,
            "the name is UTF-16, big end first");
      CHECK(nik->value[0x14] == 0 && nik->value[0x15] == 3, "and three characters long");
    }
    if (sadr) CHECK(sadr->value[0] == 49, "the country is the United States (%u)", sadr->value[0]);
    if (pc) CHECK((pc->value[0] & 0x80u) == 0, "parental controls are off");
    if (cdif) {
      for (i = 0; i < cdif->length; i++) zero |= cdif->value[i];
      CHECK(zero == 0, "no guest remotes");
    }
    if (dinf) {
      /* Four remotes, each the address wii_remote.h gives - most
       * significant byte first, as printed - and the name a remote gives. */
      CHECK(dinf->value[0] == WII_REMOTES, "%u remotes are registered", dinf->value[0]);
      for (i = 0; i < 16u; i++) {
        const uint8_t *entry = dinf->value + 1u + i * 70u;
        uint8_t address[6];
        unsigned b;
        zero = 0;
        if (i < WII_REMOTES) {
          wii_remote_address(i, address);
          CHECK(memcmp(entry, address, 6) == 0, "remote %u's address is %02x:%02x:%02x:%02x:%02x:%02x", i, entry[0],
                entry[1], entry[2], entry[3], entry[4], entry[5]);
          CHECK(entry[0] == 0x00 && entry[1] == 0x1e && entry[5] == 0x10u + i, "which starts 00:1e and ends %02x",
                0x10u + i);
          CHECK(strcmp((const char *)entry + 6, WII_REMOTE_NAME) == 0, "remote %u's name", i);
          for (b = 6u + (unsigned)strlen(WII_REMOTE_NAME); b < 70u; b++) zero |= entry[b];
        } else {
          for (b = 0; b < 70u; b++) zero |= entry[b];
        }
        CHECK(zero == 0, "entry %u has nothing else in it", i);
      }
    }
  }

  /* The same settings give the same bytes, and these bytes are the ones
   * recorded: a change to the file is a change somebody meant. */
  EXPECT(wii_sysconf_build(&settings, again), 0);
  CHECK(memcmp(sysconf, again, sizeof(sysconf)) == 0, "building twice gives one file");
  {
    const uint64_t hash = fnv(FNV_START, sysconf, sizeof(sysconf));
    CHECK(hash == SYSCONF_HASH, "SYSCONF's hash is %016llx", (unsigned long long)hash);
  }

  /* setting.txt. */
  memset(text, 0xee, sizeof(text));
  EXPECT(wii_setting_txt_build(&settings, text), 0);
  {
    static const char want[] =
        "AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\nSERNO=000000000\r\nVIDEO=NTSC\r\nGAME=US\r\n";
    const size_t length = sizeof(want) - 1u;
    uint32_t key = 0x73b5dbfau;
    unsigned stored_zero = 0, tail = 0;
    for (i = 0; i < 0x100u; i++) {
      plain[i] = (uint8_t)(text[i] ^ (uint8_t)key);
      key = (key << 1) | (key >> 31);
      if (i < length && !text[i]) stored_zero++;
      if (i >= length) tail |= text[i];
    }
    plain[0x100] = 0;
    CHECK(memcmp(plain, want, length) == 0, "the text under the key is a console's");
    CHECK(stored_zero == 0, "no character is stored as zero");
    CHECK(tail == 0, "and what follows the text is plain zeroes");
    /* The runtime's own routine is the same key, from any offset. */
    memcpy(plain, text, 0x100);
    wii_setting_txt_crypt(plain, 40, 0);
    wii_setting_txt_crypt(plain + 40, length - 40u, 40);
    CHECK(memcmp(plain, want, length) == 0, "wii_setting_txt_crypt undoes it, in two pieces");
  }
  CHECK(txt_value(text, "AREA", value, sizeof(value)) && !strcmp(value, "USA"), "AREA is %s", value);
  CHECK(txt_value(text, "MODEL", value, sizeof(value)) && !strcmp(value, "RVL-001(USA)"), "MODEL is %s", value);
  CHECK(txt_value(text, "VIDEO", value, sizeof(value)) && !strcmp(value, "NTSC"), "VIDEO is %s", value);
  CHECK(txt_value(text, "GAME", value, sizeof(value)) && !strcmp(value, "US"), "GAME is %s", value);
  CHECK(txt_value(text, "SERNO", value, sizeof(value)) && !strcmp(value, "000000000"), "the serial is nobody's (%s)",
        value);
  CHECK(!txt_value(text, "NOPE", value, sizeof(value)), "a line that is not there");
  {
    const uint64_t hash = fnv(FNV_START, text, sizeof(text));
    CHECK(hash == SETTING_TXT_HASH, "setting.txt's hash is %016llx", (unsigned long long)hash);
  }

  /* Every region and video standard builds, and none stores a zero. */
  {
    static const struct {
      uint8_t region, video;
      const char *area, *game, *video_name, *code;
    } consoles[] = {
        {WII_REGION_JPN, WII_VIDEO_NTSC, "JPN", "JP", "NTSC", "LJ"},
        {WII_REGION_USA, WII_VIDEO_NTSC, "USA", "US", "NTSC", "LU"},
        {WII_REGION_USA, WII_VIDEO_MPAL, "USA", "US", "MPAL", "LU"},
        {WII_REGION_EUR, WII_VIDEO_PAL, "EUR", "EU", "PAL", "LEH"},
        {WII_REGION_KOR, WII_VIDEO_NTSC, "KOR", "KR", "NTSC", "LKM"},
    };
    for (i = 0; i < sizeof(consoles) / sizeof(consoles[0]); i++) {
      WiiSettings s;
      wii_settings_default(&s);
      s.region = consoles[i].region;
      s.video = consoles[i].video;
      EXPECT(wii_setting_txt_build(&s, text), 0);
      CHECK(txt_value(text, "AREA", value, sizeof(value)) && !strcmp(value, consoles[i].area), "AREA %s", value);
      CHECK(txt_value(text, "GAME", value, sizeof(value)) && !strcmp(value, consoles[i].game), "GAME %s", value);
      CHECK(txt_value(text, "VIDEO", value, sizeof(value)) && !strcmp(value, consoles[i].video_name), "VIDEO %s", value);
      CHECK(txt_value(text, "CODE", value, sizeof(value)) && !strcmp(value, consoles[i].code), "CODE %s", value);
      EXPECT(wii_sysconf_build(&s, again), 0);
      CHECK(conf_parse(again, items, 64, &end) == (int)CONF_EXPECTED, "its SYSCONF parses");
    }
  }

  /* Settings a frontend changed reach the file. */
  {
    WiiSettings s;
    const uint8_t *found;
    wii_settings_default(&s);
    s.widescreen = 1;
    s.language = WII_LANGUAGE_FRENCH;
    s.sound = WII_SOUND_SURROUND;
    s.progressive = 1;
    s.sensor_bar_top = 1;
    s.display_offset = -4;
    s.counter_bias = 0x12345678u;
    strcpy(s.nickname, "Ten chars!");
    EXPECT(wii_sysconf_build(&s, again), 0);
    found = wii_sysconf_find(again, sizeof(again), "IPL.AR", NULL, NULL);
    CHECK(found && found[0] == 1, "16:9");
    found = wii_sysconf_find(again, sizeof(again), "IPL.LNG", NULL, NULL);
    CHECK(found && found[0] == WII_LANGUAGE_FRENCH, "French");
    found = wii_sysconf_find(again, sizeof(again), "IPL.SND", NULL, NULL);
    CHECK(found && found[0] == 2, "surround");
    found = wii_sysconf_find(again, sizeof(again), "IPL.PGS", NULL, NULL);
    CHECK(found && found[0] == 1, "progressive");
    found = wii_sysconf_find(again, sizeof(again), "BT.BAR", NULL, NULL);
    CHECK(found && found[0] == 1, "the bar above the screen");
    found = wii_sysconf_find(again, sizeof(again), "IPL.DH", NULL, NULL);
    CHECK(found && found[0] == 0xfc, "a negative offset is a signed byte");
    found = wii_sysconf_find(again, sizeof(again), "IPL.CB", NULL, NULL);
    CHECK(found && be32_at(found) == 0x12345678u, "the clock's bias");
    found = wii_sysconf_find(again, sizeof(again), "IPL.NIK", NULL, NULL);
    CHECK(found && found[19] == '!' && found[0x15] == 10, "a ten-character name");
  }

  /* Settings that cannot be expressed are refused, not guessed at. */
  {
    WiiSettings s;
    wii_settings_default(&s);
    s.nickname[0] = 0;
    EXPECT(wii_sysconf_build(&s, again), -1);
    wii_settings_default(&s);
    s.language = 10;
    EXPECT(wii_sysconf_build(&s, again), -1);
    wii_settings_default(&s);
    s.region = 4;
    EXPECT(wii_setting_txt_build(&s, text), -1);
    wii_settings_default(&s);
    s.sensitivity = 0;
    EXPECT(wii_sysconf_build(&s, again), -1);
    wii_settings_default(&s);
    s.nickname[1] = '\xe9';
    EXPECT(wii_sysconf_build(&s, again), -1);
  }
}

/* ------------------------------------------------------------------ */

static void expect_entry(WiiNand *nand, const char *path, int directory, uint32_t uid, uint16_t gid, uint8_t owner,
                         uint8_t group, uint8_t other) {
  WiiNandAttr attr;
  memset(&attr, 0xff, sizeof(attr));
  if (wii_nand_get_attr(nand, &ios, path, &attr) != 0) {
    CHECK(0, "%s is missing", path);
    return;
  }
  CHECK(attr.is_directory == directory && attr.uid == uid && attr.gid == gid && attr.owner == owner &&
            attr.group == group && attr.other == other,
        "%s is %s %x:%x %u%u%u (want %s %x:%x %u%u%u)", path, attr.is_directory ? "dir" : "file", (unsigned)attr.uid,
        attr.gid, attr.owner, attr.group, attr.other, directory ? "dir" : "file", (unsigned)uid, gid, owner, group, other);
}

static void test_format(void) {
  WiiNand *nand = wii_nand_create();
  /* A game off a disc: a user number of its own, its maker as its group. */
  static const WiiNandCaller game = {0x1001, 0x3031};
  static const WiiNandCaller stranger = {0x1007, 0x3545};
  char names[16][13];
  uint32_t count = 0, size = 0, position = 0;
  const uint8_t *data = NULL;
  uint64_t first;
  static uint8_t buffer[WII_SYSCONF_SIZE];

  EXPECT(wii_nand_create_file(nand, &ios, "/junk", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_format(nand, NULL), 0);
  EXPECT(wii_nand_get(nand, "/junk", &data, &size), WII_FS_ENOENT);

  EXPECT(wii_nand_read_dir(nand, &ios, "/", names[0], 16, &count), 0);
  CHECK(count == 8, "the root holds eight directories (%u)", (unsigned)count);
  expect_entry(nand, "/sys", 1, 0, 0, RW, RW, 0);
  expect_entry(nand, "/sys/uid.sys", 0, 0, 0, RW, RW, 0);
  expect_entry(nand, "/ticket", 1, 0, 0, RW, RW, 0);
  expect_entry(nand, "/title", 1, 0, 0, RW, RW, RO);
  expect_entry(nand, "/title/00000001", 1, 0, 0, RW, RW, RO);
  expect_entry(nand, "/title/00000001/00000002", 1, 0, 0, RW, RW, RO);
  expect_entry(nand, "/title/00000001/00000002/content", 1, 0, 0, RW, RW, 0);
  expect_entry(nand, "/title/00000001/00000002/data", 1, 0x1000, 1, RW, 0, 0);
  expect_entry(nand, "/title/00000001/00000002/data/setting.txt", 0, 0x1000, 1, RW, RO, RO);
  expect_entry(nand, "/shared1", 1, 0, 0, RW, RW, 0);
  expect_entry(nand, "/shared2", 1, 0, 0, RW, RW, RW);
  expect_entry(nand, "/shared2/sys", 1, 0, 0, RW, RW, RW);
  expect_entry(nand, "/shared2/sys/SYSCONF", 0, 0, 0, RW, RW, RW);
  expect_entry(nand, "/tmp", 1, 0, 0, RW, RW, RW);
  expect_entry(nand, "/import", 1, 0, 0, RW, RW, 0);
  expect_entry(nand, "/meta", 1, 0, 0, RW, RW, RW);

  /* The two files are the ones the builders make. */
  EXPECT(wii_nand_get(nand, "/shared2/sys/SYSCONF", &data, &size), 0);
  CHECK(size == WII_SYSCONF_SIZE && fnv(FNV_START, data, size) == SYSCONF_HASH,
        "SYSCONF is the default one");
  EXPECT(wii_nand_get(nand, "/title/00000001/00000002/data/setting.txt", &data, &size), 0);
  CHECK(size == WII_SETTING_TXT_SIZE && fnv(FNV_START, data, size) == SETTING_TXT_HASH,
        "setting.txt is the default one");

  /* The table of user numbers starts with the system menu at 0x1000. */
  EXPECT(wii_nand_get(nand, "/sys/uid.sys", &data, &size), 0);
  {
    static const uint8_t menu[12] = {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0x10, 0};
    CHECK(size == 12 && memcmp(data, menu, 12) == 0, "uid.sys holds the system menu");
  }
  CHECK(wii_nand_title_uid(nand, 0x0000000100000002ull, 0) == 0x1000, "which is user 0x1000");
  CHECK(wii_nand_title_uid(nand, 0x0001000052535045ull, 0) == 0, "a title nobody has seen has no number");

  /* What a game finds: it can read the settings, save the remotes it
   * paired, use /tmp - and not touch what is IOS's. */
  EXPECT(wii_nand_open(nand, &stranger, "/shared2/sys/SYSCONF", RW), 0);
  EXPECT(wii_nand_read(nand, "/shared2/sys/SYSCONF", RW, &position, buffer, WII_SYSCONF_SIZE), WII_SYSCONF_SIZE);
  CHECK(memcmp(buffer, "SCv0", 4) == 0, "SYSCONF reads");
  EXPECT(wii_nand_close(nand, "/shared2/sys/SYSCONF"), 0);
  EXPECT(wii_nand_open(nand, &stranger, "/title/00000001/00000002/data/setting.txt", RO), 0);
  EXPECT(wii_nand_close(nand, "/title/00000001/00000002/data/setting.txt"), 0);
  EXPECT(wii_nand_open(nand, &stranger, "/title/00000001/00000002/data/setting.txt", RW), WII_FS_EACCESS);
  EXPECT(wii_nand_create_dir(nand, &stranger, "/tmp/sys", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &stranger, "/sys/x", 0, RW, RW, RW), WII_FS_EACCESS);
  EXPECT(wii_nand_create_file(nand, &stranger, "/title/x", 0, RW, RW, RW), WII_FS_EACCESS);
  EXPECT(wii_nand_open(nand, &stranger, "/sys/uid.sys", RO), WII_FS_EACCESS);
  /* /shared2 is everyone's: the SDK remakes /shared2/sys itself when it
   * finds it gone, so a game has to be able to. */
  EXPECT(wii_nand_delete(nand, &stranger, "/shared2/sys"), 0);
  EXPECT(wii_nand_create_dir(nand, &stranger, "/shared2/sys", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_format(nand, NULL), 0);

  /* A title's own directories. */
  EXPECT(wii_nand_add_title(nand, 0x0001000052535045ull, 0x3031), 0);
  CHECK(wii_nand_title_uid(nand, 0x0001000052535045ull, 0) == 0x1001, "the first title after the menu is user 0x1001");
  expect_entry(nand, "/title/00010000", 1, 0, 0, RW, RW, RO);
  expect_entry(nand, "/title/00010000/52535045", 1, 0, 0, RW, RW, RO);
  expect_entry(nand, "/title/00010000/52535045/content", 1, 0, 0, RW, RW, 0);
  expect_entry(nand, "/title/00010000/52535045/data", 1, 0x1001, 0x3031, RW, 0, 0);
  EXPECT(wii_nand_create_file(nand, &game, "/title/00010000/52535045/data/save.bin", 0, RW, RW, 0), 0);
  EXPECT(wii_nand_create_file(nand, &stranger, "/title/00010000/52535045/data/x", 0, RW, RW, 0), WII_FS_EACCESS);
  EXPECT(wii_nand_read_dir(nand, &stranger, "/title/00010000/52535045/data", NULL, 0, &count), WII_FS_EACCESS);
  EXPECT(wii_nand_create_file(nand, &game, "/title/00010000/52535045/content/x", 0, RW, RW, 0), WII_FS_EACCESS);
  /* Again changes nothing, and the save is still there. */
  EXPECT(wii_nand_add_title(nand, 0x0001000052535045ull, 0x3031), 0);
  EXPECT(wii_nand_get(nand, "/title/00010000/52535045/data/save.bin", &data, &size), 0);
  EXPECT(wii_nand_get(nand, "/sys/uid.sys", &data, &size), 0);
  CHECK(size == 24, "and the table has two titles (%u bytes)", (unsigned)size);
  EXPECT(wii_nand_add_title(nand, 0x00010000534d4e45ull, 0x3031), 0);
  CHECK(wii_nand_title_uid(nand, 0x00010000534d4e45ull, 0) == 0x1002, "and the next is 0x1002");

  /* Formatting is the same every time. */
  EXPECT(wii_nand_format(nand, NULL), 0);
  first = tree_hash(nand);
  EXPECT(wii_nand_format(nand, NULL), 0);
  CHECK(tree_hash(nand) == first, "two formats give one filesystem");

  /* New settings replace the two files and nothing else. */
  {
    WiiSettings s;
    wii_settings_default(&s);
    s.widescreen = 1;
    s.region = WII_REGION_EUR;
    s.video = WII_VIDEO_PAL;
    EXPECT(wii_nand_put(nand, "/tmp/keep", "x", 1), 0);
    EXPECT(wii_nand_apply_settings(nand, &s), 0);
    EXPECT(wii_nand_get(nand, "/shared2/sys/SYSCONF", &data, &size), 0);
    data = wii_sysconf_find(data, size, "IPL.AR", NULL, NULL);
    CHECK(data && data[0] == 1, "the new SYSCONF says 16:9");
    EXPECT(wii_nand_get(nand, "/tmp/keep", &data, &size), 0);
    expect_entry(nand, "/shared2/sys/SYSCONF", 0, 0, 0, RW, RW, RW);
    expect_entry(nand, "/title/00000001/00000002/data/setting.txt", 0, 0x1000, 1, RW, RO, RO);
    /* Settings that cannot be written leave a console with nothing. */
    s.language = 99;
    EXPECT(wii_nand_format(nand, &s), -1);
    EXPECT(wii_nand_read_dir(nand, &ios, "/", NULL, 0, &count), 0);
    CHECK(count == 0, "a failed format leaves only the root (%u)", (unsigned)count);
  }

  /* /tmp is emptied, and stays. */
  EXPECT(wii_nand_format(nand, NULL), 0);
  EXPECT(wii_nand_create_dir(nand, &game, "/tmp/sys", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_create_file(nand, &game, "/tmp/sys/f", 0, RW, RW, RW), 0);
  wii_nand_clear_tmp(nand);
  EXPECT(wii_nand_read_dir(nand, &ios, "/tmp", NULL, 0, &count), 0);
  CHECK(count == 0, "/tmp is empty");
  wii_nand_destroy(nand);
}

/* ------------------------------------------------------------------ */
/* The file on the host                                                */

/* CRC-32 worked a bit at a time, to make altered files whose checksum is
 * right. */
static uint32_t crc32_slow(const uint8_t *data, size_t size) {
  uint32_t crc = 0xffffffffu;
  size_t i;
  unsigned bit;
  for (i = 0; i < size; i++) {
    crc ^= data[i];
    for (bit = 0; bit < 8u; bit++) crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
  }
  return ~crc;
}

static void put_be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

/* Write `image` with one byte changed and the checksum made right again,
 * and say whether it loads. */
static int loads_altered(const uint8_t *image, size_t size, size_t at, uint8_t value, char *reason, size_t reason_size) {
  const char *path = in_scratch("altered.nand");
  uint8_t *copy = (uint8_t *)malloc(size);
  WiiNand *nand;
  int missing = 1;
  memcpy(copy, image, size);
  copy[at] = value;
  put_be32(copy + 20, crc32_slow(copy + 24, size - 24u));
  write_whole(path, copy, size);
  free(copy);
  nand = wii_nand_load(path, &missing, reason, reason_size);
  CHECK(missing == 0, "a damaged file is not a missing one");
  if (nand) {
    wii_nand_destroy(nand);
    return 1;
  }
  return 0;
}

static void test_host_file(void) {
  char path[512], temporary[520];
  WiiNand *nand = wii_nand_create(), *loaded;
  static const WiiNandCaller game = {0x1001, 0x3031};
  char reason[160];
  int missing = 0;
  uint8_t *image, *second;
  size_t size = 0, second_size = 0;
  uint64_t hash;
  uint32_t position = 0;
  static uint8_t save[70000];
  unsigned i;

  snprintf(path, sizeof(path), "%s", in_scratch("console.nand"));
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  remove(path);
  remove(temporary);
  remove_directory(temporary);
  for (i = 0; i < sizeof(save); i++) save[i] = (uint8_t)(i ^ (i >> 7));

  /* No file, and an empty one, are a console never switched on. */
  loaded = wii_nand_load(path, &missing, reason, sizeof(reason));
  CHECK(!loaded && missing == 1 && reason[0], "no file: %s", reason);
  write_whole(path, save, 0);
  loaded = wii_nand_load(path, &missing, reason, sizeof(reason));
  CHECK(!loaded && missing == 1, "an empty file: %s", reason);
  loaded = wii_nand_load("", &missing, reason, sizeof(reason));
  CHECK(!loaded && missing == 0, "no name: %s", reason);

  /* A console with a game's saves on it. */
  EXPECT(wii_nand_format(nand, NULL), 0);
  EXPECT(wii_nand_add_title(nand, 0x0001000052535045ull, 0x3031), 0);
  EXPECT(wii_nand_create_file(nand, &game, "/title/00010000/52535045/data/save.bin", 0x21, RW, RO, 0), 0);
  EXPECT(wii_nand_open(nand, &game, "/title/00010000/52535045/data/save.bin", RW), 0);
  EXPECT(wii_nand_write(nand, "/title/00010000/52535045/data/save.bin", RW, &position, save, sizeof(save)),
         (int)sizeof(save));
  EXPECT(wii_nand_close(nand, "/title/00010000/52535045/data/save.bin"), 0);
  EXPECT(wii_nand_create_file(nand, &game, "/title/00010000/52535045/data/banner.bin", 0, RW, RW, RO), 0);
  EXPECT(wii_nand_create_dir(nand, &game, "/title/00010000/52535045/data/dir", 0, RW, 0, 0), 0);
  EXPECT(wii_nand_create_file(nand, &game, "/title/00010000/52535045/data/dir/empty", 0, RW, 0, 0), 0);
  EXPECT(wii_nand_create_dir(nand, &game, "/tmp/sys", 0, RW, RW, RW), 0);
  EXPECT(wii_nand_put(nand, "/tmp/sys/scratch", save, 100), 0);

  /* When it changed, by the owner's clock. */
  CHECK(wii_nand_dirty(nand), "a filesystem that has been written to is dirty");
  wii_nand_set_time(nand, 5000);
  CHECK(wii_nand_changed_at(nand) <= 5000, "it changed before now");
  EXPECT(wii_nand_put(nand, "/tmp/sys/scratch", save, 101), 0);
  CHECK(wii_nand_changed_at(nand) == 5000, "a change is stamped with the time (%llu)",
        (unsigned long long)wii_nand_changed_at(nand));
  wii_nand_set_time(nand, 9000);
  CHECK(wii_nand_changed_at(nand) == 5000, "and time passing does not move it");
  wii_nand_set_time(nand, 100);
  CHECK(wii_nand_changed_at(nand) == 100, "a clock that started again cannot leave the change in the future");

  /* There and back. */
  EXPECT(wii_nand_save(nand, path), 0);
  CHECK(!wii_nand_dirty(nand), "saved is clean");
  loaded = wii_nand_load(path, &missing, reason, sizeof(reason));
  CHECK(loaded != NULL && missing == 0, "it loads: %s", reason);
  if (!loaded) return;
  CHECK(!wii_nand_dirty(loaded), "a loaded filesystem is clean");
  CHECK(tree_hash(loaded) != tree_hash(nand), "what was under /tmp was not kept");
  wii_nand_clear_tmp(nand);
  CHECK(!wii_nand_dirty(nand), "emptying /tmp changes nothing the file holds");
  hash = tree_hash(nand);
  CHECK(tree_hash(loaded) == hash, "everything else is the same, in the same order");
  {
    WiiNandStats a, b;
    wii_nand_stats(nand, &a);
    wii_nand_stats(loaded, &b);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0, "with the same space used (%u clusters, %u entries)", (unsigned)b.used_clusters,
          (unsigned)b.used_inodes);
    CHECK(b.used_clusters == 5u + 1u + 1u + 1u, "SYSCONF, setting.txt, uid.sys and five of save");
  }
  CHECK(wii_nand_title_uid(loaded, 0x0001000052535045ull, 0) == 0x1001, "the title is still user 0x1001");

  /* The file is the same whoever writes it. */
  image = read_whole(path, &size);
  EXPECT(wii_nand_save(loaded, in_scratch("second.nand")), 0);
  second = read_whole(in_scratch("second.nand"), &second_size);
  CHECK(image && second && size == second_size && memcmp(image, second, size) == 0,
        "saving what was loaded gives the same bytes (%u, %u)", (unsigned)size, (unsigned)second_size);
  free(second);
  wii_nand_destroy(loaded);
  if (!image) return;
  CHECK(memcmp(image, "NCWiiFS", 8) == 0, "the file says what it is");
  CHECK(crc32_slow((const uint8_t *)"123456789", 9) == 0xcbf43926u, "the test's CRC is the standard one");
  CHECK(crc32_slow(image + 24, size - 24u) == be32_at(image + 20), "and it is the file's");

  /* Cut short anywhere, it is refused whole. */
  {
    static const size_t fractions[] = {1, 7, 8, 23, 24, 51, 52, 53, 200};
    size_t cuts[16], n = 0;
    for (i = 0; i < sizeof(fractions) / sizeof(fractions[0]); i++) cuts[n++] = fractions[i];
    cuts[n++] = size / 3u;
    cuts[n++] = size / 2u;
    cuts[n++] = size - 0x4001u;
    cuts[n++] = size - 1u;
    for (i = 0; i < n; i++) {
      write_whole(in_scratch("cut.nand"), image, cuts[i]);
      reason[0] = 0;
      loaded = wii_nand_load(in_scratch("cut.nand"), &missing, reason, sizeof(reason));
      CHECK(!loaded && missing == 0 && reason[0], "cut to %u bytes: %s", (unsigned)cuts[i], reason);
      if (loaded) wii_nand_destroy(loaded);
    }
  }
  /* Added to. */
  {
    uint8_t *longer = (uint8_t *)malloc(size + 1u);
    memcpy(longer, image, size);
    longer[size] = 0;
    write_whole(in_scratch("long.nand"), longer, size + 1u);
    free(longer);
    loaded = wii_nand_load(in_scratch("long.nand"), &missing, reason, sizeof(reason));
    CHECK(!loaded && missing == 0, "a byte added: %s", reason);
    if (loaded) wii_nand_destroy(loaded);
  }
  /* One bit wrong anywhere. */
  {
    static const size_t fixed[] = {0, 7, 9, 11, 13, 17, 19, 20, 23, 24, 25, 26, 38, 42, 45, 48, 76};
    size_t at[32], n = 0;
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) at[n++] = fixed[i];
    at[n++] = size / 2u;
    at[n++] = size - 1u;
    for (i = 0; i < n; i++) {
      uint8_t *copy = (uint8_t *)malloc(size);
      memcpy(copy, image, size);
      copy[at[i]] ^= 0x10u;
      write_whole(in_scratch("flip.nand"), copy, size);
      free(copy);
      reason[0] = 0;
      loaded = wii_nand_load(in_scratch("flip.nand"), &missing, reason, sizeof(reason));
      CHECK(!loaded && missing == 0 && reason[0], "a bit flipped at %u: %s", (unsigned)at[i], reason);
      if (loaded) wii_nand_destroy(loaded);
    }
  }
  /* Something else entirely. */
  write_whole(in_scratch("other.nand"), save, sizeof(save));
  loaded = wii_nand_load(in_scratch("other.nand"), &missing, reason, sizeof(reason));
  CHECK(!loaded && missing == 0 && strstr(reason, "not a native-console"), "not one of ours: %s", reason);
  /* A later version of the format. */
  {
    uint8_t *copy = (uint8_t *)malloc(size);
    memcpy(copy, image, size);
    copy[11] = 2;
    write_whole(in_scratch("newer.nand"), copy, size);
    free(copy);
    loaded = wii_nand_load(in_scratch("newer.nand"), &missing, reason, sizeof(reason));
    CHECK(!loaded && strstr(reason, "version 2"), "a newer format: %s", reason);
  }
  free(image);

  /* A file whose checksum is right and whose contents break ISFS's rules
   * was not written by this code, and is refused by name. The filesystem:
   * the root, a directory "a" with a file "f" of three bytes in it, and a
   * file "y" - so "a" is at 52, "f" at 80, "y" at 111. */
  {
    WiiNand *tiny = wii_nand_create();
    EXPECT(wii_nand_create_file(tiny, &ios, "/y", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_dir(tiny, &ios, "/a", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_put(tiny, "/a/f", "abc", 3), 0);
    EXPECT(wii_nand_save(tiny, in_scratch("tiny.nand")), 0);
    wii_nand_destroy(tiny);
    image = read_whole(in_scratch("tiny.nand"), &size);
    CHECK(image && size == 24u + 28u * 4u + 3u, "the tiny file is %u bytes", (unsigned)size);
    if (image && size == 24u + 28u * 4u + 3u) {
      CHECK(image[52 + 2] == 'a' && image[80 + 2] == 'f' && image[111 + 2] == 'y', "and laid out as expected");
      CHECK(loads_altered(image, size, 80 + 28, 'x', reason, sizeof(reason)), "a changed byte of a file's contents is fine");
      CHECK(!loads_altered(image, size, 52 + 0, 3, reason, sizeof(reason)), "a third kind of entry: %s", reason);
      CHECK(!loads_altered(image, size, 24 + 0, 2, reason, sizeof(reason)), "a root that is a file: %s", reason);
      CHECK(!loads_altered(image, size, 24 + 1, 1, reason, sizeof(reason)), "a root with a name: %s", reason);
      CHECK(!loads_altered(image, size, 52 + 1, 0, reason, sizeof(reason)), "an entry with no name: %s", reason);
      CHECK(!loads_altered(image, size, 52 + 1, 13, reason, sizeof(reason)), "a thirteen-character name: %s", reason);
      CHECK(!loads_altered(image, size, 52 + 2, '/', reason, sizeof(reason)), "a slash in a name: %s", reason);
      CHECK(!loads_altered(image, size, 52 + 2, 0x07, reason, sizeof(reason)), "a control character in a name: %s", reason);
      CHECK(!loads_altered(image, size, 52 + 7, 'x', reason, sizeof(reason)), "a name longer than it says: %s", reason);
      CHECK(!loads_altered(image, size, 52 + 21, 4, reason, sizeof(reason)), "permissions out of range: %s", reason);
      CHECK(!loads_altered(image, size, 111 + 2, 'a', reason, sizeof(reason)), "two entries of one name: %s", reason);
      CHECK(!loads_altered(image, size, 80 + 27, 200, reason, sizeof(reason)), "a file longer than the file: %s", reason);
      CHECK(!loads_altered(image, size, 80 + 27, 2, reason, sizeof(reason)), "a file shorter than it is: %s", reason);
      CHECK(!loads_altered(image, size, 24 + 27, 3, reason, sizeof(reason)), "a directory with more than there is: %s", reason);
      CHECK(!loads_altered(image, size, 24 + 27, 1, reason, sizeof(reason)), "a directory with less: %s", reason);
      CHECK(!loads_altered(image, size, 15, 5, reason, sizeof(reason)), "a count that is not the count: %s", reason);
    }
    free(image);
  }
  /* A directory nine deep. */
  {
    WiiNand *deep = wii_nand_create();
    EXPECT(wii_nand_create_dir(deep, &ios, "/1", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_dir(deep, &ios, "/1/2", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_dir(deep, &ios, "/1/2/3", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_dir(deep, &ios, "/1/2/3/4", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_dir(deep, &ios, "/1/2/3/4/5", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_dir(deep, &ios, "/1/2/3/4/5/6", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_dir(deep, &ios, "/1/2/3/4/5/6/7", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_dir(deep, &ios, "/1/2/3/4/5/6/7/8", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_create_file(deep, &ios, "/1/2/3/4/5/6/7/8/f", 0, RW, RW, RW), 0);
    EXPECT(wii_nand_save(deep, in_scratch("deep.nand")), 0);
    wii_nand_destroy(deep);
    image = read_whole(in_scratch("deep.nand"), &size);
    loaded = wii_nand_load(in_scratch("deep.nand"), &missing, reason, sizeof(reason));
    CHECK(loaded != NULL, "eight deep with a file in the eighth loads: %s", reason);
    if (loaded) wii_nand_destroy(loaded);
    /* The last entry is the file: make it a directory with nothing in it. */
    if (image && size == 24u + 28u * 10u) {
      CHECK(!loads_altered(image, size, size - 28u, 1, reason, sizeof(reason)), "a ninth directory: %s", reason);
    }
    free(image);
  }

  /* Writing that fails leaves the file that was there. The temporary
   * name is taken by a directory, so the write cannot begin. */
  EXPECT(wii_nand_save(nand, path), 0);
  EXPECT(wii_nand_put(nand, "/title/00010000/52535045/data/banner.bin", "banner", 6), 0);
  CHECK(make_directory(temporary) == 0, "the test could make a directory in the way");
  EXPECT(wii_nand_save(nand, path), -1);
  CHECK(wii_nand_dirty(nand), "a failed save leaves the filesystem dirty");
  loaded = wii_nand_load(path, &missing, reason, sizeof(reason));
  CHECK(loaded && tree_hash(loaded) == hash, "and the file as it was: %s", reason);
  if (loaded) wii_nand_destroy(loaded);
  remove_directory(temporary);
  /* Somewhere that does not exist. */
  EXPECT(wii_nand_save(nand, in_scratch("no-such-directory/console.nand")), -1);
  EXPECT(wii_nand_save(nand, ""), -1);
  /* What a run that died mid-write leaves behind - half a file under the
   * temporary name - is not the filesystem, and the next save replaces it. */
  write_whole(temporary, save, 1000);
  loaded = wii_nand_load(path, &missing, reason, sizeof(reason));
  CHECK(loaded && tree_hash(loaded) == hash, "a stale temporary file is not read: %s", reason);
  if (loaded) wii_nand_destroy(loaded);
  EXPECT(wii_nand_save(nand, path), 0);
  CHECK(!wii_nand_dirty(nand), "then it saves");
  loaded = wii_nand_load(path, &missing, reason, sizeof(reason));
  CHECK(loaded && tree_hash(loaded) == tree_hash(nand) && tree_hash(nand) != hash, "and the file is the new one: %s",
        reason);
  if (loaded) wii_nand_destroy(loaded);
  {
    FILE *left = fopen(temporary, "rb");
    CHECK(left == NULL, "with no temporary file left over");
    if (left) fclose(left);
  }

  /* A freshly formatted console's file is the recorded one. */
  EXPECT(wii_nand_format(nand, NULL), 0);
  EXPECT(wii_nand_save(nand, in_scratch("fresh.nand")), 0);
  image = read_whole(in_scratch("fresh.nand"), &size);
  if (image) {
    const uint64_t fresh = fnv(FNV_START, image, size);
    CHECK(fresh == FRESH_NAND_HASH, "a fresh console's file is %u bytes, hash %016llx",
          (unsigned)size, (unsigned long long)fresh);
    free(image);
  }
  wii_nand_destroy(nand);
}

/* The television's shape is one byte of SYSCONF: setting it changes that
 * byte and nothing else in the file or the flash, and setting it to what it
 * is leaves the flash clean. */
static void test_picture_shape(void) {
  WiiNand *nand = wii_nand_create();
  static uint8_t before[WII_SYSCONF_SIZE];
  const uint8_t *data = NULL, *item;
  uint32_t size = 0, differ = 0, i;
  char path[512];

  CHECK(wii_nand_widescreen(nand) == -1 && wii_nand_set_widescreen(nand, 1) == -1,
        "a flash with no SYSCONF has no shape to read or set");
  EXPECT(wii_nand_format(nand, NULL), 0);
  CHECK(wii_nand_widescreen(nand) == 0, "a console fresh from its box is set to 4:3");

  /* Written, so that the flash is clean and a change shows. */
  snprintf(path, sizeof(path), "%s/shape.nand", scratch);
  EXPECT(wii_nand_save(nand, path), 0);
  EXPECT(wii_nand_set_widescreen(nand, 0), 0);
  CHECK(!wii_nand_dirty(nand), "setting the shape it already has changes nothing");

  EXPECT(wii_nand_get(nand, "/shared2/sys/SYSCONF", &data, &size), 0);
  CHECK(size == WII_SYSCONF_SIZE, "SYSCONF is there");
  if (size == WII_SYSCONF_SIZE) memcpy(before, data, size);
  EXPECT(wii_nand_set_widescreen(nand, 1), 0);
  CHECK(wii_nand_widescreen(nand) == 1 && wii_nand_dirty(nand), "16:9, and the flash has something to write");
  EXPECT(wii_nand_get(nand, "/shared2/sys/SYSCONF", &data, &size), 0);
  item = wii_sysconf_find(data, size, "IPL.AR", NULL, NULL);
  CHECK(item && item[0] == 1, "IPL.AR is one");
  for (i = 0; i < size && size == WII_SYSCONF_SIZE; i++) differ += data[i] != before[i];
  CHECK(differ == 1u, "and it is the only byte of the file that changed (%u)", (unsigned)differ);
  expect_entry(nand, "/shared2/sys/SYSCONF", 0, 0, 0, RW, RW, RW);

  EXPECT(wii_nand_set_widescreen(nand, 0), 0);
  CHECK(wii_nand_widescreen(nand) == 0, "and back");
  remove(path);
  wii_nand_destroy(nand);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: wii_nand_test <scratch directory>\n");
    return 2;
  }
  scratch = argv[1];
  test_paths();
  test_files();
  test_permissions();
  test_names();
  test_table_full();
  test_settings_files();
  test_format();
  test_picture_shape();
  test_host_file();
  printf("wii_nand_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
