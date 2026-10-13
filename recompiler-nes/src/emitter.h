#pragma once

#include <cstddef>
#include <string>

#include "analyzer.h"
#include "rom.h"

namespace nesrecomp {

struct EmitOptions {
    std::string name;        // C identifier: executable and file prefix
    std::string out_dir;
    std::string runtime_dir; // runtime-nes source tree to snapshot
    size_t functions_per_file = 120;
    bool inline_handlers = false; // see NES_RC_INLINE_OPS in nes_cpu_impl.h
};

struct EmitStats {
    size_t files = 0;
    size_t functions = 0;
    size_t entries = 0;
    size_t instructions = 0;
};

bool emit_project(const Rom &rom, const Analyzer &an, const EmitOptions &options, EmitStats &stats, std::string &error);

} // namespace nesrecomp
