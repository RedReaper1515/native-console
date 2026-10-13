/*
 * MMPX 2x pixel-art magnification, used by the Smooth display filter.
 *
 * Ported from the reference implementation by Morgan McGuire and Mara
 * Gagiu, https://casual-effects.com/research/McGuire2021PixelArt/ (via the
 * C99 version at https://github.com/ITotalJustice/mmpx):
 *
 *   Copyright 2020 Morgan McGuire & Mara Gagiu.
 *   Available under the MIT license.
 *
 * The rules are the reference's. This version reads through a copy of the
 * image padded by three pixels of repeated border, instead of clamping
 * every read, and compares luma on RGB alone (every GBA pixel is opaque).
 */
#include "gba_upscale.h"

#include <stdbool.h>
#include <stdlib.h>

enum { PAD = 3 };

static uint32_t luma(uint32_t c)
{
    return ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF) + 1;
}

static bool all_eq2(uint32_t b, uint32_t a0, uint32_t a1)
{
    return ((b ^ a0) | (b ^ a1)) == 0;
}

static bool all_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2)
{
    return ((b ^ a0) | (b ^ a1) | (b ^ a2)) == 0;
}

static bool all_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    return ((b ^ a0) | (b ^ a1) | (b ^ a2) | (b ^ a3)) == 0;
}

static bool any_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2)
{
    return b == a0 || b == a1 || b == a2;
}

static bool none_eq2(uint32_t b, uint32_t a0, uint32_t a1)
{
    return b != a0 && b != a1;
}

static bool none_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    return b != a0 && b != a1 && b != a2 && b != a3;
}

int gba_mmpx_scale2x(const uint32_t *src, int w, int h, uint32_t *dst)
{
    const int pw = w + 2 * PAD, ph = h + 2 * PAD;
    uint32_t *pad = (uint32_t *)malloc((size_t)pw * (size_t)ph * sizeof *pad);
    if (!pad) return 0;
    for (int y = 0; y < ph; y++) {
        const int sy = y < PAD ? 0 : y >= h + PAD ? h - 1 : y - PAD;
        for (int x = 0; x < pw; x++) {
            const int sx = x < PAD ? 0 : x >= w + PAD ? w - 1 : x - PAD;
            pad[y * pw + x] = src[sy * w + sx];
        }
    }

    for (int y = 0; y < h; y++) {
        const uint32_t *row = pad + (y + PAD) * pw + PAD; /* row[x] is source (x, y) */
        uint32_t *out0 = dst + 2 * y * 2 * w, *out1 = out0 + 2 * w;
        for (int x = 0; x < w; x++) {
            /* S(dx, dy) is the source pixel at (x + dx, y + dy). */
#define S(dx, dy) row[x + (dx) + (dy) * pw]
            const uint32_t A = S(-1, -1), B = S(0, -1), C = S(1, -1);
            const uint32_t D = S(-1, 0), E = S(0, 0), F = S(1, 0);
            const uint32_t G = S(-1, 1), H = S(0, 1), I = S(1, 1);
            uint32_t J = E, K = E, L = E, M = E; /* top left, top right, bottom left, bottom right */

            if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0) {
                const uint32_t P = S(0, -2), Q = S(-2, 0), R = S(2, 0), Sd = S(0, 2);
                const uint32_t Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

                /* 1:1 slopes */
                if ((D == B && D != H && D != F) && (El >= Dl || E == A) && any_eq3(E, A, C, G) &&
                    (El < Dl || A != D || E != P || E != Q))
                    J = D;
                if ((B == F && B != D && B != H) && (El >= Bl || E == C) && any_eq3(E, A, C, I) &&
                    (El < Bl || C != B || E != P || E != R))
                    K = B;
                if ((H == D && H != F && H != B) && (El >= Hl || E == G) && any_eq3(E, A, G, I) &&
                    (El < Hl || G != H || E != Sd || E != Q))
                    L = H;
                if ((F == H && F != B && F != D) && (El >= Fl || E == I) && any_eq3(E, C, G, I) &&
                    (El < Fl || I != H || E != R || E != Sd))
                    M = F;

                /* Intersections */
                if ((E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && F != S(3, 0)) K = M = F;
                if ((E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && D != S(-3, 0)) J = L = D;
                if ((E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && H != S(0, 3)) L = M = H;
                if ((E != B && all_eq4(E, A, C, H, Sd) && all_eq2(B, D, F)) && B != S(0, -3)) J = K = B;
                if (Bl < El && all_eq4(E, G, H, I, Sd) && none_eq4(E, A, D, C, F)) J = K = B;
                if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) L = M = H;
                if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) K = M = F;
                if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) J = L = D;

                /* 2:1 slopes */
                if (H != B) {
                    if (H != A && H != E && H != C) {
                        if (all_eq3(H, G, F, R) && none_eq2(H, D, S(2, -1))) L = M;
                        if (all_eq3(H, I, D, Q) && none_eq2(H, F, S(-2, -1))) M = L;
                    }
                    if (B != I && B != G && B != E) {
                        if (all_eq3(B, A, F, R) && none_eq2(B, D, S(2, 1))) J = K;
                        if (all_eq3(B, C, D, Q) && none_eq2(B, F, S(-2, 1))) K = J;
                    }
                }
                if (F != D) {
                    if (D != I && D != E && D != C) {
                        if (all_eq3(D, A, H, Sd) && none_eq2(D, B, S(1, 2))) J = L;
                        if (all_eq3(D, G, B, P) && none_eq2(D, H, S(1, -2))) L = J;
                    }
                    if (F != E && F != A && F != G) {
                        if (all_eq3(F, C, H, Sd) && none_eq2(F, B, S(-1, 2))) K = M;
                        if (all_eq3(F, I, B, P) && none_eq2(F, H, S(-1, -2))) M = K;
                    }
                }
            }
#undef S
            out0[2 * x] = J;
            out0[2 * x + 1] = K;
            out1[2 * x] = L;
            out1[2 * x + 1] = M;
        }
    }
    free(pad);
    return 1;
}
