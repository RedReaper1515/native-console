/*
 * The Wii's flash memory, as the filesystem a game sees.
 *
 * A Wii has 512 MB of flash soldered to its board. IOS keeps a filesystem
 * on it - ISFS - and a game reaches that filesystem only by asking: saves
 * go in /title/<id>/data, the console's settings are in /shared2/sys, and
 * /tmp is scratch space. None of the chip beneath (pages, spare bytes,
 * error correction, the encryption and signing of every cluster) is
 * visible to a game, so none of it is here. What is here is the tree
 * itself: directories and files with the owner, group, permissions and
 * attribute byte ISFS keeps for each, its limits, and its error numbers.
 *
 * This file is in two parts. The first is the tree and nothing else: it
 * knows no machine and no IOS, takes the caller's identity as a parameter
 * of every call, and can be driven from a test. The second, at the end, is
 * where a machine keeps its tree and how that reaches a file on the host
 * (wii_ios_fs.c).
 *
 * On the host the whole filesystem is ONE file, written whole and replaced
 * atomically, as a GameCube memory card is (gc_card.h): the hub keeps a
 * game's persistent files by name, and a directory tree would not survive
 * a rebuild. That file is a format of this project's own. It is NOT a NAND
 * dump - no console's dump can be loaded and this cannot be written to a
 * console - because a real dump is encrypted with keys that are a
 * console's own and that this project does not have or want.
 *
 * The limits and error numbers are ISFS's, from WiiBrew's /dev/fs and
 * Hardware/NAND pages and from the result names in the SDK's IPC client
 * (doldecomp/ogws); libogc's isfs.c (zlib licence) is the client whose
 * buffer layouts the device in wii_ios_fs.c answers.
 */
#ifndef WII_NAND_H
#define WII_NAND_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "wii_sysconf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GcMachine;

/* ISFS's own results. The four at the end of the first group are what the
 * chip reports when it is failing - a superblock that will not write, an
 * uncorrectable page, a cluster whose signature is wrong, a filesystem
 * busy with something else - and a tree in memory cannot fail in any of
 * those ways, so they are named and never returned. */
enum {
  WII_FS_OK = 0,
  WII_FS_EINVAL = -101,       /* a malformed path, a file where a directory is needed, a bad argument */
  WII_FS_EACCESS = -102,      /* the caller may not */
  WII_FS_EEXIST = -105,
  WII_FS_ENOENT = -106,
  WII_FS_ENFILE = -107,       /* no entry left in the file table */
  WII_FS_EFBIG = -108,        /* no cluster left on the chip */
  WII_FS_EFDEXHAUSTED = -109, /* sixteen descriptors are open */
  WII_FS_EDIRDEPTH = -110,    /* a directory more than eight deep */
  WII_FS_EFDOPEN = -111,      /* deleting or moving something that is open */
  WII_FS_ENOTEMPTY = -115,    /* giving away a file that has contents */
  WII_FS_ESHUTDOWN = -119,    /* the filesystem has been told to stop */

  WII_FS_ECORRUPT = -103,
  WII_FS_EIO = -114,
  WII_FS_ECHECK = -116,
  WII_FS_EBUSY = -118
};

/* ISFS's limits. A name is at most twelve characters because that is the
 * width of the field in a file-table entry; a path travels in a 64-byte
 * buffer with its terminator; the table has 6,143 entries. */
#define WII_NAND_NAME_MAX 12u
#define WII_NAND_PATH_MAX 63u
#define WII_NAND_DEPTH_MAX 8u
#define WII_NAND_INODES 6143u
#define WII_NAND_OPEN_MAX 16u

/* The chip: 512 MB in clusters of 16 KB, 0x8000 of them. The first
 * sixty-four hold the two boot stages and the last two hundred and
 * fifty-six the sixteen copies of the filesystem's own tables, and the
 * filesystem counts neither: the figures it reports add up to the 0x7ec0
 * between. Of those it holds 0x300 in reserve, which leaves 0x7bc0 -
 * 495 MB - that files can have. */
#define WII_NAND_CLUSTER 0x4000u
#define WII_NAND_CHIP_CLUSTERS 0x8000u
#define WII_NAND_CLUSTERS 0x7ec0u
#define WII_NAND_RESERVED_CLUSTERS 0x300u
#define WII_NAND_USABLE_CLUSTERS (WII_NAND_CLUSTERS - WII_NAND_RESERVED_CLUSTERS)

/* Permission bits, the same three times over: for the owner, for the
 * owner's group, and for everyone else. They are also the modes a file is
 * opened with. */
#define WII_NAND_READ 1u
#define WII_NAND_WRITE 2u

typedef struct WiiNand WiiNand;

/* Who is asking: the user and group of the running title. User zero is
 * IOS itself and is refused nothing. */
typedef struct WiiNandCaller {
  uint32_t uid;
  uint16_t gid;
} WiiNandCaller;

typedef struct WiiNandAttr {
  uint32_t uid;
  uint16_t gid;
  uint8_t owner, group, other; /* WII_NAND_READ | WII_NAND_WRITE each */
  uint8_t attributes;          /* a byte ISFS stores and never interprets */
  uint8_t is_directory;        /* reported; ignored when setting */
} WiiNandAttr;

typedef struct WiiNandStats {
  uint32_t cluster_size;
  uint32_t free_clusters;
  uint32_t used_clusters;
  uint32_t bad_clusters;
  uint32_t reserved_clusters;
  uint32_t free_inodes;
  uint32_t used_inodes;
} WiiNandStats;

/* ------------------------------------------------------------------ */
/* The tree                                                            */

/* A filesystem holding nothing but its root, or NULL. */
WiiNand *wii_nand_create(void);
void wii_nand_destroy(WiiNand *nand);

/* What a console's flash holds before any game has run: /sys (with the
 * table of which title has which user number), /ticket, /title,
 * /title/00000001/00000002/data with setting.txt, /shared1, /shared2,
 * /shared2/sys with SYSCONF, /tmp, /import and /meta. Everything already
 * in the filesystem is discarded. `settings` may be NULL for the defaults.
 * Zero, or -1 (out of memory, or settings that cannot be expressed), in
 * which case the filesystem is left with only its root. */
int wii_nand_format(WiiNand *nand, const WiiSettings *settings);

/* Write SYSCONF and setting.txt again from `settings`, replacing whatever
 * is there - what a game stored in them included. */
int wii_nand_apply_settings(WiiNand *nand, const WiiSettings *settings);

/* A title's directories: /title/<high>/<low>/content, which is IOS's, and
 * /title/<high>/<low>/data, which is the title's own - owned by its user
 * number and the group in its metadata, and closed to everyone else, the
 * way IOS makes them when a title is first installed or first run from a
 * disc. Does nothing where they already exist. Returns zero or an ISFS
 * error. */
int32_t wii_nand_add_title(WiiNand *nand, uint64_t title_id, uint16_t group_id);

/* The user number IOS has given a title, from /sys/uid.sys: the system
 * menu has 0x1000 and each title after it the next number, in the order
 * they were first seen. With `allocate` a title not in the table is added
 * to it; without, it gets zero. */
uint32_t wii_nand_title_uid(WiiNand *nand, uint64_t title_id, int allocate);

/* Whether permissions are checked. They are by default; turning it off
 * makes every caller IOS, for the day a game is found to need it. */
void wii_nand_enforce_permissions(WiiNand *nand, int enforce);

/* Each returns zero or an ISFS error. Paths are absolute, at most 63
 * characters, each component at most twelve. A new file or directory
 * belongs to the caller. Deleting a directory deletes what is in it. */
int32_t wii_nand_create_file(WiiNand *nand, const WiiNandCaller *who, const char *path,
                             uint8_t attributes, uint8_t owner, uint8_t group, uint8_t other);
int32_t wii_nand_create_dir(WiiNand *nand, const WiiNandCaller *who, const char *path,
                            uint8_t attributes, uint8_t owner, uint8_t group, uint8_t other);
int32_t wii_nand_delete(WiiNand *nand, const WiiNandCaller *who, const char *path);
/* A file keeps its name and may only change directory; a directory may
 * change both. Something of the same kind already at `to` is replaced. */
int32_t wii_nand_rename(WiiNand *nand, const WiiNandCaller *who, const char *from, const char *to);
int32_t wii_nand_get_attr(WiiNand *nand, const WiiNandCaller *who, const char *path, WiiNandAttr *attr);
int32_t wii_nand_set_attr(WiiNand *nand, const WiiNandCaller *who, const char *path,
                          const WiiNandAttr *attr);
/* The names in a directory, newest first as ISFS lists them: `*count` is
 * how many there are when `names` is NULL, and otherwise how many were
 * written - at most `max`, each in thirteen bytes padded with zeroes. */
int32_t wii_nand_read_dir(WiiNand *nand, const WiiNandCaller *who, const char *path, char *names,
                          uint32_t max, uint32_t *count);
/* What a directory and everything under it uses: clusters, and entries in
 * the file table, itself included. */
int32_t wii_nand_usage(WiiNand *nand, const WiiNandCaller *who, const char *path, uint32_t *clusters,
                       uint32_t *inodes);
void wii_nand_stats(const WiiNand *nand, WiiNandStats *stats);
/* ISFS's own format, which only IOS may ask for: everything goes, and
 * nothing is put back. */
int32_t wii_nand_wipe(WiiNand *nand, const WiiNandCaller *who);
/* What IOS does to /tmp every time it starts: empty it. This does not
 * make the filesystem dirty - nothing under /tmp reaches the host's file. */
void wii_nand_clear_tmp(WiiNand *nand);

/* Open files. A descriptor's own state - its path, the mode it was opened
 * with, its position - is the caller's to keep (the IOS kernel keeps it
 * in its descriptor table, where a save state finds it); the filesystem
 * remembers only that the file is open, so that it cannot be deleted or
 * moved, and how many are.
 *
 * `mode` is WII_NAND_READ, WII_NAND_WRITE or both. A read returns the
 * number of bytes read, which is fewer than asked at the end of the file;
 * a write extends the file; a seek returns the new position, which may not
 * lie past the end. `whence` is 0 from the start, 1 from the position, 2
 * from the end. */
int32_t wii_nand_open(WiiNand *nand, const WiiNandCaller *who, const char *path, uint32_t mode);
int32_t wii_nand_close(WiiNand *nand, const char *path);
int32_t wii_nand_read(WiiNand *nand, const char *path, uint32_t mode, uint32_t *position, void *out,
                      uint32_t size);
int32_t wii_nand_write(WiiNand *nand, const char *path, uint32_t mode, uint32_t *position,
                       const void *in, uint32_t size);
int32_t wii_nand_seek(WiiNand *nand, const char *path, uint32_t *position, int32_t where, int32_t whence);
int32_t wii_nand_length(WiiNand *nand, const char *path, uint32_t *length);
unsigned wii_nand_open_count(const WiiNand *nand);
/* After the descriptor table has been replaced (a reset, a save state):
 * nothing is open, and then each descriptor that is says so again. */
void wii_nand_forget_open(WiiNand *nand);
int32_t wii_nand_mark_open(WiiNand *nand, const char *path);

/* The console's picture shape - SYSCONF's IPL.AR, which a game reads when
 * it starts: 1 for 16:9, 0 for 4:3, or -1 when the flash has no SYSCONF or
 * the file has no such item. Setting it changes that one byte and leaves
 * the rest of the file - the remotes a game has paired among it - as it
 * is, and changes nothing at all when the shape is already the one asked
 * for; 0, or -1 when there is nothing to change. */
int wii_nand_widescreen(WiiNand *nand);
int wii_nand_set_widescreen(WiiNand *nand, int widescreen);

/* A whole file at once, as IOS: for whoever sets a console up (tickets,
 * title metadata) or wants a save out. `put` creates the file if it is not
 * there, owned by IOS and open to all, and leaves an existing file's owner
 * and permissions alone. `get` returns a pointer into the tree that is
 * good until the next call that changes the file. */
int32_t wii_nand_put(WiiNand *nand, const char *path, const void *data, uint32_t size);
int32_t wii_nand_get(WiiNand *nand, const char *path, const uint8_t **data, uint32_t *size);

/* ------------------------------------------------------------------ */
/* One file on the host                                                */

/* Read a filesystem back. NULL when it cannot be, with why in `reason`:
 * and `*missing` set when the reason is only that there is no such file
 * (or an empty one), which is a console that has never been switched on
 * rather than a damaged one. A file that is damaged in any way - short,
 * long, a checksum that does not match, an entry that breaks one of
 * ISFS's rules - is refused whole; nothing is ever half loaded. */
WiiNand *wii_nand_load(const char *path, int *missing, char *reason, size_t reason_size);

/* Write the filesystem to `path`: to `<path>.tmp`, flushed to the disk,
 * then renamed over the file, so that what is on the disk is always one
 * whole filesystem - the old one or the new one - whatever happens part
 * way. What is under /tmp is not written. Zero, and the filesystem is no
 * longer dirty; or -1, the file untouched and the filesystem still dirty. */
int wii_nand_save(WiiNand *nand, const char *path);

/* The filesystem as the bytes wii_nand_save writes, in memory the caller
 * frees - with what is under /tmp as well when `with_tmp` is set, which a
 * host file never has and a save state must. NULL when there is no memory. */
uint8_t *wii_nand_image(const WiiNand *nand, int with_tmp, size_t *size);
/* A filesystem from such bytes, checked as wii_nand_load checks a file; or
 * NULL, with what is wrong in `reason`. */
WiiNand *wii_nand_from_image(const uint8_t *data, size_t size, char *reason, size_t reason_size);
/* The filesystem is not what its host file holds: write it back. */
void wii_nand_mark_changed(WiiNand *nand);

/* Whether anything has changed since the last save, and when it last did,
 * by a clock the owner sets (the machine's cycle count): enough to write
 * the file back once a game has stopped writing rather than after every
 * request. */
int wii_nand_dirty(const WiiNand *nand);
uint64_t wii_nand_changed_at(const WiiNand *nand);
void wii_nand_set_time(WiiNand *nand, uint64_t now);

/* ------------------------------------------------------------------ */
/* A machine's flash (wii_ios_fs.c)                                    */

/* The machine's filesystem. A machine that was given no file gets one in
 * memory the first time anything asks - formatted, with the default
 * settings - and it is never written anywhere: a headless run touches no
 * file it was not handed. NULL on a GameCube. */
WiiNand *wii_nand_of(struct GcMachine *m);

/* Back the machine's flash with `path`: loaded if the file is there,
 * formatted (and written, so the file exists from then on) if it is not.
 * A file that is there and cannot be loaded is left exactly as it is, the
 * reason goes to `log`, and -1 is returned - the caller decides whether to
 * run without saving or not at all. From then on the file is written back
 * about a second after the last change, and by wii_nand_detach. */
int wii_nand_attach(struct GcMachine *m, const char *path, FILE *log);
/* The same flash as wii_nand_attach would give, held in memory and never
 * written back - and no file is made where there is none: for a second
 * machine that must see the saves the first one does without both of them
 * writing the one file (a differential's interpreter), as
 * gc_card_insert_unbacked is for the memory card. */
int wii_nand_attach_unbacked(struct GcMachine *m, const char *path, FILE *log);
/* Write back what has changed, now. Zero when there was nothing to write
 * or it was written. */
int wii_nand_flush(struct GcMachine *m);
/* Write back and let go: for the machine's destruction. */
void wii_nand_detach(struct GcMachine *m);

/* The identity requests to the filesystem are made under: the running
 * title's user number and group. Until the title manager says otherwise
 * that is user 0x1000 and the maker code of the disc in the drive as the
 * group ("01", 0x3031, with no disc). */
void wii_fs_identity(struct GcMachine *m, uint32_t *uid, uint16_t *gid);

/* The same for a machine's flash (made, in memory, if it has none yet):
 * what a frontend's "16:9" setting writes before a game starts, and what
 * it asks to know which shape the game that is running was started in. */
int wii_console_widescreen(struct GcMachine *m);
int wii_console_set_widescreen(struct GcMachine *m, int widescreen);

/* After a save state has replaced the descriptor table: tell the
 * filesystem again which files are open. */
void wii_fs_descriptors_replaced(struct GcMachine *m);

/* The flash in a save state (gc_state.c): the whole filesystem, /tmp
 * included, and what belongs to this boot of IOS. Loading is in two steps
 * as the memory card's is: with `apply` zero the bytes are only checked.
 * Applied, the state's filesystem replaces the machine's, and when that
 * changes a file on the host the file as it was is kept as `<file>.bak`
 * first - a state loaded by mistake costs no save that cannot be copied
 * back. */
struct GcStateBuffer;
void wii_fs_state_save(struct GcMachine *m, struct GcStateBuffer *b);
int wii_fs_state_load(struct GcMachine *m, const uint8_t *payload, size_t size, int apply);

#ifdef __cplusplus
}
#endif

#endif /* WII_NAND_H */
