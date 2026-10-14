/*
 * Cartridge: header detection, the memory map (LoROM, HiROM, ExHiROM,
 * SA-1) as a table of 4 KiB pages, ROM mirroring and battery RAM.
 *
 * A logical SNES address is not a file offset: the mapper decides. The
 * table gives each page of the 24-bit address space a type and the offset
 * of its first byte in ROM or SRAM; ROM that is not a power of two in size
 * mirrors the way the address decoders do (the largest power-of-two part
 * first, then the rest repeated).
 */
#include "snes.h"

#include <string.h>

/* Map `addr` into a ROM of `size` bytes the way cartridge address lines
 * mirror: split off the largest power of two, repeat the remainder. */
uint32_t snes_rom_mirror(uint32_t addr, uint32_t size)
{
    if (size == 0) return 0;
    uint32_t base = 0, mask = 1u << 23;
    while (addr >= size) {
        while (!(addr & mask)) mask >>= 1;
        addr -= mask;
        if (size > mask) {
            size -= mask;
            base += mask;
        }
        mask >>= 1;
    }
    return base + addr;
}

static int score_header(const uint8_t *rom, size_t size, uint32_t at, SnesMapper expected)
{
    if (at + 0x40 > size) return -1000;
    const uint8_t *h = rom + at;
    int score = 0;
    uint16_t checksum = (uint16_t)(h[0x1E] | (h[0x1F] << 8));
    uint16_t complement = (uint16_t)(h[0x1C] | (h[0x1D] << 8));
    if ((uint16_t)(checksum + complement) == 0xFFFFu) score += 8;
    uint8_t map = h[0x15];
    switch (expected) {
    case SNES_MAP_LOROM:
        if (map == 0x20 || map == 0x30 || map == 0x23 || map == 0x32) score += 6;
        break;
    case SNES_MAP_HIROM:
        if (map == 0x21 || map == 0x31) score += 6;
        break;
    case SNES_MAP_EXHIROM:
        if (map == 0x25 || map == 0x35) score += 8;
        break;
    default: break;
    }
    if (h[0x17] >= 0x07 && h[0x17] <= 0x0D) score += 2; /* ROM size 128 KiB - 8 MiB */
    if (h[0x18] <= 0x08) score += 1;
    if (h[0x19] <= 0x14) score += 1;
    int printable = 0;
    for (int i = 0; i < 21; i++) printable += (h[i] >= 0x20 && h[i] < 0x7F);
    score += printable / 7;
    /* Reset vector in ROM space, and a plausible first instruction. */
    uint16_t reset = (uint16_t)(h[0x3C] | (h[0x3D] << 8));
    if (reset >= 0x8000) {
        score += 2;
        uint32_t off = expected == SNES_MAP_LOROM ? (at & ~0x7FFFu) + (reset - 0x8000u) : (at & ~0xFFFFu) + reset;
        if (off < size) {
            uint8_t op = rom[off];
            if (op == 0x78 || op == 0x18 || op == 0x38 || op == 0x9C || op == 0x4C || op == 0x5C || op == 0xC2 ||
                op == 0xE2 || op == 0xA9 || op == 0xA2 || op == 0x20 || op == 0x22 || op == 0xAD)
                score += 3;
        }
    } else {
        score -= 4;
    }
    return score;
}

bool snes_cart_parse(const uint8_t *rom, size_t size, SnesCartInfo *info)
{
    memset(info, 0, sizeof *info);
    if (size < 0x8000) return false;
    struct { uint32_t at; SnesMapper mapper; } candidates[] = {
        { 0x7FC0u, SNES_MAP_LOROM },
        { 0xFFC0u, SNES_MAP_HIROM },
        { 0x40FFC0u, SNES_MAP_EXHIROM },
    };
    int best = -1, best_score = -1000;
    for (int i = 0; i < 3; i++) {
        int s = score_header(rom, size, candidates[i].at, candidates[i].mapper);
        if (s > best_score) {
            best_score = s;
            best = i;
        }
    }
    if (best < 0 || best_score < 4) return false;
    const uint8_t *h = rom + candidates[best].at;
    info->header_at = candidates[best].at;
    info->mapper = candidates[best].mapper;
    info->map_mode = h[0x15];
    info->cart_type = h[0x16];
    info->region = h[0x19];
    info->rom_size = (uint32_t)size;
    info->fastrom = (h[0x15] & 0x10) != 0;
    int t = 0;
    for (int i = 0; i < 21; i++) {
        uint8_t ch = h[i];
        info->title[t++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : ' ';
    }
    while (t > 0 && info->title[t - 1] == ' ') t--;
    info->title[t] = '\0';

    /* Cartridge type: low nibble 0 ROM, 1 +RAM, 2 +RAM+battery; 3-6 add a
     * coprocessor (high nibble picks which). */
    uint8_t type = h[0x16];
    uint8_t low = type & 0x0F, chip = type >> 4;
    bool has_ram = low == 1 || low == 2 || low == 4 || low == 5;
    info->battery = low == 2 || low == 5 || low == 6;
    if (h[0x18] && h[0x18] <= 0x08 && (has_ram || info->battery)) info->sram_size = 0x400u << h[0x18];
    if (low >= 3 && low <= 6) {
        switch (chip) {
        case 0x0: info->coprocessor = "DSP"; break;
        case 0x1: info->coprocessor = "SuperFX"; break;
        case 0x2: info->coprocessor = "OBC1"; break;
        case 0x3: info->coprocessor = NULL; info->mapper = SNES_MAP_SA1; break;
        case 0x4: info->coprocessor = "S-DD1"; break;
        case 0x5: info->coprocessor = "S-RTC"; break;
        case 0xE: info->coprocessor = "other"; break;
        case 0xF: info->coprocessor = h[0x0F] == 0x10 ? "Cx4" : "SPC7110/ST01x"; break;
        default: info->coprocessor = "unknown"; break;
        }
    }
    if (info->mapper == SNES_MAP_SA1) {
        /* BW-RAM size is in the same header byte; SA-1 carts may declare it
         * without the RAM bit. */
        info->sram_size = h[0x18] ? 0x400u << h[0x18] : 0;
        if (info->sram_size > SNES_SRAM_MAX) info->sram_size = SNES_SRAM_MAX;
    }
    if (info->sram_size > SNES_SRAM_MAX) info->sram_size = SNES_SRAM_MAX;
    return true;
}

static void set_page(SnesMachine *m, uint32_t bank, uint32_t addr, uint8_t type, uint32_t offset, uint32_t mask)
{
    uint32_t page = (bank << 4) | (addr >> 12);
    m->page_type[page] = type;
    m->page_offset[page] = offset;
    m->page_mask[page] = mask;
}

static void map_rom(SnesMachine *m, uint32_t bank_lo, uint32_t bank_hi, uint32_t addr_lo, uint32_t addr_hi,
                    uint32_t (*linear)(uint32_t bank, uint32_t addr))
{
    for (uint32_t bank = bank_lo; bank <= bank_hi; bank++) {
        for (uint32_t addr = addr_lo; addr <= addr_hi; addr += 0x1000u) {
            set_page(m, bank, addr, SNES_PAGE_ROM, snes_rom_mirror(linear(bank, addr), m->rom_size), 0xFFFFFFFFu);
        }
    }
}

static void map_sram(SnesMachine *m, uint32_t bank_lo, uint32_t bank_hi, uint32_t addr_lo, uint32_t addr_hi,
                     uint32_t (*linear)(uint32_t bank, uint32_t addr))
{
    if (!m->cart.sram_size) return;
    for (uint32_t bank = bank_lo; bank <= bank_hi; bank++) {
        for (uint32_t addr = addr_lo; addr <= addr_hi; addr += 0x1000u) {
            set_page(m, bank, addr, SNES_PAGE_SRAM, linear(bank, addr) & (m->cart.sram_size - 1u),
                     m->cart.sram_size - 1u);
        }
    }
}

static uint32_t lorom_linear(uint32_t bank, uint32_t addr) { return ((bank & 0x7Fu) << 15) | (addr & 0x7FFFu); }
static uint32_t hirom_linear(uint32_t bank, uint32_t addr) { return ((bank & 0x3Fu) << 16) | addr; }
static uint32_t exhirom_linear(uint32_t bank, uint32_t addr)
{
    /* Banks C0-FF (and 80-BF:8000) hold the first 4 MiB; 40-7D (and
     * 00-3F:8000) the second. */
    return ((bank & 0x80u) ? 0u : 0x400000u) | ((bank & 0x3Fu) << 16) | addr;
}
static uint32_t lorom_sram_linear(uint32_t bank, uint32_t addr) { return ((bank & 0x0Fu) << 15) | (addr & 0x7FFFu); }
static uint32_t hirom_sram_linear(uint32_t bank, uint32_t addr) { return ((bank & 0x1Fu) << 13) | (addr - 0x6000u); }

void snes_cart_map(SnesMachine *m)
{
    memset(m->page_type, SNES_PAGE_OPEN, sizeof m->page_type);
    memset(m->page_offset, 0, sizeof m->page_offset);
    memset(m->page_mask, 0, sizeof m->page_mask);
    m->sa1.present = false;

    /* System area of banks 00-3F and 80-BF: WRAM mirror and I/O. */
    for (uint32_t bank = 0; bank < 0x100; bank++) {
        if (bank & 0x40u) continue;
        for (uint32_t addr = 0; addr < 0x6000u; addr += 0x1000u) set_page(m, bank, addr, SNES_PAGE_SYSTEM, 0, 0);
    }
    for (uint32_t bank = 0x7E; bank <= 0x7F; bank++) {
        for (uint32_t addr = 0; addr < 0x10000u; addr += 0x1000u)
            set_page(m, bank, addr, SNES_PAGE_WRAM, ((bank & 1u) << 16) | addr, 0x1FFFFu);
    }

    switch (m->cart.mapper) {
    case SNES_MAP_LOROM:
        map_rom(m, 0x00, 0x7D, 0x8000, 0xFFFF, lorom_linear);
        map_rom(m, 0x80, 0xFF, 0x8000, 0xFFFF, lorom_linear);
        /* Large LoROM boards also decode $0000-$7FFF of banks 40-6F. */
        map_rom(m, 0x40, 0x6F, 0x0000, 0x7FFF, lorom_linear);
        map_rom(m, 0xC0, 0xEF, 0x0000, 0x7FFF, lorom_linear);
        if (m->cart.sram_size) {
            map_sram(m, 0x70, 0x7D, 0x0000, 0x7FFF, lorom_sram_linear);
            map_sram(m, 0xF0, 0xFF, 0x0000, 0x7FFF, lorom_sram_linear);
        } else {
            map_rom(m, 0x70, 0x7D, 0x0000, 0x7FFF, lorom_linear);
            map_rom(m, 0xF0, 0xFF, 0x0000, 0x7FFF, lorom_linear);
        }
        break;
    case SNES_MAP_HIROM:
        map_rom(m, 0x00, 0x3F, 0x8000, 0xFFFF, hirom_linear);
        map_rom(m, 0x80, 0xBF, 0x8000, 0xFFFF, hirom_linear);
        map_rom(m, 0x40, 0x7D, 0x0000, 0xFFFF, hirom_linear);
        map_rom(m, 0xC0, 0xFF, 0x0000, 0xFFFF, hirom_linear);
        map_sram(m, 0x20, 0x3F, 0x6000, 0x7FFF, hirom_sram_linear);
        map_sram(m, 0xA0, 0xBF, 0x6000, 0x7FFF, hirom_sram_linear);
        break;
    case SNES_MAP_EXHIROM:
        map_rom(m, 0x00, 0x3F, 0x8000, 0xFFFF, exhirom_linear);
        map_rom(m, 0x80, 0xBF, 0x8000, 0xFFFF, exhirom_linear);
        map_rom(m, 0x40, 0x7D, 0x0000, 0xFFFF, exhirom_linear);
        map_rom(m, 0xC0, 0xFF, 0x0000, 0xFFFF, exhirom_linear);
        map_sram(m, 0x80, 0xBF, 0x6000, 0x7FFF, hirom_sram_linear);
        break;
    case SNES_MAP_SA1:
        /* ROM through the Super MMC, BW-RAM and I-RAM (snes_sa1.c). */
        snes_sa1_map(m);
        break;
    }
    /* WRAM banks always win. */
    for (uint32_t bank = 0x7E; bank <= 0x7F; bank++) {
        for (uint32_t addr = 0; addr < 0x10000u; addr += 0x1000u)
            set_page(m, bank, addr, SNES_PAGE_WRAM, ((bank & 1u) << 16) | addr, 0x1FFFFu);
    }
}

uint8_t snes_cart_read(SnesMachine *m, uint32_t addr, uint8_t mdr)
{
    uint32_t page = addr >> 12;
    switch (m->page_type[page]) {
    case SNES_PAGE_ROM: return m->rom[m->page_offset[page] + (addr & 0xFFFu)];
    case SNES_PAGE_SRAM: return m->sram[(m->page_offset[page] + (addr & 0xFFFu)) & m->page_mask[page]];
    case SNES_PAGE_COPROC: return m->sa1.present ? snes_sa1_cpu_read(m, addr, mdr) : mdr;
    default: return mdr;
    }
}

void snes_cart_write(SnesMachine *m, uint32_t addr, uint8_t v)
{
    uint32_t page = addr >> 12;
    if (m->page_type[page] == SNES_PAGE_COPROC) {
        if (m->sa1.present) snes_sa1_cpu_write(m, addr, v);
    } else if (m->page_type[page] == SNES_PAGE_SRAM) {
        uint32_t off = (m->page_offset[page] + (addr & 0xFFFu)) & m->page_mask[page];
        if (m->sram[off] != v) {
            m->sram[off] = v;
            m->sram_dirty = true;
        }
    }
}
