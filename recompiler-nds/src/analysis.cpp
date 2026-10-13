#include "analysis.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "nds_bios_data.h"
}

namespace ndsrecomp {

namespace {

// A module name as a C identifier.
std::string identifier(const std::string &name)
{
    std::string out;
    for (char ch : name) out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "m" + out;
    return out;
}

// Is there a function, or a label, of this instruction set at the address?
bool symbol_starts_code(const DsdModule &symbols, uint32_t key)
{
    if (const DsdFunction *fn = symbols.function_at(key_addr(key))) return fn->thumb == key_thumb(key);
    // Labels are few (a call into the middle of a function): look them up
    // in the symbol list, which is in address order.
    auto it = std::lower_bound(symbols.symbols.begin(), symbols.symbols.end(), key_addr(key),
                               [](const DsdSymbol &s, uint32_t address) { return s.address < address; });
    for (; it != symbols.symbols.end() && it->address == key_addr(key); ++it) {
        if (it->kind == DsdSymbolKind::Label && it->thumb == key_thumb(key)) return true;
    }
    return false;
}

const DsdModule *symbols_for(const DsdConfig &config, const Module &module)
{
    switch (module.kind) {
    case ModuleKind::Arm9Main: return config.find(DsdModuleKind::Main);
    case ModuleKind::Itcm: return config.find(DsdModuleKind::Itcm);
    case ModuleKind::Arm9Autoload: return config.find(DsdModuleKind::Autoload, module.autoload_index);
    case ModuleKind::Arm9Overlay: return config.find(DsdModuleKind::Overlay, module.overlay_id);
    default: return nullptr; // ds-decomp describes the ARM9 side only
    }
}

// "CPU ADDRESS a|t [COUNT [OPCODE]]" lines, as nds_run --trace-out writes
// them.
bool read_trace(const std::string &path, std::vector<TraceKey> keys[2], std::string &error)
{
    std::ifstream in(path);
    if (!in) {
        error = "cannot read trace " + path;
        return false;
    }
    std::string line;
    size_t number = 0;
    while (std::getline(in, line)) {
        number++;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        int cpu = 0;
        std::string addr, mode;
        if (!(fields >> cpu >> addr >> mode) || (cpu != 9 && cpu != 7) || (mode != "a" && mode != "t")) {
            error = path + ":" + std::to_string(number) +
                    ": not a trace line (want \"9|7 ADDRESS a|t [COUNT [OPCODE]]\")";
            return false;
        }
        auto hex_field = [&](const std::string &text, uint32_t &value) {
            try {
                size_t used = 0;
                value = static_cast<uint32_t>(std::stoul(text, &used, 16));
                return used == text.size();
            } catch (const std::exception &) {
                return false;
            }
        };
        TraceKey trace;
        uint32_t value = 0;
        if (!hex_field(addr, value)) {
            error = path + ":" + std::to_string(number) + ": bad address " + addr;
            return false;
        }
        trace.key = make_key(value, mode == "t");
        std::string count, opcode;
        if (fields >> count >> opcode) {
            if (!hex_field(opcode, trace.opcode)) {
                error = path + ":" + std::to_string(number) + ": bad opcode " + opcode;
                return false;
            }
            trace.has_opcode = true;
        }
        keys[cpu == 7 ? 1 : 0].push_back(trace);
    }
    return true;
}

// A copy of part of an image at another address.
struct Alias {
    size_t image;    // index of the image the bytes are from
    uint32_t base;   // where the copy starts
    uint32_t offset; // of the copy's first byte in the image
    uint32_t size;
    size_t matched;  // traced instructions that agree with it
};

// Code a program copies somewhere else before it runs it (a homebrew
// start-up routine moving the ARM7 program into work RAM, say) is still an
// image's bytes, at another address. A trace shows where: instructions that
// ran at addresses where no image has them, whose opcodes sit in an image a
// fixed distance away. The distance most of them agree on gives a copy of
// that image, cut down to the pages the trace saw run. It can cost nothing
// but space: like all compiled code it runs only while memory holds its
// bytes.
std::vector<Alias> find_aliases(const std::vector<std::unique_ptr<AnalysedImage>> &images,
                                const std::vector<TraceKey> &keys, bool arm7)
{
    const size_t kMinMatches = 16; // fewer could be chance
    const size_t kMaxPlaces = 64;  // of one opcode in one image: a common opcode says little
    std::vector<Alias> found;

    auto in_place = [&](const TraceKey &t) {
        const uint32_t size = key_thumb(t.key) ? 2u : 4u;
        for (const auto &entry : images) {
            const CodeImage &image = entry->image;
            if (image.arm7 != arm7 || !image.contains(key_addr(t.key), size)) continue;
            if (t.opcode == (size == 2u ? image.read16(key_addr(t.key)) : image.read32(key_addr(t.key)))) return true;
        }
        return false;
    };
    // ARM instructions only: a 16-bit opcode alone is too common to vote.
    std::vector<TraceKey> lost;
    for (const TraceKey &t : keys) {
        const uint32_t addr = key_addr(t.key);
        if (!t.has_opcode || key_thumb(t.key) || (addr & 3u)) continue;
        if (arm7 ? addr < 0x4000u : addr >= 0xFFFF0000u) continue; // the BIOS
        if (!in_place(t)) lost.push_back(t);
    }

    for (size_t index = 0; index < images.size() && lost.size() >= kMinMatches; index++) {
        const CodeImage &image = images[index]->image;
        if (image.arm7 != arm7) continue;
        for (int round = 0; round < 4 && lost.size() >= kMinMatches; round++) {
            std::unordered_set<uint32_t> wanted;
            for (const TraceKey &t : lost) wanted.insert(t.opcode);
            std::unordered_map<uint32_t, std::vector<uint32_t>> places; // opcode -> addresses in the image
            for (uint32_t at = (image.base + 3u) & ~3u; image.contains(at, 4); at += 4) {
                const uint32_t op = image.read32(at);
                if (!wanted.count(op)) continue;
                std::vector<uint32_t> &list = places[op];
                if (list.size() <= kMaxPlaces) list.push_back(at);
            }
            std::unordered_map<int64_t, size_t> votes; // distance -> instructions that fit it
            for (const TraceKey &t : lost) {
                const auto it = places.find(t.opcode);
                if (it == places.end() || it->second.size() > kMaxPlaces) continue;
                for (uint32_t at : it->second) votes[static_cast<int64_t>(key_addr(t.key)) - at]++;
            }
            int64_t delta = 0;
            size_t best = 0;
            for (const auto &vote : votes) {
                if (vote.second > best || (vote.second == best && vote.first < delta)) {
                    best = vote.second;
                    delta = vote.first;
                }
            }
            if (best < kMinMatches || delta == 0) break;

            // Everything the trace ran inside the image moved by that
            // distance must agree with it, not only what voted.
            size_t inside = 0, matched = 0;
            uint32_t low = 0xFFFFFFFFu, high = 0;
            std::vector<TraceKey> rest;
            for (const TraceKey &t : lost) {
                const int64_t source = static_cast<int64_t>(key_addr(t.key)) - delta;
                const bool within = source >= image.base && source + 4 <= static_cast<int64_t>(image.end());
                if (within) inside++;
                if (within && image.read32(static_cast<uint32_t>(source)) == t.opcode) {
                    matched++;
                    low = std::min(low, key_addr(t.key));
                    high = std::max(high, key_addr(t.key) + 4u);
                } else {
                    rest.push_back(t);
                }
            }
            if (matched < kMinMatches || matched * 10 < inside * 8) break;

            // Whole pages around what ran, within the image.
            const int64_t copy_low = std::max<int64_t>(low & ~0xFFFu, static_cast<int64_t>(image.base) + delta);
            const int64_t copy_high = std::min<int64_t>((static_cast<int64_t>(high) + 0xFFF) & ~static_cast<int64_t>(0xFFF),
                                                        static_cast<int64_t>(image.end()) + delta);
            Alias alias;
            alias.image = index;
            alias.base = static_cast<uint32_t>(copy_low);
            alias.offset = static_cast<uint32_t>(copy_low - delta - image.base);
            alias.size = static_cast<uint32_t>(copy_high - copy_low);
            alias.matched = matched;
            found.push_back(alias);
            lost.swap(rest);
        }
    }
    return found;
}

} // namespace

size_t Analysis::function_count() const
{
    size_t n = 0;
    for (const auto &image : images) n += image->analyzer->functions.size();
    return n;
}

size_t Analysis::instruction_count() const
{
    size_t n = 0;
    for (const auto &image : images) {
        for (const Function &fn : image->analyzer->functions) n += fn.insns.size();
    }
    return n;
}

bool build_analysis(const Rom &rom, const ModuleSet &modules, const AnalysisOptions &options, Analysis &out,
                   std::string &error)
{
    out.images.clear();
    out.notes.clear();
    const bool have_symbols = !options.symbols_dir.empty();
    if (have_symbols && !out.config.load(options.symbols_dir, error)) return false;

    std::vector<TraceKey> trace_keys[2];
    for (const std::string &path : options.trace_files) {
        if (!read_trace(path, trace_keys, error)) return false;
    }

    std::set<std::string> names;
    auto add_image = [&](const std::string &name, bool arm7, uint32_t base, const uint8_t *bytes, size_t size,
                         AnalyzerOptions analysis, const DsdModule *symbols) {
        auto entry = std::make_unique<AnalysedImage>();
        entry->image.name = identifier(name);
        while (!names.insert(entry->image.name).second) entry->image.name += "_";
        entry->image.arm7 = arm7;
        entry->image.base = base;
        entry->image.bytes.assign(bytes, bytes + size);
        entry->symbols = symbols;
        analysis.trace_keys = trace_keys[arm7 ? 1 : 0];
        entry->analyzer = std::make_unique<Analyzer>(entry->image, std::move(analysis));
        out.images.push_back(std::move(entry));
    };

    // Modules that hold no code but may hold pointers to it: the SDK's 3D
    // library keeps its tables of rendering functions in the DTCM block.
    std::vector<const Module *> data_modules;

    for (const Module &module : modules.modules) {
        if (module.bytes.empty()) continue;
        if (module.kind == ModuleKind::Dtcm) { // data only
            data_modules.push_back(&module);
            continue;
        }
        AnalyzerOptions analysis;
        const DsdModule *symbols = have_symbols ? symbols_for(out.config, module) : nullptr;
        size_t symbol_entries = 0;
        // A configuration for another game, or another revision of this
        // one, would only plant entry points in the wrong places.
        if (symbols && symbols->hash != module.dsd_hash) {
            error = "the symbols in " + options.symbols_dir + " are not for this ROM: module " + module.name +
                    " has hash " + hex(module.dsd_hash, 16) + ", the configuration expects " +
                    hex(symbols->hash, 16);
            return false;
        }

        if (module.kind == ModuleKind::Arm9Main) {
            analysis.entries.push_back(make_key(rom.header.arm9.entry & ~1u, rom.header.arm9.entry & 1u));
        } else if (module.kind == ModuleKind::Arm7Main) {
            analysis.entries.push_back(make_key(rom.header.arm7.entry & ~1u, rom.header.arm7.entry & 1u));
        }
        // An overlay's static initialisers: a table of function pointers the
        // SDK calls when it has loaded the overlay.
        for (uint64_t at = module.static_init_start; at + 4 <= module.static_init_end; at += 4) {
            if (!module.contains(static_cast<uint32_t>(at), 4)) break;
            uint32_t pointer = module.read32(static_cast<uint32_t>(at));
            if (pointer) analysis.entries.push_back(make_key(pointer & ~1u, pointer & 1u));
            analysis.data_words.push_back(static_cast<uint32_t>(at));
        }
        if (symbols) {
            for (const DsdFunction &fn : symbols->functions) {
                analysis.entries.push_back(make_key(fn.address, fn.thumb));
                symbol_entries++;
            }
            for (const DsdSection &section : symbols->sections) {
                if (section.kind != DsdSectionKind::Code) analysis.data_ranges.emplace_back(section.start, section.end);
            }
            // Data inside a code section (the SDK's build information, for
            // one); data symbols elsewhere are inside the sections above.
            for (const DsdDataRange &data : symbols->data) {
                const DsdSection *section = symbols->section_at(data.address);
                if (data.size && section && section->kind == DsdSectionKind::Code) {
                    analysis.data_ranges.emplace_back(data.address, data.address + data.size);
                }
            }
            // Labels: places inside functions that are called directly.
            for (const DsdSymbol &symbol : symbols->symbols) {
                if (symbol.kind == DsdSymbolKind::Label) {
                    analysis.entries.push_back(make_key(symbol.address, symbol.thumb));
                }
            }
            for (const DsdRelocation &reloc : symbols->relocations) {
                if (reloc.kind == DsdRelocKind::Load || reloc.kind == DsdRelocKind::OverlayId ||
                    reloc.kind == DsdRelocKind::LinkTimeConst) {
                    analysis.data_words.push_back(reloc.from);
                }
            }
        }
        add_image(module.name, module_is_arm7(module.kind), module.load_address, module.bytes.data(),
                  module.bytes.size(), std::move(analysis), symbols);
        out.images.back()->symbol_entries = symbol_entries;
    }

    // Copies of the images at the other addresses a trace saw them run at.
    for (int cpu = 0; cpu < 2; cpu++) {
        const std::vector<Alias> aliases = find_aliases(out.images, trace_keys[cpu], cpu == 1);
        for (const Alias &alias : aliases) {
            // Copied out first: add_image() grows the list the source is in.
            const std::string source = out.images[alias.image]->image.name;
            const std::vector<uint8_t> bytes(out.images[alias.image]->image.bytes.begin() + alias.offset,
                                             out.images[alias.image]->image.bytes.begin() + alias.offset + alias.size);
            add_image(source + "_at_" + hex(alias.base, 8).substr(2), cpu == 1, alias.base, bytes.data(), bytes.size(),
                      AnalyzerOptions(), nullptr);
            out.notes.push_back(std::string(cpu == 1 ? "arm7" : "arm9") + ": " + std::to_string(alias.matched) +
                                " traced instructions ran in a copy of " + source + " at " + hex(alias.base) + " (" +
                                hex(alias.size) + " bytes): compiled there as " + out.images.back()->image.name);
        }
    }

    if (options.bios) {
        // The exception vectors; everything else in a BIOS is reached from
        // them or through its table of software interrupts, which the
        // pointer scan reads.
        static const uint32_t kVectors[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x18, 0x1C };
        AnalyzerOptions bios9, bios7;
        for (uint32_t vector : kVectors) {
            bios9.entries.push_back(make_key(0xFFFF0000u + vector, false));
            bios7.entries.push_back(make_key(vector, false));
        }
        add_image("bios9", false, 0xFFFF0000u, nds_bios_arm9, NDS_BIOS_ARM9_CODE_SIZE, std::move(bios9), nullptr);
        add_image("bios7", true, 0x00000000u, nds_bios_arm7, NDS_BIOS_ARM7_CODE_SIZE, std::move(bios7), nullptr);
    }

    for (auto &image : out.images) image->analyzer->seed();

    // Is `key` a place to start a function in `image`? With symbols, only
    // where they say a function of that instruction set starts.
    auto acceptable = [](const AnalysedImage &image, uint32_t key) {
        return !image.symbols || symbol_starts_code(*image.symbols, key);
    };

    // Pass calls and branches that leave an image to the images of the same
    // processor that have code at the target, until nothing new turns up.
    // Overlays share addresses, so one target can land in several.
    std::vector<std::set<uint32_t>> passed(out.images.size());
    auto pass_external = [&]() {
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t from = 0; from < out.images.size(); from++) {
                std::vector<uint32_t> fresh;
                for (uint32_t key : out.images[from]->analyzer->external_targets) {
                    if (passed[from].insert(key).second) fresh.push_back(key);
                }
                for (uint32_t key : fresh) {
                    for (size_t to = 0; to < out.images.size(); to++) {
                        AnalysedImage &target = *out.images[to];
                        if (to == from || target.image.arm7 != out.images[from]->image.arm7) continue;
                        if (!target.image.contains(key_addr(key), key_thumb(key) ? 2u : 4u)) continue;
                        if (!acceptable(target, key)) continue;
                        if (target.analyzer->add_entry(key)) {
                            target.external_entries++;
                            changed = true;
                        }
                    }
                }
            }
        }
    };
    pass_external();

    // Function pointers in data: any aligned word, in any image of the same
    // processor, that points at what looks like the start of a function.
    // Images with symbols need none of this.
    if (options.pointer_scan) {
        auto scan_word = [&](uint32_t value, bool arm7) {
            for (auto &target : out.images) {
                if (target->symbols || target->image.arm7 != arm7) continue;
                if (!target->analyzer->looks_like_function(value)) continue;
                if (target->analyzer->add_entry(make_key(value & ~1u, value & 1u))) target->pointer_entries++;
            }
        };
        for (const auto &source : out.images) {
            const CodeImage &from = source->image;
            for (uint32_t at = (from.base + 3u) & ~3u; from.contains(at, 4); at += 4) {
                scan_word(from.read32(at), from.arm7);
            }
        }
        for (const Module *module : data_modules) {
            for (uint32_t at = (module->load_address + 3u) & ~3u; module->contains(at, 4); at += 4) {
                scan_word(module->read32(at), module_is_arm7(module->kind));
            }
        }
        pass_external();

        // Functions that push nothing are told from data by what surrounds
        // them, which the functions found so far settle. Finding one can
        // settle the next (a row of veneers), so the words that point into
        // an image are gone through in address order of what they point at,
        // until a round finds nothing.
        std::vector<uint32_t> candidates[2];
        auto collect = [&](uint32_t value, bool arm7) {
            for (const auto &target : out.images) {
                if (target->symbols || target->image.arm7 != arm7) continue;
                if (target->image.contains(value & ~1u, 2)) {
                    candidates[arm7 ? 1 : 0].push_back(value);
                    return;
                }
            }
        };
        for (const auto &source : out.images) {
            const CodeImage &from = source->image;
            for (uint32_t at = (from.base + 3u) & ~3u; from.contains(at, 4); at += 4) {
                collect(from.read32(at), from.arm7);
            }
        }
        for (const Module *module : data_modules) {
            for (uint32_t at = (module->load_address + 3u) & ~3u; module->contains(at, 4); at += 4) {
                collect(module->read32(at), module_is_arm7(module->kind));
            }
        }
        for (auto &list : candidates) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (int cpu = 0; cpu < 2; cpu++) {
                for (uint32_t value : candidates[cpu]) {
                    for (auto &target : out.images) {
                        if (target->symbols || target->image.arm7 != (cpu == 1)) continue;
                        if (!target->analyzer->looks_like_leaf(value)) continue;
                        if (target->analyzer->add_entry(make_key(value & ~1u, value & 1u))) {
                            target->pointer_entries++;
                            changed = true;
                        }
                    }
                }
            }
            pass_external();
        }
    }

    for (auto &image : out.images) image->analyzer->finish();
    // Trace keys can reveal more calls out of an image.
    pass_external();
    for (auto &image : out.images) image->analyzer->finish();
    return true;
}

} // namespace ndsrecomp
