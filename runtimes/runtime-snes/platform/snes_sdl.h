#ifndef SNES_SDL_H
#define SNES_SDL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SnesRcProgram;

/* True when the command line asks for a headless run (test and diagnostic
 * options), which snes_runner_main handles. */
bool snes_sdl_wants_headless(int argc, char **argv);

/* Windowed frontend. program may be NULL (pure interpreter). */
int snes_sdl_main(int argc, char **argv, const struct SnesRcProgram *program, const char *default_rom);

#ifdef __cplusplus
}
#endif

#endif /* SNES_SDL_H */
