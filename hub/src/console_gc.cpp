// GameCube and Wii backends: drive gcrecomp, CMake, and Ninja.
//
// The two consoles are one family in everything but name. A Wii is the
// GameCube's design with more of everything, the runtime that plays both is
// runtime-gc, and gcrecomp reads either kind of disc - so one class below
// serves both, told which it is. They are two entries in the hub because a
// player thinks of them as two consoles, because both use .iso and the disc
// itself has to say whose it is, and because one can be supported before the
// other: Wii stays unsupported until a retail game has been through the
// gates (docs/WII-PLAN.md), and "Try Wii games" in Settings (or
// NATIVE_CONSOLE_WII=1 in the environment) turns it on for whoever is
// doing that.
//
// A retail Wii disc is encrypted. gcrecomp needs the Wii common key once, to
// build; the hub passes the file named in Settings (Wii common key file),
// and gcrecomp also looks in WII_COMMON_KEY, WII_KEY_FILE and beside the
// disc. No key ships with this program. The built game carries the key to
// its own disc and needs nothing more.
//
// A Wii game's saves are the console's flash memory, <game_id>.nand next to
// the executable; the GameCube memory card (a Wii has the slots) is kept too.
//
// A GameCube game is a disc, and only its executable is code. gcrecomp pulls
// the DOL out of the disc and recompiles it into a project that carries its
// own copy of runtime-gc (and Dear ImGui), so the project builds anywhere.
// The generated executable embeds that code and still boots through the
// disc's own apploader, reading every other file from the disc, which is
// installed beside it as <game_id>.gcdata (the name the generated program
// looks for). Nothing from a console's firmware is needed.
//
// The disc is over a gigabyte and the library already keeps it in rom/, so
// the pipeline installs <game_id>.gcdata as a hard link to that copy where
// the volume allows (a copy otherwise): one set of bytes, and the game
// folder is still self-contained.
//
// The memory card is <game_id>.card next to the executable and the in-game
// screenshot <game_id>.bmp; the frontend's settings live in the user's
// preference directory, not beside the game.
//
// Covers come from the generated program's headless runner, which boots the
// disc beside it the way the window does, runs recompiled to a field and
// dumps the picture on screen. It inserts no memory card, so it never
// touches a save.
//
// Profiling rides on the cover run rather than being a build step of its
// own. Code reached only through function pointers is invisible to
// gcrecomp's static analysis and runs interpreted; the runner's --trace-out
// records where the interpreter was entered, and gcrecomp --trace makes those
// addresses roots. A profiling step before the build would need a built game
// to run (or the interpreter, at a sixth of the speed) and minutes of it -
// the cover run already spends them, so it writes <game_dir>/profile/
// cover.trace, and every build passes each *.trace in profile/ to gcrecomp
// (a longer play's --trace-out dropped there counts too). The cover run never
// reaches play, so a profile run plays 3,000 fields of --auto-input into
// profile/play.trace (profile_runs). The first build has no trace; the cover
// capture creates cover.trace and the hub rebuilds; that build runs the play
// profile (a minute on a traced build, five on an untraced one) and the hub
// rebuilds again. docs/GC-PLAN.md (P6, and "Quick wins") has what it gains.
//
// Export: Windows packs the disc beside the executable. Linux and macOS are
// P7 (docs/GC-PLAN.md) - neither packs the disc yet, and the macOS kit has no
// bundle script for this runtime - so the hub says so rather than producing
// a game that cannot start.
#include "hub_consoles.h"
#include "hub_platform.h"
#include "hub_settings.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace hub {

namespace {

// The disc header's magic word, at 0x1C of a GameCube disc and at 0x18 of a
// Wii disc.
constexpr uint32_t kDiscMagic = 0xC2339F3Du;
constexpr uint32_t kWiiMagic = 0x5D1C9EA3u;
// A CISO keeps its block map in the first 0x8000 bytes; the disc starts after.
constexpr size_t kCisoHeader = 0x8000;

uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

const char* region_name(char code) {
    switch (code) {
        case 'E': return "USA";
        case 'J': return "Japan";
        case 'P': return "Europe";
        case 'D': return "Germany";
        case 'F': return "France";
        case 'S': return "Spain";
        case 'I': return "Italy";
        case 'U': return "Australia";
        case 'K': return "Korea";
        default: return nullptr;
    }
}

// What the front of an image says it is. The header is the disc's own first
// bytes, wherever the container keeps them.
struct DiscFront {
    const uint8_t* header = nullptr;
    const char* container = "full disc image";
    std::string problem;  // why this is not an image anything here reads
};

DiscFront disc_front(const std::vector<uint8_t>& head) {
    DiscFront front;
    auto magic = [&](const char* text) { return head.size() >= 4 && std::memcmp(head.data(), text, 4) == 0; };
    size_t base = 0;
    if (magic("CISO")) {
        base = kCisoHeader;
        front.container = "CISO image";
    } else if (magic("WBFS")) {
        // A WBFS keeps a copy of the disc's header in its second drive
        // sector, whose size the first sector gives as a power of two.
        const unsigned shift = head.size() > 8 ? head[8] : 0;
        if (shift < 9 || shift > 14) {
            front.problem = "this WBFS file's header makes no sense";
            return front;
        }
        base = static_cast<size_t>(1) << shift;
        front.container = "WBFS image";
    } else if (magic("RVZ\x01") || magic("WIA\x01") || (head.size() >= 4 && be32(head.data()) == 0x01C00BB1u)) {
        front.problem = "RVZ, WIA and GCZ images are not read yet; convert the disc to ISO (or WBFS for a Wii "
                        "disc, CISO for a GameCube one), for example with Dolphin's Convert File";
        return front;
    }
    if (head.size() < base + 0x440) {
        front.problem = "file is too small to be a disc image";
        return front;
    }
    front.header = &head[base];
    return front;
}

// "Try Wii games" (hub_consoles.h, set_wii_trial).
std::atomic<bool> g_wii_trial{false};

class DiscBackend final : public ConsoleBackend {
public:
    explicit DiscBackend(bool wii) : wii_(wii) {}

    std::string family_id() const override { return wii_ ? "wii" : "gc"; }
    std::string family_name() const override { return wii_ ? "Wii" : "GameCube"; }
    // The compressed formats are claimed so that dropping one says why it is
    // not taken, instead of nothing happening.
    std::vector<std::string> extensions() const override {
        if (wii_) return {".iso", ".wbfs", ".ciso", ".rvz", ".wia"};
        return {".iso", ".gcm", ".ciso", ".rvz", ".gcz"};
    }
    bool supported() const override {
        if (!wii_) return true;
        if (g_wii_trial.load()) return true;
        const char* enabled = std::getenv("NATIVE_CONSOLE_WII");
        return enabled && *enabled && std::strcmp(enabled, "0") != 0;
    }
    std::string unsupported_reason() const override {
        return "Wii support is being built and no retail game has passed its gates yet (docs/WII-PLAN.md); "
               "turn on \"Try Wii games\" in Settings to try one anyway";
    }

    bool probe(const std::string& rom_path, RomInfo& info, std::string& error) const override {
        std::vector<uint8_t> head;
        if (!platform::read_binary_file(rom_path, head, kCisoHeader + 0x440)) {
            error = "cannot read " + rom_path;
            return false;
        }
        const DiscFront front = disc_front(head);
        if (!front.header) {
            error = front.problem;
            return false;
        }
        const uint8_t* disc = front.header;
        const bool is_wii = be32(disc + 0x18) == kWiiMagic;
        const bool is_gamecube = !is_wii && be32(disc + 0x1C) == kDiscMagic;
        if (!is_wii && !is_gamecube) {
            error = "no GameCube or Wii disc header";
            return false;
        }
        if (is_wii != wii_) {
            error = is_wii ? "this is a Wii disc" : "this is a GameCube disc";
            return false;
        }
        const std::string game_code = sanitize_header_title(std::string(reinterpret_cast<const char*>(disc), 6));
        info.console_id = wii_ ? "wii" : "gc";
        info.header_title = sanitize_header_title(std::string(reinterpret_cast<const char*>(disc + 0x20), wii_ ? 0x40 : 0x3E0));
        info.title = title_from_file_name(rom_path);
        info.size = platform::file_size(rom_path);
        std::ostringstream notes;
        notes << "game code " << game_code;
        if (game_code.size() >= 4) {
            if (const char* region = region_name(game_code[3])) notes << ", " << region;
        }
        if (disc[6]) notes << ", disc " << (int)disc[6] + 1;
        notes << ", " << front.container;
        if (wii_) notes << "; building it needs the Wii common key (Settings)";
        info.notes = notes.str();
        return true;
    }

    std::string recompiler_name() const override { return "gcrecomp"; }

    std::vector<GenerateStep> generate_steps(const Settings& s, const std::string& rom_path,
                                             const std::string& game_id, const std::string& source_dir,
                                             const std::string& data_dir) const override {
        const std::string recompiler = s.recompiler(family_id());
        if (recompiler.empty()) return {};
        std::vector<std::string> recompile = {recompiler, rom_path, "-o", source_dir, "--name", game_id};
        if (wii_ && !s.wii_key_file.empty()) {
            recompile.push_back("--key-file");
            recompile.push_back(s.wii_key_file);
        }
        if (platform::is_directory(data_dir)) {
            for (const auto& trace : platform::list_files_recursive(data_dir, {".trace"})) {
                if (platform::parent_path(trace) != data_dir) continue;
                recompile.push_back("--trace");
                recompile.push_back(trace);
            }
        }
        return {
            {"recompiling the disc's executable to C", recompile},
        };
    }

    std::string runtime_folder() const override { return "runtime-gc"; }

    // The window, the controller and the sound are the generated program's
    // SDL frontend; without SDL2 it would build a headless test runner.
    bool requires_sdl2() const override { return true; }

    std::vector<std::string> launch_args(const std::string& game_dir) const override {
        (void)game_dir;
        return {};
    }

    // The disc, beside the executable under the name it looks for.
    std::vector<std::pair<std::string, std::string>> install_files(const BuildRequest& request) const override {
        return {{request.rom_path, request.game_id + ".gcdata"}};
    }

    bool is_persistence_file(const std::string& game_id, const std::string& file_name) const override {
        // <id>.card.bak is the card a loaded save state replaced (gc_card.c).
        if (file_name == game_id + ".card" || file_name == game_id + ".card.bak" || file_name == game_id + ".bmp") {
            return true;
        }
        // A Wii's flash memory: the game's saves and the console's settings
        // (runtime-gc/include/wii_nand.h).
        if (wii_ && (file_name == game_id + ".nand" || file_name == game_id + ".nand.bak")) return true;
        // Save states, slots 1-10 (runtime-gc/include/gc_state.h).
        for (int slot = 1; slot <= 10; slot++) {
            if (file_name == game_id + ".state" + std::to_string(slot)) return true;
        }
        // The OpenGL renderer's shader cache (runtime-gc/src/gc_gl_programs.c):
        // the list of pipelines the game has drawn with, <id>.pipelines, and
        // the compiled programs, <id>.<16 hex digits>.glbin. Kept, so a rebuilt
        // game compiles the pipelines it needs before they are drawn with; a
        // program whose shader text changed has a new name and is compiled.
        if (file_name == game_id + ".pipelines") return true;
        const std::string prefix = game_id + ".";
        const std::string suffix = ".glbin";
        if (file_name.size() == prefix.size() + 16 + suffix.size() && file_name.compare(0, prefix.size(), prefix) == 0 &&
            file_name.compare(file_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            for (size_t i = prefix.size(); i < prefix.size() + 16; i++) {
                if (!std::isxdigit(static_cast<unsigned char>(file_name[i]))) return false;
            }
            return true;
        }
        return false;
    }

    std::string save_note(const std::string& game_id) const override {
        if (wii_) {
            return "The console's flash memory, which holds the game's saves (" + game_id + ".nand), a GameCube memory "
                   "card if the game uses one (" + game_id + ".card) and the OpenGL shader cache (" + game_id +
                   ".pipelines, " + game_id + ".*.glbin) are written next to the executable.";
        }
        return "The memory card (" + game_id + ".card), save states (" + game_id +
               ".state1-10) and the OpenGL shader cache (" + game_id + ".pipelines, " + game_id +
               ".*.glbin) are written next to the executable.";
    }

    // A Windows export of Shrek 2 works (docs/GC-PLAN.md, P6); no Wii game
    // has been exported, and Linux and macOS are P7, not started for either.
    std::string export_problem(const std::string& target_id) const override {
        if (wii_) return untested_export_problem(family_name(), target_id, {});
        return untested_export_problem(family_name(), target_id, {"windows"});
    }

    bool supports_cover_capture() const override { return true; }

    // The hub's setting is tuned for cartridges, which show a title screen
    // within seconds (600 frames, ten seconds). A disc spends its first
    // minute spinning up, loading and on publisher logos, so a GameCube game
    // runs five times as far: 600 becomes 3000 fields, fifty seconds of game
    // time and about two minutes headless. For Shrek 2, 600 fields is the
    // DreamWorks logo and 3000 its title menu.
    int cover_capture_frame(int setting_frame) const override { return setting_frame * 5; }

    // A field is the machine's frame: it counts the halves of an interlaced
    // picture, sixty a second.
    std::vector<std::string> cover_capture_args(const std::string& game_dir,
                                                int frame,
                                                const std::string& screenshot_prefix) const override {
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "_%05d.ppm", frame);
        std::vector<std::string> args = {"--frames", std::to_string(frame), "--dump-frame", screenshot_prefix + suffix};
        // The entry points the run found, for the next build (see the top).
        const std::string profile = platform::join(game_dir, "profile");
        std::string error;
        if (platform::ensure_directory(profile, error)) {
            args.push_back("--trace-out");
            args.push_back(platform::join(profile, "cover.trace"));
        }
        return args;
    }

    // The cover run has no input and never leaves the title, so play's code
    // (reached through function pointers) would still run interpreted. A
    // second run plays: --auto-input presses through the menus from field 600
    // and is in play by about 1,800. Its trace is a file of its own, so a run
    // that fails leaves the cover's trace as it was.
    std::vector<ProfileRun> profile_runs(const std::string& game_dir) const override {
        const std::string profile = platform::join(game_dir, "profile");
        const std::string output = platform::join(profile, "play.trace");
        const std::string scratch = output + ".part";
        return {{"recording 3000 fields of scripted play for the profile",
                 {"--frames", "3000", "--auto-input", "--trace-out", scratch},
                 output,
                 scratch}};
    }

private:
    bool wii_;
};

}  // namespace

void set_wii_trial(bool on) { g_wii_trial.store(on); }

std::unique_ptr<ConsoleBackend> make_gc_backend() {
    return std::make_unique<DiscBackend>(false);
}

std::unique_ptr<ConsoleBackend> make_wii_backend() {
    return std::make_unique<DiscBackend>(true);
}

}  // namespace hub
