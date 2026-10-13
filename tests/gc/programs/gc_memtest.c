/*
 * Memory: address translation, byte order, the caches and the two engines
 * that move data without going through a load or a store.
 *
 * dolphin-emu/hwtests is pure CPU - it runs entirely out of registers and
 * says nothing about the machine underneath. This is the other half of the
 * P2 gate (docs/GC-PLAN.md): the parts of the address space that only exist
 * because there is a GameCube around the Gekko.
 *
 * Where the answer is a convention rather than an arithmetic fact - how the
 * locked-cache DMA registers are laid out, what a quantised store does with
 * its scale - the test asks libogc to do it rather than encoding this
 * author's reading of the manual. libogc ran on the hardware for twenty
 * years; a disagreement with it is our bug, not its.
 */

#include <string.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/cast.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "gc_test_report.h"

#define PAGE 4096u

/* Two pages, so that an access can be made to straddle the boundary
 * between them, and 32-byte aligned so a cache line can be addressed. */
static u8 area[2 * PAGE] ATTRIBUTE_ALIGN(PAGE);
static u8 scratch[256] ATTRIBUTE_ALIGN(32);

static volatile u8 *uncached(void *p) { return (volatile u8 *)MEM_K0_TO_K1(p); }

static void fill(u8 *p, u32 size, u32 seed) {
  u32 i;
  for (i = 0; i < size; i++) p[i] = (u8)(seed + i * 7u);
}

/* ------------------------------------------------------------------ */

static void test_boot_state(void) {
  const u32 *globals = (const u32 *)0x80000000u;
  gc_test_begin("the state the apploader leaves behind");

  GC_CHECK_EQ(globals[0x28 / 4], 0x01800000u, "24 MB of main memory at 0x80000028");
  GC_CHECK_EQ(globals[0xf8 / 4], (u32)PPC_BUS_CLOCK, "the bus clock at 0x800000f8");
  GC_CHECK_EQ(globals[0xfc / 4], (u32)PPC_CORE_CLOCK, "the core clock at 0x800000fc");
  /* Paired singles and the quantised load and store forms are on before a
   * game's first instruction. The write-gather pipe is not: libogc leaves
   * HID2[WPE] clear until GX_Init needs it, so the test below turns it on
   * for itself. */
  GC_CHECK_EQ(mfhid2() & 0xa0000000u, 0xa0000000u, "HID2 LSQE and PSE are set");
  GC_CHECK((mfmsr() & (MSR_IR | MSR_DR)) == (MSR_IR | MSR_DR), "translation is on");

  gc_test_end();
}

static void test_byte_order(void) {
  volatile u32 *word = (volatile u32 *)scratch;
  volatile u16 *half = (volatile u16 *)scratch;
  volatile u8 *byte = scratch;

  gc_test_begin("memory holds what the console holds: big-endian");

  *word = 0x01020304u;
  GC_CHECK_EQ(byte[0], 0x01u, "the most significant byte is first");
  GC_CHECK_EQ(byte[1], 0x02u, "byte 1");
  GC_CHECK_EQ(byte[2], 0x03u, "byte 2");
  GC_CHECK_EQ(byte[3], 0x04u, "byte 3");
  GC_CHECK_EQ(half[0], 0x0102u, "the first halfword");
  GC_CHECK_EQ(half[1], 0x0304u, "the second halfword");

  byte[0] = 0xde; byte[1] = 0xad; byte[2] = 0xbe; byte[3] = 0xef;
  GC_CHECK_EQ(*word, 0xdeadbeefu, "four bytes read back as one word");

  /* A doubleword, which the FPU load and store move in one access. */
  {
    volatile u64 *dword = (volatile u64 *)scratch;
    *dword = 0x0102030405060708ull;
    GC_CHECK_EQ(word[0], 0x01020304u, "the high word of a doubleword");
    GC_CHECK_EQ(word[1], 0x05060708u, "the low word of a doubleword");
  }

  gc_test_end();
}

static void test_byte_reversed(void) {
  u32 got32 = 0;
  u16 got16 = 0;
  volatile u32 *word = (volatile u32 *)scratch;

  gc_test_begin("the byte-reversed loads and stores");

  *word = 0x01020304u;
  asm volatile("lwbrx %0, 0, %1" : "=r"(got32) : "b"(scratch) : "memory");
  GC_CHECK_EQ(got32, 0x04030201u, "lwbrx");
  asm volatile("lhbrx %0, 0, %1" : "=r"(got16) : "b"(scratch) : "memory");
  GC_CHECK_EQ(got16, 0x0201u, "lhbrx");

  asm volatile("stwbrx %0, 0, %1" : : "r"(0xaabbccddu), "b"(scratch) : "memory");
  GC_CHECK_EQ(*word, 0xddccbbaau, "stwbrx");
  asm volatile("sthbrx %0, 0, %1" : : "r"(0x1234u), "b"(scratch) : "memory");
  GC_CHECK_EQ(*(volatile u16 *)scratch, 0x3412u, "sthbrx");

  gc_test_end();
}

/* An access at every offset in a window, including across a 32-byte cache
 * line and across the page boundary in the middle of `area`. */
static void test_unaligned(void) {
  u32 offset;
  gc_test_begin("unaligned accesses, across a cache line and across a page");

  for (offset = 0; offset < 8; offset++) {
    u8 *at = area + PAGE - 4 + offset; /* -4 .. +3 around the boundary */
    volatile u32 *word = (volatile u32 *)at;
    volatile u16 *half = (volatile u16 *)at;

    *word = 0x11223344u + offset;
    GC_CHECK_EQ(*word, 0x11223344u + offset, "a word across the page boundary");
    GC_CHECK_EQ(at[0], (u8)(0x11u), "its first byte");
    GC_CHECK_EQ(at[3], (u8)(0x44u + offset), "its last byte");

    *half = (u16)(0xa5a5u ^ offset);
    GC_CHECK_EQ(*half, (u16)(0xa5a5u ^ offset), "a halfword across the page boundary");
  }

  for (offset = 0; offset < 8; offset++) {
    /* The same again at a cache-line boundary well inside one page. */
    u8 *at = area + 256 - 4 + offset;
    volatile u32 *word = (volatile u32 *)at;
    double value = 1.0 + (double)offset;
    volatile double *dword = (volatile double *)at;

    *word = 0x55667788u + offset;
    GC_CHECK_EQ(*word, 0x55667788u + offset, "a word across a cache line");

    *dword = value;
    GC_CHECK(*dword == value, "a double across a cache line");
  }

  gc_test_end();
}

static void test_cached_and_uncached(void) {
  volatile u8 *shadow = uncached(scratch);
  u32 i;

  gc_test_begin("the cached and uncached views are one memory");

  fill(scratch, 64, 0x10u);
  DCFlushRange(scratch, 64);
  for (i = 0; i < 64; i++) {
    GC_CHECK_EQ(shadow[i], (u8)(0x10u + i * 7u), "a flushed write is visible uncached");
  }

  for (i = 0; i < 64; i++) shadow[i] = (u8)(0x90u - i * 3u);
  DCInvalidateRange(scratch, 64);
  for (i = 0; i < 64; i++) {
    GC_CHECK_EQ(scratch[i], (u8)(0x90u - i * 3u), "an uncached write is visible cached");
  }

  GC_CHECK_EQ((u32)MEM_VIRTUAL_TO_PHYSICAL(scratch), (u32)scratch & 0x03ffffffu,
              "the physical address of a cached pointer");

  gc_test_end();
}

static void test_dcbz(void) {
  u32 i;
  gc_test_begin("dcbz zeroes the block it lands in, and nothing else");

  fill(area, 128, 0x01u);
  DCFlushRange(area, 128);

  /* Unaligned: the block is still the aligned 32 bytes containing the
   * address, which is the whole point of the instruction. */
  asm volatile("dcbz 0, %0" : : "b"(area + 40) : "memory");
  DCFlushRange(area, 128);

  for (i = 0; i < 128; i++) {
    const u8 want = (i >= 32 && i < 64) ? 0u : (u8)(0x01u + i * 7u);
    GC_CHECK_EQ(uncached(area)[i], want, "a byte around the zeroed block");
  }

  gc_test_end();
}

/* ------------------------------------------------------------------ */
/* The forms that move more than one register at a time. r24-r31 are
 * clobbered; the compiler is told, and so keeps nothing there. */

static void test_multiple_and_string(void) {
  u32 i;
  gc_test_begin("load and store multiple, and the string forms");

  for (i = 0; i < 8; i++) ((u32 *)scratch)[i] = 0xc0de0000u + i;
  DCFlushRange(scratch, 32);

  asm volatile("lmw 24, 0(%0)\n\t"
               "stmw 24, 64(%0)"
               : : "b"(scratch)
               : "memory", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31");
  for (i = 0; i < 8; i++) {
    GC_CHECK_EQ(((u32 *)scratch)[16 + i], 0xc0de0000u + i, "lmw then stmw moved a word");
  }

  /* stswi and lswi move a byte count, not a register count, and the last
   * register is filled from the most significant end. Nineteen bytes is
   * five registers, the last of them three-quarters used. */
  memset(scratch + 128, 0, 32);
  DCFlushRange(scratch + 128, 32);
  asm volatile("lswi 24, %0, 19\n\t"
               "stswi 24, %1, 19"
               : : "b"(scratch), "b"(scratch + 128)
               : "memory", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31");
  for (i = 0; i < 19; i++) {
    GC_CHECK_EQ(scratch[128 + i], scratch[i], "lswi then stswi moved a byte");
  }
  for (i = 19; i < 32; i++) {
    GC_CHECK_EQ(scratch[128 + i], 0u, "stswi did not write past its count");
  }

  gc_test_end();
}

/* ------------------------------------------------------------------ */
/* Quantised load and store. The scale and the type live in a GQR, and the
 * pair of singles is converted on the way out and back on the way in. */

static void test_quantised(void) {
  static const struct {
    u32 type;
    const char *name;
    u32 bytes;    /* per value */
    s32 low, high; /* the range the type can hold */
  } types[] = {
      {GQR_TYPE_U8, "u8", 1, 0, 255},
      {GQR_TYPE_S8, "s8", 1, -128, 127},
      {GQR_TYPE_U16, "u16", 2, 0, 65535},
      {GQR_TYPE_S16, "s16", 2, -32768, 32767},
  };
  u32 t;

  gc_test_begin("quantised load and store");

  /* GQR0 is the float identity: no conversion, no scale. Everything below
   * loads its pair through it and stores through GQR2. */
  __set_gqr(912, 0);

  /* A float pair goes out as two singles and comes back unchanged. */
  {
    volatile f32 *out = (volatile f32 *)scratch;
    f32 back[2];
    out[0] = 1.5f;
    out[1] = -2.25f;
    CAST_SetGQR2(GQR_TYPE_F32, 0);
    asm volatile("psq_l 1, 0(%0), 0, 0\n\t"
                 "psq_st 1, 32(%0), 0, 2"
                 : : "b"(scratch) : "memory", "fr1");
    memcpy(back, scratch + 32, sizeof(back));
    GC_CHECK(back[0] == 1.5f, "the first lane of a float pair");
    GC_CHECK(back[1] == -2.25f, "the second lane of a float pair");
  }

  for (t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
    /* A value the type holds, one below its floor and one above its
     * ceiling: quantisation saturates rather than wrapping. */
    const f32 inputs[4] = {(f32)(types[t].high / 2), (f32)types[t].low,
                           (f32)types[t].low - 4.0f, (f32)types[t].high + 4.0f};
    u32 i;
    CAST_SetGQR2(types[t].type, 0);
    for (i = 0; i + 1 < 4; i += 2) {
      s32 want[2];
      s32 got[2];
      u32 lane;
      volatile f32 *pair = (volatile f32 *)scratch;

      pair[0] = inputs[i];
      pair[1] = inputs[i + 1];
      memset(scratch + 32, 0xaa, 8);
      asm volatile("psq_l 1, 0(%0), 0, 0\n\t"
                   "psq_st 1, 32(%0), 0, 2"
                   : : "b"(scratch) : "memory", "fr1");

      for (lane = 0; lane < 2; lane++) {
        const u8 *at = scratch + 32 + lane * types[t].bytes;
        s32 value = (s32)inputs[i + lane];
        if (value < types[t].low) value = types[t].low;
        if (value > types[t].high) value = types[t].high;
        want[lane] = value;
        if (types[t].bytes == 1) {
          got[lane] = types[t].low ? (s32)(s8)at[0] : (s32)at[0];
        } else {
          const u16 raw = (u16)((at[0] << 8) | at[1]);
          got[lane] = types[t].low ? (s32)(s16)raw : (s32)raw;
        }
        snprintf(gc_test_line, sizeof(gc_test_line), "psq_st %s of %d: got %d, want %d",
                 types[t].name, (int)inputs[i + lane], (int)got[lane], (int)want[lane]);
        gc_test_check(got[lane] == want[lane], gc_test_line);
      }

      /* And back: a quantised load of what was just stored returns the
       * saturated values as floats. */
      {
        f32 back[2];
        asm volatile("psq_l 1, 32(%0), 0, 2\n\t"
                     "psq_st 1, 48(%0), 0, 0"
                     : : "b"(scratch) : "memory", "fr1");
        memcpy(back, scratch + 48, sizeof(back));
        for (lane = 0; lane < 2; lane++) {
          snprintf(gc_test_line, sizeof(gc_test_line), "psq_l %s lane %u: got %d, want %d",
                   types[t].name, (unsigned)lane, (int)back[lane], (int)want[lane]);
          gc_test_check(back[lane] == (f32)want[lane], gc_test_line);
        }
      }
    }
  }

  gc_test_end();
}

/* ------------------------------------------------------------------ */

static void test_locked_cache(void) {
  u8 *lc = (u8 *)LCGetBase();
  u32 i;

  gc_test_begin("the locked cache and its DMA");

  LCEnable();
  GC_CHECK((mfhid2() & 0x10000000u) != 0u, "HID2 LCE is set once the cache is enabled");

  /* It is storage in its own right, separate from main memory. */
  LCAllocNoInvalidate(lc, 64);
  for (i = 0; i < 64; i++) lc[i] = (u8)(0x40u + i);
  for (i = 0; i < 64; i++) GC_CHECK_EQ(lc[i], (u8)(0x40u + i), "a byte written into the cache");

  /* Out to memory and back in, through the DMA the hardware provides for
   * exactly this - programmed by libogc, not by us. */
  memset(scratch, 0, 64);
  DCFlushRange(scratch, 64);
  LCStoreData(scratch, lc, 64);
  LCQueueWait(0);
  for (i = 0; i < 64; i++) {
    GC_CHECK_EQ(uncached(scratch)[i], (u8)(0x40u + i), "a byte the DMA stored to memory");
  }

  for (i = 0; i < 64; i++) uncached(scratch)[i] = (u8)(0xf0u - i);
  memset(lc, 0, 64);
  LCLoadData(lc, scratch, 64);
  LCQueueWait(0);
  for (i = 0; i < 64; i++) {
    GC_CHECK_EQ(lc[i], (u8)(0xf0u - i), "a byte the DMA loaded into the cache");
  }

  LCDisable();
  gc_test_end();
}

static void test_write_gather(void) {
  static const u32 pattern[8] = {0x00112233u, 0x44556677u, 0x8899aabbu, 0xccddeeffu,
                                 0x0f0f0f0fu, 0xf0f0f0f0u, 0x12345678u, 0x9abcdef0u};
  volatile u32 *pipe = (volatile u32 *)0xcc008000u;
  u32 i;

  gc_test_begin("the write-gather pipe");

  memset(scratch, 0, 64);
  DCFlushRange(scratch, 64);

  /* The pipe gathers only while HID2[WPE] is set, and writes to the
   * physical address in WPAR, 32 bytes at a time; writing WPAR drops
   * anything half-gathered. */
  mthid2(mfhid2() | 0x40000000u);
  ppcsync();
  mtwpar(MEM_VIRTUAL_TO_PHYSICAL(scratch));
  ppcsync();
  for (i = 0; i < 8; i++) pipe[i] = pattern[i];
  ppcsync();

  for (i = 0; i < 8; i++) {
    const volatile u32 *got = (const volatile u32 *)uncached(scratch);
    GC_CHECK_EQ(got[i], pattern[i], "a word the pipe gathered and wrote");
  }
  /* Nothing beyond the 32 bytes it gathered. */
  for (i = 32; i < 64; i++) GC_CHECK_EQ(uncached(scratch)[i], 0u, "a byte past the block");

  mthid2(mfhid2() & ~0x40000000u);
  ppcsync();
  gc_test_end();
}

static void test_reservations(void) {
  volatile u32 *word = (volatile u32 *)scratch;
  u32 read = 0;
  u32 stored;

  gc_test_begin("lwarx and stwcx.");

  *word = 0x5a5a5a5au;
  DCFlushRange(scratch, 32);

  asm volatile("lwarx %0, 0, %1" : "=r"(read) : "b"(scratch) : "memory");
  GC_CHECK_EQ(read, 0x5a5a5a5au, "lwarx read the word");

  asm volatile("stwcx. %1, 0, %2\n\t"
               "mfcr %0"
               : "=r"(stored)
               : "r"(0xa5a5a5a5u), "b"(scratch)
               : "memory", "cr0");
  GC_CHECK((stored & 0x20000000u) != 0u, "the store succeeded while the reservation held");
  GC_CHECK_EQ(*word, 0xa5a5a5a5u, "and the word changed");

  /* The reservation is gone now: a second store with no lwarx in front of
   * it must fail and leave memory alone. */
  asm volatile("stwcx. %1, 0, %2\n\t"
               "mfcr %0"
               : "=r"(stored)
               : "r"(0xdeadbeefu), "b"(scratch)
               : "memory", "cr0");
  GC_CHECK((stored & 0x20000000u) == 0u, "the store failed with no reservation");
  GC_CHECK_EQ(*word, 0xa5a5a5a5u, "and the word did not change");

  /* A store from anywhere into the block clears it. */
  asm volatile("lwarx %0, 0, %1" : "=r"(read) : "b"(scratch) : "memory");
  word[4] = 0x11111111u;
  asm volatile("stwcx. %1, 0, %2\n\t"
               "mfcr %0"
               : "=r"(stored)
               : "r"(0xcafef00du), "b"(scratch)
               : "memory", "cr0");
  GC_CHECK((stored & 0x20000000u) == 0u, "a store into the block cleared the reservation");

  gc_test_end();
}

/* ------------------------------------------------------------------ */

int main(void) {
  VIDEO_Init();

  test_boot_state();
  test_byte_order();
  test_byte_reversed();
  test_unaligned();
  test_cached_and_uncached();
  test_dcbz();
  test_multiple_and_string();
  test_quantised();
  test_locked_cache();
  test_write_gather();
  test_reservations();

  gc_test_finish();
  return 0;
}
