/**
 * gbrt_run.h - a played run, kept so that it can be played again.
 *
 * A Game Boy has nothing random in it. What a game does follows from the
 * ROM, from what was in the cartridge's memory and its clock when it was
 * switched on, and from the cycle each button went down and came up. Keep
 * those and the run can be had again, to the cycle, as often as wanted: to
 * record what the game did with its data, which code ran, what was on the
 * screen.
 *
 * `--record-run DIR` keeps them while somebody plays. Each sitting, from
 * the game starting to the game closing, is a segment: a text file,
 * `segment-0001.run`, beside the cartridge memory the sitting began with,
 * `segment-0001.sav`. DIR is also where the game keeps its save while it
 * records, so a second sitting begins with what the first one left.
 *
 * `--replay-run FILE` plays a segment again with nobody at the controls.
 * It starts from the segment's own cartridge memory, never from a player's
 * save, and writes no save. The buttons change at the cycles they changed
 * at, and the machine is stopped at each of those cycles to change them, so
 * a replay does not depend on how the game was built or on where its loop
 * happens to look for input. Every so many frames the recording wrote down
 * a hash of the machine; the replay has to arrive at each of them, and says
 * at which frame it first did not.
 *
 * The file, a line each:
 *
 *   native-console-run 1                 the format and its version
 *   console gb
 *   segment N
 *   rom SHA256 SIZE TITLE
 *   model dmg|cgb|sgb COMPATIBILITY      1 for a CGB running a DMG cartridge
 *   clock CYCLES-PER-SECOND
 *   checkpoint-frames N
 *   save FILE SIZE SHA256 blank|used     or "save -" with no cartridge memory
 *   rtc S M H DL DH LS LM LH LDL LDH LATCH REMAINDER ACTIVE   with a clock chip
 *   data-mod ENTRIES
 *   unpacked REPLACEMENTS SHA256        of a pack of unpacked data; "0 -" with none
 *   started UNIX-TIME
 *   begin
 *   i CYCLE BUTTONS [!]                  the buttons held from this cycle on
 *   k FRAME CYCLE HASH                   a checkpoint
 *   e CYCLE FRAME HASH SAVE-SHA256       the game was closed here
 *
 * BUTTONS is letters, as `--input` has them (U D L R A B, S for Start, T for
 * Select), or `-` for none. `!` says the joypad interrupt was asked for at
 * that cycle. A segment with no `e` line was not closed: the game was
 * killed, or the machine went down. It replays to its last line.
 *
 * The hash covers the registers, the cycle and frame counts, the work RAM,
 * the high RAM, the cartridge's memory, its bank registers and its clock. It
 * leaves out video memory and the hardware registers: the runtime brings
 * some of those up to date only when the game looks at them, so a recompiled
 * game and an interpreted one can hold them differently at the same cycle.
 */

#ifndef GBRT_RUN_H
#define GBRT_RUN_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Have a replay write a save state into `dir` every `every_frames` frames
 * (`segment-NNNN-fFFFFFFFFF.state`), frame 0 included. A replay that was
 * started from one of them with --load-state-file goes on from there. */
void gbrt_run_set_states(const char* dir, uint64_t every_frames);

/* Start recording a segment into `dir`, which exists. After the game's init
 * (the cartridge memory is loaded by then), after its data mod and its pack
 * of unpacked data were loaded, which the record's header tells, and before
 * it runs. False, with the reason on stderr, if the run cannot be recorded. */
bool gbrt_run_record_start(GBContext* ctx, const char* dir);

/* Start replaying the segment in `file`. After the game's init, after its
 * data mod and its pack of unpacked data were loaded, and after a state was
 * loaded, if one was. False, with the reason on stderr, if this game cannot
 * replay it: another ROM, another model, a missing save, other data mods,
 * or a pack that replaces other things than the record was played with. */
bool gbrt_run_replay_start(GBContext* ctx, const char* file);

bool gbrt_run_recording(void);
bool gbrt_run_replaying(void);

/* The number of the segment being recorded or replayed, or 0. */
unsigned gbrt_run_segment(void);

/* Before the machine runs on, while recording: the buttons it will see
 * (active low, as the joypad register has them), and whether the joypad
 * interrupt was asked for since the last call. */
void gbrt_run_record_slice(GBContext* ctx, uint8_t dpad, uint8_t buttons,
                           bool interrupt);

/* Before the machine runs on, while replaying: the buttons it is to see
 * from here, whether to ask for the joypad interrupt, and how far it may
 * run before the next call: no further than `budget` cycles, and no
 * further than the cycle of the record's next line. */
uint32_t gbrt_run_replay_slice(GBContext* ctx, uint32_t budget,
                               uint8_t* dpad, uint8_t* buttons,
                               bool* interrupt);

/* The replay has reached the end of its record, or has left it. */
bool gbrt_run_replay_over(void);

/* When the game stops. A recording writes its last line; a replay says how
 * it went. False if a replay did not arrive where the recording did. */
bool gbrt_run_finish(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_RUN_H */
