// Persistent hub settings: data locations and the host toolchain used to build games.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace hub {

struct Settings {
    // Locations (data_dir is chosen at startup and not persisted inside settings.json).
    std::string data_dir;
    std::string games_dir;
    std::string covers_dir;

    // Toolchain.
    // Recompiler executable per console family id ("gb" -> gbrecomp, "gba" ->
    // gbarecomp, "snes" -> snesrecomp; ConsoleBackend::recompiler_name()).
    std::map<std::string, std::string> recompilers;
    std::string cmake_exe;
    std::string ninja_exe;
    std::string vcvars_bat;      // Windows only: MSVC environment script
    std::string sdl2_cmake_dir;  // SDL2_DIR passed to generated projects
    std::string sdl2_dll;        // Windows only: copied next to generated executables
    std::string build_type = "Release";
    int generated_compile_jobs = 8;
    bool compress_executables = true;  // NTFS LZX on Windows; the exe stays directly runnable
    bool compact_resume = true;        // gbrecomp --compact-resume: smaller executables
    // How much of the ROM the recompiler compiles. Anything not compiled runs
    // exactly through the interpreter, so this only trades size for speed:
    //   full     - every plausible pointer target (largest, historical default)
    //   balanced - pointer targets that pass the code-likeness probe
    //   small    - no pointer-derived seeds; reachable code plus scanned code
    //   minimal  - reachable code only; nearly everything interpreted
    std::string code_coverage = "balanced";

    // Runtime and presentation defaults.
    std::string gb_extra_launch_args;  // appended to every Game Boy launch
    bool hide_game_console = true;
    // How GameCube games draw when the hub starts them: "" leaves it to the
    // game's own menu setting, "software", "opengl" or "fast" passes --renderer.
    std::string gc_renderer;
    // How large the fast renderer draws when the hub starts a GameCube or
    // Wii game: 0 leaves it to the game's own menu setting, 1 to 4 passes
    // --internal-scale.
    int gc_internal_scale = 0;
    // The Wii common key, which a retail Wii disc needs once, when it is
    // built: a file of 16 bytes, of 32 hex digits, or a console's keys.bin.
    // It is the user's to supply; none ships with this program.
    std::string wii_key_file;
    // Wii is not supported yet; this turns its backend on for someone who
    // wants to try a game anyway (hub_consoles.h, set_wii_trial).
    bool wii_trial = false;
    int cover_capture_frame = 600;
    std::string default_view = "grid";
    float ui_scale = 0.0f;  // 0 = follow display DPI

    // "Compile for export": builds for other computers.
    std::string docker_exe;        // Linux / SteamOS builds run in Valve's Steam Runtime SDK container
    std::string steam_runtime_image = "registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest";
    std::string export_dir;        // last destination folder
    std::string export_targets = "windows,linux,macos";  // last selection

    // "Add to Steam".
    std::string steam_root;     // empty = detect the Steam install
    std::string steam_account;  // userdata folder name (account id); empty = most recent sign-in

    // A Game Boy game's edited data (hub_edited_data.h): what builds a data
    // tree into an image before the game starts. Both are found when empty,
    // and written to settings.json only when set.
    std::string python_exe;    // empty = python on PATH
    std::string gb_data_tool;  // tools/gb_data.py; empty = the one beside the hub or in its checkout

    std::string settings_path() const;
    bool load(std::string& error);
    bool save(std::string& error) const;

    // The recompiler for a console family, or "" when none is set.
    std::string recompiler(const std::string& family_id) const;

    // Fills any empty toolchain fields from the hub executable location, PATH, and known installs.
    void detect_defaults();
    // Problems every build has (CMake, Ninja, MSVC); each console adds its
    // own in ConsoleBackend::toolchain_problems().
    std::vector<std::string> shared_toolchain_problems() const;
    // Every console's problems, each once: empty when all of them can build.
    std::vector<std::string> validate() const;
};

}  // namespace hub
