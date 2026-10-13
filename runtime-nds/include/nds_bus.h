/*
 * Bus interfaces the two CPU cores call: nds9_bus_* for the ARM9 and
 * nds7_bus_* for the ARM7. The machine runtime implements them with the two
 * memory maps; the vector harness implements them by replaying recorded
 * transactions. Linking picks the implementation, so the cores have no
 * function-pointer indirection.
 *
 * Addresses are passed exactly as the CPU issues them, which may be
 * misaligned: the bus ignores the low bits of halfword and word accesses.
 * The CPU performs the rotation of misaligned word loads itself. Each call
 * adds its own cycles to cpu->cycles.
 */
#ifndef NDS_BUS_H
#define NDS_BUS_H

#include "nds_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Access flags (the same values as the ARM7TDMI test vectors use). */
enum {
    NDS_ACCESS_NSEQ = 0,
    NDS_ACCESS_SEQ = 1,
    NDS_ACCESS_CODE = 2,
    NDS_ACCESS_DMA = 4,
    NDS_ACCESS_LOCK = 8,
};

uint32_t nds9_bus_read8(NdsCpu *cpu, uint32_t addr, int access);
uint32_t nds9_bus_read16(NdsCpu *cpu, uint32_t addr, int access);
uint32_t nds9_bus_read32(NdsCpu *cpu, uint32_t addr, int access);
void nds9_bus_write8(NdsCpu *cpu, uint32_t addr, uint8_t value, int access);
void nds9_bus_write16(NdsCpu *cpu, uint32_t addr, uint16_t value, int access);
void nds9_bus_write32(NdsCpu *cpu, uint32_t addr, uint32_t value, int access);
void nds9_bus_idle(NdsCpu *cpu, int cycles);

uint32_t nds7_bus_read8(NdsCpu *cpu, uint32_t addr, int access);
uint32_t nds7_bus_read16(NdsCpu *cpu, uint32_t addr, int access);
uint32_t nds7_bus_read32(NdsCpu *cpu, uint32_t addr, int access);
void nds7_bus_write8(NdsCpu *cpu, uint32_t addr, uint8_t value, int access);
void nds7_bus_write16(NdsCpu *cpu, uint32_t addr, uint16_t value, int access);
void nds7_bus_write32(NdsCpu *cpu, uint32_t addr, uint32_t value, int access);
void nds7_bus_idle(NdsCpu *cpu, int cycles);

/* Called by CP15 after a write changed how memory is laid out (the control
 * register or a TCM register), so the bus can remap ITCM and DTCM. */
void nds9_bus_cp15_changed(NdsCpu *cpu);

/* Called by the CP15 "wait for interrupt" operation: the ARM9 stops until
 * an enabled interrupt is pending. */
void nds9_bus_wait_for_interrupt(NdsCpu *cpu);

#ifdef __cplusplus
}
#endif

#endif /* NDS_BUS_H */
