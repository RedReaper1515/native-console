/*
 * What the interpreter ran: the places it was entered (the run loop's
 * entries, branch targets) and the RDRAM pages the code was in, as they
 * were when it ran. n64recomp --trace reads the file this writes; the
 * pages let it compile code that a game copies or decompresses into RDRAM
 * at run time. --coverage prints where execution fell back to the
 * interpreter most.
 */
#ifndef N64_COVERAGE_H
#define N64_COVERAGE_H

#include "n64.h"

#ifdef __cplusplus
extern "C" {
#endif

void n64_coverage_enable(N64Machine *m);
/* The same record without routing every CPU store through the bus: for a
 * game being played. A page is snapshotted when an entry is first seen in
 * it and again after a DMA wrote to it. */
void n64_coverage_enable_light(N64Machine *m);
/* Whether CPU stores must reach the bus for coverage (not in light mode). */
bool n64_coverage_sees_stores(const N64Machine *m);
void n64_coverage_free(N64Machine *m);
/* The interpreter is about to run the instruction at c->pc, entered by a
 * jump or from the run loop. */
void n64_coverage_entry(N64Machine *m);
/* RDRAM was written (bus stores and DMA): its pages are hashed again. */
void n64_coverage_written(N64Machine *m, uint32_t paddr, uint32_t size);
bool n64_coverage_write_trace(N64Machine *m, const char *path);
/* Adds this run to the trace at `path`: the entries and pages the file
 * already holds are kept, this run's are added, and the file ends with
 * this run's image of RDRAM. Written beside the old file, then swapped in. */
bool n64_coverage_merge_trace(N64Machine *m, const char *path);
/* Entry points recorded so far. */
uint32_t n64_coverage_entries(const N64Machine *m);
void n64_coverage_report(N64Machine *m);

#ifdef __cplusplus
}
#endif

#endif /* N64_COVERAGE_H */
