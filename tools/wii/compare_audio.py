#!/usr/bin/env python3
"""Line up two dumps of a game's sound and say how alike they are.

    python tools/wii/compare_audio.py ours.raw dolphin.raw

Both files are what the runner's --dump-audio writes and what
tools/wii/oracle_dolphin_game.py --audio brings back: sixteen-bit
little-endian stereo with no header. The two machines do not reach the same
point at the same moment, so the files are lined up by content: a stretch of
the second file is slid along the first to where the two agree best, and
from there the samples are compared one for one until either file ends.

What it prints is the offset it found, and then for each second: how many
samples are the same to the bit, the largest difference, and the correlation.
A mixer that does what the other does gives the same samples for as long as
the game asks both for the same sounds - which, for music started by the
same press of a button, is until something else (a sound effect, a different
frame for the press) makes the two games differ. So read the first seconds
after the lined-up point: identical there and drifting later is the games
diverging; never identical is the mixers differing.

Exit status 0 when at least --need seconds are identical to the bit.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np


def load(path: Path) -> np.ndarray:
    data = np.fromfile(path, dtype="<i2")
    return data[: len(data) // 2 * 2].reshape(-1, 2).astype(np.int32)


def first_sound(samples: np.ndarray) -> int:
    loud = np.flatnonzero(np.abs(samples).max(axis=1) > 0)
    return int(loud[0]) if len(loud) else len(samples)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ours", type=Path)
    parser.add_argument("theirs", type=Path)
    parser.add_argument("--rate", type=int, default=32000)
    parser.add_argument("--need", type=float, default=1.0, help="seconds that must be identical for success")
    args = parser.parse_args()

    ours, theirs = load(args.ours), load(args.theirs)
    a, b = first_sound(ours), first_sound(theirs)
    if a == len(ours) or b == len(theirs):
        print("compare_audio: one of the files is all silence", file=sys.stderr)
        return 1

    # A second of the other file from where its sound begins, slid over ours.
    probe = theirs[b:b + args.rate, 0].astype(np.float64)
    reach = ours[:, 0].astype(np.float64)
    if len(probe) < 256 or len(reach) < len(probe):
        print("compare_audio: too little sound to line up", file=sys.stderr)
        return 1
    size = 1 << int(np.ceil(np.log2(len(reach) + len(probe))))
    score = np.fft.irfft(np.fft.rfft(reach, size) * np.conj(np.fft.rfft(probe, size)), size)[: len(reach) - len(probe) + 1]
    at = int(np.argmax(score))
    offset = at - b  # theirs[i] is ours[i + offset]
    print(f"sound begins at sample {a} of {args.ours.name} and {b} of {args.theirs.name}; "
          f"lined up with {args.theirs.name} {offset:+d} samples ({offset / args.rate:+.2f} s)")

    start = max(b, -offset)
    count = min(len(theirs) - start, len(ours) - (start + offset))
    x = ours[start + offset:start + offset + count]
    y = theirs[start:start + count]
    identical_run = 0
    still_identical = True
    for second in range(0, count // args.rate + 1):
        p, q = x[second * args.rate:(second + 1) * args.rate], y[second * args.rate:(second + 1) * args.rate]
        if not len(p):
            break
        same = int((p == q).all(axis=1).sum())
        worst = int(np.abs(p - q).max())
        pf, qf = p.astype(np.float64).ravel(), q.astype(np.float64).ravel()
        denominator = np.sqrt((pf * pf).sum() * (qf * qf).sum())
        correlation = float((pf * qf).sum() / denominator) if denominator else 1.0
        print(f"{second:4d}s  identical {same:6d} of {len(p):6d}  largest difference {worst:6d}  correlation {correlation:+.4f}")
        if still_identical and same == len(p):
            identical_run += len(p)
        else:
            if still_identical:
                differing = np.flatnonzero((p != q).any(axis=1))
                identical_run += int(differing[0]) if len(differing) else len(p)
            still_identical = False
    print(f"identical to the bit for the first {identical_run} samples ({identical_run / args.rate:.2f} s) from the lined-up point")
    return 0 if identical_run >= args.need * args.rate else 1


if __name__ == "__main__":
    sys.exit(main())
