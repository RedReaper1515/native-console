/* What a trace keeps (gc_coverage.c): addresses the interpreter was entered
 * at, not the ones an interrupt returned to, and whatever the file already
 * held. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_coverage.h"

static int failures;

static void check(int ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

static void note(GcMachine *m, uint32_t pc, uint64_t cycles) {
  m->cpu.pc = pc;
  m->cpu.cycles = cycles;
  gc_coverage_note(m);
}

static int listed(const char *text, uint32_t address) {
  char line[16];
  snprintf(line, sizeof(line), "\n%08x\n", address);
  return strstr(text, line) != NULL;
}

static char *read_all(const char *path) {
  FILE *file = fopen(path, "r"); /* text mode, as the trace was written */
  char *text;
  long size;
  if (!file) return NULL;
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  text = (char *)calloc(1, (size_t)size + 2);
  text[0] = '\n'; /* so the first address matches "\n%08x\n" too */
  (void)fread(text + 1, 1, (size_t)size, file); /* shorter in text mode, and zero-filled after */
  fclose(file);
  return text;
}

int main(void) {
  const char *path = "gc_coverage_test.trace";
  GcMachine *m = gc_machine_create();
  GcCoverage *coverage = gc_coverage_create();
  FILE *file;
  char *text;
  if (!m || !coverage) return 2;
  m->coverage = coverage;
  m->cpu.msr = 0x00009032u;
  m->cpu.srr1 = 0x00001032u; /* differs from MSR in EE */
  m->cpu.srr0 = 0x80001000u;

  note(m, 0x80001000u, 100); /* entered by a branch: SRR0 matches but MSR does not */
  note(m, 0x80001004u, 101); /* continuing */
  m->cpu.srr1 = m->cpu.msr;
  m->cpu.srr0 = 0x80002008u;
  note(m, 0x80002008u, 200); /* an interrupt came back to the middle of a block */
  note(m, 0x8000200cu, 201); /* continuing from the resume */
  note(m, 0x80003000u, 300); /* entered, with SRR0 elsewhere */
  note(m, 0x80003000u, 400); /* entered again */

  check(coverage->entry[gc_coverage_index(0x80001000u)] == GC_COVERAGE_ENTERED, "a branch target is entered");
  check(coverage->entry[gc_coverage_index(0x80001004u)] == 0, "the next instruction continues");
  check(coverage->entry[gc_coverage_index(0x80002008u)] == GC_COVERAGE_RESUMED, "a resume is resumed");
  check(coverage->entry[gc_coverage_index(0x8000200cu)] == 0, "after a resume, continuing");
  check(coverage->instructions == 6, "six instructions counted");

  file = fopen(path, "w");
  if (!file) return 2;
  fprintf(file, "# an earlier run\n80004000\n80001000\n");
  fclose(file);
  check(gc_coverage_write_trace(coverage, path, "test") == 0, "the trace is written");
  text = read_all(path);
  check(text != NULL, "the trace reads back");
  if (text) {
    check(listed(text, 0x80001000u), "entered address listed");
    check(listed(text, 0x80003000u), "second entered address listed");
    check(listed(text, 0x80004000u), "the earlier run's address kept");
    check(!listed(text, 0x80002008u), "resumed address left out");
    check(!listed(text, 0x80001004u), "continued address left out");
    {
      /* Each address once, although the file already held one of them. */
      const char *first = strstr(text, "\n80001000\n");
      check(first && !strstr(first + 1, "\n80001000\n"), "no address twice");
    }
    free(text);
  }
  remove(path);
  m->coverage = NULL;
  gc_coverage_destroy(coverage);
  gc_machine_destroy(m);
  printf("gc_coverage: %s\n", failures ? "FAIL" : "ok");
  return failures ? 1 : 0;
}
