// GBA code image as seen by the recompiler: the ROM, plus optional IWRAM and
// EWRAM images from a runtime trace for code the game runs from RAM.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp {

constexpr uint32_t kRomBase = 0x08000000u;
constexpr uint32_t kEwramBase = 0x02000000u;
constexpr uint32_t kIwramBase = 0x03000000u;

inline bool is_ram_address(uint32_t addr) { return (addr >> 24) == 0x02u || (addr >> 24) == 0x03u; }

struct Rom {
    std::vector<uint8_t> data;
    std::vector<uint8_t> iwram; // empty unless loaded
    std::vector<uint8_t> ewram;
    std::string sha256;
    std::string title;     // header 0xA0, 12 bytes
    std::string game_code; // header 0xAC, 4 bytes

    bool load(const std::string &path, std::string &error);
    // RAM images written next to a trace by the runtime (<trace>.iwram, .ewram).
    bool load_ram_images(const std::string &trace_path, std::string &error);

    // Pointer to `size` bytes at `addr` in the ROM or a loaded RAM image.
    const uint8_t *ptr(uint32_t addr, uint32_t size) const
    {
        auto in = [&](const std::vector<uint8_t> &v, uint32_t base) -> const uint8_t * {
            if (addr < base) return nullptr;
            uint64_t off = static_cast<uint64_t>(addr) - base;
            return off + size <= v.size() ? &v[off] : nullptr;
        };
        switch (addr >> 24) {
        case 0x02: return in(ewram, kEwramBase);
        case 0x03: return in(iwram, kIwramBase);
        default: return in(data, kRomBase);
        }
    }
    bool contains(uint32_t addr, uint32_t size) const { return ptr(addr, size) != nullptr; }
    uint32_t read32(uint32_t addr) const
    {
        const uint8_t *p = ptr(addr, 4);
        return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    uint16_t read16(uint32_t addr) const
    {
        const uint8_t *p = ptr(addr, 2);
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
};

} // namespace gbarecomp
