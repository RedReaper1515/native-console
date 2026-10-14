#include "rom.h"

#include <fstream>
#include <iterator>

extern "C" {
#include "gba_sha256.h"
}

namespace gbarecomp {

static std::string header_string(const std::vector<uint8_t> &data, size_t offset, size_t length)
{
    std::string out;
    for (size_t i = 0; i < length && offset + i < data.size(); i++) {
        char c = static_cast<char>(data[offset + i]);
        if (c == '\0') break;
        out += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return out;
}

bool Rom::load(const std::string &path, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (data.size() < 0xC0 || data.size() > 0x2000000) {
        error = path + ": not a GBA ROM (size " + std::to_string(data.size()) + ")";
        return false;
    }
    if (data[0xB2] != 0x96) {
        error = path + ": header fixed byte at 0xB2 is not 0x96";
        return false;
    }
    char digest[65];
    gba_sha256_hex(data.data(), data.size(), digest);
    sha256 = digest;
    title = header_string(data, 0xA0, 12);
    game_code = header_string(data, 0xAC, 4);
    return true;
}

static bool read_exact(const std::string &path, size_t size, std::vector<uint8_t> &out, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.size() != size) {
        error = path + ": expected " + std::to_string(size) + " bytes, found " + std::to_string(out.size());
        return false;
    }
    return true;
}

bool Rom::load_ram_images(const std::string &trace_path, std::string &error)
{
    return read_exact(trace_path + ".iwram", 0x8000, iwram, error) &&
           read_exact(trace_path + ".ewram", 0x40000, ewram, error);
}

} // namespace gbarecomp
