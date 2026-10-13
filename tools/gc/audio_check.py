"""Measure a sound capture and hold it to what clean sound looks like.

    python tools/gc/audio_check.py CAPTURE.raw --section music:2:15 --section silence:40:44 ...

CAPTURE is raw 16-bit little-endian stereo, as `--dump-audio` writes it (32 kHz
unless --rate says otherwise). Each --section is KIND:FROM:TO in seconds:

  music    something is playing: the level is in a sane range, the spectrum is
           tonal rather than flat, little energy sits above 10 kHz, and nothing
           drops out to digital silence part-way through
  silence  nothing is playing: every sample is zero
  any      no expectation about content; only the whole-capture checks apply

and over the whole capture: no runs of samples pinned at the rails, and few
clicks (sample-to-sample jumps far outside their neighbourhood).

Why these, and why these numbers: docs/GC-PLAN.md, "Clean sound". The short
version is that each check is one way this has gone wrong. Sound decoded from
the wrong memory is full-scale noise - flat spectrum, a third of its energy
above 10 kHz, pinned at the rails. A decoder that re-reads the end of a sample
buzzes and clicks. A frontend that runs dry drops to zero in the middle of
music. The thresholds sit an order of magnitude or more away from what Shrek 2
measures when it is right, and an order of magnitude or more away from what it
measured when each of those was wrong.

Exit status 0 when every check passes, 1 when one fails, 2 on bad input.
Needs numpy.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:  # the caller reports this as a skip, not a pass
    np = None

WINDOW = 2048                 # 64 ms at 32 kHz: long enough to resolve tones
LOUD_DB = -50.0               # windows quieter than this say nothing about tone
MUSIC_RMS_DB = (-45.0, -12.0) # a section's level, dBFS
MUSIC_FLATNESS_MAX = 0.05     # median power flatness of its loud windows
MUSIC_HIGH_BAND_MAX = 0.005   # median share of power above 10 kHz
MUSIC_DROPOUTS_MAX = 2        # runs of >= 64 zero frames inside it
CLIP_RUNS_MAX = 5             # runs of >= 3 samples at the rails, whole capture
CLICKS_PER_SECOND_MAX = 4.0   # second-difference outliers, whole capture


def load(path: Path) -> "np.ndarray":
    data = np.fromfile(path, dtype="<i2")
    return data[: data.size // 2 * 2].reshape(-1, 2).astype(np.int32)


def runs(mask: "np.ndarray", minimum: int) -> int:
    if not mask.any():
        return 0
    edges = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    return int(np.count_nonzero(np.flatnonzero(edges == -1) - np.flatnonzero(edges == 1) >= minimum))


def clicks(channel: "np.ndarray") -> int:
    """Second-difference outliers: |x[n] - 2x[n-1] + x[n-2]| above 3000 and more
    than eight times the RMS of its 256-sample neighbourhood (the median of nine
    blocks around it, so a click cannot raise its own threshold). Hits within 32
    samples of each other are one click."""
    d2 = np.abs(np.diff(channel.astype(np.float64), 2))
    block = 256
    padded = np.concatenate((d2, np.zeros((-d2.size) % block)))
    rms = np.sqrt(np.mean(padded.reshape(-1, block) ** 2, axis=1))
    if rms.size >= 9:
        stacked = np.stack([np.roll(rms, k) for k in range(-4, 5)])
        local = np.median(stacked, axis=0)
    else:
        local = rms
    local = np.repeat(local, block)[: d2.size]
    hits = np.flatnonzero((d2 > 3000) & (d2 > 8 * np.maximum(local, 50)))
    if hits.size == 0:
        return 0
    return int(1 + np.count_nonzero(np.diff(hits) > 32))


def spectrum_stats(mono: "np.ndarray", rate: int) -> tuple[float, float, int]:
    """Median power flatness and median share above 10 kHz over the loud windows,
    and how many windows were loud."""
    window = np.hanning(WINDOW)
    freqs = np.fft.rfftfreq(WINDOW, 1.0 / rate)
    flat, high = [], []
    for start in range(0, mono.size - WINDOW + 1, WINDOW // 2):
        chunk = mono[start:start + WINDOW]
        if 20 * np.log10(np.sqrt(np.mean(chunk ** 2)) + 1e-12) < LOUD_DB:
            continue
        power = np.abs(np.fft.rfft(chunk * window)) ** 2 + 1e-20
        flat.append(float(np.exp(np.mean(np.log(power))) / np.mean(power)))
        high.append(float(power[freqs > 10000].sum() / power.sum()))
    if not flat:
        return 0.0, 0.0, 0
    return float(np.median(flat)), float(np.median(high)), len(flat)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=Path)
    parser.add_argument("--rate", type=int, default=32000)
    parser.add_argument("--section", action="append", default=[], help="KIND:FROM:TO (seconds)")
    args = parser.parse_args()
    if np is None:
        print("audio_check: numpy is not installed; nothing was checked")
        return 2
    if not args.capture.exists():
        print(f"audio_check: {args.capture} does not exist")
        return 2
    data = load(args.capture)
    rate = args.rate
    seconds = data.shape[0] / rate
    failures = 0

    def check(ok: bool, line: str) -> None:
        nonlocal failures
        print(f"audio_check: {'ok    ' if ok else 'FAILED'} {line}")
        failures += 0 if ok else 1

    print(f"audio_check: {args.capture.name}, {seconds:.1f} s at {rate} Hz")
    if seconds < 1.0:
        check(False, "the capture is shorter than a second")
        return 1

    rails = (data >= 32767) | (data <= -32767)
    clip_runs = runs(rails[:, 0], 3) + runs(rails[:, 1], 3)
    check(clip_runs <= CLIP_RUNS_MAX, f"whole capture: {clip_runs} runs pinned at the rails (at most {CLIP_RUNS_MAX}); "
          f"{int(rails.sum())} samples at the rails")
    click_count = clicks(data[:, 0]) + clicks(data[:, 1])
    check(click_count / seconds <= CLICKS_PER_SECOND_MAX,
          f"whole capture: {click_count} clicks, {click_count / seconds:.2f} a second (at most {CLICKS_PER_SECOND_MAX})")

    for spec in args.section:
        try:
            kind, start, end = spec.split(":")
            a, b = int(float(start) * rate), int(float(end) * rate)
        except ValueError:
            print(f"audio_check: bad --section {spec!r}")
            return 2
        part = data[a:min(b, data.shape[0])]
        name = f"{kind} {float(start):g}-{float(end):g} s"
        if part.shape[0] < rate // 2:
            check(False, f"{name}: the capture does not reach it")
            continue
        if kind == "silence":
            loud = int(np.count_nonzero(part))
            check(loud == 0, f"{name}: {loud} non-zero samples (silence has none)")
            continue
        if kind != "music":
            continue
        mono = part.mean(axis=1) / 32768.0
        level = 20 * np.log10(np.sqrt(np.mean(mono ** 2)) + 1e-12)
        check(MUSIC_RMS_DB[0] <= level <= MUSIC_RMS_DB[1],
              f"{name}: level {level:.1f} dBFS (between {MUSIC_RMS_DB[0]:g} and {MUSIC_RMS_DB[1]:g})")
        flatness, high, windows = spectrum_stats(mono, rate)
        check(windows > 0 and flatness <= MUSIC_FLATNESS_MAX,
              f"{name}: spectral flatness {flatness:.4f} over {windows} loud windows (at most {MUSIC_FLATNESS_MAX}; "
              f"white noise is about 0.56)")
        check(windows > 0 and high <= MUSIC_HIGH_BAND_MAX,
              f"{name}: {high * 100:.2f}% of the power above 10 kHz (at most {MUSIC_HIGH_BAND_MAX * 100:g}%)")
        silent = (part[:, 0] == 0) & (part[:, 1] == 0)
        drops = runs(silent, 64)
        check(drops <= MUSIC_DROPOUTS_MAX, f"{name}: {drops} drops to digital silence (at most {MUSIC_DROPOUTS_MAX})")

    print(f"audio_check: {'every check passed' if failures == 0 else f'{failures} checks FAILED'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
