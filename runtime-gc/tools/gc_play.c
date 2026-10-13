/* gc_play: play a disc in a window with the interpreter and no
 * recompiled code. Headless options fall through to the runner, so one
 * program answers both ways of asking. */
#include <stddef.h>

#include "gc_sdl.h"

int gc_runner_main(int argc, char **argv);

int main(int argc, char **argv) {
  if (gc_sdl_wants_headless(argc, argv)) return gc_runner_main(argc, argv);
  return gc_sdl_main(argc, argv, NULL, NULL);
}
