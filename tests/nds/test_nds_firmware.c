/*
 * The synthesised firmware image, checked against the documentation rather
 * than against the code that builds it:
 *
 *   - CRC16 against the published check values of the algorithm and
 *     against the eight table constants GBATEK prints (each is the CRC of a
 *     byte with one bit set);
 *   - the header, the Wi-Fi calibration block, the access point slots and
 *     both user settings copies at the offsets GBATEK gives, each block's
 *     CRC recomputed here with GBATEK's own formulation of the algorithm;
 *   - which user settings copy counts as current, by GBATEK's counter rule;
 *   - the block left in RAM, field by field;
 *   - the touch calibration, by converting the readings back to pixels the
 *     way software does.
 *
 * GBATEK sections: "BIOS Misc Functions", "DS Firmware Header", "DS Firmware
 * Wifi Calibration Data", "DS Firmware Wifi Internet Access Points", "DS
 * Firmware User Settings", "DS Wifi Configuration Ports", "DS Wifi RF9008
 * Registers", "DS Touch Screen Controller (TSC)".
 */
#include <stdio.h>
#include <string.h>

#include "nds_firmware.h"

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

/* GBATEK's GetCRC16 exactly as it is written there, as the reference. */
static uint16_t reference_crc16(const uint8_t *data, size_t length, uint16_t initial)
{
    static const uint32_t val[8] = { 0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001 };
    uint32_t crc = initial;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            uint32_t carry = crc & 1u;
            crc >>= 1;
            if (carry) crc ^= val[j] << (7 - j);
        }
    }
    return (uint16_t)crc;
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le24(const uint8_t *p) { return le16(p) | ((uint32_t)p[2] << 16); }

static uint8_t g_image[NDS_FIRMWARE_SIZE];

/* ------------------------------------------------------------------ */

static void test_crc16(void)
{
    static const uint8_t digits[] = "123456789";
    /* The catalogued check values of the reflected CRC-16 with polynomial
     * A001h: "CRC-16/ARC" (initial value 0) and "CRC-16/MODBUS" (FFFFh). */
    CHECK(nds_crc16(digits, 9, 0x0000) == 0xBB3D, "CRC16(\"123456789\", 0) = %04X, expected BB3D",
          nds_crc16(digits, 9, 0x0000));
    CHECK(nds_crc16(digits, 9, 0xFFFF) == 0x4B37, "CRC16(\"123456789\", FFFF) = %04X, expected 4B37",
          nds_crc16(digits, 9, 0xFFFF));

    /* Nothing to sum: the initial value comes back. */
    CHECK(nds_crc16(digits, 0, 0x1234) == 0x1234, "CRC16 of no bytes is %04X", nds_crc16(digits, 0, 0x1234));

    /* GBATEK's val[j] is the CRC (initial value 0) of the byte 1 << j. */
    static const uint16_t val[8] = { 0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001 };
    for (int j = 0; j < 8; j++) {
        uint8_t byte = (uint8_t)(1u << j);
        CHECK(nds_crc16(&byte, 1, 0) == val[j], "CRC16(%02X) = %04X, expected %04X", byte, nds_crc16(&byte, 1, 0),
              val[j]);
    }

    /* A run of bytes against the reference formulation, both initial values
     * and a length that is not a multiple of anything. */
    uint8_t buffer[301];
    uint32_t seed = 0x2545F491u;
    for (size_t i = 0; i < sizeof buffer; i++) {
        seed = seed * 1664525u + 1013904223u;
        buffer[i] = (uint8_t)(seed >> 24);
    }
    CHECK(nds_crc16(buffer, sizeof buffer, 0) == reference_crc16(buffer, sizeof buffer, 0),
          "CRC16 of 301 bytes differs from GBATEK's formulation (initial 0)");
    CHECK(nds_crc16(buffer, sizeof buffer, 0xFFFF) == reference_crc16(buffer, sizeof buffer, 0xFFFF),
          "CRC16 of 301 bytes differs from GBATEK's formulation (initial FFFF)");
}

/* ------------------------------------------------------------------ */

static void test_header(void)
{
    /* No boot code: the parts' addresses and CRCs are zero. */
    CHECK(le16(g_image + 0x00) == 0 && le16(g_image + 0x02) == 0 && le16(g_image + 0x0C) == 0 &&
              le16(g_image + 0x10) == 0 && le16(g_image + 0x16) == 0,
          "header claims boot code parts");
    CHECK(memcmp(g_image + 0x08, "MAC", 3) != 0, "identifier claims to be Nintendo's firmware");
    /* [14h] bits 12-15: chip size in units of 128 KiB. */
    CHECK((le16(g_image + 0x14) >> 12) * 0x20000u == NDS_FIRMWARE_SIZE, "header chip size field %04X",
          le16(g_image + 0x14));
    CHECK(g_image[0x1D] == 0xFF, "console type %02X, expected FF (Nintendo DS)", g_image[0x1D]);
    CHECK(le16(g_image + 0x20) * 8u == 0x3FE00u, "user settings offset %05X, expected 3FE00",
          le16(g_image + 0x20) * 8u);
    /* "User Settings Offset (usually last 200h flash bytes)" */
    CHECK(le16(g_image + 0x20) * 8u == NDS_FIRMWARE_SIZE - 0x200u, "user settings are not the last 200h bytes");
}

static void test_wifi(void)
{
    uint32_t length = le16(g_image + 0x2C);

    CHECK(length == 0x138, "config length %04X, expected 0138", length);
    CHECK(le16(g_image + 0x2A) == reference_crc16(g_image + 0x2C, length, 0), "Wi-Fi block CRC %04X, computed %04X",
          le16(g_image + 0x2A), reference_crc16(g_image + 0x2C, length, 0));
    CHECK(g_image[0x2E] == 0x00, "byte 2Eh is %02X, expected 00", g_image[0x2E]);
    CHECK(g_image[0x2F] == 0x00, "version %02X, expected 00 (firmware v1-v4)", g_image[0x2F]);
    /* v1-v5: 00 09 BF xx xx xx */
    CHECK(g_image[0x36] == 0x00 && g_image[0x37] == 0x09 && g_image[0x38] == 0xBF,
          "MAC address %02X:%02X:%02X, expected the 00:09:BF prefix", g_image[0x36], g_image[0x37], g_image[0x38]);
    CHECK(le16(g_image + 0x3C) == 0x3FFE, "enabled channels %04X, expected 3FFE", le16(g_image + 0x3C));
    CHECK((le16(g_image + 0x3C) & ~0x7FFEu) == 0, "channel bits outside 1-14 are set");
    CHECK(le16(g_image + 0x3E) == 0xFFFF, "flags %04X, expected FFFF", le16(g_image + 0x3E));
    CHECK(g_image[0x40] == 0x02, "RF type %02X, expected 02", g_image[0x40]);
    CHECK(g_image[0x41] == 0x18, "RF bits per entry %02X, expected 18", g_image[0x41]);
    CHECK(g_image[0x42] == 0x0C, "RF entries %02X, expected 0C", g_image[0x42]);
    CHECK(g_image[0x43] == 0x01, "byte 43h is %02X, expected 01", g_image[0x43]);

    /* "DS Wifi Configuration Ports": the value of each port after it is
     * initialised from the firmware, by the firmware offset it comes from. */
    static const struct {
        uint32_t offset;
        uint16_t value;
        const char *port;
    } config[] = {
        { 0x44, 0x0002, "W_CONFIG_146h" }, { 0x46, 0x0017, "W_CONFIG_148h" }, { 0x48, 0x0026, "W_CONFIG_14Ah" },
        { 0x4A, 0x1818, "W_CONFIG_14Ch" }, { 0x4C, 0x0048, "W_CONFIG_120h" }, { 0x4E, 0x4840, "W_CONFIG_122h" },
        { 0x50, 0x0058, "W_CONFIG_154h" }, { 0x52, 0x0042, "W_CONFIG_144h" }, { 0x54, 0x0140, "W_CONFIG_130h" },
        { 0x56, 0x8064, "W_CONFIG_132h" }, { 0x58, 0xE0E0, "W_CONFIG_140h" }, { 0x5A, 0x2443, "W_CONFIG_142h" },
        { 0x5E, 0x0032, "W_CONFIG_124h" }, { 0x60, 0x01F4, "W_CONFIG_128h" }, { 0x62, 0x0101, "W_CONFIG_150h" },
    };
    for (size_t i = 0; i < sizeof config / sizeof config[0]; i++) {
        CHECK(le16(g_image + config[i].offset) == config[i].value, "%s initial value %04X, expected %04X",
              config[i].port, le16(g_image + config[i].offset), config[i].value);
    }
    /* W_POWER_TX has three writable bits. */
    CHECK((le16(g_image + 0x5C) & ~7u) == 0, "W_POWER_TX initial value %04X has bits it cannot hold",
          le16(g_image + 0x5C));

    /* "DS Wifi Baseband Chip (BB)": the ID register, the bit the wake-up
     * sequence toggles, and the registers with documented initial values. */
    CHECK(g_image[0x64 + 0x00] == 0x6D, "BB[00h] = %02X, expected 6D", g_image[0x64]);
    CHECK((g_image[0x64 + 0x01] & 0x80) != 0, "BB[01h] = %02X: bit 7 must be set", g_image[0x65]);
    CHECK(g_image[0x64 + 0x13] == 0x00, "BB[13h] = %02X, expected 00", g_image[0x64 + 0x13]);
    CHECK(g_image[0x64 + 0x1E] == 0xBB, "BB[1Eh] = %02X, expected BB", g_image[0x64 + 0x1E]);
    CHECK(g_image[0x64 + 0x35] == 0x1F, "BB[35h] = %02X, expected 1F", g_image[0x64 + 0x35]);
    CHECK(g_image[0x64 + 0x5D] == 0x01, "BB[5Dh] = %02X, expected 01", g_image[0x64 + 0x5D]);
    CHECK(g_image[0xCD] == 0x00, "byte CDh is %02X, expected 00", g_image[0xCD]);

    /* "RF chip data (Type2) (initial NDS settings from firmware, example)",
     * stored as 24-bit values in the order RF[0,4,5,6,7,8,9,A,B,1,2,3]. */
    static const uint32_t rf[12] = { 0x00C007, 0x129C03, 0x141728, 0x1AE8BA, 0x1D456F, 0x23FFFA,
                                     0x241D30, 0x280001, 0x2C0000, 0x069C03, 0x080022, 0x0DFF6F };
    static const uint32_t rf_index[12] = { 0, 4, 5, 6, 7, 8, 9, 10, 11, 1, 2, 3 };
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t value = le24(g_image + 0xCE + 3 * i);
        CHECK(value == rf[i], "RF initial value %u is %06X, expected %06X", i, value, rf[i]);
        CHECK((value >> 18) == rf_index[i], "RF initial value %u addresses register %u, expected %u", i, value >> 18,
              rf_index[i]);
    }

    /*
     * The per-channel RF[5] and RF[6]. Channel 1 is GBATEK's example (the
     * values "also written when changing channels"). The others follow from
     * the synthesiser arithmetic: 22 MHz * (N + F / 2^24) = channel - 374.
     *   channel 7, 2442 MHz: 2068 = 22 * 94 exactly, so N = 94, F = 0:
     *     RF[5] = 5 << 18 | 94 << 6 = 141780h, RF[6] = 6 << 18 = 180000h
     *   channel 14, 2484 MHz: 2110 = 22 * 95 + 20, F = 20/22 * 2^24 =
     *     15252014.5, to the nearest 15252015 = E8BA2Fh:
     *     RF[5] = 140000h | 95 << 6 | E8BA2Fh >> 18 = 1417FAh
     *     RF[6] = 180000h | (E8BA2Fh & 3FFFFh) = 18BA2Fh
     */
    CHECK(le24(g_image + 0xF2) == 0x141728 && le24(g_image + 0xF5) == 0x1AE8BA, "channel 1 RF values %06X %06X",
          le24(g_image + 0xF2), le24(g_image + 0xF5));
    CHECK(le24(g_image + 0xF2 + 6 * 6) == 0x141780 && le24(g_image + 0xF5 + 6 * 6) == 0x180000,
          "channel 7 RF values %06X %06X", le24(g_image + 0xF2 + 36), le24(g_image + 0xF5 + 36));
    CHECK(le24(g_image + 0xF2 + 6 * 13) == 0x1417FA && le24(g_image + 0xF5 + 6 * 13) == 0x18BA2F,
          "channel 14 RF values %06X %06X", le24(g_image + 0xF2 + 78), le24(g_image + 0xF5 + 78));
    /* Every channel's frequency, from the stored values back to MHz. */
    for (uint32_t channel = 1; channel <= 14; channel++) {
        uint32_t rf5 = le24(g_image + 0xF2 + 6 * (channel - 1));
        uint32_t rf6 = le24(g_image + 0xF5 + 6 * (channel - 1));
        uint32_t whole = (rf5 >> 6) & 0xFFFu;
        uint32_t fraction = ((rf5 & 0x3Fu) << 18) | (rf6 & 0x3FFFFu);
        /* kHz, rounded: 22000 * (whole + fraction / 2^24) + 374000 */
        uint32_t khz = 22000u * whole + (uint32_t)(((uint64_t)22000u * fraction + (1u << 23)) >> 24) + 374000u;
        uint32_t want = channel == 14 ? 2484000u : 2407000u + 5000u * channel;
        CHECK((rf5 >> 18) == 5 && (rf6 >> 18) == 6 && khz == want, "channel %u tunes to %u kHz, expected %u", channel,
              khz, want);
    }
    for (uint32_t i = 0; i < 14; i++) {
        /* "usually somewhat B1h..B7h" and "usually 10h-filled" */
        CHECK(g_image[0x146 + i] >= 0xB1 && g_image[0x146 + i] <= 0xB7, "channel %u BB[1Eh] value %02X", i + 1,
              g_image[0x146 + i]);
        CHECK(g_image[0x154 + i] == 0x10, "channel %u RF[9] bits %02X, expected 10", i + 1, g_image[0x154 + i]);
    }
    CHECK(g_image[0x162] >= 0x19 && g_image[0x162] <= 0x1C, "byte 162h is %02X, expected 19-1C", g_image[0x162]);
    CHECK(g_image[0x163] == 0xFF, "byte 163h is %02X, expected FF", g_image[0x163]);

    /* 164h-1FFh: unused, FFh-filled, outside the CRC. */
    uint8_t filled[0x9C];
    memset(filled, 0xFF, sizeof filled);
    CHECK(memcmp(g_image + 0x164, filled, sizeof filled) == 0, "164h-1FFh is not FFh-filled");

    /* Changing the MAC address keeps the block valid. */
    static uint8_t copy[NDS_FIRMWARE_SIZE];
    static const uint8_t mac[6] = { 0x00, 0x09, 0xBF, 0x12, 0x34, 0x56 };
    memcpy(copy, g_image, sizeof copy);
    nds_firmware_set_mac(copy, mac);
    CHECK(memcmp(copy + 0x36, mac, 6) == 0, "the MAC address was not replaced");
    CHECK(le16(copy + 0x2A) == reference_crc16(copy + 0x2C, 0x138, 0), "Wi-Fi block CRC is wrong after a MAC change");
    CHECK(le16(copy + 0x2A) != le16(g_image + 0x2A), "Wi-Fi block CRC did not change with the MAC");
    memcpy(copy + 0x36, g_image + 0x36, 6);
    memcpy(copy + 0x2A, g_image + 0x2A, 2);
    CHECK(memcmp(copy, g_image, sizeof copy) == 0, "a MAC change touched other bytes");
}

static void test_layout(void)
{
    /* No code or data: 200h up to the first access point reads as erased
     * flash, and so does the unused slot at 3FD00h. */
    bool erased = true;
    for (uint32_t i = 0x200; i < 0x3FA00; i++) erased = erased && g_image[i] == 0xFF;
    CHECK(erased, "200h-3F9FFh is not FFh-filled");
    erased = true;
    for (uint32_t i = 0x3FD00; i < 0x3FE00; i++) erased = erased && g_image[i] == 0xFF;
    CHECK(erased, "3FD00h-3FDFFh is not FFh-filled");

    /* Access points 1-3 at settings - 400h, - 300h, - 200h: status FFh
     * (not configured), CRC16 with initial value 0 over 000h-0FDh. */
    for (uint32_t slot = 0; slot < 3; slot++) {
        const uint8_t *ap = g_image + le16(g_image + 0x20) * 8u - 0x400u + 0x100u * slot;
        CHECK(ap[0xE7] == 0xFF, "access point %u status %02X, expected FF", slot + 1, ap[0xE7]);
        CHECK(le16(ap + 0xFE) == reference_crc16(ap, 0xFE, 0), "access point %u CRC %04X, computed %04X", slot + 1,
              le16(ap + 0xFE), reference_crc16(ap, 0xFE, 0));
        CHECK(ap[0x40] == 0, "access point %u has an SSID", slot + 1);
    }
}

/* Which copy is current, by GBATEK's rule, worked out here. */
static int reference_current(const uint8_t *image)
{
    const uint8_t *copy0 = image + le16(image + 0x20) * 8u;
    const uint8_t *copy1 = copy0 + 0x100;
    bool valid0 = reference_crc16(copy0, 0x70, 0xFFFF) == le16(copy0 + 0x72);
    bool valid1 = reference_crc16(copy1, 0x70, 0xFFFF) == le16(copy1 + 0x72);
    if (!valid0 && !valid1) return -1;
    if (!valid0) return 1;
    if (!valid1) return 0;
    return le16(copy1 + 0x70) == ((le16(copy0 + 0x70) + 1u) & 0x7Fu) ? 1 : 0;
}

static void expect_current(int line, const uint8_t *image, int want)
{
    uint32_t offset = 0;
    bool found = nds_firmware_current_settings(image, &offset);
    int got = !found ? -1 : offset == 0x3FE00u ? 0 : offset == 0x3FF00u ? 1 : 2;
    g_checks++;
    if (got != want || reference_current(image) != want) {
        g_failures++;
        printf("FAIL %s:%d: current settings copy is %d (GBATEK's rule gives %d), expected %d\n", __FILE__, line, got,
               reference_current(image), want);
    }
}

static void set_counter(uint8_t *copy, uint16_t counter)
{
    copy[0x70] = (uint8_t)counter;
    copy[0x71] = (uint8_t)(counter >> 8);
}

static void test_settings_copies(void)
{
    static uint8_t image[NDS_FIRMWARE_SIZE];
    memcpy(image, g_image, sizeof image);
    uint8_t *copy0 = image + 0x3FE00;
    uint8_t *copy1 = image + 0x3FF00;

    for (int i = 0; i < 2; i++) {
        const uint8_t *copy = i ? copy1 : copy0;
        CHECK(le16(copy + 0x72) == reference_crc16(copy, 0x70, 0xFFFF), "user settings %d CRC %04X, computed %04X", i,
              le16(copy + 0x72), reference_crc16(copy, 0x70, 0xFFFF));
        CHECK(le16(copy + 0x70) <= 0x7F, "user settings %d update counter %04X is out of range", i,
              le16(copy + 0x70));
        CHECK(le16(copy) == 5, "user settings %d version %u, expected 5", i, le16(copy));
        bool filled = true;
        for (uint32_t k = 0x74; k < 0x100; k++) filled = filled && copy[k] == 0xFF;
        CHECK(filled, "user settings %d: 74h-FFh is not FFh-filled", i);
    }
    CHECK(memcmp(copy0, copy1, 0x70) == 0, "the two user settings copies differ");
    CHECK(le16(copy1 + 0x70) == ((le16(copy0 + 0x70) + 1u) & 0x7Fu), "counters %u and %u: the second copy is not newer",
          le16(copy0 + 0x70), le16(copy1 + 0x70));
    expect_current(__LINE__, image, 1);

    /* A game saves new settings over the older copy, with the next count. */
    copy0[0x02] = 7;
    set_counter(copy0, 2);
    uint16_t crc = reference_crc16(copy0, 0x70, 0xFFFF);
    copy0[0x72] = (uint8_t)crc;
    copy0[0x73] = (uint8_t)(crc >> 8);
    expect_current(__LINE__, image, 0);

    /* The counter is seven bits: 7Fh is followed by 00h. (The CRC does not
     * cover the counter.) */
    set_counter(copy0, 0x7F);
    set_counter(copy1, 0x00);
    expect_current(__LINE__, image, 1);
    set_counter(copy0, 0x00);
    set_counter(copy1, 0x7F);
    expect_current(__LINE__, image, 0);

    /* A copy with a bad CRC does not count, whatever its counter says. */
    set_counter(copy0, 5);
    set_counter(copy1, 6);
    expect_current(__LINE__, image, 1);
    copy1[0x10] ^= 0x01;
    expect_current(__LINE__, image, 0);
    copy0[0x10] ^= 0x01;
    expect_current(__LINE__, image, -1);
    copy1[0x10] ^= 0x01;
    expect_current(__LINE__, image, 1);

    /* A header pointing outside the image finds nothing. */
    memcpy(image, g_image, sizeof image);
    image[0x20] = 0xFF;
    image[0x21] = 0xFF;
    uint32_t offset = 0;
    CHECK(!nds_firmware_current_settings(image, &offset), "settings found through a header offset outside the image");
}

static void test_ram_block(void)
{
    NdsUserSettings s;
    uint8_t block[NDS_USER_SETTINGS_SIZE];
    static const uint16_t nickname[] = { 'Z', 'e', 'l', 'd', 'a', 0x00E9, 0x30BC, 'x', 'y', 'z' }; /* 10, the most */
    static const uint16_t message[] = { 'H', 'i', '!', 0x2665 };

    memset(&s, 0, sizeof s);
    memcpy(s.nickname, nickname, sizeof nickname);
    s.nickname_length = 10;
    memcpy(s.message, message, sizeof message);
    s.message_length = 4;
    s.favourite_colour = 11;
    s.birthday_month = 12;
    s.birthday_day = 31;
    s.language = NDS_LANGUAGE_GERMAN;
    s.alarm_hour = 23;
    s.alarm_minute = 59;
    s.alarm_enabled = 1;
    s.touch_adc_x1 = 0x02DF;
    s.touch_adc_y1 = 0x032C;
    s.touch_scr_x1 = 32;
    s.touch_scr_y1 = 33;
    s.touch_adc_x2 = 0x0D3B;
    s.touch_adc_y2 = 0x0CE7;
    s.touch_scr_x2 = 224;
    s.touch_scr_y2 = 160;
    s.flags = NDS_SETTINGS_FLAGS_OKAY | NDS_SETTINGS_FLAG_AUTOSTART;
    memset(block, 0xAA, sizeof block);
    nds_user_settings_ram_block(&s, block);

    CHECK(le16(block + 0x00) == 5, "version %u at 00h, expected 5", le16(block));
    CHECK(block[0x02] == 11, "favourite colour %u at 02h, expected 11", block[0x02]);
    CHECK(block[0x03] == 12 && block[0x04] == 31, "birthday %u/%u at 03h/04h, expected 12/31", block[0x03], block[0x04]);
    CHECK(block[0x05] == 0, "byte 05h is %02X, expected 00", block[0x05]);
    bool same = true;
    for (int i = 0; i < 10; i++) same = same && le16(block + 0x06 + 2 * i) == nickname[i];
    CHECK(same, "nickname at 06h is not the UTF-16 string given");
    CHECK(le16(block + 0x1A) == 10, "nickname length %u at 1Ah, expected 10", le16(block + 0x1A));
    same = true;
    for (int i = 0; i < 26; i++) same = same && le16(block + 0x1C + 2 * i) == (i < 4 ? message[i] : 0u);
    CHECK(same, "message at 1Ch is not the UTF-16 string given, zero-padded");
    CHECK(le16(block + 0x50) == 4, "message length %u at 50h, expected 4", le16(block + 0x50));
    CHECK(block[0x52] == 23 && block[0x53] == 59, "alarm %u:%u at 52h/53h, expected 23:59", block[0x52], block[0x53]);
    CHECK((block[0x56] & 0x80) != 0, "alarm enable byte %02X at 56h: bit 7 is clear", block[0x56]);
    CHECK(block[0x57] == 0, "byte 57h is %02X, expected 00", block[0x57]);
    CHECK(le16(block + 0x58) == 0x02DF && le16(block + 0x5A) == 0x032C, "calibration ADC point 1 (%03X,%03X) at 58h",
          le16(block + 0x58), le16(block + 0x5A));
    CHECK(block[0x5C] == 32 && block[0x5D] == 33, "calibration pixel 1 (%u,%u) at 5Ch", block[0x5C], block[0x5D]);
    CHECK(le16(block + 0x5E) == 0x0D3B && le16(block + 0x60) == 0x0CE7, "calibration ADC point 2 (%03X,%03X) at 5Eh",
          le16(block + 0x5E), le16(block + 0x60));
    CHECK(block[0x62] == 224 && block[0x63] == 160, "calibration pixel 2 (%u,%u) at 62h", block[0x62], block[0x63]);
    /* Language in bits 0-2; autostart is bit 6; "settings okay" bits 10,
     * 11, 13, 14, 15 set and "settings lost" bit 9 clear. */
    CHECK((le16(block + 0x64) & 7u) == 3, "language %u at 64h, expected 3 (German)", le16(block + 0x64) & 7u);
    CHECK((le16(block + 0x64) & 0x0040u) != 0, "autostart flag is not bit 6 of 64h (%04X)", le16(block + 0x64));
    CHECK((le16(block + 0x64) & 0xEE00u) == 0xEC00u, "flags at 64h (%04X) would make the firmware ask again",
          le16(block + 0x64));
    CHECK(le16(block + 0x68) == 0 && le16(block + 0x6A) == 0, "RTC offset at 68h is not zero");

    /* And back. */
    NdsUserSettings parsed;
    uint8_t again[NDS_USER_SETTINGS_SIZE];
    nds_user_settings_from_block(&parsed, block);
    nds_user_settings_ram_block(&parsed, again);
    CHECK(memcmp(block, again, sizeof block) == 0, "settings read back from a block write a different block");
    CHECK(parsed.language == NDS_LANGUAGE_GERMAN && parsed.nickname_length == 10 && parsed.touch_adc_x2 == 0x0D3B &&
              parsed.touch_scr_y1 == 33 && parsed.alarm_enabled == 1 && parsed.nickname[6] == 0x30BC,
          "settings read back from a block are not those written");

    /* Lengths beyond the fields are cut to them. */
    s.nickname_length = 200;
    s.message_length = 200;
    nds_user_settings_ram_block(&s, block);
    CHECK(le16(block + 0x1A) == 10 && le16(block + 0x50) == 26, "over-long strings: lengths %u and %u",
          le16(block + 0x1A), le16(block + 0x50));
    CHECK(block[0x52] == 23, "an over-long message ran into the alarm at 52h");

    /* The defaults: English, a nickname, settings the firmware would accept. */
    nds_user_settings_default(&s);
    nds_user_settings_ram_block(&s, block);
    CHECK((le16(block + 0x64) & 7u) == 1, "default language %u, expected 1 (English)", le16(block + 0x64) & 7u);
    CHECK(le16(block + 0x1A) >= 1 && le16(block + 0x1A) <= 10 && le16(block + 0x06) != 0, "default nickname is empty");
    CHECK((le16(block + 0x64) & 0xEE00u) == 0xEC00u, "default flags %04X", le16(block + 0x64));
    CHECK(block[0x03] >= 1 && block[0x03] <= 12 && block[0x04] >= 1 && block[0x04] <= 31, "default birthday %u/%u",
          block[0x03], block[0x04]);
    CHECK(block[0x02] <= 15, "default colour %u", block[0x02]);

    /* What the built image holds is this block. */
    CHECK(memcmp(g_image + 0x3FF00, block, sizeof block) == 0, "the image's settings are not the RAM block");
}

/* ------------------------------------------------------------------ */

/*
 * Reading to pixel, the way software does it. libnds (touch.c): a scale of
 * pixels per ADC unit with 19 fraction bits, an offset that puts the
 * midpoint of the two readings on the midpoint of the two pixels, and half
 * a unit of rounding.
 *
 * The arithmetic of this one function is ported from libnds, the BlocksDS
 * fork (https://github.com/blocksds/libnds, commit
 * 4674cdfed005eb9a3699f4896e5c587ebc8b6a09), source/arm7/touch.c:
 * touchInit() and touchApplyCalibration(). This is an altered version: one
 * axis, the calibration passed in instead of read from the user settings,
 * no clamping to the screen. The original notice follows.
 *
 * Copyright (C) 2005 Michael Noland (joat)
 * Copyright (C) 2005-2008 Jason Rogers (dovoto)
 * Copyright (C) 2005-2017 Dave Murphy (WinterMute)
 * Copyright (C) 2024 Adrian "asie" Siekierka
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must not
 *      claim that you wrote the original software. If you use this software
 *      in a product, an acknowledgment in the product documentation would be
 *      appreciated but is not required.
 *   2. Altered source versions must be plainly marked as such, and must not
 *      be misrepresented as being the original software.
 *   3. This notice may not be removed or altered from any source
 *      distribution.
 */
static int libnds_pixel(int adc, int adc1, int adc2, int scr1, int scr2)
{
    int scale = ((scr2 - scr1) << 19) / (adc2 - adc1);
    int offset = ((adc1 + adc2) * scale - ((scr1 + scr2) << 19)) / 2;
    return (adc * scale - offset + scale / 2) >> 19;
}

/* The same line in exact arithmetic, rounding down. */
static int exact_pixel(int adc, int adc1, int adc2, int scr1, int scr2)
{
    long long num = (long long)(adc - adc1) * (scr2 - scr1);
    long long den = adc2 - adc1;
    long long q = num / den;
    if (num % den != 0 && ((num < 0) != (den < 0))) q--;
    return (int)q + scr1;
}

static void test_touch(void)
{
    NdsUserSettings s;
    uint16_t ax, ay;
    nds_user_settings_default(&s);

    /* The default calibration is 16 units to the pixel through (32,32) =
     * (200h,200h): pixel p covers readings 16p to 16p + 15, middle 16p + 8. */
    CHECK(s.touch_scr_x1 == 32 && s.touch_scr_y1 == 32 && s.touch_scr_x2 == 224 && s.touch_scr_y2 == 160,
          "default calibration pixels (%u,%u) (%u,%u)", s.touch_scr_x1, s.touch_scr_y1, s.touch_scr_x2, s.touch_scr_y2);
    CHECK(s.touch_adc_x1 == 0x200 && s.touch_adc_y1 == 0x200 && s.touch_adc_x2 == 0xE00 && s.touch_adc_y2 == 0xA00,
          "default calibration readings (%03X,%03X) (%03X,%03X)", s.touch_adc_x1, s.touch_adc_y1, s.touch_adc_x2,
          s.touch_adc_y2);
    static const struct {
        int x, y;
        uint16_t ax, ay;
    } points[] = {
        { 0, 0, 8, 8 },           { 255, 0, 4088, 8 },      { 0, 191, 8, 3064 },
        { 255, 191, 4088, 3064 }, { 128, 96, 2056, 1544 }, { 32, 32, 520, 520 },
    };
    for (size_t i = 0; i < sizeof points / sizeof points[0]; i++) {
        nds_touch_adc_from_screen(&s, points[i].x, points[i].y, &ax, &ay);
        CHECK(ax == points[i].ax && ay == points[i].ay, "pixel (%d,%d) reads (%u,%u), expected (%u,%u)", points[i].x,
              points[i].y, ax, ay, points[i].ax, points[i].ay);
    }
    /* Outside the screen: the nearest edge. */
    nds_touch_adc_from_screen(&s, -5, 1000, &ax, &ay);
    CHECK(ax == 8 && ay == 3064, "pixel (-5,1000) reads (%u,%u), expected the corner (8,3064)", ax, ay);

    /* Every pixel comes back, through both conversions, for the default
     * calibration and for one with real-looking, uneven numbers (about
     * 13.8 and 19.6 units to the pixel) and for one with an axis reversed. */
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 1) {
            s.touch_adc_x1 = 0x02DF;
            s.touch_adc_y1 = 0x032C;
            s.touch_adc_x2 = 0x0D3B;
            s.touch_adc_y2 = 0x0CE7;
        } else if (pass == 2) {
            s.touch_adc_x1 = 0x0D3B;
            s.touch_adc_x2 = 0x02DF;
        }
        int wrong = 0, out_of_range = 0;
        for (int y = 0; y < 192; y++) {
            for (int x = 0; x < 256; x++) {
                nds_touch_adc_from_screen(&s, x, y, &ax, &ay);
                if (ax < 1 || ax > 0xFFE || ay < 1 || ay > 0xFFE) out_of_range++;
                if (exact_pixel(ax, s.touch_adc_x1, s.touch_adc_x2, 32, 224) != x) wrong++;
                if (exact_pixel(ay, s.touch_adc_y1, s.touch_adc_y2, 32, 160) != y) wrong++;
                if (libnds_pixel(ax, s.touch_adc_x1, s.touch_adc_x2, 32, 224) != x) wrong++;
                if (libnds_pixel(ay, s.touch_adc_y1, s.touch_adc_y2, 32, 160) != y) wrong++;
            }
        }
        CHECK(wrong == 0, "calibration %d: %d readings convert back to the wrong pixel", pass, wrong);
        CHECK(out_of_range == 0, "calibration %d: %d readings outside 001h-FFEh", pass, out_of_range);
    }

    /* A calibration that would put a pixel beyond the controller's range
     * stays inside it, clear of the pen-up readings 000h and FFFh. */
    nds_user_settings_default(&s);
    s.touch_adc_x1 = 0x010;
    s.touch_adc_x2 = 0xFF0;
    s.touch_adc_y1 = 0xFF0;
    s.touch_adc_y2 = 0x010;
    nds_touch_adc_from_screen(&s, 0, 0, &ax, &ay);
    CHECK(ax == 0x001 && ay == 0xFFE, "out-of-range readings (%03X,%03X), expected (001,FFE)", ax, ay);

    /* Two calibration points on one pixel column or row cannot divide. */
    s.touch_scr_x2 = s.touch_scr_x1;
    s.touch_scr_y2 = s.touch_scr_y1;
    nds_touch_adc_from_screen(&s, 100, 100, &ax, &ay);
    CHECK(ax == 0x010 && ay == 0xFF0, "degenerate calibration reads (%03X,%03X)", ax, ay);
}

int main(void)
{
    NdsUserSettings s;
    nds_user_settings_default(&s);
    memset(g_image, 0x55, sizeof g_image);
    nds_firmware_build(g_image, &s);

    test_crc16();
    test_header();
    test_wifi();
    test_layout();
    test_settings_copies();
    test_ram_block();
    test_touch();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
