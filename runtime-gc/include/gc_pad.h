/*
 * The controllers, and the serial interface they answer on.
 *
 * Four ports, each of which may hold a standard controller. What a game
 * asks for and what it gets back is a short exchange of bytes: a command
 * out, a reply in, through a 128-byte buffer the interface shares between
 * all four channels.
 *
 * The frontend sets `GcPad` and this turns it into the bytes the hardware
 * would have returned. Nothing here knows about SDL, a keyboard or a
 * gamepad; that mapping belongs to whatever is driving the machine.
 */
#ifndef GC_PAD_H
#define GC_PAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcMachine GcMachine;

/* The button bits, in the order the hardware reports them - which is also
 * the order the SDK's own header lists, so a game's `PAD_BUTTON_A` and
 * this agree without a translation step. */
#define GC_PAD_LEFT 0x0001u
#define GC_PAD_RIGHT 0x0002u
#define GC_PAD_DOWN 0x0004u
#define GC_PAD_UP 0x0008u
#define GC_PAD_Z 0x0010u
#define GC_PAD_R 0x0020u
#define GC_PAD_L 0x0040u
#define GC_PAD_A 0x0100u
#define GC_PAD_B 0x0200u
#define GC_PAD_X 0x0400u
#define GC_PAD_Y 0x0800u
#define GC_PAD_START 0x1000u

/* One controller's state as a frontend sees it. The sticks are signed and
 * centred on zero here; the hardware reports them biased by 128, and the
 * conversion happens on the way out rather than being something a caller
 * has to remember. */
typedef struct GcPad {
  int present;
  uint16_t buttons;
  int8_t stick_x, stick_y;
  int8_t c_stick_x, c_stick_y;
  uint8_t trigger_l, trigger_r;
} GcPad;

#define GC_PAD_PORTS 4

/* Put a controller in a port, or take it out. Safe to call at any time;
 * the machine reads it when a game next asks. */
void gc_pad_set(GcMachine *m, unsigned port, const GcPad *pad);

#ifdef __cplusplus
}
#endif

#endif /* GC_PAD_H */
