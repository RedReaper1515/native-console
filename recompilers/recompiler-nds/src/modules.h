// The code modules of a DS ROM.
//
// Nothing on the DS executes from the cartridge. The boot programs are copied
// to RAM, the SDK's start-up code decompresses the ARM9 one in place and
// copies "autoload" blocks out of each program to where they run (the ARM9's
// ITCM and DTCM, the ARM7's work RAM), and overlays are read over each other
// on demand. A module is one such run of bytes at the address it runs at:
//
//   ARM9  main        the program up to its BSS, decompressed
//         itcm, dtcm  autoload blocks, at 0x01FF8000 and 0x027E0000
//         ovNNN       overlays, decompressed
//   ARM7  arm7        the program up to its BSS
//         arm7_autoload_N  its autoload blocks: on SDK games nearly all of
//                     the ARM7's code, which runs from 0x037F8000 and
//                     0x027E0000 and never from where the ROM loads it
//         arm7_ovNNN  overlays (rare)
//
// The ARM9 boundaries are the ones AetiasHax/ds-rom gives arm9.bin, itcm.bin,
// dtcm.bin and the overlay files, so ds-decomp symbols, relocations and
// module hashes apply as they are. ds-rom keeps the ARM7 program whole; it is
// split here because a recompiler needs the code at the address it runs at.
//
// Where the start-up parameters are and how a program is laid out comes from
// the SDK's own start-up code (crt0) and from ds-rom (MIT, Copyright (c) 2025
// Aetias): raw/build_info.rs, raw/autoload_info.rs, raw/rom.rs, arm9.rs, at
// commit 3bfef54. The layout and three constants are theirs (the addresses
// that make an autoload block a TCM, and in rom.cpp the count of zero bytes
// that tells a secure area that was never encrypted); the code is not.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rom.h"

namespace ndsrecomp {

// FxHash, 64-bit: the hash rustc and Firefox use, as the `fxhash` crate's
// hash64() computes it for a byte slice (the length is hashed first, then the
// bytes as little-endian 8-, 4-, 2- and 1-byte words). ds-decomp's
// config.yaml records it for every module file. The algorithm is the crate's
// (fxhash 0.2.1, Apache-2.0 or MIT); these few lines are our own.
uint64_t fxhash64(const uint8_t *data, size_t size);

enum class ModuleKind { Arm9Main, Itcm, Dtcm, Arm9Autoload, Arm9Overlay, Arm7Main, Arm7Autoload, Arm7Overlay };

const char *module_kind_name(ModuleKind kind); // "arm9_main", "itcm", ...
inline bool module_is_arm7(ModuleKind kind)
{
    return kind == ModuleKind::Arm7Main || kind == ModuleKind::Arm7Autoload || kind == ModuleKind::Arm7Overlay;
}
inline bool module_is_overlay(ModuleKind kind)
{
    return kind == ModuleKind::Arm9Overlay || kind == ModuleKind::Arm7Overlay;
}

// The SDK's start-up parameters ("module params"; ds-rom calls them the build
// info): six words in the start-up code, which the ARM9 follows with the SDK
// version and the words 0xDEC00621 and 0x2106C0DE.
struct ModuleParams {
    bool present = false;
    uint32_t offset = 0;              // of the structure, from the start of the program
    uint32_t autoload_list_start = 0; // +0x00: 12-byte entries {address, size, BSS size}
    uint32_t autoload_list_end = 0;   // +0x04
    uint32_t autoload_start = 0;      // +0x08: where the blocks' bytes are stored, one after another
    uint32_t bss_start = 0;           // +0x0C
    uint32_t bss_end = 0;             // +0x10
    uint32_t compressed_end = 0;      // +0x14: end of the BLZ data, 0 if the program is not compressed
    uint32_t sdk_version = 0;         // +0x18, when the nitro code follows
    bool has_nitrocode = false;
};

struct Module {
    ModuleKind kind = ModuleKind::Arm9Main;
    std::string name;        // "main", "itcm", "dtcm", "ov000", "arm7", ...: ds-decomp's names for the ARM9
    int overlay_id = -1;     // overlays
    int autoload_index = -1; // autoload blocks: position in the program's autoload list
    uint32_t load_address = 0;
    // The initialised bytes, as they are in guest RAM once the module is
    // loaded: after the start-up code has decompressed the program and
    // copied the autoload blocks, or after the SDK has read and decompressed
    // an overlay.
    std::vector<uint8_t> bytes;
    uint32_t bss_size = 0;          // zero-filled bytes that follow
    uint32_t static_init_start = 0; // overlays: table of static initialiser pointers, run on load
    uint32_t static_init_end = 0;
    uint32_t file_id = 0;    // overlays: FAT entry
    bool compressed = false; // stored BLZ-compressed in the ROM (an autoload block: inside its compressed program)
    // FxHash64 of `bytes`.
    uint64_t hash = 0;
    // FxHash64 of the file ds-rom extracts for this module, which is what a
    // ds-decomp config.yaml records. The same as `hash` for every module but
    // a compressed ARM9 main: ds-rom clears the module params' compressed_end
    // word there, and in RAM that word keeps its value.
    uint64_t dsd_hash = 0;

    uint32_t size() const { return static_cast<uint32_t>(bytes.size()); }
    // One past the initialised bytes, and one past the BSS. build_modules()
    // refuses modules that do not fit below 4 GiB, so neither wraps.
    uint64_t end() const { return static_cast<uint64_t>(load_address) + bytes.size(); }
    uint64_t bss_end() const { return end() + bss_size; }
    bool contains(uint32_t address, uint32_t length = 1) const
    {
        return address >= load_address && static_cast<uint64_t>(address) + length <= end();
    }
    // `length` initialised bytes at a guest address, or nullptr.
    const uint8_t *ptr(uint32_t address, uint32_t length) const
    {
        return contains(address, length) ? bytes.data() + (address - load_address) : nullptr;
    }
    // The caller has checked contains().
    uint32_t read32(uint32_t address) const { return get32(bytes.data() + (address - load_address)); }
    uint16_t read16(uint32_t address) const { return get16(bytes.data() + (address - load_address)); }
};

// What was learnt about one CPU's boot program.
struct Program {
    ModuleParams params;
    bool compressed = false;
    uint32_t stored_prefix = 0; // compressed: bytes at the start that are stored as they are
    uint32_t image_size = 0;    // bytes of the program once decompressed
    uint32_t autoload_count = 0;
};

struct ModuleSet {
    std::vector<Module> modules; // ARM9 main, its autoloads, its overlays; then the same for the ARM7
    Program arm9;
    Program arm7;
    std::vector<std::string> notes; // things a reader should know that are not errors

    const Module *find(const std::string &name) const;
    // The overlay with that ID or the autoload block with that index; with a
    // negative index (the default), the first module of the kind.
    const Module *find(ModuleKind kind, int index = -1) const;
    uint64_t total_bytes() const;
};

// Produces every module of the ROM. Fails if the secure area is encrypted
// (decrypting it takes Nintendo's key table, which is never shipped), if a
// BLZ stream is damaged, or if a table points outside the image.
bool build_modules(const Rom &rom, ModuleSet &out, std::string &error);

} // namespace ndsrecomp
