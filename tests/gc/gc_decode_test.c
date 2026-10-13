/*
 * The decode classifier, checked against the table it was generated from.
 *
 * This is layer-one evidence: it proves the generated tables agree with the
 * ISA definition, not that the ISA definition matches a Gekko. The hardware
 * layer is hwtests/cputest and the vectors captured from an oracle
 * (docs/GC-PLAN.md, P1).
 *
 * Three checks:
 *  1. Every (primary, extended opcode) pair decodes to the same opcode that
 *     an exhaustive scan of the metadata picks. This covers all 131,072
 *     dispatch positions, so a mis-generated table cannot hide.
 *  2. Every opcode's canonical encoding decodes back to itself, with the
 *     free bits set both ways.
 *  3. Field accessors, on encodings written out by hand from the manual.
 */

#include <stdio.h>
#include <stdint.h>

#include "gc_cpu_decode.h"

static int failures;

#define CHECK(condition, ...)                             \
  do {                                                    \
    if (!(condition)) {                                   \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
      failures++;                                         \
    }                                                     \
  } while (0)

/* The reference decoder: a linear scan, matching only the bits inside the
 * primary opcode and the extended-opcode field, which is what the hardware
 * decodes on. */
static GcOp reference_decode(uint32_t insn) {
  const uint32_t selector = 0xfc0007ffu;
  int op;
  for (op = 1; op < GC_OP_COUNT; op++) {
    const uint32_t mask = gc_op_info[op].bitmask & selector;
    if ((insn & mask) == (gc_op_info[op].pattern & mask)) return (GcOp)op;
  }
  return GC_OP_ILLEGAL;
}

static void check_dispatch_space(void) {
  uint32_t primary, extended;
  unsigned mismatches = 0;
  unsigned decoded = 0;
  for (primary = 0; primary < 64; primary++) {
    for (extended = 0; extended < 2048; extended++) {
      const uint32_t insn = (primary << 26) | extended;
      const GcOp got = gc_decode(insn);
      const GcOp want = reference_decode(insn);
      if (got != want) {
        if (mismatches < 8) {
          printf("FAIL decode(%08x) = %s, expected %s\n", insn, gc_op_info[got].name,
                 gc_op_info[want].name);
        }
        mismatches++;
      }
      if (got != GC_OP_ILLEGAL) decoded++;
    }
  }
  CHECK(mismatches == 0, "%u dispatch positions disagree with the table", mismatches);
  printf("  dispatch positions: 131072 checked, %u decode to an opcode\n", decoded);
}

static void check_round_trip(void) {
  int op;
  for (op = 1; op < GC_OP_COUNT; op++) {
    const uint32_t mask = gc_op_info[op].bitmask;
    const uint32_t pattern = gc_op_info[op].pattern;
    /* The canonical encoding, then the same with every free bit set: both
     * must come back as this opcode, because free bits are operands. */
    CHECK(gc_decode(pattern) == (GcOp)op, "%s: canonical encoding decodes to %s",
          gc_op_info[op].name, gc_op_info[gc_decode(pattern)].name);
    CHECK(gc_decode(pattern | ~mask) == (GcOp)op, "%s: free bits set decodes to %s",
          gc_op_info[op].name, gc_op_info[gc_decode(pattern | ~mask)].name);
    CHECK(gc_decode_is_canonical((GcOp)op, pattern), "%s: canonical encoding rejected",
          gc_op_info[op].name);
  }
}

static void check_fields(void) {
  /* addi r3, r4, -2 */
  const uint32_t addi = 0x3864fffeu;
  CHECK(gc_decode(addi) == GC_OP_ADDI, "addi decodes to %s", gc_op_info[gc_decode(addi)].name);
  CHECK(GC_F_rD(addi) == 3, "addi rD = %u", GC_F_rD(addi));
  CHECK(GC_F_rA(addi) == 4, "addi rA = %u", GC_F_rA(addi));
  CHECK(GC_F_simm(addi) == -2, "addi simm = %d", (int)GC_F_simm(addi));

  /* lwz r5, 16(r6) */
  const uint32_t lwz = 0x80a60010u;
  CHECK(gc_decode(lwz) == GC_OP_LWZ, "lwz decodes to %s", gc_op_info[gc_decode(lwz)].name);
  CHECK(GC_F_rD(lwz) == 5 && GC_F_rA(lwz) == 6 && GC_F_simm(lwz) == 16, "lwz fields");

  /* b +0x100, and bl -4: the branch displacement is shifted, not scaled. */
  CHECK(gc_decode(0x48000100u) == GC_OP_B, "b decodes");
  CHECK(GC_F_LI(0x48000100u) == 0x100, "b displacement = %d", (int)GC_F_LI(0x48000100u));
  CHECK(GC_M_LK(0x48000100u) == 0, "b is not linking");
  CHECK(GC_F_LI(0x4bfffffdu) == -4, "bl displacement = %d", (int)GC_F_LI(0x4bfffffdu));
  CHECK(GC_M_LK(0x4bfffffdu) == 1, "bl links");

  /* mflr r0 is mfspr r0, 8, and the SPR field holds its two halves the
   * other way round - the classic place to get PowerPC decoding wrong. */
  const uint32_t mflr = 0x7c0802a6u;
  CHECK(gc_decode(mflr) == GC_OP_MFSPR, "mflr decodes to %s", gc_op_info[gc_decode(mflr)].name);
  CHECK(GC_F_spr(mflr) == 8, "mflr spr = %u", GC_F_spr(mflr));
  CHECK(GC_F_rD(mflr) == 0, "mflr rD = %u", GC_F_rD(mflr));

  /* mtctr r4 is mtspr 9, r4. */
  const uint32_t mtctr = 0x7c8903a6u;
  CHECK(gc_decode(mtctr) == GC_OP_MTSPR, "mtctr decodes to %s",
        gc_op_info[gc_decode(mtctr)].name);
  CHECK(GC_F_spr(mtctr) == 9, "mtctr spr = %u", GC_F_spr(mtctr));
  CHECK(GC_F_rS(mtctr) == 4, "mtctr rS = %u", GC_F_rS(mtctr));

  /* blr is bclr with BO = 20 (branch always). */
  CHECK(gc_decode(0x4e800020u) == GC_OP_BCLR, "blr decodes to %s",
        gc_op_info[gc_decode(0x4e800020u)].name);
  CHECK(GC_F_BO(0x4e800020u) == 20, "blr BO = %u", GC_F_BO(0x4e800020u));

  /* rlwinm r3, r4, 8, 16, 23 - the mask fields are separate operands. */
  const uint32_t rlwinm = 0x54834416u | (16u << 6) | (23u << 1);
  CHECK(gc_decode(rlwinm) == GC_OP_RLWINM, "rlwinm decodes");

  /* The paired-single forms: psq_l keeps W and I where a longer
   * displacement would be. */
  const uint32_t psq_l = 0xe0030008u;
  CHECK(gc_decode(psq_l) == GC_OP_PSQ_L, "psq_l decodes to %s",
        gc_op_info[gc_decode(psq_l)].name);
  CHECK(GC_F_ps_offset(psq_l) == 8, "psq_l offset = %d", (int)GC_F_ps_offset(psq_l));
  CHECK(GC_F_ps_W(psq_l) == 0, "psq_l W = %u", GC_F_ps_W(psq_l));
  CHECK(GC_F_ps_I(psq_l) == 0, "psq_l I = %u", GC_F_ps_I(psq_l));
  CHECK(gc_op_info[GC_OP_PSQ_L].paired, "psq_l is marked paired-single");
  CHECK(!gc_op_info[GC_OP_ADD].paired, "add is not marked paired-single");

  /* Opcodes this CPU does not have decode as illegal rather than as
   * something plausible: mtmsrd, and the 64-bit ld and std. AltiVec is not
   * on this list because it occupies primary opcode 4, the same space the
   * paired-single instructions use - a CPU has one or the other, and the
   * generated table takes only the paired singles. */
  CHECK(gc_decode(0x7c0002a4u) == GC_OP_ILLEGAL, "mtmsrd is illegal");
  CHECK(gc_decode(0xe8030000u) == GC_OP_ILLEGAL, "ld is illegal");
  CHECK(gc_decode(0xf8030000u) == GC_OP_ILLEGAL, "std is illegal");
}

int main(void) {
  printf("gc_decode_test\n");
  check_dispatch_space();
  check_round_trip();
  check_fields();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("OK: %d opcodes\n", GC_OP_COUNT - 1);
  return 0;
}
