#include "gc_test_bus.h"

uint8_t gc_test_memory[GC_TEST_SIZE];
unsigned gc_test_syscalls;
unsigned gc_test_faults;

/* An access outside the block is a fault, not a wrap: a test that strays is
 * a test that is wrong, and silently wrapping would hide it. */
static int in_range(uint32_t ea, unsigned size) {
  const uint32_t offset = ea - GC_TEST_BASE;
  return ea >= GC_TEST_BASE && offset <= GC_TEST_SIZE - size;
}

static uint8_t *at(GcCpu *c, uint32_t ea, unsigned size) {
  if (!in_range(ea, size)) {
    gc_test_faults++;
    c->dar = ea;
    c->pending_vector = GC_VEC_DSI;
    return NULL;
  }
  return &gc_test_memory[ea - GC_TEST_BASE];
}

void gc_test_reset_memory(void) {
  memset(gc_test_memory, 0, sizeof(gc_test_memory));
  gc_test_syscalls = 0;
  gc_test_faults = 0;
}

uint32_t gc_test_read32(uint32_t address) {
  const uint8_t *p = &gc_test_memory[address - GC_TEST_BASE];
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void gc_test_write32(uint32_t address, uint32_t value) {
  uint8_t *p = &gc_test_memory[address - GC_TEST_BASE];
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

void gc_test_load_code(GcCpu *c, uint32_t address, const uint32_t *words, unsigned count) {
  unsigned i;
  for (i = 0; i < count; i++) gc_test_write32(address + 4 * i, words[i]);
  c->pc = address;
  c->npc = address + 4;
}

uint32_t gc_bus_fetch(GcCpu *c, uint32_t ea) {
  const uint8_t *p = at(c, ea, 4);
  if (!p) return 0;
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint8_t gc_bus_read8(GcCpu *c, uint32_t ea) {
  const uint8_t *p = at(c, ea, 1);
  return p ? p[0] : 0;
}

uint16_t gc_bus_read16(GcCpu *c, uint32_t ea) {
  const uint8_t *p = at(c, ea, 2);
  if (!p) return 0;
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t gc_bus_read32(GcCpu *c, uint32_t ea) { return gc_bus_fetch(c, ea); }

uint64_t gc_bus_read64(GcCpu *c, uint32_t ea) {
  const uint8_t *p = at(c, ea, 8);
  uint64_t value = 0;
  unsigned i;
  if (!p) return 0;
  for (i = 0; i < 8; i++) value = (value << 8) | p[i];
  return value;
}

void gc_bus_write8(GcCpu *c, uint32_t ea, uint8_t value) {
  uint8_t *p = at(c, ea, 1);
  if (p) p[0] = value;
}

void gc_bus_write16(GcCpu *c, uint32_t ea, uint16_t value) {
  uint8_t *p = at(c, ea, 2);
  if (!p) return;
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

void gc_bus_write32(GcCpu *c, uint32_t ea, uint32_t value) {
  uint8_t *p = at(c, ea, 4);
  if (!p) return;
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

void gc_bus_write64(GcCpu *c, uint32_t ea, uint64_t value) {
  uint8_t *p = at(c, ea, 8);
  unsigned i;
  if (!p) return;
  for (i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (56 - 8 * i));
}

void gc_bus_dcbz(GcCpu *c, uint32_t ea) {
  uint8_t *p = at(c, ea, 32);
  if (p) memset(p, 0, 32);
}

void gc_bus_dcbz_l(GcCpu *c, uint32_t ea) { gc_bus_dcbz(c, ea); }

void gc_bus_icbi(GcCpu *c, uint32_t ea) {
  (void)c;
  (void)ea;
}

int gc_bus_spr_write(GcCpu *c, uint32_t spr, uint32_t value) {
  (void)c;
  (void)spr;
  (void)value;
  return 0;
}

int gc_bus_spr_read(GcCpu *c, uint32_t spr, uint32_t *value) {
  (void)c;
  (void)spr;
  (void)value;
  return 0;
}

/* The harness uses `sc` as its "the program is finished" marker rather than
 * taking a real exception. */
/* There is one flat, untranslated memory here, so the physical block a
 * reservation covers is the effective one. A store clears it the same way
 * the machine's bus does. */
void gc_bus_reserve(GcCpu *c, uint32_t ea) {
  c->reserve = 1;
  c->reserve_addr = ea & ~31u;
}

int gc_bus_reserve_held(GcCpu *c, uint32_t ea) {
  return c->reserve && c->reserve_addr == (ea & ~31u);
}

void gc_bus_syscall(GcCpu *c) {
  gc_test_syscalls++;
  c->stopped = GC_STOP_HALT;
}
