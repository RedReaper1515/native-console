/*
 * SYSCONF and setting.txt, written from a WiiSettings (wii_sysconf.h).
 *
 * SYSCONF (WiiBrew, "/shared2/sys/SYSCONF"; libogc's conf.c reads it one
 * way and the SDK's SC library another, and the file has to satisfy both):
 *
 *   0x0000  "SCv0"
 *   0x0004  the number of items, 16 bits
 *   0x0006  an offset for each item, 16 bits each, in the order the items
 *           are stored, and one more: where the last item ends
 *   ...     the items, back to back, the first straight after the offsets
 *   0x3fae  thirty-nine 16-bit entries, one for each setting the system
 *           software has a name for, the first name's in the *last* entry:
 *           where in the offset table that setting's offset is, or zero
 *   0x3ffc  "SCed"
 *
 * An item is one byte holding its type in the top three bits and the
 * length of its name, less one, in the bottom five; the name; and the
 * value. A byte, a short, a long and a long long are that many bytes, most
 * significant first; a boolean is one. An array is its length *less one* -
 * in one byte for a small array and two for a big one - and then its
 * bytes.
 *
 * What each reader insists on, and so what is guaranteed here:
 *
 *  - libogc wants the file to be 0x4000 bytes and to begin "SCv0". It finds
 *    an item by walking the offset table and comparing names, and never
 *    looks at the table at the end.
 *  - The SDK wants both magics, the first item to start exactly where the
 *    offset table ends, every item to start exactly where the one before
 *    ended, the extra offset to be where the last ends, and all of it to
 *    stop short of the table at the end. It then zeroes that table and
 *    builds its own - so the one written here is never trusted by a game,
 *    and is correct anyway. A game built with an early SDK knows fewer
 *    names and so a shorter table; the items end kilobytes before either.
 *  - Both ask for an array by its exact length (the SDK refuses an item
 *    whose length differs from the structure it is reading into), which is
 *    why the sizes below are a real file's and not round numbers.
 *
 * setting.txt (WiiBrew's page of that name; the SDK's reader is
 * scapi_prdinfo.c): lines of NAME=value ending in a carriage return and a
 * line feed, each byte XORed with the low byte of a 32-bit key that starts
 * at 0x73b5dbfa and rotates left one bit per byte, in a file of 256 bytes.
 * The bytes after the text are plain zeroes, not zeroes under the key: the
 * SDK's reader treats a stored zero as "nothing here" and does not unmask
 * it. (For the same reason a character that the key would turn into zero
 * cannot be stored at all; building refuses a text in which that happens,
 * and none of the texts this file can produce does.)
 */

#include <stdio.h>
#include <string.h>

#include "wii_ios.h"
#include "wii_remote.h"
#include "wii_sysconf.h"

/* ------------------------------------------------------------------ */
/* The settings                                                        */

void wii_settings_default(WiiSettings *settings) {
  memset(settings, 0, sizeof(*settings));
  settings->region = WII_REGION_USA;
  settings->video = WII_VIDEO_NTSC;
  settings->language = WII_LANGUAGE_ENGLISH;
  settings->widescreen = 0;
  settings->sound = WII_SOUND_STEREO;
  settings->progressive = 0;
  settings->sixty_hertz = 1;
  settings->screen_saver = 1;
  /* Below the screen: where the remote's camera model puts the bar's two
   * lights (wii_remote.c), which has to be where a game is told it is. */
  settings->sensor_bar_top = 0;
  settings->sensitivity = 3;
  /* A little over two thirds: the SDK's own answer when the setting is
   * missing is one more than this. */
  settings->speaker_volume = 0x58;
  settings->rumble = 1;
  settings->display_offset = 0;
  settings->country = 49;
  settings->counter_bias = 0;
  memcpy(settings->nickname, "Wii", 4);
}

static int settings_valid(const WiiSettings *s) {
  size_t i, length;
  if (s->region > WII_REGION_KOR || s->video > WII_VIDEO_MPAL) return 0;
  if (s->language > WII_LANGUAGE_KOREAN || s->sound > WII_SOUND_SURROUND) return 0;
  if (s->sensitivity < 1u || s->sensitivity > 5u || s->speaker_volume > 127u) return 0;
  if (s->display_offset < -32 || s->display_offset > 32) return 0;
  for (length = 0; length < sizeof(s->nickname) && s->nickname[length]; length++) {
  }
  /* libogc calls a console with no name a broken file. */
  if (length == 0 || length > 10u) return 0;
  for (i = 0; i < length; i++) {
    if ((unsigned char)s->nickname[i] < 0x20u || (unsigned char)s->nickname[i] > 0x7eu) return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* SYSCONF                                                             */

/* The settings the system software has names for, in the order of its own
 * table: a setting's position here is its number, and the table at the end
 * of the file is indexed by that number, backwards. The order is the one
 * WiiBrew lists for that table, and its first thirty-five (less BT.CDIF,
 * which a later SDK slipped in) are the names in the SDK's scsystem.c. */
enum {
  IPL_CB, IPL_AR, IPL_ARN, IPL_CD, IPL_CD2, IPL_DH, IPL_E60, IPL_EULA, IPL_FRC, IPL_IDL,
  IPL_INC, IPL_LNG, IPL_NIK, IPL_PC, IPL_PGS, IPL_SSV, IPL_SADR, IPL_SND, IPL_UPT, NET_CNF,
  NET_CTPC, NET_PROF, NET_WCPC, NET_WCFG, DEV_BTM, DEV_VIM, DEV_CTC, DEV_DSM, BT_DINF, BT_CDIF,
  BT_SENS, BT_SPKV, BT_MOT, BT_BAR, DVD_CNF, WWW_RST, IPL_TID, MPLS_MOVIE, IPL_APD,
  SETTING_COUNT
};

static const char *const setting_names[SETTING_COUNT] = {
    "IPL.CB",   "IPL.AR",   "IPL.ARN",  "IPL.CD",   "IPL.CD2",  "IPL.DH",   "IPL.E60", "IPL.EULA",
    "IPL.FRC",  "IPL.IDL",  "IPL.INC",  "IPL.LNG",  "IPL.NIK",  "IPL.PC",   "IPL.PGS", "IPL.SSV",
    "IPL.SADR", "IPL.SND",  "IPL.UPT",  "NET.CNF",  "NET.CTPC", "NET.PROF", "NET.WCPC", "NET.WCFG",
    "DEV.BTM",  "DEV.VIM",  "DEV.CTC",  "DEV.DSM",  "BT.DINF",  "BT.CDIF",  "BT.SENS", "BT.SPKV",
    "BT.MOT",   "BT.BAR",   "DVD.CNF",  "WWW.RST",  "IPL.TID",  "MPLS.MOVIE", "IPL.APD",
};

#define SYSCONF_END_MAGIC (WII_SYSCONF_SIZE - 4u)
#define SYSCONF_LOOKUP (SYSCONF_END_MAGIC - 2u * SETTING_COUNT)

/* The sizes of the arrays, each the size a console's own file has. */
#define IDL_SIZE 2u      /* stand-by connection, slot light */
#define NIK_SIZE 0x16u   /* ten UTF-16 characters and their count */
#define PC_SIZE 0x4au    /* parental controls */
#define SADR_SIZE 0x1008u /* the address book's "simple address" */
#define CDIF_SIZE 0x205u /* guest remotes: a count and six of 6 + 64 + 16 bytes */

typedef struct Item {
  uint8_t setting;
  uint8_t type;
  uint32_t length;
  const uint8_t *data;
} Item;

typedef struct Builder {
  Item items[SETTING_COUNT];
  unsigned count;
  /* Where the one- to eight-byte values live until they are written. */
  uint8_t scalars[SETTING_COUNT][8];
} Builder;

static void add_array(Builder *b, unsigned setting, const uint8_t *data, uint32_t length) {
  Item *item = &b->items[b->count++];
  item->setting = (uint8_t)setting;
  /* A small array's length byte holds up to 255, meaning 256 bytes. */
  item->type = (uint8_t)(length > 0x100u ? WII_SYSCONF_BIGARRAY : WII_SYSCONF_SMALLARRAY);
  item->length = length;
  item->data = data;
}

static void add_value(Builder *b, unsigned setting, unsigned type, uint64_t value) {
  Item *item = &b->items[b->count];
  uint8_t *bytes = b->scalars[b->count];
  unsigned i;
  b->count++;
  item->setting = (uint8_t)setting;
  item->type = (uint8_t)type;
  switch (type) {
    case WII_SYSCONF_SHORT: item->length = 2; break;
    case WII_SYSCONF_LONG: item->length = 4; break;
    case WII_SYSCONF_LONGLONG: item->length = 8; break;
    default: item->length = 1; break;
  }
  for (i = 0; i < item->length; i++) {
    bytes[i] = (uint8_t)(value >> (8u * (item->length - 1u - i)));
  }
  item->data = bytes;
}

/* The console's list of remotes.
 *
 *   +0x000  how many are registered
 *   +0x001  ten registered remotes
 *   +0x2bd  six that are connected now, which the system software fills in
 *           as it runs and nothing reads from a file
 *
 * and a remote is its Bluetooth address and sixty-four bytes of name.
 *
 * The address is stored most significant byte first - the order it is
 * printed in, 00:1e:4e:.., and the reverse of the order it has in every
 * packet of the Bluetooth host interface, which sends addresses least
 * significant byte first. The evidence: the SDK's remote library (WUD.c in
 * doldecomp/ogws) copies this field with memcpy to and from the Bluetooth
 * stack's own address type and compares the two with memcmp, and that
 * stack (Broadcom's, Apache-licensed, bt_types.h beside it) fills its type
 * from a packet through STREAM_TO_BDADDR, which reverses the six bytes. So
 * a stack that is handed an address by the controller holds it reversed,
 * and the file holds what the stack holds. wii_remote_address() gives the
 * bytes in that same most-significant-first order, so they go in as they
 * come; the Bluetooth controller is the one that reverses them, when it
 * puts them in a packet.
 *
 * The SDK keeps more than a name in the sixty-four bytes (a link key at
 * +0x20 in the versions decompiled). They are left zero here: the SDK asks
 * the Bluetooth controller, not this file, for the keys it has stored. */
static void build_remotes(uint8_t out[WII_SYSCONF_REMOTES_SIZE]) {
  static const char name[] = WII_REMOTE_NAME;
  unsigned i;
  memset(out, 0, WII_SYSCONF_REMOTES_SIZE);
  out[0] = (uint8_t)WII_REMOTES;
  for (i = 0; i < WII_REMOTES; i++) {
    uint8_t *entry = out + 1u + i * WII_SYSCONF_REMOTE_ENTRY;
    wii_remote_address(i, entry);
    memcpy(entry + 6, name, sizeof(name) - 1u);
  }
}

int wii_sysconf_build(const WiiSettings *settings, uint8_t out[WII_SYSCONF_SIZE]) {
  /* static would be shared between machines; these are a few kilobytes of
   * stack, used once when a console's flash is formatted. */
  uint8_t idle[IDL_SIZE], nickname[NIK_SIZE], parental[PC_SIZE], address[SADR_SIZE];
  uint8_t remotes[WII_SYSCONF_REMOTES_SIZE], guests[CDIF_SIZE];
  Builder builder;
  Builder *b = &builder;
  uint32_t at;
  unsigned i;
  size_t length;

  if (!settings || !out || !settings_valid(settings)) return -1;
  memset(b, 0, sizeof(*b));

  /* Stand-by: WiiConnect24 off, the slot's light off. */
  memset(idle, 0, sizeof(idle));

  /* The name: ten characters of UTF-16, most significant byte first, and
   * how many of them are used. */
  memset(nickname, 0, sizeof(nickname));
  length = strlen(settings->nickname);
  for (i = 0; i < length; i++) nickname[i * 2u + 1u] = (uint8_t)settings->nickname[i];
  nickname[0x15] = (uint8_t)length;

  /* Parental controls: off, which is the first byte's top bit clear, and
   * nothing else is looked at then. */
  memset(parental, 0, sizeof(parental));

  /* The address book's entry: a country, and no region, town or names. */
  memset(address, 0, sizeof(address));
  address[0] = settings->country;

  build_remotes(remotes);
  /* No guest remotes. */
  memset(guests, 0, sizeof(guests));

  /* The items, in the order of the system's own table. Two names in that
   * table are left out - NET.CNF and NET.PROF, the network connection's
   * settings - because no public description gives their type or size, a
   * console that has never been on a network has none to give, and an item
   * of the wrong size is refused by the SDK exactly as a missing one is.
   * IPL.ARN, IPL.TID and IPL.APD (an auto-run mode, the SD menu's
   * temporary title and a later console's power-down timer) are left out
   * as settings of features this console does not have. */
  add_value(b, IPL_CB, WII_SYSCONF_LONG, settings->counter_bias);
  add_value(b, IPL_AR, WII_SYSCONF_BYTE, settings->widescreen ? 1u : 0u);
  add_value(b, IPL_CD, WII_SYSCONF_BOOL, 1);   /* the first set-up has been done */
  add_value(b, IPL_CD2, WII_SYSCONF_BOOL, 1);  /* and the second */
  add_value(b, IPL_DH, WII_SYSCONF_BYTE, (uint8_t)settings->display_offset);
  add_value(b, IPL_E60, WII_SYSCONF_BYTE, settings->sixty_hertz ? 1u : 0u);
  add_value(b, IPL_EULA, WII_SYSCONF_BOOL, 1); /* the agreement has been read */
  add_value(b, IPL_FRC, WII_SYSCONF_LONG, 0x28); /* channel slots free */
  add_array(b, IPL_IDL, idle, sizeof(idle));
  add_value(b, IPL_INC, WII_SYSCONF_LONG, 8);    /* and taken: forty-eight in all */
  add_value(b, IPL_LNG, WII_SYSCONF_BYTE, settings->language);
  add_array(b, IPL_NIK, nickname, sizeof(nickname));
  add_array(b, IPL_PC, parental, sizeof(parental));
  add_value(b, IPL_PGS, WII_SYSCONF_BYTE, settings->progressive ? 1u : 0u);
  add_value(b, IPL_SSV, WII_SYSCONF_BYTE, settings->screen_saver ? 1u : 0u);
  add_array(b, IPL_SADR, address, sizeof(address));
  add_value(b, IPL_SND, WII_SYSCONF_BYTE, settings->sound);
  add_value(b, IPL_UPT, WII_SYSCONF_BYTE, 2);    /* last updated over the network */
  add_value(b, NET_CTPC, WII_SYSCONF_LONG, 0);   /* nothing on the network is restricted */
  add_value(b, NET_WCPC, WII_SYSCONF_BOOL, 0);
  add_value(b, NET_WCFG, WII_SYSCONF_LONG, 0);   /* WiiConnect24 is off */
  add_value(b, DEV_BTM, WII_SYSCONF_BYTE, 0);    /* a production console */
  add_value(b, DEV_VIM, WII_SYSCONF_BYTE, settings->video);
  add_value(b, DEV_CTC, WII_SYSCONF_BYTE,
            settings->region == WII_REGION_KOR ? 6u : settings->region);
  add_value(b, DEV_DSM, WII_SYSCONF_BYTE, 0);
  add_array(b, BT_DINF, remotes, sizeof(remotes));
  add_array(b, BT_CDIF, guests, sizeof(guests));
  add_value(b, BT_SENS, WII_SYSCONF_LONG, settings->sensitivity);
  add_value(b, BT_SPKV, WII_SYSCONF_BYTE, settings->speaker_volume);
  add_value(b, BT_MOT, WII_SYSCONF_BYTE, settings->rumble ? 1u : 0u);
  add_value(b, BT_BAR, WII_SYSCONF_BYTE, settings->sensor_bar_top ? 1u : 0u);
  add_value(b, DVD_CNF, WII_SYSCONF_BYTE, 0);
  add_value(b, WWW_RST, WII_SYSCONF_BYTE, 0);    /* the Internet Channel is not restricted */
  add_value(b, MPLS_MOVIE, WII_SYSCONF_BYTE, 1); /* the MotionPlus film has been watched */

  memset(out, 0, WII_SYSCONF_SIZE);
  memcpy(out, "SCv0", 4);
  wii_put16(out + 4, (uint16_t)b->count);
  at = 6u + 2u * (b->count + 1u);
  for (i = 0; i < b->count; i++) {
    const Item *item = &b->items[i];
    const char *name = setting_names[item->setting];
    const size_t name_length = strlen(name);
    const uint32_t extra = item->type == WII_SYSCONF_BIGARRAY ? 2u : item->type == WII_SYSCONF_SMALLARRAY ? 1u : 0u;
    if (at + 1u + name_length + extra + item->length > SYSCONF_LOOKUP) return -1;
    wii_put16(out + 6u + 2u * i, (uint16_t)at);
    out[at++] = (uint8_t)((item->type << 5) | (name_length - 1u));
    memcpy(out + at, name, name_length);
    at += (uint32_t)name_length;
    if (item->type == WII_SYSCONF_BIGARRAY) {
      wii_put16(out + at, (uint16_t)(item->length - 1u));
      at += 2u;
    } else if (item->type == WII_SYSCONF_SMALLARRAY) {
      out[at++] = (uint8_t)(item->length - 1u);
    }
    memcpy(out + at, item->data, item->length);
    at += item->length;
    /* The table at the end: this setting's entry, counted back from the
     * last, says where its offset is. */
    wii_put16(out + SYSCONF_END_MAGIC - 2u - 2u * item->setting, (uint16_t)(6u + 2u * i));
  }
  wii_put16(out + 6u + 2u * b->count, (uint16_t)at);
  memcpy(out + SYSCONF_END_MAGIC, "SCed", 4);
  return 0;
}

const uint8_t *wii_sysconf_find(const uint8_t *file, size_t size, const char *name, unsigned *type,
                                uint32_t *length) {
  const size_t name_length = name ? strlen(name) : 0;
  unsigned count, i;
  if (!file || size < 8u || !name_length || memcmp(file, "SCv0", 4) != 0) return NULL;
  count = wii_be16(file + 4);
  if (6u + 2u * ((size_t)count + 1u) > size) return NULL;
  for (i = 0; i < count; i++) {
    const size_t at = wii_be16(file + 6u + 2u * i);
    size_t value;
    unsigned kind;
    uint32_t bytes;
    if (at + 1u > size) return NULL;
    kind = file[at] >> 5;
    value = at + 1u + (file[at] & 0x1fu) + 1u;
    if (value > size) return NULL;
    if ((file[at] & 0x1fu) + 1u != name_length || memcmp(file + at + 1u, name, name_length) != 0) continue;
    switch (kind) {
      case WII_SYSCONF_BIGARRAY:
        if (value + 2u > size) return NULL;
        bytes = (uint32_t)wii_be16(file + value) + 1u;
        value += 2u;
        break;
      case WII_SYSCONF_SMALLARRAY:
        if (value + 1u > size) return NULL;
        bytes = (uint32_t)file[value] + 1u;
        value += 1u;
        break;
      case WII_SYSCONF_BYTE:
      case WII_SYSCONF_BOOL: bytes = 1; break;
      case WII_SYSCONF_SHORT: bytes = 2; break;
      case WII_SYSCONF_LONG: bytes = 4; break;
      case WII_SYSCONF_LONGLONG: bytes = 8; break;
      default: return NULL;
    }
    if (value + bytes > size) return NULL;
    if (type) *type = kind;
    if (length) *length = bytes;
    return file + value;
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* setting.txt                                                         */

void wii_setting_txt_crypt(uint8_t *bytes, size_t length, size_t offset) {
  uint32_t key = 0x73b5dbfau;
  size_t i;
  /* The key has thirty-two positions and comes round again. */
  for (i = 0; i < (offset & 31u); i++) key = (key << 1) | (key >> 31);
  for (i = 0; i < length; i++) {
    bytes[i] ^= (uint8_t)key;
    key = (key << 1) | (key >> 31);
  }
}

/* What the factory wrote, by region: the area, the model number printed
 * under the console, the letters its serial number starts with, and the
 * region a game checks its disc against. */
static const struct {
  const char *area;
  const char *model;
  const char *code;
  const char *game;
} regions[] = {
    {"JPN", "RVL-001(JPN)", "LJ", "JP"},
    {"USA", "RVL-001(USA)", "LU", "US"},
    {"EUR", "RVL-001(EUR)", "LEH", "EU"},
    {"KOR", "RVL-001(KOR)", "LKM", "KR"},
};

/* A serial number no console has: the factory never numbered one zero. */
#define WII_SERIAL_NUMBER "000000000"

int wii_setting_txt_build(const WiiSettings *settings, uint8_t out[WII_SETTING_TXT_SIZE]) {
  static const char *const video[] = {"NTSC", "PAL", "MPAL"};
  char text[WII_SETTING_TXT_SIZE + 64u];
  int length;
  size_t i;

  if (!settings || !out || !settings_valid(settings)) return -1;
  /* The lines and their order are a console's own. DVD is the region of a
   * film player that was never sold, and MPCH the wireless channels the
   * console may use to talk to a DS. */
  length = snprintf(text, sizeof(text),
                    "AREA=%s\r\nMODEL=%s\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=%s\r\nSERNO=%s\r\nVIDEO=%s\r\nGAME=%s\r\n",
                    regions[settings->region].area, regions[settings->region].model,
                    regions[settings->region].code, WII_SERIAL_NUMBER, video[settings->video],
                    regions[settings->region].game);
  if (length <= 0 || (size_t)length >= WII_SETTING_TXT_SIZE) return -1;

  memset(out, 0, WII_SETTING_TXT_SIZE);
  memcpy(out, text, (size_t)length);
  wii_setting_txt_crypt(out, (size_t)length, 0);
  /* See the note at the top: a character stored as zero would be read by
   * a game as the end of the text. */
  for (i = 0; i < (size_t)length; i++) {
    if (!out[i]) return -1;
  }
  return 0;
}
