#ifndef N64_RUNNER_H
#define N64_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct N64RcProgram;
struct N64Machine;

/* Headless runner (see n64_runner.c). program may be NULL; default_rom is
 * used when no ROM path is given on the command line. */
int n64_runner_main(int argc, char **argv, const struct N64RcProgram *program, const char *default_rom);

/* Buttons and stick the built-in input script holds on a frame (used by
 * --auto-input and profiling): START and A through boot screens and menus,
 * then movement and the other buttons. */
uint16_t n64_auto_input(uint64_t frame, int8_t *x, int8_t *y);

/* Profiling run for n64recomp --profile: the interpreter for `frames`
 * frames under the input script; writes the trace (entry points and the
 * RDRAM code they ran from, see n64_coverage.c). Nothing else is written. */
bool n64_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path, uint64_t *instructions);

/* Machine state for A/B comparison, as a stable binary file. */
bool n64_dump_state(struct N64Machine *m, const char *path);

/* The last frame as a binary PPM (P6). */
bool n64_dump_ppm(const struct N64Machine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* N64_RUNNER_H */
