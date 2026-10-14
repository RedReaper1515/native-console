/*
 * Reading a Wii disc image.
 *
 * A Wii disc is a GameCube disc inside a wrapper. The wrapper is plain:
 * a header with the same game id and a magic word of its own, and at
 * 0x40000 a table of partitions. One of them, of type 0, is the game. It
 * starts with a ticket - which holds the partition's own key, encrypted -
 * and a few words saying where its metadata and its data are, and its
 * data is the part that looks like a GameCube disc: header, apploader at
 * 0x2440, DOL, filesystem.
 *
 * That data is stored in clusters of 0x8000 bytes. The first 0x400 of a
 * cluster are SHA-1 hashes, of the cluster's own contents and of its
 * neighbours' hash tables; the other 0x7c00 are the contents. Both parts
 * are encrypted with AES-128 in CBC mode under the partition's title key:
 * the hashes with an initialisation vector of zeroes, the contents with
 * sixteen bytes taken from the still-encrypted hashes, at 0x3d0. So byte
 * D of the game's data is in cluster D / 0x7c00 at D % 0x7c00, and
 * reading it means decrypting that cluster - which is why the last few
 * decrypted clusters are kept, a game reading a file in small pieces
 * would otherwise decrypt the same 31 KB over and over.
 *
 * The layout is described on WiiBrew ("Wii disc", "Ticket", "Title
 * metadata"); this file was written from that description. Everything is
 * big-endian and assembled a byte at a time, as in gc_disc.c.
 *
 * Also here is WBFS, the container most Wii images are kept in: the disc
 * cut into large sectors, with a table saying where in the file each one
 * is, or that it is absent and reads as zeroes. It is CISO's idea with a
 * different header, and sits under the partition logic in the same way.
 *
 * No key is in this file. See gc_disc.h for where one comes from.
 */

#include <stdlib.h>
#include <string.h>

#include "gc_disc.h"
#include "wii_crypto.h"
#include "wii_disc.h"

#define CLUSTER_SIZE 0x8000u
#define CLUSTER_HASHES 0x400u
#define CLUSTER_DATA 0x7c00u
/* The first table in a cluster's hashes: one SHA-1 for each 0x400 bytes
 * of its contents. */
#define CLUSTER_H0_COUNT 31u
#define CLUSTER_IV_AT 0x3d0u

/* How many decrypted clusters are kept: half a megabyte, which covers a
 * game reading a few files in turn. */
#define CACHE_ENTRIES 16u

#define PARTITION_TABLE_AT 0x40000u
#define PARTITION_HEADER_SIZE 0x2c0u
#define PARTITION_TYPE_GAME 0u
/* Sanity bounds, not format limits: a retail disc has two or three
 * partitions and a metadata block of a few hundred bytes. */
#define PARTITION_MAX_COUNT 64u
#define BLOB_MAX_SIZE 0x100000u

#define TICKET_TITLE_KEY 0x1bfu
#define TICKET_TITLE_ID 0x1dcu
#define TICKET_KEY_INDEX 0x1f1u

/* A double-layer disc in 0x8000-byte sectors, which is what a WBFS's
 * table is always sized for, whatever the disc inside it. */
#define WBFS_DISC_SECTORS (143432u * 2u)
#define WBFS_DISC_HEADER_COPY 0x100u

typedef struct WiiCluster {
  uint64_t index;
  uint64_t used; /* when it was last asked for; 0 for a slot holding nothing */
  uint8_t data[CLUSTER_DATA];
} WiiCluster;

struct WiiDisc {
  FILE *log;       /* the one gc_disc_open was given */
  int told_locked; /* the locked-disc line is said once, not once a read */

  /* The WBFS container: the sector size as a shift, and for each sector
   * of the disc the sector of the file that holds it, or 0 for none. */
  int is_wbfs;
  unsigned wbfs_shift;
  uint32_t wbfs_sectors;
  uint16_t *wbfs_table;

  /* The game partition. */
  int has_partition;
  int has_hashes;    /* the disc header's byte 0x60 is clear */
  int is_encrypted;  /* ... and so is 0x61 */
  uint64_t partition_offset;
  uint64_t data_offset; /* on the disc, not within the partition */
  uint64_t data_size;   /* decrypted */
  uint8_t ticket[WII_TICKET_SIZE];
  uint8_t *tmd;
  uint32_t tmd_size;
  uint8_t *certs;
  uint32_t cert_size;

  int unlocked;
  int has_key;
  uint8_t title_key[16];
  WiiAes aes;

  uint64_t clock;
  WiiCluster *cache;
  uint8_t raw[CLUSTER_SIZE]; /* a cluster as stored, on its way in */
};

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

/* The words a Wii disc stores as "offset >> 2". */
static uint64_t be32_shifted(const uint8_t *p) { return (uint64_t)be32(p) << 2; }

static struct WiiDisc *state_of(GcDisc *disc) {
  if (!disc->wii) disc->wii = (struct WiiDisc *)calloc(1, sizeof(struct WiiDisc));
  return disc->wii;
}

/* ------------------------------------------------------------------ */
/* WBFS                                                                */

/* A WBFS was designed as a filesystem for a whole drive, holding many
 * discs; a .wbfs file is one of those with a single disc in it. Its
 * header gives two sector sizes as powers of two - the drive's, and the
 * much larger one the discs are cut into - and one byte per disc slot.
 * Each slot's information block, starting at the second drive sector, is
 * a copy of the disc's first 0x100 bytes followed by the table: one
 * big-endian halfword per WBFS sector of a double-layer disc, naming the
 * sector of the file that holds it. */
int wii_wbfs_open(GcDisc *disc, const char *path, FILE *log) {
  const uint64_t file_size = disc->size;
  uint8_t head[12];
  uint8_t *slots, *table;
  uint32_t hd_sector, slot_count, slot, info_size, sectors, i, last = 0;
  unsigned hd_shift, wbfs_shift;
  int any = 0, beyond = 0;
  struct WiiDisc *w;

  if (file_size < sizeof(head)) return 0;
  if (gc_disc_file_read(disc, 0, head, sizeof(head)) != 0) return 0;
  if (!(head[0] == 'W' && head[1] == 'B' && head[2] == 'F' && head[3] == 'S')) return 0;

  hd_shift = head[8];
  wbfs_shift = head[9];
  /* The table has a halfword per sector, so sectors smaller than a disc
   * sector or larger than a disc make no sense; drive sectors are 512
   * bytes in practice and 4096 on a few drives. */
  if (hd_shift < 9u || hd_shift > 16u || wbfs_shift < 15u || wbfs_shift > 30u ||
      hd_shift >= wbfs_shift) {
    if (log) {
      fprintf(log, "gc_disc: %s is a WBFS with sector sizes of 2^%u and 2^%u, which make no sense\n",
              path, hd_shift, wbfs_shift);
    }
    return -1;
  }
  hd_sector = 1u << hd_shift;
  sectors = WBFS_DISC_SECTORS >> (wbfs_shift - 15u);
  info_size = (WBFS_DISC_HEADER_COPY + sectors * 2u + hd_sector - 1u) & ~(hd_sector - 1u);

  /* Which slot holds a disc. A .wbfs file has one, in the first slot, but
   * the header says so rather than this assuming it. */
  slot_count = hd_sector - (uint32_t)sizeof(head);
  slots = (uint8_t *)malloc(slot_count);
  if (!slots) return -1;
  if (gc_disc_file_read(disc, sizeof(head), slots, slot_count) != 0) {
    free(slots);
    if (log) fprintf(log, "gc_disc: %s is a WBFS cut off inside its header\n", path);
    return -1;
  }
  for (slot = 0; slot < slot_count && !slots[slot]; slot++) {
  }
  free(slots);
  if (slot == slot_count) {
    if (log) fprintf(log, "gc_disc: %s is a WBFS with no disc in it\n", path);
    return -1;
  }

  w = state_of(disc);
  if (!w) return -1;
  table = (uint8_t *)malloc(sectors * 2u);
  w->wbfs_table = (uint16_t *)malloc(sectors * sizeof(uint16_t));
  if (!table || !w->wbfs_table ||
      gc_disc_file_read(disc, (uint64_t)hd_sector + (uint64_t)slot * info_size + WBFS_DISC_HEADER_COPY,
                        table, sectors * 2u) != 0) {
    free(table);
    if (log) fprintf(log, "gc_disc: %s is a WBFS cut off inside its sector table\n", path);
    return -1;
  }
  for (i = 0; i < sectors; i++) {
    const uint32_t at = be16(table + i * 2u);
    w->wbfs_table[i] = (uint16_t)at;
    if (!at) continue;
    any = 1;
    last = i;
    if (((uint64_t)at << wbfs_shift) >= file_size) beyond = 1;
  }
  free(table);

  if (!any) {
    if (log) fprintf(log, "gc_disc: %s is a WBFS whose disc has no sectors\n", path);
    return -1;
  }
  /* A WBFS too large for one file on a FAT32 drive is split: the table
   * goes on counting through a second file, .wbf1, and a third. That
   * shows here as sectors placed past the end of this file. */
  if (beyond) {
    if (log) {
      fprintf(log,
              "gc_disc: %s keeps part of its disc outside this file: it is a split WBFS "
              "(.wbfs + .wbf1 ...), which is not supported, or it is cut short. Join the "
              "parts into one .wbfs, or convert to ISO\n",
              path);
    }
    return -1;
  }

  w->is_wbfs = 1;
  w->wbfs_shift = wbfs_shift;
  w->wbfs_sectors = sectors;
  /* As for a CISO: the disc runs to the end of the last sector the file
   * keeps, and what lies between the ones it keeps is zeroes. */
  disc->size = (uint64_t)(last + 1u) << wbfs_shift;
  if (log) {
    fprintf(log, "gc_disc: a WBFS in sectors of %u bytes, standing for %llu\n", 1u << wbfs_shift,
            (unsigned long long)disc->size);
  }
  return 1;
}

int wii_disc_is_wbfs(const GcDisc *disc) { return disc && disc->wii && disc->wii->is_wbfs; }

int wii_wbfs_read(GcDisc *disc, uint64_t offset, void *destination, uint32_t length) {
  const struct WiiDisc *w = disc->wii;
  const uint32_t sector_size = 1u << w->wbfs_shift;

  while (length) {
    const uint64_t sector = offset >> w->wbfs_shift;
    const uint32_t within = (uint32_t)(offset & (sector_size - 1u));
    uint32_t piece = sector_size - within;
    if (piece > length) piece = length;
    if (sector >= w->wbfs_sectors || !w->wbfs_table[sector]) {
      memset(destination, 0, piece);
    } else {
      const uint64_t at = ((uint64_t)w->wbfs_table[sector] << w->wbfs_shift) + within;
      if (gc_disc_file_read(disc, at, destination, piece) != 0) return -1;
    }
    destination = (uint8_t *)destination + piece;
    offset += piece;
    length -= piece;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Clusters                                                            */

static const uint8_t zero_iv[16] = {0};

/* One cluster off the disc and decrypted: its contents into `data`, and
 * its hashes into `hashes` when the caller wants them. A disc whose
 * header says it is not encrypted keeps the same clusters in the clear. */
static int load_cluster(GcDisc *disc, const WiiAes *aes, uint64_t index, uint8_t *data,
                        uint8_t *hashes) {
  struct WiiDisc *w = disc->wii;
  if (gc_disc_read_raw(disc, w->data_offset + index * CLUSTER_SIZE, w->raw, CLUSTER_SIZE) != 0) {
    return -1;
  }
  if (!w->is_encrypted) {
    memcpy(data, w->raw + CLUSTER_HASHES, CLUSTER_DATA);
    if (hashes) memcpy(hashes, w->raw, CLUSTER_HASHES);
    return 0;
  }
  /* The contents' vector is read out of the hashes while they are still
   * encrypted, so the two parts can be done in either order. */
  wii_aes_cbc_decrypt(aes, w->raw + CLUSTER_IV_AT, w->raw + CLUSTER_HASHES, data, CLUSTER_DATA);
  if (hashes) wii_aes_cbc_decrypt(aes, zero_iv, w->raw, hashes, CLUSTER_HASHES);
  return 0;
}

static void forget_clusters(struct WiiDisc *w) {
  unsigned i;
  for (i = 0; i < CACHE_ENTRIES; i++) w->cache[i].used = 0;
}

/* A decrypted cluster, from the ones kept if it is there. Otherwise it
 * replaces whichever was asked for longest ago. */
static const uint8_t *cluster(GcDisc *disc, uint64_t index) {
  struct WiiDisc *w = disc->wii;
  WiiCluster *oldest = &w->cache[0];
  unsigned i;

  for (i = 0; i < CACHE_ENTRIES; i++) {
    WiiCluster *slot = &w->cache[i];
    if (slot->used && slot->index == index) {
      slot->used = ++w->clock;
      return slot->data;
    }
    if (slot->used < oldest->used) oldest = slot;
  }
  /* Nothing is kept of a cluster that failed to read. */
  oldest->used = 0;
  if (load_cluster(disc, &w->aes, index, oldest->data, NULL) != 0) return NULL;
  oldest->index = index;
  oldest->used = ++w->clock;
  return oldest->data;
}

int wii_disc_read(GcDisc *disc, uint64_t offset, void *destination, uint32_t length) {
  struct WiiDisc *w = disc ? disc->wii : NULL;

  if (!w || !w->has_partition) return -1;
  if (!w->unlocked) {
    if (w->log && !w->told_locked) fprintf(w->log, "gc_disc: %s\n", gc_disc_key_help());
    w->told_locked = 1;
    return -1;
  }
  if (offset > w->data_size || length > w->data_size - offset) return -1;

  /* With neither hashes nor encryption there are no clusters either: the
   * data is simply there. */
  if (!w->has_hashes && !w->is_encrypted) {
    return gc_disc_read_raw(disc, w->data_offset + offset, destination, length);
  }

  while (length) {
    const uint64_t index = offset / CLUSTER_DATA;
    const uint32_t within = (uint32_t)(offset % CLUSTER_DATA);
    const uint8_t *data = cluster(disc, index);
    uint32_t piece = CLUSTER_DATA - within;
    if (piece > length) piece = length;
    if (!data) return -1;
    memcpy(destination, data + within, piece);
    destination = (uint8_t *)destination + piece;
    offset += piece;
    length -= piece;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */

const char *gc_disc_key_help(void) {
  return "this Wii disc's game partition is encrypted and no key for it was given. Set "
         "WII_COMMON_KEY to the common key as 32 hex digits, or WII_KEY_FILE to the path of "
         "a key file, or put wii-common-key.bin or keys.bin beside the image; a program can "
         "call gc_disc_set_common_key() or gc_disc_set_title_key()";
}

/* Which common key the ticket says its title key is under. */
static const char *common_key_name(const struct WiiDisc *w) {
  switch (w->ticket[TICKET_KEY_INDEX]) {
    case 0: return "standard";
    case 1: return "Korean";
    case 2: return "Wii U (vWii)";
    default: return "an unknown";
  }
}

enum { KEY_FITS = 0, KEY_WRONG = 1, KEY_UNREADABLE = -1 };

/* Is this the partition's title key? Decrypting with the wrong key does
 * not fail, it produces noise, so the answer has to come from the data:
 * the first cluster is decrypted and each of its thirty-one blocks held
 * to the SHA-1 the cluster carries for it. Noise does not pass that.
 *
 * A disc whose header turns hash checking off cannot be asked that way -
 * its hashes are allowed to be wrong - so there the test is the Wii magic
 * word, which the partition's own header has at 0x18 like the disc's. */
static int key_fits(GcDisc *disc, const WiiAes *aes) {
  struct WiiDisc *w = disc->wii;
  uint8_t *data = (uint8_t *)malloc(CLUSTER_DATA + CLUSTER_HASHES);
  uint8_t *hashes = data ? data + CLUSTER_DATA : NULL;
  int result = KEY_FITS;
  unsigned i;

  if (!data) return KEY_UNREADABLE;
  if (load_cluster(disc, aes, 0, data, hashes) != 0) {
    free(data);
    return KEY_UNREADABLE;
  }
  if (w->has_hashes) {
    for (i = 0; i < CLUSTER_H0_COUNT; i++) {
      uint8_t digest[WII_SHA1_SIZE];
      wii_sha1(data + i * 0x400u, 0x400u, digest);
      if (memcmp(digest, hashes + i * WII_SHA1_SIZE, WII_SHA1_SIZE) != 0) result = KEY_WRONG;
    }
  } else if (be32(data + 0x18) != WII_DISC_MAGIC) {
    result = KEY_WRONG;
  }
  free(data);
  return result;
}

/* Tries a title key, and opens the partition with it if it is the one.
 * Opening is also when the partition's header and filesystem can first be
 * read, so that happens here; a partition that decrypts but holds no
 * filesystem this can read is put back as it was. */
static int try_title_key(GcDisc *disc, const uint8_t key[16], FILE *log) {
  struct WiiDisc *w = disc->wii;
  WiiAes aes;
  int fits;

  wii_aes_set_key(&aes, key);
  fits = key_fits(disc, &aes);
  if (fits == KEY_UNREADABLE && log) {
    fprintf(log, "gc_disc: the game partition's first cluster is not inside the image\n");
  }
  if (fits != KEY_FITS) return fits;
  /* A partition has one title key, so a key that fits an open partition
   * is the key already in use, and the filesystem callers may be holding
   * entries of stays where it is. */
  if (w->unlocked) return KEY_FITS;

  w->aes = aes;
  memcpy(w->title_key, key, sizeof(w->title_key));
  w->has_key = 1;
  w->unlocked = 1;
  forget_clusters(w);
  if (gc_disc_load_layout(disc, log) != 0) {
    w->unlocked = 0;
    w->has_key = 0;
    memset(w->title_key, 0, sizeof(w->title_key));
    memset(&w->aes, 0, sizeof(w->aes));
    forget_clusters(w);
    return KEY_UNREADABLE;
  }
  return KEY_FITS;
}

/* The ticket's title key is one AES block, encrypted under the common key
 * with the title's id - eight bytes, then eight zeroes - as the vector. */
static int try_common_key(GcDisc *disc, const uint8_t key[16], FILE *log) {
  const struct WiiDisc *w = disc->wii;
  uint8_t iv[16], title_key[16];
  WiiAes aes;

  memset(iv, 0, sizeof(iv));
  memcpy(iv, w->ticket + TICKET_TITLE_ID, 8);
  wii_aes_set_key(&aes, key);
  wii_aes_cbc_decrypt(&aes, iv, w->ticket + TICKET_TITLE_KEY, title_key, sizeof(title_key));
  return try_title_key(disc, title_key, log);
}

/* A Wii disc with a game partition, or nothing. */
static struct WiiDisc *partition_of(const GcDisc *disc) {
  return disc && disc->is_wii && disc->wii && disc->wii->has_partition ? disc->wii : NULL;
}

int gc_disc_set_title_key(GcDisc *disc, const uint8_t key[16]) {
  struct WiiDisc *w = partition_of(disc);
  int fits;

  if (!w || !key) return -1;
  if (!w->is_encrypted) return 0; /* open already, and no key would change that */
  fits = try_title_key(disc, key, w->log);
  if (fits == KEY_WRONG && w->log) {
    fprintf(w->log, "gc_disc: wrong key: that title key does not open this disc's game partition\n");
  }
  return fits == KEY_FITS ? 0 : -1;
}

int gc_disc_set_common_key(GcDisc *disc, const uint8_t key[16], FILE *log) {
  struct WiiDisc *w = partition_of(disc);
  int fits;

  if (!w || !key) {
    if (log && disc && !disc->is_wii) fprintf(log, "gc_disc: a GameCube disc has no key\n");
    return -1;
  }
  if (!w->is_encrypted) return 0;
  fits = try_common_key(disc, key, log);
  if (fits == KEY_WRONG && log) {
    fprintf(log,
            "gc_disc: wrong key: that common key does not open this disc's game partition "
            "(its ticket asks for the %s common key)\n",
            common_key_name(w));
  }
  return fits == KEY_FITS ? 0 : -1;
}

static int hex_digit(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Thirty-two hexadecimal digits, with any amount of white space among
 * them and nothing else. */
static int parse_hex_key(const uint8_t *text, size_t length, uint8_t key[16]) {
  unsigned digits = 0;
  size_t i;

  memset(key, 0, 16);
  for (i = 0; i < length; i++) {
    const int value = hex_digit(text[i]);
    if (is_space(text[i])) continue;
    if (value < 0 || digits >= 32u) return -1;
    key[digits / 2u] = (uint8_t)((key[digits / 2u] << 4) | value);
    digits++;
  }
  return digits == 32u ? 0 : -1;
}

/* The most of a key file that is looked at. A keys.bin is a kilobyte. */
#define KEY_FILE_MAX 0x10000u
/* Where the common key is in a console's one-time-programmable memory,
 * and where that memory starts in a BootMii keys.bin: after a 0x100-byte
 * text header (WiiBrew, "Hardware/OTP" and "Bootmii/NAND dump format"). */
#define OTP_COMMON_KEY 0x14u
#define KEYS_BIN_OTP 0x100u

/* Reads a key file and tries what it holds. `quiet` is for the files
 * gc_disc_open looks for on its own account: one that is not there is
 * not worth a line, though one that is there and wrong still is. */
static int load_key_file(GcDisc *disc, const char *path, FILE *log, int quiet) {
  struct WiiDisc *w = partition_of(disc);
  uint8_t *bytes;
  uint8_t hex_key[16];
  const uint8_t *candidates[2];
  unsigned count = 0, i;
  size_t size;
  FILE *file;
  int fits = KEY_WRONG;

  if (!w || !path) return -1;
  if (!w->is_encrypted) return 0;
  file = fopen(path, "rb");
  if (!file) {
    if (log && !quiet) fprintf(log, "gc_disc: cannot open the key file %s\n", path);
    return -1;
  }
  bytes = (uint8_t *)malloc(KEY_FILE_MAX);
  if (!bytes) {
    fclose(file);
    return -1;
  }
  size = fread(bytes, 1, KEY_FILE_MAX, file);
  fclose(file);

  if (size == 16u) {
    candidates[count++] = bytes;
  } else if (parse_hex_key(bytes, size, hex_key) == 0) {
    candidates[count++] = hex_key;
  } else if (size >= OTP_COMMON_KEY + 16u) {
    /* A dump of the one-time-programmable memory on its own, or the same
     * thing behind keys.bin's header. Trying both costs nothing, because
     * a candidate is only ever accepted by decrypting the disc. */
    candidates[count++] = bytes + OTP_COMMON_KEY;
    if (size >= KEYS_BIN_OTP + OTP_COMMON_KEY + 16u) {
      candidates[count++] = bytes + KEYS_BIN_OTP + OTP_COMMON_KEY;
    }
  } else {
    if (log) {
      fprintf(log,
              "gc_disc: %s is not a key file: it is neither 16 bytes, nor 32 hex digits, nor "
              "a keys.bin or OTP dump\n",
              path);
    }
    free(bytes);
    return -1;
  }

  for (i = 0; i < count && fits != KEY_FITS; i++) {
    const int result = try_common_key(disc, candidates[i], log);
    /* Unreadable outranks wrong: it is the more useful thing to know. */
    if (result == KEY_FITS || fits != KEY_UNREADABLE) fits = result;
  }
  if (fits == KEY_WRONG && log) {
    fprintf(log,
            "gc_disc: wrong key: the key in %s does not open this disc's game partition "
            "(its ticket asks for the %s common key)\n",
            path, common_key_name(w));
  }
  free(bytes);
  return fits == KEY_FITS ? 0 : -1;
}

int gc_disc_load_key_file(GcDisc *disc, const char *path, FILE *log) {
  if (log && disc && !disc->is_wii) fprintf(log, "gc_disc: a GameCube disc has no key\n");
  return load_key_file(disc, path, log, 0);
}

int gc_disc_title_key(const GcDisc *disc, uint8_t out[16]) {
  const struct WiiDisc *w = partition_of(disc);
  if (!w || !w->unlocked || !w->has_key || !out) return -1;
  memcpy(out, w->title_key, sizeof(w->title_key));
  return 0;
}

int gc_disc_is_locked(const GcDisc *disc) {
  const struct WiiDisc *w = partition_of(disc);
  return w && !w->unlocked;
}

/* The title key a generated game carries for its own disc (gc_disc.h). */
static uint8_t embedded_title_key[16];
static int have_embedded_title_key;

void gc_disc_embedded_title_key(const uint8_t key[16]) {
  memcpy(embedded_title_key, key, sizeof(embedded_title_key));
  have_embedded_title_key = 1;
}

/* The places gc_disc_open looks for a common key (gc_disc.h). */
static void find_a_key(GcDisc *disc, const char *path, FILE *log) {
  static const char *const beside[] = {"wii-common-key.bin", "keys.bin"};
  const char *text = getenv("WII_COMMON_KEY");
  const char *file = getenv("WII_KEY_FILE");
  const char *slash = strrchr(path, '/');
  const char *backslash = strrchr(path, '\\');
  size_t directory;
  char *name;
  unsigned i;

  /* The program's own key first, and quietly: a game started with some
   * other disc beside it simply goes on to look for the common key. */
  if (have_embedded_title_key && try_title_key(disc, embedded_title_key, NULL) == 0) return;

  if (text && *text) {
    uint8_t key[16];
    if (parse_hex_key((const uint8_t *)text, strlen(text), key) != 0) {
      if (log) fprintf(log, "gc_disc: WII_COMMON_KEY is set, but not to 32 hex digits\n");
    } else if (gc_disc_set_common_key(disc, key, log) == 0) {
      return;
    }
  }
  if (file && *file && load_key_file(disc, file, log, 0) == 0) return;

  /* Beside the image: its path up to and including the last separator,
   * of either kind, with the file's name in place of the image's. */
  if (backslash && (!slash || backslash > slash)) slash = backslash;
  directory = slash ? (size_t)(slash - path) + 1u : 0u;
  name = (char *)malloc(directory + 32u);
  if (!name) return;
  for (i = 0; i < sizeof(beside) / sizeof(beside[0]); i++) {
    memcpy(name, path, directory);
    strcpy(name + directory, beside[i]);
    if (load_key_file(disc, name, log, 1) == 0) break;
  }
  free(name);
}

/* ------------------------------------------------------------------ */
/* The partition                                                       */

/* The game partition's offset: the first entry of type 0 in the table.
 * The table is four groups, each a count and the place its entries are;
 * retail discs use the first group only, and put the system update in
 * front of the game. */
static int find_game_partition(GcDisc *disc, uint64_t *offset, FILE *log) {
  uint8_t groups[32];
  unsigned group, total = 0;

  if (gc_disc_read_raw(disc, PARTITION_TABLE_AT, groups, sizeof(groups)) != 0) {
    if (log) fprintf(log, "gc_disc: this Wii disc ends before its partition table\n");
    return -1;
  }
  for (group = 0; group < 4u; group++) {
    const uint32_t count = be32(groups + group * 8u);
    const uint64_t at = be32_shifted(groups + group * 8u + 4u);
    uint32_t i;

    if (count > PARTITION_MAX_COUNT) {
      if (log) fprintf(log, "gc_disc: a partition table of %u entries is not one\n", count);
      return -1;
    }
    for (i = 0; i < count; i++) {
      uint8_t entry[8];
      if (gc_disc_read_raw(disc, at + i * 8u, entry, sizeof(entry)) != 0) {
        if (log) fprintf(log, "gc_disc: this Wii disc's partition table runs off the image\n");
        return -1;
      }
      total++;
      if (be32(entry + 4) == PARTITION_TYPE_GAME) {
        *offset = be32_shifted(entry);
        return 0;
      }
    }
  }
  if (log) {
    fprintf(log, "gc_disc: this Wii disc has %u partitions and none of them is a game\n", total);
  }
  return -1;
}

/* One of the blobs the partition header points at. */
static uint8_t *read_blob(GcDisc *disc, uint64_t offset, uint32_t size) {
  uint8_t *bytes;
  if (!size || size > BLOB_MAX_SIZE) return NULL;
  bytes = (uint8_t *)malloc(size);
  if (bytes && gc_disc_read_raw(disc, offset, bytes, size) != 0) {
    free(bytes);
    bytes = NULL;
  }
  return bytes;
}

int wii_disc_open(GcDisc *disc, const char *path, FILE *log) {
  struct WiiDisc *w = state_of(disc);
  uint8_t header[PARTITION_HEADER_SIZE];
  uint8_t flags[2];
  uint64_t stored_size;

  if (!w) return -1;
  w->log = log;
  disc->is_wii = 1;
  disc->offset_shift = 2;

  /* Two bytes of the disc header can turn the hashing and the encryption
   * off. Retail discs have both clear; they are honoured because a disc
   * that sets them cannot be read any other way. */
  if (gc_disc_read_raw(disc, 0x60, flags, sizeof(flags)) != 0) return -1;
  w->has_hashes = flags[0] == 0;
  w->is_encrypted = flags[1] == 0;

  if (find_game_partition(disc, &w->partition_offset, log) != 0) return -1;
  if (gc_disc_read_raw(disc, w->partition_offset, header, sizeof(header)) != 0) {
    if (log) fprintf(log, "gc_disc: the game partition's header is not inside the image\n");
    return -1;
  }
  memcpy(w->ticket, header, sizeof(w->ticket));
  w->tmd_size = be32(header + 0x2a4);
  w->cert_size = be32(header + 0x2ac);
  w->data_offset = w->partition_offset + be32_shifted(header + 0x2b8);
  stored_size = be32_shifted(header + 0x2bc);
  if (w->data_offset == w->partition_offset || !stored_size) {
    if (log) fprintf(log, "gc_disc: the game partition's header says it holds no data\n");
    return -1;
  }
  /* The size the header gives is of the data as stored. With clusters,
   * 0x400 bytes in every 0x8000 of that are hashes. */
  w->data_size = (!w->has_hashes && !w->is_encrypted)
                     ? stored_size
                     : stored_size / CLUSTER_SIZE * CLUSTER_DATA;

  w->tmd = read_blob(disc, w->partition_offset + be32_shifted(header + 0x2a8), w->tmd_size);
  if (!w->tmd) {
    if (log) {
      fprintf(log, "gc_disc: the game partition's title metadata (%u bytes) cannot be read\n",
              w->tmd_size);
    }
    w->tmd_size = 0;
    return -1;
  }
  /* The certificate chain is only ever handed on, so a partition without
   * one is still a partition. */
  w->certs = read_blob(disc, w->partition_offset + be32_shifted(header + 0x2b0), w->cert_size);
  if (!w->certs) w->cert_size = 0;

  w->cache = (WiiCluster *)calloc(CACHE_ENTRIES, sizeof(WiiCluster));
  if (!w->cache) return -1;
  w->has_partition = 1;

  if (!w->is_encrypted) {
    w->unlocked = 1;
    if (gc_disc_load_layout(disc, log) != 0) return -1;
  } else {
    find_a_key(disc, path, log);
  }
  if (log) {
    fprintf(log, "gc_disc: a Wii disc; its game partition is at 0x%llx with %llu bytes of data, %s\n",
            (unsigned long long)w->partition_offset, (unsigned long long)w->data_size,
            !w->is_encrypted ? "not encrypted" : w->unlocked ? "open" : "locked");
  }
  return 0;
}

void wii_disc_close(GcDisc *disc) {
  struct WiiDisc *w = disc ? disc->wii : NULL;
  if (!w) return;
  free(w->wbfs_table);
  free(w->tmd);
  free(w->certs);
  free(w->cache);
  /* The key is not this program's to leave lying in freed memory. */
  memset(w, 0, sizeof(*w));
  free(w);
  disc->wii = NULL;
}

uint64_t gc_disc_partition_offset(const GcDisc *disc) {
  const struct WiiDisc *w = partition_of(disc);
  return w ? w->partition_offset : 0;
}

uint64_t gc_disc_data_size(const GcDisc *disc) {
  const struct WiiDisc *w = partition_of(disc);
  return w ? w->data_size : 0;
}

const uint8_t *gc_disc_ticket(const GcDisc *disc, uint32_t *size) {
  const struct WiiDisc *w = partition_of(disc);
  if (size) *size = w ? WII_TICKET_SIZE : 0;
  return w ? w->ticket : NULL;
}

const uint8_t *gc_disc_tmd(const GcDisc *disc, uint32_t *size) {
  const struct WiiDisc *w = partition_of(disc);
  if (size) *size = w ? w->tmd_size : 0;
  return w ? w->tmd : NULL;
}

const uint8_t *gc_disc_cert_chain(const GcDisc *disc, uint32_t *size) {
  const struct WiiDisc *w = partition_of(disc);
  if (size) *size = w ? w->cert_size : 0;
  return w ? w->certs : NULL;
}
