/* nes_play: play a ROM in a window with the interpreter (no recompiled
 * program). Headless options fall through to the runner. */
#include <stddef.h>

#include "nes_runner.h"
#include "nes_sdl.h"

int main(int argc, char **argv)
{
    if (nes_sdl_wants_headless(argc, argv)) return nes_runner_main(argc, argv, NULL, NULL);
    return nes_sdl_main(argc, argv, NULL, NULL);
}
