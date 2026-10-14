/*
 * Reading a GameCube disc image.
 *
 * The format is small: a 0x440-byte header with the game's id and the
 * offsets of the executable and the filesystem, then a filesystem table of
 * twelve-byte entries followed by their names. Everything on a disc is
 * big-endian, so every field is assembled a byte at a time rather than cast
 * - the host may be either way round, and a struct overlay would be wrong
 * on one of them.
 *
 * A Wii disc keeps exactly that inside an encrypted partition. Getting at
 * the partition is wii_disc.c's business; once it has, the header and the
 * filesystem are read by the code below, through the same gc_disc_read,
 * with one difference - a Wii disc stores its offsets divided by four.
 */

#include <stdlib.h>
#include <string.h>

#include "gc_disc.h"
#include "gc_perf.h"
#include "wii_disc.h"

#define FST_ENTRY_SIZE 12u
/* A sanity bound, not a hardware limit: the largest retail filesystems are
 * a few thousand entries, and a corrupt header should not be allowed to ask
 * for a gigabyte of them. */
#define FST_MAX_ENTRIES 200000u

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t be24(const uint8_t *p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* How long the file is, in a size that can hold it. */
static int seek_to_end(FILE *file, uint64_t *size) {
#if defined(_MSC_VER)
  if (_fseeki64(file, 0, SEEK_END) != 0) return -1;
  *size = (uint64_t)_ftelli64(file);
#else
  if (fseeko(file, 0, SEEK_END) != 0) return -1;
  *size = (uint64_t)ftello(file);
#endif
  return 0;
}

/* A seek that can reach the far end of a disc. `fseek` takes a long, and a
 * long is thirty-two bits on Windows - which is just enough for a
 * GameCube disc and not enough to rely on. */
static int seek_to(FILE *file, uint64_t offset) {
#if defined(_MSC_VER)
  return _fseeki64(file, (__int64)offset, SEEK_SET);
#else
  return fseeko(file, (off_t)offset, SEEK_SET);
#endif
}

/* Bytes straight out of the file, wherever the file keeps them. */
int gc_disc_file_read(GcDisc *disc, uint64_t offset, void *destination, uint32_t length) {
  if (seek_to(disc->file, offset) != 0) return -1;
  if (length && fread(destination, 1, length, disc->file) != length) return -1;
  return 0;
}

/* Is this a CISO, and if so where is each of its blocks?
 *
 * The map says which blocks the file keeps; the ones it keeps follow the
 * header back to back, in map order. So walking the map once gives both
 * the position of every block and the size of the disc the file stands
 * for - which is not the size of the file, and is what every offset in
 * the disc's own header is relative to. Returns 0 for "not a CISO" as
 * well as for "a CISO, understood"; only a broken one is an error. */
static int open_ciso(GcDisc *disc, FILE *log) {
  uint8_t head[GC_CISO_HEADER];
  uint32_t stored = 0, last = 0, i;

  if (disc->size < sizeof(head)) return 0;
  if (gc_disc_file_read(disc, 0, head, sizeof(head)) != 0) return 0;
  if (!(head[0] == 'C' && head[1] == 'I' && head[2] == 'S' && head[3] == 'O')) return 0;

  disc->ciso_block_size = (uint32_t)head[4] | ((uint32_t)head[5] << 8) |
                          ((uint32_t)head[6] << 16) | ((uint32_t)head[7] << 24);
  if (!disc->ciso_block_size || (disc->ciso_block_size & (disc->ciso_block_size - 1u))) {
    if (log) fprintf(log, "gc_disc: a CISO block size of %u makes no sense\n",
                     disc->ciso_block_size);
    return -1;
  }

  disc->ciso_blocks = GC_CISO_MAP_ENTRIES;
  disc->ciso_position = (uint32_t *)malloc(sizeof(uint32_t) * disc->ciso_blocks);
  if (!disc->ciso_position) return -1;
  for (i = 0; i < disc->ciso_blocks; i++) {
    if (head[8 + i]) {
      disc->ciso_position[i] = stored++;
      last = i;
    } else {
      disc->ciso_position[i] = GC_CISO_ABSENT;
    }
  }
  if (!stored) {
    if (log) fprintf(log, "gc_disc: this CISO stores no blocks at all\n");
    return -1;
  }
  /* The disc runs to the end of the last block the file keeps. Everything
   * between the blocks it keeps is zeroes, and reads there are answered
   * as zeroes rather than as failures. */
  disc->size = (uint64_t)(last + 1u) * disc->ciso_block_size;
  if (log) {
    fprintf(log, "gc_disc: a CISO of %u blocks of %u bytes, standing for %llu\n", stored,
            disc->ciso_block_size, (unsigned long long)disc->size);
  }
  return 0;
}

static int read_disc(GcDisc *disc, uint64_t offset, void *destination, uint32_t length);

int gc_disc_read(GcDisc *disc, uint64_t offset, void *destination, uint32_t length) {
  const double started = gc_perf_now_ms();
  /* What a game reads from a Wii disc is the inside of its game
   * partition, which wii_disc.c decrypts out of the disc below. */
  const int result = disc && disc->is_wii ? wii_disc_read(disc, offset, destination, length)
                                          : read_disc(disc, offset, destination, length);
  gc_perf.disc_reads++;
  gc_perf.disc_bytes += length;
  gc_perf.disc_ms += gc_perf_now_ms() - started;
  return result;
}

/* The disc itself. A GameCube disc has nothing else, so there this is the
 * read above and is counted as one; a Wii disc's raw reads are mostly the
 * clusters a read above is waiting for, and counting those too would
 * count every byte twice. */
int gc_disc_read_raw(GcDisc *disc, uint64_t offset, void *destination, uint32_t length) {
  if (disc && !disc->is_wii) return gc_disc_read(disc, offset, destination, length);
  return read_disc(disc, offset, destination, length);
}

static int read_disc(GcDisc *disc, uint64_t offset, void *destination, uint32_t length) {
  if (!disc || !disc->file) return -1;
  /* Written so that an offset near the top of sixty-four bits cannot
   * wrap round into range. */
  if (offset > disc->size || length > disc->size - offset) return -1;
  if (wii_disc_is_wbfs(disc)) return wii_wbfs_read(disc, offset, destination, length);
  if (!disc->ciso_block_size) return gc_disc_file_read(disc, offset, destination, length);

  /* A CISO read is the same read, split at block boundaries, with the
   * blocks the file left out answered as the zeroes they stand for. */
  while (length) {
    const uint64_t block = offset / disc->ciso_block_size;
    const uint32_t within = (uint32_t)(offset % disc->ciso_block_size);
    uint32_t piece = disc->ciso_block_size - within;
    if (piece > length) piece = length;
    if (block >= disc->ciso_blocks || disc->ciso_position[block] == GC_CISO_ABSENT) {
      memset(destination, 0, piece);
    } else {
      const uint64_t at = (uint64_t)GC_CISO_HEADER +
                          (uint64_t)disc->ciso_position[block] * disc->ciso_block_size + within;
      if (gc_disc_file_read(disc, at, destination, piece) != 0) return -1;
    }
    destination = (uint8_t *)destination + piece;
    offset += piece;
    length -= piece;
  }
  return 0;
}

/* The filesystem, as the disc holds it, plus an index over it. The names
 * live in a string table that follows the entries; a name that runs off the
 * end of the table is a corrupt disc, and the whole table is rejected
 * rather than half-trusted. */
static int read_filesystem(GcDisc *disc, FILE *log) {
  uint32_t count, strings_at, i;

  if (disc->fst_size < FST_ENTRY_SIZE || disc->fst_size > FST_ENTRY_SIZE * FST_MAX_ENTRIES) {
    if (log) fprintf(log, "gc_disc: a filesystem of %u bytes is not one\n", disc->fst_size);
    return -1;
  }
  disc->fst = (uint8_t *)malloc(disc->fst_size);
  if (!disc->fst) return -1;
  if (gc_disc_read(disc, gc_disc_fst_offset(disc), disc->fst, disc->fst_size) != 0) {
    if (log) fprintf(log, "gc_disc: the filesystem is not inside the image\n");
    return -1;
  }

  count = be32(disc->fst + 8); /* the root entry's "next" is the total */
  if (!count || count > FST_MAX_ENTRIES || count * FST_ENTRY_SIZE > disc->fst_size) {
    if (log) fprintf(log, "gc_disc: a filesystem claiming %u entries does not fit\n", count);
    return -1;
  }
  strings_at = count * FST_ENTRY_SIZE;

  disc->entries = (GcDiscEntry *)calloc(count, sizeof(GcDiscEntry));
  if (!disc->entries) return -1;
  disc->entry_count = count;
  disc->strings = (const char *)disc->fst + strings_at;

  for (i = 0; i < count; i++) {
    const uint8_t *raw = disc->fst + i * FST_ENTRY_SIZE;
    const uint32_t name_at = strings_at + be24(raw + 1);
    GcDiscEntry *entry = &disc->entries[i];

    entry->is_directory = raw[0] != 0;
    entry->offset = be32(raw + 4);
    entry->size = be32(raw + 8);
    entry->parent = entry->is_directory ? entry->offset : 0;

    if (i == 0) {
      entry->name = ""; /* the root has no name of its own */
      continue;
    }
    if (name_at >= disc->fst_size ||
        !memchr(disc->fst + name_at, 0, disc->fst_size - name_at)) {
      if (log) fprintf(log, "gc_disc: entry %u has a name outside the table\n", i);
      return -1;
    }
    entry->name = (const char *)disc->fst + name_at;
  }
  return 0;
}

static void drop_filesystem(GcDisc *disc) {
  free(disc->entries);
  free(disc->fst);
  disc->entries = NULL;
  disc->fst = NULL;
  disc->strings = NULL;
  disc->entry_count = 0;
}

/* The game's id and name, which a Wii disc has twice: in the clear at the
 * front of the disc, and again inside its game partition. */
static void read_identity(GcDisc *disc, const uint8_t *header) {
  memcpy(disc->game_code, header, 6);
  disc->game_code[6] = 0;
  memcpy(disc->name, header + 0x20, sizeof(disc->name) - 1);
  disc->name[sizeof(disc->name) - 1] = 0;
}

/* A size a Wii disc stores as a quarter of itself, in bytes - or, if the
 * bytes would not fit in the field, the largest value there is, which no
 * check that follows will take for a real size. */
static uint32_t stored_size(const GcDisc *disc, const uint8_t *p) {
  const uint32_t stored = be32(p);
  return stored > (0xffffffffu >> disc->offset_shift) ? 0xffffffffu : stored << disc->offset_shift;
}

/* Everything the 0x440-byte header says, and the filesystem it points at.
 * The header is the disc's first bytes on a GameCube disc and the game
 * partition's first bytes on a Wii disc; either way it is `header`. */
static int load_layout(GcDisc *disc, const uint8_t *header, FILE *log) {
  read_identity(disc, header);
  disc->dol_offset = be32(header + 0x420);
  disc->fst_offset = be32(header + 0x424);
  disc->fst_size = stored_size(disc, header + 0x428);
  disc->fst_max_size = stored_size(disc, header + 0x42c);

  drop_filesystem(disc);
  if (read_filesystem(disc, log) != 0) {
    drop_filesystem(disc);
    return -1;
  }
  return 0;
}

int gc_disc_load_layout(GcDisc *disc, FILE *log) {
  uint8_t header[GC_DISC_HEADER_SIZE];

  if (gc_disc_read(disc, 0, header, sizeof(header)) != 0) {
    if (log) fprintf(log, "gc_disc: the game partition is too short to hold a disc header\n");
    return -1;
  }
  if (disc->is_wii && be32(header + 0x18) != WII_DISC_MAGIC) {
    if (log) {
      fprintf(log, "gc_disc: the game partition has no Wii magic at 0x18 (found %08x)\n",
              be32(header + 0x18));
    }
    return -1;
  }
  return load_layout(disc, header, log);
}

/* The compressed formats this does not read, by the magic word each one
 * starts with. They are worth telling apart from "not a disc": the file
 * is fine, and the person holding it can do something about it. */
static const char *unsupported_format(const uint8_t *magic) {
  if (magic[0] == 'R' && magic[1] == 'V' && magic[2] == 'Z' && magic[3] == 1) return "an RVZ";
  if (magic[0] == 'W' && magic[1] == 'I' && magic[2] == 'A' && magic[3] == 1) return "a WIA";
  /* GCZ's magic is the word 0xb10bc001, stored little-endian. */
  if (magic[0] == 0x01 && magic[1] == 0xc0 && magic[2] == 0x0b && magic[3] == 0xb1) return "a GCZ";
  return NULL;
}

/* "game.wbf1": the second file of a WBFS split to fit a FAT32 drive. It
 * has no header of its own - it is the middle of something. */
static int is_wbfs_continuation(const char *path) {
  const size_t length = strlen(path);
  const char *end = path + length;
  return length >= 5u && end[-5] == '.' && (end[-4] == 'w' || end[-4] == 'W') &&
         (end[-3] == 'b' || end[-3] == 'B') && (end[-2] == 'f' || end[-2] == 'F') &&
         end[-1] >= '0' && end[-1] <= '9';
}

GcDisc *gc_disc_open(const char *path, FILE *log) {
  uint8_t header[GC_DISC_HEADER_SIZE];
  uint8_t magic[4] = {0, 0, 0, 0};
  int wbfs;
  GcDisc *disc = (GcDisc *)calloc(1, sizeof(GcDisc));
  if (!disc) return NULL;

  disc->file = fopen(path, "rb");
  if (!disc->file) {
    if (log) fprintf(log, "gc_disc: cannot open %s\n", path);
    gc_disc_close(disc);
    return NULL;
  }
  if (seek_to_end(disc->file, &disc->size) != 0) {
    if (log) fprintf(log, "gc_disc: cannot measure %s\n", path);
    gc_disc_close(disc);
    return NULL;
  }

  if (is_wbfs_continuation(path)) {
    if (log) {
      fprintf(log,
              "gc_disc: %s is a later part of a split WBFS, which is not supported: join the "
              "parts into one .wbfs, or convert to ISO\n",
              path);
    }
    gc_disc_close(disc);
    return NULL;
  }
  if (disc->size >= sizeof(magic) && gc_disc_file_read(disc, 0, magic, sizeof(magic)) == 0 &&
      unsupported_format(magic)) {
    if (log) {
      fprintf(log,
              "gc_disc: %s is %s image, which this cannot read yet: convert it to ISO or WBFS "
              "for now\n",
              path, unsupported_format(magic));
    }
    gc_disc_close(disc);
    return NULL;
  }

  /* A compressed image announces itself in its first four bytes. Working
   * it out here means every read below, and every read a game makes
   * later, goes through one path that already knows. */
  wbfs = wii_wbfs_open(disc, path, log);
  if (wbfs < 0 || (!wbfs && open_ciso(disc, log) != 0)) {
    gc_disc_close(disc);
    return NULL;
  }

  if (gc_disc_read_raw(disc, 0, header, sizeof(header)) != 0) {
    if (log) fprintf(log, "gc_disc: %s is too short to be a disc\n", path);
    gc_disc_close(disc);
    return NULL;
  }

  if (be32(header + 0x18) == WII_DISC_MAGIC) {
    /* The id and name from the front of the disc stand until the game
     * partition is open, which replaces them with its own - and without
     * a key that is all that is known about what is in it. */
    read_identity(disc, header);
    if (wii_disc_open(disc, path, log) != 0) {
      gc_disc_close(disc);
      return NULL;
    }
    return disc;
  }

  if (be32(header + 0x1c) != GC_DISC_MAGIC) {
    if (log) {
      fprintf(log,
              "gc_disc: %s has no GameCube disc magic at 0x1c (found %08x) and no Wii disc "
              "magic at 0x18 (found %08x)\n",
              path, be32(header + 0x1c), be32(header + 0x18));
    }
    gc_disc_close(disc);
    return NULL;
  }

  if (load_layout(disc, header, log) != 0) {
    gc_disc_close(disc);
    return NULL;
  }
  return disc;
}

int gc_disc_console(const char *path) {
  GcDisc *disc = gc_disc_open(path, NULL);
  int console;
  if (!disc) return 0;
  console = disc->is_wii ? 2 : 1;
  gc_disc_close(disc);
  return console;
}

void gc_disc_close(GcDisc *disc) {
  if (disc) free(disc->ciso_position);
  if (!disc) return;
  wii_disc_close(disc);
  if (disc->file) fclose(disc->file);
  free(disc->entries);
  free(disc->fst);
  free(disc);
}

uint64_t gc_disc_dol_offset(const GcDisc *disc) {
  return disc ? (uint64_t)disc->dol_offset << disc->offset_shift : 0;
}

uint64_t gc_disc_fst_offset(const GcDisc *disc) {
  return disc ? (uint64_t)disc->fst_offset << disc->offset_shift : 0;
}

uint64_t gc_disc_entry_offset(const GcDisc *disc, const GcDiscEntry *entry) {
  /* A directory keeps its parent's index where a file keeps its offset. */
  if (!disc || !entry || entry->is_directory) return 0;
  return (uint64_t)entry->offset << disc->offset_shift;
}

/* One path segment against an entry's name. */
static int segment_is(const char *name, const char *segment, size_t length) {
  return strlen(name) == length && strncmp(name, segment, length) == 0;
}

int gc_disc_read_dol(GcDisc *disc, void **data, uint32_t *size) {
  uint8_t header[0x100];
  uint32_t length = sizeof(header);
  unsigned i;
  void *bytes;

  if (!disc || gc_disc_read(disc, gc_disc_dol_offset(disc), header, sizeof(header)) != 0) return -1;
  for (i = 0; i < 18u; i++) {
    const uint32_t offset = be32(header + i * 4);
    const uint32_t section = be32(header + 0x90 + i * 4);
    if (section && offset + section > length) length = offset + section;
  }
  bytes = malloc(length);
  if (!bytes) return -1;
  if (gc_disc_read(disc, gc_disc_dol_offset(disc), bytes, length) != 0) {
    free(bytes);
    return -1;
  }
  *data = bytes;
  *size = length;
  return 0;
}

const GcDiscEntry *gc_disc_find(const GcDisc *disc, const char *path) {
  uint32_t at = 1;            /* entry 0 is the root */
  uint32_t end;
  if (!disc || !disc->entries || !path) return NULL;
  end = disc->entries[0].size;

  while (*path == '/') path++;
  while (*path) {
    const char *slash = strchr(path, '/');
    const size_t length = slash ? (size_t)(slash - path) : strlen(path);
    int found = 0;

    for (; at < end; at++) {
      const GcDiscEntry *entry = &disc->entries[at];
      if (!segment_is(entry->name, path, length)) {
        /* Skip a whole subtree that is not the one being descended into. */
        if (entry->is_directory) at = entry->size - 1;
        continue;
      }
      if (slash) {
        if (!entry->is_directory) return NULL;
        end = entry->size; /* the directory's entries run up to here */
        at++;
      } else {
        return entry->is_directory ? NULL : entry;
      }
      found = 1;
      break;
    }
    if (!found) return NULL;
    path = slash ? slash + 1 : path + length;
    while (*path == '/') path++;
  }
  return NULL;
}
