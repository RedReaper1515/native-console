/*
 * The synthesised firmware flash image (see nds_firmware.h).
 *
 * Every byte here is produced from the documentation: GBATEK's "DS Firmware
 * Header", "DS Firmware Wifi Calibration Data", "DS Firmware Wifi Internet
 * Access Points" and "DS Firmware User Settings", and, for the radio's
 * values, its "DS Wifi Configuration Ports", "DS Wifi Baseband Chip (BB)"
 * and "DS Wifi RF9008 Registers". Nothing is copied from a firmware dump.
 *
 * The console described is the original DS: console type FFh, the RF9008
 * radio ("type 2"), a 256 KiB flash chip.
 *
 * What a game does with the image:
 *   - reads header[20h] to find the user settings, checks both copies'
 *     CRC16 and takes the newer (most games use the copy the firmware left
 *     in RAM instead, which nds_user_settings_ram_block() provides);
 *   - reads the Wi-Fi block, checks its CRC16, and writes the values into
 *     the radio's registers. Games with wireless play do this at start-up
 *     and stop if the CRC is wrong;
 *   - reads the three access point slots and checks their CRC16s before
 *     offering Nintendo Wi-Fi Connection;
 *   - a few rewrite the older user settings copy.
 */
#include <string.h>

#include "nds_firmware.h"

/* ------------------------------------------------------------------ */
/* CRC16                                                               */
/* ------------------------------------------------------------------ */

/*
 * GBATEK, "BIOS Misc Functions", GetCRC16: each byte is XORed into the
 * value, which is then shifted right eight times, and each 1 shifted out
 * XORs in A001h. (GBATEK writes the eight steps with a table, C0C1h C181h
 * ... A001h shifted left by 7 down to 0, which is the same thing with the
 * XORs applied ahead of the shifts.) This is the common reflected CRC-16;
 * only the initial value varies between the firmware's blocks.
 */
uint16_t nds_crc16(const uint8_t *data, size_t length, uint16_t initial)
{
    uint32_t crc = initial;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc & 1u) ? (crc >> 1) ^ 0xA001u : crc >> 1;
    }
    return (uint16_t)crc;
}

/* ------------------------------------------------------------------ */
/* User settings                                                       */
/* ------------------------------------------------------------------ */

/* Offsets in a user settings copy ("DS Firmware User Settings"). */
enum {
    US_VERSION = 0x00,        /* 2: always 5 */
    US_COLOUR = 0x02,         /* 1 */
    US_BIRTH_MONTH = 0x03,    /* 1 */
    US_BIRTH_DAY = 0x04,      /* 1 */
    US_NICKNAME = 0x06,       /* 20: UTF-16 */
    US_NICKNAME_LENGTH = 0x1A, /* 2 */
    US_MESSAGE = 0x1C,        /* 52: UTF-16 */
    US_MESSAGE_LENGTH = 0x50, /* 2 */
    US_ALARM_HOUR = 0x52,     /* 1 */
    US_ALARM_MINUTE = 0x53,   /* 1 */
    US_ALARM_ENABLE = 0x56,   /* 1: 80h = alarm on */
    US_TOUCH_ADC1 = 0x58,     /* 2 + 2: x, y */
    US_TOUCH_SCR1 = 0x5C,     /* 1 + 1 */
    US_TOUCH_ADC2 = 0x5E,     /* 2 + 2 */
    US_TOUCH_SCR2 = 0x62,     /* 1 + 1 */
    US_LANGUAGE_FLAGS = 0x64, /* 2 */
    US_COUNTER = 0x70,        /* 2: flash only */
    US_CRC = 0x72,            /* 2: flash only; CRC16 of 00h-6Fh, initial value FFFFh */
};

/*
 * The default calibration. Software converts a reading to a pixel with
 * fixed-point arithmetic: it keeps the scale, pixels per ADC unit, to some
 * number of fraction bits, and a scale that does not fit them puts the
 * result off by a pixel towards the edges of the screen. One pixel to
 * exactly 16 units is a power of two and fits whatever the precision, so
 * every pixel is reachable: reading = 16 * pixel, pinned at the two points
 * the firmware asks the player to tap, (32,32) and (224,160).
 */
#define DEFAULT_UNITS_PER_PIXEL 16u

void nds_user_settings_default(NdsUserSettings *s)
{
    static const char name[] = "Player";

    memset(s, 0, sizeof *s);
    s->nickname_length = (uint8_t)(sizeof name - 1);
    for (size_t i = 0; i < sizeof name - 1; i++) s->nickname[i] = (uint8_t)name[i];
    s->favourite_colour = 0;
    s->birthday_month = 1;
    s->birthday_day = 1;
    s->language = NDS_LANGUAGE_ENGLISH;
    s->touch_scr_x1 = 32;
    s->touch_scr_y1 = 32;
    s->touch_scr_x2 = 224;
    s->touch_scr_y2 = 160;
    s->touch_adc_x1 = (uint16_t)(32u * DEFAULT_UNITS_PER_PIXEL);
    s->touch_adc_y1 = (uint16_t)(32u * DEFAULT_UNITS_PER_PIXEL);
    s->touch_adc_x2 = (uint16_t)(224u * DEFAULT_UNITS_PER_PIXEL);
    s->touch_adc_y2 = (uint16_t)(160u * DEFAULT_UNITS_PER_PIXEL);
    s->flags = NDS_SETTINGS_FLAGS_OKAY;
}

void nds_user_settings_ram_block(const NdsUserSettings *s, uint8_t out[NDS_USER_SETTINGS_SIZE])
{
    uint32_t nickname_length = s->nickname_length <= NDS_NICKNAME_MAX ? s->nickname_length : NDS_NICKNAME_MAX;
    uint32_t message_length = s->message_length <= NDS_MESSAGE_MAX ? s->message_length : NDS_MESSAGE_MAX;
    uint32_t language = s->language;

    if (language > NDS_LANGUAGE_SPANISH) language = NDS_LANGUAGE_ENGLISH;

    /* What is not set below is zero: the unused bytes at 05h and 57h, the
     * strings' padding, the year at 66h and the RTC offset at 68h, which
     * the firmware changes only when the owner sets the clock in its menu. */
    memset(out, 0, NDS_USER_SETTINGS_SIZE);
    nds_put_le16(out + US_VERSION, 5);
    out[US_COLOUR] = s->favourite_colour & 15u;
    out[US_BIRTH_MONTH] = s->birthday_month;
    out[US_BIRTH_DAY] = s->birthday_day;
    for (uint32_t i = 0; i < nickname_length; i++) nds_put_le16(out + US_NICKNAME + 2u * i, s->nickname[i]);
    nds_put_le16(out + US_NICKNAME_LENGTH, (uint16_t)nickname_length);
    for (uint32_t i = 0; i < message_length; i++) nds_put_le16(out + US_MESSAGE + 2u * i, s->message[i]);
    nds_put_le16(out + US_MESSAGE_LENGTH, (uint16_t)message_length);
    out[US_ALARM_HOUR] = s->alarm_hour;
    out[US_ALARM_MINUTE] = s->alarm_minute;
    out[US_ALARM_ENABLE] = s->alarm_enabled ? 0x80u : 0u;
    nds_put_le16(out + US_TOUCH_ADC1, s->touch_adc_x1);
    nds_put_le16(out + US_TOUCH_ADC1 + 2, s->touch_adc_y1);
    out[US_TOUCH_SCR1] = s->touch_scr_x1;
    out[US_TOUCH_SCR1 + 1] = s->touch_scr_y1;
    nds_put_le16(out + US_TOUCH_ADC2, s->touch_adc_x2);
    nds_put_le16(out + US_TOUCH_ADC2 + 2, s->touch_adc_y2);
    out[US_TOUCH_SCR2] = s->touch_scr_x2;
    out[US_TOUCH_SCR2 + 1] = s->touch_scr_y2;
    nds_put_le16(out + US_LANGUAGE_FLAGS, (uint16_t)((s->flags & 0xFFF8u) | language));
}

void nds_user_settings_from_block(NdsUserSettings *s, const uint8_t block[NDS_USER_SETTINGS_SIZE])
{
    uint32_t nickname_length = nds_get_le16(block + US_NICKNAME_LENGTH);
    uint32_t message_length = nds_get_le16(block + US_MESSAGE_LENGTH);
    uint16_t language_flags = nds_get_le16(block + US_LANGUAGE_FLAGS);

    if (nickname_length > NDS_NICKNAME_MAX) nickname_length = NDS_NICKNAME_MAX;
    if (message_length > NDS_MESSAGE_MAX) message_length = NDS_MESSAGE_MAX;

    memset(s, 0, sizeof *s);
    for (uint32_t i = 0; i < nickname_length; i++) s->nickname[i] = nds_get_le16(block + US_NICKNAME + 2u * i);
    for (uint32_t i = 0; i < message_length; i++) s->message[i] = nds_get_le16(block + US_MESSAGE + 2u * i);
    s->nickname_length = (uint8_t)nickname_length;
    s->message_length = (uint8_t)message_length;
    s->favourite_colour = block[US_COLOUR] & 15u;
    s->birthday_month = block[US_BIRTH_MONTH];
    s->birthday_day = block[US_BIRTH_DAY];
    s->language = (uint8_t)(language_flags & 7u);
    s->alarm_hour = block[US_ALARM_HOUR];
    s->alarm_minute = block[US_ALARM_MINUTE];
    s->alarm_enabled = (block[US_ALARM_ENABLE] & 0x80u) ? 1u : 0u;
    s->touch_adc_x1 = nds_get_le16(block + US_TOUCH_ADC1);
    s->touch_adc_y1 = nds_get_le16(block + US_TOUCH_ADC1 + 2);
    s->touch_scr_x1 = block[US_TOUCH_SCR1];
    s->touch_scr_y1 = block[US_TOUCH_SCR1 + 1];
    s->touch_adc_x2 = nds_get_le16(block + US_TOUCH_ADC2);
    s->touch_adc_y2 = nds_get_le16(block + US_TOUCH_ADC2 + 2);
    s->touch_scr_x2 = block[US_TOUCH_SCR2];
    s->touch_scr_y2 = block[US_TOUCH_SCR2 + 1];
    s->flags = (uint16_t)(language_flags & 0xFFF8u);
}

/* One copy as the flash holds it: the block, an update counter, the CRC,
 * and FFh up to the next 100h. */
static void put_settings_copy(uint8_t *copy, const NdsUserSettings *s, uint16_t counter)
{
    nds_user_settings_ram_block(s, copy);
    nds_put_le16(copy + US_COUNTER, counter);
    nds_put_le16(copy + US_CRC, nds_crc16(copy, NDS_USER_SETTINGS_SIZE, 0xFFFFu));
}

static bool settings_copy_valid(const uint8_t *copy)
{
    return nds_crc16(copy, NDS_USER_SETTINGS_SIZE, 0xFFFFu) == nds_get_le16(copy + US_CRC);
}

bool nds_firmware_current_settings(const uint8_t image[NDS_FIRMWARE_SIZE], uint32_t *offset)
{
    /* Header[20h] is the first copy's offset in units of 8 bytes; the
     * second copy is 100h above it. */
    uint32_t first = (uint32_t)nds_get_le16(image + 0x20) * 8u;
    if (first > NDS_FIRMWARE_SIZE - 2u * NDS_FIRMWARE_SETTINGS_SPACING) return false;

    const uint8_t *copy0 = image + first;
    const uint8_t *copy1 = copy0 + NDS_FIRMWARE_SETTINGS_SPACING;
    bool valid0 = settings_copy_valid(copy0);
    bool valid1 = settings_copy_valid(copy1);
    if (!valid0 && !valid1) return false;

    /* "IF count1=((count0+1) AND 7Fh) THEN area1=newer ELSE area0=newer" */
    bool second = valid1;
    if (valid0 && valid1) {
        uint32_t count0 = nds_get_le16(copy0 + US_COUNTER);
        uint32_t count1 = nds_get_le16(copy1 + US_COUNTER);
        second = count1 == ((count0 + 1u) & 0x7Fu);
    }
    *offset = second ? first + NDS_FIRMWARE_SETTINGS_SPACING : first;
    return true;
}

/* ------------------------------------------------------------------ */
/* Touch screen calibration                                            */
/* ------------------------------------------------------------------ */

static int32_t floor_div(int32_t a, int32_t b)
{
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/*
 * One axis. Software turns a reading into a pixel with the line through the
 * two calibration points:
 *
 *   pixel = (adc - adc1) * (scr2 - scr1) / (adc2 - adc1) + scr1
 *
 * This solves it for the reading in the middle of the pixel, so that the
 * rounding in the software's arithmetic cannot tip the result into a
 * neighbouring pixel.
 *
 * GBATEK ("DS Touch Screen Controller (TSC)", "Touchscreen Position") ends
 * the formula with "+ (scr1 - 1)", taking the stored pixel coordinates to
 * count from 1. Software we can read does not: libnds's touchInit() and
 * touchApplyCalibration() (source/arm7/touch.c) map the midpoint of the two
 * readings to (scr1 + scr2) / 2 with no such offset. The emulators are
 * split the same way: melonDS's calibration (reading = 16 * pixel, pinned
 * at pixels (0,0) and (255,191)) is only right without the offset, and
 * DeSmuME's (the same scale, pinned one pixel higher) only with it. We
 * follow the software, as it is what reads our values; if GBATEK is right
 * about Nintendo's own library, a game sees touches one pixel up and left.
 */
static uint16_t adc_for_pixel(int pixel, int adc1, int adc2, int scr1, int scr2)
{
    int32_t pixels = scr2 - scr1;
    int32_t units = adc2 - adc1;
    int32_t adc = adc1;
    if (pixels != 0) adc += floor_div((2 * (pixel - scr1) + 1) * units, 2 * pixels);
    /* 000h and FFFh are what the controller reads with the pen up. */
    if (adc < 0x001) adc = 0x001;
    if (adc > 0xFFE) adc = 0xFFE;
    return (uint16_t)adc;
}

void nds_touch_adc_from_screen(const NdsUserSettings *s, int x, int y, uint16_t *adc_x, uint16_t *adc_y)
{
    if (x < 0) x = 0;
    if (x > NDS_SCREEN_WIDTH - 1) x = NDS_SCREEN_WIDTH - 1;
    if (y < 0) y = 0;
    if (y > NDS_SCREEN_HEIGHT - 1) y = NDS_SCREEN_HEIGHT - 1;
    *adc_x = adc_for_pixel(x, s->touch_adc_x1, s->touch_adc_x2, s->touch_scr_x1, s->touch_scr_x2);
    *adc_y = adc_for_pixel(y, s->touch_adc_y1, s->touch_adc_y2, s->touch_scr_y1, s->touch_scr_y2);
}

/* ------------------------------------------------------------------ */
/* Wi-Fi calibration                                                   */
/* ------------------------------------------------------------------ */

/* Offsets in the image ("DS Firmware Wifi Calibration Data"). */
enum {
    WIFI_CRC = 0x2A,          /* 2: CRC16 of [2Ch, 2Ch + length), initial value 0 */
    WIFI_LENGTH = 0x2C,       /* 2 */
    WIFI_VERSION = 0x2F,      /* 1 */
    WIFI_MAC = 0x36,          /* 6 */
    WIFI_CHANNELS = 0x3C,     /* 2 */
    WIFI_FLAGS = 0x3E,        /* 2 */
    WIFI_RF_TYPE = 0x40,      /* 1 */
    WIFI_RF_BITS = 0x41,      /* 1 */
    WIFI_RF_ENTRIES = 0x42,   /* 1 */
    WIFI_UNKNOWN_43 = 0x43,   /* 1 */
    WIFI_CONFIG = 0x44,       /* 16 x 2: initial values of 16 MAC registers */
    WIFI_BB = 0x64,           /* 69h: initial values of the baseband chip's registers */
    WIFI_RF_INIT = 0xCE,      /* 12 x 3: initial values of the RF chip's registers */
    WIFI_RF_CHANNEL = 0xF2,   /* 14 x 2 x 3: RF[5] and RF[6] for channels 1-14 */
    WIFI_BB_CHANNEL = 0x146,  /* 14: BB[1Eh] for channels 1-14 */
    WIFI_RF9_CHANNEL = 0x154, /* 14: RF[9] bits 10-14 for channels 1-14 */
    WIFI_UNKNOWN_162 = 0x162, /* 1 */
    WIFI_END = 0x200,
};

#define WIFI_CONFIG_LENGTH 0x138u /* "usually 0138h, ie. entries 2Ch..163h" */

/* The address the built image carries: Nintendo's prefix for the original
 * DS (00:09:BF, GBATEK) and a suffix of our own, "NC" 01. */
static const uint8_t k_default_mac[6] = { 0x00, 0x09, 0xBF, 0x4E, 0x43, 0x01 };

/*
 * What the 16 MAC registers hold once initialised from the firmware, in the
 * order the firmware stores them. From "DS Wifi Configuration Ports", which
 * lists each port's value "after initialization from firmware settings" and
 * notes that they "seem to be identical in all currently existing
 * consoles". The one that list does not give is W_POWER_TX, which gets the
 * port's documented reset value.
 */
static const uint16_t k_wifi_config[16] = {
    0x0002, /* 44h W_CONFIG_146h */
    0x0017, /* 46h W_CONFIG_148h */
    0x0026, /* 48h W_CONFIG_14Ah */
    0x1818, /* 4Ah W_CONFIG_14Ch */
    0x0048, /* 4Ch W_CONFIG_120h */
    0x4840, /* 4Eh W_CONFIG_122h */
    0x0058, /* 50h W_CONFIG_154h */
    0x0042, /* 52h W_CONFIG_144h */
    0x0140, /* 54h W_CONFIG_130h */
    0x8064, /* 56h W_CONFIG_132h */
    0xE0E0, /* 58h W_CONFIG_140h */
    0x2443, /* 5Ah W_CONFIG_142h */
    0x0003, /* 5Ch W_POWER_TX */
    0x0032, /* 5Eh W_CONFIG_124h */
    0x01F4, /* 60h W_CONFIG_128h */
    0x0101, /* 62h W_CONFIG_150h */
};

/*
 * The RF9008's twelve initial register values, in the firmware's order
 * (RF[0,4,5,6,7,8,9,0Ah,0Bh,1,2,3]). Each is 24 bits: the register number
 * in bits 18-22 and 18 bits of data. From the table "RF chip data (Type2)
 * (initial NDS settings from firmware, example)" in "DS Wifi RF9008
 * Registers".
 */
static const uint32_t k_rf_init[12] = {
    0x00C007, 0x129C03, 0x141728, 0x1AE8BA, 0x1D456F, 0x23FFFA,
    0x241D30, 0x280001, 0x2C0000, 0x069C03, 0x080022, 0x0DFF6F,
};

static void put_le24(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
}

/*
 * RF[5] and RF[6] for a channel: the synthesiser's divider. RF[5] bits 6-17
 * are the whole part N and the fraction's 24 bits are split between RF[5]
 * bits 0-5 (the top six) and RF[6] (the low eighteen). The oscillator runs
 * at 22 MHz * (N + fraction / 2^24) and sits 374 MHz, the chip's
 * intermediate frequency, below the channel: GBATEK's example values for
 * channel 1 (RF[5] = 01728h, RF[6] = 2E8BAh) are N = 92 and a fraction of
 * 14/22, which is 2038 MHz, and channel 1 is 2412 MHz. The same sum gives
 * the other thirteen. Channels are 5 MHz apart, with channel 14 at 2484.
 */
static void rf_channel(uint32_t channel, uint32_t *rf5, uint32_t *rf6)
{
    uint32_t mhz = channel == 14u ? 2484u : 2407u + 5u * channel;
    uint32_t oscillator = mhz - 374u;
    uint32_t whole = oscillator / 22u;
    uint32_t fraction = (((oscillator % 22u) << 24) + 11u) / 22u; /* to the nearest */
    *rf5 = (5u << 18) | (whole << 6) | (fraction >> 18);
    *rf6 = (6u << 18) | (fraction & 0x3FFFFu);
}

static void put_wifi_crc(uint8_t *image)
{
    uint32_t length = nds_get_le16(image + WIFI_LENGTH);
    if (length > WIFI_END - WIFI_LENGTH) length = WIFI_END - WIFI_LENGTH;
    nds_put_le16(image + WIFI_CRC, nds_crc16(image + WIFI_LENGTH, length, 0));
}

static void put_wifi_calibration(uint8_t *image)
{
    /* The block's own fields; 2Eh and 30h-35h are unused and zero on the
     * original DS, as is CDh. Version 0 is firmware v1-v4. */
    memset(image + WIFI_LENGTH, 0, WIFI_CONFIG_LENGTH);
    nds_put_le16(image + WIFI_LENGTH, (uint16_t)WIFI_CONFIG_LENGTH);
    image[WIFI_VERSION] = 0;
    memcpy(image + WIFI_MAC, k_default_mac, sizeof k_default_mac);
    nds_put_le16(image + WIFI_CHANNELS, 0x3FFE); /* channels 1-13 */
    nds_put_le16(image + WIFI_FLAGS, 0xFFFF);
    image[WIFI_RF_TYPE] = 2;      /* RF9008 */
    image[WIFI_RF_BITS] = 0x18;   /* 24 bits to an RF value */
    image[WIFI_RF_ENTRIES] = 12;
    image[WIFI_UNKNOWN_43] = 1;

    for (uint32_t i = 0; i < 16u; i++) nds_put_le16(image + WIFI_CONFIG + 2u * i, k_wifi_config[i]);

    /* The baseband chip. "DS Wifi Baseband Chip (BB)" gives the chip's ID,
     * the one register that always reads 01h, and the initial values of
     * the registers software depends on; the rest are not documented and
     * are left zero. Bit 7 of BB[01h] has to be set: bringing the radio up
     * clears and restores it. */
    image[WIFI_BB + 0x00] = 0x6D; /* chip ID */
    image[WIFI_BB + 0x01] = 0x9E;
    image[WIFI_BB + 0x13] = 0x00; /* carrier sense only */
    image[WIFI_BB + 0x1E] = 0xBB; /* gain */
    image[WIFI_BB + 0x35] = 0x1F; /* energy detection threshold */
    image[WIFI_BB + 0x5D] = 0x01;

    for (uint32_t i = 0; i < 12u; i++) put_le24(image + WIFI_RF_INIT + 3u * i, k_rf_init[i]);
    for (uint32_t channel = 1; channel <= 14u; channel++) {
        uint32_t rf5, rf6;
        rf_channel(channel, &rf5, &rf6);
        put_le24(image + WIFI_RF_CHANNEL + 6u * (channel - 1u), rf5);
        put_le24(image + WIFI_RF_CHANNEL + 6u * (channel - 1u) + 3u, rf6);
    }
    /* Per-channel gain: consoles differ ("usually somewhat B1h..B7h"); the
     * middle of that. Per-channel transmit gain bits: "usually 10h-filled". */
    memset(image + WIFI_BB_CHANNEL, 0xB4, 14);
    memset(image + WIFI_RF9_CHANNEL, 0x10, 14);
    image[WIFI_UNKNOWN_162] = 0x1A; /* "usually 19h..1Ch" */
    image[WIFI_UNKNOWN_162 + 1] = 0xFF; /* unused, inside the CRC */
    put_wifi_crc(image);
}

void nds_firmware_set_mac(uint8_t image[NDS_FIRMWARE_SIZE], const uint8_t mac[6])
{
    memcpy(image + WIFI_MAC, mac, 6);
    put_wifi_crc(image);
}

/* ------------------------------------------------------------------ */
/* The image                                                           */
/* ------------------------------------------------------------------ */

void nds_firmware_build(uint8_t image[NDS_FIRMWARE_SIZE], const NdsUserSettings *s)
{
    /* Erased flash reads FFh, and so does everything a real image fills
     * with code: 200h up to the access points, and the unused slot at
     * 3FD00h. */
    memset(image, 0xFF, NDS_FIRMWARE_SIZE);

    /*
     * The header, 00h-29h ("DS Firmware Header"). The addresses and CRCs of
     * the five parts of boot code and data are zero: there are none. (The
     * DSi's flash, which has no boot code either, zero-fills them too.)
     */
    memset(image, 0, 0x2A);
    memcpy(image + 0x08, "NCFW", 4);    /* identifier: Nintendo's is "MAC" and a version letter */
    nds_put_le16(image + 0x14, 0x2000); /* bits 12-15: chip size / 128 KiB */
    image[0x1D] = 0xFF;                 /* console type: Nintendo DS */
    image[0x1E] = 0xFF;                 /* unused */
    image[0x1F] = 0xFF;
    nds_put_le16(image + 0x20, (uint16_t)(NDS_FIRMWARE_SETTINGS_OFFSET / 8u));
    image[0x28] = 0xFF; /* unused */
    image[0x29] = 0xFF;

    put_wifi_calibration(image);

    /*
     * Three access point slots, none configured ("DS Firmware Wifi Internet
     * Access Points"): zero, with the status byte at E7h saying "not
     * configured" and a CRC16 (initial value 0) of the first FEh bytes at
     * FEh. A game that offers Nintendo Wi-Fi Connection reads all three and
     * wants the CRCs right.
     */
    for (uint32_t slot = 0; slot < 3u; slot++) {
        uint8_t *ap = image + NDS_FIRMWARE_ACCESS_POINT_OFFSET + 0x100u * slot;
        memset(ap, 0, 0x100);
        ap[0xE7] = 0xFF;
        nds_put_le16(ap + 0xFE, nds_crc16(ap, 0xFE, 0));
    }

    /* Both copies of the user settings. The firmware writes them in turn,
     * raising the counter each time: here the second is the later. */
    put_settings_copy(image + NDS_FIRMWARE_SETTINGS_OFFSET, s, 0);
    put_settings_copy(image + NDS_FIRMWARE_SETTINGS_OFFSET + NDS_FIRMWARE_SETTINGS_SPACING, s, 1);
}
