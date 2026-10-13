/*
 * The ARM9's maths unit (GBATEK, "DS Maths"): a divider and a square-root
 * unit behind I/O registers 0x04000280-0x040002BF.
 *
 * Results are computed when a control or operand register is written. The
 * hardware takes 18 or 34 cycles to divide and 13 to take a root, and shows
 * a busy flag meanwhile; here the result is ready at once and the flag
 * never reads set.
 */
#include "nds.h"

static void divide(NdsMachine *m)
{
    /* Bit 14: the denominator is zero, judged on all 64 bits whatever the
     * mode. */
    m->divcnt &= 0xBFFFu;
    if (m->div_denom == 0) m->divcnt |= 0x4000u;

    switch (m->divcnt & 3u) {
    case 0: { /* 32 / 32 */
        int32_t numer = (int32_t)(uint32_t)m->div_numer;
        int32_t denom = (int32_t)(uint32_t)m->div_denom;
        if (denom == 0) {
            /* Quotient +1 or -1, opposite in sign to the numerator; the
             * remainder is the numerator. On an overflow in this mode the
             * upper half of the sign-extended quotient is inverted. */
            uint32_t quotient = numer < 0 ? 1u : 0xFFFFFFFFu;
            m->div_result = (uint64_t)(int64_t)(int32_t)quotient ^ 0xFFFFFFFF00000000u;
            m->div_remainder = (uint64_t)(int64_t)numer;
        } else if (numer == INT32_MIN && denom == -1) {
            m->div_result = 0x0000000080000000u;
            m->div_remainder = 0;
        } else {
            m->div_result = (uint64_t)(int64_t)(numer / denom);
            m->div_remainder = (uint64_t)(int64_t)(numer % denom);
        }
        break;
    }
    case 2: { /* 64 / 64 */
        int64_t numer = (int64_t)m->div_numer;
        int64_t denom = (int64_t)m->div_denom;
        if (denom == 0) {
            m->div_result = numer < 0 ? 1u : UINT64_MAX;
            m->div_remainder = (uint64_t)numer;
        } else if (numer == INT64_MIN && denom == -1) {
            m->div_result = (uint64_t)INT64_MIN;
            m->div_remainder = 0;
        } else {
            m->div_result = (uint64_t)(numer / denom);
            m->div_remainder = (uint64_t)(numer % denom);
        }
        break;
    }
    default: { /* 64 / 32 (mode 3 is the same) */
        int64_t numer = (int64_t)m->div_numer;
        int64_t denom = (int32_t)(uint32_t)m->div_denom;
        if (denom == 0) {
            m->div_result = numer < 0 ? 1u : UINT64_MAX;
            m->div_remainder = (uint64_t)numer;
        } else if (numer == INT64_MIN && denom == -1) {
            m->div_result = (uint64_t)INT64_MIN;
            m->div_remainder = 0;
        } else {
            m->div_result = (uint64_t)(numer / denom);
            m->div_remainder = (uint64_t)(numer % denom);
        }
        break;
    }
    }
}

/* Integer square root, rounded down. */
static void square_root(NdsMachine *m)
{
    uint64_t value = (m->sqrtcnt & 1u) ? m->sqrt_param : (uint32_t)m->sqrt_param;
    uint64_t root = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    m->sqrt_result = (uint32_t)root;
}

/* The unit's registers as one little-endian image, so any access width at
 * any offset is a matter of bytes. */
static uint8_t read_byte(const NdsMachine *m, uint32_t offset)
{
    uint64_t value;
    uint32_t base;
    if (offset < 0x284) { value = m->divcnt & 0x4003u; base = 0x280; }
    else if (offset >= 0x290 && offset < 0x298) { value = m->div_numer; base = 0x290; }
    else if (offset >= 0x298 && offset < 0x2A0) { value = m->div_denom; base = 0x298; }
    else if (offset >= 0x2A0 && offset < 0x2A8) { value = m->div_result; base = 0x2A0; }
    else if (offset >= 0x2A8 && offset < 0x2B0) { value = m->div_remainder; base = 0x2A8; }
    else if (offset >= 0x2B0 && offset < 0x2B4) { value = m->sqrtcnt & 0x0001u; base = 0x2B0; }
    else if (offset >= 0x2B4 && offset < 0x2B8) { value = m->sqrt_result; base = 0x2B4; }
    else if (offset >= 0x2B8 && offset < 0x2C0) { value = m->sqrt_param; base = 0x2B8; }
    else return 0;
    return (uint8_t)(value >> ((offset - base) * 8u));
}

uint32_t nds_math_read(NdsMachine *m, uint32_t offset, int size)
{
    uint32_t value = 0;
    for (int i = 0; i < size; i++) value |= (uint32_t)read_byte(m, offset + (uint32_t)i) << (8 * i);
    return value;
}

static void put_byte64(uint64_t *reg, uint32_t index, uint8_t byte)
{
    *reg = (*reg & ~((uint64_t)0xFF << (index * 8u))) | ((uint64_t)byte << (index * 8u));
}

void nds_math_write(NdsMachine *m, uint32_t offset, uint32_t value, int size)
{
    bool div = false, root = false;
    for (int i = 0; i < size; i++) {
        uint32_t at = offset + (uint32_t)i;
        uint8_t byte = (uint8_t)(value >> (8 * i));
        if (at == 0x280) { m->divcnt = (uint16_t)((m->divcnt & 0xFFFCu) | (byte & 3u)); div = true; }
        else if (at < 0x284) div = true;
        else if (at >= 0x290 && at < 0x298) { put_byte64(&m->div_numer, at - 0x290, byte); div = true; }
        else if (at >= 0x298 && at < 0x2A0) { put_byte64(&m->div_denom, at - 0x298, byte); div = true; }
        else if (at == 0x2B0) { m->sqrtcnt = (uint16_t)(byte & 1u); root = true; }
        else if (at > 0x2B0 && at < 0x2B4) root = true;
        else if (at >= 0x2B8 && at < 0x2C0) { put_byte64(&m->sqrt_param, at - 0x2B8, byte); root = true; }
    }
    if (div) divide(m);
    if (root) square_root(m);
}
