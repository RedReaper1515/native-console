/*
 * The memory card as a commercial game's save library sees it.
 *
 * gc_cardtest (tests/gc/programs) saves through libogc, and libogc is
 * permissive: it never compares a card's header with the console. The retail
 * CARD library every commercial game links does, on every mount - and that
 * difference is how a card that passed every test program was called
 * "corrupted and needs to be formatted" by the first game asked to save on
 * it (Shrek 2; docs/GC-PLAN.md, "Memory card: a real game's saves").
 *
 * So this drives the card through the expansion-bus registers the way the
 * retail library does and applies the retail library's mount checks, as
 * described in docs/gc-memory-card.md and GC-PLAN.md:
 *
 *  - the identifier and the unlocked status bit;
 *  - the console's settings memory: slot A's flash-ID checksum, and the
 *    header serial against that flash ID plus the generator seeded with the
 *    format time (the check libogc does not make);
 *  - the header, directory and allocation-table checksums, the free count.
 *
 * Then it saves the way the library does - erase a sector, program it a page
 * at a time, update the older directory and table copies - checking that each
 * erase and program ends with the card's interrupt, puts the machine down,
 * picks a fresh one up against the same file and checks the mount and the
 * saved bytes again. Around that: a card formatted elsewhere (another size,
 * another console's flash ID, a real format time) mounts; a file that is not
 * a card is refused and left alone; a card held in memory only writes
 * nothing; write-back leaves no temporary file behind; and loading a save
 * state, which puts the state's card back, keeps the card it replaced in
 * `<card>.bak`.
 *
 *   gc_card_test DIRECTORY
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_card.h"
#include "gc_state.h"

static unsigned failures;

#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      failures++;                                                                                  \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
    }                                                                                              \
  } while (0)

/* ------------------------------------------------------------ the bus */

#define EXI 0x6800u
#define WORK 0x00100000u /* physical main memory the transfers use */

static void csr(GcMachine *m, uint32_t value) { gc_io_write(m, EXI + 0x00, 4, value); }
static uint32_t csr_read(GcMachine *m) { return gc_io_read(m, EXI + 0x00, 4); }

static void select_card(GcMachine *m) { csr(m, 0x80u | (5u << 4)); } /* device 0, 16 MHz */
static void select_settings(GcMachine *m) { csr(m, 0x100u | (3u << 4)); } /* device 1 */
static void deselect(GcMachine *m) { csr(m, 0); }

static void imm_write(GcMachine *m, const uint8_t *bytes, unsigned length) {
  unsigned done = 0;
  while (done < length) {
    unsigned piece = length - done > 4u ? 4u : length - done, i;
    uint32_t word = 0;
    for (i = 0; i < piece; i++) word |= (uint32_t)bytes[done + i] << ((3u - i) * 8u);
    gc_io_write(m, EXI + 0x10, 4, word);
    gc_io_write(m, EXI + 0x0c, 4, ((piece - 1u) << 4) | (1u << 2) | 1u);
    done += piece;
  }
}

static uint32_t imm_read(GcMachine *m, unsigned length) {
  gc_io_write(m, EXI + 0x0c, 4, ((length - 1u) << 4) | 1u);
  return gc_io_read(m, EXI + 0x10, 4);
}

static uint8_t *dma(GcMachine *m, uint32_t length, int write) {
  gc_io_write(m, EXI + 0x04, 4, WORK);
  gc_io_write(m, EXI + 0x08, 4, length);
  gc_io_write(m, EXI + 0x0c, 4, ((write ? 1u : 0u) << 2) | 3u);
  return gc_machine_ram(m, WORK, length);
}

static void pack_address(uint8_t *command, uint32_t address) {
  command[1] = (uint8_t)((address >> 17) & 0x7f);
  command[2] = (uint8_t)((address >> 9) & 0xff);
  command[3] = (uint8_t)((address >> 7) & 0x03);
  command[4] = (uint8_t)(address & 0x7f);
}

/* The card's own interrupt, which a program and an erase end with. Returns
 * whether it was raised, and acknowledges it. */
static int card_interrupt(GcMachine *m) {
  const int raised = (csr_read(m) & 0x0002u) != 0;
  csr(m, 0x0002u);
  return raised;
}

static void card_read(GcMachine *m, uint32_t address, uint8_t *out, uint32_t length) {
  uint32_t done;
  for (done = 0; done < length; done += 512u) {
    uint8_t command[5] = {0x52};
    const uint8_t latency[4] = {0};
    pack_address(command, address + done);
    select_card(m);
    imm_write(m, command, 5);
    imm_write(m, latency, 4);
    memcpy(out + done, dma(m, 512u, 0), 512u);
    deselect(m);
  }
}

static void card_erase(GcMachine *m, uint32_t sector) {
  uint8_t command[5] = {0xf1};
  pack_address(command, sector);
  select_card(m);
  imm_write(m, command, 3);
  deselect(m);
  CHECK(card_interrupt(m), "no interrupt after erasing %06x", sector);
}

static void card_program(GcMachine *m, uint32_t address, const uint8_t *data, uint32_t length) {
  uint32_t done;
  for (done = 0; done < length; done += 128u) {
    uint8_t command[5] = {0xf2};
    uint8_t *ram;
    pack_address(command, address + done);
    select_card(m);
    imm_write(m, command, 5);
    ram = gc_machine_ram(m, WORK, 128u);
    memcpy(ram, data + done, 128u);
    dma(m, 128u, 1);
    deselect(m);
    if (!card_interrupt(m)) {
      CHECK(0, "no interrupt after programming %06x", address + done);
      return;
    }
  }
}

static void settings(GcMachine *m, uint8_t out[64]) {
  const uint8_t command[4] = {0x20, 0x00, 0x01, 0x00};
  select_settings(m);
  imm_write(m, command, 4);
  memcpy(out, dma(m, 64u, 0), 64u);
  deselect(m);
}

/* --------------------------------------------------- the retail checks */

#define SECTOR 0x2000u

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void checksum(const uint8_t *p, unsigned bytes, uint16_t *a, uint16_t *b) {
  unsigned i;
  *a = *b = 0;
  for (i = 0; i + 1 < bytes; i += 2) {
    const uint16_t w = be16(p + i);
    *a = (uint16_t)(*a + w);
    *b = (uint16_t)(*b + (uint16_t)~w);
  }
  if (*a == 0xffff) *a = 0;
  if (*b == 0xffff) *b = 0;
}

/* The generator the retail library runs over a signed 64-bit format time. */
static int64_t lcg(int64_t v) {
  const uint64_t n = (uint64_t)v * 1103515245ull + 12345ull;
  return (n & 0x8000000000000000ull) ? (int64_t)~(~n >> 16) : (int64_t)(n >> 16);
}

typedef struct Mounted {
  uint32_t megabits;
  uint8_t system[5 * SECTOR];
  int current_dir; /* index (1 or 2) of the newer directory copy */
  int current_fat; /* 3 or 4 */
} Mounted;

/* Mount the card as the retail library does. Returns 0 when it would, and
 * says why not otherwise. */
static int mount(GcMachine *m, Mounted *out, const char *label) {
  uint8_t sram[64], *h;
  uint32_t id, status;
  uint16_t a, b;
  int64_t rand;
  unsigned i, sum = 0;
  const unsigned before = failures;

  select_card(m);
  { const uint8_t zero[2] = {0, 0}; imm_write(m, zero, 2); }
  id = imm_read(m, 4);
  deselect(m);
  CHECK((id & ~0xfcu) == 0 && gc_card_megabits(id * (1u << 17)) == id, "%s: identifier %08x", label, id);
  out->megabits = id;

  select_card(m);
  { const uint8_t command[2] = {0x83, 0}; imm_write(m, command, 2); }
  status = imm_read(m, 1) >> 24;
  deselect(m);
  CHECK((status & 0x40u) && !(status & 0x18u), "%s: status %02x (want unlocked, no errors)", label, status);

  settings(m, sram);
  for (i = 0; i < 12; i++) sum += sram[0x14 + i];
  CHECK(sram[0x3a] == (uint8_t)~sum, "%s: slot A flash-ID checksum %02x, want %02x", label, sram[0x3a],
        (uint8_t)~sum);

  card_read(m, 0, out->system, 5 * SECTOR);
  h = out->system;
  CHECK(be16(h + 0x20) == 0 && be16(h + 0x22) == id, "%s: header device %u size %u", label, be16(h + 0x20),
        be16(h + 0x22));
  checksum(h, 0x1fc, &a, &b);
  CHECK(a == be16(h + 0x1fc) && b == be16(h + 0x1fe), "%s: header checksum", label);
  rand = 0;
  for (i = 0; i < 8; i++) rand = (int64_t)(((uint64_t)rand << 8) | h[0x0c + i]);
  for (i = 0; i < 12; i++) {
    rand = lcg(rand);
    if (h[i] != (uint8_t)(sram[0x14 + i] + (uint8_t)rand)) {
      CHECK(0, "%s: header serial byte %u is %02x, the console's flash ID says %02x - the retail library "
               "calls this card corrupted", label, i, h[i], (uint8_t)(sram[0x14 + i] + (uint8_t)rand));
      break;
    }
    rand = lcg(rand) & 0x7fff;
  }
  CHECK(be16(h + 0x24) == 0, "%s: encoding %u", label, be16(h + 0x24));

  {
    int ok[2];
    for (i = 0; i < 2; i++) {
      const uint8_t *d = out->system + (1 + i) * SECTOR;
      checksum(d, 0x1ffc, &a, &b);
      ok[i] = a == be16(d + 0x1ffc) && b == be16(d + 0x1ffe);
      CHECK(ok[i], "%s: directory copy %u checksum", label, i + 1);
    }
    /* The generation counters are signed in the retail library, so a
     * formatted directory's 0xffff is -1 and its first update, 0, is newer. */
    out->current_dir =
        (int16_t)be16(out->system + SECTOR + 0x1ffa) > (int16_t)be16(out->system + 2 * SECTOR + 0x1ffa) ? 1 : 2;
    for (i = 0; i < 2; i++) {
      const uint8_t *f = out->system + (3 + i) * SECTOR;
      const unsigned blocks = id * 16u;
      unsigned k, free_blocks = 0;
      checksum(f + 4, 0x1ffc, &a, &b);
      CHECK(a == be16(f) && b == be16(f + 2), "%s: allocation table copy %u checksum", label, i + 1);
      for (k = 5; k < blocks; k++) free_blocks += be16(f + 2u * k) == 0;
      CHECK(free_blocks == be16(f + 6), "%s: table copy %u counts %u free, says %u", label, i + 1, free_blocks,
            be16(f + 6));
    }
    out->current_fat =
        (int16_t)be16(out->system + 3 * SECTOR + 4) > (int16_t)be16(out->system + 4 * SECTOR + 4) ? 3 : 4;
  }
  return failures == before ? 0 : -1;
}

/* One save the way the library makes one: the data sector erased and
 * programmed, then the older allocation table and the older directory
 * rewritten as the newer ones. */
static void save_file(GcMachine *m, Mounted *mt, uint16_t block, const uint8_t *data) {
  uint8_t sector[SECTOR];
  const int dir = mt->current_dir == 1 ? 2 : 1, fat = mt->current_fat == 3 ? 4 : 3;
  uint8_t *d = mt->system + dir * SECTOR, *f = mt->system + fat * SECTOR;
  uint16_t a, b;

  card_erase(m, block * SECTOR);
  card_program(m, block * SECTOR, data, SECTOR);

  memcpy(f, mt->system + mt->current_fat * SECTOR, SECTOR);
  put16(f + 2u * block, 0xffff);
  put16(f + 4, (uint16_t)(be16(f + 4) + 1));
  put16(f + 6, (uint16_t)(be16(f + 6) - 1));
  put16(f + 8, block);
  checksum(f + 4, 0x1ffc, &a, &b);
  put16(f, a);
  put16(f + 2, b);
  memcpy(sector, f, SECTOR);
  card_erase(m, (uint32_t)fat * SECTOR);
  card_program(m, (uint32_t)fat * SECTOR, sector, SECTOR);

  memcpy(d, mt->system + mt->current_dir * SECTOR, SECTOR);
  memcpy(d, "GTST01", 6);
  memset(d + 8, 0, 32);
  memcpy(d + 8, "card test", 9);
  put16(d + 0x36, block);
  put16(d + 0x38, 1);
  put16(d + 0x1ffa, (uint16_t)(be16(d + 0x1ffa) + 1));
  checksum(d, 0x1ffc, &a, &b);
  put16(d + 0x1ffc, a);
  put16(d + 0x1ffe, b);
  memcpy(sector, d, SECTOR);
  card_erase(m, (uint32_t)dir * SECTOR);
  card_program(m, (uint32_t)dir * SECTOR, sector, SECTOR);
}

/* ---------------------------------------------------------- the cases */

static long file_size(const char *path) {
  FILE *f = fopen(path, "rb");
  long size;
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fclose(f);
  return size;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  if (fwrite(data, 1, size, f) != size) {
    fclose(f);
    return -1;
  }
  return fclose(f);
}

/* A new card: mounts, takes a save, and the save is there after a restart. */
static void fresh_card_survives_restart(const char *dir) {
  char path[1024], temporary[1100];
  uint8_t data[SECTOR], back[SECTOR];
  Mounted mt;
  GcMachine *m;
  unsigned i;

  snprintf(path, sizeof path, "%s/fresh.card", dir);
  snprintf(temporary, sizeof temporary, "%s.tmp", path);
  remove(path);
  for (i = 0; i < SECTOR; i++) data[i] = (uint8_t)(i * 7u + 3u);

  m = gc_machine_create();
  CHECK(gc_card_insert(m, path) == 0, "a new card at %s", path);
  CHECK(file_size(path) == 0x80000, "the new card is on disk at 524288 bytes (%ld)", file_size(path));
  if (mount(m, &mt, "new card") == 0) {
    CHECK(mt.megabits == 4, "a new card is 4 Mbit (%u)", mt.megabits);
    save_file(m, &mt, 5, data);
  }
  gc_machine_destroy(m); /* ejects, writing the card back */
  CHECK(file_size(temporary) < 0, "write-back left %s behind", temporary);

  m = gc_machine_create();
  CHECK(gc_card_insert(m, path) == 0, "the saved card back in");
  if (mount(m, &mt, "saved card after a restart") == 0) {
    const uint8_t *d = mt.system + mt.current_dir * SECTOR;
    CHECK(memcmp(d, "GTST01", 6) == 0, "the saved file's directory entry is on the card");
    card_read(m, 5 * SECTOR, back, SECTOR);
    CHECK(memcmp(back, data, SECTOR) == 0, "the saved file's data is on the card");
  }
  gc_machine_destroy(m);
}

/* A card formatted somewhere else: a 16 Mbit image with another console's
 * flash ID and a real format time, as the retail library's format leaves it. */
static void foreign_card_mounts(const char *dir) {
  const uint32_t bytes = 0x200000u, blocks = 256u;
  static const uint8_t flash[12] = {0x13, 0x57, 0x9b, 0xdf, 0x24, 0x68, 0xac, 0xe0, 0x11, 0x22, 0x33, 0x44};
  const int64_t format_time = 0x0123456789abcdefll;
  uint8_t *image = (uint8_t *)malloc(bytes);
  char path[1024];
  uint16_t a, b;
  int64_t rand = format_time;
  unsigned i, copy;
  Mounted mt;
  GcMachine *m;

  if (!image) return;
  memset(image, 0xff, bytes);
  for (i = 0; i < 8; i++) image[0x0c + i] = (uint8_t)(format_time >> (56 - 8 * i));
  for (i = 0; i < 12; i++) {
    rand = lcg(rand);
    image[i] = (uint8_t)(flash[i] + (uint8_t)rand);
    rand = lcg(rand) & 0x7fff;
  }
  memset(image + 0x14, 0, 0x12);
  put16(image + 0x22, 16);
  checksum(image, 0x1fc, &a, &b);
  put16(image + 0x1fc, a);
  put16(image + 0x1fe, b);
  for (copy = 0; copy < 2; copy++) {
    uint8_t *d = image + (1 + copy) * SECTOR, *f = image + (3 + copy) * SECTOR;
    checksum(d, 0x1ffc, &a, &b);
    put16(d + 0x1ffc, a);
    put16(d + 0x1ffe, b);
    memset(f, 0, SECTOR);
    put16(f + 4, (uint16_t)copy);
    put16(f + 6, (uint16_t)(blocks - 5));
    put16(f + 8, 4);
    checksum(f + 4, 0x1ffc, &a, &b);
    put16(f, a);
    put16(f + 2, b);
  }
  snprintf(path, sizeof path, "%s/foreign16.card", dir);
  CHECK(write_file(path, image, bytes) == 0, "write %s", path);
  m = gc_machine_create();
  CHECK(gc_card_insert(m, path) == 0, "a 16 Mbit card goes in");
  if (mount(m, &mt, "16 Mbit card from another console") == 0) {
    uint8_t data[SECTOR];
    CHECK(mt.megabits == 16, "it reports 16 Mbit (%u)", mt.megabits);
    memset(data, 0x5a, sizeof data);
    save_file(m, &mt, 200, data); /* a block a 4 Mbit card does not have */
  }
  gc_machine_destroy(m);
  CHECK(file_size(path) == (long)bytes, "the 16 Mbit card stays 16 Mbit (%ld)", file_size(path));
  m = gc_machine_create();
  CHECK(gc_card_insert(m, path) == 0, "the 16 Mbit card back in");
  if (mount(m, &mt, "16 Mbit card after a restart") == 0) {
    uint8_t back[SECTOR];
    card_read(m, 200 * SECTOR, back, SECTOR);
    CHECK(back[0] == 0x5a && back[SECTOR - 1] == 0x5a, "block 200 kept its save");
  }
  gc_machine_destroy(m);
  free(image);
}

/* A file that is not a card is refused and not touched; a card in memory
 * only is never written. */
static void refusals(const char *dir) {
  char path[1024];
  uint8_t junk[100000];
  GcMachine *m;
  Mounted mt;

  snprintf(path, sizeof path, "%s/short.card", dir);
  memset(junk, 0x33, sizeof junk);
  CHECK(write_file(path, junk, sizeof junk) == 0, "write %s", path);
  m = gc_machine_create();
  CHECK(gc_card_insert(m, path) != 0, "a 100000-byte file is not a card");
  CHECK(!gc_card_present(m), "and the slot stays empty");
  gc_machine_destroy(m);
  CHECK(file_size(path) == (long)sizeof junk, "and the file is left alone (%ld)", file_size(path));

  snprintf(path, sizeof path, "%s/unbacked.card", dir);
  remove(path);
  m = gc_machine_create();
  CHECK(gc_card_insert_unbacked(m, path) == 0, "a card in memory only");
  if (mount(m, &mt, "card in memory only") == 0) {
    uint8_t data[SECTOR];
    memset(data, 1, sizeof data);
    save_file(m, &mt, 6, data);
  }
  gc_machine_destroy(m);
  CHECK(file_size(path) < 0, "a card in memory only wrote %s", path);
}

/* A state holds the card, so loading one puts the card back as it was - and
 * the saves made since go to `<card>.bak` rather than nowhere. */
static void state_load_keeps_a_backup(const char *dir) {
  char path[1024], backup[1100];
  GcStateIdentity id;
  GcStateBuffer state;
  uint8_t data[SECTOR];
  char error[256];
  Mounted mt;
  GcMachine *m;
  FILE *f;

  snprintf(path, sizeof path, "%s/stated.card", dir);
  snprintf(backup, sizeof backup, "%s.bak", path);
  remove(path);
  remove(backup);
  memset(&state, 0, sizeof state);
  memset(data, 0x77, sizeof data);

  m = gc_machine_create();
  CHECK(gc_card_insert(m, path) == 0, "a card for the state");
  gc_state_identity(&id, m, "card_test", NULL, 0);
  CHECK(gc_state_save_memory(m, &id, 0, &state) == 0, "a state with a blank card in it");
  if (mount(m, &mt, "card before the state is loaded") == 0) save_file(m, &mt, 5, data);
  CHECK(gc_state_load_memory(m, &id, state.data, state.size, error, sizeof error) == 0, "load the state: %s",
        error);
  gc_machine_destroy(m); /* writes the state's card back */

  m = gc_machine_create();
  CHECK(gc_card_insert(m, path) == 0, "the card after the state");
  if (mount(m, &mt, "card after the state") == 0) {
    CHECK(memcmp(mt.system + mt.current_dir * SECTOR, "GTST01", 6) != 0,
          "loading the state put the card back as it was in the state");
  }
  gc_machine_destroy(m);

  f = fopen(backup, "rb");
  CHECK(f != NULL, "the card as it was before the state is kept in %s", backup);
  if (f) {
    fclose(f);
    m = gc_machine_create();
    CHECK(gc_card_insert_unbacked(m, backup) == 0, "the backup is a card");
    if (mount(m, &mt, "the backup") == 0) {
      CHECK(memcmp(mt.system + mt.current_dir * SECTOR, "GTST01", 6) == 0, "the backup has the save");
    }
    gc_machine_destroy(m);
  }
  gc_state_buffer_free(&state);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: gc_card_test DIRECTORY\n");
    return 2;
  }
  fresh_card_survives_restart(argv[1]);
  foreign_card_mounts(argv[1]);
  refusals(argv[1]);
  state_load_keeps_a_backup(argv[1]);
  if (failures) {
    printf("gc_card_test: %u failures\n", failures);
    return 1;
  }
  printf("gc_card_test: a new card, a foreign 16 Mbit card and a card in memory mount as the retail library "
         "mounts them, take a save and keep it across a restart; a state's card keeps the one it replaced\n");
  return 0;
}
