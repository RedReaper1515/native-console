// Checks a ds-decomp configuration against the modules of a ROM.
//
// Three things are compared, and they are independent of each other:
//
//   1. Each module's hash in config.yaml against the hash of the module as
//      this recompiler cut and decompressed it. A match means the bytes are
//      the ones the decompilation was written against.
//   2. Each relocation against the bytes it describes: a "load" must find a
//      word equal to the target address, a call a BL or BLX whose
//      destination is the target, and the target must lie in the module the
//      relocation names. A match means the relocation grammar is read the
//      way dsd writes it.
//   3. Each symbol against the module's extent: functions inside the
//      initialised bytes and aligned for their instruction set, data inside
//      them, BSS symbols inside the BSS.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "modules.h"
#include "symbols.h"

namespace ndsrecomp {

enum class RelocCheck {
    Verified,     // the bytes say what the relocation says
    Unverifiable, // nothing in the bytes to compare with (a link-time constant)
    Bad,
};

// Does `module` hold, at reloc.from, what the relocation describes, and is
// the target in the module the relocation names? `detail` explains a Bad.
RelocCheck check_relocation(const ModuleSet &modules, const Module &module, const DsdRelocation &reloc,
                            std::string &detail);

struct DsdModuleCheck {
    const DsdModule *config = nullptr;
    const Module *module = nullptr; // nullptr: the ROM has no such module
    bool hash_ok = false;
    size_t arm_functions = 0;
    size_t thumb_functions = 0;
    size_t labels = 0;
    size_t data_symbols = 0;
    size_t bss_symbols = 0;
    size_t calls = 0; // relocations, by what they are
    size_t loads = 0;
    size_t other_relocations = 0;
    size_t relocations_verified = 0;
    size_t relocations_unverifiable = 0;
    size_t relocations_bad = 0;
    size_t symbols_bad = 0;
    // Functions whose stated extent runs into the next function. Not an
    // error: a function may end in another's entry point.
    size_t overlapping_functions = 0;
    std::vector<std::string> problems; // the first few, for display

    bool ok() const { return module && hash_ok && relocations_bad == 0 && symbols_bad == 0; }
};

struct DsdCheck {
    std::vector<DsdModuleCheck> modules;       // one per module of the config, in its order
    std::vector<const Module *> not_in_config; // ARM9 modules of the ROM that the config does not describe

    size_t hashes_matched() const;
    bool ok() const;
};

// `modules` and `config` must outlive the result, which points into both.
DsdCheck check_dsd_config(const ModuleSet &modules, const DsdConfig &config);

} // namespace ndsrecomp
