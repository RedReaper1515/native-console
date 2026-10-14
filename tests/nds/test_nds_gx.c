/*
 * Directed tests for the DS geometry engine (runtimes/runtime-nds/src/nds_gx.c): the
 * command FIFO and its timing, the matrix commands and stacks, lighting,
 * texture coordinate generation, the tests, and polygon set-up (culling,
 * clipping, the viewport, the RAM limits).
 *
 * Expected values are worked out here from the documentation, never taken
 * from the module. Each check says where its expectation comes from:
 *
 *   GBATEK    problemkaputt.de/gbatek.htm, the "DS 3D" chapters. Timings are
 *             the "Cy." column of its geometry command table.
 *   melonDS   behaviour GBATEK leaves open and melonDS's authors found on
 *             hardware (bit widths in lighting, the texture matrix's
 *             scale, strip vertex sharing). No hardware capture backs these
 *             checks: they pin the model.
 *   polyrastertest   the vertex limit scenes of Jaklyy/polyrastertest's
 *             later tests.h, whose comments say what hardware draws.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nds_gx.h"

static NdsGx gx;
static int g_checks, g_failures;
static const char *g_section = "";

static void check(const char *name, long long got, long long want, int line)
{
    g_checks++;
    if (got == want) return;
    g_failures++;
    printf("FAIL %s: %s (line %d): got %lld (0x%llX), want %lld (0x%llX)\n", g_section, name, line, got,
           (unsigned long long)got, want, (unsigned long long)want);
}

#define EQ(name, got, want) check(name, (long long)(got), (long long)(want), __LINE__)
#define OK(name, condition) check(name, (condition) ? 1 : 0, 1, __LINE__)

/* ------------------------------------------------------------------ */
/* Access                                                              */
/* ------------------------------------------------------------------ */

#define GXFIFO 0x04000400u
#define GXSTAT 0x04000600u
#define RAM_COUNT 0x04000604u

/* Command ports, by command code. */
#define PORT(code) (0x04000400u + 4u * (code))
enum {
    MTX_MODE = 0x10, MTX_PUSH, MTX_POP, MTX_STORE, MTX_RESTORE, MTX_IDENTITY, MTX_LOAD_4x4, MTX_LOAD_4x3,
    MTX_MULT_4x4, MTX_MULT_4x3, MTX_MULT_3x3, MTX_SCALE, MTX_TRANS,
    COLOR = 0x20, NORMAL, TEXCOORD, VTX_16, VTX_10, VTX_XY, VTX_XZ, VTX_YZ, VTX_DIFF, POLYGON_ATTR,
    TEXIMAGE_PARAM, PLTT_BASE,
    DIF_AMB = 0x30, SPE_EMI, LIGHT_VECTOR, LIGHT_COLOR, SHININESS,
    BEGIN_VTXS = 0x40, END_VTXS,
    SWAP_BUFFERS = 0x50, VIEWPORT = 0x60, BOX_TEST = 0x70, POS_TEST, VEC_TEST
};

#define ONE 0x1000 /* 1.0 in 20.12 and 4.12 */

static uint32_t r32(uint32_t addr)
{
    return nds_gx_read(&gx, addr, 4);
}

static void w32(uint32_t addr, uint32_t value)
{
    nds_gx_write(&gx, addr, value, 4);
}

static void run(uint32_t cycles)
{
    nds_gx_run(&gx, cycles);
}

/* Let the engine finish everything it has (it stops at a SWAP_BUFFERS). */
static void drain(void)
{
    uint32_t cycles;
    while ((cycles = nds_gx_cycles_until_event(&gx)) != 0) run(cycles);
}

/* Send one FIFO entry through a command port and let it run. */
static void cmd(unsigned code, uint32_t param)
{
    w32(PORT(code), param);
    drain();
}

static void cmdn(unsigned code, const int32_t *params, int count)
{
    for (int i = 0; i < count; i++) w32(PORT(code), (uint32_t)params[i]);
    drain();
}

static void vblank(void)
{
    nds_gx_vblank(&gx);
    drain();
}

static void fresh(void)
{
    nds_gx_reset(&gx);
}

static int32_t clip_matrix(int index)
{
    return (int32_t)r32(0x04000640u + 4u * (uint32_t)index);
}

static int32_t vector_matrix(int index) /* 3x3, row by row */
{
    return (int32_t)r32(0x04000680u + 4u * (uint32_t)index);
}

static unsigned polygons(void)
{
    return r32(RAM_COUNT) & 0xFFFu;
}

static unsigned vertices(void)
{
    return (r32(RAM_COUNT) >> 16) & 0x1FFFu;
}

static void vtx16(int x, int y, int z)
{
    w32(PORT(VTX_16), ((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
    w32(PORT(VTX_16), (uint16_t)z);
    drain();
}

/* The polygon and vertex the geometry engine stored last. */
static const NdsGxPolygon *last_polygon(void)
{
    return &gx.polygons[gx.geometry_bank][gx.polygon_count - 1];
}

static const NdsGxVertex *polygon_vertex(const NdsGxPolygon *p, int index)
{
    return &gx.vertices[gx.geometry_bank][p->vertex[index]];
}

#define CULL_NONE (3u << 6) /* POLYGON_ATTR: draw both faces */
#define OPAQUE (31u << 16)

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static void test_registers(void)
{
    g_section = "registers";
    fresh();

    /* GBATEK GXSTAT: after reset the FIFO is empty (bit 26), less than half
     * full (bit 25), nothing is busy and the stack pointers are zero. */
    EQ("GXSTAT at reset", r32(GXSTAT), 0x06000000u);
    EQ("RAM_COUNT at reset", r32(RAM_COUNT), 0);

    /* DISP3DCNT: bits 0-11 and 14 are read/write; 12 and 13 are flags the
     * engine sets, which writing 1 acknowledges; 15-31 are not used. */
    w32(0x04000060, 0xFFFFFFFFu);
    EQ("DISP3DCNT all ones", r32(0x04000060), 0x4FFFu);
    w32(0x04000060, 0);
    EQ("DISP3DCNT zero", r32(0x04000060), 0);
    for (int bit = 0; bit < 16; bit++) {
        uint32_t want = (bit <= 11 || bit == 14) ? 1u << bit : 0;
        nds_gx_write(&gx, 0x04000060, 1u << bit, 2);
        EQ("DISP3DCNT single bit", nds_gx_read(&gx, 0x04000060, 2), want);
    }
    /* Byte access. */
    nds_gx_write(&gx, 0x04000060, 0, 2);
    nds_gx_write(&gx, 0x04000061, 0x4A, 1);
    EQ("DISP3DCNT high byte", nds_gx_read(&gx, 0x04000060, 2), 0x4A00u);
    EQ("DISP3DCNT byte read", nds_gx_read(&gx, 0x04000061, 1), 0x4Au);
    nds_gx_write(&gx, 0x04000060, 0xA5, 1);
    EQ("DISP3DCNT low byte", nds_gx_read(&gx, 0x04000060, 4), 0x4AA5u);

    /* GXSTAT: "Bit 30-31 are R/W"; the rest cannot be written. */
    for (uint32_t mode = 0; mode < 4; mode++) {
        w32(GXSTAT, mode << 30);
        EQ("GXSTAT IRQ mode", r32(GXSTAT) >> 30, mode);
    }
    w32(GXSTAT, 0x3FFF7FFFu);
    EQ("GXSTAT read-only bits", r32(GXSTAT), 0x06000000u);
    nds_gx_write(&gx, GXSTAT + 3, 0x80, 1);
    EQ("GXSTAT IRQ mode by byte", r32(GXSTAT) >> 30, 2);
    EQ("GXSTAT high half", nds_gx_read(&gx, GXSTAT + 2, 2), 0x8600u);
    w32(GXSTAT, 0);

    /* The rendering engine's registers are write-only. */
    w32(0x04000350, 0x12345678u);
    EQ("CLEAR_COLOR reads zero", r32(0x04000350), 0);
    w32(0x04000380, 0x7FFF7FFFu);
    EQ("TOON_TABLE reads zero", r32(0x04000380), 0);
    EQ("GXFIFO reads zero", r32(GXFIFO), 0);
    /* RDLINES_COUNT: "Minimum Number (minus 2) of buffered lines in
     * previous frame (0..46)"; a renderer that never falls behind has 46. */
    EQ("RDLINES_COUNT", r32(0x04000320) & 0x3Fu, 46);

    /* Any size at any offset: a result register read in pieces. */
    fresh();
    {
        static const int32_t m[16] = {0x11223344, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE};
        cmd(MTX_MODE, 0);
        cmdn(MTX_LOAD_4x4, m, 16);
        EQ("CLIPMTX byte 0", nds_gx_read(&gx, 0x04000640, 1), 0x44);
        EQ("CLIPMTX byte 3", nds_gx_read(&gx, 0x04000643, 1), 0x11);
        EQ("CLIPMTX middle half", nds_gx_read(&gx, 0x04000641, 2), 0x2233);
        EQ("CLIPMTX across words", nds_gx_read(&gx, 0x04000643, 2), 0x0011);
    }
}

/* ------------------------------------------------------------------ */
/* FIFO                                                                */
/* ------------------------------------------------------------------ */

static unsigned fifo_level(void)
{
    return (r32(GXSTAT) >> 16) & 0x1FFu;
}

static void test_fifo(void)
{
    g_section = "fifo";
    fresh();

    /* SWAP_BUFFERS halts the engine until VBlank ("the Geometry Engine is
     * halted for that duration"), so what is written after it stays put.
     * GBATEK "FIFO / PIPE Number of Entries": the first four entries go to
     * the PIPE, which GXSTAT's count leaves out; the FIFO then takes 256. */
    w32(PORT(SWAP_BUFFERS), 0);
    OK("busy while waiting for VBlank", r32(GXSTAT) & (1u << 27));
    EQ("the swap itself was taken", fifo_level(), 0);
    for (int i = 0; i < 4; i++) w32(PORT(MTX_MODE), 0);
    EQ("four entries: all in the PIPE", fifo_level(), 0);
    OK("FIFO empty flag", r32(GXSTAT) & (1u << 26));
    w32(PORT(MTX_MODE), 0);
    EQ("fifth entry: FIFO has one", fifo_level(), 1);
    OK("not empty", !(r32(GXSTAT) & (1u << 26)));
    OK("less than half full", r32(GXSTAT) & (1u << 25));
    for (int i = 0; i < 126; i++) w32(PORT(MTX_MODE), 0);
    EQ("127 entries", fifo_level(), 127);
    OK("127: less than half full", r32(GXSTAT) & (1u << 25));
    OK("127: DMA may send", nds_gx_dma_request(&gx));
    w32(PORT(MTX_MODE), 0);
    EQ("128 entries", fifo_level(), 128);
    OK("128: not less than half full", !(r32(GXSTAT) & (1u << 25)));
    OK("128: no DMA request", !nds_gx_dma_request(&gx));
    OK("128: not full", !nds_gx_fifo_full(&gx));
    for (int i = 0; i < 127; i++) w32(PORT(MTX_MODE), 0);
    EQ("255 entries", fifo_level(), 255);
    OK("255: not full", !nds_gx_fifo_full(&gx));
    w32(PORT(MTX_MODE), 0);
    EQ("256 entries", fifo_level(), 256);
    OK("256: the full bit (24)", r32(GXSTAT) & (1u << 24));
    OK("256: full", nds_gx_fifo_full(&gx));

    /* A write into a full FIFO: on hardware the CPU is held. Here the entry
     * is kept and the FIFO goes on reporting full until it has room. */
    w32(PORT(MTX_MODE), 1);
    EQ("257th entry: FIFO still 256", fifo_level(), 256);
    OK("257th entry: still full", nds_gx_fifo_full(&gx));
    EQ("nothing dropped", gx.dropped, 0);

    /* Time alone changes nothing before VBlank. */
    run(100000);
    EQ("halted: FIFO unchanged", fifo_level(), 256);
    EQ("halted: no event pending", nds_gx_cycles_until_event(&gx), 0);

    /* VBlank: the swap runs, 392 cycles; then entries are taken again.
     * "If the PIPE runs half empty (less than 3 entries) then 2 entries are
     * moved from the FIFO to the PIPE": taking the first leaves 3, the
     * second 2, and then two move up (and the held entry moves in). */
    nds_gx_vblank(&gx);
    EQ("after VBlank: 392 cycles to the next entry", nds_gx_cycles_until_event(&gx), 392);
    run(391);
    EQ("swap still running", fifo_level(), 256);
    run(1);
    EQ("first entry taken from the PIPE", fifo_level(), 256);
    OK("held entry still held", nds_gx_fifo_full(&gx));
    run(1);
    EQ("second taken, two refilled, the held one in", fifo_level(), 255);
    OK("no longer full", !nds_gx_fifo_full(&gx));
    run(1);
    EQ("third taken", fifo_level(), 255);
    run(1);
    EQ("fourth taken, two refilled", fifo_level(), 253);
    drain();
    EQ("all done", fifo_level(), 0);
    EQ("idle", r32(GXSTAT) & (1u << 27), 0);
    /* The last entry written was MTX_MODE 1: entries ran in order. */
    EQ("ran in order", gx.matrix_mode, 1);

    /* What nds_gx.h promises a machine that waits for a level: a FIFO of L
     * entries cannot hold fewer than N until nds_gx_cycles_until_event()
     * plus L - N - 1 cycles have passed. It follows from the two rules above
     * (the engine takes one entry at a time, and two move up for every other
     * one taken) and from no entry taking less than a cycle: SHININESS, 32
     * parameters in 32 cycles, and the one-cycle commands are the fastest
     * there are. Checked every cycle of a full FIFO draining, against half
     * full (the DMA's and interrupt mode 1's level) and empty (mode 2's). */
    fresh();
    {
        static const struct {
            uint8_t code, entries, times;
        } stream[] = {
            {SHININESS, 32, 1}, {MTX_MODE, 1, 20}, {VTX_16, 2, 10}, {MTX_TRANS, 3, 5}, {COLOR, 1, 10},
            {SHININESS, 32, 1}, {MTX_MODE, 1, 20}, {VTX_16, 2, 10}, {MTX_TRANS, 3, 5}, {COLOR, 1, 10},
            {SHININESS, 32, 1}, {MTX_MODE, 1, 20}, {COLOR, 1, 15},
        };
        unsigned written = 0;
        for (size_t i = 0; i < sizeof stream / sizeof stream[0]; i++) {
            for (unsigned k = 0; k < (unsigned)stream[i].entries * stream[i].times; k++) {
                w32(PORT(stream[i].code), 0);
                written++;
            }
        }
        /* The engine took the first at once: one running, four in the PIPE. */
        EQ("the stream is one entry, a PIPE and a FIFO", written, 1 + 4 + 256);
        EQ("FIFO full", fifo_level(), 256);

        uint64_t now = 0, half_until = 0, some_until = 0;
        unsigned early = 0;
        for (;;) {
            unsigned level = fifo_level();
            uint32_t busy = nds_gx_cycles_until_event(&gx);
            if (busy == 0) break;
            if (level >= 128) {
                uint64_t until = now + busy + (level > 129 ? level - 129 : 0);
                if (until > half_until) half_until = until;
            }
            if (level >= 1) {
                uint64_t until = now + busy + (level > 2 ? level - 2 : 0);
                if (until > some_until) some_until = until;
            }
            run(1);
            now++;
            level = fifo_level();
            if ((now < half_until && level < 128) || (now < some_until && level < 1)) early++;
        }
        EQ("no level reached before the promised time", early, 0);
        EQ("and the FIFO drained", fifo_level(), 0);
        /* 3 SHININESS, 60 MTX_MODE, 20 VTX_16, 10 MTX_TRANS, 35 COLOR. */
        EQ("in the commands' own time", now, 3 * 32 + 60 * 1 + 20 * 9 + 10 * 22 + 35 * 1);
    }
}

static void test_irq(void)
{
    g_section = "irq";
    fresh();

    /* GXSTAT bits 30-31: "Command FIFO IRQ (0=Never, 1=Less than half
     * full, 2=Empty, 3=Reserved)"; "the IRQ flag is set while and as long as
     * the IRQ condition is true". */
    OK("mode 0, empty: no request", !nds_gx_irq(&gx));
    w32(GXSTAT, 1u << 30);
    OK("mode 1, empty: request", nds_gx_irq(&gx));
    w32(GXSTAT, 2u << 30);
    OK("mode 2, empty: request", nds_gx_irq(&gx));
    w32(GXSTAT, 3u << 30);
    OK("mode 3: no request", !nds_gx_irq(&gx));

    w32(PORT(SWAP_BUFFERS), 0); /* hold the engine */
    for (int i = 0; i < 4 + 1; i++) w32(PORT(MTX_MODE), 0);
    w32(GXSTAT, 2u << 30);
    OK("mode 2, one entry: no request", !nds_gx_irq(&gx));
    w32(GXSTAT, 1u << 30);
    OK("mode 1, one entry: request", nds_gx_irq(&gx));
    for (int i = 0; i < 127; i++) w32(PORT(MTX_MODE), 0);
    OK("mode 1, 128 entries: no request", !nds_gx_irq(&gx));
    w32(GXSTAT, 0);
    OK("mode 0, 128 entries: no request", !nds_gx_irq(&gx));

    /* The request comes back as the FIFO drains. */
    w32(GXSTAT, 1u << 30);
    nds_gx_vblank(&gx);
    run(392 + 2);
    OK("mode 1: below half again", nds_gx_irq(&gx));
    EQ("at 126 entries", fifo_level(), 126);
    w32(GXSTAT, 2u << 30);
    OK("mode 2: not yet", !nds_gx_irq(&gx));
    drain();
    OK("mode 2: empty", nds_gx_irq(&gx));
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

/* Send a command's parameters to an idle engine and count the cycles until
 * it is idle again. */
static uint32_t time_command(unsigned code, int params)
{
    uint32_t cycles = 0;
    for (int i = 0; i < (params ? params : 1); i++) w32(PORT(code), 0);
    while (r32(GXSTAT) & (1u << 27)) {
        if (cycles > 100000) return cycles;
        run(1);
        cycles++;
    }
    return cycles;
}

static void test_timing(void)
{
    /* GBATEK "DS 3D I/O Map": command, parameters, cycles. */
    static const struct {
        unsigned code;
        int params;
        uint32_t cycles;
        const char *name;
    } table[] = {
        {MTX_MODE, 1, 1, "MTX_MODE"},         {MTX_PUSH, 0, 17, "MTX_PUSH"},
        {MTX_POP, 1, 36, "MTX_POP"},          {MTX_STORE, 1, 17, "MTX_STORE"},
        {MTX_RESTORE, 1, 36, "MTX_RESTORE"},  {MTX_IDENTITY, 0, 19, "MTX_IDENTITY"},
        {MTX_LOAD_4x4, 16, 34, "MTX_LOAD_4x4"}, {MTX_LOAD_4x3, 12, 30, "MTX_LOAD_4x3"},
        {MTX_MULT_4x4, 16, 35, "MTX_MULT_4x4"}, {MTX_MULT_4x3, 12, 31, "MTX_MULT_4x3"},
        {MTX_MULT_3x3, 9, 28, "MTX_MULT_3x3"}, {MTX_SCALE, 3, 22, "MTX_SCALE"},
        {MTX_TRANS, 3, 22, "MTX_TRANS"},      {COLOR, 1, 1, "COLOR"},
        {NORMAL, 1, 9, "NORMAL"},             {TEXCOORD, 1, 1, "TEXCOORD"},
        {VTX_16, 2, 9, "VTX_16"},             {VTX_10, 1, 8, "VTX_10"},
        {VTX_XY, 1, 8, "VTX_XY"},             {VTX_XZ, 1, 8, "VTX_XZ"},
        {VTX_YZ, 1, 8, "VTX_YZ"},             {VTX_DIFF, 1, 8, "VTX_DIFF"},
        {POLYGON_ATTR, 1, 1, "POLYGON_ATTR"}, {TEXIMAGE_PARAM, 1, 1, "TEXIMAGE_PARAM"},
        {PLTT_BASE, 1, 1, "PLTT_BASE"},       {DIF_AMB, 1, 4, "DIF_AMB"},
        {SPE_EMI, 1, 4, "SPE_EMI"},           {LIGHT_VECTOR, 1, 6, "LIGHT_VECTOR"},
        {LIGHT_COLOR, 1, 1, "LIGHT_COLOR"},   {SHININESS, 32, 32, "SHININESS"},
        {BEGIN_VTXS, 1, 1, "BEGIN_VTXS"},     {END_VTXS, 0, 1, "END_VTXS"},
        {VIEWPORT, 1, 1, "VIEWPORT"},         {BOX_TEST, 3, 103, "BOX_TEST"},
        {POS_TEST, 2, 9, "POS_TEST"},         {VEC_TEST, 1, 5, "VEC_TEST"},
    };
    g_section = "timing";
    fresh();
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) EQ(table[i].name, time_command(table[i].code, table[i].params), table[i].cycles);

    /* "In MTX_MODE=2 (Simultanous Set), MTX_MULT/TRANS take additional 30
     * cycles." Scale does not touch the vector matrix and takes its usual
     * time. */
    cmd(MTX_MODE, 2);
    EQ("MTX_MULT_4x4, mode 2", time_command(MTX_MULT_4x4, 16), 65);
    EQ("MTX_MULT_4x3, mode 2", time_command(MTX_MULT_4x3, 12), 61);
    EQ("MTX_MULT_3x3, mode 2", time_command(MTX_MULT_3x3, 9), 58);
    EQ("MTX_TRANS, mode 2", time_command(MTX_TRANS, 3), 52);
    EQ("MTX_SCALE, mode 2", time_command(MTX_SCALE, 3), 22);
    EQ("MTX_LOAD_4x4, mode 2", time_command(MTX_LOAD_4x4, 16), 34);

    /* "NORMAL commands takes 9..12 cycles, depending on the number of
     * enabled lights": four timings for the five counts 0-4, so none and
     * one share the first (melonDS). The enables are POLYGON_ATTR bits 0-3
     * as of the last BEGIN_VTXS. */
    static const uint32_t normal_cycles[5] = {9, 9, 10, 11, 12};
    for (unsigned lights = 0; lights <= 4; lights++) {
        cmd(POLYGON_ATTR, (1u << lights) - 1u);
        cmd(BEGIN_VTXS, 0);
        EQ("NORMAL by lights", time_command(NORMAL, 1), normal_cycles[lights]);
    }

    /* Undefined commands take no parameters and a cycle; so does a NOP. */
    EQ("undefined command 0x1F", time_command(0x1F, 0), 1);

    /* A command's time starts when its last parameter arrives: each entry
     * takes a cycle to read. */
    fresh();
    w32(PORT(MTX_LOAD_4x4), 0);
    EQ("first parameter read: one cycle", nds_gx_cycles_until_event(&gx), 1);
    run(1);
    EQ("waiting for parameters: idle", r32(GXSTAT) & (1u << 27), 0);
    for (int i = 0; i < 14; i++) w32(PORT(MTX_LOAD_4x4), 0), run(1);
    w32(PORT(MTX_LOAD_4x4), 0);
    EQ("sixteenth parameter: the rest of 34", nds_gx_cycles_until_event(&gx), 34 - 15);
    drain();

    /* "Total execution time of SwapBuffers is Duration until VBlank, plus
     * 392 cycles." */
    w32(PORT(SWAP_BUFFERS), 0);
    run(50000);
    OK("SWAP_BUFFERS: busy until VBlank", r32(GXSTAT) & (1u << 27));
    nds_gx_vblank(&gx);
    run(391);
    OK("SWAP_BUFFERS: busy 391 cycles after", r32(GXSTAT) & (1u << 27));
    run(1);
    OK("SWAP_BUFFERS: done at 392", !(r32(GXSTAT) & (1u << 27)));

    /* GXSTAT bit 14: "Matrix Stack Busy ... Currently executing a Push/Pop
     * command", and bit 0 for the tests; both from the moment the command is
     * written. */
    w32(PORT(MTX_PUSH), 0);
    OK("stack busy during MTX_PUSH", r32(GXSTAT) & (1u << 14));
    run(17);
    OK("stack idle after", !(r32(GXSTAT) & (1u << 14)));
    w32(PORT(VEC_TEST), 0);
    OK("test busy during VEC_TEST", r32(GXSTAT) & 1u);
    run(5);
    OK("test idle after", !(r32(GXSTAT) & 1u));
    /* Queued behind a slow command, they already show. */
    w32(PORT(MTX_LOAD_4x4), 0);
    for (int i = 0; i < 15; i++) w32(PORT(MTX_LOAD_4x4), 0);
    w32(PORT(POS_TEST), 0);
    w32(PORT(POS_TEST), 0);
    OK("test busy while queued", r32(GXSTAT) & 1u);
    drain();
    OK("test idle when done", !(r32(GXSTAT) & 1u));
}

/* ------------------------------------------------------------------ */
/* Packed commands                                                     */
/* ------------------------------------------------------------------ */

static void test_packed(void)
{
    g_section = "packed";
    fresh();

    /* GBATEK "GXFIFO / Packed Commands": four command bytes in a word,
     * lowest first, then the parameters of each in turn. Here: MTX_MODE 1,
     * MTX_TRANS (3, 4, 5), MTX_PUSH (none), MTX_MODE 0. */
    w32(GXFIFO, (uint32_t)MTX_MODE | (MTX_TRANS << 8) | (MTX_PUSH << 16) | ((uint32_t)MTX_MODE << 24));
    w32(GXFIFO, 1);
    w32(GXFIFO, 3 * ONE);
    w32(GXFIFO, 4 * ONE);
    w32(GXFIFO, 5 * ONE);
    w32(GXFIFO, 0);
    drain();
    EQ("translation x", clip_matrix(12), 3 * ONE);
    EQ("translation y", clip_matrix(13), 4 * ONE);
    EQ("translation z", clip_matrix(14), 5 * ONE);
    EQ("pushed once", (r32(GXSTAT) >> 8) & 31u, 1);
    EQ("ended in mode 0", gx.matrix_mode, 0);

    /* Unpacked: "command (upper 24bit zero)" then its parameters. */
    w32(GXFIFO, MTX_MODE);
    w32(GXFIFO, 3);
    drain();
    EQ("unpacked MTX_MODE", gx.matrix_mode, 3);

    /* The mirrors of GXFIFO up to 0x43F take the same words. */
    w32(GXFIFO + 0x3C, MTX_MODE);
    w32(GXFIFO + 0x10, 2);
    drain();
    EQ("GXFIFO mirror", gx.matrix_mode, 2);

    /* Counting FIFO entries with the engine held: a command without
     * parameters is one entry; zero bytes are padding and put nothing in;
     * "Invalid commands ... do not fetch any parameters". */
    w32(PORT(SWAP_BUFFERS), 0);
    for (int i = 0; i < 4; i++) w32(PORT(MTX_MODE), 0); /* fill the PIPE */
    w32(GXFIFO, MTX_IDENTITY);
    EQ("one command, three padding bytes", fifo_level(), 1);
    w32(GXFIFO, (uint32_t)MTX_IDENTITY | (MTX_IDENTITY << 8) | (MTX_IDENTITY << 16) | ((uint32_t)MTX_IDENTITY << 24));
    EQ("four commands without parameters", fifo_level(), 5);
    w32(GXFIFO, (uint32_t)MTX_IDENTITY << 16);
    EQ("padding below a command", fifo_level(), 6);
    w32(GXFIFO, 0xFF);
    EQ("undefined command: one entry, no parameters", fifo_level(), 7);
    w32(GXFIFO, MTX_IDENTITY);
    EQ("and the next word is a command", fifo_level(), 8);
    /* An all-zero word is a NOP that does go into the FIFO (melonDS). */
    w32(GXFIFO, 0);
    EQ("all-zero word", fifo_level(), 9);
    /* A command with parameters is one entry per parameter. */
    w32(GXFIFO, MTX_TRANS);
    EQ("command word alone: nothing yet", fifo_level(), 9);
    w32(GXFIFO, 1);
    w32(GXFIFO, 2);
    w32(GXFIFO, 3);
    EQ("three parameters", fifo_level(), 12);

    /* GXFIFO and the command ports are 32-bit registers. */
    nds_gx_write(&gx, PORT(MTX_IDENTITY), 0, 2);
    nds_gx_write(&gx, GXFIFO, MTX_IDENTITY, 1);
    EQ("narrow writes do nothing", fifo_level(), 12);
    vblank();
    EQ("drained", fifo_level(), 0);
}

/* ------------------------------------------------------------------ */
/* Matrices                                                            */
/* ------------------------------------------------------------------ */

/* The documented arithmetic, done the plain way: a sum of exact products,
 * divided by 4096 rounding down, cut to 32 bits. (Rounding down is not
 * written with %: MSVC 19.51 at /O2, with this function inlined into the
 * loop below, makes -103743488 % 4096 come out as -4096.) */
static int32_t fx(long long sum)
{
    long long q = sum / 4096; /* towards zero */
    if (q * 4096 > sum) q--;
    return (int32_t)(uint32_t)(unsigned long long)q;
}

static void expect_product(const char *name, const int32_t m[16], const int32_t c[16])
{
    /* C = M * C: "cyx = ay1*b1x + ay2*b2x + ay3*b3x + ay4*b4x" */
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            long long sum = 0;
            for (int k = 0; k < 4; k++) sum += (long long)m[row * 4 + k] * c[k * 4 + col];
            EQ(name, clip_matrix(row * 4 + col), fx(sum));
        }
    }
}

static void test_matrices(void)
{
    static const int32_t a[16] = {
        0x1800, -0x0800, 0x0123, 0x0010, -0x2000, 0x0FFF, 0x0001, -0x0001,
        0x0333, 0x0555, -0x1234, 0x0040, 0x7000, -0x3000, 0x0100, 0x1000,
    };
    static const int32_t b[16] = {
        0x0400, 0x1000, -0x0010, 0x0002, 0x0C00, -0x0C00, 0x2000, 0x0000,
        -0x0001, 0x0003, 0x0800, 0x0FFF, 0x0100, 0x0200, -0x0300, 0x1000,
    };
    int32_t expanded[16];
    g_section = "matrices";

    /* MTX_LOAD_4x4, read back through CLIPMTX_RESULT with the other matrix
     * the identity ("so the ClipMatrix becomes equal to the DESIRED
     * matrix"). */
    fresh();
    cmd(MTX_MODE, 0);
    cmdn(MTX_LOAD_4x4, a, 16);
    for (int i = 0; i < 16; i++) EQ("MTX_LOAD_4x4 projection", clip_matrix(i), a[i]);
    cmd(MTX_IDENTITY, 0);
    for (int i = 0; i < 16; i++) EQ("MTX_IDENTITY", clip_matrix(i), i % 5 == 0 ? ONE : 0);
    cmd(MTX_MODE, 1);
    cmdn(MTX_LOAD_4x4, a, 16);
    for (int i = 0; i < 16; i++) EQ("MTX_LOAD_4x4 position", clip_matrix(i), a[i]);

    /* MTX_LOAD_4x3: twelve parameters, the fourth column (0, 0, 0, 1.0). */
    cmdn(MTX_LOAD_4x3, a, 12);
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int32_t want = col < 3 ? a[row * 3 + col] : (row == 3 ? ONE : 0);
            EQ("MTX_LOAD_4x3", clip_matrix(row * 4 + col), want);
        }
    }

    /* "ClipMatrix = PositionMatrix * ProjectionMatrix". */
    cmd(MTX_MODE, 0);
    cmdn(MTX_LOAD_4x4, b, 16);
    cmd(MTX_MODE, 1);
    cmdn(MTX_LOAD_4x4, a, 16);
    expect_product("clip = position * projection", a, b);

    /* MTX_MULT_4x4: "Sets C=M*C". */
    cmd(MTX_MODE, 0);
    cmd(MTX_IDENTITY, 0);
    cmd(MTX_MODE, 1);
    cmdn(MTX_LOAD_4x4, b, 16);
    cmdn(MTX_MULT_4x4, a, 16);
    expect_product("MTX_MULT_4x4", a, b);

    /* MTX_MULT_4x3 and MTX_MULT_3x3: the missing elements are the
     * identity's. */
    cmdn(MTX_LOAD_4x4, b, 16);
    cmdn(MTX_MULT_4x3, a, 12);
    memset(expanded, 0, sizeof expanded);
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 3; col++) expanded[row * 4 + col] = a[row * 3 + col];
    expanded[15] = ONE;
    expect_product("MTX_MULT_4x3", expanded, b);

    cmdn(MTX_LOAD_4x4, b, 16);
    cmdn(MTX_MULT_3x3, a, 9);
    memset(expanded, 0, sizeof expanded);
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) expanded[row * 4 + col] = a[row * 3 + col];
    expanded[15] = ONE;
    expect_product("MTX_MULT_3x3", expanded, b);

    /* MTX_SCALE: the diagonal (m0, m1, m2, 1.0). By hand for one element:
     * row 0 of b is scaled by 1.5 (0x1800): 0x0400 * 1.5 = 0x0600, and
     * -0x0010 * 1.5 = -0x0018. */
    cmdn(MTX_LOAD_4x4, b, 16);
    cmdn(MTX_SCALE, a, 3);
    EQ("MTX_SCALE by hand", clip_matrix(0), 0x0600);
    EQ("MTX_SCALE by hand, negative", clip_matrix(2), -0x0018);
    memset(expanded, 0, sizeof expanded);
    expanded[0] = a[0];
    expanded[5] = a[1];
    expanded[10] = a[2];
    expanded[15] = ONE;
    expect_product("MTX_SCALE", expanded, b);

    /* MTX_TRANS: the identity with (m0, m1, m2) in the bottom row. */
    cmdn(MTX_LOAD_4x4, b, 16);
    cmdn(MTX_TRANS, a, 3);
    memset(expanded, 0, sizeof expanded);
    expanded[0] = expanded[5] = expanded[10] = expanded[15] = ONE;
    expanded[12] = a[0];
    expanded[13] = a[1];
    expanded[14] = a[2];
    expect_product("MTX_TRANS", expanded, b);

    /* Rounding and width (GBATEK "Fixed-Point Numbers": the sum keeps its
     * full 24-bit fraction and is "shifted-right by 12"): the shift rounds
     * down, and the result is cut to 32 bits. By hand: 0.5 * -1/4096 is
     * -1/8192, which becomes -1; 0x7FFFFFFF * 2.0 wraps to -2. */
    {
        static const int32_t half[3] = {0x0800, 0x0800, 0x0800};
        static const int32_t twice[3] = {0x2000, 0x2000, 0x2000};
        static const int32_t tiny[16] = {-1, 1, 0, 0, 0, 0x7FFFFFFF, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE};
        cmdn(MTX_LOAD_4x4, tiny, 16);
        cmdn(MTX_SCALE, half, 3);
        EQ("rounds down below zero", clip_matrix(0), -1);
        EQ("rounds down above zero", clip_matrix(1), 0);
        cmdn(MTX_LOAD_4x4, tiny, 16);
        cmdn(MTX_SCALE, twice, 3);
        EQ("cut to 32 bits", clip_matrix(5), -2);
    }

    /* MTX_MODE 2: load, multiply and translate go to the position and the
     * vector matrix; "MTX_SCALE in Mode 2: uses ONLY Position Matrix". */
    fresh();
    cmd(MTX_MODE, 2);
    cmdn(MTX_LOAD_4x4, b, 16);
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) EQ("mode 2 load: vector matrix", vector_matrix(row * 3 + col), b[row * 4 + col]);
    {
        static const int32_t twice[3] = {0x2000, 0x2000, 0x2000};
        cmdn(MTX_SCALE, twice, 3);
        EQ("mode 2 scale: position scaled", clip_matrix(0), 2 * b[0]);
        EQ("mode 2 scale: vector not", vector_matrix(0), b[0]);
        cmd(MTX_IDENTITY, 0);
        cmdn(MTX_MULT_3x3, a, 9);
        EQ("mode 2 multiply: position", clip_matrix(1), a[1]);
        EQ("mode 2 multiply: vector", vector_matrix(1), a[1]);
    }
    /* Mode 1: the position matrix alone. */
    cmd(MTX_MODE, 1);
    cmd(MTX_IDENTITY, 0);
    EQ("mode 1 identity: position", clip_matrix(1), 0);
    EQ("mode 1 identity: vector kept", vector_matrix(1), a[1]);
    /* Mode 0 and 3 leave both alone; mode 3 is the texture matrix, which
     * has no result register (see the texture coordinate tests). */
    cmd(MTX_MODE, 3);
    cmdn(MTX_LOAD_4x4, b, 16);
    EQ("mode 3 load: clip unchanged", clip_matrix(0), ONE);
    EQ("mode 3 load: vector unchanged", vector_matrix(1), a[1]);
}

/* ------------------------------------------------------------------ */
/* Matrix stacks                                                       */
/* ------------------------------------------------------------------ */

static unsigned position_level(void)
{
    return (r32(GXSTAT) >> 8) & 31u;
}

static unsigned projection_level(void)
{
    return (r32(GXSTAT) >> 13) & 1u;
}

static unsigned stack_error(void)
{
    return (r32(GXSTAT) >> 15) & 1u;
}

static void load_scalar(int32_t value)
{
    int32_t m[16] = {0};
    m[0] = m[5] = m[10] = value;
    m[15] = ONE;
    cmdn(MTX_LOAD_4x4, m, 16);
}

static void test_stacks(void)
{
    g_section = "stacks";
    fresh();

    /* GBATEK "DS 3D Matrix Stack": MTX_PUSH "Sets [S]=C, and then S=S+1";
     * MTX_POP "Sets S=S-N, and then C=[S]". */
    cmd(MTX_MODE, 2);
    load_scalar(0x111);
    cmd(MTX_PUSH, 0);
    EQ("push: level 1", position_level(), 1);
    load_scalar(0x222);
    cmd(MTX_PUSH, 0);
    load_scalar(0x333);
    cmd(MTX_PUSH, 0);
    EQ("three pushes: level 3", position_level(), 3);
    load_scalar(0x444);
    cmd(MTX_POP, 1);
    EQ("pop 1: level 2", position_level(), 2);
    EQ("pop 1: the last pushed", clip_matrix(0), 0x333);
    EQ("pop 1: vector matrix too", vector_matrix(0), 0x333);
    cmd(MTX_POP, 2);
    EQ("pop 2: level 0", position_level(), 0);
    EQ("pop 2: the first pushed", clip_matrix(0), 0x111);
    /* "Zero or negative values can be used to pop previously 'deallocated'
     * values": the offset is a signed 6-bit number. */
    cmd(MTX_POP, 0x3E); /* -2 */
    EQ("pop -2: level 2", position_level(), 2);
    EQ("pop -2: entry 2", clip_matrix(0), 0x333);
    cmd(MTX_POP, 0);
    EQ("pop 0: entry 2 again", clip_matrix(0), 0x333);
    EQ("no error so far", stack_error(), 0);

    /* "MTX_PUSH/POP/STORE/RESTORE in Mode 1: uses BOTH Position AND Vector
     * Matrices." */
    fresh();
    cmd(MTX_MODE, 2);
    load_scalar(0x555);
    cmd(MTX_MODE, 1);
    cmd(MTX_PUSH, 0);
    cmd(MTX_MODE, 2);
    load_scalar(0x666);
    cmd(MTX_MODE, 1);
    cmd(MTX_POP, 1);
    EQ("mode 1 pop restores the vector matrix", vector_matrix(0), 0x555);

    /* MTX_STORE "Sets [N]=C", MTX_RESTORE "Sets C=[N]"; "The stack pointer S
     * is not used, and is left unchanged." */
    cmd(MTX_MODE, 2);
    load_scalar(0x777);
    cmd(MTX_STORE, 30);
    load_scalar(0x888);
    cmd(MTX_STORE, 5);
    cmd(MTX_RESTORE, 30);
    EQ("restore 30", clip_matrix(0), 0x777);
    cmd(MTX_RESTORE, 5);
    EQ("restore 5", clip_matrix(0), 0x888);
    EQ("store and restore leave the pointer", position_level(), 0);
    EQ("entries 0-30 are valid", stack_error(), 0);
    /* "Stack Address (0..30) (31 causes overflow in GXSTAT.15)"; entry 31
     * "appears to exist". */
    load_scalar(0x999);
    cmd(MTX_STORE, 31);
    EQ("store 31: error", stack_error(), 1);
    w32(GXSTAT, 1u << 15);
    EQ("error acknowledged", stack_error(), 0);
    load_scalar(0xAAA);
    cmd(MTX_RESTORE, 31);
    EQ("restore 31: error", stack_error(), 1);
    EQ("restore 31: but it holds what was stored", clip_matrix(0), 0x999);
    w32(GXSTAT, 1u << 15);

    /* Overflow: 31 entries. The 31st push fills entry 30; the 32nd goes to
     * entry 31 and sets the error flag. */
    fresh();
    cmd(MTX_MODE, 1);
    for (int i = 0; i < 31; i++) cmd(MTX_PUSH, 0);
    EQ("31 pushes: level 31", position_level(), 31);
    EQ("31 pushes: no error", stack_error(), 0);
    cmd(MTX_PUSH, 0);
    EQ("32nd push: error", stack_error(), 1);
    /* "Position & Vector Matrix Stack Level (0..31) (lower 5bit of 6bit
     * value)". */
    EQ("32nd push: level reads 0 (of 32)", position_level(), 0);
    w32(GXSTAT, 1u << 15);
    EQ("acknowledge leaves the position pointer", gx.position_sp, 32);
    /* Underflow. */
    fresh();
    cmd(MTX_MODE, 1);
    cmd(MTX_POP, 1);
    EQ("pop below zero: error", stack_error(), 1);
    EQ("pop below zero: level 63 reads 31", position_level(), 31);

    /* The projection stack: one entry; "the parameter value is ignored,
     * the offset is always +1 in that mode". */
    fresh();
    cmd(MTX_MODE, 0);
    load_scalar(0x123);
    cmd(MTX_PUSH, 0);
    EQ("projection push: level 1", projection_level(), 1);
    EQ("projection push: no error", stack_error(), 0);
    load_scalar(0x456);
    cmd(MTX_POP, 17);
    EQ("projection pop: level 0", projection_level(), 0);
    EQ("projection pop: restored", clip_matrix(0), 0x123);
    EQ("projection pop: no error", stack_error(), 0);
    cmd(MTX_PUSH, 0);
    cmd(MTX_PUSH, 0);
    EQ("second projection push: error", stack_error(), 1);
    /* GXSTAT bit 15: "Writing 1 ... does reset the Error Flag, and
     * additionally resets the Projection Stack Pointer". */
    cmd(MTX_PUSH, 0);
    EQ("projection level before acknowledge", projection_level(), 1);
    w32(GXSTAT, 1u << 15);
    EQ("acknowledge: error clear", stack_error(), 0);
    EQ("acknowledge: projection level 0", projection_level(), 0);
    cmd(MTX_POP, 1);
    EQ("projection pop at level 0: error", stack_error(), 1);
    /* Store and restore in projection mode use the one entry. */
    fresh();
    cmd(MTX_MODE, 0);
    load_scalar(0x321);
    cmd(MTX_STORE, 9);
    load_scalar(0x654);
    cmd(MTX_RESTORE, 20);
    EQ("projection store/restore", clip_matrix(0), 0x321);
    EQ("projection store/restore: no error", stack_error(), 0);

    /* The texture stack: one entry, its own pointer, not in GXSTAT. */
    fresh();
    cmd(MTX_MODE, 3);
    cmd(MTX_PUSH, 0);
    EQ("texture push: position level unchanged", position_level(), 0);
    EQ("texture push: projection level unchanged", projection_level(), 0);
    EQ("texture push: no error", stack_error(), 0);
    cmd(MTX_PUSH, 0);
    EQ("second texture push: error", stack_error(), 1);
}

/* ------------------------------------------------------------------ */
/* Lighting                                                            */
/* ------------------------------------------------------------------ */

#define RGB15(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 5) | ((uint32_t)(b) << 10))
/* A packed 10-bit vector, 1.9 fixed point. */
#define VEC10(x, y, z) (((uint32_t)(x) & 0x3FFu) | (((uint32_t)(y) & 0x3FFu) << 10) | (((uint32_t)(z) & 0x3FFu) << 20))

static void light_setup(unsigned enabled)
{
    fresh();
    cmd(MTX_MODE, 2);
    cmd(MTX_IDENTITY, 0);
    cmd(POLYGON_ATTR, enabled | CULL_NONE | OPAQUE);
    cmd(BEGIN_VTXS, 0);
}

static void test_lighting(void)
{
    g_section = "lighting";

    /* GBATEK "Internal Operation on Normal Command": "VertexColor =
     * EmissionColor", then for each enabled light the specular, diffuse and
     * ambient terms. No light enabled: the emission colour, "no matter if
     * any lights are enabled". */
    light_setup(0);
    cmd(DIF_AMB, RGB15(31, 31, 31) | (RGB15(31, 31, 31) << 16));
    cmd(SPE_EMI, RGB15(31, 31, 31) | (RGB15(5, 10, 15) << 16));
    cmd(LIGHT_COLOR, RGB15(31, 31, 31));
    cmd(LIGHT_VECTOR, VEC10(0, 0, -512));
    cmd(NORMAL, VEC10(0, 0, 511));
    EQ("no lights: emission red", gx.color[0], 5);
    EQ("no lights: emission green", gx.color[1], 10);
    EQ("no lights: emission blue", gx.color[2], 15);

    /*
     * One light shining straight down -z onto a surface facing +z:
     * "DiffuseLevel = max(0,-(LightVector*NormalVector))" is as large as it
     * gets. In the engine's fixed point (the widths are melonDS's): the
     * light vector is kept negated, (0, 0, 512); the normal is
     * (0, 0, 511); their product loses 9 bits: 512 * 511 >> 9 = 511. The
     * diffuse term is diffuse * light * 511 with 14 fraction bits:
     * 31 * 31 * 511 >> 14 = 29, and 16 * 8 * 511 >> 14 = 3.
     */
    light_setup(1);
    cmd(DIF_AMB, RGB15(31, 16, 0));
    cmd(SPE_EMI, 0);
    cmd(LIGHT_COLOR, RGB15(31, 8, 31));
    cmd(LIGHT_VECTOR, VEC10(0, 0, -512));
    cmd(NORMAL, VEC10(0, 0, 511));
    EQ("diffuse, head on: red", gx.color[0], 31 * 31 * 511 >> 14);
    EQ("diffuse, head on: green", gx.color[1], 16 * 8 * 511 >> 14);
    EQ("diffuse, head on: blue", gx.color[2], 0);
    /* Light from behind the surface, and edge on: no diffuse light. */
    cmd(NORMAL, VEC10(0, 0, -512));
    EQ("diffuse, from behind", gx.color[0], 0);
    cmd(NORMAL, VEC10(511, 0, 0));
    EQ("diffuse, edge on", gx.color[0], 0);
    /* At 60 degrees, half: the normal (0, 0.866, 0.5) is (0, 443, 256), the
     * dot product 512 * 256 >> 9 = 256, and 31 * 31 * 256 >> 14 = 15. */
    cmd(NORMAL, VEC10(0, 443, 256));
    EQ("diffuse at 60 degrees", gx.color[0], 31 * 31 * 256 >> 14);

    /* Ambient: "VertexColor + AmbientColor*LightColor", whatever the
     * directions; a plain shift in the engine (melonDS): a * l >> 5. */
    light_setup(1);
    cmd(DIF_AMB, RGB15(0, 0, 0) | (RGB15(31, 16, 8) << 16));
    cmd(SPE_EMI, 0);
    cmd(LIGHT_COLOR, RGB15(31, 31, 16));
    cmd(LIGHT_VECTOR, VEC10(0, 0, -512));
    cmd(NORMAL, VEC10(0, 0, -512));
    EQ("ambient red", gx.color[0], 31 * 31 >> 5);
    EQ("ambient green", gx.color[1], 16 * 31 >> 5);
    EQ("ambient blue", gx.color[2], 8 * 16 >> 5);

    /* Lights add up and the sum saturates at 31. Two lights, each giving
     * ambient 16 * 31 >> 5 = 15: the 14-bit fractions add first, so
     * 2 * (16 * 31 << 9) >> 14 = 31. Emission 20 on top: saturated. */
    light_setup(3);
    cmd(DIF_AMB, RGB15(0, 0, 0) | (RGB15(16, 16, 16) << 16));
    cmd(SPE_EMI, 0);
    cmd(LIGHT_COLOR, RGB15(31, 31, 31));
    cmd(LIGHT_COLOR, RGB15(31, 31, 31) | (1u << 30));
    cmd(NORMAL, VEC10(0, 0, 511));
    EQ("two lights add", gx.color[0], (2 * ((16 * 31) << 9)) >> 14);
    cmd(SPE_EMI, RGB15(20, 20, 20) << 16);
    cmd(NORMAL, VEC10(0, 0, 511));
    EQ("saturates at 31", gx.color[0], 31);
    /* Only the lights enabled in POLYGON_ATTR as of BEGIN_VTXS count. */
    cmd(SPE_EMI, 0);
    cmd(POLYGON_ATTR, 2 | CULL_NONE | OPAQUE);
    cmd(NORMAL, VEC10(0, 0, 511));
    EQ("POLYGON_ATTR not applied before BEGIN_VTXS", gx.color[0], 31);
    cmd(BEGIN_VTXS, 0);
    cmd(NORMAL, VEC10(0, 0, 511));
    EQ("one light after BEGIN_VTXS", gx.color[0], 16 * 31 >> 5);

    /*
     * Specular: "ShininessLevel = max(0,(-HalfVector)*(NormalVector))^2",
     * the half vector being halfway between the light and the line of
     * sight (0, 0, -1.0). Light and sight both along -z onto a surface
     * facing +z: the level is at its largest. In fixed point (melonDS):
     * dot + Nz = 511 + 511 = 1022; squared and cut, 1022 * 1022 >> 10 =
     * 1020; times the light's reciprocal 2^18 / (512 + 512) = 256, >> 8,
     * less 512: 508, capped at 511. Specular 31, light 31: 31 * 508 * 31
     * >> 14 = 29.
     */
    light_setup(1);
    cmd(DIF_AMB, 0);
    cmd(SPE_EMI, RGB15(31, 31, 31));
    cmd(LIGHT_COLOR, RGB15(31, 31, 31));
    cmd(LIGHT_VECTOR, VEC10(0, 0, -512));
    cmd(NORMAL, VEC10(0, 0, 511));
    EQ("specular, head on", gx.color[0], 31 * 508 * 31 >> 14);
    /* Away from the half vector it falls to nothing well before 90
     * degrees: at 60, (dot + Nz) = 256 + 256 = 512, squared >> 10 = 256,
     * 256 * 256 >> 8 = 256, less 512: below zero. */
    cmd(NORMAL, VEC10(0, 443, 256));
    EQ("specular at 60 degrees", gx.color[0], 0);
    /* "Specular Reflection Shininess Table (0=Disable, 1=Enable)": the
     * level indexes the 128-entry table (level >> 2), and the entry is the
     * level used (doubled, the table being 8 bits). Entry 508 >> 2 = 127. */
    {
        int32_t table[32];
        for (int i = 0; i < 32; i++) table[i] = 0;
        table[31] = (int32_t)0x40000000; /* entry 127 = 0x40 */
        cmdn(SHININESS, table, 32);
        cmd(SPE_EMI, RGB15(31, 31, 31) | 0x8000u);
        cmd(NORMAL, VEC10(0, 0, 511));
        EQ("specular through the table", gx.color[0], 31 * (0x40 << 1) * 31 >> 14);
        table[31] = 0;
        cmdn(SHININESS, table, 32);
        cmd(NORMAL, VEC10(0, 0, 511));
        EQ("specular through a zero table", gx.color[0], 0);
    }

    /* LIGHT_VECTOR and NORMAL go through the directional matrix ("the
     * incoming vector is multiplied by the current Directional Matrix"):
     * turning both by the same rotation changes nothing; turning the light
     * alone by 90 degrees about x puts it edge on. */
    {
        /* +y to +z, +z to -y */
        static const int32_t quarter[9] = {ONE, 0, 0, 0, 0, ONE, 0, -ONE, 0};
        light_setup(1);
        cmd(DIF_AMB, RGB15(31, 31, 31));
        cmd(LIGHT_COLOR, RGB15(31, 31, 31));
        cmdn(MTX_MULT_3x3, quarter, 9);
        cmd(LIGHT_VECTOR, VEC10(0, 0, -512));
        cmd(NORMAL, VEC10(0, 0, 511));
        EQ("both rotated: as head on", gx.color[0], 31 * 31 * 511 >> 14);
        cmd(MTX_IDENTITY, 0);
        cmd(NORMAL, VEC10(0, 0, 511));
        EQ("light rotated alone: edge on", gx.color[0], 0);
        /* "MTX_SCALE doesn't change Vector Matrix ... for keeping the length
         * of the light vector's intact". */
        static const int32_t twice[3] = {2 * ONE, 2 * ONE, 2 * ONE};
        cmd(LIGHT_VECTOR, VEC10(0, 0, -512));
        cmdn(MTX_SCALE, twice, 3);
        cmd(NORMAL, VEC10(0, 0, 511));
        EQ("scale leaves lighting alone", gx.color[0], 31 * 31 * 511 >> 14);
    }

    /* DIF_AMB bit 15: "Set Diffuse Reflection Color as Vertex Color"; COLOR
     * sets it directly. */
    light_setup(0);
    cmd(COLOR, RGB15(1, 2, 3));
    EQ("COLOR red", gx.color[0], 1);
    EQ("COLOR green", gx.color[1], 2);
    EQ("COLOR blue", gx.color[2], 3);
    cmd(DIF_AMB, RGB15(7, 8, 9));
    EQ("DIF_AMB without bit 15", gx.color[0], 1);
    cmd(DIF_AMB, RGB15(7, 8, 9) | 0x8000u);
    EQ("DIF_AMB bit 15: red", gx.color[0], 7);
    EQ("DIF_AMB bit 15: blue", gx.color[2], 9);
}

/* ------------------------------------------------------------------ */
/* Polygons                                                            */
/* ------------------------------------------------------------------ */

/* Identity matrices: clip coordinates are the vertex coordinates and W is
 * 1.0, so the view volume is -1.0..1.0 on every axis. The viewport is the
 * whole screen. */
static void polygon_setup(uint32_t attr)
{
    fresh();
    cmd(VIEWPORT, (255u << 16) | (191u << 24));
    cmd(POLYGON_ATTR, attr);
    cmd(BEGIN_VTXS, 0);
}

static void triangle(int x0, int y0, int x1, int y1, int x2, int y2)
{
    vtx16(x0, y0, 0);
    vtx16(x1, y1, 0);
    vtx16(x2, y2, 0);
}

static void test_texcoords(void)
{
    static const int32_t texture_matrix[16] = {
        2 * ONE, 0, 0, 0,       /* S' = 2 S ... */
        0, ONE / 2, 0, 0,       /* T' = T / 2 ... */
        0, 0, ONE, 0,
        16 * ONE, 32 * ONE, 0, ONE, /* ... + 16, + 32 */
    };
    g_section = "texture coordinates";

    /* Mode 0: "( S' T' ) = ( S T )". */
    polygon_setup(CULL_NONE | OPAQUE);
    cmd(MTX_MODE, 3);
    cmdn(MTX_LOAD_4x4, texture_matrix, 16);
    cmd(TEXIMAGE_PARAM, 0u << 30);
    cmd(TEXCOORD, (0x0123u << 16) | 0xFE00u);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    EQ("mode 0: S", polygon_vertex(last_polygon(), 2)->tex[0], -0x200);
    EQ("mode 0: T", polygon_vertex(last_polygon(), 2)->tex[1], 0x123);

    /* Mode 1, at TEXCOORD: "( S T 1/16 1/16 ) * M", S and T being 12.4:
     * with the matrix above S' = 2 S + 16/16 texel = 2 S + 16 units,
     * T' = T / 2 + 32. */
    cmd(TEXIMAGE_PARAM, 1u << 30);
    cmd(TEXCOORD, (0x0123u << 16) | 0xFE00u);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    EQ("mode 1: S", polygon_vertex(last_polygon(), 0)->tex[0], 2 * -0x200 + 16);
    EQ("mode 1: T", polygon_vertex(last_polygon(), 0)->tex[1], 0x123 / 2 + 32);
    /* m[8] counts as m[12] does. */
    {
        int32_t m[16];
        memcpy(m, texture_matrix, sizeof m);
        m[8] = 5 * ONE;
        m[9] = -3 * ONE;
        cmdn(MTX_LOAD_4x4, m, 16);
        cmd(TEXCOORD, 0);
        triangle(0, 0, 0x400, 0, 0, 0x400);
        EQ("mode 1: the third row", polygon_vertex(last_polygon(), 0)->tex[0], 16 + 5);
        EQ("mode 1: the third row, T", polygon_vertex(last_polygon(), 0)->tex[1], 32 - 3);
        cmdn(MTX_LOAD_4x4, texture_matrix, 16);
    }
    /* "the S',T' values are clipped to 16bit size": 0x7000 * 2 wraps. */
    cmd(TEXCOORD, 0x7000u);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    EQ("mode 1: wraps at 16 bits", polygon_vertex(last_polygon(), 0)->tex[0], 2 * 0x7000 + 16 - 0x10000);

    /* Mode 2, at NORMAL: "( Nx Ny Nz 1.0 ) * M" with (S, T) as the bottom
     * row. The matrix is in texcoord units per 1.0 (melonDS): a scale of
     * 1024.0 takes a normal of length 1 to 1024 units, half a 128-texel
     * texture, which is how sphere mapping is set up. Normal x = 0.5 (256):
     * 256 * 1024.0 >> 9 = 512 units, plus S. */
    {
        static const int32_t sphere[3] = {1024 * ONE, -1024 * ONE, ONE};
        cmd(MTX_IDENTITY, 0);
        cmdn(MTX_SCALE, sphere, 3);
        cmd(TEXIMAGE_PARAM, 2u << 30);
        cmd(TEXCOORD, (0x0400u << 16) | 0x0400u);
        cmd(NORMAL, VEC10(256, 128, 0));
        triangle(0, 0, 0x400, 0, 0, 0x400);
        EQ("mode 2: S", polygon_vertex(last_polygon(), 0)->tex[0], 0x400 + 512);
        EQ("mode 2: T", polygon_vertex(last_polygon(), 0)->tex[1], 0x400 - 256);
        /* TEXCOORD itself does not transform in this mode. */
        cmd(TEXCOORD, (0x0010u << 16) | 0x0020u);
        triangle(0, 0, 0x400, 0, 0, 0x400);
        EQ("mode 2: TEXCOORD alone: S as written", polygon_vertex(last_polygon(), 0)->tex[0], 0x20);
    }

    /* Mode 3, at each vertex: "( Vx Vy Vz 1.0 ) * M" with (S, T) as the
     * bottom row, in the same units: a scale of 16.0 gives one texel (16
     * units) per 1.0 of vertex coordinate. */
    {
        static const int32_t per_unit[3] = {16 * ONE, 32 * ONE, ONE};
        cmd(MTX_IDENTITY, 0);
        cmdn(MTX_SCALE, per_unit, 3);
        cmd(TEXIMAGE_PARAM, 3u << 30);
        cmd(TEXCOORD, (0x0100u << 16) | 0x0080u);
        triangle(0, 0, 0x800, 0, 0, 0xC00);
        EQ("mode 3: vertex 0 S", polygon_vertex(last_polygon(), 0)->tex[0], 0x80);
        EQ("mode 3: vertex 1 S", polygon_vertex(last_polygon(), 1)->tex[0], 0x80 + 8);
        EQ("mode 3: vertex 1 T", polygon_vertex(last_polygon(), 1)->tex[1], 0x100);
        EQ("mode 3: vertex 2 T", polygon_vertex(last_polygon(), 2)->tex[1], 0x100 + 24);
    }
}

static void test_vertices(void)
{
    g_section = "vertices";
    polygon_setup(CULL_NONE | OPAQUE);

    /* The VTX commands (GBATEK "DS 3D Polygon Definitions by Vertices"). */
    cmd(VTX_10, VEC10(0x10, -0x20, 0x30)); /* 6 fraction bits */
    EQ("VTX_10 x", gx.vertex[0], 0x10 << 6);
    EQ("VTX_10 y", gx.vertex[1], -(0x20 << 6));
    EQ("VTX_10 z", gx.vertex[2], 0x30 << 6);
    cmd(VTX_XY, (0x0222u << 16) | 0x0111u);
    EQ("VTX_XY x", gx.vertex[0], 0x111);
    EQ("VTX_XY y", gx.vertex[1], 0x222);
    EQ("VTX_XY keeps z", gx.vertex[2], 0x30 << 6);
    cmd(VTX_XZ, (0x0444u << 16) | 0x0333u);
    EQ("VTX_XZ x", gx.vertex[0], 0x333);
    EQ("VTX_XZ keeps y", gx.vertex[1], 0x222);
    EQ("VTX_XZ z", gx.vertex[2], 0x444);
    EQ("three vertices: a polygon", polygons(), 1);
    cmd(VTX_YZ, (0xFFFFu << 16) | 0x0555u);
    EQ("VTX_YZ keeps x", gx.vertex[0], 0x333);
    EQ("VTX_YZ y", gx.vertex[1], 0x555);
    EQ("VTX_YZ z", gx.vertex[2], -1);
    /* VTX_DIFF: "The 9bit fractional values are divided by 8 ... and that
     * 12bit fraction is then added to the old vtx coordinates": steps of
     * 1/4096. */
    cmd(VTX_DIFF, VEC10(5, -6, 511));
    EQ("VTX_DIFF x", gx.vertex[0], 0x333 + 5);
    EQ("VTX_DIFF y", gx.vertex[1], 0x555 - 6);
    EQ("VTX_DIFF z", gx.vertex[2], -1 + 511);
    vtx16(-0x1000, 0x0800, 0x0040);
    EQ("VTX_16 x", gx.vertex[0], -0x1000);
    EQ("VTX_16 y", gx.vertex[1], 0x800);
    EQ("VTX_16 z", gx.vertex[2], 0x40);
    EQ("six vertices: two polygons", polygons(), 2);

    /* BEGIN_VTXS: "3*N vertices per N triangles", "4*N vertices per N
     * quads", "3+(N-1) vertices per N triangles" in a strip, "4+(N-1)*2
     * vertices per N quads" in a quad strip. Twelve vertices in a fan that
     * stays inside the view volume. */
    static const int expect_polygons[4] = {4, 3, 10, 5};
    static const int expect_vertices[4] = {12, 12, 12, 12};
    for (unsigned type = 0; type < 4; type++) {
        polygon_setup(CULL_NONE | OPAQUE);
        cmd(BEGIN_VTXS, type);
        for (int i = 0; i < 12; i++) vtx16(-0x800 + 0x100 * i, (i & 1) ? 0x400 : -0x400, 0);
        EQ("polygons by primitive type", polygons(), expect_polygons[type]);
        EQ("vertices by primitive type", vertices(), expect_vertices[type]);
    }
    /* A strip's polygons are in consistent winding: triangle k is
     * (k, k+1, k+2) or, for odd k, (k+1, k, k+2) (melonDS); a quad strip's
     * quad k is (2k, 2k+1, 2k+3, 2k+2), the last two swapped so that the
     * four go round. Shared vertices are the same entries of vertex RAM. */
    polygon_setup(CULL_NONE | OPAQUE);
    cmd(BEGIN_VTXS, 2);
    for (int i = 0; i < 5; i++) vtx16(-0x800 + 0x100 * i, (i & 1) ? 0x400 : -0x400, 0);
    {
        const NdsGxPolygon *p = gx.polygons[gx.geometry_bank];
        EQ("strip triangle 0", p[0].vertex[0] * 100 + p[0].vertex[1] * 10 + p[0].vertex[2], 12);
        EQ("strip triangle 1", p[1].vertex[0] * 100 + p[1].vertex[1] * 10 + p[1].vertex[2], 213);
        EQ("strip triangle 2", p[2].vertex[0] * 100 + p[2].vertex[1] * 10 + p[2].vertex[2], 234);
        OK("strip triangles face the same way", (p[0].flags & NDS_GX_POLY_FRONT) == (p[1].flags & NDS_GX_POLY_FRONT) &&
                                                    (p[1].flags & NDS_GX_POLY_FRONT) == (p[2].flags & NDS_GX_POLY_FRONT));
    }
    polygon_setup(CULL_NONE | OPAQUE);
    cmd(BEGIN_VTXS, 3);
    for (int i = 0; i < 6; i++) vtx16(-0x800 + 0x100 * (i / 2), (i & 1) ? 0x400 : -0x400, 0);
    {
        /* Vertex i was sent at x = -0x800 + 0x100 * (i / 2), low (even i)
         * or high (odd i). */
        const NdsGxPolygon *p = gx.polygons[gx.geometry_bank];
        static const int order[2][4] = {{0, 1, 3, 2}, {2, 3, 5, 4}};
        for (int quad = 0; quad < 2; quad++) {
            for (int i = 0; i < 4; i++) {
                int sent = order[quad][i];
                const NdsGxVertex *v = &gx.vertices[gx.geometry_bank][p[quad].vertex[i]];
                EQ("quad strip vertex x", v->pos[0], -0x800 + 0x100 * (sent / 2));
                EQ("quad strip vertex y", v->pos[1], (sent & 1) ? 0x400 : -0x400);
            }
        }
        EQ("quad strip shares its middle vertices", p[1].vertex[0] * 10 + p[1].vertex[1], p[0].vertex[3] * 10 + p[0].vertex[2]);
    }

    /* "Writes to POLYGON_ATTR have no effect until next BEGIN_VTXS";
     * TEXIMAGE_PARAM and PLTT_BASE apply at once. */
    polygon_setup(CULL_NONE | OPAQUE | (5u << 24));
    cmd(POLYGON_ATTR, CULL_NONE | OPAQUE | (9u << 24));
    cmd(TEXIMAGE_PARAM, 0x1234);
    cmd(PLTT_BASE, 0xFFFF);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    EQ("polygon ID as latched", last_polygon()->attr >> 24, 5);
    EQ("TEXIMAGE_PARAM", last_polygon()->tex_param, 0x1234);
    EQ("PLTT_BASE is 13 bits", last_polygon()->palette, 0x1FFF);
    cmd(BEGIN_VTXS, 0);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    EQ("polygon ID after BEGIN_VTXS", last_polygon()->attr >> 24, 9);
}

static void test_viewport(void)
{
    g_section = "viewport";

    /* GBATEK "Notes on VTX commands":
     *   screen_x = (xx+ww)*viewport_width / (2*ww) + viewport_x1
     *   screen_y = (yy+ww)*viewport_height / (2*ww) + viewport_y1
     * with (0, 0) the lower left; the module counts lines from the top. */
    polygon_setup(CULL_NONE | OPAQUE);
    triangle(0, 0, -0x1000, 0x1000, 0x1000, -0x1000);
    {
        const NdsGxPolygon *p = last_polygon();
        EQ("centre x", polygon_vertex(p, 0)->sx, 128);
        EQ("centre y", polygon_vertex(p, 0)->sy, 96);
        EQ("top left x", polygon_vertex(p, 1)->sx, 0);
        EQ("top left y", polygon_vertex(p, 1)->sy, 0);
        EQ("bottom right x", polygon_vertex(p, 2)->sx, 256);
        EQ("bottom right y", polygon_vertex(p, 2)->sy, 192);
        EQ("top vertex", p->top, 1);
        EQ("bottom vertex", p->bottom, 2);
        EQ("top line", p->y_top, 0);
        EQ("bottom line", p->y_bottom, 192);
    }
    /* A window 128 wide and 64 high, 64 from the left and 32 from the
     * bottom: X1 = 64, Y1 = 32, X2 = 191, Y2 = 95. Its top line is
     * 191 - 95 = 96 from the top of the screen. */
    cmd(VIEWPORT, 64u | (32u << 8) | (191u << 16) | (95u << 24));
    triangle(0, 0, -0x1000, 0x1000, 0x1000, -0x1000);
    {
        const NdsGxPolygon *p = last_polygon();
        EQ("window: centre x", polygon_vertex(p, 0)->sx, 64 + 64);
        EQ("window: centre y", polygon_vertex(p, 0)->sy, 96 + 32);
        EQ("window: left", polygon_vertex(p, 1)->sx, 64);
        EQ("window: top", polygon_vertex(p, 1)->sy, 96);
        EQ("window: right", polygon_vertex(p, 2)->sx, 64 + 128);
        EQ("window: bottom", polygon_vertex(p, 2)->sy, 96 + 64);
    }
    /* A quarter of the way: x = -0.5, y = 0.25 with W = 1.0 in the full
     * screen: (0.5 * 256 / 2, 0.75 * 192 / 2) = (64, 72). */
    cmd(VIEWPORT, (255u << 16) | (191u << 24));
    triangle(-0x800, 0x400, 0, 0, 0x400, 0x400);
    EQ("quarter x", polygon_vertex(last_polygon(), 0)->sx, 64);
    EQ("quarter y", polygon_vertex(last_polygon(), 0)->sy, 72);

    /* The division is by W: a projection that puts z into W. With W = 2.0
     * the same clip x is half as far from the centre. */
    {
        static const int32_t projection[16] = {ONE, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE, ONE, 0, 0, 0, 0};
        fresh();
        cmd(VIEWPORT, (255u << 16) | (191u << 24));
        cmd(MTX_MODE, 0);
        cmdn(MTX_LOAD_4x4, projection, 16);
        cmd(POLYGON_ATTR, CULL_NONE | OPAQUE);
        cmd(BEGIN_VTXS, 0);
        vtx16(0x1000, 0, 0x2000);
        vtx16(0x1000, 0x1000, 0x1000);
        vtx16(0, 0x1000, 0x1000);
        EQ("W = 2.0: x", polygon_vertex(last_polygon(), 0)->sx, 128 + 64);
        EQ("W = 1.0: x", polygon_vertex(last_polygon(), 1)->sx, 256);
        EQ("stored W", polygon_vertex(last_polygon(), 0)->pos[3], 0x2000);
        /* W normalised per polygon to 16 bits: 0x2000 and 0x1000 need 14
         * bits, so nothing moves (the step is 4 bits); the interpolators
         * get them as they are. */
        EQ("normalised W, far", last_polygon()->w[0], 0x2000);
        EQ("normalised W, near", last_polygon()->w[1], 0x1000);
    }
}

static void test_culling(void)
{
    g_section = "culling";

    /* POLYGON_ATTR bit 6 "Polygon Back Surface", bit 7 "Polygon Front
     * Surface" (0=Hide, 1=Render); "The vertices are normally arranged
     * anti-clockwise". With y up: (0,0) (1,0) (0,1) is anticlockwise. */
    static const struct {
        uint32_t attr;
        int front_drawn, back_drawn;
    } cases[4] = {{0, 0, 0}, {1u << 6, 0, 1}, {1u << 7, 1, 0}, {3u << 6, 1, 1}};
    for (int i = 0; i < 4; i++) {
        polygon_setup(cases[i].attr | OPAQUE);
        triangle(0, 0, 0x400, 0, 0, 0x400);
        EQ("anticlockwise triangle", polygons(), cases[i].front_drawn);
        if (polygons()) OK("flagged front", last_polygon()->flags & NDS_GX_POLY_FRONT);
        polygon_setup(cases[i].attr | OPAQUE);
        triangle(0, 0, 0, 0x400, 0x400, 0);
        EQ("clockwise triangle", polygons(), cases[i].back_drawn);
        if (polygons()) OK("flagged back", !(last_polygon()->flags & NDS_GX_POLY_FRONT));
        /* "Line Segments are rendered always because they do not have any
         * front and back sides": a triangle with a vertex twice. */
        polygon_setup(cases[i].attr | OPAQUE);
        triangle(0, 0, 0, 0, 0x400, 0x400);
        EQ("line segment", polygons(), 1);
    }
    /* Facing is decided before perspective division, so a polygon keeps
     * its side when W is not 1: anticlockwise at W = 4.0. */
    {
        static const int32_t scale[3] = {ONE / 4, ONE / 4, ONE / 4};
        polygon_setup((1u << 7) | OPAQUE);
        cmd(MTX_MODE, 0);
        cmdn(MTX_SCALE, scale, 3);
        triangle(0, 0, 0x1000, 0, 0, 0x1000);
        EQ("anticlockwise, scaled", polygons(), 1);
    }
}

static void test_clipping(void)
{
    g_section = "clipping";

    /* GBATEK "Clipping": "Polygons are clipped to the 6 sides of the view
     * volume ... vertic(es) are replaced by two newly created vertices
     * (which are located on the intersections of the polygon edges and the
     * view volume edge)."
     *
     * A triangle with one vertex at 2.0 on an axis, the others at 0 and at
     * 0.5 across: the vertex outside is replaced by two on the plane, one
     * on each edge. Both edges cross halfway, so the new vertices are at
     * (1.0, 0) and (1.0, 0.25): exact in 4.12. For the far and near planes
     * the axis is z, which needs POLYGON_ATTR bit 12 for the far one. */
    for (int axis = 0; axis < 3; axis++) {
        for (int side = -1; side <= 1; side += 2) {
            int other = axis == 0 ? 1 : 0;
            int16_t a[3] = {0, 0, 0}, b[3] = {0, 0, 0}, c[3] = {0, 0, 0};
            b[axis] = (int16_t)(side * 0x2000);
            c[other] = 0x800;
            polygon_setup(CULL_NONE | OPAQUE | (1u << 12));
            vtx16(a[0], a[1], a[2]);
            vtx16(b[0], b[1], b[2]);
            vtx16(c[0], c[1], c[2]);
            EQ("one vertex outside: a polygon", polygons(), 1);
            EQ("one vertex outside: four vertices", vertices(), 4);
            if (polygons() != 1 || vertices() != 4) continue;
            const NdsGxPolygon *p = last_polygon();
            /* In order: a, the crossing of a-b, the crossing of b-c, c. */
            EQ("first vertex kept", polygon_vertex(p, 0)->pos[axis], 0);
            EQ("crossing a-b: on the plane", polygon_vertex(p, 1)->pos[axis], side * 0x1000);
            EQ("crossing a-b: across", polygon_vertex(p, 1)->pos[other], 0);
            EQ("crossing b-c: on the plane", polygon_vertex(p, 2)->pos[axis], side * 0x1000);
            EQ("crossing b-c: across", polygon_vertex(p, 2)->pos[other], 0x400);
            EQ("last vertex kept", polygon_vertex(p, 3)->pos[other], 0x800);
            EQ("W stays 1.0", polygon_vertex(p, 1)->pos[3], 0x1000);
            OK("made by the clipper", polygon_vertex(p, 1)->clipped && polygon_vertex(p, 2)->clipped);
            OK("the others were not", !polygon_vertex(p, 0)->clipped && !polygon_vertex(p, 3)->clipped);
        }
    }

    /* Colours and texture coordinates are interpolated to the crossing:
     * halfway between 30 and 0 is 15, between 0x100 and 0x300 is 0x200. */
    polygon_setup(CULL_NONE | OPAQUE);
    cmd(COLOR, RGB15(30, 0, 8));
    cmd(TEXCOORD, (0x0100u << 16) | 0x0100u);
    vtx16(0x2000, 0, 0);
    cmd(COLOR, RGB15(0, 30, 8));
    cmd(TEXCOORD, (0x0300u << 16) | 0x0300u);
    vtx16(0, 0, 0);
    vtx16(0, 0x800, 0);
    {
        /* The first vertex is the one outside: the clipped polygon starts
         * with the crossing of its first edge and ends with the crossing of
         * its last (polyrastertest scenes 62-67 show this order). */
        const NdsGxPolygon *p = last_polygon();
        EQ("four vertices", p->count, 4);
        EQ("starts with the first edge's crossing", polygon_vertex(p, 0)->pos[1], 0);
        EQ("ends with the last edge's crossing", polygon_vertex(p, 3)->pos[1], 0x400);
        EQ("red halfway", polygon_vertex(p, 0)->color[0], 15);
        EQ("green halfway", polygon_vertex(p, 0)->color[1], 15);
        EQ("blue constant", polygon_vertex(p, 0)->color[2], 8);
        EQ("S halfway", polygon_vertex(p, 0)->tex[0], 0x200);
        EQ("T halfway", polygon_vertex(p, 0)->tex[1], 0x200);
        EQ("clipped x on the screen edge", polygon_vertex(p, 0)->sx, 256);
    }

    /* Two vertices outside: still a triangle. All outside: nothing. All
     * inside (touching the planes counts as inside): untouched. */
    polygon_setup(CULL_NONE | OPAQUE);
    triangle(0, 0, 0x2000, 0, 0x2000, 0x800);
    EQ("two outside: three vertices", vertices(), 3);
    polygon_setup(CULL_NONE | OPAQUE);
    triangle(0x1800, 0, 0x2000, 0, 0x2000, 0x800);
    EQ("all outside: no polygon", polygons(), 0);
    EQ("all outside: no vertices", vertices(), 0);
    polygon_setup(CULL_NONE | OPAQUE);
    triangle(-0x1000, -0x1000, 0x1000, -0x1000, 0x1000, 0x1000);
    EQ("on the planes: three vertices", vertices(), 3);
    OK("on the planes: not clipped", !polygon_vertex(last_polygon(), 0)->clipped);
    /* Each vertex outside is replaced by two crossings; neighbours outside
     * the same plane share theirs. A quad with two neighbouring corners
     * past the right plane keeps four vertices; with one corner past it,
     * five; with one corner past the right plane and another past the top,
     * six. */
    polygon_setup(CULL_NONE | OPAQUE);
    cmd(BEGIN_VTXS, 1);
    vtx16(0, 0, 0);
    vtx16(0x1800, 0, 0);
    vtx16(0x1800, 0x800, 0);
    vtx16(0, 0x800, 0);
    EQ("quad, two corners past one plane: four vertices", last_polygon()->count, 4);
    cmd(BEGIN_VTXS, 1);
    vtx16(0, 0, 0);
    vtx16(0x1800, 0, 0);
    vtx16(0x800, 0x800, 0);
    vtx16(0, 0x800, 0);
    EQ("quad, one corner past a plane: five vertices", last_polygon()->count, 5);
    cmd(BEGIN_VTXS, 1);
    vtx16(0, 0, 0);
    vtx16(0x1800, 0, 0);
    vtx16(0, 0x1800, 0);
    vtx16(-0x800, 0x800, 0);
    EQ("quad, corners past two planes: six vertices", last_polygon()->count, 6);

    /* POLYGON_ATTR bit 12: "Far-plane intersecting polygons (0=Hide,
     * 1=Render/clipped)". The near plane always clips. */
    polygon_setup(CULL_NONE | OPAQUE);
    vtx16(0, 0, 0);
    vtx16(0, 0x800, 0x2000);
    vtx16(0x800, 0, 0);
    EQ("across the far plane, bit 12 clear: hidden", polygons(), 0);
    polygon_setup(CULL_NONE | OPAQUE);
    vtx16(0, 0, 0);
    vtx16(0, 0x800, -0x2000);
    vtx16(0x800, 0, 0);
    EQ("across the near plane, bit 12 clear: clipped", vertices(), 4);

    /* Strips: "clipped polygons which are part of polygon strips are
     * converted to separate polygons (which does increase number of entries
     * in Vertex RAM)". Four triangles, the third vertex-sharing pair cut by
     * the right plane: triangle 0 (3 vertices), triangle 1 shares two and
     * adds one (4); triangle 2 has its new vertex outside: four vertices of
     * its own (8); triangle 3 follows a clipped polygon, so shares nothing,
     * and with one vertex outside has four more (12). */
    polygon_setup(CULL_NONE | OPAQUE);
    cmd(BEGIN_VTXS, 2);
    vtx16(0, 0, 0);
    vtx16(0, 0x800, 0);
    vtx16(0x400, 0, 0);
    EQ("strip: first triangle", vertices(), 3);
    vtx16(0x400, 0x800, 0);
    EQ("strip: second shares two", vertices(), 4);
    vtx16(0x1800, 0, 0);
    EQ("strip: third clipped, on its own", vertices(), 8);
    EQ("strip: third has four vertices", last_polygon()->count, 4);
    vtx16(0x800, 0x800, 0);
    EQ("strip: fourth after a clipped one, on its own", vertices(), 12);
    EQ("strip: four polygons", polygons(), 4);
    /* And a strip resumes sharing once a polygon is whole again. */
    vtx16(0x800, 0, 0);
    EQ("strip: fifth clipped too (shares the outside vertex)", vertices(), 16);
    vtx16(0xC00, 0x800, 0);
    vtx16(0xC00, 0, 0);
    {
        unsigned before = vertices();
        vtx16(0xE00, 0x800, 0);
        EQ("strip: sharing resumes", vertices(), before + 1);
    }
}

static void test_one_dot(void)
{
    g_section = "one dot";

    /* DISP_1DOT_DEPTH (GBATEK): a polygon whose vertices are all on one
     * pixel is hidden if its W is beyond the limit, "W-Coordinate
     * (Unsigned, 12bit integer, 3bit fractional part)", unless
     * POLYGON_ATTR bit 13 is set. W = 1.0 here, which is 8 in 12.3. */
    for (int enable = 0; enable < 2; enable++) {
        uint32_t attr = CULL_NONE | OPAQUE | (enable ? 1u << 13 : 0);
        polygon_setup(attr);
        triangle(0, 0, 1, 0, 0, -1);
        EQ("limit at reset is the farthest", polygons(), 1);
        polygon_setup(attr);
        nds_gx_write(&gx, 0x04000610, 7, 2);
        triangle(0, 0, 1, 0, 0, -1);
        EQ("one dot beyond the limit", polygons(), enable);
        polygon_setup(attr);
        nds_gx_write(&gx, 0x04000610, 8, 2);
        triangle(0, 0, 1, 0, 0, -1);
        EQ("one dot at the limit", polygons(), 1);
        /* A polygon that covers more than a pixel is not a dot. */
        polygon_setup(attr);
        nds_gx_write(&gx, 0x04000610, 0, 2);
        triangle(0, 0, 0x100, 0, 0, 0x100);
        EQ("larger polygon beyond the limit", polygons(), 1);
    }
}

static void test_limits(void)
{
    g_section = "limits";

    /* GBATEK RAM_COUNT: "Number of Polygons currently stored in Polygon
     * List RAM (0..2048)", "Number of Vertices currently stored in Vertex
     * RAM (0..6144)"; DISP3DCNT bit 13 "Polygon/Vertex RAM Overflow
     * (0=None, 1=Overflow/Acknowledge)". */

    /* Polygons: one strip of 2049 triangles shares vertices, so only the
     * polygon limit is in the way. */
    polygon_setup(CULL_NONE | OPAQUE);
    cmd(BEGIN_VTXS, 2);
    for (int i = 0; i < 2048 + 2; i++) vtx16((i & 1) ? 0x100 : 0, (i & 2) ? 0x100 : 0, 0);
    EQ("2048 polygons", polygons(), 2048);
    EQ("2050 vertices", vertices(), 2050);
    EQ("no overflow yet", r32(0x04000060) & 0x2000u, 0);
    vtx16(0x200, 0x200, 0);
    EQ("the 2049th is refused", polygons(), 2048);
    EQ("and adds no vertices", vertices(), 2050);
    EQ("overflow flag", r32(0x04000060) & 0x2000u, 0x2000u);
    w32(0x04000060, 0);
    EQ("writing 0 leaves the flag", r32(0x04000060) & 0x2000u, 0x2000u);
    w32(0x04000060, 0x2000u);
    EQ("writing 1 acknowledges it", r32(0x04000060) & 0x2000u, 0);

    /* Vertices: 1536 separate quads fill vertex RAM exactly. */
    polygon_setup(CULL_NONE | OPAQUE);
    cmd(BEGIN_VTXS, 1);
    for (int i = 0; i < 1536; i++) {
        vtx16(0, 0, 0);
        vtx16(0x100, 0, 0);
        vtx16(0x100, 0x100, 0);
        vtx16(0, 0x100, 0);
    }
    EQ("6144 vertices", vertices(), 6144);
    EQ("1536 polygons", polygons(), 1536);
    EQ("exactly full: no overflow", r32(0x04000060) & 0x2000u, 0);
    cmd(BEGIN_VTXS, 0);
    triangle(0, 0, 0x100, 0, 0, 0x100);
    EQ("no room for a triangle", polygons(), 1536);
    EQ("vertex count unchanged", vertices(), 6144);
    EQ("vertex overflow flag", r32(0x04000060) & 0x2000u, 0x2000u);

    /* polyrastertest (tests.h, "Vertex Limit"): 1533 quads and 3
     * triangles leave room for three vertices. A quad that clipping cuts
     * down to a triangle fits: "This should render the polygon due it it
     * barely fitting in the limit thanks to clipping"; a whole quad does
     * not: "This should not render due to it just barely overflowing the
     * limit". So the count that matters is the clipped polygon's. */
    for (int whole = 0; whole < 2; whole++) {
        polygon_setup(CULL_NONE | OPAQUE);
        cmd(BEGIN_VTXS, 1);
        for (int i = 0; i < 1533; i++) {
            vtx16(0, 0, 0);
            vtx16(0x100, 0, 0);
            vtx16(0x100, 0x100, 0);
            vtx16(0, 0x100, 0);
        }
        cmd(BEGIN_VTXS, 0);
        for (int i = 0; i < 3; i++) triangle(0, 0, 0x100, 0, 0, 0x100);
        EQ("6141 vertices", vertices(), 6141);
        cmd(BEGIN_VTXS, 1);
        if (whole) {
            vtx16(-0x200, 0, 0);
            vtx16(0, -0x200, 0);
            vtx16(0x200, 0, 0);
            vtx16(0, 0x200, 0);
            EQ("a whole quad does not fit", polygons(), 1536);
            EQ("whole quad: overflow", r32(0x04000060) & 0x2000u, 0x2000u);
        } else {
            /* A diamond with three corners beyond the right plane. */
            vtx16(0xC00, 0, 0);
            vtx16(0x1400, -0x800, 0);
            vtx16(0x1C00, 0, 0);
            vtx16(0x1400, 0x800, 0);
            EQ("a quad clipped to a triangle fits", polygons(), 1537);
            EQ("vertex RAM exactly full", vertices(), 6144);
            EQ("clipped quad: no overflow", r32(0x04000060) & 0x2000u, 0);
        }
    }
}

static void test_swap(void)
{
    g_section = "swap";

    /* GBATEK "Swap Buffers": "the newly defined polygons/vertices are
     * passed to the rendering engine ... The other buffer is emptied, and
     * passed to the Geometry Engine". "SwapBuffers isn't executed until next
     * VBlank". */
    polygon_setup(CULL_NONE | OPAQUE);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    w32(PORT(SWAP_BUFFERS), 0);
    drain();
    EQ("before VBlank: still counted", polygons(), 2);
    EQ("before VBlank: nothing to render", gx.render_count, 0);
    w32(PORT(BEGIN_VTXS), 0);
    w32(PORT(VTX_16), 0);
    w32(PORT(VTX_16), 0);
    drain();
    EQ("commands after the swap wait", vertices(), 6);
    vblank();
    EQ("after VBlank: rendering has two", gx.render_count, 2);
    EQ("after VBlank: polygon RAM empty", polygons(), 0);
    /* The vertex sent after the swap went into the new list. */
    vtx16(0x400, 0, 0);
    vtx16(0, 0x400, 0);
    EQ("the new list", polygons(), 1);
    EQ("the new list's vertices", vertices(), 3);
    /* Without a swap, VBlank changes nothing. */
    vblank();
    EQ("VBlank without swap: list kept", polygons(), 1);
    EQ("VBlank without swap: rendering keeps its list", gx.render_count, 2);

    /* The order polygons are drawn in. Opaque polygons first, sorted by
     * bottom line, then top line, then as sent (melonDS); translucent
     * polygons after them ("translucent polygons are always rendered last,
     * even with auto-sort disabled"), sorted the same way unless
     * SWAP_BUFFERS bit 0 ("Translucent polygon Y-sorting (0=Auto-sort,
     * 1=Manual-sort)") of the swap before said otherwise.
     *
     * Five polygons, in this order: translucent low on the screen;
     * opaque low; translucent high; opaque high; opaque high again. */
    for (int manual = 0; manual < 2; manual++) {
        fresh();
        cmd(VIEWPORT, (255u << 16) | (191u << 24));
        cmd(SWAP_BUFFERS, (uint32_t)manual);
        vblank();
        static const struct {
            uint32_t alpha;
            int y;
        } list[5] = {{15, -0x800}, {31, -0x800}, {15, 0x800}, {31, 0x800}, {31, 0x800}};
        for (int i = 0; i < 5; i++) {
            cmd(POLYGON_ATTR, CULL_NONE | (list[i].alpha << 16));
            cmd(BEGIN_VTXS, 0);
            triangle(0, list[i].y, 0x400, list[i].y, 0, list[i].y - 0x400);
        }
        cmd(SWAP_BUFFERS, 0);
        vblank();
        EQ("five to render", gx.render_count, 5);
        /* y = 0x800 is higher on the screen: smaller lines, drawn first. */
        EQ("opaque, high, first sent", gx.render_order[0], 3);
        EQ("opaque, high, second sent", gx.render_order[1], 4);
        EQ("opaque, low", gx.render_order[2], 1);
        EQ("translucent first", gx.render_order[3], manual ? 0 : 2);
        EQ("translucent second", gx.render_order[4], manual ? 2 : 0);
    }

    /* SWAP_BUFFERS bit 1, "Depth Buffering (0=With Z-value, 1=With
     * W-value)", for the polygons sent after it. Z at the middle of the
     * view volume: (0 * 0x4000 / w + 0x3FFF) * 0x200 (melonDS's reading of
     * hardware; GBATEK has no formula). W = 1.0. */
    fresh();
    cmd(POLYGON_ATTR, CULL_NONE | OPAQUE);
    cmd(BEGIN_VTXS, 0);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    EQ("Z-buffering: depth from Z", last_polygon()->depth[0], 0x3FFFu * 0x200u);
    OK("Z-buffering flag", !(last_polygon()->flags & NDS_GX_POLY_W_BUFFER));
    cmd(SWAP_BUFFERS, 2);
    vblank();
    cmd(BEGIN_VTXS, 0);
    triangle(0, 0, 0x400, 0, 0, 0x400);
    EQ("W-buffering: depth is W", last_polygon()->depth[0], 0x1000);
    OK("W-buffering flag", last_polygon()->flags & NDS_GX_POLY_W_BUFFER);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void box_test(int x, int y, int z, int w, int h, int d)
{
    w32(PORT(BOX_TEST), ((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
    w32(PORT(BOX_TEST), ((uint32_t)(uint16_t)w << 16) | (uint16_t)z);
    w32(PORT(BOX_TEST), ((uint32_t)(uint16_t)d << 16) | (uint16_t)h);
    drain();
}

static void test_tests(void)
{
    g_section = "tests";

    /* POS_TEST: "Multiplies the specified line-vector (x,y,z,1) by the clip
     * coordinate matrix." A translation and a scale, by hand. */
    {
        static const int32_t m[16] = {2 * ONE, 0, 0, 0, 0, 3 * ONE, 0, 0, 0, 0, ONE / 2, ONE, 0x100, 0x200, 0x300, 0x400};
        fresh();
        cmd(MTX_MODE, 0);
        cmdn(MTX_LOAD_4x4, m, 16);
        w32(PORT(POS_TEST), (0x0800u << 16) | 0x0400u);
        w32(PORT(POS_TEST), 0xFE00u);
        drain();
        EQ("POS_RESULT x", (int32_t)r32(0x04000620), 2 * 0x400 + 0x100);
        EQ("POS_RESULT y", (int32_t)r32(0x04000624), 3 * 0x800 + 0x200);
        EQ("POS_RESULT z", (int32_t)r32(0x04000628), -0x200 / 2 + 0x300);
        EQ("POS_RESULT w", (int32_t)r32(0x0400062C), -0x200 + 0x400);
        /* "POS_TEST overwrites the internal VTX registers". */
        EQ("vertex x overwritten", gx.vertex[0], 0x400);
        EQ("vertex z overwritten", gx.vertex[2], -0x200);
    }

    /* VEC_TEST: "Multiplies the specified line-vector (x,y,z,0) by the
     * directional vector matrix"; the result is "4bit sign, 0bit integer,
     * 12bit fractional part". The identity: 1.9 becomes 1.12. */
    fresh();
    cmd(MTX_MODE, 2);
    cmd(VEC_TEST, VEC10(0x100, -0x80, 0x1FF));
    EQ("VEC_RESULT x", (int16_t)nds_gx_read(&gx, 0x04000630, 2), 0x100 << 3);
    EQ("VEC_RESULT y", (int16_t)nds_gx_read(&gx, 0x04000632, 2), -(0x80 << 3));
    EQ("VEC_RESULT z", (int16_t)nds_gx_read(&gx, 0x04000634, 2), 0x1FF << 3);
    /* "+1.0 aka 1000h will be returned as -1.0 aka F000h due to overflow
     * and sign-expansion": 0.5 through a matrix that doubles. */
    {
        static const int32_t twice[9] = {2 * ONE, 0, 0, 0, 2 * ONE, 0, 0, 0, 2 * ONE};
        cmdn(MTX_MULT_3x3, twice, 9);
        cmd(VEC_TEST, VEC10(0x100, 0x80, 0));
        EQ("VEC_RESULT overflow", nds_gx_read(&gx, 0x04000630, 2), 0xF000u);
        EQ("VEC_RESULT in range", nds_gx_read(&gx, 0x04000632, 2), 0x0800u);
    }

    /* BOX_TEST: "indicates if one or more of the 6 faces of the box are
     * fully or parts of inside of the view volume"; "it will return false if
     * the whole view volume is located inside of the box". The view volume
     * is -1.0..1.0 here. GBATEK also says to enable far-plane-intersecting
     * polygons first; without that a box across the far plane is rejected. */
    fresh();
    cmd(POLYGON_ATTR, (1u << 12) | (1u << 13));
    cmd(BEGIN_VTXS, 0);
    box_test(-0x400, -0x400, -0x400, 0x800, 0x800, 0x800);
    EQ("box inside", (r32(GXSTAT) >> 1) & 1u, 1);
    box_test(0x1800, 0, 0, 0x400, 0x400, 0x400);
    EQ("box outside", (r32(GXSTAT) >> 1) & 1u, 0);
    box_test(0x0C00, 0, 0, 0x800, 0x400, 0x400);
    EQ("box across the right plane", (r32(GXSTAT) >> 1) & 1u, 1);
    box_test(-0x2000, -0x2000, -0x2000, 0x4000, 0x4000, 0x4000);
    EQ("box around the view volume", (r32(GXSTAT) >> 1) & 1u, 0);
    box_test(0, 0, 0x0C00, 0x400, 0x400, 0x800);
    EQ("box across the far plane", (r32(GXSTAT) >> 1) & 1u, 1);
    /* A thin box right through the view volume from in front of the near
     * plane to behind the far one: neither end face is in view, and its
     * four long faces all cross the far plane. They count only if polygons
     * that cross the far plane are drawn (POLYGON_ATTR bit 12, as of the
     * last BEGIN_VTXS). */
    box_test(-0x400, -0x400, -0x2000, 0x800, 0x800, 0x4000);
    EQ("long box, far-plane polygons drawn", (r32(GXSTAT) >> 1) & 1u, 1);
    cmd(POLYGON_ATTR, 0);
    box_test(-0x400, -0x400, -0x2000, 0x800, 0x800, 0x4000);
    EQ("POLYGON_ATTR not applied before BEGIN_VTXS", (r32(GXSTAT) >> 1) & 1u, 1);
    cmd(BEGIN_VTXS, 0);
    box_test(-0x400, -0x400, -0x2000, 0x800, 0x800, 0x4000);
    EQ("long box, far-plane polygons hidden", (r32(GXSTAT) >> 1) & 1u, 0);
    /* A box whose near face is in view still shows. */
    box_test(0, 0, 0x0C00, 0x400, 0x400, 0x800);
    EQ("far-plane polygons hidden: the near face counts", (r32(GXSTAT) >> 1) & 1u, 1);
}

/* ------------------------------------------------------------------ */
/* Robustness                                                          */
/* ------------------------------------------------------------------ */

static uint32_t g_seed = 0x1234ABCDu;

static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

/* A value with interesting bits more often than a uniform one. Here and
 * below no expression draws two random numbers: the order in which a compiler
 * evaluates operands and arguments is its own choice, and the run has to be
 * the same one everywhere. */
static uint32_t rnd_value(void)
{
    uint32_t low;
    switch (rnd() % 6) {
    case 0: return 0;
    case 1: return 0xFFFFFFFFu;
    case 2: return 0x80000000u >> (rnd() % 32);
    case 3: return rnd() & 0xFFFFu;
    case 4:
        low = rnd() & 0x1FFFu;
        return low | ((rnd() & 0x1FFFu) << 16);
    default: return rnd();
    }
}

static void test_garbage(void)
{
    NdsGxVram vram, partial;
    uint8_t *memory[10];
    g_section = "garbage";

    /* Nothing a program can write may take the module out of bounds: random
     * register writes of every size, random commands through every port and
     * packed through GXFIFO, with swaps, VBlanks and renders in between.
     * The address sanitizer build is what gives this teeth; here it must
     * simply keep its counts in range. Texture memory is random too, in
     * blocks of exactly the slots' sizes, so that a read past a slot is
     * caught; some renders get it whole, some with slots missing, some
     * with none. */
    for (int i = 0; i < 10; i++) {
        size_t size = i < 4 ? 0x20000u : 0x4000u;
        memory[i] = (uint8_t *)malloc(size);
        if (!memory[i]) {
            OK("memory for the texture slots", 0);
            return;
        }
        for (size_t k = 0; k < size; k++) memory[i][k] = (uint8_t)(rnd() >> 11);
    }
    for (int i = 0; i < 4; i++) vram.texture[i] = memory[i];
    for (int i = 0; i < 6; i++) vram.palette[i] = memory[4 + i];
    partial = vram;
    partial.texture[1] = partial.texture[3] = NULL;
    partial.palette[0] = partial.palette[5] = NULL;

    fresh();
    for (int i = 0; i < 150000; i++) {
        unsigned what = rnd() % 100;
        if (what < 40) {
            /* a command port, biased to the ones that do things */
            static const uint8_t likely[] = {MTX_MODE, MTX_PUSH, MTX_POP, MTX_STORE, MTX_RESTORE, MTX_IDENTITY,
                                             MTX_SCALE, MTX_TRANS, COLOR, NORMAL, TEXCOORD, VTX_10, VTX_XY, VTX_XZ,
                                             VTX_YZ, VTX_DIFF, POLYGON_ATTR, TEXIMAGE_PARAM, PLTT_BASE, DIF_AMB,
                                             SPE_EMI, LIGHT_VECTOR, LIGHT_COLOR, BEGIN_VTXS, END_VTXS, VIEWPORT,
                                             VEC_TEST, VTX_16, VTX_16, VTX_10, VTX_10, VTX_10};
            unsigned code = likely[rnd() % sizeof likely];
            w32(PORT(code), rnd_value());
        } else if (what < 55) {
            unsigned code = 0x10 + rnd() % 0x70;
            w32(PORT(code), rnd_value());
        } else if (what < 70) {
            uint32_t addr = GXFIFO + 4 * (rnd() % 16);
            uint32_t value;
            if (rnd() % 3) value = rnd_value();
            else value = rnd() & 0x7F7F7F7Fu;
            w32(addr, value);
        } else if (what < 85) {
            static const int sizes[3] = {1, 2, 4};
            uint32_t addr, value;
            int size;
            if (rnd() % 3 == 0) addr = 0x04000060u + rnd() % 4;
            else if (rnd() % 2) addr = 0x04000320u + rnd() % 0xA0;
            else addr = 0x04000600u + rnd() % 0xA4;
            value = rnd_value();
            size = sizes[rnd() % 3];
            nds_gx_write(&gx, addr, value, size);
            size = sizes[rnd() % 3];
            (void)nds_gx_read(&gx, addr, size);
        } else if (what < 97) {
            run(rnd() % 3000);
        } else if (what < 99) {
            w32(PORT(SWAP_BUFFERS), rnd());
            run(rnd() % 5000);
            nds_gx_vblank(&gx);
        } else if (rnd() % 3 == 0) {
            unsigned which = rnd() % 4;
            nds_gx_vblank(&gx);
            nds_gx_render(&gx, which == 0 ? NULL : (which == 1 ? &partial : &vram));
            (void)nds_gx_line(&gx, (int)(rnd() % 400) - 100);
        }
        if (gx.polygon_count > NDS_GX_MAX_POLYGONS || gx.vertex_count > NDS_GX_MAX_VERTICES ||
            gx.queue_count > NDS_GX_QUEUE_ENTRIES || gx.pipe_count > NDS_GX_PIPE_ENTRIES ||
            gx.fifo_count > NDS_GX_FIFO_ENTRIES || gx.exec_count >= 32 || gx.position_sp > 63) {
            OK("counts in range", 0);
            return;
        }
    }
    OK("survived 150,000 random operations", 1);
    /* Every SWAP_BUFFERS left in the queue needs a VBlank of its own. */
    for (int i = 0; i <= NDS_GX_QUEUE_ENTRIES && (r32(GXSTAT) & (1u << 27)); i++) {
        drain();
        nds_gx_vblank(&gx);
    }
    drain();
    EQ("and drains to idle", r32(GXSTAT) & (1u << 27), 0);
    for (int i = 0; i < 10; i++) free(memory[i]);

    /* Writing on while the FIFO is full, which a machine must not do (the
     * hardware holds the CPU instead). The module's own contract (nds_gx.h):
     * it keeps NDS_GX_QUEUE_ENTRIES entries in all, the PIPE and the FIFO
     * and what waits behind them; what does not fit even there is dropped
     * and counted in "dropped", never stored out of bounds. SWAP_BUFFERS
     * halts the engine, so nothing is taken while these are written. */
    fresh();
    w32(PORT(SWAP_BUFFERS), 0);
    for (int i = 0; i < NDS_GX_QUEUE_ENTRIES; i++) w32(PORT(MTX_MODE), 0);
    EQ("a whole queue: nothing dropped", gx.dropped, 0);
    for (int i = 0; i < 100; i++) w32(PORT(MTX_MODE), 0);
    EQ("100 entries more than the queue holds: 100 dropped", gx.dropped, 100);
    EQ("the FIFO still reports 256", (r32(GXSTAT) >> 16) & 0x1FFu, 256);
    OK("and full", nds_gx_fifo_full(&gx));
    nds_gx_vblank(&gx);
    drain();
    EQ("what was kept runs, and the engine is idle", r32(GXSTAT) & (1u << 27), 0);
    EQ("the FIFO is empty", (r32(GXSTAT) >> 16) & 0x1FFu, 0);
    OK("and no longer full", !nds_gx_fifo_full(&gx));
}

int main(void)
{
    test_registers();
    test_fifo();
    test_irq();
    test_timing();
    test_packed();
    test_matrices();
    test_stacks();
    test_lighting();
    test_texcoords();
    test_vertices();
    test_viewport();
    test_culling();
    test_clipping();
    test_one_dot();
    test_limits();
    test_swap();
    test_tests();
    test_garbage();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
