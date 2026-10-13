/*
 * The RDRAM interface (RI) and the RDRAM chips' own registers
 * (03F00000-03FFFFFF), which the cartridge's IPL3 programs at boot: it
 * gives each chip a device ID, which decides where in the address space
 * the chip answers, and calibrates each chip's output current. Until every
 * chip answers where it is and has enough current, RDRAM accesses go
 * through the device IDs (n64_rdram_translate), and reads from a chip
 * with too little current lose bits. Once IPL3 is done the mapping is the
 * identity and RDRAM is plain memory again.
 *
 * Register layout, device ID and current-control encodings, the write
 * delay rule and the mapping follow ares (n64/rdram/rdram.cpp,
 * n64/rdram/io.cpp, n64/ri/io.cpp; ISC). Ported from ares
 * (https://github.com/ares-emulator/ares). Copyright (c) 2004-2025 ares
 * team, Near et al. Permission to use, copy, modify, and/or distribute
 * this software for any purpose with or without fee is hereby granted,
 * provided that the above copyright notice and this permission notice
 * appear in all copies. THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR
 * DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE.
 *
 * Differences: the chips' current thresholds and the bit loss of an
 * under-powered chip come from a fixed-seed generator rather than the
 * host's, so runs stay deterministic.
 */
#include "n64_io.h"
#include "n64_coverage.h"

#include <string.h>

#define CHIP_BYTES 0x200000u

static uint32_t rng(N64Machine *m)
{
    uint32_t x = m->rdram_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rdram_rng = x;
    return x;
}

static bool ri_active(const N64Machine *m) { return m->ri_current_loaded && m->ri_regs[3] == 0x14u; }

/* The CPU's RDRAM shortcut is only right while RDRAM is plain memory and no
 * repeated write is armed (MI_MODE bit 7); coverage turns it off too. */
void n64_rdram_update_fast(N64Machine *m)
{
    /* Coverage sees RDRAM writes only through the bus (a game's window
     * records without them). */
    const bool plain = m->rdram_identity && !(m->mi_mode & 0x80u) && !n64_coverage_sees_stores(m);
    m->cpu.links.rdram_size = plain ? m->rdram_size : 0;
}

static uint16_t decode_device_id(uint32_t v)
{
    return (uint16_t)((v >> 26 & 0x3Fu) | (v >> 23 & 1u) << 6 | (v >> 8 & 0xFFu) << 7 | (v >> 7 & 1u) << 15);
}

static const unsigned cci_bits[6] = { 6, 14, 22, 7, 15, 23 };

static uint8_t decode_cci(uint32_t mode)
{
    uint8_t cci = 0;
    for (unsigned i = 0; i < 6; i++) cci |= (uint8_t)((mode >> cci_bits[i] & 1u) << i);
    return cci ^ 0x3Fu;
}

static uint32_t encode_cci(uint8_t cci)
{
    cci ^= 0x3Fu;
    uint32_t v = 0;
    for (unsigned i = 0; i < 6; i++) v |= (uint32_t)(cci >> i & 1u) << cci_bits[i];
    return v;
}

static void update_mapping(N64Machine *m)
{
    bool identity = true;
    for (unsigned n = 0; n < 4; n++) {
        const N64RdramChip *chip = &m->rdram_chips[n];
        if (!chip->present) continue;
        if (!chip->enable || chip->device_id != n * 2u || chip->cci < chip->cc_high) identity = false;
    }
    m->rdram_identity = identity && ri_active(m);
    n64_rdram_update_fast(m);
}

void n64_rdram_power(N64Machine *m)
{
    memset(m->ri_regs, 0, sizeof m->ri_regs);
    m->ri_current_loaded = false;
    m->rdram_rng = 0x2545F491u;
    const unsigned count = m->rdram_size / CHIP_BYTES;
    for (unsigned n = 0; n < 4; n++) {
        N64RdramChip *chip = &m->rdram_chips[n];
        memset(chip, 0, sizeof *chip);
        chip->present = n < count;
        if (!chip->present) continue;
        chip->device_type = 0xB4190010u;
        chip->device_manufacturer = 0x00000500u;
        chip->delay = 0x00000023u; /* write delay 4, write bits 3 */
        chip->write_delay = 4;
        chip->cc_low = (uint8_t)(8u + (rng(m) & 3u));
        chip->cc_high = (uint8_t)(14u + (rng(m) & 3u));
    }
    m->rdram_identity = false;
    n64_rdram_update_fast(m);
}

/* The state IPL3 leaves: the RI running, each chip enabled at its own
 * address with full current. Used when IPL3 does not run (n64_boot.c). */
void n64_rdram_configure(N64Machine *m)
{
    m->ri_regs[0] = 0x0Eu;       /* RI_MODE */
    m->ri_regs[1] = 0x40u;       /* RI_CONFIG */
    m->ri_regs[3] = 0x14u;       /* RI_SELECT */
    m->ri_regs[4] = 0x00063634u; /* RI_REFRESH */
    m->ri_current_loaded = true;
    for (unsigned n = 0; n < 4; n++) {
        N64RdramChip *chip = &m->rdram_chips[n];
        if (!chip->present) continue;
        chip->enable = true;
        chip->device_id = (uint16_t)(n * 2u);
        chip->cci = chip->cc_high;
        chip->mode = encode_cci(chip->cci) | 1u << 25;
    }
    update_mapping(m);
}

bool n64_rdram_translate(N64Machine *m, uint32_t pa, uint32_t *out, unsigned *chip_index)
{
    if (!ri_active(m)) return false;
    for (unsigned n = 0; n < 4; n++) {
        const N64RdramChip *chip = &m->rdram_chips[n];
        if (!chip->present || !chip->enable) continue;
        if ((uint32_t)(chip->device_id >> 1) != pa >> 21) continue;
        *out = n * CHIP_BYTES + (pa & 0x1FFFFFu);
        if (chip_index) *chip_index = n;
        return true;
    }
    return false;
}

/* A read from a chip with too little output current loses bits. */
uint32_t n64_rdram_degrade(N64Machine *m, unsigned chip_index, uint32_t value)
{
    const N64RdramChip *chip = &m->rdram_chips[chip_index];
    if (chip->cci >= chip->cc_high) return value;
    if (chip->cci <= chip->cc_low) return 0;
    const uint32_t progress = (uint32_t)(chip->cci - chip->cc_low) * 256u / (uint32_t)(chip->cc_high - chip->cc_low);
    uint32_t result = 0;
    for (unsigned bit = 0; bit < 32; bit++) {
        if (!(value >> bit & 1u)) continue;
        if ((rng(m) & 255u) < progress) result |= 1u << bit;
    }
    return result;
}

static N64RdramChip *select_chip(N64Machine *m, uint32_t select)
{
    if (!ri_active(m)) return NULL;
    bool seen_disabled = false;
    for (unsigned n = 0; n < 4; n++) {
        N64RdramChip *chip = &m->rdram_chips[n];
        if (!chip->present) continue;
        const bool match = (chip->device_id & ~1u) == (select & ~1u);
        if (chip->enable) {
            if (match) return chip;
        } else if (!seen_disabled) {
            seen_disabled = true;
            if (match) return chip;
        }
    }
    return NULL;
}

static uint32_t read_register(const N64RdramChip *chip, unsigned index)
{
    if (!chip->enable || index >= 10) return 0;
    switch (index) {
    case 0: return chip->device_type;
    case 1: return chip->device_id_reg;
    case 2: return chip->delay | 3u << 24 | 3u << 16 | 2u << 8 | 3u;
    case 3: {
        uint32_t v = chip->mode & ~0x00C0C0C0u;
        v |= encode_cci(chip->auto_current ? chip->cc_internal : decode_cci(chip->mode));
        return v ^ 0xC0C0C0C0u;
    }
    case 4: return chip->refresh_interval;
    case 5: return chip->refresh_row;
    case 6: return chip->ras_interval;
    case 7: return chip->min_interval;
    case 8: return chip->address_select;
    default: return chip->device_manufacturer;
    }
}

static void write_register(N64Machine *m, N64RdramChip *chip, unsigned index, uint32_t v, unsigned repeat)
{
    if (index >= 10) return;
    /* Until the write delay is programmed to 1, a register only takes a
     * write repeated over 16 bytes or more (MI_MODE repeat mode), which
     * arrives with its halves swapped. */
    if (chip->write_delay != 1) {
        if (repeat < 16) return;
        v = v << 16 | v >> 16;
    }
    switch (index) {
    case 0: return;
    case 1:
        chip->device_id_reg = v;
        chip->device_id = decode_device_id(v);
        break;
    case 2:
        chip->delay = v & 0x38381838u;
        chip->write_delay = (uint8_t)(chip->delay >> 3 & 7u);
        break;
    case 3:
        chip->mode = v;
        chip->enable = (v >> 25 & 1u) != 0;
        chip->auto_current = (v >> 31) != 0;
        chip->cci = decode_cci(v);
        if (chip->auto_current) chip->cc_internal = chip->cci;
        break;
    case 4: chip->refresh_interval = v; break;
    case 5: chip->refresh_row = v; break;
    case 6: chip->ras_interval = v; break;
    case 7: chip->min_interval = v; break;
    case 8: chip->address_select = v; break;
    default: return;
    }
    update_mapping(m);
}

/* pa is the physical address in 03F00000-03FFFFFF: bit 19 broadcasts,
 * bits 10-18 select a chip by device ID, offsets 0x200 and up are the
 * row register. */
uint32_t n64_rdram_reg_read(N64Machine *m, uint32_t pa)
{
    const bool broadcast = (pa >> 19 & 1u) != 0;
    const uint32_t select = pa >> 10 & 0x1FFu, offset = pa & 0x3FFu;
    if (broadcast || !ri_active(m)) return 0;
    N64RdramChip *chip = select_chip(m, select);
    if (!chip) return 0;
    if (offset >= 0x200u) return chip->row;
    return read_register(chip, (offset >> 2) & 15u);
}

void n64_rdram_reg_write(N64Machine *m, uint32_t pa, uint32_t v, unsigned repeat)
{
    const bool broadcast = (pa >> 19 & 1u) != 0;
    const uint32_t select = pa >> 10 & 0x1FFu, offset = pa & 0x3FFu;
    if (!ri_active(m)) return;
    if (offset >= 0x200u) {
        for (unsigned n = 0; n < 4; n++) {
            N64RdramChip *chip = &m->rdram_chips[n];
            if (chip->present && (broadcast || chip == select_chip(m, select))) chip->row = v;
        }
        return;
    }
    const unsigned index = (offset >> 2) & 15u;
    if (broadcast) {
        for (unsigned n = 0; n < 4; n++) {
            if (m->rdram_chips[n].present) write_register(m, &m->rdram_chips[n], index, v, repeat);
        }
        return;
    }
    N64RdramChip *chip = select_chip(m, select);
    if (chip) write_register(m, chip, index, v, repeat);
}

/* ---- RI ---- */

uint32_t n64_ri_read(N64Machine *m, uint32_t offset)
{
    const unsigned r = (offset >> 2) & 7u;
    if (r == 2) {
        /* RI_CURRENT_LOAD is write-only; a read returns mixed bits. */
        return (m->ri_regs[6] & 1u) | 1u << 1 | 1u << 2 | (m->ri_regs[0] & 8u) | (m->ri_regs[3] & 0x10u);
    }
    return m->ri_regs[r];
}

void n64_ri_write(N64Machine *m, uint32_t offset, uint32_t v)
{
    const unsigned r = (offset >> 2) & 7u;
    switch (r) {
    case 2:
        m->ri_regs[2] = v;
        m->ri_current_loaded = true;
        break;
    case 6: m->ri_regs[6] = 0; break;         /* RI_ERROR: cleared by any write */
    case 7: m->ri_regs[7] = 0x00FFu; break;   /* RI_BANK_STATUS */
    default: m->ri_regs[r] = v; break;
    }
    update_mapping(m);
}
