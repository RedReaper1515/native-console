/*
 * One basic block as C, with the guest registers it uses in C locals.
 *
 * The first emitter called the interpreter's handler for every instruction
 * and wrapped each call in the machine's per-instruction work. That kept one
 * implementation of every instruction, and it cost: every register a C load
 * and store through the CPU structure, which the compiler reloads after
 * every store to guest memory (a store through a byte pointer may alias
 * anything); the count, the program counter and the next address stored
 * after every instruction; the event check before every one. A sampling
 * profile of Shrek 2's swamp put a third of the time inside generated blocks
 * in that glue (docs/GC-PLAN.md, "Registers in locals").
 *
 * What a block looks like now:
 *
 *  - **Entry.** One check that nothing the machine schedules falls on any of
 *    the block's instructions (cycles + count <= next_event); otherwise
 *    gc_rt_enter does what the first instruction's check would have done
 *    and, when an event still falls inside, the interpreter steps the block
 *    exactly. Then the registers the block reads before writing are loaded
 *    into locals.
 *  - **The instructions.** The common integer, branch, load and store
 *    instructions are written out against the locals, with the same
 *    arithmetic as their handlers in gc_cpu_impl.h (instruction for
 *    instruction: the gc_recompiled_fuzz test holds every written-out form
 *    to its handler on random states). Loads and stores go straight through
 *    the shortcut to main memory and otherwise call gc_rt_load/gc_rt_store.
 *    Floating-point registers stay in the CPU structure: the paired and
 *    scalar arithmetic calls its handler (inline for the cheap ones, one
 *    shared copy in the runtime for the heavy ones), which touches no general
 *    register. Anything else is handed to its handler out of line
 *    (gc_rt_insn), with the locals written back before and read again after.
 *  - **Moments.** Anything out of line that can move the next event (an I/O
 *    access clears it) is followed by the check the next instruction would
 *    have made (gc_rt_resync), so an interrupt is still taken before the
 *    same instruction.
 *  - **Exits.** The normal end writes back what was written and returns the
 *    next block when it is known, so the dispatcher does not look it up. An
 *    early exit (an exception, an interrupt, an event inside) jumps to a
 *    label that writes back exactly the registers written before that
 *    instruction: the labels are chained from the last instruction down, so
 *    each register is stored once, at the label of its first write.
 *
 * Parts. The optimiser's time grows much faster than the function it is
 * given (docs/GC-PLAN.md, "How long a game takes to build"), so a block whose
 * estimated cost passes BlockOptions::part_cost continues in another
 * function: the registers are written back, the count stored, and the next
 * part called as the last statement. A part keeps the block's entry check
 * (it covered every instruction of the block) and its own locals.
 */
#include "block.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include "gc_cpu.h"
#include "gc_cpu_decode.h"
#include "gc_cpu_ops.h"
}

namespace gcrecomp {
namespace {

constexpr unsigned CR = 32, XER = 33, LR = 34, CTR = 35, FPR0 = 36, NREG = 68;

/* A set of registers: the 32 general ones, CR, XER, LR, CTR, and the 32
 * floating-point ones (FPR0 + n). */
struct Mask {
  uint64_t lo = 0, hi = 0;
  constexpr Mask() = default;
  constexpr Mask(uint64_t l, uint64_t h) : lo(l), hi(h) {}
  Mask operator|(Mask o) const { return {lo | o.lo, hi | o.hi}; }
  Mask operator&(Mask o) const { return {lo & o.lo, hi & o.hi}; }
  Mask operator~() const { return {~lo, ~hi}; }
  Mask &operator|=(Mask o) {
    lo |= o.lo;
    hi |= o.hi;
    return *this;
  }
  Mask &operator&=(Mask o) {
    lo &= o.lo;
    hi &= o.hi;
    return *this;
  }
  explicit operator bool() const { return lo != 0 || hi != 0; }
};
constexpr Mask ALL{~0ull, (1ull << (NREG - 64)) - 1u};

inline Mask bit(unsigned r) { return r < 64 ? Mask(1ull << r, 0) : Mask(0, 1ull << (r - 64)); }
inline bool is_fpr(unsigned r) { return r >= FPR0; }

std::string hex(uint32_t value) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "0x%08xu", value);
  return buffer;
}

std::string num(uint64_t value) { return std::to_string(value) + "u"; }

const char *handler_name(GcOp op) {
  static const char *table[GC_OP_COUNT];
  static bool filled = false;
  if (!filled) {
#define GC_NAME(enum_name, function) table[enum_name] = #function;
    GC_CPU_OPS(GC_NAME)
#undef GC_NAME
    filled = true;
  }
  return (op > 0 && op < GC_OP_COUNT) ? table[op] : nullptr;
}

std::string local_name(unsigned r) {
  if (r < 32) return "r" + std::to_string(r);
  if (is_fpr(r)) return "f" + std::to_string(r - FPR0);
  switch (r) {
    case CR: return "cr";
    case XER: return "xer";
    case LR: return "lr";
    default: return "ctr";
  }
}

std::string memory_name(unsigned r) {
  if (r < 32) return "c->gpr[" + std::to_string(r) + "]";
  if (is_fpr(r)) return "c->fpr[" + std::to_string(r - FPR0) + "]";
  switch (r) {
    case CR: return "c->cr";
    case XER: return "c->xer";
    case LR: return "c->lr";
    default: return "c->ctr";
  }
}

/* A register written back to the processor, and read from it. */
std::string put_text(unsigned r) {
  if (is_fpr(r)) return "gc_rt_lanes_put(&" + memory_name(r) + ", " + local_name(r) + ");";
  return memory_name(r) + " = " + local_name(r) + ";";
}
std::string get_text(unsigned r) {
  if (is_fpr(r)) return local_name(r) + " = gc_rt_lanes_get(&" + memory_name(r) + ");";
  return local_name(r) + " = " + memory_name(r) + ";";
}

/* The note a paired instruction leaves in the steady state (gc_fp_lazy_note),
 * kept in the block's locals, written to the processor when anything could
 * read it. */
const char *const NOTE_FLUSH =
    "if (nd) { c->fp_lazy = nk; c->fp_lazy_a = na; c->fp_lazy_b = nb; c->fp_lazy_c = nc; c->fp_lazy_result = nr; nd = 0; } ";

/* The floating-point handlers that are called rather than inlined unless
 * fp_inline: the ones that carry the IEEE flag model, which is what made
 * generated files slow to compile (the same list as the file split's cost). */
bool heavy_fp(GcOp op) {
  switch (op) {
    case GC_OP_PS_ADD: case GC_OP_PS_SUB: case GC_OP_PS_MUL: case GC_OP_PS_DIV:
    case GC_OP_PS_MADD: case GC_OP_PS_MADDS0: case GC_OP_PS_MADDS1: case GC_OP_PS_MSUB:
    case GC_OP_PS_NMSUB: case GC_OP_PS_NMADD: case GC_OP_PS_SUM0: case GC_OP_PS_SUM1:
    case GC_OP_PS_MULS0: case GC_OP_PS_MULS1: case GC_OP_PS_RES: case GC_OP_PS_RSQRTE:
    case GC_OP_FADD: case GC_OP_FADDS: case GC_OP_FSUB: case GC_OP_FSUBS:
    case GC_OP_FMUL: case GC_OP_FMULS: case GC_OP_FDIV: case GC_OP_FDIVS:
    case GC_OP_FMADD: case GC_OP_FMADDS: case GC_OP_FMSUB: case GC_OP_FMSUBS:
    case GC_OP_FNMADD: case GC_OP_FNMADDS: case GC_OP_FNMSUB: case GC_OP_FNMSUBS:
    case GC_OP_FRES: case GC_OP_FRSQRTE:
    case GC_OP_FCMPU: case GC_OP_FCMPO: case GC_OP_FRSP: case GC_OP_FCTIW:
    case GC_OP_FCTIWZ: case GC_OP_PS_CMPU0: case GC_OP_PS_CMPO0: case GC_OP_PS_CMPU1:
    case GC_OP_PS_CMPO1: case GC_OP_MTFSF: case GC_OP_MTFSFI: case GC_OP_MTFSB0: case GC_OP_MTFSB1:
    case GC_OP_MCRFS: case GC_OP_MFFS:
      return true;
    default:
      return false;
  }
}

/* Floating-point handlers that touch nothing but the floating-point
 * registers, FPSCR, the host's floating-point state, and CR (a compare's
 * field, or CR1 for an Rc form): no general register, XER, LR, CTR, program
 * counter, count, memory or event. Established by reading them in
 * gc_fpu_impl.h; they cannot raise an exception (no floating-point
 * exception is modelled). */
bool fp_register_only(GcOp op, uint32_t word, bool *touches_cr) {
  const bool rc = GC_M_Rc(word) != 0;
  switch (op) {
    case GC_OP_FCMPU: case GC_OP_FCMPO: case GC_OP_PS_CMPU0: case GC_OP_PS_CMPO0:
    case GC_OP_PS_CMPU1: case GC_OP_PS_CMPO1: case GC_OP_MCRFS:
      *touches_cr = true;
      return true;
    case GC_OP_FADD: case GC_OP_FADDS: case GC_OP_FSUB: case GC_OP_FSUBS: case GC_OP_FMUL:
    case GC_OP_FMULS: case GC_OP_FDIV: case GC_OP_FDIVS: case GC_OP_FMADD: case GC_OP_FMADDS:
    case GC_OP_FMSUB: case GC_OP_FMSUBS: case GC_OP_FNMADD: case GC_OP_FNMADDS: case GC_OP_FNMSUB:
    case GC_OP_FNMSUBS: case GC_OP_FMR: case GC_OP_FNEG: case GC_OP_FABS: case GC_OP_FNABS:
    case GC_OP_FRSP: case GC_OP_FCTIW: case GC_OP_FCTIWZ: case GC_OP_FSEL: case GC_OP_FRES:
    case GC_OP_FRSQRTE: case GC_OP_MFFS: case GC_OP_MTFSF: case GC_OP_MTFSFI: case GC_OP_MTFSB0:
    case GC_OP_MTFSB1: case GC_OP_PS_ADD: case GC_OP_PS_SUB: case GC_OP_PS_MUL: case GC_OP_PS_DIV:
    case GC_OP_PS_MULS0: case GC_OP_PS_MULS1: case GC_OP_PS_MADD: case GC_OP_PS_MSUB:
    case GC_OP_PS_NMADD: case GC_OP_PS_NMSUB: case GC_OP_PS_MADDS0: case GC_OP_PS_MADDS1:
    case GC_OP_PS_SUM0: case GC_OP_PS_SUM1: case GC_OP_PS_NEG: case GC_OP_PS_ABS: case GC_OP_PS_NABS:
    case GC_OP_PS_MR: case GC_OP_PS_MERGE00: case GC_OP_PS_MERGE01: case GC_OP_PS_MERGE10:
    case GC_OP_PS_MERGE11: case GC_OP_PS_SEL: case GC_OP_PS_RES: case GC_OP_PS_RSQRTE:
      *touches_cr = rc;
      return true;
    default:
      return false;
  }
}

/* The state of one C function while its instructions are written. */
struct Fn {
  unsigned count = 0;      /* instructions in the whole block */
  unsigned index = 0;      /* the instruction being written, from the block's start */
  unsigned part_start = 0; /* its function's first instruction */
  uint32_t address = 0;
  uint32_t word = 0;
  Mask cached;         /* registers with a local in this function */
  Mask dirty;          /* written to their local since memory last had them */
  bool note = false;   /* the function keeps the steady state's note in locals */
  bool uses_note = false; /* this instruction does */
  bool maps = false;   /* fml / fms are declared */
  /* The block branches back to its own start inside the function: a
   * register written anywhere in it may be newer than memory anywhere. */
  bool loop = false;
  Mask loop_written;
  /* What this instruction does with state, as recorded while writing it. */
  Mask lread, lwrite, mread, mwrite;
  bool load_map = false, store_map = false;
  std::string out;

  /* A read and a write of the same register by one instruction count as a
   * read (it is loaded on entry): the text of an instruction is built in
   * C++ expressions whose operands are evaluated in no fixed order, so
   * what is recorded must not depend on the order of these calls. */
  std::string R(unsigned r) {
    lread |= bit(r);
    return local_name(r);
  }
  std::string W(unsigned r) {
    lwrite |= bit(r);
    dirty |= bit(r);
    return local_name(r);
  }
  std::string X(unsigned i) const { return "x_" + std::to_string(i); }
  std::string here() const { return X(index); }
  std::string next() const { return X(index + 1); }
  std::string bottom() const { return "x_nf"; }
  std::string left() const { return num(count - index - 1); }
  std::string cycles() const { return "cyc + " + num(index); }
  std::string flush(Mask mask) {
    std::string text;
    const Mask todo = mask & (dirty | (loop ? loop_written : Mask())) & cached;
    for (unsigned r = 0; r < NREG; r++) {
      if (todo & bit(r)) text += put_text(r) + " ";
    }
    dirty &= ~todo;
    return text;
  }
  std::string reload(Mask mask) {
    std::string text;
    const Mask todo = mask & cached;
    for (unsigned r = 0; r < NREG; r++) {
      if (todo & bit(r)) text += get_text(r) + " ";
    }
    dirty &= ~todo;
    return text;
  }
  std::string note_flush() const { return note ? NOTE_FLUSH : ""; }
  std::string steady_again() const { return note ? "steady = GC_RT_STEADY(c); " : ""; }
  std::string maps_again() const {
    return maps ? "fml = GC_RT_LOAD_MAP(c); fms = GC_RT_STORE_MAP(c, fml); " : "";
  }
};

uint32_t simm(uint32_t w) { return (uint32_t)GC_F_simm(w); }

/* rA|0 + d */
std::string ea_d(Fn &f, uint32_t w, uint32_t d) {
  const unsigned ra = GC_F_rA(w);
  return ra ? "(" + f.R(ra) + " + " + hex(d) + ")" : hex(d);
}
std::string ea_du(Fn &f, uint32_t w, uint32_t d) { return "(" + f.R(GC_F_rA(w)) + " + " + hex(d) + ")"; }
std::string ea_x(Fn &f, uint32_t w) {
  const unsigned ra = GC_F_rA(w);
  return ra ? "(" + f.R(ra) + " + " + f.R(GC_F_rB(w)) + ")" : f.R(GC_F_rB(w));
}
std::string ea_xu(Fn &f, uint32_t w) { return "(" + f.R(GC_F_rA(w)) + " + " + f.R(GC_F_rB(w)) + ")"; }

/* CR0 from a result, as gc_set_cr0. */
std::string cr0_of(Fn &f, const std::string &value) {
  const std::string rhs = "GC_RT_SETF(" + f.R(CR) + ", 0u, GC_RT_CMPS(" + value + ", 0u, " + f.R(XER) + "))";
  return f.W(CR) + " = " + rhs + "; ";
}

std::string set_ca(Fn &f, const std::string &carry) {
  const std::string rhs = "(" + f.R(XER) + " & ~0x20000000u) | ((" + carry + ") ? 0x20000000u : 0u)";
  return f.W(XER) + " = " + rhs + "; ";
}

enum class Kind { Template, FpCall, Fallback, End };

/* A load: fast path, slow path, and `writes` - the statements that complete
 * the instruction, emitted on both paths. */
std::string load_text(Fn &f, const std::string &ea, unsigned size, const std::string &value_type,
                      const std::function<std::string(Fn &, const std::string &)> &writes) {
  f.load_map = true;
  const char *get = size == 1 ? "gc_rt_get8" : size == 2 ? "gc_rt_get16" : size == 4 ? "gc_rt_get32" : "gc_rt_get64";
  std::string t = "{ const uint32_t ea = " + ea + "; " + value_type + " v; ";
  t += "if (GC_RT_OK(fml, ea, " + num(size) + ")) v = " + get + "(GC_RT_AT(fml, ea)); ";
  t += "else { const uint64_t w = gc_rt_load(m, ea, " + f.cycles() + ", GC_RT_WHERE(" + hex(f.address) + ", " +
       f.left() + ", " + num(size) + ")); v = (" + value_type + ")w; if (gc_rt_status) { if (gc_rt_status == 1) goto " +
       f.here() + "; " + writes(f, "v") + "goto " + f.next() + "; } } ";
  t += writes(f, "v") + "}";
  return t;
}

std::string store_text(Fn &f, const std::string &ea, unsigned size, const std::string &value,
                       const std::string &after) {
  f.store_map = true;
  const char *put = size == 1 ? "gc_rt_put8" : size == 2 ? "gc_rt_put16" : size == 4 ? "gc_rt_put32" : "gc_rt_put64";
  const std::string type = size == 8 ? "uint64_t" : "uint32_t";
  std::string t = "{ const uint32_t ea = " + ea + "; const " + type + " v = " + value + "; ";
  t += "if (GC_RT_OK(fms, ea, " + num(size) + ")) " + put + "(GC_RT_AT(fms, ea), v); ";
  t += "else { const int st = gc_rt_store(m, ea, (uint64_t)v, " + f.cycles() + ", GC_RT_WHERE(" + hex(f.address) +
       ", " + f.left() + ", " + num(size) + ")); if (st == 1) goto " + f.here() + "; if (st) { " + after + "goto " +
       f.next() + "; } } ";
  t += after + "}";
  return t;
}

/* A handler, out of line, with the registers written back before and read
 * again after. */
std::string fallback_text(Fn &f) {
  f.mread = f.mwrite = ALL;
  std::string t = "{ " + f.flush(ALL) + f.note_flush();
  t += "if (gc_rt_insn(m, " + hex(f.word) + ", " + hex(f.address) + ", " + f.cycles() + ", " + f.left() +
       ")) goto " + f.bottom() + "; ";
  t += f.reload(ALL) + f.maps_again() + f.steady_again() + "}";
  return t;
}

/* Writes instruction f.word at f.address. Returns what kind it was; for
 * Kind::End (a branch that ends the block) `end` receives the pieces the
 * caller assembles around the write-back. */
struct EndInfo {
  std::string before;     /* computed before the registers are written back */
  std::string condition;  /* empty: unconditional */
  std::string target;     /* expression, or empty for an address constant */
  uint32_t target_address = 0;
  bool known_target = false;
  uint32_t fall = 0;
  bool link = false;   /* a call: LK */
  bool ret = false;    /* a return: bclr without LK */
};

Kind write_insn(Fn &f, GcOp op, const BlockOptions &options, EndInfo *end) {
  const uint32_t w = f.word;
  const unsigned rd = GC_F_rD(w), ra = GC_F_rA(w), rb = GC_F_rB(w), rs = GC_F_rS(w);
  const bool rc = GC_M_Rc(w) != 0;
  const bool oe = GC_M_OE(w) != 0;
  std::string &o = f.out;

  auto logical = [&](const std::string &expr) {
    std::string t = "{ const uint32_t v = " + expr + "; " + f.W(ra) + " = v; ";
    if (rc) t += cr0_of(f, "v");
    o += t + "}";
    return Kind::Template;
  };
  /* gc_add without OE: result, the carry when asked, CR0 when Rc. */
  auto adder = [&](const std::string &a, const std::string &b, const std::string &carry, bool set_carry,
                   bool allow_rc) {
    std::string t = "{ const uint64_t wide = (uint64_t)" + a + " + (uint64_t)" + b + " + (uint64_t)(" + carry +
                    "); const uint32_t v = (uint32_t)wide; ";
    if (set_carry) t += set_ca(f, "(wide >> 32) != 0u");
    if (allow_rc && rc) t += cr0_of(f, "v");
    t += f.W(rd) + " = v; }";
    o += t;
    return Kind::Template;
  };
  auto carry_in = [&]() { return "((" + f.R(XER) + " >> 29) & 1u)"; };

  switch (op) {
    /* ---- integer arithmetic (gc_cpu_impl.h) ---- */
    case GC_OP_ADDI:
      o += f.W(rd) + " = " + ea_d(f, w, simm(w)) + ";";
      return Kind::Template;
    case GC_OP_ADDIS:
      o += f.W(rd) + " = " + ea_d(f, w, GC_F_uimm(w) << 16) + ";";
      return Kind::Template;
    case GC_OP_ADD:
      if (oe) break;
      return adder(f.R(ra), f.R(rb), "0u", false, true);
    case GC_OP_ADDC:
      if (oe) break;
      return adder(f.R(ra), f.R(rb), "0u", true, true);
    case GC_OP_ADDE:
      if (oe) break;
      return adder(f.R(ra), f.R(rb), carry_in(), true, true);
    case GC_OP_ADDZE:
      if (oe) break;
      return adder(f.R(ra), "0u", carry_in(), true, true);
    case GC_OP_ADDME:
      if (oe) break;
      return adder(f.R(ra), "0xffffffffu", carry_in(), true, true);
    case GC_OP_ADDIC:
      return adder(f.R(ra), hex(simm(w)), "0u", true, false);
    case GC_OP_ADDIC_: {
      std::string t = "{ const uint64_t wide = (uint64_t)" + f.R(ra) + " + (uint64_t)" + hex(simm(w)) +
                      "; const uint32_t v = (uint32_t)wide; " + set_ca(f, "(wide >> 32) != 0u");
      t += f.W(rd) + " = v; " + cr0_of(f, "v") + "}";
      o += t;
      return Kind::Template;
    }
    case GC_OP_SUBF:
      if (oe) break;
      return adder("(~" + f.R(ra) + ")", f.R(rb), "1u", false, true);
    case GC_OP_SUBFC:
      if (oe) break;
      return adder("(~" + f.R(ra) + ")", f.R(rb), "1u", true, true);
    case GC_OP_SUBFE:
      if (oe) break;
      return adder("(~" + f.R(ra) + ")", f.R(rb), carry_in(), true, true);
    case GC_OP_SUBFZE:
      if (oe) break;
      return adder("(~" + f.R(ra) + ")", "0u", carry_in(), true, true);
    case GC_OP_SUBFME:
      if (oe) break;
      return adder("(~" + f.R(ra) + ")", "0xffffffffu", carry_in(), true, true);
    case GC_OP_SUBFIC:
      return adder("(~" + f.R(ra) + ")", hex(simm(w)), "1u", true, false);
    case GC_OP_NEG: {
      if (oe) break;
      std::string t = "{ const uint32_t v = ~" + f.R(ra) + " + 1u; ";
      if (rc) t += cr0_of(f, "v");
      o += t + f.W(rd) + " = v; }";
      return Kind::Template;
    }
    case GC_OP_MULLI:
      o += f.W(rd) + " = " + f.R(ra) + " * " + hex(simm(w)) + ";";
      return Kind::Template;
    case GC_OP_MULLW: {
      if (oe) break;
      std::string t = "{ const uint32_t v = (uint32_t)((int64_t)(int32_t)" + f.R(ra) + " * (int64_t)(int32_t)" +
                      f.R(rb) + "); ";
      if (rc) t += cr0_of(f, "v");
      o += t + f.W(rd) + " = v; }";
      return Kind::Template;
    }
    case GC_OP_MULHW: {
      std::string t = "{ const uint32_t v = (uint32_t)((uint64_t)((int64_t)(int32_t)" + f.R(ra) +
                      " * (int64_t)(int32_t)" + f.R(rb) + ") >> 32); ";
      if (rc) t += cr0_of(f, "v");
      o += t + f.W(rd) + " = v; }";
      return Kind::Template;
    }
    case GC_OP_MULHWU: {
      std::string t = "{ const uint32_t v = (uint32_t)(((uint64_t)" + f.R(ra) + " * (uint64_t)" + f.R(rb) +
                      ") >> 32); ";
      if (rc) t += cr0_of(f, "v");
      o += t + f.W(rd) + " = v; }";
      return Kind::Template;
    }
    case GC_OP_DIVW: {
      if (oe) break;
      std::string t = "{ const int32_t a = (int32_t)" + f.R(ra) + ", b = (int32_t)" + f.R(rb) +
                      "; const uint32_t v = (b == 0 || (a == (int32_t)0x80000000 && b == -1)) ? "
                      "(a < 0 ? 0xffffffffu : 0u) : (uint32_t)(a / b); ";
      if (rc) t += cr0_of(f, "v");
      o += t + f.W(rd) + " = v; }";
      return Kind::Template;
    }
    case GC_OP_DIVWU: {
      if (oe) break;
      std::string t = "{ const uint32_t a = " + f.R(ra) + ", b = " + f.R(rb) + "; const uint32_t v = b ? a / b : 0u; ";
      if (rc) t += cr0_of(f, "v");
      o += t + f.W(rd) + " = v; }";
      return Kind::Template;
    }

    /* ---- compare ---- */
    case GC_OP_CMP: case GC_OP_CMPI: case GC_OP_CMPL: case GC_OP_CMPLI: {
      const bool is_signed = op == GC_OP_CMP || op == GC_OP_CMPI;
      const std::string b = (op == GC_OP_CMP || op == GC_OP_CMPL) ? f.R(rb)
                            : is_signed                            ? hex(simm(w))
                                                                   : hex(GC_F_uimm(w));
      const std::string rhs = "GC_RT_SETF(" + f.R(CR) + ", " + num(GC_F_crfD(w)) + ", " +
                              (is_signed ? "GC_RT_CMPS(" : "GC_RT_CMPU(") + f.R(ra) + ", " + b + ", " + f.R(XER) +
                              "))";
      o += f.W(CR) + " = " + rhs + ";";
      return Kind::Template;
    }

    /* ---- logical ---- */
    case GC_OP_AND: return logical("(" + f.R(rs) + " & " + f.R(rb) + ")");
    case GC_OP_ANDC: return logical("(" + f.R(rs) + " & ~" + f.R(rb) + ")");
    case GC_OP_OR: return logical("(" + f.R(rs) + " | " + f.R(rb) + ")");
    case GC_OP_ORC: return logical("(" + f.R(rs) + " | ~" + f.R(rb) + ")");
    case GC_OP_XOR: return logical("(" + f.R(rs) + " ^ " + f.R(rb) + ")");
    case GC_OP_NAND: return logical("~(" + f.R(rs) + " & " + f.R(rb) + ")");
    case GC_OP_NOR: return logical("~(" + f.R(rs) + " | " + f.R(rb) + ")");
    case GC_OP_EQV: return logical("~(" + f.R(rs) + " ^ " + f.R(rb) + ")");
    case GC_OP_EXTSB: return logical("(uint32_t)(int32_t)(int8_t)" + f.R(rs));
    case GC_OP_EXTSH: return logical("(uint32_t)(int32_t)(int16_t)" + f.R(rs));
    case GC_OP_CNTLZW: return logical("gc_rt_clz32(" + f.R(rs) + ")");
    case GC_OP_ORI:
      o += f.W(ra) + " = " + f.R(rs) + " | " + hex(GC_F_uimm(w)) + ";";
      return Kind::Template;
    case GC_OP_ORIS:
      o += f.W(ra) + " = " + f.R(rs) + " | " + hex(GC_F_uimm(w) << 16) + ";";
      return Kind::Template;
    case GC_OP_XORI:
      o += f.W(ra) + " = " + f.R(rs) + " ^ " + hex(GC_F_uimm(w)) + ";";
      return Kind::Template;
    case GC_OP_XORIS:
      o += f.W(ra) + " = " + f.R(rs) + " ^ " + hex(GC_F_uimm(w) << 16) + ";";
      return Kind::Template;
    case GC_OP_ANDI_: case GC_OP_ANDIS_: {
      const uint32_t mask = op == GC_OP_ANDI_ ? GC_F_uimm(w) : GC_F_uimm(w) << 16;
      std::string t = "{ const uint32_t v = " + f.R(rs) + " & " + hex(mask) + "; " + f.W(ra) + " = v; ";
      o += t + cr0_of(f, "v") + "}";
      return Kind::Template;
    }

    /* ---- rotate and shift ---- */
    case GC_OP_RLWINM: case GC_OP_RLWNM: case GC_OP_RLWIMI: {
      const unsigned mb = GC_F_MB(w), me = GC_F_ME(w);
      const uint32_t begin = 0xffffffffu >> mb;
      const uint32_t finish = (me >= 31) ? 0xffffffffu : ~(0xffffffffu >> (me + 1));
      const uint32_t mask = (mb <= me) ? (begin & finish) : (begin | finish);
      std::string rotated;
      if (op == GC_OP_RLWNM) {
        rotated = "gc_rt_rotl32(" + f.R(rs) + ", " + f.R(rb) + ")";
      } else {
        const unsigned sh = GC_F_SH(w);
        rotated = sh ? "((" + f.R(rs) + " << " + num(sh) + ") | (" + f.R(rs) + " >> " + num(32 - sh) + "))"
                     : f.R(rs);
      }
      if (op == GC_OP_RLWIMI) {
        return logical("((" + rotated + " & " + hex(mask) + ") | (" + f.R(ra) + " & " + hex(~mask) + "))");
      }
      return logical("(" + rotated + " & " + hex(mask) + ")");
    }
    case GC_OP_SLW: case GC_OP_SRW: {
      const std::string amount = "(" + f.R(rb) + " & 0x3fu)";
      const std::string value = f.R(rs);
      return logical("(" + amount + " < 32u ? (" + value + (op == GC_OP_SLW ? " << " : " >> ") + amount + ") : 0u)");
    }
    case GC_OP_SRAWI: {
      const unsigned sh = GC_F_SH(w);
      std::string t = "{ const uint32_t value = " + f.R(rs) + "; const uint32_t v = (uint32_t)((int32_t)value >> " +
                      num(sh) + "); ";
      t += set_ca(f, "(value & 0x80000000u) != 0u && (value & " + hex(sh ? ((1u << sh) - 1u) : 0u) + ") != 0u");
      t += f.W(ra) + " = v; ";
      if (rc) t += cr0_of(f, "v");
      o += t + "}";
      return Kind::Template;
    }
    case GC_OP_SRAW: {
      std::string t = "{ const uint32_t value = " + f.R(rs) + ", amount = " + f.R(rb) +
                      " & 0x3fu; uint32_t v, lost; if (amount >= 32u) { v = (value & 0x80000000u) ? 0xffffffffu : 0u; "
                      "lost = value; } else { v = (uint32_t)((int32_t)value >> amount); lost = amount ? (value & "
                      "((1u << amount) - 1u)) : 0u; } ";
      t += set_ca(f, "(value & 0x80000000u) != 0u && lost != 0u");
      t += f.W(ra) + " = v; ";
      if (rc) t += cr0_of(f, "v");
      o += t + "}";
      return Kind::Template;
    }

    /* ---- condition register logic ---- */
    case GC_OP_CRAND: case GC_OP_CRANDC: case GC_OP_CROR: case GC_OP_CRORC: case GC_OP_CRXOR:
    case GC_OP_CRNAND: case GC_OP_CRNOR: case GC_OP_CREQV: {
      const std::string a = "((" + f.R(CR) + " >> " + num(31 - GC_F_crbA(w)) + ") & 1u)";
      const std::string b = "((" + f.R(CR) + " >> " + num(31 - GC_F_crbB(w)) + ") & 1u)";
      std::string v;
      switch (op) {
        case GC_OP_CRAND: v = "(" + a + " & " + b + ")"; break;
        case GC_OP_CRANDC: v = "(" + a + " & ~" + b + " & 1u)"; break;
        case GC_OP_CROR: v = "(" + a + " | " + b + ")"; break;
        case GC_OP_CRORC: v = "((" + a + " | ~" + b + ") & 1u)"; break;
        case GC_OP_CRXOR: v = "(" + a + " ^ " + b + ")"; break;
        case GC_OP_CRNAND: v = "(~(" + a + " & " + b + ") & 1u)"; break;
        case GC_OP_CRNOR: v = "(~(" + a + " | " + b + ") & 1u)"; break;
        default: v = "(~(" + a + " ^ " + b + ") & 1u)"; break;
      }
      const uint32_t mask = 0x80000000u >> GC_F_crbD(w);
      const std::string rhs = v + " ? (" + f.R(CR) + " | " + hex(mask) + ") : (" + f.R(CR) + " & " + hex(~mask) + ")";
      o += f.W(CR) + " = " + rhs + ";";
      return Kind::Template;
    }
    case GC_OP_MCRF: {
      const std::string rhs = "GC_RT_SETF(" + f.R(CR) + ", " + num(GC_F_crfD(w)) + ", ((" + f.R(CR) + " >> " +
                              num(28 - 4 * GC_F_crfS(w)) + ") & 0xfu))";
      o += f.W(CR) + " = " + rhs + ";";
      return Kind::Template;
    }
    case GC_OP_MFCR:
      o += f.W(rd) + " = " + f.R(CR) + ";";
      return Kind::Template;
    case GC_OP_MTCRF: {
      const uint32_t fields = GC_F_crm(w);
      uint32_t mask = 0;
      for (unsigned i = 0; i < 8; i++) {
        if (fields & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
      }
      const std::string rhs = "(" + f.R(CR) + " & " + hex(~mask) + ") | (" + f.R(rs) + " & " + hex(mask) + ")";
      o += f.W(CR) + " = " + rhs + ";";
      return Kind::Template;
    }
    case GC_OP_MCRXR: {
      std::string t = "{ const uint32_t x = " + f.R(XER) + "; ";
      const std::string rhs = "GC_RT_SETF(" + f.R(CR) + ", " + num(GC_F_crfD(w)) + ", (x >> 28))";
      t += f.W(CR) + " = " + rhs + "; " + f.W(XER) + " = x & ~0xe0000000u; }";
      o += t;
      return Kind::Template;
    }

    /* ---- special registers the core keeps as plain fields ---- */
    case GC_OP_MFSPR: {
      const uint32_t spr = GC_F_spr(w);
      std::string src;
      switch (spr) {
        case GC_SPR_XER: src = f.R(XER); break;
        case GC_SPR_LR: src = f.R(LR); break;
        case GC_SPR_CTR: src = f.R(CTR); break;
        case GC_SPR_DSISR: src = "c->dsisr"; break;
        case GC_SPR_DAR: src = "c->dar"; break;
        case GC_SPR_SDR1: src = "c->sdr1"; break;
        case GC_SPR_SRR0: src = "c->srr0"; break;
        case GC_SPR_SRR1: src = "c->srr1"; break;
        case GC_SPR_EAR: src = "c->ear"; break;
        case GC_SPR_PVR: src = "c->pvr"; break;
        case GC_SPR_HID0: src = "c->hid0"; break;
        case GC_SPR_HID1: src = "c->hid1"; break;
        case GC_SPR_HID2: src = "c->hid2"; break;
        case GC_SPR_WPAR: src = "c->wpar"; break;
        case GC_SPR_DMAU: src = "c->dmau"; break;
        case GC_SPR_DMAL: src = "c->dmal"; break;
        case GC_SPR_L2CR: src = "c->l2cr"; break;
        default:
          if (spr >= GC_SPR_SPRG0 && spr < GC_SPR_SPRG0 + 4) src = "c->sprg[" + num(spr - GC_SPR_SPRG0) + "]";
          else if (spr >= GC_SPR_GQR0 && spr < GC_SPR_GQR0 + 8) src = "c->gqr[" + num(spr - GC_SPR_GQR0) + "]";
          else if (spr >= GC_SPR_IBAT0U && spr < GC_SPR_IBAT0U + 8) src = "c->ibat[" + num(spr - GC_SPR_IBAT0U) + "]";
          else if (spr >= GC_SPR_DBAT0U && spr < GC_SPR_DBAT0U + 8) src = "c->dbat[" + num(spr - GC_SPR_DBAT0U) + "]";
          break;
      }
      if (src.empty()) break;
      o += f.W(rd) + " = " + src + ";";
      return Kind::Template;
    }
    case GC_OP_MTSPR: {
      const uint32_t spr = GC_F_spr(w);
      std::string dst;
      switch (spr) {
        case GC_SPR_XER: {
          const std::string v = f.R(rs);
          o += f.W(XER) + " = " + v + " & " + hex(GC_XER_WRITABLE) + ";";
          return Kind::Template;
        }
        case GC_SPR_LR: {
          const std::string v = f.R(rs);
          o += f.W(LR) + " = " + v + ";";
          return Kind::Template;
        }
        case GC_SPR_CTR: {
          const std::string v = f.R(rs);
          o += f.W(CTR) + " = " + v + ";";
          return Kind::Template;
        }
        case GC_SPR_DSISR: dst = "c->dsisr"; break;
        case GC_SPR_DAR: dst = "c->dar"; break;
        case GC_SPR_SDR1: dst = "c->sdr1"; break;
        case GC_SPR_SRR0: dst = "c->srr0"; break;
        case GC_SPR_SRR1: dst = "c->srr1"; break;
        case GC_SPR_EAR: dst = "c->ear"; break;
        case GC_SPR_HID0: dst = "c->hid0"; break;
        case GC_SPR_HID1: dst = "c->hid1"; break;
        case GC_SPR_L2CR: dst = "c->l2cr"; break;
        default:
          if (spr >= GC_SPR_SPRG0 && spr < GC_SPR_SPRG0 + 4) dst = "c->sprg[" + num(spr - GC_SPR_SPRG0) + "]";
          else if (spr >= GC_SPR_GQR0 && spr < GC_SPR_GQR0 + 8) dst = "c->gqr[" + num(spr - GC_SPR_GQR0) + "]";
          else if (spr >= GC_SPR_IBAT0U && spr < GC_SPR_IBAT0U + 8) dst = "c->ibat[" + num(spr - GC_SPR_IBAT0U) + "]";
          break;
      }
      if (dst.empty()) break;
      o += dst + " = " + f.R(rs) + ";";
      return Kind::Template;
    }
    case GC_OP_MFMSR:
      o += f.W(rd) + " = c->msr;";
      return Kind::Template;

    /* ---- no-ops ---- */
    case GC_OP_SYNC: case GC_OP_ISYNC: case GC_OP_EIEIO: case GC_OP_TLBSYNC: case GC_OP_TLBIE:
    case GC_OP_DCBT: case GC_OP_DCBTST: case GC_OP_DCBST: case GC_OP_DCBF: case GC_OP_DCBI:
      o += "/* nothing */";
      return Kind::Template;

    /* ---- loads ---- */
    case GC_OP_LBZ: case GC_OP_LBZU: case GC_OP_LBZX: case GC_OP_LBZUX:
    case GC_OP_LHZ: case GC_OP_LHZU: case GC_OP_LHZX: case GC_OP_LHZUX:
    case GC_OP_LHA: case GC_OP_LHAU: case GC_OP_LHAX: case GC_OP_LHAUX:
    case GC_OP_LWZ: case GC_OP_LWZU: case GC_OP_LWZX: case GC_OP_LWZUX:
    case GC_OP_LHBRX: case GC_OP_LWBRX: {
      const bool update = op == GC_OP_LBZU || op == GC_OP_LBZUX || op == GC_OP_LHZU || op == GC_OP_LHZUX ||
                          op == GC_OP_LHAU || op == GC_OP_LHAUX || op == GC_OP_LWZU || op == GC_OP_LWZUX;
      const bool indexed = op == GC_OP_LBZX || op == GC_OP_LBZUX || op == GC_OP_LHZX || op == GC_OP_LHZUX ||
                           op == GC_OP_LHAX || op == GC_OP_LHAUX || op == GC_OP_LWZX || op == GC_OP_LWZUX ||
                           op == GC_OP_LHBRX || op == GC_OP_LWBRX;
      const unsigned size = (op == GC_OP_LBZ || op == GC_OP_LBZU || op == GC_OP_LBZX || op == GC_OP_LBZUX) ? 1u
                            : (op == GC_OP_LWZ || op == GC_OP_LWZU || op == GC_OP_LWZX || op == GC_OP_LWZUX ||
                               op == GC_OP_LWBRX)
                                ? 4u
                                : 2u;
      std::string conv = "v";
      if (op == GC_OP_LHA || op == GC_OP_LHAU || op == GC_OP_LHAX || op == GC_OP_LHAUX) {
        conv = "(uint32_t)(int32_t)(int16_t)v";
      } else if (op == GC_OP_LHBRX) {
        conv = "(uint32_t)GC_RT_BSWAP16((uint16_t)v)";
      } else if (op == GC_OP_LWBRX) {
        conv = "GC_RT_BSWAP32(v)";
      }
      const std::string ea = indexed ? (update ? ea_xu(f, w) : ea_x(f, w)) : (update ? ea_du(f, w, simm(w)) : ea_d(f, w, simm(w)));
      o += load_text(f, ea, size, "uint32_t", [&](Fn &g, const std::string &) {
        std::string t = g.W(rd) + " = " + conv + "; ";
        if (update) t += g.W(ra) + " = ea; ";
        return t;
      });
      return Kind::Template;
    }

    /* ---- stores ---- */
    case GC_OP_STB: case GC_OP_STBU: case GC_OP_STBX: case GC_OP_STBUX:
    case GC_OP_STH: case GC_OP_STHU: case GC_OP_STHX: case GC_OP_STHUX:
    case GC_OP_STW: case GC_OP_STWU: case GC_OP_STWX: case GC_OP_STWUX:
    case GC_OP_STHBRX: case GC_OP_STWBRX: {
      const bool update = op == GC_OP_STBU || op == GC_OP_STBUX || op == GC_OP_STHU || op == GC_OP_STHUX ||
                          op == GC_OP_STWU || op == GC_OP_STWUX;
      const bool indexed = op == GC_OP_STBX || op == GC_OP_STBUX || op == GC_OP_STHX || op == GC_OP_STHUX ||
                           op == GC_OP_STWX || op == GC_OP_STWUX || op == GC_OP_STHBRX || op == GC_OP_STWBRX;
      const unsigned size = (op == GC_OP_STB || op == GC_OP_STBU || op == GC_OP_STBX || op == GC_OP_STBUX) ? 1u
                            : (op == GC_OP_STW || op == GC_OP_STWU || op == GC_OP_STWX || op == GC_OP_STWUX ||
                               op == GC_OP_STWBRX)
                                ? 4u
                                : 2u;
      const std::string ea = indexed ? (update ? ea_xu(f, w) : ea_x(f, w)) : (update ? ea_du(f, w, simm(w)) : ea_d(f, w, simm(w)));
      std::string value = f.R(rs);
      if (op == GC_OP_STHBRX) value = "(uint32_t)GC_RT_BSWAP16((uint16_t)" + value + ")";
      if (op == GC_OP_STWBRX) value = "GC_RT_BSWAP32(" + value + ")";
      const std::string after = update ? f.W(ra) + " = ea; " : "";
      o += store_text(f, ea, size, value, after);
      return Kind::Template;
    }
    case GC_OP_LMW: case GC_OP_STMW: {
      /* Every word at once when the shortcut covers the whole range, which
       * then keeps all of them in one piece; otherwise the handler. */
      const unsigned first = rd;
      const unsigned words = 32u - first;
      const bool load = op == GC_OP_LMW;
      const Mask before = f.dirty;
      const std::string ea = ea_d(f, w, simm(w));
      std::string slow = fallback_text(f);
      Mask after_slow = f.dirty;
      f.dirty = before;
      std::string fast;
      if (load) {
        f.load_map = true;
        fast = "const uint8_t *p = GC_RT_AT(fml, ea); ";
        for (unsigned r = first; r < 32; r++) fast += f.W(r) + " = gc_rt_get32(p + " + num(4 * (r - first)) + "); ";
      } else {
        f.store_map = true;
        fast = "uint8_t *p = GC_RT_AT(fms, ea); ";
        for (unsigned r = first; r < 32; r++) fast += "gc_rt_put32(p + " + num(4 * (r - first)) + ", " + f.R(r) + "); ";
      }
      f.dirty |= after_slow;
      o += "{ const uint32_t ea = " + ea + "; if (GC_RT_OK(" + (load ? "fml" : "fms") + ", ea, " + num(4 * words) +
           ")) { " + fast + "} else " + slow + " }";
      return Kind::Template;
    }

    /* ---- floating-point loads and stores (gc_fpu_impl.h) ---- */
    case GC_OP_LFS: case GC_OP_LFSU: case GC_OP_LFSX: case GC_OP_LFSUX:
    case GC_OP_LFD: case GC_OP_LFDU: case GC_OP_LFDX: case GC_OP_LFDUX: {
      const bool single = op == GC_OP_LFS || op == GC_OP_LFSU || op == GC_OP_LFSX || op == GC_OP_LFSUX;
      const bool update = op == GC_OP_LFSU || op == GC_OP_LFSUX || op == GC_OP_LFDU || op == GC_OP_LFDUX;
      const bool indexed = op == GC_OP_LFSX || op == GC_OP_LFSUX || op == GC_OP_LFDX || op == GC_OP_LFDUX;
      const std::string ea = indexed ? (update ? ea_xu(f, w) : ea_x(f, w)) : (update ? ea_du(f, w, simm(w)) : ea_d(f, w, simm(w)));
      const unsigned frd = FPR0 + rd;
      o += load_text(f, ea, single ? 4u : 8u, single ? "uint32_t" : "uint64_t", [&](Fn &g, const std::string &) {
        std::string t;
        if (single) {
          t = "{ const uint64_t d = gc_single_to_double(v); " + g.W(frd) + " = gc_rt_lanes_set(d, d); } ";
        } else {
          const std::string hi = "gc_rt_lanes_hi(" + g.R(frd) + ")";
          t = g.W(frd) + " = gc_rt_lanes_set(v, " + hi + "); ";
        }
        if (update) t += g.W(ra) + " = ea; ";
        return t;
      });
      return Kind::Template;
    }
    case GC_OP_STFS: case GC_OP_STFSU: case GC_OP_STFSX: case GC_OP_STFSUX:
    case GC_OP_STFD: case GC_OP_STFDU: case GC_OP_STFDX: case GC_OP_STFDUX: case GC_OP_STFIWX: {
      const bool single = op == GC_OP_STFS || op == GC_OP_STFSU || op == GC_OP_STFSX || op == GC_OP_STFSUX;
      const bool update = op == GC_OP_STFSU || op == GC_OP_STFSUX || op == GC_OP_STFDU || op == GC_OP_STFDUX;
      const bool indexed = op == GC_OP_STFSX || op == GC_OP_STFSUX || op == GC_OP_STFDX || op == GC_OP_STFDUX ||
                           op == GC_OP_STFIWX;
      const std::string ea = indexed ? (update ? ea_xu(f, w) : ea_x(f, w)) : (update ? ea_du(f, w, simm(w)) : ea_d(f, w, simm(w)));
      const std::string ps0 = "gc_rt_lanes_lo(" + f.R(FPR0 + rs) + ")";
      const std::string after = update ? f.W(ra) + " = ea; " : "";
      if (op == GC_OP_STFIWX) {
        o += store_text(f, ea, 4u, "(uint32_t)" + ps0, after);
      } else if (single) {
        o += store_text(f, ea, 4u, "gc_store_single(" + ps0 + ")", after);
      } else {
        o += store_text(f, ea, 8u, ps0, after);
      }
      return Kind::Template;
    }
    case GC_OP_PSQ_L: case GC_OP_PSQ_LU: case GC_OP_PSQ_LX: case GC_OP_PSQ_LUX:
    case GC_OP_PSQ_ST: case GC_OP_PSQ_STU: case GC_OP_PSQ_STX: case GC_OP_PSQ_STUX: {
      const bool store = op == GC_OP_PSQ_ST || op == GC_OP_PSQ_STU || op == GC_OP_PSQ_STX || op == GC_OP_PSQ_STUX;
      const bool update = op == GC_OP_PSQ_LU || op == GC_OP_PSQ_LUX || op == GC_OP_PSQ_STU || op == GC_OP_PSQ_STUX;
      const bool indexed = op == GC_OP_PSQ_LX || op == GC_OP_PSQ_LUX || op == GC_OP_PSQ_STX || op == GC_OP_PSQ_STUX;
      const uint32_t offset = (uint32_t)GC_F_ps_offset(w);
      const std::string ea = indexed ? (update ? ea_xu(f, w) : ea_x(f, w)) : (update ? ea_du(f, w, offset) : ea_d(f, w, offset));
      const unsigned gqr = indexed ? GC_F_ps_IX(w) : GC_F_ps_I(w);
      const unsigned wbit = indexed ? GC_F_ps_WX(w) : GC_F_ps_W(w);
      const std::string after = update ? f.W(ra) + " = ea; " : "";
      std::string fast;
      if (options.psq_inline) {
        fast = store ? "gc_psq_store_in(c, fms, " : "gc_psq_load_in(c, fml, ";
        if (store) f.store_map = true; else f.load_map = true;
      } else {
        fast = store ? "gc_rt_psq_store_fast(c, " : "gc_psq_load_fast(c, ";
      }
      /* The register goes through the processor: a store reads it there, a
       * load writes it there and the local is read again. */
      const unsigned fr = FPR0 + rd;
      const std::string before = store ? f.flush(bit(fr)) : "";
      const std::string again = store ? "" : f.W(fr) + " = gc_rt_lanes_get(&c->fpr[" + num(rd) + "]); ";
      o += "{ const uint32_t ea = " + ea + "; " + before + "if (!" + fast + hex(w) + ", ea, " + num(gqr) + ", " +
           num(wbit) + ")) { const int st = gc_rt_psq(m, " + hex(w) + ", ea, " + (store ? "1" : "0") + ", " +
           hex(f.address) + ", " + f.cycles() + ", " + f.left() + "); if (st == 1) goto " + f.here() +
           "; if (st) { " + after + again + "goto " + f.next() + "; } } " + after + again + "}";
      return Kind::Template;
    }

    /* ---- branches: the end of the block ---- */
    case GC_OP_B: {
      const uint32_t target = GC_M_AA(w) ? (uint32_t)GC_F_LI(w) : f.address + (uint32_t)GC_F_LI(w);
      if (GC_M_LK(w)) end->before += f.W(LR) + " = " + hex(f.address + 4u) + "; ";
      end->known_target = true;
      end->target_address = target;
      end->fall = f.address + 4u;
      end->link = GC_M_LK(w) != 0;
      return Kind::End;
    }
    case GC_OP_BC: case GC_OP_BCLR: case GC_OP_BCCTR: {
      const uint32_t bo = GC_F_BO(w);
      std::string cond;
      if (op == GC_OP_BCLR) {
        end->before += "const uint32_t target = " + f.R(LR) + " & ~3u; ";
      } else if (op == GC_OP_BCCTR) {
        end->before += "const uint32_t target = " + f.R(CTR) + " & ~3u; ";
      }
      if (op != GC_OP_BCCTR && (bo & 0x04u) == 0) {
        const std::string v = f.R(CTR);
        end->before += f.W(CTR) + " = " + v + " - 1u; ";
        cond = std::string("(") + local_name(CTR) + ((bo & 0x02u) ? " == 0u)" : " != 0u)");
      }
      if ((bo & 0x10u) == 0) {
        const std::string c = "(((" + f.R(CR) + " >> " + num(31 - GC_F_BI(w)) + ") & 1u) == " + num((bo >> 3) & 1u) + ")";
        cond = cond.empty() ? c : cond + " && " + c;
      }
      if (GC_M_LK(w)) end->before += f.W(LR) + " = " + hex(f.address + 4u) + "; ";
      end->condition = cond;
      end->fall = f.address + 4u;
      end->link = GC_M_LK(w) != 0;
      end->ret = op == GC_OP_BCLR && !GC_M_LK(w);
      if (op == GC_OP_BC) {
        end->known_target = true;
        end->target_address = GC_M_AA(w) ? (uint32_t)GC_F_BD(w) : f.address + (uint32_t)GC_F_BD(w);
      } else {
        end->target = "target";
      }
      return Kind::End;
    }

    default:
      break;
  }

  /* ---- the paired steady state on the locals ---- */
  if (options.fpr_locals && !rc) {
    const unsigned fd = FPR0 + GC_F_frD(w), fa = FPR0 + GC_F_frA(w), fb = FPR0 + GC_F_frB(w), fc = FPR0 + GC_F_frC(w);
    std::string kernel, note_b, note_c = "0", kind;
    bool paired = true;
    switch (op) {
      case GC_OP_PS_ADD: case GC_OP_PS_SUB: {
        const std::string A = f.R(fa), B = f.R(fb);
        kernel = options.kernels_inline
                     ? "gc_rt_ps_binary(" + A + ", " + B + ", " + (op == GC_OP_PS_ADD ? "GC_FP_ADD" : "GC_FP_SUB") + ", &res)"
                     : "GC_RT_ACCEPTED(res = gc_rt_ps_binary_call(" + A + ", " + B + ", " +
                           (op == GC_OP_PS_ADD ? "GC_FP_ADD" : "GC_FP_SUB") + "))";
        note_b = "gc_rt_lanes_hi(" + B + ")";
        kind = op == GC_OP_PS_ADD ? "1u" : "2u";
        break;
      }
      case GC_OP_PS_MUL: case GC_OP_PS_MULS0: case GC_OP_PS_MULS1: {
        const std::string A = f.R(fa), C = f.R(fc);
        const std::string lanes = op == GC_OP_PS_MUL ? C : (op == GC_OP_PS_MULS0 ? "gc_rt_lanes_dup_lo(" : "gc_rt_lanes_dup_hi(") + C + ")";
        kernel = options.kernels_inline ? "gc_rt_ps_binary(" + A + ", " + lanes + ", GC_FP_MUL, &res)"
                                        : "GC_RT_ACCEPTED(res = gc_rt_ps_binary_call(" + A + ", " + lanes + ", GC_FP_MUL))";
        note_b = op == GC_OP_PS_MULS0 ? "gc_rt_lanes_lo(" + C + ")" : "gc_rt_lanes_hi(" + C + ")";
        kind = "3u";
        break;
      }
      case GC_OP_PS_MADD: case GC_OP_PS_MSUB: case GC_OP_PS_NMADD: case GC_OP_PS_NMSUB:
      case GC_OP_PS_MADDS0: case GC_OP_PS_MADDS1: {
        const int negate_add = op == GC_OP_PS_MSUB || op == GC_OP_PS_NMSUB;
        const int negate_result = op == GC_OP_PS_NMADD || op == GC_OP_PS_NMSUB;
        const std::string A = f.R(fa), B = f.R(fb), C = f.R(fc);
        const std::string lanes = op == GC_OP_PS_MADDS0   ? "gc_rt_lanes_dup_lo(" + C + ")"
                                  : op == GC_OP_PS_MADDS1 ? "gc_rt_lanes_dup_hi(" + C + ")"
                                                          : C;
        kernel = options.kernels_inline
                     ? "gc_rt_ps_madd(" + A + ", " + B + ", " + lanes + ", " + std::to_string(negate_add) + ", " +
                           std::to_string(negate_result) + ", &res)"
                     : "GC_RT_ACCEPTED(res = gc_rt_ps_madd_call(" + A + ", " + B + ", " + lanes + ", " +
                           std::to_string(2 * negate_add + negate_result) + "u))";
        note_b = "gc_rt_lanes_hi(" + B + ")";
        note_c = op == GC_OP_PS_MADDS0 ? "gc_rt_lanes_lo(" + C + ")" : "gc_rt_lanes_hi(" + C + ")";
        kind = std::to_string(4 + 2 * negate_add + negate_result) + "u";
        break;
      }
      default:
        paired = false;
        break;
    }
    if (paired) {
      const std::string A = f.R(fa);
      const Mask before = f.dirty;
      std::string slow = "{ " + f.flush(bit(fa) | bit(fb) | bit(fc) | bit(fd)) + f.note_flush();
      f.uses_note = true;
      slow += std::string(heavy_fp(op) && !options.fp_inline ? "gc_rt_" : "") + handler_name(op) + "(c, " + hex(w) +
              "); " + f.W(fd) + " = gc_rt_lanes_get(&c->fpr[" + num(GC_F_frD(w)) + "]); " + f.steady_again() + "}";
      f.dirty = before;
      o += "{ GcRtLanes res; if (steady && " + kernel + ") { nk = " + kind + "; na = gc_rt_lanes_hi(" + A +
           "); nb = " + note_b + "; nc = " + note_c + "; nr = gc_rt_lanes_lo(res); nd = 1; " + f.W(fd) +
           " = res; } else " + slow + " }";
      return Kind::Template;
    }
    switch (op) {
      /* The paired moves (GC_PS_MOVE in gc_fpu_impl.h): ps0 rounded in the
       * guest's rounding mode, ps1 truncated. */
      case GC_OP_PS_MR: case GC_OP_PS_NEG: case GC_OP_PS_ABS: case GC_OP_PS_NABS:
      case GC_OP_PS_MERGE00: case GC_OP_PS_MERGE01: case GC_OP_PS_MERGE10: case GC_OP_PS_MERGE11: {
        std::string lane0, lane1;
        if (op == GC_OP_PS_MR || op == GC_OP_PS_NEG || op == GC_OP_PS_ABS || op == GC_OP_PS_NABS) {
          const std::string B = f.R(fb);
          const std::string mask = op == GC_OP_PS_NEG ? " ^ 0x8000000000000000ull"
                                   : op == GC_OP_PS_ABS ? " & 0x7fffffffffffffffull"
                                   : op == GC_OP_PS_NABS ? " | 0x8000000000000000ull" : "";
          lane0 = "(gc_rt_lanes_lo(" + B + ")" + mask + ")";
          lane1 = "(gc_rt_lanes_hi(" + B + ")" + mask + ")";
        } else {
          const std::string A = f.R(fa), B = f.R(fb);
          lane0 = std::string(op == GC_OP_PS_MERGE10 || op == GC_OP_PS_MERGE11 ? "gc_rt_lanes_hi(" : "gc_rt_lanes_lo(") + A + ")";
          lane1 = std::string(op == GC_OP_PS_MERGE01 || op == GC_OP_PS_MERGE11 ? "gc_rt_lanes_hi(" : "gc_rt_lanes_lo(") + B + ")";
        }
        o += f.W(fd) + " = gc_rt_lanes_set(gc_ps_round(c, " + lane0 + "), gc_ps_truncate(" + lane1 + "));";
        return Kind::Template;
      }
      case GC_OP_FMR: case GC_OP_FNEG: case GC_OP_FABS: case GC_OP_FNABS: {
        const std::string B = "gc_rt_lanes_lo(" + f.R(fb) + ")";
        const std::string value = op == GC_OP_FMR    ? B
                                  : op == GC_OP_FNEG ? "(" + B + " ^ 0x8000000000000000ull)"
                                  : op == GC_OP_FABS ? "(" + B + " & 0x7fffffffffffffffull)"
                                                     : "(" + B + " | 0x8000000000000000ull)";
        const std::string hi = "gc_rt_lanes_hi(" + f.R(fd) + ")";
        o += f.W(fd) + " = gc_rt_lanes_set(" + value + ", " + hi + ");";
        return Kind::Template;
      }
      default:
        break;
    }
  }

  /* ---- floating-point arithmetic: its handler, which touches no general register ---- */
  {
    bool touches_cr = false;
    if (fp_register_only(op, w, &touches_cr)) {
      const Mask fields = bit(FPR0 + GC_F_frD(w)) | bit(FPR0 + GC_F_frA(w)) | bit(FPR0 + GC_F_frB(w)) |
                          bit(FPR0 + GC_F_frC(w));
      std::string t = "{ ";
      if (options.fpr_locals) {
        f.mread |= fields;
        f.mwrite |= bit(FPR0 + GC_F_frD(w));
        t += f.flush(fields) + f.note_flush();
      }
      if (touches_cr) {
        f.mread |= bit(CR);
        f.mwrite |= bit(CR);
        t += f.flush(bit(CR));
      }
      const bool out_of_line = !options.fp_inline && heavy_fp(op);
      /* The steady state's fast path inline (gc_ps_binary_lazy), the handler
       * when it declines. */
      const bool lazy = out_of_line && !rc && options.lazy_inline &&
                        (op == GC_OP_PS_ADD || op == GC_OP_PS_SUB || op == GC_OP_PS_MUL || op == GC_OP_PS_MULS0 ||
                         op == GC_OP_PS_MULS1 || op == GC_OP_PS_MADD || op == GC_OP_PS_MSUB ||
                         op == GC_OP_PS_NMADD || op == GC_OP_PS_NMSUB || op == GC_OP_PS_MADDS0 ||
                         op == GC_OP_PS_MADDS1);
      if (lazy) t += std::string("if (!gc_lazy_") + (handler_name(op) + 6) + "(c, " + hex(w) + ")) ";
      t += std::string(out_of_line ? "gc_rt_" : "") + handler_name(op) + "(c, " + hex(w) + "); ";
      if (touches_cr) t += f.reload(bit(CR));
      if (options.fpr_locals) t += f.reload(bit(FPR0 + GC_F_frD(w))) + f.steady_again();
      o += t + "}";
      return Kind::FpCall;
    }
  }

  o += fallback_text(f);
  return Kind::Fallback;
}

double cost_of(Kind kind, GcOp op, const BlockOptions &options) {
  switch (kind) {
    case Kind::FpCall: return (options.fp_inline && heavy_fp(op)) ? 8.0 : (heavy_fp(op) ? 3.0 : 2.0);
    case Kind::Fallback: return 1.0;
    case Kind::End: return 2.0;
    default: break;
  }
  switch (op) {
    case GC_OP_PS_ADD: case GC_OP_PS_SUB: case GC_OP_PS_MUL: case GC_OP_PS_MULS0: case GC_OP_PS_MULS1:
    case GC_OP_PS_MADD: case GC_OP_PS_MSUB: case GC_OP_PS_NMADD: case GC_OP_PS_NMSUB: case GC_OP_PS_MADDS0:
    case GC_OP_PS_MADDS1:
      return options.kernels_inline ? 10.0 : 3.0;
    case GC_OP_PSQ_L: case GC_OP_PSQ_LU: case GC_OP_PSQ_LX: case GC_OP_PSQ_LUX: return 4.0;
    case GC_OP_PSQ_ST: case GC_OP_PSQ_STU: case GC_OP_PSQ_STX: case GC_OP_PSQ_STUX: return options.fp_inline ? 8.0 : 2.0;
    case GC_OP_LMW: case GC_OP_STMW: return 4.0;
    default: return 1.5;
  }
}

}  // namespace

BlockCode emit_block(const Image &image, const std::vector<Block> &trace,
                     const std::function<bool(uint32_t)> &has_block, const BlockOptions &options) {
  BlockCode code;
  const Block &block = trace.front();
  char name[64];
  snprintf(name, sizeof(name), "gc_block_%08x", block.start);

  struct Insn {
    uint32_t address, word;
    GcOp op;
  };
  std::vector<Insn> insns;
  unsigned first_count = 0; /* instructions of the first block */
  uint32_t trace_end = block.end;
  for (size_t b = 0; b < trace.size(); b++) {
    bool whole = true;
    for (uint32_t address = trace[b].start; address < trace[b].end; address += 4u) {
      uint32_t word = 0;
      if (!image.word(address, &word)) {
        whole = false;
        break;
      }
      const GcOp op = gc_decode(word);
      if (!handler_name(op)) {
        whole = false;
        break;
      }
      insns.push_back({address, word, op});
    }
    if (b == 0) first_count = (unsigned)insns.size();
    trace_end = trace[b].end;
    if (!whole) break;
  }
  const unsigned count = (unsigned)insns.size();
  code.instructions = count;
  if (!count) return code;
  const uint32_t end_address = block.start + 4u * count;

  /* First pass: what each instruction does with state, and what it costs. */
  struct Plan {
    Kind kind;
    Mask lread, lwrite, mread, mwrite;
    bool load_map, store_map, note;
    double cost;
  };
  std::vector<Plan> plans(count);
  for (unsigned i = 0; i < count; i++) {
    Fn f;
    EndInfo end;
    f.count = count;
    f.index = i;
    f.address = insns[i].address;
    f.word = insns[i].word;
    f.cached = ALL;
    Kind kind = write_insn(f, insns[i].op, options, &end);
    plans[i] = {kind, f.lread, f.lwrite, f.mread, f.mwrite, f.load_map, f.store_map, f.uses_note,
                cost_of(kind, insns[i].op, options)};
    if (kind == Kind::Template || kind == Kind::End) code.written_out++;
  }

  bool self_target = false;
  {
    Fn f;
    EndInfo end;
    f.count = count;
    f.index = count - 1;
    f.address = insns[count - 1].address;
    f.word = insns[count - 1].word;
    f.cached = ALL;
    if (write_insn(f, insns[count - 1].op, options, &end) == Kind::End) {
      self_target = trace.size() == 1 && end.known_target && end.target_address == block.start;
    }
  }

  /* Parts: [from, to) ranges. */
  std::vector<std::pair<unsigned, unsigned>> parts;
  {
    unsigned from = 0;
    double cost = 0.0;
    for (unsigned i = 0; i < count; i++) {
      const bool full = (options.part_cost && cost + plans[i].cost > options.part_cost) ||
                        (options.part_insns && i - from >= options.part_insns);
      if (i > from && full) {
        parts.push_back({from, i});
        from = i;
        cost = 0.0;
      }
      cost += plans[i].cost;
    }
    parts.push_back({from, count});
  }

  /* Written last part first, so each part is defined before the one that
   * calls it. */
  std::string text;
  for (size_t p = parts.size(); p-- > 0;) {
    const unsigned from = parts[p].first, to = parts[p].second;
    std::string fname = p == 0 ? std::string(name) : std::string(name) + "_" + std::to_string(p);
    Mask cached;
    bool maps = false, note = false;
    for (unsigned i = from; i < to; i++) {
      cached |= plans[i].lread | plans[i].lwrite;
      maps = maps || plans[i].load_map || plans[i].store_map;
      note = note || plans[i].note;
    }
    /* A block that branches back to its own first instruction loops inside
     * its function while the next iteration is clear of events, with its
     * registers kept in the locals. */
    const bool loop = options.loops && parts.size() == 1 && plans[count - 1].kind == Kind::End && self_target;
    Mask written;
    for (unsigned i = from; i < to; i++) written |= plans[i].lwrite;
    /* Loaded on entry: the registers whose first use in the part is a read
     * of the local. A first write needs nothing; neither does a first use by
     * a handler, which is followed by a reload. A loop loads them all: an
     * exit in a later iteration writes back what the earlier one wrote. */
    Mask load;
    std::vector<Mask> first_write(to - from + 1);
    for (unsigned r = 0; r < NREG; r++) {
      if (!(cached & bit(r))) continue;
      for (unsigned i = from; i < to; i++) {
        if (plans[i].lread & bit(r)) {
          load |= bit(r);
          break;
        }
        /* A reload counts only when it always happens: the handler of a
         * load or store multiple runs on its slow path alone. */
        const bool always_reloads = plans[i].kind == Kind::Fallback || plans[i].kind == Kind::FpCall;
        if ((plans[i].lwrite | (always_reloads ? plans[i].mwrite : Mask())) & bit(r)) break;
      }
      for (unsigned i = from; i < to; i++) {
        if (plans[i].lwrite & bit(r)) {
          first_write[i - from] |= bit(r);
          break;
        }
      }
    }
    if (loop) {
      load = cached;
      std::fill(first_write.begin(), first_write.end(), Mask());
    }

    std::string body;
    if (p == 0) {
      body += "void *" + fname + "(GcMachine *m) {\n  GcCpu *const c = &m->cpu;\n  " + (loop ? "" : "const ") +
              "uint64_t cyc = c->cycles;\n";
    } else {
      body += "GC_PART " + fname + "(GcMachine *m) {\n  GcCpu *const c = &m->cpu;\n  const uint64_t cyc = c->cycles - " +
              num(from) + ";\n";
    }
    for (unsigned r = 0; r < NREG; r++) {
      if (!(cached & bit(r))) continue;
      if (is_fpr(r)) {
        body += "  GcRtLanes " + local_name(r);
        if (load & bit(r)) body += " = gc_rt_lanes_get(&" + memory_name(r) + ")";
      } else {
        body += "  uint32_t " + local_name(r);
        if (load & bit(r)) body += " = " + memory_name(r);
      }
      body += ";\n";
    }
    if (maps) body += "  const GcFastMap *fml, *fms;\n";
    if (note) body += "  uint8_t nk = 0; uint64_t na = 0, nb = 0, nc = 0, nr = 0; int nd = 0, steady;\n";
    if (p == 0) {
      /* A function with a floating-point instruction in it runs only with
       * the floating-point unit on; with it off the interpreter takes the
       * block, and raises the exception the instruction asks for
       * (gc_cpu_ops.h, gc_insn_needs_fp). */
      bool needs_fp = false;
      for (unsigned i = 0; i < count; i++) needs_fp = needs_fp || gc_insn_needs_fp(insns[i].word);
      if (needs_fp) {
        body += "  if (GC_RT_COLD(!(c->msr & GC_MSR_FP))) return gc_rt_enter_stepped(m, " + hex(block.start) + ", " +
                num(first_count) + ");\n";
      }
      body += "  if (GC_RT_COLD(cyc + " + num(count) + " > c->next_event)) return gc_rt_enter(m, " + hex(block.start) +
              ", " + num(count) + ", " + num(first_count) + ", " + fname + ");\n";
    }
    if (maps) body += "  fml = GC_RT_LOAD_MAP(c); fms = GC_RT_STORE_MAP(c, fml);\n";
    if (note) body += "  steady = GC_RT_STEADY(c);\n";
    if (loop) body += "top:\n";

    Fn f;
    f.loop = loop;
    f.loop_written = written;
    f.count = count;
    f.part_start = from;
    f.cached = cached;
    f.maps = maps;
    f.note = note;
    std::string tail; /* the end of the function: write-back and exit */
    bool ended = false;
    for (unsigned i = from; i < to; i++) {
      f.index = i;
      f.address = insns[i].address;
      f.word = insns[i].word;
      f.lread = f.lwrite = f.mread = f.mwrite = Mask();
      f.uses_note = false;
      f.out.clear();
      EndInfo end;
      char disasm[96];
      snprintf(disasm, sizeof(disasm), "%08x %08x %s", insns[i].address, insns[i].word, gc_op_info[insns[i].op].name);
      const Kind kind = write_insn(f, insns[i].op, options, &end);
      body += "  /* " + std::string(disasm) + " */\n";
      if (kind != Kind::End) {
        body += "  " + f.out + "\n";
        continue;
      }
      auto go = [&](const std::string &target, bool known, uint32_t address) {
        std::string s = "c->pc = c->npc = " + target + "; return ";
        if (known && has_block(address)) {
          char next[64];
          snprintf(next, sizeof(next), "gc_block_%08x", address);
          s += std::string("(void *)") + next + "; ";
          code.links.insert(address);
        } else {
          s += "0; ";
        }
        return s;
      };
      if (i + 1 < count) {
        /* A branch inside the function: the block it ended is followed here
         * by the one it falls into, so only a taken branch leaves. */
        std::string t = "  { " + end.before;
        if (!end.condition.empty()) t += "const int taken = " + end.condition + "; if (taken) ";
        const Mask dirty = f.dirty;
        t += "{ " + f.flush(ALL) + f.note_flush() + "c->cycles = cyc + " + num(i + 1) + "; " +
             go(end.known_target ? hex(end.target_address) : end.target, end.known_target, end.target_address) + "} ";
        f.dirty = dirty;
        body += t + "}\n";
        continue;
      }
      /* The branch that ends the block. */
      ended = true;
      std::string t = "  { " + end.before;
      if (!end.condition.empty()) t += "const int taken = " + end.condition + "; ";
      t += f.flush(ALL) + f.note_flush();
      t += "c->cycles = cyc + " + num(count) + "; ";
      const std::string target = end.known_target ? hex(end.target_address) : end.target;
      std::string call;
      if (end.link && options.returns) {
        call = "GC_RT_CALL(" + hex(end.fall) + ", ";
        if (has_block(end.fall)) {
          char next[64];
          snprintf(next, sizeof(next), "gc_block_%08x", end.fall);
          call += next;
          code.links.insert(end.fall);
        } else {
          call += "0";
        }
        call += "); ";
      }
      auto taken_go = [&]() -> std::string {
        if (end.ret && options.returns) return "c->pc = c->npc = target; return gc_rt_return(target); ";
        return call + go(target, end.known_target, end.target_address);
      };
      if (loop) {
        /* Around again while the next iteration is clear of events;
         * otherwise to the dispatcher, whose next block is this one and whose
         * entry check does what is due. */
        const std::string back = "cyc += " + num(count) + "; if (cyc + " + num(count) +
                                 " <= c->next_event) goto top; " + f.flush(ALL) + f.note_flush() +
                                 "c->cycles = cyc; c->pc = c->npc = " +
                                 hex(block.start) + "; return (void *)" + std::string(name) + "; ";
        t = "  { " + end.before;
        if (!end.condition.empty()) t += "const int taken = " + end.condition + "; ";
        t += (end.condition.empty() ? back : "if (taken) { " + back + "} ");
        if (!end.condition.empty()) {
          t += f.flush(ALL) + f.note_flush() + "c->cycles = cyc + " + num(count) + "; " + go(hex(end.fall), true, end.fall);
        }
        body += t + "}\n";
        continue;
      }
      if (end.condition.empty()) {
        t += taken_go();
      } else {
        t += "if (taken) { " + taken_go() + "} ";
        t += go(hex(end.fall), true, end.fall);
      }
      body += t + "}\n";
    }
    if (!ended) {
      std::string t = "  " + f.flush(ALL) + f.note_flush();
      if (to < count) {
        t += "c->cycles = cyc + " + num(to) + "; return " + std::string(name) + "_" + std::to_string(p + 1) + "(m);\n";
      } else {
        t += "c->cycles = cyc + " + num(count) + "; c->pc = c->npc = " + hex(end_address) + "; return ";
        if (end_address == trace_end && has_block(end_address)) {
          char next[64];
          snprintf(next, sizeof(next), "gc_block_%08x", end_address);
          t += std::string("(void *)") + next + ";\n";
          code.links.insert(end_address);
        } else {
          t += "0;\n";
        }
      }
      body += t;
    }
    /* The exits, from the last instruction down: x_k writes back what the
     * instructions before k wrote, and falls into x_{k-1}. */
    for (unsigned k = to; k > from; k--) {
      body += "x_" + std::to_string(k) + ":";
      const Mask regs = first_write[k - 1 - from];
      for (unsigned r = 0; r < NREG; r++) {
        if (regs & bit(r)) body += " " + put_text(r);
      }
      body += "\n";
    }
    body += "x_" + std::to_string(from) + ":";
    if (loop) {
      for (unsigned r = 0; r < NREG; r++) {
        if (written & bit(r)) body += " " + put_text(r);
      }
    }
    body += "\nx_nf:\n  " + std::string(note ? NOTE_FLUSH : "") + "return gc_rt_exit(m, " + hex(end_address) + ");\n}\n\n";
    text += body;
    for (unsigned i = from; i < to; i++) code.cost += plans[i].cost;
    code.cost += 2.0;
  }
  code.text = text;
  return code;
}

}  // namespace gcrecomp
