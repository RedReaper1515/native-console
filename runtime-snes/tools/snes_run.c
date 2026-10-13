/* snes_run: the headless runner with the interpreter alone. */
#include "snes_runner.h"

int main(int argc, char **argv) { return snes_runner_main(argc, argv, NULL, NULL); }
