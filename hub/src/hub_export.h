// "Compile for export": builds games to run on other computers.
//
// A ROM is recompiled once into a portable C project, which is then compiled
// for each selected target and packed into one transferable file per target:
//   windows  <id>-windows-x64.zip       MSVC on this computer (Windows hosts)
//   linux    <id>-linux-x86_64.tar.gz   Valve's Steam Runtime SDK in Docker; runs on SteamOS
//   macos    <id>-macos-kit.tar.gz      the project plus build.command, which makes
//                                        <id>.app on any Mac (Apple's compiler only runs there)
//
// A hub built with families from outside the repository (hub_extra.h) may have
// more targets, which those families bring with them.
#pragma once

#include "hub_settings.h"

#include <atomic>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace hub {

// FirstExtra and the values after it are the targets of families from outside,
// in the order they were registered; nothing is called FirstExtra itself.
enum class ExportTarget { Windows, Linux, MacKit, FirstExtra };

struct ExportTargetInfo {
    ExportTarget target;
    const char* id;           // settings / command line: "windows", "linux", "macos"
    const char* label;        // UI
    const char* description;  // UI, one line
};

const std::vector<ExportTargetInfo>& export_target_infos();
const ExportTargetInfo& export_target_info(ExportTarget target);
// Parses "windows,linux,macos"; unknown names are reported in `unknown`.
std::vector<ExportTarget> parse_export_targets(const std::string& csv, std::vector<std::string>* unknown = nullptr);
std::string format_export_targets(const std::vector<ExportTarget>& targets);
// File written to the destination folder for this game and target.
std::string export_file_name(const std::string& game_id, ExportTarget target);

// Empty when this computer can export the target; otherwise what is missing.
// Cheap checks only (files and settings); Docker's state is checked by the job.
std::string export_target_problem(ExportTarget target, const Settings& settings);

// An id for an exported game: slug of the title, unique among `taken`.
std::string make_export_id(const std::string& title, const std::set<std::string>& taken);

struct ExportRequest {
    std::string game_id;
    std::string title;
    std::string family_id;
    std::string rom_path;
    std::vector<ExportTarget> targets;
    std::string dest_dir;
    std::string work_dir;  // scratch folder, wiped before and after
};

struct ExportCallbacks {
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> step;
    std::atomic<bool>* cancel = nullptr;
};

// Runs one export on the calling thread. Returns the files written to dest_dir.
bool run_export(const ExportRequest& request, const Settings& settings, const ExportCallbacks& callbacks,
                std::vector<std::string>& outputs, std::string& error);

}  // namespace hub
