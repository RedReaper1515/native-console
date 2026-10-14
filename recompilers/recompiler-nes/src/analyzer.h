// Code discovery for NES ROMs: which keys (PRG page, CPU address) are 6502
// instructions, how control flows between them, and how they group into
// functions.
//
// Inside one 8 KiB page everything is certain: the bytes are fixed and a
// branch or jump that stays in the page lands where it says. A function is
// therefore a set of instructions in one page. Leaving the page (a jump or
// call to another slot, falling off its end) depends on what the cartridge
// has mapped there at that moment, which only the run loop knows; the
// generated code returns to it, and the analyzer guesses the candidates
// (Rom::pages_at) so they are compiled and waiting.
//
// Beyond code known to run (the reset vector, trace keys), every such
// candidate, every callee and every return site is a guess, and a wrong
// guess decodes data. Guessed code is accepted region by region, only when
// it looks like code (see Analyzer::explore_region()).
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "rom.h"

extern "C" {
#include "nes_cpu_decode.h"
}

namespace nesrecomp {

struct Insn {
    uint32_t key = 0;
    uint8_t op = 0;
    uint8_t len = 1;
    uint8_t flow = NES_FLOW_NEXT;
    bool operand_known = false; // the operand bytes are in the same page as the opcode
    uint16_t operand = 0;       // operand bytes, little-endian
    uint32_t next = 0;          // key of the instruction that follows in this page (0 = none or another page)
    uint32_t target = 0;        // key of a branch/jump target in this page (0 = none or another page)
    uint16_t target_addr = 0;   // static JMP/JSR/branch target address (0 = unknown)
    uint16_t next_addr = 0;     // address after the instruction when control can continue there
    bool exits = false;         // returns to the run loop after executing
};

struct Function {
    uint32_t entry = 0;          // key
    std::vector<uint32_t> insns; // keys, in address order
};

struct AnalyzerOptions {
    std::vector<uint32_t> trace_keys; // entry keys seen at run time (nesrecomp --trace)
    // Larger C functions compile superlinearly slower (MSVC /O2); code past
    // the cap continues in another function through the run loop.
    size_t max_function_insns = 64;
    // How many guesses deep static exploration may go from code known to
    // run (trace keys, the reset vector).
    unsigned speculation_depth = 1000;
    // A guessed region this large is taken for data decoded as code.
    size_t max_speculative_region = 3000;
    // Follow jump tables found next to JMP (ind) and RTS dispatch code.
    bool jump_tables = true;
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
    std::set<uint32_t> labels;           // keys the run loop can enter at
    std::vector<Function> functions;

    size_t trace_entries = 0;
    size_t indirect_jumps = 0;
    size_t tables_found = 0;     // jump tables read
    size_t table_entries = 0;
    size_t page_exits = 0;       // jumps, calls and fall-throughs that leave their page
    size_t regions_rejected = 0; // guessed code dropped as data

private:
    struct Root {
        uint32_t key;
        unsigned depth; // 0 = known to run; each guess adds one
    };

    bool decode(uint32_t key, Insn &out) const;
    void explore_region(const Root &root, std::deque<Root> &queue);
    bool overlaps_code(const Insn &in) const;
    void guess(uint32_t from, uint16_t addr, std::vector<uint32_t> &out) const;
    void note_tables(const std::vector<Insn> &found);
    void read_tables(std::vector<std::vector<uint32_t>> &entries);
    void add_function(uint32_t key);
    void add_label(uint32_t key);
    void build_functions();

    // Per byte of PRG ROM: 1 = an accepted instruction starts here, 2 =
    // inside one.
    std::vector<uint8_t> owner_;
    std::set<uint32_t> rejected_;

    // Jump tables (see analyzer.cpp): candidates found in accepted code, and
    // the RAM pointers JMP (ind) goes through.
    struct Table {
        uint32_t from;    // key of the code that loads from the table
        uint16_t pointer; // the RAM pointer it fills (unused for the RTS form)
        uint16_t lo, hi;  // address of entry 0's low and high byte
        bool minus_one;   // entries are address - 1 (pushed for RTS)
        bool read;
    };
    std::vector<Table> tables_;
    std::set<uint16_t> jump_pointers_;
};

} // namespace nesrecomp
