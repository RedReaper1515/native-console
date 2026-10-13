/*
 * Directed tests for what the ARM9 does differently from the ARM7, and for
 * the ARMv5TE instructions. The upstream ARM7TDMI vectors cover the ARM7;
 * the generated ARMv5TE vectors cover the ARM9's predictable instruction
 * space against an independent implementation. This file pins the rest:
 * behaviour that is specific to the DS's ARM946E-S and that neither set can
 * express, each with where it comes from.
 *
 *   GBATEK        problemkaputt.de/gbatek.htm, the ARM CPU chapters
 *   rockwrestler  RockPolish/rockwrestler src9/tests/armv5.s, whose
 *                 expectations pass on hardware
 *   ARM ARM       ARM DDI 0100, the ARMv5TE architecture manual
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "nds_bus.h"
#include "nds_cpu.h"

/* ------------------------------------------------------------------ */
/* A 64 KiB RAM at 0x02000000 for both buses                           */
/* ------------------------------------------------------------------ */

#define RAM_BASE 0x02000000u
#define RAM_SIZE 0x10000u

static uint8_t g_ram[RAM_SIZE];
static int g_accesses;
static int g_cp15_changes;
static int g_waits;

static uint8_t *ram_at(uint32_t addr)
{
    return &g_ram[(addr - RAM_BASE) & (RAM_SIZE - 1u)];
}

static uint32_t ram_read(uint32_t addr, uint32_t size)
{
    uint32_t value = 0;
    g_accesses++;
    addr &= ~(size - 1u);
    for (uint32_t i = 0; i < size; i++) value |= (uint32_t)*ram_at(addr + i) << (8 * i);
    return value;
}

static void ram_write(uint32_t addr, uint32_t size, uint32_t value)
{
    g_accesses++;
    addr &= ~(size - 1u);
    for (uint32_t i = 0; i < size; i++) *ram_at(addr + i) = (uint8_t)(value >> (8 * i));
}

#define DEFINE_BUS(prefix)                                                              \
    uint32_t prefix##_bus_read8(NdsCpu *cpu, uint32_t addr, int access)                 \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        return ram_read(addr, 1);                                                       \
    }                                                                                   \
    uint32_t prefix##_bus_read16(NdsCpu *cpu, uint32_t addr, int access)                \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        return ram_read(addr, 2);                                                       \
    }                                                                                   \
    uint32_t prefix##_bus_read32(NdsCpu *cpu, uint32_t addr, int access)                \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        return ram_read(addr, 4);                                                       \
    }                                                                                   \
    void prefix##_bus_write8(NdsCpu *cpu, uint32_t addr, uint8_t value, int access)     \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        ram_write(addr, 1, value);                                                      \
    }                                                                                   \
    void prefix##_bus_write16(NdsCpu *cpu, uint32_t addr, uint16_t value, int access)   \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        ram_write(addr, 2, value);                                                      \
    }                                                                                   \
    void prefix##_bus_write32(NdsCpu *cpu, uint32_t addr, uint32_t value, int access)   \
    {                                                                                   \
        (void)cpu; (void)access;                                                        \
        ram_write(addr, 4, value);                                                      \
    }                                                                                   \
    void prefix##_bus_idle(NdsCpu *cpu, int cycles)                                     \
    {                                                                                   \
        cpu->cycles += (uint64_t)cycles;                                                \
    }

DEFINE_BUS(nds9)
DEFINE_BUS(nds7)

void nds9_bus_cp15_changed(NdsCpu *cpu)
{
    (void)cpu;
    g_cp15_changes++;
}

void nds9_bus_wait_for_interrupt(NdsCpu *cpu)
{
    (void)cpu;
    g_waits++;
}

static void poke32(uint32_t addr, uint32_t value)
{
    for (uint32_t i = 0; i < 4; i++) *ram_at(addr + i) = (uint8_t)(value >> (8 * i));
}

static uint32_t peek32(uint32_t addr)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; i++) value |= (uint32_t)*ram_at(addr + i) << (8 * i);
    return value;
}

/* ------------------------------------------------------------------ */
/* Test plumbing                                                       */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;
static const char *g_section = "";

static void check_eq(const char *what, uint32_t got, uint32_t want, int line)
{
    g_checks++;
    if (got == want) return;
    g_failures++;
    printf("FAIL %s: %s = %08" PRIX32 ", expected %08" PRIX32 " (line %d)\n", g_section, what, got,
           want, line);
}

#define EQ(what, got, want) check_eq(what, (uint32_t)(got), (uint32_t)(want), __LINE__)

#define CODE 0x02000100u /* where the instruction under test "is" */
#define DATA 0x02004000u

/* A fresh ARM9 in system mode with ARM state, or an ARM7 likewise. */
static void fresh9(NdsCpu *c)
{
    memset(c, 0, sizeof *c);
    nds9_cpu_reset(c);
    nds_cpu_set_cpsr(c, NDS_MODE_SYS);
    memset(g_ram, 0, sizeof g_ram);
    g_accesses = 0;
}

static void fresh7(NdsCpu *c)
{
    memset(c, 0, sizeof *c);
    nds7_cpu_reset(c);
    nds_cpu_set_cpsr(c, NDS_MODE_SYS);
    memset(g_ram, 0, sizeof g_ram);
    g_accesses = 0;
}

static void arm9(NdsCpu *c, uint32_t op)
{
    c->cpsr &= ~NDS_CPSR_T;
    c->r[15] = CODE + 8;
    nds9_cpu_execute_arm(c, op);
}

static void thumb9(NdsCpu *c, uint16_t op)
{
    c->cpsr |= NDS_CPSR_T;
    c->r[15] = CODE + 4;
    nds9_cpu_execute_thumb(c, op);
}

static void arm7(NdsCpu *c, uint32_t op)
{
    c->cpsr &= ~NDS_CPSR_T;
    c->r[15] = CODE + 8;
    nds7_cpu_execute_arm(c, op);
}

static void thumb7(NdsCpu *c, uint16_t op)
{
    c->cpsr |= NDS_CPSR_T;
    c->r[15] = CODE + 4;
    nds7_cpu_execute_thumb(c, op);
}

/* ------------------------------------------------------------------ */
/* LDM and STM (rockwrestler's cases, numbered as there)                */
/* ------------------------------------------------------------------ */

static void test_block_transfer(void)
{
    NdsCpu c;
    g_section = "ldm/stm";

    /* 0-1: ldmia r1!, {r2} */
    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    c.r[1] = DATA;
    arm9(&c, 0xE8B10004u);
    EQ("r2", c.r[2], 0xC0FFEEABu);
    EQ("r1", c.r[1], DATA + 4);

    /* 2-3: stmia r1!, {r2} */
    fresh9(&c);
    c.r[1] = DATA;
    c.r[2] = 0xC0FFEEABu;
    arm9(&c, 0xE8A10004u);
    EQ("[r1]", peek32(DATA), 0xC0FFEEABu);
    EQ("r1", c.r[1], DATA + 4);

    /* 4: ldmia r1!, {r1}: the base is the only register, so the writeback
     * wins over the loaded value. */
    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    c.r[1] = DATA;
    arm9(&c, 0xE8B10002u);
    EQ("r1 (only register)", c.r[1], DATA + 4);

    /* 5-7: ldmia r3!, {r1-r3}: the base is last, so the loaded value stays. */
    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    poke32(DATA + 4, 0xDEADBEEFu);
    poke32(DATA + 8, 0xD00DFEEDu);
    c.r[3] = DATA;
    arm9(&c, 0xE8B3000Eu);
    EQ("r1", c.r[1], 0xC0FFEEABu);
    EQ("r2", c.r[2], 0xDEADBEEFu);
    EQ("r3 (base last)", c.r[3], 0xD00DFEEDu);

    /* 8-A: ldmia r2!, {r1-r3}: the base is not last, so the writeback wins. */
    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    poke32(DATA + 4, 0xDEADBEEFu);
    poke32(DATA + 8, 0xD00DFEEDu);
    c.r[2] = DATA;
    arm9(&c, 0xE8B2000Eu);
    EQ("r1", c.r[1], 0xC0FFEEABu);
    EQ("r3", c.r[3], 0xD00DFEEDu);
    EQ("r2 (base not last)", c.r[2], DATA + 12);

    /* B: ldmia r2!, {}: nothing is transferred and the base moves by 0x40. */
    fresh9(&c);
    c.r[2] = DATA;
    arm9(&c, 0xE8B20000u);
    EQ("r2 (empty list)", c.r[2], DATA + 0x40);
    EQ("bus accesses (empty list)", g_accesses, 0);
    EQ("branched (empty list)", c.branched, 0);

    /* The same for a decrementing store: stmdb r2!, {} (GBATEK). */
    fresh9(&c);
    c.r[2] = DATA;
    arm9(&c, 0xE9220000u);
    EQ("r2 (empty stmdb)", c.r[2], DATA - 0x40);
    EQ("bus accesses (empty stmdb)", g_accesses, 0);

    /* C-F: ldmia r13!, {r14}^ in IRQ mode: the user bank's r14 is loaded,
     * and the writeback goes to the IRQ bank's r13. */
    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    c.r[13] = 0x11111111u; /* user r13 */
    c.r[14] = 0x22222222u; /* user r14 */
    nds_cpu_set_cpsr(&c, NDS_MODE_IRQ);
    c.r[13] = DATA;
    c.r[14] = 0;
    arm9(&c, 0xE8FD4000u);
    EQ("r14_irq", c.r[14], 0);
    EQ("r13_irq", c.r[13], DATA + 4);
    EQ("r14_usr", nds_cpu_get_user_reg(&c, 14), 0xC0FFEEABu);
    EQ("r13_usr", nds_cpu_get_user_reg(&c, 13), 0x11111111u);

    /* 10-11: ldmia r13, {r13}^ in IRQ mode. */
    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    nds_cpu_set_cpsr(&c, NDS_MODE_IRQ);
    c.r[13] = DATA;
    arm9(&c, 0xE8DD2000u);
    EQ("r13_irq unchanged", c.r[13], DATA);
    EQ("r13_usr loaded", nds_cpu_get_user_reg(&c, 13), 0xC0FFEEABu);

    /* 12-13: stmia r1!, {r1} stores the old base. */
    fresh9(&c);
    c.r[1] = DATA;
    arm9(&c, 0xE8A10002u);
    EQ("r1", c.r[1], DATA + 4);
    EQ("[old r1]", peek32(DATA), DATA);

    /* 14-17: stmia r3!, {r1-r3} stores the old base even when it is not
     * first (the ARM7TDMI stores the new one there). */
    fresh9(&c);
    c.r[1] = 0xDEADBEEFu;
    c.r[2] = 0xC0FFEEABu;
    c.r[3] = DATA;
    arm9(&c, 0xE8A3000Eu);
    EQ("[r3+0]", peek32(DATA), 0xDEADBEEFu);
    EQ("[r3+4]", peek32(DATA + 4), 0xC0FFEEABu);
    EQ("[r3+8] (old base)", peek32(DATA + 8), DATA);
    EQ("r3", c.r[3], DATA + 12);

    /* Thumb LDMIA is not the ARM-state instruction: with the base in the
     * list it never writes the base back, wherever the base is in the list
     * (GBATEK, THUMB.15). picolibc's gmtime_r does "ldmia r0!, {r0, r1}",
     * which BlocksDS's ipc/transfer_region test prints the date with. */
    g_section = "thumb ldmia";
    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    poke32(DATA + 4, 0xDEADBEEFu);
    c.r[0] = DATA;
    thumb9(&c, 0xC803u); /* ldmia r0!, {r0, r1}: the base first of two */
    EQ("r0 (base first: loaded)", c.r[0], 0xC0FFEEABu);
    EQ("r1", c.r[1], 0xDEADBEEFu);

    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    c.r[1] = DATA;
    thumb9(&c, 0xC902u); /* ldmia r1!, {r1}: the base the only register */
    EQ("r1 (base only: loaded)", c.r[1], 0xC0FFEEABu);

    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    poke32(DATA + 4, 0xDEADBEEFu);
    c.r[1] = DATA;
    thumb9(&c, 0xC903u); /* ldmia r1!, {r0, r1}: the base last */
    EQ("r0", c.r[0], 0xC0FFEEABu);
    EQ("r1 (base last: loaded)", c.r[1], 0xDEADBEEFu);

    fresh9(&c);
    poke32(DATA, 0xC0FFEEABu);
    poke32(DATA + 4, 0xDEADBEEFu);
    c.r[2] = DATA;
    thumb9(&c, 0xCA03u); /* ldmia r2!, {r0, r1}: the base not in the list */
    EQ("r0", c.r[0], 0xC0FFEEABu);
    EQ("r2 (written back)", c.r[2], DATA + 8);

    fresh7(&c);
    poke32(DATA, 0xC0FFEEABu);
    poke32(DATA + 4, 0xDEADBEEFu);
    c.r[0] = DATA;
    thumb7(&c, 0xC803u);
    EQ("arm7 r0 (base first: loaded)", c.r[0], 0xC0FFEEABu);
    EQ("arm7 r1", c.r[1], 0xDEADBEEFu);

    /* The ARM7 keeps the ARM7TDMI behaviour for the same instructions. */
    g_section = "ldm/stm arm7";
    fresh7(&c);
    c.r[1] = 0xDEADBEEFu;
    c.r[2] = 0xC0FFEEABu;
    c.r[3] = DATA;
    arm7(&c, 0xE8A3000Eu);
    EQ("[r3+8] (new base)", peek32(DATA + 8), DATA + 12);

    fresh7(&c);
    poke32(DATA, 0xC0FFEEABu);
    c.r[1] = DATA;
    arm7(&c, 0xE8B10002u);
    EQ("r1 (loaded, no writeback)", c.r[1], 0xC0FFEEABu);

    fresh7(&c);
    poke32(DATA, 0x02000200u);
    c.r[2] = DATA;
    arm7(&c, 0xE8B20000u);
    EQ("r2 (empty list)", c.r[2], DATA + 0x40);
    EQ("r15 loaded (empty list)", c.r[15], 0x02000200u + 8);
}

/* ------------------------------------------------------------------ */
/* Which instructions change the instruction set                        */
/* ------------------------------------------------------------------ */

static void test_interworking(void)
{
    NdsCpu c;
    g_section = "interworking";

    /* ldr pc, [r0]: bit 0 of the loaded word selects Thumb (GBATEK,
     * rockwrestler "ldrpopr15"). */
    fresh9(&c);
    poke32(DATA, 0x02000201u);
    c.r[0] = DATA;
    arm9(&c, 0xE590F000u);
    EQ("ldr pc: T", nds_cpu_thumb(&c), 1);
    EQ("ldr pc: r15", c.r[15], 0x02000200u + 4);

    fresh9(&c);
    poke32(DATA, 0x02000202u);
    c.r[0] = DATA;
    arm9(&c, 0xE590F000u);
    EQ("ldr pc even: T", nds_cpu_thumb(&c), 0);
    EQ("ldr pc even: r15 aligned", c.r[15], 0x02000200u + 8);

    /* CP15 control bit 15 turns that off (GBATEK, CP15 control register). */
    fresh9(&c);
    nds9_cp15_write(&c, 0x100, c.cp15.control | NDS_CP15_PRE_ARMV5);
    poke32(DATA, 0x02000201u);
    c.r[0] = DATA;
    arm9(&c, 0xE590F000u);
    EQ("ldr pc, pre-ARMv5 mode: T", nds_cpu_thumb(&c), 0);
    EQ("ldr pc, pre-ARMv5 mode: r15", c.r[15], 0x02000200u + 8);

    /* The ARM7 never switches on a load. */
    fresh7(&c);
    poke32(DATA, 0x02000201u);
    c.r[0] = DATA;
    arm7(&c, 0xE590F000u);
    EQ("arm7 ldr pc: T", nds_cpu_thumb(&c), 0);

    /* ldmia r0, {pc} */
    fresh9(&c);
    poke32(DATA, 0x02000201u);
    c.r[0] = DATA;
    arm9(&c, 0xE8908000u);
    EQ("ldm pc: T", nds_cpu_thumb(&c), 1);
    EQ("ldm pc: r15", c.r[15], 0x02000200u + 4);

    /* Thumb pop {pc} to an even address goes to ARM state. */
    fresh9(&c);
    poke32(DATA, 0x02000200u);
    c.r[13] = DATA;
    thumb9(&c, 0xBD00u);
    EQ("pop pc even: T", nds_cpu_thumb(&c), 0);
    EQ("pop pc even: r15", c.r[15], 0x02000200u + 8);
    EQ("pop pc: sp", c.r[13], DATA + 4);

    fresh9(&c);
    poke32(DATA, 0x02000201u);
    c.r[13] = DATA;
    thumb9(&c, 0xBD00u);
    EQ("pop pc odd: T", nds_cpu_thumb(&c), 1);
    EQ("pop pc odd: r15", c.r[15], 0x02000200u + 4);

    /* On the ARM7 pop {pc} stays in Thumb state. */
    fresh7(&c);
    poke32(DATA, 0x02000200u);
    c.r[13] = DATA;
    thumb7(&c, 0xBD00u);
    EQ("arm7 pop pc: T", nds_cpu_thumb(&c), 1);

    /* mov pc, r0 does not change state on either (rockwrestler). */
    fresh9(&c);
    c.r[0] = 0x02000201u;
    arm9(&c, 0xE1A0F000u);
    EQ("mov pc: T", nds_cpu_thumb(&c), 0);
    EQ("mov pc: r15", c.r[15], 0x02000200u + 8);

    /* Thumb mov pc, r0 (0x4687) likewise stays in Thumb. */
    fresh9(&c);
    c.r[0] = 0x02000200u;
    thumb9(&c, 0x4687u);
    EQ("thumb mov pc: T", nds_cpu_thumb(&c), 1);
    EQ("thumb mov pc: r15", c.r[15], 0x02000200u + 4);

    /* blx r0 */
    fresh9(&c);
    c.r[0] = 0x02000201u;
    arm9(&c, 0xE12FFF30u);
    EQ("blx reg: T", nds_cpu_thumb(&c), 1);
    EQ("blx reg: r15", c.r[15], 0x02000200u + 4);
    EQ("blx reg: lr", c.r[14], CODE + 4);

    /* blx lr reads the target before writing the link. */
    fresh9(&c);
    c.r[14] = 0x02000300u;
    arm9(&c, 0xE12FFF3Eu);
    EQ("blx lr: r15", c.r[15], 0x02000300u + 8);
    EQ("blx lr: lr", c.r[14], CODE + 4);

    /* blx label: always to Thumb; H adds 2. Offset +0x10 from pc+8. */
    fresh9(&c);
    arm9(&c, 0xFA000004u);
    EQ("blx imm: T", nds_cpu_thumb(&c), 1);
    EQ("blx imm: r15", c.r[15], CODE + 8 + 0x10 + 4);
    EQ("blx imm: lr", c.r[14], CODE + 4);

    fresh9(&c);
    arm9(&c, 0xFB000004u);
    EQ("blx imm H: r15", c.r[15], CODE + 8 + 0x12 + 4);

    /* Thumb blx r0 (0x4780): lr = next instruction | 1. */
    fresh9(&c);
    c.r[0] = 0x02000200u;
    thumb9(&c, 0x4780u);
    EQ("thumb blx reg: T", nds_cpu_thumb(&c), 0);
    EQ("thumb blx reg: r15", c.r[15], 0x02000200u + 8);
    EQ("thumb blx reg: lr", c.r[14], (CODE + 2) | 1u);

    /* On the ARM7 the same encoding is a plain bx. */
    fresh7(&c);
    c.r[0] = 0x02000200u;
    c.r[14] = 0x55555555u;
    thumb7(&c, 0x4780u);
    EQ("arm7 0x4780: lr untouched", c.r[14], 0x55555555u);
    EQ("arm7 0x4780: T", nds_cpu_thumb(&c), 0);

    /* Thumb blx suffix: to ARM state, target word-aligned. */
    fresh9(&c);
    c.r[14] = 0x02000202u;
    thumb9(&c, 0xE800u | 0x004u); /* + 8 */
    EQ("thumb blx suffix: T", nds_cpu_thumb(&c), 0);
    EQ("thumb blx suffix: r15", c.r[15], 0x02000208u + 8);
    EQ("thumb blx suffix: lr", c.r[14], (CODE + 2) | 1u);

    /* An odd offset in the blx suffix is undefined (ARM ARM; melonDS). */
    fresh9(&c);
    c.r[14] = 0x02000200u;
    thumb9(&c, 0xE801u);
    EQ("thumb blx suffix odd: mode", nds_cpu_mode(&c), NDS_MODE_UND);
    EQ("thumb blx suffix odd: r15", c.r[15], 0xFFFF0004u + 8);

    /* And the suffix does not exist on the ARM7 at all. */
    fresh7(&c);
    thumb7(&c, 0xE800u);
    EQ("arm7 blx suffix: mode", nds_cpu_mode(&c), NDS_MODE_UND);
    EQ("arm7 blx suffix: r15", c.r[15], 0x00000004u + 8);

    /* msr cpsr_c cannot set T on the ARM9. */
    fresh9(&c);
    c.r[0] = NDS_MODE_SYS | NDS_CPSR_T;
    arm9(&c, 0xE121F000u);
    EQ("msr cpsr_c: T", nds_cpu_thumb(&c), 0);

    /* Exception return: any data-processing opcode with S and Rd = R15
     * copies SPSR to CPSR (ARM ARM), and the instruction set is then the
     * one SPSR names. The generated vectors can only cover SUBS and MOVS,
     * so check two of the others: adds pc, lr, #0 and eors pc, lr, #0. */
    static const uint32_t returns[] = { 0xE29EF000u, 0xE23EF000u };
    for (int i = 0; i < 2; i++) {
        fresh9(&c);
        nds_cpu_set_cpsr(&c, NDS_MODE_IRQ | NDS_CPSR_I);
        c.spsr[NDS_BANK_IRQ] = 0x60000000u | NDS_MODE_SYS | NDS_CPSR_T;
        c.r[14] = 0x02000203u;
        c.flag_n = 1;
        arm9(&c, returns[i]);
        EQ("exception return: cpsr", nds_cpu_get_cpsr(&c), 0x60000000u | NDS_MODE_SYS | NDS_CPSR_T);
        EQ("exception return: r15", c.r[15], 0x02000202u + 4);

        fresh9(&c);
        nds_cpu_set_cpsr(&c, NDS_MODE_SVC);
        c.spsr[NDS_BANK_SVC] = NDS_MODE_USR;
        c.r[14] = 0x02000206u;
        arm9(&c, returns[i]);
        EQ("exception return to ARM: cpsr", nds_cpu_get_cpsr(&c), NDS_MODE_USR);
        EQ("exception return to ARM: r15 aligned", c.r[15], 0x02000204u + 8);
    }
}

/* ------------------------------------------------------------------ */
/* Loads from odd addresses                                            */
/* ------------------------------------------------------------------ */

static void test_misaligned(void)
{
    NdsCpu c;
    g_section = "misaligned";

    /* ldrh r1, [r0] with r0 odd: the ARM9 reads the aligned halfword. */
    fresh9(&c);
    poke32(DATA, 0x8899AABBu);
    c.r[0] = DATA + 1;
    arm9(&c, 0xE1D010B0u);
    EQ("arm9 ldrh odd", c.r[1], 0x0000AABBu);

    /* The ARM7TDMI rotates it by 8. */
    fresh7(&c);
    poke32(DATA, 0x8899AABBu);
    c.r[0] = DATA + 1;
    arm7(&c, 0xE1D010B0u);
    EQ("arm7 ldrh odd", c.r[1], 0xBB0000AAu);

    /* ldrsh r1, [r0] with r0 odd: aligned halfword, sign-extended. */
    fresh9(&c);
    poke32(DATA, 0x8899AABBu);
    c.r[0] = DATA + 1;
    arm9(&c, 0xE1D010F0u);
    EQ("arm9 ldrsh odd", c.r[1], 0xFFFFAABBu);

    /* The ARM7TDMI sign-extends the addressed byte. */
    fresh7(&c);
    poke32(DATA, 0x8899AABBu);
    c.r[0] = DATA + 1;
    arm7(&c, 0xE1D010F0u);
    EQ("arm7 ldrsh odd", c.r[1], 0xFFFFFFAAu);

    /* ldr r1, [r0] rotates on both. */
    fresh9(&c);
    poke32(DATA, 0x8899AABBu);
    c.r[0] = DATA + 1;
    arm9(&c, 0xE5901000u);
    EQ("arm9 ldr odd", c.r[1], 0xBB8899AAu);
}

/* ------------------------------------------------------------------ */
/* CLZ, saturating arithmetic, the 16-bit multiplies                    */
/* ------------------------------------------------------------------ */

static uint32_t q_flag(const NdsCpu *c)
{
    return (c->cpsr & NDS_CPSR_Q) ? 1u : 0u;
}

static void test_dsp(void)
{
    NdsCpu c;
    g_section = "clz";
    static const uint32_t clz_in[] = { 0, 1, 0x80000000u, 0x00010000u, 0x7FFFFFFFu };
    static const uint32_t clz_out[] = { 32, 31, 0, 15, 1 };
    for (int i = 0; i < 5; i++) {
        fresh9(&c);
        c.r[0] = clz_in[i];
        arm9(&c, 0xE16F1F10u); /* clz r1, r0 */
        EQ("clz", c.r[1], clz_out[i]);
    }

    /* On the ARM7 the encoding is not CLZ: r1 is left alone. */
    fresh7(&c);
    c.r[0] = 1;
    c.r[1] = 0x12345678u;
    arm7(&c, 0xE16F1F10u);
    EQ("arm7 clz encoding leaves r1", c.r[1], 0x12345678u);

    g_section = "qadd";
    /* qadd r1, r2, r3 (rockwrestler) */
    fresh9(&c);
    c.r[2] = 2; c.r[3] = 3;
    arm9(&c, 0xE1031052u);
    EQ("2+3", c.r[1], 5);
    EQ("2+3 Q", q_flag(&c), 0);

    c.r[2] = 0x7FFFFFFFu; c.r[3] = 1;
    arm9(&c, 0xE1031052u);
    EQ("max+1", c.r[1], 0x7FFFFFFFu);
    EQ("max+1 Q", q_flag(&c), 1);

    /* Q is sticky. */
    c.r[2] = 6; c.r[3] = 7;
    arm9(&c, 0xE1031052u);
    EQ("6+7", c.r[1], 13);
    EQ("6+7 Q still set", q_flag(&c), 1);

    /* msr cpsr_f, r4 with r4 = 0 clears it. */
    c.r[4] = 0;
    arm9(&c, 0xE128F004u);
    EQ("Q cleared by msr", q_flag(&c), 0);

    c.r[2] = 0x80000000u; c.r[3] = 0xFFFFFFFFu;
    arm9(&c, 0xE1031052u);
    EQ("min-1", c.r[1], 0x80000000u);
    EQ("min-1 Q", q_flag(&c), 1);

    g_section = "qsub";
    fresh9(&c);
    c.r[2] = 0x7FFFFFFFu; c.r[3] = 0xFFFFFFFFu; /* max - (-1) */
    arm9(&c, 0xE1231052u);
    EQ("max-(-1)", c.r[1], 0x7FFFFFFFu);
    EQ("max-(-1) Q", q_flag(&c), 1);

    fresh9(&c);
    c.r[2] = 0x80000000u; c.r[3] = 1;
    arm9(&c, 0xE1231052u);
    EQ("min-1", c.r[1], 0x80000000u);
    EQ("min-1 Q", q_flag(&c), 1);

    fresh9(&c);
    c.r[2] = 5; c.r[3] = 9;
    arm9(&c, 0xE1231052u);
    EQ("5-9", c.r[1], 0xFFFFFFFCu);
    EQ("5-9 Q", q_flag(&c), 0);

    g_section = "qdadd";
    /* qdadd r1, r2, r3: r2 + sat(2 * r3) (rockwrestler) */
    fresh9(&c);
    c.r[2] = 0x80000000u; c.r[3] = 0x7FFFFFFEu; /* doubling saturates to max */
    arm9(&c, 0xE1431052u);
    EQ("min + sat(2*big)", c.r[1], 0xFFFFFFFFu);
    EQ("min + sat(2*big) Q", q_flag(&c), 1);

    fresh9(&c);
    c.r[2] = 0x80000000u; c.r[3] = 0x3FFFFFFFu; /* doubles without clamping */
    arm9(&c, 0xE1431052u);
    EQ("min + 2*0x3FFFFFFF", c.r[1], 0xFFFFFFFEu);
    EQ("min + 2*0x3FFFFFFF Q", q_flag(&c), 0);

    fresh9(&c);
    c.r[2] = 0x7FFFFFFFu; c.r[3] = 0xBFFFFFFFu; /* doubling clamps to min */
    arm9(&c, 0xE1431052u);
    EQ("max + sat(2*neg)", c.r[1], 0xFFFFFFFFu);
    EQ("max + sat(2*neg) Q", q_flag(&c), 1);

    g_section = "qdsub";
    fresh9(&c);
    c.r[2] = 0x7FFFFFFFu; c.r[3] = 0x40000000u; /* doubling clamps to max */
    arm9(&c, 0xE1631052u);
    EQ("max - sat(2*0x40000000)", c.r[1], 0);
    EQ("max - sat(2*0x40000000) Q", q_flag(&c), 1);

    fresh9(&c);
    c.r[2] = 100; c.r[3] = 5;
    arm9(&c, 0xE1631052u);
    EQ("100 - 10", c.r[1], 90);
    EQ("100 - 10 Q", q_flag(&c), 0);

    g_section = "smlaxy";
    /* smlabb r1, r2, r3, r5 = r2.lo * r3.lo + r5 (rockwrestler) */
    fresh9(&c);
    c.r[2] = 7; c.r[3] = 10; c.r[5] = 2;
    arm9(&c, 0xE1015382u);
    EQ("7*10+2", c.r[1], 72);
    EQ("7*10+2 Q", q_flag(&c), 0);

    fresh9(&c);
    c.r[2] = 0x0000FFFDu; c.r[3] = 11; c.r[5] = 100; /* -3 * 11 + 100 */
    arm9(&c, 0xE1015382u);
    EQ("-3*11+100", c.r[1], 67);

    fresh9(&c);
    c.r[2] = 0x00008000u; c.r[3] = 0x00008000u; c.r[5] = 0x40000000u; /* 2^30 + 2^30 */
    arm9(&c, 0xE1015382u);
    EQ("overflow result wraps", c.r[1], 0x80000000u);
    EQ("overflow sets Q", q_flag(&c), 1);

    /* smlatt r1, r2, r3, r5 uses the top halves. */
    fresh9(&c);
    c.r[2] = 0xFFFC1234u; c.r[3] = 0x00055678u; c.r[5] = 1; /* -4 * 5 + 1 */
    arm9(&c, 0xE10153E2u);
    EQ("-4*5+1 (top halves)", c.r[1], 0xFFFFFFEDu);

    g_section = "smulxy";
    /* smulbb r1, r2, r3 */
    fresh9(&c);
    c.r[2] = 0x12348000u; c.r[3] = 0x56788000u;
    arm9(&c, 0xE1610382u);
    EQ("-32768*-32768", c.r[1], 0x40000000u);
    EQ("smul never sets Q", q_flag(&c), 0);

    /* smultb r1, r2, r3: top of r2, bottom of r3 */
    fresh9(&c);
    c.r[2] = 0x00030000u; c.r[3] = 0x0000FFFEu;
    arm9(&c, 0xE16103A2u);
    EQ("3*-2", c.r[1], 0xFFFFFFFAu);

    g_section = "smulwy/smlawy";
    /* smulwb r1, r2, r3 = (r2 * r3.lo) >> 16 */
    fresh9(&c);
    c.r[2] = 0x00070000u; c.r[3] = 0x0000FFFDu;
    arm9(&c, 0xE12103A2u);
    EQ("(7<<16)*-3>>16", c.r[1], 0xFFFFFFEBu);

    /* smlawb r1, r2, r3, r5 = (r2 * r3.lo) >> 16 + r5 */
    fresh9(&c);
    c.r[2] = 0x00070000u; c.r[3] = 0x0000FFFDu; c.r[5] = 100;
    arm9(&c, 0xE1215382u);
    EQ("100 + (7<<16)*-3>>16", c.r[1], 79);
    EQ("no Q", q_flag(&c), 0);

    fresh9(&c);
    c.r[2] = 0x7FFFFFFFu; c.r[3] = 0x00007FFFu; c.r[5] = 0x7FFFFFFFu;
    arm9(&c, 0xE1215382u);
    EQ("smlawb overflow sets Q", q_flag(&c), 1);

    g_section = "smlalxy";
    /* smlalbb r1 (lo), r4 (hi), r2, r3 */
    fresh9(&c);
    c.r[1] = 0xFFFFFFFFu; c.r[4] = 0;
    c.r[2] = 2; c.r[3] = 3;
    arm9(&c, 0xE1441382u);
    EQ("lo", c.r[1], 5);
    EQ("hi (carry)", c.r[4], 1);

    fresh9(&c);
    c.r[1] = 0; c.r[4] = 0;
    c.r[2] = 0x0000FFFFu; c.r[3] = 1; /* -1 */
    arm9(&c, 0xE1441382u);
    EQ("lo (-1)", c.r[1], 0xFFFFFFFFu);
    EQ("hi (-1)", c.r[4], 0xFFFFFFFFu);
    EQ("smlal never sets Q", q_flag(&c), 0);

    g_section = "mul flags";
    /* muls r1, r2, r3 leaves C alone on the ARM9 (GBATEK). */
    fresh9(&c);
    c.flag_c = 1;
    c.r[2] = 3; c.r[3] = 0xFFFFFFFFu;
    arm9(&c, 0xE0110392u);
    EQ("result", c.r[1], 0xFFFFFFFDu);
    EQ("N", c.flag_n, 1);
    EQ("C kept (set)", c.flag_c, 1);
    fresh9(&c);
    c.flag_c = 0;
    c.r[2] = 0x10000u; c.r[3] = 0x10000u;
    arm9(&c, 0xE0110392u);
    EQ("Z", c.flag_z, 1);
    EQ("C kept (clear)", c.flag_c, 0);

    /* Thumb muls r0, r1 (0x4348) likewise. */
    fresh9(&c);
    c.flag_c = 1;
    c.r[0] = 5; c.r[1] = 6;
    thumb9(&c, 0x4348u);
    EQ("thumb mul", c.r[0], 30);
    EQ("thumb mul C kept", c.flag_c, 1);
}

/* ------------------------------------------------------------------ */
/* LDRD and STRD                                                       */
/* ------------------------------------------------------------------ */

static void test_double(void)
{
    NdsCpu c;
    g_section = "ldrd/strd";

    /* ldrd r2, [r0] */
    fresh9(&c);
    poke32(DATA, 0x11111111u);
    poke32(DATA + 4, 0x22222222u);
    c.r[0] = DATA;
    arm9(&c, 0xE1C020D0u);
    EQ("ldrd r2", c.r[2], 0x11111111u);
    EQ("ldrd r3", c.r[3], 0x22222222u);
    EQ("ldrd base", c.r[0], DATA);

    /* strd r2, [r0, #8]! */
    fresh9(&c);
    c.r[0] = DATA;
    c.r[2] = 0xAAAAAAAAu;
    c.r[3] = 0xBBBBBBBBu;
    arm9(&c, 0xE1E020F8u);
    EQ("strd low", peek32(DATA + 8), 0xAAAAAAAAu);
    EQ("strd high", peek32(DATA + 12), 0xBBBBBBBBu);
    EQ("strd writeback", c.r[0], DATA + 8);

    /* ldrd r2, [r0], #-8 (post-indexed, down) */
    fresh9(&c);
    poke32(DATA, 0x11111111u);
    poke32(DATA + 4, 0x22222222u);
    c.r[0] = DATA;
    arm9(&c, 0xE04020D8u);
    EQ("ldrd post r2", c.r[2], 0x11111111u);
    EQ("ldrd post r3", c.r[3], 0x22222222u);
    EQ("ldrd post base", c.r[0], DATA - 8);

    /* An odd Rd is undefined (ARM ARM). */
    fresh9(&c);
    c.r[0] = DATA;
    arm9(&c, 0xE1C030D0u);
    EQ("ldrd odd Rd: mode", nds_cpu_mode(&c), NDS_MODE_UND);

    /* On the ARM7 the encoding transfers nothing. */
    fresh7(&c);
    poke32(DATA, 0x11111111u);
    c.r[0] = DATA;
    c.r[2] = 0x77777777u;
    arm7(&c, 0xE1C020D0u);
    EQ("arm7 ldrd encoding leaves r2", c.r[2], 0x77777777u);
    EQ("arm7 ldrd encoding: no bus access", g_accesses, 0);
}

/* ------------------------------------------------------------------ */
/* Exceptions and the vector base                                      */
/* ------------------------------------------------------------------ */

static void test_exceptions(void)
{
    NdsCpu c;
    g_section = "exceptions";

    /* bkpt: prefetch abort, LR = instruction + 4 (ARM ARM, GBATEK). */
    fresh9(&c);
    c.flag_c = 1;
    arm9(&c, 0xE1200070u);
    EQ("bkpt: mode", nds_cpu_mode(&c), NDS_MODE_ABT);
    EQ("bkpt: r15", c.r[15], 0xFFFF000Cu + 8);
    EQ("bkpt: lr", c.r[14], CODE + 4);
    EQ("bkpt: spsr", c.spsr[NDS_BANK_ABT], NDS_MODE_SYS | 0x20000000u);
    EQ("bkpt: I", (c.cpsr & NDS_CPSR_I) != 0, 1);

    fresh9(&c);
    thumb9(&c, 0xBE00u);
    EQ("thumb bkpt: mode", nds_cpu_mode(&c), NDS_MODE_ABT);
    EQ("thumb bkpt: T cleared", nds_cpu_thumb(&c), 0);
    EQ("thumb bkpt: lr", c.r[14], CODE + 4);
    EQ("thumb bkpt: spsr T", (c.spsr[NDS_BANK_ABT] & NDS_CPSR_T) != 0, 1);

    /* On the ARM7 0xBE00 is undefined. */
    fresh7(&c);
    thumb7(&c, 0xBE00u);
    EQ("arm7 0xBE00: mode", nds_cpu_mode(&c), NDS_MODE_UND);

    /* Undefined instruction at the high vectors, then at the low ones
     * after CP15 control bit 13 is cleared. */
    fresh9(&c);
    arm9(&c, 0xE7F000F0u);
    EQ("udf: mode", nds_cpu_mode(&c), NDS_MODE_UND);
    EQ("udf: r15 (high vectors)", c.r[15], 0xFFFF0004u + 8);
    EQ("udf: lr", c.r[14], CODE + 4);

    fresh9(&c);
    nds9_cp15_write(&c, 0x100, c.cp15.control & ~NDS_CP15_HIGH_VECTORS);
    arm9(&c, 0xE7F000F0u);
    EQ("udf: r15 (low vectors)", c.r[15], 0x00000004u + 8);

    /* swi from Thumb: LR = next instruction. */
    fresh9(&c);
    thumb9(&c, 0xDF05u);
    EQ("thumb swi: mode", nds_cpu_mode(&c), NDS_MODE_SVC);
    EQ("thumb swi: r15", c.r[15], 0xFFFF0008u + 8);
    EQ("thumb swi: lr", c.r[14], CODE + 2);

    /* Thumb conditional branch with condition 14: undefined on the ARM9
     * (ARM ARM), "always" on the ARM7TDMI (its test vectors). */
    fresh9(&c);
    thumb9(&c, 0xDE10u);
    EQ("thumb bcc 14: mode", nds_cpu_mode(&c), NDS_MODE_UND);
    fresh7(&c);
    thumb7(&c, 0xDE10u);
    EQ("arm7 thumb bcc 14: branches", c.r[15], CODE + 4 + 0x20 + 4);

    /* The unconditional space: pld does nothing; the rest is undefined. */
    fresh9(&c);
    c.r[0] = DATA;
    arm9(&c, 0xF5D0F000u);
    EQ("pld: no bus access", g_accesses, 0);
    EQ("pld: r15 advances", c.r[15], CODE + 12);
    EQ("pld: mode", nds_cpu_mode(&c), NDS_MODE_SYS);

    fresh9(&c);
    arm9(&c, 0xF1A00000u); /* mov r0, r0 with condition 1111 */
    EQ("cond 1111 mov: mode", nds_cpu_mode(&c), NDS_MODE_UND);

    /* Encodings ARMv6 later allocated are undefined on the ARM9 (ARM ARM,
     * "extending the instruction set": unallocated encodings in these
     * spaces are undefined from ARMv5). The ARM7TDMI decodes them as the
     * multiply or swap they resemble. */
    static const uint32_t later[] = {
        0xE0412493u, /* umaal r2, r1, r3, r4 */
        0xE0612493u, /* mls r1, r3, r4, r2 */
        0xE1912F9Fu, /* ldrex r2, [r1] */
        0xE1812F93u, /* strex r2, r3, [r1] */
        0xE3001234u, /* movw r1, #0x234 */
        0xE12FFF21u, /* bxj r1 */
    };
    for (int i = 0; i < 6; i++) {
        fresh9(&c);
        c.r[1] = DATA;
        arm9(&c, later[i]);
        EQ("ARMv6 encoding on the ARM9: mode", nds_cpu_mode(&c), NDS_MODE_UND);
        EQ("ARMv6 encoding on the ARM9: no bus access", g_accesses, 0);
        /* (The BXJ encoding is an MSR to the ARM7TDMI, so the mode can
         * change; what matters is that there is no trap.) */
        fresh7(&c);
        c.r[1] = DATA;
        arm7(&c, later[i]);
        EQ("ARMv6 encoding on the ARM7: no trap", nds_cpu_mode(&c) != NDS_MODE_UND, 1);
    }

    /* On the ARM7 condition 1111 never executes. */
    fresh7(&c);
    c.r[0] = 5;
    arm7(&c, 0xF3A00001u); /* mov r0, #1 */
    EQ("arm7 cond 1111: skipped", c.r[0], 5);
    EQ("arm7 cond 1111: mode", nds_cpu_mode(&c), NDS_MODE_SYS);

    /* IRQ entry: LR = next instruction + 4, at the vector base. */
    fresh9(&c);
    c.cpsr &= ~NDS_CPSR_T;
    c.r[15] = CODE + 8;
    EQ("irq taken", nds9_cpu_irq(&c), 1);
    EQ("irq: mode", nds_cpu_mode(&c), NDS_MODE_IRQ);
    EQ("irq: r15", c.r[15], 0xFFFF0018u + 8);
    EQ("irq: lr", c.r[14], CODE + 4);
    EQ("irq masked", nds9_cpu_irq(&c), 0);
}

/* ------------------------------------------------------------------ */
/* CP15                                                                */
/* ------------------------------------------------------------------ */

static void test_cp15(void)
{
    NdsCpu c;
    g_section = "cp15";

    /* mrc p15, 0, r0, c0, c0, 0..2: the ID block (GBATEK). */
    fresh9(&c);
    arm9(&c, 0xEE100F10u);
    EQ("main id", c.r[0], 0x41059461u);
    arm9(&c, 0xEE100F30u);
    EQ("cache type", c.r[0], 0x0F0D2112u);
    arm9(&c, 0xEE100F50u);
    EQ("tcm size", c.r[0], 0x00140180u);

    /* The control register: fixed ones in bits 3-6, only some bits
     * writable. mcr p15, 0, r0, c1, c0, 0 then mrc. */
    fresh9(&c);
    g_cp15_changes = 0;
    c.r[0] = 0xFFFFFFFFu;
    arm9(&c, 0xEE010F10u);
    arm9(&c, 0xEE111F10u);
    EQ("control after writing ones", c.r[1], 0x000FF0FDu);
    EQ("bus told", g_cp15_changes, 1);
    c.r[0] = 0;
    arm9(&c, 0xEE010F10u);
    arm9(&c, 0xEE111F10u);
    EQ("control after writing zeros", c.r[1], 0x00000078u);
    EQ("vector base follows bit 13", c.vector_base, 0);

    /* DTCM and ITCM registers keep only their defined bits. */
    fresh9(&c);
    c.r[0] = 0xFFFFFFFFu;
    arm9(&c, 0xEE090F11u); /* mcr p15, 0, r0, c9, c1, 0 */
    arm9(&c, 0xEE191F11u);
    EQ("dtcm", c.r[1], 0xFFFFF03Eu);
    arm9(&c, 0xEE090F31u); /* mcr p15, 0, r0, c9, c1, 1 */
    arm9(&c, 0xEE191F31u);
    EQ("itcm", c.r[1], 0x0000003Eu);

    /* The legacy permission registers are a two-bit view of the four-bit
     * ones. */
    fresh9(&c);
    c.r[0] = 0x0000E4D2u; /* regions 0..7 = 2,0,1,3,0,1,2,3 */
    arm9(&c, 0xEE050F10u); /* mcr p15, 0, r0, c5, c0, 0 */
    arm9(&c, 0xEE151F50u); /* mrc p15, 0, r1, c5, c0, 2 */
    EQ("extended view", c.r[1], 0x32103102u);
    arm9(&c, 0xEE151F10u);
    EQ("legacy view", c.r[1], 0x0000E4D2u);

    /* Region registers. mcr p15, 0, r0, c6, c3, 0 */
    fresh9(&c);
    c.r[0] = 0x0200002Bu;
    arm9(&c, 0xEE060F13u);
    arm9(&c, 0xEE161F13u);
    EQ("region 3", c.r[1], 0x0200002Bu);

    /* Wait for interrupt: mcr p15, 0, r0, c7, c0, 4. */
    fresh9(&c);
    g_waits = 0;
    arm9(&c, 0xEE070F90u);
    EQ("wait for interrupt", g_waits, 1);

    /* Coprocessor access from user mode is undefined; so is any
     * coprocessor other than 15. */
    fresh9(&c);
    nds_cpu_set_cpsr(&c, NDS_MODE_USR);
    arm9(&c, 0xEE100F10u);
    EQ("user mode mrc: mode", nds_cpu_mode(&c), NDS_MODE_UND);
    fresh9(&c);
    arm9(&c, 0xEE100E10u); /* p14 */
    EQ("p14: mode", nds_cpu_mode(&c), NDS_MODE_UND);

    /* mrc with Rd = r15 sets NZCV from the top bits (ARM ARM). The main
     * ID register starts 0x4...: Z set, the others clear. */
    fresh9(&c);
    c.flag_n = 1; c.flag_c = 1; c.flag_v = 1;
    arm9(&c, 0xEE10FF10u);
    EQ("mrc r15: N", c.flag_n, 0);
    EQ("mrc r15: Z", c.flag_z, 1);
    EQ("mrc r15: C", c.flag_c, 0);
    EQ("mrc r15: V", c.flag_v, 0);
    EQ("mrc r15: no branch", c.r[15], CODE + 12);

    /* The ARM7 has no coprocessors. */
    fresh7(&c);
    arm7(&c, 0xEE100F10u);
    EQ("arm7 mrc: mode", nds_cpu_mode(&c), NDS_MODE_UND);
}

/* ------------------------------------------------------------------ */
/* The step function on each processor                                 */
/* ------------------------------------------------------------------ */

static void test_step(void)
{
    NdsCpu c;
    g_section = "step";

    /* Three instructions through the pipeline: mov r0, #1; add r0, r0,
     * #2; b . on the ARM9. */
    fresh9(&c);
    poke32(CODE, 0xE3A00001u);
    poke32(CODE + 4, 0xE2800002u);
    poke32(CODE + 8, 0xEAFFFFFEu);
    c.r[15] = CODE + 8;
    nds9_cpu_refill(&c);
    nds9_cpu_step(&c);
    nds9_cpu_step(&c);
    EQ("arm9 r0", c.r[0], 3);
    nds9_cpu_step(&c);
    EQ("arm9 r15 after b .", c.r[15], CODE + 8 + 8);
    EQ("arm9 pipe refilled", c.pipe[0], 0xEAFFFFFEu);

    fresh7(&c);
    poke32(CODE, 0xE3A00001u);
    poke32(CODE + 4, 0xE2800002u);
    poke32(CODE + 8, 0xEAFFFFFEu);
    c.r[15] = CODE + 8;
    nds7_cpu_refill(&c);
    nds7_cpu_step(&c);
    nds7_cpu_step(&c);
    EQ("arm7 r0", c.r[0], 3);
    nds7_cpu_step(&c);
    EQ("arm7 r15 after b .", c.r[15], CODE + 8 + 8);

    /* Reset state. */
    memset(&c, 0, sizeof c);
    nds9_cpu_reset(&c);
    EQ("arm9 reset r15", c.r[15], 0xFFFF0000u + 8);
    EQ("arm9 reset cpsr", nds_cpu_get_cpsr(&c), NDS_MODE_SVC | NDS_CPSR_I | NDS_CPSR_F);
    EQ("arm9 reset control", c.cp15.control, 0x00002078u);
    nds7_cpu_reset(&c);
    EQ("arm7 reset r15", c.r[15], 8);
}

int main(void)
{
    test_block_transfer();
    test_interworking();
    test_misaligned();
    test_dsp();
    test_double();
    test_exceptions();
    test_cp15();
    test_step();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
