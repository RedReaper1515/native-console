/* n64_run: the headless runner with the interpreter alone (n64_runner.c). */
#include "n64_runner.h"

int main(int argc, char **argv) { return n64_runner_main(argc, argv, NULL, NULL); }
