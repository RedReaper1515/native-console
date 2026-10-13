/*
 * The physical memory map, as the CPU core reaches it through the
 * n64_bus_* functions (n64_bus.h).
 *
 *   00000000-03EFFFFF  RDRAM (4 or 8 MiB present, the rest reads zero)
 *   03F00000-03FFFFFF  RDRAM registers
 *   04000000-0403FFFF  SP DMEM and IMEM (mirrored every 8 KiB)
 *   04040000-040FFFFF  SP registers, SP_PC
 *   04100000-041FFFFF  DP command registers
 *   04300000-048FFFFF  MI, VI, AI, PI, RI, SI
 *   05000000-1FBFFFFF  the cartridge bus (PI domains 1 and 2), including
 *                      the ISViewer debug port at 13FF0000
 *   1FC00000-1FC007BF  PIF ROM (not present: reads zero, see N64-PLAN.md)
 *   1FC007C0-1FC007FF  PIF RAM
 *
 * Registers are 32 bits wide; narrower and wider accesses to them are made
 * of word accesses.
 *
 * Until IPL3 has mapped every RDRAM chip where it belongs, RDRAM accesses
 * go through the chips' device IDs (n64_rdram.c). MI_MODE's repeat mode
 * (one CPU store written over up to 128 bytes, the way IPL3 programs the
 * RDRAM chips) follows ares's MI::writeRdram (n64/mi/bus.hpp; ISC).
 */
#include <stdlib.h>
#include <string.h>

#include "n64_bus.h"
#include "n64_io.h"
#include "n64_coverage.h"

#define ISV_BASE 0x13FF0000u
#define ISV_END 0x14000000u

static void isv_emit(N64Machine *m, uint32_t len)
{
    if (len > sizeof m->isv_buffer - 0x20u) len = sizeof m->isv_buffer - 0x20u;
    if (m->isv_len + len + 1u > m->isv_cap) {
        size_t cap = m->isv_cap ? m->isv_cap : 4096u;
        while (cap < m->isv_len + len + 1u) cap *= 2u;
        char *p = realloc(m->isv_log, cap);
        if (!p) return;
        m->isv_log = p;
        m->isv_cap = cap;
    }
    memcpy(m->isv_log + m->isv_len, m->isv_buffer + 0x20, len);
    m->isv_len += len;
    m->isv_log[m->isv_len] = '\0';
}

/* Where an RDRAM address lives in m->rdram, or false (and RI_ERROR) when
 * no chip answers. */
static bool ram_at(N64Machine *m, uint32_t pa, uint32_t size, uint32_t *out, unsigned *chip)
{
    *chip = 4;
    if (m->rdram_identity) {
        *out = pa;
        return pa + size <= m->rdram_size;
    }
    if (!n64_rdram_translate(m, pa, out, chip)) {
        m->ri_regs[6] |= 1u;
        return false;
    }
    return *out + size <= m->rdram_size;
}

static uint32_t ram_read_word(N64Machine *m, uint32_t pa)
{
    uint32_t at;
    unsigned chip;
    if (!ram_at(m, pa, 4, &at, &chip)) return 0;
    const uint32_t v = n64_be32(m->rdram + at);
    return chip < 4 ? n64_rdram_degrade(m, chip, v) : v;
}

static void ram_write(N64Machine *m, uint32_t pa, uint64_t v, uint32_t size)
{
    uint32_t at;
    unsigned chip;
    if (!ram_at(m, pa, size, &at, &chip)) return;
    for (uint32_t i = 0; i < size; i++) m->rdram[at + i] = (uint8_t)(v >> (8u * (size - 1u - i)));
    n64_ram_written(m, at, size);
}

static uint32_t read_word(N64Machine *m, uint32_t pa)
{
    if (pa < 0x03F00000u) return ram_read_word(m, pa);
    if (pa < 0x04000000u) {
        /* Odd words read zero unless MI_MODE selects the RDRAM registers. */
        if (!(m->mi_mode & 0x200u) && (pa & 4u)) return 0;
        return n64_rdram_reg_read(m, pa);
    }
    if (pa < 0x04300000u) return n64_sp_read(m, pa < 0x04040000u ? 0x04000000u | (pa & 0x1FFFu) : pa);
    if (pa < 0x04400000u) return n64_mi_read(m, pa & 0xFFFFFu);
    if (pa < 0x04500000u) return n64_vi_read(m, pa & 0xFFFFFu);
    if (pa < 0x04600000u) return n64_ai_read(m, pa & 0xFFFFFu);
    if (pa < 0x04700000u) return n64_pi_read(m, pa & 0xFFFFFu);
    if (pa < 0x04800000u) return n64_ri_read(m, pa & 0xFFFFFu);
    if (pa < 0x04900000u) return n64_si_read(m, pa & 0xFFFFFu);
    if (pa >= ISV_BASE && pa < ISV_END) return n64_be32(m->isv_buffer + (pa & 0xFFFCu));
    if (pa >= 0x05000000u && pa < 0x1FC00000u) {
        n64_cart_select(m, pa & ~3u);
        uint32_t hi = n64_cart_read_half(m);
        uint32_t lo = n64_cart_read_half(m);
        return hi << 16 | lo;
    }
    if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) return n64_be32(m->pif_ram + (pa & 0x3Cu));
    return 0;
}

static void write_word(N64Machine *m, uint32_t pa, uint32_t v)
{
    if (pa < 0x03F00000u) {
        ram_write(m, pa, v, 4);
        return;
    }
    if (pa < 0x04000000u) {
        n64_rdram_reg_write(m, pa, v, 0);
        return;
    }
    if (pa < 0x04300000u) {
        n64_sp_write(m, pa < 0x04040000u ? 0x04000000u | (pa & 0x1FFFu) : pa, v);
        return;
    }
    if (pa < 0x04400000u) n64_mi_write(m, pa & 0xFFFFFu, v);
    else if (pa < 0x04500000u) n64_vi_write(m, pa & 0xFFFFFu, v);
    else if (pa < 0x04600000u) n64_ai_write(m, pa & 0xFFFFFu, v);
    else if (pa < 0x04700000u) n64_pi_write(m, pa & 0xFFFFFu, v);
    else if (pa < 0x04800000u) n64_ri_write(m, pa & 0xFFFFFu, v);
    else if (pa < 0x04900000u) n64_si_write(m, pa & 0xFFFFFu, v);
    else if (pa >= ISV_BASE && pa < ISV_END) {
        n64_put_be32(m->isv_buffer + (pa & 0xFFFCu), v);
        if ((pa & 0xFFFCu) == 0x14u) isv_emit(m, v);
    } else if (pa >= 0x05000000u && pa < 0x1FC00000u) {
        m->pi_latch = v;
        n64_cart_select(m, pa & ~3u);
        n64_cart_write_half(m, (uint16_t)(v >> 16));
        n64_cart_write_half(m, (uint16_t)v);
    } else if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) {
        n64_put_be32(m->pif_ram + (pa & 0x3Cu), v);
        n64_pif_cpu_written(m);
    }
}

/* MI_MODE repeat mode: the next CPU store is repeated over
 * (MI_MODE & 0x7F) + 1 bytes, wrapping within 2 KiB rows; to an RDRAM
 * register it carries that length. Returns true when it took the store. */
static bool repeat_write(N64Machine *m, uint32_t pa, uint64_t value, uint32_t size)
{
    if (!(m->mi_mode & 0x80u) || pa >= 0x04000000u) return false; /* only RDRAM and its registers repeat */
    m->mi_mode &= ~0x80u;
    n64_rdram_update_fast(m);
    uint32_t length = (m->mi_mode & 0x7Fu) + 1u;
    if (pa >= 0x03F00000u) {
        uint32_t word = (uint32_t)value;
        if (size == 1) word = (uint32_t)value << (24u - 8u * (pa & 3u));
        if (size == 2) word = (uint32_t)value << (16u - 8u * (pa & 2u));
        if (size == 8) word = (uint32_t)(value >> 32);
        n64_rdram_reg_write(m, pa & ~3u, word, length);
        return true;
    }
    if (size == 1) {
        value &= 0xFFFFFFFFu >> (24u - (pa & 3u) * 8u);
        value = (uint32_t)(value << 24 | value >> 8);
    } else if (size == 2) {
        value &= 0xFFFFFFFFu >> (16u - (pa & 2u) * 8u);
        value = (uint32_t)(value << 16 | value >> 16);
    }
    if (size != 8) value = value << 32 | (uint32_t)value;
    uint32_t end = (pa & ~7u) + length;
    if (end > m->rdram_size) end = m->rdram_size;
    if (end <= pa) return true;
    length = end - pa;
#define NEXT(n) pa = (pa & ~0x7FFu) | ((pa + (n)) & 0x7FFu)
    if (pa & 1u) {
        ram_write(m, pa, value >> 56, 1);
        value = value << 8 | value >> 56;
        NEXT(1);
        length -= 1;
    }
    if ((pa & 2u) && length >= 2) {
        ram_write(m, pa, value >> 48, 2);
        value = value << 16 | value >> 48;
        NEXT(2);
        length -= 2;
    }
    if ((pa & 4u) && length >= 4) {
        ram_write(m, pa, value >> 32, 4);
        value = value << 32 | value >> 32;
        NEXT(4);
        length -= 4;
    }
    while (length >= 8) {
        ram_write(m, pa, value, 8);
        NEXT(8);
        length -= 8;
    }
#undef NEXT
    if (length >= 4) {
        ram_write(m, pa, value >> 32, 4);
        value <<= 32;
        pa += 4;
        length -= 4;
    }
    if (length >= 2) {
        ram_write(m, pa, value >> 48, 2);
        value <<= 16;
        pa += 2;
        length -= 2;
    }
    if (length == 1) ram_write(m, pa, value >> 56, 1);
    return true;
}

void n64_ram_written(N64Machine *m, uint32_t paddr, uint32_t size)
{
    if (m->rc_code_map) n64_bus_code_written(&m->cpu, paddr, size);
    if (m->coverage) n64_coverage_written(m, paddr, size);
}

uint32_t n64_bus_fetch(N64Cpu *c, uint32_t pa) { return read_word(N64_MACHINE(c), pa & ~3u); }

uint8_t n64_bus_read8(N64Cpu *c, uint32_t pa)
{
    uint32_t w = read_word(N64_MACHINE(c), pa & ~3u);
    return (uint8_t)(w >> (8u * (3u - (pa & 3u))));
}

uint16_t n64_bus_read16(N64Cpu *c, uint32_t pa)
{
    uint32_t w = read_word(N64_MACHINE(c), pa & ~3u);
    return (uint16_t)(w >> (8u * (2u - (pa & 2u))));
}

uint32_t n64_bus_read32(N64Cpu *c, uint32_t pa) { return read_word(N64_MACHINE(c), pa & ~3u); }

uint64_t n64_bus_read64(N64Cpu *c, uint32_t pa)
{
    N64Machine *m = N64_MACHINE(c);
    uint64_t hi = read_word(m, pa & ~7u);
    return hi << 32 | read_word(m, (pa & ~7u) + 4u);
}

/* Narrow stores to RDRAM touch only their bytes; to registers and the
 * cartridge bus they are word writes of the value shifted into place, as
 * the RCP sees the CPU's SysAD write. */
void n64_bus_write8(N64Cpu *c, uint32_t pa, uint8_t v)
{
    N64Machine *m = N64_MACHINE(c);
    if (repeat_write(m, pa, v, 1)) return;
    if (pa < 0x03F00000u) {
        ram_write(m, pa, v, 1);
        return;
    }
    if (pa < 0x04040000u && pa >= 0x04000000u) {
        n64_sync_rsp(m);
        ((pa & 0x1000u) ? m->rsp.imem : m->rsp.dmem)[pa & 0xFFFu] = v;
        return;
    }
    write_word(m, pa & ~3u, (uint32_t)v << (8u * (3u - (pa & 3u))));
}

void n64_bus_write16(N64Cpu *c, uint32_t pa, uint16_t v)
{
    N64Machine *m = N64_MACHINE(c);
    if (repeat_write(m, pa, v, 2)) return;
    if (pa < 0x03F00000u) {
        ram_write(m, pa, v, 2);
        return;
    }
    if (pa < 0x04040000u && pa >= 0x04000000u) {
        n64_sync_rsp(m);
        n64_put_be16(((pa & 0x1000u) ? m->rsp.imem : m->rsp.dmem) + (pa & 0xFFEu), v);
        return;
    }
    write_word(m, pa & ~3u, (uint32_t)v << (8u * (2u - (pa & 2u))));
}

void n64_bus_write32(N64Cpu *c, uint32_t pa, uint32_t v)
{
    N64Machine *m = N64_MACHINE(c);
    if (repeat_write(m, pa, v, 4)) return;
    write_word(m, pa & ~3u, v);
}

void n64_bus_write64(N64Cpu *c, uint32_t pa, uint64_t v)
{
    N64Machine *m = N64_MACHINE(c);
    if (repeat_write(m, pa, v, 8)) return;
    write_word(m, pa & ~7u, (uint32_t)(v >> 32));
    write_word(m, (pa & ~7u) + 4u, (uint32_t)v);
}

uint8_t n64_peek(const N64Machine *m, uint32_t pa)
{
    if (pa < m->rdram_size) return m->rdram[pa];
    if (pa >= 0x04000000u && pa < 0x04040000u) return ((pa & 0x1000u) ? m->rsp.imem : m->rsp.dmem)[pa & 0xFFFu];
    if (pa >= 0x10000000u && pa < 0x1FC00000u && pa - 0x10000000u < m->rom_size) return m->rom[pa - 0x10000000u];
    if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) return m->pif_ram[pa & 0x3Fu];
    return 0;
}
