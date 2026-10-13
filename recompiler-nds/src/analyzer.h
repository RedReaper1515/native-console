// Code discovery for one DS code image (a module at the address it runs at,
// for one processor): which addresses are ARM or Thumb instructions, how
// control flows between them, and how they group into functions.
//
// Nothing here has to be right for the generated program to be correct. A
// native function runs only while guest memory holds exactly the bytes it
// was compiled from, and it implements those bytes as instructions from the
// address it was entered at, whatever they are. What the analysis decides is
// how much of the game runs natively and how much generated code there is:
// a missed function is interpreted, and data decoded as code is dead weight.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ndsrecomp {

// A code location: address with bit 0 set for Thumb (the same key the
// runtime's entry table uses).
inline uint32_t make_key(uint32_t addr, bool thumb) { return (addr & ~1u) | (thumb ? 1u : 0u); }
inline uint32_t key_addr(uint32_t key) { return key & ~1u; }
inline bool key_thumb(uint32_t key) { return (key & 1u) != 0; }

// A run of bytes at the address it executes at, for one processor.
struct CodeImage {
    std::string name;  // "main", "itcm", "ov003", "arm7", "bios9", ...: unique, a C identifier
    bool arm7 = false; // run by the ARM7 (ARMv4T) rather than the ARM9 (ARMv5TE)
    uint32_t base = 0;
    std::vector<uint8_t> bytes;

    int v5() const { return arm7 ? 0 : 1; }
    uint64_t end() const { return static_cast<uint64_t>(base) + bytes.size(); }
    bool contains(uint32_t addr, uint32_t size) const
    {
        return addr >= base && static_cast<uint64_t>(addr) + size <= end();
    }
    // The caller has checked contains().
    uint16_t read16(uint32_t addr) const
    {
        const uint8_t *p = bytes.data() + (addr - base);
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    uint32_t read32(uint32_t addr) const
    {
        const uint8_t *p = bytes.data() + (addr - base);
        return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
};

enum class Flow : uint8_t {
    Next,         // continues with the following instruction
    CondBranch,   // to target, or continues
    Branch,       // to target only
    Call,         // BL, BLX: target is a function; continues at the return site
    CallIndirect, // call through a register: continues at the return site
    Return,       // bx lr, pop {pc}, ldm {..pc}
    Indirect,     // jump through a register with an unknown target
    Table,        // jump table: to one of `table`, or continues if conditional
    Stop,         // undefined instruction, breakpoint, or end of decodable code
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
    bool exits_after = false; // MSR to CPSR: return to the run loop afterwards
};

struct Function {
    uint32_t entry = 0;          // key
    std::vector<uint32_t> insns; // keys in address order, all in the entry's instruction set
};

// An instruction a run interpreted. With the opcode it had there, it is
// taken only by the images that hold that opcode at the address: overlays
// share addresses.
struct TraceKey {
    uint32_t key = 0;
    uint32_t opcode = 0;
    bool has_opcode = false;
};

struct AnalyzerOptions {
    // Keys that start functions: the entry point, vectors, symbols.
    std::vector<uint32_t> entries;
    // Keys the runtime interpreted (nds_run --trace-out): each becomes a
    // place native code can be entered at and a place to discover code
    // from, and a function entry if no other function reaches it.
    std::vector<TraceKey> trace_keys;
    // [start, end) address ranges that are known not to be code.
    std::vector<std::pair<uint32_t, uint32_t>> data_ranges;
    // Addresses of words known to hold data (a relocated pointer, say).
    std::vector<uint32_t> data_words;
};

class Analyzer {
public:
    Analyzer(const CodeImage &image, AnalyzerOptions options);

    // Explore from the options' entries.
    void seed();
    // Add a function entry found from outside (a call from another image, a
    // pointer in data) and explore from it. False if it was known, lies
    // outside the image or in data, or is a misaligned ARM address.
    bool add_entry(uint32_t key);
    // Could a word holding `value` be a pointer to a function here? True if
    // it points at what looks like the start of one.
    bool looks_like_function(uint32_t value) const;
    // The same question for a function that pushes nothing (a getter, an
    // empty virtual function, a veneer): true if what is before `value`
    // ends something and what is at it runs into a return or a jump within
    // a few instructions. Depends on what has been explored so far.
    bool looks_like_leaf(uint32_t value) const;
    // Add the trace keys and group everything found into functions. Call
    // last; may be called again after more add_entry() calls.
    void finish();

    const CodeImage &image;
    AnalyzerOptions options;

    std::map<uint32_t, Insn> insns;      // key -> instruction
    std::set<uint32_t> function_entries; // keys
    std::set<uint32_t> labels;           // keys the run loop can enter at
    std::set<uint32_t> data_words;       // word addresses holding literals or tables
    std::vector<Function> functions;
    // Keys outside the image that code here calls or branches to. Whoever
    // owns the other images of this processor decides which of them the
    // target is in.
    std::set<uint32_t> external_targets;

    size_t tables_found = 0;
    size_t indirect_unresolved = 0;
    size_t trace_entries = 0; // trace keys that became function entries

private:
    void push(uint32_t key);
    bool add_function(uint32_t key);
    void call_target(uint32_t key);
    void branch_target(uint32_t key, bool from_thumb);
    void explore(uint32_t key);
    void drain();
    bool is_data(uint32_t addr, uint32_t size) const;
    void build_functions();
    std::vector<uint32_t> reach(uint32_t entry) const;

    std::vector<uint32_t> worklist_;
};

} // namespace ndsrecomp
