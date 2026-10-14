#pragma once

#include <cstddef>
#include <string>

#include "analyzer.h"
#include "rom.h"

namespace snesrecomp {

struct EmitOptions {
    std::string name;        // C identifier: executable and file prefix
    std::string out_dir;
    std::string runtime_dir; // runtime-snes source tree to snapshot
    size_t functions_per_file = 120;
    bool inline_handlers = false; // see SNES_RC_INLINE_OPS in snes_cpu_impl.h
};

struct EmitStats {
    size_t files = 0;
    size_t functions = 0;
    size_t entries = 0;
    size_t instructions = 0;
    size_t wram_functions = 0;
    size_t wram_bytes = 0;
};

bool emit_project(const Rom &rom, const Analyzer &an, const EmitOptions &options, EmitStats &stats, std::string &error);

} // namespace snesrecomp
