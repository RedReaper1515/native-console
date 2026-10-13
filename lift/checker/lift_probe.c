/*
 * lift_probe.c - where does a helper that takes its target in registers go?
 *
 * Hand-written assembly reaches much of its code through helpers: one that
 * switches bank and jumps to the address in HL, one that looks a number up
 * in a table of functions. To an analysis that follows calls, the code they
 * reach has no caller. But at most places such a helper is called from, the
 * registers it reads hold constants, and then the ROM itself says where it
 * goes: run the helper from that call with those constants and watch.
 *
 * Each probe is run from several states that agree on the known registers
 * and on nothing else. If control leaves for the same address, with the
 * same bank mapped, every time, that address was decided by the constants.
 *
 *   lift_probe ROM < probes > results
 *
 * A probe is a line of thirteen numbers:
 *   id bank entry jump a b c d e h l trials steps
 * `bank` is the ROM bank mapped where the call is made, `entry` the helper's
 * address, `jump` nonzero when the site jumps to the helper where it could
 * have called it, and a register is -1 when it is not known there.
 *
 * A result is one line: the id, then
 *   to BANK ADDRESS RETURNS_TO AT   control left for BANK:ADDRESS from the
 *                                   instruction at AT, with RETURNS_TO on
 *                                   the stack for the target to return to
 *                                   (0 when that is the helper's own caller)
 *   table AT RETURNS_TO BANK N (SLOT TARGET_BANK TARGET) x N
 *                                   it went to a different place each time,
 *                                   always from the instruction at AT, and
 *                                   each time the address came out of the
 *                                   ROM: the two bytes at SLOT, in bank BANK
 *   returns                         it came back without any such transfer
 *   varies                          it went to different places
 *   STATUS                          no state got that far (a lift_status_name)
 */
#include "lift_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETURN_SENTINEL 0xFEFEu

static uint64_t next_random(uint64_t* state) {
    uint64_t value = (*state += 0x9E3779B97F4A7C15ull);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

static void fill_random(uint8_t* bytes, size_t size, uint64_t* rng) {
    for (size_t i = 0; i < size; ++i) bytes[i] = (uint8_t)next_random(rng);
}

typedef struct Outcome {
    LiftStatus status;
    uint32_t bank;
    uint16_t target;
    uint16_t returns_to;
    uint16_t at;
    int from_rom;       /* the target is two bytes the helper read from the ROM... */
    uint16_t slot;      /* ...at this address... */
    uint16_t slot_bank; /* ...with this bank mapped there */
} Outcome;

#define MAX_TRIALS 64u

static LiftMachine g_machine;

static Outcome run_once(const uint8_t* rom, size_t rom_size, unsigned bank, unsigned entry,
                        const int known[7], unsigned steps, uint64_t seed) {
    LiftMachine* m = &g_machine;
    uint64_t rng = seed;
    lift_machine_init(m, rom, rom_size);
    fill_random(&m->wram[0][0], sizeof(m->wram), &rng);
    fill_random(m->hram, sizeof(m->hram), &rng);
    fill_random(m->io, sizeof(m->io), &rng);
    m->seed = next_random(&rng);
    if (bank != 0) lift_machine_select_rom_bank(m, bank);
    m->step_limit = steps;
    m->probe = 1;

    GBContext regs;
    memset(&regs, 0, sizeof(regs));
    uint8_t* const slots[7] = {&regs.a, &regs.b, &regs.c, &regs.d, &regs.e, &regs.h, &regs.l};
    for (int i = 0; i < 7; ++i) {
        *slots[i] = known[i] >= 0 ? (uint8_t)known[i] : (uint8_t)next_random(&rng);
    }
    const uint64_t flags = next_random(&rng);
    regs.f_z = (uint8_t)(flags & 1u);
    regs.f_n = (uint8_t)((flags >> 1) & 1u);
    regs.f_h = (uint8_t)((flags >> 2) & 1u);
    regs.f_c = (uint8_t)((flags >> 3) & 1u);
    regs.machine = m;

    /* As a CALL leaves it: the return address on the stack, and a record of
     * where, so that the helper's own RET is not taken for a jump. */
    const uint16_t sp = (uint16_t)(0xDFF0u - 2u * (next_random(&rng) % 16u));
    lift_machine_poke(m, sp, (uint8_t)(RETURN_SENTINEL & 0xFFu));
    lift_machine_poke(m, (uint16_t)(sp + 1u), (uint8_t)(RETURN_SENTINEL >> 8));
    regs.sp = sp;
    regs.pc = (uint16_t)entry;
    m->probe_call_sp[0] = sp;
    m->probe_calls = 1;
    m->return_pc = RETURN_SENTINEL;
    m->return_sp = (uint16_t)(sp + 2u);

    Outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    outcome.status = lift_cpu_run_until(m, &regs, RETURN_SENTINEL, m->return_sp);
    if (outcome.status == LIFT_TRANSFER) {
        outcome.target = m->probe_target;
        outcome.bank = m->probe_target >= 0x4000 ? m->probe_bank : 0;
        outcome.at = m->probe_at;
        const uint16_t top = (uint16_t)(lift_machine_peek(m, regs.sp) |
                                        ((uint16_t)lift_machine_peek(m, (uint16_t)(regs.sp + 1u)) << 8));
        /* The helper's own return address still on top: the target returns
         * to whoever called the helper. */
        outcome.returns_to = (regs.sp == sp && top == RETURN_SENTINEL) ? 0 : top;
        /* Did the address come out of the ROM? The most recent two bytes
         * read from it, next to each other, that spell the target. */
        for (unsigned high = m->probe_reads; high-- > 0u && !outcome.from_rom;) {
            for (unsigned low = 0; low < m->probe_reads; ++low) {
                if (m->probe_read_address[high] == (uint16_t)(m->probe_read_address[low] + 1u) &&
                    m->probe_read_bank[high] == m->probe_read_bank[low] &&
                    (uint16_t)(m->probe_read_value[low] | ((uint16_t)m->probe_read_value[high] << 8)) ==
                        outcome.target) {
                    outcome.from_rom = 1;
                    outcome.slot = m->probe_read_address[low];
                    outcome.slot_bank = m->probe_read_bank[low];
                    break;
                }
            }
        }
    }
    return outcome;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: lift_probe ROM < probes > results\n");
        return 2;
    }
    FILE* file = fopen(argv[1], "rb");
    if (!file) {
        fprintf(stderr, "lift_probe: cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t* rom = length > 0 ? (uint8_t*)malloc((size_t)length) : NULL;
    if (!rom || fread(rom, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "lift_probe: cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(file);

    char line[256];
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    while (fgets(line, sizeof(line), stdin)) {
        long id;
        unsigned bank, entry, jump, trials, steps;
        int known[7];
        if (sscanf(line, "%ld %u %u %u %d %d %d %d %d %d %d %u %u", &id, &bank, &entry, &jump,
                   &known[0], &known[1], &known[2], &known[3], &known[4], &known[5], &known[6],
                   &trials, &steps) != 13) {
            continue;
        }
        (void)jump; /* a jump to the helper leaves the same stack as a call */
        static Outcome seen[MAX_TRIALS];
        if (trials > MAX_TRIALS) trials = MAX_TRIALS;
        Outcome first;
        memset(&first, 0, sizeof(first));
        int agree = 1;
        int table = 1; /* one instruction, one way back, every address out of one bank of the ROM */
        unsigned reached = 0;
        unsigned returned = 0;
        LiftStatus failure = LIFT_OK;
        for (unsigned trial = 0; trial < trials; ++trial) {
            const uint64_t seed = 0x5EED0000u + ((uint64_t)id << 20) + trial;
            const Outcome outcome = run_once(rom, (size_t)length, bank, entry, known, steps, seed);
            if (outcome.status == LIFT_OK) {
                returned++;
                continue;
            }
            if (outcome.status != LIFT_TRANSFER) {
                failure = outcome.status;
                continue;
            }
            if (reached == 0) first = outcome;
            if (outcome.target != first.target || outcome.bank != first.bank ||
                outcome.returns_to != first.returns_to || outcome.at != first.at) {
                agree = 0;
            }
            /* The same address out of two different places in the ROM is two
             * entries of a table that happen to match (past the end of a
             * short one, everything may read as zero), not one place the
             * constants chose. */
            if (outcome.from_rom && first.from_rom && outcome.slot != first.slot) agree = 0;
            if (!outcome.from_rom || outcome.at != first.at || outcome.returns_to != first.returns_to ||
                outcome.slot_bank != first.slot_bank) {
                table = 0;
            }
            seen[reached++] = outcome;
        }
        if (reached == trials && agree) {
            printf("%ld to %u %u %u %u\n", id, (unsigned)first.bank, (unsigned)first.target,
                   (unsigned)first.returns_to, (unsigned)first.at);
        } else if (reached == trials && table) {
            printf("%ld table %u %u %u %u", id, (unsigned)first.at, (unsigned)first.returns_to,
                   (unsigned)first.slot_bank, reached);
            for (unsigned i = 0; i < reached; ++i) {
                printf(" %u %u %u", (unsigned)seen[i].slot, (unsigned)seen[i].bank, (unsigned)seen[i].target);
            }
            printf("\n");
        } else if (reached != 0) {
            /* Some states went elsewhere, or not at all: not the constants' doing. */
            printf("%ld varies\n", id);
        } else if (returned != 0) {
            printf("%ld returns\n", id);
        } else {
            printf("%ld %s\n", id, lift_status_name(failure));
        }
    }
    free(rom);
    return 0;
}
