// Game Boy / Game Boy Color backend: drives gbrecomp, CMake, and Ninja.
#include "hub_consoles.h"
#include "hub_platform.h"
#include "hub_settings.h"

#include <cstdio>
#include <sstream>

namespace hub {

namespace {

class GameBoyBackend final : public ConsoleBackend {
public:
    std::string family_id() const override { return "gb"; }
    std::string family_name() const override { return "Game Boy / Game Boy Color"; }
    std::vector<std::string> extensions() const override { return {".gb", ".gbc", ".sgb"}; }
    bool supported() const override { return true; }

    bool probe(const std::string& rom_path, RomInfo& info, std::string& error) const override {
        std::vector<uint8_t> header;
        if (!platform::read_binary_file(rom_path, header, 0x150)) {
            error = "cannot read " + rom_path;
            return false;
        }
        if (header.size() < 0x150) {
            error = "file is too small to be a Game Boy ROM";
            return false;
        }
        const uint8_t cgb_flag = header[0x143];
        const bool cgb_only = cgb_flag == 0xC0;
        const bool cgb_capable = cgb_flag == 0x80 || cgb_only;
        // Titles are 16 bytes on DMG carts and 11 or 15 bytes on CGB carts.
        std::string raw(reinterpret_cast<const char*>(&header[0x134]), cgb_capable ? 15 : 16);
        info.header_title = sanitize_header_title(raw);
        info.console_id = cgb_capable ? "gbc" : "gb";
        info.title = title_from_file_name(rom_path);
        info.size = platform::file_size(rom_path);

        std::ostringstream notes;
        notes << (cgb_only ? "CGB only" : cgb_capable ? "CGB enhanced" : "DMG");
        if (header[0x146] == 0x03) notes << ", SGB";
        static const char* mappers[] = {"ROM only", "MBC1", "MBC1+RAM", "MBC1+RAM+BATTERY"};
        const uint8_t cart = header[0x147];
        if (cart < 4) notes << ", " << mappers[cart];
        else if (cart >= 0x0F && cart <= 0x13) notes << ", MBC3";
        else if (cart >= 0x19 && cart <= 0x1E) notes << ", MBC5";
        else if (cart == 0x05 || cart == 0x06) notes << ", MBC2";
        else {
            char buf[16];
            std::snprintf(buf, sizeof(buf), ", cart 0x%02X", cart);
            notes << buf;
        }
        info.notes = notes.str();
        return true;
    }

    static std::vector<std::string> coverage_flags(const std::string& profile) {
        if (profile == "full") return {"--pointer-scan", "loose"};
        if (profile == "small") return {"--pointer-scan", "off"};
        if (profile == "minimal") return {"--pointer-scan", "off", "--no-scan"};
        return {"--pointer-scan", "strict"};  // balanced
    }

    std::string recompiler_name() const override { return "gbrecomp"; }

    std::vector<GenerateStep> generate_steps(const Settings& s, const std::string& rom_path,
                                             const std::string& game_id, const std::string& source_dir,
                                             const std::string& data_dir) const override {
        (void)data_dir;
        const std::string recompiler = s.recompiler(family_id());
        if (recompiler.empty()) return {};
        std::vector<std::string> argv = {recompiler, rom_path, "-o", source_dir, "--output-prefix", game_id};
        if (s.compact_resume) argv.push_back("--compact-resume");
        for (auto& flag : coverage_flags(s.code_coverage)) argv.push_back(flag);
        return {{"recompiling ROM to C", argv}};
    }

    std::string runtime_folder() const override { return "runtime"; }

    // The Game Boy project always builds its SDL frontend.
    bool requires_sdl2() const override { return true; }
    std::vector<std::string> cmake_args(const Settings& s) const override {
        return {"-DGBRECOMP_GENERATED_COMPILE_JOBS=" + std::to_string(s.generated_compile_jobs)};
    }

    std::vector<std::string> launch_args(const std::string& game_dir) const override {
        return {"--log-file", platform::join(game_dir, "last-run.log")};
    }

    std::vector<std::string> record_run_args(const std::string& run_dir) const override {
        return {"--record-run", run_dir};
    }

    std::string link_name() const override { return "Link cable (multiplayer, trading)"; }
    int link_port() const override { return 47402; }

    std::vector<std::string> two_player_args(const std::string& game_dir, int player) const override {
        // Player 2's own log beside player 1's (the last --log-file wins).
        if (player != 2) return {};
        return {"--log-file", platform::join(game_dir, "last-run-p2.log")};
    }

    bool is_persistence_file(const std::string& game_id, const std::string& file_name) const override {
        if (file_name == "imgui.ini") return true;
        if (file_name == game_id + ".sav" || file_name == game_id + ".rtc") return true;
        return file_name.rfind(game_id + ".state", 0) == 0;
    }

    bool supports_cover_capture() const override { return true; }

    std::vector<std::string> cover_capture_args(const std::string& game_dir,
                                                int frame,
                                                const std::string& screenshot_prefix) const override {
        // The scratch save directory keeps automated runs away from the player's real battery save.
        return {"--headless",
                "--no-audio",
                "--limit-frames", std::to_string(frame + 1),
                "--dump-frames", std::to_string(frame),
                "--screenshot-prefix", screenshot_prefix,
                "--save-dir", platform::join(game_dir, "scratch-save"),
                "--log-file", platform::join(game_dir, "cover-capture.log")};
    }
};

}  // namespace

std::unique_ptr<ConsoleBackend> make_gameboy_backend() {
    return std::make_unique<GameBoyBackend>();
}

}  // namespace hub
