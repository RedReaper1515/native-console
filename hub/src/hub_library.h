// Game library model persisted to library.json in the hub data directory.
#pragma once

#include <string>
#include <vector>

namespace hub {

struct GameEntry {
    std::string id;           // slug, also the generated target/file prefix
    std::string title;
    std::string console_id;   // "gb", "gbc", "gba"
    std::string family_id;    // backend family, "gb" or "gba"
    std::string description;
    std::string notes;        // header-derived hints
    std::string header_title;
    std::string cover_path;   // absolute path to an image, empty when none
    std::string rom_path;     // copy managed by the hub
    std::string rom_original_path;
    std::string rom_sha256;
    std::string game_dir;
    std::string exe_path;
    std::string status;       // "queued", "building", "ready", "failed", "unsupported"
    std::string last_error;
    std::string launch_args;  // extra per-game arguments (space separated)
    std::string texture_pack; // selected file in game_dir/texture-packs, empty = original art
    std::string voxel_profile; // selected file in game_dir/voxel-profiles, empty = 2D
    std::string edited_data;  // a data tree folder or a built image the game starts with (hub_edited_data.h), empty = its own data
    std::string added_at;
    bool favorite = false;
    long long rom_size = 0;
    long long last_played = 0;  // unix seconds, 0 = never
    long long play_count = 0;
    long long play_seconds = 0;  // accumulated time the game was running
    long long cover_version = 0;  // bumped when the cover file changes so textures reload
};

class Library {
public:
    void set_path(const std::string& path) { path_ = path; }
    const std::string& path() const { return path_; }

    bool load(std::string& error);
    bool save(std::string& error) const;

    std::vector<GameEntry>& games() { return games_; }
    const std::vector<GameEntry>& games() const { return games_; }

    GameEntry* find(const std::string& id);
    const GameEntry* find(const std::string& id) const;
    const GameEntry* find_by_hash(const std::string& sha256) const;
    bool remove(const std::string& id);

    // Builds a unique identifier that is a valid C identifier from a human title.
    std::string make_unique_id(const std::string& preferred_base) const;

private:
    std::string path_;
    std::vector<GameEntry> games_;
};

std::string slugify(const std::string& text);
std::string sha256_hex_of_file(const std::string& path);
std::string format_bytes(long long bytes);
std::string format_relative_time(long long unix_seconds);
std::string format_duration(long long seconds);

}  // namespace hub
