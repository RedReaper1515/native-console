// Super Nintendo backend: drives snesrecomp, CMake, and Ninja.
//
// The generated executable carries its ROM (a resource on Windows) and needs
// no firmware: the APU boot ROM is our own. Battery saves are <game_id>.sav
// next to it, save states <game_id>.state1 to .state10.
//
// Before recompiling, snesrecomp --profile plays the game with the
// interpreter under a built-in input script and records where code is
// entered (both CPUs of an SA-1 cartridge), and the WRAM code it ran. The
// trace is kept in <game_dir>/profile/ (outside the wiped work tree); other
// traces placed there (a game's --trace-out from longer play) are used too.
#include "hub_consoles.h"
#include "hub_platform.h"
#include "hub_settings.h"

#include <cstdio>
#include <sstream>

namespace hub {

namespace {

// How well a header candidate at `at` looks like the real one (the runtime's
// snes_cart.c scores the same way, in more detail).
int score_header(const std::vector<uint8_t>& rom, size_t at, bool lorom) {
    if (at + 0x40 > rom.size()) return -1000;
    const uint8_t* h = &rom[at];
    int score = 0;
    const unsigned checksum = h[0x1E] | h[0x1F] << 8, complement = h[0x1C] | h[0x1D] << 8;
    if (((checksum + complement) & 0xFFFFu) == 0xFFFFu) score += 8;
    const uint8_t map = h[0x15];
    if (lorom && (map == 0x20 || map == 0x30 || map == 0x23 || map == 0x32)) score += 6;
    if (!lorom && (map == 0x21 || map == 0x31 || map == 0x25 || map == 0x35)) score += 6;
    if (h[0x17] >= 0x07 && h[0x17] <= 0x0D) score += 2;
    int printable = 0;
    for (int i = 0; i < 21; ++i) printable += h[i] >= 0x20 && h[i] < 0x7F;
    score += printable / 7;
    const unsigned reset = h[0x3C] | h[0x3D] << 8;
    score += reset >= 0x8000 ? 2 : -4;
    return score;
}

const char* coprocessor_name(uint8_t type, uint8_t subtype) {
    if ((type & 0x0F) < 3 || (type & 0x0F) > 6) return nullptr;
    switch (type >> 4) {
        case 0x0: return "DSP";
        case 0x1: return "Super FX";
        case 0x2: return "OBC1";
        case 0x3: return "SA-1";
        case 0x4: return "S-DD1";
        case 0x5: return "S-RTC";
        case 0xF: return subtype == 0x10 ? "Cx4" : "SPC7110 / ST01x";
        default: return "unknown";
    }
}

class SuperNintendoBackend final : public ConsoleBackend {
public:
    std::string family_id() const override { return "snes"; }
    std::string family_name() const override { return "Super Nintendo"; }
    std::vector<std::string> extensions() const override { return {".sfc", ".smc"}; }
    bool supported() const override { return true; }

    bool probe(const std::string& rom_path, RomInfo& info, std::string& error) const override {
        std::vector<uint8_t> rom;
        if (!platform::read_binary_file(rom_path, rom, 0x410200)) {
            error = "cannot read " + rom_path;
            return false;
        }
        const long long size = platform::file_size(rom_path);
        if (size % 1024 == 512 && rom.size() > 512) rom.erase(rom.begin(), rom.begin() + 512);  // copier header
        if (rom.size() < 0x8000) {
            error = "file is too small to be a Super Nintendo ROM";
            return false;
        }
        struct Candidate { size_t at; bool lorom; const char* map; };
        const Candidate candidates[] = {{0x7FC0, true, "LoROM"}, {0xFFC0, false, "HiROM"}, {0x40FFC0, false, "ExHiROM"}};
        const Candidate* best = nullptr;
        int best_score = 3;
        for (const auto& c : candidates) {
            const int s = score_header(rom, c.at, c.lorom);
            if (s > best_score) {
                best_score = s;
                best = &c;
            }
        }
        if (!best) {
            error = "no Super Nintendo cartridge header found";
            return false;
        }
        const uint8_t* h = &rom[best->at];
        const uint8_t type = h[0x16];
        const char* chip = coprocessor_name(type, h[0x0F]);
        if (chip && std::string(chip) != "SA-1") {
            error = std::string("the cartridge uses a ") + chip + " coprocessor, which is not supported yet";
            return false;
        }
        info.console_id = "snes";
        info.header_title = sanitize_header_title(std::string(reinterpret_cast<const char*>(h), 21));
        info.title = title_from_file_name(rom_path);
        info.size = size;
        std::ostringstream notes;
        notes << (chip ? "SA-1" : best->map);
        if ((h[0x15] & 0x10) != 0) notes << ", FastROM";
        const uint8_t low = type & 0x0F;
        if ((low == 2 || low == 5 || low == 6) && h[0x18]) notes << ", battery RAM " << (1 << h[0x18]) << " KiB";
        static const char* regions[] = {"Japan", "USA", "Europe"};
        if (h[0x19] < 3) notes << ", " << regions[h[0x19]];
        info.notes = notes.str();
        return true;
    }

    std::string recompiler_name() const override { return "snesrecomp"; }

    // Profiles the game headless into data_dir/boot.txt (and boot.txt.wram),
    // then recompiles with that trace and any other traces in data_dir.
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

    std::string runtime_folder() const override { return "runtime-snes"; }

    std::vector<std::string> launch_args(const std::string& game_dir) const override {
        (void)game_dir;
        return {};
    }

    bool is_persistence_file(const std::string& game_id, const std::string& file_name) const override {
        if (file_name == game_id + ".sav" || file_name == game_id + ".bmp") return true;
        return file_name.rfind(game_id + ".state", 0) == 0;
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

std::unique_ptr<ConsoleBackend> make_snes_backend() {
    return std::make_unique<SuperNintendoBackend>();
}

}  // namespace hub
