// nesrecomp: recompile a NES ROM into a C project that links the NES
// runtime.
//
//   nesrecomp ROM -o OUTDIR [--name NAME] [--runtime DIR] [--trace FILE]...
//             [--profile FILE [--profile-frames N]] [--functions-per-file N]
//             [--max-function N] [--speculation N] [--no-jump-tables]
//             [--inline-handlers] [--verbose]
//
// The generated project embeds the ROM and refuses to run recompiled code
// against a ROM whose SHA-256 differs from the one it was built from.
// --trace FILE adds the code entry points a previous run saw (written by
// the runtime's --trace-out; one hex key per line, PRG page << 16 | PC).
// A cartridge switches banks at run time, so a trace is how the recompiler
// learns which bank was mapped when code was reached through a switch.
// Repeatable.
// --profile FILE first runs the ROM with the interpreter for N frames
// (default 6000) under a built-in input script, writes that trace to FILE,
// and uses it like --trace. Without -o, nesrecomp stops after profiling.
// --speculation N limits how many guesses about the mapped bank (and jump
// table entries) exploration may stack up beyond code known to run. The
// default is no limit: a wrong guess is rejected when it does not look like
// code, can never run against the wrong bytes if it is kept, and the whole
// of PRG ROM bounds what can be decoded.
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
#include "nes_runner.h"
}

#ifndef NESRECOMP_DEFAULT_RUNTIME_DIR
#define NESRECOMP_DEFAULT_RUNTIME_DIR ""
#endif
#ifndef NESRECOMP_VERSION
#define NESRECOMP_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using namespace nesrecomp;

// Machine-readable identity, recorded in release manifests.
static void print_version_json()
{
    std::cout << "{\"schema\":\"nesrecomp.version\",\"schema_version\":1,"
              << "\"version\":\"" << NESRECOMP_VERSION << "\","
              << "\"abis\":{},"
              << "\"features\":[\"embedded-rom\",\"external-runtime-snapshot\",\"profile\",\"trace\"]}\n";
}

// The runtime-nes tree to snapshot: --runtime if given; else
// $NESRECOMP_RUNTIME_DIR, runtime-nes next to the executable (a release),
// two levels up (build/bin in a checkout), then the checkout it was built from.
static std::string find_runtime_dir(const char *argv0, const std::string &explicit_dir)
{
    auto usable = [](const fs::path &dir) {
        std::error_code ec;
        return !dir.empty() && fs::is_regular_file(dir / "include" / "nes_recomp.h", ec);
    };
    if (!explicit_dir.empty()) return usable(explicit_dir) ? explicit_dir : std::string();

    std::vector<fs::path> candidates;
    if (const char *env = std::getenv("NESRECOMP_RUNTIME_DIR"); env && *env) candidates.emplace_back(env);
    std::error_code ec;
    const fs::path exe_dir = fs::absolute(argv0, ec).parent_path();
    if (!ec) {
        candidates.push_back(exe_dir / "runtime-nes");
        candidates.push_back(exe_dir.parent_path().parent_path() / "runtime-nes");
    }
    candidates.emplace_back(NESRECOMP_DEFAULT_RUNTIME_DIR);
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
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "nes_" + out;
    return out;
}

static int usage()
{
    std::cerr << "usage: nesrecomp ROM -o OUTDIR [--name NAME] [--runtime DIR] [--trace FILE]...\n"
                 "                 [--profile FILE [--profile-frames N]] [--functions-per-file N]\n"
                 "                 [--max-function N] [--speculation N] [--no-jump-tables]\n"
                 "                 [--inline-handlers] [--verbose]\n"
                 "       nesrecomp ROM --profile FILE [--profile-frames N]\n"
                 "       nesrecomp --version | --version-json\n";
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << NESRECOMP_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--version-json") {
        print_version_json();
        return 0;
    }
    std::string rom_path, out_dir, name, profile_path, runtime_arg;
    uint64_t profile_frames = 6000;
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
        else if (a == "--no-jump-tables") aopts.jump_tables = false;
        else if (a == "--speculation") aopts.speculation_depth = static_cast<unsigned>(std::strtoul(value().c_str(), nullptr, 10));
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
            std::cerr << "runtime-nes not found (pass --runtime DIR)\n";
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

    if (!profile_path.empty()) {
        std::error_code ec;
        fs::path parent = fs::path(profile_path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        uint64_t instructions = 0;
        if (!nes_profile_rom(rom.file.data(), rom.file.size(), profile_frames, profile_path.c_str(), &instructions)) {
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
    }

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

    std::printf("%s: %s, %u KiB PRG in %u pages, sha256 %.12s\n", rom_path.c_str(), rom.info.board,
                rom.info.prg_size / 1024u, rom.pages(), rom.sha256.c_str());
    std::printf("  %zu instructions decoded (%.1f%% of PRG bytes start one), %zu functions (%zu from traces), %zu entry points\n",
                analyzer.insns.size(), 100.0 * static_cast<double>(analyzer.insns.size()) / rom.info.prg_size,
                stats.functions, analyzer.trace_entries, stats.entries);
    std::printf("  %zu jumps out of a page, %zu indirect jumps, %zu jump tables (%zu entries), %zu guessed regions rejected as data\n",
                analyzer.page_exits, analyzer.indirect_jumps, analyzer.tables_found, analyzer.table_entries,
                analyzer.regions_rejected);
    std::printf("  wrote %zu source files (%zu emitted instructions) to %s\n", stats.files, stats.instructions,
                out_dir.c_str());
    return 0;
}
