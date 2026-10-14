/* nds_play: play a ROM in a window with the interpreter (no recompiled
 * program). Headless options fall through to the runner. */
#include <stddef.h>

#include "nds_runner.h"
#include "nds_sdl.h"

int main(int argc, char **argv)
{
    if (nds_sdl_wants_headless(argc, argv)) return nds_runner_main(argc, argv, NULL, NULL);
    return nds_sdl_main(argc, argv, NULL, NULL);
}
