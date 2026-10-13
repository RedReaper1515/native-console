#include "emitter.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "gc_cpu_decode.h"
#include "gc_cpu_ops.h"
}

namespace gcrecomp {
namespace {

/* The handler for an opcode, by the same list the interpreter dispatches
 * from - so the emitter cannot name a function the interpreter does not
 * call, or miss one it does. */
const char *handler_of(GcOp op) {
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

bool write_file(const std::string &path, const std::string &text, std::string *error) {
  FILE *file = fopen(path.c_str(), "wb");
  if (!file) {
    *error = "cannot write " + path;
    return false;
  }
  fwrite(text.data(), 1, text.size(), file);
  fclose(file);
  return true;
}

std::string hex(uint32_t value) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "0x%08x", value);
  return buffer;
}

/* The most blocks one translation unit holds, whatever they cost.
 *
 * A commercial game has tens of thousands, and a single file holding all
 * of them exceeds what an object file can describe - MSVC stops with
 * "number of sections exceeded object file format limit", which is a
 * limit nothing smaller than a real game reaches, so the test programs
 * never found it. */
#define GC_MAX_BLOCKS_PER_FILE 2048

/* What one instruction costs to compile, in the units the file split below
 * balances.
 *
 * Measured with MSVC /O2 /fp:strict (docs/GC-PLAN.md, "How long a game
 * takes to build"): once functions are short, time is close to linear in
 * the instructions, but not the same for every instruction. The floating
 * point arithmetic - the paired-single and scalar add, subtract, multiply,
 * divide and fused forms, the compares, the conversions and the quantised
 * stores - inline the whole IEEE flag model, and each costs the optimiser
 * several times what a load, a branch or an integer operation does. A file
 * split by count put those in a few files, and those files were the build's
 * wall time. */
double insn_cost(const char *handler) {
  static const char *const HEAVY[] = {
      "gc_op_ps_add",   "gc_op_ps_sub",     "gc_op_ps_mul",     "gc_op_ps_div",
      "gc_op_ps_madd",  "gc_op_ps_madds0",  "gc_op_ps_madds1",  "gc_op_ps_msub",
      "gc_op_ps_nmsub", "gc_op_ps_nmadd",   "gc_op_ps_sum0",    "gc_op_ps_sum1",
      "gc_op_ps_muls0", "gc_op_ps_muls1",   "gc_op_ps_res",     "gc_op_ps_rsqrte",
      "gc_op_fadd",     "gc_op_fadds",      "gc_op_fsub",       "gc_op_fsubs",
      "gc_op_fmul",     "gc_op_fmuls",      "gc_op_fdiv",       "gc_op_fdivs",
      "gc_op_fmadd",    "gc_op_fmadds",     "gc_op_fmsub",      "gc_op_fmsubs",
      "gc_op_fnmadd",   "gc_op_fnmadds",    "gc_op_fnmsub",     "gc_op_fnmsubs",
      "gc_op_fsqrt",    "gc_op_fsqrts",     "gc_op_fres",       "gc_op_frsqrte",
      "gc_op_fcmpu",    "gc_op_fcmpo",      "gc_op_frsp",       "gc_op_fctiw",
      "gc_op_fctiwz",   "gc_op_psq_st",     "gc_op_psq_stu",    "gc_op_psq_stx",
      "gc_op_psq_stux",
  };
  for (const char *heavy : HEAVY) {
    if (!strcmp(handler, heavy)) return 8.0;
  }
  return 1.0;
}

/* Every function also costs something for itself: its frame, its entry,
 * the call to the next part. */
constexpr double GC_FUNCTION_COST = 1.0;

/* The cost one file is aimed at when the caller did not choose a count.
 * Each file also pays a fixed price - reading and optimising the runtime's
 * inline headers, a second or two - so a game should be in rather more
 * pieces than a machine has cores, not hundreds. */
constexpr double GC_COST_PER_FILE = 10000.0;
constexpr unsigned GC_MAX_FILES = 256;

/* A wait loop: a block that branches back to its own first instruction and,
 * run a second time, provably leaves the machine exactly as the first run
 * did.
 *
 * Games wait for an interrupt handler to set a flag by reading it over and
 * over - Shrek 2 spends 28 % of a run in three instructions, `lwz r0,
 * flag(r13); cmplwi r0, 0; beq` - and each iteration is a trip through the
 * dispatcher. When nothing can happen until cpu.next_event, every one of
 * those iterations reads the same memory into the same registers, so the
 * generated code runs the loop once, as always, and then advances the count
 * by the whole iterations that fit before next_event. What would have run
 * after that - the instructions at which something is due - runs as
 * before, one at a time with its check. The argument is in
 * gc_machine_idle_iterations (gc.c); this function decides which loops it
 * applies to, and it only has to be right in one direction: a loop it
 * refuses simply runs.
 *
 * A second iteration repeats the first when nothing it reads was written by
 * the first. So for each instruction the state it reads and writes is
 * tracked - the general registers, the condition register a field at a
 * time, XER - and the loop qualifies when no state is both read before any
 * write to it and written. Only instructions whose whole effect is known
 * and that cannot raise or reach a device are allowed: loads (whose
 * addresses the generated code also checks are plain main memory, see
 * GC_IDLE_LOAD), compares, register arithmetic without carry, and a
 * branch back that neither links nor counts. */
struct IdleLoad {
  size_t index;      /* which instruction */
  bool indexed;      /* X form: rA|0 + rB, rather than rA|0 + d */
  unsigned size;     /* bytes */
};

constexpr size_t GC_IDLE_MAX_INSNS = 16;

bool idle_loop_shape(const std::vector<uint32_t> &words, uint32_t start,
                     std::vector<IdleLoad> *loads) {
  enum : unsigned { CR0 = 32, XER = 40 };
  const auto gpr = [](unsigned r) { return (uint64_t)1 << r; };
  const auto crf = [](unsigned f) { return (uint64_t)1 << (CR0 + f); };
  const uint64_t xer = (uint64_t)1 << XER;
  uint64_t read_first = 0, written = 0;
  if (words.empty() || words.size() > GC_IDLE_MAX_INSNS) return false;
  loads->clear();
  for (size_t i = 0; i < words.size(); i++) {
    const uint32_t w = words[i];
    const GcOp op = gc_decode(w);
    const unsigned rd = GC_F_rD(w), ra = GC_F_rA(w), rb = GC_F_rB(w);
    uint64_t reads = 0, writes = 0;
    const bool last = (i + 1 == words.size());
    const auto base = [&](unsigned r) { return r ? gpr(r) : 0u; };
    const auto rc = [&]() {
      if (GC_M_Rc(w)) {
        reads |= xer; /* SO is copied into CR0 */
        writes |= crf(0);
      }
    };
    switch (op) {
      case GC_OP_LBZ: case GC_OP_LHZ: case GC_OP_LHA: case GC_OP_LWZ:
        reads = base(ra);
        writes = gpr(rd);
        loads->push_back({i, false, op == GC_OP_LBZ ? 1u : op == GC_OP_LWZ ? 4u : 2u});
        break;
      case GC_OP_LBZX: case GC_OP_LHZX: case GC_OP_LHAX: case GC_OP_LWZX:
        reads = base(ra) | gpr(rb);
        writes = gpr(rd);
        loads->push_back({i, true, op == GC_OP_LBZX ? 1u : op == GC_OP_LWZX ? 4u : 2u});
        break;
      case GC_OP_CMP: case GC_OP_CMPL:
        reads = gpr(ra) | gpr(rb) | xer;
        writes = crf(GC_F_crfD(w));
        break;
      case GC_OP_CMPI: case GC_OP_CMPLI:
        reads = gpr(ra) | xer;
        writes = crf(GC_F_crfD(w));
        break;
      case GC_OP_ADDI: case GC_OP_ADDIS:
        reads = base(ra);
        writes = gpr(rd);
        break;
      case GC_OP_ORI: case GC_OP_ORIS: case GC_OP_XORI: case GC_OP_XORIS:
        reads = gpr(GC_F_rS(w));
        writes = gpr(ra);
        break;
      case GC_OP_ANDI_: case GC_OP_ANDIS_:
        reads = gpr(GC_F_rS(w)) | xer;
        writes = gpr(ra) | crf(0);
        break;
      case GC_OP_RLWINM: case GC_OP_EXTSB: case GC_OP_EXTSH: case GC_OP_CNTLZW:
        reads = gpr(GC_F_rS(w));
        writes = gpr(ra);
        rc();
        break;
      case GC_OP_AND: case GC_OP_ANDC: case GC_OP_OR: case GC_OP_ORC: case GC_OP_XOR:
      case GC_OP_NAND: case GC_OP_NOR: case GC_OP_EQV: case GC_OP_SLW: case GC_OP_SRW:
      case GC_OP_RLWNM:
        reads = gpr(GC_F_rS(w)) | gpr(rb);
        writes = gpr(ra);
        rc();
        break;
      case GC_OP_B: {
        if (!last || GC_M_LK(w)) return false;
        const uint32_t target = GC_M_AA(w) ? (uint32_t)GC_F_LI(w)
                                           : start + 4u * (uint32_t)i + (uint32_t)GC_F_LI(w);
        if (target != start) return false;
        break;
      }
      case GC_OP_BC: {
        const uint32_t bo = GC_F_BO(w);
        if (!last || GC_M_LK(w) || (bo & 0x04u) == 0) return false; /* links, or counts */
        const uint32_t target = GC_M_AA(w) ? (uint32_t)GC_F_BD(w)
                                           : start + 4u * (uint32_t)i + (uint32_t)GC_F_BD(w);
        if (target != start) return false;
        if ((bo & 0x10u) == 0) reads = crf(GC_F_BI(w) / 4u);
        break;
      }
      default:
        return false;
    }
    read_first |= reads & ~written;
    written |= writes;
    /* Only the last instruction may leave the block. */
    if (!last && (op == GC_OP_B || op == GC_OP_BC)) return false;
    if (last && op != GC_OP_B && op != GC_OP_BC) return false;
  }
  return (read_first & written) == 0;
}

/* Where a wait loop starting at blocks[at] ends, or 0 if none starts there.
 *
 * Usually the loop is the block. But an interrupt returns into the middle
 * of a wait loop - that is where the program was waiting - so a trace of a
 * run names the loop's second and third instructions as entry points, and
 * as roots they cut the loop into blocks of one instruction each, none of
 * which branches back to its own start. A branch into the loop from outside
 * does the same without any trace (Shrek 2 has six). So the instructions of the blocks
 * that follow on without a gap are read too, up to the first branch. When
 * those form a wait loop, the block at its head is emitted as the whole
 * loop; the blocks inside it stay as they are, for a run that enters there,
 * and go to the head through the dispatcher like any other block. The
 * head's code is then exactly what it is without the trace, and blocks
 * that share addresses are harmless: the dispatcher finds a block by where
 * it starts. */
uint32_t idle_loop_end(const Image &image, const std::vector<Block> &blocks, size_t at,
                       std::vector<uint32_t> *words, std::vector<IdleLoad> *loads) {
  uint32_t address = blocks[at].start;
  words->clear();
  for (size_t i = at; i < blocks.size() && blocks[i].start == address; i++) {
    for (; address < blocks[i].end; address += 4u) {
      uint32_t insn = 0;
      if (words->size() == GC_IDLE_MAX_INSNS || !image.word(address, &insn) ||
          !handler_of(gc_decode(insn))) {
        return 0;
      }
      words->push_back(insn);
      const GcOp op = gc_decode(insn);
      if (op == GC_OP_B || op == GC_OP_BC) {
        return idle_loop_shape(*words, blocks[at].start, loads) ? address + 4u : 0;
      }
    }
  }
  return 0;
}

bool copy_tree(const std::filesystem::path &from, const std::filesystem::path &to,
               std::string *error) {
  std::error_code ec;
  std::filesystem::create_directories(to, ec);
  std::filesystem::copy(from, to,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing,
                        ec);
  if (ec) {
    *error = "cannot copy " + from.string() + " to " + to.string() + ": " + ec.message();
    return false;
  }
  return true;
}

/* The runtime, copied into the project so that the project builds on its
 * own - moved, exported, or with the checkout gcrecomp came from long
 * gone. Only what a game links: the library sources, the headers, the
 * frontend and Dear ImGui for its menu. The test tools stay behind. */
bool snapshot_runtime(const Options &options, std::string *error) {
  namespace fs = std::filesystem;
  const fs::path from(options.runtime_dir);
  const fs::path to = fs::path(options.out_dir) / "runtime-gc";
  if (!fs::is_regular_file(from / "include" / "gc.h")) {
    *error = "no GameCube runtime in " + from.string();
    return false;
  }
  std::error_code ec;
  fs::remove_all(to, ec);
  for (const char *sub : {"include", "src", "platform"}) {
    if (!copy_tree(from / sub, to / sub, error)) return false;
  }
  if (fs::is_directory(from / "cmake") && !copy_tree(from / "cmake", to / "cmake", error)) {
    return false;
  }
  fs::copy_file(from / "CMakeLists.txt", to / "CMakeLists.txt",
                fs::copy_options::overwrite_existing, ec);
  if (ec) {
    *error = "cannot copy " + (from / "CMakeLists.txt").string() + ": " + ec.message();
    return false;
  }
  /* Dear ImGui: one already in a runtime snapshot or a release, or the
   * copy vendored with the Game Boy runtime in the source tree. */
  fs::path imgui = from / "vendor" / "imgui";
  if (!fs::is_regular_file(imgui / "imgui.h")) imgui = from.parent_path() / "runtime" / "vendor" / "imgui";
  if (!fs::is_regular_file(imgui / "imgui.h")) {
    *error = "Dear ImGui not found beside " + from.string();
    return false;
  }
  return copy_tree(imgui, to / "vendor" / "imgui", error);
}

}  // namespace

const char *generated_prologue() {
  return
      "/* Generated by gcrecomp. Do not edit.\n"
      " *\n"
      " * One function per basic block (recompiler-gc/src/block.cpp): the guest\n"
      " * registers a block uses in C locals, the common instructions written out\n"
      " * against them, everything else through the interpreter's handler. A\n"
      " * block answers the block that runs next when it knows it.\n"
      " */\n"
      "#include \"gc.h\"\n"
      "#include \"gc_bus.h\"\n"
      "#include \"gc_cpu.h\"\n"
      "#include \"gc_cpu_impl.h\"\n"
      "#include \"gc_fpu_impl.h\"\n"
      "#include \"gc_recompiled_rt.h\"\n"
      "#include \"recompiled.h\"\n"
      "\n"
      "#if defined(_MSC_VER)\n"
      "#pragma warning(disable : 4102) /* an exit label no instruction needed */\n"
      "#endif\n"
      "\n"
      "/* The rest of a long block (block.cpp, \"Parts\"): never inlined back,\n"
      " * because the optimiser's time grows far faster than the function. */\n"
      "#define GC_PART static GC_RT_NOINLINE void *\n"
      "\n"
      "/* A wait loop (idle_loop_shape in the emitter) keeps the per-instruction\n"
      " * form: one instruction, its handler, the check before it. After one\n"
      " * iteration that went back to the start with nothing due and every load\n"
      " * in plain main memory, the iterations that fit before next_event are\n"
      " * counted rather than run. gc_machine_idle_until in gc.c has the\n"
      " * argument. */\n"
      "#define GC_INSN_BODY(address, word, handler)                   \\\n"
      "  if (c->cycles >= c->next_event) {                           \\\n"
      "    if (gc_machine_due(m)) return 0;                          \\\n"
      "    if (c->pc != (address)) return 0;                         \\\n"
      "  }                                                           \\\n"
      "  c->npc = (address) + 4u;                                    \\\n"
      "  c->branched = 0;                                            \\\n"
      "  handler(c, (word));                                         \\\n"
      "  if (c->pending_vector) gc_cpu_exception(c, c->pending_vector); \\\n"
      "  c->pc = c->npc;                                             \\\n"
      "  c->cycles += 1\n"
      "#define GC_INSN(address, word, handler)                        \\\n"
      "  GC_INSN_BODY(address, word, handler);                       \\\n"
      "  if (c->branched || c->stopped) return 0\n"
      "#define GC_IDLE_ENTRY                                          \\\n"
      "  const uint64_t idle_from = c->cycles;                       \\\n"
      "  const uint64_t idle_event = c->next_event;                  \\\n"
      "  int idle_fast = 1\n"
      "#define GC_IDLE_LOAD(ea, size) idle_fast &= GC_BUS_FAST_OK(c, (ea), (size)) ? 1 : 0\n"
      "#define GC_IDLE_SKIP(start, count)                             \\\n"
      "  if (c->branched && !c->stopped && c->npc == (start) && idle_fast && \\\n"
      "      c->next_event == idle_event && idle_from + (count) <= idle_event) \\\n"
      "    gc_machine_idle_until(c, idle_event, (count))\n"
      "\n";
}

bool emit_project(const Image &image, const Analysis &analysis, const Options &options,
                  std::string *error) {
  const char *const PROLOGUE = generated_prologue();
  std::string declarations;
  std::string table;
  std::vector<std::string> sources;

  /* Every block as C first, with what it costs to compile, and then into
   * files by that cost - not by a count of blocks, because a block's cost
   * depends on its length and the file holding the longest ones used to be
   * the whole build's wall time. */
  struct Unit {
    std::string text;
    double cost = 0.0;
    std::set<uint32_t> links;
  };
  std::vector<Unit> units;
  units.reserve(analysis.blocks.size());
  double total_cost = 0.0;
  unsigned idle_loops = 0;
  unsigned split_idle_loops = 0; /* of those, loops that roots had cut into blocks */
  unsigned instructions = 0, written_out = 0;

  /* Which blocks something enters other than by falling into them: a root
   * (the entry, a vector, an address the program stores, a trace), a branch
   * target, the return address of a call. A block that is none of those,
   * right after a block that ends in a conditional branch without a link,
   * is written into its predecessor's function (block.cpp) and has no
   * function of its own - an address inside it reaches the interpreter, as
   * any address with no block does. */
  std::set<uint32_t> entered(analysis.roots.begin(), analysis.roots.end());
  std::vector<bool> falls_in(analysis.blocks.size(), false); /* block i+1 is entered only from block i */
  std::vector<bool> idle_head(analysis.blocks.size(), false);
  {
    std::vector<uint32_t> scratch;
    std::vector<IdleLoad> scratch_loads;
    for (size_t i = 0; i < analysis.blocks.size(); i++) {
      idle_head[i] = idle_loop_end(image, analysis.blocks, i, &scratch, &scratch_loads) != 0;
    }
  }
  for (const Block &block : analysis.blocks) {
    uint32_t last = 0;
    if (block.end <= block.start || !image.word(block.end - 4u, &last)) continue;
    const GcOp op = gc_decode(last);
    const uint32_t at = block.end - 4u;
    if (op == GC_OP_B) entered.insert(GC_M_AA(last) ? (uint32_t)GC_F_LI(last) : at + (uint32_t)GC_F_LI(last));
    if (op == GC_OP_BC) entered.insert(GC_M_AA(last) ? (uint32_t)GC_F_BD(last) : at + (uint32_t)GC_F_BD(last));
    if ((op == GC_OP_B || op == GC_OP_BC || op == GC_OP_BCLR || op == GC_OP_BCCTR) && GC_M_LK(last)) {
      entered.insert(block.end);
    }
  }
  for (size_t i = 0; i + 1 < analysis.blocks.size(); i++) {
    const Block &from = analysis.blocks[i], &to = analysis.blocks[i + 1];
    uint32_t last = 0;
    if (from.end != to.start || entered.count(to.start) || idle_head[i] || idle_head[i + 1]) continue;
    if (!image.word(from.end - 4u, &last)) continue;
    const GcOp op = gc_decode(last);
    if ((op == GC_OP_BC || op == GC_OP_BCLR || op == GC_OP_BCCTR) && !GC_M_LK(last)) falls_in[i] = true;
  }
  std::set<uint32_t> starts;   /* blocks with a function of their own */
  std::vector<bool> merged(analysis.blocks.size(), false);
  std::vector<std::vector<Block>> traces(analysis.blocks.size());
  for (size_t i = 0; i < analysis.blocks.size(); i++) {
    if (merged[i]) continue;
    traces[i].push_back(analysis.blocks[i]);
    starts.insert(analysis.blocks[i].start);
    uint32_t length = (analysis.blocks[i].end - analysis.blocks[i].start) / 4u;
    for (size_t j = i; options.block.trace_insns && j + 1 < analysis.blocks.size() && falls_in[j] &&
                       traces[i].size() < 16u;
         j++) {
      const uint32_t more = (analysis.blocks[j + 1].end - analysis.blocks[j + 1].start) / 4u;
      if (length + more > options.block.trace_insns) break;
      traces[i].push_back(analysis.blocks[j + 1]);
      merged[j + 1] = true;
      length += more;
    }
  }
  const std::function<bool(uint32_t)> has_block = [&](uint32_t address) { return starts.count(address) != 0; };
  unsigned merged_count = 0;

  for (size_t block_index = 0; block_index < analysis.blocks.size(); block_index++) {
    if (merged[block_index]) {
      merged_count++;
      continue;
    }
    const Block &block = analysis.blocks[block_index];
    char name[64];
    snprintf(name, sizeof(name), "gc_block_%08x", block.start);
    std::vector<uint32_t> words;
    std::vector<IdleLoad> loads;
    const uint32_t loop_end = idle_loop_end(image, analysis.blocks, block_index, &words, &loads);
    if (loop_end) {
      /* One function whatever its length: a wait loop is a handful of
       * loads and compares, which cost the optimiser nothing. */
      Unit unit;
      char tail[160];
      unit.text += "void *" + std::string(name) + "(GcMachine *m) {\n";
      unit.text += "  GcCpu *c = &m->cpu;\n  GC_IDLE_ENTRY;\n";
      size_t next_load = 0;
      for (size_t i = 0; i < words.size(); i++) {
        const uint32_t address = block.start + 4u * (uint32_t)i;
        const char *handler = handler_of(gc_decode(words[i]));
        if (next_load < loads.size() && loads[next_load].index == i) {
          char check[160];
          snprintf(check, sizeof(check), "  GC_IDLE_LOAD(%s(c, %su), %uu);\n",
                   loads[next_load].indexed ? "gc_ea_x" : "gc_ea_d", hex(words[i]).c_str(),
                   loads[next_load].size);
          unit.text += check;
          next_load++;
        }
        unit.text += std::string(i + 1 < words.size() ? "  GC_INSN(" : "  GC_INSN_BODY(") +
                     hex(address) + "u, " + hex(words[i]) + "u, " + handler + ");\n";
        unit.cost += insn_cost(handler);
      }
      snprintf(tail, sizeof(tail), "  GC_IDLE_SKIP(%su, %uu);\n  return 0;\n}\n\n", hex(block.start).c_str(),
               (unsigned)words.size());
      unit.text += tail;
      unit.cost += GC_FUNCTION_COST;
      total_cost += unit.cost;
      units.push_back(std::move(unit));
      idle_loops++;
      if (loop_end != block.end) split_idle_loops++;
      declarations += "void *" + std::string(name) + "(GcMachine *m);\n";
      table += "  {" + hex(block.start) + "u, " + hex(loop_end) + "u, " + name + "},\n";
      continue;
    }

    {
      BlockCode code = emit_block(image, traces[block_index], has_block, options.block);
      Unit unit;
      unit.text = std::move(code.text);
      unit.cost = code.cost;
      unit.links = std::move(code.links);
      instructions += code.instructions;
      written_out += code.written_out;
      total_cost += unit.cost;
      units.push_back(std::move(unit));
    }
    declarations += "void *" + std::string(name) + "(GcMachine *m);\n";
    table += "  {" + hex(block.start) + "u, " + hex(traces[block_index].back().end) + "u, " + name + "},\n";
  }

  if (merged_count) {
    fprintf(stderr, "gcrecomp: %u blocks entered only by falling into them, written into the block before\n",
            merged_count);
  }
  if (idle_loops) {
    fprintf(stderr, "gcrecomp: %u wait loops counted rather than run (%u of them across blocks)\n",
            idle_loops, split_idle_loops);
  }
  if (instructions) {
    fprintf(stderr, "gcrecomp: %u of %u instructions written out, the rest through their handlers\n", written_out,
            instructions);
  }

  {
    unsigned files = options.files;
    if (!files) files = (unsigned)(total_cost / GC_COST_PER_FILE) + 1u;
    files = std::max(1u, std::min(files, GC_MAX_FILES));
    const double target = total_cost / files;
    std::string chunk;
    std::set<uint32_t> chunk_links;
    size_t in_chunk = 0;
    double done_cost = 0.0;
    auto finish_chunk = [&]() -> bool {
      if (chunk.empty()) return true;
      char file[64];
      snprintf(file, sizeof(file), "recompiled_blocks_%03u.c", (unsigned)sources.size());
      sources.push_back(file);
      /* The blocks this file names as the next to run. */
      std::string linked;
      for (uint32_t address : chunk_links) {
        char line[64];
        snprintf(line, sizeof(line), "void *gc_block_%08x(GcMachine *m);\n", address);
        linked += line;
      }
      const bool ok = write_file(options.out_dir + "/" + file, PROLOGUE + linked + "\n" + chunk, error);
      chunk.clear();
      chunk_links.clear();
      in_chunk = 0;
      return ok;
    };
    for (const Unit &unit : units) {
      chunk += unit.text;
      chunk_links.insert(unit.links.begin(), unit.links.end());
      done_cost += unit.cost;
      /* Aimed at where the running total says this file should end, so
       * that rounding in one file does not push every later one along. */
      const bool full = done_cost >= target * (double)(sources.size() + 1u) - 1e-9;
      if ((full || ++in_chunk >= GC_MAX_BLOCKS_PER_FILE) && !finish_chunk()) return false;
    }
    if (!finish_chunk()) return false;
  }

  /* The table names every block, so it declares them all and lives on its
   * own rather than in whichever piece happened to be last. */
  {
    std::string blocks =
        "/* Generated by gcrecomp. Do not edit. */\n"
        "#include \"recompiled.h\"\n\n" +
        declarations + "\nconst GcRecompiledBlock gc_recompiled_blocks[] = {\n" + table + "};\n";
    blocks += "const unsigned gc_recompiled_block_count =\n"
              "    (unsigned)(sizeof(gc_recompiled_blocks) / sizeof(gc_recompiled_blocks[0]));\n";
    {
      uint32_t lowest = 0xffffffffu, highest = 0;
      for (const Block &block : analysis.blocks) {
        if (block.start < lowest) lowest = block.start;
        if (block.end > highest) highest = block.end;
      }
      blocks += "const uint32_t gc_recompiled_base = " + hex(lowest) + "u;\n";
      {
        char span[96];
        snprintf(span, sizeof(span), "const unsigned gc_recompiled_span = %uu;\n",
                 (unsigned)((highest - lowest) / 4u + 1u));
        blocks += span;
      }
    }
    if (!write_file(options.out_dir + "/recompiled_table.c", blocks, error)) return false;
    sources.push_back("recompiled_table.c");
  }

  {
    std::string header =
        "/* Generated by gcrecomp. Do not edit. */\n"
        "#ifndef GC_RECOMPILED_H\n"
        "#define GC_RECOMPILED_H\n"
        "\n"
        "#include \"gc.h\"\n"
        "\n"
        "/* A block, and the guest addresses it covers: it starts at `address`\n"
        " * and ends before `end`. `run` answers the block to run next when it\n"
        " * knows it, or null (GcRecompiledFn, gc_recompiled_rt.h). */\n"
        "typedef struct GcRecompiledBlock {\n"
        "  uint32_t address;\n"
        "  uint32_t end;\n"
        "  void *(*run)(GcMachine *m);\n"
        "} GcRecompiledBlock;\n"
        "\n"
        "extern const GcRecompiledBlock gc_recompiled_blocks[];\n"
        "extern const unsigned gc_recompiled_block_count;\n"
        "\n"
        "/* The lowest address any block starts at, and how many instruction\n"
        " * slots lie between it and the highest - which is what the dispatcher\n"
        " * indexes by. */\n"
        "extern const uint32_t gc_recompiled_base;\n"
        "extern const unsigned gc_recompiled_span;\n"
        "\n"
        "/* The image the blocks were recompiled from, so the program can check\n"
        " * at startup that what is in memory is what was recompiled. */\n"
        "extern const unsigned char gc_recompiled_image[];\n"
        "extern const unsigned gc_recompiled_image_size;\n"
        "extern const char gc_recompiled_name[];\n"
        "\n"
        "/* A Wii disc's title key - the key to the one disc this program was\n"
        " * built from - and whether there is one. */\n"
        "extern const unsigned char gc_recompiled_title_key[16];\n"
        "extern const int gc_recompiled_has_title_key;\n"
        "\n"
        "/* What the disc content is called beside the executable. The code is\n"
        " * embedded in the program; the content is a file, because it is the\n"
        " * part that is the game's rather than ours. */\n"
        "#define GC_RECOMPILED_DISC \"" + options.name + ".gcdata\"\n"
        "\n"
        "/* Which console the program is for: 0 a GameCube, 1 a Wii. */\n"
        "#define GC_RECOMPILED_WII " + std::string(image.wii ? "1" : "0") + "\n"
        "\n"
        "void gc_recompiled_run(GcMachine *m, uint64_t until_field);\n"
        "\n"
        "#endif\n";
    if (!write_file(options.out_dir + "/recompiled.h", header, error)) return false;
  }


  /* The dispatcher. It belongs to the generated project rather than the
   * runtime because it names the generated table; everything it does with
   * the machine is the runtime's own. */
  {
    std::string run =
        "/* Generated by gcrecomp. Do not edit. */\n"
        "#include <stdlib.h>\n"
        "\n"
        "#include \"recompiled.h\"\n"
        "#include \"gc_recompiled_rt.h\"\n"
        "\n"
        "/* The block starting at an address, in one load.\n"
        " *\n"
        " * This runs between every block, which on a real workload is every\n"
        " * few instructions, so how it looks an address up is not a detail.\n"
        " * The first version halved a sorted range - eleven dependent loads a\n"
        " * time - and that alone made the recompiled build *slower* than the\n"
        " * interpreter it replaced, on a program where it was otherwise doing\n"
        " * strictly less work. Two other suspects were measured first and\n"
        " * neither mattered: letting the compiler choose what to inline, and\n"
        " * link-time optimisation across the runtime boundary.\n"
        " *\n"
        " * Guest code is dense, so an array indexed by the instruction\n"
        " * number - one entry per four bytes between the lowest and highest\n"
        " * block - is a bounds check and a load. It is mostly empty, and\n"
        " * being mostly empty costs a pointer per four bytes of address\n"
        " * space the program spans, which is megabytes on a machine that has\n"
        " * gigabytes. */\n"
        "static void *(**table)(GcMachine *);\n"
        "static unsigned table_hooks = 0xffffffffu;\n"
        "/* The blocks left out for a hook (at most 8, as many as there are\n"
        " * hooks): a block that says one of them comes next is not run straight\n"
        " * away, and the loop below looks the address up like any other. */\n"
        "static void *hooked_run[8];\n"
        "static unsigned hooked_count;\n"
        "\n"
        "/* A block that contains an address the runtime answers calls to\n"
        " * (OSReport, in a test program) is left out, and the interpreter runs\n"
        " * it: the interpreter looks for those addresses before every\n"
        " * instruction, and recompiled code only looks when something is due.\n"
        " * A game has no such addresses and loses nothing. */\n"
        "static void build_table(void) {\n"
        "  uint32_t hooks[8];\n"
        "  const unsigned hook_count = gc_os_hook_addresses(hooks, 8u);\n"
        "  unsigned i, k;\n"
        "  if (!table) table = (void *(**)(GcMachine *))malloc(gc_recompiled_span * sizeof(*table));\n"
        "  if (!table) return;\n"
        "  for (i = 0; i < gc_recompiled_span; i++) table[i] = 0;\n"
        "  table_hooks = gc_os_hook_generation();\n"
        "  hooked_count = 0;\n"
        "  for (i = 0; i < gc_recompiled_block_count; i++) {\n"
        "    const GcRecompiledBlock *b = &gc_recompiled_blocks[i];\n"
        "    const uint32_t index = (b->address - gc_recompiled_base) >> 2;\n"
        "    int hooked = 0;\n"
        "    for (k = 0; k < hook_count; k++) {\n"
        "      if (hooks[k] >= b->address && hooks[k] < b->end) hooked = 1;\n"
        "    }\n"
        "    if (index < gc_recompiled_span && !hooked) table[index] = b->run;\n"
        "    if (hooked && hooked_count < 8u) hooked_run[hooked_count++] = (void *)b->run;\n"
        "  }\n"
        "}\n"
        "\n"
        "/* Recompiled where there is a block and interpreted where there is\n"
        " * not. The fallback is not a failure mode: the analyzer only claims\n"
        " * code it can prove is reachable, so everything it could not prove -\n"
        " * a jump table it did not recognise, a module linked at run time,\n"
        " * code the program wrote itself - arrives here and simply runs.\n"
        " * Missing a block costs speed; inventing one would cost correctness. */\n"
        "void gc_recompiled_run(GcMachine *m, uint64_t until_field) {\n"
        "  if (!table || table_hooks != gc_os_hook_generation()) build_table();\n"
        "  /* Whoever called may have changed the machine since the last run. */\n"
        "  m->cpu.next_event = 0;\n"
        "  gc_rt_until_field = until_field;\n"
        "  while (m->field < until_field && !m->finished && !m->cpu.stopped) {\n"
        "    const uint32_t index = (m->cpu.pc - gc_recompiled_base) >> 2;\n"
        "    void *(*run)(GcMachine *) = (table && index < gc_recompiled_span) ? table[index] : 0;\n"
        "    if (run) {\n"
        "      void *next = run(m);\n"
        "      /* And the blocks each block says come next, straight away: what\n"
        "       * this loop does between blocks, a block that needs it finds on\n"
        "       * entry (gc_recompiled_rt.h). */\n"
        "      if (!hooked_count) {\n"
        "        while (next) next = ((void *(*)(GcMachine *))next)(m);\n"
        "      } else {\n"
        "        for (;;) {\n"
        "          unsigned k;\n"
        "          for (k = 0; k < hooked_count && next != hooked_run[k]; k++) {}\n"
        "          if (!next || k < hooked_count) break;\n"
        "          next = ((void *(*)(GcMachine *))next)(m);\n"
        "        }\n"
        "      }\n"
        "    } else {\n"
        "      /* The interpreter expects a machine that is up to date: a block\n"
        "       * can return with something due at this very instruction and\n"
        "       * not yet done, and the interpreter would otherwise take that\n"
        "       * interrupt one instruction late. It keeps the machine current\n"
        "       * as it goes, and the recompiled code that follows looks again\n"
        "       * before it relies on anything. */\n"
        "      gc_machine_sync(m);\n"
        "      gc_machine_step(m);\n"
        "      m->cpu.next_event = 0;\n"
        "    }\n"
        "    /* A field that ended inside the block is counted before the\n"
        "     * loop decides whether to go on, as it was when the machine did\n"
        "     * its work after every instruction. */\n"
        "    if (m->cpu.cycles >= m->next_field_cycles) {\n"
        "      gc_machine_sync(m);\n"
        "      m->cpu.next_event = 0;\n"
        "    }\n"
        "  }\n"
        "  /* And leave it up to date for whoever looks next - the interpreter,\n"
        "   * a frontend, a state dump. */\n"
        "  gc_machine_sync(m);\n"
        "}\n";
    if (!write_file(options.out_dir + "/recompiled_run.c", run, error)) return false;
  }

  /* The image itself, so the program carries what it was built from. The
   * code is embedded; disc content is not, and travels beside the
   * executable as ID.gcdata. */
  {
    std::string data =
        "/* Generated by gcrecomp. Do not edit. */\n"
        "#include \"recompiled.h\"\n"
        "\nconst char gc_recompiled_name[] = \"" + options.name + "\";\n"
        "\nconst unsigned char gc_recompiled_image[] = {\n";
    /* The executable's own bytes, which the reader already has - for a
     * disc that is the DOL pulled out of it and not the disc itself. */
    const std::vector<uint8_t> &raw = image.raw;
    data.reserve(raw.size() * 6 + 4096);
    for (size_t i = 0; i < raw.size(); i++) {
      char byte[8];
      snprintf(byte, sizeof(byte), "%u,", (unsigned)raw[i]);
      data += byte;
      if ((i % 24u) == 23u) data += "\n";
    }
    data += "\n};\n";
    {
      char count[64];
      snprintf(count, sizeof(count), "\nconst unsigned gc_recompiled_image_size = %uu;\n",
               (unsigned)raw.size());
      data += count;
    }
    {
      std::string key = "\nconst unsigned char gc_recompiled_title_key[16] = {";
      for (size_t i = 0; i < 16; i++) {
        char byte[8];
        snprintf(byte, sizeof(byte), "%u,", image.title_key.size() == 16 ? (unsigned)image.title_key[i] : 0u);
        key += byte;
      }
      key += "};\nconst int gc_recompiled_has_title_key = ";
      key += image.title_key.size() == 16 ? "1" : "0";
      key += ";\n";
      data += key;
    }
    if (!write_file(options.out_dir + "/recompiled_image.c", data, error)) return false;
  }

  /* The program's own entry point: the window when there is one, and the
   * runtime's headless runner (runtime-gc/src/gc_runner.c) otherwise - the
   * same machine, boot state and loader as gc_run, with the recompiled
   * dispatcher in place of the interpreter's loop. That is what makes the
   * phase gate meaningful: the two differ in one thing. */
  {
    std::string main_c =
        "/* Generated by gcrecomp. Do not edit. */\n"
        "#include <stddef.h>\n"
        "\n"
        "#include \"gc.h\"\n"
        "#include \"gc_runner.h\"\n"
        "#include \"recompiled.h\"\n"
        "#include \"gc_disc.h\"\n"
        "#include \"wii.h\"\n"
        "#ifdef GC_HAVE_SDL\n"
        "#include \"gc_sdl.h\"\n"
        "#endif\n"
        "\n"
        "/* The runner steps the block table itself when --differential looks for\n"
        " * the block that diverged, through its own copy of the layout. */\n"
        "typedef char gc_block_layout_matches_the_runner[\n"
        "    (sizeof(GcRecompiledBlock) == sizeof(GcRunnerBlock) &&\n"
        "     offsetof(GcRecompiledBlock, address) == offsetof(GcRunnerBlock, address) &&\n"
        "     offsetof(GcRecompiledBlock, end) == offsetof(GcRunnerBlock, end) &&\n"
        "     offsetof(GcRecompiledBlock, run) == offsetof(GcRunnerBlock, run)) ? 1 : -1];\n"
        "\n"
        "/* A window if there is one to be had, and the headless runner if not\n"
        " * - or if the arguments are the runner's own, which is how the test\n"
        " * tooling drives a generated game without a screen. */\n"
        "int main(int argc, char **argv) {\n"
        "  GcRunnerProgram runner;\n"
        "  /* Before any machine exists (wii.h). */\n"
        "  gc_console_select(GC_RECOMPILED_WII);\n"
        "  if (gc_recompiled_has_title_key) gc_disc_embedded_title_key(gc_recompiled_title_key);\n"
        "#ifdef GC_HAVE_SDL\n"
        "  if (!gc_sdl_wants_headless(argc, argv)) {\n"
        "    GcSdlProgram program;\n"
        "    program.name = gc_recompiled_name;\n"
        "    program.image = gc_recompiled_image;\n"
        "    program.image_size = gc_recompiled_image_size;\n"
        "    program.run = gc_recompiled_run;\n"
        "    /* The code is embedded; the disc content is not, and travels\n"
        "     * beside the executable under the game's own identifier. */\n"
        "    return gc_sdl_main(argc, argv, &program, GC_RECOMPILED_DISC);\n"
        "  }\n"
        "#endif\n"
        "  runner.name = gc_recompiled_name;\n"
        "  runner.image = gc_recompiled_image;\n"
        "  runner.image_size = gc_recompiled_image_size;\n"
        "  runner.disc_name = GC_RECOMPILED_DISC;\n"
        "  runner.run = gc_recompiled_run;\n"
        "  runner.blocks = (const GcRunnerBlock *)(const void *)gc_recompiled_blocks;\n"
        "  runner.block_count = gc_recompiled_block_count;\n"
        "  runner.base = gc_recompiled_base;\n"
        "  runner.span = gc_recompiled_span;\n"
        "  return gc_runner_main(argc, argv, &runner);\n"
        "}\n";
    if (!write_file(options.out_dir + "/main.c", main_c, error)) return false;
  }

  {
    /* Strict floating point is not optional. A contracted multiply-add
     * gives different numbers from the two operations it replaces, and the
     * vector tests measured that difference on real hardware. */
    std::string generated;
    for (const std::string &source : sources) generated += "    " + source + "\n";
    std::string cmake =
        "# Generated by gcrecomp. Do not edit.\n"
        "cmake_minimum_required(VERSION 3.16)\n"
        "# Release unless told otherwise. Before project(): with MSVC and a\n"
        "# single-configuration generator, project() itself makes it Debug.\n"
        "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n"
        "    set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" FORCE)\n"
        "endif()\n"
        "project(" + options.name + " C CXX)\n"
        "set(CMAKE_C_STANDARD 99)\n"
        "\n"
        "# Before the runtime is added, because that is where the window, the\n"
        "# controller, the sound and the settings menu are built - and they are\n"
        "# built only if this succeeds. Without SDL2 the program still works;\n"
        "# it just has no screen, which is what the test tooling wants anyway.\n"
        "find_package(SDL2 QUIET)\n"
        "\n"
        "# The runtime snapshot gcrecomp copied in, so the project builds on its\n"
        "# own. A game links the libraries and needs none of the test tools.\n"
        "set(GC_RUNTIME_TOOLS OFF)\n"
        "add_subdirectory(runtime-gc runtime-gc EXCLUDE_FROM_ALL)\n"
        "\n"
        "add_executable(" + options.name + "\n"
        "    main.c\n"
        "    recompiled_run.c\n"
        "    recompiled_image.c\n" +
        generated + ")\n"
        "target_include_directories(" + options.name + " PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})\n"
        "target_link_libraries(" + options.name + " PRIVATE gcruntime)\n"
        "if(TARGET gcplatform)\n"
        "    target_compile_definitions(" + options.name + " PRIVATE GC_HAVE_SDL)\n"
        "    target_link_libraries(" + options.name + " PRIVATE gcplatform)\n"
        "    get_target_property(_gc_sdl2_type SDL2::SDL2 TYPE)\n"
        "    if(WIN32 AND _gc_sdl2_type STREQUAL \"SHARED_LIBRARY\")\n"
        "        add_custom_command(TARGET " + options.name + " POST_BUILD\n"
        "            COMMAND ${CMAKE_COMMAND} -E copy_if_different\n"
        "                $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:" + options.name + ">)\n"
        "    endif()\n"
        "endif()\n"
        "if(MSVC)\n"
        "    target_compile_options(" + options.name + " PRIVATE /fp:strict /bigobj)\n"
        "else()\n"
        "    target_compile_options(" + options.name +
        " PRIVATE -ffp-contract=off -frounding-math)\n"
        "endif()\n";
    if (!write_file(options.out_dir + "/CMakeLists.txt", cmake, error)) return false;
  }
  return snapshot_runtime(options, error);
}

}  // namespace gcrecomp
