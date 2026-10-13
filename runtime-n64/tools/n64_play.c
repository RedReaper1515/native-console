/* n64_play: play a ROM in a window with the interpreter (no recompiled
 * program). Headless options fall through to the runner. */
#include <stddef.h>

#include "n64_runner.h"
#include "n64_sdl.h"

int main(int argc, char **argv)
{
    if (n64_sdl_wants_headless(argc, argv)) return n64_runner_main(argc, argv, NULL, NULL);
    return n64_sdl_main(argc, argv, NULL, NULL);
}
