/*
 * The headless runner every generated GameCube program carries. See
 * gc_runner.h for the options.
 */
#include "gc_runner.h"
#include "wii.h"
#include "wii_nand.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include "gc.h"
#include "gc_audio.h"
#include "gc_ax.h"
#include "gc_card.h"
#include "gc_coverage.h"
#include "gc_differential.h"
#include "gc_disc.h"
#include "gc_efb.h"
#include "gc_gl.h"
#include "gc_gx.h"
#include "gc_pad.h"
#include "gc_perf.h"
#include "gc_state.h"
#include "gc_zelda.h"
#include <time.h>

/* The disc content's name, beside this executable - wherever it was
 * started from, because the hub starts a game from its own folder and a
 * shortcut may start it from anywhere. */
static int beside_executable(char *out, size_t size, const char *argv0, const char *name) {
  size_t length = 0, cut = 0, i;
#if defined(_WIN32)
  length = GetModuleFileNameA(NULL, out, (DWORD)size);
  if (length >= size) length = 0;
#elif defined(__APPLE__)
  uint32_t capacity = (uint32_t)size;
  if (_NSGetExecutablePath(out, &capacity) == 0) length = strlen(out);
#else
  {
    const ssize_t got = readlink("/proc/self/exe", out, size - 1u);
    if (got > 0) length = (size_t)got;
  }
#endif
  if (!length && argv0) {
    length = strlen(argv0);
    if (length >= size) return -1;
    memcpy(out, argv0, length);
  }
  out[length] = 0;
  for (i = 0; i < length; i++) {
    if (out[i] == '/' || out[i] == '\\') cut = i + 1u;
  }
  if (cut + strlen(name) + 1u > size) return -1;
  strcpy(out + cut, name);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */

typedef struct GcRunner {
  const GcRunnerProgram *program;
  const char *disc_path;
  const char *frame_path;
  const char *trace_path;
  uint64_t frame_every;
  uint64_t frame_from; /* --dump-frame-from: no numbered frame before this field */
  int wii_aspect;      /* --wii-aspect: 0 leaves the console's setting; 1 is 4:3, 2 is 16:9 */
  uint64_t frames;
  int interpret;
  int differential;
  int coverage;
  int auto_input;
  GcInputScript *script;
  int is_elf;
  /* What the machine played, and what the sound microcode did, a field at a
   * time. Headless the machine runs in its own time rather than the host's,
   * so a sound that is wrong here is wrong in the machine - not a host that
   * fell behind and let the audio engine repeat its last buffer. */
  FILE *audio;
  FILE *audio_log;
  /* Save states: where to write one and at which field, where to start
   * from, and whether to print the state's hashes at the end. */
  const char *save_path;
  uint64_t save_at[16];
  unsigned save_count;
  int save_raw;
  const char *load_path;
  int state_hash;
  /* The OpenGL path instead of the reference rasteriser (gc_gl.h). */
  int use_gl;
  GcGlOptions gl;
  /* --field-log: a line a field of where the time went (gc_perf.h). */
  int field_log;
  /* --dump-memory FILE: main memory as the run left it. */
  const char *memory_path;
  /* --watch ADDRESS: every write to that address goes in the log. */
  uint32_t watch;
  /* --card FILE: a memory card in slot A, backed by FILE on the machine
   * being run and held in memory on every other (a differential's
   * interpreter, the locate passes). Without it the slot is empty. */
  const char *card_path;
  /* An ELF to take symbols from, for a test program that boots off a disc
   * and so carries none of its own (--symbols). */
  const char *symbols_path;
  /* A Wii's flash memory (--nand): like the card, none unless named, and
   * only the reporting machine writes it. */
  const char *nand_path;
} GcRunner;

/* ------------------------------------------------------------------ */
/* Where the recompiled code spends its time                           */

/* The machine's own profiler samples the program counter, and only the
 * interpreter keeps one: recompiled code is native, and its guest pc exists
 * at block boundaries and not between them. So this samples the host
 * instead - suspend the thread running the machine, read its instruction
 * pointer, let it go - and maps that back to a guest address afterwards,
 * through the block table the program already hands the runner
 * (GcRunnerProgram.blocks: each block's guest address and the function it
 * was compiled to).
 *
 * The mapping is the greatest block entry at or below the sample. A block's
 * tail parts (GC_PART) are separate functions that are not in the table and
 * land on the block they belong to, which is what one wants. A runtime
 * function that the linker happened to place between two blocks is
 * attributed to the earlier one, which is the one inaccuracy here; samples
 * outside the blocks' range altogether are counted apart, and that split -
 * guest code against everything else - is the first thing to read.
 *
 * Windows only, because suspending a thread and reading its context is.
 * Everywhere else it is a no-op that says so. */
#if defined(_WIN32)
#define GC_GUEST_PROFILE_MAX (1u << 22)
/* How far a sample may sit past a block's entry and still be that block.
 * Blocks are chained in parts of a few instructions each, so their
 * functions are small; this is generous by a wide margin and still tight
 * enough to keep code that is not in the table out. */
#define GC_GUEST_PROFILE_REACH 8192u

typedef struct GuestProfile {
  HANDLE thread;      /* the machine's thread, duplicated */
  HANDLE sampler;
  volatile LONG stop;
  unsigned hz;
  uint64_t *sample;
  size_t samples;
  uint64_t suspends_failed;
} GuestProfile;

static DWORD WINAPI guest_profile_main(LPVOID argument) {
  GuestProfile *p = (GuestProfile *)argument;
  /* Sleep(1) is fifteen milliseconds on a machine nobody has asked for
   * better, which samples at sixty hertz and not a thousand. The period is
   * waited out on the clock instead, which costs this thread a core and
   * buys the rate that was asked for. */
  LARGE_INTEGER frequency, next;
  const double period_s = 1.0 / (double)(p->hz ? p->hz : 1000u);
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&next);
  while (!InterlockedCompareExchange(&p->stop, 0, 0)) {
    CONTEXT context;
    LARGE_INTEGER now;
    next.QuadPart += (LONGLONG)(period_s * (double)frequency.QuadPart);
    for (;;) {
      QueryPerformanceCounter(&now);
      if (now.QuadPart >= next.QuadPart) break;
      if ((double)(next.QuadPart - now.QuadPart) / (double)frequency.QuadPart > 0.002) Sleep(1);
      else YieldProcessor();
    }
    if (SuspendThread(p->thread) == (DWORD)-1) {
      p->suspends_failed++;
      continue;
    }
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(p->thread, &context) && p->samples < GC_GUEST_PROFILE_MAX) {
      p->sample[p->samples++] = (uint64_t)context.Rip;
    }
    ResumeThread(p->thread);
  }
  return 0;
}

static GuestProfile *guest_profile_start(unsigned hz) {
  GuestProfile *p = (GuestProfile *)calloc(1, sizeof(GuestProfile));
  if (!p) return NULL;
  p->hz = hz ? hz : 1000u;
  p->sample = (uint64_t *)malloc(sizeof(uint64_t) * GC_GUEST_PROFILE_MAX);
  if (!p->sample ||
      !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &p->thread, 0,
                       FALSE, DUPLICATE_SAME_ACCESS)) {
    free(p->sample);
    free(p);
    return NULL;
  }
  p->sampler = CreateThread(NULL, 0, guest_profile_main, p, 0, NULL);
  if (!p->sampler) {
    CloseHandle(p->thread);
    free(p->sample);
    free(p);
    return NULL;
  }
  return p;
}

typedef struct GuestProfileEntry {
  uint64_t at;      /* the block's function */
  uint32_t address; /* and the guest address it came from */
  uint64_t samples;
} GuestProfileEntry;

static int by_host(const void *x, const void *y) {
  const GuestProfileEntry *a = (const GuestProfileEntry *)x, *b = (const GuestProfileEntry *)y;
  return a->at < b->at ? -1 : a->at > b->at ? 1 : 0;
}

static int by_samples(const void *x, const void *y) {
  const GuestProfileEntry *a = (const GuestProfileEntry *)x, *b = (const GuestProfileEntry *)y;
  return a->samples > b->samples ? -1 : a->samples < b->samples ? 1 : 0;
}

static void guest_profile_report(GuestProfile *p, const GcRunnerProgram *program, FILE *out) {
  GuestProfileEntry *entry;
  uint64_t outside = 0;
  unsigned i, shown;
  size_t s;

  InterlockedExchange(&p->stop, 1);
  WaitForSingleObject(p->sampler, 2000);
  CloseHandle(p->sampler);
  CloseHandle(p->thread);

  {
    /* GC_GUEST_PROFILE_RAW=<file>: every sample as it was taken - the
     * program's base address, then the instruction pointers, eight bytes
     * each - for a link map to name the runtime's own functions by, which
     * the block table cannot. */
    const char *raw_path = getenv("GC_GUEST_PROFILE_RAW");
    FILE *raw = raw_path && *raw_path ? fopen(raw_path, "wb") : NULL;
    if (raw) {
      const uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
      fwrite(&base, sizeof(base), 1, raw);
      fwrite(p->sample, sizeof(uint64_t), p->samples, raw);
      fclose(raw);
    }
  }

  if (!program || !program->blocks || !program->block_count) {
    fprintf(out, "%s: guest profile: %llu samples, and no block table to place them in\n",
            program ? program->name : "gc", (unsigned long long)p->samples);
    free(p->sample);
    free(p);
    return;
  }
  entry = (GuestProfileEntry *)calloc(program->block_count, sizeof(GuestProfileEntry));
  if (!entry) return;
  for (i = 0; i < program->block_count; i++) {
    entry[i].at = (uint64_t)(uintptr_t)program->blocks[i].run;
    entry[i].address = program->blocks[i].address;
  }
  qsort(entry, program->block_count, sizeof(*entry), by_host);

  for (s = 0; s < p->samples; s++) {
    const uint64_t rip = p->sample[s];
    unsigned low = 0, high = program->block_count;
    if (rip < entry[0].at) {
      outside++;
      continue;
    }
    while (high - low > 1u) {
      const unsigned middle = low + (high - low) / 2u;
      if (entry[middle].at <= rip) low = middle;
      else high = middle;
    }
    /* Further past a block's entry than any one block's function could
     * reach. The rule here - the nearest entry at or below - cannot see
     * where a function ends, so without this every byte above the highest
     * entry in the table lands on it, and the highest entry happened to be
     * the last function of its object file. That put the whole runtime, and
     * every system library above it, on one nine-instruction epilogue and
     * called it a fifth of the run. */
    if (rip - entry[low].at > GC_GUEST_PROFILE_REACH) {
      outside++;
      continue;
    }
    entry[low].samples++;
  }

  fprintf(out, "%s: guest profile: %llu samples at %u Hz; %llu in recompiled code (%.1f%%), %llu elsewhere\n",
          program->name, (unsigned long long)p->samples, p->hz,
          (unsigned long long)(p->samples - outside),
          p->samples ? 100.0 * (double)(p->samples - outside) / (double)p->samples : 0.0,
          (unsigned long long)outside);
  qsort(entry, program->block_count, sizeof(*entry), by_samples);
  shown = 0;
  for (i = 0; i < program->block_count && shown < 20u; i++) {
    if (!entry[i].samples) break;
    fprintf(out, "%s:   guest %08x  %llu samples  %.1f%%\n", program->name, entry[i].address,
            (unsigned long long)entry[i].samples,
            p->samples ? 100.0 * (double)entry[i].samples / (double)p->samples : 0.0);
    shown++;
  }
  free(entry);
  free(p->sample);
  free(p);
}
#endif

/* The frames the audio engine produced since the last call: raw 16-bit
 * stereo, little-endian, left then right, at the engine's rate - 32 kHz for
 * everything AX plays. */
static void drain_audio(GcRunner *r, GcMachine *m) {
  int16_t block[4096 * 2];
  unsigned got;
  if (r->audio) {
    while ((got = gc_audio_take(m, block, 4096u)) > 0) fwrite(block, 4u, got, r->audio);
  }
  if (r->audio_log) {
    fprintf(r->audio_log, "%llu %llu %llu %llu %llu %llu\n", (unsigned long long)m->field,
            (unsigned long long)gc_ax_lists(m), (unsigned long long)gc_ax_bad_lists(m),
            (unsigned long long)gc_ax_voices(m), (unsigned long long)gc_ax_voices_running(m),
            (unsigned long long)gc_audio_overruns(m));
  }
}

/* One press every sixty fields from field 600 (ten seconds - a disc spends
 * those spinning up): for the first twenty presses START and then A three
 * times, a few fields each, which is what gets past logos, a title screen,
 * a "press start", the menus a new game asks about and a skippable intro.
 * After that a cycle that walks the stick round and uses the other buttons
 * but never START, which would pause, holding a direction for most of its
 * slot. Nothing in it knows about any game. */
void gc_auto_input(uint64_t field, GcPad *pad) {
  static const struct {
    uint16_t buttons;
    int8_t x, y;
  } play[] = {
      {GC_PAD_A, 0, 0},   {0, 0, 100},    {GC_PAD_B, 0, 0},   {0, 100, 0},  {GC_PAD_X, 0, 0},
      {0, 0, -100},       {GC_PAD_A, 0, 0}, {0, -100, 0},     {GC_PAD_Y, 0, 0}, {0, 70, 70},
      {GC_PAD_R, 0, 0},   {0, -70, 70},   {GC_PAD_Z, 0, 0},   {0, 0, 100},  {GC_PAD_A, 0, 0},
      {0, 70, -70},
  };
  const uint64_t first = 600, slot_fields = 60, menu_slots = 20;
  uint64_t slot, at;
  memset(pad, 0, sizeof(*pad));
  pad->present = 1;
  if (field < first) return;
  slot = (field - first) / slot_fields;
  at = (field - first) % slot_fields;
  if (slot < menu_slots) {
    if (at < 6u) pad->buttons = (uint16_t)(slot % 4u == 0u ? GC_PAD_START : GC_PAD_A);
    return;
  }
  {
    const size_t k = (size_t)((slot - menu_slots) % (sizeof(play) / sizeof(play[0])));
    const int direction = play[k].x || play[k].y;
    if (at < (direction ? 48u : 6u)) {
      pad->buttons = play[k].buttons;
      pad->stick_x = play[k].x;
      pad->stick_y = play[k].y;
    }
  }
}

static int load_script(GcRunner *r, const char *path) {
  r->script = gc_input_script_load(r->program->name, path);
  return r->script ? 0 : -1;
}

static int has_input(const GcRunner *r) { return r->auto_input || r->script; }

static void pad_for(const GcRunner *r, uint64_t field, GcPad *pad) {
  if (r->auto_input) {
    gc_auto_input(field, pad);
    return;
  }
  gc_input_script_pad(r->script, field, pad);
}

/* The field's input, on one machine. On a GameCube that is the controller in
 * the first port. On a Wii it is the first remote, with a Nunchuk, holding
 * what the controller's state says under the names the window's default
 * layout gives them (gc_sdl.cpp, build_remote) and pointing at the middle
 * of the screen; no GameCube controller is plugged in, so that a game which
 * reads both does not take every press twice. One script format, and one
 * built-in script, then drive either console. */
static void input_to(const GcRunner *r, GcMachine *m, uint64_t field) {
  GcPad pad;
  pad_for(r, field, &pad);
  if (wii_state(m)) {
    WiiRemoteInput remote;
    wii_remote_rest(&remote, 1);
    if (pad.buttons & GC_PAD_A) remote.buttons |= WII_REMOTE_A;
    if (pad.buttons & GC_PAD_B) remote.buttons |= WII_REMOTE_B;
    if (pad.buttons & GC_PAD_X) remote.buttons |= WII_REMOTE_ONE;
    if (pad.buttons & GC_PAD_Y) remote.buttons |= WII_REMOTE_TWO;
    if (pad.buttons & GC_PAD_START) remote.buttons |= WII_REMOTE_PLUS;
    if (pad.buttons & GC_PAD_UP) remote.buttons |= WII_REMOTE_UP;
    if (pad.buttons & GC_PAD_DOWN) remote.buttons |= WII_REMOTE_DOWN;
    if (pad.buttons & GC_PAD_LEFT) remote.buttons |= WII_REMOTE_LEFT;
    if (pad.buttons & GC_PAD_RIGHT) remote.buttons |= WII_REMOTE_RIGHT;
    remote.extension = WII_EXTENSION_NUNCHUK;
    remote.nunchuk_stick_x = pad.stick_x;
    remote.nunchuk_stick_y = pad.stick_y;
    if (pad.buttons & GC_PAD_L) remote.nunchuk_buttons |= WII_NUNCHUK_Z;
    if (pad.buttons & (GC_PAD_R | GC_PAD_Z)) remote.nunchuk_buttons |= WII_NUNCHUK_C;
    /* The second stick is where the remote points, as a place and not a
     * speed, so that a script can put the pointer on a button. */
    remote.pointer_visible = 1;
    remote.pointer_x = (uint16_t)(32768 + pad.c_stick_x * 256);
    remote.pointer_y = (uint16_t)(32768 - pad.c_stick_y * 256);
    wii_remote_set(m, 0, &remote);
    pad.present = 0;
  }
  gc_pad_set(m, 0, &pad);
}

/* ------------------------------------------------------------------ */
/* Booting                                                             */

static FILE *quiet_log(void) {
#if defined(_WIN32)
  return fopen("NUL", "w");
#else
  return fopen("/dev/null", "w");
#endif
}

/* A machine with the program at its entry point: the disc's apploader for
 * a game, the embedded ELF for a test program. */
static GcMachine *boot(const GcRunner *r, FILE *log, struct GcDisc **disc_out) {
  const GcRunnerProgram *p = r->program;
  GcMachine *m = gc_machine_create();
  struct GcDisc *disc = 0;
  uint32_t entry = 0;
  int booted = 0;
  *disc_out = 0;
  if (!m) return NULL;
  m->log = log;
  m->watch_address = r->watch;
  /* Telling where NaNs are stored needs every store to go the long way,
   * which a watch on an address nothing is at arranges. */
  if (gc_fp_invalid_trace && !m->watch_address) m->watch_address = 0xffffffffu;
  gc_install_boot_state(m);
  if (r->disc_path) {
    disc = gc_disc_open(r->disc_path, log);
    if (!disc || gc_attach_disc(m, disc) != 0) {
      fprintf(stderr, "%s: cannot use %s as a disc\n", p->name, r->disc_path);
      gc_disc_close(disc);
      gc_machine_destroy(m);
      return NULL;
    }
    if (!r->is_elf) {
      if (gc_run_apploader(m, disc, &entry) == 0) booted = 1;
      else gc_machine_reset_after_failed_boot(m);
    }
  }
  if (!booted && gc_load_elf(m, p->image, p->image_size, &entry) != 0 &&
      gc_load_dol(m, p->image, p->image_size, &entry) != 0) {
    fprintf(stderr, "%s: the embedded image will not load\n", p->name);
    gc_disc_close(disc);
    gc_machine_destroy(m);
    return NULL;
  }
  if (r->symbols_path) {
    FILE *file = fopen(r->symbols_path, "rb");
    long length = 0;
    void *bytes = NULL;
    if (file && fseek(file, 0, SEEK_END) == 0) length = ftell(file);
    if (file && length > 0 && fseek(file, 0, SEEK_SET) == 0) {
      bytes = malloc((size_t)length);
      if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) length = 0;
    }
    if (file) fclose(file);
    if (!bytes || length <= 0 || gc_elf_symbols(m, bytes, (size_t)length) != 0) {
      fprintf(stderr, "%s: cannot read symbols from %s%c", p->name, r->symbols_path, 10);
    }
    free(bytes);
  }
  m->cpu.pc = entry;
  m->cpu.npc = entry + 4;
  m->cpu.gpr[1] = 0x816ffff0u;
  m->cpu.gpr[2] = 0x81100000u;
  m->cpu.gpr[13] = 0x81100000u;
  *disc_out = disc;
  return m;
}

static double seconds_now(void) {
  struct timespec now;
  timespec_get(&now, TIME_UTC);
  return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void identity_of(const GcRunner *r, const GcMachine *m, GcStateIdentity *id) {
  const GcRunnerProgram *p = r->program;
  gc_state_identity(id, m, p->name, p->image, p->image_size);
}

/* A booted machine, carried to the state --load-state names when there is
 * one. The program boots first either way: that is what installs the hooks
 * its symbols name, which are the program's and not the state's. */
static GcMachine *boot_from_state(const GcRunner *r, FILE *log, struct GcDisc **disc_out, int report);
static void release(GcMachine *m, struct GcDisc *disc);

static GcMachine *boot_from_state(const GcRunner *r, FILE *log, struct GcDisc **disc_out, int report) {
  GcMachine *m = boot(r, log, disc_out);
  GcStateIdentity id;
  char error[512];
  double started;
  /* The card goes in before a state is loaded, so the state's card image
   * reaches the file. Only the machine that reports is file-backed. */
  if (m && r->card_path &&
      (report ? gc_card_insert(m, r->card_path) : gc_card_insert_unbacked(m, r->card_path)) != 0) {
    fprintf(stderr, "%s: %s is not a memory card image this machine can use\n", r->program->name, r->card_path);
    release(m, *disc_out);
    *disc_out = 0;
    return NULL;
  }
  if (m && r->nand_path && wii_state(m) &&
      (report ? wii_nand_attach(m, r->nand_path, stderr) : wii_nand_attach_unbacked(m, r->nand_path, stderr)) != 0) {
    fprintf(stderr, "%s: %s is not a flash memory file this machine can use\n", r->program->name, r->nand_path);
    release(m, *disc_out);
    *disc_out = 0;
    return NULL;
  }
  if (m && r->wii_aspect && wii_state(m) && wii_console_set_widescreen(m, r->wii_aspect == 2) != 0) {
    fprintf(stderr, "%s: the console's settings have no picture shape to set\n", r->program->name);
  }
  if (!m || !r->load_path) return m;
  identity_of(r, m, &id);
  started = seconds_now();
  if (gc_state_load_file(m, &id, r->load_path, error, sizeof(error)) != 0) {
    fprintf(stderr, "%s: cannot load the state %s: %s\n", r->program->name, r->load_path, error);
    release(m, *disc_out);
    *disc_out = 0;
    return NULL;
  }
  if (report) {
    fprintf(stderr, "%s: loaded the state %s (field %llu, instruction %llu) in %.0f ms\n", r->program->name,
            r->load_path, (unsigned long long)m->field, (unsigned long long)m->cpu.cycles,
            (seconds_now() - started) * 1000.0);
  }
  return m;
}

/* With more than one --save-at field, each state's file name carries its
 * field: states.state becomes states-001400.state. */
static int save_state(const GcRunner *r, GcMachine *m) {
  GcStateIdentity id;
  char error[512], path[1100];
  const double started = seconds_now();
  const char *dot = strrchr(r->save_path, '.');
  const char *slash = strrchr(r->save_path, '/'), *backslash = strrchr(r->save_path, '\\');
  if (backslash > slash) slash = backslash;
  if (!dot || (slash && dot < slash)) dot = r->save_path + strlen(r->save_path);
  if (r->save_count > 1u) {
    snprintf(path, sizeof(path), "%.*s-%06llu%s", (int)(dot - r->save_path), r->save_path,
             (unsigned long long)m->field, dot);
  } else {
    snprintf(path, sizeof(path), "%s", r->save_path);
  }
  identity_of(r, m, &id);
  if (gc_state_save_file(m, &id, path, !r->save_raw, error, sizeof(error)) != 0) {
    fprintf(stderr, "%s: cannot save the state: %s\n", r->program->name, error);
    return -1;
  }
  {
    FILE *file = fopen(path, "rb");
    long size = -1;
    if (file) {
      fseek(file, 0, SEEK_END);
      size = ftell(file);
      fclose(file);
    }
    fprintf(stderr, "%s: saved the state at field %llu (instruction %llu) to %s, %ld bytes, in %.0f ms\n",
            r->program->name, (unsigned long long)m->field, (unsigned long long)m->cpu.cycles, path, size,
            (seconds_now() - started) * 1000.0);
  }
  return 0;
}

static void release(GcMachine *m, struct GcDisc *disc) {
  if (m) {
    gc_coverage_destroy((GcCoverage *)m->coverage);
    m->coverage = NULL;
    gc_machine_destroy(m);
  }
  gc_disc_close(disc);
}

/* ------------------------------------------------------------------ */
/* Running a field                                                     */

static void run_one(const GcRunner *r, GcMachine *m, uint64_t until_field) {
  if (r->interpret || !r->program->run) gc_machine_run(m, until_field);
  else r->program->run(m, until_field);
}

/* One field of the pair, or of the one machine: the controller for the
 * field on both, the recompiled machine to the end of it, and the
 * interpreter to the same instruction count. Returns -1 when the
 * interpreter could not get there. */
static int run_field(const GcRunner *r, GcMachine *a, GcMachine *b) {
  const uint64_t field = a->field;
  if (has_input(r)) {
    input_to(r, a, field);
    if (b) input_to(r, b, field);
  }
  run_one(r, a, field + 1u);
  return b ? gc_differential_catch_up(a, b) : 0;
}

static int running(const GcMachine *m, uint64_t frames) {
  return m->field < frames && !m->finished && !m->cpu.stopped;
}

/* ------------------------------------------------------------------ */
/* Finding the block that diverged                                     */

#define GC_RING 32u
#define GC_MEMORY_CHECK_EVERY 1000000u
#define GC_WATCH 64u

/* The bytes a first look found different, watched after every block on a
 * second look. */
typedef struct GcWatch {
  uint32_t offset[GC_WATCH];
  unsigned count;
} GcWatch;

static void watch_differences(const GcMachine *a, const GcMachine *b, GcWatch *watch) {
  uint32_t i;
  watch->count = 0;
  for (i = 0; i < gc_ram_size && watch->count < GC_WATCH; i++) {
    if (a->mem1[i] != b->mem1[i]) watch->offset[watch->count++] = i;
  }
}

static int watched_differ(const GcMachine *a, const GcMachine *b, const GcWatch *watch) {
  unsigned i;
  for (i = 0; i < watch->count; i++) {
    if (a->mem1[watch->offset[i]] != b->mem1[watch->offset[i]]) return 1;
  }
  return 0;
}

typedef enum GcLocated { GC_LOCATE_FAILED, GC_LOCATE_CPU, GC_LOCATE_MEMORY, GC_LOCATE_END, GC_LOCATE_CLEAN } GcLocated;

/* Both machines from the start to the last field they agreed at, then that
 * field block by block. Without a watch list, the processors are compared
 * after every block and main memory every million instructions; with one,
 * the watched bytes are compared after every block as well, which names the
 * block that wrote them. The blocks are stepped the way the dispatcher in
 * recompiled_run.c steps them, one table lookup at a time; this is a
 * diagnostic, and if the generated dispatcher runs blocks some other way
 * (chained) the field may come out clean here, which is itself worth
 * knowing. */
static GcLocated locate_pass(const GcRunner *r, uint64_t field, const GcWatch *watch, GcWatch *found) {
  const GcRunnerProgram *p = r->program;
  struct GcDisc *disc_a = 0, *disc_b = 0;
  FILE *const quiet = quiet_log();
  FILE *const log = quiet ? quiet : stderr;
  GcMachine *a = boot_from_state(r, log, &disc_a, 0);
  GcMachine *b = a ? boot_from_state(r, log, &disc_b, 0) : NULL;
  void *(**table)(GcMachine *) = 0;
  uint32_t ring_pc[GC_RING];
  uint64_t ring_cycles[GC_RING];
  uint8_t ring_native[GC_RING];
  unsigned ring_at = 0, i;
  uint64_t checked_at, blocks = 0;
  GcLocated result = GC_LOCATE_FAILED;

  if (!a || !b || !p->blocks || !p->span) goto done;
  while (running(a, field)) {
    if (run_field(r, a, b) != 0) break;
  }
  if (a->field != field || gc_differential_compare(a, b, NULL) != 0) {
    fprintf(stderr, "%s: the second run did not agree at field %llu as the first did; the runs are not "
                    "deterministic, so the block cannot be found this way\n",
            p->name, (unsigned long long)field);
    goto done;
  }

  table = (void *(**)(GcMachine *))calloc(p->span, sizeof(*table));
  if (!table) goto done;
  {
    uint32_t hooks[8];
    const unsigned hook_count = gc_os_hook_addresses(hooks, 8u);
    for (i = 0; i < p->block_count; i++) {
      const GcRunnerBlock *block = &p->blocks[i];
      const uint32_t index = (block->address - p->base) >> 2;
      unsigned k;
      int hooked = 0;
      for (k = 0; k < hook_count; k++) {
        if (hooks[k] >= block->address && hooks[k] < block->end) hooked = 1;
      }
      if (index < p->span && !hooked) table[index] = block->run;
    }
  }

  if (has_input(r)) {
    input_to(r, a, field);
    input_to(r, b, field);
  }
  memset(ring_pc, 0, sizeof(ring_pc));
  checked_at = a->cpu.cycles;
  a->cpu.next_event = 0;
  while (running(a, field + 1u)) {
    const uint32_t index = (a->cpu.pc - p->base) >> 2;
    void *(*run)(GcMachine *) = index < p->span ? table[index] : 0;
    ring_pc[ring_at] = a->cpu.pc;
    ring_cycles[ring_at] = a->cpu.cycles;
    ring_native[ring_at] = run != 0;
    ring_at = (ring_at + 1u) % GC_RING;
    if (run) {
      run(a);
    } else {
      gc_machine_sync(a);
      gc_machine_step(a);
      a->cpu.next_event = 0;
    }
    if (a->cpu.cycles >= a->next_field_cycles) {
      gc_machine_sync(a);
      a->cpu.next_event = 0;
    }
    blocks++;
    if (gc_differential_catch_up(a, b) != 0 || gc_differential_compare_cpu(a, b, NULL) != 0) {
      fprintf(stderr, "%s: the processors first differ after block %llu of field %llu, at instruction %llu:\n",
              p->name, (unsigned long long)blocks, (unsigned long long)field, (unsigned long long)a->cpu.cycles);
      gc_differential_compare_cpu(a, b, stderr);
      result = GC_LOCATE_CPU;
      break;
    }
    if (watch && watched_differ(a, b, watch)) {
      fprintf(stderr, "%s: main memory first differs after block %llu of field %llu, at instruction %llu:\n",
              p->name, (unsigned long long)blocks, (unsigned long long)field, (unsigned long long)a->cpu.cycles);
      gc_differential_compare(a, b, stderr);
      result = GC_LOCATE_MEMORY;
      break;
    }
    if (!watch && a->cpu.cycles - checked_at >= GC_MEMORY_CHECK_EVERY) {
      if (memcmp(a->mem1, b->mem1, gc_ram_size) != 0) {
        fprintf(stderr, "%s: main memory first differs between instructions %llu and %llu of field %llu\n",
                p->name, (unsigned long long)checked_at, (unsigned long long)a->cpu.cycles,
                (unsigned long long)field);
        if (found) watch_differences(a, b, found);
        result = GC_LOCATE_MEMORY;
        goto done; /* the caller looks again, watching those bytes */
      }
      checked_at = a->cpu.cycles;
    }
  }
  if (result == GC_LOCATE_FAILED) {
    if (gc_differential_compare(a, b, NULL) != 0) {
      fprintf(stderr, "%s: the processors agreed after every block of field %llu, and the machines differ at "
                      "its end:\n",
              p->name, (unsigned long long)field);
      gc_differential_compare(a, b, stderr);
      if (found && memcmp(a->mem1, b->mem1, gc_ram_size) != 0) watch_differences(a, b, found);
      result = GC_LOCATE_END;
    } else {
      fprintf(stderr, "%s: field %llu is clean stepped a block at a time, so the divergence depends on how the "
                      "dispatcher runs blocks rather than on a block\n",
              p->name, (unsigned long long)field);
      result = GC_LOCATE_CLEAN;
      goto done;
    }
  }
  if (result != GC_LOCATE_END || !found || !found->count) {
    fprintf(stderr, "%s: the blocks up to it, oldest first (address, instruction count, how it ran):\n", p->name);
    for (i = 0; i < GC_RING; i++) {
      const unsigned j = (ring_at + i) % GC_RING;
      if (!ring_pc[j]) continue;
      fprintf(stderr, "  %08x  %llu  %s\n", ring_pc[j], (unsigned long long)ring_cycles[j],
              ring_native[j] ? "recompiled" : "interpreted");
    }
  }

done:
  free(table);
  release(a, disc_a);
  release(b, disc_b);
  if (quiet) fclose(quiet);
  return result;
}

static void locate(const GcRunner *r, uint64_t field) {
  GcWatch found;
  GcLocated first;
  found.count = 0;
  fprintf(stderr, "%s: running both again to field %llu to find the block\n", r->program->name,
          (unsigned long long)field);
  first = locate_pass(r, field, NULL, &found);
  if ((first == GC_LOCATE_MEMORY || first == GC_LOCATE_END) && found.count) {
    fprintf(stderr, "%s: and once more, watching the %u bytes that differ after every block\n", r->program->name,
            found.count);
    locate_pass(r, field, &found, NULL);
  }
}

/* ------------------------------------------------------------------ */

static void dump_numbered(const GcRunner *r, GcMachine *m) {
  char numbered[1024];
  size_t stem = strlen(r->frame_path);
  if (stem > 4 && !strcmp(r->frame_path + stem - 4, ".ppm")) stem -= 4;
  snprintf(numbered, sizeof(numbered), "%.*s-%06llu.ppm", (int)stem, r->frame_path, (unsigned long long)m->field);
  gc_efb_write_frame(m, numbered);
}

int gc_runner_main(int argc, char **argv, const GcRunnerProgram *program) {
  GcRunner r;
  GcMachine *m = 0, *shadow = 0;
  struct GcDisc *disc = 0, *shadow_disc = 0;
  FILE *shadow_log = 0;
  char default_disc[1024];
  const char *input_path = 0;
  int status = 0;
  int diverged = 0;
#if defined(_WIN32)
  GuestProfile *profile = NULL;
#endif
  uint64_t start_field = 0;
  int i;

  memset(&r, 0, sizeof(r));
  r.program = program;
  r.frames = 600;
  r.is_elf = program->image_size >= 4u && program->image[0] == 0x7f && program->image[1] == 'E' &&
             program->image[2] == 'L' && program->image[3] == 'F';

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
      r.frames = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--disc") && i + 1 < argc) {
      r.disc_path = argv[++i];
    } else if (!strcmp(argv[i], "--dump-frame") && i + 1 < argc) {
      r.frame_path = argv[++i];
    } else if (!strcmp(argv[i], "--dump-frame-every") && i + 1 < argc) {
      r.frame_every = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--dump-frame-from") && i + 1 < argc) {
      r.frame_from = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--wii-aspect") && i + 1 < argc) {
      /* The console's picture shape, set before the game starts. */
      const char *shape = argv[++i];
      r.wii_aspect = !strcmp(shape, "16:9") ? 2 : (!strcmp(shape, "4:3") ? 1 : -1);
      if (r.wii_aspect < 0) {
        fprintf(stderr, "%s: --wii-aspect takes 4:3 or 16:9, not %s\n", program->name, shape);
        return 2;
      }
    } else if (!strcmp(argv[i], "--interpret")) {
      /* The same program without the recompiled blocks. */
      r.interpret = 1;
    } else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
      input_path = argv[++i];
    } else if (!strcmp(argv[i], "--auto-input")) {
      r.auto_input = 1;
    } else if (!strcmp(argv[i], "--differential")) {
      r.differential = 1;
    } else if (!strcmp(argv[i], "--coverage")) {
      r.coverage = 1;
    } else if (!strcmp(argv[i], "--save-state") && i + 1 < argc) {
      r.save_path = argv[++i];
    } else if (!strcmp(argv[i], "--save-at") && i + 1 < argc) {
      char *text = argv[++i];
      while (*text && r.save_count < sizeof(r.save_at) / sizeof(r.save_at[0])) {
        r.save_at[r.save_count++] = strtoull(text, &text, 10);
        if (*text == ',') text++;
        else break;
      }
    } else if (!strcmp(argv[i], "--state-uncompressed")) {
      r.save_raw = 1;
    } else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) {
      r.load_path = argv[++i];
    } else if (!strcmp(argv[i], "--state-hash")) {
      r.state_hash = 1;
    } else if (!strcmp(argv[i], "--card") && i + 1 < argc) {
      r.card_path = argv[++i];
    } else if (!strcmp(argv[i], "--symbols") && i + 1 < argc) {
      r.symbols_path = argv[++i];
    } else if (!strcmp(argv[i], "--nand") && i + 1 < argc) {
      r.nand_path = argv[++i];
    } else if (!strcmp(argv[i], "--gl")) {
      r.use_gl = 1;
    } else if (!strcmp(argv[i], "--gl-fast")) {
      /* The OpenGL path's fast way of drawing (gc_gl.h). */
      r.use_gl = 1;
      r.gl.fast = 1;
    } else if (!strcmp(argv[i], "--gl-scale") && i + 1 < argc) {
      /* ... at this many times the console's size (the fast path only). */
      r.gl.scale = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--gl-anisotropy") && i + 1 < argc) {
      /* ... filtering slanted textures with up to this many samples (the
       * fast path only; 1 is the reference's filtering). */
      r.gl.anisotropy = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--gl-shaders") && i + 1 < argc) {
      r.gl.shaders = argv[++i];
    } else if (!strcmp(argv[i], "--gl-cache") && i + 1 < argc) {
      r.gl.cache_dir = argv[++i];
    } else if (!strcmp(argv[i], "--field-log") && i + 1 < argc) {
      const char *path = argv[++i];
      if (gc_perf_field_log_open(path) != 0) {
        fprintf(stderr, "%s: cannot write %s\n", program->name, path);
        return 2;
      }
      r.field_log = 1;
    } else if (!strcmp(argv[i], "--dump-memory") && i + 1 < argc) {
      r.memory_path = argv[++i];
    } else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
      r.watch = (uint32_t)strtoul(argv[++i], NULL, 16);
    } else if (!strcmp(argv[i], "--trace-invalid") && i + 1 < argc) {
      gc_fp_invalid_trace = 1;
      gc_fp_invalid_from = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--trace-out") && i + 1 < argc) {
      r.trace_path = argv[++i];
    } else if (!strcmp(argv[i], "--dump-audio") && i + 1 < argc) {
      const char *path = argv[++i];
      r.audio = fopen(path, "wb");
      if (!r.audio) {
        fprintf(stderr, "%s: cannot write %s\n", program->name, path);
        return 2;
      }
    } else if (!strcmp(argv[i], "--audio-log") && i + 1 < argc) {
      const char *path = argv[++i];
      r.audio_log = fopen(path, "w");
      if (!r.audio_log) {
        fprintf(stderr, "%s: cannot write %s\n", program->name, path);
        return 2;
      }
      fprintf(r.audio_log, "# field lists bad_lists voices_walked voices_running ring_overruns (running totals)\n");
    }
  }
  if (input_path && load_script(&r, input_path) != 0) return 2;
  if (input_path && r.auto_input) {
    fprintf(stderr, "%s: --input and --auto-input are two scripts; pick one\n", program->name);
    return 2;
  }
  if ((r.differential || r.coverage || r.trace_path) && (r.interpret || !program->run)) {
    fprintf(stderr, "%s: --differential, --coverage and --trace-out are about recompiled code, and "
                    "--interpret runs none\n",
            program->name);
    return 2;
  }

  if (!r.disc_path && !r.is_elf &&
      beside_executable(default_disc, sizeof(default_disc), argv[0], program->disc_name) == 0) {
    FILE *probe = fopen(default_disc, "rb");
    if (probe) {
      fclose(probe);
      r.disc_path = default_disc;
    }
  }

  if (r.save_path && !r.save_count) r.save_at[r.save_count++] = r.frames;
  m = boot_from_state(&r, stderr, &disc, 1);
  if (!m) return 1;
  if (r.use_gl) {
    /* Only the machine being measured; a differential's shadow stays on the
     * reference, which is what it is there to be. Not a quiet fallback:
     * asking for the card and silently getting the reference would make
     * every comparison meaningless. */
    if (!r.gl.id) r.gl.id = program->name;
    if (gc_gl_start_with(m, &r.gl) != 0) {
      fprintf(stderr, "%s: --gl was asked for and there is no OpenGL path\n", program->name);
      release(m, disc);
      return 1;
    }
  }
  /* The command stream draws on a thread of its own unless GC_GX_THREAD=0
   * (gc_gx_thread.h); a differential's shadow stays on this thread. */
  {
    /* GC_VI_DOUBLE=1: the video interface sweeps a frame in one field, so a
     * game that draws on every second retrace draws every field. */
    const char *setting = getenv("GC_VI_DOUBLE");
    gc_vi_double = setting && strtol(setting, NULL, 10) != 0;
    if (gc_vi_double) {
      fprintf(stderr, "%s: the video interface is running at twice its rate\n", program->name);
    }
  }
  gc_gx_thread_start(m);
#if defined(_WIN32)
  {
    /* GC_GUEST_PROFILE=<hz>: sample the thread that runs the machine. */
    const char *setting = getenv("GC_GUEST_PROFILE");
    const long hz = setting ? strtol(setting, NULL, 10) : 0;
    if (hz > 0) profile = guest_profile_start((unsigned)hz);
  }
#endif
  start_field = m->field;
  if (r.differential) {
    shadow_log = quiet_log();
    /* Both machines from the same state: the comparison starts where the
     * state does, and a divergence is looked for from there too. */
    shadow = boot_from_state(&r, shadow_log, &shadow_disc, 0);
    if (!shadow) {
      release(m, disc);
      return 1;
    }
  }
  if (r.coverage || r.trace_path) {
    m->coverage = gc_coverage_create();
    if (shadow && r.coverage) shadow->coverage = gc_coverage_create();
  }

  if (!has_input(&r) && !shadow && !r.frame_every && !r.audio && !r.audio_log && !r.save_path && !r.load_path &&
      !r.state_hash && !r.field_log) {
    /* The run as it always was: one call, no controller. */
    run_one(&r, m, r.frames);
  } else {
    unsigned saved = 0, k;
    while (running(m, r.frames)) {
      const uint64_t field = m->field;
      for (k = 0; r.save_path && k < r.save_count; k++) {
        if (!(saved & (1u << k)) && field == r.save_at[k]) {
          saved |= 1u << k;
          if (save_state(&r, m) != 0) status = 1;
        }
      }
      int failed;
      gc_perf_field_begin(m);
      failed = run_field(&r, m, shadow) != 0;
      gc_perf_field_end(m);
      if (failed || (shadow && gc_differential_compare(m, shadow, NULL) != 0)) {
        fprintf(stderr,
                "%s: DIVERGED in field %llu: at instruction %llu the recompiled machine is at %08x and the "
                "interpreter at %08x (instruction %llu)\n",
                program->name, (unsigned long long)field, (unsigned long long)m->cpu.cycles, m->cpu.pc,
                shadow->cpu.pc, (unsigned long long)shadow->cpu.cycles);
        gc_differential_compare(m, shadow, stderr);
        diverged = 1;
        status = 3;
        locate(&r, field);
        break;
      }
      if (r.frame_every && r.frame_path && m->field >= r.frame_from && m->field % r.frame_every == 0u) {
        dump_numbered(&r, m);
      }
      drain_audio(&r, m);
    }
    for (k = 0; r.save_path && k < r.save_count; k++) {
      if (!(saved & (1u << k)) && m->field == r.save_at[k] && !diverged) {
        saved |= 1u << k;
        if (save_state(&r, m) != 0) status = 1;
      }
      if (!(saved & (1u << k))) {
        fprintf(stderr, "%s: the run ended at field %llu before field %llu, so that state was not saved\n",
                program->name, (unsigned long long)m->field, (unsigned long long)r.save_at[k]);
        status = 1;
      }
    }
  }
  if (r.audio) fclose(r.audio);
  if (r.audio_log) fclose(r.audio_log);
  gc_perf_field_log_close();

#if defined(_WIN32)
  if (profile) guest_profile_report(profile, program, stderr);
#endif
  fprintf(stderr, "%s: %llu fields, %llu instructions, pc %08x\n", program->name, (unsigned long long)m->field,
          (unsigned long long)m->cpu.cycles, m->cpu.pc);
  {
    /* Whatever the rasteriser drops is said out loud once, at the end. A
     * feature that is missing and silent is a feature nobody finds. */
    static const char *const DROPPED[3] = {"lines", "line strips", "points"};
    unsigned k;
    for (k = 0; k < 3u; k++) {
      if (!m->gx.kind_vertices[5u + k]) continue;
      fprintf(stderr, "%s: gx: %llu vertices of %s were not drawn - this rasteriser draws triangles only\n",
              program->name, (unsigned long long)m->gx.kind_vertices[5u + k], DROPPED[k]);
    }
    if (m->gx.unclipped_triangles) {
      fprintf(stderr, "%s: gx: %u triangles reached the eye plane and were clipped against it\n",
              program->name, m->gx.unclipped_triangles);
    }
    if (m->gx.unsupported_indirect) {
      fprintf(stderr, "%s: gx: %u texture coordinates went through a dynamic indirect matrix, which is not modelled\n",
              program->name, m->gx.unsupported_indirect);
    }
    if (m->efb.unsupported_copies) {
      fprintf(stderr, "%s: gx: %u framebuffer copies this does not support\n", program->name,
              m->efb.unsupported_copies);
    }
  }
  {
    /* What is on the screen and where the last frame was put: when the two
     * are different places the picture is whatever was in memory. */
    GcViDisplay d;
    gc_gx_thread_sync(m); /* the pixel count is the graphics thread's */
    if (gc_vi_display(m, &d) == 0) {
      fprintf(stderr, "%s: display: the video interface shows %08x and %08x, %ux%u; ", program->name, d.top,
              d.bottom, d.width, d.height);
    } else {
      fprintf(stderr, "%s: display: the video interface shows nothing; ", program->name);
    }
    fprintf(stderr, "%llu copies, the last for display to %08x, %ux%u\n", (unsigned long long)m->efb.copies,
            m->efb.display_address, m->efb.display_width, m->efb.display_height);
    fprintf(stderr, "%s: graphics: %llu bytes, %llu commands, %llu primitives, %llu vertices, %llu display lists, "
                    "%llu triangles, %llu pixels\n",
            program->name, (unsigned long long)m->gx.bytes, (unsigned long long)m->gx.commands,
            (unsigned long long)m->gx.primitives, (unsigned long long)m->gx.vertices,
            (unsigned long long)m->gx.display_lists, (unsigned long long)m->gx.triangles,
            (unsigned long long)m->gx.pixels);
    if (gc_pi_fifo_recorded()) {
      fprintf(stderr, "%s: graphics: %llu bytes of commands were recorded into memory rather than run (display "
                      "lists the game builds itself)\n",
              program->name, (unsigned long long)gc_pi_fifo_recorded());
    }
  }
  if (r.memory_path) {
    /* Main memory, for looking at a game from outside: its heaps, its
     * threads, what it had loaded. MEM1, and on a Wii MEM2 straight after
     * it. The registers go to the log, since a stack is no use without
     * its pointer. */
    FILE *f = fopen(r.memory_path, "wb");
    if (!f || fwrite(m->mem1, 1, gc_ram_size, f) != gc_ram_size) {
      fprintf(stderr, "%s: cannot write %s\n", program->name, r.memory_path);
    } else {
      unsigned k;
      fprintf(stderr, "%s: memory: %u bytes in %s; pc %08x lr %08x msr %08x\n", program->name, gc_ram_size,
              r.memory_path, m->cpu.pc, m->cpu.lr, m->cpu.msr);
      for (k = 0; k < 32u; k += 8u) {
        fprintf(stderr, "%s: memory: r%-2u %08x %08x %08x %08x %08x %08x %08x %08x\n", program->name, k,
                m->cpu.gpr[k], m->cpu.gpr[k + 1u], m->cpu.gpr[k + 2u], m->cpu.gpr[k + 3u], m->cpu.gpr[k + 4u],
                m->cpu.gpr[k + 5u], m->cpu.gpr[k + 6u], m->cpu.gpr[k + 7u]);
      }
    }
    if (f) fclose(f);
  }
  if (gc_console_is_wii()) {
    /* What the game asked the console for: a game that has stopped is
     * usually waiting on one of these. */
    const WiiState *w = wii_state(m);
    fprintf(stderr, "%s: wii: %u IOS requests, %u to devices that are not there and %u this does not know; "
                    "the disc was read %llu times for %llu bytes\n",
            program->name, w->ios.requests, w->ios.unknown_devices, w->ios.unknown_commands,
            (unsigned long long)w->di.reads, (unsigned long long)w->di.bytes);
  }
  if (gc_zelda_commands(m) || gc_zelda_frames(m)) {
    const GcZelda *z = gc_ax_zelda(m);
    char sources[160] = "";
    unsigned source, used = 0;
    for (source = 0; source < 64u; source++) {
      if ((z->sources_used >> source) & 1u) {
        used += (unsigned)snprintf(sources + used, sizeof(sources) - used, " %u", source);
        if (used >= sizeof(sources)) break;
      }
    }
    fprintf(stderr, "%s: sound: the Zelda microcode took %llu commands and rendered %llu frames; %llu voice-frames "
                    "mixed (%llu of them by position), %llu voices ended, sample sources%s, %llu reverb line-frames, "
                    "%llu voice-frames from a source that is not written\n",
            program->name, (unsigned long long)z->commands, (unsigned long long)z->frames,
            (unsigned long long)z->voices_mixed, (unsigned long long)z->voices_placed,
            (unsigned long long)z->voices_finished, used ? sources : " none", (unsigned long long)z->reverb_frames,
            (unsigned long long)z->unknown_sources);
  }
  if (shadow && !diverged && r.load_path) {
    fprintf(stderr, "%s: differential: the recompiled and interpreted machines agree at the end of every one of "
                    "%llu fields, from the state's field %llu to %llu\n",
            program->name, (unsigned long long)(m->field - start_field), (unsigned long long)start_field,
            (unsigned long long)m->field);
  } else if (shadow && !diverged) {
    fprintf(stderr, "%s: differential: the recompiled and interpreted machines agree at the end of every one of "
                    "%llu fields\n",
            program->name, (unsigned long long)m->field);
  }
  if (r.coverage && m->coverage) {
    gc_coverage_report(stderr, program->name, m->cpu.cycles, (GcCoverage *)m->coverage,
                       shadow && !diverged ? (GcCoverage *)shadow->coverage : NULL);
  }
  if (r.trace_path && m->coverage) {
    char comment[128];
    snprintf(comment, sizeof(comment), "%s, %llu fields", program->name, (unsigned long long)m->field);
    if (gc_coverage_write_trace((GcCoverage *)m->coverage, r.trace_path, comment) != 0) {
      fprintf(stderr, "%s: cannot write %s\n", program->name, r.trace_path);
      status = 1;
    }
  }
  if (r.state_hash) {
    /* Where each way of running happened to stop is not the machine's
     * business - recompiled code finishes its block - so, as the frontend's
     * --audio-stats does, the interpreter carries the machine to a count
     * both reach before anything is hashed. */
    const uint64_t hash_at = m->field * GC_CYCLES_PER_FIELD + 100000u;
    while (m->cpu.cycles < hash_at && !m->finished && !m->cpu.stopped) gc_machine_step(m);
    gc_machine_sync(m);
    fprintf(stderr, "%s: state: field %llu, %llu instructions, pc %08x, hash %016llx, full %016llx\n",
            program->name, (unsigned long long)m->field, (unsigned long long)m->cpu.cycles, m->cpu.pc,
            (unsigned long long)gc_state_hash_memory(m), (unsigned long long)gc_state_hash(m));
  }
  if (r.frame_path && !r.frame_every && gc_efb_write_frame(m, r.frame_path) != 0) {
    fprintf(stderr, "%s: nothing was on the screen, so %s was not written\n", program->name, r.frame_path);
    status = 1;
  }
  release(shadow, shadow_disc);
  if (shadow_log) fclose(shadow_log);
  release(m, disc);
  gc_input_script_free(r.script);
  return status;
}
