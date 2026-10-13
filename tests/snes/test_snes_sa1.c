/*
 * SA-1 unit tests against a synthetic 4 MiB SA-1 cartridge built in memory:
 * Super MMC banking and the S-CPU vector override, BW-RAM windows, bitmap
 * view and write protection, I-RAM protection, the multiplier/divider/
 * accumulator, the variable-length bit reader, normal DMA, character
 * conversion types 1 and 2, the H/V timer counters, interrupt lines in
 * both directions, and a small SA-1 program taking timer IRQs.
 *
 * Expected values follow ares's SA-1 (the reference implementation the
 * runtime ports) and the SA-1 documentation; the absindx hardware tests
 * (roms/snes/absindx) cover RAM protection and reboot on real hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes.h"
#include "snes_bus.h"
#include "snes_recomp.h"

static int failures;
static int test_failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            test_failures++;                              \
        }                                                 \
    } while (0)

#define ROM_SIZE 0x400000u

/* The SA-1 test program at 00:8100 (ROM offset $0100), native mode:
 * opens I-RAM for writing, arms the timer IRQ once per line (HCNT = 0),
 * then sleeps; the handler counts into I-RAM $0010 and acknowledges. */
static const uint8_t sa1_program[] = {
    0x18,             /* 8100 CLC            */
    0xFB,             /* 8101 XCE            */
    0xA9, 0xFF,       /* 8102 LDA #$FF       */
    0x8D, 0x2A, 0x22, /* 8104 STA $222A CIWP */
    0xA9, 0x00,       /* 8107 LDA #$00       */
    0x8D, 0x12, 0x22, /* 8109 STA $2212 HCNT */
    0x8D, 0x13, 0x22, /* 810C STA $2213      */
    0xA9, 0x40,       /* 810F LDA #$40       */
    0x8D, 0x0A, 0x22, /* 8111 STA $220A CIE  */
    0xA9, 0x01,       /* 8114 LDA #$01       */
    0x8D, 0x10, 0x22, /* 8116 STA $2210 TMC  */
    0x58,             /* 8119 CLI            */
    0xCB,             /* 811A WAI            */
    0x80, 0xFD,       /* 811B BRA $811A      */
    0xEE, 0x10, 0x00, /* 811D INC $0010      (IRQ handler) */
    0xA9, 0x40,       /* 8120 LDA #$40       */
    0x8D, 0x0B, 0x22, /* 8122 STA $220B CIC  */
    0x40,             /* 8125 RTI            */
};
#define SA1_RESET 0x8100u
#define SA1_IRQ 0x811Du

static uint8_t *make_rom(void)
{
    uint8_t *rom = malloc(ROM_SIZE);
    if (!rom) return NULL;
    memset(rom, 0xFF, ROM_SIZE);
    /* A marker per 1 MiB region, at the same offset in each. */
    for (uint32_t region = 0; region < 4; region++) rom[region * 0x100000u + 0x1234u] = (uint8_t)(0xA0u + region);
    /* S-CPU: SEI; BRA * at the reset vector (never run here). */
    rom[0] = 0x78;
    rom[1] = 0x80;
    rom[2] = 0xFE;
    memcpy(rom + 0x100, sa1_program, sizeof sa1_program);
    /* Bit-reader data at 00:A000. */
    rom[0x2000] = 0xB4;
    rom[0x2001] = 0x6A;
    rom[0x2002] = 0x5C;
    rom[0x2003] = 0x12;
    /* LoROM header: map mode $23 (SA-1), type $35 (SA-1, RAM, battery),
     * 4 MiB ROM, 32 KiB BW-RAM. */
    uint8_t *h = rom + 0x7FC0;
    memcpy(h, "SA1 UNIT TEST        ", 21);
    h[0x15] = 0x23;
    h[0x16] = 0x35;
    h[0x17] = 0x0C;
    h[0x18] = 0x05;
    h[0x19] = 0x01;
    h[0x1C] = 0x00;
    h[0x1D] = 0x00;
    h[0x1E] = 0xFF;
    h[0x1F] = 0xFF;
    h[0x3C] = 0x00;
    h[0x3D] = 0x80;
    return rom;
}

static uint8_t *g_rom;

static SnesMachine *fresh(void)
{
    SnesMachine *m = snes_create();
    if (!m || !snes_load_rom(m, g_rom, ROM_SIZE)) {
        printf("  FAIL cannot load the synthetic SA-1 ROM\n");
        exit(1);
    }
    snes_power(m);
    return m;
}

/* S-CPU accesses without timing (DMA-style), and SA-1 bus cycles. */
static uint8_t cpur(SnesMachine *m, uint32_t addr) { return snes_bus_read_raw(m, addr, 0xEE); }
static void cpuw(SnesMachine *m, uint32_t addr, uint8_t v) { snes_bus_write_raw(m, addr, v); }
static uint8_t sa1r(SnesMachine *m, uint32_t addr) { return snes_bus_read(&m->sa1.cpu, addr); }
static void sa1w(SnesMachine *m, uint32_t addr, uint8_t v) { snes_bus_write(&m->sa1.cpu, addr, v); }

/* ---------------------------------------------------------------- tests */

static void test_cartridge(void)
{
    SnesMachine *m = fresh();
    CHECK(m->cart.mapper == SNES_MAP_SA1, "mapper %d", m->cart.mapper);
    CHECK(m->cart.coprocessor == NULL, "SA-1 is emulated, not an unsupported chip");
    CHECK(m->cart.sram_size == 0x8000u, "BW-RAM size %u", m->cart.sram_size);
    CHECK(m->sa1.present && m->sa1.sa1_resb, "the SA-1 powers up held in reset");
    snes_destroy(m);
}

static void test_super_mmc(void)
{
    SnesMachine *m = fresh();
    /* Power-on banking is linear: C0-CF = first MiB, D0 = second, ... */
    CHECK(cpur(m, 0xC01234) == 0xA0, "C0:1234 = %02X", cpur(m, 0xC01234));
    CHECK(cpur(m, 0xD01234) == 0xA1, "D0:1234 = %02X", cpur(m, 0xD01234));
    CHECK(cpur(m, 0xF01234) == 0xA3, "F0:1234 = %02X", cpur(m, 0xF01234));
    /* LoROM halves: 00:8000 = offset 0, 20:9234 = second MiB + $1234. */
    CHECK(cpur(m, 0x209234) == 0xA1, "20:9234 = %02X", cpur(m, 0x209234));
    CHECK(sa1r(m, 0xD01234) == 0xA1, "the SA-1 sees the same ROM");
    CHECK(m->sa1.mmc_linear, "power-on banking is the recompiler's");

    /* CXB = bank 3, MMC mode: C0-CF and the 00-1F halves show the fourth MiB. */
    cpuw(m, 0x002220, 0x83);
    CHECK(cpur(m, 0xC01234) == 0xA3, "C0:1234 after CXB=$83 = %02X", cpur(m, 0xC01234));
    CHECK(cpur(m, 0x009234) == 0xA3, "00:9234 after CXB=$83 = %02X", cpur(m, 0x009234));
    CHECK(sa1r(m, 0xC01234) == 0xA3, "SA-1 C0:1234 after CXB=$83");
    CHECK(!m->sa1.mmc_linear, "remapped ROM disables recompiled code");
    /* Mode bit clear: the LoROM half is linear again, C0-CF still banked. */
    cpuw(m, 0x002220, 0x03);
    CHECK(cpur(m, 0x009234) == 0xA0, "00:9234 with CXB=$03 = %02X", cpur(m, 0x009234));
    CHECK(cpur(m, 0xC01234) == 0xA3, "C0:1234 with CXB=$03 = %02X", cpur(m, 0xC01234));
    cpuw(m, 0x002220, 0x00);
    CHECK(m->sa1.mmc_linear, "banking restored");
    snes_destroy(m);
}

static void test_vector_override(void)
{
    SnesMachine *m = fresh();
    /* SIV = $1234, SNV = $5678, then SCNT enables both overrides. */
    sa1w(m, 0x00220E, 0x34);
    sa1w(m, 0x00220F, 0x12);
    sa1w(m, 0x00220C, 0x78);
    sa1w(m, 0x00220D, 0x56);
    CHECK(cpur(m, 0x00FFEE) == 0xFF, "no override yet");
    sa1w(m, 0x002209, 0x50);
    CHECK(cpur(m, 0x00FFEE) == 0x34 && cpur(m, 0x00FFEF) == 0x12, "IRQ vector override");
    CHECK(cpur(m, 0x00FFEA) == 0x78 && cpur(m, 0x00FFEB) == 0x56, "NMI vector override");
    CHECK(cpur(m, 0x00FFFC) == 0x00 && cpur(m, 0x00FFFD) == 0x80, "other vectors are ROM");
    CHECK(cpur(m, 0x80FFEE) == 0xFF, "only bank 00 is overridden");
    CHECK((cpur(m, 0x002300) & 0x50) == 0x50, "SFR shows both switches");
    sa1w(m, 0x002209, 0x00);
    CHECK(cpur(m, 0x00FFEE) == 0xFF, "override off");
    CHECK(m->page_type[0x00F] == SNES_PAGE_ROM, "vector page is plain ROM again");
    snes_destroy(m);
}

static void test_bwram(void)
{
    SnesMachine *m = fresh();
    /* Power-on: BWPA = $0F protects everything while both write enables are off. */
    cpuw(m, 0x400100, 0x11);
    CHECK(m->sram[0x100] == 0xFF, "S-CPU write to protected BW-RAM landed");
    /* BWPA = 2 protects $000-$3FF. */
    cpuw(m, 0x002228, 0x02);
    cpuw(m, 0x400100, 0x11);
    cpuw(m, 0x400400, 0x22);
    CHECK(m->sram[0x100] == 0xFF && m->sram[0x400] == 0x22, "BWPA=2: %02X %02X", m->sram[0x100], m->sram[0x400]);
    /* Either write enable lifts protection for both CPUs. */
    sa1w(m, 0x002227, 0x80); /* CWEN */
    cpuw(m, 0x400100, 0x11);
    CHECK(m->sram[0x100] == 0x11, "CWEN lifts S-CPU protection");
    /* S-CPU window: BMAPS = 2 puts BW-RAM $4000 at 00:6000. */
    cpuw(m, 0x002224, 0x02);
    cpuw(m, 0x006005, 0x5A);
    CHECK(m->sram[0x4005] == 0x5A, "BMAPS window write");
    CHECK(cpur(m, 0x806005) == 0x5A, "window is mirrored in bank 80");
    /* SA-1 window: BMAP = 1 puts BW-RAM $2000 at 00:6000. */
    sa1w(m, 0x002225, 0x01);
    sa1w(m, 0x006003, 0x77);
    CHECK(m->sram[0x2003] == 0x77, "BMAP window write");
    CHECK(sa1r(m, 0x402003) == 0x77, "linear view");
    CHECK(cpur(m, 0x482003) == 0x77, "S-CPU BW-RAM mirrors every 32 KiB");
    snes_destroy(m);
}

static void test_bitmap(void)
{
    SnesMachine *m = fresh();
    sa1w(m, 0x002227, 0x80);
    /* 4 bpp: two pixels per byte, low nibble first. */
    sa1w(m, 0x600000, 0x0A);
    sa1w(m, 0x600001, 0x05);
    CHECK(m->sram[0] == 0x5A, "4bpp pack = %02X", m->sram[0]);
    CHECK(sa1r(m, 0x600001) == 0x05, "4bpp read back");
    /* 2 bpp (BBF): four pixels per byte. */
    sa1w(m, 0x00223F, 0x80);
    sa1w(m, 0x600004, 3);
    sa1w(m, 0x600005, 1);
    sa1w(m, 0x600006, 2);
    sa1w(m, 0x600007, 0);
    CHECK(m->sram[1] == 0x27, "2bpp pack = %02X", m->sram[1]);
    CHECK(sa1r(m, 0x600006) == 2, "2bpp read back");
    /* BMAP with bit 7: the $6000 window shows the bitmap view. */
    sa1w(m, 0x002225, 0x80);
    CHECK(sa1r(m, 0x006005) == 1, "bitmap window");
    snes_destroy(m);
}

static void test_iram(void)
{
    SnesMachine *m = fresh();
    /* Power-on: neither CPU may write I-RAM. */
    cpuw(m, 0x003000, 0x12);
    sa1w(m, 0x000000, 0x34);
    CHECK(m->sa1.iram[0] == 0x00, "I-RAM protected at power-on");
    /* SIWP bit n opens page n ($3000 + n * $100) to the S-CPU. */
    cpuw(m, 0x002229, 0x01);
    cpuw(m, 0x003000, 0x12);
    cpuw(m, 0x003100, 0x56);
    CHECK(m->sa1.iram[0] == 0x12 && m->sa1.iram[0x100] == 0x00, "SIWP page protection");
    /* CIWP for the SA-1, which sees I-RAM at $0000 and $3000. */
    sa1w(m, 0x00222A, 0x02);
    sa1w(m, 0x000100, 0x9A);
    sa1w(m, 0x003001, 0xBC);
    CHECK(m->sa1.iram[0x100] == 0x9A && m->sa1.iram[1] == 0x00, "CIWP page protection");
    CHECK(sa1r(m, 0x803100) == 0x9A && cpur(m, 0x003100) == 0x9A, "both CPUs read I-RAM");
    snes_destroy(m);
}

static uint64_t mr(SnesMachine *m)
{
    uint64_t v = 0;
    for (int i = 4; i >= 0; i--) v = v << 8 | sa1r(m, 0x002306u + (uint32_t)i);
    return v;
}

static void set_ab(SnesMachine *m, uint16_t a, uint16_t b)
{
    sa1w(m, 0x002251, (uint8_t)a);
    sa1w(m, 0x002252, (uint8_t)(a >> 8));
    sa1w(m, 0x002253, (uint8_t)b);
    sa1w(m, 0x002254, (uint8_t)(b >> 8)); /* starts the operation */
}

static void test_arithmetic(void)
{
    SnesMachine *m = fresh();
    sa1w(m, 0x002250, 0x00); /* signed multiply */
    set_ab(m, 0xFFFD, 1000);
    CHECK(mr(m) == 0xFFFFF448ull, "-3 * 1000 = %010llX", (unsigned long long)mr(m));
    sa1w(m, 0x002250, 0x01); /* signed dividend / unsigned divisor */
    set_ab(m, 0xFFF9, 2);
    CHECK(mr(m) == 0x0001FFFCull, "-7 / 2 = %010llX (remainder 1, quotient -4)", (unsigned long long)mr(m));
    set_ab(m, 1234, 0);
    CHECK(mr(m) == 0, "division by zero");
    sa1w(m, 0x002250, 0x02); /* cumulative sum, cleared by the MCNT write */
    for (int i = 0; i < 3; i++) set_ab(m, 0x7FFF, 0x7FFF);
    CHECK(mr(m) == 3ull * 0x3FFF0001ull, "sigma = %010llX", (unsigned long long)mr(m));
    CHECK(sa1r(m, 0x00230B) == 0x00, "no overflow");
    sa1w(m, 0x002250, 0x02);
    set_ab(m, 0xFFFF, 1);
    CHECK(mr(m) == 0xFFFFFFFFFFull && sa1r(m, 0x00230B) == 0x80, "sigma below zero wraps 40 bits, OF set");
    snes_destroy(m);
}

static void test_bit_reader(void)
{
    SnesMachine *m = fresh();
    sa1w(m, 0x002259, 0x00);
    sa1w(m, 0x00225A, 0xA0);
    sa1w(m, 0x00225B, 0x00); /* VDA = 00:A000: B4 6A 5C 12 */
    CHECK(sa1r(m, 0x00230C) == 0xB4 && sa1r(m, 0x00230D) == 0x6A, "bits 0-15");
    sa1w(m, 0x002258, 0x04); /* fixed mode: advance 4 bits now */
    CHECK(sa1r(m, 0x00230C) == 0xAB && sa1r(m, 0x00230D) == 0xC6, "bits 4-19");
    sa1w(m, 0x002258, 0x83); /* auto mode: advance 3 bits after each VDPH read */
    CHECK(sa1r(m, 0x00230D) == 0xC6, "VDPH before the advance");
    CHECK(sa1r(m, 0x00230C) == 0xD5, "bits 7-14 = %02X", sa1r(m, 0x00230C));
    sa1w(m, 0x002258, 0x80); /* 0 means 16 */
    (void)sa1r(m, 0x00230D);
    CHECK(sa1r(m, 0x00230C) == 0x24, "bits 23-30 = %02X", sa1r(m, 0x00230C));
    snes_destroy(m);
}

static void test_dma_normal(void)
{
    SnesMachine *m = fresh();
    sa1w(m, 0x00220A, 0x20); /* DMA IRQ enable */
    sa1w(m, 0x002230, 0x80); /* DMA on: ROM -> I-RAM */
    sa1w(m, 0x002232, 0x00);
    sa1w(m, 0x002233, 0xA0);
    sa1w(m, 0x002234, 0x00);
    sa1w(m, 0x002238, 4);
    sa1w(m, 0x002239, 0);
    const uint64_t before = m->sa1.cpu.cycles;
    sa1w(m, 0x002235, 0x00);
    sa1w(m, 0x002236, 0x02); /* starts the transfer */
    CHECK(memcmp(&m->sa1.iram[0x200], "\xB4\x6A\x5C\x12", 4) == 0, "ROM -> I-RAM copy");
    CHECK(m->sa1.cpu.cycles - before >= 4u * 2u + 4u, "the transfer takes SA-1 time");
    CHECK(sa1r(m, 0x002301) & 0x20, "DMA IRQ flag in CFR");
    /* I-RAM -> BW-RAM, started by the high DDA byte. */
    sa1w(m, 0x002227, 0x80);
    sa1w(m, 0x002230, 0x86); /* source I-RAM, destination BW-RAM */
    sa1w(m, 0x002232, 0x00);
    sa1w(m, 0x002233, 0x02);
    sa1w(m, 0x002238, 4);
    sa1w(m, 0x002239, 0); /* the first transfer left DTC at $FFFF */
    sa1w(m, 0x002235, 0x40);
    sa1w(m, 0x002236, 0x00);
    CHECK(m->sram[0x40] == 0xFF, "a BW-RAM destination waits for the bank byte");
    sa1w(m, 0x002237, 0x00);
    CHECK(memcmp(&m->sram[0x40], "\xB4\x6A\x5C\x12", 4) == 0, "I-RAM -> BW-RAM copy");
    snes_destroy(m);
}

static void test_char_conversion(void)
{
    SnesMachine *m = fresh();
    /* Type 2: the SA-1 writes 8 pixels to BRF; they land as one planar line. */
    sa1w(m, 0x002230, 0xA0); /* DMA on, character conversion, type 2 */
    sa1w(m, 0x002231, 0x02); /* 2 bpp */
    sa1w(m, 0x002235, 0x00);
    sa1w(m, 0x002236, 0x01); /* DDA = I-RAM $0100 */
    static const uint8_t px[8] = { 0, 1, 2, 3, 0, 1, 2, 3 };
    for (int i = 0; i < 8; i++) sa1w(m, 0x002240u + (uint32_t)i, px[i]);
    CHECK(m->sa1.iram[0x100] == 0x55 && m->sa1.iram[0x101] == 0x33, "type 2 line 0 = %02X %02X",
          m->sa1.iram[0x100], m->sa1.iram[0x101]);
    for (int i = 0; i < 8; i++) sa1w(m, 0x002248u + (uint32_t)i, px[7 - i]);
    CHECK(m->sa1.iram[0x102] == 0xAA && m->sa1.iram[0x103] == 0xCC, "type 2 line 1 = %02X %02X",
          m->sa1.iram[0x102], m->sa1.iram[0x103]);

    /* Type 1: the S-CPU reads a linear 2 bpp bitmap in BW-RAM as characters. */
    sa1w(m, 0x002227, 0x80);
    for (uint32_t i = 0; i < 16; i++) sa1w(m, 0x401000u + i, 0x39); /* pixels 1 2 3 0 1 2 3 0 */
    cpuw(m, 0x002201, 0x20); /* character conversion IRQ to the S-CPU */
    sa1w(m, 0x002230, 0xB0); /* DMA on, character conversion, type 1 */
    sa1w(m, 0x002231, 0x02); /* 2 bpp, one character wide */
    sa1w(m, 0x002232, 0x00);
    sa1w(m, 0x002233, 0x10);
    sa1w(m, 0x002234, 0x00); /* source BW-RAM $1000 */
    sa1w(m, 0x002235, 0x00);
    sa1w(m, 0x002236, 0x03); /* destination I-RAM $0300: starts */
    CHECK(m->coproc_irq, "type 1 raises the S-CPU IRQ");
    CHECK(cpur(m, 0x002300) & 0x20, "SFR character conversion flag");
    CHECK(cpur(m, 0x007000) == 0xAA && cpur(m, 0x007001) == 0x66, "converted bytes through the window");
    CHECK(m->sa1.iram[0x30E] == 0xAA && m->sa1.iram[0x30F] == 0x66, "the whole character is in I-RAM");
    cpuw(m, 0x002231, 0x80); /* CHDEND */
    CHECK(cpur(m, 0x007000) == 0x39, "plain BW-RAM again after CHDEND");
    snes_destroy(m);
}

static void test_interrupt_lines(void)
{
    SnesMachine *m = fresh();
    /* SA-1 -> S-CPU: SCNT bit 7, gated by SIE, cleared by SIC. */
    sa1w(m, 0x002209, 0x80);
    CHECK(!m->coproc_irq && (cpur(m, 0x002300) & 0x80), "flag without enable");
    cpuw(m, 0x002201, 0x80); /* enabling with the flag up asserts the line */
    CHECK(m->coproc_irq && m->irq_since != UINT64_MAX, "S-CPU IRQ line");
    cpuw(m, 0x002202, 0x80);
    CHECK(!m->coproc_irq && !(cpur(m, 0x002300) & 0x80), "SIC clears");
    /* S-CPU -> SA-1: CCNT bit 7 sets the SA-1 IRQ flag. */
    cpuw(m, 0x002200, 0xA0); /* still in reset */
    CHECK(sa1r(m, 0x002301) & 0x80, "SA-1 IRQ flag in CFR");
    sa1w(m, 0x00220B, 0x80);
    CHECK(!(sa1r(m, 0x002301) & 0x80), "CIC clears");
    snes_destroy(m);
}

static void test_timer_counters(void)
{
    SnesMachine *m = fresh();
    SnesCpu *c = &m->sa1.cpu;
    sa1w(m, 0x002211, 0); /* restart at 0 */
    c->cycles += 1364u * 3u + 400u;
    CHECK(sa1r(m, 0x002302) == 100 && sa1r(m, 0x002303) == 0, "HCR = %d dots", m->sa1.hcr);
    CHECK(sa1r(m, 0x002304) == 3 && sa1r(m, 0x002305) == 0, "VCR = %d lines", m->sa1.vcr);
    /* Linear mode: one 20-bit counter, H is the low 11 bits. */
    sa1w(m, 0x002210, 0x80);
    sa1w(m, 0x002211, 0);
    c->cycles += 2048u * 5u + 398u;
    CHECK(sa1r(m, 0x002302) == 100 && sa1r(m, 0x002304) == 5, "linear HCR %d VCR %d", m->sa1.hcr, m->sa1.vcr);
    snes_destroy(m);
}

/* The SA-1 released from reset runs the program at CRV and takes one timer
 * IRQ per line. */
static void test_program_timer_irq(void)
{
    SnesMachine *m = fresh();
    cpuw(m, 0x002203, SA1_RESET & 0xFFu);
    cpuw(m, 0x002204, SA1_RESET >> 8);
    cpuw(m, 0x002207, SA1_IRQ & 0xFFu);
    cpuw(m, 0x002208, SA1_IRQ >> 8);
    cpuw(m, 0x002200, 0x00); /* release */
    CHECK(m->sa1.cpu.pc == SA1_RESET && m->sa1.cpu.flag_e, "reset at CRV in emulation mode");
    const uint64_t start = m->cpu.cycles;
    m->cpu.cycles = start + 1364u * 100u;
    snes_sa1_run(m, m->cpu.cycles);
    const unsigned count = m->sa1.iram[0x10];
    CHECK(count >= 99 && count <= 101, "timer IRQs in 100 lines: %u", count);
    CHECK(m->sa1.cpu.wai && !m->sa1.cpu.flag_e, "sleeping in native mode between IRQs");
    CHECK(m->sa1.cpu.cycles >= m->cpu.cycles, "caught up to the S-CPU");
    /* Holding RDYB stops it; releasing resumes. */
    cpuw(m, 0x002200, 0x40);
    m->cpu.cycles += 1364u * 10u;
    snes_sa1_run(m, m->cpu.cycles);
    CHECK(m->sa1.iram[0x10] == count, "no IRQs while held");
    cpuw(m, 0x002200, 0x00);
    snes_destroy(m);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} Test;

static const Test tests[] = {
    { "cartridge", test_cartridge },
    { "super_mmc", test_super_mmc },
    { "vector_override", test_vector_override },
    { "bwram", test_bwram },
    { "bitmap", test_bitmap },
    { "iram", test_iram },
    { "arithmetic", test_arithmetic },
    { "bit_reader", test_bit_reader },
    { "dma_normal", test_dma_normal },
    { "char_conversion", test_char_conversion },
    { "interrupt_lines", test_interrupt_lines },
    { "timer_counters", test_timer_counters },
    { "program_timer_irq", test_program_timer_irq },
};

int main(void)
{
    g_rom = make_rom();
    if (!g_rom) return 2;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        test_failures = 0;
        tests[i].fn();
        printf("%s %s\n", test_failures ? "FAIL" : "PASS", tests[i].name);
        failures += test_failures;
    }
    free(g_rom);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
