/*
 * Small helpers every part of the N64 runtime shares: force-inlining, and
 * big-endian loads and stores.
 *
 * Guest memory (RDRAM, the RSP's DMEM and IMEM, TMEM, the cartridge ROM)
 * is stored big-endian, byte for byte as the console holds it. Every
 * access of more than one byte goes through these helpers, so code that
 * writes a structure as words and reads it as bytes works on any host.
 * Fields are written one byte at a time: MSVC 19.51 /O2 once miscompiled a
 * loop of shifted stores (docs/ADDING-A-CONSOLE.md).
 */
#ifndef N64_COMMON_H
#define N64_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#define N64_FORCEINLINE static __forceinline
#else
#define N64_FORCEINLINE static inline __attribute__((always_inline))
#endif

static inline uint16_t n64_be16(const uint8_t *p) { return (uint16_t)(((uint32_t)p[0] << 8) | p[1]); }

static inline uint32_t n64_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t n64_be64(const uint8_t *p) { return ((uint64_t)n64_be32(p) << 32) | n64_be32(p + 4); }

static inline void n64_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void n64_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void n64_put_be64(uint8_t *p, uint64_t v)
{
    n64_put_be32(p, (uint32_t)(v >> 32));
    n64_put_be32(p + 4, (uint32_t)v);
}

static inline void n64_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* RDRAM: 4 MiB, or 8 MiB with the Expansion Pak (the default here, as the
 * hardware and most emulators boot with it present). */
#define N64_RDRAM_MAX 0x800000u

#endif /* N64_COMMON_H */
