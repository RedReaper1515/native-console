// ROM in, C project out: the step behind `ndsrecomp ROM -o DIR --name ID`.
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

#include "modules.h"
#include "rom.h"

namespace ndsrecomp {

struct RecompileOptions {
    std::string out_dir;
    std::string name;        // project and executable name, a C identifier
    std::string runtime_dir; // runtime-nds sources, as find_runtime_dir() resolved them
    std::string rom_path;
    std::string symbols_dir; // a ds-decomp configuration, or empty
    std::vector<std::string> trace_files;
    bool pointer_scan = true;
    bool bios = true;
    bool inline_handlers = false;
    size_t instructions_per_file = 12000;
};

// Analyses every code image of the ROM and writes the project. A summary
// goes to `log`.
bool recompile(const Rom &rom, const ModuleSet &modules, const RecompileOptions &options, std::ostream &log,
               std::string &error);

// Where the runtime sources are: `arg` (--runtime) if given, then
// $NDSRECOMP_RUNTIME_DIR, then runtime-nds/ next to the executable (a release
// archive), two folders up from it (build/bin in a checkout), and last
// `compiled_default`. Empty if none of them holds include/nds.h.
std::string find_runtime_dir(const std::string &argv0, const std::string &arg, const std::string &compiled_default);

// Is `name` usable as a project name: a C identifier?
bool valid_project_name(const std::string &name);

} // namespace ndsrecomp
