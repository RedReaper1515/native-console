#include "hub_export.h"

#include "hub_consoles.h"
#include "hub_extra.h"
#include "hub_json.h"
#include "hub_library.h"
#include "hub_platform.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hub {

namespace {

// The hub's own targets, then any that came with families from outside
// (hub_extra.h), numbered from ExportTarget::FirstExtra in the order they
// were registered.
const std::vector<ExportTargetInfo>& targets() {
    static const std::vector<ExportTargetInfo> list = [] {
        std::vector<ExportTargetInfo> v = {
            {ExportTarget::Windows, "windows", "Windows (x64)", "A .zip with the .exe and SDL2.dll"},
            {ExportTarget::Linux, "linux", "Linux / SteamOS (x86-64)", "A .tar.gz built in Valve's Steam Runtime; runs on a Steam Deck"},
            {ExportTarget::MacKit, "macos", "macOS (build kit)", "A .tar.gz to finish on any Mac: double-click build.command to get the .app"},
        };
        const auto& extra = extra_families().export_targets;
        for (size_t i = 0; i < extra.size(); ++i) {
            v.push_back({static_cast<ExportTarget>(static_cast<int>(ExportTarget::FirstExtra) + static_cast<int>(i)),
                         extra[i].id.c_str(), extra[i].label.c_str(), extra[i].description.c_str()});
        }
        return v;
    }();
    return list;
}

// The target a family from outside brought, or null for one of the hub's own.
const ExtraExportTarget* extra_target(ExportTarget target) {
    const int index = static_cast<int>(target) - static_cast<int>(ExportTarget::FirstExtra);
    const auto& extra = extra_families().export_targets;
    if (index < 0 || index >= static_cast<int>(extra.size())) return nullptr;
    return &extra[static_cast<size_t>(index)];
}

std::string command_line(const std::vector<std::string>& argv) {
    std::string line;
    for (size_t i = 0; i < argv.size(); ++i) line += (i ? " " : "") + platform::quote_arg(argv[i]);
    return line;
}

// Single-quoted for sh/bash.
std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

bool write_file(const std::string& path, const std::string& text, std::string& error) {
    return write_text_file_atomic(path, text, error);
}

class Runner {
public:
    Runner(const ExportCallbacks& cb) : cb_(cb) {}

    void log(const std::string& line) const { if (cb_.log) cb_.log(line); }
    void step(const std::string& s) const {
        if (cb_.step) cb_.step(s);
        log("[export] " + s);
    }
    bool cancelled() const { return cb_.cancel && *cb_.cancel; }

    // Runs argv, streaming output to the log. Quiet mode keeps output for the error only.
    bool run(const std::vector<std::string>& argv, const std::string& cwd, std::string& error, bool quiet = false) const {
        return run_line(command_line(argv), cwd, error, quiet);
    }

    bool run_line(const std::string& line, const std::string& cwd, std::string& error, bool quiet = false) const {
        std::vector<std::string> captured;
        platform::RunResult r = platform::run_capture(line, cwd, [&](const std::string& out) {
            if (quiet) {
                captured.push_back(out);
                if (captured.size() > 40) captured.erase(captured.begin());
            } else {
                log(out);
            }
        }, cb_.cancel);
        if (cancelled()) {
            error = "cancelled";
            return false;
        }
        if (!r.started) {
            error = r.error;
            return false;
        }
        if (r.exit_code != 0) {
            error = "exited with code " + std::to_string(r.exit_code);
            for (const auto& l : captured) log(l);
            return false;
        }
        return true;
    }

private:
    const ExportCallbacks& cb_;
};

// ---------------------------------------------------------------------------
// Archives. Windows builds .tar.gz with its own bsdtar (System32\tar.exe) from
// an mtree spec, which records Unix permissions (build.command and Linux
// executables must stay executable). POSIX hosts set the bits on disk.
// ---------------------------------------------------------------------------

#ifdef _WIN32
std::string mtree_escape(const std::string& path) {
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '.' || c == '/' || c == '_' || c == '-' || c == '+') {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%03o", c);
            out += buf;
        }
    }
    return out;
}
#endif

std::string system_tar() {
#ifdef _WIN32
    std::string root = platform::getenv_utf8("SystemRoot");
    if (root.empty()) root = "C:\\Windows";
    return platform::join(platform::join(root, "System32"), "tar.exe");
#else
    return "tar";
#endif
}

// Packs parent/entry into out (.tar.gz); paths in `executables` (relative to entry, '/') get mode 0755.
bool make_tar_gz(const Runner& runner, const std::string& parent, const std::string& entry, const std::string& out,
                 const std::set<std::string>& executables, std::string& error) {
    std::error_code ec;
    fs::remove(platform::to_path(out), ec);
#ifdef _WIN32
    std::ostringstream spec;
    spec << "#mtree\n";
    const fs::path root = platform::to_path(platform::join(parent, entry));
    spec << "./" << mtree_escape(entry) << " type=dir mode=0755\n";
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::string rel = platform::from_path(it->path().lexically_relative(root));
        for (char& c : rel) if (c == '\\') c = '/';
        const std::string name = "./" + mtree_escape(entry + "/" + rel);
        if (it->is_directory()) {
            spec << name << " type=dir mode=0755\n";
        } else if (it->is_regular_file()) {
            spec << name << " type=file mode=" << (executables.count(rel) ? "0755" : "0644")
                 << " contents=" << mtree_escape(entry + "/" + rel) << "\n";
        }
    }
    if (ec) {
        error = "cannot list " + platform::from_path(root) + ": " + ec.message();
        return false;
    }
    const std::string spec_path = platform::join(parent, entry + ".mtree");
    if (!write_file(spec_path, spec.str(), error)) return false;
    bool ok = runner.run({system_tar(), "-czf", out, "@" + platform::file_name(spec_path)}, parent, error, true);
    fs::remove(platform::to_path(spec_path), ec);
    if (!ok) error = "tar: " + error;
    return ok;
#else
    for (const auto& rel : executables) {
        fs::permissions(platform::to_path(platform::join(platform::join(parent, entry), rel)),
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec,
                        ec);
    }
    if (!runner.run({system_tar(), "-czf", out, "-C", parent, entry}, parent, error, true)) {
        error = "tar: " + error;
        return false;
    }
    return true;
#endif
}

bool copy_tree(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(platform::to_path(to), ec);
    fs::copy(platform::to_path(from), platform::to_path(to),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy " + from + " to " + to + ": " + ec.message();
        return false;
    }
    return true;
}

bool publish(const Runner& runner, const std::string& file, const std::string& dest_dir,
             std::vector<std::string>& outputs, std::string& error) {
    const std::string dest = platform::join(dest_dir, platform::file_name(file));
    if (!platform::copy_file(file, dest, error)) return false;
    outputs.push_back(dest);
    runner.log("[export] wrote " + dest + " (" + std::to_string(platform::file_size(dest) / 1024) + " KiB)");
    return true;
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

bool export_windows(const Runner& runner, const ExportRequest& req, const Settings& s, const ConsoleBackend& backend,
                    const std::string& src, std::vector<std::string>& outputs, std::string& error) {
#ifndef _WIN32
    (void)runner; (void)req; (void)s; (void)backend; (void)src; (void)outputs;
    error = "Windows builds need a Windows computer";
    return false;
#else
    runner.step("compiling for Windows");
    const std::string build = platform::join(req.work_dir, "bw");  // short: MSVC path limit
    const std::string script = platform::join(req.work_dir, "compile-windows.cmd");
    auto q = [](const std::string& v) { return "\"" + v + "\""; };
    std::ostringstream cmd;
    cmd << "@echo off\r\nsetlocal\r\n"
        << "call " << q(s.vcvars_bat) << " >nul\r\n"
        << "if errorlevel 1 (echo vcvars64.bat failed & exit /b 1)\r\n"
        << "set \"PATH=" << platform::parent_path(s.cmake_exe) << ";" << platform::parent_path(s.ninja_exe) << ";%PATH%\"\r\n"
        << q(s.cmake_exe) << " -G Ninja -S " << q(src) << " -B " << q(build) << " -DCMAKE_BUILD_TYPE=" << s.build_type
        << " -DSDL2_DIR=" << q(s.sdl2_cmake_dir) << " -DGBRECOMP_GENERATED_COMPILE_JOBS=" << s.generated_compile_jobs << "\r\n"
        << "if errorlevel 1 exit /b 1\r\n"
        << q(s.ninja_exe) << " -C " << q(build) << "\r\n"
        << "if errorlevel 1 exit /b 1\r\n";
    if (!write_file(script, cmd.str(), error)) return false;
    if (!runner.run_line(platform::script_command(script), req.work_dir, error)) {
        error = "Windows compile " + error;
        return false;
    }

    runner.step("packing the Windows build");
    const std::string entry = req.game_id + "-windows-x64";
    const std::string stage = platform::join(platform::join(req.work_dir, "stage"), entry);
    if (!platform::ensure_directory(stage, error)) return false;
    const std::string exe = req.game_id + ".exe";
    if (!platform::copy_file(platform::join(build, exe), platform::join(stage, exe), error)) return false;
    if (!platform::copy_file(s.sdl2_dll, platform::join(stage, "SDL2.dll"), error)) return false;
    // What the game reads beside itself (a GameCube disc's content): linked
    // into the stage where the volume allows, since the stage is thrown away.
    BuildRequest install;
    install.game_id = req.game_id;
    install.rom_path = req.rom_path;
    install.settings = &s;
    for (const auto& [source, name] : backend.install_files(install)) {
        bool linked = false;
        if (!platform::link_or_copy_file(source, platform::join(stage, name), linked, error)) return false;
    }
    std::ostringstream readme;
    readme << req.title << " for Windows (x64)\r\n\r\n"
           << "Run " << exe << ". Keep SDL2.dll next to it.\r\n"
           << backend.save_note(req.game_id) << "\r\n";
    if (!write_file(platform::join(stage, "README.txt"), readme.str(), error)) return false;
    const std::string zip = platform::join(req.work_dir, entry + ".zip");
    std::error_code ec;
    fs::remove(platform::to_path(zip), ec);
    if (!runner.run({system_tar(), "-a", "-cf", zip, entry}, platform::parent_path(stage), error, true)) {
        error = "zip: " + error;
        return false;
    }
    return publish(runner, zip, req.dest_dir, outputs, error);
#endif
}

bool docker_ready(const Runner& runner, const Settings& s, std::string& error) {
    if (!runner.run({s.docker_exe, "info", "--format", "{{.OSType}}"}, "", error, true)) {
        error = "Docker is not running. Start Docker Desktop and try again";
        return false;
    }
    std::string ignored;
    if (runner.run({s.docker_exe, "image", "inspect", "--format", "{{.Id}}", s.steam_runtime_image}, "", ignored, true)) {
        return true;
    }
    runner.step("downloading the Steam Runtime SDK (several GB, first Linux export only)");
    if (!runner.run({s.docker_exe, "pull", s.steam_runtime_image}, "", error, true)) {
        error = "docker pull " + s.steam_runtime_image + " failed: " + error;
        return false;
    }
    return true;
}

bool export_linux(const Runner& runner, const ExportRequest& req, const Settings& s, const ConsoleBackend& backend,
                  std::vector<std::string>& outputs, std::string& error) {
    runner.step("starting Docker");
    if (!docker_ready(runner, s, error)) return false;

    runner.step("compiling for Linux / SteamOS");
    const std::string entry = req.game_id + "-linux-x86_64";
    std::ostringstream readme;
    readme << req.title << " for Linux and SteamOS (x86-64)\n\n"
           << "Needs glibc 2.29 or newer and SDL2; SteamOS and current desktop Linux have both.\n\n"
           << "Steam Deck: in Desktop Mode, extract this archive (for example into ~/Games),\n"
           << "then in Steam choose Add a Non-Steam Game and pick " << entry << "/" << req.game_id << ".\n"
           << backend.save_note(req.game_id) << "\n";
    if (!write_file(platform::join(req.work_dir, "README-linux.txt"), readme.str(), error)) return false;
    // gcc 14 in the SDK links libstdc++ statically: the game needs only glibc and SDL2.
    std::ostringstream sh;
    sh << "#!/bin/bash\nset -euo pipefail\n"
       << "export CC=gcc-14 CXX=g++-14\n"
       << "cmake -G Ninja -S /w/src -B /tmp/build -DCMAKE_BUILD_TYPE=" << s.build_type
       << " -DGBRECOMP_GENERATED_COMPILE_JOBS=$(nproc) >/dev/null\n"
       << "ninja -C /tmp/build\n"
       << "rm -rf /tmp/stage && mkdir -p /tmp/stage/" << entry << "\n"
       << "install -m 0755 /tmp/build/" << req.game_id << " /tmp/stage/" << entry << "/" << req.game_id << "\n"
       << "install -m 0644 /w/README-linux.txt /tmp/stage/" << entry << "/README.txt\n"
       << "tar -C /tmp/stage -czf /w/" << entry << ".tar.gz " << entry << "\n";
    if (!write_file(platform::join(req.work_dir, "compile-linux.sh"), sh.str(), error)) return false;

    std::vector<std::string> argv = {s.docker_exe, "run", "--rm", "-v", req.work_dir + ":/w"};
#ifndef _WIN32
    // Files written into the work folder should belong to the user, not root.
    argv.push_back("--user");
    argv.push_back(std::to_string(getuid()) + ":" + std::to_string(getgid()));
#endif
    argv.push_back(s.steam_runtime_image);
    argv.push_back("bash");
    argv.push_back("/w/compile-linux.sh");
    if (!runner.run(argv, req.work_dir, error)) {
        error = "Linux compile " + error;
        return false;
    }
    return publish(runner, platform::join(req.work_dir, entry + ".tar.gz"), req.dest_dir, outputs, error);
}

bool export_mac_kit(const Runner& runner, const ExportRequest& req, const ConsoleBackend& backend,
                    const std::string& src, std::vector<std::string>& outputs, std::string& error) {
    runner.step("assembling the macOS build kit");
    const std::string entry = req.game_id + "-macos-kit";
    const std::string stage_root = platform::join(req.work_dir, "stage");
    const std::string kit = platform::join(stage_root, entry);
    if (!copy_tree(src, platform::join(kit, "project"), error)) return false;

    const std::string finisher = "project/" + backend.runtime_folder() + "/cmake/make_macos_app.sh";
    std::ostringstream command;
    command << "#!/bin/bash\n"
            << "# Builds " << req.game_id << ".app next to this file. Double-click it in Finder,\n"
            << "# or run: bash build.command\n"
            << "cd \"$(dirname \"$0\")\" || exit 1\n"
            << "bash " << finisher << " --project project --build build --out . --name " << req.game_id
            << " --title " << sh_quote(req.title) << "\n"
            << "status=$?\n"
            << "[ $status -eq 0 ] && open -R " << req.game_id << ".app\n"
            << "exit $status\n";
    if (!write_file(platform::join(kit, "build.command"), command.str(), error)) return false;

    std::ostringstream readme;
    readme << req.title << " for macOS: build kit\n\n"
           << "This folder becomes a Mac app on any Mac (Apple Silicon or Intel, macOS 11 or newer).\n"
           << "Apple's compiler only runs on macOS, so the last step happens there.\n\n"
           << "1. Once per Mac, install the developer tools in Terminal:\n"
           << "     xcode-select --install\n"
           << "     brew install cmake ninja        (Homebrew: https://brew.sh)\n"
           << "2. Double-click build.command. If macOS says it cannot be opened, open Terminal,\n"
           << "   type \"bash \" (with a space), drag build.command into the window and press Return.\n"
           << "3. After a few minutes " << req.game_id << ".app appears next to build.command.\n"
           << "   Move it to Applications.\n\n"
           << "Battery saves go to ~/Library/Application Support/gbrecompiled/" << req.game_id << "/.\n";
    if (!write_file(platform::join(kit, "README.txt"), readme.str(), error)) return false;

    runner.step("packing the macOS build kit");
    const std::string archive = platform::join(req.work_dir, entry + ".tar.gz");
    if (!make_tar_gz(runner, stage_root, entry, archive, {"build.command", finisher}, error)) return false;
    return publish(runner, archive, req.dest_dir, outputs, error);
}

}  // namespace

const std::vector<ExportTargetInfo>& export_target_infos() { return targets(); }

const ExportTargetInfo& export_target_info(ExportTarget target) {
    for (const auto& t : targets()) {
        if (t.target == target) return t;
    }
    return targets().front();
}

std::vector<ExportTarget> parse_export_targets(const std::string& csv, std::vector<std::string>* unknown) {
    std::vector<ExportTarget> out;
    std::stringstream ss(csv);
    std::string name;
    while (std::getline(ss, name, ',')) {
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
        if (name.empty()) continue;
        bool found = false;
        for (const auto& t : targets()) {
            if (name == t.id) {
                found = true;
                bool dup = false;
                for (auto e : out) dup = dup || e == t.target;
                if (!dup) out.push_back(t.target);
            }
        }
        if (!found && unknown) unknown->push_back(name);
    }
    return out;
}

std::string format_export_targets(const std::vector<ExportTarget>& targets) {
    std::string out;
    for (auto t : targets) out += (out.empty() ? "" : ",") + std::string(export_target_info(t).id);
    return out;
}

std::string export_file_name(const std::string& game_id, ExportTarget target) {
    switch (target) {
        case ExportTarget::Windows: return game_id + "-windows-x64.zip";
        case ExportTarget::Linux: return game_id + "-linux-x86_64.tar.gz";
        case ExportTarget::MacKit: return game_id + "-macos-kit.tar.gz";
        default: break;
    }
    if (const ExtraExportTarget* extra = extra_target(target)) return game_id + extra->file_suffix;
    return game_id;
}

std::string export_target_problem(ExportTarget target, const Settings& s) {
    switch (target) {
        case ExportTarget::Windows: {
#ifndef _WIN32
            (void)s;
            return "needs a Windows computer";
#else
            std::vector<std::string> problems;
            if (s.vcvars_bat.empty() || !platform::exists(s.vcvars_bat)) problems.push_back("MSVC Build Tools (vcvars64.bat)");
            if (s.cmake_exe.empty() || !platform::exists(s.cmake_exe)) problems.push_back("CMake");
            if (s.ninja_exe.empty() || !platform::exists(s.ninja_exe)) problems.push_back("Ninja");
            if (s.sdl2_cmake_dir.empty() || !platform::is_directory(s.sdl2_cmake_dir)) problems.push_back("SDL2 CMake folder");
            if (s.sdl2_dll.empty() || !platform::exists(s.sdl2_dll)) problems.push_back("SDL2.dll");
            if (problems.empty()) return "";
            std::string out = "needs ";
            for (size_t i = 0; i < problems.size(); ++i) out += (i ? ", " : "") + problems[i];
            return out + " (see Settings)";
#endif
        }
        case ExportTarget::Linux:
            if (s.docker_exe.empty() || !platform::exists(s.docker_exe)) return "needs Docker Desktop (see Settings)";
            return "";
        case ExportTarget::MacKit:
#ifdef _WIN32
            if (!platform::exists(system_tar())) return "needs Windows' tar.exe (Windows 10 1803 or newer)";
#endif
            return "";
        default: break;
    }
    if (const ExtraExportTarget* extra = extra_target(target)) return extra->problem ? extra->problem(s) : "";
    return "";
}

std::string make_export_id(const std::string& title, const std::set<std::string>& taken) {
    std::string base = slugify(title);
    if (base.size() > 40) base.resize(40);
    while (!base.empty() && base.back() == '_') base.pop_back();
    if (base.empty()) base = "game";
    if (std::isdigit(static_cast<unsigned char>(base.front()))) base = "g_" + base;
    std::string candidate = base;
    for (int n = 2; taken.count(candidate); ++n) candidate = base + "_" + std::to_string(n);
    return candidate;
}

bool run_export(const ExportRequest& req, const Settings& s, const ExportCallbacks& cb,
                std::vector<std::string>& outputs, std::string& error) {
    Runner runner(cb);
    const ConsoleBackend* backend = backend_for_family(req.family_id);
    if (!backend || !backend->supported()) {
        error = "no supported backend for '" + req.family_id + "'";
        return false;
    }
    if (req.targets.empty()) {
        error = "no export targets selected";
        return false;
    }
    for (auto t : req.targets) {
        std::string problem = backend->export_problem(export_target_info(t).id);
        if (problem.empty()) problem = export_target_problem(t, s);
        if (!problem.empty()) {
            error = std::string(export_target_info(t).label) + ": " + problem;
            return false;
        }
    }
    if (!platform::ensure_directory(req.dest_dir, error)) return false;
    if (platform::is_directory(req.work_dir) && !platform::remove_tree(req.work_dir, error)) return false;
    if (!platform::ensure_directory(req.work_dir, error)) return false;

    // One recompile; every target compiles the same portable project.
    const std::string src = platform::join(req.work_dir, "src");
    const auto generate = backend->generate_steps(s, req.rom_path, req.game_id, src, platform::join(req.work_dir, "profile"));
    if (generate.empty()) {
        /*
         * Nothing to generate is not the same as nothing configured. A family
         * that runs the game's own executable has no recompiler to invoke and
         * no portable project to produce -- its runtime is built from its own
         * source, per target. Asking such a family for a generate step and
         * refusing when none comes back turned every one of its exports into
         * "the recompiler is not set", which is both wrong and unfixable from
         * Settings. What it can be asked is whether it is able to build at
         * all, which is what it answers here.
         */
        const auto problems = backend->toolchain_problems(s);
        if (!problems.empty()) {
            error = problems.front();
            return false;
        }
    } else if (!platform::exists(generate.front().argv.front())) {
        error = "the recompiler for " + backend->family_name() + " is not set (see Settings)";
        return false;
    }
    for (const auto& gen : generate) {
        runner.step(gen.label + " (" + platform::file_name(req.rom_path) + ")");
        if (!runner.run(gen.argv, req.work_dir, error, true)) {
            error = gen.label + " failed: " + error;
            return false;
        }
    }

    for (auto target : req.targets) {
        if (runner.cancelled()) {
            error = "cancelled";
            return false;
        }
        bool ok = false;
        switch (target) {
            case ExportTarget::Windows: ok = export_windows(runner, req, s, *backend, src, outputs, error); break;
            case ExportTarget::Linux: ok = export_linux(runner, req, s, *backend, outputs, error); break;
            case ExportTarget::MacKit: ok = export_mac_kit(runner, req, *backend, src, outputs, error); break;
            default:
                if (const ExtraExportTarget* extra = extra_target(target)) {
                    ExportTools tools;
                    tools.step = [&](const std::string& text) { runner.step(text); };
                    tools.run = [&](const std::vector<std::string>& argv, const std::string& cwd, std::string& why) {
                        return runner.run(argv, cwd, why);
                    };
                    tools.docker_ready = [&](std::string& why) { return docker_ready(runner, s, why); };
                    tools.publish = [&](const std::string& file, std::vector<std::string>& written, std::string& why) {
                        return publish(runner, file, req.dest_dir, written, why);
                    };
                    ok = extra->run && extra->run(req, s, *backend, tools, outputs, error);
                    if (!ok && error.empty()) error = std::string(export_target_info(target).label) + " export failed";
                }
                break;
        }
        if (!ok) return false;
    }

    std::string rm_err;
    if (!platform::remove_tree(req.work_dir, rm_err)) runner.log("[export] warning: " + rm_err);
    return true;
}

}  // namespace hub
