#ifndef GC_ZELDA_H
#define GC_ZELDA_H

#include <stdint.h>

struct GcMachine;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The other sound microcode: the one Nintendo's own games upload.
 *
 * AX (gc_ax.h) is the microcode the SDK gives everybody. Nintendo's own
 * sound library carries a different one, usually called "Zelda" after the
 * game it was first studied in: The Wind Waker, Super Mario Sunshine and
 * Pikmin on a GameCube, Twilight Princess and both Super Mario Galaxy games
 * on a Wii. It shares nothing with AX but the mailbox. Where AX is handed a
 * list of commands in memory and answers once, this one is fed its commands
 * a word at a time through the mailbox, acknowledges each, and while it
 * renders asks the game, sixteen voices at a time, which voices to play.
 *
 * A game that gets AX's greeting from this microcode waits for ever for the
 * second word of the real one, which is how Super Mario Galaxy 2 stopped
 * before this file existed.
 *
 * Two files. gc_zelda.c is the conversation: the greeting, the commands,
 * the acknowledgements, the per-frame and per-sixteen-voices
 * synchronisation, the end of a frame, and the hand-over to another
 * microcode. gc_zelda_mix.c is the sound: each voice's samples fetched and
 * decoded, resampled, filtered and mixed into the frame's buffers with its
 * volumes, the reverb, and the frame written out. docs/gc-dsp-zelda.md has
 * both, where each part comes from, and what is still not done (the early
 * members' protocol, a variable filter, the back channels' fold-down).
 */

/* What differs between the family's members (docs/gc-dsp-zelda.md, "The
 * family"). */
enum {
  GC_ZELDA_NO_ARAM = 0x001u,              /* a Wii's: samples come from main memory */
  GC_ZELDA_MAKE_DOLBY_LOUDER = 0x002u,    /* a placed voice's gains have one bit more */
  GC_ZELDA_LIGHT_PROTOCOL = 0x004u,       /* the early members' other conversation: not written */
  GC_ZELDA_FOUR_MIXING_DESTS = 0x008u,    /* a voice has four destinations, not six */
  GC_ZELDA_TINY_VPB = 0x010u,             /* a shorter voice block: not written */
  GC_ZELDA_VOLUME_EXPLICIT_STEP = 0x020u, /* a destination gives its volume's step, not its target */
  GC_ZELDA_SYNC_PER_FRAME = 0x040u,       /* asks which voices once a frame, not per sixteen */
  GC_ZELDA_NO_CMD_0D = 0x080u,            /* command 0x0d takes no word */
  GC_ZELDA_SUPPORTS_GBA_CRYPTO = 0x100u,  /* also signs a Game Boy Advance's program: not written */
  GC_ZELDA_WEIRD_CMD_0C = 0x200u,         /* command 0x0c is something else: not written */
  GC_ZELDA_COMBINED_CMD_0D = 0x400u       /* the render command carries two more words */
};

enum { GC_ZELDA_WAITING, GC_ZELDA_RENDERING, GC_ZELDA_WRITING_COMMAND, GC_ZELDA_HALTED };

#define GC_ZELDA_COMMAND_WORDS 64u
#define GC_ZELDA_MAIL_QUEUE 1024u
#define GC_ZELDA_SYNC_GROUPS 256u
/* A frame is eighty samples; a voice mixes into any of thirteen buffers of
 * them (gc_zelda_mix.c names them). */
#define GC_ZELDA_FRAME 0x50u
#define GC_ZELDA_BUFFERS 13u

/* Plain data, so that a save state can carry it; it lives inside the sound
 * state block (gc_ax.c), because the machine's own structure cannot grow
 * without turning every existing save state away. */
typedef struct GcZelda {
  uint32_t active; /* the microcode running is one of this family */
  uint32_t hash;
  uint32_t flags;
  uint32_t mail_state;
  uint32_t expected_command_mails;

  /* Which voices the game has said to render: up to what voice number, and
   * one bit per voice in groups of sixteen, the first voice in the top bit. */
  uint32_t sync_max_voice;
  uint32_t sync_second_half;
  uint16_t sync_flags[GC_ZELDA_SYNC_GROUPS];

  /* Commands as the mailbox delivered them, a ring of words. */
  uint32_t command[GC_ZELDA_COMMAND_WORDS];
  uint32_t command_read;
  uint32_t command_write;
  uint32_t pending_commands;
  uint32_t can_execute; /* cleared from the end of a frame until the game answers it */

  uint32_t requested_frames;
  uint32_t voices_per_frame;
  uint32_t current_frame;
  uint32_t current_voice;

  /* What the set-up and render commands named. Addresses are the game's. */
  uint32_t voice_blocks;
  uint32_t tables;
  uint32_t afc_coefficients;
  uint32_t reverb_blocks;
  uint32_t sample_base;
  uint32_t output_volume;
  uint32_t output_left;
  uint32_t output_right;

  /* The hand-over to another microcode: ten words after the game says so. */
  uint32_t upload_in_progress;
  uint32_t upload_step;
  uint32_t upload[10];

  /* Mail owed to the game, oldest first. A mail marked as interrupting
   * raises the DSP interrupt when it reaches the mailbox. */
  uint32_t queue_head;
  uint32_t queue_count;
  uint32_t staged_interrupts; /* whether the mail the machine is holding back interrupts */
  uint32_t reserved;
  struct {
    uint32_t mail;
    uint32_t interrupts;
  } queue[GC_ZELDA_MAIL_QUEUE];

  /* For the runner's summary and for whoever wonders why it is quiet. */
  uint64_t commands;
  uint64_t frames;
  uint64_t voices_asked_for;
  uint64_t dropped_mail;

  /* --- the mixer (gc_zelda_mix.c) --- */
  /* The tables the set-up command points at, copied when it is given:
   * four-tap resampling coefficients for sixty-four positions between two
   * samples, four wave patterns of sixty-four samples, a quarter sine for
   * surround positions, and sixteen pairs of decoder coefficients. */
  int16_t resampling[0x100];
  int16_t patterns[0x100];
  int16_t sine[0x80];
  int16_t afc[0x20];
  /* The frame being mixed. Some of these carry over from one frame to the
   * next (the reverb's), so they are state and not scratch. */
  int16_t buffer[GC_ZELDA_BUFFERS][GC_ZELDA_FRAME];
  /* The reverb: where each of its four delay lines has got to, and the
   * eight samples before a frame that its filter reaches back for. */
  uint16_t reverb_frame[4];
  int16_t reverb_last8[4][8];
  uint32_t prepared; /* this frame's buffers have been set up */
  uint32_t unknown_sources_said;
  uint64_t voices_mixed;
  uint64_t voices_finished;
  uint64_t voices_placed;  /* mixed by position (the surround path), not by destination */
  uint64_t unknown_sources;
  uint64_t sources_used;   /* one bit per sample source number a voice has had */
  uint64_t reverb_frames;  /* frames a reverb line was on for */
} GcZelda;

/* Whether `hash` (gc_dsp_ucode_hash) is one of the family, and its traits. */
int gc_zelda_knows(uint32_t hash, uint32_t *flags);

/* The microcode has just been started. Returns zero when it is a member
 * this can speak to, and non-zero for the light protocol's members. */
int gc_zelda_boot(struct GcMachine *m, uint32_t hash);

/* The microcode running is one of the family. */
int gc_zelda_active(const struct GcMachine *m);

/* What gc_zelda_mail asks of the caller, which owns the ROM and the
 * microcode upload. */
enum {
  GC_ZELDA_MAIL_DONE = 0,
  GC_ZELDA_MAIL_TO_ROM = 1,   /* the game sent the microcode back to the ROM */
  GC_ZELDA_MAIL_NEW_UCODE = 2 /* ten words named another microcode: see gc_zelda_handover */
};

/* A word from the game. */
int gc_zelda_mail(struct GcMachine *m, uint32_t mail);

/* The microcode the game handed over to: where it is in main memory and how
 * long. Valid after gc_zelda_mail returned GC_ZELDA_MAIL_NEW_UCODE. */
void gc_zelda_handover(const struct GcMachine *m, uint32_t *address, uint32_t *length);

/* The game has come back to this microcode from another: it is told so. */
void gc_zelda_resume(struct GcMachine *m);

/* The next mail owed to the game, if there is one. */
int gc_zelda_next_mail(struct GcMachine *m, uint32_t *mail, int *interrupts);

uint64_t gc_zelda_frames(const struct GcMachine *m);
uint64_t gc_zelda_commands(const struct GcMachine *m);
uint64_t gc_zelda_voices_mixed(const struct GcMachine *m);
uint64_t gc_zelda_voices_finished(const struct GcMachine *m);

/* The mixer (gc_zelda_mix.c), called by the conversation: the tables a
 * set-up command names, the start of a frame, one voice, and the end of a
 * frame, which writes it out and moves the output addresses on. */
void gc_zelda_mix_setup(struct GcMachine *m, GcZelda *z);
void gc_zelda_mix_prepare(struct GcMachine *m, GcZelda *z);
void gc_zelda_mix_voice(struct GcMachine *m, GcZelda *z, unsigned voice);
void gc_zelda_mix_finish(struct GcMachine *m, GcZelda *z);

/* The identification both this and the log use: every byte of the uploaded
 * microcode folded into a word, exclusive-or then a rotation left by three.
 * It is the one Dolphin's DSP-HLE uses, chosen so that the numbers here are
 * the numbers its tables and everyone's notes already use. */
uint32_t gc_dsp_ucode_hash(const uint8_t *bytes, uint32_t length);

/* The family's state inside the sound state block (gc_ax.c). NULL only when
 * memory ran out. */
GcZelda *gc_ax_zelda(struct GcMachine *m);

#ifdef __cplusplus
}
#endif

#endif /* GC_ZELDA_H */
