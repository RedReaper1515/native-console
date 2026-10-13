/*
 * APU unit tests: boot ROM protocol, memory map, timers, S-DSP voice,
 * KOFF, echo buffer writes, port handshaking and determinism.
 *
 *   test_snes_apu [--bench]
 *
 * Every test is named and each check is counted; any failed check fails
 * the run (exit status 1). The S-CPU side is driven only through
 * snes_apu_cpu_write / snes_apu_cpu_read / snes_apu_run; internal state is
 * read (never written, except where a test says it loads a program
 * directly) to check results.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "snes_apu.h"

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

/* Garbage first: power must initialise every byte (the determinism test
 * powers instances over different garbage and compares them). */
static SnesApu *new_apu_over(uint8_t garbage)
{
    SnesApu *a = (SnesApu *)malloc(sizeof *a);
    if (!a) {
        printf("out of memory\n");
        exit(2);
    }
    memset(a, garbage, sizeof *a);
    snes_apu_power(a);
    return a;
}

static SnesApu *new_apu(void) { return new_apu_over(0xA7); }

/* The documented power-on contents of audio RAM. */
static uint8_t power_pattern(uint32_t addr) { return (addr & 0x20u) ? 0xFF : 0x00; }

/* ---- S-CPU side helpers ------------------------------------------------ */

/* Poll a port as the S-CPU would, running the APU in small steps. */
static bool wait_port(SnesApu *a, unsigned port, uint8_t value, uint32_t max_cycles)
{
    const uint64_t start = a->cycles;
    while (snes_apu_cpu_read(a, port) != value) {
        if (a->cycles - start > max_cycles) return false;
        snes_apu_run(a, 12);
    }
    return true;
}

typedef struct {
    uint16_t addr;
    const uint8_t *data;
    size_t len;
} Block;

/* The documented uploader (fullsnes "SNES APU Main CPU Communication
 * Port"), using only the port interface. */
static bool ipl_upload(SnesApu *a, const Block *blocks, int count, uint16_t entry)
{
    if (!wait_port(a, 0, 0xAA, 200000) || snes_apu_cpu_read(a, 1) != 0xBB) return false;
    uint8_t kick = 0xCC;
    for (int b = 0; b < count; b++) {
        snes_apu_cpu_write(a, 2, (uint8_t)(blocks[b].addr & 0xFFu));
        snes_apu_cpu_write(a, 3, (uint8_t)(blocks[b].addr >> 8));
        snes_apu_cpu_write(a, 1, 0x01);
        snes_apu_cpu_write(a, 0, kick);
        if (!wait_port(a, 0, kick, 20000)) return false;
        size_t i;
        for (i = 0; i < blocks[b].len; i++) {
            snes_apu_cpu_write(a, 1, blocks[b].data[i]);
            snes_apu_cpu_write(a, 0, (uint8_t)(i & 0xFFu));
            if (!wait_port(a, 0, (uint8_t)(i & 0xFFu), 20000)) return false;
        }
        kick = (uint8_t)(((i + 2u) & 0xFFu) | 1u);
    }
    snes_apu_cpu_write(a, 2, (uint8_t)(entry & 0xFFu));
    snes_apu_cpu_write(a, 3, (uint8_t)(entry >> 8));
    snes_apu_cpu_write(a, 1, 0x00);
    snes_apu_cpu_write(a, 0, kick);
    return wait_port(a, 0, kick, 20000);
}

/* Run whole instructions until PC reaches `pc`. */
static bool run_to_pc(SnesApu *a, uint16_t pc, uint32_t max_cycles)
{
    const uint64_t start = a->cycles;
    while (a->smp.pc != pc) {
        if (a->cycles - start > max_cycles) return false;
        snes_apu_run(a, 1);
    }
    return true;
}

/* Load a program directly into audio RAM and point the SPC700 at it
 * (tests that are not about the boot ROM). */
static void load_program(SnesApu *a, uint16_t addr, const uint8_t *code, size_t len)
{
    memcpy(&a->aram[addr], code, len);
    a->smp.pc = addr;
}

/* ---- Voice session: a program that writes DSP registers from a table -- */

/* One BRR block: 8 samples at +7, 8 at -7, shift 12, filter 0, end+loop:
 * a square wave with a period of 16 source samples. (Not -8: see
 * test_dsp_gauss_overflow.) */
static const uint8_t square_brr[9] = { 0xC3, 0x77, 0x77, 0x77, 0x77, 0x99, 0x99, 0x99, 0x99 };
/* Sixteen samples of -8 << 12, filter 0, end+loop. */
static const uint8_t min_brr[9] = { 0xC3, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 };
static const uint8_t dir_table[4] = { 0x00, 0x05, 0x00, 0x05 }; /* start = loop = $0500 */

/* At $0200: for each (reg, value) pair at $0300: $F2 = reg, $F3 = value. */
static void make_table_program(uint8_t prog[20], unsigned pairs)
{
    const uint8_t p[20] = {
        0xCD, 0x00,                          /* 0200 mov x,#0            */
        0xF5, 0x00, 0x03,                    /* 0202 mov a,!$0300+x      */
        0xC4, 0xF2,                          /* 0205 mov $F2,a           */
        0x3D,                                /* 0207 inc x               */
        0xF5, 0x00, 0x03,                    /* 0208 mov a,!$0300+x      */
        0xC4, 0xF3,                          /* 020B mov $F3,a           */
        0x3D,                                /* 020D inc x               */
        0xC8, (uint8_t)(pairs * 2u),         /* 020E cmp x,#2n           */
        0xD0, 0xF0,                          /* 0210 bne $0202           */
        0x2F, 0xFE,                          /* 0212 bra $0212           */
    };
    memcpy(prog, p, sizeof p);
}
#define TABLE_PROGRAM_IDLE 0x0212

typedef struct {
    bool echo;          /* voice 0 into the echo buffer at $6000 (EDL 2) */
    bool echo_write;    /* FLG bit 5 clear */
    bool noise;
    const uint8_t *brr; /* 9-byte block at $0500; NULL = square_brr */
} VoiceSetup;

static unsigned make_voice_table(uint8_t *t, const VoiceSetup *s)
{
    const uint8_t base[][2] = {
        { 0x5D, 0x04 },                               /* DIR $0400 */
        { 0x00, 0x7F }, { 0x01, 0x7F },               /* V0 volume */
        { 0x02, 0x00 }, { 0x03, 0x10 },               /* V0 pitch $1000 (32 kHz) */
        { 0x04, 0x00 },                               /* SRCN 0 */
        { 0x05, 0x00 }, { 0x07, 0x7F },               /* GAIN direct $7F */
        { 0x0C, 0x7F }, { 0x1C, 0x7F },               /* main volume */
        { 0x2C, s->echo ? 0x40 : 0x00 }, { 0x3C, s->echo ? 0x40 : 0x00 },
        { 0x0D, 0x00 },                               /* EFB 0 */
        { 0x0F, 0x7F },                               /* FIR0 */
        { 0x6D, 0x60 }, { 0x7D, 0x02 },               /* ESA $6000, EDL 2 (4 KiB) */
        { 0x4D, s->echo ? 0x01 : 0x00 },              /* EON */
        { 0x3D, s->noise ? 0x01 : 0x00 },             /* NON */
        { 0x5C, 0x00 },                               /* KOFF */
        { 0x6C, (uint8_t)(s->echo_write ? 0x00 : 0x20) }, /* FLG: run, unmuted */
        { 0x4C, 0x01 },                               /* KON voice 0 */
    };
    unsigned n = (unsigned)(sizeof base / sizeof base[0]);
    for (unsigned i = 0; i < n; i++) {
        t[i * 2] = base[i][0];
        t[i * 2 + 1] = base[i][1];
    }
    return n;
}

/* Boot, upload the table program, table, directory and sample through the
 * IPL protocol, and run until the table has been written. */
static bool start_voice_session(SnesApu *a, const VoiceSetup *s)
{
    uint8_t table[64], prog[20];
    unsigned pairs = make_voice_table(table, s);
    make_table_program(prog, pairs);
    const Block blocks[] = {
        { 0x0200, prog, sizeof prog },
        { 0x0300, table, pairs * 2u },
        { 0x0400, dir_table, sizeof dir_table },
        { 0x0500, s->brr ? s->brr : square_brr, sizeof square_brr },
    };
    if (!ipl_upload(a, blocks, 4, 0x0200)) return false;
    return run_to_pc(a, TABLE_PROGRAM_IDLE, 20000);
}

static size_t drain(SnesApu *a, int16_t *buf, size_t have, size_t cap)
{
    return have + snes_apu_read_samples(a, buf + have * 2u, cap - have);
}

/* ---- Tests ------------------------------------------------------------ */

static void test_boot_handshake(void)
{
    SnesApu *a = new_apu();
    CHECK(a->smp.pc == 0xFFC0, "reset vector points at $%04X, expected $FFC0", a->smp.pc);
    for (unsigned p = 0; p < 4; p++)
        CHECK(snes_apu_cpu_read(a, p) == 0x00, "port %u reads $%02X before the boot ROM runs", p, snes_apu_cpu_read(a, p));
    CHECK(wait_port(a, 0, 0xAA, 20000), "port 0 never read $AA");
    CHECK(snes_apu_cpu_read(a, 1) == 0xBB, "port 1 reads $%02X, expected $BB", snes_apu_cpu_read(a, 1));
    CHECK(snes_apu_cpu_read(a, 2) == 0x00 && snes_apu_cpu_read(a, 3) == 0x00, "ports 2/3 changed during boot");
    CHECK(a->smp.sp == 0xEF, "SP is $%02X after the handshake, expected $EF", a->smp.sp);
    /* Zero page cleared ($01-$EF) over the power-on pattern. */
    int dirty = 0;
    for (unsigned i = 0x01; i <= 0xEF; i++) dirty += a->aram[i] != 0;
    CHECK(dirty == 0, "%d zero-page bytes not cleared", dirty);
    CHECK(a->aram[0x20] == 0x00 && a->aram[0x0120] == 0xFF, "power-on RAM pattern not as documented");
    /* It keeps waiting for $CC: nothing changes without S-CPU input. */
    snes_apu_run(a, 100000);
    CHECK(snes_apu_cpu_read(a, 0) == 0xAA && snes_apu_cpu_read(a, 1) == 0xBB, "ports changed while waiting for $CC");
    free(a);
}

static void test_boot_upload_and_run(void)
{
    SnesApu *a = new_apu();
    /* Records the registers it was entered with, then signals on ports 2/3. */
    static const uint8_t prog[] = {
        0x0D,             /* 0200 push psw        */
        0xC4, 0x10,       /* 0201 mov $10,a       */
        0xD8, 0x11,       /* 0203 mov $11,x       */
        0xCB, 0x12,       /* 0205 mov $12,y       */
        0x9D,             /* 0207 mov x,sp        */
        0xD8, 0x13,       /* 0208 mov $13,x       */
        0x8F, 0x5A, 0xF6, /* 020A mov $F6,#$5A    */
        0x8F, 0xA5, 0xF7, /* 020D mov $F7,#$A5    */
        0x2F, 0xFE,       /* 0210 bra $0210       */
    };
    /* A second block longer than 256 bytes (index wrap). */
    static uint8_t data[300];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7u + 3u);
    const Block blocks[] = { { 0x0200, prog, sizeof prog }, { 0x1000, data, sizeof data } };
    CHECK(ipl_upload(a, blocks, 2, 0x0200), "upload protocol stalled");
    CHECK(memcmp(&a->aram[0x0200], prog, sizeof prog) == 0, "program block not stored at $0200");
    CHECK(memcmp(&a->aram[0x1000], data, sizeof data) == 0, "300-byte block not stored at $1000");
    CHECK(a->aram[0x0FFF] == power_pattern(0x0FFF) && a->aram[0x112C] == power_pattern(0x112C) &&
              a->aram[0x0212] == power_pattern(0x0212),
          "upload wrote outside the blocks");

    const uint64_t t0 = a->cycles;
    while (snes_apu_cpu_read(a, 3) != 0xA5 && a->cycles - t0 < 1000) snes_apu_run(a, 1);
    CHECK(snes_apu_cpu_read(a, 2) == 0x5A && snes_apu_cpu_read(a, 3) == 0xA5,
          "uploaded program did not run (ports 2/3 = $%02X/$%02X)", snes_apu_cpu_read(a, 2), snes_apu_cpu_read(a, 3));
    /* Documented state at the jump: A = X = Y = 0, SP = $EF, PSW = $02. */
    CHECK(a->aram[0x10] == 0 && a->aram[0x11] == 0 && a->aram[0x12] == 0, "A/X/Y at entry = $%02X/$%02X/$%02X",
          a->aram[0x10], a->aram[0x11], a->aram[0x12]);
    CHECK(a->aram[0x13] == 0xEE, "SP at entry = $%02X, expected $EF", (unsigned)(a->aram[0x13] + 1u));
    CHECK(a->aram[0x01EF] == 0x02, "PSW at entry = $%02X, expected $02", a->aram[0x01EF]);
    CHECK(a->aram[0x00] == 0x00 && a->aram[0x01] == 0x02, "[$0000] = $%02X%02X, expected the entry $0200",
          a->aram[0x01], a->aram[0x00]);
    CHECK(snes_apu_cpu_read(a, 1) == 0xBB, "port 1 = $%02X after the jump, expected $BB", snes_apu_cpu_read(a, 1));

    /* Jumping back to $FFC0 re-runs the boot protocol. */
    static const uint8_t reboot[] = { 0x8F, 0x80, 0xF1, 0x5F, 0xC0, 0xFF }; /* ROM on; jmp $FFC0 */
    load_program(a, 0x0300, reboot, sizeof reboot);
    snes_apu_cpu_write(a, 0, 0x00);
    CHECK(wait_port(a, 0, 0xAA, 20000) && snes_apu_cpu_read(a, 1) == 0xBB, "re-entry at $FFC0 did not handshake");
    free(a);
}

static void test_memory_map(void)
{
    SnesApu *a = new_apu();
    static const uint8_t prog[] = {
        0xE8, 0x12,       /* 0200 mov a,#$12            */
        0xC5, 0xC0, 0xFF, /* 0202 mov !$FFC0,a (to RAM)  */
        0xE9, 0xC0, 0xFF, /* 0205 mov x,!$FFC0 (ROM)     */
        0x8F, 0x00, 0xF1, /* 0208 mov $F1,#$00 (ROM off) */
        0xEC, 0xC0, 0xFF, /* 020B mov y,!$FFC0 (RAM)     */
        0x8F, 0x0C, 0xF2, /* 020E mov $F2,#$0C (MVOLL)   */
        0x8F, 0x55, 0xF3, /* 0211 mov $F3,#$55           */
        0x8F, 0x8C, 0xF2, /* 0214 mov $F2,#$8C (mirror)  */
        0x8F, 0x66, 0xF3, /* 0217 mov $F3,#$66 (ignored) */
        0xE4, 0xF3,       /* 021A mov a,$F3              */
        0x8F, 0x77, 0xF5, /* 021C mov $F5,#$77 (port 1)  */
        0x2F, 0xFE,       /* 021F bra $021F              */
    };
    load_program(a, 0x0200, prog, sizeof prog);
    CHECK(run_to_pc(a, 0x021F, 1000), "program did not finish");
    CHECK(a->smp.x == 0xCD, "read of $FFC0 with the ROM on gave $%02X, expected the boot ROM's $CD", a->smp.x);
    CHECK(a->smp.y == 0x12, "read of $FFC0 with the ROM off gave $%02X, expected the RAM's $12", a->smp.y);
    CHECK(a->aram[0xFFC0] == 0x12, "write to $FFC0 did not reach RAM");
    CHECK(a->dsp.regs[0x0C] == 0x55 && a->dsp.main_volume[0] == 0x55, "DSP write through $F2/$F3 failed");
    CHECK(a->smp.a == 0x55, "DSP read through the $8C mirror gave $%02X", a->smp.a);
    CHECK(snes_apu_cpu_read(a, 1) == 0x77, "SPC write to $F5 not visible on port 1");
    CHECK(a->aram[0xF5] == 0x77, "I/O write did not also reach RAM");
    free(a);
}

/* Record when a timer output changes while the SPC700 spins in a 4-cycle
 * loop; intervals must be exact. */
static void test_timer_rates(void)
{
    SnesApu *a = new_apu();
    static const uint8_t prog[] = {
        0x8F, 0x04, 0xFA, /* 0200 mov $FA,#4  : T0 8 kHz / 4 -> every 512 cycles  */
        0x8F, 0x08, 0xFB, /* 0203 mov $FB,#8  : T1 8 kHz / 8 -> every 1024 cycles */
        0x8F, 0x04, 0xFC, /* 0206 mov $FC,#4  : T2 64 kHz / 4 -> every 64 cycles  */
        0x8F, 0x07, 0xF1, /* 0209 mov $F1,#7  : enable all three                   */
        0x2F, 0xFE,       /* 020C bra $020C                                         */
    };
    load_program(a, 0x0200, prog, sizeof prog);
    CHECK(run_to_pc(a, 0x020C, 100), "timer program did not start");
    const uint32_t expect[3] = { 512, 1024, 64 };
    uint64_t last[3] = { 0, 0, 0 };
    int seen[3] = { 0, 0, 0 }, bad[3] = { 0, 0, 0 }, wraps = 0;
    uint8_t prev[3] = { a->timer[0].stage3, a->timer[1].stage3, a->timer[2].stage3 };
    for (int step = 0; step < 20000; step++) {
        snes_apu_run(a, 1);
        for (int t = 0; t < 3; t++) {
            uint8_t now = a->timer[t].stage3;
            if (now == prev[t]) continue;
            if (now != ((prev[t] + 1u) & 15u)) bad[t]++;
            if (now == 0) wraps += t == 2;
            if (seen[t] && a->cycles - last[t] != expect[t]) bad[t]++;
            last[t] = a->cycles;
            seen[t]++;
            prev[t] = now;
        }
    }
    for (int t = 0; t < 3; t++) {
        CHECK(seen[t] >= 10, "timer %d counted only %d times", t, seen[t]);
        CHECK(bad[t] == 0, "timer %d: %d increments off the %u-cycle period or not by one", t, bad[t], expect[t]);
    }
    CHECK(wraps >= 2, "timer 2 output never wrapped from 15 to 0");
    CHECK(a->timer[2].stage3 < 16, "timer output exceeds 4 bits");
    /* 8 kHz and 64 kHz sources, from the SMP clock. */
    CHECK(SNES_SMP_HZ / 128u == 8010u && SNES_SMP_HZ / 16u == 64080u, "timer source clocks");
    free(a);
}

static void test_timer_read_clears(void)
{
    SnesApu *a = new_apu();
    static const uint8_t prog[] = {
        0x8F, 0x00, 0xFA, /* 0200 mov $FA,#0 : T0 / 256 -> every 32768 cycles */
        0x8F, 0x01, 0xF1, /* 0203 mov $F1,#1                                   */
        0x2F, 0xFE,       /* 0206 bra $0206                                    */
    };
    static const uint8_t reader[] = {
        0xE4, 0xFD, /* 0300 mov a,$FD */
        0xF8, 0xFD, /* 0302 mov x,$FD */
        0x2F, 0xFE, /* 0304 bra $0304 */
    };
    load_program(a, 0x0200, prog, sizeof prog);
    CHECK(run_to_pc(a, 0x0206, 100), "program did not start");
    snes_apu_run(a, 3u * 32768u + 16384u);
    CHECK(a->timer[0].stage3 == 3, "timer 0 = %u after 3.5 periods, expected 3", a->timer[0].stage3);
    memcpy(&a->aram[0x0300], reader, sizeof reader);
    a->smp.pc = 0x0300;
    CHECK(run_to_pc(a, 0x0304, 100), "reader did not run");
    CHECK(a->smp.a == 3, "first read of T0OUT = %u, expected 3", a->smp.a);
    CHECK(a->smp.x == 0, "second read of T0OUT = %u, expected 0 (cleared on read)", a->smp.x);
    CHECK(a->timer[0].stage3 == 0, "T0OUT not cleared");
    /* A 0->1 enable resets the counter; writing 1 again does not. */
    static const uint8_t reenable[] = { 0x8F, 0x01, 0xF1, 0x2F, 0xFE }; /* 0400 mov $F1,#1; bra */
    snes_apu_run(a, 2u * 32768u);
    uint8_t before = a->timer[0].stage3;
    load_program(a, 0x0400, reenable, sizeof reenable);
    CHECK(run_to_pc(a, 0x0403, 100), "re-enable did not run");
    CHECK(before == 2 && a->timer[0].stage3 == 2, "writing an already-set enable bit changed T0OUT (%u -> %u)", before,
          a->timer[0].stage3);
    free(a);
}

/* Rising zero crossings of the left channel in [from, to). */
static int crossings(const int16_t *s, size_t from, size_t to, size_t *pos, int max)
{
    int n = 0;
    for (size_t i = from + 1; i < to && n < max; i++)
        if (s[(i - 1) * 2] < 0 && s[i * 2] >= 0) pos[n++] = i;
    return n;
}

static void test_dsp_voice_and_koff(void)
{
    SnesApu *a = new_apu();
    VoiceSetup vs = { false, false, false, NULL };
    CHECK(start_voice_session(a, &vs), "voice session did not start");
    enum { CAP = 4096 };
    int16_t *buf = (int16_t *)calloc(CAP * 2, sizeof *buf);
    size_t n = drain(a, buf, 0, CAP); /* discard boot-time frames */
    n = 0;
    for (int i = 0; i < 1200; i++) {
        snes_apu_run(a, 32);
        n = drain(a, buf, n, CAP);
    }
    CHECK(n >= 1199 && n <= 1201, "%zu frames for 1200 x 32 SMP cycles, expected 1200", n);
    int peak = 0;
    for (size_t i = 200; i < n; i++) {
        int v = buf[i * 2] < 0 ? -buf[i * 2] : buf[i * 2];
        if (v > peak) peak = v;
    }
    CHECK(peak > 20000, "voice output peak %d, expected a loud square wave", peak);
    size_t pos[128];
    int nc = crossings(buf, 200, n, pos, 128);
    int off_period = 0;
    for (int i = 1; i < nc; i++) off_period += pos[i] - pos[i - 1] != 16;
    CHECK(nc >= 50, "only %d rising zero crossings", nc);
    CHECK(off_period == 0, "%d periods differ from 16 samples (pitch $1000, 16-sample loop)", off_period);
    int lr = 0;
    for (size_t i = 200; i < n; i++) lr += buf[i * 2] != buf[i * 2 + 1];
    CHECK(lr == 0, "left and right differ with equal volumes");
    CHECK(a->dsp.regs[0x7C] & 1u, "ENDX bit 0 not set after the looping block's end flag");
    CHECK(a->dsp.regs[0x08] == 0x7F, "ENVX = $%02X, expected $7F (GAIN direct $7F)", a->dsp.regs[0x08]);

    /* KOFF: release by 8 per sample from $7F0 -> silent within 254 samples. */
    snes_dsp_write(&a->dsp, 0x5C, 0x01);
    n = 0;
    for (int i = 0; i < 600; i++) {
        snes_apu_run(a, 32);
        n = drain(a, buf, n, CAP);
    }
    int early = 0, late = 0, tail = 0;
    for (size_t i = 0; i < 32; i++) early = abs(buf[i * 2]) > early ? abs(buf[i * 2]) : early;
    for (size_t i = 150; i < 182; i++) late = abs(buf[i * 2]) > late ? abs(buf[i * 2]) : late;
    for (size_t i = 300; i < n; i++) tail += buf[i * 2] != 0 || buf[i * 2 + 1] != 0;
    CHECK(early > late && late > 0, "release not decaying (peaks %d then %d)", early, late);
    CHECK(tail == 0, "%d non-silent frames after the release finished", tail);
    CHECK(a->dsp.voice[0].envelope_mode == SNES_DSP_ENV_RELEASE && a->dsp.voice[0].envelope == 0,
          "voice not released (mode %u, envelope %u)", a->dsp.voice[0].envelope_mode, a->dsp.voice[0].envelope);
    CHECK(a->dsp.regs[0x08] == 0, "ENVX = $%02X after release", a->dsp.regs[0x08]);
    free(buf);
    free(a);
}

/* Hardware quirk (fullsnes, "4-Point Gaussian Interpolation"): the
 * table's four taps at i = 0 sum to 0x801, so three or more -8 << 12
 * samples with filter 0 interpolate to +3FF8h (15-bit) instead of -4000h.
 * At pitch $1000 the index is always 0. In 16-bit terms that is 0x7FF0;
 * envelope $7F0: 0x7FF0 * 0x7F0 >> 11 = 32496; voice volume $7F: 32242;
 * main volume $7F: 31990. */
static void test_dsp_gauss_overflow(void)
{
    SnesApu *a = new_apu();
    VoiceSetup vs = { false, false, false, min_brr };
    CHECK(start_voice_session(a, &vs), "voice session did not start");
    int16_t buf[2 * 512];
    snes_apu_run(a, 32u * 400u);
    while (snes_apu_read_samples(a, buf, 256)) {
    }
    snes_apu_run(a, 32u * 256u);
    size_t n = snes_apu_read_samples(a, buf, 512);
    int wrong = 0;
    for (size_t i = 0; i < n; i++) wrong += buf[i * 2] != 31990 || buf[i * 2 + 1] != 31990;
    CHECK(n == 256, "%zu frames", n);
    CHECK(wrong == 0, "%d frames differ from the documented overflow result 31990 (first: %d)", wrong, buf[0]);
    free(a);
}

static void test_dsp_noise(void)
{
    SnesApu *a = new_apu();
    VoiceSetup vs = { false, false, true, NULL };
    CHECK(start_voice_session(a, &vs), "voice session did not start");
    snes_dsp_write(&a->dsp, 0x6C, 0x1F); /* noise at 32 kHz */
    enum { CAP = 2048 };
    int16_t *buf = (int16_t *)calloc(CAP * 2, sizeof *buf);
    size_t n = snes_apu_read_samples(a, buf, CAP);
    n = 0;
    for (int i = 0; i < 1000; i++) {
        snes_apu_run(a, 32);
        n = drain(a, buf, n, CAP);
    }
    int distinct = 0;
    for (size_t i = 201; i < n; i++) distinct += buf[i * 2] != buf[(i - 1) * 2];
    CHECK(distinct > 500, "noise output changed only %d times in %zu frames", distinct, n - 200);
    CHECK(a->dsp.noise_lfsr != 0x4000, "noise LFSR never clocked");
    free(buf);
    free(a);
}

static void test_dsp_echo_writes(void)
{
    for (int enabled = 1; enabled >= 0; enabled--) {
        SnesApu *a = new_apu();
        memset(&a->aram[0x6000], 0x55, 0x2000); /* sentinel over and past the buffer */
        VoiceSetup vs = { true, enabled != 0, false, NULL };
        CHECK(start_voice_session(a, &vs), "voice session did not start");
        snes_apu_run(a, 32u * 1500u);
        int changed = 0, nonzero = 0, outside = 0;
        for (unsigned i = 0x6000; i < 0x7000; i++) {
            changed += a->aram[i] != 0x55;
            nonzero += a->aram[i] != 0x55 && a->aram[i] != 0x00;
        }
        for (unsigned i = 0x7000; i < 0x8000; i++) outside += a->aram[i] != 0x55;
        for (unsigned i = 0x5000; i < 0x6000; i++) outside += a->aram[i] != power_pattern(i);
        if (enabled) {
            CHECK(changed > 4000, "echo writes reached only %d of 4096 buffer bytes at ESA $6000 / EDL 2", changed);
            CHECK(nonzero > 1000, "echo buffer holds no voice data (%d non-zero bytes)", nonzero);
        } else {
            CHECK(changed == 0, "%d echo-buffer bytes written with FLG bit 5 set", changed);
        }
        CHECK(outside == 0, "%d bytes written outside the echo buffer", outside);
        free(a);
    }
}

static void test_handshaking(void)
{
    SnesApu *a = new_apu();
    /* Echoes port 0 + 1 on port 1 and also writes it to $F4 (its own port-0
     * output), which must not feed back into what it reads at $F4. */
    static const uint8_t prog[] = {
        0xE4, 0xF4, /* 0200 mov a,$F4 */
        0xBC,       /* 0202 inc a     */
        0xC4, 0xF5, /* 0203 mov $F5,a */
        0xC4, 0xF4, /* 0205 mov $F4,a */
        0x2F, 0xF7, /* 0207 bra $0200 */
    };
    const Block blocks[] = { { 0x0200, prog, sizeof prog } };
    CHECK(ipl_upload(a, blocks, 1, 0x0200), "upload stalled");
    snes_apu_cpu_write(a, 0, 0x10);

    /* Step instruction by instruction: the CPU-side ports may change only
     * on the instructions that store to $F5 and $F4. */
    int spurious = 0, changes = 0;
    for (int i = 0; i < 4000; i++) {
        uint8_t before[4], after[4];
        for (unsigned p = 0; p < 4; p++) before[p] = snes_apu_cpu_read(a, p);
        const uint16_t pc = a->smp.pc;
        snes_apu_run(a, 1);
        for (unsigned p = 0; p < 4; p++) after[p] = snes_apu_cpu_read(a, p);
        for (unsigned p = 0; p < 4; p++) {
            if (before[p] == after[p]) continue;
            changes++;
            const bool writer = (p == 1 && pc == 0x0203) || (p == 0 && pc == 0x0205);
            if (!writer) spurious++;
        }
        if (i == 2000) snes_apu_cpu_write(a, 0, 0x40); /* the S-CPU changes its value */
        /* The S-CPU's own writes never show up on its read side. */
        if (snes_apu_cpu_read(a, 2) != 0x00 || snes_apu_cpu_read(a, 3) != 0x00) spurious++;
    }
    CHECK(spurious == 0, "%d port changes not caused by an SPC700 write", spurious);
    CHECK(changes >= 2, "ports never changed");
    CHECK(snes_apu_cpu_read(a, 1) == 0x41, "port 1 = $%02X, expected $41 (last S-CPU value + 1)", snes_apu_cpu_read(a, 1));
    CHECK(a->cpu_to_apu[0] == 0x40, "SPC700 writes to $F4 changed its input latch ($%02X)", a->cpu_to_apu[0]);

    /* A CPU write is seen by the SPC700 only after snes_apu_cpu_write: with
     * no write, running changes nothing. */
    snes_apu_run(a, 5000);
    CHECK(snes_apu_cpu_read(a, 1) == 0x41, "port 1 drifted without S-CPU input");
    snes_apu_cpu_write(a, 0, 0x7E);
    CHECK(snes_apu_cpu_read(a, 1) == 0x41, "port 1 changed before the SPC700 ran");
    snes_apu_run(a, 40);
    CHECK(snes_apu_cpu_read(a, 1) == 0x7F, "port 1 = $%02X after the S-CPU wrote $7E", snes_apu_cpu_read(a, 1));

    /* CONTROL bits 4/5 clear the input latches. */
    static const uint8_t clr[] = { 0x8F, 0x30, 0xF1, 0x2F, 0xFE }; /* mov $F1,#$30; bra */
    snes_apu_cpu_write(a, 2, 0x99);
    load_program(a, 0x0300, clr, sizeof clr);
    CHECK(run_to_pc(a, 0x0303, 100), "clear program did not run");
    CHECK(a->cpu_to_apu[0] == 0 && a->cpu_to_apu[2] == 0, "CONTROL bits 4/5 did not clear the input ports");
    free(a);
}

/* Two instances fed the same input are byte-identical; a third, run in
 * small chunks with a debt counter like the machine's, matches too. */
static void test_determinism(void)
{
    SnesApu *a = new_apu_over(0xA7), *b = new_apu_over(0x5C), *c = new_apu_over(0x00);
    CHECK(memcmp(a, b, sizeof *a) == 0 && memcmp(a, c, sizeof *a) == 0,
          "power state depends on the memory's previous contents");
    VoiceSetup vs = { true, true, false, NULL };
    CHECK(start_voice_session(a, &vs) && start_voice_session(b, &vs) && start_voice_session(c, &vs),
          "voice session did not start");
    CHECK(memcmp(a, b, sizeof *a) == 0, "state differs after the same boot and upload");

    enum { CAP = 8192 };
    int16_t *sa = (int16_t *)calloc(CAP * 2, sizeof *sa), *sb = (int16_t *)calloc(CAP * 2, sizeof *sb),
            *sc = (int16_t *)calloc(CAP * 2, sizeof *sc);
    size_t na = 0, nb = 0, nc = 0;
    const uint32_t total = 32u * 6000u;
    /* a: one call per 1000 cycles; b: identical calls. */
    for (uint32_t done = 0; done < total; done += 1000) {
        snes_apu_run(a, 1000);
        snes_apu_run(b, 1000);
        na = drain(a, sa, na, CAP);
        nb = drain(b, sb, nb, CAP);
    }
    /* c: tiny, irregular chunks (1..39 cycles) up to the same cycle. A run
     * never splits an instruction, so c stops on the same boundaries. */
    {
        uint32_t step = 1;
        const uint64_t target = a->cycles;
        while (c->cycles < target) {
            uint64_t left = target - c->cycles;
            snes_apu_run(c, (uint32_t)(step < left ? step : left));
            nc = drain(c, sc, nc, CAP);
            step = step % 37u + 3u;
        }
    }
    CHECK(na == nb && na > 5900, "frame counts %zu / %zu", na, nb);
    CHECK(memcmp(sa, sb, na * 2 * sizeof *sa) == 0, "sample streams differ");
    CHECK(memcmp(a, b, sizeof *a) == 0, "final states differ");
    CHECK(c->cycles == a->cycles, "chunked run ended at cycle %llu, expected %llu", (unsigned long long)c->cycles,
          (unsigned long long)a->cycles);
    CHECK(nc == na && memcmp(sa, sc, na * 2 * sizeof *sa) == 0, "chunked run produced different samples");
    CHECK(memcmp(a, c, sizeof *a) == 0, "chunked run ended in a different state");
    int loud = 0;
    for (size_t i = 0; i < na; i++) loud += sa[i * 2] != 0;
    CHECK(loud > 1000, "determinism run was silent");
    free(sa);
    free(sb);
    free(sc);
    free(a);
    free(b);
    free(c);
}

static void test_run_contract(void)
{
    SnesApu *a = new_apu();
    CHECK(snes_apu_run(a, 0) == 0, "run(0) ran cycles");
    uint32_t worst = 0;
    for (uint32_t n = 1; n < 200; n++) {
        uint32_t ran = snes_apu_run(a, n);
        CHECK(ran >= n, "run(%u) returned %u", n, ran);
        if (ran - n > worst) worst = ran - n;
    }
    CHECK(worst < 12, "overshoot %u cycles (an instruction is at most 12)", worst);
    /* 32 SMP cycles per output frame. */
    int16_t tmp[64];
    while (snes_apu_read_samples(a, tmp, 32)) {
    }
    snes_apu_run(a, 32u * 1000u);
    size_t frames = 0, got;
    while ((got = snes_apu_read_samples(a, tmp, 32)) != 0) frames += got;
    CHECK(frames >= 999 && frames <= 1001, "%zu frames for 32000 cycles", frames);
    /* Reset keeps audio RAM and restarts the boot ROM. */
    a->aram[0x8000] = 0x5E;
    snes_apu_reset(a);
    CHECK(a->aram[0x8000] == 0x5E, "reset cleared audio RAM");
    CHECK(a->smp.pc == 0xFFC0 && snes_apu_cpu_read(a, 0) == 0, "reset state");
    CHECK(wait_port(a, 0, 0xAA, 20000), "no handshake after reset");
    CHECK(snes_apu_ram(a) == a->aram, "snes_apu_ram");

    /* SLEEP halts the SPC700 for good, but time, the DSP and the timers
     * keep running. */
    static const uint8_t sleeper[] = { 0x8F, 0x01, 0xFC, 0x8F, 0x04, 0xF1, 0xEF }; /* T2 /1 on; SLEEP */
    int t2_changes = 0;
    load_program(a, 0x0200, sleeper, sizeof sleeper);
    snes_apu_run(a, 100);
    while (snes_apu_read_samples(a, tmp, 32)) {
    }
    const uint64_t c0 = a->cycles;
    uint32_t ran = 0;
    for (int i = 0; i < 100; i++) {
        uint8_t t2 = a->timer[2].stage3;
        ran += snes_apu_run(a, 32);
        t2_changes += a->timer[2].stage3 != t2;
    }
    CHECK(t2_changes == 100, "timer 2 stalled while halted (%d of 100 intervals)", t2_changes);
    frames = 0;
    while ((got = snes_apu_read_samples(a, tmp, 32)) != 0) frames += got;
    CHECK(a->smp.halt == SNES_SPC700_SLEEP, "SLEEP did not halt the core");
    CHECK(ran >= 3200 && ran <= 3202 && a->cycles - c0 == ran, "halted run(3200) ran %u cycles", ran);
    CHECK(frames >= 99 && frames <= 101, "%zu frames while halted", frames);
    CHECK(a->smp.pc == 0x0207, "PC moved while halted ($%04X)", a->smp.pc);
    free(a);
}

static void bench(void)
{
    SnesApu *a = new_apu();
    VoiceSetup vs = { true, true, false, NULL };
    if (!start_voice_session(a, &vs)) {
        printf("bench: session did not start\n");
        free(a);
        return;
    }
    int16_t tmp[2048];
    const int seconds = 20;
    clock_t t0 = clock();
    for (int s = 0; s < seconds * 100; s++) {
        snes_apu_run(a, SNES_SMP_HZ / 100u);
        while (snes_apu_read_samples(a, tmp, 1024)) {
        }
    }
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("bench: %d emulated seconds (SPC700 idle loop, voice + echo) in %.3f s: %.1fx real time\n", seconds, dt,
           dt > 0 ? seconds / dt : 0.0);
    free(a);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} Test;

int main(int argc, char **argv)
{
    static const Test tests[] = {
        { "boot_handshake", test_boot_handshake },
        { "boot_upload_and_run", test_boot_upload_and_run },
        { "memory_map", test_memory_map },
        { "timer_rates", test_timer_rates },
        { "timer_read_clears", test_timer_read_clears },
        { "dsp_voice_period_and_koff", test_dsp_voice_and_koff },
        { "dsp_gauss_overflow", test_dsp_gauss_overflow },
        { "dsp_noise", test_dsp_noise },
        { "dsp_echo_writes", test_dsp_echo_writes },
        { "apu_handshaking", test_handshaking },
        { "determinism", test_determinism },
        { "run_contract", test_run_contract },
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
    return failed || g_checks == 0 ? 1 : 0;
}
