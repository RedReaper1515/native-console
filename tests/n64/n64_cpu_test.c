/*
 * VR4300 behaviour the oracle vectors (n64_vectors.c) cannot pin, because
 * the R4000 model they come from differs or does not model it: the FPU's
 * unimplemented-operation exception, the 64-bit integer conversions, FR=0
 * register views, division by zero, MULT and SRA on values that are not
 * sign-extended, LWR's upper half, the TLB, exceptions in delay slots,
 * Random and the Count/Compare timer. Expected values are from the VR4300
 * user's manual and ares's hardware-tested behaviour (docs/N64-PLAN.md).
 *
 * It also holds the RDRAM fast path to the bus path: every load and store
 * gives the same result with RDRAM reached directly and through the bus.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "n64_cpu_impl.h"

#define MEM_SIZE 0x100000u
static uint8_t g_mem[MEM_SIZE];
static int g_failures, g_checks;

#define CHECK(cond, ...) \
    do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static uint64_t rd(uint32_t pa, uint32_t size)
{
    uint64_t v = 0;
    for (uint32_t k = 0; k < size; k++) v = (v << 8) | (pa + k < MEM_SIZE ? g_mem[pa + k] : 0);
    return v;
}
static void wr(uint32_t pa, uint32_t size, uint64_t v)
{
    for (uint32_t k = 0; k < size; k++)
        if (pa + k < MEM_SIZE) g_mem[pa + k] = (uint8_t)(v >> (8u * (size - 1u - k)));
}
uint32_t n64_bus_fetch(N64Cpu *c, uint32_t pa) { (void)c; return (uint32_t)rd(pa, 4); }
uint8_t n64_bus_read8(N64Cpu *c, uint32_t pa) { (void)c; return (uint8_t)rd(pa, 1); }
uint16_t n64_bus_read16(N64Cpu *c, uint32_t pa) { (void)c; return (uint16_t)rd(pa, 2); }
uint32_t n64_bus_read32(N64Cpu *c, uint32_t pa) { (void)c; return (uint32_t)rd(pa, 4); }
uint64_t n64_bus_read64(N64Cpu *c, uint32_t pa) { (void)c; return rd(pa, 8); }
void n64_bus_write8(N64Cpu *c, uint32_t pa, uint8_t v) { (void)c; wr(pa, 1, v); }
void n64_bus_write16(N64Cpu *c, uint32_t pa, uint16_t v) { (void)c; wr(pa, 2, v); }
void n64_bus_write32(N64Cpu *c, uint32_t pa, uint32_t v) { (void)c; wr(pa, 4, v); }
void n64_bus_write64(N64Cpu *c, uint32_t pa, uint64_t v) { (void)c; wr(pa, 8, v); }
void n64_bus_code_written(N64Cpu *c, uint32_t pa, uint32_t size) { (void)c; (void)pa; (void)size; }
void n64_bus_timer_changed(N64Cpu *c) { (void)c; }
void n64_bus_interrupt_check(N64Cpu *c) { (void)c; }

#define CODE 0xFFFFFFFF80010000ull

static void setup(N64Cpu *c, uint32_t status, bool fast)
{
    memset(c, 0, sizeof *c);
    n64_cpu_reset(c);
    c->cp0_status = status;
    n64_cpu_update_mode(c);
    c->links.rdram = fast ? g_mem : NULL;
    c->links.rdram_size = fast ? MEM_SIZE : 0;
    c->pc = CODE;
}

/* Run `n` instructions placed at CODE. */
static void run(N64Cpu *c, const uint32_t *insns, int n, int steps)
{
    for (int i = 0; i < n; i++) n64_put_be32(g_mem + 0x10000u + 4u * (uint32_t)i, insns[i]);
    for (int i = 0; i < steps; i++) (void)n64_cpu_step(c);
}

static uint32_t exc_code(const N64Cpu *c) { return (c->cp0_cause >> 2) & 31u; }

#define COP1(fmt, ft, fs, fd, fn) (0x46000000u | ((fmt) << 21) | ((ft) << 16) | ((fs) << 11) | ((fd) << 6) | (fn))
#define SR_FPU (N64_SR_CU1 | N64_SR_FR)

static void test_fpu_unimplemented(void)
{
    N64Cpu c;
    /* A denormal operand: unimplemented, nothing written, FPE taken. */
    setup(&c, SR_FPU, true);
    c.fpr[1] = 0x00000001u; /* denormal single */
    c.fpr[2] = 0x3F800000u;
    c.fpr[3] = 0x12345678u;
    uint32_t add_s = COP1(16u, 2u, 1u, 3u, 0x00u);
    run(&c, &add_s, 1, 1);
    CHECK(exc_code(&c) == N64_EXC_FPE, "denormal add.s: exception %u", exc_code(&c));
    CHECK(c.fcr31 & 0x00020000u, "denormal add.s: cause E not set (%08X)", c.fcr31);
    CHECK(c.fpr[3] == 0x12345678u, "denormal add.s wrote its result");

    /* A quiet NaN (MIPS legacy: top fraction bit clear) is unimplemented too. */
    setup(&c, SR_FPU, true);
    c.fpr[1] = 0x7F800001u;
    c.fpr[2] = 0x3F800000u;
    run(&c, &add_s, 1, 1);
    CHECK(exc_code(&c) == N64_EXC_FPE && (c.fcr31 & 0x00020000u), "qNaN add.s: not unimplemented");

    /* A result too small for a single, flush mode off: unimplemented. */
    setup(&c, SR_FPU, true);
    c.fpr[1] = 0x00800000u; /* smallest normal */
    c.fpr[2] = 0x3F000000u; /* 0.5 */
    uint32_t mul_s = COP1(16u, 2u, 1u, 3u, 0x02u);
    run(&c, &mul_s, 1, 1);
    CHECK(exc_code(&c) == N64_EXC_FPE && (c.fcr31 & 0x00020000u), "denormal result without FS");
    /* ... with FS on and no exception enabled: flushed to zero, U and I flagged. */
    setup(&c, SR_FPU, true);
    c.fcr31 = N64_FCR31_FS;
    c.fpr[1] = 0x00800000u;
    c.fpr[2] = 0x3F000000u;
    run(&c, &mul_s, 1, 1);
    CHECK(!(c.cp0_status & N64_SR_EXL), "flushed result raised an exception");
    CHECK(c.fpr[3] == 0, "flushed result %016" PRIX64, c.fpr[3]);
    CHECK((c.fcr31 & 0x0Cu) == 0x0Cu, "flushed result flags %08X", c.fcr31);

    /* A conversion out of range of a word: unimplemented, not "invalid". */
    setup(&c, SR_FPU, true);
    c.fpr[1] = 0x4F800000u; /* 2^32 */
    c.fpr[3] = 0xAAAAAAAAu;
    uint32_t cvt_w_s = COP1(16u, 0u, 1u, 3u, 0x24u);
    run(&c, &cvt_w_s, 1, 1);
    CHECK(exc_code(&c) == N64_EXC_FPE && (c.fcr31 & 0x00020000u), "cvt.w.s of 2^32");
    CHECK(c.fpr[3] == 0xAAAAAAAAu, "cvt.w.s of 2^32 wrote its result");
}

static void test_fpu_long(void)
{
    N64Cpu c;
    /* cvt.l.d, round.l.s, trunc.l.d and the conversions back. */
    setup(&c, SR_FPU, true);
    double d = -12345678901.75;
    memcpy(&c.fpr[1], &d, 8);
    uint32_t prog[] = {
        COP1(17u, 0u, 1u, 2u, 0x25u), /* cvt.l.d f2, f1 (round to nearest) */
        COP1(17u, 0u, 1u, 3u, 0x09u), /* trunc.l.d f3, f1 */
        COP1(17u, 0u, 1u, 4u, 0x0Au), /* ceil.l.d f4, f1 */
        COP1(17u, 0u, 1u, 5u, 0x0Bu), /* floor.l.d f5, f1 */
        COP1(21u, 0u, 2u, 6u, 0x21u), /* cvt.d.l f6, f2 */
    };
    run(&c, prog, 5, 5);
    CHECK((int64_t)c.fpr[2] == -12345678902ll, "cvt.l.d %" PRId64, (int64_t)c.fpr[2]);
    CHECK((int64_t)c.fpr[3] == -12345678901ll, "trunc.l.d %" PRId64, (int64_t)c.fpr[3]);
    CHECK((int64_t)c.fpr[4] == -12345678901ll, "ceil.l.d %" PRId64, (int64_t)c.fpr[4]);
    CHECK((int64_t)c.fpr[5] == -12345678902ll, "floor.l.d %" PRId64, (int64_t)c.fpr[5]);
    double back;
    memcpy(&back, &c.fpr[6], 8);
    CHECK(back == -12345678902.0, "cvt.d.l %f", back);
    CHECK(c.fcr31 & 0x04u, "inexact flag not set by the inexact conversions (%08X)", c.fcr31);

    /* Round to nearest, ties to even. */
    setup(&c, SR_FPU, true);
    float half = 2.5f;
    uint32_t hb;
    memcpy(&hb, &half, 4);
    c.fpr[1] = hb;
    uint32_t round_l_s = COP1(16u, 0u, 1u, 2u, 0x08u);
    run(&c, &round_l_s, 1, 1);
    CHECK(c.fpr[2] == 2u, "round.l.s 2.5 -> %" PRIu64, c.fpr[2]);

    /* cvt.s.l beyond 2^55: unimplemented. */
    setup(&c, SR_FPU, true);
    c.fpr[1] = 0x0080000000000000ull;
    uint32_t cvt_s_l = COP1(21u, 0u, 1u, 2u, 0x20u);
    run(&c, &cvt_s_l, 1, 1);
    CHECK(exc_code(&c) == N64_EXC_FPE && (c.fcr31 & 0x00020000u), "cvt.s.l of 2^55");
}

static void test_fpu_fr0(void)
{
    N64Cpu c;
    /* With FR clear, MTC1 to an odd register writes the upper half of the
     * even one, and MFC1 reads it back from there. */
    setup(&c, N64_SR_CU1, true);
    c.gpr[8] = 0x11111111u;
    c.gpr[9] = 0x22222222u;
    uint32_t prog[] = {
        0x44882000u, /* mtc1 t0, f4 */
        0x44892800u, /* mtc1 t1, f5 */
        0x440A2800u, /* mfc1 t2, f5 */
        0x442B2000u, /* dmfc1 t3, f4 */
    };
    run(&c, prog, 4, 4);
    CHECK(c.fpr[4] == 0x2222222211111111ull, "FR=0 pair %016" PRIX64, c.fpr[4]);
    CHECK(c.fpr[5] == 0, "FR=0 odd register itself written");
    CHECK(c.gpr[10] == 0x22222222u, "FR=0 mfc1 odd %016" PRIX64, c.gpr[10]);
    CHECK(c.gpr[11] == 0x2222222211111111ull, "FR=0 dmfc1 %016" PRIX64, c.gpr[11]);
}

static void test_integer_quirks(void)
{
    N64Cpu c;
    /* DIV by zero: LO = -1 for a non-negative dividend, 1 for a negative
     * one; HI = the dividend. DIVU: LO = all ones. */
    setup(&c, 0, true);
    c.gpr[8] = 7;
    c.gpr[9] = 0;
    uint32_t div = 0x0109001Au, divu = 0x0109001Bu;
    run(&c, &div, 1, 1);
    CHECK(c.lo == UINT64_MAX && c.hi == 7, "div 7/0: lo %016" PRIX64 " hi %016" PRIX64, c.lo, c.hi);
    setup(&c, 0, true);
    c.gpr[8] = (uint64_t)-7;
    c.gpr[9] = 0;
    run(&c, &div, 1, 1);
    CHECK(c.lo == 1 && c.hi == (uint64_t)-7, "div -7/0");
    setup(&c, 0, true);
    c.gpr[8] = 0xFFFFFFFF80000000ull;
    c.gpr[9] = 0;
    run(&c, &divu, 1, 1);
    CHECK(c.lo == UINT64_MAX && c.hi == 0xFFFFFFFF80000000ull, "divu by 0");
    /* SRA shifts the whole register before truncating (ares). */
    setup(&c, 0, true);
    c.gpr[8] = 0x00000001FFFFFFF0ull;
    uint32_t sra = 0x00084903u; /* sra t1, t0, 4 */
    run(&c, &sra, 1, 1);
    CHECK(c.gpr[9] == 0x000000001FFFFFFFull, "sra of a wide value %016" PRIX64, c.gpr[9]);
    /* MULT uses rt's low 35 bits, sign-extended, against all of rs. */
    setup(&c, 0, true);
    c.gpr[8] = 0x0000000100000002ull;
    c.gpr[9] = 3;
    uint32_t mult = 0x01090018u;
    run(&c, &mult, 1, 1);
    CHECK(c.lo == 6 && c.hi == 3, "mult of a wide rs: lo %016" PRIX64 " hi %016" PRIX64, c.lo, c.hi);
    /* LWR not loading the top byte keeps rt's upper half. */
    setup(&c, 0, true);
    wr(0x20000, 4, 0x11223344u);
    c.gpr[8] = 0xFFFFFFFF80020000ull;
    c.gpr[9] = 0xAAAAAAAABBBBBBBBull;
    uint32_t lwr = 0x99090001u; /* lwr t1, 1(t0) */
    run(&c, &lwr, 1, 1);
    CHECK(c.gpr[9] == 0xAAAAAAAABBBB1122ull, "lwr upper half %016" PRIX64, c.gpr[9]);
}

static void test_delay_slot_exception(void)
{
    N64Cpu c;
    /* A misaligned load in a taken branch's delay slot: EPC is the branch,
     * Cause.BD is set, BadVAddr is the address. */
    setup(&c, 0, true);
    c.gpr[8] = 0xFFFFFFFF80020001ull;
    uint32_t prog[] = { 0x10000004u, 0x8D090000u }; /* beq zero, zero, +4; lw t1, 0(t0) */
    run(&c, prog, 2, 2);
    CHECK(exc_code(&c) == N64_EXC_ADEL, "delay-slot exception code %u", exc_code(&c));
    CHECK(c.cp0_epc == CODE, "EPC %016" PRIX64, c.cp0_epc);
    CHECK(c.cp0_cause & N64_CAUSE_BD, "BD not set");
    CHECK(c.cp0_bad_vaddr == 0xFFFFFFFF80020001ull, "BadVAddr %016" PRIX64, c.cp0_bad_vaddr);
    CHECK(c.pc == 0xFFFFFFFF80000180ull, "vector %016" PRIX64, c.pc);
}

static void test_tlb(void)
{
    N64Cpu c;
    setup(&c, 0, true);
    /* Map virtual 0x00400000 (even page) and 0x00401000 (odd page, valid
     * but not dirty) to physical 0x20000 and 0x21000, 4 KiB pages, ASID 5. */
    c.cp0_page_mask = 0;
    c.cp0_entry_hi = 0x00400000u | 5u;
    c.cp0_entry_lo[0] = ((0x20000u >> 12) << 6) | 0x6u; /* D, V */
    c.cp0_entry_lo[1] = ((0x21000u >> 12) << 6) | 0x2u; /* V only */
    c.cp0_index = 3;
    n64_cpu_tlbwi(&c);
    wr(0x20010, 4, 0xCAFEF00Du);
    uint32_t pa = 0;
    CHECK(n64_cpu_translate(&c, 0x00400010u, false, &pa) && pa == 0x20010u, "TLB translate %08X", pa);
    c.gpr[8] = 0x00400010u;
    uint32_t lw = 0x8D090000u;
    run(&c, &lw, 1, 1);
    CHECK(c.gpr[9] == 0xFFFFFFFFCAFEF00Dull, "lw through the TLB %016" PRIX64, c.gpr[9]);
    /* A store to the odd page: TLB modification. */
    c.pc = CODE;
    c.gpr[8] = 0x00401000u;
    uint32_t sw = 0xAD090000u;
    run(&c, &sw, 1, 1);
    CHECK(exc_code(&c) == N64_EXC_MOD, "store to a clean page: code %u", exc_code(&c));
    /* An unmapped address: refill vector 0x80000000 when EXL was clear. */
    setup(&c, 0, true);
    c.gpr[8] = 0x00800000u;
    run(&c, &lw, 1, 1);
    CHECK(exc_code(&c) == N64_EXC_TLBL && c.pc == 0xFFFFFFFF80000000ull, "TLB miss: code %u pc %016" PRIX64,
          exc_code(&c), c.pc);
    CHECK((c.cp0_entry_hi & ~0xFFull) == 0x00800000u, "EntryHi after miss %016" PRIX64, c.cp0_entry_hi);
    /* TLBP finds the entry; another ASID does not. */
    setup(&c, 0, true);
    c.cp0_entry_hi = 0x00400000u | 5u;
    c.cp0_entry_lo[0] = 0x6u;
    c.cp0_entry_lo[1] = 0x6u;
    c.cp0_index = 7;
    n64_cpu_tlbwi(&c);
    c.cp0_entry_hi = 0x00400000u | 5u;
    n64_cpu_tlbp(&c);
    CHECK(c.cp0_index == 7, "TLBP index %08X", c.cp0_index);
    c.cp0_entry_hi = 0x00400000u | 6u;
    n64_cpu_tlbp(&c);
    CHECK(c.cp0_index & 0x80000000u, "TLBP matched another ASID");
}

static void test_timer_and_random(void)
{
    N64Cpu c;
    setup(&c, 0, true);
    n64_cpu_cop0_write(&c, 9, 100);  /* Count */
    n64_cpu_cop0_write(&c, 11, 150); /* Compare */
    uint64_t due = n64_cpu_timer_due(&c);
    CHECK(due == c.cycles + 100u, "timer due in %" PRIu64 " cycles, want 100", due - c.cycles);
    c.cycles = due;
    CHECK(n64_cpu_count(&c) == 150u, "Count at the due cycle %u", n64_cpu_count(&c));
    /* Random stays within [Wired, 31] and moves with each instruction. */
    n64_cpu_cop0_write(&c, 6, 10);
    uint32_t seen_min = 99, seen_max = 0;
    for (int i = 0; i < 100; i++) {
        uint32_t r = (uint32_t)n64_cpu_cop0_read(&c, 1);
        if (r < seen_min) seen_min = r;
        if (r > seen_max) seen_max = r;
        c.instructions++;
    }
    CHECK(seen_min == 10 && seen_max == 31, "Random range %u-%u", seen_min, seen_max);
}

/* Every load and store gives the same answer through the RDRAM shortcut
 * and through the bus. */
static void test_fast_path(void)
{
    static const uint32_t ops[] = {
        0x81090003u, 0x91090003u, 0x85090002u, 0x95090002u, 0x8D090004u, 0x9D090004u, 0xDD090008u,
        0x89090001u, 0x99090002u, 0x69090003u, 0x6D090005u, /* lb lbu lh lhu lw lwu ld lwl lwr ldl ldr */
        0xA1090003u, 0xA5090002u, 0xAD090004u, 0xFD090008u, 0xA9090001u, 0xB9090002u, 0xB1090003u, 0xB5090005u,
    };
    for (size_t k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        uint8_t snap[2][32];
        uint64_t reg[2];
        for (int fast = 0; fast < 2; fast++) {
            N64Cpu c;
            for (int i = 0; i < 32; i++) g_mem[0x20000 + i] = (uint8_t)(0x91 + 7 * i);
            setup(&c, 0, fast != 0);
            c.gpr[8] = 0xFFFFFFFF80020000ull;
            c.gpr[9] = 0x0123456789ABCDEFull;
            run(&c, &ops[k], 1, 1);
            memcpy(snap[fast], g_mem + 0x20000, 32);
            reg[fast] = c.gpr[9];
        }
        CHECK(reg[0] == reg[1] && !memcmp(snap[0], snap[1], 32), "fast path differs for %08X", ops[k]);
    }
}

int main(void)
{
    test_fpu_unimplemented();
    test_fpu_long();
    test_fpu_fr0();
    test_integer_quirks();
    test_delay_slot_exception();
    test_tlb();
    test_timer_and_random();
    test_fast_path();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
