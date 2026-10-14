/*
 * The Gekko interpreter: decode, dispatch, exceptions and the SPR file.
 *
 * The interpreter exists for three reasons and is never a second
 * implementation of instruction semantics: it is the oracle that
 * --differential compares recompiled code against, it is the fallback for
 * code the analyzer could not reach, and it is what the test programs run
 * on before there is a recompiler at all. Every instruction here calls the
 * same force-inlined handler that generated code calls.
 */

#include <stdio.h>
#include <string.h>

#include "gc.h"
#include "gc_cpu.h"

#include "gc_bus.h"
#include "gc_cpu_impl.h"
#include "gc_cpu_ops.h"
#include "gc_fpu_impl.h"

/* Where NaNs are born, said when a run asks (gc_cpu.h). The processor is
 * the first member of a machine, whose log and field these are; nothing
 * sets the switch for a processor that stands alone. */
int gc_fp_invalid_trace = 0;
uint64_t gc_fp_invalid_from = 0;

void gc_fp_invalid_report(GcCpu *c, uint32_t which) {
  static unsigned said;
  GcMachine *m = (GcMachine *)c;
  const char *kind = which == GC_FPSCR_VXISI   ? "infinity less infinity"
                     : which == GC_FPSCR_VXIDI ? "infinity over infinity"
                     : which == GC_FPSCR_VXZDZ ? "nothing over nothing"
                     : which == GC_FPSCR_VXIMZ ? "nothing times infinity"
                     : which == GC_FPSCR_VXSQRT ? "a square root of less than nothing"
                     : which == GC_FPSCR_VXSNAN ? "a signalling NaN"
                                                : "another kind";
  if (!m->log || m->field < gc_fp_invalid_from || said >= 400u) return;
  said++;
  fprintf(m->log, "gc: field %llu: invalid operation (%s, %08x) at pc %08x, lr %08x%s\n",
          (unsigned long long)m->field, kind, which, c->pc, c->lr,
          said == 400u ? "; no more of these will be said" : "");
}

void gc_cpu_reset(GcCpu *c) {
  unsigned i;
  for (i = 0; i < 32; i++) {
    c->gpr[i] = 0;
    c->fpr[i].ps0 = 0;
    c->fpr[i].ps1 = 0;
  }
  c->pc = 0;
  c->npc = 0;
  c->cr = 0;
  c->xer = 0;
  c->lr = 0;
  c->ctr = 0;
  /* Out of reset the machine is in supervisor mode with translation off and
   * exceptions at the high vectors. The apploader state gcrecomp installs
   * (BATs, MSR[IR]/[DR]) is applied on top of this. */
  c->msr = GC_MSR_IP | GC_MSR_ME;
  c->fpscr = 0;
  c->fp_lazy = 0;
  for (i = 0; i < 8; i++) c->gqr[i] = 0;
  c->hid0 = 0;
  c->hid1 = 0;
  c->hid2 = 0;
  c->wpar = 0;
  c->dmau = 0;
  c->dmal = 0;
  c->l2cr = 0;
  c->sdr1 = 0;
  c->ear = 0;
  /* Gekko reports itself as a 750 derivative; games read PVR to pick code
   * paths. */
  c->pvr = 0x00083214u;
  c->srr0 = 0;
  c->srr1 = 0;
  c->dar = 0;
  c->dsisr = 0;
  for (i = 0; i < 4; i++) c->sprg[i] = 0;
  for (i = 0; i < 16; i++) c->sr[i] = 0;
  for (i = 0; i < 8; i++) {
    c->ibat[i] = 0;
    c->dbat[i] = 0;
  }
  c->reserve = 0;
  c->reserve_addr = 0;
  c->cycles = 0;
  c->tb_base = 0;
  c->tb_base_cycles = 0;
  c->dec = 0;
  c->dec_write_cycles = 0;
  c->next_event = 0;
  c->branched = 0;
  c->stopped = GC_STOP_NONE;
  c->stop_insn = 0;
  c->pending_vector = 0;
  gc_cpu_forget_fast(c);
}

uint32_t gc_cpu_fpscr(const GcCpu *c) {
  GcCpu copy;
  if (!c->fp_lazy) return c->fpscr;
  copy = *c;
  gc_fp_settle(&copy);
  return copy.fpscr;
}

uint64_t gc_cpu_timebase(const GcCpu *c) {
  return c->tb_base + (c->cycles - c->tb_base_cycles) / GC_CYCLES_PER_TB;
}

void gc_cpu_set_timebase(GcCpu *c, uint64_t value) {
  c->tb_base = value;
  c->tb_base_cycles = c->cycles;
}

/* The decrementer counts down at the timebase rate from the value last
 * written. Deriving it rather than stepping it keeps it exact however large
 * a slice the recompiled code ran for. */
static uint32_t gc_decrementer(const GcCpu *c) {
  const uint64_t elapsed = (c->cycles - c->dec_write_cycles) / GC_CYCLES_PER_TB;
  return c->dec - (uint32_t)elapsed;
}

uint32_t gc_spr_read(GcCpu *c, uint32_t spr) {
  uint32_t value = 0;
  switch (spr) {
    case GC_SPR_XER: return c->xer;
    case GC_SPR_LR: return c->lr;
    case GC_SPR_CTR: return c->ctr;
    case GC_SPR_DSISR: return c->dsisr;
    case GC_SPR_DAR: return c->dar;
    case GC_SPR_DEC: return gc_decrementer(c);
    case GC_SPR_SDR1: return c->sdr1;
    case GC_SPR_SRR0: return c->srr0;
    case GC_SPR_SRR1: return c->srr1;
    case GC_SPR_EAR: return c->ear;
    case GC_SPR_PVR: return c->pvr;
    case GC_SPR_TBL_R: return (uint32_t)gc_cpu_timebase(c);
    case GC_SPR_TBU_R: return (uint32_t)(gc_cpu_timebase(c) >> 32);
    case GC_SPR_HID0: return c->hid0;
    case GC_SPR_HID1: return c->hid1;
    case GC_SPR_HID2: return c->hid2;
    case GC_SPR_WPAR: return c->wpar;
    case GC_SPR_DMAU: return c->dmau;
    case GC_SPR_DMAL: return c->dmal;
    case GC_SPR_L2CR: return c->l2cr;
    default: break;
  }
  if (spr >= GC_SPR_SPRG0 && spr < GC_SPR_SPRG0 + 4) return c->sprg[spr - GC_SPR_SPRG0];
  if (spr >= GC_SPR_GQR0 && spr < GC_SPR_GQR0 + 8) return c->gqr[spr - GC_SPR_GQR0];
  if (spr >= GC_SPR_IBAT0U && spr < GC_SPR_IBAT0U + 8) return c->ibat[spr - GC_SPR_IBAT0U];
  if (spr >= GC_SPR_DBAT0U && spr < GC_SPR_DBAT0U + 8) return c->dbat[spr - GC_SPR_DBAT0U];
  if (gc_bus_spr_read(c, spr, &value)) return value;
  return 0;
}

void gc_spr_write(GcCpu *c, uint32_t spr, uint32_t value) {
  switch (spr) {
    case GC_SPR_XER: c->xer = value & GC_XER_WRITABLE; return;
    case GC_SPR_LR: c->lr = value; return;
    case GC_SPR_CTR: c->ctr = value; return;
    case GC_SPR_DSISR: c->dsisr = value; return;
    case GC_SPR_DAR: c->dar = value; return;
    case GC_SPR_DEC:
      c->dec = value;
      c->dec_write_cycles = c->cycles;
      c->next_event = 0; /* the moment it runs out has moved */
      return;
    case GC_SPR_SDR1: c->sdr1 = value; return;
    case GC_SPR_SRR0: c->srr0 = value; return;
    case GC_SPR_SRR1: c->srr1 = value; return;
    case GC_SPR_EAR: c->ear = value; return;
    case GC_SPR_TBL_W: gc_cpu_set_timebase(c, (gc_cpu_timebase(c) & 0xffffffff00000000ull) | value); return;
    case GC_SPR_TBU_W:
      gc_cpu_set_timebase(c, ((uint64_t)value << 32) | (uint32_t)gc_cpu_timebase(c));
      return;
    case GC_SPR_HID0: c->hid0 = value; return;
    case GC_SPR_HID1: c->hid1 = value; return;
    case GC_SPR_HID2:
      c->hid2 = value;
      gc_cpu_forget_fast(c); /* the locked cache may have come or gone */
      return;
    case GC_SPR_L2CR: c->l2cr = value; return;
    default: break;
  }
  if (spr >= GC_SPR_SPRG0 && spr < GC_SPR_SPRG0 + 4) {
    c->sprg[spr - GC_SPR_SPRG0] = value;
    return;
  }
  if (spr >= GC_SPR_GQR0 && spr < GC_SPR_GQR0 + 8) {
    c->gqr[spr - GC_SPR_GQR0] = value;
    return;
  }
  if (spr >= GC_SPR_IBAT0U && spr < GC_SPR_IBAT0U + 8) {
    c->ibat[spr - GC_SPR_IBAT0U] = value;
    return;
  }
  if (spr >= GC_SPR_DBAT0U && spr < GC_SPR_DBAT0U + 8) {
    c->dbat[spr - GC_SPR_DBAT0U] = value;
    gc_cpu_forget_fast(c); /* rebuilt from the new BATs when next needed */
    return;
  }
  /* The write-gather pipe (WPAR) and the locked-cache DMA (DMAU/DMAL) have
   * side effects the machine owns. */
  if (gc_bus_spr_write(c, spr, value)) return;
}

void gc_cpu_exception(GcCpu *c, uint32_t vector) {
  /* Where the handler returns to.
   *
   * For almost every exception that is the instruction that caused it:
   * it has not completed, and it runs again once whatever went wrong has
   * been dealt with. A system call is the exception to that. `sc` *has*
   * completed - asking is the whole of what it does - so the architecture
   * specifies the instruction after it, and returning to the `sc` itself
   * executes it again, and again.
   *
   * Nothing in the test programs uses `sc`, so this cost nothing for a
   * long time. A commercial game uses it on every cache flush: Shrek 2
   * spent a hundred per cent of its time alternating between one `sc` and
   * the eight instructions of its handler, which from the outside looked
   * like a game waiting for hardware that never answered. */
  c->srr0 = (vector == GC_VEC_SYSCALL) ? (c->pc + 4u) : c->pc;
  /* SRR1 keeps the MSR bits the architecture saves plus the exception's own
   * status bits, which the caller has already put in place. */
  c->srr1 = (c->srr1 & 0x783f0000u) | (c->msr & 0x0000ff73u);
  c->msr &= ~(GC_MSR_POW | GC_MSR_EE | GC_MSR_PR | GC_MSR_FP | GC_MSR_FE0 | GC_MSR_SE |
              GC_MSR_BE | GC_MSR_FE1 | GC_MSR_IR | GC_MSR_DR | GC_MSR_RI | GC_MSR_LE);
  if (c->msr & GC_MSR_ILE) c->msr |= GC_MSR_LE;
  c->npc = ((c->msr & GC_MSR_IP) ? 0xfff00000u : 0u) + vector;
  c->pending_vector = 0;
  c->branched = 1;
}

/* Each handler in a function of its own, which the dispatch below calls.
 *
 * Inlined straight into the switch, all 221 handlers made one function that
 * took the optimiser 10 seconds alone and three times that beside the rest
 * of a game's build, where it was the last thing every build waited for;
 * the compiler's time grows far faster than the function it is given. As
 * functions of their own it is under two seconds. What the interpreter
 * pays is a call per instruction, measured in docs/GC-PLAN.md. */
#if defined(_MSC_VER)
#define GC_STEP_FN static __declspec(noinline) void
#else
#define GC_STEP_FN static __attribute__((noinline)) void
#endif
#define GC_STEP(enum_name, handler) \
  GC_STEP_FN gc_step_##handler(GcCpu *c, uint32_t insn) { handler(c, insn); }
GC_CPU_OPS(GC_STEP)
#undef GC_STEP

uint32_t gc_cpu_step(GcCpu *c) {
  uint32_t insn;
  GcOp op;

  c->branched = 0;
  c->npc = c->pc + 4u;
  insn = gc_bus_fetch(c, c->pc);
  if (c->pending_vector) {
    const uint32_t vector = c->pending_vector;
    gc_cpu_exception(c, vector);
    c->pc = c->npc;
    c->cycles += 1;
    return 1;
  }

  if (!(c->msr & GC_MSR_FP) && gc_insn_needs_fp(insn)) {
    /* The floating-point unit is off (gc_cpu_ops.h): the instruction does
     * not run, and comes back to once the handler has switched it on. */
    c->srr1 = 0;
    gc_cpu_exception(c, GC_VEC_FP_UNAVAILABLE);
    c->pc = c->npc;
    c->cycles += 1;
    return 1;
  }

  op = gc_decode(insn);
  switch (op) {
/* One case per opcode, from the single list in gc_cpu_ops.h that the
     * recompiler's emitter also reads. Keeping the dispatch here and a
     * table there is how an emitter and an interpreter drift apart. */
#define GC_CASE(enum_name, handler) \
  case enum_name: gc_step_##handler(c, insn); break;
    GC_CPU_OPS(GC_CASE)
#undef GC_CASE

    case GC_OP_ILLEGAL:
      c->stopped = GC_STOP_ILLEGAL;
      c->stop_insn = insn;
      return 0;

    default:
      /* A real Gekko opcode this core does not implement yet. Stopping is
       * the fail-closed answer: a silently ignored instruction would show
       * up much later as a wrong frame. */
      c->stopped = GC_STOP_UNHANDLED;
      c->stop_insn = insn;
      return 0;
  }

  gc_cpu_retire(c);
  return 1;
}

/* One instruction's handler and nothing around it: no fetch, no retirement.
 * Recompiled code calls this for an instruction its emitter hands to the
 * handler out of line (gc_rt_insn in gc_recompiled_rt.c), through the same
 * per-opcode functions as the dispatch above. An opcode with no handler
 * stops the core, as stepping it would. */
void gc_cpu_execute(GcCpu *c, uint32_t insn) {
  switch (gc_decode(insn)) {
#define GC_CASE(enum_name, handler) \
  case enum_name: gc_step_##handler(c, insn); return;
    GC_CPU_OPS(GC_CASE)
#undef GC_CASE
    default:
      c->stopped = GC_STOP_UNHANDLED;
      c->stop_insn = insn;
      return;
  }
}

void gc_cpu_retire(GcCpu *c) {
  /* A GcCpu is the first member of a GcMachine, so this is the machine. */
  GcMachine *m = (GcMachine *)c;
  if (m->keep_branch_history && c->branched) {
    /* A repeat of the branch just recorded is a loop going round again,
     * and a loop of two instructions will fill any ring before the
     * interesting part of the history reaches it. Counting repeats instead
     * of recording them keeps the record long enough to be worth having. */
    const unsigned last = (m->branch_next + GC_BRANCH_HISTORY - 1u) % GC_BRANCH_HISTORY;
    if (m->branch_from[last] == c->pc && m->branch_to[last] == c->npc) {
      m->branch_repeat[last]++;
    } else {
      m->branch_from[m->branch_next] = c->pc;
      m->branch_to[m->branch_next] = c->npc;
      m->branch_repeat[m->branch_next] = 0;
      m->branch_next = (m->branch_next + 1u) % GC_BRANCH_HISTORY;
    }
  }
  if (c->pending_vector) {
    gc_cpu_exception(c, c->pending_vector);
  }
  c->pc = c->npc;
  c->cycles += 1;
}

void gc_cpu_run(GcCpu *c, uint64_t until_cycles) {
  while (c->cycles < until_cycles && !c->stopped) {
    if (!gc_cpu_step(c)) return;
  }
}
