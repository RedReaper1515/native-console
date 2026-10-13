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
 * The colour combiner, the blender, dither and noise.
 *
 * Ported from parallel-rdp (commit 388d70f5835b352d841d9d9e5a08c5de01470f41)
 * shaders/combiner.h, blender.h, dither.h, noise.h and clamping.h.
 *
 * The combiner selectors arrive normalized the way rdp_renderer.cpp's
 * normalize_combiner() leaves them: every input that is a per-primitive
 * constant (primitive, environment, key centre and scale, the convert
 * constants) is folded into the constant_* inputs, and the selector
 * becomes "zero", which reads them. See rdp_build_combiner_constants() in
 * n64_rdp_raster.c.
 */
#ifndef N64_RDP_COMBINE_H
#define N64_RDP_COMBINE_H

#include "n64_rdp_internal.h"
#include "n64_rdp_quad.h"

/* noise.h: a hash of the pixel and the primitive standing in for the
 * hardware's noise generator. */
RDP_FORCE_INLINE uint16_t n64_rdp_noise_seed(uint32_t x, uint32_t y, uint32_t primitive_offset)
{
    const uint32_t prime = 1103515245u;
    uint32_t s0 = x, s1 = y, s2 = primitive_offset;
    int i;
    for (i = 0; i < 3; i++) {
        uint32_t n0 = ((s0 >> 8) ^ s1) * prime;
        uint32_t n1 = ((s1 >> 8) ^ s2) * prime;
        uint32_t n2 = ((s2 >> 8) ^ s0) * prime;
        s0 = n0;
        s1 = n1;
        s2 = n2;
    }
    return (uint16_t)(s0 >> 16);
}

static const uint8_t dither_matrices[2][16] = {
    {0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0},
    {0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2},
};

RDP_FORCE_INLINE void n64_rdp_rgb_dither(uint8_t rgb[3], int32_t dith)
{
    int c;
    for (c = 0; c < 3; c++) {
        int32_t orig = rgb[c];
        int32_t d = (dith >> (3 * c)) & 7;
        int32_t up = orig > 247 ? 255 : (orig & 0xf8) + 8;
        int32_t replace_sign = rdp_asr(d - (orig & 7), 31);
        int32_t v = orig + ((up - orig) & replace_sign);
        rgb[c] = (uint8_t)(v & 0xff);
    }
}

RDP_FORCE_INLINE void n64_rdp_dither_coefficients(int32_t x, int32_t y, int32_t mode_rgb, int32_t mode_alpha,
                                                  uint16_t noise, int32_t *rgb_dither, int32_t *alpha_dither)
{
    const int32_t splat = (1 << 0) | (1 << 3) | (1 << 6);
    int32_t idx = (y & 3) * 4 + (x & 3);

    if (mode_rgb < 2)
        *rgb_dither = dither_matrices[mode_rgb][idx] * splat;
    else if (mode_rgb == 2)
        *rgb_dither = n64_rdp_noise_dither_color(noise);
    else
        *rgb_dither = 0;

    if (mode_alpha == 3) {
        *alpha_dither = 0;
    } else if (mode_alpha == 2) {
        *alpha_dither = n64_rdp_noise_dither_alpha(noise);
    } else {
        *alpha_dither = mode_rgb >= 2 ? dither_matrices[mode_rgb & 1][idx] : (*rgb_dither & 7);
        if (mode_alpha == 1)
            *alpha_dither = ~*alpha_dither & 7;
    }
}

RDP_FORCE_INLINE int32_t n64_rdp_clamp_9bit(int32_t color)
{
    return rdp_clamp(rdp_sext(color - 0x80, 9) + 0x80, 0, 0xff);
}

/* Sign-extend to 9 bits around 0x80 without clamping. */
RDP_FORCE_INLINE int32_t special_expand(int32_t v) { return rdp_sext(v - 0x80, 9) + 0x80; }

RDP_FORCE_INLINE int32_t combiner_equation(int32_t a, int32_t b, int32_t c, int32_t d)
{
    int32_t color;
    c = rdp_sext(c, 9);
    a = special_expand(a);
    b = special_expand(b);
    d = special_expand(d);
    color = (a - b) * c + 0x80;
    return rdp_asr(color, 8) + d;
}

/* Normalized selector values (combiner.h). */
enum {
    MULADD_COMBINED = 0, MULADD_TEXEL0 = 1, MULADD_TEXEL1 = 2, MULADD_SHADE = 4, MULADD_ONE = 6, MULADD_NOISE = 7,
    MULSUB_COMBINED = 0, MULSUB_TEXEL0 = 1, MULSUB_TEXEL1 = 2, MULSUB_SHADE = 4, MULSUB_K4 = 7,
    MUL_COMBINED = 0, MUL_TEXEL0 = 1, MUL_TEXEL1 = 2, MUL_SHADE = 4, MUL_COMBINED_ALPHA = 7, MUL_TEXEL0_ALPHA = 8,
    MUL_TEXEL1_ALPHA = 9, MUL_SHADE_ALPHA = 11, MUL_LOD_FRAC = 13, MUL_K5 = 15,
    ADD_COMBINED = 0, ADD_TEXEL0 = 1, ADD_TEXEL1 = 2, ADD_SHADE = 4, ADD_ONE = 6,
    ALPHA_COMBINED = 0, ALPHA_TEXEL0 = 1, ALPHA_TEXEL1 = 2, ALPHA_SHADE = 4, ALPHA_ONE = 6,
    ALPHA_MUL_LOD_FRAC = 0, ALPHA_MUL_TEXEL0 = 1, ALPHA_MUL_TEXEL1 = 2, ALPHA_MUL_SHADE = 4,
};

/* The alpha input shared by the a, b and d slots. */
RDP_INLINE int32_t select_alpha_addsub(const RdpCombinerInputs *in, int sel, const uint8_t *constant)
{
    switch (sel) {
    case ALPHA_COMBINED: return in->combined[3];
    case ALPHA_TEXEL0: return in->texel0[3];
    case ALPHA_TEXEL1: return in->texel1[3];
    case ALPHA_SHADE: return in->shade[3];
    case ALPHA_ONE: return 0x100;
    default: return constant[3];
    }
}

RDP_INLINE void select_muladd(const RdpCombinerInputs *in, int sel_rgb, int sel_alpha, int32_t out[4])
{
    int c;
    for (c = 0; c < 3; c++) {
        switch (sel_rgb) {
        case MULADD_COMBINED: out[c] = in->combined[c]; break;
        case MULADD_TEXEL0: out[c] = in->texel0[c]; break;
        case MULADD_TEXEL1: out[c] = in->texel1[c]; break;
        case MULADD_SHADE: out[c] = in->shade[c]; break;
        case MULADD_NOISE: out[c] = in->noise; break;
        case MULADD_ONE: out[c] = 0x100; break;
        default: out[c] = in->constant_muladd[c]; break;
        }
    }
    out[3] = select_alpha_addsub(in, sel_alpha, in->constant_muladd);
}

RDP_INLINE void select_mulsub(const RdpCombinerInputs *in, int sel_rgb, int sel_alpha, int32_t out[4])
{
    int c;
    for (c = 0; c < 3; c++) {
        switch (sel_rgb) {
        case MULSUB_COMBINED: out[c] = in->combined[c]; break;
        case MULSUB_TEXEL0: out[c] = in->texel0[c]; break;
        case MULSUB_TEXEL1: out[c] = in->texel1[c]; break;
        case MULSUB_SHADE: out[c] = in->shade[c]; break;
        case MULSUB_K4: out[c] = (in->constant_mulsub[1] << 8) | in->constant_mulsub[2]; break;
        default: out[c] = in->constant_mulsub[c]; break;
        }
    }
    out[3] = select_alpha_addsub(in, sel_alpha, in->constant_mulsub);
}

RDP_INLINE void select_mul(const RdpCombinerInputs *in, int sel_rgb, int sel_alpha, int32_t out[4])
{
    int c;
    for (c = 0; c < 3; c++) {
        switch (sel_rgb) {
        case MUL_COMBINED: out[c] = in->combined[c]; break;
        case MUL_COMBINED_ALPHA: out[c] = in->combined[3]; break;
        case MUL_TEXEL0: out[c] = in->texel0[c]; break;
        case MUL_TEXEL1: out[c] = in->texel1[c]; break;
        case MUL_SHADE: out[c] = in->shade[c]; break;
        case MUL_TEXEL0_ALPHA: out[c] = in->texel0[3]; break;
        case MUL_TEXEL1_ALPHA: out[c] = in->texel1[3]; break;
        case MUL_SHADE_ALPHA: out[c] = in->shade[3]; break;
        case MUL_LOD_FRAC: out[c] = in->lod_frac; break;
        case MUL_K5: out[c] = (in->constant_mul[1] << 8) | in->constant_mul[2]; break;
        default: out[c] = in->constant_mul[c]; break;
        }
    }
    switch (sel_alpha) {
    case ALPHA_MUL_LOD_FRAC: out[3] = in->lod_frac; break;
    case ALPHA_MUL_TEXEL0: out[3] = in->texel0[3]; break;
    case ALPHA_MUL_TEXEL1: out[3] = in->texel1[3]; break;
    case ALPHA_MUL_SHADE: out[3] = in->shade[3]; break;
    default: out[3] = in->constant_mul[3]; break;
    }
}

RDP_INLINE void select_add(const RdpCombinerInputs *in, int sel_rgb, int sel_alpha, int32_t out[4])
{
    int c;
    for (c = 0; c < 3; c++) {
        switch (sel_rgb) {
        case ADD_COMBINED: out[c] = in->combined[c]; break;
        case ADD_TEXEL0: out[c] = in->texel0[c]; break;
        case ADD_TEXEL1: out[c] = in->texel1[c]; break;
        case ADD_SHADE: out[c] = in->shade[c]; break;
        case ADD_ONE: out[c] = 0x100; break;
        default: out[c] = in->constant_add[c]; break;
        }
    }
    out[3] = select_alpha_addsub(in, sel_alpha, in->constant_add);
}

RDP_INLINE void combine(const RdpCombinerInputs *in, const uint8_t sel[8], int32_t out[4])
{
    int32_t a[4], b[4], c[4], d[4];
    int i;
    select_muladd(in, sel[RDP_CC_RGB_MULADD], sel[RDP_CC_A_MULADD], a);
    select_mulsub(in, sel[RDP_CC_RGB_MULSUB], sel[RDP_CC_A_MULSUB], b);
    select_mul(in, sel[RDP_CC_RGB_MUL], sel[RDP_CC_A_MUL], c);
    select_add(in, sel[RDP_CC_RGB_ADD], sel[RDP_CC_A_ADD], d);
    for (i = 0; i < 4; i++)
        out[i] = combiner_equation(a[i], b[i], c[i], d[i]);
}

/* The same selections from the pixel's value table (RDP_V_*): `source`
 * holds where each slot reads, as n64_rdp_combiner_sources() chose it. The
 * colour channels are worked out together and the alpha on its own; the
 * result's last lane is not the alpha (the caller puts it there).
 *
 * After the sign extensions a - b is within -511..511 and c within
 * -256..255, whatever was in the table, so the product may be taken in 16
 * bits a side. */
RDP_FORCE_INLINE RdpQuad combine_values(const int32_t *v, const uint8_t source[8], int32_t *alpha)
{
    const RdpQuad bias = quad_splat(0x80);
    RdpQuad a = quad_load(v + source[RDP_CC_RGB_MULADD]), b = quad_load(v + source[RDP_CC_RGB_MULSUB]);
    RdpQuad c = quad_load(v + source[RDP_CC_RGB_MUL]), d = quad_load(v + source[RDP_CC_RGB_ADD]);
    *alpha = combiner_equation(v[source[RDP_CC_A_MULADD]], v[source[RDP_CC_A_MULSUB]], v[source[RDP_CC_A_MUL]],
                               v[source[RDP_CC_A_ADD]]);
    c = quad_asr(quad_shl(c, 23), 23);
    a = quad_add(quad_asr(quad_shl(quad_sub(a, bias), 23), 23), bias);
    b = quad_add(quad_asr(quad_shl(quad_sub(b, bias), 23), 23), bias);
    d = quad_add(quad_asr(quad_shl(quad_sub(d, bias), 23), 23), bias);
    return quad_add(quad_asr(quad_add(quad_mul16(quad_sub(a, b), c), bias), 8), d);
}

/* n64_rdp_clamp_9bit() of each lane. */
RDP_FORCE_INLINE RdpQuad quad_clamp_9bit(RdpQuad q)
{
    const RdpQuad bias = quad_splat(0x80);
    return quad_clamp_u8(quad_add(quad_asr(quad_shl(quad_sub(q, bias), 23), 23), bias));
}

/* Where the normalized selectors `sel` of one cycle read in the value
 * table. combined, texel0 and texel1 are the entries that cycle sees as
 * COMBINED (RDP_V_ZERO before there is one), TEXEL0 and TEXEL1
 * (RDP_V_TEXEL0, RDP_V_TEXEL1 or RDP_V_NEXT). Every case is one of
 * select_muladd(), select_mulsub(), select_mul(), select_add() and
 * select_alpha_addsub() above. */
RDP_INLINE void n64_rdp_combiner_sources(const uint8_t sel[8], int cycle, int combined, int texel0, int texel1,
                                         uint8_t source[8])
{
    int constants = RDP_V_CONSTANTS + 16 * cycle;
    int combined_alpha = combined == RDP_V_COMBINED ? RDP_V_COMBINED_ALPHA : RDP_V_ZERO;
    int texel0_alpha = RDP_V_TEXEL0_ALPHA + (texel0 - RDP_V_TEXEL0);
    int texel1_alpha = RDP_V_TEXEL0_ALPHA + (texel1 - RDP_V_TEXEL0);
    int slot;

    switch (sel[RDP_CC_RGB_MULADD]) {
    case MULADD_COMBINED: source[RDP_CC_RGB_MULADD] = (uint8_t)combined; break;
    case MULADD_TEXEL0: source[RDP_CC_RGB_MULADD] = (uint8_t)texel0; break;
    case MULADD_TEXEL1: source[RDP_CC_RGB_MULADD] = (uint8_t)texel1; break;
    case MULADD_SHADE: source[RDP_CC_RGB_MULADD] = RDP_V_SHADE; break;
    case MULADD_NOISE: source[RDP_CC_RGB_MULADD] = RDP_V_NOISE; break;
    case MULADD_ONE: source[RDP_CC_RGB_MULADD] = RDP_V_ONE; break;
    default: source[RDP_CC_RGB_MULADD] = (uint8_t)constants; break;
    }
    switch (sel[RDP_CC_RGB_MULSUB]) {
    case MULSUB_COMBINED: source[RDP_CC_RGB_MULSUB] = (uint8_t)combined; break;
    case MULSUB_TEXEL0: source[RDP_CC_RGB_MULSUB] = (uint8_t)texel0; break;
    case MULSUB_TEXEL1: source[RDP_CC_RGB_MULSUB] = (uint8_t)texel1; break;
    case MULSUB_SHADE: source[RDP_CC_RGB_MULSUB] = RDP_V_SHADE; break;
    case MULSUB_K4: source[RDP_CC_RGB_MULSUB] = (uint8_t)(RDP_V_K4 + 4 * cycle); break;
    default: source[RDP_CC_RGB_MULSUB] = (uint8_t)(constants + 4); break;
    }
    switch (sel[RDP_CC_RGB_MUL]) {
    case MUL_COMBINED: source[RDP_CC_RGB_MUL] = (uint8_t)combined; break;
    case MUL_COMBINED_ALPHA: source[RDP_CC_RGB_MUL] = (uint8_t)combined_alpha; break;
    case MUL_TEXEL0: source[RDP_CC_RGB_MUL] = (uint8_t)texel0; break;
    case MUL_TEXEL1: source[RDP_CC_RGB_MUL] = (uint8_t)texel1; break;
    case MUL_SHADE: source[RDP_CC_RGB_MUL] = RDP_V_SHADE; break;
    case MUL_TEXEL0_ALPHA: source[RDP_CC_RGB_MUL] = (uint8_t)texel0_alpha; break;
    case MUL_TEXEL1_ALPHA: source[RDP_CC_RGB_MUL] = (uint8_t)texel1_alpha; break;
    case MUL_SHADE_ALPHA: source[RDP_CC_RGB_MUL] = RDP_V_SHADE_ALPHA; break;
    case MUL_LOD_FRAC: source[RDP_CC_RGB_MUL] = RDP_V_LOD; break;
    case MUL_K5: source[RDP_CC_RGB_MUL] = (uint8_t)(RDP_V_K5 + 4 * cycle); break;
    default: source[RDP_CC_RGB_MUL] = (uint8_t)(constants + 8); break;
    }
    switch (sel[RDP_CC_RGB_ADD]) {
    case ADD_COMBINED: source[RDP_CC_RGB_ADD] = (uint8_t)combined; break;
    case ADD_TEXEL0: source[RDP_CC_RGB_ADD] = (uint8_t)texel0; break;
    case ADD_TEXEL1: source[RDP_CC_RGB_ADD] = (uint8_t)texel1; break;
    case ADD_SHADE: source[RDP_CC_RGB_ADD] = RDP_V_SHADE; break;
    case ADD_ONE: source[RDP_CC_RGB_ADD] = RDP_V_ONE; break;
    default: source[RDP_CC_RGB_ADD] = (uint8_t)(constants + 12); break;
    }
    /* The alpha of the a, b and d slots; RDP_CC_A_MUL is done below. */
    for (slot = 0; slot < 4; slot++) {
        uint8_t *out = &source[RDP_CC_A_MULADD + slot];
        switch (sel[RDP_CC_A_MULADD + slot]) {
        case ALPHA_COMBINED: *out = (uint8_t)(combined + 3); break;
        case ALPHA_TEXEL0: *out = (uint8_t)(texel0 + 3); break;
        case ALPHA_TEXEL1: *out = (uint8_t)(texel1 + 3); break;
        case ALPHA_SHADE: *out = RDP_V_SHADE + 3; break;
        case ALPHA_ONE: *out = RDP_V_ONE; break;
        default: *out = (uint8_t)(constants + 4 * slot + 3); break;
        }
    }
    switch (sel[RDP_CC_A_MUL]) {
    case ALPHA_MUL_LOD_FRAC: source[RDP_CC_A_MUL] = RDP_V_LOD; break;
    case ALPHA_MUL_TEXEL0: source[RDP_CC_A_MUL] = (uint8_t)(texel0 + 3); break;
    case ALPHA_MUL_TEXEL1: source[RDP_CC_A_MUL] = (uint8_t)(texel1 + 3); break;
    case ALPHA_MUL_SHADE: source[RDP_CC_A_MUL] = RDP_V_SHADE + 3; break;
    default: source[RDP_CC_A_MUL] = (uint8_t)(constants + 8 + 3); break;
    }
}

RDP_INLINE void n64_rdp_combiner_cycle0(const RdpCombinerInputs *in, const uint8_t sel[8], int32_t alpha_dith,
                                        int32_t coverage, int cvg_times_alpha, int alpha_cvg_select, int alpha_test,
                                        int32_t *alpha_reference, int32_t out[4])
{
    combine(in, sel, out);
    if (alpha_test) {
        int32_t clamped_alpha = n64_rdp_clamp_9bit(out[3]);
        /* Expand 0xff to 0x100 so the products divide by a power of two. */
        int32_t expanded_alpha = clamped_alpha + ((clamped_alpha + 1) >> 8);
        if (alpha_cvg_select) {
            if (cvg_times_alpha)
                expanded_alpha = (expanded_alpha * coverage + 4) >> 3;
            else
                expanded_alpha = coverage << 5;
        } else {
            expanded_alpha += alpha_dith;
        }
        *alpha_reference = rdp_clamp(expanded_alpha, 0, 0xff);
    } else {
        *alpha_reference = 0;
    }
}

RDP_INLINE void n64_rdp_combiner_cycle1(const RdpCombinerInputs *in, const uint8_t sel[8], int32_t alpha_dith,
                                        int32_t *coverage, int cvg_times_alpha, int alpha_cvg_select, int32_t out[4])
{
    int32_t expanded_alpha, modulated_alpha;
    int i;
    combine(in, sel, out);
    for (i = 0; i < 4; i++)
        out[i] = n64_rdp_clamp_9bit(out[i]);

    expanded_alpha = out[3] + ((out[3] + 1) >> 8);
    if (cvg_times_alpha) {
        modulated_alpha = (expanded_alpha * *coverage + 4) >> 3;
        *coverage = modulated_alpha >> 5;
    } else {
        modulated_alpha = *coverage << 5;
    }
    if (alpha_cvg_select)
        expanded_alpha = modulated_alpha;
    else
        expanded_alpha += alpha_dith;
    out[3] = rdp_clamp(expanded_alpha, 0, 0xff);
}

RDP_FORCE_INLINE const uint8_t *blend_rgb_input(const RdpBlendInputs *in, int sel)
{
    switch (sel) {
    case 0: return in->pixel_color;
    case 1: return in->memory_color;
    case 2: return in->blend_color;
    default: return in->fog_color;
    }
}

/* One blender cycle: (P * A + M * B) / (A + B), with the shortcuts the
 * final cycle takes and its divider. */
RDP_FORCE_INLINE void n64_rdp_blender(const RdpBlendInputs *in, const uint8_t modes[4], int force_blend, int blend_en,
                                      int color_on_coverage, int coverage_wrap, const int32_t blend_shift[2],
                                      int final_cycle, uint8_t out[3])
{
    const uint8_t *rgb1 = blend_rgb_input(in, modes[2]);
    const uint8_t *rgb0;
    int32_t a0, a1;
    int c;

    if (final_cycle && color_on_coverage && !coverage_wrap) {
        out[0] = rgb1[0];
        out[1] = rgb1[1];
        out[2] = rgb1[2];
        return;
    }

    rgb0 = blend_rgb_input(in, modes[0]);
    if (final_cycle && (!blend_en || (modes[1] == 0 && modes[3] == 0 && in->pixel_color[3] == 0xff))) {
        out[0] = rgb0[0];
        out[1] = rgb0[1];
        out[2] = rgb0[2];
        return;
    }

    switch (modes[1]) {
    case 0: a0 = in->pixel_color[3]; break;
    case 1: a0 = in->fog_color[3]; break;
    case 2: a0 = in->shade_alpha; break;
    default: a0 = 0; break;
    }
    switch (modes[3]) {
    case 0: a1 = ~a0 & 0xff; break;
    case 1: a1 = in->memory_color[3]; break;
    case 2: a1 = 0xff; break;
    default: a1 = 0; break;
    }
    a0 >>= 3;
    a1 >>= 3;
    if (modes[3] == 1) {
        a0 = (a0 >> blend_shift[0]) & 0x3c;
        a1 = (a1 >> blend_shift[1]) | 3;
    }

    for (c = 0; c < 3; c++) {
        int32_t blended = rgb0[c] * a0 + rgb1[c] * (a1 + 1);
        int32_t v;
        if (!final_cycle || force_blend) {
            v = blended >> 5;
        } else {
            /* The hardware's divider: a straight divide in normal cases, and
             * parallel-rdp's table for the edge cases. */
            int32_t blend_sum = (a0 >> 2) + (a1 >> 2) + 1;
            v = n64_rdp_blender_lut[(blend_sum << 11) | ((blended >> 2) & 0x7ff)];
        }
        out[c] = (uint8_t)(v & 0xff);
    }
}

#endif /* N64_RDP_COMBINE_H */
