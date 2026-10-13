#ifndef GBA_SDL_H
#define GBA_SDL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GbaRcProgram;

/* True when the command line asks for a headless run (test and diagnostic
 * options), which gba_runner_main handles. */
bool gba_sdl_wants_headless(int argc, char **argv);

/* Windowed frontend. program may be NULL (pure interpreter). */
int gba_sdl_main(int argc, char **argv, const struct GbaRcProgram *program, const char *default_rom);

#ifdef __cplusplus
}
#endif

#endif /* GBA_SDL_H */
