// Code discovery for SNES ROMs: which (address, CPU mode) pairs are 65C816
// instructions, how control flows between them, and how they group into
// functions.
//
// The 65C816's instruction lengths depend on the M and X flags, so a code
// location is a key: the 24-bit PC plus the mode (snes_recomp.h). The mode
// is tracked along control flow: REP/SEP with their immediate operands
// change it, XCE after CLC/SEC switches emulation mode, and PLP/RTI make it
// unknown (the continuation is found at run time and supplied by a trace).
// Code entered in a mode it was never compiled for runs interpreted.
//
// Beyond code known to run, exploration guesses (a callee runs in its
// caller's mode, a call returns in it, a jump table's entries are code), and
// a wrong guess decodes data. Guessed code is therefore accepted region by
// region, only when it looks like code (see Analyzer::run()).
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "rom.h"

extern "C" {
#include "snes_cpu_decode.h"
#include "snes_recomp.h"
}

namespace snesrecomp {

inline uint32_t make_key(uint32_t pc24, unsigned mode) { return SNES_RC_KEY(pc24, mode); }
inline uint32_t key_pc(uint32_t key) { return SNES_RC_KEY_PC(key); }
inline unsigned key_mode(uint32_t key) { return SNES_RC_KEY_MODE(key); }
// The address `n` bytes after pc, wrapping within the bank like the PC.
inline uint32_t pc_add(uint32_t pc24, uint32_t n) { return (pc24 & 0xFF0000u) | ((pc24 + n) & 0xFFFFu); }

struct Insn {
    uint32_t key = 0;
    uint32_t pc = 0;
    uint8_t mode = 0;
    uint8_t op = 0;
    uint8_t len = 1;
    uint8_t flow = SNES_FLOW_NEXT;
    uint32_t operand = 0;  // operand bytes, little-endian
    uint32_t next = 0;     // key of the instruction that follows when control falls through (0 = none)
    uint32_t target = 0;   // key of a static branch/jump target (0 = none)
    bool target_in_function = false; // branch/jump target explored as part of the same function
    std::vector<uint32_t> table;     // jump-table targets (JMP/JSR (a,X))
    bool exits = false;    // returns to the dispatcher after executing
};

struct Function {
    uint32_t entry = 0;          // key
    std::vector<uint32_t> insns; // keys, in emission order
    bool wram = false;           // compiled from a WRAM image
};

struct AnalyzerOptions {
    std::vector<uint32_t> trace_keys; // entry keys seen at run time (snesrecomp --trace)
    // Larger C functions compile superlinearly slower (MSVC /O2); code past
    // the cap continues in another function through the dispatcher.
    size_t max_function_insns = 64;
    // How many guesses deep static exploration may go from code known to
    // run (trace keys, the reset vector); see Analyzer::run().
    unsigned speculation_depth = 1000;
    // A guessed region this large is taken for data decoded as code.
    size_t max_speculative_region = 3000;
    bool verbose = false; // progress of each phase on stderr
};

class Analyzer {
public:
    Analyzer(const Rom &rom, AnalyzerOptions options);
    void run();

    const Rom &rom;
    AnalyzerOptions options;

    std::map<uint32_t, Insn> insns;      // key -> instruction
    std::set<uint32_t> function_entries; // keys
    std::set<uint32_t> labels;           // keys the dispatcher can enter at
    std::vector<Function> functions;

    size_t tables_found = 0;
    size_t indirect_unresolved = 0;
    size_t trace_entries = 0;
    size_t regions_rejected = 0; // guessed code dropped as data (see run())

private:
    struct Root {
        uint32_t key;
        unsigned depth; // 0 = known to run; each guess adds one
    };

    bool decode(uint32_t key, Insn &out) const;
    void explore_region(const Root &root, std::deque<Root> &queue);
    bool overlaps_code(const Insn &in) const;
    void add_function(uint32_t key);
    void add_label(uint32_t key);
    void build_functions();
    std::vector<uint32_t> read_jump_table(const Insn &in) const;

    // Per byte of the 24-bit space: 1 = an accepted instruction starts here,
    // 2 = inside one.
    std::vector<uint8_t> owner_;
    std::set<uint32_t> rejected_;
};

} // namespace snesrecomp
