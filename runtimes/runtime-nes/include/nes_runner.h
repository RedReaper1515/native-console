#ifndef NES_RUNNER_H
#define NES_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NesRcProgram;
struct NesMachine;

/* Headless runner (see nes_runner.c). program may be NULL; default_rom is
 * used when no ROM path is given on the command line. */
int nes_runner_main(int argc, char **argv, const struct NesRcProgram *program, const char *default_rom);

/* Buttons (NES_BTN_* bits) the built-in input script holds on a frame: it
 * presses START and A through boot screens, menus and text, then moves
 * around and uses the other buttons. Used by --auto-input and profiling. */
uint8_t nes_auto_input(uint64_t frame);

/* Profiling run for nesrecomp --profile: runs `rom` with the interpreter
 * for `frames` frames under the built-in input script and writes the trace
 * (entry points, see nes_coverage.c) to trace_path. Nothing else is
 * written; battery RAM stays in memory. */
bool nes_profile_rom(const uint8_t *rom, size_t size, uint64_t frames, const char *trace_path,
                     uint64_t *instructions);

/* Text of the background, reading each nametable entry's tile number as an
 * ASCII code (how simple test ROMs print). */
void nes_screen_text(struct NesMachine *m, char *out, size_t cap);

/* Machine state for A/B comparison: CPU registers, work RAM, PRG RAM, CHR
 * RAM, nametables, palette, OAM and the cartridge's registers, as a stable
 * binary file. */
bool nes_dump_state(struct NesMachine *m, const char *path);

/* The current frame as a binary PPM (P6), 256 x 240. */
bool nes_dump_ppm(const struct NesMachine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NES_RUNNER_H */
