/*
 * The Wii Remotes, as whoever supplies their input sees them.
 *
 * A frontend (or a script, or a test) says what each remote's buttons,
 * accelerometer, pointer and extension are doing. The remote's own side of
 * things - the Bluetooth link a game's stack talks to, the reports, the
 * calibration memory, the camera that turns a pointer into the two dots of a
 * sensor bar - is the machine's (wii_ios_bt.c, wii_remote.c) and is derived
 * from this, the same way a GameCube pad's wire protocol is derived from
 * GcPad.
 *
 * The input is part of the machine's state: it is set between fields, as a
 * pad is, and a run that sets the same input at the same fields is the same
 * run.
 */
#ifndef WII_REMOTE_H
#define WII_REMOTE_H

#include <stdint.h>

#include "gc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WII_REMOTES 4u

/* The buttons, as bits of the remote's own report (the core buttons word,
 * first byte in the high half). */
enum {
  WII_REMOTE_LEFT = 0x0100,
  WII_REMOTE_RIGHT = 0x0200,
  WII_REMOTE_DOWN = 0x0400,
  WII_REMOTE_UP = 0x0800,
  WII_REMOTE_PLUS = 0x1000,
  WII_REMOTE_TWO = 0x0001,
  WII_REMOTE_ONE = 0x0002,
  WII_REMOTE_B = 0x0004,
  WII_REMOTE_A = 0x0008,
  WII_REMOTE_MINUS = 0x0010,
  WII_REMOTE_HOME = 0x0080
};

/* What is plugged into the remote's expansion port. */
enum { WII_EXTENSION_NONE = 0, WII_EXTENSION_NUNCHUK = 1, WII_EXTENSION_CLASSIC = 2 };

enum { WII_NUNCHUK_Z = 0x01, WII_NUNCHUK_C = 0x02 };

/* The Classic Controller's buttons, as bits of its own report (first byte in
 * the high half), active high here whatever the wire does. */
enum {
  WII_CLASSIC_RIGHT = 0x8000,
  WII_CLASSIC_DOWN = 0x4000,
  WII_CLASSIC_L = 0x2000,
  WII_CLASSIC_MINUS = 0x1000,
  WII_CLASSIC_HOME = 0x0800,
  WII_CLASSIC_PLUS = 0x0400,
  WII_CLASSIC_R = 0x0200,
  WII_CLASSIC_ZL = 0x0080,
  WII_CLASSIC_B = 0x0040,
  WII_CLASSIC_Y = 0x0020,
  WII_CLASSIC_A = 0x0010,
  WII_CLASSIC_X = 0x0008,
  WII_CLASSIC_ZR = 0x0004,
  WII_CLASSIC_LEFT = 0x0002,
  WII_CLASSIC_UP = 0x0001
};

/* One remote's input. Everything is an integer so that a script can write
 * it and two runs cannot differ in the last bit of a float.
 *
 * Acceleration is in thousandths of a g, along the remote's own axes: x to
 * its left, y towards its expansion port, z out of its face. A remote lying
 * face up on a table reads (0, 0, 1000); pointed at the screen and held
 * level it reads the same.
 *
 * The pointer is where the remote is aimed on the screen, 0..65535 across
 * and down, and whether it is aimed at the screen at all. The machine works
 * out what the remote's camera would see of a sensor bar from that. */
typedef struct WiiRemoteInput {
  uint8_t present;   /* switched on and connected */
  uint8_t extension; /* WII_EXTENSION_* */
  uint16_t buttons;  /* WII_REMOTE_* */
  int16_t accel_x, accel_y, accel_z;
  uint8_t pointer_visible;
  uint8_t reserved0;
  uint16_t pointer_x, pointer_y;

  /* A Nunchuk: stick -128..127 (right and up positive), acceleration as
   * above along the Nunchuk's own axes, buttons WII_NUNCHUK_*. */
  int8_t nunchuk_stick_x, nunchuk_stick_y;
  uint8_t nunchuk_buttons;
  uint8_t reserved1;
  int16_t nunchuk_accel_x, nunchuk_accel_y, nunchuk_accel_z;

  /* A Classic Controller: sticks -128..127, triggers 0..255, buttons
   * WII_CLASSIC_*. */
  int8_t classic_left_x, classic_left_y, classic_right_x, classic_right_y;
  uint8_t classic_trigger_l, classic_trigger_r;
  uint16_t classic_buttons;
} WiiRemoteInput;

/* A remote at rest: present or not, lying flat, aimed away, nothing held
 * and nothing plugged in. */
void wii_remote_rest(WiiRemoteInput *input, int present);

/* Set and read a remote's input. Nothing happens on a GameCube, or for an
 * index of four or more. */
void wii_remote_set(GcMachine *m, unsigned index, const WiiRemoteInput *input);
const WiiRemoteInput *wii_remote_get(const GcMachine *m, unsigned index);

/* What a game last asked of a remote, for a frontend to show or feel: the
 * four player lights (bit 0 the first) and whether it is rumbling. */
unsigned wii_remote_leds(const GcMachine *m, unsigned index);
int wii_remote_rumble(const GcMachine *m, unsigned index);

/* A remote's Bluetooth address: the same six bytes in the console's list of
 * registered remotes (SYSCONF's BT.DINF) and in the link, which is how a
 * game knows a remote that connects is one of its own. Not a real
 * manufacturer's range. `out` is most significant byte first. */
static inline void wii_remote_address(unsigned index, uint8_t out[6]) {
  out[0] = 0x00;
  out[1] = 0x1e;
  out[2] = 0x4e;
  out[3] = 0x43;
  out[4] = 0x57;
  out[5] = (uint8_t)(0x10u + index);
}
/* And the console's own. */
static inline void wii_console_bluetooth_address(uint8_t out[6]) {
  out[0] = 0x00;
  out[1] = 0x1e;
  out[2] = 0x4e;
  out[3] = 0x43;
  out[4] = 0x57;
  out[5] = 0x01;
}
/* The name a remote gives. */
#define WII_REMOTE_NAME "Nintendo RVL-CNT-01"

#ifdef __cplusplus
}
#endif

#endif /* WII_REMOTE_H */
