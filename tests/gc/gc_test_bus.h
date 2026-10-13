/*
 * A flat big-endian memory for the CPU tests.
 *
 * The vector harness and the unit tests link this instead of the machine.
 * It is deliberately dumb: one block of memory at a base address, no
 * translation, no MMIO, no timing. What it does keep is the byte order -
 * guest memory here holds big-endian, exactly as it does on the console, so
 * a test that writes a word and reads its bytes back sees what hardware
 * would.
 */
#ifndef GC_TEST_BUS_H
#define GC_TEST_BUS_H

#include <stdint.h>
#include <string.h>

#include "gc_bus.h"
#include "gc_cpu.h"

#define GC_TEST_BASE 0x80000000u
#define GC_TEST_SIZE (1u << 20)

extern uint8_t gc_test_memory[GC_TEST_SIZE];
extern unsigned gc_test_syscalls;
extern unsigned gc_test_faults;

void gc_test_reset_memory(void);

/* Write an instruction stream at an address and point the CPU at it. */
void gc_test_load_code(GcCpu *c, uint32_t address, const uint32_t *words, unsigned count);

/* Helpers that see memory the way the guest does. */
uint32_t gc_test_read32(uint32_t address);
void gc_test_write32(uint32_t address, uint32_t value);

#endif /* GC_TEST_BUS_H */
