#include "analyzer.h"

#include <algorithm>
#include <deque>

extern "C" {
#include "gc_cpu_decode.h"
#include "gc_cpu_ops.h"
}

namespace gcrecomp {
namespace {

/* The opcodes this core implements, from the one list the interpreter's
 * dispatch is also built from. An opcode missing here is one no handler
 * exists for, and a block containing it is left to the interpreter. */
bool implemented(GcOp op) {
  static bool table[GC_OP_COUNT];
  static bool filled = false;
  if (!filled) {
#define GC_MARK(enum_name, handler) table[enum_name] = true;
    GC_CPU_OPS(GC_MARK)
#undef GC_MARK
    filled = true;
  }
  return op > 0 && op < GC_OP_COUNT && table[op];
}

/* Does this instruction end a block, and where can it go? */
struct Flow {
  bool ends = false;
  bool falls_through = true;  /* the next instruction is reachable */
  bool has_target = false;
  uint32_t target = 0;
};

Flow flow_of(GcOp op, uint32_t insn, uint32_t address) {
  Flow flow;
  switch (op) {
    case GC_OP_B: {
      /* The unconditional branch. AA says whether the displacement is
       * absolute, LK whether it is a call - and a call falls through,
       * because that is where it comes back to. */
      const int32_t displacement = gc_sext32((insn >> 2) & 0x00ffffffu, 24) * 4;
      flow.ends = true;
      flow.has_target = true;
      flow.target = ((insn >> 1) & 1u) ? (uint32_t)displacement
                                       : (uint32_t)((int32_t)address + displacement);
      flow.falls_through = ((insn & 1u) != 0);
      return flow;
    }
    case GC_OP_BC: {
      const int32_t displacement = gc_sext32((insn >> 2) & 0x3fffu, 14) * 4;
      flow.ends = true;
      flow.has_target = true;
      flow.target = ((insn >> 1) & 1u) ? (uint32_t)displacement
                                       : (uint32_t)((int32_t)address + displacement);
      /* A conditional branch always falls through when it is not taken,
       * whether or not it is a call. */
      flow.falls_through = true;
      return flow;
    }
    case GC_OP_BCCTR:
    case GC_OP_BCLR:
      /* Where it goes is a run-time value. The block ends and the
       * dispatcher picks it up; a conditional form still falls through. */
      flow.ends = true;
      flow.falls_through = (((insn >> 21) & 0x14u) != 0x14u) || ((insn & 1u) != 0);
      return flow;
    case GC_OP_SC:
    case GC_OP_RFI:
      flow.ends = true;
      flow.falls_through = false;
      return flow;
    default:
      return flow;
  }
}

}  // namespace

std::vector<uint32_t> pointer_roots(const Image &image) {
  std::set<uint32_t> found;
  auto consider = [&](uint32_t value) {
    if ((value & 3u) == 0u && image.executable_at(value)) found.insert(value);
  };
  for (const Segment &segment : image.segments) {
    const size_t words = segment.bytes.size() / 4u;
    const uint8_t *const p = segment.bytes.data();
    auto word_at = [&](size_t i) {
      return ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
             p[i * 4 + 3];
    };
    if (!segment.executable) {
      /* Addresses stored as data. Word-aligned: the compilers that made
       * these programs align pointers, and a pointer split across two
       * words would not be one anybody jumps through. */
      for (size_t i = 0; i < words; i++) consider(word_at(i));
      continue;
    }
    /* An address built in a register: `addis rD, 0, hi` (lis) and, within
     * the next eight instructions, `addi rX, rD, lo` or `ori rX, rD, lo`.
     * The first instruction that takes rD as that operand decides; a false
     * pair only adds a candidate. */
    for (size_t i = 0; i < words; i++) {
      const uint32_t insn = word_at(i);
      if ((insn >> 26) != 15u || ((insn >> 16) & 31u) != 0u) continue;
      const unsigned rd = (insn >> 21) & 31u;
      const uint32_t high = (insn & 0xffffu) << 16;
      for (size_t j = i + 1u; j < words && j <= i + 8u; j++) {
        const uint32_t next = word_at(j);
        const unsigned op = next >> 26;
        if (op == 14u && ((next >> 16) & 31u) == rd) { /* addi rX, rD, lo */
          consider(high + (uint32_t)(int32_t)(int16_t)(next & 0xffffu));
          break;
        }
        if (op == 24u && ((next >> 21) & 31u) == rd) { /* ori rX, rD, lo */
          consider(high | (next & 0xffffu));
          break;
        }
      }
    }
  }
  return std::vector<uint32_t>(found.begin(), found.end());
}

Analysis analyze(const Image &image, const std::vector<uint32_t> &extra_roots) {
  Analysis analysis;
  std::set<uint32_t> starts;
  std::set<uint32_t> visited;
  std::deque<uint32_t> queue;

  auto add_root = [&](uint32_t address) {
    if (!image.executable_at(address)) return;
    if (starts.insert(address).second) queue.push_back(address);
  };

  add_root(image.entry);
  for (uint32_t root : extra_roots) add_root(root);
  analysis.roots = starts;

  /* Walk every block reachable from a root, adding the roots that each
   * block's exits reveal, until nothing new appears. */
  while (!queue.empty()) {
    uint32_t address = queue.front();
    queue.pop_front();
    if (!visited.insert(address).second) continue;

    Block block;
    block.start = address;
    for (;;) {
      uint32_t insn = 0;
      if (!image.word(address, &insn) || !image.executable_at(address)) break;

      const GcOp op = gc_decode(insn);
      if (!gc_decode_is_canonical(op, insn) || !implemented(op)) {
        /* Stop before it. A block that contains an instruction with no
         * handler cannot be emitted, and stopping here leaves the part
         * before it recompiled and the rest to the interpreter. */
        if (op != GC_OP_ILLEGAL) analysis.unimplemented[gc_op_info[op].name]++;
        break;
      }
      address += 4u;
      analysis.instructions++;

      {
        const Flow flow = flow_of(op, insn, address - 4u);
        if (flow.has_target) add_root(flow.target);
        if (flow.ends) {
          if (flow.falls_through) add_root(address);
          break;
        }
      }
      /* Another block starts here, so this one stops: blocks never
       * overlap, and the dispatcher can then name any of them. */
      if (starts.count(address)) break;
    }
    block.end = address;
    if (block.end > block.start) analysis.blocks.push_back(block);
  }

  std::sort(analysis.blocks.begin(), analysis.blocks.end(),
            [](const Block &a, const Block &b) { return a.start < b.start; });

  /* A block discovered before the block that lands in its middle has to be
   * cut short, or the two overlap and the dispatcher has two answers for
   * one address. */
  for (size_t i = 0; i + 1 < analysis.blocks.size(); i++) {
    if (analysis.blocks[i].end > analysis.blocks[i + 1].start) {
      analysis.blocks[i].end = analysis.blocks[i + 1].start;
    }
  }
  analysis.blocks.erase(
      std::remove_if(analysis.blocks.begin(), analysis.blocks.end(),
                     [](const Block &b) { return b.end <= b.start; }),
      analysis.blocks.end());
  return analysis;
}

}  // namespace gcrecomp
