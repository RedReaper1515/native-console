/*
 * The disc: a GameCube or Wii image, its filesystem, and reads at an offset.
 *
 * A disc is 1.36 GB, so it is never loaded into memory - the image stays
 * open and reads are served from it. That is also what the hardware does,
 * and it is what lets `gc_run` and the recompiled game share one path:
 * the drive answers a DVD read, and whoever asked gets bytes.
 *
 * The filesystem is small enough to hold: a few thousand entries and a
 * string table, read once. `gcrecomp` walks it to decide what goes into
 * `ID.gcdata` (docs/GC-PLAN.md, "Contract deviations"), and the runtime
 * walks it to answer a game asking for a file by name.
 *
 * A Wii disc is the same thing one layer down. The disc itself holds a
 * partition table and, in its game partition, an encrypted GameCube-style
 * disc: the same header, the same apploader at 0x2440, the same
 * filesystem. So for a Wii disc `gc_disc_read` answers with the decrypted
 * contents of that partition, addressed from the partition's start, and
 * everything built on it works unchanged; `gc_disc_read_raw` is the disc
 * underneath (src/wii_disc.c). Two things differ and both are in this
 * header: the partition needs a key before it can be read, and the
 * offsets its header and filesystem store are a quarter of the real ones.
 */
#ifndef GC_DISC_H
#define GC_DISC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_DISC_MAGIC 0xc2339f3du
#define GC_DISC_HEADER_SIZE 0x440u
#define GC_CISO_ABSENT 0xffffffffu

/* A GameCube disc says what it is at 0x1c and a Wii disc four bytes
 * earlier, at 0x18 - and again at 0x18 inside its game partition. */
#define WII_DISC_MAGIC 0x5d1c9ea3u
#define WII_TICKET_SIZE 0x2a4u

typedef struct GcDiscEntry {
  int is_directory;
  const char *name;  /* into the string table below */
  uint32_t offset;   /* a file's position on the disc, as the disc stores
                      * it: gc_disc_entry_offset() gives the bytes */
  uint32_t size;     /* a file's length; a directory's end index */
  uint32_t parent;   /* a directory's parent index */
} GcDiscEntry;

/* A CISO's header is a magic word, a block size, and one byte per block
 * saying whether that block is in the file at all. Blocks that are all
 * zeroes - and a disc is mostly zeroes - are simply left out, and the ones
 * that remain follow the header back to back in map order. */
#define GC_CISO_MAGIC 0x4f534943u /* "CISO", little-endian as it is stored */
#define GC_CISO_HEADER 0x8000u
#define GC_CISO_MAP_ENTRIES (GC_CISO_HEADER - 8u)

struct WiiDisc; /* what src/wii_disc.c keeps about a Wii disc */

typedef struct GcDisc {
  FILE *file;
  uint64_t size; /* of the disc, which for a CISO is not the file's size */

  /* Zero when the image is a plain one. Otherwise the block size and,
   * per block, where it sits among the blocks the file actually stores -
   * or GC_CISO_ABSENT for a block that was left out. */
  uint32_t ciso_block_size;
  uint32_t ciso_blocks;
  uint32_t *ciso_position;

  char game_code[8]; /* the six-character id, terminated */
  char name[0x400];  /* the game's own title, terminated */

  /* Where the executable and the filesystem are, as the disc stores it.
   * On a GameCube disc that is a byte offset; on a Wii disc it is a
   * quarter of one. gc_disc_dol_offset() and gc_disc_fst_offset() give
   * bytes on either. The two sizes are always in bytes. */
  uint32_t dol_offset;
  uint32_t fst_offset;
  uint32_t fst_size;
  uint32_t fst_max_size;

  uint8_t *fst;      /* the table exactly as the disc holds it */
  GcDiscEntry *entries;
  uint32_t entry_count;
  const char *strings;

  /* Everything above is as it was before there were Wii discs, in type
   * and in order - a save state hashes some of it by size. */
  int is_wii;
  unsigned offset_shift; /* 0, or 2 on a Wii disc: stored offset << shift = bytes */
  struct WiiDisc *wii;   /* NULL unless the image is a Wii disc or a WBFS */
} GcDisc;

/* Which console a disc image is for, without keeping it open: 0 when the
 * file is not a disc this can read, 1 for a GameCube disc, 2 for a Wii
 * disc (locked or not). For choosing the machine before it is made. */
int gc_disc_console(const char *path);

/* Opens an image and reads its header and filesystem. Returns NULL and
 * says why on `log` if the file is not a disc this can read.
 *
 * The image may be a plain one (.iso, .gcm), a CISO, or a single-file
 * WBFS. A Wii disc opens whether or not there is a key for it: without
 * one the game partition stays locked (see below), and the fields above
 * that come from inside it - the filesystem, the DOL's offset - stay
 * empty until a key is supplied. `log` is kept, for the one line a read
 * of a locked disc produces. */
GcDisc *gc_disc_open(const char *path, FILE *log);
void gc_disc_close(GcDisc *disc);

/* Bytes at a disc offset. Returns 0 on success; a read past the end of the
 * image is a failure, not a short read filled with zeroes.
 *
 * On a Wii disc the offset is into the decrypted game partition, and a
 * read of a locked partition fails. */
int gc_disc_read(GcDisc *disc, uint64_t offset, void *destination, uint32_t length);

/* Bytes of the disc itself: on a Wii disc, the unencrypted area in front
 * of the partitions, a partition's header, or its data as stored - still
 * encrypted. On a GameCube disc this is gc_disc_read. */
int gc_disc_read_raw(GcDisc *disc, uint64_t disc_offset, void *destination, uint32_t length);

/* The executable the disc boots: a copy of its DOL, and how long it is.
 * How long is not written down anywhere - it is however far the last of
 * the DOL's eighteen sections reaches. The caller owns the bytes. */
int gc_disc_read_dol(GcDisc *disc, void **data, uint32_t *size);

/* An entry by path, "opening.bnr" or "audio/track01.ast", case-sensitively
 * as the disc holds it. Returns NULL when there is no such file. */
const GcDiscEntry *gc_disc_find(const GcDisc *disc, const char *path);

/* Byte offsets, for gc_disc_read, on either kind of disc. An entry that is
 * a directory has no offset and answers 0. */
uint64_t gc_disc_dol_offset(const GcDisc *disc);
uint64_t gc_disc_fst_offset(const GcDisc *disc);
uint64_t gc_disc_entry_offset(const GcDisc *disc, const GcDiscEntry *entry);

/* ------------------------------------------------------------------ */
/* Wii discs                                                           */

/* The key.
 *
 * A Wii game partition is encrypted with a key of its own, the title key,
 * which the partition's ticket holds encrypted with a key every console
 * has, the common key. Neither is in this repository and neither will be:
 * the reader is handed one or the other by whoever holds the disc.
 *
 * gc_disc_open() looks for the common key in these places, in this order,
 * and stops at the first that opens the partition:
 *
 *   the environment variable WII_COMMON_KEY, as 32 hexadecimal digits
 *   the environment variable WII_KEY_FILE, as the path of a key file
 *   a file named wii-common-key.bin, then keys.bin, beside the image
 *
 * A key is only ever accepted by being right: the partition's first
 * cluster is decrypted with it and held to the hashes the cluster carries
 * of its own contents. A key that fails is reported as the wrong key, and
 * the disc stays locked rather than reading as noise.
 *
 * Each of these returns 0 when the partition is open afterwards and -1
 * otherwise - including on a GameCube disc, which has no key to set. */

/* The title key itself, already decrypted: what a generated game embeds
 * for its own disc. A wrong one is reported on the log given to
 * gc_disc_open(). */
int gc_disc_set_title_key(GcDisc *disc, const uint8_t key[16]);

/* The same for every disc this process opens from now on: a generated game
 * calls this once, with the title key of the disc it was built from, before
 * it opens the disc beside it. A key that does not fit the disc that is
 * opened is ignored, and the usual places are searched. */
void gc_disc_embedded_title_key(const uint8_t key[16]);
/* The common key, which decrypts the title key in the ticket. */
int gc_disc_set_common_key(GcDisc *disc, const uint8_t key[16], FILE *log);

/* The common key from a file, in any of three forms: sixteen bytes and
 * nothing else; a text file of 32 hexadecimal digits, with or without
 * white space among them; or a BootMii `keys.bin` or a dump of a console's
 * one-time-programmable memory, in which the common key has a fixed
 * place. */
int gc_disc_load_key_file(GcDisc *disc, const char *path, FILE *log);

/* The title key in use, for the recompiler to embed. Returns -1 while the
 * partition is locked, and for a disc that has no key: a GameCube disc, or
 * a Wii disc whose header says it is not encrypted. */
int gc_disc_title_key(const GcDisc *disc, uint8_t out[16]);

/* Nonzero for a Wii disc whose game partition has no key yet. */
int gc_disc_is_locked(const GcDisc *disc);

/* The sentence a locked disc's read puts on the log, for a caller that
 * wants to say it somewhere else: that a key is needed, and the ways of
 * supplying one. */
const char *gc_disc_key_help(void);

/* Where the game partition starts on the disc, and how many bytes of
 * decrypted data it holds. Both are 0 for a GameCube disc. */
uint64_t gc_disc_partition_offset(const GcDisc *disc);
uint64_t gc_disc_data_size(const GcDisc *disc);

/* The partition's ticket, its title metadata and its certificate chain,
 * exactly as the disc holds them: what a game is handed when it opens the
 * partition, and what the system software's /dev/es is asked about. The
 * bytes belong to the disc and last until it is closed. Each returns NULL
 * and a size of 0 where there is none - on a GameCube disc, and for a
 * chain the partition does not carry. `size` may be NULL. */
const uint8_t *gc_disc_ticket(const GcDisc *disc, uint32_t *size);
const uint8_t *gc_disc_tmd(const GcDisc *disc, uint32_t *size);
const uint8_t *gc_disc_cert_chain(const GcDisc *disc, uint32_t *size);

#ifdef __cplusplus
}
#endif

#endif /* GC_DISC_H */
