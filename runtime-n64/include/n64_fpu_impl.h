/*
 * VR4300 floating-point unit (COP1) handlers, included by n64_cpu_impl.h.
 *
 * The arithmetic is the host's IEEE single and double arithmetic; the
 * exception flags come from the host's floating-point environment, read
 * straight after each operation, and the guest's rounding mode is put into
 * the host's around the operation when it is not round-to-nearest. That
 * needs strict floating-point code generation in every file that includes
 * this header (runtime-n64/CMakeLists.txt, and the generated project's
 * CMakeLists.txt): no contraction into fused multiply-adds, no motion of
 * operations across the flag reads. Operands are read through volatile
 * copies after the flags are cleared for the same reason.
 *
 * What the VR4300 adds to IEEE follows ares (n64/cpu/interpreter-fpu.cpp,
 * ISC): a denormal or quiet-NaN operand, a denormal result (unless flush
 * mode is on and neither the underflow nor the inexact exception is
 * enabled), and a conversion whose result does not fit raise the
 * unimplemented-operation exception; NaN results become 0x7FBFFFFF and
 * 0x7FF7FFFFFFFFFFFF; with Status.FR clear, the odd registers' 32-bit
 * views are the upper halves of the even ones for moves and loads, and
 * operands come from the even register. Ported from ares
 * (https://github.com/ares-emulator/ares). Copyright (c) 2004-2025 ares
 * team, Near et al. Permission to use, copy, modify, and/or distribute this
 * software for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear in all
 * copies.
 *
 * Conversions to integers do their own rounding in integer arithmetic
 * (n64_fpu_round), so they do not depend on the host's rounding mode or on
 * which C library rounding function raises "inexact".
 */
#ifndef N64_FPU_IMPL_H
#define N64_FPU_IMPL_H

#include <fenv.h>
#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#pragma fenv_access(on)
#endif

/* Flag index for the cause, enable and flag fields of FCR31. */
enum { N64_FPE_I = 0, N64_FPE_U = 1, N64_FPE_O = 2, N64_FPE_Z = 3, N64_FPE_V = 4 };
#define N64_FCR31_CAUSE_MASK 0x0003F000u
#define N64_FCR31_CAUSE_E 0x00020000u

void n64_fpu_set_host_round(uint32_t mode);

static inline float n64_f32(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static inline double n64_f64(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static inline uint32_t n64_f32_bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
static inline uint64_t n64_f64_bits(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

/* ---- register views (see the header comment) ---- */

#define N64_FR(c) (((c)->cp0_status & N64_SR_FR) != 0)

/* MTC1/MFC1/LWC1/SWC1: with FR clear an odd register is the upper half of
 * the even one. */
static inline uint32_t n64_fpr_rd_t32(const N64Cpu *c, unsigned r)
{
    if (N64_FR(c)) return (uint32_t)c->fpr[r];
    if (r & 1u) return (uint32_t)(c->fpr[r & ~1u] >> 32);
    return (uint32_t)c->fpr[r];
}
static inline void n64_fpr_wr_t32(N64Cpu *c, unsigned r, uint32_t v)
{
    if (N64_FR(c)) c->fpr[r] = (c->fpr[r] & 0xFFFFFFFF00000000ull) | v;
    else if (r & 1u) c->fpr[r & ~1u] = (c->fpr[r & ~1u] & 0xFFFFFFFFull) | ((uint64_t)v << 32);
    else c->fpr[r] = (c->fpr[r] & 0xFFFFFFFF00000000ull) | v;
}
/* An fs operand (32 bits): the even register when FR is clear. */
static inline uint32_t n64_fpr_rd_s32(const N64Cpu *c, unsigned r)
{
    return (uint32_t)c->fpr[N64_FR(c) ? r : (r & ~1u)];
}
/* An ft operand (32 bits): the register as named. */
static inline uint32_t n64_fpr_rd_ft32(const N64Cpu *c, unsigned r) { return (uint32_t)c->fpr[r]; }
/* A 32-bit result clears the upper half. */
static inline void n64_fpr_wr_d32(N64Cpu *c, unsigned r, uint32_t v) { c->fpr[r] = v; }
/* LDC1/SDC1/DMFC1/DMTC1 and fs operands (64 bits): even register when FR
 * is clear. */
static inline uint64_t n64_fpr_rd_t64(const N64Cpu *c, unsigned r) { return c->fpr[N64_FR(c) ? r : (r & ~1u)]; }
static inline void n64_fpr_wr_t64(N64Cpu *c, unsigned r, uint64_t v) { c->fpr[N64_FR(c) ? r : (r & ~1u)] = v; }
/* An ft operand or a result (64 bits): the register as named. */
static inline uint64_t n64_fpr_rd_ft64(const N64Cpu *c, unsigned r) { return c->fpr[r]; }
static inline void n64_fpr_wr_d64(N64Cpu *c, unsigned r, uint64_t v) { c->fpr[r] = v; }

/* ---- exception bookkeeping ---- */

N64_FORCEINLINE bool n64_fpe(N64Cpu *c, unsigned f)
{
    c->fcr31 |= 1u << (12u + f);
    if (c->fcr31 & (1u << (7u + f))) return true;
    c->fcr31 |= 1u << (2u + f);
    return false;
}
N64_FORCEINLINE bool n64_fpe_unimpl(N64Cpu *c)
{
    c->fcr31 |= N64_FCR31_CAUSE_E;
    return true;
}
N64_FORCEINLINE void n64_fpu_raise(N64Cpu *c) { n64_cpu_exception(c, N64_EXC_FPE, 0, false); }

/* Every computational COP1 instruction starts here: COP1 must be usable,
 * and the cause field is cleared. */
N64_FORCEINLINE bool n64_fpu_start(N64Cpu *c)
{
    if (!(c->cp0_status & N64_SR_CU1)) {
        n64_cpu_unusable(c, 1);
        return false;
    }
    c->fcr31 &= ~N64_FCR31_CAUSE_MASK;
    return true;
}

/* MIPS (legacy NaN encoding): a NaN is signalling when the top fraction
 * bit is set. */
static inline bool n64_isnan32(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu); }
static inline bool n64_isnan64(uint64_t b)
{
    return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0x000FFFFFFFFFFFFFull);
}
static inline bool n64_snan32(uint32_t b) { return n64_isnan32(b) && (b & 0x00400000u); }
static inline bool n64_snan64(uint64_t b) { return n64_isnan64(b) && (b & 0x0008000000000000ull); }
static inline bool n64_denormal32(uint32_t b) { return (b & 0x7F800000u) == 0 && (b & 0x007FFFFFu); }
static inline bool n64_denormal64(uint64_t b) { return (b & 0x7FF0000000000000ull) == 0 && (b & 0x000FFFFFFFFFFFFFull); }
static inline bool n64_isinf32(uint32_t b) { return (b & 0x7FFFFFFFu) == 0x7F800000u; }
static inline bool n64_isinf64(uint64_t b) { return (b & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }

/* One operand. */
N64_FORCEINLINE bool n64_fpu_in1_32(N64Cpu *c, uint32_t b)
{
    if (n64_denormal32(b)) {
        n64_fpe_unimpl(c);
        n64_fpu_raise(c);
        return false;
    }
    if (n64_isnan32(b)) {
        if (n64_snan32(b) ? n64_fpe(c, N64_FPE_V) : n64_fpe_unimpl(c)) {
            n64_fpu_raise(c);
            return false;
        }
    }
    return true;
}
N64_FORCEINLINE bool n64_fpu_in1_64(N64Cpu *c, uint64_t b)
{
    if (n64_denormal64(b)) {
        n64_fpe_unimpl(c);
        n64_fpu_raise(c);
        return false;
    }
    if (n64_isnan64(b)) {
        if (n64_snan64(b) ? n64_fpe(c, N64_FPE_V) : n64_fpe_unimpl(c)) {
            n64_fpu_raise(c);
            return false;
        }
    }
    return true;
}

/* Two operands: a quiet NaN or a denormal is unimplemented; a signalling
 * NaN is invalid. */
N64_FORCEINLINE bool n64_fpu_in2_32(N64Cpu *c, uint32_t a, uint32_t b)
{
    if ((n64_isnan32(a) && !n64_snan32(a)) || (n64_isnan32(b) && !n64_snan32(b)) || n64_denormal32(a) ||
        n64_denormal32(b)) {
        n64_fpe_unimpl(c);
        n64_fpu_raise(c);
        return false;
    }
    if ((n64_snan32(a) || n64_snan32(b)) && n64_fpe(c, N64_FPE_V)) {
        n64_fpu_raise(c);
        return false;
    }
    return true;
}
N64_FORCEINLINE bool n64_fpu_in2_64(N64Cpu *c, uint64_t a, uint64_t b)
{
    if ((n64_isnan64(a) && !n64_snan64(a)) || (n64_isnan64(b) && !n64_snan64(b)) || n64_denormal64(a) ||
        n64_denormal64(b)) {
        n64_fpe_unimpl(c);
        n64_fpu_raise(c);
        return false;
    }
    if ((n64_snan64(a) || n64_snan64(b)) && n64_fpe(c, N64_FPE_V)) {
        n64_fpu_raise(c);
        return false;
    }
    return true;
}

/* The host flags after an operation. Returns true when the operation must
 * not write its result (an exception was raised, or it is unimplemented). */
#define N64_HOST_FE (FE_DIVBYZERO | FE_INEXACT | FE_UNDERFLOW | FE_OVERFLOW | FE_INVALID)
N64_FORCEINLINE bool n64_fpu_host_flags(N64Cpu *c, int exc, bool conv)
{
    if (!exc) return false;
    if (conv && (exc & FE_INVALID)) {
        n64_fpe_unimpl(c);
        n64_fpu_raise(c);
        return true;
    }
    if (exc & FE_UNDERFLOW) {
        if (!(c->fcr31 & N64_FCR31_FS) || (c->fcr31 & (1u << (7u + N64_FPE_U))) ||
            (c->fcr31 & (1u << (7u + N64_FPE_I)))) {
            n64_fpe_unimpl(c);
            n64_fpu_raise(c);
            return true;
        }
    }
    bool raise = false;
    if (exc & FE_DIVBYZERO) raise |= n64_fpe(c, N64_FPE_Z);
    if (exc & FE_INEXACT) raise |= n64_fpe(c, N64_FPE_I);
    if (exc & FE_UNDERFLOW) raise |= n64_fpe(c, N64_FPE_U);
    if (exc & FE_OVERFLOW) raise |= n64_fpe(c, N64_FPE_O);
    if (exc & FE_INVALID) raise |= n64_fpe(c, N64_FPE_V);
    if (raise) n64_fpu_raise(c);
    return raise;
}

/* The result stage: denormal results are unimplemented unless flushing is
 * allowed; NaN results become the default NaN. */
N64_FORCEINLINE bool n64_fpu_out32(N64Cpu *c, uint32_t *b)
{
    if (n64_denormal32(*b)) {
        if (!(c->fcr31 & N64_FCR31_FS) || (c->fcr31 & ((1u << (7u + N64_FPE_U)) | (1u << (7u + N64_FPE_I))))) {
            n64_fpe_unimpl(c);
            n64_fpu_raise(c);
            return false;
        }
        (void)n64_fpe(c, N64_FPE_U);
        (void)n64_fpe(c, N64_FPE_I);
        uint32_t sign = *b & 0x80000000u;
        switch (c->fcr31 & 3u) {
        case 2: *b = sign ? 0x80000000u : 0x00800000u; break; /* toward +inf */
        case 3: *b = sign ? 0x80800000u : 0u; break;          /* toward -inf */
        default: *b = sign; break;
        }
        return true;
    }
    if (n64_isnan32(*b)) *b = 0x7FBFFFFFu;
    return true;
}
N64_FORCEINLINE bool n64_fpu_out64(N64Cpu *c, uint64_t *b)
{
    if (n64_denormal64(*b)) {
        if (!(c->fcr31 & N64_FCR31_FS) || (c->fcr31 & ((1u << (7u + N64_FPE_U)) | (1u << (7u + N64_FPE_I))))) {
            n64_fpe_unimpl(c);
            n64_fpu_raise(c);
            return false;
        }
        (void)n64_fpe(c, N64_FPE_U);
        (void)n64_fpe(c, N64_FPE_I);
        uint64_t sign = *b & 0x8000000000000000ull;
        switch (c->fcr31 & 3u) {
        case 2: *b = sign ? 0x8000000000000000ull : 0x0010000000000000ull; break;
        case 3: *b = sign ? 0x8010000000000000ull : 0u; break;
        default: *b = sign; break;
        }
        return true;
    }
    if (n64_isnan64(*b)) *b = 0x7FF7FFFFFFFFFFFFull;
    return true;
}

/* Run EXPR (which reads its operands from volatile locals declared before
 * the macro) under the guest's rounding mode, and collect the host flags
 * into `exc`. */
#define N64_FPU_EVAL(c, type, res, exc, expr) \
    volatile type res; \
    int exc; \
    { \
        uint32_t n64_rm_ = (c)->fcr31 & 3u; \
        if (n64_rm_) n64_fpu_set_host_round(n64_rm_); \
        res = (expr); \
        exc = fetestexcept(N64_HOST_FE); \
        if (n64_rm_) n64_fpu_set_host_round(0); \
    }

/* ---- arithmetic ---- */

#define N64_FPU_BIN32(name, op, extra) \
    N64_OP n64_op_##name(N64Cpu *c, uint32_t i) \
    { \
        if (!n64_fpu_start(c)) return; \
        uint32_t ab = n64_fpr_rd_s32(c, N64_FS(i)), bb = n64_fpr_rd_ft32(c, N64_FT(i)); \
        if (!n64_fpu_in2_32(c, ab, bb)) return; \
        feclearexcept(N64_HOST_FE); \
        volatile float a = n64_f32(ab), b = n64_f32(bb); \
        N64_FPU_EVAL(c, float, r, exc, a op b) \
        if (n64_fpu_host_flags(c, exc, false)) return; \
        uint32_t rb = n64_f32_bits(r); \
        if (!n64_fpu_out32(c, &rb)) return; \
        n64_fpr_wr_d32(c, N64_FD(i), rb); \
        c->cycles += (extra); \
    }
#define N64_FPU_BIN64(name, op, extra) \
    N64_OP n64_op_##name(N64Cpu *c, uint32_t i) \
    { \
        if (!n64_fpu_start(c)) return; \
        uint64_t ab = n64_fpr_rd_t64(c, N64_FS(i)), bb = n64_fpr_rd_ft64(c, N64_FT(i)); \
        if (!n64_fpu_in2_64(c, ab, bb)) return; \
        feclearexcept(N64_HOST_FE); \
        volatile double a = n64_f64(ab), b = n64_f64(bb); \
        N64_FPU_EVAL(c, double, r, exc, a op b) \
        if (n64_fpu_host_flags(c, exc, false)) return; \
        uint64_t rb = n64_f64_bits(r); \
        if (!n64_fpu_out64(c, &rb)) return; \
        n64_fpr_wr_d64(c, N64_FD(i), rb); \
        c->cycles += (extra); \
    }

N64_FPU_BIN32(fadd_s, +, 2)
N64_FPU_BIN32(fsub_s, -, 2)
N64_FPU_BIN32(fmul_s, *, 4)
N64_FPU_BIN32(fdiv_s, /, 28)
N64_FPU_BIN64(fadd_d, +, 2)
N64_FPU_BIN64(fsub_d, -, 2)
N64_FPU_BIN64(fmul_d, *, 7)
N64_FPU_BIN64(fdiv_d, /, 57)

N64_OP n64_op_fsqrt_s(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint32_t ab = n64_fpr_rd_s32(c, N64_FS(i));
    if (!n64_fpu_in1_32(c, ab)) return;
    feclearexcept(N64_HOST_FE);
    volatile float a = n64_f32(ab);
    N64_FPU_EVAL(c, float, r, exc, sqrtf(a))
    if (n64_fpu_host_flags(c, exc, false)) return;
    uint32_t rb = n64_f32_bits(r);
    if (!n64_fpu_out32(c, &rb)) return;
    n64_fpr_wr_d32(c, N64_FD(i), rb);
    c->cycles += 28;
}
N64_OP n64_op_fsqrt_d(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint64_t ab = n64_fpr_rd_t64(c, N64_FS(i));
    if (!n64_fpu_in1_64(c, ab)) return;
    feclearexcept(N64_HOST_FE);
    volatile double a = n64_f64(ab);
    N64_FPU_EVAL(c, double, r, exc, sqrt(a))
    if (n64_fpu_host_flags(c, exc, false)) return;
    uint64_t rb = n64_f64_bits(r);
    if (!n64_fpu_out64(c, &rb)) return;
    n64_fpr_wr_d64(c, N64_FD(i), rb);
    c->cycles += 57;
}

/* ABS and NEG flip or clear the sign; the operand and result checks still
 * apply (a NaN result becomes the default NaN). */
N64_OP n64_op_fabs_s(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint32_t b = n64_fpr_rd_s32(c, N64_FS(i));
    if (!n64_fpu_in1_32(c, b)) return;
    b &= 0x7FFFFFFFu;
    if (!n64_fpu_out32(c, &b)) return;
    n64_fpr_wr_d32(c, N64_FD(i), b);
}
N64_OP n64_op_fabs_d(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint64_t b = n64_fpr_rd_t64(c, N64_FS(i));
    if (!n64_fpu_in1_64(c, b)) return;
    b &= 0x7FFFFFFFFFFFFFFFull;
    if (!n64_fpu_out64(c, &b)) return;
    n64_fpr_wr_d64(c, N64_FD(i), b);
}
N64_OP n64_op_fneg_s(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint32_t b = n64_fpr_rd_s32(c, N64_FS(i));
    if (!n64_fpu_in1_32(c, b)) return;
    b ^= 0x80000000u;
    if (!n64_fpu_out32(c, &b)) return;
    n64_fpr_wr_d32(c, N64_FD(i), b);
}
N64_OP n64_op_fneg_d(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint64_t b = n64_fpr_rd_t64(c, N64_FS(i));
    if (!n64_fpu_in1_64(c, b)) return;
    b ^= 0x8000000000000000ull;
    if (!n64_fpu_out64(c, &b)) return;
    n64_fpr_wr_d64(c, N64_FD(i), b);
}

/* MOV copies the 64-bit register without looking at it. */
N64_OP n64_op_fmov_s(N64Cpu *c, uint32_t i)
{
    if (!(c->cp0_status & N64_SR_CU1)) {
        n64_cpu_unusable(c, 1);
        return;
    }
    n64_fpr_wr_d64(c, N64_FD(i), n64_fpr_rd_t64(c, N64_FS(i)));
}
#define n64_op_fmov_d n64_op_fmov_s

N64_OP n64_op_cop1_unimpl(N64Cpu *c, uint32_t i)
{
    (void)i;
    if (!n64_fpu_start(c)) return;
    n64_fpe_unimpl(c);
    n64_fpu_raise(c);
}

/* ---- conversions between formats ---- */

N64_OP n64_op_fcvt_d_s(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint32_t ab = n64_fpr_rd_s32(c, N64_FS(i));
    if (!n64_fpu_in1_32(c, ab)) return;
    feclearexcept(N64_HOST_FE);
    volatile float a = n64_f32(ab);
    N64_FPU_EVAL(c, double, r, exc, (double)a)
    if (n64_fpu_host_flags(c, exc, false)) return;
    uint64_t rb = n64_f64_bits(r);
    if (!n64_fpu_out64(c, &rb)) return;
    n64_fpr_wr_d64(c, N64_FD(i), rb);
}
N64_OP n64_op_fcvt_s_d(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint64_t ab = n64_fpr_rd_t64(c, N64_FS(i));
    if (!n64_fpu_in1_64(c, ab)) return;
    feclearexcept(N64_HOST_FE);
    volatile double a = n64_f64(ab);
    N64_FPU_EVAL(c, float, r, exc, (float)a)
    if (n64_fpu_host_flags(c, exc, false)) return;
    uint32_t rb = n64_f32_bits(r);
    if (!n64_fpu_out32(c, &rb)) return;
    n64_fpr_wr_d32(c, N64_FD(i), rb);
    c->cycles += 1;
}
N64_OP n64_op_fcvt_s_w(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    feclearexcept(N64_HOST_FE);
    volatile int32_t a = (int32_t)n64_fpr_rd_s32(c, N64_FS(i));
    N64_FPU_EVAL(c, float, r, exc, (float)a)
    if (n64_fpu_host_flags(c, exc, false)) return;
    uint32_t rb = n64_f32_bits(r);
    if (!n64_fpu_out32(c, &rb)) return;
    n64_fpr_wr_d32(c, N64_FD(i), rb);
    c->cycles += 4;
}
N64_OP n64_op_fcvt_d_w(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    feclearexcept(N64_HOST_FE);
    volatile int32_t a = (int32_t)n64_fpr_rd_s32(c, N64_FS(i));
    N64_FPU_EVAL(c, double, r, exc, (double)a)
    if (n64_fpu_host_flags(c, exc, false)) return;
    uint64_t rb = n64_f64_bits(r);
    if (!n64_fpu_out64(c, &rb)) return;
    n64_fpr_wr_d64(c, N64_FD(i), rb);
    c->cycles += 4;
}
/* From 64-bit integers: only values below 2^55 in magnitude convert. */
N64_FORCEINLINE bool n64_fpu_l_ok(N64Cpu *c, int64_t v)
{
    if (v >= (int64_t)0x0080000000000000ll || v < -(int64_t)0x0080000000000000ll) {
        n64_fpe_unimpl(c);
        n64_fpu_raise(c);
        return false;
    }
    return true;
}
N64_OP n64_op_fcvt_s_l(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    int64_t v = (int64_t)n64_fpr_rd_t64(c, N64_FS(i));
    if (!n64_fpu_l_ok(c, v)) return;
    feclearexcept(N64_HOST_FE);
    volatile int64_t a = v;
    N64_FPU_EVAL(c, float, r, exc, (float)a)
    if (n64_fpu_host_flags(c, exc, false)) return;
    uint32_t rb = n64_f32_bits(r);
    if (!n64_fpu_out32(c, &rb)) return;
    n64_fpr_wr_d32(c, N64_FD(i), rb);
    c->cycles += 4;
}
N64_OP n64_op_fcvt_d_l(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    int64_t v = (int64_t)n64_fpr_rd_t64(c, N64_FS(i));
    if (!n64_fpu_l_ok(c, v)) return;
    feclearexcept(N64_HOST_FE);
    volatile int64_t a = v;
    N64_FPU_EVAL(c, double, r, exc, (double)a)
    if (n64_fpu_host_flags(c, exc, false)) return;
    uint64_t rb = n64_f64_bits(r);
    if (!n64_fpu_out64(c, &rb)) return;
    n64_fpr_wr_d64(c, N64_FD(i), rb);
    c->cycles += 4;
}

/* ---- conversions to integers ----
 * mode: 0 nearest-even, 1 toward zero, 2 toward +inf, 3 toward -inf (the
 * FCR31 encoding). x is finite and in range when this runs. The rounding
 * is exact: x - floor(x) of a double is exact. */
static inline double n64_fpu_round(double x, uint32_t mode)
{
    double f = floor(x);
    double frac = x - f;
    switch (mode) {
    case 1: return x < 0 ? (frac != 0 ? f + 1.0 : f) : f;
    case 2: return frac != 0 ? f + 1.0 : f;
    case 3: return f;
    default:
        if (frac > 0.5) return f + 1.0;
        if (frac < 0.5) return f;
        return fmod(f, 2.0) != 0 ? f + 1.0 : f; /* tie: to even */
    }
}

/* Range checks: a 32-bit result needs [-2^31, 2^31); a 64-bit one needs
 * |x| < 2^53. Denormals, infinities and NaNs are unimplemented. */
N64_FORCEINLINE bool n64_fpu_conv_ok(N64Cpu *c, double x, bool is_denormal, bool special, bool to64)
{
    bool bad = is_denormal || special;
    if (!bad) {
        if (to64) bad = x >= 9007199254740992.0 || x <= -9007199254740992.0;
        else bad = x >= 2147483648.0 || x < -2147483648.0;
    }
    if (bad) {
        n64_fpe_unimpl(c);
        n64_fpu_raise(c);
        return false;
    }
    return true;
}

/* mode < 0 means the current rounding mode (CVT.W, CVT.L). */
N64_FORCEINLINE void n64_fpu_to_int(N64Cpu *c, uint32_t i, bool from64, bool to64, int mode)
{
    if (!n64_fpu_start(c)) return;
    double x;
    bool den, special;
    if (from64) {
        uint64_t b = n64_fpr_rd_t64(c, N64_FS(i));
        x = n64_f64(b);
        den = n64_denormal64(b);
        special = n64_isnan64(b) || n64_isinf64(b);
    } else {
        uint32_t b = n64_fpr_rd_s32(c, N64_FS(i));
        x = (double)n64_f32(b);
        den = n64_denormal32(b);
        special = n64_isnan32(b) || n64_isinf32(b);
    }
    if (!n64_fpu_conv_ok(c, x, den, special, to64)) return;
    double r = n64_fpu_round(x, mode < 0 ? (c->fcr31 & 3u) : (uint32_t)mode);
    if (!to64 && (r >= 2147483648.0 || r < -2147483648.0)) {
        /* Rounding pushed it out of range (e.g. 2147483647.5 upward). */
        n64_fpe_unimpl(c);
        n64_fpu_raise(c);
        return;
    }
    if (r != x && n64_fpe(c, N64_FPE_I)) {
        n64_fpu_raise(c);
        return;
    }
    if (to64) n64_fpr_wr_d64(c, N64_FD(i), (uint64_t)(int64_t)r);
    else n64_fpr_wr_d32(c, N64_FD(i), (uint32_t)(int32_t)r);
    c->cycles += 4;
}

N64_OP n64_op_fround_l_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, true, 0); }
N64_OP n64_op_ftrunc_l_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, true, 1); }
N64_OP n64_op_fceil_l_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, true, 2); }
N64_OP n64_op_ffloor_l_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, true, 3); }
N64_OP n64_op_fround_w_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, false, 0); }
N64_OP n64_op_ftrunc_w_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, false, 1); }
N64_OP n64_op_fceil_w_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, false, 2); }
N64_OP n64_op_ffloor_w_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, false, 3); }
N64_OP n64_op_fcvt_w_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, false, -1); }
N64_OP n64_op_fcvt_l_s(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, false, true, -1); }
N64_OP n64_op_fround_l_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, true, 0); }
N64_OP n64_op_ftrunc_l_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, true, 1); }
N64_OP n64_op_fceil_l_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, true, 2); }
N64_OP n64_op_ffloor_l_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, true, 3); }
N64_OP n64_op_fround_w_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, false, 0); }
N64_OP n64_op_ftrunc_w_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, false, 1); }
N64_OP n64_op_fceil_w_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, false, 2); }
N64_OP n64_op_ffloor_w_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, false, 3); }
N64_OP n64_op_fcvt_w_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, false, -1); }
N64_OP n64_op_fcvt_l_d(N64Cpu *c, uint32_t i) { n64_fpu_to_int(c, i, true, true, -1); }

/* ---- compares ----
 * The low four bits of the function choose the condition: bit 0
 * unordered, bit 1 equal, bit 2 less; bit 3 makes a quiet NaN invalid too. */
N64_FORCEINLINE void n64_fpu_compare(N64Cpu *c, uint32_t i, bool unordered, bool lt, bool eq)
{
    const uint32_t cond = i & 0xFu;
    bool result;
    if (unordered) result = (cond & 1u) != 0;
    else result = ((cond & 2u) && eq) || ((cond & 4u) && lt);
    if (result) c->fcr31 |= N64_FCR31_C;
    else c->fcr31 &= ~N64_FCR31_C;
}

N64_OP n64_op_fc_s(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint32_t ab = n64_fpr_rd_s32(c, N64_FS(i)), bb = n64_fpr_rd_ft32(c, N64_FT(i));
    if (n64_isnan32(ab) || n64_isnan32(bb)) {
        const bool signal_quiet = (i & 8u) != 0;
        if (n64_isnan32(ab) && (signal_quiet || n64_snan32(ab)) && n64_fpe(c, N64_FPE_V)) {
            n64_fpu_raise(c);
            return;
        }
        if (n64_isnan32(bb) && (signal_quiet || n64_snan32(bb)) && n64_fpe(c, N64_FPE_V)) {
            n64_fpu_raise(c);
            return;
        }
        n64_fpu_compare(c, i, true, false, false);
        return;
    }
    float a = n64_f32(ab), b = n64_f32(bb);
    n64_fpu_compare(c, i, false, a < b, a == b);
}
N64_OP n64_op_fc_d(N64Cpu *c, uint32_t i)
{
    if (!n64_fpu_start(c)) return;
    uint64_t ab = n64_fpr_rd_t64(c, N64_FS(i)), bb = n64_fpr_rd_ft64(c, N64_FT(i));
    if (n64_isnan64(ab) || n64_isnan64(bb)) {
        const bool signal_quiet = (i & 8u) != 0;
        if (n64_isnan64(ab) && (signal_quiet || n64_snan64(ab)) && n64_fpe(c, N64_FPE_V)) {
            n64_fpu_raise(c);
            return;
        }
        if (n64_isnan64(bb) && (signal_quiet || n64_snan64(bb)) && n64_fpe(c, N64_FPE_V)) {
            n64_fpu_raise(c);
            return;
        }
        n64_fpu_compare(c, i, true, false, false);
        return;
    }
    double a = n64_f64(ab), b = n64_f64(bb);
    n64_fpu_compare(c, i, false, a < b, a == b);
}

/* ---- moves, loads and stores ---- */

#define N64_NEED_CU1(c) \
    do { \
        if (!((c)->cp0_status & N64_SR_CU1)) { \
            n64_cpu_unusable(c, 1); \
            return; \
        } \
    } while (0)

N64_OP n64_op_mfc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    n64_wr(c, N64_RT(i), n64_sext32(n64_fpr_rd_t32(c, N64_FS(i))));
}
N64_OP n64_op_dmfc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    n64_wr(c, N64_RT(i), n64_fpr_rd_t64(c, N64_FS(i)));
}
N64_OP n64_op_mtc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    n64_fpr_wr_t32(c, N64_FS(i), (uint32_t)N64_GT(c, i));
}
N64_OP n64_op_dmtc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    n64_fpr_wr_t64(c, N64_FS(i), N64_GT(c, i));
}
N64_OP n64_op_cfc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    uint32_t v = 0;
    if (N64_RD(i) == 0) v = 0x0A00u; /* implementation 0x0A, revision 0 */
    else if (N64_RD(i) == 31) v = c->fcr31;
    n64_wr(c, N64_RT(i), n64_sext32(v));
}
/* Writing FCR31 can raise the exception it enables. */
N64_OP n64_op_ctc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    if (N64_RD(i) != 31) return;
    c->fcr31 = (uint32_t)N64_GT(c, i) & N64_FCR31_WRITABLE;
    uint32_t cause = (c->fcr31 >> 12) & 0x3Fu, enable = (c->fcr31 >> 7) & 0x1Fu;
    if ((cause & enable) || (cause & 0x20u)) n64_fpu_raise(c);
}
N64_OP n64_op_lwc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    uint32_t v;
    if (n64_load32(c, N64_EA(c, i), &v)) n64_fpr_wr_t32(c, N64_FT(i), v);
}
N64_OP n64_op_ldc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    uint64_t v;
    if (n64_load64(c, N64_EA(c, i), &v)) n64_fpr_wr_t64(c, N64_FT(i), v);
}
N64_OP n64_op_swc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    (void)n64_store32(c, N64_EA(c, i), n64_fpr_rd_t32(c, N64_FT(i)));
}
N64_OP n64_op_sdc1(N64Cpu *c, uint32_t i)
{
    N64_NEED_CU1(c);
    (void)n64_store64(c, N64_EA(c, i), n64_fpr_rd_t64(c, N64_FT(i)));
}

#ifdef __cplusplus
}
#endif

#endif /* N64_FPU_IMPL_H */
