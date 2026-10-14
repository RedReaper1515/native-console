/*
 * The flash filesystem as IOS serves it: /dev/fs, and every path that is
 * not a device.
 *
 * A game talks to the filesystem two ways. Things done to a *name* - make
 * a file or a directory, delete, rename, list, read and change attributes,
 * ask how full the chip is - are requests to the device /dev/fs. Things
 * done to a file's *contents* are done by opening the file's own path as
 * if it were a device and reading, writing and seeking on the descriptor
 * that comes back. Both are here; the tree they act on is wii_nand.c, and
 * this file is only the request formats and the machine's side of things:
 * whose flash it is, when it is written to the host, and how long each
 * request takes.
 *
 * The request numbers and buffer layouts are WiiBrew's /dev/fs page and
 * libogc's isfs.c (zlib licence; layouts and numbers, no code), which is
 * the client the test programs are built with. The SDK's client
 * (doldecomp/ogws, src/revolution/FS/fs.c) sends the same bytes.
 *
 * A descriptor's state is the kernel's WiiIosFile and nothing else: the
 * path it was opened with, the mode, the position, and whether it has
 * been written through. The tree is found again from the path on every
 * request. That keeps WiiIos plain data, so a save state can carry it, and
 * costs a walk of at most nine names.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_disc.h"
#include "gc_state.h"
#include "wii.h"
#include "wii_nand.h"

extern const WiiIosDevice wii_ios_fs;
extern const WiiIosDevice wii_ios_file;

/* /dev/fs's requests. */
enum {
  FS_FORMAT = 1,
  FS_GET_STATS = 2,
  FS_CREATE_DIR = 3,
  FS_READ_DIR = 4, /* vectored */
  FS_SET_ATTR = 5,
  FS_GET_ATTR = 6,
  FS_DELETE = 7,
  FS_RENAME = 8,
  FS_CREATE_FILE = 9,
  FS_SET_FILE_VERSION_CONTROL = 10,
  FS_GET_FILE_STATS = 11, /* on a file's descriptor, not on /dev/fs */
  FS_GET_USAGE = 12,      /* vectored */
  FS_SHUTDOWN = 13
};

#define FS_PATH 64u
/* The block that names something and its attributes:
 *   +0x00 owner, +0x04 group, +0x06 the path, +0x46 the owner's, the
 *   group's and everyone else's permissions, +0x49 the attribute byte,
 *   and two bytes to make a multiple of four. */
#define FS_ATTR_BLOCK 0x4cu
#define FS_STATS_BLOCK 0x1cu
#define FS_NAME 13u

/* How long the flash takes, in ticks of the timebase (60.75 MHz on a Wii:
 * twelve processor cycles a tick).
 *
 * The three figures are measurements, not guesses, and not ours: the
 * Dolphin project timed these requests on consoles and fitted the cost of
 * a 16 KB cluster read, a cluster written and a superblock rewritten, and
 * publishes the results in its IOS filesystem (GPL; the numbers are facts
 * about the hardware and are all that is taken). They differ a little
 * between IOS versions - 115,000 to 165,000 for a cluster read, 300,000 to
 * 370,000 written, 3.2 to 4.1 million for a superblock - and these are
 * from the middle of each range. In everyday units: about
 * 8 MB a second read, 3 MB a second written, and 55 ms for every change
 * to the file table, which is why a Wii's save screen says not to touch
 * the power for a second or two.
 *
 * The model is simpler than a console in two ways. Time is proportional
 * to the bytes moved, where IOS moves whole clusters and keeps the last
 * one; and the table is charged when a name changes and when a file that
 * was written is closed, where IOS has more cases. Nothing correct
 * depends on either: what matters is that the figures are in machine
 * time, so a run is the same every time, and that a save is not
 * instantaneous. */
#define FS_CLUSTER_READ_TICKS 125000u
#define FS_CLUSTER_WRITE_TICKS 300000u
#define FS_SUPERBLOCK_TICKS 3350000u

static uint64_t read_cycles(uint32_t bytes) {
  return (uint64_t)bytes * FS_CLUSTER_READ_TICKS * GC_CYCLES_PER_TB / WII_NAND_CLUSTER;
}

static uint64_t write_cycles(uint32_t bytes) {
  return (uint64_t)bytes * FS_CLUSTER_WRITE_TICKS * GC_CYCLES_PER_TB / WII_NAND_CLUSTER;
}

/* A change to the file table that took: one superblock rewritten. A
 * request that was refused wrote nothing. */
static int32_t table_changed(WiiIosCall *call, int32_t result) {
  if (result == 0) call->delay += (uint64_t)FS_SUPERBLOCK_TICKS * GC_CYCLES_PER_TB;
  return result;
}

/* ------------------------------------------------------------------ */
/* Whose flash                                                         */

/* What hangs off WiiHost::nand: the tree, the host file behind it if
 * there is one, and what belongs to this boot of IOS. */
typedef struct WiiFsHost {
  WiiNand *nand;
  char *path; /* NULL: in memory only, never written */
  /* After a write-back failed: when to try again, and whether it was said. */
  uint64_t retry_at;
  int write_failed;
  /* The filesystem has been told to shut down (request 13). */
  int shut_down;
  /* How many refusals have been logged: a game denied something it needed
   * is the first thing to look for when it will not save. */
  unsigned refusals;
} WiiFsHost;

static WiiFsHost *host_of(GcMachine *m, int create) {
  WiiState *w = wii_state(m);
  WiiFsHost *host;
  if (!w) return NULL;
  host = (WiiFsHost *)w->host.nand;
  if (host || !create) return host;

  /* Nobody attached a file: a console fresh from the factory, in memory. */
  host = (WiiFsHost *)calloc(1, sizeof(*host));
  if (!host) return NULL;
  host->nand = wii_nand_create();
  if (!host->nand || wii_nand_format(host->nand, NULL) != 0) {
    wii_nand_destroy(host->nand);
    free(host);
    if (m->log) fprintf(m->log, "wii: no memory for the flash filesystem\n");
    return NULL;
  }
  w->host.nand = host;
  return host;
}

WiiNand *wii_nand_of(GcMachine *m) {
  WiiFsHost *host = host_of(m, 1);
  return host ? host->nand : NULL;
}

int wii_console_widescreen(GcMachine *m) { return wii_nand_widescreen(wii_nand_of(m)); }

int wii_console_set_widescreen(GcMachine *m, int widescreen) {
  return wii_nand_set_widescreen(wii_nand_of(m), widescreen);
}

void wii_fs_descriptors_replaced(GcMachine *m) {
  WiiState *w = wii_state(m);
  WiiFsHost *host = host_of(m, 0);
  unsigned i;
  if (!w || !host) return;
  wii_nand_forget_open(host->nand);
  for (i = 0; i < WII_IOS_FILES; i++) {
    const WiiIosFile *file = &w->ios.files[i];
    if (wii_ios_device_of(file) == &wii_ios_file) wii_nand_mark_open(host->nand, file->path);
  }
}

int wii_nand_flush(GcMachine *m) {
  WiiFsHost *host = host_of(m, 0);
  if (!host || !host->path || !wii_nand_dirty(host->nand)) return 0;
  return wii_nand_save(host->nand, host->path);
}

void wii_nand_detach(GcMachine *m) {
  WiiState *w = wii_state(m);
  WiiFsHost *host = host_of(m, 0);
  if (!host) return;
  if (wii_nand_flush(m) != 0) {
    fprintf(stderr, "wii: cannot write the flash filesystem to %s; what changed since it was last written is lost\n",
            host->path);
  }
  wii_nand_destroy(host->nand);
  free(host->path);
  free(host);
  w->host.nand = NULL;
}

static int attach(GcMachine *m, const char *path, FILE *log, int write_back) {
  WiiState *w = wii_state(m);
  WiiFsHost *host;
  char reason[160];
  int missing = 0;
  size_t length;
  if (!w || !path || !path[0]) return -1;
  wii_nand_detach(m);

  host = (WiiFsHost *)calloc(1, sizeof(*host));
  if (!host) return -1;
  length = strlen(path) + 1u;
  host->path = (char *)malloc(length);
  if (!host->path) {
    free(host);
    return -1;
  }
  memcpy(host->path, path, length);

  host->nand = wii_nand_load(path, &missing, reason, sizeof(reason));
  if (!host->nand && missing) {
    /* A console that has never been switched on: format rather than
     * refuse, and write the file at once, so that it exists - and is seen
     * to be writable - before a game is asked to trust it with a save. */
    host->nand = wii_nand_create();
    if (!host->nand || wii_nand_format(host->nand, NULL) != 0 ||
        (write_back && wii_nand_save(host->nand, path) != 0)) {
      if (log) fprintf(log, "wii: cannot create the flash filesystem %s\n", path);
      wii_nand_destroy(host->nand);
      host->nand = NULL;
    }
  } else if (!host->nand) {
    /* There is a file and it is not a filesystem. Overwriting it would be
     * wrong - it may be somebody's saves, damaged, or somebody's something
     * else - so it is left exactly as it is. */
    if (log) fprintf(log, "wii: %s was not loaded and has not been touched: %s\n", path, reason);
  }
  if (!host->nand) {
    free(host->path);
    free(host);
    return -1;
  }
  if (!write_back) {
    /* The same flash, in memory only: nothing is ever written. */
    free(host->path);
    host->path = NULL;
  }
  w->host.nand = host;
  wii_nand_set_time(host->nand, m->cpu.cycles);
  wii_fs_descriptors_replaced(m);
  return 0;
}

int wii_nand_attach(GcMachine *m, const char *path, FILE *log) { return attach(m, path, log, 1); }

/* The section: a word of what this boot of IOS has been told (bit 0: shut
 * down), a word kept for later, then the filesystem's image. */
#define FS_STATE_HEAD 8u

void wii_fs_state_save(GcMachine *m, GcStateBuffer *b) {
  WiiFsHost *host = host_of(m, 1);
  size_t size = 0;
  uint8_t *image = host ? wii_nand_image(host->nand, 1, &size) : NULL;
  if (!image) {
    b->failed = 1;
    return;
  }
  gc_state_put_u32(b, host->shut_down ? 1u : 0u);
  gc_state_put_u32(b, 0u);
  gc_state_put(b, image, size);
  free(image);
}

int wii_fs_state_load(GcMachine *m, const uint8_t *payload, size_t size, int apply) {
  WiiFsHost *host;
  WiiNand *nand;
  uint32_t flags;
  char reason[160];
  if (!wii_state(m) || size < FS_STATE_HEAD) return -1;
  memcpy(&flags, payload, sizeof(flags));
  if (flags > 1u) return -1;
  nand = wii_nand_from_image(payload + FS_STATE_HEAD, size - FS_STATE_HEAD, reason, sizeof(reason));
  if (!nand) return -1;
  if (!apply) {
    wii_nand_destroy(nand);
    return 0;
  }
  host = host_of(m, 1);
  if (!host) {
    wii_nand_destroy(nand);
    return -1;
  }
  if (host->path) {
    /* Is the file on the host about to change? What it holds is the
     * filesystem without /tmp, so that is what is compared. */
    size_t was_size = 0, now_size = 0;
    uint8_t *was = wii_nand_image(host->nand, 0, &was_size);
    uint8_t *now = wii_nand_image(nand, 0, &now_size);
    const int same = was && now && was_size == now_size && memcmp(was, now, was_size) == 0;
    free(was);
    free(now);
    if (!same) {
      const size_t length = strlen(host->path);
      char *backup = (char *)malloc(length + 5u);
      /* Putting the state's flash back throws away every save made since
       * the state: right for the game, whose memory holds the state's
       * files, and hard on whoever made them. */
      if (backup) {
        memcpy(backup, host->path, length);
        memcpy(backup + length, ".bak", 5u);
      }
      if (!backup || wii_nand_save(host->nand, backup) != 0) {
        fprintf(stderr, "wii: cannot keep the flash filesystem as it was in %s.bak\n", host->path);
      }
      free(backup);
      wii_nand_mark_changed(nand);
    }
  }
  wii_nand_destroy(host->nand);
  host->nand = nand;
  host->shut_down = (int)(flags & 1u);
  host->retry_at = 0;
  wii_nand_set_time(nand, m->cpu.cycles);
  wii_fs_descriptors_replaced(m);
  return 0;
}

int wii_nand_attach_unbacked(GcMachine *m, const char *path, FILE *log) { return attach(m, path, log, 0); }

void wii_fs_identity(GcMachine *m, uint32_t *uid, uint16_t *gid) {
  const WiiState *w = wii_state(m);
  WiiFsHost *host = host_of(m, 0);
  uint32_t user = 0x1000u;
  uint16_t group = 0x3031u; /* "01" */
  /* A disc's id is four characters of game and two of maker, and the
   * maker's two are the group of everything the game owns. */
  if (m->disc && m->disc->game_code[4] && m->disc->game_code[5]) {
    group = (uint16_t)(((uint16_t)(uint8_t)m->disc->game_code[4] << 8) | (uint8_t)m->disc->game_code[5]);
  }
  /* Once the title manager has given the running title a user number
   * (wii_nand_add_title), that is who it is. */
  if (w && w->title_id && host) {
    const uint32_t given = wii_nand_title_uid(host->nand, w->title_id, 0);
    if (given) user = given;
  }
  if (uid) *uid = user;
  if (gid) *gid = group;
}

/* Once a field: write the file back when a game has stopped writing.
 *
 * A save is a burst of requests - the SDK alone makes a directory, a
 * file, a dozen writes, a rename and a delete for one - and writing the
 * whole filesystem to the host after each would be megabytes of file I/O
 * a save. A second without a change is the burst over. */
static void fs_field(GcMachine *m) {
  WiiFsHost *host = host_of(m, 0);
  if (!host) return;
  wii_nand_set_time(host->nand, m->cpu.cycles);
  if (!host->path || !wii_nand_dirty(host->nand)) return;
  if (m->cpu.cycles - wii_nand_changed_at(host->nand) < gc_cpu_hz) return;
  if (m->cpu.cycles < host->retry_at) return;
  if (wii_nand_save(host->nand, host->path) != 0) {
    /* Somebody else has the file open, or the disk is full. Say so once
     * and try again in a second; the filesystem stays in memory and is
     * written the moment it can be. */
    if (!host->write_failed) {
      fprintf(stderr, "wii: cannot write the flash filesystem to %s; will keep trying\n", host->path);
    }
    host->write_failed = 1;
    host->retry_at = m->cpu.cycles + gc_cpu_hz;
  } else if (host->write_failed) {
    fprintf(stderr, "wii: the flash filesystem is written to %s\n", host->path);
    host->write_failed = 0;
  }
}

/* A reset, or IOS reloaded: what belonged to that boot goes. The flash
 * itself does not - a console's saves survive its reset button - except
 * for /tmp, which IOS empties every time it starts. */
static void fs_reset(GcMachine *m) {
  WiiFsHost *host = host_of(m, 0);
  if (!host) return;
  wii_nand_set_time(host->nand, m->cpu.cycles);
  wii_nand_forget_open(host->nand);
  wii_nand_clear_tmp(host->nand);
  host->shut_down = 0;
  host->retry_at = 0;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */

typedef struct Fs {
  WiiFsHost *host;
  WiiNand *nand;
  WiiNandCaller who;
} Fs;

/* What every request starts with. Zero, or the error to answer with. */
static int32_t fs_begin(WiiIosCall *call, Fs *fs) {
  fs->host = host_of(call->m, 1);
  if (!fs->host) return WII_IPC_ENOMEM;
  fs->nand = fs->host->nand;
  wii_nand_set_time(fs->nand, call->m->cpu.cycles);
  wii_fs_identity(call->m, &fs->who.uid, &fs->who.gid);
  return fs->host->shut_down ? WII_FS_ESHUTDOWN : 0;
}

static int32_t refused(WiiIosCall *call, Fs *fs, const char *what, const char *path, int32_t result) {
  if (result == WII_FS_EACCESS && call->m->log && fs->host->refusals < 16u) {
    fs->host->refusals++;
    fprintf(call->m->log, "wii: the filesystem refused user %x group %04x: %s %s\n", (unsigned)fs->who.uid,
            (unsigned)fs->who.gid, what, path);
  }
  return result;
}

static int32_t unknown(WiiIosCall *call, uint32_t command) {
  WiiIos *ios = &wii_state(call->m)->ios;
  ios->unknown_commands++;
  if (call->m->log && ios->unknown_commands <= 32) {
    fprintf(call->m->log, "wii: the filesystem has no request %u (on %s), pc %08x\n", (unsigned)command,
            call->file->path, call->m->cpu.pc);
  }
  return WII_FS_EINVAL;
}

/* A path out of a 64-byte field: it has to end inside it. */
static int path_from(const uint8_t *field, char path[FS_PATH]) {
  if (!field || !memchr(field, 0, FS_PATH)) return -1;
  memset(path, 0, FS_PATH);
  memcpy(path, field, strlen((const char *)field));
  return 0;
}

/* ISFS allows sixteen descriptors at once, /dev/fs's own among them. The
 * kernel has already chosen a slot for the one being opened and not yet
 * marked it open, so this counts the others. */
static unsigned descriptors(GcMachine *m) {
  const WiiIos *ios = &wii_state(m)->ios;
  unsigned i, count = 0;
  for (i = 0; i < WII_IOS_FILES; i++) {
    const WiiIosDevice *device = wii_ios_device_of(&ios->files[i]);
    if (device == &wii_ios_fs || device == &wii_ios_file) count++;
  }
  return count;
}

static int32_t fs_open(WiiIosCall *call, const char *path, uint32_t mode) {
  Fs fs;
  const int32_t result = fs_begin(call, &fs);
  (void)path;
  (void)mode;
  if (result) return result;
  if (descriptors(call->m) >= WII_NAND_OPEN_MAX) return WII_FS_EFDEXHAUSTED;
  return 0;
}

static int32_t fs_ioctl(WiiIosCall *call, uint32_t command, uint8_t *in, uint32_t in_size, uint8_t *out,
                        uint32_t out_size) {
  Fs fs;
  char path[FS_PATH], other[FS_PATH];
  int32_t result = fs_begin(call, &fs);
  if (result) return result;

  switch (command) {
    case FS_FORMAT:
      /* Only IOS may. A game that asks is told so, and nothing happens. */
      return table_changed(call, refused(call, &fs, "format", "the flash", wii_nand_wipe(fs.nand, &fs.who)));

    case FS_GET_STATS: {
      WiiNandStats stats;
      if (!out || out_size < FS_STATS_BLOCK) return WII_FS_EINVAL;
      wii_nand_stats(fs.nand, &stats);
      wii_put32(out + 0x00, stats.cluster_size);
      wii_put32(out + 0x04, stats.free_clusters);
      wii_put32(out + 0x08, stats.used_clusters);
      wii_put32(out + 0x0c, stats.bad_clusters);
      wii_put32(out + 0x10, stats.reserved_clusters);
      wii_put32(out + 0x14, stats.free_inodes);
      wii_put32(out + 0x18, stats.used_inodes);
      return 0;
    }

    case FS_CREATE_DIR:
    case FS_CREATE_FILE:
      /* The owner and group in the block are not the caller's to choose -
       * neither client fills them in - and a new entry is the caller's. */
      if (!in || in_size < FS_ATTR_BLOCK || path_from(in + 6, path) != 0) return WII_FS_EINVAL;
      if (command == FS_CREATE_DIR) {
        result = wii_nand_create_dir(fs.nand, &fs.who, path, in[0x49], in[0x46], in[0x47], in[0x48]);
      } else {
        result = wii_nand_create_file(fs.nand, &fs.who, path, in[0x49], in[0x46], in[0x47], in[0x48]);
      }
      return table_changed(call, refused(call, &fs, "create", path, result));

    case FS_SET_ATTR: {
      WiiNandAttr attr;
      if (!in || in_size < FS_ATTR_BLOCK || path_from(in + 6, path) != 0) return WII_FS_EINVAL;
      memset(&attr, 0, sizeof(attr));
      attr.uid = wii_be32(in);
      attr.gid = wii_be16(in + 4);
      attr.owner = in[0x46];
      attr.group = in[0x47];
      attr.other = in[0x48];
      attr.attributes = in[0x49];
      result = wii_nand_set_attr(fs.nand, &fs.who, path, &attr);
      return table_changed(call, refused(call, &fs, "change the attributes of", path, result));
    }

    case FS_GET_ATTR: {
      WiiNandAttr attr;
      if (!in || in_size < FS_PATH || path_from(in, path) != 0) return WII_FS_EINVAL;
      if (!out || out_size < FS_ATTR_BLOCK) return WII_FS_EINVAL;
      result = wii_nand_get_attr(fs.nand, &fs.who, path, &attr);
      if (result) return refused(call, &fs, "read the attributes of", path, result);
      /* The owner, the group, the permissions and the attribute byte, and
       * nothing else: IOS leaves the path's sixty-four bytes and the
       * padding as it found them, and so does this. */
      wii_put32(out, attr.uid);
      wii_put16(out + 4, attr.gid);
      out[0x46] = attr.owner;
      out[0x47] = attr.group;
      out[0x48] = attr.other;
      out[0x49] = attr.attributes;
      return 0;
    }

    case FS_DELETE:
      if (!in || in_size < FS_PATH || path_from(in, path) != 0) return WII_FS_EINVAL;
      return table_changed(call, refused(call, &fs, "delete", path, wii_nand_delete(fs.nand, &fs.who, path)));

    case FS_RENAME:
      if (!in || in_size < 2u * FS_PATH || path_from(in, path) != 0 || path_from(in + FS_PATH, other) != 0) {
        return WII_FS_EINVAL;
      }
      return table_changed(call, refused(call, &fs, "rename", path, wii_nand_rename(fs.nand, &fs.who, path, other)));

    case FS_SET_FILE_VERSION_CONTROL: {
      /* Takes the same block as the attribute requests and, as far as
       * anything public says, changes nothing a title can observe: it is
       * answered as done for something that exists. */
      WiiNandAttr attr;
      if (!in || in_size < FS_ATTR_BLOCK || path_from(in + 6, path) != 0) return WII_FS_EINVAL;
      return refused(call, &fs, "set the version control of", path, wii_nand_get_attr(fs.nand, &fs.who, path, &attr));
    }

    case FS_SHUTDOWN:
      /* The console is about to lose power: everything goes to the host
       * now, and nothing more is accepted until IOS starts again. */
      if (wii_nand_flush(call->m) != 0 && fs.host->path) {
        fprintf(stderr, "wii: cannot write the flash filesystem to %s\n", fs.host->path);
      }
      fs.host->shut_down = 1;
      return 0;

    case FS_GET_FILE_STATS:
      /* A file's, asked of the file. */
      return WII_FS_EINVAL;

    default: break;
  }
  return unknown(call, command);
}

static int32_t fs_ioctlv(WiiIosCall *call, uint32_t command, unsigned in_count, unsigned io_count,
                         const WiiIosVector *v) {
  GcMachine *m = call->m;
  Fs fs;
  char path[FS_PATH];
  int32_t result = fs_begin(call, &fs);
  if (result) return result;

  switch (command) {
    case FS_READ_DIR: {
      /* Two forms. One buffer each way: how many entries are there. Two
       * each way: the path and how many names there is room for, then the
       * names, thirteen bytes each, and how many were written. */
      uint8_t *count_out;
      uint32_t count = 0;
      if (in_count < 1u || v[0].size != FS_PATH || path_from(wii_ios_buffer(m, &v[0]), path) != 0) {
        return WII_FS_EINVAL;
      }
      if (in_count == 1u && io_count == 1u) {
        count_out = wii_ios_buffer(m, &v[1]);
        if (!count_out || v[1].size != 4u) return WII_FS_EINVAL;
        result = wii_nand_read_dir(fs.nand, &fs.who, path, NULL, 0, &count);
      } else if (in_count == 2u && io_count == 2u) {
        const uint8_t *max_in = wii_ios_buffer(m, &v[1]);
        uint8_t *names = wii_ios_buffer(m, &v[2]);
        uint32_t max;
        count_out = wii_ios_buffer(m, &v[3]);
        if (!max_in || v[1].size != 4u || !count_out || v[3].size != 4u) return WII_FS_EINVAL;
        /* Read before anything is written: both clients pass the same
         * word as the limit and as the place for the count. */
        max = wii_be32(max_in);
        /* Room for exactly as many names as the limit says. */
        if (max > 0xffffffffu / FS_NAME || v[2].size != max * FS_NAME || (max && !names)) return WII_FS_EINVAL;
        result = wii_nand_read_dir(fs.nand, &fs.who, path, (char *)names, max, &count);
        if (result == 0 && names) {
          /* The names go back end to end, each closed by its zero - "aa",
           * zero, "bb", zero - and not in thirteen-byte slots, which is
           * only the room a caller has to leave for each. devkitPro's own
           * ISFS example walks them that way, and so does a second
           * implementation (tools/wii/oracle_dolphin.py, which is how the
           * slots this used to write were caught). The rest of the buffer
           * is cleared so that what is in it never depends on what was. */
          uint32_t i, used = 0;
          for (i = 0; i < count; i++) {
            const uint8_t *slot = names + i * FS_NAME;
            const uint32_t length = (uint32_t)strlen((const char *)slot) + 1u;
            memmove(names + used, slot, length);
            used += length;
          }
          memset(names + used, 0, max * FS_NAME - used);
        }
      } else {
        return WII_FS_EINVAL;
      }
      /* "No access" here is how the SDK learns that a path is a directory
       * it may not list, so it is not worth a log line. */
      if (result) return result;
      wii_put32(count_out, count);
      return 0;
    }

    case FS_GET_USAGE: {
      uint8_t *clusters_out, *inodes_out;
      uint32_t clusters = 0, inodes = 0;
      if (in_count != 1u || io_count != 2u || v[0].size != FS_PATH ||
          path_from(wii_ios_buffer(m, &v[0]), path) != 0) {
        return WII_FS_EINVAL;
      }
      clusters_out = wii_ios_buffer(m, &v[1]);
      inodes_out = wii_ios_buffer(m, &v[2]);
      if (!clusters_out || v[1].size != 4u || !inodes_out || v[2].size != 4u) return WII_FS_EINVAL;
      result = wii_nand_usage(fs.nand, &fs.who, path, &clusters, &inodes);
      if (result) return result;
      wii_put32(clusters_out, clusters);
      wii_put32(inodes_out, inodes);
      return 0;
    }

    default: break;
  }
  return unknown(call, command);
}

/* ------------------------------------------------------------------ */
/* Files                                                               */

static int32_t file_open(WiiIosCall *call, const char *path, uint32_t mode) {
  Fs fs;
  int32_t result = fs_begin(call, &fs);
  if (result) return result;
  {
    /* The kernel keeps 63 characters of a path. One that was longer is
     * not the file its first 63 characters name: look at what the game
     * actually passed. */
    const uint8_t *block = wii_ios_memory(call->m, call->request, 0x20);
    const uint8_t *last = block ? wii_ios_memory(call->m, wii_be32(block + 0x0c) + WII_NAND_PATH_MAX, 1) : NULL;
    if (strlen(path) == WII_NAND_PATH_MAX && (!last || *last)) return WII_FS_EINVAL;
  }
  if (descriptors(call->m) >= WII_NAND_OPEN_MAX) return WII_FS_EFDEXHAUSTED;
  /* A mode is two bits, read and write, and IOS looks at no others. */
  mode &= WII_NAND_READ | WII_NAND_WRITE;
  call->file->mode = mode;
  /* A file that is not there is the commonest answer of all - every game
   * looks for its save before it has one - and is said quietly. */
  return refused(call, &fs, "open", path, wii_nand_open(fs.nand, &fs.who, path, mode));
}

/* What a descriptor keeps beside its path, mode and position
 * (WiiIosFile::user): whether it has been written through, because the
 * file's new length reaches the file table when it is closed. */
#define FILE_WRITTEN 0

static int32_t file_close(WiiIosCall *call) {
  WiiFsHost *host = host_of(call->m, 0);
  /* Even after a shutdown: a descriptor can always be given back. */
  if (host) wii_nand_close(host->nand, call->file->path);
  if (call->file->user[FILE_WRITTEN]) table_changed(call, 0);
  return 0;
}

/* /dev/fs itself has no contents to read, write or seek in. */
static int32_t fs_read(WiiIosCall *call, uint8_t *data, uint32_t size) {
  (void)call;
  (void)data;
  (void)size;
  return WII_FS_EINVAL;
}

static int32_t fs_write(WiiIosCall *call, const uint8_t *data, uint32_t size) {
  (void)call;
  (void)data;
  (void)size;
  return WII_FS_EINVAL;
}

static int32_t fs_seek(WiiIosCall *call, int32_t where, int32_t whence) {
  (void)call;
  (void)where;
  (void)whence;
  return WII_FS_EINVAL;
}

/* `data` is guest memory. The tree's bytes are the host's, and they move
 * between the two with memcpy and nothing else: guest pages can be
 * write-protected by the graphics thread, which a copy in user mode
 * survives and a read() straight into them from a file would not. */
static int32_t file_read(WiiIosCall *call, uint8_t *data, uint32_t size) {
  Fs fs;
  int32_t result = fs_begin(call, &fs);
  if (result) return result;
  if (size && !data) return WII_FS_EINVAL;
  result = wii_nand_read(fs.nand, call->file->path, call->file->mode, &call->file->position, data, size);
  if (result > 0) call->delay += read_cycles((uint32_t)result);
  return result;
}

static int32_t file_write(WiiIosCall *call, const uint8_t *data, uint32_t size) {
  Fs fs;
  int32_t result = fs_begin(call, &fs);
  if (result) return result;
  if (size && !data) return WII_FS_EINVAL;
  result = wii_nand_write(fs.nand, call->file->path, call->file->mode, &call->file->position, data, size);
  if (result > 0) {
    call->delay += write_cycles((uint32_t)result);
    call->file->user[FILE_WRITTEN] = 1;
  }
  return result;
}

static int32_t file_seek(WiiIosCall *call, int32_t where, int32_t whence) {
  Fs fs;
  const int32_t result = fs_begin(call, &fs);
  if (result) return result;
  return wii_nand_seek(fs.nand, call->file->path, &call->file->position, where, whence);
}

static int32_t file_ioctl(WiiIosCall *call, uint32_t command, uint8_t *in, uint32_t in_size, uint8_t *out,
                          uint32_t out_size) {
  Fs fs;
  uint32_t length = 0;
  int32_t result = fs_begin(call, &fs);
  (void)in;
  (void)in_size;
  if (result) return result;
  if (command != FS_GET_FILE_STATS) return unknown(call, command);
  /* The file's length and the descriptor's position. */
  if (!out || out_size < 8u) return WII_FS_EINVAL;
  result = wii_nand_length(fs.nand, call->file->path, &length);
  if (result) return result;
  wii_put32(out, length);
  wii_put32(out + 4, call->file->position);
  return 0;
}

const WiiIosDevice wii_ios_fs = {
    "/dev/fs", 0, fs_open, NULL, fs_read, fs_write, fs_seek, fs_ioctl, fs_ioctlv, fs_field, fs_reset,
};

const WiiIosDevice wii_ios_file = {
    "/", 1, file_open, file_close, file_read, file_write, file_seek, file_ioctl, NULL, NULL, NULL,
};
