#include "image.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

extern "C" {
#include "n64_sha256.h"
}

namespace n64recomp {

bool Image::load(const std::string &path, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // The runtime normalises byte order and parses the header; use it so the
    // two never disagree.
    N64Machine *m = n64_create();
    if (!m) {
        error = "out of memory";
        return false;
    }
    if (!n64_load_rom(m, data.data(), data.size())) {
        n64_destroy(m);
        error = path + ": not an N64 ROM (too small)";
        return false;
    }
    rom.assign(m->rom, m->rom + m->rom_size);
    cart = m->cart;
    // The static view is what IPL3 leaves: the game's first megabyte at its
    // entry point. (At run time a known IPL3 runs and puts it there.)
    m->hle_boot = true;
    n64_power(m);
    entry_vaddr = (uint32_t)m->cpu.pc;
    View boot;
    boot.boot = true;
    boot.paddr = entry_vaddr & 0x1FFFFFFFu;
    if (boot.paddr >= m->rdram_size) {
        n64_destroy(m);
        error = path + ": the entry point is outside RDRAM";
        return false;
    }
    uint32_t size = 0x100000u;
    if (boot.paddr + size > m->rdram_size) size = m->rdram_size - boot.paddr;
    boot.bytes.assign(m->rdram + boot.paddr, m->rdram + boot.paddr + size);
    views.push_back(std::move(boot));
    n64_destroy(m);
    char digest[65];
    n64_sha256_hex(rom.data(), rom.size(), digest);
    sha256 = digest;
    return true;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

bool Image::add_trace(const std::string &path, std::string &error)
{
    std::ifstream in(path);
    if (!in) {
        error = "cannot read trace " + path;
        return false;
    }
    View full; // this trace's final image, if it has one
    full.full = true;
    auto page_bytes = [&](const std::string &line, uint8_t *out) {
        for (size_t k = 0; k < 0x1000; k++) {
            int hi = hexval(line[11 + 2 * k]), lo = hexval(line[12 + 2 * k]);
            if (hi < 0 || lo < 0) return false;
            out[k] = (uint8_t)(hi << 4 | lo);
        }
        return true;
    };
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2 || line[0] == '#') continue;
        if (line[0] == 'F' && line.size() >= 2 + 9 + 0x2000) {
            const uint32_t pa = (uint32_t)std::stoul(line.substr(2, 8), nullptr, 16);
            if (pa & 0xFFFu || pa + 0x1000u > N64_RDRAM_MAX) continue;
            if (full.bytes.empty()) {
                full.bytes.assign(N64_RDRAM_MAX, 0);
                full.present.assign(N64_RDRAM_MAX >> 12, 0);
                full.ran.assign(N64_RDRAM_MAX >> 12, 0);
            }
            if (!page_bytes(line, &full.bytes[pa])) {
                error = path + ": malformed page line";
                return false;
            }
            full.present[pa >> 12] = 1;
            continue;
        }
        if (line[0] == 'E') {
            TraceEntry e;
            std::istringstream s(line.substr(2));
            s >> std::hex >> e.vaddr >> e.paddr;
            if (s) trace_entries.push_back(e);
        } else if (line[0] == 'P' && line.size() >= 2 + 9 + 0x2000) {
            View v;
            v.paddr = (uint32_t)std::stoul(line.substr(2, 8), nullptr, 16);
            v.bytes.resize(0x1000);
            for (size_t k = 0; k < 0x1000; k++) {
                int hi = hexval(line[11 + 2 * k]), lo = hexval(line[12 + 2 * k]);
                if (hi < 0 || lo < 0) {
                    error = path + ": malformed page line";
                    return false;
                }
                v.bytes[k] = (uint8_t)(hi << 4 | lo);
            }
            // A snapshot that only repeats the boot image adds nothing.
            const View &b = views[0];
            if (b.contains(v.paddr, 0x1000) &&
                std::equal(v.bytes.begin(), v.bytes.end(), b.bytes.begin() + (v.paddr - b.paddr)))
                continue;
            bool dup = false;
            for (const View &o : views) dup = dup || (!o.boot && o.paddr == v.paddr && o.bytes == v.bytes);
            if (!dup) views.push_back(std::move(v));
        }
    }
    if (full.bytes.empty()) return true;

    // Where the final image holds the bytes code ran from, a page snapshot
    // adds nothing to it; where it does not, the snapshot stays (an overlay
    // since replaced, or a page whose data changed).
    const View &b = views[0];
    for (uint32_t page = 0; page < full.ran.size(); page++) {
        const uint32_t lo = std::max(page << 12, b.paddr), hi = std::min((page + 1u) << 12, b.paddr + (uint32_t)b.bytes.size());
        if (lo < hi && std::equal(full.bytes.begin() + lo, full.bytes.begin() + hi, b.bytes.begin() + (lo - b.paddr)))
            full.ran[page] = 1;
    }
    for (size_t i = 1; i < views.size();) {
        const View &v = views[i];
        if (!v.full && v.bytes.size() == 0x1000 &&
            std::equal(v.bytes.begin(), v.bytes.end(), full.bytes.begin() + v.paddr)) {
            full.ran[v.paddr >> 12] = 1;
            views.erase(views.begin() + (std::ptrdiff_t)i);
        } else {
            i++;
        }
    }
    views.push_back(std::move(full));
    return true;
}

} // namespace n64recomp
