/* gc_run: the headless runner, as a program. The runner itself is in
 * gc_run.c, which gc_play links behind a window instead. */
int gc_runner_main(int argc, char **argv);

int main(int argc, char **argv) { return gc_runner_main(argc, argv); }
