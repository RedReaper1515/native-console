// Console backend registry. Each backend knows how to identify ROMs for one
// hardware family and how to turn them into a native executable.
//
// Adding a console: implement ConsoleBackend in a new console_<family>.cpp,
// register it in backends() (hub_consoles.cpp), and add its console ids to
// all_consoles(). Its recompiler is found, stored and shown in Settings by
// recompiler_name() alone, and the shared build script (prepare_build)
// serves it unchanged. console_snes.cpp is the example to copy; the full
// process is in docs/ADDING-A-CONSOLE.md. A family that does not compile
// anything overrides prepare_build instead. A family whose code is kept
// outside the repository is registered through hub_extra.h.
#pragma once

#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace hub {

struct Settings;

struct ConsoleInfo {
    std::string id;          // "gb", "gbc", "gba", "snes", "gc"
    std::string name;        // "Game Boy Color"
    std::string short_name;  // "GBC"
    std::string family_id;   // backend family that handles it
    unsigned rgb = 0;        // 0xRRGGBB of its badge, for a console the hub has no colour for
};

struct RomInfo {
    std::string console_id;    // one of ConsoleInfo::id
    std::string title;         // human title derived from header or file name
    std::string header_title;  // raw cartridge header title
    std::string notes;         // e.g. mapper or region hints
    long long size = 0;
};

struct BuildRequest {
    std::string game_id;   // stable identifier, safe as a C identifier and file prefix
    std::string game_dir;  // per-game working directory owned by the hub
    std::string rom_path;  // ROM copy inside game_dir
    // Where the ROM was imported from, for a family whose game is more than
    // the imported file and needs its siblings.
    std::string rom_original_path;
    const Settings* settings = nullptr;
};

struct BuildPlan {
    std::string script_path;          // generated build script the pipeline executes
    std::string expected_executable;  // where the script leaves the executable (inside work_dir)
    std::string source_dir;           // generated project directory (inside work_dir)
    std::string work_dir;             // transient generation + compile tree; wiped before and after a build
};

class ConsoleBackend {
public:
    virtual ~ConsoleBackend() = default;

    virtual std::string family_id() const = 0;
    virtual std::string family_name() const = 0;
    virtual std::vector<std::string> extensions() const = 0;  // lower-case, with leading dot
    // Exact file names this family's game goes by when it has no extension,
    // lower-case. A console's ROM is matched by extensions() instead.
    virtual std::vector<std::string> file_names() const { return {}; }

    // A console that is not supported yet can still be one its owner has
    // asked to try (set_wii_trial below): supported() says yes for it then.
    virtual bool supported() const = 0;
    virtual std::string unsupported_reason() const { return ""; }

    virtual bool probe(const std::string& rom_path, RomInfo& info, std::string& error) const = 0;

    // The recompiler's executable name without extension ("gbarecomp").
    // Settings finds it next to the hub, in bin/ and on PATH, and keeps its
    // path under family_id() (Settings::recompiler()).
    virtual std::string recompiler_name() const = 0;

    // The commands (argv, run in order) that turn the ROM into a self-contained
    // CMake project in source_dir. data_dir keeps anything else they produce,
    // such as profiling traces. The project is portable C, so the same tree
    // compiles for every export target. Empty when the recompiler is not
    // configured.
    struct GenerateStep {
        std::string label;              // "recompiling ROM to C"
        std::vector<std::string> argv;
    };
    virtual std::vector<GenerateStep> generate_steps(const Settings& settings, const std::string& rom_path,
                                                     const std::string& game_id, const std::string& source_dir,
                                                     const std::string& data_dir) const = 0;

    // Folder inside a generated project that holds the runtime snapshot.
    virtual std::string runtime_folder() const = 0;

    // What stops this family's builds: the shared toolchain (CMake, Ninja,
    // MSVC), the recompiler, and SDL2 when requires_sdl2().
    virtual std::vector<std::string> toolchain_problems(const Settings& settings) const;
    // The generated project cannot configure without SDL2.
    virtual bool requires_sdl2() const { return false; }
    // Extra arguments for configuring the generated project ("-DNAME=value").
    virtual std::vector<std::string> cmake_args(const Settings& settings) const {
        (void)settings;
        return {};
    }

    // Writes the build script (generate steps, CMake, Ninja, SDL2.dll) and
    // reports where the executable will be. The default serves every backend.
    virtual bool prepare_build(const BuildRequest& request, BuildPlan& plan, std::string& error) const;

    // Arguments appended to every launch of a built game from this family.
    virtual std::vector<std::string> launch_args(const std::string& game_dir) const = 0;

    // Games of this family can be played together: two windows on this
    // computer, or one hosting for others across a network. Empty when they
    // cannot; otherwise the heading the game's page shows, and the UDP port
    // a host listens on (for the tooltip). The runtime takes --player N,
    // --link-host, --link-local and --link-join HOST.
    virtual std::string link_name() const { return ""; }
    virtual int link_port() const { return 0; }
    // Extra arguments for window `player` (1 or 2) of a two-player launch on
    // this computer, where the two windows have to share the devices.
    virtual std::vector<std::string> two_player_args(const std::string& game_dir, int player) const {
        (void)game_dir; (void)player;
        return {};
    }

    // A played run kept so that it can be played again, to the cycle (the
    // runtime's --record-run): the arguments that record one sitting into
    // run_dir, which is also where the game keeps its save meanwhile. Empty
    // when the family's runtime cannot record one.
    virtual std::vector<std::string> record_run_args(const std::string& run_dir) const {
        (void)run_dir;
        return {};
    }

    // Extra files installed next to the executable: (source path, installed file name).
    virtual std::vector<std::pair<std::string, std::string>> install_files(const BuildRequest& request) const {
        (void)request;
        return {};
    }

    // Files the runtime writes next to the executable that must survive rebuilds
    // (battery save, RTC, savestates, UI layout).
    virtual bool is_persistence_file(const std::string& game_id, const std::string& file_name) const = 0;

    // The line an exported game's README uses to say where its saves go, for
    // the builds that keep them next to the executable.
    virtual std::string save_note(const std::string& game_id) const {
        return "Battery saves (" + game_id + ".sav) are written next to the executable.";
    }

    // Why games of this family cannot be exported for a target ("windows",
    // "linux", "macos" - ExportTargetInfo::id) yet; empty when they can.
    // The Windows export packs install_files() beside the executable.
    virtual std::string export_problem(const std::string& target_id) const {
        (void)target_id;
        return "";
    }

    // Headless run that dumps one frame as a Netpbm image with the given prefix.
    virtual bool supports_cover_capture() const { return false; }
    // The frame this family captures for the hub's cover setting (tuned for
    // cartridges, which reach a title screen in seconds).
    virtual int cover_capture_frame(int setting_frame) const { return setting_frame; }
    virtual std::vector<std::string> cover_capture_args(const std::string& game_dir,
                                                        int frame,
                                                        const std::string& screenshot_prefix) const {
        (void)game_dir; (void)frame; (void)screenshot_prefix;
        return {};
    }

    // Headless runs that only record a profile for the next build (a family
    // that finds code by tracing a run, such as GameCube). The hub runs them
    // as a Profile job after a build of a game that has its cover, when an
    // output is missing - so after the rebuild with the cover run's trace -
    // and rebuilds when they create a file. Each writes `scratch` (args say
    // so), which replaces `output` only when the run exits cleanly; `output`
    // is copied to `scratch` first, so a run that adds to its file keeps what
    // the file held. A run that fails is logged and dropped. Outputs belong in
    // <game_dir>/profile/, which every build reads.
    struct ProfileRun {
        std::string label;              // the job step shown while it runs
        std::vector<std::string> args;  // for the game's executable
        std::string output;
        std::string scratch;
    };
    virtual std::vector<ProfileRun> profile_runs(const std::string& game_dir) const {
        (void)game_dir;
        return {};
    }
};

const std::vector<ConsoleInfo>& all_consoles();
const ConsoleInfo* console_info(const std::string& console_id);
const std::vector<std::unique_ptr<ConsoleBackend>>& backends();

// Wii is not supported yet (docs/WII-PLAN.md): its backend says so until a
// retail game has been through the gates. Somebody who wants to try one
// anyway turns it on - "Try Wii games" in Settings, which the hub passes
// here when settings are loaded and saved, or NATIVE_CONSOLE_WII=1 in the
// environment. Process-wide, like the environment variable it stands beside.
void set_wii_trial(bool on);

const ConsoleBackend* backend_for_family(const std::string& family_id);
const ConsoleBackend* backend_for_file(const std::string& path);
// The files under `dir` (recursively) that some backend claims, by extension
// or by name. What an import or an export of a folder works on.
std::vector<std::string> roms_in_folder(const std::string& dir);

// Shared helpers for backends.
std::string sanitize_header_title(const std::string& raw);
std::string title_from_file_name(const std::string& path);
// Traces for --trace: data_dir/boot.txt (the profiling step's output) first,
// then any other .txt placed directly in data_dir (a game's --trace-out from
// longer play).
std::vector<std::string> trace_files(const std::string& data_dir, const std::string& boot_trace);

}  // namespace hub
