/*
 * A memory card, as the chip rather than as the file system.
 *
 * See gc_card.h for what this is and docs/gc-memory-card.md for where every
 * number in it comes from. The short version: a game's save library talks to
 * a flash chip over the expansion bus in three commands - read 512 bytes,
 * program 128 bytes, erase 8192 - and builds a directory, an allocation
 * table and two copies of each on top. None of that is the hardware's, so
 * none of it is here; the only file-system knowledge in this file is in the
 * formatter, which has to produce an image the library will accept.
 *
 * Three things in here are load-bearing and none of them is obvious:
 *
 *  - The first byte of a transfer is the *most significant* byte of the
 *    immediate register, in both directions. A status byte placed in the low
 *    byte reads back as zero, which the library reads as a locked card and
 *    answers by uploading a decryption microcode to the sound processor.
 *
 *  - A write and an erase finish by the card raising its own interrupt, not
 *    by the transfer completing. A card that skips it reads perfectly and
 *    silently never saves - the write appears to work and fails a tenth of a
 *    second later.
 *
 *  - A command is a sequence of transfers between one chip-select and the
 *    next, so the bus has to tell this file when the card is selected. There
 *    is no other framing.
 */
#include "gc_card.h"

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

#include "gc.h"

/* Raised by the bus when the card asks for attention; gc_io.c owns the
 * register it lives in. */
void gc_exi_card_interrupt(struct GcMachine *m);

/* The status byte the card reports.
 *
 * Bit 6 says the card is unlocked, and it is the one that matters: a card
 * that reports itself locked sends the library into a challenge-response
 * cipher driven by a sound-processor task, which ends by asking again and
 * failing anyway. Bits 4 and 3 are the error bits and stay clear; there is
 * no reason to simulate a failing card. Bit 0 is called ready elsewhere and
 * costs nothing. */
#define GC_CARD_STATUS_READY 0x41u
#define GC_CARD_STATUS_ERRORS 0x18u

/* The commands. Everything else is rejected, which no game notices: the
 * library issues exactly these. */
#define GC_CARD_CMD_ID 0x00u
#define GC_CARD_CMD_READ_STATUS 0x83u
#define GC_CARD_CMD_CLEAR_STATUS 0x89u
#define GC_CARD_CMD_INTERRUPT 0x81u
#define GC_CARD_CMD_READ 0x52u
#define GC_CARD_CMD_WRITE 0xf2u
#define GC_CARD_CMD_ERASE 0xf1u

/* What one of each bulk command moves. These are the card's, not the file
 * system's: a block is sixteen reads or sixty-four writes. */
#define GC_CARD_READ_SIZE 512u
#define GC_CARD_PAGE 128u

typedef struct GcCard {
  uint8_t *image;
  /* The image's size, one of the six a card can have (gc_card.h). Its
   * megabit count is also the card's identifier. */
  uint32_t bytes;
  char *path;
  int dirty;
  /* When the image was last changed. A save is a long burst of small writes
   * - an erase and sixty-four page programs for each of the data, the
   * allocation table and the directory - and writing half a megabyte back
   * after every one of them would be a hundred megabytes of file I/O for one
   * save. Writing back only once the burst has stopped costs one. */
  uint64_t changed_cycles;
  /* After a write-back failed: when to try again, and whether it was said. */
  uint64_t retry_cycles;
  int flush_failed;

  /* The command being assembled. A command arrives as several immediate
   * transfers - the library splits anything longer than four bytes - and
   * the pieces have to be concatenated, because the split is an artefact of
   * a four-byte register and not something a chip can see. */
  uint8_t command[16];
  unsigned command_length;

  int selected;
  uint8_t status;
  /* Set by a write or an erase and answered when the card is deselected.
   * See the note at the top about why this is not optional. */
  int completion_pending;
  /* GC_CARD_TRACE set in the environment when the card went in: every
   * erase, and every read and program that starts a sector, goes to stderr
   * with the field it happened in (GC_CARD_TRACE=2: every read and program). What a game's save library does to a
   * card is otherwise invisible until the image is looked at afterwards. */
  int trace;
} GcCard;

static void card_trace(const struct GcMachine *m, const GcCard *card, const char *what, uint32_t address) {
  if (!card->trace) return;
  fprintf(stderr, "card: field %llu %s %06x (block %u)\n", (unsigned long long)m->field, what, address,
          address / GC_CARD_SECTOR);
}

static GcCard *card_of(struct GcMachine *m) { return m ? (GcCard *)m->card : NULL; }

int gc_card_present(const struct GcMachine *m) { return m && m->card != NULL; }

/* ---------------------------------------------------------------- format */

/* The checksum every one of the five system structures uses: the region as
 * big-endian halfwords, their sum and the sum of their complements.
 *
 * The last two lines are not decoration. A result of 0xffff is stored as
 * zero, and that substitution is the only reason a directory of 4094 words
 * of 0xffff - which is what a freshly formatted one is - can have a valid
 * checksum at all. An implementation that computes the sums and stops also
 * has a one-in-65536 chance per structure of writing a pair the library
 * rejects. */
static void card_checksum(const uint8_t *p, unsigned bytes, uint16_t *first,
                          uint16_t *second) {
  uint16_t a = 0, b = 0;
  unsigned i;
  for (i = 0; i + 1u < bytes; i += 2u) {
    const uint16_t word = (uint16_t)(((uint16_t)p[i] << 8) | p[i + 1u]);
    a = (uint16_t)(a + word);
    b = (uint16_t)(b + (uint16_t)(word ^ 0xffffu));
  }
  if (a == 0xffffu) a = 0;
  if (b == 0xffffu) b = 0;
  *first = a;
  *second = b;
}

static void put16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static void put32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

/* The serial, and the chip identity it is built from.
 *
 * A card's header serial is twelve bytes of the card's flash identifier,
 * each plus a value from a linear congruential generator seeded with the
 * format time stored beside it. The retail library checks that relationship
 * every time it mounts a card (its VerifyID): it recomputes the generator
 * from the stored format time, adds the flash identifier the console's
 * settings memory holds for that slot, and calls the card broken - "the
 * memory card is corrupted and needs to be formatted" - if any byte
 * differs. libogc never checks it, which is how an image with an arbitrary
 * serial passed every test program and failed the first commercial game.
 *
 * On hardware the settings memory learns the identifier from the card: a
 * card is locked at power-on and the unlock handshake derives the
 * identifier from the chip and writes it into the settings block. This card
 * reports itself unlocked, so the handshake never happens, and the machine
 * does its job instead - gc_card_flash_id() recovers the identifier from the
 * header, and the settings block reports it (gc_io.c). That is what makes a
 * card copied from somewhere else mount here, as a real card moved between
 * consoles does.
 *
 * The generator is the library's, on a signed 64-bit time: multiply, add,
 * arithmetic shift. */
static int64_t card_rand(int64_t value) {
  const uint64_t next = (uint64_t)value * 1103515245ull + 12345ull;
  return (next & 0x8000000000000000ull) ? (int64_t)~(~next >> 16) : (int64_t)(next >> 16);
}

static int64_t card_format_time(const uint8_t *header) {
  uint64_t time = 0;
  unsigned i;
  for (i = 0; i < 8u; i++) time = (time << 8) | header[0x0c + i];
  return (int64_t)time;
}

/* serial[i] = flash_id[i] + generator, or back again when `recover`. */
static void card_serial_mix(const uint8_t *in, uint8_t *out, int64_t format_time, int recover) {
  int64_t rand = format_time;
  unsigned i;
  for (i = 0; i < 12u; i++) {
    rand = card_rand(rand);
    out[i] = (uint8_t)(recover ? in[i] - (uint8_t)rand : in[i] + (uint8_t)rand);
    rand = card_rand(rand) & 0x7fff;
  }
}

void gc_card_flash_id(const struct GcMachine *m, uint8_t out[12]) {
  const GcCard *card = m ? (const GcCard *)m->card : NULL;
  if (!card || !card->image) {
    memset(out, 0, 12u);
    return;
  }
  card_serial_mix(card->image, out, card_format_time(card->image), 1);
}

/* The flash identifier of a card this machine formats: stable across formats
 * of the same file and different between different files, which is what
 * telling two cards apart needs, and reproducible. */
static void card_chip_id(uint8_t out[12], const char *path) {
  uint64_t state = 0x9e3779b97f4a7c15ull;
  unsigned i;
  for (i = 0; path && path[i]; i++) {
    state = (state ^ (uint8_t)path[i]) * 0x100000001b3ull;
  }
  for (i = 0; i < 12u; i++) {
    state = ((state * 0x41c64e6dull) + 0x3039ull) >> 16;
    out[i] = (uint8_t)state;
    state = (((state * 0x41c64e6dull) + 0x3039ull) >> 16) & 0x7fffull;
  }
}

/* The five system blocks, as a format leaves them.
 *
 * The directory and the allocation table are near-mirror images of each
 * other and that is the easiest thing here to transpose: the directory is
 * filled with 0xff and keeps its checksums at the *end*, covering everything
 * before them; the table is filled with zeroes and keeps its checksums at
 * the *front*, covering everything after them. Both regions are 8188 bytes.
 * They are opposite because their empty values are opposite - a free
 * directory entry is marked by a leading 0xff, and a free block is marked by
 * a zero. */
static void card_format(uint8_t *image, uint32_t bytes, const char *path) {
  uint8_t *block;
  uint16_t first, second;
  unsigned copy;
  const uint32_t blocks = bytes / GC_CARD_SECTOR;

  /* Block 0, the header. Everything past the first 512 bytes stays 0xff and
   * is covered by no checksum. */
  block = image;
  memset(block, 0xff, GC_CARD_SECTOR);
  memset(block + 0x0c, 0, 8);   /* format time: a timebase reading, not a date */
  {
    /* With a format time of zero the generator stays at zero, so the serial
     * is the identifier itself; going through the mix keeps the one rule. */
    uint8_t chip[12];
    card_chip_id(chip, path);
    card_serial_mix(chip, block, card_format_time(block), 0);
  }
  put32(block + 0x14, 0);       /* the settings block's clock bias, which is zero */
  put32(block + 0x18, 0);       /* and its language, English */
  put32(block + 0x1c, 0);       /* the video interface's component-cable register */
  put16(block + 0x20, 0);       /* device identifier */
  put16(block + 0x22, (uint16_t)gc_card_megabits(bytes)); /* size, the same value the bus reports */
  put16(block + 0x24, 0);       /* text encoding: the console is not a Japanese one */
  memset(block + 0x26, 0xff, 0x1fcu - 0x26u);
  card_checksum(block, 0x1fcu, &first, &second);
  put16(block + 0x1fc, first);
  put16(block + 0x1fe, second);

  /* Blocks 1 and 2, the directory and its copy. Identical, and all 127
   * entries free because a formatted entry is 0xff throughout. Both carry a
   * generation counter of 0xffff, which is what filling with 0xff leaves and
   * what the library expects to find. */
  for (copy = 0; copy < 2u; copy++) {
    block = image + (1u + copy) * GC_CARD_SECTOR;
    memset(block, 0xff, GC_CARD_SECTOR);
    card_checksum(block, 0x1ffcu, &first, &second);
    put16(block + 0x1ffc, first);
    put16(block + 0x1ffe, second);
  }

  /* Blocks 3 and 4, the allocation table and its copy. The second carries
   * the higher generation counter, so it is the current one and the first is
   * where the next update will land. The free count is checked against the
   * table at every mount, so a table whose checksum is right and whose count
   * is wrong is rejected. */
  for (copy = 0; copy < 2u; copy++) {
    block = image + (3u + copy) * GC_CARD_SECTOR;
    memset(block, 0, GC_CARD_SECTOR);
    put16(block + 0x04, (uint16_t)copy);
    put16(block + 0x06, (uint16_t)(blocks - GC_CARD_SYSTEM_BLOCKS));
    put16(block + 0x08, (uint16_t)(GC_CARD_SYSTEM_BLOCKS - 1u));
    card_checksum(block + 4, 0x1ffcu, &first, &second);
    put16(block + 0x00, first);
    put16(block + 0x02, second);
  }

  /* Everything above the system area is erased flash. */
  memset(image + GC_CARD_SYSTEM_BLOCKS * GC_CARD_SECTOR, 0xff,
         bytes - GC_CARD_SYSTEM_BLOCKS * GC_CARD_SECTOR);
}

uint32_t gc_card_megabits(uint32_t bytes) {
  switch (bytes) {
  case 0x080000u: return 4u;
  case 0x100000u: return 8u;
  case 0x200000u: return 16u;
  case 0x400000u: return 32u;
  case 0x800000u: return 64u;
  case 0x1000000u: return 128u;
  default: return 0u;
  }
}

/* ------------------------------------------------------------- the slot */

static int card_insert(struct GcMachine *m, const char *path, int write_back) {
  GcCard *card;
  FILE *f;
  size_t length;

  if (!m || !path) return -1;
  gc_card_eject(m);

  /* The file's size decides the card's: any of the six sizes a card can be
   * is used as it stands, so an image from another tool or a bigger card
   * works. No file, or an empty one, is a new 4 Mbit card. Anything else is
   * not a card, and overwriting it would be wrong - so say so and take no
   * card at all. */
  length = 0;
  f = fopen(path, "rb");
  if (f) {
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    rewind(f);
    if (size < 0 || (size != 0 && gc_card_megabits((uint32_t)size) == 0)) {
      fclose(f);
      return -1;
    }
    length = (size_t)size;
  }

  card = (GcCard *)calloc(1, sizeof(*card));
  if (!card) {
    if (f) fclose(f);
    return -1;
  }
  card->bytes = length ? (uint32_t)length : GC_CARD_BYTES;
  card->image = (uint8_t *)malloc(card->bytes);
  card->path = (char *)malloc(strlen(path) + 1u);
  if (!card->image || !card->path || (length && fread(card->image, 1, length, f) != length)) {
    if (f) fclose(f);
    free(card->image);
    free(card->path);
    free(card);
    return -1;
  }
  if (f) fclose(f);
  memcpy(card->path, path, strlen(path) + 1u);
  card->status = GC_CARD_STATUS_READY;
  {
    const char *trace = getenv("GC_CARD_TRACE");
    card->trace = trace ? (atoi(trace) > 1 ? 2 : 1) : 0;
  }

  if (!length) {
    /* Format rather than refuse: a game asked to save on a machine that has
     * never saved should find a blank card, not a demand to go and make one. */
    card_format(card->image, card->bytes, path);
    card->dirty = 1;
  }

  m->card = card;
  if (!write_back) {
    /* The same card, in memory only: the formatted image (if any) is the
     * one a file-backed insert of the same path would make, and nothing is
     * ever written. */
    free(card->path);
    card->path = NULL;
    card->dirty = 0;
    return 0;
  }
  if (card->dirty) return gc_card_flush(m);
  return 0;
}

int gc_card_insert(struct GcMachine *m, const char *path) { return card_insert(m, path, 1); }

int gc_card_insert_unbacked(struct GcMachine *m, const char *path) { return card_insert(m, path, 0); }

/* Writing back replaces the file whole, through a temporary beside it.
 *
 * Opening the card for writing in place truncates it first, so a machine
 * that stopped between the truncation and the last byte - a crash, the
 * process killed, the disk full - left a short file, which the next run
 * refuses as not a card: every save on it gone, where a console would have
 * lost at most the one being made. Written to `<card>.tmp`, flushed to the
 * disk and then renamed over the card, the file on disk is always one whole
 * image, the old one or the new one. */
static int card_replace(const char *from, const char *to) {
#if defined(_WIN32)
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
  return rename(from, to);
#endif
}

/* The image, written whole to `<card><suffix>` through `<card><suffix>.tmp`. */
static int card_write(const GcCard *card, const char *suffix) {
  FILE *f;
  size_t written;
  char *target, *temporary;
  int synced;
  const size_t length = strlen(card->path) + strlen(suffix);
  target = (char *)malloc(length + 1u);
  temporary = (char *)malloc(length + 5u);
  if (!target || !temporary) {
    free(target);
    free(temporary);
    return -1;
  }
  strcpy(target, card->path);
  strcat(target, suffix);
  strcpy(temporary, target);
  strcat(temporary, ".tmp");
  f = fopen(temporary, "wb");
  if (!f) {
    free(target);
    free(temporary);
    return -1;
  }
  written = fwrite(card->image, 1, card->bytes, f);
  synced = fflush(f) == 0;
#if defined(_WIN32)
  synced = synced && _commit(_fileno(f)) == 0;
#else
  synced = synced && fsync(fileno(f)) == 0;
#endif
  if (fclose(f) != 0 || written != card->bytes || !synced || card_replace(temporary, target) != 0) {
    remove(temporary);
    free(target);
    free(temporary);
    return -1;
  }
  free(target);
  free(temporary);
  return 0;
}

int gc_card_flush(struct GcMachine *m) {
  GcCard *card = card_of(m);
  if (!card || !card->dirty || !card->path) return 0; /* a card from a state, in memory only */
  if (card_write(card, "") != 0) return -1;
  card->dirty = 0;
  return 0;
}

/* Called once a field. A card that has been written to and then left alone
 * for a whole field is written back, so a save survives whatever happens to
 * the machine next - which is the behaviour a console has, where the writing
 * is the saving and there is nothing else to wait for. */
void gc_card_advance(struct GcMachine *m) {
  GcCard *card = card_of(m);
  if (!card || !card->dirty) return;
  if (m->cpu.cycles - card->changed_cycles < GC_CYCLES_PER_FIELD) return;
  if (m->cpu.cycles < card->retry_cycles) return;
  if (gc_card_flush(m) != 0) {
    /* Somebody else has the file open, or the disk is full. Say so once and
     * try again a second later rather than every field; the image stays in
     * memory and is written the moment it can be. */
    if (!card->flush_failed) {
      fprintf(stderr, "gc: cannot write the memory card %s; will keep trying\n", card->path);
    }
    card->flush_failed = 1;
    card->retry_cycles = m->cpu.cycles + 60u * GC_CYCLES_PER_FIELD;
  } else if (card->flush_failed) {
    fprintf(stderr, "gc: the memory card %s is written\n", card->path);
    card->flush_failed = 0;
  }
}

void gc_card_eject(struct GcMachine *m) {
  GcCard *card = card_of(m);
  if (!card) return;
  gc_card_flush(m);
  free(card->image);
  free(card->path);
  free(card);
  m->card = NULL;
}

/* ---------------------------------------------------------- the commands */

/* The address the two bulk commands carry, packed into four bytes as a
 * 15-bit segment, a 2-bit page within it and a 7-bit byte within that -
 * which is exactly the granularity of the commands themselves, four pages of
 * 128 bytes to a 512-byte segment. */
static uint32_t card_address(const uint8_t *command) {
  return ((uint32_t)command[1] << 17) | ((uint32_t)command[2] << 9) |
         ((uint32_t)command[3] << 7) | (uint32_t)command[4];
}

void gc_card_select(struct GcMachine *m, int selected) {
  GcCard *card = card_of(m);
  if (!card) return;
  if (selected) {
    if (!card->selected) card->command_length = 0;
    card->selected = 1;
    return;
  }
  if (!card->selected) return;
  card->selected = 0;
  /* A program and an erase report completion by asking for attention, after
   * the bus has been released. Nothing else does. */
  if (card->completion_pending) {
    card->completion_pending = 0;
    gc_exi_card_interrupt(m);
  }
}

/* An immediate write: the next piece of the command. `word` holds the bytes
 * in the register's order, the first on the wire in the top byte. */
int gc_card_command_bytes(struct GcMachine *m, uint32_t word, unsigned count) {
  GcCard *card = card_of(m);
  unsigned i;
  if (!card || !card->selected) return 0;
  for (i = 0; i < count && i < 4u; i++) {
    if (card->command_length >= sizeof(card->command)) break;
    card->command[card->command_length++] = (uint8_t)(word >> ((3u - i) * 8u));
  }
  /* The two commands with no data phase happen here, because there is
   * nothing else to wait for. */
  if (card->command_length == 1u && card->command[0] == GC_CARD_CMD_CLEAR_STATUS) {
    card->status = (uint8_t)(card->status & (0xffu ^ GC_CARD_STATUS_ERRORS));
  } else if (card->command_length == 3u && card->command[0] == GC_CARD_CMD_ERASE) {
    /* An erase names a 512-byte segment and clears the 8192-byte sector
     * containing it. That size is not a guess: the library erases one
     * directory copy and rewrites only it, which would be impossible if an
     * erase took anything larger with it. */
    const uint32_t address = ((uint32_t)card->command[1] << 17) |
                             ((uint32_t)card->command[2] << 9);
    const uint32_t sector = (address / GC_CARD_SECTOR) * GC_CARD_SECTOR;
    card_trace(m, card, "erase", sector);
    if (sector + GC_CARD_SECTOR <= card->bytes) {
      memset(card->image + sector, 0xff, GC_CARD_SECTOR);
      card->dirty = 1;
      card->changed_cycles = m->cpu.cycles;
    }
    card->completion_pending = 1;
  }
  return 1;
}

int gc_card_immediate_read(struct GcMachine *m, unsigned count, uint32_t *out) {
  GcCard *card = card_of(m);
  if (!card || !card->selected || !out) return 0;
  (void)count;
  /* The identifier: two zero bytes in, four out. Read as four bytes, the
   * register's value arrives at the guest as the word it is, so the
   * identifier goes in unshifted. */
  if (card->command_length >= 2u && card->command[0] == GC_CARD_CMD_ID &&
      card->command[1] == 0u) {
    *out = gc_card_megabits(card->bytes);
    return 1;
  }
  /* The status: one byte, and therefore the *top* byte of the register. */
  if (card->command_length >= 1u && card->command[0] == GC_CARD_CMD_READ_STATUS) {
    *out = (uint32_t)card->status << 24;
    return 1;
  }
  *out = 0;
  return 1;
}

int gc_card_dma(struct GcMachine *m, uint8_t *ram, uint32_t length, int to_card) {
  GcCard *card = card_of(m);
  uint32_t address;
  if (!card || !card->selected || !ram) return 0;
  if (card->command_length < 5u) return 0;
  address = card_address(card->command);

  if (!to_card && card->command[0] == GC_CARD_CMD_READ) {
    const uint32_t take = (length < GC_CARD_READ_SIZE) ? length : GC_CARD_READ_SIZE;
    if (address % GC_CARD_SECTOR == 0u || card->trace > 1) card_trace(m, card, "read", address);
    if (address >= card->bytes) return 0;
    memcpy(ram, card->image + address,
           (address + take <= card->bytes) ? take : (card->bytes - address));
    return 1;
  }
  if (to_card && card->command[0] == GC_CARD_CMD_WRITE) {
    const uint32_t take = (length < GC_CARD_PAGE) ? length : GC_CARD_PAGE;
    if (address % GC_CARD_SECTOR == 0u || card->trace > 1) card_trace(m, card, "program", address);
    if (address + take > card->bytes) return 0;
    /* Flash programming only clears bits, which is why the file system
     * erases a sector before it rewrites one. Modelling that is not just
     * pedantry: a write that sets bits would let a library bug that skips
     * the erase appear to work here and fail on hardware. */
    {
      uint32_t i, unerased = 0;
      for (i = 0; i < take; i++) {
        if (ram[i] & (uint8_t)~card->image[address + i]) unerased++;
        card->image[address + i] &= ram[i];
      }
      /* A page programmed over bits that were not erased keeps the old
       * zeroes and is not what the guest wrote; a library does not do this,
       * so under the trace it is worth saying. */
      if (unerased) card_trace(m, card, "program over unerased bits at", address);
    }
    card->dirty = 1;
    card->changed_cycles = m->cpu.cycles;
    card->completion_pending = 1;
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------ save states */

#include "gc_state.h"

/* Everything before the image. The image's size is whatever is left, which
 * is how a state holds a card of any size without a field of its own - and
 * why a state from before cards had sizes still loads. */
#define GC_CARD_STATE_HEAD (4u + 4u + 8u + 16u + 4u * 4u)

/* The card's image is part of the state, and so is the command it is in
 * the middle of. A game keeps the card's directory and allocation table in
 * its own memory once it has mounted it, so resuming against the image as
 * it is on disk now - after saves made since - would be resuming a game
 * that is wrong about its own card, and its next write would corrupt it.
 * The state's image is therefore put back, and written to the card's file.
 *
 * Whether the image still has to be written back, and since when, is the
 * host's business and not the guest's: a hash leaves both out. */
void gc_card_state_save(struct GcMachine *m, GcStateBuffer *b, int for_hash) {
  GcCard *card = card_of(m);
  gc_state_put_u32(b, card ? 1u : 0u);
  if (!card) return;
  gc_state_put_u32(b, for_hash ? 0u : (uint32_t)(card->dirty != 0));
  gc_state_put_u64(b, for_hash ? 0u : card->changed_cycles);
  gc_state_put(b, card->command, sizeof(card->command));
  gc_state_put_u32(b, card->command_length);
  gc_state_put_u32(b, (uint32_t)card->selected);
  gc_state_put_u32(b, card->status);
  gc_state_put_u32(b, (uint32_t)card->completion_pending);
  gc_state_put(b, card->image, card->bytes);
}

int gc_card_state_load(struct GcMachine *m, const uint8_t *payload, size_t size, int apply) {
  GcCard *card = card_of(m);
  uint32_t present, dirty = 0, length = 0, selected = 0, status = 0, pending = 0;
  uint64_t changed = 0;
  const uint8_t *p = payload;
  if (size < 4u) return -1;
  memcpy(&present, p, 4u);
  if (present > 1u) return -1;
  if (!present && size != 4u) return -1;
  if (present && (size < GC_CARD_STATE_HEAD || gc_card_megabits((uint32_t)(size - GC_CARD_STATE_HEAD)) == 0)) {
    return -1;
  }
  if (present) {
    memcpy(&dirty, p + 4u, 4u);
    memcpy(&changed, p + 8u, 8u);
    memcpy(&length, p + 32u, 4u);
    memcpy(&selected, p + 36u, 4u);
    memcpy(&status, p + 40u, 4u);
    memcpy(&pending, p + 44u, 4u);
    if (length > 16u || status > 0xffu) return -1;
  }
  if (!apply) return 0;
  if (!present) {
    /* Saved with the slot empty: it is empty again, and whatever was in it
     * is written back first. */
    gc_card_eject(m);
    return 0;
  }
  {
    const uint32_t bytes = (uint32_t)(size - GC_CARD_STATE_HEAD);
    if (!card) {
      /* A machine with no card (the headless runner) takes the state's card
       * as an image in memory with no file behind it. */
      card = (GcCard *)calloc(1, sizeof(*card));
      if (!card) return -1;
      card->bytes = 0;
      m->card = card;
    }
    if (card->path && card->image &&
        (card->bytes != bytes || memcmp(card->image, p + GC_CARD_STATE_HEAD, bytes) != 0)) {
      /* Putting the state's card back throws away every save made since the
       * state - which is right for the game, whose memory holds the state's
       * directory, and hard on whoever made them. The card as it was goes to
       * `<card>.bak` first, so a state loaded by mistake costs nothing that
       * cannot be copied back. */
      if (card_write(card, ".bak") != 0) {
        fprintf(stderr, "gc: cannot keep the memory card as it was in %s.bak\n", card->path);
      }
    }
    if (card->bytes != bytes) {
      /* The state's card is a different size from the one in the slot: the
       * slot takes the state's, as it takes its contents. */
      uint8_t *image = (uint8_t *)calloc(1, bytes);
      if (!image) return -1;
      free(card->image);
      card->image = image;
      card->bytes = bytes;
      dirty = 1;
    }
    if (memcmp(card->image, p + GC_CARD_STATE_HEAD, bytes) != 0) {
      memcpy(card->image, p + GC_CARD_STATE_HEAD, bytes);
      dirty = 1;
    }
  }
  card->dirty = dirty != 0;
  card->changed_cycles = changed;
  memcpy(card->command, p + 16u, sizeof(card->command));
  card->command_length = length;
  card->selected = selected != 0;
  card->status = (uint8_t)status;
  card->completion_pending = pending != 0;
  return 0;
}
