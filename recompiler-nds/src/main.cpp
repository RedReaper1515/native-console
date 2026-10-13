// ndsrecomp: recompile a Nintendo DS ROM into a C project that links the DS
// runtime, and look into the ROM on the way.
//
//   ndsrecomp ROM -o OUTDIR [--name ID] [--runtime DIR] [--symbols DIR] [--trace FILE]...
//             [--no-pointer-scan] [--no-bios] [--inline-handlers]
//   ndsrecomp ROM --profile FILE [--profile-frames N]
//                                         play the game with the interpreter and write a trace
//   ndsrecomp ROM --info                  header summary and the table of code modules
//   ndsrecomp ROM --files                 the file system: ID, ROM offset, size, path
//   ndsrecomp ROM --extract DIR           each module, decompressed, as DIR/<file>.bin, and DIR/modules.json
//   ndsrecomp ROM --symbols DIR --check   check a ds-decomp configuration (DIR holds config.yaml) against the ROM:
//                                         module hashes, relocations, symbol extents
//   ndsrecomp --version | --version-json
//
// The ROM and the options may come in any order, and the modes can be
// combined. The exit status is 0 only if everything asked for succeeded; a
// bad command line is 2.
//
// -o writes the project (recompile.h). --name is the project and executable
// name, a C identifier; without it the ROM's file name is used, made into
// one. --runtime is the runtime-nds tree to snapshot; without it the usual
// places are searched (see find_runtime_dir). --symbols gives the analyzer a
// ds-decomp configuration for the ARM9 side. --trace FILE adds the code a
// previous run had to interpret (written by the game's --trace-out);
// repeatable. --profile FILE first plays the game for N frames (default
// 6000, over three starts) with the interpreter under the runtime's
// built-in input script and writes what ran to FILE: interrupt handlers and
// functions called only through pointers the analysis cannot follow are
// found that way. With -o the file is then used like a --trace.
// --no-pointer-scan leaves out function pointers found in data,
// --no-bios the replacement BIOS images, and --inline-handlers folds the
// instruction handlers into the generated code (faster code, slower builds).
//
// A module is a run of code at the address it executes at: the ARM9 program
// with its autoload blocks (ITCM, DTCM) split off, each overlay, and the ARM7
// program with its autoload blocks; see modules.h. The ROM's secure area must
// be decrypted, as dumps normally are.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "check.h"
#include "modules.h"
#include "nds_runner.h"
#include "recompile.h"
#include "rom.h"
#include "symbols.h"

#ifndef NDSRECOMP_DEFAULT_RUNTIME_DIR
#define NDSRECOMP_DEFAULT_RUNTIME_DIR ""
#endif
#ifndef NDSRECOMP_VERSION
#define NDSRECOMP_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using namespace ndsrecomp;

namespace {

// Machine-readable identity, recorded in release manifests.
void print_version_json()
{
    std::cout << "{\"schema\":\"ndsrecomp.version\",\"schema_version\":1,"
              << "\"version\":\"" << NDSRECOMP_VERSION << "\","
              << "\"abis\":{},"
              << "\"features\":[\"dsd-symbols\",\"module-extract\",\"profile\",\"recompile\",\"traces\"]}\n";
}

int usage()
{
    std::cerr << "usage: ndsrecomp ROM -o OUTDIR [--name ID] [--runtime DIR] [--symbols DIR] [--trace FILE]...\n"
                 "                 [--no-pointer-scan] [--no-bios] [--inline-handlers]\n"
                 "       ndsrecomp ROM --profile FILE [--profile-frames N]\n"
                 "       ndsrecomp ROM --info\n"
                 "       ndsrecomp ROM --files\n"
                 "       ndsrecomp ROM --extract DIR\n"
                 "       ndsrecomp ROM --symbols DIR --check\n"
                 "       ndsrecomp --version | --version-json\n";
    return 2;
}

// A project name from a file name: what is not a letter or a digit becomes
// an underscore, and a name cannot start with a digit.
std::string sanitize(const std::string &text)
{
    std::string out;
    for (const char ch : text) out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "nds_" + out;
    return out;
}

// Text from the ROM, made safe to print: anything outside printable ASCII
// becomes \xNN.
std::string printable(const std::string &text)
{
    std::string out;
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out += ch;
        } else {
            char escape[8];
            std::snprintf(escape, sizeof escape, "\\x%02x", static_cast<unsigned>(c));
            out += escape;
        }
    }
    return out;
}

std::string json_string(const std::string &text)
{
    std::string out = "\"";
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20 || c >= 0x7F) {
            char escape[8];
            std::snprintf(escape, sizeof escape, "\\u%04x", static_cast<unsigned>(c));
            out += escape;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

std::string size_text(uint64_t bytes)
{
    char text[32];
    const double kib = static_cast<double>(bytes) / 1024.0;
    if (bytes >= 1024 * 1024) std::snprintf(text, sizeof text, "%.2f MiB", kib / 1024.0);
    else std::snprintf(text, sizeof text, "%.1f KiB", kib);
    return text;
}

// "0-3, 7, 19-29" for a sorted list of overlay IDs.
std::string id_ranges(const std::vector<int> &ids)
{
    std::string out;
    for (size_t i = 0; i < ids.size();) {
        size_t last = i;
        while (last + 1 < ids.size() && ids[last + 1] == ids[last] + 1) last++;
        if (!out.empty()) out += ", ";
        out += std::to_string(ids[i]);
        if (last > i) out += "-" + std::to_string(ids[last]);
        i = last + 1;
    }
    return out;
}

// The file --extract writes a module to. Where ds-decomp has a name for a
// module's file, this is that name.
std::string module_file_name(const Module &module)
{
    switch (module.kind) {
    case ModuleKind::Arm9Main: return "arm9.bin";
    case ModuleKind::Arm9Overlay: return "arm9_" + module.name + ".bin";
    default: return module.name + ".bin";
    }
}

void print_program(const char *name, const ProgramHeader &header, uint32_t hook, const Program &program)
{
    std::printf("  %s: ROM %s, %s bytes, loads at %s, entry %s", name, hex(header.rom_offset).c_str(),
                hex(header.size, 1).c_str(), hex(header.load_address).c_str(), hex(header.entry).c_str());
    if (hook) std::printf(", autoload hook %s", hex(hook).c_str());
    std::printf("\n");
    const ModuleParams &params = program.params;
    if (!params.present) {
        if (header.size) std::printf("        no SDK module params: taken as one module\n");
        return;
    }
    std::printf("        module params at +%s: BSS %s-%s, %u autoload block%s at %s (list %s-%s)",
                hex(params.offset, 1).c_str(), hex(params.bss_start).c_str(), hex(params.bss_end).c_str(),
                program.autoload_count, program.autoload_count == 1 ? "" : "s", hex(params.autoload_start).c_str(),
                hex(params.autoload_list_start).c_str(), hex(params.autoload_list_end).c_str());
    if (params.has_nitrocode) std::printf(", SDK %s", hex(params.sdk_version).c_str());
    std::printf("\n");
    if (program.compressed) {
        std::printf("        BLZ-compressed to %s: %s bytes stored, %s bytes decompressed "
                    "(the first %s are stored as they are)\n",
                    hex(params.compressed_end).c_str(), hex(params.compressed_end - header.load_address, 1).c_str(),
                    hex(program.image_size, 1).c_str(), hex(program.stored_prefix, 1).c_str());
    }
}

void print_header(const std::string &path, const Rom &rom)
{
    const Header &h = rom.header;
    std::printf("%s\n", path.c_str());
    std::printf("  title \"%s\", game code %s, maker %s, version %u, %s (%zu bytes)\n", printable(h.title).c_str(),
                printable(h.game_code).c_str(), printable(h.maker_code).c_str(), static_cast<unsigned>(h.rom_version),
                size_text(rom.data.size()).c_str(), rom.data.size());
    const char *unit = h.unit_code == 0   ? "DS"
                       : h.unit_code == 2 ? "DS and DSi"
                       : h.unit_code == 3 ? "DSi only"
                                          : "unknown";
    std::printf("  unit code %u (%s), device capacity %u (%s chip), %s bytes in use, header size %s\n",
                static_cast<unsigned>(h.unit_code), unit, static_cast<unsigned>(h.device_capacity),
                h.device_capacity < 20 ? size_text(0x20000ull << h.device_capacity).c_str() : "?",
                hex(h.rom_size, 1).c_str(), hex(h.header_size, 1).c_str());
    std::printf("  header CRC %s %s, logo CRC %s %s\n", hex(h.header_crc, 4).c_str(),
                h.header_crc_ok() ? "ok" : ("MISMATCH, computed " + hex(h.header_crc_computed, 4)).c_str(),
                hex(h.logo_crc, 4).c_str(),
                h.logo_crc_ok() ? "ok" : ("MISMATCH, computed " + hex(h.logo_crc_computed, 4)).c_str());
    std::printf("  secure area: %s", secure_area_name(rom.secure_area));
    if (rom.secure_area == SecureArea::Decrypted || rom.secure_area == SecureArea::DecryptedId) {
        // The header's CRC is of the area as the card sends it, encrypted,
        // so it cannot be checked on a decrypted dump; the area's own can.
        std::printf("%s (the header's CRC of it, %s, is of the encrypted form and is not checked)",
                    rom.secure_area_self_check ? ", its own CRC ok" : ", without a valid CRC of its own",
                    hex(h.secure_area_crc, 4).c_str());
    }
    std::printf("\n");
    std::printf("  files: %zu, %zu of them named, in %zu director%s (FNT %s+%s, FAT %s+%s)\n", rom.files.size(),
                rom.named_file_count(), rom.directories.size(), rom.directories.size() == 1 ? "y" : "ies",
                hex(h.fnt_offset).c_str(), hex(h.fnt_size, 1).c_str(), hex(h.fat_offset).c_str(),
                hex(h.fat_size, 1).c_str());
    std::printf("  overlays: %zu ARM9 (table %s+%s), %zu ARM7 (table %s+%s)\n", rom.arm9_overlays.size(),
                hex(h.arm9_overlay_offset).c_str(), hex(h.arm9_overlay_size, 1).c_str(), rom.arm7_overlays.size(),
                hex(h.arm7_overlay_offset).c_str(), hex(h.arm7_overlay_size, 1).c_str());
    if (h.icon_offset) std::printf("  icon and title at %s\n", hex(h.icon_offset).c_str());
    else std::printf("  no icon and title\n");
}

// Overlays linked at one address replace each other; overlays linked at
// different addresses can still overlap. Printed per load address.
void print_overlay_map(const ModuleSet &set, ModuleKind kind)
{
    struct Slot {
        std::vector<int> ids;
        uint64_t code_end = 0;
        uint64_t bss_end = 0;
    };
    std::map<uint32_t, Slot> slots;
    for (const Module &module : set.modules) {
        if (module.kind != kind) continue;
        Slot &slot = slots[module.load_address];
        slot.ids.push_back(module.overlay_id);
        slot.code_end = std::max(slot.code_end, module.end());
        slot.bss_end = std::max(slot.bss_end, module.bss_end());
    }
    if (slots.empty()) return;
    std::printf("\n%s overlays by load address (%zu addresses):\n", kind == ModuleKind::Arm9Overlay ? "ARM9" : "ARM7",
                slots.size());
    std::printf("  %-10s  %-10s  %-10s  %-22s  %s\n", "load", "code to", "BSS to", "overlays",
                "also overlaps the overlays at");
    for (auto &[address, slot] : slots) {
        std::sort(slot.ids.begin(), slot.ids.end());
        std::string others;
        for (const auto &[other_address, other] : slots) {
            if (other_address == address || other_address >= slot.bss_end || address >= other.bss_end) continue;
            if (!others.empty()) others += " ";
            others += hex(other_address);
        }
        std::printf("  %-10s  %-10s  %-10s  %-22s  %s\n", hex(address).c_str(), hex(slot.code_end).c_str(),
                    hex(slot.bss_end).c_str(), id_ranges(slot.ids).c_str(), others.empty() ? "-" : others.c_str());
    }
}

void print_modules(const ModuleSet &set)
{
    std::printf("\nmodules: %zu, %s bytes of code and data\n", set.modules.size(), hex(set.total_bytes(), 1).c_str());
    std::printf("  %-13s  %-15s  %-10s  %-9s  %-9s  %-16s  %s\n", "kind", "name", "load", "size", "BSS", "hash", "");
    for (const Module &module : set.modules) {
        std::string remarks = module.compressed ? "compressed" : "";
        if (module.dsd_hash != module.hash) {
            remarks += std::string(remarks.empty() ? "" : ", ") + "ds-decomp hash " + hex(module.dsd_hash, 16).substr(2);
        }
        std::printf("  %-13s  %-15s  %-10s  %-9s  %-9s  %-16s  %s\n", module_kind_name(module.kind), module.name.c_str(),
                    hex(module.load_address).c_str(), hex(module.size(), 6).c_str(), hex(module.bss_size, 6).c_str(),
                    hex(module.hash, 16).substr(2).c_str(), remarks.c_str());
    }
    uint64_t overlay_bytes = 0;
    size_t overlay_count = 0;
    for (const Module &module : set.modules) {
        if (module.kind != ModuleKind::Arm9Overlay) continue;
        overlay_bytes += module.bytes.size();
        overlay_count++;
    }
    if (overlay_count) {
        std::printf("  the %zu ARM9 overlays hold %s bytes (%s)\n", overlay_count, hex(overlay_bytes, 1).c_str(),
                    size_text(overlay_bytes).c_str());
    }
    print_overlay_map(set, ModuleKind::Arm9Overlay);
    print_overlay_map(set, ModuleKind::Arm7Overlay);
    for (const std::string &note : set.notes) std::printf("note: %s\n", note.c_str());
}

void print_files(const Rom &rom)
{
    std::map<uint32_t, std::string> overlays;
    for (const OverlayEntry &overlay : rom.arm9_overlays) {
        overlays[overlay.file_id] = "(ARM9 overlay " + std::to_string(overlay.id) + ")";
    }
    for (const OverlayEntry &overlay : rom.arm7_overlays) {
        overlays[overlay.file_id] = "(ARM7 overlay " + std::to_string(overlay.id) + ")";
    }
    std::printf("%-5s  %-10s  %-10s  %s\n", "id", "offset", "size", "path");
    for (const FileEntry &file : rom.files) {
        std::string name = printable(file.path);
        if (name.empty()) {
            const auto it = overlays.find(file.id);
            name = it != overlays.end() ? it->second : "(unnamed)";
        }
        if (!file.valid) name += "  [FAT range outside the image]";
        std::printf("%-5u  %-10s  %-10s  %s\n", static_cast<unsigned>(file.id), hex(file.offset).c_str(),
                    hex(file.size, 1).c_str(), name.c_str());
    }
}

bool extract(const Rom &rom, const ModuleSet &set, const std::string &directory, std::string &error)
{
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        error = "cannot create " + directory + ": " + ec.message();
        return false;
    }
    std::string json = "{\n  \"schema\": \"ndsrecomp.modules\",\n  \"schema_version\": 1,\n";
    json += "  \"rom\": {\"title\": " + json_string(rom.header.title) + ", \"game_code\": " +
            json_string(rom.header.game_code) + ", \"maker_code\": " + json_string(rom.header.maker_code) +
            ", \"version\": " + std::to_string(rom.header.rom_version) + ", \"size\": " +
            std::to_string(rom.data.size()) + ", \"header_crc\": " + json_string(hex(rom.header.header_crc, 4)) + "},\n";
    json += "  \"hash\": \"FxHash64 of the file: the length, then the bytes as little-endian 64-bit words "
            "(the Rust fxhash crate's hash64 of a byte slice)\",\n";
    json += "  \"dsd_hash\": \"the same hash of the file ds-rom extracts, as a ds-decomp config.yaml records it; "
            "differs from hash only where ds-rom clears the module params' compressed_end word\",\n";
    json += "  \"modules\": [\n";
    for (size_t i = 0; i < set.modules.size(); i++) {
        const Module &module = set.modules[i];
        const std::string file = module_file_name(module);
        const fs::path target = fs::path(directory) / file;
        std::ofstream out(target, std::ios::binary);
        out.write(reinterpret_cast<const char *>(module.bytes.data()), static_cast<std::streamsize>(module.bytes.size()));
        out.close();
        if (!out) {
            error = "cannot write " + target.string();
            return false;
        }
        json += "    {\"kind\": " + json_string(module_kind_name(module.kind)) + ", \"name\": " + json_string(module.name) +
                ", \"cpu\": " + (module_is_arm7(module.kind) ? "7" : "9") + ", \"file\": " + json_string(file) +
                ", \"load_address\": " + json_string(hex(module.load_address)) + ", \"size\": " +
                std::to_string(module.bytes.size()) + ", \"bss_size\": " + std::to_string(module.bss_size) +
                ", \"compressed\": " + (module.compressed ? "true" : "false") + ", \"hash\": " +
                json_string(hex(module.hash, 16).substr(2)) + ", \"dsd_hash\": " +
                json_string(hex(module.dsd_hash, 16).substr(2));
        if (module.overlay_id >= 0) {
            json += ", \"overlay_id\": " + std::to_string(module.overlay_id) + ", \"file_id\": " +
                    std::to_string(module.file_id) + ", \"static_init_start\": " +
                    json_string(hex(module.static_init_start)) + ", \"static_init_end\": " +
                    json_string(hex(module.static_init_end));
        }
        if (module.autoload_index >= 0) json += ", \"autoload_index\": " + std::to_string(module.autoload_index);
        if (module.kind == ModuleKind::Arm9Main) json += ", \"entry\": " + json_string(hex(rom.header.arm9.entry));
        if (module.kind == ModuleKind::Arm7Main) json += ", \"entry\": " + json_string(hex(rom.header.arm7.entry));
        json += i + 1 < set.modules.size() ? "},\n" : "}\n";
    }
    json += "  ]\n}\n";
    const fs::path index = fs::path(directory) / "modules.json";
    std::ofstream out(index, std::ios::binary);
    out << json;
    out.close();
    if (!out) {
        error = "cannot write " + index.string();
        return false;
    }
    std::printf("wrote %zu modules (%s bytes) and modules.json to %s\n", set.modules.size(),
                hex(set.total_bytes(), 1).c_str(), directory.c_str());
    return true;
}

// Loads a ds-decomp configuration and checks it against the ROM's modules.
bool check_symbols(const ModuleSet &set, const std::string &directory)
{
    DsdConfig config;
    std::string error;
    if (!config.load(directory, error)) {
        std::cerr << error << "\n";
        return false;
    }
    const DsdCheck check = check_dsd_config(set, config);
    std::printf("\nds-decomp configuration in %s: %zu modules\n", config.directory.c_str(), config.modules.size());
    if (config.enable_dsprot) {
        std::printf("note: this configuration was made with DS Protect decryption, which ndsrecomp does not do; "
                    "the hashes of protected modules will differ\n");
    }
    std::printf("  %-8s  %-16s  %-8s  %10s  %6s  %6s  %6s  %6s  %7s  %7s  %s\n", "module", "hash", "", "functions", "ARM",
                "Thumb", "data", "BSS", "calls", "loads", "relocations found in the bytes");
    size_t arm = 0, thumb = 0, labels = 0, data = 0, bss = 0, calls = 0, loads = 0, other = 0;
    size_t verified = 0, unverifiable = 0, bad_relocations = 0, bad_symbols = 0, overlapping = 0;
    for (const DsdModuleCheck &row : check.modules) {
        const DsdModule &module = *row.config;
        const size_t relocations = row.calls + row.loads + row.other_relocations;
        std::printf("  %-8s  %016llx  %-8s  %10zu  %6zu  %6zu  %6zu  %6zu  %7zu  %7zu  %zu of %zu%s\n", module.name.c_str(),
                    static_cast<unsigned long long>(module.hash), !row.module ? "MISSING" : row.hash_ok ? "ok" : "MISMATCH",
                    row.arm_functions + row.thumb_functions, row.arm_functions, row.thumb_functions, row.data_symbols,
                    row.bss_symbols, row.calls, row.loads, row.relocations_verified, relocations,
                    row.relocations_bad || row.symbols_bad ? "  FAILED" : "");
        for (const std::string &problem : row.problems) std::printf("            %s\n", problem.c_str());
        arm += row.arm_functions;
        thumb += row.thumb_functions;
        labels += row.labels;
        data += row.data_symbols;
        bss += row.bss_symbols;
        calls += row.calls;
        loads += row.loads;
        other += row.other_relocations;
        verified += row.relocations_verified;
        unverifiable += row.relocations_unverifiable;
        bad_relocations += row.relocations_bad;
        bad_symbols += row.symbols_bad;
        overlapping += row.overlapping_functions;
    }
    for (const Module *module : check.not_in_config) {
        std::printf("  %-8s  the ROM has this module, the configuration does not\n", module->name.c_str());
    }
    std::printf("module hashes: %zu of %zu match\n", check.hashes_matched(), check.modules.size());
    std::printf("symbols: %zu functions (%zu ARM, %zu Thumb), %zu labels, %zu data, %zu BSS; %zu outside their module\n",
                arm + thumb, arm, thumb, labels, data, bss, bad_symbols);
    if (overlapping) {
        std::printf("         %zu function%s run%s into the next one\n", overlapping, overlapping == 1 ? "" : "s",
                    overlapping == 1 ? "s" : "");
    }
    std::printf("relocations: %zu (%zu calls, %zu loads, %zu other); %zu found in the module bytes as described, "
                "%zu not checkable (link-time constants), %zu wrong\n",
                calls + loads + other, calls, loads, other, verified, unverifiable, bad_relocations);
    std::printf("check: %s\n", check.ok() ? "passed" : "FAILED");
    return check.ok();
}

int run(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << NDSRECOMP_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--version-json") {
        print_version_json();
        return 0;
    }
    std::string rom_path, runtime_arg, extract_dir, symbols_dir, profile_path;
    uint64_t profile_frames = 6000;
    bool profile_frames_given = false;
    RecompileOptions project; // what -o and the options that go with it ask for
    bool info = false, files = false, check = false;
    bool project_options = false; // an option that means something only with -o
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        bool missing = false;
        auto value = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            missing = true;
            return std::string();
        };
        if (a == "-o" || a == "--output") project.out_dir = value();
        else if (a == "--extract") extract_dir = value();
        else if (a == "--symbols") symbols_dir = value();
        else if (a == "--info") info = true;
        else if (a == "--files") files = true;
        else if (a == "--check") check = true;
        else if (a == "--profile") profile_path = value();
        else if (a == "--profile-frames") {
            profile_frames = std::strtoull(value().c_str(), nullptr, 10);
            profile_frames_given = true;
        }
        else if (!a.empty() && a[0] != '-' && rom_path.empty()) rom_path = a;
        else {
            if (a == "--name") project.name = value();
            else if (a == "--runtime") runtime_arg = value();
            else if (a == "--trace") project.trace_files.push_back(value());
            else if (a == "--no-pointer-scan") project.pointer_scan = false;
            else if (a == "--no-bios") project.bios = false;
            else if (a == "--inline-handlers") project.inline_handlers = true;
            else return usage();
            project_options = true;
        }
        if (missing) return usage();
    }
    const bool want_project = !project.out_dir.empty();
    const bool want_profile = !profile_path.empty();
    if (rom_path.empty() ||
        (!info && !files && !check && extract_dir.empty() && !want_project && !want_profile)) {
        return usage();
    }
    if (profile_frames_given && (!want_profile || profile_frames == 0)) return usage();
    if (check && symbols_dir.empty()) return usage();
    if (!symbols_dir.empty() && !check && !want_project) return usage();
    if (project_options && !want_project) return usage();

    Rom rom;
    std::string error;
    if (!rom.load(rom_path, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    for (const std::string &warning : rom.warnings) std::cerr << "warning: " << warning << "\n";
    if (info) print_header(rom_path, rom);
    if (files) print_files(rom);

    if (want_profile) {
        std::error_code ec;
        const fs::path parent = fs::path(profile_path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        uint64_t instructions[2] = { 0, 0 };
        if (!nds_profile_rom(rom.data.data(), rom.data.size(), profile_frames, profile_path.c_str(), instructions)) {
            std::cerr << "profiling failed: the runtime cannot load the ROM, or cannot write " << profile_path << "\n";
            return 1;
        }
        std::printf("profiled %llu frames (arm9 %llu instructions, arm7 %llu) into %s\n",
                    static_cast<unsigned long long>(profile_frames),
                    static_cast<unsigned long long>(instructions[0]),
                    static_cast<unsigned long long>(instructions[1]), profile_path.c_str());
        std::fflush(stdout);
        if (want_project) project.trace_files.push_back(profile_path);
    }

    bool ok = true;
    if (info || check || !extract_dir.empty() || want_project) {
        ModuleSet set;
        if (!build_modules(rom, set, error)) {
            std::cerr << rom_path << ": " << error << "\n";
            return 1;
        }
        if (info) {
            print_program("ARM9", rom.header.arm9, rom.header.arm9_autoload_hook, set.arm9);
            print_program("ARM7", rom.header.arm7, rom.header.arm7_autoload_hook, set.arm7);
            print_modules(set);
        }
        if (!extract_dir.empty() && !extract(rom, set, extract_dir, error)) {
            std::cerr << error << "\n";
            ok = false;
        }
        if (check && !check_symbols(set, symbols_dir)) ok = false;
        if (want_project && !ok) {
            // With --check, a configuration that does not describe this ROM
            // must not go on to steer the analyzer.
            std::cerr << "ndsrecomp: not recompiling, because what was asked for before it failed\n";
        } else if (want_project) {
            project.rom_path = rom_path;
            project.symbols_dir = symbols_dir;
            if (project.name.empty()) project.name = sanitize(fs::path(rom_path).stem().string());
            project.runtime_dir = find_runtime_dir(argv[0], runtime_arg, NDSRECOMP_DEFAULT_RUNTIME_DIR);
            if (!recompile(rom, set, project, std::cout, error)) {
                std::cerr << error << "\n";
                return 1;
            }
        }
    }
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        // Out of memory on an absurd input, mostly: a clean failure, not a crash.
        std::cerr << "ndsrecomp: " << e.what() << "\n";
        return 1;
    }
}
