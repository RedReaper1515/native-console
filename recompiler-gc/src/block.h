/*
 * One basic block as C: the guest registers it uses in C locals, the common
 * instructions written out against them, and everything else handed to the
 * interpreter's handler. block.cpp has the design; docs/GC-PLAN.md
 * ("Registers in locals") the measurements and the argument that the result
 * is instruction for instruction the machine the interpreter runs.
 */
#ifndef GCRECOMP_BLOCK_H
#define GCRECOMP_BLOCK_H

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "analyzer.h"
#include "image.h"

namespace gcrecomp {

struct BlockOptions {
  /* The most estimated compile cost one C function holds before the block
   * continues in another (block.cpp, "Parts"). 0 never splits. */
  unsigned part_cost = 400;
  /* The most instructions one C function holds. 0 is no limit. */
  unsigned part_insns = 0;
  /* Inline the floating-point arithmetic handlers instead of calling one
   * shared copy in the runtime. */
  bool fp_inline = false;
  /* Quantised loads and stores through the block's maps, inline. */
  bool psq_inline = false;
  /* The paired arithmetic's steady-state fast path inline before the call. */
  bool lazy_inline = false;
  /* A block that branches to its own start loops inside its function. */
  bool loops = true;
  /* The most instructions a block and the blocks written into its function
   * after it (reached only by falling into them) hold together. 0 writes
   * every block on its own. */
  unsigned trace_insns = 64;
  /* Calls and returns through a return-address stack (gc_rt_return). */
  bool returns = true;
  /* Floating-point registers, and the paired steady state's note, in the
   * block's locals; the steady state's arithmetic inline on them. */
  bool fpr_locals = true;
  /* Those kernels inline rather than one shared copy each. */
  bool kernels_inline = false;
};

struct BlockCode {
  std::string text;
  double cost = 0.0;
  /* Blocks this one names directly as the next to run, so the file holding
   * it declares them. */
  std::set<uint32_t> links;
  unsigned instructions = 0;
  unsigned written_out = 0; /* instructions not handed to a handler */
};

/* `trace` is a block and the blocks that follow it without a gap which
 * nothing enters but by falling into them: they are written into one
 * function. `has_block` answers whether a block function starts at an
 * address, for the direct links. */
BlockCode emit_block(const Image &image, const std::vector<Block> &trace,
                     const std::function<bool(uint32_t)> &has_block, const BlockOptions &options);

}  // namespace gcrecomp

#endif
