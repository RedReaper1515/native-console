/*
 * The 2C02's colours as RGB, computed from its composite video signal.
 *
 * Reference: the nesdev wiki, "NTSC video". For every pixel the PPU
 * outputs a square wave at the colour subcarrier frequency, in one of 12
 * phases (the hue, colour & 15) between two of the voltages below (chosen
 * by the brightness, colour >> 4). Hue 0 stays at the high voltage, hue
 * $D at the low one, and $E/$F are black. Each emphasis bit attenuates
 * the signal during half of the subcarrier cycle.
 *
 * The table here is what an ideal decoder shows for a screen filled with
 * one colour: one subcarrier cycle is sampled at the PPU's 12 steps, the
 * mean is Y and the products with sine and cosine are U and V (burst is
 * phase 8, pure -U), converted with the standard YUV matrix. Black is the
 * level of colour $1D and white that of colour $20, as on a set without
 * the 7.5 IRE setup. Chroma gets the demodulation gain of 2 and nothing
 * else: no burst-amplitude correction, no hue or saturation adjustment,
 * no gamma step, and no model of the level-dependent phase shift.
 *
 * Everything is integer arithmetic on exact inputs (millivolts, a sine
 * table scaled by 65536, matrix coefficients scaled by 10^6), so the
 * table is the same on every compiler and platform.
 */
#include "nes_ppu.h"

/* Millivolts into 75 ohms: low and high level for brightness 0-3, then
 * the same with the emphasis attenuator on. */
static const int32_t level_mv[16] = {
    228, 312, 552, 880, /* low */
    616, 840, 1100, 1100, /* high */
    192, 256, 448, 712, /* low, attenuated */
    500, 676, 896, 896, /* high, attenuated */
};

enum {
    BLACK_MV = 312,
    WHITE_MV = 1100,
    SAMPLES = 12,
    SINE_ONE = 65536
};

/* sin(15 + 30 k degrees) * 65536. */
static const int32_t sine[SAMPLES] = {
    16962, 46341, 63303, 63303, 46341, 16962, -16962, -46341, -63303, -63303, -46341, -16962,
};

/* The wave of hue `hue` is high for six of the twelve steps. */
static int in_phase(unsigned hue, unsigned step) { return (hue + step) % 12u < 6u; }

/* The voltage for colour | emphasis << 6 at subcarrier step 0-11. */
static int32_t signal_mv(unsigned pixel, unsigned step)
{
    const unsigned hue = pixel & 0x0Fu;
    const unsigned level = hue > 13u ? 1u : (pixel >> 4) & 3u;
    const unsigned emphasis = pixel >> 6;

    const int attenuated = hue < 14u && (((emphasis & 1u) && in_phase(0xC, step)) ||
                                         ((emphasis & 2u) && in_phase(0x4, step)) ||
                                         ((emphasis & 4u) && in_phase(0x8, step)));
    const unsigned row = attenuated ? 8u : 0u;

    int32_t low = level_mv[row + level];
    int32_t high = level_mv[row + 4u + level];
    if (hue == 0u) low = high;
    if (hue > 12u) high = low;
    return in_phase(hue, step) ? high : low;
}

/* 255 * numerator / denominator, rounded to nearest and clamped. */
static uint32_t channel(int64_t numerator, int64_t denominator)
{
    if (numerator <= 0) return 0;
    const int64_t c = (numerator * 510 + denominator) / (denominator * 2);
    return c > 255 ? 255u : (uint32_t)c;
}

void nes_ppu_palette(uint32_t rgb[512])
{
    /* Y = y_sum / (12 * 788); U = 2 * u_sum / (12 * 788 * 65536). */
    const int64_t unit = (int64_t)SAMPLES * (WHITE_MV - BLACK_MV) * SINE_ONE;
    const int64_t denominator = unit * 1000000;

    for (unsigned pixel = 0; pixel < 512u; pixel++) {
        int64_t y_sum = 0, u_sum = 0, v_sum = 0;
        for (unsigned step = 0; step < SAMPLES; step++) {
            const int64_t s = signal_mv(pixel, step) - BLACK_MV;
            /* The sample is centred at (step + 2.5) * 30 degrees. */
            y_sum += s;
            u_sum += s * sine[(step + 2u) % SAMPLES];
            v_sum += s * sine[(step + 5u) % SAMPLES]; /* cosine */
        }
        const int64_t y = y_sum * SINE_ONE * 1000000;
        const int64_t u = u_sum * 2;
        const int64_t v = v_sum * 2;

        const uint32_t r = channel(y + 1139883 * v, denominator);
        const uint32_t g = channel(y - 394642 * u - 580622 * v, denominator);
        const uint32_t b = channel(y + 2032062 * u, denominator);
        rgb[pixel] = (r << 16) | (g << 8) | b;
    }
}
