/*
 * The recompiled blocks, as C.
 *
 * The emitter sequences handlers and never implements one. Every guest
 * instruction becomes a call to the same gc_op_* function the interpreter
 * calls, with the instruction word as a compile-time constant so that the
 * field extractions inside it fold away and what is left is the
 * arithmetic. Everything around the instruction - the hooks, the
 * interrupts, the retirement, the video interface - is the runtime's own
 * gc_machine_before and gc_machine_after, called in the same places
 * gc_machine_step calls them.
 *
 * What that buys is the fetch and the decode: a bus read, a table lookup
 * and a switch, gone at every instruction. What it deliberately does not
 * buy is running several instructions between interrupt checks, which
 * would deliver interrupts late and move the beam in jumps.
 */
#ifndef GCRECOMP_EMITTER_H
#define GCRECOMP_EMITTER_H

#include <string>

#include "analyzer.h"
#include "block.h"
#include "image.h"

namespace gcrecomp {

struct Options {
  std::string name = "GAME";
  std::string out_dir;
  std::string runtime_dir;
  /* How a block becomes C functions (block.h). */
  BlockOptions block;
  /* How many recompiled_blocks_NNN.c files. 0 chooses from the estimated
   * compile cost. */
  unsigned files = 0;
};

/* What every generated block file starts with: the includes and the macros
 * its blocks use (it includes the project's recompiled.h). */
const char *generated_prologue();

/* Writes the generated project. Returns false with `error` set. */
bool emit_project(const Image &image, const Analysis &analysis, const Options &options,
                  std::string *error);

}  // namespace gcrecomp

#endif
