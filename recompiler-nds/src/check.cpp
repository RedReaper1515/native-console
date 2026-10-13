#include "check.h"

namespace ndsrecomp {

namespace {

constexpr size_t kMaxProblems = 8;

// Sign-extends the low `bits` bits of `value`.
int32_t sign_extend(uint32_t value, unsigned bits)
{
    const uint32_t sign = 1u << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

// The module a relocation's target belongs to, or nullptr.
const Module *target_module(const ModuleSet &modules, DsdRelocTarget target, uint32_t index)
{
    // ModuleSet::find takes a negative index to mean "any": an index too
    // large for an int must not turn into one.
    if (index > 0x7FFFFFFFu) return nullptr;
    switch (target) {
    case DsdRelocTarget::Main: return modules.find(ModuleKind::Arm9Main);
    case DsdRelocTarget::Itcm: return modules.find(ModuleKind::Itcm);
    case DsdRelocTarget::Dtcm: return modules.find(ModuleKind::Dtcm);
    case DsdRelocTarget::Autoload: return modules.find(ModuleKind::Arm9Autoload, static_cast<int>(index));
    case DsdRelocTarget::Overlay:
    case DsdRelocTarget::Overlays: return modules.find(ModuleKind::Arm9Overlay, static_cast<int>(index));
    case DsdRelocTarget::None: break;
    }
    return nullptr;
}

// Is reloc.to inside every module the relocation names? Calls must land in
// initialised bytes; a load may also point into the BSS.
bool target_in_module(const ModuleSet &modules, const DsdRelocation &reloc, bool code, std::string &detail)
{
    auto inside = [&](const Module *module, const std::string &name) {
        if (!module) {
            detail = "names " + name + ", which the ROM does not have";
            return false;
        }
        const uint64_t limit = code ? module->end() : module->bss_end() + 1;
        if (reloc.to < module->load_address || reloc.to >= limit) {
            detail = "target " + hex(reloc.to) + " is outside " + name + " (" + hex(module->load_address) + "-" +
                     hex(module->bss_end()) + ")";
            return false;
        }
        return true;
    };
    switch (reloc.target) {
    case DsdRelocTarget::None: return true;
    case DsdRelocTarget::Main: return inside(target_module(modules, reloc.target, 0), "main");
    case DsdRelocTarget::Itcm: return inside(target_module(modules, reloc.target, 0), "itcm");
    case DsdRelocTarget::Dtcm: return inside(target_module(modules, reloc.target, 0), "dtcm");
    case DsdRelocTarget::Autoload:
        return inside(target_module(modules, reloc.target, reloc.autoload_index),
                      "autoload " + std::to_string(reloc.autoload_index));
    case DsdRelocTarget::Overlay:
    case DsdRelocTarget::Overlays:
        for (const uint16_t id : reloc.overlays) {
            if (!inside(target_module(modules, reloc.target, id), "overlay " + std::to_string(id))) return false;
        }
        return true;
    }
    return true;
}

void add_problem(DsdModuleCheck &check, const std::string &text)
{
    if (check.problems.size() < kMaxProblems) check.problems.push_back(text);
}

} // namespace

RelocCheck check_relocation(const ModuleSet &modules, const Module &module, const DsdRelocation &reloc,
                            std::string &detail)
{
    if (!module.contains(reloc.from, 4)) {
        detail = "is outside the module's bytes";
        return RelocCheck::Bad;
    }
    const uint32_t expected = static_cast<uint32_t>(reloc.to + static_cast<uint64_t>(reloc.addend));
    uint32_t found = 0;
    bool code = true;
    switch (reloc.kind) {
    case DsdRelocKind::LinkTimeConst: return RelocCheck::Unverifiable;
    case DsdRelocKind::OverlayId:
        if (module.read32(reloc.from) != reloc.to) {
            detail = "the word is " + hex(module.read32(reloc.from)) + ", not the overlay ID " +
                     std::to_string(reloc.to);
            return RelocCheck::Bad;
        }
        if (!target_module(modules, DsdRelocTarget::Overlay, reloc.to)) {
            detail = "names overlay " + std::to_string(reloc.to) + ", which the ROM does not have";
            return RelocCheck::Bad;
        }
        return RelocCheck::Verified;
    case DsdRelocKind::Load:
        found = module.read32(reloc.from);
        code = false;
        break;
    case DsdRelocKind::ArmCall:
    case DsdRelocKind::ArmBranch:
    case DsdRelocKind::ArmCallThumb: {
        const uint32_t insn = module.read32(reloc.from);
        const uint32_t offset = static_cast<uint32_t>(sign_extend(insn & 0x00FFFFFFu, 24)) << 2;
        bool form;
        if (reloc.kind == DsdRelocKind::ArmCallThumb) {
            // BLX: condition field 1111, bit 24 is bit 1 of the offset.
            form = (insn & 0xFE000000u) == 0xFA000000u;
            found = reloc.from + 8 + offset + ((insn >> 23) & 2u);
        } else {
            const bool link = (insn & 0x01000000u) != 0;
            form = (insn >> 28) != 0xFu && (insn & 0x0E000000u) == 0x0A000000u &&
                   link == (reloc.kind == DsdRelocKind::ArmCall);
            found = reloc.from + 8 + offset;
        }
        if (!form) {
            detail = "the instruction " + hex(insn) + " is not the branch " + dsd_reloc_kind_name(reloc.kind) +
                     " describes";
            return RelocCheck::Bad;
        }
        break;
    }
    case DsdRelocKind::ThumbCall:
    case DsdRelocKind::ThumbCallArm: {
        // The two halves of a Thumb BL: 11110 and the high offset, then
        // 11111 (BL) or 11101 (BLX, which lands on a word boundary).
        const uint32_t high = module.read16(reloc.from);
        const uint32_t low = module.read16(reloc.from + 2);
        const uint32_t suffix = reloc.kind == DsdRelocKind::ThumbCall ? 0xF800u : 0xE800u;
        if ((high & 0xF800u) != 0xF000u || (low & 0xF800u) != suffix) {
            detail = "the instructions " + hex(high, 4) + " " + hex(low, 4) + " are not the branch " +
                     dsd_reloc_kind_name(reloc.kind) + " describes";
            return RelocCheck::Bad;
        }
        found = reloc.from + 4 + (static_cast<uint32_t>(sign_extend(high & 0x7FFu, 11)) << 12) + ((low & 0x7FFu) << 1);
        if (reloc.kind == DsdRelocKind::ThumbCallArm) found &= ~3u;
        break;
    }
    }
    if (found != expected) {
        detail = "leads to " + hex(found) + ", not " + hex(expected);
        return RelocCheck::Bad;
    }
    return target_in_module(modules, reloc, code, detail) ? RelocCheck::Verified : RelocCheck::Bad;
}

size_t DsdCheck::hashes_matched() const
{
    size_t count = 0;
    for (const DsdModuleCheck &check : modules) {
        if (check.hash_ok) count++;
    }
    return count;
}

bool DsdCheck::ok() const
{
    if (modules.empty() || !not_in_config.empty()) return false;
    for (const DsdModuleCheck &check : modules) {
        if (!check.ok()) return false;
    }
    return true;
}

DsdCheck check_dsd_config(const ModuleSet &modules, const DsdConfig &config)
{
    DsdCheck result;
    std::vector<bool> described(modules.modules.size(), false);
    for (const DsdModule &dsd : config.modules) {
        DsdModuleCheck check;
        check.config = &dsd;
        switch (dsd.kind) {
        case DsdModuleKind::Main: check.module = modules.find(ModuleKind::Arm9Main); break;
        case DsdModuleKind::Itcm: check.module = modules.find(ModuleKind::Itcm); break;
        case DsdModuleKind::Dtcm: check.module = modules.find(ModuleKind::Dtcm); break;
        case DsdModuleKind::Autoload:
            check.module = dsd.autoload_index >= 0 ? modules.find(ModuleKind::Arm9Autoload, dsd.autoload_index) : nullptr;
            break;
        case DsdModuleKind::Overlay:
            check.module = dsd.overlay_id >= 0 ? modules.find(ModuleKind::Arm9Overlay, dsd.overlay_id) : nullptr;
            break;
        }

        for (const DsdSymbol &symbol : dsd.symbols) {
            switch (symbol.kind) {
            case DsdSymbolKind::Function: (symbol.thumb ? check.thumb_functions : check.arm_functions)++; break;
            case DsdSymbolKind::Label: check.labels++; break;
            case DsdSymbolKind::Data: check.data_symbols++; break;
            case DsdSymbolKind::Bss: check.bss_symbols++; break;
            }
        }
        for (size_t i = 0; i + 1 < dsd.functions.size(); i++) {
            const DsdFunction &function = dsd.functions[i];
            if (static_cast<uint64_t>(function.address) + function.size > dsd.functions[i + 1].address) {
                check.overlapping_functions++;
            }
        }
        for (const DsdRelocation &reloc : dsd.relocations) {
            switch (reloc.kind) {
            case DsdRelocKind::Load: check.loads++; break;
            case DsdRelocKind::OverlayId:
            case DsdRelocKind::LinkTimeConst: check.other_relocations++; break;
            default: check.calls++; break;
            }
        }

        if (!check.module) {
            add_problem(check, "the ROM has no module for " + dsd.name);
            result.modules.push_back(std::move(check));
            continue;
        }
        const Module &module = *check.module;
        described[static_cast<size_t>(check.module - modules.modules.data())] = true;
        check.hash_ok = module.dsd_hash == dsd.hash;
        if (!check.hash_ok) {
            add_problem(check, "hash " + hex(module.dsd_hash, 16) + ", the config has " + hex(dsd.hash, 16));
        }

        for (const DsdSymbol &symbol : dsd.symbols) {
            bool ok = true;
            switch (symbol.kind) {
            case DsdSymbolKind::Function:
                ok = module.contains(symbol.address, symbol.unknown || symbol.size == 0 ? 1 : symbol.size) &&
                     symbol.address % (symbol.thumb ? 2u : 4u) == 0;
                break;
            case DsdSymbolKind::Label: ok = module.contains(symbol.address); break;
            case DsdSymbolKind::Data: ok = symbol.address >= module.load_address && symbol.address <= module.end(); break;
            case DsdSymbolKind::Bss: ok = symbol.address >= module.end() && symbol.address <= module.bss_end(); break;
            }
            if (!ok) {
                check.symbols_bad++;
                add_problem(check, "symbol " + symbol.name + " at " + hex(symbol.address) +
                                       " does not fit the module (" + hex(module.load_address) + ", " +
                                       hex(module.size(), 1) + " bytes, " + hex(module.bss_size, 1) + " BSS)");
            }
        }
        for (const DsdRelocation &reloc : dsd.relocations) {
            std::string detail;
            switch (check_relocation(modules, module, reloc, detail)) {
            case RelocCheck::Verified: check.relocations_verified++; break;
            case RelocCheck::Unverifiable: check.relocations_unverifiable++; break;
            case RelocCheck::Bad:
                check.relocations_bad++;
                add_problem(check, std::string(dsd_reloc_kind_name(reloc.kind)) + " at " + hex(reloc.from) + " " + detail);
                break;
            }
        }
        result.modules.push_back(std::move(check));
    }
    for (size_t i = 0; i < modules.modules.size(); i++) {
        if (!described[i] && !module_is_arm7(modules.modules[i].kind)) result.not_in_config.push_back(&modules.modules[i]);
    }
    return result;
}

} // namespace ndsrecomp
