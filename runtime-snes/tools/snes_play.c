/* snes_play: play a ROM in a window with the interpreter (no recompiled
 * program). Headless options fall through to the runner. */
#include <stddef.h>

#include "snes_runner.h"
#include "snes_sdl.h"

int main(int argc, char **argv)
{
    if (snes_sdl_wants_headless(argc, argv)) return snes_runner_main(argc, argv, NULL, NULL);
    return snes_sdl_main(argc, argv, NULL, NULL);
}
