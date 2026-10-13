/*
 * The out-of-line half of recompiled code (gc_recompiled_rt.h).
 *
 * Every function here is entered with the machine not quite current: the
 * block that calls keeps its registers in locals, and only the program
 * counter and the instruction count are handed over. That is enough for
 * everything below, because nothing here reads a general register, CR,
 * XER, LR or CTR: the bus translates with MSR and the BATs, an exception
 * saves the program counter and MSR, and gc_machine_due looks at the clocks
 * and the interrupt lines. The one thing in gc_machine_due that reads
 * registers - a hooked function answered in place (gc_os_hook) - happens
 * only at a hooked address, and the dispatcher leaves every block that
 * contains one to the interpreter (build_table in the generated
 * recompiled_run.c). The interpreter stepping a block to its end
 * (step_block) reads everything, so gc_rt_exit runs it only after the block
 * has written its registers back.
 */
/* The handlers first: gc_recompiled_rt.h adds the lane kernels only where
 * they are included. */
#include "gc_cpu_impl.h"
#include "gc_fpu_impl.h"
#include "gc_recompiled_rt.h"

const GcFastMap gc_rt_no_map = {{0}, {0}};
uint64_t gc_rt_until_field = ~(uint64_t)0;
int gc_rt_status;
GcRtReturn gc_rt_returns[GC_RT_RETURNS];
unsigned gc_rt_return_top;

/* Set when a block has to leave because something is due at an instruction
 * inside it, and read by gc_rt_exit once the block has written its
 * registers back. One machine runs recompiled code at a time. */
static int step_pending;

void gc_cpu_execute(GcCpu *c, uint32_t insn);

/* The interpreter, one instruction at a time, from the program counter to
 * the end of the block - or to wherever control leaves it first: a taken
 * branch, an exception, an interrupt, a stop. This is what the block would
 * have done with its per-instruction checks, and what the dispatcher does
 * for an address with no block (gc_machine_sync, then gc_machine_step), so
 * the block boundary the dispatcher sees next is the one it always saw. */
static void step_block(GcMachine *m, uint32_t end) {
  GcCpu *const c = &m->cpu;
  for (;;) {
    const uint32_t from = c->pc;
    gc_machine_sync(m);
    gc_machine_step(m);
    c->next_event = 0;
    if (c->stopped || m->finished || c->pc != from + 4u || c->pc == end) break;
  }
}

void *gc_rt_enter(GcMachine *m, uint32_t start, uint32_t count, uint32_t first, GcRecompiledFn self) {
  GcCpu *const c = &m->cpu;
  /* A function can hold blocks that follow its first one and are entered
   * only by falling into them; the dispatcher's view is still block by
   * block, so what runs here without the function stops at the first
   * block's end. */
  const uint32_t end = start + 4u * first;
  /* What the dispatcher does between blocks, which a block that runs the
   * next block itself skips: a field that ended at the last instruction is
   * counted, and the run stops at this block boundary if that was the field
   * it ran to, or the machine finished or stopped. Anything that changes the
   * field, finishes or stops the machine also leaves next_event at or below
   * the count, so the entry check of the next block comes here. */
  if (c->cycles >= m->next_field_cycles) {
    gc_machine_sync(m);
    c->next_event = 0;
  }
  if (m->field >= gc_rt_until_field || m->finished || c->stopped) return NULL;
  /* What the first instruction's check would have done. */
  if (c->cycles >= c->next_event) {
    if (gc_machine_due(m)) return NULL;
    if (c->pc != start) return NULL;
  }
  /* Nothing else inside: the block runs now, as it would have after that
   * check - even if the check just ended the field the run was for, in
   * which case the dispatcher has to see the block end, as it always did,
   * and not the block it says comes next. */
  if (c->cycles + count <= c->next_event) {
    if (m->field >= gc_rt_until_field) {
      /* The check ended the field the run was for: the dispatcher returns
       * after this block, and so the rest of the function must not run. */
      step_block(m, end);
      return NULL;
    }
    return self(m);
  }
  step_block(m, end);
  return NULL;
}

/* A block that uses the floating-point unit, entered with the unit off:
 * the interpreter runs it, because it is the interpreter that raises the
 * exception the first such instruction asks for (gc_cpu_ops.h). What the
 * dispatcher does between blocks first, as gc_rt_enter does it. */
void *gc_rt_enter_stepped(GcMachine *m, uint32_t start, uint32_t first) {
  GcCpu *const c = &m->cpu;
  if (c->cycles >= m->next_field_cycles) {
    gc_machine_sync(m);
    c->next_event = 0;
  }
  if (m->field >= gc_rt_until_field || m->finished || c->stopped) return NULL;
  step_block(m, start + 4u * first);
  return NULL;
}

void *gc_rt_exit(GcMachine *m, uint32_t end) {
  if (step_pending) {
    step_pending = 0;
    step_block(m, end);
  }
  return NULL;
}

/* Before the instruction at `pc`, count `cycles`, with `left` instructions
 * of the block from there on (at least one): the per-instruction check,
 * and whether the rest of the block is still clear of events. */
int gc_rt_resync(GcMachine *m, uint32_t pc, uint64_t cycles, uint32_t left) {
  GcCpu *const c = &m->cpu;
  c->pc = c->npc = pc;
  c->cycles = cycles;
  if (cycles >= c->next_event) {
    if (gc_machine_due(m)) return 1;
    if (c->pc != pc) return 1;
  }
  if (cycles + left > c->next_event) {
    step_pending = 1;
    return 1;
  }
  return 0;
}

/* After an out-of-line access by the instruction at `pc`: 1 when it raised
 * an exception (taken now, and the instruction wrote nothing), 2 when it
 * completed but the block has to leave after it, 0 to go on. */
static int after_access(GcMachine *m, uint32_t pc, uint64_t cycles, uint32_t left) {
  GcCpu *const c = &m->cpu;
  if (c->pending_vector) {
    c->npc = pc + 4u;
    gc_cpu_exception(c, c->pending_vector);
    c->pc = c->npc;
    c->cycles = cycles + 1u;
    return 1;
  }
  if (c->stopped) {
    c->npc = c->pc = pc + 4u;
    c->cycles = cycles + 1u;
    return 2;
  }
  /* The last instruction of a block leaves the next check to the block
   * that follows, as it always did. */
  if (!left) return 0;
  if (c->next_event >= cycles + 1u + left) return 0;
  return gc_rt_resync(m, pc + 4u, cycles + 1u, left) ? 2 : 0;
}

uint64_t gc_rt_load(GcMachine *m, uint32_t ea, uint64_t cycles, uint64_t where) {
  GcCpu *const c = &m->cpu;
  const uint32_t pc = (uint32_t)where, left = (uint32_t)(where >> 32) & 0xffffffu;
  uint64_t value;
  c->pc = c->npc = pc;
  c->cycles = cycles;
  switch (where >> 56) {
    case 1: value = gc_bus_fast_read8(c, ea); break;
    case 2: value = gc_bus_fast_read16(c, ea); break;
    case 4: value = gc_bus_fast_read32(c, ea); break;
    default: value = gc_bus_fast_read64(c, ea); break;
  }
  gc_rt_status = after_access(m, pc, cycles, left);
  return value;
}

int gc_rt_store(GcMachine *m, uint32_t ea, uint64_t value, uint64_t cycles, uint64_t where) {
  GcCpu *const c = &m->cpu;
  const uint32_t pc = (uint32_t)where, left = (uint32_t)(where >> 32) & 0xffffffu;
  const unsigned size = (unsigned)(where >> 56);
  c->pc = c->npc = pc;
  c->cycles = cycles;
  switch (size) {
    case 1: gc_bus_fast_write8(c, ea, (uint8_t)value); break;
    case 2: gc_bus_fast_write16(c, ea, (uint16_t)value); break;
    case 4: gc_bus_fast_write32(c, ea, (uint32_t)value); break;
    default: gc_bus_fast_write64(c, ea, value); break;
  }
  return after_access(m, pc, cycles, left);
}

/* A quantised load or store the shortcut did not cover: the instruction's
 * own path through the bus. `insn` is the instruction word, so the
 * handler's own field decoding picks the register, GQR and W. */
int gc_rt_psq(GcMachine *m, uint32_t insn, uint32_t ea, int store, uint32_t pc, uint64_t cycles, uint32_t left) {
  GcCpu *const c = &m->cpu;
  const int indexed = (insn >> 26) == 4u;
  const unsigned gqr = indexed ? GC_F_ps_IX(insn) : GC_F_ps_I(insn);
  const unsigned w = indexed ? GC_F_ps_WX(insn) : GC_F_ps_W(insn);
  c->pc = c->npc = pc;
  c->cycles = cycles;
  if (store) {
    gc_psq_store(c, insn, ea, gqr, w);
  } else {
    gc_psq_load(c, insn, ea, gqr, w);
  }
  return after_access(m, pc, cycles, left);
}

#define GC_RT_FP_DEFINE(handler) \
  GC_RT_NOINLINE void gc_rt_##handler(GcCpu *c, uint32_t insn) { handler(c, insn); }
GC_RT_FP_CALLS(GC_RT_FP_DEFINE)
#undef GC_RT_FP_DEFINE

GC_RT_NOINLINE GcRtLanes GC_RT_VECTORCALL gc_rt_ps_binary_call(GcRtLanes a, GcRtLanes b, unsigned kind) {
  GcRtLanes r;
  return gc_rt_ps_binary(a, b, (GcFpKind)kind, &r) ? r : gc_rt_lanes_set(~(uint64_t)0, ~(uint64_t)0);
}

GC_RT_NOINLINE GcRtLanes GC_RT_VECTORCALL gc_rt_ps_madd_call(GcRtLanes a, GcRtLanes b, GcRtLanes cc, unsigned negations) {
  GcRtLanes r;
  return gc_rt_ps_madd(a, b, cc, (int)(negations >> 1), (int)(negations & 1u), &r) ? r
                                                                                 : gc_rt_lanes_set(~(uint64_t)0, ~(uint64_t)0);
}

GC_RT_NOINLINE int gc_rt_psq_store_fast(GcCpu *c, uint32_t insn, uint32_t ea, unsigned gqr_index, unsigned w) {
  return gc_psq_store_fast(c, insn, ea, gqr_index, w);
}

/* An instruction the emitter did not write out: its handler, with the
 * per-instruction glue of the old emitter (GC_INSN) around it. The block
 * wrote its registers back first and reads them again after. 1 when the
 * block has to leave (a branch was taken, an exception or a stop, an event
 * before the next instruction), 0 to go on. */
int gc_rt_insn(GcMachine *m, uint32_t insn, uint32_t pc, uint64_t cycles, uint32_t left) {
  GcCpu *const c = &m->cpu;
  c->pc = pc;
  c->cycles = cycles;
  c->npc = pc + 4u;
  c->branched = 0;
  gc_cpu_execute(c, insn);
  if (c->pending_vector) gc_cpu_exception(c, c->pending_vector);
  c->pc = c->npc;
  c->cycles = cycles + 1u;
  if (c->branched || c->stopped) return 1;
  /* An mtmsr that left the floating-point unit off: what is left of the
   * block was entered with it on and may use it, so the interpreter runs
   * the rest (gc_rt_enter_stepped). */
  if (left && !(c->msr & GC_MSR_FP) && (insn & 0xfc0007feu) == 0x7c000124u) {
    step_pending = 1;
    return 1;
  }
  if (!left || c->next_event >= cycles + 1u + left) return 0;
  return gc_rt_resync(m, pc + 4u, cycles + 1u, left);
}
