#ifndef GC_AX_H
#define GC_AX_H

#include <stdint.h>

struct GcMachine;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AX: the microcode a game uploads to make sound, done on this side.
 *
 * A game hands the sound processor a *command list* five milliseconds at a
 * time. The list initialises nine mixing buffers, points at a linked list of
 * voices, asks for all of them to be mixed, and writes the result somewhere
 * the audio DMA will find it. This file does what the microcode would have
 * done with that list.
 *
 * Everything here is written from docs/gc-dsp-ax.md parts B and C, which
 * were compiled from Dolphin's implementation read as a behaviour reference.
 * There is no hardware oracle for any of it - no test suite, no captured
 * output - so the honest description is "a second implementation's account,
 * followed carefully". The places where that account is a guess are marked
 * **Unverified:** in the doc and repeated here where they change what the
 * code does.
 *
 * The one structural check available was worth having: the parameter block's
 * field order is given as a list rather than as offsets, and adding the
 * field widths up gives 122 halfwords - exactly the 244 bytes the doc says
 * the block is. A list with a field of the wrong width would not close.
 */

/* Run a command list at `address`, `halfwords` long. Returns zero when the
 * list ended properly - at an END - and non-zero when the walk had to stop
 * early, which means either a command this does not know or a list that ran
 * off its own end. Both are worth telling apart from success: a walker that
 * has lost its place is worse than no walker, because the halfwords it reads
 * next are not commands. */
int gc_ax_run_list(struct GcMachine *m, uint32_t address, uint32_t halfwords);

/* How many lists have been run, and how many stopped early. For the runner's
 * summary: a game whose lists are all ending badly is one whose sound is
 * silence for a findable reason. */
uint64_t gc_ax_lists(const struct GcMachine *m);
uint64_t gc_ax_bad_lists(const struct GcMachine *m);

/* How many voices those lists walked, and how many were playing. A game
 * whose lists all parse and whose voices are all stopped is a different
 * problem from one whose voices play the wrong thing, and from the far end
 * the two are the same silence. */
uint64_t gc_ax_voices(const struct GcMachine *m);
uint64_t gc_ax_voices_running(const struct GcMachine *m);

void gc_ax_reset(struct GcMachine *m);

#ifdef __cplusplus
}
#endif

#endif /* GC_AX_H */
