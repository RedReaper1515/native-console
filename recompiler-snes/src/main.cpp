// snesrecomp: recompile a SNES ROM into a C project that links the SNES
// runtime.
//
//   snesrecomp ROM -o OUTDIR [--name NAME] [--runtime DIR] [--trace FILE]...
//              [--profile FILE [--profile-frames N]] [--functions-per-file N]
//              [--max-function N] [--speculation N] [--inline-handlers] [--verbose]
//
// The generated project embeds the ROM and refuses to run recompiled code
// against a ROM whose SHA-256 differs from the one it was built from.
// --trace FILE adds the code entry points a previous run saw (written by
// the runtime's --trace-out; one hex key per line, PC | mode << 24). The
// WRAM image beside it (FILE.wram) lets code the game runs from work RAM be
// compiled too. Repeatable.
// --profile FILE first runs the ROM with the interpreter for N frames
// (default 6000) under a built-in input script, writes that trace to FILE,
// and uses it like --trace. Without -o, snesrecomp stops after profiling.
// --speculation N limits how many guesses deep exploration goes beyond code
// known to run (default 4 with a trace, unlimited without one).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "analyzer.h"
#include "emitter.h"
#include "rom.h"

extern "C" {
#include "snes_runner.h"
}

#ifndef SNESRECOMP_DEFAULT_RUNTIME_DIR
#define SNESRECOMP_DEFAULT_RUNTIME_DIR ""
#endif
#ifndef SNESRECOMP_VERSION
#define SNESRECOMP_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using namespace snesrecomp;

// Machine-readable identity, recorded in release manifests.
static void print_version_json()
{
    std::cout << "{\"schema\":\"snesrecomp.version\",\"schema_version\":1,"
              << "\"version\":\"" << SNESRECOMP_VERSION << "\","
              << "\"abis\":{},"
              << "\"features\":[\"embedded-rom\",\"external-runtime-snapshot\",\"profile\",\"trace\"]}\n";
}

// The runtime-snes tree to snapshot: --runtime if given; else
// $SNESRECOMP_RUNTIME_DIR, runtime-snes next to the executable (a release),
// two levels up (build/bin in a checkout), then the checkout it was built from.
static std::string find_runtime_dir(const char *argv0, const std::string &explicit_dir)
{
    auto usable = [](const fs::path &dir) {
        std::error_code ec;
        return !dir.empty() && fs::is_regular_file(dir / "include" / "snes_recomp.h", ec);
    };
    if (!explicit_dir.empty()) return usable(explicit_dir) ? explicit_dir : std::string();

    std::vector<fs::path> candidates;
    if (const char *env = std::getenv("SNESRECOMP_RUNTIME_DIR"); env && *env) candidates.emplace_back(env);
    std::error_code ec;
    const fs::path exe_dir = fs::absolute(argv0, ec).parent_path();
    if (!ec) {
        candidates.push_back(exe_dir / "runtime-snes");
        candidates.push_back(exe_dir.parent_path().parent_path() / "runtime-snes");
    }
    candidates.emplace_back(SNESRECOMP_DEFAULT_RUNTIME_DIR);
    for (const fs::path &dir : candidates) {
        if (!usable(dir)) continue;
        const fs::path canonical = fs::weakly_canonical(dir, ec);
        return (ec ? dir : canonical).string();
    }
    return {};
}

static std::string sanitize(const std::string &s)
{
    std::string out;
    for (char ch : s) out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "snes_" + out;
    return out;
}

static int usage()
{
    std::cerr << "usage: snesrecomp ROM -o OUTDIR [--name NAME] [--runtime DIR] [--trace FILE]...\n"
                 "                  [--profile FILE [--profile-frames N]] [--functions-per-file N]\n"
                 "                  [--max-function N] [--speculation N] [--inline-handlers] [--verbose]\n"
                 "       snesrecomp ROM --profile FILE [--profile-frames N]\n"
                 "       snesrecomp --version | --version-json\n";
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << SNESRECOMP_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--version-json") {
        print_version_json();
        return 0;
    }
    std::string rom_path, out_dir, name, profile_path, runtime_arg;
    uint64_t profile_frames = 6000;
    bool speculation_set = false;
    std::vector<std::string> traces;
    AnalyzerOptions aopts;
    EmitOptions eopts;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "-o" || a == "--output") out_dir = value();
        else if (a == "--name") name = value();
        else if (a == "--runtime") runtime_arg = value();
        else if (a == "--functions-per-file") eopts.functions_per_file = std::strtoul(value().c_str(), nullptr, 10);
        else if (a == "--max-function") aopts.max_function_insns = std::strtoul(value().c_str(), nullptr, 10);
        else if (a == "--trace") traces.push_back(value());
        else if (a == "--verbose") aopts.verbose = true;
        else if (a == "--inline-handlers") eopts.inline_handlers = true;
        else if (a == "--speculation") {
            aopts.speculation_depth = static_cast<unsigned>(std::strtoul(value().c_str(), nullptr, 10));
            speculation_set = true;
        }
        else if (a == "--profile") profile_path = value();
        else if (a == "--profile-frames") profile_frames = std::strtoull(value().c_str(), nullptr, 10);
        else if (!a.empty() && a[0] != '-' && rom_path.empty()) rom_path = a;
        else return usage();
    }
    if (rom_path.empty() || (out_dir.empty() && profile_path.empty())) return usage();
    std::string runtime_dir;
    if (!out_dir.empty()) {
        runtime_dir = find_runtime_dir(argv[0], runtime_arg);
        if (runtime_dir.empty()) {
            std::cerr << "runtime-snes not found (pass --runtime DIR)\n";
            return 1;
        }
    }
    if (eopts.functions_per_file == 0) eopts.functions_per_file = 120;

    Rom rom;
    std::string error;
    if (!rom.load(rom_path, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (rom.machine()->cart.coprocessor) {
        std::cerr << rom_path << ": uses a " << rom.machine()->cart.coprocessor
                  << " coprocessor, which is not supported\n";
        return 1;
    }

    if (!profile_path.empty()) {
        std::error_code ec;
        fs::path parent = fs::path(profile_path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        uint64_t instructions = 0;
        if (!snes_profile_rom(rom.data.data(), rom.data.size(), profile_frames, profile_path.c_str(), &instructions)) {
            std::cerr << "profiling failed: cannot write " << profile_path << "\n";
            return 1;
        }
        std::printf("profiled %llu frames (%llu instructions) into %s\n", static_cast<unsigned long long>(profile_frames),
                    static_cast<unsigned long long>(instructions), profile_path.c_str());
        if (out_dir.empty()) return 0;
        traces.push_back(profile_path);
    }

    for (const std::string &path : traces) {
        std::ifstream trace(path);
        if (!trace) {
            std::cerr << "cannot read trace " << path << "\n";
            return 1;
        }
        std::string line;
        while (std::getline(trace, line)) {
            if (!line.empty() && line[0] != '#') aopts.trace_keys.push_back(std::strtoul(line.c_str(), nullptr, 16));
        }
        if (fs::exists(path + ".wram") && !rom.load_wram_image(path, error)) {
            std::cerr << error << "\n";
            return 1;
        }
    }

    // With a trace, code known to run is dense and deep guessing mostly
    // decodes data (KDL3: 48k instructions at depth 4, 198k unbounded).
    if (!speculation_set && !aopts.trace_keys.empty()) aopts.speculation_depth = 4;

    Analyzer analyzer(rom, aopts);
    analyzer.run();

    eopts.name = sanitize(name.empty() ? fs::path(rom_path).stem().string() : name);
    eopts.out_dir = out_dir;
    eopts.runtime_dir = runtime_dir;
    EmitStats stats;
    if (!emit_project(rom, analyzer, eopts, stats, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    std::printf("%s: \"%s\" sha256 %.12s\n", rom_path.c_str(), rom.title.c_str(), rom.sha256.c_str());
    std::printf("  %zu instructions decoded, %zu functions (%zu from traces), %zu entry points\n", analyzer.insns.size(),
                stats.functions, analyzer.trace_entries, stats.entries);
    if (stats.wram_functions) {
        std::printf("  %zu functions run from WRAM (%zu bytes checked against live WRAM before they run)\n",
                    stats.wram_functions, stats.wram_bytes);
    }
    std::printf("  %zu jump tables, %zu unresolved indirect jumps, %zu guessed regions rejected as data\n",
                analyzer.tables_found, analyzer.indirect_unresolved, analyzer.regions_rejected);
    std::printf("  wrote %zu source files (%zu emitted instructions) to %s\n", stats.files, stats.instructions,
                out_dir.c_str());
    return 0;
}
