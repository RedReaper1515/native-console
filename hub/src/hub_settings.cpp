#include "hub_settings.h"

#include "hub_consoles.h"
#include "hub_json.h"
#include "hub_platform.h"

#include <algorithm>
#include <filesystem>

#ifndef HUB_SDL2_DIR
#define HUB_SDL2_DIR ""
#endif

namespace fs = std::filesystem;

namespace hub {

namespace {

std::string first_existing(const std::vector<std::string>& candidates) {
    for (const auto& c : candidates) {
        if (!c.empty() && platform::exists(c)) return c;
    }
    return "";
}

#ifdef _WIN32
// Enumerates Visual Studio / Build Tools installation roots such as
// C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools.
std::vector<std::string> visual_studio_roots() {
    std::vector<std::string> roots;
    std::vector<std::string> bases = {
        platform::getenv_utf8("ProgramFiles(x86)"),
        platform::getenv_utf8("ProgramFiles"),
    };
    for (const auto& base : bases) {
        if (base.empty()) continue;
        std::string vs = platform::join(base, "Microsoft Visual Studio");
        if (!platform::is_directory(vs)) continue;
        std::error_code ec;
        std::vector<std::string> versions;
        for (const auto& entry : fs::directory_iterator(platform::to_path(vs), ec)) {
            if (entry.is_directory()) versions.push_back(platform::from_path(entry.path()));
        }
        // Newest version first.
        std::sort(versions.rbegin(), versions.rend());
        for (const auto& version : versions) {
            std::error_code ec2;
            for (const auto& edition : fs::directory_iterator(platform::to_path(version), ec2)) {
                if (edition.is_directory()) roots.push_back(platform::from_path(edition.path()));
            }
        }
    }
    return roots;
}
#endif

}  // namespace

std::string Settings::settings_path() const {
    return platform::join(data_dir, "settings.json");
}

bool Settings::load(std::string& error) {
    std::string text;
    if (!read_text_file(settings_path(), text)) {
        return true;  // first run
    }
    Json j;
    if (!Json::parse(text, j, error)) {
        error = "settings.json: " + error;
        return false;
    }
    games_dir = j.get_string("games_dir", games_dir);
    covers_dir = j.get_string("covers_dir", covers_dir);
    // Settings from before the recompiler map had one key per recompiler.
    for (const auto& [family, key] : {std::pair<const char*, const char*>{"gb", "gbrecomp_exe"}, {"gba", "gbarecomp_exe"}}) {
        const std::string path = j.get_string(key, "");
        if (!path.empty()) recompilers[family] = path;
    }
    if (const Json* map = j.get("recompilers"); map && map->is_object()) {
        for (const auto& [family, value] : map->object) {
            if (value.is_string()) recompilers[family] = value.string;
        }
    }
    cmake_exe = j.get_string("cmake_exe", cmake_exe);
    ninja_exe = j.get_string("ninja_exe", ninja_exe);
    vcvars_bat = j.get_string("vcvars_bat", vcvars_bat);
    sdl2_cmake_dir = j.get_string("sdl2_cmake_dir", sdl2_cmake_dir);
    sdl2_dll = j.get_string("sdl2_dll", sdl2_dll);
    build_type = j.get_string("build_type", build_type);
    generated_compile_jobs = static_cast<int>(j.get_int("generated_compile_jobs", generated_compile_jobs));
    compress_executables = j.get_bool("compress_executables", compress_executables);
    compact_resume = j.get_bool("compact_resume", compact_resume);
    code_coverage = j.get_string("code_coverage", code_coverage);
    gb_extra_launch_args = j.get_string("gb_extra_launch_args", gb_extra_launch_args);
    hide_game_console = j.get_bool("hide_game_console", hide_game_console);
    gc_renderer = j.get_string("gc_renderer", gc_renderer);
    if (gc_renderer != "software" && gc_renderer != "opengl" && gc_renderer != "fast") gc_renderer.clear();
    gc_internal_scale = static_cast<int>(j.get_int("gc_internal_scale", gc_internal_scale));
    if (gc_internal_scale < 0 || gc_internal_scale > 4) gc_internal_scale = 0;
    wii_key_file = j.get_string("wii_key_file", wii_key_file);
    wii_trial = j.get_bool("wii_trial", wii_trial);
    cover_capture_frame = static_cast<int>(j.get_int("cover_capture_frame", cover_capture_frame));
    default_view = j.get_string("default_view", default_view);
    ui_scale = static_cast<float>(j.get_number("ui_scale", ui_scale));
    steam_root = j.get_string("steam_root", steam_root);
    steam_account = j.get_string("steam_account", steam_account);
    docker_exe = j.get_string("docker_exe", docker_exe);
    steam_runtime_image = j.get_string("steam_runtime_image", steam_runtime_image);
    export_dir = j.get_string("export_dir", export_dir);
    export_targets = j.get_string("export_targets", export_targets);
    python_exe = j.get_string("python_exe", python_exe);
    gb_data_tool = j.get_string("gb_data_tool", gb_data_tool);
    return true;
}

bool Settings::save(std::string& error) const {
    Json j = Json::make_object();
    j.set("schema", "native-console.hub.settings");
    j.set("schema_version", 1);
    j.set("games_dir", games_dir);
    j.set("covers_dir", covers_dir);
    Json map = Json::make_object();
    for (const auto& [family, path] : recompilers) map.set(family, path);
    j.set("recompilers", map);
    // Also under the old keys, so an older hub reading this file still finds them.
    j.set("gbrecomp_exe", recompiler("gb"));
    j.set("gbarecomp_exe", recompiler("gba"));
    j.set("cmake_exe", cmake_exe);
    j.set("ninja_exe", ninja_exe);
    j.set("vcvars_bat", vcvars_bat);
    j.set("sdl2_cmake_dir", sdl2_cmake_dir);
    j.set("sdl2_dll", sdl2_dll);
    j.set("build_type", build_type);
    j.set("generated_compile_jobs", generated_compile_jobs);
    j.set("compress_executables", compress_executables);
    j.set("compact_resume", compact_resume);
    j.set("code_coverage", code_coverage);
    j.set("gb_extra_launch_args", gb_extra_launch_args);
    j.set("hide_game_console", hide_game_console);
    j.set("gc_renderer", gc_renderer);
    j.set("gc_internal_scale", gc_internal_scale);
    j.set("wii_key_file", wii_key_file);
    j.set("wii_trial", wii_trial);
    j.set("cover_capture_frame", cover_capture_frame);
    j.set("default_view", default_view);
    j.set("ui_scale", static_cast<double>(ui_scale));
    j.set("steam_root", steam_root);
    j.set("steam_account", steam_account);
    j.set("docker_exe", docker_exe);
    j.set("steam_runtime_image", steam_runtime_image);
    j.set("export_dir", export_dir);
    j.set("export_targets", export_targets);
    if (!python_exe.empty()) j.set("python_exe", python_exe);
    if (!gb_data_tool.empty()) j.set("gb_data_tool", gb_data_tool);
    return write_text_file_atomic(settings_path(), j.dump(2), error);
}

void Settings::detect_defaults() {
    const std::string exe_dir = platform::executable_dir();

    if (games_dir.empty()) games_dir = platform::join(data_dir, "games");
    if (covers_dir.empty()) covers_dir = platform::join(data_dir, "covers");

#ifdef _WIN32
    const std::string exe_suffix = ".exe";
#else
    const std::string exe_suffix = "";
#endif

    // Each console's recompiler: next to the hub, in bin/, or on PATH.
    for (const auto& backend : backends()) {
        std::string& path = recompilers[backend->family_id()];
        if (!path.empty()) continue;
        const std::string name = backend->recompiler_name();
        path = first_existing({
            platform::join(exe_dir, name + exe_suffix),
            platform::join(platform::join(exe_dir, "bin"), name + exe_suffix),
            platform::find_on_path(name),
        });
    }

    std::vector<std::string> cmake_candidates = {platform::find_on_path("cmake")};
    std::vector<std::string> ninja_candidates = {platform::find_on_path("ninja")};
    std::vector<std::string> vcvars_candidates;
#ifdef _WIN32
    for (const auto& root : visual_studio_roots()) {
        const std::string ext = platform::join(root, "Common7\\IDE\\CommonExtensions\\Microsoft");
        cmake_candidates.push_back(platform::join(ext, "CMake\\CMake\\bin\\cmake.exe"));
        ninja_candidates.push_back(platform::join(ext, "CMake\\Ninja\\ninja.exe"));
        vcvars_candidates.push_back(platform::join(root, "VC\\Auxiliary\\Build\\vcvars64.bat"));
    }
#endif
    if (cmake_exe.empty()) cmake_exe = first_existing(cmake_candidates);
    if (ninja_exe.empty()) ninja_exe = first_existing(ninja_candidates);
    if (vcvars_bat.empty()) vcvars_bat = first_existing(vcvars_candidates);

    if (sdl2_cmake_dir.empty()) {
        // A release carries SDL2's development files beside the hub, in
        // SDL2/; a hub run from its build tree has the SDL2 it was built with.
        const std::string beside = platform::join(platform::join(exe_dir, "SDL2"), "cmake");
        std::string compiled_in = HUB_SDL2_DIR;
        if (platform::is_directory(beside)) sdl2_cmake_dir = beside;
        else if (platform::is_directory(compiled_in)) sdl2_cmake_dir = platform::absolute(compiled_in);
    }
#ifdef _WIN32
    if (sdl2_dll.empty()) {
        std::vector<std::string> dll_candidates = {platform::join(exe_dir, "SDL2.dll")};
        if (!sdl2_cmake_dir.empty()) {
            std::string sdl_root = platform::parent_path(sdl2_cmake_dir);
            dll_candidates.push_back(platform::join(sdl_root, "lib\\x64\\SDL2.dll"));
            dll_candidates.push_back(platform::join(sdl_root, "bin\\SDL2.dll"));
        }
        sdl2_dll = first_existing(dll_candidates);
    }
#endif
    if (docker_exe.empty()) {
        std::vector<std::string> docker_candidates = {platform::find_on_path("docker")};
#ifdef _WIN32
        docker_candidates.push_back(platform::join(platform::getenv_utf8("ProgramFiles"), "Docker\\Docker\\resources\\bin\\docker.exe"));
#else
        docker_candidates.push_back("/usr/local/bin/docker");
        docker_candidates.push_back("/usr/bin/docker");
#endif
        docker_exe = first_existing(docker_candidates);
    }
    if (steam_runtime_image.empty()) steam_runtime_image = "registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest";
    if (build_type.empty()) build_type = "Release";
    if (generated_compile_jobs < 0) generated_compile_jobs = 0;
    if (cover_capture_frame <= 0) cover_capture_frame = 600;
}

std::string Settings::recompiler(const std::string& family_id) const {
    auto it = recompilers.find(family_id);
    return it == recompilers.end() ? std::string() : it->second;
}

std::vector<std::string> Settings::shared_toolchain_problems() const {
    std::vector<std::string> problems;
    if (cmake_exe.empty() || !platform::exists(cmake_exe)) problems.push_back("CMake executable not found");
    if (ninja_exe.empty() || !platform::exists(ninja_exe)) problems.push_back("Ninja executable not found");
#ifdef _WIN32
    if (vcvars_bat.empty() || !platform::exists(vcvars_bat)) problems.push_back("vcvars64.bat not found (MSVC Build Tools)");
#endif
    return problems;
}

std::vector<std::string> Settings::validate() const {
    std::vector<std::string> problems;
    for (const auto& backend : backends()) {
        for (const auto& p : backend->toolchain_problems(*this)) {
            if (std::find(problems.begin(), problems.end(), p) == problems.end()) problems.push_back(p);
        }
    }
    return problems;
}

}  // namespace hub
