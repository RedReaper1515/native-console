/*
 * Cartridge backup memory, driven a byte at a time the way the AUXSPI port
 * delivers a program's bytes: nds_save_transfer() for each byte sent while
 * chip select is held, nds_save_deselect() when it is released. The command
 * sequences are GBATEK's ("DS Cartridge Backup", and "DS Firmware Serial
 * Flash Memory" for FLASH) and those libnds's cardEeprom.c sends.
 *
 * The test keeps its own copy of what the chip's memory should hold,
 * changed only by what the documentation says a command does, and compares
 * the whole memory after every command.
 *
 * Covered: each family (EEPROM 0.5 KiB, 8 KiB, 64 KiB, 128 KiB; FRAM; FLASH
 * 256 KiB to 8 MiB) read and written over its whole address range, the
 * write enable latch, the status register and its protection bits, page
 * limits, the FLASH commands, the infrared cards' prefix, recognising each
 * family from the first commands of a program when the type is unknown, and
 * the table of games, including the first game, Phantom Hourglass.
 */
#include <stdio.h>
#include <string.h>

#include "nds_save.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                \
    do {                                                \
        g_checks++;                                     \
        if (!(cond)) {                                  \
            g_failures++;                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

#define MOST 0x800000u /* the largest chip: 8 MiB of FLASH */

static NdsSave g_save;
static uint8_t g_memory[MOST];
static uint8_t g_expected[MOST];
static uint32_t g_size;  /* bytes of memory given to the chip */
static int g_width;      /* address bytes */
static bool g_small;     /* the 0.5 KiB EEPROM, with its ninth address bit in the command */
static bool g_dirty;
static const char *g_name = "";

static uint8_t exchange(uint8_t out) { return nds_save_transfer(&g_save, g_memory, out, &g_dirty); }
static void release(void) { nds_save_deselect(&g_save); }

static uint8_t pattern(uint32_t i, uint32_t seed)
{
    return (uint8_t)((i * 7u + seed) ^ (i >> 8) ^ (i >> 15) ^ (seed >> 3));
}

static void setup(NdsSaveType type, uint32_t size, int width, const char *name)
{
    g_size = size;
    g_width = width;
    g_small = type == NDS_SAVE_EEPROM_SMALL;
    g_name = name;
    g_dirty = false;
    for (uint32_t i = 0; i < size; i++) g_memory[i] = g_expected[i] = pattern(i, 0x11);
    nds_save_init(&g_save, type, size);
}

static void expect_memory(int line, const char *what)
{
    g_checks++;
    if (g_size && memcmp(g_memory, g_expected, g_size) != 0) {
        uint32_t at = 0;
        while (g_memory[at] == g_expected[at]) at++;
        g_failures++;
        printf("FAIL %s:%d: %s: %s: memory differs at %06X (%02X, expected %02X)\n", __FILE__, line, g_name, what, at,
               g_memory[at], g_expected[at]);
        memcpy(g_memory, g_expected, g_size);
    }
}

static void simple(uint8_t command)
{
    exchange(command);
    release();
}

static uint8_t status(void)
{
    exchange(0x05);
    uint8_t value = exchange(0);
    release();
    return value;
}

static void send_address(uint32_t address, int width)
{
    for (int i = width - 1; i >= 0; i--) exchange((uint8_t)(address >> (8 * i)));
}

/* A read as the chip's family wants it: 03h and the address, or for the
 * 0.5 KiB EEPROM 03h/0Bh by the address's ninth bit. */
static void begin_read(uint32_t address)
{
    exchange(g_small && (address & 0x100u) ? 0x0B : 0x03);
    send_address(address, g_width);
}

/* An EEPROM or FRAM write: 02h (0Ah for the 0.5 KiB chip's upper half), the
 * address, the bytes. */
static void store(uint32_t address, const uint8_t *bytes, uint32_t count)
{
    exchange(g_small && (address & 0x100u) ? 0x0A : 0x02);
    send_address(address, g_width);
    for (uint32_t i = 0; i < count; i++) exchange(bytes[i]);
    release();
}

static void flash_command(uint8_t command, uint32_t address, const uint8_t *bytes, uint32_t count)
{
    exchange(command);
    send_address(address, 3);
    for (uint32_t i = 0; i < count; i++) exchange(bytes[i]);
    release();
}

/* Read the whole memory in one command, and two bytes more: the address
 * counter goes from the last byte to the first. */
static void expect_read_all(int line)
{
    bool same = true;
    begin_read(0);
    for (uint32_t i = 0; i < g_size; i++) same = same && exchange(0) == g_expected[i];
    same = same && exchange(0) == g_expected[0] && exchange(0) == g_expected[1];
    release();
    g_checks++;
    if (!same) {
        g_failures++;
        printf("FAIL %s:%d: %s: reading the whole memory and on past its end\n", __FILE__, line, g_name);
    }
}

/* ------------------------------------------------------------------ */
/* EEPROM and FRAM                                                     */
/* ------------------------------------------------------------------ */

/* page is the most one write command changes: 0 for FRAM. */
static void test_eeprom(NdsSaveType type, uint32_t size, int width, uint32_t page, const char *name)
{
    uint8_t bytes[300];
    uint8_t got[4];

    setup(type, size, width, name);
    uint8_t base = (uint8_t)(g_small ? 0xF0 : 0x00);

    /* GBATEK's table of hardware responses: RDSR reads F0h on the EEPROM
     * with the 8+1 bit address and 00h on the other EEPROMs and FRAM; RDID
     * reads FFh FFh FFh on all of them. */
    CHECK(status() == base, "%s: status %02X, expected %02X", name, status(), base);
    exchange(0x9F);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    release();
    CHECK(got[0] == 0xFF && got[1] == 0xFF && got[2] == 0xFF, "%s: RDID %02X %02X %02X, expected FF FF FF", name, got[0],
          got[1], got[2]);

    /* WREN and WRDI move bit 1; the status register repeats while clocked. */
    simple(0x06);
    exchange(0x05);
    got[0] = exchange(0);
    got[1] = exchange(0);
    release();
    CHECK(got[0] == (base | 0x02) && got[1] == (base | 0x02), "%s: status after WREN %02X %02X, expected %02X", name,
          got[0], got[1], base | 0x02);
    simple(0x04);
    CHECK(status() == base, "%s: status after WRDI %02X, expected %02X", name, status(), base);

    /* Reading. */
    expect_read_all(__LINE__);
    begin_read(size - 3);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    got[3] = exchange(0);
    release();
    CHECK(got[0] == g_expected[size - 3] && got[2] == g_expected[size - 1] && got[3] == g_expected[0],
          "%s: reading across the end of memory", name);
    if (!g_small) {
        /* Address bits the chip does not have are ignored. */
        uint32_t beyond = (width == 2 ? 0x10000u : 0x1000000u) - size;
        if (beyond) {
            exchange(0x03);
            send_address(size + 5, width);
            got[0] = exchange(0);
            release();
            CHECK(got[0] == g_expected[5], "%s: address %X reads %02X, expected the byte at 5 (%02X)", name, size + 5,
                  got[0], g_expected[5]);
        }
        /* 0Bh and 0Ah belong to the 0.5 KiB chip and to FLASH. */
        exchange(0x0B);
        send_address(0, width);
        got[0] = exchange(0);
        release();
        simple(0x06);
        exchange(0x0A);
        send_address(0, width);
        exchange((uint8_t)~g_expected[0]);
        exchange((uint8_t)~g_expected[1]);
        release();
        expect_memory(__LINE__, "command 0Ah");
        CHECK(got[0] == 0xFF, "%s: command 0Bh was answered (%02X)", name, got[0]);
        simple(0x04);
    }
    CHECK(!g_dirty, "%s: dirty before anything was written", name);

    /* A write needs the latch. */
    for (uint32_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(0x40u + i * 5u);
    store(0, bytes, 8);
    expect_memory(__LINE__, "write without WREN");
    CHECK(!g_dirty, "%s: a rejected write set dirty", name);

    /* A write: the bytes land, the latch drops ("WEL gets reset on ...
     * WRITE"), and writing takes no time. */
    simple(0x06);
    store(8, bytes, 8);
    memcpy(g_expected + 8, bytes, 8);
    expect_memory(__LINE__, "write of 8 bytes");
    CHECK(status() == base, "%s: status after a write %02X, expected %02X (latch and busy clear)", name, status(), base);
    CHECK(g_dirty, "%s: a write did not set dirty", name);
    store(16, bytes, 8);
    expect_memory(__LINE__, "second write without a second WREN");
    /* Writing what is already there changes nothing. */
    g_dirty = false;
    simple(0x06);
    store(8, bytes, 8);
    expect_memory(__LINE__, "write of the same bytes");
    CHECK(!g_dirty, "%s: a write that changed nothing set dirty", name);

    /* The whole memory, a page at a time (FRAM: 256 bytes at a time). */
    uint32_t step = page ? page : 256;
    for (uint32_t at = 0; at < size; at += step) {
        for (uint32_t i = 0; i < step; i++) bytes[i] = pattern(at + i, 0x77);
        simple(0x06);
        store(at, bytes, step);
        memcpy(g_expected + at, bytes, step);
    }
    expect_memory(__LINE__, "writing every page");
    expect_read_all(__LINE__);
    CHECK(g_dirty, "%s: writing the whole memory did not set dirty", name);

    if (page) {
        /* "write 1..MAX byte(s)", MAX the page size: past the end of the
         * page the bytes go to its start. */
        uint32_t start = 3 * page - 2;
        if (start + 2 > size) start = size - 2;
        uint32_t first = start & ~(page - 1);
        for (uint32_t i = 0; i < 4; i++) bytes[i] = (uint8_t)(0xE0u + i);
        simple(0x06);
        store(start, bytes, 4);
        g_expected[start] = 0xE0;
        g_expected[start + 1] = 0xE1;
        g_expected[first] = 0xE2;
        g_expected[first + 1] = 0xE3;
        expect_memory(__LINE__, "write across the end of a page");
    } else {
        /* FRAM: "No limit". One command writes everything, starting in the
         * middle and running on through the end of memory to the start. */
        uint32_t start = size / 2 + 3;
        simple(0x06);
        exchange(0x02);
        send_address(start, width);
        for (uint32_t i = 0; i < size; i++) {
            uint32_t at = (start + i) & (size - 1);
            g_expected[at] = pattern(at, 0xC3);
            exchange(g_expected[at]);
        }
        release();
        expect_memory(__LINE__, "one write of the whole memory");
        CHECK(status() == base, "%s: status after the long write %02X", name, status());
    }

    /* WRSR: "allows to change ONLY the two WP bits, and the SRWD bit (if
     * any)"; it needs the latch and resets it. */
    exchange(0x01);
    exchange(0x0C);
    release();
    CHECK(status() == base, "%s: WRSR without WREN changed the status to %02X", name, status());
    simple(0x06);
    exchange(0x01);
    exchange(0xFF);
    release();
    uint8_t all = (uint8_t)(g_small ? 0xFC : 0x8C);
    CHECK(status() == all, "%s: status after writing FF is %02X, expected %02X", name, status(), all);

    /* Protection 3: all of memory. */
    for (uint32_t i = 0; i < 4; i++) bytes[i] = (uint8_t)(0x99u + i);
    simple(0x06);
    store(0, bytes, 4);
    simple(0x06);
    store(size - 4, bytes, 4);
    expect_memory(__LINE__, "writes with all memory protected");
    /* 2: the upper half. */
    simple(0x06);
    exchange(0x01);
    exchange(0x08);
    release();
    CHECK((status() & 0x0C) == 0x08, "%s: protection bits %02X, expected 08", name, status());
    simple(0x06);
    store(size / 2 - 4, bytes, 4);
    memcpy(g_expected + size / 2 - 4, bytes, 4);
    simple(0x06);
    store(size / 2, bytes, 4);
    expect_memory(__LINE__, "writes with the upper half protected");
    /* 1: the upper quarter. */
    simple(0x06);
    exchange(0x01);
    exchange(0x04);
    release();
    simple(0x06);
    store(size / 4 * 3 - 4, bytes, 4);
    memcpy(g_expected + size / 4 * 3 - 4, bytes, 4);
    simple(0x06);
    store(size / 4 * 3, bytes, 4);
    simple(0x06);
    store(size - 4, bytes, 4);
    expect_memory(__LINE__, "writes with the upper quarter protected");
    /* 0: none. */
    simple(0x06);
    exchange(0x01);
    exchange(0x00);
    release();
    CHECK(status() == base, "%s: status after clearing the protection %02X, expected %02X", name, status(), base);
    simple(0x06);
    store(size - 4, bytes, 4);
    memcpy(g_expected + size - 4, bytes, 4);
    expect_memory(__LINE__, "write to the top of memory with protection off");

    /* Bytes that are not commands, and FLASH's commands, do nothing. */
    simple(0x06);
    exchange(0xD8);
    send_address(0, 3);
    release();
    exchange(0xDB);
    send_address(0, 3);
    release();
    exchange(0x00);
    exchange(0x00);
    release();
    exchange(0xFF);
    exchange(0xFF);
    release();
    expect_memory(__LINE__, "FLASH and unknown commands");
    CHECK(status() == (base | 0x02), "%s: status %02X: an unknown command changed the latch", name, status());

    /* WREN, then a byte with chip select still held: the chip waits to be
     * deselected, and what follows is not a command. */
    simple(0x04);
    exchange(0x06);
    exchange(0x02);
    send_address(0, width);
    exchange((uint8_t)~g_expected[0]);
    release();
    expect_memory(__LINE__, "a write sent in the same chip select as WREN");
}

/* ------------------------------------------------------------------ */
/* FLASH                                                               */
/* ------------------------------------------------------------------ */

static void test_flash(uint32_t size, const uint8_t id[3], const char *name)
{
    static uint8_t bytes[600];
    uint8_t got[4];
    bool same;

    setup(NDS_SAVE_FLASH, size, 3, name);

    /* The same table for FLASH: RDSR reads 00h and RDID an ID. */
    CHECK(status() == 0x00, "%s: status %02X, expected 00", name, status());
    exchange(0x9F);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    release();
    CHECK(got[0] == id[0] && got[1] == id[1] && got[2] == id[2], "%s: RDID %02X %02X %02X, expected %02X %02X %02X", name,
          got[0], got[1], got[2], id[0], id[1], id[2]);
    simple(0x06);
    CHECK(status() == 0x02, "%s: status after WREN %02X, expected 02", name, status());
    simple(0x04);
    CHECK(status() == 0x00, "%s: status after WRDI %02X, expected 00", name, status());

    /* 03h READ over everything; 0Bh FAST with its dummy byte. */
    expect_read_all(__LINE__);
    exchange(0x0B);
    send_address(size - 2, 3);
    exchange(0);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    release();
    CHECK(got[0] == g_expected[size - 2] && got[1] == g_expected[size - 1] && got[2] == g_expected[0],
          "%s: fast read across the end of memory: %02X %02X %02X", name, got[0], got[1], got[2]);
    if (size < 0x1000000u) {
        exchange(0x03);
        send_address(size + 0x123, 3);
        got[0] = exchange(0);
        release();
        CHECK(got[0] == g_expected[0x123], "%s: address %X reads %02X, expected the byte at 123h", name, size + 0x123,
              got[0]);
    }
    CHECK(!g_dirty, "%s: dirty before anything was written", name);

    /* 0Ah PW: needs the latch, replaces the bytes addressed, drops the
     * latch. */
    for (uint32_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(0x31u + i * 11u);
    flash_command(0x0A, 0x000100, bytes, 16);
    expect_memory(__LINE__, "page write without WREN");
    simple(0x06);
    flash_command(0x0A, 0x000100, bytes, 16);
    memcpy(g_expected + 0x100, bytes, 16);
    expect_memory(__LINE__, "page write of 16 bytes");
    CHECK(status() == 0x00, "%s: status after a page write %02X", name, status());
    CHECK(g_dirty, "%s: a page write did not set dirty", name);
    flash_command(0x0A, 0x000200, bytes, 16);
    expect_memory(__LINE__, "second page write without a second WREN");

    /* Every page of the chip. */
    for (uint32_t at = 0; at < size; at += 256) {
        for (uint32_t i = 0; i < 256; i++) bytes[i] = pattern(at + i, 0x5A);
        simple(0x06);
        flash_command(0x0A, at, bytes, 256);
        memcpy(g_expected + at, bytes, 256);
    }
    expect_memory(__LINE__, "page-writing the whole chip");
    expect_read_all(__LINE__);

    /* A write stays in its page; of more than 256 bytes the last 256
     * count. */
    for (uint32_t i = 0; i < 4; i++) bytes[i] = (uint8_t)(0xD0u + i);
    simple(0x06);
    flash_command(0x0A, 0x0003FE, bytes, 4);
    g_expected[0x3FE] = 0xD0;
    g_expected[0x3FF] = 0xD1;
    g_expected[0x300] = 0xD2;
    g_expected[0x301] = 0xD3;
    expect_memory(__LINE__, "page write across the end of a page");
    for (uint32_t i = 0; i < 258; i++) bytes[i] = (uint8_t)(i * 3u + 1u);
    simple(0x06);
    flash_command(0x0A, 0x000400, bytes, 258);
    memcpy(g_expected + 0x400, bytes, 256);
    g_expected[0x400] = bytes[256];
    g_expected[0x401] = bytes[257];
    expect_memory(__LINE__, "page write of 258 bytes");

    /* DBh PE and D8h SE make a page of 100h and a sector of 10000h bytes
     * FFh; they need the latch and drop it. */
    flash_command(0xDB, 0x000512, bytes, 0);
    flash_command(0xD8, 0x012345, bytes, 0);
    expect_memory(__LINE__, "erases without WREN");
    simple(0x06);
    flash_command(0xDB, 0x000512, bytes, 0);
    memset(g_expected + 0x500, 0xFF, 0x100);
    expect_memory(__LINE__, "page erase");
    CHECK(status() == 0x00, "%s: status after a page erase %02X", name, status());
    simple(0x06);
    flash_command(0xD8, 0x012345, bytes, 0);
    memset(g_expected + 0x10000, 0xFF, 0x10000);
    expect_memory(__LINE__, "sector erase");
    CHECK(status() == 0x00, "%s: status after a sector erase %02X", name, status());
    simple(0x06);
    flash_command(0xD8, size - 1, bytes, 0);
    memset(g_expected + size - 0x10000, 0xFF, 0x10000);
    expect_memory(__LINE__, "erase of the last sector");

    /* 02h PP changes bits from 1 to 0 only: into erased flash it stores
     * the bytes, over data it ANDs. */
    for (uint32_t i = 0; i < 16; i++) bytes[i] = (uint8_t)(0x0Fu + i * 16u);
    flash_command(0x02, 0x010000, bytes, 16);
    expect_memory(__LINE__, "page program without WREN");
    simple(0x06);
    flash_command(0x02, 0x010000, bytes, 16);
    memcpy(g_expected + 0x10000, bytes, 16);
    expect_memory(__LINE__, "page program into erased flash");
    CHECK(status() == 0x00, "%s: status after a page program %02X", name, status());
    simple(0x06);
    flash_command(0x02, 0x000000, bytes, 16);
    for (uint32_t i = 0; i < 16; i++) g_expected[i] &= bytes[i];
    expect_memory(__LINE__, "page program over data");
    /* A place programmed twice in one command keeps the later byte. */
    memset(bytes, 0xFF, 300);
    bytes[0] = 0x0F;
    bytes[256] = 0xF0;
    simple(0x06);
    flash_command(0x02, 0x010880, bytes, 257);
    g_expected[0x10880] = 0xF0;
    expect_memory(__LINE__, "page program of 257 bytes");

    /* B9h DP: only ABh RDP is obeyed after it. */
    simple(0xB9);
    exchange(0x9F);
    got[0] = exchange(0);
    release();
    exchange(0x03);
    send_address(0x000100, 3);
    same = exchange(0) == g_expected[0x100] && exchange(0) == g_expected[0x101] && exchange(0) == g_expected[0x102];
    release();
    simple(0x06);
    flash_command(0x0A, 0x000100, bytes, 4);
    expect_memory(__LINE__, "page write in deep power-down");
    CHECK(got[0] != id[0] && !same, "%s: answered in deep power-down", name);
    simple(0xAB);
    exchange(0x9F);
    got[0] = exchange(0);
    release();
    CHECK(got[0] == id[0], "%s: RDID after release from deep power-down %02X", name, got[0]);
    CHECK(status() == 0x00, "%s: WREN was obeyed in deep power-down (status %02X)", name, status());

    /* 01h WRSR is not one of this chip's commands. */
    simple(0x06);
    exchange(0x01);
    exchange(0xFF);
    release();
    CHECK(status() == 0x02, "%s: status after 01h FFh is %02X, expected 02", name, status());
    simple(0x04);
}

/* ------------------------------------------------------------------ */
/* No chip                                                             */
/* ------------------------------------------------------------------ */

static void test_none(NdsSaveType type, uint32_t size, const char *name)
{
    setup(NDS_SAVE_NONE, 0x200, 2, name);
    nds_save_init(&g_save, type, size);
    /* And for no chip: RDSR and RDID read FFh. */
    CHECK(status() == 0xFF, "%s: status %02X, expected FF", name, status());
    exchange(0x9F);
    CHECK(exchange(0) == 0xFF && exchange(0) == 0xFF && exchange(0) == 0xFF, "%s: RDID answered", name);
    release();
    simple(0x06);
    store(0, (const uint8_t *)"\x12\x34", 2);
    exchange(0x03);
    send_address(0, 2);
    CHECK(exchange(0) == 0xFF, "%s: a read was answered", name);
    release();
    expect_memory(__LINE__, "commands to a card with no chip");
    CHECK(!g_dirty, "%s: dirty with no chip", name);
}

/* ------------------------------------------------------------------ */
/* Cards with an infrared port                                         */
/* ------------------------------------------------------------------ */

static void test_infrared(void)
{
    uint8_t bytes[16];
    uint8_t got[4];

    CHECK(nds_save_has_infrared("IPKE") && nds_save_has_infrared("IRBO") && nds_save_has_infrared("IMWE"),
          "game codes starting with I are not infrared cards");
    CHECK(!nds_save_has_infrared("AZEE") && !nds_save_has_infrared("CPUE"), "other game codes are infrared cards");

    setup(NDS_SAVE_FLASH, 0x80000, 3, "infrared card");
    nds_save_set_infrared(&g_save, true);

    /* "00h,cmd,params[...] Savedata access" */
    exchange(0x00);
    exchange(0x9F);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    release();
    CHECK(got[0] == 0x20 && got[1] == 0x40 && got[2] == 0x13, "RDID behind the 00h prefix: %02X %02X %02X", got[0],
          got[1], got[2]);
    for (uint32_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(0xB0u + i);
    exchange(0x00);
    exchange(0x06);
    release();
    exchange(0x00);
    flash_command(0x0A, 0x012300, bytes, 16);
    memcpy(g_expected + 0x12300, bytes, 16);
    expect_memory(__LINE__, "page write behind the 00h prefix");
    exchange(0x00);
    exchange(0x03);
    send_address(0x012300, 3);
    got[0] = exchange(0);
    got[1] = exchange(0);
    release();
    CHECK(got[0] == 0xB0 && got[1] == 0xB1, "read behind the 00h prefix: %02X %02X", got[0], got[1]);

    /* Without the prefix the bytes are the infrared processor's, not the
     * chip's. */
    exchange(0x06);
    release();
    exchange(0x0A);
    send_address(0x000000, 3);
    exchange(0x55);
    release();
    exchange(0x9F);
    got[0] = exchange(0);
    release();
    expect_memory(__LINE__, "commands without the 00h prefix");
    CHECK(got[0] != 0x20, "the chip answered RDID without the 00h prefix");
    /* "08h,ver  NEW: Returns version (ver=AAh)" */
    exchange(0x08);
    got[0] = exchange(0);
    release();
    CHECK(got[0] == 0xAA, "infrared version %02X, expected AA", got[0]);
    /* "01h,00h,00h  Infrared RX (none, len=0, plus dummy data=0)" */
    exchange(0x01);
    got[0] = exchange(0);
    got[1] = exchange(0);
    release();
    CHECK(got[0] == 0x00 && got[1] == 0x00, "infrared receive with nothing received: %02X %02X", got[0], got[1]);

    /* A card without the port takes the first byte as the chip's command. */
    nds_save_set_infrared(&g_save, false);
    exchange(0x9F);
    got[0] = exchange(0);
    release();
    CHECK(got[0] == 0x20, "RDID on a card without the infrared port: %02X", got[0]);
}

/* ------------------------------------------------------------------ */
/* An unknown chip                                                     */
/* ------------------------------------------------------------------ */

#define UNKNOWN_SIZE 0x100000u /* what the machine allows a chip it does not know */

static void unknown(const char *name)
{
    g_size = UNKNOWN_SIZE;
    g_name = name;
    g_dirty = false;
    memset(g_memory, 0xFF, UNKNOWN_SIZE); /* a new chip */
    memset(g_expected, 0xFF, UNKNOWN_SIZE);
    nds_save_init(&g_save, NDS_SAVE_UNKNOWN, UNKNOWN_SIZE);
}

/* The first write of a program: WREN, then command, address and data in
 * one chip select, then, as programs do, a status poll. */
static void first_write(uint8_t command, uint32_t address, int width, uint32_t count, uint32_t at)
{
    simple(0x06);
    exchange(command);
    send_address(address, width);
    for (uint32_t i = 0; i < count; i++) {
        g_expected[at + i] = (uint8_t)(0x21u + i * 3u);
        exchange(g_expected[at + i]);
    }
    release();
    exchange(0x05);
    exchange(0);
    release();
}

static void expect_type(int line, NdsSaveType type, uint32_t size)
{
    g_checks++;
    if (g_save.type != type || g_save.size != size) {
        g_failures++;
        printf("FAIL %s:%d: %s: recognised as type %d with %X bytes, expected type %d with %X\n", __FILE__, line, g_name,
               (int)g_save.type, g_save.size, (int)type, size);
    }
}

static void test_unknown(void)
{
    uint8_t got[4];

    /* Before anything tells: reads see a new chip, the latch works, no
     * memory is touched. */
    unknown("unknown chip");
    CHECK(g_save.type == NDS_SAVE_UNKNOWN, "an unknown chip starts as type %d", (int)g_save.type);
    exchange(0x03);
    got[0] = exchange(0x00);
    got[1] = exchange(0x00);
    got[2] = exchange(0x00);
    got[3] = exchange(0x00);
    release();
    CHECK(got[0] == 0xFF && got[1] == 0xFF && got[2] == 0xFF && got[3] == 0xFF, "a read before the chip is known");
    CHECK(status() == 0x00, "status before the chip is known %02X", status());
    simple(0x06);
    CHECK(status() == 0x02, "status after WREN before the chip is known %02X", status());
    simple(0x04);
    CHECK(g_save.type == NDS_SAVE_UNKNOWN && !g_dirty, "reads and status polls decided the type");

    /* EEPROM 0.5 KiB: a page of 16 bytes after one address byte. */
    unknown("0.5 KiB EEPROM, a page to 20h");
    first_write(0x02, 0x20, 1, 16, 0x20);
    expect_type(__LINE__, NDS_SAVE_EEPROM_SMALL, 0x200);
    expect_memory(__LINE__, "the first write");
    CHECK(g_dirty, "the first write did not set dirty");
    CHECK(status() == 0xF0, "status once recognised %02X, expected F0", status());
    /* ... through the command for the upper half ... */
    unknown("0.5 KiB EEPROM, a page to 120h");
    first_write(0x0A, 0x20, 1, 16, 0x120);
    expect_type(__LINE__, NDS_SAVE_EEPROM_SMALL, 0x200);
    expect_memory(__LINE__, "the first write");
    /* ... and from a single byte. */
    unknown("0.5 KiB EEPROM, one byte");
    first_write(0x02, 0x05, 1, 1, 0x05);
    expect_type(__LINE__, NDS_SAVE_EEPROM_SMALL, 0x200);
    expect_memory(__LINE__, "the first write");

    /* EEPROM with two address bytes: a page of 32 (the 8 KiB chip) or 128
     * (the 64 KiB), or one byte. */
    unknown("EEPROM, a page of 32 to 0100h");
    first_write(0x02, 0x0100, 2, 32, 0x0100);
    expect_type(__LINE__, NDS_SAVE_EEPROM, 0x10000);
    expect_memory(__LINE__, "the first write");
    CHECK(status() == 0x00, "status once recognised %02X, expected 00", status());
    unknown("EEPROM, a page of 128 to 8080h");
    first_write(0x02, 0x8080, 2, 128, 0x8080);
    expect_type(__LINE__, NDS_SAVE_EEPROM, 0x10000);
    expect_memory(__LINE__, "the first write");
    unknown("EEPROM, one byte to 1234h");
    first_write(0x02, 0x1234, 2, 1, 0x1234);
    expect_type(__LINE__, NDS_SAVE_EEPROM, 0x10000);
    expect_memory(__LINE__, "the first write");
    /* It then reads and writes as that chip. */
    exchange(0x03);
    send_address(0x1234, 2);
    got[0] = exchange(0);
    release();
    CHECK(got[0] == 0x21, "the byte written reads back as %02X", got[0]);
    g_width = 2;
    g_small = false;
    simple(0x06);
    store(0x2000, (const uint8_t *)"\xA1\xA2\xA3\xA4", 4);
    memcpy(g_expected + 0x2000, "\xA1\xA2\xA3\xA4", 4);
    expect_memory(__LINE__, "a later write");
    /* Its page size is not known, so none is imposed: a write that would
     * wrap at the end of the 64 KiB chip's 128-byte page runs straight on. */
    simple(0x06);
    store(0x207E, (const uint8_t *)"\xB1\xB2\xB3\xB4", 4);
    memcpy(g_expected + 0x207E, "\xB1\xB2\xB3\xB4", 4);
    expect_memory(__LINE__, "a write across a 128-byte boundary");

    /* FRAM: more in one command than any EEPROM page holds. Recognised at
     * chip select ... */
    unknown("FRAM, 200 bytes to 0010h");
    first_write(0x02, 0x0010, 2, 200, 0x0010);
    expect_type(__LINE__, NDS_SAVE_FRAM, 0x8000);
    expect_memory(__LINE__, "the first write");
    /* ... or, for a write longer than a FLASH page too, while it is still
     * arriving. */
    unknown("FRAM, 512 bytes to 0010h");
    first_write(0x02, 0x0010, 2, 512, 0x0010);
    expect_type(__LINE__, NDS_SAVE_FRAM, 0x8000);
    expect_memory(__LINE__, "the first write");

    /* EEPROM 128 KiB: 02h with three address bytes and a page of 256. */
    unknown("128 KiB EEPROM, a page to 012300h");
    first_write(0x02, 0x012300, 3, 256, 0x012300);
    expect_type(__LINE__, NDS_SAVE_EEPROM_LARGE, 0x20000);
    expect_memory(__LINE__, "the first write");
    unknown("128 KiB EEPROM, one byte");
    first_write(0x02, 0x000042, 3, 1, 0x000042);
    expect_type(__LINE__, NDS_SAVE_EEPROM_LARGE, 0x20000);
    expect_memory(__LINE__, "the first write");

    /* A 02h write with three address bytes could be FLASH's page program
     * as well. If a FLASH command follows, FLASH it is: here the sector
     * erase a program sends before programming somewhere else. */
    unknown("FLASH, a page program before any erase");
    first_write(0x02, 0x012300, 3, 256, 0x012300);
    expect_type(__LINE__, NDS_SAVE_EEPROM_LARGE, 0x20000);
    memset(g_memory + 0x30000, 0x00, 0x10000);
    simple(0x06);
    flash_command(0xD8, 0x030000, NULL, 0);
    expect_type(__LINE__, NDS_SAVE_FLASH, UNKNOWN_SIZE);
    expect_memory(__LINE__, "the sector erase");
    exchange(0x9F);
    got[0] = exchange(0);
    release();
    CHECK(got[0] == 0x20, "the chip does not answer RDID as FLASH (%02X)", got[0]);
    /* A chip given as the 128 KiB EEPROM stays one whatever is sent. */
    setup(NDS_SAVE_EEPROM_LARGE, 0x20000, 3, "128 KiB EEPROM, given");
    simple(0x06);
    flash_command(0xD8, 0x010000, NULL, 0);
    exchange(0x9F);
    got[0] = exchange(0);
    release();
    expect_type(__LINE__, NDS_SAVE_EEPROM_LARGE, 0x20000);
    expect_memory(__LINE__, "FLASH commands");
    CHECK(got[0] == 0xFF, "an EEPROM answered RDID (%02X)", got[0]);

    /* FLASH: 0Ah with three address bytes ("FLASH does use 0Ah=Write+
     * Erase"), ... */
    unknown("FLASH, a page write to 012300h");
    first_write(0x0A, 0x012300, 3, 256, 0x012300);
    expect_type(__LINE__, NDS_SAVE_FLASH, UNKNOWN_SIZE);
    expect_memory(__LINE__, "the first write");
    /* ... or an erase ("or D8h/DBh=Erase and 02h=Write separatly"), ... */
    unknown("FLASH, a sector erase");
    memset(g_memory + 0x20000, 0x00, 0x10000);
    simple(0x06);
    flash_command(0xD8, 0x020000, NULL, 0);
    expect_type(__LINE__, NDS_SAVE_FLASH, UNKNOWN_SIZE);
    expect_memory(__LINE__, "the erase");
    simple(0x06);
    flash_command(0x02, 0x020000, (const uint8_t *)"\x12\x34", 2);
    g_expected[0x20000] = 0x12;
    g_expected[0x20001] = 0x34;
    expect_memory(__LINE__, "the page program after it");
    unknown("FLASH, a page erase");
    simple(0x06);
    flash_command(0xDB, 0x000100, NULL, 0);
    expect_type(__LINE__, NDS_SAVE_FLASH, UNKNOWN_SIZE);
    /* ... or a request for its ID, which only FLASH answers. */
    unknown("FLASH, RDID");
    exchange(0x9F);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    release();
    expect_type(__LINE__, NDS_SAVE_FLASH, UNKNOWN_SIZE);
    CHECK(got[0] == 0x20 && got[1] == 0x40 && got[2] == 0x14, "RDID from a recognised 1 MiB FLASH: %02X %02X %02X", got[0],
          got[1], got[2]);
    CHECK(!g_dirty, "recognising FLASH from RDID set dirty");
    /* Protection bits a program wrote before the chip was known are an
     * EEPROM's: FLASH's status register has bits 0 and 1 only, and nothing
     * of it is protected. */
    unknown("FLASH after a WRSR");
    simple(0x06);
    exchange(0x01);
    exchange(0x8C);
    release();
    CHECK(status() == 0x8C, "status before the chip is known, after writing 8C: %02X", status());
    exchange(0x9F);
    exchange(0);
    release();
    expect_type(__LINE__, NDS_SAVE_FLASH, UNKNOWN_SIZE);
    CHECK(status() == 0x00, "FLASH status %02X after an earlier WRSR, expected 00", status());
    simple(0x06);
    flash_command(0x0A, 0x0FFF00, (const uint8_t *)"\x5A", 1);
    g_expected[0xFFF00] = 0x5A;
    expect_memory(__LINE__, "a page write to the top of memory");

    /* A first write without WREN still tells the chip, and stores nothing. */
    unknown("EEPROM, first write without WREN");
    exchange(0x02);
    send_address(0x0100, 2);
    for (uint32_t i = 0; i < 32; i++) exchange((uint8_t)i);
    release();
    CHECK(status() == 0x00, "status %02X", status());
    expect_type(__LINE__, NDS_SAVE_EEPROM, 0x10000);
    expect_memory(__LINE__, "a write without WREN");
    CHECK(!g_dirty, "a write without WREN set dirty");

    /* A write whose length says nothing (eight bytes after the command) is
     * passed over; the next one decides. */
    unknown("a write of ambiguous length");
    simple(0x06);
    exchange(0x02);
    for (uint32_t i = 0; i < 8; i++) exchange((uint8_t)(0x40u + i));
    release();
    CHECK(status() == 0x02, "status %02X after the write that was passed over", status());
    CHECK(g_save.type == NDS_SAVE_UNKNOWN, "an ambiguous write decided the type (%d)", (int)g_save.type);
    expect_memory(__LINE__, "the write passed over");
    first_write(0x02, 0x0040, 2, 32, 0x0040);
    expect_type(__LINE__, NDS_SAVE_EEPROM, 0x10000);
    expect_memory(__LINE__, "the write after it");

    /* The chip cannot be larger than the memory it was given. */
    g_size = 0x8000;
    g_name = "FLASH in 32 KiB of memory";
    memset(g_memory, 0xFF, sizeof g_memory);
    memset(g_expected, 0xFF, sizeof g_expected);
    nds_save_init(&g_save, NDS_SAVE_UNKNOWN, 0x8000);
    first_write(0x0A, 0x012300, 3, 256, 0x2300);
    expect_type(__LINE__, NDS_SAVE_FLASH, 0x8000);
    g_size = sizeof g_memory;
    expect_memory(__LINE__, "the first write");

    /* A save file's length names a family that will read it. */
    static const struct {
        uint32_t size;
        int type; /* -1: no chip has this size */
    } files[] = {
        { 0x200, NDS_SAVE_EEPROM_SMALL }, { 0x2000, NDS_SAVE_FRAM },   { 0x8000, NDS_SAVE_FRAM },
        { 0x10000, NDS_SAVE_FRAM },       { 0x20000, NDS_SAVE_EEPROM_LARGE }, { 0x40000, NDS_SAVE_FLASH },
        { 0x80000, NDS_SAVE_FLASH },      { 0x100000, NDS_SAVE_FLASH }, { 0x800000, NDS_SAVE_FLASH },
        { 0, -1 },                        { 0x100, -1 },                { 0x80001, -1 },
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        NdsSaveType type = NDS_SAVE_NONE;
        bool known = nds_save_type_for_size(files[i].size, &type);
        CHECK(known == (files[i].type >= 0) && (!known || (int)type == files[i].type),
              "a save file of %X bytes: %s, type %d", files[i].size, known ? "known" : "not known", (int)type);
    }
}

/* ------------------------------------------------------------------ */
/* A console reset                                                     */
/* ------------------------------------------------------------------ */

/* The console is switched off and on with the card in it: the chip is the
 * same chip over the same memory, and comes up as a chip does at power-up. */
static void test_reset(void)
{
    uint8_t got[2];

    /* FLASH: the write enable latch is clear ("WEL gets reset on
     * Power-up"), deep power-down is over, a command in flight is gone. */
    setup(NDS_SAVE_FLASH, 0x80000, 3, "FLASH across a reset");
    simple(0x06);
    simple(0xB9);
    exchange(0x03);
    exchange(0x01); /* one of three address bytes, and then the power goes */
    nds_save_reset(&g_save);
    CHECK(g_save.type == NDS_SAVE_FLASH && g_save.size == 0x80000, "a reset changed the chip: type %d, %X bytes",
          (int)g_save.type, g_save.size);
    CHECK(status() == 0x00, "status after a reset %02X, expected 00", status());
    exchange(0x9F);
    got[0] = exchange(0);
    release();
    CHECK(got[0] == 0x20, "RDID after a reset %02X: the chip is still in deep power-down", got[0]);
    flash_command(0x0A, 0x000100, (const uint8_t *)"\x00", 1);
    expect_memory(__LINE__, "a page write after a reset, without WREN");
    exchange(0x03);
    send_address(0x000123, 3);
    got[0] = exchange(0);
    release();
    CHECK(got[0] == g_expected[0x123], "a read after a reset returns %02X, expected %02X", got[0], g_expected[0x123]);

    /* EEPROM: the protection bits "are non-volatile (remain intact during
     * power-down)". */
    setup(NDS_SAVE_EEPROM, 0x2000, 2, "EEPROM across a reset");
    simple(0x06);
    exchange(0x01);
    exchange(0x0C);
    release();
    simple(0x06);
    nds_save_reset(&g_save);
    CHECK(status() == 0x0C, "status after a reset %02X, expected 0C (protection kept, latch clear)", status());
    simple(0x06);
    store(0x0010, (const uint8_t *)"\x00\x00", 2);
    expect_memory(__LINE__, "a write to protected memory after a reset");

    /* A chip found out from its first write stays found out, the write
     * still waiting to be stored is stored, and the chip keeps what
     * recognition gave it (here: no page limit). */
    unknown("EEPROM recognised, then a reset");
    first_write(0x02, 0x0100, 2, 32, 0x0100);
    nds_save_reset(&g_save);
    expect_type(__LINE__, NDS_SAVE_EEPROM, 0x10000);
    g_width = 2;
    g_small = false;
    simple(0x06);
    store(0x207E, (const uint8_t *)"\xB1\xB2\xB3\xB4", 4);
    memcpy(g_expected + 0x207E, "\xB1\xB2\xB3\xB4", 4);
    expect_memory(__LINE__, "writes either side of a reset");
    unknown("a first write, a reset before anything else");
    simple(0x06);
    exchange(0x02);
    send_address(0x0040, 2);
    for (uint32_t i = 0; i < 32; i++) {
        g_expected[0x40 + i] = (uint8_t)(0x60u + i);
        exchange(g_expected[0x40 + i]);
    }
    release();
    nds_save_reset(&g_save);
    CHECK(status() == 0x00, "status %02X", status());
    expect_memory(__LINE__, "the first write");
    /* One guessed to be the 128 KiB EEPROM can still turn out to be FLASH. */
    unknown("128 KiB EEPROM guessed, a reset, then an erase");
    first_write(0x02, 0x012300, 3, 256, 0x012300);
    nds_save_reset(&g_save);
    expect_type(__LINE__, NDS_SAVE_EEPROM_LARGE, 0x20000);
    simple(0x06);
    flash_command(0xD8, 0x030000, NULL, 0);
    expect_type(__LINE__, NDS_SAVE_FLASH, UNKNOWN_SIZE);
    /* A write cut off by the reset is forgotten, and the chip is still
     * unknown. */
    unknown("a first write cut off by a reset");
    simple(0x06);
    exchange(0x02);
    send_address(0x0040, 2);
    exchange(0x55);
    nds_save_reset(&g_save);
    CHECK(g_save.type == NDS_SAVE_UNKNOWN, "a write cut off by a reset decided the type (%d)", (int)g_save.type);
    first_write(0x02, 0x20, 1, 16, 0x20);
    expect_type(__LINE__, NDS_SAVE_EEPROM_SMALL, 0x200);
    expect_memory(__LINE__, "the first whole write");

    /* An infrared card is still one. */
    setup(NDS_SAVE_FLASH, 0x80000, 3, "infrared card across a reset");
    nds_save_set_infrared(&g_save, true);
    exchange(0x00);
    exchange(0x9F);
    nds_save_reset(&g_save);
    exchange(0x9F);
    got[0] = exchange(0);
    release();
    exchange(0x00);
    exchange(0x9F);
    got[1] = exchange(0);
    release();
    CHECK(got[0] != 0x20 && got[1] == 0x20, "after a reset the chip answers %02X without the prefix and %02X with it",
          got[0], got[1]);
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

static void expect_game(int line, const char *code, bool listed, NdsSaveType want_type, uint32_t want_size)
{
    NdsSaveType type = NDS_SAVE_UNKNOWN;
    uint32_t size = 0xFFFFFFFFu;
    bool found = nds_save_lookup(code, &type, &size);
    g_checks++;
    if (found != listed || (found && (type != want_type || size != want_size))) {
        g_failures++;
        printf("FAIL %s:%d: %s: %s, type %d, %X bytes; expected %s, type %d, %X bytes\n", __FILE__, line, code,
               found ? "listed" : "not listed", (int)type, size, listed ? "listed" : "not listed", (int)want_type,
               want_size);
    }
}

static void test_table(void)
{
    /* The games GBATEK names as examples of each chip ("DS Cartridge
     * Backup", "DS Firmware Serial Flash Memory", "DS Cartridge NAND"). */
    expect_game(__LINE__, "AMFE", true, NDS_SAVE_EEPROM_SMALL, 0x200);   /* "Metroid Demo": First Hunt */
    expect_game(__LINE__, "ASME", true, NDS_SAVE_EEPROM, 0x2000);        /* "Super Mario DS" */
    expect_game(__LINE__, "AWKE", true, NDS_SAVE_EEPROM, 0x10000);       /* "Downhill Jam" */
    expect_game(__LINE__, "C2SE", true, NDS_SAVE_EEPROM_LARGE, 0x20000); /* "Explorers of Sky" */
    expect_game(__LINE__, "AH9E", true, NDS_SAVE_FLASH, 0x40000);        /* "Skateland" */
    expect_game(__LINE__, "AMCE", true, NDS_SAVE_FLASH, 0x40000);        /* "Mariokart" */
    expect_game(__LINE__, "AZEP", true, NDS_SAVE_FLASH, 0x80000);        /* "DS Zelda, NTR-AZEP-0" */
    expect_game(__LINE__, "ADAE", true, NDS_SAVE_FLASH, 0x80000);        /* "P-Letter Diamond, ADAE" */
    expect_game(__LINE__, "BKIP", true, NDS_SAVE_FLASH, 0x100000);       /* "Spirit Tracks, NTR-BKIP" */
    expect_game(__LINE__, "VAAV", true, NDS_SAVE_FLASH, 0x800000);       /* "Art Academy only, TWL-VAAV" */
    expect_game(__LINE__, "UORE", true, NDS_SAVE_NAND, 0x1000000);       /* "16MB for WarioWare DIY" */
    expect_game(__LINE__, "UXBP", true, NDS_SAVE_NAND, 0x800000);        /* "8MB for Jam with the Band" */
    /* "Over the Hedge ... does actually use 0.5Kbyte EEPROM" and "force
     * 64Kbyte EEPROM on gamecode ARFx". */
    expect_game(__LINE__, "AH5E", true, NDS_SAVE_EEPROM_SMALL, 0x200);
    expect_game(__LINE__, "ARFE", true, NDS_SAVE_EEPROM, 0x10000);
    /* The first game, in its three releases. */
    expect_game(__LINE__, "AZEE", true, NDS_SAVE_FLASH, 0x80000);
    expect_game(__LINE__, "AZEJ", true, NDS_SAVE_FLASH, 0x80000);

    /* Not games, or not listed. */
    expect_game(__LINE__, "ZZZZ", false, NDS_SAVE_UNKNOWN, 0);
    expect_game(__LINE__, "0000", false, NDS_SAVE_UNKNOWN, 0);
    expect_game(__LINE__, "azee", false, NDS_SAVE_UNKNOWN, 0);
    expect_game(__LINE__, "AZE ", false, NDS_SAVE_UNKNOWN, 0);
    expect_game(__LINE__, "AZE\xC9", false, NDS_SAVE_UNKNOWN, 0);
    /* Homebrew built with libnds carries this code. */
    expect_game(__LINE__, "####", false, NDS_SAVE_UNKNOWN, 0);

    /* The table itself: in order, each word a game code and a kind that
     * exists; first and last entries findable. */
    bool ordered = true, kinds = true;
    for (uint32_t i = 0; i < nds_save_db_count; i++) {
        if (i && (nds_save_db[i] >> 4) <= (nds_save_db[i - 1] >> 4)) ordered = false;
        if ((nds_save_db[i] & 15u) >= nds_save_db_kind_count) kinds = false;
        if ((nds_save_db[i] >> 4) >= 36u * 36u * 36u * 36u) kinds = false;
    }
    CHECK(ordered, "the table is not in ascending order of game code");
    CHECK(kinds, "the table has a word that is not a game code and a kind");
    CHECK(nds_save_db_count == 6776, "the table has %u games, expected 6776", nds_save_db_count);
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int pass = 0; pass < 2 && nds_save_db_count; pass++) {
        uint32_t word = nds_save_db[pass ? nds_save_db_count - 1 : 0];
        uint32_t code = word >> 4;
        char text[5] = { digits[code / 46656u % 36u], digits[code / 1296u % 36u], digits[code / 36u % 36u],
                         digits[code % 36u], 0 };
        NdsSaveType type;
        uint32_t size;
        CHECK(nds_save_lookup(text, &type, &size), "the %s entry of the table, %s, is not found", pass ? "last" : "first",
              text);
    }
    /* Every kind is a type and a size nds_save_init() accepts. */
    for (uint32_t kind = 0; kind < nds_save_db_kind_count; kind++) {
        NdsSave save;
        NdsSaveType type = (NdsSaveType)nds_save_db_kinds[kind].type;
        uint32_t size = nds_save_db_kinds[kind].size;
        nds_save_init(&save, type, size);
        bool chip = type != NDS_SAVE_NONE && type != NDS_SAVE_NAND;
        CHECK(chip ? (save.type == type && save.size == size) : save.type == NDS_SAVE_NONE,
              "kind %u (type %d, %X bytes) starts as type %d with %X bytes", kind, (int)type, size, (int)save.type,
              save.size);
    }
}

/* ------------------------------------------------------------------ */
/* Phantom Hourglass                                                   */
/* ------------------------------------------------------------------ */

/*
 * The first game from end to end: its chip from the table, then that chip
 * through the protocol. GBATEK: "FLASH 512K bytes ST 45PE40V6 (eg. DS
 * Zelda, NTR-AZEP-0)", "ID 20h,40h,13h".
 */
static void test_phantom_hourglass(void)
{
    static uint8_t block[0x2000];
    NdsSaveType type = NDS_SAVE_NONE;
    uint32_t size = 0;
    uint8_t got[3];

    CHECK(nds_save_lookup("AZEE", &type, &size), "AZEE is not in the table");
    CHECK(type == NDS_SAVE_FLASH && size == 0x80000, "AZEE: type %d with %X bytes, expected FLASH with 80000", (int)type,
          size);
    CHECK(!nds_save_has_infrared("AZEE"), "AZEE is taken for an infrared card");

    setup(type, size, 3, "Phantom Hourglass");
    memset(g_memory, 0xFF, size); /* a card that has never been saved to */
    memset(g_expected, 0xFF, size);
    nds_save_init(&g_save, type, size);

    exchange(0x9F);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    release();
    CHECK(got[0] == 0x20 && got[1] == 0x40 && got[2] == 0x13, "the chip's ID is %02X %02X %02X, expected 20 40 13", got[0],
          got[1], got[2]);

    /* Save 8 KiB at 10000h: for each page WREN, page write, then the status
     * register until "write in progress" is clear. */
    for (uint32_t i = 0; i < sizeof block; i++) block[i] = pattern(i, 0x99);
    for (uint32_t at = 0; at < sizeof block; at += 256) {
        simple(0x06);
        CHECK(status() & 0x02, "the write enable latch did not set");
        flash_command(0x0A, 0x010000 + at, block + at, 256);
        exchange(0x05);
        int polls = 0;
        while ((exchange(0) & 0x01) && polls < 100) polls++;
        release();
        CHECK(polls < 100, "write in progress never cleared");
    }
    memcpy(g_expected + 0x10000, block, sizeof block);
    expect_memory(__LINE__, "the save");
    CHECK(g_dirty, "saving did not set dirty");

    /* Load it back. */
    bool same = true;
    exchange(0x03);
    send_address(0x010000, 3);
    for (uint32_t i = 0; i < sizeof block; i++) same = same && exchange(0) == block[i];
    release();
    CHECK(same, "the save read back differs from the save written");

    /* The memory is the save file: a new chip with the same bytes has the
     * save. */
    nds_save_init(&g_save, type, size);
    exchange(0x03);
    send_address(0x010000, 3);
    same = exchange(0) == block[0] && exchange(0) == block[1];
    release();
    CHECK(same, "the save is not there after the chip is set up again over the same memory");
}

int main(void)
{
    static const uint8_t id_256k[3] = { 0x20, 0x40, 0x12 }; /* "ST M45PE20 - 256 KBytes" */
    static const uint8_t id_512k[3] = { 0x20, 0x40, 0x13 }; /* "ST 45PE40V6 - 512 KBytes (DS Zelda)" */
    static const uint8_t id_1m[3] = { 0x20, 0x40, 0x14 };   /* "ST 45PE80V6 - 1024 Kbytes (eg. Spirit Tracks)" */
    static const uint8_t id_8m[3] = { 0xC2, 0x20, 0x17 };   /* "Macronix MX25L6445EZNI-10G 8192 Kbytes" */

    /* "EEPROM 0.5K bytes, 16 bytes", "8K, 32", "64K, 128", "128K, ?" (the
     * 128 KiB part, ST's M95M01, has pages of 256); "FRAM 8K, 32K, No
     * limit". */
    test_eeprom(NDS_SAVE_EEPROM_SMALL, 0x200, 1, 16, "EEPROM 0.5 KiB");
    test_eeprom(NDS_SAVE_EEPROM, 0x2000, 2, 32, "EEPROM 8 KiB");
    test_eeprom(NDS_SAVE_EEPROM, 0x10000, 2, 128, "EEPROM 64 KiB");
    test_eeprom(NDS_SAVE_EEPROM_LARGE, 0x20000, 3, 256, "EEPROM 128 KiB");
    test_eeprom(NDS_SAVE_FRAM, 0x2000, 2, 0, "FRAM 8 KiB");
    test_eeprom(NDS_SAVE_FRAM, 0x8000, 2, 0, "FRAM 32 KiB");

    test_flash(0x40000, id_256k, "FLASH 256 KiB");
    test_flash(0x80000, id_512k, "FLASH 512 KiB");
    test_flash(0x100000, id_1m, "FLASH 1 MiB");
    test_flash(0x800000, id_8m, "FLASH 8 MiB");

    test_none(NDS_SAVE_NONE, 0, "no save");
    test_none(NDS_SAVE_NAND, 0x800000, "NAND card");
    test_none(NDS_SAVE_FLASH, 0, "FLASH of no bytes");
    test_infrared();
    test_unknown();
    test_reset();
    test_table();
    test_phantom_hourglass();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
