/*
 * The ARM7's SPI bus and its three devices, driven through the two
 * registers the way programs drive them (the sequences are those of
 * libnds's spi.c, firmware.c, tsc.c and microphone.c: set SPICNT with
 * "hold", exchange bytes through SPIDATA, end by clearing hold before the
 * last byte or by writing 0 to SPICNT).
 *
 * Expected values come from GBATEK: "DS Serial Peripheral Interface Bus
 * (SPI)", "DS Firmware Serial Flash Memory", "DS Touch Screen Controller
 * (TSC)" and "DS Power Management Device". The test keeps its own copy of
 * what the flash should hold and compares the whole image after every
 * operation.
 */
#include <stdio.h>
#include <string.h>

#include "nds_firmware.h"
#include "nds_spi.h"

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

#define SPICNT 0x040001C0u
#define SPIDATA 0x040001C2u

enum {
    ENABLE = 0x8000,
    IRQ = 0x4000,
    HOLD = 0x0800,
    WIDE = 0x0400,
    POWER = 0x0000,
    FIRMWARE = 0x0100,
    TOUCH = 0x0200,
    NOTHING = 0x0300,
};

static NdsSpi g_spi;
static uint8_t g_flash[NDS_FIRMWARE_SIZE];    /* the image the device works on */
static uint8_t g_expected[NDS_FIRMWARE_SIZE]; /* what the documentation says it should hold */
static bool g_irq;

static void control(uint32_t value)
{
    bool irq = nds_spi_write(&g_spi, g_flash, SPICNT, value, 2);
    CHECK(!irq, "a write to SPICNT asked for the interrupt");
}

static uint8_t exchange(uint8_t out)
{
    g_irq = nds_spi_write(&g_spi, g_flash, SPIDATA, out, 2);
    uint32_t in = nds_spi_read(&g_spi, SPIDATA, 2);
    CHECK((in & 0xFF00u) == 0, "SPIDATA bits 8-15 read %04X", in);
    CHECK((nds_spi_read(&g_spi, SPICNT, 2) & 0x0080u) == 0, "busy is set after a transfer");
    return (uint8_t)in;
}

static void expect_image(int line, const char *what)
{
    g_checks++;
    if (memcmp(g_flash, g_expected, sizeof g_flash) != 0) {
        uint32_t at = 0;
        while (g_flash[at] == g_expected[at]) at++;
        g_failures++;
        printf("FAIL %s:%d: %s: flash differs at %05X (%02X, expected %02X)\n", __FILE__, line, what, at, g_flash[at],
               g_expected[at]);
        memcpy(g_flash, g_expected, sizeof g_flash);
    }
}

/* ------------------------------------------------------------------ */
/* The bus                                                             */
/* ------------------------------------------------------------------ */

static void test_bus(void)
{
    nds_spi_reset(&g_spi);
    CHECK(nds_spi_read(&g_spi, SPICNT, 4) == 0, "registers after reset read %08X", nds_spi_read(&g_spi, SPICNT, 4));

    /* Bits 0-1, 8-11, 14 and 15 can be written; 2-6, 12 and 13 are unused;
     * 7 is the busy flag. */
    control(0xFFFF);
    CHECK(nds_spi_read(&g_spi, SPICNT, 2) == 0xCF03, "SPICNT after writing FFFF reads %04X, expected CF03",
          nds_spi_read(&g_spi, SPICNT, 2));
    control(0);
    CHECK(nds_spi_read(&g_spi, SPICNT, 2) == 0, "SPICNT after writing 0 reads %04X", nds_spi_read(&g_spi, SPICNT, 2));

    /* Byte and word access. */
    nds_spi_write(&g_spi, g_flash, SPICNT + 1, 0x89, 1);
    CHECK(nds_spi_read(&g_spi, SPICNT, 2) == 0x8900, "a byte written to 0x040001C1: SPICNT %04X",
          nds_spi_read(&g_spi, SPICNT, 2));
    nds_spi_write(&g_spi, g_flash, SPICNT, 0x02, 1);
    CHECK(nds_spi_read(&g_spi, SPICNT, 2) == 0x8902, "a byte written to 0x040001C0: SPICNT %04X",
          nds_spi_read(&g_spi, SPICNT, 2));
    CHECK(nds_spi_read(&g_spi, SPICNT + 1, 1) == 0x89 && nds_spi_read(&g_spi, SPICNT, 1) == 0x02,
          "SPICNT read a byte at a time");
    nds_spi_write(&g_spi, g_flash, SPIDATA, 0x9F, 1); /* firmware: read ID */
    nds_spi_write(&g_spi, g_flash, SPIDATA, 0x00, 1);
    CHECK(nds_spi_read(&g_spi, SPIDATA, 1) == 0x20, "SPIDATA read as a byte: %02X", nds_spi_read(&g_spi, SPIDATA, 1));
    CHECK(nds_spi_read(&g_spi, SPICNT, 4) == 0x00208902u, "both registers read as a word: %08X",
          nds_spi_read(&g_spi, SPICNT, 4));
    CHECK(nds_spi_read(&g_spi, SPIDATA + 1, 1) == 0, "SPIDATA's high byte reads %02X", nds_spi_read(&g_spi, SPIDATA + 1, 1));
    /* A write to SPIDATA's unused high byte is not a transfer. */
    nds_spi_write(&g_spi, g_flash, SPIDATA + 1, 0x00, 1);
    CHECK(exchange(0) == 0x40, "a write to 0x040001C3 clocked the bus");
    control(0);

    /* One word write: SPICNT is set, then the byte goes out under it. */
    nds_spi_write(&g_spi, g_flash, SPICNT, 0x009F0000u | ENABLE | HOLD | FIRMWARE, 4);
    CHECK(exchange(0) == 0x20, "a word write did not set SPICNT and then send the byte");
    control(0);

    /* With the bus off, a write to SPIDATA does nothing. */
    control(HOLD | FIRMWARE | IRQ);
    bool irq = nds_spi_write(&g_spi, g_flash, SPIDATA, 0x9F, 2);
    CHECK(!irq, "a transfer with the bus disabled raised the interrupt");
    control(ENABLE | HOLD | FIRMWARE);
    CHECK(exchange(0x9F) == 0 && exchange(0) == 0x20, "the bus clocked a byte while it was disabled");
    control(0);

    /* The interrupt: when a transfer finishes with bit 14 set. */
    control(ENABLE | FIRMWARE | IRQ);
    exchange(0x05);
    CHECK(g_irq, "no interrupt with SPICNT bit 14 set");
    control(ENABLE | FIRMWARE);
    exchange(0x05);
    CHECK(!g_irq, "an interrupt with SPICNT bit 14 clear");

    /* Hold keeps the device selected from byte to byte; a transfer without
     * it is the last of the command. */
    control(ENABLE | HOLD | FIRMWARE);
    exchange(0x9F);
    CHECK(exchange(0) == 0x20, "first ID byte %02X", g_spi.data);
    control(ENABLE | FIRMWARE);
    CHECK(exchange(0) == 0x40, "the device was released when hold was cleared, before the last byte");
    control(ENABLE | HOLD | FIRMWARE);
    CHECK(exchange(0) != 0x12, "the device was still selected after a transfer without hold");

    /* Switching the bus off releases the device (libnds ends every access
     * this way). */
    control(ENABLE | HOLD | FIRMWARE);
    exchange(0x9F);
    exchange(0);
    control(0);
    control(ENABLE | HOLD | FIRMWARE);
    exchange(0x9F);
    CHECK(exchange(0) == 0x20, "disabling the bus did not end the command");
    control(0);

    /* So does pointing the bus at another device. */
    control(ENABLE | HOLD | FIRMWARE);
    exchange(0x9F);
    exchange(0);
    control(ENABLE | HOLD | TOUCH);
    control(ENABLE | HOLD | FIRMWARE);
    exchange(0x9F);
    CHECK(exchange(0) == 0x20, "selecting another device did not end the command");
    control(0);

    /* Device 3 is reserved: transfers complete, nothing answers. */
    control(ENABLE | HOLD | NOTHING | IRQ);
    CHECK(exchange(0x9F) == 0 && exchange(0) == 0, "device 3 answered");
    CHECK(g_irq, "no interrupt for a transfer to device 3");
    control(0);

    /* The 16-bit mode: "only each 2nd byte will appear in SPIDATA". */
    control(ENABLE | HOLD | WIDE | FIRMWARE);
    CHECK(exchange(0x9F) == 0x20, "16-bit mode: SPIDATA after the first write is %02X, expected the 2nd byte, 20",
          g_spi.data);
    CHECK(exchange(0) == 0x12, "16-bit mode: SPIDATA after the second write is %02X, expected the 4th byte, 12",
          g_spi.data);
    control(0);
}

/* ------------------------------------------------------------------ */
/* Firmware flash                                                      */
/* ------------------------------------------------------------------ */

static void flash_begin(void) { control(ENABLE | HOLD | FIRMWARE); }
static void flash_end(void) { control(0); }

static void flash_address(uint32_t address)
{
    exchange((uint8_t)(address >> 16));
    exchange((uint8_t)(address >> 8));
    exchange((uint8_t)address);
}

static void flash_simple(uint8_t instruction)
{
    flash_begin();
    exchange(instruction);
    flash_end();
}

static uint8_t flash_status(void)
{
    flash_begin();
    exchange(0x05);
    uint8_t status = exchange(0);
    flash_end();
    return status;
}

/* PW, PP: instruction, address, data. */
static void flash_write(uint8_t instruction, uint32_t address, const uint8_t *data, uint32_t count)
{
    flash_begin();
    exchange(instruction);
    flash_address(address);
    for (uint32_t i = 0; i < count; i++) exchange(data[i]);
    flash_end();
}

/* PE, SE: instruction and address. */
static void flash_erase(uint8_t instruction, uint32_t address)
{
    flash_begin();
    exchange(instruction);
    flash_address(address);
    flash_end();
}

static void test_flash(void)
{
    NdsUserSettings settings;
    uint8_t data[300];
    uint8_t got[8];

    nds_user_settings_default(&settings);
    nds_firmware_build(g_flash, &settings);
    /* Something to tell places apart in the areas the image leaves erased. */
    for (uint32_t i = 0x10000; i < 0x3FA00; i++) g_flash[i] = (uint8_t)(i * 7u + (i >> 8) * 13u + (i >> 16));
    memcpy(g_expected, g_flash, sizeof g_flash);
    nds_spi_reset(&g_spi);
    CHECK(!nds_spi_firmware_dirty(&g_spi), "flash dirty after reset");

    /* 9Fh RDID: "ID 20h,40h,12h - ST M45PE20 - 256 KBytes". */
    flash_begin();
    exchange(0x9F);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    flash_end();
    CHECK(got[0] == 0x20 && got[1] == 0x40 && got[2] == 0x12, "RDID %02X %02X %02X, expected 20 40 12", got[0], got[1],
          got[2]);

    /* 03h READ: the data stream starts after the three address bytes and
     * goes on as long as it is clocked. */
    flash_begin();
    exchange(0x03);
    flash_address(0x012345);
    bool same = true;
    for (uint32_t i = 0; i < 600; i++) same = same && exchange(0) == g_expected[0x012345 + i];
    flash_end();
    CHECK(same, "READ at 012345h returned other than the image");
    /* The header: the user settings offset a game looks up first. */
    flash_begin();
    exchange(0x03);
    flash_address(0x000020);
    got[0] = exchange(0);
    got[1] = exchange(0);
    flash_end();
    CHECK(((uint32_t)got[0] | ((uint32_t)got[1] << 8)) * 8u == 0x3FE00u, "header[20h] read through the bus: %02X %02X",
          got[0], got[1]);
    /* Past the last byte the address counter starts again at 0. */
    flash_begin();
    exchange(0x03);
    flash_address(0x03FFFE);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    got[3] = exchange(0);
    flash_end();
    CHECK(got[0] == g_expected[0x3FFFE] && got[1] == g_expected[0x3FFFF] && got[2] == g_expected[0] &&
              got[3] == g_expected[1],
          "READ across the end of the chip: %02X %02X %02X %02X", got[0], got[1], got[2], got[3]);
    /* The chip decodes 18 address bits. */
    flash_begin();
    exchange(0x03);
    flash_address(0xFD2345);
    got[0] = exchange(0);
    flash_end();
    CHECK(got[0] == g_expected[0x012345], "READ at FD2345h is %02X, expected the byte at 012345h", got[0]);

    /* 0Bh FAST: one dummy byte between the address and the data. */
    flash_begin();
    exchange(0x0B);
    flash_address(0x023456);
    exchange(0);
    got[0] = exchange(0);
    got[1] = exchange(0);
    flash_end();
    CHECK(got[0] == g_expected[0x023456] && got[1] == g_expected[0x023457], "FAST read %02X %02X, expected %02X %02X",
          got[0], got[1], g_expected[0x023456], g_expected[0x023457]);

    /* 05h RDSR, 06h WREN, 04h WRDI. Bit 1 is the write enable latch, bit 0
     * write in progress, the rest zero; the register repeats endlessly. */
    CHECK(flash_status() == 0x00, "status after reset %02X, expected 00", flash_status());
    flash_simple(0x06);
    flash_begin();
    exchange(0x05);
    got[0] = exchange(0);
    got[1] = exchange(0);
    got[2] = exchange(0);
    flash_end();
    CHECK(got[0] == 0x02 && got[1] == 0x02 && got[2] == 0x02, "status after WREN %02X %02X %02X, expected 02 repeated",
          got[0], got[1], got[2]);
    flash_simple(0x04);
    CHECK(flash_status() == 0x00, "status after WRDI %02X, expected 00", flash_status());
    CHECK(!nds_spi_firmware_dirty(&g_spi), "reading the flash made it dirty");

    /* 0Ah PW needs WREN first. */
    for (uint32_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(0xA0u + i * 3u);
    flash_write(0x0A, 0x02A010, data, 16);
    expect_image(__LINE__, "page write without WREN");
    CHECK(!nds_spi_firmware_dirty(&g_spi), "a rejected write made the flash dirty");

    /* PW: the bytes sent replace those addressed, the rest of the page
     * stays, and the latch drops when the instruction completes. */
    flash_simple(0x06);
    flash_write(0x0A, 0x02A010, data, 16);
    memcpy(g_expected + 0x02A010, data, 16);
    expect_image(__LINE__, "page write of 16 bytes");
    CHECK(flash_status() == 0x00, "status after a page write %02X: WEL and WIP should be clear", flash_status());
    CHECK(nds_spi_firmware_dirty(&g_spi), "a write did not make the flash dirty");
    nds_spi_clear_firmware_dirty(&g_spi);
    CHECK(!nds_spi_firmware_dirty(&g_spi), "the dirty flag did not clear");
    /* ... so a second write needs a second WREN. */
    flash_write(0x0A, 0x02A020, data, 16);
    expect_image(__LINE__, "second page write without a second WREN");

    /* "Write/Program may not cross page-boundaries": past the page's end
     * the bytes go to its start. */
    flash_simple(0x06);
    flash_write(0x0A, 0x02B0FE, data, 4);
    g_expected[0x02B0FE] = data[0];
    g_expected[0x02B0FF] = data[1];
    g_expected[0x02B000] = data[2];
    g_expected[0x02B001] = data[3];
    expect_image(__LINE__, "page write across the end of a page");
    /* More than 256 bytes: the last 256 are the ones written (ST's
     * datasheet for the M45PE20). */
    flash_simple(0x06);
    flash_write(0x0A, 0x02C000, data, 258);
    memcpy(g_expected + 0x02C000, data, 256);
    g_expected[0x02C000] = data[256];
    g_expected[0x02C001] = data[257];
    expect_image(__LINE__, "page write of 258 bytes");

    /* 02h PP changes bits from 1 to 0 only. */
    flash_write(0x02, 0x02D000, data, 8);
    expect_image(__LINE__, "page program without WREN");
    flash_simple(0x06);
    flash_write(0x02, 0x02D000, data, 8);
    for (uint32_t i = 0; i < 8; i++) g_expected[0x02D000 + i] &= data[i];
    expect_image(__LINE__, "page program");
    CHECK(flash_status() == 0x00, "status after a page program %02X", flash_status());
    /* Into erased flash it stores the bytes as they are. */
    CHECK(g_expected[0x3FD00] == 0xFF, "the image's unused slot is not erased");
    flash_simple(0x06);
    flash_write(0x02, 0x3FD00, data, 8);
    memcpy(g_expected + 0x3FD00, data, 8);
    expect_image(__LINE__, "page program into erased flash");
    /* A byte position programmed twice in one instruction keeps the later
     * byte: 257 bytes from the middle of the page, the first 0Fh and the
     * 257th, back at the same place, F0h. The other 255 are FFh, which
     * programs nothing. */
    memset(data, 0xFF, sizeof data);
    data[0] = 0x0F;
    data[256] = 0xF0;
    CHECK(g_expected[0x3FD80] == 0xFF, "the byte the next check programs is not erased");
    flash_simple(0x06);
    flash_write(0x02, 0x3FD80, data, 257);
    g_expected[0x3FD80] = 0xF0;
    expect_image(__LINE__, "page program of 257 bytes");

    /* DBh PE: one page of 100h bytes becomes FFh. Any address inside the
     * page names it. */
    flash_erase(0xDB, 0x02A0AB);
    expect_image(__LINE__, "page erase without WREN");
    flash_simple(0x06);
    flash_erase(0xDB, 0x02A0AB);
    memset(g_expected + 0x02A000, 0xFF, 0x100);
    expect_image(__LINE__, "page erase");
    CHECK(flash_status() == 0x00, "status after a page erase %02X", flash_status());

    /* D8h SE: a sector of 10000h bytes. */
    flash_erase(0xD8, 0x021234);
    expect_image(__LINE__, "sector erase without WREN");
    flash_simple(0x06);
    flash_erase(0xD8, 0x021234);
    memset(g_expected + 0x020000, 0xFF, 0x10000);
    expect_image(__LINE__, "sector erase");
    CHECK(flash_status() == 0x00, "status after a sector erase %02X", flash_status());
    CHECK(nds_spi_firmware_dirty(&g_spi), "an erase did not make the flash dirty");

    /* /W is low on a DS: the first 256 pages cannot be written, programmed
     * or erased. */
    for (uint32_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)i;
    flash_simple(0x06);
    flash_write(0x0A, 0x000000, data, 64);
    flash_simple(0x06);
    flash_write(0x0A, 0x00FF00, data, 64);
    flash_simple(0x06);
    flash_write(0x02, 0x000036, data, 6);
    flash_simple(0x06);
    flash_erase(0xDB, 0x000000);
    flash_simple(0x06);
    flash_erase(0xD8, 0x00FFFF);
    expect_image(__LINE__, "writes and erases in the write-protected first 256 pages");
    /* The page after them can. */
    flash_simple(0x06);
    flash_write(0x0A, 0x010000, data, 4);
    memcpy(g_expected + 0x010000, data, 4);
    expect_image(__LINE__, "page write to the first unprotected page");

    /* B9h DP: "ignores all further instructions, except RDP" (ABh). */
    flash_simple(0xB9);
    flash_begin();
    exchange(0x9F);
    got[0] = exchange(0);
    flash_end();
    CHECK(got[0] != 0x20, "RDID answered in deep power-down");
    flash_begin();
    exchange(0x03);
    flash_address(0x010000);
    got[0] = exchange(0);
    got[1] = exchange(0);
    flash_end();
    CHECK(!(got[0] == data[0] && got[1] == data[1]), "READ answered in deep power-down");
    flash_simple(0x06);
    flash_write(0x0A, 0x010000, data + 100, 4);
    expect_image(__LINE__, "page write in deep power-down");
    flash_simple(0xAB);
    flash_begin();
    exchange(0x9F);
    got[0] = exchange(0);
    flash_end();
    CHECK(got[0] == 0x20, "RDID after release from deep power-down: %02X", got[0]);
    CHECK(flash_status() == 0x00, "WREN was obeyed in deep power-down (status %02X)", flash_status());

    /* Bytes that are not instructions do nothing. */
    flash_simple(0x06);
    flash_begin();
    exchange(0x01);
    exchange(0xFF);
    exchange(0xFF);
    flash_end();
    flash_begin();
    exchange(0xFF);
    exchange(0x00);
    flash_end();
    flash_begin();
    exchange(0x00);
    exchange(0x00);
    flash_end();
    expect_image(__LINE__, "unknown instructions");
    CHECK(flash_status() == 0x02, "status %02X: an unknown instruction changed the latch", flash_status());
    flash_simple(0x04);

    /*
     * The whole of what a program does to save new user settings (libnds's
     * writeFirmwarePage): WREN, read the status until the latch shows,
     * page write, read the status until "write in progress" clears, read
     * back. The new copy goes over the older one with the next counter.
     */
    uint8_t page[256];
    NdsUserSettings changed = settings;
    changed.favourite_colour = 9;
    changed.language = NDS_LANGUAGE_FRENCH;
    memset(page, 0xFF, sizeof page);
    nds_user_settings_ram_block(&changed, page);
    page[0x70] = 2; /* copy 0 held 0, copy 1 holds 1 */
    page[0x71] = 0;
    uint16_t crc = nds_crc16(page, 0x70, 0xFFFF);
    page[0x72] = (uint8_t)crc;
    page[0x73] = (uint8_t)(crc >> 8);

    flash_simple(0x06);
    flash_begin();
    exchange(0x05);
    int polls = 0;
    while (!(exchange(0) & 0x02) && polls < 100) polls++;
    flash_end();
    CHECK(polls < 100, "the write enable latch never showed in the status register");
    flash_write(0x0A, 0x3FE00, page, 256);
    flash_begin();
    exchange(0x05);
    polls = 0;
    while ((exchange(0) & 0x01) && polls < 100) polls++;
    flash_end();
    CHECK(polls < 100, "write in progress never cleared");
    flash_begin();
    exchange(0x03);
    flash_address(0x3FE00);
    same = true;
    for (uint32_t i = 0; i < 256; i++) same = same && exchange(0) == page[i];
    flash_end();
    CHECK(same, "the page read back differs from the page written");
    memcpy(g_expected + 0x3FE00, page, 256);
    expect_image(__LINE__, "user settings page write");
    uint32_t offset = 0;
    CHECK(nds_firmware_current_settings(g_flash, &offset) && offset == 0x3FE00u,
          "the rewritten copy is not the current one (offset %05X)", offset);
}

/* ------------------------------------------------------------------ */
/* Touch screen controller                                             */
/* ------------------------------------------------------------------ */

/* One 12-bit reading, as libnds's tscRead() takes it: the control byte,
 * then two bytes, the last without hold. */
static uint32_t tsc_read(uint8_t command, uint8_t raw[2])
{
    uint8_t high, low;
    control(ENABLE | HOLD | TOUCH | 1);
    exchange(command);
    high = exchange(0);
    control(ENABLE | TOUCH | 1);
    low = exchange(0);
    if (raw) {
        raw[0] = high;
        raw[1] = low;
    }
    /* "One dummy bit (zero), followed by the ... conversion result (MSB
     * first)": seven bits in the first byte, the rest at the top of the
     * second. */
    return (((uint32_t)high << 5) | ((uint32_t)low >> 3)) & 0xFFFu;
}

static uint8_t tsc_command(uint32_t channel, bool eight_bit, bool single_ended)
{
    return (uint8_t)(0x80u | (channel << 4) | (eight_bit ? 0x08u : 0u) | (single_ended ? 0x04u : 0u));
}

/* A reading in both resolutions. An 8-bit conversion gives the top eight
 * bits, followed at once by padding. */
static void expect_channel(int line, const char *what, uint32_t channel, bool single_ended, uint32_t want)
{
    uint8_t raw[2];
    uint32_t got12 = tsc_read(tsc_command(channel, false, single_ended), raw);
    g_checks++;
    if (got12 != want || (raw[0] & 0x80u) || (raw[1] & 0x07u)) {
        g_failures++;
        printf("FAIL %s:%d: %s: 12-bit reading %03X (bytes %02X %02X), expected %03X\n", __FILE__, line, what, got12,
               raw[0], raw[1], want);
    }
    uint32_t got8 = tsc_read(tsc_command(channel, true, single_ended), raw) >> 4;
    g_checks++;
    if (got8 != (want >> 4) || (raw[0] & 0x80u) || (raw[1] & 0x7Fu)) {
        g_failures++;
        printf("FAIL %s:%d: %s: 8-bit reading %02X (bytes %02X %02X), expected %02X\n", __FILE__, line, what, got8,
               raw[0], raw[1], want >> 4);
    }
}

static void test_touch(void)
{
    uint8_t raw[2];
    nds_spi_reset(&g_spi);

    /* Pen up: "Touchscreen X-Position (... or 000h=released)" and
     * "Y-Position (... or FFFh=released)". */
    expect_channel(__LINE__, "X, pen up", 5, false, 0x000);
    expect_channel(__LINE__, "Y, pen up", 1, false, 0xFFF);
    /* No contact, no current: Z1 reads ground and Z2 the reference. */
    expect_channel(__LINE__, "Z1, pen up", 3, false, 0x000);
    expect_channel(__LINE__, "Z2, pen up", 4, false, 0xFFF);
    CHECK(!nds_spi_pen_irq(&g_spi), "pen interrupt asserted with the pen up");

    /* Pen down. */
    nds_spi_set_touch(&g_spi, true, 0x123, 0xABC);
    expect_channel(__LINE__, "X, pen down", 5, false, 0x123);
    expect_channel(__LINE__, "Y, pen down", 1, false, 0xABC);
    CHECK(nds_spi_pen_irq(&g_spi), "pen interrupt not asserted with the pen down");
    /* The exact bytes: 0 d11-d5, then d4-d0 000. 123h = 0001 0010 0011. */
    tsc_read(0xD0, raw);
    CHECK(raw[0] == 0x09 && raw[1] == 0x18, "X = 123h comes as %02X %02X, expected 09 18", raw[0], raw[1]);
    /* 8-bit: 0 d7-d1, then d0 and zeros. ABh = 1010 1011. */
    tsc_read(0x98, raw);
    CHECK(raw[0] == 0x55 && raw[1] == 0x80, "Y = ABh (8-bit) comes as %02X %02X, expected 55 80", raw[0], raw[1]);

    /* Pressure: both of GBATEK's formulas must give the same contact
     * resistance, a positive one that is small against the plates'. With
     * Rx_plate = Ry_plate = 4096:
     *   R = X * (Z2 / Z1 - 1)
     *   R = X * (4096 / Z1 - 1) - (4096 - Y) */
    static const uint16_t points[][2] = { { 0x800, 0x800 }, { 0x123, 0xABC }, { 0xE00, 0x200 }, { 0x200, 0xE00 } };
    for (size_t i = 0; i < sizeof points / sizeof points[0]; i++) {
        double x = points[i][0], y = points[i][1];
        nds_spi_set_touch(&g_spi, true, points[i][0], points[i][1]);
        double z1 = tsc_read(tsc_command(3, false, true), NULL);
        double z2 = tsc_read(tsc_command(4, false, true), NULL);
        CHECK(z1 > 0 && z2 > z1 && z2 <= 4095, "pressure at (%03X,%03X): Z1 %g, Z2 %g", points[i][0], points[i][1], z1,
              z2);
        if (z1 > 0) {
            double r1 = x * (z2 / z1 - 1.0);
            double r2 = x * (4096.0 / z1 - 1.0) - (4096.0 - y);
            /* Z1 and Z2 are whole numbers: allow their rounding. */
            double slack = 3.0 * x / z1 + 4.0;
            CHECK(r1 > 100.0 && r1 < 4096.0, "contact resistance %g at (%03X,%03X)", r1, points[i][0], points[i][1]);
            CHECK(r1 - r2 < slack && r2 - r1 < slack, "the two pressure formulas give %g and %g at (%03X,%03X)", r1, r2,
                  points[i][0], points[i][1]);
        }
    }
    nds_spi_set_touch(&g_spi, true, 0x123, 0xABC);
    expect_channel(__LINE__, "Z1 in 8 bits", 3, true, tsc_read(tsc_command(3, false, true), NULL));
    expect_channel(__LINE__, "Z2 in 8 bits", 4, true, tsc_read(tsc_command(4, false, true), NULL));

    /* "Battery Voltage (not used, connected to GND in NDS, always 000h)" */
    expect_channel(__LINE__, "battery", 2, true, 0x000);

    /* Temperature: K = (TP1 - TP0) * 8568 / 4096 should be a room's. */
    uint32_t tp0 = tsc_read(tsc_command(0, false, true), NULL);
    uint32_t tp1 = tsc_read(tsc_command(7, false, true), NULL);
    double kelvin = ((double)tp1 - (double)tp0) * 8568.0 / 4096.0;
    CHECK(kelvin > 273.15 + 15.0 && kelvin < 273.15 + 35.0, "temperature %g K (TP0 %03X, TP1 %03X)", kelvin, tp0, tp1);
    expect_channel(__LINE__, "temperature 0 in both resolutions", 0, true, tp0);
    expect_channel(__LINE__, "temperature 1 in both resolutions", 7, true, tp1);

    /* The microphone on the AUX input: silence at mid-scale, then what the
     * machine supplies. libnds reads it single-ended, in 12 or 8 bits. */
    expect_channel(__LINE__, "microphone after reset", 6, true, 0x800);
    nds_spi_set_microphone(&g_spi, 0xABC);
    expect_channel(__LINE__, "microphone", 6, true, 0xABC);
    nds_spi_set_microphone(&g_spi, 0x005);
    expect_channel(__LINE__, "microphone, small sample", 6, true, 0x005);
    /* The pen does not disturb it. */
    nds_spi_set_touch(&g_spi, false, 0, 0);
    expect_channel(__LINE__, "microphone, pen up", 6, true, 0x005);
    expect_channel(__LINE__, "X after the pen lifts", 5, false, 0x000);
    expect_channel(__LINE__, "Y after the pen lifts", 1, false, 0xFFF);

    /* After the result: "endless padding (zero)". */
    nds_spi_set_touch(&g_spi, true, 0xFFF, 0xFFF);
    control(ENABLE | HOLD | TOUCH);
    exchange(0xD0);
    exchange(0);
    exchange(0);
    CHECK(exchange(0) == 0 && exchange(0) == 0, "bytes after the conversion result are not zero");
    control(0);

    /* A run of samples (libnds's __tscReadToBuffer): the control byte for
     * the next conversion goes out while the low bits of the last come in. */
    static const uint16_t run[4] = { 0x111, 0x7FF, 0x800, 0xEEE };
    control(ENABLE | HOLD | TOUCH);
    nds_spi_set_touch(&g_spi, true, run[0], 0x555);
    exchange(0xD0);
    bool same = true;
    for (int i = 0; i < 4; i++) {
        uint8_t high = exchange(0);
        if (i < 3) nds_spi_set_touch(&g_spi, true, run[i + 1], 0x555);
        uint8_t low = exchange(0xD0);
        same = same && ((((uint32_t)high << 5) | ((uint32_t)low >> 3)) & 0xFFFu) == run[i];
    }
    control(0);
    CHECK(same, "back-to-back samples do not match the positions set");

    /* A byte without the start bit starts nothing. */
    nds_spi_set_touch(&g_spi, true, 0xFFF, 0xFFF);
    control(ENABLE | HOLD | TOUCH);
    exchange(0x50);
    CHECK(exchange(0) == 0 && exchange(0) == 0, "a byte without the start bit started a conversion");
    control(0);

    /* Chip select released in mid-reply: the conversion is abandoned. */
    control(ENABLE | TOUCH);
    exchange(0xD0);
    control(ENABLE | HOLD | TOUCH);
    CHECK(exchange(0) == 0 && exchange(0) == 0, "a conversion survived chip select being released");
    control(0);

    /* Power-down mode bit 0 switches the pen interrupt output off. */
    nds_spi_set_touch(&g_spi, true, 0x400, 0x400);
    tsc_read(0xD0, NULL);
    CHECK(nds_spi_pen_irq(&g_spi), "pen interrupt off after a control byte with power-down mode 0");
    tsc_read(0xD1, NULL);
    CHECK(!nds_spi_pen_irq(&g_spi), "pen interrupt on after a control byte with power-down mode 1");
    tsc_read(0xD3, NULL);
    CHECK(!nds_spi_pen_irq(&g_spi), "pen interrupt on after a control byte with power-down mode 3");
    tsc_read(0xD2, NULL);
    CHECK(nds_spi_pen_irq(&g_spi), "pen interrupt off after a control byte with power-down mode 2");
    /* The conversion itself does not depend on the mode. */
    CHECK(tsc_read(0xD1, NULL) == 0x400 && tsc_read(0x93, NULL) == 0x400, "readings change with the power-down mode");
    tsc_read(0x84, NULL); /* libnds finishes with a temperature read in mode 0 */
    CHECK(nds_spi_pen_irq(&g_spi), "pen interrupt off after the closing read");
    nds_spi_set_touch(&g_spi, false, 0, 0);
    CHECK(!nds_spi_pen_irq(&g_spi), "pen interrupt asserted after the pen lifted");

    /* Readings are 12 bits whatever is set. */
    nds_spi_set_touch(&g_spi, true, 0xF123, 0xFABC);
    expect_channel(__LINE__, "X beyond 12 bits", 5, false, 0x123);
    expect_channel(__LINE__, "Y beyond 12 bits", 1, false, 0xABC);
    nds_spi_set_microphone(&g_spi, 0xF800);
    expect_channel(__LINE__, "microphone beyond 12 bits", 6, true, 0x800);
}

/* ------------------------------------------------------------------ */
/* Power management                                                    */
/* ------------------------------------------------------------------ */

/* libnds's writePowerManagement(): the index with hold, the data without. */
static uint8_t power_access(uint8_t index, uint8_t value)
{
    control(ENABLE | HOLD | POWER | 2);
    exchange(index);
    control(ENABLE | POWER | 2);
    uint8_t reply = exchange(value);
    control(0);
    return reply;
}

static uint8_t power_read(uint8_t reg) { return power_access((uint8_t)(reg | 0x80u), 0); }

static void test_power(void)
{
    nds_spi_reset(&g_spi);

    /* As the firmware leaves it: sound on and not muted, both backlights
     * on, the power LED steady, the console not shutting down; the battery
     * good; "the microphone amplifier is switched off after power up". */
    uint8_t reg0 = power_read(0);
    CHECK((reg0 & 0x01) && !(reg0 & 0x02), "register 0 = %02X: sound amplifier off or muted", reg0);
    CHECK((reg0 & 0x0C) == 0x0C, "register 0 = %02X: a backlight is off", reg0);
    CHECK(!(reg0 & 0x70), "register 0 = %02X: LED blinking or shut down", reg0);
    CHECK(power_read(1) == 0x00, "battery status %02X, expected 00 (good)", power_read(1));
    CHECK(power_read(2) == 0x00, "microphone amplifier %02X, expected 00 (off)", power_read(2));
    CHECK(power_read(3) <= 0x03, "microphone gain %02X", power_read(3));

    /* Register 0: bits 0-6 read and write, "Bit7 Not used (always 0)". */
    power_access(0, 0xFF);
    CHECK(power_read(0) == 0x7F, "register 0 after writing FF reads %02X, expected 7F", power_read(0));
    power_access(0, 0x00);
    CHECK(power_read(0) == 0x00, "register 0 after writing 00 reads %02X", power_read(0));
    power_access(0, 0x0D);
    CHECK(power_read(0) == 0x0D, "register 0 after writing 0D reads %02X", power_read(0));
    /* With the read bit set the data byte is not written. */
    power_access(0x80, 0x00);
    CHECK(power_read(0) == 0x0D, "a read access wrote register 0 (%02X)", power_read(0));

    /* Register 1: read-only. */
    power_access(1, 0xFF);
    CHECK(power_read(1) == 0x00, "battery status was written: %02X", power_read(1));
    nds_spi_set_battery_low(&g_spi, true);
    CHECK(power_read(1) == 0x01, "battery status %02X with the battery low, expected 01", power_read(1));
    nds_spi_set_battery_low(&g_spi, false);
    CHECK(power_read(1) == 0x00, "battery status %02X with the battery good", power_read(1));

    /* Register 2: bit 0 only. Register 3: bits 0-1 only. */
    power_access(2, 0xFF);
    CHECK(power_read(2) == 0x01, "register 2 after writing FF reads %02X, expected 01", power_read(2));
    power_access(2, 0x00);
    CHECK(power_read(2) == 0x00, "register 2 after writing 00 reads %02X", power_read(2));
    power_access(3, 0xFF);
    CHECK(power_read(3) == 0x03, "register 3 after writing FF reads %02X, expected 03", power_read(3));
    power_access(3, 0x02);
    CHECK(power_read(3) == 0x02, "register 3 after writing 02 reads %02X", power_read(3));

    /* "On Old-DS, registers 4..7Fh are mirrors of 0..3." */
    power_access(2, 0x01);
    CHECK(power_read(4) == 0x0D && power_read(5) == 0x00 && power_read(6) == 0x01 && power_read(7) == 0x02,
          "registers 4-7 read %02X %02X %02X %02X, expected the mirrors 0D 00 01 02", power_read(4), power_read(5),
          power_read(6), power_read(7));
    CHECK(power_read(0x7C) == 0x0D && power_read(0x7F) == 0x02, "registers 7Ch and 7Fh read %02X %02X", power_read(0x7C),
          power_read(0x7F));
    power_access(0x10, 0x05);
    CHECK(power_read(0) == 0x05, "a write to register 10h did not reach its mirror, register 0 (%02X)", power_read(0));

    /* What the machine sees. */
    power_access(0, 0x0D);
    CHECK(nds_spi_power_register(&g_spi, NDS_POWER_REG_CONTROL) == 0x0D &&
              nds_spi_power_register(&g_spi, NDS_POWER_REG_MIC_AMP) == 0x01 &&
              nds_spi_power_register(&g_spi, NDS_POWER_REG_MIC_GAIN) == 0x02,
          "the registers as the machine reads them: %02X %02X %02X", nds_spi_power_register(&g_spi, 0),
          nds_spi_power_register(&g_spi, 2), nds_spi_power_register(&g_spi, 3));
    power_access(0, 0x0D | 0x40);
    CHECK(nds_spi_power_register(&g_spi, NDS_POWER_REG_CONTROL) & NDS_POWER_SHUTDOWN,
          "the shut-down bit is not visible to the machine");

    /* The index is the first byte after chip select only: further bytes
     * are data for the same register. */
    control(ENABLE | HOLD | POWER);
    exchange(0x03);
    exchange(0x01);
    exchange(0x03);
    control(0);
    CHECK(power_read(3) == 0x03 && power_read(0) == 0x4D, "a second data byte went elsewhere: registers 3, 0 = %02X, %02X",
          power_read(3), power_read(0));
    control(ENABLE | HOLD | POWER);
    exchange(0x83);
    uint8_t first = exchange(0);
    uint8_t second = exchange(0);
    control(0);
    CHECK(first == 0x03 && second == 0x03, "two data bytes of a read: %02X %02X, expected 03 03", first, second);
}

int main(void)
{
    test_bus();
    test_flash();
    test_touch();
    test_power();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
