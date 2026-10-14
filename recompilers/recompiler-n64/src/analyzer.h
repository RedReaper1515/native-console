// Code discovery: which words of each view of RDRAM are VR4300 code, how
// control flows between them, and how they group into C functions.
//
// Exploration is recursive descent from addresses known to run: the boot
// entry point and the entries a trace recorded. It follows both ways of a
// branch, the target of a J, and the callee of a JAL (a function of its
// own); it stops at JR, ERET, SYSCALL and BREAK, and at a word that is not
// an instruction a game would run. Nothing is guessed: code reached only
// through a pointer is compiled when a trace says it ran, and until then
// the interpreter runs it. In a trace's whole image of RDRAM the descent
// crosses pages and reaches callees the traced run never called; a page
// snapshot is its own 4 KiB world, for code the image no longer holds.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "image.h"

extern "C" {
#include "n64_cpu_decode.h"
}

namespace n64recomp {

struct Insn {
    uint32_t vaddr = 0;
    uint32_t word = 0;
    N64Op op = N64_OP_invalid;
    uint16_t flags = 0;
    bool bad = false;    // not compiled: the run loop interprets it
    uint32_t target = 0; // static branch or jump target
    bool has_target = false;
};

struct Function {
    int view = 0;
    uint32_t entry = 0;
    uint32_t vp = 0; // vaddr - paddr, constant within the function
    std::map<uint32_t, Insn> insns;
    std::set<uint32_t> labels; // where the dispatcher may enter
    uint32_t lo = 0, hi = 0;   // vaddr range [lo, hi)
};

struct AnalyzerOptions {
    // Larger C functions compile superlinearly slower; code past the cap
    // continues in another function through the dispatcher.
    size_t max_function_insns = 3000;
    bool verbose = false;
};

class Analyzer {
public:
    Analyzer(const Image &image, AnalyzerOptions options) : image(image), options(options) {}
    void run();

    const Image &image;
    AnalyzerOptions options;
    std::vector<Function> functions;
    size_t instructions = 0;
    size_t trace_roots = 0;
    size_t bad = 0;
    size_t duplicates = 0; // functions folded into an identical one from another view
    size_t covered = 0;    // page-snapshot entries a whole image already held

private:
    struct Root {
        int view;
        uint32_t vaddr, vp;
    };
    bool decode(const Root &where, uint32_t vaddr, Insn &out) const;
    void explore(const Root &root, std::vector<Root> &queue);
    void add_label(size_t function, uint32_t vaddr);
    bool in_full_view(const Root &root);
    void merge_duplicates();
    std::vector<std::map<uint32_t, size_t>> owner_; // per view: vaddr -> function
};

} // namespace n64recomp
