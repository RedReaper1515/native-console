/*
 * The shortcut to main memory against the long way round.
 *
 * gc_bus.c fills the fast map (GcFastMap, gc_cpu.h) for the parts of the
 * address space where every address translates to main memory by the same
 * arithmetic, and generated code then loads and stores through it without
 * asking the bus. This holds the map to the bus's own walk: main memory is
 * filled with each word's own physical address, the data BATs are set to
 * random and deliberately awkward values - blocks smaller than the map's
 * 16 MB pieces, blocks that do not start a piece, earlier BATs overlapping
 * later ones, masks with holes in them, supervisor-invalid pairs - and at
 * every probe address the map offers, the word it points at has to be the
 * word gc_bus_read32 reads. A probe the map declines is not checked: the long
 * way is what answers it anyway.
 *
 * It also checks the map is not empty where it should not be: the layout
 * Shrek 2 uses, 8 MB at 0x81000000 on DBAT2, has to be answered by the map,
 * or its loading screen goes the long way two million times a field.
 */

#include <stdio.h>
#include <string.h>

#include "gc.h"
#include "gc_bus.h"
#include "gc_cpu_impl.h"
#include "gc_fpu_impl.h"

static uint64_t state = 0x2545f4914f6cdd1dull;

static uint32_t next(void) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (uint32_t)(state >> 16);
}

static const uint32_t starts[] = {0x00000000u, 0x80000000u, 0x81000000u, 0x81400000u, 0x81800000u,
                                  0x80800000u, 0x90000000u, 0xc0000000u, 0xc1000000u, 0x7e000000u};
static const uint32_t lengths[] = {0x000u, 0x001u, 0x003u, 0x00fu, 0x03fu, 0x07fu, 0x0ffu, 0x7ffu};
static const uint32_t physicals[] = {0x00000000u, 0x01000000u, 0x00800000u, 0x01400000u, 0x01800000u, 0x0c000000u};

static uint32_t random_upper(void) {
  const uint32_t r = next();
  uint32_t bepi = starts[r % (sizeof(starts) / sizeof(starts[0]))];
  uint32_t bl = lengths[(r >> 8) % (sizeof(lengths) / sizeof(lengths[0]))];
  if (((r >> 12) & 7u) == 0) bepi = next() & 0xfffe0000u;
  if (((r >> 15) & 7u) == 0) bl = next() & 0x7ffu; /* a mask with holes */
  return (bepi & 0xfffe0000u) | (bl << 2) | (((r >> 18) & 15u) ? 2u : 0u) | ((r >> 22) & 1u);
}

static uint32_t random_lower(void) {
  const uint32_t r = next();
  uint32_t brpn = physicals[r % (sizeof(physicals) / sizeof(physicals[0]))];
  if (((r >> 8) & 3u) == 0) brpn = next() & 0x01fe0000u;
  return brpn | 2u;
}

static unsigned probes, fast_probes, failures;

static void probe(GcMachine *m, uint32_t ea) {
  GcCpu *c = &m->cpu;
  ea &= ~3u;
  probes++;
  if (!GC_BUS_FAST_OK(c, ea, 4u)) return;
  {
    const uint8_t *p = GC_BUS_FAST_AT(c, ea);
    const uint32_t fast = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    uint32_t slow;
    c->pending_vector = 0;
    slow = gc_bus_read32(c, ea);
    fast_probes++;
    if (c->pending_vector || slow != fast) {
      if (failures < 20) {
        printf("FAIL: ea %08x msr %08x: map %08x, bus %08x (vector %x); dbat %08x %08x %08x %08x %08x %08x %08x %08x\n",
               ea, c->msr, fast, slow, c->pending_vector, c->dbat[0], c->dbat[1], c->dbat[2], c->dbat[3],
               c->dbat[4], c->dbat[5], c->dbat[6], c->dbat[7]);
      }
      failures++;
    }
    c->pending_vector = 0;
  }
}

/* Addresses on either side of every boundary a BAT or a piece draws. */
static void probe_all(GcMachine *m) {
  unsigned i, k;
  for (i = 0; i < 4u; i++) {
    const uint32_t base = m->cpu.dbat[i * 2] & 0xfffe0000u;
    for (k = 17; k <= 28; k++) {
      const uint32_t edge = base + (1u << k);
      probe(m, base);
      probe(m, edge - 4u);
      probe(m, edge);
      probe(m, (base & 0xff000000u) + (1u << k) - 4u);
      probe(m, (base & 0xff000000u) + (1u << k));
    }
  }
  for (i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
    probe(m, starts[i]);
    probe(m, starts[i] + 0x00fffffcu);
    probe(m, starts[i] + (next() & 0x00fffffcu));
  }
  for (i = 0; i < 64u; i++) probe(m, next() | (next() & 1u ? 0x80000000u : 0u));
}

int main(void) {
  GcMachine *m = gc_machine_create();
  const unsigned configurations = 20000u;
  unsigned i, shrek_fast;
  if (!m) return 1;
  m->log = NULL;
  for (i = 0; i + 4u <= GC_MEM1_SIZE; i += 4u) {
    m->mem1[i] = (uint8_t)(i >> 24);
    m->mem1[i + 1] = (uint8_t)(i >> 16);
    m->mem1[i + 2] = (uint8_t)(i >> 8);
    m->mem1[i + 3] = (uint8_t)i;
  }
  m->cpu.sdr1 = 0; /* no page table: an address no BAT takes faults */

  for (i = 0; i < configurations; i++) {
    unsigned k;
    GcCpu *c = &m->cpu;
    for (k = 0; k < 4u; k++) {
      c->dbat[k * 2] = random_upper();
      c->dbat[k * 2 + 1] = random_lower();
    }
    c->msr = (next() & 7u) ? GC_MSR_DR : 0u;
    gc_cpu_forget_fast(c);
    (void)gc_bus_read32(c, 0x80000000u); /* builds the map */
    c->pending_vector = 0;
    probe_all(m);
  }

  /* Shrek 2's layout. */
  {
    GcCpu *c = &m->cpu;
    memset(c->dbat, 0, sizeof(c->dbat));
    c->dbat[0] = 0x800001feu; /* 16 MB at 0x80000000 */
    c->dbat[1] = 0x00000002u;
    c->dbat[4] = 0x810000feu; /* 8 MB at 0x81000000 */
    c->dbat[5] = 0x01000002u;
    c->msr = GC_MSR_DR;
    gc_cpu_forget_fast(c);
    (void)gc_bus_read32(c, 0x80000000u);
    c->pending_vector = 0;
    shrek_fast = fast_probes;
    probe(m, 0x81000000u);
    probe(m, 0x81400000u);
    probe(m, 0x817ffffcu);
    shrek_fast = fast_probes - shrek_fast;
    probe(m, 0x81800000u);
    if (GC_BUS_FAST_OK(c, 0x81800000u, 4u) || GC_BUS_FAST_OK(c, 0x817ffffeu, 4u)) {
      printf("FAIL: the map answers past the end of the 8 MB block\n");
      failures++;
    }

    /* A quantised store that writes both lanes straight through the map
     * still has to clear a reservation on the block it writes, which only
     * the long way does. psq_st f4, 0(r5), GQR0 = u8. */
    c->gqr[0] = 0x00000004u;
    c->fpr[4].ps0 = 0x4000000000000000ull;
    c->fpr[4].ps1 = 0x4008000000000000ull;
    c->gpr[5] = 0x81400000u;
    gc_bus_reserve(c, 0x81400000u);
    gc_op_psq_st(c, 0xf0850000u);
    if (c->reserve || m->mem1[0x01400000] != 2 || m->mem1[0x01400001] != 3) {
      printf("FAIL: psq_st through the map kept a reservation on the block it wrote (%u) or wrote %u %u\n",
             c->reserve, m->mem1[0x01400000], m->mem1[0x01400001]);
      failures++;
    }
  }

  printf("gc_bus_fast_test: %u configurations, %u probes, %u through the map, %d failures\n", configurations, probes,
         fast_probes, failures);
  if (shrek_fast != 3u) {
    printf("FAIL: an 8 MB block at the start of a piece is not in the map (%u of 3 probes)\n", shrek_fast);
    failures++;
  }
  if (fast_probes < probes / 20u) {
    printf("FAIL: the map answered too few probes for this to have tested it\n");
    failures++;
  }
  gc_machine_destroy(m);
  return failures ? 1 : 0;
}
