/*
 * The RSP interpreter's fast paths against its reference.
 *
 * n64_rsp_impl.h keeps a portable handler for every vector load, store and
 * computational instruction (n64_rsp_ref_*); the interpreter runs
 * n64_rsp_op_*, which on x86 is SSE2 and on a little-endian host copies
 * whole lanes. This test holds the two against each other, and the
 * interpreter's decode cache and run loop against a plain decode-and-step
 * interpreter kept here:
 *
 *  - every computational instruction, reference and fast handler, over
 *    every pair of edge-case lane values with every carry and compare flag
 *    pattern and saturating accumulators, then over random state, elements
 *    and aliased registers: vd, the accumulator slices, VCO, VCC, VCE and
 *    the divide state must match;
 *  - every vector load and store the same way: registers and DMEM;
 *  - scalar loads and stores against a byte-at-a-time model, across the end
 *    of DMEM;
 *  - random instruction words, one at a time and as programs run in slices
 *    of random budgets, through n64_rsp_run and through the plain
 *    interpreter: every register, flag, memory byte, counter and callback
 *    must match, as must the cycles each slice reports;
 *  - IMEM rewritten under the interpreter (by the bus, by a DMA the CPU
 *    starts, by a DMA the RSP starts over the instructions ahead of it):
 *    the new instructions run, never a stale decoding.
 *
 * The plain interpreter (ref_execute, ref_step, ref_run) is the decode and
 * step n64_rsp.c had before it kept decodings, calling the reference
 * handlers; like n64_rsp.c it follows ares's rsp/interpreter.cpp.
 */
#include "n64_rsp_impl.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static unsigned long g_checks;

/* ---- the plain interpreter ---- */

static void ref_execute_special(N64Rsp *rsp, uint32_t op)
{
    switch (op & 0x3Fu) {
    case 0x00: n64_rsp_op_sll(rsp, op); break;
    case 0x02: n64_rsp_op_srl(rsp, op); break;
    case 0x03: n64_rsp_op_sra(rsp, op); break;
    case 0x04: n64_rsp_op_sllv(rsp, op); break;
    case 0x06: n64_rsp_op_srlv(rsp, op); break;
    case 0x07: n64_rsp_op_srav(rsp, op); break;
    case 0x08: n64_rsp_op_jr(rsp, op); break;
    case 0x09: n64_rsp_op_jalr(rsp, op); break;
    case 0x0D: n64_rsp_op_break(rsp, op); break;
    case 0x20: /* ADD */
    case 0x21: n64_rsp_op_addu(rsp, op); break;
    case 0x22: /* SUB */
    case 0x23: n64_rsp_op_subu(rsp, op); break;
    case 0x24: n64_rsp_op_and(rsp, op); break;
    case 0x25: n64_rsp_op_or(rsp, op); break;
    case 0x26: n64_rsp_op_xor(rsp, op); break;
    case 0x27: n64_rsp_op_nor(rsp, op); break;
    case 0x2A: n64_rsp_op_slt(rsp, op); break;
    case 0x2B: n64_rsp_op_sltu(rsp, op); break;
    default: n64_rsp_op_special_invalid(rsp, op); break;
    }
}

static void ref_execute_regimm(N64Rsp *rsp, uint32_t op)
{
    switch (N64_RSP_RT(op)) {
    case 0x00: n64_rsp_op_bltz(rsp, op); break;
    case 0x01: n64_rsp_op_bgez(rsp, op); break;
    case 0x10: n64_rsp_op_bltzal(rsp, op); break;
    case 0x11: n64_rsp_op_bgezal(rsp, op); break;
    default: break; /* the likely and trap forms do nothing */
    }
}

static void ref_execute_cop2(N64Rsp *rsp, uint32_t op)
{
    if (!(op & (1u << 25))) {
        switch (N64_RSP_RS(op)) {
        case 0x00: n64_rsp_op_mfc2(rsp, op); break;
        case 0x02: n64_rsp_op_cfc2(rsp, op); break;
        case 0x04: n64_rsp_op_mtc2(rsp, op); break;
        case 0x06: n64_rsp_op_ctc2(rsp, op); break;
        default: break;
        }
        return;
    }
    switch (op & 0x3Fu) {
    case 0x00: n64_rsp_ref_vmulf(rsp, op); break;
    case 0x01: n64_rsp_ref_vmulu(rsp, op); break;
    case 0x02: n64_rsp_ref_vrndp(rsp, op); break;
    case 0x03: n64_rsp_ref_vmulq(rsp, op); break;
    case 0x04: n64_rsp_ref_vmudl(rsp, op); break;
    case 0x05: n64_rsp_ref_vmudm(rsp, op); break;
    case 0x06: n64_rsp_ref_vmudn(rsp, op); break;
    case 0x07: n64_rsp_ref_vmudh(rsp, op); break;
    case 0x08: n64_rsp_ref_vmacf(rsp, op); break;
    case 0x09: n64_rsp_ref_vmacu(rsp, op); break;
    case 0x0A: n64_rsp_ref_vrndn(rsp, op); break;
    case 0x0B: n64_rsp_ref_vmacq(rsp, op); break;
    case 0x0C: n64_rsp_ref_vmadl(rsp, op); break;
    case 0x0D: n64_rsp_ref_vmadm(rsp, op); break;
    case 0x0E: n64_rsp_ref_vmadn(rsp, op); break;
    case 0x0F: n64_rsp_ref_vmadh(rsp, op); break;
    case 0x10: n64_rsp_ref_vadd(rsp, op); break;
    case 0x11: n64_rsp_ref_vsub(rsp, op); break;
    case 0x13: n64_rsp_ref_vabs(rsp, op); break;
    case 0x14: n64_rsp_ref_vaddc(rsp, op); break;
    case 0x15: n64_rsp_ref_vsubc(rsp, op); break;
    case 0x1D: n64_rsp_ref_vsar(rsp, op); break;
    case 0x20: n64_rsp_ref_vlt(rsp, op); break;
    case 0x21: n64_rsp_ref_veq(rsp, op); break;
    case 0x22: n64_rsp_ref_vne(rsp, op); break;
    case 0x23: n64_rsp_ref_vge(rsp, op); break;
    case 0x24: n64_rsp_ref_vcl(rsp, op); break;
    case 0x25: n64_rsp_ref_vch(rsp, op); break;
    case 0x26: n64_rsp_ref_vcr(rsp, op); break;
    case 0x27: n64_rsp_ref_vmrg(rsp, op); break;
    case 0x28: n64_rsp_ref_vand(rsp, op); break;
    case 0x29: n64_rsp_ref_vnand(rsp, op); break;
    case 0x2A: n64_rsp_ref_vor(rsp, op); break;
    case 0x2B: n64_rsp_ref_vnor(rsp, op); break;
    case 0x2C: n64_rsp_ref_vxor(rsp, op); break;
    case 0x2D: n64_rsp_ref_vnxor(rsp, op); break;
    case 0x30: n64_rsp_ref_vrcp(rsp, op); break;
    case 0x31: n64_rsp_ref_vrcpl(rsp, op); break;
    case 0x32: n64_rsp_ref_vrcph(rsp, op); break;
    case 0x33: n64_rsp_ref_vmov(rsp, op); break;
    case 0x34: n64_rsp_ref_vrsq(rsp, op); break;
    case 0x35: n64_rsp_ref_vrsql(rsp, op); break;
    case 0x36: n64_rsp_ref_vrsqh(rsp, op); break;
    case 0x37: /* VNOP */
    case 0x3F: n64_rsp_ref_vnop(rsp, op); break; /* VNULL */
    default: n64_rsp_ref_vzero(rsp, op); break;
    }
}

static void ref_execute_lwc2(N64Rsp *rsp, uint32_t op)
{
    switch (N64_RSP_RD(op)) {
    case 0x00: n64_rsp_ref_lbv(rsp, op); break;
    case 0x01: n64_rsp_ref_lsv(rsp, op); break;
    case 0x02: n64_rsp_ref_llv(rsp, op); break;
    case 0x03: n64_rsp_ref_ldv(rsp, op); break;
    case 0x04: n64_rsp_ref_lqv(rsp, op); break;
    case 0x05: n64_rsp_ref_lrv(rsp, op); break;
    case 0x06: n64_rsp_ref_lpv(rsp, op); break;
    case 0x07: n64_rsp_ref_luv(rsp, op); break;
    case 0x08: n64_rsp_ref_lhv(rsp, op); break;
    case 0x09: n64_rsp_ref_lfv(rsp, op); break;
    case 0x0A: n64_rsp_ref_lwv(rsp, op); break;
    case 0x0B: n64_rsp_ref_ltv(rsp, op); break;
    default: break;
    }
}

static void ref_execute_swc2(N64Rsp *rsp, uint32_t op)
{
    switch (N64_RSP_RD(op)) {
    case 0x00: n64_rsp_ref_sbv(rsp, op); break;
    case 0x01: n64_rsp_ref_ssv(rsp, op); break;
    case 0x02: n64_rsp_ref_slv(rsp, op); break;
    case 0x03: n64_rsp_ref_sdv(rsp, op); break;
    case 0x04: n64_rsp_ref_sqv(rsp, op); break;
    case 0x05: n64_rsp_ref_srv(rsp, op); break;
    case 0x06: n64_rsp_ref_spv(rsp, op); break;
    case 0x07: n64_rsp_ref_suv(rsp, op); break;
    case 0x08: n64_rsp_ref_shv(rsp, op); break;
    case 0x09: n64_rsp_ref_sfv(rsp, op); break;
    case 0x0A: n64_rsp_ref_swv(rsp, op); break;
    case 0x0B: n64_rsp_ref_stv(rsp, op); break;
    default: break;
    }
}

/* Opcodes the RSP does not implement (COP1, COP3, the likely branches,
 * 64-bit loads and stores, LWL/LWR/SWL/SWR, CACHE, LL/SC...) do nothing. */
static void ref_execute(N64Rsp *rsp, uint32_t op)
{
    switch (op >> 26) {
    case 0x00: ref_execute_special(rsp, op); break;
    case 0x01: ref_execute_regimm(rsp, op); break;
    case 0x02: n64_rsp_op_j(rsp, op); break;
    case 0x03: n64_rsp_op_jal(rsp, op); break;
    case 0x04: n64_rsp_op_beq(rsp, op); break;
    case 0x05: n64_rsp_op_bne(rsp, op); break;
    case 0x06: n64_rsp_op_blez(rsp, op); break;
    case 0x07: n64_rsp_op_bgtz(rsp, op); break;
    case 0x08: /* ADDI */
    case 0x09: n64_rsp_op_addiu(rsp, op); break;
    case 0x0A: n64_rsp_op_slti(rsp, op); break;
    case 0x0B: n64_rsp_op_sltiu(rsp, op); break;
    case 0x0C: n64_rsp_op_andi(rsp, op); break;
    case 0x0D: n64_rsp_op_ori(rsp, op); break;
    case 0x0E: n64_rsp_op_xori(rsp, op); break;
    case 0x0F: n64_rsp_op_lui(rsp, op); break;
    case 0x10:
        if (N64_RSP_RS(op) == 0x00)
            n64_rsp_op_mfc0(rsp, op);
        else if (N64_RSP_RS(op) == 0x04)
            n64_rsp_op_mtc0(rsp, op);
        break;
    case 0x12: ref_execute_cop2(rsp, op); break;
    case 0x20: n64_rsp_op_lb(rsp, op); break;
    case 0x21: n64_rsp_op_lh(rsp, op); break;
    case 0x23: /* LW */
    case 0x27: n64_rsp_op_lw(rsp, op); break; /* LWU */
    case 0x24: n64_rsp_op_lbu(rsp, op); break;
    case 0x25: n64_rsp_op_lhu(rsp, op); break;
    case 0x28: n64_rsp_op_sb(rsp, op); break;
    case 0x29: n64_rsp_op_sh(rsp, op); break;
    case 0x2B: n64_rsp_op_sw(rsp, op); break;
    case 0x32: ref_execute_lwc2(rsp, op); break;
    case 0x3A: ref_execute_swc2(rsp, op); break;
    default: break;
    }
}

/* One instruction. The PC pair (pc, next_pc) is what makes delay slots
 * work: a taken branch only replaces next_pc for its delay slot, so a
 * branch in a delay slot behaves as on MIPS (the first target runs one
 * instruction, then the second). */
static void ref_step(N64Rsp *rsp)
{
    uint32_t op = n64_be32(rsp->imem + (rsp->pc & 0xFFCu));
    uint32_t next = rsp->next_pc;
    rsp->branch_pending = 0;
    ref_execute(rsp, op);
    rsp->r[0] = 0;
    rsp->next_pc = rsp->branch_pending ? rsp->branch_target : ((next + 4u) & 0xFFCu);
    rsp->pc = next & 0xFFCu;
    rsp->branch_pending = 0;
    rsp->branch_target = 0;
    rsp->cycles++;
    rsp->instructions++;
    if (rsp->status & N64_SP_STATUS_SSTEP) rsp->status |= N64_SP_STATUS_HALT;
}

static uint64_t ref_run(N64Rsp *rsp, uint64_t budget)
{
    uint64_t used = 0;
    while (used < budget && !(rsp->status & N64_SP_STATUS_HALT)) {
        ref_step(rsp);
        used++;
    }
    return used;
}

/* ---- random and edge-case state ---- */

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void)
{
    g_seed ^= g_seed >> 12;
    g_seed ^= g_seed << 25;
    g_seed ^= g_seed >> 27;
    return (uint32_t)((g_seed * 0x2545F4914F6CDD1Dull) >> 32);
}

/* Lane values where signs, saturation and carries change. */
static const uint16_t g_edge[16] = {0x0000, 0x0001, 0x0002, 0x7FFE, 0x7FFF, 0x8000, 0x8001, 0xFFFE,
                                    0xFFFF, 0x4000, 0xC000, 0x00FF, 0xFF00, 0x5555, 0xAAAA, 0x0080};

static uint16_t rnd_lane(void)
{
    uint32_t r = rnd();
    return (r & 0x10000u) ? (uint16_t)r : g_edge[(r >> 20) & 15u];
}

static void rnd_vector(uint16_t *v)
{
    unsigned n;
    for (n = 0; n < 8; n++) v[n] = rnd_lane();
}

/* The accumulator near the points where its clamps and carries act. */
static void rnd_accumulator(N64RspVu *vu)
{
    static const uint16_t slice[8] = {0x0000, 0xFFFF, 0x7FFF, 0x8000, 0x0001, 0xFFFE, 0x8001, 0x7FFE};
    unsigned n;
    for (n = 0; n < 8; n++) {
        uint32_t r = rnd();
        if (r & 1u) {
            vu->acc_h[n] = (uint16_t)rnd();
            vu->acc_m[n] = (uint16_t)rnd();
            vu->acc_l[n] = (uint16_t)rnd();
        } else {
            vu->acc_h[n] = slice[(r >> 4) & 7u];
            vu->acc_m[n] = slice[(r >> 8) & 7u];
            vu->acc_l[n] = slice[(r >> 12) & 7u];
        }
    }
}

static void rnd_flags(N64RspVu *vu)
{
    uint32_t r = rnd(), s = rnd();
    vu->vco_lo = (uint16_t)(r & 0xFFu);
    vu->vco_hi = (uint16_t)((r >> 8) & 0xFFu);
    vu->vcc_lo = (uint16_t)((r >> 16) & 0xFFu);
    vu->vcc_hi = (uint16_t)((r >> 24) & 0xFFu);
    vu->vce = (uint8_t)s;
    vu->div_in = n64_rsp_i16(rnd_lane());
    vu->div_out = n64_rsp_i16(rnd_lane());
    vu->div_dp = (uint8_t)((s >> 8) & 1u);
}

/* A scalar register: any value, a small one, or one near the end of DMEM. */
static uint32_t rnd_scalar(void)
{
    uint32_t r = rnd();
    switch (r & 3u) {
    case 0: return rnd();
    case 1: return (r >> 8) & 0xFFFu;
    case 2: return 0xFD0u + ((r >> 8) & 0x7Fu);
    default: return (r >> 4) & 0xFF0u;
    }
}

static void rnd_bytes(uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (uint8_t)(rnd() >> 13);
}

/* A whole RSP with nothing left at zero. */
static void rnd_rsp(N64Rsp *rsp)
{
    unsigned i;
    N64RspLinks links = rsp->links;
    memset(rsp, 0, sizeof *rsp);
    rsp->links = links;
    rnd_bytes(rsp->dmem, sizeof rsp->dmem);
    rnd_bytes(rsp->imem, sizeof rsp->imem);
    for (i = 1; i < 32; i++) rsp->r[i] = rnd_scalar();
    for (i = 0; i < 32; i++) rnd_vector(rsp->vu.v[i]);
    rnd_accumulator(&rsp->vu);
    rnd_flags(&rsp->vu);
    rsp->next_pc = 4;
}

/* Everything the RSP's state is, in the order the struct has it; the
 * decode cache and the links are not state. */
#define RSP_STATE_SIZE offsetof(N64Rsp, decoded)

static const char *vu_difference(const N64Rsp *a, const N64Rsp *b)
{
    if (memcmp(a->vu.v, b->vu.v, sizeof a->vu.v)) return "a vector register";
    if (memcmp(a->vu.acc_h, b->vu.acc_h, sizeof a->vu.acc_h)) return "the accumulator's high slice";
    if (memcmp(a->vu.acc_m, b->vu.acc_m, sizeof a->vu.acc_m)) return "the accumulator's middle slice";
    if (memcmp(a->vu.acc_l, b->vu.acc_l, sizeof a->vu.acc_l)) return "the accumulator's low slice";
    if (a->vu.vco_lo != b->vu.vco_lo || a->vu.vco_hi != b->vu.vco_hi) return "VCO";
    if (a->vu.vcc_lo != b->vu.vcc_lo || a->vu.vcc_hi != b->vu.vcc_hi) return "VCC";
    if (a->vu.vce != b->vu.vce) return "VCE";
    if (a->vu.div_in != b->vu.div_in || a->vu.div_out != b->vu.div_out || a->vu.div_dp != b->vu.div_dp)
        return "the divide state";
    if (memcmp(a->r, b->r, sizeof a->r)) return "a scalar register";
    return NULL;
}

static const char *first_difference(const N64Rsp *a, const N64Rsp *b)
{
    const char *what = vu_difference(a, b);
    if (what) return what;
    if (a->pc != b->pc || a->next_pc != b->next_pc) return "the PC";
    if (a->branch_pending != b->branch_pending || a->branch_target != b->branch_target) return "the branch scratch";
    if (memcmp(a->dmem, b->dmem, sizeof a->dmem)) return "DMEM";
    if (memcmp(a->imem, b->imem, sizeof a->imem)) return "IMEM";
    if (a->status != b->status) return "SP_STATUS";
    if (a->cycles != b->cycles || a->instructions != b->instructions) return "the cycle count";
    if (memcmp(a, b, RSP_STATE_SIZE)) return "an SP register";
    return NULL;
}

static void report(const char *test, const char *name, uint32_t op, const char *what)
{
    g_failures++;
    if (g_failures <= 40) fprintf(stderr, "%s: %s (word %08lX): %s differs\n", test, name, (unsigned long)op, what);
}

/* ---- computational instructions ---- */

typedef void (*Handler)(N64Rsp *rsp, uint32_t op);

typedef struct Instruction {
    const char *name;
    unsigned code; /* COP2 function, or the LWC2/SWC2 rd field */
    Handler ref, fast;
} Instruction;

#define INSTRUCTION(name, code) {#name, code, n64_rsp_ref_##name, n64_rsp_op_##name}

static const Instruction g_computational[] = {
    INSTRUCTION(vmulf, 0x00), INSTRUCTION(vmulu, 0x01), INSTRUCTION(vrndp, 0x02), INSTRUCTION(vmulq, 0x03),
    INSTRUCTION(vmudl, 0x04), INSTRUCTION(vmudm, 0x05), INSTRUCTION(vmudn, 0x06), INSTRUCTION(vmudh, 0x07),
    INSTRUCTION(vmacf, 0x08), INSTRUCTION(vmacu, 0x09), INSTRUCTION(vrndn, 0x0A), INSTRUCTION(vmacq, 0x0B),
    INSTRUCTION(vmadl, 0x0C), INSTRUCTION(vmadm, 0x0D), INSTRUCTION(vmadn, 0x0E), INSTRUCTION(vmadh, 0x0F),
    INSTRUCTION(vadd, 0x10),  INSTRUCTION(vsub, 0x11),  INSTRUCTION(vzero, 0x12), INSTRUCTION(vabs, 0x13),
    INSTRUCTION(vaddc, 0x14), INSTRUCTION(vsubc, 0x15), INSTRUCTION(vsar, 0x1D),  INSTRUCTION(vlt, 0x20),
    INSTRUCTION(veq, 0x21),   INSTRUCTION(vne, 0x22),   INSTRUCTION(vge, 0x23),   INSTRUCTION(vcl, 0x24),
    INSTRUCTION(vch, 0x25),   INSTRUCTION(vcr, 0x26),   INSTRUCTION(vmrg, 0x27),  INSTRUCTION(vand, 0x28),
    INSTRUCTION(vnand, 0x29), INSTRUCTION(vor, 0x2A),   INSTRUCTION(vnor, 0x2B),  INSTRUCTION(vxor, 0x2C),
    INSTRUCTION(vnxor, 0x2D), INSTRUCTION(vrcp, 0x30),  INSTRUCTION(vrcpl, 0x31), INSTRUCTION(vrcph, 0x32),
    INSTRUCTION(vmov, 0x33),  INSTRUCTION(vrsq, 0x34),  INSTRUCTION(vrsql, 0x35), INSTRUCTION(vrsqh, 0x36),
    INSTRUCTION(vnop, 0x37),
};

static const Instruction g_loads[] = {
    INSTRUCTION(lbv, 0x00), INSTRUCTION(lsv, 0x01), INSTRUCTION(llv, 0x02), INSTRUCTION(ldv, 0x03),
    INSTRUCTION(lqv, 0x04), INSTRUCTION(lrv, 0x05), INSTRUCTION(lpv, 0x06), INSTRUCTION(luv, 0x07),
    INSTRUCTION(lhv, 0x08), INSTRUCTION(lfv, 0x09), INSTRUCTION(lwv, 0x0A), INSTRUCTION(ltv, 0x0B),
};

static const Instruction g_stores[] = {
    INSTRUCTION(sbv, 0x00), INSTRUCTION(ssv, 0x01), INSTRUCTION(slv, 0x02), INSTRUCTION(sdv, 0x03),
    INSTRUCTION(sqv, 0x04), INSTRUCTION(srv, 0x05), INSTRUCTION(spv, 0x06), INSTRUCTION(suv, 0x07),
    INSTRUCTION(shv, 0x08), INSTRUCTION(sfv, 0x09), INSTRUCTION(swv, 0x0A), INSTRUCTION(stv, 0x0B),
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t enc_vector(unsigned funct, unsigned vd, unsigned vs, unsigned vt, unsigned e)
{
    return (0x12u << 26) | (1u << 25) | ((uint32_t)e << 21) | ((uint32_t)vt << 16) | ((uint32_t)vs << 11) |
           ((uint32_t)vd << 6) | funct;
}

/* Runs the reference on `a` and the fast handler on `b`, which hold the
 * same state, and compares the vector unit and the scalar registers, and
 * DMEM when the instruction is a load or a store. */
static void compare_handlers(const char *test, const Instruction *ins, uint32_t op, N64Rsp *a, N64Rsp *b, int memory)
{
    const char *what;
    ins->ref(a, op);
    ins->fast(b, op);
    g_checks++;
    what = vu_difference(a, b);
    if (!what && memory && memcmp(a->dmem, b->dmem, sizeof a->dmem)) what = "DMEM";
    if (what) {
        report(test, ins->name, op, what);
        memcpy(b, a, RSP_STATE_SIZE);
    }
}

/* Every pair of edge values in vs and vt (all 256 pairs, eight to a
 * register), under every pattern of the flags an instruction reads and
 * with the accumulator at its clamping and carrying points. */
static void test_computational_edges(N64Rsp *a, N64Rsp *b)
{
    static const uint8_t flag[4] = {0x00, 0xFF, 0xA5, 0x3C};
    static const uint16_t acc[8][3] = {
        {0x0000, 0x0000, 0x0000}, {0xFFFF, 0xFFFF, 0xFFFF}, {0x0000, 0x7FFF, 0xFFFF}, {0xFFFF, 0x8000, 0x0000},
        {0x7FFF, 0xFFFF, 0xFFFF}, {0x8000, 0x0000, 0x0000}, {0x0000, 0xFFFF, 0xFFFF}, {0xFFFF, 0x7FFF, 0x8000},
    };
    unsigned i, pair, f, k, n;
    rnd_rsp(a);
    memcpy(b, a, RSP_STATE_SIZE);
    for (i = 0; i < COUNT(g_computational); i++) {
        const Instruction *ins = &g_computational[i];
        for (pair = 0; pair < 256; pair += 8) {
            for (f = 0; f < 256; f++) {
                for (k = 0; k < 8; k++) {
                    /* Lane n has pair + n: vs walks the edge table while vt
                     * holds one of its values. */
                    for (n = 0; n < 8; n++) {
                        a->vu.v[1][n] = g_edge[(pair + n) & 15u];
                        a->vu.v[2][n] = g_edge[(pair + n) >> 4];
                        a->vu.acc_h[n] = acc[(k + n) & 7u][0];
                        a->vu.acc_m[n] = acc[(k + n) & 7u][1];
                        a->vu.acc_l[n] = acc[(k + n) & 7u][2];
                    }
                    a->vu.vco_lo = flag[f & 3u];
                    a->vu.vco_hi = flag[(f >> 2) & 3u];
                    a->vu.vcc_lo = flag[(f >> 4) & 3u];
                    a->vu.vcc_hi = flag[(f >> 6) & 3u];
                    a->vu.vce = flag[(f + k) & 3u];
                    a->vu.div_dp = (uint8_t)(k & 1u);
                    b->vu = a->vu;
                    compare_handlers("computational edges", ins, enc_vector(ins->code, 3, 1, 2, (k & 1u) ? 9 : 0), a, b, 0);
                }
            }
        }
    }
}

/* Random state, registers that alias (vd is often vs or vt) and every
 * element, eight instructions in a row so each runs on what the last left. */
static void test_computational_random(N64Rsp *a, N64Rsp *b)
{
    unsigned i, round, step;
    rnd_rsp(a);
    memcpy(b, a, RSP_STATE_SIZE);
    for (i = 0; i < COUNT(g_computational); i++) {
        const Instruction *ins = &g_computational[i];
        for (round = 0; round < 4000; round++) {
            rnd_vector(a->vu.v[rnd() & 3u]);
            rnd_vector(a->vu.v[rnd() & 3u]);
            rnd_accumulator(&a->vu);
            rnd_flags(&a->vu);
            b->vu = a->vu;
            for (step = 0; step < 8; step++) {
                uint32_t r = rnd();
                compare_handlers("computational random", ins,
                                 enc_vector(ins->code, r & 3u, (r >> 2) & 3u, (r >> 4) & 3u, (r >> 6) & 15u), a, b,
                                 0);
            }
        }
    }
}

/* ---- vector loads and stores ---- */

static uint32_t enc_vmem(unsigned opc, unsigned kind, unsigned vt, unsigned base, unsigned e, uint32_t off7)
{
    return ((uint32_t)opc << 26) | ((uint32_t)base << 21) | ((uint32_t)vt << 16) | ((uint32_t)kind << 11) |
           ((uint32_t)e << 7) | (off7 & 0x7Fu);
}

static void test_vector_memory(N64Rsp *a, N64Rsp *b)
{
    unsigned set, i, round;
    rnd_rsp(a);
    memcpy(b, a, RSP_STATE_SIZE);
    for (set = 0; set < 2; set++) {
        const Instruction *list = set ? g_stores : g_loads;
        for (i = 0; i < 12; i++) {
            for (round = 0; round < 40000; round++) {
                uint32_t r = rnd();
                unsigned vt = (r >> 8) & 31u;
                /* Element 0 and aligned addresses, which are the fast
                 * paths, half the time; anything otherwise. */
                unsigned e = (r & 1u) ? 0u : (r >> 2) & 15u;
                uint32_t off = (r & 2u) ? 0u : (r >> 16) & 0x7Fu;
                uint32_t base = rnd_scalar();
                if (r & 0x40u) base &= ~7u;
                if (r & 0x80u) base &= ~15u;
                a->r[5] = b->r[5] = base;
                if (!(r & 0x3000u)) {
                    rnd_vector(a->vu.v[vt]);
                    memcpy(b->vu.v[vt], a->vu.v[vt], sizeof a->vu.v[vt]);
                }
                compare_handlers(set ? "vector stores" : "vector loads", &list[i],
                                 enc_vmem(set ? 0x3A : 0x32, list[i].code, vt, 5, e, off), a, b, 1);
            }
        }
    }
}

/* ---- scalar loads and stores ---- */

static uint32_t enc_i(unsigned opc, unsigned rt, unsigned rs, uint32_t imm)
{
    return ((uint32_t)opc << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (imm & 0xFFFFu);
}

/* LB, LH, LW, LBU, LHU, LWU and SB, SH, SW against DMEM read and written a
 * byte at a time, each byte wrapping on its own. Every address from below
 * the end of DMEM to past it, then random ones. IMEM must stay as it was:
 * no store reaches it. */
static void test_scalar_memory(N64Rsp *a, N64Rsp *b)
{
    static const struct {
        unsigned opc, size, is_signed, is_store;
    } kind[] = {{0x20, 1, 1, 0}, {0x21, 2, 1, 0}, {0x23, 4, 0, 0}, {0x24, 1, 0, 0}, {0x25, 2, 0, 0},
                {0x27, 4, 0, 0}, {0x28, 1, 0, 1}, {0x29, 2, 0, 1}, {0x2B, 4, 0, 1}};
    unsigned k, round, i;
    rnd_rsp(a);
    memcpy(b, a, RSP_STATE_SIZE);
    for (k = 0; k < COUNT(kind); k++) {
        for (round = 0; round < 20000; round++) {
            uint32_t address = round < 0x40 ? 0xFE0u + round : round < 0x80 ? 0x1FE0u + (round - 0x40) : rnd();
            uint32_t imm = rnd() & 0xFFFFu, value = rnd(), model = 0;
            uint32_t op = enc_i(kind[k].opc, 7, 6, imm);
            b->r[6] = address - n64_rsp_simm(imm);
            b->r[7] = value;
            if (kind[k].is_store) {
                for (i = 0; i < kind[k].size; i++)
                    a->dmem[(address + i) & 0xFFFu] = (uint8_t)(value >> (8u * (kind[k].size - 1u - i)));
            } else {
                for (i = 0; i < kind[k].size; i++) model = (model << 8) | a->dmem[(address + i) & 0xFFFu];
                if (kind[k].is_signed && kind[k].size == 1) model = (model ^ 0x80u) - 0x80u;
                if (kind[k].is_signed && kind[k].size == 2) model = (model ^ 0x8000u) - 0x8000u;
            }
            switch (kind[k].opc) {
            case 0x20: n64_rsp_op_lb(b, op); break;
            case 0x21: n64_rsp_op_lh(b, op); break;
            case 0x24: n64_rsp_op_lbu(b, op); break;
            case 0x25: n64_rsp_op_lhu(b, op); break;
            case 0x28: n64_rsp_op_sb(b, op); break;
            case 0x29: n64_rsp_op_sh(b, op); break;
            case 0x2B: n64_rsp_op_sw(b, op); break;
            default: n64_rsp_op_lw(b, op); break;
            }
            g_checks++;
            if (!kind[k].is_store && b->r[7] != model) report("scalar memory", "load", op, "the loaded value");
            if (kind[k].is_store && memcmp(a->dmem, b->dmem, sizeof a->dmem)) {
                report("scalar memory", "store", op, "DMEM");
                memcpy(b->dmem, a->dmem, sizeof a->dmem);
            }
            if (memcmp(a->imem, b->imem, sizeof a->imem)) {
                report("scalar memory", "store", op, "IMEM");
                memcpy(b->imem, a->imem, sizeof a->imem);
            }
        }
    }
}

static void test_clz(void)
{
    unsigned i, round;
    for (round = 0; round < 200000; round++) {
        uint32_t shift = rnd() & 31u;
        uint32_t v = round < 32 ? 1u << round : round < 64 ? (1u << (round - 32)) - 1u : rnd() >> shift;
        unsigned want = 0;
        for (i = 0; i < 32 && !((v << i) & 0x80000000u); i++) want++;
        g_checks++;
        if (n64_rsp_clz32(v) != want) report("clz", "n64_rsp_clz32", v, "the count");
    }
}

/* ---- the interpreter against the plain one ---- */

/* What the machine is told and when: every callback's arguments, and the
 * PC and the cycle count the RSP shows while it is in the callback. */
typedef struct Recorder {
    const N64Rsp *rsp;
    uint64_t hash;
    unsigned long calls;
} Recorder;

static void record(Recorder *rec, uint32_t what, uint32_t value)
{
    rec->hash = (rec->hash ^ (what * 0x9E3779B1u + value)) * 1099511628211ull;
    rec->hash = (rec->hash ^ rec->rsp->pc ^ (rec->rsp->cycles << 12) ^ (rec->rsp->instructions << 36)) * 1099511628211ull;
    rec->calls++;
}

static void rec_interrupt(void *user, int level) { record((Recorder *)user, 1, (uint32_t)level); }

static uint32_t rec_dp_read(void *user, unsigned reg)
{
    Recorder *rec = (Recorder *)user;
    record(rec, 2, reg);
    return (uint32_t)(rec->hash >> 20);
}

static void rec_dp_write(void *user, unsigned reg, uint32_t value) { record((Recorder *)user, 3u + reg, value); }

static void rec_ram_written(void *user, uint32_t addr, uint32_t size) { record((Recorder *)user, 20, addr ^ size); }

/* Two RSPs with their own RDRAM and callbacks: `a` is run by the plain
 * interpreter, `b` by n64_rsp_run. */
#define PAIR_RDRAM 0x10000u

typedef struct Pair {
    N64Rsp *a, *b;
    uint8_t *rdram_a, *rdram_b;
    Recorder rec_a, rec_b;
} Pair;

static void pair_link(Pair *p)
{
    N64RspLinks links;
    memset(&links, 0, sizeof links);
    links.rdram_size = PAIR_RDRAM;
    links.interrupt = rec_interrupt;
    links.dp_read = rec_dp_read;
    links.dp_write = rec_dp_write;
    links.ram_written = rec_ram_written;
    p->a->links = links;
    p->a->links.rdram = p->rdram_a;
    p->a->links.user = &p->rec_a;
    p->rec_a.rsp = p->a;
    p->rec_b.rsp = p->b;
    p->b->links = links;
    p->b->links.rdram = p->rdram_b;
    p->b->links.user = &p->rec_b;
}

/* Fresh random state in both; the decode cache of `b` is kept, as it would
 * be across anything that rewrites IMEM. */
static void pair_randomise(Pair *p)
{
    rnd_rsp(p->a);
    memcpy(p->b, p->a, RSP_STATE_SIZE);
    rnd_bytes(p->rdram_a, PAIR_RDRAM);
    memcpy(p->rdram_b, p->rdram_a, PAIR_RDRAM);
    p->rec_a.hash = p->rec_b.hash = 0;
    p->rec_a.calls = p->rec_b.calls = 0;
}

static int pair_compare(Pair *p, const char *test, uint32_t op, int check_rdram)
{
    const char *what = first_difference(p->a, p->b);
    g_checks++;
    if (!what && (p->rec_a.hash != p->rec_b.hash || p->rec_a.calls != p->rec_b.calls)) what = "the callbacks";
    if (!what && check_rdram && memcmp(p->rdram_a, p->rdram_b, PAIR_RDRAM)) what = "RDRAM";
    if (!what) return 1;
    report(test, "n64_rsp_run", op, what);
    memcpy(p->b, p->a, RSP_STATE_SIZE);
    memcpy(p->rdram_b, p->rdram_a, PAIR_RDRAM);
    p->rec_b.hash = p->rec_a.hash;
    p->rec_b.calls = p->rec_a.calls;
    return 0;
}

/* An instruction word: every primary opcode, SPECIAL and COP2 function,
 * REGIMM, COP0 and COP2 sub-opcode and load/store kind turns up, valid or
 * not, with random fields. Registers stay in a small set so results feed
 * later instructions. */
static uint32_t rnd_word(void)
{
    uint32_t r = rnd(), s = rnd();
    uint32_t fields = s & 0x03FFFFFFu;
    switch (r & 15u) {
    case 0:
    case 1: return (fields & ~0x3Fu) | ((r >> 8) & 0x3Fu);                         /* SPECIAL */
    case 2: return (0x01u << 26) | fields;                                         /* REGIMM */
    case 3: return (0x10u << 26) | (((r >> 8) & 1u) ? 0x04u << 21 : 0u) | (fields & 0x001FFFFFu); /* MFC0, MTC0 */
    case 4: return (0x10u << 26) | fields;                                         /* COP0, any */
    case 5: return (0x12u << 26) | (fields & 0x01FFFFFFu);                         /* COP2 moves */
    case 6:
    case 7:
    case 8:
    case 9: return (0x12u << 26) | (1u << 25) | fields;                            /* COP2 computational */
    case 10: return (0x32u << 26) | fields;                                        /* LWC2 */
    case 11: return (0x3Au << 26) | fields;                                        /* SWC2 */
    case 12: return ((0x20u + ((r >> 8) & 15u)) << 26) | fields;                   /* loads and stores */
    case 13: return ((2u + ((r >> 8) % 6u)) << 26) | fields;                       /* J, JAL and the branches */
    default: return (((r >> 8) & 0x3Fu) << 26) | fields;                           /* any opcode */
    }
}

/* One random instruction at a time at a random PC, sometimes in a delay
 * slot (next_pc anywhere) and sometimes single-stepping. */
static void test_single_instructions(Pair *p)
{
    unsigned long round;
    pair_randomise(p);
    for (round = 0; round < 600000; round++) {
        uint32_t r = rnd(), op = rnd_word();
        uint32_t pc = (r >> 4) & 0xFFCu;
        uint64_t used_a, used_b;
        unsigned i;
        if (!(round & 0x3FFFu)) pair_randomise(p);
        n64_put_be32(p->a->imem + pc, op);
        p->a->pc = pc;
        p->a->next_pc = (r & 7u) ? (pc + 4u) & 0xFFCu : (rnd() & 0xFFCu);
        p->a->status &= ~(N64_SP_STATUS_HALT | N64_SP_STATUS_SSTEP);
        if (!(r & 0xF0000u)) p->a->status |= N64_SP_STATUS_SSTEP;
        for (i = 0; i < 3; i++) {
            unsigned reg = 1u + (rnd() % 31u);
            p->a->r[reg] = rnd_scalar();
        }
        rnd_vector(p->a->vu.v[rnd() & 31u]);
        if (!(r & 0x300000u)) rnd_accumulator(&p->a->vu);
        if (!(r & 0xC00000u)) rnd_flags(&p->a->vu);
        memcpy(p->b, p->a, RSP_STATE_SIZE);
        used_a = ref_run(p->a, 1);
        used_b = n64_rsp_run(p->b, 1);
        if (used_a != used_b) report("single instructions", "n64_rsp_run", op, "the cycles used");
        pair_compare(p, "single instructions", op, (op >> 26) == 0x10u);
    }
}

/* Programs of random words filling IMEM, run from a random PC in slices of
 * random budgets (as the machine does; a few are longer than the run
 * loop's own chains) until they halt or 400 instructions, sometimes 4,000,
 * have run. Their DMAs and nothing else rewrite IMEM as they go. */
static void test_programs(Pair *p)
{
    unsigned program, i;
    for (program = 0; program < 3000; program++) {
        unsigned long left = (program % 10u) == 7u ? 4000 : 400;
        uint32_t last = 0;
        if (!(program & 15u)) pair_randomise(p);
        /* A fresh program most times; otherwise the last one again with a
         * few words replaced, over decodings the interpreter still holds. */
        if (program & 3u) {
            for (i = 0; i < 0x400u; i++) n64_put_be32(p->a->imem + 4u * i, rnd_word());
        } else {
            for (i = 0; i < 40; i++) {
                uint32_t at = rnd() & 0xFFCu;
                n64_put_be32(p->a->imem + at, rnd_word());
            }
        }
        memcpy(p->b->imem, p->a->imem, sizeof p->a->imem);
        p->a->pc = p->b->pc = rnd() & 0xFFCu;
        p->a->next_pc = p->b->next_pc = (p->a->pc + 4u) & 0xFFCu;
        p->a->status = p->b->status = p->a->status & ~(N64_SP_STATUS_HALT | N64_SP_STATUS_SSTEP);
        /* Now and then the CPU has asked for single steps. */
        if ((program % 23u) == 11u) p->a->status = p->b->status = p->a->status | N64_SP_STATUS_SSTEP;
        while (left) {
            uint32_t longest = (rnd() & 7u) ? 60u : 1500u;
            uint64_t budget = 1u + (rnd() % longest), used_a, used_b;
            if (budget > left) budget = left;
            last = n64_be32(p->a->imem + (p->a->pc & 0xFFCu));
            used_a = ref_run(p->a, budget);
            used_b = n64_rsp_run(p->b, budget);
            if (used_a != used_b) report("programs", "n64_rsp_run", last, "the cycles used");
            if (!pair_compare(p, "programs", last, 1)) break;
            if (n64_rsp_halted(p->a)) break;
            left -= (unsigned long)budget;
        }
    }
}

/* ---- IMEM rewritten under the interpreter ---- */

#define CHECK_EQ(got, want)                                                                                         \
    do {                                                                                                            \
        unsigned long long got_ = (unsigned long long)(got), want_ = (unsigned long long)(want);                    \
        g_checks++;                                                                                                 \
        if (got_ != want_) {                                                                                        \
            fprintf(stderr, "%s:%d: %s = 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #got, got_, want_);        \
            g_failures++;                                                                                           \
        }                                                                                                           \
    } while (0)

#define ADDIU(t, s, i) enc_i(0x09, t, s, (uint32_t)(i))
#define ORI(t, s, i) enc_i(0x0D, t, s, i)
#define XORI(t, s, i) enc_i(0x0E, t, s, i)
#define LUI(t, i) enc_i(0x0F, t, 0, i)
#define SW(t, s, i) enc_i(0x2B, t, s, (uint32_t)(i))
#define BNE(s, t, off) enc_i(0x05, t, s, (uint32_t)(off))
#define J(addr) ((0x02u << 26) | (((uint32_t)(addr) >> 2) & 0x03FFFFFFu))
#define MTC0(t, c) ((0x10u << 26) | (0x04u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(c) << 11))
#define SQV(vt, e, off, base) enc_vmem(0x3A, 0x04, vt, base, e, (uint32_t)(off))
#define BREAK 0x0000000Du
#define NOP 0u

static void put_words(uint8_t *mem, uint32_t at, const uint32_t *words, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) n64_put_be32(mem + ((at + 4u * (uint32_t)i) & 0xFFCu), words[i]);
}

static void start_at(N64Rsp *rsp, uint32_t pc)
{
    n64_rsp_reg_write(rsp, N64_SP_REG_PC, pc);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x1u | 0x4u); /* clear HALT and BROKE */
}

static void test_imem_rewritten(N64Rsp *rsp, uint8_t *rdram)
{
    unsigned i;

    /* The bus writes IMEM directly (n64_bus.c stores into rsp->imem and
     * tells nobody). A word that has run and is then replaced by a
     * different instruction runs as the new one. */
    {
        const uint32_t first[] = {ORI(1, 0, 0x0055), ADDIU(2, 0, 7), BREAK};
        const uint32_t second[] = {LUI(1, 0x1234), XORI(2, 2, 0x00F0), NOP, ADDIU(3, 0, 9), BREAK};
        n64_rsp_reset(rsp);
        put_words(rsp->imem, 0, first, COUNT(first));
        start_at(rsp, 0);
        CHECK_EQ(n64_rsp_run(rsp, 100), 3);
        CHECK_EQ(rsp->r[1], 0x55);
        CHECK_EQ(rsp->r[2], 7);
        put_words(rsp->imem, 0, second, COUNT(second));
        start_at(rsp, 0);
        CHECK_EQ(n64_rsp_run(rsp, 100), 5);
        CHECK_EQ(rsp->r[1], 0x12340000u);
        CHECK_EQ(rsp->r[2], 7 ^ 0xF0);
        CHECK_EQ(rsp->r[3], 9);
        /* One byte of one word, as an 8-bit bus write does: ADDIU r3,r0,9
         * becomes ADDIU r3,r0,0x109. */
        rsp->imem[3 * 4 + 2] = 0x01;
        start_at(rsp, 0);
        CHECK_EQ(n64_rsp_run(rsp, 100), 5);
        CHECK_EQ(rsp->r[3], 0x109);
    }

    /* A word ahead of the PC replaced between two calls of one run, in a
     * loop whose every word has already run: the loop adds 1 to r1 each
     * time round until the bus turns the add into an add of 0x100. */
    {
        const uint32_t loop[] = {ADDIU(1, 1, 1), ADDIU(2, 2, 1), BNE(2, 3, -3), NOP, BREAK};
        const uint32_t patch = ADDIU(1, 1, 0x100), xor_patch = XORI(1, 1, 0xFFFF);
        n64_rsp_reset(rsp);
        put_words(rsp->imem, 0x40, loop, COUNT(loop));
        rsp->r[3] = 10;
        start_at(rsp, 0x40);
        CHECK_EQ(n64_rsp_run(rsp, 4 * 4 + 1), 4 * 4 + 1); /* four times round, then the first add again */
        CHECK_EQ(rsp->r[1], 5);
        CHECK_EQ(rsp->pc, 0x44);
        put_words(rsp->imem, 0x40, &patch, 1);
        CHECK_EQ(n64_rsp_run(rsp, 3 + 4), 3 + 4); /* the rest of the fifth pass and the sixth */
        CHECK_EQ(rsp->r[1], 0x105);
        CHECK_EQ(rsp->r[2], 6);
        put_words(rsp->imem, 0x40, &xor_patch, 1); /* a different instruction altogether */
        CHECK_EQ(n64_rsp_run(rsp, 4), 4);
        CHECK_EQ(rsp->r[1], 0x105 ^ 0xFFFF);
    }

    /* A DMA the CPU starts into IMEM over a program that has run. */
    {
        const uint32_t first[] = {ADDIU(1, 0, 1), ADDIU(2, 0, 2), ADDIU(3, 0, 3), BREAK};
        const uint32_t second[] = {LUI(1, 0xAAAA), ORI(2, 0, 0xBBBB), XORI(3, 3, 0x3), BREAK};
        n64_rsp_reset(rsp);
        put_words(rsp->imem, 0x100, first, COUNT(first));
        start_at(rsp, 0x100);
        CHECK_EQ(n64_rsp_run(rsp, 100), 4);
        CHECK_EQ(rsp->r[3], 3);
        put_words(rdram, 0x800, second, COUNT(second));
        n64_rsp_reg_write(rsp, N64_SP_REG_MEM_ADDR, 0x1100);
        n64_rsp_reg_write(rsp, N64_SP_REG_DRAM_ADDR, 0x800);
        n64_rsp_reg_write(rsp, N64_SP_REG_RD_LEN, 16 - 1);
        start_at(rsp, 0x100);
        CHECK_EQ(n64_rsp_run(rsp, 100), 4);
        CHECK_EQ(rsp->r[1], 0xAAAA0000u);
        CHECK_EQ(rsp->r[2], 0xBBBB);
        CHECK_EQ(rsp->r[3], 0);
    }

    /* A DMA the RSP starts itself, over the instructions just ahead of it.
     * The program runs once with its original tail (so every word is
     * decoded), then again with r10 set so that it loads a new tail from
     * RDRAM before reaching it. */
    {
        const uint32_t prog[] = {
            BNE(10, 0, 3),      /* 0x200: r10 != 0: go and load */
            NOP,
            J(0x220),           /* 0x208: straight to the tail */
            NOP,
            MTC0(11, 0),        /* 0x210: SP_MEM_ADDR = 0x1220 */
            MTC0(12, 1),        /* 0x214: SP_DRAM_ADDR = 0x900 */
            MTC0(13, 2),        /* 0x218: SP_RD_LEN = 15: the DMA happens here */
            NOP,                /* 0x21C */
            ADDIU(1, 0, 0x11),  /* 0x220: the tail */
            ADDIU(2, 0, 0x22),
            ADDIU(3, 0, 0x33),
            BREAK,
        };
        const uint32_t tail[] = {LUI(1, 0x00C0), XORI(2, 2, 0xFF), ORI(4, 0, 0x44), BREAK};
        n64_rsp_reset(rsp);
        put_words(rsp->imem, 0x200, prog, COUNT(prog));
        put_words(rdram, 0x900, tail, COUNT(tail));
        rsp->r[11] = 0x1220;
        rsp->r[12] = 0x900;
        rsp->r[13] = 15;
        start_at(rsp, 0x200);
        CHECK_EQ(n64_rsp_run(rsp, 100), 8);
        CHECK_EQ(rsp->r[1], 0x11);
        CHECK_EQ(rsp->r[2], 0x22);
        CHECK_EQ(rsp->r[3], 0x33);
        rsp->r[10] = 1;
        start_at(rsp, 0x200);
        CHECK_EQ(n64_rsp_run(rsp, 100), 10);
        CHECK_EQ(rsp->r[1], 0x00C00000u);
        CHECK_EQ(rsp->r[2], 0x22 ^ 0xFF);
        CHECK_EQ(rsp->r[3], 0x33);
        CHECK_EQ(rsp->r[4], 0x44);
        CHECK_EQ(n64_be32(rsp->imem + 0x220), tail[0]);
    }

    /* The RSP's own stores cannot rewrite IMEM: an address with bit 12 set
     * (IMEM, in the CPU's and the DMA's numbering) is a DMEM address to
     * them, and a store across 0xFFF wraps to DMEM 0. */
    {
        const uint32_t prog[] = {
            SW(1, 2, 8),     /* r2 = 0x1000: DMEM 0x008, not IMEM 0x008 (the NOP below) */
            SW(1, 3, 0),     /* r3 = 0xFFE: DMEM 0xFFE, 0xFFF, 0x000, 0x001 */
            NOP,
            SQV(1, 0, 0, 4), /* r4 = 0x1FF8: the first half of v1 to DMEM 0xFF8-0xFFF */
            BREAK,
        };
        uint8_t imem[0x1000];
        n64_rsp_reset(rsp);
        put_words(rsp->imem, 0, prog, COUNT(prog));
        memcpy(imem, rsp->imem, sizeof imem);
        rsp->r[1] = 0xDEADBEEFu;
        rsp->r[2] = 0x1000;
        rsp->r[3] = 0xFFE;
        for (i = 0; i < 8; i++) rsp->vu.v[1][i] = (uint16_t)(0x1011u * (i + 1u));
        rsp->r[4] = 0x1FF8;
        start_at(rsp, 0);
        CHECK_EQ(n64_rsp_run(rsp, 100), 5);
        CHECK_EQ(memcmp(imem, rsp->imem, sizeof imem), 0);
        CHECK_EQ(n64_be32(rsp->dmem + 8), 0xDEADBEEFu);
        CHECK_EQ(n64_be16(rsp->dmem + 0xFFE), 0x1011u * 4u); /* the SQV came after the SW */
        CHECK_EQ(n64_be16(rsp->dmem + 0), 0xBEEF);
        CHECK_EQ(n64_be16(rsp->dmem + 0xFF8), 0x1011);
    }
}

int main(void)
{
    Pair pair;
    memset(&pair, 0, sizeof pair);
    pair.a = (N64Rsp *)calloc(1, sizeof(N64Rsp));
    pair.b = (N64Rsp *)calloc(1, sizeof(N64Rsp));
    pair.rdram_a = (uint8_t *)calloc(1, PAIR_RDRAM);
    pair.rdram_b = (uint8_t *)calloc(1, PAIR_RDRAM);
    if (!pair.a || !pair.b || !pair.rdram_a || !pair.rdram_b) return 2;
    pair_link(&pair);

    test_clz();
    test_computational_edges(pair.a, pair.b);
    test_computational_random(pair.a, pair.b);
    test_vector_memory(pair.a, pair.b);
    test_scalar_memory(pair.a, pair.b);
    test_single_instructions(&pair);
    test_programs(&pair);
    test_imem_rewritten(pair.b, pair.rdram_b);

    free(pair.rdram_b);
    free(pair.rdram_a);
    free(pair.b);
    free(pair.a);
    if (g_failures) {
        fprintf(stderr, "n64_rsp_fast: %d of %lu checks failed\n", g_failures, g_checks);
        return 1;
    }
#if defined(N64_RSP_SSE2)
    printf("n64_rsp_fast: %lu checks passed (SSE2 vector unit)\n", g_checks);
#elif defined(N64_RSP_FAST_MEM)
    printf("n64_rsp_fast: %lu checks passed (reference vector unit, whole-lane loads and stores)\n", g_checks);
#else
    printf("n64_rsp_fast: %lu checks passed (reference handlers only)\n", g_checks);
#endif
    return 0;
}
