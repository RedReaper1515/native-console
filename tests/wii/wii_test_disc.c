/*
 * Builds the test disc wii_test_disc.h describes.
 *
 * The work goes inside out, the way the format nests: the game
 * partition's contents in the clear, then the same cut into clusters with
 * their hash tables and encrypted, then the partition around those - a
 * ticket, title metadata, the top-level hash table - then the disc around
 * the partition, and last the container around the disc.
 *
 * The disc is held as a handful of extents rather than as one buffer,
 * because one of the discs built here has its game partition past four
 * gigabytes and is otherwise empty. A container that leaves empty space
 * out is written by asking, block by block, whether anything is there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wii_crypto.h"
#include "wii_test_disc.h"

#define CLUSTER_SIZE 0x8000u
#define CLUSTER_HASHES 0x400u
#define CLUSTER_DATA 0x7c00u
#define H0_COUNT 31u
#define H1_AT 0x280u
#define H2_AT 0x340u
#define IV_AT 0x3d0u
#define H3_AT 0x8000u
#define H3_SIZE 0x18000u
#define TMD_AT 0x2c0u
#define CERT_AT 0x500u
#define WII_MAGIC 0x5d1c9ea3u

static void put32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static void put16(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

/* ------------------------------------------------------------------ */
/* The game partition's contents                                       */

/* The first bytes of a disc, which a Wii disc has twice: at the front of
 * the disc and at the front of the game partition. */
static void put_disc_header(uint8_t *at) {
  memcpy(at, WII_TEST_GAME_CODE, 6);
  put32(at + 0x18, WII_MAGIC);
  memcpy(at + 0x20, WII_TEST_GAME_NAME, sizeof(WII_TEST_GAME_NAME));
}

/* One filesystem entry: a flag, three bytes of name offset, and two
 * words - a file's offset (a quarter of it, on a Wii disc) and size, or a
 * directory's parent and the index one past its last descendant. */
static void put_entry(uint8_t *at, int is_directory, uint32_t name, uint32_t a, uint32_t b) {
  at[0] = is_directory ? 1 : 0;
  at[1] = (uint8_t)(name >> 16);
  at[2] = (uint8_t)(name >> 8);
  at[3] = (uint8_t)name;
  put32(at + 4, a);
  put32(at + 8, b);
}

static uint32_t put_filesystem(uint8_t *at) {
  static const char names[] = "data\0pattern.bin\0second.bin\0big.bin\0hello.txt";
  const uint32_t count = 6;
  uint32_t size;

  /* In filesystem order: a directory's entries follow it. */
  put_entry(at + 0 * 12, 1, 0, 0, count);
  put_entry(at + 1 * 12, 1, 0, 0, 4);
  put_entry(at + 2 * 12, 0, 5, WII_TEST_PATTERN_AT >> 2, WII_TEST_PATTERN_SIZE);
  put_entry(at + 3 * 12, 0, 17, WII_TEST_SECOND_AT >> 2, WII_TEST_SECOND_SIZE);
  put_entry(at + 4 * 12, 0, 28, WII_TEST_BIG_AT >> 2, WII_TEST_BIG_SIZE);
  put_entry(at + 5 * 12, 0, 36, WII_TEST_HELLO_AT >> 2, WII_TEST_HELLO_SIZE);
  memcpy(at + count * 12, names, sizeof(names));
  /* The size is stored as a quarter of itself too, so it is a multiple
   * of four. */
  size = (count * 12u + (uint32_t)sizeof(names) + 3u) & ~3u;
  return size;
}

const uint8_t *wii_test_disc_plain(void) {
  static uint8_t *plain;
  uint32_t i, fst_size;

  if (plain) return plain;
  plain = (uint8_t *)calloc(1, WII_TEST_DATA_SIZE);
  if (!plain) return NULL;

  put_disc_header(plain);
  fst_size = put_filesystem(plain + WII_TEST_FST_AT);
  put32(plain + 0x420, WII_TEST_DOL_AT >> 2);
  put32(plain + 0x424, WII_TEST_FST_AT >> 2);
  put32(plain + 0x428, fst_size >> 2);
  put32(plain + 0x42c, fst_size >> 2);

  /* An apploader's header: a date, an entry point, the code's size and
   * its trailer's. The code itself is a pattern - nothing here runs it. */
  memcpy(plain + WII_TEST_APPLOADER_AT, "2026/10/06", 10);
  put32(plain + WII_TEST_APPLOADER_AT + 0x10, WII_TEST_APPLOADER_ENTRY);
  put32(plain + WII_TEST_APPLOADER_AT + 0x14, WII_TEST_APPLOADER_SIZE);
  put32(plain + WII_TEST_APPLOADER_AT + 0x18, 0);
  for (i = 0; i < WII_TEST_APPLOADER_SIZE; i++) {
    plain[WII_TEST_APPLOADER_AT + 0x20u + i] = wii_test_apploader_byte(i);
  }

  /* A DOL of one text section: its place in the file, its address, its
   * size, and the entry point. */
  put32(plain + WII_TEST_DOL_AT + 0x00, 0x100);
  put32(plain + WII_TEST_DOL_AT + 0x48, 0x80004000u);
  put32(plain + WII_TEST_DOL_AT + 0x90, WII_TEST_DOL_TEXT_SIZE);
  put32(plain + WII_TEST_DOL_AT + 0xe0, 0x80004000u);
  for (i = 0; i < WII_TEST_DOL_TEXT_SIZE; i++) {
    plain[WII_TEST_DOL_AT + 0x100u + i] = wii_test_dol_byte(i);
  }

  memcpy(plain + WII_TEST_HELLO_AT, WII_TEST_HELLO, sizeof(WII_TEST_HELLO) - 1u);
  for (i = 0; i < WII_TEST_PATTERN_SIZE; i++) plain[WII_TEST_PATTERN_AT + i] = wii_test_pattern_byte(i);
  memset(plain + WII_TEST_SECOND_AT, 0xa5, WII_TEST_SECOND_SIZE);
  for (i = 0; i < WII_TEST_BIG_SIZE; i++) plain[WII_TEST_BIG_AT + i] = wii_test_big_byte(i);
  return plain;
}

/* ------------------------------------------------------------------ */
/* Clusters                                                            */

/* The data as the disc stores it: each 0x7c00 bytes behind 0x400 bytes of
 * hashes, and the two encrypted separately.
 *
 * The hashes are a tree. H0 is thirty-one hashes of the cluster's own
 * contents, 0x400 bytes each. H1 is the hashes of the H0 tables of the
 * eight clusters in this cluster's subgroup, and every cluster of the
 * subgroup carries a copy. H2 is the same one level up: the hashes of the
 * H1 tables of the eight subgroups in a group of sixty-four clusters. One
 * hash of each group's H2 table goes into H3, which lives outside the
 * clusters, unencrypted, in the partition's header area.
 *
 * A retail partition is padded to whole groups. This one is twenty-four
 * clusters, and the entries for clusters that are not there are zeroes.
 * `h3` receives the top-level table. */
/* The partition's contents for a disc: the usual ones, or - for a disc
 * that boots - a copy of them with the apploader and the DOL it was given
 * (wii_test_disc.h). `clusters` receives how many clusters that is, and
 * `owned` whether the caller frees it. */
static const uint8_t *plain_for(const WiiTestDisc *options, uint32_t *clusters, int *owned) {
  const uint8_t *usual = wii_test_disc_plain();
  uint8_t *plain;
  uint32_t size;
  *clusters = WII_TEST_CLUSTERS;
  *owned = 0;
  if (!usual || (!options->apploader && !options->dol)) return usual;

  size = WII_TEST_DATA_SIZE + ((options->dol_size + 0x7bffu) / 0x7c00u) * 0x7c00u;
  if (size / 0x7c00u > WII_TEST_MAX_CLUSTERS || options->apploader_size > WII_TEST_APPLOADER_ROOM) {
    fprintf(stderr, "wii_test_disc: the apploader or the DOL is too big for a test disc\n");
    return NULL;
  }
  plain = (uint8_t *)calloc(1, size);
  if (!plain) return NULL;
  memcpy(plain, usual, WII_TEST_DATA_SIZE);
  if (options->apploader) {
    put32(plain + WII_TEST_APPLOADER_AT + 0x10, options->apploader_entry);
    put32(plain + WII_TEST_APPLOADER_AT + 0x14, options->apploader_size);
    memset(plain + WII_TEST_APPLOADER_AT + 0x20u, 0, WII_TEST_APPLOADER_ROOM);
    memcpy(plain + WII_TEST_APPLOADER_AT + 0x20u, options->apploader, options->apploader_size);
  }
  if (options->dol) {
    memcpy(plain + WII_TEST_BOOT_DOL_AT, options->dol, options->dol_size);
    put32(plain + 0x420, WII_TEST_BOOT_DOL_AT >> 2);
  }
  *clusters = size / 0x7c00u;
  *owned = 1;
  return plain;
}

static uint8_t *build_clusters(const WiiTestDisc *options, const WiiAes *aes, uint8_t *h3,
                               const uint8_t *plain, uint32_t clusters) {
  static const uint8_t zero_iv[16] = {0};
  uint8_t *stored = (uint8_t *)calloc(clusters, CLUSTER_SIZE);
  uint8_t h1[8][8 * WII_SHA1_SIZE]; /* a table per subgroup */
  uint8_t h2[8 * WII_SHA1_SIZE];
  uint32_t c, i;

  if (!plain || !stored) {
    free(stored);
    return NULL;
  }
  memset(h1, 0, sizeof(h1));
  memset(h2, 0, sizeof(h2));

  for (c = 0; c < clusters; c++) {
    uint8_t *cluster = stored + c * CLUSTER_SIZE;
    memcpy(cluster + CLUSTER_HASHES, plain + c * CLUSTER_DATA, CLUSTER_DATA);
    for (i = 0; i < H0_COUNT; i++) {
      wii_sha1(cluster + CLUSTER_HASHES + i * 0x400u, 0x400u, cluster + i * WII_SHA1_SIZE);
    }
    wii_sha1(cluster, H0_COUNT * WII_SHA1_SIZE, h1[c / 8u] + (c % 8u) * WII_SHA1_SIZE);
  }
  for (i = 0; i < (clusters + 7u) / 8u; i++) {
    wii_sha1(h1[i], sizeof(h1[i]), h2 + i * WII_SHA1_SIZE);
  }
  memset(h3, 0, H3_SIZE);
  wii_sha1(h2, sizeof(h2), h3);

  for (c = 0; c < clusters; c++) {
    uint8_t *cluster = stored + c * CLUSTER_SIZE;
    memcpy(cluster + H1_AT, h1[c / 8u], sizeof(h1[0]));
    memcpy(cluster + H2_AT, h2, sizeof(h2));
    /* A disc that turns hash checking off is allowed hashes that are
     * wrong. Make them wrong, so a reader that trusted them would fail. */
    if (options->no_hashes) memset(cluster, 0x5a, H0_COUNT * WII_SHA1_SIZE);
    if (options->no_encryption) continue;
    /* The hashes first, under a vector of zeroes; then the contents,
     * under sixteen bytes of the hashes as they now are, encrypted. */
    wii_aes_cbc_encrypt(aes, zero_iv, cluster, cluster, CLUSTER_HASHES);
    wii_aes_cbc_encrypt(aes, cluster + IV_AT, cluster + CLUSTER_HASHES, cluster + CLUSTER_HASHES,
                        CLUSTER_DATA);
  }
  return stored;
}

/* ------------------------------------------------------------------ */
/* The partition's header area                                         */

/* Ticket, the words that say where everything is, title metadata, a
 * certificate chain, and the H3 table: the 0x20000 bytes in front of the
 * clusters. Nothing here is signed - the signature fields are zeroes -
 * because nothing that reads a disc checks signatures, and a real one
 * could only be made with a key nobody outside Nintendo has. */
static uint8_t *build_partition_header(const WiiTestDisc *options, const uint8_t *h3,
                                       uint64_t stored_size) {
  uint8_t *header = (uint8_t *)calloc(1, WII_TEST_PARTITION_DATA);
  uint8_t *tmd;
  uint8_t iv[16];
  WiiAes common;
  uint32_t i;

  if (!header) return NULL;

  /* The ticket. What the reader needs of it: the title key, encrypted
   * under the common key with the title's id as the vector; the id; and
   * which common key that was. */
  put32(header, 0x00010001u); /* an RSA-2048 signature would follow */
  memcpy(header + 0x140, "Root-TEST-XS-native-console", 27);
  memcpy(header + 0x1dc, "\0\1\0\0RWTE", 8);
  header[0x1f1] = options->common_key_index;
  memset(iv, 0, sizeof(iv));
  memcpy(iv, header + 0x1dc, 8);
  wii_aes_set_key(&common, (const uint8_t *)WII_TEST_COMMON_KEY);
  wii_aes_cbc_encrypt(&common, iv, (const uint8_t *)WII_TEST_TITLE_KEY, header + 0x1bf, 16);

  put32(header + 0x2a4, WII_TEST_TMD_SIZE);
  put32(header + 0x2a8, TMD_AT >> 2);
  put32(header + 0x2ac, WII_TEST_CERT_SIZE);
  put32(header + 0x2b0, CERT_AT >> 2);
  put32(header + 0x2b4, H3_AT >> 2);
  put32(header + 0x2b8, WII_TEST_PARTITION_DATA >> 2);
  put32(header + 0x2bc, (uint32_t)(stored_size >> 2));

  /* Title metadata: a header of 0x1e4 bytes and one content record, the
   * partition's data, whose hash is the hash of the H3 table. That is
   * where the tree of hashes above ends. */
  tmd = header + TMD_AT;
  put32(tmd, 0x00010001u);
  memcpy(tmd + 0x140, "Root-TEST-CP-native-console", 27);
  memcpy(tmd + 0x18c, "\0\1\0\0RWTE", 8);
  put16(tmd + 0x1de, 1); /* one content */
  put32(tmd + 0x1e4 + 0x00, 0);
  put16(tmd + 0x1e4 + 0x04, 0);
  put16(tmd + 0x1e4 + 0x06, 3);
  put32(tmd + 0x1e4 + 0x08, (uint32_t)(stored_size >> 32));
  put32(tmd + 0x1e4 + 0x0c, (uint32_t)stored_size);
  wii_sha1(h3, H3_SIZE, tmd + 0x1e4 + 0x10);

  for (i = 0; i < WII_TEST_CERT_SIZE; i++) header[CERT_AT + i] = wii_test_cert_byte(i);
  memcpy(header + H3_AT, h3, H3_SIZE);
  return header;
}

/* ------------------------------------------------------------------ */
/* The disc, as extents                                                */

typedef struct Extent {
  uint64_t at;
  const uint8_t *bytes;
  uint64_t size;
} Extent;

typedef struct Image {
  Extent extents[4];
  unsigned count;
  uint64_t end;
} Image;

static void image_add(Image *image, uint64_t at, const uint8_t *bytes, uint64_t size) {
  Extent *extent = &image->extents[image->count++];
  extent->at = at;
  extent->bytes = bytes;
  extent->size = size;
  if (at + size > image->end) image->end = at + size;
}

/* A range of the disc: zeroes, with whatever extents cross it laid over.
 * Returns whether there is anything in it but zeroes - which is what a
 * container asks before deciding to store a block, so the stretches of
 * nothing inside an extent are left out along with the gaps between. */
static int image_read(const Image *image, uint64_t at, uint8_t *out, uint64_t size) {
  int crossed = 0;
  uint64_t byte;
  unsigned i;

  memset(out, 0, (size_t)size);
  for (i = 0; i < image->count; i++) {
    const Extent *extent = &image->extents[i];
    const uint64_t from = at > extent->at ? at : extent->at;
    const uint64_t to = at + size < extent->at + extent->size ? at + size : extent->at + extent->size;
    if (from >= to) continue;
    memcpy(out + (from - at), extent->bytes + (from - extent->at), (size_t)(to - from));
    crossed = 1;
  }
  if (!crossed) return 0;
  for (byte = 0; byte < size; byte++) {
    if (out[byte]) return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Containers                                                          */

/* The disc as it is, up to the end of its last cluster. A real image
 * runs on to 4.7 GB; a reader has no business needing the rest. */
static int write_iso(FILE *file, const Image *image) {
  uint8_t *bytes;
  int ok;

  if (image->end > 0x4000000u) {
    fprintf(stderr, "wii_test_disc: a plain image of %llu bytes is more than a test should write\n",
            (unsigned long long)image->end);
    return -1;
  }
  bytes = (uint8_t *)malloc((size_t)image->end);
  if (!bytes) return -1;
  image_read(image, 0, bytes, image->end);
  ok = fwrite(bytes, 1, (size_t)image->end, file) == image->end;
  free(bytes);
  return ok ? 0 : -1;
}

/* CISO: a 0x8000-byte header - the magic, the block size little-endian,
 * a byte per block saying whether it is stored - and then the stored
 * blocks, back to back. */
static int write_ciso(FILE *file, const Image *image, unsigned shift) {
  const uint64_t block_size = 1ull << shift;
  const uint64_t blocks = (image->end + block_size - 1u) >> shift;
  uint8_t *head = (uint8_t *)calloc(1, 0x8000u);
  uint8_t *block = (uint8_t *)malloc((size_t)block_size);
  uint64_t i;
  int ok = head && block;

  if (ok && blocks > 0x8000u - 8u) {
    fprintf(stderr, "wii_test_disc: blocks of 2^%u bytes are too small for this disc\n", shift);
    ok = 0;
  }
  if (ok) {
    memcpy(head, "CISO", 4);
    head[4] = (uint8_t)block_size;
    head[5] = (uint8_t)(block_size >> 8);
    head[6] = (uint8_t)(block_size >> 16);
    head[7] = (uint8_t)(block_size >> 24);
    for (i = 0; i < blocks; i++) head[8u + i] = (uint8_t)image_read(image, i << shift, block, block_size);
    ok = fwrite(head, 1, 0x8000u, file) == 0x8000u;
  }
  for (i = 0; ok && i < blocks; i++) {
    if (image_read(image, i << shift, block, block_size)) {
      ok = fwrite(block, 1, (size_t)block_size, file) == block_size;
    }
  }
  free(head);
  free(block);
  return ok ? 0 : -1;
}

/* WBFS: the first sector of the file holds the header - the magic, the
 * drive's size in 512-byte sectors, the two sector sizes as shifts, and a
 * byte per disc slot - and, from the second 512 bytes, the one disc's
 * information: a copy of its first 0x100 bytes, then a halfword for each
 * sector of a double-layer disc saying which sector of the file holds it.
 *
 * The sectors are stored in the reverse of disc order. Any order is
 * legal, and the one a reader would get right by accident is the one
 * worth avoiding. */
static int write_wbfs(FILE *file, const Image *image, unsigned shift) {
  const uint64_t sector_size = 1ull << shift;
  const uint32_t sectors = (143432u * 2u) >> (shift - 15u);
  uint8_t *head = (uint8_t *)calloc(1, (size_t)sector_size);
  uint8_t *sector = (uint8_t *)malloc((size_t)sector_size);
  uint32_t *used = (uint32_t *)malloc(sizeof(uint32_t) * sectors);
  uint32_t count = 0, i;
  int ok = head && sector && used;

  if (ok && (0x300u + (uint64_t)sectors * 2u > sector_size || image->end > (uint64_t)sectors << shift)) {
    fprintf(stderr, "wii_test_disc: sectors of 2^%u bytes do not suit this disc\n", shift);
    ok = 0;
  }
  if (ok) {
    for (i = 0; i < sectors; i++) {
      if (image_read(image, (uint64_t)i << shift, sector, sector_size)) used[count++] = i;
    }
    memcpy(head, "WBFS", 4);
    put32(head + 4, (uint32_t)(((uint64_t)(count + 1u) << shift) >> 9));
    head[8] = 9;
    head[9] = (uint8_t)shift;
    head[12] = 1; /* the first slot holds a disc */
    image_read(image, 0, head + 0x200, 0x100);
    for (i = 0; i < count; i++) put16(head + 0x300 + used[i] * 2u, count - i);
    ok = fwrite(head, 1, (size_t)sector_size, file) == sector_size;
  }
  /* The file's sector f, counting from one, holds the disc's
   * used[count - f]. */
  for (i = count; ok && i > 0; i--) {
    image_read(image, (uint64_t)used[i - 1u] << shift, sector, sector_size);
    ok = fwrite(sector, 1, (size_t)sector_size, file) == sector_size;
  }
  free(head);
  free(sector);
  free(used);
  return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */

int wii_test_disc_write(const char *path, const WiiTestDisc *options) {
  /* With neither hashes nor encryption the data is not in clusters at
   * all: the partition's contents are simply there. */
  const int bare = options->no_hashes && options->no_encryption;
  uint32_t clusters = 0;
  int plain_owned = 0;
  const uint8_t *plain = plain_for(options, &clusters, &plain_owned);
  const uint64_t stored_size = bare ? (uint64_t)clusters * 0x7c00u : (uint64_t)clusters * CLUSTER_SIZE;
  uint8_t *front = (uint8_t *)calloc(1, WII_TEST_UPDATE_PARTITION);
  uint8_t *update = (uint8_t *)malloc(0x2c0u);
  uint8_t *h3 = (uint8_t *)malloc(H3_SIZE);
  uint8_t *stored = NULL, *header = NULL;
  Image image;
  WiiAes title;
  FILE *file = NULL;
  int result = -1;
  uint32_t i;

  memset(&image, 0, sizeof(image));
  wii_aes_set_key(&title, (const uint8_t *)WII_TEST_TITLE_KEY);
  if (front && update && h3 && plain) stored = build_clusters(options, &title, h3, plain, clusters);
  if (stored) header = build_partition_header(options, h3, stored_size);
  if (!header) goto done;

  /* The front of the disc: the header, with the two bytes that can turn
   * hashing and encryption off; the partition table, four groups of
   * which the first has two entries; and the region block with its
   * magic word at the very end. */
  put_disc_header(front);
  front[0x60] = options->no_hashes ? 1 : 0;
  front[0x61] = options->no_encryption ? 1 : 0;
  put32(front + 0x40000, 2);
  put32(front + 0x40004, 0x40020u >> 2);
  /* The system update comes first on a retail disc, and a reader that
   * took the first partition rather than the first game would get it. */
  put32(front + 0x40020, WII_TEST_UPDATE_PARTITION >> 2);
  put32(front + 0x40024, 1);
  put32(front + 0x40028, (uint32_t)(options->partition_offset >> 2));
  put32(front + 0x4002c, 0);
  put32(front + 0x4e000, 1);
  put32(front + 0x4fffc, 0xc3f81a8eu);

  /* An update partition's header, filled with something that is plainly
   * not a ticket: this is only ever here to be skipped. */
  for (i = 0; i < 0x2c0u; i++) update[i] = (uint8_t)(0xe0u | (i & 0x0fu));

  image_add(&image, 0, front, WII_TEST_UPDATE_PARTITION);
  image_add(&image, WII_TEST_UPDATE_PARTITION, update, 0x2c0u);
  image_add(&image, options->partition_offset, header, WII_TEST_PARTITION_DATA);
  image_add(&image, options->partition_offset + WII_TEST_PARTITION_DATA,
            bare ? plain : stored, stored_size);

  file = fopen(path, "wb");
  if (!file) {
    fprintf(stderr, "wii_test_disc: cannot write %s\n", path);
    goto done;
  }
  switch (options->container) {
    case WII_TEST_ISO: result = write_iso(file, &image); break;
    case WII_TEST_CISO: result = write_ciso(file, &image, options->shift); break;
    case WII_TEST_WBFS: result = write_wbfs(file, &image, options->shift); break;
  }
  if (fclose(file) != 0) result = -1;
  if (result != 0) fprintf(stderr, "wii_test_disc: %s was not written\n", path);

done:
  free(front);
  free(update);
  free(h3);
  free(stored);
  free(header);
  if (plain_owned) free((void *)plain);
  return result;
}
