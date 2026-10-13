/*
 * Gekko interpreter unit tests.
 *
 * These are layer-one evidence (AGENTS.md): they pin the behaviour this
 * implementation intends, taken from the IBM Gekko User's Manual. They are
 * not hardware evidence. That comes from hwtests/cputest and from the
 * vectors captured off an oracle in P1; where the manual leaves something
 * undefined, the case is marked here and listed in docs/GC-PLAN.md under
 * "Behaviour pinned by the vectors" so the oracle run settles it.
 *
 * Instructions are built from the generated metadata rather than from
 * hand-written opcode numbers: gc_op_info[op].pattern already holds every
 * fixed bit, so a test cannot encode an instruction the decoder disagrees
 * with.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gc_bus.h"
#include "gc_cpu.h"
#include "gc_cpu_ops.h"
#include "gc_test_bus.h"

static int failures;
static const char *current;

#define CHECK(condition, ...)                     \
  do {                                            \
    if (!(condition)) {                           \
      printf("FAIL [%s] %s:%d: ", current, __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

#define CHECK_U32(got, want, what)                                        \
  do {                                                                    \
    const uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);           \
    CHECK(g_ == w_, "%s = 0x%08x, expected 0x%08x", what, g_, w_);        \
  } while (0)

#define CHECK_U64(got, want, what)                                        \
  do {                                                                    \
    const uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want);           \
    CHECK(g_ == w_, "%s = 0x%016llx, expected 0x%016llx", what,           \
          (unsigned long long)g_, (unsigned long long)w_);                \
  } while (0)

/* ------------------------------------------------------------------ */
/* Encoding helpers                                                    */

static uint32_t enc(GcOp op) { return gc_op_info[op].pattern; }

static uint32_t enc_d(GcOp op, unsigned d, unsigned a, uint32_t imm) {
  return enc(op) | (d << 21) | (a << 16) | (imm & 0xffffu);
}

static uint32_t enc_x(GcOp op, unsigned d, unsigned a, unsigned b) {
  return enc(op) | (d << 21) | (a << 16) | (b << 11);
}

static uint32_t enc_a(GcOp op, unsigned d, unsigned a, unsigned b, unsigned cc) {
  return enc(op) | (d << 21) | (a << 16) | (b << 11) | (cc << 6);
}

static uint32_t enc_m(GcOp op, unsigned s, unsigned a, unsigned sh, unsigned mb, unsigned me) {
  return enc(op) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1);
}

static uint32_t enc_psq(GcOp op, unsigned frd, unsigned ra, unsigned w, unsigned i,
                        int32_t offset) {
  return enc(op) | (frd << 21) | (ra << 16) | (w << 15) | (i << 12) |
         ((uint32_t)offset & 0xfffu);
}

static uint32_t with_rc(uint32_t insn) { return insn | 1u; }
static uint32_t with_oe(uint32_t insn) { return insn | (1u << 10); }

/* ------------------------------------------------------------------ */
/* Running                                                             */

#define CODE_BASE (GC_TEST_BASE + 0x1000u)
#define DATA_BASE (GC_TEST_BASE + 0x2000u)

static GcCpu cpu;

static void start(const char *name) {
  current = name;
  gc_test_reset_memory();
  gc_cpu_reset(&cpu);
  cpu.msr |= GC_MSR_FP;
  cpu.hid2 = GC_HID2_PSE | GC_HID2_LSQE;
  cpu.pc = CODE_BASE;
  cpu.npc = CODE_BASE + 4;
}

/* Execute one instruction at the code base and return the cycles charged. */
static void step(uint32_t insn) {
  gc_test_write32(CODE_BASE, insn);
  cpu.pc = CODE_BASE;
  gc_cpu_step(&cpu);
}

static void run(const uint32_t *words, unsigned count, unsigned max_steps) {
  unsigned i;
  gc_test_load_code(&cpu, CODE_BASE, words, count);
  for (i = 0; i < max_steps && !cpu.stopped; i++) gc_cpu_step(&cpu);
}

static uint64_t bits_of(double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static double value_of(uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/* ------------------------------------------------------------------ */

static void test_endianness(void) {
  start("endianness");
  cpu.gpr[3] = 0x11223344u;
  cpu.gpr[4] = DATA_BASE;
  step(enc_d(GC_OP_STW, 3, 4, 0));
  /* A word written by the guest is big-endian in guest memory: the game
   * reading it back a byte at a time must see 11 22 33 44. */
  CHECK_U32(gc_test_memory[DATA_BASE - GC_TEST_BASE + 0], 0x11, "byte 0");
  CHECK_U32(gc_test_memory[DATA_BASE - GC_TEST_BASE + 3], 0x44, "byte 3");
  step(enc_d(GC_OP_LBZ, 5, 4, 0));
  CHECK_U32(cpu.gpr[5], 0x11, "lbz of the high byte");
  step(enc_d(GC_OP_LHZ, 5, 4, 2));
  CHECK_U32(cpu.gpr[5], 0x3344, "lhz of the low half");
  /* And the byte-reversing forms read the same memory the other way. */
  step(enc_x(GC_OP_LWBRX, 5, 4, 0));
  CHECK_U32(cpu.gpr[5], 0x44332211u, "lwbrx");
}

static void test_arithmetic(void) {
  start("arithmetic");
  cpu.gpr[3] = 0xfffffffeu;
  cpu.gpr[4] = 3;
  step(enc_x(GC_OP_ADDC, 5, 3, 4));
  CHECK_U32(cpu.gpr[5], 1, "addc result");
  CHECK(cpu.xer & GC_XER_CA, "addc set the carry");
  /* adde folds the carry in. */
  cpu.gpr[3] = 0;
  cpu.gpr[4] = 0;
  step(enc_x(GC_OP_ADDE, 5, 3, 4));
  CHECK_U32(cpu.gpr[5], 1, "adde adds the carry");
  CHECK(!(cpu.xer & GC_XER_CA), "adde cleared the carry");

  /* subf rD,rA,rB is rB - rA, not rA - rB. */
  cpu.gpr[3] = 10;
  cpu.gpr[4] = 4;
  step(enc_x(GC_OP_SUBF, 5, 3, 4));
  CHECK_U32(cpu.gpr[5], (uint32_t)-6, "subf is rB - rA");

  /* Overflow is sticky in SO and only OE forms write OV. */
  cpu.xer = 0;
  cpu.gpr[3] = 0x7fffffffu;
  cpu.gpr[4] = 1;
  step(with_oe(enc_x(GC_OP_ADD, 5, 3, 4)));
  CHECK(cpu.xer & GC_XER_OV, "add. with OE set OV");
  CHECK(cpu.xer & GC_XER_SO, "and SO");
  cpu.gpr[4] = 0;
  step(with_oe(enc_x(GC_OP_ADD, 5, 3, 4)));
  CHECK(!(cpu.xer & GC_XER_OV), "OV cleared when there is no overflow");
  CHECK(cpu.xer & GC_XER_SO, "SO stays set");

  /* Rc writes CR0 from the result and copies SO in. */
  cpu.gpr[3] = 0xffffffffu;
  cpu.gpr[4] = 0;
  step(with_rc(enc_x(GC_OP_ADD, 5, 3, 4)));
  CHECK_U32(cpu.cr >> 28, 0x8 | 0x1, "CR0 is LT with SO");

  /* Division: by zero and the most negative value over -1 are
   * architecturally undefined. What this core leaves behind is pinned in
   * docs/GC-PLAN.md and has to be confirmed against an oracle. */
  cpu.gpr[3] = 100;
  cpu.gpr[4] = 7;
  step(enc_x(GC_OP_DIVW, 5, 3, 4));
  CHECK_U32(cpu.gpr[5], 14, "divw");
  cpu.gpr[3] = (uint32_t)-100;
  step(enc_x(GC_OP_DIVW, 5, 3, 4));
  CHECK_U32(cpu.gpr[5], (uint32_t)-14, "divw truncates toward zero");
  cpu.gpr[4] = 0;
  cpu.xer = 0;
  step(with_oe(enc_x(GC_OP_DIVW, 5, 3, 4)));
  CHECK(cpu.xer & GC_XER_OV, "divw by zero sets OV");

  /* mulhw takes the high half of the signed product. */
  cpu.gpr[3] = 0x40000000u;
  cpu.gpr[4] = 4;
  step(enc_x(GC_OP_MULHW, 5, 3, 4));
  CHECK_U32(cpu.gpr[5], 1, "mulhw");
  step(enc_x(GC_OP_MULLW, 5, 3, 4));
  CHECK_U32(cpu.gpr[5], 0, "mullw keeps the low half");
}

static void test_compare(void) {
  start("compare");
  cpu.gpr[3] = 0xffffffffu; /* -1 signed, the largest unsigned */
  cpu.gpr[4] = 1;
  step(enc_x(GC_OP_CMP, 0, 3, 4));
  CHECK_U32((cpu.cr >> 28) & 0xe, 0x8, "cmp says -1 < 1");
  step(enc_x(GC_OP_CMPL, 0, 3, 4));
  CHECK_U32((cpu.cr >> 28) & 0xe, 0x4, "cmpl says 0xffffffff > 1");
  /* The compare field selector is three bits starting at bit 6, so crfD 7
   * lands in the bottom nibble of CR. */
  step(enc_x(GC_OP_CMP, 7 << 2, 3, 4));
  CHECK_U32(cpu.cr & 0xe, 0x8, "cmp into CR7");
}

static void test_rotate_and_shift(void) {
  start("rotate and shift");
  cpu.gpr[3] = 0xffffffffu;
  /* A mask with MB > ME wraps: it selects the two ends, not the middle.
   * This is the encoding a compiler uses to clear a middle bitfield. */
  step(enc_m(GC_OP_RLWINM, 3, 4, 0, 24, 7));
  CHECK_U32(cpu.gpr[4], 0xff0000ffu, "rlwinm with a wrapped mask");
  step(enc_m(GC_OP_RLWINM, 3, 4, 0, 8, 23));
  CHECK_U32(cpu.gpr[4], 0x00ffff00u, "rlwinm with a normal mask");

  cpu.gpr[3] = 0x12345678u;
  step(enc_m(GC_OP_RLWINM, 3, 4, 8, 0, 31));
  CHECK_U32(cpu.gpr[4], 0x34567812u, "rlwinm rotates left");

  /* rlwimi merges into the destination. */
  cpu.gpr[3] = 0x000000ffu;
  cpu.gpr[4] = 0xaaaaaaaau;
  step(enc_m(GC_OP_RLWIMI, 3, 4, 8, 16, 23));
  CHECK_U32(cpu.gpr[4], 0xaaaaffaau, "rlwimi keeps the bits outside the mask");

  /* Shifts take six bits of the count, so 32 or more clears the register
   * instead of being undefined as the C shift would be. */
  cpu.gpr[3] = 0xffffffffu;
  cpu.gpr[5] = 32;
  step(enc_x(GC_OP_SLW, 3, 4, 5));
  CHECK_U32(cpu.gpr[4], 0, "slw by 32");
  step(enc_x(GC_OP_SRW, 3, 4, 5));
  CHECK_U32(cpu.gpr[4], 0, "srw by 32");

  /* srawi sets the carry only when a one bit is shifted out of a negative
   * value (hwtests/cputest/srawix.cpp). */
  cpu.gpr[3] = 0xfffffffeu; /* -2: bit shifted out is 0 */
  cpu.xer = 0;
  step(enc(GC_OP_SRAWI) | (3u << 21) | (4u << 16) | (1u << 11));
  CHECK_U32(cpu.gpr[4], 0xffffffffu, "srawi of -2 by 1");
  CHECK(!(cpu.xer & GC_XER_CA), "srawi with nothing lost clears CA");
  cpu.gpr[3] = 0xffffffffu; /* -1: a one bit is shifted out */
  step(enc(GC_OP_SRAWI) | (3u << 21) | (4u << 16) | (1u << 11));
  CHECK(cpu.xer & GC_XER_CA, "srawi of -1 by 1 sets CA");
  /* A positive value never sets CA. */
  cpu.gpr[3] = 0x7fffffffu;
  step(enc(GC_OP_SRAWI) | (3u << 21) | (4u << 16) | (4u << 11));
  CHECK(!(cpu.xer & GC_XER_CA), "srawi of a positive value clears CA");

  cpu.gpr[3] = 0x00008000u;
  step(enc_x(GC_OP_CNTLZW, 3, 4, 0));
  CHECK_U32(cpu.gpr[4], 16, "cntlzw");
  cpu.gpr[3] = 0;
  step(enc_x(GC_OP_CNTLZW, 3, 4, 0));
  CHECK_U32(cpu.gpr[4], 32, "cntlzw of zero");
}

static void test_condition_register(void) {
  start("condition register");
  cpu.cr = 0x12345678u;
  step(enc_x(GC_OP_MFCR, 3, 0, 0));
  CHECK_U32(cpu.gpr[3], 0x12345678u, "mfcr");

  /* mtcrf writes only the fields its mask names. */
  cpu.gpr[3] = 0xffffffffu;
  cpu.cr = 0;
  step(enc(GC_OP_MTCRF) | (3u << 21) | (0x81u << 12));
  CHECK_U32(cpu.cr, 0xf000000fu, "mtcrf wrote CR0 and CR7 only");

  /* mcrxr moves XER's summary bits and clears them. */
  cpu.xer = GC_XER_SO | GC_XER_CA | 0x7f;
  cpu.cr = 0;
  step(enc(GC_OP_MCRXR) | (1u << 23));
  CHECK_U32((cpu.cr >> 24) & 0xf, 0xa, "mcrxr wrote SO and CA into CR1");
  CHECK_U32(cpu.xer, 0x7f, "mcrxr cleared SO, OV and CA but not the byte count");
}

static void test_branches(void) {
  start("branches");
  /* bl links the return address even though this one is taken. */
  step(enc(GC_OP_B) | 0x100u | 1u);
  CHECK_U32(cpu.pc, CODE_BASE + 0x100u, "b to a relative target");
  CHECK_U32(cpu.lr, CODE_BASE + 4u, "bl set LR");

  /* A not-taken conditional branch with LK still links: this is how
   * position-independent code finds out where it is. */
  start("branches: bcl");
  cpu.ctr = 1;
  /* bdnz+: BO = 16 (decrement CTR, branch if not zero), with LK. */
  step(enc(GC_OP_BC) | (16u << 21) | 0x20u | 1u);
  CHECK_U32(cpu.ctr, 0, "bdnz decremented CTR");
  CHECK_U32(cpu.pc, CODE_BASE + 4u, "bdnz fell through at zero");
  CHECK_U32(cpu.lr, CODE_BASE + 4u, "a not-taken bcl still links");

  /* A counted loop, run to completion. */
  start("branches: loop");
  {
    const uint32_t code[] = {
        enc_d(GC_OP_ADDI, 3, 0, 0),                    /* li r3, 0 */
        enc_d(GC_OP_ADDI, 3, 3, 1),                    /* addi r3, r3, 1 */
        enc(GC_OP_BC) | (16u << 21) | (0xfffcu & 0xfffcu), /* bdnz -4 */
        enc(GC_OP_SC),
    };
    cpu.ctr = 5;
    run(code, 4, 64);
    CHECK_U32(cpu.gpr[3], 5, "the loop ran CTR times");
    CHECK_U32(cpu.ctr, 0, "CTR reached zero");
    CHECK(cpu.stopped == GC_STOP_HALT, "the program stopped at sc");
  }

  /* blr goes to LR with its low two bits ignored. */
  start("branches: blr");
  cpu.lr = CODE_BASE + 0x42u;
  step(enc(GC_OP_BCLR) | (20u << 21));
  CHECK_U32(cpu.pc, CODE_BASE + 0x40u, "blr ignores the low two bits of LR");

  /* bctr does not decrement CTR. */
  start("branches: bctr");
  cpu.ctr = CODE_BASE + 0x80u;
  step(enc(GC_OP_BCCTR) | (20u << 21));
  CHECK_U32(cpu.pc, CODE_BASE + 0x80u, "bctr jumped to CTR");
  CHECK_U32(cpu.ctr, CODE_BASE + 0x80u, "bctr left CTR alone");
}

static void test_loads_and_stores(void) {
  start("loads and stores");
  cpu.gpr[4] = DATA_BASE;
  cpu.gpr[3] = 0x0000fedcu;
  step(enc_d(GC_OP_STH, 3, 4, 0));
  step(enc_d(GC_OP_LHA, 5, 4, 0));
  CHECK_U32(cpu.gpr[5], 0xfffffedcu, "lha sign-extends");
  step(enc_d(GC_OP_LHZ, 5, 4, 0));
  CHECK_U32(cpu.gpr[5], 0x0000fedcu, "lhz does not");

  /* The update forms write the effective address back to rA. */
  cpu.gpr[4] = DATA_BASE;
  step(enc_d(GC_OP_LWZU, 5, 4, 8));
  CHECK_U32(cpu.gpr[4], DATA_BASE + 8u, "lwzu updated rA");

  /* A faulting access updates nothing. */
  start("loads and stores: fault");
  cpu.gpr[4] = 0x10000000u; /* outside the test memory */
  cpu.gpr[5] = 0xccccccccu;
  step(enc_d(GC_OP_LWZU, 5, 4, 0));
  CHECK_U32(cpu.gpr[5], 0xccccccccu, "a faulting load leaves rD alone");
  CHECK_U32(cpu.gpr[4], 0x10000000u, "and leaves rA alone");
  CHECK(gc_test_faults == 1, "the bus reported one fault");

  /* Load and store multiple move rD..r31 upward. */
  start("loads and stores: multiple");
  {
    unsigned i;
    for (i = 28; i < 32; i++) cpu.gpr[i] = 0x1000u + i;
    cpu.gpr[4] = DATA_BASE;
    step(enc_d(GC_OP_STMW, 28, 4, 0));
    for (i = 28; i < 32; i++) {
      CHECK_U32(gc_test_read32(DATA_BASE + 4 * (i - 28)), 0x1000u + i, "stmw word");
      cpu.gpr[i] = 0;
    }
    step(enc_d(GC_OP_LMW, 28, 4, 0));
    for (i = 28; i < 32; i++) CHECK_U32(cpu.gpr[i], 0x1000u + i, "lmw word");
  }

  /* The string forms pack four bytes to a register and wrap past r31. */
  start("loads and stores: string");
  {
    cpu.gpr[4] = DATA_BASE;
    gc_test_write32(DATA_BASE, 0xaabbccddu);
    gc_test_write32(DATA_BASE + 4, 0xeeff0011u);
    cpu.gpr[31] = 0;
    cpu.gpr[0] = 0;
    step(enc(GC_OP_LSWI) | (31u << 21) | (4u << 16) | (6u << 11));
    CHECK_U32(cpu.gpr[31], 0xaabbccddu, "lswi filled r31");
    CHECK_U32(cpu.gpr[0], 0xeeff0000u, "lswi wrapped into r0 and zeroed the rest");

    /* A count that is a whole number of registers stops at the last one it
     * fills. Eight bytes into r31 are r31 and r0; r1 is not part of it. A
     * count field of zero means thirty-two bytes - eight registers - and
     * from r25 that ends at r0 with r1, the stack pointer, untouched. */
    cpu.gpr[1] = 0x5a5a5a5au;
    step(enc(GC_OP_LSWI) | (31u << 21) | (4u << 16) | (8u << 11));
    CHECK_U32(cpu.gpr[0], 0xeeff0011u, "lswi of eight bytes filled r0");
    CHECK_U32(cpu.gpr[1], 0x5a5a5a5au, "and left the register after it alone");
    {
      unsigned i;
      for (i = 0; i < 8u; i++) gc_test_write32(DATA_BASE + 4u * i, 0x01010101u * (i + 1u));
    }
    cpu.gpr[4] = DATA_BASE;
    step(enc(GC_OP_LSWI) | (25u << 21) | (4u << 16) | (0u << 11));
    CHECK_U32(cpu.gpr[25], 0x01010101u, "lswi of thirty-two bytes starts at r25");
    CHECK_U32(cpu.gpr[0], 0x08080808u, "ends at r0");
    CHECK_U32(cpu.gpr[1], 0x5a5a5a5au, "and leaves r1 alone");
  }

  /* dcbz clears the whole 32-byte line the address falls in. */
  start("loads and stores: dcbz");
  {
    unsigned i;
    for (i = 0; i < 64; i++) gc_test_memory[DATA_BASE - GC_TEST_BASE + i] = 0xff;
    cpu.gpr[4] = DATA_BASE + 4u; /* not line-aligned */
    step(enc_x(GC_OP_DCBZ, 0, 4, 0));
    for (i = 0; i < 32; i++) {
      CHECK_U32(gc_test_memory[DATA_BASE - GC_TEST_BASE + i], 0, "dcbz cleared the line");
    }
    CHECK_U32(gc_test_memory[DATA_BASE - GC_TEST_BASE + 32], 0xff, "and only that line");
  }
}

static void test_reservation(void) {
  start("lwarx and stwcx.");
  cpu.gpr[4] = DATA_BASE;
  cpu.gpr[3] = 0x55555555u;
  gc_test_write32(DATA_BASE, 0x11111111u);
  step(enc_x(GC_OP_LWARX, 5, 4, 0));
  CHECK_U32(cpu.gpr[5], 0x11111111u, "lwarx read the word");
  step(with_rc(enc_x(GC_OP_STWCX_, 3, 4, 0)));
  CHECK_U32(gc_test_read32(DATA_BASE), 0x55555555u, "stwcx. stored");
  CHECK_U32((cpu.cr >> 28) & 0x2, 0x2, "stwcx. set EQ on success");

  /* Without a reservation the store does not happen. */
  cpu.gpr[3] = 0x99999999u;
  step(with_rc(enc_x(GC_OP_STWCX_, 3, 4, 0)));
  CHECK_U32(gc_test_read32(DATA_BASE), 0x55555555u, "a second stwcx. did nothing");
  CHECK_U32((cpu.cr >> 28) & 0x2, 0, "and cleared EQ");
}

static void test_sprs_and_time(void) {
  start("SPRs and the timebase");
  cpu.gpr[3] = 0x1234u;
  step(enc(GC_OP_MTSPR) | (3u << 21) | ((GC_SPR_CTR & 0x1fu) << 16) |
       (((GC_SPR_CTR >> 5) & 0x1fu) << 11));
  CHECK_U32(cpu.ctr, 0x1234u, "mtctr");
  step(enc(GC_OP_MFSPR) | (5u << 21) | ((GC_SPR_CTR & 0x1fu) << 16) |
       (((GC_SPR_CTR >> 5) & 0x1fu) << 11));
  CHECK_U32(cpu.gpr[5], 0x1234u, "mfctr");

  /* A GQR round-trips through mtspr/mfspr, which is how a game sets up a
   * quantised load before using it. */
  cpu.gpr[3] = 0x00040004u;
  step(enc(GC_OP_MTSPR) | (3u << 21) | ((GC_SPR_GQR0 & 0x1fu) << 16) |
       (((GC_SPR_GQR0 >> 5) & 0x1fu) << 11));
  CHECK_U32(cpu.gqr[0], 0x00040004u, "mtspr GQR0");

  /* The timebase advances with the cycle counter at one tick per twelve
   * CPU clocks and mftb never goes backwards. */
  {
    uint64_t before = gc_cpu_timebase(&cpu);
    unsigned i;
    for (i = 0; i < 64; i++) step(enc_d(GC_OP_ADDI, 3, 0, 0));
    CHECK(gc_cpu_timebase(&cpu) >= before, "the timebase did not go backwards");
    CHECK(gc_cpu_timebase(&cpu) > before, "the timebase advanced");
    before = gc_cpu_timebase(&cpu);
    step(enc(GC_OP_MFTB) | (5u << 21) | ((268u & 0x1fu) << 16) | (((268u >> 5) & 0x1fu) << 11));
    CHECK_U32(cpu.gpr[5], (uint32_t)before, "mftb read the timebase");
  }
}

static void test_float_scalar(void) {
  start("scalar floating point");
  cpu.gpr[4] = DATA_BASE;

  /* lfs loads a single into both lanes; stfs rounds back to single. */
  gc_test_write32(DATA_BASE, 0x3fc00000u); /* 1.5f */
  step(enc_d(GC_OP_LFS, 1, 4, 0));
  CHECK(value_of(cpu.fpr[1].ps0) == 1.5, "lfs loaded 1.5 into ps0");
  CHECK_U64(cpu.fpr[1].ps1, cpu.fpr[1].ps0, "lfs filled ps1 as well");

  /* stfs does not round: the conversion is defined bit by bit and drops
   * the significand's tail, so 0.1 goes out as 3dcccccc where a host cast
   * would give 3dcccccd. The vectors pin this. */
  cpu.fpr[2].ps0 = bits_of(0.1);
  step(enc_d(GC_OP_STFS, 2, 4, 8));
  CHECK_U32(gc_test_read32(DATA_BASE + 8), 0x3dccccccu, "stfs truncates 0.1 to single");

  /* stfd keeps all 64 bits, so a double survives a round trip. */
  step(enc_d(GC_OP_STFD, 2, 4, 16));
  step(enc_d(GC_OP_LFD, 3, 4, 16));
  CHECK_U64(cpu.fpr[3].ps0, bits_of(0.1), "lfd/stfd round trip");

  /* Multiply-add is fused: one rounding, not two. With these operands an
   * unfused implementation gives exactly 0.0 and a fused one does not. */
  cpu.fpr[1].ps0 = bits_of(1.0 + pow(2.0, -52));
  cpu.fpr[2].ps0 = bits_of(1.0 - pow(2.0, -52));
  cpu.fpr[3].ps0 = bits_of(-1.0);
  step(enc_a(GC_OP_FMADD, 4, 1, 3, 2));
  CHECK(value_of(cpu.fpr[4].ps0) != 0.0, "fmadd kept the product's extra bits");
  CHECK(value_of(cpu.fpr[4].ps0) == -pow(2.0, -104), "fmadd result");

  /* fsel selects on "greater than or equal to zero", and -0.0 qualifies. */
  cpu.fpr[1].ps0 = bits_of(-0.0);
  cpu.fpr[2].ps0 = bits_of(11.0); /* frB, chosen when frA < 0 */
  cpu.fpr[3].ps0 = bits_of(22.0); /* frC, chosen when frA >= 0 */
  step(enc_a(GC_OP_FSEL, 4, 1, 2, 3));
  CHECK(value_of(cpu.fpr[4].ps0) == 22.0, "fsel treats -0.0 as >= 0");
  cpu.fpr[1].ps0 = bits_of(-1.0);
  step(enc_a(GC_OP_FSEL, 4, 1, 2, 3));
  CHECK(value_of(cpu.fpr[4].ps0) == 11.0, "fsel takes frB when frA < 0");

  /* fctiwz truncates toward zero and leaves 0xfff80000 in the high word. */
  cpu.fpr[1].ps0 = bits_of(-2.75);
  step(enc_x(GC_OP_FCTIWZ, 4, 0, 1));
  CHECK_U64(cpu.fpr[4].ps0, 0xfff8000000000000ull | 0xfffffffeu, "fctiwz of -2.75");
  cpu.fpr[1].ps0 = bits_of(1e30);
  step(enc_x(GC_OP_FCTIWZ, 4, 0, 1));
  CHECK_U64(cpu.fpr[4].ps0, 0xfff8000000000000ull | 0x7fffffffu, "fctiwz saturates");
  CHECK(cpu.fpscr & GC_FPSCR_VXCVI, "fctiwz out of range set VXCVI");
  CHECK(cpu.fpscr & GC_FPSCR_VX, "and the invalid-operation summary");

  /* FPRF reports the class and sign of every result. */
  start("scalar floating point: FPRF");
  cpu.fpr[1].ps0 = bits_of(-1.0);
  cpu.fpr[2].ps0 = bits_of(0.0);
  step(enc_a(GC_OP_FADD, 4, 1, 2, 0));
  CHECK_U32((cpu.fpscr & GC_FPSCR_FPRF) >> 12, 0x08, "FPRF says negative normal");
  cpu.fpr[1].ps0 = bits_of(0.0);
  step(enc_a(GC_OP_FADD, 4, 1, 2, 0));
  CHECK_U32((cpu.fpscr & GC_FPSCR_FPRF) >> 12, 0x02, "FPRF says positive zero");

  /* A signalling NaN comes back quieted, and it is the operand order that
   * decides which NaN survives. */
  start("scalar floating point: NaN");
  cpu.fpr[1].ps0 = 0x7ff4000000000000ull; /* SNaN */
  cpu.fpr[2].ps0 = 0x7ffc000000000000ull; /* QNaN */
  step(enc_a(GC_OP_FADD, 4, 1, 2, 0));
  CHECK_U64(cpu.fpr[4].ps0, 0x7ffc000000000000ull, "the SNaN in frA is quieted and returned");
  CHECK(cpu.fpscr & GC_FPSCR_VXSNAN, "VXSNAN was raised");
  cpu.fpr[1].ps0 = bits_of(1.0);
  cpu.fpr[2].ps0 = 0x7ff8123400000000ull;
  step(enc_a(GC_OP_FADD, 4, 1, 2, 0));
  CHECK_U64(cpu.fpr[4].ps0, 0x7ff8123400000000ull, "a QNaN passes through with its payload");

  /* Infinity minus infinity is invalid; the default QNaN comes out. */
  cpu.fpr[1].ps0 = 0x7ff0000000000000ull;
  cpu.fpr[2].ps0 = 0xfff0000000000000ull;
  step(enc_a(GC_OP_FADD, 4, 1, 2, 0));
  CHECK(cpu.fpscr & GC_FPSCR_VXISI, "inf + -inf set VXISI");
  CHECK_U64(cpu.fpr[4].ps0, 0x7ff8000000000000ull, "and gave the default QNaN");
}

static void test_paired_singles(void) {
  start("paired singles");
  cpu.fpr[1].ps0 = bits_of(1.0);
  cpu.fpr[1].ps1 = bits_of(2.0);
  cpu.fpr[2].ps0 = bits_of(10.0);
  cpu.fpr[2].ps1 = bits_of(20.0);
  step(enc_a(GC_OP_PS_ADD, 3, 1, 2, 0));
  CHECK(value_of(cpu.fpr[3].ps0) == 11.0, "ps_add lane 0");
  CHECK(value_of(cpu.fpr[3].ps1) == 22.0, "ps_add lane 1");

  /* The merges are the lane shuffles. */
  step(enc_a(GC_OP_PS_MERGE10, 3, 1, 2, 0));
  CHECK(value_of(cpu.fpr[3].ps0) == 2.0, "ps_merge10 takes frA's ps1");
  CHECK(value_of(cpu.fpr[3].ps1) == 10.0, "and frB's ps0");

  /* ps_muls0 scales both lanes by one lane of frC. */
  cpu.fpr[4].ps0 = bits_of(3.0);
  cpu.fpr[4].ps1 = bits_of(100.0);
  step(enc_a(GC_OP_PS_MULS0, 3, 1, 0, 4));
  CHECK(value_of(cpu.fpr[3].ps0) == 3.0, "ps_muls0 lane 0");
  CHECK(value_of(cpu.fpr[3].ps1) == 6.0, "ps_muls0 scaled lane 1 by the same value");

  /* ps_sum0 crosses the lanes: ps0 = frA(ps0) + frB(ps1), and frC's ps1
   * passes through. This is the last step of a dot product. */
  step(enc_a(GC_OP_PS_SUM0, 3, 1, 2, 4));
  CHECK(value_of(cpu.fpr[3].ps0) == 21.0, "ps_sum0 summed across the lanes");
  CHECK(value_of(cpu.fpr[3].ps1) == 100.0, "ps_sum0 passed frC's ps1 through");

  /* The sign-manipulating forms touch both lanes and nothing else. */
  step(enc_x(GC_OP_PS_NEG, 3, 0, 1));
  CHECK(value_of(cpu.fpr[3].ps0) == -1.0 && value_of(cpu.fpr[3].ps1) == -2.0, "ps_neg");
}

static void test_quantised(void) {
  start("quantised load and store");
  cpu.gpr[4] = DATA_BASE;

  /* Type 7 (signed 16-bit), scale 1: stored as value * 2, loaded back as
   * value / 2. */
  cpu.gqr[0] = (1u << 24) | (7u << 16) | (1u << 8) | 7u;
  cpu.fpr[1].ps0 = bits_of(3.0);
  cpu.fpr[1].ps1 = bits_of(-4.0);
  step(enc_psq(GC_OP_PSQ_ST, 1, 4, 0, 0, 0));
  CHECK_U32(gc_test_read32(DATA_BASE) >> 16, 6, "psq_st scaled lane 0");
  CHECK_U32(gc_test_read32(DATA_BASE) & 0xffff, 0xfff8u, "psq_st scaled lane 1");
  step(enc_psq(GC_OP_PSQ_L, 2, 4, 0, 0, 0));
  CHECK(value_of(cpu.fpr[2].ps0) == 3.0, "psq_l unscaled lane 0");
  CHECK(value_of(cpu.fpr[2].ps1) == -4.0, "psq_l unscaled lane 1");

  /* Saturation, not wraparound: the point of the unsigned 8-bit type is
   * colour components. */
  cpu.gqr[1] = (0u << 24) | (4u << 16) | (0u << 8) | 4u;
  cpu.fpr[1].ps0 = bits_of(300.0);
  cpu.fpr[1].ps1 = bits_of(-5.0);
  step(enc_psq(GC_OP_PSQ_ST, 1, 4, 0, 1, 16));
  CHECK_U32(gc_test_memory[DATA_BASE - GC_TEST_BASE + 16], 255, "psq_st clamps at the top");
  CHECK_U32(gc_test_memory[DATA_BASE - GC_TEST_BASE + 17], 0, "psq_st clamps at the bottom");

  /* W = 1 moves one value and loads 1.0 into the other lane, which is how
   * a two-component vector is read into a register pair. */
  cpu.fpr[2].ps0 = 0;
  cpu.fpr[2].ps1 = 0;
  step(enc_psq(GC_OP_PSQ_L, 2, 4, 1, 1, 16));
  CHECK(value_of(cpu.fpr[2].ps0) == 255.0, "psq_l with W=1 loaded one value");
  CHECK(value_of(cpu.fpr[2].ps1) == 1.0, "and put 1.0 in the other lane");

  /* Type 0 is a plain single-precision pair, no conversion. */
  cpu.gqr[2] = 0;
  cpu.fpr[1].ps0 = bits_of(1.5);
  cpu.fpr[1].ps1 = bits_of(-2.5);
  step(enc_psq(GC_OP_PSQ_ST, 1, 4, 0, 2, 32));
  CHECK_U32(gc_test_read32(DATA_BASE + 32), 0x3fc00000u, "psq_st float lane 0");
  CHECK_U32(gc_test_read32(DATA_BASE + 36), 0xc0200000u, "psq_st float lane 1");
}

/* The floating-point unit switched off (MSR[FP] clear): an instruction that
 * needs it does not run. The processor goes to vector 0x800 with the
 * instruction's own address to come back to, and nothing the instruction
 * would have written is written. That is how an operating system loads a
 * thread's floating-point registers only when the thread wants them
 * (gc_cpu_ops.h, gc_insn_needs_fp). */
static void test_fp_unavailable(void) {
  static const struct {
    GcOp op;
    const char *name;
  } NEEDS[] = {
      {GC_OP_FADD, "fadd"},   {GC_OP_FADDS, "fadds"}, {GC_OP_FMR, "fmr"},       {GC_OP_FCMPU, "fcmpu"},
      {GC_OP_MFFS, "mffs"},   {GC_OP_LFS, "lfs"},     {GC_OP_STFD, "stfd"},     {GC_OP_LFSX, "lfsx"},
      {GC_OP_STFIWX, "stfiwx"}, {GC_OP_PS_ADD, "ps_add"}, {GC_OP_PSQ_L, "psq_l"},
  };
  unsigned i;
  start("floating point unavailable");
  for (i = 0; i < sizeof(NEEDS) / sizeof(NEEDS[0]); i++) {
    /* Registers 3, 1 and 2, or register 3 at the data base: every form
     * here would write f3, the condition register or the data. */
    const uint32_t insn = enc(NEEDS[i].op) | (3u << 21) | (4u << 16) | (2u << 11);
    uint32_t vector;
    CHECK(gc_insn_needs_fp(insn), "%s is taken to need the floating-point unit", NEEDS[i].name);
    cpu.gpr[4] = DATA_BASE;
    cpu.gpr[2] = 0;
    cpu.fpr[3].ps0 = bits_of(7.0);
    cpu.fpr[3].ps1 = bits_of(9.0);
    cpu.fpr[4].ps0 = bits_of(1.0);
    cpu.fpr[2].ps0 = bits_of(2.0);
    cpu.cr = 0x12345678u;
    gc_test_write32(DATA_BASE, 0xcafef00du);
    gc_test_write32(DATA_BASE + 4u, 0x0badf00du);
    cpu.msr = (cpu.msr | GC_MSR_IR | GC_MSR_DR | GC_MSR_EE) & ~GC_MSR_FP;
    cpu.srr0 = cpu.srr1 = 0xffffffffu;
    cpu.pending_vector = 0;
    vector = ((cpu.msr & GC_MSR_IP) ? 0xfff00000u : 0u) + 0x800u;
    step(insn);
    CHECK_U32(cpu.pc, vector, "the vector");
    CHECK_U32(cpu.srr0, CODE_BASE, "SRR0 is the instruction itself");
    CHECK((cpu.srr1 & GC_MSR_FP) == 0 && (cpu.srr1 & GC_MSR_EE) != 0, "%s: SRR1 keeps the MSR it interrupted (%08x)",
          NEEDS[i].name, cpu.srr1);
    CHECK((cpu.msr & (GC_MSR_EE | GC_MSR_IR | GC_MSR_DR | GC_MSR_FP)) == 0, "%s: the handler starts as any does (%08x)",
          NEEDS[i].name, cpu.msr);
    CHECK(value_of(cpu.fpr[3].ps0) == 7.0 && value_of(cpu.fpr[3].ps1) == 9.0, "%s wrote no register", NEEDS[i].name);
    CHECK_U32(cpu.cr, 0x12345678u, "the condition register");
    CHECK(gc_test_read32(DATA_BASE) == 0xcafef00du && gc_test_read32(DATA_BASE + 4u) == 0x0badf00du,
          "%s wrote no memory", NEEDS[i].name);
  }

  /* What does not need it runs with it off: integer work, and the cache
   * instruction that shares the paired singles' opcode. */
  CHECK(!gc_insn_needs_fp(enc_d(GC_OP_ADDI, 3, 0, 5)), "addi does not need the floating-point unit");
  CHECK(!gc_insn_needs_fp(enc_x(GC_OP_DCBZ_L, 0, 4, 0)), "dcbz_l does not either");
  cpu.msr = (cpu.msr | GC_MSR_IR | GC_MSR_DR) & ~GC_MSR_FP;
  step(enc_d(GC_OP_ADDI, 3, 0, 5));
  CHECK_U32(cpu.gpr[3], 5, "addi with the floating-point unit off");
  CHECK_U32(cpu.pc, CODE_BASE + 4u, "and no exception");

  /* And back on, the instruction that was refused runs. */
  cpu.msr |= GC_MSR_FP;
  cpu.fpr[1].ps0 = bits_of(1.0);
  cpu.fpr[2].ps0 = bits_of(2.0);
  step(enc_a(GC_OP_FADD, 3, 1, 2, 0));
  CHECK(value_of(cpu.fpr[3].ps0) == 3.0, "fadd with the floating-point unit on");
}

int main(void) {
  printf("gc_cpu_test\n");
  test_endianness();
  test_arithmetic();
  test_compare();
  test_rotate_and_shift();
  test_condition_register();
  test_branches();
  test_loads_and_stores();
  test_reservation();
  test_sprs_and_time();
  test_float_scalar();
  test_paired_singles();
  test_quantised();
  test_fp_unavailable();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
