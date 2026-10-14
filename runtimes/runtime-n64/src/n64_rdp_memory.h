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
 * The memory side of the pixel pipeline: colour and depth image reads and
 * writes in every format with the hidden ninth bits, the fill and copy
 * pipes, the depth test, depth compression and coverage update.
 *
 * Ported from parallel-rdp (commit 388d70f5835b352d841d9d9e5a08c5de01470f41)
 * shaders/memory_interfacing.h, depth_test.h, z_encode.h and the coverage
 * half of coverage.h.
 *
 * parallel-rdp keeps a pixel's colour and depth in registers while it runs
 * every primitive of a batch over that pixel, and writes them back at the
 * end. Here each primitive loads and stores the pixel itself. The stored
 * forms round-trip exactly what the next primitive reads, so the two
 * agree, with one exception noted at n64_rdp_pixel_store().
 *
 * Addresses follow the seam contract rather than parallel-rdp's wrap
 * within RDRAM: a byte address is masked to 24 bits, and bytes beyond
 * rdram_size read as zero and are not written. The hidden bits are one
 * byte per 16-bit RDRAM word, holding two bits, as in parallel-rdp.
 */
#ifndef N64_RDP_MEMORY_H
#define N64_RDP_MEMORY_H

#include "n64_rdp_internal.h"

RDP_INLINE void rdram_write8(const N64Rdp *rdp, uint32_t addr, uint8_t v)
{
    addr &= 0xffffffu;
    if (addr < rdp->links.rdram_size)
        rdp->links.rdram[addr] = v;
}

RDP_INLINE uint8_t hidden_read(const N64Rdp *rdp, uint32_t halfword)
{
    uint32_t addr = (halfword << 1) & 0xffffffu;
    return addr < rdp->links.rdram_size ? rdp->links.hidden[addr >> 1] : 0;
}

RDP_INLINE void hidden_write(const N64Rdp *rdp, uint32_t halfword, uint8_t v)
{
    uint32_t addr = (halfword << 1) & 0xffffffu;
    if (addr < rdp->links.rdram_size)
        rdp->links.hidden[addr >> 1] = v;
}

RDP_INLINE int color_depth_alias(const N64RdpState *st) { return st->fb_addr == st->depth_addr; }

/* alias_color_to_depth: a depth image on top of the colour image reads
 * the colour just written. */
RDP_INLINE void alias_color_to_depth(const N64RdpState *st, RdpPixelMemory *mem)
{
    switch (st->fb_fmt) {
    case N64_RDP_FB_RGBA5551: {
        uint32_t word;
        mem->dz = (uint8_t)((mem->color[3] >> 3) | (mem->color[2] & 8));
        word = (uint32_t)(mem->color[0] & 0xf8u) << 6;
        word |= (uint32_t)(mem->color[1] & 0xf8u) << 1;
        word |= (uint32_t)(mem->color[2] & 0xf8u) >> 4;
        mem->depth = (uint16_t)word;
        break;
    }
    case N64_RDP_FB_IA88: {
        uint32_t word = ((uint32_t)mem->color[0] << 8) | mem->color[3];
        uint32_t hidden_word = (word & 1u) * 3u;
        mem->depth = (uint16_t)(word >> 2);
        mem->dz = (uint8_t)(((word & 3u) << 2) | hidden_word);
        break;
    }
    default: break;
    }
}

RDP_INLINE void alias_depth_to_color(const N64RdpState *st, RdpPixelMemory *mem)
{
    uint32_t word = ((uint32_t)mem->depth << 4) | mem->dz;
    switch (st->fb_fmt) {
    case N64_RDP_FB_RGBA5551:
        mem->color[0] = (uint8_t)((word >> 10) & 0xf8u);
        mem->color[1] = (uint8_t)((word >> 5) & 0xf8u);
        mem->color[2] = (uint8_t)((word >> 0) & 0xf8u);
        mem->color[3] = (uint8_t)((word & 7u) << 5);
        break;
    case N64_RDP_FB_IA88:
        mem->color[0] = (uint8_t)((word >> 10) & 0xffu);
        mem->color[3] = (uint8_t)((word >> 2) & 0xffu);
        break;
    default: break;
    }
    mem->color_dirty = 1;
}

/* init_tile: load_vram_color and load_vram_depth for pixel (x, y). */
RDP_INLINE void n64_rdp_pixel_load(const N64Rdp *rdp, RdpPixelMemory *mem, uint32_t x, uint32_t y)
{
    const N64RdpState *st = rdp->state;
    uint32_t pixel = st->fb_width * y + x;
    uint32_t index;

    mem->color_dirty = 0;
    mem->depth_dirty = 0;

    switch (st->fb_fmt) {
    case N64_RDP_FB_I4:
    case N64_RDP_FB_I8: {
        uint8_t word;
        index = st->fb_addr + pixel;
        word = n64_rdp_rdram_read8(rdp, index);
        mem->color[0] = mem->color[1] = mem->color[2] = word;
        mem->color[3] = hidden_read(rdp, index >> 1);
        break;
    }
    case N64_RDP_FB_RGBA5551: {
        uint32_t word;
        index = (st->fb_addr >> 1) + pixel;
        word = n64_rdp_rdram_read16(rdp, index << 1);
        mem->color[0] = (uint8_t)((word >> 8) & 0xf8u);
        mem->color[1] = (uint8_t)((word >> 3) & 0xf8u);
        mem->color[2] = (uint8_t)((word << 2) & 0xf8u);
        mem->color[3] = (uint8_t)((hidden_read(rdp, index) << 5) | ((word & 1u) << 7));
        break;
    }
    case N64_RDP_FB_IA88: {
        uint32_t word;
        index = (st->fb_addr >> 1) + pixel;
        word = n64_rdp_rdram_read16(rdp, index << 1);
        mem->color[0] = mem->color[1] = mem->color[2] = (uint8_t)(word >> 8);
        mem->color[3] = (uint8_t)(word & 0xffu);
        break;
    }
    default: {
        uint32_t word;
        index = (st->fb_addr >> 2) + pixel;
        word = n64_rdp_rdram_read32(rdp, index << 2);
        mem->color[0] = (uint8_t)(word >> 24);
        mem->color[1] = (uint8_t)(word >> 16);
        mem->color[2] = (uint8_t)(word >> 8);
        mem->color[3] = (uint8_t)word;
        break;
    }
    }
    mem->color_index = index;

    mem->depth_index = (st->depth_addr >> 1) + pixel;
    {
        uint32_t word = n64_rdp_rdram_read16(rdp, mem->depth_index << 1);
        mem->depth = (uint16_t)(word >> 2);
        mem->dz = (uint8_t)(hidden_read(rdp, mem->depth_index) | ((word & 3u) << 2));
    }
}

/* finish_tile: store_vram_color and store_vram_depth.
 *
 * An 8-bit image writes the red channel to even pixels and the green one
 * to odd pixels. parallel-rdp keeps the unwritten channels in registers,
 * so a later primitive in the same batch that blends with an odd 8-bit
 * pixel reads red where this port reads back the stored green. That case
 * (blending into an 8-bit colour image) has no defined hardware use. */
RDP_INLINE void n64_rdp_pixel_store(N64Rdp *rdp, const RdpPixelMemory *mem)
{
    const N64RdpState *st = rdp->state;
    uint32_t index = mem->color_index;

    if (mem->color_dirty) {
        switch (st->fb_fmt) {
        case N64_RDP_FB_I4:
            rdram_write8(rdp, index, 0);
            if (index & 1u)
                hidden_write(rdp, index >> 1, mem->color[3]);
            break;
        case N64_RDP_FB_I8: {
            uint8_t col = (index & 1u) ? mem->color[1] : mem->color[0];
            rdram_write8(rdp, index, col);
            if (index & 1u)
                hidden_write(rdp, index >> 1, (uint8_t)((col & 1u) * 3u));
            break;
        }
        case N64_RDP_FB_RGBA5551: {
            uint32_t cov = (uint32_t)mem->color[3] >> 5;
            uint32_t word = ((uint32_t)(mem->color[0] & 0xf8u) << 8) | ((uint32_t)(mem->color[1] & 0xf8u) << 3) |
                            ((uint32_t)(mem->color[2] & 0xf8u) >> 2) | (cov >> 2);
            rdram_write8(rdp, index << 1, (uint8_t)(word >> 8));
            rdram_write8(rdp, (index << 1) + 1, (uint8_t)word);
            hidden_write(rdp, index, (uint8_t)(cov & 3u));
            break;
        }
        case N64_RDP_FB_IA88:
            rdram_write8(rdp, index << 1, mem->color[0]);
            rdram_write8(rdp, (index << 1) + 1, mem->color[3]);
            hidden_write(rdp, index, (uint8_t)((mem->color[3] & 1u) * 3u));
            break;
        default:
            rdram_write8(rdp, index << 2, mem->color[0]);
            rdram_write8(rdp, (index << 2) + 1, mem->color[1]);
            rdram_write8(rdp, (index << 2) + 2, mem->color[2]);
            rdram_write8(rdp, (index << 2) + 3, mem->color[3]);
            hidden_write(rdp, 2u * index, (uint8_t)((mem->color[1] & 1u) * 3u));
            hidden_write(rdp, 2u * index + 1u, (uint8_t)((mem->color[3] & 1u) * 3u));
            break;
        }
    }

    if (!color_depth_alias(st) && mem->depth_dirty) {
        uint32_t word = ((uint32_t)mem->depth << 2) | ((uint32_t)mem->dz >> 2);
        rdram_write8(rdp, mem->depth_index << 1, (uint8_t)(word >> 8));
        rdram_write8(rdp, (mem->depth_index << 1) + 1, (uint8_t)word);
        hidden_write(rdp, mem->depth_index, (uint8_t)(mem->dz & 3u));
    }
}

RDP_FORCE_INLINE void write_color(const N64RdpState *st, RdpPixelMemory *mem, uint32_t r, uint32_t g, uint32_t b,
                                  uint32_t a)
{
    mem->color[0] = (uint8_t)r;
    mem->color[1] = (uint8_t)g;
    mem->color[2] = (uint8_t)b;
    if (st->fb_fmt != N64_RDP_FB_I4)
        mem->color[3] = (uint8_t)a;
    mem->color_dirty = 1;
}

/* The fill pipe: the fill colour register holds two 16-bit pixels, four
 * 8-bit ones or one 32-bit one, chosen by the pixel's address. A 4-bit
 * colour image is not written. */
RDP_INLINE void n64_rdp_fill_color(const N64Rdp *rdp, RdpPixelMemory *mem, uint32_t col)
{
    const N64RdpState *st = rdp->state;
    switch (st->fb_fmt) {
    case N64_RDP_FB_RGBA8888:
        write_color(st, mem, (col >> 24) & 0xffu, (col >> 16) & 0xffu, (col >> 8) & 0xffu, col & 0xffu);
        break;
    case N64_RDP_FB_RGBA5551:
        col >>= ((mem->color_index & 1u) ^ 1u) * 16u;
        write_color(st, mem, (col >> 8) & 0xf8u, (col >> 3) & 0xf8u, (col << 2) & 0xf8u, (col & 1u) * 0xe0u);
        break;
    case N64_RDP_FB_IA88:
        col >>= ((mem->color_index & 1u) ^ 1u) * 16u;
        col &= 0xffffu;
        write_color(st, mem, (col >> 8) & 0xffu, (col >> 8) & 0xffu, (col >> 8) & 0xffu, col & 0xffu);
        break;
    case N64_RDP_FB_I8:
        col >>= ((mem->color_index & 3u) ^ 3u) * 8u;
        col &= 0xffu;
        write_color(st, mem, col, col, col, col);
        break;
    default: return;
    }
    if (color_depth_alias(st))
        alias_color_to_depth(st, mem);
}

/* The copy pipe writes the texel as it is. parallel-rdp writes 4-bit,
 * 8-bit and RGBA5551 images only: a copy into IA88 or 32-bit images
 * leaves memory alone. */
RDP_INLINE void n64_rdp_copy_pipeline(const N64Rdp *rdp, RdpPixelMemory *mem, uint32_t word)
{
    const N64RdpState *st = rdp->state;
    switch (st->fb_fmt) {
    case N64_RDP_FB_I4:
        mem->color[0] = mem->color[1] = mem->color[2] = mem->color[3] = 0;
        mem->color_dirty = 1;
        break;
    case N64_RDP_FB_I8:
        word &= 0xffu;
        write_color(st, mem, word, word, word, word);
        break;
    case N64_RDP_FB_RGBA5551:
        write_color(st, mem, (word >> 8) & 0xf8u, (word >> 3) & 0xf8u, (word << 2) & 0xf8u, (word & 1u) * 0xe0u);
        break;
    default: break;
    }
    if (color_depth_alias(st))
        alias_color_to_depth(st, mem);
}

/* z_encode.h: depth is stored as a 14-bit float (3-bit exponent counting
 * leading ones, 11-bit mantissa) of an 18-bit unsigned value. */
RDP_FORCE_INLINE int32_t z_decompress(uint32_t z_)
{
    int32_t z = (int32_t)z_;
    int32_t exponent = z >> 11;
    int32_t mantissa = z & 0x7ff;
    int32_t shift = rdp_max(6 - exponent, 0);
    int32_t base = 0x40000 - (0x40000 >> exponent);
    return (mantissa << shift) + base;
}

RDP_FORCE_INLINE uint16_t z_compress(int32_t z)
{
    int32_t inv_z = rdp_max(0x3ffff - z, 1);
    int32_t exponent = rdp_clamp(17 - rdp_find_msb((uint32_t)inv_z), 0, 7);
    int32_t shift = rdp_max(6 - exponent, 0);
    int32_t mantissa = (z >> shift) & 0x7ff;
    return (uint16_t)((exponent << 11) + mantissa);
}

RDP_FORCE_INLINE int32_t dz_decompress(int32_t dz) { return 1 << dz; }

RDP_FORCE_INLINE int32_t dz_compress(int32_t dz) { return rdp_max(rdp_find_msb((uint32_t)dz), 0); }

RDP_FORCE_INLINE int32_t combine_dz(int32_t dz)
{
    /* The largest power of two not above dz. */
    if (dz != 0)
        dz = 1 << rdp_find_msb((uint32_t)dz);
    return dz;
}

enum { Z_MODE_OPAQUE = 0, Z_MODE_INTERPENETRATING = 1, Z_MODE_TRANSPARENT = 2, Z_MODE_DECAL = 3 };

RDP_FORCE_INLINE int depth_test(int32_t z, int32_t dz, int32_t dz_compressed, uint32_t current_depth,
                                int32_t current_dz, int32_t *coverage_count, int32_t current_coverage_count,
                                int z_compare, int z_mode, int force_blend, int aa_enable, int *blend_en,
                                int *coverage_wrap, int32_t blend_shift[2])
{
    int depth_pass;

    if (z_compare) {
        int32_t memory_z = z_decompress(current_depth);
        int32_t memory_dz = dz_decompress(current_dz);
        int32_t precision_factor = (int32_t)(current_depth >> 11) & 0xf;
        int coplanar = 0;
        int32_t combined_dz, combined_dz_interpenetrate, z_closest_possible;
        int farther, overflow, max_z, front, nearer;

        blend_shift[0] = rdp_clamp(dz_compressed - current_dz, 0, 4);
        blend_shift[1] = rdp_clamp(current_dz - dz_compressed, 0, 4);

        if (precision_factor < 3) {
            if (memory_dz != 0x8000) {
                memory_dz = rdp_max(memory_dz << 1, 16 >> precision_factor);
            } else {
                coplanar = 1;
                memory_dz = 0xffff;
            }
        }

        combined_dz = combine_dz(dz | memory_dz);
        combined_dz_interpenetrate = combined_dz;
        combined_dz <<= 3;

        farther = coplanar || ((z + combined_dz) >= memory_z);
        overflow = (*coverage_count + current_coverage_count) >= 8;

        *blend_en = force_blend || (!overflow && aa_enable && farther);
        *coverage_wrap = overflow;

        depth_pass = 0;
        max_z = memory_z == 0x3ffff;
        front = z < memory_z;
        z_closest_possible = z - combined_dz;
        nearer = coplanar || (z_closest_possible <= memory_z);

        switch (z_mode) {
        case Z_MODE_OPAQUE:
            /* Less-than, except that a surface close enough to the stored
             * one only adds coverage until the coverage overflows. */
            depth_pass = max_z || (overflow ? front : nearer);
            break;
        case Z_MODE_INTERPENETRATING:
            if (!front || !farther || !overflow) {
                depth_pass = max_z || (overflow ? front : nearer);
            } else {
                /* Scale coverage by how far in front of the stored surface
                 * this one is. */
                int32_t cvg_coeff;
                combined_dz_interpenetrate = dz_compress(combined_dz_interpenetrate & 0xffff);
                cvg_coeff = ((memory_z >> combined_dz_interpenetrate) - (z >> combined_dz_interpenetrate)) & 0xf;
                *coverage_count = rdp_min((cvg_coeff * *coverage_count) >> 3, 8);
                depth_pass = 1;
            }
            break;
        case Z_MODE_TRANSPARENT:
            depth_pass = front || max_z;
            break;
        default:
            /* Decal: pass when |z - memory_z| <= max(dz, memory_dz). */
            depth_pass = farther && nearer && !max_z;
            break;
        }
    } else {
        int overflow;
        blend_shift[0] = 0;
        blend_shift[1] = rdp_min(0xf - dz_compressed, 4);
        overflow = (*coverage_count + current_coverage_count) >= 8;
        *blend_en = force_blend || (!overflow && aa_enable);
        *coverage_wrap = overflow;
        depth_pass = 1;
    }
    return depth_pass;
}

enum { COVERAGE_CLAMP = 0, COVERAGE_WRAP = 1, COVERAGE_ZAP = 2, COVERAGE_SAVE = 3 };

RDP_FORCE_INLINE int32_t blend_coverage(int32_t coverage, int32_t memory_coverage, int blend_en, int mode)
{
    switch (mode) {
    case COVERAGE_CLAMP:
        /* Memory coverage reads as 7 unless image read is on. */
        return blend_en ? rdp_min(7, memory_coverage + coverage) : (coverage - 1) & 7;
    case COVERAGE_WRAP: return (coverage + memory_coverage) & 7;
    case COVERAGE_ZAP: return 7;
    default: return memory_coverage;
    }
}

RDP_FORCE_INLINE void n64_rdp_depth_blend(const N64Rdp *rdp, const RdpPrimitive *prim, RdpPixelMemory *mem,
                                          const RdpShaded *shaded)
{
    const N64RdpState *st = rdp->state;
    const RdpDerived *derived = &prim->derived;
    int32_t z = shaded->z_dith >> 9;
    int32_t dith = shaded->z_dith & 0x1ff;
    int32_t coverage_count = shaded->coverage_count;
    uint32_t flags = prim->depth_blend_flags;
    int force_blend = (flags & RDP_DB_FORCE_BLEND) != 0;
    int z_compare = (flags & RDP_DB_DEPTH_TEST) != 0;
    int z_update = (flags & RDP_DB_DEPTH_UPDATE) != 0;
    int image_read_enable = (flags & RDP_DB_IMAGE_READ) != 0;
    int color_on_coverage = (flags & RDP_DB_COLOR_ON_COVERAGE) != 0;
    int blend_multicycle = (flags & RDP_DB_MULTI_CYCLE) != 0;
    int aa_enable = (flags & RDP_DB_AA) != 0;
    int dither_en = (flags & RDP_DB_DITHER) != 0;
    int blend_en = 0, coverage_wrap = 0;
    int32_t blend_shift[2] = {0, 0};
    uint8_t memory_color[4];
    int32_t memory_coverage;
    int z_pass;

    /* decode_memory_color */
    memory_color[3] = image_read_enable ? (uint8_t)(mem->color[3] & 0xe0u) : 0xe0;
    switch (st->fb_fmt) {
    case N64_RDP_FB_I4:
        memory_color[0] = memory_color[1] = memory_color[2] = 0;
        memory_color[3] = 0xe0;
        break;
    case N64_RDP_FB_I8:
        memory_color[0] = memory_color[1] = memory_color[2] = mem->color[0];
        memory_color[3] = 0xe0;
        break;
    case N64_RDP_FB_RGBA5551:
        memory_color[0] = (uint8_t)(mem->color[0] & 0xf8u);
        memory_color[1] = (uint8_t)(mem->color[1] & 0xf8u);
        memory_color[2] = (uint8_t)(mem->color[2] & 0xf8u);
        break;
    case N64_RDP_FB_IA88:
        memory_color[0] = memory_color[1] = memory_color[2] = mem->color[0];
        break;
    default:
        memory_color[0] = mem->color[0];
        memory_color[1] = mem->color[1];
        memory_color[2] = mem->color[2];
        break;
    }
    memory_coverage = memory_color[3] >> 5;

    z_pass = depth_test(z, derived->dz, derived->dz_compressed, mem->depth, mem->dz, &coverage_count, memory_coverage,
                        z_compare, prim->z_mode, force_blend, aa_enable, &blend_en, &coverage_wrap, blend_shift);

    if (z_pass && (!aa_enable || coverage_count != 0)) {
        RdpBlendInputs in;
        const uint8_t *modes = prim->blend_modes[0];
        uint8_t rgb[3];
        int32_t new_coverage;
        int c;

        for (c = 0; c < 4; c++) {
            in.pixel_color[c] = shaded->combined[c];
            in.memory_color[c] = memory_color[c];
            in.fog_color[c] = derived->fog_color[c];
            in.blend_color[c] = derived->blend_color[c];
        }
        in.shade_alpha = (uint8_t)shaded->shade_alpha;

        if (blend_multicycle) {
            /* The first cycle's result replaces the pixel colour, keeping
             * the combined alpha. */
            n64_rdp_blender(&in, modes, force_blend, blend_en, color_on_coverage, coverage_wrap, blend_shift, 0, rgb);
            in.pixel_color[0] = rgb[0];
            in.pixel_color[1] = rgb[1];
            in.pixel_color[2] = rgb[2];
            modes = prim->blend_modes[1];
        }
        n64_rdp_blender(&in, modes, force_blend, blend_en, color_on_coverage, coverage_wrap, blend_shift, 1, rgb);

        if (dither_en)
            n64_rdp_rgb_dither(rgb, dith);

        /* coverage_count may have been scaled by the interpenetrating
         * depth mode. */
        new_coverage = blend_coverage(coverage_count, memory_coverage, blend_en, prim->coverage_mode);
        write_color(st, mem, rgb[0], rgb[1], rgb[2], (uint32_t)new_coverage << 5);

        if (z_update) {
            mem->depth = z_compress(z);
            mem->dz = derived->dz_compressed;
            mem->depth_dirty = 1;
            if (color_depth_alias(st))
                alias_depth_to_color(st, mem);
        } else if (color_depth_alias(st)) {
            alias_color_to_depth(st, mem);
        }
    }
}

#endif /* N64_RDP_MEMORY_H */
