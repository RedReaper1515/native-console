#ifndef GC_SDL_H
#define GC_SDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GcMachine;

/*
 * What a recompiled game hands the frontend: its name, the image its code
 * was built from, and the function that runs a field of it. The last is the
 * only thing that differs between a recompiled game and the interpreter, so
 * it is the only thing passed in - a frontend that took anything else would
 * be two frontends.
 */
typedef struct GcSdlProgram {
  const char *name;
  const uint8_t *image;
  size_t image_size;
  /* Run until the field counter reaches this value. Null runs interpreted. */
  void (*run)(struct GcMachine *m, uint64_t until_field);
} GcSdlProgram;

/* The window, the controller, the sound and the menu. `default_disc` is the
 * file to look for beside the executable when none is named. */
int gc_sdl_main(int argc, char **argv, const GcSdlProgram *program, const char *default_disc);

/* Whether these arguments are for the headless runner rather than a window.
 * A build that has both calls this first. */
bool gc_sdl_wants_headless(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* GC_SDL_H */
