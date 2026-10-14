// NES code image as seen by the recompiler: PRG ROM in 8 KiB pages, and what
// the cartridge's board does with them.
//
// A code location is a key (nes_recomp.h): the CPU address plus the PRG page
// mapped there. The board decides which pages can appear at which of the
// four 8 KiB slots ($8000, $A000, $C000, $E000); Banking describes that well
// enough to guess where a jump out of a page lands. The guesses only decide
// how much is compiled: at run time a key is looked up from the cartridge's
// real page table, so a wrong guess is code that is never entered.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "nes.h"
#include "nes_recomp.h"
}

namespace nesrecomp {

inline uint32_t make_key(uint32_t page, uint32_t pc) { return NES_RC_KEY(page, pc); }
inline uint16_t key_pc(uint32_t key) { return NES_RC_KEY_PC(key); }
inline uint32_t key_page(uint32_t key) { return NES_RC_KEY_PAGE(key); }
// Which 8 KiB slot of $8000-$FFFF an address is in (0-3).
inline unsigned slot_of(uint32_t pc) { return (pc >> 13) & 3u; }

struct Banking {
    // How many 8 KiB pages switch together (1, 2 or 4): a 16 KiB bank is two
    // consecutive pages in two consecutive slots.
    unsigned bank_pages = 4;
    // Per slot: the page that sits there in the board's usual configuration,
    // or -1 when the slot is switched.
    int fixed_page[4] = { -1, -1, -1, -1 };
};

class Rom {
public:
    bool load(const std::string &path, std::string &error);

    uint32_t pages() const { return info.prg_size / NES_PRG_PAGE; }
    // Is there PRG ROM under this key?
    bool is_code(uint32_t key) const { return key_pc(key) >= 0x8000u && key_page(key) < pages(); }
    // Offset in PRG ROM of the byte a key names.
    uint32_t offset(uint32_t key) const { return key_page(key) * NES_PRG_PAGE + (key_pc(key) & 0x1FFFu); }
    uint8_t read(uint32_t key) const { return prg[offset(key)]; }

    // The pages a jump from `from` to address `target` may land in, most
    // likely first. `certain` is set when there is only one answer.
    std::vector<uint32_t> pages_at(uint32_t from, uint16_t target, bool &certain) const;
    // Every page that can sit in a slot.
    std::vector<uint32_t> pages_in_slot(unsigned slot) const;
    // The page in a slot at power-on (where the reset vector is read from).
    uint32_t power_on_page(unsigned slot) const;

    std::vector<uint8_t> file; // the whole .nes file
    const uint8_t *prg = nullptr;
    NesCartInfo info{};
    Banking banking;
    std::string sha256;
    std::string title;
};

} // namespace nesrecomp
