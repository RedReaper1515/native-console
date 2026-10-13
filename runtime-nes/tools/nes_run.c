/* nes_run: the headless runner with the interpreter alone. */
#include "nes_runner.h"

int main(int argc, char **argv) { return nes_runner_main(argc, argv, NULL, NULL); }
