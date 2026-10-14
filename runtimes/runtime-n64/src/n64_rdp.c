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
 * The RDP's command interface: the DPC registers, the command reader, and
 * the decoding of every command into the rasteriser's state.
 *
 * Command decoding follows parallel-rdp's rdp_device.cpp (CommandProcessor)
 * and the state setters of rdp_renderer.cpp (Renderer::set_*), commit
 * 388d70f5835b352d841d9d9e5a08c5de01470f41. The DPC register behaviour is
 * this project's (docs/N64-PLAN.md, "The RDP"): parallel-rdp leaves it to
 * its host.
 */
#include "n64_rdp_internal.h"

#include <stdlib.h>
#include <string.h>

/* How many 64-bit words each command takes (the triangle lengths are the
 * edge coefficients plus the optional shade, texture and depth blocks). */
static uint32_t rdp_command_length(uint32_t op)
{
    switch (op) {
    case 0x08: return 4;
    case 0x09: return 6;
    case 0x0a: return 12;
    case 0x0b: return 14;
    case 0x0c: return 12;
    case 0x0d: return 14;
    case 0x0e: return 20;
    case 0x0f: return 22;
    case RDP_OP_TEXTURE_RECTANGLE:
    case RDP_OP_TEXTURE_RECTANGLE_FLIP: return 2;
    default: return 1;
    }
}

static void rdp_set_flag(uint32_t *flags, int cond, uint32_t mask)
{
    *flags &= ~mask;
    if (cond)
        *flags |= mask;
}

/* CommandProcessor::decode_triangle_setup. The lower bit of each X value
 * is dropped, as parallel-rdp does to gain a bit for upscaling: the
 * rasteriser below expects 15 fractional bits. */
static void rdp_decode_triangle(const N64RdpState *st, RdpTriangle *setup, const uint32_t *w)
{
    int copy_cycle = (st->static_flags & RDP_RAST_COPY) != 0;
    int flip = (w[0] & 0x800000u) != 0;
    int sign_dxhdy = (w[5] & 0x80000000u) != 0;
    int do_offset = flip == sign_dxhdy;

    memset(setup, 0, sizeof *setup);
    setup->flags |= flip ? RDP_TRI_FLIP : 0;
    setup->flags |= do_offset ? RDP_TRI_DO_OFFSET : 0;
    setup->flags |= copy_cycle ? RDP_TRI_SKIP_XFRAC : 0;
    setup->tile = (w[0] >> 16) & 63;

    setup->yl = rdp_sext((int32_t)(w[0] & 0x3fff), 14);
    setup->ym = rdp_sext((int32_t)((w[1] >> 16) & 0x3fff), 14);
    setup->yh = rdp_sext((int32_t)(w[1] & 0x3fff), 14);

    setup->xl = rdp_asr(rdp_sext(rdp_u2i(w[2]), 28), 1);
    setup->xh = rdp_asr(rdp_sext(rdp_u2i(w[4]), 28), 1);
    setup->xm = rdp_asr(rdp_sext(rdp_u2i(w[6]), 28), 1);
    setup->dxldy = rdp_asr(rdp_sext(rdp_u2i(w[3] >> 2), 28), 1);
    setup->dxhdy = rdp_asr(rdp_sext(rdp_u2i(w[5] >> 2), 28), 1);
    setup->dxmdy = rdp_asr(rdp_sext(rdp_u2i(w[7] >> 2), 28), 1);
}

/* The shade, texture and depth coefficient blocks (decode_rgba_setup,
 * decode_tex_setup, decode_z_setup): integer halves in the first four
 * words, fractions in the next four. */
static int32_t rdp_hi_lo(uint32_t hi_word, uint32_t lo_word) { return rdp_u2i((hi_word & 0xffff0000u) | (lo_word >> 16)); }
static int32_t rdp_lo_lo(uint32_t hi_word, uint32_t lo_word) { return rdp_u2i((hi_word << 16) | (lo_word & 0xffffu)); }

static void rdp_decode_rgba(RdpAttributes *a, const uint32_t *w)
{
    int c;
    for (c = 0; c < 2; c++) {
        a->rgba[2 * c] = rdp_hi_lo(w[0 + c], w[4 + c]);
        a->rgba[2 * c + 1] = rdp_lo_lo(w[0 + c], w[4 + c]);
        a->drgba_dx[2 * c] = rdp_hi_lo(w[2 + c], w[6 + c]);
        a->drgba_dx[2 * c + 1] = rdp_lo_lo(w[2 + c], w[6 + c]);
        a->drgba_de[2 * c] = rdp_hi_lo(w[8 + c], w[12 + c]);
        a->drgba_de[2 * c + 1] = rdp_lo_lo(w[8 + c], w[12 + c]);
        a->drgba_dy[2 * c] = rdp_hi_lo(w[10 + c], w[14 + c]);
        a->drgba_dy[2 * c + 1] = rdp_lo_lo(w[10 + c], w[14 + c]);
    }
}

static void rdp_decode_tex(RdpAttributes *a, const uint32_t *w)
{
    /* s and t share the first word; w takes the next. Index 2 is z. */
    a->stzw[0] = rdp_hi_lo(w[0], w[4]);
    a->stzw[1] = rdp_lo_lo(w[0], w[4]);
    a->stzw[3] = rdp_hi_lo(w[1], w[5]);
    a->dstzw_dx[0] = rdp_hi_lo(w[2], w[6]);
    a->dstzw_dx[1] = rdp_lo_lo(w[2], w[6]);
    a->dstzw_dx[3] = rdp_hi_lo(w[3], w[7]);
    a->dstzw_de[0] = rdp_hi_lo(w[8], w[12]);
    a->dstzw_de[1] = rdp_lo_lo(w[8], w[12]);
    a->dstzw_de[3] = rdp_hi_lo(w[9], w[13]);
    a->dstzw_dy[0] = rdp_hi_lo(w[10], w[14]);
    a->dstzw_dy[1] = rdp_lo_lo(w[10], w[14]);
    a->dstzw_dy[3] = rdp_hi_lo(w[11], w[15]);
}

static void rdp_decode_z(RdpAttributes *a, const uint32_t *w)
{
    a->stzw[2] = rdp_u2i(w[0]);
    a->dstzw_dx[2] = rdp_u2i(w[1]);
    a->dstzw_de[2] = rdp_u2i(w[2]);
    a->dstzw_dy[2] = rdp_u2i(w[3]);
}

static void rdp_triangle(N64Rdp *rdp, const uint32_t *w, uint32_t op)
{
    RdpTriangle setup;
    RdpAttributes attr;
    uint32_t next = 8;
    memset(&attr, 0, sizeof attr);
    rdp_decode_triangle(rdp->state, &setup, w);
    if (op & 4) {
        rdp_decode_rgba(&attr, w + next);
        next += 16;
    }
    if (op & 2) {
        rdp_decode_tex(&attr, w + next);
        next += 16;
    }
    if (op & 1)
        rdp_decode_z(&attr, w + next);
    n64_rdp_draw_primitive(rdp, &setup, &attr);
}

static void rdp_rectangle_setup(const N64RdpState *st, RdpTriangle *setup, const uint32_t *w)
{
    uint32_t xl = (w[0] >> 12) & 0xfff;
    uint32_t yl = (w[0] >> 0) & 0xfff;
    uint32_t xh = (w[1] >> 12) & 0xfff;
    uint32_t yh = (w[1] >> 0) & 0xfff;

    /* Fill and copy modes draw the bottom scanline too. */
    if (st->static_flags & (RDP_RAST_COPY | RDP_RAST_FILL))
        yl |= 3;

    memset(setup, 0, sizeof *setup);
    setup->xh = (int32_t)(xh << 13);
    setup->xl = (int32_t)(xl << 13);
    setup->xm = (int32_t)(xl << 13);
    setup->ym = (int32_t)yl;
    setup->yl = (int32_t)yl;
    setup->yh = (int32_t)yh;
    setup->flags = RDP_TRI_FLIP | RDP_TRI_DISABLE_UPSCALING;
}

static void rdp_fill_rectangle(N64Rdp *rdp, const uint32_t *w)
{
    RdpTriangle setup;
    RdpAttributes attr;
    rdp_rectangle_setup(rdp->state, &setup, w);
    memset(&attr, 0, sizeof attr);
    n64_rdp_draw_primitive(rdp, &setup, &attr);
}

/* op_texture_rectangle and op_texture_rectangle_flip: the flip variant
 * steps s down the screen and t across it. */
static void rdp_texture_rectangle(N64Rdp *rdp, const uint32_t *w, int flip)
{
    RdpTriangle setup;
    RdpAttributes attr;
    int32_t s = (int32_t)((w[2] >> 16) & 0xffff);
    int32_t t = (int32_t)(w[2] & 0xffff);
    int32_t dsdx = rdp_sext((int32_t)((w[3] >> 16) & 0xffff), 16);
    int32_t dtdy = rdp_sext((int32_t)(w[3] & 0xffff), 16);

    rdp_rectangle_setup(rdp->state, &setup, w);
    setup.tile = (w[1] >> 24) & 7;
    memset(&attr, 0, sizeof attr);
    attr.stzw[0] = rdp_shl(s, 16);
    attr.stzw[1] = rdp_shl(t, 16);
    if (!flip) {
        attr.dstzw_dx[0] = rdp_shl(dsdx, 11);
        attr.dstzw_de[1] = rdp_shl(dtdy, 11);
        attr.dstzw_dy[1] = rdp_shl(dtdy, 11);
    } else {
        attr.dstzw_dx[1] = rdp_shl(dtdy, 11);
        attr.dstzw_de[0] = rdp_shl(dsdx, 11);
        attr.dstzw_dy[0] = rdp_shl(dsdx, 11);
    }
    if (rdp->state->static_flags & RDP_RAST_COPY)
        setup.flags |= RDP_TRI_SKIP_XFRAC;
    n64_rdp_draw_primitive(rdp, &setup, &attr);
}

static void rdp_set_other_modes(N64RdpState *st, const uint32_t *w)
{
    uint32_t *sf = &st->static_flags;
    uint32_t *db = &st->depth_blend_flags;

    rdp_set_flag(sf, w[0] & (1u << 19), RDP_RAST_PERSPECTIVE);
    rdp_set_flag(sf, w[0] & (1u << 18), RDP_RAST_DETAIL_LOD);
    rdp_set_flag(sf, w[0] & (1u << 17), RDP_RAST_SHARPEN_LOD);
    rdp_set_flag(sf, w[0] & (1u << 16), RDP_RAST_TEX_LOD);
    rdp_set_flag(sf, w[0] & (1u << 15), RDP_RAST_TLUT);
    rdp_set_flag(sf, w[0] & (1u << 14), RDP_RAST_TLUT_TYPE);
    rdp_set_flag(sf, w[0] & (1u << 13), RDP_RAST_SAMPLE_QUAD);
    rdp_set_flag(sf, w[0] & (1u << 12), RDP_RAST_MID_TEXEL);
    rdp_set_flag(sf, w[0] & (1u << 11), RDP_RAST_BILERP0);
    rdp_set_flag(sf, w[0] & (1u << 10), RDP_RAST_BILERP1);
    rdp_set_flag(sf, w[0] & (1u << 9), RDP_RAST_CONVERT_ONE);
    rdp_set_flag(db, w[1] & (1u << 14), RDP_DB_FORCE_BLEND);
    rdp_set_flag(sf, w[1] & (1u << 13), RDP_RAST_ALPHA_CVG_SELECT);
    rdp_set_flag(sf, w[1] & (1u << 12), RDP_RAST_CVG_TIMES_ALPHA);
    rdp_set_flag(db, w[1] & (1u << 7), RDP_DB_COLOR_ON_COVERAGE);
    rdp_set_flag(db, w[1] & (1u << 6), RDP_DB_IMAGE_READ);
    rdp_set_flag(db, w[1] & (1u << 5), RDP_DB_DEPTH_UPDATE);
    rdp_set_flag(db, w[1] & (1u << 4), RDP_DB_DEPTH_TEST);
    rdp_set_flag(sf, w[1] & (1u << 3), RDP_RAST_AA);
    rdp_set_flag(db, w[1] & (1u << 3), RDP_DB_AA);
    rdp_set_flag(sf, w[1] & (1u << 1), RDP_RAST_ALPHA_TEST_DITHER);
    rdp_set_flag(sf, w[1] & (1u << 0), RDP_RAST_ALPHA_TEST);
    st->dither = (w[0] >> 4) & 0x0f;
    rdp_set_flag(db, (st->dither >> 2) != 3, RDP_DB_DITHER);
    st->coverage_mode = (uint8_t)((w[1] >> 8) & 3);
    st->z_mode = (uint8_t)((w[1] >> 10) & 3);

    *sf &= ~(RDP_RAST_MULTI_CYCLE | RDP_RAST_FILL | RDP_RAST_COPY);
    *db &= ~RDP_DB_MULTI_CYCLE;
    switch ((w[0] >> 20) & 3) {
    case 1:
        *sf |= RDP_RAST_MULTI_CYCLE;
        *db |= RDP_DB_MULTI_CYCLE;
        break;
    case 2: *sf |= RDP_RAST_COPY; break;
    case 3: *sf |= RDP_RAST_FILL; break;
    default: break;
    }

    st->blend_modes[0][0] = (uint8_t)((w[1] >> 30) & 3);
    st->blend_modes[1][0] = (uint8_t)((w[1] >> 28) & 3);
    st->blend_modes[0][1] = (uint8_t)((w[1] >> 26) & 3);
    st->blend_modes[1][1] = (uint8_t)((w[1] >> 24) & 3);
    st->blend_modes[0][2] = (uint8_t)((w[1] >> 22) & 3);
    st->blend_modes[1][2] = (uint8_t)((w[1] >> 20) & 3);
    st->blend_modes[0][3] = (uint8_t)((w[1] >> 18) & 3);
    st->blend_modes[1][3] = (uint8_t)((w[1] >> 16) & 3);
    st->use_prim_depth = (w[1] & (1u << 2)) != 0;
}

static void rdp_set_combine(N64RdpState *st, const uint32_t *w)
{
    st->combiner[0][RDP_CC_RGB_MULADD] = (uint8_t)((w[0] >> 20) & 0xf);
    st->combiner[0][RDP_CC_RGB_MUL] = (uint8_t)((w[0] >> 15) & 0x1f);
    st->combiner[0][RDP_CC_RGB_MULSUB] = (uint8_t)((w[1] >> 28) & 0xf);
    st->combiner[0][RDP_CC_RGB_ADD] = (uint8_t)((w[1] >> 15) & 0x7);
    st->combiner[0][RDP_CC_A_MULADD] = (uint8_t)((w[0] >> 12) & 0x7);
    st->combiner[0][RDP_CC_A_MULSUB] = (uint8_t)((w[1] >> 12) & 0x7);
    st->combiner[0][RDP_CC_A_MUL] = (uint8_t)((w[0] >> 9) & 0x7);
    st->combiner[0][RDP_CC_A_ADD] = (uint8_t)((w[1] >> 9) & 0x7);

    st->combiner[1][RDP_CC_RGB_MULADD] = (uint8_t)((w[0] >> 5) & 0xf);
    st->combiner[1][RDP_CC_RGB_MUL] = (uint8_t)((w[0] >> 0) & 0x1f);
    st->combiner[1][RDP_CC_RGB_MULSUB] = (uint8_t)((w[1] >> 24) & 0xf);
    st->combiner[1][RDP_CC_RGB_ADD] = (uint8_t)((w[1] >> 6) & 0x7);
    st->combiner[1][RDP_CC_A_MULADD] = (uint8_t)((w[1] >> 21) & 0x7);
    st->combiner[1][RDP_CC_A_MULSUB] = (uint8_t)((w[1] >> 3) & 0x7);
    st->combiner[1][RDP_CC_A_MUL] = (uint8_t)((w[1] >> 18) & 0x7);
    st->combiner[1][RDP_CC_A_ADD] = (uint8_t)((w[1] >> 0) & 0x7);
}

static void rdp_set_tile(N64RdpState *st, const uint32_t *w)
{
    N64RdpTile *t = &st->tiles[(w[1] >> 24) & 7];
    uint32_t slo = t->slo, shi = t->shi, tlo = t->tlo, thi = t->thi;

    /* SET_TILE replaces the descriptor but not the tile size. */
    memset(t, 0, sizeof *t);
    t->slo = slo;
    t->shi = shi;
    t->tlo = tlo;
    t->thi = thi;
    t->offset = ((w[0] >> 0) & 511) << 3;
    t->stride = ((w[0] >> 9) & 511) << 3;
    t->size = (uint8_t)((w[0] >> 19) & 3);
    t->fmt = (uint8_t)((w[0] >> 21) & 7);
    t->palette = (uint8_t)((w[1] >> 20) & 15);
    t->shift_s = (uint8_t)((w[1] >> 0) & 15);
    t->mask_s = (uint8_t)((w[1] >> 4) & 15);
    t->shift_t = (uint8_t)((w[1] >> 10) & 15);
    t->mask_t = (uint8_t)((w[1] >> 14) & 15);
    if (w[1] & (1u << 8))
        t->flags |= N64_RDP_TILE_MIRROR_S;
    if (w[1] & (1u << 9))
        t->flags |= N64_RDP_TILE_CLAMP_S;
    if (w[1] & (1u << 18))
        t->flags |= N64_RDP_TILE_MIRROR_T;
    if (w[1] & (1u << 19))
        t->flags |= N64_RDP_TILE_CLAMP_T;

    /* A zero mask means no wrapping, which the sampler treats as clamping;
     * masks above 10 behave as 10. */
    if (t->mask_s > 10)
        t->mask_s = 10;
    else if (t->mask_s == 0)
        t->flags |= N64_RDP_TILE_CLAMP_S;
    if (t->mask_t > 10)
        t->mask_t = 10;
    else if (t->mask_t == 0)
        t->flags |= N64_RDP_TILE_CLAMP_T;
}

static void rdp_load(N64Rdp *rdp, const uint32_t *w, int mode)
{
    N64RdpState *st = rdp->state;
    RdpLoadInfo info;
    info.tex_addr = st->tex_addr;
    info.tex_width = st->tex_width;
    info.fmt = st->tex_fmt;
    info.size = st->tex_size;
    info.slo = (w[0] >> 12) & 0xfff;
    info.shi = (w[1] >> 12) & 0xfff;
    info.tlo = (w[0] >> 0) & 0xfff;
    info.thi = (w[1] >> 0) & 0xfff;
    info.mode = mode;
    n64_rdp_load_tile(rdp, (w[1] >> 24) & 7, &info);
}

static void rdp_execute_command(N64Rdp *rdp, const uint32_t *w)
{
    N64RdpState *st = rdp->state;
    uint32_t op = (w[0] >> 24) & 63;

    switch (op) {
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        rdp_triangle(rdp, w, op);
        break;
    case RDP_OP_TEXTURE_RECTANGLE: rdp_texture_rectangle(rdp, w, 0); break;
    case RDP_OP_TEXTURE_RECTANGLE_FLIP: rdp_texture_rectangle(rdp, w, 1); break;
    case RDP_OP_SYNC_FULL:
        /* Rendering is synchronous, so the sync only raises DP. */
        n64_rdp_larger_kick(rdp);
        if (rdp->links.interrupt)
            rdp->links.interrupt(rdp->links.user, 1);
        break;
    case RDP_OP_SET_KEY_GB:
        st->key_width[1] = (w[0] >> 12) & 0xfff;
        st->key_width[2] = (w[0] >> 0) & 0xfff;
        st->key_center[1] = (uint8_t)(w[1] >> 24);
        st->key_scale[1] = (uint8_t)(w[1] >> 16);
        st->key_center[2] = (uint8_t)(w[1] >> 8);
        st->key_scale[2] = (uint8_t)(w[1] >> 0);
        break;
    case RDP_OP_SET_KEY_R:
        st->key_width[0] = (w[1] >> 16) & 0xfff;
        st->key_center[0] = (uint8_t)(w[1] >> 8);
        st->key_scale[0] = (uint8_t)(w[1] >> 0);
        break;
    case RDP_OP_SET_CONVERT: {
        uint64_t merged = ((uint64_t)w[0] << 32) | w[1];
        int i;
        for (i = 0; i < 4; i++) {
            int32_t k = rdp_sext((int32_t)((merged >> (45 - 9 * i)) & 0x1ff), 9);
            st->convert[i] = 2 * k + 1;
        }
        st->convert[4] = (int32_t)((merged >> 9) & 0x1ff);
        st->convert[5] = (int32_t)((merged >> 0) & 0x1ff);
        break;
    }
    case RDP_OP_SET_SCISSOR:
        st->scissor_xlo = (w[0] >> 12) & 0xfff;
        st->scissor_xhi = (w[1] >> 12) & 0xfff;
        st->scissor_ylo = (w[0] >> 0) & 0xfff;
        st->scissor_yhi = (w[1] >> 0) & 0xfff;
        rdp_set_flag(&st->static_flags, w[1] & (1u << 25), RDP_RAST_INTERLACE_FIELD);
        rdp_set_flag(&st->static_flags, w[1] & (1u << 24), RDP_RAST_INTERLACE_KEEP_ODD);
        break;
    case RDP_OP_SET_PRIM_DEPTH:
        st->prim_depth = (int32_t)(((w[1] >> 16) & 0x7fff) << 16);
        st->prim_dz = (uint16_t)(w[1] & 0xffff);
        break;
    case RDP_OP_SET_OTHER_MODES: rdp_set_other_modes(st, w); break;
    case RDP_OP_LOAD_TLUT: rdp_load(rdp, w, RDP_UPLOAD_TLUT); break;
    case RDP_OP_SET_TILE_SIZE: {
        N64RdpTile *t = &st->tiles[(w[1] >> 24) & 7];
        t->slo = (w[0] >> 12) & 0xfff;
        t->shi = (w[1] >> 12) & 0xfff;
        t->tlo = (w[0] >> 0) & 0xfff;
        t->thi = (w[1] >> 0) & 0xfff;
        break;
    }
    case RDP_OP_LOAD_BLOCK: rdp_load(rdp, w, RDP_UPLOAD_BLOCK); break;
    case RDP_OP_LOAD_TILE: rdp_load(rdp, w, RDP_UPLOAD_TILE); break;
    case RDP_OP_SET_TILE: rdp_set_tile(st, w); break;
    case RDP_OP_FILL_RECTANGLE: rdp_fill_rectangle(rdp, w); break;
    case RDP_OP_SET_FILL_COLOR: st->fill_color = w[1]; break;
    case RDP_OP_SET_FOG_COLOR: st->fog_color = w[1]; break;
    case RDP_OP_SET_BLEND_COLOR: st->blend_color = w[1]; break;
    case RDP_OP_SET_PRIM_COLOR:
        st->prim_min_level = (uint8_t)((w[0] >> 8) & 31);
        st->prim_lod_frac = (uint8_t)(w[0] & 0xff);
        st->prim_color = w[1];
        break;
    case RDP_OP_SET_ENV_COLOR: st->env_color = w[1]; break;
    case RDP_OP_SET_COMBINE: rdp_set_combine(st, w); break;
    case RDP_OP_SET_TEXTURE_IMAGE:
        st->tex_fmt = (uint8_t)((w[0] >> 21) & 7);
        st->tex_size = (uint8_t)((w[0] >> 19) & 3);
        st->tex_width = (w[0] & 0x3ff) + 1;
        st->tex_addr = w[1] & 0xffffffu;
        break;
    case RDP_OP_SET_MASK_IMAGE: st->depth_addr = w[1] & 0xffffffu; break;
    case RDP_OP_SET_COLOR_IMAGE: {
        uint32_t fmt = (w[0] >> 21) & 7;
        uint32_t size = (w[0] >> 19) & 3;
        switch (size) {
        case 0: st->fb_fmt = N64_RDP_FB_I4; break;
        case 1: st->fb_fmt = N64_RDP_FB_I8; break;
        case 2: st->fb_fmt = fmt ? N64_RDP_FB_IA88 : N64_RDP_FB_RGBA5551; break;
        default: st->fb_fmt = N64_RDP_FB_RGBA8888; break;
        }
        st->fb_width = (w[0] & 1023) + 1;
        st->fb_addr = w[1] & 0xffffffu;
        break;
    }
    default:
        /* NOP, SYNC_LOAD, SYNC_PIPE, SYNC_TILE and the undefined opcodes
         * do nothing. */
        break;
    }
}

static void rdp_execute_words(N64Rdp *rdp, const uint64_t *cmd, uint32_t len)
{
    uint32_t w[2 * N64_RDP_MAX_COMMAND_WORDS];
    uint32_t i;
    for (i = 0; i < len; i++) {
        w[2 * i] = (uint32_t)(cmd[i] >> 32);
        w[2 * i + 1] = (uint32_t)cmd[i];
    }
    rdp_execute_command(rdp, w);
}

void n64_rdp_execute(N64Rdp *rdp, const uint64_t *words, size_t count)
{
    size_t i = 0;
    while (i < count) {
        uint32_t len = rdp_command_length((uint32_t)(words[i] >> 56) & 63);
        if (count - i < len)
            break;
        rdp_execute_words(rdp, words + i, len);
        i += len;
    }
}

/* Read one 64-bit command word at DPC_CURRENT into the command buffer, and
 * run the command when it is complete. */
static void rdp_push_word(N64Rdp *rdp, uint64_t word)
{
    N64RdpState *st = rdp->state;
    uint32_t len;
    st->cmd_buf[st->cmd_words++] = word;
    len = rdp_command_length((uint32_t)(st->cmd_buf[0] >> 56) & 63);
    if (st->cmd_words >= len) {
        st->cmd_words = 0;
        rdp_execute_words(rdp, st->cmd_buf, len);
    }
}

static void rdp_run(N64Rdp *rdp)
{
    if (rdp->status & N64_DPC_STATUS_FREEZE)
        return;
    while (rdp->current < rdp->end) {
        uint64_t word;
        if (rdp->status & N64_DPC_STATUS_XBUS) {
            word = rdp->links.dmem ? n64_be64(rdp->links.dmem + (rdp->current & 0xff8u)) : 0;
        } else {
            word = ((uint64_t)n64_rdp_rdram_read32(rdp, rdp->current) << 32) |
                   n64_rdp_rdram_read32(rdp, rdp->current + 4);
        }
        rdp->current = (rdp->current + 8) & 0xffffffu;
        rdp_push_word(rdp, word);
    }
    rdp->status &= ~(N64_DPC_STATUS_END_VALID | N64_DPC_STATUS_CMD_BUSY | N64_DPC_STATUS_PIPE_BUSY |
                     N64_DPC_STATUS_TMEM_BUSY | N64_DPC_STATUS_DMA_BUSY | N64_DPC_STATUS_START_GCLK);
    rdp->status |= N64_DPC_STATUS_CBUF_READY;
}

bool n64_rdp_init(N64Rdp *rdp, uint8_t *rdram, uint32_t rdram_size, const uint8_t *dmem)
{
    memset(rdp, 0, sizeof *rdp);
    rdp->state = (N64RdpState *)calloc(1, sizeof(N64RdpState));
    /* One byte of hidden bits per 16-bit RDRAM word, as parallel-rdp
     * keeps them: two bits each. */
    rdp->links.hidden = (uint8_t *)malloc(rdram_size / 2 ? rdram_size / 2 : 1);
    if (!rdp->state || !rdp->links.hidden) {
        n64_rdp_free(rdp);
        return false;
    }
    rdp->links.rdram = rdram;
    rdp->links.rdram_size = rdram_size;
    rdp->links.dmem = dmem;
    n64_rdp_reset(rdp);
    return true;
}

void n64_rdp_free(N64Rdp *rdp)
{
    n64_rdp_free_load_plans(rdp);
    n64_rdp_set_scale(rdp, 1);
    free(rdp->state);
    free(rdp->links.hidden);
    rdp->state = NULL;
    rdp->links.hidden = NULL;
}

void n64_rdp_reset(N64Rdp *rdp)
{
    N64RdpState *st = rdp->state;
    int i;
    rdp->start = rdp->end = rdp->current = 0;
    rdp->clock = rdp->bufbusy = rdp->pipebusy = rdp->tmem = 0;
    rdp->status = N64_DPC_STATUS_CBUF_READY;
    memset(st, 0, sizeof *st);
    /* parallel-rdp's power-on defaults: 16-bit tiles, an 8-bit colour
     * image of width 0 (nothing is drawn until SET_COLOR_IMAGE). */
    for (i = 0; i < 8; i++)
        st->tiles[i].size = RDP_SIZE_16;
    st->fb_fmt = N64_RDP_FB_I8;
    /* parallel-rdp clears hidden RDRAM to all ones (0x03 per word). */
    if (rdp->links.hidden)
        memset(rdp->links.hidden, 0x03, rdp->links.rdram_size / 2);
    /* The larger picture starts again too. */
    {
        int scale = n64_rdp_scale(rdp);
        n64_rdp_set_scale(rdp, 1);
        n64_rdp_set_scale(rdp, scale);
    }
}

uint32_t n64_rdp_reg_read(N64Rdp *rdp, unsigned reg)
{
    switch (reg & 7) {
    case N64_DPC_REG_START: return rdp->start;
    case N64_DPC_REG_END: return rdp->end;
    case N64_DPC_REG_CURRENT: return rdp->current;
    case N64_DPC_REG_STATUS: return rdp->status;
    case N64_DPC_REG_CLOCK: return rdp->clock;
    case N64_DPC_REG_BUFBUSY: return rdp->bufbusy;
    case N64_DPC_REG_PIPEBUSY: return rdp->pipebusy;
    default: return rdp->tmem;
    }
}

void n64_rdp_reg_write(N64Rdp *rdp, unsigned reg, uint32_t value)
{
    switch (reg & 7) {
    case N64_DPC_REG_START:
        /* A second START before END keeps the first. */
        if (!(rdp->status & N64_DPC_STATUS_START_VALID)) {
            rdp->start = value & 0xfffff8u;
            rdp->status |= N64_DPC_STATUS_START_VALID;
        }
        break;
    case N64_DPC_REG_END:
        rdp->end = value & 0xfffff8u;
        if (rdp->status & N64_DPC_STATUS_START_VALID) {
            rdp->current = rdp->start;
            rdp->status &= ~N64_DPC_STATUS_START_VALID;
        }
        rdp->status |= N64_DPC_STATUS_END_VALID;
        rdp_run(rdp);
        break;
    case N64_DPC_REG_STATUS:
        if (value & 0x001)
            rdp->status &= ~N64_DPC_STATUS_XBUS;
        if (value & 0x002)
            rdp->status |= N64_DPC_STATUS_XBUS;
        if (value & 0x008)
            rdp->status |= N64_DPC_STATUS_FREEZE;
        if (value & 0x010)
            rdp->status &= ~N64_DPC_STATUS_FLUSH;
        if (value & 0x020)
            rdp->status |= N64_DPC_STATUS_FLUSH;
        if (value & 0x040)
            rdp->tmem = 0;
        if (value & 0x080)
            rdp->pipebusy = 0;
        if (value & 0x100)
            rdp->bufbusy = 0;
        if (value & 0x200)
            rdp->clock = 0;
        /* Unfreezing runs whatever arrived while frozen. */
        if ((value & 0x004) && !(value & 0x008)) {
            rdp->status &= ~N64_DPC_STATUS_FREEZE;
            rdp_run(rdp);
        }
        break;
    default:
        /* CURRENT and the counters are read-only. */
        break;
    }
}
