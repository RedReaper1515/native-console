/*
 * The bus the 65C816 core calls. The final program provides these: the
 * machine (snes_bus.c) for the game, a transaction-replaying bus for the
 * SingleStepTests harness. Each call is one bus cycle; the implementation
 * charges its master-clock cost to c->cycles and keeps c->mdr (open bus).
 *
 * Addresses are 24-bit (bank << 16 | offset). The CPU never masks for the
 * memory map: mirrors, open bus and I/O are the bus's business. The
 * machine's bus serves both the S-CPU (c->id 0) and the SA-1 (c->id 1).
 */
#ifndef SNES_BUS_H
#define SNES_BUS_H

#include "snes_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t snes_bus_read(SnesCpu *c, uint32_t addr);
void snes_bus_write(SnesCpu *c, uint32_t addr, uint8_t value);

/* An internal operation cycle (no memory access): 6 master clocks on the
 * S-CPU. */
void snes_bus_idle(SnesCpu *c);

/* The SA-1's extra cycle after a jump into ROM (see snes_io_jump in
 * snes_cpu_impl.h); only called for c->id != 0. */
void snes_bus_idle_jump(SnesCpu *c);

#ifdef __cplusplus
}
#endif

#endif /* SNES_BUS_H */
