/*
 * Drawing: the whole graphics pipeline, end to end, from a game's point of
 * view.
 *
 * dolphin-emu/hwtests' gxtest programs check the pipeline a piece at a
 * time and read their results back through a texture copy. None of them
 * ever copies a frame out for *display*, which is the path a real game
 * uses every sixtieth of a second - so none of them would notice if that
 * path were missing altogether.
 *
 * This one sets the machine up the way the devkitPro examples do, draws a
 * few flat-coloured quads through libogc's GX, copies the result to the
 * display buffer, and then checks both ends: the framebuffer through a
 * peek, and the display buffer through the luma and chroma it was
 * converted into. Running it under `gc_run --dump-frame` also produces a
 * picture, which is the only way to look at what the rasteriser is
 * actually doing.
 */

#include <malloc.h>
#include <string.h>

#include <gccore.h>

#include "gc_test_report.h"

#define FIFO_SIZE (256 * 1024)

static GXRModeObj *mode;
static void *framebuffer;
static void *fifo;

/* A quad at a position and colour, in the coordinates the orthographic
 * projection below uses: -1 to 1 across the screen, y upwards. */
static void quad(f32 left, f32 top, f32 right, f32 bottom, u8 r, u8 g, u8 b) {
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position3f32(left, top, 0.0f);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(right, top, 0.0f);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(right, bottom, 0.0f);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(left, bottom, 0.0f);
  GX_Color4u8(r, g, b, 255);
  GX_End();
}

static void setup(void) {
  GXColor background = {0, 0, 0, 255};
  Mtx44 projection;
  Mtx model;

  VIDEO_Init();
  mode = VIDEO_GetPreferredMode(NULL);
  framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
  VIDEO_Configure(mode);
  VIDEO_SetNextFramebuffer(framebuffer);
  VIDEO_SetBlack(FALSE);
  VIDEO_Flush();

  fifo = MEM_K0_TO_K1(memalign(32, FIFO_SIZE));
  memset(fifo, 0, FIFO_SIZE);
  GX_Init(fifo, FIFO_SIZE);

  GX_SetCopyClear(background, 0x00ffffff);
  GX_SetViewport(0, 0, mode->fbWidth, mode->efbHeight, 0, 1);
  GX_SetDispCopyYScale((f32)mode->xfbHeight / (f32)mode->efbHeight);
  GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
  GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
  GX_SetDispCopyDst(mode->fbWidth, mode->xfbHeight);
  GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
  GX_SetFieldMode(mode->field_rendering,
                  ((mode->viHeight == 2 * mode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
  GX_SetCullMode(GX_CULL_NONE);
  GX_SetDispCopyGamma(GX_GM_1_0);

  /* Flat on the screen: no perspective, no camera, one unit each way. */
  memset(projection, 0, sizeof(projection));
  projection[0][0] = 1.0f;
  projection[1][1] = 1.0f;
  projection[2][2] = -1.0f;
  projection[3][3] = 1.0f;
  GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
  guMtxIdentity(model);
  GX_LoadPosMtxImm(model, GX_PNMTX0);

  GX_ClearVtxDesc();
  GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
  GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GX_SetNumChans(1);
  GX_SetNumTexGens(0);
  GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
  GX_SetColorUpdate(GX_TRUE);

  /* A clearing copy before anything is drawn, which is what every example
   * and every game does: it is the only thing that puts the far value into
   * the depth buffer, and without it the first depth test rejects
   * everything. */
  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
}

/* Four bands across the top and a smaller square in the middle, so that a
 * picture of the result is obviously right or obviously wrong. */
static void draw(void) {
  GX_SetCopyClear((GXColor){0, 0, 0, 255}, 0x00ffffff);

  quad(-1.0f, 1.0f, -0.5f, 0.5f, 255, 0, 0);
  quad(-0.5f, 1.0f, 0.0f, 0.5f, 0, 255, 0);
  quad(0.0f, 1.0f, 0.5f, 0.5f, 0, 0, 255);
  quad(0.5f, 1.0f, 1.0f, 0.5f, 255, 255, 255);
  quad(-0.25f, 0.25f, 0.25f, -0.25f, 128, 64, 192);

  GX_DrawDone();
}

/* The framebuffer, read back a pixel at a time. The bands are each an
 * eighth of the screen wide and a quarter of it tall. */
static void test_framebuffer(void) {
  static const struct {
    u16 x, y;
    u8 r, g, b;
    const char *what;
  } points[] = {
      {80, 40, 255, 0, 0, "the red band"},
      {240, 40, 0, 255, 0, "the green band"},
      {400, 40, 0, 0, 255, "the blue band"},
      {560, 40, 255, 255, 255, "the white band"},
      {320, 264, 128, 64, 192, "the square in the middle"},
      {40, 400, 0, 0, 0, "the cleared background"},
  };
  unsigned i;

  gc_test_begin("what the rasteriser put in the framebuffer");
  for (i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
    GXColor got;
    GX_PeekARGB(points[i].x, points[i].y, &got);
    snprintf(gc_test_line, sizeof(gc_test_line), "%s: got %u,%u,%u, wanted %u,%u,%u",
             points[i].what, got.r, got.g, got.b, points[i].r, points[i].g, points[i].b);
    gc_test_check(got.r == points[i].r && got.g == points[i].g && got.b == points[i].b,
                  gc_test_line);
  }
  gc_test_end();
}

/* The display buffer, which is not red, green and blue at all: it is luma
 * and chroma, two pixels to a pair, in studio range. Black is luma 16 and
 * white is 235. */
static void test_display_buffer(void) {
  const u32 *xfb = (const u32 *)framebuffer;
  const u32 pitch = (u32)mode->fbWidth / 2u; /* words, two pixels each */
  unsigned i;
  static const struct {
    u16 x, y;
    u8 luma;
    const char *what;
  } points[] = {
      {80, 40, 81, "red is luma 81"},
      {240, 40, 145, "green is luma 145"},
      {400, 40, 41, "blue is luma 41"},
      {560, 40, 235, "white is luma 235"},
      {80, 400, 16, "black is luma 16"},
  };

  gc_test_begin("what the copy put in the display buffer");
  for (i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
    const u32 word = xfb[points[i].y * pitch + (points[i].x >> 1)];
    /* Luma is the first and third byte; which one depends on which of the
     * pair this pixel is. */
    const u8 luma = (points[i].x & 1u) ? (u8)(word >> 8) : (u8)(word >> 24);
    snprintf(gc_test_line, sizeof(gc_test_line), "%s: got %u", points[i].what, luma);
    /* One step either way: the conversion rounds, and the copy filter has
     * touched the row above and below. */
    gc_test_check(luma + 1u >= points[i].luma && luma <= points[i].luma + 1u, gc_test_line);
  }
  gc_test_end();
}

/* The chroma at a seam.
 *
 * Two pixels of the display buffer share one Cb and one Cr, and the rule
 * that reduces two chroma pairs to one cannot be seen anywhere except at a
 * colour boundary: inside a flat band every rule gives the same answer.
 * The quads leave a hard red-to-green edge at x = 160, so this reads the
 * three macropixels around it.
 *
 * The sample belongs to the pair's *even* pixel and reaches one pixel
 * either side of it, weighted one, two, one. So the first pair of the
 * green band carries a quarter of the red band's chroma, and the last pair
 * of the red band - whose right-hand tap is still red - carries none of
 * green's. Averaging the pair instead leaves both of them pure, and
 * averaging across the seam puts a half in the first and a half in the
 * last; either is caught here.
 *
 * Red converts to Cb 90, Cr 240 and green to Cb 54, Cr 34, so the seam
 * pair is (90 + 2*54 + 54)/4 = 63 and (240 + 2*34 + 34)/4 = 85. */
static void test_seam_chroma(void) {
  const u32 *xfb = (const u32 *)framebuffer;
  const u32 pitch = (u32)mode->fbWidth / 2u;
  unsigned i;
  static const struct {
    u16 x, y;
    u8 cb, cr;
    const char *what;
  } points[] = {
      {240, 40, 54, 34, "green well away from the seam"},
      {160, 40, 63, 85, "the first pair of green, a quarter of the way to red"},
      {158, 40, 90, 240, "the last pair of red, untouched by green"},
  };

  gc_test_begin("the chroma filter at a colour seam");
  for (i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
    const u32 word = xfb[points[i].y * pitch + (points[i].x >> 1)];
    const u8 cb = (u8)(word >> 16), cr = (u8)word;
    snprintf(gc_test_line, sizeof(gc_test_line), "%s: got Cb %u Cr %u", points[i].what, cb, cr);
    /* One step either way, because the conversion rounds. */
    gc_test_check(cb + 1u >= points[i].cb && cb <= points[i].cb + 1u && cr + 1u >= points[i].cr &&
                      cr <= points[i].cr + 1u,
                  gc_test_line);
  }
  gc_test_end();
}

/* Hand the finished buffer to the video interface and let it scan out.
 * A game does this every frame; this program does it because otherwise
 * nothing is ever *presented*, and a reference emulator asked to dump a
 * frame has nothing to dump - which is exactly what
 * tools/gc/capture_reference.py found. */
static void present(void) {
  unsigned i;
  VIDEO_SetNextFramebuffer(framebuffer);
  VIDEO_Flush();
  for (i = 0; i < 4u; i++) VIDEO_WaitVSync();
}

int main(void) {
  setup();
  draw();

  /* Look at the framebuffer first: the copy below clears it. */
  test_framebuffer();

  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
  test_display_buffer();
  test_seam_chroma();
  present();

  gc_test_finish();
  return 0;
}
