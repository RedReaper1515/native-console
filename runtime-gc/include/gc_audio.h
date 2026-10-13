#ifndef GC_AUDIO_H
#define GC_AUDIO_H

#include <stdint.h>

struct GcMachine;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where sound leaves the machine.
 *
 * The sound processor mixes into a buffer in main memory and a separate
 * engine walks that buffer out to the converter, thirty-two bytes at a time,
 * looping for ever and raising an interrupt each time it reaches the end.
 * That interrupt is what a game's audio thread is paced by: it is the moment
 * the game is told "the buffer you gave me has been played, give me the next
 * one". A machine that never raises it leaves the game's mixer starved, and
 * one that raises it at the wrong rate makes everything the game does with
 * sound run fast or slow - which is not a sound bug from the outside, it is
 * a game that is running at the wrong speed.
 *
 * So this is worth having before there is anything to hear. What the buffer
 * contains is the sound processor's business (docs/gc-dsp-ax.md, parts B and
 * C); this file plays whatever is there.
 */

/* Advance the engine to the machine's current cycle, playing any blocks that
 * have come due. Called from the machine's step. */
void gc_audio_advance(struct GcMachine *m);

/* The instruction count at which the next block comes due, if that is
 * earlier than `next`; otherwise `next`. See gc_machine_due() in gc.c. */
uint64_t gc_audio_next_event(const struct GcMachine *m, uint64_t next);

/* The length-and-enable register, at the point it is written. Bit 15 starts
 * and stops the engine; the rest is the length in 32-byte blocks. */
void gc_audio_length_write(struct GcMachine *m, uint32_t value);

/* How much of the current buffer is left, in 32-byte blocks. A game reads
 * this to find out how far through the buffer the engine is, so it has to
 * fall with time rather than sit at either end. */
uint32_t gc_audio_blocks_left(const struct GcMachine *m);

/* Take played frames, as interleaved left/right 16-bit pairs. Returns how
 * many frames were available, which may be fewer than asked for. This is the
 * host's end of the pipe; nothing in the runtime calls it. */
unsigned gc_audio_take(struct GcMachine *m, int16_t *out, unsigned frames);

/* How many frames the machine produced that nobody took. A frontend that
 * keeps up reads zero for ever; a number that climbs means the machine is
 * running ahead of the sound device, which is worth being able to see. */
uint64_t gc_audio_overruns(const struct GcMachine *m);

/* Whether the engine is running at all. A machine with no sound engine
 * started is producing silence on purpose, and a frontend can say so; a
 * machine that is playing and falling behind is a different thing entirely,
 * and the two must not be papered over with the same silence. */
int gc_audio_playing(const struct GcMachine *m);

/* The rate the engine is playing at now, in frames a second: 32000 or 48000,
 * from the audio interface's control word. What gc_audio_take() returns is at
 * this rate, and a frontend that assumes 32 kHz plays a game that switched to
 * 48 kHz half again too slowly and lets its queue grow without bound. */
unsigned gc_audio_rate(const struct GcMachine *m);

void gc_audio_reset(struct GcMachine *m);
void gc_audio_free(struct GcMachine *m);

#ifdef __cplusplus
}
#endif

#endif
