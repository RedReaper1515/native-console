/*
 * Which addresses hold code, and where the basic blocks begin and end.
 *
 * The analyzer follows control flow from a set of roots rather than
 * sweeping linearly, because a linear sweep over a text section decodes
 * jump tables, string literals and alignment padding as instructions and
 * emits handlers for them. Following flow is conservative in the other
 * direction: it finds only code it can prove is reachable, and everything
 * it misses falls back to the interpreter rather than becoming wrong.
 * That asymmetry is the whole reason to do it this way - the cost of
 * missing a block is speed, and the cost of inventing one is a wrong
 * answer.
 *
 * A block ends at anything that can change the flow of control, and at the
 * start of any other block, so blocks never overlap. An indirect branch
 * ends a block and adds no successor: where it goes is a run-time
 * question, and the dispatcher answers it.
 */
#ifndef GCRECOMP_ANALYZER_H
#define GCRECOMP_ANALYZER_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "image.h"

namespace gcrecomp {

struct Block {
  uint32_t start = 0;
  uint32_t end = 0; /* one past the last instruction */
};

struct Analysis {
  std::vector<Block> blocks;
  std::set<uint32_t> roots;
  /* Addresses that decode to an opcode this core does not implement, and
   * the opcodes themselves - reported so a gap is visible rather than
   * silently interpreted for ever. */
  std::map<std::string, unsigned> unimplemented;
  unsigned instructions = 0;
};

/* `extra_roots` lets a caller add entry points the flow cannot reach on
 * its own - an interrupt vector, or something a trace found. */
Analysis analyze(const Image &image, const std::vector<uint32_t> &extra_roots);

/* Entry points no branch names, found from how a program refers to its own
 * code: every word of a data segment that holds the address of an
 * instruction (a C++ virtual table, a callback table, a switch's jump
 * table, a constructor list), and every address a program builds in a
 * register with a `lis` and an `addi` or `ori` (a callback handed to a
 * function). Sorted, without duplicates.
 *
 * A game written in C++ calls most of its functions through pointers, so
 * without these most of it is only ever reached through the interpreter:
 * Shrek 2's direct flow reaches 122,333 of its 622,776 instructions, and a
 * level's new objects arrive as code the interpreter runs until a trace
 * names it. A candidate that is not really an entry point costs a block
 * nothing ever jumps to, or a block boundary where one block would have
 * done - speed, not correctness: every block is still only entered at its
 * first instruction by the dispatcher, and a word that does not decode to
 * an instruction stops a block before it, as it does for any root. */
std::vector<uint32_t> pointer_roots(const Image &image);

}  // namespace gcrecomp

#endif
