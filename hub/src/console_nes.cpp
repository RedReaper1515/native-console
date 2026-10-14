// NES backend: drives nesrecomp, CMake, and Ninja.
//
// The generated executable carries its ROM (a resource on Windows) and needs
// no firmware: the NES has none. Battery saves are <game_id>.sav next to it,
// save states <game_id>.state1 to .state10.
//
// Before recompiling, nesrecomp --profile plays the game with the
// interpreter under a built-in input script and records where code is
// entered and which PRG bank was mapped there: a cartridge switches banks at
// run time, so that is how code reached through a bank switch is found. The
// trace is kept in <game_dir>/profile/ (outside the wiped work tree); other
// traces placed there (a game's --trace-out from longer play) are used too.
#include "hub_consoles.h"
#include "hub_platform.h"
#include "hub_settings.h"

#include <cstdio>
#include <sstream>

namespace hub {

namespace {

// The boards runtime-nes/src/nes_cart.c implements, by iNES mapper number.
const char* board_name(unsigned mapper, bool chr_rom) {
    switch (mapper) {
        case 0: return "NROM";
        case 1: return "MMC1";
        case 2: return "UxROM";
        case 3: return "CNROM";
        case 4: return "MMC3";
        case 7: return "AxROM";
        case 9: return "MMC2";
        case 10: return "MMC4";
        case 11: return "Color Dreams";
        case 34: return chr_rom ? "NINA-001" : "BNROM";
        case 66: return "GxROM";
        case 71: return "Camerica";
        default: return nullptr;
    }
}

class NesBackend final : public ConsoleBackend {
public:
    std::string family_id() const override { return "nes"; }
    std::string family_name() const override { return "Nintendo Entertainment System"; }
    std::vector<std::string> extensions() const override { return {".nes"}; }
    bool supported() const override { return true; }

    bool probe(const std::string& rom_path, RomInfo& info, std::string& error) const override {
        std::vector<uint8_t> h;
        if (!platform::read_binary_file(rom_path, h, 16)) {
            error = "cannot read " + rom_path;
            return false;
        }
        if (h.size() < 16 || h[0] != 'N' || h[1] != 'E' || h[2] != 'S' || h[3] != 0x1A) {
            error = "no iNES header found (headerless and UNIF dumps are not supported)";
            return false;
        }
        const bool nes20 = (h[7] & 0x0C) == 0x08;
        // Old tools wrote text over bytes 7-15; then the high mapper nibble
        // is garbage (the runtime's nes_cart_parse decides the same way).
        bool dirty = false;
        for (int i = 12; i < 16; ++i) dirty = dirty || h[i] != 0;
        unsigned mapper = h[6] >> 4;
        if (nes20) mapper |= (h[7] & 0xF0u) | ((h[8] & 0x0Fu) << 8);
        else if (!dirty) mapper |= h[7] & 0xF0u;
        if ((h[7] & 0x03) != 0 && nes20) {
            error = "arcade (Vs. System / PlayChoice-10) images are not supported";
            return false;
        }
        const bool chr_rom = h[5] != 0 || (nes20 && (h[9] >> 4) != 0);
        const char* board = board_name(mapper, chr_rom);
        if (!board) {
            error = "the cartridge uses mapper " + std::to_string(mapper) + ", which is not supported yet";
            return false;
        }
        if (nes20 && (h[12] & 3) == 1) {
            error = "PAL cartridges are not supported yet (the runtime has NTSC timing only)";
            return false;
        }
        info.console_id = "nes";
        info.header_title.clear();  // an iNES image carries no title
        info.title = title_from_file_name(rom_path);
        info.size = platform::file_size(rom_path);
        std::ostringstream notes;
        notes << board << ", " << (h[4] * 16) << " KiB PRG";
        if (chr_rom) notes << ", " << (h[5] * 8) << " KiB CHR";
        else notes << ", CHR RAM";
        if (h[6] & 0x02) notes << ", battery RAM";
        info.notes = notes.str();
        return true;
    }

    std::string recompiler_name() const override { return "nesrecomp"; }

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
        return {
            {"profiling the game", {recompiler, rom_path, "--profile", boot_trace}},
            {"recompiling ROM to C", recompile},
        };
    }

    std::string runtime_folder() const override { return "runtime-nes"; }

    std::vector<std::string> launch_args(const std::string& game_dir) const override {
        (void)game_dir;
        return {};
    }

    bool is_persistence_file(const std::string& game_id, const std::string& file_name) const override {
        if (file_name == game_id + ".sav" || file_name == game_id + ".bmp") return true;
        return file_name.rfind(game_id + ".state", 0) == 0;
    }

    // No NES export has been tested yet, on any target.
    std::string export_problem(const std::string& target_id) const override {
        return untested_export_problem(family_name(), target_id, {});
    }

    bool supports_cover_capture() const override { return true; }

    std::vector<std::string> cover_capture_args(const std::string& game_dir,
                                                int frame,
                                                const std::string& screenshot_prefix) const override {
        (void)game_dir;
        // Headless: the runner never writes battery saves, so the player's
        // save is safe. It dumps the last frame as <prefix>_<frame>.ppm.
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "_%05d.ppm", frame);
        return {"--frames", std::to_string(frame), "--dump-frame", screenshot_prefix + suffix, "--quiet"};
    }
};

}  // namespace

std::unique_ptr<ConsoleBackend> make_nes_backend() {
    return std::make_unique<NesBackend>();
}

}  // namespace hub
