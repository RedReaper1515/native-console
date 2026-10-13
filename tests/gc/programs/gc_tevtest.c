/*
 * The texture environment, from a game's point of view.
 *
 * dolphin-emu/hwtests' `tev` program sweeps the combiner exhaustively and
 * is the authority on this arithmetic - but its sweep is some sixty-one
 * thousand render-and-copy round trips and will not finish in an
 * interpreter. This is the short version: a handful of configurations
 * chosen so that each one separates the hardware's arithmetic from the
 * arithmetic somebody would write if they had not read docs/gc-tev.md.
 *
 * That is the whole design principle here. A case whose expected value is
 * the same under both the real rule and the obvious wrong rule tests
 * nothing, however good it looks. Each case below names the wrong answer
 * it rules out.
 *
 * It also exists because the OpenGL path generates a shader for this
 * arithmetic, and `tools/gc/compare_gl.py` holds that shader to the
 * reference rasteriser by running this program both ways. A pass-through
 * stage - which is all any other program uses - would let almost any
 * generator through.
 */

#include <malloc.h>
#include <string.h>

#include <gccore.h>

#include "gc_test_report.h"

#define FIFO_SIZE (256 * 1024)

static GXRModeObj *mode;
static void *framebuffer;
static void *fifo;

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
  GX_SetNumTevStages(1);
  GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
  GX_SetColorUpdate(GX_TRUE);

  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
}

/* A quad over the top-left corner, flat, in one colour. */
static void draw(u8 r, u8 g, u8 b) {
  const f32 right = -1.0f + 2.0f * 64.0f / (f32)mode->fbWidth;
  const f32 bottom = 1.0f - 2.0f * 64.0f / (f32)mode->efbHeight;

  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position3f32(-1.0f, 1.0f, 0.0f);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(right, 1.0f, 0.0f);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(right, bottom, 0.0f);
  GX_Color4u8(r, g, b, 255);
  GX_Position3f32(-1.0f, bottom, 0.0f);
  GX_Color4u8(r, g, b, 255);
  GX_End();
  GX_DrawDone();
}

/* One configuration, drawn and read back. The alpha side is held at a
 * plain pass-through throughout: it is the same arithmetic on one channel
 * instead of three, and letting it vary here would only make each failure
 * harder to place. */
static void check(const char *what, u8 a, u8 b, u8 c, u8 d, u8 op, u8 bias, u8 scale, u8 clamped,
                  u8 raster, int expect, const char *rules_out) {
  GXColor got;

  GX_SetTevColorIn(GX_TEVSTAGE0, a, b, c, d);
  GX_SetTevColorOp(GX_TEVSTAGE0, op, bias, scale, clamped, GX_TEVPREV);
  GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
  GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
  draw(raster, raster, raster);

  GX_PeekARGB(20, 20, &got);
  snprintf(gc_test_line, sizeof(gc_test_line), "%s: wanted %d, got %u (not %s)", what, expect,
           got.r, rules_out);
  gc_test_check((int)got.r == expect, gc_test_line);
}

static void test_combiner(void) {
  gc_test_begin("the combiner's arithmetic");

  /* The control. Everything below is a change from this one line, so if
   * this fails nothing else means anything. */
  check("a stage that passes the colour through", GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC,
        GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, 173, 173, "anything else");

  /* The interpolation weight is widened, c + (c >> 7), so it spans 0 to
   * 256 rather than 0 to 255 - which means a weight of "all the way" really
   * does select b outright. Without the widening the answer is one short,
   * and one short of full white is the kind of error that never looks like
   * an error. */
  check("a weight of 255 selects b completely", GX_CC_ZERO, GX_CC_ONE, GX_CC_RASC, GX_CC_ZERO,
        GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, 255, 255, "254, from a weight of 255");

  /* Halfway, doubled. The widening moves the weight from 128 to 129, and
   * the doubling makes that worth a whole level on the way out. */
  check("half of the colour, doubled", GX_CC_ZERO, GX_CC_RASC, GX_CC_HALF, GX_CC_ZERO,
        GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, 100, 101, "100, from a weight of 128");

  /* Subtracting, which is the same machinery with the sign flipped. */
  check("one minus the colour", GX_CC_ZERO, GX_CC_ONE, GX_CC_RASC, GX_CC_ONE, GX_TEV_SUB,
        GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, 100, 155, "156 or 154");

  /* The one that matters most. With the stage's clamp off, the result
   * leaves the combiner as an eleven-bit signed number and narrows to
   * eight bits by **wrapping**. 100 + 255 is 355, and 355 as a byte is 99 -
   * a value *darker* than either input. An implementation that clamps
   * gives 255, which looks perfectly reasonable and is wrong. */
  check("an unclamped overflow wraps", GX_CC_ZERO, GX_CC_ONE, GX_CC_ONE, GX_CC_RASC, GX_TEV_ADD,
        GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, 100, 99, "255, from clamping");

  /* And with the clamp on, the same configuration does clamp. The pair is
   * the point: one bit of register state, two completely different
   * answers. */
  check("and with the clamp on, it clamps", GX_CC_ZERO, GX_CC_ONE, GX_CC_ONE, GX_CC_RASC,
        GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, 100, 255, "99");

  gc_test_end();
}

int main(void) {
  setup();
  test_combiner();
  gc_test_finish();
  return 0;
}
