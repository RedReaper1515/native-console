/*
 * Save states, exactly: a program run straight through against the same
 * program saved part way and resumed from the file in a fresh machine.
 *
 *   gc_state_test --out DIR [--disc IMAGE] [--card] PROGRAM.elf...
 *   gc_state_test --out DIR [--disc IMAGE] [--card] --verify PROGRAM.elf...
 *
 * For each program the first form runs it straight to its end (or a cap on
 * the instruction count), saves states at a quarter, a half and three
 * quarters of the way - at instruction boundaries inside a field as well as
 * at field ends - and holds every resumed run to the straight one: the
 * instruction count, the program counter, the full state hash
 * (gc_state_hash: memory, ARAM, the framebuffer, every register and device)
 * and a hash of all the sound taken from the machine after the save point.
 * A run that saves must also end where the straight run did, which says
 * saving changes nothing. It then checks that a state is refused - and the
 * machine left alone - when it is damaged, cut short, or belongs to another
 * program, and writes what it expects for the second form, which a separate
 * CTest runs in a fresh process.
 *
 * A controller in port 1 changes every field, and with --card a memory card
 * is in slot 1, so both are carried by the states too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_audio.h"
#include "gc_card.h"
#include "gc_disc.h"
#include "gc_pad.h"
#include "gc_state.h"

#define CAP_INSTRUCTIONS 60000000u
#define POINTS 4u

static int failures;
static FILE *quiet; /* where the programs' own reports go */

static void check(int ok, const char *program, const char *what) {
  printf("%s %s: %s\n", ok ? "ok  " : "FAIL", program, what);
  if (!ok) failures++;
}

static void *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  void *data;
  long length;
  if (!file) return NULL;
  fseek(file, 0, SEEK_END);
  length = ftell(file);
  fseek(file, 0, SEEK_SET);
  data = length > 0 ? malloc((size_t)length) : NULL;
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size = (size_t)length;
  return data;
}

static int write_file(const char *path, const void *data, size_t size) {
  FILE *file = fopen(path, "wb");
  size_t written;
  if (!file) return -1;
  written = fwrite(data, 1, size, file);
  return (fclose(file) == 0 && written == size) ? 0 : -1;
}

typedef struct Options {
  const char *out;
  const char *disc_path;
  int card;
} Options;

typedef struct Program {
  const char *path;
  const char *name;
  uint8_t *image;
  size_t image_size;
  uint8_t *card_image; /* the blank card every machine starts with */
} Program;

typedef struct Booted {
  GcMachine *m;
  GcDisc *disc;
  GcStateIdentity id;
} Booted;

/* The controller for a field: something different every field, so a state
 * that lost the pads would show. */
static void pad_for(uint64_t field, GcPad *pad) {
  memset(pad, 0, sizeof(*pad));
  pad->present = 1;
  pad->buttons = (uint16_t)(((field * 2654435761u) >> 7) & 0x1f7fu);
  pad->stick_x = (int8_t)((field * 37u) & 0xffu);
  pad->stick_y = (int8_t)((field * 11u) & 0xffu);
  pad->trigger_l = (uint8_t)(field * 5u);
}

static int boot(const Options *o, const Program *p, const char *card_name, Booted *b) {
  uint32_t entry = 0;
  GcPad pad;
  memset(b, 0, sizeof(*b));
  b->m = gc_machine_create();
  if (!b->m) return -1;
  b->m->log = quiet ? quiet : stderr;
  pad_for(0, &pad);
  gc_pad_set(b->m, 0, &pad);
  gc_install_boot_state(b->m);
  if (o->disc_path) {
    b->disc = gc_disc_open(o->disc_path, stderr);
    if (!b->disc || gc_attach_disc(b->m, b->disc) != 0) return -1;
  }
  if (gc_load_elf(b->m, p->image, p->image_size, &entry) != 0 &&
      gc_load_dol(b->m, p->image, p->image_size, &entry) != 0) {
    return -1;
  }
  b->m->cpu.pc = entry;
  b->m->cpu.npc = entry + 4u;
  b->m->cpu.gpr[1] = 0x816ffff0u;
  b->m->cpu.gpr[2] = 0x81100000u;
  b->m->cpu.gpr[13] = 0x81100000u;
  if (o->card) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", o->out, card_name);
    if (write_file(path, p->card_image, GC_CARD_BYTES) != 0 || gc_card_insert(b->m, path) != 0) return -1;
  }
  gc_state_identity(&b->id, b->m, p->name, p->image, p->image_size);
  return 0;
}

static void shut(Booted *b) {
  gc_machine_destroy(b->m);
  gc_disc_close(b->disc);
  memset(b, 0, sizeof(*b));
}

/* How a run ended. */
typedef struct Ending {
  uint64_t cycles;
  uint32_t pc;
  uint64_t field;
  uint64_t hash;
  uint64_t audio; /* every frame taken after `audio_from` */
} Ending;

static int running(const GcMachine *m) {
  return !m->finished && !m->cpu.stopped && m->cpu.cycles < CAP_INSTRUCTIONS;
}

/* Runs to `until` instructions (or the end), a step at a time, handing over
 * a controller when a field begins and taking the sound when one ends, as a
 * frontend would. Returns at exactly `until` when it gets there. */
static void run_to(GcMachine *m, uint64_t until, uint64_t audio_from, uint64_t *audio) {
  while (running(m) && m->cpu.cycles < until) {
    const uint64_t field = m->field;
    gc_machine_step(m);
    if (m->field != field) {
      int16_t block[512 * 2];
      unsigned got;
      GcPad pad;
      while ((got = gc_audio_take(m, block, 512u)) > 0) {
        if (m->cpu.cycles > audio_from) {
          unsigned i;
          for (i = 0; i < got * 2u; i++) *audio = (*audio ^ (uint16_t)block[i]) * 0x100000001b3ull;
        }
      }
      pad_for(m->field, &pad);
      gc_pad_set(m, 0, &pad);
    }
  }
}

static void ending_of(GcMachine *m, uint64_t audio, Ending *e) {
  e->cycles = m->cpu.cycles;
  e->pc = m->cpu.pc;
  e->field = m->field;
  e->hash = gc_state_hash(m);
  e->audio = audio;
}

static int same(const Ending *a, const Ending *b) {
  return a->cycles == b->cycles && a->pc == b->pc && a->field == b->field && a->hash == b->hash &&
         a->audio == b->audio;
}

static void state_path(const Options *o, const Program *p, unsigned point, char *out, size_t size) {
  snprintf(out, size, "%s/%s-%u.state", o->out, p->name, point);
}

/* One resumed run from a state file, to the end. */
static int resume(const Options *o, const Program *p, const char *path, uint64_t point, Ending *e) {
  Booted b;
  char error[512];
  uint64_t audio = 0xcbf29ce484222325ull;
  if (boot(o, p, "resume.card", &b) != 0) return -1;
  if (gc_state_load_file(b.m, &b.id, path, error, sizeof(error)) != 0) {
    printf("     %s: %s\n", path, error);
    shut(&b);
    return -1;
  }
  if (b.m->cpu.cycles != point) {
    shut(&b);
    return -1;
  }
  run_to(b.m, CAP_INSTRUCTIONS, point, &audio);
  ending_of(b.m, audio, e);
  shut(&b);
  return 0;
}

static void test_program(const Options *o, Program *p) {
  Booted a, s;
  Ending straight, saver;
  uint64_t points[POINTS];
  uint64_t audio = 0xcbf29ce484222325ull;
  unsigned i;
  char path[1024], line[256];

  if (boot(o, p, "straight.card", &a) != 0) {
    check(0, p->name, "boots");
    return;
  }
  run_to(a.m, CAP_INSTRUCTIONS, 0, &audio);
  ending_of(a.m, 0, &straight);
  shut(&a);
  printf("     %s: straight run ends at instruction %llu, field %llu, pc %08x, hash %016llx\n", p->name,
         (unsigned long long)straight.cycles, (unsigned long long)straight.field, straight.pc,
         (unsigned long long)straight.hash);
  if (straight.cycles < 16u) {
    check(0, p->name, "runs long enough to save part way");
    return;
  }
  /* A quarter, a half and three quarters of the way, off any round number
   * so they fall inside fields; and the first field end, if there is one. */
  points[0] = straight.cycles / 4u + 3u;
  points[1] = straight.cycles / 2u + 1u;
  points[2] = straight.cycles / 4u * 3u + 7u;
  points[3] = straight.field ? GC_CYCLES_PER_FIELD : straight.cycles / 3u;

  /* The saving run, which must end where the straight one did. The sound it
   * hashes starts at each point in turn, so it is run once per point. */
  {
    Ending with_audio[POINTS];
    for (i = 0; i < POINTS; i++) {
      Booted b;
      uint64_t sound = 0xcbf29ce484222325ull;
      char error[512];
      if (boot(o, p, "saver.card", &b) != 0) {
        check(0, p->name, "boots for saving");
        return;
      }
      run_to(b.m, points[i], points[i], &sound);
      state_path(o, p, i, path, sizeof(path));
      check(b.m->cpu.cycles == points[i] && gc_state_save_file(b.m, &b.id, path, i % 2u, error, sizeof(error)) == 0,
            p->name, "saves a state part way");
      run_to(b.m, CAP_INSTRUCTIONS, points[i], &sound);
      ending_of(b.m, sound, &with_audio[i]);
      shut(&b);
      if (i == 0) {
        saver = with_audio[0];
        saver.audio = 0;
        check(same(&saver, &straight), p->name, "a run that saved ends exactly where the straight run did");
      }
    }
    for (i = 0; i < POINTS; i++) {
      Ending resumed;
      char what[160];
      state_path(o, p, i, path, sizeof(path));
      snprintf(what, sizeof(what), "resumed from instruction %llu (%s) ends identically, sound included",
               (unsigned long long)points[i], i % 2u ? "compressed" : "uncompressed");
      check(resume(o, p, path, points[i], &resumed) == 0 && same(&resumed, &with_audio[i]), p->name, what);
    }
    snprintf(path, sizeof(path), "%s/%s.expected", o->out, p->name);
    {
      FILE *file = fopen(path, "w");
      if (file) {
        for (i = 0; i < POINTS; i++) {
          fprintf(file, "%llu %llu %08x %llu %016llx %016llx\n", (unsigned long long)points[i],
                  (unsigned long long)with_audio[i].cycles, with_audio[i].pc,
                  (unsigned long long)with_audio[i].field, (unsigned long long)with_audio[i].hash,
                  (unsigned long long)with_audio[i].audio);
        }
        fclose(file);
      }
    }
  }

  /* Refusals: the machine must come through each of them untouched. */
  {
    size_t size = 0;
    uint8_t *bytes;
    char error[512];
    uint64_t before;
    GcStateIdentity other;
    /* The uncompressed state: a flipped byte in a compressed one can land in
     * a match that copies the same bytes from somewhere else, and load fine. */
    state_path(o, p, 0, path, sizeof(path));
    bytes = (uint8_t *)read_file(path, &size);
    if (!bytes || boot(o, p, "refuse.card", &s) != 0) {
      check(0, p->name, "reads its state back");
      free(bytes);
      return;
    }
    run_to(s.m, 1000u, 0, &audio);
    before = gc_state_hash(s.m);

    other = s.id;
    other.program_hash ^= 0x10u;
    check(gc_state_load_memory(s.m, &other, bytes, size, error, sizeof(error)) != 0 &&
              strstr(error, "different program") && gc_state_hash(s.m) == before,
          p->name, "a state from another program is refused");
    printf("     (%s)\n", error);
    other = s.id;
    snprintf(other.game_id, sizeof(other.game_id), "OTHER1");
    check(gc_state_load_memory(s.m, &other, bytes, size, error, sizeof(error)) != 0 && gc_state_hash(s.m) == before,
          p->name, "a state from another game is refused");
    printf("     (%s)\n", error);
    check(gc_state_load_memory(s.m, &s.id, bytes, size - 100u, error, sizeof(error)) != 0 &&
              gc_state_hash(s.m) == before,
          p->name, "a cut-short state is refused");
    printf("     (%s)\n", error);
    bytes[size - 1000u] ^= 0x5au;
    check(gc_state_load_memory(s.m, &s.id, bytes, size, error, sizeof(error)) != 0 && gc_state_hash(s.m) == before,
          p->name, "a damaged state is refused");
    printf("     (%s)\n", error);
    bytes[size - 1000u] ^= 0x5au;
    bytes[12] ^= 1u; /* the header's own size */
    check(gc_state_load_memory(s.m, &s.id, bytes, size, error, sizeof(error)) != 0 && gc_state_hash(s.m) == before,
          p->name, "a state with a damaged header is refused");
    printf("     (%s)\n", error);
    bytes[12] ^= 1u;
    check(gc_state_load_memory(s.m, &s.id, bytes, size, error, sizeof(error)) == 0 && gc_state_hash(s.m) != before,
          p->name, "and the undamaged state loads");
    shut(&s);
    free(bytes);
  }
  (void)line;
}

/* The second form: states written by another process, resumed here. */
static void verify_program(const Options *o, Program *p) {
  char path[1024];
  FILE *file;
  unsigned i;
  snprintf(path, sizeof(path), "%s/%s.expected", o->out, p->name);
  file = fopen(path, "r");
  if (!file) {
    check(0, p->name, "finds what the saving process expected");
    return;
  }
  for (i = 0; i < POINTS; i++) {
    unsigned long long point, cycles, field, hash, audio;
    unsigned pc;
    Ending want, got;
    char what[160];
    if (fscanf(file, "%llu %llu %x %llu %llx %llx", &point, &cycles, &pc, &field, &hash, &audio) != 6) {
      check(0, p->name, "reads what the saving process expected");
      break;
    }
    want.cycles = cycles;
    want.pc = pc;
    want.field = field;
    want.hash = hash;
    want.audio = audio;
    state_path(o, p, i, path, sizeof(path));
    snprintf(what, sizeof(what), "in a fresh process, resumed from instruction %llu ends identically", point);
    check(resume(o, p, path, point, &got) == 0 && same(&got, &want), p->name, what);
  }
  fclose(file);
}

int main(int argc, char **argv) {
  Options o;
  int verify = 0, i, programs = 0;
  memset(&o, 0, sizeof(o));
#if defined(_WIN32)
  quiet = fopen("NUL", "w");
#else
  quiet = fopen("/dev/null", "w");
#endif
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--out") && i + 1 < argc) o.out = argv[++i];
    else if (!strcmp(argv[i], "--disc") && i + 1 < argc) o.disc_path = argv[++i];
    else if (!strcmp(argv[i], "--card")) o.card = 1;
    else if (!strcmp(argv[i], "--verify")) verify = 1;
  }
  if (!o.out) {
    fprintf(stderr, "usage: gc_state_test --out DIR [--disc IMAGE] [--card] [--verify] PROGRAM.elf...\n");
    return 2;
  }
  for (i = 1; i < argc; i++) {
    Program p;
    const char *leaf, *c;
    char name[128];
    if (argv[i][0] == '-') {
      if (!strcmp(argv[i], "--out") || !strcmp(argv[i], "--disc")) i++;
      continue;
    }
    memset(&p, 0, sizeof(p));
    p.path = argv[i];
    leaf = p.path;
    for (c = p.path; *c; c++) {
      if (*c == '/' || *c == '\\') leaf = c + 1;
    }
    snprintf(name, sizeof(name), "%s", leaf);
    if (strrchr(name, '.')) *strrchr(name, '.') = 0;
    p.name = name;
    p.image = (uint8_t *)read_file(p.path, &p.image_size);
    if (!p.image) {
      /* Built by tools/gc/build_gc_programs.py, which needs devkitPPC. */
      printf("gc_state_test: %s not built; nothing to check\n", p.path);
      continue;
    }
    programs++;
    if (o.card) {
      /* One blank card, formatted once and copied to every machine: a card
       * is formatted with a serial number taken from its path. */
      char path[1024];
      GcMachine *m = gc_machine_create();
      size_t size = 0;
      snprintf(path, sizeof(path), "%s/%s-blank.card", o.out, p.name);
      if (!verify) remove(path);
      if (m) {
        gc_card_insert(m, path);
        gc_machine_destroy(m);
      }
      p.card_image = (uint8_t *)read_file(path, &size);
      if (!p.card_image || size != GC_CARD_BYTES) {
        check(0, p.name, "makes a blank memory card");
        free(p.image);
        continue;
      }
    }
    if (verify) verify_program(&o, &p);
    else test_program(&o, &p);
    free(p.card_image);
    free(p.image);
  }
  printf("gc_state_test: %s (%d program%s)\n", failures ? "FAIL" : "PASS", programs, programs == 1 ? "" : "s");
  return failures ? 1 : 0;
}
