/*
 * Blending that reads the framebuffer, and overlapping triangles that read
 * what each other just wrote.
 *
 * This exists because of a gap found the hard way. The OpenGL path cannot
 * let one draw read pixels the same draw is writing, so a batch whose
 * pixels read the destination is cut into layers, drawn in order, with the
 * framebuffer made readable between them - by copying it, or, where the
 * driver has ARB_texture_barrier, by a barrier. Both are delicate, and
 * neither was covered: every program in tools/gc/compare_gl.py's suite came
 * back with `dst_copies=0`, so the whole layered path was being changed on
 * the strength of looking at Shrek 2's frames by hand.
 *
 * So: overlapping quads in a single batch, with blend modes that read what
 * is already there. A pixel under n of them has been through the blender n
 * times, which is only right if the layers ran in order - and the program
 * works out what each pixel should be by doing the same arithmetic on the
 * CPU, which is the guest's own answer rather than the graphics pipeline's.
 *
 * The three modes are the three branches of the pixel engine's blend
 * (docs/gc-bp-registers.md, gc_gx_blend.c): the factored blend, subtract,
 * and a logic op. All three read the destination, so all three layer.
 */

#include <malloc.h>
#include <string.h>

#include <gccore.h>

#include "gc_test_report.h"

#define FIFO_SIZE (256 * 1024)

static GXRModeObj *mode;
static void *framebuffer;
static void *fifo;

/* The background every section starts from, and the colour every quad
 * adds. Neither is a round number: a channel that came out of the wrong
 * arithmetic should not land on the right answer by luck. */
static const u8 BG_R = 40, BG_G = 80, BG_B = 120;
static const u8 QUAD_R = 20, QUAD_G = 30, QUAD_B = 40;

/* Where a point in the projection's coordinates lands on the screen. */
static u16 screen_x(f32 x) { return (u16)((x + 1.0f) * 0.5f * (f32)mode->fbWidth); }
static u16 screen_y(f32 y) { return (u16)((1.0f - y) * 0.5f * (f32)mode->efbHeight); }

static int clamp_byte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* One channel through the blender, the way the pixel engine does it: each
 * side is multiplied by a factor and the sum is shifted down by eight. A
 * factor is 0 to 255 with its top bit added, so "one" is 256 and the sum
 * is the sum (gc_gx_blend.c; until 2026-10-06 this took "one" as 255, which
 * cost every layer a level). */
static int blend_one_one(int source, int destination) {
  return clamp_byte((source * 256 + destination * 256) >> 8);
}

static void quad(f32 left, f32 top, f32 right, f32 bottom, u8 r, u8 g, u8 b, u8 a) {
  GX_Position3f32(left, top, 0.0f);
  GX_Color4u8(r, g, b, a);
  GX_Position3f32(right, top, 0.0f);
  GX_Color4u8(r, g, b, a);
  GX_Position3f32(right, bottom, 0.0f);
  GX_Color4u8(r, g, b, a);
  GX_Position3f32(left, bottom, 0.0f);
  GX_Color4u8(r, g, b, a);
}

static void setup(void) {
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
  GX_SetColorUpdate(GX_TRUE);

  /* Depth off throughout: every quad here is meant to reach the blender,
   * and a depth test would throw the later ones away. */
  GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
}

/* Clear the framebuffer to the background by copying with the clear on.
 * The copy is also what makes the previous section's pixels go away. */
static void clear_to_background(void) {
  GXColor background;
  background.r = BG_R;
  background.g = BG_G;
  background.b = BG_B;
  background.a = 255;
  GX_SetCopyClear(background, 0x00ffffff);
  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
}

/* Four quads nested inside each other, in one batch, each adding the same
 * colour. A pixel is covered by one, two, three or four of them depending
 * where it is, so the four bands hold four different answers - and the
 * answers are only right if the four quads reached the blender in the order
 * they were sent. A single draw that let them all read the original
 * background would leave every band the same.
 *
 * The right-hand edge is shared so that the overlap is nested rather than
 * staggered: whatever the rasteriser does with edges, a pixel well inside a
 * band is covered by exactly as many quads as the band's number. */
static void draw_additive(void) {
  GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
  GX_Begin(GX_QUADS, GX_VTXFMT0, 16);
  quad(-0.8f, 0.5f, 0.8f, -0.5f, QUAD_R, QUAD_G, QUAD_B, 255);
  quad(-0.6f, 0.5f, 0.8f, -0.5f, QUAD_R, QUAD_G, QUAD_B, 255);
  quad(-0.4f, 0.5f, 0.8f, -0.5f, QUAD_R, QUAD_G, QUAD_B, 255);
  quad(-0.2f, 0.5f, 0.8f, -0.5f, QUAD_R, QUAD_G, QUAD_B, 255);
  GX_End();
  GX_DrawDone();
}

static void test_additive(void) {
  /* The middle of each band, and a point outside every quad. */
  static const f32 at[5] = {-0.9f, -0.7f, -0.5f, -0.3f, 0.4f};
  static const int covered[5] = {0, 1, 2, 3, 4};
  unsigned i;

  gc_test_begin("layered blending: quads that read what the quads before them wrote");
  for (i = 0; i < 5u; i++) {
    int want_r = BG_R, want_g = BG_G, want_b = BG_B;
    GXColor got;
    int n;
    for (n = 0; n < covered[i]; n++) {
      want_r = blend_one_one(QUAD_R, want_r);
      want_g = blend_one_one(QUAD_G, want_g);
      want_b = blend_one_one(QUAD_B, want_b);
    }
    GX_PeekARGB(screen_x(at[i]), screen_y(0.0f), &got);
    snprintf(gc_test_line, sizeof(gc_test_line),
             "under %d quad(s): got %u,%u,%u, wanted %d,%d,%d", covered[i], got.r, got.g, got.b,
             want_r, want_g, want_b);
    gc_test_check(got.r == (u8)want_r && got.g == (u8)want_g && got.b == (u8)want_b, gc_test_line);
  }
  gc_test_end();
}

/* Subtract is its own branch of the blender and ignores both factors: the
 * destination less the source, clamped at zero. Three nested quads take the
 * background down in steps and the fourth band reaches the floor, which is
 * the clamp. */
static void draw_subtract(void) {
  GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
  GX_Begin(GX_QUADS, GX_VTXFMT0, 16);
  quad(-0.8f, 0.5f, 0.8f, -0.5f, QUAD_R, QUAD_G, QUAD_B, 255);
  quad(-0.6f, 0.5f, 0.8f, -0.5f, QUAD_R, QUAD_G, QUAD_B, 255);
  quad(-0.4f, 0.5f, 0.8f, -0.5f, QUAD_R, QUAD_G, QUAD_B, 255);
  quad(-0.2f, 0.5f, 0.8f, -0.5f, QUAD_R, QUAD_G, QUAD_B, 255);
  GX_End();
  GX_DrawDone();
}

static void test_subtract(void) {
  static const f32 at[5] = {-0.9f, -0.7f, -0.5f, -0.3f, 0.4f};
  static const int covered[5] = {0, 1, 2, 3, 4};
  unsigned i;

  gc_test_begin("layered subtract, down to the clamp");
  for (i = 0; i < 5u; i++) {
    int want_r = BG_R, want_g = BG_G, want_b = BG_B;
    GXColor got;
    int n;
    for (n = 0; n < covered[i]; n++) {
      want_r = clamp_byte(want_r - QUAD_R);
      want_g = clamp_byte(want_g - QUAD_G);
      want_b = clamp_byte(want_b - QUAD_B);
    }
    GX_PeekARGB(screen_x(at[i]), screen_y(0.0f), &got);
    snprintf(gc_test_line, sizeof(gc_test_line),
             "under %d quad(s): got %u,%u,%u, wanted %d,%d,%d", covered[i], got.r, got.g, got.b,
             want_r, want_g, want_b);
    gc_test_check(got.r == (u8)want_r && got.g == (u8)want_g && got.b == (u8)want_b, gc_test_line);
  }
  gc_test_end();
}

/* A logic op reads the destination too, and is the one branch where the
 * arithmetic is bitwise. AND with a source that clears the low bits of
 * every channel is idempotent, so the second and later quads over a pixel
 * must leave it alone - which is a different way for the layering to be
 * wrong and still look plausible. */
static void draw_logic(void) {
  GX_SetBlendMode(GX_BM_LOGIC, GX_BL_ONE, GX_BL_ONE, GX_LO_AND);
  GX_Begin(GX_QUADS, GX_VTXFMT0, 16);
  quad(-0.8f, 0.5f, 0.8f, -0.5f, 0xf0, 0xf0, 0xf0, 255);
  quad(-0.6f, 0.5f, 0.8f, -0.5f, 0xf0, 0xf0, 0xf0, 255);
  quad(-0.4f, 0.5f, 0.8f, -0.5f, 0xcc, 0xcc, 0xcc, 255);
  quad(-0.2f, 0.5f, 0.8f, -0.5f, 0xcc, 0xcc, 0xcc, 255);
  GX_End();
  GX_DrawDone();
}

static void test_logic(void) {
  static const f32 at[5] = {-0.9f, -0.7f, -0.5f, -0.3f, 0.4f};
  /* The masks each band has been through, in order. */
  static const int masks[5][4] = {
      {0, 0, 0, 0}, {0xf0, 0, 0, 0}, {0xf0, 0xf0, 0, 0}, {0xf0, 0xf0, 0xcc, 0}, {0xf0, 0xf0, 0xcc, 0xcc}};
  unsigned i;

  gc_test_begin("layered logic op: AND, applied once per quad in order");
  for (i = 0; i < 5u; i++) {
    int want_r = BG_R, want_g = BG_G, want_b = BG_B;
    GXColor got;
    int n;
    for (n = 0; n < 4; n++) {
      if (!masks[i][n]) continue;
      want_r &= masks[i][n];
      want_g &= masks[i][n];
      want_b &= masks[i][n];
    }
    GX_PeekARGB(screen_x(at[i]), screen_y(0.0f), &got);
    snprintf(gc_test_line, sizeof(gc_test_line),
             "under %d quad(s): got %u,%u,%u, wanted %d,%d,%d", i, got.r, got.g, got.b, want_r,
             want_g, want_b);
    gc_test_check(got.r == (u8)want_r && got.g == (u8)want_g && got.b == (u8)want_b, gc_test_line);
  }
  gc_test_end();
}

static void quad_at(f32 left, f32 top, f32 right, f32 bottom, f32 z, u8 r, u8 g, u8 b) {
  GX_Position3f32(left, top, z);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(right, top, z);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(right, bottom, z);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(left, bottom, z);
  GX_Color4u8(r, g, b, 255);
}

/* Where the depth test sits. With "depth before texture" a pixel is tested
 * and its depth written before the combiner runs, so a pixel the alpha test
 * then kills has left its depth behind; with it off, both happen after, and
 * such a pixel leaves nothing (gc_gx_blend.c, gc_gx_raster.c).
 *
 * Each half of the screen gets a quad every pixel of which the alpha test
 * kills, and then a second quad a little further away that is drawn only
 * where nothing nearer has been written. On the left the depth test is
 * before texture: the first quad's depth is there and the second is hidden.
 * On the right it is after: nothing is there and the second shows.
 *
 * This is the one pipeline setting an OpenGL program has to declare and
 * cannot branch on (early_fragment_tests), so it is also what tells a
 * general program that has it from one that does not. */
static const u8 PROBE_R = 200, PROBE_G = 60, PROBE_B = 90;

static void draw_depth_location_half(f32 left, f32 right, u8 before_texture) {
  GX_SetZCompLoc(before_texture);
  GX_SetAlphaCompare(GX_NEVER, 0, GX_AOP_AND, GX_NEVER, 0);
  GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  quad_at(left, 0.5f, right, -0.5f, 0.6f, 255, 255, 255);
  GX_End();

  GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GX_SetZMode(GX_TRUE, GX_LESS, GX_FALSE);
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  quad_at(left, 0.5f, right, -0.5f, 0.4f, PROBE_R, PROBE_G, PROBE_B);
  GX_End();
}

static void draw_depth_location(void) {
  GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  /* The clear writes depth only while depth updates are on. */
  GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
  clear_to_background();
  draw_depth_location_half(-0.8f, -0.05f, GX_TRUE);
  draw_depth_location_half(0.05f, 0.8f, GX_FALSE);
  GX_DrawDone();
  GX_SetZCompLoc(GX_TRUE);
  GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

static void test_depth_location(void) {
  static const f32 at[3] = {-0.4f, 0.4f, 0.9f};
  static const char *const where[3] = {"depth before texture", "depth after texture", "outside both"};
  static const int shows[3] = {0, 1, 0};
  unsigned i;

  gc_test_begin("a pixel the alpha test kills leaves its depth only when depth is before texture");
  for (i = 0; i < 3u; i++) {
    const int want_r = shows[i] ? PROBE_R : BG_R;
    const int want_g = shows[i] ? PROBE_G : BG_G;
    const int want_b = shows[i] ? PROBE_B : BG_B;
    GXColor got;
    GX_PeekARGB(screen_x(at[i]), screen_y(0.0f), &got);
    snprintf(gc_test_line, sizeof(gc_test_line), "%s: got %u,%u,%u, wanted %d,%d,%d", where[i], got.r, got.g,
             got.b, want_r, want_g, want_b);
    gc_test_check(got.r == (u8)want_r && got.g == (u8)want_g && got.b == (u8)want_b, gc_test_line);
  }
  gc_test_end();
}

/* Left on the screen so that a run with --dump-frame produces a picture of
 * the last section rather than an empty buffer. */
static void present(void) {
  unsigned i;
  VIDEO_SetNextFramebuffer(framebuffer);
  VIDEO_Flush();
  for (i = 0; i < 4u; i++) VIDEO_WaitVSync();
}

int main(void) {
  setup();

  clear_to_background();
  draw_additive();
  test_additive();

  clear_to_background();
  draw_subtract();
  test_subtract();

  clear_to_background();
  draw_logic();
  test_logic();

  draw_depth_location();
  test_depth_location();

  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
  present();

  gc_test_finish();
  return 0;
}
