// Code discovery for GBA ROMs: which ROM addresses are ARM or Thumb code,
// how control flows between them, and how they group into functions.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "rom.h"

namespace gbarecomp {

// A code location: address with bit 0 set for Thumb (the same key the
// runtime's entry table uses).
inline uint32_t make_key(uint32_t addr, bool thumb) { return (addr & ~1u) | (thumb ? 1u : 0u); }
inline uint32_t key_addr(uint32_t key) { return key & ~1u; }
inline bool key_thumb(uint32_t key) { return (key & 1u) != 0; }

enum class Flow : uint8_t {
    Next,         // continues with the following instruction
    CondBranch,   // to target, or continues
    Branch,       // to target only
    Call,         // BL: target is a function; continues at the return site
    CallIndirect, // call through a register (mov lr, pc; bx rN): continues at the return site
    Return,       // bx lr, pop {pc}, ldm {..pc}
    Indirect,     // jump through a register with an unknown target
    Table,        // jump table: to one of `table`, or continues if conditional
    Stop,         // undefined instruction or end of decodable code
};

struct Insn {
    uint32_t addr = 0;
    uint32_t op = 0;
    bool thumb = false;
    uint8_t size = 4;
    Flow flow = Flow::Next;
    bool conditional = false; // ARM condition other than AL (a Table may fall through)
    uint32_t target = 0;      // key, for CondBranch/Branch/Call
    std::vector<uint32_t> table;
    bool exits_after = false; // MSR to CPSR: return to the dispatcher afterwards
};

struct Function {
    uint32_t entry = 0;          // key
    std::vector<uint32_t> insns; // keys in address order
};

struct AnalyzerOptions {
    bool pointer_scan = true;
    std::vector<uint32_t> extra_entries; // keys that start functions
    // Keys the runtime interpreted (gbarecomp --trace): each becomes a label
    // the dispatcher can enter at and a place to discover code from, and a
    // function entry if no other function reaches it.
    std::vector<uint32_t> trace_keys;
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
    std::set<uint32_t> data_words;       // word addresses holding literals or tables
    std::vector<Function> functions;

    size_t tables_found = 0;
    size_t indirect_unresolved = 0;
    size_t pointer_entries = 0;
    size_t trace_entries = 0; // trace keys that became function entries

private:
    void push(uint32_t key);
    void add_function(uint32_t key);
    void explore(uint32_t key);
    void drain();
    bool is_data(uint32_t addr, uint32_t size) const;
    void scan_pointers();
    void build_functions();
    std::vector<uint32_t> reach(uint32_t entry) const;

    std::vector<uint32_t> worklist_;
};

} // namespace gbarecomp
