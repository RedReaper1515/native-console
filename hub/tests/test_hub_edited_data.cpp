// Unit tests for a Game Boy game started with edited data: what a remembered
// path is, the command that builds a tree, the arguments the game is started
// with, the failure paths, and the choice kept in the library.
//
//   test_hub_edited_data [<python executable> <tools/gb_data.py> [<gbrecomp> <make_mapper_rom.py>]]
//
// With the first two arguments the real tool also splits and builds a small
// tree; with all four a tree is made of a fixture ROM as the hub's job makes it.
#include "hub_edited_data.h"
#include "hub_json.h"
#include "hub_library.h"
#include "hub_platform.h"

extern "C" {
#include "gbrt_hash.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hub;

// The two platform services hub_library.cpp uses, so that this test links
// without the platform layer.
namespace hub::platform {
bool read_binary_file(const std::string& path, std::vector<uint8_t>& out, size_t) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
long long now_unix() { return 0; }
}  // namespace hub::platform

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

namespace fs = std::filesystem;

const std::string kRomA(64, 'a');
const std::string kRomB(64, 'b');

std::string join(const std::string& a, const std::string& b) { return (fs::path(a) / b).string(); }
bool exists(const std::string& path) { return fs::exists(path); }
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void write_file(const std::string& path, const std::string& data) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The head of a manifest as gb_data.py split writes it, with `padding` bytes of regions after it.
std::string manifest(const std::string& rom_sha256, const std::string& schema = "gbrecomp.data-tree", int version = 1,
                     size_t padding = 0) {
    return "{\n \"schema\": \"" + schema + "\",\n \"version\": " + std::to_string(version) +
           ",\n \"rom\": {\n  \"size\": 32768,\n  \"sha1\": \"00\",\n  \"sha256\": \"" + rom_sha256 +
           "\",\n  \"title\": \"TEST GAME\"\n },\n \"regions\": [\n  \"" + std::string(padding, 'x') + "\"\n ]\n}\n";
}

// A file that passes for a game's executable: it knows these flags.
void write_exe(const std::string& path, const std::vector<std::string>& flags) {
    std::string exe(4096, 'z');
    for (const auto& flag : flags) {
        exe += flag;
        exe.push_back('\0');
        exe += "zz";
    }
    write_file(path, exe);
}

void test_describe(const std::string& root) {
    using edited_data::Kind;
    CHECK(edited_data::describe("", kRomA).kind == Kind::None);

    auto missing = edited_data::describe(join(root, "nowhere"), kRomA);
    CHECK(missing.kind == Kind::Unusable && has(missing.error, "not there"));

    const std::string empty_dir = join(root, "empty");
    fs::create_directories(empty_dir);
    auto bare = edited_data::describe(empty_dir, kRomA);
    CHECK(bare.kind == Kind::Unusable && has(bare.error, "manifest.json"));

    const std::string tree = join(root, "tree");
    write_file(join(tree, "manifest.json"), manifest(kRomA));
    auto good = edited_data::describe(tree, kRomA);
    CHECK(good.kind == Kind::Tree && good.path == tree && good.error.empty() && good.pack.empty());

    // A tree of another ROM is named as that, with the title its manifest has.
    auto other = edited_data::describe(tree, kRomB);
    CHECK(other.kind == Kind::Unusable && has(other.error, "another ROM") && has(other.error, "TEST GAME"));

    const std::string odd = join(root, "odd");
    write_file(join(odd, "manifest.json"), manifest(kRomA, "gbrecomp.lift-export"));
    CHECK(edited_data::describe(odd, kRomA).kind == Kind::Unusable);
    write_file(join(odd, "manifest.json"), manifest(kRomA, "gbrecomp.data-tree", 2));
    CHECK(edited_data::describe(odd, kRomA).kind == Kind::Unusable);
    write_file(join(odd, "manifest.json"), "{\"schema\": \"gbrecomp.data-tree\", \"version\": 1, \"regions\": []}");
    CHECK(edited_data::describe(odd, kRomA).kind == Kind::Unusable);
    write_file(join(odd, "manifest.json"), "not json at all");
    CHECK(has(edited_data::describe(odd, kRomA).error, "version 1 gbrecomp.data-tree"));

    // A real manifest runs to megabytes of regions: only its head is read.
    const std::string big = join(root, "big");
    write_file(join(big, "manifest.json"), manifest(kRomA, "gbrecomp.data-tree", 1, 300000));
    CHECK(edited_data::describe(big, kRomA).kind == Kind::Tree);
    CHECK(edited_data::describe(big, kRomB).kind == Kind::Unusable);
    // One that says which ROM it is of only after its regions is read whole.
    const std::string late = join(root, "late");
    write_file(join(late, "manifest.json"),
               "{\"regions\": [\"" + std::string(100000, 'x') + "\"], \"version\": 1, \"schema\": \"gbrecomp.data-tree\", "
               "\"rom\": {\"sha256\": \"" + kRomA + "\"}}");
    CHECK(edited_data::describe(late, kRomA).kind == Kind::Tree);
    CHECK(has(edited_data::describe(late, kRomB).error, "another ROM"));

    // An image is a file; its pack is beside it under its own name with .gbup.
    const std::string image = join(root, "images/game.gb");
    write_file(image, "image");
    auto alone = edited_data::describe(image, kRomA);
    CHECK(alone.kind == Kind::Image && alone.pack.empty() && alone.error.empty());
    write_file(join(root, "images/other.gbup"), "pack");
    CHECK(edited_data::describe(image, kRomA).pack.empty());
    write_file(join(root, "images/game.gbup"), "pack");
    CHECK(edited_data::describe(image, kRomA).pack == join(root, "images/game.gbup"));
    // The pack chosen by mistake is not its own pack.
    CHECK(edited_data::describe(join(root, "images/game.gbup"), kRomA).pack.empty());
}

void test_arguments() {
    auto plain = edited_data::launch_args("D:/a b/image.gb", "");
    CHECK(plain == (std::vector<std::string>{"--rom-image", "D:/a b/image.gb"}));
    auto packed = edited_data::launch_args("image.gb", "image.gbup");
    CHECK(packed == (std::vector<std::string>{"--rom-image", "image.gb", "--unpacked", "image.gbup"}));

    auto command = edited_data::build_command({"py thon", "tools/gb_data.py"}, "my tree", "out/image.gb");
    CHECK(command == (std::vector<std::string>{"py thon", "-X", "utf8", "tools/gb_data.py", "build", "my tree", "-o",
                                               "out/image.gb"}));

    CHECK(edited_data::log_file_of({"--log-file", "a.log", "--rom-image", "x"}) == "a.log");
    CHECK(edited_data::log_file_of({"--log-file", "a.log", "--model", "cgb", "--log-file", "b.log"}) == "b.log");
    CHECK(edited_data::log_file_of({"--rom-image", "x"}).empty());
    CHECK(edited_data::log_file_of({"--rom-image", "--log-file"}).empty());
}

// What the runner was asked to do, and what it is to do about it.
struct FakeTool {
    int calls = 0;
    std::vector<std::string> argv;
    int exit_code = 0;
    bool write_image = true;
    bool write_pack = false;
    std::vector<std::string> prints;

    edited_data::Runner runner() {
        return [this](const std::vector<std::string>& args, std::vector<std::string>& output) {
            ++calls;
            argv = args;
            const std::string image = args.empty() ? "" : args.back();
            if (write_image) write_file(image, "built image");
            if (write_pack) write_file(fs::path(image).replace_extension(".gbup").string(), "built pack");
            for (const auto& line : prints) output.push_back(line);
            return exit_code;
        };
    }
};

void test_prepare_nothing_and_image(const std::string& root) {
    const std::string game_dir = join(root, "games/plain");
    const std::string exe = join(game_dir, "game/plain.exe");
    write_exe(exe, {"--rom-image", "--unpacked", "--unpacked-harvest"});
    const edited_data::Game game{game_dir, exe, kRomA};
    const edited_data::Tool tool{"python", "gb_data.py"};

    // Nothing chosen: nothing to pass, nothing run, and the game's folder is as it was.
    FakeTool fake;
    auto nothing = edited_data::prepare("", game, tool, fake.runner());
    CHECK(nothing.args.empty() && nothing.error.empty() && nothing.summary.empty() && nothing.output.empty());
    CHECK(fake.calls == 0);
    CHECK(!exists(edited_data::build_dir(game_dir)));
    size_t entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(game_dir)) entries += entry.is_regular_file();
    CHECK(entries == 1);

    // An image is passed as it is, with the pack beside it when there is one.
    const std::string image = join(root, "built/red.gb");
    write_file(image, "image");
    auto start = edited_data::prepare(image, game, tool, fake.runner());
    CHECK(start.error.empty() && start.args == (std::vector<std::string>{"--rom-image", image}));
    write_file(join(root, "built/red.gbup"), "pack");
    start = edited_data::prepare(image, game, tool, fake.runner());
    CHECK(start.error.empty());
    CHECK(start.args == (std::vector<std::string>{"--rom-image", image, "--unpacked", join(root, "built/red.gbup")}));
    // No tool is needed for an image, and the hub builds nothing for it.
    start = edited_data::prepare(image, game, edited_data::Tool{}, fake.runner());
    CHECK(start.error.empty() && start.args.size() == 4);
    CHECK(fake.calls == 0 && !exists(edited_data::build_dir(game_dir)));

    // What is no longer there stops the start, and says what.
    start = edited_data::prepare(join(root, "built/gone.gb"), game, tool, fake.runner());
    CHECK(start.args.empty() && start.error == "The edited data cannot be used: it is not there.");

    // A game built before --rom-image would ignore it and start with its own data.
    const std::string old_exe = join(root, "games/old/game/old.exe");
    write_exe(old_exe, {"--texture-pack", "--rom-imagery"});
    start = edited_data::prepare(image, {join(root, "games/old"), old_exe, kRomA}, tool, fake.runner());
    CHECK(start.args.empty() && has(start.error, "Rebuild"));
    // And one built before --unpacked would start without the pack.
    const std::string mid_exe = join(root, "games/mid/game/mid.exe");
    write_exe(mid_exe, {"--rom-image", "--unpacked-harvest"});
    start = edited_data::prepare(image, {join(root, "games/mid"), mid_exe, kRomA}, tool, fake.runner());
    CHECK(start.args.empty() && has(start.error, "Rebuild") && has(start.error, "red.gbup"));
    CHECK(fake.calls == 0);
}

void test_prepare_tree(const std::string& root) {
    const std::string game_dir = join(root, "games/tree_game");
    const std::string exe = join(game_dir, "game/tree_game.exe");
    write_exe(exe, {"--rom-image", "--unpacked"});
    const edited_data::Game game{game_dir, exe, kRomA};
    const edited_data::Tool tool{"C:/Python/python.exe", "D:/hub/tools/gb_data.py"};
    const std::string tree = join(root, "my tree");
    write_file(join(tree, "manifest.json"), manifest(kRomA));
    const std::string image = join(edited_data::build_dir(game_dir), "image.gb");
    const std::string pack = join(edited_data::build_dir(game_dir), "image.gbup");
    CHECK(edited_data::build_dir(game_dir) == join(game_dir, "edited-data"));

    // The tool builds the tree into the game's own folder, and the game is started with what it wrote.
    FakeTool fake;
    fake.write_pack = true;
    fake.prints = {"wrote " + image + ": 2 of 6 files edited (SHA-1 11, the original's is 22)",
                   "  bank_00/0200_unknown.bin (00:0200, 256 bytes)",
                   "wrote " + pack + ": 1 routine to watch, and 3 replacements for what it makes (the game takes it with --unpacked)"};
    auto start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(fake.calls == 1);
    CHECK(fake.argv == (std::vector<std::string>{tool.python, "-X", "utf8", tool.script, "build", tree, "-o", image}));
    CHECK(start.error.empty());
    CHECK(start.args == (std::vector<std::string>{"--rom-image", image, "--unpacked", pack}));
    CHECK(start.summary == "2 of 6 files edited; 1 routine to watch, and 3 replacements for what it makes");
    CHECK(start.output.size() == 3);
    CHECK(read_file(image) == "built image");

    // A tree with no unpackers.json gets no pack: the last build's pack must not go with the new image.
    fake.write_pack = false;
    fake.prints = {"wrote " + image + ": the original ROM, byte for byte (SHA-1 33)"};
    start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(start.error.empty() && start.args == (std::vector<std::string>{"--rom-image", image}));
    CHECK(!exists(pack));
    CHECK(start.summary == "the original ROM, byte for byte");
    // Words the hub does not know are shown as the tool printed them.
    fake.prints = {"something new"};
    CHECK(edited_data::prepare(tree, game, tool, fake.runner()).summary == "something new");
    // It says all went well and writes nothing: the image of the build before is not taken for this one's.
    CHECK(exists(image));
    fake.write_image = false;
    start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(start.args.empty() && has(start.error, "no image"));
    fake.write_image = true;

    // The tool refuses: its own message is the error, the game gets nothing, and no image is left to be mistaken for this one.
    fake.exit_code = 2;
    fake.write_image = false;
    fake.prints = {"some progress", "gb_data: bank_00/0000_code.bin (00:0000, 256 bytes) has been edited, and it is code"};
    start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(start.args.empty());
    CHECK(start.error == "bank_00/0000_code.bin (00:0000, 256 bytes) has been edited, and it is code");
    CHECK(!exists(image));
    // It stops with an image already written (the pack could not be): still not started, and the image goes.
    fake.write_image = true;
    fake.write_pack = true;
    start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(start.args.empty() && !start.error.empty());
    CHECK(!exists(image) && !exists(pack));
    fake.write_pack = false;
    // It falls over without saying why in its own words: the last lines it printed, and the exit code.
    fake.exit_code = 1;
    fake.prints = {"Traceback (most recent call last):", "", "KeyError: 'regions'"};
    start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(start.args.empty() && has(start.error, "exit code 1") && has(start.error, "KeyError: 'regions'"));
    fake.prints = {"1", "2", "3", "4", "5", "6", "7", "8"};
    start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(has(start.error, "\n3\n4\n5\n6\n7\n8") && !has(start.error, "\n2\n"));
    // It could not be started at all.
    fake.exit_code = -1;
    fake.write_image = false;
    fake.prints = {"CreateProcess failed: not found"};
    start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(start.args.empty() && has(start.error, "could not be started") && has(start.error, "CreateProcess failed"));
    // It says all went well and wrote nothing.
    fake.exit_code = 0;
    fake.prints.clear();
    start = edited_data::prepare(tree, game, tool, fake.runner());
    CHECK(start.args.empty() && has(start.error, "no image"));

    // Without Python, or without the tool, nothing is run and the way out is named.
    fake = FakeTool{};
    start = edited_data::prepare(tree, game, {"", tool.script}, fake.runner());
    CHECK(start.args.empty() && has(start.error, "Python was not found") && has(start.error, "choose the image"));
    start = edited_data::prepare(tree, game, {tool.python, ""}, fake.runner());
    CHECK(start.args.empty() && has(start.error, "gb_data.py was not found"));
    // A tree of another ROM is not built at all.
    start = edited_data::prepare(tree, {game_dir, exe, kRomB}, tool, fake.runner());
    CHECK(start.args.empty() && start.error == "The edited data cannot be used: it was split from another ROM (TEST GAME).");
    // Nor one for a game that would not use it.
    const std::string old_exe = join(root, "games/old_tree/game/old.exe");
    write_exe(old_exe, {"--texture-pack"});
    start = edited_data::prepare(tree, {join(root, "games/old_tree"), old_exe, kRomA}, tool, fake.runner());
    CHECK(start.args.empty() && has(start.error, "Rebuild"));
    CHECK(!exists(edited_data::build_dir(join(root, "games/old_tree"))));
    CHECK(fake.calls == 0);

    // The tool writes a pack the game does not know.
    const std::string mid_exe = join(root, "games/mid_tree/game/mid.exe");
    write_exe(mid_exe, {"--rom-image"});
    fake.write_pack = true;
    start = edited_data::prepare(tree, {join(root, "games/mid_tree"), mid_exe, kRomA}, tool, fake.runner());
    CHECK(fake.calls == 1 && start.args.empty() && has(start.error, "Rebuild"));

    // Clearing the choice takes what the hub built with it, and only that.
    std::string error;
    CHECK(exists(edited_data::build_dir(game_dir)));
    CHECK(edited_data::remove_build(game_dir, error) && error.empty());
    CHECK(!exists(edited_data::build_dir(game_dir)) && exists(exe) && exists(join(tree, "manifest.json")));
    CHECK(edited_data::remove_build(game_dir, error));
}

void test_refusal(const std::string& root) {
    const std::string started = "[LOG] Redirecting runtime output to last-run.log\n"
                                "[GBRT] ROM image D:\\x\\image.gb: 3 bytes differ from the ROM this game was built from\n"
                                "[GBRT] Unpacked: 1 routine watched, 2 replacements\n";
    CHECK(edited_data::refusal(started).empty());
    CHECK(edited_data::refusal("").empty());
    // Only the game's own two lines count, whatever else a log says.
    CHECK(edited_data::refusal("[PPU] the sprite in slot 3 of 40 not used: off screen\n[GBRT] ROM image loader ready\n").empty());

    const std::string code = "[LOG] Redirecting runtime output to last-run.log\r\n"
                             "[GBRT] ROM image D:\\my trees\\image.gb not used: the byte at offset 0x00150 is not the one "
                             "this game's code was compiled from (code and the cartridge header cannot be edited in an image: "
                             "recompile for that)\r\n";
    std::string said = edited_data::refusal(code);
    CHECK(has(said, "did not take the image") && has(said, "the byte at offset 0x00150 is not the one"));
    CHECK(!has(said, "\r") && !has(said, "[GBRT]") && !has(said, "my trees"));

    // The path is the user's, and may say anything.
    said = edited_data::refusal("[GBRT] ROM image C:\\was not used: before\\image.gb not used: it is shorter than the 32768 bytes "
                                "of the ROM this game was built from");
    CHECK(said == "The game did not take the image: it is shorter than the 32768 bytes of the ROM this game was built from.");

    said = edited_data::refusal("Frame limit: 10\n[GBRT] ROM image x.gb: 0 bytes differ from the ROM this game was built from\n"
                                "[GBRT] Pack of unpacked data not used: it is of another ROM than the one this game was built from\n");
    CHECK(said == "The game did not take the pack of unpacked data: it is of another ROM than the one this game was built from.");

    const std::string log = join(root, "logs/last-run.log");
    write_file(log, code);
    CHECK(has(edited_data::refusal_in_file(log), "0x00150"));
    write_file(log, started);
    CHECK(edited_data::refusal_in_file(log).empty());
    CHECK(edited_data::refusal_in_file(join(root, "logs/none.log")).empty());
    CHECK(edited_data::refusal_in_file("").empty());
    // The game says it as it starts: a long session's log is not read to its end for it.
    write_file(log, code + std::string(200000, '.') + "\n");
    CHECK(has(edited_data::refusal_in_file(log), "0x00150"));
}

void test_executable_support(const std::string& root) {
    // The flag across the scanner's 1 MiB read boundary.
    std::string exe((1u << 20) + 64, 'z');
    const std::string flag = "--rom-image";
    std::memcpy(exe.data() + (1u << 20) - 5, flag.c_str(), flag.size() + 1);
    const std::string with = join(root, "exes/new.exe");
    write_file(with, exe);
    CHECK(edited_data::executable_supports(with, "--rom-image"));
    CHECK(!edited_data::executable_supports(with, "--unpacked"));
    CHECK(!edited_data::executable_supports(with, "--rom"));
    CHECK(!edited_data::executable_supports(with, ""));

    // A longer flag that starts the same is another flag.
    const std::string harvest = join(root, "exes/harvest.exe");
    write_exe(harvest, {"--unpacked-harvest"});
    CHECK(!edited_data::executable_supports(harvest, "--unpacked"));
    CHECK(edited_data::executable_supports(harvest, "--unpacked-harvest"));
    CHECK(!edited_data::executable_supports(join(root, "exes/missing.exe"), "--rom-image"));
}

void test_find_script(const std::string& root) {
    // Beside the hub, as a release that carried it would have it.
    const std::string beside = join(root, "release");
    write_file(join(beside, "tools/gb_data.py"), "");
    CHECK(fs::equivalent(edited_data::find_script(beside), join(beside, "tools/gb_data.py")));
    // In the checkout the hub was built in: build/bin, two folders down.
    const std::string checkout = join(root, "checkout");
    write_file(join(checkout, "tools/gb_data.py"), "");
    fs::create_directories(join(checkout, "build/bin"));
    const std::string found = edited_data::find_script(join(checkout, "build/bin"));
    CHECK(!found.empty() && fs::equivalent(found, join(checkout, "tools/gb_data.py")));
    CHECK(!has(found, ".."));
    // Nowhere near: a release archive has no tools.
    fs::create_directories(join(root, "bare/a/b/c"));
    CHECK(edited_data::find_script(join(root, "bare/a/b/c")).empty());
}

// The choice is kept with the game, and a game without one is written as it always was.
void test_library_remembers(const std::string& root) {
    const std::string path = join(root, "library/library.json");
    fs::create_directories(fs::path(path).parent_path());
    const std::string tree = root + "\\my tree \xC3\xA9";  // a backslash, a space, and a letter outside ASCII
    {
        Library library;
        library.set_path(path);
        GameEntry plain;
        plain.id = "plain";
        plain.family_id = "gb";
        GameEntry modded = plain;
        modded.id = "modded";
        modded.edited_data = tree;
        library.games() = {plain, modded};
        std::string error;
        CHECK(library.save(error));
    }
    const std::string text = read_file(path);
    const size_t at = text.find("\"edited_data\"");
    CHECK(at != std::string::npos && text.find("\"edited_data\"", at + 1) == std::string::npos);
    CHECK(at > text.find("\"modded\""));

    Library library;
    library.set_path(path);
    std::string error;
    CHECK(library.load(error));
    CHECK(library.find("plain") && library.find("plain")->edited_data.empty());
    CHECK(library.find("modded") && library.find("modded")->edited_data == tree);

    // Cleared, it is gone from the file too.
    library.find("modded")->edited_data.clear();
    CHECK(library.save(error));
    CHECK(!has(read_file(path), "edited_data"));
}

// The two commands that make a tree, as a runner sees them, and what it does about each.
struct FakeTreeTools {
    std::vector<std::vector<std::string>> calls;
    int export_exit = 0;
    int split_exit = 0;
    bool write_export = true;
    bool write_manifest = true;
    bool export_was_there = false;  // when the split ran
    std::vector<std::string> export_prints, split_prints;

    edited_data::Runner runner() {
        return [this](const std::vector<std::string>& argv, std::vector<std::string>& output) {
            calls.push_back(argv);
            const bool exporting = argv.size() > 2 && argv[argv.size() - 2] == "--lift-export";
            if (exporting) {
                if (write_export) write_file(argv.back(), "{}");
                for (const auto& line : export_prints) output.push_back(line);
                return export_exit;
            }
            export_was_there = argv.size() > 5 && exists(argv[5]);
            if (write_manifest) write_file(join(argv.back(), "manifest.json"), manifest(kRomA));
            for (const auto& line : split_prints) output.push_back(line);
            return split_exit;
        };
    }
};

void test_make_tree(const std::string& root) {
    CHECK(edited_data::tree_dir(join(root, "hd"), "red") == join(join(join(root, "hd"), "data-trees"), "red"));

    // The export is asked of the very command a build recompiles with: the analysis is the build's.
    const std::vector<std::string> recompile = {"gbrecomp", "rom.gb", "-o", "work/src", "--output-prefix", "red",
                                                "--compact-resume", "--pointer-scan", "strict"};
    std::vector<std::string> expected = recompile;
    expected.push_back("--lift-export");
    expected.push_back("my tree.export.json");
    CHECK(edited_data::export_command(recompile, "my tree.export.json") == expected);
    const edited_data::Tool tool{"python", "tools/gb_data.py"};
    CHECK(edited_data::split_command(tool, "x.json", "rom.gb", "my tree") ==
          (std::vector<std::string>{"python", "-X", "utf8", "tools/gb_data.py", "split", "x.json", "--rom", "rom.gb", "-o",
                                    "my tree"}));

    const std::string tree = join(root, "hd/data-trees/made");
    const std::string export_file = tree + ".export.json";
    std::vector<std::string> output;

    // What cannot work is said before anything is run.
    FakeTreeTools fake;
    CHECK(has(edited_data::make_tree({}, "rom.gb", tree, tool, fake.runner(), output), "recompiler is not set"));
    CHECK(has(edited_data::make_tree(recompile, "rom.gb", tree, {"", tool.script}, fake.runner(), output), "Python was not found"));
    CHECK(has(edited_data::make_tree(recompile, "rom.gb", tree, {tool.python, ""}, fake.runner(), output), "gb_data.py was not found"));
    CHECK(fake.calls.empty() && output.empty());

    // The recompiler does not know --lift-export: its words, and no split.
    fake.export_exit = 1;
    fake.write_export = false;
    fake.export_prints = {"Error: unknown option --lift-export"};
    std::string error = edited_data::make_tree(recompile, "rom.gb", tree, tool, fake.runner(), output);
    CHECK(has(error, "the recompiler stopped with exit code 1") && has(error, "unknown option --lift-export"));
    CHECK(fake.calls.size() == 1 && !exists(join(tree, "manifest.json")));
    // It says all went well and wrote nothing.
    fake = FakeTreeTools{};
    fake.write_export = false;
    error = edited_data::make_tree(recompile, "rom.gb", tree, tool, fake.runner(), output);
    CHECK(has(error, "wrote no description") && fake.calls.size() == 1);

    // The split refuses: the tool's own message, and the export is not left lying.
    fake = FakeTreeTools{};
    fake.split_exit = 2;
    fake.write_manifest = false;
    fake.split_prints = {"gb_data: rom.gb is not the ROM x.json was made from"};
    output.clear();
    error = edited_data::make_tree(recompile, "rom.gb", tree, tool, fake.runner(), output);
    CHECK(error == "rom.gb is not the ROM x.json was made from");
    CHECK(fake.calls.size() == 2 && fake.export_was_there && !exists(export_file));
    CHECK(output == fake.split_prints);
    // It says all went well and wrote no tree.
    fake = FakeTreeTools{};
    fake.write_manifest = false;
    CHECK(has(edited_data::make_tree(recompile, "rom.gb", tree, tool, fake.runner(), output), "no tree"));
    // It falls over: its last lines, not the recompiler's.
    fake.split_exit = 1;
    fake.export_prints = {"Wrote lift export"};
    fake.split_prints = {"Traceback (most recent call last):", "ValueError"};
    error = edited_data::make_tree(recompile, "rom.gb", tree, tool, fake.runner(), output);
    CHECK(has(error, "the data tool stopped with exit code 1") && has(error, "ValueError") && !has(error, "lift export"));

    // Both go well: the two commands in order, the export gone again, and a tree.
    fake = FakeTreeTools{};
    error = edited_data::make_tree(recompile, "rom.gb", tree, tool, fake.runner(), output);
    CHECK(error.empty());
    CHECK(fake.calls.size() == 2 && fake.export_was_there);
    if (fake.calls.size() == 2) {
        CHECK(fake.calls[0] == edited_data::export_command(recompile, export_file));
        CHECK(fake.calls[1] == edited_data::split_command(tool, export_file, "rom.gb", tree));
    }
    CHECK(!exists(export_file));
    CHECK(edited_data::describe(tree, kRomA).kind == edited_data::Kind::Tree);

    // A tree that is there is never made again: its files may have edits in them.
    fake = FakeTreeTools{};
    write_file(join(tree, "bank_00/edited.bin"), "somebody's work");
    error = edited_data::make_tree(recompile, "rom.gb", tree, tool, fake.runner(), output);
    CHECK(has(error, "already holds a data tree") && fake.calls.empty());
    CHECK(read_file(join(tree, "bank_00/edited.bin")) == "somebody's work");
}

// ---------------------------------------------------------------------------
// With the real tool
// ---------------------------------------------------------------------------

std::string in_quotes(const std::string& arg) { return "\"" + arg + "\""; }

// Runs the command through the shell with its output in a file.
edited_data::Runner shell_runner(const std::string& scratch) {
    return [scratch](const std::vector<std::string>& argv, std::vector<std::string>& output) {
        const std::string out = join(scratch, "tool-output.txt");
        std::string command;
        for (const auto& arg : argv) command += in_quotes(arg) + " ";
        command += "> " + in_quotes(out) + " 2>&1";
#ifdef _WIN32
        command = in_quotes(command);  // cmd /c drops the outermost pair
#endif
        const int status = std::system(command.c_str());
        std::ifstream in(out);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            output.push_back(line);
        }
#ifdef _WIN32
        return status;
#else
        return status == -1 ? -1 : (status >> 8) & 0xFF;
#endif
    };
}

std::string hex(const uint8_t* bytes, size_t count) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}

void test_real_tool(const std::string& root, const std::string& python, const std::string& script) {
    const std::string scratch = join(root, "real tool");  // a space: the paths are quoted all the way
    fs::create_directories(scratch);
    const edited_data::Runner run = shell_runner(scratch);

    // A ROM of two banks, and the export that says where its data is.
    std::string rom(0x8000, '\0');
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = static_cast<char>((i * 7 + (i >> 8)) & 0xFF);
    uint8_t digest[32];
    gbrt_sha256(reinterpret_cast<const uint8_t*>(rom.data()), rom.size(), digest);
    const std::string rom_sha256 = hex(digest, 32);
    const std::string rom_path = join(scratch, "tiny.gb");
    write_file(rom_path, rom);
    const std::string export_path = join(scratch, "tiny.json");
    write_file(export_path,
               "{\"schema\": \"gbrecomp.lift-export\", \"rom\": {\"size\": 32768, \"sha256\": \"" + rom_sha256 +
               "\", \"title\": \"TINY\"}, \"data_regions\": ["
               "{\"rom_offset\": 256, \"size\": 80, \"bank\": 0, \"address\": 256, \"guess\": \"header\"}, "
               "{\"rom_offset\": 512, \"size\": 256, \"bank\": 0, \"address\": 512, \"guess\": \"unknown\"}]}");
    const std::string tree = join(scratch, "tiny tree");
    std::vector<std::string> output;
    CHECK(run({python, script, "split", export_path, "--rom", rom_path, "-o", tree}, output) == 0);
    // The data file and a file of code, as the manifest names them.
    std::string data_file, code_file, parse_error;
    long long data_offset = -1;
    Json tree_manifest;
    CHECK(Json::parse(read_file(join(tree, "manifest.json")), tree_manifest, parse_error));
    if (const Json* regions = tree_manifest.get("regions")) {
        for (const Json& region : regions->array) {
            if (region.get_string("kind") == "data" && data_file.empty()) {
                data_file = join(tree, region.get_string("file"));
                data_offset = region.get_int("offset");
            }
            if (region.get_string("kind") == "code" && code_file.empty()) code_file = join(tree, region.get_string("file"));
        }
    }
    CHECK(data_offset == 512 && exists(data_file) && exists(code_file));
    if (failures) return;

    const std::string game_dir = join(scratch, "games/tiny");
    const std::string exe = join(game_dir, "game/tiny.exe");
    write_exe(exe, {"--rom-image", "--unpacked"});
    const edited_data::Game game{game_dir, exe, rom_sha256};
    const edited_data::Tool tool{python, script};
    const std::string image = join(edited_data::build_dir(game_dir), "image.gb");
    CHECK(edited_data::describe(tree, rom_sha256).kind == edited_data::Kind::Tree);
    CHECK(edited_data::describe(tree, kRomA).kind == edited_data::Kind::Unusable);

    // Unedited, the image the hub has built is the ROM.
    auto start = edited_data::prepare(tree, game, tool, run);
    CHECK(start.error.empty());
    CHECK(start.args == (std::vector<std::string>{"--rom-image", image}));
    CHECK(start.summary == "the original ROM, byte for byte");
    CHECK(read_file(image) == rom);

    // A data file edited, and the image has the edit where the file lies.
    std::string data = read_file(data_file);
    CHECK(data.size() == 256);
    data[5] = static_cast<char>(data[5] ^ 0x55);
    write_file(data_file, data);
    start = edited_data::prepare(tree, game, tool, run);
    CHECK(start.error.empty() && start.args.size() == 2);
    CHECK(start.summary.rfind("1 of ", 0) == 0 && has(start.summary, "files edited") && !has(start.summary, "SHA-1"));
    std::string expected = rom;
    expected[0x205] = data[5];
    CHECK(read_file(image) == expected);

    // The tree says which routine unpacks: the pack is built beside the image and passed too.
    write_file(join(tree, "unpackers.json"),
               "{\"schema\": \"gbrecomp.unpackers\", \"version\": 1, \"unpackers\": [{\"name\": \"pictures\", "
               "\"function\": {\"bank\": 0, \"address\": 441}, \"stream\": {\"bank\": {\"register\": \"a\"}, "
               "\"address\": {\"register\": \"hl\"}}, \"made\": [{\"address\": 49408, \"size\": 32}]}]}");
    start = edited_data::prepare(tree, game, tool, run);
    const std::string pack = join(edited_data::build_dir(game_dir), "image.gbup");
    CHECK(start.error.empty());
    CHECK(start.args == (std::vector<std::string>{"--rom-image", image, "--unpacked", pack}));
    CHECK(has(start.summary, "files edited; 1 routine to watch") && !has(start.summary, "--unpacked"));
    CHECK(read_file(pack).rfind("GBUP", 0) == 0);

    // An edit to code: the tool refuses, in its words, and the game is not started.
    std::string code = read_file(code_file);
    code[0] = static_cast<char>(code[0] ^ 1);
    write_file(code_file, code);
    start = edited_data::prepare(tree, game, tool, run);
    CHECK(start.args.empty());
    CHECK(has(start.error, "has been edited, and it is code") && has(start.error, "_code.bin"));
    CHECK(!has(start.error, "gb_data: "));
    CHECK(!exists(image) && !exists(pack));

    // A tool that is not there.
    start = edited_data::prepare(tree, game, {python, join(scratch, "no_such_tool.py")}, run);
    CHECK(start.args.empty() && has(start.error, "exit code"));
}

// Making a tree with the real recompiler and the real tool, of a ROM the repository makes.
void test_real_tree(const std::string& root, const std::string& python, const std::string& script,
                    const std::string& recompiler, const std::string& rom_maker) {
    const std::string scratch = join(root, "real tree");
    fs::create_directories(scratch);
    const edited_data::Runner run = shell_runner(scratch);
    const std::string rom_path = join(scratch, "fixture.gb");
    std::vector<std::string> output;
    CHECK(run({python, rom_maker, "--mapper", "mbc1", "--output", rom_path}, output) == 0);
    const std::string rom = read_file(rom_path);
    CHECK(rom.size() >= 0x8000);
    uint8_t digest[32];
    gbrt_sha256(reinterpret_cast<const uint8_t*>(rom.data()), rom.size(), digest);
    const std::string rom_sha256 = hex(digest, 32);

    // A build's command as the Game Boy backend writes it, folder for the C and all.
    const std::string unused = join(scratch, "work/src");
    const std::vector<std::string> recompile = {recompiler, rom_path, "-o", unused, "--output-prefix", "fixture",
                                                "--compact-resume", "--pointer-scan", "strict"};
    const std::string tree = join(scratch, "data-trees/fixture");
    const edited_data::Tool tool{python, script};
    output.clear();
    std::string error = edited_data::make_tree(recompile, rom_path, tree, tool, run, output);
    CHECK(error.empty());
    if (!error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return;
    }
    CHECK(!exists(unused));  // told to describe the ROM, the recompiler writes no code
    CHECK(!exists(tree + ".export.json"));
    CHECK(edited_data::describe(tree, rom_sha256).kind == edited_data::Kind::Tree);

    // The tree it made, built unedited, is the ROM.
    const std::string game_dir = join(scratch, "games/fixture");
    const std::string exe = join(game_dir, "game/fixture.exe");
    write_exe(exe, {"--rom-image", "--unpacked"});
    auto start = edited_data::prepare(tree, {game_dir, exe, rom_sha256}, tool, run);
    CHECK(start.error.empty() && start.summary == "the original ROM, byte for byte");
    CHECK(start.args.size() == 2 && read_file(start.args.back()) == rom);

    // Asked again, it leaves the tree alone.
    CHECK(has(edited_data::make_tree(recompile, rom_path, tree, tool, run, output), "already holds a data tree"));
}

}  // namespace

int main(int argc, char** argv) {
    const auto root_path = fs::temp_directory_path() / "hub_edited_data_test";
    fs::remove_all(root_path);
    fs::create_directories(root_path);
    const std::string root = root_path.string();

    test_describe(root);
    test_arguments();
    test_prepare_nothing_and_image(root);
    test_prepare_tree(root);
    test_refusal(root);
    test_executable_support(root);
    test_find_script(root);
    test_library_remembers(root);
    test_make_tree(root);
    if (argc >= 3) test_real_tool(root, argv[1], argv[2]);
    else std::printf("no Python and gb_data.py given: the real tool was not run\n");
    if (argc >= 5) test_real_tree(root, argv[1], argv[2], argv[3], argv[4]);
    else std::printf("no recompiler and ROM maker given: no tree was made with the real ones\n");

    fs::remove_all(root_path);
    if (failures == 0) std::printf("hub edited data tests passed\n");
    return failures == 0 ? 0 : 1;
}
