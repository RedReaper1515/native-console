#include "hub_library.h"

#include "hub_json.h"
#include "hub_platform.h"

extern "C" {
#include "gbrt_hash.h"
}

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace hub {

namespace {

GameEntry entry_from_json(const Json& j) {
    GameEntry e;
    e.id = j.get_string("id");
    e.title = j.get_string("title");
    e.console_id = j.get_string("console_id");
    e.family_id = j.get_string("family_id");
    e.description = j.get_string("description");
    e.notes = j.get_string("notes");
    e.header_title = j.get_string("header_title");
    e.cover_path = j.get_string("cover_path");
    e.rom_path = j.get_string("rom_path");
    e.rom_original_path = j.get_string("rom_original_path");
    e.rom_sha256 = j.get_string("rom_sha256");
    e.game_dir = j.get_string("game_dir");
    e.exe_path = j.get_string("exe_path");
    e.status = j.get_string("status");
    e.last_error = j.get_string("last_error");
    e.launch_args = j.get_string("launch_args");
    e.texture_pack = j.get_string("texture_pack");
    e.voxel_profile = j.get_string("voxel_profile");
    e.edited_data = j.get_string("edited_data");
    e.added_at = j.get_string("added_at");
    e.favorite = j.get_bool("favorite");
    e.rom_size = j.get_int("rom_size");
    e.last_played = j.get_int("last_played");
    e.play_count = j.get_int("play_count");
    e.play_seconds = j.get_int("play_seconds");
    e.cover_version = j.get_int("cover_version");
    return e;
}

Json entry_to_json(const GameEntry& e) {
    Json j = Json::make_object();
    j.set("id", e.id);
    j.set("title", e.title);
    j.set("console_id", e.console_id);
    j.set("family_id", e.family_id);
    j.set("description", e.description);
    j.set("notes", e.notes);
    j.set("header_title", e.header_title);
    j.set("cover_path", e.cover_path);
    j.set("rom_path", e.rom_path);
    j.set("rom_original_path", e.rom_original_path);
    j.set("rom_sha256", e.rom_sha256);
    j.set("game_dir", e.game_dir);
    j.set("exe_path", e.exe_path);
    j.set("status", e.status);
    j.set("last_error", e.last_error);
    j.set("launch_args", e.launch_args);
    j.set("texture_pack", e.texture_pack);
    j.set("voxel_profile", e.voxel_profile);
    j.set("added_at", e.added_at);
    j.set("favorite", e.favorite);
    j.set("rom_size", e.rom_size);
    j.set("last_played", e.last_played);
    j.set("play_count", e.play_count);
    j.set("play_seconds", e.play_seconds);
    j.set("cover_version", e.cover_version);
    // Only for a game that has some: every other entry is written as it always was.
    if (!e.edited_data.empty()) j.set("edited_data", e.edited_data);
    return j;
}

}  // namespace

bool Library::load(std::string& error) {
    games_.clear();
    std::string text;
    if (!read_text_file(path_, text)) return true;  // empty library on first run
    Json root;
    if (!Json::parse(text, root, error)) {
        error = "library.json: " + error;
        return false;
    }
    const Json* games = root.get("games");
    if (!games || !games->is_array()) return true;
    for (const auto& g : games->array) {
        GameEntry e = entry_from_json(g);
        if (!e.id.empty()) games_.push_back(std::move(e));
    }
    return true;
}

bool Library::save(std::string& error) const {
    Json root = Json::make_object();
    root.set("schema", "native-console.hub.library");
    root.set("schema_version", 1);
    Json games = Json::make_array();
    for (const auto& g : games_) games.push(entry_to_json(g));
    root.set("games", std::move(games));
    return write_text_file_atomic(path_, root.dump(2), error);
}

GameEntry* Library::find(const std::string& id) {
    for (auto& g : games_) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

const GameEntry* Library::find(const std::string& id) const {
    for (const auto& g : games_) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

const GameEntry* Library::find_by_hash(const std::string& sha256) const {
    if (sha256.empty()) return nullptr;
    for (const auto& g : games_) {
        if (g.rom_sha256 == sha256) return &g;
    }
    return nullptr;
}

bool Library::remove(const std::string& id) {
    auto it = std::remove_if(games_.begin(), games_.end(), [&](const GameEntry& g) { return g.id == id; });
    if (it == games_.end()) return false;
    games_.erase(it, games_.end());
    return true;
}

std::string Library::make_unique_id(const std::string& preferred_base) const {
    std::string base = slugify(preferred_base);
    if (base.empty()) base = "game";
    if (base.size() > 40) base.resize(40);
    while (!base.empty() && base.back() == '_') base.pop_back();
    if (base.empty() || std::isdigit(static_cast<unsigned char>(base.front()))) base = "g_" + base;
    std::string candidate = base;
    int n = 2;
    while (find(candidate) != nullptr) {
        candidate = base + "_" + std::to_string(n++);
    }
    return candidate;
}

std::string slugify(const std::string& text) {
    std::string out;
    bool last_sep = true;
    for (unsigned char c : text) {
        if (c == 0x27 || c == 0xE2 || c == 0x80 || c == 0x99 || c == 0x98) continue;  // drop ASCII and UTF-8 apostrophes
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            last_sep = false;
        } else if (!last_sep) {
            out.push_back('_');
            last_sep = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

std::string sha256_hex_of_file(const std::string& path) {
    std::vector<uint8_t> data;
    if (!platform::read_binary_file(path, data)) return "";
    uint8_t digest[32];
    gbrt_sha256(data.data(), data.size(), digest);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

std::string format_bytes(long long bytes) {
    char buf[64];
    if (bytes < 0) return "?";
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%lld B", bytes);
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.0f KiB", bytes / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f MiB", bytes / (1024.0 * 1024.0));
    return buf;
}

std::string format_duration(long long seconds) {
    if (seconds <= 0) return "0 min";
    if (seconds < 60) return std::to_string(seconds) + " s";
    long long minutes = seconds / 60;
    if (minutes < 60) return std::to_string(minutes) + " min";
    long long hours = minutes / 60;
    minutes %= 60;
    return std::to_string(hours) + " h " + std::to_string(minutes) + " min";
}

std::string format_relative_time(long long unix_seconds) {
    if (unix_seconds <= 0) return "never";
    long long delta = platform::now_unix() - unix_seconds;
    if (delta < 60) return "just now";
    if (delta < 3600) return std::to_string(delta / 60) + " min ago";
    if (delta < 86400) return std::to_string(delta / 3600) + " h ago";
    if (delta < 86400 * 30) return std::to_string(delta / 86400) + " d ago";
    return std::to_string(delta / (86400 * 30)) + " mo ago";
}

}  // namespace hub
