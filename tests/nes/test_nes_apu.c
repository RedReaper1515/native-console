/*
 * NES APU unit tests: length counters, frame counter and IRQ timing,
 * envelopes, sweep, the channels' sequencers, the DMC, the mixer tables,
 * the output stage, chunked runs and the event horizon.
 *
 *   test_nes_apu [--bench]
 *
 * Every test is named and each check is counted; any failed check fails
 * the run (exit status 1). The APU is driven through its register and run
 * interface; internal state is read (never written) to check results.
 *
 * Times follow nes_apu.h: an access is made at `cycle` == n and the step
 * that follows it advances `cycle` to n + 1, so "after step s" means
 * `cycle` == s + 1. The expectations are those of blargg's apu_test and
 * blargg_apu_2005.07.30 readmes and of the nesdev wiki, restated in that
 * convention.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nes_apu.h"

static int g_checks, g_failed_checks, g_test_failed;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        g_checks++;                                               \
        if (!(cond)) {                                            \
            g_failed_checks++;                                    \
            g_test_failed = 1;                                    \
            printf("    FAIL line %d: ", __LINE__);               \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        }                                                         \
    } while (0)

#define PI 3.14159265358979323846
#define CPU_HZ (39375000.0 / 22.0)
#define CYCLES_PER_SECOND 1789773u

/* ---- The DMC's bus ------------------------------------------------------ */

typedef struct {
    uint64_t cycle;
    uint16_t addr;
} Fetch;

#define MAX_FETCHES 32768u

/* Tests with two instances give each its own log through `user`. */
typedef struct {
    Fetch fetch[MAX_FETCHES];
    unsigned count;
} FetchLog;

static uint8_t g_mem[0x10000];
static FetchLog g_log;

uint8_t nes_apu_dmc_read(NesApu *a, uint16_t addr)
{
    FetchLog *log = a->user ? (FetchLog *)a->user : &g_log;
    if (log->count < MAX_FETCHES) {
        log->fetch[log->count].cycle = a->cycle;
        log->fetch[log->count].addr = addr;
    }
    log->count++;
    return g_mem[addr];
}

/* ---- Helpers ------------------------------------------------------------ */

/* Garbage first: power must initialise every byte. */
static NesApu *new_apu_over(uint8_t garbage)
{
    NesApu *a = (NesApu *)malloc(sizeof *a);
    if (!a) {
        printf("out of memory\n");
        exit(2);
    }
    memset(a, garbage, sizeof *a);
    nes_apu_power(a);
    g_log.count = 0;
    return a;
}

static NesApu *new_apu(void) { return new_apu_over(0xA7); }

static void run(NesApu *a, uint32_t n) { nes_apu_run(a, n); }
static void wr(NesApu *a, uint16_t addr, uint8_t v) { nes_apu_write(a, addr, v); }
static uint8_t peek(const NesApu *a) { return nes_apu_peek(a, 0x4015); }

static void run_to(NesApu *a, uint64_t cycle)
{
    CHECK(a->cycle <= cycle, "run_to(%llu) from %llu", (unsigned long long)cycle, (unsigned long long)a->cycle);
    if (a->cycle < cycle) run(a, (uint32_t)(cycle - a->cycle));
}

/* Run until `cycle` has the wanted parity (1 = get access, 0 = put). */
static void align(NesApu *a, unsigned parity)
{
    while ((a->cycle & 1u) != parity) run(a, 1);
}

/* One quarter + half frame clock: a $4017 write with bit 7 set clocks the
 * units when the sequence restarts, 2 or 3 steps later. Leaves the frame
 * counter in 5-step mode at the start of its sequence. */
static void clock_frame(NesApu *a)
{
    wr(a, 0x4017, 0x80);
    run(a, 4);
}

static void clock_frames(NesApu *a, int n)
{
    while (n-- > 0) clock_frame(a);
}

/* A pulse channel's level while its duty output is high, without letting
 * time pass: restart the sequencer of a copy (duty 3 is high at step 0). */
static unsigned pulse_high_level(const NesApu *a, unsigned ch)
{
    NesApu *copy = (NesApu *)malloc(sizeof *copy);
    if (!copy) exit(2);
    *copy = *a;
    wr(copy, (uint16_t)(0x4000u + 4u * ch), (uint8_t)(0xC0u | (a->pulse[ch].env.loop << 5) | (a->pulse[ch].env.constant << 4) | a->pulse[ch].env.volume));
    wr(copy, (uint16_t)(0x4003u + 4u * ch), (uint8_t)(a->pulse[ch].period >> 8));
    /* The $4003 write may be a length reload; keep the counter as it was. */
    const unsigned level = a->len[ch].counter ? (copy->len[ch].counter ? nes_apu_channel_level(copy, ch) : 0) : 0;
    free(copy);
    return level;
}

static uint32_t g_rng = 0x2A03C0DEu;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* ---- Power -------------------------------------------------------------- */

static void test_power_state(void)
{
    NesApu *a = new_apu_over(0x00);
    NesApu *b = new_apu_over(0xFF);
    CHECK(memcmp(a, b, sizeof *a) == 0, "power leaves bytes that depend on the previous contents");
    CHECK(a->cycle == 0 && a->user == NULL, "cycle/user after power");
    CHECK(a->rate == NES_APU_DEFAULT_RATE, "rate %u", a->rate);
    CHECK(peek(a) == 0, "status %02X", peek(a));
    CHECK(!nes_apu_irq(a), "irq at power");
    CHECK(a->noise.lfsr == 1, "lfsr %04X", a->noise.lfsr);
    CHECK(a->dmc.level == 0 && a->dmc.sample_addr == 0xC000 && a->dmc.sample_len == 1, "dmc registers");
    CHECK(nes_apu_channel_level(a, NES_APU_TRIANGLE) == 15, "triangle starts at the first step of its waveform");
    CHECK(nes_apu_read(a, 0x4000) == 0 && nes_apu_read(a, 0x4017) == 0 && nes_apu_read(a, 0x4014) == 0, "write-only registers read 0");

    /* $4017 = $00 at power: the sequence restarts in the first step and
     * the frame IRQ arrives 29828 steps later. */
    CHECK(nes_apu_cycles_until_event(a) >= 1 && nes_apu_cycles_until_event(a) <= 29829, "horizon at power %u", nes_apu_cycles_until_event(a));
    run_to(a, 29828);
    CHECK(!(peek(a) & 0x40) && !nes_apu_irq(a), "frame IRQ early");
    run(a, 1);
    CHECK((peek(a) & 0x40) && nes_apu_irq(a), "frame IRQ missing after power");

    /* Nothing is enabled: length loads are refused. */
    wr(a, 0x4003, 0x08);
    wr(a, 0x400B, 0x08);
    wr(a, 0x400F, 0x08);
    run(a, 2);
    CHECK((peek(a) & 0x1F) == 0, "length counters loaded while disabled: %02X", peek(a));
    free(a);
    free(b);
}

/* ---- Length counters ---------------------------------------------------- */

static const uint8_t wiki_length_table[32] = {
    10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
    12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

static void test_length_table_and_halt(void)
{
    static const uint16_t load_reg[4] = { 0x4003, 0x4007, 0x400B, 0x400F };
    static const uint16_t halt_reg[4] = { 0x4000, 0x4004, 0x4008, 0x400C };
    static const uint8_t halt_bit[4] = { 0x20, 0x20, 0x80, 0x20 };
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4015, 0x0F);
    for (unsigned ch = 0; ch < 4; ch++) {
        for (unsigned i = 0; i < 32; i++) {
            wr(a, load_reg[ch], (uint8_t)(i << 3));
            CHECK(a->len[ch].counter == wiki_length_table[i], "channel %u index %u: %u", ch, i, a->len[ch].counter);
            CHECK(peek(a) & (1u << ch), "status bit %u", ch);
        }
    }

    for (unsigned ch = 0; ch < 4; ch++) {
        wr(a, halt_reg[ch], 0x00);
        wr(a, load_reg[ch], 0x00); /* 10 */
        clock_frames(a, 3);
        CHECK(a->len[ch].counter == 7, "channel %u after 3 clocks: %u", ch, a->len[ch].counter);
        wr(a, halt_reg[ch], halt_bit[ch]);
        clock_frames(a, 3);
        CHECK(a->len[ch].counter == 7, "channel %u halted: %u", ch, a->len[ch].counter);
        wr(a, halt_reg[ch], 0x00);
        clock_frames(a, 6);
        CHECK(a->len[ch].counter == 1 && (peek(a) & (1u << ch)), "channel %u before the end: %u", ch, a->len[ch].counter);
        clock_frame(a);
        CHECK(a->len[ch].counter == 0 && !(peek(a) & (1u << ch)), "channel %u at the end: %u", ch, a->len[ch].counter);
        clock_frames(a, 3);
        CHECK(a->len[ch].counter == 0, "channel %u wrapped: %u", ch, a->len[ch].counter);
    }

    /* Disabling zeroes at once and refuses loads; the others are untouched. */
    for (unsigned ch = 0; ch < 4; ch++) wr(a, load_reg[ch], 0x08);
    CHECK((peek(a) & 0x0F) == 0x0F, "all loaded");
    wr(a, 0x4015, 0x0A);
    CHECK((peek(a) & 0x0F) == 0x0A, "disable: %02X", peek(a));
    wr(a, 0x4003, 0x08);
    wr(a, 0x400B, 0x08);
    run(a, 2);
    CHECK((peek(a) & 0x0F) == 0x0A, "load while disabled: %02X", peek(a));
    CHECK(a->len[1].counter == 254 && a->len[3].counter == 254, "enabled channels keep counting");
    free(a);
}

/* `*value` keeps `before` up to step `step` and is `after` once it ran. */
static void expect_change(NesApu *a, uint64_t step, const uint8_t *value, unsigned before, unsigned after, const char *what)
{
    run_to(a, step);
    CHECK(*value == before, "%s: %u before step %llu, expected %u", what, *value, (unsigned long long)step, before);
    run(a, 1);
    CHECK(*value == after, "%s: %u after step %llu, expected %u", what, *value, (unsigned long long)step, after);
}

/* Pulse 1's length counter counts half frames and the triangle's linear
 * counter counts quarter frames. Returns the cycle of the $4017 write. */
static uint64_t frame_setup(NesApu *a, unsigned parity, uint8_t mode)
{
    run(a, 20);
    align(a, parity);
    wr(a, 0x4015, 0x05);
    wr(a, 0x4000, 0x00);
    wr(a, 0x4003, 0x08); /* 254 */
    wr(a, 0x4008, 0x7F); /* control clear, reload value 127 */
    wr(a, 0x400B, 0x08); /* reload flag */
    const uint64_t at = a->cycle;
    wr(a, 0x4017, mode);
    return at;
}

static void test_frame_4step_positions(void)
{
    for (unsigned parity = 0; parity < 2; parity++) {
        NesApu *a = new_apu();
        const uint64_t A = frame_setup(a, parity, 0x00);
        /* A write on a put cycle (even) restarts the sequence in step A + 2,
         * on a get cycle in step A + 3 (the phase is pinned by blargg's
         * cpu_interrupts_v2/4-irq_and_dma, see nes_apu.h): blargg's 7459 / 7460 cycles from
         * the write to the first clock. */
        const uint64_t R = A + (parity ? 3u : 2u);
        run_to(a, R);
        CHECK(a->fc_pos != 0, "parity %u: restarted early", parity);
        CHECK(a->tri.linear == 0 && a->len[0].counter == 254, "parity %u: mode 0 clocked on the write", parity);
        run(a, 1);
        CHECK(a->fc_pos == 0, "parity %u: no restart in step %llu", parity, (unsigned long long)R);
        CHECK(a->tri.linear == 0 && a->len[0].counter == 254, "parity %u: mode 0 clocked at the restart", parity);

        expect_change(a, R + 7457, &a->tri.linear, 0, 127, "quarter 1");
        CHECK(a->len[0].counter == 254, "half clock at quarter 1");
        expect_change(a, R + 14913, &a->tri.linear, 127, 126, "quarter 2");
        CHECK(a->len[0].counter == 253, "half 1: %u", a->len[0].counter);
        expect_change(a, R + 22371, &a->tri.linear, 126, 125, "quarter 3");
        CHECK(a->len[0].counter == 253, "half clock at quarter 3");
        run_to(a, R + 29829);
        CHECK(a->len[0].counter == 253, "half 2 early");
        expect_change(a, R + 29829, &a->tri.linear, 125, 124, "quarter 4");
        CHECK(a->len[0].counter == 252, "half 2: %u", a->len[0].counter);
        /* The next sequence starts 29830 steps after the first. */
        expect_change(a, R + 29830 + 7457, &a->tri.linear, 124, 123, "quarter 5");
        expect_change(a, R + 29830 + 14913, &a->len[0].counter, 252, 251, "half 3");
        CHECK(a->tri.linear == 122, "quarter 6: %u", a->tri.linear);
        free(a);
    }
}

static void test_frame_5step_positions(void)
{
    for (unsigned parity = 0; parity < 2; parity++) {
        NesApu *a = new_apu();
        const uint64_t A = frame_setup(a, parity, 0x80);
        const uint64_t R = A + (parity ? 3u : 2u);
        /* Bit 7 set: quarter and half clock when the sequence restarts. */
        run_to(a, R);
        CHECK(a->tri.linear == 0 && a->len[0].counter == 254, "parity %u: clocked before the restart step", parity);
        run(a, 1);
        CHECK(a->tri.linear == 127 && a->len[0].counter == 253, "parity %u: immediate clock: linear %u length %u", parity, a->tri.linear,
              a->len[0].counter);

        expect_change(a, R + 7457, &a->tri.linear, 127, 126, "quarter 1");
        CHECK(a->len[0].counter == 253, "half clock at quarter 1");
        expect_change(a, R + 14913, &a->len[0].counter, 253, 252, "half 1");
        CHECK(a->tri.linear == 125, "quarter 2");
        expect_change(a, R + 22371, &a->tri.linear, 125, 124, "quarter 3");
        /* Nothing where the 4-step sequence ends. */
        run_to(a, R + 29832);
        CHECK(a->tri.linear == 124 && a->len[0].counter == 252, "5-step mode clocked at the 4-step positions");
        expect_change(a, R + 37281, &a->len[0].counter, 252, 251, "half 2");
        CHECK(a->tri.linear == 123, "quarter 4");
        run(a, 1);
        CHECK(a->tri.linear == 123 && a->len[0].counter == 251, "clock at the wrap");
        expect_change(a, R + 37282 + 7457, &a->tri.linear, 123, 122, "quarter 5");
        expect_change(a, R + 37282 + 14913, &a->len[0].counter, 251, 250, "half 3");
        CHECK(!(peek(a) & 0x40) && !nes_apu_irq(a), "frame IRQ in 5-step mode");
        free(a);
    }
}

/* INC $4017 and friends write on two successive cycles: each write starts
 * its own delay. */
static void test_frame_write_twice(void)
{
    NesApu *a = new_apu();
    frame_setup(a, 0, 0x00);
    run(a, 50);
    align(a, 0);
    const uint64_t A = a->cycle;
    const unsigned before = a->len[0].counter;
    wr(a, 0x4017, 0x80);
    run(a, 1);
    wr(a, 0x4017, 0x80);
    run_to(a, A + 3);
    CHECK(a->len[0].counter == before - 1u, "first write's clock in step A+2: %u", a->len[0].counter);
    run_to(a, A + 5);
    CHECK(a->len[0].counter == before - 2u, "second write's clock in step A+4: %u", a->len[0].counter);
    run(a, 20);
    CHECK(a->len[0].counter == before - 2u, "extra clocks: %u", a->len[0].counter);
    free(a);
}

/* ---- Frame IRQ ---------------------------------------------------------- */

static void test_frame_irq_flag(void)
{
    NesApu *a = new_apu();
    run(a, 21);
    align(a, 0);
    uint64_t A = a->cycle;
    wr(a, 0x4017, 0x40); /* clear the power-on sequence's flag for good */
    wr(a, 0x4017, 0x00);
    uint64_t X = A + 2 + 29828; /* the first of the three steps that set the flag */

    run(a, 4);
    CHECK(nes_apu_cycles_until_event(a) == X + 1 - a->cycle, "event horizon %u, expected %llu", nes_apu_cycles_until_event(a),
          (unsigned long long)(X + 1 - a->cycle));
    run_to(a, X);
    CHECK(!(peek(a) & 0x40) && !nes_apu_irq(a), "flag set too soon");
    CHECK(nes_apu_cycles_until_event(a) == 1, "event horizon at the edge: %u", nes_apu_cycles_until_event(a));
    run(a, 1);
    CHECK((peek(a) & 0x40) && nes_apu_irq(a), "flag set too late");
    run(a, 9); /* cycle == X + 10, a put access */
    CHECK((peek(a) & 0x40) && nes_apu_irq(a), "flag does not stay set");
    CHECK(nes_apu_cycles_until_event(a) == NES_APU_EVENT_MAX, "a set flag with nothing pending: %u", nes_apu_cycles_until_event(a));

    /* A read returns the flag and clears it in the first put step from the
     * read on: the step of a put access, the one after a get access. */
    CHECK(nes_apu_read(a, 0x4015) & 0x40, "read does not see the flag");
    CHECK(nes_apu_cycles_until_event(a) == 1, "horizon after a read on put: %u", nes_apu_cycles_until_event(a));
    run(a, 1);
    CHECK(!(peek(a) & 0x40) && !nes_apu_irq(a), "read on a put cycle did not clear");
    CHECK(!(nes_apu_read(a, 0x4015) & 0x40), "second read sees the flag");

    X += 29830;
    CHECK(nes_apu_cycles_until_event(a) == X + 1 - a->cycle, "horizon to the next frame");
    run_to(a, X);
    CHECK(!(peek(a) & 0x40), "second frame: too soon");
    run(a, 1);
    CHECK(peek(a) & 0x40, "second frame: too late (29830 cycles per sequence)");
    run_to(a, X + 9); /* a get access */
    CHECK(nes_apu_read(a, 0x4015) & 0x40, "read");
    CHECK(nes_apu_cycles_until_event(a) == 2, "horizon after a read on get: %u", nes_apu_cycles_until_event(a));
    run(a, 1);
    CHECK((peek(a) & 0x40) && nes_apu_irq(a), "read on a get cycle cleared in the get step");
    run(a, 1);
    CHECK(!(peek(a) & 0x40) && !nes_apu_irq(a), "read on a get cycle did not clear in the next put step");

    /* The flag is set in three successive steps: a read between them
     * returns it set and does not get rid of it. */
    X += 29830;
    run_to(a, X + 1);
    CHECK(nes_apu_read(a, 0x4015) & 0x40, "first read in the window");
    run(a, 1);
    CHECK(peek(a) & 0x40, "flag lost after the second step");
    CHECK(nes_apu_read(a, 0x4015) & 0x40, "second read in the window");
    run(a, 1);
    CHECK((peek(a) & 0x40) && nes_apu_irq(a), "flag cleared although the third step set it");
    CHECK(nes_apu_read(a, 0x4015) & 0x40, "read after the window");
    run(a, 2);
    CHECK(!(peek(a) & 0x40), "flag survives a read after the window");

    /* A read just before the first step does not suppress it. */
    X += 29830;
    run_to(a, X);
    CHECK(!(nes_apu_read(a, 0x4015) & 0x40), "flag before the first step");
    run(a, 1);
    CHECK(peek(a) & 0x40, "read in the cycle of the first step suppressed the flag");

    /* $4017 writes: only the inhibit bit clears the flag. */
    wr(a, 0x4017, 0x00);
    CHECK(peek(a) & 0x40, "$4017 = $00 cleared the flag");
    wr(a, 0x4017, 0x80);
    CHECK(peek(a) & 0x40, "$4017 = $80 cleared the flag");
    wr(a, 0x4017, 0x40);
    CHECK(!(peek(a) & 0x40) && !nes_apu_irq(a), "$4017 = $40 did not clear the flag");
    free(a);

    /* Inhibited: the line never moves. The flag itself is raised on the
     * first two steps and dropped on the third (as ares does it). */
    a = new_apu();
    run(a, 21);
    align(a, 0);
    A = a->cycle;
    wr(a, 0x4017, 0x40);
    X = A + 2 + 29828;
    bool line = false;
    while (a->cycle < X) {
        run(a, 1);
        line |= nes_apu_irq(a);
    }
    CHECK(!(peek(a) & 0x40), "inhibited flag set early");
    run(a, 1);
    line |= nes_apu_irq(a);
    CHECK(peek(a) & 0x40, "inhibited flag, first step");
    run(a, 1);
    line |= nes_apu_irq(a);
    CHECK(peek(a) & 0x40, "inhibited flag, second step");
    run(a, 1);
    line |= nes_apu_irq(a);
    CHECK(!(peek(a) & 0x40), "inhibited flag left set");
    for (int i = 0; i < 40000; i++) {
        run(a, 1);
        line |= nes_apu_irq(a);
    }
    CHECK(!line, "IRQ line moved while inhibited");
    CHECK(nes_apu_cycles_until_event(a) == NES_APU_EVENT_MAX, "inhibited horizon %u", nes_apu_cycles_until_event(a));
    free(a);

    /* 5-step mode: never. */
    a = new_apu();
    run(a, 20);
    wr(a, 0x4017, 0x40);
    wr(a, 0x4017, 0x80);
    bool flag = false;
    for (int i = 0; i < 2 * 37282 + 100; i++) {
        run(a, 1);
        flag |= (peek(a) & 0x40) != 0;
    }
    CHECK(!flag, "flag set in 5-step mode");
    free(a);
}

/* ---- Length counter writes in the clocking cycle ------------------------ */

/* Pulse 1 enabled; returns the step of the first half frame clock. */
static NesApu *half_setup(uint8_t r4000, int load, uint64_t *half_step)
{
    NesApu *a = new_apu();
    run(a, 21);
    align(a, 0);
    wr(a, 0x4015, 0x01);
    wr(a, 0x4000, r4000);
    if (load) wr(a, 0x4003, 0x00); /* 10 */
    *half_step = a->cycle + 2 + 14913;
    wr(a, 0x4017, 0x00);
    return a;
}

static void test_length_same_cycle_rules(void)
{
    uint64_t H;
    NesApu *a;

    /* blargg 10.len_halt_timing: a halt change takes effect after the
     * clock of its own cycle. */
    a = half_setup(0x00, 1, &H);
    run_to(a, H - 1);
    wr(a, 0x4000, 0x20);
    run(a, 2);
    CHECK(a->len[0].counter == 10, "halted one cycle before the clock, still clocked: %u", a->len[0].counter);
    free(a);

    a = half_setup(0x00, 1, &H);
    run_to(a, H);
    wr(a, 0x4000, 0x20);
    run(a, 1);
    CHECK(a->len[0].counter == 9, "halted in the clocking cycle, not clocked: %u", a->len[0].counter);
    CHECK(a->len[0].halt == 1, "halt not applied after the clock");
    free(a);

    a = half_setup(0x20, 1, &H);
    run_to(a, H - 1);
    wr(a, 0x4000, 0x00);
    run(a, 2);
    CHECK(a->len[0].counter == 9, "unhalted one cycle before the clock, not clocked: %u", a->len[0].counter);
    free(a);

    a = half_setup(0x20, 1, &H);
    run_to(a, H);
    wr(a, 0x4000, 0x00);
    run(a, 1);
    CHECK(a->len[0].counter == 10, "unhalted in the clocking cycle, clocked: %u", a->len[0].counter);
    CHECK(a->len[0].halt == 0, "unhalt not applied after the clock");
    free(a);

    /* blargg 11.len_reload_timing: a reload in the clocking cycle is
     * ignored when the counter is not zero. */
    a = half_setup(0x00, 1, &H);
    run_to(a, H - 1);
    wr(a, 0x4003, 0x08);
    run(a, 2);
    CHECK(a->len[0].counter == 253, "reload just before the clock: %u", a->len[0].counter);
    free(a);

    a = half_setup(0x00, 1, &H);
    run_to(a, H + 1);
    wr(a, 0x4003, 0x08);
    run(a, 1);
    CHECK(a->len[0].counter == 254, "reload just after the clock: %u", a->len[0].counter);
    free(a);

    a = half_setup(0x00, 0, &H);
    run_to(a, H);
    wr(a, 0x4003, 0x08);
    CHECK(a->len[0].counter == 0, "reload applied before the clock");
    run(a, 1);
    CHECK(a->len[0].counter == 254, "reload during the clock with the counter at 0: %u", a->len[0].counter);
    free(a);

    a = half_setup(0x00, 1, &H);
    run_to(a, H);
    wr(a, 0x4003, 0x08);
    run(a, 1);
    CHECK(a->len[0].counter == 9, "reload during the clock with the counter above 0: %u", a->len[0].counter);
    run(a, 3);
    CHECK(a->len[0].counter == 9, "dropped reload applied later: %u", a->len[0].counter);
    free(a);

    /* A halted counter is not decremented, so nothing blocks the reload
     * (our reading of the hardware; the test ROMs do not cover it). */
    a = half_setup(0x20, 1, &H);
    run_to(a, H);
    wr(a, 0x4003, 0x08);
    run(a, 1);
    CHECK(a->len[0].counter == 254, "reload during the clock of a halted counter: %u", a->len[0].counter);
    free(a);

    /* The same rule at the clock a $4017 write with bit 7 set makes. */
    a = half_setup(0x00, 1, &H);
    run(a, 100);
    align(a, 0);
    const uint64_t A = a->cycle;
    wr(a, 0x4017, 0x80);
    run_to(a, A + 2);
    wr(a, 0x4003, 0x08);
    run(a, 1);
    CHECK(a->len[0].counter == 9, "reload during the $4017 clock: %u", a->len[0].counter);
    free(a);
}

/* ---- Envelope ----------------------------------------------------------- */

static void test_envelope(void)
{
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4015, 0x09);
    wr(a, 0x4002, 0xFF);
    wr(a, 0x4000, 0xC3); /* decay, no loop, divider period 3 */
    wr(a, 0x4003, 0x09); /* start flag, length 254, period $1FF */
    CHECK(a->pulse[0].env.start == 1, "start flag");
    clock_frame(a);
    CHECK(a->pulse[0].env.decay == 15 && a->pulse[0].env.start == 0, "first clock loads 15: %u", a->pulse[0].env.decay);
    CHECK(pulse_high_level(a, 0) == 15, "level %u", pulse_high_level(a, 0));
    for (int level = 14; level >= 0; level--) {
        clock_frames(a, 3);
        CHECK(a->pulse[0].env.decay == level + 1, "decay moved early at %d: %u", level, a->pulse[0].env.decay);
        clock_frame(a);
        CHECK(a->pulse[0].env.decay == level, "decay %u, expected %d (one step per 4 clocks)", a->pulse[0].env.decay, level);
    }
    CHECK(pulse_high_level(a, 0) == 0, "level at the end %u", pulse_high_level(a, 0));
    clock_frames(a, 12);
    CHECK(a->pulse[0].env.decay == 0, "decay left 0 without the loop flag: %u", a->pulse[0].env.decay);

    /* Loop: 0 is followed by 15. */
    wr(a, 0x4000, 0xE3);
    wr(a, 0x4003, 0x09);
    clock_frames(a, 1 + 15 * 4);
    CHECK(a->pulse[0].env.decay == 0, "looping decay at 0: %u", a->pulse[0].env.decay);
    clock_frames(a, 4);
    CHECK(a->pulse[0].env.decay == 15, "loop reload: %u", a->pulse[0].env.decay);
    clock_frames(a, 4);
    CHECK(a->pulse[0].env.decay == 14, "after the loop: %u", a->pulse[0].env.decay);

    /* Constant volume: the parameter is the level; the decay keeps going. */
    wr(a, 0x4000, 0xFA);
    CHECK(pulse_high_level(a, 0) == 10, "constant volume %u", pulse_high_level(a, 0));
    clock_frames(a, 11);
    CHECK(a->pulse[0].env.decay == 13, "decay under constant volume: %u", a->pulse[0].env.decay);
    wr(a, 0x4000, 0xEA);
    CHECK(pulse_high_level(a, 0) == 13, "back to the decay level: %u", pulse_high_level(a, 0));

    /* A period of 0 steps every clock. */
    wr(a, 0x4000, 0xC0);
    wr(a, 0x4003, 0x09);
    clock_frames(a, 6);
    CHECK(a->pulse[0].env.decay == 10, "period 0: %u", a->pulse[0].env.decay);

    /* The noise channel has the same unit. */
    wr(a, 0x400C, 0x01);
    wr(a, 0x400F, 0x08);
    clock_frame(a);
    CHECK(a->noise.env.decay == 15, "noise start: %u", a->noise.env.decay);
    clock_frames(a, 6);
    CHECK(a->noise.env.decay == 12, "noise decay: %u", a->noise.env.decay);
    wr(a, 0x400C, 0x17);
    CHECK(a->noise.env.constant == 1 && a->noise.env.volume == 7, "noise constant volume");
    free(a);
}

/* ---- Sweep -------------------------------------------------------------- */

static void pulse_setup(NesApu *a, unsigned ch, unsigned period, uint8_t sweep)
{
    const uint16_t base = (uint16_t)(0x4000u + 4u * ch);
    wr(a, base, 0xFF); /* duty 3, halt, constant volume 15 */
    wr(a, (uint16_t)(base + 1u), sweep);
    wr(a, (uint16_t)(base + 2u), (uint8_t)(period & 0xFFu));
    wr(a, (uint16_t)(base + 3u), (uint8_t)(0x08u | (period >> 8)));
}

static void test_sweep(void)
{
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4015, 0x03);

    /* Negate: pulse 1 subtracts one more than pulse 2. */
    pulse_setup(a, 0, 0x100, 0x89); /* enabled, period 0, negate, shift 1 */
    pulse_setup(a, 1, 0x100, 0x89);
    clock_frame(a);
    CHECK(a->pulse[0].period == 0x07F, "pulse 1 ones' complement: %03X", a->pulse[0].period);
    CHECK(a->pulse[1].period == 0x080, "pulse 2 two's complement: %03X", a->pulse[1].period);
    clock_frame(a);
    CHECK(a->pulse[0].period == 0x03F && a->pulse[1].period == 0x040, "second step: %03X %03X", a->pulse[0].period, a->pulse[1].period);
    CHECK(pulse_high_level(a, 0) == 15 && pulse_high_level(a, 1) == 15, "sounding");

    /* Down to below 8: muted, and the period stops moving. */
    clock_frames(a, 2);
    CHECK(a->pulse[0].period == 0x00F && a->pulse[1].period == 0x010, "fourth step: %03X %03X", a->pulse[0].period, a->pulse[1].period);
    clock_frame(a);
    CHECK(a->pulse[0].period == 0x007 && a->pulse[1].period == 0x008, "fifth step: %03X %03X", a->pulse[0].period, a->pulse[1].period);
    CHECK(pulse_high_level(a, 0) == 0, "period 7 is muted");
    CHECK(pulse_high_level(a, 1) == 15, "period 8 is not muted");
    clock_frames(a, 3);
    CHECK(a->pulse[0].period == 0x007, "a muted channel's period changed: %03X", a->pulse[0].period);
    CHECK(a->pulse[1].period == 0x004, "pulse 2 sweeps once more from 8 and stops: %03X", a->pulse[1].period);
    CHECK(pulse_high_level(a, 1) == 0, "pulse 2 below 8 is muted");

    /* Upward: stops, muted, when the target passes $7FF. */
    pulse_setup(a, 0, 0x100, 0x81);
    static const unsigned up[] = { 0x180, 0x240, 0x360, 0x510, 0x798 };
    for (unsigned i = 0; i < 5; i++) {
        clock_frame(a);
        CHECK(a->pulse[0].period == up[i], "up step %u: %03X", i, a->pulse[0].period);
    }
    CHECK(pulse_high_level(a, 0) == 0, "target above $7FF does not mute");
    clock_frames(a, 3);
    CHECK(a->pulse[0].period == 0x798, "period moved past the limit: %03X", a->pulse[0].period);

    /* The adder mutes even with the sweep disabled; negate never does. */
    pulse_setup(a, 0, 0x400, 0x00);
    CHECK(pulse_high_level(a, 0) == 0, "$400 + $400 does not mute");
    pulse_setup(a, 0, 0x3FF, 0x00);
    CHECK(pulse_high_level(a, 0) == 15, "$3FF + $3FF mutes");
    pulse_setup(a, 0, 0x400, 0x08);
    CHECK(pulse_high_level(a, 0) == 15, "negate mutes at $400");
    pulse_setup(a, 0, 0x7FF, 0x07);
    CHECK(pulse_high_level(a, 0) == 0, "$7FF + $F does not mute");
    pulse_setup(a, 0, 0x7F0, 0x07);
    CHECK(pulse_high_level(a, 0) == 15, "$7F0 + $F mutes");

    /* Shift 0 or disabled: muting still applies but the period stays. */
    pulse_setup(a, 0, 0x200, 0x88);
    clock_frames(a, 4);
    CHECK(a->pulse[0].period == 0x200, "shift 0 changed the period: %03X", a->pulse[0].period);
    pulse_setup(a, 0, 0x200, 0x09);
    clock_frames(a, 4);
    CHECK(a->pulse[0].period == 0x200, "disabled sweep changed the period: %03X", a->pulse[0].period);

    /* Divider: with period P the update comes every P + 1 clocks, and a
     * write to the sweep register restarts the divider at the next clock
     * without an update (unless the divider was at 0 anyway). */
    while (a->pulse[1].sweep_divider) clock_frame(a);
    pulse_setup(a, 1, 0x400, 0xA9); /* enabled, period 2, negate, shift 1 */
    clock_frame(a);
    CHECK(a->pulse[1].period == 0x200, "update with the divider at 0: %03X", a->pulse[1].period);
    clock_frames(a, 2);
    CHECK(a->pulse[1].period == 0x200, "update before the divider ran out: %03X", a->pulse[1].period);
    clock_frame(a);
    CHECK(a->pulse[1].period == 0x100, "update after 3 clocks: %03X", a->pulse[1].period);
    clock_frame(a);
    wr(a, 0x4005, 0xA9); /* reload flag */
    clock_frame(a);
    CHECK(a->pulse[1].period == 0x100 && a->pulse[1].sweep_divider == 2, "reload: period %03X divider %u", a->pulse[1].period,
          a->pulse[1].sweep_divider);
    clock_frames(a, 2);
    CHECK(a->pulse[1].period == 0x100, "update too soon after a reload: %03X", a->pulse[1].period);
    clock_frame(a);
    CHECK(a->pulse[1].period == 0x080, "update 3 clocks after the reload: %03X", a->pulse[1].period);
    free(a);
}

/* ---- Pulse sequencer ---------------------------------------------------- */

static void test_pulse_sequencer(void)
{
    /* The wiki's output waveforms, starting at sequencer step 0. */
    static const char *const wave[4] = { "01000000", "01100000", "01111000", "10011111" };
    const unsigned period = 20;
    const unsigned step_len = 2 * (period + 1);
    for (unsigned ch = 0; ch < 2; ch++) {
        for (unsigned duty = 0; duty < 4; duty++) {
            NesApu *a = new_apu();
            run(a, 10 + ch);
            wr(a, 0x4015, 0x03);
            const uint16_t base = (uint16_t)(0x4000u + 4u * ch);
            wr(a, base, (uint8_t)((duty << 6) | 0x3Au)); /* halt, constant volume 10 */
            wr(a, (uint16_t)(base + 2u), (uint8_t)period);
            run(a, 3 * step_len); /* the timer picks the period up */
            const unsigned timer = a->pulse[ch].timer;
            wr(a, (uint16_t)(base + 3u), 0x08);
            CHECK(a->pulse[ch].step == 0, "$4003 does not restart the sequencer");
            CHECK(a->pulse[ch].timer == timer, "$4003 touched the timer");

            /* Wait for the first step, then sample the middle of each. */
            char seen[9] = { 0 };
            seen[0] = nes_apu_channel_level(a, ch) ? '1' : '0';
            unsigned guard = 0;
            while (a->pulse[ch].step == 0 && guard++ < step_len) run(a, 1);
            CHECK(guard <= step_len, "sequencer does not advance");
            const uint64_t t0 = a->cycle;
            for (unsigned k = 1; k < 8; k++) {
                run_to(a, t0 + (k - 1) * step_len + step_len / 2);
                const unsigned level = nes_apu_channel_level(a, ch);
                CHECK(level == 0 || level == 10, "level %u", level);
                seen[k] = level ? '1' : '0';
            }
            CHECK(strcmp(seen, wave[duty]) == 0, "pulse %u duty %u: %s, expected %s", ch + 1, duty, seen, wave[duty]);

            /* Exactly 16 (t + 1) CPU cycles per period. */
            run_to(a, t0 + 8 * step_len - 1);
            CHECK(a->pulse[ch].step == 0, "step %u one cycle before the period ends", a->pulse[ch].step);
            run(a, 1);
            CHECK(a->pulse[ch].step == 7, "step %u after 16 (t + 1) cycles", a->pulse[ch].step);
            unsigned high = 0;
            for (unsigned i = 0; i < 8 * step_len; i++) {
                high += nes_apu_channel_level(a, ch) != 0;
                run(a, 1);
            }
            static const unsigned eighths[4] = { 1, 2, 4, 6 };
            CHECK(high == eighths[duty] * step_len, "duty %u: %u high cycles of %u", duty, high, 8 * step_len);

            /* Silent with the length counter at 0. */
            wr(a, 0x4015, 0x00);
            unsigned any = 0;
            for (unsigned i = 0; i < 8 * step_len; i++) {
                any |= nes_apu_channel_level(a, ch);
                run(a, 1);
            }
            CHECK(any == 0, "output with the length counter at 0");
            free(a);
        }
    }
}

/* ---- Triangle ----------------------------------------------------------- */

static void test_triangle(void)
{
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4015, 0x04);
    wr(a, 0x4008, 0x85); /* control set, reload value 5 */
    wr(a, 0x400A, 20);
    wr(a, 0x400B, 0x08); /* length 254, reload flag */
    CHECK(a->tri.reload_flag == 1 && a->tri.linear == 0, "reload flag");
    run(a, 200);
    CHECK(nes_apu_channel_level(a, NES_APU_TRIANGLE) == 15 && a->tri.step == 0, "sequencer ran with the linear counter at 0");

    clock_frame(a);
    CHECK(a->tri.linear == 5 && a->tri.reload_flag == 1, "reload with control set: linear %u flag %u", a->tri.linear, a->tri.reload_flag);
    clock_frames(a, 3);
    CHECK(a->tri.linear == 5, "control set keeps reloading: %u", a->tri.linear);

    /* Control clear: the next clock still reloads, then clears the flag. */
    wr(a, 0x4008, 0x05);
    clock_frame(a);
    CHECK(a->tri.linear == 5 && a->tri.reload_flag == 0, "last reload: linear %u flag %u", a->tri.linear, a->tri.reload_flag);

    /* The waveform: 32 steps of 21 cycles, 15..0 then 0..15. */
    unsigned seq[96] = { 0 };
    unsigned n = 0;
    unsigned last_step = a->tri.step;
    uint64_t last_change = 0;
    for (unsigned i = 0; i < 75 * 21 && n < 96; i++) {
        run(a, 1);
        if (a->tri.step != last_step) {
            if (last_change) CHECK(a->cycle - last_change == 21, "step length %u", (unsigned)(a->cycle - last_change));
            last_change = a->cycle;
            last_step = a->tri.step;
            seq[n++] = nes_apu_channel_level(a, NES_APU_TRIANGLE);
        }
    }
    CHECK(n >= 70, "only %u steps", n);
    /* The waveform starts at the second of the two 15s. */
    unsigned start = 1;
    while (start < n && !(seq[start] == 15 && seq[start - 1] == 15)) start++;
    CHECK(start + 32 <= n, "waveform has no repeated 15 (%u of %u)", start, n);
    for (unsigned i = 0; i < 32 && start + i < n; i++) {
        const unsigned expected = i < 16 ? 15 - i : i - 16;
        CHECK(seq[start + i] == expected, "waveform step %u: %u, expected %u", i, seq[start + i], expected);
    }

    /* The linear counter counts down and the sequencer stops where it is. */
    for (int expect = 4; expect >= 0; expect--) {
        clock_frame(a);
        CHECK(a->tri.linear == expect, "linear %u, expected %d", a->tri.linear, expect);
    }
    clock_frame(a);
    CHECK(a->tri.linear == 0, "linear counter wrapped: %u", a->tri.linear);
    const unsigned frozen = a->tri.step;
    const unsigned level = nes_apu_channel_level(a, NES_APU_TRIANGLE);
    CHECK(frozen != 0 && level != 0 && level != 15, "the test should stop the sequencer mid-wave (step %u)", frozen);
    run(a, 5000);
    CHECK(a->tri.step == frozen && nes_apu_channel_level(a, NES_APU_TRIANGLE) == level, "sequencer moved or reset with the linear counter at 0");

    /* It resumes from the same step. */
    wr(a, 0x4008, 0x7F);
    wr(a, 0x400B, 0x08);
    clock_frame(a);
    CHECK(a->tri.linear == 0x7F, "reload %u", a->tri.linear);
    unsigned guard = 0;
    while (a->tri.step == frozen && guard++ < 30) run(a, 1);
    CHECK(a->tri.step == ((frozen + 1u) & 31u), "resumed at step %u after %u", a->tri.step, frozen);

    /* The length counter stops it the same way. */
    wr(a, 0x4015, 0x00);
    const unsigned frozen2 = a->tri.step;
    run(a, 5000);
    CHECK(a->tri.step == frozen2, "sequencer moved with the length counter at 0");
    CHECK(nes_apu_channel_level(a, NES_APU_TRIANGLE) == (frozen2 < 16 ? 15 - frozen2 : frozen2 - 16), "frozen level");
    free(a);
}

/* ---- Noise -------------------------------------------------------------- */

static unsigned lfsr_next(unsigned r, int mode)
{
    const unsigned fb = (r ^ (r >> (mode ? 6 : 1))) & 1u;
    return (r >> 1) | (fb << 14);
}

static void test_noise(void)
{
    static const uint16_t wiki_period[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4015, 0x08);
    wr(a, 0x400C, 0x3C); /* halt, constant volume 12 */
    wr(a, 0x400F, 0x08);

    /* The timer period for every rate. */
    for (unsigned rate = 0; rate < 16; rate++) {
        wr(a, 0x400E, (uint8_t)rate);
        unsigned last = a->noise.lfsr;
        uint64_t changed[4] = { 0 };
        unsigned n = 0;
        for (unsigned i = 0; i < 5 * 4068 + 16 && n < 4; i++) {
            run(a, 1);
            if (a->noise.lfsr != last) {
                last = a->noise.lfsr;
                changed[n++] = a->cycle;
            }
        }
        CHECK(n == 4, "rate %u: no clocks", rate);
        /* The first interval may still be the previous rate's. */
        if (n == 4) {
            CHECK(changed[2] - changed[1] == wiki_period[rate] && changed[3] - changed[2] == wiki_period[rate], "rate %u: period %u, expected %u",
                  rate, (unsigned)(changed[3] - changed[2]), wiki_period[rate]);
        }
    }
    free(a);

    /* The long sequence from the power-on value, 32767 steps. */
    a = new_apu();
    wr(a, 0x4015, 0x08);
    wr(a, 0x400C, 0x3C);
    wr(a, 0x400F, 0x08);
    wr(a, 0x400E, 0x00);
    unsigned ref = 1;
    unsigned mismatches = 0, level_errors = 0, period = 0;
    CHECK(a->noise.lfsr == 1, "lfsr before the first clock: %04X", a->noise.lfsr);
    for (unsigned k = 1; k <= 32767 + 10; k++) {
        const unsigned before = a->noise.lfsr;
        unsigned guard = 0;
        while (a->noise.lfsr == before && guard++ < 16) run(a, 1);
        ref = lfsr_next(ref, 0);
        mismatches += a->noise.lfsr != ref;
        /* Bit 0 set silences the channel. */
        level_errors += nes_apu_channel_level(a, NES_APU_NOISE) != ((ref & 1u) ? 0u : 12u);
        if (!period && ref == 1) period = k;
    }
    CHECK(mismatches == 0, "%u steps differ from the reference shift register", mismatches);
    CHECK(level_errors == 0, "%u steps with the wrong output", level_errors);
    CHECK(period == 32767, "long sequence length %u", period);
    CHECK(lfsr_next(1, 0) == 0x4000 && lfsr_next(0x4000, 0) == 0x2000, "reference");

    /* Short mode: feedback from bit 6, 93 or 31 steps. */
    wr(a, 0x400E, 0x80);
    const unsigned first = a->noise.lfsr;
    ref = first;
    mismatches = 0;
    period = 0;
    for (unsigned k = 1; k <= 200; k++) {
        const unsigned before = a->noise.lfsr;
        unsigned guard = 0;
        while (a->noise.lfsr == before && guard++ < 16) run(a, 1);
        ref = lfsr_next(ref, 1);
        mismatches += a->noise.lfsr != ref;
        if (!period && ref == first) period = k;
    }
    CHECK(mismatches == 0, "short mode: %u steps differ", mismatches);
    CHECK(period == 93 || period == 31, "short sequence length %u", period);

    /* Silent with the length counter at 0. */
    wr(a, 0x4015, 0x00);
    unsigned any = 0;
    for (unsigned i = 0; i < 400; i++) {
        any |= nes_apu_channel_level(a, NES_APU_NOISE);
        run(a, 1);
    }
    CHECK(any == 0, "noise output with the length counter at 0");
    free(a);
}

/* ---- DMC ---------------------------------------------------------------- */

static void test_dmc_rates(void)
{
    static const uint16_t wiki_rate[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
    memset(g_mem, 0x55, sizeof g_mem);
    for (unsigned rate = 0; rate < 16; rate++) {
        NesApu *a = new_apu();
        run(a, 10);
        wr(a, 0x4017, 0x40);
        wr(a, 0x4010, (uint8_t)(0x40u | rate)); /* loop */
        wr(a, 0x4012, 0x00);
        wr(a, 0x4013, 0x00); /* one byte */
        wr(a, 0x4015, 0x10);
        run(a, 8u * 428u * 7u);
        CHECK(g_log.count >= 6 && g_log.count < MAX_FETCHES, "rate %u: %u fetches", rate, g_log.count);
        /* The first fetch is the load; the output unit's first cycle still
         * has the power-on timer value in it. */
        for (unsigned i = 3; i < g_log.count && i < 6; i++) {
            const uint64_t gap = g_log.fetch[i].cycle - g_log.fetch[i - 1].cycle;
            CHECK(gap == 8u * wiki_rate[rate], "rate %u: %u cycles per byte, expected %u", rate, (unsigned)gap, 8u * wiki_rate[rate]);
            CHECK(g_log.fetch[i].cycle & 1u, "rate %u: reload fetch in an even step", rate);
            CHECK(g_log.fetch[i].addr == 0xC000, "looping one byte: address %04X", g_log.fetch[i].addr);
        }
        CHECK(!nes_apu_irq(a) && (peek(a) & 0x10), "looping sample: status %02X", peek(a));
        free(a);
    }
}

/* Play `bytes` bytes of `value` from level `start`; reports the extremes. */
static void dmc_play(uint8_t value, unsigned len_reg, uint8_t start, unsigned *final, unsigned *lo, unsigned *hi, unsigned *fetches)
{
    memset(g_mem, value, sizeof g_mem);
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4010, 0x0F);
    wr(a, 0x4011, start);
    wr(a, 0x4012, 0x00);
    wr(a, 0x4013, (uint8_t)len_reg);
    wr(a, 0x4015, 0x10);
    const unsigned bytes = len_reg * 16u + 1u;
    *lo = *hi = start;
    for (unsigned i = 0; i < 1000u + (bytes + 3u) * 8u * 54u; i++) {
        run(a, 1);
        if (a->dmc.level < *lo) *lo = a->dmc.level;
        if (a->dmc.level > *hi) *hi = a->dmc.level;
    }
    *final = a->dmc.level;
    *fetches = g_log.count;
    CHECK(!(peek(a) & 0x10), "sample still playing");
    CHECK(a->dmc.silence == 1, "output unit not silent after the sample");
    CHECK(nes_apu_channel_level(a, NES_APU_DMC) == a->dmc.level, "channel level");
    free(a);
}

static void test_dmc_playback(void)
{
    unsigned final, lo, hi, fetches;

    /* Bits are played from bit 0: $0F rises 4 steps of 2, then falls. */
    dmc_play(0x0F, 0, 64, &final, &lo, &hi, &fetches);
    CHECK(fetches == 1, "%u fetches for a one-byte sample", fetches);
    CHECK(hi == 72 && lo == 64 && final == 64, "$0F from 64: low %u high %u final %u", lo, hi, final);
    dmc_play(0xF0, 0, 64, &final, &lo, &hi, &fetches);
    CHECK(hi == 64 && lo == 56 && final == 64, "$F0 from 64: low %u high %u final %u", lo, hi, final);
    dmc_play(0x55, 0, 64, &final, &lo, &hi, &fetches);
    CHECK(hi == 66 && lo == 64 && final == 64, "$55 from 64: low %u high %u final %u", lo, hi, final);
    dmc_play(0xFF, 0, 1, &final, &lo, &hi, &fetches);
    CHECK(final == 17, "eight ones from 1: %u", final);

    /* The level does not wrap: a step that would leave 0..127 is skipped. */
    dmc_play(0xFF, 1, 100, &final, &lo, &hi, &fetches);
    CHECK(fetches == 17, "%u fetches for 17 bytes", fetches);
    CHECK(final == 126 && hi == 126 && lo == 100, "clamp at the top: low %u high %u final %u", lo, hi, final);
    dmc_play(0xFF, 1, 101, &final, &lo, &hi, &fetches);
    CHECK(final == 127 && hi == 127, "odd level reaches 127: high %u final %u", hi, final);
    dmc_play(0x00, 1, 27, &final, &lo, &hi, &fetches);
    CHECK(final == 1 && lo == 1 && hi == 27, "clamp at the bottom: low %u high %u final %u", lo, hi, final);
    dmc_play(0x00, 1, 26, &final, &lo, &hi, &fetches);
    CHECK(final == 0 && lo == 0, "even level reaches 0: low %u final %u", lo, final);

    /* $4011 sets the level directly, 7 bits. */
    NesApu *a = new_apu();
    wr(a, 0x4011, 0xFF);
    CHECK(a->dmc.level == 0x7F, "$4011: %u", a->dmc.level);
    run(a, 5000);
    CHECK(a->dmc.level == 0x7F, "level moved while silent: %u", a->dmc.level);
    free(a);
}

static void test_dmc_first_fetch(void)
{
    memset(g_mem, 0xAA, sizeof g_mem);
    for (unsigned parity = 0; parity < 2; parity++) {
        NesApu *a = new_apu();
        run(a, 30);
        align(a, parity);
        wr(a, 0x4017, 0x40);
        wr(a, 0x4010, 0x8F); /* IRQ enabled */
        wr(a, 0x4012, 0x12);
        wr(a, 0x4013, 0x00);
        const uint64_t A = a->cycle;
        wr(a, 0x4015, 0x10);
        CHECK(peek(a) & 0x10, "bytes remaining right after the write");
        /* The request: the even step that puts the halt on the get cycle
         * of the second APU cycle after the write. */
        const uint64_t s = A + (parity ? 3u : 2u);
        CHECK(nes_apu_cycles_until_event(a) == s + 1 - A, "parity %u: horizon %u", parity, nes_apu_cycles_until_event(a));
        run_to(a, s);
        CHECK(g_log.count == 0, "parity %u: fetched early", parity);
        run(a, 1);
        CHECK(g_log.count == 1 && g_log.fetch[0].cycle == s, "parity %u: fetch at %llu, expected %llu", parity,
              (unsigned long long)g_log.fetch[0].cycle, (unsigned long long)s);
        CHECK((s & 1u) == 0, "load request in an odd step");
        CHECK(g_log.fetch[0].addr == 0xC000 + 0x12 * 64, "address %04X", g_log.fetch[0].addr);
        /* The byte is in the buffer at once (the CPU is halted for the 3
         * cycles of the DMA: halt s+1, dummy s+2, read s+3). */
        CHECK(!(peek(a) & 0x10), "parity %u: bytes remaining after the only byte", parity);
        CHECK((peek(a) & 0x80) && nes_apu_irq(a), "parity %u: IRQ at the end of the sample", parity);
        CHECK(a->dmc.buffer_full && a->dmc.buffer == 0xAA, "parity %u: buffer", parity);
        CHECK(nes_apu_cycles_until_event(a) == NES_APU_EVENT_MAX, "parity %u: horizon after the sample %u", parity, nes_apu_cycles_until_event(a));
        free(a);
    }

    /* Reloads: requested in the odd step whose timer clock ends an output
     * cycle; the byte arrives 4 steps on. */
    NesApu *a = new_apu();
    run(a, 30);
    wr(a, 0x4017, 0x40);
    wr(a, 0x4010, 0x0F);
    wr(a, 0x4013, 0x02); /* 33 bytes */
    wr(a, 0x4015, 0x10);
    while (g_log.count < 3) run(a, 1);
    const uint64_t s = g_log.fetch[2].cycle;
    CHECK(s & 1u, "reload request in an even step");
    CHECK(a->cycle == s + 1, "fetch not logged in its own step");
    CHECK(a->dmc.bits == 8 && a->dmc.shift == 0xAA, "request not at the start of an output cycle");
    CHECK(a->dmc.buffer_full && a->dmc.remaining == 33 - 3 && a->dmc.addr == 0xC003, "reload byte: %u remaining, next %04X", a->dmc.remaining,
          a->dmc.addr);
    CHECK(nes_apu_cycles_until_event(a) == 8u * 54u, "horizon to the next reload %u", nes_apu_cycles_until_event(a));
    run(a, 8u * 54u - 1u);
    CHECK(g_log.count == 3, "reload early");
    run(a, 1);
    CHECK(g_log.count == 4 && g_log.fetch[3].cycle == s + 8u * 54u, "reload late");
    free(a);
}

/* A sample restarted while the buffer still holds a byte, a few cycles
 * before the output unit takes it: the reload the output unit would ask
 * for at once waits until the memory reader has started, 3 or 4 steps
 * after the $4015 write as for a load (AccuracyCoin, "Delta Modulation
 * Channel" tests L, M and N: the DMA comes 1, 2 and 3 cycles late and its
 * length changes with the cycle it lands on). */
static void test_dmc_restart_before_reload(void)
{
    memset(g_mem, 0x55, sizeof g_mem);
    for (int before = -2; before <= 6; before++) {
        NesApu *a = new_apu();
        run(a, 10);
        wr(a, 0x4017, 0x40);
        wr(a, 0x4010, 0x4F); /* loop, fastest rate */
        wr(a, 0x4013, 0x00);
        wr(a, 0x4015, 0x10);
        run(a, 2000);
        /* The step in which the output unit will take the buffered byte. */
        CHECK(a->dmc.buffer_full && a->dmc.remaining, "playing");
        const uint64_t empties = a->cycle + nes_apu_cycles_until_dmc(a) - 1u;
        wr(a, 0x4015, 0x00); /* stop: the buffered byte stays */
        CHECK(a->dmc.buffer_full && !a->dmc.remaining, "stopped with a byte in the buffer");
        g_log.count = 0;
        run_to(a, (uint64_t)((int64_t)empties - before));
        const uint64_t A = a->cycle;
        wr(a, 0x4015, 0x10);
        /* The memory reader starts in step A + 2 (A even) or A + 3. */
        const uint64_t starts = A + ((A & 1u) ? 3u : 2u);
        const uint64_t want = before < 0 ? starts : (empties > starts ? empties : starts);
        CHECK(nes_apu_cycles_until_dmc(a) == want + 1u - A, "written %d steps before: horizon %u, expected %llu", before,
              nes_apu_cycles_until_dmc(a), (unsigned long long)(want + 1u - A));
        run(a, 40);
        CHECK(g_log.count == 1 && g_log.fetch[0].cycle == want, "written %d steps before: fetch in step %llu, expected %llu (%u fetches)",
              before, (unsigned long long)g_log.fetch[0].cycle, (unsigned long long)want, g_log.count);
        free(a);
    }
}

static void test_dmc_control(void)
{
    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)(i * 7u);
    const uint32_t whole = 1000u + 20u * 8u * 54u; /* longer than 17 bytes */

    /* End of sample: bytes remaining clear, IRQ only when enabled. (The
     * frame IRQ is inhibited so the line shows the DMC's alone.) */
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4017, 0x40);
    wr(a, 0x4010, 0x0F);
    wr(a, 0x4012, 0x40);
    wr(a, 0x4013, 0x01); /* 17 bytes */
    wr(a, 0x4015, 0x10);
    CHECK(a->dmc.remaining == 17, "length from $4013: %u", a->dmc.remaining);
    run(a, 3000);
    CHECK(peek(a) & 0x10, "bytes remaining while playing");
    const unsigned remaining = a->dmc.remaining;
    wr(a, 0x4013, 0x05);
    wr(a, 0x4012, 0x00);
    CHECK(a->dmc.remaining == remaining, "$4013 changed the current sample");
    wr(a, 0x4015, 0x10);
    CHECK(a->dmc.remaining == remaining, "$4015 = $10 restarted a playing sample");
    run(a, whole);
    CHECK(!(peek(a) & 0x10) && !(peek(a) & 0x80) && !nes_apu_irq(a), "end without IRQ: %02X", peek(a));
    CHECK(g_log.count == 17, "%u fetches", g_log.count);
    for (unsigned i = 0; i < 17 && i < g_log.count; i++) {
        CHECK(g_log.fetch[i].addr == 0xD000 + i, "fetch %u from %04X", i, g_log.fetch[i].addr);
    }

    /* $4015 = $10 after the end starts again with the new address and
     * length; this time with the IRQ. */
    g_log.count = 0;
    wr(a, 0x4013, 0x01);
    wr(a, 0x4010, 0x8F);
    wr(a, 0x4015, 0x10);
    CHECK(a->dmc.remaining == 17 && (peek(a) & 0x10), "restart");
    run(a, whole);
    CHECK(g_log.count == 17 && g_log.fetch[0].addr == 0xC000 && g_log.fetch[16].addr == 0xC010, "restarted sample's fetches");
    CHECK((peek(a) & 0x90) == 0x80 && nes_apu_irq(a), "end with IRQ: %02X", peek(a));
    CHECK(nes_apu_read(a, 0x4015) & 0x80, "read");
    run(a, 4);
    CHECK((peek(a) & 0x80) && nes_apu_irq(a), "reading $4015 cleared the DMC IRQ");
    wr(a, 0x4010, 0x0F);
    CHECK(!(peek(a) & 0x80) && !nes_apu_irq(a), "clearing the IRQ enable bit left the flag");

    g_log.count = 0;
    wr(a, 0x4010, 0x8F);
    wr(a, 0x4015, 0x10);
    run(a, whole);
    CHECK(peek(a) & 0x80, "second IRQ");
    wr(a, 0x4015, 0x00);
    CHECK(!(peek(a) & 0x80) && !nes_apu_irq(a), "a $4015 write left the DMC IRQ");
    wr(a, 0x4015, 0x10);
    run(a, whole);
    CHECK(peek(a) & 0x80, "third IRQ");
    wr(a, 0x4015, 0x10); /* also restarts */
    CHECK(!(peek(a) & 0x80) && (peek(a) & 0x10), "$4015 = $10 left the DMC IRQ");

    /* $4015 = $00 stops at once. */
    run(a, 2000);
    CHECK(peek(a) & 0x10, "playing");
    wr(a, 0x4015, 0x00);
    CHECK(!(peek(a) & 0x10), "stop");
    g_log.count = 0;
    run(a, whole);
    CHECK(g_log.count == 0 && !(peek(a) & 0x80), "fetches or IRQ after a stop");
    free(a);

    /* Loop: restarts from the sample address with the latched length, no
     * IRQ, until the flag is cleared; then it ends normally. */
    a = new_apu();
    run(a, 10);
    wr(a, 0x4017, 0x40);
    wr(a, 0x4010, 0xCF);
    wr(a, 0x4012, 0x40);
    wr(a, 0x4013, 0x01);
    wr(a, 0x4015, 0x10);
    bool irq = false, stopped = false;
    for (uint32_t i = 0; i < 3u * whole; i++) {
        run(a, 1);
        irq |= nes_apu_irq(a);
        stopped |= !(peek(a) & 0x10);
    }
    CHECK(!irq && !stopped, "looped sample ended or raised its IRQ");
    CHECK(g_log.count > 40 && g_log.fetch[17].addr == 0xD000 && g_log.fetch[16].addr == 0xD010 && g_log.fetch[34].addr == 0xD000,
          "loop addresses");
    wr(a, 0x4013, 0x02); /* the next pass is 33 bytes */
    wr(a, 0x4010, 0x8F);
    wr(a, 0x4010, 0xCF); /* clearing and setting the loop flag changes nothing */
    run(a, whole);
    CHECK(peek(a) & 0x10, "loop stopped by toggling the flag");
    const unsigned at = g_log.count;
    unsigned pass_start = at;
    while (pass_start > 0 && g_log.fetch[pass_start - 1].addr != 0xD000) pass_start--;
    wr(a, 0x4010, 0x8F);
    run(a, 3u * whole);
    CHECK(!(peek(a) & 0x10) && (peek(a) & 0x80), "sample did not end after the loop flag was cleared: %02X", peek(a));
    CHECK(pass_start >= 1 && g_log.count == pass_start - 1u + 33u, "last pass: %u fetches from %u, started at %u", g_log.count, at, pass_start);
    free(a);

    /* The address wraps from $FFFF to $8000. */
    a = new_apu();
    run(a, 10);
    wr(a, 0x4010, 0x0F);
    wr(a, 0x4012, 0xFF); /* $FFC0 */
    wr(a, 0x4013, 0x04); /* 65 bytes */
    wr(a, 0x4015, 0x10);
    run(a, 1000u + 70u * 8u * 54u);
    CHECK(g_log.count == 65, "%u fetches", g_log.count);
    CHECK(g_log.fetch[0].addr == 0xFFC0 && g_log.fetch[63].addr == 0xFFFF, "addresses up to $FFFF: %04X %04X", g_log.fetch[0].addr,
          g_log.fetch[63].addr);
    CHECK(g_log.fetch[64].addr == 0x8000, "wrap to %04X", g_log.fetch[64].addr);
    free(a);

    /* A stop keeps the buffered byte, and a start with the buffer full
     * needs no load: the old byte plays, then the new sample is fetched
     * from its first address. */
    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)(i * 7u);
    a = new_apu();
    run(a, 11);
    wr(a, 0x4017, 0x40);
    wr(a, 0x4010, 0x0F);
    wr(a, 0x4012, 0x40);
    wr(a, 0x4013, 0x01);
    wr(a, 0x4015, 0x10);
    run(a, 10);
    CHECK(g_log.count == 1 && a->dmc.buffer_full && a->dmc.remaining == 16, "first byte");
    wr(a, 0x4015, 0x00);
    CHECK(a->dmc.buffer_full && a->dmc.remaining == 0, "stop dropped the buffered byte");
    wr(a, 0x4012, 0x80);
    wr(a, 0x4015, 0x10);
    run(a, 10);
    CHECK(g_log.count == 1 && a->dmc.remaining == 17, "start with a full buffer fetched at once");
    run(a, 2000);
    CHECK(g_log.count >= 2 && g_log.fetch[1].addr == 0xE000 && (g_log.fetch[1].cycle & 1u), "new sample's first fetch from %04X",
          g_log.fetch[1].addr);
    free(a);

    /* A stop before the load fetch is made: no fetch at all, and the
     * next start makes its own. */
    a = new_apu();
    run(a, 11);
    wr(a, 0x4010, 0x0F);
    wr(a, 0x4015, 0x10);
    run(a, 1);
    wr(a, 0x4015, 0x00);
    run(a, 2000);
    CHECK(g_log.count == 0 && !a->dmc.buffer_full, "fetch after a stop: %u", g_log.count);
    align(a, 1);
    const uint64_t start = a->cycle;
    wr(a, 0x4015, 0x10);
    run(a, 4);
    CHECK(g_log.count == 1 && g_log.fetch[0].cycle == start + 3, "load after a cancelled load");
    free(a);

    /* A stop and a start before the load fetch: the delay starts again. */
    a = new_apu();
    run(a, 12);
    wr(a, 0x4010, 0x0F);
    wr(a, 0x4015, 0x10); /* at 12: would fetch in step 14 */
    run(a, 1);
    wr(a, 0x4015, 0x00);
    wr(a, 0x4015, 0x10); /* at 13: fetches in step 16 */
    run(a, 10);
    CHECK(g_log.count == 1 && g_log.fetch[0].cycle == 16, "load restarted: fetch at %llu", (unsigned long long)g_log.fetch[0].cycle);
    free(a);
}

/* ---- Status register ---------------------------------------------------- */

static void test_status(void)
{
    memset(g_mem, 0, sizeof g_mem);
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4017, 0x40);
    CHECK(peek(a) == 0x00, "idle status %02X", peek(a));
    wr(a, 0x4015, 0x1F);
    CHECK(peek(a) == 0x10, "DMC started: %02X", peek(a));
    wr(a, 0x4003, 0x08);
    CHECK(peek(a) == 0x11, "%02X", peek(a));
    wr(a, 0x4007, 0x08);
    CHECK(peek(a) == 0x13, "%02X", peek(a));
    wr(a, 0x400B, 0x08);
    CHECK(peek(a) == 0x17, "%02X", peek(a));
    wr(a, 0x400F, 0x08);
    CHECK(peek(a) == 0x1F, "%02X", peek(a));
    CHECK(nes_apu_read(a, 0x4015) == 0x1F, "read");
    run(a, 20);
    CHECK(peek(a) == 0x0F, "one-byte sample fetched: %02X", peek(a));
    /* Bit 5 is never driven: the caller supplies it. */
    wr(a, 0x4017, 0x00);
    run(a, 30000);
    CHECK(peek(a) == 0x4F, "with the frame IRQ: %02X", peek(a));
    wr(a, 0x4010, 0x80);
    wr(a, 0x4015, 0x1F);
    run(a, 20);
    CHECK(peek(a) == 0xCF, "with both IRQs: %02X", peek(a));
    CHECK(nes_apu_read(a, 0x4015) == 0xCF, "read");
    run(a, 2);
    CHECK(peek(a) == 0x8F, "the read clears the frame IRQ only: %02X", peek(a));
    wr(a, 0x4015, 0x00);
    CHECK(peek(a) == 0x00, "all off: %02X", peek(a));
    free(a);
}

/* ---- Mixer tables ------------------------------------------------------- */

static void test_mixer_tables(void)
{
    double worst = 0;
    CHECK(nes_apu_pulse_dac[0] == 0, "pulse[0]");
    for (unsigned n = 1; n < 31; n++) {
        const double v = 65536.0 * 95.88 / (8128.0 / n + 100.0);
        const double err = fabs(nes_apu_pulse_dac[n] - v);
        if (err > worst) worst = err;
        CHECK(nes_apu_pulse_dac[n] > nes_apu_pulse_dac[n - 1], "pulse table not monotonic at %u", n);
    }
    CHECK(worst <= 0.5001, "pulse table off by %.4f", worst);

    worst = 0;
    unsigned bad_order = 0;
    CHECK(nes_apu_tnd_dac[0] == 0, "tnd[0]");
    for (unsigned d = 0; d < 128; d++) {
        for (unsigned t = 0; t < 16; t++) {
            for (unsigned n = 0; n < 16; n++) {
                const unsigned i = (d << 8) | (t << 4) | n;
                if (i == 0) continue;
                const double v = 65536.0 * 159.79 / (1.0 / (t / 8227.0 + n / 12241.0 + d / 22638.0) + 100.0);
                const double err = fabs(nes_apu_tnd_dac[i] - v);
                if (err > worst) worst = err;
                if (n && nes_apu_tnd_dac[i] <= nes_apu_tnd_dac[i - 1]) bad_order++;
                if (t && nes_apu_tnd_dac[i] <= nes_apu_tnd_dac[i - 16]) bad_order++;
                if (d && nes_apu_tnd_dac[i] <= nes_apu_tnd_dac[i - 256]) bad_order++;
            }
        }
    }
    CHECK(worst <= 0.5001, "tnd table off by %.4f", worst);
    CHECK(bad_order == 0, "tnd table not monotonic in %u places", bad_order);
    CHECK(nes_apu_pulse_dac[30] + nes_apu_tnd_dac[32767] == 65536, "full scale %u", nes_apu_pulse_dac[30] + nes_apu_tnd_dac[32767]);

    /* The decimator's taps: unity gain at DC, linear phase. */
    int sum = 0, asym = 0;
    for (unsigned k = 0; k < NES_APU_FIR_TAPS; k++) {
        sum += nes_apu_fir[k];
        asym |= nes_apu_fir[k] != nes_apu_fir[NES_APU_FIR_TAPS - 1 - k];
    }
    CHECK(sum == 32768 && !asym, "FIR taps: sum %d, asymmetric %d", sum, asym);
}

/* ---- Output stage ------------------------------------------------------- */

#define CAPTURE_MAX (2u * 192000u)
static int16_t g_capture[CAPTURE_MAX];

/* Run for `cycles`, draining as a frontend would; returns the samples. */
static size_t capture(NesApu *a, uint64_t cycles, int16_t *dst, size_t max)
{
    size_t n = 0;
    while (cycles) {
        const uint32_t chunk = cycles > 29780 ? 29780u : (uint32_t)cycles;
        run(a, chunk);
        cycles -= chunk;
        n += nes_apu_read_samples(a, (dst ? dst : g_capture) + n, (dst ? max : CAPTURE_MAX) - n);
    }
    return n;
}

/* The amplitude of the component at `hz` (Hann-windowed correlation). */
static double tone_amplitude(const int16_t *x, size_t n, double hz, double rate)
{
    double re = 0, im = 0, wsum = 0;
    for (size_t i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * cos(2.0 * PI * (double)i / (double)n);
        const double ph = 2.0 * PI * hz * (double)i / rate;
        re += w * x[i] * cos(ph);
        im -= w * x[i] * sin(ph);
        wsum += w;
    }
    return 2.0 * sqrt(re * re + im * im) / wsum;
}

#define FFT_N 32768u
static double g_re[FFT_N], g_im[FFT_N];

/* In-place radix-2 FFT of g_re/g_im. */
static void fft(void)
{
    for (unsigned i = 1, j = 0; i < FFT_N; i++) {
        unsigned bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = g_re[i];
            g_re[i] = g_re[j];
            g_re[j] = t;
            t = g_im[i];
            g_im[i] = g_im[j];
            g_im[j] = t;
        }
    }
    for (unsigned len = 2; len <= FFT_N; len <<= 1) {
        const double ang = -2.0 * PI / len;
        for (unsigned i = 0; i < FFT_N; i += len) {
            for (unsigned k = 0; k < len / 2; k++) {
                const double wr_ = cos(ang * k), wi = sin(ang * k);
                const unsigned p = i + k, q = i + k + len / 2;
                const double xr = g_re[q] * wr_ - g_im[q] * wi;
                const double xi = g_re[q] * wi + g_im[q] * wr_;
                g_re[q] = g_re[p] - xr;
                g_im[q] = g_im[p] - xi;
                g_re[p] += xr;
                g_im[p] += xi;
            }
        }
    }
}

/* The largest component (as a sine amplitude) between two frequencies. */
static double band_peak(const int16_t *x, double from_hz, double to_hz, double rate)
{
    double wsum = 0;
    for (unsigned i = 0; i < FFT_N; i++) {
        const double w = 0.5 - 0.5 * cos(2.0 * PI * i / FFT_N);
        g_re[i] = w * x[i];
        g_im[i] = 0;
        wsum += w;
    }
    fft();
    double peak = 0;
    const unsigned lo = (unsigned)(from_hz * FFT_N / rate), hi = (unsigned)(to_hz * FFT_N / rate);
    for (unsigned b = lo; b <= hi && b < FFT_N / 2; b++) {
        const double m = 2.0 * sqrt(g_re[b] * g_re[b] + g_im[b] * g_im[b]) / wsum;
        if (m > peak) peak = m;
    }
    return peak;
}

static double filter_gain(double hz)
{
    const double hp1 = hz / sqrt(hz * hz + 90.0 * 90.0);
    const double hp2 = hz / sqrt(hz * hz + 440.0 * 440.0);
    const double lp = 1.0 / sqrt(1.0 + (hz / 14000.0) * (hz / 14000.0));
    return hp1 * hp2 * lp;
}

static void test_output_silence_and_dc(void)
{
    /* Nothing playing: exact zeros from the first sample, although the
     * DAC is not at zero (the triangle rests at 15). */
    NesApu *a = new_apu();
    size_t n = capture(a, CYCLES_PER_SECOND, NULL, 0);
    CHECK(n >= 47999 && n <= 48001, "%u samples in a second", (unsigned)n);
    unsigned nonzero = 0;
    for (size_t i = 0; i < n; i++) nonzero += g_capture[i] != 0;
    CHECK(nonzero == 0, "%u non-zero samples of silence", nonzero);
    CHECK(a->overruns == 0, "overruns %u", a->overruns);

    /* A step in the DAC level passes, then decays to exactly zero. */
    wr(a, 0x4011, 0x7F);
    n = capture(a, CYCLES_PER_SECOND, NULL, 0);
    int peak = 0;
    for (size_t i = 0; i < 480; i++) {
        if (g_capture[i] > peak) peak = g_capture[i];
    }
    const double step = (nes_apu_tnd_dac[(127u << 8) | (15u << 4)] - nes_apu_tnd_dac[15u << 4]) / 2.0;
    CHECK(peak > 0.75 * step && peak < 1.10 * step, "step of %.0f peaks at %d", step, peak);
    int tail = 0;
    for (size_t i = n / 2; i < n; i++) {
        if (abs(g_capture[i]) > tail) tail = abs(g_capture[i]);
    }
    CHECK(tail == 0, "DC left after half a second: %d", tail);
    /* 20 ms on, the two high-pass filters have taken nearly all of it. */
    CHECK(abs(g_capture[960]) < 0.01 * step, "DC after 20 ms: %d", g_capture[960]);

    /* Back down: the mirror image, and again to exactly zero. */
    wr(a, 0x4011, 0x00);
    n = capture(a, CYCLES_PER_SECOND, NULL, 0);
    int trough = 0;
    for (size_t i = 0; i < 480; i++) {
        if (g_capture[i] < trough) trough = g_capture[i];
    }
    CHECK(-trough > 0.75 * step && -trough < 1.10 * step, "step down troughs at %d", trough);
    tail = 0;
    for (size_t i = n / 2; i < n; i++) {
        if (abs(g_capture[i]) > tail) tail = abs(g_capture[i]);
    }
    CHECK(tail == 0, "DC left after the step down: %d", tail);
    free(a);
}

static void test_output_tone(void)
{
    /* Pulse 1, 50 % duty, volume 15, period 111: 998.8 Hz. */
    NesApu *a = new_apu();
    wr(a, 0x4015, 0x01);
    wr(a, 0x4000, 0xBF);
    wr(a, 0x4002, 111);
    wr(a, 0x4003, 0x08);
    capture(a, CYCLES_PER_SECOND / 2, NULL, 0);
    const size_t n = capture(a, CYCLES_PER_SECOND, NULL, 0);
    const double hz = CPU_HZ / (16.0 * 112.0);
    const double pp = nes_apu_pulse_dac[15] / 2.0; /* the DAC's swing in output units */
    const double expected = 2.0 / PI * pp * filter_gain(hz);
    const double got = tone_amplitude(g_capture, n, hz, 48000.0);
    CHECK(fabs(got - expected) < 0.02 * expected, "1 kHz fundamental %.1f, expected %.1f", got, expected);
    /* The third harmonic of a square is a third of the fundamental. */
    const double third = tone_amplitude(g_capture, n, 3.0 * hz, 48000.0);
    const double expected3 = 2.0 / (3.0 * PI) * pp * filter_gain(3.0 * hz);
    CHECK(fabs(third - expected3) < 0.03 * expected3, "third harmonic %.1f, expected %.1f", third, expected3);
    int lo = 0, hi = 0;
    long long sum = 0;
    for (size_t i = 0; i < n; i++) {
        if (g_capture[i] < lo) lo = g_capture[i];
        if (g_capture[i] > hi) hi = g_capture[i];
        sum += g_capture[i];
    }
    CHECK(hi > 0.45 * pp && hi < 0.95 * pp && -lo > 0.45 * pp && -lo < 0.95 * pp, "square of %.0f swings %d..%d", pp, lo, hi);
    CHECK(llabs(sum) < 4 * (long long)n, "mean %.3f: DC in the output", (double)sum / (double)n);
    free(a);

    /* The mute switch takes the channel out of the mix and leaves the
     * emulation alone. */
    a = new_apu();
    NesApu *b = new_apu();
    for (NesApu *x = a;; x = b) {
        wr(x, 0x4015, 0x01);
        wr(x, 0x4000, 0xBF);
        wr(x, 0x4002, 111);
        wr(x, 0x4003, 0x08);
        if (x == b) break;
    }
    nes_apu_set_mute(b, 1u << NES_APU_PULSE1);
    capture(a, CYCLES_PER_SECOND / 4, NULL, 0);
    const size_t m = capture(b, CYCLES_PER_SECOND / 4, NULL, 0);
    unsigned nonzero = 0;
    for (size_t i = m / 2; i < m; i++) nonzero += g_capture[i] != 0;
    CHECK(nonzero == 0, "muted channel audible in %u samples", nonzero);
    CHECK(memcmp(a->pulse, b->pulse, sizeof a->pulse) == 0 && memcmp(a->len, b->len, sizeof a->len) == 0 && a->fc_pos == b->fc_pos,
          "mute changed the channel state");
    CHECK(nes_apu_channel_level(a, 0) == nes_apu_channel_level(b, 0), "channel level ignores the mute");
    nes_apu_set_mute(b, 0xFF);
    CHECK(b->mute == 0x1F, "mute mask %02X", b->mute);
    free(a);
    free(b);
}

static void test_output_aliasing(void)
{
    /* Pulse period 8: 12.43 kHz, the highest the channel plays. Its odd
     * harmonics lie far above the output's Nyquist frequency and would
     * fold down to 10.7, 9.0, 7.3, 5.6 ... kHz. */
    NesApu *a = new_apu();
    wr(a, 0x4015, 0x01);
    wr(a, 0x4000, 0xBF);
    wr(a, 0x4002, 8);
    wr(a, 0x4003, 0x08);
    capture(a, CYCLES_PER_SECOND / 2, NULL, 0);
    size_t n = capture(a, CYCLES_PER_SECOND, NULL, 0);
    CHECK(n >= FFT_N, "capture too short");
    const double hz = CPU_HZ / (16.0 * 9.0);
    const double pp = nes_apu_pulse_dac[15] / 2.0;
    const double fundamental = tone_amplitude(g_capture, FFT_N, hz, 48000.0);
    const double expected = 2.0 / PI * pp * filter_gain(hz);
    CHECK(fabs(fundamental - expected) < 0.04 * expected, "12.4 kHz fundamental %.1f, expected %.1f", fundamental, expected);
    double low = band_peak(g_capture, 20.0, 11500.0, 48000.0);
    CHECK(low < fundamental / 200.0, "aliases up to %.2f below 11.5 kHz against a fundamental of %.1f (%.1f dB)", low, fundamental,
          20.0 * log10(low / fundamental));
    printf("    12.4 kHz pulse: fundamental %.1f, largest alias below 11.5 kHz %.2f (%.1f dB)\n", fundamental, low, 20.0 * log10(low / fundamental));
    free(a);

    /* The triangle at periods 0 and 1 (55.9 and 28.0 kHz) is ultrasonic:
     * on hardware it comes out as a level, not as sound. */
    for (unsigned period = 0; period < 2; period++) {
        a = new_apu();
        wr(a, 0x4015, 0x04);
        wr(a, 0x4008, 0xFF);
        wr(a, 0x400A, (uint8_t)period);
        wr(a, 0x400B, 0x08);
        clock_frame(a);
        capture(a, CYCLES_PER_SECOND / 2, NULL, 0);
        n = capture(a, CYCLES_PER_SECOND, NULL, 0);
        CHECK(a->tri.linear != 0 && a->len[2].counter != 0, "triangle not running");
        const double swing = nes_apu_tnd_dac[15u << 4] / 2.0;
        low = band_peak(g_capture, 20.0, 20000.0, 48000.0);
        double rms = 0;
        for (size_t i = 0; i < n; i++) rms += (double)g_capture[i] * g_capture[i];
        rms = sqrt(rms / (double)n);
        CHECK(low < swing / 300.0, "triangle period %u: %.2f in the audible band of a %.0f swing", period, low, swing);
        CHECK(rms < swing / 100.0, "triangle period %u: rms %.2f of a %.0f swing", period, rms, swing);
        printf("    triangle period %u: audible peak %.2f, rms %.2f (swing %.0f)\n", period, low, rms, swing);
        free(a);
    }
}

static void test_output_rates(void)
{
    static const uint32_t rates[] = { 8000, 22050, 32000, 44100, 48000, 96000, 192000 };
    for (unsigned r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        NesApu *a = new_apu();
        nes_apu_set_rate(a, rates[r]);
        CHECK(a->rate == rates[r] && a->ring_count == 0, "rate %u", a->rate);
        wr(a, 0x4015, 0x01);
        wr(a, 0x4000, 0xBF);
        wr(a, 0x4002, 223); /* 499.4 Hz */
        wr(a, 0x4003, 0x08);
        capture(a, CYCLES_PER_SECOND / 2, NULL, 0);
        const uint32_t before = a->samples;
        const size_t n = capture(a, CYCLES_PER_SECOND, NULL, 0);
        CHECK(a->samples - before == n, "sample counter");
        CHECK(n + 1 >= rates[r] && n <= rates[r] + 1u, "%u samples in a second at %u Hz", (unsigned)n, rates[r]);
        const double hz = CPU_HZ / (16.0 * 224.0);
        const double expected = 2.0 / PI * (nes_apu_pulse_dac[15] / 2.0) * filter_gain(hz);
        const double got = tone_amplitude(g_capture, n, hz, (double)rates[r]);
        CHECK(fabs(got - expected) < 0.02 * expected, "%u Hz: 500 Hz tone %.1f, expected %.1f", rates[r], got, expected);
        CHECK(a->overruns == 0, "overruns at %u Hz", rates[r]);

        /* Silence settles on exact zeros at every rate. */
        wr(a, 0x4015, 0x00);
        const size_t m = capture(a, CYCLES_PER_SECOND, NULL, 0);
        unsigned nonzero = 0;
        for (size_t i = m / 2; i < m; i++) nonzero += g_capture[i] != 0;
        CHECK(nonzero == 0, "%u Hz: %u non-zero samples of silence", rates[r], nonzero);
        free(a);
    }

    NesApu *a = new_apu();
    nes_apu_set_rate(a, 1);
    CHECK(a->rate == NES_APU_MIN_RATE, "rate clamp %u", a->rate);
    nes_apu_set_rate(a, 4000000);
    CHECK(a->rate == NES_APU_MAX_RATE, "rate clamp %u", a->rate);
    free(a);
}

static void test_output_ring(void)
{
    /* Without a reader the ring keeps the newest samples and counts the
     * rest. A twin that is drained provides the reference stream. */
    NesApu *a = new_apu();
    NesApu *b = new_apu();
    static int16_t all[40000];
    size_t total = 0;
    for (NesApu *x = a;; x = b) {
        wr(x, 0x4015, 0x0F);
        wr(x, 0x4000, 0x7F);
        wr(x, 0x4002, 0x54);
        wr(x, 0x4003, 0x09);
        wr(x, 0x400C, 0x3F);
        wr(x, 0x400E, 0x05);
        wr(x, 0x400F, 0x08);
        if (x == b) break;
    }
    for (unsigned i = 0; i < 1000; i++) {
        run(a, 1193);
        run(b, 1193);
        total += nes_apu_read_samples(b, all + total, 40000 - total);
    }
    CHECK(total > NES_APU_RING_SAMPLES + 1000 && total < 40000, "%u samples", (unsigned)total);
    CHECK(nes_apu_samples_available(a) == NES_APU_RING_SAMPLES, "ring holds %u", (unsigned)nes_apu_samples_available(a));
    CHECK(a->overruns == total - NES_APU_RING_SAMPLES, "overruns %u, expected %u", a->overruns, (unsigned)(total - NES_APU_RING_SAMPLES));
    CHECK(b->overruns == 0, "drained twin overran");
    static int16_t kept[NES_APU_RING_SAMPLES];
    /* Partial reads, then the rest. */
    size_t got = nes_apu_read_samples(a, kept, 100);
    got += nes_apu_read_samples(a, kept + got, 5000);
    got += nes_apu_read_samples(a, kept + got, NES_APU_RING_SAMPLES);
    CHECK(got == NES_APU_RING_SAMPLES, "read %u", (unsigned)got);
    CHECK(memcmp(kept, all + total - NES_APU_RING_SAMPLES, sizeof kept) == 0, "the ring did not keep the newest samples");
    CHECK(nes_apu_read_samples(a, kept, 16) == 0 && nes_apu_samples_available(a) == 0, "ring not empty");
    int loud = 0;
    for (size_t i = 0; i < total; i++) {
        if (abs(all[i]) > loud) loud = abs(all[i]);
    }
    CHECK(loud > 1000 && loud < 32767, "pulse + noise peak %d", loud);
    free(a);
    free(b);
}

/* ---- Chunked runs ------------------------------------------------------- */

typedef struct {
    uint64_t cycle;
    uint16_t addr; /* 0: drain, 1: read $4015, 2: reset, else a register */
    uint8_t value;
} ScriptEvent;

#define SCRIPT_EVENTS 1400u

/* One write of a script, `gap` cycles after the previous event. */
static unsigned script_write(ScriptEvent *ev, unsigned i, uint64_t *at, uint32_t gap, uint16_t addr, uint8_t value)
{
    if (i >= SCRIPT_EVENTS) return i;
    *at += gap;
    ev[i].cycle = *at;
    ev[i].addr = addr;
    ev[i].value = value;
    return i + 1u;
}

/* A random session that keeps all five channels busy: notes (the writes of
 * a note land on successive instructions, a few cycles apart), raw
 * register writes, DMC samples, frame counter modes, status reads, drains
 * and the odd reset. Every rnd() call is its own statement, so the script
 * does not depend on the compiler's order of evaluation. */
static void make_script(ScriptEvent *ev, uint64_t span)
{
    static const uint8_t modes[6] = { 0x00, 0x00, 0x00, 0x80, 0x40, 0xC0 };
    uint64_t at = 5;
    unsigned i = 0;
    while (i < SCRIPT_EVENTS) {
        const uint32_t gap = 1u + rnd() % (uint32_t)(3u * span / SCRIPT_EVENTS);
        const unsigned kind = rnd() % 100u;
        const uint8_t v0 = (uint8_t)rnd();
        const uint8_t v1 = (uint8_t)rnd();
        const uint8_t v2 = (uint8_t)rnd();
        const uint8_t v3 = (uint8_t)rnd();
        if (kind < 10) {
            i = script_write(ev, i, &at, gap, 0, 0); /* drain */
        } else if (kind < 16) {
            i = script_write(ev, i, &at, gap, 1, 0); /* read $4015 */
        } else if (kind < 17) {
            i = script_write(ev, i, &at, gap, 2, 0); /* reset */
        } else if (kind < 25) {
            i = script_write(ev, i, &at, gap, 0x4015, (v1 & 7u) ? 0x1Fu : v0);
        } else if (kind < 31) {
            i = script_write(ev, i, &at, gap, 0x4017, modes[v0 % 6u]);
        } else if (kind < 37) {
            /* A DMC sample at one of the fast rates. */
            i = script_write(ev, i, &at, gap, 0x4010, (uint8_t)((v0 & 0xC0u) | 0x08u | (v1 & 7u)));
            i = script_write(ev, i, &at, 4u + (v1 >> 5), 0x4012, v2);
            i = script_write(ev, i, &at, 4u + (v2 >> 5), 0x4013, v3 & 3u);
            i = script_write(ev, i, &at, 4u + (v3 >> 5), 0x4015, 0x1Fu);
        } else if (kind < 40) {
            i = script_write(ev, i, &at, gap, 0x4011, v0);
        } else if (kind < 56) {
            /* A pulse note: duty/envelope, sweep, period, length. */
            const uint16_t base = (uint16_t)(0x4000u + 4u * (v0 & 1u));
            i = script_write(ev, i, &at, gap, base, v1);
            i = script_write(ev, i, &at, 4u + (v0 >> 6), (uint16_t)(base + 1u), (v2 & 0x40u) ? v2 : 0x08u);
            i = script_write(ev, i, &at, 4u + (v1 >> 6), (uint16_t)(base + 2u), v3);
            i = script_write(ev, i, &at, 4u + (v2 >> 6), (uint16_t)(base + 3u), (uint8_t)((v0 & 0xF8u) | ((v2 >> 3) & 3u)));
        } else if (kind < 66) {
            /* A triangle note. */
            i = script_write(ev, i, &at, gap, 0x4008, v0);
            i = script_write(ev, i, &at, 4u + (v0 >> 6), 0x400A, v1);
            i = script_write(ev, i, &at, 4u + (v1 >> 6), 0x400B, (uint8_t)((v2 & 0xF8u) | (v3 & 1u)));
        } else if (kind < 76) {
            /* A noise note. */
            i = script_write(ev, i, &at, gap, 0x400C, v0);
            i = script_write(ev, i, &at, 4u + (v0 >> 6), 0x400E, v1 & 0x8Fu);
            i = script_write(ev, i, &at, 4u + (v1 >> 6), 0x400F, v2);
        } else {
            i = script_write(ev, i, &at, gap, (uint16_t)(0x4000u + v0 % 0x14u), v1);
        }
    }
}

/* Play the script; chunk == 0 runs cycle by cycle, otherwise in random
 * chunks of up to `chunk` cycles. Returns the number of samples. */
static size_t play_script(NesApu *a, const ScriptEvent *ev, uint32_t chunk, int16_t *out, size_t max)
{
    size_t n = 0;
    for (unsigned i = 0; i < SCRIPT_EVENTS; i++) {
        while (a->cycle < ev[i].cycle) {
            uint32_t step = 1;
            if (chunk) {
                step = 1u + rnd() % chunk;
                if (step > ev[i].cycle - a->cycle) step = (uint32_t)(ev[i].cycle - a->cycle);
            }
            nes_apu_run(a, step);
        }
        if (ev[i].addr == 0) n += nes_apu_read_samples(a, out + n, max - n);
        else if (ev[i].addr == 1) (void)nes_apu_read(a, 0x4015);
        else if (ev[i].addr == 2) nes_apu_reset(a);
        else nes_apu_write(a, ev[i].addr, ev[i].value);
    }
    n += nes_apu_read_samples(a, out + n, max - n);
    return n;
}

static void test_chunked_runs(void)
{
    static ScriptEvent script[SCRIPT_EVENTS];
    static int16_t out_a[CAPTURE_MAX / 2], out_b[CAPTURE_MAX / 2];
    static FetchLog log_a, log_b;
    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)rnd();
    static const uint32_t chunks[] = { 2, 7, 113, 5000, 100000 };
    for (unsigned c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
        make_script(script, 3u * CYCLES_PER_SECOND);
        NesApu *a = new_apu_over(0x11);
        NesApu *b = new_apu_over(0xEE);
        log_a.count = log_b.count = 0;
        a->user = &log_a;
        b->user = &log_b;
        const size_t na = play_script(a, script, 0, out_a, CAPTURE_MAX / 2);
        const size_t nb = play_script(b, script, chunks[c], out_b, CAPTURE_MAX / 2);
        CHECK(na == nb && na > 60000, "chunk %u: %u samples against %u", chunks[c], (unsigned)nb, (unsigned)na);
        CHECK(memcmp(out_a, out_b, na * sizeof out_a[0]) == 0, "chunk %u: samples differ", chunks[c]);
        CHECK(log_a.count == log_b.count && log_a.count > 100 && log_a.count < MAX_FETCHES, "chunk %u: %u fetches against %u", chunks[c],
              log_b.count, log_a.count);
        CHECK(memcmp(log_a.fetch, log_b.fetch, (log_a.count < MAX_FETCHES ? log_a.count : MAX_FETCHES) * sizeof(Fetch)) == 0,
              "chunk %u: fetches differ", chunks[c]);
        a->user = b->user = NULL;
        CHECK(memcmp(a, b, sizeof *a) == 0, "chunk %u: final states differ", chunks[c]);
        int loud = 0;
        for (size_t i = 0; i < na; i++) {
            if (abs(out_a[i]) > loud) loud = abs(out_a[i]);
        }
        CHECK(loud > 2000, "chunk %u: the script is nearly silent (peak %d)", chunks[c], loud);
        free(a);
        free(b);
    }
}

/* The sample stream of a fixed script, pinned: the output stage is integer
 * arithmetic, so every compiler and platform must produce these exact
 * samples. A deliberate change to the APU's sound or timing changes the
 * hash; update it from the line this test prints (and say so in the
 * commit). */
/* 2026-10-06: re-pinned when the frame counter moved to put steps (see
 * "Frame counter" in nes_apu.h); it was 0xF6C4F1F188B7BF05. */
#define GOLDEN_STREAM_HASH UINT64_C(0x85809062156BFEEA)

static void test_golden_stream(void)
{
    static ScriptEvent script[SCRIPT_EVENTS];
    static int16_t out[CAPTURE_MAX / 2];
    static FetchLog log;
    g_rng = 0x4E455331u;
    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)rnd();
    make_script(script, 3u * CYCLES_PER_SECOND);
    NesApu *a = new_apu();
    log.count = 0;
    a->user = &log;
    const size_t n = play_script(a, script, 977, out, CAPTURE_MAX / 2);
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    for (size_t i = 0; i < n; i++) {
        const unsigned v = (uint16_t)out[i];
        hash = (hash ^ (v & 0xFFu)) * UINT64_C(0x100000001B3);
        hash = (hash ^ (v >> 8)) * UINT64_C(0x100000001B3);
    }
    hash = (hash ^ (log.count & 0xFFFFu)) * UINT64_C(0x100000001B3);
    printf("    %u samples, %u fetches, hash 0x%016llX\n", (unsigned)n, log.count, (unsigned long long)hash);
    CHECK(hash == GOLDEN_STREAM_HASH, "sample stream hash 0x%016llX, expected 0x%016llX", (unsigned long long)hash,
          (unsigned long long)GOLDEN_STREAM_HASH);
    free(a);
}

/* ---- Event horizon ------------------------------------------------------ */

static void test_event_horizon(void)
{
    static FetchLog log;
    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)rnd();
    NesApu *a = new_apu();
    a->user = &log;
    log.count = 0;
    unsigned queries = 0, early = 0, exact = 0, finite = 0, irq_edges = 0, fetches = 0;
    while (a->cycle < 12u * CYCLES_PER_SECOND) {
        /* Something for the APU to do, now and then. */
        const unsigned kind = rnd() % 96u;
        if (kind < 6) {
            const unsigned loop = rnd() & 1u;
            const unsigned rate = 0x08u | (rnd() & 7u);
            nes_apu_write(a, 0x4010, (uint8_t)(0x80u | (loop << 6) | rate));
            nes_apu_write(a, 0x4012, (uint8_t)rnd());
            nes_apu_write(a, 0x4013, (uint8_t)(rnd() & 3u));
            nes_apu_write(a, 0x4015, 0x10);
        } else if (kind < 8) {
            static const uint8_t modes[6] = { 0x00, 0x00, 0x00, 0x00, 0x80, 0x40 };
            nes_apu_write(a, 0x4017, modes[rnd() % 6u]);
        } else if (kind < 20) {
            (void)nes_apu_read(a, 0x4015);
        } else if (kind < 23) {
            nes_apu_write(a, 0x4015, (uint8_t)(rnd() & 0x1Fu));
        } else if (kind < 25) {
            nes_apu_write(a, 0x4010, (uint8_t)rnd());
        } else if (kind < 30) {
            nes_apu_run(a, rnd() % 7u);
        }

        const uint32_t n = nes_apu_cycles_until_event(a);
        queries++;
        CHECK(n >= 1 && n <= NES_APU_EVENT_MAX, "horizon %u out of range", n);
        const bool irq = nes_apu_irq(a);
        const uint32_t fetch_seen = log.count;
        /* Nothing may happen in the first n - 1 cycles. A long wait is
         * mostly followed only part of the way, so the registers stay
         * busy. */
        uint32_t quiet_cycles = n - 1u;
        const bool whole = quiet_cycles <= 2000u || (quiet_cycles < 40000u && (rnd() & 3u) == 0);
        if (!whole) quiet_cycles = rnd() % 2000u;
        bool quiet = true;
        for (uint32_t i = 0; i < quiet_cycles && quiet; i++) {
            nes_apu_run(a, 1);
            quiet = nes_apu_irq(a) == irq && log.count == fetch_seen;
        }
        if (!quiet) {
            early++;
            if (early <= 5) printf("    event before the horizon of %u at cycle %llu\n", n, (unsigned long long)a->cycle);
        }
        if (whole && n < NES_APU_EVENT_MAX) {
            finite++;
            nes_apu_run(a, 1);
            if (nes_apu_irq(a) != irq || log.count != fetch_seen) exact++;
        }
        irq_edges += nes_apu_irq(a) != irq;
        fetches += log.count - fetch_seen;
        if (log.count > MAX_FETCHES - 64u) log.count = 0;
        (void)nes_apu_read_samples(a, g_capture, CAPTURE_MAX);
    }
    CHECK(early == 0, "%u of %u horizons were overestimates", early, queries);
    CHECK(queries > 5000 && irq_edges > 200 && fetches > 1000, "weak run: %u queries, %u IRQ edges, %u fetches", queries, irq_edges, fetches);
    /* Nearly every finite horizon ends in a real event (a frame IRQ put
     * off by a $4017 write, or a cleared flag that is set again, do not). */
    CHECK(exact * 10u > finite * 9u, "only %u of %u finite horizons were exact", exact, finite);
    printf("    %u queries, %u finite, %u exact, %u IRQ edges, %u fetches\n", queries, finite, exact, irq_edges, fetches);
    free(a);
}

/* ---- Reset -------------------------------------------------------------- */

static void test_reset(void)
{
    memset(g_mem, 0xFF, sizeof g_mem);
    for (unsigned mode = 0; mode < 2; mode++) {
        NesApu *a = new_apu();
        nes_apu_set_rate(a, 44100);
        nes_apu_set_mute(a, 1u << NES_APU_NOISE);
        run(a, 10);
        wr(a, 0x4015, 0x1F);
        wr(a, 0x4000, 0x7F);
        wr(a, 0x4002, 0x34);
        wr(a, 0x4008, 0xFF);
        wr(a, 0x400A, 0x40);
        wr(a, 0x400B, 0x08);
        wr(a, 0x400C, 0x3F);
        wr(a, 0x400F, 0x08);
        wr(a, 0x4010, 0x8F);
        wr(a, 0x4012, 0x33);
        wr(a, 0x4013, 0x00);
        wr(a, 0x4015, 0x1F);
        clock_frame(a);
        wr(a, 0x4017, 0x00);
        run(a, 31000); /* frame IRQ and DMC IRQ are both set */
        CHECK((peek(a) & 0xC0) == 0xC0 && nes_apu_irq(a), "setup: %02X", peek(a));
        wr(a, 0x4011, 0x55);
        wr(a, 0x4017, mode ? 0x80 : 0x00);
        run(a, 500 + (rnd() & 1u));
        wr(a, 0x4003, 0x09); /* envelope start flag */
        CHECK(a->tri.step != 0, "triangle did not move");
        const uint64_t T = a->cycle;
        const uint32_t samples = a->samples;

        nes_apu_reset(a);
        CHECK(peek(a) == 0x00 && !nes_apu_irq(a), "status after reset %02X", peek(a));
        CHECK(a->cycle == T && a->rate == 44100 && a->mute == (1u << NES_APU_NOISE) && a->samples == samples, "reset touched the clock or the output");
        CHECK(a->fc_mode == mode && a->fc_inhibit == 0, "frame counter mode %u", a->fc_mode);
        CHECK(a->tri.step == 0 && nes_apu_channel_level(a, NES_APU_TRIANGLE) == 15, "triangle phase");
        CHECK(a->dmc.level == 1, "DMC level %u", a->dmc.level);
        CHECK(a->pulse[0].period == 0x134 && a->dmc.sample_addr == 0xCCC0 && a->dmc.irq_enable == 1, "registers lost");
        CHECK(a->pulse[0].env.start == 1, "envelope start flag");

        /* $4017 is written again: the sequence restarts in the next step,
         * and 5-step mode clocks the units as it does. */
        const uint64_t R = T;
        run_to(a, R + 1);
        CHECK(a->fc_pos == 0, "sequence not restarted: position %u", a->fc_pos);
        CHECK(a->pulse[0].env.start == (mode ? 0 : 1), "mode %u: units %s at the restart", mode, mode ? "not clocked" : "clocked");

        /* Everything works at once. */
        wr(a, 0x4015, 0x01);
        wr(a, 0x4003, 0x08);
        CHECK(peek(a) == 0x01, "length load after reset: %02X", peek(a));
        if (mode == 0) {
            run_to(a, R + 29828);
            CHECK(!(peek(a) & 0x40), "frame IRQ early after reset");
            run(a, 1);
            CHECK((peek(a) & 0x40) && nes_apu_irq(a), "frame IRQ after reset");
        } else {
            run(a, 40000);
            CHECK(!(peek(a) & 0x40), "frame IRQ in 5-step mode after reset");
        }
        free(a);
    }

    /* The inhibit bit is part of what is written again. */
    NesApu *a = new_apu();
    run(a, 10);
    wr(a, 0x4017, 0x40);
    run(a, 100);
    nes_apu_reset(a);
    CHECK(a->fc_inhibit == 1 && a->fc_mode == 0, "inhibit bit lost");
    bool line = false;
    for (int i = 0; i < 31000; i++) {
        run(a, 1);
        line |= nes_apu_irq(a);
    }
    CHECK(!line, "IRQ after a reset with the inhibit bit set");
    free(a);
}

/* ---- Benchmark ---------------------------------------------------------- */

static void bench(void)
{
    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)(i * 31u);
    NesApu *a = new_apu();
    wr(a, 0x4015, 0x1F);
    wr(a, 0x4000, 0xBF);
    wr(a, 0x4002, 0xFD);
    wr(a, 0x4003, 0x08);
    wr(a, 0x4004, 0x7F);
    wr(a, 0x4006, 0x7C);
    wr(a, 0x4007, 0x09);
    wr(a, 0x4008, 0xFF);
    wr(a, 0x400A, 0xA8);
    wr(a, 0x400B, 0x08);
    wr(a, 0x400C, 0x3F);
    wr(a, 0x400E, 0x04);
    wr(a, 0x400F, 0x08);
    wr(a, 0x4010, 0x4F);
    wr(a, 0x4013, 0xFF);
    wr(a, 0x4015, 0x1F);
    const int seconds = 60;
    const clock_t t0 = clock();
    for (int s = 0; s < seconds; s++) {
        for (unsigned f = 0; f < 60; f++) {
            uint32_t left = 29830;
            while (left) {
                uint32_t n = nes_apu_cycles_until_event(a);
                if (n > left) n = left;
                nes_apu_run(a, n);
                left -= n;
            }
            (void)nes_apu_read_samples(a, g_capture, CAPTURE_MAX);
        }
    }
    const double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("bench: %d emulated seconds (all five channels) in %.3f s: %.1fx real time\n", seconds, dt, dt > 0 ? seconds / dt : 0.0);
    free(a);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} Test;

int main(int argc, char **argv)
{
    static const Test tests[] = {
        { "power_state", test_power_state },
        { "length_table_and_halt", test_length_table_and_halt },
        { "frame_4step_positions", test_frame_4step_positions },
        { "frame_5step_positions", test_frame_5step_positions },
        { "frame_write_twice", test_frame_write_twice },
        { "frame_irq_flag", test_frame_irq_flag },
        { "length_same_cycle_rules", test_length_same_cycle_rules },
        { "envelope", test_envelope },
        { "sweep", test_sweep },
        { "pulse_sequencer", test_pulse_sequencer },
        { "triangle", test_triangle },
        { "noise", test_noise },
        { "dmc_rates", test_dmc_rates },
        { "dmc_playback", test_dmc_playback },
        { "dmc_first_fetch", test_dmc_first_fetch },
        { "dmc_restart_before_reload", test_dmc_restart_before_reload },
        { "dmc_control", test_dmc_control },
        { "status_register", test_status },
        { "mixer_tables", test_mixer_tables },
        { "output_silence_and_dc", test_output_silence_and_dc },
        { "output_tone_and_mute", test_output_tone },
        { "output_aliasing", test_output_aliasing },
        { "output_rates", test_output_rates },
        { "output_ring", test_output_ring },
        { "chunked_runs", test_chunked_runs },
        { "golden_stream", test_golden_stream },
        { "event_horizon", test_event_horizon },
        { "reset", test_reset },
    };
    int failed = 0;
    const int count = (int)(sizeof tests / sizeof tests[0]);
    for (int i = 0; i < count; i++) {
        g_test_failed = 0;
        tests[i].fn();
        printf("%s %s\n", g_test_failed ? "[FAIL]" : "[ ok ]", tests[i].name);
        failed += g_test_failed;
    }
    printf("%d/%d tests passed, %d checks, %d failed checks\n", count - failed, count, g_checks, g_failed_checks);
    if (argc > 1 && !strcmp(argv[1], "--bench")) bench();
    /* --soak N: the two randomised tests again, N times, on new seeds. */
    if (argc > 2 && !strcmp(argv[1], "--soak")) {
        const int rounds = atoi(argv[2]);
        for (int i = 0; i < rounds; i++) {
            g_rng = 0x9E3779B9u * (uint32_t)(i + 1) + 1u;
            g_test_failed = 0;
            test_chunked_runs();
            test_event_horizon();
            failed += g_test_failed;
            printf("%s soak round %d\n", g_test_failed ? "[FAIL]" : "[ ok ]", i + 1);
        }
    }
    return failed || g_checks == 0 ? 1 : 0;
}
