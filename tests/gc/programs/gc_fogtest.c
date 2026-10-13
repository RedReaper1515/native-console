/*
 * Fog, from a game's point of view.
 *
 * None of dolphin-emu/hwtests' gxtest programs touches the fog unit, so
 * without this it would have no measured evidence behind it at all - and
 * unlike most of the pixel pipeline, fog is not a register the SDK passes
 * through. GX_SetFog performs a change of variables: a game hands it four
 * distances and it writes three constants, a shift and a function select,
 * encoding two of the constants as the top twenty bits of a float word.
 * Everything between those four distances and a fogged pixel is ours to
 * get right, and most of it is arithmetic that fails quietly.
 *
 * The projection here is the same flat orthographic one gc_drawtest uses,
 * so a vertex at z = v lands at screen depth 1 - v: z = 0 is the far
 * plane and z = 1 the near one. Five strips at five depths give five
 * points on the curve.
 *
 * What this pins and what it does not: the linear curve and the whole
 * register path - libogc's encoding, our decoding, the float
 * reconstruction, the blend - are checked against numbers worked out by
 * hand. The exponential curves are checked against *our own* formulas,
 * because docs/gc-fog-indirect.md could not establish them from any source
 * and says so. Those subtests exist to stop the curves changing by
 * accident, not because they are known to be right; a hardware test would
 * overwrite the expectations, not the structure.
 */

#include <malloc.h>
#include <math.h>
#include <string.h>

#include <gccore.h>

#include "gc_test_report.h"

#define FIFO_SIZE (256 * 1024)
#define STRIPS 5

static GXRModeObj *mode;
static void *framebuffer;
static void *fifo;

/* The five screen depths the strips sit at, and where to look for each. */
static const f32 SCREEN_Z[STRIPS] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

static u16 strip_x(unsigned i) { return (u16)(128u * i + 64u); }

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
  /* ALWAYS rather than LEQUAL, because the strips are deliberately at
   * different depths and the point is to fog them, not to sort them. */
  GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
  GX_SetColorUpdate(GX_TRUE);

  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
}

/* Five white strips across the screen, each at its own depth. A vertex at
 * z = v lands at screen depth 1 - v under this projection. */
static void draw_strips(void) {
  unsigned i;
  for (i = 0; i < STRIPS; i++) {
    const f32 left = -1.0f + 0.4f * (f32)i;
    const f32 right = left + 0.4f;
    const f32 v = 1.0f - SCREEN_Z[i];
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(left, 1.0f, v);
    GX_Color4u8(255, 255, 255, 255);
    GX_Position3f32(right, 1.0f, v);
    GX_Color4u8(255, 255, 255, 255);
    GX_Position3f32(right, -1.0f, v);
    GX_Color4u8(255, 255, 255, 255);
    GX_Position3f32(left, -1.0f, v);
    GX_Color4u8(255, 255, 255, 255);
    GX_End();
  }
  GX_DrawDone();
}

/* Wipe the framebuffer so one block cannot read the block before it. */
static void clear(void) {
  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
}

/* With no fog at all, five white strips stay white. This is the control:
 * if it fails, nothing below means anything. */
static void test_no_fog(void) {
  GXColor black = {0, 0, 0, 255};
  unsigned i;

  gc_test_begin("no fog leaves the colour alone");
  GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, black);
  clear();
  draw_strips();

  for (i = 0; i < STRIPS; i++) {
    GXColor got;
    GX_PeekARGB(strip_x(i), 200, &got);
    snprintf(gc_test_line, sizeof(gc_test_line), "the strip at depth %d/4 is still white: got %u",
             (int)(SCREEN_Z[i] * 4.0f), got.r);
    gc_test_check(got.r == 255 && got.g == 255 && got.b == 255, gc_test_line);
  }
  gc_test_end();
}

/* Linear fog, with the four distances chosen so that the whole change of
 * variables collapses to the identity: near 0, far 1, start 0, end 1 make
 * A = 1 and C = 0, so the fog coordinate *is* the screen depth and the
 * factor is the screen depth too.
 *
 * A white strip fogged towards black by factor f is 255(1-f), so the five
 * depths give 255, 191, 128, 64 and 0. Those are worked out by hand from
 * the definition, not from the implementation. */
static void test_linear(void) {
  GXColor black = {0, 0, 0, 255};
  static const u8 expect[STRIPS] = {255, 191, 128, 64, 0};
  unsigned i;

  gc_test_begin("linear fog towards black");
  GX_SetFog(GX_FOG_ORTHO_LIN, 0.0f, 1.0f, 0.0f, 1.0f, black);
  clear();
  draw_strips();

  for (i = 0; i < STRIPS; i++) {
    GXColor got;
    GX_PeekARGB(strip_x(i), 200, &got);
    snprintf(gc_test_line, sizeof(gc_test_line), "depth %d/4 fogs to %u: got %u",
             (int)(SCREEN_Z[i] * 4.0f), expect[i], got.r);
    /* Two levels either way: the depth is interpolated into 24 bits and
     * truncated, and the blend rounds. */
    gc_test_check(got.r + 2u >= expect[i] && got.r <= expect[i] + 2u, gc_test_line);
  }
  gc_test_end();
}

/* The fog colour is a colour, not just a darkening, and alpha never goes
 * through it - the register has no alpha field to blend towards. */
static void test_colour_and_alpha(void) {
  GXColor red = {255, 0, 0, 0};
  GXColor got;

  gc_test_begin("the fog colour, and the alpha it does not touch");
  GX_SetFog(GX_FOG_ORTHO_LIN, 0.0f, 1.0f, 0.0f, 1.0f, red);
  clear();
  draw_strips();

  /* The last strip is at the far end, where the factor is one and the
   * pixel is the fog colour outright. */
  GX_PeekARGB(strip_x(STRIPS - 1u), 200, &got);
  snprintf(gc_test_line, sizeof(gc_test_line), "fully fogged is the fog colour: got %u %u %u",
           got.r, got.g, got.b);
  gc_test_check(got.r >= 253 && got.g <= 2 && got.b <= 2, gc_test_line);

  snprintf(gc_test_line, sizeof(gc_test_line), "and its alpha is untouched: got %u", got.a);
  gc_test_check(got.a == 255, gc_test_line);

  /* The near end is not fogged at all, so it keeps the white it was. */
  GX_PeekARGB(strip_x(0), 200, &got);
  snprintf(gc_test_line, sizeof(gc_test_line), "unfogged is still white: got %u %u %u", got.r,
           got.g, got.b);
  gc_test_check(got.r == 255 && got.g == 255 && got.b == 255, gc_test_line);
  gc_test_end();
}

/* The exponential curve.
 *
 * **This pins an assumption, not a measurement.** No source read while
 * writing docs/gc-fog-indirect.md states the four exponential shapes: the
 * SDK's header is a bare list of enumerators and its source passes the
 * three-bit select straight through. The shape used here, f = 1 - 2^(-8z),
 * is what reimplementations converge on and it is at least self-consistent
 * with the reverse forms. The subtest exists so that the curve cannot
 * change without somebody noticing; when a hardware test settles the real
 * shape, the expectations here are what it replaces.
 *
 * The endpoints are worth keeping either way: any sane curve leaves the
 * near end alone and fogs the far end nearly completely. */
static void test_exponential(void) {
  GXColor black = {0, 0, 0, 255};
  unsigned i;

  gc_test_begin("exponential fog (pins our assumed curve)");
  GX_SetFog(GX_FOG_ORTHO_EXP, 0.0f, 1.0f, 0.0f, 1.0f, black);
  clear();
  draw_strips();

  for (i = 0; i < STRIPS; i++) {
    const f32 f = 1.0f - powf(2.0f, -8.0f * SCREEN_Z[i]);
    const int expect = (int)(255.0f * (1.0f - f) + 0.5f);
    GXColor got;
    GX_PeekARGB(strip_x(i), 200, &got);
    snprintf(gc_test_line, sizeof(gc_test_line), "depth %d/4 fogs to about %d: got %u",
             (int)(SCREEN_Z[i] * 4.0f), expect, got.r);
    gc_test_check((int)got.r + 2 >= expect && (int)got.r <= expect + 2, gc_test_line);
  }
  gc_test_end();
}

/* The perspective path, which is the one that actually matters.
 *
 * Orthographic fog is a multiply and a subtract. Perspective fog is where
 * the hardware earns its registers: it holds A, a 24-bit B magnitude and a
 * shift, and evaluates A/(B - zs) to recover something proportional to
 * eye-space Z from a depth that is anything but linear in it. libogc
 * derives all three from the four distances, truncates A and C to the top
 * twenty bits of their float words, and chooses the shift so that the
 * subtraction can be done against the raw depth. Nothing in the
 * orthographic block above exercises any of that.
 *
 * The geometry is arranged so no arithmetic is needed to place it: each
 * strip is a quad far larger than the frustum, sitting at a known eye
 * distance, with the scissor cutting it down to its own columns. A quad at
 * constant eye depth has constant screen depth across it, so there is
 * nothing to interpolate and nothing to round.
 *
 * With near 1, far 11, start 1 and end 11, the fog coordinate is
 * (d - 1)/10, so distances 1.5, 3.5, 6, 8.5 and 11 fog a white strip to
 * 242, 191, 128, 64 and 0. */
static void test_perspective(void) {
  GXColor black = {0, 0, 0, 255};
  static const f32 DISTANCE[STRIPS] = {1.5f, 3.5f, 6.0f, 8.5f, 11.0f};
  static const u8 expect[STRIPS] = {242, 191, 128, 64, 0};
  Mtx44 projection;
  unsigned i;

  gc_test_begin("linear fog through a perspective projection");

  guPerspective(projection, 60.0f, 4.0f / 3.0f, 1.0f, 11.0f);
  GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);
  GX_SetFog(GX_FOG_PERSP_LIN, 1.0f, 11.0f, 1.0f, 11.0f, black);

  GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
  clear();

  for (i = 0; i < STRIPS; i++) {
    /* Wide enough to cover the frustum at this distance whatever the
     * field of view is, and scissored down to one strip. */
    const f32 half = DISTANCE[i] * 4.0f;
    const f32 z = -DISTANCE[i];
    GX_SetScissor(128u * i, 0, 128, mode->efbHeight);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(-half, half, z);
    GX_Color4u8(255, 255, 255, 255);
    GX_Position3f32(half, half, z);
    GX_Color4u8(255, 255, 255, 255);
    GX_Position3f32(half, -half, z);
    GX_Color4u8(255, 255, 255, 255);
    GX_Position3f32(-half, -half, z);
    GX_Color4u8(255, 255, 255, 255);
    GX_End();
  }
  GX_DrawDone();
  GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);

  for (i = 0; i < STRIPS; i++) {
    GXColor got;
    GX_PeekARGB(strip_x(i), 200, &got);
    snprintf(gc_test_line, sizeof(gc_test_line), "eye distance %d tenths fogs to %u: got %u",
             (int)(DISTANCE[i] * 10.0f), expect[i], got.r);
    /* Four levels either way. The reciprocal is taken on a depth already
     * quantised to 24 bits, and A and C have had twelve mantissa bits
     * thrown away rather than rounded, so a little drift is the hardware's
     * own and not a mistake. */
    gc_test_check((int)got.r + 4 >= (int)expect[i] && (int)got.r <= (int)expect[i] + 4,
                  gc_test_line);
  }
  gc_test_end();
}

int main(void) {
  setup();
  test_no_fog();
  test_linear();
  test_colour_and_alpha();
  test_exponential();
  test_perspective();
  gc_test_finish();
  return 0;
}
