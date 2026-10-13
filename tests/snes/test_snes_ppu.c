/*
 * SNES PPU unit tests: register semantics (VRAM/CGRAM/OAM ports, write-twice
 * latches, multiplier, counter latch, open bus) and rendering of small
 * scenes built directly in VRAM/CGRAM/OAM, checked pixel by pixel.
 *
 * Each test is named; the program prints PASS/FAIL per test and exits
 * non-zero if any check failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_ppu.h"

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

/* The PPU is ~2.5 MB; keep it out of the stack. */
static SnesPpu ppu_storage;

/* ------------------------------------------------------------- helpers */

static SnesPpu *fresh(void)
{
    SnesPpu *p = &ppu_storage;
    snes_ppu_power(p);
    p->vblank = true; /* registers are written in vblank unless a test says otherwise */
    p->vcounter = 230;
    p->hcounter = 0;
    return p;
}

static void w(SnesPpu *p, uint8_t reg, uint8_t v) { snes_ppu_write(p, reg, v); }
static uint8_t r(SnesPpu *p, uint8_t reg) { return snes_ppu_read(p, reg, 0xEE); }

static uint16_t vram_word(const SnesPpu *p, uint16_t addr)
{
    const uint8_t *v = snes_ppu_vram(p);
    uint32_t a = (uint32_t)(addr & 0x7FFFu) * 2u;
    return (uint16_t)(v[a] | (v[a + 1] << 8));
}

static uint16_t cgram_word(const SnesPpu *p, uint8_t index)
{
    const uint8_t *c = snes_ppu_cgram(p);
    return (uint16_t)(c[index * 2] | (c[index * 2 + 1] << 8));
}

/* Word writes through the port: VMAIN = increment after the high byte. */
static void vram_set(SnesPpu *p, uint16_t addr, uint16_t value)
{
    w(p, 0x15, 0x80);
    w(p, 0x16, (uint8_t)addr);
    w(p, 0x17, (uint8_t)(addr >> 8));
    w(p, 0x18, (uint8_t)value);
    w(p, 0x19, (uint8_t)(value >> 8));
}

static void cgram_set(SnesPpu *p, uint8_t index, uint16_t color)
{
    w(p, 0x21, index);
    w(p, 0x22, (uint8_t)color);
    w(p, 0x22, (uint8_t)(color >> 8));
}

static void scroll(SnesPpu *p, uint8_t reg, uint16_t value)
{
    w(p, reg, (uint8_t)value);
    w(p, reg, (uint8_t)(value >> 8));
}

/* The documented BGR555 -> XRGB8888 conversion at full brightness. */
static uint32_t xrgb(uint16_t c)
{
    uint32_t r5 = c & 31u, g5 = (c >> 5) & 31u, b5 = (c >> 10) & 31u;
    uint32_t r8 = (r5 << 3) | (r5 >> 2), g8 = (g5 << 3) | (g5 >> 2), b8 = (b5 << 3) | (b5 >> 2);
    return (r8 << 16) | (g8 << 8) | b8;
}

static uint16_t rgb5(unsigned r, unsigned g, unsigned b) { return (uint16_t)(r | (g << 5) | (b << 10)); }

/* Render picture line y as the machine would, then return to vblank. */
static void render(SnesPpu *p, int y)
{
    p->vblank = false;
    p->vcounter = (uint16_t)y;
    p->hcounter = 274;
    snes_ppu_render_line(p, y);
    p->vblank = true;
    p->vcounter = 230;
    p->hcounter = 0;
}

static uint32_t px(const SnesPpu *p, int x, int row) { return p->framebuffer[row * SNES_FB_WIDTH + x]; }

/* A 4bpp tile whose every row is `lo` (planes 0/1) and `hi` (planes 2/3). */
static void tile4(SnesPpu *p, uint16_t base, unsigned tile, uint16_t lo, uint16_t hi)
{
    for (unsigned row = 0; row < 8; row++) {
        vram_set(p, (uint16_t)(base + tile * 16u + row), lo);
        vram_set(p, (uint16_t)(base + tile * 16u + 8u + row), hi);
    }
}

/* A 4bpp tile filled with one color. */
static void tile4_solid(SnesPpu *p, uint16_t base, unsigned tile, unsigned color)
{
    uint16_t lo = (uint16_t)(((color & 1u) ? 0x00FFu : 0u) | ((color & 2u) ? 0xFF00u : 0u));
    uint16_t hi = (uint16_t)(((color & 4u) ? 0x00FFu : 0u) | ((color & 8u) ? 0xFF00u : 0u));
    tile4(p, base, tile, lo, hi);
}

/* Mode 1, BG1: map at word $0400, characters at $1000, main screen on,
 * display on at full brightness. */
static void mode1_bg1(SnesPpu *p)
{
    w(p, 0x05, 0x01);
    w(p, 0x07, 0x04); /* BG1SC: map $0400, 32x32 */
    w(p, 0x0B, 0x01); /* BG12NBA: BG1 chars $1000 */
    w(p, 0x2C, 0x01); /* TM: BG1 */
    scroll(p, 0x0D, 0);
    scroll(p, 0x0E, 0);
    w(p, 0x00, 0x0F);
}

static void map_set(SnesPpu *p, uint16_t map, unsigned tx, unsigned ty, uint16_t entry)
{
    vram_set(p, (uint16_t)(map + ty * 32u + tx), entry);
}

/* ------------------------------------------------------ register tests */

static void test_vram_increment_and_remap(void)
{
    SnesPpu *p = fresh();

    /* Increment after the high byte, step 1. */
    w(p, 0x15, 0x80);
    w(p, 0x16, 0x00); w(p, 0x17, 0x10);
    w(p, 0x18, 0x11); w(p, 0x19, 0x22);
    w(p, 0x18, 0x33); w(p, 0x19, 0x44);
    CHECK(vram_word(p, 0x1000) == 0x2211 && vram_word(p, 0x1001) == 0x4433,
          "step 1 high-byte increment: %04X %04X", vram_word(p, 0x1000), vram_word(p, 0x1001));

    /* Increment after the low byte: consecutive low-byte writes. */
    w(p, 0x15, 0x00);
    w(p, 0x16, 0x00); w(p, 0x17, 0x20);
    w(p, 0x18, 0xA1); w(p, 0x18, 0xA2);
    CHECK(vram_word(p, 0x2000) == 0x00A1 && vram_word(p, 0x2001) == 0x00A2,
          "low-byte increment: %04X %04X", vram_word(p, 0x2000), vram_word(p, 0x2001));
    /* A high-byte write does not move the address in this mode. */
    w(p, 0x19, 0xB1); w(p, 0x19, 0xB2);
    CHECK(vram_word(p, 0x2002) == 0xB200, "high byte in low-increment mode: %04X", vram_word(p, 0x2002));

    /* Steps 32 and 128 (both codes 2 and 3). */
    w(p, 0x15, 0x81);
    w(p, 0x16, 0x00); w(p, 0x17, 0x30);
    w(p, 0x18, 0x01); w(p, 0x19, 0x00); w(p, 0x18, 0x02); w(p, 0x19, 0x00);
    CHECK(vram_word(p, 0x3000) == 1 && vram_word(p, 0x3020) == 2, "step 32");
    w(p, 0x15, 0x82);
    w(p, 0x16, 0x00); w(p, 0x17, 0x31);
    w(p, 0x18, 0x03); w(p, 0x19, 0x00); w(p, 0x18, 0x04); w(p, 0x19, 0x00);
    CHECK(vram_word(p, 0x3100) == 3 && vram_word(p, 0x3180) == 4, "step 128 (code 2)");
    w(p, 0x15, 0x83);
    w(p, 0x16, 0x00); w(p, 0x17, 0x32);
    w(p, 0x18, 0x05); w(p, 0x19, 0x00); w(p, 0x18, 0x06); w(p, 0x19, 0x00);
    CHECK(vram_word(p, 0x3200) == 5 && vram_word(p, 0x3280) == 6, "step 128 (code 3)");

    /* Remap 1: aaaaaaaaYYYxxxxx -> aaaaaaaaxxxxxYYY. */
    w(p, 0x15, 0x84);
    w(p, 0x16, 0x21); w(p, 0x17, 0x40); /* a = $4021 -> $4009 */
    w(p, 0x18, 0x5A); w(p, 0x19, 0x00);
    CHECK(vram_word(p, 0x4009) == 0x005A, "remap 1: $4021 -> $4009 holds %04X", vram_word(p, 0x4009));
    w(p, 0x18, 0x5B); w(p, 0x19, 0x00); /* a = $4022 -> $4011 */
    CHECK(vram_word(p, 0x4011) == 0x005B, "remap 1: $4022 -> $4011 holds %04X", vram_word(p, 0x4011));
    /* Remap 2: 9-bit rotate. */
    w(p, 0x15, 0x88);
    w(p, 0x16, 0x41); w(p, 0x17, 0x50); /* $5041 -> $5009 */
    w(p, 0x18, 0x6A); w(p, 0x19, 0x00);
    CHECK(vram_word(p, 0x5009) == 0x006A, "remap 2: $5041 -> $5009 holds %04X", vram_word(p, 0x5009));
    /* Remap 3: 10-bit rotate. */
    w(p, 0x15, 0x8C);
    w(p, 0x16, 0x81); w(p, 0x17, 0x60); /* $6081 -> $6009 */
    w(p, 0x18, 0x7A); w(p, 0x19, 0x00);
    CHECK(vram_word(p, 0x6009) == 0x007A, "remap 3: $6081 -> $6009 holds %04X", vram_word(p, 0x6009));

    /* Word addresses wrap at 32K. */
    vram_set(p, 0x8005, 0xBEEF);
    CHECK(vram_word(p, 0x0005) == 0xBEEF, "$8005 did not mirror to $0005");
}

static void test_vram_write_blocked_active_display(void)
{
    SnesPpu *p = fresh();
    vram_set(p, 0x0100, 0x1111);
    vram_set(p, 0x0101, 0x2222);
    w(p, 0x00, 0x0F); /* display on */

    /* Active display: writes are dropped but the address still steps. */
    p->vblank = false;
    p->vcounter = 100;
    w(p, 0x15, 0x80);
    w(p, 0x16, 0x00); w(p, 0x17, 0x01);
    w(p, 0x18, 0xAA); w(p, 0x19, 0xBB);
    CHECK(vram_word(p, 0x0100) == 0x1111, "active-display write landed: %04X", vram_word(p, 0x0100));
    /* Reads during active display return 0. */
    w(p, 0x16, 0x01); w(p, 0x17, 0x01);
    CHECK(r(p, 0x39) == 0x00 && r(p, 0x3A) == 0x00, "active-display VRAM read not zero");

    /* The address advanced past $0100: the next vblank write goes to $0102
     * (the last access set $0101 and incremented once). */
    w(p, 0x16, 0x00); w(p, 0x17, 0x01);
    w(p, 0x18, 0xAA); w(p, 0x19, 0xBB); /* dropped, -> $0101 */
    p->vblank = true;
    p->vcounter = 230;
    w(p, 0x18, 0xCC); w(p, 0x19, 0xDD);
    CHECK(vram_word(p, 0x0101) == 0xDDCC, "address did not step during the dropped write: $0101 = %04X",
          vram_word(p, 0x0101));

    /* Forced blank opens VRAM during the picture. */
    p->vblank = false;
    p->vcounter = 100;
    w(p, 0x00, 0x8F);
    w(p, 0x16, 0x00); w(p, 0x17, 0x01);
    w(p, 0x18, 0x34); w(p, 0x19, 0x12);
    CHECK(vram_word(p, 0x0100) == 0x1234, "forced-blank write dropped: %04X", vram_word(p, 0x0100));
}

static void test_vram_read_prefetch(void)
{
    SnesPpu *p = fresh();
    vram_set(p, 0x0200, 0x1234);
    vram_set(p, 0x0201, 0x5678);

    /* Setting the address prefetches; a read returns the old prefetch, then
     * refills it from the OLD address and increments (fullsnes: "the first
     * word will be received twice"). Increment on high byte: */
    w(p, 0x15, 0x80);
    w(p, 0x16, 0x00); w(p, 0x17, 0x02);
    CHECK(r(p, 0x39) == 0x34, "prefetch low");
    CHECK(r(p, 0x39) == 0x34, "low read advanced in high-increment mode");
    CHECK(r(p, 0x3A) == 0x12, "prefetch high");
    uint8_t lo = r(p, 0x39), hi = r(p, 0x3A);
    CHECK(lo == 0x34 && hi == 0x12, "first word not received twice: %02X%02X", hi, lo);
    lo = r(p, 0x39); hi = r(p, 0x3A);
    CHECK(lo == 0x78 && hi == 0x56, "third read should be the next word: %02X%02X", hi, lo);

    /* Increment on low byte: the high byte still belongs to the first word. */
    w(p, 0x15, 0x00);
    w(p, 0x16, 0x00); w(p, 0x17, 0x02);
    lo = r(p, 0x39); hi = r(p, 0x3A);
    CHECK(lo == 0x34 && hi == 0x12, "low-increment first word %02X%02X", hi, lo);
    lo = r(p, 0x39); hi = r(p, 0x3A);
    CHECK(lo == 0x34 && hi == 0x56, "low-increment second read %02X%02X, expected 5634", hi, lo);

    /* Writes do not touch the prefetch. */
    w(p, 0x15, 0x80);
    w(p, 0x16, 0x00); w(p, 0x17, 0x02);     /* prefetch = $1234 */
    w(p, 0x18, 0xEF); w(p, 0x19, 0xBE);     /* $0200 := $BEEF, address -> $0201 */
    lo = r(p, 0x39); hi = r(p, 0x3A);
    CHECK(lo == 0x34 && hi == 0x12, "prefetch refreshed by a write: %02X%02X", hi, lo);
    CHECK(vram_word(p, 0x0200) == 0xBEEF, "write after prefetch");
    /* Re-addressing refetches. */
    w(p, 0x16, 0x00); w(p, 0x17, 0x02);
    lo = r(p, 0x39); hi = r(p, 0x3A);
    CHECK(lo == 0xEF && hi == 0xBE, "prefetch after re-addressing: %02X%02X", hi, lo);
}

static void test_cgram_latch_format_xrgb(void)
{
    SnesPpu *p = fresh();

    cgram_set(p, 5, 0xFFFF);
    CHECK(cgram_word(p, 5) == 0x7FFF, "bit 15 kept: %04X", cgram_word(p, 5));

    /* One write only fills the latch. */
    w(p, 0x21, 6);
    w(p, 0x22, 0x12);
    CHECK(cgram_word(p, 6) == 0x0000, "first write committed");
    w(p, 0x22, 0x34);
    CHECK(cgram_word(p, 6) == 0x3412, "pair committed as %04X", cgram_word(p, 6));
    w(p, 0x22, 0x56); w(p, 0x22, 0x78); /* address auto-increments */
    CHECK(cgram_word(p, 7) == 0x7856, "auto-increment: %04X", cgram_word(p, 7));
    /* CGADD resets the latch phase. */
    w(p, 0x21, 8); w(p, 0x22, 0x99); w(p, 0x21, 8); w(p, 0x22, 0x11); w(p, 0x22, 0x22);
    CHECK(cgram_word(p, 8) == 0x2211, "CGADD did not reset the phase: %04X", cgram_word(p, 8));

    /* Reads alternate low/high; bit 7 of the high read is PPU2 open bus. */
    w(p, 0x21, 6);
    CHECK(r(p, 0x3B) == 0x12, "read low");
    CHECK(r(p, 0x3B) == 0x34, "read high (open bus bit 7 clear)");
    w(p, 0x21, 5);
    CHECK(r(p, 0x3B) == 0xFF, "read low of $7FFF");
    CHECK(r(p, 0x3B) == 0xFF, "read high of $7FFF with open bus bit 7 set");

    /* Exact XRGB conversion via the backdrop. */
    static const uint16_t colors[] = { 0x0000, 0x7FFF, 0x0421, 0x401F, 0x03E0, 0x2D6B };
    static const uint32_t expect[] = { 0x000000, 0xFFFFFF, 0x080808, 0xFF0084, 0x00FF00, 0x5A5A5A };
    w(p, 0x00, 0x0F);
    for (unsigned i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        cgram_set(p, 0, colors[i]);
        render(p, 10);
        CHECK(px(p, 0, 9) == expect[i] && px(p, 255, 9) == expect[i], "color %04X -> %06X, expected %06X",
              colors[i], px(p, 0, 9), expect[i]);
        CHECK(expect[i] == xrgb(colors[i]), "test table disagrees with the formula for %04X", colors[i]);
    }

    /* Brightness scales the 5-bit channels by (N + 1) / 16; 0 is black. */
    cgram_set(p, 0, 0x7FFF);
    w(p, 0x00, 0x07);
    render(p, 10);
    CHECK(px(p, 0, 9) == 0x7B7B7B, "brightness 7: %06X", px(p, 0, 9));
    w(p, 0x00, 0x00);
    render(p, 10);
    CHECK(px(p, 0, 9) == 0x000000, "brightness 0 not black: %06X", px(p, 0, 9));
    w(p, 0x00, 0x8F);
    render(p, 10);
    CHECK(px(p, 0, 9) == 0x000000, "forced blank not black: %06X", px(p, 0, 9));
}

/* Sprite 0 scene: 8x8/16x16 sizes, OBJ characters at word 0. */
static void sprite_scene(SnesPpu *p)
{
    w(p, 0x01, 0x00); /* OBSEL: 8x8 / 16x16, base 0 */
    for (unsigned t = 0; t < 32; t++) tile4_solid(p, 0x0000, t, 0);
    tile4_solid(p, 0x0000, 1, 1);
    tile4_solid(p, 0x0000, 2, 1);
    tile4_solid(p, 0x0000, 17, 1);
    tile4_solid(p, 0x0000, 18, 1);
    cgram_set(p, 0, rgb5(0, 0, 8));
    cgram_set(p, 129, rgb5(31, 0, 0));
    w(p, 0x2C, 0x10); /* TM: OBJ */
    /* Park all sprites off-screen at Y = $F0 (8x8 and 16x16 stay below line 224). */
    w(p, 0x02, 0); w(p, 0x03, 0);
    for (unsigned n = 0; n < 128; n++) { w(p, 0x04, 0); w(p, 0x04, 0xF0); w(p, 0x04, 0); w(p, 0x04, 0); }
    for (unsigned n = 0; n < 32; n++) w(p, 0x04, 0x00);
}

static void test_oam_low_high_table_sprite(void)
{
    SnesPpu *p = fresh();
    sprite_scene(p);
    const uint8_t *oam = snes_ppu_oam(p);

    /* Low table: pairs are written on the odd byte. */
    w(p, 0x02, 0x00); w(p, 0x03, 0x00);
    w(p, 0x04, 0x10);
    CHECK(oam[0] == 0x00, "even byte written before its pair");
    w(p, 0x04, 0x20);
    CHECK(oam[0] == 0x10 && oam[1] == 0x20, "pair not written: %02X %02X", oam[0], oam[1]);
    w(p, 0x04, 0x01); w(p, 0x04, 0x30);
    CHECK(oam[2] == 0x01 && oam[3] == 0x30, "second pair: %02X %02X", oam[2], oam[3]);

    /* OAM reads. */
    w(p, 0x02, 0x00); w(p, 0x03, 0x00);
    uint8_t a = r(p, 0x38), b = r(p, 0x38), c = r(p, 0x38), d = r(p, 0x38);
    CHECK(a == 0x10 && b == 0x20 && c == 0x01 && d == 0x30, "OAM read %02X %02X %02X %02X", a, b, c, d);

    /* High table: written directly, byte by byte; $220+ mirrors $200. */
    w(p, 0x02, 0x00); w(p, 0x03, 0x01);
    w(p, 0x04, 0x00);
    CHECK(oam[0x200] == 0x00, "high table byte");
    w(p, 0x02, 0x0F); w(p, 0x03, 0x01); /* byte $21E */
    w(p, 0x04, 0xA1); w(p, 0x04, 0xA2); w(p, 0x04, 0x00); w(p, 0x04, 0xA4);
    CHECK(oam[0x21E] == 0xA1 && oam[0x21F] == 0xA2, "high table tail %02X %02X", oam[0x21E], oam[0x21F]);
    CHECK(oam[0x200] == 0x00 && oam[0x201] == 0xA4, "$220/$221 did not mirror to $200/$201: %02X %02X",
          oam[0x200], oam[0x201]);
    w(p, 0x02, 0x00); w(p, 0x03, 0x01); w(p, 0x04, 0x00); w(p, 0x04, 0x00); /* clear $200-$201 */
    w(p, 0x02, 0x0F); w(p, 0x03, 0x01); w(p, 0x04, 0x00); w(p, 0x04, 0x00); /* and $21E-$21F */

    /* Rendered: sprite 0 at (16, 32), 8x8, color 1 of palette 0. OAM Y = 32
     * shows from picture line 33 = output row 32. */
    w(p, 0x00, 0x0F);
    uint32_t red = xrgb(rgb5(31, 0, 0)), back = xrgb(rgb5(0, 0, 8));
    render(p, 33);
    CHECK(px(p, 16, 32) == red && px(p, 23, 32) == red, "8x8 sprite missing: %06X %06X", px(p, 16, 32), px(p, 23, 32));
    CHECK(px(p, 15, 32) == back && px(p, 24, 32) == back, "8x8 sprite too wide: %06X %06X", px(p, 15, 32), px(p, 24, 32));
    render(p, 32);
    CHECK(px(p, 16, 31) == back, "sprite shown one line early");

    /* High table size bit: 16x16. */
    w(p, 0x02, 0x00); w(p, 0x03, 0x01); w(p, 0x04, 0x02);
    render(p, 33);
    CHECK(px(p, 24, 32) == red && px(p, 31, 32) == red && px(p, 32, 32) == back, "size bit did not make 16x16");
    render(p, 48);
    CHECK(px(p, 16, 47) == red, "16x16 bottom row missing");

    /* High table X bit 8: X = $110 is off-screen. */
    w(p, 0x02, 0x00); w(p, 0x03, 0x01); w(p, 0x04, 0x03);
    render(p, 33);
    CHECK(px(p, 16, 32) == back && px(p, 24, 32) == back, "X bit 8 ignored");

    /* X = $1F8 wraps: a 16-wide sprite covers x = 0-7. */
    w(p, 0x02, 0x00); w(p, 0x03, 0x00); w(p, 0x04, 0xF8); w(p, 0x04, 0x20);
    render(p, 33);
    CHECK(px(p, 0, 32) == red && px(p, 7, 32) == red && px(p, 8, 32) == back,
          "X = $1F8 did not wrap: %06X %06X %06X", px(p, 0, 32), px(p, 7, 32), px(p, 8, 32));
}

static void test_bgofs_double_write_latch(void)
{
    SnesPpu *p = fresh();

    w(p, 0x0D, 0x34); w(p, 0x0D, 0x01);
    CHECK(p->bg_hofs[0] == 0x0134, "BG1HOFS = %04X, expected 0134", p->bg_hofs[0]);
    w(p, 0x0E, 0x78); w(p, 0x0E, 0x02);
    CHECK(p->bg_vofs[0] == 0x0278, "BG1VOFS = %04X, expected 0278", p->bg_vofs[0]);

    /* One PPU1 latch serves all BGnxOFS: a VOFS write after a HOFS write
     * takes its low byte from the HOFS value. */
    w(p, 0x0F, 0x55);
    w(p, 0x10, 0x03);
    CHECK(p->bg_vofs[1] == 0x0355, "BG2VOFS = %04X, expected 0355 (shared latch)", p->bg_vofs[1]);

    /* HOFS takes bits 3-7 from the PPU1 latch (last write to any BGnxOFS)
     * and bits 0-2 from the PPU2 latch (last HOFS write). */
    w(p, 0x11, 0x06);         /* PPU1 = PPU2 = $06 */
    w(p, 0x12, 0xA8);         /* PPU1 = $A8 */
    w(p, 0x11, 0x01);
    CHECK(p->bg_hofs[2] == 0x01AE, "BG3HOFS = %04X, expected 01AE", p->bg_hofs[2]);

    /* BG1HOFS/VOFS also load the mode 7 offsets through the mode 7 latch,
     * which M7A-M7Y share. */
    w(p, 0x1B, 0x77);         /* mode 7 latch = $77 */
    w(p, 0x0D, 0x12);
    CHECK(p->m7_hofs == 0x1277, "M7HOFS = %04X, expected 1277", p->m7_hofs);
    w(p, 0x0E, 0x05);
    CHECK(p->m7_vofs == 0x0512, "M7VOFS = %04X, expected 0512", p->m7_vofs);
    w(p, 0x1F, 0x34); w(p, 0x1F, 0x12);
    CHECK(p->m7x == 0x1234, "M7X = %04X", p->m7x);

    /* Rendered: BG1 scrolled by 4 pixels moves a tile edge. */
    p = fresh();
    mode1_bg1(p);
    tile4_solid(p, 0x1000, 1, 1);
    cgram_set(p, 1, rgb5(31, 31, 31));
    map_set(p, 0x0400, 1, 0, 0x0001); /* tile at x = 8-15 */
    scroll(p, 0x0D, 4);
    render(p, 1);
    CHECK(px(p, 4, 0) == xrgb(0x7FFF) && px(p, 3, 0) == 0 && px(p, 11, 0) == xrgb(0x7FFF) && px(p, 12, 0) == 0,
          "HOFS 4 did not move the tile to x = 4-11");
}

static void test_m7_multiplier(void)
{
    SnesPpu *p = fresh();
    w(p, 0x1B, 0x34); w(p, 0x1B, 0x12); /* M7A = $1234 */
    w(p, 0x1C, 0xFF); w(p, 0x1C, 0x80); /* M7B high byte = -128 */
    /* $1234 * -128 = -596480 = $F6E600 (24-bit) */
    uint8_t lo = r(p, 0x34), mid = r(p, 0x35), hi = r(p, 0x36);
    CHECK(lo == 0x00 && mid == 0xE6 && hi == 0xF6, "$1234 * -128 = %02X%02X%02X, expected F6E600", hi, mid, lo);

    w(p, 0x1B, 0xFF); w(p, 0x1B, 0xFF); /* M7A = -1 */
    w(p, 0x1C, 0x7F);                   /* one write: high byte = $7F */
    lo = r(p, 0x34); mid = r(p, 0x35); hi = r(p, 0x36);
    CHECK(lo == 0x81 && mid == 0xFF && hi == 0xFF, "-1 * 127 = %02X%02X%02X, expected FFFF81", hi, mid, lo);

    w(p, 0x1B, 0xFF); w(p, 0x1B, 0x7F); /* M7A = 32767 */
    w(p, 0x1C, 0x00); w(p, 0x1C, 0x80); /* -128 */
    lo = r(p, 0x34); mid = r(p, 0x35); hi = r(p, 0x36);
    /* 32767 * -128 = -4194176 = $C00080 */
    CHECK(lo == 0x80 && mid == 0x00 && hi == 0xC0, "32767 * -128 = %02X%02X%02X, expected C00080", hi, mid, lo);
}

static void test_counter_latch_opct_stat78(void)
{
    SnesPpu *p = fresh();

    CHECK((r(p, 0x3F) & 0x40) == 0, "latch flag set at power");
    p->hcounter = 0x122;
    p->vcounter = 0x106;
    snes_ppu_latch_counters(p);
    p->hcounter = 5;
    p->vcounter = 7;

    uint8_t h1 = r(p, 0x3C), h2 = r(p, 0x3C);
    CHECK(h1 == 0x22 && h2 == 0x23, "OPHCT reads %02X %02X, expected 22 23 (bits 1-7 of the 2nd read are open bus)",
          h1, h2);
    uint8_t v1 = r(p, 0x3D), v2 = r(p, 0x3D);
    CHECK(v1 == 0x06 && v2 == 0x07, "OPVCT reads %02X %02X, expected 06 07", v1, v2);

    /* STAT78: latch flag set, version 3, NTSC, field 0; bit 5 open bus
     * (PPU2 MDR = $07 from the last OPVCT read). */
    uint8_t s = r(p, 0x3F);
    CHECK(s == 0x43, "STAT78 = %02X, expected 43", s);
    s = r(p, 0x3F);
    CHECK(s == 0x03, "STAT78 flag not cleared by the read: %02X", s);

    /* STAT78 also resets the OPHCT/OPVCT phase. */
    (void)r(p, 0x3C);
    (void)r(p, 0x3F);
    CHECK(r(p, 0x3C) == 0x22, "OPHCT phase not reset by STAT78");

    /* Field toggles at frame start. */
    snes_ppu_frame_start(p);
    CHECK((r(p, 0x3F) & 0x80) == 0x80, "field bit not set after frame start");
    snes_ppu_frame_start(p);
    CHECK((r(p, 0x3F) & 0x80) == 0x00, "field bit did not toggle back");

    /* WRIO bit 7 low holds the latch line: bit 6 reads 1. */
    snes_ppu_set_wrio(p, 0x7F);
    CHECK((r(p, 0x3F) & 0x40) == 0x40 && (r(p, 0x3F) & 0x40) == 0x40, "WRIO bit 7 low: flag not held");
    snes_ppu_set_wrio(p, 0xFF);
    CHECK((r(p, 0x3F) & 0x40) == 0x00, "flag stuck after WRIO bit 7 high");

    /* STAT77: version 1, no flags. */
    CHECK((r(p, 0x3E) & 0xEF) == 0x01, "STAT77 = %02X", r(p, 0x3E));
}

static void test_write_only_reads_open_bus(void)
{
    SnesPpu *p = fresh();

    /* PPU1 MDR := $10 via the multiplier (16 * 1). */
    w(p, 0x1B, 0x10); w(p, 0x1B, 0x00);
    w(p, 0x1C, 0x00); w(p, 0x1C, 0x01);
    CHECK(r(p, 0x34) == 0x10, "MPYL");

    static const uint8_t ppu1_regs[] = { 0x04, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x14, 0x15, 0x16,
                                         0x18, 0x19, 0x1A, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A };
    for (unsigned i = 0; i < sizeof ppu1_regs; i++) {
        uint8_t v = snes_ppu_read(p, ppu1_regs[i], 0xA5);
        CHECK(v == 0x10, "$21%02X read %02X, expected PPU1 open bus 10", ppu1_regs[i], v);
    }
    for (unsigned reg = 0x00; reg <= 0x33; reg++) {
        bool ppu1 = memchr(ppu1_regs, (int)reg, sizeof ppu1_regs) != NULL;
        if (ppu1) continue;
        uint8_t v = snes_ppu_read(p, (uint8_t)reg, 0xA5);
        CHECK(v == 0xA5, "$21%02X read %02X, expected CPU open bus A5", reg, v);
    }
    CHECK(snes_ppu_read(p, 0x37, 0x5A) == 0x5A, "$2137 did not return CPU open bus");
    /* STAT77 bit 4 is PPU1 open bus. */
    CHECK((r(p, 0x3E) & 0x10) == 0x10, "STAT77 bit 4 not PPU1 open bus");

    /* snes_ppu_reg shadows the last written value. */
    w(p, 0x2C, 0x13);
    CHECK(snes_ppu_reg(p, 0x2C) == 0x13, "register shadow");
}

static void test_oam_priority_rotation(void)
{
    SnesPpu *p = fresh();
    sprite_scene(p);
    cgram_set(p, 129, rgb5(31, 0, 0));
    cgram_set(p, 145, rgb5(0, 31, 0)); /* palette 1 color 1 */
    /* Sprite 0: red, sprite 5: green, both at (40, 40). */
    w(p, 0x02, 0); w(p, 0x03, 0);
    w(p, 0x04, 40); w(p, 0x04, 40); w(p, 0x04, 1); w(p, 0x04, 0x00);
    w(p, 0x02, 10); w(p, 0x03, 0); /* word 10 = sprite 5 */
    w(p, 0x04, 40); w(p, 0x04, 40); w(p, 0x04, 1); w(p, 0x04, 0x02);
    w(p, 0x00, 0x0F);

    w(p, 0x02, 0); w(p, 0x03, 0x00);
    render(p, 41);
    CHECK(px(p, 40, 40) == xrgb(rgb5(31, 0, 0)), "sprite 0 should win without rotation");
    /* Rotation: first sprite = (OAMADD >> 1) & 127 = 5. */
    w(p, 0x02, 10); w(p, 0x03, 0x80);
    render(p, 41);
    CHECK(px(p, 40, 40) == xrgb(rgb5(0, 31, 0)), "sprite 5 should win with rotation to 5");
}

static void test_obj_range_time_over(void)
{
    SnesPpu *p = fresh();
    sprite_scene(p);
    w(p, 0x00, 0x0F);
    /* 33 8x8 sprites on line 50. */
    w(p, 0x02, 0); w(p, 0x03, 0);
    for (unsigned n = 0; n < 33; n++) { w(p, 0x04, (uint8_t)(n * 4)); w(p, 0x04, 50); w(p, 0x04, 1); w(p, 0x04, 0); }
    snes_ppu_frame_start(p);
    render(p, 51);
    uint8_t s = r(p, 0x3E);
    CHECK((s & 0x40) && !(s & 0x80), "33 sprites: STAT77 = %02X, expected range over only", s);
    snes_ppu_frame_start(p);
    CHECK((r(p, 0x3E) & 0xC0) == 0, "frame start did not clear the flags");

    /* 5 sprites of 64 pixels (8 tiles each) = 40 tiles > 34. */
    p = fresh();
    sprite_scene(p);
    w(p, 0x00, 0x0F);
    w(p, 0x01, 0x40); /* OBSEL size 2: 8x8 / 64x64 */
    w(p, 0x02, 0); w(p, 0x03, 0);
    for (unsigned n = 0; n < 5; n++) { w(p, 0x04, (uint8_t)(n * 40)); w(p, 0x04, 50); w(p, 0x04, 0); w(p, 0x04, 0); }
    w(p, 0x02, 0); w(p, 0x03, 1); w(p, 0x04, 0xAA); w(p, 0x04, 0x02); /* sprites 0-4 large */
    snes_ppu_frame_start(p);
    render(p, 51);
    s = r(p, 0x3E);
    CHECK((s & 0x80) && !(s & 0x40), "40 tiles: STAT77 = %02X, expected time over only", s);
}

/* ------------------------------------------------------ rendering tests */

static void test_render_mode1_bg_tile(void)
{
    SnesPpu *p = fresh();
    mode1_bg1(p);
    /* Tile 1: pixel x has color x (planes 0-2 = $55, $33, $0F). */
    tile4(p, 0x1000, 1, 0x3355, 0x000F);
    map_set(p, 0x0400, 0, 0, (uint16_t)(0x0001 | (2u << 10))); /* palette 2 */
    cgram_set(p, 0, rgb5(1, 2, 3));
    for (unsigned c = 1; c < 16; c++) cgram_set(p, (uint8_t)(32 + c), rgb5(c * 2, 31 - c, c));

    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(1, 2, 3)), "color 0 is not transparent: %06X", px(p, 0, 0));
    for (unsigned x = 1; x < 8; x++)
        CHECK(px(p, (int)x, 0) == xrgb(rgb5(x * 2, 31 - x, x)), "pixel %u = %06X, expected %06X", x, px(p, (int)x, 0),
              xrgb(rgb5(x * 2, 31 - x, x)));
    CHECK(px(p, 8, 0) == xrgb(rgb5(1, 2, 3)), "next map entry not empty");

    /* Horizontal flip reverses the pixels. */
    map_set(p, 0x0400, 0, 0, (uint16_t)(0x4001 | (2u << 10)));
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(14, 24, 7)) && px(p, 7, 0) == xrgb(rgb5(1, 2, 3)), "h-flip");

    /* BG3 priority bit (BGMODE bit 3): a high-priority BG3 tile goes above BG1. */
    p = fresh();
    mode1_bg1(p);
    tile4_solid(p, 0x1000, 1, 1);
    map_set(p, 0x0400, 0, 0, 0x2001); /* BG1 high priority */
    w(p, 0x09, 0x08);                 /* BG3SC: map $0800 */
    w(p, 0x0C, 0x02);                 /* BG3 chars $2000 */
    for (unsigned row = 0; row < 8; row++) vram_set(p, (uint16_t)(0x2000 + 8 + row), 0x00FF); /* 2bpp tile 1 color 1 */
    vram_set(p, 0x0800, 0x2001);      /* BG3 high priority, palette 0 */
    scroll(p, 0x11, 0); scroll(p, 0x12, 0);
    cgram_set(p, 1, rgb5(31, 0, 0));  /* BG1 color (palette 0 color 1) and BG3 color 1 coincide... */
    cgram_set(p, 1, rgb5(31, 0, 0));
    w(p, 0x2C, 0x05);
    /* Distinguish: move BG1 to palette 1. */
    map_set(p, 0x0400, 0, 0, (uint16_t)(0x2001 | (1u << 10)));
    cgram_set(p, 17, rgb5(0, 31, 0));
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(0, 31, 0)), "without BG3 priority, BG1 high should win: %06X", px(p, 0, 0));
    w(p, 0x05, 0x09);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(31, 0, 0)), "with BG3 priority, BG3 high should win: %06X", px(p, 0, 0));
}

static void test_render_sprite_over_bg_priority(void)
{
    SnesPpu *p = fresh();
    sprite_scene(p);
    mode1_bg1(p);
    w(p, 0x2C, 0x11); /* BG1 + OBJ */
    tile4_solid(p, 0x1000, 1, 1);
    cgram_set(p, 1, rgb5(0, 0, 31)); /* BG1 blue */
    map_set(p, 0x0400, 2, 2, 0x0001); /* BG tile at (16, 16), low priority */
    map_set(p, 0x0400, 3, 2, 0x2001); /* (24, 16), high priority */
    uint32_t red = xrgb(rgb5(31, 0, 0)), blue = xrgb(rgb5(0, 0, 31));

    /* Sprites 0 and 1 over the two BG tiles; OAM Y = 16 shows on line 17 =
     * row 16, and the BG shows BG line 17 there (map row 2). */
    for (unsigned prio = 0; prio < 4; prio++) {
        w(p, 0x02, 0); w(p, 0x03, 0);
        uint8_t attr = (uint8_t)(prio << 4);
        w(p, 0x04, 16); w(p, 0x04, 16); w(p, 0x04, 1); w(p, 0x04, attr);
        w(p, 0x04, 24); w(p, 0x04, 16); w(p, 0x04, 1); w(p, 0x04, attr);
        render(p, 17);
        /* Mode 1: BG1 = 6/9, OBJ = 2/4/7/10. */
        uint32_t want_low = prio >= 2 ? red : blue;
        uint32_t want_high = prio >= 3 ? red : blue;
        CHECK(px(p, 16, 16) == want_low, "OBJ priority %u vs BG1 low: %06X", prio, px(p, 16, 16));
        CHECK(px(p, 24, 16) == want_high, "OBJ priority %u vs BG1 high: %06X", prio, px(p, 24, 16));
    }
    /* OBJ over the backdrop outside the BG tiles. */
    CHECK(px(p, 16, 16) == red, "sprite not drawn");
}

static void test_render_window_masks_bg(void)
{
    SnesPpu *p = fresh();
    mode1_bg1(p);
    tile4_solid(p, 0x1000, 1, 1);
    for (unsigned tx = 0; tx < 32; tx++) map_set(p, 0x0400, tx, 0, 0x0001);
    cgram_set(p, 1, rgb5(31, 31, 0));
    uint32_t bg = xrgb(rgb5(31, 31, 0)), back = 0;

    w(p, 0x26, 16); w(p, 0x27, 31); /* window 1: 16-31 */
    w(p, 0x28, 24); w(p, 0x29, 47); /* window 2: 24-47 */

    w(p, 0x23, 0x02); /* BG1: window 1 enabled */
    render(p, 1);
    CHECK(px(p, 20, 0) == bg, "TMW off: window must not mask");
    w(p, 0x2E, 0x01); /* TMW: BG1 */
    render(p, 1);
    CHECK(px(p, 15, 0) == bg && px(p, 16, 0) == back && px(p, 31, 0) == back && px(p, 32, 0) == bg,
          "window 1 inside mask: %06X %06X %06X %06X", px(p, 15, 0), px(p, 16, 0), px(p, 31, 0), px(p, 32, 0));
    w(p, 0x23, 0x03); /* inverted */
    render(p, 1);
    CHECK(px(p, 15, 0) == back && px(p, 16, 0) == bg && px(p, 31, 0) == bg && px(p, 32, 0) == back, "inverted window 1");

    /* Both windows: OR, AND, XOR, XNOR. */
    w(p, 0x23, 0x0A);
    static const struct { uint8_t logic; bool at20, at28, at40, at50; } cases[] = {
        { 0, true, true, true, false },   /* OR */
        { 1, false, true, false, false }, /* AND */
        { 2, true, false, true, false },  /* XOR */
        { 3, false, true, false, true },  /* XNOR */
    };
    for (unsigned i = 0; i < 4; i++) {
        w(p, 0x2A, cases[i].logic);
        render(p, 1);
        CHECK((px(p, 20, 0) == back) == cases[i].at20 && (px(p, 28, 0) == back) == cases[i].at28
                  && (px(p, 40, 0) == back) == cases[i].at40 && (px(p, 50, 0) == back) == cases[i].at50,
              "window logic %u", cases[i].logic);
    }

    /* A left edge beyond the right edge is an empty window. */
    w(p, 0x2A, 0); w(p, 0x23, 0x02);
    w(p, 0x26, 40); w(p, 0x27, 30);
    render(p, 1);
    CHECK(px(p, 35, 0) == bg && px(p, 30, 0) == bg, "empty window masked something");

    /* TSW affects only the sub screen. */
    w(p, 0x26, 16); w(p, 0x27, 31);
    w(p, 0x2E, 0x00); w(p, 0x2F, 0x01);
    render(p, 1);
    CHECK(px(p, 20, 0) == bg, "TSW masked the main screen");
}

static void test_render_color_math_add_half(void)
{
    SnesPpu *p = fresh();
    mode1_bg1(p);
    tile4_solid(p, 0x1000, 1, 1);
    tile4_solid(p, 0x1000, 2, 2);
    map_set(p, 0x0400, 0, 0, 0x0001);          /* BG1 at x 0-7 (color 1) */
    map_set(p, 0x0400, 1, 0, 0x0001);          /* and 8-15 */
    w(p, 0x08, 0x08);                          /* BG2SC: map $0800 */
    w(p, 0x0B, 0x11);                          /* BG2 chars $1000 too */
    vram_set(p, 0x0800, 0x0002);               /* BG2 at x 0-7 (color 2) only */
    scroll(p, 0x0F, 0); scroll(p, 0x10, 0);
    uint16_t A = rgb5(20, 10, 4), B = rgb5(16, 3, 30), F = rgb5(15, 0, 9);
    cgram_set(p, 1, A);
    cgram_set(p, 2, B);
    w(p, 0x32, (uint8_t)(0x20 | 15)); w(p, 0x32, (uint8_t)(0x40 | 0)); w(p, 0x32, (uint8_t)(0x80 | 9));
    w(p, 0x2C, 0x01); /* main: BG1 */
    w(p, 0x2D, 0x02); /* sub: BG2 */

    /* Sub screen add: channel-wise saturating add. */
    w(p, 0x30, 0x02);
    w(p, 0x31, 0x01);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(31, 13, 31)), "add: %06X", px(p, 0, 0));
    /* Where the sub screen is transparent, the fixed color is used. */
    CHECK(px(p, 8, 0) == xrgb(rgb5(31, 10, 13)), "add over transparent sub uses fixed color: %06X", px(p, 8, 0));

    /* Half add. */
    w(p, 0x31, 0x41);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(18, 6, 17)), "half add: %06X", px(p, 0, 0));
    /* Transparent sub: fixed color, not halved. */
    CHECK(px(p, 8, 0) == xrgb(rgb5(31, 10, 13)), "half add over transparent sub was halved: %06X", px(p, 8, 0));

    /* Subtract, and half subtract. */
    w(p, 0x31, 0x81);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(4, 7, 0)), "sub: %06X", px(p, 0, 0));
    w(p, 0x31, 0xC1);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(2, 3, 0)), "half sub: %06X", px(p, 0, 0));

    /* Fixed color math (CGWSEL bit 1 clear). */
    w(p, 0x30, 0x00);
    w(p, 0x31, 0x01);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(31, 10, 13)), "fixed add: %06X", px(p, 0, 0));
    w(p, 0x31, 0x41);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(17, 5, 6)), "fixed half add: %06X", px(p, 0, 0));

    /* Layer not enabled in CGADSUB: no math. */
    w(p, 0x31, 0x02);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(A), "math applied to a disabled layer");

    /* Math window "never" (CGWSEL bits 4-5 = 3) disables math. */
    w(p, 0x30, 0x30);
    w(p, 0x31, 0x01);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(A), "math window never: %06X", px(p, 0, 0));
    /* Clip main to black always (bits 6-7 = 3) with math on: black + fixed. */
    w(p, 0x30, 0xC0);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(F), "clip to black then add fixed: %06X", px(p, 0, 0));
}

static void test_render_mode7_identity(void)
{
    SnesPpu *p = fresh();
    w(p, 0x05, 0x07);
    w(p, 0x2C, 0x01);
    w(p, 0x1B, 0x00); w(p, 0x1B, 0x01); /* A = 1.0 */
    w(p, 0x1C, 0x00); w(p, 0x1C, 0x00);
    w(p, 0x1D, 0x00); w(p, 0x1D, 0x00);
    w(p, 0x1E, 0x00); w(p, 0x1E, 0x01); /* D = 1.0 */
    w(p, 0x1F, 0x00); w(p, 0x1F, 0x00);
    w(p, 0x20, 0x00); w(p, 0x20, 0x00);
    scroll(p, 0x0D, 0); scroll(p, 0x0E, 0);

    /* Map (low bytes): (0,0) = tile 1, (1,0) = tile 2. Tile data (high
     * bytes, word tile * 64 + y * 8 + x): tile 1 pixel x = 1 + x, tile 2 = $20. */
    w(p, 0x15, 0x00); /* increment after low byte */
    w(p, 0x16, 0x00); w(p, 0x17, 0x00);
    w(p, 0x18, 1); w(p, 0x18, 2);
    w(p, 0x15, 0x80); /* increment after high byte */
    w(p, 0x16, 64); w(p, 0x17, 0x00);
    for (unsigned i = 0; i < 64; i++) w(p, 0x19, (uint8_t)(1 + (i & 7)));
    for (unsigned i = 0; i < 64; i++) w(p, 0x19, 0x20);
    for (unsigned c = 1; c <= 8; c++) cgram_set(p, (uint8_t)c, rgb5(c * 3, c, 31 - c));
    cgram_set(p, 0x20, rgb5(0, 31, 31));
    cgram_set(p, 0, rgb5(2, 2, 2));
    w(p, 0x00, 0x0F);

    render(p, 1);
    for (unsigned x = 0; x < 8; x++)
        CHECK(px(p, (int)x, 0) == xrgb(rgb5((1 + x) * 3, 1 + x, 30 - x)), "mode 7 pixel %u = %06X", x, px(p, (int)x, 0));
    CHECK(px(p, 8, 0) == xrgb(rgb5(0, 31, 31)) && px(p, 15, 0) == xrgb(rgb5(0, 31, 31)), "mode 7 tile 2");
    CHECK(px(p, 16, 0) == xrgb(rgb5(2, 2, 2)), "mode 7 tile 0 should be transparent: %06X", px(p, 16, 0));

    /* H flip: screen x = 255 shows plane x = 0. */
    w(p, 0x1A, 0x01);
    render(p, 1);
    CHECK(px(p, 255, 0) == xrgb(rgb5(3, 1, 30)), "mode 7 h-flip: %06X", px(p, 255, 0));
    w(p, 0x1A, 0x00);

    /* Scroll 4: screen x = 0 shows plane x = 4. */
    scroll(p, 0x0D, 4);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(15, 5, 26)), "mode 7 scroll: %06X", px(p, 0, 0));
    scroll(p, 0x0D, 0);

    /* Scale 2.0 (A = D = 0.5): screen x = 2 shows plane x = 1. */
    w(p, 0x1B, 0x80); w(p, 0x1B, 0x00);
    render(p, 1);
    CHECK(px(p, 2, 0) == xrgb(rgb5(6, 2, 29)) && px(p, 3, 0) == xrgb(rgb5(6, 2, 29)), "mode 7 zoom");
    w(p, 0x1B, 0x00); w(p, 0x1B, 0x01);

    /* Repeat mode 2: outside the 1024x1024 plane is transparent. */
    scroll(p, 0x0D, 0x1FFC); /* -4 */
    w(p, 0x1A, 0x80);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(2, 2, 2)) && px(p, 4, 0) == xrgb(rgb5(3, 1, 30)), "mode 7 repeat 2: %06X %06X",
          px(p, 0, 0), px(p, 4, 0));
}

static void test_render_16x16_tile(void)
{
    SnesPpu *p = fresh();
    mode1_bg1(p);
    w(p, 0x05, 0x11); /* mode 1, BG1 16x16 */
    tile4_solid(p, 0x1000, 1, 1);
    tile4_solid(p, 0x1000, 2, 2);
    tile4_solid(p, 0x1000, 17, 3);
    tile4_solid(p, 0x1000, 18, 4);
    for (unsigned c = 1; c <= 4; c++) cgram_set(p, (uint8_t)c, rgb5(c * 7, 0, 0));
    map_set(p, 0x0400, 0, 0, 0x0001);

    render(p, 1); /* BG line 1: top half */
    CHECK(px(p, 0, 0) == xrgb(rgb5(7, 0, 0)) && px(p, 7, 0) == xrgb(rgb5(7, 0, 0)), "16x16 top-left");
    CHECK(px(p, 8, 0) == xrgb(rgb5(14, 0, 0)) && px(p, 15, 0) == xrgb(rgb5(14, 0, 0)), "16x16 top-right");
    CHECK(px(p, 16, 0) == 0, "16x16 tile wider than 16");
    render(p, 9); /* BG line 9: bottom half */
    CHECK(px(p, 0, 8) == xrgb(rgb5(21, 0, 0)) && px(p, 8, 8) == xrgb(rgb5(28, 0, 0)), "16x16 bottom");
    render(p, 16);
    CHECK(px(p, 0, 15) == 0, "16x16 tile taller than 16 (line 16 is the next map row)");

    /* Flips swap the quarters. */
    map_set(p, 0x0400, 0, 0, 0xC001);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(28, 0, 0)) && px(p, 8, 0) == xrgb(rgb5(21, 0, 0)), "16x16 hv-flip");
}

static void test_render_mosaic(void)
{
    SnesPpu *p = fresh();
    mode1_bg1(p);
    /* Tile 1: pixel x has color x + 1; tile 2 (second map row): color 9. */
    tile4(p, 0x1000, 1, 0x66AA, 0x011E);
    tile4_solid(p, 0x1000, 2, 9);
    for (unsigned tx = 0; tx < 32; tx++) {
        map_set(p, 0x0400, tx, 0, 0x0001);
        map_set(p, 0x0400, tx, 1, 0x0002);
    }
    for (unsigned c = 1; c <= 9; c++) cgram_set(p, (uint8_t)c, rgb5(c * 3, 31 - c * 3, c));
    w(p, 0x06, 0x31); /* size 4, BG1 */

    snes_ppu_frame_start(p);
    for (int y = 1; y <= 9; y++) render(p, y);
    /* Horizontal: blocks of 4 take the leftmost pixel. */
    CHECK(px(p, 0, 0) == xrgb(rgb5(3, 28, 1)) && px(p, 3, 0) == xrgb(rgb5(3, 28, 1)), "mosaic block 0");
    CHECK(px(p, 4, 0) == xrgb(rgb5(15, 16, 5)) && px(p, 7, 0) == xrgb(rgb5(15, 16, 5)), "mosaic block 1: %06X",
          px(p, 4, 0));
    /* Vertical: lines 5-8 repeat BG line 5 (first tile row), line 9 starts
     * a block at BG line 9 (second tile row). */
    CHECK(px(p, 0, 7) == xrgb(rgb5(3, 28, 1)), "mosaic line 8 should repeat line 5: %06X", px(p, 0, 7));
    CHECK(px(p, 0, 8) == xrgb(rgb5(27, 4, 9)), "mosaic line 9 should start a new block: %06X", px(p, 0, 8));

    /* Without mosaic the pixels are distinct. */
    w(p, 0x06, 0x00);
    render(p, 1);
    CHECK(px(p, 1, 0) == xrgb(rgb5(6, 25, 2)), "mosaic off");
}

static void test_render_hires_width_512(void)
{
    SnesPpu *p = fresh();
    w(p, 0x05, 0x05); /* mode 5: BG1 4bpp, 16-pixel-wide tiles */
    w(p, 0x07, 0x04);
    w(p, 0x0B, 0x01);
    scroll(p, 0x0D, 0); scroll(p, 0x0E, 0);
    tile4_solid(p, 0x1000, 1, 1);
    tile4_solid(p, 0x1000, 2, 2);
    map_set(p, 0x0400, 0, 0, 0x0001);
    cgram_set(p, 0, rgb5(0, 0, 10));
    cgram_set(p, 1, rgb5(31, 0, 0));
    cgram_set(p, 2, rgb5(0, 31, 0));
    w(p, 0x2C, 0x01); w(p, 0x2D, 0x01); /* main and sub: both halves of each dot */
    w(p, 0x00, 0x0F);

    snes_ppu_frame_start(p);
    render(p, 1);
    /* A later line in mode 1 (256 wide) is doubled when the frame is published. */
    w(p, 0x05, 0x01);
    cgram_set(p, 0, rgb5(5, 5, 5));
    render(p, 2);
    snes_ppu_vblank_start(p);
    CHECK(p->out_width == 512 && p->out_height == 224, "output %ux%u, expected 512x224", p->out_width, p->out_height);
    uint32_t c1 = xrgb(rgb5(31, 0, 0)), c2 = xrgb(rgb5(0, 31, 0));
    CHECK(px(p, 0, 0) == c1 && px(p, 7, 0) == c1, "hires tile 1 at 0-7: %06X %06X", px(p, 0, 0), px(p, 7, 0));
    CHECK(px(p, 8, 0) == c2 && px(p, 15, 0) == c2, "hires tile 2 at 8-15: %06X %06X", px(p, 8, 0), px(p, 15, 0));
    CHECK(px(p, 16, 0) == xrgb(rgb5(0, 0, 10)), "hires pixel 16 should be backdrop: %06X", px(p, 16, 0));
    /* Row 1 (mode 1: tile 1 is an 8-pixel red tile at x = 0-7) doubled. */
    CHECK(px(p, 0, 1) == c1 && px(p, 15, 1) == c1 && px(p, 16, 1) == xrgb(rgb5(5, 5, 5))
              && px(p, 511, 1) == xrgb(rgb5(5, 5, 5)),
          "256-wide row not doubled: %06X %06X %06X %06X", px(p, 0, 1), px(p, 15, 1), px(p, 16, 1), px(p, 511, 1));

    /* Main screen only: even (sub screen) pixels show the backdrop. */
    w(p, 0x05, 0x05);
    w(p, 0x2D, 0x00);
    cgram_set(p, 0, rgb5(0, 0, 10));
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(0, 0, 10)) && px(p, 1, 0) == c1, "hires main/sub interleave: %06X %06X",
          px(p, 0, 0), px(p, 1, 0));

    /* Pseudo-hires (SETINI bit 3) in mode 1: odd = main, even = sub. */
    mode1_bg1(p);
    w(p, 0x33, 0x08);
    w(p, 0x2D, 0x00);
    map_set(p, 0x0400, 0, 0, 0x0001);
    render(p, 1);
    CHECK(p->row_wide[0] && px(p, 0, 0) == xrgb(rgb5(0, 0, 10)) && px(p, 1, 0) == c1 && px(p, 15, 0) == c1
              && px(p, 16, 0) == xrgb(rgb5(0, 0, 10)),
          "pseudo-hires: %06X %06X %06X", px(p, 0, 0), px(p, 1, 0), px(p, 16, 0));

    /* A frame with no hires line goes back to 256. */
    w(p, 0x33, 0x00);
    snes_ppu_frame_start(p);
    for (int y = 1; y <= 224; y++) render(p, y);
    snes_ppu_vblank_start(p);
    CHECK(p->out_width == 256, "width stayed %u after a low-res frame", p->out_width);
}

static void test_render_interlace_rows(void)
{
    SnesPpu *p = fresh();
    w(p, 0x33, 0x01);
    w(p, 0x00, 0x0F);
    cgram_set(p, 0, rgb5(31, 0, 0));
    snes_ppu_frame_start(p); /* field 1 */
    render(p, 1);
    cgram_set(p, 0, rgb5(0, 31, 0));
    snes_ppu_frame_start(p); /* field 0 */
    render(p, 1);
    snes_ppu_vblank_start(p);
    CHECK(p->out_height == 448, "interlaced height %u", p->out_height);
    CHECK(px(p, 0, 0) == xrgb(rgb5(0, 31, 0)) && px(p, 0, 1) == xrgb(rgb5(31, 0, 0)),
          "field rows: row 0 %06X, row 1 %06X", px(p, 0, 0), px(p, 0, 1));

    /* Overscan: 239 lines. */
    p = fresh();
    w(p, 0x33, 0x04);
    w(p, 0x00, 0x0F);
    snes_ppu_frame_start(p);
    for (int y = 1; y <= 239; y++) render(p, y);
    snes_ppu_vblank_start(p);
    CHECK(p->out_height == 239, "overscan height %u", p->out_height);
}

static void test_render_direct_color(void)
{
    SnesPpu *p = fresh();
    w(p, 0x05, 0x03); /* mode 3: BG1 8bpp */
    w(p, 0x07, 0x04);
    w(p, 0x0B, 0x01);
    w(p, 0x2C, 0x01);
    scroll(p, 0x0D, 0); scroll(p, 0x0E, 0);
    /* 8bpp tile 1 = 32 words at $1000 + 32: color $FF (all planes set). */
    for (unsigned i = 0; i < 32; i++) vram_set(p, (uint16_t)(0x1000 + 32 + i), 0xFFFF);
    map_set(p, 0x0400, 0, 0, (uint16_t)(0x0001 | (7u << 10))); /* palette group 7 = bgr all 1 */
    cgram_set(p, 0xFF, rgb5(1, 1, 1));
    w(p, 0x00, 0x0F);
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(1, 1, 1)), "8bpp palette lookup: %06X", px(p, 0, 0));
    w(p, 0x30, 0x01); /* direct color */
    render(p, 1);
    /* BBGGGRRR + bgr -> 0BBb00GG Gg0RRRr0: channels 30, 30, 28. */
    CHECK(px(p, 0, 0) == xrgb(rgb5(30, 30, 28)), "direct color $FF group 7: %06X", px(p, 0, 0));
    map_set(p, 0x0400, 0, 0, 0x0001); /* group 0 */
    render(p, 1);
    CHECK(px(p, 0, 0) == xrgb(rgb5(28, 28, 24)), "direct color $FF group 0: %06X", px(p, 0, 0));
}

/* ---------------------------------------------------------------- main */

typedef struct {
    const char *name;
    void (*fn)(void);
} Test;

/* ---- mode 7 at a higher resolution (snes_ppu_set_mode7_scale) ---- */

/* A plane with something different at every texel: four tiles over the
 * map, each with its own 64 colours. */
static void m7_plane(SnesPpu *p)
{
    w(p, 0x05, 0x07);
    w(p, 0x2C, 0x01);
    w(p, 0x00, 0x0F);
    for (unsigned ty = 0; ty < 128; ty++)
        for (unsigned tx = 0; tx < 128; tx++) p->vram[2u * (ty << 7 | tx)] = (uint8_t)(1u + ((tx * 3u + ty * 5u) & 3u));
    for (unsigned t = 1; t <= 4; t++)
        for (unsigned i = 0; i < 64; i++) p->vram[2u * (t << 6 | i) + 1u] = (uint8_t)(1u + (t * 40u + (i * 7u) % 37u) % 250u);
    for (unsigned c = 1; c < 256; c++) cgram_set(p, (uint8_t)c, rgb5(c & 31u, (c >> 3) & 31u, (c * 5u) & 31u));
    cgram_set(p, 0, rgb5(1, 2, 3));
}

static void m7_matrix(SnesPpu *p, int a, int b, int c, int d, int cx, int cy, int hofs, int vofs)
{
    const int v[6] = { a, b, c, d, cx, cy };
    for (unsigned i = 0; i < 6; i++) {
        w(p, (uint8_t)(0x1B + i), (uint8_t)v[i]);
        w(p, (uint8_t)(0x1B + i), (uint8_t)((unsigned)v[i] >> 8));
    }
    scroll(p, 0x0D, (uint16_t)hofs);
    scroll(p, 0x0E, (uint16_t)vofs);
}

static void m7_frame(SnesPpu *p, void (*per_line)(SnesPpu *, int))
{
    snes_ppu_frame_start(p);
    for (int y = 1; y <= 224; y++) {
        if (per_line) per_line(p, y);
        render(p, y);
    }
    snes_ppu_vblank_start(p);
}

static int64_t m7_floor_div(int64_t v, int64_t d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

/* The plane's colour at screen position (xs, ys) in n-ths of a pixel, for a
 * matrix and offsets given in n-ths too: the console's formula in exact
 * arithmetic. */
static uint32_t m7_model(const SnesPpu *p, int n, const int64_t m[8], int64_t xs, int64_t ys)
{
    const int64_t a = m[0], b = m[1], c = m[2], d = m[3], hc = m[4], vc = m[5], hcenter = m[6], vcenter = m[7];
    const int64_t unit = 256 * (int64_t)n * n;
    const uint32_t x = (uint32_t)m7_floor_div(a * hc + b * vc + b * ys + hcenter * 256 * n + a * xs, unit);
    const uint32_t y = (uint32_t)m7_floor_div(c * hc + d * vc + d * ys + vcenter * 256 * n + c * xs, unit);
    const uint8_t *v = snes_ppu_vram(p);
    const uint32_t tile = v[2u * (((y >> 3) & 127u) << 7 | ((x >> 3) & 127u))];
    const uint8_t palette = v[2u * (tile << 6 | (y & 7u) << 3 | (x & 7u)) + 1u];
    return xrgb(cgram_word(p, palette));
}

static void m7_line_mode(SnesPpu *p, int y) { w(p, 0x05, y <= 8 ? 0x01 : 0x07); }

/* A tilt: the matrix grows by 2 a line, with a cut at line 100. */
static int m7_tilt_a(int y) { return 0x100 + 2 * y + (y >= 100 ? 0x200 : 0); }
static void m7_line_tilt(SnesPpu *p, int y) { m7_matrix(p, m7_tilt_a(y), 0, 0, m7_tilt_a(y), 0, 0, 0, 0); }

static void test_render_mode7_larger(void)
{
    static uint32_t console[224 * 256];
    static const struct { int a, b, c, d, cx, cy, hofs, vofs, sel; } cases[4] = {
        { 0x100, 0, 0, 0x100, 0, 0, 0, 0, 0 },          /* as it is */
        { 0xC0, 0x40, -0x40, 0xC0, 64, 48, 37, -5, 0 }, /* turned and enlarged about (64, 48) */
        { 0x200, 0, 0, 0x200, 0, 0, 3, 7, 0 },          /* two texels to a pixel */
        { 0x140, 0x80, -0x80, 0x140, 10, 20, 0, 0, 3 }, /* flipped both ways */
    };
    SnesPpu *p = fresh();
    int width = 0, height = 0;
    m7_plane(p);

    CHECK(snes_ppu_mode7_scale(p) == 1 && !snes_ppu_large_frame(p, &width, &height), "a larger picture at a scale of 1");
    CHECK(!snes_ppu_set_mode7_scale(p, 5) && snes_ppu_mode7_scale(p) == 1, "5 is not a scale");

    /* Matrices the console's rounding to 64ths loses nothing of: every
     * sample is the plane at its own place, and the sample at a whole
     * pixel is the console's pixel. */
    for (unsigned i = 0; i < 4; i++) {
        const int hc = cases[i].hofs - cases[i].cx, vc = cases[i].vofs - cases[i].cy;
        m7_matrix(p, cases[i].a, cases[i].b, cases[i].c, cases[i].d, cases[i].cx, cases[i].cy, cases[i].hofs, cases[i].vofs);
        w(p, 0x1A, (uint8_t)cases[i].sel);
        snes_ppu_set_mode7_scale(p, 1);
        m7_frame(p, NULL);
        for (int row = 0; row < 224; row++) memcpy(console + row * 256, p->framebuffer + row * SNES_FB_WIDTH, 256 * sizeof console[0]);
        for (int n = 2; n <= 4; n++) {
            CHECK(snes_ppu_set_mode7_scale(p, n) && snes_ppu_mode7_scale(p) == n, "scale %d refused", n);
            m7_frame(p, NULL);
            unsigned changed = 0, wrong = 0, not_console = 0;
            for (int row = 0; row < 224; row++)
                changed += memcmp(console + row * 256, p->framebuffer + row * SNES_FB_WIDTH, 256 * sizeof console[0]) != 0;
            CHECK(changed == 0, "case %u at %dx: %u of the console's rows changed", i, n, changed);
            const uint32_t *large = snes_ppu_large_frame(p, &width, &height);
            CHECK(large && width == 256 * n && height == 224 * n, "case %u at %dx: the larger picture is %d x %d", i, n, width, height);
            if (!large || width != 256 * n || height != 224 * n) continue;
            const int64_t m[8] = { (int64_t)cases[i].a * n, (int64_t)cases[i].b * n, (int64_t)cases[i].c * n, (int64_t)cases[i].d * n,
                                   (int64_t)hc * n, (int64_t)vc * n, (int64_t)cases[i].cx * n, (int64_t)cases[i].cy * n };
            for (int ys = 0; ys < height; ys++) {
                for (int xs = 0; xs < width; xs++) {
                    /* Line y is output row y - 1; a flip runs the other way from 255. */
                    const int64_t py = (int64_t)ys + n, px_ = xs;
                    const int64_t my = (cases[i].sel & 2) ? (int64_t)255 * n - py : py;
                    const int64_t mx = (cases[i].sel & 1) ? (int64_t)255 * n - px_ : px_;
                    const uint32_t got = large[(size_t)ys * (size_t)width + (size_t)xs];
                    wrong += got != m7_model(p, n, m, mx, my);
                    if (ys % n == 0 && xs % n == 0) not_console += got != console[(ys / n) * 256 + xs / n];
                }
            }
            CHECK(wrong == 0, "case %u at %dx: %u samples are not the plane's", i, n, wrong);
            CHECK(not_console == 0, "case %u at %dx: %u whole-pixel samples are not the console's", i, n, not_console);
        }
    }
    w(p, 0x1A, 0x00);

    /* A window that hides the plane hides every sample of those pixels. */
    m7_matrix(p, 0xC0, 0x40, -0x40, 0xC0, 0, 0, 0, 0);
    w(p, 0x23, 0x02); w(p, 0x26, 100); w(p, 0x27, 150); w(p, 0x2E, 0x01);
    snes_ppu_set_mode7_scale(p, 4);
    m7_frame(p, NULL);
    {
        const uint32_t *large = snes_ppu_large_frame(p, &width, &height);
        unsigned wrong = 0, open = 0;
        for (int ys = 0; large && ys < height; ys++)
            for (int xs = 0; xs < width; xs++) {
                const bool hidden = xs / 4 >= 100 && xs / 4 <= 150;
                const uint32_t got = large[(size_t)ys * (size_t)width + (size_t)xs];
                if (hidden) wrong += got != xrgb(rgb5(1, 2, 3));
                else open += got != xrgb(rgb5(1, 2, 3));
            }
        CHECK(large && wrong == 0, "%u samples show the plane through its window", wrong);
        CHECK(open > 100000, "the plane outside the window: %u samples", open);
    }
    w(p, 0x23, 0x00); w(p, 0x2E, 0x00);

    /* A matrix that changes from line to line is carried on between lines
     * by its last change, but not across a cut. */
    snes_ppu_set_mode7_scale(p, 4);
    m7_frame(p, m7_line_tilt);
    {
        const uint32_t *large = snes_ppu_large_frame(p, &width, &height);
        unsigned wrong = 0;
        for (int y = 1; large && y <= 224; y++) {
            const int a = m7_tilt_a(y), step = y == 1 || y == 100 ? 0 : 2;
            for (int k = 0; k < 4; k++) {
                const int64_t m[8] = { (int64_t)a * 4 + step * k, 0, 0, (int64_t)a * 4 + step * k, 0, 0, 0, 0 };
                for (int xs = 0; xs < width; xs++)
                    wrong += large[((size_t)(y - 1) * 4u + (size_t)k) * (size_t)width + (size_t)xs] !=
                             m7_model(p, 4, m, xs, (int64_t)y * 4 + k);
            }
        }
        CHECK(large && wrong == 0, "tilted plane: %u samples are not the plane's", wrong);
    }

    /* Lines of another mode are the console's pixels, repeated. */
    m7_matrix(p, 0xC0, 0x40, -0x40, 0xC0, 0, 0, 0, 0);
    m7_frame(p, m7_line_mode);
    {
        const uint32_t *large = snes_ppu_large_frame(p, &width, &height);
        unsigned wrong = 0;
        for (int ys = 0; large && ys < 8 * 4; ys++)
            for (int xs = 0; xs < width; xs++)
                wrong += large[(size_t)ys * (size_t)width + (size_t)xs] != px(p, xs / 4, ys / 4);
        CHECK(large && wrong == 0, "lines of mode 1: %u samples are not the console's", wrong);
    }

    /* Mosaic on the plane, and a frame without the plane: no larger picture. */
    w(p, 0x05, 0x07);
    w(p, 0x06, 0x31);
    m7_frame(p, NULL);
    CHECK(!snes_ppu_large_frame(p, &width, &height), "a larger picture of a mosaic");
    w(p, 0x06, 0x00);
    m7_frame(p, NULL);
    CHECK(snes_ppu_large_frame(p, &width, &height) != NULL, "no larger picture once the mosaic is off");
    w(p, 0x05, 0x01);
    m7_frame(p, NULL);
    CHECK(!snes_ppu_large_frame(p, &width, &height), "a larger picture of a frame without mode 7");
    snes_ppu_set_mode7_scale(p, 1);
    CHECK(snes_ppu_mode7_scale(p) == 1 && !snes_ppu_large_frame(p, &width, &height), "the scale goes back to 1");
}

int main(void)
{
    static const Test tests[] = {
        { "vram_increment_and_remap", test_vram_increment_and_remap },
        { "vram_write_blocked_active_display", test_vram_write_blocked_active_display },
        { "vram_read_prefetch", test_vram_read_prefetch },
        { "cgram_latch_format_xrgb", test_cgram_latch_format_xrgb },
        { "oam_low_high_table_sprite", test_oam_low_high_table_sprite },
        { "bgofs_double_write_latch", test_bgofs_double_write_latch },
        { "m7_multiplier", test_m7_multiplier },
        { "counter_latch_opct_stat78", test_counter_latch_opct_stat78 },
        { "write_only_reads_open_bus", test_write_only_reads_open_bus },
        { "oam_priority_rotation", test_oam_priority_rotation },
        { "obj_range_time_over", test_obj_range_time_over },
        { "render_mode1_bg_tile", test_render_mode1_bg_tile },
        { "render_sprite_over_bg_priority", test_render_sprite_over_bg_priority },
        { "render_window_masks_bg", test_render_window_masks_bg },
        { "render_color_math_add_half", test_render_color_math_add_half },
        { "render_mode7_identity", test_render_mode7_identity },
        { "render_mode7_larger", test_render_mode7_larger },
        { "render_16x16_tile", test_render_16x16_tile },
        { "render_mosaic", test_render_mosaic },
        { "render_hires_width_512", test_render_hires_width_512 },
        { "render_interlace_rows", test_render_interlace_rows },
        { "render_direct_color", test_render_direct_color },
    };
    int failed_tests = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        test_failures = 0;
        tests[i].fn();
        printf("%s %s\n", test_failures ? "FAIL" : "PASS", tests[i].name);
        if (test_failures) failed_tests++;
        failures += test_failures;
    }
    printf("%d of %d tests failed (%d checks)\n", failed_tests, (int)(sizeof tests / sizeof tests[0]), failures);
    return failures ? 1 : 0;
}
