// Steam file formats and artwork for the hub's "Add to Steam" feature.
// Pure data transforms with no file or process access, so they are unit tested
// (hub/tests/test_hub_steam.cpp). hub_steam.cpp does the I/O.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "hub_platform.h"  // platform::Image

namespace hub::steam {

// Binary KeyValues, the format of userdata/<account>/config/shortcuts.vdf.
// Every value type Steam writes is kept, so a parse/write cycle reproduces the
// file byte for byte; unknown types make parsing fail instead of losing data.
struct VdfNode {
    enum Type : uint8_t {
        Map = 0x00,
        String = 0x01,
        Int32 = 0x02,
        Float32 = 0x03,
        Pointer = 0x04,
        Color = 0x06,
        UInt64 = 0x07,
        Int64 = 0x0A,
    };

    uint8_t type = Map;
    std::string key;
    std::string str;                // String
    uint32_t i32 = 0;               // Int32, Float32 bits, Pointer, Color
    uint64_t u64 = 0;               // UInt64, Int64
    std::vector<VdfNode> children;  // Map

    // Keys are matched case-insensitively; Steam has written both "AppName" and "appname".
    VdfNode* find(const std::string& k);
    const VdfNode* find(const std::string& k) const;
    VdfNode& set_string(const std::string& k, const std::string& v);
    VdfNode& set_int(const std::string& k, uint32_t v);
    VdfNode& child_map(const std::string& k);
    std::string get_string(const std::string& k) const;
};

// The root is a Map whose children are the file's top-level entries ("shortcuts").
bool parse_binary_vdf(const std::vector<uint8_t>& bytes, VdfNode& root, std::string& error);
std::vector<uint8_t> write_binary_vdf(const VdfNode& root);

// A non-Steam shortcut the way the hub wants it. Paths are unquoted.
struct ShortcutSpec {
    std::string game_id;  // hub game id, recovered later from LaunchOptions
    std::string app_name;
    std::string exe;
    std::string start_dir;
    std::string launch_options;
    std::string icon;  // empty keeps the existing icon
    std::vector<std::string> tags;
};

uint32_t crc32(const std::string& data);
// Steam's id for a non-Steam shortcut: CRC32 of the quoted Exe field plus the
// name, with the top bit set. Grid artwork is named after it.
uint32_t shortcut_app_id(const std::string& exe_field, const std::string& app_name);
std::string quote_path(const std::string& path);

// A shortcut belongs to the hub when its Exe is the hub executable and its
// LaunchOptions contain "--launch <id>". Returns that id, or "" otherwise.
std::string hub_game_id_of(const VdfNode& shortcut, const std::string& hub_exe_file_name);
VdfNode* find_hub_shortcut(VdfNode& root, const std::string& game_id, const std::string& hub_exe_file_name);

struct UpsertOutcome {
    int added = 0;
    int updated = 0;
    std::map<std::string, uint32_t> app_ids;  // game id -> shortcut app id
};

// Adds or updates one shortcut per spec. Existing hub shortcuts keep their app
// id (and so their Steam artwork and play time); other shortcuts and fields the
// hub does not manage are left alone. Creates the "shortcuts" map when missing.
UpsertOutcome upsert_shortcuts(VdfNode& root, const std::vector<ShortcutSpec>& specs, const std::string& hub_exe_file_name);

// Removes the hub's shortcuts for the given ids, or all of them when ids is
// empty. Returns the app ids that were removed.
std::vector<uint32_t> remove_shortcuts(VdfNode& root, const std::vector<std::string>& ids, const std::string& hub_exe_file_name);

// Text KeyValues, the format of config/loginusers.vdf.
struct TextVdf {
    std::string key;
    std::string value;
    bool is_map = false;
    std::vector<TextVdf> children;

    const TextVdf* find(const std::string& k) const;  // case-insensitive
};
bool parse_text_vdf(const std::string& text, TextVdf& root, std::string& error);

// Steam library artwork built from a cover image: a darkened, blurred fill of
// the cover behind the cover itself, scaled by a whole factor when it is small
// pixel art. An empty cover yields a plain dark image.
platform::Image compose_capsule(const platform::Image& cover, int width, int height);
// Square icon: the cover's centre, scaled by a whole factor up to 256 pixels.
platform::Image compose_icon(const platform::Image& cover);

}  // namespace hub::steam
