// Two decodes of the same ROM bytes from different alignments cannot both be
// real code. A seed that lands on the operand of `ld a,($c178)` decodes
// `ld a,b / pop bc` across the same bytes; the analyzer must keep the
// alignment that explains more bytes, exclude the other one, and report it.
#include "recompiler/analyzer.h"
#include "recompiler/rom.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

const gbrecomp::AnalysisDiagnostic* find_diagnostic(const gbrecomp::AnalysisResult& result,
                                                    const std::string& kind,
                                                    uint16_t address) {
    for (const auto& [id, diagnostic] : result.analysis_diagnostics) {
        (void)id;
        if (diagnostic.kind == kind && diagnostic.address == address) return &diagnostic;
    }
    return nullptr;
}

std::optional<gbrecomp::ROM> make_rom() {
    std::vector<uint8_t> bytes(4u * 0x4000, 0);
    bytes[0x147] = 0x01;  // MBC1
    bytes[0x148] = 0x01;  // 64 KiB
    bytes[0x100] = 0xC3;  // JP $0150
    bytes[0x101] = 0x50;
    bytes[0x102] = 0x01;
    // $0150: LD A,($C178) / AND A / JR Z,+2 / INC A / INC A / RET
    const uint8_t body[] = {0xFA, 0x78, 0xC1, 0xA7, 0x28, 0x02, 0x3C, 0x3C, 0xC9};
    for (size_t i = 0; i < sizeof(body); ++i) bytes[0x150 + i] = body[i];
    return gbrecomp::ROM::load_from_buffer(std::move(bytes), "overlap-resolution");
}

// `jr z,+1 / ld a,$af / halt`: the branch skips the load's opcode and runs
// its operand as `xor a`. Both alignments are real code reached from the reset
// entry, so resolution must keep the branch target.
std::optional<gbrecomp::ROM> make_skip_rom() {
    std::vector<uint8_t> bytes(4u * 0x4000, 0);
    bytes[0x147] = 0x01;  // MBC1
    bytes[0x148] = 0x01;  // 64 KiB
    bytes[0x100] = 0xC3;  // JP $0150
    bytes[0x101] = 0x50;
    bytes[0x102] = 0x01;
    const uint8_t body[] = {0x28, 0x01, 0x3E, 0xAF, 0x76, 0xC9};
    for (size_t i = 0; i < sizeof(body); ++i) bytes[0x150 + i] = body[i];
    return gbrecomp::ROM::load_from_buffer(std::move(bytes), "overlap-skip");
}

gbrecomp::AnalyzerOptions make_options(bool resolve) {
    gbrecomp::AnalyzerOptions options;
    options.aggressive_scan = false;
    options.analyze_all_banks = false;
    options.pointer_scan = gbrecomp::AnalyzerOptions::PointerScan::Off;
    options.classify_data_regions = false;
    options.max_instructions = 100;
    options.entry_points = {0x00000151u};  // seed inside the operand of the load
    options.resolve_overlapping_code = resolve;
    return options;
}

}  // namespace

int main() {
    auto rom = make_rom();
    if (!rom || !rom->is_valid()) {
        std::cerr << "failed to load fixture\n";
        return 2;
    }

    // Without resolution both alignments are compiled (historical behaviour).
    {
        const auto result = gbrecomp::analyze(*rom, make_options(false));
        if (result.get_instruction(0, 0x150) == nullptr || result.get_instruction(0, 0x151) == nullptr ||
            result.get_instruction(0, 0x152) == nullptr) {
            std::cerr << "unresolved analysis should keep both alignments\n";
            return 1;
        }
        if (find_diagnostic(result, "overlapping_decode_excluded", 0x151) != nullptr) {
            std::cerr << "unresolved analysis must not report exclusions\n";
            return 1;
        }
    }

    const auto result = gbrecomp::analyze(*rom, make_options(true));
    const gbrecomp::Instruction* load = result.get_instruction(0, 0x150);
    if (load == nullptr || load->length != 3) {
        std::cerr << "the real LD A,(nn) at $0150 was lost\n";
        return 1;
    }
    if (result.get_instruction(0, 0x151) != nullptr || result.get_instruction(0, 0x152) != nullptr) {
        std::cerr << "the misaligned decode inside the operand was kept\n";
        return 1;
    }
    for (uint16_t addr : {0x153, 0x154, 0x156, 0x157, 0x158}) {
        if (result.get_instruction(0, addr) == nullptr) {
            std::cerr << "shared instruction at " << std::hex << addr << " was lost\n";
            return 1;
        }
    }
    const auto* diagnostic = find_diagnostic(result, "overlapping_decode_excluded", 0x151);
    if (diagnostic == nullptr || diagnostic->status != "applied" || !diagnostic->has_related_address ||
        diagnostic->related_address != 0x150 || diagnostic->suggested_annotation.empty()) {
        std::cerr << "excluded decode was not reported against the kept alignment\n";
        return 1;
    }
    // The seed that landed inside the operand was a call target; no function
    // may remain there and the direct call bookkeeping must not bind to it.
    if (result.get_function(0, 0x151) != nullptr) {
        std::cerr << "a function was created at the excluded address\n";
        return 1;
    }

    // A direct branch from anchored code into an operand byte is not a
    // misaligned decode: both alignments stay compiled.
    {
        auto skip_rom = make_skip_rom();
        if (!skip_rom || !skip_rom->is_valid()) {
            std::cerr << "failed to load skip fixture\n";
            return 2;
        }
        gbrecomp::AnalyzerOptions options = make_options(true);
        options.entry_points.clear();
        const auto skip = gbrecomp::analyze(*skip_rom, options);
        if (skip.get_instruction(0, 0x152) == nullptr || skip.get_instruction(0, 0x153) == nullptr ||
            find_diagnostic(skip, "overlapping_decode_excluded", 0x153) != nullptr) {
            std::cerr << "the branch target inside the load operand was excluded\n";
            return 1;
        }
    }
    return 0;
}
