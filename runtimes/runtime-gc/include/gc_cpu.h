/*
 * Gekko (PowerPC 750CXe) state.
 *
 * Everything the guest can observe lives here, in the architectural layout,
 * so that a state dump is comparable between the interpreter and recompiled
 * code and between an MSVC build and a gcc build (ADDING-A-CONSOLE.md, P7).
 *
 * Two representation decisions are load-bearing:
 *
 *  - **Floating-point registers hold raw doubles, not host doubles.** A
 *    Gekko FPR is 64 bits wide and always holds an IEEE-754 double bit
 *    pattern; a single-precision value is the double that represents it.
 *    Storing the bits rather than a `double` keeps NaN payloads, signalling
 *    NaNs and denormals exactly as the guest left them, which is what
 *    `stfd` after `lfs`, `ps_merge` and `hwtests/cputest/nan.cpp` see.
 *
 *  - **Guest memory is big-endian and the bus does the swapping.** No guest
 *    value is held in a C local across a bus access, so a structure the game
 *    writes as words and reads as bytes behaves as it does on hardware.
 *
 * The second half of each FPR (`ps1`) is real architectural state: paired
 * single instructions read and write it, `lfs` fills both halves, and
 * `ps_merge` moves it around. It is not scratch.
 */
#ifndef GC_CPU_H
#define GC_CPU_H

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "gc_cpu_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A floating-point register: two IEEE-754 double bit patterns. In scalar
 * use only ps0 matters; paired-single instructions use both lanes, each
 * holding a single-precision value in double format. */
typedef struct GcFpr {
  uint64_t ps0;
  uint64_t ps1;
} GcFpr;

/* XER, in architectural bit order (bit 0 is the most significant). */
#define GC_XER_SO 0x80000000u
#define GC_XER_OV 0x40000000u
#define GC_XER_CA 0x20000000u
#define GC_XER_BYTES 0x0000007fu /* string-instruction byte count */
/* The bits XER actually has. A write drops everything else, which the
 * vectors show and hwtests/cputest/mtspr.cpp will confirm on hardware
 * (docs/GC-PLAN.md, "Behaviour pinned by the vectors"). */
#define GC_XER_WRITABLE 0xe000ff7fu

/* MSR bits this implementation observes. */
#define GC_MSR_POW 0x00040000u /* power management */
#define GC_MSR_ILE 0x00010000u /* interrupt little-endian */
#define GC_MSR_EE 0x00008000u  /* external interrupt enable */
#define GC_MSR_PR 0x00004000u  /* problem state (user mode) */
#define GC_MSR_FP 0x00002000u  /* floating-point available */
#define GC_MSR_ME 0x00001000u  /* machine check enable */
#define GC_MSR_FE0 0x00000800u
#define GC_MSR_SE 0x00000400u /* single step */
#define GC_MSR_BE 0x00000200u /* branch trace */
#define GC_MSR_FE1 0x00000100u
#define GC_MSR_IP 0x00000040u /* exception prefix */
#define GC_MSR_IR 0x00000020u /* instruction address translation */
#define GC_MSR_DR 0x00000010u /* data address translation */
#define GC_MSR_PM 0x00000004u
#define GC_MSR_RI 0x00000002u /* recoverable interrupt */
#define GC_MSR_LE 0x00000001u /* little-endian mode */

/* FPSCR bits, architectural numbering (bit 0 most significant). */
#define GC_FPSCR_FX 0x80000000u
#define GC_FPSCR_FEX 0x40000000u
#define GC_FPSCR_VX 0x20000000u
#define GC_FPSCR_OX 0x10000000u
#define GC_FPSCR_UX 0x08000000u
#define GC_FPSCR_ZX 0x04000000u
#define GC_FPSCR_XX 0x02000000u
#define GC_FPSCR_VXSNAN 0x01000000u
#define GC_FPSCR_VXISI 0x00800000u
#define GC_FPSCR_VXIDI 0x00400000u
#define GC_FPSCR_VXZDZ 0x00200000u
#define GC_FPSCR_VXIMZ 0x00100000u
#define GC_FPSCR_VXVC 0x00080000u
#define GC_FPSCR_FR 0x00040000u /* fraction rounded */
#define GC_FPSCR_FI 0x00020000u /* fraction inexact */
#define GC_FPSCR_FPRF 0x0001f000u
#define GC_FPSCR_VXSOFT 0x00000400u
#define GC_FPSCR_VXSQRT 0x00000200u
#define GC_FPSCR_VXCVI 0x00000100u
#define GC_FPSCR_VE 0x00000080u
#define GC_FPSCR_OE 0x00000040u
#define GC_FPSCR_UE 0x00000020u
#define GC_FPSCR_ZE 0x00000010u
#define GC_FPSCR_XE 0x00000008u
#define GC_FPSCR_NI 0x00000004u /* non-IEEE mode */
#define GC_FPSCR_RN 0x00000003u /* rounding mode */
/* Bit 20 of FPSCR is reserved and reads back as zero however it is written;
 * FX aside, the two summary bits are derived rather than stored. The
 * vectors show all three. */
#define GC_FPSCR_RESERVED 0x00000800u

/* The Gekko-only bits of HID2 (SPR 920). */
#define GC_HID2_LSQE 0x80000000u /* quantised load/store enabled */
#define GC_HID2_WPE 0x40000000u  /* write-gather pipe enabled */
#define GC_HID2_PSE 0x20000000u  /* paired singles enabled */
#define GC_HID2_LCE 0x10000000u  /* locked cache enabled */

/* Exception vectors (offsets from the vector base, 0x00000000 or
 * 0xfff00000 depending on MSR[IP]). */
#define GC_VEC_RESET 0x00000100u
#define GC_VEC_MACHINE_CHECK 0x00000200u
#define GC_VEC_DSI 0x00000300u
#define GC_VEC_ISI 0x00000400u
#define GC_VEC_EXTERNAL 0x00000500u
#define GC_VEC_ALIGNMENT 0x00000600u
#define GC_VEC_PROGRAM 0x00000700u
#define GC_VEC_FP_UNAVAILABLE 0x00000800u
#define GC_VEC_DECREMENTER 0x00000900u
#define GC_VEC_SYSCALL 0x00000c00u
#define GC_VEC_TRACE 0x00000d00u
#define GC_VEC_PERF_MONITOR 0x00000f00u

/* SPR numbers the core implements directly. Anything else goes through the
 * machine's SPR hook. */
enum {
  GC_SPR_XER = 1,
  GC_SPR_LR = 8,
  GC_SPR_CTR = 9,
  GC_SPR_DSISR = 18,
  GC_SPR_DAR = 19,
  GC_SPR_DEC = 22,
  GC_SPR_SDR1 = 25,
  GC_SPR_SRR0 = 26,
  GC_SPR_SRR1 = 27,
  GC_SPR_SPRG0 = 272,
  GC_SPR_EAR = 282,
  GC_SPR_TBL_W = 284,
  GC_SPR_TBU_W = 285,
  GC_SPR_IBAT0U = 528,
  GC_SPR_DBAT0U = 536,
  GC_SPR_GQR0 = 912,
  GC_SPR_HID2 = 920,
  GC_SPR_WPAR = 921,
  GC_SPR_DMAU = 922,
  GC_SPR_DMAL = 923,
  GC_SPR_HID0 = 1008,
  GC_SPR_HID1 = 1009,
  GC_SPR_IABR = 1010,
  GC_SPR_DABR = 1013,
  GC_SPR_L2CR = 1017,
  GC_SPR_ICTC = 1019,
  GC_SPR_THRM1 = 1020,
  GC_SPR_PVR = 287,
  GC_SPR_TBL_R = 268,
  GC_SPR_TBU_R = 269
};

/* Why the interpreter stopped stepping. */
typedef enum GcStop {
  GC_STOP_NONE = 0,
  GC_STOP_ILLEGAL,   /* an opcode this CPU does not have */
  GC_STOP_UNHANDLED, /* a real opcode this core does not implement yet */
  GC_STOP_HALT,      /* the machine asked the core to stop */
  /* An address that is not memory, when the machine was told to stop
   * at the first one rather than count them. */
  GC_STOP_BAD_ACCESS
} GcStop;

/* Where main memory sits in the host's address space for each 16 MB of the
 * guest's, when every address in that piece translates to main memory the
 * same way: `base[ea >> 24] + (ea & 0xffffff)` is the byte, for offsets
 * below `limit`. A limit of zero means "ask the bus". The bus fills this in
 * (gc_bus.c) and it is never guest state; see gc_bus.h for how loads and
 * stores use it. */
typedef struct GcFastMap {
  uint8_t *base[256];
  uint32_t limit[256];
} GcFastMap;

typedef struct GcCpu {
  uint32_t gpr[32];
  GcFpr fpr[32];

  uint32_t pc;  /* the instruction being executed */
  uint32_t npc; /* where execution goes next */

  uint32_t cr;  /* CR0 in bits 31..28, as the architecture numbers it */
  uint32_t xer;
  uint32_t lr;
  uint32_t ctr;
  uint32_t msr;
  uint32_t fpscr; /* can be behind: gc_fp_settle (gc_fpu_impl.h) */

  uint32_t gqr[8];
  uint32_t hid0;
  uint32_t hid1;
  uint32_t hid2;
  uint32_t wpar;
  uint32_t dmau;
  uint32_t dmal;
  uint32_t l2cr;
  uint32_t sdr1;
  uint32_t ear;
  uint32_t pvr;

  uint32_t srr0;
  uint32_t srr1;
  uint32_t dar;
  uint32_t dsisr;
  uint32_t sprg[4];
  uint32_t sr[16];      /* segment registers */
  uint32_t ibat[8];     /* IBAT0U, IBAT0L, IBAT1U, ... */
  uint32_t dbat[8];

  /* lwarx/stwcx. reservation. */
  uint32_t reserve_addr;
  uint8_t reserve;

  /* Timing. The timebase and the decrementer both tick at the bus clock
   * divided by four; `cycles` counts CPU clocks and is the single source
   * both are derived from. Per-instruction costs are a placeholder until
   * P8 measures them. */
  uint64_t cycles;
  uint64_t tb_base;  /* timebase value at cycles == tb_base_cycles */
  uint64_t tb_base_cycles;
  uint32_t dec;
  uint64_t dec_write_cycles;

  /* The earliest value of `cycles` at which the machine around the core
   * may have something to do: a display interrupt the beam is about to
   * reach, the decrementer running out, a field ending, an interrupt
   * already waiting. Not architectural state - it is a promise the machine
   * makes to recompiled code that nothing between here and then needs
   * checking, which is what lets that code skip the per-instruction work.
   * Anything that could break the promise (an I/O access, a change to MSR
   * or the decrementer, an interrupt line moving) sets it to zero, which
   * means "check before the next instruction". The interpreter does not
   * read it; see gc_machine_due() in gc.c. */
  uint64_t next_event;

  /* Set by a handler that changed the flow of control; recompiled code
   * checks it to decide whether to fall through to the next block. */
  uint8_t branched;

  /* Non-zero once the core has stopped; see GcStop. */
  uint8_t stopped;
  uint32_t stop_insn;

  /* What FPSCR has not been told yet, when fp_lazy is not zero: see "FPSCR,
   * a little behind" in gc_fpu_impl.h. */
  uint8_t fp_lazy;
  uint64_t fp_lazy_a, fp_lazy_b, fp_lazy_c, fp_lazy_result;

  /* Pending exception vector, taken at the next instruction boundary. */
  uint32_t pending_vector;

  /* The machine behind the bus (opaque to the core). */
  void *machine;

  /* The bus's shortcut to main memory, one map with data translation off
   * and one with it on, and whether they describe the current BATs. A bus
   * that never fills them in (the vector harness) leaves every limit zero,
   * and every access goes the long way. */
  GcFastMap fast[2];
  uint8_t fast_ready;
} GcCpu;

/* Forget the shortcut to main memory: every access asks the bus again, and
 * the bus rebuilds it. Anything that changes how data addresses translate
 * calls this - a DBAT or HID2 write, a reset, the loader's BATs. */
static inline void gc_cpu_forget_fast(GcCpu *c) {
  memset(c->fast, 0, sizeof(c->fast));
  c->fast_ready = 0;
}

/* Cycles per timebase tick: the CPU runs at 486 MHz, the bus at 162 MHz,
 * and the timebase at the bus clock divided by four. */
#define GC_CYCLES_PER_TB 12

void gc_cpu_reset(GcCpu *c);

/* FPSCR as the guest would read it, without changing the machine (the
 * register itself can be a note behind: gc_fpu_impl.h). */
uint32_t gc_cpu_fpscr(const GcCpu *c);

/* Execute one instruction. Returns the number of CPU cycles charged. */
uint32_t gc_cpu_step(GcCpu *c);

/* Run until `until_cycles`, an exception the machine handles, or a stop. */
void gc_cpu_run(GcCpu *c, uint64_t until_cycles);

/* The tail of one instruction: an exception it raised, the program counter,
 * and the cycle it cost. The interpreter calls this at the end of
 * gc_cpu_step; recompiled code calls it in the same place, so the two
 * cannot disagree about what retiring an instruction means. */
void gc_cpu_retire(GcCpu *c);

/* Take an exception now: save SRR0/SRR1 and jump to the vector. */
void gc_cpu_exception(GcCpu *c, uint32_t vector);

/* The current timebase value. */
uint64_t gc_cpu_timebase(const GcCpu *c);
void gc_cpu_set_timebase(GcCpu *c, uint64_t value);

/* Disassemble one instruction into `buffer`. Returns `buffer`. */
const char *gc_disasm(uint32_t insn, uint32_t pc, char *buffer, size_t size);

/* Where NaNs are born. An invalid operation - infinity less infinity,
 * nothing times infinity, nothing over nothing, a square root of less than
 * nothing - gives a NaN from operands that were not, and a game whose scene
 * has vanished into NaNs made the first of them in one of these. When
 * gc_fp_invalid_trace is set (the runner's --trace-invalid FIELD), each is
 * reported to the machine's log from that field on: the field, which kind,
 * the program counter and the link register. Recompiled code reports the
 * same; its program counter is the block's unless the instruction is one
 * that can fault. So is each store of a default NaN to memory (gc_bus.c). */
extern int gc_fp_invalid_trace;
extern uint64_t gc_fp_invalid_from;
void gc_fp_invalid_report(GcCpu *c, uint32_t which);

#ifdef __cplusplus
}
#endif

#endif /* GC_CPU_H */
