"""Generates runtimes/runtime-nes/src/nes_apu_tables.c.

    python tools/nes/gen_apu_tables.py runtimes/runtime-nes/src/nes_apu_tables.c

Needs numpy (for the FIR prototype only).

All table values come from integer arithmetic (Python big integers), except
the FIR prototype, whose rounded integer taps are the table itself.
"""
import sys
import numpy as np

out_path = sys.argv[1]

SCALE = 65536

def rdiv(n, d):
    return (2 * n + d) // (2 * d)

# pulse_out = 95.88 / (8128 / n + 100) = 9588 n / (812800 + 10000 n)
pulse = [0] + [rdiv(SCALE * 9588 * n, 812800 + 10000 * n) for n in range(1, 31)]

# tnd_out = 159.79 / (1 / (t/8227 + n/12241 + d/22638) + 100)
#         = 15979 N / (100 (D + 100 N)),
#   N = t*12241*22638 + n*8227*22638 + d*8227*12241, D = 8227*12241*22638
A, B, C = 12241 * 22638, 8227 * 22638, 8227 * 12241
D = 8227 * 12241 * 22638
tnd = []
for d in range(128):
    for t in range(16):
        for n in range(16):
            N = t * A + n * B + d * C
            tnd.append(rdiv(SCALE * 15979 * N, 100 * (D + 100 * N)))
assert max(tnd) < 65536 and max(pulse) < 65536

# 64-tap Kaiser low-pass at 4x the output rate, decimating by 4.
TAPS = 64
FC = 0.1172      # cycles per intermediate sample: -6 dB at 0.469 x output rate
BETA = 5.65
k = np.arange(TAPS) - (TAPS - 1) / 2.0
h = 2 * FC * np.sinc(2 * FC * k) * np.kaiser(TAPS, BETA)
h = h / h.sum() * 32768.0
taps = [int(round(v)) for v in h]
res = 32768 - sum(taps)
assert res % 2 == 0
taps[TAPS // 2 - 1] += res // 2
taps[TAPS // 2] += res // 2
assert sum(taps) == 32768 and taps == taps[::-1]

# Report the response of the integer taps.
w = np.array(taps) / 32768.0
def gain(f_rel_out):  # frequency as a fraction of the output rate
    f = f_rel_out / 4.0
    return abs(np.sum(w * np.exp(-2j * np.pi * f * np.arange(TAPS))))
for f in (0.1, 0.2, 0.3, 0.35, 0.4, 0.45, 0.469, 0.5, 0.55, 0.583, 0.6, 0.7, 0.8, 1.0, 1.5, 2.0):
    print("f = %.3f fs: %7.2f dB" % (f, 20 * np.log10(max(gain(f), 1e-9))))
fs = np.linspace(0.6, 2.0, 2000)
print("worst stopband (0.6..2.0 fs): %.2f dB" % max(20 * np.log10(max(gain(f), 1e-9)) for f in fs))
print("sum |taps| =", sum(abs(t) for t in taps))

def rows(values, per, fmt):
    lines = []
    for i in range(0, len(values), per):
        lines.append("    " + ", ".join(fmt % v for v in values[i:i + per]) + ",")
    return "\n".join(lines)

src = []
src.append("""/*
 * NES APU constant tables: the two DAC curves and the output decimator.
 *
 * The DAC tables hold the output of the nesdev wiki's mixer formulas as a
 * fraction of full scale times 65536, rounded to nearest (the two parts
 * add up to at most 65536):
 *
 *   nes_apu_pulse_dac[p1 + p2]             = 95.88 / (8128 / (p1 + p2) + 100)
 *   nes_apu_tnd_dac[d << 8 | t << 4 | n]   = 159.79 / (1 / (t / 8227 + n / 12241 + d / 22638) + 100)
 *
 * with p1, p2, t, n in 0..15 and d in 0..127; a zero input gives zero.
 * They were produced with exact integer arithmetic:
 *
 *   pulse[k] = round(65536 * 9588 k / (812800 + 10000 k))
 *   tnd      = round(65536 * 15979 N / (100 (D + 100 N)))
 *              N = t * 12241 * 22638 + n * 8227 * 22638 + d * 8227 * 12241
 *              D = 8227 * 12241 * 22638
 *
 * tests/nes/test_nes_apu.c checks every entry against the formulas. The big
 * table is indexed directly instead of the wiki's 203-entry approximation
 * (3 t + 2 n + d), which is up to 4.7 % off.
 *
 * nes_apu_fir is a 64-tap Kaiser-windowed (beta 5.65) sinc low-pass that
 * runs at four times the output rate before the final 4:1 decimation:
 * flat to about 0.35 of the output rate, -6 dB at 0.469, below -60 dB
 * from 0.6 up. The taps are symmetric and sum to exactly 32768.
 */
#include "nes_apu.h"
""")
src.append("const uint16_t nes_apu_pulse_dac[31] = {\n" + rows(pulse, 8, "%5d") + "\n};\n")
src.append("const int16_t nes_apu_fir[NES_APU_FIR_TAPS] = {\n" + rows(taps, 8, "%5d") + "\n};\n")
src.append("const uint16_t nes_apu_tnd_dac[32768] = {\n" + rows(tnd, 16, "%d") + "\n};\n")
with open(out_path, "w", newline="\n") as f:
    f.write("\n".join(src))
print("pulse max", max(pulse), "tnd max", max(tnd), "bytes", sum(len(s) for s in src))
