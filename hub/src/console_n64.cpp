// Nintendo 64 backend: drives n64recomp, CMake, and Ninja.
//
// The generated executable carries its ROM (a resource on Windows) and needs
// no firmware: the boot is the runtime's own (n64_boot.c). Saves are
// <game_id>.sav next to it: the cartridge's EEPROM, SRAM or FlashRAM, then
// the Controller Paks.
//
// N64 code runs from RDRAM, where games copy and decompress it at run time.
// Before recompiling, n64recomp --profile plays the game with the
// interpreter under a built-in input script and records where code ran and
// the RDRAM pages it ran from; the recompiler compiles those as well. The
// trace is kept in <game_dir>/profile/ (outside the wiped work tree); other
// traces placed there (a game's --trace-out from longer play) are used too.
// A game also records, while it is played, the code its build left to the
// interpreter (<id>.trace beside the save): a rebuild compiles that as well.
//
// The playbook's gates up to P6 passed on Windows with Ocarina of Time
// (docs/N64-PLAN.md). Linux, SteamOS and macOS builds (P7) are not done.
#include "hub_consoles.h"
#include "hub_platform.h"
#include "hub_settings.h"

#include <cstdio>
#include <sstream>

namespace hub {

namespace {

// The header's first word in the three byte orders ROM files come in.
enum class Order { Native, Swapped16, Little, Unknown };

Order detect(const std::vector<uint8_t>& h) {
    if (h.size() < 4) return Order::Unknown;
    if (h[0] == 0x80 && h[1] == 0x37) return Order::Native;
    if (h[0] == 0x37 && h[1] == 0x80) return Order::Swapped16;
    if (h[0] == 0x40 && h[1] == 0x12 && h[3] == 0x80) return Order::Little;
    return Order::Unknown;
}

uint8_t byte_at(const std::vector<uint8_t>& h, size_t i, Order order) {
    switch (order) {
        case Order::Swapped16: return h[i ^ 1];
        case Order::Little: return h[i ^ 3];
        default: return h[i];
    }
}

class Nintendo64Backend final : public ConsoleBackend {
public:
    std::string family_id() const override { return "n64"; }
    std::string family_name() const override { return "Nintendo 64"; }
    std::vector<std::string> extensions() const override { return {".z64", ".n64", ".v64"}; }
    bool supported() const override { return true; }

    bool probe(const std::string& rom_path, RomInfo& info, std::string& error) const override {
        std::vector<uint8_t> h;
        if (!platform::read_binary_file(rom_path, h, 0x40)) {
            error = "cannot read " + rom_path;
            return false;
        }
        const long long size = platform::file_size(rom_path);
        const Order order = detect(h);
        if (h.size() < 0x40 || size < 0x101000 || order == Order::Unknown) {
            error = "no Nintendo 64 cartridge header found";
            return false;
        }
        std::string title, id;
        for (size_t i = 0x20; i < 0x34; ++i) title += static_cast<char>(byte_at(h, i, order));
        for (size_t i = 0x3B; i < 0x3F; ++i) {
            const uint8_t ch = byte_at(h, i, order);
            id += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
        }
        info.console_id = "n64";
        info.header_title = sanitize_header_title(title);
        info.title = title_from_file_name(rom_path);
        info.size = size;
        std::ostringstream notes;
        notes << "Game code " << id << ", " << (size / (1024 * 1024)) << " MiB";
        if (order == Order::Swapped16) notes << ", byte-swapped (.v64)";
        if (order == Order::Little) notes << ", little-endian (.n64)";
        info.notes = notes.str();
        return true;
    }

    std::string recompiler_name() const override { return "n64recomp"; }

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
        // What the game recorded while it was played: the code its last
        // build left to the interpreter (<game_dir>/game/<id>.trace, kept
        // across rebuilds as a persistence file).
        const std::string play_trace =
            platform::join(platform::join(platform::parent_path(data_dir), "game"), game_id + ".trace");
        if (platform::exists(play_trace)) {
            recompile.push_back("--trace");
            recompile.push_back(play_trace);
        }
        return {
            {"profiling the game", {recompiler, rom_path, "--profile", boot_trace}},
            {"recompiling ROM to C", recompile},
        };
    }

    std::string runtime_folder() const override { return "runtime-n64"; }

    std::vector<std::string> launch_args(const std::string& game_dir) const override {
        (void)game_dir;
        return {};
    }

    bool is_persistence_file(const std::string& game_id, const std::string& file_name) const override {
        return file_name == game_id + ".sav" || file_name == game_id + ".bmp" || file_name == game_id + ".trace";
    }

    // A Windows export of Ocarina of Time unpacks and runs (docs/N64-PLAN.md, P6);
    // Linux and macOS are P7, not started.
    std::string export_problem(const std::string& target_id) const override {
        return untested_export_problem(family_name(), target_id, {"windows"});
    }

    bool supports_cover_capture() const override { return true; }
    // A cartridge of this console shows its makers' logos for twenty seconds
    // or more before its title: three times the frame the setting names.
    int cover_capture_frame(int setting_frame) const override { return setting_frame * 3; }

    std::vector<std::string> cover_capture_args(const std::string& game_dir,
                                                int frame,
                                                const std::string& screenshot_prefix) const override {
        (void)game_dir;
        // Headless: the runner never writes saves, so the player's save is
        // safe. It dumps the last frame as <prefix>_<frame>.ppm.
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "_%05d.ppm", frame);
        return {"--frames", std::to_string(frame), "--dump-frame", screenshot_prefix + suffix, "--quiet"};
    }
};

}  // namespace

std::unique_ptr<ConsoleBackend> make_n64_backend() {
    return std::make_unique<Nintendo64Backend>();
}

}  // namespace hub
