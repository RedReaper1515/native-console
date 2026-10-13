#pragma once

#include <string>

#include "analyzer.h"

namespace n64recomp {

struct EmitOptions {
    std::string name;        // the game id: C identifier, executable name
    std::string out_dir;
    std::string runtime_dir; // the runtime-n64 tree to snapshot
    size_t functions_per_file = 60;
    // Inline every handler into generated code, not only the small ones:
    // somewhat faster code, a build several times slower.
    bool inline_handlers = false;
};

struct EmitStats {
    size_t files = 0, functions = 0, entries = 0, instructions = 0, block_bytes = 0;
};

bool emit_project(const Image &image, const Analyzer &an, const EmitOptions &options, EmitStats &stats,
                  std::string &error);

// The C function for one analyzed function, written to `out` (used by the
// project writer and by the recompiler's own tests).
void emit_function(const Function &fn, size_t index, std::string &out, bool inline_handlers = false);
std::string function_name(const Function &fn, size_t index);

} // namespace n64recomp
