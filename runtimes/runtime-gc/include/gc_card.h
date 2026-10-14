#ifndef GC_CARD_H
#define GC_CARD_H

#include <stdint.h>

struct GcMachine;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A memory card in the first slot.
 *
 * The card is a flash chip on the expansion bus that answers a handful of
 * one-byte commands. Everything a game's save system does - mounting,
 * listing, creating, reading, writing - is built out of three of them: read
 * 512 bytes, program 128 bytes, erase one 8 KB sector. The file system on
 * top of that is the card library's, not the hardware's, and this file does
 * not implement it; it implements the chip, and the library running on the
 * guest builds the rest.
 *
 * The image is a plain file of the card's exact size, so it is the same
 * thing every other tool calls a memory card image. See
 * docs/gc-memory-card.md for the bus, the commands, the layout and the
 * things that are still unverified.
 */

/* A new card is a 4 Mbit card: the smallest size the library accepts, 64
 * blocks of 8 KB, of which five are the file system's own and 59 are a
 * game's. Its identifier is `0x0004`, whose sector-size and latency fields
 * are both index zero - which is what every card that was ever sold reports.
 * An existing image may be any of the six sizes (4 to 128 Mbit, 0.5 to 16
 * MB, always 8 KB blocks), and the card then reports that size. */
#define GC_CARD_SECTOR 0x2000u
#define GC_CARD_BLOCKS 64u
#define GC_CARD_SYSTEM_BLOCKS 5u
#define GC_CARD_BYTES (GC_CARD_BLOCKS * GC_CARD_SECTOR)
#define GC_CARD_ID 0x00000004u

/* The megabit count - which is also the card's identifier - of an image of
 * `bytes`, or zero when no card is that size. */
uint32_t gc_card_megabits(uint32_t bytes);

/* Put a card in the first slot, backed by `path`. An existing file of a
 * card's size is used as it stands; no file, or an empty one, is formatted
 * as a new 4 Mbit card, so a game asked to save on a machine that has never
 * saved gets a blank card rather than a demand to format one. A file of any
 * other size is refused and left alone. Returns zero on success. The image
 * is written back a field after the guest stops writing to it, and on
 * eject, always by replacing the file whole (see gc_card_flush). */
int gc_card_insert(struct GcMachine *m, const char *path);

/* The same card as gc_card_insert would give, held in memory and never
 * written back: for a second machine that must see the card the first one
 * does without both of them writing the one file (a differential's
 * interpreter). */
int gc_card_insert_unbacked(struct GcMachine *m, const char *path);

/* A save state carries the card's image (gc_card_state_save), and loading
 * one puts it back and writes it to the file - the game's memory holds that
 * card's directory, so anything else would corrupt it. The image it
 * replaced, saves made since the state included, is kept in `<path>.bak`. */

/* Write the image back, if anything changed: to `<path>.tmp`, flushed to the
 * disk, then renamed over the card, so the file is always one whole image
 * even if the process dies part way. Called at the end of a run and safe to
 * call at any other time. */
int gc_card_flush(struct GcMachine *m);

void gc_card_eject(struct GcMachine *m);

/* Whether the slot has anything in it. The bus reports an empty slot as
 * empty, and a game asked to save on one says there is no memory card,
 * which is a correct answer rather than a failure. */
int gc_card_present(const struct GcMachine *m);

/* The card chip's 12-byte flash identifier, which the console's settings
 * memory holds for slot A and the save library checks the card's header
 * serial against. Recovered from the header (serial minus the generator
 * run seeded by the format time), so any self-consistent card mounts; zero
 * for an empty slot. See gc_card.c. */
void gc_card_flash_id(const struct GcMachine *m, uint8_t out[12]);

/* What the expansion bus hands over. Each is a no-op when the slot is
 * empty, and each returns whether the card answered, so the bus can leave
 * the register alone when it did not. */
void gc_card_select(struct GcMachine *m, int selected);
int gc_card_command_bytes(struct GcMachine *m, uint32_t word, unsigned count);
int gc_card_immediate_read(struct GcMachine *m, unsigned count, uint32_t *out);
int gc_card_dma(struct GcMachine *m, uint8_t *ram, uint32_t length, int to_card);

#ifdef __cplusplus
}
#endif

#endif
