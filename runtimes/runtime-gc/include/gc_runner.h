/*
 * The headless runner every generated GameCube program carries: the test
 * tooling's way in, the hub's for a cover, and where a recompiled program is
 * checked against the interpreter.
 *
 *   GAME [--disc IMAGE] [--frames N] [--dump-frame FILE.ppm [--dump-frame-every N [--dump-frame-from FIELD]]]
 *        [--interpret] [--input SCRIPT | --auto-input] [--differential]
 *        [--coverage] [--trace-out FILE]
 *        [--save-state FILE [--save-at FIELD[,FIELD...]] [--state-uncompressed]]
 *        [--load-state FILE] [--state-hash] [--card FILE] [--field-log FILE]
 *        [--dump-memory FILE] [--watch ADDRESS] [--trace-invalid FIELD]
 *        [--wii-aspect 4:3|16:9]
 *
 * It lives in the runtime rather than in the text gcrecomp writes, so that
 * it is compiled, read and changed like the rest of the machine; the
 * generated main.c hands it the program (gc_runner_main).
 *
 * A game boots the way the window does: from the disc named, or the one
 * beside the executable, through the disc's own apploader. A test program
 * is an ELF and is simply loaded, with the disc (if any) in the drive, as
 * gc_run does. No memory card is inserted unless --card FILE names one, so
 * a headless run never touches a save it was not given. With --card the
 * run's machine uses and writes back FILE (formatting it when it does not
 * exist); a --differential's interpreter sees the same card in memory only.
 *
 * --frames counts fields. Without --input or --auto-input no controller is
 * plugged in, as it always was.
 *
 * --input SCRIPT plugs a controller into port 1 and plays a script of its
 * states, a line each: `FIELD BUTTONS [SX SY [CX CY [L R]]]`. The state
 * holds from that field until the next line's. BUTTONS is `-` for none or
 * names joined by `+` (A B X Y Z L R START UP DOWN LEFT RIGHT); the sticks
 * are -128..127 and the triggers 0..255. `#` starts a comment. The script is
 * data about a run, not a change to the machine: the pad is set between
 * fields, exactly as a frontend sets it. --auto-input plays a built-in
 * script instead (gc_auto_input). On a Wii the same script drives the first
 * remote and its Nunchuk, and CX CY are where the remote points: 0 0 is the
 * middle of the screen, -128 the left edge or the bottom, 127 the right
 * edge or the top.
 *
 * --differential runs the recompiled program and the interpreter side by
 * side, a field at a time, and stops at the first field whose end they
 * disagree at: every register the processor has, the instruction count,
 * main memory, ARAM, the embedded framebuffer, the I/O registers and the
 * machine's own state (interrupts, the beam, the DSP's mail, the drive).
 * On a divergence it runs both again to the last field they agreed at and
 * steps that one field block by block to say which block went wrong.
 * docs/GC-PLAN.md ("P6") says what the comparison assumes.
 *
 * --coverage reports the native share: how much of the run was recompiled
 * and how much interpreted, by instructions and (with --differential) by
 * address, and where the interpreted instructions went. --trace-out FILE
 * writes every address the interpreter was entered at, for
 * `gcrecomp --trace FILE`.
 *
 * --save-state FILE writes a save state (gc_state.h) when the field counter
 * reaches --save-at (default: --frames, the end of the run). With several
 * fields, FILE's name gets each one: s.state becomes s-001400.state.
 * --load-state FILE starts from a state instead of from boot: the program
 * boots as usual and the state is loaded over it, so --frames still counts
 * from zero (a state at 1400 run with --frames 2000 runs 600 fields), and
 * --input and --auto-input keep playing by field number. With --differential
 * both machines load the same state. --state-hash prints, at the end, the
 * frontend's --audio-stats hash and the full state hash, after carrying the
 * machine to the same instruction count however it was run.
 *
 * --field-log FILE writes a line a field of where the host's time went
 * (gc_perf.h): wall time, instructions run and interpreted, the command
 * stream, the OpenGL path and its waits, textures, programs, readbacks and
 * disc reads. Measurement only; the machine runs the same.
 *
 * --dump-memory FILE writes main memory as the run left it (MEM1, then on
 * a Wii MEM2) and puts the processor's registers in the log: for reading a
 * game's own structures from outside when it has stopped somewhere.
 *
 * --watch ADDRESS (hexadecimal, as the game sees it) puts every write that
 * covers that address in the log: the value, the instruction and the link
 * register. For finding who made a value what it should not be. It turns
 * the shortcut to main memory off, so the run is several times slower.
 *
 * --trace-invalid FIELD puts every floating-point invalid operation from
 * that field on in the log (gc_cpu.h, gc_fp_invalid_trace): where a NaN is
 * born. For a game whose scene has vanished: give the field before it did.
 * The first four hundred are said, and the first two hundred stores of a
 * default NaN to memory after them, each with its address - which finds a
 * NaN that was not born of an invalid operation but read from somewhere.
 * Like --watch, several times slower.
 */
#ifndef GC_RUNNER_H
#define GC_RUNNER_H

#include <stdint.h>

#include "gc.h"
#include "gc_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The layout of a generated program's block table (GcRecompiledBlock),
 * which the generated main.c checks at compile time. */
typedef struct GcRunnerBlock {
  uint32_t address;
  uint32_t end;
  /* Answers the block to run next, or null (GcRecompiledFn). */
  void *(*run)(GcMachine *m);
} GcRunnerBlock;

typedef struct GcRunnerProgram {
  const char *name;
  const unsigned char *image;
  unsigned image_size;
  /* What the disc content is called beside the executable. */
  const char *disc_name;
  /* Run recompiled to a field; the generated dispatcher. */
  void (*run)(GcMachine *m, uint64_t until_field);
  /* The blocks, for stepping one at a time when a differential run looks
   * for the block that diverged. */
  const GcRunnerBlock *blocks;
  unsigned block_count;
  uint32_t base;
  unsigned span;
} GcRunnerProgram;

int gc_runner_main(int argc, char **argv, const GcRunnerProgram *program);

/* The built-in input script --auto-input plays: the controller's state for
 * a field. Deterministic, and about no game in particular. */
void gc_auto_input(uint64_t field, GcPad *pad);

/* An --input script, for a frontend that plays one (the SDL frontend's
 * --play-input, for measuring a window on the same play as a headless run).
 * Load prints what is wrong with the file, under NAME, and returns NULL. */
typedef struct GcInputScript GcInputScript;
GcInputScript *gc_input_script_load(const char *name, const char *path);
void gc_input_script_pad(const GcInputScript *script, uint64_t field, GcPad *pad);
void gc_input_script_free(GcInputScript *script);

#ifdef __cplusplus
}
#endif

#endif /* GC_RUNNER_H */
