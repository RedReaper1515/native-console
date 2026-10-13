// Native Console Hub: library, import/build pipeline, and launcher front end.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include "hub_consoles.h"
#include "hub_edited_data.h"
#include "hub_export.h"
#include "hub_library.h"
#include "hub_pipeline.h"
#include "hub_platform.h"
#include "hub_settings.h"
#include "hub_json.h"
#include "hub_steam.h"
#include "hub_texture_packs.h"
#include "hub_voxel_profiles.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <SDL_syswm.h>
#endif

#ifndef HUB_VERSION
#define HUB_VERSION "0.1.2"
#endif

namespace hub {

namespace {

// ---------------------------------------------------------------------------
// Small ImGui helpers
// ---------------------------------------------------------------------------

int input_text_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}

bool input_string(const char* label, std::string& value, ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputText(label, value.data(), value.capacity() + 1, flags, input_text_callback, &value);
}

bool input_string_multiline(const char* label, std::string& value, const ImVec2& size, ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextMultiline(label, value.data(), value.capacity() + 1, size, flags, input_text_callback, &value);
}

std::string ellipsize(const std::string& text, float max_width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) return text;
    std::string out = text;
    while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > max_width) out.pop_back();
    return out + "...";
}

void draw_star(ImDrawList* dl, ImVec2 center, float radius, ImU32 color, bool filled) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        float r = (i % 2 == 0) ? radius : radius * 0.45f;
        float a = -3.14159265f / 2.0f + static_cast<float>(i) * 3.14159265f / 5.0f;
        pts[i] = ImVec2(center.x + std::cos(a) * r, center.y + std::sin(a) * r);
    }
    if (filled) dl->AddConvexPolyFilled(pts, 10, color);  // slightly concave; acceptable for a 10-point star at this size
    dl->AddPolyline(pts, 10, color, ImDrawFlags_Closed, 1.5f);
}

std::vector<std::string> split_args(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : text) {
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && (c == ' ' || c == '\t')) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

const char* job_state_name(JobState s) {
    switch (s) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Succeeded: return "succeeded";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "?";
}

const char* job_kind_name(JobKind k) {
    switch (k) {
        case JobKind::Build: return "Build";
        case JobKind::Export: return "Export";
        case JobKind::Profile: return "Profile";
        case JobKind::DataTree: return "Data tree";
        case JobKind::CaptureCover: break;
    }
    return "Capture cover";
}

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------

struct Toast {
    std::string text;
    bool error = false;
    double expires_at = 0.0;
};

struct Theme {
    ImU32 card = IM_COL32(35, 39, 48, 255);
    ImU32 card_hover = IM_COL32(46, 51, 62, 255);
    ImU32 card_selected = IM_COL32(58, 82, 130, 255);
    ImU32 text = IM_COL32(230, 232, 236, 255);
    ImU32 text_dim = IM_COL32(150, 156, 168, 255);
    ImU32 accent = IM_COL32(79, 140, 255, 255);
    ImU32 ok = IM_COL32(61, 220, 151, 255);
    ImU32 warn = IM_COL32(255, 190, 70, 255);
    ImU32 error = IM_COL32(255, 96, 96, 255);
    ImU32 star = IM_COL32(255, 205, 60, 255);
};

ImU32 console_color(const std::string& console_id) {
    if (console_id == "gb") return IM_COL32(139, 172, 15, 255);
    if (console_id == "gbc") return IM_COL32(182, 76, 255, 255);
    if (console_id == "gba") return IM_COL32(91, 110, 225, 255);
    if (console_id == "snes") return IM_COL32(206, 70, 70, 255);
    if (console_id == "nes") return IM_COL32(196, 52, 52, 255);
    if (console_id == "nds") return IM_COL32(78, 154, 182, 255);
    if (console_id == "n64") return IM_COL32(46, 139, 87, 255);
    if (console_id == "gc") return IM_COL32(106, 90, 205, 255);
    if (console_id == "wii") return IM_COL32(52, 190, 237, 255);
    if (const ConsoleInfo* info = console_info(console_id); info && info->rgb) {
        return IM_COL32((info->rgb >> 16) & 255, (info->rgb >> 8) & 255, info->rgb & 255, 255);
    }
    return IM_COL32(120, 120, 120, 255);
}

ImU32 darken(ImU32 c, float f) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x * f, v.y * f, v.z * f, v.w));
}

class App {
public:
    enum class View { Library, Favorites, Jobs, Settings };

    struct RunningGame {
        platform::Process process;
        long long started_at = 0;
        // Started with edited data: the log the game says in if it refuses
        // the image or the pack. Empty for every other start.
        std::string edited_log;
    };

    // How launch() treats the edited data a Game Boy game may have chosen.
    enum class EditedData {
        Build,     // make it ready first, then start with it; nothing chosen, start at once as always
        Ready,     // `extra` already ends with its arguments (poll_prepared)
        Original,  // start with the game's own data whatever is chosen
    };

    // One window of a start that waits for its edited data: launch()'s extra and slot.
    struct Window {
        std::vector<std::string> extra;
        std::string slot;
    };

    // A start waiting while the data tool builds a tree, off the UI thread:
    // a tree is thousands of files, and reading them cold takes seconds.
    struct PreparedStart {
        std::future<edited_data::Start> result;
        std::vector<Window> windows;
    };

    // Something the user has to read before going on: why a game did not start.
    struct Notice {
        std::string title;
        std::string text;
    };

    // Installed size, refreshed occasionally rather than every frame.
    struct SizeInfo {
        long long logical = -1;
        long long on_disk = -1;
        double measured_at = -1.0;
    };

    // An "Add to Steam" or "Remove from Steam" request waiting in its dialog.
    struct SteamPending {
        std::vector<std::string> ids;  // with remove and no ids: every hub shortcut
        std::string label;
        int skipped = 0;
        bool remove = false;
        std::string root, account, account_label, error, note;
        bool steam_running = false;
        bool waiting_for_exit = false;
        double next_check = 0.0;
        double deadline = 0.0;
        bool picking = false;                 // the user chooses the games in the dialog
        std::map<std::string, bool> checked;  // chooser selection by game id
        std::set<std::string> in_steam;       // hub games already in this Steam account
    };

    App(Settings settings, Library library) : settings_(std::move(settings)), library_(std::move(library)) {
        grid_mode_ = settings_.default_view != "list";
        settings_edit_ = settings_;
    }

    // Imports files or folders; used by the UI, drag-and-drop, and the command line.
    void import_paths(const std::vector<std::string>& paths) {
        int queued = 0, skipped = 0;
        for (const auto& raw : paths) {
            std::string path = platform::absolute(raw);
            if (platform::is_directory(path)) {
                for (const auto& file : roms_in_folder(path)) {
                    if (import_rom(file)) ++queued; else ++skipped;
                }
            } else if (import_rom(path)) {
                ++queued;
            } else {
                ++skipped;
            }
        }
        if (queued || skipped) {
            toast("Imported " + std::to_string(queued) + " game(s)" + (skipped ? ", skipped " + std::to_string(skipped) : ""), skipped > 0 && queued == 0);
        }
        save_library();
    }

    bool import_rom(const std::string& path) {
        const ConsoleBackend* backend = backend_for_file(path);
        if (!backend) {
            log_line("skip (unknown extension): " + path);
            return false;
        }
        RomInfo info;
        std::string err;
        if (!backend->probe(path, info, err)) {
            log_line("skip (" + err + "): " + path);
            return false;
        }
        std::string sha = sha256_hex_of_file(path);
        if (const GameEntry* existing = library_.find_by_hash(sha)) {
            log_line("skip (already in library as '" + existing->title + "'): " + path);
            return false;
        }

        GameEntry e;
        e.id = library_.make_unique_id(info.title.empty() ? info.header_title : info.title);
        e.title = info.title.empty() ? (info.header_title.empty() ? e.id : info.header_title) : info.title;
        e.console_id = info.console_id;
        e.family_id = backend->family_id();
        e.notes = info.notes;
        e.header_title = info.header_title;
        e.rom_original_path = path;
        e.rom_sha256 = sha;
        e.rom_size = info.size;
        e.added_at = platform::now_iso8601();
        e.game_dir = platform::join(settings_.games_dir, e.id);
        e.rom_path = platform::join(platform::join(e.game_dir, "rom"), platform::file_name(path));
        if (!platform::copy_file(path, e.rom_path, err)) {
            toast(err, true);
            return false;
        }
        if (backend->supported()) {
            e.status = "queued";
        } else {
            e.status = "unsupported";
            e.last_error = backend->unsupported_reason();
        }
        library_.games().push_back(e);
        if (backend->supported()) queue_build(library_.games().back());
        log_line("imported " + e.title + " [" + e.console_id + "] as " + e.id);
        return true;
    }

    void queue_build(GameEntry& e) {
        e.status = "queued";
        e.last_error.clear();
        jobs_.enqueue_build(e.id, e.title, e.family_id, e.game_dir, e.rom_path, e.rom_original_path, settings_);
    }

    void queue_cover_capture(GameEntry& e) {
        if (e.exe_path.empty() || !platform::exists(e.exe_path)) {
            toast("Build the game before capturing a cover", true);
            return;
        }
        jobs_.enqueue_cover_capture(e.id, e.title, e.family_id, e.game_dir, e.exe_path, settings_.covers_dir,
                                    settings_.cover_capture_frame, settings_);
        toast("Capturing cover for " + e.title);
    }

    // extra: arguments for this launch only (the link cable's player and
    // host/join options). slot: a second window of the same game is tracked
    // as "<id>#<slot>" so both can run.
    void launch(GameEntry& e, const std::vector<std::string>& extra = {}, const std::string& slot = "",
                EditedData edited = EditedData::Build) {
        if (e.status != "ready" || e.exe_path.empty()) {
            toast("This game is not built yet", true);
            return;
        }
        if (!platform::exists(e.exe_path)) {
            e.status = "failed";
            e.last_error = "executable is missing; rebuild the game";
            save_library();
            toast(e.last_error, true);
            return;
        }
        const std::string key = slot.empty() ? e.id : e.id + "#" + slot;
        auto it = running_.find(key);
        if (it != running_.end() && platform::process_running(it->second.process)) {
            toast(e.title + " is already running");
            return;
        }
        if (edited == EditedData::Build && uses_edited_data(e)) {
            start_with_edited_data(e, {{extra, slot}});
            return;
        }
        std::vector<std::string> args = launch_arguments(e);
        args.insert(args.end(), extra.begin(), extra.end());
        platform::Process proc;
        std::string err;
        if (!platform::launch_detached(e.exe_path, args, platform::parent_path(e.exe_path), settings_.hide_game_console, proc, err)) {
            toast("Launch failed: " + err, true);
            return;
        }
        if (it != running_.end()) platform::process_release(it->second.process);
        running_[key] = RunningGame{proc, platform::now_unix(),
                                    edited == EditedData::Ready ? edited_data::log_file_of(args) : std::string()};
        if (!slot.empty()) return;  // play time and count follow player 1's window
        e.last_played = platform::now_unix();
        e.play_count += 1;
        save_library();
    }

    // Two windows of a GB/GBC/GBA game on this computer, joined by the link
    // cable: player 1 hosts for this computer only; player 2 keeps its saves
    // and settings apart (the runtimes' --player profiles).
    void launch_two_players(GameEntry& e) {
        if (is_running(e.id) || is_running(e.id + "#p2")) {
            toast(e.title + " is already running; close it first", true);
            return;
        }
        const ConsoleBackend* backend = backend_for_family(e.family_id);
        std::vector<std::string> p1 = {"--player", "1", "--link-host", "--link-local"};
        std::vector<std::string> p2 = {"--player", "2", "--link-join", "127.0.0.1"};
        if (backend) {
            for (const auto& a : backend->two_player_args(e.game_dir, 1)) p1.push_back(a);
            for (const auto& a : backend->two_player_args(e.game_dir, 2)) p2.push_back(a);
        }
        if (uses_edited_data(e)) {
            // Made ready once, and both windows start with it.
            start_with_edited_data(e, {{p1, ""}, {p2, "p2"}});
            return;
        }
        launch(e, p1);
        if (!is_running(e.id)) return;
        launch(e, p2, "p2");
    }

    // ------------------------------------------------------------------
    // Edited data (Game Boy): a data tree or a built image the game starts with
    // ------------------------------------------------------------------

    static bool uses_edited_data(const GameEntry& e) {
        return e.family_id == "gb" && !e.edited_data.empty();
    }

    static edited_data::Game edited_game(const GameEntry& e) {
        return {e.game_dir, e.exe_path, e.rom_sha256};
    }

    // Python and tools/gb_data.py: as set, else as found. Looked for once,
    // and again after the settings are saved.
    const edited_data::Tool& data_tool() {
        if (!data_tool_) {
            edited_data::Tool tool;
            tool.python = settings_.python_exe;
#ifdef _WIN32
            // "python3" last: on Windows it is often the stub that only offers the Store.
            const char* names[] = {"python", "py", "python3"};
#else
            const char* names[] = {"python3", "python"};
#endif
            for (const char* name : names) {
                if (tool.python.empty()) tool.python = platform::find_on_path(name);
            }
            tool.script = settings_.gb_data_tool.empty() ? edited_data::find_script(platform::executable_dir())
                                                         : settings_.gb_data_tool;
            data_tool_ = tool;
        }
        return *data_tool_;
    }

    // Runs the data tool for edited_data::prepare and collects what it prints.
    static int run_data_tool(const std::vector<std::string>& argv, std::vector<std::string>& output) {
        std::string command;
        for (const auto& a : argv) command += (command.empty() ? "" : " ") + platform::quote_arg(a);
        const platform::RunResult result =
            platform::run_capture(command, "", [&](const std::string& line) { output.push_back(line); }, nullptr);
        if (!result.started) {
            output.push_back(result.error);
            return -1;
        }
        return result.exit_code;
    }

    // Starts the game once its edited data is ready: the tool builds a tree
    // first, an image is taken as it is. poll_prepared() opens the windows.
    void start_with_edited_data(GameEntry& e, std::vector<Window> windows) {
        if (preparing_.count(e.id)) {
            toast(e.title + " is starting: its edited data is being built");
            return;
        }
        PreparedStart pending;
        pending.windows = std::move(windows);
        pending.result = std::async(std::launch::async, edited_data::prepare, e.edited_data, edited_game(e), data_tool(),
                                    edited_data::Runner(run_data_tool));
        preparing_.emplace(e.id, std::move(pending));
    }

    void poll_prepared() {
        for (auto it = preparing_.begin(); it != preparing_.end();) {
            if (it->second.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            const edited_data::Start start = it->second.result.get();
            const std::vector<Window> windows = std::move(it->second.windows);
            GameEntry* e = library_.find(it->first);
            it = preparing_.erase(it);
            if (!e) continue;
            for (const auto& line : start.output) log_line("[edited data] " + line);
            if (!start.error.empty()) {
                notify(e->title + " was not started", start.error + "\n\nEdited data: " + e->edited_data);
                continue;
            }
            if (!start.summary.empty()) {
                edited_summary_[e->id] = start.summary;
                toast(e->title + ": " + start.summary);
            }
            for (const Window& window : windows) {
                std::vector<std::string> extra = window.extra;
                extra.insert(extra.end(), start.args.begin(), start.args.end());
                launch(*e, extra, window.slot, EditedData::Ready);
                if (!is_running(e->id)) break;  // the first window did not open
            }
        }
    }

    // A game that has ended: if it was started with edited data and its log
    // says it refused the image or the pack, the user is told why. The game
    // says so and exits before it opens a window, so nothing else would show.
    void report_refusal(const std::string& key, const RunningGame& run) {
        if (run.edited_log.empty()) return;
        const std::string refused = edited_data::refusal_in_file(run.edited_log);
        if (refused.empty()) return;
        const GameEntry* e = library_.find(key.substr(0, key.find('#')));
        notify((e ? e->title : key) + " did not start", refused + (e ? "\n\nEdited data: " + e->edited_data : ""));
    }

    void notify(const std::string& title, const std::string& text) {
        log_line("[error] " + title + ": " + text);
        notice_ = Notice{title, text};
    }

    void set_edited_data(GameEntry& g, const std::string& path) {
        g.edited_data = path;
        save_library();
        edited_summary_.erase(g.id);
        // What the hub built of a tree goes when the tree is no longer the choice.
        if (edited_data::describe(path, g.rom_sha256).kind != edited_data::Kind::Tree) {
            std::string err;
            if (!edited_data::remove_build(g.game_dir, err)) toast(err, true);
        }
        refresh_edited_data(g);
    }

    // A job that takes the game's ROM apart into a data tree, in
    // <data dir>/data-trees/<id>; poll_jobs chooses the tree for the game.
    void queue_data_tree(GameEntry& g) {
        jobs_.enqueue_data_tree(g.id, g.title, g.family_id, g.game_dir, g.rom_path,
                                edited_data::tree_dir(settings_.data_dir, g.id), data_tool(), settings_);
    }

    void refresh_edited_data(const GameEntry& g) {
        edited_choice_ = g.family_id == "gb" ? edited_data::describe(g.edited_data, g.rom_sha256) : edited_data::Choice{};
        edited_choice_game_ = g.id;
        edited_checked_ = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    }

    // Scanning the executable for the flag is slow, so cache it per file and size.
    bool edited_data_supported(const GameEntry& g) {
        const long long size = platform::file_size(g.exe_path);
        auto it = edited_data_support_.find(g.exe_path);
        if (it == edited_data_support_.end() || it->second.first != size) {
            bool supported = size > 0 && edited_data::executable_supports(g.exe_path, "--rom-image");
            it = edited_data_support_.insert_or_assign(g.exe_path, std::make_pair(size, supported)).first;
        }
        return it->second.second;
    }

    void draw_edited_data_section(GameEntry& g) {
        if (g.family_id != "gb") return;
        // Looked at again now and then: a folder can be moved, and a tree put right.
        if (edited_choice_game_ != g.id || ImGui::GetTime() - edited_checked_ > 2.0) refresh_edited_data(g);

        ImGui::TextDisabled("Edited data");
        ImGui::SetNextItemWidth(-1);
        input_string("##editeddata", edit_edited_);
        if (ImGui::IsItemDeactivatedAfterEdit() && edit_edited_ != g.edited_data) set_edited_data(g, edit_edited_);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("A data tree folder (tools/gb_data.py split) with your edits in it, or an image built from one.\n"
                              "The game starts with it. Empty: the game starts with its own data.");
        }
        if (edit_edited_.empty() && !ImGui::IsItemActive()) {
            ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + ImGui::GetStyle().FramePadding.x, min.y + ImGui::GetStyle().FramePadding.y),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), "None: the game's own data");
        }

        const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        if (ImGui::Button("Data tree...", ImVec2(third, 0))) {
            std::string dir;
            if (platform::pick_folder(dir)) {
                edit_edited_ = dir;
                set_edited_data(g, dir);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Built image...", ImVec2(third, 0))) {
            std::vector<std::string> paths;
            if (platform::pick_files({{"ROM images", "*.gb;*.gbc"}, {"All files", "*.*"}}, false, paths) && !paths.empty()) {
                edit_edited_ = paths.front();
                set_edited_data(g, paths.front());
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(g.edited_data.empty());
        if (ImGui::Button("Clear", ImVec2(third, 0))) {
            edit_edited_.clear();
            set_edited_data(g, "");
            toast(g.title + " starts with its own data again");
        }
        ImGui::EndDisabled();

        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::BeginDisabled(jobs_.has_pending_for(g.id));
        if (ImGui::Button("Make data tree", ImVec2(half, 0))) {
            queue_data_tree(g);
            toast("Taking " + g.title + " apart into files");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Takes the game's ROM apart into files to edit (DATA_FILES.md), in\n%s,\n"
                              "with the options this hub builds games with, and chooses the tree for this game.\n"
                              "A tree that is already there is left as it is.",
                              edited_data::tree_dir(settings_.data_dir, g.id).c_str());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(edited_choice_.kind != edited_data::Kind::Tree && edited_choice_.kind != edited_data::Kind::Image);
        if (ImGui::Button("Open folder##editeddata", ImVec2(half, 0))) {
            platform::open_in_file_manager(edited_choice_.kind == edited_data::Kind::Tree ? g.edited_data
                                                                                          : platform::parent_path(g.edited_data));
        }
        ImGui::EndDisabled();

        if (!g.edited_data.empty()) {
            const ImVec4 warn = ImGui::ColorConvertU32ToFloat4(theme_.warn);
            ImGui::PushFont(font_small_);
            ImGui::PushTextWrapPos();
            if (edited_choice_.kind == edited_data::Kind::Unusable) {
                ImGui::TextColored(warn, "Cannot be used: %s. The game will not start until this is put right or cleared.",
                                   edited_choice_.error.c_str());
            } else if (g.status == "ready" && !edited_data_supported(g)) {
                ImGui::TextColored(warn, "This build predates edited data. Rebuild the game to use it.");
            } else if (edited_choice_.kind == edited_data::Kind::Tree) {
                const edited_data::Tool& tool = data_tool();
                if (tool.python.empty() || tool.script.empty()) {
                    ImGui::TextColored(warn, "%s was not found, and a data tree is built with it. Set it in Settings, or choose a built image.",
                                       tool.python.empty() ? "Python" : "tools/gb_data.py");
                } else {
                    ImGui::TextDisabled("A data tree: built into an image each time the game starts.");
                }
                auto said = edited_summary_.find(g.id);
                if (said != edited_summary_.end()) ImGui::TextDisabled("Last start: %s.", said->second.c_str());
            } else if (edited_choice_.kind == edited_data::Kind::Image) {
                ImGui::TextDisabled("%s", edited_choice_.pack.empty() ? "A built image." : "A built image, with its pack of unpacked data.");
            }
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        ImGui::Dummy(ImVec2(0, 4 * scale_));
    }

    std::vector<std::string> launch_arguments(const GameEntry& e) const {
        std::vector<std::string> args;
        if (const ConsoleBackend* backend = backend_for_family(e.family_id)) {
            args = backend->launch_args(e.game_dir);
        }
        for (const auto& a : split_args(settings_.gb_extra_launch_args)) args.push_back(a);
        if (e.family_id == "gba") {
            for (const auto& a : texture_packs::launch_args(e.game_dir, e.texture_pack, e.rom_sha256, "gba")) args.push_back(a);
        }
        if (e.family_id == "gb") {
            for (const auto& a : texture_packs::launch_args(e.game_dir, e.texture_pack, e.rom_sha256)) args.push_back(a);
            voxel_profiles::update_builtin_copies(e.game_dir);
            for (const auto& a : voxel_profiles::launch_args(e.game_dir, e.voxel_profile, e.rom_sha256)) args.push_back(a);
        }
        if ((e.family_id == "gc" || e.family_id == "wii") && !settings_.gc_renderer.empty()) {
            // For this run only; the game's own menu setting stays as it was.
            args.push_back("--renderer");
            args.push_back(settings_.gc_renderer);
        }
        if ((e.family_id == "gc" || e.family_id == "wii") && settings_.gc_internal_scale > 0) {
            // Likewise for this run only. It means something to the fast
            // renderer alone; the others draw at the console's size.
            args.push_back("--internal-scale");
            args.push_back(std::to_string(settings_.gc_internal_scale));
        }
        for (const auto& a : split_args(e.launch_args)) args.push_back(a);
        return args;
    }

    // ------------------------------------------------------------------
    // Steam shortcuts (--launch) and their play time
    // ------------------------------------------------------------------

    // Runs one game for a Steam shortcut: no hub window, the game's console
    // hidden, and the hub waits so Steam shows the game as running. Play time is
    // journaled rather than saved to library.json, because a GUI hub that is open
    // at the same time would overwrite it; ingest_play_sessions() applies it.
    // quiet (--batch): what goes wrong is printed and not put in a box, which
    // would wait for somebody to click it.
    int run_launch(const std::string& id, bool quiet = false) {
        auto fail = [&](const std::string& message) {
            if (quiet) {
                std::fprintf(stderr, "%s\n", message.c_str());
                return 1;
            }
            log_line("[launch] " + message);
            platform::show_message_box("Native Console Hub", message);
            return 1;
        };
        const GameEntry* e = library_.find(id);
        if (!e) {
            return fail("\"" + id + "\" is no longer in the Native Console library. Remove its Steam shortcut, or add the game to Steam again from the hub.");
        }
        if (e->status != "ready" || e->exe_path.empty() || !platform::exists(e->exe_path)) {
            return fail(e->title + " is not built. Build it in the Native Console Hub, then start it from Steam again.");
        }
        std::vector<std::string> args = launch_arguments(*e);
        std::string edited_log;
        if (uses_edited_data(*e)) {
            const edited_data::Start start = edited_data::prepare(e->edited_data, edited_game(*e), data_tool(), run_data_tool);
            for (const auto& line : start.output) log_line("[launch] " + line);
            if (!start.error.empty()) {
                return fail(e->title + " was not started.\n\n" + start.error + "\n\nEdited data: " + e->edited_data);
            }
            args.insert(args.end(), start.args.begin(), start.args.end());
            edited_log = edited_data::log_file_of(args);
        }
        platform::Process process;
        std::string err;
        if (!platform::launch_detached(e->exe_path, args, platform::parent_path(e->exe_path),
                                       settings_.hide_game_console, process, err)) {
            return fail("Could not start " + e->title + ": " + err);
        }
        platform::tie_to_process_lifetime(process);
        log_line("[launch] started " + e->title);
        const long long started = platform::now_unix();
        long long credited_until = started;
        bool new_session = true;
        while (platform::process_running(process)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const long long now = platform::now_unix();
            if (now - credited_until >= 60) {  // checkpoint so a killed launcher loses little
                append_play_session(id, started, now - credited_until, new_session);
                new_session = false;
                credited_until = now;
            }
        }
        append_play_session(id, started, std::max(0LL, platform::now_unix() - credited_until), new_session);
        platform::process_release(process);
        log_line("[launch] " + e->title + " exited");
        // A game that refuses its image or pack says so and exits before it opens a window.
        const std::string refused = edited_data::refusal_in_file(edited_log);
        if (!refused.empty()) return fail(e->title + " did not start.\n\n" + refused + "\n\nEdited data: " + e->edited_data);
        return 0;
    }

    std::string play_journal_path() const { return platform::join(settings_.data_dir, "play-sessions.jsonl"); }

    void append_play_session(const std::string& id, long long started, long long seconds, bool new_session) {
        Json line = Json::make_object();
        line.set("id", id);
        line.set("started", started);
        line.set("seconds", seconds);
        line.set("new_session", new_session);
        std::ofstream out(platform::to_path(play_journal_path()), std::ios::binary | std::ios::app);
        out << line.dump(0) << "\n";
    }

    // Applies play time journaled by Steam launches to the library.
    void ingest_play_sessions() {
        const std::string journal = play_journal_path();
        const std::string claimed = journal + ".ingest";
        std::error_code ec;
        if (!platform::exists(claimed)) {
            if (!platform::exists(journal)) return;
            // Renaming fails while a launcher is appending; the next call retries.
            std::filesystem::rename(platform::to_path(journal), platform::to_path(claimed), ec);
            if (ec) return;
        }
        std::string text;
        if (!read_text_file(claimed, text)) return;
        bool changed = false;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            Json j;
            std::string err;
            if (line.empty() || !Json::parse(line, j, err)) continue;
            GameEntry* e = library_.find(j.get_string("id"));
            if (!e) continue;
            e->play_seconds += std::max(0LL, j.get_int("seconds"));
            if (j.get_bool("new_session")) e->play_count += 1;
            e->last_played = std::max(e->last_played, j.get_int("started"));
            changed = true;
        }
        if (changed) save_library();
        std::filesystem::remove(platform::to_path(claimed), ec);
    }

    // ------------------------------------------------------------------
    // Add to Steam
    // ------------------------------------------------------------------

    std::vector<const GameEntry*> games_in_scope(bool favorites_only, const std::string& console_id) const {
        std::vector<const GameEntry*> out;
        for (const auto& g : library_.games()) {
            if (favorites_only && !g.favorite) continue;
            if (!console_id.empty() && g.console_id != console_id) continue;
            out.push_back(&g);
        }
        return out;
    }

    static bool steam_ready(const GameEntry& g) {
        return g.status == "ready" && !g.exe_path.empty() && platform::exists(g.exe_path);
    }

    std::string artwork_manifest_path() const { return platform::join(settings_.data_dir, "steam-artwork.json"); }

    bool resolve_steam(std::string& root, std::string& account, std::string& error) const {
        root = settings_.steam_root.empty() ? steam::detect_root() : settings_.steam_root;
        if (root.empty() || !platform::is_directory(root)) {
            error = root.empty() ? "Steam was not found on this computer. If it is installed, set its folder in Settings > Steam."
                                 : "The Steam folder " + root + " does not exist. Check Settings > Steam.";
            return false;
        }
        account = steam::pick_account(steam::list_accounts(root), settings_.steam_account);
        if (account.empty()) {
            error = "No Steam account was found in " + root + ". Sign in to Steam once, then try again.";
            return false;
        }
        return true;
    }

    bool perform_steam_export(const std::vector<std::string>& ids, const std::string& root, const std::string& account,
                              steam::ExportReport& report, std::string& error) {
        steam::ExportRequest request;
        request.root = root;
        request.account = account;
        request.hub_exe = platform::executable_path();
        request.data_dir = settings_.data_dir;
        request.artwork_manifest = artwork_manifest_path();
        for (const std::string& id : ids) {
            const GameEntry* g = library_.find(id);
            if (!g) continue;
            const ConsoleInfo* console = console_info(g->console_id);
            request.games.push_back({g->id, g->title, console ? console->name : "", g->cover_path});
        }
        return steam::export_games(request, report, error);
    }

    // Opens the Steam dialog for these games. Games that are not built are skipped.
    void request_steam_export(const std::vector<const GameEntry*>& games, const std::string& noun) {
        SteamPending p;
        for (const GameEntry* g : games) {
            if (steam_ready(*g)) {
                p.ids.push_back(g->id);
                if (p.label.empty()) p.label = g->title;
            } else {
                ++p.skipped;
            }
        }
        if (p.ids.empty()) {
            toast(games.empty() ? "There are no games to add to Steam here"
                                : "Build these games first; only built games can be added to Steam", true);
            return;
        }
        if (p.ids.size() > 1) p.label = std::to_string(p.ids.size()) + " " + noun;
        open_steam_dialog(std::move(p));
    }

    // The "Add to Steam" chooser: every game with a checkbox, plus one-click
    // selections for the whole library, favorites, and each console.
    void open_steam_picker(const std::vector<const GameEntry*>& preselect) {
        SteamPending p;
        p.picking = true;
        for (const GameEntry* g : preselect) {
            if (steam_ready(*g)) p.checked[g->id] = true;
        }
        if (resolve_steam(p.root, p.account, p.error)) {
            p.account_label = steam::account_label(steam::list_accounts(p.root), p.account);
            p.in_steam = steam::games_in_steam(p.root, p.account, platform::executable_path());
        }
        steam_pending_ = std::move(p);
    }

    void open_steam_dialog(SteamPending p) {
        if (resolve_steam(p.root, p.account, p.error)) {
            p.account_label = steam::account_label(steam::list_accounts(p.root), p.account);
        }
        steam_pending_ = std::move(p);
    }

    // True when the dialog is finished (the result is toasted). On failure the
    // error stays in the dialog.
    bool apply_steam(SteamPending& p) {
        std::string err;
        if (p.remove) {
            int removed = 0;
            steam::RemoveRequest request{p.root, p.account, platform::executable_path(), artwork_manifest_path(), p.ids};
            if (!steam::remove_games(request, removed, err)) {
                p.error = err;
                return false;
            }
            toast(removed ? "Removed " + std::to_string(removed) + " game(s) from Steam" : "Steam had no games from this hub");
            return true;
        }
        steam::ExportReport report;
        if (!perform_steam_export(p.ids, p.root, p.account, report, err)) {
            p.error = err;
            return false;
        }
        std::string message = "Steam: " + std::to_string(report.added) + " added, " + std::to_string(report.updated) +
                              " updated. They appear the next time Steam starts.";
        if (report.artwork_kept) message += " Kept " + std::to_string(report.artwork_kept) + " artwork file(s) you changed in Steam.";
        toast(message);
        for (const std::string& w : report.warnings) toast(w, true);
        return true;
    }

    // Command-line "Add to Steam" (--steam-add / --steam-remove).
    int run_steam_cli(const std::string& scope, bool remove, const std::string& root_override, const std::string& account_override) {
        if (!root_override.empty()) settings_.steam_root = root_override;
        if (!account_override.empty()) settings_.steam_account = account_override;
        std::string root, account, err;
        if (!resolve_steam(root, account, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        // Only the Steam install that is actually running can overwrite the file.
        auto normalized = [](const std::string& path) {
            std::string s = platform::absolute(path);
            std::replace(s.begin(), s.end(), '/', '\\');
            while (s.size() > 3 && s.back() == '\\') s.pop_back();
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        const std::string detected = steam::detect_root();
        if (!detected.empty() && normalized(detected) == normalized(root) && steam::steam_running()) {
            std::fprintf(stderr, "Steam is running. Exit Steam first: it rewrites its shortcuts when it exits.\n");
            return 3;
        }
        if (remove) {
            int removed = 0;
            steam::RemoveRequest request{root, account, platform::executable_path(), artwork_manifest_path(), {}};
            if (!steam::remove_games(request, removed, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            std::printf("[steam] account %s: removed %d shortcut(s)\n", account.c_str(), removed);
            return 0;
        }
        std::vector<const GameEntry*> games;
        if (scope == "all") {
            games = games_in_scope(false, "");
        } else if (scope == "favorites") {
            games = games_in_scope(true, "");
        } else if (scope.rfind("console:", 0) == 0) {
            games = games_in_scope(false, scope.substr(8));
        } else if (scope.rfind("game:", 0) == 0) {
            const GameEntry* g = library_.find(scope.substr(5));
            if (!g) {
                std::fprintf(stderr, "no game with id '%s' in the library\n", scope.substr(5).c_str());
                return 2;
            }
            games.push_back(g);
        } else {
            std::fprintf(stderr, "unknown --steam-add scope '%s' (use all, favorites, console:<id>, or game:<id>)\n", scope.c_str());
            return 2;
        }
        std::vector<std::string> ids;
        int skipped = 0;
        for (const GameEntry* g : games) {
            if (steam_ready(*g)) ids.push_back(g->id);
            else ++skipped;
        }
        steam::ExportReport report;
        if (!ids.empty() && !perform_steam_export(ids, root, account, report, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("[steam] account %s: %d added, %d updated, %d skipped (not built); artwork %d written, %d kept\n",
                    account.c_str(), report.added, report.updated, skipped, report.artwork_written, report.artwork_kept);
        for (const std::string& w : report.warnings) std::printf("[steam] warning: %s\n", w.c_str());
        return 0;
    }

    bool is_running(const std::string& id) {
        auto it = running_.find(id);
        return it != running_.end() && platform::process_running(it->second.process);
    }

    // Credits the elapsed session time to the game and resets the session clock.
    void credit_play_time(const std::string& id, RunningGame& run) {
        long long now = platform::now_unix();
        if (GameEntry* e = library_.find(id)) {
            if (now > run.started_at) e->play_seconds += now - run.started_at;
        }
        run.started_at = now;
    }

    // Total play time including the session in progress, for display.
    long long play_seconds_including_session(const GameEntry& e) {
        auto it = running_.find(e.id);
        if (it == running_.end() || it->second.started_at <= 0) return e.play_seconds;
        return e.play_seconds + std::max(0LL, platform::now_unix() - it->second.started_at);
    }

    const SizeInfo& size_for(const GameEntry& e) {
        SizeInfo& info = sizes_[e.id];
        double now = ImGui::GetTime();
        if (info.measured_at < 0.0 || now - info.measured_at > 5.0) {
            info.measured_at = now;
            if (!e.exe_path.empty() && platform::exists(e.exe_path)) {
                info.logical = platform::file_size(e.exe_path);
                info.on_disk = platform::file_size_on_disk(e.exe_path);
            } else {
                info.logical = info.on_disk = -1;
            }
        }
        return info;
    }

    // Applies finished job results to the library. Safe to call from the UI thread only.
    void poll_jobs() {
        for (const auto& job : jobs_.jobs()) {
            JobState state = job->state;
            if (job->consumed) continue;
            GameEntry* e = library_.find(job->game_id);
            if (state == JobState::Running && e && job->kind == JobKind::Build && e->status != "building") {
                e->status = "building";
                continue;
            }
            if (state == JobState::Queued || state == JobState::Running) continue;
            job->consumed = true;
            if (job->kind == JobKind::Export) {
                if (state == JobState::Succeeded) toast("Exported " + job->game_title + " to " + job->result_snapshot());
                else if (state == JobState::Failed) toast(job->game_title + ": export failed. " + job->error_snapshot(), true);
                continue;
            }
            if (!e) continue;
            if (job->kind == JobKind::Build) {
                if (state == JobState::Succeeded) {
                    e->status = "ready";
                    e->exe_path = job->result_snapshot();
                    e->last_error.clear();
                    toast(e->title + " is ready to play");
                    // Only for consoles that can capture one; a GameCube build
                    // would otherwise end with a failed job every time.
                    const ConsoleBackend* backend = backend_for_family(e->family_id);
                    if (e->cover_path.empty() && settings_.cover_capture_frame > 0 && backend &&
                        backend->supports_cover_capture()) {
                        queue_cover_capture(*e);
                    } else if (backend) {
                        queue_missing_profile(*e, *backend);
                    }
                } else if (state == JobState::Cancelled) {
                    // Asked for, so not an error: the game is as it was before.
                    const bool installed = !e->exe_path.empty() && platform::exists(e->exe_path);
                    e->status = installed ? "ready" : "failed";
                    e->last_error = installed ? "" : "The build was cancelled: choose Build to start it again.";
                    toast(e->title + (installed ? ": build cancelled, previous build kept" : ": build cancelled"));
                } else if (!e->exe_path.empty() && platform::exists(e->exe_path)) {
                    // The previous build is still installed and playable.
                    e->status = "ready";
                    e->last_error = "Last rebuild failed; the previous build is still installed. " + job->error_snapshot();
                    toast(e->title + ": rebuild failed, previous build kept", true);
                } else {
                    e->status = "failed";
                    e->last_error = job->error_snapshot();
                    toast(e->title + ": build failed", true);
                }
            } else if (job->kind == JobKind::DataTree) {
                if (state == JobState::Succeeded) {
                    // The tree just made is what the game starts with from now on.
                    e->edited_data = job->result_snapshot();
                    edited_summary_.erase(e->id);
                    if (selected_id_ == e->id) edit_edited_ = e->edited_data;
                    edited_choice_game_.clear();
                    toast(e->title + ": its data is in " + e->edited_data + ". Edit the files there, then press Play.");
                } else if (state == JobState::Failed) {
                    toast(e->title + ": no data tree was made. " + job->error_snapshot(), true);
                }
            } else {
                if (job->kind == JobKind::CaptureCover) {
                    if (state == JobState::Succeeded) {
                        e->cover_path = job->result_snapshot();
                        e->cover_version += 1;
                    } else if (state == JobState::Failed) {
                        toast(e->title + ": cover capture failed", true);
                    }
                } else if (state == JobState::Failed) {
                    toast(e->title + ": profiling failed. " + job->error_snapshot(), true);
                }
                // The job recorded a profile the installed build was made
                // without (GameCube: a trace): rebuild with it.
                if (state == JobState::Succeeded && job->profile_created) rebuild_with_profile_.insert(e->id);
            }
            save_library();
        }
        start_profile_rebuilds();
    }

    // After a build of a game that has its cover: the backend's profile runs
    // whose output is missing, once a session a game. They run against the
    // build just made, which already has the cover's trace - GameCube's play
    // run takes about a minute there and five on a build with no trace.
    void queue_missing_profile(GameEntry& e, const ConsoleBackend& backend) {
        if (profile_attempted_.count(e.id)) return;
        bool missing = false;
        for (const auto& run : backend.profile_runs(e.game_dir)) {
            if (!platform::exists(run.output)) missing = true;
        }
        if (!missing) return;
        profile_attempted_.insert(e.id);
        jobs_.enqueue_profile(e.id, e.title, e.family_id, e.game_dir, e.exe_path, settings_);
    }

    // Rebuilds a game whose cover capture or profile job recorded a new
    // profile, once it is ready and not running from the hub (installing over
    // a running executable fails). A build already pending for it reads the
    // profile anyway.
    void start_profile_rebuilds() {
        for (auto it = rebuild_with_profile_.begin(); it != rebuild_with_profile_.end();) {
            GameEntry* e = library_.find(*it);
            bool build_pending = false;
            for (const auto& j : jobs_.jobs()) {
                const JobState s = j->state;
                if (j->kind == JobKind::Build && j->game_id == *it && (s == JobState::Queued || s == JobState::Running)) {
                    build_pending = true;
                }
            }
            if (!e || build_pending || e->status != "ready") {
                it = rebuild_with_profile_.erase(it);
                continue;
            }
            // Another capture or profile still to run for it: wait, rebuild once.
            auto running = running_.find(*it);
            if (jobs_.has_pending_for(*it) ||
                (running != running_.end() && platform::process_running(running->second.process))) {
                ++it;
                continue;
            }
            log_line("[info] " + e->title + ": rebuilding with its new profile");
            queue_build(*e);
            save_library();
            it = rebuild_with_profile_.erase(it);
        }
    }

    void poll_running() {
        bool changed = false;
        for (auto it = running_.begin(); it != running_.end();) {
            if (!platform::process_running(it->second.process)) {
                credit_play_time(it->first, it->second);
                report_refusal(it->first, it->second);
                platform::process_release(it->second.process);
                it = running_.erase(it);
                changed = true;
            } else {
                // Checkpoint long sessions every minute so a hub crash loses little.
                if (platform::now_unix() - it->second.started_at >= 60) {
                    credit_play_time(it->first, it->second);
                    changed = true;
                }
                ++it;
            }
        }
        if (changed) save_library();
    }

    void save_library() {
        std::string err;
        if (!library_.save(err)) toast(err, true);
    }

    void toast(const std::string& text, bool error = false) {
        Toast t;
        t.text = text;
        t.error = error;
        t.expires_at = ImGui::GetCurrentContext() ? ImGui::GetTime() + (error ? 8.0 : 4.0) : 0.0;
        toasts_.push_back(t);
        log_line(std::string(error ? "[error] " : "[info] ") + text);
    }

    void log_line(const std::string& line) {
        std::fprintf(stdout, "%s\n", line.c_str());
        std::fflush(stdout);
    }

    // ------------------------------------------------------------------
    // GUI
    // ------------------------------------------------------------------

    std::string job_history_dir() const { return platform::join(settings_.data_dir, "job-history"); }

    int run_gui(const std::vector<std::string>& initial_imports) {
        // Finished jobs are kept from one run of the hub to the next, and the
        // Jobs view lists the ones earlier runs left (Clear history there).
        jobs_.set_history_dir(job_history_dir(), true);
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        float dpi = 96.0f;
        if (SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) != 0) dpi = 96.0f;
        scale_ = settings_.ui_scale > 0.0f ? settings_.ui_scale : std::max(1.0f, dpi / 96.0f);

        window_ = SDL_CreateWindow("Native Console Hub", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   static_cast<int>(1280 * scale_), static_cast<int>(800 * scale_),
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window_) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 1;
        }
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer_) {
            std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return 1;
        }
        SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
        apply_window_icon();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        ini_path_ = platform::join(settings_.data_dir, "imgui.ini");
        io.IniFilename = ini_path_.c_str();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        setup_fonts(io);
        setup_style();
        ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
        ImGui_ImplSDLRenderer2_Init(renderer_);

        if (!initial_imports.empty()) import_paths(initial_imports);
        ingest_play_sessions();
        auto problems = settings_.validate();
        if (!problems.empty()) {
            toast("Toolchain incomplete: " + problems.front() + ". Check Settings.", true);
        }

        bool quit = false;
        while (!quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT) quit = true;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window_)) quit = true;
                if (event.type == SDL_DROPFILE && event.drop.file) {
                    dropped_.push_back(event.drop.file);
                    SDL_free(event.drop.file);
                }
                if (event.type == SDL_DROPCOMPLETE && !dropped_.empty()) {
                    import_paths(dropped_);
                    dropped_.clear();
                }
            }
            if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(50);
                continue;
            }
            poll_jobs();
            poll_prepared();
            poll_running();
            if (SDL_GetTicks() - last_ingest_ticks_ > 3000) {
                last_ingest_ticks_ = SDL_GetTicks();
                ingest_play_sessions();
            }

            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            draw_frame();
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer_, 20, 22, 26, 255);
            SDL_RenderClear(renderer_);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
            SDL_RenderPresent(renderer_);
        }

        for (auto& kv : textures_) {
            if (kv.second) SDL_DestroyTexture(kv.second);
        }
        for (auto& kv : running_) credit_play_time(kv.first, kv.second);
        if (!running_.empty()) save_library();
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
        return 0;
    }

    // Command-line mode: import, build or export everything queued, and exit.
    int run_batch(const std::vector<std::string>& imports, bool rebuild_all, const std::vector<std::string>& rebuilds,
                  const std::vector<std::string>& exports, const std::vector<ExportTarget>& export_targets,
                  const std::string& export_dest, const std::vector<std::string>& data_trees = {}) {
        // Recorded for the Jobs view of the next hub window, not loaded here:
        // what this run reports and exits on is its own jobs.
        jobs_.set_history_dir(job_history_dir(), false);
        ingest_play_sessions();
        import_paths(imports);
        bool any_failed = false;
        // --rebuild names games by id; an unknown id is an error, not a no-op.
        for (const std::string& id : rebuilds) {
            bool found = false;
            for (auto& g : library_.games()) found = found || g.id == id;
            if (!found) {
                std::fprintf(stderr, "no game with id %s in the library\n", id.c_str());
                any_failed = true;
            }
        }
        for (auto& g : library_.games()) {
            const bool wanted = rebuild_all || std::find(rebuilds.begin(), rebuilds.end(), g.id) != rebuilds.end();
            if (wanted && backend_for_family(g.family_id) && backend_for_family(g.family_id)->supported()) queue_build(g);
        }
        // --data-tree names games by id too. After the builds: the tree is of
        // the game as the hub's settings build it now.
        for (const std::string& id : data_trees) {
            GameEntry* g = library_.find(id);
            if (g && g->family_id == "gb") {
                queue_data_tree(*g);
            } else {
                std::fprintf(stderr, g ? "%s is not a Game Boy game: it has no data tree\n" : "no game with id %s in the library\n",
                             id.c_str());
                any_failed = true;
            }
        }
        if (!exports.empty()) {
            for (auto t : export_targets) {
                std::string problem = export_target_problem(t, settings_);
                if (!problem.empty()) {
                    std::fprintf(stderr, "cannot export %s: %s\n", export_target_info(t).id, problem.c_str());
                    return 2;
                }
            }
            if (queue_exports(exports, export_targets, export_dest) == 0) {
                std::fprintf(stderr, "no supported ROMs to export\n");
                any_failed = true;
            }
        }
        std::map<int, size_t> printed;
        while (true) {
            for (const auto& job : jobs_.jobs()) {
                std::vector<std::string> lines = job->log_snapshot();
                size_t& done = printed[job->id];
                for (; done < lines.size(); ++done) {
                    std::printf("[job %d %s] %s\n", job->id, job->game_id.c_str(), lines[done].c_str());
                }
            }
            std::fflush(stdout);
            poll_jobs();
            if (!jobs_.busy()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        for (const auto& job : jobs_.jobs()) {
            JobState s = job->state;
            std::printf("[job %d %s] %s: %s %s\n", job->id, job->game_id.c_str(), job_kind_name(job->kind), job_state_name(s),
                        s == JobState::Succeeded ? job->result_snapshot().c_str() : job->error_snapshot().c_str());
            if (s != JobState::Succeeded) any_failed = true;
            if (job->kind == JobKind::Export) {
                std::lock_guard<std::mutex> lock(job->mutex);
                for (const auto& out : job->outputs) std::printf("[job %d %s]   %s\n", job->id, job->game_id.c_str(), out.c_str());
            }
        }
        save_library();
        return any_failed ? 1 : 0;
    }

private:
    // ------------------------------------------------------------------
    // Setup
    // ------------------------------------------------------------------

    // Use the executable's embedded icon (hub/assets/hub.rc) for the title bar
    // and taskbar instead of SDL's default.
    void apply_window_icon() {
#ifdef _WIN32
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window_, &info)) return;
        HWND hwnd = info.info.win.window;
        HINSTANCE module = GetModuleHandleW(nullptr);
        HICON big = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0));
        HICON small = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0));
        if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
#endif
    }

    void setup_fonts(ImGuiIO& io) {
        std::vector<std::string> candidates;
#ifdef _WIN32
        std::string windir = platform::getenv_utf8("WINDIR");
        if (windir.empty()) windir = "C:\\Windows";
        candidates.push_back(platform::join(windir, "Fonts\\segoeui.ttf"));
        candidates.push_back(platform::join(windir, "Fonts\\arial.ttf"));
#elif defined(__APPLE__)
        candidates.push_back("/System/Library/Fonts/SFNS.ttf");
        candidates.push_back("/Library/Fonts/Arial.ttf");
#else
        candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        candidates.push_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
#endif
        std::string font;
        for (const auto& c : candidates) {
            if (platform::exists(c)) { font = c; break; }
        }
        if (!font.empty()) {
            font_body_ = io.Fonts->AddFontFromFileTTF(font.c_str(), std::round(17.0f * scale_));
            font_title_ = io.Fonts->AddFontFromFileTTF(font.c_str(), std::round(26.0f * scale_));
            font_small_ = io.Fonts->AddFontFromFileTTF(font.c_str(), std::round(14.0f * scale_));
        }
        if (!font_body_) {
            ImFontConfig cfg;
            cfg.SizePixels = 13.0f * scale_;
            font_body_ = io.Fonts->AddFontDefault(&cfg);
            font_title_ = font_body_;
            font_small_ = font_body_;
        }
        io.FontDefault = font_body_;
    }

    void setup_style() {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::StyleColorsDark();
        style.WindowRounding = 0.0f;
        style.ChildRounding = 8.0f;
        style.FrameRounding = 6.0f;
        style.PopupRounding = 8.0f;
        style.GrabRounding = 6.0f;
        style.ScrollbarRounding = 8.0f;
        style.WindowPadding = ImVec2(12, 12);
        style.FramePadding = ImVec2(10, 6);
        style.ItemSpacing = ImVec2(10, 8);
        style.ScrollbarSize = 12.0f;
        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.078f, 0.086f, 0.102f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.106f, 0.118f, 0.141f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.16f, 0.98f);
        c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.23f, 0.28f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.28f, 0.36f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.18f, 0.21f, 0.27f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.32f, 0.44f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.40f, 0.60f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.22f, 0.30f, 0.48f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.37f, 0.58f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.31f, 0.43f, 0.68f, 1.0f);
        c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
        c[ImGuiCol_Separator] = ImVec4(0.25f, 0.28f, 0.34f, 1.0f);
        c[ImGuiCol_CheckMark] = ImVec4(0.31f, 0.55f, 1.0f, 1.0f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.31f, 0.55f, 1.0f, 1.0f);
        c[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.64f, 1.0f);
        style.ScaleAllSizes(scale_);
    }

    // ------------------------------------------------------------------
    // Textures
    // ------------------------------------------------------------------

    SDL_Texture* texture_for(const GameEntry& e) {
        if (e.cover_path.empty()) return nullptr;
        std::string key = e.cover_path + "#" + std::to_string(e.cover_version);
        auto it = textures_.find(key);
        if (it != textures_.end()) return it->second;
        SDL_Texture* tex = nullptr;
        platform::Image img;
        std::string err;
        if (platform::load_image(e.cover_path, img, err)) {
            tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
            if (tex) {
                SDL_UpdateTexture(tex, nullptr, img.rgba.data(), img.width * 4);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(tex, img.width <= 320 ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
                texture_sizes_[tex] = ImVec2(static_cast<float>(img.width), static_cast<float>(img.height));
            }
        } else {
            log_line("cover load failed: " + err);
        }
        textures_[key] = tex;
        return tex;
    }

    ImVec2 texture_size(SDL_Texture* tex) {
        auto it = texture_sizes_.find(tex);
        return it == texture_sizes_.end() ? ImVec2(1, 1) : it->second;
    }

    // Draws a cover (or placeholder) fitted inside the rectangle.
    void draw_cover(ImDrawList* dl, const GameEntry& e, ImVec2 p0, ImVec2 p1, float rounding) {
        SDL_Texture* tex = texture_for(e);
        ImVec2 box(p1.x - p0.x, p1.y - p0.y);
        if (tex) {
            ImVec2 sz = texture_size(tex);
            float s = std::min(box.x / sz.x, box.y / sz.y);
            ImVec2 fit(sz.x * s, sz.y * s);
            ImVec2 a(p0.x + (box.x - fit.x) * 0.5f, p0.y + (box.y - fit.y) * 0.5f);
            ImVec2 b(a.x + fit.x, a.y + fit.y);
            dl->AddRectFilled(p0, p1, IM_COL32(12, 13, 16, 255), rounding);
            dl->AddImageRounded(reinterpret_cast<ImTextureID>(tex), a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, fit.x >= box.x - 1 ? rounding : 0.0f);
            return;
        }
        ImU32 base = console_color(e.console_id);
        dl->AddRectFilledMultiColor(p0, p1, darken(base, 0.55f), darken(base, 0.35f), darken(base, 0.18f), darken(base, 0.30f));
        dl->AddRect(p0, p1, darken(base, 0.8f), rounding);
        // Title, word-wrapped and centered.
        ImGui::PushFont(font_title_);
        float wrap = box.x * 0.82f;
        ImVec2 ts = ImGui::CalcTextSize(e.title.c_str(), nullptr, false, wrap);
        ImGui::PopFont();
        ImVec2 tp(p0.x + (box.x - std::min(ts.x, wrap)) * 0.5f, p0.y + (box.y - ts.y) * 0.5f - 6.0f * scale_);
        dl->AddText(font_title_, font_title_->FontSize, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 160), e.title.c_str(), nullptr, wrap);
        dl->AddText(font_title_, font_title_->FontSize, tp, IM_COL32(245, 245, 245, 255), e.title.c_str(), nullptr, wrap);
        const ConsoleInfo* ci = console_info(e.console_id);
        std::string badge = ci ? ci->short_name : e.console_id;
        ImVec2 bs = ImGui::CalcTextSize(badge.c_str());
        dl->AddText(ImVec2(p1.x - bs.x - 8.0f * scale_, p1.y - bs.y - 6.0f * scale_), IM_COL32(255, 255, 255, 170), badge.c_str());
    }

    // ------------------------------------------------------------------
    // Frame
    // ------------------------------------------------------------------

    void draw_frame() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##root", nullptr, flags);
        ImGui::PopStyleVar();

        const float sidebar_w = 220.0f * scale_;
        ImGui::BeginChild("##sidebar", ImVec2(sidebar_w, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        draw_sidebar();
        ImGui::EndChild();
        ImGui::SameLine(0, 0);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.078f, 0.086f, 0.102f, 1.0f));
        ImGui::BeginChild("##main", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();
        switch (view_) {
            case View::Library:
            case View::Favorites: draw_library_view(); break;
            case View::Jobs: draw_jobs_view(); break;
            case View::Settings: draw_settings_view(); break;
        }
        ImGui::EndChild();

        draw_modals();
        ImGui::End();
        draw_toasts();
    }

    void draw_sidebar() {
        ImGui::Dummy(ImVec2(0, 6 * scale_));
        ImGui::Indent(12 * scale_);
        ImGui::PushFont(font_title_);
        ImGui::TextUnformatted("Native Console");
        ImGui::PopFont();
        ImGui::PushFont(font_small_);
        ImGui::TextDisabled("Hub %s", HUB_VERSION);
        ImGui::PopFont();
        ImGui::Unindent(12 * scale_);
        ImGui::Dummy(ImVec2(0, 10 * scale_));

        auto nav = [&](const char* label, View v, const char* count = nullptr) {
            bool active = view_ == v;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            std::string text = std::string("  ") + label;
            if (count) text += std::string("   ") + count;
            if (ImGui::Button(text.c_str(), ImVec2(-8 * scale_, 34 * scale_))) view_ = v;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        };
        ImGui::Indent(8 * scale_);
        std::string lib_count = std::to_string(library_.games().size());
        long long fav = std::count_if(library_.games().begin(), library_.games().end(), [](const GameEntry& g) { return g.favorite; });
        std::string fav_count = std::to_string(fav);
        int active_jobs = 0;
        for (const auto& j : jobs_.jobs()) {
            JobState s = j->state;
            if (s == JobState::Queued || s == JobState::Running) ++active_jobs;
        }
        std::string job_count = active_jobs ? std::to_string(active_jobs) + " active" : "";
        nav("Library", View::Library, lib_count.c_str());
        nav("Favorites", View::Favorites, fav_count.c_str());
        nav("Jobs", View::Jobs, job_count.empty() ? nullptr : job_count.c_str());
        nav("Settings", View::Settings);
        ImGui::Unindent(8 * scale_);

        ImGui::Dummy(ImVec2(0, 12 * scale_));
        ImGui::Indent(12 * scale_);
        ImGui::PushFont(font_small_);
        ImGui::TextDisabled("CONSOLES");
        ImGui::PopFont();
        ImGui::Unindent(12 * scale_);
        ImGui::Indent(8 * scale_);
        auto console_button = [&](const std::string& id, const std::string& label, bool supported) {
            bool active = console_filter_ == id;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::Button(("      " + label + "##console_" + id).c_str(), ImVec2(-8 * scale_, 30 * scale_))) {
                console_filter_ = id;
                if (view_ != View::Library && view_ != View::Favorites) view_ = View::Library;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem(("##console_ctx_" + id).c_str())) {
                const std::string item = id.empty() ? std::string("Add all games to Steam...") : "Add " + label + " games to Steam...";
                if (ImGui::MenuItem(item.c_str())) open_steam_picker(games_in_scope(false, id));
                ImGui::EndPopup();
            }
            if (!id.empty()) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(ImVec2(pos.x + 16 * scale_, pos.y + 15 * scale_), 5 * scale_, console_color(id));
                if (!supported) {
                    ImGui::PushFont(font_small_);
                    ImVec2 ts = ImGui::CalcTextSize("soon");
                    dl->AddText(ImVec2(pos.x + ImGui::GetContentRegionAvail().x - ts.x - 4 * scale_, pos.y + 8 * scale_), IM_COL32(150, 156, 168, 255), "soon");
                    ImGui::PopFont();
                }
            }
        };
        console_button("", "All consoles", true);
        for (const auto& c : all_consoles()) {
            const ConsoleBackend* b = backend_for_family(c.family_id);
            console_button(c.id, c.name, b && b->supported());
        }
        ImGui::Unindent(8 * scale_);

        ImGui::Dummy(ImVec2(0, 12 * scale_));
        ImGui::Indent(12 * scale_);
        ImGui::PushFont(font_small_);
        ImGui::TextDisabled("IMPORT");
        ImGui::PopFont();
        ImGui::Unindent(12 * scale_);
        ImGui::Indent(8 * scale_);
        if (ImGui::Button("Import ROM...", ImVec2(-8 * scale_, 32 * scale_))) import_dialog(false);
        if (ImGui::Button("Import folder...", ImVec2(-8 * scale_, 32 * scale_))) import_dialog(true);
        ImGui::PushFont(font_small_);
        ImGui::TextDisabled("or drop ROMs / folders here");
        ImGui::PopFont();
        ImGui::Unindent(8 * scale_);

        ImGui::Dummy(ImVec2(0, 12 * scale_));
        ImGui::Indent(12 * scale_);
        ImGui::PushFont(font_small_);
        ImGui::TextDisabled("EXPORT");
        ImGui::PopFont();
        ImGui::Unindent(12 * scale_);
        ImGui::Indent(8 * scale_);
        if (ImGui::Button("Compile for export...", ImVec2(-8 * scale_, 32 * scale_))) open_export_dialog({});
        ImGui::PushFont(font_small_);
        ImGui::TextDisabled("builds for other computers");
        ImGui::PopFont();
        ImGui::Unindent(8 * scale_);

        // Footer.
        float footer_h = 44 * scale_;
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - footer_h));
        ImGui::Indent(12 * scale_);
        ImGui::PushFont(font_small_);
        if (jobs_.busy()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.27f, 1.0f), "Building...");
        } else {
            ImGui::TextDisabled("Idle");
        }
        if (ImGui::SmallButton("Open data folder")) platform::open_in_file_manager(settings_.data_dir);
        ImGui::PopFont();
        ImGui::Unindent(12 * scale_);
    }

    static std::vector<platform::FileFilter> rom_file_filters() {
        std::vector<platform::FileFilter> filters;
        // A family whose game has no extension is named outright ("game").
        auto add = [](std::string& pattern, const ConsoleBackend& b) {
            for (const auto& ext : b.extensions()) pattern += (pattern.empty() ? "" : ";") + ("*" + ext);
            for (const auto& name : b.file_names()) pattern += (pattern.empty() ? "" : ";") + name;
        };
        std::string all;
        for (const auto& b : backends()) add(all, *b);
        filters.push_back({"All supported ROMs", all});
        for (const auto& b : backends()) {
            std::string pattern;
            add(pattern, *b);
            filters.push_back({b->family_name(), pattern});
        }
        filters.push_back({"All files", "*.*"});
        return filters;
    }

    void import_dialog(bool folder) {
        std::vector<std::string> paths;
        if (folder) {
            std::string dir;
            if (platform::pick_folder(dir)) paths.push_back(dir);
        } else {
            platform::pick_files(rom_file_filters(), true, paths);
        }
        if (!paths.empty()) import_paths(paths);
    }

    // ------------------------------------------------------------------
    // Compile for export
    // ------------------------------------------------------------------

    // ROM files named directly or found (recursively) in named folders.
    static std::vector<std::string> expand_rom_paths(const std::vector<std::string>& paths) {
        std::vector<std::string> roms;
        for (const auto& raw : paths) {
            std::string path = platform::absolute(raw);
            if (platform::is_directory(path)) {
                for (const auto& file : roms_in_folder(path)) roms.push_back(file);
            } else if (backend_for_file(path)) {
                roms.push_back(path);
            }
        }
        return roms;
    }

    // Queues one export job per ROM; used by the dialog and --export. Returns the count.
    int queue_exports(const std::vector<std::string>& paths, const std::vector<ExportTarget>& targets, const std::string& dest) {
        std::set<std::string> taken;
        int queued = 0;
        for (const auto& rom : expand_rom_paths(paths)) {
            const ConsoleBackend* backend = backend_for_file(rom);
            RomInfo info;
            std::string err;
            if (!backend || !backend->supported() || !backend->probe(rom, info, err)) {
                log_line("export: skip " + rom + (err.empty() ? "" : " (" + err + ")"));
                continue;
            }
            std::string title = !info.title.empty() ? info.title : !info.header_title.empty() ? info.header_title : platform::file_stem(rom);
            std::string id;
            // A game that is already in the library keeps its id and title.
            if (const GameEntry* g = library_.find_by_hash(sha256_hex_of_file(rom))) {
                id = g->id;
                title = g->title;
            }
            if (id.empty() || taken.count(id)) id = make_export_id(title, taken);
            taken.insert(id);
            jobs_.enqueue_export(id, title, backend->family_id(), rom, format_export_targets(targets),
                                 platform::absolute(dest), settings_);
            ++queued;
        }
        return queued;
    }

    void open_export_dialog(std::vector<std::string> sources) {
        export_ = ExportDialog{};
        export_.open = true;
        export_.sources = std::move(sources);
        export_.roms = expand_rom_paths(export_.sources);
        export_.dest = settings_.export_dir;
        for (auto t : parse_export_targets(settings_.export_targets)) export_.checked[t] = true;
    }

    void draw_export_modal() {
        const char* popup = "Compile for export";
        if (export_.open && !ImGui::IsPopupOpen(popup)) ImGui::OpenPopup(popup);
        ImGui::SetNextWindowSize(ImVec2(640 * scale_, 0));
        if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        bool close = false;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Builds games to run on other computers. Each game becomes one file per target that you can copy over.");

        ImGui::SeparatorText("Games");
        if (export_.sources.empty()) {
            ImGui::TextDisabled("Choose ROM files or a folder of ROMs.");
        } else {
            const size_t shown = std::min<size_t>(export_.sources.size(), 4);
            for (size_t i = 0; i < shown; ++i) ImGui::TextUnformatted(export_.sources[i].c_str());
            if (export_.sources.size() > shown) ImGui::TextDisabled("and %d more", static_cast<int>(export_.sources.size() - shown));
            if (export_.roms.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.error), "No supported ROMs found there.");
            else ImGui::TextDisabled("%d ROM(s) to export", static_cast<int>(export_.roms.size()));
        }
        if (ImGui::Button("Choose ROMs...")) {
            std::vector<std::string> paths;
            if (platform::pick_files(rom_file_filters(), true, paths) && !paths.empty()) {
                export_.sources = paths;
                export_.roms = expand_rom_paths(paths);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Choose folder...")) {
            std::string dir;
            if (platform::pick_folder(dir)) {
                export_.sources = {dir};
                export_.roms = expand_rom_paths(export_.sources);
            }
        }

        ImGui::SeparatorText("Build for");
        std::vector<ExportTarget> selected;
        for (const auto& info : export_target_infos()) {
            ImGui::PushID(info.id);
            const std::string problem = export_target_problem(info.target, settings_);
            bool on = problem.empty() && export_.checked[info.target];
            ImGui::BeginDisabled(!problem.empty());
            if (ImGui::Checkbox(info.label, &on)) export_.checked[info.target] = on;
            ImGui::EndDisabled();
            ImGui::Indent(28 * scale_);
            ImGui::PushFont(font_small_);
            if (problem.empty()) ImGui::TextDisabled("%s", info.description);
            else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn), "Unavailable: %s", problem.c_str());
            // A console that cannot go to this target yet, among the chosen ROMs.
            if (problem.empty() && on) {
                std::set<const ConsoleBackend*> warned;
                for (const auto& rom : export_.roms) {
                    const ConsoleBackend* backend = backend_for_file(rom);
                    if (!backend || warned.count(backend)) continue;
                    warned.insert(backend);
                    const std::string console_problem = backend->export_problem(info.id);
                    if (!console_problem.empty()) {
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn), "%s: %s",
                                           backend->family_name().c_str(), console_problem.c_str());
                    }
                }
            }
            ImGui::PopFont();
            ImGui::Unindent(28 * scale_);
            if (on) selected.push_back(info.target);
            ImGui::PopID();
        }

        ImGui::SeparatorText("Save to");
        /*
         * Typed as well as picked. A folder dialog has its own ideas about what
         * may be selected -- a drive's root is the awkward case, and a drive is
         * exactly where an export tends to go -- so the path is editable and
         * the button is only a convenience. It is made when the export runs.
         */
        {
            char buf[1024];
            std::snprintf(buf, sizeof buf, "%s", export_.dest.c_str());
            ImGui::SetNextItemWidth(-8 * scale_);
            if (ImGui::InputTextWithHint("##export-dest", "H:\\ or any folder -- typed or pasted",
                                         buf, sizeof buf))
                export_.dest = buf;
        }
        if (ImGui::Button("Choose destination...")) {
            std::string dir;
            if (platform::pick_folder(dir)) export_.dest = dir;
        }
        if (!export_.dest.empty() && !platform::is_directory(export_.dest))
            ImGui::TextDisabled("This folder does not exist yet; it will be created.");

        ImGui::Dummy(ImVec2(0, 8 * scale_));
        const bool ready = !export_.roms.empty() && !selected.empty() && !export_.dest.empty();
        ImGui::BeginDisabled(!ready);
        const std::string go = "Export " + std::to_string(export_.roms.size()) + " game(s)";
        if (ImGui::Button(go.c_str(), ImVec2(180 * scale_, 34 * scale_))) {
            settings_.export_dir = settings_edit_.export_dir = export_.dest;
            settings_.export_targets = settings_edit_.export_targets = format_export_targets(selected);
            std::string err;
            if (!settings_.save(err)) toast(err, true);
            const int queued = queue_exports(export_.sources, selected, export_.dest);
            toast("Exporting " + std::to_string(queued) + " game(s) to " + export_.dest);
            view_ = View::Jobs;
            close = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120 * scale_, 34 * scale_))) close = true;
        ImGui::PopTextWrapPos();
        if (close) {
            export_.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ------------------------------------------------------------------
    // Library view
    // ------------------------------------------------------------------

    std::vector<GameEntry*> filtered_games() {
        std::vector<GameEntry*> out;
        std::string needle = search_;
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (auto& g : library_.games()) {
            if (view_ == View::Favorites && !g.favorite) continue;
            if (!console_filter_.empty() && g.console_id != console_filter_) continue;
            if (!needle.empty()) {
                std::string hay = g.title + " " + g.header_title + " " + g.description;
                std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (hay.find(needle) == std::string::npos) continue;
            }
            out.push_back(&g);
        }
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        switch (sort_mode_) {
            case 0: std::sort(out.begin(), out.end(), [&](GameEntry* a, GameEntry* b) { return lower(a->title) < lower(b->title); }); break;
            case 1: std::sort(out.begin(), out.end(), [](GameEntry* a, GameEntry* b) { return a->last_played > b->last_played; }); break;
            case 2: std::sort(out.begin(), out.end(), [](GameEntry* a, GameEntry* b) { return a->added_at > b->added_at; }); break;
            default: break;
        }
        return out;
    }

    void draw_library_view() {
        ImGui::Dummy(ImVec2(0, 4 * scale_));
        ImGui::Indent(14 * scale_);
        ImGui::PushFont(font_title_);
        ImGui::TextUnformatted(view_ == View::Favorites ? "Favorites" : "Library");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * scale_);
        ImGui::SetNextItemWidth(260 * scale_);
        input_string("##search", search_, ImGuiInputTextFlags_None);
        if (search_.empty() && !ImGui::IsItemActive()) {
            ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 10 * scale_, min.y + 6 * scale_), IM_COL32(120, 126, 138, 255), "Search titles...");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170 * scale_);
        const char* sorts[] = {"Sort: Title", "Sort: Recently played", "Sort: Recently added"};
        ImGui::Combo("##sort", &sort_mode_, sorts, 3);
        ImGui::SameLine();
        if (ImGui::Button(grid_mode_ ? "List view" : "Grid view")) grid_mode_ = !grid_mode_;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * scale_);
        ImGui::SliderFloat("##cardsize", &card_size_, 140.0f, 320.0f, "Size");
        ImGui::SameLine();
        if (ImGui::Button("Add to Steam...")) open_steam_picker({});
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Choose games to add to Steam: any mix of single games, your favorites, a console, or the whole library");
        }
        ImGui::Unindent(14 * scale_);
        ImGui::Dummy(ImVec2(0, 2 * scale_));

        std::vector<GameEntry*> games = filtered_games();
        GameEntry* selected = selected_id_.empty() ? nullptr : library_.find(selected_id_);
        float detail_w = selected ? 360.0f * scale_ : 0.0f;
        ImVec2 avail = ImGui::GetContentRegionAvail();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##content", ImVec2(avail.x - detail_w, 0), ImGuiChildFlags_None);
        if (games.empty()) {
            ImGui::Dummy(ImVec2(0, 40 * scale_));
            ImGui::Indent(24 * scale_);
            if (library_.games().empty()) {
                ImGui::TextDisabled("Your library is empty.");
                ImGui::TextDisabled("Import a ROM or a folder of ROMs from the sidebar; each game is recompiled to a native executable.");
            } else if (view_ == View::Favorites) {
                ImGui::TextDisabled("No favorites yet. Select a game and press the star.");
            } else {
                ImGui::TextDisabled("No games match the current filter.");
            }
            ImGui::Unindent(24 * scale_);
        } else if (grid_mode_) {
            draw_grid(games);
        } else {
            draw_list(games);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        if (selected) {
            ImGui::SameLine(0, 0);
            ImGui::BeginChild("##detail", ImVec2(detail_w, 0), ImGuiChildFlags_Border);
            draw_detail(*selected);
            ImGui::EndChild();
        }
    }

    void draw_grid(const std::vector<GameEntry*>& games) {
        const float pad = 14.0f * scale_;
        const float card_w = card_size_ * scale_;
        const float spacing = 14.0f * scale_;
        float avail = ImGui::GetContentRegionAvail().x - pad * 2;
        int columns = std::max(1, static_cast<int>((avail + spacing) / (card_w + spacing)));
        ImGui::Dummy(ImVec2(0, 4 * scale_));
        ImGui::Indent(pad);
        int col = 0;
        for (GameEntry* g : games) {
            if (col > 0) ImGui::SameLine(0, spacing);
            draw_card(*g, card_w);
            if (++col >= columns) {
                col = 0;
                ImGui::Dummy(ImVec2(0, spacing - ImGui::GetStyle().ItemSpacing.y));
            }
        }
        ImGui::Unindent(pad);
        ImGui::Dummy(ImVec2(0, pad));
    }

    void draw_card(GameEntry& g, float card_w) {
        const float inner = 10.0f * scale_;
        const float cover_h = card_w;
        const float card_h = cover_h + 62.0f * scale_;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 end(pos.x + card_w, pos.y + card_h);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // The group makes the card one item anchored at its top edge, so the
        // grid's SameLine() places the next card beside it rather than at the
        // bottom corner left by the cursor moves below.
        ImGui::BeginGroup();
        ImGui::PushID(g.id.c_str());
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##card", ImVec2(card_w, card_h));
        bool hovered = ImGui::IsItemHovered();
        bool selected = selected_id_ == g.id;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) select(g);
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launch(g);
        if (ImGui::BeginPopupContextItem("##ctx")) {
            select(g);
            draw_context_menu(g);
            ImGui::EndPopup();
        }

        ImU32 bg = selected ? theme_.card_selected : hovered ? theme_.card_hover : theme_.card;
        dl->AddRectFilled(pos, end, bg, 10.0f * scale_);
        ImVec2 c0(pos.x + inner, pos.y + inner);
        ImVec2 c1(pos.x + card_w - inner, pos.y + cover_h - inner);
        draw_cover(dl, g, c0, c1, 6.0f * scale_);

        // Status badge.
        std::string badge;
        ImU32 badge_col = 0;
        if (is_running(g.id)) { badge = "RUNNING"; badge_col = theme_.ok; }
        else if (g.status == "building") { badge = "BUILDING"; badge_col = theme_.warn; }
        else if (g.status == "queued") { badge = "QUEUED"; badge_col = theme_.warn; }
        else if (g.status == "failed") { badge = "FAILED"; badge_col = theme_.error; }
        else if (g.status == "unsupported") { badge = "COMING SOON"; badge_col = IM_COL32(120, 126, 138, 255); }
        if (!badge.empty()) {
            ImGui::PushFont(font_small_);
            ImVec2 ts = ImGui::CalcTextSize(badge.c_str());
            ImVec2 b0(c0.x + 6 * scale_, c0.y + 6 * scale_);
            ImVec2 b1(b0.x + ts.x + 12 * scale_, b0.y + ts.y + 6 * scale_);
            dl->AddRectFilled(b0, b1, darken(badge_col, 0.35f), 4.0f * scale_);
            dl->AddText(ImVec2(b0.x + 6 * scale_, b0.y + 3 * scale_), badge_col, badge.c_str());
            ImGui::PopFont();
        }

        // Favorite star (clickable overlay).
        ImVec2 star_center(c1.x - 16 * scale_, c0.y + 16 * scale_);
        ImGui::SetCursorScreenPos(ImVec2(star_center.x - 14 * scale_, star_center.y - 14 * scale_));
        ImGui::InvisibleButton("##fav", ImVec2(28 * scale_, 28 * scale_));
        bool star_hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            g.favorite = !g.favorite;
            save_library();
        }
        if (g.favorite || hovered || star_hovered) {
            dl->AddCircleFilled(star_center, 13 * scale_, IM_COL32(0, 0, 0, 140));
            draw_star(dl, star_center, 9 * scale_, g.favorite ? theme_.star : IM_COL32(230, 230, 230, 200), g.favorite);
        }

        // Title and meta.
        ImGui::PushClipRect(ImVec2(pos.x + inner, pos.y + cover_h), ImVec2(end.x - inner, end.y), true);
        std::string title = ellipsize(g.title, card_w - inner * 2);
        dl->AddText(ImVec2(pos.x + inner, pos.y + cover_h + 2 * scale_), theme_.text, title.c_str());
        ImGui::PushFont(font_small_);
        const ConsoleInfo* ci = console_info(g.console_id);
        std::string meta = (ci ? ci->name : g.console_id);
        long long played = play_seconds_including_session(g);
        if (played > 0) meta += "  ·  " + format_duration(played);
        else if (g.last_played > 0) meta += "  ·  played " + format_relative_time(g.last_played);
        dl->AddText(font_small_, font_small_->FontSize, ImVec2(pos.x + inner, pos.y + cover_h + 26 * scale_), theme_.text_dim, ellipsize(meta, card_w - inner * 2).c_str());
        ImGui::PopFont();
        ImGui::PopClipRect();

        if (selected) dl->AddRect(pos, end, theme_.accent, 10.0f * scale_, 0, 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(pos.x, end.y));
        ImGui::Dummy(ImVec2(card_w, 0));
        ImGui::PopID();
        ImGui::EndGroup();
    }

    void draw_list(const std::vector<GameEntry*>& games) {
        ImGui::Indent(14 * scale_);
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##games", 8, flags, ImVec2(-14 * scale_, 0))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30 * scale_);
            ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 4.0f);
            ImGui::TableSetupColumn("Console", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Play time", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Last played", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80 * scale_);
            ImGui::TableHeadersRow();
            for (GameEntry* g : games) {
                ImGui::PushID(g->id.c_str());
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 34 * scale_);
                ImGui::TableSetColumnIndex(0);
                ImVec2 cell = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##fav", ImVec2(24 * scale_, 24 * scale_));
                if (ImGui::IsItemClicked()) {
                    g->favorite = !g->favorite;
                    save_library();
                }
                draw_star(ImGui::GetWindowDrawList(), ImVec2(cell.x + 12 * scale_, cell.y + 12 * scale_), 8 * scale_,
                          g->favorite ? theme_.star : IM_COL32(110, 116, 128, 255), g->favorite);
                ImGui::TableSetColumnIndex(1);
                bool selected = selected_id_ == g->id;
                if (ImGui::Selectable(g->title.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 26 * scale_))) select(*g);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launch(*g);
                if (ImGui::BeginPopupContextItem("##ctx")) {
                    select(*g);
                    draw_context_menu(*g);
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(2);
                const ConsoleInfo* ci = console_info(g->console_id);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(console_color(g->console_id)), "%s", ci ? ci->name.c_str() : g->console_id.c_str());
                ImGui::TableSetColumnIndex(3);
                draw_status_text(*g);
                ImGui::TableSetColumnIndex(4);
                {
                    long long played = play_seconds_including_session(*g);
                    if (played > 0) ImGui::TextUnformatted(format_duration(played).c_str());
                    else ImGui::TextDisabled("none");
                }
                ImGui::TableSetColumnIndex(5);
                ImGui::TextDisabled("%s", format_relative_time(g->last_played).c_str());
                ImGui::TableSetColumnIndex(6);
                {
                    const SizeInfo& sz = size_for(*g);
                    if (sz.on_disk > 0) ImGui::TextDisabled("%s", format_bytes(sz.on_disk).c_str());
                    else ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(7);
                if (g->status == "ready") {
                    if (ImGui::SmallButton(is_running(g->id) ? "Running" : "Play")) launch(*g);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Unindent(14 * scale_);
    }

    void draw_status_text(const GameEntry& g) {
        if (is_running(g.id)) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.ok), "Running");
        else if (g.status == "ready") ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.ok), "Ready");
        else if (g.status == "building") ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn), "Building...");
        else if (g.status == "queued") ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn), "Queued");
        else if (g.status == "failed") ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.error), "Build failed");
        else if (g.status == "unsupported") ImGui::TextDisabled("Coming soon");
        else ImGui::TextDisabled("%s", g.status.c_str());
    }

    void select(GameEntry& g) {
        if (selected_id_ != g.id) {
            selected_id_ = g.id;
            edit_title_ = g.title;
            edit_description_ = g.description;
            edit_args_ = g.launch_args;
            edit_edited_ = g.edited_data;
            refresh_texture_packs(g);
            refresh_voxel_profiles(g);
            refresh_edited_data(g);
        }
    }

    // ------------------------------------------------------------------
    // Texture packs (Game Boy and Game Boy Advance)
    // ------------------------------------------------------------------

    void refresh_texture_packs(const GameEntry& g) {
        texture_packs_ = g.family_id == "gb" || g.family_id == "gba" ? texture_packs::list_packs(g.game_dir, g.family_id)
                                                                     : std::vector<texture_packs::PackInfo>{};
        texture_packs_game_ = g.id;
    }

    // Scanning the executable for the flag is slow, so cache it per file and size.
    bool texture_packs_supported(const GameEntry& g) {
        const long long size = platform::file_size(g.exe_path);
        auto it = texture_pack_support_.find(g.exe_path);
        if (it == texture_pack_support_.end() || it->second.first != size) {
            bool supported = size > 0 && texture_packs::executable_supports_texture_packs(g.exe_path);
            it = texture_pack_support_.insert_or_assign(g.exe_path, std::make_pair(size, supported)).first;
        }
        return it->second.second;
    }

    static std::string texture_pack_label(const texture_packs::PackInfo& pack) {
        if (!pack.valid()) return pack.file_name + " (unusable)";
        return pack.pack_id + "  " + std::to_string(pack.scale) + "x, " + std::to_string(pack.entries) + " tiles";
    }

    void add_texture_pack_dialog(GameEntry& g) {
        std::vector<std::string> paths;
        const bool gba = g.family_id == "gba";
        if (!platform::pick_files({{"Texture packs", gba ? "*.gbatp" : "*.gbtp"}, {"All files", "*.*"}}, false, paths) ||
            paths.empty())
            return;
        texture_packs::PackInfo installed;
        std::string err;
        if (!texture_packs::import_pack(g.game_dir, paths.front(), g.rom_sha256, installed, err, g.family_id)) {
            toast(err, true);
            return;
        }
        g.texture_pack = installed.file_name;
        save_library();
        refresh_texture_packs(g);
        toast("Texture pack \"" + installed.pack_id + "\" added and selected");
    }

    // Playing together: two windows here, or host / join across the network.
    // A GB/GBC or GBA link cable (runtime gbrt_link.h, runtime-gba
    // gba_link.h) - the backend says whether its family has a link and what
    // to call it.
    void draw_link_cable_section(GameEntry& g) {
        const ConsoleBackend* backend = backend_for_family(g.family_id);
        if (!backend || backend->link_name().empty()) return;
        const float w = ImGui::GetContentRegionAvail().x;
        const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::TextDisabled("%s", backend->link_name().c_str());
        if (ImGui::Button("2 Players on this PC", ImVec2(-1, 0))) launch_two_players(g);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Opens two linked windows. Player 2 keeps its own saves and settings,\n"
                              "and uses the second controller. Whichever window has focus takes the keyboard.");
        }
        if (ImGui::Button("Host for other PCs", ImVec2(half, 0))) launch(g, {"--link-host"});
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Player 1. Others join this computer's address (port %d).", backend->link_port());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(link_join_address_.empty());
        if (ImGui::Button("Join", ImVec2(half, 0))) launch(g, {"--link-join", link_join_address_});
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(-1);
        input_string("##linkjoin", link_join_address_);
        if (link_join_address_.empty() && !ImGui::IsItemActive()) {
            ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + ImGui::GetStyle().FramePadding.x, min.y + ImGui::GetStyle().FramePadding.y),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), "Host's address to join, e.g. 192.168.1.20");
        }
    }

    // Where a game's recorded runs are kept: outside the game's own folder,
    // so that rebuilding the game or taking it out of the library leaves them.
    std::string runs_dir(const GameEntry& g) const {
        return platform::join(platform::join(settings_.data_dir, "runs"), g.id);
    }

    // The run a new sitting would be added to: the newest folder that holds
    // a recording. Empty when the game has none.
    std::string latest_run(const GameEntry& g) const {
        std::string latest;
        for (const std::string& name : platform::list_subdirectories(runs_dir(g))) {
            // Sorted, and named by when they were started: the last one is the newest.
            if (platform::exists(platform::join(platform::join(runs_dir(g), name), "segment-0001.run"))) latest = name;
        }
        return latest;
    }

    void record_run(GameEntry& g, const std::string& name) {
        const ConsoleBackend* backend = backend_for_family(g.family_id);
        if (!backend) return;
        if (is_running(g.id)) {
            toast(g.title + " is already running; close it first", true);
            return;
        }
        const std::string dir = platform::join(runs_dir(g), name);
        std::string err;
        if (!platform::ensure_directory(dir, err)) {
            toast(err, true);
            return;
        }
        // A run is recorded of the game as it is: the record names the ROM,
        // and would not replay without the edits.
        launch(g, backend->record_run_args(dir), "", EditedData::Original);
        run_record_checked_ = -1.0;
    }

    // A speedrun or a playthrough recorded for whoever is taking the game
    // apart: every button with the cycle it changed on, from a new save.
    void draw_run_record_section(GameEntry& g) {
        const ConsoleBackend* backend = backend_for_family(g.family_id);
        if (!backend || backend->record_run_args("x").empty()) return;
        const double now = ImGui::GetTime();
        if (run_record_game_ != g.id || run_record_checked_ < 0.0 || now - run_record_checked_ > 2.0) {
            run_record_game_ = g.id;
            run_record_latest_ = latest_run(g);
            run_record_checked_ = now;
        }
        const float w = ImGui::GetContentRegionAvail().x;
        const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::TextDisabled("Record a run");
        if (ImGui::Button("New run", ImVec2(half, 0))) {
            char name[40];
            const std::time_t t = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &t);
#else
            localtime_r(&t, &local);
#endif
            std::strftime(name, sizeof(name), "run-%Y%m%d-%H%M%S", &local);
            record_run(g, name);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Starts the game from a new save and records every button with its exact timing,\n"
                              "so the whole run can be played back later. Your own save is not touched.\n"
                              "Save states cannot be loaded while it records; fast-forward and pausing are fine.");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(run_record_latest_.empty());
        if (ImGui::Button("Continue run", ImVec2(half, 0))) record_run(g, run_record_latest_);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (run_record_latest_.empty()) ImGui::SetTooltip("No run has been recorded for this game yet.");
            else ImGui::SetTooltip("Another sitting of %s, from the save it left.", run_record_latest_.c_str());
        }
        if (!run_record_latest_.empty()) {
            if (ImGui::Button("Runs folder", ImVec2(-1, 0))) platform::open_in_file_manager(runs_dir(g));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("To hand a run on, send the whole of its folder (%s).", run_record_latest_.c_str());
            }
        }
    }

    void draw_texture_pack_section(GameEntry& g) {
        if (g.family_id != "gb" && g.family_id != "gba") return;
        if (texture_packs_game_ != g.id) refresh_texture_packs(g);

        ImGui::TextDisabled("Texture pack");
        const texture_packs::PackInfo* selected = nullptr;
        for (const auto& pack : texture_packs_) {
            if (pack.file_name == g.texture_pack) selected = &pack;
        }
        std::string preview = g.texture_pack.empty() ? "Original art"
                              : selected ? texture_pack_label(*selected)
                                         : g.texture_pack + " (missing)";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##texturepack", preview.c_str())) {
            if (ImGui::Selectable("Original art", g.texture_pack.empty())) {
                g.texture_pack.clear();
                save_library();
            }
            for (const auto& pack : texture_packs_) {
                const bool usable = pack.valid() && pack.fits_rom(g.rom_sha256);
                ImGui::BeginDisabled(!usable);
                if (ImGui::Selectable(texture_pack_label(pack).c_str(), pack.file_name == g.texture_pack)) {
                    g.texture_pack = pack.file_name;
                    save_library();
                }
                ImGui::EndDisabled();
                if (!usable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", pack.valid() ? "Made for a different ROM" : pack.error.c_str());
                } else if (usable && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\nLicense: %s", pack.file_name.c_str(), pack.license.c_str());
                }
            }
            ImGui::EndCombo();
        }

        const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        if (ImGui::Button("Add pack...", ImVec2(third, 0))) add_texture_pack_dialog(g);
        ImGui::SameLine();
        ImGui::BeginDisabled(g.texture_pack.empty() || !selected);
        if (ImGui::Button("Remove pack", ImVec2(third, 0))) {
            std::string err;
            if (texture_packs::remove_pack(g.game_dir, g.texture_pack, err)) {
                toast("Removed texture pack " + g.texture_pack);
                g.texture_pack.clear();
                save_library();
            } else {
                toast(err, true);
            }
            refresh_texture_packs(g);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Packs folder", ImVec2(third, 0))) {
            std::string err;
            const std::string dir = texture_packs::packs_dir(g.game_dir);
            if (platform::ensure_directory(dir, err)) platform::open_in_file_manager(dir);
            else toast(err, true);
            refresh_texture_packs(g);
        }

        if (!g.texture_pack.empty()) {
            ImGui::PushFont(font_small_);
            if (!selected) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn),
                                   "The selected pack is missing; the game runs with original art.");
            } else if (!selected->valid() || !selected->fits_rom(g.rom_sha256)) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn),
                                   "The selected pack is unusable; the game runs with original art.");
            } else if (g.status == "ready" && !texture_packs_supported(g)) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn),
                                   "This build predates texture packs. Rebuild the game to use one.");
            }
            ImGui::PopFont();
        }
        ImGui::Dummy(ImVec2(0, 4 * scale_));
    }

    // ------------------------------------------------------------------
    // 3D view (Game Boy)
    // ------------------------------------------------------------------

    void refresh_voxel_profiles(const GameEntry& g) {
        voxel_profiles_.clear();
        voxel_differs_from_builtin_ = false;
        if (g.family_id == "gb") {
            voxel_profiles::update_builtin_copies(g.game_dir);
            voxel_differs_from_builtin_ = !g.voxel_profile.empty() &&
                                          voxel_profiles::differs_from_builtin(g.game_dir, g.voxel_profile);
            voxel_profiles_ = voxel_profiles::list_profiles(g.game_dir);
            for (auto& builtin : voxel_profiles::available_builtins(g.game_dir, g.rom_sha256)) {
                voxel_profiles_.push_back(std::move(builtin));
            }
        }
        voxel_profiles_game_ = g.id;
    }

    bool voxel_view_supported(const GameEntry& g) {
        const long long size = platform::file_size(g.exe_path);
        auto it = voxel_support_.find(g.exe_path);
        if (it == voxel_support_.end() || it->second.first != size) {
            bool supported = size > 0 && voxel_profiles::executable_supports_voxel_view(g.exe_path);
            it = voxel_support_.insert_or_assign(g.exe_path, std::make_pair(size, supported)).first;
        }
        return it->second.second;
    }

    const voxel_profiles::ProfileInfo* selected_voxel_profile(const GameEntry& g) const {
        for (const auto& profile : voxel_profiles_) {
            if (!profile.builtin && profile.file_name == g.voxel_profile) return &profile;
        }
        return nullptr;
    }

    static std::string voxel_label(const voxel_profiles::ProfileInfo& profile) {
        if (!profile.valid()) return profile.file_name + " (unusable)";
        return profile.builtin ? profile.title + " (built-in)" : profile.title;
    }

    void select_voxel_profile(GameEntry& g, const voxel_profiles::ProfileInfo& profile) {
        std::string file = profile.file_name;
        if (profile.builtin) {
            voxel_profiles::ProfileInfo installed;
            std::string err;
            if (!voxel_profiles::install_builtin(g.game_dir, profile.file_name, installed, err)) {
                toast(err, true);
                return;
            }
            file = installed.file_name;
        }
        g.voxel_profile = file;
        save_library();
        refresh_voxel_profiles(g);
    }

    void add_voxel_profile_dialog(GameEntry& g) {
        std::vector<std::string> paths;
        if (!platform::pick_files({{"3D view profiles", "*.voxel"}, {"All files", "*.*"}}, false, paths) || paths.empty()) return;
        voxel_profiles::ProfileInfo installed;
        std::string err;
        if (!voxel_profiles::import_profile(g.game_dir, paths.front(), g.rom_sha256, installed, err)) {
            toast(err, true);
            return;
        }
        g.voxel_profile = installed.file_name;
        save_library();
        refresh_voxel_profiles(g);
        toast("3D profile \"" + installed.title + "\" added and selected");
    }

    void draw_voxel_section(GameEntry& g) {
        if (g.family_id != "gb") return;
        if (voxel_profiles_game_ != g.id) refresh_voxel_profiles(g);

        ImGui::TextDisabled("3D view");
        const voxel_profiles::ProfileInfo* selected = selected_voxel_profile(g);
        const std::string preview = g.voxel_profile.empty() ? "Off (2D)"
                                    : selected ? voxel_label(*selected)
                                               : g.voxel_profile + " (missing)";
        ImGui::SetNextItemWidth(-1);
        std::optional<voxel_profiles::ProfileInfo> picked;  // applied after the combo: it refreshes the list
        if (ImGui::BeginCombo("##voxelprofile", preview.c_str())) {
            if (ImGui::Selectable("Off (2D)", g.voxel_profile.empty())) {
                g.voxel_profile.clear();
                save_library();
            }
            for (const auto& profile : voxel_profiles_) {
                const bool usable = profile.valid() && profile.matches(g.rom_sha256);
                ImGui::BeginDisabled(!usable);
                const bool chosen = !profile.builtin && profile.file_name == g.voxel_profile;
                if (ImGui::Selectable(voxel_label(profile).c_str(), chosen)) picked = profile;
                ImGui::EndDisabled();
                if (!usable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", profile.valid() ? "Made for a different ROM" : profile.error.c_str());
                } else if (usable && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", profile.builtin ? "Ships with the hub; selecting it copies it into the game's folder"
                                                            : profile.file_name.c_str());
                }
            }
            ImGui::EndCombo();
        }
        if (picked) {
            select_voxel_profile(g, *picked);
            selected = selected_voxel_profile(g);
        }

        const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        if (ImGui::Button("Add profile...", ImVec2(third, 0))) add_voxel_profile_dialog(g);
        ImGui::SameLine();
        ImGui::BeginDisabled(g.voxel_profile.empty() || !selected);
        if (ImGui::Button("Remove profile", ImVec2(third, 0))) {
            std::string err;
            if (voxel_profiles::remove_profile(g.game_dir, g.voxel_profile, err)) {
                toast("Removed 3D profile " + g.voxel_profile);
                g.voxel_profile.clear();
                save_library();
            } else {
                toast(err, true);
            }
            refresh_voxel_profiles(g);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Profiles folder", ImVec2(third, 0))) {
            std::string err;
            const std::string dir = voxel_profiles::profiles_dir(g.game_dir);
            if (platform::ensure_directory(dir, err)) platform::open_in_file_manager(dir);
            else toast(err, true);
            refresh_voxel_profiles(g);
        }

        selected = selected_voxel_profile(g);  // the buttons may have refreshed the list
        ImGui::PushFont(font_small_);
        if (!g.voxel_profile.empty()) {
            if (!selected) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn),
                                   "The selected profile is missing; the game runs in 2D.");
            } else if (!selected->valid() || !selected->matches(g.rom_sha256)) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn),
                                   "The selected profile is unusable; the game runs in 2D.");
            } else if (g.status == "ready" && !voxel_view_supported(g)) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.warn),
                                   "This build predates the 3D view. Rebuild the game to use it.");
            } else {
                ImGui::TextDisabled("In game, F4 switches between 3D and 2D.");
            }
            if (selected && voxel_differs_from_builtin_) {
                ImGui::TextDisabled("This copy differs from the hub's built-in version.");
                if (ImGui::SmallButton("Reset to built-in")) {
                    voxel_profiles::ProfileInfo installed;
                    std::string err;
                    if (voxel_profiles::install_builtin(g.game_dir, g.voxel_profile, installed, err)) {
                        toast("Reset " + g.voxel_profile + " to the built-in version");
                    } else {
                        toast(err, true);
                    }
                    refresh_voxel_profiles(g);
                }
            }
        } else if (!voxel_profiles_.empty() && voxel_profiles_.back().builtin) {
            ImGui::TextDisabled("A built-in 3D profile is available for this game.");
        }
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4 * scale_));
    }

    void draw_context_menu(GameEntry& g) {
        if (g.status == "ready" && ImGui::MenuItem("Play")) launch(g);
        if (ImGui::MenuItem(g.favorite ? "Remove from favorites" : "Add to favorites")) {
            g.favorite = !g.favorite;
            save_library();
        }
        if (ImGui::MenuItem("Add to Steam...", nullptr, false, steam_ready(g))) request_steam_export({&g}, "games");
        if (ImGui::MenuItem("Compile for export...")) open_export_dialog({g.rom_path});
        ImGui::Separator();
        const ConsoleBackend* backend = backend_for_family(g.family_id);
        bool can_build = backend && backend->supported() && !jobs_.has_pending_for(g.id);
        if (ImGui::MenuItem("Rebuild", nullptr, false, can_build)) {
            queue_build(g);
            save_library();
        }
        if (ImGui::MenuItem("Set cover art...")) set_cover_dialog(g);
        if (ImGui::MenuItem("Capture cover from game", nullptr, false, g.status == "ready" && backend && backend->supports_cover_capture())) queue_cover_capture(g);
        if (ImGui::MenuItem("Open game folder")) platform::open_in_file_manager(g.game_dir);
        ImGui::Separator();
        if (ImGui::MenuItem("Remove from library...")) {
            confirm_remove_id_ = g.id;
            confirm_remove_files_ = false;
        }
    }

    void set_cover_dialog(GameEntry& g) {
        std::vector<std::string> paths;
        if (!platform::pick_files({{"Images", "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.ppm"}, {"All files", "*.*"}}, false, paths) || paths.empty()) return;
        std::string err;
        platform::Image probe;
        if (!platform::load_image(paths.front(), probe, err)) {
            toast(err, true);
            return;
        }
        if (!platform::ensure_directory(settings_.covers_dir, err)) {
            toast(err, true);
            return;
        }
        std::string dest = platform::join(settings_.covers_dir, g.id + platform::file_extension_lower(paths.front()));
        if (!platform::copy_file(paths.front(), dest, err)) {
            toast(err, true);
            return;
        }
        g.cover_path = dest;
        g.cover_version += 1;
        save_library();
        toast("Cover updated");
    }

    void import_save_dialog(GameEntry& g) {
        if (g.exe_path.empty()) {
            toast("Build the game first so the save location exists", true);
            return;
        }
        std::vector<std::string> paths;
        if (!platform::pick_files({{"Battery saves", "*.sav;*.srm"}, {"All files", "*.*"}}, false, paths) || paths.empty()) return;
        std::string dest = platform::join(platform::parent_path(g.exe_path), g.id + ".sav");
        std::string err;
        if (!platform::copy_file(paths.front(), dest, err)) {
            toast(err, true);
            return;
        }
        toast("Save imported to " + dest);
    }

    void draw_detail(GameEntry& g) {
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float cover_h = w * 0.9f;
        ImGui::Dummy(ImVec2(w, cover_h));
        draw_cover(dl, g, p0, ImVec2(p0.x + w, p0.y + cover_h), 8.0f * scale_);

        ImGui::PushFont(font_title_);
        ImGui::SetNextItemWidth(-1);
        if (input_string("##title", edit_title_) ) {}
        if (ImGui::IsItemDeactivatedAfterEdit() && !edit_title_.empty() && edit_title_ != g.title) {
            g.title = edit_title_;
            save_library();
        }
        ImGui::PopFont();

        const ConsoleInfo* ci = console_info(g.console_id);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(console_color(g.console_id)), "%s", ci ? ci->name.c_str() : g.console_id.c_str());
        if (!g.notes.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("· %s", g.notes.c_str());
        }
        draw_status_text(g);
        if (!g.last_error.empty()) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g.status == "unsupported" ? theme_.text_dim : theme_.error), "%s", g.last_error.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, 4 * scale_));

        // Primary actions.
        const ConsoleBackend* backend = backend_for_family(g.family_id);
        bool running = is_running(g.id);
        if (g.status == "ready") {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.55f, 0.38f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.66f, 0.46f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.48f, 0.33f, 1.0f));
            const bool preparing = preparing_.count(g.id) != 0;
            ImGui::BeginDisabled(preparing);
            if (ImGui::Button(preparing ? "Building edited data..." : running ? "Running" : "Play", ImVec2(-1, 40 * scale_))) launch(g);
            ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
            draw_link_cable_section(g);
            draw_run_record_section(g);
        } else if (g.status == "building" || g.status == "queued") {
            ImGui::BeginDisabled();
            ImGui::Button(g.status == "building" ? "Building..." : "Queued", ImVec2(-1, 40 * scale_));
            ImGui::EndDisabled();
        }
        bool can_build = backend && backend->supported() && !jobs_.has_pending_for(g.id);
        float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::BeginDisabled(!can_build);
        if (ImGui::Button(g.status == "ready" ? "Rebuild" : "Build", ImVec2(half, 0))) {
            queue_build(g);
            save_library();
            view_ = View::Jobs;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(g.favorite ? "Unfavorite" : "Favorite", ImVec2(half, 0))) {
            g.favorite = !g.favorite;
            save_library();
        }
        if (ImGui::Button("Set cover art...", ImVec2(half, 0))) set_cover_dialog(g);
        ImGui::SameLine();
        ImGui::BeginDisabled(!(g.status == "ready" && backend && backend->supports_cover_capture()));
        if (ImGui::Button("Capture cover", ImVec2(half, 0))) queue_cover_capture(g);
        ImGui::EndDisabled();
        if (ImGui::Button("Open folder", ImVec2(half, 0))) platform::open_in_file_manager(g.game_dir);
        ImGui::SameLine();
        if (ImGui::Button("Import save...", ImVec2(half, 0))) import_save_dialog(g);
        ImGui::BeginDisabled(!steam_ready(g));
        if (ImGui::Button("Add to Steam...", ImVec2(-1, 0))) request_steam_export({&g}, "games");
        ImGui::EndDisabled();
        if (ImGui::Button("Compile for export...", ImVec2(-1, 0))) open_export_dialog({g.rom_path});

        ImGui::Dummy(ImVec2(0, 6 * scale_));
        ImGui::TextDisabled("Description");
        input_string_multiline("##desc", edit_description_, ImVec2(-1, 140 * scale_));
        if (ImGui::IsItemDeactivatedAfterEdit() && edit_description_ != g.description) {
            g.description = edit_description_;
            save_library();
        }

        draw_texture_pack_section(g);
        draw_voxel_section(g);
        draw_edited_data_section(g);

        ImGui::TextDisabled("Extra launch arguments");
        ImGui::SetNextItemWidth(-1);
        input_string("##args", edit_args_);
        if (ImGui::IsItemDeactivatedAfterEdit() && edit_args_ != g.launch_args) {
            g.launch_args = edit_args_;
            save_library();
        }

        ImGui::Dummy(ImVec2(0, 6 * scale_));
        ImGui::PushFont(font_small_);
        ImGui::TextDisabled("ROM: %s (%s)", platform::file_name(g.rom_path).c_str(), format_bytes(g.rom_size).c_str());
        {
            const SizeInfo& sz = size_for(g);
            if (sz.logical > 0 && sz.on_disk > 0 && sz.on_disk < sz.logical - 4096) {
                ImGui::TextDisabled("Executable: %s (%s on disk, compressed)", format_bytes(sz.logical).c_str(), format_bytes(sz.on_disk).c_str());
            } else if (sz.logical > 0) {
                ImGui::TextDisabled("Executable: %s", format_bytes(sz.logical).c_str());
            }
        }
        if (!g.header_title.empty()) ImGui::TextDisabled("Header title: %s", g.header_title.c_str());
        ImGui::TextDisabled("Play time: %s across %lld session(s), last %s",
                            format_duration(play_seconds_including_session(g)).c_str(), g.play_count,
                            format_relative_time(g.last_played).c_str());
        ImGui::TextDisabled("Added %s", g.added_at.c_str());
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0, 10 * scale_));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button("Remove from library...", ImVec2(-1, 0))) {
            confirm_remove_id_ = g.id;
            confirm_remove_files_ = false;
        }
        ImGui::PopStyleColor(2);
    }

    // ------------------------------------------------------------------
    // Jobs view
    // ------------------------------------------------------------------

    void draw_jobs_view() {
        ImGui::Dummy(ImVec2(0, 4 * scale_));
        ImGui::Indent(14 * scale_);
        ImGui::PushFont(font_title_);
        ImGui::TextUnformatted("Jobs");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8 * scale_);
        if (ImGui::SmallButton("Clear history")) jobs_.clear_history();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Finished jobs stay listed here, with their logs, until you clear them.\n"
                              "Clearing deletes the list and the logs. Running and queued jobs stay,\n"
                              "and so does each game's own build.log.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll log", &log_autoscroll_);
        ImGui::Unindent(14 * scale_);

        // Newest first: with earlier runs' jobs kept, the one just started
        // would otherwise be at the bottom of a long list.
        std::vector<std::shared_ptr<Job>> jobs = jobs_.jobs();
        std::reverse(jobs.begin(), jobs.end());
        std::shared_ptr<Job> selected;
        for (const auto& j : jobs) {
            if (j->id == selected_job_) selected = j;
        }
        if (!selected && !jobs.empty()) {
            // Prefer the running job, otherwise the newest.
            for (const auto& j : jobs) {
                if (j->state == JobState::Running) selected = j;
            }
            if (!selected) selected = jobs.front();
            selected_job_ = selected->id;
        }

        ImGui::Indent(14 * scale_);
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##jobs", 7, flags, ImVec2(-14 * scale_, 220 * scale_))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36 * scale_);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 2.5f);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Finished", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80 * scale_);
            ImGui::TableHeadersRow();
            for (const auto& j : jobs) {
                ImGui::PushID(j->id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char label[32];
                std::snprintf(label, sizeof(label), "%d", j->id);
                // The row's selectable covers every column, the Cancel
                // button's too. AllowOverlap lets the button, drawn after it,
                // have its own clicks: without it the row took them, and
                // Cancel only ever selected the job.
                if (ImGui::Selectable(label, selected_job_ == j->id,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    selected_job_ = j->id;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(job_kind_name(j->kind));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(j->game_title.c_str());
                ImGui::TableSetColumnIndex(3);
                JobState s = j->state;
                ImU32 col = s == JobState::Succeeded ? theme_.ok : s == JobState::Failed ? theme_.error : s == JobState::Running ? theme_.warn : theme_.text_dim;
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", job_state_name(s));
                ImGui::TableSetColumnIndex(4);
                std::string step = s == JobState::Failed ? j->error_snapshot() : j->step_snapshot();
                if (s == JobState::Running) {
                    step += "  (" + format_duration(j->step_elapsed_seconds()) + ", total " +
                            format_duration(j->total_elapsed_seconds()) + ")";
                } else if (s == JobState::Succeeded) {
                    step = "done in " + format_duration(j->total_elapsed_seconds());
                } else if (s == JobState::Failed || s == JobState::Cancelled) {
                    step += "  (after " + format_duration(j->total_elapsed_seconds()) + ")";
                }
                ImGui::TextUnformatted(step.c_str());
                ImGui::TableSetColumnIndex(5);
                if (s != JobState::Queued && s != JobState::Running && j->finished_at > 0) {
                    ImGui::TextDisabled("%s", format_relative_time(j->finished_at).c_str());
                }
                ImGui::TableSetColumnIndex(6);
                if (s == JobState::Queued || s == JobState::Running) {
                    if (j->cancel) {
                        ImGui::TextDisabled("cancelling");
                    } else if (ImGui::SmallButton("Cancel")) {
                        jobs_.cancel(j->id);
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::TextDisabled("Log");
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.055f, 0.07f, 1.0f));
        ImGui::BeginChild("##joblog", ImVec2(-14 * scale_, -14 * scale_), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(font_small_);
        if (selected) {
            selected->load_history_log();  // a job of an earlier run: its log is read when first shown
            std::vector<std::string> lines = selected->log_snapshot();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(lines.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const std::string& line = lines[static_cast<size_t>(i)];
                    bool is_error = line.find("error") != std::string::npos || line.find("FAILED") != std::string::npos;
                    if (is_error) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme_.error));
                    ImGui::TextUnformatted(line.c_str());
                    if (is_error) ImGui::PopStyleColor();
                }
            }
            if (log_autoscroll_ && selected->state == JobState::Running && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
        } else {
            ImGui::TextDisabled("No jobs yet. Import a ROM to start a build.");
        }
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Unindent(14 * scale_);
    }

    // ------------------------------------------------------------------
    // Settings view
    // ------------------------------------------------------------------

    void path_field(const char* label, std::string& value, bool folder, const char* filter_label = nullptr, const char* filter_pattern = nullptr) {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-(110 * scale_));
        if (input_string("##value", value)) settings_dirty_ = true;
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(100 * scale_, 0))) {
            if (folder) {
                std::string dir;
                if (platform::pick_folder(dir)) { value = dir; settings_dirty_ = true; }
            } else {
                std::vector<std::string> paths;
                std::vector<platform::FileFilter> filters;
                if (filter_label && filter_pattern) filters.push_back({filter_label, filter_pattern});
                filters.push_back({"All files", "*.*"});
                if (platform::pick_files(filters, false, paths) && !paths.empty()) { value = paths.front(); settings_dirty_ = true; }
            }
        }
        bool ok = folder ? platform::is_directory(value) : platform::exists(value);
        ImGui::PushFont(font_small_);
        if (value.empty()) ImGui::TextDisabled("not set");
        else if (ok) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.ok), "found");
        else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.error), "missing");
        ImGui::PopFont();
        ImGui::PopID();
    }

    void draw_settings_view() {
        ImGui::Dummy(ImVec2(0, 4 * scale_));
        ImGui::Indent(14 * scale_);
        ImGui::PushFont(font_title_);
        ImGui::TextUnformatted("Settings");
        ImGui::PopFont();
        ImGui::BeginChild("##settings_scroll", ImVec2(-14 * scale_, -14 * scale_), ImGuiChildFlags_None);
        ImGui::PushItemWidth(-1);

        ImGui::SeparatorText("Locations");
        ImGui::TextDisabled("Data directory: %s", settings_.data_dir.c_str());
        path_field("Games directory (generated projects and executables)", settings_edit_.games_dir, true);
        path_field("Covers directory", settings_edit_.covers_dir, true);

        ImGui::SeparatorText("Toolchain");
        for (const auto& b : backends()) {
            const std::string label = b->recompiler_name() + " executable (" + b->family_name() + ")";
            path_field(label.c_str(), settings_edit_.recompilers[b->family_id()], false, "Executables", "*.exe");
        }
        path_field("CMake executable", settings_edit_.cmake_exe, false, "Executables", "*.exe");
        path_field("Ninja executable", settings_edit_.ninja_exe, false, "Executables", "*.exe");
#ifdef _WIN32
        path_field("MSVC environment script (vcvars64.bat)", settings_edit_.vcvars_bat, false, "Batch files", "*.bat");
        path_field("SDL2.dll", settings_edit_.sdl2_dll, false, "DLLs", "*.dll");
#endif
        path_field("SDL2 CMake directory (contains sdl2-config.cmake)", settings_edit_.sdl2_cmake_dir, true);
        path_field("Docker (Compile for export: Linux / SteamOS)", settings_edit_.docker_exe, false, "Executables", "*.exe");
        // A Game Boy game's edited data: a data tree is built with these before the game starts.
        path_field("Python (builds a Game Boy game's edited data; leave empty to find it on PATH)", settings_edit_.python_exe, false,
                   "Executables", "*.exe");
        path_field("gb_data.py (leave empty to use the one beside the hub)", settings_edit_.gb_data_tool, false, "Python scripts", "*.py");
        ImGui::TextDisabled("In use: %s, %s", data_tool().python.empty() ? "no Python found" : data_tool().python.c_str(),
                            data_tool().script.empty() ? "no gb_data.py found" : data_tool().script.c_str());
        ImGui::TextUnformatted("Build type");
        ImGui::SetNextItemWidth(200 * scale_);
        const char* build_types[] = {"Release", "MinSizeRel", "RelWithDebInfo", "Debug"};
        int bt = 0;
        for (int i = 0; i < 4; ++i) if (settings_edit_.build_type == build_types[i]) bt = i;
        if (ImGui::Combo("##buildtype", &bt, build_types, 4)) { settings_edit_.build_type = build_types[bt]; settings_dirty_ = true; }
        ImGui::TextUnformatted("Parallel compile jobs for generated sources (0 = unlimited)");
        ImGui::SetNextItemWidth(200 * scale_);
        if (ImGui::InputInt("##jobs", &settings_edit_.generated_compile_jobs)) settings_dirty_ = true;
        if (platform::supports_transparent_compression()) {
            if (ImGui::Checkbox("Compress installed executables (NTFS LZX; the game still runs directly)", &settings_edit_.compress_executables)) settings_dirty_ = true;
        }
        if (ImGui::Checkbox("Compact resume points (smaller executables; --differential single-stepping runs mostly interpreted)", &settings_edit_.compact_resume)) settings_dirty_ = true;
        ImGui::TextUnformatted("Code coverage (what the recompiler compiles; the rest runs exactly through the interpreter)");
        {
            const char* profiles[] = {"full", "balanced", "small", "minimal"};
            const char* labels[] = {
                "Full: every plausible pointer target (largest executables)",
                "Balanced: pointer targets that look like code (recommended)",
                "Small: no pointer-derived seeds; about 10% slower at worst",
                "Minimal: reachable code only; nearly everything interpreted",
            };
            int current = 1;
            for (int i = 0; i < 4; ++i) if (settings_edit_.code_coverage == profiles[i]) current = i;
            ImGui::SetNextItemWidth(520 * scale_);
            if (ImGui::Combo("##coverage", &current, labels, 4)) { settings_edit_.code_coverage = profiles[current]; settings_dirty_ = true; }
        }

        ImGui::SeparatorText("Launching");
        ImGui::TextUnformatted("Extra arguments for every Game Boy launch (e.g. --model cgb)");
        if (input_string("##gbargs", settings_edit_.gb_extra_launch_args)) settings_dirty_ = true;
        if (ImGui::Checkbox("Hide the game's console window", &settings_edit_.hide_game_console)) settings_dirty_ = true;
        {
            const char* values[] = {"", "software", "opengl", "fast"};
            const char* labels[] = {
                "GameCube renderer: the game's own setting",
                "GameCube renderer: Software (the CPU draws)",
                "GameCube renderer: OpenGL (the graphics card draws; same picture)",
                "GameCube renderer: OpenGL, fast (the card rasterises and blends; much quicker, close)",
            };
            int current = 0;
            for (int i = 0; i < 4; ++i) if (settings_edit_.gc_renderer == values[i]) current = i;
            ImGui::SetNextItemWidth(620 * scale_);
            if (ImGui::Combo("##gcrenderer", &current, labels, 4)) { settings_edit_.gc_renderer = values[current]; settings_dirty_ = true; }
            if (settings_edit_.gc_renderer == "fast") {
                ImGui::TextDisabled("A game built before the fast renderer existed will not start with it: rebuild the game.");
            }
        }
        {
            const char* labels[] = {
                "GameCube internal resolution: the game's own setting",
                "GameCube internal resolution: 1x (the console's own, 640 by 528)",
                "GameCube internal resolution: 2x (1280 by 1056)",
                "GameCube internal resolution: 3x (1920 by 1584)",
                "GameCube internal resolution: 4x (2560 by 2112)",
            };
            int current = settings_edit_.gc_internal_scale;
            if (current < 0 || current > 4) current = 0;
            ImGui::SetNextItemWidth(620 * scale_);
            if (ImGui::Combo("##gcinternalscale", &current, labels, 5)) { settings_edit_.gc_internal_scale = current; settings_dirty_ = true; }
            if (settings_edit_.gc_internal_scale > 0) {
                ImGui::TextDisabled("Used by the OpenGL, fast renderer only. A game built before it existed will not start with it: rebuild the game.");
            }
        }

        ImGui::SeparatorText("Wii");
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("A retail Wii disc is encrypted, and building a game from one needs the Wii common key once. "
                            "No key ships with this program: point this at your own - a file holding the 16 bytes, "
                            "32 hex digits, or a keys.bin dumped from your console. A built game does not need it again.");
        ImGui::PopTextWrapPos();
        ImGui::TextUnformatted("Wii common key file");
        if (input_string("##wiikey", settings_edit_.wii_key_file)) settings_dirty_ = true;
        if (ImGui::Checkbox("Try Wii games", &settings_edit_.wii_trial)) settings_dirty_ = true;
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("Wii is not supported yet: no retail game has passed its gates, and one that starts may be slow, "
                            "wrong or stop. With this on the hub builds Wii discs anyway. Discs already in the library "
                            "that were refused can then be built with Rebuild.");
        ImGui::PopTextWrapPos();

        ImGui::SeparatorText("Steam");
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("Add to Steam creates non-Steam game shortcuts that start games through this hub, with the cover art as Steam artwork. "
                            "Steam has to be closed while shortcuts change; the hub offers to close it for you.");
        ImGui::PopTextWrapPos();
        if (!steam_detect_done_) {
            steam_detect_done_ = true;
            steam_detected_root_ = steam::detect_root();
        }
        path_field("Steam folder (leave empty to detect it)", settings_edit_.steam_root, true);
        ImGui::TextDisabled("Detected: %s", steam_detected_root_.empty() ? "not found" : steam_detected_root_.c_str());
        {
            const std::string root = settings_edit_.steam_root.empty() ? steam_detected_root_ : settings_edit_.steam_root;
            if (!steam_accounts_loaded_ || root != steam_accounts_root_) {
                steam_accounts_loaded_ = true;
                steam_accounts_root_ = root;
                steam_accounts_ = steam::list_accounts(root);
            }
            ImGui::TextUnformatted("Steam account");
            const char* automatic = "Automatic (most recent sign-in)";
            const std::string preview = settings_edit_.steam_account.empty() ? std::string(automatic)
                                                                             : steam::account_label(steam_accounts_, settings_edit_.steam_account);
            ImGui::SetNextItemWidth(360 * scale_);
            if (ImGui::BeginCombo("##steamaccount", preview.c_str())) {
                if (ImGui::Selectable(automatic, settings_edit_.steam_account.empty())) {
                    settings_edit_.steam_account.clear();
                    settings_dirty_ = true;
                }
                for (const auto& a : steam_accounts_) {
                    const std::string item = steam::account_label(steam_accounts_, a.id);
                    if (ImGui::Selectable(item.c_str(), settings_edit_.steam_account == a.id)) {
                        settings_edit_.steam_account = a.id;
                        settings_dirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (steam_accounts_.empty()) ImGui::TextDisabled("No Steam accounts found in that folder.");
        }
        if (ImGui::Button("Remove this hub's games from Steam...", ImVec2(300 * scale_, 0))) {
            SteamPending p;
            p.remove = true;
            open_steam_dialog(std::move(p));
        }

        ImGui::SeparatorText("Covers");
        ImGui::TextUnformatted("Frame captured for automatic covers after a build (0 disables)");
        ImGui::TextDisabled("GameCube games run five times as far: a disc spends its first minute loading and on logos.");
        ImGui::SetNextItemWidth(200 * scale_);
        if (ImGui::InputInt("##coverframe", &settings_edit_.cover_capture_frame)) settings_dirty_ = true;

        ImGui::SeparatorText("Interface");
        ImGui::TextUnformatted("UI scale (0 = follow display, takes effect on restart)");
        ImGui::SetNextItemWidth(200 * scale_);
        if (ImGui::InputFloat("##scale", &settings_edit_.ui_scale, 0.25f, 0.5f, "%.2f")) settings_dirty_ = true;
        const char* views[] = {"Grid", "List"};
        int dv = settings_edit_.default_view == "list" ? 1 : 0;
        ImGui::TextUnformatted("Default view");
        ImGui::SetNextItemWidth(200 * scale_);
        if (ImGui::Combo("##defaultview", &dv, views, 2)) { settings_edit_.default_view = dv ? "list" : "grid"; settings_dirty_ = true; }

        ImGui::Dummy(ImVec2(0, 10 * scale_));
        ImGui::BeginDisabled(!settings_dirty_);
        if (ImGui::Button("Save settings", ImVec2(160 * scale_, 34 * scale_))) {
            settings_edit_.data_dir = settings_.data_dir;
            settings_ = settings_edit_;
            set_wii_trial(settings_.wii_trial);
            data_tool_.reset();  // Python or gb_data.py may have been set
            // A console just turned on: its games that were refused can be built.
            {
                bool changed = false;
                for (GameEntry& g : library_.games()) {
                    const ConsoleBackend* backend = backend_for_family(g.family_id);
                    if (g.status == "unsupported" && backend && backend->supported()) {
                        g.status = "failed";
                        g.last_error = backend->family_name() + " is turned on now: choose Rebuild to build this game.";
                        changed = true;
                    }
                }
                if (changed) save_library();
            }
            std::string err;
            if (settings_.save(err)) { toast("Settings saved"); settings_dirty_ = false; }
            else toast(err, true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Auto-detect toolchain", ImVec2(200 * scale_, 34 * scale_))) {
            Settings probe = settings_edit_;
            probe.recompilers.clear(); probe.cmake_exe.clear(); probe.ninja_exe.clear();
            probe.vcvars_bat.clear(); probe.sdl2_cmake_dir.clear(); probe.sdl2_dll.clear(); probe.docker_exe.clear();
            probe.detect_defaults();
            settings_edit_ = probe;
            settings_dirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert", ImVec2(100 * scale_, 34 * scale_))) {
            settings_edit_ = settings_;
            settings_dirty_ = false;
        }
        auto problems = settings_edit_.validate();
        if (problems.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.ok), "Toolchain looks complete.");
        } else {
            for (const auto& p : problems) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.error), "%s", p.c_str());
        }

        ImGui::SeparatorText("Consoles");
        for (const auto& b : backends()) {
            std::string exts;
            for (const auto& e : b->extensions()) exts += (exts.empty() ? "" : ", ") + e;
            if (b->supported()) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.ok), "%s", b->family_name().c_str());
            } else {
                ImGui::TextDisabled("%s (coming soon)", b->family_name().c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", exts.c_str());
            if (!b->supported()) {
                ImGui::PushTextWrapPos();
                ImGui::TextDisabled("%s", b->unsupported_reason().c_str());
                ImGui::PopTextWrapPos();
            }
        }

        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::Unindent(14 * scale_);
    }

    // ------------------------------------------------------------------
    // Modals and toasts
    // ------------------------------------------------------------------

    void draw_steam_picker(SteamPending& p) {
        ImGui::BeginDisabled(p.waiting_for_exit);
        auto select = [&](const std::vector<const GameEntry*>& games) {
            p.checked.clear();
            for (const GameEntry* g : games) {
                if (steam_ready(*g)) p.checked[g->id] = true;
            }
        };
        ImGui::Dummy(ImVec2(0, 4 * scale_));
        ImGui::TextUnformatted("Select:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Entire library")) select(games_in_scope(false, ""));
        const std::vector<const GameEntry*> favorites = games_in_scope(true, "");
        ImGui::SameLine();
        ImGui::BeginDisabled(favorites.empty());
        if (ImGui::SmallButton("Favorites")) select(favorites);
        ImGui::EndDisabled();
        for (const auto& c : all_consoles()) {
            const std::vector<const GameEntry*> games = games_in_scope(false, c.id);
            if (games.empty()) continue;
            ImGui::SameLine();
            if (ImGui::SmallButton((c.name + "##pick_console_" + c.id).c_str())) select(games);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) p.checked.clear();

        ImGui::BeginChild("##steam_pick", ImVec2(0, 260 * scale_), ImGuiChildFlags_Border);
        for (const auto& g : library_.games()) {
            const bool ready = steam_ready(g);
            auto it = p.checked.find(g.id);
            bool on = ready && it != p.checked.end() && it->second;
            ImGui::BeginDisabled(!ready);
            if (ImGui::Checkbox((g.title + "##pick_" + g.id).c_str(), &on)) p.checked[g.id] = on;
            ImGui::EndDisabled();
            std::string note;
            if (const ConsoleInfo* console = console_info(g.console_id)) note = console->short_name;
            if (g.favorite) note += note.empty() ? "favorite" : ", favorite";
            if (!ready) note += note.empty() ? "not built" : ", not built";
            else if (p.in_steam.count(g.id)) note += note.empty() ? "already in Steam" : ", already in Steam";
            if (!note.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", note.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::EndDisabled();

        p.ids.clear();
        int updates = 0;
        for (const auto& g : library_.games()) {
            auto it = p.checked.find(g.id);
            if (it == p.checked.end() || !it->second || !steam_ready(g)) continue;
            p.ids.push_back(g.id);
            if (p.in_steam.count(g.id)) ++updates;
        }
        std::string summary = std::to_string(p.ids.size()) + " selected";
        if (updates) summary += "; " + std::to_string(updates) + " already in Steam will be updated";
        ImGui::TextDisabled("%s", summary.c_str());
    }

    void draw_steam_modal() {
        const char* popup = "Steam##steam_dialog";
        if (steam_pending_ && !ImGui::IsPopupOpen(popup)) ImGui::OpenPopup(popup);
        ImGui::SetNextWindowSize(ImVec2(540 * scale_, 0));
        if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        if (!steam_pending_) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        SteamPending& p = *steam_pending_;
        const double now = ImGui::GetTime();
        const ImVec4 warn(1.0f, 0.75f, 0.27f, 1.0f);
        bool close = false;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520 * scale_);
        if (p.remove) {
            ImGui::TextUnformatted("Remove this hub's games from Steam?");
        } else if (p.picking) {
            ImGui::TextUnformatted("Choose the games to add to Steam");
        } else {
            ImGui::Text("Add %s to Steam?", p.label.c_str());
        }
        ImGui::Dummy(ImVec2(0, 4 * scale_));
        if (!p.error.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.error), "%s", p.error.c_str());
            ImGui::Dummy(ImVec2(0, 6 * scale_));
            if (ImGui::Button("Close", ImVec2(120 * scale_, 0))) close = true;
        } else {
            ImGui::TextDisabled("Steam account: %s", p.account_label.c_str());
            if (p.picking) draw_steam_picker(p);
            if (p.remove) {
                ImGui::TextDisabled("Only shortcuts this hub created are removed, with the artwork it wrote. Your other non-Steam games are not touched.");
            } else {
                ImGui::TextDisabled("Each shortcut starts the game through this hub, which hides the console window and records play time. The cover art becomes the Steam artwork.");
                if (p.skipped > 0) ImGui::TextDisabled("%d game(s) here are not built yet and will be skipped.", p.skipped);
            }
            if (now >= p.next_check) {
                p.next_check = now + 0.5;
                p.steam_running = steam::steam_running();
            }
            ImGui::Dummy(ImVec2(0, 6 * scale_));
            const bool nothing_selected = !p.remove && p.ids.empty();
            if (p.waiting_for_exit && !p.steam_running) {
                p.waiting_for_exit = false;
                close = apply_steam(p);
            } else if (p.waiting_for_exit && now > p.deadline) {
                p.waiting_for_exit = false;
                p.note = "Steam did not exit within a minute. Exit it from its tray icon, then try again.";
            }
            if (!close && p.error.empty()) {
                if (!p.note.empty()) ImGui::TextColored(warn, "%s", p.note.c_str());
                if (p.waiting_for_exit) {
                    ImGui::TextColored(warn, "Waiting for Steam to exit...");
                    if (ImGui::Button("Cancel", ImVec2(120 * scale_, 0))) close = true;
                } else if (p.steam_running) {
                    ImGui::TextColored(warn, "Steam is running. It has to be closed while the hub changes its shortcuts, otherwise it overwrites them when it exits.");
                    ImGui::BeginDisabled(nothing_selected);
                    if (ImGui::Button(p.remove ? "Close Steam and remove" : "Close Steam and add", ImVec2(220 * scale_, 0))) {
                        std::string err;
                        if (steam::request_shutdown(p.root, err)) {
                            p.waiting_for_exit = true;
                            p.note.clear();
                            p.deadline = now + 60.0;
                            p.next_check = now + 1.0;
                        } else {
                            p.error = "Could not ask Steam to exit: " + err;
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120 * scale_, 0))) close = true;
                } else {
                    const std::string action = p.remove    ? std::string("Remove")
                                               : p.picking ? "Add " + std::to_string(p.ids.size()) + " to Steam"
                                                           : std::string("Add to Steam");
                    ImGui::BeginDisabled(nothing_selected);
                    if (ImGui::Button(action.c_str(), ImVec2(180 * scale_, 0))) close = apply_steam(p);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120 * scale_, 0))) close = true;
                }
            }
        }
        ImGui::PopTextWrapPos();
        if (close) {
            steam_pending_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Why a game did not start, kept on the screen until it has been read.
    void draw_notice_modal() {
        const char* popup = "Not started";
        if (notice_ && !ImGui::IsPopupOpen(popup)) ImGui::OpenPopup(popup);
        ImGui::SetNextWindowSize(ImVec2(560 * scale_, 0));
        if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        if (!notice_) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.error), "%s", notice_->title.c_str());
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(notice_->text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6 * scale_));
        if (ImGui::Button("OK", ImVec2(120 * scale_, 0))) {
            notice_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void draw_modals() {
        draw_steam_modal();
        draw_export_modal();
        draw_notice_modal();
        if (!confirm_remove_id_.empty()) {
            ImGui::OpenPopup("Remove game");
        }
        ImGui::SetNextWindowSize(ImVec2(460 * scale_, 0));
        if (ImGui::BeginPopupModal("Remove game", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            GameEntry* g = library_.find(confirm_remove_id_);
            if (!g) {
                confirm_remove_id_.clear();
                ImGui::CloseCurrentPopup();
            } else {
                ImGui::PushTextWrapPos();
                ImGui::Text("Remove \"%s\" from the library?", g->title.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Checkbox("Also delete its folder (ROM copy, generated code, executable, and save file)", &confirm_remove_files_);
                if (confirm_remove_files_) {
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_.error), "The battery save in that folder will be lost.");
                }
                ImGui::Dummy(ImVec2(0, 6 * scale_));
                if (ImGui::Button("Remove", ImVec2(120 * scale_, 0))) {
                    std::string id = confirm_remove_id_;
                    std::string game_dir = g->game_dir;
                    std::string cover = g->cover_path;
                    auto it = running_.find(id);
                    if (it != running_.end()) {
                        platform::process_terminate(it->second.process);
                        platform::process_release(it->second.process);
                        running_.erase(it);
                    }
                    library_.remove(id);
                    if (confirm_remove_files_) {
                        std::string err;
                        if (!game_dir.empty() && !platform::remove_tree(game_dir, err)) toast(err, true);
                        if (!cover.empty()) platform::remove_tree(cover, err);
                    }
                    if (selected_id_ == id) selected_id_.clear();
                    save_library();
                    confirm_remove_id_.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120 * scale_, 0))) {
                    confirm_remove_id_.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    void draw_toasts() {
        double now = ImGui::GetTime();
        toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.expires_at > 0 && t.expires_at < now; }), toasts_.end());
        if (toasts_.empty()) return;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        float y = vp->WorkPos.y + vp->WorkSize.y - 16 * scale_;
        int index = 0;
        for (auto it = toasts_.rbegin(); it != toasts_.rend() && index < 5; ++it, ++index) {
            ImGui::PushFont(font_body_);
            ImVec2 ts = ImGui::CalcTextSize(it->text.c_str(), nullptr, false, 420 * scale_);
            ImGui::PopFont();
            ImVec2 size(ts.x + 28 * scale_, ts.y + 20 * scale_);
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - 16 * scale_, y - size.y), ImGuiCond_Always);
            ImGui::SetNextWindowSize(size);
            ImGui::SetNextWindowBgAlpha(0.96f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, it->error ? ImVec4(0.45f, 0.14f, 0.14f, 1.0f) : ImVec4(0.14f, 0.24f, 0.40f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale_);
            ImGui::Begin(("##toast" + std::to_string(index)).c_str(), nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::PushTextWrapPos(420 * scale_);
            ImGui::TextUnformatted(it->text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            y -= size.y + 8 * scale_;
        }
    }

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    Settings settings_;
    Settings settings_edit_;
    bool settings_dirty_ = false;
    Library library_;
    JobQueue jobs_;
    Theme theme_;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    float scale_ = 1.0f;
    ImFont* font_body_ = nullptr;
    ImFont* font_title_ = nullptr;
    ImFont* font_small_ = nullptr;
    std::string ini_path_;

    View view_ = View::Library;
    bool grid_mode_ = true;
    float card_size_ = 190.0f;
    std::string search_;
    std::string console_filter_;
    int sort_mode_ = 0;
    std::string selected_id_;
    std::string edit_title_, edit_description_, edit_args_;
    std::string link_join_address_;  // "Join" on a GBA game's page
    std::string run_record_game_;    // the game run_record_latest_ was looked up for
    std::string run_record_latest_;  // its newest recorded run's folder name
    double run_record_checked_ = -1.0;
    std::vector<texture_packs::PackInfo> texture_packs_;
    std::string texture_packs_game_;
    std::unordered_map<std::string, std::pair<long long, bool>> texture_pack_support_;
    std::vector<voxel_profiles::ProfileInfo> voxel_profiles_;  // installed, then built-ins not yet installed
    std::string voxel_profiles_game_;
    bool voxel_differs_from_builtin_ = false;  // the selected profile is an edited or older built-in copy
    std::unordered_map<std::string, std::pair<long long, bool>> voxel_support_;
    std::string edit_edited_;              // the edited data path as typed on the game's page
    edited_data::Choice edited_choice_;    // what the selected game's edited data is
    std::string edited_choice_game_;       // the game edited_choice_ was looked up for
    double edited_checked_ = -1.0;
    std::map<std::string, std::string> edited_summary_;  // game id -> what the tool built at the last start
    std::unordered_map<std::string, std::pair<long long, bool>> edited_data_support_;
    std::map<std::string, PreparedStart> preparing_;     // game id -> a start waiting for its edited data
    std::optional<edited_data::Tool> data_tool_;         // data_tool()
    std::optional<Notice> notice_;
    int selected_job_ = 0;
    bool log_autoscroll_ = true;
    std::string confirm_remove_id_;
    bool confirm_remove_files_ = false;
    std::vector<std::string> dropped_;

    std::unordered_map<std::string, SDL_Texture*> textures_;
    std::unordered_map<SDL_Texture*, ImVec2> texture_sizes_;
    std::map<std::string, RunningGame> running_;
    std::unordered_map<std::string, SizeInfo> sizes_;
    std::vector<Toast> toasts_;

    // "Compile for export" dialog, shown while `open` is set.
    struct ExportDialog {
        bool open = false;
        std::vector<std::string> sources;  // files or folders the user chose
        std::vector<std::string> roms;     // ROM files found in them
        std::map<ExportTarget, bool> checked;
        std::string dest;
    };
    ExportDialog export_;

    std::optional<SteamPending> steam_pending_;
    bool steam_detect_done_ = false;
    std::string steam_detected_root_;
    bool steam_accounts_loaded_ = false;
    std::string steam_accounts_root_;
    std::vector<steam::Account> steam_accounts_;
    Uint32 last_ingest_ticks_ = 0;
    std::set<std::string> rebuild_with_profile_;  // game ids (start_profile_rebuilds)
    std::set<std::string> profile_attempted_;     // game ids (queue_missing_profile)
};

void print_usage() {
    std::string targets;
    for (const auto& info : export_target_infos()) targets += (targets.empty() ? "" : ", ") + std::string(info.id);
    std::printf(
        "Native Console Hub %s\n"
        "Usage: native_console_hub [--data-dir <dir>] [--import <rom-or-folder>]... [--batch] [--rebuild-all | --rebuild <game-id>...]\n"
        "       native_console_hub [--data-dir <dir>] --export <rom-or-folder>... --export-to <dir> [--export-targets <list>]\n"
        "       native_console_hub [--data-dir <dir>] --launch <game-id>\n"
        "       native_console_hub [--data-dir <dir>] --steam-add <what> | --steam-remove [--steam-root <dir>] [--steam-user <id>]\n"
        "  --data-dir <dir>   Library, covers, settings, and generated games live here\n"
        "  --import <path>    Import a ROM or a folder of ROMs at startup (repeatable)\n"
        "  --batch            No window: run imports and queued builds, then exit (0 = all succeeded)\n"
        "  --rebuild-all      With --batch, rebuild every supported game in the library\n"
        "  --rebuild <id>     With --batch, rebuild one game (repeatable)\n"
        "  --data-tree <id>   With --batch, take a Game Boy game's ROM apart into files to edit, in\n"
        "                     <data-dir>/data-trees/<id>, and start the game with them from then on (repeatable)\n"
        "  --export <path>    Compile a ROM or a folder of ROMs for other computers, then exit (repeatable)\n"
        "  --export-to <dir>  Destination folder for --export\n"
        "  --export-targets <list>  Comma-separated %s (default: the last selection)\n"
        "  --launch <id>      Start one game (used by Steam shortcuts), wait for it, and record play time;\n"
        "                     with --batch, a game that cannot be started is reported here and not in a message box\n"
        "  --steam-add <what> Add games to Steam and exit: all, favorites, console:<id>, or game:<id>\n"
        "  --steam-remove     Remove every shortcut this hub added to Steam and exit\n"
        "  --steam-root <dir> Steam folder for --steam-add/--steam-remove (default: detected)\n"
        "  --steam-user <id>  Steam account id, the userdata folder name (default: most recent sign-in)\n",
        HUB_VERSION, targets.c_str());
}

}  // namespace

int hub_main(int argc, char** argv) {
    std::string data_dir;
    std::vector<std::string> imports;
    bool batch = false;
    bool rebuild_all = false;
    std::string launch_id, steam_add, steam_root, steam_user;
    bool steam_remove = false;
    std::vector<std::string> exports, rebuilds, data_trees;
    std::string export_dest, export_targets_arg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--import" && i + 1 < argc) imports.push_back(argv[++i]);
        else if (arg == "--batch") batch = true;
        else if (arg == "--rebuild-all") rebuild_all = true;
        else if (arg == "--rebuild" && i + 1 < argc) rebuilds.push_back(argv[++i]);
        else if (arg == "--data-tree" && i + 1 < argc) data_trees.push_back(argv[++i]);
        else if (arg == "--export" && i + 1 < argc) exports.push_back(argv[++i]);
        else if (arg == "--export-to" && i + 1 < argc) export_dest = argv[++i];
        else if (arg == "--export-targets" && i + 1 < argc) export_targets_arg = argv[++i];
        else if (arg == "--launch" && i + 1 < argc) launch_id = argv[++i];
        else if (arg == "--steam-add" && i + 1 < argc) steam_add = argv[++i];
        else if (arg == "--steam-remove") steam_remove = true;
        else if (arg == "--steam-root" && i + 1 < argc) steam_root = argv[++i];
        else if (arg == "--steam-user" && i + 1 < argc) steam_user = argv[++i];
        else if (arg == "--help" || arg == "-h") { print_usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); print_usage(); return 2; }
    }
    if (data_dir.empty()) data_dir = platform::getenv_utf8("NATIVE_CONSOLE_HUB_DATA");
    if (data_dir.empty()) data_dir = platform::default_data_dir();
    data_dir = platform::absolute(data_dir);

    std::string err;
    if (!platform::ensure_directory(data_dir, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    Settings settings;
    settings.data_dir = data_dir;
    if (!settings.load(err)) std::fprintf(stderr, "%s\n", err.c_str());
    settings.detect_defaults();
    set_wii_trial(settings.wii_trial);
    // A Steam launch only reads settings; a GUI hub may be editing them.
    if (launch_id.empty() && !settings.save(err)) std::fprintf(stderr, "%s\n", err.c_str());
    platform::ensure_directory(settings.games_dir, err);
    platform::ensure_directory(settings.covers_dir, err);

    Library library;
    library.set_path(platform::join(data_dir, "library.json"));
    if (!library.load(err)) std::fprintf(stderr, "%s\n", err.c_str());
    // Games imported while their console was not supported yet become
    // buildable once it is (Rebuild in the game's menu).
    for (GameEntry& g : library.games()) {
        const ConsoleBackend* backend = backend_for_family(g.family_id);
        if (g.status == "unsupported" && backend && backend->supported()) {
            g.status = "failed";
            g.last_error = backend->family_name() + " is supported now: choose Rebuild to build this game.";
        }
        // Jobs do not outlive the hub, so a game still marked queued or
        // building was interrupted by the last hub closing. Left alone it
        // would sit "Queued" with nothing to run it. A Steam launch skips
        // this: a hub window may be building that game right now.
        if (launch_id.empty() && (g.status == "queued" || g.status == "building")) {
            if (!g.exe_path.empty() && platform::exists(g.exe_path)) {
                g.status = "ready";
                g.last_error = "The last build was interrupted when the hub closed; the previous build is still installed.";
            } else {
                g.status = "failed";
                g.last_error = "The build was interrupted when the hub closed: choose Build to start it again.";
            }
        }
    }

    std::vector<ExportTarget> export_targets;
    if (!exports.empty()) {
        if (export_dest.empty()) {
            std::fprintf(stderr, "--export needs --export-to <dir>\n");
            return 2;
        }
        std::vector<std::string> unknown;
        export_targets = parse_export_targets(export_targets_arg.empty() ? settings.export_targets : export_targets_arg, &unknown);
        if (!unknown.empty() || export_targets.empty()) {
            std::fprintf(stderr, "--export-targets: use windows, linux and/or macos\n");
            return 2;
        }
        batch = true;
    }

    std::printf("[hub] data dir: %s\n", data_dir.c_str());
    App app(std::move(settings), std::move(library));
    if (!launch_id.empty()) return app.run_launch(launch_id, batch);
    if (!steam_add.empty() || steam_remove) return app.run_steam_cli(steam_add, steam_remove, steam_root, steam_user);
    return batch ? app.run_batch(imports, rebuild_all, rebuilds, exports, export_targets, export_dest, data_trees)
                 : app.run_gui(imports);
}

}  // namespace hub

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Batch mode talks to the parent console; GUI mode logs to hub.log in the data directory.
    bool batch = false;
    bool launch = false;
    for (int i = 1; i < __argc; ++i) {
        if (std::strcmp(__argv[i], "--batch") == 0 || std::strcmp(__argv[i], "--help") == 0 || std::strcmp(__argv[i], "-h") == 0 ||
            std::strcmp(__argv[i], "--steam-add") == 0 || std::strcmp(__argv[i], "--steam-remove") == 0 ||
            std::strcmp(__argv[i], "--export") == 0) batch = true;
        if (std::strcmp(__argv[i], "--launch") == 0) launch = true;
    }
    HANDLE inherited_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    bool have_stdout = inherited_stdout != nullptr && inherited_stdout != INVALID_HANDLE_VALUE;
    if (batch && have_stdout) {
        // Launched with a redirected or inherited stdout (a pipe, a file, or a console): keep it.
    } else if (batch && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    } else {
        std::string dir = hub::platform::getenv_utf8("NATIVE_CONSOLE_HUB_DATA");
        for (int i = 1; i + 1 < __argc; ++i) {
            if (std::strcmp(__argv[i], "--data-dir") == 0) dir = __argv[i + 1];
        }
        if (dir.empty()) dir = hub::platform::default_data_dir();
        std::string ignore;
        hub::platform::ensure_directory(dir, ignore);
        // A Steam launch must not truncate the log of a GUI hub that is open.
        std::string log = hub::platform::join(dir, launch ? "launch.log" : "hub.log");
        FILE* f = nullptr;
        freopen_s(&f, log.c_str(), "w", stdout);
        freopen_s(&f, log.c_str(), "a", stderr);
    }
    return hub::hub_main(__argc, __argv);
}
#else
int main(int argc, char** argv) {
    return hub::hub_main(argc, argv);
}
#endif
