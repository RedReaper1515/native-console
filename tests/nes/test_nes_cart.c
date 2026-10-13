/*
 * Cartridge unit tests on synthetic images: the iNES / NES 2.0 header, and
 * each board's banking, mirroring and interrupt, driven through the same
 * entry points the bus uses. Every PRG byte holds its 8 KiB page number and
 * every CHR byte its 1 KiB page number, so a read says which page is mapped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes.h"
#include "nes_recomp.h"

static int g_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(got, want)                                                                                            \
    do {                                                                                                               \
        long long g_ = (long long)(got), w_ = (long long)(want);                                                       \
        if (g_ != w_) {                                                                                                \
            printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #got, g_, w_);                      \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* An image with `prg16` 16 KiB PRG banks and `chr8` 8 KiB CHR banks. */
static uint8_t *make_image(unsigned mapper, unsigned prg16, unsigned chr8, uint8_t flags6, size_t *size)
{
    const size_t prg = (size_t)prg16 * 0x4000u, chr = (size_t)chr8 * 0x2000u;
    *size = 16u + prg + chr;
    uint8_t *file = calloc(1, *size);
    memcpy(file, "NES\x1A", 4);
    file[4] = (uint8_t)prg16;
    file[5] = (uint8_t)chr8;
    file[6] = (uint8_t)(((mapper & 0x0Fu) << 4) | flags6);
    file[7] = (uint8_t)(mapper & 0xF0u);
    for (size_t i = 0; i < prg; i++) file[16 + i] = (uint8_t)(i / 0x2000u);
    for (size_t i = 0; i < chr; i++) file[16 + prg + i] = (uint8_t)(i / 0x400u);
    /* Reset vector: $8000, so power-on finds code space. */
    file[16 + prg - 4] = 0x00;
    file[16 + prg - 3] = 0x80;
    return file;
}

static NesMachine *make_machine(unsigned mapper, unsigned prg16, unsigned chr8, uint8_t flags6)
{
    size_t size;
    uint8_t *file = make_image(mapper, prg16, chr8, flags6, &size);
    NesMachine *m = nes_create();
    if (!m || !nes_load_rom(m, file, size)) {
        printf("  FAIL: cannot load a mapper %u image\n", mapper);
        g_failures++;
        free(file);
        nes_destroy(m);
        return NULL;
    }
    free(file);
    nes_power(m);
    return m;
}

/* The PRG page number at a CPU address, and the CHR page at a PPU address. */
static unsigned prg_at(NesMachine *m, uint16_t addr) { return nes_cart_read(m, addr, 0); }
static unsigned chr_at(NesMachine *m, uint16_t addr) { return nes_cart_ppu_read(m, addr, 0); }

/* A register write some cycles after the last one (the MMC1 ignores writes
 * on consecutive cycles). */
static void reg_write(NesMachine *m, uint16_t addr, uint8_t value)
{
    m->cpu.cycles += 10;
    nes_cart_write(m, addr, value);
}

/* Which nametable RAM page each of the four slots uses. */
static void check_mirroring(NesMachine *m, unsigned a, unsigned b, unsigned c, unsigned d)
{
    CHECK_EQ(m->cart.nt_page[0], a * 0x400u);
    CHECK_EQ(m->cart.nt_page[1], b * 0x400u);
    CHECK_EQ(m->cart.nt_page[2], c * 0x400u);
    CHECK_EQ(m->cart.nt_page[3], d * 0x400u);
}

static void test_header(void)
{
    printf("header\n");
    size_t size;
    NesCartInfo info;
    uint8_t *file = make_image(1, 16, 0, 0x02, &size);
    CHECK(nes_cart_parse(file, size, &info));
    CHECK_EQ(info.mapper, 1);
    CHECK_EQ(info.prg_size, 0x40000);
    CHECK_EQ(info.chr_size, 0);
    CHECK_EQ(info.chr_ram_size, 0x2000);
    CHECK_EQ(info.prg_ram_size, 0x2000);
    CHECK(info.battery && !info.nes20 && !info.trainer);
    CHECK_EQ(info.mirroring, NES_MIRROR_HORIZONTAL);
    CHECK(info.board != NULL);

    /* NES 2.0: the mapper's high bits, RAM sizes as shift counts. */
    file[7] = 0x08;
    file[8] = 0x00;
    file[10] = 0x70; /* 8 KiB battery RAM */
    file[11] = 0x07; /* 8 KiB CHR RAM */
    CHECK(nes_cart_parse(file, size, &info));
    CHECK(info.nes20);
    CHECK_EQ(info.prg_ram_size, 0x2000);
    CHECK_EQ(info.chr_ram_size, 0x2000);
    file[10] = 0x00;
    CHECK(nes_cart_parse(file, size, &info));
    CHECK_EQ(info.prg_ram_size, 0);

    /* "DiskDude!" over bytes 7-15: the high mapper nibble is garbage. */
    memcpy(file + 7, "DiskDude!", 9);
    CHECK(nes_cart_parse(file, size, &info));
    CHECK_EQ(info.mapper, 1);

    /* Not an image, a truncated one, an unknown board. */
    memset(file + 7, 0, 9);
    CHECK(!nes_cart_parse(file, size - 1, &info));
    file[0] = 'X';
    CHECK(!nes_cart_parse(file, size, &info));
    file[0] = 'N';
    file[6] = 0xF2;
    file[7] = 0xF0; /* mapper 255 */
    CHECK(nes_cart_parse(file, size, &info));
    CHECK(info.board == NULL);
    NesMachine *m = nes_create();
    CHECK(!nes_load_rom(m, file, size));
    nes_destroy(m);
    free(file);
}

static void test_nrom(void)
{
    printf("nrom\n");
    NesMachine *m = make_machine(0, 1, 1, 0x01);
    if (!m) return;
    /* 16 KiB is mirrored at $C000. */
    CHECK_EQ(prg_at(m, 0x8000), 0);
    CHECK_EQ(prg_at(m, 0xA000), 1);
    CHECK_EQ(prg_at(m, 0xC000), 0);
    CHECK_EQ(prg_at(m, 0xFFFF), 1);
    CHECK_EQ(nes_rc_key_at(m, 0xC123), NES_RC_KEY(0, 0xC123));
    CHECK_EQ(nes_rc_key_at(m, 0x0200), NES_RC_NO_KEY);
    check_mirroring(m, 0, 1, 0, 1); /* vertical */
    /* PRG RAM, and writes to ROM that change nothing. */
    nes_cart_write(m, 0x6123, 0x5A);
    CHECK_EQ(nes_cart_read(m, 0x6123, 0), 0x5A);
    CHECK(m->prg_ram_dirty);
    const uint64_t gen = m->cart.prg_gen;
    reg_write(m, 0x8000, 0xFF);
    CHECK_EQ(m->cart.prg_gen, gen);
    /* Open bus below $6000. */
    CHECK_EQ(nes_cart_read(m, 0x5000, 0x42), 0x42);
    nes_destroy(m);
}

static void mmc1_load(NesMachine *m, uint16_t addr, uint8_t value)
{
    for (int bit = 0; bit < 5; bit++) reg_write(m, addr, (uint8_t)((value >> bit) & 1u));
}

static void test_mmc1(void)
{
    printf("mmc1\n");
    NesMachine *m = make_machine(1, 8, 4, 0x00); /* 128 KiB PRG, 32 KiB CHR */
    if (!m) return;
    /* Power-on: the last bank fixed at $C000, bank 0 at $8000. */
    CHECK_EQ(prg_at(m, 0x8000), 0);
    CHECK_EQ(prg_at(m, 0xBFFF), 1);
    CHECK_EQ(prg_at(m, 0xC000), 14);
    CHECK_EQ(prg_at(m, 0xFFFF), 15);

    /* Five serial writes select a bank; the generation counts the change. */
    uint64_t gen = m->cart.prg_gen;
    mmc1_load(m, 0xE000, 3);
    CHECK_EQ(prg_at(m, 0x8000), 6);
    CHECK_EQ(prg_at(m, 0xA000), 7);
    CHECK_EQ(prg_at(m, 0xC000), 14);
    CHECK(m->cart.prg_gen > gen);
    CHECK_EQ(nes_rc_key_at(m, 0x8010), NES_RC_KEY(6, 0x8010));

    /* A write with bit 7 resets the shift register mid-sequence. */
    reg_write(m, 0xE000, 1);
    reg_write(m, 0xE000, 1);
    reg_write(m, 0xE000, 0x80);
    mmc1_load(m, 0xE000, 5);
    CHECK_EQ(prg_at(m, 0x8000), 10);

    /* The second of two writes on consecutive cycles is ignored: a
     * read-modify-write on a ROM byte holding $FF resets the register and
     * its second write (bit 0 = 0) does not shift in. */
    reg_write(m, 0xE000, 1);
    reg_write(m, 0xFFFF, 0xFF);
    m->cpu.cycles += 1;
    nes_cart_write(m, 0xFFFF, 0x00);
    CHECK_EQ(m->cart.shift_count, 0);
    mmc1_load(m, 0xE000, 2);
    CHECK_EQ(prg_at(m, 0x8000), 4);

    /* Control: mode 2 fixes the first bank at $8000 and switches $C000;
     * modes 0/1 switch 32 KiB. */
    mmc1_load(m, 0x8000, 0x08);
    CHECK_EQ(prg_at(m, 0x8000), 0);
    CHECK_EQ(prg_at(m, 0xC000), 4);
    mmc1_load(m, 0x8000, 0x00);
    mmc1_load(m, 0xE000, 5);
    CHECK_EQ(prg_at(m, 0x8000), 8);
    CHECK_EQ(prg_at(m, 0xC000), 10);
    /* The reset bit forces the last bank back to $C000. */
    reg_write(m, 0x8000, 0x80);
    CHECK_EQ(prg_at(m, 0xC000), 14);
    CHECK_EQ(prg_at(m, 0x8000), 10);

    /* Mirroring: control bits 0-1. */
    mmc1_load(m, 0x8000, 0x0C);
    check_mirroring(m, 0, 0, 0, 0);
    mmc1_load(m, 0x8000, 0x0D);
    check_mirroring(m, 1, 1, 1, 1);
    mmc1_load(m, 0x8000, 0x0E);
    check_mirroring(m, 0, 1, 0, 1);
    mmc1_load(m, 0x8000, 0x0F);
    check_mirroring(m, 0, 0, 1, 1);

    /* CHR: 8 KiB mode ignores bit 0; 4 KiB mode has two banks. */
    mmc1_load(m, 0xA000, 3);
    CHECK_EQ(chr_at(m, 0x0000), 8);
    CHECK_EQ(chr_at(m, 0x1C00), 15);
    mmc1_load(m, 0x8000, 0x1F);
    mmc1_load(m, 0xA000, 5);
    mmc1_load(m, 0xC000, 2);
    CHECK_EQ(chr_at(m, 0x0000), 20);
    CHECK_EQ(chr_at(m, 0x1000), 8);

    /* PRG register bit 4 disables PRG RAM. */
    nes_cart_write(m, 0x6000, 0x77);
    CHECK_EQ(nes_cart_read(m, 0x6000, 0xEE), 0x77);
    mmc1_load(m, 0xE000, 0x10);
    CHECK_EQ(nes_cart_read(m, 0x6000, 0xEE), 0xEE);
    nes_destroy(m);
}

static void test_uxrom_cnrom_axrom(void)
{
    printf("uxrom, cnrom, axrom, gxrom\n");
    NesMachine *m = make_machine(2, 8, 0, 0x00);
    if (m) {
        CHECK_EQ(prg_at(m, 0x8000), 0);
        CHECK_EQ(prg_at(m, 0xC000), 14);
        reg_write(m, 0x8000, 5);
        CHECK_EQ(prg_at(m, 0x8000), 10);
        CHECK_EQ(prg_at(m, 0xA000), 11);
        CHECK_EQ(prg_at(m, 0xE000), 15);
        /* CHR RAM is writable. */
        nes_cart_ppu_write(m, 0x0123, 0x99, 0);
        CHECK_EQ(chr_at(m, 0x0123), 0x99);
        nes_destroy(m);
    }
    m = make_machine(3, 2, 4, 0x00);
    if (m) {
        const uint64_t gen = m->cart.prg_gen;
        reg_write(m, 0x8000, 2);
        CHECK_EQ(chr_at(m, 0x0000), 16);
        CHECK_EQ(chr_at(m, 0x1C00), 23);
        CHECK_EQ(prg_at(m, 0x8000), 0);
        CHECK_EQ(m->cart.prg_gen, gen); /* a CHR switch is not a PRG change */
        /* CHR ROM ignores writes. */
        nes_cart_ppu_write(m, 0x0000, 0x99, 0);
        CHECK_EQ(chr_at(m, 0x0000), 16);
        nes_destroy(m);
    }
    m = make_machine(7, 8, 0, 0x00);
    if (m) {
        reg_write(m, 0x8000, 0x12);
        CHECK_EQ(prg_at(m, 0x8000), 8);
        CHECK_EQ(prg_at(m, 0xE000), 11);
        check_mirroring(m, 1, 1, 1, 1);
        reg_write(m, 0x8000, 0x01);
        CHECK_EQ(prg_at(m, 0x8000), 4);
        check_mirroring(m, 0, 0, 0, 0);
        nes_destroy(m);
    }
    m = make_machine(66, 8, 4, 0x00);
    if (m) {
        reg_write(m, 0x8000, 0x21);
        CHECK_EQ(prg_at(m, 0x8000), 8);
        CHECK_EQ(chr_at(m, 0x0000), 8);
        nes_destroy(m);
    }
}

/* Make PPU A12 rise at dot `dots` after it has been low long enough. */
static void a12_rise(NesMachine *m, uint64_t *dots)
{
    nes_cart_ppu_address(m, 0x0000, *dots);
    *dots += 100;
    nes_cart_ppu_address(m, 0x1000, *dots);
    *dots += 4;
}

static void test_mmc3(void)
{
    printf("mmc3\n");
    NesMachine *m = make_machine(4, 8, 8, 0x00); /* 16 PRG pages, 64 CHR pages */
    if (!m) return;
    /* Power-on: R6 at $8000, R7 at $A000, the last two pages fixed. */
    CHECK_EQ(prg_at(m, 0xC000), 14);
    CHECK_EQ(prg_at(m, 0xE000), 15);
    reg_write(m, 0x8000, 6);
    reg_write(m, 0x8001, 5);
    reg_write(m, 0x8000, 7);
    reg_write(m, 0x8001, 9);
    CHECK_EQ(prg_at(m, 0x8000), 5);
    CHECK_EQ(prg_at(m, 0xA000), 9);
    CHECK_EQ(nes_rc_key_at(m, 0xA004), NES_RC_KEY(9, 0xA004));
    /* Bit 6 swaps $8000 and $C000. */
    reg_write(m, 0x8000, 0x46);
    CHECK_EQ(prg_at(m, 0x8000), 14);
    CHECK_EQ(prg_at(m, 0xC000), 5);
    CHECK_EQ(prg_at(m, 0xE000), 15);

    /* CHR: R0/R1 are 2 KiB, R2-R5 1 KiB; bit 7 swaps the halves. */
    reg_write(m, 0x8000, 0);
    reg_write(m, 0x8001, 0x10);
    reg_write(m, 0x8000, 2);
    reg_write(m, 0x8001, 0x21);
    CHECK_EQ(chr_at(m, 0x0000), 0x10);
    CHECK_EQ(chr_at(m, 0x0400), 0x11);
    CHECK_EQ(chr_at(m, 0x1000), 0x21);
    reg_write(m, 0x8000, 0x80);
    CHECK_EQ(chr_at(m, 0x1000), 0x10);
    CHECK_EQ(chr_at(m, 0x0000), 0x21);

    /* Mirroring. */
    reg_write(m, 0xA000, 0);
    check_mirroring(m, 0, 1, 0, 1);
    reg_write(m, 0xA000, 1);
    check_mirroring(m, 0, 0, 1, 1);

    /* The scanline counter: reload to the latch, count down, raise the IRQ
     * at zero when enabled; $E000 acknowledges and disables. */
    uint64_t dots = 1000;
    reg_write(m, 0xC000, 2); /* latch */
    reg_write(m, 0xC001, 0); /* reload at the next clock */
    reg_write(m, 0xE001, 0); /* enable */
    CHECK(nes_cart_watches_ppu(m));
    a12_rise(m, &dots);      /* counter = 2 */
    CHECK_EQ(m->cart.irq, 0);
    a12_rise(m, &dots);      /* 1 */
    CHECK_EQ(m->cart.irq, 0);
    a12_rise(m, &dots);      /* 0: IRQ */
    CHECK_EQ(m->cart.irq, 1);
    reg_write(m, 0xE000, 0);
    CHECK_EQ(m->cart.irq, 0);
    CHECK(!nes_cart_watches_ppu(m));
    /* Disabled: the counter still runs, no interrupt. */
    for (int i = 0; i < 6; i++) a12_rise(m, &dots);
    CHECK_EQ(m->cart.irq, 0);

    /* A rise soon after a fall does not count (sprite fetches toggle A12
     * every 8 dots). */
    reg_write(m, 0xC000, 1);
    reg_write(m, 0xC001, 0);
    reg_write(m, 0xE001, 0);
    a12_rise(m, &dots); /* counter = 1 */
    for (int i = 0; i < 8; i++) {
        nes_cart_ppu_address(m, 0x0000, dots);
        dots += 4;
        nes_cart_ppu_address(m, 0x1000, dots);
        dots += 4;
    }
    CHECK_EQ(m->cart.irq, 0);
    a12_rise(m, &dots); /* 0: IRQ */
    CHECK_EQ(m->cart.irq, 1);

    /* PRG RAM protect. */
    reg_write(m, 0xA001, 0x80);
    nes_cart_write(m, 0x6000, 0x11);
    CHECK_EQ(nes_cart_read(m, 0x6000, 0), 0x11);
    reg_write(m, 0xA001, 0xC0);
    nes_cart_write(m, 0x6000, 0x22);
    CHECK_EQ(nes_cart_read(m, 0x6000, 0), 0x11);
    nes_destroy(m);
}

static void test_mmc2(void)
{
    printf("mmc2\n");
    NesMachine *m = make_machine(9, 8, 8, 0x00);
    if (!m) return;
    CHECK_EQ(prg_at(m, 0xA000), 13);
    CHECK_EQ(prg_at(m, 0xE000), 15);
    reg_write(m, 0xA000, 3);
    CHECK_EQ(prg_at(m, 0x8000), 3);
    /* $B000/$C000: the left bank for latch $FD / $FE; $D000/$E000: right. */
    reg_write(m, 0xB000, 1);
    reg_write(m, 0xC000, 2);
    reg_write(m, 0xD000, 3);
    reg_write(m, 0xE000, 4);
    CHECK_EQ(chr_at(m, 0x0000), 8);  /* latch 0 powers on at $FE: bank 2 */
    CHECK_EQ(chr_at(m, 0x1000), 16); /* latch 1 at $FE: bank 4 */
    /* Fetching tile $FD's row switches after the fetch. */
    CHECK_EQ(chr_at(m, 0x0FD8), 11);
    CHECK_EQ(chr_at(m, 0x0000), 4);
    CHECK_EQ(chr_at(m, 0x1FD8), 19);
    CHECK_EQ(chr_at(m, 0x1000), 12);
    (void)chr_at(m, 0x1FE8);
    CHECK_EQ(chr_at(m, 0x1000), 16);
    nes_destroy(m);
}

static void test_four_screen(void)
{
    printf("four-screen\n");
    NesMachine *m = make_machine(4, 8, 8, 0x08);
    if (!m) return;
    check_mirroring(m, 0, 1, 2, 3);
    reg_write(m, 0xA000, 1); /* the mirroring register has nothing to switch */
    check_mirroring(m, 0, 1, 2, 3);
    nes_cart_ppu_write(m, 0x2C05, 0x66, 0);
    CHECK_EQ(nes_cart_ppu_read(m, 0x2C05, 0), 0x66);
    CHECK_EQ(nes_cart_ppu_read(m, 0x2005, 0), 0x00);
    nes_destroy(m);
}

int main(void)
{
    test_header();
    test_nrom();
    test_mmc1();
    test_uxrom_cnrom_axrom();
    test_mmc3();
    test_mmc2();
    test_four_screen();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all cartridge tests passed\n");
    return 0;
}
