/*
 * Every Gekko opcode this core implements, paired with the handler that
 * implements it.
 *
 * One list, used three ways: the interpreter builds its dispatch switch
 * from it, the recompiler's emitter builds a table of handler *names* from
 * it, and anything that wants to know whether an opcode is implemented at
 * all asks it. The alternative - a switch in the interpreter and a
 * separate table in the emitter - is precisely how an emitter and an
 * interpreter drift apart, which is the mistake ADDING-A-CONSOLE.md warns
 * about from the Game Boy work.
 *
 * An opcode absent from this list is one the core does not implement. The
 * interpreter stops on it and the emitter refuses to recompile the block,
 * which is the fail-closed answer: a silently skipped instruction shows up
 * much later as a wrong frame.
 *
 * Generated once from the hand-written dispatch that came before it, and
 * maintained here since. Adding an opcode means adding a line here and a
 * handler in gc_cpu_impl.h or gc_fpu_impl.h - and nothing else.
 */
#ifndef GC_CPU_OPS_H
#define GC_CPU_OPS_H

#include <stdint.h>

/* X(enum, handler) */
#define GC_CPU_OPS(X) \
  X(GC_OP_ADD,        gc_op_add) \
  X(GC_OP_ADDC,       gc_op_addc) \
  X(GC_OP_ADDE,       gc_op_adde) \
  X(GC_OP_ADDI,       gc_op_addi) \
  X(GC_OP_ADDIC,      gc_op_addic) \
  X(GC_OP_ADDIC_,     gc_op_addic_) \
  X(GC_OP_ADDIS,      gc_op_addis) \
  X(GC_OP_ADDME,      gc_op_addme) \
  X(GC_OP_ADDZE,      gc_op_addze) \
  X(GC_OP_SUBF,       gc_op_subf) \
  X(GC_OP_SUBFC,      gc_op_subfc) \
  X(GC_OP_SUBFE,      gc_op_subfe) \
  X(GC_OP_SUBFIC,     gc_op_subfic) \
  X(GC_OP_SUBFME,     gc_op_subfme) \
  X(GC_OP_SUBFZE,     gc_op_subfze) \
  X(GC_OP_NEG,        gc_op_neg) \
  X(GC_OP_MULLI,      gc_op_mulli) \
  X(GC_OP_MULLW,      gc_op_mullw) \
  X(GC_OP_MULHW,      gc_op_mulhw) \
  X(GC_OP_MULHWU,     gc_op_mulhwu) \
  X(GC_OP_DIVW,       gc_op_divw) \
  X(GC_OP_DIVWU,      gc_op_divwu) \
  X(GC_OP_CMP,        gc_op_cmp) \
  X(GC_OP_CMPI,       gc_op_cmpi) \
  X(GC_OP_CMPL,       gc_op_cmpl) \
  X(GC_OP_CMPLI,      gc_op_cmpli) \
  X(GC_OP_AND,        gc_op_and) \
  X(GC_OP_ANDC,       gc_op_andc) \
  X(GC_OP_ANDI_,      gc_op_andi_) \
  X(GC_OP_ANDIS_,     gc_op_andis_) \
  X(GC_OP_OR,         gc_op_or) \
  X(GC_OP_ORC,        gc_op_orc) \
  X(GC_OP_ORI,        gc_op_ori) \
  X(GC_OP_ORIS,       gc_op_oris) \
  X(GC_OP_XOR,        gc_op_xor) \
  X(GC_OP_XORI,       gc_op_xori) \
  X(GC_OP_XORIS,      gc_op_xoris) \
  X(GC_OP_NAND,       gc_op_nand) \
  X(GC_OP_NOR,        gc_op_nor) \
  X(GC_OP_EQV,        gc_op_eqv) \
  X(GC_OP_EXTSB,      gc_op_extsb) \
  X(GC_OP_EXTSH,      gc_op_extsh) \
  X(GC_OP_CNTLZW,     gc_op_cntlzw) \
  X(GC_OP_RLWIMI,     gc_op_rlwimi) \
  X(GC_OP_RLWINM,     gc_op_rlwinm) \
  X(GC_OP_RLWNM,      gc_op_rlwnm) \
  X(GC_OP_SLW,        gc_op_slw) \
  X(GC_OP_SRW,        gc_op_srw) \
  X(GC_OP_SRAW,       gc_op_sraw) \
  X(GC_OP_SRAWI,      gc_op_srawi) \
  X(GC_OP_CRAND,      gc_op_crand) \
  X(GC_OP_CRANDC,     gc_op_crandc) \
  X(GC_OP_CREQV,      gc_op_creqv) \
  X(GC_OP_CRNAND,     gc_op_crnand) \
  X(GC_OP_CRNOR,      gc_op_crnor) \
  X(GC_OP_CROR,       gc_op_cror) \
  X(GC_OP_CRORC,      gc_op_crorc) \
  X(GC_OP_CRXOR,      gc_op_crxor) \
  X(GC_OP_MCRF,       gc_op_mcrf) \
  X(GC_OP_MCRXR,      gc_op_mcrxr) \
  X(GC_OP_MFCR,       gc_op_mfcr) \
  X(GC_OP_MTCRF,      gc_op_mtcrf) \
  X(GC_OP_B,          gc_op_b) \
  X(GC_OP_BC,         gc_op_bc) \
  X(GC_OP_BCLR,       gc_op_bclr) \
  X(GC_OP_BCCTR,      gc_op_bcctr) \
  X(GC_OP_SC,         gc_op_sc) \
  X(GC_OP_RFI,        gc_op_rfi) \
  X(GC_OP_TW,         gc_op_tw) \
  X(GC_OP_TWI,        gc_op_twi) \
  X(GC_OP_MFSPR,      gc_op_mfspr) \
  X(GC_OP_MTSPR,      gc_op_mtspr) \
  X(GC_OP_MFTB,       gc_op_mftb) \
  X(GC_OP_MFMSR,      gc_op_mfmsr) \
  X(GC_OP_MTMSR,      gc_op_mtmsr) \
  X(GC_OP_MFSR,       gc_op_mfsr) \
  X(GC_OP_MTSR,       gc_op_mtsr) \
  X(GC_OP_MFSRIN,     gc_op_mfsrin) \
  X(GC_OP_MTSRIN,     gc_op_mtsrin) \
  X(GC_OP_SYNC,       gc_op_sync) \
  X(GC_OP_ISYNC,      gc_op_isync) \
  X(GC_OP_EIEIO,      gc_op_eieio) \
  X(GC_OP_TLBIE,      gc_op_tlbie) \
  X(GC_OP_TLBSYNC,    gc_op_tlbsync) \
  X(GC_OP_DCBT,       gc_op_dcbt) \
  X(GC_OP_DCBTST,     gc_op_dcbtst) \
  X(GC_OP_DCBST,      gc_op_dcbst) \
  X(GC_OP_DCBF,       gc_op_dcbf) \
  X(GC_OP_DCBI,       gc_op_dcbi) \
  X(GC_OP_DCBZ,       gc_op_dcbz) \
  X(GC_OP_DCBZ_L,     gc_op_dcbz_l) \
  X(GC_OP_ICBI,       gc_op_icbi) \
  X(GC_OP_LBZ,        gc_op_lbz) \
  X(GC_OP_LBZU,       gc_op_lbzu) \
  X(GC_OP_LBZX,       gc_op_lbzx) \
  X(GC_OP_LBZUX,      gc_op_lbzux) \
  X(GC_OP_LHZ,        gc_op_lhz) \
  X(GC_OP_LHZU,       gc_op_lhzu) \
  X(GC_OP_LHZX,       gc_op_lhzx) \
  X(GC_OP_LHZUX,      gc_op_lhzux) \
  X(GC_OP_LHA,        gc_op_lha) \
  X(GC_OP_LHAU,       gc_op_lhau) \
  X(GC_OP_LHAX,       gc_op_lhax) \
  X(GC_OP_LHAUX,      gc_op_lhaux) \
  X(GC_OP_LWZ,        gc_op_lwz) \
  X(GC_OP_LWZU,       gc_op_lwzu) \
  X(GC_OP_LWZX,       gc_op_lwzx) \
  X(GC_OP_LWZUX,      gc_op_lwzux) \
  X(GC_OP_LHBRX,      gc_op_lhbrx) \
  X(GC_OP_LWBRX,      gc_op_lwbrx) \
  X(GC_OP_STB,        gc_op_stb) \
  X(GC_OP_STBU,       gc_op_stbu) \
  X(GC_OP_STBX,       gc_op_stbx) \
  X(GC_OP_STBUX,      gc_op_stbux) \
  X(GC_OP_STH,        gc_op_sth) \
  X(GC_OP_STHU,       gc_op_sthu) \
  X(GC_OP_STHX,       gc_op_sthx) \
  X(GC_OP_STHUX,      gc_op_sthux) \
  X(GC_OP_STW,        gc_op_stw) \
  X(GC_OP_STWU,       gc_op_stwu) \
  X(GC_OP_STWX,       gc_op_stwx) \
  X(GC_OP_STWUX,      gc_op_stwux) \
  X(GC_OP_STHBRX,     gc_op_sthbrx) \
  X(GC_OP_STWBRX,     gc_op_stwbrx) \
  X(GC_OP_LMW,        gc_op_lmw) \
  X(GC_OP_STMW,       gc_op_stmw) \
  X(GC_OP_LSWI,       gc_op_lswi) \
  X(GC_OP_LSWX,       gc_op_lswx) \
  X(GC_OP_STSWI,      gc_op_stswi) \
  X(GC_OP_STSWX,      gc_op_stswx) \
  X(GC_OP_LWARX,      gc_op_lwarx) \
  X(GC_OP_STWCX_,     gc_op_stwcx_) \
  X(GC_OP_FADD,       gc_op_fadd) \
  X(GC_OP_FADDS,      gc_op_fadds) \
  X(GC_OP_FSUB,       gc_op_fsub) \
  X(GC_OP_FSUBS,      gc_op_fsubs) \
  X(GC_OP_FMUL,       gc_op_fmul) \
  X(GC_OP_FMULS,      gc_op_fmuls) \
  X(GC_OP_FDIV,       gc_op_fdiv) \
  X(GC_OP_FDIVS,      gc_op_fdivs) \
  X(GC_OP_FMADD,      gc_op_fmadd) \
  X(GC_OP_FMADDS,     gc_op_fmadds) \
  X(GC_OP_FMSUB,      gc_op_fmsub) \
  X(GC_OP_FMSUBS,     gc_op_fmsubs) \
  X(GC_OP_FNMADD,     gc_op_fnmadd) \
  X(GC_OP_FNMADDS,    gc_op_fnmadds) \
  X(GC_OP_FNMSUB,     gc_op_fnmsub) \
  X(GC_OP_FNMSUBS,    gc_op_fnmsubs) \
  X(GC_OP_FMR,        gc_op_fmr) \
  X(GC_OP_FNEG,       gc_op_fneg) \
  X(GC_OP_FABS,       gc_op_fabs) \
  X(GC_OP_FNABS,      gc_op_fnabs) \
  X(GC_OP_FRSP,       gc_op_frsp) \
  X(GC_OP_FCTIW,      gc_op_fctiw) \
  X(GC_OP_FCTIWZ,     gc_op_fctiwz) \
  X(GC_OP_FSEL,       gc_op_fsel) \
  X(GC_OP_FRES,       gc_op_fres) \
  X(GC_OP_FRSQRTE,    gc_op_frsqrte) \
  X(GC_OP_FCMPU,      gc_op_fcmpu) \
  X(GC_OP_FCMPO,      gc_op_fcmpo) \
  X(GC_OP_MFFS,       gc_op_mffs) \
  X(GC_OP_MTFSF,      gc_op_mtfsf) \
  X(GC_OP_MTFSFI,     gc_op_mtfsfi) \
  X(GC_OP_MTFSB0,     gc_op_mtfsb0) \
  X(GC_OP_MTFSB1,     gc_op_mtfsb1) \
  X(GC_OP_MCRFS,      gc_op_mcrfs) \
  X(GC_OP_LFS,        gc_op_lfs) \
  X(GC_OP_LFSU,       gc_op_lfsu) \
  X(GC_OP_LFSX,       gc_op_lfsx) \
  X(GC_OP_LFSUX,      gc_op_lfsux) \
  X(GC_OP_LFD,        gc_op_lfd) \
  X(GC_OP_LFDU,       gc_op_lfdu) \
  X(GC_OP_LFDX,       gc_op_lfdx) \
  X(GC_OP_LFDUX,      gc_op_lfdux) \
  X(GC_OP_STFS,       gc_op_stfs) \
  X(GC_OP_STFSU,      gc_op_stfsu) \
  X(GC_OP_STFSX,      gc_op_stfsx) \
  X(GC_OP_STFSUX,     gc_op_stfsux) \
  X(GC_OP_STFD,       gc_op_stfd) \
  X(GC_OP_STFDU,      gc_op_stfdu) \
  X(GC_OP_STFDX,      gc_op_stfdx) \
  X(GC_OP_STFDUX,     gc_op_stfdux) \
  X(GC_OP_STFIWX,     gc_op_stfiwx) \
  X(GC_OP_PS_ADD,     gc_op_ps_add) \
  X(GC_OP_PS_SUB,     gc_op_ps_sub) \
  X(GC_OP_PS_MUL,     gc_op_ps_mul) \
  X(GC_OP_PS_DIV,     gc_op_ps_div) \
  X(GC_OP_PS_MULS0,   gc_op_ps_muls0) \
  X(GC_OP_PS_MULS1,   gc_op_ps_muls1) \
  X(GC_OP_PS_MADD,    gc_op_ps_madd) \
  X(GC_OP_PS_MSUB,    gc_op_ps_msub) \
  X(GC_OP_PS_NMADD,   gc_op_ps_nmadd) \
  X(GC_OP_PS_NMSUB,   gc_op_ps_nmsub) \
  X(GC_OP_PS_MADDS0,  gc_op_ps_madds0) \
  X(GC_OP_PS_MADDS1,  gc_op_ps_madds1) \
  X(GC_OP_PS_SUM0,    gc_op_ps_sum0) \
  X(GC_OP_PS_SUM1,    gc_op_ps_sum1) \
  X(GC_OP_PS_NEG,     gc_op_ps_neg) \
  X(GC_OP_PS_ABS,     gc_op_ps_abs) \
  X(GC_OP_PS_NABS,    gc_op_ps_nabs) \
  X(GC_OP_PS_MR,      gc_op_ps_mr) \
  X(GC_OP_PS_MERGE00, gc_op_ps_merge00) \
  X(GC_OP_PS_MERGE01, gc_op_ps_merge01) \
  X(GC_OP_PS_MERGE10, gc_op_ps_merge10) \
  X(GC_OP_PS_MERGE11, gc_op_ps_merge11) \
  X(GC_OP_PS_SEL,     gc_op_ps_sel) \
  X(GC_OP_PS_RES,     gc_op_ps_res) \
  X(GC_OP_PS_RSQRTE,  gc_op_ps_rsqrte) \
  X(GC_OP_PS_CMPU0,   gc_op_ps_cmpu0) \
  X(GC_OP_PS_CMPO0,   gc_op_ps_cmpo0) \
  X(GC_OP_PS_CMPU1,   gc_op_ps_cmpu1) \
  X(GC_OP_PS_CMPO1,   gc_op_ps_cmpo1) \
  X(GC_OP_PSQ_L,      gc_op_psq_l) \
  X(GC_OP_PSQ_LU,     gc_op_psq_lu) \
  X(GC_OP_PSQ_LX,     gc_op_psq_lx) \
  X(GC_OP_PSQ_LUX,    gc_op_psq_lux) \
  X(GC_OP_PSQ_ST,     gc_op_psq_st) \
  X(GC_OP_PSQ_STU,    gc_op_psq_stu) \
  X(GC_OP_PSQ_STX,    gc_op_psq_stx) \
  X(GC_OP_PSQ_STUX,   gc_op_psq_stux)

/* Whether an instruction needs the floating-point unit.
 *
 * With MSR[FP] clear such an instruction does not run: the processor takes
 * the floating-point unavailable exception (vector 0x800) with SRR0 at the
 * instruction, so that it runs when the handler has switched the unit on
 * and returned. An operating system leans on this to save and load the
 * floating-point registers only when they are wanted: every exception
 * clears the bit, a thread that is switched to starts without it, and the
 * first floating-point instruction afterwards is what brings the thread's
 * registers back. A core that lets the instruction run regardless leaves
 * every thread and every interrupt handler sharing one set of registers,
 * and a game computes with another thread's numbers a few times a minute.
 *
 * By encoding, so that the interpreter (gc_cpu_step) and the recompiler's
 * emitter (a block that holds one checks the bit where it is entered) ask
 * the same question: the floating-point loads and stores, primary opcodes
 * 59 and 63, and the paired singles - all of primary opcode 4 but dcbz_l,
 * which is a cache operation that lives there. */
static inline int gc_insn_needs_fp(uint32_t insn) {
  switch (insn >> 26) {
    case 4:
      return ((insn >> 1) & 0x3ffu) != 1014u;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: /* lfs to stfdu */
    case 56: case 57: case 60: case 61:                                     /* psq_l, psq_lu, psq_st, psq_stu */
    case 59: case 63:
      return 1;
    case 31:
      switch ((insn >> 1) & 0x3ffu) {
        case 535: case 567: case 599: case 631: /* lfsx, lfsux, lfdx, lfdux */
        case 663: case 695: case 727: case 759: /* stfsx, stfsux, stfdx, stfdux */
        case 983:                               /* stfiwx */
          return 1;
        default:
          return 0;
      }
    default:
      return 0;
  }
}

#endif /* GC_CPU_OPS_H */
