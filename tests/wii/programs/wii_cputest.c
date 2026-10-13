/*
 * Every instruction a 750CL has, against a test validated on the real
 * processor.
 *
 * The test is Andrew Church's ppc750cl.s (achurch.org/cpu-tests; "No
 * copyright is claimed on this file"). It is not in this repository:
 * tools/fetch_wii_tests.py fetches it at a pinned hash and
 * tools/wii/build_wii_programs.py assembles it into this program, which is
 * skipped with a note when the file has not been fetched.
 *
 * The routine exercises each instruction and writes a 32-byte record for
 * every one whose result is not the processor's: the instruction word, its
 * address, and what it produced. Each record becomes one failing subtest
 * here, so the baseline's number for this program is the count of
 * instructions the core still gets wrong - a debt list with addresses.
 *
 * Three of its options are off. The absolute-branch tests need the code at
 * address 0x01000000, which nothing maps; the system-call and trap tests
 * need handlers that return a particular value, and the handlers here are
 * libogc's. Setting the quantisation registers is on: this runs in
 * supervisor state, as a game does.
 *
 * A fourth is off because of the core, not the test: the undocumented
 * instruction forms (mulhw with its overflow bit set is the first), which
 * a 750CL executes with the stray bit ignored and this core stops on as
 * not an instruction. docs/WII-PLAN.md lists it; turn the option back on
 * when the decoder accepts them.
 */
#include <gccore.h>
#include <malloc.h>
#include <ogc/machine/processor.h>
#include <stdlib.h>
#include <string.h>

#include "gc_test_report.h"

__asm__(
    "   .section .text\n"
    "   .balign 32\n"
    "   .globl ppc750cl_test\n"
    "CAN_SET_GQR = 1\n"
    "TEST_BA = 0\n"
    "TEST_SC = 0\n"
    "TEST_TRAP = 0\n"
    "TEST_UNDOCUMENTED_INSNS = 0\n"
    "ppc750cl_test:\n"
    "   .include \"ppc750cl.s\"\n"
    "   .balign 32\n"
    /* The way in. The routine keeps to the ABI's registers, but a core that
     * gets an instruction wrong can leave any register holding anything -
     * the stack pointer included - and the point of the run is to get back
     * and say which instruction it was. So the three registers a C program
     * cannot do without are kept here and put back whatever comes out. */
    "   .globl ppc750cl_run\n"
    "ppc750cl_run:\n"
    "   mflr 0\n"
    "   lis 9,ppc750cl_kept@ha\n"
    "   addi 9,9,ppc750cl_kept@l\n"
    "   stw 0,0(9)\n"
    "   stw 1,4(9)\n"
    "   stw 2,8(9)\n"
    "   stw 13,12(9)\n"
    "   bl ppc750cl_test\n"
    "   lis 9,ppc750cl_kept@ha\n"
    "   addi 9,9,ppc750cl_kept@l\n"
    "   lwz 0,0(9)\n"
    "   lwz 10,4(9)\n"
    "   lwz 2,8(9)\n"
    "   lwz 13,12(9)\n"
    "   stw 1,16(9)\n" /* what the routine left in the stack pointer */
    "   mr 1,10\n"
    "   mtlr 0\n"
    "   blr\n"
    "   .section .text\n");

u32 ppc750cl_kept[5];

extern int ppc750cl_run(int zero, void *scratch, void *failures, double one);
extern char ppc750cl_test[];

#define FAILURE_BYTES 0x40000u

static char line[256];

int main(void) {
  u8 *scratch = (u8 *)memalign(32, 0x8000);
  u32 *failures = (u32 *)memalign(32, FAILURE_BYTES);
  int count, i, shown;
  u32 level;

  VIDEO_Init();
  gc_test_begin("every 750CL instruction (achurch ppc750cl.s)");
  if (!scratch || !failures) {
    GC_CHECK(0, "no memory for the test's buffers");
    gc_test_end();
    gc_test_finish();
  }
  memset(scratch, 0, 0x8000);
  memset(failures, 0, FAILURE_BYTES);

  /* With interrupts off: the routine uses every register and the time
   * base, and is not written to be preempted. */
  _CPU_ISR_Disable(level);
  count = ppc750cl_run(0, scratch, failures, 1.0);
  _CPU_ISR_Restore(level);

  snprintf(line, sizeof(line), "the test bootstrapped (it returned %d)", count);
  GC_CHECK(count >= 0, line);
  snprintf(line, sizeof(line), "the stack pointer came back as it went in (%08x, not %08x)",
           (unsigned)ppc750cl_kept[4], (unsigned)ppc750cl_kept[1]);
  GC_CHECK(ppc750cl_kept[4] == ppc750cl_kept[1], line);
  shown = count < (int)(FAILURE_BYTES / 32u) ? count : (int)(FAILURE_BYTES / 32u);
  for (i = 0; i < shown; i++) {
    const u32 *r = failures + i * 8;
    snprintf(line, sizeof(line), "insn %08x at +%05x: %08x %08x %08x %08x", (unsigned)r[0],
             (unsigned)(r[1] - (u32)ppc750cl_test), (unsigned)r[2], (unsigned)r[3], (unsigned)r[4],
             (unsigned)r[5]);
    /* Every one of them, not the first dozen the harness prints: the list
     * is the result. */
    gc_test_subtests++;
    gc_test_failures++;
    OSReport("  FAIL %s\n", line);
  }
  gc_test_end();
  gc_test_finish();
  return 0;
}
