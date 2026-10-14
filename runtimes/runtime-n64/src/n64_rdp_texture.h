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
 * The texture unit: coordinate clamp, wrap, mirror, mask and shift, texel
 * fetch for every format and size (TLUT, YUV, the 32-bit split across the
 * two TMEM halves), bilinear and mid-texel filtering, the convert unit,
 * LOD and mip selection, the copy pipe's 64-bit fetch, and the perspective
 * divide.
 *
 * Ported from parallel-rdp (commit 388d70f5835b352d841d9d9e5a08c5de01470f41)
 * shaders/texture.h and shaders/perspective.h, function for function.
 *
 * TMEM is stored here big-endian as the RDP addresses it, while the
 * shaders index a buffer of host-order 32-bit words. tmem8_at() and
 * tmem16_at() take the shaders' buffer indices and undo that swizzle
 * (byte index ^ 3, 16-bit index ^ 1), so every index expression below is
 * the shader's own.
 */
#ifndef N64_RDP_TEXTURE_H
#define N64_RDP_TEXTURE_H

#include "n64_rdp_internal.h"

RDP_FORCE_INLINE int32_t tmem8_at(const N64RdpState *st, uint32_t index)
{
    return st->tmem[(index ^ 3u) & 0xfffu];
}

RDP_FORCE_INLINE int32_t tmem16_at(const N64RdpState *st, uint32_t index)
{
    uint32_t w = (index ^ 1u) & 0x7ffu;
    return (int32_t)(((uint32_t)st->tmem[2 * w] << 8) | st->tmem[2 * w + 1]);
}

RDP_FORCE_INLINE int32_t texel_mask_s(const N64RdpTile *tile, int32_t s)
{
    if (tile->mask_s != 0) {
        int32_t mask = 1 << tile->mask_s;
        if (tile->flags & N64_RDP_TILE_MIRROR_S)
            s ^= rdp_max((s & mask) - 1, 0);
        s &= mask - 1;
    }
    return s;
}

RDP_FORCE_INLINE int32_t texel_mask_t(const N64RdpTile *tile, int32_t t)
{
    if (tile->mask_t != 0) {
        int32_t mask = 1 << tile->mask_t;
        if (tile->flags & N64_RDP_TILE_MIRROR_T)
            t ^= rdp_max((t & mask) - 1, 0);
        t &= mask - 1;
    }
    return t;
}

RDP_INLINE void set4(int32_t out[4], int32_t r, int32_t g, int32_t b, int32_t a)
{
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
}

/* A texel of any format but YUV has four channels of 0 to 255; the fetches
 * below return them in one word, red in the low byte. */
RDP_FORCE_INLINE uint32_t pack4(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

RDP_INLINE void unpack4(uint32_t texel, int32_t out[4])
{
    set4(out, (int32_t)(texel & 0xffu), (int32_t)((texel >> 8) & 0xffu), (int32_t)((texel >> 16) & 0xffu),
         (int32_t)(texel >> 24));
}

RDP_FORCE_INLINE uint32_t convert_rgba16(uint32_t word)
{
    uint32_t r = (word >> 11) & 31u, g = (word >> 6) & 31u, b = (word >> 1) & 31u;
    return pack4((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (word & 1u) * 0xffu);
}

RDP_FORCE_INLINE uint32_t convert_ia16(uint32_t word)
{
    uint32_t i = word >> 8;
    return pack4(i, i, i, word & 0xffu);
}

/* The byte a 4-bit or 8-bit texel lives in: odd lines swap 32-bit words. */
RDP_FORCE_INLINE uint32_t byte_index(const N64RdpTile *tile, uint32_t s_bytes, uint32_t t, uint32_t wrap)
{
    uint32_t byte_offset = (tile->offset + tile->stride * t + s_bytes) & wrap;
    return (byte_offset ^ ((t & 1u) << 2)) ^ 3u;
}

/* The 16-bit word a 16-bit or 32-bit texel lives in. */
RDP_FORCE_INLINE uint32_t word_index(const N64RdpTile *tile, uint32_t s, uint32_t t, uint32_t wrap)
{
    uint32_t byte_offset = (tile->offset + tile->stride * t + s * 2u) & wrap;
    return ((byte_offset >> 1) ^ ((t & 1u) << 1)) ^ 1u;
}

RDP_FORCE_INLINE uint32_t sample_texel_rgba4(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t)
{
    uint32_t shift = (~s & 1u) * 4u;
    uint32_t word = ((uint32_t)tmem8_at(st, byte_index(tile, s >> 1, t, 0xfff)) >> shift) & 0xfu;
    word |= word << 4;
    return pack4(word, word, word, word);
}

RDP_FORCE_INLINE uint32_t sample_texel_ia4(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t)
{
    uint32_t shift = (~s & 1u) * 4u;
    uint32_t word = ((uint32_t)tmem8_at(st, byte_index(tile, s >> 1, t, 0xfff)) >> shift) & 0xfu;
    uint32_t intensity = word & 0xeu;
    intensity = (intensity << 4) | (intensity << 1) | (intensity >> 2);
    return pack4(intensity, intensity, intensity, (word & 1u) * 0xffu);
}

RDP_FORCE_INLINE uint32_t sample_texel_ci4(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t,
                                           uint32_t pal)
{
    uint32_t shift = (~s & 1u) * 4u;
    uint32_t word = ((uint32_t)tmem8_at(st, byte_index(tile, s >> 1, t, 0xfff)) >> shift) & 0xfu;
    word |= pal << 4;
    return pack4(word, word, word, word);
}

RDP_FORCE_INLINE uint32_t tlut_lookup(const N64RdpState *st, uint32_t lut_entry, int tlut_type)
{
    uint32_t word = (uint32_t)tmem16_at(st, 0x400u | lut_entry);
    return tlut_type ? convert_ia16(word) : convert_rgba16(word);
}

RDP_FORCE_INLINE uint32_t sample_texel_ci4_tlut(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t,
                                                uint32_t pal, uint32_t lut_offset, uint32_t addr_xor, int tlut_type)
{
    uint32_t shift = (~s & 1u) * 4u;
    uint32_t word = ((uint32_t)tmem8_at(st, byte_index(tile, s >> 1, t, 0x7ff)) >> shift) & 0xfu;
    word |= pal << 4;
    return tlut_lookup(st, ((word << 2) + lut_offset) ^ addr_xor, tlut_type);
}

RDP_FORCE_INLINE uint32_t sample_texel_ci8_tlut(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t,
                                                uint32_t lut_offset, uint32_t addr_xor, int tlut_type)
{
    uint32_t word = (uint32_t)tmem8_at(st, byte_index(tile, s, t, 0x7ff));
    return tlut_lookup(st, ((word << 2) + lut_offset) ^ addr_xor, tlut_type);
}

RDP_FORCE_INLINE uint32_t sample_texel_ci32(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t)
{
    uint32_t word = (uint32_t)tmem16_at(st, word_index(tile, s, t, 0xfff));
    return pack4(word >> 8, word & 0xffu, word >> 8, word & 0xffu);
}

RDP_FORCE_INLINE uint32_t sample_texel_ci32_tlut(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t,
                                                 uint32_t lut_offset, uint32_t addr_xor, int tlut_type)
{
    uint32_t word = (uint32_t)tmem16_at(st, word_index(tile, s, t, 0x7ff));
    return tlut_lookup(st, (((word >> 6) & ~3u) + lut_offset) ^ addr_xor, tlut_type);
}

RDP_FORCE_INLINE uint32_t sample_texel_rgba8(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t)
{
    uint32_t word = (uint32_t)tmem8_at(st, byte_index(tile, s, t, 0xfff));
    return pack4(word, word, word, word);
}

RDP_FORCE_INLINE uint32_t sample_texel_ia8(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t)
{
    uint32_t word = (uint32_t)tmem8_at(st, byte_index(tile, s, t, 0xfff));
    uint32_t intensity = word >> 4;
    uint32_t alpha = word & 0xfu;
    alpha |= alpha << 4;
    intensity |= intensity << 4;
    return pack4(intensity, intensity, intensity, alpha);
}

RDP_INLINE void sample_texel_yuv16(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t,
                                   uint32_t chroma_x, int32_t out[4])
{
    uint32_t byte_offset = tile->offset + tile->stride * t;
    uint32_t index_luma = (((byte_offset + s) & 0x7ffu) ^ ((t & 1u) << 2)) ^ 3u;
    uint32_t index_chroma = ((((byte_offset + chroma_x * 2u) & 0x7ffu) >> 1) ^ ((t & 1u) << 1)) ^ 1u;
    int32_t luma = tmem8_at(st, index_luma | 0x800u);
    int32_t chroma = tmem16_at(st, index_chroma);
    int32_t u = (chroma >> 8) & 0xff;
    int32_t v = chroma & 0xff;
    set4(out, u - 0x80, v - 0x80, luma, luma);
}

RDP_FORCE_INLINE uint32_t sample_texel_rgba16(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t)
{
    return convert_rgba16((uint32_t)tmem16_at(st, word_index(tile, s, t, 0xfff)));
}

RDP_FORCE_INLINE uint32_t sample_texel_ia16(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t)
{
    return convert_ia16((uint32_t)tmem16_at(st, word_index(tile, s, t, 0xfff)));
}

RDP_FORCE_INLINE uint32_t sample_texel_rgba32(const N64RdpState *st, const N64RdpTile *tile, uint32_t s, uint32_t t)
{
    uint32_t index = word_index(tile, s, t, 0x7ff);
    uint32_t lower = (uint32_t)tmem16_at(st, index);
    uint32_t upper = (uint32_t)tmem16_at(st, index | 0x400u);
    return pack4(lower >> 8, lower & 0xffu, upper >> 8, upper & 0xffu);
}

/* Shift a 17-bit coordinate and move it to the tile's origin, clamping it
 * to the tile when the clamp bit is set (10.5 fixed point out). */
RDP_FORCE_INLINE int32_t clamp_and_shift_coord(int clamp_bit, int32_t coord, int32_t lo, int32_t hi, int32_t shift)
{
    coord = rdp_clamp(coord, -0x8000, 0x7fff);
    if (shift < 11)
        coord = rdp_asr(coord, shift);
    else
        coord = rdp_asr(rdp_shl(coord, 32 - shift), 16);

    if (clamp_bit) {
        int clamp_hi = rdp_asr(coord, 3) >= hi;
        if (clamp_hi)
            coord = (((hi >> 2) - (lo >> 2)) & 0x3ff) << 5;
        else
            coord = rdp_max(coord - (lo << 3), 0);
    } else {
        coord -= lo << 3;
    }
    return coord;
}

RDP_INLINE int32_t shift_coord(int32_t coord, int32_t lo, int32_t shift)
{
    coord = rdp_clamp(coord, -0x8000, 0x7fff);
    if (shift < 11)
        coord = rdp_asr(coord, shift);
    else
        coord = rdp_asr(rdp_shl(coord, 32 - shift), 16);
    return coord - (lo << 3);
}

/* The copy pipe reads 4x16-bit words; this returns the one for s_offset. */
RDP_INLINE int32_t sample_texture_copy_word(const N64RdpState *st, const N64RdpTile *tile, int32_t s, int32_t t,
                                            int32_t s_offset, int tlut)
{
    int high_word = s_offset < 2;
    int replicate_8bpp = high_word && tile->size != 2 && !tlut;
    int32_t samp;
    int32_t s_shamt = rdp_min(tile->size, 2);
    int large_texel = tile->size == 3;
    uint32_t idx_mask = (large_texel || tlut) ? 0x3ffu : 0x7ffu;

    if (replicate_8bpp) {
        /* The high word of 8-bpp replication samples two 8-bit texels. */
        int32_t s0, s1, samp0, samp1, tm;
        uint32_t tbase, nib0, nib1, index0, index1;
        s += 2 * s_offset;
        s0 = s;
        s1 = s + 1;
        if (tile->mask_s != 0) {
            int32_t mask = 1 << tile->mask_s;
            if (tile->flags & N64_RDP_TILE_MIRROR_S) {
                s0 ^= rdp_max((s0 & mask) - 1, 0);
                s1 ^= rdp_max((s1 & mask) - 1, 0);
            }
            s0 &= mask - 1;
            s1 &= mask - 1;
        }
        tm = texel_mask_t(tile, t);
        tbase = tile->offset + tile->stride * (uint32_t)tm;
        nib0 = (tbase * 2u + ((uint32_t)s0 << s_shamt)) & 0x1fffu;
        nib1 = (tbase * 2u + ((uint32_t)s1 << s_shamt)) & 0x1fffu;
        nib0 ^= ((uint32_t)tm & 1u) * 8u;
        nib1 ^= ((uint32_t)tm & 1u) * 8u;
        index0 = (nib0 >> 2) & idx_mask;
        index1 = (nib1 >> 2) & idx_mask;
        samp0 = tmem16_at(st, index0 ^ 1u);
        samp1 = tmem16_at(st, index1 ^ 1u);

        if (tile->size == 1) {
            samp0 >>= 8 - 4 * (int32_t)(nib0 & 2u);
            samp1 >>= 8 - 4 * (int32_t)(nib1 & 2u);
            samp0 &= 0xff;
            samp1 &= 0xff;
        } else if (tile->size == 0) {
            samp0 >>= 12 - 4 * (int32_t)(nib0 & 3u);
            samp1 >>= 12 - 4 * (int32_t)(nib1 & 3u);
            samp0 = (samp0 & 0xf) * 0x11;
            samp1 = (samp1 & 0xf) * 0x11;
        } else {
            samp0 >>= 8;
            samp1 >>= 8;
        }
        samp = (samp0 << 8) | samp1;
    } else {
        int32_t sm, tm;
        uint32_t tbase, nibble_offset, index;
        s += s_offset;
        sm = texel_mask_s(tile, s);
        tm = texel_mask_t(tile, t);
        tbase = tile->offset + tile->stride * (uint32_t)tm;
        nibble_offset = (tbase * 2u + ((uint32_t)sm << s_shamt)) & 0x1fffu;
        nibble_offset ^= ((uint32_t)tm & 1u) * 8u;
        index = (nibble_offset >> 2) & idx_mask;
        samp = tmem16_at(st, index ^ 1u);

        if (tlut) {
            if (tile->size == 0) {
                samp >>= 12 - 4 * (int32_t)(nibble_offset & 3u);
                samp &= 0xf;
                samp |= tile->palette << 4;
            } else {
                samp >>= 8 - 4 * (int32_t)(nibble_offset & 2u);
                samp &= 0xff;
            }
            samp <<= 2;
            samp += s_offset;
            samp = tmem16_at(st, ((uint32_t)samp | 0x400u) ^ 1u);
        }
    }
    return samp;
}

RDP_INLINE int32_t n64_rdp_sample_texture_copy(const N64Rdp *rdp, const RdpPrimitive *prim, const N64RdpTile *tile,
                                               int32_t s, int32_t t, int32_t s_offset, int tlut)
{
    const N64RdpState *st = rdp->state;
    int32_t samp;
    s = rdp_asr(shift_coord(s, (int32_t)tile->slo, tile->shift_s), 5);
    t = rdp_asr(shift_coord(t, (int32_t)tile->tlo, tile->shift_t), 5);

    if (prim->fb_size == 0) {
        samp = 0;
    } else if (prim->fb_size == 1) {
        samp = sample_texture_copy_word(st, tile, s, t, s_offset >> 1, tlut);
        samp >>= 8 - 8 * (s_offset & 1);
        samp &= 0xff;
    } else {
        samp = sample_texture_copy_word(st, tile, s, t, s_offset, tlut);
    }
    return samp;
}

/* 3-tap bilinear on two channels (the YUV paths). */
RDP_INLINE void bilinear_3tap(const int32_t *t00, const int32_t *t10, const int32_t *t01, const int32_t *t11,
                              int32_t frac_x, int32_t frac_y, int32_t out[2])
{
    int32_t sum_frac = frac_x + frac_y;
    const int32_t *t_base = sum_frac >= 32 ? t11 : t00;
    int32_t fx = sum_frac >= 32 ? 32 - frac_y : frac_x;
    int32_t fy = sum_frac >= 32 ? 32 - frac_x : frac_y;
    int c;
    for (c = 0; c < 2; c++) {
        int32_t accum = (t10[c] - t_base[c]) * fx;
        accum += (t01[c] - t_base[c]) * fy;
        accum += 0x10;
        out[c] = rdp_asr(accum, 5) + t_base[c];
    }
}

RDP_INLINE void n64_rdp_texture_convert_factors(const int32_t texel_in[4], const int32_t factors[4], int32_t out[4])
{
    int32_t r = rdp_sext(texel_in[0], 9), g = rdp_sext(texel_in[1], 9), b = rdp_sext(texel_in[2], 9);
    set4(out, b + rdp_asr(factors[0] * g + 0x80, 8), b + rdp_asr(factors[1] * r + factors[2] * g + 0x80, 8),
         b + rdp_asr(factors[3] * r + 0x80, 8), b);
}

/* Fetch one texel of the given tile at integer (s, t), any format but
 * YUV, packed. */
RDP_FORCE_INLINE uint32_t sample_texel_packed(const N64RdpState *st, const N64RdpTile *tile, int32_t s, int32_t t)
{
    uint32_t us = (uint32_t)s, ut = (uint32_t)t;
    switch (tile->fmt) {
    case RDP_FMT_RGBA:
        switch (tile->size) {
        case 0: return sample_texel_rgba4(st, tile, us, ut);
        case 1: return sample_texel_rgba8(st, tile, us, ut);
        case 2: return sample_texel_rgba16(st, tile, us, ut);
        default: return sample_texel_rgba32(st, tile, us, ut);
        }
    case RDP_FMT_CI:
        switch (tile->size) {
        case 0: return sample_texel_ci4(st, tile, us, ut, tile->palette);
        case 1: return sample_texel_rgba8(st, tile, us, ut);
        default: return sample_texel_ci32(st, tile, us, ut);
        }
    case RDP_FMT_IA:
        switch (tile->size) {
        case 0: return sample_texel_ia4(st, tile, us, ut);
        case 1: return sample_texel_ia8(st, tile, us, ut);
        case 2: return sample_texel_ia16(st, tile, us, ut);
        default: return sample_texel_ci32(st, tile, us, ut);
        }
    case RDP_FMT_I:
        switch (tile->size) {
        case 0: return sample_texel_rgba4(st, tile, us, ut);
        case 1: return sample_texel_rgba8(st, tile, us, ut);
        default: return sample_texel_ci32(st, tile, us, ut);
        }
    default:
        /* Formats 5-7: texture.h leaves the texel unwritten; zero here. */
        return 0;
    }
}

RDP_INLINE void sample_texel(const N64RdpState *st, const N64RdpTile *tile, int32_t s, int32_t t, int32_t out[4])
{
    unpack4(sample_texel_packed(st, tile, s, t), out);
}

RDP_FORCE_INLINE uint32_t sample_texel_tlut_packed(const N64RdpState *st, const N64RdpTile *tile, int32_t s, int32_t t,
                                                   uint32_t lut_offset, uint32_t addr_xor, int tlut_type)
{
    uint32_t us = (uint32_t)s, ut = (uint32_t)t;
    switch (tile->size) {
    case 0: return sample_texel_ci4_tlut(st, tile, us, ut, tile->palette, lut_offset, addr_xor, tlut_type);
    case 1: return sample_texel_ci8_tlut(st, tile, us, ut, lut_offset, addr_xor, tlut_type);
    default: return sample_texel_ci32_tlut(st, tile, us, ut, lut_offset, addr_xor, tlut_type);
    }
}

RDP_INLINE void sample_texel_tlut(const N64RdpState *st, const N64RdpTile *tile, int32_t s, int32_t t,
                                  uint32_t lut_offset, uint32_t addr_xor, int tlut_type, int32_t out[4])
{
    unpack4(sample_texel_tlut_packed(st, tile, s, t, lut_offset, addr_xor, tlut_type), out);
}

/* Four channels of 0 to 255 spread to 16 bits each, so that sums and small
 * products of the four can be worked out in one 64-bit operation. */
RDP_FORCE_INLINE uint64_t spread4(uint32_t texel)
{
    uint64_t x = texel;
    x = (x | (x << 16)) & 0x0000ffff0000ffffull;
    return (x | (x << 8)) & 0x00ff00ff00ff00ffull;
}

/* n64_rdp_sample_texture() for a tile of any format but YUV in a cycle with
 * bilerp on and no convert_one, which is what games draw with: the same
 * coordinates and texels, with the four channels filtered together. The
 * filter (t10 - base) * fx + (t01 - base) * fy + 16 >> 5, plus base, is
 * base * (32 - fx - fy) + t10 * fx + t01 * fy + 16 >> 5: the same value, as
 * 32 * base is added before the shift, with no term negative and none above
 * 255 * 32 + 16, so the channels cannot borrow or carry into each other.
 * Returns the texel packed.
 *
 * `last` remembers the texels of the footprint the previous call fetched.
 * Along a scanline of a magnified texture most pixels land on the
 * footprint of the one before, and only the fractions differ; TMEM and the
 * tiles do not change while a primitive is drawn. */
typedef struct RdpTexelFootprint {
    const N64RdpTile *tile; /* NULL: nothing remembered */
    int32_t s0, s1, t0, t1;
    int32_t corner; /* which corner is the base, the TLUT bank, and whether all four were fetched */
    uint32_t t_base, t10, t01, t11;
} RdpTexelFootprint;

RDP_FORCE_INLINE uint32_t n64_rdp_sample_texture_bilerp(const N64Rdp *rdp, const N64RdpTile *tile, int32_t s, int32_t t,
                                                        int tlut, int tlut_type, int sample_quad,
                                                        int mid_texel_state, RdpTexelFootprint *last)
{
    const N64RdpState *st = rdp->state;
    int32_t frac_x, frac_y, sum_frac, s0, t0, s1, t1, tdiff, base_s, base_t, corner;
    uint32_t t_base = 0, t10 = 0, t01 = 0, t11 = 0;
    uint64_t accum;
    int mid_texel, upper_lut;

    s = clamp_and_shift_coord((tile->flags & N64_RDP_TILE_CLAMP_S) != 0, s, (int32_t)tile->slo, (int32_t)tile->shi,
                              tile->shift_s);
    t = clamp_and_shift_coord((tile->flags & N64_RDP_TILE_CLAMP_T) != 0, t, (int32_t)tile->tlo, (int32_t)tile->thi,
                              tile->shift_t);
    if (sample_quad || tlut) {
        frac_x = s & 31;
        frac_y = t & 31;
    } else {
        frac_x = frac_y = 0;
    }
    sum_frac = frac_x + frac_y;
    s = rdp_asr(s, 5);
    t = rdp_asr(t, 5);

    s0 = texel_mask_s(tile, s);
    t0 = texel_mask_t(tile, t);
    s1 = texel_mask_s(tile, s + 1);
    t1 = texel_mask_t(tile, t + 1);
    tdiff = rdp_max(t1 - t0, -255);
    t1 = (t0 & 0xff) + tdiff;
    t0 &= 0xff;

    mid_texel = mid_texel_state && frac_x == 0x10 && frac_y == 0x10;
    upper_lut = sum_frac >= 0x20;
    if (mid_texel)
        sum_frac = 0;
    base_s = sum_frac >= 0x20 ? s1 : s0;
    base_t = sum_frac >= 0x20 ? t1 : t0;

    if (tlut && !sample_quad) {
        base_s = s0;
        base_t = t0;
        s1 = s0;
        t1 = t0;
    }
    corner = (sum_frac >= 0x20) | (upper_lut << 1) | (mid_texel << 2);
    if (last->tile == tile && last->s0 == s0 && last->s1 == s1 && last->t0 == t0 && last->t1 == t1 &&
        last->corner == corner) {
        t_base = last->t_base;
        t10 = last->t10;
        t01 = last->t01;
        t11 = last->t11;
    } else {
        if (tlut) {
            if (tile->fmt == RDP_FMT_RGBA || tile->fmt == RDP_FMT_CI || tile->fmt == RDP_FMT_IA ||
                tile->fmt == RDP_FMT_I) {
                uint32_t addr_xor = upper_lut ? 2u : 1u;
                t_base = sample_texel_tlut_packed(st, tile, base_s, base_t, sum_frac >= 0x20 ? 3u : 0u, addr_xor,
                                                  tlut_type);
                t10 = sample_texel_tlut_packed(st, tile, s1, t0, 1, addr_xor, tlut_type);
                t01 = sample_texel_tlut_packed(st, tile, s0, t1, 2, addr_xor, tlut_type);
                if (mid_texel)
                    t11 = sample_texel_tlut_packed(st, tile, s1, t1, 3, addr_xor, tlut_type);
            }
        } else {
            t_base = sample_texel_packed(st, tile, base_s, base_t);
            if (sample_quad) {
                t10 = sample_texel_packed(st, tile, s1, t0);
                t01 = sample_texel_packed(st, tile, s0, t1);
            }
            if (mid_texel)
                t11 = sample_texel_packed(st, tile, s1, t1);
        }
        last->tile = tile;
        last->s0 = s0;
        last->s1 = s1;
        last->t0 = t0;
        last->t1 = t1;
        last->corner = corner;
        last->t_base = t_base;
        last->t10 = t10;
        last->t01 = t01;
        last->t11 = t11;
    }

    if (mid_texel) {
        accum = (spread4(t_base) + spread4(t01) + spread4(t10) + spread4(t11) + 0x0002000200020002ull) >> 2;
    } else if (sample_quad || tlut) {
        uint32_t fx = (uint32_t)(sum_frac >= 32 ? 32 - frac_y : frac_x);
        uint32_t fy = (uint32_t)(sum_frac >= 32 ? 32 - frac_x : frac_y);
        accum = spread4(t_base) * (32u - fx - fy) + spread4(t10) * fx + spread4(t01) * fy;
        accum = (accum + 0x0010001000100010ull) >> 5;
    } else {
        accum = spread4(t_base);
    }
    return pack4((uint32_t)(accum & 0xffu), (uint32_t)((accum >> 16) & 0xffu), (uint32_t)((accum >> 32) & 0xffu),
                 (uint32_t)((accum >> 48) & 0xffu));
}

RDP_INLINE void n64_rdp_sample_texture(const N64Rdp *rdp, const N64RdpTile *tile, int32_t s, int32_t t, int tlut,
                                       int tlut_type, int sample_quad, int mid_texel_state, int convert_one, int bilerp,
                                       const int32_t factors[4], const int32_t prev_cycle[4], int32_t out[4])
{
    const N64RdpState *st = rdp->state;
    int32_t frac_x, frac_y, sum_frac, s0, t0, s1, t1, tdiff, chroma_frac;
    int32_t base_s, base_t;
    int32_t t_base[4] = {0, 0, 0, 0}, t10[4] = {0, 0, 0, 0}, t01[4] = {0, 0, 0, 0}, t11[4] = {0, 0, 0, 0};
    int32_t accum[4];
    int mid_texel, upper_lut, yuv;
    int c;

    s = clamp_and_shift_coord((tile->flags & N64_RDP_TILE_CLAMP_S) != 0, s, (int32_t)tile->slo, (int32_t)tile->shi,
                              tile->shift_s);
    t = clamp_and_shift_coord((tile->flags & N64_RDP_TILE_CLAMP_T) != 0, t, (int32_t)tile->tlo, (int32_t)tile->thi,
                              tile->shift_t);

    if (sample_quad || tlut) {
        frac_x = s & 31;
        frac_y = t & 31;
    } else {
        frac_x = frac_y = 0;
    }
    sum_frac = frac_x + frac_y;
    s = rdp_asr(s, 5);
    t = rdp_asr(t, 5);

    s0 = texel_mask_s(tile, s);
    t0 = texel_mask_t(tile, t);
    s1 = texel_mask_s(tile, s + 1);
    t1 = texel_mask_t(tile, t + 1);

    /* Very specific logic for t0 and t1, as parallel-rdp has it. */
    tdiff = rdp_max(t1 - t0, -255);
    t1 = (t0 & 0xff) + tdiff;
    t0 &= 0xff;

    mid_texel = mid_texel_state && bilerp && frac_x == 0x10 && frac_y == 0x10;
    upper_lut = sum_frac >= 0x20;
    if (mid_texel)
        sum_frac = 0; /* sample all four texels */

    yuv = tile->fmt == RDP_FMT_YUV;
    base_s = sum_frac >= 0x20 ? s1 : s0;
    base_t = sum_frac >= 0x20 ? t1 : t0;
    chroma_frac = ((s0 & 1) << 4) | (frac_x >> 1);

    if (tlut) {
        if (!sample_quad) {
            /* Sample the bilinear footprint across the four TLUT banks
             * instead of across texels. */
            base_s = s0;
            base_t = t0;
            s1 = s0;
            t1 = t0;
        }
        if (tile->fmt == RDP_FMT_RGBA || tile->fmt == RDP_FMT_CI || tile->fmt == RDP_FMT_IA ||
            tile->fmt == RDP_FMT_I) {
            int upper = sum_frac >= 0x20;
            uint32_t addr_xor = upper_lut ? 2u : 1u;
            sample_texel_tlut(st, tile, base_s, base_t, upper ? 3u : 0u, addr_xor, tlut_type, t_base);
            if (bilerp) {
                sample_texel_tlut(st, tile, s1, t0, 1, addr_xor, tlut_type, t10);
                sample_texel_tlut(st, tile, s0, t1, 2, addr_xor, tlut_type, t01);
            }
            if (mid_texel)
                sample_texel_tlut(st, tile, s1, t1, 3, addr_xor, tlut_type, t11);
        }
    } else if (yuv) {
        uint32_t chroma_x0 = (uint32_t)s0 >> 1;
        uint32_t chroma_x1 = (uint32_t)(s1 + (s1 - s0)) >> 1;
        /* Only 16-bit YUV gives meaningful results; texture.h samples every
         * YUV size as 16-bit. */
        sample_texel_yuv16(st, tile, (uint32_t)s0, (uint32_t)t0, chroma_x0, t_base);
        if (sample_quad) {
            sample_texel_yuv16(st, tile, (uint32_t)s1, (uint32_t)t0, chroma_x1, t10);
            sample_texel_yuv16(st, tile, (uint32_t)s0, (uint32_t)t1, chroma_x0, t01);
            sample_texel_yuv16(st, tile, (uint32_t)s1, (uint32_t)t1, chroma_x1, t11);
        }
    } else {
        sample_texel(st, tile, base_s, base_t, t_base);
        if (sample_quad) {
            sample_texel(st, tile, s1, t0, t10);
            sample_texel(st, tile, s0, t1, t01);
        }
        if (mid_texel)
            sample_texel(st, tile, s1, t1, t11);
    }

    if (convert_one) {
        /* Bilerp with convert_one: the previous cycle's texel supplies the
         * conversion factors. Without bilerp this function is not called. */
        int32_t prev[4];
        for (c = 0; c < 4; c++)
            prev[c] = rdp_sext(prev_cycle[c], 9);
        if (sample_quad) {
            int mid_rg = yuv ? (mid_texel_state && chroma_frac == 0x10 && frac_y == 0x10) : mid_texel;
            int mid_ba = mid_texel;
            int upper_ba = sum_frac >= 32;
            int upper_rg = yuv ? ((chroma_frac + frac_y) >= 32 && !mid_rg) : upper_ba;
            int32_t frg0 = upper_rg ? prev[1] : prev[0], frg1 = upper_rg ? prev[0] : prev[1];
            int32_t fba0 = upper_ba ? prev[1] : prev[0], fba1 = upper_ba ? prev[0] : prev[1];
            int32_t conv[4];

            for (c = 0; c < 2; c++) {
                if (mid_rg) {
                    conv[c] = frg0 * (t01[c] - t11[c]) + frg1 * (t10[c] - t11[c]) + rdp_shl(t_base[c] - t11[c], 6) +
                              0x80;
                } else {
                    int32_t base = upper_rg && yuv ? t11[c] : t_base[c];
                    conv[c] = frg0 * (t10[c] - base) + frg1 * (t01[c] - base) + 0x80;
                }
            }
            for (c = 2; c < 4; c++) {
                if (mid_ba) {
                    conv[c] = fba0 * (t01[c] - t11[c]) + fba1 * (t10[c] - t11[c]) + rdp_shl(t_base[c] - t11[c], 6) +
                              0x80;
                } else {
                    int32_t base = upper_ba && yuv ? t11[c] : t_base[c];
                    conv[c] = fba0 * (t10[c] - base) + fba1 * (t01[c] - base) + 0x80;
                }
            }
            for (c = 0; c < 4; c++)
                accum[c] = rdp_asr(conv[c], 8) + prev[2];
        } else {
            for (c = 0; c < 4; c++)
                accum[c] = prev[2];
        }
    } else if (yuv) {
        if (sample_quad) {
            if (bilerp) {
                int mid_chroma = mid_texel_state && chroma_frac == 0x10 && frac_y == 0x10;
                if (mid_chroma) {
                    for (c = 0; c < 2; c++)
                        accum[c] = rdp_asr(t_base[c] + t10[c] + t11[c] + t01[c] + 2, 2);
                } else {
                    bilinear_3tap(t_base, t10, t01, t11, chroma_frac, frac_y, accum);
                }
                if (mid_texel) {
                    for (c = 2; c < 4; c++)
                        accum[c] = rdp_asr(t_base[c] + t10[c] + t11[c] + t01[c] + 2, 2);
                } else {
                    bilinear_3tap(t_base + 2, t10 + 2, t01 + 2, t11 + 2, frac_x, frac_y, accum + 2);
                }
            } else {
                /* Nearest within the bilinear footprint: t00 or t11, chosen
                 * separately for chroma and luma. */
                const int32_t *luma = frac_x + frac_y >= 32 ? t11 : t_base;
                const int32_t *chroma = chroma_frac + frac_y >= 32 ? t11 : t_base;
                accum[0] = chroma[0];
                accum[1] = chroma[1];
                accum[2] = luma[2];
                accum[3] = luma[3];
            }
        } else {
            for (c = 0; c < 4; c++)
                accum[c] = t_base[c];
        }
    } else if (mid_texel) {
        for (c = 0; c < 4; c++)
            accum[c] = rdp_asr(t_base[c] + t01[c] + t10[c] + t11[c] + 2, 2);
    } else if (bilerp && (sample_quad || tlut)) {
        int32_t fx = sum_frac >= 32 ? 32 - frac_y : frac_x;
        int32_t fy = sum_frac >= 32 ? 32 - frac_x : frac_y;
        for (c = 0; c < 4; c++) {
            int32_t a = (t10[c] - t_base[c]) * fx;
            a += (t01[c] - t_base[c]) * fy;
            a += 0x10;
            accum[c] = rdp_asr(a, 5) + t_base[c];
        }
    } else {
        for (c = 0; c < 4; c++)
            accum[c] = t_base[c];
    }

    /* A cycle that spends no math on bilerp converts instead. */
    if (!bilerp && !convert_one)
        n64_rdp_texture_convert_factors(accum, factors, out);
    else
        for (c = 0; c < 4; c++)
            out[c] = accum[c];
}

RDP_INLINE void n64_rdp_compute_lod_2cycle(uint32_t *tile0, uint32_t *tile1, int32_t *lod_frac, uint32_t max_level,
                                           int32_t min_lod, const int32_t st[2], const int32_t st_dx[2],
                                           const int32_t st_dy[2], int perspective_overflow, int tex_lod_en,
                                           int sharpen_tex_en, int detail_tex_en)
{
    int magnify = 0;
    int distant = 0;
    uint32_t tile_offset = 0;

    if (perspective_overflow) {
        distant = 1;
        *lod_frac = 0xff;
    } else {
        int32_t dx0 = st_dx[0] - st[0], dx1 = st_dx[1] - st[1];
        int32_t dy0 = st_dy[0] - st[0], dy1 = st_dy[1] - st[1];
        int32_t max_d;
        /* Kinda abs, except one less than expected when negative. */
        dx0 ^= rdp_asr(dx0, 31);
        dx1 ^= rdp_asr(dx1, 31);
        dy0 ^= rdp_asr(dy0, 31);
        dy1 ^= rdp_asr(dy1, 31);
        max_d = rdp_max(rdp_max(dx0, dy0), rdp_max(dx1, dy1));

        if (max_d >= 0x4000) {
            distant = 1;
            *lod_frac = 0xff;
            tile_offset = max_level;
        } else if (max_d < 32) {
            /* LOD < 0. */
            distant = max_level == 0u;
            magnify = 1;
            if (!sharpen_tex_en && !detail_tex_en)
                *lod_frac = distant ? 0xff : 0;
            else
                *lod_frac = (rdp_max(min_lod, max_d) << 3) + (sharpen_tex_en ? -0x100 : 0);
        } else {
            int32_t mip_base = rdp_max(rdp_find_msb((uint32_t)(max_d >> 5)), 0);
            distant = (uint32_t)mip_base >= max_level;
            if (distant && !sharpen_tex_en && !detail_tex_en) {
                *lod_frac = 0xff;
            } else {
                *lod_frac = ((max_d << 3) >> mip_base) & 0xff;
                tile_offset = (uint32_t)mip_base;
            }
        }
    }

    if (tex_lod_en) {
        if (distant)
            tile_offset = max_level;
        if (!detail_tex_en) {
            *tile0 = (*tile0 + tile_offset) & 7u;
            if (distant || (!sharpen_tex_en && magnify))
                *tile1 = *tile0;
            else
                *tile1 = (*tile0 + 1) & 7u;
        } else {
            *tile1 = (*tile0 + tile_offset + ((distant || magnify) ? 1u : 2u)) & 7u;
            *tile0 = (*tile0 + tile_offset + (magnify ? 0u : 1u)) & 7u;
        }
    }
}

/* perspective.h: the reciprocal table, (base, slope * 4). */
static const int16_t perspective_table[64][2] = {
    {0x4000, -252 * 4}, {0x3f04, -244 * 4}, {0x3e10, -238 * 4}, {0x3d22, -230 * 4},
    {0x3c3c, -223 * 4}, {0x3b5d, -218 * 4}, {0x3a83, -210 * 4}, {0x39b1, -205 * 4},
    {0x38e4, -200 * 4}, {0x381c, -194 * 4}, {0x375a, -189 * 4}, {0x369d, -184 * 4},
    {0x35e5, -179 * 4}, {0x3532, -175 * 4}, {0x3483, -170 * 4}, {0x33d9, -166 * 4},
    {0x3333, -162 * 4}, {0x3291, -157 * 4}, {0x31f4, -155 * 4}, {0x3159, -150 * 4},
    {0x30c3, -147 * 4}, {0x3030, -143 * 4}, {0x2fa1, -140 * 4}, {0x2f15, -137 * 4},
    {0x2e8c, -134 * 4}, {0x2e06, -131 * 4}, {0x2d83, -128 * 4}, {0x2d03, -125 * 4},
    {0x2c86, -123 * 4}, {0x2c0b, -120 * 4}, {0x2b93, -117 * 4}, {0x2b1e, -115 * 4},
    {0x2aab, -113 * 4}, {0x2a3a, -110 * 4}, {0x29cc, -108 * 4}, {0x2960, -106 * 4},
    {0x28f6, -104 * 4}, {0x288e, -102 * 4}, {0x2828, -100 * 4}, {0x27c4, -98 * 4},
    {0x2762, -96 * 4},  {0x2702, -94 * 4},  {0x26a4, -92 * 4},  {0x2648, -91 * 4},
    {0x25ed, -89 * 4},  {0x2594, -87 * 4},  {0x253d, -86 * 4},  {0x24e7, -85 * 4},
    {0x2492, -83 * 4},  {0x243f, -81 * 4},  {0x23ee, -80 * 4},  {0x239e, -79 * 4},
    {0x234f, -77 * 4},  {0x2302, -76 * 4},  {0x22b6, -74 * 4},  {0x226c, -74 * 4},
    {0x2222, -72 * 4},  {0x21da, -71 * 4},  {0x2193, -70 * 4},  {0x214d, -69 * 4},
    {0x2108, -67 * 4},  {0x20c5, -67 * 4},  {0x2082, -65 * 4},  {0x2041, -65 * 4},
};

/* s16 divided by s1.15: (x * rcp) >> shift with the reciprocal from the
 * table. The result is 17-bit signed (LOD uses all 17 bits). */
RDP_FORCE_INLINE void n64_rdp_perspective_divide(const int32_t stw[3], int *overflow, int32_t out[2])
{
    int32_t w = stw[2];
    int w_carry = w <= 0;
    int32_t shift, normout, wnorm, rcp, temp_mask, prod[2], oob[2], temp[2];
    int c;

    w &= 0x7fff;
    shift = rdp_min(14 - rdp_find_msb((uint32_t)w), 14);
    normout = (w << shift) & 0x3fff;
    wnorm = normout & 0xff;
    rcp = rdp_asr(perspective_table[normout >> 8][1] * wnorm, 10) + perspective_table[normout >> 8][0];

    temp_mask = (int32_t)(((1u << 30) - 1u) & (0u - ((1u << 29) >> shift)));
    for (c = 0; c < 2; c++) {
        prod[c] = stw[c] * rcp;
        oob[c] = prod[c] & temp_mask;
        if (shift != 14) {
            prod[c] = rdp_asr(prod[c], 13 - shift);
            temp[c] = prod[c];
        } else {
            temp[c] = rdp_shl(prod[c], 1);
        }
    }

    for (c = 0; c < 2; c++) {
        if (oob[c] != temp_mask && oob[c] != 0) {
            temp[c] = (prod[c] & (1 << 29)) == 0 ? 0x7fff : -0x8000;
            *overflow = 1;
        }
    }

    if (w_carry) {
        temp[0] = temp[1] = 0x7fff;
        *overflow = 1;
    }

    out[0] = rdp_clamp(temp[0], -0x10000, 0xffff);
    out[1] = rdp_clamp(temp[1], -0x10000, 0xffff);
}

#endif /* N64_RDP_TEXTURE_H */
