// Families from outside the repository.
//
// A hub can be built with console families whose code is kept somewhere else:
// a folder that holds a hub_extra.cmake, named by NATIVE_CONSOLE_HUB_EXTRA_DIR
// (hub/extra when that is not set). The file adds the folder's sources to the
// hub, and one of them defines register_extra_families(), which hands over
// the consoles, their backends and any export target of their own. A hub
// built without such a folder links hub_extra_none.cpp, which hands over
// nothing, and is the hub this repository describes.
//
// Nothing else in the hub knows these families: what they need from it is
// what ConsoleBackend already offers every family, and the tools below.
#pragma once

#include "hub_consoles.h"
#include "hub_export.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hub {

// What an export target from outside works with: the hub's own way of running
// a command, of starting Docker and of putting a finished file where the
// export was asked to go.
struct ExportTools {
    std::function<void(const std::string& text)> step;
    // Runs argv in cwd, streaming its output to the job's log.
    std::function<bool(const std::vector<std::string>& argv, const std::string& cwd, std::string& error)> run;
    std::function<bool(std::string& error)> docker_ready;
    // Moves `file` into the request's destination folder and adds it to `outputs`.
    std::function<bool(const std::string& file, std::vector<std::string>& outputs, std::string& error)> publish;
};

struct ExtraExportTarget {
    std::string id;           // settings / command line, as ExportTargetInfo::id
    std::string label;        // UI
    std::string description;  // UI, one line
    std::string file_suffix;  // the file written is <game id> + this
    // Empty when this computer can export the target; otherwise what is missing.
    std::function<std::string(const Settings& settings)> problem;
    std::function<bool(const ExportRequest& request, const Settings& settings, const ConsoleBackend& backend,
                       const ExportTools& tools, std::vector<std::string>& outputs, std::string& error)> run;
};

struct ExtraFamilies {
    std::vector<ConsoleInfo> consoles;
    std::vector<std::unique_ptr<ConsoleBackend>> backends;
    std::vector<ExtraExportTarget> export_targets;
};

// Defined once per hub: by the folder of families it was built with, or by
// hub_extra_none.cpp.
void register_extra_families(ExtraFamilies& families);

// What was registered. The backends are moved into backends() the first time
// that is asked for, and are null here afterwards.
ExtraFamilies& extra_families();

}  // namespace hub
