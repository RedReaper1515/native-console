/*
 * MMPX 2x (gba_upscale.c), used by the Smooth display filter: flat areas
 * stay flat, output uses only source colours, tiny images read their
 * repeated border safely, a lone diagonal is redrawn, and a pseudo-random
 * image of colour runs gives the output the reference implementation
 * (McGuire and Gagiu's, as ported to C99 by ITotalJustice/mmpx, with its
 * border clamp fixed) gives for it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba_upscale.h"

/* Output hashes of the reference implementation for the images below. */
#define REFERENCE_HASH 0x7f7b6ee654049fc3ull
#define REFERENCE_COLUMN_HASH 0x2080f96e08c45a73ull

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

enum { W = 64, H = 48 };
static uint32_t src[W * H], dst[W * H * 4];

static uint32_t rng_state;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* Runs of a few colours, so the edge rules fire. */
static void make_runs(uint32_t seed)
{
    static const uint32_t palette[] = { 0xFF102030u, 0xFFF0E0D0u, 0xFF40A040u, 0xFFA04040u };
    rng_state = seed;
    for (int i = 0; i < W * H; i++) {
        if (i && rng() % 4) src[i] = src[i - (rng() % 2 || i < W ? 1 : W)];
        else src[i] = palette[rng() % 4];
    }
}

static uint64_t fnv1a(const uint32_t *px, size_t count)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < count; i++) {
        for (int b = 0; b < 32; b += 8) {
            h ^= (px[i] >> b) & 0xFF;
            h *= 1099511628211ull;
        }
    }
    return h;
}

int main(void)
{
    /* Flat */
    for (int i = 0; i < W * H; i++) src[i] = 0xFF336699u;
    CHECK(gba_mmpx_scale2x(src, W, H, dst), "flat: allocation");
    int flat = 1;
    for (int i = 0; i < W * H * 4; i++) flat &= dst[i] == 0xFF336699u;
    CHECK(flat, "flat image changed");

    /* 1x1 and 1-wide images read only their repeated border. */
    uint32_t one = 0xFFABCDEFu, one_out[4] = { 0 };
    CHECK(gba_mmpx_scale2x(&one, 1, 1, one_out), "1x1: allocation");
    CHECK(one_out[0] == one && one_out[1] == one && one_out[2] == one && one_out[3] == one, "1x1 changed");
    uint32_t column[3] = { 0xFF000000u, 0xFFFFFFFFu, 0xFF000000u }, column_out[12];
    CHECK(gba_mmpx_scale2x(column, 1, 3, column_out), "1x3: allocation");
    CHECK(fnv1a(column_out, 12) == REFERENCE_COLUMN_HASH, "1x3: output hash %016llx",
          (unsigned long long)fnv1a(column_out, 12));

    /* A dark diagonal line on white: the bright pixels beside each step
     * take the line's colour on the side that faces it, as MMPX's 1:1 rule
     * does, so the output is no longer plain 2x2 blocks. */
    enum { N = 8 };
    uint32_t diag[N * N], diag_out[N * N * 4];
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) diag[y * N + x] = x == y ? 0xFF000000u : 0xFFFFFFFFu;
    }
    CHECK(gba_mmpx_scale2x(diag, N, N, diag_out), "diagonal: allocation");
    int blocky = 1, dark = 0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const uint32_t *o0 = diag_out + y * 2 * 2 * N + 2 * x, *o1 = o0 + 2 * N;
            blocky &= o0[0] == diag[y * N + x] && o0[1] == diag[y * N + x] && o1[0] == diag[y * N + x] &&
                      o1[1] == diag[y * N + x];
        }
    }
    for (int i = 0; i < N * N * 4; i++) dark += diag_out[i] == 0xFF000000u;
    CHECK(!blocky, "diagonal came out as plain 2x2 blocks");
    CHECK(dark > N * 4, "diagonal did not thicken into a smooth line (%d dark pixels)", dark);

    /* Colour runs: only source colours, and the reference output. */
    make_runs(12345);
    CHECK(gba_mmpx_scale2x(src, W, H, dst), "runs: allocation");
    int foreign = 0;
    for (int i = 0; i < W * H * 4; i++) {
        foreign += dst[i] != 0xFF102030u && dst[i] != 0xFFF0E0D0u && dst[i] != 0xFF40A040u && dst[i] != 0xFFA04040u;
    }
    CHECK(foreign == 0, "%d output pixels are not source colours", foreign);
    const uint64_t hash = fnv1a(dst, (size_t)W * H * 4);
    CHECK(hash == REFERENCE_HASH, "runs: output hash %016llx, reference %016llx", (unsigned long long)hash,
          (unsigned long long)REFERENCE_HASH);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gba_mmpx_upscale: all passed\n");
    return 0;
}
