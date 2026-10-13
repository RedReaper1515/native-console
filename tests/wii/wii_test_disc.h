/*
 * A small Wii disc to test the reader against, and what is on it.
 *
 * Nobody's retail disc is in this repository and neither is any key that
 * would open one. The reader still has to be tested, so wii_test_disc.c
 * builds a disc that is structurally the real thing - a header, a
 * partition table with an update partition in front of the game, a
 * ticket, title metadata, a data partition of encrypted clusters with
 * their hash tables - around two keys that were made up for it. The keys
 * are below, and they are text: "NC-test-common-k" is not a key anyone's
 * console has.
 *
 * It is a C program rather than a Python script, as the GameCube one is,
 * because building it needs AES and Python's standard library has none.
 * It uses the runtime's own (wii_crypto.c), which is held to the
 * standards' published vectors by wii_crypto_test.c; the reader is then
 * held to this disc.
 *
 * The contents and the expectations are one pair, as in
 * tools/gc/make_test_disc.py: the files are filled by arithmetic rules
 * declared here, so the test checks bytes against the rule rather than
 * against a second copy.
 */
#ifndef WII_TEST_DISC_H
#define WII_TEST_DISC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WII_TEST_GAME_CODE "RWTE01"
#define WII_TEST_GAME_NAME "native console wii disc test"

/* Made up for this test. Neither opens anything but the disc built here. */
#define WII_TEST_COMMON_KEY "NC-test-common-k"
#define WII_TEST_TITLE_KEY "NC-test-title-ke"

/* The layout of the decrypted game partition. Everything a Wii disc
 * stores as "offset >> 2" is a multiple of four here, as it has to be. */
#define WII_TEST_APPLOADER_AT 0x2440u
#define WII_TEST_APPLOADER_SIZE 0x200u
#define WII_TEST_APPLOADER_ENTRY 0x81200000u
#define WII_TEST_DOL_AT 0x3000u
#define WII_TEST_DOL_TEXT_SIZE 0x400u
#define WII_TEST_DOL_SIZE (0x100u + WII_TEST_DOL_TEXT_SIZE)
#define WII_TEST_FST_AT 0x4000u

/* The files. pattern.bin starts in the first cluster and ends in the
 * third; big.bin runs over twenty clusters, more than the reader keeps
 * decrypted at once. */
#define WII_TEST_HELLO "Hello from the Wii disc.\n"
#define WII_TEST_HELLO_AT 0x6000u
#define WII_TEST_HELLO_SIZE 32u
#define WII_TEST_PATTERN_AT 0x7000u
#define WII_TEST_PATTERN_SIZE 0x9000u
#define WII_TEST_SECOND_AT 0x10000u
#define WII_TEST_SECOND_SIZE 32u
#define WII_TEST_BIG_AT 0x10400u
#define WII_TEST_BIG_SIZE (20u * 0x7c00u)

#define WII_TEST_CLUSTERS 24u
#define WII_TEST_DATA_SIZE (WII_TEST_CLUSTERS * 0x7c00u)

/* Where things are on the disc. A retail disc has its game partition at
 * 0xf800000; a quarter of a megabyte in keeps the plain image small. */
#define WII_TEST_UPDATE_PARTITION 0x50000u
#define WII_TEST_NEAR_PARTITION 0x60000u
/* Past four gigabytes, where a thirty-two-bit offset has wrapped round.
 * Only the containers that leave empty space out can hold a disc with its
 * partition here in a file of a few megabytes. */
#define WII_TEST_FAR_PARTITION 0x108000000ull
#define WII_TEST_PARTITION_DATA 0x20000u /* from the partition's start to its clusters */
#define WII_TEST_TMD_SIZE 0x208u
#define WII_TEST_CERT_SIZE 0x300u

/* The rules the files are filled by. */
static inline uint8_t wii_test_pattern_byte(uint32_t i) { return (uint8_t)((i * 7u + 13u) & 0xffu); }
static inline uint8_t wii_test_big_byte(uint32_t i) {
  return (uint8_t)((i * 31u + (i >> 8) * 17u + (i >> 16) * 3u + 5u) & 0xffu);
}
static inline uint8_t wii_test_apploader_byte(uint32_t i) { return (uint8_t)((i * 3u + 1u) & 0xffu); }
static inline uint8_t wii_test_dol_byte(uint32_t i) { return (uint8_t)((i * 5u + 2u) & 0xffu); }
static inline uint8_t wii_test_cert_byte(uint32_t i) { return (uint8_t)((i * 11u + 7u) & 0xffu); }

typedef enum WiiTestContainer {
  WII_TEST_ISO,  /* the disc, byte for byte, ending after its last cluster */
  WII_TEST_CISO, /* blocks of 1 << shift, the empty ones left out */
  WII_TEST_WBFS  /* sectors of 1 << shift, likewise, in a shuffled order */
} WiiTestContainer;

typedef struct WiiTestDisc {
  WiiTestContainer container;
  unsigned shift;            /* CISO block or WBFS sector size; unused for an ISO */
  uint64_t partition_offset; /* of the game partition */
  uint8_t common_key_index;  /* what the ticket claims; 0 on almost every disc */
  int no_hashes;             /* the disc header's byte 0x60 */
  int no_encryption;         /* ... and 0x61 */
  /* A disc that boots, when these are given: a real apploader (its code,
   * up to WII_TEST_APPLOADER_ROOM bytes, and its entry point) in place of
   * the pattern, and a real DOL, which goes after everything else at
   * WII_TEST_BOOT_DOL_AT. Everything the files hold stays where it was. */
  const uint8_t *apploader;
  uint32_t apploader_size;
  uint32_t apploader_entry;
  const uint8_t *dol;
  uint32_t dol_size;
} WiiTestDisc;

/* Where a bootable disc's DOL goes, how much apploader there is room for in
 * front of the pattern DOL, and the most clusters one disc can have here
 * (one group of the hash tree). */
#define WII_TEST_BOOT_DOL_AT WII_TEST_DATA_SIZE
#define WII_TEST_APPLOADER_ROOM (WII_TEST_DOL_AT - WII_TEST_APPLOADER_AT - 0x20u)
#define WII_TEST_MAX_CLUSTERS 64u

/* Writes the disc to `path`. Returns 0, or -1 with the reason on stderr. */
int wii_test_disc_write(const char *path, const WiiTestDisc *options);

/* The game partition's decrypted contents, WII_TEST_DATA_SIZE bytes: what
 * every read of any of the discs above should agree with. */
const uint8_t *wii_test_disc_plain(void);

#ifdef __cplusplus
}
#endif

#endif /* WII_TEST_DISC_H */
