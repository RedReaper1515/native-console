// Game Boy Advance backend: drives gbarecomp, CMake, and Ninja.
//
// The generated executable carries its ROM (a resource on Windows) and needs
// no BIOS. Battery saves are <game_id>.sav next to it, save states from the
// in-game menu <game_id>.state1 to .state10.
//
// Before recompiling, gbarecomp --profile plays the game with the
// interpreter under a built-in input script and records where code is
// entered, including code the game copies to RAM. Static analysis alone
// misses that code, and it would otherwise run interpreted. The trace is
// kept in <game_dir>/profile/ (outside the wiped work tree); other traces
// placed there (a game's --trace-out from longer play) are used as well.
#include "hub_consoles.h"
#include "hub_platform.h"
#include "hub_settings.h"

#include <cstdio>
#include <sstream>

namespace hub {

namespace {

class GameBoyAdvanceBackend final : public ConsoleBackend {
public:
    std::string family_id() const override { return "gba"; }
    std::string family_name() const override { return "Game Boy Advance"; }
    std::vector<std::string> extensions() const override { return {".gba"}; }
    bool supported() const override { return true; }

    bool probe(const std::string& rom_path, RomInfo& info, std::string& error) const override {
        std::vector<uint8_t> header;
        if (!platform::read_binary_file(rom_path, header, 0xC0)) {
            error = "cannot read " + rom_path;
            return false;
        }
        if (header.size() < 0xC0) {
            error = "file is too small to be a Game Boy Advance ROM";
            return false;
        }
        if (header[0xB2] != 0x96) {
            error = "missing GBA header fixed byte (0x96 at 0xB2)";
            return false;
        }
        info.console_id = "gba";
        info.header_title = sanitize_header_title(std::string(reinterpret_cast<const char*>(&header[0xA0]), 12));
        info.title = title_from_file_name(rom_path);
        info.size = platform::file_size(rom_path);
        std::ostringstream notes;
        notes << "game code " << sanitize_header_title(std::string(reinterpret_cast<const char*>(&header[0xAC]), 4))
              << ", maker " << sanitize_header_title(std::string(reinterpret_cast<const char*>(&header[0xB0]), 2));
        info.notes = notes.str();
        return true;
    }

    std::string recompiler_name() const override { return "gbarecomp"; }

    // Profiles the game headless into data_dir/boot.txt, then recompiles with
    // that trace and any other traces already in data_dir.
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

    std::string runtime_folder() const override { return "runtime-gba"; }

    std::vector<std::string> launch_args(const std::string& game_dir) const override {
        (void)game_dir;
        return {};
    }

    std::string link_name() const override { return "Link cable (multiplayer, trading)"; }
    int link_port() const override { return 47400; }

    bool is_persistence_file(const std::string& game_id, const std::string& file_name) const override {
        if (file_name == game_id + ".sav" || file_name == game_id + ".bmp") return true;
        return file_name.rfind(game_id + ".state", 0) == 0;  // save states from the in-game menu
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

std::unique_ptr<ConsoleBackend> make_gba_backend() {
    return std::make_unique<GameBoyAdvanceBackend>();
}

}  // namespace hub
