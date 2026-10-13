#include "rom.h"

#include <filesystem>
#include <fstream>
#include <iterator>

extern "C" {
#include "nes_sha256.h"
}

namespace nesrecomp {

namespace {

// What each board (runtime-nes/src/nes_cart.c) does with PRG ROM.
Banking banking_for(const NesCartInfo &info)
{
    Banking b;
    const int n = static_cast<int>(info.prg_size / NES_PRG_PAGE);
    switch (info.mapper) {
    case 0:
    case 3: // no PRG banking; a 16 KiB ROM is mirrored
        b.bank_pages = 4;
        for (int s = 0; s < 4; s++) b.fixed_page[s] = s % n;
        break;
    case 1:  // MMC1: 16 KiB at $8000, the last bank at $C000 (its usual mode)
    case 2:  // UxROM
    case 10: // MMC4
    case 71: // Camerica
        b.bank_pages = 2;
        b.fixed_page[2] = n - 2;
        b.fixed_page[3] = n - 1;
        break;
    case 4: // MMC3: 8 KiB banks; only $E000 is always the last one
        b.bank_pages = 1;
        b.fixed_page[3] = n - 1;
        break;
    case 9: // MMC2: 8 KiB at $8000, the last three fixed
        b.bank_pages = 1;
        b.fixed_page[1] = n - 3;
        b.fixed_page[2] = n - 2;
        b.fixed_page[3] = n - 1;
        break;
    default: // AxROM, Color Dreams, BNROM, GxROM: all 32 KiB switches
        b.bank_pages = 4;
        break;
    }
    return b;
}

} // namespace

bool Rom::load(const std::string &path, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    file.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!nes_cart_parse(file.data(), file.size(), &info)) {
        error = path + ": not an iNES image";
        return false;
    }
    if (!info.board) {
        error = path + ": mapper " + std::to_string(info.mapper) + " is not supported";
        return false;
    }
    if (info.prg_size % NES_PRG_PAGE != 0) {
        error = path + ": PRG ROM is not a multiple of 8 KiB";
        return false;
    }
    prg = file.data() + info.prg_offset;
    banking = banking_for(info);
    char digest[65];
    nes_sha256_hex(file.data(), file.size(), digest);
    sha256 = digest;
    title = std::filesystem::path(path).stem().string();
    return true;
}

std::vector<uint32_t> Rom::pages_in_slot(unsigned slot) const
{
    std::vector<uint32_t> out;
    if (banking.fixed_page[slot] >= 0) {
        out.push_back(static_cast<uint32_t>(banking.fixed_page[slot]));
        return out;
    }
    for (uint32_t p = 0; p < pages(); p++) {
        if (p % banking.bank_pages == slot % banking.bank_pages) out.push_back(p);
    }
    return out;
}

uint32_t Rom::power_on_page(unsigned slot) const
{
    if (banking.fixed_page[slot] >= 0) return static_cast<uint32_t>(banking.fixed_page[slot]);
    // The runtime's boards power on with their first bank selected.
    return (slot % banking.bank_pages) % pages();
}

std::vector<uint32_t> Rom::pages_at(uint32_t from, uint16_t target, bool &certain) const
{
    std::vector<uint32_t> out;
    certain = false;
    if (target < 0x8000u) return out;
    const unsigned from_slot = slot_of(key_pc(from)), slot = slot_of(target);
    const uint32_t page = key_page(from);
    if (slot == from_slot) {
        certain = true;
        out.push_back(page);
        return out;
    }
    if (banking.fixed_page[slot] >= 0) {
        out.push_back(static_cast<uint32_t>(banking.fixed_page[slot]));
        return out;
    }
    // Another slot of the bank this code is in moves with it.
    const unsigned bp = banking.bank_pages;
    if (bp > 1 && slot / bp == from_slot / bp && page % bp == from_slot % bp) {
        out.push_back(page - from_slot + slot);
        return out;
    }
    return pages_in_slot(slot);
}

} // namespace nesrecomp
