/*
 * Recompiled blocks against the interpreter, on random machine states.
 *
 * gc_recompiled_fuzz_gen writes random blocks through gcrecomp's block
 * emitter (every opcode the core implements, at every position, in blocks
 * that fall through into a second one); this runs each of them from many
 * random states - registers, floating-point values of every class, FPSCR in
 * and out of the steady state, GQRs, a reservation, problem state, a data
 * window of random bytes the base registers point into, an event falling
 * inside the block or not, the shortcut to main memory built or not - on
 * one machine, and the same instructions through the interpreter on a
 * second machine given the same state, and compares every register and the
 * data window after the same number of instructions. Written-out
 * instructions, handler fallbacks, write-back labels, side exits, the slow
 * paths through the bus and the event helpers are all held to the
 * interpreter this way, which is what keeps the emitter and the handlers
 * from drifting apart.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_bus.h"
#include "gc_cpu.h"
#include "gc_recompiled_rt.h"

typedef struct GcFuzzCase {
  uint32_t start;
  unsigned first, total;
  void *(*run)(GcMachine *m);
} GcFuzzCase;

extern const GcFuzzCase gc_fuzz_cases[];
extern const unsigned gc_fuzz_case_count;
extern const uint32_t gc_fuzz_code_base;
extern const unsigned gc_fuzz_code_size;
extern const unsigned char gc_fuzz_code[];

#define WINDOW 0x80400000u
#define WINDOW_SIZE 0x10000u

static uint64_t state = 0x853c49e6748fea9bull;

static uint32_t next32(void) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (uint32_t)(state >> 11);
}

static uint64_t next64(void) { return ((uint64_t)next32() << 32) | next32(); }

static uint32_t random_gpr(unsigned r) {
  if (r >= 1 && r <= 6) return WINDOW + (next32() % (WINDOW_SIZE - 0x400u)) + 0x200u;
  switch (next32() % 8u) {
    case 0: return 0;
    case 1: return 0xffffffffu;
    case 2: return 0x80000000u;
    case 3: return next32() % 64u;
    case 4: return 0x7fffffffu;
    default: {
      uint32_t v = next32();
      /* Keep random addresses off the register blocks. */
      if ((v >> 24) == 0xcc || (v >> 24) == 0xcd || (v >> 24) == 0xc8) v ^= 0x40000000u;
      return v;
    }
  }
}

static uint64_t random_fpr(void) {
  switch (next32() % 14u) {
    case 12:
    case 13: {
      /* A small whole number times a power of two: a single whose low 26
       * significand bits are clear, as the multiply fast paths want. */
      double d = (double)((int32_t)(next32() % 8192u) - 4096) * (double)(1u << (next32() % 12u)) / 1024.0;
      uint64_t bits;
      memcpy(&bits, &d, 8);
      return bits;
    }
    case 0: return 0;
    case 1: return 0x8000000000000000ull;
    case 2: return 0x7ff0000000000000ull | (next32() & 1u ? 0x8000000000000000ull : 0);
    case 3: return 0x7ff8000000000000ull | (next64() & 0x0007ffffffffffffull); /* quiet NaN */
    case 4: return 0x7ff0000000000001ull | (next64() & 0x0007ffffffffffffull); /* signalling NaN */
    case 5: return next64() & 0x800fffffffffffffull;                          /* denormal */
    case 6: {
      /* A single in double format. */
      float f = (float)((int32_t)next32()) / (float)(1u << (next32() % 31u));
      double d = (double)f;
      uint64_t bits;
      memcpy(&bits, &d, 8);
      return bits;
    }
    case 7: {
      double d = (double)(int32_t)(next32() % 2000u) - 1000.0;
      uint64_t bits;
      memcpy(&bits, &d, 8);
      return bits;
    }
    default: {
      /* A moderate single, as the fast paths want. */
      float f = (float)(next32() % 100000u) / 256.0f * ((next32() & 1u) ? 1.0f : -1.0f);
      double d = (double)f;
      uint64_t bits;
      memcpy(&bits, &d, 8);
      return bits;
    }
  }
}

static uint32_t random_fpscr(void) {
  switch (next32() % 4u) {
    case 0: return 0;
    case 1: return GC_FPSCR_FX | GC_FPSCR_XX;          /* the steady state */
    case 2: return GC_FPSCR_FX | GC_FPSCR_XX | GC_FPSCR_NI;
    default: return next32() & ~(GC_FPSCR_FEX | GC_FPSCR_VX | GC_FPSCR_RESERVED | 0xf8u);
  }
}

static uint32_t random_gqr(void) {
  static const unsigned types[] = {0, 4, 5, 6, 7, 0, 0, 4};
  const unsigned lt = types[next32() % 8u], st = types[next32() % 8u];
  const unsigned ls = next32() % 64u, ss = next32() % 64u;
  return (ls << 24) | (lt << 16) | (ss << 8) | st;
}

/* The state compared: everything but what only says how the machine is
 * being run (the shortcut, the next event, the branch flag, the next
 * address), with FPSCR as the guest reads it, and a pending note as it is. */
static void normalize(const GcCpu *from, GcCpu *to) {
  *to = *from;
  to->fpscr = gc_cpu_fpscr(from);
  /* A note still pending has to be the same note: kind, operands, result. */
  if (!from->fp_lazy) to->fp_lazy_a = to->fp_lazy_b = to->fp_lazy_c = to->fp_lazy_result = 0;
  to->npc = 0;
  to->branched = 0;
  to->next_event = 0;
  memset(to->fast, 0, sizeof(to->fast));
  to->fast_ready = 0;
  to->machine = NULL;
}

static void report(const GcCpu *a, const GcCpu *b) {
  unsigned i;
  if (a->pc != b->pc) printf("    pc: recompiled %08x, interpreted %08x\n", a->pc, b->pc);
  if (a->cycles != b->cycles) printf("    cycles: recompiled %llu, interpreted %llu\n", (unsigned long long)a->cycles, (unsigned long long)b->cycles);
  for (i = 0; i < 32; i++) {
    if (a->gpr[i] != b->gpr[i]) printf("    r%u: recompiled %08x, interpreted %08x\n", i, a->gpr[i], b->gpr[i]);
    if (a->fpr[i].ps0 != b->fpr[i].ps0 || a->fpr[i].ps1 != b->fpr[i].ps1) {
      printf("    f%u: recompiled %016llx/%016llx, interpreted %016llx/%016llx\n", i,
             (unsigned long long)a->fpr[i].ps0, (unsigned long long)a->fpr[i].ps1,
             (unsigned long long)b->fpr[i].ps0, (unsigned long long)b->fpr[i].ps1);
    }
  }
  if (a->cr != b->cr) printf("    cr: recompiled %08x, interpreted %08x\n", a->cr, b->cr);
  if (a->xer != b->xer) printf("    xer: recompiled %08x, interpreted %08x\n", a->xer, b->xer);
  if (a->lr != b->lr) printf("    lr: recompiled %08x, interpreted %08x\n", a->lr, b->lr);
  if (a->ctr != b->ctr) printf("    ctr: recompiled %08x, interpreted %08x\n", a->ctr, b->ctr);
  if (a->msr != b->msr) printf("    msr: recompiled %08x, interpreted %08x\n", a->msr, b->msr);
  if (a->fpscr != b->fpscr) printf("    fpscr: recompiled %08x, interpreted %08x\n", a->fpscr, b->fpscr);
  if (a->srr0 != b->srr0) printf("    srr0: recompiled %08x, interpreted %08x\n", a->srr0, b->srr0);
  if (a->reserve != b->reserve) printf("    reserve: recompiled %u, interpreted %u\n", a->reserve, b->reserve);
  if (a->stopped != b->stopped) printf("    stopped: recompiled %u, interpreted %u\n", a->stopped, b->stopped);
}

int main(int argc, char **argv) {
  const unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 24u;
  GcMachine *a = gc_machine_create(), *b = gc_machine_create();
  unsigned k, round, failures = 0, runs = 0, early = 0;
  uint64_t cycles = 100000u;
  if (!a || !b) {
    printf("gc_recompiled_fuzz: cannot create the machines\n");
    return 1;
  }
  a->log = b->log = NULL;
  gc_install_boot_state(a);
  gc_install_boot_state(b);
  memcpy(gc_machine_ram(a, gc_fuzz_code_base & 0x0fffffffu, gc_fuzz_code_size), gc_fuzz_code, gc_fuzz_code_size);
  memcpy(gc_machine_ram(b, gc_fuzz_code_base & 0x0fffffffu, gc_fuzz_code_size), gc_fuzz_code, gc_fuzz_code_size);

  for (k = 0; k < gc_fuzz_case_count; k++) {
    const GcFuzzCase *fc = &gc_fuzz_cases[k];
    for (round = 0; round < rounds; round++) {
      GcCpu *const ca = &a->cpu, *const cb = &b->cpu;
      GcCpu na, nb, start_state;
      unsigned i;
      uint8_t *wa = gc_machine_ram(a, WINDOW & 0x0fffffffu, WINDOW_SIZE);
      uint8_t *wb = gc_machine_ram(b, WINDOW & 0x0fffffffu, WINDOW_SIZE);
      for (i = 0; i < WINDOW_SIZE; i++) wa[i] = (uint8_t)next32();
      memcpy(wb, wa, WINDOW_SIZE);

      /* A block can write BATs, HID2, segment registers and the decrementer;
       * every run starts from the state a game boots in. */
      gc_cpu_reset(ca);
      gc_install_boot_state(a);
      gc_install_boot_state(b);
      for (i = 0; i < 32; i++) {
        ca->gpr[i] = random_gpr(i);
        ca->fpr[i].ps0 = random_fpr();
        ca->fpr[i].ps1 = (next32() & 3u) ? random_fpr() : ca->fpr[i].ps0;
      }
      ca->cr = next32();
      ca->xer = next32() & GC_XER_WRITABLE;
      ca->lr = (next32() & 1u) ? random_gpr(1) : next32();
      ca->ctr = (next32() & 1u) ? next32() % 4u : next32();
      ca->fpscr = random_fpscr();
      ca->fp_lazy = 0;
      for (i = 0; i < 8; i++) ca->gqr[i] = random_gqr();
      /* One run in eight with the floating-point unit off: a block that
       * uses it then takes the exception at its first such instruction,
       * recompiled as interpreted. */
      ca->msr = GC_MSR_IR | GC_MSR_DR | ((next32() % 8u) ? GC_MSR_FP : 0u) | GC_MSR_ME |
                ((next32() % 10u) == 0 ? GC_MSR_PR : 0u);
      ca->hid2 = GC_HID2_LSQE | GC_HID2_WPE | ((next32() % 8u) ? GC_HID2_PSE : 0u);
      ca->reserve = (next32() % 16u) == 0;
      ca->reserve_addr = WINDOW & 0x0fffffffu;
      ca->pending_vector = 0;
      ca->stopped = 0;
      ca->srr0 = ca->srr1 = 0;
      /* A little further on each time, staying well inside the first field:
       * a machine run for fields without its frontend backs sound up. */
      cycles += 1u + next32() % 16u;
      ca->cycles = cycles;
      ca->pc = ca->npc = fc->start;
      gc_cpu_forget_fast(ca);
      if (next32() & 1u) (void)gc_bus_read8(ca, WINDOW); /* build the shortcut */

      /* The same machine for the interpreter. */
      {
        void *const machine = cb->machine;
        *cb = *ca;
        cb->machine = machine;
        gc_cpu_forget_fast(cb);
      }
      gc_machine_sync(a);
      gc_machine_sync(b);
      /* Far enough for any block, near enough that one which loops on itself
       * (a random bdnz back to its start) comes back within 20,000. */
      ca->next_event = (next32() % 5u) == 0 ? cycles + next32() % (fc->total + 2u) : cycles + 20000u;

      start_state = *ca;
      (void)fc->run(a);
      runs++;
      if (ca->cycles - cycles < fc->total) early++;
      while (cb->cycles < ca->cycles && !cb->stopped) {
        gc_machine_sync(b);
        gc_machine_step(b);
      }

      normalize(ca, &na);
      normalize(cb, &nb);
      if (memcmp(&na, &nb, sizeof(na)) != 0 || memcmp(wa, wb, WINDOW_SIZE) != 0) {
        if (failures < 12) {
          printf("FAIL: case %u (block %08x, %u instructions, %u in the first block), round %u:\n", k, fc->start,
                 fc->total, fc->first, round);
          for (i = 0; i < fc->total; i++) {
            const uint32_t at = fc->start - gc_fuzz_code_base + 4u * i;
            const uint32_t word = ((uint32_t)gc_fuzz_code[at] << 24) | ((uint32_t)gc_fuzz_code[at + 1] << 16) |
                                  ((uint32_t)gc_fuzz_code[at + 2] << 8) | gc_fuzz_code[at + 3];
            printf("    %08x %08x %s\n", fc->start + 4u * i, word, gc_op_info[gc_decode(word)].name);
          }
          report(&na, &nb);
          printf("    before: msr %08x reserve %u next_event-cycles %lld fast %u\n", start_state.msr, start_state.reserve, (long long)(start_state.next_event - start_state.cycles), start_state.fast_ready);
          for (i = 0; i < 32; i++) printf("%s r%u=%08x", (i % 6) ? "" : "\n   ", i, start_state.gpr[i]);
          printf("\n");
          if (memcmp(wa, wb, WINDOW_SIZE) != 0) printf("    the data window differs\n");
          {
            const uint8_t *ma = gc_machine_ram(a, 0, GC_MEM1_SIZE), *mb = gc_machine_ram(b, 0, GC_MEM1_SIZE);
            uint32_t q, shown = 0;
            for (q = 0; q < GC_MEM1_SIZE && shown < 4; q++) {
              if (ma[q] != mb[q]) { printf("    memory differs at %08x: %02x %02x\n", q, ma[q], mb[q]); shown++; q = (q | 0xfff); }
            }
          }
        }
        failures++;
        /* Take the two machines back to one state. */
        {
          void *const machine = cb->machine;
          *cb = *ca;
          cb->machine = machine;
          gc_cpu_forget_fast(cb);
        }
      }
      if (cb->cycles > cycles) cycles = cb->cycles;
      if (ca->cycles > cycles) cycles = ca->cycles;
    }
  }
  printf("gc_recompiled_fuzz: %u cases, %u runs (%u left their block early), %u failures\n", gc_fuzz_case_count, runs,
         early, failures);
  gc_machine_destroy(a);
  gc_machine_destroy(b);
  return failures ? 1 : 0;
}
