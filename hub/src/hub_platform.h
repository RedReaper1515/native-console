// Host-specific services for the hub: dialogs, processes, images, paths.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hub::platform {

// UTF-8 <-> std::filesystem::path helpers. All hub strings are UTF-8.
std::filesystem::path to_path(const std::string& utf8);
std::string from_path(const std::filesystem::path& p);
std::string join(const std::string& a, const std::string& b);
bool exists(const std::string& path);
bool is_directory(const std::string& path);
bool ensure_directory(const std::string& path, std::string& error);
bool copy_file(const std::string& from, const std::string& to, std::string& error);
// A second name for the same file where the volume allows it (a hard link), else
// a copy. `to` is replaced. `linked` says which happened.
bool link_or_copy_file(const std::string& from, const std::string& to, bool& linked, std::string& error);
bool remove_tree(const std::string& path, std::string& error);
std::string file_name(const std::string& path);
std::string file_stem(const std::string& path);
std::string file_extension_lower(const std::string& path);
std::string parent_path(const std::string& path);
std::string absolute(const std::string& path);
long long file_size(const std::string& path);
long long file_size_on_disk(const std::string& path);  // allocated bytes; smaller than file_size when transparently compressed

// Applies OS-level transparent compression so the file stays directly executable
// (NTFS LZX via compact.exe on Windows). No-op elsewhere.
bool supports_transparent_compression();
bool compress_file_transparent(const std::string& path, std::string& error);
bool read_binary_file(const std::string& path, std::vector<uint8_t>& out, size_t max_bytes = 0);
std::vector<std::string> list_files_recursive(const std::string& dir, const std::vector<std::string>& extensions_lower);
// The names of the folders directly inside dir, sorted. Empty if dir is not there.
std::vector<std::string> list_subdirectories(const std::string& dir);

std::string executable_dir();
std::string executable_path();  // absolute path of the running hub executable, "" when unknown
std::string default_data_dir();
std::string home_dir();
std::string now_iso8601();
long long now_unix();
std::string find_on_path(const std::string& executable_name);
std::string getenv_utf8(const char* name);

struct FileFilter {
    std::string label;    // "Game Boy ROMs"
    std::string pattern;  // "*.gb;*.gbc"
};

bool pick_files(const std::vector<FileFilter>& filters, bool allow_multiple, std::vector<std::string>& out_paths);
bool pick_folder(std::string& out_path);
void open_in_file_manager(const std::string& path);
void show_message_box(const std::string& title, const std::string& message);

// Runs a child process, capturing stdout/stderr line by line. Blocks until exit or cancel.
struct RunResult {
    bool started = false;
    int exit_code = -1;
    std::string error;
};
RunResult run_capture(const std::string& command_line,
                      const std::string& working_dir,
                      const std::function<void(const std::string& line)>& on_line,
                      std::atomic<bool>* cancel_flag);

// Wraps a script path into the command line the host shell needs to execute it.
std::string script_command(const std::string& script_path);
std::string script_extension();  // ".cmd" or ".sh"
std::string quote_arg(const std::string& arg);

// Detached child process (a launched game). Handles are released by ~Process.
struct Process {
    void* handle = nullptr;
    unsigned long pid = 0;
    bool valid() const { return handle != nullptr || pid != 0; }
};
bool launch_detached(const std::string& executable,
                     const std::vector<std::string>& args,
                     const std::string& working_dir,
                     bool hide_console,
                     Process& out_process,
                     std::string& error);
bool process_running(Process& process);
void process_release(Process& process);
void process_terminate(Process& process);
// Ends the child when this process exits for any reason (a Windows job object),
// so stopping a game from Steam also closes it. False when unsupported.
bool tie_to_process_lifetime(Process& process);
// True when a process with this executable file name is running (case-insensitive on Windows).
bool process_running_by_name(const std::string& exe_file_name);
// Steam's install folder, "" when Steam is not installed.
std::string steam_install_dir();

// Decoded RGBA8 image.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};
bool load_image(const std::string& path, Image& out, std::string& error);
bool save_image_png(const std::string& path, const Image& image, std::string& error);

}  // namespace hub::platform
