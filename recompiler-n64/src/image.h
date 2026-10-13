// What n64recomp compiles from: the ROM, and views of RDRAM holding code.
//
// N64 code runs from RDRAM. View 0 is RDRAM as the boot leaves it (the
// first megabyte of the game at its load address). Traces from earlier
// runs (n64recomp --profile, a game's --trace-out) add 4 KiB page
// snapshots of RDRAM as it was when code there ran - code the game copied
// or decompressed in - and the addresses where execution entered. A trace
// also ends with all of RDRAM as its run left it: one view (full) in which
// a function can cross a page and a call can be followed into code the run
// never reached.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "n64.h"
}

namespace n64recomp {

struct View {
    uint32_t paddr = 0;
    std::vector<uint8_t> bytes;
    bool boot = false;
    // A whole RDRAM image. Per 4 KiB page: present (the image holds bytes
    // for it; the rest is empty RAM, not code) and ran (code is known to
    // have run from exactly these bytes: a page snapshot or the boot image
    // equals it).
    bool full = false;
    std::vector<uint8_t> present, ran;

    bool contains(uint32_t pa, uint32_t size = 4) const { return pa >= paddr && pa - paddr + size <= bytes.size(); }
    uint32_t word(uint32_t pa) const
    {
        const uint8_t *p = &bytes[pa - paddr];
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
};

struct TraceEntry {
    uint32_t vaddr = 0, paddr = 0;
};

class Image {
public:
    bool load(const std::string &path, std::string &error);
    bool add_trace(const std::string &path, std::string &error);

    std::vector<uint8_t> rom; // normalised to big-endian
    N64Cart cart{};
    std::string sha256;
    uint32_t entry_vaddr = 0; // where the boot jumps
    std::vector<View> views;  // [0] = the boot image
    std::vector<TraceEntry> trace_entries;
};

} // namespace n64recomp
