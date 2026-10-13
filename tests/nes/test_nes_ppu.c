/*
 * NES PPU unit tests: the register file ($2000-$2007), VBlank/NMI timing
 * and the $2002 race, frame length, the bus accesses of a scanline, and
 * rendering of small scenes checked pixel by pixel (the larger ones against
 * a reference renderer written here from the hardware rules).
 *
 * The test cartridge is 8 KiB of CHR RAM and 2 KiB of nametable RAM with
 * selectable mirroring. It provides the three nes_ppu_bus_* functions and
 * logs every call with the PPU's position.
 *
 * Each test is named; the program prints PASS/FAIL per test and exits
 * non-zero if any check failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes_ppu.h"

static int failures;
static int test_failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            if (test_failures < 20) {                     \
                printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                      \
                printf("\n");                             \
            }                                             \
            test_failures++;                              \
        }                                                 \
    } while (0)

/* ---------------------------------------------------------- the cartridge */

enum { MIRROR_HORIZONTAL, MIRROR_VERTICAL };
enum { LOG_CAPACITY = 4096 };

typedef struct {
    uint16_t scanline, dot, addr;
    char kind; /* 'R' read, 'W' write, 'A' address only */
} BusEvent;

typedef struct {
    uint8_t chr[0x2000];
    uint8_t nt[0x800];
    int mirroring;

    /* Every access goes into the running hash; the events themselves are
     * kept only while `logging` is set. */
    uint64_t hash;
    uint64_t total;
    bool logging;
    size_t count;
    BusEvent log[LOG_CAPACITY];
} Cart;

static Cart cart;
static NesPpu ppu_storage;

static unsigned nt_index(unsigned addr)
{
    const unsigned table = (addr >> 10) & 3u;
    const unsigned ram = cart.mirroring == MIRROR_VERTICAL ? (table & 1u) : (table >> 1);
    return ram * 0x400u + (addr & 0x3FFu);
}

static uint64_t fnv(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

static void cart_log(const NesPpu *p, char kind, uint16_t addr)
{
    Cart *c = (Cart *)p->user;
    c->hash = fnv(c->hash, ((uint64_t)p->scanline << 40) | ((uint64_t)p->dot << 24) | ((uint64_t)addr << 8) |
                               (uint64_t)(unsigned char)kind);
    c->total++;
    if (c->logging && c->count < LOG_CAPACITY) {
        BusEvent *e = &c->log[c->count++];
        e->scanline = p->scanline;
        e->dot = p->dot;
        e->addr = addr;
        e->kind = kind;
    }
}

uint8_t nes_ppu_bus_read(NesPpu *p, uint16_t addr)
{
    Cart *c = (Cart *)p->user;
    cart_log(p, 'R', addr);
    return addr < 0x2000u ? c->chr[addr] : c->nt[nt_index(addr)];
}

void nes_ppu_bus_write(NesPpu *p, uint16_t addr, uint8_t value)
{
    Cart *c = (Cart *)p->user;
    cart_log(p, 'W', addr);
    if (addr < 0x2000u)
        c->chr[addr] = value;
    else
        c->nt[nt_index(addr)] = value;
}

void nes_ppu_bus_address(NesPpu *p, uint16_t addr) { cart_log(p, 'A', addr); }

/* ---------------------------------------------------------------- helpers */

static NesPpu *fresh(void)
{
    NesPpu *p = &ppu_storage;
    memset(&cart, 0, sizeof cart);
    cart.hash = 0xCBF29CE484222325ull;
    cart.mirroring = MIRROR_VERTICAL;
    memset(p, 0, sizeof *p);
    p->user = &cart;
    nes_ppu_power(p);
    return p;
}

static void run(NesPpu *p, uint32_t dots) { nes_ppu_run(p, dots); }

/* Run dot by dot until the PPU is in (scanline, dot). */
static void run_to(NesPpu *p, unsigned scanline, unsigned dot)
{
    for (uint32_t guard = 0; guard < 400000u; guard++) {
        if (p->scanline == scanline && p->dot == dot) return;
        nes_ppu_run(p, 1);
    }
    CHECK(0, "run_to(%u, %u) never arrived", scanline, dot);
}

/* A register access followed by the rest of a four-cycle instruction, as
 * a program spaces them. Tests about exact dots call nes_ppu_read/write. */
static void wr(NesPpu *p, unsigned reg, unsigned value)
{
    nes_ppu_write(p, reg, (uint8_t)value);
    nes_ppu_run(p, 12);
}

static uint8_t rd(NesPpu *p, unsigned reg)
{
    uint8_t v = nes_ppu_read(p, reg);
    nes_ppu_run(p, 12);
    return v;
}

static void set_addr(NesPpu *p, unsigned addr)
{
    wr(p, 6, addr >> 8);
    wr(p, 6, addr & 0xFFu);
}

static void pal_set(NesPpu *p, unsigned index, unsigned value)
{
    set_addr(p, 0x3F00u + index);
    wr(p, 7, value);
}

static uint8_t pal_get(NesPpu *p, unsigned index)
{
    set_addr(p, 0x3F00u + index);
    return (uint8_t)(rd(p, 7) & 0x3Fu);
}

/* Colours used by the scenes: every palette entry is distinct. */
enum { BACKDROP = 0x0F };
static unsigned bgc(unsigned pal, unsigned c) { return (pal << 4) | c; }
static unsigned spc(unsigned pal, unsigned c) { return (pal << 4) | (c + 4u); }

static void standard_palette(NesPpu *p)
{
    for (unsigned pal = 0; pal < 4; pal++) {
        pal_set(p, pal * 4u, 0x3D); /* never shown, except entry 0 below */
        pal_set(p, 0x10u + pal * 4u, 0x3D);
        for (unsigned c = 1; c < 4; c++) {
            pal_set(p, pal * 4u + c, bgc(pal, c));
            pal_set(p, 0x10u + pal * 4u + c, spc(pal, c));
        }
    }
    /* $3F04/$3F08/$3F0C (and their sprite mirrors) keep $3D; the backdrop
     * is written last because $3F10 mirrors it. */
    pal_set(p, 0, BACKDROP);
    set_addr(p, 0x0000);
}

static void tile_rows(unsigned table, unsigned n, const uint8_t lo[8], const uint8_t hi[8])
{
    memcpy(&cart.chr[table * 0x1000u + n * 16u], lo, 8);
    memcpy(&cart.chr[table * 0x1000u + n * 16u + 8u], hi, 8);
}

static void tile_solid(unsigned table, unsigned n, unsigned colour)
{
    uint8_t lo[8], hi[8];
    memset(lo, (colour & 1u) ? 0xFF : 0x00, 8);
    memset(hi, (colour & 2u) ? 0xFF : 0x00, 8);
    tile_rows(table, n, lo, hi);
}

/* The "L" tile: top row and left column in colour 1, the bottom-right
 * pixel in colour 2. Every flip of it is different. */
static void tile_l(unsigned table, unsigned n)
{
    static const uint8_t lo[8] = { 0xFF, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
    static const uint8_t hi[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    tile_rows(table, n, lo, hi);
}

static unsigned l_colour(unsigned c, unsigned r)
{
    if (c == 7 && r == 7) return 2;
    return (r == 0 || c == 0) ? 1u : 0u;
}

static unsigned chr_pixel(unsigned table, unsigned n, unsigned c, unsigned r)
{
    const uint8_t lo = cart.chr[table * 0x1000u + n * 16u + r];
    const uint8_t hi = cart.chr[table * 0x1000u + n * 16u + 8u + r];
    return ((lo >> (7u - c)) & 1u) | (((hi >> (7u - c)) & 1u) << 1);
}

static void nt_put(unsigned table, unsigned col, unsigned row, unsigned tile)
{
    cart.nt[nt_index(0x2000u + table * 0x400u + row * 32u + col)] = (uint8_t)tile;
}

static void at_put(unsigned table, unsigned bx, unsigned by, unsigned value)
{
    cart.nt[nt_index(0x23C0u + table * 0x400u + by * 8u + bx)] = (uint8_t)value;
}

/* OAM is poked directly; the attribute bits that do not exist are masked
 * as a write through $2004 would. */
static void oam_hide_all(NesPpu *p)
{
    memset(p->oam, 0xFF, sizeof p->oam);
    for (unsigned n = 0; n < 64; n++) p->oam[n * 4u + 2u] = 0xE3;
}

static void oam_put(NesPpu *p, unsigned n, unsigned y, unsigned tile, unsigned attr, unsigned x)
{
    p->oam[n * 4u + 0u] = (uint8_t)y;
    p->oam[n * 4u + 1u] = (uint8_t)tile;
    p->oam[n * 4u + 2u] = (uint8_t)(attr & 0xE3u);
    p->oam[n * 4u + 3u] = (uint8_t)x;
}

/* Run to the next completed picture; returns the dots that took. */
static uint32_t frame(NesPpu *p)
{
    uint32_t n = 0;
    p->frame_ready = false;
    do {
        nes_ppu_run(p, 1);
        n++;
    } while (!p->frame_ready && n < 400000u);
    return n;
}

static void frames(NesPpu *p, unsigned count)
{
    while (count--) frame(p);
}

/* A picture rendered from start to end with the current registers: the
 * first frame() finishes the one in progress (whose start may have had
 * other settings), the second is complete. Ends at scanline 240 dot 0. */
static void render(NesPpu *p)
{
    frame(p);
    frame(p);
}

static unsigned px(const NesPpu *p, unsigned x, unsigned y) { return p->frame[y * NES_PPU_WIDTH + x]; }

static uint32_t rng_state;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* ------------------------------------------------- the reference renderer */

typedef struct {
    unsigned scroll_x, scroll_y; /* 0-511, 0-479 */
    uint8_t ctrl, mask;
} RefScene;

static unsigned ref_palette(const NesPpu *p, unsigned index, uint8_t mask)
{
    index &= 0x1Fu;
    if (!(index & 3u)) index &= 0x0Fu;
    unsigned c = p->palette[index];
    if (mask & 0x01u) c &= 0x30u;
    return c;
}

/* Background palette index (0 = transparent) at picture position (x, y). */
static unsigned ref_bg(const RefScene *s, unsigned x, unsigned y)
{
    const unsigned wx = (x + s->scroll_x) % 512u, wy = (y + s->scroll_y) % 480u;
    const unsigned base = 0x2000u + (((wy / 240u) << 1) | (wx >> 8)) * 0x400u;
    const unsigned col = (wx & 255u) >> 3, row = (wy % 240u) >> 3;
    const unsigned tile = cart.nt[nt_index(base + row * 32u + col)];
    const unsigned at = cart.nt[nt_index(base + 0x3C0u + (row >> 2) * 8u + (col >> 2))];
    const unsigned pal = (at >> (((row & 2u) << 1) | (col & 2u))) & 3u;
    const unsigned c = chr_pixel((s->ctrl >> 4) & 1u, tile, wx & 7u, (wy % 240u) & 7u);
    return c ? (pal << 2) | c : 0u;
}

/* The frame[] value at (x, y): background, the first eight sprites in OAM
 * order that cover the previous line's number, priority, clipping. */
static unsigned ref_pixel(const NesPpu *p, const RefScene *s, unsigned x, unsigned y)
{
    const uint8_t mask = s->mask;
    const unsigned height = (s->ctrl & 0x20u) ? 16u : 8u;
    unsigned bg = 0, sprite = 0;
    bool behind = false;

    if ((mask & 0x08u) && (x >= 8u || (mask & 0x02u))) bg = ref_bg(s, x, y);

    if ((mask & 0x10u) && (x >= 8u || (mask & 0x04u)) && y >= 1u) {
        unsigned found = 0;
        for (unsigned n = 0; n < 64u && found < 8u && !sprite; n++) {
            const uint8_t *o = &p->oam[n * 4u];
            unsigned row = (y - 1u) - o[0];
            if (y - 1u < o[0] || row >= height) continue;
            found++;
            if (x < o[3] || x - o[3] >= 8u) continue;
            unsigned c = x - o[3];
            if (o[2] & 0x40u) c = 7u - c;
            if (o[2] & 0x80u) row = height - 1u - row;
            unsigned table, tile;
            if (height == 16u) {
                table = o[1] & 1u;
                tile = (o[1] & 0xFEu) + (row >> 3);
            } else {
                table = (s->ctrl >> 3) & 1u;
                tile = o[1];
            }
            const unsigned colour = chr_pixel(table, tile, c, row & 7u);
            if (colour) {
                sprite = 0x10u | ((o[2] & 3u) << 2) | colour;
                behind = (o[2] & 0x20u) != 0;
            }
        }
    }

    unsigned index = bg;
    if (sprite && (!bg || !behind)) index = sprite;
    return ref_palette(p, index, mask) | ((unsigned)(mask >> 5) << 6);
}

/* Compare the whole picture with the reference; returns mismatches. */
static unsigned compare_with_reference(const NesPpu *p, const RefScene *s, const char *what)
{
    unsigned bad = 0;
    for (unsigned y = 0; y < NES_PPU_HEIGHT; y++) {
        for (unsigned x = 0; x < NES_PPU_WIDTH; x++) {
            const unsigned want = ref_pixel(p, s, x, y);
            if (px(p, x, y) != want) {
                if (bad < 3) CHECK(0, "%s: (%u, %u) is %03X, reference %03X", what, x, y, px(p, x, y), want);
                bad++;
            }
        }
    }
    CHECK(bad == 0, "%s: %u pixels differ from the reference", what, bad);
    return bad;
}

/* Write a scroll position and control byte the way a game does in VBlank. */
static void set_scene(NesPpu *p, const RefScene *s)
{
    wr(p, 0, (unsigned)(s->ctrl & 0xFCu) | ((s->scroll_y / 240u) << 1) | (s->scroll_x >> 8));
    rd(p, 2);
    wr(p, 5, s->scroll_x & 0xFFu);
    wr(p, 5, s->scroll_y % 240u);
    wr(p, 1, s->mask);
}

/* Random tiles, nametables, attributes and sprites. */
static void random_scene(NesPpu *p, uint32_t seed, unsigned sprites)
{
    rng_state = seed;
    standard_palette(p);
    for (unsigned i = 16; i < sizeof cart.chr; i++) cart.chr[i] = (uint8_t)rng();
    memset(&cart.chr[0x1000], 0, 16); /* tile 0 of both tables stays empty */
    for (unsigned i = 0; i < sizeof cart.nt; i++) cart.nt[i] = (uint8_t)((rng() & 3u) ? rng() & 0x1Fu : 0u);
    for (unsigned t = 0; t < 2; t++)
        for (unsigned i = 0; i < 64; i++) cart.nt[t * 0x400u + 0x3C0u + i] = (uint8_t)rng();
    oam_hide_all(p);
    for (unsigned n = 0; n < sprites; n++) oam_put(p, n, rng() % 236u, rng() & 0xFFu, rng() & 0xFFu, rng() & 0xFFu);
}

/* -------------------------------------------------------------- registers */

static void test_scroll_latch_wiki_example(void)
{
    NesPpu *p = fresh();

    nes_ppu_write(p, 0, 0x03);
    CHECK(p->t == 0x0C00, "$2000 nametable bits: t=%04X", p->t);
    nes_ppu_write(p, 0, 0x00);
    CHECK(p->t == 0x0000, "t=%04X", p->t);

    /* The split-scroll example of the wiki's "PPU scrolling" page:
     * $2006=$04, $2005=$3E, $2005=$7D, $2006=$EF. */
    nes_ppu_write(p, 6, 0x04);
    CHECK(p->t == 0x0400 && p->w == 1, "after $2006=$04: t=%04X w=%u", p->t, p->w);
    nes_ppu_write(p, 5, 0x3E);
    CHECK(p->t == 0x64E0 && p->w == 0, "after $2005=$3E: t=%04X w=%u", p->t, p->w);
    nes_ppu_write(p, 5, 0x7D);
    CHECK(p->t == 0x64EF && p->fine_x == 5 && p->w == 1, "after $2005=$7D: t=%04X x=%u w=%u", p->t, p->fine_x, p->w);
    CHECK(p->v == 0x0000, "v must not change yet: %04X", p->v);
    nes_ppu_write(p, 6, 0xEF);
    CHECK(p->t == 0x64EF && p->w == 0, "after $2006=$EF: t=%04X w=%u", p->t, p->w);
    run(p, 2);
    CHECK(p->v == 0x0000, "v changes on the third dot after the write: %04X", p->v);
    run(p, 1);
    CHECK(p->v == 0x64EF, "v=%04X", p->v);

    /* $2002 clears the write toggle. */
    nes_ppu_write(p, 5, 0x00);
    CHECK(p->w == 1, "w=%u", p->w);
    nes_ppu_read(p, 2);
    CHECK(p->w == 0, "w after $2002 read=%u", p->w);

    /* A plain scroll pair. X=$AB: coarse 21, fine 3. Y=$CD: coarse 25,
     * fine 5. The nametable bits are untouched. */
    nes_ppu_write(p, 0, 0x02);
    nes_ppu_write(p, 5, 0xAB);
    nes_ppu_write(p, 5, 0xCD);
    CHECK(p->t == (0x5000u | 0x0800u | (25u << 5) | 21u) && p->fine_x == 3, "t=%04X x=%u", p->t, p->fine_x);

    /* The first $2006 write clears bit 14 and keeps the low byte. */
    p->t = 0x7FFF;
    nes_ppu_write(p, 6, 0xFF);
    CHECK(p->t == 0x3FFF, "t=%04X", p->t);
    nes_ppu_write(p, 6, 0x12);
    CHECK(p->t == 0x3F12, "t=%04X", p->t);
}

static void test_data_port_buffer_and_increment(void)
{
    NesPpu *p = fresh();

    set_addr(p, 0x2105);
    wr(p, 7, 0xAA);
    wr(p, 7, 0xBB);
    CHECK(cart.nt[nt_index(0x2105)] == 0xAA && cart.nt[nt_index(0x2106)] == 0xBB, "nametable writes");
    CHECK(p->v == 0x2107, "v=%04X", p->v);

    /* Reads are one behind. */
    set_addr(p, 0x2105);
    p->read_buffer = 0x99;
    CHECK(rd(p, 7) == 0x99, "the first read returns the old buffer");
    CHECK(rd(p, 7) == 0xAA, "second read");
    CHECK(rd(p, 7) == 0xBB, "third read");
    CHECK(p->v == 0x2108, "v=%04X", p->v);

    /* Pattern tables go to the cartridge too. */
    set_addr(p, 0x1234);
    wr(p, 7, 0x5E);
    CHECK(cart.chr[0x1234] == 0x5E, "CHR write");

    /* +32. */
    wr(p, 0, 0x04);
    set_addr(p, 0x2000);
    wr(p, 7, 0x01);
    CHECK(p->v == 0x2020, "v=%04X after a +32 write", p->v);
    rd(p, 7);
    CHECK(p->v == 0x2040, "v=%04X after a +32 read", p->v);
    wr(p, 0, 0x00);

    /* $3000-$3EFF mirror the nametables (in the cartridge). */
    set_addr(p, 0x3105);
    rd(p, 7);
    CHECK(rd(p, 7) == 0xAA, "$3105 mirrors $2105");
    CHECK(rd(p, 7) == 0xBB, "$3106 mirrors $2106");

    /* A palette read is immediate; its top two bits are the I/O latch;
     * the buffer gets the nametable byte underneath ($3F05 -> $2F05). */
    cart.nt[nt_index(0x2F05)] = 0x5C;
    pal_set(p, 5, 0x2A);
    set_addr(p, 0x3F05);
    wr(p, 2, 0xC0); /* a write to the read-only register sets the latch */
    CHECK(rd(p, 7) == (0xC0 | 0x2A), "palette read");
    CHECK(p->read_buffer == 0x5C, "buffer under the palette: %02X", p->read_buffer);
    set_addr(p, 0x2000);
    CHECK(rd(p, 7) == 0x5C, "the buffered byte comes out next");

    /* v is 15 bits, the bus 14. */
    set_addr(p, 0x3FFF);
    wr(p, 7, 0x11);
    CHECK(p->v == 0x4000 && p->bus_addr == 0x0000, "v=%04X bus=%04X", p->v, p->bus_addr);
    CHECK(p->palette[0x1F] == 0x11, "$3FFF is palette entry $1F");
    wr(p, 7, 0x77);
    CHECK(cart.chr[0] == 0x77, "v=$4000 addresses $0000");
}

static void test_data_port_read_twice_in_a_row(void)
{
    NesPpu *p = fresh();
    cart.nt[nt_index(0x2000)] = 0x10;
    cart.nt[nt_index(0x2001)] = 0x20;
    set_addr(p, 0x2000);
    rd(p, 7); /* fills the buffer with $10 */
    /* Two reads on consecutive CPU cycles (an indexed read's dummy cycle):
     * the second does nothing and returns the latch. */
    uint8_t a = nes_ppu_read(p, 7);
    run(p, 3);
    uint8_t b = nes_ppu_read(p, 7);
    run(p, 3);
    CHECK(a == 0x10 && b == 0x10, "reads %02X %02X", a, b);
    CHECK(p->v == 0x2002, "only one of them increments: v=%04X", p->v);
    run(p, 3);
    CHECK(rd(p, 7) == 0x20, "a read two cycles later works");
}

static void test_palette_mirrors_and_greyscale(void)
{
    NesPpu *p = fresh();
    for (unsigned i = 0; i < 32; i++) pal_set(p, i, i + 0x20u);

    /* $3F10/$14/$18/$1C are $3F00/$04/$08/$0C: the later write wins. */
    for (unsigned i = 0; i < 32; i++) {
        const unsigned want = (i & 3u) ? i + 0x20u : (i | 0x10u) + 0x20u;
        CHECK(pal_get(p, i) == want, "palette[%02X]=%02X want %02X", i, pal_get(p, i), want);
    }
    pal_set(p, 0x00, 0x01);
    CHECK(pal_get(p, 0x10) == 0x01, "$3F00 writes through to $3F10");
    pal_set(p, 0x14, 0x02);
    CHECK(pal_get(p, 0x04) == 0x02, "$3F14 writes through to $3F04");
    pal_set(p, 0x18, 0x03);
    CHECK(pal_get(p, 0x08) == 0x03, "$3F18 -> $3F08");
    pal_set(p, 0x0C, 0x04);
    CHECK(pal_get(p, 0x1C) == 0x04, "$3F0C -> $3F1C");
    pal_set(p, 0x11, 0x05);
    CHECK(pal_get(p, 0x01) == 0x21, "$3F11 is separate from $3F01");

    /* The 32 entries repeat up to $3FFF. */
    set_addr(p, 0x3FE5);
    CHECK((rd(p, 7) & 0x3Fu) == 0x25, "$3FE5 mirrors $3F05");

    /* Six bits. */
    pal_set(p, 0x02, 0xFF);
    CHECK(p->palette[2] == 0x3F, "stored %02X", p->palette[2]);

    /* Greyscale masks what is read; the RAM keeps the colour. */
    pal_set(p, 0x03, 0x2B);
    wr(p, 1, 0x01);
    CHECK(pal_get(p, 0x03) == 0x20, "greyscale read %02X", pal_get(p, 0x03));
    wr(p, 1, 0x00);
    CHECK(pal_get(p, 0x03) == 0x2B, "colour read %02X", pal_get(p, 0x03));
}

static void test_oam_port(void)
{
    NesPpu *p = fresh();

    wr(p, 3, 0x10);
    wr(p, 4, 0x11);
    wr(p, 4, 0x22);
    wr(p, 4, 0xFF);
    wr(p, 4, 0x44);
    CHECK(p->oam_addr == 0x14, "OAMADDR increments on write: %02X", p->oam_addr);
    wr(p, 3, 0x10);
    CHECK(rd(p, 4) == 0x11 && rd(p, 4) == 0x11, "reads do not increment");
    wr(p, 3, 0x12);
    CHECK(rd(p, 4) == 0xE3, "attribute bits 2-4 read 0: %02X", rd(p, 4));
    wr(p, 3, 0x13);
    CHECK(rd(p, 4) == 0x44, "X byte");
    wr(p, 3, 0xFF);
    wr(p, 4, 0x5A);
    CHECK(p->oam_addr == 0x00 && p->oam[0xFF] == 0x5A, "OAMADDR wraps");

    /* During rendering. */
    oam_hide_all(p);
    wr(p, 3, 0x00);
    wr(p, 1, 0x18);
    run_to(p, 10, 30);
    CHECK(nes_ppu_read(p, 4) == 0xFF, "dots 1-64 read $FF");
    run_to(p, 10, 70);
    nes_ppu_write(p, 3, 0x21);
    uint8_t copy[256];
    memcpy(copy, p->oam, 256);
    nes_ppu_write(p, 4, 0x99);
    CHECK(p->oam_addr == 0x24, "a write during rendering steps the sprite number only: %02X", p->oam_addr);
    CHECK(!memcmp(copy, p->oam, 256), "and writes nothing");
    run_to(p, 10, 256);
    CHECK(p->oam_addr != 0, "evaluation is still counting at dot 256");
    for (unsigned d = 257; d <= 320; d++) {
        run_to(p, 10, d);
        CHECK(p->oam_addr == 0, "OAMADDR is 0 at dot %u", d);
        nes_ppu_write(p, 3, 0x40);
    }

    /* The pre-render line does it too, though it evaluates nothing. */
    run_to(p, 261, 100);
    nes_ppu_write(p, 3, 0x40);
    run_to(p, 261, 256);
    CHECK(p->oam_addr == 0x40, "pre-render line, dot 256: %02X", p->oam_addr);
    run(p, 1);
    CHECK(p->oam_addr == 0x00, "pre-render line, dot 257: %02X", p->oam_addr);

    /* With rendering off nothing touches it. */
    wr(p, 1, 0x00);
    wr(p, 3, 0x40);
    frames(p, 2);
    CHECK(p->oam_addr == 0x40, "rendering off: %02X", p->oam_addr);
}

static void test_oam_addr_copy_when_rendering_starts(void)
{
    NesPpu *p = fresh();
    for (unsigned i = 0; i < 256; i++) p->oam[i] = (uint8_t)i;
    run_to(p, 250, 0);
    wr(p, 3, 0x23);
    wr(p, 1, 0x18);
    run_to(p, 261, 9);
    for (unsigned i = 0; i < 8; i++) CHECK(p->oam[i] == 0x20 + i, "oam[%u]=%02X", i, p->oam[i]);
    CHECK(p->oam[8] == 8, "only eight bytes are copied");

    /* Not with OAMADDR below 8. */
    p = fresh();
    for (unsigned i = 0; i < 256; i++) p->oam[i] = (uint8_t)i;
    run_to(p, 250, 0);
    wr(p, 3, 0x07);
    wr(p, 1, 0x18);
    run_to(p, 261, 9);
    for (unsigned i = 0; i < 8; i++) CHECK(p->oam[i] == i, "oam[%u]=%02X", i, p->oam[i]);
}

static void test_io_latch_open_bus_and_decay(void)
{
    NesPpu *p = fresh();
    static const unsigned write_only[] = { 0, 1, 3, 5, 6 };

    wr(p, 3, 0xA5);
    for (unsigned i = 0; i < 5; i++) CHECK(rd(p, write_only[i]) == 0xA5, "reg %u reads the latch", write_only[i]);
    CHECK(rd(p, 2) == 0x05, "$2002: low five bits are the latch, status is clear");
    CHECK(rd(p, 0) == 0x05, "the $2002 read put its value on the latch");

    /* A bit that nothing refreshes reads 0 after about 600 ms. */
    wr(p, 3, 0xFF);
    frames(p, 10);
    CHECK(rd(p, 5) == 0xFF, "still held after 10 frames");
    frames(p, 40);
    CHECK(rd(p, 5) == 0x00, "decayed after 50 frames");

    /* $2002 drives only bits 7-5, so the others keep decaying. */
    wr(p, 3, 0xFF);
    frames(p, 20);
    CHECK(rd(p, 2) == 0x1F, "status clear, latch bits still set");
    frames(p, 20);
    CHECK(rd(p, 5) == 0x00, "the low bits were not refreshed by the $2002 read");

    /* $2004 drives all eight. */
    p->oam[0] = 0x5A;
    wr(p, 3, 0x00);
    frames(p, 30);
    CHECK(rd(p, 4) == 0x5A, "OAM read");
    frames(p, 30);
    CHECK(rd(p, 0) == 0x5A, "refreshed by the OAM read 30 frames ago");
    frames(p, 40);
    CHECK(rd(p, 0) == 0x00, "decayed");
}

static void test_reset_and_warmup(void)
{
    NesPpu *p = fresh();
    Cart *owner = (Cart *)p->user;

    pal_set(p, 1, 0x2C);
    set_addr(p, 0x2345);
    wr(p, 0, 0x9F);
    wr(p, 1, 0xE7); /* everything but the two rendering enables */
    wr(p, 3, 0x33);
    wr(p, 5, 0x7F);
    p->status = 0xE0;
    p->read_buffer = 0x44;
    run_to(p, 100, 50);
    const uint16_t v = p->v;

    nes_ppu_reset(p);
    CHECK(p->ctrl == 0 && p->mask == 0, "PPUCTRL/PPUMASK cleared");
    CHECK(p->t == 0 && p->fine_x == 0 && p->w == 0, "scroll latch cleared: t=%04X x=%u w=%u", p->t, p->fine_x, p->w);
    CHECK(p->read_buffer == 0, "read buffer cleared");
    CHECK(p->v == v, "v survives: %04X", p->v);
    CHECK(p->status == 0xE0 && p->oam_addr == 0x33 && p->palette[1] == 0x2C, "status, OAMADDR and palette survive");
    CHECK(p->scanline == 0 && p->dot == 0 && !p->odd_frame, "position (%u, %u)", p->scanline, p->dot);
    CHECK(p->user == owner, "user kept");
    CHECK(p->warmup_dots == 0, "no warm-up unless the machine asks");

    /* The warm-up, when the machine enables it. */
    p->warmup_dots = 30;
    nes_ppu_write(p, 0, 0x80);
    nes_ppu_write(p, 1, 0x1E);
    nes_ppu_write(p, 5, 0xFF);
    nes_ppu_write(p, 6, 0x3F);
    CHECK(p->ctrl == 0 && p->mask == 0 && p->t == 0 && p->w == 0, "writes ignored during warm-up");
    CHECK(nes_ppu_read(p, 0) == 0x3F, "but they reach the latch");
    nes_ppu_write(p, 3, 0x12);
    CHECK(p->oam_addr == 0x12, "OAMADDR is not affected");
    run(p, 29);
    nes_ppu_write(p, 0, 0x80);
    CHECK(p->ctrl == 0, "still ignored after 29 dots");
    run(p, 1);
    nes_ppu_write(p, 0, 0x80);
    CHECK(p->ctrl == 0x80, "accepted after 30 dots");

    /* Power keeps the owner and loads the timing model. */
    nes_ppu_power(p);
    CHECK(p->user == owner && p->lines == 262 && p->vblank_line == 241 && p->odd_skip, "power state");
    CHECK(p->scanline == 0 && p->dot == 0 && p->frame_count == 0 && !p->frame_ready, "power position");
}

/* ----------------------------------------------------------------- timing */

static void test_vblank_set_and_clear_dots(void)
{
    NesPpu *p = fresh();

    run_to(p, 239, 340);
    CHECK(!p->frame_ready && p->frame_count == 0, "no picture yet");
    run(p, 1);
    CHECK(p->frame_ready && p->frame_count == 1 && p->scanline == 240 && p->dot == 0, "frame_ready at (240, 0)");

    run_to(p, 241, 0);
    CHECK(!(p->status & 0x80), "clear at (241, 0)");
    run(p, 1);
    CHECK(p->status & 0x80, "set at (241, 1)");

    p->status |= 0x60; /* sprite 0 hit and overflow */
    run_to(p, 261, 0);
    CHECK(p->status == 0xE0, "all three still set at (261, 0): %02X", p->status);
    run(p, 1);
    CHECK(p->status == 0x00, "all three cleared at (261, 1): %02X", p->status);

    /* Reading clears the flag only. */
    run_to(p, 245, 7);
    p->status |= 0x60;
    CHECK(nes_ppu_read(p, 2) == 0xE0, "read");
    CHECK(p->status == 0x60, "after read: %02X", p->status);
    CHECK(nes_ppu_read(p, 2) == 0x60, "second read");
}

static void test_status_read_race(void)
{
    NesPpu *p = fresh();
    wr(p, 0, 0x80); /* NMI enabled */

    /* Two dots before: nothing special. */
    run_to(p, 240, 340);
    CHECK(!(nes_ppu_read(p, 2) & 0x80), "read at (240, 340) is clear");
    run_to(p, 241, 1);
    CHECK((p->status & 0x80) && nes_ppu_nmi_line(p), "the flag and NMI happen normally");

    /* The dot before: reads clear, and no flag or NMI this frame. */
    run_to(p, 241, 0);
    CHECK(!(nes_ppu_read(p, 2) & 0x80), "read at (241, 0) is clear");
    bool nmi = false, flag = false;
    for (unsigned i = 0; i < 20u * 341u; i++) {
        run(p, 1);
        nmi = nmi || nes_ppu_nmi_line(p);
        flag = flag || (p->status & 0x80);
    }
    CHECK(!flag && !nmi, "flag %d, NMI %d after a read on the dot before", flag, nmi);
    run_to(p, 241, 1);
    CHECK(p->status & 0x80, "the next frame is normal again");

    /* The same dot: reads set, clears it, and the line drops with it. */
    run_to(p, 241, 1);
    CHECK(nes_ppu_nmi_line(p), "line high at (241, 1)");
    CHECK(nes_ppu_read(p, 2) & 0x80, "read at (241, 1) is set");
    CHECK(!(p->status & 0x80) && !nes_ppu_nmi_line(p), "cleared, NMI withdrawn");
    run(p, 3000);
    CHECK(!(p->status & 0x80) && !nes_ppu_nmi_line(p), "and it stays clear");

    /* One dot later: the same. */
    run_to(p, 241, 2);
    CHECK(nes_ppu_read(p, 2) & 0x80, "read at (241, 2) is set");
    CHECK(!(p->status & 0x80) && !nes_ppu_nmi_line(p), "cleared, NMI withdrawn");

    /* The clear: set through (261, 0), clear from (261, 1). */
    run_to(p, 0, 0);
    run_to(p, 261, 0);
    CHECK(nes_ppu_read(p, 2) & 0x80, "read at (261, 0) is set");
    run_to(p, 261, 1);
    CHECK(!(nes_ppu_read(p, 2) & 0x80), "read at (261, 1) is clear");
}

static void test_nmi_line_level(void)
{
    NesPpu *p = fresh();

    run_to(p, 241, 1);
    CHECK(!nes_ppu_nmi_line(p), "flag set but NMI disabled");
    run_to(p, 250, 0);
    nes_ppu_write(p, 0, 0x80);
    CHECK(nes_ppu_nmi_line(p), "enabling during VBlank raises the line");
    nes_ppu_write(p, 0, 0x00);
    CHECK(!nes_ppu_nmi_line(p), "disabling lowers it");
    nes_ppu_write(p, 0, 0x80);
    CHECK(nes_ppu_nmi_line(p), "and enabling again gives another edge");
    run_to(p, 261, 0);
    CHECK(nes_ppu_nmi_line(p), "high until the flag is cleared");
    run(p, 1);
    CHECK(!nes_ppu_nmi_line(p), "low at (261, 1)");

    run_to(p, 241, 0);
    CHECK(!nes_ppu_nmi_line(p), "low at (241, 0)");
    run(p, 1);
    CHECK(nes_ppu_nmi_line(p), "high at (241, 1)");
    nes_ppu_read(p, 2);
    CHECK(!nes_ppu_nmi_line(p), "a $2002 read lowers it");
    nes_ppu_write(p, 0, 0x00);
    nes_ppu_write(p, 0, 0x80);
    CHECK(!nes_ppu_nmi_line(p), "no flag, no NMI, whatever the enable does");
}

static void test_frame_length_even_odd(void)
{
    static const uint8_t masks[] = { 0x08, 0x10, 0x18 };
    NesPpu *p = fresh();

    /* Rendering off: every frame is 341 * 262 dots; the parity still
     * toggles. */
    frame(p);
    for (unsigned i = 0; i < 4; i++) {
        const bool odd = p->odd_frame;
        const uint32_t n = frame(p);
        CHECK(n == 89342u, "rendering off: %u dots", (unsigned)n);
        CHECK(p->odd_frame != odd, "parity toggles");
    }

    /* Rendering on (background, sprites, or both): the pre-render line of
     * an odd frame loses a dot. frame() measures from (240, 0), so the
     * pre-render line inside it belongs to the frame current at its start. */
    for (unsigned m = 0; m < 3; m++) {
        p = fresh();
        wr(p, 1, masks[m]);
        frame(p);
        for (unsigned i = 0; i < 6; i++) {
            const bool odd = p->odd_frame;
            const uint32_t n = frame(p);
            CHECK(n == (odd ? 89341u : 89342u), "mask %02X, %s frame: %u dots", masks[m], odd ? "odd" : "even",
                  (unsigned)n);
        }
    }

    /* The jump is from (261, 339) to (0, 0). */
    p = fresh();
    wr(p, 1, 0x08);
    frame(p);
    if (!p->odd_frame) frame(p);
    CHECK(p->odd_frame, "on an odd frame");
    run_to(p, 261, 339);
    run(p, 1);
    CHECK(p->scanline == 0 && p->dot == 0 && !p->odd_frame, "odd: (261, 339) -> (%u, %u)", p->scanline, p->dot);
    run_to(p, 261, 339);
    run(p, 1);
    CHECK(p->scanline == 261 && p->dot == 340, "even: (261, 339) -> (%u, %u)", p->scanline, p->dot);

    /* Rendering switched off before that dot: nothing is skipped. */
    run_to(p, 261, 338);
    CHECK(p->odd_frame, "odd again");
    nes_ppu_write(p, 1, 0x00);
    run(p, 2);
    CHECK(p->scanline == 261 && p->dot == 340, "disabled at dot 338: (%u, %u)", p->scanline, p->dot);
}

static void test_dots_until_event_never_overestimates(void)
{
    static const uint8_t masks[] = { 0x00, 0x18 };
    for (unsigned m = 0; m < 2; m++) {
        NesPpu *p = fresh();
        wr(p, 1, masks[m]);
        rng_state = 12345u + m;
        unsigned events = 0, short_by_one = 0;
        uint64_t total = 0;
        while (total < 6u * 89342u) {
            const uint32_t promised = nes_ppu_dots_until_event(p);
            CHECK(promised >= 1, "returned 0 at (%u, %u)", p->scanline, p->dot);
            if (!promised) return;
            /* Sometimes stop short and ask again from wherever that is. */
            uint32_t steps = promised;
            if (rng() & 1u) steps = 1u + rng() % promised;
            for (uint32_t i = 0; i < steps; i++) {
                p->frame_ready = false;
                const bool vbl = (p->status & 0x80) != 0;
                run(p, 1);
                total++;
                const bool event = p->frame_ready || (!vbl && (p->status & 0x80));
                if (i + 1 < promised) {
                    CHECK(!event, "event %u dots after (promised %u)", (unsigned)(i + 1), (unsigned)promised);
                } else if (event) {
                    events++;
                } else {
                    /* An underestimate: allowed only by one dot. */
                    short_by_one++;
                    CHECK(nes_ppu_dots_until_event(p) == 1, "short by more than one dot at (%u, %u)", p->scanline,
                          p->dot);
                }
            }
        }
        CHECK(events >= 8, "mask %02X: saw %u events", masks[m], events);
        /* With rendering on the skipped dot is predicted exactly. */
        if (masks[m]) CHECK(short_by_one == 0, "mask %02X: %u underestimates", masks[m], short_by_one);
    }
}

/* -------------------------------------------------------------------- bus */

static uint32_t event_time(const BusEvent *e) { return (uint32_t)e->scanline * 341u + e->dot; }

/* Rises of A12 that follow at least `quiet` dots of A12 low, the way an
 * MMC3 filters them. `a12` and `low_since` carry the state in. */
static unsigned filtered_rises(const BusEvent *log, size_t count, unsigned quiet, bool a12, uint32_t low_since,
                               unsigned dots[], unsigned max)
{
    unsigned n = 0;
    for (size_t i = 0; i < count; i++) {
        const bool now = (log[i].addr & 0x1000u) != 0;
        const uint32_t t = event_time(&log[i]);
        if (now && !a12 && t - low_since >= quiet && n < max) dots[n++] = log[i].dot;
        if (!now && a12) low_since = t;
        a12 = now;
    }
    return n;
}

static void fetch_scene(NesPpu *p, unsigned ctrl)
{
    for (unsigned i = 0; i < sizeof cart.nt; i++) cart.nt[i] = (uint8_t)(i * 7u + 3u);
    oam_hide_all(p);
    wr(p, 0, ctrl);
    wr(p, 5, 0);
    wr(p, 5, 0);
    wr(p, 3, 0);
    wr(p, 1, 0x18);
    render(p);
}

static void test_scanline_fetch_sequence(void)
{
    NesPpu *p = fresh();
    fetch_scene(p, 0x08); /* background $0000, sprites $1000 */

    /* Scanline 10: coarse Y 1, fine Y 2. */
    run_to(p, 9, 340);
    cart.logging = true;
    cart.count = 0;
    run(p, 341);
    cart.logging = false;
    CHECK(p->scanline == 10 && p->dot == 340, "position (%u, %u)", p->scanline, p->dot);

    BusEvent want[200];
    size_t n = 0;
#define WANT(d, a, k)                    \
    do {                                 \
        want[n].scanline = 10;           \
        want[n].dot = (uint16_t)(d);     \
        want[n].addr = (uint16_t)(a);    \
        want[n].kind = (k);              \
        n++;                             \
    } while (0)

    /* Dot 0: the pattern address of the tile about to be fetched (the
     * nametable byte read at dots 337/339 of the line before). */
    WANT(0, cart.nt[nt_index(0x2022)] * 16u + 2u, 'A');
    /* Dots 1-256: tiles 2..33 of the row. */
    for (unsigned k = 0; k < 32; k++) {
        const unsigned c = k + 2u, base = c >= 32u ? 0x2400u : 0x2000u, cx = c & 31u;
        const unsigned nt = base + 32u + cx;
        const unsigned tile = cart.nt[nt_index(nt)];
        WANT(1 + 8 * k, nt, 'R');
        WANT(3 + 8 * k, base + 0x3C0u + (cx >> 2), 'R');
        WANT(5 + 8 * k, tile * 16u + 2u, 'R');
        WANT(7 + 8 * k, tile * 16u + 10u, 'R');
    }
    /* Dots 257-320: eight empty sprite slots. Two garbage nametable
     * fetches, then tile $FF of the sprite table. An empty slot holds
     * $FF in every byte, so the row is (10 - 255) & 7 = 3, flipped
     * vertically by the attribute byte: 4. */
    for (unsigned s = 0; s < 8; s++) {
        WANT(257 + 8 * s, s == 0 ? 0x2022u : 0x2020u, 'R');
        WANT(259 + 8 * s, 0x2020u, 'R');
        WANT(261 + 8 * s, 0x1FF4u, 'R');
        WANT(263 + 8 * s, 0x1FFCu, 'R');
    }
    /* Dots 321-336: the first two tiles of scanline 11 (fine Y 3). */
    for (unsigned k = 0; k < 2; k++) {
        const unsigned tile = cart.nt[nt_index(0x2020u + k)];
        WANT(321 + 8 * k, 0x2020u + k, 'R');
        WANT(323 + 8 * k, 0x23C0u, 'R');
        WANT(325 + 8 * k, tile * 16u + 3u, 'R');
        WANT(327 + 8 * k, tile * 16u + 11u, 'R');
    }
    /* Dots 337 and 339: the third tile's nametable byte, twice. */
    WANT(337, 0x2022u, 'R');
    WANT(339, 0x2022u, 'R');
#undef WANT

    CHECK(cart.count == n, "%u bus events, want %u", (unsigned)cart.count, (unsigned)n);
    for (size_t i = 0; i < n && i < cart.count; i++) {
        const BusEvent *e = &cart.log[i];
        CHECK(e->scanline == 10 && e->dot == want[i].dot && e->addr == want[i].addr && e->kind == want[i].kind,
              "event %u: %c $%04X at dot %u, want %c $%04X at dot %u", (unsigned)i, e->kind, e->addr, e->dot,
              want[i].kind, want[i].addr, want[i].dot);
    }

    /* The picture is over at (240, 1): the pins go back to v. Nothing is
     * accessed from there until the pre-render line, whose dot 0 shows no
     * pattern address (it follows a VBlank line, which fetched nothing):
     * blargg's mmc3_test_2/4-scanline_timing has the frame's first A12
     * rise with the background at $1000 at the pre-render line's first
     * pattern fetch, 256 dots before the sprite fetch's. */
    run_to(p, 240, 0);
    cart.logging = true;
    cart.count = 0;
    run_to(p, 261, 0);
    cart.logging = false;
    CHECK(cart.count == 1, "%u events between the picture and the pre-render line", (unsigned)cart.count);
    CHECK(cart.log[0].kind == 'A' && cart.log[0].scanline == 240 && cart.log[0].dot == 1 &&
              cart.log[0].addr == (p->v & 0x3FFFu),
          "bus returns to v at (240, 1): %c $%04X at (%u, %u)", cart.log[0].kind, cart.log[0].addr,
          cart.log[0].scanline, cart.log[0].dot);

    /* The pre-render line makes the same accesses as a picture line. */
    cart.logging = true;
    cart.count = 0;
    run_to(p, 261, 340);
    cart.logging = false;
    CHECK(cart.count == 170, "pre-render line: %u reads", (unsigned)cart.count);
}

static void test_a12_rises_once_per_line(void)
{
    NesPpu *p = fresh();
    unsigned dots[64];

    /* Background at $0000, sprites at $1000: A12 is low for the whole
     * background part of the line and rises at the first sprite pattern
     * fetch, dot 261 (its address appears there; the wiki's MMC3 page
     * calls this "PPU cycle 260"). The other seven sprite slots follow
     * four dots of low each and are filtered out. */
    fetch_scene(p, 0x08);
    run_to(p, 19, 340);
    cart.logging = true;
    cart.count = 0;
    run(p, 341 * 3);
    cart.logging = false;
    unsigned n = filtered_rises(cart.log, cart.count, 12, false, 0, dots, 64);
    CHECK(n == 3, "3 lines: %u filtered rises", n);
    for (unsigned i = 0; i < n; i++) CHECK(dots[i] == 261, "rise %u at dot %u", i, dots[i]);
    n = filtered_rises(cart.log, cart.count, 0, false, 0, dots, 64);
    CHECK(n == 24, "unfiltered: one per sprite slot, %u in 3 lines", n);

    /* Swapped: the long low stretch is the sprite part, and the rise is
     * the first background pattern fetch after it, dot 325 (the wiki's
     * "cycle 324"), on the line before the one it counts. During the
     * background fetches A12 rises every 8 dots, first at dot 0 (the
     * idle dot shows the pattern address) and then 5, 13, .. */
    p = fresh();
    fetch_scene(p, 0x10);
    run_to(p, 19, 340);
    cart.logging = true;
    cart.count = 0;
    run(p, 341 * 3);
    cart.logging = false;
    n = filtered_rises(cart.log, cart.count, 12, true, 0, dots, 64);
    CHECK(n == 3, "swapped: %u filtered rises", n);
    for (unsigned i = 0; i < n; i++) CHECK(dots[i] == 325, "swapped: rise %u at dot %u", i, dots[i]);
    n = filtered_rises(cart.log, cart.count, 0, false, 0, dots, 64);
    CHECK(n >= 35 && dots[0] == 0 && dots[1] == 5 && dots[2] == 13, "swapped, unfiltered: %u, first at %u, %u, %u", n,
          dots[0], dots[1], dots[2]);

    /* An odd frame's skipped dot replaces dot 0 of scanline 0, so there
     * is no pattern address there. */
    wr(p, 1, 0x18);
    frame(p);
    if (!p->odd_frame) frame(p);
    run_to(p, 261, 336);
    cart.logging = true;
    cart.count = 0;
    run_to(p, 0, 4);
    cart.logging = false;
    CHECK(cart.count == 4, "odd frame, around the skip: %u events", (unsigned)cart.count);
    CHECK(cart.log[2].scanline == 0 && cart.log[2].dot == 1 && cart.log[2].kind == 'R', "no event at (0, 0)");
}

static void test_address_bus_reports(void)
{
    NesPpu *p = fresh();

    /* The second $2006 write: the pins change three dots later. */
    cart.logging = true;
    nes_ppu_write(p, 6, 0x10);
    nes_ppu_write(p, 6, 0x00);
    const unsigned at = p->dot;
    run(p, 6);
    CHECK(cart.count == 1 && cart.log[0].kind == 'A' && cart.log[0].addr == 0x1000 && cart.log[0].dot == at + 3,
          "$2006: %u events, %c $%04X at dot %u (written at %u)", (unsigned)cart.count, cart.log[0].kind,
          cart.log[0].addr, cart.log[0].dot, at);

    /* $2007: the access, then the incremented address. */
    cart.count = 0;
    nes_ppu_read(p, 7);
    CHECK(cart.count == 2 && cart.log[0].kind == 'R' && cart.log[0].addr == 0x1000 && cart.log[1].kind == 'A' &&
              cart.log[1].addr == 0x1001,
          "$2007 read: %u events", (unsigned)cart.count);
    run(p, 9);
    cart.count = 0;
    nes_ppu_write(p, 7, 0x00);
    CHECK(cart.count == 2 && cart.log[0].kind == 'W' && cart.log[0].addr == 0x1001 && cart.log[1].kind == 'A' &&
              cart.log[1].addr == 0x1002,
          "$2007 write: %u events", (unsigned)cart.count);

    /* The palette is inside the PPU: a write there reaches no cartridge,
     * a read still reads the bus, and A12/A13 show the real address. */
    cart.count = 0;
    set_addr(p, 0x3F00);
    CHECK(cart.count == 1 && cart.log[0].kind == 'A' && cart.log[0].addr == 0x3F00, "$2006 to the palette");
    cart.count = 0;
    nes_ppu_write(p, 7, 0x21);
    CHECK(cart.count == 1 && cart.log[0].kind == 'A' && cart.log[0].addr == 0x3F01, "palette write: only the increment");
    run(p, 9);
    cart.count = 0;
    nes_ppu_read(p, 7);
    CHECK(cart.count == 2 && cart.log[0].kind == 'R' && cart.log[0].addr == 0x3F01, "palette read reads the bus");

    /* Rendering switched off in mid-line: the pins return to v. */
    set_addr(p, 0x1ABC);
    wr(p, 1, 0x18);
    run_to(p, 50, 100);
    const uint16_t v = p->v;
    cart.count = 0;
    nes_ppu_write(p, 1, 0x00);
    CHECK(cart.count == 1 && cart.log[0].kind == 'A' && cart.log[0].addr == (v & 0x3FFFu), "PPUMASK off: %c $%04X, v=%04X",
          cart.log[0].kind, cart.log[0].addr, v);
    cart.logging = false;
}

static void test_data_port_during_rendering(void)
{
    /* A $2007 access while rendering increments coarse X and Y together.
     * That also exercises every wrap of the two increments. */
    static const uint16_t cases[][2] = {
        { 0x0000, 0x1001 }, /* coarse X + 1, fine Y + 1 */
        { 0x001F, 0x1400 }, /* coarse X wraps and switches nametable */
        { 0x041F, 0x1000 },
        { 0x7000, 0x0021 }, /* fine Y wraps into coarse Y */
        { 0x73A0, 0x0801 }, /* row 29 wraps and switches nametable */
        { 0x7BA0, 0x0001 },
        { 0x73C0, 0x03E1 }, /* row 30 just counts on */
        { 0x73E0, 0x0001 }, /* row 31 wraps without switching */
        { 0x7FFF, 0x0800 }, /* row 31 with coarse X 31: X nametable toggles, Y does not */
    };
    NesPpu *p = fresh();
    wr(p, 1, 0x18);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        run_to(p, 100 + i, 300); /* no scroll update is due here */
        p->v = cases[i][0];
        nes_ppu_read(p, 7);
        CHECK(p->v == cases[i][1], "read: v %04X -> %04X, want %04X", cases[i][0], p->v, cases[i][1]);
        run(p, 9);
        p->v = cases[i][0];
        nes_ppu_write(p, 7, 0x00);
        CHECK(p->v == cases[i][1], "write: v %04X -> %04X, want %04X", cases[i][0], p->v, cases[i][1]);
    }

    /* Outside the picture it is the plain +1, rendering enabled or not. */
    run_to(p, 245, 0);
    p->v = 0x2000;
    nes_ppu_read(p, 7);
    CHECK(p->v == 0x2001, "VBlank: v=%04X", p->v);
}

/* ------------------------------------------------------------- background */

static void test_background_tile_scroll_fine_x(void)
{
    /* One tile with a diagonal line at column 2, row 1 of nametable 0, and
     * one at column 0, row 3 of nametable 1 (vertical mirroring). */
    static const uint8_t lo[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
    static const uint8_t zero[8] = { 0 };
    NesPpu *p = fresh();
    standard_palette(p);
    tile_rows(0, 1, lo, zero);
    nt_put(0, 2, 1, 1);
    nt_put(1, 0, 3, 1);

    /* Every fine X, across a coarse X step. */
    for (unsigned sx = 0; sx < 17; sx++) {
        RefScene s = { sx, 0, 0x00, 0x0A };
        set_scene(p, &s);
        render(p);
        unsigned lit = 0;
        for (unsigned y = 0; y < 240; y++)
            for (unsigned x = 0; x < 256; x++) {
                /* The first tile's diagonal starts at (16 - sx, 8); the
                 * second one's at (256 - sx, 24). */
                const bool a = y >= 8 && y < 16 && x + sx == 16u + (y - 8u);
                const bool b = y >= 24 && y < 32 && x + sx == 256u + (y - 24u);
                const unsigned want = (a || b) ? bgc(0, 1) : (unsigned)BACKDROP;
                if (px(p, x, y) != want) CHECK(0, "scroll %u: (%u, %u)=%02X want %02X", sx, x, y, px(p, x, y), want);
                lit += px(p, x, y) != BACKDROP;
            }
        CHECK(lit == 8u + (sx < 8u ? sx : 8u), "scroll %u: %u lit pixels", sx, lit);
    }

    /* Vertical scroll, and both nametable select bits. */
    static const unsigned positions[][2] = { { 0, 5 }, { 3, 8 }, { 250, 9 }, { 256, 0 }, { 300, 231 }, { 5, 240 }, { 511, 479 } };
    for (unsigned i = 0; i < sizeof positions / sizeof positions[0]; i++) {
        RefScene s = { positions[i][0], positions[i][1], 0x00, 0x0A };
        char what[48];
        snprintf(what, sizeof what, "scroll (%u, %u)", s.scroll_x, s.scroll_y);
        set_scene(p, &s);
        render(p);
        compare_with_reference(p, &s, what);
    }

    /* Every fine Y, across a coarse Y step. */
    for (unsigned sy = 0; sy < 17; sy++) {
        RefScene s = { 2, sy, 0x00, 0x0A };
        set_scene(p, &s);
        render(p);
        for (unsigned i = 0; i < 8; i++) {
            /* The first tile's diagonal starts at (14, 8 - sy). */
            if (8u + i < sy) continue;
            CHECK(px(p, 14 + i, 8 + i - sy) == bgc(0, 1), "scroll Y %u: diagonal pixel %u is %02X", sy, i,
                  px(p, 14 + i, 8 + i - sy));
        }
        compare_with_reference(p, &s, "fine Y");
    }
}

static void test_attribute_quadrants(void)
{
    NesPpu *p = fresh();
    standard_palette(p);
    tile_solid(0, 1, 1);
    tile_solid(0, 2, 3);
    for (unsigned i = 0; i < 0x3C0; i++) cart.nt[i] = 1;
    nt_put(0, 9, 9, 2);
    /* Bits 1-0 top left, 3-2 top right, 5-4 bottom left, 7-6 bottom right. */
    at_put(0, 0, 0, 0xE4); /* palettes 0, 1, 2, 3 */
    at_put(0, 1, 0, 0x1B); /* palettes 3, 2, 1, 0 */
    at_put(0, 2, 2, 0x6C); /* block at tiles (8..11, 8..11): 0, 3, 2, 1 */

    RefScene s = { 0, 0, 0x00, 0x0A };
    set_scene(p, &s);
    render(p);

    static const struct {
        unsigned x, y, pal;
    } want[] = {
        { 0, 0, 0 },   { 15, 15, 0 }, { 16, 0, 1 },  { 31, 15, 1 }, { 0, 16, 2 },  { 15, 31, 2 }, { 16, 16, 3 },
        { 31, 31, 3 }, { 32, 0, 3 },  { 48, 0, 2 },  { 32, 16, 1 }, { 48, 16, 0 }, { 64, 64, 0 }, { 80, 64, 3 },
        { 64, 80, 2 }, { 80, 80, 1 }, { 95, 95, 1 }, { 96, 96, 0 },
    };
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
        CHECK(px(p, want[i].x, want[i].y) == bgc(want[i].pal, 1), "(%u, %u)=%02X want palette %u", want[i].x, want[i].y,
              px(p, want[i].x, want[i].y), want[i].pal);
    /* The tile at (9, 9) is in the bottom-left quadrant of its block. */
    CHECK(px(p, 72, 72) == bgc(0, 3) && px(p, 79, 79) == bgc(0, 3), "colour 3 tile: %02X", px(p, 72, 72));
    compare_with_reference(p, &s, "attributes");

    /* The quadrant follows the scroll, not the screen. */
    s.scroll_x = 13;
    s.scroll_y = 22;
    set_scene(p, &s);
    render(p);
    CHECK(px(p, 2, 0) == bgc(2, 1) && px(p, 3, 0) == bgc(3, 1), "scrolled: %02X %02X", px(p, 2, 0), px(p, 3, 0));
    compare_with_reference(p, &s, "attributes, scrolled");
}

static void test_left_column_clipping(void)
{
    NesPpu *p = fresh();
    standard_palette(p);
    tile_solid(0, 1, 1);
    tile_solid(1, 1, 2);
    for (unsigned i = 0; i < 0x3C0; i++) cart.nt[i] = 1;
    oam_hide_all(p);
    oam_put(p, 0, 99, 1, 0x00, 4); /* columns 4..11 of lines 100..107 */

    /* All four combinations of the two clip bits. */
    for (unsigned bits = 0; bits < 4; bits++) {
        RefScene s = { 0, 0, 0x08, (uint8_t)(0x18u | (bits << 1)) };
        const bool bg_left = bits & 1u, sprite_left = (bits & 2u) != 0;
        set_scene(p, &s);
        render(p);
        for (unsigned x = 0; x < 16; x++) {
            const unsigned bg = (x >= 8 || bg_left) ? bgc(0, 1) : (unsigned)BACKDROP;
            const bool sprite = x >= 4 && x < 12 && (x >= 8 || sprite_left);
            CHECK(px(p, x, 50) == bg, "mask %02X: background at x=%u is %02X", s.mask, x, px(p, x, 50));
            CHECK(px(p, x, 100) == (sprite ? spc(0, 2) : bg), "mask %02X: sprite line at x=%u is %02X", s.mask, x,
                  px(p, x, 100));
        }
    }
}

static void test_emphasis_and_greyscale_in_output(void)
{
    NesPpu *p = fresh();
    standard_palette(p);
    tile_solid(0, 1, 1);
    for (unsigned i = 0; i < 0x3C0; i++) cart.nt[i] = 1;
    at_put(0, 0, 0, 0x03); /* top-left 16x16 uses palette 3: colour $31 */

    /* Each emphasis bit, rendering on and off. */
    for (unsigned bit = 0; bit < 3; bit++) {
        RefScene s = { 0, 0, 0x00, (uint8_t)(0x0Au | (0x20u << bit)) };
        set_scene(p, &s);
        render(p);
        CHECK(px(p, 0, 0) == (bgc(3, 1) | (0x40u << bit)), "emphasis bit %u: %03X", bit, px(p, 0, 0));
        CHECK(px(p, 100, 100) == (bgc(0, 1) | (0x40u << bit)), "emphasis bit %u: %03X", bit, px(p, 100, 100));
        wr(p, 1, 0x20u << bit);
        render(p);
        CHECK(px(p, 0, 0) == (BACKDROP | (0x40u << bit)), "emphasis bit %u, rendering off: %03X", bit, px(p, 0, 0));
    }

    /* Greyscale keeps the brightness bits only; with all emphasis. */
    RefScene s = { 0, 0, 0x00, 0xEB };
    set_scene(p, &s);
    render(p);
    CHECK(px(p, 0, 0) == (0x30u | 0x1C0u), "greyscale + emphasis: %03X", px(p, 0, 0));
    CHECK(px(p, 100, 100) == (0x00u | 0x1C0u), "greyscale + emphasis: %03X", px(p, 100, 100));
}

static void test_rendering_disabled_backdrop(void)
{
    NesPpu *p = fresh();
    standard_palette(p);
    tile_solid(0, 1, 1);
    for (unsigned i = 0; i < 0x3C0; i++) cart.nt[i] = 1;

    render(p);
    unsigned bad = 0;
    for (unsigned i = 0; i < 240u * 256u; i++) bad += p->frame[i] != BACKDROP;
    CHECK(bad == 0, "rendering off: %u pixels are not the backdrop", bad);

    /* v pointing into the palette selects that entry instead. */
    set_addr(p, 0x3F16);
    render(p);
    bad = 0;
    for (unsigned i = 0; i < 240u * 256u; i++) bad += p->frame[i] != spc(1, 2);
    CHECK(bad == 0, "v=$3F16: %u pixels are not palette entry $16 (%02X)", bad, p->frame[0]);

    /* Switched off and on inside the picture: lines 100-119 show the
     * backdrop, and no bus access happens there. */
    set_addr(p, 0x0000);
    RefScene s = { 0, 0, 0x00, 0x0A };
    set_scene(p, &s);
    render(p);
    run_to(p, 99, 330);
    const uint64_t accesses = cart.total;
    nes_ppu_write(p, 1, 0x00);
    run_to(p, 119, 330);
    CHECK(cart.total == accesses + 1, "bus events while disabled (one: the pins return to v): %u",
          (unsigned)(cart.total - accesses));
    nes_ppu_write(p, 1, 0x0A);
    run_to(p, 240, 0);
    CHECK(px(p, 50, 99) == bgc(0, 1) && px(p, 50, 100) == BACKDROP && px(p, 50, 119) == BACKDROP, "lines 99/100/119: %02X %02X %02X",
          px(p, 50, 99), px(p, 50, 100), px(p, 50, 119));
    CHECK(px(p, 50, 121) == bgc(0, 1), "line 121: %02X", px(p, 50, 121));
}

/* ---------------------------------------------------------------- sprites */

static void test_sprites_8x8_flips_and_palettes(void)
{
    NesPpu *p = fresh();
    standard_palette(p);
    tile_l(1, 5);
    oam_hide_all(p);
    /* One sprite per flip combination, each with its own palette. */
    for (unsigned f = 0; f < 4; f++) oam_put(p, f, 30, 5, (f << 6) | f, 40 + 20 * f);

    RefScene s = { 0, 0, 0x08, 0x1E };
    set_scene(p, &s);
    render(p);
    for (unsigned f = 0; f < 4; f++)
        for (unsigned r = 0; r < 8; r++)
            for (unsigned c = 0; c < 8; c++) {
                const unsigned colour = l_colour((f & 1u) ? 7u - c : c, (f & 2u) ? 7u - r : r);
                const unsigned want = colour ? spc(f, colour) : (unsigned)BACKDROP;
                const unsigned x = 40 + 20 * f + c, y = 31 + r; /* a sprite is drawn one line below its Y */
                CHECK(px(p, x, y) == want, "flip %u: (%u, %u)=%02X want %02X", f, c, r, px(p, x, y), want);
            }
    CHECK(px(p, 40, 30) == BACKDROP && px(p, 40, 39) == BACKDROP, "nothing above or below");
    compare_with_reference(p, &s, "8x8 sprites");

    /* Sprites from pattern table 0 when PPUCTRL bit 3 is clear. */
    tile_solid(0, 5, 3);
    s.ctrl = 0x00;
    set_scene(p, &s);
    render(p);
    CHECK(px(p, 43, 34) == spc(0, 3), "sprite table 0: %02X", px(p, 43, 34));

    /* Y of 0 puts a sprite on lines 1-8; nothing can be on line 0. Y of
     * 239 and above is off the picture. */
    oam_hide_all(p);
    oam_put(p, 0, 0, 5, 0, 10);
    oam_put(p, 1, 239, 5, 0, 30);
    oam_put(p, 2, 232, 5, 0, 50);
    render(p);
    CHECK(px(p, 10, 0) == BACKDROP && px(p, 10, 1) == spc(0, 3) && px(p, 10, 8) == spc(0, 3) && px(p, 10, 9) == BACKDROP,
          "Y=0: %02X %02X %02X %02X", px(p, 10, 0), px(p, 10, 1), px(p, 10, 8), px(p, 10, 9));
    CHECK(px(p, 30, 239) == BACKDROP && px(p, 30, 0) == BACKDROP, "Y=239 is never shown");
    CHECK(px(p, 50, 233) == spc(0, 3) && px(p, 50, 239) == spc(0, 3), "Y=232 reaches the last line");

    /* A sprite at the right edge is cut off, not wrapped. */
    oam_hide_all(p);
    oam_put(p, 0, 50, 5, 0, 252);
    render(p);
    CHECK(px(p, 252, 51) == spc(0, 3) && px(p, 255, 51) == spc(0, 3) && px(p, 0, 51) == BACKDROP, "X=252");
}

static void test_sprites_8x16(void)
{
    static const uint8_t half[8] = { 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F };
    NesPpu *p = fresh();
    standard_palette(p);
    /* Tile number $03: table 1, tiles 2 (top) and 3 (bottom).
     * Tile number $02: table 0, tiles 2 and 3. */
    tile_l(1, 2);
    tile_rows(1, 3, half, half); /* right half, colour 3 */
    tile_solid(0, 2, 2);
    tile_solid(0, 3, 1);
    oam_hide_all(p);
    oam_put(p, 0, 60, 0x03, 0x00, 20);
    oam_put(p, 1, 60, 0x03, 0x80, 40); /* vertical flip */
    oam_put(p, 2, 60, 0x03, 0xC1, 60); /* both flips, palette 1 */
    oam_put(p, 3, 60, 0x02, 0x00, 80); /* the other table */

    /* PPUCTRL bit 3 must not matter for 8x16; try both. */
    for (unsigned table_bit = 0; table_bit < 2; table_bit++) {
        RefScene s = { 0, 0, (uint8_t)(0x20u | (table_bit << 3)), 0x1E };
        set_scene(p, &s);
        render(p);
        for (unsigned r = 0; r < 16; r++)
            for (unsigned c = 0; c < 8; c++) {
                unsigned colour = r < 8 ? l_colour(c, r) : (c >= 4 ? 3u : 0u);
                CHECK(px(p, 20 + c, 61 + r) == (colour ? spc(0, colour) : (unsigned)BACKDROP), "plain (%u, %u)=%02X", c, r,
                      px(p, 20 + c, 61 + r));
                /* Flipped vertically, the two tiles swap as well. */
                const unsigned fr = 15u - r;
                colour = fr < 8 ? l_colour(c, fr) : (c >= 4 ? 3u : 0u);
                CHECK(px(p, 40 + c, 61 + r) == (colour ? spc(0, colour) : (unsigned)BACKDROP), "v-flip (%u, %u)=%02X", c, r,
                      px(p, 40 + c, 61 + r));
                const unsigned fc = 7u - c;
                colour = fr < 8 ? l_colour(fc, fr) : (fc >= 4 ? 3u : 0u);
                CHECK(px(p, 60 + c, 61 + r) == (colour ? spc(1, colour) : (unsigned)BACKDROP), "both flips (%u, %u)=%02X", c,
                      r, px(p, 60 + c, 61 + r));
                CHECK(px(p, 80 + c, 61 + r) == spc(0, r < 8 ? 2u : 1u), "table 0 (%u, %u)=%02X", c, r,
                      px(p, 80 + c, 61 + r));
            }
        CHECK(px(p, 20, 60) == BACKDROP && px(p, 20, 77) == BACKDROP, "16 lines tall");
        compare_with_reference(p, &s, "8x16 sprites");
    }
}

static void test_sprite_priority(void)
{
    NesPpu *p = fresh();
    standard_palette(p);
    tile_solid(0, 1, 1);
    tile_solid(1, 6, 3);
    /* Opaque background at x 80..103, y 80..111. */
    for (unsigned row = 10; row < 14; row++)
        for (unsigned col = 10; col < 13; col++) nt_put(0, col, row, 1);
    oam_hide_all(p);
    oam_put(p, 3, 79, 6, 0x00, 80);   /* in front, over background */
    oam_put(p, 4, 79, 6, 0x20, 90);   /* behind, over background */
    oam_put(p, 5, 79, 6, 0x20, 120);  /* behind, over nothing */
    oam_put(p, 6, 79, 6, 0x00, 140);  /* two overlapping sprites: */
    oam_put(p, 7, 79, 6, 0x01, 144);  /*   the lower number wins */
    oam_put(p, 8, 99, 6, 0x22, 84);   /* behind the background, and in front */
    oam_put(p, 9, 99, 6, 0x03, 84);   /*   of a higher-numbered sprite */
    oam_put(p, 10, 99, 6, 0x22, 130); /* the same pair over nothing */
    oam_put(p, 11, 99, 6, 0x03, 130);

    RefScene s = { 0, 0, 0x08, 0x1E };
    set_scene(p, &s);
    render(p);
    CHECK(px(p, 80, 80) == spc(0, 3), "front sprite over background: %02X", px(p, 80, 80));
    CHECK(px(p, 88, 80) == bgc(0, 1), "behind sprite hidden by background: %02X", px(p, 88, 80));
    CHECK(px(p, 95, 80) == bgc(0, 1), "behind sprite hidden by background: %02X", px(p, 95, 80));
    CHECK(px(p, 120, 80) == spc(0, 3), "behind sprite over transparent background: %02X", px(p, 120, 80));
    CHECK(px(p, 143, 80) == spc(0, 3) && px(p, 144, 80) == spc(0, 3) && px(p, 147, 80) == spc(0, 3),
          "the lower sprite number wins the overlap");
    CHECK(px(p, 148, 80) == spc(1, 3) && px(p, 151, 80) == spc(1, 3), "the other one shows beside it");
    CHECK(px(p, 84, 100) == bgc(0, 1), "a behind sprite still masks a later front sprite: %02X", px(p, 84, 100));
    CHECK(px(p, 130, 100) == spc(2, 3), "and over nothing it shows itself: %02X", px(p, 130, 100));
    compare_with_reference(p, &s, "priority");
}

/* Render the scene twice, then return scanline * 1000 + dot of the first
 * dot of the next picture at which the sprite 0 hit flag is set, or -1. */
static long first_hit(NesPpu *p, uint8_t ctrl, uint8_t mask)
{
    RefScene s = { 0, 0, ctrl, mask };
    set_scene(p, &s);
    render(p);
    run_to(p, 261, 1);
    CHECK(!(p->status & 0x40), "hit flag cleared at the pre-render line");
    for (;;) {
        run(p, 1);
        if (p->status & 0x40) return (long)p->scanline * 1000 + p->dot;
        if (p->scanline == 240) return -1;
    }
}

static void test_sprite_zero_hit(void)
{
    NesPpu *p = fresh();
    standard_palette(p);
    tile_solid(0, 1, 1);
    tile_solid(1, 1, 2);
    for (unsigned i = 0; i < 0x3C0; i++) cart.nt[i] = 1;
    oam_hide_all(p);

    /* Pixel x is produced at dot x + 2, and the flag is set there. */
    oam_put(p, 0, 49, 1, 0, 100);
    long at = first_hit(p, 0x08, 0x1E);
    CHECK(at == 50 * 1000 + 102, "sprite at (100, 49): hit at %ld", at);
    CHECK(nes_ppu_read(p, 2) & 0x40, "$2002 shows it");
    run_to(p, 261, 0);
    CHECK(p->status & 0x40, "it stays set until the pre-render line");

    /* Sprite priority does not matter. */
    oam_put(p, 0, 49, 1, 0x20, 100);
    at = first_hit(p, 0x08, 0x1E);
    CHECK(at == 50 * 1000 + 102, "behind the background: hit at %ld", at);

    /* Never at x = 255. */
    oam_put(p, 0, 49, 1, 0, 255);
    at = first_hit(p, 0x08, 0x1E);
    CHECK(at == -1, "x=255: hit at %ld", at);
    oam_put(p, 0, 49, 1, 0, 254);
    at = first_hit(p, 0x08, 0x1E);
    CHECK(at == 50 * 1000 + 256, "x=254: hit at %ld", at);

    /* Not in the left 8 pixels unless both layers show there. */
    oam_put(p, 0, 49, 1, 0, 4);
    at = first_hit(p, 0x08, 0x1E);
    CHECK(at == 50 * 1000 + 6, "no clipping: hit at %ld", at);
    at = first_hit(p, 0x08, 0x1C);
    CHECK(at == 50 * 1000 + 10, "background clipped: hit at %ld", at);
    at = first_hit(p, 0x08, 0x1A);
    CHECK(at == 50 * 1000 + 10, "sprites clipped: hit at %ld", at);
    at = first_hit(p, 0x08, 0x18);
    CHECK(at == 50 * 1000 + 10, "both clipped: hit at %ld", at);

    /* Both layers must be enabled. */
    oam_put(p, 0, 49, 1, 0, 100);
    at = first_hit(p, 0x08, 0x16);
    CHECK(at == -1, "background off: hit at %ld", at);
    at = first_hit(p, 0x08, 0x0E);
    CHECK(at == -1, "sprites off: hit at %ld", at);

    /* Both pixels must be opaque. */
    nt_put(0, 12, 6, 0);
    nt_put(0, 13, 6, 0); /* x 96..111, y 48..55 transparent */
    at = first_hit(p, 0x08, 0x1E);
    CHECK(at == 56 * 1000 + 102, "over a hole in the background: hit at %ld", at);
    nt_put(0, 12, 6, 1);
    nt_put(0, 13, 6, 1);

    /* Only sprite 0. */
    oam_hide_all(p);
    oam_put(p, 1, 49, 1, 0, 100);
    at = first_hit(p, 0x08, 0x1E);
    CHECK(at == -1, "sprite 1: hit at %ld", at);

    /* The last line it can happen on is 239 (Y = 238). */
    oam_hide_all(p);
    oam_put(p, 0, 238, 1, 0, 100);
    at = first_hit(p, 0x08, 0x1E);
    CHECK(at == 239 * 1000 + 102, "Y=238: hit at %ld", at);
    oam_put(p, 0, 239, 1, 0, 100);
    at = first_hit(p, 0x08, 0x1E);
    CHECK(at == -1, "Y=239: hit at %ld", at);
}

/* Returns scanline * 1000 + dot at which the overflow flag is first set in
 * a complete frame, or -1. */
static long first_overflow(NesPpu *p)
{
    RefScene s = { 0, 0, 0x08, 0x1E };
    set_scene(p, &s);
    render(p);
    run_to(p, 261, 1);
    for (;;) {
        run(p, 1);
        if (p->status & 0x20) return (long)p->scanline * 1000 + p->dot;
        if (p->scanline == 240) return -1;
    }
}

static void test_sprite_overflow(void)
{
    NesPpu *p = fresh();
    standard_palette(p);
    tile_solid(1, 1, 2);

    /* Eight on a line: all shown, no flag. */
    oam_hide_all(p);
    for (unsigned n = 0; n < 8; n++) oam_put(p, n, 20, 1, 0, 10 * n);
    long at = first_overflow(p);
    CHECK(at == -1, "8 sprites: overflow at %ld", at);
    for (unsigned n = 0; n < 8; n++) CHECK(px(p, 10 * n, 21) == spc(0, 2), "sprite %u shown", n);

    /* Nine: the ninth is dropped and the flag is set as it is found.
     * Eight copies take dots 65-128; the ninth Y is read at dot 129 and
     * compared at dot 130. */
    oam_put(p, 8, 20, 1, 0, 80);
    at = first_overflow(p);
    CHECK(at == 20 * 1000 + 130, "9 sprites: overflow at %ld", at);
    CHECK(px(p, 80, 21) == BACKDROP && px(p, 70, 21) == spc(0, 2), "the ninth sprite is not shown");
    CHECK(nes_ppu_read(p, 2) & 0x20, "$2002 shows it");
    run_to(p, 261, 0);
    CHECK(p->status & 0x20, "set until the pre-render line");
    run(p, 1);
    CHECK(!(p->status & 0x20), "cleared at (261, 1)");

    /* Sprites that are not in range cost two dots each. */
    oam_hide_all(p);
    for (unsigned n = 0; n < 9; n++) oam_put(p, 10 + n, 20, 1, 0, 10 * n);
    at = first_overflow(p);
    CHECK(at == 20 * 1000 + 150, "9 sprites after 10 hidden ones: overflow at %ld", at);

    /* The bug. After eight sprites the search steps the byte index along
     * with the sprite number: sprite 8 is judged by its Y (dot 130),
     * sprite 9 by its tile number (132), sprite 10 by its attribute
     * (134), sprite 11 by its X (136), sprite 12 by its Y again (138).
     * Eight sprites at Y = 30 occupy lines 30-37 here.
     *
     * False negative: a ninth sprite at entry 9 is missed. */
    oam_hide_all(p);
    for (unsigned n = 0; n < 8; n++) oam_put(p, n, 30, 1, 0, 10 * n);
    oam_put(p, 8, 200, 1, 0, 0);
    oam_put(p, 9, 30, 0xF0, 0, 80);
    at = first_overflow(p);
    CHECK(at == -1, "false negative: overflow at %ld", at);

    /* False positive: only eight sprites on the line, but the tile number
     * of sprite 9 looks like a Y in range. */
    oam_put(p, 9, 200, 32, 0, 80);
    at = first_overflow(p);
    CHECK(at == 32 * 1000 + 132, "tile number as Y: overflow at %ld", at);

    oam_put(p, 9, 200, 0xF0, 0, 80);
    oam_put(p, 10, 200, 0xF0, 0x20, 80); /* attribute $20 = 32 */
    at = first_overflow(p);
    CHECK(at == 32 * 1000 + 134, "attribute as Y: overflow at %ld", at);

    oam_put(p, 10, 200, 0xF0, 0x00, 80);
    oam_put(p, 11, 200, 0xF0, 0, 31);
    at = first_overflow(p);
    CHECK(at == 31 * 1000 + 136, "X as Y: overflow at %ld", at);

    oam_put(p, 11, 200, 0xF0, 0, 250);
    oam_put(p, 12, 33, 0xF0, 0, 250);
    at = first_overflow(p);
    CHECK(at == 33 * 1000 + 138, "Y again: overflow at %ld", at);

    /* Nothing is evaluated with rendering off. */
    oam_hide_all(p);
    for (unsigned n = 0; n < 12; n++) oam_put(p, n, 20, 1, 0, 10 * n);
    wr(p, 1, 0x00);
    frames(p, 2);
    CHECK(!(p->status & 0x20), "rendering off: no overflow");
    /* With only the background enabled it is. */
    wr(p, 1, 0x08);
    frames(p, 2);
    CHECK(p->status & 0x20, "background only: overflow still detected");
}

static void test_sprite_evaluation_starts_at_oamaddr(void)
{
    /* Evaluation starts at OAMADDR, and the first sprite it looks at is
     * the one that can hit. OAMADDR is 0 by then unless it is written
     * between dot 320 of one line and dot 65 of the next. */
    NesPpu *p = fresh();
    standard_palette(p);
    tile_solid(0, 1, 1);
    tile_solid(1, 1, 2);
    for (unsigned i = 0; i < 0x3C0; i++) cart.nt[i] = 1;
    oam_hide_all(p);
    oam_put(p, 4, 49, 1, 0, 100);
    RefScene s = { 0, 0, 0x08, 0x1E };
    set_scene(p, &s);
    render(p);
    frame(p);
    CHECK(!(p->status & 0x40), "sprite 4 does not hit");
    run_to(p, 49, 30);
    nes_ppu_write(p, 3, 0x10);
    run_to(p, 240, 0);
    CHECK(p->status & 0x40, "evaluation started at sprite 4: it is treated as sprite 0");
}

/* --------------------------------------------------------------- whole PPU */

static void test_random_scenes_match_reference(void)
{
    static const struct {
        uint32_t seed;
        unsigned sprites;
        RefScene s;
    } cases[] = {
        { 1, 24, { 0, 0, 0x08, 0x1E } },     { 2, 64, { 77, 131, 0x08, 0x1E } },   { 3, 64, { 300, 250, 0x10, 0x18 } },
        { 4, 40, { 511, 479, 0x18, 0x1A } }, { 5, 64, { 129, 7, 0x28, 0x1E } },    { 6, 64, { 3, 3, 0x30, 0x1C } },
        { 7, 64, { 8, 16, 0x00, 0x1F } },    { 8, 64, { 250, 100, 0x08, 0xFE } },  { 9, 64, { 60, 60, 0x08, 0x16 } },
        { 10, 64, { 60, 60, 0x08, 0x0E } },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        NesPpu *p = fresh();
        char what[64];
        cart.mirroring = (i & 1u) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
        random_scene(p, cases[i].seed, cases[i].sprites);
        snprintf(what, sizeof what, "random scene %u (ctrl %02X mask %02X)", i, cases[i].s.ctrl, cases[i].s.mask);
        set_scene(p, &cases[i].s);
        render(p);
        compare_with_reference(p, &cases[i].s, what);
    }
}

/* A fixed program of register accesses at absolute dot times, run with a
 * given way of chopping the time into nes_ppu_run() calls. */
typedef struct {
    uint32_t at;
    int reg; /* negative: read register -reg - 1 */
    uint8_t value;
} ScriptStep;

static const ScriptStep script[] = {
    { 500, 0, 0x08 },       { 503, 5, 0x53 },    { 506, 5, 0x10 },    { 1000, 1, 0x1E },
    { 30000, 5, 0x21 },     { 30003, 5, 0x47 },  { 45000, 6, 0x21 },  { 45003, 6, 0x47 },  /* mid-frame v change */
    { 60000, 1, 0x00 },     { 60300, -8, 0 },    { 60309, 7, 0x77 },  { 61000, 1, 0x1E },  /* off and on again */
    { 80000, -3, 0 },       { 82166, -3, 0 },    { 100000, 3, 0x44 }, { 100003, 4, 0x99 }, /* (241, 0) is dot 82181 */
    { 120000, 0, 0x30 },    { 150000, -8, 0 },   { 150003, -8, 0 },   { 150030, 7, 0x12 }, /* $2007 while rendering */
    { 171523, -3, 0 },      { 171524, -3, 0 },   { 200000, 6, 0x3F }, { 200003, 6, 0x07 },
    { 200100, 1, 0xF9 },    { 230000, -5, 0 },   { 260000, 0, 0x88 }, { 261000, 3, 0x21 },
};

static void script_scene(NesPpu *p)
{
    random_scene(p, 99, 64);
    /* Put the PPU back at power-on position with the scene in place. */
    uint8_t oam[256], palette[32];
    memcpy(oam, p->oam, 256);
    memcpy(palette, p->palette, 32);
    nes_ppu_power(p);
    memcpy(p->oam, oam, 256);
    memcpy(p->palette, palette, 32);
    cart.hash = 0xCBF29CE484222325ull;
    cart.total = 0;
}

/* mode 0: one dot per call; 1: random chunks; 2: as far as possible. */
static uint64_t run_script(NesPpu *p, int mode, uint32_t total)
{
    uint64_t reads = 0xCBF29CE484222325ull;
    uint32_t now = 0;
    size_t next = 0;
    rng_state = 4242;
    while (now < total) {
        const uint32_t limit = next < sizeof script / sizeof script[0] ? script[next].at : total;
        while (now < limit) {
            uint32_t n = limit - now;
            if (mode == 0) n = 1;
            if (mode == 1) {
                const uint32_t r = 1u + rng() % 997u;
                if (r < n) n = r;
            }
            nes_ppu_run(p, n);
            now += n;
        }
        if (next < sizeof script / sizeof script[0]) {
            const ScriptStep *s = &script[next++];
            if (s->reg < 0)
                reads = fnv(reads, nes_ppu_read(p, (unsigned)(-s->reg - 1)));
            else
                nes_ppu_write(p, (unsigned)s->reg, s->value);
        }
    }
    return reads;
}

static void test_run_in_chunks_matches_dot_by_dot(void)
{
    static NesPpu reference;
    static uint8_t ref_chr[0x2000], ref_nt[0x800];
    const uint32_t total = 290000;

    NesPpu *p = fresh();
    script_scene(p);
    const uint64_t ref_reads = run_script(p, 0, total);
    const uint64_t ref_hash = cart.hash, ref_total = cart.total;
    memcpy(&reference, p, sizeof reference);
    memcpy(ref_chr, cart.chr, sizeof ref_chr);
    memcpy(ref_nt, cart.nt, sizeof ref_nt);
    CHECK(ref_total > 100000, "the reference run made %u bus accesses", (unsigned)ref_total);
    CHECK(p->frame_count == 3, "frames: %u", (unsigned)p->frame_count);

    for (int mode = 1; mode <= 2; mode++) {
        p = fresh();
        script_scene(p);
        const uint64_t reads = run_script(p, mode, total);
        CHECK(reads == ref_reads, "mode %d: register reads differ", mode);
        CHECK(cart.total == ref_total && cart.hash == ref_hash, "mode %d: bus log differs (%u accesses, reference %u)", mode,
              (unsigned)cart.total, (unsigned)ref_total);
        CHECK(!memcmp(p->frame, reference.frame, sizeof p->frame), "mode %d: picture differs", mode);
        CHECK(!memcmp(p, &reference, sizeof *p), "mode %d: PPU state differs", mode);
        CHECK(!memcmp(cart.chr, ref_chr, sizeof ref_chr) && !memcmp(cart.nt, ref_nt, sizeof ref_nt),
              "mode %d: cartridge memory differs", mode);
    }
}

static void test_palette_table(void)
{
    static uint32_t rgb[512], again[512];
    nes_ppu_palette(rgb);
    nes_ppu_palette(again);
    CHECK(!memcmp(rgb, again, sizeof rgb), "not deterministic");

    /* The table is part of byte-identical frame comparisons between
     * compilers, so it is pinned. */
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned i = 0; i < 512; i++) h = fnv(h, rgb[i]);
    CHECK(h == 0xCA521C397F2DB456ull, "palette hash %016llX", (unsigned long long)h);

#define R(c) (((c) >> 16) & 0xFFu)
#define G(c) (((c) >> 8) & 0xFFu)
#define B(c) ((c) & 0xFFu)
    for (unsigned i = 0; i < 512; i++) CHECK(rgb[i] <= 0xFFFFFFu, "entry %u has high bits", i);
    CHECK(rgb[0x20] == 0xFFFFFF && rgb[0x30] == 0xFFFFFF, "$20/$30 are white: %06X %06X", (unsigned)rgb[0x20],
          (unsigned)rgb[0x30]);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(rgb[i * 16 + 0x0E] == 0 && rgb[i * 16 + 0x0F] == 0, "$%XE/$%XF are black", i, i);
        CHECK(R(rgb[i * 16]) == G(rgb[i * 16]) && G(rgb[i * 16]) == B(rgb[i * 16]), "$%X0 is grey: %06X", i,
              (unsigned)rgb[i * 16]);
        CHECK(R(rgb[i * 16 + 13]) == G(rgb[i * 16 + 13]) && G(rgb[i * 16 + 13]) == B(rgb[i * 16 + 13]), "$%XD is grey", i);
    }
    CHECK(rgb[0x0D] == 0 && rgb[0x1D] == 0, "$0D and $1D are black");
    CHECK(R(rgb[0x00]) < R(rgb[0x10]) && R(rgb[0x10]) < R(rgb[0x20]), "greys get brighter");
    CHECK(R(rgb[0x2D]) < R(rgb[0x3D]) && R(rgb[0x3D]) < 255, "$2D < $3D < white");
    /* Hues: $x2 blue, $x6 red, $xA green. */
    CHECK(B(rgb[0x12]) > R(rgb[0x12]) && B(rgb[0x12]) > G(rgb[0x12]), "$12 is blue: %06X", (unsigned)rgb[0x12]);
    CHECK(R(rgb[0x16]) > G(rgb[0x16]) && R(rgb[0x16]) > B(rgb[0x16]), "$16 is red: %06X", (unsigned)rgb[0x16]);
    CHECK(G(rgb[0x1A]) > R(rgb[0x1A]) && G(rgb[0x1A]) > B(rgb[0x1A]), "$1A is green: %06X", (unsigned)rgb[0x1A]);
    /* Emphasis: bit 6 of the index tints red, bit 7 green, bit 8 blue;
     * all three darken; black is not affected. */
    const uint32_t red = rgb[0x30 | 0x040], green = rgb[0x30 | 0x080], blue = rgb[0x30 | 0x100], all = rgb[0x30 | 0x1C0];
    CHECK(R(red) > G(red) && R(red) > B(red), "red emphasis on white: %06X", (unsigned)red);
    CHECK(G(green) > R(green) && G(green) > B(green), "green emphasis on white: %06X", (unsigned)green);
    CHECK(B(blue) > R(blue) && B(blue) > G(blue), "blue emphasis on white: %06X", (unsigned)blue);
    CHECK(R(all) == G(all) && G(all) == B(all) && R(all) < 255 && R(all) > 128, "all emphasis darkens white: %06X",
          (unsigned)all);
    CHECK(rgb[0x0F | 0x1C0] == 0, "black stays black");
#undef R
#undef G
#undef B
}

/* ------------------------------------------------------------------- main */

typedef struct {
    const char *name;
    void (*fn)(void);
} Test;

int main(void)
{
    static const Test tests[] = {
        { "scroll_latch_wiki_example", test_scroll_latch_wiki_example },
        { "data_port_buffer_and_increment", test_data_port_buffer_and_increment },
        { "data_port_read_twice_in_a_row", test_data_port_read_twice_in_a_row },
        { "palette_mirrors_and_greyscale", test_palette_mirrors_and_greyscale },
        { "oam_port", test_oam_port },
        { "oam_addr_copy_when_rendering_starts", test_oam_addr_copy_when_rendering_starts },
        { "io_latch_open_bus_and_decay", test_io_latch_open_bus_and_decay },
        { "reset_and_warmup", test_reset_and_warmup },
        { "vblank_set_and_clear_dots", test_vblank_set_and_clear_dots },
        { "status_read_race", test_status_read_race },
        { "nmi_line_level", test_nmi_line_level },
        { "frame_length_even_odd", test_frame_length_even_odd },
        { "dots_until_event_never_overestimates", test_dots_until_event_never_overestimates },
        { "scanline_fetch_sequence", test_scanline_fetch_sequence },
        { "a12_rises_once_per_line", test_a12_rises_once_per_line },
        { "address_bus_reports", test_address_bus_reports },
        { "data_port_during_rendering", test_data_port_during_rendering },
        { "background_tile_scroll_fine_x", test_background_tile_scroll_fine_x },
        { "attribute_quadrants", test_attribute_quadrants },
        { "left_column_clipping", test_left_column_clipping },
        { "emphasis_and_greyscale_in_output", test_emphasis_and_greyscale_in_output },
        { "rendering_disabled_backdrop", test_rendering_disabled_backdrop },
        { "sprites_8x8_flips_and_palettes", test_sprites_8x8_flips_and_palettes },
        { "sprites_8x16", test_sprites_8x16 },
        { "sprite_priority", test_sprite_priority },
        { "sprite_zero_hit", test_sprite_zero_hit },
        { "sprite_overflow", test_sprite_overflow },
        { "sprite_evaluation_starts_at_oamaddr", test_sprite_evaluation_starts_at_oamaddr },
        { "random_scenes_match_reference", test_random_scenes_match_reference },
        { "run_in_chunks_matches_dot_by_dot", test_run_in_chunks_matches_dot_by_dot },
        { "palette_table", test_palette_table },
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
