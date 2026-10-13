#include "hub_voxel_profiles.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <sstream>

namespace hub::voxel_profiles {

namespace {

namespace fs = std::filesystem;

// UTF-8 path helpers, kept local so this file builds without the platform layer.
fs::path to_path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.c_str()), utf8.size()));
}

std::string from_path(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

std::string join(const std::string& a, const std::string& b) {
    return from_path(to_path(a) / to_path(b));
}

std::string base_name(const std::string& path) {
    return from_path(to_path(path).filename());
}

constexpr const char* kFlag = "--voxel-profile";
constexpr long long kMaxBytes = 1 << 20;  // the runtime's limit

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool is_sha256(const std::string& s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool read_text(const std::string& path, std::string& text, std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(to_path(path), ec);
    if (ec) {
        error = "cannot read file";
        return false;
    }
    if (static_cast<long long>(size) > kMaxBytes) {
        error = "file is too large for a voxel profile";
        return false;
    }
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in) {
        error = "cannot read file";
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

bool write_text(const std::string& path, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(to_path(path).parent_path(), ec);
    std::ofstream out(to_path(path), std::ios::binary | std::ios::trunc);
    if (ec || !out) {
        error = "cannot write " + path;
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

constexpr const char* kBuiltinMarker = "# hub-builtin ";

std::string content_hash(const std::string& text) {
    uint64_t hash = 1469598103934665603ULL;  // FNV-1a
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

std::string builtin_copy_text(const BuiltinProfile& builtin) {
    return std::string(builtin.text) + kBuiltinMarker + content_hash(builtin.text) + "\n";
}

const BuiltinProfile* find_builtin(const std::string& file_name) {
    for (const BuiltinProfile& builtin : builtin_profiles()) {
        if (file_name == builtin.file_name) return &builtin;
    }
    return nullptr;
}

bool safe_file_name(const std::string& name) {
    return !name.empty() && name == base_name(name) && name.front() != '.';
}

}  // namespace

bool ProfileInfo::matches(const std::string& rom_sha256) const {
    return roms.empty() || std::find(roms.begin(), roms.end(), lower(rom_sha256)) != roms.end();
}

ProfileInfo parse_profile(const std::string& text, const std::string& file_name) {
    ProfileInfo info;
    info.file_name = file_name;
    info.title = file_name;
    std::istringstream lines(text);
    std::string line;
    bool header = false;
    while (std::getline(lines, line)) {
        const size_t hash = line.find('#');
        line = trim(hash == std::string::npos ? line : line.substr(0, hash));
        if (line.empty()) continue;
        if (!header) {
            if (line != "voxel-profile 1") {
                info.error = "not a voxel profile";
                return info;
            }
            header = true;
            continue;
        }
        if (line.rfind("title ", 0) == 0) {
            info.title = trim(line.substr(6));
        } else if (line.rfind("rom ", 0) == 0) {
            const std::string sha = trim(line.substr(4));
            if (!is_sha256(sha)) {
                info.error = "malformed rom line";
                return info;
            }
            info.roms.push_back(lower(sha));
        }
    }
    if (!header) info.error = "not a voxel profile";
    return info;
}

ProfileInfo read_profile(const std::string& path) {
    std::string text, error;
    ProfileInfo info;
    if (read_text(path, text, error)) {
        info = parse_profile(text, base_name(path));
    } else {
        info.file_name = base_name(path);
        info.title = info.file_name;
        info.error = error;
    }
    info.path = path;
    return info;
}

std::string profiles_dir(const std::string& game_dir) {
    return join(game_dir, "voxel-profiles");
}

std::vector<ProfileInfo> list_profiles(const std::string& game_dir) {
    std::vector<ProfileInfo> profiles;
    std::error_code ec;
    for (fs::directory_iterator it(to_path(profiles_dir(game_dir)), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code file_ec;
        if (lower(from_path(it->path().extension())) == ".voxel" && it->is_regular_file(file_ec)) {
            profiles.push_back(read_profile(from_path(it->path())));
        }
    }
    std::sort(profiles.begin(), profiles.end(),
              [](const ProfileInfo& a, const ProfileInfo& b) { return a.file_name < b.file_name; });
    return profiles;
}

std::vector<ProfileInfo> available_builtins(const std::string& game_dir, const std::string& rom_sha256) {
    std::vector<ProfileInfo> out;
    std::error_code ec;
    for (const BuiltinProfile& builtin : builtin_profiles()) {
        ProfileInfo info = parse_profile(builtin.text, builtin.file_name);
        info.builtin = true;
        if (!info.valid() || info.roms.empty() || !info.matches(rom_sha256)) continue;
        if (fs::exists(to_path(join(profiles_dir(game_dir), builtin.file_name)), ec)) continue;
        out.push_back(info);
    }
    return out;
}

bool install_builtin(const std::string& game_dir, const std::string& file_name,
                     ProfileInfo& installed, std::string& error) {
    const BuiltinProfile* builtin = find_builtin(file_name);
    if (!builtin) {
        error = "no built-in profile named " + file_name;
        return false;
    }
    const std::string dest = join(profiles_dir(game_dir), file_name);
    if (!write_text(dest, builtin_copy_text(*builtin), error)) return false;
    installed = read_profile(dest);
    return true;
}

int update_builtin_copies(const std::string& game_dir) {
    int updated = 0;
    for (const BuiltinProfile& builtin : builtin_profiles()) {
        const std::string path = join(profiles_dir(game_dir), builtin.file_name);
        std::string text, error;
        if (!read_text(path, text, error)) continue;
        const std::string current = builtin_copy_text(builtin);
        if (text == current) continue;
        const size_t marker = text.rfind(kBuiltinMarker);
        if (marker == std::string::npos) continue;  // not written by the hub
        const std::string body = text.substr(0, marker);
        const std::string recorded = trim(text.substr(marker + std::strlen(kBuiltinMarker)));
        if (recorded != content_hash(body)) continue;  // edited since it was installed
        if (write_text(path, current, error)) updated++;
    }
    return updated;
}

bool differs_from_builtin(const std::string& game_dir, const std::string& file_name) {
    const BuiltinProfile* builtin = find_builtin(file_name);
    if (!builtin) return false;
    std::string text, error;
    if (!read_text(join(profiles_dir(game_dir), file_name), text, error)) return false;
    return text != builtin_copy_text(*builtin);
}

bool import_profile(const std::string& game_dir, const std::string& source_path,
                    const std::string& rom_sha256, ProfileInfo& installed, std::string& error) {
    ProfileInfo source = read_profile(source_path);
    if (!source.valid()) {
        error = source.file_name + ": " + source.error;
        return false;
    }
    if (!source.matches(rom_sha256)) {
        error = "\"" + source.title + "\" was made for a different ROM";
        return false;
    }
    std::string name = source.file_name;
    if (lower(from_path(to_path(name).extension())) != ".voxel") name += ".voxel";
    const std::string dest = join(profiles_dir(game_dir), name);
    std::error_code ec;
    if (!fs::equivalent(to_path(dest), to_path(source_path), ec)) {
        ec.clear();
        fs::create_directories(to_path(profiles_dir(game_dir)), ec);
        if (!ec) fs::copy_file(to_path(source_path), to_path(dest), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy the profile into " + profiles_dir(game_dir) + ": " + ec.message();
            return false;
        }
    }
    installed = read_profile(dest);
    if (!installed.valid()) {
        error = "the copied profile is unusable: " + installed.error;
        return false;
    }
    return true;
}

bool remove_profile(const std::string& game_dir, const std::string& file_name, std::string& error) {
    if (!safe_file_name(file_name)) {
        error = "invalid profile name";
        return false;
    }
    std::error_code ec;
    fs::remove(to_path(join(profiles_dir(game_dir), file_name)), ec);
    if (ec) {
        error = "cannot remove " + file_name + ": " + ec.message();
        return false;
    }
    return true;
}

bool executable_supports_voxel_view(const std::string& exe_path) {
    std::ifstream in(to_path(exe_path), std::ios::binary);
    if (!in) return false;
    const std::string needle = kFlag;
    std::vector<char> buffer(1 << 20);
    std::string carry;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        std::string chunk = carry;
        chunk.append(buffer.data(), static_cast<size_t>(got));
        if (chunk.find(needle) != std::string::npos) return true;
        carry = chunk.substr(chunk.size() >= needle.size() ? chunk.size() - needle.size() + 1 : 0);
    }
    return false;
}

std::vector<std::string> launch_args(const std::string& game_dir, const std::string& selected_file,
                                     const std::string& rom_sha256) {
    if (!safe_file_name(selected_file)) return {};
    ProfileInfo profile = read_profile(join(profiles_dir(game_dir), selected_file));
    if (!profile.valid() || !profile.matches(rom_sha256)) return {};
    return {kFlag, profile.path};
}

}  // namespace hub::voxel_profiles
