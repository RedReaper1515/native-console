// gbarecomp: recompile a GBA ROM into a C project that links the GBA runtime.
//
//   gbarecomp ROM -o OUTDIR [--name NAME] [--runtime DIR] [--trace FILE]...
//             [--profile FILE [--profile-frames N]]
//             [--no-pointer-scan] [--instructions-per-file N] [--functions-per-file N]
//             [--inline-handlers]
//
// The generated project embeds the ROM and refuses to run recompiled code
// against a ROM whose SHA-256 differs from the one it was built from.
// --trace FILE adds the code entry points a previous run saw (written by the
// runtime's --trace-out; one hex key per line, address | 1 for Thumb). The
// RAM images beside it (FILE.iwram, FILE.ewram) let code the game runs from
// RAM be compiled too. Repeatable.
// --profile FILE first runs the ROM with the interpreter for N frames
// (default 6000) under a built-in input script, writes that trace to FILE,
// and uses it like --trace. Without -o, gbarecomp stops after profiling.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "analyzer.h"
#include "emitter.h"
#include "gba_runner.h"
#include "rom.h"

#ifndef GBARECOMP_DEFAULT_RUNTIME_DIR
#define GBARECOMP_DEFAULT_RUNTIME_DIR ""
#endif
#ifndef GBARECOMP_VERSION
#define GBARECOMP_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using namespace gbarecomp;

// Machine-readable identity, recorded in release manifests.
static void print_version_json()
{
    std::cout << "{\"schema\":\"gbarecomp.version\",\"schema_version\":1,"
              << "\"version\":\"" << GBARECOMP_VERSION << "\","
              << "\"abis\":{},"
              << "\"features\":[\"embedded-rom\",\"external-runtime-snapshot\",\"profile\",\"trace\"]}\n";
}

// The runtime-gba tree to snapshot: --runtime if given; else
// $GBARECOMP_RUNTIME_DIR, runtime-gba next to the executable (a release),
// two levels up (build/bin in a checkout), then the checkout it was built from.
static std::string find_runtime_dir(const char *argv0, const std::string &explicit_dir)
{
    auto usable = [](const fs::path &dir) {
        std::error_code ec;
        return !dir.empty() && fs::is_regular_file(dir / "include" / "gba_recomp.h", ec);
    };
    if (!explicit_dir.empty()) return usable(explicit_dir) ? explicit_dir : std::string();

    std::vector<fs::path> candidates;
    if (const char *env = std::getenv("GBARECOMP_RUNTIME_DIR"); env && *env) candidates.emplace_back(env);
    std::error_code ec;
    const fs::path exe_dir = fs::absolute(argv0, ec).parent_path();
    if (!ec) {
        candidates.push_back(exe_dir / "runtime-gba");
        candidates.push_back(exe_dir.parent_path().parent_path() / "runtime-gba");
    }
    candidates.emplace_back(GBARECOMP_DEFAULT_RUNTIME_DIR);
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
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "gba_" + out;
    return out;
}

static int usage()
{
    std::cerr << "usage: gbarecomp ROM -o OUTDIR [--name NAME] [--runtime DIR] [--trace FILE]...\n"
                 "                 [--profile FILE [--profile-frames N]]\n"
                 "                 [--no-pointer-scan] [--instructions-per-file N] [--functions-per-file N]\n"
                 "                 [--inline-handlers]\n"
                 "       gbarecomp ROM --profile FILE [--profile-frames N]\n"
                 "       gbarecomp --version | --version-json\n";
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << GBARECOMP_VERSION << "\n";
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
        else if (a == "--no-pointer-scan") aopts.pointer_scan = false;
        else if (a == "--functions-per-file") eopts.functions_per_file = std::strtoul(value().c_str(), nullptr, 10);
        else if (a == "--instructions-per-file") eopts.instructions_per_file = std::strtoul(value().c_str(), nullptr, 10);
        else if (a == "--inline-handlers") eopts.inline_handlers = true;
        else if (a == "--trace") traces.push_back(value());
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
            std::cerr << "runtime-gba not found (pass --runtime DIR)\n";
            return 1;
        }
    }
    if (eopts.instructions_per_file == 0) eopts.instructions_per_file = 4000;

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
        if (!gba_profile_rom(rom.data.data(), rom.data.size(), profile_frames, profile_path.c_str(), &instructions)) {
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
        // RAM code is compiled from the images the runtime wrote beside the trace.
        if (fs::exists(path + ".iwram") && !rom.load_ram_images(path, error)) {
            std::cerr << error << "\n";
            return 1;
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

    std::printf("%s: \"%s\" %s sha256 %.12s\n", rom_path.c_str(), rom.title.c_str(), rom.game_code.c_str(),
                rom.sha256.c_str());
    std::printf("  %zu instructions decoded, %zu functions (%zu from pointer scan, %zu from traces), %zu entry points\n",
                analyzer.insns.size(), stats.functions, analyzer.pointer_entries, analyzer.trace_entries, stats.entries);
    if (stats.ram_functions) {
        std::printf("  %zu functions run from RAM (%zu bytes checked against live RAM before they run)\n",
                    stats.ram_functions, stats.ram_bytes);
    }
    std::printf("  %zu jump tables, %zu unresolved indirect branches, %zu literal/table words\n",
                analyzer.tables_found, analyzer.indirect_unresolved, analyzer.data_words.size());
    std::printf("  wrote %zu source files (%zu emitted instructions) to %s\n", stats.files, stats.instructions,
                out_dir.c_str());
    return 0;
}
