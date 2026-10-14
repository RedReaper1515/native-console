// A DS game as the recompiler sees it: every code image of both processors
// (the ARM9 program, ITCM, each overlay, the ARM7 program and its autoload
// blocks, and the two BIOS images the runtime boots with), each analysed,
// with what one image learns about another passed along: a call from the
// main program into an overlay makes an entry point there.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "analyzer.h"
#include "modules.h"
#include "rom.h"
#include "symbols.h"

namespace ndsrecomp {

struct AnalysisOptions {
    // A ds-decomp configuration (the directory holding config.yaml) for the
    // ARM9 side: its functions become entry points, its data sections and
    // relocated words are not decoded, and an address is taken as a function
    // only where it says there is one. Empty for none.
    std::string symbols_dir;
    // Files written by a game executable's --trace-out: addresses the
    // interpreter had to run.
    std::vector<std::string> trace_files;
    // Look for function pointers in data (images without symbols only).
    bool pointer_scan = true;
    // Compile the replacement BIOS images the runtime boots with.
    bool bios = true;
};

struct AnalysedImage {
    CodeImage image;
    std::unique_ptr<Analyzer> analyzer;
    const DsdModule *symbols = nullptr; // from the configuration, if it has this module
    size_t symbol_entries = 0;
    size_t external_entries = 0; // entry points other images supplied
    size_t pointer_entries = 0;  // found by the pointer scan
};

struct Analysis {
    DsdConfig config; // what `symbols` point into
    std::vector<std::unique_ptr<AnalysedImage>> images;
    std::vector<std::string> notes;

    size_t function_count() const;
    size_t instruction_count() const;
};

bool build_analysis(const Rom &rom, const ModuleSet &modules, const AnalysisOptions &options, Analysis &out,
                   std::string &error);

} // namespace ndsrecomp
