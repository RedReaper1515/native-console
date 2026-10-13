/*
 * The lift checker runs original code on its own small SM83 interpreter
 * (lift/checker/lift_cpu.c). Whatever that interpreter gets wrong, every
 * verdict of the checker gets wrong with it, so this holds it to the
 * runtime's interpreter: each opcode, from many random states, must leave
 * the same registers, flags, memory and cycle count in both.
 */
#include "gbrt.h"
#include "lift_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_ADDRESS 0x0200u
#define TRIALS_PER_OPCODE 200

static uint64_t next_random(uint64_t* state) {
    uint64_t value = (*state += 0x9E3779B97F4A7C15ull);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

static int is_undefined(uint8_t op) {
    switch (op) {
        case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4: case 0xEB:
        case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD:
            return 1;
        default:
            return 0;
    }
}

/* Keep every memory operand inside work RAM or high RAM, where both
 * machines are plain memory. */
static void constrain(uint8_t code[3], GBContext* regs, uint64_t* rng) {
    const uint8_t op = code[0];
    const uint16_t ram = (uint16_t)(0xC800u + next_random(rng) % 0x700u);
    int uses_hl = 0;
    if (op == 0xCB) {
        uses_hl = (code[1] & 7u) == 6u;
    } else if (op >= 0x40 && op <= 0xBF) {
        uses_hl = (op & 7u) == 6u || (op >= 0x70 && op <= 0x77);
    } else {
        switch (op) {
            case 0x34: case 0x35: case 0x36: case 0x22: case 0x32: case 0x2A: case 0x3A:
                uses_hl = 1;
                break;
            default:
                break;
        }
    }
    if (uses_hl) regs->hl = ram;
    if (op == 0x02 || op == 0x0A) regs->bc = ram;
    if (op == 0x12 || op == 0x1A) regs->de = ram;
    if (op == 0xEA || op == 0xFA || op == 0x08) {
        code[1] = (uint8_t)(ram & 0xFFu);
        code[2] = (uint8_t)(ram >> 8);
    }
    if (op == 0xE0 || op == 0xF0) code[1] = (uint8_t)(0x80u + next_random(rng) % 0x7Fu);
    if (op == 0xE2 || op == 0xF2) regs->c = (uint8_t)(0x80u + next_random(rng) % 0x7Fu);
    regs->sp = (uint16_t)(0xD000u + 2u * (next_random(rng) % 0x400u));
}

static int report(const char* what, const uint8_t code[3], unsigned original, unsigned lifted) {
    fprintf(stderr, "opcode %02X %02X %02X: %s: runtime %04X, lift checker %04X\n", code[0],
            code[1], code[2], what, original, lifted);
    return 1;
}

int main(void) {
    GBConfig config;
    memset(&config, 0, sizeof(config));
    config.model = GB_MODEL_DMG;
    GBContext* runtime = gb_context_create(&config);
    if (!runtime) return 2;

    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof(rom));
    if (!gb_context_load_rom(runtime, rom, sizeof(rom))) return 2;
    gb_context_reset(runtime, true);

    static LiftMachine machine;
    lift_machine_init(&machine, rom, sizeof(rom));
    machine.step_limit = 0;
    machine.bus_limit = 0;
    machine.return_pc = 0xFEFE;

    uint64_t rng = 0x5EEDu;
    for (size_t i = 0; i < 0x8000u; ++i) runtime->wram[i] = (uint8_t)next_random(&rng);
    for (size_t i = 0; i < 0x7Fu; ++i) runtime->hram[i] = (uint8_t)next_random(&rng);
    memcpy(machine.wram, runtime->wram, 0x8000u);
    memcpy(machine.hram, runtime->hram, 0x7Fu);

    unsigned checked = 0;
    for (unsigned index = 0; index < 512; ++index) {
        const uint8_t op = index < 256 ? (uint8_t)index : 0xCB;
        if (index < 256 && (op == 0xCB || op == 0x76 || op == 0x10 || is_undefined(op))) continue;

        for (int trial = 0; trial < TRIALS_PER_OPCODE; ++trial) {
            uint8_t code[3];
            code[0] = op;
            code[1] = index < 256 ? (uint8_t)next_random(&rng) : (uint8_t)(index - 256);
            code[2] = (uint8_t)next_random(&rng);

            GBContext mine;
            memset(&mine, 0, sizeof(mine));
            const uint64_t bits = next_random(&rng);
            mine.a = (uint8_t)next_random(&rng);
            mine.bc = (uint16_t)next_random(&rng);
            mine.de = (uint16_t)next_random(&rng);
            mine.hl = (uint16_t)next_random(&rng);
            mine.f_z = (uint8_t)(bits & 1u);
            mine.f_n = (uint8_t)((bits >> 1) & 1u);
            mine.f_h = (uint8_t)((bits >> 2) & 1u);
            mine.f_c = (uint8_t)((bits >> 3) & 1u);
            constrain(code, &mine, &rng);
            mine.pc = CODE_ADDRESS;

            runtime->a = mine.a;
            runtime->bc = mine.bc;
            runtime->de = mine.de;
            runtime->hl = mine.hl;
            runtime->sp = mine.sp;
            runtime->pc = mine.pc;
            runtime->f_z = mine.f_z;
            runtime->f_n = mine.f_n;
            runtime->f_h = mine.f_h;
            runtime->f_c = mine.f_c;
            runtime->ime = 0;
            runtime->ime_pending = 0;
            runtime->halted = 0;
            runtime->io[0x0F] = 0;
            runtime->io[0x80] = 0;
            memcpy(runtime->rom + CODE_ADDRESS, code, 3);
            memcpy(rom + CODE_ADDRESS, code, 3);

            const uint32_t runtime_cycles = gb_debug_step(runtime, GB_EXECUTION_INTERPRETER);
            machine.cycles = 0;
            machine.status = LIFT_OK;
            if (!lift_cpu_step(&machine, &mine)) return report("status", code, 0, machine.status);

            if (runtime->a != mine.a) return report("a", code, runtime->a, mine.a);
            if (runtime->bc != mine.bc) return report("bc", code, runtime->bc, mine.bc);
            if (runtime->de != mine.de) return report("de", code, runtime->de, mine.de);
            if (runtime->hl != mine.hl) return report("hl", code, runtime->hl, mine.hl);
            if (runtime->sp != mine.sp) return report("sp", code, runtime->sp, mine.sp);
            if (runtime->pc != mine.pc) return report("pc", code, runtime->pc, mine.pc);
            if (!runtime->f_z != !mine.f_z) return report("flag z", code, runtime->f_z, mine.f_z);
            if (!runtime->f_n != !mine.f_n) return report("flag n", code, runtime->f_n, mine.f_n);
            if (!runtime->f_h != !mine.f_h) return report("flag h", code, runtime->f_h, mine.f_h);
            if (!runtime->f_c != !mine.f_c) return report("flag c", code, runtime->f_c, mine.f_c);
            const unsigned runtime_ime = runtime->ime || runtime->ime_pending;
            if (runtime_ime != (unsigned)(mine.ime != 0)) {
                return report("ime", code, runtime_ime, mine.ime);
            }
            if (runtime_cycles != (uint32_t)machine.cycles) {
                return report("cycles", code, runtime_cycles, (unsigned)machine.cycles);
            }
            if (memcmp(runtime->wram, machine.wram, 0x8000u) != 0) {
                for (unsigned i = 0; i < 0x8000u; ++i) {
                    if (runtime->wram[i] != ((const uint8_t*)machine.wram)[i]) {
                        fprintf(stderr, "work RAM offset %04X differs\n", i);
                        break;
                    }
                }
                return report("work RAM", code, 0, 1);
            }
            if (memcmp(runtime->hram, machine.hram, 0x7Fu) != 0) {
                return report("high RAM", code, 0, 1);
            }
            ++checked;
        }
    }

    gb_context_destroy(runtime);

    /* Code in the switchable area that maps another bank over itself: what
     * runs next is not the code that was lifted, so the state is one the
     * checker has to refuse. The same write from the home bank is how a
     * game switches banks, and is fine. */
    {
        static uint8_t banked[0x10000];
        memset(banked, 0, sizeof(banked));
        banked[0x147] = 0x01;      /* MBC1, four banks */
        banked[0x0200] = 0x77;     /* home: ld [hl], a */
        banked[0x0201] = 0xC9;     /*       ret */
        banked[0x4000] = 0x77;     /* bank 1: ld [hl], a */
        banked[0x4010] = 0xCD;     /* bank 1: call $0200 */
        banked[0x4011] = 0x00;
        banked[0x4012] = 0x02;
        /* A helper in the home bank that leaves another bank mapped when it
         * returns to banked code does the same thing one step removed; one
         * that leaves the caller's bank mapped is every bank-switching
         * helper there is. */
        static const struct {
            uint16_t pc;
            uint8_t bank_written;
            int steps;
            LiftStatus expected;
        } kCases[] = {
            {0x0200, 2, 1, LIFT_OK},
            {0x4000, 2, 1, LIFT_BANK_PULLED},
            {0x4010, 2, 3, LIFT_FOREIGN_BANK},
            {0x4010, 1, 3, LIFT_OK},
        };
        for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
            static LiftMachine mapped;
            lift_machine_init(&mapped, banked, sizeof(banked));
            GBContext regs;
            memset(&regs, 0, sizeof(regs));
            regs.a = kCases[i].bank_written;
            regs.hl = 0x2000;
            regs.sp = 0xDFF0;
            regs.pc = kCases[i].pc;
            for (int step = 0; step < kCases[i].steps; ++step) lift_cpu_step(&mapped, &regs);
            if (mapped.rom_bank != kCases[i].bank_written || mapped.status != kCases[i].expected) {
                fprintf(stderr, "bank %u written from code at %04X: bank %u mapped, status %s (expected %s)\n",
                        (unsigned)kCases[i].bank_written, (unsigned)kCases[i].pc,
                        (unsigned)mapped.rom_bank, lift_status_name(mapped.status),
                        lift_status_name(kCases[i].expected));
                return 1;
            }
        }
    }

    if (checked < 490u * TRIALS_PER_OPCODE) {
        fprintf(stderr, "only %u instruction executions were compared\n", checked);
        return 1;
    }
    printf("%u instruction executions agree\n", checked);
    return 0;
}
