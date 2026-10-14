/*
 * Copyright (c) 2020 Themaister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Private declarations shared by the RDP's source files: the per-primitive
 * structures, the flag bits and the integer helpers.
 *
 * Ported from parallel-rdp (github.com/Themaister/parallel-rdp-standalone,
 * commit 388d70f5835b352d841d9d9e5a08c5de01470f41): rdp_common.hpp,
 * rdp_data_structures.hpp and shaders/data_structures.h. The structures keep
 * parallel-rdp's names so the port can be read side by side with it.
 *
 * The shaders are GLSL, where int arithmetic wraps and >> of a negative
 * value is arithmetic. C leaves both undefined or implementation-defined,
 * so every operation that can overflow or shift a negative value goes
 * through the helpers below, which do the arithmetic in uint32_t.
 */
#ifndef N64_RDP_INTERNAL_H
#define N64_RDP_INTERNAL_H

#include "n64_rdp.h"
#include "n64_rdp_state.h"

/* Command opcodes (rdp_common.hpp, Op). */
enum {
    RDP_OP_FILL_TRIANGLE = 0x08,
    RDP_OP_SHADE_TEXTURE_Z_TRIANGLE = 0x0f,
    RDP_OP_TEXTURE_RECTANGLE = 0x24,
    RDP_OP_TEXTURE_RECTANGLE_FLIP = 0x25,
    RDP_OP_SYNC_LOAD = 0x26,
    RDP_OP_SYNC_PIPE = 0x27,
    RDP_OP_SYNC_TILE = 0x28,
    RDP_OP_SYNC_FULL = 0x29,
    RDP_OP_SET_KEY_GB = 0x2a,
    RDP_OP_SET_KEY_R = 0x2b,
    RDP_OP_SET_CONVERT = 0x2c,
    RDP_OP_SET_SCISSOR = 0x2d,
    RDP_OP_SET_PRIM_DEPTH = 0x2e,
    RDP_OP_SET_OTHER_MODES = 0x2f,
    RDP_OP_LOAD_TLUT = 0x30,
    RDP_OP_SET_TILE_SIZE = 0x32,
    RDP_OP_LOAD_BLOCK = 0x33,
    RDP_OP_LOAD_TILE = 0x34,
    RDP_OP_SET_TILE = 0x35,
    RDP_OP_FILL_RECTANGLE = 0x36,
    RDP_OP_SET_FILL_COLOR = 0x37,
    RDP_OP_SET_FOG_COLOR = 0x38,
    RDP_OP_SET_BLEND_COLOR = 0x39,
    RDP_OP_SET_PRIM_COLOR = 0x3a,
    RDP_OP_SET_ENV_COLOR = 0x3b,
    RDP_OP_SET_COMBINE = 0x3c,
    RDP_OP_SET_TEXTURE_IMAGE = 0x3d,
    RDP_OP_SET_MASK_IMAGE = 0x3e,
    RDP_OP_SET_COLOR_IMAGE = 0x3f,
};

/* Texture formats and sizes. */
enum { RDP_FMT_RGBA = 0, RDP_FMT_YUV = 1, RDP_FMT_CI = 2, RDP_FMT_IA = 3, RDP_FMT_I = 4 };
enum { RDP_SIZE_4 = 0, RDP_SIZE_8 = 1, RDP_SIZE_16 = 2, RDP_SIZE_32 = 3 };

/* TriangleSetupFlagBits. */
#define RDP_TRI_FLIP 0x01u
#define RDP_TRI_DO_OFFSET 0x02u
#define RDP_TRI_SKIP_XFRAC 0x04u
#define RDP_TRI_INTERLACE_FIELD 0x08u
#define RDP_TRI_INTERLACE_KEEP_ODD 0x10u
#define RDP_TRI_DISABLE_UPSCALING 0x20u
#define RDP_TRI_NATIVE_LOD 0x40u
#define RDP_TRI_FILL_COPY_RASTER 0x80u

/* StaticRasterizationFlagBits. */
#define RDP_RAST_INTERLACE_FIELD (1u << 0)
#define RDP_RAST_INTERLACE_KEEP_ODD (1u << 1)
#define RDP_RAST_AA (1u << 2)
#define RDP_RAST_PERSPECTIVE (1u << 3)
#define RDP_RAST_TLUT (1u << 4)
#define RDP_RAST_TLUT_TYPE (1u << 5)
#define RDP_RAST_CVG_TIMES_ALPHA (1u << 6)
#define RDP_RAST_ALPHA_CVG_SELECT (1u << 7)
#define RDP_RAST_MULTI_CYCLE (1u << 8)
#define RDP_RAST_TEX_LOD (1u << 9)
#define RDP_RAST_SHARPEN_LOD (1u << 10)
#define RDP_RAST_DETAIL_LOD (1u << 11)
#define RDP_RAST_FILL (1u << 12)
#define RDP_RAST_COPY (1u << 13)
#define RDP_RAST_SAMPLE_QUAD (1u << 14)
#define RDP_RAST_ALPHA_TEST (1u << 15)
#define RDP_RAST_ALPHA_TEST_DITHER (1u << 16)
#define RDP_RAST_MID_TEXEL (1u << 17)
#define RDP_RAST_USES_TEXEL0 (1u << 18)
#define RDP_RAST_USES_TEXEL1 (1u << 19)
#define RDP_RAST_USES_LOD (1u << 20)
#define RDP_RAST_USES_PIPELINED_TEXEL1 (1u << 21)
#define RDP_RAST_CONVERT_ONE (1u << 22)
#define RDP_RAST_BILERP0 (1u << 23)
#define RDP_RAST_BILERP1 (1u << 24)
#define RDP_RAST_NEED_NOISE_DUAL (1u << 25)
/* Not in parallel-rdp: the second cycle reads TEXEL1, which is the next
 * pixel's texel0 (see shade_pixel). */
#define RDP_RAST_USES_NEXT_TEXEL0 (1u << 26)
#define RDP_RAST_NEED_NOISE (1u << 28)

/* DepthBlendFlagBits. */
#define RDP_DB_DEPTH_TEST (1u << 0)
#define RDP_DB_DEPTH_UPDATE (1u << 1)
#define RDP_DB_FORCE_BLEND (1u << 3)
#define RDP_DB_IMAGE_READ (1u << 4)
#define RDP_DB_COLOR_ON_COVERAGE (1u << 5)
#define RDP_DB_MULTI_CYCLE (1u << 6)
#define RDP_DB_AA (1u << 7)
#define RDP_DB_DITHER (1u << 8)

/* Combiner selector slots within N64RdpState.combiner[c]. */
enum { RDP_CC_RGB_MULADD, RDP_CC_RGB_MULSUB, RDP_CC_RGB_MUL, RDP_CC_RGB_ADD,
       RDP_CC_A_MULADD, RDP_CC_A_MULSUB, RDP_CC_A_MUL, RDP_CC_A_ADD };

/* The coverage word's mode bits (data_structures.h). */
#define RDP_COVERAGE_FILL 0x40
#define RDP_COVERAGE_COPY 0x20

typedef struct RdpTriangle {
    int32_t xh, xm, xl;
    int32_t yh, ym, yl; /* int16 in parallel-rdp; every value written fits */
    int32_t dxhdy, dxmdy, dxldy;
    uint32_t flags;
    uint32_t tile; /* tile in bits 0-2, max LOD level in bits 3-5 */
} RdpTriangle;

typedef struct RdpAttributes {
    int32_t rgba[4], drgba_dx[4], drgba_de[4], drgba_dy[4];
    int32_t stzw[4], dstzw_dx[4], dstzw_de[4], dstzw_dy[4];
} RdpAttributes;

/* DerivedSetup: per-primitive constants. constants[cycle][slot][channel],
 * with slots muladd, mulsub, mul, add. */
typedef struct RdpDerived {
    uint8_t constants[2][4][4];
    uint8_t fog_color[4];
    uint8_t blend_color[4];
    uint32_t fill_color;
    uint16_t dz;
    uint8_t dz_compressed;
    uint8_t min_lod;
    int32_t factors[4];
} RdpDerived;

/* The combiner's inputs for one pixel, laid out so that each slot of each
 * cycle reads from a fixed place chosen once per primitive
 * (n64_rdp_raster.c, shade_row). Every entry is four values, read whole by
 * a colour slot and one at a time by an alpha slot: zero, the first cycle's
 * result, the two texels, the texel the second cycle sees as TEXEL1, the
 * shade, one, the LOD fraction and the noise; the alphas of five of those
 * repeated for the colour channels; then the per-primitive constants of
 * each cycle and slot, and K4 and K5 of each cycle. */
enum {
    RDP_V_ZERO = 0,
    RDP_V_COMBINED = 4,
    RDP_V_TEXEL0 = 8,
    RDP_V_TEXEL1 = 12,
    RDP_V_NEXT = 16,
    RDP_V_SHADE = 20,
    RDP_V_ONE = 24,
    RDP_V_LOD = 28,
    RDP_V_NOISE = 32,
    RDP_V_COMBINED_ALPHA = 36,
    RDP_V_TEXEL0_ALPHA = 40,
    RDP_V_TEXEL1_ALPHA = 44,
    RDP_V_NEXT_ALPHA = 48,
    RDP_V_SHADE_ALPHA = 52,
    RDP_V_CONSTANTS = 56, /* [cycle][slot][4] */
    RDP_V_K4 = 88,        /* [cycle][4] */
    RDP_V_K5 = 96,        /* [cycle][4] */
    RDP_V_COUNT = 104,
};

/* What shade_row() works out once per primitive. */
typedef struct RdpFast {
    int enabled;
    /* The colour and depth images at pixel 0, and their hidden bits. */
    uint8_t *color, *color_hidden, *depth, *depth_hidden;
    /* For a primitive drawn larger (scale_log2 not 0): the same four in
     * slice 0 of the larger picture, and how far apart the slices are. A
     * pixel's words are in the slice its place within the console's pixel
     * names, at the console's pixel. */
    size_t slice_stride, slice_hidden_stride;
    /* Where each combiner slot (RDP_CC_*) of each cycle reads. */
    uint8_t source[2][8];
    int32_t values[RDP_V_COUNT]; /* the constant entries filled in */
} RdpFast;

/* Everything the pixel pipeline needs about one primitive. */
typedef struct RdpPrimitive {
    RdpTriangle setup;
    RdpAttributes attr;
    RdpDerived derived;
    int32_t scissor_xlo, scissor_ylo, scissor_xhi, scissor_yhi;
    uint32_t static_flags; /* normalized */
    int32_t dither;
    uint8_t combiner[2][8]; /* normalized */
    uint32_t depth_blend_flags;
    uint8_t blend_modes[2][4];
    uint8_t coverage_mode, z_mode;
    uint32_t primitive_index;
    int32_t span_ylo, span_yhi; /* scanlines with spans; spans[0] is span_ylo */
    /* Colour image, as the pixel pipeline sees it (GlobalFBInfo). */
    int32_t fb_size; /* 0, 1, 2 or 4 bytes per pixel (0 = 4-bit) */
    int32_t dx_mask, dx_shift;
    RdpFast fast;
    /* The primitive's spans, spans[0] being scanline span_ylo:
     * N64RdpState.spans for the console's own picture. */
    const N64RdpSpan *spans;
    /* parallel-rdp's SCALING_LOG2: 0 for the console's own picture; 1 or 2
     * when the primitive is being drawn again two or four times as large
     * (n64_rdp_raster.c, "The larger picture"). x, y, the spans and
     * span_ylo, span_yhi are then in the larger picture's pixels. */
    int scale_log2;
} RdpPrimitive;

/* What LOAD_TILE, LOAD_BLOCK and LOAD_TLUT pass to the TMEM loader. */
enum { RDP_UPLOAD_TILE = 0, RDP_UPLOAD_TLUT = 1, RDP_UPLOAD_BLOCK = 2 };

typedef struct RdpLoadInfo {
    uint32_t tex_addr, tex_width;
    uint32_t fmt, size;
    uint32_t slo, shi, tlo, thi;
    int mode;
} RdpLoadInfo;

/* The pixel pipeline's functions are defined in headers (n64_rdp_memory.h,
 * n64_rdp_combine.h, n64_rdp_texture.h) and compiled into the rasteriser,
 * so that the compiler sees a primitive's whole pixel loop at once. */
#if defined(_MSC_VER)
#define RDP_INLINE static __inline
#define RDP_FORCE_INLINE static __forceinline
#elif defined(__GNUC__)
#define RDP_INLINE static inline
#define RDP_FORCE_INLINE static inline __attribute__((always_inline))
#else
#define RDP_INLINE static inline
#define RDP_FORCE_INLINE static inline
#endif

/* GLSL integer semantics in C. MSVC, GCC and Clang define the conversion of
 * an out-of-range value to a signed type as modular and >> of a negative
 * value as arithmetic, which is what the shaders rely on; anything else
 * gets the same results from unsigned arithmetic. */
#if defined(_MSC_VER) || defined(__GNUC__)
static inline int32_t rdp_u2i(uint32_t v) { return (int32_t)v; }
static inline int32_t rdp_asr(int32_t v, int s) { return v >> s; }
#else
static inline int32_t rdp_u2i(uint32_t v)
{
    return v < 0x80000000u ? (int32_t)v : (int32_t)(v - 0x80000000u) - 0x7fffffff - 1;
}
static inline int32_t rdp_asr(int32_t v, int s)
{
    return v < 0 ? rdp_u2i(~(~(uint32_t)v >> s)) : (int32_t)((uint32_t)v >> s);
}
#endif
static inline int32_t rdp_add(int32_t a, int32_t b) { return rdp_u2i((uint32_t)a + (uint32_t)b); }
static inline int32_t rdp_sub(int32_t a, int32_t b) { return rdp_u2i((uint32_t)a - (uint32_t)b); }
static inline int32_t rdp_mul(int32_t a, int32_t b) { return rdp_u2i((uint32_t)a * (uint32_t)b); }
static inline int32_t rdp_shl(int32_t v, int s) { return rdp_u2i((uint32_t)v << s); }
/* Sign-extend the low `bits` bits (GLSL bitfieldExtract(v, 0, bits)). */
static inline int32_t rdp_sext(int32_t v, int bits) { return rdp_asr(rdp_shl(v, 32 - bits), 32 - bits); }
static inline int32_t rdp_min(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t rdp_max(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t rdp_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* GLSL findMSB of a non-negative value and findLSB; -1 for zero. */
#if defined(_MSC_VER)
#include <intrin.h>
static inline int rdp_find_msb(uint32_t v)
{
    unsigned long n;
    return _BitScanReverse(&n, v) ? (int)n : -1;
}
static inline int rdp_find_lsb(uint32_t v)
{
    unsigned long n;
    return _BitScanForward(&n, v) ? (int)n : -1;
}
#elif defined(__GNUC__)
static inline int rdp_find_msb(uint32_t v) { return v ? 31 - __builtin_clz(v) : -1; }
static inline int rdp_find_lsb(uint32_t v) { return v ? __builtin_ctz(v) : -1; }
#else
static inline int rdp_find_msb(uint32_t v)
{
    int n = -1;
    while (v) {
        v >>= 1;
        n++;
    }
    return n;
}
static inline int rdp_find_lsb(uint32_t v)
{
    int n = 0;
    if (!v)
        return -1;
    while (!(v & 1u)) {
        v >>= 1;
        n++;
    }
    return n;
}
#endif
static inline int rdp_bit_count(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (int)((((v + (v >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24);
}

/* n64_rdp_raster.c: draw one primitive now (Renderer::draw_shaded_primitive
 * followed by span setup and the ubershader's per-pixel loop). */
void n64_rdp_draw_primitive(N64Rdp *rdp, RdpTriangle *setup, const RdpAttributes *attr);

/* n64_rdp_tmem.c: LOAD_TILE, LOAD_BLOCK and LOAD_TLUT, and the release of
 * what they keep in links.load_plans. */
void n64_rdp_load_tile(N64Rdp *rdp, uint32_t tile, const RdpLoadInfo *info);
void n64_rdp_free_load_plans(N64Rdp *rdp);

/* Whether the reference paths are in use (n64_rdp_set_reference). */
int n64_rdp_reference_path(void);

/* A picture is complete (SYNC_FULL): what is waiting to be drawn larger
 * can be, while the game goes on. */
void n64_rdp_larger_kick(N64Rdp *rdp);

#include "n64_rdp_workers.h"

/* RDRAM bytes with the contract's bounds (24-bit address, zero beyond
 * rdram_size). */
static inline uint8_t n64_rdp_rdram_read8(const N64Rdp *rdp, uint32_t addr)
{
    addr &= 0xffffffu;
    return addr < rdp->links.rdram_size ? rdp->links.rdram[addr] : 0;
}

static inline uint16_t n64_rdp_rdram_read16(const N64Rdp *rdp, uint32_t addr)
{
    return (uint16_t)((n64_rdp_rdram_read8(rdp, addr) << 8) | n64_rdp_rdram_read8(rdp, addr + 1));
}

static inline uint32_t n64_rdp_rdram_read32(const N64Rdp *rdp, uint32_t addr)
{
    return ((uint32_t)n64_rdp_rdram_read16(rdp, addr) << 16) | n64_rdp_rdram_read16(rdp, addr + 2);
}

/* The colour and depth of one pixel while a primitive shades it
 * (memory_interfacing.h's current_color / current_depth). */
typedef struct RdpPixelMemory {
    uint8_t color[4];
    int color_dirty;
    uint16_t depth;
    uint8_t dz;
    int depth_dirty;
    uint32_t color_index; /* in pixels */
    uint32_t depth_index; /* in 16-bit words */
} RdpPixelMemory;

/* What shade_pixel hands to the memory stage (ShadedData). */
typedef struct RdpShaded {
    uint8_t combined[4];
    int32_t z_dith;
    int32_t coverage_count;
    int32_t shade_alpha;
} RdpShaded;

/* n64_rdp_combine.h: noise, dither, combiner, blender. */
static inline int32_t n64_rdp_noise_combiner(uint16_t n) { return ((n & 7) << 6) | 0x20; }
static inline int32_t n64_rdp_noise_dither_alpha(uint16_t n) { return n & 7; }
static inline int32_t n64_rdp_noise_dither_color(uint16_t n) { return n & 0x1ff; }
static inline int32_t n64_rdp_noise_blend_threshold(uint16_t n) { return n & 0xff; }

typedef struct RdpCombinerInputs {
    uint8_t constant_muladd[4];
    uint8_t constant_mulsub[4];
    uint8_t constant_mul[4];
    uint8_t constant_add[4];
    uint8_t shade[4];
    int32_t combined[4];
    int32_t texel0[4];
    int32_t texel1[4];
    int32_t lod_frac;
    int32_t noise;
} RdpCombinerInputs;

typedef struct RdpBlendInputs {
    uint8_t pixel_color[4];
    uint8_t memory_color[4];
    uint8_t fog_color[4];
    uint8_t blend_color[4];
    uint8_t shade_alpha;
} RdpBlendInputs;

/* n64_rdp_luts.c: parallel-rdp's blender divider table (luts.hpp). */
extern const uint8_t n64_rdp_blender_lut[0x8000];

#endif /* N64_RDP_INTERNAL_H */
