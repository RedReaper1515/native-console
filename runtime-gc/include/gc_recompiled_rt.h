/*
 * What recompiled code is written against (gcrecomp's emitter, block by
 * block): the dispatcher's function type, the shortcut to main memory as
 * generated code uses it, and the few out-of-line helpers a block calls
 * when something leaves its fast path.
 *
 * A generated block keeps the guest registers it uses in C locals, checks
 * once on entry that nothing the machine schedules falls inside it, and
 * writes the registers back when it leaves. docs/GC-PLAN.md ("Registers in
 * locals") has the argument that this is observably the machine the
 * interpreter runs, instruction for instruction; the helpers below are
 * where the moments that argument depends on are handled:
 *
 *  - gc_rt_enter: an event falls inside the block. Whatever is due at its
 *    first instruction is done; if the rest then fits, the block runs, and
 *    otherwise the interpreter steps it to its end, exactly.
 *  - gc_rt_resync: an out-of-line call may have moved the next event (an
 *    I/O access clears it). Before the next instruction, the machine does
 *    what the per-instruction check would have done there.
 *  - gc_rt_load / gc_rt_store / gc_rt_psq: an access the shortcut does not
 *    cover, through the bus, then the fault check and the resync.
 *  - gc_rt_exit: a block that leaves early (an exception, an interrupt, an
 *    event inside it) has written its registers back; if it left because
 *    of an event inside, the interpreter finishes the block, so that the
 *    dispatcher sees the same block boundaries as always.
 */
#ifndef GC_RECOMPILED_RT_H
#define GC_RECOMPILED_RT_H

#include <string.h>

#include "gc.h"
#include "gc_bus.h"
#include "gc_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A block: runs, and answers the block that runs next when it knows it (a
 * direct branch, a fall-through), or null for the dispatcher to look up the
 * program counter. The pointer is a GcRecompiledFn, and the dispatcher calls
 * it straight away: whatever it would have done between the two blocks - a
 * field ending, the machine finishing or stopping - leaves next_event at or
 * below the count, so the next block's entry check sends it to gc_rt_enter,
 * which does it there. */
typedef void *(*GcRecompiledFn)(GcMachine *m);

#if defined(_MSC_VER)
#define GC_RT_COLD(x) (x)
#define GC_RT_NOINLINE __declspec(noinline)
#else
#define GC_RT_COLD(x) __builtin_expect(!!(x), 0)
#define GC_RT_NOINLINE __attribute__((noinline))
#endif

/* A map with nothing in it: every access through it takes the long way. */
extern const GcFastMap gc_rt_no_map;

/* The shortcut as gc_bus_fast_* take it (gc_bus.h), decided once where the
 * block starts and again after anything that could change MSR or the
 * reservation: problem state or a held reservation send stores (and, for
 * problem state, loads) the long way. */
#define GC_RT_LOAD_MAP(c) \
  (((c)->msr & GC_MSR_PR) ? &gc_rt_no_map : &(c)->fast[((c)->msr & GC_MSR_DR) ? 1 : 0])
#define GC_RT_STORE_MAP(c, load_map) ((c)->reserve ? &gc_rt_no_map : (load_map))
#define GC_RT_OK(map, ea, size) ((((ea) & 0xffffffu) + (size)) <= (map)->limit[(ea) >> 24])
#define GC_RT_AT(map, ea) ((map)->base[(ea) >> 24] + ((ea) & 0xffffffu))

#if defined(_MSC_VER)
#include <stdlib.h>
#define GC_RT_BSWAP16(x) _byteswap_ushort(x)
#define GC_RT_BSWAP32(x) _byteswap_ulong(x)
#define GC_RT_BSWAP64(x) _byteswap_uint64(x)
#else
#define GC_RT_BSWAP16(x) __builtin_bswap16(x)
#define GC_RT_BSWAP32(x) __builtin_bswap32(x)
#define GC_RT_BSWAP64(x) __builtin_bswap64(x)
#endif

static inline uint32_t gc_rt_get8(const uint8_t *p) { return p[0]; }
static inline uint32_t gc_rt_get16(const uint8_t *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return GC_RT_BSWAP16(v);
}
static inline uint32_t gc_rt_get32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return GC_RT_BSWAP32(v);
}
static inline uint64_t gc_rt_get64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return GC_RT_BSWAP64(v);
}
static inline void gc_rt_put8(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; }
static inline void gc_rt_put16(uint8_t *p, uint32_t v) {
  const uint16_t w = GC_RT_BSWAP16((uint16_t)v);
  memcpy(p, &w, 2);
}
static inline void gc_rt_put32(uint8_t *p, uint32_t v) {
  const uint32_t w = GC_RT_BSWAP32(v);
  memcpy(p, &w, 4);
}
static inline void gc_rt_put64(uint8_t *p, uint64_t v) {
  const uint64_t w = GC_RT_BSWAP64(v);
  memcpy(p, &w, 8);
}

/* A CR field's four bits from a compare, SO copied from XER. */
#define GC_RT_CMPS(a, b, xer) \
  ((((int32_t)(a) < (int32_t)(b)) ? 8u : (((int32_t)(a) > (int32_t)(b)) ? 4u : 2u)) | ((xer) >> 31))
#define GC_RT_CMPU(a, b, xer) \
  ((((uint32_t)(a) < (uint32_t)(b)) ? 8u : (((uint32_t)(a) > (uint32_t)(b)) ? 4u : 2u)) | ((xer) >> 31))
#define GC_RT_SETF(cr, field, bits) (((cr) & ~(0xf0000000u >> (4u * (field)))) | ((bits) << (28u - 4u * (field))))

static inline uint32_t gc_rt_clz32(uint32_t v) {
  uint32_t n = 0;
  if (!v) return 32u;
#if defined(_MSC_VER)
  {
    unsigned long index;
    _BitScanReverse(&index, v);
    n = 31u - (uint32_t)index;
  }
#else
  n = (uint32_t)__builtin_clz(v);
#endif
  return n;
}

static inline uint32_t gc_rt_rotl32(uint32_t v, uint32_t n) {
  n &= 31u;
  return n ? ((v << n) | (v >> (32u - n))) : v;
}


/* The field the dispatcher runs to: a block that finds the field reached, or
 * the machine finished or stopped, returns to it (gc_rt_enter). */
extern uint64_t gc_rt_until_field;
/* What the last gc_rt_load found, as after_access answers (0 go on, 1 an
 * exception, 2 leave after the instruction). A global rather than a pointer
 * argument, because a block with an address-taken local cannot tail-call. */
extern int gc_rt_status;

/* Where calls return to, as a processor's return-address stack keeps it: a
 * call (a branch with LK) pushes its return address and the block that starts
 * there, a return (bclr without LK) pops one and, when the address it pops is
 * where LR goes, answers that block as the next to run. A wrong guess costs a
 * lookup and nothing else: the address is compared, never assumed. */
typedef struct GcRtReturn {
  uint32_t pc;
  void *run;
} GcRtReturn;
#define GC_RT_RETURNS 64u
extern GcRtReturn gc_rt_returns[GC_RT_RETURNS];
extern unsigned gc_rt_return_top;
#define GC_RT_CALL(return_pc, block)                                  \
  do {                                                                \
    const unsigned gc_rt_t = (gc_rt_return_top + 1u) & (GC_RT_RETURNS - 1u); \
    gc_rt_return_top = gc_rt_t;                                       \
    gc_rt_returns[gc_rt_t].pc = (return_pc);                          \
    gc_rt_returns[gc_rt_t].run = (void *)(block);                     \
  } while (0)
static inline void *gc_rt_return(uint32_t target) {
  const unsigned t = gc_rt_return_top;
  gc_rt_return_top = (t - 1u) & (GC_RT_RETURNS - 1u);
  return gc_rt_returns[t].pc == target ? gc_rt_returns[t].run : 0;
}

/* The instruction's address, the block's instructions after it and the
 * access size, in one argument: pc | left << 32 | size << 56. */
#define GC_RT_WHERE(pc, left, size) ((uint64_t)(pc) | ((uint64_t)(left) << 32) | ((uint64_t)(size) << 56))

/* A floating-point register's two lanes as a block keeps them in a local:
 * an SSE register on x86-64, the register itself elsewhere. The paired
 * steady state's kernels (gc_sse_ps_binary, gc_sse_ps_madd in gc_fpu_impl.h)
 * work on them directly, and a block keeps the note that instruction leaves
 * (FPSCR, a little behind) in locals too, writing it to the processor before
 * anything that could read it. */
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
typedef __m128d GcRtLanes;
static inline GcRtLanes gc_rt_lanes_get(const GcFpr *p) {
  return _mm_castsi128_pd(_mm_loadu_si128((const __m128i *)(const void *)p));
}
static inline void gc_rt_lanes_put(GcFpr *p, GcRtLanes v) { _mm_storeu_si128((__m128i *)(void *)p, _mm_castpd_si128(v)); }
static inline uint64_t gc_rt_lanes_lo(GcRtLanes v) { return (uint64_t)_mm_cvtsi128_si64(_mm_castpd_si128(v)); }
static inline uint64_t gc_rt_lanes_hi(GcRtLanes v) {
  return (uint64_t)_mm_cvtsi128_si64(_mm_castpd_si128(_mm_unpackhi_pd(v, v)));
}
static inline GcRtLanes gc_rt_lanes_set(uint64_t lo, uint64_t hi) {
  return _mm_castsi128_pd(_mm_set_epi64x((long long)hi, (long long)lo));
}
static inline GcRtLanes gc_rt_lanes_dup_lo(GcRtLanes v) { return _mm_unpacklo_pd(v, v); }
static inline GcRtLanes gc_rt_lanes_dup_hi(GcRtLanes v) { return _mm_unpackhi_pd(v, v); }
#else
typedef GcFpr GcRtLanes;
static inline GcRtLanes gc_rt_lanes_get(const GcFpr *p) { return *p; }
static inline void gc_rt_lanes_put(GcFpr *p, GcRtLanes v) { *p = v; }
static inline uint64_t gc_rt_lanes_lo(GcRtLanes v) { return v.ps0; }
static inline uint64_t gc_rt_lanes_hi(GcRtLanes v) { return v.ps1; }
static inline GcRtLanes gc_rt_lanes_set(uint64_t lo, uint64_t hi) {
  GcRtLanes v;
  v.ps0 = lo;
  v.ps1 = hi;
  return v;
}
static inline GcRtLanes gc_rt_lanes_dup_lo(GcRtLanes v) { return gc_rt_lanes_set(v.ps0, v.ps0); }
static inline GcRtLanes gc_rt_lanes_dup_hi(GcRtLanes v) { return gc_rt_lanes_set(v.ps1, v.ps1); }
#endif

/* The same kernels out of line, for a block that would rather call one copy
 * than inline it (it costs the optimiser far less): the lanes go in and the
 * result comes back in SSE registers (__vectorcall under MSVC; the x86-64
 * System V convention already does), and a declined instruction answers
 * lanes of all ones - a NaN, which no accepted result is. `kind` is the
 * GcFpKind for gc_rt_ps_binary_call and negate_add * 2 + negate_result for
 * gc_rt_ps_madd_call. */
#if defined(_MSC_VER) && defined(_M_X64)
#define GC_RT_VECTORCALL __vectorcall
#else
#define GC_RT_VECTORCALL
#endif
GcRtLanes GC_RT_VECTORCALL gc_rt_ps_binary_call(GcRtLanes a, GcRtLanes b, unsigned kind);
GcRtLanes GC_RT_VECTORCALL gc_rt_ps_madd_call(GcRtLanes a, GcRtLanes b, GcRtLanes cc, unsigned negations);
#define GC_RT_ACCEPTED(lanes) (gc_rt_lanes_lo(lanes) != ~(uint64_t)0)

/* The steady state a paired instruction without Rc needs for its note
 * (gc_fp_lazy_ok and gc_fp_fast_mode in gc_fpu_impl.h), asked once where a
 * block starts and again after anything out of line: nothing inside a block
 * changes FPSCR's mode bits, MSR or the host's rounding without a call. */
#define GC_RT_STEADY(c)                                                                                     \
  (((c)->fpscr & (GC_FP_LAZY_STEADY_MASK | GC_FPSCR_RN)) == (GC_FPSCR_FX | GC_FPSCR_XX) &&                   \
   ((c)->msr & (GC_MSR_FE0 | GC_MSR_FE1)) == 0 && gc_host_nearest())

/* Out of line, and each one's argument is in gc_recompiled_rt.c. */
void *gc_rt_enter(GcMachine *m, uint32_t start, uint32_t count, uint32_t first, GcRecompiledFn self);
void *gc_rt_enter_stepped(GcMachine *m, uint32_t start, uint32_t first);
void *gc_rt_exit(GcMachine *m, uint32_t end);
int gc_rt_resync(GcMachine *m, uint32_t pc, uint64_t cycles, uint32_t left);
uint64_t gc_rt_load(GcMachine *m, uint32_t ea, uint64_t cycles, uint64_t where);
int gc_rt_store(GcMachine *m, uint32_t ea, uint64_t value, uint64_t cycles, uint64_t where);
int gc_rt_psq(GcMachine *m, uint32_t insn, uint32_t ea, int store, uint32_t pc, uint64_t cycles, uint32_t left);
/* An instruction the emitter hands to its interpreter handler, out of line
 * (the handler runs through the interpreter's own step function): 0 to go
 * on, otherwise leave. */
int gc_rt_insn(GcMachine *m, uint32_t insn, uint32_t pc, uint64_t cycles, uint32_t left);

/* The floating-point handlers that carry the IEEE flag model, as one shared
 * copy each rather than inlined at every instruction (which is what made a
 * generated file slow to compile): gc_rt_<handler>(c, insn). They touch the
 * floating-point registers, FPSCR and CR and nothing else. */
#define GC_RT_FP_CALLS(X)                                                                          \
  X(gc_op_ps_add) X(gc_op_ps_sub) X(gc_op_ps_mul) X(gc_op_ps_div) X(gc_op_ps_madd) X(gc_op_ps_madds0) \
  X(gc_op_ps_madds1) X(gc_op_ps_msub) X(gc_op_ps_nmsub) X(gc_op_ps_nmadd) X(gc_op_ps_sum0)          \
  X(gc_op_ps_sum1) X(gc_op_ps_muls0) X(gc_op_ps_muls1) X(gc_op_ps_res) X(gc_op_ps_rsqrte)            \
  X(gc_op_fadd) X(gc_op_fadds) X(gc_op_fsub) X(gc_op_fsubs) X(gc_op_fmul) X(gc_op_fmuls)             \
  X(gc_op_fdiv) X(gc_op_fdivs) X(gc_op_fmadd) X(gc_op_fmadds) X(gc_op_fmsub) X(gc_op_fmsubs)         \
  X(gc_op_fnmadd) X(gc_op_fnmadds) X(gc_op_fnmsub) X(gc_op_fnmsubs) X(gc_op_fres) X(gc_op_frsqrte)   \
  X(gc_op_fcmpu) X(gc_op_fcmpo) X(gc_op_frsp) X(gc_op_fctiw) X(gc_op_fctiwz) X(gc_op_ps_cmpu0)       \
  X(gc_op_ps_cmpo0) X(gc_op_ps_cmpu1) X(gc_op_ps_cmpo1) X(gc_op_mtfsf) X(gc_op_mtfsfi)               \
  X(gc_op_mtfsb0) X(gc_op_mtfsb1) X(gc_op_mcrfs) X(gc_op_mffs)
#define GC_RT_FP_DECLARE(handler) void gc_rt_##handler(GcCpu *c, uint32_t insn);
GC_RT_FP_CALLS(GC_RT_FP_DECLARE)
#undef GC_RT_FP_DECLARE
int gc_rt_psq_store_fast(GcCpu *c, uint32_t insn, uint32_t ea, unsigned gqr_index, unsigned w);

/* The paired steady state's kernels on lanes, for a block that keeps them in
 * locals: gc_sse_ps_binary and gc_sse_ps_madd on x86-64, the scalar kernels
 * lane by lane elsewhere. 1 with the result when both lanes are accepted.
 * Only where the floating-point handlers are included, as in a generated
 * block file. */
#ifdef GC_FPU_IMPL_H
static inline int gc_rt_ps_binary(GcRtLanes a, GcRtLanes b, GcFpKind kind, GcRtLanes *out) {
#if defined(_M_X64) || defined(__x86_64__)
  return gc_sse_ps_binary(_mm_castpd_si128(a), _mm_castpd_si128(b), kind, out);
#else
  uint64_t r0, r1;
  int u0, u1;
  if (kind == GC_FP_MUL) {
    if (!gc_fp_fast_mul(a.ps0, b.ps0, &r0, &u0) || !gc_fp_fast_mul(a.ps1, b.ps1, &r1, &u1)) return 0;
  } else {
    if (!gc_fp_fast_add(a.ps0, b.ps0, kind == GC_FP_SUB, &r0, &u0) ||
        !gc_fp_fast_add(a.ps1, b.ps1, kind == GC_FP_SUB, &r1, &u1)) {
      return 0;
    }
  }
  *out = gc_rt_lanes_set(r0, r1);
  return 1;
#endif
}

static inline int gc_rt_ps_madd(GcRtLanes a, GcRtLanes b, GcRtLanes cc, int negate_add, int negate_result,
                                GcRtLanes *out) {
#if defined(_M_X64) || defined(__x86_64__)
  return gc_sse_ps_madd(_mm_castpd_si128(a), _mm_castpd_si128(b), _mm_castpd_si128(cc), negate_add, negate_result, out);
#else
  uint64_t r0, r1;
  int u0, u1;
  if (!gc_fp_fast_madd(a.ps0, b.ps0, cc.ps0, negate_add, negate_result, &r0, &u0) ||
      !gc_fp_fast_madd(a.ps1, b.ps1, cc.ps1, negate_add, negate_result, &r1, &u1)) {
    return 0;
  }
  *out = gc_rt_lanes_set(r0, r1);
  return 1;
#endif
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* GC_RECOMPILED_RT_H */
