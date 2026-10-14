/*
 * ARM7TDMI multiply carry flag.
 *
 * Ported from zaydlang/multiplication-algorithm (impl.h, bitwise.h), which
 * models the ARM7TDMI's Booth-recoded carry-save multiplier to reproduce the
 * "meaningless" carry flag the CPU leaves after MUL, MLA and the long
 * multiplies with S.
 * Write-up: https://bmchtech.github.io/post/multiply/
 *
 * This is an altered version of the original: the global accumulator shift
 * register became a local, the multiplication result is dropped (the
 * interpreter computes it directly), and names follow this runtime. It is
 * the copy in runtimes/runtime-gba/src/gba_mul_carry.c, for the DS's ARM7; the ARM9
 * leaves C alone after a multiply. The original notice follows.
 *
 * Copyright (c) 2024 zaydlang
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must not
 *      claim that you wrote the original software. If you use this software
 *      in a product, an acknowledgment in the product documentation would be
 *      appreciated but is not required.
 *   2. Altered source versions must be plainly marked as such, and must not
 *      be misrepresented as being the original software.
 *   3. This notice may not be removed or altered from any source
 *      distribution.
 */
#include "nds_cpu_ops.h"

typedef struct {
    uint64_t value;
    bool carry;
} BoothAddend;

typedef struct {
    uint64_t output;
    uint64_t carry;
} CsaPair;

typedef struct {
    uint64_t lo;
    uint64_t hi;
} U128;

static inline bool bit_of(uint64_t x, int n)
{
    return (x >> n) & 1u;
}

static inline uint64_t bit_mask(int lo, int hi)
{
    return ((1ull << (hi - lo)) - 1ull) << lo;
}

static inline uint64_t sign_extend(uint64_t value, int from_size, int to_size)
{
    if (bit_of(value, from_size - 1)) value |= bit_mask(from_size, to_size);
    return value;
}

static inline uint64_t asr_n(uint64_t value, int shift, int size)
{
    int64_t extended = (int64_t)sign_extend(value, size, 64);
    /* Arithmetic shift written without relying on signed >> semantics. */
    uint64_t shifted = (uint64_t)extended >> shift;
    if (extended < 0) shifted |= ~(~0ull >> shift);
    return shifted & bit_mask(0, size);
}

static BoothAddend booth_recode(uint64_t input, unsigned chunk)
{
    BoothAddend out = { 0, false };
    switch (chunk & 7u) {
    case 1:
    case 2: out.value = input; break;
    case 3: out.value = 2 * input; break;
    case 4: out.value = ~(2 * input); out.carry = true; break;
    case 5:
    case 6: out.value = ~input; out.carry = true; break;
    default: break; /* 0 and 7 add nothing */
    }
    out.value &= 0x3FFFFFFFFull;
    return out;
}

static CsaPair csa(uint64_t a, uint64_t b, uint64_t c)
{
    CsaPair out = { a ^ b ^ c, (a & b) | (b & c) | (c & a) };
    return out;
}

/* One cycle: four Booth addends compressed by four carry-save adders. */
static CsaPair csa_array(CsaPair in, const BoothAddend addends[4], uint64_t *acc_shift)
{
    CsaPair cur = in;
    CsaPair final_out = { 0, 0 };

    for (int i = 0; i < 4; i++) {
        cur.output &= 0x1FFFFFFFFull;
        cur.carry &= 0x1FFFFFFFFull;

        CsaPair r = csa(cur.output, addends[i].value & 0x1FFFFFFFFull, cur.carry);

        /* Inject the carry from Booth negation. */
        r.carry <<= 1;
        r.carry |= addends[i].carry ? 1u : 0u;

        /* The bottom two bits are final; later addends are 4x larger. */
        final_out.output |= (r.output & 3u) << (2 * i);
        final_out.carry |= (r.carry & 3u) << (2 * i);
        r.output >>= 2;
        r.carry >>= 2;

        /* Sign-extension handling (TransH / High in the patents). */
        uint64_t magic = (uint64_t)bit_of(*acc_shift, 0) + (uint64_t)!bit_of(cur.carry, 32) +
                         (uint64_t)!bit_of(addends[i].value, 33);
        r.output |= magic << 31;
        r.carry |= (uint64_t)!bit_of(*acc_shift, 1) << 32;
        *acc_shift >>= 2;

        cur = r;
    }

    final_out.output |= cur.output << 8;
    final_out.carry |= cur.carry << 8;
    return final_out;
}

static U128 u128_ror(U128 v, int shift)
{
    U128 out = {
        (v.lo >> shift) | (v.hi << (64 - shift)),
        (v.hi >> shift) | (v.lo << (64 - shift)),
    };
    return out;
}

static bool flavor_signed(int flavor)
{
    return flavor == NDS_MUL_SHORT || flavor == NDS_MUL_LONG_SIGNED;
}

static bool should_terminate(uint64_t multiplier, int flavor)
{
    if (flavor_signed(flavor)) return multiplier == 0x1FFFFFFFFull || multiplier == 0;
    return multiplier == 0;
}

uint8_t nds_mul_carry(int flavor, uint32_t rm, uint32_t rs, uint64_t accumulator)
{
    uint64_t multiplicand = rm;
    uint64_t multiplier = rs;

    if (flavor_signed(flavor)) {
        multiplier = sign_extend(multiplier, 32, 34);
        multiplicand = sign_extend(multiplicand, 32, 34);
    } else {
        multiplier &= 0x1FFFFFFFFull;
        multiplicand &= 0x1FFFFFFFFull;
    }

    CsaPair cur;
    cur.carry = (multiplier & 1u) ? ~multiplicand : 0;
    cur.output = accumulator;
    uint64_t acc_shift = accumulator >> 34;

    U128 partial_sum = { cur.output & 1u, 0 };
    U128 partial_carry = { cur.carry & 1u, 0 };
    cur.output >>= 1;
    cur.carry >>= 1;
    partial_sum = u128_ror(partial_sum, 1);
    partial_carry = u128_ror(partial_carry, 1);

    int iterations = 0;
    do {
        BoothAddend addends[4];
        for (int i = 0; i < 4; i++) {
            addends[i] = booth_recode(multiplicand, (unsigned)(multiplier >> (2 * i)) & 7u);
        }
        cur = csa_array(cur, addends, &acc_shift);

        partial_sum.lo |= cur.output & 0xFFu;
        partial_carry.lo |= cur.carry & 0xFFu;
        cur.output >>= 8;
        cur.carry >>= 8;
        partial_sum = u128_ror(partial_sum, 8);
        partial_carry = u128_ror(partial_carry, 8);

        multiplier = asr_n(multiplier, 8, 33);
        iterations++;
    } while (!should_terminate(multiplier, flavor));
    partial_sum.lo |= cur.output;
    partial_carry.lo |= cur.carry;

    static const int correction_ror[5] = { 0, 23, 15, 7, 31 };
    partial_carry = u128_ror(partial_carry, correction_ror[iterations]);

    if (flavor == NDS_MUL_SHORT && iterations == 4) {
        return (uint8_t)((partial_carry.hi >> 31) & 1u);
    }
    return (uint8_t)((partial_carry.hi >> 63) & 1u);
}
