/*
 * A Wii Remote read the way a program reads one: through a whole Bluetooth
 * stack (libogc's lwbt) and the library on top of it (wiiuse and WPAD),
 * talking to the console's Bluetooth module through IOS. Nothing here knows
 * how any of that works. It waits for the first remote, asks for buttons,
 * accelerometer and pointer, and says whether what arrives is what the
 * runner put on the remote (gc_run --pad-test, or --pad-test=classic for
 * the other extension; the figures are in runtimes/runtime-gc/tools/gc_run.c).
 *
 * So a pass covers the lot: the module's start-up, the remote's connection
 * and its two channels, the handshake that reads its calibration out of
 * its memory, the data reports and their bit layouts, the camera's dots
 * and the extension - against code written by other people for real
 * remotes.
 *
 * wii-libs: wiiuse bte
 */
#include <gccore.h>
#include <math.h>
#include <ogc/conf.h>
#include <stdlib.h>
#include <string.h>
#include <wiiuse/wpad.h>

#include "gc_test_report.h"

static char line[256];

static int near(float got, float want, float within) { return fabsf(got - want) <= within; }

/* Let the library take what has arrived, for a number of fields. */
static void scan(int fields) {
  int i;
  for (i = 0; i < fields; i++) {
    VIDEO_WaitVSync();
    WPAD_ScanPads();
  }
}

int main(void) {
  WPADData *data;
  u32 type = 0, held;
  int i, connected = 0;
  s32 result;

  VIDEO_Init();

  gc_test_begin("the first remote connects");
  result = WPAD_Init();
  snprintf(line, sizeof(line), "WPAD_Init returns %d", (int)result);
  GC_CHECK(result == WPAD_ERR_NONE, line);
  /* Buttons, accelerometer and pointer; the pointer on a 640 by 480
   * picture. Asked for before any remote is there, as programs do. */
  WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
  WPAD_SetVRes(WPAD_CHAN_0, 640, 480);
  for (i = 0; i < 600 && !connected; i++) {
    VIDEO_WaitVSync();
    WPAD_ScanPads();
    connected = WPAD_Probe(WPAD_CHAN_0, &type) == WPAD_ERR_NONE;
  }
  snprintf(line, sizeof(line), "remote 0 is there after %d fields (probe %d)", i,
           (int)WPAD_Probe(WPAD_CHAN_0, &type));
  GC_CHECK(connected, line);
  GC_CHECK(WPAD_Probe(WPAD_CHAN_1, NULL) != WPAD_ERR_NONE, "and remote 1, which is switched off, is not");
  gc_test_end();

  /* The library now has the remote's calibration to read, its extension
   * to identify and its camera to set up, and says the pointer is valid
   * when all of that is done. */
  data = WPAD_Data(WPAD_CHAN_0);
  for (i = 0; i < 480 && !(data->ir.valid && data->exp.type != WPAD_EXP_NONE); i++) scan(1);
  scan(30);
  held = WPAD_ButtonsHeld(WPAD_CHAN_0);

  gc_test_begin("buttons");
  snprintf(line, sizeof(line), "A, B, + and left are held and nothing else is (%08x)", (unsigned)held);
  GC_CHECK((held & 0xffffu) == (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_PLUS | WPAD_BUTTON_LEFT), line);
  gc_test_end();

  /* The remote reads 0.5 g to its left, 0.3 g towards its front and 0.812 g
   * out of its face: rolled about 32 degrees. Its Y and Z are a bit
   * coarser than its X, and the tolerance is two of their steps. */
  gc_test_begin("accelerometer");
  snprintf(line, sizeof(line), "gravity is (%d, %d, %d) thousandths of a g; (500, -300, 812) was set",
           (int)(data->gforce.x * 1000), (int)(data->gforce.y * 1000), (int)(data->gforce.z * 1000));
  GC_CHECK(near(data->gforce.x, 0.500f, 0.02f) && near(data->gforce.y, -0.300f, 0.04f) &&
               near(data->gforce.z, 0.812f, 0.04f),
           line);
  snprintf(line, sizeof(line), "roll is %d degrees; about 32 was set", (int)data->orient.roll);
  GC_CHECK(near(data->orient.roll, 31.6f, 3.0f), line);
  snprintf(line, sizeof(line), "the battery reads %d", (int)data->battery_level);
  GC_CHECK(data->battery_level > 100, line);
  gc_test_end();

  /* Three tenths of the way across and seven tenths of the way down. The
   * library works that out from two dots and the roll above, so this is
   * the camera, its format, the geometry and the roll correction at once.
   * Its arithmetic is not quite a straight line: aimed in turn at four
   * points of the picture with the remote level, it read back each to
   * within 11 pixels across and 6 down (the middle as (310, 238)), and
   * this point, rolled or not, to within 9 and 3. A roll taken the wrong
   * way round would be out by a hundred. */
  gc_test_begin("pointer");
  snprintf(line, sizeof(line), "the pointer is valid, with %d dots: (%d, %d) and (%d, %d)", data->ir.num_dots,
           data->ir.dot[0].rx, data->ir.dot[0].ry, data->ir.dot[1].rx, data->ir.dot[1].ry);
  GC_CHECK(data->ir.valid && data->ir.num_dots == 2, line);
  snprintf(line, sizeof(line), "it is at (%d, %d) of 640 by 480; (192, 336) was set", (int)data->ir.x,
           (int)data->ir.y);
  GC_CHECK(near(data->ir.x, 192.0f, 12.0f) && near(data->ir.y, 336.0f, 12.0f), line);
  gc_test_end();

  gc_test_begin("extension");
  if (data->exp.type == WPAD_EXP_NUNCHUK) {
    const struct nunchuk_t *n = &data->exp.nunchuk;
    /* Stick half left and three quarters up, Z held, tipped. */
    GC_CHECK((held >> 16) == (WPAD_NUNCHUK_BUTTON_Z >> 16), "a Nunchuk with Z held and C not");
    snprintf(line, sizeof(line), "its stick reads (%d, %d); (80, 201) was set", n->js.pos.x, n->js.pos.y);
    GC_CHECK(n->js.pos.x == 80 && n->js.pos.y == 201, line);
    snprintf(line, sizeof(line), "its stick is %d thousandths out at %d degrees", (int)(n->js.mag * 1000),
             (int)n->js.ang);
    GC_CHECK(near(n->js.mag, 0.90f, 0.05f) && near(n->js.ang, -33.7f, 2.0f), line);
    snprintf(line, sizeof(line), "its gravity is (%d, %d, %d) thousandths of a g; (-250, 400, 880) was set",
             (int)(n->gforce.x * 1000), (int)(n->gforce.y * 1000), (int)(n->gforce.z * 1000));
    GC_CHECK(near(n->gforce.x, -0.250f, 0.01f) && near(n->gforce.y, 0.400f, 0.01f) &&
                 near(n->gforce.z, 0.880f, 0.01f),
             line);
  } else if (data->exp.type == WPAD_EXP_CLASSIC) {
    const struct classic_ctrl_t *c = &data->exp.classic;
    const u32 want = WPAD_CLASSIC_BUTTON_A | WPAD_CLASSIC_BUTTON_X | WPAD_CLASSIC_BUTTON_ZL |
                     WPAD_CLASSIC_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_UP | WPAD_CLASSIC_BUTTON_FULL_R;
    snprintf(line, sizeof(line), "a Classic Controller with A, X, ZL, -, up and R held (%08x)", (unsigned)held);
    GC_CHECK((held & 0xffff0000u) == want, line);
    /* Left stick right and a little down, right stick left and a little
     * up: 6 bits and 5 bits, about their middles of 32 and 16. */
    snprintf(line, sizeof(line), "its sticks read (%d, %d) and (%d, %d); (52, 22) and (7, 19) were set",
             c->ljs.pos.x, c->ljs.pos.y, c->rjs.pos.x, c->rjs.pos.y);
    GC_CHECK(c->ljs.pos.x == 52 && c->ljs.pos.y == 22 && c->rjs.pos.x == 7 && c->rjs.pos.y == 19, line);
    snprintf(line, sizeof(line), "its triggers read %d and %d thousandths; 25/31 and 10/31 were set",
             (int)(c->l_shoulder * 1000), (int)(c->r_shoulder * 1000));
    GC_CHECK(near(c->l_shoulder, 25.0f / 31.0f, 0.02f) && near(c->r_shoulder, 10.0f / 31.0f, 0.02f), line);
    snprintf(line, sizeof(line), "its left stick is %d thousandths out at %d degrees", (int)(c->ljs.mag * 1000),
             (int)c->ljs.ang);
    GC_CHECK(c->ljs.mag > 0.7f && c->ljs.mag <= 1.0f && c->ljs.ang > 90.0f && c->ljs.ang < 135.0f, line);
  } else {
    snprintf(line, sizeof(line), "an extension is plugged in (type %d)", (int)data->exp.type);
    GC_CHECK(0, line);
  }
  gc_test_end();

  /* What the library read, for whoever reads the log. */
  OSReport("pad: held %08x extension %d\n", (unsigned)held, (int)data->exp.type);
  OSReport("pad: gravity %d %d %d roll %d pitch %d\n", (int)(data->gforce.x * 1000), (int)(data->gforce.y * 1000),
           (int)(data->gforce.z * 1000), (int)data->orient.roll, (int)data->orient.pitch);
  OSReport("pad: pointer %d %d valid %d angle %d\n", (int)data->ir.x, (int)data->ir.y, data->ir.valid,
           (int)data->ir.angle);
  OSReport("pad: dots %d %d and %d %d\n", data->ir.dot[0].rx, data->ir.dot[0].ry, data->ir.dot[1].rx,
           data->ir.dot[1].ry);

  gc_test_finish();
  return 0;
}
