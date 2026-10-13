/*
 * Small things every part of the DS runtime shares: the time base, and
 * little-endian access to guest memory.
 */
#ifndef NDS_COMMON_H
#define NDS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Time. Everything in the machine is timed in "system cycles" of the
 * 33.513982 MHz clock. That is the ARM7's clock; the ARM9 runs at twice it.
 * The LCD's dot clock is a sixth of it: a scanline is 355 dots, a frame is
 * 263 lines, 192 of them visible, which gives 59.8261 frames a second.
 */
#define NDS_CLOCK_HZ 33513982
#define NDS_CYCLES_PER_DOT 6
#define NDS_DOTS_PER_LINE 355
#define NDS_VISIBLE_DOTS 256
#define NDS_CYCLES_PER_LINE (NDS_DOTS_PER_LINE * NDS_CYCLES_PER_DOT) /* 2130 */
#define NDS_LINES_PER_FRAME 263
#define NDS_VISIBLE_LINES 192
#define NDS_CYCLES_PER_FRAME (NDS_CYCLES_PER_LINE * NDS_LINES_PER_FRAME) /* 560190 */

#define NDS_SCREEN_WIDTH 256
#define NDS_SCREEN_HEIGHT 192

/* The sound mixer produces one stereo frame every 1024 system cycles
 * (32728.5 Hz). */
#define NDS_CYCLES_PER_SAMPLE 1024

/*
 * Guest memory is little-endian bytes. Read and write it through these,
 * never through a cast pointer: the addresses are often unaligned and the
 * host may not be little-endian.
 */
static inline uint16_t nds_get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t nds_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void nds_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline void nds_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* A 1, 2 or 4 byte little-endian value. */
static inline uint32_t nds_get_le(const uint8_t *p, int size)
{
    if (size == 4) return nds_get_le32(p);
    if (size == 2) return nds_get_le16(p);
    return p[0];
}

static inline void nds_put_le(uint8_t *p, uint32_t value, int size)
{
    if (size == 4) nds_put_le32(p, value);
    else if (size == 2) nds_put_le16(p, (uint16_t)value);
    else p[0] = (uint8_t)value;
}

#ifdef __cplusplus
}
#endif

#endif /* NDS_COMMON_H */
