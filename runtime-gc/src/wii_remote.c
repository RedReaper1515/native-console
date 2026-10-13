/*
 * A Wii Remote, as a game's Bluetooth stack sees one once its two HID
 * channels are open (wii_bt.h, wii_remote.h).
 *
 * The remote is a HID device whose descriptor says only how long each
 * report is. What the bytes mean was worked out by the people who wrote
 * the references here, and it is from their descriptions that this is
 * written:
 *
 *   xwiimote's doc/PROTOCOL (MIT-style): the reports, their flags, the
 *       error codes of a read
 *   WiiBrew's Wiimote pages: the memory map, the calibration blocks, the
 *       register spaces, the camera's three formats, the extensions' six
 *       bytes and how they are obscured
 *
 * The console sends "output reports" 0x10-0x1a; the remote sends "input
 * reports": 0x20 its status, 0x21 memory it was asked to read, 0x22 an
 * acknowledgement, and 0x30-0x3f its buttons, accelerometer, camera and
 * extension in whichever combination was asked for. Every output report's
 * first byte carries the rumble motor in bit 0 and "acknowledge this" in
 * bit 1.
 *
 * What is made up, because every remote's is its own: the calibration. The
 * accelerometer here reads 512 at rest and 104 more for each g, on all
 * three axes, and the memory says exactly that - so a game that calibrates
 * (they all do) gets back the thousandths of a g the input holds. The
 * Nunchuk's reads 512 and 200 a g; sticks rest at the middle of their
 * range and reach the fraction of it a real stick's gate allows.
 *
 * The camera. A remote does not know where it is pointed: it has a camera
 * that reports up to four bright dots in a 1024 by 768 field, and a game
 * works the pointer out from the two a sensor bar makes. Going the other
 * way needs a room, and this is the room assumed:
 *
 *   - The camera sees 33 degrees across and 23 up and down (WiiBrew's
 *     measurement), about 31 units of its field a degree.
 *   - The player is three metres from a picture 95 cm wide and 4:3, which
 *     therefore spans 560 by 420 units: the pointer's 0..65535 across the
 *     picture is 560 units of camera, and down it 420.
 *   - The sensor bar's two lights are 20 cm apart: 118 units, wherever
 *     the remote points.
 *   - The bar is below the picture, which is what the console's settings
 *     must then say (SYSCONF's BT.BAR = 0), and sits 107 units under the
 *     picture's middle. That figure is not the room's - a bar under a
 *     picture that size would be twice as far down - but software's:
 *     pointer arithmetic does not know the room either, and assumes a
 *     fixed offset for a bar below the screen. 107 is the one libogc
 *     assumes, found by aiming at a known point and reading the pointer
 *     back (tests/wii/programs/wii_padtest.c).
 *   - Aiming right moves the dots left in the camera's view; aiming up
 *     moves them down. The camera counts X from its left and Y from the
 *     bottom, so aiming at the top left of the picture puts the dots at
 *     high X and low Y.
 *   - The remote's roll is whatever its accelerometer says it is, and the
 *     dots turn about the middle of the view by that much - which is what
 *     a game undoes with the same accelerometer reading.
 *
 * The box and the offset are the ones libogc's pointer arithmetic assumes,
 * so a libogc program reads back the pointer it was given. A retail game's
 * arithmetic (its own idea of the box, and of where a bar below the screen
 * is) may put the same dots somewhere slightly different, which will show
 * as a pointer that is off by a constant; the constants are below.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "wii.h"

/* ------------------------------------------------------------------ */
/* Figures                                                             */

#define ACCEL_ZERO 512
#define ACCEL_PER_G 104
#define NUNCHUK_ZERO 512
#define NUNCHUK_PER_G 200
/* How far a stick travels either side of its middle, in the units each
 * reports in: the Nunchuk's 8 bits, the Classic Controller's left stick's
 * 6 and its right stick's 5. */
#define NUNCHUK_STICK_REACH 96
#define CLASSIC_LEFT_REACH 26
#define CLASSIC_RIGHT_REACH 13

#define CAMERA_WIDTH 1024
#define CAMERA_HEIGHT 768
#define CAMERA_SCREEN_WIDTH 560
#define CAMERA_SCREEN_HEIGHT 420
#define CAMERA_HALF_BAR 59
#define CAMERA_BAR_BELOW 107
/* How big a dot the camera says each light is, 0-15. */
#define CAMERA_DOT_SIZE 2

/* Fresh batteries. */
#define BATTERY_LEVEL 0xc8u

/* A read's error codes. */
enum { READ_OK = 0, READ_NOTHING_THERE = 7, READ_NO_SUCH_ADDRESS = 8 };

static WiiRemoteState *remote_state(const GcMachine *m, unsigned index) {
  WiiBluetooth *bt = wii_bluetooth(m);
  return bt && index < WII_REMOTES ? &bt->remote[index] : NULL;
}

unsigned wii_remote_leds(const GcMachine *m, unsigned index) {
  const WiiRemoteState *r = remote_state(m, index);
  return r ? r->leds : 0u;
}

int wii_remote_rumble(const GcMachine *m, unsigned index) {
  const WiiRemoteState *r = remote_state(m, index);
  return r ? r->rumble : 0;
}

/* ------------------------------------------------------------------ */
/* Memory as it leaves the factory                                     */

/* The byte that ends a calibration block: everything before it, plus
 * 0x55. */
static uint8_t checksum(const uint8_t *block, unsigned count) {
  unsigned sum = 0x55, i;
  for (i = 0; i < count; i++) sum += block[i];
  return (uint8_t)sum;
}

void wii_remote_factory(WiiRemoteState *r) {
  /* Where the camera saw four reference lights when the remote was made,
   * one in each quarter of its view. Games use them to correct the
   * camera's alignment; these describe a camera that needs none. */
  static const uint16_t reference[4][2] = {{160, 680}, {864, 680}, {864, 88}, {160, 88}};
  uint8_t *e = r->eeprom;
  uint8_t camera[11], accel[10];
  memset(e, 0, WII_REMOTE_EEPROM);

  camera[0] = (uint8_t)reference[0][0];
  camera[1] = (uint8_t)reference[0][1];
  camera[2] = (uint8_t)(((reference[0][1] >> 8) << 6) | ((reference[0][0] >> 8) << 4) |
                        ((reference[1][1] >> 8) << 2) | (reference[1][0] >> 8));
  camera[3] = (uint8_t)reference[1][0];
  camera[4] = (uint8_t)reference[1][1];
  camera[5] = (uint8_t)reference[2][0];
  camera[6] = (uint8_t)reference[2][1];
  camera[7] = (uint8_t)(((reference[2][1] >> 8) << 6) | ((reference[2][0] >> 8) << 4) |
                        ((reference[3][1] >> 8) << 2) | (reference[3][0] >> 8));
  camera[8] = (uint8_t)reference[3][0];
  camera[9] = (uint8_t)reference[3][1];
  camera[10] = checksum(camera, 10);
  memcpy(e + 0x00, camera, 11);
  memcpy(e + 0x0b, camera, 11);

  /* The accelerometer's reading at rest and under one g, each axis: the
   * top eight bits of each, then the low two of all three in one byte. */
  accel[0] = accel[1] = accel[2] = (uint8_t)(ACCEL_ZERO >> 2);
  accel[3] = (uint8_t)(((ACCEL_ZERO & 3) << 4) | ((ACCEL_ZERO & 3) << 2) | (ACCEL_ZERO & 3));
  accel[4] = accel[5] = accel[6] = (uint8_t)((ACCEL_ZERO + ACCEL_PER_G) >> 2);
  accel[7] = (uint8_t)((((ACCEL_ZERO + ACCEL_PER_G) & 3) << 4) | (((ACCEL_ZERO + ACCEL_PER_G) & 3) << 2) |
                       ((ACCEL_ZERO + ACCEL_PER_G) & 3));
  accel[8] = 0x40; /* speaker volume and motor, as every remote has it */
  accel[9] = checksum(accel, 9);
  memcpy(e + 0x16, accel, 10);
  memcpy(e + 0x20, accel, 10);
}

void wii_remote_power_on(WiiRemoteState *r) {
  /* Everything before the memory, which is the last member. */
  memset(r, 0, offsetof(WiiRemoteState, eeprom));
  r->mode = 0x30;
  r->reporting = 1;
}

/* ------------------------------------------------------------------ */
/* The extension                                                       */

/* An extension obscures what it sends with two tables made from sixteen key
 * bytes the console writes: wii_extension_cipher.c. Until a key is written
 * it has the tables of sixteen zeros, which are 0x97 throughout, and that
 * is all a homebrew library ever sets; a retail game's library writes a key
 * of its own. */

/* An extension's calibration block ends in two check bytes: the sum of
 * the fourteen before plus 0x55, and that plus 0x55 again. */
static void extension_checksums(uint8_t *block) {
  block[14] = checksum(block, 14);
  block[15] = (uint8_t)(block[14] + 0x55u);
}

/* Something has been plugged in, or pulled out: its register block as it
 * is before a game has touched it. */
static void extension_changed(WiiRemoteState *r) {
  uint8_t *reg = r->extension_reg;
  uint8_t calibration[16];
  memset(reg, 0, sizeof(r->extension_reg));
  r->cipher_on = 0;
  if (r->extension == WII_EXTENSION_NONE) return;

  memset(calibration, 0, sizeof(calibration));
  /* What it is: the last six bytes of the block. */
  reg[0xfc] = 0xa4;
  reg[0xfd] = 0x20;
  if (r->extension == WII_EXTENSION_NUNCHUK) {
    /* Accelerometer at rest and at one g, as the remote's own; then each
     * stick axis's highest, lowest and resting reading. */
    calibration[0] = calibration[1] = calibration[2] = (uint8_t)(NUNCHUK_ZERO >> 2);
    calibration[4] = calibration[5] = calibration[6] = (uint8_t)((NUNCHUK_ZERO + NUNCHUK_PER_G) >> 2);
    calibration[8] = calibration[11] = (uint8_t)(128 + NUNCHUK_STICK_REACH);
    calibration[9] = calibration[12] = (uint8_t)(128 - NUNCHUK_STICK_REACH);
    calibration[10] = calibration[13] = 128;
  } else {
    unsigned axis;
    reg[0xfe] = 0x01;
    reg[0xff] = 0x01;
    /* Highest, lowest and resting reading of each of the four stick
     * axes, all on an eight-bit scale whatever the axis reports in. */
    for (axis = 0; axis < 4u; axis++) {
      const int reach = axis < 2u ? CLASSIC_LEFT_REACH << 2 : CLASSIC_RIGHT_REACH << 3;
      calibration[axis * 3u + 0u] = (uint8_t)(128 + reach);
      calibration[axis * 3u + 1u] = (uint8_t)(128 - reach);
      calibration[axis * 3u + 2u] = 128;
    }
  }
  extension_checksums(calibration);
  memcpy(reg + 0x20, calibration, 16);
  memcpy(reg + 0x30, calibration, 16);

  /* An extension starts out obscuring what it sends, with the key it has
   * before any is written: all zeros. */
  reg[0xf0] = 0xaa;
  r->cipher_on = 1;
  (void)wii_extension_cipher_tables(reg + 0x40, r->cipher_add, r->cipher_xor);
}

static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }

/* a / b to the nearest whole number, b positive. */
static int divide(int a, int b) { return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b); }

/* An acceleration in thousandths of a g as a ten-bit reading. */
static int accel_reading(int milli_g, int zero, int per_g) {
  return clamp(zero + divide(milli_g * per_g, 1000), 0, 1023);
}

/* The six bytes an extension reports. */
static void extension_data(const WiiRemoteState *r, const WiiRemoteInput *in, uint8_t out[6]) {
  if (r->extension == WII_EXTENSION_NUNCHUK) {
    const int x = accel_reading(in->nunchuk_accel_x, NUNCHUK_ZERO, NUNCHUK_PER_G);
    const int y = accel_reading(in->nunchuk_accel_y, NUNCHUK_ZERO, NUNCHUK_PER_G);
    const int z = accel_reading(in->nunchuk_accel_z, NUNCHUK_ZERO, NUNCHUK_PER_G);
    out[0] = (uint8_t)(128 + divide(in->nunchuk_stick_x * NUNCHUK_STICK_REACH, 127));
    out[1] = (uint8_t)(128 + divide(in->nunchuk_stick_y * NUNCHUK_STICK_REACH, 127));
    out[2] = (uint8_t)(x >> 2);
    out[3] = (uint8_t)(y >> 2);
    out[4] = (uint8_t)(z >> 2);
    /* The low bits of the three, and the two buttons, which read 0 when
     * held. */
    out[5] = (uint8_t)(((z & 3) << 6) | ((y & 3) << 4) | ((x & 3) << 2) |
                       ((in->nunchuk_buttons & WII_NUNCHUK_C) ? 0 : 2) |
                       ((in->nunchuk_buttons & WII_NUNCHUK_Z) ? 0 : 1));
  } else if (r->extension == WII_EXTENSION_CLASSIC) {
    const unsigned lx = (unsigned)(32 + divide(in->classic_left_x * CLASSIC_LEFT_REACH, 127)) & 63u;
    const unsigned ly = (unsigned)(32 + divide(in->classic_left_y * CLASSIC_LEFT_REACH, 127)) & 63u;
    const unsigned rx = (unsigned)(16 + divide(in->classic_right_x * CLASSIC_RIGHT_REACH, 127)) & 31u;
    const unsigned ry = (unsigned)(16 + divide(in->classic_right_y * CLASSIC_RIGHT_REACH, 127)) & 31u;
    const unsigned lt = in->classic_trigger_l >> 3, rt = in->classic_trigger_r >> 3;
    /* Buttons read 0 when held, and one bit is always 1. */
    const unsigned buttons = (~(unsigned)in->classic_buttons & 0xfeffu) | 0x0100u;
    out[0] = (uint8_t)(((rx >> 3) << 6) | lx);
    out[1] = (uint8_t)((((rx >> 1) & 3u) << 6) | ly);
    out[2] = (uint8_t)(((rx & 1u) << 7) | ((lt >> 3) << 5) | ry);
    out[3] = (uint8_t)(((lt & 7u) << 5) | rt);
    out[4] = (uint8_t)(buttons >> 8);
    out[5] = (uint8_t)buttons;
  } else {
    memset(out, 0xff, 6);
  }
}

/* One byte of the extension's register block as it is sent: what it
 * holds, obscured if the cipher is on. The six bytes of controls are at
 * the start, and again from 8. */
static uint8_t extension_byte(const WiiRemoteState *r, const WiiRemoteInput *in, unsigned address) {
  uint8_t live[6];
  uint8_t value;
  address &= 0xffu;
  if (address < 6u || (address >= 8u && address < 14u)) {
    extension_data(r, in, live);
    value = live[address < 6u ? address : address - 8u];
  } else {
    value = r->extension_reg[address];
  }
  if (r->cipher_on) value = (uint8_t)((uint8_t)(value - r->cipher_add[address & 7u]) ^ r->cipher_xor[address & 7u]);
  return value;
}

/* The bytes of extension a data report carries: the register block from
 * its start, of which only the first six are anything. */
static void extension_stream(const WiiRemoteState *r, const WiiRemoteInput *in, uint8_t *out, unsigned count) {
  uint8_t live[6];
  unsigned i;
  if (r->extension == WII_EXTENSION_NONE) {
    memset(out, 0xff, count);
    return;
  }
  extension_data(r, in, live);
  for (i = 0; i < count; i++) {
    uint8_t value = i < 6u ? live[i] : 0u;
    if (r->cipher_on) value = (uint8_t)((uint8_t)(value - r->cipher_add[i & 7u]) ^ r->cipher_xor[i & 7u]);
    out[i] = value;
  }
}

/* ------------------------------------------------------------------ */
/* The camera                                                          */

static unsigned integer_root(uint32_t value) {
  uint32_t root = 0, bit = 1u << 30;
  while (bit > value) bit >>= 2;
  while (bit) {
    if (value >= root + bit) {
      value -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

/* The two dots a sensor bar makes for where the remote is aimed (the
 * geometry is at the top of this file). Returns a bit for each that is in
 * view. */
static unsigned camera_dots(const WiiRemoteInput *in, int x[2], int y[2]) {
  const int ax = in->accel_x, az = in->accel_z;
  const unsigned length = integer_root((uint32_t)(ax * ax) + (uint32_t)(az * az));
  int middle_x, middle_y;
  unsigned i, seen = 0;
  if (!in->pointer_visible) return 0;
  middle_x = CAMERA_WIDTH / 2 + CAMERA_SCREEN_WIDTH / 2 - divide((int)in->pointer_x * CAMERA_SCREEN_WIDTH, 65535);
  middle_y = CAMERA_HEIGHT / 2 - CAMERA_SCREEN_HEIGHT / 2 + divide((int)in->pointer_y * CAMERA_SCREEN_HEIGHT, 65535) -
             CAMERA_BAR_BELOW;
  x[0] = middle_x - CAMERA_HALF_BAR;
  x[1] = middle_x + CAMERA_HALF_BAR;
  y[0] = y[1] = middle_y;
  for (i = 0; i < 2u; i++) {
    /* Rolled clockwise as the player sees it, the remote's left side
     * rises and its accelerometer reads positive X; the view turns the
     * other way about its middle. A remote on end has no roll to speak
     * of. */
    if (length >= 100u) {
      const int dx = x[i] - CAMERA_WIDTH / 2, dy = y[i] - CAMERA_HEIGHT / 2;
      x[i] = CAMERA_WIDTH / 2 + divide(dx * az - dy * ax, (int)length);
      y[i] = CAMERA_HEIGHT / 2 + divide(dx * ax + dy * az, (int)length);
    }
    if (x[i] >= 0 && x[i] < CAMERA_WIDTH && y[i] >= 0 && y[i] < CAMERA_HEIGHT) seen |= 1u << i;
  }
  return seen;
}

/* Whether the camera is running: both of its switches on, and the
 * register that starts it written. */
static int camera_running(const WiiRemoteState *r) { return (r->camera & 3u) == 3u && r->camera_reg[0x30] == 0x08u; }

/* What the camera has to say, in the format it was put in: 10, 12 or 36
 * bytes for its four dots, of which a sensor bar is the first two and the
 * rest are empty. Returns how many bytes that is. */
static unsigned camera_data(const WiiRemoteState *r, const WiiRemoteInput *in, uint8_t out[36]) {
  int x[2] = {0, 0}, y[2] = {0, 0};
  unsigned seen, i;
  memset(out, 0xff, 36);
  if (!camera_running(r)) return 0;
  seen = camera_dots(in, x, y);
  switch (r->camera_reg[0x33]) {
    case 1: /* basic: two dots in five bytes, twice */
      if (seen & 1u) {
        out[0] = (uint8_t)x[0];
        out[1] = (uint8_t)y[0];
      }
      if (seen & 2u) {
        out[3] = (uint8_t)x[1];
        out[4] = (uint8_t)y[1];
      }
      out[2] = (uint8_t)(((seen & 1u) ? ((y[0] >> 8) << 6) | ((x[0] >> 8) << 4) : 0xf0) |
                         ((seen & 2u) ? ((y[1] >> 8) << 2) | (x[1] >> 8) : 0x0f));
      return 10;
    case 3: /* extended: three bytes a dot, with its size */
      for (i = 0; i < 2u; i++) {
        if (!(seen & (1u << i))) continue;
        out[i * 3u + 0u] = (uint8_t)x[i];
        out[i * 3u + 1u] = (uint8_t)y[i];
        out[i * 3u + 2u] = (uint8_t)(((y[i] >> 8) << 6) | ((x[i] >> 8) << 4) | CAMERA_DOT_SIZE);
      }
      return 12;
    case 5: /* full: nine bytes a dot, with the box around it and how bright */
      for (i = 0; i < 2u; i++) {
        uint8_t *dot = out + i * 9u;
        if (!(seen & (1u << i))) continue;
        dot[0] = (uint8_t)x[i];
        dot[1] = (uint8_t)y[i];
        dot[2] = (uint8_t)(((y[i] >> 8) << 6) | ((x[i] >> 8) << 4) | CAMERA_DOT_SIZE);
        /* The box is in the sensor's own 128 by 96. */
        dot[3] = (uint8_t)clamp((x[i] >> 3) - 1, 0, 127);
        dot[4] = (uint8_t)clamp((y[i] >> 3) - 1, 0, 127);
        dot[5] = (uint8_t)clamp((x[i] >> 3) + 1, 0, 127);
        dot[6] = (uint8_t)clamp((y[i] >> 3) + 1, 0, 127);
        dot[7] = 0;
        dot[8] = 0x40;
      }
      return 36;
    default: break;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Input reports                                                       */

static void put_buttons(const WiiRemoteInput *in, uint8_t *out) {
  out[0] = (uint8_t)((in->buttons >> 8) & 0x1fu);
  out[1] = (uint8_t)(in->buttons & 0x9fu);
}

static int valid_mode(unsigned mode) { return (mode >= 0x30u && mode <= 0x37u) || mode == 0x3du || mode == 0x3eu || mode == 0x3fu; }

/* The data report the remote would send now: its number, then its
 * contents. */
static unsigned data_report(const WiiRemoteState *r, const WiiRemoteInput *in, uint8_t *out) {
  /* What follows the buttons in each report: accelerometer or not, bytes
   * of camera, bytes of extension. */
  static const struct {
    uint8_t accel, camera, extension;
  } layout[8] = {{0, 0, 0}, {1, 0, 0}, {0, 0, 8}, {1, 12, 0}, {0, 0, 19}, {1, 0, 16}, {0, 10, 9}, {1, 10, 6}};
  const int ax = accel_reading(in->accel_x, ACCEL_ZERO, ACCEL_PER_G);
  const int ay = accel_reading(in->accel_y, ACCEL_ZERO, ACCEL_PER_G);
  const int az = accel_reading(in->accel_z, ACCEL_ZERO, ACCEL_PER_G);
  uint8_t camera[36];
  unsigned n = 0;

  if (r->mode == 0x3du) {
    out[n++] = 0x3d;
    extension_stream(r, in, out + n, 21);
    return n + 21u;
  }
  if (r->mode >= 0x3eu) {
    /* Interleaved: the camera's full format is too long for one report,
     * so two alternate. Each has half of it and one accelerometer axis,
     * and the third axis is spread over the spare button bits of both. */
    const unsigned second = r->interleave;
    const unsigned z = (unsigned)az >> 2;
    out[n++] = second ? 0x3f : 0x3e;
    put_buttons(in, out + n);
    out[n] |= (uint8_t)(((second ? z : z >> 4) & 3u) << 5);
    out[n + 1u] |= (uint8_t)(((second ? z >> 2 : z >> 6) & 3u) << 5);
    n += 2;
    out[n++] = (uint8_t)((second ? ay : ax) >> 2);
    (void)camera_data(r, in, camera);
    memcpy(out + n, camera + (second ? 18 : 0), 18);
    return n + 18u;
  }

  out[n++] = r->mode;
  put_buttons(in, out + n);
  if (layout[r->mode & 7u].accel) {
    /* Eight bits of each axis, and the low bits in what the buttons leave
     * free: two of X, one of Y and one of Z. */
    out[n] |= (uint8_t)((ax & 3) << 5);
    out[n + 1u] |= (uint8_t)((((ay >> 1) & 1) << 5) | (((az >> 1) & 1) << 6));
    out[n + 2u] = (uint8_t)(ax >> 2);
    out[n + 3u] = (uint8_t)(ay >> 2);
    out[n + 4u] = (uint8_t)(az >> 2);
    n += 3;
  }
  n += 2;
  if (layout[r->mode & 7u].camera) {
    /* The remote does not know the camera's format: it takes as many
     * bytes from the camera as the report has room for, whatever they
     * are. A game that sets a format the report does not fit gets the
     * start of it, as it would from the real thing. */
    const unsigned want = layout[r->mode & 7u].camera;
    (void)camera_data(r, in, camera);
    memcpy(out + n, camera, want);
    n += want;
  }
  if (layout[r->mode & 7u].extension) {
    extension_stream(r, in, out + n, layout[r->mode & 7u].extension);
    n += layout[r->mode & 7u].extension;
  }
  return n;
}

/* One step of a memory read: up to sixteen bytes, or why not. Returns the
 * report's length; `step` is how far the read moved and `done` whether
 * that was the end of it. */
static unsigned read_report(const WiiRemoteState *r, const WiiRemoteInput *in, uint8_t *out, unsigned *step,
                            int *done) {
  const uint32_t address = r->read_address;
  const unsigned count = r->read_left < 16u ? r->read_left : 16u;
  unsigned error = READ_OK, i;
  uint8_t *data = out + 6;
  memset(data, 0, 16);

  if (r->read_space == 1u) {
    /* The memory: 0x1700 bytes, at every multiple of 0x10000. */
    const uint32_t offset = address & 0xffffu;
    if (offset + count > WII_REMOTE_EEPROM) {
      error = READ_NO_SUCH_ADDRESS;
    } else {
      memcpy(data, r->eeprom + offset, count);
    }
  } else if (r->read_space == 2u) {
    /* A register space, chosen by the top byte less its lowest bit. */
    switch ((address >> 16) & 0xfeu) {
      case 0xa2:
        for (i = 0; i < count; i++) {
          const unsigned at = (address + i) & 0xffu;
          data[i] = at < sizeof(r->speaker_reg) ? r->speaker_reg[at] : 0u;
        }
        break;
      case 0xa4:
        if (r->extension == WII_EXTENSION_NONE) {
          error = READ_NOTHING_THERE;
        } else {
          for (i = 0; i < count; i++) data[i] = extension_byte(r, in, address + i);
        }
        break;
      case 0xa6:
        /* Where a MotionPlus answers. This remote has none, and says what
         * an empty expansion port says. */
        error = READ_NOTHING_THERE;
        break;
      case 0xb0:
        for (i = 0; i < count; i++) {
          const unsigned at = (address + i) & 0xffu;
          data[i] = at < sizeof(r->camera_reg) ? r->camera_reg[at] : 0u;
        }
        break;
      default: error = READ_NO_SUCH_ADDRESS; break;
    }
  } else {
    error = READ_NO_SUCH_ADDRESS;
  }

  out[0] = 0x21;
  put_buttons(in, out + 1);
  out[3] = (uint8_t)(((error ? 15u : count - 1u) << 4) | error);
  out[4] = (uint8_t)(address >> 8);
  out[5] = (uint8_t)address;
  *step = count;
  *done = error || count == r->read_left;
  return 22;
}

/* The next input report: an answer owed, a step of a read, or data - in
 * that order, because data reports stop while a read is answered. */
static unsigned next_report(const WiiRemoteState *r, const WiiRemoteInput *in, uint8_t *out, unsigned *step,
                            int *done) {
  *step = 0;
  *done = 0;
  if (r->reply_count) {
    const WiiRemoteReply *reply = &r->reply[0];
    out[0] = reply->kind;
    put_buttons(in, out + 1);
    if (reply->kind == 0x20u) {
      out[3] = (uint8_t)((r->leds << 4) | (r->extension != WII_EXTENSION_NONE ? 0x02u : 0u) |
                         ((r->speaker & 1u) ? 0x04u : 0u) | ((r->camera & 3u) == 3u ? 0x08u : 0u));
      out[4] = out[5] = 0;
      out[6] = BATTERY_LEVEL;
      return 7;
    }
    out[3] = reply->report;
    out[4] = reply->error;
    return 5;
  }
  if (r->read_space) return read_report(r, in, out, step, done);
  return data_report(r, in, out);
}

unsigned wii_remote_peek(GcMachine *m, unsigned index, uint8_t *out) {
  unsigned step;
  int done;
  return next_report(remote_state(m, index), &wii_state(m)->remote_input[index], out, &step, &done);
}

unsigned wii_remote_take(GcMachine *m, unsigned index, uint64_t when, uint8_t *out) {
  WiiRemoteState *r = remote_state(m, index);
  unsigned step, size;
  int done;
  size = next_report(r, &wii_state(m)->remote_input[index], out, &step, &done);
  r->next_report = when + WII_BT_REPORT_CYCLES;
  if (r->reply_count) {
    r->reply_count--;
    memmove(&r->reply[0], &r->reply[1], r->reply_count * sizeof(r->reply[0]));
  } else if (r->read_space) {
    r->read_address += step;
    r->read_left = (uint16_t)(r->read_left - step);
    if (done) r->read_space = 0;
  } else {
    memcpy(r->last_report, out, size);
    r->last_length = (uint8_t)size;
    if (r->mode >= 0x3eu) r->interleave ^= 1u;
  }
  return size;
}

int wii_remote_ready(GcMachine *m, unsigned index, uint64_t *when) {
  const WiiRemoteState *r = remote_state(m, index);
  uint8_t report[24];
  unsigned size;
  *when = r->next_report;
  if (r->reply_count || r->read_space) return 1;
  if (!r->reporting) return 0;
  if (r->continuous) return 1;
  /* Otherwise only when something has changed since the last one. */
  size = data_report(r, &wii_state(m)->remote_input[index], report);
  return size != r->last_length || memcmp(report, r->last_report, size) != 0;
}

/* ------------------------------------------------------------------ */
/* Output reports                                                      */

static void owe(GcMachine *m, WiiRemoteState *r, unsigned kind, unsigned report, unsigned error) {
  if (r->reply_count == WII_REMOTE_REPLIES) {
    /* The console is not reading what it asks for. */
    if (m->log) fprintf(m->log, "wii: a remote owes more than %u answers; the oldest is dropped\n", WII_REMOTE_REPLIES);
    r->reply_count--;
    memmove(&r->reply[0], &r->reply[1], r->reply_count * sizeof(r->reply[0]));
  }
  r->reply[r->reply_count].kind = (uint8_t)kind;
  r->reply[r->reply_count].report = (uint8_t)report;
  r->reply[r->reply_count].error = (uint8_t)error;
  r->reply_count++;
}

void wii_remote_poll(GcMachine *m, unsigned index) {
  WiiRemoteState *r = remote_state(m, index);
  uint8_t want = wii_state(m)->remote_input[index].extension;
  if (want > WII_EXTENSION_CLASSIC) want = WII_EXTENSION_NONE;
  if (want == r->extension) return;
  r->extension = want;
  extension_changed(r);
  /* The remote says so unasked, and stops its data reports until it is
   * told again which it should send. */
  owe(m, r, 0x20, 0, 0);
  r->reporting = 0;
}

/* Write to a register space. Returns the error for the acknowledgement. */
static unsigned write_registers(GcMachine *m, unsigned index, uint32_t address, const uint8_t *data, unsigned count) {
  WiiRemoteState *r = remote_state(m, index);
  unsigned i;
  switch ((address >> 16) & 0xfeu) {
    case 0xa2:
      for (i = 0; i < count; i++) {
        const unsigned at = (address + i) & 0xffu;
        if (at < sizeof(r->speaker_reg)) r->speaker_reg[at] = data[i];
      }
      return 0;
    case 0xb0:
      for (i = 0; i < count; i++) {
        const unsigned at = (address + i) & 0xffu;
        if (at < sizeof(r->camera_reg)) r->camera_reg[at] = data[i];
      }
      return 0;
    case 0xa4: {
      if (r->extension == WII_EXTENSION_NONE) return READ_NOTHING_THERE;
      for (i = 0; i < count; i++) {
        const unsigned at = (address + i) & 0xffu;
        /* What it is cannot be written over, except the Classic
         * Controller's choice of report format. */
        if (at >= 0xfau && at != 0xfeu) continue;
        r->extension_reg[at] = data[i];
      }
      /* 0x55 at 0xf0 turns the cipher off, 0xaa turns it on, and the
       * sixteen bytes from 0x40 are its key. */
      if (r->extension_reg[0xf0] == 0x55u) r->cipher_on = 0;
      if (r->extension_reg[0xf0] == 0xaau) r->cipher_on = 1;
      (void)wii_extension_cipher_tables(r->extension_reg + 0x40, r->cipher_add, r->cipher_xor);
      return 0;
    }
    case 0xa6: return READ_NOTHING_THERE; /* no MotionPlus */
    default: break;
  }
  return READ_NO_SUCH_ADDRESS;
}

void wii_remote_output(GcMachine *m, unsigned index, const uint8_t *report, unsigned size) {
  WiiRemoteState *r = remote_state(m, index);
  const uint8_t *p = report + 1;
  const unsigned id = report[0], length = size - 1u;
  unsigned error = 0;
  int acknowledge;
  if (size < 2u) return;

  /* Every report carries the motor's state and may ask to be
   * acknowledged. */
  r->rumble = p[0] & 1u;
  acknowledge = (p[0] & 2u) != 0;

  switch (id) {
    case 0x10: break; /* the motor alone */
    case 0x11: r->leds = p[0] >> 4; break;
    case 0x12: /* which data report, and whether to send it unchanged */
      if (length < 2u || !valid_mode(p[1])) {
        error = 3;
        acknowledge = 1;
        break;
      }
      r->mode = p[1];
      r->continuous = (p[0] & 4u) ? 1u : 0u;
      r->reporting = 1;
      r->interleave = 0;
      r->last_length = 0; /* the first one goes whatever it holds */
      break;
    case 0x13: r->camera = (uint8_t)((r->camera & ~1u) | ((p[0] & 4u) ? 1u : 0u)); break;
    case 0x1a: r->camera = (uint8_t)((r->camera & ~2u) | ((p[0] & 4u) ? 2u : 0u)); break;
    case 0x14: r->speaker = (uint8_t)((r->speaker & ~1u) | ((p[0] & 4u) ? 1u : 0u)); break;
    case 0x19: r->speaker = (uint8_t)((r->speaker & ~2u) | ((p[0] & 4u) ? 2u : 0u)); break;
    case 0x15: /* status, please: the status report is the answer */
      owe(m, r, 0x20, 0, 0);
      acknowledge = 0;
      break;
    case 0x16: { /* write memory: space, address, count, sixteen bytes */
      uint32_t address;
      unsigned count;
      acknowledge = 1;
      if (length < 5u) {
        error = 3;
        break;
      }
      address = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
      count = p[4];
      if (count > 16u || length < 5u + count) {
        error = 3;
      } else if ((p[0] & 0x0cu) == 0u) {
        const uint32_t offset = address & 0xffffu;
        if (offset + count > WII_REMOTE_EEPROM) {
          error = READ_NO_SUCH_ADDRESS;
        } else {
          memcpy(r->eeprom + offset, p + 5, count);
        }
      } else if ((p[0] & 0x0cu) == 0x0cu) {
        error = 4; /* both spaces at once is neither */
      } else {
        error = write_registers(m, index, address, p + 5, count);
      }
      break;
    }
    case 0x17: /* read memory: space, address, count; answered by 0x21 reports */
      if (length < 6u) {
        error = 3;
        acknowledge = 1;
        break;
      }
      r->read_address = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
      r->read_left = wii_be16(p + 4);
      r->read_space = (p[0] & 0x0cu) == 0u ? 1u : (p[0] & 0x0cu) == 0x0cu ? 3u : 2u;
      if (!r->read_left) r->read_space = 0;
      break;
    case 0x18: break; /* sound for the speaker: heard by nobody */
    default:
      if (m->log) fprintf(m->log, "wii: remote %u was sent output report %02x, which it does not have\n", index, id);
      return;
  }
  if (acknowledge) owe(m, r, 0x22, id, error);
}
