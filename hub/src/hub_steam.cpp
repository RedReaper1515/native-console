// File and process side of "Add to Steam". Formats and artwork live in hub_steam_vdf.cpp.
#include "hub_steam.h"

#include "hub_json.h"
#include "hub_library.h"
#include "hub_platform.h"
#include "hub_steam_vdf.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace hub::steam {

namespace fs = std::filesystem;

namespace {

constexpr unsigned long long kSteamId64Base = 76561197960265728ULL;

std::string config_dir(const std::string& root, const std::string& account) {
    return platform::join(platform::join(platform::join(root, "userdata"), account), "config");
}

bool load_shortcuts(const std::string& path, VdfNode& root, std::string& error) {
    root = VdfNode{};
    if (!platform::exists(path)) return true;
    std::vector<uint8_t> bytes;
    if (!platform::read_binary_file(path, bytes)) {
        error = "cannot read " + path;
        return false;
    }
    std::string parse_error;
    if (!parse_binary_vdf(bytes, root, parse_error)) {
        error = "cannot parse " + path + " (" + parse_error + "); it was left untouched";
        return false;
    }
    return true;
}

bool save_shortcuts(const std::string& path, const VdfNode& root, std::string& error) {
    if (platform::exists(path)) {
        std::string ignore;
        platform::copy_file(path, path + ".native-console.bak", ignore);
    }
    const std::vector<uint8_t> bytes = write_binary_vdf(root);
    return write_text_file_atomic(path, std::string(bytes.begin(), bytes.end()), error);
}

// Artwork the hub wrote, by path and content hash. A file whose hash no longer
// matches was replaced by the user in Steam and is never overwritten or deleted.
struct ArtworkManifest {
    std::string path;
    Json files = Json::make_object();

    void load() {
        std::string text, error;
        Json doc;
        if (!read_text_file(path, text) || !Json::parse(text, doc, error)) return;
        if (const Json* f = doc.get("files"); f && f->is_object()) files = *f;
    }

    bool save(std::string& error) const {
        Json doc = Json::make_object();
        doc.set("schema", "native-console.hub.steam-artwork");
        doc.set("schema_version", 1);
        doc.set("files", files);
        return write_text_file_atomic(path, doc.dump(2), error);
    }

    bool ours(const std::string& file) const {
        const Json* hash = files.get(file);
        return hash && hash->is_string() && hash->string == sha256_hex_of_file(file);
    }

    void forget(const std::string& file) {
        files.object.erase(std::remove_if(files.object.begin(), files.object.end(),
                                          [&](const auto& kv) { return kv.first == file; }),
                           files.object.end());
    }
};

enum class ArtResult { Written, KeptUserFile, Failed };

ArtResult write_artwork(ArtworkManifest& manifest, const std::string& file, const platform::Image& image, std::string& warning) {
    if (platform::exists(file)) {
        if (!manifest.ours(file)) return ArtResult::KeptUserFile;
        std::string ignore;
        platform::remove_tree(file, ignore);
    }
    std::string error;
    if (!platform::save_image_png(file, image, error)) {
        warning = error;
        return ArtResult::Failed;
    }
    manifest.files.set(file, sha256_hex_of_file(file));
    return ArtResult::Written;
}

// Grid file names Steam looks for, keyed by the shortcut's app id.
std::vector<std::string> artwork_files(const std::string& grid, uint32_t app_id) {
    const std::string base = platform::join(grid, std::to_string(app_id));
    return {base + "p.png", base + ".png", base + "_hero.png", base + "_icon.png"};
}

}  // namespace

std::string detect_root() {
    return platform::steam_install_dir();
}

std::vector<Account> list_accounts(const std::string& root) {
    std::vector<Account> accounts;
    if (root.empty()) return accounts;
    std::error_code ec;
    const fs::path userdata = platform::to_path(platform::join(root, "userdata"));
    for (auto it = fs::directory_iterator(userdata, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string name = platform::from_path(it->path().filename());
        const bool numeric = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        if (!numeric || name == "0") continue;
        Account account;
        account.id = name;
        accounts.push_back(account);
    }

    // Display names come from config/loginusers.vdf, keyed by 64-bit SteamID.
    std::string text, error;
    TextVdf doc;
    if (read_text_file(platform::join(platform::join(root, "config"), "loginusers.vdf"), text) && parse_text_vdf(text, doc, error)) {
        if (const TextVdf* users = doc.find("users")) {
            for (const TextVdf& user : users->children) {
                const unsigned long long id64 = std::strtoull(user.key.c_str(), nullptr, 10);
                if (id64 < kSteamId64Base) continue;
                const std::string id = std::to_string(id64 - kSteamId64Base);
                for (Account& account : accounts) {
                    if (account.id != id) continue;
                    if (const TextVdf* v = user.find("PersonaName")) account.persona = v->value;
                    if (const TextVdf* v = user.find("MostRecent")) account.most_recent = v->value == "1";
                    if (const TextVdf* v = user.find("Timestamp")) account.timestamp = std::strtoll(v->value.c_str(), nullptr, 10);
                }
            }
        }
    }
    std::sort(accounts.begin(), accounts.end(), [](const Account& a, const Account& b) { return a.id < b.id; });
    return accounts;
}

std::string pick_account(const std::vector<Account>& accounts, const std::string& preferred) {
    if (!preferred.empty()) {
        for (const Account& a : accounts) {
            if (a.id == preferred) return a.id;
        }
    }
    for (const Account& a : accounts) {
        if (a.most_recent) return a.id;
    }
    const Account* best = nullptr;
    for (const Account& a : accounts) {
        if (!best || a.timestamp > best->timestamp) best = &a;
    }
    return best ? best->id : "";
}

std::string account_label(const std::vector<Account>& accounts, const std::string& id) {
    for (const Account& a : accounts) {
        if (a.id == id) return a.persona.empty() ? "Account " + id : a.persona + " (" + id + ")";
    }
    return "Account " + id;
}

bool steam_running() {
#ifdef _WIN32
    return platform::process_running_by_name("steam.exe");
#else
    return platform::process_running_by_name("steam");
#endif
}

bool request_shutdown(const std::string& root, std::string& error) {
#ifdef _WIN32
    const std::string exe = platform::join(root, "steam.exe");
#else
    std::string exe = platform::find_on_path("steam");
    if (exe.empty()) exe = platform::join(root, "steam.sh");
#endif
    if (!platform::exists(exe)) {
        error = "cannot find " + exe;
        return false;
    }
    platform::Process process;
    if (!platform::launch_detached(exe, {"-shutdown"}, root, true, process, error)) return false;
    platform::process_release(process);
    return true;
}

bool export_games(const ExportRequest& request, ExportReport& report, std::string& error) {
    if (request.hub_exe.empty()) {
        error = "cannot determine the hub executable path";
        return false;
    }
    if (request.root.empty() || request.account.empty()) {
        error = "no Steam folder or account selected";
        return false;
    }
    const std::string config = config_dir(request.root, request.account);
    const std::string grid = platform::join(config, "grid");
    if (!platform::ensure_directory(grid, error)) return false;
    const std::string vdf_path = platform::join(config, "shortcuts.vdf");

    VdfNode root;
    if (!load_shortcuts(vdf_path, root, error)) return false;

    const std::string hub_name = platform::file_name(request.hub_exe);
    std::vector<ShortcutSpec> specs;
    for (const ExportGame& game : request.games) {
        ShortcutSpec spec;
        spec.game_id = game.id;
        spec.app_name = game.title;
        spec.exe = request.hub_exe;
        spec.start_dir = platform::parent_path(request.hub_exe);
        spec.launch_options = "--data-dir " + platform::quote_arg(request.data_dir) + " --launch " + game.id;
        spec.tags = {"Native Console"};
        if (!game.console_name.empty()) spec.tags.push_back(game.console_name);
        specs.push_back(std::move(spec));
    }
    const UpsertOutcome outcome = upsert_shortcuts(root, specs, hub_name);
    report.added = outcome.added;
    report.updated = outcome.updated;

    ArtworkManifest manifest;
    manifest.path = request.artwork_manifest;
    manifest.load();
    for (const ExportGame& game : request.games) {
        auto id = outcome.app_ids.find(game.id);
        if (id == outcome.app_ids.end()) continue;
        const std::vector<std::string> files = artwork_files(grid, id->second);
        platform::Image cover;
        std::string image_error;
        if (!game.cover_path.empty() && platform::load_image(game.cover_path, cover, image_error)) {
            const platform::Image images[] = {
                compose_capsule(cover, 600, 900),    // library capsule
                compose_capsule(cover, 920, 430),    // wide capsule (recent games)
                compose_capsule(cover, 1920, 620),   // hero banner on the game page
                compose_icon(cover),
            };
            for (size_t i = 0; i < files.size(); ++i) {
                std::string warning;
                switch (write_artwork(manifest, files[i], images[i], warning)) {
                    case ArtResult::Written: ++report.artwork_written; break;
                    case ArtResult::KeptUserFile: ++report.artwork_kept; break;
                    case ArtResult::Failed: report.warnings.push_back(game.title + ": " + warning); break;
                }
            }
        } else if (!game.cover_path.empty()) {
            report.warnings.push_back(game.title + ": " + image_error);
        }
        if (VdfNode* shortcut = find_hub_shortcut(root, game.id, hub_name)) {
            const std::string& icon = files.back();
            if (platform::exists(icon)) {
                shortcut->set_string("icon", icon);
            } else if (shortcut->get_string("icon").empty()) {
                shortcut->set_string("icon", request.hub_exe);
            }
        }
    }

    if (!save_shortcuts(vdf_path, root, error)) return false;
    std::string manifest_error;
    if (!manifest.save(manifest_error)) report.warnings.push_back(manifest_error);
    return true;
}

bool remove_games(const RemoveRequest& request, int& removed, std::string& error) {
    removed = 0;
    const std::string config = config_dir(request.root, request.account);
    const std::string vdf_path = platform::join(config, "shortcuts.vdf");
    if (!platform::exists(vdf_path)) return true;
    VdfNode root;
    if (!load_shortcuts(vdf_path, root, error)) return false;
    const std::vector<uint32_t> app_ids = remove_shortcuts(root, request.ids, platform::file_name(request.hub_exe));
    removed = static_cast<int>(app_ids.size());
    if (app_ids.empty()) return true;

    ArtworkManifest manifest;
    manifest.path = request.artwork_manifest;
    manifest.load();
    const std::string grid = platform::join(config, "grid");
    for (uint32_t app_id : app_ids) {
        for (const std::string& file : artwork_files(grid, app_id)) {
            if (platform::exists(file) && manifest.ours(file)) {
                std::string ignore;
                platform::remove_tree(file, ignore);
            }
            manifest.forget(file);
        }
    }
    if (!save_shortcuts(vdf_path, root, error)) return false;
    std::string manifest_error;
    manifest.save(manifest_error);
    return true;
}

std::set<std::string> games_in_steam(const std::string& root, const std::string& account, const std::string& hub_exe) {
    std::set<std::string> ids;
    VdfNode shortcuts;
    std::string error;
    if (!load_shortcuts(platform::join(config_dir(root, account), "shortcuts.vdf"), shortcuts, error)) return ids;
    const VdfNode* list = shortcuts.find("shortcuts");
    if (!list) return ids;
    const std::string hub_name = platform::file_name(hub_exe);
    for (const VdfNode& shortcut : list->children) {
        if (shortcut.type != VdfNode::Map) continue;
        const std::string id = hub_game_id_of(shortcut, hub_name);
        if (!id.empty()) ids.insert(id);
    }
    return ids;
}

}  // namespace hub::steam
