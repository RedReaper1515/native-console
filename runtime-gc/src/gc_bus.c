/*
 * The bus: address translation, the physical memory map, the locked cache
 * and the write-gather pipe.
 *
 * Translation is modelled, not assumed. A game sets its own BATs and the
 * usual 0x80000000 mapping is only a convention, so this walks the BAT
 * registers the guest wrote. Anything that is not memory, not the locked
 * cache and not a register block is counted as a bad access and reported at
 * the end of a run: an address nobody implemented is the most likely reason
 * a program wanders off, and silently returning zero hides it.
 *
 * Guest memory holds big-endian bytes exactly as the console does. Every
 * access swaps here, so the core and the recompiled code deal in host-order
 * values and a structure the game writes as words and reads as bytes still
 * behaves.
 */

#include <string.h>

#include "gc.h"
#include "gc_bus.h"
#include "gc_efb.h"
#include "gc_gx.h"
#include "wii.h"

/* ------------------------------------------------------------------ */
/* Translation                                                         */

/* A BAT pair covers (BL + 1) * 128 KB. The upper register holds the
 * effective block number and the valid bits, the lower the physical block
 * number. */
static int bat_match(uint32_t upper, uint32_t lower, uint32_t ea, int problem_state,
                     uint32_t *physical) {
  const uint32_t bl = (upper >> 2) & 0x7ffu;
  const uint32_t block = (bl << 17) | 0x1ffffu; /* bits inside the block */
  const uint32_t valid = problem_state ? (upper & 1u) : ((upper >> 1) & 1u);
  if (!valid) return 0;
  if (((ea ^ upper) & 0xfffe0000u & ~block) != 0) return 0;
  *physical = (lower & 0xfffe0000u & ~block) | (ea & block);
  return 1;
}

/* The hashed page table.
 *
 * Block translations cover a few large regions and that is all most
 * GameCube software ever needs - so this sat unimplemented for a long
 * time, with a comment saying nobody uses it. Shrek 2 uses it: it takes a
 * region no block covers, installs its own handler at the data storage
 * vector, and maps pages as they are asked for. Without this the handler
 * is entered, does its work, returns, and faults again on the same address
 * for ever, which looks like a hang and is a missing feature.
 *
 * The architecture's own algorithm, unchanged: the top four bits of the
 * address choose a segment register, which supplies a virtual segment id;
 * that and the page index are hashed into a group of eight entries; the
 * group is searched, and then the group the complementary hash names. An
 * entry matches when it is valid, its segment id agrees, its hash bit says
 * which of the two groups it was filed under, and its abbreviated page
 * index agrees with the address.
 *
 * The referenced and changed bits are written back on a hit. Software that
 * reclaims pages reads them, and a page that never looks referenced is a
 * page that gets reclaimed while it is in use. */
static int page_table_lookup(GcCpu *c, uint32_t ea, int is_store, uint32_t *physical) {
  GcMachine *m = (GcMachine *)c;
  const uint32_t segment = c->sr[(ea >> 28) & 15u];
  const uint32_t vsid = segment & 0x00ffffffu;
  const uint32_t page_index = (ea >> 12) & 0xffffu;
  const uint32_t api = (ea >> 22) & 0x3fu;
  const uint32_t htabmask = c->sdr1 & 0x1ffu;
  unsigned which;

  /* A direct-store segment is a different thing entirely and nothing here
   * has one. */
  if (segment & 0x80000000u) return 0;

  for (which = 0; which < 2u; which++) {
    const uint32_t hash =
        (which ? ~((vsid & 0x7ffffu) ^ page_index) : ((vsid & 0x7ffffu) ^ page_index)) & 0x7ffffu;
    const uint32_t group = (((c->sdr1 >> 25) & 0x7fu) << 25) |
                           (((((c->sdr1 >> 16) & 0x1ffu) & ~htabmask) |
                             ((hash >> 10) & htabmask))
                            << 16) |
                           ((hash & 0x3ffu) << 6);
    uint8_t *entries = gc_machine_ram(m, group, 64u);
    unsigned slot;
    if (!entries) return 0;

    for (slot = 0; slot < 8u; slot++) {
      uint8_t *entry = entries + slot * 8u;
      const uint32_t high = ((uint32_t)entry[0] << 24) | ((uint32_t)entry[1] << 16) |
                            ((uint32_t)entry[2] << 8) | entry[3];
      const uint32_t low = ((uint32_t)entry[4] << 24) | ((uint32_t)entry[5] << 16) |
                           ((uint32_t)entry[6] << 8) | entry[7];
      if (!(high & 0x80000000u)) continue;                  /* not valid */
      if (((high >> 7) & 0x00ffffffu) != vsid) continue;    /* another segment */
      if (((high >> 6) & 1u) != which) continue;            /* filed under the other hash */
      if ((high & 0x3fu) != api) continue;                  /* another page */

      /* Referenced, and changed when this is a store. */
      {
        uint32_t updated = low | 0x00000100u;
        if (is_store) updated |= 0x00000080u;
        if (updated != low) {
          entry[4] = (uint8_t)(updated >> 24);
          entry[5] = (uint8_t)(updated >> 16);
          entry[6] = (uint8_t)(updated >> 8);
          entry[7] = (uint8_t)updated;
        }
      }
      *physical = (low & 0xfffff000u) | (ea & 0x00000fffu);
      return 1;
    }
  }
  return 0;
}

/* What an access is, for translation: a data read, an instruction fetch,
 * or a store. A store has to say so, because the page table records it: a
 * hit sets the page's changed bit, and software that pages memory out
 * decides from that bit whether a page must be written back before it is
 * reused. A store that left it clear made every page look clean - Shrek 2's
 * virtual memory then dropped pages it had just filled and read garbage
 * back, and freed a block through a pointer made of it. */
#define GC_ACCESS_READ 0
#define GC_ACCESS_FETCH 1
#define GC_ACCESS_STORE 2

static int translate(GcCpu *c, uint32_t ea, int access, uint32_t *physical) {
  const int is_fetch = access == GC_ACCESS_FETCH;
  const uint32_t *bats = is_fetch ? c->ibat : c->dbat;
  const int problem_state = (c->msr & GC_MSR_PR) != 0;
  const uint32_t enable = is_fetch ? GC_MSR_IR : GC_MSR_DR;
  unsigned i;

  if ((c->msr & enable) == 0) {
    /* Translation off: the effective address is the physical address. */
    *physical = ea;
    return 1;
  }
  for (i = 0; i < 4; i++) {
    if (bat_match(bats[i * 2], bats[i * 2 + 1], ea, problem_state, physical)) return 1;
  }
  /* Broadway's second four pairs, when HID4 has turned them on (wii.h). */
  {
    const WiiState *w = (const WiiState *)c->machine;
    if (w && (w->hid4 & WII_HID4_SBE)) {
      const uint32_t *high = is_fetch ? w->ibat_high : w->dbat_high;
      for (i = 0; i < 4; i++) {
        if (bat_match(high[i * 2], high[i * 2 + 1], ea, problem_state, physical)) return 1;
      }
    }
  }
  /* No block matched, so the page table - if there is one. */
  if (c->sdr1) return page_table_lookup(c, ea, access == GC_ACCESS_STORE, physical);
  return 0;
}

/* ------------------------------------------------------------------ */
/* The physical map                                                    */

/* Main memory, and only main memory. The framebuffer is not addressable as
 * bytes and the register block is not memory at all; both are answered
 * before anything asks for a pointer. */
uint8_t *gc_machine_ram(GcMachine *m, uint32_t physical, uint32_t size) {
  const uint32_t offset = gc_ram_offset(physical);
  if (offset == GC_RAM_NONE) return NULL;
  /* The two blocks sit end to end in the host and nowhere near each other
   * in the machine: an access may not run off the end of the one it began
   * in. */
  if (size > (offset < GC_MEM1_SIZE ? GC_MEM1_SIZE : gc_ram_size) - offset) return NULL;
  return m->mem1 + offset;
}

/* The locked cache answers at its own effective address range once the
 * guest turns it on; it is 16 KB of the data cache, not main memory. */
static uint8_t *locked_cache(GcMachine *m, uint32_t ea, uint32_t size) {
  const uint32_t offset = ea - GC_LOCKED_CACHE_EA;
  if ((m->cpu.hid2 & GC_HID2_LCE) == 0) return NULL;
  if (ea < GC_LOCKED_CACHE_EA || offset + size > GC_LOCKED_CACHE_SIZE) return NULL;
  return m->locked_cache + offset;
}

static void bad_access(GcMachine *m, const char *what, uint32_t ea, uint32_t physical) {
  m->bad_access++;
  if (m->bad_access <= 16 && m->log) {
    fprintf(m->log, "gc: %s at ea %08x (physical %08x), pc %08x\n", what, ea, physical,
            m->cpu.pc);
  }
  /* The first one, in full, when asked. An address that is not memory is
   * almost always a pointer nobody set, and the useful questions - which
   * register held it, and who called the function that used it - do not
   * survive to the millionth one. */
  if (m->stop_on_bad_access && !m->cpu.stopped) {
    m->cpu.stopped = GC_STOP_BAD_ACCESS;
    m->cpu.stop_insn = ea;
  }
}

/* The write-gather pipe collects 32 bytes at a time and writes them, as one
 * burst, to the physical address in WPAR. A game points WPAR at the command
 * FIFO and the burst is a display list; anything else, and it is a fast
 * uncached memcpy, which is what
 * tests/gc/programs/gc_memtest.c uses to watch the pipe work.
 *
 * The FIFO end of it belongs to P3. Until then a burst aimed at the
 * register block is counted rather than written, so that a game driving the
 * pipe is visible and does not land on top of the registers. */
static void gather_flush(GcMachine *m) {
  const uint32_t physical = m->cpu.wpar & ~31u;
  uint8_t *p;
  if (physical >= GC_PHYS_IO && physical < GC_PHYS_IO + GC_IO_SIZE) {
    m->gather_bursts++;
    /* Aimed at the FIFO port, which is where a game aims it: the burst
     * goes into the graphics ring and the graphics processor reads it. */
    if (physical - GC_PHYS_IO >= 0x8000u) gc_gx_fifo_write(m, m->gather, sizeof(m->gather));
    return;
  }
  p = gc_machine_ram(m, physical, sizeof(m->gather));
  if (!p) {
    bad_access(m, "the write-gather pipe aimed at nothing", m->cpu.wpar, physical);
    return;
  }
  memcpy(p, m->gather, sizeof(m->gather));
  m->gather_bursts++;
}

static void gather_write(GcMachine *m, const uint8_t *bytes, unsigned size) {
  unsigned i;
  for (i = 0; i < size; i++) {
    m->gather[m->gather_used++] = bytes[i];
    if (m->gather_used == sizeof(m->gather)) {
      gather_flush(m);
      m->gather_used = 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Access                                                              */

static uint64_t load_bytes(const uint8_t *p, unsigned size) {
  uint64_t value = 0;
  unsigned i;
  for (i = 0; i < size; i++) value = (value << 8) | p[i];
  return value;
}

static void store_bytes(uint8_t *p, unsigned size, uint64_t value) {
  unsigned i;
  for (i = 0; i < size; i++) p[i] = (uint8_t)(value >> (8 * (size - 1 - i)));
}

/* Why a data access failed, in the register the handler reads to find out.
 *
 * A game that wants more address space than a GameCube has can install its
 * own handler here and map memory on demand - Shrek 2 does exactly that,
 * replacing the vector with a branch to its own code and then using a
 * region no block translation covers. That handler needs two things: the
 * address, which goes in DAR, and *what kind* of access it was, which goes
 * here. Leaving this at zero tells the handler the translation succeeded
 * and the fault was something else, and there is nothing sensible for it
 * to do with that.
 *
 * Bit 1 says the translation was not found; bit 6 says it was a store. The
 * rest - protection, external control, the address breakpoint - do not
 * arise here. */
#define GC_DSISR_NOT_FOUND 0x40000000u
#define GC_DSISR_STORE 0x02000000u

/* The shortcut to main memory (GcFastMap, gc_bus.h).
 *
 * A piece of the address space - 16 MB of it - goes into the map only when
 * the long way round would take every address in it to main memory by the
 * same arithmetic: not the locked cache, a single BAT that covers the whole
 * piece and is the first to match every address in it, and a physical
 * address inside main memory. Anything else - the register block, the
 * framebuffer window, the page table, a BAT smaller than the piece - is left
 * out and keeps going the long way. So a load or store through the map does
 * exactly what bus_read and bus_write would have done, minus the walking.
 *
 * One thing the long way does for RAM is not "exactly nothing" and so turns
 * the map off rather than being skipped: a watched address. A store while a
 * reservation is held also goes the long way, so that the reservation can
 * be cleared. (The OpenGL path used to turn it off too, because the
 * framebuffer window's handlers told it about every store before checking
 * the address; they check first now, and the path hears only about the
 * window, which the map never covers.) */
#define GC_FAST_PIECE 0x01000000u

/* How much of a piece, from its start, one BAT takes to main memory by the
 * same arithmetic: 0 if it does not match the start at all.
 *
 * A block ignores its low k bits, where k is the number of ones at the
 * bottom of its mask (at least 17). Every address in [start, start + 2^k)
 * differs from the start only in those bits, so each of them matches
 * exactly when the start does, and translates to the start's physical
 * address plus its distance from the start. Capped at the piece. A BAT that
 * covers the whole piece gives the whole piece; a smaller one - Shrek 2
 * maps 8 MB at 0x81000000 - gives the part it covers, which the map's limit
 * can express because that part begins at the piece's start. */
static uint32_t bat_covers_prefix(uint32_t upper, uint32_t lower, uint32_t start, uint32_t *physical) {
  const uint32_t bl = (upper >> 2) & 0x7ffu;
  const uint32_t block = (bl << 17) | 0x1ffffu;
  uint32_t extent = 0x20000u;
  if (!((upper >> 1) & 1u)) return 0; /* not valid in supervisor state */
  if (((start ^ upper) & 0xfffe0000u & ~block) != 0) return 0;
  while (extent < GC_FAST_PIECE && (block & extent) != 0) extent <<= 1;
  *physical = (lower & 0xfffe0000u & ~block) | (start & block);
  return extent;
}

/* Does this BAT match any address in [start, start + extent)? Addresses
 * there differ from the start only in their low bits (extent is a power of
 * two), so only the bits above those are constrained. */
static int bat_touches_prefix(uint32_t upper, uint32_t start, uint32_t extent) {
  const uint32_t bl = (upper >> 2) & 0x7ffu;
  const uint32_t block = (bl << 17) | 0x1ffffu;
  if (!((upper >> 1) & 1u)) return 0;
  return ((start ^ upper) & 0xfffe0000u & ~block & ~(extent - 1u)) == 0;
}

static void fast_build(GcCpu *c) {
  GcMachine *m = (GcMachine *)c;
  const WiiState *w = (const WiiState *)c->machine;
  /* The data BATs in the order they are searched: the four every Gekko has,
   * then Broadway's second four when they are on. */
  uint32_t bats[16];
  const unsigned bat_count = (w && (w->hid4 & WII_HID4_SBE)) ? 8u : 4u;
  unsigned mode, piece, i;
  memcpy(bats, c->dbat, sizeof(c->dbat));
  if (bat_count > 4u) memcpy(bats + 8, w->dbat_high, sizeof(w->dbat_high));
  gc_cpu_forget_fast(c);
  c->fast_ready = 1;
  if (m->watch_address) return;
  for (mode = 0; mode < 2u; mode++) {
    for (piece = 0; piece < 256u; piece++) {
      const uint32_t start = piece << 24;
      uint32_t physical = 0;
      uint32_t extent = 0;
      if (start == (GC_LOCKED_CACHE_EA & 0xff000000u)) continue;
      if (mode == 0) {
        physical = start;
        extent = GC_FAST_PIECE;
      } else {
        /* The first BAT that matches the start decides the prefix; one
         * before it that matches anything in the prefix would win there
         * instead, and then the piece goes the long way. */
        for (i = 0; i < bat_count && !extent; i++) {
          extent = bat_covers_prefix(bats[i * 2], bats[i * 2 + 1], start, &physical);
        }
        if (extent) {
          unsigned j;
          for (j = 0; j + 1u < i; j++) {
            if (bat_touches_prefix(bats[j * 2], start, extent)) extent = 0;
          }
        }
      }
      if (!extent) continue;
      {
        /* Up to the end of whichever block of memory the piece starts in. */
        const uint32_t offset = gc_ram_offset(physical);
        uint32_t left;
        if (offset == GC_RAM_NONE) continue;
        left = (offset < GC_MEM1_SIZE ? GC_MEM1_SIZE : gc_ram_size) - offset;
        c->fast[mode].base[piece] = m->mem1 + offset;
        c->fast[mode].limit[piece] = left < extent ? left : extent;
      }
    }
  }
}

static uint64_t bus_read(GcCpu *c, uint32_t ea, unsigned size, int is_fetch) {
  GcMachine *m = (GcMachine *)c;
  uint32_t physical;
  uint8_t *p;

  if (!c->fast_ready) fast_build(c);

  p = locked_cache(m, ea, size);
  if (p) return load_bytes(p, size);

  if (!translate(c, ea, is_fetch ? GC_ACCESS_FETCH : GC_ACCESS_READ, &physical)) {
    bad_access(m, is_fetch ? "untranslated fetch" : "untranslated read", ea, 0);
    c->dar = ea;
    /* An instruction fetch reports itself in SRR1 rather than DSISR, and
     * the bit means the same thing in both. */
    if (is_fetch) {
      c->srr1 = (c->srr1 & ~0x78000000u) | GC_DSISR_NOT_FOUND;
    } else {
      c->dsisr = GC_DSISR_NOT_FOUND;
    }
    c->pending_vector = is_fetch ? GC_VEC_ISI : GC_VEC_DSI;
    return 0;
  }

  if (physical >= GC_PHYS_IO && physical < GC_PHYS_IO + GC_IO_SIZE) {
    return gc_io_read(m, physical - GC_PHYS_IO, size);
  }
  if (c->machine && wii_io_claims(physical)) return wii_io_read(m, physical, size);
  {
    uint32_t pixel;
    if (gc_efb_read(m, physical, size, &pixel)) return pixel;
  }

  p = gc_machine_ram(m, physical, size);
  if (!p) {
    bad_access(m, "read from nothing", ea, physical);
    return 0;
  }
  return load_bytes(p, size);
}

static void bus_write(GcCpu *c, uint32_t ea, unsigned size, uint64_t value) {
  /* A watched address, when one was asked for. Half of finding out why a
   * pointer is null is finding out whether anything ever wrote it. */
  {
    GcMachine *watcher = (GcMachine *)c;
    if (watcher->watch_address && ea <= watcher->watch_address &&
        watcher->watch_address < ea + size && watcher->log) {
      fprintf(watcher->log, "gc: watch %08x written as %0*llx by pc %08x (lr %08x)\n",
              watcher->watch_address, (int)(size * 2u), (unsigned long long)value, c->pc, c->lr);
    }
    /* A NaN going into memory, when a run asked to be told where NaNs come
     * from (gc_cpu.h): the first stores after the field given are the
     * nearest thing to where a scene's NaNs began. */
    if (gc_fp_invalid_trace && watcher->log && watcher->field >= gc_fp_invalid_from &&
        /* Not the stack: a function that takes a variable number of
         * arguments saves eight floating-point registers there whatever
         * they hold, and they often hold a NaN nobody will read. */
        ea - c->gpr[1] >= 0x10000u &&
        ((size == 4u && ((uint32_t)value & 0x7fffffffu) == 0x7fc00000u) ||
         (size == 8u && (value & 0x7fffffffffffffffull) == 0x7ff8000000000000ull))) {
      static unsigned said;
      if (said < 200u) {
        said++;
        fprintf(watcher->log, "gc: field %llu: a NaN stored to %08x by pc %08x (lr %08x)\n",
                (unsigned long long)watcher->field, ea, c->pc, c->lr);
      }
    }
  }
  GcMachine *m = (GcMachine *)c;
  uint32_t physical;
  uint8_t *p;

  if (!c->fast_ready) fast_build(c);
  p = locked_cache(m, ea, size);
  if (p) {
    store_bytes(p, size, value);
    return;
  }

  if (!translate(c, ea, GC_ACCESS_STORE, &physical)) {
    bad_access(m, "untranslated write", ea, 0);
    c->dar = ea;
    c->dsisr = GC_DSISR_NOT_FOUND | GC_DSISR_STORE;
    c->pending_vector = GC_VEC_DSI;
    return;
  }

  if (physical >= GC_PHYS_IO && physical < GC_PHYS_IO + GC_IO_SIZE) {
    /* The write-gather pipe is a single address the guest streams into,
     * and it only gathers while HID2[WPE] is set - which libogc leaves
     * clear until GX_Init turns it on, so a store here with it clear is a
     * program getting ahead of itself rather than normal traffic. */
    if (physical - GC_PHYS_IO >= 0x8000u) {
      uint8_t bytes[8];
      if ((c->hid2 & GC_HID2_WPE) == 0) {
        bad_access(m, "the write-gather pipe is not enabled", ea, physical);
        return;
      }
      store_bytes(bytes, size, value);
      gather_write(m, bytes, size);
      return;
    }
    gc_io_write(m, physical - GC_PHYS_IO, size, (uint32_t)value);
    return;
  }
  if (c->machine && wii_io_claims(physical)) {
    wii_io_write(m, physical, size, (uint32_t)value);
    return;
  }
  if (gc_efb_write(m, physical, size, (uint32_t)value)) return;

  p = gc_machine_ram(m, physical, size);
  if (!p) {
    bad_access(m, "write to nothing", ea, physical);
    return;
  }
  store_bytes(p, size, value);

  /* A store into the block a reservation covers clears it, wherever it came
   * from - including through the other view of the same memory, which is
   * why the reservation is kept as a physical address. */
  if (c->reserve && (physical & ~31u) == c->reserve_addr) c->reserve = 0;
}

void gc_bus_reserve(GcCpu *c, uint32_t ea) {
  uint32_t physical;
  if (!translate(c, ea, 0, &physical)) {
    c->reserve = 0;
    return;
  }
  c->reserve = 1;
  c->reserve_addr = physical & ~31u;
}

int gc_bus_reserve_held(GcCpu *c, uint32_t ea) {
  uint32_t physical;
  if (!c->reserve) return 0;
  if (!translate(c, ea, 0, &physical)) return 0;
  return (physical & ~31u) == c->reserve_addr;
}

uint32_t gc_bus_fetch(GcCpu *c, uint32_t ea) { return (uint32_t)bus_read(c, ea, 4, 1); }

uint8_t gc_bus_read8(GcCpu *c, uint32_t ea) { return (uint8_t)bus_read(c, ea, 1, 0); }
uint16_t gc_bus_read16(GcCpu *c, uint32_t ea) { return (uint16_t)bus_read(c, ea, 2, 0); }
uint32_t gc_bus_read32(GcCpu *c, uint32_t ea) { return (uint32_t)bus_read(c, ea, 4, 0); }
uint64_t gc_bus_read64(GcCpu *c, uint32_t ea) { return bus_read(c, ea, 8, 0); }

void gc_bus_write8(GcCpu *c, uint32_t ea, uint8_t value) { bus_write(c, ea, 1, value); }
void gc_bus_write16(GcCpu *c, uint32_t ea, uint16_t value) { bus_write(c, ea, 2, value); }
void gc_bus_write32(GcCpu *c, uint32_t ea, uint32_t value) { bus_write(c, ea, 4, value); }
void gc_bus_write64(GcCpu *c, uint32_t ea, uint64_t value) { bus_write(c, ea, 8, value); }

/* ------------------------------------------------------------------ */
/* Cache maintenance                                                   */

void gc_bus_dcbz(GcCpu *c, uint32_t ea) {
  GcMachine *m = (GcMachine *)c;
  uint32_t physical;
  uint8_t *p = locked_cache(m, ea, 32);
  if (p) {
    memset(p, 0, 32);
    return;
  }
  if (!translate(c, ea, GC_ACCESS_STORE, &physical)) {
    bad_access(m, "untranslated dcbz", ea, 0);
    return;
  }
  p = gc_machine_ram(m, physical, 32);
  if (!p) {
    bad_access(m, "dcbz on nothing", ea, physical);
    return;
  }
  memset(p, 0, 32);
}

/* dcbz_l allocates a line in the locked cache without reading memory: it is
 * how a game claims scratch space, and the line comes up as zeroes. */
void gc_bus_dcbz_l(GcCpu *c, uint32_t ea) {
  GcMachine *m = (GcMachine *)c;
  uint8_t *p = locked_cache(m, ea, 32);
  if (!p) {
    bad_access(m, "dcbz_l outside the locked cache", ea, 0);
    return;
  }
  memset(p, 0, 32);
}

void gc_bus_icbi(GcCpu *c, uint32_t ea) {
  (void)c;
  (void)ea;
  /* Recompiled code installed from this address range is invalidated here
   * once there is a recompiler (P4). */
}

/* ------------------------------------------------------------------ */
/* SPRs the machine owns                                               */

int gc_bus_spr_write(GcCpu *c, uint32_t spr, uint32_t value) {
  GcMachine *m = (GcMachine *)c;
  switch (spr) {
    case GC_SPR_WPAR:
      /* The write-gather pipe's target address, and writing it drops
       * whatever was half-gathered. */
      c->wpar = value & ~0x1fu;
      m->gather_used = 0;
      return 1;
    case GC_SPR_DMAU:
      c->dmau = value;
      return 1;
    case GC_SPR_DMAL:
      c->dmal = value;
      /* A locked-cache DMA moves lines between the cache and memory; the
       * transfer is instant here because nothing can observe it in
       * progress. */
      if (value & 2u) {
        /* Neither address arrives whole. libogc writes the locked-cache
         * address here with 0xe0000000 already masked off, and the memory
         * address with 0x80000000 left on, and both work on a console - so
         * the hardware ignores the bits above each. The memory side is
         * therefore a physical address with the view-selector bits dropped,
         * and the cache side an offset into the 16 KB.
         *
         * The length is seven bits of 32-byte blocks split across the two
         * registers - five here, two there - and zero means 128. */
        const uint32_t physical = c->dmau & 0x1fffffe0u;
        const uint32_t length = ((c->dmau & 0x1fu) << 2) | ((value >> 2) & 3u);
        const uint32_t bytes = (length ? length : 128u) * 32u;
        const uint32_t offset = value & (GC_LOCKED_CACHE_SIZE - 32u);
        uint8_t *memory = gc_machine_ram(m, physical, bytes);
        uint8_t *cache =
            (offset + bytes <= GC_LOCKED_CACHE_SIZE) ? m->locked_cache + offset : NULL;
        if (memory && cache) {
          if (value & 0x10u) {
            memcpy(cache, memory, bytes); /* load into the locked cache */
          } else {
            memcpy(memory, cache, bytes); /* store back */
          }
        } else {
          bad_access(m, "locked-cache DMA outside memory", c->dmau, physical);
        }
      }
      return 1;
    default: break;
  }
  return wii_spr_write(m, spr, value);
}

int gc_bus_spr_read(GcCpu *c, uint32_t spr, uint32_t *value) {
  return wii_spr_read((GcMachine *)c, spr, value);
}

/* A system call is a real exception: the program's own handler deals with
 * it. Test programs that want to stop use the runner's hooks instead. */
void gc_bus_syscall(GcCpu *c) { c->pending_vector = GC_VEC_SYSCALL; }
