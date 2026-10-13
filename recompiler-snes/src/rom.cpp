#include "rom.h"

#include <cstring>
#include <fstream>
#include <iterator>

extern "C" {
#include "snes_recomp.h"
#include "snes_sha256.h"
}

namespace snesrecomp {

bool Rom::load(const std::string &path, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    machine_.reset(snes_create());
    if (!machine_ || !snes_load_rom(machine_.get(), file.data(), file.size())) {
        error = path + ": not a SNES ROM (no valid header found)";
        return false;
    }
    // The machine dropped any copier header; keep the image it uses.
    data.assign(machine_->rom, machine_->rom + machine_->rom_size);
    char digest[65];
    snes_sha256_hex(data.data(), data.size(), digest);
    sha256 = digest;
    title = machine_->cart.title;
    return true;
}

bool Rom::load_wram_image(const std::string &trace_path, std::string &error)
{
    std::ifstream in(trace_path + ".wram", std::ios::binary);
    if (!in) {
        error = "cannot read " + trace_path + ".wram";
        return false;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (image.size() != SNES_WRAM_SIZE) {
        error = trace_path + ".wram: expected " + std::to_string(SNES_WRAM_SIZE) + " bytes";
        return false;
    }
    std::memcpy(machine_->wram, image.data(), image.size());
    has_wram = true;
    return true;
}

bool Rom::is_wram(uint32_t pc24) const { return snes_rc_wram_offset(pc24) >= 0; }

bool Rom::is_code(uint32_t pc24) const
{
    if (is_wram(pc24)) return has_wram;
    return machine_->page_type[(pc24 & 0xFFFFFFu) >> 12] == SNES_PAGE_ROM;
}

} // namespace snesrecomp
