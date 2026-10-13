// n64recomp: recompile a Nintendo 64 ROM into a C project that links the
// N64 runtime.
//
//   n64recomp ROM -o OUTDIR [--name NAME] [--runtime DIR] [--trace FILE]...
//             [--profile FILE [--profile-frames N]] [--functions-per-file N]
//             [--max-function N] [--inline-handlers] [--verbose]
//   n64recomp ROM --profile FILE [--profile-frames N]
//   n64recomp --version | --version-json
//
// The generated project embeds the ROM and refuses to run recompiled code
// against a ROM whose SHA-256 differs from the one it was built from.
// Code is compiled from RDRAM as the boot leaves it, and from the RDRAM
// pages a trace recorded (the runtime's --trace-out, or --profile, which
// first plays the ROM with the interpreter under a built-in input script
// for N frames, default 3000). Every compiled function carries the bytes it
// was compiled from and runs only while RDRAM holds them.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "analyzer.h"
#include "emitter.h"
#include "image.h"

extern "C" {
#include "n64_runner.h"
}

#ifndef N64RECOMP_DEFAULT_RUNTIME_DIR
#define N64RECOMP_DEFAULT_RUNTIME_DIR ""
#endif
#ifndef N64RECOMP_VERSION
#define N64RECOMP_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using namespace n64recomp;

static void print_version_json()
{
    std::cout << "{\"schema\":\"n64recomp.version\",\"schema_version\":1,"
              << "\"version\":\"" << N64RECOMP_VERSION << "\","
              << "\"abis\":{},"
              << "\"features\":[\"embedded-rom\",\"external-runtime-snapshot\",\"profile\",\"trace\",\"ram-code-check\"]}\n";
}

// The runtime-n64 tree to snapshot: --runtime if given; else
// $N64RECOMP_RUNTIME_DIR, runtime-n64 next to the executable (a release),
// two levels up (build/bin in a checkout), then the checkout it was built
// from.
static std::string find_runtime_dir(const char *argv0, const std::string &explicit_dir)
{
    auto usable = [](const fs::path &dir) {
        std::error_code ec;
        return !dir.empty() && fs::is_regular_file(dir / "include" / "n64_recomp.h", ec);
    };
    if (!explicit_dir.empty()) return usable(explicit_dir) ? explicit_dir : std::string();
    std::vector<fs::path> candidates;
    if (const char *env = std::getenv("N64RECOMP_RUNTIME_DIR"); env && *env) candidates.emplace_back(env);
    std::error_code ec;
    const fs::path exe_dir = fs::absolute(argv0, ec).parent_path();
    if (!ec) {
        candidates.push_back(exe_dir / "runtime-n64");
        candidates.push_back(exe_dir.parent_path().parent_path() / "runtime-n64");
    }
    candidates.emplace_back(N64RECOMP_DEFAULT_RUNTIME_DIR);
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
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "n64_" + out;
    return out;
}

static int usage()
{
    std::cerr << "usage: n64recomp ROM -o OUTDIR [--name NAME] [--runtime DIR] [--trace FILE]...\n"
                 "                 [--profile FILE [--profile-frames N]] [--functions-per-file N]\n"
                 "                 [--max-function N] [--inline-handlers] [--verbose]\n"
                 "       n64recomp ROM --profile FILE [--profile-frames N]\n"
                 "       n64recomp --version | --version-json\n";
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << N64RECOMP_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--version-json") {
        print_version_json();
        return 0;
    }
    std::string rom_path, out_dir, name, profile_path, runtime_arg;
    uint64_t profile_frames = 3000;
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
        else if (a == "--profile") profile_path = value();
        else if (a == "--profile-frames") profile_frames = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--inline-handlers") eopts.inline_handlers = true;
        else if (a == "--verbose") aopts.verbose = true;
        else if (!a.empty() && a[0] != '-' && rom_path.empty()) rom_path = a;
        else return usage();
    }
    if (rom_path.empty() || (out_dir.empty() && profile_path.empty())) return usage();
    if (eopts.functions_per_file == 0) eopts.functions_per_file = 60;
    if (aopts.max_function_insns == 0) aopts.max_function_insns = 3000;
    std::string runtime_dir;
    if (!out_dir.empty()) {
        runtime_dir = find_runtime_dir(argv[0], runtime_arg);
        if (runtime_dir.empty()) {
            std::cerr << "runtime-n64 not found (pass --runtime DIR)\n";
            return 1;
        }
    }

    Image image;
    std::string error;
    if (!image.load(rom_path, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    if (!profile_path.empty()) {
        std::error_code ec;
        fs::path parent = fs::path(profile_path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        uint64_t instructions = 0;
        if (!n64_profile_rom(image.rom.data(), image.rom.size(), profile_frames, profile_path.c_str(), &instructions)) {
            std::cerr << "profiling failed: cannot write " << profile_path << "\n";
            return 1;
        }
        std::printf("profiled %llu frames (%llu instructions) into %s\n", (unsigned long long)profile_frames,
                    (unsigned long long)instructions, profile_path.c_str());
        if (out_dir.empty()) return 0;
        traces.push_back(profile_path);
    }
    for (const std::string &path : traces) {
        if (!image.add_trace(path, error)) {
            std::cerr << error << "\n";
            return 1;
        }
    }

    Analyzer analyzer(image, aopts);
    analyzer.run();

    eopts.name = sanitize(name.empty() ? fs::path(rom_path).stem().string() : name);
    eopts.out_dir = out_dir;
    eopts.runtime_dir = runtime_dir;
    EmitStats stats;
    if (!emit_project(image, analyzer, eopts, stats, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    std::printf("%s: \"%s\" %.4s CIC %u sha256 %.12s\n", rom_path.c_str(), image.cart.title, image.cart.id,
                image.cart.cic, image.sha256.c_str());
    std::printf("  %zu views of RDRAM (%zu trace entries), %zu functions, %zu instructions, %zu entry points\n",
                image.views.size(), analyzer.trace_roots, stats.functions, stats.instructions, stats.entries);
    std::printf("  %zu branches left to the interpreter (delay slots that cannot be compiled)\n", analyzer.bad);
    std::printf("  %zu identical functions from other views folded, %zu snapshot entries already in a whole image\n",
                analyzer.duplicates, analyzer.covered);
    std::printf("  wrote %zu source files (%zu bytes of code checked against RDRAM) to %s\n", stats.files,
                stats.block_bytes, out_dir.c_str());
    return 0;
}
