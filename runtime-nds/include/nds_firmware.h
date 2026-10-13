/*
 * The firmware flash image and the user settings in it.
 *
 * A DS keeps its firmware in a 256 KiB serial flash chip: a header, the
 * calibration data of the Wi-Fi hardware, the boot code and menu, three
 * Wi-Fi access point slots, and two copies of the user settings (nickname,
 * language, touch screen calibration and so on). Games never run the
 * firmware's code, but they do read the flash: the header for the address of
 * the user settings, the settings themselves, and the Wi-Fi calibration when
 * they bring up the radio.
 *
 * Our build has no Nintendo firmware. nds_firmware_build() writes an image
 * with everything a game looks at and nothing else: no code, and no byte
 * taken from a real console. The layout is GBATEK's ("DS Firmware Header",
 * "DS Firmware Wifi Calibration Data", "DS Firmware Wifi Internet Access
 * Points", "DS Firmware User Settings"); nds_firmware.c says where each
 * value comes from.
 */
#ifndef NDS_FIRMWARE_H
#define NDS_FIRMWARE_H

#include "nds_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The flash chip of the original DS: 256 KiB. */
#define NDS_FIRMWARE_SIZE 0x40000

/* The part of a user settings copy the firmware also leaves in RAM. */
#define NDS_USER_SETTINGS_SIZE 0x70
/* Where: 0x027FFC80 (GBATEK, "BIOS RAM Usage"). */
#define NDS_USER_SETTINGS_RAM_ADDRESS 0x027FFC80u

/* The two copies in the image, and the three access point slots below
 * them. The header holds the first copy's offset divided by 8. */
#define NDS_FIRMWARE_SETTINGS_OFFSET 0x3FE00u
#define NDS_FIRMWARE_SETTINGS_SPACING 0x100u
#define NDS_FIRMWARE_ACCESS_POINT_OFFSET 0x3FA00u

/* Language numbers ("Language and Flags", bits 0-2). */
enum {
    NDS_LANGUAGE_JAPANESE = 0,
    NDS_LANGUAGE_ENGLISH = 1,
    NDS_LANGUAGE_FRENCH = 2,
    NDS_LANGUAGE_GERMAN = 3,
    NDS_LANGUAGE_ITALIAN = 4,
    NDS_LANGUAGE_SPANISH = 5,
};

/* Bits of "Language and Flags" above the language, as NdsUserSettings.flags
 * holds them. The firmware asks for the settings again unless all of the
 * "settings okay" bits are set. */
#define NDS_SETTINGS_FLAG_GBA_LOWER_SCREEN 0x0008u
#define NDS_SETTINGS_FLAG_BACKLIGHT_MASK 0x0030u /* DS Lite only */
#define NDS_SETTINGS_FLAG_AUTOSTART 0x0040u
#define NDS_SETTINGS_FLAG_LOST 0x0200u
#define NDS_SETTINGS_FLAGS_OKAY 0xFC00u /* bits 10, 11, 13, 14, 15 (12 has no function) */

#define NDS_NICKNAME_MAX 10
#define NDS_MESSAGE_MAX 26

/*
 * What the owner entered in the firmware's settings menu. Plain data.
 *
 * The touch screen calibration is two points, each the controller's 12-bit
 * reading (adc) at a known pixel (scr). The firmware asks the player to tap
 * (32,32) and (224,160).
 */
typedef struct NdsUserSettings {
    uint16_t nickname[NDS_NICKNAME_MAX]; /* UTF-16 code units, not terminated */
    uint16_t message[NDS_MESSAGE_MAX];
    uint8_t nickname_length; /* 0-10 */
    uint8_t message_length;  /* 0-26 */
    uint8_t favourite_colour; /* 0-15 */
    uint8_t birthday_month;   /* 1-12 */
    uint8_t birthday_day;     /* 1-31 */
    uint8_t language;         /* NDS_LANGUAGE_* */
    uint8_t alarm_hour;       /* 0-23 */
    uint8_t alarm_minute;     /* 0-59 */
    uint8_t alarm_enabled;    /* 0 or 1 */
    uint8_t touch_scr_x1, touch_scr_y1;
    uint8_t touch_scr_x2, touch_scr_y2;
    uint16_t touch_adc_x1, touch_adc_y1;
    uint16_t touch_adc_x2, touch_adc_y2;
    uint16_t flags; /* NDS_SETTINGS_FLAG_*: bits 3-15 of "Language and Flags" */
} NdsUserSettings;

/* English, the nickname "Player", no message, and a calibration of 16
 * controller units to the pixel on both axes (see nds_firmware.c for why). */
void nds_user_settings_default(NdsUserSettings *s);

/* The BIOS's CRC16 (SWI 0Eh, GetCRC16), which is the checksum of every
 * block in the firmware. */
uint16_t nds_crc16(const uint8_t *data, size_t length, uint16_t initial);

/*
 * A firmware flash image a game's own code accepts: the header fields games
 * read, a Wi-Fi calibration block with a valid CRC (games initialise the
 * radio and some refuse to continue without it), three unconfigured access
 * point slots, and both copies of the user settings with their update
 * counters and CRCs. The second copy is the newer one. No Nintendo code or
 * data; everything else reads FFh, as erased flash does.
 */
void nds_firmware_build(uint8_t image[NDS_FIRMWARE_SIZE], const NdsUserSettings *s);

/* The 0x70-byte user settings block the firmware leaves at 0x027FFC80 when
 * it starts a game. */
void nds_user_settings_ram_block(const NdsUserSettings *s, uint8_t out[NDS_USER_SETTINGS_SIZE]);

/* The reverse: the settings in a block, for an image that was not built
 * here (a saved one a game has since rewritten, or a user's own dump). */
void nds_user_settings_from_block(NdsUserSettings *s, const uint8_t block[NDS_USER_SETTINGS_SIZE]);

/*
 * The offset of the user settings copy in force, found the way the firmware
 * and games find it: the header gives the first copy's place, a copy counts
 * if its CRC16 is right, and of two good copies the one whose update counter
 * is one above the other's is the newer. False if neither copy is good.
 */
bool nds_firmware_current_settings(const uint8_t image[NDS_FIRMWARE_SIZE], uint32_t *offset);

/* Replaces the console's MAC address in an image and corrects the Wi-Fi
 * block's CRC. The built image has 00:09:BF:4E:43:01; two consoles that
 * are to talk to each other need different addresses. */
void nds_firmware_set_mac(uint8_t image[NDS_FIRMWARE_SIZE], const uint8_t mac[6]);

/*
 * Screen pixel to the touch controller's 12-bit ADC values, by the
 * calibration in the settings. (x, y) is a pixel of the lower screen,
 * (0,0) top left; coordinates outside the screen are brought to its edge.
 * The result is the middle of the range of readings that software turns
 * back into that pixel, kept inside 001h-FFEh so that it never looks like
 * the pen-up reading.
 */
void nds_touch_adc_from_screen(const NdsUserSettings *s, int x, int y, uint16_t *adc_x, uint16_t *adc_y);

#ifdef __cplusplus
}
#endif

#endif /* NDS_FIRMWARE_H */
