// Texture packs for Game Boy (.gbtp) and Game Boy Advance (.gbatp) games: the
// packs installed for a game, importing a pack, and the launch arguments that
// select one. The runtimes own the formats and validate every entry; the hub
// reads only the header to show and match packs. See TEXTURE_PACKS.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hub::texture_packs {

struct PackInfo {
    std::string family;      // "gb" or "gba", from the header's magic
    std::string path;        // absolute path of the pack file
    std::string file_name;   // name inside the game's texture-packs folder
    std::string pack_id;
    std::string license;
    std::string rom_sha256;  // lowercase hex; all zeros in a GBA pack made for any ROM
    uint32_t scale = 0;
    uint32_t entries = 0;
    long long size = 0;
    std::string error;       // why the file is unusable, empty when usable

    bool valid() const { return error.empty(); }
    bool fits_rom(const std::string& rom) const {
        return rom_sha256 == rom || (family == "gba" && rom_sha256 == std::string(64, '0'));
    }
};

// Parses the header (184 bytes for .gbtp, 192 for .gbatp) and checks it
// against the file size.
PackInfo parse_header(const uint8_t* data, size_t length, long long file_size);
PackInfo read_pack(const std::string& path);

// games/<id>/texture-packs
std::string packs_dir(const std::string& game_dir);

// Installed packs for a console family, sorted by file name. Unusable files
// are listed with an error.
std::vector<PackInfo> list_packs(const std::string& game_dir, const std::string& family = "gb");

// Copies a pack into the game's folder after checking that it is a texture
// pack for this ROM. Reimporting a pack with the same ID replaces it.
bool import_pack(const std::string& game_dir, const std::string& source_path,
                 const std::string& rom_sha256, PackInfo& installed, std::string& error,
                 const std::string& family = "gb");

bool remove_pack(const std::string& game_dir, const std::string& file_name, std::string& error);

// True when the executable was generated with --texture-pack support. Older
// games ignore unknown arguments, so the hub checks before relying on it.
bool executable_supports_texture_packs(const std::string& exe_path);

// {"--texture-pack", path} for the selected pack when it is installed, usable
// and made for this ROM; empty otherwise.
std::vector<std::string> launch_args(const std::string& game_dir, const std::string& selected_file,
                                     const std::string& rom_sha256, const std::string& family = "gb");

}  // namespace hub::texture_packs
