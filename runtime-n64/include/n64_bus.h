/*
 * The physical bus the VR4300 core calls. The final program provides it:
 * the machine (n64_bus.c) for a game, a flat memory for the vector harness
 * in tests/n64.
 *
 * Addresses are physical (the core translates, n64_cpu.c). Values are in
 * host order; memory holds big-endian bytes. The core reaches RDRAM itself
 * through links.rdram when the address is in it, so these see RDRAM only
 * from the slow path; they must give the same result.
 *
 * Contract: a read or write may advance c->cycles (an I/O access can stall
 * the CPU) and may raise an interrupt line, but never raises a CPU
 * exception, and never changes c->pc.
 */
#ifndef N64_BUS_H
#define N64_BUS_H

#include "n64_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t n64_bus_fetch(N64Cpu *c, uint32_t paddr);

uint8_t n64_bus_read8(N64Cpu *c, uint32_t paddr);
uint16_t n64_bus_read16(N64Cpu *c, uint32_t paddr);
uint32_t n64_bus_read32(N64Cpu *c, uint32_t paddr);
uint64_t n64_bus_read64(N64Cpu *c, uint32_t paddr);

void n64_bus_write8(N64Cpu *c, uint32_t paddr, uint8_t value);
void n64_bus_write16(N64Cpu *c, uint32_t paddr, uint16_t value);
void n64_bus_write32(N64Cpu *c, uint32_t paddr, uint32_t value);
void n64_bus_write64(N64Cpu *c, uint32_t paddr, uint64_t value);

/* A store from the fast path landed in a granule of RDRAM that
 * links.code_map marks: recompiled code there must be checked again. */
void n64_bus_code_written(N64Cpu *c, uint32_t paddr, uint32_t size);

/* Count or Compare was written, or the timer interrupt was acknowledged:
 * the machine reschedules its timer event (n64_cpu_timer_due). */
void n64_bus_timer_changed(N64Cpu *c);

/* Status or Cause was written, or ERET ran: an interrupt may now be taken.
 * The machine makes the run loop look (it sets c->next_sync). */
void n64_bus_interrupt_check(N64Cpu *c);

#ifdef __cplusplus
}
#endif

#endif /* N64_BUS_H */
