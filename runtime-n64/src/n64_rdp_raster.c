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
 * The rasteriser: per-primitive setup, span setup, and the per-pixel
 * pipeline from coverage through texturing and the combiner to the memory
 * stage.
 *
 * Ported from parallel-rdp (commit 388d70f5835b352d841d9d9e5a08c5de01470f41):
 *  - rdp_renderer.cpp: draw_shaded_primitive, fixup_triangle_setup,
 *    allocate_span_jobs, build_derived_attributes, build_combiner_constants,
 *    deduce_static_texture_state, deduce_noise_state, normalize_combiner
 *    and normalize_static_state;
 *  - shaders/span_setup.comp, shaders/shading.h, shaders/coverage.h,
 *    shaders/interpolation.h and shaders/ubershader.comp. The console's
 *    picture is drawn at an upscaling factor of 1; the factor itself is
 *    carried too (RdpPrimitive.scale_log2), for the larger picture at the
 *    end of this file.
 *
 * parallel-rdp batches primitives and shades them tile by tile on the GPU;
 * here each primitive is drawn when its command arrives, scanline by
 * scanline and pixel by pixel. Every pixel a primitive touches depends
 * only on the primitive and that pixel's memory, so the order is
 * immaterial and the results are the same.
 *
 * That is also what lets a primitive's scanlines be drawn by several
 * threads at once (draw_rows and n64_rdp_workers.c), and each scanline by a
 * loop that does the per-primitive work once (shade_row, fill_row) instead
 * of once per pixel. shade_pixel() and the pixel-by-pixel loop in
 * draw_rows() stay as the port of the shaders, which the faster paths are
 * tested against (n64_rdp_set_reference).
 */
#include "n64_rdp_internal.h"

#include "n64_rdp_combine.h"
#include "n64_rdp_texture.h"

#include "n64_rdp_memory.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* N64_RDP_REFERENCE=1 (or n64_rdp_set_reference) draws every pixel through
 * the pixel-by-pixel port of the shaders and gathers every load over the
 * whole of TMEM, without the shortcuts that are held to give the same
 * bytes; the unit test compares the two. */
static int reference_path = -1; /* not chosen yet */

void n64_rdp_set_reference(int reference) { reference_path = reference != 0; }

int n64_rdp_reference_path(void)
{
    if (reference_path < 0) {
        const char *text = getenv("N64_RDP_REFERENCE");
        reference_path = text && *text && *text != '0';
    }
    return reference_path;
}

/* Selector values in the raw command encoding (rdp_common.hpp). */
enum {
    RGB_MULADD_PRIMITIVE = 3, RGB_MULADD_ENV = 5, RGB_MULADD_NOISE = 7, RGB_MULADD_ZERO = 8,
    RGB_MULSUB_PRIMITIVE = 3, RGB_MULSUB_ENV = 5, RGB_MULSUB_KEY_CENTER = 6, RGB_MULSUB_K4 = 7, RGB_MULSUB_ZERO = 8,
    RGB_MUL_PRIMITIVE = 3, RGB_MUL_ENV = 5, RGB_MUL_KEY_SCALE = 6, RGB_MUL_PRIMITIVE_ALPHA = 10,
    RGB_MUL_ENV_ALPHA = 12, RGB_MUL_LOD_FRAC = 13, RGB_MUL_PRIM_LOD_FRAC = 14, RGB_MUL_K5 = 15, RGB_MUL_ZERO = 16,
    RGB_ADD_PRIMITIVE = 3, RGB_ADD_ENV = 5, RGB_ADD_ZERO = 7,
    ALPHA_PRIMITIVE = 3, ALPHA_ENV = 5, ALPHA_ZERO = 7,
    ALPHA_MUL_PRIMITIVE = 3, ALPHA_MUL_ENV = 5, ALPHA_MUL_PRIM_LOD_FRAC = 6, ALPHA_MUL_ZERO = 7,
}; /* ALPHA_MUL_LOD_FRAC (0) is the same raw and normalized: n64_rdp_combine.h */

static void encode_rgb(uint8_t *rgba, uint32_t color)
{
    rgba[0] = (uint8_t)(color >> 24);
    rgba[1] = (uint8_t)(color >> 16);
    rgba[2] = (uint8_t)(color >> 8);
}

/* Fold the per-primitive constant inputs of one cycle into the derived
 * constants (build_combiner_constants). */
static void build_combiner_constants(const N64RdpState *st, RdpDerived *d, int cycle)
{
    const uint8_t *comb = st->combiner[cycle];
    uint8_t(*out)[4] = d->constants[cycle];
    uint32_t prim = st->prim_color, env = st->env_color;

    switch (comb[RDP_CC_RGB_MULADD]) {
    case RGB_MULADD_ENV: encode_rgb(out[0], env); break;
    case RGB_MULADD_PRIMITIVE: encode_rgb(out[0], prim); break;
    default: break;
    }
    switch (comb[RDP_CC_RGB_MULSUB]) {
    case RGB_MULSUB_ENV: encode_rgb(out[1], env); break;
    case RGB_MULSUB_PRIMITIVE: encode_rgb(out[1], prim); break;
    case RGB_MULSUB_K4: encode_rgb(out[1], (uint32_t)st->convert[4] << 8); break; /* 9 bits */
    case RGB_MULSUB_KEY_CENTER:
        out[1][0] = st->key_center[0];
        out[1][1] = st->key_center[1];
        out[1][2] = st->key_center[2];
        break;
    default: break;
    }
    switch (comb[RDP_CC_RGB_MUL]) {
    case RGB_MUL_PRIMITIVE: encode_rgb(out[2], prim); break;
    case RGB_MUL_ENV: encode_rgb(out[2], env); break;
    case RGB_MUL_PRIMITIVE_ALPHA: encode_rgb(out[2], 0x01010101u * (prim & 0xffu)); break;
    case RGB_MUL_ENV_ALPHA: encode_rgb(out[2], 0x01010101u * (env & 0xffu)); break;
    case RGB_MUL_PRIM_LOD_FRAC: encode_rgb(out[2], 0x01010101u * st->prim_lod_frac); break;
    case RGB_MUL_K5: encode_rgb(out[2], (uint32_t)st->convert[5] << 8); break; /* 9 bits */
    case RGB_MUL_KEY_SCALE:
        out[2][0] = st->key_scale[0];
        out[2][1] = st->key_scale[1];
        out[2][2] = st->key_scale[2];
        break;
    default: break;
    }
    switch (comb[RDP_CC_RGB_ADD]) {
    case RGB_ADD_PRIMITIVE: encode_rgb(out[3], prim); break;
    case RGB_ADD_ENV: encode_rgb(out[3], env); break;
    default: break;
    }
    if (comb[RDP_CC_A_MULADD] == ALPHA_PRIMITIVE)
        out[0][3] = (uint8_t)prim;
    else if (comb[RDP_CC_A_MULADD] == ALPHA_ENV)
        out[0][3] = (uint8_t)env;
    if (comb[RDP_CC_A_MULSUB] == ALPHA_PRIMITIVE)
        out[1][3] = (uint8_t)prim;
    else if (comb[RDP_CC_A_MULSUB] == ALPHA_ENV)
        out[1][3] = (uint8_t)env;
    switch (comb[RDP_CC_A_MUL]) {
    case ALPHA_MUL_PRIMITIVE: out[2][3] = (uint8_t)prim; break;
    case ALPHA_MUL_ENV: out[2][3] = (uint8_t)env; break;
    case ALPHA_MUL_PRIM_LOD_FRAC: out[2][3] = st->prim_lod_frac; break;
    default: break;
    }
    if (comb[RDP_CC_A_ADD] == ALPHA_PRIMITIVE)
        out[3][3] = (uint8_t)prim;
    else if (comb[RDP_CC_A_ADD] == ALPHA_ENV)
        out[3][3] = (uint8_t)env;
}

static int32_t normalize_dzpix(int32_t dz)
{
    if (dz >= 0x8000)
        return 0x8000;
    if (dz == 0)
        return 1;
    return 1 << (rdp_find_msb((uint32_t)dz) + 1);
}

static uint8_t dz_compress_setup(int32_t dz)
{
    int val = 0;
    if (dz & 0xff00)
        val |= 8;
    if (dz & 0xf0f0)
        val |= 4;
    if (dz & 0xcccc)
        val |= 2;
    if (dz & 0xaaaa)
        val |= 1;
    return (uint8_t)val;
}

static void build_derived_attributes(const N64RdpState *st, const RdpAttributes *attr, RdpDerived *d)
{
    int i;
    memset(d, 0, sizeof *d);
    if (st->use_prim_depth) {
        d->dz = st->prim_dz;
        d->dz_compressed = dz_compress_setup(d->dz);
    } else {
        int32_t dzdx = rdp_asr(attr->dstzw_dx[2], 16);
        int32_t dzdy = rdp_asr(attr->dstzw_dy[2], 16);
        int32_t dzpix = (dzdx < 0 ? (~dzdx & 0x7fff) : dzdx) + (dzdy < 0 ? (~dzdy & 0x7fff) : dzdy);
        dzpix = normalize_dzpix(dzpix);
        d->dz = (uint16_t)dzpix;
        d->dz_compressed = dz_compress_setup(dzpix);
    }
    build_combiner_constants(st, d, 0);
    build_combiner_constants(st, d, 1);
    for (i = 0; i < 4; i++) {
        d->fog_color[i] = (uint8_t)(st->fog_color >> (24 - 8 * i));
        d->blend_color[i] = (uint8_t)(st->blend_color >> (24 - 8 * i));
    }
    d->fill_color = st->fill_color;
    d->min_lod = st->prim_min_level;
    for (i = 0; i < 4; i++)
        d->factors[i] = st->convert[i];
}

/* Which texels the combiner reads, from the raw selectors. */
static int accesses_texel(const uint8_t *c, int texel1)
{
    uint8_t t = texel1 ? 2 : 1;
    return c[RDP_CC_RGB_MULADD] == t || c[RDP_CC_RGB_MULSUB] == t || c[RDP_CC_RGB_MUL] == t ||
           c[RDP_CC_RGB_ADD] == t || c[RDP_CC_RGB_MUL] == (texel1 ? 9 : 8) || c[RDP_CC_A_MULADD] == t ||
           c[RDP_CC_A_MULSUB] == t || c[RDP_CC_A_MUL] == t || c[RDP_CC_A_ADD] == t;
}

static int accesses_lod_frac(const uint8_t *c)
{
    return c[RDP_CC_RGB_MUL] == RGB_MUL_LOD_FRAC || c[RDP_CC_A_MUL] == ALPHA_MUL_LOD_FRAC;
}

/* deduce_static_texture_state: the bits of it that change shading. They
 * stay in the state, as in parallel-rdp. */
static void deduce_texture_use(N64RdpState *st)
{
    uint32_t f = st->static_flags;
    int multi = (f & RDP_RAST_MULTI_CYCLE) != 0;
    int uses_texel0, uses_texel1, uses_pipelined_texel1, uses_next_texel0, uses_lod_frac;

    if (f & (RDP_RAST_FILL | RDP_RAST_COPY))
        return;

    if (multi) {
        /* The second cycle sees this pixel's texel1 as TEXEL0 and the next
         * pixel's texel0 as TEXEL1. */
        uses_texel0 = accesses_texel(st->combiner[0], 0);
        uses_texel1 = accesses_texel(st->combiner[0], 1) || accesses_texel(st->combiner[1], 0);
        uses_next_texel0 = accesses_texel(st->combiner[1], 1);
        uses_pipelined_texel1 = 0;
        uses_lod_frac = accesses_lod_frac(st->combiner[0]) || accesses_lod_frac(st->combiner[1]);
    } else {
        uses_texel0 = accesses_texel(st->combiner[1], 0);
        uses_texel1 = 0;
        uses_next_texel0 = 0;
        /* Texel1 in one-cycle mode reads the next pixel's texel. */
        uses_pipelined_texel1 = accesses_texel(st->combiner[1], 1);
        /* Deviation from parallel-rdp, which computes LOD in one-cycle mode
         * only with texture LOD on and otherwise leaves the combiner's
         * LOD_FRAC undefined. The LOD unit runs whenever the combiner reads
         * LOD_FRAC (as angrylion models it): Peter Lemon's Cycle1Texture*
         * ROMs scale texel alpha by LOD_FRAC and show the texture opaque,
         * a magnified level-0 texture reading 0xff. */
        uses_lod_frac = accesses_lod_frac(st->combiner[1]);
    }
    if (uses_texel1 && (f & RDP_RAST_CONVERT_ONE))
        uses_texel0 = 1;

    f &= ~(RDP_RAST_USES_TEXEL0 | RDP_RAST_USES_TEXEL1 | RDP_RAST_USES_PIPELINED_TEXEL1 | RDP_RAST_USES_NEXT_TEXEL0 |
           RDP_RAST_USES_LOD);
    if (uses_next_texel0)
        f |= RDP_RAST_USES_NEXT_TEXEL0;
    if (uses_texel0)
        f |= RDP_RAST_USES_TEXEL0;
    if (uses_texel1)
        f |= RDP_RAST_USES_TEXEL1;
    if (uses_pipelined_texel1)
        f |= RDP_RAST_USES_PIPELINED_TEXEL1;
    if (uses_lod_frac || (f & RDP_RAST_TEX_LOD))
        f |= RDP_RAST_USES_LOD;
    st->static_flags = f;
}

static void deduce_noise_state(N64RdpState *st)
{
    uint32_t f = st->static_flags & ~(RDP_RAST_NEED_NOISE | RDP_RAST_NEED_NOISE_DUAL);
    int multi = (f & RDP_RAST_MULTI_CYCLE) != 0;

    if ((st->dither & 3) == 2 || ((st->dither >> 2) & 3) == 2) {
        st->static_flags = f | RDP_RAST_NEED_NOISE;
        return;
    }
    if (f & (RDP_RAST_COPY | RDP_RAST_FILL)) {
        st->static_flags = f;
        return;
    }
    if (multi && st->combiner[0][RDP_CC_RGB_MULADD] == RGB_MULADD_NOISE)
        f |= RDP_RAST_NEED_NOISE;
    if (st->combiner[1][RDP_CC_RGB_MULADD] == RGB_MULADD_NOISE)
        f |= RDP_RAST_NEED_NOISE;
    /* Two noise inputs must see different values. */
    if (multi && st->combiner[0][RDP_CC_RGB_MULADD] == RGB_MULADD_NOISE &&
        st->combiner[1][RDP_CC_RGB_MULADD] == RGB_MULADD_NOISE)
        f |= RDP_RAST_NEED_NOISE_DUAL;
    if ((f & (RDP_RAST_ALPHA_TEST | RDP_RAST_ALPHA_TEST_DITHER)) == (RDP_RAST_ALPHA_TEST | RDP_RAST_ALPHA_TEST_DITHER))
        f |= RDP_RAST_NEED_NOISE;
    st->static_flags = f;
}

/* normalize_combiner: inputs that are per-primitive constants become
 * "zero", which reads the derived constants. */
static void normalize_combiner(const uint8_t in[8], uint8_t out[8])
{
    uint8_t v;
    v = in[RDP_CC_RGB_MULADD];
    out[RDP_CC_RGB_MULADD] = (v == 7 || v == 1 || v == 2 || v == 0 || v == 6 || v == 4) ? v : RGB_MULADD_ZERO;
    v = in[RDP_CC_RGB_MULSUB];
    out[RDP_CC_RGB_MULSUB] = (v == 0 || v == 1 || v == 2 || v == 4 || v == 7) ? v : RGB_MULSUB_ZERO;
    v = in[RDP_CC_RGB_MUL];
    out[RDP_CC_RGB_MUL] = (v == 0 || v == 7 || v == 1 || v == 2 || v == 8 || v == 9 || v == 4 || v == 11 ||
                           v == 13 || v == 15)
                              ? v
                              : RGB_MUL_ZERO;
    v = in[RDP_CC_RGB_ADD];
    out[RDP_CC_RGB_ADD] = (v == 1 || v == 2 || v == 0 || v == 6 || v == 4) ? v : RGB_ADD_ZERO;
    v = in[RDP_CC_A_MULADD];
    out[RDP_CC_A_MULADD] = (v == 0 || v == 1 || v == 2 || v == 4 || v == 6) ? v : ALPHA_ZERO;
    v = in[RDP_CC_A_MULSUB];
    out[RDP_CC_A_MULSUB] = (v == 0 || v == 1 || v == 2 || v == 4 || v == 6) ? v : ALPHA_ZERO;
    v = in[RDP_CC_A_MUL];
    out[RDP_CC_A_MUL] = (v == 0 || v == 1 || v == 2 || v == 4) ? v : ALPHA_MUL_ZERO;
    v = in[RDP_CC_A_ADD];
    out[RDP_CC_A_ADD] = (v == 0 || v == 1 || v == 2 || v == 4 || v == 6) ? v : ALPHA_ZERO;
}

static void normalize_static_state(const N64RdpState *st, RdpPrimitive *p)
{
    uint32_t f = st->static_flags;
    memset(p->combiner, 0, sizeof p->combiner);
    p->dither = 0;

    if (f & RDP_RAST_FILL) {
        p->static_flags = RDP_RAST_FILL;
        return;
    }
    if (f & RDP_RAST_COPY) {
        p->static_flags = f & (RDP_RAST_COPY | RDP_RAST_TLUT | RDP_RAST_TLUT_TYPE | RDP_RAST_USES_TEXEL0 |
                               RDP_RAST_TEX_LOD | RDP_RAST_DETAIL_LOD | RDP_RAST_PERSPECTIVE | RDP_RAST_ALPHA_TEST);
        return;
    }
    if ((f & (RDP_RAST_MULTI_CYCLE | RDP_RAST_USES_PIPELINED_TEXEL1)) == 0)
        f &= ~(RDP_RAST_BILERP1 | RDP_RAST_CONVERT_ONE);
    p->static_flags = f;
    p->dither = (int32_t)st->dither;
    normalize_combiner(st->combiner[0], p->combiner[0]);
    normalize_combiner(st->combiner[1], p->combiner[1]);
}

/* Convert a value with 15 fractional bits to 3, keeping a sticky bit for
 * anything below. */
static int32_t quantize_x(int32_t x) { return rdp_asr(x, 12) | ((x & 0xfff) != 0); }

/* span_setup.comp, interpolate_snapped: `d` steps of a derivative. At a
 * scale a step is a fraction of a pixel, and the sum is kept exact at whole
 * pixels: the whole pixels take the derivative, the rest its quantized
 * fraction. */
RDP_FORCE_INLINE int32_t snapped_steps(int32_t dvalue, int32_t d, int log2)
{
    return rdp_add(rdp_mul(rdp_asr(d, log2), dvalue), rdp_mul(d & ((1 << log2) - 1), rdp_asr(dvalue, log2)));
}

/* span_setup.comp for one scanline y, of the picture at the primitive's
 * scale (SCALING_LOG2 = p->scale_log2, SCALING_FACTOR = scale). */
static void span_setup(const RdpPrimitive *p, int32_t y, N64RdpSpan *span)
{
    const RdpTriangle *setup = &p->setup;
    const RdpAttributes *attr = &p->attr;
    const int log2 = p->scale_log2;
    const int32_t scale = 1 << log2;
    int flip = (setup->flags & RDP_TRI_FLIP) != 0;
    int interlace_en = (setup->flags & RDP_TRI_INTERLACE_FIELD) != 0;
    int keep_odd_field = (setup->flags & RDP_TRI_INTERLACE_KEEP_ODD) != 0;
    int32_t xleft[4], xright[4];
    int32_t yh_base, ym_base, ylo, yhi, lo_scissor, hi_scissor, start_x, end_x;
    int all_invalid = 1, all_over = 1, all_under = 1;
    int i;

    /* Interpolate RGBA and STZW to the scanline. */
    {
        int do_offset = (setup->flags & RDP_TRI_DO_OFFSET) != 0;
        int skip_xfrac = (setup->flags & RDP_TRI_SKIP_XFRAC) != 0;
        int32_t dy = y - rdp_asr(setup->yh, 2) * scale;
        int32_t xh = rdp_add(rdp_mul(setup->xh, scale), rdp_mul(dy, rdp_shl(setup->dxhdy, 2)));
        int32_t drgba_diff[4] = {0, 0, 0, 0}, dstzw_diff[4] = {0, 0, 0, 0};
        int32_t xfrac;

        /* With do_offset the attributes latch at the last sub-scanline
         * rather than the first. */
        if (do_offset) {
            xh = rdp_add(xh, rdp_mul(scale * 3, setup->dxhdy));
            for (i = 0; i < 4; i++) {
                int32_t deh = attr->drgba_de[i] & ~0x1ff, dyh = attr->drgba_dy[i] & ~0x1ff;
                drgba_diff[i] = rdp_add(rdp_sub(rdp_sub(deh, rdp_asr(deh, 2)), dyh), rdp_asr(dyh, 2));
                deh = attr->dstzw_de[i] & ~0x1ff;
                dyh = attr->dstzw_dy[i] & ~0x1ff;
                dstzw_diff[i] = rdp_add(rdp_sub(rdp_sub(deh, rdp_asr(deh, 2)), dyh), rdp_asr(dyh, 2));
            }
        }

        span->interpolation_base_x = rdp_asr(xh, 15);
        xfrac = skip_xfrac ? 0 : (rdp_asr(xh, 7) & 0xff);

        for (i = 0; i < 4; i++) {
            int32_t v = rdp_add(attr->rgba[i], snapped_steps(attr->drgba_de[i], dy, log2));
            v = rdp_add(v & ~0x1ff, drgba_diff[i]);
            v = rdp_sub(v, snapped_steps(rdp_asr(attr->drgba_dx[i], 8) & ~1, xfrac, log2));
            span->rgba[i] = v & ~0x3ff;

            v = rdp_add(attr->stzw[i], snapped_steps(attr->dstzw_de[i], dy, log2));
            v = rdp_add(v & ~0x1ff, dstzw_diff[i]);
            v = rdp_sub(v, snapped_steps(rdp_asr(attr->dstzw_dx[i], 8) & ~1, xfrac, log2));
            span->stzw[i] = v & ~0x3ff;
        }
    }

    /* The edges at the four sub-scanlines. */
    yh_base = (setup->yh & ~3) * scale;
    ym_base = setup->ym * scale;
    ylo = rdp_max(setup->yh, p->scissor_ylo) * scale;
    yhi = rdp_min(setup->yl, p->scissor_yhi) * scale;
    lo_scissor = scale * (p->scissor_xlo << 1);
    hi_scissor = scale * (p->scissor_xhi << 1);

    for (i = 0; i < 4; i++) {
        int32_t y_sub = y * 4 + i;
        int clip_y = y_sub < ylo || y_sub >= yhi;
        int32_t xh = rdp_add(rdp_mul(setup->xh, scale), rdp_mul(y_sub - yh_base, setup->dxhdy));
        int32_t xm = rdp_add(rdp_mul(setup->xm, scale), rdp_mul(y_sub - yh_base, setup->dxmdy));
        int32_t xl = rdp_add(rdp_mul(setup->xl, scale), rdp_mul(y_sub - ym_base, setup->dxldy));
        int32_t xh_q, xl_q, l, r;
        int invalid;
        if (y_sub < scale * setup->ym)
            xl = xm;
        /* The setup kept 28 bits less the dropped one, which is what leaves
         * room for a scale; overflow wraps. */
        xl = rdp_sext(xl, 27 + log2);
        xh = rdp_sext(xh, 27 + log2);
        xh_q = quantize_x(xh);
        xl_q = quantize_x(xl);
        l = flip ? xh_q : xl_q;
        r = flip ? xl_q : xh_q;

        invalid = rdp_asr(l, 1) > rdp_asr(r, 1);
        if (!(rdp_min(l, r) >= hi_scissor))
            all_over = 0;
        if (!(rdp_max(l, r) < lo_scissor))
            all_under = 0;
        l = rdp_clamp(l, lo_scissor, hi_scissor);
        r = rdp_clamp(r, lo_scissor, hi_scissor);
        invalid = invalid || clip_y;
        if (!invalid)
            all_invalid = 0;
        xleft[i] = invalid ? 0xffff : l;
        xright[i] = invalid ? 0 : r;
    }

    start_x = rdp_min(rdp_min(xleft[0], xleft[1]), rdp_min(xleft[2], xleft[3])) >> 3;
    end_x = rdp_max(rdp_max(xright[0], xright[1]), rdp_max(xright[2], xright[3])) >> 3;
    for (i = 0; i < 4; i++) {
        span->xleft[i] = (uint16_t)xleft[i];
        span->xright[i] = (uint16_t)xright[i];
    }
    span->start_x = start_x;
    span->end_x = end_x;
    span->valid_line = (uint16_t)(!all_invalid && !all_over && !all_under);
    if (interlace_en && (rdp_asr(y, log2) & 1) != keep_odd_field)
        span->valid_line = 0;
    span->lodlength = (int16_t)rdp_sext(flip ? end_x - span->interpolation_base_x : span->interpolation_base_x - start_x,
                                        16);
}

/* coverage.h: eight sample points per pixel, two per sub-scanline,
 * numbered so the first covered one is the lowest set bit:
 *   0x01        0x02
 *         0x04        0x08
 *   0x10        0x20
 *         0x40        0x80 */
RDP_FORCE_INLINE int32_t compute_coverage(const N64RdpSpan *span, int32_t x)
{
    static const int32_t offset[8] = {0, 4, 2, 6, 0, 4, 2, 6};
    int32_t xs = x << 3;
    int32_t coverage = 0;
    int i;
    for (i = 0; i < 8; i++) {
        int line = i >> 1;
        int32_t sample = (xs + offset[i]) & 0xffff;
        if (!(sample < span->xleft[line]) && !(sample >= span->xright[line]))
            coverage |= 1 << i;
    }
    return coverage;
}

/* interpolation.h: attributes at the first covered sample. */
RDP_FORCE_INLINE void centroid_offsets(int32_t coverage, int32_t *xoff, int32_t *yoff)
{
    int32_t first = rdp_find_lsb((uint32_t)coverage);
    *yoff = first >> 1;
    *xoff = ((first & 1) << 1) + (*yoff & 1);
}

static void interpolate_rgba(const N64RdpSpan *span, const RdpAttributes *attr, int32_t dx, int32_t coverage,
                             int log2, uint8_t out[4])
{
    int32_t xoff, yoff;
    int i;
    centroid_offsets(coverage, &xoff, &yoff);
    for (i = 0; i < 4; i++) {
        int32_t rgba = rdp_add(span->rgba[i], rdp_mul(rdp_asr(attr->drgba_dx[i] & ~0x1f, log2), dx));
        /* RGBA is interpolated to 9 bits; the extra bit catches slight
         * underflow and overflow for the clamp. */
        int32_t snapped = rdp_asr(rgba, 14);
        snapped = rdp_shl(snapped, 2 + log2);
        snapped = rdp_add(snapped, rdp_add(rdp_mul(xoff, rdp_asr(attr->drgba_dx[i], 14)),
                                           rdp_mul(yoff, rdp_asr(attr->drgba_dy[i], 14))));
        out[i] = (uint8_t)n64_rdp_clamp_9bit(rdp_asr(snapped, 4 + log2));
    }
}

RDP_FORCE_INLINE int32_t clamp_z(int32_t z)
{
    /* [0, 0x3ffff] maps to itself, [0x40000, 0x5ffff] to 0x3ffff and
     * [0x60000, 0x7ffff] to 0. */
    z -= 1 << 17;
    z = rdp_sext(z, 19);
    z += 1 << 17;
    return rdp_clamp(z, 0, 0x3ffff);
}

RDP_FORCE_INLINE void divide_st(const int32_t stw[3], int perspective, int *overflow, int32_t st[2])
{
    int32_t s16[3];
    s16[0] = rdp_asr(stw[0], 16);
    s16[1] = rdp_asr(stw[1], 16);
    s16[2] = rdp_asr(stw[2], 16);
    if (perspective) {
        n64_rdp_perspective_divide(s16, overflow, st);
    } else {
        st[0] = s16[0];
        st[1] = s16[1];
    }
}

/* stw at dx pixels from the span's base: s, t, w are stzw[0], [1], [3]. */
static void interpolate_stw(const int32_t stzw[4], const int32_t dstzw_dx[4], int32_t dx, int log2, int32_t stw[3])
{
    stw[0] = rdp_add(stzw[0], rdp_mul(rdp_asr(dstzw_dx[0] & ~0x1f, log2), dx));
    stw[1] = rdp_add(stzw[1], rdp_mul(rdp_asr(dstzw_dx[1] & ~0x1f, log2), dx));
    stw[2] = rdp_add(stzw[3], rdp_mul(rdp_asr(dstzw_dx[3] & ~0x1f, log2), dx));
}

static void interpolate_stz(const N64RdpSpan *span, const RdpAttributes *attr, int32_t dx, int32_t coverage,
                            int perspective, int uses_lod, int32_t flip_direction, int log2, int32_t st[2],
                            int32_t st_dx[2], int32_t st_dy[2], int32_t *z, int *st_overflow)
{
    int32_t stw[3], stw_dx[3], stw_dy[3];
    int32_t snapped_z, xoff, yoff;
    interpolate_stw(span->stzw, attr->dstzw_dx, dx, log2, stw);

    divide_st(stw, perspective, st_overflow, st);
    if (uses_lod) {
        /* At a scale the step down a line is taken forwards whichever way
         * the primitive is walked (interpolation.h). */
        const int32_t down = log2 ? (flip_direction < 0 ? -flip_direction : flip_direction) : 1;
        stw_dx[0] = rdp_add(stw[0], rdp_mul(flip_direction, rdp_asr(attr->dstzw_dx[0] & ~0x1f, log2)));
        stw_dx[1] = rdp_add(stw[1], rdp_mul(flip_direction, rdp_asr(attr->dstzw_dx[1] & ~0x1f, log2)));
        stw_dx[2] = rdp_add(stw[2], rdp_mul(flip_direction, rdp_asr(attr->dstzw_dx[3] & ~0x1f, log2)));
        stw_dy[0] = rdp_add(stw[0], rdp_mul(down, rdp_asr(attr->dstzw_dy[0] & ~0x7fff, log2)));
        stw_dy[1] = rdp_add(stw[1], rdp_mul(down, rdp_asr(attr->dstzw_dy[1] & ~0x7fff, log2)));
        stw_dy[2] = rdp_add(stw[2], rdp_mul(down, rdp_asr(attr->dstzw_dy[3] & ~0x7fff, log2)));
        divide_st(stw_dx, perspective, st_overflow, st_dx);
        divide_st(stw_dy, perspective, st_overflow, st_dy);
    }

    /* Whole pixels take the derivative, the fraction of one its quantized
     * part, so that z is what the console has at each whole pixel. */
    *z = rdp_add(span->stzw[2], rdp_add(rdp_mul(attr->dstzw_dx[2], rdp_asr(dx, log2)),
                                        rdp_mul(rdp_asr(attr->dstzw_dx[2], log2), dx & ((1 << log2) - 1))));
    centroid_offsets(coverage, &xoff, &yoff);
    snapped_z = rdp_shl(rdp_asr(*z, 10), 2 + log2);
    snapped_z = rdp_add(snapped_z, rdp_add(rdp_mul(xoff, rdp_asr(attr->dstzw_dx[2], 10)),
                                           rdp_mul(yoff, rdp_asr(attr->dstzw_dy[2], 10))));
    *z = clamp_z(rdp_asr(snapped_z, 5 + log2));
}

/* The next pixel's texel0, which the second cycle of a two-cycle pixel
 * reads as TEXEL1: the RDP fetches the next pixel's first texel while it
 * finishes this one. At the end of a span longer than two pixels the next
 * pixel is the start of the next scanline. The texel is sampled exactly as
 * that pixel samples its texel0, LOD and tile selection included.
 *
 * Not in parallel-rdp, which gives the second cycle this pixel's texel0
 * (its comment notes the pipelining but leaves it out). angrylion models
 * the hardware this way, and Lemmy's RDPTex0And1 ROM shows the second
 * cycle's TEXEL1 one pixel ahead of TEXEL0. */
static void next_texel0(const N64Rdp *rdp, const RdpPrimitive *p, const N64RdpSpan *span, int32_t x, int32_t y,
                        int32_t dx, int32_t direction, int32_t out[4])
{
    static const int32_t zero[4] = {0, 0, 0, 0};
    const N64RdpState *st = rdp->state;
    const RdpAttributes *attr = &p->attr;
    const N64RdpSpan *next = &p->spans[y - p->span_ylo + 1];
    const int log2 = p->scale_log2;
    uint32_t f = p->static_flags;
    int perspective = (f & RDP_RAST_PERSPECTIVE) != 0;
    int flip = (p->setup.flags & RDP_TRI_FLIP) != 0;
    int overflow = 0;
    int32_t stw[3], stw_dx[3], stw_dy[3], s_t[2], st_dx[2], st_dy[2], lod_frac = 0;
    uint32_t tile0 = p->setup.tile & 7u, tile1 = (tile0 + 1) & 7u;
    int i;

    if (x == (flip ? span->end_x : span->start_x) && span->lodlength >= 3 && next->valid_line) {
        stw[0] = next->stzw[0];
        stw[1] = next->stzw[1];
        stw[2] = next->stzw[3];
    } else {
        /* The pixel after this one on the console: a whole pixel on. */
        interpolate_stw(span->stzw, attr->dstzw_dx, dx + direction * (1 << log2), log2, stw);
    }
    divide_st(stw, perspective, &overflow, s_t);
    if (f & RDP_RAST_USES_LOD) {
        static const int comp[3] = {0, 1, 3};
        for (i = 0; i < 3; i++) {
            stw_dx[i] = rdp_add(stw[i], rdp_mul(direction, rdp_asr(attr->dstzw_dx[comp[i]] & ~0x1f, log2)));
            stw_dy[i] = rdp_add(stw[i], rdp_asr(attr->dstzw_dy[comp[i]] & ~0x7fff, log2));
        }
        divide_st(stw_dx, perspective, &overflow, st_dx);
        divide_st(stw_dy, perspective, &overflow, st_dy);
        n64_rdp_compute_lod_2cycle(&tile0, &tile1, &lod_frac, p->setup.tile >> 3, p->derived.min_lod, s_t, st_dx,
                                   st_dy, overflow, (f & RDP_RAST_TEX_LOD) != 0, (f & RDP_RAST_SHARPEN_LOD) != 0,
                                   (f & RDP_RAST_DETAIL_LOD) != 0);
    }
    n64_rdp_sample_texture(rdp, &st->tiles[tile0], s_t[0], s_t[1], (f & RDP_RAST_TLUT) != 0,
                           (f & RDP_RAST_TLUT_TYPE) != 0, (f & RDP_RAST_SAMPLE_QUAD) != 0,
                           (f & RDP_RAST_MID_TEXEL) != 0, 0, (f & RDP_RAST_BILERP0) != 0, p->derived.factors, zero,
                           out);
}

/* shading.h: shade_pixel. Returns whether the pixel goes on to the
 * memory stage. */
static int shade_pixel(const N64Rdp *rdp, const RdpPrimitive *p, int32_t x, int32_t y, RdpShaded *shaded)
{
    const N64RdpState *st = rdp->state;
    const int log2 = p->scale_log2;
    /* A primitive that is not to be drawn larger is the console's pixel in
     * every one of that pixel's places (TRIANGLE_SETUP_DISABLE_UPSCALING). */
    const int32_t snap = log2 && (p->setup.flags & RDP_TRI_DISABLE_UPSCALING) ? ~((1 << log2) - 1) : ~0;
    const N64RdpSpan *span = &p->spans[(y & snap) - p->span_ylo];
    const RdpAttributes *attr = &p->attr;
    const RdpDerived *derived = &p->derived;
    uint32_t f = p->static_flags;
    int tlut = (f & RDP_RAST_TLUT) != 0;
    int tlut_type = (f & RDP_RAST_TLUT_TYPE) != 0;
    int sample_quad = (f & RDP_RAST_SAMPLE_QUAD) != 0;
    int cvg_times_alpha = (f & RDP_RAST_CVG_TIMES_ALPHA) != 0;
    int alpha_cvg_select = (f & RDP_RAST_ALPHA_CVG_SELECT) != 0;
    int perspective = (f & RDP_RAST_PERSPECTIVE) != 0;
    int tex_lod_en = (f & RDP_RAST_TEX_LOD) != 0;
    int sharpen_lod_en = (f & RDP_RAST_SHARPEN_LOD) != 0;
    int detail_lod_en = (f & RDP_RAST_DETAIL_LOD) != 0;
    int aa_enable = (f & RDP_RAST_AA) != 0;
    int multi_cycle = (f & RDP_RAST_MULTI_CYCLE) != 0;
    int interlace_en = (f & RDP_RAST_INTERLACE_FIELD) != 0;
    int alpha_test = (f & RDP_RAST_ALPHA_TEST) != 0;
    int alpha_test_dither = (f & RDP_RAST_ALPHA_TEST_DITHER) != 0;
    int mid_texel = (f & RDP_RAST_MID_TEXEL) != 0;
    int uses_texel0 = (f & RDP_RAST_USES_TEXEL0) != 0;
    int uses_texel1 = (f & RDP_RAST_USES_TEXEL1) != 0;
    int uses_pipelined_texel1 = (f & RDP_RAST_USES_PIPELINED_TEXEL1) != 0;
    int uses_lod = (f & RDP_RAST_USES_LOD) != 0;
    int convert_one = (f & RDP_RAST_CONVERT_ONE) != 0;
    int bilerp0 = (f & RDP_RAST_BILERP0) != 0;
    int bilerp1 = (f & RDP_RAST_BILERP1) != 0;
    int flip = (p->setup.flags & RDP_TRI_FLIP) != 0;
    uint16_t noise = 0;
    int32_t coverage, coverage_count, dx, interpolation_direction;
    int32_t st_[2] = {0, 0}, st_dx[2] = {0, 0}, st_dy[2] = {0, 0}, z = 0;
    int perspective_overflow = 0;
    uint32_t tile0, tile1, max_level;
    int32_t lod_frac = 0;
    int32_t texel0[4] = {0, 0, 0, 0}, texel1[4] = {0, 0, 0, 0};
    int32_t rgb_dith, alpha_dith, alpha_reference, combined[4];
    RdpCombinerInputs in;
    uint8_t shade[4];
    int i;

    x &= snap;
    y &= snap;
    if (f & RDP_RAST_NEED_NOISE)
        noise = n64_rdp_noise_seed((uint32_t)x, (uint32_t)y, p->primitive_index);

    if (f & RDP_RAST_COPY) {
        int32_t cdx, snapped_dx, s_offset, lerp_dx, stw[3], dstw[4];
        int32_t texel;
        if (x < span->start_x || x > span->end_x)
            return 0;
        /* interpolate_st_copy: the copy pipe steps st once per 64-bit
         * word of output. */
        cdx = flip ? x - span->start_x : span->end_x - x;
        /* The copy pipe repeats pixels at a scale: there is nothing to
         * filter (interpolation.h). */
        cdx = rdp_asr(cdx, log2);
        snapped_dx = cdx & p->dx_mask;
        s_offset = cdx - snapped_dx;
        lerp_dx = rdp_asr(cdx, p->dx_shift) * (flip ? 1 : -1);
        for (i = 0; i < 4; i++)
            dstw[i] = attr->dstzw_dx[i];
        interpolate_stw(span->stzw, dstw, lerp_dx, 0, stw);
        divide_st(stw, perspective, &perspective_overflow, st_);
        texel = n64_rdp_sample_texture_copy(rdp, p, &st->tiles[p->setup.tile & 7], st_[0], st_[1], s_offset, tlut);
        shaded->z_dith = texel;
        shaded->coverage_count = RDP_COVERAGE_COPY;
        if (alpha_test && p->fb_size == 2 && (texel & 1) == 0)
            return 0;
        return 1;
    }
    if (f & RDP_RAST_FILL) {
        shaded->coverage_count = RDP_COVERAGE_FILL;
        return x >= span->start_x && x <= span->end_x;
    }

    coverage = compute_coverage(span, x);
    if (coverage == 0)
        return 0;
    coverage_count = rdp_bit_count((uint32_t)coverage);
    /* Without AA only the first sample counts. */
    if (!aa_enable && (coverage & 1) == 0)
        return 0;

    dx = x - span->interpolation_base_x;
    interpolation_direction = flip ? 1 : -1;
    interpolate_rgba(span, attr, dx, coverage, log2, shade);
    interpolate_stz(span, attr, dx, coverage, perspective, uses_lod, interpolation_direction, log2, st_, st_dx,
                    st_dy, &z, &perspective_overflow);

    tile0 = p->setup.tile & 7u;
    tile1 = (tile0 + 1) & 7u;
    max_level = p->setup.tile >> 3;
    if (uses_lod) {
        n64_rdp_compute_lod_2cycle(&tile0, &tile1, &lod_frac, max_level, derived->min_lod, st_, st_dx, st_dy,
                                   perspective_overflow, tex_lod_en, sharpen_lod_en, detail_lod_en);
    }

    if (uses_texel0) {
        static const int32_t zero[4] = {0, 0, 0, 0};
        n64_rdp_sample_texture(rdp, &st->tiles[tile0], st_[0], st_[1], tlut, tlut_type, sample_quad, mid_texel, 0,
                               bilerp0, derived->factors, zero, texel0);
    }

    /* Texel1 in one-cycle mode peeks at the next pixel, or at the start of
     * the next scanline at the end of a long span. */
    if (uses_pipelined_texel1) {
        const N64RdpSpan *next = &p->spans[y - p->span_ylo + 1];
        int long_span = span->lodlength >= 8;
        int end_span = x == (flip ? span->end_x : span->start_x);
        int unused_overflow = 0;
        if (end_span && long_span && next->valid_line) {
            int32_t stw[3];
            stw[0] = next->stzw[0];
            stw[1] = next->stzw[1];
            stw[2] = next->stzw[3];
            divide_st(stw, perspective, &unused_overflow, st_);
        } else {
            int32_t stw[3];
            interpolate_stw(span->stzw, attr->dstzw_dx, dx + interpolation_direction * (1 << log2), log2, stw);
            divide_st(stw, perspective, &unused_overflow, st_);
        }
        tile1 = tile0;
        uses_texel1 = 1;
    }

    if (uses_texel1) {
        if (convert_one && !bilerp1)
            n64_rdp_texture_convert_factors(texel0, derived->factors, texel1);
        else
            n64_rdp_sample_texture(rdp, &st->tiles[tile1], st_[0], st_[1], tlut, tlut_type, sample_quad, mid_texel,
                                   convert_one, bilerp1, derived->factors, texel0, texel1);
    }

    n64_rdp_dither_coefficients(x, interlace_en ? rdp_asr(y, 1) : y, p->dither >> 2, p->dither & 3, noise,
                                &rgb_dith, &alpha_dith);

    /* The combiner. */
    for (i = 0; i < 4; i++) {
        in.shade[i] = shade[i];
        in.combined[i] = 0;
        in.texel0[i] = texel0[i];
        in.texel1[i] = texel1[i];
    }
    in.lod_frac = lod_frac;
    in.noise = n64_rdp_noise_combiner(noise);

    if (multi_cycle) {
        memcpy(in.constant_muladd, derived->constants[0][0], 4);
        memcpy(in.constant_mulsub, derived->constants[0][1], 4);
        memcpy(in.constant_mul, derived->constants[0][2], 4);
        memcpy(in.constant_add, derived->constants[0][3], 4);
        n64_rdp_combiner_cycle0(&in, p->combiner[0], alpha_dith, coverage_count, cvg_times_alpha, alpha_cvg_select,
                                alpha_test, &alpha_reference, combined);
        for (i = 0; i < 4; i++)
            in.combined[i] = combined[i];
        memcpy(in.constant_muladd, derived->constants[1][0], 4);
        memcpy(in.constant_mulsub, derived->constants[1][1], 4);
        memcpy(in.constant_mul, derived->constants[1][2], 4);
        memcpy(in.constant_add, derived->constants[1][3], 4);
        /* Pipelining: the second cycle sees texel1 as TEXEL0 and the next
         * pixel's texel0 as TEXEL1. */
        if (f & RDP_RAST_USES_NEXT_TEXEL0)
            next_texel0(rdp, p, span, x, y, dx, interpolation_direction, texel0);
        for (i = 0; i < 4; i++) {
            in.texel0[i] = texel1[i];
            in.texel1[i] = texel0[i];
        }
        if (f & RDP_RAST_NEED_NOISE_DUAL) {
            noise = n64_rdp_noise_seed((uint32_t)(x + 1023), (uint32_t)(y + 7), p->primitive_index + 11u);
            in.noise = n64_rdp_noise_combiner(noise);
        }
        n64_rdp_combiner_cycle1(&in, p->combiner[1], alpha_dith, &coverage_count, cvg_times_alpha, alpha_cvg_select,
                                combined);
    } else {
        memcpy(in.constant_muladd, derived->constants[1][0], 4);
        memcpy(in.constant_mulsub, derived->constants[1][1], 4);
        memcpy(in.constant_mul, derived->constants[1][2], 4);
        memcpy(in.constant_add, derived->constants[1][3], 4);
        n64_rdp_combiner_cycle1(&in, p->combiner[1], alpha_dith, &coverage_count, cvg_times_alpha, alpha_cvg_select,
                                combined);
        alpha_reference = combined[3];
    }

    /* Alpha-to-coverage can take the coverage to zero. */
    if (aa_enable && coverage_count == 0)
        return 0;

    if (alpha_test) {
        int32_t threshold = alpha_test_dither ? n64_rdp_noise_blend_threshold(noise) : derived->blend_color[3];
        if (alpha_reference < threshold)
            return 0;
    }

    for (i = 0; i < 4; i++)
        shaded->combined[i] = (uint8_t)combined[i];
    shaded->z_dith = (z << 9) | rgb_dith;
    shaded->coverage_count = coverage_count;
    shaded->shade_alpha = rdp_min(shade[3] + alpha_dith, 0xff);
    return 1;
}

/* Addresses are 24 bits; below this limit none wraps and every byte is in
 * RDRAM. */
static uint32_t rdram_limit(const N64Rdp *rdp)
{
    return rdp->links.rdram_size < 0x1000000u ? rdp->links.rdram_size : 0x1000000u;
}

/* The byte ranges [lo, hi) of the colour and depth images that scanlines
 * span_ylo..span_yhi cover. Returns 0 for a 4 or 8-bit colour image. */
static int image_ranges(const N64RdpState *st, const RdpPrimitive *p, uint32_t color[2], uint32_t depth[2])
{
    uint32_t first = st->fb_width * (uint32_t)p->span_ylo;
    uint32_t end = st->fb_width * ((uint32_t)p->span_yhi + 1u); /* pixels, exclusive */
    switch (st->fb_fmt) {
    case N64_RDP_FB_RGBA5551:
    case N64_RDP_FB_IA88:
        color[0] = 2u * ((st->fb_addr >> 1) + first);
        color[1] = 2u * ((st->fb_addr >> 1) + end);
        break;
    case N64_RDP_FB_RGBA8888:
        color[0] = 4u * ((st->fb_addr >> 2) + first);
        color[1] = 4u * ((st->fb_addr >> 2) + end);
        break;
    default: return 0;
    }
    depth[0] = 2u * ((st->depth_addr >> 1) + first);
    depth[1] = 2u * ((st->depth_addr >> 1) + end);
    return 1;
}

/* Set up shade_row() for a primitive: one or two-cycle mode drawing into
 * an RGBA5551 colour image, with the colour and depth images of its
 * scanlines apart and wholly inside RDRAM. Anything else is drawn pixel by
 * pixel. */
static void fast_setup(const N64Rdp *rdp, RdpPrimitive *p)
{
    const N64RdpState *st = rdp->state;
    RdpFast *k = &p->fast;
    uint32_t f = p->static_flags, color[2], depth[2], limit = rdram_limit(rdp);
    int multi = (f & RDP_RAST_MULTI_CYCLE) != 0;
    int cycle, slot, c;

    k->enabled = 0;
    if (n64_rdp_reference_path() || (f & (RDP_RAST_FILL | RDP_RAST_COPY)) || st->fb_fmt != N64_RDP_FB_RGBA5551)
        return;
    if (!image_ranges(st, p, color, depth) || color[1] > limit || depth[1] > limit)
        return;
    if (!(color[1] <= depth[0] || depth[1] <= color[0]))
        return;
    k->enabled = 1;
    k->slice_stride = k->slice_hidden_stride = 0;
    k->color = rdp->links.rdram + 2u * (st->fb_addr >> 1);
    k->color_hidden = rdp->links.hidden + (st->fb_addr >> 1);
    k->depth = rdp->links.rdram + 2u * (st->depth_addr >> 1);
    k->depth_hidden = rdp->links.hidden + (st->depth_addr >> 1);

    /* The first cycle of two sees this pixel's texels and no combined
     * colour; the last cycle of two sees texel1 as TEXEL0 and the next
     * pixel's texel0 as TEXEL1. */
    n64_rdp_combiner_sources(p->combiner[0], 0, RDP_V_ZERO, RDP_V_TEXEL0, RDP_V_TEXEL1, k->source[0]);
    n64_rdp_combiner_sources(p->combiner[1], 1, multi ? RDP_V_COMBINED : RDP_V_ZERO,
                             multi ? RDP_V_TEXEL1 : RDP_V_TEXEL0, multi ? RDP_V_NEXT : RDP_V_TEXEL1, k->source[1]);
    memset(k->values, 0, sizeof k->values);
    for (c = 0; c < 4; c++)
        k->values[RDP_V_ONE + c] = 0x100;
    for (cycle = 0; cycle < 2; cycle++) {
        const uint8_t(*constants)[4] = p->derived.constants[cycle];
        for (slot = 0; slot < 4; slot++)
            for (c = 0; c < 4; c++)
                k->values[RDP_V_CONSTANTS + 16 * cycle + 4 * slot + c] = constants[slot][c];
        for (c = 0; c < 4; c++) {
            k->values[RDP_V_K4 + 4 * cycle + c] = (constants[1][1] << 8) | constants[1][2];
            k->values[RDP_V_K5 + 4 * cycle + c] = (constants[2][1] << 8) | constants[2][2];
        }
    }
}

/* Scanline y of a primitive from x0 to x1, in one or two-cycle mode: what
 * shade_pixel() and the memory stage do for each pixel in turn, with the
 * work that does not change along the scanline done once.
 *
 *  - The attributes step by their x derivative instead of being multiplied
 *    out per pixel (the same sums, modulo 2^32).
 *  - Pixels the edges cover whole have all eight coverage samples; the
 *    rest are tested sample by sample.
 *  - s and t are divided only if a texel or the LOD is used, and z is
 *    interpolated only if the depth test or update is on.
 *  - The combiner reads its inputs from a table (RDP_V_*) at places chosen
 *    per primitive rather than switching on each selector per pixel, and
 *    works out the colour channels together (n64_rdp_quad.h).
 *  - The colour and depth words are read and written in place: fast_setup()
 *    has checked that they are in RDRAM and apart.
 *
 * At a scale (p->scale_log2) it is the same scanline of the larger picture,
 * with the factor where shade_pixel() has it: derivatives a fraction of
 * themselves, depth exact at the console's pixels, the next pixel a whole
 * console pixel on; and each pixel's words in its slice of the larger
 * picture, at the console's pixel. */
static void shade_row(N64Rdp *rdp, const RdpPrimitive *p, int32_t y, int32_t x0, int32_t x1, int32_t *v)
{
    static const int32_t zero[4] = {0, 0, 0, 0};
    const N64RdpState *st = rdp->state;
    const RdpFast *k = &p->fast;
    const N64RdpSpan *span = &p->spans[y - p->span_ylo];
    const RdpAttributes *attr = &p->attr;
    const RdpDerived *derived = &p->derived;
    const int log2 = p->scale_log2;
    const int32_t sub = (1 << log2) - 1;
    uint32_t f = p->static_flags;
    int tlut = (f & RDP_RAST_TLUT) != 0;
    int tlut_type = (f & RDP_RAST_TLUT_TYPE) != 0;
    int sample_quad = (f & RDP_RAST_SAMPLE_QUAD) != 0;
    int cvg_times_alpha = (f & RDP_RAST_CVG_TIMES_ALPHA) != 0;
    int alpha_cvg_select = (f & RDP_RAST_ALPHA_CVG_SELECT) != 0;
    int perspective = (f & RDP_RAST_PERSPECTIVE) != 0;
    int tex_lod_en = (f & RDP_RAST_TEX_LOD) != 0;
    int sharpen_lod_en = (f & RDP_RAST_SHARPEN_LOD) != 0;
    int detail_lod_en = (f & RDP_RAST_DETAIL_LOD) != 0;
    int aa_enable = (f & RDP_RAST_AA) != 0;
    int multi_cycle = (f & RDP_RAST_MULTI_CYCLE) != 0;
    int alpha_test = (f & RDP_RAST_ALPHA_TEST) != 0;
    int alpha_test_dither = (f & RDP_RAST_ALPHA_TEST_DITHER) != 0;
    int mid_texel = (f & RDP_RAST_MID_TEXEL) != 0;
    int uses_texel0 = (f & RDP_RAST_USES_TEXEL0) != 0;
    int uses_texel1 = (f & RDP_RAST_USES_TEXEL1) != 0;
    int uses_pipelined_texel1 = (f & RDP_RAST_USES_PIPELINED_TEXEL1) != 0;
    int uses_next_texel0 = multi_cycle && (f & RDP_RAST_USES_NEXT_TEXEL0) != 0;
    int uses_lod = (f & RDP_RAST_USES_LOD) != 0;
    int uses_st = uses_texel0 || uses_texel1 || uses_pipelined_texel1 || uses_lod;
    int uses_z = (p->depth_blend_flags & (RDP_DB_DEPTH_TEST | RDP_DB_DEPTH_UPDATE)) != 0;
    int need_noise = (f & RDP_RAST_NEED_NOISE) != 0;
    int convert_one = (f & RDP_RAST_CONVERT_ONE) != 0;
    int bilerp0 = (f & RDP_RAST_BILERP0) != 0;
    int bilerp1 = (f & RDP_RAST_BILERP1) != 0;
    int flip = (p->setup.flags & RDP_TRI_FLIP) != 0;
    int32_t direction = flip ? 1 : -1;
    int32_t dither_y = (f & RDP_RAST_INTERLACE_FIELD) ? rdp_asr(y, 1) : y;
    int32_t dither_rgb = p->dither >> 2, dither_alpha = p->dither & 3;
    uint32_t base_tile = p->setup.tile & 7u, max_level = p->setup.tile >> 3;
    const N64RdpSpan *next_span = span + 1;
    int32_t span_end = flip ? span->end_x : span->start_x;
    int next_line = uses_pipelined_texel1 && span->lodlength >= 8 && next_span->valid_line;
    /* The console's pixels of this scanline start at `row`; `pixel` is
     * this pixel's, which at a scale is the same for several. */
    const uint32_t row = st->fb_width * (uint32_t)rdp_asr(y, log2);
    const size_t slice_row = (size_t)(y & sub) << log2;
    uint32_t pixel = row + (uint32_t)rdp_asr(x0, log2);
    int32_t dx = x0 - span->interpolation_base_x;
    RdpQuad rgba, rgba_step, rgba_xoff[4], rgba_yoff[4];
    int32_t stw[3], stw_step[3], stw_dy_step[3], z, z_step, z_dx10, z_dy10;
    int32_t full_lo = 0, full_hi = 0x7fffffff, x;
    RdpTexelFootprint footprint0, footprint1;
    int i;

    memset(&footprint0, 0, sizeof footprint0);
    memset(&footprint1, 0, sizeof footprint1);

    /* The attributes at x0, and their step per pixel. The shade's four
     * channels step together; rgba_xoff[n] and rgba_yoff[n] are n times
     * its x and y derivatives, for the centroid offsets. */
    {
        int32_t start[4], step[4];
        for (i = 0; i < 4; i++) {
            step[i] = rdp_asr(attr->drgba_dx[i] & ~0x1f, log2);
            start[i] = rdp_add(span->rgba[i], rdp_mul(step[i], dx));
        }
        rgba = quad_load(start);
        rgba_step = quad_load(step);
        rgba_xoff[0] = rgba_yoff[0] = quad_zero();
        rgba_xoff[1] = quad_asr(quad_load(attr->drgba_dx), 14);
        rgba_yoff[1] = quad_asr(quad_load(attr->drgba_dy), 14);
        for (i = 2; i < 4; i++) {
            rgba_xoff[i] = quad_add(rgba_xoff[i - 1], rgba_xoff[1]);
            rgba_yoff[i] = quad_add(rgba_yoff[i - 1], rgba_yoff[1]);
        }
    }
    for (i = 0; i < 3; i++) {
        int comp = i == 2 ? 3 : i; /* s, t and w are stzw[0], [1] and [3] */
        stw_step[i] = rdp_asr(attr->dstzw_dx[comp] & ~0x1f, log2);
        stw[i] = rdp_add(span->stzw[comp], rdp_mul(stw_step[i], dx));
        stw_dy_step[i] = rdp_asr(attr->dstzw_dy[comp] & ~0x7fff, log2);
    }
    z_step = attr->dstzw_dx[2];
    z = rdp_add(span->stzw[2], rdp_mul(z_step, dx));
    z_dx10 = rdp_asr(attr->dstzw_dx[2], 10);
    z_dy10 = rdp_asr(attr->dstzw_dy[2], 10);

    /* The pixels with every sample inside the edges: on each sub-scanline,
     * the left sample at or after xleft and the right one before xright
     * (compute_coverage). */
    for (i = 0; i < 4; i++) {
        int32_t left_sample = (i & 1) ? 2 : 0;
        full_lo = rdp_max(full_lo, rdp_asr((int32_t)span->xleft[i] - left_sample + 7, 3));
        full_hi = rdp_min(full_hi, rdp_asr((int32_t)span->xright[i] - (left_sample + 4) - 1, 3));
    }

    for (x = x0; x <= x1; x++) {
        int32_t coverage_count, xoff, yoff;
        int32_t st_[2] = {0, 0}, st_dx[2] = {0, 0}, st_dy[2] = {0, 0}, stw_next[3], zv = 0;
        int perspective_overflow = 0;
        uint32_t tile0 = base_tile, tile1 = (base_tile + 1u) & 7u;
        int32_t lod_frac = 0, rgb_dith, alpha_dith, alpha_reference = 0, alpha, shade_alpha;
        uint16_t noise = 0;
        RdpQuad color;
        RdpShaded shaded;
        RdpPixelMemory mem;

        if (x >= full_lo && x <= full_hi) {
            coverage_count = 8;
            xoff = yoff = 0;
        } else {
            int32_t coverage = compute_coverage(span, x);
            /* Without AA only the first sample counts. */
            if (coverage == 0 || (!aa_enable && (coverage & 1) == 0))
                goto next_pixel;
            coverage_count = rdp_bit_count((uint32_t)coverage);
            centroid_offsets(coverage, &xoff, &yoff);
        }

        if (need_noise) {
            noise = n64_rdp_noise_seed((uint32_t)x, (uint32_t)y, p->primitive_index);
            quad_store(v + RDP_V_NOISE, quad_splat(n64_rdp_noise_combiner(noise)));
        }

        /* interpolate_rgba. */
        {
            RdpQuad snapped = quad_shl(quad_asr(rgba, 14), 2 + log2);
            snapped = quad_add(snapped, quad_add(rgba_xoff[xoff], rgba_yoff[yoff]));
            color = quad_clamp_9bit(quad_asr(snapped, 4 + log2));
            shade_alpha = quad_last(color);
            quad_store(v + RDP_V_SHADE, color);
            quad_store(v + RDP_V_SHADE_ALPHA, quad_splat_last(color));
        }

        /* interpolate_stz. */
        if (uses_st) {
            for (i = 0; i < 3; i++)
                stw_next[i] = rdp_add(stw[i], rdp_mul(direction, stw_step[i]));
            divide_st(stw, perspective, &perspective_overflow, st_);
            if (uses_lod) {
                int32_t stw_dy[3];
                for (i = 0; i < 3; i++)
                    stw_dy[i] = rdp_add(stw[i], stw_dy_step[i]);
                divide_st(stw_next, perspective, &perspective_overflow, st_dx);
                divide_st(stw_dy, perspective, &perspective_overflow, st_dy);
                n64_rdp_compute_lod_2cycle(&tile0, &tile1, &lod_frac, max_level, derived->min_lod, st_, st_dx, st_dy,
                                           perspective_overflow, tex_lod_en, sharpen_lod_en, detail_lod_en);
                quad_store(v + RDP_V_LOD, quad_splat(lod_frac));
            }
        }
        if (uses_z) {
            /* At a scale z is not a sum of equal steps: whole pixels take
             * the derivative, the rest its quantized fraction. */
            const int32_t zx = !log2 ? z
                                     : rdp_add(span->stzw[2], rdp_add(rdp_mul(z_step, rdp_asr(dx, log2)),
                                                                      rdp_mul(rdp_asr(z_step, log2), dx & sub)));
            int32_t snapped_z = rdp_shl(rdp_asr(zx, 10), 2 + log2);
            snapped_z = rdp_add(snapped_z, rdp_add(rdp_mul(xoff, z_dx10), rdp_mul(yoff, z_dy10)));
            zv = clamp_z(rdp_asr(snapped_z, 5 + log2));
        }

        /* The texels. A texel the combiner does not read stays zero in the
         * table. */
        if (uses_texel0) {
            const N64RdpTile *tile = &st->tiles[tile0];
            if (bilerp0 && tile->fmt != RDP_FMT_YUV) {
                RdpQuad texel = quad_from_bytes(n64_rdp_sample_texture_bilerp(
                    rdp, tile, st_[0], st_[1], tlut, tlut_type, sample_quad, mid_texel, &footprint0));
                quad_store(v + RDP_V_TEXEL0, texel);
                quad_store(v + RDP_V_TEXEL0_ALPHA, quad_splat_last(texel));
            } else {
                n64_rdp_sample_texture(rdp, tile, st_[0], st_[1], tlut, tlut_type, sample_quad, mid_texel, 0, bilerp0,
                                       derived->factors, zero, v + RDP_V_TEXEL0);
                quad_store(v + RDP_V_TEXEL0_ALPHA, quad_splat(v[RDP_V_TEXEL0 + 3]));
            }
        }

        /* Texel1 in one-cycle mode peeks at the next pixel, or at the start
         * of the next scanline at the end of a long span. */
        if (uses_pipelined_texel1) {
            int unused_overflow = 0;
            if (x == span_end && next_line) {
                stw_next[0] = next_span->stzw[0];
                stw_next[1] = next_span->stzw[1];
                stw_next[2] = next_span->stzw[3];
            } else if (log2) {
                /* The pixel after this one on the console. */
                for (i = 0; i < 3; i++)
                    stw_next[i] = rdp_add(stw[i], rdp_mul(direction * (sub + 1), stw_step[i]));
            }
            divide_st(stw_next, perspective, &unused_overflow, st_);
            tile1 = tile0;
        }
        if (uses_texel1 || uses_pipelined_texel1) {
            const N64RdpTile *tile = &st->tiles[tile1];
            if (bilerp1 && !convert_one && tile->fmt != RDP_FMT_YUV) {
                RdpQuad texel = quad_from_bytes(n64_rdp_sample_texture_bilerp(
                    rdp, tile, st_[0], st_[1], tlut, tlut_type, sample_quad, mid_texel, &footprint1));
                quad_store(v + RDP_V_TEXEL1, texel);
                quad_store(v + RDP_V_TEXEL1_ALPHA, quad_splat_last(texel));
            } else {
                if (convert_one && !bilerp1)
                    n64_rdp_texture_convert_factors(v + RDP_V_TEXEL0, derived->factors, v + RDP_V_TEXEL1);
                else
                    n64_rdp_sample_texture(rdp, tile, st_[0], st_[1], tlut, tlut_type, sample_quad, mid_texel,
                                           convert_one, bilerp1, derived->factors, v + RDP_V_TEXEL0, v + RDP_V_TEXEL1);
                quad_store(v + RDP_V_TEXEL1_ALPHA, quad_splat(v[RDP_V_TEXEL1 + 3]));
            }
        }

        n64_rdp_dither_coefficients(x, dither_y, dither_rgb, dither_alpha, noise, &rgb_dith, &alpha_dith);

        if (multi_cycle) {
            color = combine_values(v, k->source[0], &alpha);
            if (alpha_test) {
                /* n64_rdp_combiner_cycle0. */
                int32_t clamped_alpha = n64_rdp_clamp_9bit(alpha);
                int32_t expanded_alpha = clamped_alpha + ((clamped_alpha + 1) >> 8);
                if (alpha_cvg_select) {
                    if (cvg_times_alpha)
                        expanded_alpha = (expanded_alpha * coverage_count + 4) >> 3;
                    else
                        expanded_alpha = coverage_count << 5;
                } else {
                    expanded_alpha += alpha_dith;
                }
                alpha_reference = rdp_clamp(expanded_alpha, 0, 0xff);
            }
            quad_store(v + RDP_V_COMBINED, quad_with_last(color, alpha));
            quad_store(v + RDP_V_COMBINED_ALPHA, quad_splat(alpha));
            if (uses_next_texel0) {
                next_texel0(rdp, p, span, x, y, dx, direction, v + RDP_V_NEXT);
                quad_store(v + RDP_V_NEXT_ALPHA, quad_splat(v[RDP_V_NEXT + 3]));
            }
            if (f & RDP_RAST_NEED_NOISE_DUAL) {
                noise = n64_rdp_noise_seed((uint32_t)(x + 1023), (uint32_t)(y + 7), p->primitive_index + 11u);
                quad_store(v + RDP_V_NOISE, quad_splat(n64_rdp_noise_combiner(noise)));
            }
        }
        {
            /* n64_rdp_combiner_cycle1. */
            int32_t expanded_alpha, modulated_alpha;
            color = quad_clamp_9bit(combine_values(v, k->source[1], &alpha));
            alpha = n64_rdp_clamp_9bit(alpha);
            expanded_alpha = alpha + ((alpha + 1) >> 8);
            if (cvg_times_alpha) {
                modulated_alpha = (expanded_alpha * coverage_count + 4) >> 3;
                coverage_count = modulated_alpha >> 5;
            } else {
                modulated_alpha = coverage_count << 5;
            }
            if (alpha_cvg_select)
                expanded_alpha = modulated_alpha;
            else
                expanded_alpha += alpha_dith;
            alpha = rdp_clamp(expanded_alpha, 0, 0xff);
        }
        if (!multi_cycle)
            alpha_reference = alpha;

        /* Alpha-to-coverage can take the coverage to zero. */
        if (aa_enable && coverage_count == 0)
            goto next_pixel;
        if (alpha_test) {
            int32_t threshold = alpha_test_dither ? n64_rdp_noise_blend_threshold(noise) : derived->blend_color[3];
            if (alpha_reference < threshold)
                goto next_pixel;
        }

        {
            uint32_t bytes = quad_to_bytes(quad_with_last(color, alpha));
            shaded.combined[0] = (uint8_t)bytes;
            shaded.combined[1] = (uint8_t)(bytes >> 8);
            shaded.combined[2] = (uint8_t)(bytes >> 16);
            shaded.combined[3] = (uint8_t)(bytes >> 24);
        }
        shaded.z_dith = (zv << 9) | rgb_dith;
        shaded.coverage_count = coverage_count;
        shaded.shade_alpha = rdp_min(shade_alpha + alpha_dith, 0xff);

        /* n64_rdp_pixel_load, n64_rdp_depth_blend, n64_rdp_pixel_store. */
        {
            /* The slice this pixel's words are in: 0 for the console's own. */
            const size_t slice = slice_row + (size_t)(x & sub);
            uint8_t *const color_at = k->color + slice * k->slice_stride + 2u * (size_t)pixel;
            uint8_t *const depth_at = k->depth + slice * k->slice_stride + 2u * (size_t)pixel;
            uint8_t *const color_hidden_at = k->color_hidden + slice * k->slice_hidden_stride + pixel;
            uint8_t *const depth_hidden_at = k->depth_hidden + slice * k->slice_hidden_stride + pixel;
            uint32_t word = n64_be16(color_at);
            mem.color[0] = (uint8_t)((word >> 8) & 0xf8u);
            mem.color[1] = (uint8_t)((word >> 3) & 0xf8u);
            mem.color[2] = (uint8_t)((word << 2) & 0xf8u);
            mem.color[3] = (uint8_t)((*color_hidden_at << 5) | ((word & 1u) << 7));
            word = n64_be16(depth_at);
            mem.depth = (uint16_t)(word >> 2);
            mem.dz = (uint8_t)(*depth_hidden_at | ((word & 3u) << 2));
            mem.color_dirty = 0;
            mem.depth_dirty = 0;
            mem.color_index = mem.depth_index = 0; /* unused: the words are addressed directly */
            n64_rdp_depth_blend(rdp, p, &mem, &shaded);
            if (mem.color_dirty) {
                uint32_t cov = (uint32_t)mem.color[3] >> 5;
                word = ((uint32_t)(mem.color[0] & 0xf8u) << 8) | ((uint32_t)(mem.color[1] & 0xf8u) << 3) |
                       ((uint32_t)(mem.color[2] & 0xf8u) >> 2) | (cov >> 2);
                n64_put_be16(color_at, (uint16_t)word);
                *color_hidden_at = (uint8_t)(cov & 3u);
            }
            if (mem.depth_dirty) {
                n64_put_be16(depth_at, (uint16_t)(((uint32_t)mem.depth << 2) | ((uint32_t)mem.dz >> 2)));
                *depth_hidden_at = (uint8_t)(mem.dz & 3u);
            }
        }

    next_pixel:
        dx++;
        pixel = row + (uint32_t)rdp_asr(x + 1, log2);
        rgba = quad_add(rgba, rgba_step);
        for (i = 0; i < 3; i++)
            stw[i] = rdp_add(stw[i], stw_step[i]);
        z = rdp_add(z, z_step);
    }
}

/* The fill pipe over pixels x0..x1 of scanline y, written straight into
 * RDRAM: what n64_rdp_fill_color() and n64_rdp_pixel_store() leave there,
 * pixel by pixel. Returns 0, having written nothing, when part of the run
 * lies outside RDRAM; the caller then goes pixel by pixel. */
static int fill_row(N64Rdp *rdp, const RdpPrimitive *p, int32_t y, int32_t x0, int32_t x1)
{
    const N64RdpState *st = rdp->state;
    uint8_t *rdram = rdp->links.rdram, *hidden = rdp->links.hidden;
    uint32_t col = p->derived.fill_color;
    uint32_t pixel = st->fb_width * (uint32_t)y + (uint32_t)x0, count = (uint32_t)(x1 - x0) + 1u, index, i;
    uint32_t limit = rdram_limit(rdp);

    switch (st->fb_fmt) {
    case N64_RDP_FB_RGBA5551:
    case N64_RDP_FB_IA88:
        /* Both store the 16-bit half of the fill colour as it is, and keep
         * its low bit in the hidden bits. */
        index = (st->fb_addr >> 1) + pixel;
        if (2u * (index + count) > limit)
            return 0;
        for (i = 0; i < count; i++, index++) {
            uint32_t word = (index & 1u) ? col & 0xffffu : col >> 16;
            rdram[2u * index] = (uint8_t)(word >> 8);
            rdram[2u * index + 1u] = (uint8_t)word;
            hidden[index] = (uint8_t)((word & 1u) * 3u);
        }
        return 1;
    case N64_RDP_FB_RGBA8888:
        index = (st->fb_addr >> 2) + pixel;
        if (4u * (index + count) > limit)
            return 0;
        for (i = 0; i < count; i++, index++) {
            n64_put_be32(rdram + 4u * index, col);
            hidden[2u * index] = (uint8_t)(((col >> 16) & 1u) * 3u);
            hidden[2u * index + 1u] = (uint8_t)((col & 1u) * 3u);
        }
        return 1;
    case N64_RDP_FB_I8:
        index = st->fb_addr + pixel;
        if (index + count > limit)
            return 0;
        for (i = 0; i < count; i++, index++) {
            uint8_t byte = (uint8_t)(col >> (((index & 3u) ^ 3u) * 8u));
            rdram[index] = byte;
            if (index & 1u)
                hidden[index >> 1] = (uint8_t)((byte & 1u) * 3u);
        }
        return 1;
    default:
        /* A 4-bit colour image is not written. */
        return 1;
    }
}

/* Draw scanlines y_first..y_last of a primitive whose spans are set up.
 * Only the pixels' own memory is written, so the scanlines of a primitive
 * that rows_independent() accepts can be drawn in any order and at once. */
static void draw_rows(N64Rdp *rdp, const RdpPrimitive *p, int32_t y_first, int32_t y_last)
{
    const N64RdpState *st = rdp->state;
    int fill = (p->static_flags & RDP_RAST_FILL) != 0 && !n64_rdp_reference_path();
    int32_t values[RDP_V_COUNT];
    int32_t y;

    if (p->fast.enabled)
        memcpy(values, p->fast.values, sizeof values);

    for (y = y_first; y <= y_last; y++) {
        const N64RdpSpan *span = &p->spans[y - p->span_ylo];
        int32_t x, x0, x1;
        if (!span->valid_line)
            continue;
        /* Pixels outside [start_x, end_x] have no coverage; parallel-rdp
         * shades only the colour image's width. */
        x0 = rdp_max(span->start_x, 0);
        x1 = rdp_min(span->end_x, (int32_t)st->fb_width - 1);
        if (x1 < x0)
            continue;
        if (fill && fill_row(rdp, p, y, x0, x1))
            continue;
        if (p->fast.enabled) {
            shade_row(rdp, p, y, x0, x1, values);
            continue;
        }
        for (x = x0; x <= x1; x++) {
            RdpShaded shaded;
            RdpPixelMemory mem;
            if (!shade_pixel(rdp, p, x, y, &shaded))
                continue;
            n64_rdp_pixel_load(rdp, &mem, (uint32_t)x, (uint32_t)y);
            if (shaded.coverage_count & RDP_COVERAGE_FILL)
                n64_rdp_fill_color(rdp, &mem, p->derived.fill_color);
            else if (shaded.coverage_count & RDP_COVERAGE_COPY)
                n64_rdp_copy_pipeline(rdp, &mem, (uint32_t)shaded.z_dith);
            else
                n64_rdp_depth_blend(rdp, p, &mem, &shaded);
            n64_rdp_pixel_store(rdp, &mem);
        }
    }
}

/* Whether no two scanlines of the primitive touch the same memory. A pixel
 * reads and writes its own colour and depth words and their hidden bits,
 * and nothing else in RDRAM, so scanlines are independent unless the colour
 * and depth images overlap with pixels out of step, an image wraps round
 * the 24-bit address space, or the colour image is 4 or 8 bits wide, where
 * two neighbouring pixels share one byte of hidden bits. */
static int rows_independent(const N64Rdp *rdp, const RdpPrimitive *p)
{
    const N64RdpState *st = rdp->state;
    uint32_t color[2], depth[2];
    if (!image_ranges(st, p, color, depth) || color[1] > 0x1000000u || depth[1] > 0x1000000u)
        return 0;
    if (color[1] <= depth[0] || depth[1] <= color[0])
        return 1;
    /* A depth image on the very words of a 16-bit colour image: each pixel
     * still has its word to itself. */
    return st->fb_fmt != N64_RDP_FB_RGBA8888 && (st->fb_addr >> 1) == (st->depth_addr >> 1);
}

/* A primitive shared out between threads, a few scanlines at a time. */
typedef struct RdpRowsJob {
    N64Rdp *rdp;
    const RdpPrimitive *prim;
    int32_t rows_per_piece;
    long next_piece;
} RdpRowsJob;

static void draw_rows_piece(void *job_, unsigned thread)
{
    RdpRowsJob *job = (RdpRowsJob *)job_;
    const RdpPrimitive *p = job->prim;
    (void)thread;
    for (;;) {
        int32_t y = p->span_ylo + (int32_t)n64_rdp_workers_take(&job->next_piece) * job->rows_per_piece;
        if (y > p->span_yhi)
            return;
        draw_rows(job->rdp, p, y, rdp_min(y + job->rows_per_piece - 1, p->span_yhi));
    }
}

/* Thread thresholds: see n64_rdp_draw_primitive(). */
#define RDP_SPLIT_PIXELS 512
#define RDP_PIXELS_PER_THREAD 256
#define RDP_LARGE_PIXELS 2048

/* ------------------------------------------------------------------ */
/* The larger picture                                                  */
/* ------------------------------------------------------------------ */

/*
 * n64_rdp_set_scale(): every primitive drawn a second time, two or four
 * times as large each way. This is parallel-rdp's upscaling, whose factor
 * the shaders carry everywhere (SCALING_LOG2) and this port had fixed at 1:
 * the same span setup and pixel pipeline on a finer grid, with attributes
 * stepped by a fraction of their derivatives and kept exact at the
 * console's own pixels.
 *
 * The console's picture is drawn as it always was, into RDRAM, by the same
 * code as before: nothing a game can read changes. The larger one goes
 * into a shadow of RDRAM that has scale * scale copies of it ("slices"),
 * and of the hidden bits. Sample (sx, sy) of the pixel at an address is at
 * that same address in slice sy * scale + sx, so an image anywhere in
 * RDRAM, of any width, has its larger twin without anything being told
 * about it, and the memory stage works on a slice as it does on RDRAM: it
 * is handed an N64Rdp whose links point at the slice.
 *
 * What the shadow cannot know is what the game writes into RDRAM itself: a
 * picture decoded by the CPU, a framebuffer cleared with a loop. So a third
 * copy, `reference`, holds RDRAM as it was when the slices were last in
 * step with it. Before a primitive is drawn larger, each pixel it may touch
 * whose bytes differ from the reference is brought into step: every slice
 * gets the console's pixel. After the console's own pass the reference
 * takes those pixels as RDRAM now has them. Whoever shows the picture
 * (n64_vi.c) does the same comparison: a pixel that differs from the
 * reference is the console's own, whatever the slices hold.
 *
 * Rectangles are not drawn larger (parallel-rdp marks them
 * TRIANGLE_SETUP_DISABLE_UPSCALING): each slice gets the console's
 * rectangle, by the console's own code. Triangles are drawn at the scale by
 * shade_row() where the console's own triangle would use it, and otherwise
 * by the pixel-by-pixel port of the shaders.
 */
/*
 * Drawing larger is several times the console's work, and nothing the game
 * does waits on it, so primitives are not drawn larger as they come. Each
 * is put in a queue with what it is drawn from - its spans at the scale,
 * the RDP's state (TMEM, the tiles) as it was, the pixels to bring into
 * step - and the queue is replayed later: when a picture is complete
 * (SYNC_FULL), when it is full, or when someone wants the slices
 * (n64_rdp_larger_view). A replay is by bands of the console's scanlines,
 * each band on its own thread drawing the queue's primitives in order, so
 * the slices end as they would have had each primitive been drawn when it
 * came: no two bands share a byte, which is what a queue's primitives are
 * checked for as they join it (queue_takes). A complete picture is replayed
 * on a background thread while the game runs on; whoever wants the slices
 * waits for it.
 *
 * The reference is kept by the thread that runs the RDP, as primitives come:
 * what is in step never depends on when the slices are written.
 */
#define LARGER_BAND 8 /* the console's scanlines to a band */
#define LARGER_ENTRIES 4096u
#define LARGER_SPANS (192u * 1024u)
#define LARGER_STATES 512u
#define LARGER_BYTES ((size_t)8 << 20)
#define LARGER_IMAGES 4
/* What of the RDP's state a primitive is drawn from, and how much of that
 * says two primitives are drawn from the same. */
#define LARGER_STATE_BYTES ((offsetof(N64RdpState, cmd_buf) + 15u) & ~(size_t)15u)
#define LARGER_STATE_SAME offsetof(N64RdpState, primitive_count)

/* The bytes of an image a queue's primitives touch. */
typedef struct RdpLargerImage {
    uint32_t base, row_bytes; /* where its first scanline is, and how long a scanline */
    uint32_t lo, hi;
} RdpLargerImage;

/* Pixels brought into step before a primitive is drawn: in a queue's
 * bytes, followed by `bytes` of RDRAM and half as many hidden bits. */
typedef struct RdpLargerRun {
    int32_t y; /* the console's scanline they are on */
    uint32_t addr, bytes;
} RdpLargerRun;

typedef struct RdpLargerEntry {
    RdpPrimitive prim;    /* at the scale; or the console's own, for one not drawn larger */
    const uint8_t *state; /* the RDP's state it is drawn from */
    size_t runs, runs_end;
    int32_t first, last; /* the console's scanlines */
    int whole;
} RdpLargerEntry;

struct RdpLarger;
typedef struct RdpLargerQueue {
    struct RdpLarger *larger;
    RdpLargerEntry *entries;
    N64RdpSpan *spans;
    uint8_t *states, *bytes;
    uint32_t count, span_count, state_count;
    size_t byte_count;
    int32_t first, last; /* the scanlines its entries have */
    RdpLargerImage images[LARGER_IMAGES];
    int image_count;
    N64Rdp rdp; /* the RDP's links, for the replay */
    long next_band;
} RdpLargerQueue;

typedef struct RdpLarger {
    int log2;           /* 1 or 2 */
    uint32_t slices;    /* scale * scale */
    uint8_t *rdram;     /* the slices, one after another, each rdram_size bytes */
    uint8_t *hidden;    /* ... and their hidden bits, each rdram_size / 2 */
    uint8_t *reference; /* RDRAM when the slices were last in step with it */
    /* One queue fills while the other is replayed. */
    RdpLargerQueue queue[2];
    int filling;
    RdpAsync *async;
    /* For each of the console's scanlines the primitive crosses, the first
     * and last pixel either picture may touch. */
    int16_t first[N64_RDP_MAX_SPANS], last[N64_RDP_MAX_SPANS];
} RdpLarger;

static void queue_reset(RdpLargerQueue *q)
{
    q->count = q->span_count = q->state_count = 0;
    q->byte_count = 0;
    q->first = 0x7fffffff;
    q->last = -1;
    q->image_count = 0;
}

static void larger_free(N64Rdp *rdp)
{
    RdpLarger *l = (RdpLarger *)rdp->links.larger;
    int i;
    if (!l)
        return;
    /* What is still queued is not drawn: nothing is left to draw it in. */
    n64_rdp_async_stop(l->async);
    for (i = 0; i < 2; i++) {
        free(l->queue[i].entries);
        free(l->queue[i].spans);
        free(l->queue[i].states);
        free(l->queue[i].bytes);
    }
    free(l->rdram);
    free(l->hidden);
    free(l->reference);
    free(l);
    rdp->links.larger = NULL;
}

int n64_rdp_scale(const N64Rdp *rdp)
{
    const RdpLarger *l = rdp ? (const RdpLarger *)rdp->links.larger : NULL;
    return l ? 1 << l->log2 : 1;
}

bool n64_rdp_set_scale(N64Rdp *rdp, int scale)
{
    RdpLarger *l;
    size_t size;
    int i, ok;
    if (!rdp)
        return false;
    larger_free(rdp);
    if (scale <= 1)
        return true;
    if ((scale != 2 && scale != 4) || !rdp->links.rdram || !rdp->links.hidden)
        return false;
    size = rdp->links.rdram_size;
    l = (RdpLarger *)calloc(1, sizeof *l);
    if (!l)
        return false;
    rdp->links.larger = l;
    l->log2 = scale == 2 ? 1 : 2;
    l->slices = (uint32_t)(scale * scale);
    l->rdram = (uint8_t *)calloc(l->slices, size);
    l->hidden = (uint8_t *)malloc(l->slices * (size / 2));
    /* Nothing is in step to begin with unless RDRAM is all zeroes, which
     * is what the slices are. */
    l->reference = (uint8_t *)calloc(1, size);
    ok = l->rdram && l->hidden && l->reference;
    for (i = 0; i < 2; i++) {
        RdpLargerQueue *q = &l->queue[i];
        q->larger = l;
        q->entries = (RdpLargerEntry *)malloc(LARGER_ENTRIES * sizeof *q->entries);
        q->spans = (N64RdpSpan *)malloc(LARGER_SPANS * sizeof *q->spans);
        q->states = (uint8_t *)malloc(LARGER_STATES * LARGER_STATE_BYTES);
        q->bytes = (uint8_t *)malloc(LARGER_BYTES);
        queue_reset(q);
        ok = ok && q->entries && q->spans && q->states && q->bytes;
    }
    if (!ok) {
        larger_free(rdp);
        return false;
    }
    /* Hidden bits start as the console's do (n64_rdp_reset). */
    memset(l->hidden, 0x03, l->slices * (size / 2));
    /* Without the thread a complete picture is drawn where it is found. */
    l->async = n64_rdp_async_start();
    return true;
}

static void draw_rows(N64Rdp *rdp, const RdpPrimitive *p, int32_t y_first, int32_t y_last);
static int rows_independent(const N64Rdp *rdp, const RdpPrimitive *p);

/* The console's scanlines first to last of a primitive, drawn larger:
 * by the row code where the console's own primitive would use it, and
 * otherwise pixel by pixel through the shaders' port, each pixel's memory
 * in its slice. `views` are the slices as the memory stage sees them. */
static void draw_rows_larger(N64Rdp *views, const RdpPrimitive *q, int32_t first, int32_t last)
{
    const N64RdpState *st = views[0].state;
    const int log2 = q->scale_log2;
    const int32_t scale = 1 << log2, mask = scale - 1;
    const int32_t width = (int32_t)st->fb_width << log2;
    int32_t values[RDP_V_COUNT];
    int32_t y;
    if (q->fast.enabled)
        memcpy(values, q->fast.values, sizeof values);
    for (y = first * scale; y <= last * scale + mask; y++) {
        const N64RdpSpan *span = &q->spans[y - q->span_ylo];
        int32_t x, x0, x1;
        if (!span->valid_line)
            continue;
        x0 = rdp_max(span->start_x, 0);
        x1 = rdp_min(span->end_x, width - 1);
        if (q->fast.enabled) {
            if (x1 >= x0)
                shade_row(&views[0], q, y, x0, x1, values);
            continue;
        }
        for (x = x0; x <= x1; x++) {
            N64Rdp *view = &views[(y & mask) * scale + (x & mask)];
            RdpShaded shaded;
            RdpPixelMemory mem;
            if (!shade_pixel(view, q, x, y, &shaded))
                continue;
            n64_rdp_pixel_load(view, &mem, (uint32_t)(x >> log2), (uint32_t)(y >> log2));
            if (shaded.coverage_count & RDP_COVERAGE_FILL)
                n64_rdp_fill_color(view, &mem, q->derived.fill_color);
            else if (shaded.coverage_count & RDP_COVERAGE_COPY)
                n64_rdp_copy_pipeline(view, &mem, (uint32_t)shaded.z_dith);
            else
                n64_rdp_depth_blend(view, q, &mem, &shaded);
            n64_rdp_pixel_store(view, &mem);
        }
    }
}

/* A queue's primitives, in order, on the console's scanlines b0 to b1. */
static void larger_replay(const RdpLargerQueue *q, int32_t b0, int32_t b1)
{
    const RdpLarger *l = q->larger;
    const size_t size = q->rdp.links.rdram_size, half = size / 2;
    N64Rdp views[16];
    uint32_t i, slice;
    for (slice = 0; slice < l->slices; slice++) {
        views[slice] = q->rdp;
        views[slice].links.rdram = l->rdram + slice * size;
        views[slice].links.hidden = l->hidden + slice * half;
    }
    for (i = 0; i < q->count; i++) {
        const RdpLargerEntry *e = &q->entries[i];
        const int32_t lo = rdp_max(e->first, b0), hi = rdp_min(e->last, b1);
        size_t at;
        if (hi < lo)
            continue;
        /* The pixels the game had written: every sample is the console's. */
        for (at = e->runs; at < e->runs_end;) {
            RdpLargerRun run;
            memcpy(&run, q->bytes + at, sizeof run);
            at += sizeof run;
            if (run.y >= lo && run.y <= hi) {
                for (slice = 0; slice < l->slices; slice++) {
                    memcpy(l->rdram + slice * size + run.addr, q->bytes + at, run.bytes);
                    memcpy(l->hidden + slice * half + (run.addr >> 1), q->bytes + at + run.bytes, run.bytes >> 1);
                }
            }
            at += run.bytes + (run.bytes >> 1);
        }
        for (slice = 0; slice < l->slices; slice++)
            views[slice].state = (N64RdpState *)e->state;
        if (e->whole) {
            /* The console's own primitive, in each slice. */
            for (slice = 0; slice < l->slices; slice++) {
                RdpPrimitive own = e->prim;
                fast_setup(&views[slice], &own);
                draw_rows(&views[slice], &own, lo, hi);
            }
        } else {
            draw_rows_larger(views, &e->prim, lo, hi);
        }
    }
}

static void larger_replay_bands(void *queue, unsigned thread)
{
    RdpLargerQueue *q = (RdpLargerQueue *)queue;
    const int32_t first_band = q->first / LARGER_BAND;
    (void)thread;
    for (;;) {
        const int32_t y = (first_band + (int32_t)n64_rdp_workers_take(&q->next_band)) * LARGER_BAND;
        if (y > q->last)
            return;
        larger_replay(q, y, y + LARGER_BAND - 1);
    }
}

/* Draw what a queue holds, and empty it. */
static void larger_flush(void *queue, unsigned thread)
{
    RdpLargerQueue *q = (RdpLargerQueue *)queue;
    (void)thread;
    if (q->count) {
        const unsigned threads = n64_rdp_workers_threads();
        q->next_band = 0;
        if (threads < 2u || n64_rdp_workers_run(larger_replay_bands, q, threads - 1u, 1) != 0)
            larger_replay_bands(q, 0);
    }
    queue_reset(q);
}

/* Everything queued is drawn: the slices are what they would be had each
 * primitive been drawn as it came. */
static void larger_sync(RdpLarger *l)
{
    if (l->async)
        n64_rdp_async_wait(l->async);
    larger_flush(&l->queue[l->filling], 0);
}

/* What is queued goes to the background thread, and the other queue, which
 * that thread has emptied, fills from here on. */
static void larger_kick(RdpLarger *l)
{
    RdpLargerQueue *q = &l->queue[l->filling];
    if (!q->count)
        return;
    if (!l->async || n64_rdp_workers_threads() < 2u) {
        larger_flush(q, 0);
        return;
    }
    n64_rdp_async_wait(l->async);
    l->filling ^= 1;
    n64_rdp_async_run(l->async, larger_flush, q);
}

void n64_rdp_larger_kick(N64Rdp *rdp)
{
    if (rdp->links.larger)
        larger_kick((RdpLarger *)rdp->links.larger);
}

/* While the background thread replays a queue the helpers are its. */
static int larger_replaying(const N64Rdp *rdp)
{
    const RdpLarger *l = (const RdpLarger *)rdp->links.larger;
    return l && l->async && n64_rdp_async_busy(l->async);
}

const uint8_t *n64_rdp_larger_pixel(const N64Rdp *rdp, uint32_t addr, uint32_t bytes, size_t *stride)
{
    RdpLarger *l = rdp ? (RdpLarger *)rdp->links.larger : NULL;
    if (!l || addr > rdp->links.rdram_size || bytes > rdp->links.rdram_size - addr)
        return NULL;
    if (memcmp(l->reference + addr, rdp->links.rdram + addr, bytes) != 0)
        return NULL;
    larger_sync(l);
    if (stride)
        *stride = rdp->links.rdram_size;
    return l->rdram + addr;
}

static int queue_touches(const RdpLargerQueue *q, uint32_t lo, uint32_t hi)
{
    int i;
    for (i = 0; i < q->image_count; i++)
        if (q->images[i].lo < hi && lo < q->images[i].hi)
            return 1;
    return 0;
}

bool n64_rdp_larger_view(const N64Rdp *rdp, N64RdpLargerView *view)
{
    return n64_rdp_larger_view_of(rdp, view, 0, 0xffffffffu);
}

bool n64_rdp_larger_view_of(const N64Rdp *rdp, N64RdpLargerView *view, uint32_t lo, uint32_t hi)
{
    RdpLarger *l = rdp ? (RdpLarger *)rdp->links.larger : NULL;
    if (!l || !view)
        return false;
    /* What is being queued comes after what is being replayed, so it is
     * drawn only once that is; a queue that touches none of these bytes
     * leaves them as they are whenever it is drawn. (A queue being replayed
     * keeps its images until the replay is over, and then has none.) */
    if (queue_touches(&l->queue[l->filling], lo, hi))
        larger_sync(l);
    else if (l->async && n64_rdp_async_busy(l->async) && queue_touches(&l->queue[l->filling ^ 1], lo, hi))
        n64_rdp_async_wait(l->async);
    view->log2 = l->log2;
    view->rdram = l->rdram;
    view->hidden = l->hidden;
    view->reference = l->reference;
    view->rdram_stride = rdp->links.rdram_size;
    view->hidden_stride = rdp->links.rdram_size / 2;
    return true;
}

/* Whether a queue can take a primitive that touches these two images
 * (colour and depth): afterwards, as before, two of the queue's images
 * either start at the same byte with scanlines of the same length - then a
 * scanline of one is that scanline of the other - or share no byte. Either
 * way a band's bytes are no other band's. */
static int queue_takes(RdpLargerQueue *q, const RdpLargerImage images[2])
{
    RdpLargerImage set[LARGER_IMAGES + 2];
    int count = q->image_count, i, j;
    memcpy(set, q->images, sizeof q->images);
    for (i = 0; i < 2; i++) {
        for (j = 0; j < count; j++)
            if (set[j].base == images[i].base && set[j].row_bytes == images[i].row_bytes)
                break;
        if (j == count) {
            set[count++] = images[i];
        } else {
            if (images[i].lo < set[j].lo)
                set[j].lo = images[i].lo;
            if (images[i].hi > set[j].hi)
                set[j].hi = images[i].hi;
        }
    }
    if (count > LARGER_IMAGES)
        return 0;
    for (i = 0; i < count; i++)
        for (j = i + 1; j < count; j++)
            if (set[i].lo < set[j].hi && set[j].lo < set[i].hi)
                return 0;
    memcpy(q->images, set, sizeof q->images);
    q->image_count = count;
    return 1;
}

/* The bytes of a queue it takes to bring `count` units of `unit` bytes at
 * `addr` into step. */
static size_t larger_stale(const N64Rdp *rdp, const RdpLarger *l, uint32_t addr, uint32_t unit, uint32_t count)
{
    const uint8_t *rdram = rdp->links.rdram;
    size_t need = 0;
    uint32_t i = 0;
    if (!memcmp(l->reference + addr, rdram + addr, (size_t)unit * count))
        return 0;
    while (i < count) {
        uint32_t from;
        while (i < count && !memcmp(l->reference + addr + i * unit, rdram + addr + i * unit, unit))
            i++;
        from = i;
        while (i < count && memcmp(l->reference + addr + i * unit, rdram + addr + i * unit, unit) != 0)
            i++;
        if (i > from)
            need += sizeof(RdpLargerRun) + (size_t)(i - from) * unit * 3u / 2u;
    }
    return need;
}

/* Bring `count` units of `unit` bytes at `addr`, on scanline y, into step:
 * where RDRAM is not what the reference has, the replay gives every slice
 * RDRAM's bytes and the console's hidden bits as they are now, and the
 * reference takes RDRAM's bytes. */
static void larger_step_in(const N64Rdp *rdp, RdpLarger *l, RdpLargerQueue *q, int32_t y, uint32_t addr,
                           uint32_t unit, uint32_t count)
{
    const uint8_t *rdram = rdp->links.rdram;
    uint32_t i = 0;
    if (!memcmp(l->reference + addr, rdram + addr, (size_t)unit * count))
        return;
    while (i < count) {
        RdpLargerRun run;
        uint32_t from;
        while (i < count && !memcmp(l->reference + addr + i * unit, rdram + addr + i * unit, unit))
            i++;
        from = i;
        while (i < count && memcmp(l->reference + addr + i * unit, rdram + addr + i * unit, unit) != 0)
            i++;
        if (i == from)
            break;
        run.y = y;
        run.addr = addr + from * unit;
        run.bytes = (i - from) * unit;
        memcpy(q->bytes + q->byte_count, &run, sizeof run);
        q->byte_count += sizeof run;
        memcpy(q->bytes + q->byte_count, rdram + run.addr, run.bytes);
        q->byte_count += run.bytes;
        memcpy(q->bytes + q->byte_count, rdp->links.hidden + (run.addr >> 1), run.bytes >> 1);
        q->byte_count += run.bytes >> 1;
        memcpy(l->reference + run.addr, rdram + run.addr, run.bytes);
    }
}

/*
 * The larger picture of primitive p, whose own spans are set up: queued,
 * with the pixels it may touch brought into step. Returns whether it was,
 * in which case larger_done() is owed after the console's own pass.
 */
static int larger_draw(N64Rdp *rdp, const RdpPrimitive *p)
{
    RdpLarger *l = (RdpLarger *)rdp->links.larger;
    const N64RdpState *st = rdp->state;
    const int32_t scale = 1 << l->log2;
    const int32_t rows = p->span_yhi - p->span_ylo + 1;
    const uint32_t pixel_bytes = st->fb_fmt == N64_RDP_FB_RGBA8888 ? 4u : 2u;
    const int whole = (p->setup.flags & RDP_TRI_DISABLE_UPSCALING) != 0;
    uint32_t color[2], depth[2], limit = rdram_limit(rdp), span_need;
    RdpLargerImage images[2];
    RdpLargerQueue *q;
    RdpLargerEntry *e;
    N64RdpSpan *spans;
    size_t need = 0;
    int32_t y;
    int alone;

    /* A 16 or 32-bit colour image wholly inside RDRAM, as the fast path
     * asks; anything else has no larger picture, and its pixels fall out
     * of step. */
    if (!image_ranges(st, p, color, depth) || color[1] > limit || depth[1] > limit || rows > N64_RDP_MAX_SPANS - 1)
        return 0;
    span_need = (uint32_t)(whole ? rows + 1 : rows * scale + 1);
    images[0].base = pixel_bytes * (st->fb_addr / pixel_bytes);
    images[0].row_bytes = st->fb_width * pixel_bytes;
    images[0].lo = color[0];
    images[0].hi = color[1];
    images[1].base = 2u * (st->depth_addr >> 1);
    images[1].row_bytes = st->fb_width * 2u;
    images[1].lo = depth[0];
    images[1].hi = depth[1];

    /* A primitive whose scanlines share bytes with each other is drawn by
     * itself, after everything before it. */
    alone = !rows_independent(rdp, p);
    q = &l->queue[l->filling];
    if (alone) {
        larger_sync(l);
    } else if (q->count == LARGER_ENTRIES || q->state_count == LARGER_STATES ||
               q->span_count + span_need > LARGER_SPANS || !queue_takes(q, images)) {
        larger_kick(l);
        q = &l->queue[l->filling];
        if (!queue_takes(q, images))
            return 0;
    }

    e = &q->entries[q->count];
    spans = q->spans + q->span_count;
    e->prim = *p;
    e->whole = whole;
    e->first = p->span_ylo;
    e->last = p->span_yhi;
    if (whole) {
        memcpy(spans, p->spans, span_need * sizeof *spans);
    } else {
        /* The fast path, if the console's own primitive would take it: the
         * same images, in slice 0, and the way to the other slices. */
        fast_setup(rdp, &e->prim);
        if (e->prim.fast.enabled) {
            e->prim.fast.color = l->rdram + 2u * (st->fb_addr >> 1);
            e->prim.fast.color_hidden = l->hidden + (st->fb_addr >> 1);
            e->prim.fast.depth = l->rdram + 2u * (st->depth_addr >> 1);
            e->prim.fast.depth_hidden = l->hidden + (st->depth_addr >> 1);
            e->prim.fast.slice_stride = rdp->links.rdram_size;
            e->prim.fast.slice_hidden_stride = rdp->links.rdram_size / 2;
        }
        e->prim.scale_log2 = l->log2;
        e->prim.spans = spans;
        e->prim.span_ylo = p->span_ylo * scale;
        e->prim.span_yhi = p->span_yhi * scale + (scale - 1);
        for (y = e->prim.span_ylo; y <= e->prim.span_yhi + 1; y++)
            span_setup(&e->prim, y, &spans[y - e->prim.span_ylo]);
    }

    /* The pixels either picture may touch on each of the console's lines,
     * and what it takes to bring them into step. */
    for (y = 0; y < rows; y++) {
        const N64RdpSpan *own = &p->spans[y];
        int32_t lo = 0x7fff, hi = -1, k;
        if (own->valid_line) {
            lo = own->start_x;
            hi = own->end_x;
        }
        for (k = 0; k < scale && !whole; k++) {
            const N64RdpSpan *span = &spans[y * scale + k];
            if (!span->valid_line)
                continue;
            lo = rdp_min(lo, rdp_asr(span->start_x, l->log2));
            hi = rdp_max(hi, rdp_asr(span->end_x, l->log2));
        }
        lo = rdp_max(lo, 0);
        hi = rdp_min(hi, (int32_t)st->fb_width - 1);
        l->first[y] = (int16_t)lo;
        l->last[y] = (int16_t)hi;
        if (hi >= lo) {
            const uint32_t pixel = st->fb_width * (uint32_t)(p->span_ylo + y) + (uint32_t)lo;
            const uint32_t count = (uint32_t)(hi - lo + 1);
            need += larger_stale(rdp, l, pixel_bytes * ((st->fb_addr / pixel_bytes) + pixel), pixel_bytes, count);
            need += larger_stale(rdp, l, 2u * ((st->depth_addr >> 1) + pixel), 2u, count);
        }
    }
    if (need > LARGER_BYTES - q->byte_count) {
        if (need > LARGER_BYTES)
            return 0;
        /* No room: draw what is queued - this primitive is not yet part of
         * it - and start the queue again with this one. */
        larger_sync(l);
        memmove(q->spans, spans, span_need * sizeof *spans);
        spans = q->spans;
        if (e != &q->entries[0])
            q->entries[0] = *e;
        e = &q->entries[0];
        if (!alone && !queue_takes(q, images))
            return 0;
    }
    /* After any move: the spans are where they will stay. */
    e->prim.spans = spans;
    e->runs = q->byte_count;
    for (y = 0; y < rows; y++) {
        if (l->last[y] >= l->first[y]) {
            const uint32_t pixel = st->fb_width * (uint32_t)(p->span_ylo + y) + (uint32_t)l->first[y];
            const uint32_t count = (uint32_t)(l->last[y] - l->first[y] + 1);
            larger_step_in(rdp, l, q, p->span_ylo + y, pixel_bytes * ((st->fb_addr / pixel_bytes) + pixel),
                           pixel_bytes, count);
            larger_step_in(rdp, l, q, p->span_ylo + y, 2u * ((st->depth_addr >> 1) + pixel), 2u, count);
        }
    }
    e->runs_end = q->byte_count;

    /* The state it is drawn from: the last primitive's, if nothing a
     * primitive is drawn from has changed. */
    if (!q->state_count || memcmp(q->states + (q->state_count - 1u) * LARGER_STATE_BYTES, st, LARGER_STATE_SAME) != 0) {
        memcpy(q->states + q->state_count * LARGER_STATE_BYTES, st, LARGER_STATE_BYTES);
        q->state_count++;
    }
    e->state = q->states + (q->state_count - 1u) * LARGER_STATE_BYTES;

    if (!q->count)
        q->rdp = *rdp;
    q->count++;
    q->span_count += span_need;
    q->first = rdp_min(q->first, e->first);
    q->last = rdp_max(q->last, e->last);
    if (alone) {
        larger_replay(q, e->first, e->last);
        queue_reset(q);
    }
    return 1;
}

/* After the console's own pass: the pixels larger_draw() brought into step
 * are in step again, with what the RDP has now drawn there. */
static void larger_done(N64Rdp *rdp, const RdpPrimitive *p)
{
    RdpLarger *l = (RdpLarger *)rdp->links.larger;
    const N64RdpState *st = rdp->state;
    const uint32_t pixel_bytes = st->fb_fmt == N64_RDP_FB_RGBA8888 ? 4u : 2u;
    const int32_t rows = p->span_yhi - p->span_ylo + 1;
    int32_t y;
    for (y = 0; y < rows; y++) {
        const uint32_t pixel = st->fb_width * (uint32_t)(p->span_ylo + y) + (uint32_t)l->first[y];
        uint32_t count, addr;
        if (l->last[y] < l->first[y])
            continue;
        count = (uint32_t)(l->last[y] - l->first[y] + 1);
        addr = pixel_bytes * ((st->fb_addr / pixel_bytes) + pixel);
        memcpy(l->reference + addr, rdp->links.rdram + addr, (size_t)pixel_bytes * count);
        addr = 2u * ((st->depth_addr >> 1) + pixel);
        memcpy(l->reference + addr, rdp->links.rdram + addr, 2u * (size_t)count);
    }
}

/* Pixels of work below which a primitive is not worth a second thread, the
 * work each further thread is brought in for, and the work sleeping
 * helpers are woken for (n64_rdp_workers.c): a pixel takes a few tens of
 * nanoseconds, a helper that is awake starts in well under a microsecond,
 * and one that is asleep in tens of them. */

void n64_rdp_draw_primitive(N64Rdp *rdp, RdpTriangle *setup, const RdpAttributes *attr)
{
    N64RdpState *st = rdp->state;
    RdpPrimitive prim;
    RdpPrimitive *p = &prim;
    int32_t start_y, min_line, max_line, y, pixels = 0;
    int larger = 0;

    /* fixup_triangle_setup: a YM above the first sub-scanline drawn is
     * never seen, so move it out of range. */
    start_y = setup->yh & ~3;
    if (setup->ym < start_y)
        setup->ym = 0x7fff;
    if (st->static_flags & RDP_RAST_INTERLACE_FIELD) {
        setup->flags |= RDP_TRI_INTERLACE_FIELD;
        if (st->static_flags & RDP_RAST_INTERLACE_KEEP_ODD)
            setup->flags |= RDP_TRI_INTERLACE_KEEP_ODD;
    }
    if (st->static_flags & (RDP_RAST_COPY | RDP_RAST_FILL))
        setup->flags |= RDP_TRI_FILL_COPY_RASTER;

    p->setup = *setup;
    p->attr = *attr;
    p->spans = st->spans;
    p->scale_log2 = 0;
    if (st->use_prim_depth) {
        p->attr.stzw[2] = st->prim_depth;
        p->attr.dstzw_dx[2] = 0;
        p->attr.dstzw_de[2] = 0;
        p->attr.dstzw_dy[2] = 0;
    }
    build_derived_attributes(st, attr, &p->derived);
    p->scissor_xlo = (int32_t)st->scissor_xlo;
    p->scissor_ylo = (int32_t)st->scissor_ylo;
    p->scissor_xhi = (int32_t)st->scissor_xhi;
    p->scissor_yhi = (int32_t)st->scissor_yhi;

    deduce_texture_use(st);
    deduce_noise_state(st);
    normalize_static_state(st, p);
    p->depth_blend_flags = st->depth_blend_flags;
    memcpy(p->blend_modes, st->blend_modes, sizeof p->blend_modes);
    p->coverage_mode = st->coverage_mode;
    p->z_mode = st->z_mode;
    /* Every primitive counts toward the noise seed, drawn or not. */
    p->primitive_index = st->primitive_count++;

    switch (st->fb_fmt) {
    case N64_RDP_FB_I4: p->fb_size = 0; p->dx_mask = 0; p->dx_shift = 0; break;
    case N64_RDP_FB_I8: p->fb_size = 1; p->dx_mask = ~7; p->dx_shift = 3; break;
    case N64_RDP_FB_RGBA5551:
    case N64_RDP_FB_IA88: p->fb_size = 2; p->dx_mask = ~3; p->dx_shift = 2; break;
    default: p->fb_size = 4; p->dx_mask = ~1; p->dx_shift = 1; break;
    }

    /* allocate_span_jobs: the scanlines the primitive and scissor share. */
    min_line = rdp_max(setup->yh, p->scissor_ylo) >> 2;
    max_line = rdp_asr(rdp_min(setup->yl - 1, p->scissor_yhi - 1), 2);
    if (max_line < min_line || st->fb_width == 0)
        return;
    p->span_ylo = min_line;
    p->span_yhi = max_line;

    /* Span setup runs one scanline past the end for the pipelined texel1
     * peek. */
    for (y = min_line; y <= max_line + 1 && y - min_line < N64_RDP_MAX_SPANS; y++) {
        const N64RdpSpan *span = &st->spans[y - min_line];
        span_setup(p, y, &st->spans[y - min_line]);
        if (span->valid_line && y <= max_line)
            pixels += rdp_max(rdp_min(span->end_x, (int32_t)st->fb_width - 1) - rdp_max(span->start_x, 0) + 1, 0);
    }

    /* The larger picture first: it has to see which pixels the game has
     * written since the RDP last drew them, and after the console's own
     * pass RDRAM no longer shows that. */
    if (rdp->links.larger)
        larger = larger_draw(rdp, p);

    fast_setup(rdp, p);

    /* Fill is a store per pixel: it is over before a helper could start. */
    if (pixels >= RDP_SPLIT_PIXELS && !(p->static_flags & RDP_RAST_FILL) && n64_rdp_workers_threads() > 1 &&
        !larger_replaying(rdp) && rows_independent(rdp, p)) {
        RdpRowsJob job;
        int32_t rows = max_line - min_line + 1;
        int32_t threads = rdp_min(pixels / RDP_PIXELS_PER_THREAD, (int32_t)n64_rdp_workers_threads());
        job.rdp = rdp;
        job.prim = p;
        /* Many pieces a thread - a scanline each, but for the tallest
         * primitives - so that a thread on a slower core, or one whose
         * scanlines turn out long, takes fewer of them. */
        job.rows_per_piece = rdp_max(rows / (threads * 16), 1);
        job.next_piece = 0;
        if (n64_rdp_workers_run(draw_rows_piece, &job, (unsigned)threads - 1u, pixels >= RDP_LARGE_PIXELS) == 0) {
            if (larger)
                larger_done(rdp, p);
            return;
        }
    }
    draw_rows(rdp, p, min_line, max_line);
    if (larger)
        larger_done(rdp, p);
}
