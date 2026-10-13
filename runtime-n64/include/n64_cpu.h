/*
 * The VR4300 (the N64's MIPS III CPU): its state, and the functions the
 * interpreter, the generated code and the machine share.
 *
 * Guest state is explicit (docs/N64-PLAN.md, ground rules): the 32
 * general registers, HI and LO, the program counter and the delay-slot
 * state, coprocessor 0 (system control, the TLB, Count and Compare) and
 * the floating-point unit all live here. Every memory access goes through
 * n64_cpu_impl.h, which translates the address here and hands physical
 * accesses to the n64_bus_* functions that the final program provides
 * (n64_bus.h). Nothing else in the struct points at host memory except the
 * `links` member, which save states scrub.
 *
 * Time is counted in pipeline cycles (93.75 MHz). Count advances once
 * every two, as on the hardware.
 */
#ifndef N64_CPU_H
#define N64_CPU_H

#include "n64_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N64_CPU_HZ 93750000u

/* Exception codes (Cause bits 2-6). */
enum {
    N64_EXC_INT = 0,
    N64_EXC_MOD = 1,
    N64_EXC_TLBL = 2,
    N64_EXC_TLBS = 3,
    N64_EXC_ADEL = 4,
    N64_EXC_ADES = 5,
    N64_EXC_IBE = 6,
    N64_EXC_DBE = 7,
    N64_EXC_SYS = 8,
    N64_EXC_BP = 9,
    N64_EXC_RI = 10,
    N64_EXC_CPU = 11,
    N64_EXC_OV = 12,
    N64_EXC_TR = 13,
    N64_EXC_FPE = 15,
    N64_EXC_WATCH = 23,
};

/* Status register bits. */
#define N64_SR_IE 0x00000001u
#define N64_SR_EXL 0x00000002u
#define N64_SR_ERL 0x00000004u
#define N64_SR_KSU 0x00000018u
#define N64_SR_UX 0x00000020u
#define N64_SR_SX 0x00000040u
#define N64_SR_KX 0x00000080u
#define N64_SR_IM 0x0000FF00u
#define N64_SR_BEV 0x00400000u
#define N64_SR_RE 0x02000000u
#define N64_SR_FR 0x04000000u
#define N64_SR_CU0 0x10000000u
#define N64_SR_CU1 0x20000000u
#define N64_SR_CU2 0x40000000u
/* Bits 19, 21 (TS) and 23 cannot be written. */
#define N64_SR_WRITABLE 0xFF57FFFFu

/* Cause register bits. */
#define N64_CAUSE_BD 0x80000000u
#define N64_CAUSE_IP_MI 0x00000400u    /* IP2: the RCP's MI interrupt line */
#define N64_CAUSE_IP_TIMER 0x00008000u /* IP7: Count reached Compare */

/* FCR31. */
#define N64_FCR31_WRITABLE 0x0183FFFFu
#define N64_FCR31_C 0x00800000u
#define N64_FCR31_FS 0x01000000u

typedef struct N64TlbEntry {
    /* As written by TLBWI/TLBWR (what TLBR gives back). */
    uint32_t page_mask;
    uint64_t entry_hi; /* VPN2 (masked to the page size), region, ASID */
    uint64_t entry_lo[2];
    /* Derived when written. */
    uint64_t vpn;       /* bits 13-39 of the virtual address this entry covers */
    uint64_t vmask;     /* which of bits 13-39 must match */
    uint32_t odd_bit;   /* the bit that selects entry_lo[1] */
    uint32_t page_off;  /* offset mask within one page */
    uint32_t pfn[2];    /* physical page base */
    uint8_t region;
    uint8_t asid;
    uint8_t global;
    uint8_t valid[2];
    uint8_t dirty[2];
} N64TlbEntry;

/* Host links: the only host pointers in the CPU. */
typedef struct N64CpuLinks {
    uint8_t *rdram;      /* big-endian RDRAM, for the fast path */
    uint32_t rdram_size;
    /* One byte per 64-byte granule of RDRAM: non-zero where recompiled code
     * was compiled from, so a store there must tell the dispatcher
     * (n64_bus_code_written). NULL when nothing is compiled. */
    const uint8_t *code_map;
    void *machine;
} N64CpuLinks;

#define N64_CODE_GRANULE_SHIFT 6u

typedef struct N64Cpu {
    uint64_t gpr[32];
    uint64_t hi, lo;

    /* The instruction about to run. */
    uint64_t pc;
    /* Set by an instruction while it runs: the address of the one after it
     * (the interpreter's sequencing; ERET writes it too). */
    uint64_t next_pc;
    /* The instruction at pc is the delay slot of a branch: after it,
     * execution continues at delay_target. */
    uint8_t delay_pending;
    /* The instruction running now is a delay slot (for EPC and Cause.BD). */
    uint8_t in_delay;
    /* An exception was raised by the instruction running now: pc already
     * holds the vector. Cleared by whoever ran the instruction. */
    uint8_t exc;
    uint8_t llbit;
    uint64_t delay_target;

    /* Coprocessor 0. */
    uint32_t cp0_index;
    uint32_t cp0_wired;
    uint32_t cp0_page_mask;
    uint32_t cp0_status;
    uint32_t cp0_cause;
    uint32_t cp0_compare; /* the 32-bit register value */
    uint32_t cp0_config;
    uint32_t cp0_watch_lo, cp0_watch_hi;
    uint32_t cp0_tag_lo;
    uint32_t cp0_parity;
    uint64_t cp0_entry_lo[2];
    uint64_t cp0_context;
    uint64_t cp0_bad_vaddr;
    uint64_t cp0_entry_hi;
    uint64_t cp0_epc;
    uint64_t cp0_ll_addr;
    uint64_t cp0_xcontext;
    uint64_t cp0_error_epc;
    uint64_t cp0_latch; /* what reading an unimplemented register gives */
    /* Count is (count2 + cycles) >> 1 (low 32 bits); see n64_cpu_count. */
    uint64_t count2;
    /* Random counts down once per instruction from 31 to Wired. */
    uint64_t random_base;
    N64TlbEntry tlb[32];

    /* Derived from Status: 1 in kernel mode (KSU = 0, or EXL or ERL set). */
    uint8_t kernel;
    /* Derived from Status: 64-bit addressing for the current mode. */
    uint8_t addr64;
    /* Reverse-endian user mode is not modelled; see N64-PLAN.md. */
    uint8_t pad_mode[2];

    /* Floating-point unit: 32 64-bit registers (Status.FR selects how the
     * 32-bit views map onto them, see n64_fpu_impl.h) and FCR31. */
    uint64_t fpr[32];
    uint32_t fcr31;

    /* The CPU's COP2 is absent; its moves read and write one latch. */
    uint64_t cop2_latch;

    /* Time and counts. */
    uint64_t cycles;       /* pipeline cycles since power-on */
    uint64_t next_sync;    /* the run loop wants control at this cycle (n64_rc_sync) */
    uint64_t instructions; /* retired instructions */
    uint64_t interpreted;  /* of those, how many the interpreter ran */
    uint64_t idled;        /* of those, how many were idle-loop turns taken at once (n64_rc_idle) */

    N64CpuLinks links;
} N64Cpu;

/* Power-on state (cold reset): what the hardware has before the PIF boot
 * code runs. n64_boot.c then puts the CPU into the state the boot code
 * leaves. */
void n64_cpu_reset(N64Cpu *c);

/* Run one instruction with the interpreter (an exception raised by it, or
 * a pending interrupt, is taken as on the hardware). Returns true when the
 * instruction ends a sync point (n64_cpu_decode.h): a load or store, a
 * system control instruction, the end of a branch, or an exception. The
 * run loop checks for due events only there, and generated code does the
 * same, so both see events at the same instructions. */
bool n64_cpu_step(N64Cpu *c);

/* Raise exception `code` for the instruction at c->pc. ce is the
 * coprocessor number for N64_EXC_CPU; tlb_miss selects the TLB refill
 * vector. Sets c->exc. */
void n64_cpu_exception(N64Cpu *c, uint32_t code, uint32_t ce, bool tlb_miss);

/* Set or clear an interrupt line (N64_CAUSE_IP_*). */
void n64_cpu_set_irq(N64Cpu *c, uint32_t ip_bit, bool level);

/* Take a pending, enabled interrupt now, between instructions. Returns
 * true when one was taken (c->pc is then the vector). */
bool n64_cpu_poll_interrupt(N64Cpu *c);

static inline uint32_t n64_cpu_count(const N64Cpu *c) { return (uint32_t)((c->count2 + c->cycles) >> 1); }

/* The cycle at which Count next equals Compare. */
uint64_t n64_cpu_timer_due(const N64Cpu *c);

/* Coprocessor 0 register access (MFC0/MTC0 semantics). */
uint64_t n64_cpu_cop0_read(N64Cpu *c, unsigned reg);
void n64_cpu_cop0_write(N64Cpu *c, unsigned reg, uint64_t value);

/* Recompute kernel/addr64 after a Status write. */
void n64_cpu_update_mode(N64Cpu *c);

/* Virtual to physical translation for an access of `size` bytes
 * (alignment is checked by the caller). On failure raises the exception
 * (address error or TLB) and returns false. store selects the store
 * exception codes and the dirty check. */
bool n64_cpu_translate(N64Cpu *c, uint64_t vaddr, bool store, uint32_t *paddr);

/* Translation for a debugger or the dispatcher: no exceptions, no state
 * change. */
bool n64_cpu_translate_quiet(const N64Cpu *c, uint64_t vaddr, uint32_t *paddr);

/* Instruction fetch at c->pc: translates and reads; raises the fetch
 * exception and returns false on failure. */
bool n64_cpu_fetch(N64Cpu *c, uint32_t *insn);

/* TLB instructions. */
void n64_cpu_tlbr(N64Cpu *c);
void n64_cpu_tlbwi(N64Cpu *c);
void n64_cpu_tlbwr(N64Cpu *c);
void n64_cpu_tlbp(N64Cpu *c);
void n64_cpu_eret(N64Cpu *c);

#ifdef __cplusplus
}
#endif

#endif /* N64_CPU_H */
