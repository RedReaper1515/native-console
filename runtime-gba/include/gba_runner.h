#ifndef GBA_RUNNER_H
#define GBA_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GbaRcProgram;

/* Headless runner (see gba_runner.c). program may be NULL; default_rom is
 * used when no ROM path is given on the command line. */
int gba_runner_main(int argc, char **argv, const struct GbaRcProgram *program, const char *default_rom);

/* Buttons (GBA_KEY_* bits) the built-in input script holds on a frame: it
 * presses START and A through boot screens, menus and text, then moves
 * around and uses the other buttons. Used by --auto-input and profiling. */
uint16_t gba_auto_input(uint64_t frame);

/* Profiling run for gbarecomp --profile: runs `rom` with the interpreter for
 * `frames` frames under the built-in input script and writes the trace
 * (entry points and RAM images, see gba_coverage.c) to trace_path. Nothing
 * else is written; battery saves stay in memory. */
bool gba_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path,
                     uint64_t *instructions);

#ifdef __cplusplus
}
#endif

#endif /* GBA_RUNNER_H */
