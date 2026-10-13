/*
 * The controllers, from a game's point of view.
 *
 * Everything a game learns about a controller arrives through the serial
 * interface as a short exchange of bytes, and almost every way of getting
 * that wrong still looks like a working controller when nobody is
 * touching it. A stick reported without its bias reads as hard left from
 * the moment the game starts. A button word assembled in the wrong order
 * reads as a different button. A port that answers nothing at all reads as
 * a controller that is simply unplugged, which a game accepts quietly.
 *
 * So this runs twice: once against a controller at rest, where the
 * interesting question is whether it is *there* and whether the sticks
 * really are centred, and once - under `gc_run --pad-test` - against a
 * controller holding a deliberately awkward pattern that no plausible
 * mistake would reproduce. Which of the two is being checked comes from
 * the machine itself: the program reads the controller and decides.
 */

#include <malloc.h>
#include <string.h>

#include <gccore.h>

#include "gc_test_report.h"

static void test_present(void) {
  u32 connected;

  gc_test_begin("a controller in the first port");

  PAD_Init();
  connected = PAD_ScanPads();

  /* Exactly one, rather than a particular bit.
   *
   * The SDK's own header names the channels from the top - channel 0 is
   * 0x80000000 - but what this function returns is numbered from the
   * bottom, and nothing says so. Asserting the bit would be asserting a
   * reading of undocumented behaviour; asserting the *count* says what
   * this test is actually about, which is that one port answers and three
   * do not. */
  snprintf(gc_test_line, sizeof(gc_test_line), "something is plugged in: got %08lx",
           (unsigned long)connected);
  gc_test_check(connected != 0, gc_test_line);
  snprintf(gc_test_line, sizeof(gc_test_line), "and only one thing: got %08lx",
           (unsigned long)connected);
  gc_test_check((connected & (connected - 1u)) == 0, gc_test_line);
  gc_test_end();
}

/* At rest, or holding the pattern the machine was told to hold. Both are
 * checked here because the program cannot be told which to expect - it
 * reads the controller and the answer has to be one or the other, with
 * nothing in between that would pass. */
static void test_state(void) {
  s8 stick_x, stick_y, c_x, c_y;
  u8 trigger_l, trigger_r;
  u16 buttons;

  gc_test_begin("what the controller reports");

  PAD_ScanPads();
  buttons = PAD_ButtonsHeld(PAD_CHAN0);
  stick_x = PAD_StickX(PAD_CHAN0);
  stick_y = PAD_StickY(PAD_CHAN0);
  c_x = PAD_SubStickX(PAD_CHAN0);
  c_y = PAD_SubStickY(PAD_CHAN0);
  trigger_l = PAD_TriggerL(PAD_CHAN0);
  trigger_r = PAD_TriggerR(PAD_CHAN0);

  if (buttons == 0) {
    /* At rest. The sticks are the point: a bias applied the wrong way
     * round gives -128 here, which is a stick held hard over, and a game
     * would read it as the player doing that. */
    gc_test_check(stick_x == 0 && stick_y == 0, "at rest, the stick is centred");
    gc_test_check(c_x == 0 && c_y == 0, "and so is the second one");
    snprintf(gc_test_line, sizeof(gc_test_line), "and the triggers are up: got %u and %u",
             trigger_l, trigger_r);
    gc_test_check(trigger_l == 0 && trigger_r == 0, gc_test_line);
  } else {
    snprintf(gc_test_line, sizeof(gc_test_line), "A, Start and Left are held: got %04x", buttons);
    gc_test_check(buttons == (PAD_BUTTON_A | PAD_BUTTON_START | PAD_BUTTON_LEFT), gc_test_line);
    snprintf(gc_test_line, sizeof(gc_test_line), "the stick is at -100, 75: got %d, %d", stick_x,
             stick_y);
    gc_test_check(stick_x == -100 && stick_y == 75, gc_test_line);
    snprintf(gc_test_line, sizeof(gc_test_line), "the second stick is at 40, -30: got %d, %d", c_x,
             c_y);
    gc_test_check(c_x == 40 && c_y == -30, gc_test_line);
    snprintf(gc_test_line, sizeof(gc_test_line), "the triggers are at 200 and 100: got %u and %u",
             trigger_l, trigger_r);
    gc_test_check(trigger_l == 200 && trigger_r == 100, gc_test_line);
  }
  gc_test_end();
}

/* An empty port answers, and what it says is "there is nothing here". A
 * port that says nothing at all leaves a game waiting, which is a
 * different failure and a much quieter one. */
static void test_empty_ports(void) {
  unsigned port;

  gc_test_begin("the three empty ports");
  PAD_ScanPads();
  for (port = 1; port < 4u; port++) {
    /* Nothing held, no stick, no trigger - and, more to the point, the
     * question returned at all. */
    const u16 buttons = PAD_ButtonsHeld((s32)port);
    const s8 stick = PAD_StickX((s32)port);
    snprintf(gc_test_line, sizeof(gc_test_line), "port %u reports nothing: got %04x and %d", port,
             buttons, stick);
    gc_test_check(buttons == 0 && stick == 0, gc_test_line);
  }
  gc_test_end();
}

int main(void) {
  VIDEO_Init();
  test_present();
  test_state();
  test_empty_ports();
  gc_test_finish();
  return 0;
}
