/*
 * The verdict format the runner reads, and a subtest counter to go with it.
 *
 * tools/gc/run_gc_tests.py parses one line per test:
 *
 *     Test 3 passed (128 subtests)
 *     Test 4 failed (96 subtests, 2 failures)
 *
 * That is the shape dolphin-emu/hwtests prints, and printing the same thing
 * means one runner and one baseline file cover both suites. The lines are
 * all that is shared: this harness is ours, written from the format rather
 * than copied from a GPL suite (docs/GC-PLAN.md, "What we reuse").
 *
 * Verdicts leave through a weak `OSReport`, which is how a GameCube program
 * talks to a debugger it may not have. On hardware the weak definition here
 * is what runs and nothing is printed; under `gc_run` the symbol is hooked
 * and the text arrives on stdout.
 */
#ifndef GC_TEST_REPORT_H
#define GC_TEST_REPORT_H

#include <stdio.h>

#include <gccore.h>

__attribute__((weak, noinline)) void OSReport(const char *fmt, ...) { (void)fmt; }

static unsigned gc_test_index;
static unsigned gc_test_subtests;
static unsigned gc_test_failures;
static unsigned gc_test_reported;
static char gc_test_line[512];

static void gc_test_begin(const char *name) {
  gc_test_index++;
  gc_test_subtests = 0;
  gc_test_failures = 0;
  snprintf(gc_test_line, sizeof(gc_test_line), "--- Test %u: %s", gc_test_index, name);
  OSReport("%s\n", gc_test_line);
}

/* One subtest. `detail` is printed only when it fails, so a passing run is
 * a dozen lines and a failing one says which case and what it saw. */
static void gc_test_check(int condition, const char *detail) {
  gc_test_subtests++;
  if (condition) return;
  gc_test_failures++;
  if (gc_test_failures <= 12) OSReport("  FAIL %s\n", detail);
}

#define GC_CHECK_EQ(got, want, what)                                                    \
  do {                                                                                  \
    const unsigned long long gc_got_ = (unsigned long long)(got);                        \
    const unsigned long long gc_want_ = (unsigned long long)(want);                      \
    snprintf(gc_test_line, sizeof(gc_test_line), "%s: got %016llx, want %016llx", (what), \
             gc_got_, gc_want_);                                                         \
    gc_test_check(gc_got_ == gc_want_, gc_test_line);                                    \
  } while (0)

#define GC_CHECK(condition, what) gc_test_check((condition) ? 1 : 0, (what))

static void gc_test_end(void) {
  gc_test_reported++;
  if (gc_test_failures) {
    OSReport("Test %u failed (%u subtests, %u failures)\n", gc_test_index, gc_test_subtests,
             gc_test_failures);
  } else {
    OSReport("Test %u passed (%u subtests)\n", gc_test_index, gc_test_subtests);
  }
}

/* A program that stops here has finished every test it has. The runner
 * hooks SYS_ResetSystem; on hardware it really does reset. */
static void gc_test_finish(void) {
  OSReport("%u tests reported\n", gc_test_reported);
  SYS_ResetSystem(SYS_POWEROFF, 0, 0);
}

#endif /* GC_TEST_REPORT_H */
