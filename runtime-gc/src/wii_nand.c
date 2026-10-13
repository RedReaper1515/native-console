/*
 * The flash filesystem's contents (wii_nand.h): a tree in memory with
 * ISFS's rules, and one file on the host to keep it in.
 *
 * Where the rules come from. The limits (twelve-character names, 64-byte
 * path buffers, eight directories deep, sixteen descriptors, 16 KB
 * clusters on a 512 MB chip) and the error numbers are on WiiBrew's
 * /dev/fs and Hardware/NAND pages, and the numbers have names in the SDK's
 * IPC client. What ISFS does in the cases a specification would leave open
 * - which check comes first, what deleting an open file says, that
 * renaming replaces what is at the destination, that a file cannot change
 * its name when it moves, that a directory lists its newest entry first,
 * that the file table has 6,143 entries and the filesystem keeps 0x300
 * clusters back - was established on consoles by the Dolphin project,
 * whose filesystem is the behaviour reference here; it is GPL and none of
 * its code or text is in this file. The SDK's NAND library
 * (doldecomp/ogws) is the other check: what a retail game does with each
 * answer is in the comments below where it decided something.
 *
 * Nothing here knows about IOS or the machine. wii_ios_fs.c is the device
 * that turns requests into these calls.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "wii_nand.h"

typedef struct Node Node;
struct Node {
  Node *parent;
  Node *child; /* the newest entry of a directory */
  Node *next;  /* the entry made before this one */
  char name[WII_NAND_NAME_MAX + 1u];
  uint8_t directory;
  uint8_t attributes;
  uint8_t perm[3]; /* owner, group, other */
  uint16_t gid;
  uint32_t uid;
  uint8_t *data;
  uint32_t size;
  uint32_t capacity;
  uint32_t opens; /* descriptors on this file */
};

struct WiiNand {
  Node *root;
  uint32_t inodes;   /* entries, the root among them */
  uint32_t clusters; /* in use by files */
  unsigned open_files;
  int enforce;
  int dirty;
  uint64_t now;
  uint64_t changed;
};

static const WiiNandCaller ios_itself = {0, 0};

#define USABLE_CLUSTERS WII_NAND_USABLE_CLUSTERS

static uint32_t clusters_of(uint32_t size) {
  return size / WII_NAND_CLUSTER + (size % WII_NAND_CLUSTER ? 1u : 0u);
}

static void touch(WiiNand *nand) {
  nand->dirty = 1;
  nand->changed = nand->now;
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */

/* Whether a path is one ISFS would look at, and how many names are in it
 * (none for the root). A name longer than a table entry can hold is a
 * malformed path, not a missing file. */
static int32_t path_check(const char *path, unsigned *components) {
  size_t length, i, name = 0;
  unsigned count = 0;
  if (!path || path[0] != '/') return WII_FS_EINVAL;
  for (length = 0; path[length]; length++) {
    if (length >= WII_NAND_PATH_MAX) return WII_FS_EINVAL;
  }
  if (length > 1u) {
    if (path[length - 1u] == '/') return WII_FS_EINVAL;
    for (i = 1; i <= length; i++) {
      const unsigned char c = (unsigned char)path[i];
      if (c == '/' || c == 0) {
        if (name == 0 || name > WII_NAND_NAME_MAX) return WII_FS_EINVAL;
        count++;
        name = 0;
      } else {
        if (c < 0x20u || c > 0x7eu) return WII_FS_EINVAL;
        name++;
      }
    }
  }
  if (components) *components = count;
  return 0;
}

static Node *child_named(const Node *directory, const char *name, size_t length) {
  Node *node;
  for (node = directory->child; node; node = node->next) {
    if (strlen(node->name) == length && memcmp(node->name, name, length) == 0) return node;
  }
  return NULL;
}

/* The entry at the first `length` characters of a checked path. */
static int32_t walk(const WiiNand *nand, const char *path, size_t length, Node **out) {
  Node *node = nand->root;
  size_t at = 1;
  while (at < length) {
    size_t end = at;
    while (end < length && path[end] != '/') end++;
    if (!node->directory) return WII_FS_ENOENT;
    node = child_named(node, path + at, end - at);
    if (!node) return WII_FS_ENOENT;
    at = end + 1u;
  }
  *out = node;
  return 0;
}

static int32_t lookup(const WiiNand *nand, const char *path, Node **out) {
  return walk(nand, path, strlen(path), out);
}

/* The directory a checked, non-root path's last name is in, and that name. */
static int32_t lookup_parent(const WiiNand *nand, const char *path, Node **parent, const char **name) {
  const char *slash = strrchr(path, '/');
  const int32_t result = walk(nand, path, (size_t)(slash - path), parent);
  if (result) return result;
  if (!(*parent)->directory) return WII_FS_ENOENT;
  *name = slash + 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Permissions                                                         */

/* Every permission check there is goes through here, so that there is one
 * place to relax them. IOS itself is user zero and may do anything; anyone
 * else gets the owner's bits for something they own, the group's for
 * something their group owns, and the rest's otherwise.
 *
 * What is checked, and it is all that is: opening a file needs the bits
 * of the mode it is opened with; creating, deleting and moving need write
 * on the directory the name is in (both, for a move); listing a directory
 * needs read on it; asking for something's attributes needs read on the
 * directory it is in; changing them needs to be its owner, and changing
 * its owner needs to be IOS. Directories on the way to a path are not
 * checked, and neither is asking how much a directory uses - the SDK's
 * free-space check asks that of /ticket, which no game can read. */
static int allowed(const WiiNand *nand, const Node *node, const WiiNandCaller *who, unsigned want) {
  unsigned have;
  if (!nand->enforce || who->uid == 0) return 1;
  if (who->uid == node->uid) {
    have = node->perm[0];
  } else if (who->gid == node->gid) {
    have = node->perm[1];
  } else {
    have = node->perm[2];
  }
  return (have & want) == want;
}

void wii_nand_enforce_permissions(WiiNand *nand, int enforce) {
  if (nand) nand->enforce = enforce != 0;
}

/* ------------------------------------------------------------------ */
/* Entries                                                             */

static Node *node_new(const char *name, size_t length, int directory) {
  Node *node = (Node *)calloc(1, sizeof(Node));
  if (!node) return NULL;
  memcpy(node->name, name, length);
  node->directory = directory ? 1u : 0u;
  return node;
}

/* A new entry goes at the front of its directory's list, which is the
 * order ISFS lists a directory in. */
static void link_first(Node *parent, Node *node) {
  node->parent = parent;
  node->next = parent->child;
  parent->child = node;
}

static void link_last(Node *parent, Node *node) {
  Node **slot = &parent->child;
  while (*slot) slot = &(*slot)->next;
  node->parent = parent;
  node->next = NULL;
  *slot = node;
}

static void unlink_node(Node *node) {
  Node **slot = &node->parent->child;
  while (*slot != node) slot = &(*slot)->next;
  *slot = node->next;
  node->parent = NULL;
  node->next = NULL;
}

/* Free an entry and everything under it, keeping the filesystem's counts
 * right. The tree is at most nine entries deep, so recursion is safe. */
static void node_free(WiiNand *nand, Node *node) {
  while (node->child) {
    Node *child = node->child;
    node->child = child->next;
    node_free(nand, child);
  }
  nand->inodes--;
  nand->clusters -= clusters_of(node->size);
  nand->open_files -= node->opens;
  free(node->data);
  free(node);
}

static int in_use(const Node *node) {
  const Node *child;
  if (node->opens) return 1;
  for (child = node->child; child; child = child->next) {
    if (in_use(child)) return 1;
  }
  return 0;
}

/* How many directories deep an entry is: the root is zero. */
static unsigned depth_of(const Node *node) {
  unsigned depth = 0;
  for (; node->parent; node = node->parent) depth++;
  return depth;
}

/* How many levels of directory lie below a directory. */
static unsigned height_of(const Node *node) {
  const Node *child;
  unsigned height = 0;
  for (child = node->child; child; child = child->next) {
    if (child->directory) {
      const unsigned below = 1u + height_of(child);
      if (below > height) height = below;
    }
  }
  return height;
}

static void usage_of(const Node *node, uint32_t *clusters, uint32_t *inodes) {
  const Node *child;
  *inodes += 1u;
  *clusters += clusters_of(node->size);
  for (child = node->child; child; child = child->next) usage_of(child, clusters, inodes);
}

/* Replace a file's contents. */
static int32_t set_contents(WiiNand *nand, Node *node, const void *data, uint32_t size) {
  const uint32_t before = clusters_of(node->size), after = clusters_of(size);
  if (nand->clusters - before + after > USABLE_CLUSTERS) return WII_FS_EFBIG;
  if (size > node->capacity) {
    uint8_t *grown = (uint8_t *)realloc(node->data, size);
    if (!grown) return WII_FS_EFBIG;
    node->data = grown;
    node->capacity = size;
  }
  if (size) memcpy(node->data, data, size);
  node->size = size;
  nand->clusters = nand->clusters - before + after;
  touch(nand);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Making and unmaking a filesystem                                    */

WiiNand *wii_nand_create(void) {
  WiiNand *nand = (WiiNand *)calloc(1, sizeof(WiiNand));
  if (!nand) return NULL;
  nand->root = node_new("", 0, 1);
  if (!nand->root) {
    free(nand);
    return NULL;
  }
  /* The root is IOS's: a title can neither list it nor make anything in
   * it. */
  nand->root->perm[0] = WII_NAND_READ | WII_NAND_WRITE;
  nand->root->perm[1] = WII_NAND_READ | WII_NAND_WRITE;
  nand->inodes = 1;
  nand->enforce = 1;
  return nand;
}

void wii_nand_destroy(WiiNand *nand) {
  if (!nand) return;
  node_free(nand, nand->root);
  free(nand);
}

static void empty(WiiNand *nand, Node *directory) {
  while (directory->child) {
    Node *child = directory->child;
    directory->child = child->next;
    node_free(nand, child);
  }
}

int32_t wii_nand_wipe(WiiNand *nand, const WiiNandCaller *who) {
  if (nand->enforce && who->uid != 0) return WII_FS_EACCESS;
  empty(nand, nand->root);
  touch(nand);
  return 0;
}

void wii_nand_clear_tmp(WiiNand *nand) {
  Node *tmp;
  if (!nand) return;
  tmp = child_named(nand->root, "tmp", 3);
  /* Not a change as far as the host's file goes: what is under /tmp is
   * never written to it (wii_nand_save), so emptying /tmp leaves nothing
   * to write back. */
  if (tmp && tmp->directory) empty(nand, tmp);
}

/* ------------------------------------------------------------------ */
/* The operations                                                      */

static int32_t create(WiiNand *nand, const WiiNandCaller *who, const char *path, int directory,
                      uint8_t attributes, uint8_t owner, uint8_t group, uint8_t other) {
  unsigned components = 0;
  Node *parent, *node;
  const char *name;
  int32_t result = path_check(path, &components);
  if (result) return result;
  if (components == 0) return WII_FS_EINVAL;
  if (directory && components > WII_NAND_DEPTH_MAX) return WII_FS_EDIRDEPTH;
  result = lookup_parent(nand, path, &parent, &name);
  if (result) return result;
  if (!allowed(nand, parent, who, WII_NAND_WRITE)) return WII_FS_EACCESS;
  if (child_named(parent, name, strlen(name))) return WII_FS_EEXIST;
  if (nand->inodes >= WII_NAND_INODES) return WII_FS_ENFILE;
  node = node_new(name, strlen(name), directory);
  /* The host running out of memory is the chip running out of room. */
  if (!node) return WII_FS_ENFILE;
  node->uid = who->uid;
  node->gid = who->gid;
  node->attributes = attributes;
  node->perm[0] = owner & 3u;
  node->perm[1] = group & 3u;
  node->perm[2] = other & 3u;
  link_first(parent, node);
  nand->inodes++;
  touch(nand);
  return 0;
}

int32_t wii_nand_create_file(WiiNand *nand, const WiiNandCaller *who, const char *path,
                             uint8_t attributes, uint8_t owner, uint8_t group, uint8_t other) {
  return create(nand, who, path, 0, attributes, owner, group, other);
}

int32_t wii_nand_create_dir(WiiNand *nand, const WiiNandCaller *who, const char *path,
                            uint8_t attributes, uint8_t owner, uint8_t group, uint8_t other) {
  return create(nand, who, path, 1, attributes, owner, group, other);
}

int32_t wii_nand_delete(WiiNand *nand, const WiiNandCaller *who, const char *path) {
  unsigned components = 0;
  Node *parent, *node;
  const char *name;
  int32_t result = path_check(path, &components);
  if (result) return result;
  if (components == 0) return WII_FS_EINVAL;
  result = lookup_parent(nand, path, &parent, &name);
  if (result) return result;
  if (!allowed(nand, parent, who, WII_NAND_WRITE)) return WII_FS_EACCESS;
  node = child_named(parent, name, strlen(name));
  if (!node) return WII_FS_ENOENT;
  /* An open file, or a directory with one anywhere inside it. */
  if (in_use(node)) return WII_FS_EFDOPEN;
  unlink_node(node);
  node_free(nand, node);
  touch(nand);
  return 0;
}

/* Renaming is how the SDK finishes every save: it writes the new file
 * under /tmp/sys/<number>/<name> and moves it over the old one, so that a
 * console switched off mid-save keeps the old save whole. Two of ISFS's
 * rules exist for exactly that and both matter: the destination is
 * replaced if it is there, and a file keeps its name - only the directory
 * changes (which is why the SDK has a function to move a file and none to
 * rename one). */
int32_t wii_nand_rename(WiiNand *nand, const WiiNandCaller *who, const char *from, const char *to) {
  unsigned from_components = 0, to_components = 0;
  Node *from_parent, *to_parent, *node, *existing, *up;
  const char *from_name, *to_name;
  int32_t result;
  if (path_check(from, &from_components) || path_check(to, &to_components)) return WII_FS_EINVAL;
  if (from_components == 0 || to_components == 0) return WII_FS_EINVAL;
  result = lookup_parent(nand, from, &from_parent, &from_name);
  if (result) return result;
  result = lookup_parent(nand, to, &to_parent, &to_name);
  if (result) return result;
  if (!allowed(nand, from_parent, who, WII_NAND_WRITE) || !allowed(nand, to_parent, who, WII_NAND_WRITE)) {
    return WII_FS_EACCESS;
  }
  node = child_named(from_parent, from_name, strlen(from_name));
  if (!node) return WII_FS_ENOENT;
  if (!node->directory && strcmp(from_name, to_name) != 0) return WII_FS_EINVAL;
  if (in_use(node)) return WII_FS_EFDOPEN;
  existing = child_named(to_parent, to_name, strlen(to_name));
  if (existing == node) return 0;
  if (node->directory) {
    /* Not into itself, and not so deep that what is inside it would pass
     * the limit. */
    for (up = to_parent; up; up = up->parent) {
      if (up == node) return WII_FS_EINVAL;
    }
    if (depth_of(to_parent) + 1u + height_of(node) > WII_NAND_DEPTH_MAX) return WII_FS_EDIRDEPTH;
  }
  if (existing) {
    if (existing->directory != node->directory) return WII_FS_EINVAL;
    if (in_use(existing)) return WII_FS_EFDOPEN;
    unlink_node(existing);
    node_free(nand, existing);
  }
  unlink_node(node);
  memset(node->name, 0, sizeof(node->name));
  memcpy(node->name, to_name, strlen(to_name));
  link_first(to_parent, node);
  touch(nand);
  return 0;
}

int32_t wii_nand_get_attr(WiiNand *nand, const WiiNandCaller *who, const char *path, WiiNandAttr *attr) {
  unsigned components = 0;
  Node *node = nand->root;
  int32_t result = path_check(path, &components);
  if (result) return result;
  if (components) {
    Node *parent;
    const char *name;
    result = lookup_parent(nand, path, &parent, &name);
    if (result) return result;
    if (!allowed(nand, parent, who, WII_NAND_READ)) return WII_FS_EACCESS;
    node = child_named(parent, name, strlen(name));
    if (!node) return WII_FS_ENOENT;
  }
  attr->uid = node->uid;
  attr->gid = node->gid;
  attr->owner = node->perm[0];
  attr->group = node->perm[1];
  attr->other = node->perm[2];
  attr->attributes = node->attributes;
  attr->is_directory = node->directory;
  return 0;
}

int32_t wii_nand_set_attr(WiiNand *nand, const WiiNandCaller *who, const char *path,
                          const WiiNandAttr *attr) {
  Node *node;
  const int root = !nand->enforce || who->uid == 0;
  int32_t result = path_check(path, NULL);
  if (result) return result;
  result = lookup(nand, path, &node);
  if (result) return result;
  /* Only its owner may change something, and only IOS may give it away -
   * and not even IOS may give away a file with anything in it. */
  if (!root && who->uid != node->uid) return WII_FS_EACCESS;
  if (!root && attr->uid != node->uid) return WII_FS_EACCESS;
  if (attr->uid != node->uid && !node->directory && node->size) return WII_FS_ENOTEMPTY;
  node->uid = attr->uid;
  node->gid = attr->gid;
  node->perm[0] = attr->owner & 3u;
  node->perm[1] = attr->group & 3u;
  node->perm[2] = attr->other & 3u;
  node->attributes = attr->attributes;
  touch(nand);
  return 0;
}

/* The SDK tells a file from a directory by listing it: "invalid" means a
 * file, "no access" still means a directory. So the order of the checks
 * here is part of the interface. */
int32_t wii_nand_read_dir(WiiNand *nand, const WiiNandCaller *who, const char *path, char *names,
                          uint32_t max, uint32_t *count) {
  Node *node;
  const Node *child;
  uint32_t found = 0;
  int32_t result = path_check(path, NULL);
  if (result) return result;
  result = lookup(nand, path, &node);
  if (result) return result;
  if (!allowed(nand, node, who, WII_NAND_READ)) return WII_FS_EACCESS;
  if (!node->directory) return WII_FS_EINVAL;
  for (child = node->child; child; child = child->next) {
    if (names) {
      if (found == max) break;
      memset(names + found * (WII_NAND_NAME_MAX + 1u), 0, WII_NAND_NAME_MAX + 1u);
      memcpy(names + found * (WII_NAND_NAME_MAX + 1u), child->name, strlen(child->name));
    }
    found++;
  }
  if (count) *count = found;
  return 0;
}

int32_t wii_nand_usage(WiiNand *nand, const WiiNandCaller *who, const char *path, uint32_t *clusters,
                       uint32_t *inodes) {
  Node *node;
  uint32_t used = 0, entries = 0;
  int32_t result = path_check(path, NULL);
  (void)who;
  if (result) return result;
  result = lookup(nand, path, &node);
  if (result) return result;
  if (!node->directory) return WII_FS_EINVAL;
  usage_of(node, &used, &entries);
  if (clusters) *clusters = used;
  if (inodes) *inodes = entries;
  return 0;
}

void wii_nand_stats(const WiiNand *nand, WiiNandStats *stats) {
  stats->cluster_size = WII_NAND_CLUSTER;
  stats->used_clusters = nand->clusters;
  stats->bad_clusters = 0;
  stats->reserved_clusters = WII_NAND_RESERVED_CLUSTERS;
  stats->free_clusters = USABLE_CLUSTERS - nand->clusters;
  stats->used_inodes = nand->inodes;
  stats->free_inodes = WII_NAND_INODES - nand->inodes;
}

/* ------------------------------------------------------------------ */
/* Open files                                                          */

static int32_t file_at(const WiiNand *nand, const char *path, Node **out) {
  unsigned components = 0;
  int32_t result = path_check(path, &components);
  if (result) return result;
  if (components == 0) return WII_FS_EINVAL;
  result = lookup(nand, path, out);
  if (result) return result;
  return (*out)->directory ? WII_FS_EINVAL : 0;
}

int32_t wii_nand_open(WiiNand *nand, const WiiNandCaller *who, const char *path, uint32_t mode) {
  Node *node;
  int32_t result;
  /* In ISFS's order: no descriptor left, then what is wrong with the
   * path, then whether the caller may. */
  if (nand->open_files >= WII_NAND_OPEN_MAX) return WII_FS_EFDEXHAUSTED;
  result = file_at(nand, path, &node);
  if (result) return result;
  if (mode & ~(WII_NAND_READ | WII_NAND_WRITE)) return WII_FS_EINVAL;
  if (!allowed(nand, node, who, mode)) return WII_FS_EACCESS;
  node->opens++;
  nand->open_files++;
  return 0;
}

int32_t wii_nand_close(WiiNand *nand, const char *path) {
  Node *node;
  int32_t result = file_at(nand, path, &node);
  if (result) return result;
  if (!node->opens) return WII_FS_EINVAL;
  node->opens--;
  nand->open_files--;
  return 0;
}

int32_t wii_nand_mark_open(WiiNand *nand, const char *path) {
  Node *node;
  int32_t result = file_at(nand, path, &node);
  if (result) return result;
  node->opens++;
  nand->open_files++;
  return 0;
}

static void forget(Node *node) {
  Node *child;
  node->opens = 0;
  for (child = node->child; child; child = child->next) forget(child);
}

void wii_nand_forget_open(WiiNand *nand) {
  if (!nand) return;
  forget(nand->root);
  nand->open_files = 0;
}

unsigned wii_nand_open_count(const WiiNand *nand) { return nand->open_files; }

int32_t wii_nand_read(WiiNand *nand, const char *path, uint32_t mode, uint32_t *position, void *out,
                      uint32_t size) {
  Node *node;
  uint32_t left;
  int32_t result = file_at(nand, path, &node);
  if (result) return result;
  if (!(mode & WII_NAND_READ)) return WII_FS_EACCESS;
  if (size && !out) return WII_FS_EINVAL;
  left = *position < node->size ? node->size - *position : 0u;
  if (size > left) size = left;
  /* The result is the count, and a count has to fit in a positive number. */
  if (size > 0x7fffffffu) size = 0x7fffffffu;
  if (size) memcpy(out, node->data + *position, size);
  *position += size;
  return (int32_t)size;
}

int32_t wii_nand_write(WiiNand *nand, const char *path, uint32_t mode, uint32_t *position,
                       const void *in, uint32_t size) {
  Node *node;
  uint64_t end;
  int32_t result = file_at(nand, path, &node);
  if (result) return result;
  if (!(mode & WII_NAND_WRITE)) return WII_FS_EACCESS;
  if (size && !in) return WII_FS_EINVAL;
  if (!size) return 0;
  if (size > 0x7fffffffu) return WII_FS_EINVAL;
  end = (uint64_t)*position + size;
  if (end > 0xffffffffu) return WII_FS_EFBIG;
  if (end > node->size) {
    const uint32_t before = clusters_of(node->size), after = clusters_of((uint32_t)end);
    if (nand->clusters - before + after > USABLE_CLUSTERS) return WII_FS_EFBIG;
    if (end > node->capacity) {
      /* A save is written in pieces; room is taken a cluster at a time and
       * doubled, so a megabyte written 4 KB at a time is not 256 copies. */
      uint64_t capacity = (uint64_t)node->capacity * 2u;
      uint8_t *grown;
      if (capacity < end) capacity = end;
      capacity = (capacity + WII_NAND_CLUSTER - 1u) / WII_NAND_CLUSTER * WII_NAND_CLUSTER;
      if (capacity > 0xffffffffu) capacity = 0xffffffffu;
      grown = (uint8_t *)realloc(node->data, (size_t)capacity);
      if (!grown) return WII_FS_EFBIG;
      node->data = grown;
      node->capacity = (uint32_t)capacity;
    }
    /* A position past the end cannot be reached by seeking; were it ever
     * to be, the gap reads as zeroes. */
    if (*position > node->size) memset(node->data + node->size, 0, *position - node->size);
    node->size = (uint32_t)end;
    nand->clusters = nand->clusters - before + after;
  }
  memcpy(node->data + *position, in, size);
  *position += size;
  touch(nand);
  return (int32_t)size;
}

int32_t wii_nand_seek(WiiNand *nand, const char *path, uint32_t *position, int32_t where, int32_t whence) {
  Node *node;
  int64_t target;
  int32_t result = file_at(nand, path, &node);
  if (result) return result;
  switch (whence) {
    case 0: target = where; break;
    case 1: target = (int64_t)*position + where; break;
    case 2: target = (int64_t)node->size + where; break;
    default: return WII_FS_EINVAL;
  }
  /* Not before the start and not past the end: ISFS has no holes. */
  if (target < 0 || target > (int64_t)node->size || target > 0x7fffffff) return WII_FS_EINVAL;
  *position = (uint32_t)target;
  return (int32_t)target;
}

int32_t wii_nand_length(WiiNand *nand, const char *path, uint32_t *length) {
  Node *node;
  int32_t result = file_at(nand, path, &node);
  if (result) return result;
  *length = node->size;
  return 0;
}

int32_t wii_nand_put(WiiNand *nand, const char *path, const void *data, uint32_t size) {
  Node *node;
  int32_t result = file_at(nand, path, &node);
  if (result == WII_FS_ENOENT) {
    result = create(nand, &ios_itself, path, 0, 0, 3, 3, 3);
    if (result) return result;
    result = file_at(nand, path, &node);
  }
  if (result) return result;
  if (size && !data) return WII_FS_EINVAL;
  return set_contents(nand, node, data, size);
}

int32_t wii_nand_get(WiiNand *nand, const char *path, const uint8_t **data, uint32_t *size) {
  Node *node;
  int32_t result = file_at(nand, path, &node);
  if (result) return result;
  if (data) *data = node->data;
  if (size) *size = node->size;
  return 0;
}

/* ------------------------------------------------------------------ */
/* A console's flash before any game has run                           */

/* A directory that is there afterwards, made with these attributes if it
 * was not. */
static int32_t ensure_dir(WiiNand *nand, const char *path, uint32_t uid, uint16_t gid, uint8_t owner,
                          uint8_t group, uint8_t other) {
  Node *node;
  int32_t result = path_check(path, NULL);
  if (result) return result;
  if (lookup(nand, path, &node) == 0) return node->directory ? 0 : WII_FS_EINVAL;
  /* Made by IOS, on behalf of whoever is to own it. */
  result = create(nand, &ios_itself, path, 1, 0, owner, group, other);
  if (result) return result;
  result = lookup(nand, path, &node);
  if (result) return result;
  node->uid = uid;
  node->gid = gid;
  return 0;
}

static void own(WiiNand *nand, const char *path, uint32_t uid, uint16_t gid, uint8_t owner, uint8_t group,
                uint8_t other) {
  Node *node;
  if (lookup(nand, path, &node) != 0) return;
  node->uid = uid;
  node->gid = gid;
  node->perm[0] = owner;
  node->perm[1] = group;
  node->perm[2] = other;
}

#define RW (WII_NAND_READ | WII_NAND_WRITE)
#define RO WII_NAND_READ

#define UID_SYS "/sys/uid.sys"
#define SYSCONF_PATH "/shared2/sys/SYSCONF"
#define SETTING_TXT_PATH "/title/00000001/00000002/data/setting.txt"
/* The system menu is the first title any console runs, so the first user
 * number is its; its group is 1. */
#define SYSTEM_MENU_TITLE 0x0000000100000002ull
#define FIRST_UID 0x1000u
#define SYSTEM_MENU_GID 1u

/* /sys/uid.sys: twelve bytes a title - its 64-bit id and the 32-bit user
 * number IOS gave it - in the order the titles were first seen. */
uint32_t wii_nand_title_uid(WiiNand *nand, uint64_t title_id, int allocate) {
  Node *node;
  uint8_t *table;
  uint32_t entries, i, uid;
  uint8_t entry[12];
  if (!nand) return 0;
  if (file_at(nand, UID_SYS, &node) != 0) {
    if (!allocate) return 0;
    if (ensure_dir(nand, "/sys", 0, 0, RW, RW, 0) != 0) return 0;
    if (create(nand, &ios_itself, UID_SYS, 0, 0, RW, RW, 0) != 0) return 0;
    if (file_at(nand, UID_SYS, &node) != 0) return 0;
  }
  entries = node->size / 12u;
  for (i = 0; i < entries; i++) {
    const uint8_t *p = node->data + i * 12u;
    uint64_t id = 0;
    unsigned b;
    for (b = 0; b < 8u; b++) id = (id << 8) | p[b];
    if (id == title_id) {
      return ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 8) | p[11];
    }
  }
  if (!allocate) return 0;
  uid = FIRST_UID + entries;
  for (i = 0; i < 8u; i++) entry[i] = (uint8_t)(title_id >> (8u * (7u - i)));
  entry[8] = (uint8_t)(uid >> 24);
  entry[9] = (uint8_t)(uid >> 16);
  entry[10] = (uint8_t)(uid >> 8);
  entry[11] = (uint8_t)uid;
  table = (uint8_t *)malloc((size_t)entries * 12u + 12u);
  if (!table) return 0;
  if (entries) memcpy(table, node->data, (size_t)entries * 12u);
  memcpy(table + (size_t)entries * 12u, entry, 12u);
  if (set_contents(nand, node, table, entries * 12u + 12u) != 0) uid = 0;
  free(table);
  return uid;
}

int32_t wii_nand_add_title(WiiNand *nand, uint64_t title_id, uint16_t group_id) {
  char path[WII_NAND_PATH_MAX + 1u];
  const unsigned high = (unsigned)(title_id >> 32), low = (unsigned)title_id;
  uint32_t uid;
  int32_t result;
  if (!nand) return WII_FS_EINVAL;
  uid = wii_nand_title_uid(nand, title_id, 1);
  if (!uid) return WII_FS_ENFILE;
  result = ensure_dir(nand, "/title", 0, 0, RW, RW, RO);
  if (result) return result;
  snprintf(path, sizeof(path), "/title/%08x", high);
  result = ensure_dir(nand, path, 0, 0, RW, RW, RO);
  if (result) return result;
  snprintf(path, sizeof(path), "/title/%08x/%08x", high, low);
  result = ensure_dir(nand, path, 0, 0, RW, RW, RO);
  if (result) return result;
  snprintf(path, sizeof(path), "/title/%08x/%08x/content", high, low);
  result = ensure_dir(nand, path, 0, 0, RW, RW, 0);
  if (result) return result;
  snprintf(path, sizeof(path), "/title/%08x/%08x/data", high, low);
  return ensure_dir(nand, path, uid, group_id, RW, 0, 0);
}

/* Where IPL.AR's byte is in the SYSCONF file, or NULL. */
static uint8_t *aspect_byte(WiiNand *nand) {
  Node *node;
  const uint8_t *value;
  unsigned type = 0;
  uint32_t length = 0;
  if (!nand || file_at(nand, SYSCONF_PATH, &node) != 0 || !node->data) return NULL;
  value = wii_sysconf_find(node->data, node->size, "IPL.AR", &type, &length);
  if (!value || type != WII_SYSCONF_BYTE || length != 1u) return NULL;
  return node->data + (value - node->data);
}

int wii_nand_widescreen(WiiNand *nand) {
  const uint8_t *at = aspect_byte(nand);
  return at ? (*at != 0) : -1;
}

int wii_nand_set_widescreen(WiiNand *nand, int widescreen) {
  uint8_t *at = aspect_byte(nand);
  const uint8_t value = widescreen ? 1u : 0u;
  if (!at) return -1;
  if (*at != value) {
    *at = value;
    touch(nand);
  }
  return 0;
}

int wii_nand_apply_settings(WiiNand *nand, const WiiSettings *settings) {
  WiiSettings defaults;
  uint8_t *sysconf;
  uint8_t text[WII_SETTING_TXT_SIZE];
  int status = -1;
  if (!nand) return -1;
  if (!settings) {
    wii_settings_default(&defaults);
    settings = &defaults;
  }
  sysconf = (uint8_t *)malloc(WII_SYSCONF_SIZE);
  if (!sysconf) return -1;
  if (wii_sysconf_build(settings, sysconf) == 0 && wii_setting_txt_build(settings, text) == 0 &&
      ensure_dir(nand, "/shared2", 0, 0, RW, RW, RW) == 0 &&
      ensure_dir(nand, "/shared2/sys", 0, 0, RW, RW, RW) == 0 &&
      wii_nand_add_title(nand, SYSTEM_MENU_TITLE, SYSTEM_MENU_GID) == 0 &&
      wii_nand_put(nand, SYSCONF_PATH, sysconf, WII_SYSCONF_SIZE) == 0 &&
      wii_nand_put(nand, SETTING_TXT_PATH, text, WII_SETTING_TXT_SIZE) == 0) {
    /* SYSCONF is everyone's to read and write - a game saves the remotes
     * it has paired there, and the SDK deletes and remakes the file if it
     * finds it any other way. setting.txt is the system menu's, and
     * everyone else's to read. */
    own(nand, SYSCONF_PATH, 0, 0, RW, RW, RW);
    own(nand, SETTING_TXT_PATH, wii_nand_title_uid(nand, SYSTEM_MENU_TITLE, 0), SYSTEM_MENU_GID, RW, RO, RO);
    status = 0;
  }
  free(sysconf);
  return status;
}

int wii_nand_format(WiiNand *nand, const WiiSettings *settings) {
  /* IOS's directories and who may do what in each. /tmp, /shared2 and
   * /meta are open to every title; the rest are IOS's own, and a title
   * reaches the one directory under /title that is its. */
  static const struct {
    const char *path;
    uint8_t group, other;
  } directories[] = {
      {"/sys", RW, 0},     {"/ticket", RW, 0},   {"/title", RW, RO},     {"/shared1", RW, 0},
      {"/shared2", RW, RW}, {"/shared2/sys", RW, RW}, {"/tmp", RW, RW}, {"/import", RW, 0},
      {"/meta", RW, RW},
  };
  size_t i;
  if (!nand) return -1;
  empty(nand, nand->root);
  touch(nand);
  for (i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
    if (ensure_dir(nand, directories[i].path, 0, 0, RW, directories[i].group, directories[i].other) != 0) {
      empty(nand, nand->root);
      return -1;
    }
  }
  /* The system menu comes first in the table of user numbers, and its
   * data directory is where setting.txt lives. */
  if (wii_nand_apply_settings(nand, settings) != 0) {
    empty(nand, nand->root);
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* One file on the host                                                */

/*
 * The file, every number most significant byte first:
 *
 *   +0x00  "NCWiiFS", a zero byte
 *   +0x08  the format's version (1)
 *   +0x0c  the number of entries that follow
 *   +0x10  the length of the whole file
 *   +0x14  a CRC-32 of everything after this header
 *   +0x18  the entries, a directory followed by everything in it:
 *            +0   1 for a directory, 2 for a file
 *            +1   the length of the name (zero for the root, which is first)
 *            +2   the name, twelve bytes, padded with zeroes
 *            +14  the owner's user number
 *            +18  the group
 *            +20  the attribute byte
 *            +21  the owner's, the group's and everyone else's permissions
 *            +24  for a directory, how many entries it has directly in it;
 *                 for a file, its length - and then its bytes
 *
 * A directory's entries are in the order ISFS lists them. /tmp is written
 * as an empty directory whatever is in it: IOS empties it when it starts,
 * so nothing in it outlives the run that made it.
 */
#define FILE_MAGIC "NCWiiFS"
#define FILE_VERSION 1u
#define FILE_HEADER 0x18u
#define ENTRY_HEADER 28u

static void be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static uint32_t from_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* CRC-32, the usual one (reflected, 0xedb88320). The table is built on
 * the stack each time: this runs once a save and a shared table would be
 * one more thing two machines had in common. */
static uint32_t crc32_of(const uint8_t *data, size_t size) {
  uint32_t table[256];
  uint32_t crc = 0xffffffffu;
  size_t i;
  unsigned n, bit;
  for (n = 0; n < 256u; n++) {
    uint32_t value = n;
    for (bit = 0; bit < 8u; bit++) value = (value & 1u) ? (value >> 1) ^ 0xedb88320u : value >> 1;
    table[n] = value;
  }
  for (i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
  return crc ^ 0xffffffffu;
}

static int is_tmp(const WiiNand *nand, const Node *node) {
  return node->parent == nand->root && node->directory && strcmp(node->name, "tmp") == 0;
}

/* `with_tmp`: what is under /tmp goes too. A file on the host is a console
 * switched off, whose /tmp IOS empties when it starts; a save state is a
 * console still running, with whatever a game has there. */
static uint64_t image_size(const WiiNand *nand, const Node *node, uint32_t *entries, int with_tmp) {
  uint64_t size = ENTRY_HEADER;
  const Node *child;
  *entries += 1u;
  if (!node->directory) return size + node->size;
  if (!with_tmp && is_tmp(nand, node)) return size;
  for (child = node->child; child; child = child->next) size += image_size(nand, child, entries, with_tmp);
  return size;
}

static uint8_t *image_write(const WiiNand *nand, const Node *node, uint8_t *p, int with_tmp) {
  const Node *child;
  uint32_t children = 0;
  memset(p, 0, ENTRY_HEADER);
  p[0] = node->directory ? 1u : 2u;
  p[1] = (uint8_t)strlen(node->name);
  memcpy(p + 2, node->name, strlen(node->name));
  be32(p + 14, node->uid);
  p[18] = (uint8_t)(node->gid >> 8);
  p[19] = (uint8_t)node->gid;
  p[20] = node->attributes;
  p[21] = node->perm[0];
  p[22] = node->perm[1];
  p[23] = node->perm[2];
  if (!node->directory) {
    be32(p + 24, node->size);
    if (node->size) memcpy(p + ENTRY_HEADER, node->data, node->size);
    return p + ENTRY_HEADER + node->size;
  }
  if (with_tmp || !is_tmp(nand, node)) {
    for (child = node->child; child; child = child->next) children++;
  }
  be32(p + 24, children);
  p += ENTRY_HEADER;
  if (children) {
    for (child = node->child; child; child = child->next) p = image_write(nand, child, p, with_tmp);
  }
  return p;
}

uint8_t *wii_nand_image(const WiiNand *nand, int with_tmp, size_t *size_out) {
  uint32_t entries = 0;
  uint64_t size;
  uint8_t *image, *end;
  if (!nand) return NULL;
  size = FILE_HEADER + image_size(nand, nand->root, &entries, with_tmp);
  if (size > 0xffffffffu) return NULL;
  image = (uint8_t *)malloc((size_t)size);
  if (!image) return NULL;
  memcpy(image, FILE_MAGIC, 8);
  be32(image + 8, FILE_VERSION);
  be32(image + 12, entries);
  be32(image + 16, (uint32_t)size);
  end = image_write(nand, nand->root, image + FILE_HEADER, with_tmp);
  if (end != image + size) {
    /* The two walks disagree about the tree: nothing is written. */
    free(image);
    return NULL;
  }
  be32(image + 20, crc32_of(image + FILE_HEADER, (size_t)size - FILE_HEADER));
  if (size_out) *size_out = (size_t)size;
  return image;
}

/* Replace `to` with `from`, whole or not at all. */
static int replace_file(const char *from, const char *to) {
#if defined(_WIN32)
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
  return rename(from, to);
#endif
}

int wii_nand_save(WiiNand *nand, const char *path) {
  size_t size = 0;
  uint8_t *image;
  char *temporary;
  FILE *f;
  size_t written;
  int synced;

  if (!nand || !path || !path[0]) return -1;
  image = wii_nand_image(nand, 0, &size);
  temporary = (char *)malloc(strlen(path) + 5u);
  if (!image || !temporary) {
    free(image);
    free(temporary);
    return -1;
  }

  /* Never in place: opening the file for writing would truncate it first,
   * and a process that died before the last byte would leave a short file
   * - every save on the console gone, where a console would have lost at
   * most the one being made. */
  memcpy(temporary, path, strlen(path));
  memcpy(temporary + strlen(path), ".tmp", 5u);
  f = fopen(temporary, "wb");
  if (!f) {
    free(image);
    free(temporary);
    return -1;
  }
  written = fwrite(image, 1, size, f);
  synced = fflush(f) == 0;
#if defined(_WIN32)
  synced = synced && _commit(_fileno(f)) == 0;
#else
  synced = synced && fsync(fileno(f)) == 0;
#endif
  if (fclose(f) != 0 || written != size || !synced || replace_file(temporary, path) != 0) {
    remove(temporary);
    free(image);
    free(temporary);
    return -1;
  }
  free(image);
  free(temporary);
  nand->dirty = 0;
  return 0;
}

typedef struct Reader {
  const uint8_t *data;
  size_t size;
  size_t at;
  uint32_t entries;
  char *reason;
  size_t reason_size;
} Reader;

/* Keep the first thing found wrong; it is the one that explains the rest. */
static void refuse(Reader *r, const char *format, ...) {
  va_list arguments;
  if (!r->reason || !r->reason_size || r->reason[0]) return;
  va_start(arguments, format);
  vsnprintf(r->reason, r->reason_size, format, arguments);
  va_end(arguments);
}

/* Read one entry and everything in it, checking each of ISFS's rules on
 * the way: a file that breaks one was not written by this code, and a
 * filesystem that half obeys the rules is worse than none. */
static int read_entry(Reader *r, WiiNand *nand, Node *parent, unsigned depth) {
  const uint8_t *p = r->data + r->at;
  const unsigned number = r->entries;
  Node *node;
  uint32_t count, i;
  unsigned kind, name_length;

  if (r->size - r->at < ENTRY_HEADER) {
    refuse(r, "entry %u starts %u bytes before the end of the file", number, (unsigned)(r->size - r->at));
    return -1;
  }
  kind = p[0];
  name_length = p[1];
  count = from_be32(p + 24);
  if (kind != 1u && kind != 2u) {
    refuse(r, "entry %u is neither a file nor a directory (%u)", number, kind);
    return -1;
  }
  if (parent ? (name_length < 1u || name_length > WII_NAND_NAME_MAX) : (name_length != 0u || kind != 1u)) {
    refuse(r, "entry %u has a name of %u characters", number, name_length);
    return -1;
  }
  for (i = 0; i < WII_NAND_NAME_MAX; i++) {
    const uint8_t c = p[2u + i];
    if (i < name_length ? (c < 0x20u || c > 0x7eu || c == '/') : c != 0u) {
      refuse(r, "entry %u has a name ISFS would not allow", number);
      return -1;
    }
  }
  if (p[21] > 3u || p[22] > 3u || p[23] > 3u) {
    refuse(r, "entry %u has permissions that are not permissions", number);
    return -1;
  }
  if (parent && child_named(parent, (const char *)p + 2, name_length)) {
    refuse(r, "entry %u is the second \"%.12s\" in its directory", number, (const char *)p + 2);
    return -1;
  }
  if (kind == 1u && depth > WII_NAND_DEPTH_MAX) {
    refuse(r, "entry %u is a directory more than %u deep", number, WII_NAND_DEPTH_MAX);
    return -1;
  }
  if (nand->inodes >= WII_NAND_INODES) {
    refuse(r, "more than %u entries", WII_NAND_INODES);
    return -1;
  }

  if (parent) {
    node = node_new((const char *)p + 2, name_length, kind == 1u);
    if (!node) {
      refuse(r, "out of memory");
      return -1;
    }
    link_last(parent, node);
    nand->inodes++;
  } else {
    node = nand->root;
  }
  node->uid = from_be32(p + 14);
  node->gid = (uint16_t)(((uint16_t)p[18] << 8) | p[19]);
  node->attributes = p[20];
  node->perm[0] = p[21];
  node->perm[1] = p[22];
  node->perm[2] = p[23];
  r->at += ENTRY_HEADER;
  r->entries++;

  if (kind == 2u) {
    if (count > r->size - r->at) {
      refuse(r, "entry %u (\"%s\") says it has %u bytes and the file has %u left", number, node->name,
             (unsigned)count, (unsigned)(r->size - r->at));
      return -1;
    }
    if (nand->clusters + clusters_of(count) > USABLE_CLUSTERS) {
      refuse(r, "the files are larger than the chip");
      return -1;
    }
    if (count) {
      node->data = (uint8_t *)malloc(count);
      if (!node->data) {
        refuse(r, "out of memory");
        return -1;
      }
      memcpy(node->data, r->data + r->at, count);
    }
    node->size = count;
    node->capacity = count;
    nand->clusters += clusters_of(count);
    r->at += count;
    return 0;
  }
  for (i = 0; i < count; i++) {
    if (read_entry(r, nand, node, depth + 1u) != 0) return -1;
  }
  return 0;
}

static WiiNand *from_image(Reader *r, const uint8_t *data, size_t size);

WiiNand *wii_nand_load(const char *path, int *missing, char *reason, size_t reason_size) {
  Reader reader;
  Reader *r = &reader;
  WiiNand *nand = NULL;
  uint8_t *data = NULL;
  FILE *f;
  long length = -1;

  memset(r, 0, sizeof(*r));
  r->reason = reason;
  r->reason_size = reason_size;
  if (reason && reason_size) reason[0] = 0;
  if (missing) *missing = 0;
  if (!path || !path[0]) {
    refuse(r, "no file was named");
    return NULL;
  }

  errno = 0;
  f = fopen(path, "rb");
  if (!f) {
    /* Not there at all, or there and unreadable: only the first is a
     * console that has never been switched on. A file that exists and
     * cannot be opened must not be taken for a blank one and overwritten. */
    if (errno == ENOENT) {
      if (missing) *missing = 1;
      refuse(r, "there is no such file");
    } else {
      refuse(r, "the file could not be opened");
    }
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) == 0) length = ftell(f);
  rewind(f);
  if (length == 0) {
    fclose(f);
    if (missing) *missing = 1;
    refuse(r, "the file is empty");
    return NULL;
  }
  if (length < 0 || (data = (uint8_t *)malloc((size_t)length)) == NULL ||
      fread(data, 1, (size_t)length, f) != (size_t)length) {
    fclose(f);
    free(data);
    refuse(r, "the file could not be read");
    return NULL;
  }
  fclose(f);
  nand = from_image(r, data, (size_t)length);
  free(data);
  return nand;
}

WiiNand *wii_nand_from_image(const uint8_t *data, size_t size, char *reason, size_t reason_size) {
  Reader reader;
  memset(&reader, 0, sizeof(reader));
  reader.reason = reason;
  reader.reason_size = reason_size;
  if (reason && reason_size) reason[0] = 0;
  if (!data) {
    refuse(&reader, "there is no image");
    return NULL;
  }
  return from_image(&reader, data, size);
}

static WiiNand *from_image(Reader *r, const uint8_t *data, size_t size) {
  WiiNand *nand = NULL;
  r->data = data;
  r->size = size;

  if (r->size < FILE_HEADER + ENTRY_HEADER || memcmp(data, FILE_MAGIC, 8) != 0) {
    refuse(r, "this is not a native-console Wii flash file (a console's own NAND dump is not one)");
  } else if (from_be32(data + 8) != FILE_VERSION) {
    refuse(r, "the file is format version %u and this build reads version %u", (unsigned)from_be32(data + 8),
           FILE_VERSION);
  } else if (from_be32(data + 16) != r->size) {
    refuse(r, "the file is %u bytes and says it should be %u: it has been cut short or added to",
           (unsigned)r->size, (unsigned)from_be32(data + 16));
  } else if (crc32_of(data + FILE_HEADER, r->size - FILE_HEADER) != from_be32(data + 20)) {
    refuse(r, "the file's checksum does not match its contents");
  } else if ((nand = wii_nand_create()) == NULL) {
    refuse(r, "out of memory");
  } else {
    r->at = FILE_HEADER;
    if (read_entry(r, nand, NULL, 0) != 0) {
      wii_nand_destroy(nand);
      nand = NULL;
    } else if (r->at != r->size) {
      refuse(r, "%u bytes follow the last entry", (unsigned)(r->size - r->at));
      wii_nand_destroy(nand);
      nand = NULL;
    } else if (r->entries != from_be32(data + 12)) {
      refuse(r, "the file has %u entries and says it has %u", (unsigned)r->entries,
             (unsigned)from_be32(data + 12));
      wii_nand_destroy(nand);
      nand = NULL;
    }
  }
  return nand;
}

int wii_nand_dirty(const WiiNand *nand) { return nand && nand->dirty; }

void wii_nand_mark_changed(WiiNand *nand) {
  if (nand) touch(nand);
}

uint64_t wii_nand_changed_at(const WiiNand *nand) { return nand ? nand->changed : 0; }

void wii_nand_set_time(WiiNand *nand, uint64_t now) {
  if (!nand) return;
  nand->now = now;
  /* A machine's clock starts again when the machine is reset; a change
   * made before that happened no later than now. */
  if (nand->changed > now) nand->changed = now;
}
