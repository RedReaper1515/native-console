/*
 * The disc, from a game's side of IOS.
 *
 * This program is the DOL of the bootable test disc
 * (tools/wii/make_boot_disc.py): it got here through the disc's encrypted
 * partition and its apploader, which is the first thing it checks. Then it
 * asks /dev/di and /dev/es what a game asks them, and holds every byte read
 * to the rules the disc's files were filled by (tests/wii/wii_test_disc.h).
 *
 * Run from the disc it is on. Loaded as a bare ELF there is no disc to ask,
 * and it says so.
 */
#include <gccore.h>
#include <ogc/ipc.h>
#include <stdlib.h>
#include <string.h>

#include "gc_test_report.h"
#include "wii_test_disc.h"

static char line[256];

/* Requests are passed by address, so their buffers sit on cache lines of
 * their own. */
static u32 command[8] ATTRIBUTE_ALIGN(32);
static u8 buffer[0xa000] ATTRIBUTE_ALIGN(32);
static u8 metadata[0x49e4 + 28] ATTRIBUTE_ALIGN(32);
static u32 verdict[8] ATTRIBUTE_ALIGN(32);
static ioctlv vectors[5] ATTRIBUTE_ALIGN(32);
static u64 title ATTRIBUTE_ALIGN(32);
static u32 count ATTRIBUTE_ALIGN(32);

static s32 di_command(s32 fd, u32 number, u32 a, u32 b, void *out, u32 out_size) {
  memset(command, 0, sizeof(command));
  command[0] = number << 24;
  command[1] = a;
  command[2] = b;
  return IOS_Ioctl(fd, number, command, 0x20, out, out_size);
}

static void test_boot(void) {
  const u32 fst = *(u32 *)0x80000038;
  gc_test_begin("the disc booted this program");
  GC_CHECK(memcmp((void *)0x80000000, WII_TEST_GAME_CODE, 6) == 0, "the disc's id is in low memory");
  GC_CHECK_EQ(*(u32 *)0x80000018, 0x5d1c9ea3u, "with a Wii disc's magic word");
  GC_CHECK(memcmp((void *)0x80003180, WII_TEST_GAME_CODE, 4) == 0, "the title's four letters are at 0x80003180");
  snprintf(line, sizeof(line), "the apploader left the filesystem in MEM1 (%08x)", (unsigned)fst);
  GC_CHECK(fst >= 0x80000000u && fst < 0x81800000u, line);
  /* What it put there is not checked: libogc takes all of MEM1 for its
   * own arena and has cleared it by now, where a retail game's library
   * would have read it. test_reads reads the filesystem off the disc. */
  gc_test_end();
}

static void test_reads(s32 fd) {
  u32 i, wrong;
  s32 result;

  gc_test_begin("the drive, through IOS");
  memset(buffer, 0, 0x20);
  result = di_command(fd, 0x70, 0, 0, buffer, 0x20);
  GC_CHECK_EQ(result, 1, "reading the disc's id");
  GC_CHECK(memcmp(buffer, WII_TEST_GAME_CODE, 6) == 0, "which is the disc's");

  result = di_command(fd, 0x88, 0, 0, buffer, 0x20);
  GC_CHECK_EQ(result, 1, "asking whether a disc is in");
  GC_CHECK_EQ(*(u32 *)buffer, 2u, "one is");

  /* The filesystem, found the way the apploader finds it: the disc's
   * header says where, in words. */
  result = di_command(fd, 0x71, 0x20, 0x420 >> 2, buffer, 0x20);
  GC_CHECK_EQ(result, 1, "reading where the filesystem is");
  result = di_command(fd, 0x71, 0x20, ((u32 *)buffer)[1], buffer, 0x20);
  GC_CHECK_EQ(result, 1, "reading its first entries");
  GC_CHECK_EQ(((u32 *)buffer)[2], 6u, "the root says six entries");

  /* A file inside one cluster. */
  memset(buffer, 0, 64);
  result = di_command(fd, 0x71, WII_TEST_HELLO_SIZE, WII_TEST_HELLO_AT >> 2, buffer, WII_TEST_HELLO_SIZE);
  GC_CHECK_EQ(result, 1, "reading hello.txt");
  GC_CHECK(memcmp(buffer, WII_TEST_HELLO, sizeof(WII_TEST_HELLO) - 1) == 0, "which says hello");

  /* One that starts in the first cluster and ends in the third. */
  result = di_command(fd, 0x71, WII_TEST_PATTERN_SIZE, WII_TEST_PATTERN_AT >> 2, buffer, WII_TEST_PATTERN_SIZE);
  GC_CHECK_EQ(result, 1, "reading data/pattern.bin");
  for (i = 0, wrong = 0; i < WII_TEST_PATTERN_SIZE; i++) wrong += buffer[i] != wii_test_pattern_byte(i);
  GC_CHECK_EQ(wrong, 0u, "bytes of it that are not what the rule says");

  /* The middle of the big one, from an offset that is not a cluster's. */
  result = di_command(fd, 0x71, 0x8000, (WII_TEST_BIG_AT + 0x23450u) >> 2, buffer, 0x8000);
  GC_CHECK_EQ(result, 1, "reading from the middle of data/big.bin");
  for (i = 0, wrong = 0; i < 0x8000u; i++) wrong += buffer[i] != wii_test_big_byte(0x23450u + i);
  GC_CHECK_EQ(wrong, 0u, "bytes of that which are not what the rule says");

  /* Past the end of the partition is the drive's error, and asking what
   * went wrong clears it. */
  result = di_command(fd, 0x71, 0x20, 0x3ffffff0u, buffer, 0x20);
  GC_CHECK_EQ(result, 2, "a read past the end of the partition");
  result = di_command(fd, 0xe0, 0, 0, buffer, 0x20);
  GC_CHECK(result == 1 && *(u32 *)buffer != 0, "leaves an error to ask about");
  result = di_command(fd, 0xe0, 0, 0, buffer, 0x20);
  GC_CHECK(result == 1 && *(u32 *)buffer == 0, "which asking clears");
  gc_test_end();
}

static void test_partition(s32 fd) {
  u32 partition;
  s32 result;

  gc_test_begin("opening and closing the partition");
  /* The partition table is outside any partition: two entries, the second
   * of them the game. */
  memset(buffer, 0, 0x40);
  result = di_command(fd, 0x8d, 0x20, 0x40000 >> 2, buffer, 0x20);
  GC_CHECK_EQ(result, 1, "an unencrypted read of the partition table's header");
  GC_CHECK_EQ(*(u32 *)buffer, 2u, "which lists two partitions");
  result = di_command(fd, 0x8d, 0x20, 0x40020 >> 2, buffer, 0x20);
  GC_CHECK_EQ(result, 1, "and of its entries");
  GC_CHECK_EQ(((u32 *)buffer)[1], 1u, "the first is the system update");
  GC_CHECK_EQ(((u32 *)buffer)[3], 0u, "the second is the game");
  partition = ((u32 *)buffer)[2];

  result = di_command(fd, 0x8c, 0, 0, NULL, 0);
  GC_CHECK_EQ(result, 1, "closing the partition");
  result = di_command(fd, 0x71, 0x20, WII_TEST_HELLO_AT >> 2, buffer, 0x20);
  GC_CHECK_EQ(result, 0x20, "a read with no partition open is refused");

  /* The update partition's key is not the game's: it does not open. */
  memset(command, 0, sizeof(command));
  command[0] = 0x8b000000u;
  command[1] = WII_TEST_UPDATE_PARTITION >> 2;
  vectors[0].data = command;
  vectors[0].len = 0x20;
  vectors[1].data = NULL;
  vectors[1].len = 0;
  vectors[2].data = NULL;
  vectors[2].len = 0;
  vectors[3].data = metadata;
  vectors[3].len = 0x49e4;
  vectors[4].data = verdict;
  vectors[4].len = 0x20;
  result = IOS_Ioctlv(fd, 0x8b, 3, 2, vectors);
  GC_CHECK(result != 1, "the update partition does not open");

  memset(metadata, 0, sizeof(metadata));
  command[1] = partition;
  result = IOS_Ioctlv(fd, 0x8b, 3, 2, vectors);
  GC_CHECK_EQ(result, 1, "the game partition opens");
  GC_CHECK(memcmp(metadata + 0x18c + 4, WII_TEST_GAME_CODE, 4) == 0, "and hands back the title's metadata");
  memset(buffer, 0, 64);
  result = di_command(fd, 0x71, WII_TEST_HELLO_SIZE, WII_TEST_HELLO_AT >> 2, buffer, WII_TEST_HELLO_SIZE);
  GC_CHECK(result == 1 && memcmp(buffer, WII_TEST_HELLO, sizeof(WII_TEST_HELLO) - 1) == 0, "after which reads work again");
  gc_test_end();
}

static void test_title(void) {
  s32 es = IOS_Open("/dev/es", 0);
  s32 result;

  gc_test_begin("what IOS says the title is");
  GC_CHECK(es >= 0, "/dev/es opens");

  title = 0;
  vectors[0].data = &title;
  vectors[0].len = 8;
  result = IOS_Ioctlv(es, 0x20, 0, 1, vectors);
  GC_CHECK_EQ(result, 0, "the title id request");
  GC_CHECK_EQ(title >> 32, 0x00010000u, "a disc title");
  GC_CHECK(memcmp((u8 *)&title + 4, WII_TEST_GAME_CODE, 4) == 0, "with the disc's four letters");

  /* Its tickets: one, whose view names the same title. */
  count = 99;
  vectors[0].data = &title;
  vectors[0].len = 8;
  vectors[1].data = &count;
  vectors[1].len = 4;
  result = IOS_Ioctlv(es, 0x12, 1, 1, vectors);
  GC_CHECK(result == 0 && count == 1, "it has one ticket");
  memset(buffer, 0xee, 0x100);
  vectors[1].data = &count;
  vectors[1].len = 4;
  vectors[2].data = buffer;
  vectors[2].len = 0xd8;
  result = IOS_Ioctlv(es, 0x13, 2, 1, vectors);
  GC_CHECK_EQ(result, 0, "the ticket's view");
  GC_CHECK(memcmp(buffer + 0x10 + 4, WII_TEST_GAME_CODE, 4) == 0, "which is this title's");

  /* The metadata's view: a header and one content. */
  count = 0;
  vectors[1].data = &count;
  vectors[1].len = 4;
  result = IOS_Ioctlv(es, 0x14, 1, 1, vectors);
  GC_CHECK_EQ(result, 0, "the size of the metadata's view");
  GC_CHECK_EQ(count, 0x5cu + 0x10u, "a header and one content");
  gc_test_end();
  IOS_Close(es);
}

int main(void) {
  s32 fd;
  VIDEO_Init();
  if (memcmp((void *)0x80000000, WII_TEST_GAME_CODE, 6) != 0) {
    gc_test_begin("there is a disc");
    GC_CHECK(0, "this program was not booted from the test disc (tools/wii/make_boot_disc.py)");
    gc_test_end();
    gc_test_finish();
  }
  test_boot();
  fd = IOS_Open("/dev/di", 0);
  gc_test_begin("the drive is a device");
  GC_CHECK(fd >= 0, "/dev/di opens");
  gc_test_end();
  test_reads(fd);
  test_partition(fd);
  test_title();
  IOS_Close(fd);
  gc_test_finish();
  return 0;
}
