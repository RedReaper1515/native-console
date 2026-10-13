/*
 * What ran where. See gc_coverage.h.
 */
#include "gc_coverage.h"

#include <stdlib.h>
#include <string.h>

#include "gc.h"

#define GC_COVERAGE_SLOTS (GC_MEM1_SIZE / 4u)

GcCoverage *gc_coverage_create(void) {
  GcCoverage *coverage = (GcCoverage *)calloc(1, sizeof(GcCoverage));
  if (!coverage) return NULL;
  coverage->count = (uint32_t *)calloc(GC_COVERAGE_SLOTS, sizeof(uint32_t));
  coverage->entry = (uint8_t *)calloc(GC_COVERAGE_SLOTS, 1);
  if (!coverage->count || !coverage->entry) {
    gc_coverage_destroy(coverage);
    return NULL;
  }
  return coverage;
}

void gc_coverage_destroy(GcCoverage *coverage) {
  if (!coverage) return;
  free(coverage->count);
  free(coverage->entry);
  free(coverage);
}

/* Main memory answers at its physical address with translation off and at
 * the two usual BAT windows with it on; the counters do not care which. */
long gc_coverage_index(uint32_t address) {
  const uint32_t physical = address & 0x3fffffffu;
  if ((address & 3u) || physical >= GC_MEM1_SIZE) return -1;
  return (long)(physical >> 2);
}

/* The bits of MSR that rfi restores from SRR1 (gc_op_rfi). */
#define GC_COVERAGE_RFI_MSR 0x87c0ff73u

void gc_coverage_note(GcMachine *m) {
  GcCoverage *coverage = (GcCoverage *)m->coverage;
  const uint32_t pc = m->cpu.pc;
  const long index = gc_coverage_index(pc);
  /* Continuing is the instruction straight after the last one interpreted,
   * one instruction later. Anything else - recompiled code ran in between,
   * a branch was taken, an interrupt arrived - is an entry. */
  const int continuing = coverage->have_last && coverage->last_pc + 4u == pc &&
                         coverage->last_cycles + 1u == m->cpu.cycles;
  coverage->instructions++;
  if (index < 0) {
    coverage->outside++;
  } else {
    if (coverage->count[index] != 0xffffffffu) coverage->count[index]++;
    if (!continuing) {
      /* An interrupt handler's rfi comes back to the instruction it
       * interrupted, which is SRR0, with MSR as SRR1 had it. Code that
       * was being interpreted is interrupted anywhere, so over a long
       * run nearly every instruction of a hot loop is entered that way
       * once - and as roots those would cut the loop into blocks of one
       * instruction. The code around them was entered some other way
       * first, so they are kept apart (GC_COVERAGE_RESUMED) and left out
       * of the trace. A branch that happens to land on SRR0 with MSR
       * matching is marked resumed too; that costs a root, never a
       * wrong answer. */
      const GcCpu *c = &m->cpu;
      const int resumed = pc == c->srr0 && ((c->msr ^ c->srr1) & GC_COVERAGE_RFI_MSR) == 0u;
      coverage->entry[index] |= resumed ? GC_COVERAGE_RESUMED : GC_COVERAGE_ENTERED;
    }
  }
  coverage->last_pc = pc;
  coverage->last_cycles = m->cpu.cycles;
  coverage->have_last = 1;
}

int gc_coverage_write_trace(const GcCoverage *coverage, const char *path, const char *comment) {
  /* A trace already at the path is added to, not replaced. A run of a
   * build made with a trace interprets only what that trace missed, so its
   * entries alone would drop every root the earlier run found: the hub's
   * second cover capture of a rebuilt game would leave the next rebuild
   * with almost no trace. */
  uint8_t *kept = (uint8_t *)calloc(GC_COVERAGE_SLOTS, 1);
  unsigned i, resumed_only = 0, kept_count = 0;
  FILE *file;
  if (!kept) return -1;
  file = fopen(path, "r");
  if (file) {
    char line[128];
    while (fgets(line, sizeof(line), file)) {
      char *end = NULL;
      const unsigned long value = strtoul(line, &end, 16);
      const long index = line[0] == '#' || end == line ? -1 : gc_coverage_index((uint32_t)value);
      if (index >= 0 && !kept[index]) {
        kept[index] = 1;
        kept_count++;
      }
    }
    fclose(file);
  }
  file = fopen(path, "w");
  if (!file) {
    free(kept);
    return -1;
  }
  fprintf(file, "# gcrecomp trace: addresses the interpreter was entered at, one a line\n");
  if (comment) fprintf(file, "# %s\n", comment);
  if (kept_count) fprintf(file, "# added to the %u addresses the file already held\n", kept_count);
  for (i = 0; i < GC_COVERAGE_SLOTS; i++) {
    if (coverage->entry[i] == GC_COVERAGE_RESUMED) resumed_only++;
    if (!(coverage->entry[i] & GC_COVERAGE_ENTERED) && !kept[i]) continue;
    fprintf(file, "%08x\n", 0x80000000u | (i << 2));
  }
  fprintf(file, "# and %u addresses only ever entered by returning from an interrupt, left out\n", resumed_only);
  free(kept);
  fclose(file);
  return 0;
}

/* A run of interpreted code: an entry and the instructions after it up to
 * the next entry or the first address that never ran. */
typedef struct GcCoverageRegion {
  uint32_t address;
  uint32_t length; /* instructions */
  uint64_t count;
} GcCoverageRegion;

#define GC_COVERAGE_TOP 12u

static void print_count(FILE *out, uint64_t value) {
  /* Thousands separated, because these are ten-digit numbers read by eye. */
  char digits[32], grouped[48];
  size_t length, i, o = 0;
  snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
  length = strlen(digits);
  for (i = 0; i < length; i++) {
    if (i && (length - i) % 3u == 0u) grouped[o++] = ',';
    grouped[o++] = digits[i];
  }
  grouped[o] = 0;
  fputs(grouped, out);
}

void gc_coverage_report(FILE *out, const char *name, uint64_t total, const GcCoverage *recompiled_run,
                        const GcCoverage *everything) {
  const uint64_t interpreted = recompiled_run->instructions;
  const uint64_t recompiled = total > interpreted ? total - interpreted : 0u;
  GcCoverageRegion top[GC_COVERAGE_TOP];
  unsigned i, top_used = 0;
  uint64_t interpreted_addresses = 0;

  memset(top, 0, sizeof(top));
  for (i = 0; i < GC_COVERAGE_SLOTS; i++) {
    if (recompiled_run->count[i]) interpreted_addresses++;
  }

  fprintf(out, "%s: native share %.3f%% by instructions: ", name,
          total ? 100.0 * (double)recompiled / (double)total : 0.0);
  print_count(out, recompiled);
  fputs(" recompiled, ", out);
  print_count(out, interpreted);
  fputs(" interpreted, of ", out);
  print_count(out, total);
  fputs("\n", out);

  if (everything) {
    uint64_t ran = 0, native = 0, only_interpreted = 0;
    for (i = 0; i < GC_COVERAGE_SLOTS; i++) {
      if (!everything->count[i]) continue;
      ran++;
      /* Every execution the interpreted machine made that the recompiled
       * run did not interpret was recompiled. */
      if (everything->count[i] > recompiled_run->count[i]) native++;
      else only_interpreted++;
    }
    fprintf(out, "%s: native share %.3f%% by address: ", name, ran ? 100.0 * (double)native / (double)ran : 0.0);
    print_count(out, native);
    fputs(" of ", out);
    print_count(out, ran);
    fputs(" addresses that ran were recompiled, ", out);
    print_count(out, only_interpreted);
    fputs(" only ever interpreted, ", out);
    print_count(out, interpreted_addresses);
    fputs(" interpreted at least once\n", out);
  } else {
    fprintf(out, "%s: ", name);
    print_count(out, interpreted_addresses);
    fputs(" addresses interpreted (the share by address needs --differential, which has an\n"
          "  interpreted machine to say which addresses ran at all)\n", out);
  }
  if (recompiled_run->outside) {
    fprintf(out, "%s: ", name);
    print_count(out, recompiled_run->outside);
    fputs(" instructions interpreted outside main memory\n", out);
  }

  /* Where the interpreted instructions went, by the region each entered. */
  for (i = 0; i < GC_COVERAGE_SLOTS;) {
    GcCoverageRegion region;
    unsigned k;
    if (!recompiled_run->count[i]) {
      i++;
      continue;
    }
    region.address = 0x80000000u | (i << 2);
    region.length = 0;
    region.count = 0;
    do {
      region.count += recompiled_run->count[i];
      region.length++;
      i++;
    } while (i < GC_COVERAGE_SLOTS && recompiled_run->count[i] &&
             !(recompiled_run->entry[i] & GC_COVERAGE_ENTERED));
    for (k = 0; k < GC_COVERAGE_TOP; k++) {
      if (k >= top_used || region.count > top[k].count) break;
    }
    if (k < GC_COVERAGE_TOP) {
      memmove(&top[k + 1u], &top[k], (GC_COVERAGE_TOP - 1u - k) * sizeof(top[0]));
      top[k] = region;
      if (top_used < GC_COVERAGE_TOP) top_used++;
    }
  }
  if (top_used) {
    fprintf(out, "%s: most interpreted (entry, instructions long, instructions run):\n", name);
    for (i = 0; i < top_used; i++) {
      fprintf(out, "  %08x  %6u  ", top[i].address, top[i].length);
      print_count(out, top[i].count);
      fputs("\n", out);
    }
  }
}
