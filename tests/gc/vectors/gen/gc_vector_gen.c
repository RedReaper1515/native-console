/*
 * The vector generator: a GameCube program that measures what a Gekko
 * actually does, one instruction at a time.
 *
 * PowerPC has no SingleStepTests suite, so the vectors this project checks
 * its interpreter against are made here (docs/GC-PLAN.md, "Contract
 * deviations"). For every case in gc_cases.h - generated from the same ISA
 * table the decoder comes from - this program builds a register state,
 * executes the single instruction, captures everything it changed, and
 * prints both states. Running it on an oracle turns that into a vector
 * file; running it on real hardware would be better still, and the file
 * format does not care which it came from.
 *
 * Output goes through OSReport, which Dolphin turns into host-side logging
 * when it has the ELF's symbols. Every line is printed with a constant
 * "%s\n" format and a "V|" prefix: Dolphin guesses which register holds the
 * format string by looking at what it points at, and a bare string can be
 * mistaken for a `this` pointer.
 *
 * The inputs are printed rather than being derived from a seed both sides
 * agree on, so the vector file is self-contained and replaying it needs no
 * matching random number generator.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>

#include "gc_cases.h"
#include "gc_vector_state.h"

/* Dolphin replaces this with host-side logging; on hardware it does
 * nothing, which is what makes the same binary usable on both. */
__attribute__((weak, noinline)) void OSReport(const char *fmt, ...) { (void)fmt; }

#define SEEDS_PER_CASE 6

static char line[512];
static GcVectorState state;

static void emit(const char *text) { OSReport("%s\n", text); }

/* ------------------------------------------------------------------ */

static uint32_t rng_state;

static uint32_t next_u32(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

/* Values an instruction is most likely to be got wrong on. */
static const uint32_t interesting32[] = {
    0x00000000u, 0x00000001u, 0xffffffffu, 0x80000000u, 0x7fffffffu,
    0x0000ffffu, 0xffff0000u, 0x00008000u, 0xfffffffeu, 0x0000007fu,
};

static const uint64_t interesting64[] = {
    0x0000000000000000ull, /* +0 */
    0x8000000000000000ull, /* -0 */
    0x3ff0000000000000ull, /* 1.0 */
    0xbff0000000000000ull, /* -1.0 */
    0x7ff0000000000000ull, /* +inf */
    0xfff0000000000000ull, /* -inf */
    0x7ff8000000000000ull, /* QNaN */
    0x7ff4000000000000ull, /* SNaN */
    0x0008000000000000ull, /* a double denormal */
    0x3690000000000000ull, /* a single denormal, exactly representable */
    0x47efffffe0000000ull, /* the largest single */
    0x3fb999999999999aull, /* 0.1, not single-representable */
};

static uint32_t pick_u32(void) {
  const uint32_t r = next_u32();
  if ((r & 3u) == 0) return interesting32[(r >> 8) % (sizeof(interesting32) / sizeof(uint32_t))];
  return r;
}

/* A lane value. Most of the time it is a value a single-precision program
 * would really hold, because that is what the paired-single and the
 * single-precision forms are defined on; the rest of the time it is an
 * arbitrary double, which is what pins what the hardware does with one. */
static uint64_t pick_f64(int single_only) {
  const uint32_t r = next_u32();
  if ((r & 7u) == 0) return interesting64[(r >> 8) % (sizeof(interesting64) / sizeof(uint64_t))];
  if (single_only || (r & 1u)) {
    /* Build a single, then widen it: the exponent is kept in range so the
     * value is an ordinary number rather than an infinity most of the time. */
    union {
      float f;
      uint32_t bits;
    } single;
    union {
      double d;
      uint64_t bits;
    } wide;
    single.bits = (next_u32() & 0x807fffffu) | ((0x60u + (next_u32() & 0x3fu)) << 23);
    wide.d = (double)single.f;
    return wide.bits;
  }
  return ((uint64_t)next_u32() << 32) | next_u32();
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */

static char *put_hex32(char *out, uint32_t value) {
  static const char digits[] = "0123456789abcdef";
  int i;
  for (i = 28; i >= 0; i -= 4) *out++ = digits[(value >> i) & 0xf];
  *out++ = ' ';
  return out;
}

static char *put_hex64(char *out, uint64_t value) {
  out = put_hex32(out, (uint32_t)(value >> 32)) - 1;
  return put_hex32(out, (uint32_t)value);
}

static void emit_gpr_line(unsigned index, char direction, const GcVectorState *s) {
  char *out = line;
  unsigned i;
  out += sprintf(out, "V|A %u %c ", index, direction);
  for (i = 0; i < 8; i++) out = put_hex32(out, s->gpr[i]);
  out = put_hex32(out, s->cr);
  out = put_hex32(out, s->xer);
  out = put_hex32(out, (uint32_t)s->fpscr);
  *out = 0;
  emit(line);
}

static void emit_fpr_line(unsigned index, char direction, char tag, const GcVectorState *s,
                          unsigned first) {
  char *out = line;
  unsigned i;
  out += sprintf(out, "V|%c %u %c ", tag, index, direction);
  for (i = first; i < first + 4; i++) {
    out = put_hex64(out, s->fpr[i][0]);
    out = put_hex64(out, s->fpr[i][1]);
  }
  *out = 0;
  emit(line);
}

static void emit_mem_line(unsigned index, char direction, const GcVectorState *s) {
  char *out = line;
  unsigned i;
  out += sprintf(out, "V|D %u %c ", index, direction);
  for (i = 0; i < 16; i++) {
    uint32_t word;
    memcpy(&word, &s->mem[i * 4], sizeof(word));
    out = put_hex32(out, word);
  }
  *out = 0;
  emit(line);
}

static void emit_state(unsigned index, char direction, const GcVectorState *s, unsigned flags) {
  emit_gpr_line(index, direction, s);
  if (flags & GC_CASE_FP) {
    emit_fpr_line(index, direction, 'B', s, 0);
    emit_fpr_line(index, direction, 'C', s, 4);
  }
  if (flags & GC_CASE_MEM) emit_mem_line(index, direction, s);
}

/* ------------------------------------------------------------------ */

/* Paired singles and quantised load/store have to be switched on in HID2
 * before any of them will run, and the floating-point exception modes in
 * MSR are cleared so that a case which raises one cannot take the program
 * with it. */
static void configure_cpu(void) {
  uint32_t hid2, msr;
  asm volatile("mfspr %0, 920" : "=r"(hid2));
  hid2 |= 0xe0000000u; /* LSQE | WPE | PSE */
  asm volatile("mtspr 920, %0" ::"r"(hid2));
  asm volatile("mfmsr %0" : "=r"(msr));
  msr &= ~0x00000900u; /* FE0, FE1 */
  msr |= 0x00002000u;  /* FP available */
  asm volatile("mtmsr %0" ::"r"(msr));
  asm volatile("isync");
}

static void build_inputs(const GcCase *test, unsigned seed) {
  unsigned i;
  rng_state = 0x9e3779b9u ^ (test->insn * 2654435761u) ^ (seed * 0x85ebca6bu);
  for (i = 0; i < 16; i++) (void)next_u32(); /* warm up */

  for (i = 0; i < 8; i++) state.gpr[i] = pick_u32();
  state.cr = next_u32();
  /* The enable bits stay clear: a case must not be able to trap. Everything
   * else, including the rounding mode and non-IEEE mode, is fair game. */
  state.xer = next_u32();
  state.fpscr = (uint64_t)(next_u32() & ~0x000000f8u);

  for (i = 0; i < 8; i++) {
    const int single_only = (test->flags & GC_CASE_QUANTISED) != 0;
    state.fpr[i][0] = pick_f64(single_only);
    state.fpr[i][1] = pick_f64(single_only);
  }
  for (i = 0; i < 64; i++) state.mem[i] = (uint8_t)next_u32();

  if (test->flags & GC_CASE_MEM) {
    /* r6 is the index register of every indexed memory form. Floating-point
     * and quantised accesses stay aligned; the integer ones are allowed to
     * be misaligned, which is the behaviour worth measuring. */
    static const uint32_t aligned[] = {0, 8, 16, 24};
    static const uint32_t any[] = {0, 1, 2, 3, 8, 13, 16, 24};
    const uint32_t r = next_u32();
    if (test->flags & (GC_CASE_FP | GC_CASE_QUANTISED)) {
      state.gpr[3] = aligned[r & 3u];
    } else {
      state.gpr[3] = any[r & 7u];
    }
  }
  if (test->flags & GC_CASE_STRING) {
    /* The string forms take their length from XER, and the scratch buffer
     * is only 64 bytes. */
    state.xer = (state.xer & ~0x7fu) | ((next_u32() % 8u) + 1u);
  }
  /* What the harness will really put in r10. */
  state.gpr[7] = (uint32_t)(uintptr_t)state.mem;
}

int main(void) {
  unsigned index = 0;
  unsigned i, seed;

  VIDEO_Init();
  configure_cpu();

  sprintf(line, "V|SCRATCH %08x", (uint32_t)(uintptr_t)state.mem);
  emit(line);
  sprintf(line, "V|CASES %u SEEDS %u", (unsigned)GC_CASE_COUNT, (unsigned)SEEDS_PER_CASE);
  emit(line);

  for (i = 0; i < GC_CASE_COUNT; i++) {
    const GcCase *test = &gc_cases[i];
    for (seed = 0; seed < SEEDS_PER_CASE; seed++, index++) {
      build_inputs(test, seed);

      gc_case_code[0] = test->insn;
      gc_case_code[1] = 0x4e800020u; /* blr */
      DCFlushRange(gc_case_code, sizeof(gc_case_code));
      ICInvalidateRange(gc_case_code, sizeof(gc_case_code));

      sprintf(line, "V|K %u %s %08x %u", index, test->name, (unsigned)test->insn,
              (unsigned)test->flags);
      emit(line);
      emit_state(index, 'I', &state, test->flags);

      gc_run_case(&state);

      /* The case wrote the scratch buffer in place; everything else comes
       * back in gc_case_out. */
      memcpy(gc_case_out.mem, state.mem, sizeof(state.mem));
      emit_state(index, 'O', &gc_case_out, test->flags);
    }
  }

  sprintf(line, "V|END %u", index);
  emit(line);
  SYS_ResetSystem(SYS_POWEROFF, 0, 0);
  return 0;
}
