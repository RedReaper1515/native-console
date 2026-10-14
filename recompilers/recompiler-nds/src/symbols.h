// Reader for a ds-decomp ("dsd", AetiasHax/ds-decomp) configuration: its
// config.yaml and, for each module, symbols.txt, relocs.txt and the section
// list at the top of delinks.txt.
//
// A decompilation project built on dsd describes the ARM9 side of a game with
// these files: every function with its size and instruction set, every data
// and BSS symbol, and every place in the code that holds an address (a call,
// or a word that is a pointer), with the module the address belongs to. The
// last part is what overlays make necessary: several overlays are linked at
// the same addresses, so an address alone does not name a function.
//
// The grammar is the one in ds-decomp's docs (symbols.md, relocs.md,
// delinks.md) and parsers (lib/src/config), read at commit c4080635 (MIT,
// Copyright (c) 2025 Aetias). This is a reader written for it, not a port; it
// is as strict as dsd, so an attribute it does not know is an error and not
// silently dropped.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ndsrecomp {

enum class DsdModuleKind { Main, Itcm, Dtcm, Autoload, Overlay };

enum class DsdSymbolKind { Function, Label, Data, Bss };
enum class DsdDataType { Any, Byte, Short, Word };

struct DsdSymbol {
    std::string name;
    DsdSymbolKind kind = DsdSymbolKind::Data;
    uint32_t address = 0;
    // Function: the size the file states, literal pool included.
    // Data and BSS: the size the file states, or, where it leaves the size
    // open (data(any), word[], bss), the distance to the next symbol or to
    // the end of the section, whichever is nearer; DsdModule::finish()
    // fills that in, and it stays 0 if neither is known.
    // Label: 0.
    uint32_t size = 0;
    bool size_stated = false;
    bool thumb = false;   // function, label
    bool unknown = false; // function(...,unknown): called, but not found by dsd's analysis
    DsdDataType data_type = DsdDataType::Any;
    bool local = false;
    bool weak = false;
    // The symbol exists to give an ambiguous relocation (one that can lead
    // to several overlays) something to point at.
    bool ambiguous = false;
};

struct DsdFunction {
    uint32_t address = 0; // bit 0 clear for Thumb functions too
    uint32_t size = 0;
    bool thumb = false;
    std::string name;
};

struct DsdDataRange {
    uint32_t address = 0;
    uint32_t size = 0; // 0 when unknown
    bool bss = false;
    DsdDataType type = DsdDataType::Any;
    std::string name;
};

enum class DsdRelocKind {
    ArmCall,       // BL from ARM to ARM
    ThumbCall,     // BL from Thumb to Thumb
    ArmCallThumb,  // BLX from ARM to Thumb
    ThumbCallArm,  // BLX from Thumb to ARM
    ArmBranch,     // B from ARM to ARM
    Load,          // the 32-bit word at `from` is the address `to` + `addend`
    OverlayId,     // the word at `from` is the overlay ID `to`
    LinkTimeConst, // the word at `from` is a value the linker supplies
};
const char *dsd_reloc_kind_name(DsdRelocKind kind);

// Which module `to` is in.
enum class DsdRelocTarget { None, Main, Itcm, Dtcm, Autoload, Overlay, Overlays };

enum class DsdLinkTimeConst {
    None,
    DtcmLo,              // __DTCM_LO
    ItcmHi,              // __ITCM_HI
    CodeHi,              // __CODE_HI
    OverlayCount,        // __OVERLAY_COUNT
    Arm9CtorStart,       // ARM9_CTOR_START
    ExceptionTableStart, // __exception_table_start__
    ExceptionTableEnd,   // __exception_table_end__
};

struct DsdRelocation {
    uint32_t from = 0;
    uint32_t to = 0;    // an address; the overlay ID for OverlayId; 0 for LinkTimeConst
    int64_t addend = 0; // "add:": offset from `to`, on top of what the instruction form implies
    DsdRelocKind kind = DsdRelocKind::Load;
    DsdRelocTarget target = DsdRelocTarget::None;
    // Overlay: the one ID. Overlays: two or more, any of which may be the
    // one loaded when the code at `from` runs.
    std::vector<uint16_t> overlays;
    uint32_t autoload_index = 0; // Autoload
    DsdLinkTimeConst constant = DsdLinkTimeConst::None;
};

enum class DsdSectionKind { Code, Data, Rodata, Bss };

struct DsdSection {
    std::string name; // ".text", ".init", ".rodata", ".ctor", ".data", ".bss", ...
    DsdSectionKind kind = DsdSectionKind::Code;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t alignment = 0;
};

struct DsdModule {
    DsdModuleKind kind = DsdModuleKind::Main;
    std::string name;        // "main", "itcm", "dtcm", "ov000", ...
    int overlay_id = -1;     // Overlay
    int autoload_index = -1; // Autoload (an autoload block that is neither TCM)
    bool overlay_signed = false;
    uint64_t hash = 0; // FxHash64 of the module's file, see modules.h
    // Paths as config.yaml gives them, relative to its directory.
    std::string object_path;
    std::string delinks_path;
    std::string symbols_path;
    std::string relocations_path;

    std::vector<DsdSection> sections;       // by start address
    std::vector<DsdSymbol> symbols;         // by address
    std::vector<DsdRelocation> relocations; // by `from`, one per address
    std::vector<DsdFunction> functions;     // by address
    std::vector<DsdDataRange> data;         // by address: data and BSS symbols

    // Sorts the lists, drops relocations that are repeated word for word,
    // fills in the sizes the files leave open and builds `functions` and
    // `data`. Fails on two different relocations at one address.
    bool finish(std::string &error);

    const DsdFunction *function_at(uint32_t address) const;
    const DsdRelocation *relocation_at(uint32_t from) const;
    const DsdSection *section_at(uint32_t address) const;
};

struct DsdConfig {
    std::string directory; // of config.yaml
    std::string rom_config;
    std::string build_path;
    std::string delinks_dir;
    bool enable_dsprot = false;
    std::vector<DsdModule> modules; // main, the autoloads, the overlays, in the file's order

    // Reads config.yaml (`path` is the file or the directory holding it) and
    // every file it names.
    bool load(const std::string &path, std::string &error);

    // The first module of a kind, or the overlay or autoload with that index.
    const DsdModule *find(DsdModuleKind kind, int index = -1) const;
};

// The parsers, one text at a time; `where` names the source in messages.
// parse_dsd_config fills in the modules' names, hashes and paths only.
bool parse_dsd_config(const std::string &text, const std::string &where, DsdConfig &out, std::string &error);
bool parse_dsd_symbols(const std::string &text, const std::string &where, std::vector<DsdSymbol> &out,
                       std::string &error);
bool parse_dsd_relocations(const std::string &text, const std::string &where, std::vector<DsdRelocation> &out,
                           std::string &error);
// The section list that opens a delinks.txt; the per-file entries after it
// are skipped.
bool parse_dsd_sections(const std::string &text, const std::string &where, std::vector<DsdSection> &out,
                        std::string &error);

} // namespace ndsrecomp
