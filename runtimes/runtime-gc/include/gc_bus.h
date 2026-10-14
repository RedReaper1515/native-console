/*
 * The bus the Gekko core calls. The final program provides it: the machine
 * (gc_bus.c) for a game, a flat memory for the vector harness in tests/gc.
 *
 * Addresses are effective addresses, exactly as the instruction computed
 * them. Translation (BATs, and the page table that games do not use), the
 * memory map, the uncached alias, the locked cache and MMIO are the bus's
 * business, not the core's.
 *
 * Values are in host order: guest memory holds big-endian, and these
 * functions swap. A read that faults raises the exception itself and sets
 * `c->pending_vector`; the core checks that after the access, so no guest
 * value is held in a C local across a fault.
 */
#ifndef GC_BUS_H
#define GC_BUS_H

#include "gc_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t gc_bus_fetch(GcCpu *c, uint32_t ea);

uint8_t gc_bus_read8(GcCpu *c, uint32_t ea);
uint16_t gc_bus_read16(GcCpu *c, uint32_t ea);
uint32_t gc_bus_read32(GcCpu *c, uint32_t ea);
uint64_t gc_bus_read64(GcCpu *c, uint32_t ea);

void gc_bus_write8(GcCpu *c, uint32_t ea, uint8_t value);
void gc_bus_write16(GcCpu *c, uint32_t ea, uint16_t value);
void gc_bus_write32(GcCpu *c, uint32_t ea, uint32_t value);
void gc_bus_write64(GcCpu *c, uint32_t ea, uint64_t value);

/* The same loads and stores, taking the shortcut to main memory when the
 * bus has said it is safe (GcFastMap in gc_cpu.h, and gc_bus.c for when a
 * piece of the address space qualifies). These are what the instruction
 * handlers call. Anything the map does not cover - translation it has not
 * seen, the register block, problem state, a store while a reservation is
 * held - is handed to the functions above, which do the whole walk. */
#define GC_BUS_FAST_MAP(c) (&(c)->fast[((c)->msr & GC_MSR_DR) ? 1 : 0])
#define GC_BUS_FAST_OK(c, ea, size) \
  (!((c)->msr & GC_MSR_PR) && ((ea) & 0xffffffu) + (size) <= GC_BUS_FAST_MAP(c)->limit[(ea) >> 24])
#define GC_BUS_FAST_AT(c, ea) (GC_BUS_FAST_MAP(c)->base[(ea) >> 24] + ((ea) & 0xffffffu))
/* The same through a map already chosen. */
#if defined(_MSC_VER)
#include <stdlib.h>
#define GC_BSWAP32(x) _byteswap_ulong(x)
#define GC_BSWAP64(x) _byteswap_uint64(x)
#else
#define GC_BSWAP32(x) __builtin_bswap32(x)
#define GC_BSWAP64(x) __builtin_bswap64(x)
#endif
#define GC_BUS_MAP_OK(map, ea, size) (((ea) & 0xffffffu) + (size) <= (map)->limit[(ea) >> 24])
#define GC_BUS_MAP_AT(map, ea) ((map)->base[(ea) >> 24] + ((ea) & 0xffffffu))

static inline uint8_t gc_bus_fast_read8(GcCpu *c, uint32_t ea) {
  if (GC_BUS_FAST_OK(c, ea, 1u)) return GC_BUS_FAST_AT(c, ea)[0];
  return gc_bus_read8(c, ea);
}

static inline uint16_t gc_bus_fast_read16(GcCpu *c, uint32_t ea) {
  if (GC_BUS_FAST_OK(c, ea, 2u)) {
    const uint8_t *p = GC_BUS_FAST_AT(c, ea);
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
  }
  return gc_bus_read16(c, ea);
}

static inline uint32_t gc_bus_fast_read32(GcCpu *c, uint32_t ea) {
  if (GC_BUS_FAST_OK(c, ea, 4u)) {
    const uint8_t *p = GC_BUS_FAST_AT(c, ea);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  }
  return gc_bus_read32(c, ea);
}

static inline uint64_t gc_bus_fast_read64(GcCpu *c, uint32_t ea) {
  if (GC_BUS_FAST_OK(c, ea, 8u)) {
    const uint8_t *p = GC_BUS_FAST_AT(c, ea);
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | p[7];
  }
  return gc_bus_read64(c, ea);
}

static inline void gc_bus_fast_write8(GcCpu *c, uint32_t ea, uint8_t value) {
  if (!c->reserve && GC_BUS_FAST_OK(c, ea, 1u)) {
    GC_BUS_FAST_AT(c, ea)[0] = value;
    return;
  }
  gc_bus_write8(c, ea, value);
}

static inline void gc_bus_fast_write16(GcCpu *c, uint32_t ea, uint16_t value) {
  if (!c->reserve && GC_BUS_FAST_OK(c, ea, 2u)) {
    uint8_t *p = GC_BUS_FAST_AT(c, ea);
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
    return;
  }
  gc_bus_write16(c, ea, value);
}

static inline void gc_bus_fast_write32(GcCpu *c, uint32_t ea, uint32_t value) {
  if (!c->reserve && GC_BUS_FAST_OK(c, ea, 4u)) {
    uint8_t *p = GC_BUS_FAST_AT(c, ea);
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
    return;
  }
  gc_bus_write32(c, ea, value);
}

static inline void gc_bus_fast_write64(GcCpu *c, uint32_t ea, uint64_t value) {
  if (!c->reserve && GC_BUS_FAST_OK(c, ea, 8u)) {
    uint8_t *p = GC_BUS_FAST_AT(c, ea);
    unsigned i;
    for (i = 0; i < 8u; i++) p[i] = (uint8_t)(value >> (56u - 8u * i));
    return;
  }
  gc_bus_write64(c, ea, value);
}

/* Cache maintenance the machine may need to act on: dcbz zeroes a line,
 * icbi invalidates recompiled code, dcbz_l allocates a locked-cache line. */
void gc_bus_dcbz(GcCpu *c, uint32_t ea);
void gc_bus_dcbz_l(GcCpu *c, uint32_t ea);
void gc_bus_icbi(GcCpu *c, uint32_t ea);

/* SPRs the core does not own (the write-gather pipe, the locked-cache DMA,
 * anything the machine models). Return non-zero when handled. */
int gc_bus_spr_write(GcCpu *c, uint32_t spr, uint32_t value);
int gc_bus_spr_read(GcCpu *c, uint32_t spr, uint32_t *value);

/* A reservation covers a block of *physical* memory, not the effective
 * address that named it: a store through the uncached alias has to clear
 * one taken through the cached view, and they share no effective address.
 * The core hands over the effective address and the bus records what it
 * translates to. */
void gc_bus_reserve(GcCpu *c, uint32_t ea);
int gc_bus_reserve_held(GcCpu *c, uint32_t ea);

/* `sc`: the machine decides what a system call means (a real exception for
 * a game, a test hook for the vector harness). */
void gc_bus_syscall(GcCpu *c);

#ifdef __cplusplus
}
#endif

#endif /* GC_BUS_H */
