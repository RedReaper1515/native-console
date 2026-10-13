// Unit tests for the hub's Steam shortcut files, account list parsing, and artwork.
#include "hub_steam_vdf.h"

#include <cstdio>
#include <string>
#include <vector>

using hub::platform::Image;
using namespace hub::steam;

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

// A shortcut the hub did not create, with value types the hub never writes.
VdfNode foreign_shortcuts() {
    VdfNode root;
    VdfNode& list = root.child_map("shortcuts");
    VdfNode& shortcut = list.child_map("0");
    shortcut.set_int("appid", 0x8123ABCDu);
    shortcut.set_string("AppName", "Some Other Game");
    shortcut.set_string("Exe", "\"C:\\Games\\other.exe\"");
    shortcut.set_string("LaunchOptions", "--launch not_ours");
    VdfNode wide;
    wide.type = VdfNode::UInt64;
    wide.key = "Big";
    wide.u64 = 0x1122334455667788ull;
    shortcut.children.push_back(wide);
    VdfNode real;
    real.type = VdfNode::Float32;
    real.key = "Scale";
    real.i32 = 0x3F800000u;
    shortcut.children.push_back(real);
    shortcut.child_map("tags").set_string("0", "Mine");
    return root;
}

ShortcutSpec spec(const std::string& id, const std::string& title) {
    ShortcutSpec s;
    s.game_id = id;
    s.app_name = title;
    s.exe = "D:\\Hub\\native_console_hub.exe";
    s.start_dir = "D:\\Hub";
    s.launch_options = "--data-dir \"D:\\Hub Data\" --launch " + id;
    s.tags = {"Native Console", "Game Boy"};
    return s;
}

}  // namespace

int main() {
    const std::string hub = "native_console_hub.exe";

    CHECK(crc32("123456789") == 0xCBF43926u);

    // A parse/write cycle reproduces the file byte for byte; damage is rejected.
    {
        const std::vector<uint8_t> bytes = write_binary_vdf(foreign_shortcuts());
        CHECK(bytes.size() > 2 && bytes[bytes.size() - 1] == 0x08 && bytes[bytes.size() - 2] == 0x08);
        VdfNode parsed;
        std::string error;
        CHECK(parse_binary_vdf(bytes, parsed, error));
        CHECK(write_binary_vdf(parsed) == bytes);
        const std::vector<uint8_t> truncated(bytes.begin(), bytes.end() - 3);
        CHECK(!parse_binary_vdf(truncated, parsed, error));
        std::vector<uint8_t> unknown_type = bytes;
        unknown_type[0] = 0x05;
        CHECK(!parse_binary_vdf(unknown_type, parsed, error));
    }

    VdfNode root = foreign_shortcuts();
    const UpsertOutcome first = upsert_shortcuts(root, {spec("zelda", "Zelda"), spec("kirby", "Kirby")}, hub);
    CHECK(first.added == 2 && first.updated == 0);
    CHECK(first.app_ids.at("zelda") == shortcut_app_id("\"D:\\Hub\\native_console_hub.exe\"", "Zelda"));
    CHECK((first.app_ids.at("kirby") & 0x80000000u) != 0);
    const VdfNode* list = root.find("shortcuts");
    CHECK(list && list->children.size() == 3);
    CHECK(list->children[2].key == "2");
    CHECK(hub_game_id_of(list->children[1], hub) == "zelda");
    CHECK(hub_game_id_of(list->children[0], hub).empty());  // not the hub's exe, even with --launch
    CHECK(list->children[1].get_string("StartDir") == "\"D:\\Hub\"");
    CHECK(list->children[1].find("tags")->children.size() == 2);

    // Adding again updates in place: same app id, new title, tags merged.
    find_hub_shortcut(root, "zelda", hub)->child_map("tags").set_string("2", "User tag");
    const UpsertOutcome second = upsert_shortcuts(root, {spec("zelda", "Zelda DX")}, hub);
    CHECK(second.added == 0 && second.updated == 1);
    CHECK(second.app_ids.at("zelda") == first.app_ids.at("zelda"));
    const VdfNode* zelda = find_hub_shortcut(root, "zelda", hub);
    CHECK(zelda && zelda->get_string("AppName") == "Zelda DX");
    CHECK(zelda && zelda->find("tags")->children.size() == 3);
    CHECK(root.find("shortcuts")->children.size() == 3);
    {
        VdfNode again;
        std::string error;
        CHECK(parse_binary_vdf(write_binary_vdf(root), again, error));
        CHECK(find_hub_shortcut(again, "kirby", hub) != nullptr);
    }

    // Removal takes only the hub's shortcuts and renumbers the rest.
    const std::vector<uint32_t> removed = remove_shortcuts(root, {}, hub);
    CHECK(removed.size() == 2);
    list = root.find("shortcuts");
    CHECK(list->children.size() == 1 && list->children[0].key == "0");
    CHECK(list->children[0].get_string("AppName") == "Some Other Game");

    // loginusers.vdf
    {
        const std::string text =
            "\"users\"\n{\n\t\"76561198112309795\"\n\t{\n"
            "\t\t\"AccountName\"\t\t\"someone\"\n"
            "\t\t\"PersonaName\"\t\t\"Some \\\"One\\\"\"\n"
            "\t\t\"MostRecent\"\t\t\"1\"\n\t}\n}\n";
        TextVdf doc;
        std::string error;
        CHECK(parse_text_vdf(text, doc, error));
        const TextVdf* users = doc.find("users");
        CHECK(users && users->children.size() == 1);
        CHECK(users && users->children[0].find("personaname") != nullptr);
        CHECK(users && users->children[0].find("PersonaName")->value == "Some \"One\"");
        CHECK(!parse_text_vdf("\"users\" { \"a\" ", doc, error));
    }

    // Artwork: a Game Boy screenshot is enlarged by a whole factor and centred.
    {
        Image cover;
        cover.width = 160;
        cover.height = 144;
        cover.rgba.assign(160 * 144 * 4, 0);
        for (int y = 0; y < 144; ++y) {
            for (int x = 0; x < 160; ++x) {
                uint8_t* p = &cover.rgba[(y * 160 + x) * 4];
                p[0] = static_cast<uint8_t>(x);
                p[1] = static_cast<uint8_t>(y);
                p[2] = 7;
                p[3] = 255;
            }
        }
        const Image capsule = compose_capsule(cover, 600, 900);
        CHECK(capsule.width == 600 && capsule.height == 900 && capsule.rgba.size() == 600u * 900u * 4u);
        const uint8_t* centre = &capsule.rgba[(450 * 600 + 300) * 4];
        CHECK(centre[0] == 80 && centre[1] == 72 && centre[2] == 7 && centre[3] == 255);
        const uint8_t* corner = &capsule.rgba[0];
        CHECK(corner[3] == 255 && corner[2] < 7);  // darkened background
        const Image hero = compose_capsule(cover, 1920, 620);
        CHECK(hero.width == 1920 && hero.height == 620);
        const Image icon = compose_icon(cover);
        CHECK(icon.width == 144 && icon.height == 144);
        const Image fallback = compose_capsule(Image{}, 920, 430);
        CHECK(fallback.width == 920 && fallback.rgba.size() == 920u * 430u * 4u && fallback.rgba[3] == 255);
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("hub steam: all checks passed\n");
    return 0;
}
