/*
 * The machine: creation, reset, and the loop that runs the CPU with
 * everything else that is due.
 *
 * A "field" here is one video field, 1/60 of a second, because that is the
 * unit a headless run counts and the unit the video interface interrupts
 * on. Nothing is drawn yet - the renderer is P3 - but the timing a program
 * waits on is real, so a program that polls for a retrace makes progress.
 */

#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_audio.h"
#include "gc_bus.h"
#include "gc_coverage.h"
#include "gc_gl.h"
#include "gc_gx_thread.h"
#include "gc_memory.h"
#include "gc_perf.h"
#include "wii.h"

void gc_clear_symbols(void);
void gc_vi_field_boundary(GcMachine *m);
void gc_vi_beam(GcMachine *m, uint32_t previous, uint32_t now);
void gc_ai_advance(GcMachine *m);
void gc_dsp_advance(GcMachine *m);
void gc_card_eject(GcMachine *m);
void gc_card_advance(GcMachine *m);
void gc_audio_advance(GcMachine *m);
void gc_audio_reset(GcMachine *m);
void gc_audio_free(GcMachine *m);
void gc_ax_reset(GcMachine *m);
void gc_si_field_boundary(GcMachine *m);

/* Which console this process runs (gc.h); gc_console_select changes them. */
uint32_t gc_cpu_hz = 486000000u;
uint32_t gc_cycles_per_vi_clock = 18u;
uint32_t gc_ram_size = GC_MEM1_SIZE;

GcMachine *gc_machine_create(void) {
  GcMachine *m = (GcMachine *)calloc(1, sizeof(GcMachine));
  if (!m) return NULL;
  /* All of main memory in one block: MEM1, and on a Wii MEM2 after it. */
  m->mem1 = gc_memory_create(gc_ram_size);
  m->aram = (uint8_t *)calloc(1, GC_ARAM_SIZE);
  if (!m->mem1 || !m->aram || gc_efb_create(&m->efb) != 0 || wii_machine_create(m) != 0) {
    gc_machine_destroy(m);
    return NULL;
  }
  m->log = stderr;
  gc_machine_reset(m);
  return m;
}

void gc_machine_destroy(GcMachine *m) {
  if (!m) return;
  gc_gx_thread_stop(m);
  gc_gl_stop(m);
  gc_card_eject(m);
  gc_audio_free(m);
  gc_ax_reset(m);
  wii_machine_destroy(m);
  gc_memory_destroy(m->mem1);
  free(m->aram);
  gc_efb_destroy(&m->efb);
  free(m);
}

void gc_machine_reset(GcMachine *m) {
  FILE *log = m->log;
  const int trace = m->trace_io;
  uint8_t *mem1 = m->mem1, *aram = m->aram;
  GcEfb efb = m->efb;
  /* The OpenGL path belongs to the host, like the buffers: a reset must not
   * leak its context by forgetting it. */
  void *gl = m->gl;
  /* And a Wii's own state, which hangs off the processor (wii.h). */
  void *wii = m->cpu.machine;

  /* A graphics thread finishes what it was given before memory goes. */
  gc_gx_thread_quiesce(m);
  memset(m->mem1, 0, gc_ram_size);
  memset(m->aram, 0, GC_ARAM_SIZE);
  gc_efb_clear(&m->efb);
  /* Everything except the buffers and the caller's settings. */
  memset((char *)m + offsetof(GcMachine, locked_cache), 0,
         sizeof(GcMachine) - offsetof(GcMachine, locked_cache));
  m->mem1 = mem1;
  m->aram = aram;
  m->efb = efb;
  m->gl = gl;
  m->log = log;
  m->trace_io = trace;

  gc_cpu_reset(&m->cpu);
  m->cpu.machine = wii;
  wii_machine_reset(m);
  gc_io_reset(m);
  gc_gx_reset(&m->gx);
  gc_clear_symbols();
  m->next_field_cycles = GC_CYCLES_PER_FIELD;
  gc_gx_thread_reload(m);
}

/* ------------------------------------------------------------------ */

/* Where the beam is.
 *
 * Two things here are easy to get wrong and neither shows up in the
 * obvious test. The register counts *pixels* across a line, not VI clocks,
 * and a pixel is two clocks - so the horizontal half runs 0..857 and not
 * 0..1715. And the vertical half counts across a whole **frame**, both
 * fields, 0..524 for NTSC; it does not restart every field.
 *
 * That second one is what tells the two fields apart. The SDK decides
 * which field is coming by asking whether the position is in the first
 * half of the frame or the second, so a counter that restarts every field
 * is always in the first half and the answer never changes. Nothing else
 * notices: the retrace still arrives at the right rate and the line number
 * still looks sensible, because the SDK's line conversion takes the
 * position modulo one field's worth and hides it. Only a game that renders
 * one field at a time finds out, by drawing every frame into the same half
 * of the screen.
 *
 * The arithmetic is exact, which is the sign the units are right: a frame
 * is 2 x 8,108,100 cycles, and 16,216,200 / 36 cycles a pixel = 450,450
 * pixels = 525 lines of 858 with nothing left over. */
int gc_vi_double = 0;

static void advance_video(GcMachine *m) {
  const uint64_t span = GC_VI_FRAME_CYCLES;
  const uint64_t frame = m->cpu.cycles / span;
  const uint64_t into_frame = m->cpu.cycles - frame * span;
  uint32_t pixel = (uint32_t)(into_frame / GC_VI_CYCLES_PER_PIXEL);
  const uint32_t previous = m->vi_pixel;
  /* Once per instruction count, however many times it is asked: see
   * advanced_cycles in gc.h. A machine fresh from reset counts as having
   * been brought up to count zero, which is where it is. */
  if (m->cpu.cycles == m->advanced_cycles) return;
  m->advanced_cycles = m->cpu.cycles;
  if (pixel >= GC_VI_PIXELS_PER_FRAME) pixel = GC_VI_PIXELS_PER_FRAME - 1u;
  m->vi_pixel = pixel;
  m->vi_line = pixel / GC_VI_PIXELS_PER_LINE;
  m->vi_column = pixel % GC_VI_PIXELS_PER_LINE;
  /* Every display interrupt the beam just passed. This is where the
   * retrace comes from, so it has to happen whether or not a field ended. */
  gc_vi_beam(m, previous, pixel);
  /* The sample counter reaches the moment a game asked about. */
  gc_ai_advance(m);
  gc_dsp_advance(m);
  /* And the engine that walks the mixed sound out to the converter. */
  gc_audio_advance(m);
  /* On a Wii, whatever the second processor owes by now. */
  if (m->cpu.machine) wii_machine_advance(m);
  if (m->cpu.cycles < m->next_field_cycles) return;

  m->field++;
  m->next_field_cycles = (m->field + 1) * GC_CYCLES_PER_FIELD;
  /* The interface polls the controller ports once a field. */
  gc_si_field_boundary(m);
  /* And a card that has stopped being written to is written back. */
  gc_card_advance(m);
  if (m->cpu.machine) wii_machine_field(m);
}

/* The decrementer counts down at the timebase rate and interrupts when it
 * goes negative. */
static int decrementer_due(GcMachine *m) {
  const uint64_t elapsed = (m->cpu.cycles - m->cpu.dec_write_cycles) / GC_CYCLES_PER_TB;
  return (m->cpu.dec - (uint32_t)elapsed) & 0x80000000u;
}

/* Everything that happens before an instruction runs: a hooked function
 * answered outright, and the two interrupts that can send the machine
 * somewhere else instead. Returns non-zero when the instruction at c->pc
 * must not run, because something already happened in its place.
 *
 * The interpreter calls this before each instruction. Recompiled code calls
 * it through gc_machine_due, below, only at the instructions where
 * something can be due - which delivers every interrupt at the same
 * instruction, and gc_timetest and the recompiled-test gate measure that. */
int gc_machine_before(GcMachine *m) {
  GcCpu *c = &m->cpu;

  /* A hooked function is answered here instead of being executed. */
  if (gc_os_hook(m)) return 1;

  if (c->msr & GC_MSR_EE) {
    if (m->pi_cause & m->pi_mask) {
      gc_cpu_exception(c, GC_VEC_EXTERNAL);
      c->pc = c->npc;
      return 1;
    }
    if (decrementer_due(m)) {
      /* Rearm before taking it, so that the handler does not re-enter on
       * the next instruction. */
      c->dec = 0x7fffffffu;
      c->dec_write_cycles = c->cycles;
      gc_cpu_exception(c, GC_VEC_DECREMENTER);
      c->pc = c->npc;
      return 1;
    }
  }
  return 0;
}

/* And after it: the instruction's own tail, then the beam moves. */
void gc_machine_after(GcMachine *m) {
  gc_cpu_retire(&m->cpu);
  advance_video(m);
}

/* ------------------------------------------------------------------ */
/* Doing the per-instruction work only when it is due                  */

/* The interpreter calls gc_machine_before and advance_video around every
 * instruction, and that is the reference. It is also most of what a
 * recompiled game used to spend its time on: a sampling profile of Shrek 2
 * put the beam, the clocks and the interrupt checks at nearly two thirds of
 * the run, against five per cent for the game's own code. Almost every one
 * of those calls finds nothing to do.
 *
 * So recompiled code asks a cheaper question first - has the instruction
 * count reached cpu.next_event? - and does the work only when it has. That
 * is observably the same as doing it every time because of three
 * properties, each of which the code below keeps:
 *
 *  1. advance_video is a function of the instruction count and the
 *     registers, and it does its work once per count however often it is
 *     asked (advanced_cycles). Calling it at the end of a run of
 *     instructions does what calling it after each would have, *provided*
 *     nothing in the run was due: the beam's crossings over one long
 *     interval are the union of the short ones, and the clocks are
 *     computed, not stepped.
 *  2. Whatever is due is found in advance. gc_machine_schedule() computes the
 *     earliest count at which anything could happen - the field ending, a
 *     display interrupt's target, the decrementer running out, the audio
 *     clock, the sound processor's mail, an audio block - and may answer too
 *     early but never too late.
 *  3. Anything that could move that moment clears next_event: every I/O
 *     access, a change to MSR or the decrementer, an interrupt line moving.
 *     An I/O access also brings the beam and the clocks up to date before it
 *     looks at them, because a register that reports the beam must report
 *     where the interpreter's beam would be. So does anything that hands
 *     the machine to the interpreter (the dispatcher, before a fallback
 *     step, and when it returns).
 *
 * Two mistakes were made on the way here and both were caught only by
 * comparing a whole game against the interpreter, not by the test
 * programs: bringing the beam up to date twice at the count where a frame
 * ends wrapped it one instruction early, and handing a machine with
 * something due to the interpreter without catching it up first delivered
 * that interrupt one instruction late. docs/GC-PLAN.md has the details.
 *
 * The exact cycle at which each interrupt is taken is what gc_timetest and
 * tools/gc/run_recompiled_tests.py hold this to: the instruction count at
 * the end of every program has to match the interpreter's, and it would not
 * if a single interrupt arrived one instruction late. */
void gc_machine_sync(GcMachine *m) { advance_video(m); }

uint64_t gc_io_next_event(GcMachine *m, uint64_t next);

static void gc_machine_schedule(GcMachine *m) {
  GcCpu *c = &m->cpu;
  const uint64_t now = c->cycles;
  uint64_t next = m->next_field_cycles;

  if (c->msr & GC_MSR_EE) {
    if (m->pi_cause & m->pi_mask) {
      next = now;
    } else {
      /* The decrementer interrupts when the 32-bit count, read as signed,
       * goes negative: from a value that is not yet negative that is
       * exactly `left + 1` ticks away. */
      const uint64_t elapsed = (now - c->dec_write_cycles) / GC_CYCLES_PER_TB;
      const uint32_t left = c->dec - (uint32_t)elapsed;
      if (left & 0x80000000u) {
        next = now;
      } else {
        const uint64_t at = c->dec_write_cycles + (elapsed + (uint64_t)left + 1u) * GC_CYCLES_PER_TB;
        if (at < next) next = at;
      }
    }
  }
  next = gc_io_next_event(m, next);
  next = gc_audio_next_event(m, next);
  if (c->machine) next = wii_machine_next_event(m, next);
  c->next_event = next;
}

/* Before an instruction whose count has reached next_event: everything the
 * interpreter would have done since the last time, then what it does
 * before this instruction, then the next moment to look again. Returns what
 * gc_machine_before returns. */
int gc_machine_due(GcMachine *m) {
  int answered;
  advance_video(m);
  answered = gc_machine_before(m);
  gc_machine_schedule(m);
  return answered;
}

uint64_t gc_idle_cycles;

/* Wait loops, counted rather than run (gc_machine_idle_until in gc.h).
 *
 * A game waiting for an interrupt handler to set a flag reads the flag,
 * compares it and branches back, millions of times a field; Shrek 2 spent
 * 28 % of a run doing that. The emitter (idle_loop_shape) marks the loops
 * where a second iteration provably repeats the first: every instruction is
 * a load, a compare, register arithmetic without carry, or the branch back,
 * which neither links nor counts; and nothing an iteration reads before
 * writing it is written by the iteration. Generated code then counts the
 * further iterations only when the one that just ran
 *
 *  - went back to the loop's first instruction,
 *  - loaded only through the shortcut to main memory, so no load touched a
 *    register block, the framebuffer, the page table or a missing address -
 *    each of which can do something (clear next_event, log, fault) that a
 *    load of memory does not,
 *  - and had every one of its instruction counts below cpu.next_event,
 *    unchanged since the loop started, so gc_machine_due was not called.
 *
 * Then, from here to next_event, nothing outside the processor runs:
 * that is property 2 above, and nothing in the loop can move the moment
 * (property 3 - no I/O, no MSR or decrementer change, no store). Memory is
 * therefore what the last iteration saw, so the next iteration computes the
 * same values into the same registers and takes the same branch, and so
 * does every one after it. An iteration starting at count k runs its
 * instructions at k .. k + count - 1 and finds nothing due if the last of
 * those is below next_event; every whole iteration for which that holds is
 * added to the count at once. The at most count - 1 instructions left
 * before next_event, and the one at it, run one at a time with their
 * checks, so whatever becomes due is delivered before the same instruction
 * it would have been. The field counter, the beam and the clocks are
 * computed from the count when they are next brought up to date, and do
 * not care how it got there.
 *
 * The interpreter does not do this and remains the reference. */

/* An attempt to boot that did not work leaves the core stopped and a
 * count of accesses that went nowhere. Whatever is tried next deserves a
 * machine that is not already broken. */
void gc_machine_reset_after_failed_boot(GcMachine *m) {
  m->cpu.stopped = GC_STOP_NONE;
  m->cpu.stop_insn = 0;
  m->cpu.pending_vector = 0;
  m->bad_access = 0;
  m->finished = 0;
}

void gc_machine_step(GcMachine *m) {
  if (m->profile && m->profile_every && (m->cpu.cycles % m->profile_every) == 0u) {
    const uint32_t at = m->cpu.pc & 0x03ffffffu;
    if (at < GC_MEM1_SIZE) m->profile[at >> 2]++;
  }
  if (m->stop_at && m->cpu.pc == m->stop_at && !m->cpu.stopped) {
    m->cpu.stopped = GC_STOP_BAD_ACCESS;
    return;
  }
  if (m->stop_after && m->cpu.cycles >= m->stop_after && !m->cpu.stopped) {
    m->cpu.stopped = GC_STOP_BAD_ACCESS;
    return;
  }
  if (gc_machine_before(m)) return;
  if (m->coverage) gc_coverage_note(m);
  gc_perf.interpreted++;
  gc_cpu_step(&m->cpu);
  advance_video(m);
}

void gc_machine_run(GcMachine *m, uint64_t until_field) {
  while (m->field < until_field && !m->finished && !m->cpu.stopped) {
    gc_machine_step(m);
  }
}
