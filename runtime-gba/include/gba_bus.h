/*
 * Bus interface the ARM7TDMI core calls. The machine runtime implements it
 * with the real memory map; the SingleStepTests harness implements it by
 * replaying recorded transactions. Linking picks the implementation, so the
 * CPU core has no function-pointer indirection.
 *
 * Addresses are passed exactly as the CPU issues them, which may be
 * misaligned: the memory regions ignore the low bits, except the 8-bit
 * SRAM/Flash bus, which uses them to pick a byte. The CPU performs the
 * ARM7TDMI's rotation and sign-extension of misaligned loads itself. Each
 * call adds its own wait cycles to cpu->cycles.
 */
#ifndef GBA_BUS_H
#define GBA_BUS_H

#include "gba_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Access flags, matching the NanoBoyAdvance mask used by the test vectors. */
enum {
    GBA_ACCESS_NSEQ = 0,
    GBA_ACCESS_SEQ = 1,
    GBA_ACCESS_CODE = 2,
    GBA_ACCESS_DMA = 4,
    GBA_ACCESS_LOCK = 8,
};

uint32_t gba_bus_read8(GbaCpu *cpu, uint32_t addr, int access);
uint32_t gba_bus_read16(GbaCpu *cpu, uint32_t addr, int access);
uint32_t gba_bus_read32(GbaCpu *cpu, uint32_t addr, int access);
void gba_bus_write8(GbaCpu *cpu, uint32_t addr, uint8_t value, int access);
void gba_bus_write16(GbaCpu *cpu, uint32_t addr, uint16_t value, int access);
void gba_bus_write32(GbaCpu *cpu, uint32_t addr, uint32_t value, int access);

/* Internal (I) cycles, which also advance the prefetch unit. */
void gba_bus_idle(GbaCpu *cpu, int cycles);

#ifdef __cplusplus
}
#endif

#endif /* GBA_BUS_H */
