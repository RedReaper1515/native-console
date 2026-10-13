/*
 * The handlers the RSP interpreter runs for the vector unit's loads, stores
 * and computational instructions (n64_rsp_op_*). n64_rsp_impl.h holds the
 * reference for each (n64_rsp_ref_*) and includes this file; nothing else
 * should.
 *
 *  - On x86 the computational instructions work on all eight lanes at once
 *    with SSE2, which every x86-64 processor has. Nothing later is used:
 *    generated game projects compile the runtime with plain release flags.
 *  - On a little-endian host the loads and stores that move whole lanes
 *    within DMEM copy them as words. Any other element, alignment or wrap
 *    goes to the reference.
 *  - Everything else, and every host without those, runs the reference.
 *
 * The state layout is the reference's: a register is eight host words with
 * lane 0 first, so a register is loaded into an SSE register with lane n
 * in word n and the lane-wise operations need no reordering; the
 * accumulator is its three slices; the flag registers stay bit masks and
 * are widened to lane masks where an instruction uses them.
 *
 * The SSE2 forms of the multiplies, VADD, VSUB, VABS, VADDC and the clip
 * tests are ported from ares (https://github.com/ares-emulator/ares)
 * ares/n64/rsp/interpreter-vpu.cpp, with its SSE4.1 blends written as
 * and/andnot/or. Copyright (c) 2004-2025 ares team, Near et al. Permission
 * to use, copy, modify, and/or distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 */
#ifndef N64_RSP_FAST_H
#define N64_RSP_FAST_H

#ifndef N64_RSP_IMPL_H
#error "include n64_rsp_impl.h, which includes this file"
#endif

#if !defined(N64_RSP_REFERENCE_ONLY)
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define N64_RSP_SSE2 1
#endif
#if defined(N64_RSP_HOST_LE)
#define N64_RSP_FAST_MEM 1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define N64_RSP_USE_REF(name)                                                                                       \
    N64_FORCEINLINE void n64_rsp_op_##name(N64Rsp *rsp, uint32_t op) { n64_rsp_ref_##name(rsp, op); }

/* Loads and stores. */

#ifdef N64_RSP_FAST_MEM

/* Copies 2, 4, 8 or 16 bytes between whole lanes and DMEM. Lanes are host
 * words and DMEM holds each lane high byte first, so the two bytes of
 * every lane change places; the same function serves both directions. */
N64_FORCEINLINE void n64_rsp_copy_lanes(void *to, const void *from, unsigned bytes)
{
    if (bytes == 2u) {
        uint16_t x;
        memcpy(&x, from, 2);
        x = N64_RSP_BSWAP16(x);
        memcpy(to, &x, 2);
    } else if (bytes == 4u) {
        uint32_t x;
        memcpy(&x, from, 4);
        x = ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu);
        memcpy(to, &x, 4);
    } else {
        unsigned i;
        for (i = 0; i < bytes; i += 8u) {
            uint64_t x;
            memcpy(&x, (const uint8_t *)from + i, 8);
            x = ((x & 0x00FF00FF00FF00FFull) << 8) | ((x >> 8) & 0x00FF00FF00FF00FFull);
            memcpy((uint8_t *)to + i, &x, 8);
        }
    }
}

/* LSV, LLV, LDV and SSV, SLV, SDV at an even element, when the bytes fit in
 * the register and do not run off the end of DMEM. */
N64_FORCEINLINE int n64_rsp_vseq_whole(const N64Rsp *rsp, uint32_t op, unsigned size, uint32_t *a)
{
    unsigned e = N64_RSP_LE(op);
    *a = (rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * (int32_t)size)) & 0xFFFu;
    return !(e & 1u) && e + size <= 16u && *a + size <= 0x1000u;
}

N64_FORCEINLINE void n64_rsp_vload_seq_fast(N64Rsp *rsp, uint32_t op, unsigned size)
{
    uint32_t a;
    if (n64_rsp_vseq_whole(rsp, op, size, &a))
        n64_rsp_copy_lanes(&rsp->vu.v[N64_RSP_VT(op)][N64_RSP_LE(op) >> 1], rsp->dmem + a, size);
    else
        n64_rsp_vload_seq(rsp, op, size);
}

N64_FORCEINLINE void n64_rsp_vstore_seq_fast(N64Rsp *rsp, uint32_t op, unsigned size)
{
    uint32_t a;
    if (n64_rsp_vseq_whole(rsp, op, size, &a))
        n64_rsp_copy_lanes(rsp->dmem + a, &rsp->vu.v[N64_RSP_VT(op)][N64_RSP_LE(op) >> 1], size);
    else
        n64_rsp_vstore_seq(rsp, op, size);
}

N64_RSP_USE_REF(lbv)
N64_FORCEINLINE void n64_rsp_op_lsv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_seq_fast(rsp, op, 2); }
N64_FORCEINLINE void n64_rsp_op_llv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_seq_fast(rsp, op, 4); }
N64_FORCEINLINE void n64_rsp_op_ldv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_seq_fast(rsp, op, 8); }

N64_RSP_USE_REF(sbv)
N64_FORCEINLINE void n64_rsp_op_ssv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_seq_fast(rsp, op, 2); }
N64_FORCEINLINE void n64_rsp_op_slv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_seq_fast(rsp, op, 4); }
N64_FORCEINLINE void n64_rsp_op_sdv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_seq_fast(rsp, op, 8); }

/* LQV and SQV of a whole register: element 0 at the start of a line. */
N64_FORCEINLINE void n64_rsp_op_lqv(N64Rsp *rsp, uint32_t op)
{
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    if (N64_RSP_LE(op) == 0 && !(a & 15u))
        n64_rsp_copy_lanes(rsp->vu.v[N64_RSP_VT(op)], rsp->dmem + (a & 0xFF0u), 16);
    else
        n64_rsp_ref_lqv(rsp, op);
}

N64_FORCEINLINE void n64_rsp_op_sqv(N64Rsp *rsp, uint32_t op)
{
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 16);
    if (N64_RSP_LE(op) == 0 && !(a & 15u))
        n64_rsp_copy_lanes(rsp->dmem + (a & 0xFF0u), rsp->vu.v[N64_RSP_VT(op)], 16);
    else
        n64_rsp_ref_sqv(rsp, op);
}

/* LPV and LUV with no rotation: the eight bytes at the 8-byte line. */
N64_FORCEINLINE void n64_rsp_vload_packed_fast(N64Rsp *rsp, uint32_t op, unsigned shift)
{
    uint32_t a = rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 8);
    if ((a & 7u) == N64_RSP_LE(op)) {
        const uint8_t *p = rsp->dmem + (a & 0xFF8u);
        uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
        unsigned n;
        for (n = 0; n < 8; n++) v[n] = (uint16_t)((uint32_t)p[n] << shift);
    } else {
        n64_rsp_vload_packed(rsp, op, 8, 1, shift);
    }
}

N64_FORCEINLINE void n64_rsp_op_lpv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_packed_fast(rsp, op, 8); }
N64_FORCEINLINE void n64_rsp_op_luv(N64Rsp *rsp, uint32_t op) { n64_rsp_vload_packed_fast(rsp, op, 7); }

/* SPV and SUV from element 0, inside DMEM: one byte from each lane. */
N64_FORCEINLINE void n64_rsp_vstore_packed_fast(N64Rsp *rsp, uint32_t op, int unsigned_first)
{
    uint32_t a = (rsp->r[N64_RSP_RS(op)] + (uint32_t)(n64_rsp_voff(op) * 8)) & 0xFFFu;
    if (N64_RSP_LE(op) == 0 && a <= 0xFF8u) {
        const uint16_t *v = rsp->vu.v[N64_RSP_VT(op)];
        uint8_t *p = rsp->dmem + a;
        unsigned n;
        for (n = 0; n < 8; n++) p[n] = (uint8_t)(v[n] >> (unsigned_first ? 7 : 8));
    } else {
        n64_rsp_vstore_packed(rsp, op, unsigned_first);
    }
}

N64_FORCEINLINE void n64_rsp_op_spv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_packed_fast(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_op_suv(N64Rsp *rsp, uint32_t op) { n64_rsp_vstore_packed_fast(rsp, op, 1); }

#else /* !N64_RSP_FAST_MEM */

N64_RSP_USE_REF(lbv)
N64_RSP_USE_REF(lsv)
N64_RSP_USE_REF(llv)
N64_RSP_USE_REF(ldv)
N64_RSP_USE_REF(lqv)
N64_RSP_USE_REF(lpv)
N64_RSP_USE_REF(luv)
N64_RSP_USE_REF(sbv)
N64_RSP_USE_REF(ssv)
N64_RSP_USE_REF(slv)
N64_RSP_USE_REF(sdv)
N64_RSP_USE_REF(sqv)
N64_RSP_USE_REF(spv)
N64_RSP_USE_REF(suv)

#endif /* N64_RSP_FAST_MEM */

N64_RSP_USE_REF(lrv)
N64_RSP_USE_REF(lhv)
N64_RSP_USE_REF(lfv)
N64_RSP_USE_REF(lwv)
N64_RSP_USE_REF(ltv)
N64_RSP_USE_REF(srv)
N64_RSP_USE_REF(shv)
N64_RSP_USE_REF(sfv)
N64_RSP_USE_REF(swv)
N64_RSP_USE_REF(stv)

/* Computational instructions. */

/* The MPEG multiplies and rounds are not worth a second form. */
N64_RSP_USE_REF(vrndp)
N64_RSP_USE_REF(vrndn)
N64_RSP_USE_REF(vmulq)
N64_RSP_USE_REF(vmacq)
N64_RSP_USE_REF(vnop)

#ifdef N64_RSP_SSE2

N64_FORCEINLINE __m128i n64_rsp_vld(const uint16_t *p) { return _mm_loadu_si128((const __m128i *)p); }
N64_FORCEINLINE void n64_rsp_vst(uint16_t *p, __m128i x) { _mm_storeu_si128((__m128i *)p, x); }

/* a where the lane mask is set, b elsewhere. */
N64_FORCEINLINE __m128i n64_rsp_vsel(__m128i mask, __m128i a, __m128i b)
{
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}

/* A flag register's bits as lane masks (all ones or zero), and back. */
N64_FORCEINLINE __m128i n64_rsp_vmask(unsigned bits)
{
    const __m128i bit = _mm_set_epi16(0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01);
    return _mm_cmpeq_epi16(_mm_and_si128(_mm_set1_epi16((short)bits), bit), bit);
}

N64_FORCEINLINE uint16_t n64_rsp_vbits(__m128i mask)
{
    return (uint16_t)(_mm_movemask_epi8(_mm_packs_epi16(mask, _mm_setzero_si128())) & 0xFF);
}

N64_FORCEINLINE __m128i n64_rsp_vs(const N64Rsp *rsp, uint32_t op) { return n64_rsp_vld(rsp->vu.v[N64_RSP_VS(op)]); }

/* vt with the element selection applied (n64_rsp_elem_lane): the whole
 * register, one lane broadcast, or one lane of each pair or quarter. */
N64_FORCEINLINE __m128i n64_rsp_vte(const N64Rsp *rsp, uint32_t op)
{
    const uint16_t *t = rsp->vu.v[N64_RSP_VT(op)];
    unsigned e = N64_RSP_VE(op);
    __m128i x;
    if (e < 2u) return n64_rsp_vld(t);
    if (e >= 8u) return _mm_set1_epi16((short)t[e & 7u]);
    x = n64_rsp_vld(t);
    if (e < 4u) {
        x = _mm_srl_epi32(x, _mm_cvtsi32_si128((int)((e & 1u) << 4)));
        x = _mm_shufflelo_epi16(x, _MM_SHUFFLE(2, 2, 0, 0));
        return _mm_shufflehi_epi16(x, _MM_SHUFFLE(2, 2, 0, 0));
    }
    x = _mm_srl_epi64(x, _mm_cvtsi32_si128((int)((e & 3u) << 4)));
    x = _mm_shufflelo_epi16(x, 0);
    return _mm_shufflehi_epi16(x, 0);
}

N64_FORCEINLINE void n64_rsp_vput(N64Rsp *rsp, uint32_t op, __m128i vd) { n64_rsp_vst(rsp->vu.v[N64_RSP_VD(op)], vd); }

/* All ones where the 16-bit add a + b = sum carried out. */
N64_FORCEINLINE __m128i n64_rsp_vcarry(__m128i a, __m128i b, __m128i sum)
{
    return _mm_cmpeq_epi16(_mm_cmpeq_epi16(_mm_adds_epu16(a, b), sum), _mm_setzero_si128());
}

/* The accumulator's high and middle slices as one signed 32-bit value per
 * lane, saturated to 16 bits: what n64_rsp_acc_sat(mid = 1) gives. */
N64_FORCEINLINE __m128i n64_rsp_vsat_mid(__m128i m, __m128i h)
{
    return _mm_packs_epi32(_mm_unpacklo_epi16(m, h), _mm_unpackhi_epi16(m, h));
}

/* n64_rsp_acc_sat(mid = 0, 0x0000, 0xFFFF): the low slice, or all zeroes or
 * all ones by the sign when high:middle is not a 16-bit value. */
N64_FORCEINLINE __m128i n64_rsp_vsat_low(__m128i l, __m128i m, __m128i h)
{
    __m128i fits = _mm_cmpeq_epi16(h, _mm_srai_epi16(m, 15));
    __m128i over = _mm_andnot_si128(_mm_or_si128(fits, _mm_srai_epi16(h, 15)), _mm_cmpeq_epi16(l, l));
    return _mm_or_si128(_mm_and_si128(fits, l), over);
}

/* acc = hi:md:lo. */
N64_FORCEINLINE void n64_rsp_vacc_set(N64RspVu *vu, __m128i lo, __m128i md, __m128i hi)
{
    n64_rsp_vst(vu->acc_l, lo);
    n64_rsp_vst(vu->acc_m, md);
    n64_rsp_vst(vu->acc_h, hi);
}

/* acc += hi:md:lo, with the carries rippling up; gives back the new
 * slices. A carry into the middle term can wrap it to zero, which is a
 * carry into the high slice of its own. */
N64_FORCEINLINE void n64_rsp_vacc_add(N64RspVu *vu, __m128i lo, __m128i md, __m128i hi, __m128i *l, __m128i *m,
                                      __m128i *h)
{
    __m128i al = n64_rsp_vld(vu->acc_l), am = n64_rsp_vld(vu->acc_m), ah = n64_rsp_vld(vu->acc_h);
    __m128i sl = _mm_add_epi16(al, lo);
    __m128i c1 = n64_rsp_vcarry(al, lo, sl);
    __m128i md1 = _mm_sub_epi16(md, c1);
    __m128i c2 = _mm_and_si128(c1, _mm_cmpeq_epi16(md1, _mm_setzero_si128()));
    __m128i sm = _mm_add_epi16(am, md1);
    __m128i c3 = n64_rsp_vcarry(am, md1, sm);
    __m128i sh = _mm_sub_epi16(_mm_sub_epi16(_mm_add_epi16(ah, hi), c2), c3);
    n64_rsp_vacc_set(vu, sl, sm, sh);
    *l = sl;
    *m = sm;
    *h = sh;
}

/* The high word of signed a times unsigned b. */
N64_FORCEINLINE __m128i n64_rsp_vmulhi_su(__m128i a, __m128i b)
{
    return _mm_sub_epi16(_mm_mulhi_epu16(a, b), _mm_and_si128(b, _mm_srai_epi16(a, 15)));
}

/* VMULF/VMULU: the doubled product is high:md:lo; adding 0x8000 flips the
 * low slice's top bit and carries when it was set. */
N64_FORCEINLINE void n64_rsp_vmulf_sse2(N64Rsp *rsp, uint32_t op, int is_unsigned)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i lo = _mm_mullo_epi16(vs, vt), hi = _mm_mulhi_epi16(vs, vt);
    __m128i md = _mm_or_si128(_mm_slli_epi16(hi, 1), _mm_srli_epi16(lo, 15));
    __m128i carry;
    hi = _mm_srai_epi16(hi, 15);
    lo = _mm_slli_epi16(lo, 1);
    carry = _mm_srai_epi16(lo, 15);
    lo = _mm_xor_si128(lo, _mm_set1_epi16(-32768));
    md = _mm_sub_epi16(md, carry);
    hi = _mm_sub_epi16(hi, _mm_and_si128(carry, _mm_cmpeq_epi16(md, zero)));
    n64_rsp_vacc_set(&rsp->vu, lo, md, hi);
    if (!is_unsigned)
        n64_rsp_vput(rsp, op, n64_rsp_vsat_mid(md, hi));
    else
        n64_rsp_vput(rsp, op, _mm_andnot_si128(_mm_srai_epi16(hi, 15), _mm_or_si128(md, _mm_srai_epi16(md, 15))));
}

N64_FORCEINLINE void n64_rsp_op_vmulf(N64Rsp *rsp, uint32_t op) { n64_rsp_vmulf_sse2(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_op_vmulu(N64Rsp *rsp, uint32_t op) { n64_rsp_vmulf_sse2(rsp, op, 1); }

N64_FORCEINLINE void n64_rsp_vmacf_sse2(N64Rsp *rsp, uint32_t op, int is_unsigned)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i lo = _mm_mullo_epi16(vs, vt), hi = _mm_mulhi_epi16(vs, vt);
    __m128i md = _mm_or_si128(_mm_slli_epi16(hi, 1), _mm_srli_epi16(lo, 15));
    __m128i l, m, h;
    n64_rsp_vacc_add(&rsp->vu, _mm_slli_epi16(lo, 1), md, _mm_srai_epi16(hi, 15), &l, &m, &h);
    if (!is_unsigned) {
        n64_rsp_vput(rsp, op, n64_rsp_vsat_mid(m, h));
    } else {
        __m128i over = _mm_or_si128(_mm_srai_epi16(m, 15), _mm_cmpgt_epi16(h, _mm_setzero_si128()));
        n64_rsp_vput(rsp, op, _mm_andnot_si128(_mm_srai_epi16(h, 15), _mm_or_si128(m, over)));
    }
}

N64_FORCEINLINE void n64_rsp_op_vmacf(N64Rsp *rsp, uint32_t op) { n64_rsp_vmacf_sse2(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_op_vmacu(N64Rsp *rsp, uint32_t op) { n64_rsp_vmacf_sse2(rsp, op, 1); }

N64_FORCEINLINE void n64_rsp_op_vmudl(N64Rsp *rsp, uint32_t op)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i lo = _mm_mulhi_epu16(n64_rsp_vs(rsp, op), n64_rsp_vte(rsp, op));
    n64_rsp_vacc_set(&rsp->vu, lo, zero, zero);
    n64_rsp_vput(rsp, op, lo);
}

N64_FORCEINLINE void n64_rsp_op_vmudm(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i md = n64_rsp_vmulhi_su(vs, vt);
    n64_rsp_vacc_set(&rsp->vu, _mm_mullo_epi16(vs, vt), md, _mm_srai_epi16(md, 15));
    n64_rsp_vput(rsp, op, md);
}

N64_FORCEINLINE void n64_rsp_op_vmudn(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i lo = _mm_mullo_epi16(vs, vt), md = n64_rsp_vmulhi_su(vt, vs);
    n64_rsp_vacc_set(&rsp->vu, lo, md, _mm_srai_epi16(md, 15));
    n64_rsp_vput(rsp, op, lo);
}

N64_FORCEINLINE void n64_rsp_op_vmudh(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i md = _mm_mullo_epi16(vs, vt), hi = _mm_mulhi_epi16(vs, vt);
    n64_rsp_vacc_set(&rsp->vu, _mm_setzero_si128(), md, hi);
    n64_rsp_vput(rsp, op, n64_rsp_vsat_mid(md, hi));
}

N64_FORCEINLINE void n64_rsp_op_vmadl(N64Rsp *rsp, uint32_t op)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i l, m, h;
    n64_rsp_vacc_add(&rsp->vu, _mm_mulhi_epu16(n64_rsp_vs(rsp, op), n64_rsp_vte(rsp, op)), zero, zero, &l, &m, &h);
    n64_rsp_vput(rsp, op, n64_rsp_vsat_low(l, m, h));
}

N64_FORCEINLINE void n64_rsp_op_vmadm(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i md = n64_rsp_vmulhi_su(vs, vt);
    __m128i l, m, h;
    n64_rsp_vacc_add(&rsp->vu, _mm_mullo_epi16(vs, vt), md, _mm_srai_epi16(md, 15), &l, &m, &h);
    n64_rsp_vput(rsp, op, n64_rsp_vsat_mid(m, h));
}

N64_FORCEINLINE void n64_rsp_op_vmadn(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i md = n64_rsp_vmulhi_su(vt, vs);
    __m128i l, m, h;
    n64_rsp_vacc_add(&rsp->vu, _mm_mullo_epi16(vs, vt), md, _mm_srai_epi16(md, 15), &l, &m, &h);
    n64_rsp_vput(rsp, op, n64_rsp_vsat_low(l, m, h));
}

N64_FORCEINLINE void n64_rsp_op_vmadh(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i am = n64_rsp_vld(rsp->vu.acc_m), ah = n64_rsp_vld(rsp->vu.acc_h);
    __m128i md = _mm_mullo_epi16(vs, vt);
    __m128i sm = _mm_add_epi16(am, md);
    __m128i sh = _mm_sub_epi16(_mm_add_epi16(ah, _mm_mulhi_epi16(vs, vt)), n64_rsp_vcarry(am, md, sm));
    n64_rsp_vst(rsp->vu.acc_m, sm);
    n64_rsp_vst(rsp->vu.acc_h, sh);
    n64_rsp_vput(rsp, op, n64_rsp_vsat_mid(sm, sh));
}

/* VADD: the carry joins the smaller operand first, where it cannot
 * overflow unless both are 0x7FFF and the sum saturates anyway. */
N64_FORCEINLINE void n64_rsp_op_vadd(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i carry = n64_rsp_vmask(rsp->vu.vco_lo);
    __m128i lo = _mm_subs_epi16(_mm_min_epi16(vs, vt), carry);
    n64_rsp_vst(rsp->vu.acc_l, _mm_sub_epi16(_mm_add_epi16(vs, vt), carry));
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    n64_rsp_vput(rsp, op, _mm_adds_epi16(lo, _mm_max_epi16(vs, vt)));
}

/* VSUB: vt + carry is taken off in one saturating step; where adding the
 * carry to vt overflowed (0x7FFF), one more comes off. */
N64_FORCEINLINE void n64_rsp_op_vsub(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i carry = n64_rsp_vmask(rsp->vu.vco_lo);
    __m128i wrapped = _mm_sub_epi16(vt, carry), clamped = _mm_subs_epi16(vt, carry);
    __m128i vd = _mm_subs_epi16(vs, clamped);
    n64_rsp_vst(rsp->vu.acc_l, _mm_sub_epi16(vs, wrapped));
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    n64_rsp_vput(rsp, op, _mm_adds_epi16(vd, _mm_cmpgt_epi16(clamped, wrapped)));
}

N64_FORCEINLINE void n64_rsp_op_vabs(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i neg = _mm_srai_epi16(vs, 15);
    __m128i vd = _mm_xor_si128(_mm_andnot_si128(_mm_cmpeq_epi16(vs, _mm_setzero_si128()), vt), neg);
    n64_rsp_vst(rsp->vu.acc_l, _mm_sub_epi16(vd, neg));
    n64_rsp_vput(rsp, op, _mm_subs_epi16(vd, neg));
}

N64_FORCEINLINE void n64_rsp_op_vaddc(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i sum = _mm_add_epi16(vs, vt);
    n64_rsp_vst(rsp->vu.acc_l, sum);
    rsp->vu.vco_lo = n64_rsp_vbits(n64_rsp_vcarry(vs, vt, sum));
    rsp->vu.vco_hi = 0;
    n64_rsp_vput(rsp, op, sum);
}

N64_FORCEINLINE void n64_rsp_op_vsubc(N64Rsp *rsp, uint32_t op)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i diff = _mm_sub_epi16(vs, vt);
    __m128i equal = _mm_cmpeq_epi16(vs, vt);
    __m128i below = _mm_andnot_si128(equal, _mm_cmpeq_epi16(_mm_subs_epu16(vs, vt), zero));
    n64_rsp_vst(rsp->vu.acc_l, diff);
    rsp->vu.vco_lo = n64_rsp_vbits(below);
    rsp->vu.vco_hi = n64_rsp_vbits(_mm_cmpeq_epi16(equal, zero));
    n64_rsp_vput(rsp, op, diff);
}

N64_FORCEINLINE void n64_rsp_op_vzero(N64Rsp *rsp, uint32_t op)
{
    n64_rsp_vst(rsp->vu.acc_l, _mm_add_epi16(n64_rsp_vs(rsp, op), n64_rsp_vte(rsp, op)));
    n64_rsp_vput(rsp, op, _mm_setzero_si128());
}

N64_FORCEINLINE void n64_rsp_op_vsar(N64Rsp *rsp, uint32_t op)
{
    __m128i vd;
    switch (N64_RSP_VE(op)) {
    case 8: vd = n64_rsp_vld(rsp->vu.acc_h); break;
    case 9: vd = n64_rsp_vld(rsp->vu.acc_m); break;
    case 10: vd = n64_rsp_vld(rsp->vu.acc_l); break;
    default: vd = _mm_setzero_si128(); break;
    }
    n64_rsp_vput(rsp, op, vd);
}

/* kind: 0 VLT, 1 VEQ, 2 VNE, 3 VGE, as n64_rsp_vcompare. */
N64_FORCEINLINE void n64_rsp_vcompare_sse2(N64Rsp *rsp, uint32_t op, unsigned kind)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i co_lo = n64_rsp_vmask(rsp->vu.vco_lo), co_hi = n64_rsp_vmask(rsp->vu.vco_hi);
    __m128i equal = _mm_cmpeq_epi16(vs, vt);
    __m128i sel, vd;
    switch (kind) {
    case 0: sel = _mm_or_si128(_mm_cmplt_epi16(vs, vt), _mm_and_si128(equal, _mm_and_si128(co_lo, co_hi))); break;
    case 1: sel = _mm_andnot_si128(co_hi, equal); break;
    case 2: sel = _mm_or_si128(_mm_cmpeq_epi16(equal, _mm_setzero_si128()), co_hi); break;
    default: sel = _mm_or_si128(_mm_cmpgt_epi16(vs, vt), _mm_andnot_si128(_mm_and_si128(co_lo, co_hi), equal)); break;
    }
    vd = n64_rsp_vsel(sel, vs, vt);
    n64_rsp_vst(rsp->vu.acc_l, vd);
    rsp->vu.vcc_lo = n64_rsp_vbits(sel);
    rsp->vu.vcc_hi = 0;
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    n64_rsp_vput(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_op_vlt(N64Rsp *rsp, uint32_t op) { n64_rsp_vcompare_sse2(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_op_veq(N64Rsp *rsp, uint32_t op) { n64_rsp_vcompare_sse2(rsp, op, 1); }
N64_FORCEINLINE void n64_rsp_op_vne(N64Rsp *rsp, uint32_t op) { n64_rsp_vcompare_sse2(rsp, op, 2); }
N64_FORCEINLINE void n64_rsp_op_vge(N64Rsp *rsp, uint32_t op) { n64_rsp_vcompare_sse2(rsp, op, 3); }

/* VCH. `differ` marks lanes whose signs differ; there vt is negated, so
 * diff is vs + vt on those lanes and vs - vt on the others, and neither
 * overflows. */
N64_FORCEINLINE void n64_rsp_op_vch(N64Rsp *rsp, uint32_t op)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i differ = _mm_srai_epi16(_mm_xor_si128(vs, vt), 15);
    __m128i nvt = _mm_sub_epi16(_mm_xor_si128(vt, differ), differ);
    __m128i diff = _mm_sub_epi16(vs, nvt);
    __m128i diff0 = _mm_cmpeq_epi16(diff, zero);
    __m128i vt_neg = _mm_srai_epi16(vt, 15);
    __m128i gt = _mm_cmpgt_epi16(diff, zero);
    __m128i le = _mm_cmpeq_epi16(gt, zero), ge = _mm_or_si128(gt, diff0);
    __m128i cc_lo = n64_rsp_vsel(differ, le, vt_neg), cc_hi = n64_rsp_vsel(differ, vt_neg, ge);
    __m128i ce = _mm_and_si128(differ, _mm_cmpeq_epi16(diff, differ));
    __m128i vd = n64_rsp_vsel(n64_rsp_vsel(differ, cc_lo, cc_hi), nvt, vs);
    n64_rsp_vst(rsp->vu.acc_l, vd);
    rsp->vu.vcc_lo = n64_rsp_vbits(cc_lo);
    rsp->vu.vcc_hi = n64_rsp_vbits(cc_hi);
    rsp->vu.vco_lo = n64_rsp_vbits(differ);
    rsp->vu.vco_hi = n64_rsp_vbits(_mm_cmpeq_epi16(_mm_or_si128(diff0, ce), zero));
    rsp->vu.vce = (uint8_t)n64_rsp_vbits(ce);
    n64_rsp_vput(rsp, op, vd);
}

/* VCL. Where VCO low is set the sum's zero and carry decide "less or
 * equal" (either of them when VCE is set, both when it is not); elsewhere
 * vs >= vt unsigned decides "greater or equal". VCO high keeps the old
 * result of whichever test applies. */
N64_FORCEINLINE void n64_rsp_op_vcl(N64Rsp *rsp, uint32_t op)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i co_lo = n64_rsp_vmask(rsp->vu.vco_lo), co_hi = n64_rsp_vmask(rsp->vu.vco_hi);
    __m128i cc_lo = n64_rsp_vmask(rsp->vu.vcc_lo), cc_hi = n64_rsp_vmask(rsp->vu.vcc_hi);
    __m128i ce = n64_rsp_vmask(rsp->vu.vce);
    __m128i nvt = _mm_sub_epi16(_mm_xor_si128(vt, co_lo), co_lo);
    __m128i sum = _mm_add_epi16(vs, vt);
    __m128i sum0 = _mm_cmpeq_epi16(sum, zero);
    __m128i no_carry = _mm_cmpeq_epi16(_mm_adds_epu16(vs, vt), sum);
    __m128i le = n64_rsp_vsel(ce, _mm_or_si128(sum0, no_carry), _mm_and_si128(sum0, no_carry));
    __m128i ge = _mm_cmpeq_epi16(_mm_subs_epu16(vt, vs), zero);
    __m128i vd;
    cc_lo = n64_rsp_vsel(_mm_andnot_si128(co_hi, co_lo), le, cc_lo);
    cc_hi = n64_rsp_vsel(_mm_or_si128(co_lo, co_hi), cc_hi, ge);
    vd = n64_rsp_vsel(n64_rsp_vsel(co_lo, cc_lo, cc_hi), nvt, vs);
    n64_rsp_vst(rsp->vu.acc_l, vd);
    rsp->vu.vcc_lo = n64_rsp_vbits(cc_lo);
    rsp->vu.vcc_hi = n64_rsp_vbits(cc_hi);
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    rsp->vu.vce = 0;
    n64_rsp_vput(rsp, op, vd);
}

/* VCR. With differing signs, vs + vt + 1 <= 0 is vs + vt < 0. */
N64_FORCEINLINE void n64_rsp_op_vcr(N64Rsp *rsp, uint32_t op)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i differ = _mm_srai_epi16(_mm_xor_si128(vs, vt), 15);
    __m128i vt_neg = _mm_srai_epi16(vt, 15);
    __m128i le = _mm_srai_epi16(_mm_add_epi16(vs, vt), 15);
    __m128i ge = _mm_cmpeq_epi16(_mm_cmplt_epi16(vs, vt), _mm_setzero_si128());
    __m128i cc_lo = n64_rsp_vsel(differ, le, vt_neg), cc_hi = n64_rsp_vsel(differ, vt_neg, ge);
    __m128i vd = n64_rsp_vsel(n64_rsp_vsel(differ, cc_lo, cc_hi), _mm_xor_si128(vt, differ), vs);
    n64_rsp_vst(rsp->vu.acc_l, vd);
    rsp->vu.vcc_lo = n64_rsp_vbits(cc_lo);
    rsp->vu.vcc_hi = n64_rsp_vbits(cc_hi);
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    rsp->vu.vce = 0;
    n64_rsp_vput(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_op_vmrg(N64Rsp *rsp, uint32_t op)
{
    __m128i vd = n64_rsp_vsel(n64_rsp_vmask(rsp->vu.vcc_lo), n64_rsp_vs(rsp, op), n64_rsp_vte(rsp, op));
    n64_rsp_vst(rsp->vu.acc_l, vd);
    rsp->vu.vco_lo = 0;
    rsp->vu.vco_hi = 0;
    n64_rsp_vput(rsp, op, vd);
}

/* kind: 0 AND, 1 NAND, 2 OR, 3 NOR, 4 XOR, 5 NXOR, as n64_rsp_vlogic. */
N64_FORCEINLINE void n64_rsp_vlogic_sse2(N64Rsp *rsp, uint32_t op, unsigned kind)
{
    __m128i vs = n64_rsp_vs(rsp, op), vt = n64_rsp_vte(rsp, op);
    __m128i vd;
    switch (kind >> 1) {
    case 0: vd = _mm_and_si128(vs, vt); break;
    case 1: vd = _mm_or_si128(vs, vt); break;
    default: vd = _mm_xor_si128(vs, vt); break;
    }
    if (kind & 1u) vd = _mm_xor_si128(vd, _mm_cmpeq_epi16(vd, vd));
    n64_rsp_vst(rsp->vu.acc_l, vd);
    n64_rsp_vput(rsp, op, vd);
}

N64_FORCEINLINE void n64_rsp_op_vand(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic_sse2(rsp, op, 0); }
N64_FORCEINLINE void n64_rsp_op_vnand(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic_sse2(rsp, op, 1); }
N64_FORCEINLINE void n64_rsp_op_vor(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic_sse2(rsp, op, 2); }
N64_FORCEINLINE void n64_rsp_op_vnor(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic_sse2(rsp, op, 3); }
N64_FORCEINLINE void n64_rsp_op_vxor(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic_sse2(rsp, op, 4); }
N64_FORCEINLINE void n64_rsp_op_vnxor(N64Rsp *rsp, uint32_t op) { n64_rsp_vlogic_sse2(rsp, op, 5); }

/* The divide unit and VMOV work on one lane; only the copy of vt to the
 * accumulator's low slice is a vector. The lane is read before vd, which
 * may be the same register, is written. */
N64_FORCEINLINE void n64_rsp_vdivide_sse2(N64Rsp *rsp, uint32_t op, int is_low, int is_rsq)
{
    __m128i vt = n64_rsp_vte(rsp, op);
    uint32_t result = n64_rsp_divide(&rsp->vu, rsp->vu.v[N64_RSP_VT(op)][N64_RSP_VE(op) & 7u], is_low, is_rsq);
    n64_rsp_vst(rsp->vu.acc_l, vt);
    rsp->vu.v[N64_RSP_VD(op)][N64_RSP_VDE(op)] = (uint16_t)result;
}

N64_FORCEINLINE void n64_rsp_op_vrcp(N64Rsp *rsp, uint32_t op) { n64_rsp_vdivide_sse2(rsp, op, 0, 0); }
N64_FORCEINLINE void n64_rsp_op_vrcpl(N64Rsp *rsp, uint32_t op) { n64_rsp_vdivide_sse2(rsp, op, 1, 0); }
N64_FORCEINLINE void n64_rsp_op_vrsq(N64Rsp *rsp, uint32_t op) { n64_rsp_vdivide_sse2(rsp, op, 0, 1); }
N64_FORCEINLINE void n64_rsp_op_vrsql(N64Rsp *rsp, uint32_t op) { n64_rsp_vdivide_sse2(rsp, op, 1, 1); }

N64_FORCEINLINE void n64_rsp_op_vrcph(N64Rsp *rsp, uint32_t op)
{
    __m128i vt = n64_rsp_vte(rsp, op);
    int16_t in = n64_rsp_i16(rsp->vu.v[N64_RSP_VT(op)][N64_RSP_VE(op) & 7u]);
    n64_rsp_vst(rsp->vu.acc_l, vt);
    rsp->vu.div_dp = 1;
    rsp->vu.div_in = in;
    rsp->vu.v[N64_RSP_VD(op)][N64_RSP_VDE(op)] = (uint16_t)rsp->vu.div_out;
}

N64_FORCEINLINE void n64_rsp_op_vrsqh(N64Rsp *rsp, uint32_t op) { n64_rsp_op_vrcph(rsp, op); }

N64_FORCEINLINE void n64_rsp_op_vmov(N64Rsp *rsp, uint32_t op)
{
    __m128i vt = n64_rsp_vte(rsp, op);
    unsigned de = N64_RSP_VDE(op);
    uint16_t lane = rsp->vu.v[N64_RSP_VT(op)][n64_rsp_elem_lane[N64_RSP_VE(op)][de]];
    n64_rsp_vst(rsp->vu.acc_l, vt);
    rsp->vu.v[N64_RSP_VD(op)][de] = lane;
}

#else /* !N64_RSP_SSE2 */

N64_RSP_USE_REF(vmulf)
N64_RSP_USE_REF(vmulu)
N64_RSP_USE_REF(vmudl)
N64_RSP_USE_REF(vmudm)
N64_RSP_USE_REF(vmudn)
N64_RSP_USE_REF(vmudh)
N64_RSP_USE_REF(vmacf)
N64_RSP_USE_REF(vmacu)
N64_RSP_USE_REF(vmadl)
N64_RSP_USE_REF(vmadm)
N64_RSP_USE_REF(vmadn)
N64_RSP_USE_REF(vmadh)
N64_RSP_USE_REF(vadd)
N64_RSP_USE_REF(vsub)
N64_RSP_USE_REF(vabs)
N64_RSP_USE_REF(vaddc)
N64_RSP_USE_REF(vsubc)
N64_RSP_USE_REF(vzero)
N64_RSP_USE_REF(vsar)
N64_RSP_USE_REF(vlt)
N64_RSP_USE_REF(veq)
N64_RSP_USE_REF(vne)
N64_RSP_USE_REF(vge)
N64_RSP_USE_REF(vcl)
N64_RSP_USE_REF(vch)
N64_RSP_USE_REF(vcr)
N64_RSP_USE_REF(vmrg)
N64_RSP_USE_REF(vand)
N64_RSP_USE_REF(vnand)
N64_RSP_USE_REF(vor)
N64_RSP_USE_REF(vnor)
N64_RSP_USE_REF(vxor)
N64_RSP_USE_REF(vnxor)
N64_RSP_USE_REF(vrcp)
N64_RSP_USE_REF(vrcpl)
N64_RSP_USE_REF(vrcph)
N64_RSP_USE_REF(vmov)
N64_RSP_USE_REF(vrsq)
N64_RSP_USE_REF(vrsql)
N64_RSP_USE_REF(vrsqh)

#endif /* N64_RSP_SSE2 */

#ifdef __cplusplus
}
#endif

#endif /* N64_RSP_FAST_H */
