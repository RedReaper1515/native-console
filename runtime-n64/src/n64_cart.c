/*
 * The cartridge: the header, the CIC (from the boot code's CRC), the save
 * hardware (n64_cartdb.c), and the cartridge bus the PI and the CPU reach:
 * the ROM in domain 1, SRAM or FlashRAM in domain 2.
 *
 * The CIC table is cen64's (si/cic.c, BSD-3-Clause, Copyright (c) 2015,
 * Tyler J. Stachecki): the CRC-32 of bytes 0x40-0xFFF of each CIC's boot
 * code. The FlashRAM model follows ares (n64/cartridge/flash.cpp, ISC):
 * the command register at 0x08010000 takes a word as two halves, and
 * status, silicon ID, page-buffer and array reads are modes. Ported from
 * ares (https://github.com/ares-emulator/ares). Copyright (c) 2004-2025
 * ares team, Near et al. Permission to use, copy, modify, and/or
 * distribute this software for any purpose with or without fee is hereby
 * granted, provided that the above copyright notice and this permission
 * notice appear in all copies.
 */
#include "n64_io.h"

#include <string.h>

static uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t n64_cic_from_ipl3(const uint8_t *rom, uint32_t *seed)
{
    const uint32_t crc = crc32(rom + 0x40, 0x1000 - 0x40);
    const uint32_t aleck = crc32(rom + 0x40, 0xC00 - 0x40);
    if (aleck == 0x587BD543u) {
        *seed = 0xAC;
        return 5101;
    }
    switch (crc) {
    case 0x6170A4A1u: *seed = 0x3F; return 6101;
    case 0x009E9EA3u: *seed = 0x3F; return 7102;
    case 0x90BB6CB5u: *seed = 0x3F; return 6102;
    case 0x0B050EE0u: *seed = 0x78; return 6103;
    case 0x98BC2C86u: *seed = 0x91; return 6105;
    case 0xACC8580Au: *seed = 0x85; return 6106;
    case 0x0E018159u: *seed = 0xDD; return 8303;
    default: *seed = 0; return 0; /* unknown boot code (homebrew) */
    }
}

bool n64_cart_parse(N64Cart *cart, const uint8_t *rom, size_t size)
{
    if (size < 0x1000u) return false;
    memset(cart, 0, sizeof *cart);
    for (int i = 0; i < 20; i++) {
        char ch = (char)rom[0x20 + i];
        cart->title[i] = (ch >= 0x20 && ch < 0x7F) ? ch : ' ';
    }
    for (int i = 19; i >= 0 && cart->title[i] == ' '; i--) cart->title[i] = '\0';
    for (int i = 0; i < 4; i++) {
        char ch = (char)rom[0x3B + i];
        cart->id[i] = (ch >= 0x20 && ch < 0x7F) ? ch : '?';
    }
    cart->revision = rom[0x3F];
    cart->entry = n64_be32(rom + 8);
    cart->cic = n64_cic_from_ipl3(rom, &cart->cic_seed);
    cart->ipl3_known = cart->cic != 0;
    if (!cart->ipl3_known) {
        /* Unknown boot code (homebrew): behave like 6102. */
        cart->cic = 6102;
        cart->cic_seed = 0x3F;
    }
    switch (cart->id[3]) {
    case 'D': case 'F': case 'H': case 'I': case 'L': case 'P': case 'S': case 'U': case 'W': case 'X': case 'Y':
        cart->pal = true;
        break;
    default: cart->pal = false; break;
    }
    if (cart->cic == 7102) cart->pal = true;
    n64_cartdb_lookup(cart);
    return true;
}

/* ---- FlashRAM ---- */

#define FLASH_MS 93750u /* CPU cycles per millisecond */

static bool flash_word_indexed(const N64Machine *m) { return m->flash.model == 0; }

void n64_flash_reset(N64Machine *m)
{
    N64Flash *f = &m->flash;
    uint8_t model = m->cart.flash_model;
    memset(f, 0, sizeof *f);
    f->model = model;
    f->status = 0x80u | 0x0Cu; /* ready; Macronix reports "ok" in bits 2-3 */
    memset(f->page, 0xFF, sizeof f->page);
    m->events[N64_EV_FLASH] = UINT64_MAX;
}

void n64_flash_done(N64Machine *m)
{
    m->flash.status = (uint8_t)((m->flash.status | 0x80u) & ~3u);
    m->flash.busy = 0;
}

static void flash_command(N64Machine *m, uint32_t data)
{
    N64Flash *f = &m->flash;
    const uint8_t cmd = (uint8_t)(data >> 24);
    f->open_bus = false;
    if (f->busy) return;
    /* Macronix parts enter status mode on the second D2 in a row. */
    if (cmd == 0xD2u) {
        if (f->pending_cmd != cmd) {
            f->pending_cmd = cmd;
            f->pending_count = 1;
            return;
        }
        if (++f->pending_count < 2) return;
    }
    f->pending_cmd = 0;
    f->pending_count = 0;
    switch (cmd) {
    case 0x3C: f->erase_setup = 1; break;
    case 0x4B:
        f->erase_setup = 2;
        f->erase_sector = (uint8_t)((data & 0x3FFu) >> 7);
        break;
    case 0x78: {
        if (!f->erase_setup) break;
        if (f->erase_setup == 1) memset(m->sram, 0xFF, 0x20000u);
        else memset(m->sram + ((uint32_t)f->erase_sector << 14), 0xFF, 0x4000u);
        f->erase_setup = 0;
        f->mode = 1;
        f->status = (uint8_t)((f->status & ~0x80u) | 0x02u);
        f->status_stale = true;
        f->busy = 1;
        m->save_dirty = true;
        n64_schedule(m, N64_EV_FLASH, m->cpu.cycles + 85u * FLASH_MS);
        break;
    }
    case 0xA5: {
        uint32_t base = (data & 0x3FFu) << 7;
        for (uint32_t i = 0; i < 128u; i++) m->sram[base + i] &= f->page[i];
        memset(f->page, 0xFF, sizeof f->page);
        f->mode = 1;
        f->status = (uint8_t)((f->status & ~0x80u) | 0x01u);
        f->status_stale = true;
        f->busy = 2;
        m->save_dirty = true;
        n64_schedule(m, N64_EV_FLASH, m->cpu.cycles + 3500u * FLASH_MS / 1000u);
        break;
    }
    case 0xB4: f->mode = 3; break;
    case 0xD2:
        f->mode = 1;
        f->status_stale = true;
        break;
    case 0xE1: f->mode = 2; break;
    case 0xF0: f->mode = 0; break;
    default: break;
    }
}

static uint16_t flash_read_half(N64Machine *m)
{
    N64Flash *f = &m->flash;
    uint16_t v;
    if (f->open_bus) return (uint16_t)f->offset;
    switch (f->mode) {
    case 1:
        if (f->offset & 0x20000u) {
            f->open_bus = true;
            return (uint16_t)f->offset;
        }
        v = f->status_stale ? f->stale_value : f->status;
        f->status_stale = false;
        f->stale_value = v;
        f->offset += 2;
        f->burst++;
        return v;
    case 2: {
        const uint16_t id[4] = { 0x1111u, 0x8001u, 0x00C2u, f->model == 0 ? 0x001Eu : 0x001Du };
        v = id[f->burst & 3u];
        f->stale_value = v;
        f->offset += 2;
        f->burst++;
        return v;
    }
    case 3: {
        uint32_t i = (f->offset & 0x7Fu) & ~1u;
        v = (uint16_t)(f->page[i] << 8 | f->page[i + 1]);
        f->stale_value = v;
        f->offset += 2;
        return v;
    }
    default: {
        uint32_t mask = flash_word_indexed(m) ? 0x3FFFu : 0x7FFFu;
        uint32_t at = flash_word_indexed(m) ? f->offset << 1 : f->offset;
        v = at + 1u < 0x20000u ? (uint16_t)(m->sram[at] << 8 | m->sram[at + 1]) : 0;
        f->stale_value = v;
        uint32_t step = flash_word_indexed(m) ? 1u : 2u;
        f->offset = (f->offset & ~mask) | ((f->offset + step) & mask);
        return v;
    }
    }
}

static void flash_write_half(N64Machine *m, uint16_t v)
{
    N64Flash *f = &m->flash;
    uint32_t at = f->offset & ~1u;
    if (at == 0x10000u || at == 0x10002u) {
        f->offset += 2;
        if (!f->cir_valid) {
            f->cir_high = v;
            f->cir_valid = true;
            return;
        }
        f->cir_valid = false;
        flash_command(m, (uint32_t)f->cir_high << 16 | v);
        return;
    }
    if (f->mode == 1) {
        if (at == 0 && v == 0) f->status |= 0x0Cu;
    } else if (f->mode == 3) {
        uint32_t i = at & 0x7Fu;
        f->page[i] = (uint8_t)(v >> 8);
        f->page[i + 1] = (uint8_t)v;
    }
    f->offset += 2;
}

/* ---- the cartridge bus ---- */

static bool in_sram(const N64Machine *m, uint32_t a)
{
    return a >= 0x08000000u && a < 0x10000000u && m->cart.sram_size && !m->cart.flash;
}

void n64_cart_select(N64Machine *m, uint32_t paddr)
{
    m->cart_addr = paddr;
    if (m->cart.flash && paddr >= 0x08000000u && paddr < 0x10000000u) {
        m->flash.offset = paddr - 0x08000000u;
        m->flash.cir_valid = false;
        m->flash.burst = 0;
    }
}

uint16_t n64_cart_read_half(N64Machine *m)
{
    const uint32_t a = m->cart_addr;
    uint16_t v;
    if (a >= 0x10000000u && a < 0x1FC00000u) {
        uint32_t o = a - 0x10000000u;
        v = o + 1u < m->rom_size ? n64_be16(m->rom + o) : (uint16_t)a; /* open bus: the address */
    } else if (m->cart.flash && a >= 0x08000000u && a < 0x10000000u) {
        v = flash_read_half(m);
    } else if (in_sram(m, a)) {
        /* 32 KiB banks every 256 KiB of address space (96 KiB SRAM: 3 banks). */
        uint32_t o = ((a >> 18) & 3u) * 0x8000u + (a & 0x7FFFu);
        v = o + 1u < m->cart.sram_size ? n64_be16(m->sram + o) : 0;
    } else {
        v = (uint16_t)a;
    }
    m->cart_addr = a + 2u;
    return v;
}

void n64_cart_write_half(N64Machine *m, uint16_t v)
{
    const uint32_t a = m->cart_addr;
    if (m->cart.flash && a >= 0x08000000u && a < 0x10000000u) {
        flash_write_half(m, v);
    } else if (in_sram(m, a)) {
        uint32_t o = ((a >> 18) & 3u) * 0x8000u + (a & 0x7FFFu);
        if (o + 1u < m->cart.sram_size) {
            n64_put_be16(m->sram + o, v);
            m->save_dirty = true;
        }
    }
    m->cart_addr = a + 2u;
}
