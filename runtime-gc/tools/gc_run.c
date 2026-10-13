/*
 * gc_run: run a GameCube program headlessly on the interpreter.
 *
 * This is what the test programs run on, what reads their verdicts, and
 * what P4's differential mode will compare recompiled code against. It is
 * deterministic: no host clock, no host input, and a run that writes no
 * state or never finishes is a failure rather than a pass with an excuse.
 *
 *   gc_run program.elf [--disc IMAGE] [--frames N] [--dump-state FILE]
 *                      [--trace-io] [--allow-bad-access] [--quiet]
 *
 * The program may be an ELF (what devkitPPC builds, and what
 * dolphin-emu/hwtests produces) or a DOL (what a disc holds). With --disc
 * there is a disc in the drive as well, and the program can read it - by
 * offset, or by name through the filesystem the apploader would have put
 * in memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "wii.h"
#include "wii_nand.h"
#include "gc_audio.h"
#include "gc_ax.h"
#include "gc_card.h"
#include "gc_disc.h"
#include "gc_gl.h"
#include "gc_pad.h"
#include "gc_efb.h"

static void *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  void *data;
  long length;
  if (!file) return NULL;
  fseek(file, 0, SEEK_END);
  length = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (length <= 0) {
    fclose(file);
    return NULL;
  }
  data = malloc((size_t)length);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size = (size_t)length;
  return data;
}

/* The state dump is what a differential run and a regression check compare,
 * so it is plain text in a fixed order with no host-dependent values in
 * it. */
static int dump_state(const GcMachine *m, const char *path) {
  const GcCpu *c = &m->cpu;
  FILE *file = fopen(path, "wb");
  unsigned i;
  if (!file) return -1;
  fprintf(file, "pc %08x\nlr %08x\nctr %08x\ncr %08x\nxer %08x\nmsr %08x\nfpscr %08x\n", c->pc,
          c->lr, c->ctr, c->cr, c->xer, c->msr, gc_cpu_fpscr(c));
  for (i = 0; i < 32; i++) fprintf(file, "r%u %08x\n", i, c->gpr[i]);
  for (i = 0; i < 32; i++) {
    fprintf(file, "f%u %016llx %016llx\n", i, (unsigned long long)c->fpr[i].ps0,
            (unsigned long long)c->fpr[i].ps1);
  }
  for (i = 0; i < 8; i++) fprintf(file, "gqr%u %08x\n", i, c->gqr[i]);
  fprintf(file, "hid2 %08x\n", c->hid2);
  fprintf(file, "field %llu\n", (unsigned long long)m->field);
  fclose(file);
  return 0;
}

int gc_runner_main(int argc, char **argv) {
  const char *program = NULL;
  const char *state_path = NULL;
  const char *disc_path = NULL;
  const char *frame_path = NULL;
  const char *picture_path = NULL; /* --dump-picture: the frame as large as --gl-scale drew it */
  const char *card_path = NULL;
  const char *audio_path = NULL;
  FILE *audio_file = NULL;
  GcDisc *disc = NULL;
  uint64_t frames = 600;
  int allow_bad_access = 0;
  int stop_on_bad_access = 0;
  uint32_t watch_address = 0;
  int profile = 0;
  uint64_t stop_after = 0;
  uint32_t stop_at = 0;
  int branches = 0;
  int no_apploader = 0;
  int booted_by_apploader = 0;
  int pad_test = 0;
  int quiet = 0;
  int use_gl = 0; /* 1: --gl, 2: --gl-fast */
  int gl_scale = 0; /* --gl-scale */
  int gl_anisotropy = 0; /* --gl-anisotropy */
  GcGlOptions gl_options;
  int trace_io = 0;
  uint64_t frame_every = 0;
  size_t size = 0;
  void *data;
  GcMachine *m;
  uint32_t entry = 0;
  int status = 0;
  int wii = 0;
  int program_disc = 0;
  const char *symbols_path = NULL;
  const char *nand_path = NULL;
  int i;

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--frames") && i + 1 < argc) {
      frames = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(a, "--dump-state") && i + 1 < argc) {
      state_path = argv[++i];
    } else if (!strcmp(a, "--disc") && i + 1 < argc) {
      disc_path = argv[++i];
    } else if (!strcmp(a, "--dump-frame") && i + 1 < argc) {
      frame_path = argv[++i];
    } else if (!strcmp(a, "--dump-frame-every") && i + 1 < argc) {
      frame_every = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(a, "--gl")) {
      use_gl = 1;
    } else if (!strcmp(a, "--gl-fast")) {
      use_gl = 2;
    } else if (!strcmp(a, "--gl-scale") && i + 1 < argc) {
      gl_scale = atoi(argv[++i]);
    } else if (!strcmp(a, "--gl-anisotropy") && i + 1 < argc) {
      gl_anisotropy = atoi(argv[++i]);
    } else if (!strcmp(a, "--dump-picture") && i + 1 < argc) {
      picture_path = argv[++i];
    } else if (!strcmp(a, "--trace-io")) {
      trace_io = 1;
    } else if (!strcmp(a, "--dump-audio") && i + 1 < argc) {
      audio_path = argv[++i];
    } else if (!strcmp(a, "--card") && i + 1 < argc) {
      card_path = argv[++i];
    } else if (!strcmp(a, "--pad-test")) {
      pad_test = 1;
    } else if (!strcmp(a, "--pad-test=classic")) {
      /* The same, with a Classic Controller in the first remote in place
       * of a Nunchuk. */
      pad_test = 2;
    } else if (!strcmp(a, "--no-apploader")) {
      no_apploader = 1;
    } else if (!strcmp(a, "--branches")) {
      branches = 1;
    } else if (!strcmp(a, "--stop-at") && i + 1 < argc) {
      stop_at = (uint32_t)strtoul(argv[++i], NULL, 16);
    } else if (!strcmp(a, "--stop-after") && i + 1 < argc) {
      stop_after = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(a, "--profile")) {
      profile = 1;
    } else if (!strcmp(a, "--watch") && i + 1 < argc) {
      watch_address = (uint32_t)strtoul(argv[++i], NULL, 16);
    } else if (!strcmp(a, "--stop-on-bad-access")) {
      stop_on_bad_access = 1;
    } else if (!strcmp(a, "--allow-bad-access")) {
      allow_bad_access = 1;
    } else if (!strcmp(a, "--quiet")) {
      quiet = 1;
    } else if (!strcmp(a, "--wii")) {
      wii = 1;
    } else if (!strcmp(a, "--symbols") && i + 1 < argc) {
      symbols_path = argv[++i];
    } else if (!strcmp(a, "--nand") && i + 1 < argc) {
      nand_path = argv[++i];
    } else if (a[0] == '-') {
      fprintf(stderr, "gc_run: unknown option %s\n", a);
      return 2;
    } else if (!program) {
      program = a;
    }
  }
  if (!program) {
    fprintf(stderr,
            "usage: gc_run program.elf [--disc IMAGE] [--frames N] [--dump-state FILE]\n"
            "                      [--dump-frame FILE.ppm [--dump-frame-every FIELDS]]\n");
    return 2;
  }

  /* A disc is asked what it is rather than read: an image is gigabytes,
   * and only its own reader knows the containers it comes in. */
  program_disc = gc_disc_console(program);
  if (disc_path && !wii && gc_disc_console(disc_path) == 2) wii = 1;
  if (program_disc == 2) wii = 1;
  data = program_disc ? malloc(1) : read_file(program, &size);
  if (!data) {
    fprintf(stderr, "gc_run: cannot read %s\n", program);
    return 1;
  }

  /* Which console: a Wii when asked for one, or when the disc is a Wii's.
   * It has to be settled before the machine is made (wii.h). */
  gc_console_select(wii);

  m = gc_machine_create();
  if (!m) {
    fprintf(stderr, "gc_run: out of memory\n");
    free(data);
    return 1;
  }
  m->trace_io = trace_io;
  m->stop_on_bad_access = stop_on_bad_access;
  m->watch_address = watch_address;
  gc_cpu_forget_fast(&m->cpu); /* a watched store has to take the long way */
  m->stop_after = stop_after;
  m->stop_at = stop_at;
  {
    /* A controller in the first port. At rest by default, because that is
     * what a game finds on a console nobody is touching; with a known
     * pattern under --pad-test, so that a guest program can say whether
     * what it read is what was put there.
     *
     * The values are deliberately awkward - a negative stick, an off-centre
     * second stick, two different trigger positions - because a controller
     * read back through the wrong sign or the wrong bias still looks
     * plausible at rest and only shows up when something is held. */
    GcPad pad;
    memset(&pad, 0, sizeof(pad));
    pad.present = 1;
    if (pad_test) {
      pad.buttons = (uint16_t)(GC_PAD_A | GC_PAD_START | GC_PAD_LEFT);
      pad.stick_x = -100;
      pad.stick_y = 75;
      pad.c_stick_x = 40;
      pad.c_stick_y = -30;
      pad.trigger_l = 200;
      pad.trigger_r = 100;
    }
    gc_pad_set(m, 0, &pad);
  }
  if (wii) {
    /* And a remote, on a Wii: switched on and lying still by default,
     * because a Wii with no remote is a console nobody can use. Under
     * --pad-test it is held the way tests/wii/programs/wii_padtest.c
     * expects, and awkwardly for the same reason as the pad above: four
     * buttons that share no byte, a remote rolled and tipped so that every
     * axis reads something different, a pointer off-centre both ways, and
     * an extension whose stick and buttons are not at rest. */
    WiiRemoteInput remote;
    wii_remote_rest(&remote, 1);
    if (pad_test) {
      remote.buttons = (uint16_t)(WII_REMOTE_A | WII_REMOTE_B | WII_REMOTE_PLUS | WII_REMOTE_LEFT);
      remote.accel_x = 500;
      remote.accel_y = -300;
      remote.accel_z = 812;
      remote.pointer_visible = 1;
      remote.pointer_x = 19661; /* three tenths of the way across */
      remote.pointer_y = 45875; /* seven tenths of the way down */
    }
    if (pad_test == 1) {
      remote.extension = WII_EXTENSION_NUNCHUK;
      remote.nunchuk_stick_x = -64;
      remote.nunchuk_stick_y = 96;
      remote.nunchuk_buttons = WII_NUNCHUK_Z;
      remote.nunchuk_accel_x = -250;
      remote.nunchuk_accel_y = 400;
      remote.nunchuk_accel_z = 880;
    } else if (pad_test == 2) {
      remote.extension = WII_EXTENSION_CLASSIC;
      remote.classic_left_x = 100;
      remote.classic_left_y = -50;
      remote.classic_right_x = -90;
      remote.classic_right_y = 30;
      remote.classic_trigger_l = 200;
      remote.classic_trigger_r = 80;
      remote.classic_buttons = (uint16_t)(WII_CLASSIC_A | WII_CLASSIC_X | WII_CLASSIC_ZL | WII_CLASSIC_MINUS |
                                          WII_CLASSIC_UP | WII_CLASSIC_R);
    }
    wii_remote_set(m, 0, &remote);
  }
  m->keep_branch_history = branches;
  if (profile) {
    m->profile = (uint32_t *)calloc(GC_MEM1_SIZE / 4u, sizeof(uint32_t));
    m->profile_every = 97u; /* a prime, so a loop of any length is sampled evenly */
  }

  /* The card, if it was asked for. A failure here is fatal rather than a
   * quiet fall back to the reference rasteriser: somebody who passed --gl
   * wants to know what the card does, and silently measuring the software
   * path instead would make every comparison meaningless. */
  if (use_gl) {
    memset(&gl_options, 0, sizeof(gl_options));
    gl_options.fast = use_gl == 2;
    gl_options.scale = gl_scale;
    gl_options.anisotropy = gl_anisotropy;
  }
  if (use_gl && gc_gl_start_with(m, &gl_options) != 0) {
    fprintf(stderr, "gc_run: --gl was asked for and there is no OpenGL path\n");
    gc_machine_destroy(m);
    free(data);
    return 1;
  }

  gc_install_boot_state(m);
  /* The command stream draws on a thread of its own unless GC_GX_THREAD=0. */
  gc_gx_thread_start(m);

  /* The disc goes in before the program runs, because the state it leaves
   * in low memory is state the program reads on its way up. */
  if (disc_path) {
    disc = gc_disc_open(disc_path, stderr);
    if (!disc || gc_attach_disc(m, disc) != 0) {
      fprintf(stderr, "gc_run: cannot use %s as a disc\n", disc_path);
      gc_disc_close(disc);
      gc_machine_destroy(m);
      free(data);
      return 1;
    }
    if (!quiet) {
      fprintf(stderr, "gc_run: disc %s, %u entries\n", disc->game_code,
              disc->entry_count ? disc->entry_count - 1u : 0u);
    }
  }

  /* A disc handed over as the program means "boot this game": its own DOL
   * is the executable, and the disc it came from is the disc. Saying it
   * twice on the command line would be the other way to ask. */
  if (!disc) {
    if (program_disc) {
      void *dol = NULL;
      uint32_t dol_size = 0;
      disc = gc_disc_open(program, quiet ? NULL : stderr);
      if (!disc || gc_attach_disc(m, disc) != 0) {
        fprintf(stderr, "gc_run: cannot boot %s\n", program);
        gc_disc_close(disc);
        gc_machine_destroy(m);
        free(data);
        return 1;
      }
      free(data);
      data = NULL;
      if (!quiet) {
        fprintf(stderr, "gc_run: booting %s from %s, %u entries\n", disc->game_code, program,
                disc->entry_count ? disc->entry_count - 1u : 0u);
      }
      /* The disc's own apploader loads the game, which is how a console
       * does it and the only way to get the memory layout the game
       * expects. Reading the DOL out ourselves is the fallback, and it is
       * a fallback rather than the plan because it cannot know where the
       * game's stack is and so cannot say where free memory begins. */
      if (!no_apploader && gc_run_apploader(m, disc, &entry) == 0) {
        booted_by_apploader = 1;
      } else if (gc_machine_reset_after_failed_boot(m),
                 gc_disc_read_dol(disc, &dol, &dol_size) != 0) {
        fprintf(stderr, "gc_run: cannot read the game out of %s\n", program);
        gc_disc_close(disc);
        gc_machine_destroy(m);
        return 1;
      } else {
        data = dol;
        size = dol_size;
      }
    }
  }

  if (!booted_by_apploader &&
      gc_load_elf(m, data, size, &entry) != 0 && gc_load_dol(m, data, size, &entry) != 0) {
    fprintf(stderr, "gc_run: %s is not an ELF or a DOL this machine can load\n", program);
    gc_disc_close(disc);
    gc_machine_destroy(m);
    free(data);
    return 1;
  }
  free(data);

  /* A program booted off a disc has no symbols of its own; its ELF, when
   * there is one to name, says where the functions the runner answers are. */
  if (symbols_path) {
    size_t symbols_size = 0;
    void *symbols = read_file(symbols_path, &symbols_size);
    if (!symbols || gc_elf_symbols(m, symbols, symbols_size) != 0) {
      fprintf(stderr, "gc_run: cannot read symbols from %s\n", symbols_path);
      free(symbols);
      gc_disc_close(disc);
      gc_machine_destroy(m);
      return 1;
    }
    free(symbols);
  }

  if (!quiet) {
    fprintf(stderr, "gc_run: entry %08x, OSReport %s\n", entry,
            m->osreport_addr ? "hooked" : "not found");
  }

  m->cpu.pc = entry;
  m->cpu.npc = entry + 4;
  /* The stack and the small-data pointers a program expects from its
   * loader. libogc sets its own almost immediately; a bare program may
   * not. */
  m->cpu.gpr[1] = 0x816ffff0u;
  m->cpu.gpr[2] = 0x81100000u;
  m->cpu.gpr[13] = 0x81100000u;

  /* A Wii's flash memory, kept in a file when one is named (and only then:
   * without --nand it is a new console's, in memory, every run). */
  if (nand_path && wii_state(m) && wii_nand_attach(m, nand_path, stderr) != 0) {
    fprintf(stderr, "gc_run: %s is not a flash memory file this machine can use\n", nand_path);
    gc_machine_destroy(m);
    return 1;
  }

  if (card_path && gc_card_insert(m, card_path) != 0) {
    fprintf(stderr, "gc_run: %s is not a memory card image this machine can use\n", card_path);
    gc_machine_destroy(m);
    return 1;
  }

  if (audio_path) {
    audio_file = fopen(audio_path, "wb");
    if (!audio_file) {
      fprintf(stderr, "gc_run: cannot write %s\n", audio_path);
      gc_machine_destroy(m);
      return 1;
    }
  }

  if (audio_file || (frame_path && frame_every)) {
    /* A field at a time, because the machine's ring holds a second and a
     * run of any length would overrun it. What comes out is raw 16-bit
     * stereo at 32 kHz, which is what the converter was being fed - no
     * header, because a header would be a guess about what the reader
     * wants and `ffmpeg -f s16le -ar 32000 -ac 2` is not a hardship. */
    int16_t block[2048 * 2];
    while (m->field < frames && !m->finished && !m->cpu.stopped) {
      unsigned got;
      gc_machine_run(m, m->field + 1u);
      while (audio_file && (got = gc_audio_take(m, block, 2048u)) > 0) {
        fwrite(block, 4u, got, audio_file);
      }
      /* A frame every so often, for following something that moves - an
       * intro, a fade - rather than only the moment the run ended. */
      if (frame_every && m->field % frame_every == 0) {
        char numbered[1024];
        size_t stem = strlen(frame_path);
        if (stem > 4 && !strcmp(frame_path + stem - 4, ".ppm")) stem -= 4;
        snprintf(numbered, sizeof(numbered), "%.*s-%06llu.ppm", (int)stem, frame_path,
                 (unsigned long long)m->field);
        gc_efb_write_frame(m, numbered);
      }
    }
    if (audio_file) fclose(audio_file);
  } else {
    gc_machine_run(m, frames);
  }

  if (!quiet) {
    fprintf(stderr, "gc_run: %s\n", m->finished ? "the program stopped"
                                  : m->cpu.stopped ? "the core stopped"
                                                   : "the frame limit was reached");
    if (m->cpu.stopped == GC_STOP_BAD_ACCESS) {
      /* Everything that might name the culprit: where it happened, what
       * address it wanted, who called this function, and the registers a
       * pointer would have arrived in. */
      unsigned r;
      fprintf(stderr, "gc_run: stopped at the first bad access: ea %08x, pc %08x, lr %08x\n",
              m->cpu.stop_insn, m->cpu.pc, m->cpu.lr);
      if (m->keep_branch_history) {
        /* How control got here, most recent last. A stack frame cannot
         * answer that after a jump through a pointer nobody set. */
        unsigned k;
        fprintf(stderr, "gc_run:   the last branches taken:\n");
        for (k = 0; k < GC_BRANCH_HISTORY; k++) {
          const unsigned at = (m->branch_next + k) % GC_BRANCH_HISTORY;
          if (!m->branch_from[at] && !m->branch_to[at]) continue;
          fprintf(stderr, "gc_run:     %08x -> %08x\n", m->branch_from[at], m->branch_to[at]);
        }
      }
      /* And the way in. A PowerPC frame points at the one before it and
       * keeps the return address one word past that, so the chain can be
       * walked without symbols - which a stripped commercial DOL does not
       * have and which is exactly when this is needed. */
      {
        uint32_t frame = m->cpu.gpr[1];
        unsigned depth;
        fprintf(stderr, "gc_run:   called from:\n");
        for (depth = 0; depth < 16u && frame >= 0x80000000u && frame < 0x81800000u; depth++) {
          const uint8_t *here = gc_machine_ram(m, frame & 0x03ffffffu, 8);
          uint32_t next, back;
          if (!here) break;
          next = ((uint32_t)here[0] << 24) | ((uint32_t)here[1] << 16) |
                 ((uint32_t)here[2] << 8) | here[3];
          if (next <= frame || next < 0x80000000u || next >= 0x81800000u) break;
          {
            const uint8_t *saved = gc_machine_ram(m, (next + 4u) & 0x03ffffffu, 4);
            if (!saved) break;
            back = ((uint32_t)saved[0] << 24) | ((uint32_t)saved[1] << 16) |
                   ((uint32_t)saved[2] << 8) | saved[3];
          }
          if (!back) break;
          fprintf(stderr, "gc_run:     %08x\n", back);
          frame = next;
        }
      }
      {
        /* Any readable text an argument register or a nearby stack word
         * points at. A game about to report something puts the message in
         * an argument register or, if it went through a varargs wrapper,
         * on its own stack - and the message is usually the whole answer.
         *
         * The test for "text" is deliberately strict: printable, at least
         * eight characters, and terminated. A run of bytes that happens to
         * be printable is common; one that is also terminated is not. */
        unsigned which;
        uint32_t candidates[40];
        unsigned count = 0;
        for (which = 3u; which <= 10u && count < 40u; which++) {
          candidates[count++] = m->cpu.gpr[which];
        }
        for (which = 0; which < 32u && count < 40u; which++) {
          const uint32_t at = m->cpu.gpr[1] + which * 4u;
          const uint8_t *word = (at >= 0x80000000u && at < 0x81800000u)
                                    ? gc_machine_ram(m, at & 0x03ffffffu, 4)
                                    : NULL;
          if (!word) continue;
          candidates[count++] = ((uint32_t)word[0] << 24) | ((uint32_t)word[1] << 16) |
                                ((uint32_t)word[2] << 8) | word[3];
        }
        for (which = 0; which < count; which++) {
          const uint32_t address = candidates[which];
          const uint8_t *text;
          unsigned length = 0, seen;
          if (address < 0x80000000u || address >= 0x81800000u) continue;
          text = gc_machine_ram(m, address & 0x03ffffffu, 1);
          if (!text) continue;
          while (length < 96u && text[length] >= 0x20u && text[length] < 0x7fu) length++;
          if (length < 8u || text[length] != 0) continue;
          for (seen = 0; seen < which; seen++) {
            if (candidates[seen] == address) break;
          }
          if (seen != which) continue;
          fprintf(stderr, "gc_run:   %08x is \"%.*s\"\n", address, (int)length,
                  (const char *)text);
        }
      }
      {
        /* The block address translations, because "untranslated" means no
         * block covered the address and the next question is always which
         * blocks there were. */
        unsigned b;
        fprintf(stderr, "gc_run:   sdr1 %08x  segments %08x %08x %08x %08x\n",
                m->cpu.sdr1, m->cpu.sr[0], m->cpu.sr[7], m->cpu.sr[8], m->cpu.sr[12]);
        fprintf(stderr, "gc_run:   data blocks:");
        for (b = 0; b < 8u; b += 2u) {
          fprintf(stderr, " %08x/%08x", m->cpu.dbat[b], m->cpu.dbat[b + 1u]);
        }
        fprintf(stderr, "\n");
      }
      for (r = 0; r < 32u; r += 4u) {
        fprintf(stderr, "gc_run:   r%-2u %08x  r%-2u %08x  r%-2u %08x  r%-2u %08x\n", r,
                m->cpu.gpr[r], r + 1u, m->cpu.gpr[r + 1u], r + 2u, m->cpu.gpr[r + 2u], r + 3u,
                m->cpu.gpr[r + 3u]);
      }
    }
    if (m->profile) {
      /* The twenty addresses the program counter was seen at most often.
       * A game that is stuck sits in a handful of them. */
      unsigned best[20], count = 0, k;
      size_t i2;
      memset(best, 0, sizeof(best));
      for (i2 = 0; i2 < GC_MEM1_SIZE / 4u; i2++) {
        if (!m->profile[i2]) continue;
        for (k = 0; k < 20u; k++) {
          if (m->profile[i2] > m->profile[best[k]]) {
            unsigned j;
            for (j = 19u; j > k; j--) best[j] = best[j - 1u];
            best[k] = (unsigned)i2;
            if (count < 20u) count++;
            break;
          }
        }
      }
      fprintf(stderr, "gc_run: where the program counter was, most often first:\n");
      for (k = 0; k < count; k++) {
        if (!m->profile[best[k]]) break;
        fprintf(stderr, "gc_run:   %08x  %u\n", 0x80000000u + best[k] * 4u,
                m->profile[best[k]]);
      }
    }
    fprintf(stderr, "gc_run: %llu fields, %llu instructions, pc %08x\n",
            (unsigned long long)m->field, (unsigned long long)m->cpu.cycles, m->cpu.pc);
    if (m->unknown_io_reads || m->unknown_io_writes) {
      fprintf(stderr, "gc_run: %u reads and %u writes of registers nobody has described\n",
              m->unknown_io_reads, m->unknown_io_writes);
    }
    if (m->bad_access) {
      fprintf(stderr, "gc_run: %u accesses to addresses that are not memory\n", m->bad_access);
    }
    if (gc_ax_lists(m)) {
      /* What the sound microcode was asked to do, and how much of it it
       * understood. A game whose command lists all end early is one whose
       * sound is silence for a reason that can be found. */
      fprintf(stderr, "gc_run: sound: %llu command lists, %llu stopped early\n",
              (unsigned long long)gc_ax_lists(m), (unsigned long long)gc_ax_bad_lists(m));
      fprintf(stderr, "gc_run: sound: %llu voices walked, %llu of them playing\n",
              (unsigned long long)gc_ax_voices(m), (unsigned long long)gc_ax_voices_running(m));
    }
    gc_gx_thread_sync(m); /* the pixel count is the graphics thread's */
    if (m->gx.bytes) {
      fprintf(stderr,
              "gc_run: graphics: %llu bytes, %llu commands, %llu primitives, "
              "%llu vertices, %llu display lists\n",
              (unsigned long long)m->gx.bytes, (unsigned long long)m->gx.commands,
              (unsigned long long)m->gx.primitives, (unsigned long long)m->gx.vertices,
              (unsigned long long)m->gx.display_lists);
    }
    if (m->gx.triangles) {
      fprintf(stderr,
              "gc_run: graphics: %llu triangles, %llu pixels, %llu copies\n",
              (unsigned long long)m->gx.triangles, (unsigned long long)m->gx.pixels,
              (unsigned long long)m->efb.copies);
    }
    if (m->gx.unknown_commands || m->gx.truncated_lists) {
      fprintf(stderr,
              "gc_run: graphics: %u commands nobody could parse, %u truncated lists\n",
              m->gx.unknown_commands, m->gx.truncated_lists);
    }
    if (m->unknown_disc_commands) {
      fprintf(stderr, "gc_run: %u disc commands nobody has described\n",
              m->unknown_disc_commands);
    }
  }

  if (m->cpu.stopped == GC_STOP_ILLEGAL || m->cpu.stopped == GC_STOP_UNHANDLED) {
    fprintf(stderr, "gc_run: the core stopped on %08x at pc %08x (%s)\n", m->cpu.stop_insn,
            m->cpu.pc,
            m->cpu.stopped == GC_STOP_ILLEGAL ? "not a Gekko instruction" : "not implemented");
    status = 1;
  }
  if (m->bad_access && !allow_bad_access) status = 1;

  if (frame_path) {
    if (gc_efb_write_frame(m, frame_path) != 0) {
      fprintf(stderr, "gc_run: nothing was copied out for display, so %s was not written\n",
              frame_path);
      status = 1;
    } else if (!quiet) {
      unsigned width = 0, height = 0;
      gc_efb_frame_rgba(m, NULL, 0, 0, &width, &height);
      fprintf(stderr, "gc_run: %s, %ux%u\n", frame_path, width, height);
    }
  }

  if (picture_path && gc_gl_write_picture(m, picture_path) != 0) {
    fprintf(stderr, "gc_run: there is no larger picture of what is on screen (--gl-fast --gl-scale 2 or more, and "
                    "a display copy), so %s was not written\n",
            picture_path);
    status = 1;
  }

  if (state_path && dump_state(m, state_path) != 0) {
    fprintf(stderr, "gc_run: cannot write %s\n", state_path);
    status = 1;
  }

  gc_machine_destroy(m);
  gc_disc_close(disc);
  return status;
}
