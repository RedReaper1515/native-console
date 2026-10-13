/*
 * Two machines running one program, compared: the recompiled one and the
 * interpreter, which is the reference.
 *
 * What the comparison assumes (docs/GC-PLAN.md, "P6"):
 *
 *  - The machines are compared at the same *instruction count*, and only
 *    there. The recompiled machine is run however its code likes to run - a
 *    block at a time back to the dispatcher, or blocks chained straight into
 *    each other - and stops some instructions past where it was asked to;
 *    the interpreter is then stepped to exactly that count. Nothing here
 *    depends on where a block ends.
 *  - At that count the interpreter has retired the same instructions but has
 *    not yet looked at what is due before the next one, while recompiled
 *    code may already have (an interrupt taken on its way out of a block).
 *    When the program counters differ at an equal count, the interpreter is
 *    given that look (gc_machine_before) before anything is compared.
 *  - Both machines are brought up to date (the beam, the clocks) at that
 *    count before their devices are compared; that is a function of the
 *    count, so it is the same whenever it is done.
 *  - The two machines share nothing mutable: every piece of device state is
 *    in GcMachine (the expansion bus's device select used to be a file
 *    static, and was moved for this). What is still file-static in the
 *    runtime is either scratch rewritten on every use (the rasteriser's
 *    rows, the TEV plan), a cache keyed by its whole input (the compressed
 *    texture palette), or the same for both machines (the OS hooks, set
 *    from the executable's symbols).
 */
#ifndef GC_DIFFERENTIAL_H
#define GC_DIFFERENTIAL_H

#include <stdint.h>
#include <stdio.h>

#include "gc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Step the interpreted machine to the recompiled machine's instruction
 * count. Returns 0, or -1 when it stopped or finished before getting
 * there. */
int gc_differential_catch_up(GcMachine *recompiled, GcMachine *interpreted);

/* Compare the processors only: every architectural register and the
 * instruction count. Returns the number of registers that differ, printing
 * each to `out` when it is not null. */
unsigned gc_differential_compare_cpu(const GcMachine *recompiled, const GcMachine *interpreted, FILE *out);

/* Everything: the processor, main memory, ARAM, the embedded framebuffer,
 * the I/O registers, the graphics processor's registers and the machine's
 * own device state. Brings both machines up to date first. Returns the
 * number of things that differ, printing each (with the first differing
 * addresses) to `out` when it is not null. */
unsigned gc_differential_compare(GcMachine *recompiled, GcMachine *interpreted, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* GC_DIFFERENTIAL_H */
