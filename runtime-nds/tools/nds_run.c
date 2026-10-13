/* nds_run: the headless runner with no recompiled program (pure
 * interpreter). See src/nds_runner.c for the options. */
#include <stddef.h>

#include "nds_runner.h"

int main(int argc, char **argv)
{
    return nds_runner_main(argc, argv, NULL, NULL);
}
