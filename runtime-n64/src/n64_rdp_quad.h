/*
 * Four 32-bit integers handled together: a colour's channels, or the four
 * attributes a scanline steps (n64_rdp_raster.c, shade_row).
 *
 * With SSE2 a quad is one register; without it, four integers in a struct.
 * Both give the same lanes for the inputs the callers pass, which each
 * function states where it matters. Keeping a pixel's colours in registers
 * is most of the point: a compiler that vectorises one of two neighbouring
 * loops on its own makes the processor wait for the store between them.
 */
#ifndef N64_RDP_QUAD_H
#define N64_RDP_QUAD_H

#include "n64_rdp_internal.h"

/* N64_RDP_NO_SIMD builds the plain version on any host (the unit test is
 * the same either way). */
#if !defined(N64_RDP_NO_SIMD) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>

typedef __m128i RdpQuad;

RDP_FORCE_INLINE RdpQuad quad_zero(void) { return _mm_setzero_si128(); }
RDP_FORCE_INLINE RdpQuad quad_load(const int32_t *p) { return _mm_loadu_si128((const __m128i *)p); }
RDP_FORCE_INLINE void quad_store(int32_t *p, RdpQuad q) { _mm_storeu_si128((__m128i *)p, q); }
RDP_FORCE_INLINE RdpQuad quad_splat(int32_t x) { return _mm_set1_epi32(x); }
RDP_FORCE_INLINE RdpQuad quad_add(RdpQuad a, RdpQuad b) { return _mm_add_epi32(a, b); }
RDP_FORCE_INLINE RdpQuad quad_sub(RdpQuad a, RdpQuad b) { return _mm_sub_epi32(a, b); }
RDP_FORCE_INLINE RdpQuad quad_asr(RdpQuad a, int n) { return _mm_srai_epi32(a, n); }
RDP_FORCE_INLINE RdpQuad quad_shl(RdpQuad a, int n) { return _mm_slli_epi32(a, n); }

/* a * b, for lanes of both that fit 16 signed bits. */
RDP_FORCE_INLINE RdpQuad quad_mul16(RdpQuad a, RdpQuad b)
{
    const __m128i low = _mm_set1_epi32(0xffff);
    return _mm_madd_epi16(_mm_and_si128(a, low), _mm_and_si128(b, low));
}

/* Each lane clamped to 0..255, for lanes that fit 16 signed bits. */
RDP_FORCE_INLINE RdpQuad quad_clamp_u8(RdpQuad q)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i bytes = _mm_packus_epi16(_mm_packs_epi32(q, q), zero);
    return _mm_unpacklo_epi16(_mm_unpacklo_epi8(bytes, zero), zero);
}

/* Lanes of 0..255 as four bytes, the first lane lowest, and back. */
RDP_FORCE_INLINE uint32_t quad_to_bytes(RdpQuad q)
{
    const __m128i zero = _mm_setzero_si128();
    return (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(q, q), zero));
}
RDP_FORCE_INLINE RdpQuad quad_from_bytes(uint32_t bytes)
{
    const __m128i zero = _mm_setzero_si128();
    return _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)bytes), zero), zero);
}

RDP_FORCE_INLINE int32_t quad_last(RdpQuad q) { return _mm_cvtsi128_si32(_mm_shuffle_epi32(q, 0xff)); }
/* The last lane in all four. */
RDP_FORCE_INLINE RdpQuad quad_splat_last(RdpQuad q) { return _mm_shuffle_epi32(q, 0xff); }
/* The last lane replaced. */
RDP_FORCE_INLINE RdpQuad quad_with_last(RdpQuad q, int32_t last)
{
    const __m128i keep = _mm_set_epi32(0, -1, -1, -1);
    return _mm_or_si128(_mm_and_si128(q, keep), _mm_slli_si128(_mm_cvtsi32_si128(last), 12));
}

#else

typedef struct RdpQuad {
    int32_t c[4];
} RdpQuad;

RDP_FORCE_INLINE RdpQuad quad_splat(int32_t x)
{
    RdpQuad q;
    q.c[0] = q.c[1] = q.c[2] = q.c[3] = x;
    return q;
}
RDP_FORCE_INLINE RdpQuad quad_zero(void) { return quad_splat(0); }
RDP_FORCE_INLINE RdpQuad quad_load(const int32_t *p)
{
    RdpQuad q;
    q.c[0] = p[0];
    q.c[1] = p[1];
    q.c[2] = p[2];
    q.c[3] = p[3];
    return q;
}
RDP_FORCE_INLINE void quad_store(int32_t *p, RdpQuad q)
{
    p[0] = q.c[0];
    p[1] = q.c[1];
    p[2] = q.c[2];
    p[3] = q.c[3];
}
#define RDP_QUAD_EACH(expr)                                                                                    \
    do {                                                                                                       \
        int i_;                                                                                                \
        for (i_ = 0; i_ < 4; i_++)                                                                             \
            r.c[i_] = (expr);                                                                                  \
    } while (0)
RDP_FORCE_INLINE RdpQuad quad_add(RdpQuad a, RdpQuad b)
{
    RdpQuad r;
    RDP_QUAD_EACH(rdp_add(a.c[i_], b.c[i_]));
    return r;
}
RDP_FORCE_INLINE RdpQuad quad_sub(RdpQuad a, RdpQuad b)
{
    RdpQuad r;
    RDP_QUAD_EACH(rdp_sub(a.c[i_], b.c[i_]));
    return r;
}
RDP_FORCE_INLINE RdpQuad quad_asr(RdpQuad a, int n)
{
    RdpQuad r;
    RDP_QUAD_EACH(rdp_asr(a.c[i_], n));
    return r;
}
RDP_FORCE_INLINE RdpQuad quad_shl(RdpQuad a, int n)
{
    RdpQuad r;
    RDP_QUAD_EACH(rdp_shl(a.c[i_], n));
    return r;
}
RDP_FORCE_INLINE RdpQuad quad_mul16(RdpQuad a, RdpQuad b)
{
    RdpQuad r;
    RDP_QUAD_EACH(rdp_mul(a.c[i_], b.c[i_]));
    return r;
}
RDP_FORCE_INLINE RdpQuad quad_clamp_u8(RdpQuad q)
{
    RdpQuad r;
    RDP_QUAD_EACH(rdp_clamp(q.c[i_], 0, 0xff));
    return r;
}
#undef RDP_QUAD_EACH
RDP_FORCE_INLINE uint32_t quad_to_bytes(RdpQuad q)
{
    return (uint32_t)q.c[0] | ((uint32_t)q.c[1] << 8) | ((uint32_t)q.c[2] << 16) | ((uint32_t)q.c[3] << 24);
}
RDP_FORCE_INLINE RdpQuad quad_from_bytes(uint32_t bytes)
{
    RdpQuad q;
    q.c[0] = (int32_t)(bytes & 0xffu);
    q.c[1] = (int32_t)((bytes >> 8) & 0xffu);
    q.c[2] = (int32_t)((bytes >> 16) & 0xffu);
    q.c[3] = (int32_t)(bytes >> 24);
    return q;
}
RDP_FORCE_INLINE int32_t quad_last(RdpQuad q) { return q.c[3]; }
RDP_FORCE_INLINE RdpQuad quad_splat_last(RdpQuad q) { return quad_splat(q.c[3]); }
RDP_FORCE_INLINE RdpQuad quad_with_last(RdpQuad q, int32_t last)
{
    q.c[3] = last;
    return q;
}

#endif

#endif /* N64_RDP_QUAD_H */
