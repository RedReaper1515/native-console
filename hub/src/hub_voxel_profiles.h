// 3D view profiles for Game Boy games: the profiles installed for a game, the
// profiles built into the hub, importing a .voxel file, and the launch
// arguments that select one. The runtime owns the format and validates every
// line; the hub reads only the header, title and ROM lines. See VOXEL_VIEW.md.
#pragma once

#include <string>
#include <vector>

namespace hub::voxel_profiles {

struct ProfileInfo {
    std::string path;                  // absolute path; empty for a built-in not yet installed
    std::string file_name;             // name inside the game's voxel-profiles folder
    std::string title;                 // from the `title` line, else the file name
    std::vector<std::string> roms;     // lowercase SHA-256 from `rom` lines; empty = any ROM
    bool builtin = false;              // shipped with the hub and not installed yet
    std::string error;                 // why the file is unusable, empty when usable

    bool valid() const { return error.empty(); }
    bool matches(const std::string& rom_sha256) const;
};

struct BuiltinProfile {
    const char* file_name;
    const char* text;
};

// Profiles compiled into the hub from hub/voxel-profiles (generated source).
const std::vector<BuiltinProfile>& builtin_profiles();

ProfileInfo parse_profile(const std::string& text, const std::string& file_name);
ProfileInfo read_profile(const std::string& path);

// games/<id>/voxel-profiles
std::string profiles_dir(const std::string& game_dir);

// Installed profiles, sorted by file name. Unusable files are listed with an error.
std::vector<ProfileInfo> list_profiles(const std::string& game_dir);

// Built-in profiles for this ROM whose file is not installed for the game.
std::vector<ProfileInfo> available_builtins(const std::string& game_dir, const std::string& rom_sha256);

// Writes a built-in profile into the game's folder (so it can be edited there),
// ending with a "# hub-builtin <hash>" line that records what was written.
// Also used to reset an edited copy.
bool install_builtin(const std::string& game_dir, const std::string& file_name,
                     ProfileInfo& installed, std::string& error);

// Replaces installed copies of built-in profiles that nobody edited (their
// hub-builtin hash still matches) with the hub's current version. Returns how
// many files were updated.
int update_builtin_copies(const std::string& game_dir);

// True when `file_name` is a built-in profile whose installed copy differs from
// the hub's current version (edited, or installed by an older hub).
bool differs_from_builtin(const std::string& game_dir, const std::string& file_name);

// Copies a profile into the game's folder after checking it is usable for this ROM.
bool import_profile(const std::string& game_dir, const std::string& source_path,
                    const std::string& rom_sha256, ProfileInfo& installed, std::string& error);

bool remove_profile(const std::string& game_dir, const std::string& file_name, std::string& error);

// True when the executable was generated with --voxel-profile support.
bool executable_supports_voxel_view(const std::string& exe_path);

// {"--voxel-profile", path} for the selected profile when it is installed,
// usable and made for this ROM; empty otherwise.
std::vector<std::string> launch_args(const std::string& game_dir, const std::string& selected_file,
                                     const std::string& rom_sha256);

}  // namespace hub::voxel_profiles
