#ifndef NDS_SDL_H
#define NDS_SDL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NdsRcProgram;

/* True when the command line asks for a headless run (test and diagnostic
 * options), which nds_runner_main handles. */
bool nds_sdl_wants_headless(int argc, char **argv);

/* Windowed frontend. program may be NULL (pure interpreter). */
int nds_sdl_main(int argc, char **argv, const struct NdsRcProgram *program, const char *default_rom);

#ifdef __cplusplus
}
#endif

#endif /* NDS_SDL_H */
