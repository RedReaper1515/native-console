// Unit tests for the hub's texture pack listing, import, and launch arguments.
#include "hub_texture_packs.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hub;

// Test-local path helpers; the module under test does not use the platform layer.
namespace paths {
std::string join(const std::string& a, const std::string& b) {
    return (std::filesystem::path(a) / b).string();
}
bool exists(const std::string& path) { return std::filesystem::exists(path); }
}  // namespace paths

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

const std::string kRomA(64, 'a');
const std::string kRomB(64, 'b');

void put_le32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

std::vector<uint8_t> make_pack(const std::string& rom_hex, const std::string& pack_id, uint32_t scale, uint32_t entries) {
    const size_t side = 8u * scale;
    std::vector<uint8_t> data(184 + entries * (36 + side * side * 4), 0);
    put_le32(data, 0, 0x50544247u);
    put_le32(data, 4, 1);
    for (size_t i = 0; i < 32; ++i) data[8 + i] = static_cast<uint8_t>(std::stoi(rom_hex.substr(i * 2, 2), nullptr, 16));
    put_le32(data, 40, 32768);
    put_le32(data, 44, scale);
    put_le32(data, 48, entries);
    std::memcpy(data.data() + 56, pack_id.data(), pack_id.size());
    std::memcpy(data.data() + 120, "CC0-1.0", 7);
    return data;
}

void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void test_parse_header() {
    auto good = make_pack(kRomA, "org.example.hd", 2, 3);
    auto info = texture_packs::parse_header(good.data(), good.size(), static_cast<long long>(good.size()));
    CHECK(info.valid());
    CHECK(info.pack_id == "org.example.hd");
    CHECK(info.license == "CC0-1.0");
    CHECK(info.rom_sha256 == kRomA);
    CHECK(info.scale == 2 && info.entries == 3);

    CHECK(!texture_packs::parse_header(good.data(), good.size(), static_cast<long long>(good.size()) - 1).valid());
    CHECK(!texture_packs::parse_header(good.data(), 100, static_cast<long long>(good.size())).valid());
    auto bad = good;
    bad[0] = 'X';
    CHECK(texture_packs::parse_header(bad.data(), bad.size(), static_cast<long long>(bad.size())).error == "not a texture pack");
    bad = good;
    put_le32(bad, 44, 9);
    CHECK(!texture_packs::parse_header(bad.data(), bad.size(), static_cast<long long>(bad.size())).valid());
    bad = good;
    std::memset(bad.data() + 56, 'x', 64);  // pack ID without a terminator
    CHECK(!texture_packs::parse_header(bad.data(), bad.size(), static_cast<long long>(bad.size())).valid());
}

void test_install_and_launch(const std::string& root) {
    const std::string game = paths::join(root, "games/test_game");
    const std::string incoming = paths::join(root, "incoming");
    write_file(paths::join(incoming, "hd.gbtp"), make_pack(kRomA, "org.example/hd pack", 1, 2));
    write_file(paths::join(incoming, "other.gbtp"), make_pack(kRomB, "org.example.other", 1, 1));

    CHECK(texture_packs::list_packs(game).empty());

    texture_packs::PackInfo installed;
    std::string error;
    CHECK(!texture_packs::import_pack(game, paths::join(incoming, "other.gbtp"), kRomA, installed, error));
    CHECK(error.find("different ROM") != std::string::npos);
    CHECK(!texture_packs::import_pack(game, paths::join(root, "missing.gbtp"), kRomA, installed, error));

    error.clear();
    CHECK(texture_packs::import_pack(game, paths::join(incoming, "hd.gbtp"), kRomA, installed, error));
    CHECK(installed.file_name == "org.example_hd_pack.gbtp");
    CHECK(paths::exists(paths::join(texture_packs::packs_dir(game), installed.file_name)));
    // Reimporting the same pack ID replaces the installed file instead of adding a copy.
    write_file(paths::join(incoming, "hd.gbtp"), make_pack(kRomA, "org.example/hd pack", 1, 3));
    CHECK(texture_packs::import_pack(game, paths::join(incoming, "hd.gbtp"), kRomA, installed, error));
    CHECK(installed.entries == 3);

    // Unusable files are listed with an error; files in subfolders are ignored.
    write_file(paths::join(texture_packs::packs_dir(game), "broken.gbtp"), {1, 2, 3});
    write_file(paths::join(texture_packs::packs_dir(game), "old/nested.gbtp"), make_pack(kRomA, "nested", 1, 1));
    auto packs = texture_packs::list_packs(game);
    CHECK(packs.size() == 2);
    if (packs.size() == 2) {
        CHECK(packs[0].file_name == "broken.gbtp" && !packs[0].valid());
        CHECK(packs[1].file_name == installed.file_name && packs[1].valid());
    }

    auto args = texture_packs::launch_args(game, installed.file_name, kRomA);
    CHECK(args.size() == 2 && args[0] == "--texture-pack" && args[1] == installed.path);
    CHECK(texture_packs::launch_args(game, installed.file_name, kRomB).empty());
    CHECK(texture_packs::launch_args(game, "broken.gbtp", kRomA).empty());
    CHECK(texture_packs::launch_args(game, "", kRomA).empty());
    CHECK(texture_packs::launch_args(game, "../../incoming/hd.gbtp", kRomA).empty());

    CHECK(!texture_packs::remove_pack(game, "../hd.gbtp", error));
    CHECK(texture_packs::remove_pack(game, installed.file_name, error));
    CHECK(texture_packs::launch_args(game, installed.file_name, kRomA).empty());
}

// A .gbatp header: 192 bytes, 76-byte entries after a palette block.
std::vector<uint8_t> make_gba_pack(const std::string& rom_hex, const std::string& pack_id, uint32_t scale,
                                   uint32_t entries, uint32_t palette_bytes) {
    const size_t side = 8u * scale;
    std::vector<uint8_t> data(192 + palette_bytes + entries * (76 + side * side * 4), 0);
    put_le32(data, 0, 0x50544147u);
    put_le32(data, 4, 1);
    for (size_t i = 0; i < 32; ++i) data[8 + i] = static_cast<uint8_t>(std::stoi(rom_hex.substr(i * 2, 2), nullptr, 16));
    put_le32(data, 44, scale);
    put_le32(data, 48, entries);
    put_le32(data, 52, palette_bytes);
    std::memcpy(data.data() + 56, pack_id.data(), pack_id.size());
    std::memcpy(data.data() + 120, "CC0-1.0", 7);
    return data;
}

void test_gba_packs(const std::string& root) {
    const std::string any_rom(64, '0');
    auto pack = make_gba_pack(kRomA, "org.example.gba", 4, 5000, 64);
    auto info = texture_packs::parse_header(pack.data(), pack.size(), static_cast<long long>(pack.size()));
    CHECK(info.valid() && info.family == "gba" && info.entries == 5000 && info.scale == 4);
    CHECK(info.fits_rom(kRomA) && !info.fits_rom(kRomB));
    CHECK(!texture_packs::parse_header(pack.data(), pack.size(), static_cast<long long>(pack.size()) + 1).valid());
    auto any = make_gba_pack(any_rom, "org.example.any", 1, 1, 32);
    auto any_info = texture_packs::parse_header(any.data(), any.size(), static_cast<long long>(any.size()));
    CHECK(any_info.valid() && any_info.fits_rom(kRomB));

    const std::string game = paths::join(root, "games/gba_game");
    const std::string incoming = paths::join(root, "incoming");
    write_file(paths::join(incoming, "hd.gbatp"), pack);
    write_file(paths::join(incoming, "gb.gbtp"), make_pack(kRomA, "org.example.gb", 1, 1));
    texture_packs::PackInfo installed;
    std::string error;
    CHECK(!texture_packs::import_pack(game, paths::join(incoming, "gb.gbtp"), kRomA, installed, error, "gba"));
    CHECK(error.find("another console") != std::string::npos);
    CHECK(texture_packs::import_pack(game, paths::join(incoming, "hd.gbatp"), kRomA, installed, error, "gba"));
    CHECK(installed.file_name == "org.example.gba.gbatp");
    CHECK(texture_packs::list_packs(game, "gba").size() == 1);
    CHECK(texture_packs::list_packs(game, "gb").empty());
    auto args = texture_packs::launch_args(game, installed.file_name, kRomA, "gba");
    CHECK(args.size() == 2 && args[0] == "--texture-pack");
    CHECK(texture_packs::launch_args(game, installed.file_name, kRomA, "gb").empty());
    CHECK(texture_packs::launch_args(game, installed.file_name, kRomB, "gba").empty());
}

void test_executable_support(const std::string& root) {
    // Place the flag across the scanner's 1 MiB read boundary.
    std::vector<uint8_t> exe((1u << 20) + 64, 'z');
    const char* flag = "--texture-pack";
    std::memcpy(exe.data() + (1u << 20) - 5, flag, std::strlen(flag));
    const std::string with = paths::join(root, "new.exe");
    write_file(with, exe);
    CHECK(texture_packs::executable_supports_texture_packs(with));

    const std::string without = paths::join(root, "old.exe");
    write_file(without, std::vector<uint8_t>(4096, 'z'));
    CHECK(!texture_packs::executable_supports_texture_packs(without));
    CHECK(!texture_packs::executable_supports_texture_packs(paths::join(root, "missing.exe")));
}

}  // namespace

int main() {
    const auto root_path = std::filesystem::temp_directory_path() / "hub_texture_packs_test";
    std::filesystem::remove_all(root_path);
    std::filesystem::create_directories(root_path);
    const std::string root = root_path.string();

    test_parse_header();
    test_install_and_launch(root);
    test_gba_packs(root);
    test_executable_support(root);

    std::filesystem::remove_all(root_path);
    if (failures == 0) std::printf("hub texture pack tests passed\n");
    return failures == 0 ? 0 : 1;
}
