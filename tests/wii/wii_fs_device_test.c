/*
 * /dev/fs and the filesystem's files, driven through the mailbox: request
 * blocks written into guest memory and handed over through Hollywood's
 * registers, the way a game's IPC client does it and the way
 * wii_machine_test.c does. No guest code runs.
 *
 * What is checked here is what wii_nand_test.c cannot see: each request's
 * buffer layout, byte for byte as libogc's isfs.c and the SDK's fs.c send
 * it; that a descriptor's position is its own; the sixteen-descriptor
 * limit; how long a request takes; whose name requests are made in; and
 * the machine's flash reaching its file on the host a second after a game
 * stops writing.
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

#include "gc.h"
#include "gc_bus.h"
#include "gc_disc.h"
#include "wii.h"
#include "gc_state.h"
#include "wii_nand.h"

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

#define EXPECT(call, want)                                                           \
  do {                                                                               \
    const long got_ = (long)(call);                                                  \
    CHECK(got_ == (long)(want), "%s gave %ld, want %ld", #call, got_, (long)(want)); \
  } while (0)

#define HW(offset) (0xcd000000u + (offset))

/* Guest memory the test uses, in MEM2 where a game's request blocks are:
 * the block, a path to open, an input and an output buffer, a table of
 * vectors with the small buffers they point at, and room for file data. */
#define BLOCK 0x13300000u
#define PATH (BLOCK + 0x040u)
#define IN (BLOCK + 0x100u)
#define OUT (BLOCK + 0x200u)
#define VECTORS (BLOCK + 0x300u)
#define WORD_A (BLOCK + 0x380u)
#define WORD_B (BLOCK + 0x3a0u)
#define NAMES (BLOCK + 0x400u)
#define DATA (BLOCK + 0x1000u)
#define DATA_SIZE 0x20000u

static const char *scratch;

static GcMachine *fresh(void) {
  GcMachine *m = gc_machine_create();
  if (m) {
    m->log = NULL;
    gc_install_boot_state(m);
  }
  return m;
}

/* A machine goes away with its flash. (wii_machine_destroy will do this
 * itself; until it does, and harmlessly after, the test does.) */
static void discard(GcMachine *m) {
  wii_nand_detach(m);
  gc_machine_destroy(m);
}

static uint8_t *ram(GcMachine *m, uint32_t at, uint32_t size) { return gc_machine_ram(m, at, size); }

/* How long the last request took, from being handed over to being
 * answered. */
static uint64_t took;

/* One request, start to finish, with the mailbox's interrupts off. */
static int32_t ipc(GcMachine *m, uint32_t type, uint32_t fd, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                   uint32_t a4) {
  GcCpu *c = &m->cpu;
  uint8_t *block = ram(m, BLOCK, 0x40);
  const uint64_t start = m->cpu.cycles;
  uint64_t due;
  memset(block, 0, 0x40);
  wii_put32(block, type);
  wii_put32(block + 0x08, fd);
  wii_put32(block + 0x0c, a0);
  wii_put32(block + 0x10, a1);
  wii_put32(block + 0x14, a2);
  wii_put32(block + 0x18, a3);
  wii_put32(block + 0x1c, a4);
  gc_bus_write32(c, HW(0x000), BLOCK);
  gc_bus_write32(c, HW(0x004), 1u);

  due = wii_machine_next_event(m, ~0ull);
  if (due != ~0ull && due > m->cpu.cycles) m->cpu.cycles = due;
  wii_machine_advance(m);
  if ((gc_bus_read32(c, HW(0x004)) & 0x06u) != 0x02u) {
    CHECK(0, "request %u was not acknowledged", type);
    return -9999;
  }
  gc_bus_write32(c, HW(0x004), 0x02u);

  due = wii_machine_next_event(m, ~0ull);
  if (due == ~0ull) {
    CHECK(0, "request %u was never answered", type);
    return -9999;
  }
  if (due > m->cpu.cycles) m->cpu.cycles = due;
  wii_machine_advance(m);
  if ((gc_bus_read32(c, HW(0x004)) & 0x06u) != 0x04u) {
    CHECK(0, "request %u was not answered when it was due", type);
    return -9999;
  }
  took = due - start;
  CHECK(wii_be32(block) == WII_IOS_REPLY, "the block is marked answered");
  gc_bus_write32(c, HW(0x004), 0x04u);
  return (int32_t)wii_be32(block + 4);
}

static int32_t ios_open(GcMachine *m, const char *path, uint32_t mode) {
  uint8_t *at = ram(m, PATH, 0xc0);
  memset(at, 0, 0xc0);
  memcpy(at, path, strlen(path));
  return ipc(m, WII_IOS_OPEN, 0, PATH, mode, 0, 0, 0);
}
static int32_t ios_close(GcMachine *m, int32_t fd) { return ipc(m, WII_IOS_CLOSE, (uint32_t)fd, 0, 0, 0, 0, 0); }
static int32_t ios_read(GcMachine *m, int32_t fd, uint32_t at, uint32_t size) {
  return ipc(m, WII_IOS_READ, (uint32_t)fd, at, size, 0, 0, 0);
}
static int32_t ios_write(GcMachine *m, int32_t fd, uint32_t at, uint32_t size) {
  return ipc(m, WII_IOS_WRITE, (uint32_t)fd, at, size, 0, 0, 0);
}
static int32_t ios_seek(GcMachine *m, int32_t fd, int32_t where, int32_t whence) {
  return ipc(m, WII_IOS_SEEK, (uint32_t)fd, (uint32_t)where, (uint32_t)whence, 0, 0, 0);
}
static int32_t ios_ioctl(GcMachine *m, int32_t fd, uint32_t command, uint32_t in, uint32_t in_size, uint32_t out,
                         uint32_t out_size) {
  return ipc(m, WII_IOS_IOCTL, (uint32_t)fd, command, in, in_size, out, out_size);
}
static int32_t ios_ioctlv(GcMachine *m, int32_t fd, uint32_t command, uint32_t in_count, uint32_t io_count) {
  return ipc(m, WII_IOS_IOCTLV, (uint32_t)fd, command, in_count, io_count, VECTORS, 0);
}

static void vector(GcMachine *m, unsigned index, uint32_t address, uint32_t size) {
  uint8_t *v = ram(m, VECTORS + index * 8u, 8);
  wii_put32(v, address);
  wii_put32(v + 4, size);
}

/* A path in a 64-byte field. */
static void path_at(GcMachine *m, uint32_t at, const char *path) {
  uint8_t *p = ram(m, at, 64);
  memset(p, 0, 64);
  memcpy(p, path, strlen(path));
}

/* The block that names something and its attributes, as both clients lay
 * it out: owner, group, path, three permissions, the attribute byte, two
 * bytes of padding - 0x4c bytes. */
static void attr_block(GcMachine *m, uint32_t at, const char *path, uint32_t uid, uint16_t gid, uint8_t owner,
                       uint8_t group, uint8_t other, uint8_t attributes) {
  uint8_t *p = ram(m, at, 0x4c);
  memset(p, 0, 0x4c);
  wii_put32(p, uid);
  wii_put16(p + 4, gid);
  memcpy(p + 6, path, strlen(path));
  p[0x46] = owner;
  p[0x47] = group;
  p[0x48] = other;
  p[0x49] = attributes;
}

/* The requests, as a client makes them. */
static int32_t fs_create(GcMachine *m, int32_t fs, int directory, const char *path, uint8_t owner, uint8_t group,
                         uint8_t other) {
  /* Neither client fills in the owner; whatever is there must not matter. */
  attr_block(m, IN, path, 0xdeadbeefu, 0xbeef, owner, group, other, 0);
  return ios_ioctl(m, fs, directory ? 3u : 9u, IN, 0x4c, 0, 0);
}
static int32_t fs_delete(GcMachine *m, int32_t fs, const char *path) {
  path_at(m, IN, path);
  return ios_ioctl(m, fs, 7, IN, 64, 0, 0);
}
static int32_t fs_rename(GcMachine *m, int32_t fs, const char *from, const char *to) {
  path_at(m, IN, from);
  path_at(m, IN + 64u, to);
  return ios_ioctl(m, fs, 8, IN, 128, 0, 0);
}
static int32_t fs_get_attr(GcMachine *m, int32_t fs, const char *path) {
  path_at(m, IN, path);
  memset(ram(m, OUT, 0x50), 0xee, 0x50);
  return ios_ioctl(m, fs, 6, IN, 64, OUT, 0x4c);
}
static int32_t fs_count(GcMachine *m, int32_t fs, const char *path, uint32_t *count) {
  int32_t result;
  path_at(m, IN, path);
  wii_put32(ram(m, WORD_A, 4), 0xeeeeeeeeu);
  vector(m, 0, IN, 64);
  vector(m, 1, WORD_A, 4);
  result = ios_ioctlv(m, fs, 4, 1, 1);
  *count = wii_be32(ram(m, WORD_A, 4));
  return result;
}

static int32_t fs_usage(GcMachine *m, int32_t fs, const char *path, uint32_t *clusters, uint32_t *inodes) {
  int32_t result;
  path_at(m, IN, path);
  vector(m, 0, IN, 64);
  vector(m, 1, WORD_A, 4);
  vector(m, 2, WORD_B, 4);
  result = ios_ioctlv(m, fs, 12, 1, 2);
  *clusters = wii_be32(ram(m, WORD_A, 4));
  *inodes = wii_be32(ram(m, WORD_B, 4));
  return result;
}

static void fill(GcMachine *m, uint32_t at, uint32_t size, unsigned seed) {
  uint8_t *p = ram(m, at, size);
  uint32_t i;
  for (i = 0; i < size; i++) p[i] = (uint8_t)(i * 13u + seed + (i >> 9));
}

/* ------------------------------------------------------------------ */

static void test_requests(void) {
  GcMachine *m = fresh();
  WiiIos *ios = &wii_state(m)->ios;
  WiiNandStats stats;
  int32_t fs, file, second;
  uint8_t *out = ram(m, OUT, 0x100);
  uint8_t *data = ram(m, DATA, DATA_SIZE);
  uint32_t count = 0;
  unsigned i;

  CHECK(wii_state(m)->host.nand == NULL, "a machine has no flash until something asks");
  fs = ios_open(m, "/dev/fs", 0);
  CHECK(fs >= 0, "/dev/fs opens (%d)", (int)fs);
  CHECK(wii_state(m)->host.nand != NULL, "and then it has one, in memory");

  /* 2: how full the chip is - seven words. */
  memset(out, 0xee, 0x20);
  EXPECT(ios_ioctl(m, fs, 2, 0, 0, OUT, 0x1c), 0);
  wii_nand_stats(wii_nand_of(m), &stats);
  CHECK(wii_be32(out) == 0x4000 && wii_be32(out + 4) == stats.free_clusters && wii_be32(out + 8) == stats.used_clusters &&
            wii_be32(out + 12) == 0 && wii_be32(out + 16) == stats.reserved_clusters &&
            wii_be32(out + 20) == stats.free_inodes && wii_be32(out + 24) == stats.used_inodes,
        "cluster size, free, used, bad, reserved, free entries, used entries");
  CHECK(wii_be32(out + 4) + wii_be32(out + 8) + wii_be32(out + 12) + wii_be32(out + 16) == 0x7ec0u,
        "which add up to the clusters the filesystem counts");
  CHECK(out[0x1c] == 0xee, "and nothing past the seventh word is written");
  EXPECT(ios_ioctl(m, fs, 2, 0, 0, OUT, 0x18), WII_FS_EINVAL);
  EXPECT(ios_ioctl(m, fs, 2, 0, 0, 0, 0), WII_FS_EINVAL);
  /* A buffer with room to spare is a buffer with room. */
  EXPECT(ios_ioctl(m, fs, 2, 0, 0, OUT, 0x20), 0);
  CHECK(out[0x1c] == 0xee, "still only seven words");

  /* /dev/fs has no contents of its own. */
  EXPECT(ios_read(m, fs, DATA, 0x10), WII_FS_EINVAL);
  EXPECT(ios_write(m, fs, DATA, 0x10), WII_FS_EINVAL);
  EXPECT(ios_seek(m, fs, 0, 0), WII_FS_EINVAL);

  /* 3 and 9: make a directory and a file. The owner in the block is not
   * the caller's to choose. */
  EXPECT(fs_create(m, fs, 1, "/tmp/dev", 3, 3, 1), 0);
  EXPECT(fs_create(m, fs, 1, "/tmp/dev", 3, 3, 1), WII_FS_EEXIST);
  attr_block(m, IN, "/tmp/short", 0, 0, 3, 3, 3, 0);
  EXPECT(ios_ioctl(m, fs, 3, IN, 0x4b, 0, 0), WII_FS_EINVAL);
  EXPECT(ios_ioctl(m, fs, 9, IN, 0x40, 0, 0), WII_FS_EINVAL);
  EXPECT(ios_ioctl(m, fs, 3, 0, 0, 0, 0), WII_FS_EINVAL);
  /* A path that fills its field and never ends. */
  attr_block(m, IN, "/tmp/unending", 0, 0, 3, 3, 3, 0);
  memset(ram(m, IN + 6u, 64), 'a', 64);
  ram(m, IN + 6u, 1)[0] = '/';
  EXPECT(ios_ioctl(m, fs, 3, IN, 0x4c, 0, 0), WII_FS_EINVAL);
  attr_block(m, IN, "/tmp/dev/file.bin", 0x12345678u, 0x9abc, 3, 3, 0, 0x42);
  EXPECT(ios_ioctl(m, fs, 9, IN, 0x4c, 0, 0), 0);
  EXPECT(fs_create(m, fs, 0, "/sys/mine", 3, 3, 3), WII_FS_EACCESS);
  EXPECT(fs_create(m, fs, 0, "/tmp/none/f", 3, 3, 3), WII_FS_ENOENT);
  EXPECT(fs_create(m, fs, 0, "/tmp/dev/thirteenchars", 3, 3, 3), WII_FS_EINVAL);

  /* 6: attributes, in the same block. */
  EXPECT(fs_get_attr(m, fs, "/tmp/dev/file.bin"), 0);
  CHECK(wii_be32(out) == 0x1000u && wii_be16(out + 4) == 0x3031u, "the file is the caller's: user %x group %x",
        (unsigned)wii_be32(out), (unsigned)wii_be16(out + 4));
  CHECK(out[0x46] == 3 && out[0x47] == 3 && out[0x48] == 0 && out[0x49] == 0x42, "permissions %u %u %u, attribute %02x",
        out[0x46], out[0x47], out[0x48], out[0x49]);
  /* Those six fields are all IOS writes: the path's place and the padding
   * are left as they were. */
  for (i = 6u, count = 0; i < 0x46u; i++) count += out[i] != 0xee;
  CHECK(count == 0 && out[0x4a] == 0xee && out[0x4b] == 0xee && out[0x4c] == 0xee,
        "and nothing else in the block is touched");
  EXPECT(fs_get_attr(m, fs, "/tmp/dev"), 0);
  CHECK(out[0x46] == 3 && out[0x47] == 3 && out[0x48] == 1, "the directory's permissions");
  EXPECT(fs_get_attr(m, fs, "/tmp/dev/none"), WII_FS_ENOENT);
  /* The root's own attributes need nobody's leave; what is in it does. */
  EXPECT(fs_get_attr(m, fs, "/"), 0);
  EXPECT(fs_get_attr(m, fs, "/sys"), WII_FS_EACCESS);
  path_at(m, IN, "/tmp/dev");
  EXPECT(ios_ioctl(m, fs, 6, IN, 64, OUT, 0x48), WII_FS_EINVAL);
  EXPECT(ios_ioctl(m, fs, 6, IN, 63, OUT, 0x4c), WII_FS_EINVAL);

  /* 5: change them. The owner may; nobody but IOS may change the owner. */
  attr_block(m, IN, "/tmp/dev/file.bin", 0x1000u, 0x1234, 3, 1, 1, 7);
  EXPECT(ios_ioctl(m, fs, 5, IN, 0x4c, 0, 0), 0);
  EXPECT(fs_get_attr(m, fs, "/tmp/dev/file.bin"), 0);
  CHECK(wii_be32(out) == 0x1000u && wii_be16(out + 4) == 0x1234u && out[0x46] == 3 && out[0x47] == 1 && out[0x48] == 1 &&
            out[0x49] == 7,
        "the attributes changed");
  attr_block(m, IN, "/tmp/dev/file.bin", 0x2000u, 0x1234, 3, 1, 1, 7);
  EXPECT(ios_ioctl(m, fs, 5, IN, 0x4c, 0, 0), WII_FS_EACCESS);
  attr_block(m, IN, "/tmp/dev/none", 0x1000u, 0x1234, 3, 1, 1, 7);
  EXPECT(ios_ioctl(m, fs, 5, IN, 0x4c, 0, 0), WII_FS_ENOENT);
  EXPECT(ios_ioctl(m, fs, 5, IN, 0x40, 0, 0), WII_FS_EINVAL);

  /* A file: its path is opened as if it were a device. */
  EXPECT(ios_open(m, "/tmp/dev/none", 1), WII_FS_ENOENT);
  EXPECT(ios_open(m, "/tmp/dev", 1), WII_FS_EINVAL);
  EXPECT(ios_open(m, "/sys/uid.sys", 1), WII_FS_EACCESS);
  /* A mode is two bits; the rest are not looked at. */
  second = ios_open(m, "/tmp/dev/file.bin", 0xf5);
  CHECK(second >= 0 && ios->files[second].mode == 1, "mode 0xf5 opens for reading (%d)", (int)second);
  EXPECT(ios_close(m, second), 0);
  /* Sixty-four characters and more: not the file its first sixty-three
   * name, even though that file is there. */
  EXPECT(fs_create(m, fs, 1, "/tmp/aaaaaaaaaaaa", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 1, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 1, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 0, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/dddddddddddd", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 0, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/123456789012", 3, 3, 3), 0);
  second = ios_open(m, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/dddddddddddd", 1);
  CHECK(second >= 0, "a path of %u characters opens (%d)",
        (unsigned)strlen("/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/dddddddddddd"), (int)second);
  EXPECT(ios_close(m, second), 0);
  {
    /* Sixty-three characters exactly, then the same with one more. */
    const char *full = "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/123456789012/eeeeee";
    CHECK(strlen(full) == 63, "the test's own path");
    EXPECT(fs_delete(m, fs, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/123456789012"), 0);
    EXPECT(fs_create(m, fs, 1, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/123456789012", 3, 3, 3), 0);
    EXPECT(fs_create(m, fs, 0, full, 3, 3, 3), 0);
    second = ios_open(m, full, 1);
    CHECK(second >= 0, "sixty-three characters open (%d)", (int)second);
    EXPECT(ios_close(m, second), 0);
    EXPECT(ios_open(m, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/123456789012/eeeeeeX", 1), WII_FS_EINVAL);
    EXPECT(ios_open(m, "/tmp/aaaaaaaaaaaa/bbbbbbbbbbbb/cccccccccccc/123456789012/eeeeeeXYZ", 1), WII_FS_EINVAL);
  }

  file = ios_open(m, "/tmp/dev/file.bin", 3);
  CHECK(file >= 0 && file != fs, "the file opens (%d)", (int)file);
  CHECK(strcmp(ios->files[file].path, "/tmp/dev/file.bin") == 0 && ios->files[file].position == 0,
        "its descriptor is its path and a position");

  /* Write from guest memory, read back into it. */
  fill(m, DATA, 0x1234, 1);
  EXPECT(ios_write(m, file, DATA, 0x1234), 0x1234);
  CHECK(ios->files[file].position == 0x1234, "the position is the descriptor's (%x)", (unsigned)ios->files[file].position);
  /* 11: the length and the position, asked of the file itself. */
  memset(out, 0xee, 16);
  EXPECT(ios_ioctl(m, file, 11, 0, 0, OUT, 8), 0);
  CHECK(wii_be32(out) == 0x1234 && wii_be32(out + 4) == 0x1234 && out[8] == 0xee, "length %x, position %x",
        (unsigned)wii_be32(out), (unsigned)wii_be32(out + 4));
  EXPECT(ios_ioctl(m, file, 11, 0, 0, OUT, 4), WII_FS_EINVAL);
  EXPECT(ios_ioctl(m, fs, 11, 0, 0, OUT, 8), WII_FS_EINVAL);
  EXPECT(ios_seek(m, file, 0x100, 0), 0x100);
  memset(data + 0x8000, 0xee, 0x400);
  EXPECT(ios_read(m, file, DATA + 0x8000u, 0x200), 0x200);
  CHECK(memcmp(data + 0x8000, data + 0x100, 0x200) == 0, "what was written is read into guest memory");
  CHECK(data[0x8200] == 0xee, "and not a byte further");
  EXPECT(ios_seek(m, file, -4, 1), 0x2fc);
  EXPECT(ios_seek(m, file, 0, 2), 0x1234);
  EXPECT(ios_read(m, file, DATA + 0x8000u, 0x200), 0);
  EXPECT(ios_seek(m, file, 1, 2), WII_FS_EINVAL);
  EXPECT(ios_seek(m, file, -1, 0), WII_FS_EINVAL);
  EXPECT(ios_seek(m, file, 0, 3), WII_FS_EINVAL);
  EXPECT(ios_seek(m, file, -0x34, 2), 0x1200);
  EXPECT(ios_read(m, file, DATA + 0x8000u, 0x200), 0x34);
  /* A buffer that is not memory. */
  EXPECT(ios_seek(m, file, 0, 0), 0);
  EXPECT(ios_read(m, file, 0, 0x10), WII_FS_EINVAL);
  EXPECT(ios_read(m, file, 0x0ffffff0u, 0x10), WII_FS_EINVAL);
  EXPECT(ios_write(m, file, 0, 0x10), WII_FS_EINVAL);

  /* A second descriptor on the same file has its own position and mode. */
  second = ios_open(m, "/tmp/dev/file.bin", 1);
  CHECK(second >= 0 && second != file, "the file opens again (%d)", (int)second);
  EXPECT(ios_seek(m, file, 0x800, 0), 0x800);
  EXPECT(ios_read(m, second, DATA + 0x8000u, 4), 4);
  CHECK(memcmp(data + 0x8000, data, 4) == 0, "the second reads from its own position");
  EXPECT(ios_write(m, second, DATA, 4), WII_FS_EACCESS);
  CHECK(wii_nand_open_count(wii_nand_of(m)) == 2, "the filesystem knows two descriptors are on files");

  /* 4: list a directory. One buffer each way asks how many. */
  EXPECT(fs_count(m, fs, "/tmp/dev", &count), 0);
  CHECK(count == 1, "one entry (%u)", (unsigned)count);
  EXPECT(fs_count(m, fs, "/tmp", &count), 0);
  CHECK(count == 2, "/tmp has two (%u)", (unsigned)count);
  /* Two each way: the limit and the count share a word, as both clients
   * pass them; the buffer has thirteen bytes of room a name, and the names
   * come back end to end. */
  path_at(m, IN, "/tmp");
  wii_put32(ram(m, WORD_A, 4), 8);
  memset(ram(m, NAMES, 13u * 8u + 8u), 0xee, 13u * 8u + 8u);
  vector(m, 0, IN, 64);
  vector(m, 1, WORD_A, 4);
  vector(m, 2, NAMES, 13u * 8u);
  vector(m, 3, WORD_A, 4);
  EXPECT(ios_ioctlv(m, fs, 4, 2, 2), 0);
  CHECK(wii_be32(ram(m, WORD_A, 4)) == 2, "two names were written");
  {
    const char *names = (const char *)ram(m, NAMES, 13u * 8u);
    CHECK(strcmp(names, "aaaaaaaaaaaa") == 0 && strcmp(names + 13, "dev") == 0, "newest first: %s, %s", names, names + 13);
    /* A twelve-character name happens to fill its thirteen bytes; the one
     * after it takes four, and the room that is left is cleared. */
    CHECK(names[13 + 3] == 0 && names[13 + 4] == 0 && names[13u * 8u - 1u] == 0 &&
              (uint8_t)names[13u * 8u] == 0xee,
          "end to end, and the rest of the buffer cleared");
  }
  /* Room for one. */
  wii_put32(ram(m, WORD_A, 4), 1);
  vector(m, 2, NAMES, 13u);
  EXPECT(ios_ioctlv(m, fs, 4, 2, 2), 0);
  CHECK(wii_be32(ram(m, WORD_A, 4)) == 1, "one name fits");
  /* A limit the buffer cannot hold, and shapes that are neither form. */
  wii_put32(ram(m, WORD_A, 4), 9);
  vector(m, 2, NAMES, 13u * 8u);
  EXPECT(ios_ioctlv(m, fs, 4, 2, 2), WII_FS_EINVAL);
  EXPECT(ios_ioctlv(m, fs, 4, 2, 1), WII_FS_EINVAL);
  EXPECT(ios_ioctlv(m, fs, 4, 0, 1), WII_FS_EINVAL);
  vector(m, 0, IN, 32);
  EXPECT(ios_ioctlv(m, fs, 4, 1, 1), WII_FS_EINVAL);
  /* What the SDK tells a file from a directory by. */
  EXPECT(fs_count(m, fs, "/tmp/dev/file.bin", &count), WII_FS_EINVAL);
  EXPECT(fs_count(m, fs, "/tmp/dev/none", &count), WII_FS_ENOENT);
  EXPECT(fs_count(m, fs, "/", &count), WII_FS_EACCESS);
  EXPECT(fs_count(m, fs, "/ticket", &count), WII_FS_EACCESS);

  /* 12: what a directory uses - clusters, then entries. */
  path_at(m, IN, "/tmp/dev");
  vector(m, 0, IN, 64);
  vector(m, 1, WORD_A, 4);
  vector(m, 2, WORD_B, 4);
  EXPECT(ios_ioctlv(m, fs, 12, 1, 2), 0);
  CHECK(wii_be32(ram(m, WORD_A, 4)) == 1 && wii_be32(ram(m, WORD_B, 4)) == 2, "one cluster, two entries (%u, %u)",
        (unsigned)wii_be32(ram(m, WORD_A, 4)), (unsigned)wii_be32(ram(m, WORD_B, 4)));
  /* The SDK's free-space check asks this of directories a game cannot
   * read, and forgives only "not there". */
  path_at(m, IN, "/ticket");
  EXPECT(ios_ioctlv(m, fs, 12, 1, 2), 0);
  path_at(m, IN, "/title/00010000");
  EXPECT(ios_ioctlv(m, fs, 12, 1, 2), WII_FS_ENOENT);
  path_at(m, IN, "/tmp/dev/file.bin");
  EXPECT(ios_ioctlv(m, fs, 12, 1, 2), WII_FS_EINVAL);
  EXPECT(ios_ioctlv(m, fs, 12, 1, 1), WII_FS_EINVAL);
  EXPECT(ios_ioctlv(m, fs, 12, 2, 2), WII_FS_EINVAL);

  /* 7 and 8: delete and rename. Neither touches an open file. */
  EXPECT(fs_delete(m, fs, "/tmp/dev/file.bin"), WII_FS_EFDOPEN);
  EXPECT(fs_delete(m, fs, "/tmp/dev"), WII_FS_EFDOPEN);
  EXPECT(fs_create(m, fs, 1, "/tmp/dev2", 3, 3, 3), 0);
  EXPECT(fs_rename(m, fs, "/tmp/dev/file.bin", "/tmp/dev2/file.bin"), WII_FS_EFDOPEN);
  EXPECT(ios_close(m, second), 0);
  EXPECT(ios_close(m, file), 0);
  EXPECT(ios_close(m, file), WII_IPC_EINVAL);
  CHECK(wii_nand_open_count(wii_nand_of(m)) == 0, "closed");
  EXPECT(fs_rename(m, fs, "/tmp/dev/file.bin", "/tmp/dev2/other.bin"), WII_FS_EINVAL);
  EXPECT(fs_rename(m, fs, "/tmp/dev/file.bin", "/tmp/dev2/file.bin"), 0);
  EXPECT(fs_rename(m, fs, "/tmp/dev/file.bin", "/tmp/dev2/file.bin"), WII_FS_ENOENT);
  path_at(m, IN, "/tmp/dev2/file.bin");
  EXPECT(ios_ioctl(m, fs, 8, IN, 64, 0, 0), WII_FS_EINVAL);
  EXPECT(ios_ioctl(m, fs, 7, IN, 63, 0, 0), WII_FS_EINVAL);
  EXPECT(fs_rename(m, fs, "/tmp/dev2", "/tmp/renamed"), 0);
  file = ios_open(m, "/tmp/renamed/file.bin", 1);
  CHECK(file >= 0, "the moved file opens where it went");
  EXPECT(ios_read(m, file, DATA + 0x8000u, 0x2000), 0x1234);
  CHECK(memcmp(data + 0x8000, data, 0x1234) == 0, "with its contents");
  EXPECT(ios_close(m, file), 0);

  /* 10: version control, which takes the attribute block and changes
   * nothing a title can see. */
  attr_block(m, IN, "/tmp/renamed/file.bin", 0, 0, 0, 0, 0, 0);
  EXPECT(ios_ioctl(m, fs, 10, IN, 0x4c, 0, 0), 0);
  attr_block(m, IN, "/tmp/renamed/none", 0, 0, 0, 0, 0, 0);
  EXPECT(ios_ioctl(m, fs, 10, IN, 0x4c, 0, 0), WII_FS_ENOENT);

  /* 1: format. Not for a game. */
  EXPECT(ios_ioctl(m, fs, 1, 0, 0, 0, 0), WII_FS_EACCESS);
  EXPECT(fs_count(m, fs, "/tmp/renamed", &count), 0);

  /* Deleting a directory takes what is in it. */
  EXPECT(fs_delete(m, fs, "/tmp/renamed"), 0);
  EXPECT(fs_get_attr(m, fs, "/tmp/renamed/file.bin"), WII_FS_ENOENT);
  EXPECT(fs_delete(m, fs, "/tmp/renamed"), WII_FS_ENOENT);

  /* Requests that are the wrong kind, or that do not exist, are refused
   * and counted. */
  CHECK(ios->unknown_commands == 0 && ios->unknown_devices == 0, "nothing so far was unknown (%u, %u)",
        (unsigned)ios->unknown_commands, (unsigned)ios->unknown_devices);
  EXPECT(ios_ioctl(m, fs, 14, 0, 0, 0, 0), WII_FS_EINVAL);
  EXPECT(ios_ioctl(m, fs, 4, IN, 64, OUT, 4), WII_FS_EINVAL);
  EXPECT(ios_ioctlv(m, fs, 3, 1, 0), WII_FS_EINVAL);
  file = ios_open(m, "/tmp/dev/../dev", 1);
  CHECK(file == WII_FS_ENOENT, "ISFS has no dot-dot (%d)", (int)file);
  CHECK(ios->unknown_commands == 3, "three unknown requests (%u)", (unsigned)ios->unknown_commands);

  EXPECT(ios_close(m, fs), 0);
  discard(m);
}

/* Sixteen descriptors, /dev/fs's own among them. */
static void test_descriptors(void) {
  GcMachine *m = fresh();
  int32_t fs, files[20], other;
  unsigned i;
  char path[32];

  fs = ios_open(m, "/dev/fs", 0);
  for (i = 0; i < 17u; i++) {
    snprintf(path, sizeof(path), "/tmp/%02u", i);
    EXPECT(fs_create(m, fs, 0, path, 3, 3, 3), 0);
  }
  for (i = 0; i < 15u; i++) {
    snprintf(path, sizeof(path), "/tmp/%02u", i);
    files[i] = ios_open(m, path, 3);
    CHECK(files[i] >= 0, "descriptor %u opens (%d)", i + 2u, (int)files[i]);
  }
  EXPECT(ios_open(m, "/tmp/15", 3), WII_FS_EFDEXHAUSTED);
  EXPECT(ios_open(m, "/dev/fs", 0), WII_FS_EFDEXHAUSTED);
  /* The limit is the filesystem's, not the kernel's. */
  other = ios_open(m, "/dev/es", 0);
  CHECK(other >= 0, "another device still opens (%d)", (int)other);
  EXPECT(ios_close(m, other), 0);
  EXPECT(ios_close(m, files[7]), 0);
  files[7] = ios_open(m, "/tmp/15", 3);
  CHECK(files[7] >= 0, "one closed, one opens (%d)", (int)files[7]);
  EXPECT(ios_open(m, "/tmp/16", 3), WII_FS_EFDEXHAUSTED);

  /* A descriptor table that was replaced from outside - a save state's -
   * is counted again from what is in it. */
  CHECK(wii_nand_open_count(wii_nand_of(m)) == 15, "fifteen files are open (%u)", wii_nand_open_count(wii_nand_of(m)));
  wii_state(m)->ios.files[files[3]].open = 0;
  wii_fs_descriptors_replaced(m);
  CHECK(wii_nand_open_count(wii_nand_of(m)) == 14, "fourteen after one went (%u)", wii_nand_open_count(wii_nand_of(m)));
  EXPECT(fs_delete(m, fs, "/tmp/03"), 0);
  EXPECT(fs_delete(m, fs, "/tmp/04"), WII_FS_EFDOPEN);

  for (i = 0; i < 15u; i++) {
    if (i != 3u) EXPECT(ios_close(m, files[i]), 0);
  }
  CHECK(wii_nand_open_count(wii_nand_of(m)) == 0, "all closed");
  EXPECT(ios_close(m, fs), 0);
  discard(m);
}

/* Requests take time in proportion to what they move, and the same time
 * every run. */
typedef struct Times {
  uint64_t create, refused, write_big, read_big, read_small, close_read, close_written, end;
} Times;

static void timed_run(Times *t) {
  GcMachine *m = fresh();
  int32_t fs, file;
  fs = ios_open(m, "/dev/fs", 0);
  EXPECT(fs_create(m, fs, 0, "/tmp/t", 3, 3, 3), 0);
  t->create = took;
  EXPECT(fs_create(m, fs, 0, "/tmp/t", 3, 3, 3), WII_FS_EEXIST);
  t->refused = took;
  file = ios_open(m, "/tmp/t", 3);
  fill(m, DATA, 0x10000, 3);
  EXPECT(ios_write(m, file, DATA, 0x10000), 0x10000);
  t->write_big = took;
  EXPECT(ios_seek(m, file, 0, 0), 0);
  EXPECT(ios_read(m, file, DATA, 0x10000), 0x10000);
  t->read_big = took;
  EXPECT(ios_seek(m, file, 0, 0), 0);
  EXPECT(ios_read(m, file, DATA, 0x20), 0x20);
  t->read_small = took;
  EXPECT(ios_close(m, file), 0);
  t->close_written = took;
  file = ios_open(m, "/tmp/t", 1);
  EXPECT(ios_read(m, file, DATA, 0x20), 0x20);
  EXPECT(ios_close(m, file), 0);
  t->close_read = took;
  EXPECT(ios_close(m, fs), 0);
  t->end = m->cpu.cycles;
  discard(m);
}

static void test_time(void) {
  /* The three measured figures, in timebase ticks of twelve cycles: a
   * 16 KB cluster read, one written, and the file table rewritten. */
  const uint64_t cluster_read = 125000u * 12u, cluster_write = 300000u * 12u, table = 3350000u * 12u;
  Times first, second;
  memset(&first, 0, sizeof(first));
  memset(&second, 0, sizeof(second));
  timed_run(&first);
  timed_run(&second);
  CHECK(memcmp(&first, &second, sizeof(first)) == 0, "two runs take the same cycles (%llu, %llu)",
        (unsigned long long)first.end, (unsigned long long)second.end);
  CHECK(first.read_big == WII_IOS_REPLY_CYCLES + 4u * cluster_read, "a 64 KB read takes four clusters' time (%llu)",
        (unsigned long long)first.read_big);
  CHECK(first.read_small == WII_IOS_REPLY_CYCLES + 0x20u * cluster_read / 0x4000u, "a 32-byte read takes %llu",
        (unsigned long long)first.read_small);
  CHECK(first.write_big == WII_IOS_REPLY_CYCLES + 4u * cluster_write, "a 64 KB write takes %llu",
        (unsigned long long)first.write_big);
  CHECK(first.create == WII_IOS_REPLY_CYCLES + table, "making a file rewrites the table (%llu)",
        (unsigned long long)first.create);
  CHECK(first.refused == WII_IOS_REPLY_CYCLES, "a request that is refused writes nothing (%llu)",
        (unsigned long long)first.refused);
  CHECK(first.close_written == WII_IOS_REPLY_CYCLES + table, "closing a file that was written rewrites it too (%llu)",
        (unsigned long long)first.close_written);
  CHECK(first.close_read == WII_IOS_REPLY_CYCLES, "and closing one that was only read does not (%llu)",
        (unsigned long long)first.close_read);
  /* In a person's units, at 729 cycles a microsecond: about 8.5 ms for
   * the 64 KB read, 20 ms for the write and 55 ms for the table. */
  CHECK(first.read_big / 729u > 8000u && first.read_big / 729u < 9000u, "the read is %llu microseconds",
        (unsigned long long)(first.read_big / 729u));
  CHECK(first.write_big / 729u > 19500u && first.write_big / 729u < 20500u, "the write is %llu microseconds",
        (unsigned long long)(first.write_big / 729u));
  CHECK(first.create / 729u > 55000u && first.create / 729u < 56000u, "making a file is %llu microseconds",
        (unsigned long long)(first.create / 729u));
}

/* Whose name requests are made in. */
static void test_identity(void) {
  GcMachine *m = fresh();
  struct GcDisc disc;
  uint32_t uid = 0;
  uint16_t gid = 0;
  int32_t fs;
  const uint8_t *out;
  const uint64_t title = 0x000100004e435445ull;

  wii_fs_identity(m, &uid, &gid);
  CHECK(uid == 0x1000 && gid == 0x3031, "with no disc: user %x, group %x", (unsigned)uid, gid);
  /* A disc's maker code is the group. */
  memset(&disc, 0, sizeof(disc));
  memcpy(disc.game_code, "NCTE5Z", 7);
  m->disc = &disc;
  wii_fs_identity(m, &uid, &gid);
  CHECK(uid == 0x1000 && gid == 0x355a, "with a disc: user %x, group %x", (unsigned)uid, gid);

  /* Until the title has directories nobody can save in them... */
  fs = ios_open(m, "/dev/fs", 0);
  EXPECT(fs_create(m, fs, 0, "/title/00010000/4e435445/data/save", 3, 0, 0), WII_FS_ENOENT);
  /* ...and once the title manager has made them, the title is whoever
   * the table of user numbers says, and they are its own. */
  EXPECT(wii_nand_add_title(wii_nand_of(m), title, gid), 0);
  wii_state(m)->title_id = title;
  wii_fs_identity(m, &uid, &gid);
  CHECK(uid == 0x1001 && gid == 0x355a, "as a title: user %x, group %x", (unsigned)uid, gid);
  EXPECT(fs_create(m, fs, 0, "/title/00010000/4e435445/data/save", 3, 0, 0), 0);
  EXPECT(fs_get_attr(m, fs, "/title/00010000/4e435445/data/save"), 0);
  out = ram(m, OUT, 0x4c);
  CHECK(wii_be32(out) == 0x1001 && wii_be16(out + 4) == 0x355a, "the save is the title's (%x, %x)",
        (unsigned)wii_be32(out), (unsigned)wii_be16(out + 4));
  EXPECT(fs_create(m, fs, 0, "/title/00010000/4e435445/content/x", 3, 0, 0), WII_FS_EACCESS);
  /* Another title cannot see them. */
  wii_state(m)->title_id = 0x000100004f544845ull;
  EXPECT(fs_get_attr(m, fs, "/title/00010000/4e435445/data/save"), WII_FS_EACCESS);
  EXPECT(ios_open(m, "/title/00010000/4e435445/data/save", 1), WII_FS_EACCESS);
  wii_state(m)->title_id = title;
  EXPECT(ios_close(m, fs), 0);
  m->disc = NULL;
  discard(m);
}

/* What a retail game's libraries do, request for request. No retail code
 * runs here; the sequences are the SDK's as decompiled in doldecomp/ogws
 * (NANDOpenClose.c, NANDCore.c, NANDCheck.c, scsystem.c), and each depends
 * on an answer libogc never asks for. */
static void test_sdk_sequences(void) {
  static const char *const user_directories[] = {
      "/meta",           "/ticket",         "/title/00010000", "/title/00010001", "/title/00010003",
      "/title/00010004", "/title/00010005", "/title/00010006", "/title/00010007", "/shared2/title",
  };
#define HOME "/title/00010000/4e435445/data"
  GcMachine *m = fresh();
  const uint64_t title = 0x000100004e435445ull;
  int32_t fs, original, temporary, result;
  uint32_t count = 0, clusters = 0, inodes = 0;
  const uint8_t *out = ram(m, OUT, 0x50);
  uint8_t permissions[4];
  unsigned i;

  EXPECT(wii_nand_add_title(wii_nand_of(m), title, 0x3031), 0);
  wii_state(m)->title_id = title;
  fs = ios_open(m, "/dev/fs", 0);

  /* A first save: NANDCreate, NANDOpen, NANDWrite, NANDClose. */
  EXPECT(fs_create(m, fs, 0, HOME "/save.bin", 3, 3, 0), 0);
  original = ios_open(m, HOME "/save.bin", 2);
  fill(m, DATA, 0x800, 1);
  EXPECT(ios_write(m, original, DATA, 0x800), 0x800);
  EXPECT(ios_close(m, original), 0);

  /* NANDSafeOpen for writing: a scratch directory everyone shares (made
   * if it is not there, and "it is there" is as good), the old file's
   * attributes, the old file open to read, a directory of the game's own
   * under the scratch one, a new file in it with the old one's
   * attributes, and the old contents copied across. */
  EXPECT(fs_create(m, fs, 1, "/tmp/sys", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 1, "/tmp/sys", 3, 3, 3), WII_FS_EEXIST);
  EXPECT(fs_get_attr(m, fs, HOME "/save.bin"), 0);
  memcpy(permissions, out + 0x46, 4);
  original = ios_open(m, HOME "/save.bin", 1);
  CHECK(original >= 0, "the old save opens (%d)", (int)original);
  EXPECT(fs_create(m, fs, 1, "/tmp/sys/00000000", 3, 0, 0), 0);
  attr_block(m, IN, "/tmp/sys/00000000/save.bin", 0xdeadbeefu, 0xbeef, permissions[0], permissions[1], permissions[2],
             permissions[3]);
  EXPECT(ios_ioctl(m, fs, 9, IN, 0x4c, 0, 0), 0);
  temporary = ios_open(m, "/tmp/sys/00000000/save.bin", 2);
  CHECK(temporary >= 0, "the new save opens (%d)", (int)temporary);
  for (i = 0; i < 8u; i++) {
    result = ios_read(m, original, DATA + 0x8000u, 0x400);
    if (result <= 0) break;
    EXPECT(ios_write(m, temporary, DATA + 0x8000u, (uint32_t)result), result);
  }
  CHECK(result == 0 && i == 2, "the copy ends on a read of nothing (%d after %u)", (int)result, i);
  EXPECT(ios_seek(m, temporary, 0, 0), 0);
  /* The game writes its save. */
  fill(m, DATA, 0x1000, 2);
  EXPECT(ios_write(m, temporary, DATA, 0x1000), 0x1000);
  /* NANDSafeClose: both closed, the new file moved over the old, and the
   * game's scratch directory deleted. */
  EXPECT(ios_close(m, temporary), 0);
  EXPECT(ios_close(m, original), 0);
  EXPECT(fs_rename(m, fs, "/tmp/sys/00000000/save.bin", HOME "/save.bin"), 0);
  EXPECT(fs_delete(m, fs, "/tmp/sys/00000000"), 0);
  EXPECT(fs_count(m, fs, "/tmp/sys", &count), 0);
  CHECK(count == 0, "nothing is left in the scratch directory (%u)", (unsigned)count);
  original = ios_open(m, HOME "/save.bin", 1);
  EXPECT(ios_read(m, original, DATA + 0x8000u, 0x4000), 0x1000);
  CHECK(memcmp(ram(m, DATA, 0x1000), ram(m, DATA + 0x8000u, 0x1000), 0x1000) == 0, "the save is the new one");
  EXPECT(ios_close(m, original), 0);
  EXPECT(fs_get_attr(m, fs, HOME "/save.bin"), 0);
  CHECK(wii_be32(out) == 0x1001 && wii_be16(out + 4) == 0x3031 && memcmp(out + 0x46, permissions, 4) == 0,
        "the game's, with the attributes the old one had");
  EXPECT(fs_count(m, fs, HOME, &count), 0);
  CHECK(count == 1, "and there is one of it (%u)", (unsigned)count);

  /* NANDGetType: a path is a file if listing it is invalid, a directory
   * if it lists or may not be listed, and anything else is the error. */
  EXPECT(fs_count(m, fs, HOME "/save.bin", &count), WII_FS_EINVAL);
  EXPECT(fs_count(m, fs, HOME, &count), 0);
  EXPECT(fs_count(m, fs, "/title/00010000/4e435445/content", &count), WII_FS_EACCESS);
  EXPECT(fs_count(m, fs, HOME "/none", &count), WII_FS_ENOENT);

  /* NANDCheck: what the game's directory uses, then each of the user
   * directories - where "not there" is forgiven and nothing else is, so
   * a directory the game may not read must still answer. */
  EXPECT(fs_usage(m, fs, HOME, &clusters, &inodes), 0);
  CHECK(clusters == 1 && inodes == 2, "the game uses one cluster and two entries (%u, %u)", (unsigned)clusters,
        (unsigned)inodes);
  for (i = 0; i < sizeof(user_directories) / sizeof(user_directories[0]); i++) {
    result = fs_usage(m, fs, user_directories[i], &clusters, &inodes);
    CHECK(result == 0 || result == WII_FS_ENOENT, "usage of %s: %d", user_directories[i], (int)result);
  }

  /* SCInit: both settings files, whole, opened for reading. */
  original = ios_open(m, "/shared2/sys/SYSCONF", 1);
  EXPECT(ios_read(m, original, DATA, 0x4000), 0x4000);
  EXPECT(ios_close(m, original), 0);
  CHECK(memcmp(ram(m, DATA, 4), "SCv0", 4) == 0 && memcmp(ram(m, DATA + 0x3ffcu, 4), "SCed", 4) == 0,
        "SYSCONF, magic to magic");
  original = ios_open(m, "/title/00000001/00000002/data/setting.txt", 1);
  EXPECT(ios_read(m, original, DATA + 0x8000u, 0x100), 0x100);
  EXPECT(ios_close(m, original), 0);
  /* SCFlush: the file must be a file, and open to everyone, or the SDK
   * deletes it and makes it again; then it is opened to write and written
   * whole. */
  EXPECT(fs_count(m, fs, "/shared2/sys/SYSCONF", &count), WII_FS_EINVAL);
  EXPECT(fs_get_attr(m, fs, "/shared2/sys/SYSCONF"), 0);
  CHECK(out[0x46] == 3 && out[0x47] == 3 && out[0x48] == 3, "SYSCONF is everyone's to read and write");
  original = ios_open(m, "/shared2/sys/SYSCONF", 2);
  CHECK(original >= 0, "a game may open it to write (%d)", (int)original);
  ram(m, DATA + 0x100u, 1)[0] ^= 0xffu;
  EXPECT(ios_write(m, original, DATA, 0x4000), 0x4000);
  EXPECT(ios_close(m, original), 0);
  {
    const uint8_t *stored = NULL;
    uint32_t size = 0;
    EXPECT(wii_nand_get(wii_nand_of(m), "/shared2/sys/SYSCONF", &stored, &size), 0);
    CHECK(size == 0x4000 && memcmp(stored, ram(m, DATA, 0x4000), 0x4000) == 0, "and what it wrote is the file");
  }
  /* The other way the SDK repairs it: delete, make the directory if it is
   * gone, make the file. */
  EXPECT(fs_delete(m, fs, "/shared2/sys/SYSCONF"), 0);
  EXPECT(fs_count(m, fs, "/shared2/sys", &count), 0);
  EXPECT(fs_create(m, fs, 0, "/shared2/sys/SYSCONF", 3, 3, 3), 0);

  EXPECT(ios_close(m, fs), 0);
  discard(m);
#undef HOME
}

/* Shutting down, and starting again. */
static void test_shutdown(void) {
  GcMachine *m = fresh();
  int32_t fs, file;
  uint32_t count = 0;

  fs = ios_open(m, "/dev/fs", 0);
  EXPECT(fs_create(m, fs, 0, "/shared2/keep", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 1, "/tmp/sys", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 0, "/tmp/sys/scratch", 3, 3, 3), 0);
  file = ios_open(m, "/shared2/keep", 3);
  CHECK(file >= 0, "a file is open");

  /* 13: the console is about to lose power. */
  EXPECT(ios_ioctl(m, fs, 13, 0, 0, 0, 0), 0);
  EXPECT(ios_ioctl(m, fs, 2, 0, 0, OUT, 0x1c), WII_FS_ESHUTDOWN);
  EXPECT(fs_create(m, fs, 0, "/tmp/late", 3, 3, 3), WII_FS_ESHUTDOWN);
  EXPECT(fs_count(m, fs, "/tmp", &count), WII_FS_ESHUTDOWN);
  EXPECT(ios_write(m, file, DATA, 4), WII_FS_ESHUTDOWN);
  EXPECT(ios_read(m, file, DATA, 4), WII_FS_ESHUTDOWN);
  EXPECT(ios_seek(m, file, 0, 0), WII_FS_ESHUTDOWN);
  EXPECT(ios_open(m, "/shared2/keep", 1), WII_FS_ESHUTDOWN);
  EXPECT(ios_open(m, "/dev/fs", 0), WII_FS_ESHUTDOWN);
  /* A descriptor can always be given back. */
  EXPECT(ios_close(m, file), 0);
  EXPECT(ios_close(m, fs), 0);

  /* IOS starts again: descriptors are gone, /tmp is empty, and the flash
   * is what it was. */
  file = ios_open(m, "/dev/es", 0);
  wii_ios_reset(m);
  CHECK(wii_nand_open_count(wii_nand_of(m)) == 0, "nothing is open after a reset");
  fs = ios_open(m, "/dev/fs", 0);
  CHECK(fs >= 0, "the filesystem answers again (%d)", (int)fs);
  EXPECT(fs_count(m, fs, "/tmp", &count), 0);
  CHECK(count == 0, "/tmp was emptied (%u)", (unsigned)count);
  EXPECT(fs_get_attr(m, fs, "/shared2/keep"), 0);
  EXPECT(fs_get_attr(m, fs, "/shared2/sys/SYSCONF"), 0);

  /* A reset with a file open forgets that it was. */
  file = ios_open(m, "/shared2/keep", 3);
  CHECK(file >= 0, "open again");
  wii_ios_reset(m);
  fs = ios_open(m, "/dev/fs", 0);
  EXPECT(fs_delete(m, fs, "/shared2/keep"), 0);
  EXPECT(ios_close(m, fs), 0);
  discard(m);
}

/* ------------------------------------------------------------------ */
/* The file on the host                                                */

static int on_disk(const char *path, const char *name, const uint8_t **data, uint32_t *size, WiiNand **keep) {
  char reason[160];
  int missing = 0;
  WiiNand *nand = wii_nand_load(path, &missing, reason, sizeof(reason));
  int there;
  if (!nand) {
    CHECK(0, "%s does not load: %s", path, reason);
    return 0;
  }
  there = wii_nand_get(nand, name, data, size) == 0;
  if (keep && there) {
    *keep = nand;
  } else {
    wii_nand_destroy(nand);
  }
  return there;
}

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

static void test_host_file(void) {
  char path[512], temporary[520], log_path[520], line[256];
  GcMachine *m = fresh();
  int32_t fs, file;
  const uint8_t *data = NULL;
  uint32_t size = 0;
  uint64_t changed;
  WiiNand *loaded = NULL;
  FILE *log;

  snprintf(path, sizeof(path), "%s/machine.nand", scratch);
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  snprintf(log_path, sizeof(log_path), "%s/attach.log", scratch);
  remove(path);
  remove(temporary);
  remove_directory(temporary);

  /* No file yet: a console that has never been switched on. It is
   * formatted, and the file is there at once. */
  EXPECT(wii_nand_flush(m), 0);
  EXPECT(wii_nand_attach(m, path, NULL), 0);
  CHECK(on_disk(path, "/shared2/sys/SYSCONF", &data, &size, NULL), "attaching made the file, with a console in it");
  CHECK(!wii_nand_dirty(wii_nand_of(m)), "and there is nothing to write");

  /* A game saves. */
  fs = ios_open(m, "/dev/fs", 0);
  EXPECT(fs_create(m, fs, 0, "/shared2/save.bin", 3, 3, 3), 0);
  file = ios_open(m, "/shared2/save.bin", 3);
  fill(m, DATA, 0x3000, 9);
  changed = m->cpu.cycles;
  EXPECT(ios_write(m, file, DATA, 0x3000), 0x3000);
  EXPECT(ios_close(m, file), 0);
  CHECK(wii_nand_dirty(wii_nand_of(m)) && wii_nand_changed_at(wii_nand_of(m)) == changed,
        "the flash changed when the write was asked for");

  /* Nothing is written while the burst may still be going on... */
  wii_machine_field(m);
  CHECK(!on_disk(path, "/shared2/save.bin", &data, &size, NULL), "not written at once");
  m->cpu.cycles = changed + gc_cpu_hz - 1u;
  wii_machine_field(m);
  CHECK(!on_disk(path, "/shared2/save.bin", &data, &size, NULL), "nor a cycle short of a second later");
  /* ...and a second after the last change, it is. */
  m->cpu.cycles = changed + gc_cpu_hz;
  wii_machine_field(m);
  CHECK(on_disk(path, "/shared2/save.bin", &data, &size, &loaded), "written a second after the game stopped");
  if (loaded) {
    CHECK(size == 0x3000 && memcmp(data, ram(m, DATA, 0x3000), 0x3000) == 0, "with what the game wrote");
    wii_nand_destroy(loaded);
    loaded = NULL;
  }
  CHECK(!wii_nand_dirty(wii_nand_of(m)), "and the flash is clean");

  /* A host that will not take the file: said once, tried again a second
   * later, and nothing is lost meanwhile. */
  EXPECT(fs_create(m, fs, 0, "/shared2/second.bin", 3, 3, 3), 0);
  changed = m->cpu.cycles;
  CHECK(make_directory(temporary) == 0, "the test could put a directory in the way");
  m->cpu.cycles = changed + gc_cpu_hz + gc_cpu_hz / 2u;
  printf("(the next line is the write-back being refused, on purpose)\n");
  fflush(stdout);
  wii_machine_field(m);
  CHECK(wii_nand_dirty(wii_nand_of(m)), "a write-back that failed leaves the flash dirty");
  CHECK(!on_disk(path, "/shared2/second.bin", &data, &size, NULL), "and the file as it was");
  remove_directory(temporary);
  m->cpu.cycles += gc_cpu_hz / 2u;
  wii_machine_field(m);
  CHECK(wii_nand_dirty(wii_nand_of(m)), "it waits a second before trying again");
  m->cpu.cycles += gc_cpu_hz / 2u;
  wii_machine_field(m);
  CHECK(!wii_nand_dirty(wii_nand_of(m)) && on_disk(path, "/shared2/second.bin", &data, &size, NULL),
        "and then it is written");

  /* Letting go writes whatever is outstanding, at once. What is in /tmp
   * is not kept. */
  EXPECT(fs_create(m, fs, 0, "/shared2/third.bin", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 0, "/tmp/scratch", 3, 3, 3), 0);
  EXPECT(ios_close(m, fs), 0);
  discard(m);
  CHECK(on_disk(path, "/shared2/third.bin", &data, &size, NULL), "detaching wrote the last change");
  CHECK(!on_disk(path, "/tmp/scratch", &data, &size, NULL), "and not /tmp's");

  /* The next run finds the save. */
  m = fresh();
  EXPECT(wii_nand_attach(m, path, NULL), 0);
  file = ios_open(m, "/shared2/save.bin", 1);
  CHECK(file >= 0, "the save opens in a new machine (%d)", (int)file);
  memset(ram(m, DATA + 0x8000u, 0x3000), 0, 0x3000);
  fill(m, DATA, 0x3000, 9);
  EXPECT(ios_read(m, file, DATA + 0x8000u, 0x4000), 0x3000);
  CHECK(memcmp(ram(m, DATA, 0x3000), ram(m, DATA + 0x8000u, 0x3000), 0x3000) == 0, "and reads as it was written");
  EXPECT(ios_close(m, file), 0);
  /* Reading changes nothing, so nothing is written. */
  CHECK(!wii_nand_dirty(wii_nand_of(m)), "reading leaves the flash clean");
  discard(m);

  /* A second machine that only looks: it sees the save, and nothing it
   * does reaches the file - not after a second, and not when it goes. */
  m = fresh();
  EXPECT(wii_nand_attach_unbacked(m, path, NULL), 0);
  file = ios_open(m, "/shared2/save.bin", 1);
  CHECK(file >= 0, "an unbacked machine sees the save (%d)", (int)file);
  EXPECT(ios_close(m, file), 0);
  fs = ios_open(m, "/dev/fs", 0);
  EXPECT(fs_create(m, fs, 0, "/shared2/ghost", 3, 3, 3), 0);
  EXPECT(ios_close(m, fs), 0);
  m->cpu.cycles += 3u * (uint64_t)gc_cpu_hz;
  wii_machine_field(m);
  discard(m);
  CHECK(!on_disk(path, "/shared2/ghost", &data, &size, NULL), "and writes nothing");
  /* Where there is no file it makes none. */
  {
    char absent[540];
    FILE *made;
    snprintf(absent, sizeof(absent), "%s/absent.nand", scratch);
    remove(absent);
    m = fresh();
    EXPECT(wii_nand_attach_unbacked(m, absent, NULL), 0);
    fs = ios_open(m, "/dev/fs", 0);
    CHECK(fs >= 0, "an unbacked machine with no file has a fresh console (%d)", (int)fs);
    EXPECT(ios_close(m, fs), 0);
    discard(m);
    made = fopen(absent, "rb");
    CHECK(made == NULL, "and no file was made");
    if (made) fclose(made);
  }

  /* A file that is not a filesystem is refused, said why, and not
   * touched; the machine runs on a flash of its own in memory. */
  {
    static const char junk[] = "these are somebody's bytes and not a filesystem";
    size_t before_size = 0, after_size = 0;
    uint8_t *before, *after;
    FILE *f = fopen(path, "wb");
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);
    before = read_whole(path, &before_size);
    m = fresh();
    log = fopen(log_path, "w");
    EXPECT(wii_nand_attach(m, path, log), -1);
    if (log) fclose(log);
    line[0] = 0;
    log = fopen(log_path, "r");
    if (log) {
      if (!fgets(line, sizeof(line), log)) line[0] = 0;
      fclose(log);
    }
    CHECK(strstr(line, "has not been touched") && strstr(line, "not a native-console"), "the log says why: %s", line);
    fs = ios_open(m, "/dev/fs", 0);
    CHECK(fs >= 0, "the machine still has a filesystem (%d)", (int)fs);
    EXPECT(fs_create(m, fs, 0, "/shared2/x", 3, 3, 3), 0);
    m->cpu.cycles += 3u * (uint64_t)gc_cpu_hz;
    wii_machine_field(m);
    EXPECT(ios_close(m, fs), 0);
    discard(m);
    after = read_whole(path, &after_size);
    CHECK(before && after && before_size == after_size && memcmp(before, after, before_size) == 0,
          "and the file is byte for byte what it was");
    free(before);
    free(after);
  }

  /* No name is no file. */
  m = fresh();
  EXPECT(wii_nand_attach(m, "", NULL), -1);
  EXPECT(wii_nand_attach(m, NULL, NULL), -1);
  discard(m);
  remove(log_path);
}

/* A save state carries the flash - /tmp and all, which a host file never
 * has - and the descriptor table that says which of its files are open; a
 * load puts both back, keeps the flash it replaced beside the host file, and
 * leaves a machine a damaged state was offered to exactly as it was. */
static void test_save_state(void) {
  char path[512], backup[520], error[256];
  GcMachine *m = fresh();
  GcStateIdentity id;
  GcStateBuffer state;
  int32_t fs, file, scratch_file;
  const uint8_t *data = NULL;
  uint32_t size = 0;
  uint64_t hash;
  WiiNand *loaded = NULL;

  snprintf(path, sizeof(path), "%s/state.nand", scratch);
  snprintf(backup, sizeof(backup), "%s.bak", path);
  remove(path);
  remove(backup);
  memset(&state, 0, sizeof(state));
  EXPECT(wii_nand_attach(m, path, NULL), 0);
  gc_state_identity(&id, m, "fs-device-test", NULL, 0);

  /* A save half written, a file under /tmp, and a descriptor left open on
   * each, one of them part way through. */
  fs = ios_open(m, "/dev/fs", 0);
  EXPECT(fs_create(m, fs, 0, "/shared2/save.bin", 3, 3, 3), 0);
  EXPECT(fs_create(m, fs, 0, "/tmp/work.bin", 3, 3, 3), 0);
  file = ios_open(m, "/shared2/save.bin", 3);
  scratch_file = ios_open(m, "/tmp/work.bin", 3);
  fill(m, DATA, 0x2000, 3);
  EXPECT(ios_write(m, file, DATA, 0x2000), 0x2000);
  fill(m, DATA, 0x800, 4);
  EXPECT(ios_write(m, scratch_file, DATA, 0x800), 0x800);
  EXPECT(ios_seek(m, file, 0x1000, 0), 0x1000);
  EXPECT(wii_nand_flush(m), 0);

  hash = gc_state_hash(m);
  CHECK(gc_state_save_memory(m, &id, 1, &state) == 0 && state.size > 0, "a Wii's state is written");
  CHECK(gc_state_hash(m) == hash, "and writing it changes nothing");

  /* The game goes on: the save grows, the work file goes, the host's file
   * is brought up to date. */
  fill(m, DATA, 0x1000, 5);
  EXPECT(ios_seek(m, file, 0, 2), 0x2000);
  EXPECT(ios_write(m, file, DATA, 0x1000), 0x1000);
  EXPECT(ios_close(m, scratch_file), 0);
  EXPECT(fs_delete(m, fs, "/tmp/work.bin"), 0);
  EXPECT(wii_nand_flush(m), 0);
  CHECK(gc_state_hash(m) != hash, "the machine has moved on");

  /* A state with a byte changed is refused, and nothing is touched. */
  if (state.size > 400u) {
    const uint64_t before = gc_state_hash(m);
    state.data[state.size - 200u] ^= 0x40u;
    CHECK(gc_state_load_memory(m, &id, state.data, state.size, error, sizeof(error)) != 0, "a damaged state is refused");
    CHECK(gc_state_hash(m) == before, "with the machine as it was");
    CHECK(wii_nand_get(wii_nand_of(m), "/tmp/work.bin", &data, &size) != 0, "and its flash too");
    state.data[state.size - 200u] ^= 0x40u;
  }

  error[0] = 0;
  CHECK(gc_state_load_memory(m, &id, state.data, state.size, error, sizeof(error)) == 0, "the state loads (%s)", error);
  CHECK(gc_state_hash(m) == hash, "into the machine it was saved from");
  CHECK(wii_nand_get(wii_nand_of(m), "/shared2/save.bin", &data, &size) == 0 && size == 0x2000u,
        "the save is as long as it was (%u)", (unsigned)size);
  fill(m, DATA, 0x800, 4);
  CHECK(wii_nand_get(wii_nand_of(m), "/tmp/work.bin", &data, &size) == 0 && size == 0x800u &&
            memcmp(data, ram(m, DATA, 0x800), 0x800) == 0,
        "the file under /tmp is back, with what it held");
  /* Both descriptors still work, and the first is where it was. */
  fill(m, DATA, 0x2000, 3);
  EXPECT(ios_read(m, file, DATA + 0x10000, 0x1000), 0x1000);
  CHECK(memcmp(ram(m, DATA + 0x10000, 0x1000), ram(m, DATA + 0x1000, 0x1000), 0x1000) == 0,
        "a read through the descriptor that was open goes on from where it was");
  EXPECT(ios_seek(m, scratch_file, 0, 2), 0x800);
  EXPECT(fs_delete(m, fs, "/shared2/save.bin"), WII_FS_EFDOPEN);

  /* What the host's file held before the load is kept beside it, and the
   * file itself is written from the state's flash. */
  CHECK(on_disk(backup, "/shared2/save.bin", &data, &size, &loaded) && size == 0x3000u,
        "the flash the load replaced is in the .bak (%u bytes of save)", (unsigned)size);
  if (loaded) wii_nand_destroy(loaded);
  loaded = NULL;
  EXPECT(wii_nand_flush(m), 0);
  CHECK(on_disk(path, "/shared2/save.bin", &data, &size, &loaded) && size == 0x2000u,
        "and the host's file has the state's save (%u)", (unsigned)size);
  if (loaded) wii_nand_destroy(loaded);

  gc_state_buffer_free(&state);
  EXPECT(ios_close(m, file), 0);
  EXPECT(ios_close(m, scratch_file), 0);
  discard(m);

  /* A GameCube has no such sections, and is not offered them. */
  gc_console_select(0);
  {
    GcMachine *cube = gc_machine_create();
    GcStateBuffer cube_state;
    memset(&cube_state, 0, sizeof(cube_state));
    cube->log = NULL;
    gc_install_boot_state(cube);
    gc_state_identity(&id, cube, "fs-device-test", NULL, 0);
    CHECK(gc_state_save_memory(cube, &id, 0, &cube_state) == 0, "a GameCube's state is written as before");
    CHECK(gc_state_load_memory(cube, &id, cube_state.data, cube_state.size, error, sizeof(error)) == 0,
          "and loads (%s)", error);
    gc_state_buffer_free(&cube_state);
    gc_machine_destroy(cube);
  }
  gc_console_select(1);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: wii_fs_device_test <scratch directory>\n");
    return 2;
  }
  scratch = argv[1];
  gc_console_select(1);
  test_requests();
  test_descriptors();
  test_time();
  test_identity();
  test_sdk_sequences();
  test_shutdown();
  test_host_file();
  test_save_state();
  printf("wii_fs_device_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
