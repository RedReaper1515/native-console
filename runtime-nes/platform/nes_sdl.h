#ifndef NES_SDL_H
#define NES_SDL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NesRcProgram;

/* True when the command line asks for a headless run (test and diagnostic
 * options), which nes_runner_main handles. */
bool nes_sdl_wants_headless(int argc, char **argv);

/* Windowed frontend. program may be NULL (pure interpreter). */
int nes_sdl_main(int argc, char **argv, const struct NesRcProgram *program, const char *default_rom);

#ifdef __cplusplus
}
#endif

#endif /* NES_SDL_H */
