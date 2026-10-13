// C emission for recompiled GBA programs.
#pragma once

#include <string>

#include "analyzer.h"
#include "rom.h"

namespace gbarecomp {

struct EmitOptions {
    std::string name;        // executable / project name (C identifier-safe)
    std::string out_dir;     // generated project directory
    std::string runtime_dir; // runtime-gba source tree to snapshot into the project
    /* Generated files hold about this many guest instructions each (a
     * function is never split), so their compile times are even. */
    size_t instructions_per_file = 4000;
    size_t functions_per_file = 0; /* > 0: the old split, this many functions a file */
    bool inline_handlers = false;  /* see GBA_RC_OUTLINE_HANDLERS in gba_cpu_impl.h */
};

struct EmitStats {
    size_t functions = 0;
    size_t instructions = 0;
    size_t entries = 0;
    size_t files = 0;
    size_t ram_functions = 0;
    size_t ram_bytes = 0;
};

bool emit_project(const Rom &rom, const Analyzer &analyzer, const EmitOptions &options, EmitStats &stats,
                  std::string &error);

} // namespace gbarecomp
