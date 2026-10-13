#include "hub_platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;

namespace hub::platform {

fs::path to_path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.c_str()), utf8.size()));
}

std::string from_path(const fs::path& p) {
    std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return from_path(to_path(a) / to_path(b));
}

bool exists(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(to_path(path), ec);
}

bool is_directory(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(to_path(path), ec);
}

bool ensure_directory(const std::string& path, std::string& error) {
    std::error_code ec;
    fs::create_directories(to_path(path), ec);
    if (ec) {
        error = "cannot create directory " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool copy_file(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to_path(to).parent_path(), ec);
    ec.clear();
    fs::copy_file(to_path(from), to_path(to), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy " + from + " to " + to + ": " + ec.message();
        return false;
    }
    return true;
}

bool link_or_copy_file(const std::string& from, const std::string& to, bool& linked, std::string& error) {
    std::error_code ec;
    linked = false;
    fs::create_directories(to_path(to).parent_path(), ec);
    ec.clear();
    // Already the same file (a rebuild over an earlier link): nothing to do.
    if (fs::exists(to_path(to), ec) && fs::equivalent(to_path(from), to_path(to), ec)) {
        linked = true;
        return true;
    }
    ec.clear();
    fs::remove(to_path(to), ec);
    ec.clear();
    fs::create_hard_link(to_path(from), to_path(to), ec);
    if (!ec) {
        linked = true;
        return true;
    }
    return copy_file(from, to, error);
}

bool remove_tree(const std::string& path, std::string& error) {
    std::error_code ec;
    fs::remove_all(to_path(path), ec);
    if (ec) {
        error = "cannot remove " + path + ": " + ec.message();
        return false;
    }
    return true;
}

std::string file_name(const std::string& path) { return from_path(to_path(path).filename()); }
std::string file_stem(const std::string& path) { return from_path(to_path(path).stem()); }
std::string parent_path(const std::string& path) { return from_path(to_path(path).parent_path()); }

std::string file_extension_lower(const std::string& path) {
    std::string ext = from_path(to_path(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string absolute(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(to_path(path), ec);
    if (ec) return path;
    p = p.lexically_normal();
    return from_path(p);
}

long long file_size(const std::string& path) {
    std::error_code ec;
    auto size = fs::file_size(to_path(path), ec);
    return ec ? -1 : static_cast<long long>(size);
}

bool read_binary_file(const std::string& path, std::vector<uint8_t>& out, size_t max_bytes) {
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff len = in.tellg();
    in.seekg(0, std::ios::beg);
    if (len < 0) return false;
    size_t want = static_cast<size_t>(len);
    if (max_bytes && want > max_bytes) want = max_bytes;
    out.resize(want);
    if (want && !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(want))) return false;
    return true;
}

std::vector<std::string> list_files_recursive(const std::string& dir, const std::vector<std::string>& extensions_lower) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(to_path(dir), fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry& entry = *it;
        std::error_code fec;
        if (entry.is_regular_file(fec)) {
            std::string p = from_path(entry.path());
            std::string ext = file_extension_lower(p);
            if (extensions_lower.empty() || std::find(extensions_lower.begin(), extensions_lower.end(), ext) != extensions_lower.end()) {
                out.push_back(p);
            }
        }
        it.increment(ec);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> list_subdirectories(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::directory_iterator it(to_path(dir), fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    while (!ec && it != end) {
        std::error_code dec;
        if (it->is_directory(dec)) out.push_back(from_path(it->path().filename()));
        it.increment(ec);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

long long now_unix() {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string script_extension() {
#ifdef _WIN32
    return ".cmd";
#else
    return ".sh";
#endif
}

std::string quote_arg(const std::string& arg) {
#ifdef _WIN32
    if (arg.empty()) return "\"\"";
    bool needs = arg.find_first_of(" \t\"&|<>^()") != std::string::npos;
    if (!needs) return arg;
    std::string out = "\"";
    unsigned backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
#else
    if (arg.empty()) return "''";
    if (arg.find_first_of(" \t\"'\\$`&|<>;()*?[]#~") == std::string::npos) return arg;
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
#endif
}

namespace {

// Netpbm P6 (binary) reader; used for runtime frame dumps.
bool load_ppm(const std::string& path, Image& out, std::string& error) {
    std::vector<uint8_t> data;
    if (!read_binary_file(path, data)) {
        error = "cannot read " + path;
        return false;
    }
    size_t pos = 0;
    auto skip_ws_and_comments = [&]() {
        while (pos < data.size()) {
            if (data[pos] == '#') {
                while (pos < data.size() && data[pos] != '\n') ++pos;
            } else if (std::isspace(data[pos])) {
                ++pos;
            } else {
                break;
            }
        }
    };
    auto read_token = [&]() {
        skip_ws_and_comments();
        std::string tok;
        while (pos < data.size() && !std::isspace(data[pos])) tok.push_back(static_cast<char>(data[pos++]));
        return tok;
    };
    if (read_token() != "P6") {
        error = "not a binary PPM: " + path;
        return false;
    }
    int w = std::atoi(read_token().c_str());
    int h = std::atoi(read_token().c_str());
    int maxval = std::atoi(read_token().c_str());
    if (w <= 0 || h <= 0 || maxval <= 0 || maxval > 255) {
        error = "unsupported PPM header in " + path;
        return false;
    }
    ++pos;  // single whitespace byte after maxval
    size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (pos + need > data.size()) {
        error = "truncated PPM: " + path;
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i) {
        out.rgba[i * 4 + 0] = data[pos + i * 3 + 0];
        out.rgba[i * 4 + 1] = data[pos + i * 3 + 1];
        out.rgba[i * 4 + 2] = data[pos + i * 3 + 2];
        out.rgba[i * 4 + 3] = 255;
    }
    return true;
}

}  // namespace

bool load_image_native(const std::string& path, Image& out, std::string& error);

bool load_image(const std::string& path, Image& out, std::string& error) {
    std::string ext = file_extension_lower(path);
    if (ext == ".ppm") return load_ppm(path, out, error);
    return load_image_native(path, out, error);
}

}  // namespace hub::platform
