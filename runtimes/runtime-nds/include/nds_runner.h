#ifndef NDS_RUNNER_H
#define NDS_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NdsRcProgram;

/* Headless runner (see nds_runner.c). program may be NULL; default_rom is
 * used when no ROM path is given on the command line. */
int nds_runner_main(int argc, char **argv, const struct NdsRcProgram *program, const char *default_rom);

/* What the built-in input script does on a frame: it taps and presses its
 * way through boot screens, menus and text, then plays with the stylus,
 * the D-pad and every button. `run` picks one of any number of variations
 * that tap in different places. Used by --auto-input and profiling. */
typedef struct {
    uint32_t keys; /* NDS_KEY_* bits */
    bool touch;    /* the stylus is on the bottom screen, at x, y */
    int x, y;
} NdsAutoInput;

void nds_auto_input(uint64_t frame, unsigned run, NdsAutoInput *in);

/* How many times a profiling run starts the game over. */
#define NDS_PROFILE_RUNS 3

/* Profiling run for ndsrecomp --profile: runs `rom` with the interpreter
 * for `frames` frames under the built-in input script and writes what it
 * executed to trace_path, as --trace-out does. The frames are split over
 * NDS_PROFILE_RUNS runs with a reset before each and a different variation
 * of the script in each: a blind script that taps its way into a dead end
 * (a game's Wi-Fi set-up, say) loses one run, not everything, and the save a
 * run made is there for the next. instructions (may be NULL) receives how
 * many each CPU ran, the ARM9 first. Nothing else is written; the save
 * stays in memory. */
bool nds_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path,
                     uint64_t instructions[2]);

#ifdef __cplusplus
}
#endif

#endif /* NDS_RUNNER_H */
