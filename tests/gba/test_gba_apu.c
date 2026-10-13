/*
 * Audio mixer output stage, driven through the sound registers:
 *
 * - PSG channels that are enabled but silent (volume 0, how the MP2K sound
 *   driver leaves them) add nothing: enabling them must not step the
 *   output. They once added a -15 offset each, which shifted the mix far
 *   below the bias, clipped DirectSound peaks and clicked on every change.
 * - A DirectSound sample reaches the output at its level (not clipped) with
 *   those channels enabled, and the output is AC-coupled: a held level
 *   decays back to zero.
 * - A FIFO routed to a speaker counts pops from an empty FIFO.
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

static int16_t samples[GBA_APU_RING * 2];

/* Run `cycles` and return the left channel of the samples produced. */
static size_t run(GbaMachine *m, uint32_t cycles)
{
    gba_apu_read_samples(m, samples, GBA_APU_RING); /* discard older output */
    gba_run_cycles(m, cycles);
    return gba_apu_read_samples(m, samples, GBA_APU_RING);
}

static int max_abs(size_t n)
{
    int worst = 0;
    for (size_t i = 0; i < n; i++) {
        int v = abs(samples[i * 2]);
        if (v > worst) worst = v;
    }
    return worst;
}

int main(void)
{
    static uint8_t rom[0x1000];
    memset(rom, 0, sizeof rom);
    rom[0xB2] = 0x96;
    rom[0] = 0xFE; rom[1] = 0xFF; rom[2] = 0xFF; rom[3] = 0xEA; /* b . */

    GbaMachine *m = gba_create();
    if (!gba_load_rom(m, rom, sizeof rom)) return 2;
    gba_reset(m, true);

    gba_io_write16(m, 0x84, 0x0080, 0xFFFF); /* master enable */
    gba_io_write16(m, 0x88, 0x0200, 0xFFFF); /* SOUNDBIAS */
    gba_io_write16(m, 0x80, 0xFF77, 0xFFFF); /* all PSG channels, both sides, full volume */
    gba_io_write16(m, 0x82, 0x0002, 0xFFFF); /* PSG at 100 %, DirectSound off */
    size_t n = run(m, 1u << 18);
    CHECK(max_abs(n) == 0, "idle output is not silent (peak %d)", max_abs(n));

    /* Enable channels 1, 2 and 4 with volume 0 (DAC on, envelope up with
     * period 0 so it stays at 0) and channel 3 with output volume 0. */
    gba_io_write16(m, 0x62, 0x0800, 0xFFFF);
    gba_io_write16(m, 0x64, 0x8400, 0xFFFF);
    gba_io_write16(m, 0x68, 0x0800, 0xFFFF);
    gba_io_write16(m, 0x6C, 0x8400, 0xFFFF);
    gba_io_write16(m, 0x70, 0x0080, 0xFFFF);
    gba_io_write16(m, 0x72, 0x0000, 0xFFFF);
    gba_io_write16(m, 0x74, 0x8400, 0xFFFF);
    gba_io_write16(m, 0x78, 0x0800, 0xFFFF);
    gba_io_write16(m, 0x7C, 0x8000, 0xFFFF);
    CHECK((gba_apu_read16(m, 0x84) & 0x0F) == 0x0F, "channels not enabled (SOUNDCNT_X %04X)", gba_apu_read16(m, 0x84));
    n = run(m, 1u << 18);
    CHECK(max_abs(n) == 0, "enabling silent PSG channels moved the output (peak %d)", max_abs(n));

    /* DirectSound A at 100 %, both sides, timer 0: one sample of -100
     * should come out at -100 * 4 * 64 = -25600, unclipped. */
    gba_io_write16(m, 0x82, 0x0B06, 0xFFFF); /* PSG 100 %, A 100 %, A left+right, reset A */
    gba_io_write16(m, 0xA0, 0x9C9C, 0xFFFF); /* -100, -100 */
    gba_apu_timer_overflow(m, 0);
    n = run(m, 512 * 4);
    CHECK(n >= 2 && samples[0] > -25700 && samples[0] < -25500, "DirectSound sample came out at %d, expected ~-25600",
          n ? samples[0] : 0);

    /* Held, the level decays (AC coupling, ~10 Hz): well under 5 % after
     * half a second. */
    gba_apu_timer_overflow(m, 0); /* second -100 byte; the FIFO is then empty */
    n = run(m, 1u << 23);
    CHECK(n > 0 && abs(samples[(n - 1) * 2]) < 1280, "held level did not decay (last sample %d)",
          n ? samples[(n - 1) * 2] : 0);

    gba_apu_timer_overflow(m, 0); /* pop from the empty FIFO */
    CHECK(m->apu.fifo[0].underflows == 1, "underflows %u, expected 1", m->apu.fifo[0].underflows);
    CHECK(m->apu.fifo[1].underflows == 0, "FIFO B is not routed but counted %u underflows", m->apu.fifo[1].underflows);

    gba_destroy(m);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("apu: all checks passed\n");
    return 0;
}
