#include "hub_edited_data.h"

#include "hub_json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace hub::edited_data {

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

bool is_file(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(to_path(path), ec);
}

constexpr const char* kManifest = "manifest.json";
constexpr const char* kTreeSchema = "gbrecomp.data-tree";
constexpr const char* kImageName = "image.gb";
constexpr const char* kPackName = "image.gbup";
constexpr const char* kImageFlag = "--rom-image";
constexpr const char* kPackFlag = "--unpacked";
constexpr const char* kToolSays = "gb_data: ";  // how gb_data.py starts the line that says why it stopped
constexpr size_t kHead = 64u * 1024u;

// Up to `limit` bytes of a file; all of it when limit is 0.
bool read_head(const std::string& path, size_t limit, std::string& out) {
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in) return false;
    out.clear();
    char buffer[16384];
    while (in && (limit == 0 || out.size() < limit)) {
        const size_t want = limit == 0 ? sizeof(buffer) : std::min(sizeof(buffer), limit - out.size());
        in.read(buffer, static_cast<std::streamsize>(want));
        if (in.gcount() <= 0) break;
        out.append(buffer, static_cast<size_t>(in.gcount()));
    }
    return true;
}

// The ROM a tree was split from. A manifest lists every region of the ROM
// and runs to megabytes, and says which ROM it is of before the regions: so
// the head is cut where they start, closed, and read as JSON by itself. A
// manifest that is not laid out that way is read whole.
bool manifest_rom(const std::string& tree, std::string& rom_sha256, std::string& title, std::string& error) {
    const std::string path = join(tree, kManifest);
    std::string text;
    if (!read_head(path, kHead, text)) {
        error = "it has no manifest.json: a data tree is the folder gb_data.py split wrote";
        return false;
    }
    Json manifest;
    std::string ignore;
    bool parsed = false;
    const size_t regions = text.find("\"regions\"");
    if (regions != std::string::npos) {
        std::string head = text.substr(0, regions);
        while (!head.empty() && (head.back() == ' ' || head.back() == '\n' || head.back() == '\r' || head.back() == '\t')) {
            head.pop_back();
        }
        if (!head.empty() && head.back() == ',') head.pop_back();
        parsed = Json::parse(head + "}", manifest, ignore) && manifest.get("rom") != nullptr;
    }
    if (!parsed) parsed = read_head(path, 0, text) && Json::parse(text, manifest, ignore);
    const Json* rom = parsed && manifest.is_object() ? manifest.get("rom") : nullptr;
    if (!rom || !rom->is_object() || manifest.get_string("schema") != kTreeSchema || manifest.get_int("version") != 1 ||
        rom->get_string("sha256").empty()) {
        error = std::string("its manifest.json is not a version 1 ") + kTreeSchema + " manifest";
        return false;
    }
    rom_sha256 = rom->get_string("sha256");
    title = rom->get_string("title");
    return true;
}

// The part of a line the tool printed after "wrote <file>: ", without the
// hashes in brackets that follow it. "" when the line is not about `file`.
std::string said_of(const std::string& line, const std::string& file, const std::string& bracket) {
    const std::string marker = file + ": ";
    const size_t at = line.find(marker);
    if (line.rfind("wrote ", 0) != 0 || at == std::string::npos) return "";
    std::string said = line.substr(at + marker.size());
    const size_t cut = said.find(bracket);
    if (cut != std::string::npos) said.resize(cut);
    return said;
}

// Why the tool stopped: the lines it marks as that, or failing them the last it printed.
std::string tool_message(const std::vector<std::string>& output, int exit_code,
                         const std::string& who = "the data tool") {
    std::string message;
    for (const auto& line : output) {
        if (line.rfind(kToolSays, 0) != 0) continue;
        if (!message.empty()) message += "\n";
        message += line.substr(std::string(kToolSays).size());
    }
    if (!message.empty()) return message;
    if (exit_code == -1) message = who + " could not be started";
    else message = who + " stopped with exit code " + std::to_string(exit_code);
    size_t first = output.size();
    for (size_t kept = 0; first > 0 && kept < 6; --first) {
        if (!output[first - 1].empty()) ++kept;
    }
    for (size_t i = first; i < output.size(); ++i) {
        if (!output[i].empty()) message += "\n" + output[i];
    }
    return message;
}

}  // namespace

Choice describe(const std::string& path, const std::string& rom_sha256) {
    Choice choice;
    choice.path = path;
    if (path.empty()) return choice;
    std::error_code ec;
    if (fs::is_directory(to_path(path), ec)) {
        std::string tree_rom, title;
        if (!manifest_rom(path, tree_rom, title, choice.error)) {
            choice.kind = Kind::Unusable;
        } else if (tree_rom != rom_sha256) {
            choice.kind = Kind::Unusable;
            choice.error = "it was split from another ROM" + (title.empty() ? std::string() : " (" + title + ")");
        } else {
            choice.kind = Kind::Tree;
        }
    } else if (is_file(path)) {
        choice.kind = Kind::Image;
        // gb_data.py build writes the pack beside the image, under the image's name with .gbup.
        const std::string pack = from_path(to_path(path).replace_extension(".gbup"));
        if (pack != path && is_file(pack)) choice.pack = pack;
    } else {
        choice.kind = Kind::Unusable;
        choice.error = "it is not there";
    }
    return choice;
}

std::string find_script(const std::string& hub_dir) {
    const fs::path hub = to_path(hub_dir);
    for (const fs::path& base : {hub, hub.parent_path(), hub.parent_path().parent_path()}) {
        const fs::path candidate = base / "tools" / "gb_data.py";
        std::error_code ec;
        if (!base.empty() && fs::is_regular_file(candidate, ec)) return from_path(candidate.lexically_normal());
    }
    return "";
}

std::string build_dir(const std::string& game_dir) {
    return join(game_dir, "edited-data");
}

bool remove_build(const std::string& game_dir, std::string& error) {
    std::error_code ec;
    fs::remove_all(to_path(build_dir(game_dir)), ec);
    if (ec) error = "cannot remove " + build_dir(game_dir) + ": " + ec.message();
    return !ec;
}

std::vector<std::string> build_command(const Tool& tool, const std::string& tree, const std::string& image) {
    // UTF-8 mode: the tool prints file names, and the hub reads its output as UTF-8.
    return {tool.python, "-X", "utf8", tool.script, "build", tree, "-o", image};
}

std::vector<std::string> launch_args(const std::string& image, const std::string& pack) {
    std::vector<std::string> args = {kImageFlag, image};
    if (!pack.empty()) {
        args.push_back(kPackFlag);
        args.push_back(pack);
    }
    return args;
}

Start prepare(const std::string& chosen, const Game& game, const Tool& tool, const Runner& run) {
    Start start;
    const Choice choice = describe(chosen, game.rom_sha256);
    if (choice.kind == Kind::None) return start;
    if (choice.kind == Kind::Unusable) {
        start.error = "The edited data cannot be used: " + choice.error + ".";
        return start;
    }
    if (!executable_supports(game.exe_path, kImageFlag)) {
        start.error = "This build of the game was made before edited data could be loaded, and would start with its "
                      "own data. Rebuild the game.";
        return start;
    }

    std::string image = choice.path;
    std::string pack = choice.pack;
    if (choice.kind == Kind::Tree) {
        if (tool.python.empty() || tool.script.empty()) {
            start.error = std::string(tool.python.empty() ? "Python" : "tools/gb_data.py") +
                          " was not found, and a data tree is built into an image with it before the game starts. "
                          "Set it in Settings, or build the image where it is (gb_data.py build TREE -o IMAGE) and "
                          "choose the image.";
            return start;
        }
        const std::string dir = build_dir(game.dir);
        image = join(dir, kImageName);
        pack = join(dir, kPackName);
        // The last build's files go first: a tree that has lost its
        // unpackers.json gets no pack, and the old one must not go with it.
        std::error_code ec;
        fs::create_directories(to_path(dir), ec);
        if (!ec) fs::remove(to_path(image), ec);
        if (!ec) fs::remove(to_path(pack), ec);
        if (ec) {
            start.error = "Cannot write the image in " + dir + ": " + ec.message() + ".";
            return start;
        }
        const int exit_code = run(build_command(tool, choice.path, image), start.output);
        if (exit_code != 0 || !is_file(image)) {
            start.error = exit_code != 0 ? tool_message(start.output, exit_code)
                                         : "the data tool wrote no image to " + image;
            // The tool can stop with the image written and the pack not: neither is to be used.
            fs::remove(to_path(image), ec);
            fs::remove(to_path(pack), ec);
            return start;
        }
        if (!is_file(pack)) pack.clear();
        for (const auto& line : start.output) {
            const std::string of_image = said_of(line, kImageName, " (SHA-1");
            const std::string of_pack = said_of(line, kPackName, " (the game takes");
            if (!of_image.empty()) start.summary = of_image;
            else if (!of_pack.empty()) start.summary += (start.summary.empty() ? "" : "; ") + of_pack;
        }
        if (start.summary.empty() && !start.output.empty()) start.summary = start.output.front();
    }
    if (!pack.empty() && !executable_supports(game.exe_path, kPackFlag)) {
        start.error = "This build of the game was made before packed data could be replaced (" + pack +
                      "), and would start without the pack. Rebuild the game.";
        return start;
    }
    start.args = launch_args(image, pack);
    return start;
}

std::string tree_dir(const std::string& data_dir, const std::string& game_id) {
    return join(join(data_dir, "data-trees"), game_id);
}

std::vector<std::string> export_command(const std::vector<std::string>& recompile, const std::string& export_file) {
    std::vector<std::string> command = recompile;
    command.push_back("--lift-export");
    command.push_back(export_file);
    return command;
}

std::vector<std::string> split_command(const Tool& tool, const std::string& export_file, const std::string& rom,
                                       const std::string& tree) {
    return {tool.python, "-X", "utf8", tool.script, "split", export_file, "--rom", rom, "-o", tree};
}

std::string make_tree(const std::vector<std::string>& recompile, const std::string& rom, const std::string& tree,
                      const Tool& tool, const Runner& run, std::vector<std::string>& output) {
    if (is_file(join(tree, kManifest))) {
        return tree + " already holds a data tree, and its files may have been edited. Move it or delete it to make "
                      "the tree afresh.";
    }
    if (recompile.empty()) return "The recompiler is not set: see Settings.";
    if (tool.python.empty() || tool.script.empty()) {
        return std::string(tool.python.empty() ? "Python" : "tools/gb_data.py") +
               " was not found, and a ROM is taken apart into a data tree with it. Set it in Settings.";
    }
    const std::string export_file = tree + ".export.json";
    std::error_code ec;
    fs::create_directories(to_path(tree).parent_path(), ec);
    if (ec) return "Cannot make " + from_path(to_path(tree).parent_path()) + ": " + ec.message() + ".";

    const size_t export_said = output.size();
    int exit_code = run(export_command(recompile, export_file), output);
    std::string error;
    if (exit_code != 0 || !is_file(export_file)) {
        const std::vector<std::string> said(output.begin() + static_cast<std::ptrdiff_t>(export_said), output.end());
        error = exit_code != 0 ? tool_message(said, exit_code, "the recompiler")
                               : "the recompiler wrote no description of the ROM to " + export_file;
    } else {
        const size_t split_said = output.size();
        exit_code = run(split_command(tool, export_file, rom, tree), output);
        const std::vector<std::string> said(output.begin() + static_cast<std::ptrdiff_t>(split_said), output.end());
        if (exit_code != 0) error = tool_message(said, exit_code);
        else if (!is_file(join(tree, kManifest))) error = "the data tool wrote no tree to " + tree;
    }
    fs::remove(to_path(export_file), ec);
    return error;
}

std::string log_file_of(const std::vector<std::string>& args) {
    std::string log;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--log-file") log = args[++i];
    }
    return log;
}

std::string refusal(const std::string& log_text) {
    // The two lines the generated main prints before it gives up (c_emitter.cpp).
    static const std::string image_line = "[GBRT] ROM image ";
    static const std::string pack_line = "[GBRT] Pack of unpacked data not used: ";
    static const std::string not_used = " not used: ";
    size_t start = 0;
    while (start < log_text.size()) {
        size_t end = log_text.find('\n', start);
        if (end == std::string::npos) end = log_text.size();
        std::string line = log_text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(pack_line, 0) == 0) {
            return "The game did not take the pack of unpacked data: " + line.substr(pack_line.size()) + ".";
        }
        // The image's path lies between the two halves, and may itself have " not used: " in it.
        const size_t why = line.rfind(not_used);
        if (line.rfind(image_line, 0) == 0 && why != std::string::npos && why >= image_line.size()) {
            return "The game did not take the image: " + line.substr(why + not_used.size()) + ".";
        }
    }
    return "";
}

std::string refusal_in_file(const std::string& log_path) {
    std::string text;
    if (log_path.empty() || !read_head(log_path, kHead, text)) return "";
    return refusal(text);
}

bool executable_supports(const std::string& exe_path, const std::string& flag) {
    std::ifstream in(to_path(exe_path), std::ios::binary);
    if (!in || flag.empty()) return false;
    std::vector<char> buffer(1 << 20);
    std::string carry;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        std::string chunk = carry;
        chunk.append(buffer.data(), static_cast<size_t>(got));
        // With the byte that ends it: --unpacked is not --unpacked-harvest.
        if (chunk.find(flag + '\0') != std::string::npos) return true;
        carry = chunk.substr(chunk.size() >= flag.size() + 1 ? chunk.size() - flag.size() : 0);
    }
    return false;
}

}  // namespace hub::edited_data
