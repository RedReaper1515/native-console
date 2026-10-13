/*
 * The flash filesystem and the console's settings, from a program's side:
 * libogc's ISFS and CONF, used the way homebrew uses them, against
 * whatever answers /dev/fs.
 *
 * Everything is done under /tmp, which every title may write in and which
 * IOS empties when it starts, so the program leaves nothing behind on a
 * console. The expected answers are ISFS's own: the error numbers, a file
 * that keeps its name when it moves, a directory listed newest first,
 * sixteen descriptors.
 *
 * Verdicts are printed in the GameCube programs' format
 * (tests/gc/programs/gc_test_report.h).
 */
#include <gccore.h>
#include <ogc/conf.h>
#include <ogc/isfs.h>
#include <stdio.h>
#include <string.h>

#include "gc_test_report.h"

static char line[256];
static char message[320];
static u8 block[0x4000] ATTRIBUTE_ALIGN(32);
static u8 back[0x4000] ATTRIBUTE_ALIGN(32);
static char names[13 * 16] ATTRIBUTE_ALIGN(32);
static fstats file_stats ATTRIBUTE_ALIGN(32);
static u32 chip[7] ATTRIBUTE_ALIGN(32);

/* A call that should give exactly this result. */
#define EXPECT(call, want, what)                                                                  \
  do {                                                                                            \
    const s32 got_ = (s32)(call);                                                                 \
    snprintf(message, sizeof(message), "%.200s: got %d, want %d", (what), (int)got_, (int)(want)); \
    GC_CHECK(got_ == (s32)(want), message);                                                       \
  } while (0)

#define ROOT "/tmp/fstest"

static void test_directories(void) {
  u32 owner = 0;
  u16 group = 0;
  u8 attributes = 0, owner_perm = 0, group_perm = 0, other_perm = 0;

  gc_test_begin("the filesystem answers");
  EXPECT(ISFS_Initialize(), 0, "ISFS_Initialize");
  EXPECT(ISFS_GetStats(chip), 0, "the chip's figures");
  GC_CHECK_EQ(chip[0], 0x4000, "clusters are 16 KB");
  GC_CHECK_EQ(chip[1] + chip[2] + chip[3] + chip[4], 0x7ec0,
              "free, used, bad and reserved are the clusters the filesystem counts");
  snprintf(line, sizeof(line), "some of the file table is in use (%u used, %u free)", (unsigned)chip[6],
           (unsigned)chip[5]);
  GC_CHECK(chip[6] > 0 && chip[5] > 0, line);

  ISFS_Delete(ROOT);
  EXPECT(ISFS_CreateDir(ROOT, 0x11, 3, 3, 1), 0, "make a directory in /tmp");
  EXPECT(ISFS_CreateDir(ROOT, 0, 3, 3, 3), -105, "which is then there");
  EXPECT(ISFS_GetAttr(ROOT, &owner, &group, &attributes, &owner_perm, &group_perm, &other_perm), 0,
         "its attributes");
  snprintf(line, sizeof(line), "it belongs to whoever made it (user %x, group %04x)", (unsigned)owner, group);
  GC_CHECK(owner >= 0x1000 && group != 0, line);
  GC_CHECK(attributes == 0x11 && owner_perm == 3 && group_perm == 3 && other_perm == 1,
           "with the attribute byte and permissions it was made with");
  EXPECT(ISFS_SetAttr(ROOT, owner, group, 0x22, 3, 3, 3), 0, "its owner may change them");
  EXPECT(ISFS_GetAttr(ROOT, &owner, &group, &attributes, &owner_perm, &group_perm, &other_perm), 0, "read back");
  GC_CHECK(attributes == 0x22 && other_perm == 3, "and they changed");
  EXPECT(ISFS_GetAttr(ROOT "/none", &owner, &group, &attributes, &owner_perm, &group_perm, &other_perm), -106,
         "attributes of nothing");
  gc_test_end();
}

static void test_contents(void) {
  s32 fd, second;
  unsigned i;

  gc_test_begin("a file's contents");
  for (i = 0; i < sizeof(block); i++) block[i] = (u8)(i * 31u + (i >> 8) + 5u);
  EXPECT(ISFS_Open(ROOT "/data.bin", ISFS_OPEN_RW), -106, "open a file that is not there");
  EXPECT(ISFS_CreateFile(ROOT "/data.bin", 0, 3, 3, 0), 0, "make it");
  EXPECT(ISFS_CreateFile(ROOT "/data.bin", 0, 3, 3, 0), -105, "twice");
  fd = ISFS_Open(ROOT "/data.bin", ISFS_OPEN_RW);
  snprintf(line, sizeof(line), "open it (%d)", (int)fd);
  GC_CHECK(fd >= 0, line);

  EXPECT(ISFS_GetFileStats(fd, &file_stats), 0, "an empty file's length");
  GC_CHECK(file_stats.file_length == 0 && file_stats.file_pos == 0, "which is nothing");
  EXPECT(ISFS_Write(fd, block, 5000), 5000, "write 5000 bytes");
  EXPECT(ISFS_Write(fd, block + 5000, 0x2000), 0x2000, "and 8192 more");
  EXPECT(ISFS_GetFileStats(fd, &file_stats), 0, "the length now");
  GC_CHECK_EQ(file_stats.file_length, 5000 + 0x2000, "13192 bytes");
  GC_CHECK_EQ(file_stats.file_pos, 5000 + 0x2000, "and the position at the end");

  EXPECT(ISFS_Seek(fd, 0, 0), 0, "seek to the start");
  memset(back, 0, sizeof(back));
  EXPECT(ISFS_Read(fd, back, 5000 + 0x2000), 5000 + 0x2000, "read it all");
  GC_CHECK(memcmp(back, block, 5000 + 0x2000) == 0, "it is what was written");
  EXPECT(ISFS_Read(fd, back, 32), 0, "nothing after the end");
  EXPECT(ISFS_Seek(fd, 100, 0), 100, "seek from the start");
  EXPECT(ISFS_Seek(fd, 50, 1), 150, "from the position");
  EXPECT(ISFS_Read(fd, back, 64), 64, "read there");
  GC_CHECK(memcmp(back, block + 150, 64) == 0, "the right 64 bytes");
  EXPECT(ISFS_Seek(fd, -10, 2), 5000 + 0x2000 - 10, "seek from the end");
  EXPECT(ISFS_Read(fd, back, 64), 10, "a read that reaches the end is short");
  GC_CHECK(memcmp(back, block + 5000 + 0x2000 - 10, 10) == 0, "and right");
  EXPECT(ISFS_Seek(fd, 1, 2), -101, "not past the end");
  EXPECT(ISFS_Seek(fd, -1, 0), -101, "not before the start");
  EXPECT(ISFS_Seek(fd, 0, 7), -101, "not from nowhere");

  /* Overwrite in the middle. */
  EXPECT(ISFS_Seek(fd, 1000, 0), 1000, "back to the middle");
  EXPECT(ISFS_Write(fd, block, 32), 32, "overwrite 32 bytes");
  EXPECT(ISFS_Seek(fd, 992, 0), 992, "just before them");
  EXPECT(ISFS_Read(fd, back, 64), 64, "read across them");
  GC_CHECK(memcmp(back, block + 992, 8) == 0 && memcmp(back + 8, block, 32) == 0 &&
               memcmp(back + 40, block + 1032, 24) == 0,
           "old bytes, new bytes, old bytes");
  EXPECT(ISFS_GetFileStats(fd, &file_stats), 0, "the length after overwriting");
  GC_CHECK_EQ(file_stats.file_length, 5000 + 0x2000, "is unchanged");

  /* A second descriptor: its own position, and only the mode it asked for. */
  second = ISFS_Open(ROOT "/data.bin", ISFS_OPEN_READ);
  snprintf(line, sizeof(line), "open it again, to read (%d, %d)", (int)fd, (int)second);
  GC_CHECK(second >= 0 && second != fd, line);
  EXPECT(ISFS_Read(second, back, 32), 32, "the second reads from the start");
  GC_CHECK(memcmp(back, block, 32) == 0, "the file's first bytes");
  EXPECT(ISFS_Write(second, block, 32), -102, "and may not write");
  EXPECT(ISFS_Close(second), 0, "close the second");
  EXPECT(ISFS_Close(fd), 0, "close the first");
  EXPECT(ISFS_Open(ROOT, ISFS_OPEN_READ), -101, "a directory is not a file");
  gc_test_end();
}

static void test_names(void) {
  u32 count = 0, clusters = 0, inodes = 0;

  gc_test_begin("names");
  EXPECT(ISFS_CreateDir(ROOT "/sub", 0, 3, 3, 3), 0, "a directory inside");
  EXPECT(ISFS_CreateFile(ROOT "/newest", 0, 3, 3, 3), 0, "and another file");
  EXPECT(ISFS_ReadDir(ROOT, NULL, &count), 0, "count the entries");
  GC_CHECK_EQ(count, 3, "three");
  count = 16;
  memset(names, 0x55, sizeof(names));
  EXPECT(ISFS_ReadDir(ROOT, names, &count), 0, "list them");
  GC_CHECK_EQ(count, 3, "three names");
  /* End to end, each closed by its zero: thirteen bytes is the room a
   * name may need, not the place the next one starts. */
  snprintf(line, sizeof(line), "newest first, end to end: %.12s, %.12s, %.12s", names, names + 7, names + 11);
  GC_CHECK(!strcmp(names, "newest") && !strcmp(names + 7, "sub") && !strcmp(names + 11, "data.bin"), line);
  count = 2;
  EXPECT(ISFS_ReadDir(ROOT, names, &count), 0, "room for two");
  GC_CHECK_EQ(count, 2, "gives two");
  /* What the SDK tells a file from a directory by. */
  EXPECT(ISFS_ReadDir(ROOT "/data.bin", NULL, &count), -101, "listing a file");
  EXPECT(ISFS_ReadDir(ROOT "/none", NULL, &count), -106, "listing nothing");

  EXPECT(ISFS_GetUsage(ROOT, &clusters, &inodes), 0, "what the directory uses");
  GC_CHECK_EQ(clusters, 1, "13192 bytes are one cluster");
  GC_CHECK_EQ(inodes, 4, "itself and three entries");
  EXPECT(ISFS_GetUsage(ROOT "/none", &clusters, &inodes), -106, "what nothing uses");
  gc_test_end();
}

static void test_moving(void) {
  s32 fd;
  u32 count = 0;

  gc_test_begin("moving and deleting");
  EXPECT(ISFS_Rename(ROOT "/data.bin", ROOT "/other.bin"), -101, "a file cannot change its name");
  EXPECT(ISFS_Rename(ROOT "/data.bin", ROOT "/sub/data.bin"), 0, "but it can change directory");
  EXPECT(ISFS_Open(ROOT "/data.bin", ISFS_OPEN_READ), -106, "it is gone from where it was");
  fd = ISFS_Open(ROOT "/sub/data.bin", ISFS_OPEN_READ);
  GC_CHECK(fd >= 0, "and is where it went");
  EXPECT(ISFS_Read(fd, back, 64), 64, "with its contents");
  GC_CHECK(memcmp(back, block, 64) == 0, "unchanged");
  EXPECT(ISFS_Delete(ROOT "/sub/data.bin"), -111, "an open file cannot be deleted");
  EXPECT(ISFS_Rename(ROOT "/sub", ROOT "/moved"), -111, "nor the directory it is in moved");
  EXPECT(ISFS_Close(fd), 0, "close it");
  /* A save, the way the SDK finishes one: the new file over the old. */
  EXPECT(ISFS_CreateFile(ROOT "/data.bin", 0, 3, 3, 0), 0, "a new file of the same name");
  EXPECT(ISFS_Rename(ROOT "/data.bin", ROOT "/sub/data.bin"), 0, "moved over the old one");
  fd = ISFS_Open(ROOT "/sub/data.bin", ISFS_OPEN_READ);
  EXPECT(ISFS_GetFileStats(fd, &file_stats), 0, "the file there now");
  GC_CHECK_EQ(file_stats.file_length, 0, "is the new, empty one");
  EXPECT(ISFS_Close(fd), 0, "close it");
  EXPECT(ISFS_Rename(ROOT "/sub", ROOT "/moved"), 0, "a directory can change its name");
  EXPECT(ISFS_ReadDir(ROOT "/moved", NULL, &count), 0, "and takes what is in it");
  GC_CHECK_EQ(count, 1, "one file");
  EXPECT(ISFS_Rename(ROOT "/none", ROOT "/moved/none"), -106, "moving nothing");
  EXPECT(ISFS_Delete(ROOT "/none"), -106, "deleting nothing");
  EXPECT(ISFS_Delete(ROOT "/newest"), 0, "deleting a file");
  EXPECT(ISFS_Delete(ROOT), 0, "deleting a directory with things in it");
  EXPECT(ISFS_ReadDir(ROOT, NULL, &count), -106, "and it is gone");
  gc_test_end();
}

static void test_refusals(void) {
  static const char *const deep[] = {
      "/tmp/1", "/tmp/1/2", "/tmp/1/2/3", "/tmp/1/2/3/4", "/tmp/1/2/3/4/5", "/tmp/1/2/3/4/5/6", "/tmp/1/2/3/4/5/6/7",
  };
  s32 fds[20];
  unsigned i, opened = 0;
  s32 result = 0;

  gc_test_begin("what is refused");
  /* What is IOS's is not a title's. */
  EXPECT(ISFS_CreateFile("/sys/mine", 0, 3, 3, 3), -102, "making a file in /sys");
  EXPECT(ISFS_CreateDir("/mine", 0, 3, 3, 3), -102, "making a directory in the root");
  EXPECT(ISFS_Open("/sys/uid.sys", ISFS_OPEN_READ), -102, "reading IOS's own table");
  EXPECT(ISFS_Delete("/title"), -102, "deleting /title");
  EXPECT(ISFS_Format(), -102, "formatting the flash");
  /* Names and depth. */
  EXPECT(ISFS_CreateFile("/tmp/thirteenchars", 0, 3, 3, 3), -101, "a name of thirteen characters");
  EXPECT(ISFS_CreateFile("/tmp/twelve_chars", 0, 3, 3, 3), 0, "a name of twelve");
  EXPECT(ISFS_Delete("/tmp/twelve_chars"), 0, "and delete it");
  EXPECT(ISFS_CreateFile("/tmp/none/file", 0, 3, 3, 3), -106, "a file in a directory that is not there");
  for (i = 0; i < sizeof(deep) / sizeof(deep[0]); i++) {
    snprintf(line, sizeof(line), "directory %u deep", i + 2u);
    EXPECT(ISFS_CreateDir(deep[i], 0, 3, 3, 3), 0, line);
  }
  EXPECT(ISFS_CreateDir("/tmp/1/2/3/4/5/6/7/8", 0, 3, 3, 3), -110, "a directory nine deep");
  EXPECT(ISFS_CreateFile("/tmp/1/2/3/4/5/6/7/f", 0, 3, 3, 3), 0, "a file in the eighth");
  EXPECT(ISFS_Delete("/tmp/1"), 0, "delete the lot");

  /* Sixteen descriptors, and libogc holds /dev/fs's. One file opened over
   * and over is as good as many: the limit is on descriptors. (Making a
   * file costs a console a twentieth of a second, and this program is
   * interpreted.) */
  EXPECT(ISFS_CreateFile("/tmp/many", 0, 3, 3, 3), 0, "a file to open many times");
  for (i = 0; i < 17u; i++) {
    result = ISFS_Open("/tmp/many", ISFS_OPEN_READ);
    if (result < 0) break;
    fds[opened++] = result;
  }
  snprintf(line, sizeof(line), "fifteen descriptors open beside /dev/fs's (%u)", opened);
  GC_CHECK(opened == 15, line);
  EXPECT(result, -109, "and the next is refused");
  for (i = 0; i < opened; i++) ISFS_Close(fds[i]);
  result = ISFS_Open("/tmp/many", ISFS_OPEN_READ);
  GC_CHECK(result >= 0, "once they are closed there is room again");
  ISFS_Close(result);
  EXPECT(ISFS_Delete("/tmp/many"), 0, "and the file can go");
  gc_test_end();
}

/* The console's settings: SYSCONF and setting.txt through libogc's own
 * parser. The values are the defaults of a North American console
 * (wii_sysconf.h), and the remotes are the four the machine has
 * (wii_remote.h). */
static void test_settings(void) {
  static conf_pads pads;
  u8 nickname[11];
  u32 bias = 99;
  s8 offset = 99;
  unsigned i;

  gc_test_begin("the console's settings");
  EXPECT(CONF_Init(), 0, "CONF_Init");
  EXPECT(CONF_GetAspectRatio(), CONF_ASPECT_4_3, "the picture is 4:3");
  EXPECT(CONF_GetLanguage(), CONF_LANG_ENGLISH, "the language is English");
  EXPECT(CONF_GetSoundMode(), CONF_SOUND_STEREO, "the sound is stereo");
  EXPECT(CONF_GetProgressiveScan(), 0, "interlaced");
  EXPECT(CONF_GetEuRGB60(), 1, "60 Hz");
  EXPECT(CONF_GetSensorBarPosition(), CONF_SENSORBAR_BOTTOM, "the sensor bar is below the screen");
  EXPECT(CONF_GetIRSensitivity(), 3, "the pointer's sensitivity");
  EXPECT(CONF_GetPadMotorMode(), 1, "rumble is on");
  EXPECT(CONF_GetPadSpeakerVolume(), 0x58, "the remote's speaker volume");
  EXPECT(CONF_GetScreenSaverMode(), 1, "the screen saver is on");
  EXPECT(CONF_GetEULA(), 1, "the agreement has been read");
  EXPECT(CONF_GetShutdownMode(), CONF_SHUTDOWN_STANDBY, "switched off means standing by");
  EXPECT(CONF_GetCounterBias(&bias), 0, "the clock's bias");
  GC_CHECK_EQ(bias, 0, "is zero");
  EXPECT(CONF_GetDisplayOffsetH(&offset), 0, "the picture's offset");
  GC_CHECK_EQ(offset, 0, "is zero");
  memset(nickname, 0x55, sizeof(nickname));
  EXPECT(CONF_GetNickName(nickname), 0x16, "the console's name");
  snprintf(line, sizeof(line), "is \"%s\"", (const char *)nickname);
  GC_CHECK(strcmp((const char *)nickname, "Wii") == 0, line);

  /* The sizes libogc is told, which are a console's file's. */
  EXPECT(CONF_GetLength("BT.DINF"), 0x461, "the list of remotes is 0x461 bytes");
  EXPECT(CONF_GetType("BT.DINF"), CONF_BIGARRAY, "and a big array");
  EXPECT(CONF_GetLength("IPL.NIK"), 0x16, "the name is 0x16 bytes");
  EXPECT(CONF_GetType("IPL.NIK"), CONF_SMALLARRAY, "and a small array");
  EXPECT(CONF_GetLength("IPL.PC"), 0x4a, "parental controls are 0x4a bytes");
  EXPECT(CONF_GetLength("IPL.SADR"), 0x1008, "the address is 0x1008 bytes");
  EXPECT(CONF_GetType("IPL.CB"), CONF_LONG, "the bias is a long");
  EXPECT(CONF_GetType("IPL.CD"), CONF_BOOL, "set-up done is a boolean");
  EXPECT(CONF_GetLength("NET.PROF"), CONF_ENOENT, "there is no network profile");

  /* Four registered remotes: the machine's own addresses, most
   * significant byte first, and the name a remote gives. */
  memset(&pads, 0x55, sizeof(pads));
  EXPECT(CONF_GetPadDevices(&pads), 0, "the registered remotes");
  GC_CHECK_EQ(pads.num_registered, 4, "four of them");
  for (i = 0; i < 4; i++) {
    const u8 *a = pads.registered[i].bdaddr;
    snprintf(line, sizeof(line), "remote %u is %02x:%02x:%02x:%02x:%02x:%02x \"%.20s\"", i, a[0], a[1], a[2], a[3],
             a[4], a[5], pads.registered[i].name);
    GC_CHECK(a[0] == 0x00 && a[1] == 0x1e && a[2] == 0x4e && a[3] == 0x43 && a[4] == 0x57 && a[5] == 0x10 + i &&
                 strcmp(pads.registered[i].name, "Nintendo RVL-CNT-01") == 0,
             line);
  }
  GC_CHECK(pads.registered[4].bdaddr[5] == 0 && pads.registered[4].name[0] == 0, "and no fifth");
  GC_CHECK(pads.active[0].bdaddr[5] == 0 && pads.balance_board.bdaddr[5] == 0, "none connected, no balance board");

  /* setting.txt, which is under a key: the region, area and video
   * standard. */
  EXPECT(CONF_GetRegion(), CONF_REGION_US, "the region is the United States");
  EXPECT(CONF_GetArea(), CONF_AREA_USA, "the area is USA");
  EXPECT(CONF_GetVideo(), CONF_VIDEO_NTSC, "the video standard is NTSC");
  gc_test_end();
}

int main(void) {
  VIDEO_Init();
  test_directories();
  test_contents();
  test_names();
  test_moving();
  test_refusals();
  test_settings();
  gc_test_finish();
  return 0;
}
