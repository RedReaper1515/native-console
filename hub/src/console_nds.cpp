// Nintendo DS backend: drives ndsrecomp, CMake, and Ninja.
//
// The generated executable carries its ROM (a resource on Windows) and needs
// no firmware: the BIOS replacements and the firmware's user settings are
// the runtime's own. Saves are <game_id>.sav next to it, the contents of the
// cartridge's save chip.
//
// Before recompiling, ndsrecomp --profile plays the game with the
// interpreter under a built-in input script (stylus taps and strokes as well
// as buttons) and records what ran: interrupt handlers and functions reached
// only through pointers the analysis cannot follow are found that way. The
// trace is kept in <game_dir>/profile/ (outside the wiped work tree); other
// traces placed there (a game's --trace-out from longer play) are used too.
// A ds-decomp configuration for the game in <game_dir>/profile/symbols/
// (config.yaml and what it names) is passed as --symbols.
#include "hub_consoles.h"
#include "hub_platform.h"
#include "hub_settings.h"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace hub {

namespace {

uint32_t get32(const std::vector<uint8_t>& h, size_t at) {
    return h[at] | (uint32_t(h[at + 1]) << 8) | (uint32_t(h[at + 2]) << 16) | (uint32_t(h[at + 3]) << 24);
}

// The header's checksum: CRC-16 with polynomial 0xA001, starting at 0xFFFF.
unsigned crc16(const uint8_t* data, size_t size) {
    unsigned crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

// Where a program may load: main RAM for either CPU, and for the ARM7 the
// work RAM homebrew puts its program in.
bool loads_in_ram(uint32_t address, uint32_t size, bool arm7) {
    const uint64_t end = uint64_t(address) + size;
    if (address >= 0x02000000u && end <= 0x02400000u) return true;
    return arm7 && address >= 0x037F8000u && end <= 0x03810000u;
}

class NintendoDsBackend final : public ConsoleBackend {
public:
    std::string family_id() const override { return "nds"; }
    std::string family_name() const override { return "Nintendo DS"; }
    std::vector<std::string> extensions() const override { return {".nds"}; }
    bool supported() const override { return true; }

    bool probe(const std::string& rom_path, RomInfo& info, std::string& error) const override {
        // The header, and with it the secure area of a commercial game.
        std::vector<uint8_t> h;
        if (!platform::read_binary_file(rom_path, h, 0x8000)) {
            error = "cannot read " + rom_path;
            return false;
        }
        if (h.size() < 0x200) {
            error = "file is too small to be a Nintendo DS ROM";
            return false;
        }
        const long long file_size = platform::file_size(rom_path);
        const uint32_t arm9_offset = get32(h, 0x20), arm9_entry = get32(h, 0x24), arm9_ram = get32(h, 0x28),
                       arm9_size = get32(h, 0x2C);
        const uint32_t arm7_offset = get32(h, 0x30), arm7_ram = get32(h, 0x38), arm7_size = get32(h, 0x3C);
        const bool checksum_ok = crc16(h.data(), 0x15E) == unsigned(h[0x15E] | h[0x15F] << 8);
        const bool layout_ok = arm9_size != 0 && arm7_size != 0 && arm9_offset >= 0x200 && arm7_offset >= 0x200 &&
                               uint64_t(arm9_offset) + arm9_size <= uint64_t(file_size) &&
                               uint64_t(arm7_offset) + arm7_size <= uint64_t(file_size) &&
                               loads_in_ram(arm9_ram, arm9_size, false) && loads_in_ram(arm7_ram, arm7_size, true) &&
                               arm9_entry >= arm9_ram && arm9_entry - arm9_ram < arm9_size;
        if (!layout_ok) {
            error = checksum_ok ? "the Nintendo DS header describes programs that do not fit the file or the console's memory"
                                : "no Nintendo DS header found";
            return false;
        }
        if (h[0x12] == 0x03) {
            error = "a DSi-only title: the DSi's hardware is not supported";
            return false;
        }
        // A dump straight off a cartridge has the first 2 KiB of the ARM9
        // program encrypted; ndsrecomp decides the same way (rom.cpp).
        if (arm9_offset >= 0x4000 && arm9_offset < 0x8000 && arm9_size >= 0x800 &&
            size_t(arm9_offset) + 0x800 <= h.size()) {
            static const uint8_t destroyed_id[8] = {0xFF, 0xDE, 0xFF, 0xE7, 0xFF, 0xDE, 0xFF, 0xE7};
            const uint8_t* area = &h[arm9_offset];
            if (std::memcmp(area, destroyed_id, 8) != 0 && std::memcmp(area, "encryObj", 8) != 0) {
                size_t zeros = 0;
                for (size_t i = 0; i < 0x800; ++i) zeros += area[i] == 0;
                if (zeros < 256) {
                    error = "the dump's secure area is encrypted. A dump with a decrypted secure area is needed: "
                            "decrypting it takes Nintendo's key table, which Native Console never ships";
                    return false;
                }
            }
        }
        info.console_id = "nds";
        info.header_title = sanitize_header_title(std::string(reinterpret_cast<const char*>(&h[0x00]), 12));
        info.title = title_from_file_name(rom_path);
        info.size = file_size;
        std::ostringstream notes;
        notes << "game code " << sanitize_header_title(std::string(reinterpret_cast<const char*>(&h[0x0C]), 4))
              << ", maker " << sanitize_header_title(std::string(reinterpret_cast<const char*>(&h[0x10]), 2));
        const uint32_t overlays = get32(h, 0x54) / 32;
        if (overlays) notes << ", " << overlays << " overlays";
        if (h[0x12] == 0x02) notes << ", DSi-enhanced (runs as a DS game)";
        if (!checksum_ok) notes << ", header checksum wrong";
        info.notes = notes.str();
        return true;
    }

    std::string recompiler_name() const override { return "ndsrecomp"; }

    // Profiles the game headless into data_dir/boot.txt, then recompiles
    // with that trace and any other traces in data_dir.
    std::vector<GenerateStep> generate_steps(const Settings& s, const std::string& rom_path,
                                             const std::string& game_id, const std::string& source_dir,
                                             const std::string& data_dir) const override {
        const std::string recompiler = s.recompiler(family_id());
        if (recompiler.empty()) return {};
        const std::string boot_trace = platform::join(data_dir, "boot.txt");
        std::vector<std::string> recompile = {recompiler, rom_path, "-o", source_dir, "--name", game_id};
        for (const auto& trace : trace_files(data_dir, boot_trace)) {
            recompile.push_back("--trace");
            recompile.push_back(trace);
        }
        const std::string symbols = platform::join(data_dir, "symbols");
        if (platform::exists(platform::join(symbols, "config.yaml"))) {
            recompile.push_back("--symbols");
            recompile.push_back(symbols);
        }
        return {
            {"profiling the game", {recompiler, rom_path, "--profile", boot_trace}},
            {"recompiling ROM to C", recompile},
        };
    }

    std::string runtime_folder() const override { return "runtime-nds"; }

    std::vector<std::string> launch_args(const std::string& game_dir) const override {
        (void)game_dir;
        return {};
    }

    bool is_persistence_file(const std::string& game_id, const std::string& file_name) const override {
        // <id>.sav.bak is a save of another size that the frontend set aside
        // rather than overwrite.
        if (file_name == game_id + ".sav" || file_name == game_id + ".sav.bak" || file_name == game_id + ".bmp") {
            return true;
        }
        return file_name.rfind(game_id + ".state", 0) == 0;
    }

    // A Windows export of Phantom Hourglass unpacks and runs (docs/NDS-PLAN.md, P6);
    // Linux and macOS are P7, not started.
    std::string export_problem(const std::string& target_id) const override {
        return untested_export_problem(family_name(), target_id, {"windows"});
    }

    bool supports_cover_capture() const override { return true; }

    // A DS game takes longer to reach its title than a cartridge of the
    // consoles the setting was tuned for: logos, then often an opening
    // scene (Phantom Hourglass shows its title at about frame 2,700).
    int cover_capture_frame(int setting_frame) const override { return setting_frame * 5; }

    std::vector<std::string> cover_capture_args(const std::string& game_dir,
                                                int frame,
                                                const std::string& screenshot_prefix) const override {
        (void)game_dir;
        // Headless: the runner never writes saves, so the player's save is
        // safe. It dumps the last frame, the top screen above the bottom
        // one, as <prefix>_<frame>.ppm.
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "_%05d.ppm", frame);
        return {"--frames", std::to_string(frame), "--dump-frame", screenshot_prefix + suffix, "--quiet"};
    }
};

}  // namespace

std::unique_ptr<ConsoleBackend> make_nds_backend() {
    return std::make_unique<NintendoDsBackend>();
}

}  // namespace hub
