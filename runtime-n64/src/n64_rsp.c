/*
 * The RSP interpreter, its SP registers and its DMA engine. Instruction
 * semantics are in n64_rsp_impl.h; this file decodes each IMEM word once,
 * calls its handler and advances the PC through branch delay slots.
 *
 * Timing is one cycle per instruction. ares models the dual-issue pipeline
 * and its stalls, and DMA taking time; neither is modelled here, so a
 * DMA started by the RSP has finished before its next instruction and
 * DMA_BUSY and DMA_FULL always read 0.
 *
 * Ported from ares (https://github.com/ares-emulator/ares) ares/n64/rsp
 * (interpreter.cpp, rsp.cpp, dma.cpp, io.cpp). Copyright (c) 2004-2025
 * ares team, Near et al. Permission to use, copy, modify, and/or distribute
 * this software for any purpose with or without fee is hereby granted,
 * provided that the above copyright notice and this permission notice
 * appear in all copies.
 */
#include "n64_rsp_impl.h"

#include <string.h>

void n64_rsp_reset(N64Rsp *rsp)
{
    N64RspLinks links = rsp->links;
    memset(rsp, 0, sizeof(*rsp));
    rsp->links = links;
    rsp->pc = 0;
    rsp->next_pc = 4;
    rsp->status = N64_SP_STATUS_HALT;
}

/* The instructions as the decoder numbers them: X(NAME, handler), in lists
 * by what has to be done around the handler. */

/* Nothing: no scalar register is written, no branch, SP_STATUS untouched. */
#define N64_RSP_PLAIN(X)                                                                                            \
    X(SB, sb) X(SH, sh) X(SW, sw) X(MTC2, mtc2) X(CTC2, ctc2)                                                       \
    X(LBV, lbv) X(LSV, lsv) X(LLV, llv) X(LDV, ldv) X(LQV, lqv) X(LRV, lrv) X(LPV, lpv) X(LUV, luv)                 \
    X(SBV, sbv) X(SSV, ssv) X(SLV, slv) X(SDV, sdv) X(SQV, sqv) X(SRV, srv) X(SPV, spv) X(SUV, suv)                 \
    X(VMULF, vmulf) X(VMULU, vmulu) X(VMUDL, vmudl) X(VMUDM, vmudm) X(VMUDN, vmudn) X(VMUDH, vmudh)                 \
    X(VMACF, vmacf) X(VMACU, vmacu) X(VMADL, vmadl) X(VMADM, vmadm) X(VMADN, vmadn) X(VMADH, vmadh)                 \
    X(VADD, vadd) X(VSUB, vsub) X(VABS, vabs) X(VADDC, vaddc) X(VSUBC, vsubc) X(VSAR, vsar)                         \
    X(VLT, vlt) X(VEQ, veq) X(VNE, vne) X(VGE, vge) X(VMRG, vmrg)                                                   \
    X(VAND, vand) X(VNAND, vnand) X(VOR, vor) X(VNOR, vnor) X(VXOR, vxor) X(VNXOR, vnxor)                           \
    X(VRCP, vrcp) X(VRCPL, vrcpl) X(VRCPH, vrcph) X(VMOV, vmov) X(VRSQ, vrsq) X(VRSQL, vrsql) X(VRSQH, vrsqh)       \
    X(VZERO, vzero)

/* The same, for handlers that are long or need many registers: the run
 * loop calls them instead of taking them in line (see below). */
#define N64_RSP_CALLED(X)                                                                                           \
    X(LHV, lhv) X(LFV, lfv) X(LTV, ltv) X(SHV, shv) X(SFV, sfv) X(SWV, swv) X(STV, stv)                             \
    X(VRNDP, vrndp) X(VRNDN, vrndn) X(VMULQ, vmulq) X(VMACQ, vmacq) X(VCL, vcl) X(VCH, vch) X(VCR, vcr)

/* A scalar register may be written, and it may be r0. */
#define N64_RSP_SCALAR(X)                                                                                           \
    X(SLL, sll) X(SRL, srl) X(SRA, sra) X(SLLV, sllv) X(SRLV, srlv) X(SRAV, srav) X(ADDU, addu) X(SUBU, subu)       \
    X(AND, and) X(OR, or) X(XOR, xor) X(NOR, nor) X(SLT, slt) X(SLTU, sltu) X(SPECIAL_INVALID, special_invalid)     \
    X(ADDIU, addiu) X(SLTI, slti) X(SLTIU, sltiu) X(ANDI, andi) X(ORI, ori) X(XORI, xori) X(LUI, lui)               \
    X(LB, lb) X(LH, lh) X(LW, lw) X(LBU, lbu) X(LHU, lhu) X(MFC2, mfc2) X(CFC2, cfc2)

/* Branches and jumps: they read rsp->pc and may ask for a branch. */
#define N64_RSP_BRANCH(X)                                                                                           \
    X(JR, jr) X(JALR, jalr) X(BLTZ, bltz) X(BGEZ, bgez) X(BLTZAL, bltzal) X(BGEZAL, bgezal) X(J, j) X(JAL, jal)     \
    X(BEQ, beq) X(BNE, bne) X(BLEZ, blez) X(BGTZ, bgtz)

/* BREAK and COP0: they may change SP_STATUS and they call the machine. */
#define N64_RSP_STATUS(X) X(BREAK, break) X(MFC0, mfc0) X(MTC0, mtc0)

#define N64_RSP_ALL(X) N64_RSP_PLAIN(X) N64_RSP_CALLED(X) N64_RSP_SCALAR(X) N64_RSP_BRANCH(X) N64_RSP_STATUS(X)

enum {
    /* Everything that does nothing: the word 0, opcodes the RSP does not
     * implement (COP1, COP3, the likely branches, 64-bit loads and stores,
     * LWL/LWR/SWL/SWR, CACHE, LL/SC...), LWV, VNOP and VNULL. It is 0 so
     * that a cleared decoding (word 0, number 0) is a valid one. */
    N64_RSP_I_NOP = 0,
#define N64_RSP_NUMBER(NAME, handler) N64_RSP_I_##NAME,
    N64_RSP_ALL(N64_RSP_NUMBER)
#undef N64_RSP_NUMBER
    N64_RSP_I_COUNT
};

static unsigned n64_rsp_decode_special(uint32_t op)
{
    switch (op & 0x3Fu) {
    case 0x00: return N64_RSP_I_SLL;
    case 0x02: return N64_RSP_I_SRL;
    case 0x03: return N64_RSP_I_SRA;
    case 0x04: return N64_RSP_I_SLLV;
    case 0x06: return N64_RSP_I_SRLV;
    case 0x07: return N64_RSP_I_SRAV;
    case 0x08: return N64_RSP_I_JR;
    case 0x09: return N64_RSP_I_JALR;
    case 0x0D: return N64_RSP_I_BREAK;
    case 0x20: /* ADD */
    case 0x21: return N64_RSP_I_ADDU;
    case 0x22: /* SUB */
    case 0x23: return N64_RSP_I_SUBU;
    case 0x24: return N64_RSP_I_AND;
    case 0x25: return N64_RSP_I_OR;
    case 0x26: return N64_RSP_I_XOR;
    case 0x27: return N64_RSP_I_NOR;
    case 0x2A: return N64_RSP_I_SLT;
    case 0x2B: return N64_RSP_I_SLTU;
    default: return N64_RSP_I_SPECIAL_INVALID;
    }
}

static unsigned n64_rsp_decode_regimm(uint32_t op)
{
    switch (N64_RSP_RT(op)) {
    case 0x00: return N64_RSP_I_BLTZ;
    case 0x01: return N64_RSP_I_BGEZ;
    case 0x10: return N64_RSP_I_BLTZAL;
    case 0x11: return N64_RSP_I_BGEZAL;
    default: return N64_RSP_I_NOP; /* the likely and trap forms do nothing */
    }
}

static unsigned n64_rsp_decode_cop2(uint32_t op)
{
    if (!(op & (1u << 25))) {
        switch (N64_RSP_RS(op)) {
        case 0x00: return N64_RSP_I_MFC2;
        case 0x02: return N64_RSP_I_CFC2;
        case 0x04: return N64_RSP_I_MTC2;
        case 0x06: return N64_RSP_I_CTC2;
        default: return N64_RSP_I_NOP;
        }
    }
    switch (op & 0x3Fu) {
    case 0x00: return N64_RSP_I_VMULF;
    case 0x01: return N64_RSP_I_VMULU;
    case 0x02: return N64_RSP_I_VRNDP;
    case 0x03: return N64_RSP_I_VMULQ;
    case 0x04: return N64_RSP_I_VMUDL;
    case 0x05: return N64_RSP_I_VMUDM;
    case 0x06: return N64_RSP_I_VMUDN;
    case 0x07: return N64_RSP_I_VMUDH;
    case 0x08: return N64_RSP_I_VMACF;
    case 0x09: return N64_RSP_I_VMACU;
    case 0x0A: return N64_RSP_I_VRNDN;
    case 0x0B: return N64_RSP_I_VMACQ;
    case 0x0C: return N64_RSP_I_VMADL;
    case 0x0D: return N64_RSP_I_VMADM;
    case 0x0E: return N64_RSP_I_VMADN;
    case 0x0F: return N64_RSP_I_VMADH;
    case 0x10: return N64_RSP_I_VADD;
    case 0x11: return N64_RSP_I_VSUB;
    case 0x13: return N64_RSP_I_VABS;
    case 0x14: return N64_RSP_I_VADDC;
    case 0x15: return N64_RSP_I_VSUBC;
    case 0x1D: return N64_RSP_I_VSAR;
    case 0x20: return N64_RSP_I_VLT;
    case 0x21: return N64_RSP_I_VEQ;
    case 0x22: return N64_RSP_I_VNE;
    case 0x23: return N64_RSP_I_VGE;
    case 0x24: return N64_RSP_I_VCL;
    case 0x25: return N64_RSP_I_VCH;
    case 0x26: return N64_RSP_I_VCR;
    case 0x27: return N64_RSP_I_VMRG;
    case 0x28: return N64_RSP_I_VAND;
    case 0x29: return N64_RSP_I_VNAND;
    case 0x2A: return N64_RSP_I_VOR;
    case 0x2B: return N64_RSP_I_VNOR;
    case 0x2C: return N64_RSP_I_VXOR;
    case 0x2D: return N64_RSP_I_VNXOR;
    case 0x30: return N64_RSP_I_VRCP;
    case 0x31: return N64_RSP_I_VRCPL;
    case 0x32: return N64_RSP_I_VRCPH;
    case 0x33: return N64_RSP_I_VMOV;
    case 0x34: return N64_RSP_I_VRSQ;
    case 0x35: return N64_RSP_I_VRSQL;
    case 0x36: return N64_RSP_I_VRSQH;
    case 0x37: /* VNOP */
    case 0x3F: return N64_RSP_I_NOP; /* VNULL */
    default: return N64_RSP_I_VZERO;
    }
}

static unsigned n64_rsp_decode_lwc2(uint32_t op)
{
    switch (N64_RSP_RD(op)) {
    case 0x00: return N64_RSP_I_LBV;
    case 0x01: return N64_RSP_I_LSV;
    case 0x02: return N64_RSP_I_LLV;
    case 0x03: return N64_RSP_I_LDV;
    case 0x04: return N64_RSP_I_LQV;
    case 0x05: return N64_RSP_I_LRV;
    case 0x06: return N64_RSP_I_LPV;
    case 0x07: return N64_RSP_I_LUV;
    case 0x08: return N64_RSP_I_LHV;
    case 0x09: return N64_RSP_I_LFV;
    case 0x0B: return N64_RSP_I_LTV;
    default: return N64_RSP_I_NOP; /* 0x0A is LWV, which does nothing */
    }
}

static unsigned n64_rsp_decode_swc2(uint32_t op)
{
    switch (N64_RSP_RD(op)) {
    case 0x00: return N64_RSP_I_SBV;
    case 0x01: return N64_RSP_I_SSV;
    case 0x02: return N64_RSP_I_SLV;
    case 0x03: return N64_RSP_I_SDV;
    case 0x04: return N64_RSP_I_SQV;
    case 0x05: return N64_RSP_I_SRV;
    case 0x06: return N64_RSP_I_SPV;
    case 0x07: return N64_RSP_I_SUV;
    case 0x08: return N64_RSP_I_SHV;
    case 0x09: return N64_RSP_I_SFV;
    case 0x0A: return N64_RSP_I_SWV;
    case 0x0B: return N64_RSP_I_STV;
    default: return N64_RSP_I_NOP;
    }
}

/* An instruction word's number. */
static unsigned n64_rsp_decode(uint32_t op)
{
    if (op == 0) return N64_RSP_I_NOP;
    switch (op >> 26) {
    case 0x00: return n64_rsp_decode_special(op);
    case 0x01: return n64_rsp_decode_regimm(op);
    case 0x02: return N64_RSP_I_J;
    case 0x03: return N64_RSP_I_JAL;
    case 0x04: return N64_RSP_I_BEQ;
    case 0x05: return N64_RSP_I_BNE;
    case 0x06: return N64_RSP_I_BLEZ;
    case 0x07: return N64_RSP_I_BGTZ;
    case 0x08: /* ADDI */
    case 0x09: return N64_RSP_I_ADDIU;
    case 0x0A: return N64_RSP_I_SLTI;
    case 0x0B: return N64_RSP_I_SLTIU;
    case 0x0C: return N64_RSP_I_ANDI;
    case 0x0D: return N64_RSP_I_ORI;
    case 0x0E: return N64_RSP_I_XORI;
    case 0x0F: return N64_RSP_I_LUI;
    case 0x10:
        if (N64_RSP_RS(op) == 0x00) return N64_RSP_I_MFC0;
        if (N64_RSP_RS(op) == 0x04) return N64_RSP_I_MTC0;
        return N64_RSP_I_NOP;
    case 0x12: return n64_rsp_decode_cop2(op);
    case 0x20: return N64_RSP_I_LB;
    case 0x21: return N64_RSP_I_LH;
    case 0x23: /* LW */
    case 0x27: return N64_RSP_I_LW; /* LWU */
    case 0x24: return N64_RSP_I_LBU;
    case 0x25: return N64_RSP_I_LHU;
    case 0x28: return N64_RSP_I_SB;
    case 0x29: return N64_RSP_I_SH;
    case 0x2B: return N64_RSP_I_SW;
    case 0x32: return n64_rsp_decode_lwc2(op);
    case 0x3A: return n64_rsp_decode_swc2(op);
    default: return N64_RSP_I_NOP;
    }
}

/* The run loop is threaded code: each instruction number has a function
 * that runs the handler, moves the PC pair on, fetches the next word and
 * ends by calling that word's function. The calls are in tail position, so
 * an optimising compiler makes each a jump and one instruction costs its
 * handler, a fetch and one indirect branch. Where they stay calls (an
 * unoptimised build), the chain is as deep as the instructions run in one
 * go, which is why n64_rsp_run hands out at most N64_RSP_CHAIN at a time.
 * A compiler turns such a call into a jump only where the function's exit
 * is cheap, so the handlers that would give their run function a large
 * frame (N64_RSP_CALLED, and BREAK and COP0 with their DMA and callbacks)
 * are called out of line.
 *
 * The arguments stay in registers along the chain: `op` is the word being
 * run, `next` the address the instruction after it comes from, `left` how
 * many instructions may still run, this one included. rsp->pc is the
 * word's own address. The result is `left` when the chain ends: 0 when
 * all were run, more when a BREAK or COP0 instruction left HALT or SSTEP
 * set (only they can change SP_STATUS).
 *
 * The PC pair (pc, next) is what makes delay slots work: a taken branch
 * only replaces what follows `next`, so a branch in a delay slot behaves
 * as on MIPS (the first target runs one instruction, then the second).
 *
 * Each IMEM word is decoded once and its number kept beside the word it
 * came from; a fetch that finds a different word in IMEM decodes again, so
 * writes to IMEM need no notification. */
#define N64_RSP_CHAIN 512u

#if defined(_MSC_VER)
#define N64_RSP_NOINLINE __declspec(noinline)
#else
#define N64_RSP_NOINLINE __attribute__((noinline))
#endif

typedef uint64_t (*N64RspRun)(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left);

static uint64_t n64_rsp_run_nop(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left);
#define N64_RSP_DECLARE(NAME, handler)                                                                              \
    static uint64_t n64_rsp_run_##handler(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left);
N64_RSP_ALL(N64_RSP_DECLARE)
#undef N64_RSP_DECLARE

/* In the order of the instruction numbers. */
static const N64RspRun n64_rsp_run_table[N64_RSP_I_COUNT] = {
    n64_rsp_run_nop,
#define N64_RSP_ENTRY(NAME, handler) n64_rsp_run_##handler,
    N64_RSP_ALL(N64_RSP_ENTRY)
#undef N64_RSP_ENTRY
};

/* The word at rsp->pc is not the one its cache entry was decoded from (or
 * the entry is not one the decoder wrote): decode it, then run it. */
static N64_RSP_NOINLINE uint64_t n64_rsp_run_decoding(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left)
{
    N64RspDecoded *d = &rsp->decoded[(rsp->pc & 0xFFCu) >> 2];
    d->id = n64_rsp_decode(op);
    d->op = op;
    return n64_rsp_run_table[d->id](rsp, op, next, left);
}

/* Runs the instruction at pc and whatever follows it. */
N64_FORCEINLINE uint64_t n64_rsp_run_at(N64Rsp *rsp, uint32_t pc, uint32_t next, uint64_t left)
{
    const N64RspDecoded *d = &rsp->decoded[pc >> 2];
    const uint32_t op = n64_rsp_ld32(rsp->imem + pc);
    rsp->pc = pc;
    if (op != d->op || d->id >= N64_RSP_I_COUNT) return n64_rsp_run_decoding(rsp, op, next, left);
    return n64_rsp_run_table[d->id](rsp, op, next, left);
}

/* The instruction is done: the one at `next` runs now and the one at
 * `after` follows it, unless this was the last one allowed. */
N64_FORCEINLINE uint64_t n64_rsp_run_on(N64Rsp *rsp, uint32_t next, uint32_t after, uint64_t left)
{
    if (--left == 0) {
        rsp->pc = next & 0xFFCu;
        rsp->next_pc = after;
        return 0;
    }
    return n64_rsp_run_at(rsp, next & 0xFFCu, after, left);
}

static uint64_t n64_rsp_run_nop(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left)
{
    (void)op;
    return n64_rsp_run_on(rsp, next, (next + 4u) & 0xFFCu, left);
}

#define N64_RSP_DEFINE(NAME, handler)                                                                               \
    static uint64_t n64_rsp_run_##handler(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left)                   \
    {                                                                                                               \
        n64_rsp_op_##handler(rsp, op);                                                                              \
        return n64_rsp_run_on(rsp, next, (next + 4u) & 0xFFCu, left);                                               \
    }
N64_RSP_PLAIN(N64_RSP_DEFINE)
#undef N64_RSP_DEFINE

#define N64_RSP_DEFINE(NAME, handler)                                                                               \
    static N64_RSP_NOINLINE void n64_rsp_call_##handler(N64Rsp *rsp, uint32_t op) { n64_rsp_op_##handler(rsp, op); } \
    static uint64_t n64_rsp_run_##handler(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left)                   \
    {                                                                                                               \
        n64_rsp_call_##handler(rsp, op);                                                                            \
        return n64_rsp_run_on(rsp, next, (next + 4u) & 0xFFCu, left);                                               \
    }
N64_RSP_CALLED(N64_RSP_DEFINE)
#undef N64_RSP_DEFINE

#define N64_RSP_DEFINE(NAME, handler)                                                                               \
    static uint64_t n64_rsp_run_##handler(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left)                   \
    {                                                                                                               \
        n64_rsp_op_##handler(rsp, op);                                                                              \
        rsp->r[0] = 0;                                                                                              \
        return n64_rsp_run_on(rsp, next, (next + 4u) & 0xFFCu, left);                                               \
    }
N64_RSP_SCALAR(N64_RSP_DEFINE)
#undef N64_RSP_DEFINE

/* A branch that is taken leaves its target in the branch scratch; the
 * delay slot at `next` runs first. */
#define N64_RSP_DEFINE(NAME, handler)                                                                               \
    static uint64_t n64_rsp_run_##handler(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left)                   \
    {                                                                                                               \
        uint32_t after = (next + 4u) & 0xFFCu;                                                                      \
        n64_rsp_op_##handler(rsp, op);                                                                              \
        rsp->r[0] = 0;                                                                                              \
        if (rsp->branch_pending) {                                                                                  \
            after = rsp->branch_target;                                                                             \
            rsp->branch_pending = 0;                                                                                \
            rsp->branch_target = 0;                                                                                 \
        }                                                                                                           \
        return n64_rsp_run_on(rsp, next, after, left);                                                              \
    }
N64_RSP_BRANCH(N64_RSP_DEFINE)
#undef N64_RSP_DEFINE

/* n64_rsp_run_chain counts the whole chain in advance; around a handler
 * that calls the machine, the counts are what they would be had each
 * instruction been counted as it finished. */
#define N64_RSP_DEFINE(NAME, handler)                                                                               \
    static N64_RSP_NOINLINE void n64_rsp_call_##handler(N64Rsp *rsp, uint32_t op, uint64_t left)                    \
    {                                                                                                               \
        rsp->cycles -= left;                                                                                        \
        rsp->instructions -= left;                                                                                  \
        n64_rsp_op_##handler(rsp, op);                                                                              \
        rsp->r[0] = 0;                                                                                              \
        rsp->cycles += left;                                                                                        \
        rsp->instructions += left;                                                                                  \
    }                                                                                                               \
    static uint64_t n64_rsp_run_##handler(N64Rsp *rsp, uint32_t op, uint32_t next, uint64_t left)                   \
    {                                                                                                               \
        n64_rsp_call_##handler(rsp, op, left);                                                                      \
        if (rsp->status & (N64_SP_STATUS_HALT | N64_SP_STATUS_SSTEP)) {                                             \
            rsp->pc = next & 0xFFCu;                                                                                \
            rsp->next_pc = (next + 4u) & 0xFFCu;                                                                    \
            return left - 1u;                                                                                       \
        }                                                                                                           \
        return n64_rsp_run_on(rsp, next, (next + 4u) & 0xFFCu, left);                                               \
    }
N64_RSP_STATUS(N64_RSP_DEFINE)
#undef N64_RSP_DEFINE

/* Runs up to `limit` (1 to N64_RSP_CHAIN) instructions and returns how
 * many it ran. */
static uint64_t n64_rsp_run_chain(N64Rsp *rsp, uint64_t limit)
{
    uint64_t left;
    rsp->branch_pending = 0;
    rsp->branch_target = 0;
    rsp->cycles += limit;
    rsp->instructions += limit;
    left = n64_rsp_run_at(rsp, rsp->pc & 0xFFCu, rsp->next_pc, limit);
    rsp->cycles -= left;
    rsp->instructions -= left;
    return limit - left;
}

/* With SSTEP set, the RSP halts after every instruction it runs. */
uint64_t n64_rsp_run(N64Rsp *rsp, uint64_t budget)
{
    uint64_t used = 0;
    while (used < budget && !(rsp->status & N64_SP_STATUS_HALT)) {
        uint64_t limit = budget - used;
        if (limit > N64_RSP_CHAIN) limit = N64_RSP_CHAIN;
        if (rsp->status & N64_SP_STATUS_SSTEP) limit = 1;
        used += n64_rsp_run_chain(rsp, limit);
        if (rsp->status & N64_SP_STATUS_SSTEP) rsp->status |= N64_SP_STATUS_HALT;
    }
    return used;
}

/* RDRAM is reached in 8-byte units; bytes beyond rdram_size read as zero
 * and are not written. */
static void n64_rsp_dma_row(N64Rsp *rsp, int to_rdram, uint32_t *mem, uint32_t *dram, uint32_t length)
{
    uint8_t *spmem = (*mem & 0x1000u) ? rsp->imem : rsp->dmem;
    uint32_t region = *mem & 0x1000u;
    uint32_t i, k;
    for (i = 0; i <= length; i += 8u) {
        uint32_t m = *mem & 0xFF8u, d = *dram & 0xFFFFF8u;
        for (k = 0; k < 8u; k++) {
            int in_range = rsp->links.rdram && (uint64_t)d + k < rsp->links.rdram_size;
            if (to_rdram) {
                if (in_range) rsp->links.rdram[d + k] = spmem[m + k];
                if (in_range && k == 7u && rsp->links.ram_written) rsp->links.ram_written(rsp->links.user, d, 8u);
            } else {
                spmem[m + k] = in_range ? rsp->links.rdram[d + k] : 0;
            }
        }
        *mem = region | ((m + 8u) & 0xFF8u);
        *dram = (d + 8u) & 0xFFFFF8u;
    }
}

static void n64_rsp_dma(N64Rsp *rsp, int to_rdram, uint32_t value)
{
    uint32_t length = value & 0xFF8u;
    uint32_t count = (value >> 12) & 0xFFu;
    uint32_t skip = (value >> 20) & 0xFF8u;
    uint32_t mem = rsp->mem_addr & 0x1FF8u;
    uint32_t dram = rsp->dram_addr & 0xFFFFF8u;
    for (;;) {
        n64_rsp_dma_row(rsp, to_rdram, &mem, &dram, length);
        if (count == 0) break;
        count--;
        dram = (dram + skip) & 0xFFFFF8u;
    }
    rsp->dma_mem_addr = mem;
    rsp->dma_dram_addr = dram;
    rsp->dma_len = 0xFF8u | (skip << 20);
}

uint32_t n64_rsp_reg_read(N64Rsp *rsp, unsigned reg)
{
    switch (reg) {
    case N64_SP_REG_MEM_ADDR: return rsp->dma_mem_addr;
    case N64_SP_REG_DRAM_ADDR: return rsp->dma_dram_addr;
    case N64_SP_REG_RD_LEN:
    case N64_SP_REG_WR_LEN: return rsp->dma_len;
    case N64_SP_REG_STATUS: return rsp->status;
    case N64_SP_REG_DMA_FULL:
    case N64_SP_REG_DMA_BUSY: return 0;
    case N64_SP_REG_SEMAPHORE: {
        uint32_t v = rsp->semaphore;
        rsp->semaphore = 1;
        return v;
    }
    case N64_SP_REG_PC: return rsp->pc & 0xFFCu;
    default: return 0;
    }
}

/* SP_STATUS writes come in clear/set pairs; a pair with both bits set
 * changes nothing. */
static uint32_t n64_rsp_status_pair(uint32_t status, uint32_t value, unsigned clear_bit, uint32_t flag)
{
    uint32_t clr = (value >> clear_bit) & 1u, set = (value >> (clear_bit + 1u)) & 1u;
    if (clr && !set) status &= ~flag;
    if (set && !clr) status |= flag;
    return status;
}

static void n64_rsp_status_write(N64Rsp *rsp, uint32_t value)
{
    uint32_t s = rsp->status;
    unsigned i;
    s = n64_rsp_status_pair(s, value, 0, N64_SP_STATUS_HALT);
    if (value & (1u << 2)) s &= ~N64_SP_STATUS_BROKE;
    s = n64_rsp_status_pair(s, value, 5, N64_SP_STATUS_SSTEP);
    s = n64_rsp_status_pair(s, value, 7, N64_SP_STATUS_INTR_BREAK);
    for (i = 0; i < 8; i++) s = n64_rsp_status_pair(s, value, 9u + 2u * i, N64_SP_STATUS_SIG0 << i);
    rsp->status = s;
    if (rsp->links.interrupt) {
        uint32_t clr = (value >> 3) & 1u, set = (value >> 4) & 1u;
        if (clr && !set) rsp->links.interrupt(rsp->links.user, 0);
        if (set && !clr) rsp->links.interrupt(rsp->links.user, 1);
    }
}

void n64_rsp_reg_write(N64Rsp *rsp, unsigned reg, uint32_t value)
{
    switch (reg) {
    case N64_SP_REG_MEM_ADDR: rsp->mem_addr = value & 0x1FF8u; break;
    case N64_SP_REG_DRAM_ADDR: rsp->dram_addr = value & 0xFFFFF8u; break;
    case N64_SP_REG_RD_LEN:
        rsp->rd_len = value & 0xFF8FFFF8u;
        n64_rsp_dma(rsp, 0, value);
        break;
    case N64_SP_REG_WR_LEN:
        rsp->wr_len = value & 0xFF8FFFF8u;
        n64_rsp_dma(rsp, 1, value);
        break;
    case N64_SP_REG_STATUS: n64_rsp_status_write(rsp, value); break;
    case N64_SP_REG_SEMAPHORE: rsp->semaphore = 0; break;
    case N64_SP_REG_PC:
        rsp->pc = value & 0xFFCu;
        rsp->next_pc = (rsp->pc + 4u) & 0xFFCu;
        break;
    default: break; /* DMA_FULL and DMA_BUSY are read-only */
    }
}
