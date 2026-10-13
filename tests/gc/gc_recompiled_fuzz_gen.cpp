/*
 * Writes the recompiled half of gc_recompiled_fuzz: random blocks, emitted
 * by gcrecomp's own block emitter (recompiler-gc/src/block.cpp), for the
 * driver (gc_recompiled_fuzz.c) to run against the interpreter.
 *
 * The emitter writes the common instructions out in C rather than calling
 * the interpreter's handlers, which is what an emitter and an interpreter
 * drifting apart would look like. This is the defence: every opcode the
 * core implements appears in many blocks with random fields - registers,
 * immediates, Rc and OE, branch options, quantised forms - at the start, in
 * the middle and at the end of a block, and in blocks that continue into a
 * second one by falling through a conditional branch, so the written-out
 * forms, the handler fallbacks, the write-back labels and the side exits are
 * all exercised. The driver then runs each block from many random machine
 * states and compares the result with the interpreter's.
 *
 *   gc_recompiled_fuzz_gen OUT.c [CASES] [SEED]
 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "block.h"
#include "emitter.h"
#include "image.h"

extern "C" {
#include "gc_cpu_decode.h"
#include "gc_cpu_ops.h"
}

namespace {

uint64_t state = 0x9e3779b97f4a7c15ull;

uint32_t next32() {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (uint32_t)(state >> 11);
}

std::vector<GcOp> implemented_ops() {
  std::vector<GcOp> ops;
#define GC_ADD(enum_name, handler) ops.push_back(enum_name);
  GC_CPU_OPS(GC_ADD)
#undef GC_ADD
  return ops;
}

/* A random encoding of `op`: the bits its encoding fixes, random elsewhere,
 * with the fields that matter nudged toward the interesting. */
bool random_word(GcOp op, uint32_t *out) {
  const GcOpInfo &info = gc_op_info[op];
  for (int tries = 0; tries < 64; tries++) {
    uint32_t w = (next32() & ~info.bitmask) | info.pattern;
    /* Base registers for loads and stores point into the data window most
     * of the time (the driver keeps r1..r6 there). */
    if ((next32() & 3u) != 0) {
      const unsigned base = 1u + next32() % 6u;
      w = (w & ~(31u << 16)) | (base << 16);
      if ((w & info.bitmask & (31u << 16)) != 0) w = (next32() & ~info.bitmask) | info.pattern;
    }
    /* Floating-point instructions mostly on f1..f4 (and r1..r4 as bases), so
     * that a block's instructions read what the ones before them wrote. */
    if (info.pattern >> 26 == 4u || (info.pattern >> 26 >= 48u && info.pattern >> 26 <= 63u)) {
      if ((next32() & 3u) != 0) {
        for (unsigned shift : {21u, 16u, 11u, 6u}) {
          if ((info.bitmask >> shift & 31u) == 0) w = (w & ~(31u << shift)) | ((1u + next32() % 4u) << shift);
        }
      }
    }
    /* Small displacements, so a pointer plus a displacement stays in the
     * window more often than not. */
    if ((next32() & 1u) != 0 && (info.bitmask & 0xffffu) == 0) {
      w = (w & ~0xffffu) | ((next32() % 0x200u) - 0x100u & 0xffffu);
    }
    if ((w & info.bitmask) != info.pattern) continue;
    if (gc_decode(w) != op || !gc_decode_is_canonical(op, w)) continue;
    /* Not the write-gather pipe or the locked cache's DMA: a random write
     * there sends random commands to the graphics processor, which is slow
     * and tests nothing about the processor. */
    if (op == GC_OP_MTSPR && (GC_F_spr(w) == 921u || GC_F_spr(w) == 922u || GC_F_spr(w) == 923u)) continue;
    *out = w;
    return true;
  }
  return false;
}

bool is_branch(GcOp op) { return op == GC_OP_B || op == GC_OP_BC || op == GC_OP_BCLR || op == GC_OP_BCCTR; }

/* Ops whose handler would take the machine somewhere the driver cannot
 * follow deterministically in both runs, or that end blocks by themselves. */
bool excluded(GcOp op) {
  return op == GC_OP_RFI || op == GC_OP_SC || op == GC_OP_MTMSR || op == GC_OP_MTSR || op == GC_OP_MTSRIN;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: gc_recompiled_fuzz_gen OUT.c [CASES] [SEED]\n");
    return 2;
  }
  const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], nullptr, 10) : 4000u;
  if (argc > 3) state ^= strtoull(argv[3], nullptr, 10) * 0x2545f4914f6cdd1dull;
  const uint32_t base = 0x80100000u, spacing = 0x80u; /* up to 32 instructions a case */

  std::vector<GcOp> ops;
  for (GcOp op : implemented_ops()) {
    if (!excluded(op)) ops.push_back(op);
  }

  gcrecomp::Image image;
  gcrecomp::Segment segment;
  segment.address = base;
  segment.executable = true;
  segment.bytes.assign((size_t)cases * spacing, 0);

  struct Case {
    uint32_t start;
    unsigned first, total;
    std::vector<gcrecomp::Block> trace;
  };
  std::vector<Case> made;
  auto put = [&](uint32_t address, uint32_t word) {
    const size_t at = address - base;
    segment.bytes[at] = (uint8_t)(word >> 24);
    segment.bytes[at + 1] = (uint8_t)(word >> 16);
    segment.bytes[at + 2] = (uint8_t)(word >> 8);
    segment.bytes[at + 3] = (uint8_t)word;
  };
  /* Half the time from the floating-point instructions blocks keep in
   * locals, so that runs of them read and write each other's registers. */
  static const GcOp fp_ops[] = {GC_OP_LFS, GC_OP_LFD, GC_OP_STFS, GC_OP_STFD, GC_OP_PSQ_L, GC_OP_PSQ_ST, GC_OP_PSQ_LU,
                                GC_OP_PSQ_STU, GC_OP_FMR, GC_OP_FNEG, GC_OP_FABS, GC_OP_PS_ADD, GC_OP_PS_SUB, GC_OP_PS_MUL,
                                GC_OP_PS_MULS0, GC_OP_PS_MULS1, GC_OP_PS_MADD, GC_OP_PS_MSUB, GC_OP_PS_NMADD,
                                GC_OP_PS_NMSUB, GC_OP_PS_MADDS0, GC_OP_PS_MADDS1, GC_OP_FCMPU, GC_OP_FADDS, GC_OP_PS_MR,
                                GC_OP_PS_MERGE01, GC_OP_ADDI, GC_OP_STFIWX};
  auto random_body_op = [&]() {
    if (next32() & 1u) return fp_ops[next32() % (sizeof(fp_ops) / sizeof(fp_ops[0]))];
    for (;;) {
      const GcOp op = ops[next32() % ops.size()];
      if (!is_branch(op)) return op;
    }
  };
  auto random_branch = [&](uint32_t address, bool conditional) {
    uint32_t w = 0;
    const GcOp op = conditional ? ((next32() % 3u) == 0 ? GC_OP_BCLR : (next32() % 2u) ? GC_OP_BC : GC_OP_BCCTR)
                                : ((next32() % 4u) == 0 ? GC_OP_B : GC_OP_BC);
    for (;;) {
      if (!random_word(op, &w)) continue;
      /* No link on a conditional branch in the middle of a trace; keep the
       * options that do something (BO values from the architecture's list). */
      static const uint32_t bos[] = {0x00u, 0x02u, 0x04u, 0x08u, 0x0au, 0x0cu, 0x10u, 0x12u, 0x14u};
      if (op != GC_OP_B) w = (w & ~(31u << 21)) | (bos[next32() % 9u] << 21);
      if (op == GC_OP_BCCTR) w |= 4u << 21; /* bcctr requires BO2 */
      if (conditional) w &= ~1u;
      if (conditional && op != GC_OP_B && ((w >> 21) & 0x14u) == 0x14u) continue; /* must be able to fall */
      (void)address;
      return w;
    }
  };

  std::string text = gcrecomp::generated_prologue();
  {
    const std::string include = "#include \"recompiled.h\"\n";
    const size_t at = text.find(include);
    if (at != std::string::npos) text.erase(at, include.size());
  }
  std::string table = "const GcFuzzCase gc_fuzz_cases[] = {\n";
  gcrecomp::BlockOptions options;
  for (unsigned k = 0; k < cases; k++) {
    Case c;
    c.start = base + k * spacing;
    uint32_t address = c.start;
    /* One or two blocks; the first ends in a conditional branch without a
     * link when there are two. */
    const bool two = (next32() % 4u) == 0;
    const unsigned first_body = 1u + next32() % 6u;
    for (unsigned i = 0; i < first_body; i++, address += 4u) {
      uint32_t w = 0;
      while (!random_word(random_body_op(), &w)) {}
      put(address, w);
    }
    if (two || (next32() % 2u)) {
      put(address, random_branch(address, two));
      address += 4u;
    }
    c.trace.push_back({c.start, address});
    c.first = (address - c.start) / 4u;
    if (two) {
      const uint32_t second = address;
      const unsigned body = 1u + next32() % 5u;
      for (unsigned i = 0; i < body; i++, address += 4u) {
        uint32_t w = 0;
        while (!random_word(random_body_op(), &w)) {}
        put(address, w);
      }
      if (next32() % 2u) {
        put(address, random_branch(address, false));
        address += 4u;
      }
      c.trace.push_back({second, address});
    }
    c.total = (address - c.start) / 4u;
    made.push_back(c);
  }
  image.segments.push_back(segment);
  for (Case &c : made) {
    const gcrecomp::BlockCode code =
        gcrecomp::emit_block(image, c.trace, [](uint32_t) { return false; }, options);
    text += code.text;
    char line[160];
    snprintf(line, sizeof(line), "  {0x%08xu, %uu, %uu, gc_block_%08x},\n", c.start, c.first, c.total, c.start);
    table += line;
  }
  table += "};\nconst unsigned gc_fuzz_case_count = " + std::to_string(made.size()) + "u;\n";
  text += "\ntypedef struct GcFuzzCase {\n  uint32_t start;\n  unsigned first, total;\n"
          "  void *(*run)(GcMachine *m);\n} GcFuzzCase;\n" + table;
  text += "const uint32_t gc_fuzz_code_base = 0x" + [&] {
    char b[16];
    snprintf(b, sizeof(b), "%08x", base);
    return std::string(b);
  }() + "u;\nconst unsigned gc_fuzz_code_size = " + std::to_string(segment.bytes.size()) + "u;\nconst unsigned char gc_fuzz_code[] = {";
  for (size_t i = 0; i < segment.bytes.size(); i++) {
    if ((i % 32u) == 0) text += "\n";
    text += std::to_string(segment.bytes[i]) + ",";
  }
  text += "\n};\n";
  FILE *file = fopen(argv[1], "wb");
  if (!file) {
    fprintf(stderr, "gc_recompiled_fuzz_gen: cannot write %s\n", argv[1]);
    return 1;
  }
  fwrite(text.data(), 1, text.size(), file);
  fclose(file);
  printf("gc_recompiled_fuzz_gen: %u cases\n", cases);
  return 0;
}
