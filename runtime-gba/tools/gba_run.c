/* gba_run: the headless runner with no recompiled program (pure
 * interpreter). See src/gba_runner.c for the options. */
#include <stddef.h>

#include "gba_runner.h"

int main(int argc, char **argv)
{
    return gba_runner_main(argc, argv, NULL, NULL);
}
