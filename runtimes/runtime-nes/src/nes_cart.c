/*
 * Cartridges: the iNES / NES 2.0 header, and the mapper boards.
 *
 * Every board is the same three things: registers written through the CPU
 * bus, a rebuild of the page tables from those registers (update_*), and,
 * for a few, something that watches the PPU's address lines (the MMC3's
 * scanline counter, the MMC2's CHR latches). The page tables are what the
 * rest of the runtime reads; nothing else knows which board is fitted.
 *
 * Boards, by iNES mapper number:
 *   0   NROM            no registers
 *   1   MMC1 (SxROM)    serial register, 16/32 KiB PRG, 4/8 KiB CHR
 *   2   UxROM           16 KiB PRG at $8000
 *   3   CNROM           8 KiB CHR
 *   4   MMC3 (TxROM)    8 KiB PRG, 1/2 KiB CHR, scanline IRQ
 *   7   AxROM           32 KiB PRG, single-screen mirroring select
 *   9   MMC2 (PxROM)    8 KiB PRG, CHR switched by the tiles fetched
 *   10  MMC4 (FxROM)    16 KiB PRG, the same CHR latches
 *   11  Color Dreams    32 KiB PRG, 8 KiB CHR
 *   34  BNROM, NINA-001 32 KiB PRG (NINA-001: registers at $7FFD-$7FFF)
 *   66  GxROM           32 KiB PRG, 8 KiB CHR
 *   71  Camerica        16 KiB PRG at $8000
 *
 * Bus conflicts (a discrete-logic board ANDs the written value with the ROM
 * byte at that address) are applied only when a NES 2.0 header asks for
 * them (submapper 2): licensed games write over a matching ROM byte.
 */
#include "nes.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Header                                                              */
/* ------------------------------------------------------------------ */

static const char *board_name(unsigned mapper, uint32_t chr_size)
{
    switch (mapper) {
    case 0: return "NROM";
    case 1: return "MMC1";
    case 2: return "UxROM";
    case 3: return "CNROM";
    case 4: return "MMC3";
    case 7: return "AxROM";
    case 9: return "MMC2";
    case 10: return "MMC4";
    case 11: return "Color Dreams";
    case 34: return chr_size ? "NINA-001" : "BNROM";
    case 66: return "GxROM";
    case 71: return "Camerica";
    default: return NULL;
    }
}

static uint32_t crc32_bytes(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* NES 2.0 ROM size: a count of units, or exponent-multiplier notation when
 * the high nibble is $F. */
static uint64_t rom_size20(unsigned lsb, unsigned msb, uint32_t unit)
{
    if (msb == 0x0Fu) return ((uint64_t)1 << (lsb >> 2)) * ((lsb & 3u) * 2u + 1u);
    return (uint64_t)((msb << 8) | lsb) * unit;
}

bool nes_cart_parse(const uint8_t *file, size_t size, NesCartInfo *info)
{
    memset(info, 0, sizeof *info);
    if (!file || size < 16u) return false;
    if (file[0] != 'N' || file[1] != 'E' || file[2] != 'S' || file[3] != 0x1Au) return false;

    const uint8_t f6 = file[6], f7 = file[7];
    info->nes20 = (f7 & 0x0Cu) == 0x08u;
    info->battery = (f6 & 0x02u) != 0;
    info->trainer = (f6 & 0x04u) != 0;
    info->mirroring = (f6 & 0x08u) ? NES_MIRROR_FOUR : (f6 & 0x01u) ? NES_MIRROR_VERTICAL : NES_MIRROR_HORIZONTAL;

    uint64_t prg, chr;
    if (info->nes20) {
        info->mapper = (uint16_t)((f6 >> 4) | (f7 & 0xF0u) | ((file[8] & 0x0Fu) << 8));
        info->submapper = (uint8_t)(file[8] >> 4);
        prg = rom_size20(file[4], file[9] & 0x0Fu, 0x4000u);
        chr = rom_size20(file[5], file[9] >> 4, 0x2000u);
        const unsigned ram = file[10] & 0x0Fu, nvram = file[10] >> 4;
        info->prg_ram_size = (ram ? 64u << ram : 0u) + (nvram ? 64u << nvram : 0u);
        const unsigned cram = file[11] & 0x0Fu, cnvram = file[11] >> 4;
        info->chr_ram_size = (cram ? 64u << cram : 0u) + (cnvram ? 64u << cnvram : 0u);
        info->region = (uint8_t)(file[12] & 3u);
    } else {
        /* Old tools wrote text ("DiskDude!") over bytes 7-15; then the
         * high mapper nibble is garbage. */
        bool dirty = false;
        for (int i = 12; i < 16; i++) dirty = dirty || file[i] != 0;
        info->mapper = (uint16_t)((f6 >> 4) | (dirty ? 0u : (f7 & 0xF0u)));
        prg = (uint64_t)file[4] * 0x4000u;
        chr = (uint64_t)file[5] * 0x2000u;
        /* iNES does not say; 8 KiB at $6000 is what boards with RAM carry
         * and is harmless on the others. */
        info->prg_ram_size = 0x2000u;
        info->chr_ram_size = chr ? 0u : 0x2000u;
        info->region = NES_REGION_NTSC;
    }
    if (chr == 0 && info->chr_ram_size == 0) info->chr_ram_size = 0x2000u;
    if (info->prg_ram_size > NES_PRG_RAM_MAX) info->prg_ram_size = NES_PRG_RAM_MAX;
    if (info->chr_ram_size > NES_CHR_RAM_MAX) info->chr_ram_size = NES_CHR_RAM_MAX;

    const uint64_t prg_at = 16u + (info->trainer ? 512u : 0u);
    if (prg == 0 || prg > NES_PRG_ROM_MAX || chr > NES_CHR_ROM_MAX) return false;
    if (prg_at + prg + chr > size) return false;
    info->prg_size = (uint32_t)prg;
    info->chr_size = (uint32_t)chr;
    info->prg_offset = (uint32_t)prg_at;
    info->chr_offset = (uint32_t)(prg_at + prg);
    info->prg_crc32 = crc32_bytes(file + prg_at, (size_t)prg);
    info->board = board_name(info->mapper, info->chr_size);
    return true;
}

/* ------------------------------------------------------------------ */
/* Page tables                                                         */
/* ------------------------------------------------------------------ */

static uint32_t chr_total(const NesMachine *m) { return m->info.chr_size ? m->info.chr_size : m->info.chr_ram_size; }

/* PRG: slot 0-3 is $8000, $A000, $C000, $E000. Bank numbers wrap at the
 * ROM size, as the unconnected high address lines do. */
static void set_prg8(NesMachine *m, unsigned slot, uint32_t bank)
{
    m->cart.prg_page[slot & 3u] = (uint32_t)(((uint64_t)bank * 0x2000u) % m->info.prg_size);
}

static void set_prg16(NesMachine *m, unsigned half, uint32_t bank)
{
    set_prg8(m, half * 2u, bank * 2u);
    set_prg8(m, half * 2u + 1u, bank * 2u + 1u);
}

static void set_prg32(NesMachine *m, uint32_t bank)
{
    for (unsigned i = 0; i < 4; i++) set_prg8(m, i, bank * 4u + i);
}

/* The last 8 KiB bank, minus `back`. */
static uint32_t prg_last8(const NesMachine *m, uint32_t back) { return m->info.prg_size / 0x2000u - 1u - back; }

/* CHR: `count` 1 KiB pages starting at slot, from a bank of that many KiB. */
static void set_chr(NesMachine *m, unsigned slot, unsigned count, uint32_t bank)
{
    const uint32_t total = chr_total(m);
    for (unsigned i = 0; i < count; i++) {
        m->cart.chr_page[(slot + i) & 7u] = (uint32_t)((((uint64_t)bank * count + i) * 0x400u) % total);
    }
}

static void set_mirroring(NesMachine *m, unsigned mode)
{
    static const uint16_t layout[5][4] = {
        { 0x000, 0x000, 0x400, 0x400 }, /* horizontal */
        { 0x000, 0x400, 0x000, 0x400 }, /* vertical */
        { 0x000, 0x000, 0x000, 0x000 }, /* single, first */
        { 0x400, 0x400, 0x400, 0x400 }, /* single, second */
        { 0x000, 0x400, 0x800, 0xC00 }, /* four screens */
    };
    /* A four-screen board has its own RAM and no mirroring to switch. */
    if (m->info.mirroring == NES_MIRROR_FOUR) mode = NES_MIRROR_FOUR;
    for (unsigned i = 0; i < 4; i++) m->cart.nt_page[i] = layout[mode][i];
}

/* ------------------------------------------------------------------ */
/* Boards                                                              */
/* ------------------------------------------------------------------ */

/* MMC1. reg[0] control, reg[1] CHR bank 0, reg[2] CHR bank 1, reg[3] PRG. */
static void update_mmc1(NesMachine *m)
{
    NesCart *k = &m->cart;
    const uint8_t control = k->reg[0], chr0 = k->reg[1], chr1 = k->reg[2], prg = k->reg[3];
    static const uint8_t mirror[4] = { NES_MIRROR_SINGLE_0, NES_MIRROR_SINGLE_1, NES_MIRROR_VERTICAL,
                                       NES_MIRROR_HORIZONTAL };
    set_mirroring(m, mirror[control & 3u]);

    /* 512 KiB boards (SUROM, SXROM) take the 256 KiB half from bit 4 of the
     * CHR register, which has no CHR ROM to select there. */
    const uint32_t outer = m->info.prg_size > 0x40000u ? (chr0 & 0x10u) : 0u;
    const uint32_t bank = prg & 0x0Fu;
    switch ((control >> 2) & 3u) {
    case 0:
    case 1: /* 32 KiB */
        set_prg16(m, 0, outer | (bank & ~1u));
        set_prg16(m, 1, outer | bank | 1u);
        break;
    case 2: /* first bank fixed at $8000 */
        set_prg16(m, 0, outer);
        set_prg16(m, 1, outer | bank);
        break;
    default: /* last bank fixed at $C000 */
        set_prg16(m, 0, outer | bank);
        set_prg16(m, 1, outer | 0x0Fu);
        break;
    }

    if (control & 0x10u) {
        set_chr(m, 0, 4, chr0);
        set_chr(m, 4, 4, chr1);
    } else {
        set_chr(m, 0, 8, chr0 >> 1);
    }

    /* Boards with more than 8 KiB of PRG RAM bank it with CHR register
     * bits: 16 KiB (SOROM) with bit 3, 32 KiB (SXROM) with bits 2-3. */
    uint32_t ram_bank = 0;
    if (m->info.prg_ram_size == 0x4000u) ram_bank = (chr0 >> 3) & 1u;
    else if (m->info.prg_ram_size == 0x8000u) ram_bank = (chr0 >> 2) & 3u;
    k->prg_ram_page = ram_bank * 0x2000u;
    k->prg_ram_enable = !(prg & 0x10u);
}

static void write_mmc1(NesMachine *m, uint16_t addr, uint8_t value)
{
    NesCart *k = &m->cart;
    if (addr < 0x8000u) return;
    /* The serial port ignores the data bit of a write on the cycle after
     * another write: a read-modify-write instruction's two writes shift in
     * once. The reset bit is never ignored. */
    const uint64_t now = m->cpu.cycles;
    const bool consecutive = now - k->last_write < 2u;
    k->last_write = now;
    if (value & 0x80u) {
        k->shift = 0;
        k->shift_count = 0;
        k->reg[0] |= 0x0Cu;
        return;
    }
    if (consecutive) return;
    k->shift |= (uint8_t)((value & 1u) << k->shift_count);
    if (++k->shift_count == 5u) {
        k->reg[(addr >> 13) & 3u] = k->shift;
        k->shift = 0;
        k->shift_count = 0;
    }
}

/* MMC3. reg[0-7] are R0-R7, reg[8] mirroring ($A000), reg[9] PRG RAM
 * protect ($A001). */
static void update_mmc3(NesMachine *m)
{
    NesCart *k = &m->cart;
    if (k->bank_select & 0x40u) {
        set_prg8(m, 0, prg_last8(m, 1));
        set_prg8(m, 2, k->reg[6]);
    } else {
        set_prg8(m, 0, k->reg[6]);
        set_prg8(m, 2, prg_last8(m, 1));
    }
    set_prg8(m, 1, k->reg[7]);
    set_prg8(m, 3, prg_last8(m, 0));

    /* Two 2 KiB and four 1 KiB CHR banks; bit 7 swaps which half of the
     * pattern space gets which. */
    const unsigned flip = (k->bank_select & 0x80u) ? 4u : 0u;
    set_chr(m, 0u ^ flip, 2, k->reg[0] >> 1);
    set_chr(m, 2u ^ flip, 2, k->reg[1] >> 1);
    for (unsigned i = 0; i < 4; i++) set_chr(m, (4u + i) ^ flip, 1, k->reg[2 + i]);

    set_mirroring(m, (k->reg[8] & 1u) ? NES_MIRROR_HORIZONTAL : NES_MIRROR_VERTICAL);
    k->prg_ram_enable = (k->reg[9] & 0x80u) != 0;
    k->prg_ram_protect = (k->reg[9] & 0x40u) != 0;
}

static void write_mmc3(NesMachine *m, uint16_t addr, uint8_t value)
{
    NesCart *k = &m->cart;
    if (addr < 0x8000u) return;
    switch (addr & 0xE001u) {
    case 0x8000: k->bank_select = value; break;
    case 0x8001: k->reg[k->bank_select & 7u] = value; break;
    case 0xA000: k->reg[8] = value; break;
    case 0xA001: k->reg[9] = value; break;
    case 0xC000: k->irq_latch = value; break;
    case 0xC001:
        k->irq_counter = 0;
        k->irq_reload = 1;
        break;
    case 0xE000:
        k->irq_enable = 0;
        k->irq = 0;
        break;
    default: k->irq_enable = 1; break;
    }
}

/* The MMC3 counts scanlines from PPU A12: a rise after the line has been
 * low for a few CPU cycles clocks the counter. With the background at
 * $0000 and sprites at $1000 that happens once per line, at the first
 * sprite fetch (dot 261); the rises between sprite fetches are too close
 * together to count. With the tables the other way round it is the first
 * background fetch after the sprites', dot 325 of the line before, and
 * dot 5 of the pre-render line after VBlank. Reaching zero with the IRQ
 * enabled raises the line (the Sharp MMC3 behaviour: a reload to zero
 * raises it too; mmc3_test_2/5-MMC3 passes, 6-MMC3_alt is the other
 * revision).
 *
 * The chip's filter is three falling edges of M2 with A12 low (nesdev
 * wiki, MMC3), seven to nine dots. The PPU's own fetches only make low
 * stretches of 4 dots (between the pattern fetches of one run) or of 12
 * and more, so any threshold from 5 to 12 dots gives the same clocks;
 * blargg's mmc3_test_2 and mmc3_irq_tests pass with it. */
#define MMC3_A12_LOW_DOTS 9u

static void mmc3_a12(NesMachine *m, uint16_t addr, uint64_t dots)
{
    NesCart *k = &m->cart;
    const uint8_t a12 = (uint8_t)((addr >> 12) & 1u);
    if (a12 && !k->a12) {
        if (dots - k->a12_low_since >= MMC3_A12_LOW_DOTS) {
            if (k->irq_counter == 0 || k->irq_reload) {
                k->irq_counter = k->irq_latch;
                k->irq_reload = 0;
            } else {
                k->irq_counter--;
            }
            if (k->irq_counter == 0 && k->irq_enable) k->irq = 1;
        }
    } else if (!a12 && k->a12) {
        k->a12_low_since = dots;
    }
    k->a12 = a12;
}

/* MMC2 and MMC4. reg[0] PRG, reg[1]/reg[2] the $0000 CHR bank for latch
 * values $FD/$FE, reg[3]/reg[4] the same for $1000, reg[5] mirroring. */
static void update_mmc2(NesMachine *m)
{
    NesCart *k = &m->cart;
    if (m->info.mapper == 9) {
        set_prg8(m, 0, k->reg[0] & 0x0Fu);
        set_prg8(m, 1, prg_last8(m, 2));
        set_prg8(m, 2, prg_last8(m, 1));
        set_prg8(m, 3, prg_last8(m, 0));
    } else {
        set_prg16(m, 0, k->reg[0] & 0x0Fu);
        set_prg16(m, 1, m->info.prg_size / 0x4000u - 1u);
    }
    set_chr(m, 0, 4, k->reg[k->latch[0] ? 2 : 1] & 0x1Fu);
    set_chr(m, 4, 4, k->reg[k->latch[1] ? 4 : 3] & 0x1Fu);
    set_mirroring(m, (k->reg[5] & 1u) ? NES_MIRROR_HORIZONTAL : NES_MIRROR_VERTICAL);
}

static void write_mmc2(NesMachine *m, uint16_t addr, uint8_t value)
{
    if (addr >= 0xA000u) m->cart.reg[(addr >> 12) - 0x0Au] = value;
}

/* The latches change after the tile is fetched, so the tile that trips one
 * is still drawn from the old bank. The MMC2's left latch answers to one
 * address each way, the MMC4's to eight like the right one. */
static void mmc2_latch(NesMachine *m, uint16_t addr)
{
    NesCart *k = &m->cart;
    const uint16_t left_mask = m->info.mapper == 9 ? 0x3FFFu : 0x3FF8u;
    uint8_t latch0 = k->latch[0], latch1 = k->latch[1];
    if ((addr & left_mask) == 0x0FD8u) latch0 = 0;
    else if ((addr & left_mask) == 0x0FE8u) latch0 = 1;
    else if ((addr & 0x3FF8u) == 0x1FD8u) latch1 = 0;
    else if ((addr & 0x3FF8u) == 0x1FE8u) latch1 = 1;
    else return;
    if (latch0 == k->latch[0] && latch1 == k->latch[1]) return;
    k->latch[0] = latch0;
    k->latch[1] = latch1;
    update_mmc2(m);
}

/* The discrete-logic boards: one register, reg[0] (NINA-001 has three). */
static void update_simple(NesMachine *m)
{
    NesCart *k = &m->cart;
    const uint8_t v = k->reg[0];
    set_mirroring(m, m->info.mirroring);
    set_chr(m, 0, 8, 0);
    switch (m->info.mapper) {
    case 0:
        /* 16 KiB boards mirror it at $C000; set_prg8 wraps. */
        set_prg32(m, 0);
        break;
    case 2:
        set_prg16(m, 0, v);
        set_prg16(m, 1, m->info.prg_size / 0x4000u - 1u);
        break;
    case 3:
        set_prg32(m, 0);
        set_chr(m, 0, 8, v);
        break;
    case 7:
        set_prg32(m, v & 0x0Fu);
        set_mirroring(m, (v & 0x10u) ? NES_MIRROR_SINGLE_1 : NES_MIRROR_SINGLE_0);
        break;
    case 11:
        set_prg32(m, v & 0x03u);
        set_chr(m, 0, 8, v >> 4);
        break;
    case 34:
        if (m->info.chr_size) {
            set_prg32(m, k->reg[1] & 1u);
            set_chr(m, 0, 4, k->reg[2] & 0x0Fu);
            set_chr(m, 4, 4, k->reg[3] & 0x0Fu);
        } else {
            set_prg32(m, v);
        }
        break;
    case 66:
        set_prg32(m, (v >> 4) & 0x03u);
        set_chr(m, 0, 8, v & 0x03u);
        break;
    case 71:
        set_prg16(m, 0, v);
        set_prg16(m, 1, m->info.prg_size / 0x4000u - 1u);
        break;
    default: set_prg32(m, 0); break;
    }
}

static void write_simple(NesMachine *m, uint16_t addr, uint8_t value)
{
    NesCart *k = &m->cart;
    if (m->info.mapper == 34 && m->info.chr_size) {
        if (addr >= 0x7FFDu && addr <= 0x7FFFu) k->reg[addr - 0x7FFCu] = value;
        return;
    }
    if (addr < 0x8000u) return;
    if (m->info.mapper == 0) return;
    if (m->info.mapper == 71 && addr < 0xC000u) return; /* $8000-$BFFF: mirroring on one board only */
    if (m->info.submapper == 2) {
        value &= m->prg[m->cart.prg_page[(addr >> 13) & 3u] + (addr & 0x1FFFu)];
    }
    k->reg[0] = value;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void update(NesMachine *m)
{
    NesCart *k = &m->cart;
    uint32_t before[4];
    memcpy(before, k->prg_page, sizeof before);
    switch (m->info.mapper) {
    case 1: update_mmc1(m); break;
    case 4: update_mmc3(m); break;
    case 9:
    case 10: update_mmc2(m); break;
    default: update_simple(m); break;
    }
    if (memcmp(before, k->prg_page, sizeof before) != 0) k->prg_gen++;
}

void nes_cart_power(NesMachine *m)
{
    NesCart *k = &m->cart;
    const uint64_t gen = k->prg_gen;
    memset(k, 0, sizeof *k);
    k->prg_gen = gen + 1u;
    k->chr_writable = m->info.chr_size == 0;
    k->prg_ram_enable = m->info.prg_ram_size != 0;
    switch (m->info.mapper) {
    case 1: k->reg[0] = 0x0Cu; break; /* last bank fixed at $C000 */
    case 4:
        k->reg[0] = 0;
        k->reg[1] = 2;
        k->reg[2] = 4;
        k->reg[3] = 5;
        k->reg[4] = 6;
        k->reg[5] = 7;
        k->reg[6] = 0;
        k->reg[7] = 1;
        k->reg[9] = 0x80u; /* PRG RAM enabled: games that never write $A001 expect it */
        break;
    case 9:
    case 10:
        k->latch[0] = k->latch[1] = 1; /* $FE */
        break;
    default: break;
    }
    update(m);
    if (m->info.trainer && m->file) memcpy(&m->prg_ram[0x1000], m->file + 16, 512);
}

uint8_t nes_cart_read(NesMachine *m, uint16_t addr, uint8_t mdr)
{
    const NesCart *k = &m->cart;
    if (addr >= 0x8000u) return m->prg[k->prg_page[(addr >> 13) & 3u] + (addr & 0x1FFFu)];
    if (addr >= 0x6000u && k->prg_ram_enable && m->info.prg_ram_size) {
        return m->prg_ram[(k->prg_ram_page + (addr & 0x1FFFu)) % m->info.prg_ram_size];
    }
    return mdr;
}

void nes_cart_write(NesMachine *m, uint16_t addr, uint8_t value)
{
    NesCart *k = &m->cart;
    if (addr >= 0x6000u && addr < 0x8000u && k->prg_ram_enable && !k->prg_ram_protect && m->info.prg_ram_size) {
        m->prg_ram[(k->prg_ram_page + (addr & 0x1FFFu)) % m->info.prg_ram_size] = value;
        m->prg_ram_dirty = true;
    }
    switch (m->info.mapper) {
    case 1: write_mmc1(m, addr, value); break;
    case 4: write_mmc3(m, addr, value); break;
    case 9:
    case 10: write_mmc2(m, addr, value); break;
    default: write_simple(m, addr, value); break;
    }
    update(m);
}

uint8_t nes_cart_ppu_read(NesMachine *m, uint16_t addr, uint64_t dots)
{
    NesCart *k = &m->cart;
    uint8_t value;
    addr &= 0x3FFFu;
    if (addr < 0x2000u) {
        const uint32_t at = k->chr_page[addr >> 10] + (addr & 0x3FFu);
        value = k->chr_writable ? m->chr_ram[at] : m->chr[at];
    } else {
        value = m->nt_ram[k->nt_page[(addr >> 10) & 3u] + (addr & 0x3FFu)];
    }
    if (m->info.mapper == 4) mmc3_a12(m, addr, dots);
    else if (m->info.mapper == 9 || m->info.mapper == 10) mmc2_latch(m, addr);
    return value;
}

void nes_cart_ppu_write(NesMachine *m, uint16_t addr, uint8_t value, uint64_t dots)
{
    NesCart *k = &m->cart;
    addr &= 0x3FFFu;
    if (addr < 0x2000u) {
        if (k->chr_writable) m->chr_ram[k->chr_page[addr >> 10] + (addr & 0x3FFu)] = value;
    } else {
        m->nt_ram[k->nt_page[(addr >> 10) & 3u] + (addr & 0x3FFu)] = value;
    }
    if (m->info.mapper == 4) mmc3_a12(m, addr, dots);
}

void nes_cart_ppu_address(NesMachine *m, uint16_t addr, uint64_t dots)
{
    if (m->info.mapper == 4) mmc3_a12(m, (uint16_t)(addr & 0x3FFFu), dots);
}

bool nes_cart_watches_ppu(const NesMachine *m) { return m->info.mapper == 4 && m->cart.irq_enable; }

uint32_t nes_cart_prg_page_at(const NesMachine *m, uint16_t addr)
{
    return m->cart.prg_page[(addr >> 13) & 3u] / NES_PRG_PAGE;
}
