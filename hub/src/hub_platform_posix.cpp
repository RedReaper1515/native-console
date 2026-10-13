// POSIX implementation of the hub platform layer (Linux/macOS).
// Dialogs use zenity/osascript when available; images support PPM only until an
// image decoder is vendored. This keeps the hub buildable off Windows.
#ifndef _WIN32

#include "hub_platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace hub::platform {

std::string executable_path() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0) return "";
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::path(buffer.c_str()), ec);
    return ec ? std::string(buffer.c_str()) : from_path(resolved);
#else
    char buffer[4096];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0) return "";
    buffer[len] = '\0';
    return buffer;
#endif
}

std::string executable_dir() {
    const std::string exe = executable_path();
    return exe.empty() ? "." : from_path(fs::path(exe).parent_path());
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}

std::string default_data_dir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base = xdg ? xdg : join(home_dir(), ".local/share");
    return join(join(base, "NativeConsole"), "Hub");
}

std::string getenv_utf8(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string find_on_path(const std::string& executable_name) {
    std::string path = getenv_utf8("PATH");
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::string candidate = join(dir, executable_name);
        if (!dir.empty() && access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

namespace {

bool run_dialog_command(const std::string& command, std::vector<std::string>& out) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return false;
    char buffer[4096];
    std::string text;
    while (fgets(buffer, sizeof(buffer), pipe)) text += buffer;
    int rc = pclose(pipe);
    if (rc != 0) return false;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty()) out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return !out.empty();
}

}  // namespace

bool pick_files(const std::vector<FileFilter>& filters, bool allow_multiple, std::vector<std::string>& out_paths) {
    std::string zenity = find_on_path("zenity");
    if (!zenity.empty()) {
        std::string cmd = quote_arg(zenity) + " --file-selection --separator='\\n'";
        if (allow_multiple) cmd += " --multiple";
        for (const auto& f : filters) {
            std::string pattern = f.pattern;
            for (char& c : pattern) if (c == ';') c = ' ';
            cmd += " --file-filter=" + quote_arg(f.label + " | " + pattern);
        }
        return run_dialog_command(cmd, out_paths);
    }
    std::string osascript = find_on_path("osascript");
    if (!osascript.empty()) {
        std::string script = allow_multiple
            ? "set fs to choose file with multiple selections allowed\nset out to \"\"\nrepeat with f in fs\nset out to out & POSIX path of f & \"\\n\"\nend repeat\nreturn out"
            : "return POSIX path of (choose file)";
        return run_dialog_command(quote_arg(osascript) + " -e " + quote_arg(script), out_paths);
    }
    return false;
}

bool pick_folder(std::string& out_path) {
    std::vector<std::string> paths;
    std::string zenity = find_on_path("zenity");
    if (!zenity.empty() && run_dialog_command(quote_arg(zenity) + " --file-selection --directory", paths)) {
        out_path = paths.front();
        return true;
    }
    std::string osascript = find_on_path("osascript");
    if (!osascript.empty() && run_dialog_command(quote_arg(osascript) + " -e 'return POSIX path of (choose folder)'", paths)) {
        out_path = paths.front();
        return true;
    }
    return false;
}

void open_in_file_manager(const std::string& path) {
#ifdef __APPLE__
    std::string cmd = "open " + quote_arg(path) + " &";
#else
    std::string cmd = "xdg-open " + quote_arg(path) + " &";
#endif
    (void)std::system(cmd.c_str());
}

void show_message_box(const std::string& title, const std::string& message) {
    std::fprintf(stderr, "[%s] %s\n", title.c_str(), message.c_str());
}

std::string script_command(const std::string& script_path) {
    return "/bin/sh " + quote_arg(script_path);
}

RunResult run_capture(const std::string& command_line,
                      const std::string& working_dir,
                      const std::function<void(const std::string& line)>& on_line,
                      std::atomic<bool>* cancel_flag) {
    RunResult result;
    int fds[2];
    if (pipe(fds) != 0) {
        result.error = "pipe failed";
        return result;
    }
    pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        return result;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (!working_dir.empty()) (void)chdir(working_dir.c_str());
        execl("/bin/sh", "sh", "-c", command_line.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    result.started = true;
    std::string pending;
    char buffer[4096];
    while (true) {
        if (cancel_flag && cancel_flag->load()) {
            kill(pid, SIGTERM);
            on_line("[hub] cancelled");
            break;
        }
        ssize_t got = read(fds[0], buffer, sizeof(buffer));
        if (got <= 0) break;
        pending.append(buffer, static_cast<size_t>(got));
        size_t start = 0;
        while (true) {
            size_t nl = pending.find('\n', start);
            if (nl == std::string::npos) break;
            on_line(pending.substr(start, nl - start));
            start = nl + 1;
        }
        pending.erase(0, start);
    }
    if (!pending.empty()) on_line(pending);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

bool launch_detached(const std::string& executable,
                     const std::vector<std::string>& args,
                     const std::string& working_dir,
                     bool hide_console,
                     Process& out_process,
                     std::string& error) {
    (void)hide_console;
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        if (!working_dir.empty()) (void)chdir(working_dir.c_str());
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    out_process.pid = static_cast<unsigned long>(pid);
    out_process.handle = nullptr;
    return true;
}

bool process_running(Process& process) {
    if (!process.pid) return false;
    int status = 0;
    pid_t r = waitpid(static_cast<pid_t>(process.pid), &status, WNOHANG);
    if (r == 0) return true;
    process.pid = 0;
    return false;
}

void process_release(Process& process) {
    process.pid = 0;
    process.handle = nullptr;
}

void process_terminate(Process& process) {
    if (process.pid) kill(static_cast<pid_t>(process.pid), SIGTERM);
}

bool tie_to_process_lifetime(Process& process) {
    (void)process;
    return false;
}

bool process_running_by_name(const std::string& exe_file_name) {
    std::error_code ec;
    for (auto it = fs::directory_iterator("/proc", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::ifstream in(it->path() / "comm");
        std::string comm;
        if (in && std::getline(in, comm) && comm == exe_file_name) return true;
    }
    return false;
}

std::string steam_install_dir() {
    const std::string home = home_dir();
    const std::string candidates[] = {join(home, ".steam/steam"), join(home, ".local/share/Steam"),
                                      join(home, "Library/Application Support/Steam")};
    for (const std::string& c : candidates) {
        if (is_directory(join(c, "userdata"))) return c;
    }
    return "";
}

long long file_size_on_disk(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return file_size(path);
    return static_cast<long long>(st.st_blocks) * 512;
}

bool supports_transparent_compression() { return false; }

bool compress_file_transparent(const std::string& path, std::string& error) {
    (void)path;
    error = "transparent executable compression is not available on this platform";
    return false;
}

bool load_image_native(const std::string& path, Image& out, std::string& error) {
    (void)path;
    (void)out;
    error = "only PPM images are supported on this platform for now";
    return false;
}

bool save_image_png(const std::string& path, const Image& image, std::string& error) {
    (void)path;
    (void)image;
    error = "PNG encoding is not available on this platform yet";
    return false;
}

}  // namespace hub::platform

#endif  // !_WIN32
