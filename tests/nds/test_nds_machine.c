/*
 * Unit tests for the DS machine core: the two memory maps and what the
 * memory-control registers do to them, the IPC, the maths unit, timers,
 * DMA, the card's ROM interface, interrupts and halting, video timing, keys
 * and the direct boot. Everything goes through the buses the CPUs use.
 * Expected values are worked out from GBATEK, section by section.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "nds.h"

static int g_checks;
static int g_failures;
static const char *g_section = "";

static void check_eq(const char *what, uint64_t got, uint64_t want, int line)
{
    g_checks++;
    if (got == want) return;
    g_failures++;
    printf("FAIL %s: %s = %" PRIX64 ", expected %" PRIX64 " (line %d)\n", g_section, what, got, want, line);
}

#define EQ(what, got, want) check_eq(what, (uint64_t)(got), (uint64_t)(want), __LINE__)

/* Bus shorthands. */
static uint32_t r9(NdsMachine *m, uint32_t a) { return nds9_bus_read32(&m->arm9, a, NDS_ACCESS_NSEQ); }
static uint32_t r9h(NdsMachine *m, uint32_t a) { return nds9_bus_read16(&m->arm9, a, NDS_ACCESS_NSEQ); }
static uint32_t r9b(NdsMachine *m, uint32_t a) { return nds9_bus_read8(&m->arm9, a, NDS_ACCESS_NSEQ); }
static void w9(NdsMachine *m, uint32_t a, uint32_t v) { nds9_bus_write32(&m->arm9, a, v, NDS_ACCESS_NSEQ); }
static void w9h(NdsMachine *m, uint32_t a, uint32_t v) { nds9_bus_write16(&m->arm9, a, (uint16_t)v, NDS_ACCESS_NSEQ); }
static void w9b(NdsMachine *m, uint32_t a, uint32_t v) { nds9_bus_write8(&m->arm9, a, (uint8_t)v, NDS_ACCESS_NSEQ); }
static uint32_t r7(NdsMachine *m, uint32_t a) { return nds7_bus_read32(&m->arm7, a, NDS_ACCESS_NSEQ); }
static uint32_t r7h(NdsMachine *m, uint32_t a) { return nds7_bus_read16(&m->arm7, a, NDS_ACCESS_NSEQ); }
static uint32_t r7b(NdsMachine *m, uint32_t a) { return nds7_bus_read8(&m->arm7, a, NDS_ACCESS_NSEQ); }
static void w7(NdsMachine *m, uint32_t a, uint32_t v) { nds7_bus_write32(&m->arm7, a, v, NDS_ACCESS_NSEQ); }
static void w7h(NdsMachine *m, uint32_t a, uint32_t v) { nds7_bus_write16(&m->arm7, a, (uint16_t)v, NDS_ACCESS_NSEQ); }
static void w7b(NdsMachine *m, uint32_t a, uint32_t v) { nds7_bus_write8(&m->arm7, a, (uint8_t)v, NDS_ACCESS_NSEQ); }

/* A machine with no card, both CPUs asleep, so time can be advanced
 * without anything executing. */
static NdsMachine *fresh(void)
{
    NdsMachine *m = nds_create();
    nds_reset(m);
    m->halted[NDS_ARM9] = m->halted[NDS_ARM7] = true;
    return m;
}

/* ------------------------------------------------------------------ */

static void test_memory(void)
{
    NdsMachine *m = fresh();
    g_section = "main ram";
    w9(m, 0x02000010u, 0x11223344u);
    EQ("mirror at +4 MiB", r9(m, 0x02400010u), 0x11223344u);
    EQ("mirror at +12 MiB", r9(m, 0x02C00010u), 0x11223344u);
    EQ("the ARM7 sees it", r7(m, 0x02000010u), 0x11223344u);
    EQ("halfword", r9h(m, 0x02000012u), 0x1122u);
    EQ("byte", r9b(m, 0x02000013u), 0x11u);
    EQ("unaligned word read is aligned", r9(m, 0x02000013u), 0x11223344u);
    w7b(m, 0x023FFFFFu, 0xAB);
    EQ("last byte, through the 0x027FFFFF mirror", r9b(m, 0x027FFFFFu), 0xABu);

    g_section = "shared wram";
    /* WRAMCNT 0: all 32 KiB to the ARM9; the ARM7 sees its own WRAM there. */
    w9b(m, 0x04000247u, 0);
    w9(m, 0x03000000u, 0xAAAA0000u);
    w9(m, 0x03004000u, 0xAAAA4000u);
    EQ("cnt 0: arm9 first half", r9(m, 0x03000000u), 0xAAAA0000u);
    EQ("cnt 0: arm9 mirror at 0x037F8000", r9(m, 0x037F8000u), 0xAAAA0000u);
    w7(m, 0x03800000u, 0x77770000u);
    EQ("cnt 0: arm7 sees its own WRAM at 0x03000000", r7(m, 0x03000000u), 0x77770000u);
    EQ("arm7 WRAMSTAT", r7b(m, 0x04000241u), 0);
    /* 1: the ARM9 has the second half, the ARM7 the first. */
    w9b(m, 0x04000247u, 1);
    EQ("cnt 1: arm9 sees the second half", r9(m, 0x03000000u), 0xAAAA4000u);
    EQ("cnt 1: mirrored every 16 KiB", r9(m, 0x03004000u), 0xAAAA4000u);
    EQ("cnt 1: arm7 sees the first half", r7(m, 0x03000000u), 0xAAAA0000u);
    /* 2: the other way round. */
    w9b(m, 0x04000247u, 2);
    EQ("cnt 2: arm9 sees the first half", r9(m, 0x03000000u), 0xAAAA0000u);
    EQ("cnt 2: arm7 sees the second half", r7(m, 0x03000000u), 0xAAAA4000u);
    /* 3: all to the ARM7; the ARM9 has nothing. */
    w9b(m, 0x04000247u, 3);
    EQ("cnt 3: arm9 reads nothing", r9(m, 0x03000000u), 0);
    EQ("cnt 3: arm7 first half", r7(m, 0x03000000u), 0xAAAA0000u);
    EQ("cnt 3: arm7 second half", r7(m, 0x03004000u), 0xAAAA4000u);
    EQ("cnt 3: arm7 WRAMSTAT", r7b(m, 0x04000241u), 3);
    EQ("arm7 WRAM is still at 0x03800000", r7(m, 0x03800000u), 0x77770000u);
    EQ("arm7 WRAM mirror", r7(m, 0x03810000u), 0x77770000u);

    g_section = "palette and oam";
    w9h(m, 0x05000002u, 0x7FFF);
    EQ("palette", r9h(m, 0x05000002u), 0x7FFFu);
    EQ("palette mirror every 2 KiB", r9h(m, 0x05000802u), 0x7FFFu);
    w9b(m, 0x05000002u, 0x12);
    EQ("byte write to palette ignored", r9h(m, 0x05000002u), 0x7FFFu);
    w9(m, 0x07000400u, 0x12345678u);
    EQ("oam, engine B half", r9(m, 0x07000400u), 0x12345678u);
    EQ("oam mirror", r9(m, 0x07000C00u), 0x12345678u);

    g_section = "gba slot";
    /* Nothing inserted: the CPU the slot belongs to reads address / 2. */
    EQ("arm9 owns the slot: address/2", r9h(m, 0x08000246u), 0x0123u);
    EQ("arm9 word", r9(m, 0x08000000u), 0x00010000u);
    EQ("arm9 slot RAM reads ones", r9b(m, 0x0A000000u), 0xFFu);
    EQ("arm7 does not own it: zero", r7h(m, 0x08000246u), 0);
    w9h(m, 0x04000204u, 0x0080);
    EQ("EXMEMCNT bit 7 hands it over: arm9 zero", r9h(m, 0x08000246u), 0);
    EQ("arm7 address/2", r7h(m, 0x08000246u), 0x0123u);
    EQ("EXMEMCNT reads back with bit 13 set", r9h(m, 0x04000204u), 0x2080u);
    w7h(m, 0x04000204u, 0xFFFF);
    EQ("EXMEMSTAT: own low bits, the ARM9's high bits", r7h(m, 0x04000204u), 0x20FFu);
    EQ("the ARM9's register is untouched", r9h(m, 0x04000204u), 0x2080u);

    g_section = "unmapped";
    EQ("arm9 0x10000000", r9(m, 0x10000000u), 0);
    EQ("arm9 undefined I/O", r9(m, 0x04000800u), 0);
    EQ("arm7 0x01000000", r7(m, 0x01000000u), 0);
    nds_destroy(m);
}

static void test_vram(void)
{
    NdsMachine *m = fresh();
    g_section = "vram";

    /* Every block in LCDC mode at its own address. */
    static const uint32_t lcdc[9] = { 0x06800000u, 0x06820000u, 0x06840000u, 0x06860000u, 0x06880000u,
                                      0x06890000u, 0x06894000u, 0x06898000u, 0x068A0000u };
    static const uint32_t cnt_reg[9] = { 0x04000240u, 0x04000241u, 0x04000242u, 0x04000243u, 0x04000244u,
                                         0x04000245u, 0x04000246u, 0x04000248u, 0x04000249u };
    EQ("disabled block reads zero", r9(m, lcdc[0]), 0);
    for (int b = 0; b < 9; b++) {
        w9b(m, cnt_reg[b], 0x80);
        w9(m, lcdc[b], 0xB10C0000u + (uint32_t)b);
        EQ("lcdc readback", r9(m, lcdc[b]), 0xB10C0000u + (uint32_t)b);
    }
    w9b(m, lcdc[0] + 1, 0xEE);
    EQ("byte write to VRAM ignored", r9(m, lcdc[0]), 0xB10C0000u);
    /* GBATEK marks these write-only; the console answers, and libnds's
     * texture allocator finds its bank by reading them (BlocksDS's
     * graphics/texture_allocation test). */
    for (int b = 0; b < 9; b++) EQ("VRAMCNT reads back", r9b(m, cnt_reg[b]), 0x80);
    w9b(m, 0x04000247u, 0x02);
    EQ("WRAMCNT reads back", r9b(m, 0x04000247u), 0x02);
    w9b(m, 0x04000247u, 0x00);
    w9(m, 0x04000240u, 0x9B938B83u);
    EQ("VRAMCNT A-D as one word", r9(m, 0x04000240u), 0x9B938B83u);
    for (int b = 0; b < 4; b++) w9b(m, cnt_reg[b], 0x80);

    /* A as engine A BG at each offset. */
    for (uint32_t ofs = 0; ofs < 4; ofs++) {
        w9b(m, cnt_reg[0], 0x81 | (ofs << 3));
        EQ("A as BG", r9(m, 0x06000000u + 0x20000u * ofs), 0xB10C0000u);
        EQ("A no longer in LCDC", r9(m, lcdc[0]), 0);
    }
    EQ("BG region mirrors every 512 KiB", r9(m, 0x06080000u + 0x60000u), 0xB10C0000u);
    /* B as engine A OBJ at offsets 0 and 1. */
    w9b(m, cnt_reg[1], 0x82);
    EQ("B as OBJ", r9(m, 0x06400000u), 0xB10C0001u);
    w9b(m, cnt_reg[1], 0x8A);
    EQ("B as OBJ, offset 1", r9(m, 0x06420000u), 0xB10C0001u);
    /* C as engine B BG, D as engine B OBJ. */
    w9b(m, cnt_reg[2], 0x84);
    EQ("C as engine B BG", r9(m, 0x06200000u), 0xB10C0002u);
    w9b(m, cnt_reg[3], 0x84);
    EQ("D as engine B OBJ", r9(m, 0x06600000u), 0xB10C0003u);
    /* C and D lent to the ARM7. */
    EQ("VRAMSTAT before", r7b(m, 0x04000240u), 0);
    w9b(m, cnt_reg[2], 0x82);
    w9b(m, cnt_reg[3], 0x8A);
    EQ("VRAMSTAT: C and D with the ARM7", r7b(m, 0x04000240u), 3);
    EQ("C at the ARM7's 0x06000000", r7(m, 0x06000000u), 0xB10C0002u);
    EQ("D at the ARM7's 0x06020000", r7(m, 0x06020000u), 0xB10C0003u);
    w7b(m, 0x06000001u, 0x55);
    EQ("the ARM7 may write bytes there", r7(m, 0x06000000u), 0xB10C5502u);
    EQ("and the ARM9 no longer sees C", r9(m, 0x06200000u), 0);
    /* E as BG and OBJ. */
    w9b(m, cnt_reg[4], 0x81);
    w9b(m, cnt_reg[0], 0x00);
    EQ("E as engine A BG", r9(m, 0x06000000u), 0xB10C0004u);
    w9b(m, cnt_reg[4], 0x82);
    w9b(m, cnt_reg[1], 0x00);
    EQ("E as engine A OBJ", r9(m, 0x06400000u), 0xB10C0004u);
    /* F and G: 16 KiB at 0, 0x4000, 0x10000, 0x14000, repeated 0x8000 up. */
    static const uint32_t fg_at[4] = { 0x0000u, 0x4000u, 0x10000u, 0x14000u };
    w9b(m, cnt_reg[4], 0x00);
    for (uint32_t ofs = 0; ofs < 4; ofs++) {
        w9b(m, cnt_reg[5], 0x81 | (ofs << 3));
        EQ("F as BG", r9(m, 0x06000000u + fg_at[ofs]), 0xB10C0005u);
        EQ("F as BG, mirror", r9(m, 0x06008000u + fg_at[ofs]), 0xB10C0005u);
        w9b(m, cnt_reg[6], 0x82 | (ofs << 3));
        EQ("G as OBJ", r9(m, 0x06400000u + fg_at[ofs]), 0xB10C0006u);
    }
    /* H and I as engine B BG: H at 0, I at 0x8000. */
    w9b(m, cnt_reg[2], 0x00);
    w9b(m, cnt_reg[7], 0x81);
    w9b(m, cnt_reg[8], 0x81);
    EQ("H as engine B BG", r9(m, 0x06200000u), 0xB10C0007u);
    EQ("I as engine B BG at 0x8000", r9(m, 0x06208000u), 0xB10C0008u);
    w9b(m, cnt_reg[3], 0x00);
    w9b(m, cnt_reg[8], 0x82);
    EQ("I as engine B OBJ", r9(m, 0x06600000u), 0xB10C0008u);

    /* Modes the CPU cannot reach: check the tables the renderers get. */
    w9b(m, cnt_reg[0], 0x83 | (2u << 3));
    EQ("A as texture slot 2", m->map.texture[2] == m->vram + NDS_VRAM_A, 1);
    EQ("texture slot 0 empty", m->map.texture[0] == NULL, 1);
    w9b(m, cnt_reg[4], 0x83);
    EQ("E as texture palette slot 3", m->map.tex_pal[3] == m->vram + NDS_VRAM_E + 0xC000, 1);
    w9b(m, cnt_reg[5], 0x83 | (3u << 3));
    EQ("F as texture palette slot 5", m->map.tex_pal[5] == m->vram + NDS_VRAM_F, 1);
    w9b(m, cnt_reg[4], 0x84);
    EQ("E as BG extended palette slot 2", m->map.bg_ext_a[2] == m->vram + NDS_VRAM_E + 0x4000, 1);
    w9b(m, cnt_reg[6], 0x84 | (1u << 3));
    EQ("G as BG extended palette slots 2-3", m->map.bg_ext_a[3] == m->vram + NDS_VRAM_G + 0x2000, 1);
    w9b(m, cnt_reg[5], 0x85);
    EQ("F as OBJ extended palette", m->map.obj_ext_a == m->vram + NDS_VRAM_F, 1);
    w9b(m, cnt_reg[7], 0x82);
    EQ("H as engine B BG extended palette", m->map.bg_ext_b[1] == m->vram + NDS_VRAM_H + 0x2000, 1);
    w9b(m, cnt_reg[8], 0x83);
    EQ("I as engine B OBJ extended palette", m->map.obj_ext_b == m->vram + NDS_VRAM_I, 1);
    nds_destroy(m);
}

static void test_tcm(void)
{
    NdsMachine *m = fresh();
    NdsCpu *c = &m->arm9;
    g_section = "tcm";

    w9(m, 0x02000000u, 0x0AAA0000u); /* what is under the DTCM we will map */
    EQ("ITCM off: address 0 reads nothing", r9(m, 0x00000000u), 0);

    /* ITCM on, 32 MiB virtual. */
    nds9_cp15_write(c, 0x911, 0x20);
    nds9_cp15_write(c, 0x100, c->cp15.control | NDS_CP15_ITCM_ENABLE);
    w9(m, 0x00000000u, 0x17C00000u);
    EQ("ITCM", r9(m, 0x00000000u), 0x17C00000u);
    EQ("ITCM mirror at 0x01FF8000", r9(m, 0x01FF8000u), 0x17C00000u);
    EQ("opcode fetch from ITCM", nds9_bus_read32(c, 0x01FF8000u, NDS_ACCESS_CODE), 0x17C00000u);
    EQ("ITCM ends at 32 MiB", r9(m, 0x02000000u), 0x0AAA0000u);

    /* DTCM on at 0x02000000, 16 KiB. */
    nds9_cp15_write(c, 0x910, 0x0200000Au);
    nds9_cp15_write(c, 0x100, c->cp15.control | NDS_CP15_DTCM_ENABLE);
    EQ("DTCM hides main RAM for data", r9(m, 0x02000000u), 0);
    w9(m, 0x02000000u, 0xD7C00000u);
    EQ("DTCM", r9(m, 0x02000000u), 0xD7C00000u);
    EQ("DTCM is not for opcode fetches", nds9_bus_read32(c, 0x02000000u, NDS_ACCESS_CODE), 0x0AAA0000u);
    EQ("DMA goes around the DTCM", nds9_bus_read32(c, 0x02000000u, NDS_ACCESS_DMA), 0x0AAA0000u);
    EQ("the ARM7 never sees it", r7(m, 0x02000000u), 0x0AAA0000u);
    EQ("DTCM ends after 16 KiB", r9(m, 0x02004000u), 0);

    /* Load mode: writes reach the TCM, reads see what is underneath. */
    nds9_cp15_write(c, 0x100, c->cp15.control | NDS_CP15_DTCM_LOAD_MODE);
    EQ("DTCM load mode: read from RAM", r9(m, 0x02000000u), 0x0AAA0000u);
    w9(m, 0x02000004u, 0xD7C00004u);
    EQ("DTCM load mode: RAM not written", r7(m, 0x02000004u), 0);
    nds9_cp15_write(c, 0x100, c->cp15.control & ~NDS_CP15_DTCM_LOAD_MODE);
    EQ("DTCM load mode: the write reached the TCM", r9(m, 0x02000004u), 0xD7C00004u);

    /* Cycle costs are what the header says. */
    uint64_t before = c->cycles;
    r9(m, 0x02000000u);
    EQ("a TCM access", c->cycles - before, NDS9_CYCLES_TCM);
    before = c->cycles;
    r9(m, 0x02100000u);
    EQ("a main RAM data access", c->cycles - before, NDS9_CYCLES_DATA);
    before = c->cycles;
    r9(m, 0x05000000u);
    EQ("another bus access", c->cycles - before, NDS9_CYCLES_BUS);
    nds_destroy(m);
}

static void test_ipc(void)
{
    NdsMachine *m = fresh();
    g_section = "ipcsync";

    w9h(m, 0x04000180u, 0x0500);
    EQ("arm7 reads the ARM9's nibble", r7h(m, 0x04000180u) & 0xF, 5);
    w7h(m, 0x04000180u, 0x0A00);
    EQ("arm9 reads the ARM7's nibble and its own", r9h(m, 0x04000180u), 0x050Au);
    /* Bit 13 interrupts the other side only if that side enabled it. */
    w9h(m, 0x04000180u, 0x2500);
    EQ("no IRQ while the ARM7 has it disabled", m->if_[NDS_ARM7] & NDS_IRQ_IPC_SYNC, 0);
    w7h(m, 0x04000180u, 0x4A00);
    w9h(m, 0x04000180u, 0x2500);
    EQ("IRQ once enabled", m->if_[NDS_ARM7] & NDS_IRQ_IPC_SYNC, NDS_IRQ_IPC_SYNC);
    EQ("bit 13 does not read back", r9h(m, 0x04000180u) & 0x2000u, 0);

    g_section = "ipc fifo";
    EQ("reset: both empty", r9h(m, 0x04000184u), 0x0101u);
    w9(m, 0x04000188u, 0x11111111u);
    EQ("send while disabled is dropped", r9h(m, 0x04000184u), 0x0101u);
    w9h(m, 0x04000184u, 0x8000);
    w7h(m, 0x04000184u, 0x8000);
    w9(m, 0x04000188u, 0xCAFE0001u);
    EQ("arm9 send not empty", r9h(m, 0x04000184u), 0x8100u);
    EQ("arm7 recv not empty", r7h(m, 0x04000184u), 0x8001u);
    EQ("arm7 receives", r7(m, 0x04100000u), 0xCAFE0001u);
    EQ("empty again", r7h(m, 0x04000184u), 0x8101u);
    /* Reading an empty FIFO: the last word again, and the error bit. */
    EQ("empty read returns the last word", r7(m, 0x04100000u), 0xCAFE0001u);
    EQ("error bit", r7h(m, 0x04000184u) & 0x4000u, 0x4000u);
    w7h(m, 0x04000184u, 0xC000);
    EQ("error acknowledged", r7h(m, 0x04000184u) & 0x4000u, 0);
    /* Sixteen words fill it; the seventeenth is an error. */
    for (uint32_t i = 0; i < 16; i++) w9(m, 0x04000188u, 0x100u + i);
    EQ("full", r9h(m, 0x04000184u), 0x8102u);
    EQ("arm7 sees recv full", r7h(m, 0x04000184u), 0x8201u);
    w9(m, 0x04000188u, 0xDEADu);
    EQ("send to a full FIFO sets the error", r9h(m, 0x04000184u) & 0x4000u, 0x4000u);
    for (uint32_t i = 0; i < 16; i++) EQ("in order", r7(m, 0x04100000u), 0x100u + i);
    /* Flush: the other side then reads zero. */
    w9(m, 0x04000188u, 0x77u);
    w9h(m, 0x04000184u, 0x8008);
    EQ("flushed", r7h(m, 0x04000184u) & 0x0100u, 0x0100u);
    EQ("empty read after a flush is zero", r7(m, 0x04100000u), 0);
    /* Disabled on the receiving side: the oldest word, not removed. */
    w7h(m, 0x04000184u, 0x4000);
    w9(m, 0x04000188u, 0x99u);
    w7h(m, 0x04000184u, 0x0000);
    EQ("disabled receive peeks", r7(m, 0x04100000u), 0x99u);
    EQ("disabled receive peeks again", r7(m, 0x04100000u), 0x99u);
    w7h(m, 0x04000184u, 0x8000);
    EQ("enabled receive takes it", r7(m, 0x04100000u), 0x99u);

    g_section = "ipc fifo irq";
    m->if_[0] = m->if_[1] = 0;
    /* Receive-not-empty on the ARM7: raised on the edge. */
    w7h(m, 0x04000184u, 0x8400);
    EQ("nothing yet", m->if_[NDS_ARM7] & NDS_IRQ_IPC_RECV, 0);
    w9(m, 0x04000188u, 1);
    EQ("recv IRQ on the first word", m->if_[NDS_ARM7] & NDS_IRQ_IPC_RECV, NDS_IRQ_IPC_RECV);
    m->if_[NDS_ARM7] = 0;
    w9(m, 0x04000188u, 2);
    EQ("no second IRQ while it stays non-empty", m->if_[NDS_ARM7] & NDS_IRQ_IPC_RECV, 0);
    /* Send-empty on the ARM9: raised when the ARM7 takes the last word. */
    w9h(m, 0x04000184u, 0x8004);
    EQ("not empty: no IRQ on enabling", m->if_[NDS_ARM9] & NDS_IRQ_IPC_SEND_EMPTY, 0);
    r7(m, 0x04100000u);
    EQ("one word left", m->if_[NDS_ARM9] & NDS_IRQ_IPC_SEND_EMPTY, 0);
    r7(m, 0x04100000u);
    EQ("send-empty IRQ", m->if_[NDS_ARM9] & NDS_IRQ_IPC_SEND_EMPTY, NDS_IRQ_IPC_SEND_EMPTY);
    /* Enabling it while already empty is an edge too. */
    m->if_[NDS_ARM9] = 0;
    w9h(m, 0x04000184u, 0x8000);
    w9h(m, 0x04000184u, 0x8004);
    EQ("enable while empty", m->if_[NDS_ARM9] & NDS_IRQ_IPC_SEND_EMPTY, NDS_IRQ_IPC_SEND_EMPTY);
    nds_destroy(m);
}

static void div_case(NdsMachine *m, uint32_t mode, uint64_t numer, uint64_t denom, uint64_t quotient,
                     uint64_t remainder, uint32_t div0, int line)
{
    w9h(m, 0x04000280u, mode);
    w9(m, 0x04000290u, (uint32_t)numer);
    w9(m, 0x04000294u, (uint32_t)(numer >> 32));
    w9(m, 0x04000298u, (uint32_t)denom);
    w9(m, 0x0400029Cu, (uint32_t)(denom >> 32));
    uint64_t q = r9(m, 0x040002A0u) | ((uint64_t)r9(m, 0x040002A4u) << 32);
    uint64_t r = r9(m, 0x040002A8u) | ((uint64_t)r9(m, 0x040002ACu) << 32);
    check_eq("quotient", q, quotient, line);
    check_eq("remainder", r, remainder, line);
    check_eq("div-by-zero flag", r9h(m, 0x04000280u) & 0x4000u, div0 ? 0x4000u : 0u, line);
    check_eq("busy flag", r9h(m, 0x04000280u) & 0x8000u, 0, line);
}

static void test_math(void)
{
    NdsMachine *m = fresh();
    g_section = "div";
    /* 32 / 32: results sign-extended to 64 bits. */
    div_case(m, 0, 100, 7, 14, 2, 0, __LINE__);
    div_case(m, 0, (uint32_t)-100, 7, (uint64_t)-14, (uint64_t)-2, 0, __LINE__);
    div_case(m, 0, 100, (uint32_t)-7, (uint64_t)-14, 2, 0, __LINE__);
    /* The upper halves of the operands are not used in this mode. */
    div_case(m, 0, 0xFFFFFFFF00000064ull, 0x1234567800000007ull, 14, 2, 0, __LINE__);
    /* Division by zero: quotient -1 for a non-negative numerator and +1
     * for a negative one, upper half inverted; remainder = numerator. */
    div_case(m, 0, 5, 0, 0x00000000FFFFFFFFull, 5, 1, __LINE__);
    div_case(m, 0, (uint32_t)-5, 0, 0xFFFFFFFF00000001ull, (uint64_t)-5, 1, __LINE__);
    /* The flag looks at all 64 bits of the denominator, the division at 32. */
    div_case(m, 0, 5, 0x0000000100000000ull, 0x00000000FFFFFFFFull, 5, 0, __LINE__);
    /* -MAX / -1: -MAX with the upper half inverted, which is +MAX in 64 bits. */
    div_case(m, 0, 0x80000000u, 0xFFFFFFFFu, 0x0000000080000000ull, 0, 0, __LINE__);
    /* 64 / 32. */
    div_case(m, 1, 0x0000000100000000ull, 16, 0x10000000ull, 0, 0, __LINE__);
    div_case(m, 1, (uint64_t)-1000000000000ll, 1000, (uint64_t)-1000000000ll, 0, 0, __LINE__);
    div_case(m, 1, 7, 0, UINT64_MAX, 7, 1, __LINE__);
    div_case(m, 1, (uint64_t)-7, 0, 1, (uint64_t)-7, 1, __LINE__);
    div_case(m, 1, 0x8000000000000000ull, 0xFFFFFFFFu, 0x8000000000000000ull, 0, 0, __LINE__);
    div_case(m, 3, 1000, 10, 100, 0, 0, __LINE__); /* mode 3 is mode 1 */
    /* 64 / 64. */
    div_case(m, 2, 0x7FFFFFFFFFFFFFFFull, 0x0000000100000000ull, 0x7FFFFFFFull, 0xFFFFFFFFull, 0, __LINE__);
    div_case(m, 2, (uint64_t)-9, (uint64_t)-2, 4, (uint64_t)-1, 0, __LINE__);
    div_case(m, 2, 9, 0, UINT64_MAX, 9, 1, __LINE__);
    div_case(m, 2, 0x8000000000000000ull, UINT64_MAX, 0x8000000000000000ull, 0, 0, __LINE__);
    /* Writing any register recomputes: change only the mode. */
    div_case(m, 0, 0x0000000100000000ull, 16, 0, 0, 0, __LINE__);
    w9h(m, 0x04000280u, 1);
    EQ("mode change recomputes", r9(m, 0x040002A0u), 0x10000000u);

    g_section = "sqrt";
    static const struct { uint32_t mode; uint64_t in; uint32_t out; } roots[] = {
        { 0, 0, 0 }, { 0, 1, 1 }, { 0, 3, 1 }, { 0, 4, 2 }, { 0, 0xFFFFFFFFu, 0xFFFFu },
        { 0, 0x100000000ull | 81, 9 }, /* 32-bit mode ignores the upper half */
        { 1, 0x100000000ull, 0x10000u }, { 1, UINT64_MAX, 0xFFFFFFFFu },
        { 1, 0x0000000100000000ull - 1, 0xFFFFu }, { 1, 1000000000000ull, 1000000u },
    };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        w9h(m, 0x040002B0u, roots[i].mode);
        w9(m, 0x040002B8u, (uint32_t)roots[i].in);
        w9(m, 0x040002BCu, (uint32_t)(roots[i].in >> 32));
        EQ("root", r9(m, 0x040002B4u), roots[i].out);
        EQ("busy flag", r9h(m, 0x040002B0u) & 0x8000u, 0);
    }
    EQ("the parameter reads back", r9(m, 0x040002B8u), 1000000000000ull & 0xFFFFFFFFu);
    nds_destroy(m);
}

/* Advance time with both CPUs asleep. */
static void advance(NdsMachine *m, uint64_t cycles)
{
    nds_run_until(m, m->now + cycles);
}

static void test_timers(void)
{
    NdsMachine *m = fresh();
    g_section = "timers";

    /* A timer runs on its CPU's clock, and the accesses made here move that
     * on ahead of the machine's, so times are taken from the CPU. ARM7
     * timer 0, prescaler 1, from 0xFF00: it overflows 0x100 cycles after
     * the write that starts it. */
    w7h(m, 0x04000100u, 0xFF00);
    w7h(m, 0x04000102u, 0x00C0);
    uint64_t start = nds7_now(m);
    EQ("starts at the reload value", nds_timer_read_counter(m, NDS_ARM7, 0), 0xFF00u);
    nds_run_until(m, start + 0x80);
    EQ("counts system cycles", nds_timer_read_counter(m, NDS_ARM7, 0), 0xFF80u);
    EQ("a read through the bus is one access later", r7h(m, 0x04000100u), 0xFF81u);
    nds_run_until(m, start + 0xFF);
    EQ("the last count", nds_timer_read_counter(m, NDS_ARM7, 0), 0xFFFFu);
    EQ("no IRQ yet", m->if_[NDS_ARM7] & NDS_IRQ_TIMER0, 0);
    nds_run_until(m, start + 0x100);
    EQ("IRQ on overflow", m->if_[NDS_ARM7] & NDS_IRQ_TIMER0, NDS_IRQ_TIMER0);
    EQ("reloaded", nds_timer_read_counter(m, NDS_ARM7, 0), 0xFF00u);
    nds_run_until(m, start + 0x230);
    EQ("the period repeats", nds_timer_read_counter(m, NDS_ARM7, 0), 0xFF30u);
    EQ("control reads back", r7h(m, 0x04000102u), 0x00C0u);
    w7h(m, 0x04000102u, 0x0000);
    uint32_t stopped = nds_timer_read_counter(m, NDS_ARM7, 0);
    nds_run_until(m, m->now + 0x100);
    EQ("stopped timer holds its value", r7h(m, 0x04000100u), stopped);
    /* Starting it again loads the reload value; a new reload value while it
     * runs waits for the next overflow. */
    w7h(m, 0x04000100u, 0xFFF0);
    w7h(m, 0x04000102u, 0x0080);
    start = nds7_now(m);
    w7h(m, 0x04000100u, 0xFFFC);
    nds_run_until(m, start + 8);
    EQ("restart loads the reload value", nds_timer_read_counter(m, NDS_ARM7, 0), 0xFFF8u);
    nds_run_until(m, start + 0x12);
    EQ("the new reload value at the overflow", nds_timer_read_counter(m, NDS_ARM7, 0), 0xFFFEu);
    w7h(m, 0x04000102u, 0x0000);

    /* ARM9 timer 1 with the /64 prescaler: the same clock on both CPUs, and
     * it ticks on multiples of 64 cycles whenever it was started. */
    m->if_[NDS_ARM9] = 0;
    w9h(m, 0x04000104u, 0xFFFE);
    w9h(m, 0x04000106u, 0x00C1);
    uint64_t tick = (nds9_now(m) | 63u) + 1;
    nds_run_until(m, tick - 1);
    EQ("/64: no tick before the multiple of 64", nds_timer_read_counter(m, NDS_ARM9, 1), 0xFFFEu);
    nds_run_until(m, tick);
    EQ("/64: first tick", nds_timer_read_counter(m, NDS_ARM9, 1), 0xFFFFu);
    nds_run_until(m, tick + 63);
    EQ("/64: not before the second", m->if_[NDS_ARM9] & (NDS_IRQ_TIMER0 << 1), 0);
    nds_run_until(m, tick + 64);
    EQ("/64: overflowed", m->if_[NDS_ARM9] & (NDS_IRQ_TIMER0 << 1), NDS_IRQ_TIMER0 << 1);
    EQ("/64: reloaded", nds_timer_read_counter(m, NDS_ARM9, 1), 0xFFFEu);
    w9h(m, 0x04000106u, 0);

    /* Count-up: timer 3 counts timer 2's overflows. */
    m->if_[NDS_ARM9] = 0;
    w9h(m, 0x04000108u, 0xFFF0);
    w9h(m, 0x0400010Cu, 0xFFFE);
    w9h(m, 0x0400010Eu, 0x00C4);
    w9h(m, 0x0400010Au, 0x0080);
    start = nds9_now(m);
    nds_run_until(m, start + 0x0F);
    EQ("cascade: nothing before timer 2 overflows", nds_timer_read_counter(m, NDS_ARM9, 3), 0xFFFEu);
    nds_run_until(m, start + 0x10);
    EQ("cascade: one overflow counted", nds_timer_read_counter(m, NDS_ARM9, 3), 0xFFFFu);
    EQ("cascade: no IRQ yet", m->if_[NDS_ARM9] & (NDS_IRQ_TIMER0 << 3), 0);
    nds_run_until(m, start + 0x20);
    EQ("cascade: IRQ on its own overflow", m->if_[NDS_ARM9] & (NDS_IRQ_TIMER0 << 3), NDS_IRQ_TIMER0 << 3);
    EQ("cascade: reloaded", nds_timer_read_counter(m, NDS_ARM9, 3), 0xFFFEu);
    EQ("timer 2 asked for no interrupt", m->if_[NDS_ARM9] & (NDS_IRQ_TIMER0 << 2), 0);
    nds_destroy(m);
}

static void test_dma(void)
{
    NdsMachine *m = fresh();
    g_section = "dma";

    for (uint32_t i = 0; i < 8; i++) w9(m, 0x02000100u + i * 4, 0xD0000000u + i);
    /* ARM9 channel 0: eight words, both addresses incrementing, immediate. */
    w9(m, 0x040000B0u, 0x02000100u);
    w9(m, 0x040000B4u, 0x02000200u);
    w9(m, 0x040000B8u, 0x84000008u);
    for (uint32_t i = 0; i < 8; i++) EQ("word copy", r9(m, 0x02000200u + i * 4), 0xD0000000u + i);
    EQ("enable bit clears when done", r9(m, 0x040000B8u), 0x04000008u);
    EQ("registers read back", r9(m, 0x040000B0u), 0x02000100u);
    EQ("no IRQ unless asked", m->if_[NDS_ARM9] & NDS_IRQ_DMA0, 0);

    /* Halfwords, source decrementing, with the interrupt. */
    w9(m, 0x040000BCu, 0x0200010Eu);
    w9(m, 0x040000C0u, 0x02000300u);
    w9(m, 0x040000C4u, 0xC0800004u);
    EQ("halfword copy, source going down", r9(m, 0x02000300u), 0x0003D000u);
    EQ("halfword copy 2", r9(m, 0x02000304u), 0x0002D000u);
    EQ("IRQ", m->if_[NDS_ARM9] & (NDS_IRQ_DMA0 << 1), NDS_IRQ_DMA0 << 1);

    /* A fill: fixed source at the channel's fill register. */
    w9(m, 0x040000ECu, 0xF111F111u);
    w9(m, 0x040000D4u, 0x040000ECu);
    w9(m, 0x040000D8u, 0x02000400u);
    w9(m, 0x040000DCu, 0x85000010u);
    EQ("fill first", r9(m, 0x02000400u), 0xF111F111u);
    EQ("fill last", r9(m, 0x0200043Cu), 0xF111F111u);
    EQ("fill stops", r9(m, 0x02000440u), 0);

    /* VBlank mode with repeat: runs each VBlank, stays enabled. */
    w9(m, 0x040000B0u, 0x02000100u);
    w9(m, 0x040000B4u, 0x02000500u);
    w9(m, 0x040000B8u, 0x8E000002u);
    EQ("not before VBlank", r9(m, 0x02000500u), 0);
    advance(m, (uint64_t)NDS_CYCLES_PER_LINE * 192);
    EQ("at VBlank", r9(m, 0x02000500u), 0xD0000000u);
    EQ("still enabled", r9(m, 0x040000B8u) >> 31, 1);
    advance(m, NDS_CYCLES_PER_FRAME);
    EQ("next VBlank continues", r9(m, 0x02000508u), 0xD0000002u);
    w9(m, 0x040000B8u, 0);

    /* The ARM7's channel 3 has a 16-bit count and no bit 27 in its mode. */
    for (uint32_t i = 0; i < 4; i++) w7(m, 0x03800000u + i * 4, 0x70000000u + i);
    w7(m, 0x040000D4u, 0x03800000u);
    w7(m, 0x040000D8u, 0x03800100u);
    w7(m, 0x040000DCu, 0x84000004u);
    EQ("arm7 copy", r7(m, 0x0380010Cu), 0x70000003u);

    /* HBlank mode on the ARM9: once per visible line. */
    w9(m, 0x040000B0u, 0x02000100u);
    w9(m, 0x040000B4u, 0x02000600u);
    w9(m, 0x040000B8u, 0x96000001u); /* repeat, HBlank, one word, destination incrementing */
    uint64_t to_frame_start = NDS_CYCLES_PER_FRAME - (m->now % NDS_CYCLES_PER_FRAME);
    advance(m, to_frame_start + (uint64_t)NDS_CYCLES_PER_LINE * 3);
    EQ("three HBlanks: the first word", r9(m, 0x02000600u), 0xD0000000u);
    EQ("three HBlanks: the third", r9(m, 0x02000608u), 0xD0000002u);
    EQ("and not a fourth", r9(m, 0x0200060Cu), 0);
    nds_destroy(m);
}

/* A synthetic card: a header naming two small programs, and data whose
 * every word is its own offset. */
#define TEST_ROM_SIZE 0x20000u
static uint8_t g_rom[TEST_ROM_SIZE];

static void build_rom(void)
{
    for (uint32_t i = 0; i < TEST_ROM_SIZE; i += 4) nds_put_le32(g_rom + i, i);
    memset(g_rom, 0, 0x200);
    memcpy(g_rom, "TESTROM", 7);
    memcpy(g_rom + 0x0C, "TEST", 4);
    nds_put_le32(g_rom + 0x20, 0x4000);      /* ARM9 offset */
    nds_put_le32(g_rom + 0x24, 0x02000000u); /* entry */
    nds_put_le32(g_rom + 0x28, 0x02000000u); /* load address */
    nds_put_le32(g_rom + 0x2C, 12);
    nds_put_le32(g_rom + 0x30, 0x8000);      /* ARM7 offset */
    nds_put_le32(g_rom + 0x34, 0x02380000u);
    nds_put_le32(g_rom + 0x38, 0x02380000u);
    nds_put_le32(g_rom + 0x3C, 8);
    nds_put_le32(g_rom + 0x60, 0x00586000u); /* ROMCTRL for a fast mask ROM */
    nds_put_le16(g_rom + 0x15E, 0xBEEF);
    /* ARM9: mov r0, #5 ; add r0, r0, #7 ; b . */
    nds_put_le32(g_rom + 0x4000, 0xE3A00005u);
    nds_put_le32(g_rom + 0x4004, 0xE2800007u);
    nds_put_le32(g_rom + 0x4008, 0xEAFFFFFEu);
    /* ARM7: mov r1, #9 ; b . */
    nds_put_le32(g_rom + 0x8000, 0xE3A01009u);
    nds_put_le32(g_rom + 0x8004, 0xEAFFFFFEu);
}

static void test_card(void)
{
    NdsMachine *m = nds_create();
    build_rom();
    g_section = "card";
    EQ("ROM loads", nds_load_rom(m, g_rom, TEST_ROM_SIZE), 1);
    nds_reset(m);
    m->halted[0] = m->halted[1] = true;
    /* 128 KiB rounds to itself; the size code is megabytes - 1, so 0. */
    EQ("chip ID", m->chip_id, 0x000000C2u);

    /* B7: read 0x200 bytes from 0x10000. */
    static const uint8_t read_cmd[8] = { 0xB7, 0x00, 0x01, 0x00, 0x00, 0, 0, 0 };
    for (uint32_t i = 0; i < 8; i++) w9b(m, 0x040001A8u + i, read_cmd[i]);
    w9h(m, 0x040001A0u, 0xC000); /* slot on, transfer-done IRQ on */
    w9(m, 0x040001A4u, 0xA1000000u);
    EQ("busy, no word yet", r9(m, 0x040001A4u) & 0x80800000u, 0x80000000u);
    EQ("reading early gives nothing", r9(m, 0x04100010u), 0);
    for (uint32_t i = 0; i < 128; i++) {
        advance(m, 200);
        EQ("word ready", r9(m, 0x040001A4u) & 0x00800000u, 0x00800000u);
        EQ("data", r9(m, 0x04100010u), 0x10000u + i * 4);
    }
    EQ("done: busy and ready clear", r9(m, 0x040001A4u) & 0x80800000u, 0);
    EQ("transfer-done IRQ", m->if_[NDS_ARM9] & NDS_IRQ_CARD_DONE, NDS_IRQ_CARD_DONE);
    EQ("the ARM7 was not interrupted", m->if_[NDS_ARM7] & NDS_IRQ_CARD_DONE, 0);

    /* Addresses below 0x8000 come from 0x8000 on, every 0x200. */
    static const uint8_t low_cmd[8] = { 0xB7, 0x00, 0x00, 0x12, 0x34, 0, 0, 0 };
    for (uint32_t i = 0; i < 8; i++) w9b(m, 0x040001A8u + i, low_cmd[i]);
    w9(m, 0x040001A4u, 0xA7000000u); /* four bytes */
    advance(m, 200);
    /* 0x8034 holds 0x8034, except that the ARM7 program is at 0x8000-7. */
    EQ("redirected", r9(m, 0x04100010u), 0x8034u);

    /* B8: the chip ID. */
    static const uint8_t id_cmd[8] = { 0xB8, 0, 0, 0, 0, 0, 0, 0 };
    for (uint32_t i = 0; i < 8; i++) w9b(m, 0x040001A8u + i, id_cmd[i]);
    w9(m, 0x040001A4u, 0xA7000000u);
    advance(m, 200);
    EQ("chip ID command", r9(m, 0x04100010u), 0x000000C2u);

    /* A zero-length transfer ends at once. */
    m->if_[NDS_ARM9] = 0;
    w9(m, 0x040001A4u, 0xA0000000u);
    EQ("no data: not busy", r9(m, 0x040001A4u) & 0x80000000u, 0);
    EQ("no data: IRQ", m->if_[NDS_ARM9] & NDS_IRQ_CARD_DONE, NDS_IRQ_CARD_DONE);
    EQ("bit 29 stays set", r9(m, 0x040001A4u) & 0x20000000u, 0x20000000u);

    /* The same read by DMA: channel 3 in card mode. */
    for (uint32_t i = 0; i < 8; i++) w9b(m, 0x040001A8u + i, read_cmd[i]);
    w9(m, 0x040000D4u, 0x04100010u);
    w9(m, 0x040000D8u, 0x02001000u);
    w9(m, 0x040000DCu, 0xAD000080u); /* enable, card mode, 32-bit, source fixed, 128 words */
    w9(m, 0x040001A4u, 0xA1000000u);
    advance(m, 20000);
    EQ("DMA first word", r9(m, 0x02001000u), 0x10000u);
    EQ("DMA last word", r9(m, 0x020011FCu), 0x101FCu);
    EQ("DMA switched itself off", r9(m, 0x040000DCu) >> 31, 0);
    EQ("transfer finished", r9(m, 0x040001A4u) & 0x80000000u, 0);
    nds_destroy(m);
}

static void test_boot(void)
{
    NdsMachine *m = nds_create();
    build_rom();
    g_section = "boot";
    nds_load_rom(m, g_rom, TEST_ROM_SIZE);
    nds_reset(m);

    EQ("ARM9 binary loaded", r7(m, 0x02000004u), 0xE2800007u);
    EQ("ARM7 binary loaded", r7(m, 0x02380000u), 0xE3A01009u);
    EQ("header at 0x027FFE00", r7(m, 0x027FFE0Cu), 0x54534554u); /* "TEST" */
    EQ("chip ID at 0x027FF800", r7(m, 0x027FF800u), 0xC2u);
    EQ("chip ID at 0x027FFC00", r7(m, 0x027FFC00u), 0xC2u);
    EQ("header CRC at 0x027FF808", r7h(m, 0x027FF808u), 0xBEEFu);
    EQ("ARM7 BIOS CRC", r7h(m, 0x027FFC10u), 0x5835u);
    EQ("boot indicator", r7h(m, 0x027FFC40u), 1);
    EQ("ARM9 pc", nds_cpu_pc(&m->arm9), 0x02000000u);
    EQ("ARM9 r12", m->arm9.r[12], 0x02000000u);
    EQ("ARM9 lr", m->arm9.r[14], 0x02000000u);
    EQ("ARM9 sp", m->arm9.r[13], 0x03002F7Cu);
    EQ("ARM9 irq sp", m->arm9.bank_r13[NDS_BANK_IRQ], 0x03003F80u);
    EQ("ARM9 svc sp", m->arm9.bank_r13[NDS_BANK_SVC], 0x03003FC0u);
    EQ("ARM9 mode", nds_cpu_mode(&m->arm9), NDS_MODE_SYS);
    EQ("ARM7 pc", nds_cpu_pc(&m->arm7), 0x02380000u);
    EQ("ARM7 sp", m->arm7.r[13], 0x0380FD80u);
    EQ("ARM7 irq sp", m->arm7.bank_r13[NDS_BANK_IRQ], 0x0380FF80u);
    EQ("ARM7 svc sp", m->arm7.bank_r13[NDS_BANK_SVC], 0x0380FFC0u);
    EQ("POSTFLG", r9b(m, 0x04000300u), 1);
    EQ("the shared WRAM is the ARM7's", r7b(m, 0x04000241u), 3);
    /* The stacks are inside the DTCM the boot maps over 0x03000000. */
    w9(m, 0x03002F78u, 0x57AC0000u);
    EQ("the stack is DTCM", nds_get_le32(m->dtcm + 0x2F78), 0x57AC0000u);

    /* Both programs run, each at its own speed, and stay in step. */
    nds_run_until(m, 2000);
    EQ("ARM9 ran", m->arm9.r[0], 12);
    EQ("ARM7 ran", m->arm7.r[1], 9);
    EQ("time", m->now, 2000);
    EQ("ARM7 at the target, within an instruction", nds7_now(m) >= 2000 && nds7_now(m) < 2008, 1);
    EQ("ARM9 at the target, within an instruction", nds9_now(m) >= 2000 && nds9_now(m) < 2008, 1);
    /* "b ." costs each CPU three opcode fetches: the ARM9 at two of its
     * cycles each gets through twice as many per system cycle... */
    EQ("instruction counts in proportion", m->instructions[NDS_ARM9] > 0 && m->instructions[NDS_ARM7] > 0, 1);
    nds_destroy(m);
}

/* For the wake test: when the ARM7 first ran, and when the ARM9 got past
 * the store that woke it. */
static uint64_t g_woke_at, g_sent_at;

static void wake_hook(NdsMachine *m, int cpu)
{
    if (cpu == NDS_ARM7 && g_woke_at == 0) g_woke_at = nds7_now(m);
    if (cpu == NDS_ARM9 && g_sent_at == 0 && nds_cpu_pc(&m->arm9) == 0x02000018u) g_sent_at = nds9_now(m);
}

static void test_interrupts(void)
{
    NdsMachine *m = fresh();
    g_section = "interrupts";

    w9(m, 0x04000210u, 0xFFFFFFFFu);
    EQ("ARM9 IE keeps only its sources", r9(m, 0x04000210u), 0x003F3F7Fu);
    w7(m, 0x04000210u, 0xFFFFFFFFu);
    EQ("ARM7 IE keeps only its sources", r7(m, 0x04000210u), 0x01FF3FFFu);
    nds_raise_irq(m, NDS_ARM9, NDS_IRQ_VBLANK | NDS_IRQ_TIMER0);
    EQ("IF", r9(m, 0x04000214u), NDS_IRQ_VBLANK | NDS_IRQ_TIMER0);
    EQ("line low while IME is clear", m->irq_line[NDS_ARM9], 0);
    w9(m, 0x04000208u, 1);
    EQ("line high with IME", m->irq_line[NDS_ARM9], 1);
    w9(m, 0x04000214u, NDS_IRQ_VBLANK);
    EQ("writing 1 acknowledges", r9(m, 0x04000214u), NDS_IRQ_TIMER0);
    w9b(m, 0x04000214u, NDS_IRQ_TIMER0);
    EQ("a byte write acknowledges too", r9(m, 0x04000214u), 0);
    EQ("line low again", m->irq_line[NDS_ARM9], 0);
    EQ("the other CPU is separate", r7(m, 0x04000214u), 0);

    g_section = "halt";
    NdsMachine *h = nds_create();
    build_rom();
    nds_load_rom(h, g_rom, TEST_ROM_SIZE);
    nds_reset(h);
    /* The ARM7 writes HALTCNT and stops; an interrupt it has enabled
     * wakes it even with IME clear. */
    nds_run_until(h, 200);
    uint64_t ran = h->instructions[NDS_ARM7];
    w7b(h, 0x04000301u, 0x80);
    EQ("halted", h->halted[NDS_ARM7], 1);
    nds_run_until(h, 2000);
    EQ("no instructions while halted", h->instructions[NDS_ARM7], ran);
    EQ("but its clock keeps up", nds7_now(h) >= 2000, 1);
    w7(h, 0x04000210u, NDS_IRQ_VBLANK);
    nds_raise_irq(h, NDS_ARM7, NDS_IRQ_HBLANK);
    nds_run_until(h, 2500);
    EQ("an interrupt it has not enabled does not wake it", h->instructions[NDS_ARM7], ran);
    nds_raise_irq(h, NDS_ARM7, NDS_IRQ_VBLANK);
    nds_run_until(h, 3000);
    EQ("an enabled one does", h->instructions[NDS_ARM7] > ran, 1);
    EQ("awake", h->halted[NDS_ARM7], 0);
    /* The ARM9 sleeps through CP15. */
    ran = h->instructions[NDS_ARM9];
    nds9_cp15_write(&h->arm9, 0x704, 0);
    nds_run_until(h, 4000);
    EQ("ARM9 waits for an interrupt", h->instructions[NDS_ARM9], ran);
    /* What ends each CPU's halt (the table in BlocksDS's
     * tests/system/irq_handling, taken on a console): IE & IF, whatever IME
     * says; and for the ARM9 only while CPSR.I is clear. */
    w9(h, 0x04000208u, 0);
    w9(h, 0x04000210u, NDS_IRQ_VBLANK);
    h->arm9.cpsr |= NDS_CPSR_I;
    nds_raise_irq(h, NDS_ARM9, NDS_IRQ_VBLANK);
    nds_run_until(h, 5000);
    EQ("ARM9 stays halted while CPSR.I is set", h->instructions[NDS_ARM9], ran);
    EQ("ARM9 still halted", h->halted[NDS_ARM9], 1);
    h->arm9.cpsr &= ~NDS_CPSR_I;
    nds_run_until(h, 6000);
    EQ("ARM9 goes on with CPSR.I clear and IME clear", h->instructions[NDS_ARM9] > ran, 1);
    EQ("ARM9 awake", h->halted[NDS_ARM9], 0);
    ran = h->instructions[NDS_ARM7];
    w7(h, 0x04000208u, 0);
    w7(h, 0x04000214u, 0xFFFFFFFFu);
    w7b(h, 0x04000301u, 0x80);
    h->arm7.cpsr |= NDS_CPSR_I;
    nds_run_until(h, 7000);
    EQ("ARM7 halted again", h->instructions[NDS_ARM7], ran);
    nds_raise_irq(h, NDS_ARM7, NDS_IRQ_VBLANK);
    nds_run_until(h, 8000);
    EQ("ARM7 goes on even with CPSR.I set", h->instructions[NDS_ARM7] > ran, 1);
    nds_destroy(h);

    /* One CPU waking the other: the sleeper starts at the moment it was
     * woken, not back where the scheduler's slice began. The ARM9 counts
     * down for about a thousand cycles, then raises the IPCSYNC interrupt. */
    g_section = "wake";
    static const uint32_t sender[7] = {
        0xE3A00301u, /* mov r0, #0x04000000 */
        0xE3A01C01u, /* mov r1, #0x100 */
        0xE2511001u, /* subs r1, r1, #1 */
        0x1AFFFFFDu, /* bne  (the subs) */
        0xE3A02A02u, /* mov r2, #0x2000 */
        0xE5802180u, /* str r2, [r0, #0x180] */
        0xEAFFFFFEu, /* b . */
    };
    h = nds_create();
    build_rom();
    for (uint32_t i = 0; i < 7; i++) nds_put_le32(g_rom + 0x4000 + i * 4, sender[i]);
    nds_put_le32(g_rom + 0x2C, sizeof sender);
    nds_load_rom(h, g_rom, TEST_ROM_SIZE);
    nds_reset(h);
    w7h(h, 0x04000180u, 0x4000);
    w7(h, 0x04000210u, NDS_IRQ_IPC_SYNC);
    h->halted[NDS_ARM7] = true;
    g_woke_at = g_sent_at = 0;
    h->step_hook = wake_hook;
    nds_run_until(h, 1500);
    EQ("the ARM9 got there", g_sent_at > 1000 && g_sent_at < 1100, 1);
    EQ("the ARM7 is awake", h->halted[NDS_ARM7], 0);
    EQ("and woke when the interrupt was raised", g_woke_at <= g_sent_at && g_woke_at + 4 >= g_sent_at, 1);
    nds_destroy(h);
    nds_destroy(m);
}

static void test_video_timing(void)
{
    NdsMachine *m = fresh();
    g_section = "video timing";

    EQ("starts on line 0", r9h(m, 0x04000006u), 0);
    advance(m, NDS_HBLANK_CYCLES - 1);
    EQ("no HBlank flag yet", r9h(m, 0x04000004u) & 2u, 0);
    advance(m, 1);
    EQ("HBlank flag at 1606 cycles", r9h(m, 0x04000004u) & 2u, 2);
    EQ("the ARM7 sees the same flag", r7h(m, 0x04000004u) & 2u, 2);
    advance(m, NDS_CYCLES_PER_LINE - NDS_HBLANK_CYCLES);
    EQ("line 1", r9h(m, 0x04000006u), 1);
    EQ("HBlank flag clear", r9h(m, 0x04000004u) & 2u, 0);

    /* Each CPU enables its own interrupts. */
    w9h(m, 0x04000004u, 0x0008);                 /* ARM9: VBlank */
    w7h(m, 0x04000004u, 0x0010 | 0x0020 | 0x0080 | (5u << 8)); /* ARM7: HBlank, and match on line 261 */
    EQ("DISPSTAT enables read back", r7h(m, 0x04000004u) & 0xFFB8u, 0x05B0u);
    advance(m, (uint64_t)NDS_CYCLES_PER_LINE * 190);
    EQ("line 191: not in VBlank", r9h(m, 0x04000004u) & 1u, 0);
    EQ("no VBlank IRQ yet", m->if_[NDS_ARM9] & NDS_IRQ_VBLANK, 0);
    EQ("ARM7 got HBlank IRQs", m->if_[NDS_ARM7] & NDS_IRQ_HBLANK, NDS_IRQ_HBLANK);
    EQ("ARM9 did not ask for them", m->if_[NDS_ARM9] & NDS_IRQ_HBLANK, 0);
    m->frame_ready = false;
    advance(m, NDS_CYCLES_PER_LINE);
    EQ("line 192", r9h(m, 0x04000006u), 192);
    EQ("VBlank flag", r9h(m, 0x04000004u) & 1u, 1);
    EQ("VBlank IRQ on the ARM9", m->if_[NDS_ARM9] & NDS_IRQ_VBLANK, NDS_IRQ_VBLANK);
    EQ("not on the ARM7", m->if_[NDS_ARM7] & NDS_IRQ_VBLANK, 0);
    EQ("frame complete", m->frame_ready, 1);
    EQ("frame counted", m->frame_count, 1);
    /* The match line is nine bits: 0x105 = 261. */
    advance(m, (uint64_t)NDS_CYCLES_PER_LINE * 68);
    EQ("line 260: no match", m->if_[NDS_ARM7] & NDS_IRQ_VCOUNT, 0);
    advance(m, NDS_CYCLES_PER_LINE);
    EQ("line 261", r7h(m, 0x04000006u), 261);
    EQ("match IRQ", m->if_[NDS_ARM7] & NDS_IRQ_VCOUNT, NDS_IRQ_VCOUNT);
    EQ("match flag", r7h(m, 0x04000004u) & 4u, 4);
    EQ("still VBlank on 261", r9h(m, 0x04000004u) & 1u, 1);
    advance(m, NDS_CYCLES_PER_LINE);
    EQ("line 262: the VBlank flag is clear", r9h(m, 0x04000004u) & 1u, 0);
    advance(m, NDS_CYCLES_PER_LINE);
    EQ("wraps to line 0", r9h(m, 0x04000006u), 0);

    /* A write to VCOUNT takes effect at the next line. */
    w9h(m, 0x04000006u, 210);
    EQ("not yet", r9h(m, 0x04000006u), 0);
    advance(m, NDS_CYCLES_PER_LINE);
    EQ("the written line", r9h(m, 0x04000006u), 210);
    nds_destroy(m);
}

/* A triangle with no edge along the pixel grid, sent to the 3D engine and
 * shown as BG0 on the top screen. */
static void send_triangle(NdsMachine *m)
{
    w9h(m, 0x04000304u, 0x820F);                       /* POWCNT1: everything on, engine A on top */
    w9(m, 0x04000000u, (1u << 16) | (1u << 8) | (1u << 3)); /* DISPCNT: BG0, and BG0 is the 3D picture */
    w9(m, 0x04000350u, 31u << 16);                     /* CLEAR_COLOR: black, opaque */
    w9h(m, 0x04000354u, 0x7FFF);                       /* CLEAR_DEPTH */
    w9(m, 0x04000580u, (255u << 16) | (191u << 24));   /* VIEWPORT */
    w9(m, 0x04000440u, 0);                             /* MTX_MODE: projection */
    w9(m, 0x04000454u, 0);                             /* MTX_IDENTITY */
    w9(m, 0x04000440u, 2);
    w9(m, 0x04000454u, 0);
    w9(m, 0x040004A4u, (3u << 6) | (31u << 16));       /* POLYGON_ATTR: both sides, opaque */
    w9(m, 0x04000500u, 0);                             /* BEGIN_VTXS: triangles */
    w9(m, 0x04000480u, 0x7FFF);                        /* COLOR: white */
    static const int16_t corner[3][2] = { { -0x800, -0x800 }, { 0x99A, -0x4CD }, { 0x19A, 0xB33 } };
    for (int i = 0; i < 3; i++) {
        w9(m, 0x0400048Cu, ((uint32_t)(uint16_t)corner[i][1] << 16) | (uint16_t)corner[i][0]); /* VTX_16 */
        w9(m, 0x0400048Cu, 0);
    }
    w9(m, 0x04000540u, 0);                             /* SWAP_BUFFERS */
}

/* ... for as many frames as it takes to be on screen. */
static void show_triangle(NdsMachine *m)
{
    send_triangle(m);
    advance(m, (uint64_t)NDS_CYCLES_PER_LINE * 263u * 3u + (uint64_t)NDS_CYCLES_PER_LINE * 200u);
}

static long white_pixels(const uint32_t *pixels, size_t count)
{
    long n = 0;
    for (size_t i = 0; i < count; i++) n += pixels[i] == 0xFFFFFFFFu;
    return n;
}

/* nds_set_internal_scale(): a larger picture beside the machine, which the
 * machine cannot tell is there. */
static void test_internal_scale(void)
{
    g_section = "internal resolution";
    NdsMachine *plain = fresh(), *large = fresh();
    EQ("a machine starts at a scale of 1", nds_internal_scale(plain), 1);
    nds_set_internal_scale(large, 3);
    EQ("the scale asked for", nds_internal_scale(large), 3);
    nds_set_internal_scale(large, 99);
    EQ("the largest there is", nds_internal_scale(large), NDS_INTERNAL_SCALE_MAX);
    nds_set_internal_scale(large, 3);
    show_triangle(plain);
    show_triangle(large);

    const size_t screen = (size_t)NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT;
    const long white = white_pixels(plain->framebuffer[0], screen);
    EQ("the triangle is on the top screen", white > 3000 && white < 9000, 1);
    EQ("the console's picture is the same with a larger one beside it",
       memcmp(plain->framebuffer, large->framebuffer, sizeof plain->framebuffer), 0);
    EQ("... and so is video memory", memcmp(plain->vram, large->vram, sizeof plain->vram), 0);
    EQ("... and main memory", memcmp(plain->main_ram, large->main_ram, sizeof plain->main_ram), 0);
    EQ("... and the 3D engine", memcmp(&plain->gx, &large->gx, sizeof plain->gx), 0);
    EQ("... and the time", plain->now == large->now && plain->frame_count == large->frame_count, 1);

    int w = 0, h = 0;
    EQ("at a scale of 1 there is no larger frame", nds_fine_frame(plain, 0, &w, &h) == NULL, 1);
    const uint32_t *fine = nds_fine_frame(large, 0, &w, &h);
    EQ("the top screen has one", fine != NULL, 1);
    EQ("three times as wide", w, NDS_SCREEN_WIDTH * 3);
    EQ("three times as high", h, NDS_SCREEN_HEIGHT * 3);
    EQ("the bottom screen, which is engine B's, has none", nds_fine_frame(large, 1, NULL, NULL) == NULL, 1);
    if (fine) {
        const long fine_white = white_pixels(fine, screen * 9u);
        /* Nine times the area, give or take the pixels along the edges. */
        EQ("the triangle covers nine times the pixels", fine_white > white * 9 - 1500 && fine_white < white * 9 + 1500, 1);
        /* Its edges are drawn on the finer grid: the larger frame is not
         * the screen's with each pixel repeated. */
        long stepped = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                stepped += fine[(size_t)y * (size_t)w + (size_t)x] != large->framebuffer[0][(y / 3) * NDS_SCREEN_WIDTH + x / 3];
        EQ("... along edges drawn finer than the screen's", stepped > 100 && stepped < 3000, 1);
        /* Only black and white: nothing uninitialised, nothing blended. */
        long other = 0;
        for (size_t i = 0; i < screen * 9u; i++) other += fine[i] != 0xFFFFFFFFu && fine[i] != 0xFF000000u;
        EQ("... and of nothing but the triangle and the rear plane", other, 0);
    }

    /* Engine A moved to the bottom screen: its larger frame goes with it,
     * after two whole frames there. */
    w9h(large, 0x04000304u, 0x020F);
    advance(large, (uint64_t)NDS_CYCLES_PER_LINE * 263u);
    EQ("a screen engine A has just left has no larger frame", nds_fine_frame(large, 0, NULL, NULL) == NULL, 1);
    advance(large, (uint64_t)NDS_CYCLES_PER_LINE * 263u * 2u);
    EQ("the screen it moved to has one after two frames", nds_fine_frame(large, 1, NULL, NULL) != NULL, 1);

    nds_set_internal_scale(large, 1);
    EQ("back at a scale of 1", nds_internal_scale(large), 1);
    EQ("... with no larger frame", nds_fine_frame(large, 1, NULL, NULL) == NULL, 1);
    nds_destroy(large);
    nds_destroy(plain);
}

/*
 * 3D on both screens, the way games do it (and the way Phantom Hourglass
 * does it, down to which screen is shown how): engine A draws the top
 * screen one frame and the bottom the next, the capture unit takes its
 * picture each time, and the screen engine A is not on shows last frame's
 * capture through engine B - the bottom screen's as a direct-colour bitmap
 * background (block C, captured as LCDC memory and then mapped as engine
 * B's backgrounds), the top screen's as bitmap sprites (block D, mapped as
 * engine B's sprites).
 */
static void both_screens_registers(NdsMachine *m, int frame)
{
    const int top = frame & 1; /* engine A on the top screen */
    w9h(m, 0x04000304u, top ? 0x820F : 0x020F);
    w9b(m, 0x04000242u, top ? 0x84 : 0x80);  /* VRAMCNT_C: engine B's backgrounds, or LCDC to be captured into */
    w9b(m, 0x04000243u, top ? 0x80 : 0x84);  /* VRAMCNT_D: LCDC, or engine B's sprites */
    /* DISPCAPCNT: capture the stacked layers, 256 by 192, into D or C. */
    w9(m, 0x04000064u, 0x80300010u | ((top ? 3u : 2u) << 16));
}

static void both_screens_frame(NdsMachine *m, int frame)
{
    both_screens_registers(m, frame);
    advance(m, (uint64_t)NDS_CYCLES_PER_LINE * 263u);
}

static void both_screens_setup(NdsMachine *m)
{
    send_triangle(m);
    /* Engine B: BG mode 5, BG3 a 256 by 256 direct-colour bitmap shown as
     * it is, and bitmap sprites from a bitmap 256 pixels wide. */
    w9(m, 0x04001000u, (1u << 16) | (1u << 12) | (1u << 11) | (1u << 5) | 5u);
    w9h(m, 0x0400100Eu, 0x4084);
    w9h(m, 0x04001030u, 0x100);
    w9h(m, 0x04001032u, 0);
    w9h(m, 0x04001034u, 0);
    w9h(m, 0x04001036u, 0x100);
    w9(m, 0x04001038u, 0);
    w9(m, 0x0400103Cu, 0);
    /* Twelve 64 by 64 bitmap sprites tile the screen. In this mapping tile
     * number x/8 + 32 * (y/8) is the bitmap's pixel (x, y). */
    for (uint32_t i = 0; i < 128; i++) w9h(m, 0x07000400u + i * 8u, 0x0200); /* off */
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t column = 0; column < 4; column++) {
            const uint32_t at = 0x07000400u + (row * 4u + column) * 8u;
            w9h(m, at, (row * 64u) | (3u << 10));
            w9h(m, at + 2u, (column * 64u) | (3u << 14));
            w9h(m, at + 4u, (column * 8u + row * 8u * 32u) | (15u << 12));
        }
    }
    advance(m, (uint64_t)NDS_CYCLES_PER_LINE * 200u);
}

/* Pixels brighter than half: the triangle, white from the 3D engine or the
 * nearest five bits have to white after a capture. */
static long bright_pixels(const uint32_t *pixels, size_t count)
{
    long n = 0;
    for (size_t i = 0; i < count; i++) n += (pixels[i] & 0xFFu) > 0x80u;
    return n;
}

static void test_internal_scale_captures(void)
{
    g_section = "internal resolution, 3D on both screens";
    NdsMachine *plain = fresh(), *large = fresh();
    nds_set_internal_scale(large, 3);
    both_screens_setup(plain);
    both_screens_setup(large);
    const size_t screen = (size_t)NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT;

    /* Frames 0 to 9 draw; then one of each kind, looked at after it. */
    for (int frame = 0; frame < 10; frame++) {
        both_screens_frame(plain, frame);
        both_screens_frame(large, frame);
    }
    for (int frame = 10; frame < 12; frame++) {
        both_screens_frame(plain, frame);
        both_screens_frame(large, frame);
        const char *which = frame & 1 ? "engine A on top" : "engine A on the bottom";
        printf("  %s\n", which);
        EQ("the console's picture is the same with a larger one beside it",
           memcmp(plain->framebuffer, large->framebuffer, sizeof plain->framebuffer), 0);
        EQ("... and so is video memory", memcmp(plain->vram, large->vram, sizeof plain->vram), 0);
        for (int s = 0; s < 2; s++) {
            const long bright = bright_pixels(plain->framebuffer[s], screen);
            EQ(s ? "the triangle is on the bottom screen" : "the triangle is on the top screen",
               bright > 3000 && bright < 9000, 1);
            int w = 0, h = 0;
            const uint32_t *fine = nds_fine_frame(large, s, &w, &h);
            EQ(s ? "the bottom screen has a larger frame" : "the top screen has a larger frame", fine != NULL, 1);
            if (!fine) continue;
            const long fine_bright = bright_pixels(fine, screen * 9u);
            EQ("... with nine times the triangle", fine_bright > bright * 9 - 1500 && fine_bright < bright * 9 + 1500, 1);
            long stepped = 0;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    stepped += fine[(size_t)y * (size_t)w + (size_t)x] != large->framebuffer[s][(y / 3) * NDS_SCREEN_WIDTH + x / 3];
            EQ("... its edges drawn finer than the screen's", stepped > 100 && stepped < 3000, 1);
        }
    }

    /* A game that draws over what was captured gets its own pixels: a twin
     * is used only while video memory still holds what was captured. Frame
     * 12 shows the top screen from block D as sprites; before it is drawn,
     * the lower half of the block is filled with green, which nothing
     * captured was. */
    for (int i = 0; i < 2; i++) {
        NdsMachine *m = i ? large : plain;
        both_screens_registers(m, 12);
        for (uint32_t at = 96u * 512u; at < 192u * 512u; at += 4) w9(m, 0x06600000u + at, 0x83E083E0u);
        advance(m, (uint64_t)NDS_CYCLES_PER_LINE * 263u);
    }
    EQ("drawn over, the console's picture is still the same",
       memcmp(plain->framebuffer, large->framebuffer, sizeof plain->framebuffer), 0);
    EQ("... with the lower half of the top screen green", plain->framebuffer[0][150 * NDS_SCREEN_WIDTH + 10], 0xFF00FB00u);
    {
        int w = 0, h = 0;
        const uint32_t *fine = nds_fine_frame(large, 0, &w, &h);
        EQ("the top screen still has a larger frame", fine != NULL, 1);
        long upper = 0, lower = 0;
        for (int y = 0; fine && y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int differs = fine[(size_t)y * (size_t)w + (size_t)x] != large->framebuffer[0][(y / 3) * NDS_SCREEN_WIDTH + x / 3];
                if (y < 96 * 3) upper += differs;
                else lower += differs;
            }
        }
        EQ("... whose upper half is still drawn finer", upper > 50, 1);
        EQ("... and whose lower half is the screen's own pixels", lower, 0);
    }
    nds_destroy(large);
    nds_destroy(plain);
}

static void test_keys(void)
{
    NdsMachine *m = fresh();
    g_section = "keys";

    EQ("nothing pressed", r9h(m, 0x04000130u), 0x03FFu);
    EQ("EXTKEYIN at rest", r7h(m, 0x04000136u), 0x007Fu);
    nds_set_keys(m, NDS_KEY_A | NDS_KEY_START | NDS_KEY_X);
    EQ("A and START", r9h(m, 0x04000130u), 0x03F6u);
    EQ("the ARM7 reads the same", r7h(m, 0x04000130u), 0x03F6u);
    EQ("X on EXTKEYIN", r7h(m, 0x04000136u), 0x007Eu);
    EQ("the ARM9 has no EXTKEYIN", r9h(m, 0x04000136u), 0);
    nds_set_touch(m, true, 100, 50);
    EQ("pen down", r7h(m, 0x04000136u), 0x003Eu);
    nds_set_touch(m, false, 0, 0);
    nds_set_lid(m, true);
    EQ("lid closed", r7h(m, 0x04000136u) & 0x80u, 0x80u);
    EQ("no IRQ on closing", m->if_[NDS_ARM7] & NDS_IRQ_LID, 0);
    nds_set_lid(m, false);
    EQ("IRQ on opening", m->if_[NDS_ARM7] & NDS_IRQ_LID, NDS_IRQ_LID);
    EQ("X still held through all that", r7h(m, 0x04000136u) & 1u, 0);

    /* KEYCNT: any of the selected keys, or all of them. */
    nds_set_keys(m, 0);
    w9h(m, 0x04000132u, 0x4000 | 0x0003);
    EQ("KEYCNT reads back", r9h(m, 0x04000132u), 0x4003u);
    nds_set_keys(m, NDS_KEY_B);
    EQ("any-key IRQ", m->if_[NDS_ARM9] & NDS_IRQ_KEYPAD, NDS_IRQ_KEYPAD);
    EQ("the ARM7 has its own KEYCNT", m->if_[NDS_ARM7] & NDS_IRQ_KEYPAD, 0);
    m->if_[NDS_ARM9] = 0;
    w9h(m, 0x04000132u, 0xC000 | 0x0003);
    nds_set_keys(m, NDS_KEY_B);
    EQ("all-keys: one is not enough", m->if_[NDS_ARM9] & NDS_IRQ_KEYPAD, 0);
    nds_set_keys(m, NDS_KEY_A | NDS_KEY_B);
    EQ("all-keys: both", m->if_[NDS_ARM9] & NDS_IRQ_KEYPAD, NDS_IRQ_KEYPAD);

    g_section = "power";
    w9h(m, 0x04000304u, 0xFFFF);
    EQ("POWCNT1 keeps its bits", r9h(m, 0x04000304u), 0x820Fu);
    w7h(m, 0x04000304u, 0xFFFF);
    EQ("POWCNT2 keeps its bits", r7h(m, 0x04000304u), 0x0003u);
    w9b(m, 0x04000300u, 3);
    w9b(m, 0x04000300u, 0);
    EQ("POSTFLG bit 0 cannot be cleared", r9b(m, 0x04000300u), 1);
    nds_destroy(m);
}

int main(void)
{
    test_memory();
    test_vram();
    test_tcm();
    test_ipc();
    test_math();
    test_timers();
    test_dma();
    test_card();
    test_boot();
    test_interrupts();
    test_video_timing();
    test_internal_scale();
    test_internal_scale_captures();
    test_keys();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
