/*
 * Replay the Gekko vectors against the interpreter.
 *
 * The vector file is made by tests/gc/vectors/gen/ running on an oracle and
 * captured by tools/gc/capture_vectors.py. Each case is one instruction: a
 * full register state before, the same state after. This harness puts the
 * "before" into a GcCpu, executes exactly one instruction, and compares
 * everything - the eight visible GPRs, CR, XER, FPSCR, both lanes of the
 * eight visible FPRs, and the 64-byte scratch buffer.
 *
 * The evidence layer is a second implementation, not hardware (AGENTS.md);
 * hwtests/cputest is the hardware layer and arrives in P2. A case that is
 * known to differ is listed in KNOWN, with the reason, and is reported
 * separately - both when it fails, which is expected, and when it starts
 * passing, which means the list is out of date.
 *
 *     gc_vectors <file> [--report N] [--filter NAME]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_bus.h"
#include "gc_cpu.h"
#include "gc_fpu_impl.h"
#include "gc_test_bus.h"

#define CODE_BASE (GC_TEST_BASE + 0x100u)
#define MAX_REPORT 8

/* Cases this implementation is known to answer differently, with the reason.
 * Each one is also in docs/GC-PLAN.md under "Known deviations". */
static const struct {
  const char *name;
  const char *reason;
} KNOWN[] = {
    {"fres", "the hardware estimate table is not published; we compute exactly"},
    {"frsqrte", "the hardware estimate table is not published; we compute exactly"},
    {"ps_res", "the hardware estimate table is not published; we compute exactly"},
    {"ps_rsqrte", "the hardware estimate table is not published; we compute exactly"},
    /* addme and subfme add three things: the operand, CA, and 0xffffffff.
     * The oracle folds the last two into one operand first, so the carry out
     * of 0xffffffff + 1 is lost and it reports no carry whenever CA was set.
     * The architecture says both always carry in that case. */
    {"addme", "the oracle loses the carry out of 0xffffffff + CA"},
    {"subfme", "the oracle loses the carry out of 0xffffffff + CA"},
    /* The paired-single datapath cannot carry a full double: what lands in
     * ps0 is rounded to a single and what lands in ps1 is truncated. The
     * oracle passes a double through these instructions unchanged, so it
     * disagrees whenever a lane holds a value a single cannot represent.
     * hwtests/cputest/pairedmove.cpp checks this against hardware and
     * passes, which is why the implementation follows it and these cases
     * are listed here rather than fixed. */
    {"ps_abs", "the oracle does not narrow a lane to single precision"},
    {"ps_mr", "the oracle does not narrow a lane to single precision"},
    {"ps_nabs", "the oracle does not narrow a lane to single precision"},
    {"ps_neg", "the oracle does not narrow a lane to single precision"},
    {"ps_merge00", "the oracle does not narrow a lane to single precision"},
    {"ps_merge01", "the oracle does not narrow a lane to single precision"},
    {"ps_merge10", "the oracle does not narrow a lane to single precision"},
    {"ps_merge11", "the oracle does not narrow a lane to single precision"},
    {"ps_sum0", "the oracle does not narrow the lane it passes through"},
    {"ps_sum1", "the oracle does not narrow the lane it passes through"},
};

/* Bits of FPSCR this oracle carries no information about, because its own
 * interpreter does not model them for arithmetic: the inexact pair FI and
 * FR, the sticky inexact bit XX, and the overflow and underflow bits OX and
 * UX - a single-precision add whose result leaves the single range raises
 * neither there, though the architecture says it must. FX goes with them
 * because it is the summary of exactly those. Our implementation does model
 * them, from the manual, and the values it produces still have to match
 * exactly; hwtests/cputest is what confirms the bits on hardware
 * (docs/GC-PLAN.md, P2). Comparing them here would mean copying a gap in
 * the oracle rather than testing against it.
 *
 * Everything else is compared: the nine invalid-operation bits and their VX
 * summary, ZX, FPRF, the enables, the rounding mode and non-IEEE mode. */
#define FPSCR_NOT_MODELLED                                                      (GC_FPSCR_FI | GC_FPSCR_FR | GC_FPSCR_XX | GC_FPSCR_FX | GC_FPSCR_OX |         GC_FPSCR_UX)

/* CR1 is a copy of FPSCR[0:3] - FX, FEX, VX, OX - so the Rc form of a
 * floating-point instruction carries the same two unmodelled bits into the
 * condition register, and they come out of the CR comparison there too. */
#define CR1_NOT_MODELLED 0x09000000u /* FX and OX within CR1 */

/* The compare instructions set FPSCR[FPCC] as well as the CR field. The
 * oracle updates only the CR field, so FPRF is left out of the comparison
 * for those - and only for those. */
static int is_compare(const char *name) {
  return strncmp(name, "fcmp", 4) == 0 || strncmp(name, "ps_cmp", 6) == 0;
}

static uint32_t fpscr_mask(const char *name) {
  uint32_t mask = ~(uint32_t)FPSCR_NOT_MODELLED;
  if (is_compare(name)) mask &= ~(uint32_t)GC_FPSCR_FPRF;
  return mask;
}

static int is_known(const char *name) {
  size_t i;
  for (i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
    if (strcmp(KNOWN[i].name, name) == 0) return 1;
  }
  return 0;
}

typedef struct Vector {
  unsigned index;
  char name[24];
  uint32_t insn;
  unsigned flags;
  uint32_t gpr[2][8];
  uint32_t cr[2];
  uint32_t xer[2];
  uint32_t fpscr[2];
  uint64_t fpr[2][8][2];
  uint32_t mem[2][16];
  int has_fpr[2];
  int has_mem[2];
} Vector;

static GcCpu cpu;
static uint32_t scratch;

/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  char *data;
  long length;
  if (!file) return NULL;
  fseek(file, 0, SEEK_END);
  length = ftell(file);
  fseek(file, 0, SEEK_SET);
  data = (char *)malloc((size_t)length + 1);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return NULL;
  }
  data[length] = 0;
  *size = (size_t)length;
  fclose(file);
  return data;
}

static unsigned parse_hex_list(const char *text, uint32_t *out32, uint64_t *out64,
                               unsigned count) {
  unsigned i;
  char *end;
  for (i = 0; i < count; i++) {
    unsigned long long value = strtoull(text, &end, 16);
    if (end == text) return i;
    if (out32) out32[i] = (uint32_t)value;
    if (out64) out64[i] = value;
    text = end;
  }
  return count;
}

/* ------------------------------------------------------------------ */

static void load_state(const Vector *v, int side) {
  unsigned i;
  gc_cpu_reset(&cpu);
  cpu.msr |= GC_MSR_FP;
  cpu.hid2 = GC_HID2_PSE | GC_HID2_LSQE | GC_HID2_WPE;
  for (i = 0; i < 8; i++) cpu.gpr[3 + i] = v->gpr[side][i];
  cpu.cr = v->cr[side];
  /* Loading XER is a write to XER, so the bits the register does not have
   * are dropped here exactly as mtspr drops them. */
  cpu.xer = v->xer[side] & GC_XER_WRITABLE;
  /* Loading FPSCR is a write to FPSCR: the two summary bits are derived. */
  cpu.fpscr = 0;
  gc_fpscr_write(&cpu, v->fpscr[side], 0xffffffffu);
  for (i = 0; i < 8; i++) {
    cpu.fpr[1 + i].ps0 = v->fpr[side][i][0];
    cpu.fpr[1 + i].ps1 = v->fpr[side][i][1];
  }
  if (v->has_mem[side]) {
    for (i = 0; i < 16; i++) gc_test_write32(scratch + 4 * i, v->mem[side][i]);
  }
}

static int compare(const Vector *v, char *detail, size_t detail_size) {
  unsigned i;
  int differences = 0;
  detail[0] = 0;
  gc_fp_settle(&cpu); /* FPSCR can be behind: gc_fpu_impl.h */

#define REPORT(fmt, ...)                                                     \
  do {                                                                       \
    const size_t used = strlen(detail);                                      \
    if (used + 96 < detail_size)                                             \
      snprintf(detail + used, detail_size - used, fmt "\n", __VA_ARGS__);    \
    differences++;                                                           \
  } while (0)

  for (i = 0; i < 8; i++) {
    if (cpu.gpr[3 + i] != v->gpr[1][i]) {
      REPORT("    r%u  = %08x, expected %08x", 3 + i, cpu.gpr[3 + i], v->gpr[1][i]);
    }
  }
  {
    /* An Rc-form floating-point instruction copies FPSCR[0:3] into CR1. */
    const uint32_t cr_mask =
        (v->has_fpr[0] && (v->insn & 1u)) ? ~(uint32_t)CR1_NOT_MODELLED : 0xffffffffu;
    if ((cpu.cr & cr_mask) != (v->cr[1] & cr_mask)) {
      REPORT("    cr  = %08x, expected %08x", cpu.cr & cr_mask, v->cr[1] & cr_mask);
    }
  }
  if (cpu.xer != (v->xer[1] & GC_XER_WRITABLE)) {
    REPORT("    xer = %08x, expected %08x", cpu.xer, v->xer[1] & GC_XER_WRITABLE);
  }
  {
    const uint32_t mask = fpscr_mask(v->name);
    if ((cpu.fpscr & mask) != (v->fpscr[1] & mask)) {
      REPORT("    fpscr = %08x, expected %08x", cpu.fpscr & mask, v->fpscr[1] & mask);
    }
  }
  if (v->has_fpr[1]) {
    for (i = 0; i < 8; i++) {
      if (cpu.fpr[1 + i].ps0 != v->fpr[1][i][0]) {
        REPORT("    f%u.ps0 = %016llx, expected %016llx", 1 + i,
               (unsigned long long)cpu.fpr[1 + i].ps0, (unsigned long long)v->fpr[1][i][0]);
      }
      if (cpu.fpr[1 + i].ps1 != v->fpr[1][i][1]) {
        REPORT("    f%u.ps1 = %016llx, expected %016llx", 1 + i,
               (unsigned long long)cpu.fpr[1 + i].ps1, (unsigned long long)v->fpr[1][i][1]);
      }
    }
  }
  if (v->has_mem[1]) {
    for (i = 0; i < 16; i++) {
      const uint32_t got = gc_test_read32(scratch + 4 * i);
      if (got != v->mem[1][i]) {
        REPORT("    mem+%02x = %08x, expected %08x", i * 4, got, v->mem[1][i]);
      }
    }
  }
#undef REPORT
  return differences;
}

/* ------------------------------------------------------------------ */

typedef struct Tally {
  char name[24];
  unsigned total;
  unsigned failed;
} Tally;

static Tally tallies[512];
static unsigned tally_count;

static Tally *tally_for(const char *name) {
  unsigned i;
  for (i = 0; i < tally_count; i++) {
    if (strcmp(tallies[i].name, name) == 0) return &tallies[i];
  }
  if (tally_count == sizeof(tallies) / sizeof(tallies[0])) return &tallies[0];
  snprintf(tallies[tally_count].name, sizeof(tallies[0].name), "%s", name);
  return &tallies[tally_count++];
}

int main(int argc, char **argv) {
  const char *path = NULL;
  const char *filter = NULL;
  unsigned report_limit = MAX_REPORT;
  size_t size = 0;
  char *data, *line, *next;
  Vector current;
  int have_case = 0;
  unsigned total = 0, passed = 0, failed = 0, known_failed = 0, known_passed = 0;
  unsigned reported = 0;
  int argi;

  for (argi = 1; argi < argc; argi++) {
    if (strcmp(argv[argi], "--report") == 0 && argi + 1 < argc) {
      report_limit = (unsigned)strtoul(argv[++argi], NULL, 10);
    } else if (strcmp(argv[argi], "--filter") == 0 && argi + 1 < argc) {
      filter = argv[++argi];
    } else if (!path) {
      path = argv[argi];
    }
  }
  if (!path) {
    fprintf(stderr, "usage: gc_vectors <file> [--report N] [--filter NAME]\n");
    return 2;
  }

  data = read_file(path, &size);
  if (!data) {
    fprintf(stderr, "gc_vectors: cannot read %s\n", path);
    return 1;
  }

  gc_test_reset_memory();
  memset(&current, 0, sizeof(current));

  for (line = data; line && *line; line = next) {
    char *end = strchr(line, '\n');
    next = end ? end + 1 : NULL;
    if (end) *end = 0;
    if (end > line && end[-1] == '\r') end[-1] = 0;

    if (line[0] == '#' || line[0] == 0) continue;

    if (strncmp(line, "scratch ", 8) == 0) {
      scratch = (uint32_t)strtoul(line + 8, NULL, 16);
      if (scratch < GC_TEST_BASE || scratch + 64 > GC_TEST_BASE + GC_TEST_SIZE) {
        fprintf(stderr, "gc_vectors: scratch %08x is outside the test memory\n", scratch);
        return 1;
      }
      continue;
    }

    if (strncmp(line, "case ", 5) == 0) {
      /* case <index> <name> <insn> <flags> */
      char *cursor = line + 5;
      const char *name;
      size_t length;
      const unsigned long index = strtoul(cursor, &cursor, 10);
      while (*cursor == ' ') cursor++;
      name = cursor;
      while (*cursor && *cursor != ' ') cursor++;
      length = (size_t)(cursor - name);
      if (length == 0 || length >= sizeof(current.name)) {
        fprintf(stderr, "gc_vectors: bad case line: %s\n", line);
        return 1;
      }
      memset(&current, 0, sizeof(current));
      current.index = (unsigned)index;
      memcpy(current.name, name, length);
      current.name[length] = 0;
      current.insn = (uint32_t)strtoul(cursor, &cursor, 16);
      current.flags = (unsigned)strtoul(cursor, &cursor, 10);
      have_case = 1;
      continue;
    }

    if (!have_case) continue;

    {
      const int out = strncmp(line, "out.", 4) == 0;
      const char *rest = line + (out ? 4 : 3);
      if (*rest == '.') rest++;
      if (strncmp(rest, "gpr ", 4) == 0) {
        uint32_t values[11];
        if (parse_hex_list(rest + 4, values, NULL, 11) != 11) {
          fprintf(stderr, "gc_vectors: short gpr line: %s\n", line);
          return 1;
        }
        memcpy(current.gpr[out], values, sizeof(current.gpr[0]));
        current.cr[out] = values[8];
        current.xer[out] = values[9];
        current.fpscr[out] = values[10];
      } else if (strncmp(rest, "fpr0 ", 5) == 0) {
        uint64_t values[8];
        if (parse_hex_list(rest + 5, NULL, values, 8) != 8) return 1;
        memcpy(&current.fpr[out][0][0], values, sizeof(values));
        current.has_fpr[out] = 1;
      } else if (strncmp(rest, "fpr1 ", 5) == 0) {
        uint64_t values[8];
        if (parse_hex_list(rest + 5, NULL, values, 8) != 8) return 1;
        memcpy(&current.fpr[out][4][0], values, sizeof(values));
      } else if (strncmp(rest, "mem ", 4) == 0) {
        if (parse_hex_list(rest + 4, current.mem[out], NULL, 16) != 16) return 1;
        current.has_mem[out] = 1;
      }

      /* The last line of a case is always the output side; run it then. */
      if (out && ((current.flags & 2u) ? strncmp(rest, "mem ", 4) == 0
                                       : ((current.flags & 1u) ? strncmp(rest, "fpr1 ", 5) == 0
                                                               : strncmp(rest, "gpr ", 4) == 0))) {
        char detail[512];
        int differences;
        Tally *tally;

        if (filter && strcmp(filter, current.name) != 0) {
          have_case = 0;
          continue;
        }

        total++;
        tally = tally_for(current.name);
        tally->total++;

        load_state(&current, 0);
        gc_test_write32(CODE_BASE, current.insn);
        cpu.pc = CODE_BASE;
        cpu.npc = CODE_BASE + 4;
        gc_cpu_step(&cpu);

        differences = compare(&current, detail, sizeof(detail));
        if (cpu.stopped) {
          differences++;
          snprintf(detail, sizeof(detail), "    the core stopped (%u) on %08x\n",
                   (unsigned)cpu.stopped, current.insn);
        }

        if (differences == 0) {
          passed++;
          if (is_known(current.name)) known_passed++;
        } else if (is_known(current.name)) {
          known_failed++;
          tally->failed++;
        } else {
          failed++;
          tally->failed++;
          if (reported < report_limit) {
            reported++;
            printf("FAIL case %u %s %08x\n%s", current.index, current.name,
                   current.insn, detail);
          }
        }
        have_case = 0;
      }
    }
  }

  free(data);

  printf("gc_vectors: %u cases, %u passed, %u failed", total, passed, failed);
  if (known_failed) printf(", %u known deviations", known_failed);
  printf("\n");

  if (failed) {
    unsigned i;
    printf("failing instructions:\n");
    for (i = 0; i < tally_count; i++) {
      if (tallies[i].failed && !is_known(tallies[i].name)) {
        printf("  %-12s %u/%u\n", tallies[i].name, tallies[i].failed, tallies[i].total);
      }
    }
    return 1;
  }
  if (known_passed) {
    printf("note: %u cases of known-deviation instructions now pass; "
           "check whether the list in this file is out of date\n", known_passed);
  }
  if (total == 0) {
    fprintf(stderr, "gc_vectors: no cases ran\n");
    return 1;
  }
  return 0;
}
