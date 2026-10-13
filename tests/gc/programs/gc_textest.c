/*
 * Textures: every format the hardware can sample, drawn and read back.
 *
 * There is no hardware test for this. dolphin-emu/hwtests never samples a
 * texture at all - its `tev` program sets every stage's texture-enable bit
 * to zero - so the texture unit is the one large part of the pipeline with
 * no measured evidence behind it. What it has instead is
 * docs/gc-texture-formats.md, whose decode rules came out of four
 * permissively licensed decoders; this program is what checks that the
 * implementation matches the document, and that a texture built by libogc's
 * own GX comes back the way it went in.
 *
 * Each format gets a small image built here, byte by byte, in the tiled
 * layout the hardware reads. A quad is drawn with it, one texel to one
 * pixel, and the framebuffer is read back and compared. Where a format
 * cannot represent a colour exactly - four bits a channel, or a shared
 * palette - the expectation is what the format can hold, not what was
 * asked for.
 */

#include <malloc.h>
#include <string.h>

#include <gccore.h>

#include "gc_test_report.h"

#define FIFO_SIZE (256 * 1024)
#define TEX_SIZE 16 /* sixteen by sixteen: two tiles of most formats, one of some */

static GXRModeObj *mode;
static void *framebuffer;
static void *fifo;
static u8 texels[TEX_SIZE * TEX_SIZE * 4] ATTRIBUTE_ALIGN(32);
static u16 palette[256] ATTRIBUTE_ALIGN(32);
/* A whole mip chain: 32x32 down to 1x1 in RGB565, contiguous, which comes
 * to 2048 + 512 + 128 + 32 + 32 + 32 bytes. The last three levels each
 * cost a whole tile even though they hold four texels, one texel and one
 * texel - that rounding is the thing most likely to be got wrong, and a
 * chain built without it has every level past the third overlapping the
 * one before. */
#define MIP_BYTES 2784
static u8 mip_texels[MIP_BYTES] ATTRIBUTE_ALIGN(32);

/* A second map, for the indirect block at the end. */
static u8 indirect_texels[TEX_SIZE * TEX_SIZE] ATTRIBUTE_ALIGN(32);

/* The colour this test puts at (s, t), before any format has rounded it. */
static void wanted(unsigned s, unsigned t, u8 out[4]) {
  out[0] = (u8)(s * 17u);       /* 0, 17, 34 ... 255 across */
  out[1] = (u8)(t * 17u);       /* and down */
  out[2] = (u8)(255u - s * 17u);
  out[3] = 255;
}

/* The byte offset of texel (s, t) in a tiled image. The tile base is in
 * bytes and the position within the tile is in texels, so the two are
 * scaled separately - conflating them is a mistake that leaves the first
 * tile right and every other tile wrong. */
static u32 tiled_offset(unsigned s, unsigned t, unsigned tile_w, unsigned tile_h,
                        unsigned bytes_per_tile, unsigned bytes_per_texel) {
  const unsigned tiles_per_row = (TEX_SIZE + tile_w - 1u) / tile_w;
  const unsigned tile = (t / tile_h) * tiles_per_row + (s / tile_w);
  const unsigned within = (t % tile_h) * tile_w + (s % tile_w);
  return tile * bytes_per_tile + within * bytes_per_texel;
}

static void build_rgb565(void) {
  unsigned s, t;
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) {
      u8 c[4];
      u16 v;
      const u32 at = tiled_offset(s, t, 4, 4, 32, 2);
      wanted(s, t, c);
      v = (u16)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
      texels[at] = (u8)(v >> 8);
      texels[at + 1] = (u8)v;
    }
  }
}

static void build_rgba8(void) {
  unsigned s, t;
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) {
      u8 c[4];
      const unsigned tiles_per_row = TEX_SIZE / 4u;
      const unsigned tile = (t / 4u) * tiles_per_row + (s / 4u);
      const u32 base = tile * 64u;
      const unsigned k = (t % 4u) * 4u + (s % 4u);
      wanted(s, t, c);
      texels[base + k * 2u] = c[3];          /* alpha */
      texels[base + k * 2u + 1u] = c[0];     /* red */
      texels[base + 32u + k * 2u] = c[1];    /* green */
      texels[base + 32u + k * 2u + 1u] = c[2]; /* blue */
    }
  }
}

static void build_i8(void) {
  unsigned s, t;
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) {
      u8 c[4];
      wanted(s, t, c);
      texels[tiled_offset(s, t, 8, 4, 32, 1)] = c[0];
    }
  }
}

static void build_ia8(void) {
  unsigned s, t;
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) {
      u8 c[4];
      const u32 at = tiled_offset(s, t, 4, 4, 32, 2);
      wanted(s, t, c);
      texels[at] = c[3];      /* alpha is the high byte */
      texels[at + 1] = c[0];  /* intensity the low */
    }
  }
}

static void build_c8(void) {
  unsigned s, t, i;
  for (i = 0; i < 256; i++) {
    /* A palette in which entry i is a 565 colour derived from i. */
    palette[i] = (u16)(((i >> 3) << 11) | ((i >> 2) << 5) | (31u - (i >> 3)));
  }
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) {
      texels[tiled_offset(s, t, 8, 4, 32, 1)] = (u8)(s * 16u + t);
    }
  }
  DCFlushRange(palette, sizeof(palette));
}

/* The six formats this program did not check until a commercial game turned
 * out to lean on two of them. Every expectation below is written from
 * docs/gc-texture-formats.md rather than from the runtime's decoder: a test
 * that copied the code under test would agree with it about any bug it has.
 *
 * Four-bit formats put two texels in a byte, the even one in the high
 * nibble. `nibble_at` is the byte and the shift for texel (s, t) in an 8x8
 * tile of 32 bytes. */
static void put_nibble(unsigned s, unsigned t, u8 value) {
  const unsigned tiles_per_row = (TEX_SIZE + 7u) / 8u;
  const unsigned tile = (t / 8u) * tiles_per_row + (s / 8u);
  const unsigned k = (t % 8u) * 8u + (s % 8u);
  const u32 at = tile * 32u + (k >> 1);
  if (k & 1u) texels[at] = (u8)((texels[at] & 0xf0u) | (value & 0x0fu));
  else texels[at] = (u8)((texels[at] & 0x0fu) | ((value & 0x0fu) << 4));
}

static u8 widen4(unsigned v) { return (u8)((v << 4) | v); }
static u8 widen5(unsigned v) { return (u8)((v << 3) | (v >> 2)); }
static u8 widen6(unsigned v) { return (u8)((v << 2) | (v >> 4)); }

/* I4: a nibble of intensity, to every channel. */
static void build_i4(void) {
  unsigned s, t;
  for (t = 0; t < TEX_SIZE; t++)
    for (s = 0; s < TEX_SIZE; s++) put_nibble(s, t, (u8)((s + 2u * t) & 15u));
}
static void expect_i4(unsigned s, unsigned t, u8 out[4]) {
  out[0] = out[1] = out[2] = out[3] = widen4((s + 2u * t) & 15u);
}

/* IA4: alpha in the high nibble, intensity in the low - the same way round
 * as IA8, which the document is at pains to say the two agree on. */
static void build_ia4(void) {
  unsigned s, t;
  for (t = 0; t < TEX_SIZE; t++)
    for (s = 0; s < TEX_SIZE; s++)
      texels[tiled_offset(s, t, 8, 4, 32, 1)] = (u8)((((15u - s) & 15u) << 4) | ((s + t) & 15u));
}
static void expect_ia4(unsigned s, unsigned t, u8 out[4]) {
  out[0] = out[1] = out[2] = widen4((s + t) & 15u);
  out[3] = widen4((15u - s) & 15u);
}

/* RGB5A3, in both of its layouts: opaque five bits a channel on even rows,
 * three bits of alpha and four a channel on odd ones. The second is the
 * case that goes wrong silently - a decoder that reads five-bit fields in
 * both gets the colour subtly wrong, and only on the transparent half. */
static u16 rgb5a3_value(unsigned s, unsigned t) {
  if ((t & 1u) == 0u)
    return (u16)(0x8000u | (((s * 2u) & 31u) << 10) | (((t * 3u) & 31u) << 5) | ((31u - s) & 31u));
  return (u16)(((s & 7u) << 12) | ((s & 15u) << 8) | ((t & 15u) << 4) | ((15u - t) & 15u));
}
static void build_rgb5a3(void) {
  unsigned s, t;
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) {
      const u16 v = rgb5a3_value(s, t);
      const u32 at = tiled_offset(s, t, 4, 4, 32, 2);
      texels[at] = (u8)(v >> 8);
      texels[at + 1] = (u8)v;
    }
  }
}
static void expect_rgb5a3(unsigned s, unsigned t, u8 out[4]) {
  const u16 v = rgb5a3_value(s, t);
  if (v & 0x8000u) {
    out[0] = widen5((v >> 10) & 31u);
    out[1] = widen5((v >> 5) & 31u);
    out[2] = widen5(v & 31u);
    out[3] = 255;
  } else {
    const unsigned a3 = (v >> 12) & 7u;
    out[0] = widen4((v >> 8) & 15u);
    out[1] = widen4((v >> 4) & 15u);
    out[2] = widen4(v & 15u);
    out[3] = (u8)((a3 << 5) | (a3 << 2) | (a3 >> 1));
  }
}

/* The palette the three indexed formats share: entry i is a 565 colour that
 * is different for every i, so a wrong index cannot come back looking
 * right. Built with room for C14X2's sixteen thousand entries even though
 * the checks only reach a few hundred. */
#define BIG_PALETTE 1024
static u16 big_palette[BIG_PALETTE] ATTRIBUTE_ALIGN(32);
static u16 palette_entry(unsigned i) {
  return (u16)((((i * 7u) & 31u) << 11) | (((i * 13u) & 63u) << 5) | ((i * 5u + 3u) & 31u));
}
static void fill_big_palette(void) {
  unsigned i;
  for (i = 0; i < BIG_PALETTE; i++) big_palette[i] = palette_entry(i);
  DCFlushRange(big_palette, sizeof(big_palette));
}
static void expect_palette(unsigned index, u8 out[4]) {
  const u16 v = palette_entry(index);
  out[0] = widen5(v >> 11);
  out[1] = widen6((v >> 5) & 63u);
  out[2] = widen5(v & 31u);
  out[3] = 255;
}

/* C4: a nibble of index, addressed exactly like I4. */
static void build_c4(void) {
  unsigned s, t;
  fill_big_palette();
  for (t = 0; t < TEX_SIZE; t++)
    for (s = 0; s < TEX_SIZE; s++) put_nibble(s, t, (u8)((3u * s + t) & 15u));
}
static void expect_c4(unsigned s, unsigned t, u8 out[4]) { expect_palette((3u * s + t) & 15u, out); }

/* C14X2: fourteen bits of index in a halfword whose top two bits are
 * ignored. Setting them here, deliberately, is the check that they are. */
static void build_c14x2(void) {
  unsigned s, t;
  fill_big_palette();
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) {
      const u16 v = (u16)(0xc000u | ((s * 37u + t * 11u) % BIG_PALETTE));
      const u32 at = tiled_offset(s, t, 4, 4, 32, 2);
      texels[at] = (u8)(v >> 8);
      texels[at + 1] = (u8)v;
    }
  }
}
static void expect_c14x2(unsigned s, unsigned t, u8 out[4]) {
  expect_palette((s * 37u + t * 11u) % BIG_PALETTE, out);
}

/* CMPR. Every 8x8 tile is four 8-byte blocks, each covering a 4x4 corner
 * of it; each block is two 565 endpoints and sixteen 2-bit indices, one
 * byte a row, the leftmost texel in the top two bits.
 *
 * The two things worth a test are the ones that make this not DXT1. The
 * in-between colours are (5*C0 + 3*C1) >> 3 and its mirror, not exact
 * thirds - so red and blue endpoints give 159 and 95, where a PC decoder
 * gives 170 and 85. And the mode is chosen by comparing the endpoints as
 * raw packed halfwords, so swapping them changes the palette, not just its
 * order. `transparent_mode` builds the swapped case. */
static int cmpr_transparent_mode;
static void build_cmpr_blocks(void) {
  unsigned tile, block, row, col;
  const u16 c0 = cmpr_transparent_mode ? 0x001fu : 0xf800u; /* blue then red, or red then blue */
  const u16 c1 = cmpr_transparent_mode ? 0xf800u : 0x001fu;
  const unsigned tiles = ((TEX_SIZE + 7u) / 8u) * ((TEX_SIZE + 7u) / 8u);
  for (tile = 0; tile < tiles; tile++) {
    for (block = 0; block < 4u; block++) {
      const u32 at = tile * 32u + block * 8u;
      texels[at] = (u8)(c0 >> 8);
      texels[at + 1] = (u8)c0;
      texels[at + 2] = (u8)(c1 >> 8);
      texels[at + 3] = (u8)c1;
      for (row = 0; row < 4u; row++) {
        u8 bits = 0;
        for (col = 0; col < 4u; col++) {
          /* col + 2*row, and not the more obvious row + col.
           *
           * The check reads eight fixed texels, and row + col sends none of
           * them to index 3 in the four-colour mode or to index 2 in the
           * other - so the two formulas this block exists to test were
           * never reached, and the test passed without being able to fail.
           * This pattern puts the eight probes on all four indices, and on
           * all three in the transparent mode. Index 3 is still avoided
           * there: its colour is the one rule the sources disagree about. */
          const unsigned idx = cmpr_transparent_mode ? ((col + 2u * row) % 3u) : ((col + 2u * row) & 3u);
          bits = (u8)(bits | (idx << (6u - 2u * col)));
        }
        texels[at + 4u + row] = bits;
      }
    }
  }
}
static void build_cmpr(void) {
  cmpr_transparent_mode = 0;
  build_cmpr_blocks();
}
static void build_cmpr_transparent(void) {
  cmpr_transparent_mode = 1;
  build_cmpr_blocks();
}
static void expect_cmpr_common(unsigned s, unsigned t, u8 out[4], int transparent_mode) {
  const unsigned row = t & 3u, col = s & 3u;
  const unsigned idx = transparent_mode ? ((col + 2u * row) % 3u) : ((col + 2u * row) & 3u);
  /* The endpoints, widened: pure red and pure blue. */
  const unsigned c0[3] = {transparent_mode ? 0u : 255u, 0u, transparent_mode ? 255u : 0u};
  const unsigned c1[3] = {transparent_mode ? 255u : 0u, 0u, transparent_mode ? 0u : 255u};
  unsigned ch;
  for (ch = 0; ch < 3u; ch++) {
    unsigned v;
    if (idx == 0u) v = c0[ch];
    else if (idx == 1u) v = c1[ch];
    else if (transparent_mode) v = (c0[ch] + c1[ch]) >> 1;
    else if (idx == 2u) v = (5u * c0[ch] + 3u * c1[ch]) >> 3;
    else v = (3u * c0[ch] + 5u * c1[ch]) >> 3;
    out[ch] = (u8)v;
  }
  out[3] = 255;
}
static void expect_cmpr(unsigned s, unsigned t, u8 out[4]) { expect_cmpr_common(s, t, out, 0); }
static void expect_cmpr_transparent(unsigned s, unsigned t, u8 out[4]) {
  expect_cmpr_common(s, t, out, 1);
}

/* What the format can actually hold, for comparison. */
static void through_565(u8 c[4]) {
  c[0] = (u8)((c[0] & 0xf8u) | ((c[0] & 0xe0u) >> 5));
  c[1] = (u8)((c[1] & 0xfcu) | ((c[1] & 0xc0u) >> 6));
  c[2] = (u8)((c[2] & 0xf8u) | ((c[2] & 0xe0u) >> 5));
  c[3] = 255;
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
  GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
  GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
  GX_SetDispCopyDst(mode->fbWidth, mode->xfbHeight);
  GX_SetDispCopyYScale((f32)mode->xfbHeight / (f32)mode->efbHeight);
  GX_SetCullMode(GX_CULL_NONE);

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
  GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GX_SetNumChans(0);
  GX_SetNumTexGens(1);
  GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
  GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
  GX_SetColorUpdate(GX_TRUE);

  GX_CopyDisp(framebuffer, GX_TRUE);
  GX_DrawDone();
}

/* One texel to one pixel, in the top-left corner of the screen. The quad
 * is sixteen by sixteen pixels, which for a 640x480 viewport is the NDC
 * range below. */
static void draw_textured(void) {
  const f32 right = -1.0f + 2.0f * (f32)TEX_SIZE / (f32)mode->fbWidth;
  const f32 bottom = 1.0f - 2.0f * (f32)TEX_SIZE / (f32)mode->efbHeight;

  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position3f32(-1.0f, 1.0f, 0.0f);
  GX_TexCoord2f32(0.0f, 0.0f);
  GX_Position3f32(right, 1.0f, 0.0f);
  GX_TexCoord2f32(1.0f, 0.0f);
  GX_Position3f32(right, bottom, 0.0f);
  GX_TexCoord2f32(1.0f, 1.0f);
  GX_Position3f32(-1.0f, bottom, 0.0f);
  GX_TexCoord2f32(0.0f, 1.0f);
  GX_End();
  GX_DrawDone();
}

/* Draw one format and check a handful of texels. */
static void check_format(const char *name, u8 format, void (*build)(void),
                         void (*expect)(unsigned s, unsigned t, u8 out[4])) {
  static const struct { unsigned s, t; } probes[] = {
      {0, 0}, {1, 0}, {0, 1}, {5, 3}, {7, 7}, {8, 4}, {15, 15}, {12, 9}};
  GXTexObj object;
  unsigned i;

  gc_test_begin(name);
  memset(texels, 0, sizeof(texels));
  build();
  DCFlushRange(texels, sizeof(texels));

  GX_InitTexObj(&object, texels, TEX_SIZE, TEX_SIZE, format, GX_CLAMP, GX_CLAMP, GX_FALSE);
  if (format == GX_TF_CI8) {
    GXTlutObj tlut;
    GX_InitTlutObj(&tlut, palette, GX_TL_RGB565, 256);
    GX_LoadTlut(&tlut, GX_TLUT0);
    GX_InitTexObjCI(&object, texels, TEX_SIZE, TEX_SIZE, format, GX_CLAMP, GX_CLAMP, GX_FALSE,
                    GX_TLUT0);
  } else if (format == GX_TF_CI4 || format == GX_TF_CI14) {
    /* The shared big palette. A C4 texture only reaches its first sixteen
     * entries, which is the point: the same table serves both, so an
     * indexed format that reads the wrong width of index lands on a
     * visibly different colour. The large palette slot is used because
     * the small ones cannot hold C14X2's range. */
    GXTlutObj tlut;
    GX_InitTlutObj(&tlut, big_palette, GX_TL_RGB565, BIG_PALETTE);
    GX_LoadTlut(&tlut, GX_BIGTLUT0);
    GX_InitTexObjCI(&object, texels, TEX_SIZE, TEX_SIZE, format, GX_CLAMP, GX_CLAMP, GX_FALSE,
                    GX_BIGTLUT0);
  }
  /* The filter goes on last: initialising the object again resets it, and
   * with the default linear filter every texel comes back blended with its
   * neighbours and no exact comparison can hold. */
  GX_InitTexObjLOD(&object, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GX_LoadTexObj(&object, GX_TEXMAP0);
  draw_textured();

  for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
    u8 want[4];
    GXColor got;
    expect(probes[i].s, probes[i].t, want);
    GX_PeekARGB((u16)probes[i].s, (u16)probes[i].t, &got);
    snprintf(gc_test_line, sizeof(gc_test_line),
             "texel %u,%u: got %u,%u,%u, wanted %u,%u,%u", probes[i].s, probes[i].t, got.r,
             got.g, got.b, want[0], want[1], want[2]);
    gc_test_check(got.r == want[0] && got.g == want[1] && got.b == want[2], gc_test_line);
  }
  gc_test_end();
}

static void expect_565(unsigned s, unsigned t, u8 out[4]) {
  wanted(s, t, out);
  through_565(out);
}

static void expect_exact(unsigned s, unsigned t, u8 out[4]) { wanted(s, t, out); }

static void expect_i8(unsigned s, unsigned t, u8 out[4]) {
  u8 c[4];
  wanted(s, t, c);
  out[0] = out[1] = out[2] = out[3] = c[0];
}

static void expect_ia8(unsigned s, unsigned t, u8 out[4]) {
  u8 c[4];
  wanted(s, t, c);
  out[0] = out[1] = out[2] = c[0];
  out[3] = c[3];
}

static void expect_c8(unsigned s, unsigned t, u8 out[4]) {
  const unsigned i = s * 16u + t;
  const u16 v = (u16)(((i >> 3) << 11) | ((i >> 2) << 5) | (31u - (i >> 3)));
  out[0] = (u8)(((v >> 11) << 3) | ((v >> 11) >> 2));
  out[1] = (u8)((((v >> 5) & 0x3fu) << 2) | (((v >> 5) & 0x3fu) >> 4));
  out[2] = (u8)(((v & 0x1fu) << 3) | ((v & 0x1fu) >> 2));
  out[3] = 255;
}

/* Filtering. A texture of two colours, sampled at the centre of a texel
 * and then at the boundary between two, has to come back with the texel
 * itself in the first case and something between the two in the second -
 * which is the property that tells a working filter from a filter that is
 * off by half a texel. */
static void test_filtering(void) {
  GXTexObj object;
  GXColor centre, between;
  unsigned s, t;

  gc_test_begin("a filtered texture");

  /* Black on the left half, white on the right. */
  memset(texels, 0, sizeof(texels));
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) {
      const u32 at = tiled_offset(s, t, 4, 4, 32, 2);
      const u16 v = (s < TEX_SIZE / 2u) ? 0x0000u : 0xffffu;
      texels[at] = (u8)(v >> 8);
      texels[at + 1] = (u8)v;
    }
  }
  DCFlushRange(texels, sizeof(texels));

  GX_InitTexObj(&object, texels, TEX_SIZE, TEX_SIZE, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GX_InitTexObjLOD(&object, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE,
                   GX_ANISO_1);
  GX_LoadTexObj(&object, GX_TEXMAP0);
  draw_textured();

  /* One texel to one pixel. Pixel four is well inside the dark half.
   *
   * Pixel *seven* is the one that straddles the boundary, not pixel eight:
   * a pixel is sampled at seven twelfths across, so pixel eight's sample
   * is already past the halfway point and sits between two white texels.
   * Pixel seven's sample lands just short of it, between the last dark
   * texel and the first light one. */
  GX_PeekARGB(4, 4, &centre);
  GX_PeekARGB(7, 4, &between);

  snprintf(gc_test_line, sizeof(gc_test_line), "inside the dark half: got %u", centre.r);
  gc_test_check(centre.r == 0, gc_test_line);
  snprintf(gc_test_line, sizeof(gc_test_line), "across the boundary: got %u", between.r);
  gc_test_check(between.r > 0 && between.r < 255, gc_test_line);

  /* And with the filter off, the same two pixels are the texels
   * themselves. */
  GX_InitTexObjLOD(&object, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GX_LoadTexObj(&object, GX_TEXMAP0);
  draw_textured();
  GX_PeekARGB(7, 4, &between);
  snprintf(gc_test_line, sizeof(gc_test_line), "unfiltered, the same pixel is one texel: got %u",
           between.r);
  gc_test_check(between.r == 0, gc_test_line);

  gc_test_end();
}

/* Indirect textures.
 *
 * An indirect stage samples a map and reads the texel as a *vector of
 * offsets* rather than a colour, and those offsets move the coordinate a
 * later ordinary stage samples with. Almost nothing about that path can be
 * checked by looking at a picture: an offset that is out by a factor of
 * two, or that lands on the wrong axis, still produces a texture.
 *
 * So this arranges an offset whose value is known exactly. The regular map
 * is sixteen columns of increasing brightness, one texel to one pixel, so
 * the column a pixel samples can be read straight off its own colour. The
 * indirect map is a constant 128 everywhere, and the matrix is a single
 * element of 0.5 with a scale exponent of -4:
 *
 *     offset_s = 0.5 * 128 * 2^-4 = 4 texels,   offset_t = 0
 *
 * so every pixel should sample four columns to its right. Four columns is
 * far enough that no rounding could produce it by accident and close
 * enough that it stays inside the map.
 *
 * The three subtests are chosen to fail differently. Without indirection a
 * pixel reads its own column; with it, four along; and with the regular
 * coordinate wrapped to zero - which is how "replace the coordinate
 * entirely" is expressed - every pixel reads column four, wherever it is.
 * An implementation that applied the offset on the wrong axis, or before
 * the wrap instead of after, fails the third and passes the first two. */
static void test_indirect(void) {
  GXTexObj regular, indirect;
  /* One element, so only S moves. The second row is zero, which is what
   * keeps T where it was. */
  f32 offset[2][3] = {{0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
  GXColor got;
  unsigned s, t;

  gc_test_begin("an indirect texture offset");

  /* Sixteen columns, brightness sixteen apart. */
  for (t = 0; t < TEX_SIZE; t++) {
    for (s = 0; s < TEX_SIZE; s++) indirect_texels[tiled_offset(s, t, 8, 4, 32, 1)] = (u8)(s * 16u);
  }
  DCFlushRange(indirect_texels, sizeof(indirect_texels));
  /* And a constant map for the indirect lookup itself. */
  memset(texels, 128, TEX_SIZE * TEX_SIZE);
  DCFlushRange(texels, sizeof(texels));

  GX_InitTexObj(&regular, indirect_texels, TEX_SIZE, TEX_SIZE, GX_TF_I8, GX_CLAMP, GX_CLAMP,
                GX_FALSE);
  GX_InitTexObjLOD(&regular, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GX_LoadTexObj(&regular, GX_TEXMAP0);

  GX_InitTexObj(&indirect, texels, TEX_SIZE, TEX_SIZE, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GX_InitTexObjLOD(&indirect, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GX_LoadTexObj(&indirect, GX_TEXMAP1);

  /* Both generators read the vertex's one coordinate: the indirect map is
   * constant, so what it is sampled at does not matter, and sharing keeps
   * the vertex format alone. */
  GX_SetNumTexGens(2);
  GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GX_SetNumIndStages(1);
  GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP1);
  GX_SetIndTexMatrix(GX_ITM_0, offset, -4);

  /* The control: the same everything, with indirection switched off. */
  GX_SetTevDirect(GX_TEVSTAGE0);
  draw_textured();
  GX_PeekARGB(2, 2, &got);
  snprintf(gc_test_line, sizeof(gc_test_line), "unoffset, pixel 2 reads column 2 (32): got %u",
           got.r);
  gc_test_check(got.r == 32, gc_test_line);

  GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_0, GX_ITW_OFF,
                    GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
  draw_textured();
  GX_PeekARGB(2, 2, &got);
  snprintf(gc_test_line, sizeof(gc_test_line), "offset, pixel 2 reads column 6 (96): got %u",
           got.r);
  gc_test_check(got.r == 96, gc_test_line);
  GX_PeekARGB(5, 3, &got);
  snprintf(gc_test_line, sizeof(gc_test_line), "offset, pixel 5 reads column 9 (144): got %u",
           got.r);
  gc_test_check(got.r == 144, gc_test_line);

  /* And with the regular coordinate zeroed, the offset is the whole
   * coordinate: every pixel reads column four. */
  GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_0, GX_ITW_0,
                    GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
  draw_textured();
  GX_PeekARGB(2, 2, &got);
  snprintf(gc_test_line, sizeof(gc_test_line), "coordinate replaced, pixel 2 reads column 4 (64): got %u",
           got.r);
  gc_test_check(got.r == 64, gc_test_line);
  GX_PeekARGB(11, 6, &got);
  snprintf(gc_test_line, sizeof(gc_test_line), "and so does pixel 11: got %u", got.r);
  gc_test_check(got.r == 64, gc_test_line);

  /* Put the machine back the way the other blocks expect it. */
  GX_SetTevDirect(GX_TEVSTAGE0);
  GX_SetNumIndStages(0);
  GX_SetNumTexGens(1);
  gc_test_end();
}

/* Mipmaps, and the level of detail that picks between them.
 *
 * Every level is a different flat colour, so the colour of a pixel says
 * which level was read and nothing else has to be inferred. The texture is
 * 32 texels across; drawn onto a quad 32 pixels wide that is one texel to
 * one pixel and level 0, and each halving of the quad doubles the texels
 * per pixel and steps one level down the chain. With the nearest-level mip
 * mode the answer is a single level rather than a blend, so the expected
 * colour is exact and a near-miss in the level of detail cannot hide in a
 * gradient.
 *
 * Note the quads are square. The level of detail takes the larger of the
 * two axes, so a quad squashed in one direction would pick its level from
 * that direction alone and make the arithmetic harder to check by hand
 * for no gain.
 *
 * What this does *not* pin is the shape of the log2 between the octave
 * boundaries, which docs/gc-mipmap-lod.md marks as the one consequential
 * open question. Every case here sits exactly on a power of two, which is
 * where every candidate log2 agrees. That is deliberate: a test wants to
 * measure the thing it is about, not the thing nobody can settle. */
static void draw_square(unsigned pixels) {
  const f32 right = -1.0f + 2.0f * (f32)pixels / (f32)mode->fbWidth;
  const f32 bottom = 1.0f - 2.0f * (f32)pixels / (f32)mode->efbHeight;

  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position3f32(-1.0f, 1.0f, 0.0f);
  GX_TexCoord2f32(0.0f, 0.0f);
  GX_Position3f32(right, 1.0f, 0.0f);
  GX_TexCoord2f32(1.0f, 0.0f);
  GX_Position3f32(right, bottom, 0.0f);
  GX_TexCoord2f32(1.0f, 1.0f);
  GX_Position3f32(-1.0f, bottom, 0.0f);
  GX_TexCoord2f32(0.0f, 1.0f);
  GX_End();
  GX_DrawDone();
}

static void test_mipmaps(void) {
  static const struct {
    unsigned bytes;
    u16 colour;
  } levels[] = {{2048u, 0xf800u}, {512u, 0x07e0u}, {128u, 0x001fu},
                {32u, 0xffffu},   {32u, 0xffe0u},  {32u, 0xf81fu}};
  /* The quad's width in pixels, the level it should therefore read, and
   * what colour that level is. */
  static const struct {
    unsigned pixels;
    unsigned level;
    u8 r, g, b;
  } cases[] = {{32u, 0u, 255u, 0u, 0u},    {16u, 1u, 0u, 255u, 0u},
               {8u, 2u, 0u, 0u, 255u},     {4u, 3u, 255u, 255u, 255u},
               /* The tail of the chain, which is the part that tests the
                * rounding: a 2x2 level and a 1x1 level each cost a whole
                * tile, so reading the last one only lands in the right
                * place if the level before it was sized at 32 bytes and
                * not at the eight its texels would suggest. */
               {2u, 4u, 255u, 255u, 0u},   {1u, 5u, 255u, 0u, 255u}};
  GXTexObj object;
  unsigned i, at = 0;

  gc_test_begin("a mipmapped texture picks its level");

  /* A flat colour needs no tiling: every two bytes of the level are the
   * same, whatever order the texels are in. */
  for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
    unsigned k;
    for (k = 0; k < levels[i].bytes; k += 2u) {
      mip_texels[at + k] = (u8)(levels[i].colour >> 8);
      mip_texels[at + k + 1u] = (u8)levels[i].colour;
    }
    at += levels[i].bytes;
  }
  DCFlushRange(mip_texels, sizeof(mip_texels));

  GX_InitTexObj(&object, mip_texels, 32, 32, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_TRUE);
  /* Nearest level, point sampling within it, and the whole chain allowed.
   * The filter has to be set after GX_InitTexObj, which picks its own. */
  GX_InitTexObjLOD(&object, GX_NEAR_MIP_NEAR, GX_NEAR, 0.0f, 5.0f, 0.0f, GX_FALSE, GX_FALSE,
                   GX_ANISO_1);
  GX_LoadTexObj(&object, GX_TEXMAP0);

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    GXColor got;
    draw_square(cases[i].pixels);
    GX_PeekARGB((u16)(cases[i].pixels / 2u), (u16)(cases[i].pixels / 2u), &got);
    snprintf(gc_test_line, sizeof(gc_test_line),
             "%u pixels across reads level %u: got %u %u %u", cases[i].pixels, cases[i].level,
             got.r, got.g, got.b);
    gc_test_check(got.r == cases[i].r && got.g == cases[i].g && got.b == cases[i].b,
                  gc_test_line);
  }

  /* And the maximum level clamps: with the chain limited to level one, the
   * quad that would have read level three reads level one instead. This is
   * what separates a real clamp from a level index that happens to land in
   * range. */
  GX_InitTexObjLOD(&object, GX_NEAR_MIP_NEAR, GX_NEAR, 0.0f, 1.0f, 0.0f, GX_FALSE, GX_FALSE,
                   GX_ANISO_1);
  GX_LoadTexObj(&object, GX_TEXMAP0);
  {
    GXColor got;
    draw_square(4u);
    GX_PeekARGB(2, 2, &got);
    snprintf(gc_test_line, sizeof(gc_test_line), "clamped to level one, it is green: got %u %u %u",
             got.r, got.g, got.b);
    gc_test_check(got.r == 0 && got.g == 255 && got.b == 0, gc_test_line);
  }
  gc_test_end();
}

int main(void) {
  setup();

  check_format("a 565 texture", GX_TF_RGB565, build_rgb565, expect_565);
  check_format("an eight-bit-a-channel texture", GX_TF_RGBA8, build_rgba8, expect_exact);
  check_format("an intensity texture", GX_TF_I8, build_i8, expect_i8);
  check_format("an intensity and alpha texture", GX_TF_IA8, build_ia8, expect_ia8);
  check_format("a paletted texture", GX_TF_CI8, build_c8, expect_c8);
  check_format("a four-bit intensity texture", GX_TF_I4, build_i4, expect_i4);
  check_format("a four-bit intensity and alpha texture", GX_TF_IA4, build_ia4, expect_ia4);
  check_format("an RGB5A3 texture, in both of its layouts", GX_TF_RGB5A3, build_rgb5a3,
               expect_rgb5a3);
  check_format("a four-bit paletted texture", GX_TF_CI4, build_c4, expect_c4);
  check_format("a fourteen-bit paletted texture", GX_TF_CI14, build_c14x2, expect_c14x2);
  check_format("a compressed texture, four-colour mode", GX_TF_CMPR, build_cmpr, expect_cmpr);
  check_format("a compressed texture, with the endpoints the other way round", GX_TF_CMPR,
               build_cmpr_transparent, expect_cmpr_transparent);
  test_filtering();
  test_mipmaps();
  test_indirect();

  gc_test_finish();
  return 0;
}
