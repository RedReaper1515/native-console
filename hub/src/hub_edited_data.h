// A Game Boy game started with data somebody edited (docs/DATA_FILES.md): what a
// game remembers (a data tree folder, or an image already built from one),
// building a tree into an image and its pack before the start
// (tools/gb_data.py build), the arguments the game takes them with
// (--rom-image, --unpacked), and what the game said if it refused them.
// The tool and the game own the formats and check everything; the hub reads
// only the head of a tree's manifest, to tell which ROM the tree is of.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace hub::edited_data {

enum class Kind {
    None,      // nothing chosen: the game starts with the data built into it
    Tree,      // a data tree (gb_data.py split), built before every start
    Image,     // an image gb_data.py build wrote, with its pack if one is beside it
    Unusable,  // not there, or not of this game's ROM
};

struct Choice {
    Kind kind = Kind::None;
    std::string path;   // what the game remembers
    std::string pack;   // Image: the pack beside it (the image's name with .gbup), "" when there is none
    std::string error;  // Unusable: why
};

// What a remembered path is, for a game whose ROM has this SHA-256 (lowercase hex).
Choice describe(const std::string& path, const std::string& rom_sha256);

// What builds a tree: Python, and tools/gb_data.py. Either may be "".
struct Tool {
    std::string python;
    std::string script;
};

// tools/gb_data.py as a hub in hub_dir finds it: beside the hub, or in the
// checkout the hub was built in (build/bin). "" when it is in neither place:
// a release archive does not carry it.
std::string find_script(const std::string& hub_dir);

// games/<id>/edited-data, where a tree's image and pack are built. The hub
// makes it only for a game that has a tree chosen.
std::string build_dir(const std::string& game_dir);
// Deletes that folder. True when it is gone, or was never there.
bool remove_build(const std::string& game_dir, std::string& error);

// The command that builds a tree: python -X utf8 gb_data.py build TREE -o IMAGE.
std::vector<std::string> build_command(const Tool& tool, const std::string& tree, const std::string& image);

// {"--rom-image", image}, and {"--unpacked", pack} after it when there is a pack.
std::vector<std::string> launch_args(const std::string& image, const std::string& pack);

// Runs a command, adds what it printed to `output` a line at a time, and
// gives back its exit code: -1, with the reason as a line, if it did not start.
using Runner = std::function<int(const std::vector<std::string>& argv, std::vector<std::string>& output)>;

struct Game {
    std::string dir;         // games/<id>
    std::string exe_path;
    std::string rom_sha256;
};

struct Start {
    std::vector<std::string> args;    // for the game's command line; none when nothing is chosen
    std::string error;                // why the game is not to be started; "" when it is
    std::string summary;              // Tree: what the tool said it built ("2 of 5377 files edited")
    std::vector<std::string> output;  // Tree: everything the tool printed
};

// Makes ready what a game is started with. Nothing chosen: no arguments, and
// nothing is read, run or written. A tree: the tool builds it into
// build_dir(), and a build the tool refuses (an edit to code, a picture of
// the wrong size) is the error, in the tool's words. An image: the file as
// it is. A game built before --rom-image existed would ignore the argument
// and start with its own data, so that is an error too.
Start prepare(const std::string& chosen, const Game& game, const Tool& tool, const Runner& run);

// Where the hub makes a game's data tree: <data dir>/data-trees/<id>. Outside
// the game's own folder, like its recorded runs: the files in it get edited,
// and rebuilding the game or taking it out of the library must leave them.
std::string tree_dir(const std::string& data_dir, const std::string& game_id);

// The command that describes a ROM for its tree: the one a build recompiles
// the ROM with, told to write the lift export in place of code
// (--lift-export). The analysis is then the build's, option for option, so
// what the tree calls data is data to the built game.
std::vector<std::string> export_command(const std::vector<std::string>& recompile, const std::string& export_file);

// The command that takes the ROM apart: python -X utf8 gb_data.py split EXPORT --rom ROM -o TREE.
std::vector<std::string> split_command(const Tool& tool, const std::string& export_file, const std::string& rom,
                                       const std::string& tree);

// Makes a data tree of `rom` in `tree`: the export, then the split, and the
// export (tens of megabytes) deleted again. A folder that already holds a
// tree is left alone, since its files may have been edited. Gives back why
// no tree was made; "" when one was.
std::string make_tree(const std::vector<std::string>& recompile, const std::string& rom, const std::string& tree,
                      const Tool& tool, const Runner& run, std::vector<std::string>& output);

// The file a command line sends the game's output to (its last --log-file); "" when none.
std::string log_file_of(const std::vector<std::string>& args);

// What a game's log says of an image or a pack it did not take: the game
// checks both as it starts, says which byte or what else is wrong, and
// exits. "" when the log has no such line.
std::string refusal(const std::string& log_text);
// The same, of the head of a log file. "" when the file cannot be read.
std::string refusal_in_file(const std::string& log_path);

// True when the executable knows the flag. A game ignores an argument it
// does not know, so the hub looks before it relies on one.
bool executable_supports(const std::string& exe_path, const std::string& flag);

}  // namespace hub::edited_data
