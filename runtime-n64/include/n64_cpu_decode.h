/*
 * The VR4300 decode classifier, shared by the interpreter (n64_cpu.c),
 * the analyzer and the emitter (recompiler-n64). One table says what each
 * operation is called, which handler runs it (n64_op_<name> in
 * n64_cpu_impl.h and n64_fpu_impl.h) and how it affects control flow, so
 * the three cannot drift apart.
 *
 * The decoding follows ares (n64/cpu/decoder.cpp and interpreter.cpp,
 * ISC): unused COP0 functions and CFC0/CTC0/BC0 do nothing, unused COP1
 * encodings raise the unimplemented-operation exception, LWC2/LDC2/SWC2/
 * SDC2 decode like COP2 on their rs field, and COP3, LWC3, SWC3 and every
 * other unused encoding are reserved instructions.
 */
#ifndef N64_CPU_DECODE_H
#define N64_CPU_DECODE_H

#include "n64_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Flow and timing classes. */
#define N64_F_BRANCH 0x0001u  /* conditional PC-relative branch with a delay slot */
#define N64_F_LIKELY 0x0002u  /* ...that skips its delay slot when not taken */
#define N64_F_JUMP 0x0004u    /* J/JAL: absolute target in the 256 MiB segment */
#define N64_F_JUMPR 0x0008u   /* JR/JALR: target from a register */
#define N64_F_LINK 0x0010u    /* writes the return address */
#define N64_F_MEM 0x0020u     /* load or store */
#define N64_F_STORE 0x0040u   /* store */
#define N64_F_SYNC 0x0080u    /* a sync point: the run loop may take events after it */
#define N64_F_FAULT 0x0100u   /* may raise an exception */
#define N64_F_EXIT 0x0200u    /* always leaves straight-line code (ERET, SYSCALL, ...) */
#define N64_F_FPU 0x0400u     /* COP1 */
#define N64_F_NOP 0x0800u     /* no effect on guest state but time */
#define N64_F_TIME 0x1000u    /* reads or writes time (Count, Compare, Random) */

/* X(name, flags) for every operation. */
#define N64_OPS(X) \
    X(invalid, N64_F_FAULT | N64_F_EXIT) \
    X(sll, 0) X(srl, 0) X(sra, 0) X(sllv, 0) X(srlv, 0) X(srav, 0) \
    X(jr, N64_F_JUMPR) X(jalr, N64_F_JUMPR | N64_F_LINK) \
    X(syscall, N64_F_FAULT | N64_F_EXIT) X(break, N64_F_FAULT | N64_F_EXIT) X(sync, N64_F_NOP) \
    X(mfhi, 0) X(mthi, 0) X(mflo, 0) X(mtlo, 0) \
    X(dsllv, N64_F_FAULT) X(dsrlv, N64_F_FAULT) X(dsrav, N64_F_FAULT) \
    X(mult, 0) X(multu, 0) X(div, 0) X(divu, 0) \
    X(dmult, N64_F_FAULT) X(dmultu, N64_F_FAULT) X(ddiv, N64_F_FAULT) X(ddivu, N64_F_FAULT) \
    X(add, N64_F_FAULT) X(addu, 0) X(sub, N64_F_FAULT) X(subu, 0) \
    X(and, 0) X(or, 0) X(xor, 0) X(nor, 0) X(slt, 0) X(sltu, 0) \
    X(dadd, N64_F_FAULT) X(daddu, N64_F_FAULT) X(dsub, N64_F_FAULT) X(dsubu, N64_F_FAULT) \
    X(tge, N64_F_FAULT) X(tgeu, N64_F_FAULT) X(tlt, N64_F_FAULT) X(tltu, N64_F_FAULT) \
    X(teq, N64_F_FAULT) X(tne, N64_F_FAULT) \
    X(dsll, N64_F_FAULT) X(dsrl, N64_F_FAULT) X(dsra, N64_F_FAULT) \
    X(dsll32, N64_F_FAULT) X(dsrl32, N64_F_FAULT) X(dsra32, N64_F_FAULT) \
    X(bltz, N64_F_BRANCH) X(bgez, N64_F_BRANCH) \
    X(bltzl, N64_F_BRANCH | N64_F_LIKELY) X(bgezl, N64_F_BRANCH | N64_F_LIKELY) \
    X(tgei, N64_F_FAULT) X(tgeiu, N64_F_FAULT) X(tlti, N64_F_FAULT) X(tltiu, N64_F_FAULT) \
    X(teqi, N64_F_FAULT) X(tnei, N64_F_FAULT) \
    X(bltzal, N64_F_BRANCH | N64_F_LINK) X(bgezal, N64_F_BRANCH | N64_F_LINK) \
    X(bltzall, N64_F_BRANCH | N64_F_LIKELY | N64_F_LINK) X(bgezall, N64_F_BRANCH | N64_F_LIKELY | N64_F_LINK) \
    X(j, N64_F_JUMP) X(jal, N64_F_JUMP | N64_F_LINK) \
    X(beq, N64_F_BRANCH) X(bne, N64_F_BRANCH) X(blez, N64_F_BRANCH) X(bgtz, N64_F_BRANCH) \
    X(addi, N64_F_FAULT) X(addiu, 0) X(slti, 0) X(sltiu, 0) X(andi, 0) X(ori, 0) X(xori, 0) X(lui, 0) \
    X(beql, N64_F_BRANCH | N64_F_LIKELY) X(bnel, N64_F_BRANCH | N64_F_LIKELY) \
    X(blezl, N64_F_BRANCH | N64_F_LIKELY) X(bgtzl, N64_F_BRANCH | N64_F_LIKELY) \
    X(daddi, N64_F_FAULT) X(daddiu, N64_F_FAULT) \
    X(ldl, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) X(ldr, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) \
    X(lb, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) X(lh, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) \
    X(lwl, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) X(lw, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) \
    X(lbu, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) X(lhu, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) \
    X(lwr, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) X(lwu, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) \
    X(sb, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) X(sh, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) \
    X(swl, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) X(sw, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) \
    X(sdl, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) X(sdr, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) \
    X(swr, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) \
    X(cache, N64_F_FAULT | N64_F_SYNC) \
    X(ll, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) X(lld, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) \
    X(ld, N64_F_MEM | N64_F_FAULT | N64_F_SYNC) \
    X(sc, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) X(scd, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) \
    X(sd, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC) \
    X(lwc1, N64_F_MEM | N64_F_FAULT | N64_F_SYNC | N64_F_FPU) X(ldc1, N64_F_MEM | N64_F_FAULT | N64_F_SYNC | N64_F_FPU) \
    X(swc1, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC | N64_F_FPU) \
    X(sdc1, N64_F_MEM | N64_F_STORE | N64_F_FAULT | N64_F_SYNC | N64_F_FPU) \
    X(mfc0, N64_F_FAULT | N64_F_SYNC | N64_F_TIME) X(dmfc0, N64_F_FAULT | N64_F_SYNC | N64_F_TIME) \
    X(mtc0, N64_F_FAULT | N64_F_SYNC | N64_F_TIME) X(dmtc0, N64_F_FAULT | N64_F_SYNC | N64_F_TIME) \
    X(cop0_nop, N64_F_NOP) \
    X(tlbr, N64_F_FAULT | N64_F_SYNC) X(tlbwi, N64_F_FAULT | N64_F_SYNC) \
    X(tlbwr, N64_F_FAULT | N64_F_SYNC | N64_F_TIME) X(tlbp, N64_F_FAULT | N64_F_SYNC) \
    X(eret, N64_F_FAULT | N64_F_EXIT | N64_F_SYNC) \
    X(mfc1, N64_F_FAULT | N64_F_FPU) X(dmfc1, N64_F_FAULT | N64_F_FPU) \
    X(cfc1, N64_F_FAULT | N64_F_FPU) X(mtc1, N64_F_FAULT | N64_F_FPU) \
    X(dmtc1, N64_F_FAULT | N64_F_FPU) X(ctc1, N64_F_FAULT | N64_F_FPU) \
    X(bc1f, N64_F_BRANCH | N64_F_FAULT | N64_F_FPU) X(bc1t, N64_F_BRANCH | N64_F_FAULT | N64_F_FPU) \
    X(bc1fl, N64_F_BRANCH | N64_F_LIKELY | N64_F_FAULT | N64_F_FPU) \
    X(bc1tl, N64_F_BRANCH | N64_F_LIKELY | N64_F_FAULT | N64_F_FPU) \
    X(cop1_unimpl, N64_F_FAULT | N64_F_FPU) \
    X(fadd_s, N64_F_FAULT | N64_F_FPU) X(fsub_s, N64_F_FAULT | N64_F_FPU) \
    X(fmul_s, N64_F_FAULT | N64_F_FPU) X(fdiv_s, N64_F_FAULT | N64_F_FPU) \
    X(fsqrt_s, N64_F_FAULT | N64_F_FPU) X(fabs_s, N64_F_FAULT | N64_F_FPU) \
    X(fmov_s, N64_F_FAULT | N64_F_FPU) X(fneg_s, N64_F_FAULT | N64_F_FPU) \
    X(fround_l_s, N64_F_FAULT | N64_F_FPU) X(ftrunc_l_s, N64_F_FAULT | N64_F_FPU) \
    X(fceil_l_s, N64_F_FAULT | N64_F_FPU) X(ffloor_l_s, N64_F_FAULT | N64_F_FPU) \
    X(fround_w_s, N64_F_FAULT | N64_F_FPU) X(ftrunc_w_s, N64_F_FAULT | N64_F_FPU) \
    X(fceil_w_s, N64_F_FAULT | N64_F_FPU) X(ffloor_w_s, N64_F_FAULT | N64_F_FPU) \
    X(fcvt_d_s, N64_F_FAULT | N64_F_FPU) X(fcvt_w_s, N64_F_FAULT | N64_F_FPU) \
    X(fcvt_l_s, N64_F_FAULT | N64_F_FPU) X(fc_s, N64_F_FAULT | N64_F_FPU) \
    X(fadd_d, N64_F_FAULT | N64_F_FPU) X(fsub_d, N64_F_FAULT | N64_F_FPU) \
    X(fmul_d, N64_F_FAULT | N64_F_FPU) X(fdiv_d, N64_F_FAULT | N64_F_FPU) \
    X(fsqrt_d, N64_F_FAULT | N64_F_FPU) X(fabs_d, N64_F_FAULT | N64_F_FPU) \
    X(fmov_d, N64_F_FAULT | N64_F_FPU) X(fneg_d, N64_F_FAULT | N64_F_FPU) \
    X(fround_l_d, N64_F_FAULT | N64_F_FPU) X(ftrunc_l_d, N64_F_FAULT | N64_F_FPU) \
    X(fceil_l_d, N64_F_FAULT | N64_F_FPU) X(ffloor_l_d, N64_F_FAULT | N64_F_FPU) \
    X(fround_w_d, N64_F_FAULT | N64_F_FPU) X(ftrunc_w_d, N64_F_FAULT | N64_F_FPU) \
    X(fceil_w_d, N64_F_FAULT | N64_F_FPU) X(ffloor_w_d, N64_F_FAULT | N64_F_FPU) \
    X(fcvt_s_d, N64_F_FAULT | N64_F_FPU) X(fcvt_w_d, N64_F_FAULT | N64_F_FPU) \
    X(fcvt_l_d, N64_F_FAULT | N64_F_FPU) X(fc_d, N64_F_FAULT | N64_F_FPU) \
    X(fcvt_s_w, N64_F_FAULT | N64_F_FPU) X(fcvt_d_w, N64_F_FAULT | N64_F_FPU) \
    X(fcvt_s_l, N64_F_FAULT | N64_F_FPU) X(fcvt_d_l, N64_F_FAULT | N64_F_FPU) \
    X(mfc2, N64_F_FAULT) X(dmfc2, N64_F_FAULT) X(cfc2, N64_F_FAULT) \
    X(mtc2, N64_F_FAULT) X(dmtc2, N64_F_FAULT) X(ctc2, N64_F_FAULT) \
    X(cop2_invalid, N64_F_FAULT | N64_F_EXIT)

#define N64_OP_ENUM(name, flags) N64_OP_##name,
typedef enum N64Op { N64_OPS(N64_OP_ENUM) N64_OP_COUNT } N64Op;
#undef N64_OP_ENUM

/* Instruction fields. */
#define N64_RS(i) (((i) >> 21) & 31u)
#define N64_RT(i) (((i) >> 16) & 31u)
#define N64_RD(i) (((i) >> 11) & 31u)
#define N64_SA(i) (((i) >> 6) & 31u)
#define N64_FT(i) (((i) >> 16) & 31u)
#define N64_FS(i) (((i) >> 11) & 31u)
#define N64_FD(i) (((i) >> 6) & 31u)
#define N64_IMM(i) ((int64_t)(int16_t)(uint16_t)(i))
#define N64_UIMM(i) ((uint64_t)((i) & 0xFFFFu))
#define N64_TARGET(i) ((i) & 0x03FFFFFFu)

/* Where a taken branch at `pc` goes, and where a J or JAL at `pc` goes. */
static inline uint64_t n64_branch_target(uint64_t pc, uint32_t i) { return pc + 4u + ((uint64_t)N64_IMM(i) << 2); }
static inline uint64_t n64_jump_target(uint64_t pc, uint32_t i)
{
    return ((pc + 4u) & ~(uint64_t)0x0FFFFFFFu) | ((uint64_t)N64_TARGET(i) << 2);
}

/* A branch or jump to itself with an empty delay slot: the idle loop a
 * game's lowest thread sits in until an interrupt. Each turn costs two
 * cycles and changes nothing else, so the run loop and generated code take
 * the turns before the next sync at once (n64_rc_idle). */
static inline bool n64_is_idle_loop(uint64_t pc, uint32_t branch, uint32_t slot)
{
    if (slot != 0) return false;
    switch (branch >> 26) {
    case 0x02: return n64_jump_target(pc, branch) == pc;                                       /* J */
    case 0x04: return N64_RS(branch) == N64_RT(branch) && N64_IMM(branch) == -1;               /* BEQ r, r */
    case 0x01: return N64_RT(branch) == 0x01 && N64_RS(branch) == 0 && N64_IMM(branch) == -1;  /* BGEZ zero */
    default: return false;
    }
}

extern const uint16_t n64_op_flags[N64_OP_COUNT];
extern const char *const n64_op_names[N64_OP_COUNT];

static inline N64Op n64_decode_cop2(uint32_t i)
{
    switch (N64_RS(i)) {
    case 0x00: return N64_OP_mfc2;
    case 0x01: return N64_OP_dmfc2;
    case 0x02: return N64_OP_cfc2;
    case 0x04: return N64_OP_mtc2;
    case 0x05: return N64_OP_dmtc2;
    case 0x06: return N64_OP_ctc2;
    default: return N64_OP_cop2_invalid;
    }
}

static inline N64Op n64_decode_cop1(uint32_t i)
{
    const uint32_t fmt = N64_RS(i), fn = i & 0x3Fu;
    switch (fmt) {
    case 0x00: return N64_OP_mfc1;
    case 0x01: return N64_OP_dmfc1;
    case 0x02: return N64_OP_cfc1;
    case 0x04: return N64_OP_mtc1;
    case 0x05: return N64_OP_dmtc1;
    case 0x06: return N64_OP_ctc1;
    case 0x08:
        switch (N64_RT(i)) {
        case 0: return N64_OP_bc1f;
        case 1: return N64_OP_bc1t;
        case 2: return N64_OP_bc1fl;
        case 3: return N64_OP_bc1tl;
        default: return N64_OP_cop1_unimpl;
        }
    case 16: /* S */
        if (fn >= 0x30) return N64_OP_fc_s;
        switch (fn) {
        case 0x00: return N64_OP_fadd_s;
        case 0x01: return N64_OP_fsub_s;
        case 0x02: return N64_OP_fmul_s;
        case 0x03: return N64_OP_fdiv_s;
        case 0x04: return N64_OP_fsqrt_s;
        case 0x05: return N64_OP_fabs_s;
        case 0x06: return N64_OP_fmov_s;
        case 0x07: return N64_OP_fneg_s;
        case 0x08: return N64_OP_fround_l_s;
        case 0x09: return N64_OP_ftrunc_l_s;
        case 0x0A: return N64_OP_fceil_l_s;
        case 0x0B: return N64_OP_ffloor_l_s;
        case 0x0C: return N64_OP_fround_w_s;
        case 0x0D: return N64_OP_ftrunc_w_s;
        case 0x0E: return N64_OP_fceil_w_s;
        case 0x0F: return N64_OP_ffloor_w_s;
        case 0x21: return N64_OP_fcvt_d_s;
        case 0x24: return N64_OP_fcvt_w_s;
        case 0x25: return N64_OP_fcvt_l_s;
        default: return N64_OP_cop1_unimpl; /* includes CVT.S.S */
        }
    case 17: /* D */
        if (fn >= 0x30) return N64_OP_fc_d;
        switch (fn) {
        case 0x00: return N64_OP_fadd_d;
        case 0x01: return N64_OP_fsub_d;
        case 0x02: return N64_OP_fmul_d;
        case 0x03: return N64_OP_fdiv_d;
        case 0x04: return N64_OP_fsqrt_d;
        case 0x05: return N64_OP_fabs_d;
        case 0x06: return N64_OP_fmov_d;
        case 0x07: return N64_OP_fneg_d;
        case 0x08: return N64_OP_fround_l_d;
        case 0x09: return N64_OP_ftrunc_l_d;
        case 0x0A: return N64_OP_fceil_l_d;
        case 0x0B: return N64_OP_ffloor_l_d;
        case 0x0C: return N64_OP_fround_w_d;
        case 0x0D: return N64_OP_ftrunc_w_d;
        case 0x0E: return N64_OP_fceil_w_d;
        case 0x0F: return N64_OP_ffloor_w_d;
        case 0x20: return N64_OP_fcvt_s_d;
        case 0x24: return N64_OP_fcvt_w_d;
        case 0x25: return N64_OP_fcvt_l_d;
        default: return N64_OP_cop1_unimpl; /* includes CVT.D.D */
        }
    case 20: /* W */
        if (fn == 0x20) return N64_OP_fcvt_s_w;
        if (fn == 0x21) return N64_OP_fcvt_d_w;
        return N64_OP_cop1_unimpl;
    case 21: /* L */
        if (fn == 0x20) return N64_OP_fcvt_s_l;
        if (fn == 0x21) return N64_OP_fcvt_d_l;
        return N64_OP_cop1_unimpl;
    default: return N64_OP_cop1_unimpl; /* includes DCFC1, DCTC1 */
    }
}

static inline N64Op n64_decode(uint32_t i)
{
    switch (i >> 26) {
    case 0x00:
        switch (i & 0x3Fu) {
        case 0x00: return N64_OP_sll;
        case 0x02: return N64_OP_srl;
        case 0x03: return N64_OP_sra;
        case 0x04: return N64_OP_sllv;
        case 0x06: return N64_OP_srlv;
        case 0x07: return N64_OP_srav;
        case 0x08: return N64_OP_jr;
        case 0x09: return N64_OP_jalr;
        case 0x0C: return N64_OP_syscall;
        case 0x0D: return N64_OP_break;
        case 0x0F: return N64_OP_sync;
        case 0x10: return N64_OP_mfhi;
        case 0x11: return N64_OP_mthi;
        case 0x12: return N64_OP_mflo;
        case 0x13: return N64_OP_mtlo;
        case 0x14: return N64_OP_dsllv;
        case 0x16: return N64_OP_dsrlv;
        case 0x17: return N64_OP_dsrav;
        case 0x18: return N64_OP_mult;
        case 0x19: return N64_OP_multu;
        case 0x1A: return N64_OP_div;
        case 0x1B: return N64_OP_divu;
        case 0x1C: return N64_OP_dmult;
        case 0x1D: return N64_OP_dmultu;
        case 0x1E: return N64_OP_ddiv;
        case 0x1F: return N64_OP_ddivu;
        case 0x20: return N64_OP_add;
        case 0x21: return N64_OP_addu;
        case 0x22: return N64_OP_sub;
        case 0x23: return N64_OP_subu;
        case 0x24: return N64_OP_and;
        case 0x25: return N64_OP_or;
        case 0x26: return N64_OP_xor;
        case 0x27: return N64_OP_nor;
        case 0x2A: return N64_OP_slt;
        case 0x2B: return N64_OP_sltu;
        case 0x2C: return N64_OP_dadd;
        case 0x2D: return N64_OP_daddu;
        case 0x2E: return N64_OP_dsub;
        case 0x2F: return N64_OP_dsubu;
        case 0x30: return N64_OP_tge;
        case 0x31: return N64_OP_tgeu;
        case 0x32: return N64_OP_tlt;
        case 0x33: return N64_OP_tltu;
        case 0x34: return N64_OP_teq;
        case 0x36: return N64_OP_tne;
        case 0x38: return N64_OP_dsll;
        case 0x3A: return N64_OP_dsrl;
        case 0x3B: return N64_OP_dsra;
        case 0x3C: return N64_OP_dsll32;
        case 0x3E: return N64_OP_dsrl32;
        case 0x3F: return N64_OP_dsra32;
        default: return N64_OP_invalid;
        }
    case 0x01:
        switch (N64_RT(i)) {
        case 0x00: return N64_OP_bltz;
        case 0x01: return N64_OP_bgez;
        case 0x02: return N64_OP_bltzl;
        case 0x03: return N64_OP_bgezl;
        case 0x08: return N64_OP_tgei;
        case 0x09: return N64_OP_tgeiu;
        case 0x0A: return N64_OP_tlti;
        case 0x0B: return N64_OP_tltiu;
        case 0x0C: return N64_OP_teqi;
        case 0x0E: return N64_OP_tnei;
        case 0x10: return N64_OP_bltzal;
        case 0x11: return N64_OP_bgezal;
        case 0x12: return N64_OP_bltzall;
        case 0x13: return N64_OP_bgezall;
        default: return N64_OP_invalid;
        }
    case 0x02: return N64_OP_j;
    case 0x03: return N64_OP_jal;
    case 0x04: return N64_OP_beq;
    case 0x05: return N64_OP_bne;
    case 0x06: return N64_OP_blez;
    case 0x07: return N64_OP_bgtz;
    case 0x08: return N64_OP_addi;
    case 0x09: return N64_OP_addiu;
    case 0x0A: return N64_OP_slti;
    case 0x0B: return N64_OP_sltiu;
    case 0x0C: return N64_OP_andi;
    case 0x0D: return N64_OP_ori;
    case 0x0E: return N64_OP_xori;
    case 0x0F: return N64_OP_lui;
    case 0x10:
        switch (N64_RS(i)) {
        case 0x00: return N64_OP_mfc0;
        case 0x01: return N64_OP_dmfc0;
        case 0x02: case 0x06: case 0x08: return N64_OP_cop0_nop; /* CFC0, CTC0, BC0 */
        case 0x04: return N64_OP_mtc0;
        case 0x05: return N64_OP_dmtc0;
        default:
            if (N64_RS(i) < 0x10) return N64_OP_invalid;
            switch (i & 0x3Fu) {
            case 0x01: return N64_OP_tlbr;
            case 0x02: return N64_OP_tlbwi;
            case 0x06: return N64_OP_tlbwr;
            case 0x08: return N64_OP_tlbp;
            case 0x10: return N64_OP_invalid; /* RFE */
            case 0x18: return N64_OP_eret;
            default: return N64_OP_cop0_nop;
            }
        }
    case 0x11: return n64_decode_cop1(i);
    case 0x12: case 0x32: case 0x36: case 0x3A: case 0x3E: return n64_decode_cop2(i);
    case 0x14: return N64_OP_beql;
    case 0x15: return N64_OP_bnel;
    case 0x16: return N64_OP_blezl;
    case 0x17: return N64_OP_bgtzl;
    case 0x18: return N64_OP_daddi;
    case 0x19: return N64_OP_daddiu;
    case 0x1A: return N64_OP_ldl;
    case 0x1B: return N64_OP_ldr;
    case 0x20: return N64_OP_lb;
    case 0x21: return N64_OP_lh;
    case 0x22: return N64_OP_lwl;
    case 0x23: return N64_OP_lw;
    case 0x24: return N64_OP_lbu;
    case 0x25: return N64_OP_lhu;
    case 0x26: return N64_OP_lwr;
    case 0x27: return N64_OP_lwu;
    case 0x28: return N64_OP_sb;
    case 0x29: return N64_OP_sh;
    case 0x2A: return N64_OP_swl;
    case 0x2B: return N64_OP_sw;
    case 0x2C: return N64_OP_sdl;
    case 0x2D: return N64_OP_sdr;
    case 0x2E: return N64_OP_swr;
    case 0x2F: return N64_OP_cache;
    case 0x30: return N64_OP_ll;
    case 0x31: return N64_OP_lwc1;
    case 0x34: return N64_OP_lld;
    case 0x35: return N64_OP_ldc1;
    case 0x37: return N64_OP_ld;
    case 0x38: return N64_OP_sc;
    case 0x39: return N64_OP_swc1;
    case 0x3C: return N64_OP_scd;
    case 0x3D: return N64_OP_sdc1;
    case 0x3F: return N64_OP_sd;
    default: return N64_OP_invalid; /* COP3, LWC3, SWC3, 0x1C-0x1F */
    }
}

/* Extra pipeline cycles an operation takes beyond its first (VR4300 user
 * manual latencies, as ares charges them). The interpreter and generated
 * code both add 1 + this for every instruction. */
static inline uint32_t n64_op_extra_cycles(N64Op op)
{
    switch (op) {
    case N64_OP_mult: case N64_OP_multu: return 4;
    case N64_OP_div: case N64_OP_divu: return 36;
    case N64_OP_dmult: case N64_OP_dmultu: return 7;
    case N64_OP_ddiv: case N64_OP_ddivu: return 68;
    default: return 0;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* N64_CPU_DECODE_H */
