/*
 * What ran where: how much of a run the interpreter did, and where it was
 * entered.
 *
 * A recompiled game falls back to the interpreter wherever there is no
 * block - code static analysis never reached (a function called only
 * through a pointer), code the game copies into memory, the exception
 * handlers the OS writes into low memory. Missing a block costs speed, not
 * correctness, so nothing else says how much of a run that is.
 *
 * The record is kept by gc_machine_step, which is the one path every
 * interpreted instruction takes - the interpreter's own loop and the
 * recompiled dispatcher's fallback alike. Recompiled code never calls it,
 * so the record needs no hook in the blocks or the dispatcher, and it stays
 * correct however those are arranged (a block returning to the dispatcher
 * or chaining straight into the next one).
 *
 * That makes the native share a subtraction: the instructions a recompiled
 * run retired, less the ones it interpreted. The share by *address* needs
 * to know every address that ran, and a recompiled run cannot say that
 * without instrumenting its blocks; an interpreted machine running the same
 * program to the same instruction count can, which is what --differential
 * has beside it (gc_runner.c).
 */
#ifndef GC_COVERAGE_H
#define GC_COVERAGE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;

#define GC_COVERAGE_ENTERED 1u
#define GC_COVERAGE_RESUMED 2u

typedef struct GcCoverage {
  /* One counter per four bytes of main memory: the instructions
   * interpreted at that address. */
  uint32_t *count;
  /* One byte per four bytes of main memory, set where interpretation was
   * entered rather than continued: GC_COVERAGE_ENTERED for the first
   * instruction after recompiled code, an interrupt vector, a branch
   * target - what a trace hands back to gcrecomp - and GC_COVERAGE_RESUMED
   * for an interrupt returning to the instruction it interrupted, which is
   * not (gc_coverage_note says why). */
  uint8_t *entry;
  /* Every instruction interpreted, and those at addresses that are not
   * main memory (and so have no counter). */
  uint64_t instructions;
  uint64_t outside;
  /* The last instruction interpreted, to tell continuing from entering. */
  uint32_t last_pc;
  uint64_t last_cycles;
  int have_last;
} GcCoverage;

GcCoverage *gc_coverage_create(void);
void gc_coverage_destroy(GcCoverage *coverage);

/* The instruction at cpu.pc is about to be interpreted. gc_machine_step
 * calls this when m->coverage is set. */
void gc_coverage_note(GcMachine *m);

/* Where the counters say a guest address is, or -1 when it is not main
 * memory. */
long gc_coverage_index(uint32_t address);

/* Every address interpretation was entered at, one hexadecimal effective
 * address a line, for `gcrecomp --trace`. Addresses only the interpreter
 * could have run (outside the executable) are written too; gcrecomp keeps
 * the ones that are code in the image it recompiles. Addresses only ever
 * resumed at are not written. A trace already at `path` is merged in rather
 * than replaced. Returns 0, or -1 when the file will not open. */
int gc_coverage_write_trace(const GcCoverage *coverage, const char *path, const char *comment);

/* The native-share report for a recompiled run that retired `total`
 * instructions and interpreted what `recompiled_run` recorded.
 * `everything`, when not null, is an interpreted machine's record of the
 * same run to the same instruction count, which is what the share by
 * address needs. */
void gc_coverage_report(FILE *out, const char *name, uint64_t total, const GcCoverage *recompiled_run,
                        const GcCoverage *everything);

#ifdef __cplusplus
}
#endif

#endif /* GC_COVERAGE_H */
