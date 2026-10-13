/*
 * RSP unit tests: small programs written into IMEM as encoded words and
 * run with n64_rsp_run(), plus direct SP register traffic. Expected values
 * are worked out by hand from the instruction semantics (each case says
 * how), except the VMUDH/VMADN accumulator pattern, which is the console's
 * own output as recorded by n64-systemtest (MIT, lemmy-64).
 */
#include "n64_rsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK_EQ(got, want)                                                                                         \
    do {                                                                                                            \
        unsigned long long got_ = (unsigned long long)(got), want_ = (unsigned long long)(want);                    \
        g_checks++;                                                                                                 \
        if (got_ != want_) {                                                                                        \
            fprintf(stderr, "%s:%d: %s = 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #got, got_, want_);        \
            g_failures++;                                                                                           \
        }                                                                                                           \
    } while (0)

static void check_vec_at(int line, const char *what, const uint16_t *got, const uint16_t *want)
{
    int n, bad = 0;
    g_checks++;
    for (n = 0; n < 8; n++) bad |= got[n] != want[n];
    if (bad) {
        fprintf(stderr, "%s:%d: %s =", __FILE__, line, what);
        for (n = 0; n < 8; n++) fprintf(stderr, " %04X", got[n]);
        fprintf(stderr, ", expected");
        for (n = 0; n < 8; n++) fprintf(stderr, " %04X", want[n]);
        fprintf(stderr, "\n");
        g_failures++;
    }
}

#define CHECK_VEC(got, ...)                                                                                         \
    do {                                                                                                            \
        static const uint16_t want_[8] = {__VA_ARGS__};                                                             \
        check_vec_at(__LINE__, #got, (got), want_);                                                                 \
    } while (0)

static void set_vec(uint16_t *v, uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t e, uint16_t f, uint16_t g,
                    uint16_t h)
{
    v[0] = a, v[1] = b, v[2] = c, v[3] = d, v[4] = e, v[5] = f, v[6] = g, v[7] = h;
}

static void set_acc(N64Rsp *rsp, unsigned n, uint64_t v)
{
    rsp->vu.acc_h[n] = (uint16_t)(v >> 32);
    rsp->vu.acc_m[n] = (uint16_t)(v >> 16);
    rsp->vu.acc_l[n] = (uint16_t)v;
}

/* Encoders. */

static uint32_t enc_r(unsigned funct, unsigned rd, unsigned rs, unsigned rt, unsigned sa)
{
    return ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11) | ((uint32_t)sa << 6) | funct;
}

static uint32_t enc_i(unsigned opc, unsigned rt, unsigned rs, uint32_t imm)
{
    return ((uint32_t)opc << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (imm & 0xFFFFu);
}

#define NOP 0u
#define BREAK 0x0000000Du
#define ADDU(d, s, t) enc_r(0x21, d, s, t, 0)
#define ADD(d, s, t) enc_r(0x20, d, s, t, 0)
#define SUBU(d, s, t) enc_r(0x23, d, s, t, 0)
#define AND(d, s, t) enc_r(0x24, d, s, t, 0)
#define OR(d, s, t) enc_r(0x25, d, s, t, 0)
#define XOR(d, s, t) enc_r(0x26, d, s, t, 0)
#define NOR(d, s, t) enc_r(0x27, d, s, t, 0)
#define SLT(d, s, t) enc_r(0x2A, d, s, t, 0)
#define SLTU(d, s, t) enc_r(0x2B, d, s, t, 0)
#define SLL(d, t, sa) enc_r(0x00, d, 0, t, sa)
#define SRL(d, t, sa) enc_r(0x02, d, 0, t, sa)
#define SRA(d, t, sa) enc_r(0x03, d, 0, t, sa)
#define SLLV(d, t, s) enc_r(0x04, d, s, t, 0)
#define SRLV(d, t, s) enc_r(0x06, d, s, t, 0)
#define SRAV(d, t, s) enc_r(0x07, d, s, t, 0)
#define JR(s) enc_r(0x08, 0, s, 0, 0)
#define JALR(d, s) enc_r(0x09, d, s, 0, 0)
#define ADDIU(t, s, i) enc_i(0x09, t, s, (uint32_t)(i))
#define ADDI(t, s, i) enc_i(0x08, t, s, (uint32_t)(i))
#define SLTI(t, s, i) enc_i(0x0A, t, s, (uint32_t)(i))
#define SLTIU(t, s, i) enc_i(0x0B, t, s, (uint32_t)(i))
#define ANDI(t, s, i) enc_i(0x0C, t, s, i)
#define ORI(t, s, i) enc_i(0x0D, t, s, i)
#define XORI(t, s, i) enc_i(0x0E, t, s, i)
#define LUI(t, i) enc_i(0x0F, t, 0, i)
#define BEQ(s, t, off) enc_i(0x04, t, s, (uint32_t)(off))
#define BNE(s, t, off) enc_i(0x05, t, s, (uint32_t)(off))
#define BLEZ(s, off) enc_i(0x06, 0, s, (uint32_t)(off))
#define BGTZ(s, off) enc_i(0x07, 0, s, (uint32_t)(off))
#define BLTZ(s, off) enc_i(0x01, 0x00, s, (uint32_t)(off))
#define BGEZ(s, off) enc_i(0x01, 0x01, s, (uint32_t)(off))
#define BLTZAL(s, off) enc_i(0x01, 0x10, s, (uint32_t)(off))
#define BGEZAL(s, off) enc_i(0x01, 0x11, s, (uint32_t)(off))
#define J(addr) ((0x02u << 26) | (((uint32_t)(addr) >> 2) & 0x03FFFFFFu))
#define JAL(addr) ((0x03u << 26) | (((uint32_t)(addr) >> 2) & 0x03FFFFFFu))
#define LB(t, s, i) enc_i(0x20, t, s, (uint32_t)(i))
#define LH(t, s, i) enc_i(0x21, t, s, (uint32_t)(i))
#define LW(t, s, i) enc_i(0x23, t, s, (uint32_t)(i))
#define LBU(t, s, i) enc_i(0x24, t, s, (uint32_t)(i))
#define LHU(t, s, i) enc_i(0x25, t, s, (uint32_t)(i))
#define LWU(t, s, i) enc_i(0x27, t, s, (uint32_t)(i))
#define SB(t, s, i) enc_i(0x28, t, s, (uint32_t)(i))
#define SH(t, s, i) enc_i(0x29, t, s, (uint32_t)(i))
#define SW(t, s, i) enc_i(0x2B, t, s, (uint32_t)(i))
#define MFC0(t, c) ((0x10u << 26) | (0x00u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(c) << 11))
#define MTC0(t, c) ((0x10u << 26) | (0x04u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(c) << 11))
#define MFC2(t, v, e) ((0x12u << 26) | (0x00u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(v) << 11) | ((uint32_t)(e) << 7))
#define MTC2(t, v, e) ((0x12u << 26) | (0x04u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(v) << 11) | ((uint32_t)(e) << 7))
#define CFC2(t, c) ((0x12u << 26) | (0x02u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(c) << 11))
#define CTC2(t, c) ((0x12u << 26) | (0x06u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(c) << 11))

static uint32_t vop(unsigned funct, unsigned vd, unsigned vs, unsigned vt, unsigned e)
{
    return (0x12u << 26) | (1u << 25) | ((uint32_t)e << 21) | ((uint32_t)vt << 16) | ((uint32_t)vs << 11) |
           ((uint32_t)vd << 6) | funct;
}

/* LWC2/SWC2: `off` is in units of the access size, as the assembler
 * writes it. */
static uint32_t lwc2(unsigned kind, unsigned vt, unsigned e, unsigned base, int off)
{
    return (0x32u << 26) | ((uint32_t)base << 21) | ((uint32_t)vt << 16) | ((uint32_t)kind << 11) |
           ((uint32_t)e << 7) | ((uint32_t)off & 0x7Fu);
}

static uint32_t swc2(unsigned kind, unsigned vt, unsigned e, unsigned base, int off)
{
    return lwc2(kind, vt, e, base, off) ^ (0x32u << 26) ^ (0x3Au << 26);
}

enum { LBV, LSV, LLV, LDV, LQV, LRV, LPV, LUV, LHV, LFV, LWV, LTV };
enum { SBV, SSV, SLV, SDV, SQV, SRV, SPV, SUV, SHV, SFV, SWV, STV };

enum {
    VMULF = 0x00, VMULU = 0x01, VRNDP = 0x02, VMULQ = 0x03, VMUDL = 0x04, VMUDM = 0x05, VMUDN = 0x06,
    VMUDH = 0x07, VMACF = 0x08, VMACU = 0x09, VRNDN = 0x0A, VMACQ = 0x0B, VMADL = 0x0C, VMADM = 0x0D,
    VMADN = 0x0E, VMADH = 0x0F, VADD = 0x10, VSUB = 0x11, VSUT = 0x12, VABS = 0x13, VADDC = 0x14,
    VSUBC = 0x15, VADDB = 0x16, VSAD = 0x1A, VSAR = 0x1D, VLT = 0x20, VEQ = 0x21, VNE = 0x22, VGE = 0x23,
    VCL = 0x24, VCH = 0x25, VCR = 0x26, VMRG = 0x27, VAND = 0x28, VNAND = 0x29, VOR = 0x2A, VNOR = 0x2B,
    VXOR = 0x2C, VNXOR = 0x2D, VRCP = 0x30, VRCPL = 0x31, VRCPH = 0x32, VMOV = 0x33, VRSQ = 0x34,
    VRSQL = 0x35, VRSQH = 0x36, VNOP = 0x37, VEXTT = 0x38, VNULL = 0x3F,
};

/* Harness. */

typedef struct Recorder {
    int interrupts[16];
    int interrupt_count;
    uint32_t dp_regs[8];
    unsigned dp_writes;
} Recorder;

static void rec_interrupt(void *user, int level)
{
    Recorder *r = (Recorder *)user;
    if (r->interrupt_count < 16) r->interrupts[r->interrupt_count] = level;
    r->interrupt_count++;
}

static uint32_t rec_dp_read(void *user, unsigned reg) { return ((Recorder *)user)->dp_regs[reg & 7]; }

static void rec_dp_write(void *user, unsigned reg, uint32_t value)
{
    Recorder *r = (Recorder *)user;
    r->dp_regs[reg & 7] = value;
    r->dp_writes++;
}

static void put_words(N64Rsp *rsp, uint32_t at, const uint32_t *words, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) n64_put_be32(rsp->imem + ((at + 4u * (uint32_t)i) & 0xFFCu), words[i]);
}

/* Starts the RSP at `pc` and runs it until it halts. */
static uint64_t start_at(N64Rsp *rsp, uint32_t pc)
{
    n64_rsp_reg_write(rsp, N64_SP_REG_PC, pc);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x1); /* clear halt */
    return n64_rsp_run(rsp, 100000);
}

/* Places the words at IMEM 0 followed by BREAK and runs them. */
static void run_words(N64Rsp *rsp, const uint32_t *words, size_t n)
{
    uint32_t brk = BREAK;
    put_words(rsp, 0, words, n);
    put_words(rsp, (uint32_t)(4 * n), &brk, 1);
    start_at(rsp, 0);
    CHECK_EQ(rsp->status & 3u, 3u);
}

static void run1(N64Rsp *rsp, uint32_t word) { run_words(rsp, &word, 1); }

/* Scalar unit. */

static void test_scalar_alu(N64Rsp *rsp)
{
    const uint32_t prog[] = {
        LUI(1, 0x7FFF),         /* r1 = 7FFF0000 */
        ORI(1, 1, 0xFFFF),      /* r1 = 7FFFFFFF */
        ADDI(2, 1, 1),          /* no overflow trap: 80000000 */
        ADDIU(3, 0, -2),        /* FFFFFFFE */
        ADD(4, 3, 3),           /* FFFFFFFC */
        SUBU(5, 0, 1),          /* 80000001 */
        ANDI(6, 3, 0xF0F0),     /* zero-extended: 0000F0F0 */
        XORI(7, 3, 0xFFFF),     /* FFFF0001 */
        AND(8, 1, 3),           /* 7FFFFFFE */
        OR(9, 2, 6),            /* 8000F0F0 */
        XOR(10, 1, 3),          /* 80000001 */
        NOR(11, 0, 0),          /* FFFFFFFF */
        SLT(12, 3, 0),          /* -2 < 0: 1 */
        SLTU(13, 3, 0),         /* FFFFFFFE < 0: 0 */
        SLTI(14, 3, -1),        /* -2 < -1: 1 */
        SLTIU(15, 1, 0xFFFF),   /* 7FFFFFFF < FFFFFFFF: 1 */
        SLTIU(16, 11, 0xFFFF),  /* FFFFFFFF < FFFFFFFF: 0 */
        SLL(17, 3, 4),          /* FFFFFFE0 */
        SRL(18, 3, 4),          /* 0FFFFFFF */
        SRA(19, 3, 1),          /* FFFFFFFF */
        ADDIU(20, 0, 36),       /* shift amount 36 & 31 = 4 */
        SLLV(21, 1, 20),        /* FFFFFFF0 */
        SRLV(22, 2, 20),        /* 08000000 */
        SRAV(23, 2, 20),        /* F8000000 */
        ADDIU(0, 0, 5),         /* r0 stays 0 */
        ADDU(24, 0, 0),         /* 0 */
        enc_r(0x18, 25, 20, 1, 0), /* MULT is not an RSP op: rd = rs >> (rs & 31) = 36 >> 4 = 2 */
        LUI(26, 0x8000),
        SRA(26, 26, 31),        /* FFFFFFFF */
    };
    n64_rsp_reset(rsp);
    run_words(rsp, prog, sizeof prog / sizeof prog[0]);
    CHECK_EQ(rsp->r[1], 0x7FFFFFFFu);
    CHECK_EQ(rsp->r[2], 0x80000000u);
    CHECK_EQ(rsp->r[3], 0xFFFFFFFEu);
    CHECK_EQ(rsp->r[4], 0xFFFFFFFCu);
    CHECK_EQ(rsp->r[5], 0x80000001u);
    CHECK_EQ(rsp->r[6], 0x0000F0F0u);
    CHECK_EQ(rsp->r[7], 0xFFFF0001u);
    CHECK_EQ(rsp->r[8], 0x7FFFFFFEu);
    CHECK_EQ(rsp->r[9], 0x8000F0F0u);
    CHECK_EQ(rsp->r[10], 0x80000001u);
    CHECK_EQ(rsp->r[11], 0xFFFFFFFFu);
    CHECK_EQ(rsp->r[12], 1);
    CHECK_EQ(rsp->r[13], 0);
    CHECK_EQ(rsp->r[14], 1);
    CHECK_EQ(rsp->r[15], 1);
    CHECK_EQ(rsp->r[16], 0);
    CHECK_EQ(rsp->r[17], 0xFFFFFFE0u);
    CHECK_EQ(rsp->r[18], 0x0FFFFFFFu);
    CHECK_EQ(rsp->r[19], 0xFFFFFFFFu);
    CHECK_EQ(rsp->r[21], 0xFFFFFFF0u);
    CHECK_EQ(rsp->r[22], 0x08000000u);
    CHECK_EQ(rsp->r[23], 0xF8000000u);
    CHECK_EQ(rsp->r[0], 0);
    CHECK_EQ(rsp->r[24], 0);
    CHECK_EQ(rsp->r[25], 2);
    CHECK_EQ(rsp->r[26], 0xFFFFFFFFu);
    /* One cycle per instruction, BREAK included. */
    CHECK_EQ(rsp->cycles, sizeof prog / sizeof prog[0] + 1);
    CHECK_EQ(rsp->instructions, rsp->cycles);
}

static void test_branches(N64Rsp *rsp)
{
    /* BEQ taken: the delay slot runs, the next instruction is skipped. */
    {
        const uint32_t prog[] = {
            BEQ(0, 0, 2),    /* 0x00 -> 0x0C */
            ADDIU(1, 1, 1),  /* 0x04 delay slot */
            ADDIU(2, 2, 1),  /* 0x08 skipped */
            ADDIU(3, 3, 1),  /* 0x0C */
            BNE(0, 0, 2),    /* 0x10 not taken */
            ADDIU(4, 4, 1),  /* 0x14 */
            ADDIU(5, 5, 1),  /* 0x18 */
            ADDIU(6, 0, -1), /* 0x1C */
            BLTZAL(6, 2),    /* 0x20 taken -> 0x2C, r31 = 0x28 */
            NOP,             /* 0x24 */
            ADDIU(7, 7, 1),  /* 0x28 skipped */
            BGEZAL(6, 3),    /* 0x2C not taken, r31 still written: 0x34 */
            ADDU(8, 31, 0),  /* 0x30 delay slot of the not-taken branch */
            BLEZ(0, 2),      /* 0x34 taken -> 0x40 */
            NOP,
            ADDIU(9, 9, 1),  /* 0x3C skipped */
            BGTZ(6, 2),      /* 0x40 not taken */
            NOP,
            BGEZ(0, 2),      /* 0x48 taken -> 0x54 */
            NOP,
            ADDIU(10, 10, 1), /* 0x50 skipped */
            BLTZ(0, 2),      /* 0x54 not taken */
            NOP,
        };
        n64_rsp_reset(rsp);
        run_words(rsp, prog, sizeof prog / sizeof prog[0]);
        CHECK_EQ(rsp->r[1], 1);
        CHECK_EQ(rsp->r[2], 0);
        CHECK_EQ(rsp->r[3], 1);
        CHECK_EQ(rsp->r[4], 1);
        CHECK_EQ(rsp->r[5], 1);
        CHECK_EQ(rsp->r[7], 0);
        CHECK_EQ(rsp->r[8], 0x34);
        CHECK_EQ(rsp->r[31], 0x34);
        CHECK_EQ(rsp->r[9], 0);
        CHECK_EQ(rsp->r[10], 0);
    }
    /* JAL/JR, and a branch in a delay slot: the first target runs one
     * instruction, then the second branch's target. */
    {
        const uint32_t prog[] = {
            JAL(0x100),       /* 0x00, r31 = 0x08 */
            ADDIU(1, 0, 7),   /* 0x04 delay slot */
            J(0x200),         /* 0x08 */
            BEQ(0, 0, 0x3F),  /* 0x0C, in J's delay slot: -> 0x10 + 0xFC = 0x10C */
            ADDIU(2, 0, 1),   /* 0x10 never */
        };
        const uint32_t sub[] = {JR(31), ADDIU(3, 0, 3)};
        const uint32_t at200[] = {ADDIU(4, 0, 4), ADDIU(5, 0, 5), BREAK};
        const uint32_t at10c[] = {ADDIU(6, 0, 6), BREAK};
        n64_rsp_reset(rsp);
        put_words(rsp, 0, prog, 5);
        put_words(rsp, 0x100, sub, 2);
        put_words(rsp, 0x200, at200, 3);
        put_words(rsp, 0x10C, at10c, 2);
        start_at(rsp, 0);
        CHECK_EQ(rsp->r[1], 7);
        CHECK_EQ(rsp->r[3], 3);
        CHECK_EQ(rsp->r[31], 0x08);
        CHECK_EQ(rsp->r[4], 4); /* 0x200 ran once */
        CHECK_EQ(rsp->r[5], 0); /* then 0x10C */
        CHECK_EQ(rsp->r[6], 6);
        CHECK_EQ(rsp->r[2], 0);
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_PC), 0x114);
    }
    /* The PC wraps within IMEM; JR ignores the target's upper and low
     * bits; JALR at 0xFFC links to 0x004 (n64-systemtest's JR/JALR case). */
    {
        const uint32_t tail[] = {LUI(1, 0xFFFF), ORI(1, 1, 0xF00B), JALR(31, 1)};
        const uint32_t head[] = {ADDIU(16, 16, 1), ADDIU(17, 17, 1), ADDIU(18, 18, 1), BREAK};
        n64_rsp_reset(rsp);
        put_words(rsp, 0xFF4, tail, 3);
        put_words(rsp, 0x000, head, 4);
        start_at(rsp, 0xFF4);
        CHECK_EQ(rsp->r[16], 1); /* delay slot at 0x000 */
        CHECK_EQ(rsp->r[17], 0); /* skipped */
        CHECK_EQ(rsp->r[18], 1); /* target 0x008 */
        CHECK_EQ(rsp->r[31], 0x004);
    }
    /* BREAK in a taken branch's delay slot leaves the PC at the target;
     * in a not-taken one, after the BREAK (n64-systemtest values). */
    {
        const uint32_t taken[] = {BEQ(0, 0, 6), BREAK};
        const uint32_t not_taken[] = {BNE(0, 0, 6), BREAK};
        n64_rsp_reset(rsp);
        put_words(rsp, 0, taken, 2);
        start_at(rsp, 0);
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_PC), 0x1C);
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x3);
        n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x4); /* clear broke */
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x1);
        n64_rsp_reset(rsp);
        put_words(rsp, 0, not_taken, 2);
        start_at(rsp, 0);
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_PC), 0x8);
    }
}

static void test_scalar_memory(N64Rsp *rsp)
{
    unsigned i;
    const uint32_t prog[] = {
        LW(1, 0, 0x101),       /* unaligned: 01 02 03 04 */
        LW(2, 0, 0xFFE),       /* wraps: FE FF 00 01 */
        LH(3, 0, 0x0FF),       /* FF 00: sign-extended */
        LHU(4, 0, 0x0FF),
        LB(5, 0, 0x080),       /* 80: sign-extended */
        LBU(6, 0, 0x080),
        ADDIU(7, 0, 0x110),
        LWU(8, 7, -0x10),      /* negative offset: 0x100 */
        LUI(9, 0x1234),
        ORI(9, 9, 0x5678),
        SW(9, 0, 0xFFD),       /* wraps: FFD FFE FFF 000 */
        SH(9, 0, 0x203),
        SB(9, 0, 0x210),
        ADDIU(10, 0, 0x1000 + 0x300), /* addresses wrap at 4 KiB */
        SW(9, 10, 0),
        LB(0, 0, 0x080),       /* load to r0 is discarded */
    };
    n64_rsp_reset(rsp);
    for (i = 0; i < 0x1000; i++) rsp->dmem[i] = (uint8_t)i;
    run_words(rsp, prog, sizeof prog / sizeof prog[0]);
    CHECK_EQ(rsp->r[1], 0x01020304u);
    CHECK_EQ(rsp->r[2], 0xFEFF0001u);
    CHECK_EQ(rsp->r[3], 0xFFFFFF00u);
    CHECK_EQ(rsp->r[4], 0x0000FF00u);
    CHECK_EQ(rsp->r[5], 0xFFFFFF80u);
    CHECK_EQ(rsp->r[6], 0x80u);
    CHECK_EQ(rsp->r[8], 0x00010203u);
    CHECK_EQ(rsp->dmem[0xFFD], 0x12);
    CHECK_EQ(rsp->dmem[0xFFE], 0x34);
    CHECK_EQ(rsp->dmem[0xFFF], 0x56);
    CHECK_EQ(rsp->dmem[0x000], 0x78);
    CHECK_EQ(rsp->dmem[0x203], 0x56);
    CHECK_EQ(rsp->dmem[0x204], 0x78);
    CHECK_EQ(rsp->dmem[0x210], 0x78);
    CHECK_EQ(n64_be32(rsp->dmem + 0x300), 0x12345678u);
    CHECK_EQ(rsp->r[0], 0);
}

/* SP registers, DMA, BREAK. */

static void test_status_and_semaphore(N64Rsp *rsp, Recorder *rec)
{
    n64_rsp_reset(rsp);
    memset(rec, 0, sizeof *rec);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), N64_SP_STATUS_HALT);
    CHECK_EQ(n64_rsp_run(rsp, 10), 0); /* halted on entry */

    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x1); /* clear halt */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x3); /* both: no change */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x2); /* set halt */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 1);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x40); /* set sstep */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x21);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x20); /* clear sstep */
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x100); /* set intr break */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x41);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x80); /* clear intr break */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x01);
    /* Signals: set SIG0 (bit 10), SIG3 (bit 16) and SIG7 (bit 24), then
     * clear SIG0 (bit 9); SIG5 with both bits does nothing. */
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, (1u << 10) | (1u << 16) | (1u << 24) | (3u << 19));
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x01u | 0x0080u | 0x0400u | 0x4000u);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 1u << 9);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x01u | 0x0400u | 0x4000u);
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, (1u << 15) | (1u << 23)); /* clear SIG3, SIG7 */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x01u);

    /* The interrupt line. */
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x10); /* set intr */
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x08); /* clear intr */
    n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x18); /* both: nothing */
    CHECK_EQ(rec->interrupt_count, 2);
    CHECK_EQ(rec->interrupts[0], 1);
    CHECK_EQ(rec->interrupts[1], 0);

    /* Semaphore: a read returns the old value and sets it; a write clears. */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_SEMAPHORE), 0);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_SEMAPHORE), 1);
    n64_rsp_reg_write(rsp, N64_SP_REG_SEMAPHORE, 0xFFFFFFFFu);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_SEMAPHORE), 0);

    /* SP_PC: 12 bits, word aligned. */
    n64_rsp_reg_write(rsp, N64_SP_REG_PC, 0xFFFFF123u);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_PC), 0x120);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_DMA_FULL), 0);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_DMA_BUSY), 0);
}

static void test_break_and_cop0(N64Rsp *rsp, Recorder *rec)
{
    /* BREAK with interrupt-on-break raises the line once. */
    n64_rsp_reset(rsp);
    memset(rec, 0, sizeof *rec);
    {
        const uint32_t prog[] = {NOP, BREAK, ADDIU(1, 0, 1), BREAK};
        put_words(rsp, 0, prog, 4);
        n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x100);
        CHECK_EQ(start_at(rsp, 0), 2);
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_STATUS), 0x43);
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_PC), 0x8);
        CHECK_EQ(rec->interrupt_count, 1);
        CHECK_EQ(rec->interrupts[0], 1);
        /* Clearing halt (and broke) resumes after the BREAK. */
        n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x80 | 0x4 | 0x1);
        CHECK_EQ(n64_rsp_run(rsp, 100), 2);
        CHECK_EQ(rsp->r[1], 1);
        CHECK_EQ(rec->interrupt_count, 1); /* interrupt-on-break was cleared */
    }
    /* The budget stops a running RSP between instructions. */
    {
        const uint32_t loop[] = {ADDIU(1, 1, 1), J(0), NOP};
        n64_rsp_reset(rsp);
        put_words(rsp, 0, loop, 3);
        n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x1);
        CHECK_EQ(n64_rsp_run(rsp, 31), 31);
        CHECK_EQ(rsp->r[1], 11);
        CHECK_EQ(n64_rsp_halted(rsp), false);
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_PC), 0x4);
    }
    /* Single-step: each run of a cleared halt executes one instruction. */
    {
        const uint32_t prog[] = {ADDIU(1, 0, 1), ADDIU(2, 0, 2), BREAK};
        n64_rsp_reset(rsp);
        put_words(rsp, 0, prog, 3);
        n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x40 | 0x1);
        CHECK_EQ(n64_rsp_run(rsp, 100), 1);
        CHECK_EQ(n64_rsp_halted(rsp), true);
        CHECK_EQ(rsp->r[1], 1);
        CHECK_EQ(rsp->r[2], 0);
        n64_rsp_reg_write(rsp, N64_SP_REG_STATUS, 0x1);
        CHECK_EQ(n64_rsp_run(rsp, 100), 1);
        CHECK_EQ(rsp->r[2], 2);
        CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_PC), 0x8);
    }
    /* COP0: SP registers 0-7 (with their side effects) and the RDP's
     * command registers 8-15 through the links. */
    {
        const uint32_t prog[] = {
            MFC0(1, 7),               /* semaphore: 0, now taken */
            MFC0(2, 7),               /* 1 */
            MTC0(0, 7),               /* release */
            ORI(3, 0, 1u << 10),      /* set SIG0 */
            MTC0(3, 4),
            MFC0(4, 4),               /* status */
            ORI(5, 0, 0x1234),
            MTC0(5, 8 + 3),           /* DPC_STATUS-numbered write */
            MFC0(6, 8 + 2),           /* DPC_CURRENT */
            MFC0(0, 8 + 2),
        };
        n64_rsp_reset(rsp);
        memset(rec, 0, sizeof *rec);
        rec->dp_regs[2] = 0xCAFE0;
        run_words(rsp, prog, sizeof prog / sizeof prog[0]);
        CHECK_EQ(rsp->r[1], 0);
        CHECK_EQ(rsp->r[2], 1);
        CHECK_EQ(rsp->semaphore, 0);
        CHECK_EQ(rsp->r[4], 0x80);
        CHECK_EQ(rec->dp_regs[3], 0x1234);
        CHECK_EQ(rec->dp_writes, 1);
        CHECK_EQ(rsp->r[6], 0xCAFE0);
        CHECK_EQ(rsp->r[0], 0);
    }
}

static void test_dma(N64Rsp *rsp, uint8_t *rdram)
{
    unsigned i;
    /* rdram holds 0x10000 bytes the RSP may use and 16 guard bytes. */
    for (i = 0; i < 0x10010; i++) rdram[i] = (uint8_t)(i * 7u + (i >> 8));

    /* One row of 16 bytes, RDRAM -> DMEM; the addresses read back advanced
     * and the length reads 0xFF8 with a count of 0. */
    n64_rsp_reset(rsp);
    n64_rsp_reg_write(rsp, N64_SP_REG_MEM_ADDR, 0x0123);   /* 8-byte aligned: 0x120 */
    n64_rsp_reg_write(rsp, N64_SP_REG_DRAM_ADDR, 0x80000405u); /* 24 bits, aligned: 0x400 */
    n64_rsp_reg_write(rsp, N64_SP_REG_RD_LEN, 0x0F);
    CHECK_EQ(memcmp(rsp->dmem + 0x120, rdram + 0x400, 16), 0);
    CHECK_EQ(rsp->dmem[0x130], 0);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_MEM_ADDR), 0x130);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_DRAM_ADDR), 0x410);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_RD_LEN), 0xFF8);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_WR_LEN), 0xFF8);

    /* A length of 1 still moves 8 bytes. */
    n64_rsp_reset(rsp);
    n64_rsp_reg_write(rsp, N64_SP_REG_RD_LEN, 0x01);
    CHECK_EQ(memcmp(rsp->dmem, rdram, 8), 0);
    CHECK_EQ(rsp->dmem[8], 0);

    /* Rows: 3 rows of 8 bytes with a skip of 16 between them in RDRAM;
     * DMEM is written contiguously. */
    n64_rsp_reset(rsp);
    n64_rsp_reg_write(rsp, N64_SP_REG_MEM_ADDR, 0x200);
    n64_rsp_reg_write(rsp, N64_SP_REG_DRAM_ADDR, 0x1000);
    n64_rsp_reg_write(rsp, N64_SP_REG_RD_LEN, (16u << 20) | (2u << 12) | 0x7u);
    CHECK_EQ(memcmp(rsp->dmem + 0x200, rdram + 0x1000, 8), 0);
    CHECK_EQ(memcmp(rsp->dmem + 0x208, rdram + 0x1018, 8), 0);
    CHECK_EQ(memcmp(rsp->dmem + 0x210, rdram + 0x1030, 8), 0);
    CHECK_EQ(rsp->dmem[0x218], 0);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_MEM_ADDR), 0x218);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_DRAM_ADDR), 0x1038); /* no skip after the last row */
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_RD_LEN), (16u << 20) | 0xFF8u);

    /* IMEM select, and wrap within IMEM: 0x1FF8 + 16 bytes. */
    n64_rsp_reset(rsp);
    n64_rsp_reg_write(rsp, N64_SP_REG_MEM_ADDR, 0x1FF8);
    n64_rsp_reg_write(rsp, N64_SP_REG_DRAM_ADDR, 0x2000);
    n64_rsp_reg_write(rsp, N64_SP_REG_RD_LEN, 0xF);
    CHECK_EQ(memcmp(rsp->imem + 0xFF8, rdram + 0x2000, 8), 0);
    CHECK_EQ(memcmp(rsp->imem, rdram + 0x2008, 8), 0);
    CHECK_EQ(rsp->dmem[0xFF8], 0);
    CHECK_EQ(rsp->dmem[0], 0);
    CHECK_EQ(n64_rsp_reg_read(rsp, N64_SP_REG_MEM_ADDR), 0x1008);

    /* DMEM -> RDRAM. */
    n64_rsp_reset(rsp);
    for (i = 0; i < 0x1000; i++) rsp->dmem[i] = (uint8_t)(0xA0 + i);
    n64_rsp_reg_write(rsp, N64_SP_REG_MEM_ADDR, 0x40);
    n64_rsp_reg_write(rsp, N64_SP_REG_DRAM_ADDR, 0x3000);
    n64_rsp_reg_write(rsp, N64_SP_REG_WR_LEN, 0x17);
    CHECK_EQ(memcmp(rdram + 0x3000, rsp->dmem + 0x40, 24), 0);
    CHECK_EQ(rdram[0x3018], (uint8_t)(0x3018u * 7u + 0x30u));

    /* Beyond rdram_size: reads give zero, writes are dropped. */
    n64_rsp_reset(rsp);
    memset(rsp->dmem, 0x55, sizeof rsp->dmem);
    n64_rsp_reg_write(rsp, N64_SP_REG_DRAM_ADDR, 0xFFF8);
    n64_rsp_reg_write(rsp, N64_SP_REG_RD_LEN, 0xF);
    CHECK_EQ(memcmp(rsp->dmem, rdram + 0xFFF8, 8), 0);
    for (i = 8; i < 16; i++) CHECK_EQ(rsp->dmem[i], 0);
    n64_rsp_reg_write(rsp, N64_SP_REG_MEM_ADDR, 0x100);
    n64_rsp_reg_write(rsp, N64_SP_REG_DRAM_ADDR, 0xFFF8);
    n64_rsp_reg_write(rsp, N64_SP_REG_WR_LEN, 0xF);
    CHECK_EQ(rdram[0xFFF8], 0x55);
    for (i = 0x10000; i < 0x10010; i++) CHECK_EQ(rdram[i], (uint8_t)(i * 7u + (i >> 8)));

    /* The RSP starts a DMA itself through COP0. */
    {
        const uint32_t prog[] = {
            ORI(1, 0, 0x80), MTC0(1, 0),  /* MEM_ADDR */
            ORI(2, 0, 0x500), MTC0(2, 1), /* DRAM_ADDR */
            ORI(3, 0, 0x1F), MTC0(3, 2),  /* RD_LEN: 32 bytes */
            LW(4, 0, 0x80),
            MFC0(5, 0),
        };
        n64_rsp_reset(rsp);
        run_words(rsp, prog, sizeof prog / sizeof prog[0]);
        CHECK_EQ(memcmp(rsp->dmem + 0x80, rdram + 0x500, 32), 0);
        CHECK_EQ(rsp->r[4], n64_be32(rdram + 0x500));
        CHECK_EQ(rsp->r[5], 0xA0);
    }
}

/* Vector unit: moves, loads, stores. */

static void test_vector_moves(N64Rsp *rsp)
{
    const uint32_t prog[] = {
        MFC2(1, 1, 1),  /* bytes 1,2 */
        MFC2(2, 1, 15), /* bytes 15,0: FF 00, sign-extended */
        MFC2(3, 1, 4),
        ORI(4, 0, 0x1234),
        MTC2(4, 2, 15), /* only byte 15 */
        ORI(5, 0, 0xABCD),
        MTC2(5, 2, 3),  /* bytes 3,4 */
        ORI(6, 0, 0x1234),
        CTC2(6, 0),
        CFC2(7, 0),
        ADDIU(8, 0, -1),
        CTC2(8, 1),
        CFC2(9, 1),     /* FFFF sign-extended */
        ORI(10, 0, 0x1A5),
        CTC2(10, 2),    /* VCE keeps 8 bits */
        CFC2(11, 2),
        CFC2(12, 3),    /* 3 also reads VCE */
        MFC2(0, 1, 0),
    };
    n64_rsp_reset(rsp);
    set_vec(rsp->vu.v[1], 0x0011, 0x2233, 0x4455, 0x6677, 0x8899, 0xAABB, 0xCCDD, 0xEEFF);
    set_vec(rsp->vu.v[2], 0, 0, 0, 0, 0, 0, 0, 0);
    run_words(rsp, prog, sizeof prog / sizeof prog[0]);
    CHECK_EQ(rsp->r[1], 0x1122);
    CHECK_EQ(rsp->r[2], 0xFFFFFF00u);
    CHECK_EQ(rsp->r[3], 0x4455);
    CHECK_VEC(rsp->vu.v[2], 0x0000, 0x00AB, 0xCD00, 0, 0, 0, 0, 0x0012);
    CHECK_EQ(rsp->r[7], 0x1234);
    CHECK_EQ(rsp->vu.vco_lo, 0x34);
    CHECK_EQ(rsp->vu.vco_hi, 0x12);
    CHECK_EQ(rsp->r[9], 0xFFFFFFFFu);
    CHECK_EQ(rsp->r[11], 0xA5);
    CHECK_EQ(rsp->r[12], 0xA5);
    CHECK_EQ(rsp->r[0], 0);
}

static void fill_dmem_pattern(N64Rsp *rsp)
{
    unsigned i;
    for (i = 0; i < 0x1000; i++) rsp->dmem[i] = (uint8_t)i;
}

static void fill_vec(uint16_t *v, uint16_t x)
{
    unsigned n;
    for (n = 0; n < 8; n++) v[n] = x;
}

static void test_vector_loads(N64Rsp *rsp)
{
    unsigned r;
    const uint32_t prog[] = {
        lwc2(LQV, 1, 0, 0, 2),     /* 0x20 */
        lwc2(LQV, 2, 0, 3, 0),     /* r3 = 0x24: up to the end of the line */
        lwc2(LQV, 3, 4, 0, 2),     /* element 4 */
        lwc2(LRV, 4, 0, 3, 0),     /* 0x24: the 4 bytes before, into the end */
        lwc2(LDV, 5, 0, 0, 5),     /* 0x28 */
        lwc2(LDV, 6, 12, 0, 5),    /* element 12 stops at byte 15 */
        lwc2(LSV, 7, 3, 4, 0),     /* r4 = 0x31, element 3 */
        lwc2(LLV, 8, 14, 0, 0x10), /* 0x40, element 14 */
        lwc2(LBV, 9, 15, 0, 0x7F), /* r0 + 0x7F; offset is 7 bits signed... */
        lwc2(LDV, 10, 0, 5, 0),    /* r5 = 0xFFC: wraps */
        lwc2(LQV, 11, 0, 6, -1),   /* r6 = 0x30, offset -16 */
        lwc2(LPV, 12, 0, 0, 3),    /* 0x18 */
        lwc2(LPV, 13, 0, 7, 0),    /* r7 = 0x19 */
        lwc2(LPV, 14, 1, 0, 3),    /* 0x18, element 1 */
        lwc2(LUV, 15, 0, 0, 3),
        lwc2(LHV, 16, 0, 0, 1),    /* 0x10 */
        lwc2(LFV, 17, 0, 0, 1),
        lwc2(LFV, 18, 8, 0, 1),
        lwc2(LWV, 19, 0, 0, 1),    /* not on the RSP: nothing */
    };
    const uint32_t setup[] = {ADDIU(3, 0, 0x24), ADDIU(4, 0, 0x31), ADDIU(5, 0, 0xFFC), ADDIU(6, 0, 0x30),
                              ADDIU(7, 0, 0x19)};
    uint32_t all[32];
    memcpy(all, setup, sizeof setup);
    memcpy(all + 5, prog, sizeof prog);
    n64_rsp_reset(rsp);
    fill_dmem_pattern(rsp);
    for (r = 0; r < 32; r++) fill_vec(rsp->vu.v[r], 0xAAAA);
    run_words(rsp, all, 5 + sizeof prog / sizeof prog[0]);
    CHECK_VEC(rsp->vu.v[1], 0x2021, 0x2223, 0x2425, 0x2627, 0x2829, 0x2A2B, 0x2C2D, 0x2E2F);
    CHECK_VEC(rsp->vu.v[2], 0x2425, 0x2627, 0x2829, 0x2A2B, 0x2C2D, 0x2E2F, 0xAAAA, 0xAAAA);
    CHECK_VEC(rsp->vu.v[3], 0xAAAA, 0xAAAA, 0x2021, 0x2223, 0x2425, 0x2627, 0x2829, 0x2A2B);
    CHECK_VEC(rsp->vu.v[4], 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0x2021, 0x2223);
    CHECK_VEC(rsp->vu.v[5], 0x2829, 0x2A2B, 0x2C2D, 0x2E2F, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA);
    CHECK_VEC(rsp->vu.v[6], 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0x2829, 0x2A2B);
    CHECK_VEC(rsp->vu.v[7], 0xAAAA, 0xAA31, 0x32AA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA);
    CHECK_VEC(rsp->vu.v[8], 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0x4041);
    /* 0x7F as a 7-bit offset is -1: r0 - 1 wraps to 0xFFF. */
    CHECK_VEC(rsp->vu.v[9], 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAFF);
    CHECK_VEC(rsp->vu.v[10], 0xFCFD, 0xFEFF, 0x0001, 0x0203, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA);
    CHECK_VEC(rsp->vu.v[11], 0x2021, 0x2223, 0x2425, 0x2627, 0x2829, 0x2A2B, 0x2C2D, 0x2E2F);
    CHECK_VEC(rsp->vu.v[12], 0x1800, 0x1900, 0x1A00, 0x1B00, 0x1C00, 0x1D00, 0x1E00, 0x1F00);
    CHECK_VEC(rsp->vu.v[13], 0x1900, 0x1A00, 0x1B00, 0x1C00, 0x1D00, 0x1E00, 0x1F00, 0x2000);
    CHECK_VEC(rsp->vu.v[14], 0x2700, 0x1800, 0x1900, 0x1A00, 0x1B00, 0x1C00, 0x1D00, 0x1E00);
    CHECK_VEC(rsp->vu.v[15], 0x0C00, 0x0C80, 0x0D00, 0x0D80, 0x0E00, 0x0E80, 0x0F00, 0x0F80);
    CHECK_VEC(rsp->vu.v[16], 0x0800, 0x0900, 0x0A00, 0x0B00, 0x0C00, 0x0D00, 0x0E00, 0x0F00);
    CHECK_VEC(rsp->vu.v[17], 0x0800, 0x0A00, 0x0C00, 0x0E00, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA);
    CHECK_VEC(rsp->vu.v[18], 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0x0800, 0x0A00, 0x0C00, 0x0E00);
    CHECK_VEC(rsp->vu.v[19], 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA);
}

static void test_vector_ltv(N64Rsp *rsp)
{
    unsigned r;
    const uint32_t prog[] = {
        lwc2(LTV, 8, 0, 0, 2),  /* 0x20: diagonal of v8..v15 */
        lwc2(LTV, 16, 2, 0, 2), /* element 2: starts at v17, wraps the line */
        lwc2(LTV, 24, 0, 1, 0), /* r1 = 0x28: bit 3 offsets the start */
    };
    const uint32_t all[] = {ADDIU(1, 0, 0x28), prog[0], prog[1], prog[2]};
    n64_rsp_reset(rsp);
    fill_dmem_pattern(rsp);
    for (r = 0; r < 32; r++) fill_vec(rsp->vu.v[r], 0xAAAA);
    run_words(rsp, all, 4);
    for (r = 0; r < 8; r++) {
        CHECK_EQ(rsp->vu.v[8 + r][r], 0x2021u + 0x0202u * r);
        CHECK_EQ(rsp->vu.v[8 + r][(r + 1) & 7], 0xAAAA);
    }
    /* Element 2: v17 lane 0 = 22 23, v18 lane 1 = 24 25, ..., v23 lane 6 =
     * 2E 2F, then the read wraps to 0x20: v16 lane 7 = 20 21. */
    for (r = 0; r < 7; r++) CHECK_EQ(rsp->vu.v[17 + r][r], 0x2223u + 0x0202u * r);
    CHECK_EQ(rsp->vu.v[16][7], 0x2021);
    /* 0x28: the line is 0x28-0x37 and the read starts 8 bytes in. */
    for (r = 0; r < 4; r++) CHECK_EQ(rsp->vu.v[24 + r][r], 0x3031u + 0x0202u * r);
    for (r = 4; r < 8; r++) CHECK_EQ(rsp->vu.v[24 + r][r], 0x2829u + 0x0202u * (r - 4));
}

static void test_vector_stores(N64Rsp *rsp)
{
    static const uint8_t reg_bytes[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                          0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    unsigned i, r;
    struct {
        uint32_t word;
        uint32_t base;  /* r1 */
        uint32_t at;    /* where to compare */
        uint8_t want[16];
    } cases[] = {
        {swc2(SQV, 1, 0, 1, 0), 0x104, 0x100,
         {0xEE, 0xEE, 0xEE, 0xEE, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}},
        {swc2(SQV, 1, 2, 1, 0), 0x100, 0x100,
         {0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11}},
        {swc2(SRV, 1, 0, 1, 0), 0x104, 0x100,
         {0xCC, 0xDD, 0xEE, 0xFF, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        {swc2(SDV, 1, 0, 1, 1), 0x100, 0x100,
         {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}},
        {swc2(SDV, 1, 12, 1, 0), 0x100, 0x100,
         {0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        {swc2(SSV, 1, 15, 1, 0), 0x101, 0x100,
         {0xEE, 0xFF, 0x00, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        {swc2(SLV, 1, 4, 1, -1), 0x104, 0x100,
         {0x44, 0x55, 0x66, 0x77, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        {swc2(SBV, 1, 5, 1, 3), 0x100, 0x100,
         {0xEE, 0xEE, 0xEE, 0x55, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        /* SPV: lane tops; from element 8 on, bits 14..7 instead. */
        {swc2(SPV, 1, 0, 1, 0), 0x100, 0x100,
         {0x00, 0x22, 0x44, 0x66, 0x88, 0xAA, 0xCC, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        {swc2(SPV, 1, 8, 1, 0), 0x100, 0x100,
         {0x00, 0x44, 0x88, 0xCC, 0x11, 0x55, 0x99, 0xDD, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        {swc2(SUV, 1, 0, 1, 0), 0x100, 0x100,
         {0x00, 0x44, 0x88, 0xCC, 0x11, 0x55, 0x99, 0xDD, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        {swc2(SUV, 1, 8, 1, 0), 0x100, 0x100,
         {0x00, 0x22, 0x44, 0x66, 0x88, 0xAA, 0xCC, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}},
        /* SHV: bits 14..7 of each lane to every other byte. */
        {swc2(SHV, 1, 0, 1, 0), 0x100, 0x100,
         {0x00, 0xEE, 0x44, 0xEE, 0x88, 0xEE, 0xCC, 0xEE, 0x11, 0xEE, 0x55, 0xEE, 0x99, 0xEE, 0xDD, 0xEE}},
        /* SHV from an unaligned address rotates within the 16 bytes. */
        {swc2(SHV, 1, 0, 1, 0), 0x103, 0x100,
         {0xEE, 0xDD, 0xEE, 0x00, 0xEE, 0x44, 0xEE, 0x88, 0xEE, 0xCC, 0xEE, 0x11, 0xEE, 0x55, 0xEE, 0x99}},
        {swc2(SFV, 1, 0, 1, 0), 0x100, 0x100,
         {0x00, 0xEE, 0xEE, 0xEE, 0x44, 0xEE, 0xEE, 0xEE, 0x88, 0xEE, 0xEE, 0xEE, 0xCC, 0xEE, 0xEE, 0xEE}},
        {swc2(SFV, 1, 8, 1, 0), 0x100, 0x100,
         {0x11, 0xEE, 0xEE, 0xEE, 0x55, 0xEE, 0xEE, 0xEE, 0x99, 0xEE, 0xEE, 0xEE, 0xDD, 0xEE, 0xEE, 0xEE}},
        {swc2(SFV, 1, 1, 1, 0), 0x100, 0x100,
         {0x99, 0xEE, 0xEE, 0xEE, 0xDD, 0xEE, 0xEE, 0xEE, 0x11, 0xEE, 0xEE, 0xEE, 0x55, 0xEE, 0xEE, 0xEE}},
        {swc2(SFV, 1, 2, 1, 0), 0x100, 0x100,
         {0x00, 0xEE, 0xEE, 0xEE, 0x00, 0xEE, 0xEE, 0xEE, 0x00, 0xEE, 0xEE, 0xEE, 0x00, 0xEE, 0xEE, 0xEE}},
        /* SWV: the register from element e, written from the address's
         * offset round the 8-aligned 16 bytes. */
        {swc2(SWV, 1, 0, 1, 0), 0x104, 0x100,
         {0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}},
        {swc2(SWV, 1, 2, 1, 0), 0x100, 0x100,
         {0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11}},
        /* STV: v16..v23 hold lane l of register r as (r << 8) | l; element 0
         * stores the diagonal, element 2 starts one register later. */
        {swc2(STV, 16, 0, 1, 0), 0x100, 0x100,
         {0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06, 0x07, 0x07}},
        {swc2(STV, 16, 2, 1, 0), 0x100, 0x100,
         {0x01, 0x00, 0x02, 0x01, 0x03, 0x02, 0x04, 0x03, 0x05, 0x04, 0x06, 0x05, 0x07, 0x06, 0x00, 0x07}},
        /* SQV near the end of DMEM stops at its line; SLV wraps DMEM. */
        {swc2(SQV, 1, 0, 1, 0), 0xFF8, 0xFF0,
         {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}},
        {swc2(SLV, 1, 0, 1, 0), 0xFFE, 0xFF0,
         {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0x00, 0x11}},
    };
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t prog[3];
        unsigned k;
        n64_rsp_reset(rsp);
        memset(rsp->dmem, 0xEE, sizeof rsp->dmem);
        for (k = 0; k < 16; k++) {
            uint16_t *v = rsp->vu.v[1];
            v[k >> 1] = (uint16_t)((k & 1) ? (v[k >> 1] | reg_bytes[k]) : (reg_bytes[k] << 8));
        }
        for (r = 0; r < 8; r++)
            for (k = 0; k < 8; k++) rsp->vu.v[16 + r][k] = (uint16_t)((r << 8) | k);
        prog[0] = LUI(1, 0);
        prog[1] = ORI(1, 1, cases[i].base);
        prog[2] = cases[i].word;
        run_words(rsp, prog, 3);
        for (k = 0; k < 16; k++) {
            if (rsp->dmem[(cases[i].at + k) & 0xFFFu] != cases[i].want[k]) {
                fprintf(stderr, "%s:%d: store case %u byte %u = %02X, expected %02X\n", __FILE__, __LINE__, i, k,
                        rsp->dmem[(cases[i].at + k) & 0xFFFu], cases[i].want[k]);
                g_failures++;
                break;
            }
        }
        g_checks++;
        /* SLV at 0xFFE also wrote 0x000-0x001. */
        if (i == sizeof cases / sizeof cases[0] - 1) {
            CHECK_EQ(rsp->dmem[0], 0x22);
            CHECK_EQ(rsp->dmem[1], 0x33);
            CHECK_EQ(rsp->dmem[2], 0xEE);
        }
    }
}

/* Vector unit: computational ops. */

/* Runs one computational op with vs in v1, vt in v2 and vd in v3. */
static void run_vop(N64Rsp *rsp, unsigned funct, unsigned e) { run1(rsp, vop(funct, 3, 1, 2, e)); }

static void vreset(N64Rsp *rsp)
{
    unsigned n;
    n64_rsp_reset(rsp);
    for (n = 0; n < 8; n++) rsp->vu.v[3][n] = 0xDEAD;
}

static void test_element_select(N64Rsp *rsp)
{
    unsigned e;
    /* VOR with vs = 0 copies vt with the element selection applied. */
    static const uint16_t want[16][8] = {
        {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17}, {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17},
        {0x10, 0x10, 0x12, 0x12, 0x14, 0x14, 0x16, 0x16}, {0x11, 0x11, 0x13, 0x13, 0x15, 0x15, 0x17, 0x17},
        {0x10, 0x10, 0x10, 0x10, 0x14, 0x14, 0x14, 0x14}, {0x11, 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x15},
        {0x12, 0x12, 0x12, 0x12, 0x16, 0x16, 0x16, 0x16}, {0x13, 0x13, 0x13, 0x13, 0x17, 0x17, 0x17, 0x17},
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
        {0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12}, {0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13},
        {0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14}, {0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15},
        {0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16}, {0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17},
    };
    for (e = 0; e < 16; e++) {
        vreset(rsp);
        set_vec(rsp->vu.v[2], 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17);
        run_vop(rsp, VOR, e);
        check_vec_at(__LINE__, "element select", rsp->vu.v[3], want[e]);
    }
    /* vd aliasing a source: VADD v1 = v1 + v1[0]. */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 1, 2, 3, 4, 5, 6, 7, 8);
    run1(rsp, vop(VADD, 1, 1, 1, 8));
    CHECK_VEC(rsp->vu.v[1], 2, 3, 4, 5, 6, 7, 8, 9);
}

static void test_vmulf_vmulu(N64Rsp *rsp)
{
    /* acc = vs * vt * 2 + 0x8000 per lane:
     *   8000*8000: 0x0000_8000_8000, clamps to 7FFF (VMULU: FFFF)
     *   8000*7FFF: -0x7FFE8000 = 0xFFFF_8001_8000 -> 8001 (VMULU: 0)
     *   4000*4000: 0x2000_8000 -> 2000
     *   7FFF*7FFF: 0x7FFE_8002 -> 7FFE
     *   FFFF*4000: -0x8000 + 0x8000 = 0 -> 0
     *   0001*4000: 0x8000 + 0x8000 = 0x1_0000: rounding carries -> 0001
     *   0000*1234: 0x8000 -> 0
     *   C000*4000: -0x20000000 + 0x8000 = 0xFFFF_E000_8000 -> E000 (VMULU: 0) */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x8000, 0x8000, 0x4000, 0x7FFF, 0xFFFF, 0x0001, 0x0000, 0xC000);
    set_vec(rsp->vu.v[2], 0x8000, 0x7FFF, 0x4000, 0x7FFF, 0x4000, 0x4000, 0x1234, 0x4000);
    run_vop(rsp, VMULF, 0);
    CHECK_VEC(rsp->vu.v[3], 0x7FFF, 0x8001, 0x2000, 0x7FFE, 0x0000, 0x0001, 0x0000, 0xE000);
    CHECK_VEC(rsp->vu.acc_h, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF);
    CHECK_VEC(rsp->vu.acc_m, 0x8000, 0x8001, 0x2000, 0x7FFE, 0x0000, 0x0001, 0x0000, 0xE000);
    CHECK_VEC(rsp->vu.acc_l, 0x8000, 0x8000, 0x8000, 0x8002, 0x0000, 0x0000, 0x8000, 0x8000);
    run_vop(rsp, VMULU, 0);
    CHECK_VEC(rsp->vu.v[3], 0xFFFF, 0x0000, 0x2000, 0x7FFE, 0x0000, 0x0001, 0x0000, 0x0000);
    CHECK_VEC(rsp->vu.acc_m, 0x8000, 0x8001, 0x2000, 0x7FFE, 0x0000, 0x0001, 0x0000, 0xE000);
}

static void test_vmacf_vmacu(N64Rsp *rsp)
{
    /* acc += vs * vt * 2:
     *   0x0000_7FFF_0000 + 4000*0002*2 = 0x0000_8000_0000 -> 7FFF (U: FFFF)
     *   0xFFFF_8000_0000 + FFFF*0001*2 = 0xFFFF_7FFF_FFFE -> 8000 (U: 0)
     *   0 + 1000*1000*2 = 0x0200_0000 -> 0200 (U: 0200)
     *   0x0001_0000_0000 + 0 -> 7FFF (U: FFFF)
     *   0x7FFF_FFFF_FFFF + 7FFF*7FFF*2 wraps at 48 bits to
     *   0x8000_7FFE_0001 -> 8000 (U: 0) */
    static const uint64_t start[8] = {0x00007FFF0000ull, 0xFFFF80000000ull, 0, 0x000100000000ull,
                                      0x7FFFFFFFFFFFull, 0, 0, 0};
    unsigned n, pass;
    for (pass = 0; pass < 2; pass++) {
        vreset(rsp);
        for (n = 0; n < 8; n++) set_acc(rsp, n, start[n]);
        set_vec(rsp->vu.v[1], 0x4000, 0xFFFF, 0x1000, 0x0000, 0x7FFF, 0, 0, 0);
        set_vec(rsp->vu.v[2], 0x0002, 0x0001, 0x1000, 0x1234, 0x7FFF, 0, 0, 0);
        run_vop(rsp, pass ? VMACU : VMACF, 0);
        if (!pass)
            CHECK_VEC(rsp->vu.v[3], 0x7FFF, 0x8000, 0x0200, 0x7FFF, 0x8000, 0, 0, 0);
        else
            CHECK_VEC(rsp->vu.v[3], 0xFFFF, 0x0000, 0x0200, 0xFFFF, 0x0000, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_h, 0x0000, 0xFFFF, 0x0000, 0x0001, 0x8000, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_m, 0x8000, 0x7FFF, 0x0200, 0x0000, 0x7FFE, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_l, 0x0000, 0xFFFE, 0x0000, 0x0000, 0x0001, 0, 0, 0);
    }
    /* Two VMACFs accumulate: 0x4000 * 0x4000 * 2 twice = 0x4000_0000. */
    vreset(rsp);
    fill_vec(rsp->vu.v[1], 0x4000);
    fill_vec(rsp->vu.v[2], 0x4000);
    run_vop(rsp, VMACF, 0);
    run_vop(rsp, VMACF, 0);
    CHECK_EQ(rsp->vu.acc_m[0], 0x4000);
    CHECK_EQ(rsp->vu.v[3][0], 0x4000);
}

static void test_vmud(N64Rsp *rsp)
{
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0xFFFF, 0x8000, 0x0001, 0x7FFF, 0, 0, 0, 0);
    set_vec(rsp->vu.v[2], 0xFFFF, 0x8000, 0x0001, 0x8000, 0, 0, 0, 0);
    /* VMUDL: (u * u) >> 16: FFFE0001 -> FFFE, 40000000 -> 4000, 1 -> 0,
     * 3FFF8000 -> 3FFF. */
    run_vop(rsp, VMUDL, 0);
    CHECK_VEC(rsp->vu.v[3], 0xFFFE, 0x4000, 0x0000, 0x3FFF, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_h, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_m, 0, 0, 0, 0, 0, 0, 0, 0);
    /* VMUDM: s * u: -1 * 65535 = FFFF_FFFF_0001 -> FFFF; -32768 * 32768 =
     * -0x40000000 = FFFF_C000_0000 -> C000; 1 * 1 -> 0;
     * 7FFF * 8000 = 3FFF_8000 -> 3FFF. */
    run_vop(rsp, VMUDM, 0);
    CHECK_VEC(rsp->vu.v[3], 0xFFFF, 0xC000, 0x0000, 0x3FFF, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_h, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_l, 0x0001, 0x0000, 0x0001, 0x8000, 0, 0, 0, 0);
    /* VMUDN: u * s: 65535 * -1 -> low 0001; 32768 * -32768 -> low 0000;
     * 1 -> 1; 7FFF * -32768 = -0x3FFF8000 -> low 8000. */
    run_vop(rsp, VMUDN, 0);
    CHECK_VEC(rsp->vu.v[3], 0x0001, 0x0000, 0x0001, 0x8000, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_h, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_m, 0xFFFF, 0xC000, 0x0000, 0xC000, 0, 0, 0, 0);
    /* VMUDH: (s * s) << 16: 1 -> 0001; 0x40000000 << 16 clamps to 7FFF;
     * 1 -> 1; -0x3FFF8000 << 16 clamps to 8000. */
    run_vop(rsp, VMUDH, 0);
    CHECK_VEC(rsp->vu.v[3], 0x0001, 0x7FFF, 0x0001, 0x8000, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_h, 0x0000, 0x4000, 0x0000, 0xC000, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_m, 0x0001, 0x0000, 0x0001, 0x8000, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_l, 0, 0, 0, 0, 0, 0, 0, 0);
}

static void test_vmad(N64Rsp *rsp)
{
    unsigned n;
    /* VMADL: acc += (u * u) >> 16, result is the low slice clamped as
     * unsigned (0 below, FFFF above):
     *   0x0000_0000_FFFF + (FFFF*0002 >> 16 = 1)    = 0x0000_0001_0000 -> 0000
     *   0x0000_7FFF_FFFF + (FFFF*FFFF >> 16 = FFFE) = 0x0000_8000_FFFD -> FFFF
     *   0xFFFF_FFFF_0000 + (8000*8000 >> 16 = 4000) = 0xFFFF_FFFF_4000 -> 4000
     *   0xFFFF_0000_0000 + 0                                          -> 0000 */
    {
        static const uint64_t start[8] = {0xFFFFull, 0x7FFFFFFFull, 0xFFFFFFFF0000ull, 0xFFFF00000000ull};
        vreset(rsp);
        for (n = 0; n < 8; n++) set_acc(rsp, n, start[n]);
        set_vec(rsp->vu.v[1], 0xFFFF, 0xFFFF, 0x8000, 0, 0, 0, 0, 0);
        set_vec(rsp->vu.v[2], 0x0002, 0xFFFF, 0x8000, 0, 0, 0, 0, 0);
        run_vop(rsp, VMADL, 0);
        CHECK_VEC(rsp->vu.v[3], 0x0000, 0xFFFF, 0x4000, 0x0000, 0, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_m, 0x0001, 0x8000, 0xFFFF, 0x0000, 0, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_l, 0x0000, 0xFFFD, 0x4000, 0x0000, 0, 0, 0, 0);
    }
    /* VMADM: acc += s * u, result clamps the middle slice:
     *   0x0000_7FFF_0000 + 1*FFFF = 0x0000_7FFF_FFFF -> 7FFF
     *   0x0000_7FFF_8000 + 1*8000 = 0x0000_8000_0000 -> 7FFF (clamped)
     *   0 + -1*2                  = 0xFFFF_FFFF_FFFE -> FFFF
     *   0xFFFF_8000_0000 + -1*1   = 0xFFFF_7FFF_FFFF -> 8000 (clamped) */
    {
        static const uint64_t start[8] = {0x7FFF0000ull, 0x7FFF8000ull, 0, 0xFFFF80000000ull};
        vreset(rsp);
        for (n = 0; n < 8; n++) set_acc(rsp, n, start[n]);
        set_vec(rsp->vu.v[1], 0x0001, 0x0001, 0xFFFF, 0xFFFF, 0, 0, 0, 0);
        set_vec(rsp->vu.v[2], 0xFFFF, 0x8000, 0x0002, 0x0001, 0, 0, 0, 0);
        run_vop(rsp, VMADM, 0);
        CHECK_VEC(rsp->vu.v[3], 0x7FFF, 0x7FFF, 0xFFFF, 0x8000, 0, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_h, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_m, 0x7FFF, 0x8000, 0xFFFF, 0x7FFF, 0, 0, 0, 0);
    }
    /* VMADN: acc += u * s, result is the low slice clamped as unsigned:
     *   0 + FFFF*1               = 0x0000_0000_FFFF -> FFFF
     *   0 + 1*-1                 = 0xFFFF_FFFF_FFFF -> FFFF
     *   0x0000_0000_FFFF + 1*1   = 0x0000_0001_0000 -> 0000
     *   0xFFFF_FFFF_0000 + 1*-1  = 0xFFFF_FFFE_FFFF -> FFFF
     *   0x0000_8000_0000 + 0     -> FFFF (clamped high)
     *   0xFFFF_0000_0000 + 0     -> 0000 (clamped low) */
    {
        static const uint64_t start[8] = {0, 0, 0xFFFFull, 0xFFFFFFFF0000ull, 0x80000000ull, 0xFFFF00000000ull};
        vreset(rsp);
        for (n = 0; n < 8; n++) set_acc(rsp, n, start[n]);
        set_vec(rsp->vu.v[1], 0xFFFF, 0x0001, 0x0001, 0x0001, 0, 0, 0, 0);
        set_vec(rsp->vu.v[2], 0x0001, 0xFFFF, 0x0001, 0xFFFF, 0, 0, 0, 0);
        run_vop(rsp, VMADN, 0);
        CHECK_VEC(rsp->vu.v[3], 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0, 0);
        CHECK_VEC(rsp->vu.acc_m, 0x0000, 0xFFFF, 0x0001, 0xFFFE, 0x8000, 0x0000, 0, 0);
    }
    /* VMADH: acc += (s * s) << 16, result clamps the middle slice. */
    {
        static const uint64_t start[8] = {0x7FFF1234ull, 0, 0xFFFF80000000ull};
        vreset(rsp);
        for (n = 0; n < 8; n++) set_acc(rsp, n, start[n]);
        set_vec(rsp->vu.v[1], 0x0001, 0x0002, 0xFFFF, 0, 0, 0, 0, 0);
        set_vec(rsp->vu.v[2], 0x0001, 0x0003, 0x0001, 0, 0, 0, 0, 0);
        run_vop(rsp, VMADH, 0);
        CHECK_VEC(rsp->vu.v[3], 0x7FFF, 0x0006, 0x8000, 0, 0, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_m, 0x8000, 0x0006, 0x7FFF, 0, 0, 0, 0, 0);
        CHECK_VEC(rsp->vu.acc_l, 0x1234, 0x0000, 0x0000, 0, 0, 0, 0, 0);
    }
    /* The console's own result for VMUDH then VMADN on the same inputs
     * (n64-systemtest, "op_vector_arithmetic"; its assembler takes vd, vt,
     * vs, so vs is v1 and vt is v0). */
    {
        const uint32_t prog[] = {vop(VMUDH, 2, 1, 0, 0), vop(VMADN, 2, 1, 0, 0)};
        vreset(rsp);
        set_vec(rsp->vu.v[0], 0x7FFF, 0x7FFF, 0x7FFF, 0x0000, 0x0001, 0xFFFF, 0x7FFF, 0x8000);
        set_vec(rsp->vu.v[1], 0x7FFF, 0xFFFF, 0x0010, 0x0000, 0xFFFF, 0xFFFF, 0x7FFF, 0x8000);
        run_words(rsp, prog, 2);
        CHECK_VEC(rsp->vu.acc_h, 0x3FFF, 0xFFFF, 0x0007, 0x0000, 0xFFFF, 0x0000, 0x3FFF, 0x3FFF);
        CHECK_VEC(rsp->vu.acc_m, 0x4000, 0xFFFF, 0xFFF7, 0x0000, 0xFFFF, 0x0000, 0x4000, 0xC000);
        CHECK_VEC(rsp->vu.acc_l, 0x0001, 0x8001, 0xFFF0, 0x0000, 0xFFFF, 0x0001, 0x0001, 0x0000);
        CHECK_VEC(rsp->vu.v[2], 0xFFFF, 0x8001, 0xFFFF, 0x0000, 0xFFFF, 0x0001, 0xFFFF, 0xFFFF);
    }
}

static void test_vrnd_vmulq_vmacq(N64Rsp *rsp)
{
    unsigned n;
    /* VRNDN (vs field even: vt not shifted) adds only to negative lanes:
     *   -1 + 2 = 1 -> result (acc >> 16) 0; 5 stays 5 -> 0. */
    vreset(rsp);
    set_acc(rsp, 0, 0xFFFFFFFFFFFFull);
    set_acc(rsp, 1, 5);
    set_vec(rsp->vu.v[2], 2, 2, 0, 0, 0, 0, 0, 0);
    run1(rsp, vop(VRNDN, 3, 0, 2, 0));
    CHECK_EQ(rsp->vu.acc_l[0], 1);
    CHECK_EQ(rsp->vu.acc_h[0], 0);
    CHECK_EQ(rsp->vu.acc_l[1], 5);
    CHECK_VEC(rsp->vu.v[3], 0, 0, 0, 0, 0, 0, 0, 0);
    /* VRNDP with an odd vs field adds vt << 16 to non-negative lanes:
     *   0x0000_0001_0000 + 3<<16 = 0x0000_0004_0000 -> 0004
     *   0xFFFF_FFFF_0000 is negative: unchanged -> FFFF
     *   0x0000_7FFF_0000 + 1<<16 = 0x0000_8000_0000 -> 7FFF (clamped)
     *   0x7FFF_FFFF_0000 + 7FFF<<16 wraps to 0x8000_7FFE_0000 -> 8000 */
    vreset(rsp);
    set_acc(rsp, 0, 0x000000010000ull);
    set_acc(rsp, 1, 0xFFFFFFFF0000ull);
    set_acc(rsp, 2, 0x00007FFF0000ull);
    set_acc(rsp, 3, 0x7FFFFFFF0000ull);
    set_vec(rsp->vu.v[2], 3, 3, 1, 0x7FFF, 0, 0, 0, 0);
    run1(rsp, vop(VRNDP, 3, 1, 2, 0));
    CHECK_VEC(rsp->vu.v[3], 0x0004, 0xFFFF, 0x7FFF, 0x8000, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_h, 0x0000, 0xFFFF, 0x0000, 0x8000, 0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_m, 0x0004, 0xFFFF, 0x8000, 0x7FFE, 0, 0, 0, 0);

    /* VMULQ: product, +31 when negative, result bits clamped >> 1 & ~15:
     *   0010*0100 = 0x1000 -> 0x0800
     *   FFFF*0001 = -1 + 31 = 30 -> 15 & ~15 = 0
     *   FFF0*0002 = -32 + 31 = -1 -> FFF0
     *   7FFF*7FFF = 0x3FFF0001 -> 7FF0 (clamped)
     *   8000*7FFF = -0x3FFF8000 + 31 = 0xC000_801F -> 8000 (clamped) */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x0010, 0xFFFF, 0xFFF0, 0x7FFF, 0x8000, 0, 0, 0);
    set_vec(rsp->vu.v[2], 0x0100, 0x0001, 0x0002, 0x7FFF, 0x7FFF, 0, 0, 0);
    fill_vec(rsp->vu.acc_l, 0x1234);
    run_vop(rsp, VMULQ, 0);
    CHECK_VEC(rsp->vu.v[3], 0x0800, 0x0000, 0xFFF0, 0x7FF0, 0x8000, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_h, 0x0000, 0x0000, 0xFFFF, 0x3FFF, 0xC000, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_m, 0x1000, 0x001E, 0xFFFF, 0x0001, 0x801F, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_l, 0, 0, 0, 0, 0, 0, 0, 0);

    /* VMACQ moves bits 47..16 towards zero by 32 when bit 21 is clear:
     *   64 -> 32, result 16 & ~15 = 0010;  -64 -> -32, result FFF0;
     *   32 (bit 5 set) stays, 0010;  16 stays (below 32), 0000;
     *   0x7FFF0000 -> 0x7FFEFFE0, result clamps to 7FF0. */
    vreset(rsp);
    for (n = 0; n < 8; n++) rsp->vu.acc_l[n] = 0x5555;
    rsp->vu.acc_m[0] = 0x0040;
    rsp->vu.acc_h[1] = 0xFFFF, rsp->vu.acc_m[1] = 0xFFC0;
    rsp->vu.acc_m[2] = 0x0020;
    rsp->vu.acc_m[3] = 0x0010;
    rsp->vu.acc_h[4] = 0x7FFF;
    run_vop(rsp, VMACQ, 0);
    CHECK_VEC(rsp->vu.v[3], 0x0010, 0xFFF0, 0x0010, 0x0000, 0x7FF0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_h, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x7FFE, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_m, 0x0020, 0xFFE0, 0x0020, 0x0010, 0xFFE0, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_l, 0x5555, 0x5555, 0x5555, 0x5555, 0x5555, 0x5555, 0x5555, 0x5555);
}

static void test_vadd_vsub(N64Rsp *rsp)
{
    /* VADD folds in VCO low (lanes 0 and 4 here) and clamps; the
     * accumulator keeps the wrapped sum. */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x7FFF, 0x8000, 0x0001, 0xFFFF, 0x7FFF, 0x8000, 0x1234, 0);
    set_vec(rsp->vu.v[2], 0x0001, 0xFFFF, 0x0001, 0xFFFF, 0x0000, 0x8000, 0x4321, 0);
    rsp->vu.vco_lo = 0x11;
    rsp->vu.vco_hi = 0xFF;
    run_vop(rsp, VADD, 0);
    CHECK_VEC(rsp->vu.v[3], 0x7FFF, 0x8000, 0x0002, 0xFFFE, 0x7FFF, 0x8000, 0x5555, 0);
    CHECK_VEC(rsp->vu.acc_l, 0x8001, 0x7FFF, 0x0002, 0xFFFE, 0x8000, 0x0000, 0x5555, 0);
    CHECK_EQ(rsp->vu.vco_lo, 0);
    CHECK_EQ(rsp->vu.vco_hi, 0);

    /* VSUB: vs - vt - VCO low (lanes 2 and 4). */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x8000, 0x7FFF, 0x0005, 0x0000, 0x8000, 0, 0, 0);
    set_vec(rsp->vu.v[2], 0x0001, 0xFFFF, 0x0003, 0x0001, 0x0000, 0, 0, 0);
    rsp->vu.vco_lo = 0x14;
    run_vop(rsp, VSUB, 0);
    CHECK_VEC(rsp->vu.v[3], 0x8000, 0x7FFF, 0x0001, 0xFFFF, 0x8000, 0, 0, 0);
    CHECK_VEC(rsp->vu.acc_l, 0x7FFF, 0x8000, 0x0001, 0xFFFF, 0x7FFF, 0, 0, 0);
    CHECK_EQ(rsp->vu.vco_lo, 0);

    /* VABS: by the sign of vs; 0x8000 negated gives 7FFF in vd, 8000 in
     * the accumulator. */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0xFFFB, 0x0000, 0x0003, 0xFFFF, 0xFFFF, 0x7FFF, 0x8000, 0x0000);
    set_vec(rsp->vu.v[2], 0x1234, 0x1234, 0x1234, 0x8000, 0x0001, 0x8000, 0xFFFF, 0x8000);
    run_vop(rsp, VABS, 0);
    CHECK_VEC(rsp->vu.v[3], 0xEDCC, 0x0000, 0x1234, 0x7FFF, 0xFFFF, 0x8000, 0x0001, 0x0000);
    CHECK_VEC(rsp->vu.acc_l, 0xEDCC, 0x0000, 0x1234, 0x8000, 0xFFFF, 0x8000, 0x0001, 0x0000);

    /* VADDC: unsigned carry out to VCO low, VCO high cleared. */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0xFFFF, 0x8000, 0x0001, 0, 0, 0, 0, 0xF000);
    set_vec(rsp->vu.v[2], 0x0001, 0x8000, 0x0001, 0, 0, 0, 0, 0x1000);
    rsp->vu.vco_hi = 0xFF;
    run_vop(rsp, VADDC, 0);
    CHECK_VEC(rsp->vu.v[3], 0x0000, 0x0000, 0x0002, 0, 0, 0, 0, 0x0000);
    CHECK_EQ(rsp->vu.vco_lo, 0x83);
    CHECK_EQ(rsp->vu.vco_hi, 0);

    /* VSUBC: borrow to VCO low, not-equal to VCO high. */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x0001, 0x0003, 0x0005, 0x0000, 0xFFFF, 0x0050, 0, 0);
    set_vec(rsp->vu.v[2], 0x0003, 0x0003, 0x0001, 0xFFFF, 0xFFFF, 0x004F, 0, 0);
    run_vop(rsp, VSUBC, 0);
    CHECK_VEC(rsp->vu.v[3], 0xFFFE, 0x0000, 0x0004, 0x0001, 0x0000, 0x0001, 0, 0);
    CHECK_EQ(rsp->vu.vco_lo, 0x09);
    CHECK_EQ(rsp->vu.vco_hi, 0x2D);
}

static void test_vcompare(N64Rsp *rsp)
{
    /* vs/vt pairs from n64-systemtest's compare tests; VCO is set (both
     * bits) on lanes 0-3 and only carry on lanes 4-7. */
    static const unsigned ops[4] = {VLT, VEQ, VNE, VGE};
    static const uint16_t cc[4] = {0x8B, 0x10, 0xEF, 0x74};
    static const uint16_t vd[4][8] = {
        {0x1234, 0x1233, 0x1234, 0xF233, 0xF234, 0xF234, 0xF234, 0xF234},
        {0x1234, 0x1234, 0x1234, 0xF234, 0xF234, 0xF234, 0xF234, 0x1234},
        {0x1234, 0x1233, 0x1235, 0xF233, 0xF234, 0xF235, 0x1234, 0xF234},
        {0x1234, 0x1234, 0x1235, 0xF234, 0xF234, 0xF235, 0x1234, 0x1234},
    };
    unsigned i;
    for (i = 0; i < 4; i++) {
        vreset(rsp);
        set_vec(rsp->vu.v[1], 0x1234, 0x1233, 0x1235, 0xF233, 0xF234, 0xF235, 0x1234, 0xF234);
        set_vec(rsp->vu.v[2], 0x1234, 0x1234, 0x1234, 0xF234, 0xF234, 0xF234, 0xF234, 0x1234);
        rsp->vu.vco_lo = 0xFF;
        rsp->vu.vco_hi = 0x0F;
        rsp->vu.vcc_hi = 0xFF;
        rsp->vu.vce = 0x5A;
        run_vop(rsp, ops[i], 0);
        check_vec_at(__LINE__, "compare vd", rsp->vu.v[3], vd[i]);
        check_vec_at(__LINE__, "compare acc", rsp->vu.acc_l, vd[i]);
        CHECK_EQ(rsp->vu.vcc_lo, cc[i]);
        CHECK_EQ(rsp->vu.vcc_hi, 0);
        CHECK_EQ(rsp->vu.vco_lo, 0);
        CHECK_EQ(rsp->vu.vco_hi, 0);
        CHECK_EQ(rsp->vu.vce, 0x5A);
    }
}

static void test_vclip(N64Rsp *rsp)
{
    /* VCH, lane by lane:
     *   5 vs 3: same sign, 5-3 >= 0 -> vt; cc_hi; ne
     *   3 vs 5: same sign, < 0 -> vs; ne
     *   5 vs -5: signs differ, 5 + -5 = 0 <= 0 -> -vt = 5; cc_lo, cc_hi
     *            (vt < 0), co_lo; sum 0 so not ne
     *   5 vs -6: sum -1 -> -vt = 6; cc_lo, cc_hi, co_lo, vce; vs == ~vt
     *            so not ne
     *   5 vs -16: sum -11 -> 0x10; cc_lo, cc_hi, co_lo, ne
     *   -16 vs 5: sum -11 -> -5; cc_lo, co_lo, ne
     *   16 vs -5: sum 11 -> vs; cc_hi, co_lo, ne
     *   8000 vs 8000: same sign, diff 0 -> vt; cc_lo (vt < 0), cc_hi */
    const uint32_t prog[] = {vop(VCH, 3, 1, 2, 0), CFC2(1, 0), CFC2(2, 1), CFC2(3, 2)};
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x0005, 0x0003, 0x0005, 0x0005, 0x0005, 0xFFF0, 0x0010, 0x8000);
    set_vec(rsp->vu.v[2], 0x0003, 0x0005, 0xFFFB, 0xFFFA, 0xFFF0, 0x0005, 0xFFFB, 0x8000);
    run_words(rsp, prog, 4);
    CHECK_VEC(rsp->vu.v[3], 0x0003, 0x0003, 0x0005, 0x0006, 0x0010, 0xFFFB, 0x0010, 0x8000);
    CHECK_VEC(rsp->vu.acc_l, 0x0003, 0x0003, 0x0005, 0x0006, 0x0010, 0xFFFB, 0x0010, 0x8000);
    CHECK_EQ(rsp->vu.vcc_lo, 0xBC);
    CHECK_EQ(rsp->vu.vcc_hi, 0xDD);
    CHECK_EQ(rsp->vu.vco_lo, 0x7C);
    CHECK_EQ(rsp->vu.vco_hi, 0x73);
    CHECK_EQ(rsp->vu.vce, 0x08);
    CHECK_EQ(rsp->r[1], 0x737C);
    CHECK_EQ(rsp->r[2], 0xFFFFDDBCu);
    CHECK_EQ(rsp->r[3], 0x08);

    /* VCL, one case per path, flags set by hand:
     *   0: co_lo, co_hi, cc_lo set     -> -vt (FFFE), cc_lo kept
     *   1: co_lo, co_hi, cc_lo clear   -> vs
     *   2: co_lo, vce clear, FFFF+1 = 0 with carry: le = 0 && .. = 0 -> vs
     *   3: co_lo, vce set, same sums: le = zero || !carry = 1 -> -vt
     *   4: co_lo, 0+0 = 0 no carry: le = 1 -> -0 = 0
     *   5: co_hi only, cc_hi set       -> vt
     *   6: neither: 8000 >= 7FFF unsigned -> vt, cc_hi set
     *   7: neither: 0001 >= FFFF false -> vs, cc_hi cleared */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x1111, 0x1111, 0xFFFF, 0xFFFF, 0x0000, 0x1111, 0x8000, 0x0001);
    set_vec(rsp->vu.v[2], 0x0002, 0x0002, 0x0001, 0x0001, 0x0000, 0x2222, 0x7FFF, 0xFFFF);
    rsp->vu.vco_lo = 0x1F;
    rsp->vu.vco_hi = 0x23;
    rsp->vu.vce = 0x08;
    rsp->vu.vcc_lo = 0x05;
    rsp->vu.vcc_hi = 0xA0;
    run_vop(rsp, VCL, 0);
    CHECK_VEC(rsp->vu.v[3], 0xFFFE, 0x1111, 0xFFFF, 0xFFFF, 0x0000, 0x2222, 0x7FFF, 0x0001);
    CHECK_EQ(rsp->vu.vcc_lo, 0x19);
    CHECK_EQ(rsp->vu.vcc_hi, 0x60);
    CHECK_EQ(rsp->vu.vco_lo, 0);
    CHECK_EQ(rsp->vu.vco_hi, 0);
    CHECK_EQ(rsp->vu.vce, 0);

    /* VCR (one's complement):
     *   5 vs 3 -> vt, cc_hi;  3 vs 5 -> vs;
     *   5 vs -6: 5 - 6 + 1 = 0 <= 0 -> ~vt = 5; cc_lo, cc_hi
     *   5 vs -5: 5 - 5 + 1 = 1 -> vs; cc_hi
     *   -16 vs 5: -10 -> ~5 = FFFA; cc_lo
     *   -16 vs -8: same sign, -8 < 0 -> vs; cc_lo
     *   -8 vs -16: 8 >= 0 -> vt; cc_lo, cc_hi
     *   0 vs 0 -> vt; cc_hi */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x0005, 0x0003, 0x0005, 0x0005, 0xFFF0, 0xFFF0, 0xFFF8, 0x0000);
    set_vec(rsp->vu.v[2], 0x0003, 0x0005, 0xFFFA, 0xFFFB, 0x0005, 0xFFF8, 0xFFF0, 0x0000);
    rsp->vu.vco_lo = 0xFF;
    rsp->vu.vco_hi = 0xFF;
    rsp->vu.vce = 0xFF;
    run_vop(rsp, VCR, 0);
    CHECK_VEC(rsp->vu.v[3], 0x0003, 0x0003, 0x0005, 0x0005, 0xFFFA, 0xFFF0, 0xFFF0, 0x0000);
    CHECK_EQ(rsp->vu.vcc_lo, 0x74);
    CHECK_EQ(rsp->vu.vcc_hi, 0xCD);
    CHECK_EQ(rsp->vu.vco_lo, 0);
    CHECK_EQ(rsp->vu.vco_hi, 0);
    CHECK_EQ(rsp->vu.vce, 0);

    /* VMRG: vs where VCC low is set; clears VCO, keeps VCC. */
    vreset(rsp);
    set_vec(rsp->vu.v[1], 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888);
    set_vec(rsp->vu.v[2], 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF, 0xEFEF, 0xEFEF);
    rsp->vu.vcc_lo = 0xA5;
    rsp->vu.vco_lo = 0xFF;
    run_vop(rsp, VMRG, 0);
    CHECK_VEC(rsp->vu.v[3], 0x1111, 0xBBBB, 0x3333, 0xDDDD, 0xEEEE, 0x6666, 0xEFEF, 0x8888);
    CHECK_EQ(rsp->vu.vcc_lo, 0xA5);
    CHECK_EQ(rsp->vu.vco_lo, 0);
}

/* The clip ops against a per-lane model written from n64-systemtest's
 * formulas (which that suite checks on the console), over its operand
 * vectors, every element and its flag combinations. */
static int16_t s16_of(uint16_t v) { return (int16_t)((int32_t)v - ((v & 0x8000u) ? 0x10000 : 0)); }

static void test_vclip_reference(N64Rsp *rsp)
{
    static const uint16_t vs_in[8] = {0x8000, 0xFFFE, 0xFFFF, 0x0000, 0x0000, 0x0001, 0x7FFE, 0x7FFF};
    static const uint16_t vt_in[8] = {0x0000, 0x0001, 0x7FFE, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF, 0x0000};
    static const uint16_t vcos[4] = {0x0000, 0x00FF, 0xFF00, 0xFFFF};
    static const uint16_t vccs[4] = {0x0F33, 0x0000, 0xFFFF, 0xFFFF};
    static const uint8_t vces[4] = {0xA9, 0x00, 0xFF, 0x00};
    static const unsigned ops[3] = {VCH, VCL, VCR};
    static const uint8_t sel[16][8] = {
        {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7}, {0, 0, 2, 2, 4, 4, 6, 6}, {1, 1, 3, 3, 5, 5, 7, 7},
        {0, 0, 0, 0, 4, 4, 4, 4}, {1, 1, 1, 1, 5, 5, 5, 5}, {2, 2, 2, 2, 6, 6, 6, 6}, {3, 3, 3, 3, 7, 7, 7, 7},
        {0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 2}, {3, 3, 3, 3, 3, 3, 3, 3},
        {4, 4, 4, 4, 4, 4, 4, 4}, {5, 5, 5, 5, 5, 5, 5, 5}, {6, 6, 6, 6, 6, 6, 6, 6}, {7, 7, 7, 7, 7, 7, 7, 7},
    };
    unsigned op, e, f, g, n;
    int bad = 0;
    for (op = 0; op < 3; op++)
        for (e = 0; e < 16; e++)
            for (f = 0; f < 4; f++)
                for (g = 0; g < 4; g++) {
                    uint16_t want[8];
                    unsigned co_lo = vcos[f] & 0xFF, co_hi = vcos[f] >> 8;
                    unsigned cc_lo = vccs[g] & 0xFF, cc_hi = vccs[g] >> 8, ce = vces[g];
                    unsigned out_cc_lo = 0, out_cc_hi = 0, out_co_lo = 0, out_co_hi = 0, out_ce = 0;
                    vreset(rsp);
                    memcpy(rsp->vu.v[1], vs_in, sizeof vs_in);
                    memcpy(rsp->vu.v[2], vt_in, sizeof vt_in);
                    rsp->vu.vco_lo = (uint16_t)co_lo, rsp->vu.vco_hi = (uint16_t)co_hi;
                    rsp->vu.vcc_lo = (uint16_t)cc_lo, rsp->vu.vcc_hi = (uint16_t)cc_hi;
                    rsp->vu.vce = (uint8_t)ce;
                    run_vop(rsp, ops[op], e);
                    for (n = 0; n < 8; n++) {
                        uint16_t s = vs_in[n], t = vt_in[sel[e][n]];
                        int i1 = s16_of(t), i2 = s16_of(s); /* the suite's source1 is vt */
                        unsigned l = (cc_lo >> n) & 1, h = (cc_hi >> n) & 1;
                        if (ops[op] == VCH) {
                            unsigned lo = (i1 ^ i2) < 0, hi, x = 0;
                            if (lo) {
                                int sum = s16_of((uint16_t)(i1 + i2));
                                h = i1 < 0, l = sum <= 0, x = sum == -1;
                                hi = sum != 0 && s != (uint16_t)~t;
                                want[n] = l ? (uint16_t)-i1 : s;
                            } else {
                                l = i1 < 0, h = i2 - i1 >= 0;
                                hi = i2 - i1 != 0;
                                want[n] = h ? t : s;
                            }
                            out_co_lo |= lo << n, out_co_hi |= hi << n, out_ce |= x << n;
                        } else if (ops[op] == VCL) {
                            if ((co_lo >> n) & 1) {
                                unsigned sum0 = ((s + t) & 0xFFFF) == 0, carry = s + t > 0xFFFF;
                                if (!((co_hi >> n) & 1)) l = (sum0 && !carry) || (((ce >> n) & 1) && (sum0 || !carry));
                                want[n] = l ? (uint16_t)-i1 : s;
                            } else {
                                if (!((co_hi >> n) & 1)) h = s >= t;
                                want[n] = h ? t : s;
                            }
                        } else {
                            if ((i1 ^ i2) < 0) {
                                h = i1 < 0, l = i1 + i2 < 0;
                                want[n] = l ? (uint16_t)~t : s;
                            } else {
                                l = i1 < 0, h = i2 - i1 >= 0;
                                want[n] = h ? t : s;
                            }
                        }
                        out_cc_lo |= l << n, out_cc_hi |= h << n;
                    }
                    g_checks++;
                    if (memcmp(rsp->vu.v[3], want, sizeof want) || memcmp(rsp->vu.acc_l, want, sizeof want) ||
                        rsp->vu.vcc_lo != out_cc_lo || rsp->vu.vcc_hi != out_cc_hi || rsp->vu.vco_lo != out_co_lo ||
                        rsp->vu.vco_hi != out_co_hi || rsp->vu.vce != out_ce) {
                        if (bad++ < 5)
                            fprintf(stderr, "%s:%d: clip op %02X e=%u vco=%04X vcc=%04X vce=%02X differs\n", __FILE__,
                                    __LINE__, ops[op], e, vcos[f], vccs[g], vces[g]);
                        g_failures++;
                    }
                }
}

static void test_vlogic_and_reserved(N64Rsp *rsp)
{
    static const uint16_t a[8] = {0x1111, 0x1245, 0x3333, 0x4444, 0xB0C5, 0x6666, 0x0000, 0xFFFF};
    static const uint16_t b[8] = {0xFF0F, 0xEF20, 0x0000, 0xFFFF, 0x3312, 0x0000, 0xEFEF, 0xEFEF};
    static const unsigned ops[6] = {VAND, VNAND, VOR, VNOR, VXOR, VNXOR};
    unsigned i, n;
    for (i = 0; i < 6; i++) {
        uint16_t want[8];
        vreset(rsp);
        memcpy(rsp->vu.v[1], a, sizeof a);
        memcpy(rsp->vu.v[2], b, sizeof b);
        rsp->vu.vco_lo = 0x12;
        run_vop(rsp, ops[i], 0);
        for (n = 0; n < 8; n++) {
            uint32_t r = i < 2 ? (uint32_t)a[n] & b[n] : i < 4 ? (uint32_t)a[n] | b[n] : (uint32_t)a[n] ^ b[n];
            want[n] = (uint16_t)((i & 1) ? ~r : r);
        }
        check_vec_at(__LINE__, "logic vd", rsp->vu.v[3], want);
        check_vec_at(__LINE__, "logic acc", rsp->vu.acc_l, want);
        CHECK_EQ(rsp->vu.vco_lo, 0x12);
    }
    /* VAND spot values worked by hand: 1111&FF0F = 1101, 1245&EF20 = 0200,
     * B0C5&3312 = 3000, and with element 9 (lane 1, EF20, broadcast). */
    vreset(rsp);
    memcpy(rsp->vu.v[1], a, sizeof a);
    memcpy(rsp->vu.v[2], b, sizeof b);
    run_vop(rsp, VAND, 0);
    CHECK_VEC(rsp->vu.v[3], 0x1101, 0x0200, 0x0000, 0x4444, 0x3000, 0x0000, 0x0000, 0xEFEF);
    run_vop(rsp, VAND, 9);
    CHECK_VEC(rsp->vu.v[3], 0x0100, 0x0200, 0x2320, 0x4400, 0xA000, 0x6620, 0x0000, 0xEF20);

    /* Reserved opcodes: acc low = vs + vt, vd = 0, flags untouched. */
    {
        static const unsigned reserved[] = {VSUT, VADDB, VSAD, 0x1E, 0x2E, VEXTT, 0x3B};
        for (i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
            vreset(rsp);
            set_vec(rsp->vu.v[1], 0, 1, 0x0010, 0xFFFF, 0x7FFF, 0x7FFF, 0x7FFF, 0xFFFF);
            set_vec(rsp->vu.v[2], 0, 2, 0x7FFF, 0x7FFF, 0x0000, 0xFFFF, 0xFFFE, 0xFFFF);
            rsp->vu.vco_lo = 0x33, rsp->vu.vcc_lo = 0x44, rsp->vu.vce = 0x55;
            run_vop(rsp, reserved[i], 0);
            CHECK_VEC(rsp->vu.v[3], 0, 0, 0, 0, 0, 0, 0, 0);
            CHECK_VEC(rsp->vu.acc_l, 0, 3, 0x800F, 0x7FFE, 0x7FFF, 0x7FFE, 0x7FFD, 0xFFFE);
            CHECK_EQ(rsp->vu.vco_lo, 0x33);
            CHECK_EQ(rsp->vu.vcc_lo, 0x44);
            CHECK_EQ(rsp->vu.vce, 0x55);
        }
    }
    /* VNOP and VNULL change nothing. */
    for (i = 0; i < 2; i++) {
        vreset(rsp);
        fill_vec(rsp->vu.acc_l, 0x7777);
        set_vec(rsp->vu.v[1], 1, 2, 3, 4, 5, 6, 7, 8);
        run_vop(rsp, i ? VNULL : VNOP, 0);
        CHECK_VEC(rsp->vu.v[3], 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD);
        CHECK_VEC(rsp->vu.acc_l, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777);
    }
    /* VSAR: element 8, 9, 10 read the high, middle and low slices; others
     * read zero; the accumulator stays. */
    {
        static const uint16_t slices[3] = {0x1111, 0x2222, 0x3333};
        unsigned e;
        for (e = 0; e < 16; e++) {
            vreset(rsp);
            fill_vec(rsp->vu.acc_h, slices[0]);
            fill_vec(rsp->vu.acc_m, slices[1]);
            fill_vec(rsp->vu.acc_l, slices[2]);
            run_vop(rsp, VSAR, e);
            CHECK_EQ(rsp->vu.v[3][5], (e >= 8 && e <= 10) ? slices[e - 8] : 0);
            CHECK_EQ(rsp->vu.acc_l[5], 0x3333);
        }
    }
}

static void test_vdivide(N64Rsp *rsp)
{
    /* VRCP: input -> 32-bit result (vd lane gets the low half, div_out the
     * high half). Worked from the ROM: rcp[0] = FFFF so 1 gives
     * (0x1FFFF << 14) >> 0 = 0x7FFFC000; 2 gives that >> 1; 0x4000 gives
     * >> 14 = 0x0001FFFF; -1 gives 0x7FFFC000 ^ ~0 = 0x80003FFF;
     * 3 normalises to index 256, rcp[256] = 0x5555, so
     * (0x15555 << 14) >> 1 = 0x2AAAA000; 0 and -32768 are special. */
    static const struct {
        uint16_t in;
        uint32_t out;
    } rcp[] = {
        {0x0001, 0x7FFFC000u}, {0x0002, 0x3FFFE000u}, {0x4000, 0x0001FFFFu}, {0xFFFF, 0x80003FFFu},
        {0x0003, 0x2AAAA000u}, {0x0000, 0x7FFFFFFFu}, {0x8000, 0xFFFF0000u},
    };
    /* VRSQ: 1 -> rsq[1] = FFFF, 0x7FFFC000; 4 -> shifted by 1; 2 ->
     * rsq[0] = 0x6A09: 0x16A09 << 14 = 0x5A824000 (1/sqrt 2 = 0.7071). */
    static const struct {
        uint16_t in;
        uint32_t out;
    } rsq[] = {
        {0x0001, 0x7FFFC000u}, {0x0004, 0x3FFFE000u}, {0x0002, 0x5A824000u},
        {0x0000, 0x7FFFFFFFu}, {0x8000, 0xFFFF0000u},
    };
    unsigned i;
    for (i = 0; i < sizeof rcp / sizeof rcp[0]; i++) {
        vreset(rsp);
        set_vec(rsp->vu.v[2], 0x1000, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, rcp[i].in, 0x1007);
        /* vd = v3, de = 2, vt = v2 element 14 (lane 6). */
        run1(rsp, vop(VRCP, 3, 2, 2, 14));
        CHECK_EQ(rsp->vu.v[3][2], rcp[i].out & 0xFFFF);
        CHECK_EQ((uint16_t)rsp->vu.div_out, rcp[i].out >> 16);
        CHECK_EQ(rsp->vu.v[3][0], 0xDEAD);
        {
            uint16_t want[8];
            unsigned n;
            for (n = 0; n < 8; n++) want[n] = rcp[i].in;
            check_vec_at(__LINE__, "VRCP acc", rsp->vu.acc_l, want);
        }
    }
    for (i = 0; i < sizeof rsq / sizeof rsq[0]; i++) {
        vreset(rsp);
        set_vec(rsp->vu.v[2], rsq[i].in, 0, 0, 0, 0, 0, 0, 0);
        run1(rsp, vop(VRSQ, 3, 7, 2, 8));
        CHECK_EQ(rsp->vu.v[3][7], rsq[i].out & 0xFFFF);
        CHECK_EQ((uint16_t)rsp->vu.div_out, rsq[i].out >> 16);
    }
    /* Double precision: VRCPH latches 0x0001 (and returns the previous
     * high half, 0x3FFF from VRCP of 2), VRCPL with 0x0000 divides
     * 0x00010000: normalised index 0, (0x7FFFC000 >> 16) = 0x00007FFF.
     * A second VRCPH returns that high half, 0. */
    {
        const uint32_t prog[] = {
            vop(VRCP, 4, 0, 2, 8),  /* v2[0] = 2 */
            vop(VRCPH, 5, 1, 2, 9), /* v2[1] = 1 -> v5[1] = 3FFF */
            vop(VRCPL, 6, 2, 2, 10), /* v2[2] = 0 -> v6[2] */
            vop(VRCPH, 7, 3, 2, 11),
        };
        vreset(rsp);
        set_vec(rsp->vu.v[2], 0x0002, 0x0001, 0x0000, 0x0000, 0, 0, 0, 0);
        run_words(rsp, prog, 4);
        CHECK_EQ(rsp->vu.v[4][0], 0xE000);
        CHECK_EQ(rsp->vu.v[5][1], 0x3FFF);
        CHECK_EQ(rsp->vu.v[6][2], 0x7FFF);
        CHECK_EQ(rsp->vu.v[7][3], 0x0000);
        CHECK_EQ(rsp->vu.div_dp, 1);
    }
    /* A 32-bit input below -32768 is negated by one's complement:
     * 0xFFFF0000 -> data 0xFFFF, index 0x1FF, rcp[511] = 0x0040,
     * (0x10040 << 14) >> 15 = 0x8020, ^ ~0 = 0xFFFF7FDF. VRCPL without a
     * VRCPH first takes the sign-extended lane. */
    {
        const uint32_t prog[] = {vop(VRCPH, 4, 0, 2, 8), vop(VRCPL, 4, 1, 2, 9), vop(VRCPL, 5, 0, 2, 10)};
        vreset(rsp);
        set_vec(rsp->vu.v[2], 0xFFFF, 0x0000, 0x0002, 0, 0, 0, 0, 0);
        run_words(rsp, prog, 3);
        CHECK_EQ(rsp->vu.v[4][1], 0x7FDF);
        CHECK_EQ(rsp->vu.v[5][0], 0xE000);
        CHECK_EQ((uint16_t)rsp->vu.div_out, 0x3FFF);
        CHECK_EQ(rsp->vu.div_dp, 0);
    }
    /* VRSQH/VRSQL: 0x0001:0x0000 = 65536, 1/sqrt = 1/256:
     * shift 15, rsq[1] = FFFF, 0x7FFFC000 >> 8 = 0x007FFFC0. */
    {
        const uint32_t prog[] = {vop(VRSQH, 4, 0, 2, 8), vop(VRSQL, 4, 1, 2, 9), vop(VRSQH, 4, 2, 2, 9)};
        vreset(rsp);
        set_vec(rsp->vu.v[2], 0x0001, 0x0000, 0, 0, 0, 0, 0, 0);
        run_words(rsp, prog, 3);
        CHECK_EQ(rsp->vu.v[4][1], 0xFFC0);
        CHECK_EQ(rsp->vu.v[4][2], 0x007F);
    }
    /* The ROM tables themselves at a few known entries. */
    {
        static const uint16_t first[5] = {0xFFFF, 0xFF00, 0xFE01, 0xFD04, 0xFC07};
        unsigned k;
        for (k = 0; k < 5; k++) {
            vreset(rsp);
            /* Input 0x200 + k normalises to index k (shift 22: (data <<
             * 22) & 0x7FC00000 >> 22 = k); 0x200 = 2^9 so the result is
             * (0x10000 | rcp[k]) << 14 >> 9. */
            set_vec(rsp->vu.v[2], (uint16_t)(0x200 + k), 0, 0, 0, 0, 0, 0, 0);
            run1(rsp, vop(VRCP, 3, 0, 2, 8));
            CHECK_EQ(((uint32_t)(uint16_t)rsp->vu.div_out << 16) | rsp->vu.v[3][0],
                     ((0x10000u | first[k]) << 14) >> 9);
        }
    }
    /* VMOV: lane de of vt after element selection; vd's other lanes stay. */
    vreset(rsp);
    set_vec(rsp->vu.v[2], 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17);
    run1(rsp, vop(VMOV, 3, 5, 2, 9));
    CHECK_VEC(rsp->vu.v[3], 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0x11, 0xDEAD, 0xDEAD);
    CHECK_VEC(rsp->vu.acc_l, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11);
    run1(rsp, vop(VMOV, 3, 3, 2, 0));
    CHECK_VEC(rsp->vu.v[3], 0xDEAD, 0xDEAD, 0xDEAD, 0x13, 0xDEAD, 0x11, 0xDEAD, 0xDEAD);
}

/* A short end-to-end program: DMA a vector in, scale it, DMA it out. */
static void test_program(N64Rsp *rsp, uint8_t *rdram)
{
    static const uint16_t in[8] = {0x0100, 0xFF00, 0x7FFF, 0x8000, 0x0001, 0x0000, 0x1234, 0xC000};
    unsigned n;
    const uint32_t prog[] = {
        ORI(1, 0, 0x6000), MTC0(1, 1), /* DRAM_ADDR */
        MTC0(0, 0),                    /* MEM_ADDR 0 */
        ORI(2, 0, 15), MTC0(2, 2),     /* read 16 bytes */
        lwc2(LQV, 1, 0, 0, 0),
        ORI(3, 0, 0x4000),             /* 0.5 */
        MTC2(3, 2, 0),
        vop(VMULF, 3, 1, 2, 8),        /* v3 = v1 * 0.5 */
        swc2(SQV, 3, 0, 0, 1),         /* DMEM 0x10 */
        ORI(4, 0, 0x10), MTC0(4, 0),
        ORI(5, 0, 0x7000), MTC0(5, 1),
        MTC0(2, 3),                    /* write 16 bytes */
    };
    n64_rsp_reset(rsp);
    for (n = 0; n < 8; n++) n64_put_be16(rdram + 0x6000 + 2 * n, in[n]);
    memset(rdram + 0x7000, 0, 16);
    run_words(rsp, prog, sizeof prog / sizeof prog[0]);
    /* VMULF by 0x4000 halves with rounding: x * 0x4000 * 2 + 0x8000 >> 16 =
     * (x + 1) >> 1 for these, sign included. */
    CHECK_EQ(n64_be16(rdram + 0x7000), 0x0080);
    CHECK_EQ(n64_be16(rdram + 0x7002), 0xFF80);
    CHECK_EQ(n64_be16(rdram + 0x7004), 0x4000);
    CHECK_EQ(n64_be16(rdram + 0x7006), 0xC000);
    CHECK_EQ(n64_be16(rdram + 0x7008), 0x0001);
    CHECK_EQ(n64_be16(rdram + 0x700A), 0x0000);
    CHECK_EQ(n64_be16(rdram + 0x700C), 0x091A);
    CHECK_EQ(n64_be16(rdram + 0x700E), 0xE000);
}

/* Two RSPs in one process share nothing. */
static void test_independent_instances(N64Rsp *a, N64Rsp *b)
{
    const uint32_t prog[] = {ADDIU(1, 0, 11), BREAK};
    n64_rsp_reset(a);
    n64_rsp_reset(b);
    put_words(a, 0, prog, 2);
    start_at(a, 0);
    CHECK_EQ(a->r[1], 11);
    CHECK_EQ(b->r[1], 0);
    CHECK_EQ(b->cycles, 0);
    CHECK_EQ(n64_rsp_halted(b), true);
}

int main(void)
{
    N64Rsp *rsp = (N64Rsp *)calloc(1, sizeof(N64Rsp));
    N64Rsp *other = (N64Rsp *)calloc(1, sizeof(N64Rsp));
    uint8_t *rdram = (uint8_t *)calloc(1, 0x10010);
    Recorder rec;
    if (!rsp || !other || !rdram) return 2;
    memset(&rec, 0, sizeof rec);
    rsp->links.rdram = rdram;
    rsp->links.rdram_size = 0x10000;
    rsp->links.user = &rec;
    rsp->links.interrupt = rec_interrupt;
    rsp->links.dp_read = rec_dp_read;
    rsp->links.dp_write = rec_dp_write;

    test_scalar_alu(rsp);
    test_branches(rsp);
    test_scalar_memory(rsp);
    test_status_and_semaphore(rsp, &rec);
    test_break_and_cop0(rsp, &rec);
    test_dma(rsp, rdram);
    test_vector_moves(rsp);
    test_vector_loads(rsp);
    test_vector_ltv(rsp);
    test_vector_stores(rsp);
    test_element_select(rsp);
    test_vmulf_vmulu(rsp);
    test_vmacf_vmacu(rsp);
    test_vmud(rsp);
    test_vmad(rsp);
    test_vrnd_vmulq_vmacq(rsp);
    test_vadd_vsub(rsp);
    test_vcompare(rsp);
    test_vclip(rsp);
    test_vclip_reference(rsp);
    test_vlogic_and_reserved(rsp);
    test_vdivide(rsp);
    test_program(rsp, rdram);
    test_independent_instances(rsp, other);

    free(rdram);
    free(other);
    free(rsp);
    if (g_failures) {
        fprintf(stderr, "n64_rsp: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("n64_rsp: %d checks passed\n", g_checks);
    return 0;
}
