#ifndef SNES_RUNNER_H
#define SNES_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SnesRcProgram;
struct SnesMachine;

/* Headless runner (see snes_runner.c). program may be NULL; default_rom is
 * used when no ROM path is given on the command line. */
int snes_runner_main(int argc, char **argv, const struct SnesRcProgram *program, const char *default_rom);

/* Buttons (SNES_BTN_* bits) the built-in input script holds on a frame: it
 * presses START and A through boot screens, menus and text, then moves
 * around and uses the other buttons. Used by --auto-input and profiling. */
uint16_t snes_auto_input(uint64_t frame);

/* Profiling run for snesrecomp --profile: runs `rom` with the interpreter
 * for `frames` frames under the built-in input script and writes the trace
 * (entry points and a WRAM image, see snes_coverage.c) to trace_path.
 * Nothing else is written; battery RAM stays in memory. */
bool snes_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path,
                      uint64_t *instructions);

/* Text of the enabled tile backgrounds, reading each map entry's tile
 * number as an ASCII code (how simple test ROMs print). */
void snes_screen_text(struct SnesMachine *m, char *out, size_t cap);

/* Machine state for A/B comparison: CPU registers, WRAM, VRAM, CGRAM, OAM,
 * battery RAM and audio RAM, as a stable binary file. */
bool snes_dump_state(struct SnesMachine *m, const char *path);

/* A frame as a binary PPM (P6), the output size of the last frame. */
bool snes_dump_ppm(const struct SnesMachine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SNES_RUNNER_H */
