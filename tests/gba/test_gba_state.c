/*
 * Save states (gba_state.c):
 *
 * - Loading a state and running on gives the same machine as running on
 *   without the detour: RAM, VRAM, I/O, CPU and the frame all match.
 * - A state loads through a file into a second machine, which keeps its own
 *   ROM, CPU owner pointer and host clock setting.
 * - A state for another ROM, or of the wrong size, is refused and leaves the
 *   machine untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

/* Mode 3, then count up in r0 forever, storing it to EWRAM and VRAM. */
static const uint32_t k_program[] = {
    0xE3A01402, /* mov r1, #0x02000000 */
    0xE3A02406, /* mov r2, #0x06000000 */
    0xE3A03301, /* mov r3, #0x04000000 */
    0xE3A04C04, /* mov r4, #0x400 */
    0xE2844003, /* add r4, r4, #3 */
    0xE1C340B0, /* strh r4, [r3] (DISPCNT: mode 3, BG2) */
    0xE3A00000, /* mov r0, #0 */
    0xE2800001, /* loop: add r0, r0, #1 */
    0xE5810000, /* str r0, [r1] */
    0xE5820000, /* str r0, [r2] */
    0xEAFFFFFB, /* b loop */
};

static uint8_t rom[0x1000];
static int16_t samples[GBA_APU_RING * 2];

static GbaMachine *make_machine(const uint8_t *image)
{
    GbaMachine *m = gba_create();
    if (!m || !gba_load_rom(m, image, sizeof rom)) {
        printf("cannot create machine\n");
        exit(2);
    }
    gba_reset(m, true);
    return m;
}

/* Frames as a frontend runs them: the audio output is drained each frame. */
static void run_frames(GbaMachine *m, int frames)
{
    for (int i = 0; i < frames; i++) {
        gba_run_frame(m);
        gba_apu_read_samples(m, samples, GBA_APU_RING);
    }
}

static bool same_machine(const GbaMachine *a, const GbaMachine *b)
{
    return memcmp(a->cpu.r, b->cpu.r, sizeof a->cpu.r) == 0 && a->cpu.cpsr == b->cpu.cpsr &&
           a->cpu.cycles == b->cpu.cycles && a->frame_count == b->frame_count &&
           memcmp(a->ewram, b->ewram, sizeof a->ewram) == 0 && memcmp(a->vram, b->vram, sizeof a->vram) == 0 &&
           memcmp(a->io, b->io, sizeof a->io) == 0 &&
           memcmp(a->framebuffer, b->framebuffer, sizeof a->framebuffer) == 0;
}

int main(void)
{
    memset(rom, 0, sizeof rom);
    memcpy(rom, k_program, sizeof k_program);
    memcpy(rom + 0xA0, "STATETEST", 9);
    rom[0xB2] = 0x96;

    GbaMachine *m = make_machine(rom);
    GbaMachine *straight = make_machine(rom);
    run_frames(m, 10);
    run_frames(straight, 10);

    size_t size = gba_state_size();
    uint8_t *state = malloc(size);
    if (!state) return 2;
    gba_state_save(m, state);
    uint32_t counter = m->ewram[0] | (uint32_t)m->ewram[1] << 8;
    CHECK(counter != 0, "the test program did not run");

    /* Run ahead, come back, and run the same frames again. */
    run_frames(m, 7);
    CHECK(gba_state_load(m, state, size), "state did not load");
    CHECK(m->frame_count == 10, "frame_count %llu after load, expected 10", (unsigned long long)m->frame_count);
    CHECK(m->cpu.user == m, "CPU owner pointer not restored");
    run_frames(m, 5);
    run_frames(straight, 5);
    CHECK(same_machine(m, straight), "machine differs from an uninterrupted run after load");

    /* Through a file, into another machine. */
    const char *path = "test_gba_state.state1";
    CHECK(gba_state_save_file(m, path), "cannot write %s", path);
    GbaMachine *other = make_machine(rom);
    other->rtc.wall_clock = true;
    const uint8_t *other_rom = other->rom;
    CHECK(gba_state_load_file(other, path), "cannot load %s", path);
    CHECK(other->rom == other_rom, "load replaced the machine's ROM pointer");
    CHECK(other->cpu.user == other, "load left the CPU pointing at another machine");
    CHECK(other->rtc.wall_clock, "load changed the host clock setting");
    run_frames(other, 3);
    run_frames(straight, 3);
    CHECK(same_machine(other, straight), "machine loaded from a file differs from an uninterrupted run");
    remove(path);

    /* A different ROM, and a short buffer, are refused. */
    uint8_t rom2[sizeof rom];
    memcpy(rom2, rom, sizeof rom);
    memcpy(rom2 + 0xA0, "OTHERGAME", 9);
    GbaMachine *wrong = make_machine(rom2);
    run_frames(wrong, 2);
    CHECK(!gba_state_load(wrong, state, size), "state for another ROM was accepted");
    CHECK(wrong->frame_count == 2, "refused load changed the machine");
    CHECK(!gba_state_load(m, state, size - 1), "truncated state was accepted");
    CHECK(!gba_state_load_file(m, "missing.state"), "missing file was accepted");

    free(state);
    gba_destroy(wrong);
    gba_destroy(other);
    gba_destroy(straight);
    gba_destroy(m);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("state: all checks passed\n");
    return 0;
}
