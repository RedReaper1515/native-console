/*
 * Save states: the whole machine in a file, loaded back so that execution
 * continues exactly as if it had never stopped.
 *
 * "Exactly" is meant literally and is tested literally (docs/GC-PLAN.md,
 * "Save states"): a run that saves at field K and is resumed in a fresh
 * process ends at the same instruction count, with the same memory, the same
 * frames and the same sound as a run that went straight through - under the
 * recompiled code, under the interpreter, and across the two.
 *
 * The format is a fixed header and then a list of sections, each a tag, a
 * version and a length, so a reader can say exactly what is wrong with a file
 * rather than misread it:
 *
 *   header   "NCGCSTAT", format version, header size, the layout
 *            fingerprint (the sizes and offsets of every structure written
 *            whole), the game's identifier, a hash of the program image, a
 *            hash of the disc's table of contents, the runtime build that
 *            wrote it, the field and instruction count, and the payload's
 *            length, stored length, compression and checksum.
 *   payload  MACH (GcMachine, pointers and host settings zeroed), MEM1, ARAM,
 *            EFBC and EFBZ (the framebuffer planes), PADS, CARD (the memory
 *            card's image and the command it is in the middle of), AX (the
 *            sound microcode's buffers), AUDI (the played-sound ring), END.
 *
 * A state loads only into the same game (identifier and program image), the
 * same disc, and a runtime whose structures have the same layout; anything
 * else is refused with a message naming what differs, and the machine is left
 * untouched - a load is checked in full before anything is written.
 *
 * What is deliberately *not* in a state, because none of it changes what the
 * guest computes: the recompiled dispatcher's table, the shortcut maps to
 * main memory (rebuilt from the BATs on the next access, which the GL path
 * already relies on), cpu.next_event (every run of recompiled code starts by
 * clearing it), the rasteriser's per-thread row memos and the compressed
 * texture memo (keyed by their whole input), the OS hooks (set from the
 * program's own symbols when it boots), coverage, logs, the profile, and the
 * frontend. The disc image is not saved; which disc it was is.
 *
 * States are taken between fields, from the thread that runs the machine.
 * Nothing is in flight then: the raster helpers finish every job before the
 * triangle that started it returns.
 */
#ifndef GC_STATE_H
#define GC_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GcMachine;

/* Bumped whenever what a section means changes. Format 2 added the name and
 * the time of saving to the header; format 1 states still load, and read as
 * unnamed with no time. */
#define GC_STATE_VERSION 2u

/* A state's name, as the player typed it: bytes, including the terminator. */
#define GC_STATE_NAME_CHARS 64u

/* Which game a state belongs to. */
typedef struct GcStateIdentity {
  char game_id[16];      /* the disc's six-character code, or the program's name */
  uint64_t program_hash; /* the program image the code was built from; 0 for none */
  uint64_t disc_hash;    /* the disc's header and table of contents; 0 for no disc */
} GcStateIdentity;

/* The identity of the machine as it is now: the disc in its drive, and the
 * program image (null when there is none, as for gc_play on a disc). */
void gc_state_identity(GcStateIdentity *out, const struct GcMachine *m, const char *name,
                       const uint8_t *image, size_t image_size);

/* A growing byte buffer, for the subsystems' own save functions. */
typedef struct GcStateBuffer {
  uint8_t *data;
  size_t size;
  size_t capacity;
  int failed;
} GcStateBuffer;

void gc_state_put(GcStateBuffer *b, const void *bytes, size_t size);
void gc_state_put_u32(GcStateBuffer *b, uint32_t value);
void gc_state_put_u64(GcStateBuffer *b, uint64_t value);
void gc_state_buffer_free(GcStateBuffer *b);

/* The state as a file. Written to a temporary name and renamed, so a failed
 * save never destroys the state that was there. `compress` is 0 or 1.
 * Returns 0, or -1 with a reason in `error`. */
int gc_state_save_file(struct GcMachine *m, const GcStateIdentity *id, const char *path, int compress,
                       char *error, size_t error_size);

/* Loads a state into a machine that has booted the same program. Returns 0,
 * or -1 with a reason in `error` and the machine unchanged. */
int gc_state_load_file(struct GcMachine *m, const GcStateIdentity *id, const char *path, char *error,
                       size_t error_size);

/* What a state's header says about it. Reading this opens the file and reads
 * its first bytes; it does not decompress the payload or touch a machine, so
 * a menu can ask about ten slots without a pause. Returns 0, or -1 with a
 * reason in `error` - which includes a file that is not there. */
typedef struct GcStateInfo {
  char name[GC_STATE_NAME_CHARS]; /* empty when the state has none */
  char game_id[16];
  uint32_t version;
  uint64_t field;
  uint64_t cycles;
  uint64_t saved_at;    /* seconds since the epoch; 0 when the state does not say */
  uint64_t stored_size; /* the payload as it is stored, compressed or not */
} GcStateInfo;

int gc_state_peek_file(const char *path, GcStateInfo *out, char *error, size_t error_size);

/* Saving in two halves, for a frontend that does not want to stall a field
 * on compression and the disk: the capture copies the machine (on the thread
 * that runs it, between fields), and writing it out touches nothing of the
 * machine's and can be done on any thread. */
typedef struct GcStateCapture GcStateCapture;
GcStateCapture *gc_state_capture(struct GcMachine *m, const GcStateIdentity *id);
/* The name this state is written with, cut to GC_STATE_NAME_CHARS. A capture
 * starts unnamed; NULL or an empty string leaves it that way. */
void gc_state_capture_set_name(GcStateCapture *capture, const char *name);
int gc_state_write_capture(GcStateCapture *capture, const char *path, int compress, char *error,
                           size_t error_size);
void gc_state_capture_free(GcStateCapture *capture);

/* The same in memory: `out` receives the whole file's bytes. */
int gc_state_save_memory(struct GcMachine *m, const GcStateIdentity *id, int compress, GcStateBuffer *out);
int gc_state_load_memory(struct GcMachine *m, const GcStateIdentity *id, const uint8_t *data, size_t size,
                         char *error, size_t error_size);

/* Everything a state holds, hashed - main memory, ARAM, the framebuffer,
 * every register and device - with the host-only fields left out and FPSCR
 * taken as the guest would read it. Two machines with equal hashes continue
 * identically. Does not change the machine. */
uint64_t gc_state_hash(struct GcMachine *m);

/* The frontend's --audio-stats hash: main memory and the general and
 * floating-point registers, FNV-1a. */
uint64_t gc_state_hash_memory(const struct GcMachine *m);

/* ---------------------------------------------------------------------- */
/* Per-subsystem pieces, at the ends of their own files.                   */
/* Each save appends its section's payload; each load checks the payload   */
/* when `apply` is zero and changes the machine only when it is not.       */

void gc_card_state_save(struct GcMachine *m, GcStateBuffer *b, int for_hash);
int gc_card_state_load(struct GcMachine *m, const uint8_t *payload, size_t size, int apply);
void gc_ax_state_save(struct GcMachine *m, GcStateBuffer *b);
int gc_ax_state_load(struct GcMachine *m, const uint8_t *payload, size_t size, int apply);
void gc_audio_state_save(struct GcMachine *m, GcStateBuffer *b);
int gc_audio_state_load(struct GcMachine *m, const uint8_t *payload, size_t size, int apply);
void gc_pad_state_save(struct GcMachine *m, GcStateBuffer *b);
int gc_pad_state_load(struct GcMachine *m, const uint8_t *payload, size_t size, int apply);

#ifdef __cplusplus
}
#endif

#endif /* GC_STATE_H */
