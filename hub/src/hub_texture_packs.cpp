#include "hub_texture_packs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace hub::texture_packs {

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

std::string file_name(const std::string& path) {
    return from_path(to_path(path).filename());
}

constexpr uint32_t kMagic = 0x50544247u;     // 'GBTP'
constexpr uint32_t kGbaMagic = 0x50544147u;  // 'GATP'
constexpr uint32_t kVersion = 1u;
constexpr size_t kHeaderSize = 184u;
constexpr size_t kGbaHeaderSize = 192u;
constexpr size_t kEntrySize = 36u;
constexpr size_t kGbaEntrySize = 76u;
constexpr uint32_t kMaxScale = 8u;
constexpr uint32_t kMaxEntries = 4096u;
constexpr uint32_t kGbaMaxEntries = 65536u;

const char* extension_for(const std::string& family) {
    return family == "gba" ? ".gbatp" : ".gbtp";
}
constexpr const char* kFlag = "--texture-pack";

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool read_string(const uint8_t* p, size_t capacity, std::string& out) {
    const auto* end = static_cast<const uint8_t*>(std::memchr(p, 0, capacity));
    if (!end || end == p) return false;
    out.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(end - p));
    return true;
}

std::string safe_file_stem(const std::string& pack_id) {
    std::string stem;
    for (unsigned char c : pack_id) {
        stem.push_back(std::isalnum(c) || c == '.' || c == '_' || c == '-' ? static_cast<char>(c) : '_');
    }
    while (!stem.empty() && stem.front() == '.') stem.erase(stem.begin());
    return stem.empty() ? "texture-pack" : stem;
}

}  // namespace

PackInfo parse_header(const uint8_t* data, size_t length, long long file_size) {
    PackInfo info;
    info.size = file_size;
    const bool gba = length >= 4 && read_le32(data) == kGbaMagic;
    if (length < (gba ? kGbaHeaderSize : kHeaderSize) || (!gba && read_le32(data) != kMagic)) {
        info.error = "not a texture pack";
        return info;
    }
    info.family = gba ? "gba" : "gb";
    if (read_le32(data + 4) != kVersion) {
        info.error = "unsupported texture pack version " + std::to_string(read_le32(data + 4));
        return info;
    }
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        info.rom_sha256.push_back(hex[data[8 + i] >> 4]);
        info.rom_sha256.push_back(hex[data[8 + i] & 15]);
    }
    info.scale = read_le32(data + 44);
    info.entries = read_le32(data + 48);
    if (info.scale < 1 || info.scale > kMaxScale || info.entries < 1 || info.entries > (gba ? kGbaMaxEntries : kMaxEntries)) {
        info.error = "texture pack exceeds limits";
        return info;
    }
    if (!read_string(data + 56, 64, info.pack_id) || !read_string(data + 120, 64, info.license)) {
        info.error = "texture pack header is malformed";
        return info;
    }
    const long long side = 8LL * info.scale;
    const long long expected =
        gba ? static_cast<long long>(kGbaHeaderSize) + static_cast<long long>(read_le32(data + 52)) +
                  static_cast<long long>(info.entries) * (static_cast<long long>(kGbaEntrySize) + side * side * 4)
            : static_cast<long long>(kHeaderSize) +
                  static_cast<long long>(info.entries) * (static_cast<long long>(kEntrySize) + side * side * 4);
    if (file_size != expected) {
        info.error = "texture pack is truncated or has extra data";
    }
    return info;
}

PackInfo read_pack(const std::string& path) {
    PackInfo info;
    std::error_code ec;
    const auto size = fs::file_size(to_path(path), ec);
    std::ifstream in(to_path(path), std::ios::binary);
    std::vector<uint8_t> header(kGbaHeaderSize);
    if (ec || !in) {
        info.error = "cannot read file";
    } else {
        in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        info = parse_header(header.data(), static_cast<size_t>(in.gcount()), static_cast<long long>(size));
    }
    info.path = path;
    info.file_name = file_name(path);
    return info;
}

std::string packs_dir(const std::string& game_dir) {
    return join(game_dir, "texture-packs");
}

std::vector<PackInfo> list_packs(const std::string& game_dir, const std::string& family) {
    std::vector<PackInfo> packs;
    std::error_code ec;
    for (fs::directory_iterator it(to_path(packs_dir(game_dir)), ec), end; !ec && it != end; it.increment(ec)) {
        std::string extension = from_path(it->path().extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::error_code file_ec;
        if (extension == extension_for(family) && it->is_regular_file(file_ec)) {
            PackInfo pack = read_pack(from_path(it->path()));
            if (pack.valid() && pack.family != family) pack.error = "texture pack is for another console";
            packs.push_back(pack);
        }
    }
    std::sort(packs.begin(), packs.end(), [](const PackInfo& a, const PackInfo& b) { return a.file_name < b.file_name; });
    return packs;
}

bool import_pack(const std::string& game_dir, const std::string& source_path,
                 const std::string& rom_sha256, PackInfo& installed, std::string& error,
                 const std::string& family) {
    PackInfo source = read_pack(source_path);
    if (!source.valid()) {
        error = file_name(source_path) + ": " + source.error;
        return false;
    }
    if (source.family != family) {
        error = "\"" + source.pack_id + "\" is a texture pack for another console";
        return false;
    }
    if (!source.fits_rom(rom_sha256)) {
        error = "\"" + source.pack_id + "\" was made for a different ROM";
        return false;
    }
    const std::string dest = join(packs_dir(game_dir), safe_file_stem(source.pack_id) + extension_for(family));
    std::error_code ec;
    if (!fs::equivalent(to_path(dest), to_path(source_path), ec)) {
        ec.clear();
        fs::create_directories(to_path(packs_dir(game_dir)), ec);
        if (!ec) fs::copy_file(to_path(source_path), to_path(dest), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy the pack into " + packs_dir(game_dir) + ": " + ec.message();
            return false;
        }
    }
    installed = read_pack(dest);
    if (!installed.valid()) {
        error = "the copied pack is unusable: " + installed.error;
        return false;
    }
    return true;
}

bool remove_pack(const std::string& game_dir, const std::string& pack_file, std::string& error) {
    if (pack_file.empty() || pack_file != file_name(pack_file)) {
        error = "invalid texture pack name";
        return false;
    }
    std::error_code ec;
    fs::remove(to_path(join(packs_dir(game_dir), pack_file)), ec);
    if (ec) {
        error = "cannot remove " + pack_file + ": " + ec.message();
        return false;
    }
    return true;
}

bool executable_supports_texture_packs(const std::string& exe_path) {
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
                                     const std::string& rom_sha256, const std::string& family) {
    if (selected_file.empty() || selected_file != file_name(selected_file)) return {};
    PackInfo pack = read_pack(join(packs_dir(game_dir), selected_file));
    if (!pack.valid() || pack.family != family || !pack.fits_rom(rom_sha256)) return {};
    return {kFlag, pack.path};
}

}  // namespace hub::texture_packs
