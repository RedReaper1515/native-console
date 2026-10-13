/*
 * The state a vector case carries, shared by the guest generator and the
 * assembly harness.
 *
 * One layout, one set of offsets: the .S file gets them from the same
 * defines the C file uses, so the two cannot drift.
 *
 * Only r3-r10 and f1-f8 are visible to a case. r10 always holds the address
 * of the scratch buffer, so a load or store form has somewhere legal to
 * point; f0, r0, r1, r2 and r11 upward are the harness's, and an
 * instruction that writes them (lmw, the string forms) is caught by the
 * harness saving and restoring the whole register file around the case.
 */
#ifndef GC_VECTOR_STATE_H
#define GC_VECTOR_STATE_H

#define GC_ST_GPR 0    /* uint32_t gpr[8]  - r3..r10          */
#define GC_ST_CR 32    /* uint32_t cr                          */
#define GC_ST_XER 36   /* uint32_t xer                         */
#define GC_ST_FPSCR 40 /* uint64_t; the low word is the FPSCR  */
#define GC_ST_FPR 48   /* uint64_t fpr[8][2] - f1..f8, ps0/ps1 */
#define GC_ST_PAD 176
#define GC_ST_MEM 192 /* uint8_t mem[64], 32-byte aligned      */
#define GC_ST_SIZE 256

#define GC_FIRST_GPR 3
#define GC_FIRST_FPR 1
#define GC_SCRATCH_GPR 10

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct GcVectorState {
  uint32_t gpr[8];
  uint32_t cr;
  uint32_t xer;
  uint64_t fpscr;
  uint64_t fpr[8][2];
  uint8_t pad[16];
  uint8_t mem[64];
} GcVectorState __attribute__((aligned(32)));

/* The two instruction words a case executes: the case itself, then blr. */
extern uint32_t gc_case_code[8];

/* Filled by gc_run_case with everything the case left behind except the
 * scratch memory, which the case wrote in place. */
extern GcVectorState gc_case_out;

/* Load `state` into the visible registers, execute gc_case_code, and
 * capture the result into gc_case_out. Every register is saved and
 * restored around the case, so an instruction that writes the whole
 * register file cannot take the harness with it. */
void gc_run_case(GcVectorState *state);

#endif /* __ASSEMBLER__ */

#endif /* GC_VECTOR_STATE_H */
