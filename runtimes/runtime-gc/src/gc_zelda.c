/*
 * The "Zelda" sound microcode's conversation with a game (gc_zelda.h).
 *
 * Written from docs/gc-dsp-zelda.md, which was compiled from Dolphin's
 * DSP-HLE read as a description of behaviour; nothing here is its code.
 * There is no hardware oracle for it in this repository. What stands behind
 * each part is in the document: the protocol has a retail game's own
 * library on the other end of it (Super Mario Galaxy 2 gets from the
 * greeting to rendering frames, which it could not with one word of it
 * wrong). The sound is gc_zelda_mix.c.
 */
#include <stdio.h>
#include <string.h>

#include "gc.h"
#include "gc_zelda.h"

/* Mail to the game. Four of the task mails every microcode shares
 * (docs/gc-dsp-ax.md, A.5) and one of this family's own. */
#define MAIL_INIT 0xdcd10000u
#define MAIL_RESUME 0xdcd10001u
#define MAIL_SYNC 0xdcd10004u
#define MAIL_FRAME_END 0xdcd10005u
#define MAIL_HANDSHAKE 0xf3551111u
#define MAIL_SYNC_VALUE 0xf3550000u

/* Mail from the game that is not a command. */
#define TASK_PREFIX 0xcdd10000u
#define TASK_NEW_UCODE 0xcdd10001u
#define TASK_RESET 0xcdd10002u
#define TASK_CONTINUE 0xcdd10003u

/* The family: each member's identification and what it does differently
 * (docs/gc-dsp-zelda.md, "The family"). */
#define FLAG_MAKE_DOLBY_LOUDER GC_ZELDA_MAKE_DOLBY_LOUDER
#define FLAG_FOUR_MIXING_DESTS GC_ZELDA_FOUR_MIXING_DESTS
#define FLAG_TINY_VPB GC_ZELDA_TINY_VPB
#define FLAG_VOLUME_EXPLICIT_STEP GC_ZELDA_VOLUME_EXPLICIT_STEP
#define FLAG_NO_CMD_0D GC_ZELDA_NO_CMD_0D
#define FLAG_SUPPORTS_GBA_CRYPTO GC_ZELDA_SUPPORTS_GBA_CRYPTO
#define FLAG_WEIRD_CMD_0C GC_ZELDA_WEIRD_CMD_0C

static const struct {
  uint32_t hash;
  uint32_t flags;
} FAMILY[] = {
    /* The GameCube's own menu, NTSC and PAL. */
    {0x24b22038u, GC_ZELDA_LIGHT_PROTOCOL | FLAG_FOUR_MIXING_DESTS | FLAG_TINY_VPB |
                      FLAG_VOLUME_EXPLICIT_STEP | FLAG_NO_CMD_0D | FLAG_WEIRD_CMD_0C},
    {0x6ba3b3eau, GC_ZELDA_LIGHT_PROTOCOL | FLAG_FOUR_MIXING_DESTS | FLAG_NO_CMD_0D},
    /* Pikmin's demo; Pikmin and Animal Crossing; Luigi's Mansion. */
    {0xdf059f68u, GC_ZELDA_LIGHT_PROTOCOL | FLAG_NO_CMD_0D | FLAG_SUPPORTS_GBA_CRYPTO},
    {0x4be6a5cbu, GC_ZELDA_LIGHT_PROTOCOL | FLAG_NO_CMD_0D | FLAG_SUPPORTS_GBA_CRYPTO},
    {0x42f64ac4u, GC_ZELDA_LIGHT_PROTOCOL | FLAG_NO_CMD_0D | FLAG_WEIRD_CMD_0C},
    /* Pikmin (PAL); Super Mario Sunshine. */
    {0x267fd05au, GC_ZELDA_SYNC_PER_FRAME | FLAG_NO_CMD_0D},
    {0x56d36052u, GC_ZELDA_SYNC_PER_FRAME | FLAG_NO_CMD_0D},
    /* The Wind Waker. */
    {0x86840740u, 0u},
    /* Four Swords Adventures, Mario Kart: Double Dash, Pikmin 2. */
    {0x2fcdf1ecu, FLAG_MAKE_DOLBY_LOUDER},
    /* Twilight Princess on a GameCube, Donkey Kong Jungle Beat. */
    {0x6ca33a6du, FLAG_MAKE_DOLBY_LOUDER | GC_ZELDA_COMBINED_CMD_0D},
    /* Twilight Princess on a Wii. */
    {0x6c3f6f94u, GC_ZELDA_NO_ARAM | FLAG_MAKE_DOLBY_LOUDER | GC_ZELDA_COMBINED_CMD_0D},
    /* Super Mario Galaxy and Super Mario Galaxy 2. */
    {0xd643001fu, GC_ZELDA_NO_ARAM | FLAG_MAKE_DOLBY_LOUDER | GC_ZELDA_COMBINED_CMD_0D},
    /* Pikmin and Pikmin 2 as reissued for the Wii. */
    {0xb7eb9a9cu, GC_ZELDA_NO_ARAM | FLAG_MAKE_DOLBY_LOUDER | GC_ZELDA_COMBINED_CMD_0D},
    {0xeaeb38ccu, GC_ZELDA_NO_ARAM | FLAG_MAKE_DOLBY_LOUDER | GC_ZELDA_COMBINED_CMD_0D},
};

uint32_t gc_dsp_ucode_hash(const uint8_t *bytes, uint32_t length) {
  uint32_t hash = 0;
  uint32_t i;
  for (i = 0; i < length; i++) {
    hash ^= bytes[i];
    hash = (hash << 3) | (hash >> 29);
  }
  return hash;
}

int gc_zelda_knows(uint32_t hash, uint32_t *flags) {
  unsigned i;
  for (i = 0; i < sizeof(FAMILY) / sizeof(FAMILY[0]); i++) {
    if (FAMILY[i].hash == hash) {
      if (flags) *flags = FAMILY[i].flags;
      return 1;
    }
  }
  return 0;
}

int gc_zelda_active(const struct GcMachine *m) {
  /* Not gc_ax_zelda: asking must not allocate the state of a microcode
   * most games never load. */
  return m && m->ax && gc_ax_zelda((struct GcMachine *)m)->active;
}

uint64_t gc_zelda_frames(const struct GcMachine *m) {
  return m && m->ax ? gc_ax_zelda((struct GcMachine *)m)->frames : 0u;
}

uint64_t gc_zelda_commands(const struct GcMachine *m) {
  return m && m->ax ? gc_ax_zelda((struct GcMachine *)m)->commands : 0u;
}

uint64_t gc_zelda_voices_mixed(const struct GcMachine *m) {
  return m && m->ax ? gc_ax_zelda((struct GcMachine *)m)->voices_mixed : 0u;
}

uint64_t gc_zelda_voices_finished(const struct GcMachine *m) {
  return m && m->ax ? gc_ax_zelda((struct GcMachine *)m)->voices_finished : 0u;
}

/* ------------------------------------------------------------------ */
/* Mail to the game                                                    */

static void push_mail(struct GcMachine *m, GcZelda *z, uint32_t mail, int interrupts) {
  uint32_t at;
  if (z->queue_count == GC_ZELDA_MAIL_QUEUE) {
    /* A game that has stopped reading its mail. Nothing sensible can be
     * done with more of it, and saying so once is worth more than a queue
     * that grows for ever. */
    if (!z->dropped_mail && m->log) {
      fprintf(m->log, "gc: the sound microcode owes the game %u mails it is not reading; "
                      "the rest are dropped\n", GC_ZELDA_MAIL_QUEUE);
    }
    z->dropped_mail++;
    return;
  }
  at = (z->queue_head + z->queue_count) % GC_ZELDA_MAIL_QUEUE;
  z->queue[at].mail = mail;
  z->queue[at].interrupts = interrupts ? 1u : 0u;
  z->queue_count++;
}

int gc_zelda_next_mail(struct GcMachine *m, uint32_t *mail, int *interrupts) {
  GcZelda *z;
  if (!m || !m->ax) return 0;
  z = gc_ax_zelda(m);
  if (!z->queue_count) return 0;
  *mail = z->queue[z->queue_head].mail;
  *interrupts = (int)z->queue[z->queue_head].interrupts;
  z->queue_head = (z->queue_head + 1u) % GC_ZELDA_MAIL_QUEUE;
  z->queue_count--;
  return 1;
}

/* An acknowledgement: the task mail that says "synchronise", interrupting,
 * and then the value being synchronised on. */
static void acknowledge(struct GcMachine *m, GcZelda *z, uint32_t value) {
  push_mail(m, z, MAIL_SYNC, 1);
  push_mail(m, z, MAIL_SYNC_VALUE | (value & 0xffffu), 0);
}

/* ------------------------------------------------------------------ */
/* A frame                                                             */

static int rendering(const GcZelda *z) { return z->current_frame != z->requested_frames; }

/* Goes as far through the requested frames as the game has said which
 * voices to render, and stops to wait for more when it has not. A frame is
 * set up before its first voice (once: this is come back to), each voice
 * the game flags is mixed into it, and it is written out to the addresses
 * the render command gave, which move on by a frame each time. */
static void render(struct GcMachine *m, GcZelda *z) {
  if (!rendering(z)) return;

  while (z->current_frame < z->requested_frames) {
    /* No more voices than there are flags for. What the hardware does with
     * a larger count is not known. */
    uint32_t voices = z->voices_per_frame;
    if (voices > GC_ZELDA_SYNC_GROUPS * 16u) voices = GC_ZELDA_SYNC_GROUPS * 16u;

    gc_zelda_mix_prepare(m, z);
    while (z->current_voice < voices) {
      if (z->current_voice >= z->sync_max_voice) return; /* the game has not said yet */
      if (z->sync_flags[z->current_voice >> 4] & (1u << (15u - (z->current_voice & 15u)))) {
        z->voices_asked_for++;
        gc_zelda_mix_voice(m, z, z->current_voice);
      }
      z->current_voice++;
    }

    acknowledge(m, z, 0xff00u | z->current_frame);
    gc_zelda_mix_finish(m, z);
    z->current_voice = 0;
    z->sync_max_voice = 0;
    z->current_frame++;
  }

  /* Every frame asked for is done. The game answers this one, and nothing
   * more is executed until it has. */
  push_mail(m, z, MAIL_FRAME_END, 1);
  z->can_execute = 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */

static uint32_t take_word(struct GcMachine *m, GcZelda *z) {
  uint32_t word;
  if (z->command_read == z->command_write) {
    if (m->log) fprintf(m->log, "gc: a sound command asked for more words than it was sent\n");
    return 0;
  }
  word = z->command[z->command_read];
  z->command_read = (z->command_read + 1u) % GC_ZELDA_COMMAND_WORDS;
  return word;
}

static void put_word(GcZelda *z, uint32_t word) {
  z->command[z->command_write] = word;
  z->command_write = (z->command_write + 1u) % GC_ZELDA_COMMAND_WORDS;
}

static void halt(struct GcMachine *m, GcZelda *z, const char *why, uint32_t value) {
  if (m->log) fprintf(m->log, "gc: the sound microcode stops: %s (%08x)\n", why, value);
  z->mail_state = GC_ZELDA_HALTED;
}

static void run_commands(struct GcMachine *m, GcZelda *z) {
  /* Not while frames are being rendered, and not between the end of a
   * frame and the game's answer to it. */
  if (rendering(z) || !z->can_execute) return;

  while (z->pending_commands) {
    const uint32_t head = take_word(m, z);
    uint32_t command, sync;
    if (!(head & 0x80000000u)) {
      /* Not the start of a command. With nothing left to read this would
       * never end, so it ends here. */
      if (z->command_read == z->command_write) {
        z->pending_commands = 0;
        return;
      }
      continue;
    }
    command = (head >> 24) & 0x7fu;
    sync = head >> 16;
    z->pending_commands--;
    z->commands++;
    if (m->trace_io && m->log) fprintf(m->log, "gc: sound command %02x (%08x)\n", command, head);

    switch (command) {
      /* Commands that do nothing but are answered. 0x0c and 0x0d are among
       * them for every member spoken to here except where noted. */
      case 0x00:
      case 0x03:
      case 0x0a:
      case 0x0b:
      case 0x0c:
      case 0x0f:
        acknowledge(m, z, sync);
        break;

      /* Set-up: how many voices a frame has and where their parameter
       * blocks are, where the tables the mixing needs are (resampling
       * coefficients, wave patterns and a sine table, one after another),
       * where the sample decoder's coefficients are, and where the reverb's
       * blocks are. The tables are copied now, as the real microcode copies
       * them into its own memory; the blocks are read as they are used. */
      case 0x01:
        z->voices_per_frame = head & 0xffffu;
        z->voice_blocks = take_word(m, z);
        z->tables = take_word(m, z);
        z->afc_coefficients = take_word(m, z);
        z->reverb_blocks = take_word(m, z);
        gc_zelda_mix_setup(m, z);
        acknowledge(m, z, sync);
        break;

      /* Render: how many frames, how loud, and where the left and right go.
       * It is not acknowledged; the frames are, as each is finished, and
       * nothing after it runs until they all are. */
      case 0x02:
        z->requested_frames = (head >> 16) & 0xffu;
        z->output_volume = head & 0xffffu;
        z->output_left = take_word(m, z);
        z->output_right = take_word(m, z);
        if (z->flags & GC_ZELDA_COMBINED_CMD_0D) {
          (void)take_word(m, z);
          (void)take_word(m, z);
        }
        z->current_frame = 0;
        z->current_voice = 0;
        render(m, z);
        return;

      /* One word whose meaning is not known, which members marked as not
       * having the command do not take. */
      case 0x0d:
        if (!(z->flags & FLAG_NO_CMD_0D)) (void)take_word(m, z);
        acknowledge(m, z, sync);
        break;

      /* A Wii has no audio memory: this says where in main memory the game
       * keeps what would have been there. */
      case 0x0e:
        z->sample_base = take_word(m, z);
        acknowledge(m, z, sync);
        break;

      /* 0x04 to 0x09 crash the real microcode, and the rest do not exist. */
      default:
        halt(m, z, "a command it does not have", head);
        return;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Mail from the game                                                  */

int gc_zelda_boot(struct GcMachine *m, uint32_t hash) {
  GcZelda *z = gc_ax_zelda(m);
  uint32_t flags = 0;
  if (!z || !gc_zelda_knows(hash, &flags)) return -1;
  memset(z, 0, sizeof(*z));
  z->hash = hash;
  z->flags = flags;
  if (flags & GC_ZELDA_LIGHT_PROTOCOL) return -1;
  z->active = 1;
  z->mail_state = GC_ZELDA_WAITING;
  z->can_execute = 1;
  /* The greeting: the task mail every microcode starts with, and this
   * family's own word after it, which a game waits for. */
  push_mail(m, z, MAIL_INIT, 1);
  push_mail(m, z, MAIL_HANDSHAKE, 0);
  return 0;
}

void gc_zelda_resume(struct GcMachine *m) {
  GcZelda *z = gc_ax_zelda(m);
  if (z && z->active) push_mail(m, z, MAIL_RESUME, 1);
}

void gc_zelda_handover(const struct GcMachine *m, uint32_t *address, uint32_t *length) {
  const GcZelda *z = gc_ax_zelda((struct GcMachine *)m);
  /* Ten words: where the microcode's data memory is to be saved (three),
   * then the new microcode's code - its address in main memory, its
   * length, where it goes and where it starts - and its data (three). */
  *address = z ? z->upload[3] : 0u;
  *length = z ? (z->upload[4] & 0xffffu) : 0u;
}

int gc_zelda_mail(struct GcMachine *m, uint32_t mail) {
  GcZelda *z = gc_ax_zelda(m);
  if (!z || !z->active) return GC_ZELDA_MAIL_DONE;

  /* Before anything else: the ten words that follow "another microcode". */
  if (z->upload_in_progress) {
    z->upload[z->upload_step++] = mail;
    if (z->upload_step < 10u) return GC_ZELDA_MAIL_DONE;
    z->upload_step = 0;
    z->upload_in_progress = 0;
    return GC_ZELDA_MAIL_NEW_UCODE;
  }

  switch (z->mail_state) {
    case GC_ZELDA_WAITING:
      if (mail & 0x80000000u) {
        /* The game's answer to the end of a frame. The real microcode looks
         * only at the low half, so a word without the prefix means what it
         * would have with it. */
        switch (TASK_PREFIX | (mail & 0xffffu)) {
          case TASK_NEW_UCODE:
            z->can_execute = 1;
            run_commands(m, z);
            z->upload_in_progress = 1;
            z->upload_step = 0;
            break;
          case TASK_RESET:
            z->mail_state = GC_ZELDA_HALTED;
            z->active = 0;
            return GC_ZELDA_MAIL_TO_ROM;
          case TASK_CONTINUE:
            z->can_execute = 1;
            run_commands(m, z);
            break;
          default:
            halt(m, z, "the game told it to", mail);
            break;
        }
      } else if (!(mail & 0xffffu)) {
        /* "Which voices" is about to follow. */
        if (rendering(z)) {
          z->mail_state = GC_ZELDA_RENDERING;
        } else {
          halt(m, z, "it was told which voices to render while rendering nothing", mail);
        }
      } else {
        z->mail_state = GC_ZELDA_WRITING_COMMAND;
        z->expected_command_mails = mail & 0xffffu;
      }
      break;

    case GC_ZELDA_RENDERING:
      if (z->flags & GC_ZELDA_SYNC_PER_FRAME) {
        /* Two words a frame, thirty-two voices each. */
        const unsigned base = z->sync_second_half ? 2u : 0u;
        z->sync_flags[base] = (uint16_t)(mail >> 16);
        z->sync_flags[base + 1u] = (uint16_t)mail;
        if (z->sync_second_half) z->sync_max_voice = 0xffffu;
        render(m, z);
        if (z->sync_second_half) z->mail_state = GC_ZELDA_WAITING;
        z->sync_second_half = !z->sync_second_half;
      } else {
        /* One word for sixteen voices: which sixteen, and a bit for each. */
        z->sync_max_voice = (((mail >> 16) & 0xfu) + 1u) << 4;
        z->sync_flags[(mail >> 16) & 0xffu] = (uint16_t)mail;
        render(m, z);
        z->mail_state = GC_ZELDA_WAITING;
      }
      break;

    case GC_ZELDA_WRITING_COMMAND:
      put_word(z, mail);
      if (--z->expected_command_mails == 0u) {
        z->pending_commands++;
        z->mail_state = GC_ZELDA_WAITING;
        run_commands(m, z);
      }
      break;

    default: /* halted */
      if (m->trace_io && m->log) {
        fprintf(m->log, "gc: mail %08x to a sound microcode that has stopped\n", mail);
      }
      break;
  }
  return GC_ZELDA_MAIL_DONE;
}
