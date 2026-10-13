/*
 * What gc_disc.c and wii_disc.c ask of each other. Nothing outside those
 * two files includes this: everyone else reads a disc through gc_disc.h.
 */
#ifndef WII_DISC_INTERNAL_H
#define WII_DISC_INTERNAL_H

#include "gc_disc.h"

/* ---- gc_disc.c, for wii_disc.c ---- */

/* Bytes straight out of the image file, at an offset in the file. */
int gc_disc_file_read(GcDisc *disc, uint64_t file_offset, void *destination, uint32_t length);

/* The 0x440-byte header and the filesystem, read through gc_disc_read and
 * put into the disc's fields - which, for a Wii disc, can only happen
 * once its partition is open. Leaves no half-read filesystem behind when
 * it fails. */
int gc_disc_load_layout(GcDisc *disc, FILE *log);

/* ---- wii_disc.c, for gc_disc.c ---- */

/* Is the file a WBFS? Returns 1 and sets `disc->size` to the size of the
 * disc it stands for if so, 0 if it is something else, and -1 - with the
 * reason on `log` - for a WBFS this cannot read. `disc->size` is the
 * file's size on the way in. */
int wii_wbfs_open(GcDisc *disc, const char *path, FILE *log);
int wii_disc_is_wbfs(const GcDisc *disc);
/* Bytes of the disc a WBFS stands for. The caller has checked the range. */
int wii_wbfs_read(GcDisc *disc, uint64_t offset, void *destination, uint32_t length);

/* Finds the game partition, reads its ticket and metadata, and looks for
 * a key. Returns 0 for a disc that can be used, locked or not, and -1
 * with the reason on `log` for one that cannot. */
int wii_disc_open(GcDisc *disc, const char *path, FILE *log);
/* Decrypted bytes of the game partition. */
int wii_disc_read(GcDisc *disc, uint64_t offset, void *destination, uint32_t length);
void wii_disc_close(GcDisc *disc);

#endif /* WII_DISC_INTERNAL_H */
