/*
 * A console's settings, and the two files a game reads them from.
 *
 * Everything the Wii's settings screens decide - language, picture shape,
 * sound, where the sensor bar sits, which remotes belong to the console -
 * is kept in one 16 KB file in the flash filesystem,
 * /shared2/sys/SYSCONF. What the factory decided - the region, the video
 * standard, the serial number - is in a second, 256-byte file,
 * /title/00000001/00000002/data/setting.txt, which is lines of text under a
 * rotating XOR key. Every game reads both before it draws anything: the
 * SDK's SC library at start-up, libogc's CONF likewise.
 *
 * There is no settings screen here and no factory, so both files are
 * written from a WiiSettings: a small structure a frontend can fill in and
 * the defaults of a North American console otherwise. The files produced
 * are the real formats byte for byte, because the readers are a game's own
 * code and cannot be told otherwise.
 *
 * The formats are from WiiBrew's pages on the two files, libogc's conf.c
 * and conf.h (zlib licence: layouts and constants reused, no code), and the
 * SDK's SC library as decompiled in doldecomp/ogws (read as a reference for
 * what a retail game insists on). wii_sysconf.c says which fact came from
 * where.
 */
#ifndef WII_SYSCONF_H
#define WII_SYSCONF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where a console was sold. It decides setting.txt's AREA, MODEL, CODE and
 * GAME lines; the numbers are ours. */
enum { WII_REGION_JPN = 0, WII_REGION_USA = 1, WII_REGION_EUR = 2, WII_REGION_KOR = 3 };

/* The video standard: setting.txt's VIDEO line. */
enum { WII_VIDEO_NTSC = 0, WII_VIDEO_PAL = 1, WII_VIDEO_MPAL = 2 };

/* The system language, as SYSCONF's IPL.LNG holds it. */
enum {
  WII_LANGUAGE_JAPANESE = 0,
  WII_LANGUAGE_ENGLISH = 1,
  WII_LANGUAGE_GERMAN = 2,
  WII_LANGUAGE_FRENCH = 3,
  WII_LANGUAGE_SPANISH = 4,
  WII_LANGUAGE_ITALIAN = 5,
  WII_LANGUAGE_DUTCH = 6,
  WII_LANGUAGE_SIMPLIFIED_CHINESE = 7,
  WII_LANGUAGE_TRADITIONAL_CHINESE = 8,
  WII_LANGUAGE_KOREAN = 9
};

/* IPL.SND. */
enum { WII_SOUND_MONO = 0, WII_SOUND_STEREO = 1, WII_SOUND_SURROUND = 2 };

/* What a frontend may choose. Every field is a small integer or plain
 * text, so the structure can be written to a preferences file as it is. */
typedef struct WiiSettings {
  uint8_t region;         /* WII_REGION_* */
  uint8_t video;          /* WII_VIDEO_* */
  uint8_t language;       /* WII_LANGUAGE_* */
  uint8_t widescreen;     /* 0 for 4:3, 1 for 16:9 */
  uint8_t sound;          /* WII_SOUND_* */
  uint8_t progressive;    /* 480p, where the cable allows it */
  uint8_t sixty_hertz;    /* IPL.E60: a PAL console's 60 Hz mode; set on the others too */
  uint8_t screen_saver;   /* dim the picture when nothing moves */
  uint8_t sensor_bar_top; /* 1 when the bar sits above the screen, 0 below */
  uint8_t sensitivity;    /* the pointer camera's, 1 to 5 */
  uint8_t speaker_volume; /* the remote's speaker, 0 to 127 */
  uint8_t rumble;         /* the remote's motor */
  int8_t display_offset;  /* the picture moved sideways, -32 to 32 */
  uint8_t country;        /* the address book's country code; 49 is the United States */
  uint8_t reserved[2];
  /* Seconds to add to the clock chip's count to get the time: zero, since
   * the clock chip here already reads the time. */
  uint32_t counter_bias;
  /* The console's name: up to ten characters, ASCII here. */
  char nickname[11];
  uint8_t reserved1;
} WiiSettings;

/* A North American console as it leaves its first set-up: English, 4:3,
 * stereo, 60 Hz, interlaced, the sensor bar on top of the screen, called
 * "Wii". */
void wii_settings_default(WiiSettings *settings);

#define WII_SYSCONF_SIZE 0x4000u
#define WII_SETTING_TXT_SIZE 0x100u

/* SYSCONF's item types, as the top three bits of an item's first byte. */
enum {
  WII_SYSCONF_BIGARRAY = 1,
  WII_SYSCONF_SMALLARRAY = 2,
  WII_SYSCONF_BYTE = 3,
  WII_SYSCONF_SHORT = 4,
  WII_SYSCONF_LONG = 5,
  WII_SYSCONF_LONGLONG = 6,
  WII_SYSCONF_BOOL = 7
};

/* The size of SYSCONF's list of registered remotes (BT.DINF): a count,
 * ten registered remotes and six active ones, each a six-byte address and
 * a sixty-four-byte name. */
#define WII_SYSCONF_REMOTE_ENTRY 70u
#define WII_SYSCONF_REMOTES_SIZE (1u + 16u * WII_SYSCONF_REMOTE_ENTRY)

/* Write the two files. Both return zero, or -1 with nothing useful in
 * `out` when the settings cannot be expressed (a field out of range). The
 * output depends on the settings alone: the same settings give the same
 * bytes on every host. */
int wii_sysconf_build(const WiiSettings *settings, uint8_t out[WII_SYSCONF_SIZE]);
int wii_setting_txt_build(const WiiSettings *settings, uint8_t out[WII_SETTING_TXT_SIZE]);

/* setting.txt's obfuscation, which is its own inverse: every byte is XORed
 * with the low byte of a key that rotates left one bit a byte. `offset` is
 * where in the file `bytes` begins. */
void wii_setting_txt_crypt(uint8_t *bytes, size_t length, size_t offset);

/* Find an item in a SYSCONF file by name. Returns its value's first byte
 * (for an array, the first byte after the length), with its type and its
 * length in bytes, or NULL when the file is not a SYSCONF or has no such
 * item. For a frontend that wants to show what a game changed. */
const uint8_t *wii_sysconf_find(const uint8_t *file, size_t size, const char *name, unsigned *type,
                                uint32_t *length);

#ifdef __cplusplus
}
#endif

#endif /* WII_SYSCONF_H */
