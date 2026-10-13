// C source generation for a DS program: one C function per guest function,
// the per-processor entry tables with the hash of the bytes each function
// was compiled from, the ROM compiled in, and a CMake project around them
// with a snapshot of the runtime.
#pragma once

#include <cstddef>
#include <string>

#include "analysis.h"
#include "rom.h"

namespace ndsrecomp {

struct EmitOptions {
    std::string out_dir;
    std::string name;        // project and executable name, a C identifier
    std::string runtime_dir; // runtime-nds sources to snapshot
    std::string rom_path;    // the ROM, copied into the project and compiled in
    size_t instructions_per_file = 12000;
    // Fold the handlers into every instruction instead of calling one copy
    // per file: faster code, much slower builds.
    bool inline_handlers = false;
};

struct EmitStats {
    size_t functions = 0;
    size_t instructions = 0;
    size_t files = 0;
    size_t entries[2] = { 0, 0 }; // ARM9, ARM7
    size_t blocks = 0;
    size_t ranges = 0;
    size_t code_bytes = 0; // guest bytes compiled, counting shared code once per function
};

bool emit_project(const Rom &rom, const Analysis &program, const EmitOptions &options, EmitStats &stats,
                  std::string &error);

} // namespace ndsrecomp
