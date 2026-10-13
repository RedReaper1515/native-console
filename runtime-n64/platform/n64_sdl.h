#ifndef N64_SDL_H
#define N64_SDL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct N64RcProgram;

/* True when the command line asks for a headless run (test and diagnostic
 * options), which n64_runner_main handles. */
bool n64_sdl_wants_headless(int argc, char **argv);

/* Windowed frontend. program may be NULL (pure interpreter). */
int n64_sdl_main(int argc, char **argv, const struct N64RcProgram *program, const char *default_rom);

#ifdef __cplusplus
}
#endif

#endif /* N64_SDL_H */
