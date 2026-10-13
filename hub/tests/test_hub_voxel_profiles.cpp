// Unit tests for the hub's 3D view profiles: built-ins, import, and launch arguments.
#include "hub_voxel_profiles.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hub;

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

const std::string kLinksAwakening = "ed42628d5cd8c73a6f8c6a8965f19674876a4fa04e616834e26b81b35963ad87";
const std::string kOtherRom(64, 'b');

std::string join(const std::string& a, const std::string& b) {
    return (std::filesystem::path(a) / b).string();
}

void write_file(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

}  // namespace

int main() {
    const std::string root = (std::filesystem::temp_directory_path() / "hub_voxel_profiles_test").string();
    std::filesystem::remove_all(root);
    const std::string game = join(root, "game");
    std::filesystem::create_directories(game);

    // Parsing reads only the header, title and rom lines.
    auto parsed = voxel_profiles::parse_profile(
        "voxel-profile 1\n# comment\ntitle  My view \nrom " + std::string(64, 'A') + "\ngrid D711 10 8 16\n", "x.voxel");
    CHECK(parsed.valid());
    CHECK(parsed.title == "My view");
    CHECK(parsed.roms.size() == 1 && parsed.roms[0] == std::string(64, 'a'));
    CHECK(parsed.matches(std::string(64, 'A')));
    CHECK(!parsed.matches(kOtherRom));
    CHECK(!voxel_profiles::parse_profile("grid D711\n", "y.voxel").valid());
    CHECK(!voxel_profiles::parse_profile("voxel-profile 1\nrom 1234\n", "z.voxel").valid());
    CHECK(voxel_profiles::parse_profile("voxel-profile 1\n", "any.voxel").matches(kOtherRom));

    // The Link's Awakening profile is built in and offered only for its ROM.
    bool found = false;
    for (const auto& builtin : voxel_profiles::builtin_profiles()) {
        if (std::string(builtin.file_name) == "links_awakening.voxel") found = true;
    }
    CHECK(found);
    auto offered = voxel_profiles::available_builtins(game, kLinksAwakening);
    CHECK(offered.size() == 1 && offered[0].builtin && offered[0].file_name == "links_awakening.voxel");
    CHECK(voxel_profiles::available_builtins(game, kOtherRom).empty());

    // Installing a built-in writes it into the game's folder; it is no longer offered.
    voxel_profiles::ProfileInfo installed;
    std::string error;
    CHECK(voxel_profiles::install_builtin(game, "links_awakening.voxel", installed, error));
    CHECK(installed.valid() && !installed.builtin && installed.matches(kLinksAwakening));
    CHECK(std::filesystem::exists(join(voxel_profiles::profiles_dir(game), "links_awakening.voxel")));
    CHECK(voxel_profiles::available_builtins(game, kLinksAwakening).empty());
    CHECK(voxel_profiles::list_profiles(game).size() == 1);
    CHECK(!voxel_profiles::install_builtin(game, "nope.voxel", installed, error));
    const std::string copy = join(voxel_profiles::profiles_dir(game), "links_awakening.voxel");
    CHECK(!voxel_profiles::differs_from_builtin(game, "links_awakening.voxel"));
    CHECK(voxel_profiles::update_builtin_copies(game) == 0);

    // An unedited copy from an older hub is updated; an edited or unmarked one is kept.
    auto fnv = [](const std::string& text) {
        uint64_t hash = 1469598103934665603ULL;
        for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
        return std::string(hex);
    };
    const std::string old_body = "voxel-profile 1\ntitle Old\nrom " + kLinksAwakening + "\n";
    write_file(copy, old_body + "# hub-builtin " + fnv(old_body) + "\n");
    CHECK(voxel_profiles::differs_from_builtin(game, "links_awakening.voxel"));
    CHECK(voxel_profiles::update_builtin_copies(game) == 1);
    CHECK(!voxel_profiles::differs_from_builtin(game, "links_awakening.voxel"));
    CHECK(voxel_profiles::read_profile(copy).title != "Old");
    write_file(copy, old_body + "grid D711 10 8 16\n# hub-builtin " + fnv(old_body) + "\n");  // edited after install
    CHECK(voxel_profiles::update_builtin_copies(game) == 0);
    write_file(copy, old_body);  // installed before the marker existed
    CHECK(voxel_profiles::update_builtin_copies(game) == 0);
    CHECK(voxel_profiles::differs_from_builtin(game, "links_awakening.voxel"));
    CHECK(voxel_profiles::install_builtin(game, "links_awakening.voxel", installed, error));  // reset
    CHECK(!voxel_profiles::differs_from_builtin(game, "links_awakening.voxel"));

    // Launch arguments only for an installed, usable profile made for this ROM.
    auto args = voxel_profiles::launch_args(game, "links_awakening.voxel", kLinksAwakening);
    CHECK(args.size() == 2 && args[0] == "--voxel-profile" && args[1] == installed.path);
    CHECK(voxel_profiles::launch_args(game, "links_awakening.voxel", kOtherRom).empty());
    CHECK(voxel_profiles::launch_args(game, "", kLinksAwakening).empty());
    CHECK(voxel_profiles::launch_args(game, "missing.voxel", kLinksAwakening).empty());
    CHECK(voxel_profiles::launch_args(game, "../links_awakening.voxel", kLinksAwakening).empty());

    // Import checks the ROM; a profile without rom lines fits any game.
    const std::string mine = join(root, "incoming/mine.voxel");
    write_file(mine, "voxel-profile 1\ntitle Mine\ngrid D711 10 8 16\nroom FFF6\n");
    CHECK(voxel_profiles::import_profile(game, mine, kOtherRom, installed, error));
    CHECK(installed.title == "Mine" && installed.file_name == "mine.voxel");
    const std::string foreign = join(root, "incoming/foreign.voxel");
    write_file(foreign, "voxel-profile 1\nrom " + kLinksAwakening + "\n");
    CHECK(!voxel_profiles::import_profile(game, foreign, kOtherRom, installed, error));
    CHECK(error.find("different ROM") != std::string::npos);
    const std::string junk = join(root, "incoming/junk.voxel");
    write_file(junk, "hello\n");
    CHECK(!voxel_profiles::import_profile(game, junk, kOtherRom, installed, error));
    write_file(join(voxel_profiles::profiles_dir(game), "broken.voxel"), "nope\n");
    const auto listed = voxel_profiles::list_profiles(game);
    CHECK(listed.size() == 3 && listed[0].file_name == "broken.voxel" && !listed[0].valid());

    CHECK(voxel_profiles::remove_profile(game, "mine.voxel", error));
    CHECK(!std::filesystem::exists(join(voxel_profiles::profiles_dir(game), "mine.voxel")));
    CHECK(!voxel_profiles::remove_profile(game, "../game", error));

    // Executables advertise support by carrying the flag string.
    const std::string new_exe = join(root, "new.exe");
    const std::string old_exe = join(root, "old.exe");
    write_file(new_exe, std::string(3 << 20, 'x') + "--voxel-profile" + std::string(100, 'x'));
    write_file(old_exe, std::string(3 << 20, 'x') + "--texture-pack");
    CHECK(voxel_profiles::executable_supports_voxel_view(new_exe));
    CHECK(!voxel_profiles::executable_supports_voxel_view(old_exe));

    std::filesystem::remove_all(root);
    if (failures == 0) std::printf("hub voxel profiles: ok\n");
    return failures == 0 ? 0 : 1;
}
