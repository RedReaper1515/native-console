#include "modules.h"

#include <cstdio>

#include "blz.h"

namespace ndsrecomp {

namespace {

constexpr uint64_t kFxSeed = 0x517CC1B727220A95ull;

inline uint64_t fx_mix(uint64_t hash, uint64_t word)
{
    return (((hash << 5) | (hash >> 59)) ^ word) * kFxSeed;
}

// The two words that end the ARM9's module params: 2-10-6 ("ni-to-ro") C0DE,
// stored big-endian and then little-endian.
constexpr uint32_t kNitrocode = 0xDEC00621u;
constexpr uint32_t kNitrocodeReversed = 0x2106C0DEu;
constexpr uint32_t kParamsSize = 0x18;        // the six words every program has
constexpr uint32_t kParamsNitrocodeAt = 0x1C; // after the SDK version at 0x18
constexpr uint32_t kAutoloadEntrySize = 12;

// No program or overlay can be larger than the console's memory: 4 MiB of
// main RAM on the DS, 16 MiB on the DSi. The limits keep a hostile BLZ
// footer or overlay table from asking for gigabytes.
constexpr uint64_t kMaxImageSize = 0x1000000;   // one decompressed program or overlay
constexpr uint64_t kMaxTotalBytes = 0x20000000; // all modules of a ROM together

const uint8_t kDestroyedSecureAreaId[8] = {0xFF, 0xDE, 0xFF, 0xE7, 0xFF, 0xDE, 0xFF, 0xE7};

void read_params(const uint8_t *program, size_t size, uint32_t offset, ModuleParams &out)
{
    const uint8_t *p = program + offset;
    out = ModuleParams();
    out.offset = offset;
    out.autoload_list_start = get32(p + 0x00);
    out.autoload_list_end = get32(p + 0x04);
    out.autoload_start = get32(p + 0x08);
    out.bss_start = get32(p + 0x0C);
    out.bss_end = get32(p + 0x10);
    out.compressed_end = get32(p + 0x14);
    out.has_nitrocode = size - offset >= kParamsNitrocodeAt + 8 && get32(p + kParamsNitrocodeAt) == kNitrocode &&
                        get32(p + kParamsNitrocodeAt + 4) == kNitrocodeReversed;
    if (out.has_nitrocode) out.sdk_version = get32(p + 0x18);
}

// Do the addresses in the params describe a program of `image_size` bytes
// loaded at `load`? `detail` says what does not fit.
bool layout_fits(const ModuleParams &params, uint32_t load, size_t image_size, std::string &detail)
{
    auto inside = [&](uint32_t address) { return address >= load && address - load <= image_size; };
    if (!inside(params.autoload_list_start) || !inside(params.autoload_list_end) ||
        params.autoload_list_end < params.autoload_list_start ||
        (params.autoload_list_end - params.autoload_list_start) % kAutoloadEntrySize != 0) {
        detail = "the autoload list " + hex(params.autoload_list_start) + "-" + hex(params.autoload_list_end) +
                 " is not a list of 12-byte entries inside the program";
        return false;
    }
    if (!inside(params.autoload_start)) {
        detail = "the autoload blocks start at " + hex(params.autoload_start) + ", outside the program";
        return false;
    }
    if (!inside(params.bss_start) || params.bss_end < params.bss_start) {
        detail = "the BSS " + hex(params.bss_start) + "-" + hex(params.bss_end) +
                 " does not start inside the program";
        return false;
    }
    return true;
}

// Finds the module params of a boot program that has not been decompressed
// yet (the start-up code reads them in that state too, so they are never in
// the compressed part).
//
// The ARM9's end in the nitro code, which identifies them for certain. The
// ARM7's have no marker in SDK versions of this ROM's time: they are accepted
// only if every address in them fits the program.
ModuleParams locate_params(const Rom &rom, bool arm7, const ProgramHeader &program, const uint8_t *raw)
{
    const size_t size = program.size;
    const uint32_t load = program.load_address;
    std::vector<uint64_t> candidates; // offsets from the start of the program, most trusted first

    // 1. The header's own pointer: a ROM offset, or on DSi-era headers an
    //    offset from the start of the program.
    const uint32_t stated = arm7 ? rom.header.arm7_params_offset : rom.header.arm9_params_offset;
    if (stated != 0) {
        if (stated >= program.rom_offset) candidates.push_back(stated - program.rom_offset);
        candidates.push_back(stated);
    }
    // 2. The twelve bytes that follow the ARM9 program in the ROM: the nitro
    //    code, the offset of the params, the offset of the overlay signatures.
    if (!arm7) {
        const uint8_t *footer = rom.ptr(static_cast<uint64_t>(program.rom_offset) + program.size, 12);
        if (footer && get32(footer) == kNitrocode) candidates.push_back(get32(footer + 4));
    }
    // 3. The start-up code's autoload routine ends with one literal, the
    //    address of the params, and then the return the header's "autoload
    //    hook" points at.
    const uint32_t hook = arm7 ? rom.header.arm7_autoload_hook : rom.header.arm9_autoload_hook;
    if (hook >= load && hook - load >= 4 && hook - load <= size) {
        const uint32_t literal = get32(raw + (hook - load - 4));
        if (literal >= load) candidates.push_back(literal - load);
    }

    for (const uint64_t offset : candidates) {
        if (offset > size || size - offset < kParamsSize) continue;
        ModuleParams params;
        read_params(raw, size, static_cast<uint32_t>(offset), params);
        bool accept = params.has_nitrocode;
        if (!accept) {
            std::string detail;
            if (params.compressed_end == 0) {
                accept = layout_fits(params, load, size, detail);
            } else if (!arm7) {
                // A compressed ARM9 without the marker: the list cannot be
                // checked until the program is decompressed. (The ARM7's
                // start-up code has no decompressor: a nonzero word there
                // means these are not its params.)
                accept = params.compressed_end > load && params.compressed_end - load <= size &&
                         params.bss_start >= load && params.bss_end >= params.bss_start &&
                         params.autoload_list_end >= params.autoload_list_start;
            }
        }
        if (accept) {
            params.present = true;
            return params;
        }
    }
    // 4. The marker itself, anywhere in the program.
    for (size_t at = kParamsNitrocodeAt; at + 8 <= size; at += 4) {
        if (get32(raw + at) != kNitrocode || get32(raw + at + 4) != kNitrocodeReversed) continue;
        ModuleParams params;
        read_params(raw, size, static_cast<uint32_t>(at - kParamsNitrocodeAt), params);
        params.present = true;
        return params;
    }
    return ModuleParams();
}

// Checks a finished module against the address space and the size budget,
// hashes it and adds it to the set.
bool add_module(ModuleSet &set, Module &&module, const std::string &label, uint64_t &total, std::string &error)
{
    if (module.bss_end() > 0x100000000ull) {
        error = label + ": " + hex(module.bytes.size()) + " bytes and " + hex(module.bss_size) + " of BSS at " +
                hex(module.load_address) + " run past the end of the address space";
        return false;
    }
    total += module.bytes.size();
    if (total > kMaxTotalBytes) {
        error = label + ": the modules of this ROM add up to more than " + hex(kMaxTotalBytes) + " bytes";
        return false;
    }
    module.hash = fxhash64(module.bytes.data(), module.bytes.size());
    module.dsd_hash = module.hash;
    set.modules.push_back(std::move(module));
    return true;
}

// Decompresses `size` bytes of BLZ data, refusing an output larger than the
// console's memory before anything is allocated.
bool decompress(const uint8_t *data, size_t size, std::vector<uint8_t> &out, uint32_t &stored_prefix,
                std::string &error)
{
    BlzFooter footer;
    if (!blz_read_footer(data, size, footer, error)) return false;
    if (static_cast<uint64_t>(size) + footer.extra_size > kMaxImageSize) {
        error = "BLZ: decompresses to " + hex(static_cast<uint64_t>(size) + footer.extra_size) +
                " bytes, more than the console has memory";
        return false;
    }
    stored_prefix = static_cast<uint32_t>(size - footer.compressed_span);
    return blz_decompress(data, size, out, error);
}

// Cuts the autoload blocks out of a decompressed program.
bool split_autoloads(const std::vector<uint8_t> &image, uint32_t load, const ModuleParams &params, bool arm7,
                     std::vector<Module> &blocks, std::string &detail)
{
    blocks.clear();
    if (!layout_fits(params, load, image.size(), detail)) return false;
    const uint32_t count = (params.autoload_list_end - params.autoload_list_start) / kAutoloadEntrySize;
    const uint8_t *list = image.data() + (params.autoload_list_start - load);
    uint64_t at = params.autoload_start - load;
    bool have_itcm = false;
    bool have_dtcm = false;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t address = get32(list + i * kAutoloadEntrySize);
        const uint32_t size = get32(list + i * kAutoloadEntrySize + 4);
        const uint32_t bss_size = get32(list + i * kAutoloadEntrySize + 8);
        if (size > image.size() - at) {
            detail = "autoload block " + std::to_string(i) + " (" + hex(size) + " bytes for " + hex(address) +
                     ") runs past the end of the program";
            return false;
        }
        Module block;
        block.autoload_index = static_cast<int>(i);
        block.load_address = address;
        block.bss_size = bss_size;
        block.bytes.assign(image.begin() + static_cast<std::ptrdiff_t>(at),
                           image.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        // The addresses the SDK links the two TCMs at; the same table as
        // ds-rom's AutoloadInfo::new.
        if (arm7) {
            block.kind = ModuleKind::Arm7Autoload;
            block.name = "arm7_autoload_" + std::to_string(i);
        } else if (address == 0x01FF8000u) {
            block.kind = ModuleKind::Itcm;
            block.name = have_itcm ? "autoload_" + std::to_string(i) : "itcm";
            have_itcm = true;
        } else if (address == 0x027E0000u || address == 0x027C0000u || address == 0x023C0000u) {
            block.kind = ModuleKind::Dtcm;
            block.name = have_dtcm ? "autoload_" + std::to_string(i) : "dtcm";
            have_dtcm = true;
        } else {
            block.kind = ModuleKind::Arm9Autoload;
            block.name = "autoload_" + std::to_string(i);
        }
        blocks.push_back(std::move(block));
    }
    return true;
}

bool build_program(const Rom &rom, bool arm7, ModuleSet &set, uint64_t &total, std::string &error)
{
    const ProgramHeader &header = arm7 ? rom.header.arm7 : rom.header.arm9;
    Program &program = arm7 ? set.arm7 : set.arm9;
    const std::string label = arm7 ? "ARM7 program" : "ARM9 program";
    if (header.size == 0) return true;
    const uint8_t *raw = rom.ptr(header.rom_offset, header.size);
    if (!raw) {
        error = label + ": runs past the end of the image";
        return false;
    }
    if (header.size > kMaxImageSize) {
        error = label + ": " + hex(header.size) + " bytes is more than the console has memory";
        return false;
    }

    program.params = locate_params(rom, arm7, header, raw);
    ModuleParams &params = program.params;

    std::vector<uint8_t> image;
    if (params.present && params.compressed_end != 0) {
        if (params.compressed_end <= header.load_address || params.compressed_end - header.load_address > header.size) {
            error = label + ": the module params put the end of the compressed data at " + hex(params.compressed_end) +
                    ", outside the program (" + hex(header.load_address) + ", " + hex(header.size) + " bytes)";
            return false;
        }
        const uint32_t compressed_size = params.compressed_end - header.load_address;
        std::string detail;
        if (!decompress(raw, compressed_size, image, program.stored_prefix, detail)) {
            error = label + ": " + detail;
            return false;
        }
        if (static_cast<uint64_t>(params.offset) + kParamsSize > program.stored_prefix) {
            error = label + ": the module params at +" + hex(params.offset) + " lie inside the compressed data";
            return false;
        }
        // Bytes the ROM loads beyond the compressed data and that the
        // decompressed data does not reach stay as they were loaded.
        if (header.size > image.size()) image.insert(image.end(), raw + image.size(), raw + header.size);
        program.compressed = true;
    } else {
        image.assign(raw, raw + header.size);
    }
    program.image_size = static_cast<uint32_t>(image.size());

    if (!arm7 && rom.secure_area == SecureArea::DecryptedId && image.size() >= sizeof kDestroyedSecureAreaId) {
        for (size_t i = 0; i < sizeof kDestroyedSecureAreaId; i++) image[i] = kDestroyedSecureAreaId[i];
        set.notes.push_back("the secure area still carries its ID \"encryObj\"; main holds 0xE7FFDEFF twice "
                            "in its place, which is what the BIOS leaves in RAM");
    }

    Module main_module;
    main_module.kind = arm7 ? ModuleKind::Arm7Main : ModuleKind::Arm9Main;
    main_module.name = arm7 ? "arm7" : "main";
    main_module.load_address = header.load_address;
    main_module.compressed = program.compressed;
    size_t main_size = image.size();
    std::vector<Module> blocks;
    if (params.present) {
        std::string detail;
        if (split_autoloads(image, header.load_address, params, arm7, blocks, detail)) {
            main_size = params.bss_start - header.load_address;
            main_module.bss_size = params.bss_end - params.bss_start;
        } else if (params.has_nitrocode || program.compressed) {
            error = label + ": module params at +" + hex(params.offset) + ": " + detail;
            return false;
        } else {
            set.notes.push_back(label + ": the words at +" + hex(params.offset) +
                                " are not module params (" + detail + "); the program is taken whole");
            params = ModuleParams();
            blocks.clear();
        }
    }
    main_module.bytes.assign(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(main_size));
    // ds-rom marks a decompressed arm9.bin by clearing compressed_end, so the
    // file a ds-decomp hash describes differs from RAM in that one word.
    const bool object_differs = !arm7 && program.compressed &&
                                static_cast<uint64_t>(params.offset) + kParamsSize <= main_module.bytes.size();
    uint64_t object_hash = 0;
    if (object_differs) {
        std::vector<uint8_t> object = main_module.bytes;
        for (size_t i = 0; i < 4; i++) object[params.offset + 0x14 + i] = 0;
        object_hash = fxhash64(object.data(), object.size());
    }
    program.autoload_count = static_cast<uint32_t>(blocks.size());

    if (!add_module(set, std::move(main_module), label, total, error)) return false;
    if (object_differs) set.modules.back().dsd_hash = object_hash;
    for (Module &block : blocks) {
        // The blocks travel inside the program, compressed along with it.
        block.compressed = program.compressed;
        const std::string block_label = label + ", autoload block " + std::to_string(block.autoload_index);
        if (!add_module(set, std::move(block), block_label, total, error)) return false;
    }
    return true;
}

bool build_overlays(const Rom &rom, bool arm7, ModuleSet &set, uint64_t &total, std::string &error)
{
    const std::vector<OverlayEntry> &table = arm7 ? rom.arm7_overlays : rom.arm9_overlays;
    for (const OverlayEntry &entry : table) {
        const std::string label = std::string(arm7 ? "ARM7" : "ARM9") + " overlay " + std::to_string(entry.id);
        if (entry.id > 0xFFFFu) {
            error = label + ": the ID is not a 16-bit number";
            return false;
        }
        char name[24];
        std::snprintf(name, sizeof name, "%sov%03u", arm7 ? "arm7_" : "", static_cast<unsigned>(entry.id));
        if (set.find(name)) {
            error = label + ": the ID appears twice in the overlay table";
            return false;
        }
        std::string detail;
        uint32_t file_size = 0;
        const uint8_t *file = rom.file_data(entry.file_id, file_size, detail);
        if (!file) {
            error = label + ": " + detail;
            return false;
        }

        Module module;
        module.kind = arm7 ? ModuleKind::Arm7Overlay : ModuleKind::Arm9Overlay;
        module.name = name;
        module.overlay_id = static_cast<int>(entry.id);
        module.load_address = entry.load_address;
        module.bss_size = entry.bss_size;
        module.static_init_start = entry.static_init_start;
        module.static_init_end = entry.static_init_end;
        module.file_id = entry.file_id;
        module.compressed = entry.compressed();
        if (entry.compressed()) {
            // The SDK reads compressed_size bytes of the file and decompresses
            // them where they are.
            if (entry.compressed_size == 0 || entry.compressed_size > file_size) {
                error = label + ": the table gives a compressed size of " + hex(entry.compressed_size) +
                        ", the file has " + hex(file_size) + " bytes";
                return false;
            }
            uint32_t stored_prefix = 0;
            if (!decompress(file, entry.compressed_size, module.bytes, stored_prefix, detail)) {
                error = label + ": " + detail;
                return false;
            }
        } else {
            if (entry.size > kMaxImageSize) {
                error = label + ": " + hex(entry.size) + " bytes is more than the console has memory";
                return false;
            }
            module.bytes.assign(file, file + (file_size < entry.size ? file_size : entry.size));
        }
        if (module.bytes.size() < entry.size) {
            error = label + ": the file holds " + hex(module.bytes.size()) + " bytes, its table entry says " +
                    hex(entry.size);
            return false;
        }
        if (module.bytes.size() > entry.size) {
            set.notes.push_back(label + ": decompresses to " + hex(module.bytes.size()) +
                                " bytes; the table entry says " + hex(entry.size) + ", and the rest is dropped");
            module.bytes.resize(entry.size);
        }
        if (!add_module(set, std::move(module), label, total, error)) return false;
    }
    return true;
}

} // namespace

uint64_t fxhash64(const uint8_t *data, size_t size)
{
    uint64_t hash = fx_mix(0, size);
    for (; size >= 8; data += 8, size -= 8) {
        hash = fx_mix(hash, get32(data) | (static_cast<uint64_t>(get32(data + 4)) << 32));
    }
    if (size >= 4) {
        hash = fx_mix(hash, get32(data));
        data += 4;
        size -= 4;
    }
    if (size >= 2) {
        hash = fx_mix(hash, get16(data));
        data += 2;
        size -= 2;
    }
    if (size >= 1) hash = fx_mix(hash, data[0]);
    return hash;
}

const char *module_kind_name(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::Arm9Main: return "arm9_main";
    case ModuleKind::Itcm: return "itcm";
    case ModuleKind::Dtcm: return "dtcm";
    case ModuleKind::Arm9Autoload: return "arm9_autoload";
    case ModuleKind::Arm9Overlay: return "arm9_overlay";
    case ModuleKind::Arm7Main: return "arm7_main";
    case ModuleKind::Arm7Autoload: return "arm7_autoload";
    case ModuleKind::Arm7Overlay: return "arm7_overlay";
    }
    return "?";
}

const Module *ModuleSet::find(const std::string &name) const
{
    for (const Module &module : modules) {
        if (module.name == name) return &module;
    }
    return nullptr;
}

const Module *ModuleSet::find(ModuleKind kind, int index) const
{
    for (const Module &module : modules) {
        if (module.kind != kind) continue;
        if (index < 0) return &module;
        if ((module_is_overlay(kind) ? module.overlay_id : module.autoload_index) == index) return &module;
    }
    return nullptr;
}

uint64_t ModuleSet::total_bytes() const
{
    uint64_t total = 0;
    for (const Module &module : modules) total += module.bytes.size();
    return total;
}

bool build_modules(const Rom &rom, ModuleSet &out, std::string &error)
{
    out = ModuleSet();
    if (rom.secure_area == SecureArea::Encrypted) {
        error = "the secure area (the first 2 KiB of the ARM9 program, at ROM offset " +
                hex(rom.header.arm9.rom_offset) +
                ") is encrypted. A dump with a decrypted secure area is needed: decrypting it takes "
                "Nintendo's key table, which Native Console never ships";
        return false;
    }
    uint64_t total = 0;
    return build_program(rom, false, out, total, error) && build_overlays(rom, false, out, total, error) &&
           build_program(rom, true, out, total, error) && build_overlays(rom, true, out, total, error);
}

} // namespace ndsrecomp
