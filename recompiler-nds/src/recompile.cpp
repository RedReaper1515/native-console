#include "recompile.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <ostream>

#include "emitter.h"
#include "analysis.h"

namespace fs = std::filesystem;

namespace ndsrecomp {

bool valid_project_name(const std::string &name)
{
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    for (char ch : name) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') return false;
    }
    return true;
}

std::string find_runtime_dir(const std::string &argv0, const std::string &arg, const std::string &compiled_default)
{
    auto usable = [](const fs::path &dir) {
        std::error_code ec;
        return !dir.empty() && fs::exists(dir / "include" / "nds.h", ec) && fs::exists(dir / "src" / "nds.c", ec);
    };
    if (!arg.empty()) return usable(arg) ? arg : std::string();

    std::vector<fs::path> candidates;
    if (const char *env = std::getenv("NDSRECOMP_RUNTIME_DIR")) {
        if (*env) candidates.emplace_back(env);
    }
    std::error_code ec;
    fs::path exe = fs::absolute(fs::path(argv0), ec);
    if (!ec && exe.has_parent_path()) {
        candidates.push_back(exe.parent_path() / "runtime-nds");
        candidates.push_back(exe.parent_path().parent_path().parent_path() / "runtime-nds");
    }
    if (!compiled_default.empty()) candidates.emplace_back(compiled_default);
    for (const fs::path &dir : candidates) {
        if (usable(dir)) return dir.string();
    }
    return std::string();
}

bool recompile(const Rom &rom, const ModuleSet &modules, const RecompileOptions &options, std::ostream &log,
               std::string &error)
{
    if (!valid_project_name(options.name)) {
        error = "--name must be a C identifier, not \"" + options.name + "\"";
        return false;
    }
    if (options.runtime_dir.empty()) {
        error = "cannot find the runtime sources (runtime-nds); pass --runtime DIR";
        return false;
    }

    AnalysisOptions analysis;
    analysis.symbols_dir = options.symbols_dir;
    analysis.trace_files = options.trace_files;
    analysis.pointer_scan = options.pointer_scan;
    analysis.bios = options.bios;
    Analysis program;
    if (!build_analysis(rom, modules, analysis, program, error)) return false;

    EmitOptions emit;
    emit.out_dir = options.out_dir;
    emit.name = options.name;
    emit.runtime_dir = options.runtime_dir;
    emit.rom_path = options.rom_path;
    emit.instructions_per_file = options.instructions_per_file;
    emit.inline_handlers = options.inline_handlers;
    EmitStats stats;
    if (!emit_project(rom, program, emit, stats, error)) return false;

    char line[256];
    for (const std::string &note : program.notes) log << note << "\n";
    log << "image       cpu   functions  instructions  from symbols  from calls  from pointers  from trace\n";
    for (const auto &image : program.images) {
        size_t instructions = 0;
        for (const Function &fn : image->analyzer->functions) instructions += fn.insns.size();
        if (image->analyzer->functions.empty()) continue;
        std::snprintf(line, sizeof line, "%-10s  %s  %9zu  %12zu  %12zu  %10zu  %13zu  %10zu\n",
                      image->image.name.c_str(), image->image.arm7 ? "arm7" : "arm9",
                      image->analyzer->functions.size(), instructions, image->symbol_entries,
                      image->external_entries, image->pointer_entries, image->analyzer->trace_entries);
        log << line;
    }
    std::snprintf(line, sizeof line,
                  "%zu functions, %zu instructions (%.2f MiB of code) in %zu files; entry points: arm9 %zu, arm7 %zu\n",
                  stats.functions, stats.instructions, static_cast<double>(stats.code_bytes) / (1024.0 * 1024.0),
                  stats.files, stats.entries[0], stats.entries[1]);
    log << line;
    log << "project written to " << options.out_dir << "\n";
    return true;
}

} // namespace ndsrecomp
