#!/usr/bin/env python3
"""Compare a gc_run frame against a reference frame, within a tolerance.

`gc_run --dump-frame FILE.ppm` writes the last frame copied out for
display, decoded back to red, green and blue. P3's gate (docs/GC-PLAN.md)
is that those frames match Dolphin's software renderer, which is the second
implementation in this backend's evidence layers (AGENTS.md) - not
hardware. tools/gc/capture_reference.py produces the reference; this
compares the two and says by how much they disagree.

    python tools/gc/compare_frame.py ours.ppm reference.ppm
    python tools/gc/compare_frame.py ours.ppm reference.ppm --diff diff.ppm
    python tools/gc/compare_frame.py ours.ppm reference.ppm --json

Both files are binary PPMs (P6), which is what gc_run writes and what
capture_reference.py converts Dolphin's PNG dumps into. Nothing outside the
standard library is needed, here or anywhere else in tools/gc.

Exact equality is the wrong gate, and not because either side is sloppy.
The display buffer is YUV 4:2:2 in studio swing, so both programs decode it
back to RGB with their own rounding: a screen cleared to (0, 128, 255)
comes out of gc_run as (1, 128, 255) and out of Dolphin as (0, 127, 254) -
every channel one level apart, every pixel of the frame. That measurement
is where the defaults come from:

  --tolerance 2      a channel within this of the reference is that
                     rounding, not a difference. Twice the level the two
                     decoders were measured to disagree by.
  --max-channel 8    no channel anywhere may be further out than this. A
                     handful of badly wrong pixels - one quad the wrong
                     colour, one row the copy filter missed - fails here
                     even though it is a vanishing fraction of the frame.
  --max-different 1% and no more than this fraction of the frame may differ
                     beyond --tolerance at all, which is what catches a
                     frame that is subtly wrong everywhere.

Exceeding either limit exits 1. A missing file, a malformed header, or two
frames of different sizes exits 2: none of those is a frame that matched,
and this must never report one as if it were.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

DEFAULT_TOLERANCE = 2
DEFAULT_MAX_CHANNEL = 8
DEFAULT_MAX_DIFFERENT = 0.01


class FrameError(Exception):
    """A file that is not a frame this can compare."""


class Frame:
    """An RGB image: width, height, and three bytes a pixel, row by row."""

    def __init__(self, width: int, height: int, raster: bytes) -> None:
        self.width = width
        self.height = height
        self.raster = raster

    @property
    def pixels(self) -> int:
        return self.width * self.height


def fail(message: str) -> int:
    print(f"compare_frame: {message}", file=sys.stderr)
    return 2


def _header_tokens(data: bytes, count: int, pos: int) -> tuple[list[bytes], int]:
    """Read `count` whitespace-separated header tokens, skipping comments.

    A PPM header may carry a '#' comment anywhere between its tokens, so
    the header cannot be split on whitespace alone.
    """
    tokens: list[bytes] = []
    while len(tokens) < count:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace() and data[pos:pos + 1] != b"#":
            pos += 1
        if start == pos:
            raise FrameError("the PPM header ends before it is complete")
        tokens.append(data[start:pos])
    return tokens, pos


def _to_eight_bit(raster: bytes, maxval: int) -> bytes:
    """Scale samples to 0-255, which is the only range worth comparing in."""
    if maxval == 255:
        return raster
    if maxval < 256:
        return raster.translate(bytes(min(255, v * 255 // maxval) for v in range(256)))
    return bytes(((raster[i] << 8 | raster[i + 1]) * 255 // maxval)
                 for i in range(0, len(raster), 2))


def read_ppm(path: Path) -> Frame:
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        raise FrameError(f"{path} does not exist")
    except OSError as error:
        raise FrameError(f"{path}: {error.strerror or error}")

    if data[:2] != b"P6":
        # Dolphin's own dumps are PNGs, and pointing this at one is the
        # likeliest way to get here, so name the tool that converts them.
        raise FrameError(f"{path} is not a binary PPM (P6); Dolphin's PNG frame dumps go "
                         "through tools/gc/capture_reference.py first")
    tokens, pos = _header_tokens(data, 3, 2)
    try:
        width, height, maxval = (int(token) for token in tokens)
    except ValueError:
        raise FrameError(f"{path}: the PPM header is not three numbers")
    if width <= 0 or height <= 0:
        raise FrameError(f"{path}: a {width}x{height} frame is not an image")
    if not 0 < maxval < 65536:
        raise FrameError(f"{path}: {maxval} is not a usable maximum sample value")

    pos += 1  # exactly one whitespace byte separates the header from the raster
    wanted = width * height * 3 * (1 if maxval < 256 else 2)
    raster = data[pos:pos + wanted]
    if len(raster) != wanted:
        raise FrameError(f"{path}: the raster stops after {len(raster)} of {wanted} bytes")
    return Frame(width, height, _to_eight_bit(raster, maxval))


def write_ppm(path: Path, frame: Frame) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as out:
        out.write(b"P6\n%d %d\n255\n" % (frame.width, frame.height))
        out.write(frame.raster)


def compare(ours: Frame, reference: Frame, tolerance: int) -> tuple[dict, bytearray]:
    """Statistics, and a flag per pixel saying whether it differed.

    A pixel differs when any of its channels is more than `tolerance` from
    the reference. The maximum and the mean are reported over every channel
    regardless, because the tolerance is a judgement about what matters and
    those two numbers are the measurement it was judged against.
    """
    a, b = ours.raster, reference.raster
    width, height = ours.width, ours.height
    flags = bytearray(width * height)
    stride = width * 3
    absolute_total = 0
    max_channel = 0
    different = 0
    left, top, right, bottom = width, height, -1, -1

    for y in range(height):
        row = y * stride
        # An identical row contributes nothing to any of these statistics,
        # and the slice comparison is one memcmp rather than 3*width
        # subtractions. Frames that agree exactly are otherwise the slowest
        # case, which is the wrong way round for a test gate.
        if a[row:row + stride] == b[row:row + stride]:
            continue
        for x in range(width):
            i = row + x * 3
            dr = abs(a[i] - b[i])
            dg = abs(a[i + 1] - b[i + 1])
            db = abs(a[i + 2] - b[i + 2])
            absolute_total += dr + dg + db
            worst = dr if dr > dg else dg
            if db > worst:
                worst = db
            if worst > max_channel:
                max_channel = worst
            if worst > tolerance:
                flags[y * width + x] = 1
                different += 1
                if x < left:
                    left = x
                if x > right:
                    right = x
                if y < top:
                    top = y
                if y > bottom:
                    bottom = y

    pixels = width * height
    statistics = {
        "width": width,
        "height": height,
        "pixels": pixels,
        "different": different,
        "fraction": different / pixels,
        "max_channel": max_channel,
        "mean_absolute": absolute_total / (pixels * 3),
        # Inclusive, in pixels, and null rather than an empty rectangle
        # when nothing differed - a consumer should not have to guess
        # whether 0,0,0,0 means one corner pixel or none at all.
        "bounding_box": None if different == 0 else [left, top, right, bottom],
    }
    return statistics, flags


def diff_frame(reference: Frame, flags: bytearray) -> Frame:
    """The reference at quarter intensity, with what differed in magenta.

    Quarter intensity caps every reference channel at 63, so the magenta
    cannot be confused with anything the frame itself contains.
    """
    raster = bytearray(len(reference.raster))
    for pixel, flag in enumerate(flags):
        i = pixel * 3
        if flag:
            raster[i:i + 3] = b"\xff\x00\xff"
        else:
            raster[i] = reference.raster[i] >> 2
            raster[i + 1] = reference.raster[i + 1] >> 2
            raster[i + 2] = reference.raster[i + 2] >> 2
    return Frame(reference.width, reference.height, bytes(raster))


def fraction(text: str) -> float:
    """A fraction of the frame, as 0.01 or as 1%."""
    try:
        value = float(text[:-1]) / 100.0 if text.endswith("%") else float(text)
    except ValueError:
        raise argparse.ArgumentTypeError("expected a fraction such as 0.01, or 1%")
    if not 0.0 <= value <= 1.0:
        raise argparse.ArgumentTypeError("expected a fraction between 0 and 1")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("frame", type=Path, help="the frame gc_run --dump-frame wrote")
    parser.add_argument("reference", type=Path,
                        help="the reference frame (tools/gc/capture_reference.py)")
    parser.add_argument("--tolerance", type=int, default=DEFAULT_TOLERANCE,
                        help="per-channel difference that is not a difference "
                             f"(default {DEFAULT_TOLERANCE}; the two YUV decoders were measured "
                             "one level apart)")
    parser.add_argument("--max-channel", type=int, default=DEFAULT_MAX_CHANNEL,
                        help="largest per-channel difference allowed anywhere "
                             f"(default {DEFAULT_MAX_CHANNEL})")
    parser.add_argument("--max-different", type=fraction, default=DEFAULT_MAX_DIFFERENT,
                        help="fraction of pixels allowed to differ beyond --tolerance "
                             f"(default {DEFAULT_MAX_DIFFERENT:g}; accepts 0.01 or 1%%)")
    parser.add_argument("--diff", type=Path, metavar="FILE.ppm",
                        help="write a difference image: the reference dimmed, "
                             "differing pixels magenta")
    parser.add_argument("--json", action="store_true",
                        help="print the statistics as one line of JSON and nothing else")
    parser.add_argument("--show", type=int, default=8, metavar="N",
                        help="list the first N differing pixels (default 8; 0 for none)")
    args = parser.parse_args()

    if args.tolerance < 0 or args.max_channel < 0:
        return fail("a negative tolerance is not a tolerance")

    try:
        ours = read_ppm(args.frame)
        reference = read_ppm(args.reference)
        if (ours.width, ours.height) != (reference.width, reference.height):
            raise FrameError(
                f"{args.frame} is {ours.width}x{ours.height} and {args.reference} is "
                f"{reference.width}x{reference.height}; there is no pixel-by-pixel "
                "comparison between two different shapes")
    except FrameError as error:
        return fail(str(error))

    statistics, flags = compare(ours, reference, args.tolerance)
    within = (statistics["max_channel"] <= args.max_channel
              and statistics["fraction"] <= args.max_different)
    statistics["tolerance"] = args.tolerance
    statistics["max_channel_allowed"] = args.max_channel
    statistics["max_different_allowed"] = args.max_different
    statistics["pass"] = within

    if args.diff:
        try:
            write_ppm(args.diff, diff_frame(reference, flags))
        except OSError as error:
            return fail(f"{args.diff}: {error.strerror or error}")

    if args.json:
        print(json.dumps(statistics, separators=(",", ":")))
        return 0 if within else 1

    box = statistics["bounding_box"]
    print(f"compare_frame: {ours.width}x{ours.height}, "
          f"{statistics['different']} of {statistics['pixels']} pixels differ by more than "
          f"{args.tolerance} ({100.0 * statistics['fraction']:.4f}%)")
    print(f"  largest per-channel difference {statistics['max_channel']} "
          f"(allowed {args.max_channel})")
    print(f"  mean absolute difference {statistics['mean_absolute']:.4f} per channel")
    print("  bounding box " + ("none" if box is None else
                               f"{box[0]},{box[1]} to {box[2]},{box[3]} "
                               f"({box[2] - box[0] + 1}x{box[3] - box[1] + 1})"))
    for pixel, flag in enumerate(flags[:0] if args.show <= 0 else flags):
        if not flag:
            continue
        i = pixel * 3
        print(f"  ({pixel % ours.width},{pixel // ours.width}) "
              f"ours {ours.raster[i]:02X}{ours.raster[i + 1]:02X}{ours.raster[i + 2]:02X} "
              f"reference {reference.raster[i]:02X}{reference.raster[i + 1]:02X}"
              f"{reference.raster[i + 2]:02X}")
        args.show -= 1
        if args.show == 0:
            break
    if args.diff:
        print(f"  difference image: {args.diff}")
    print("PASS" if within else "FAIL")
    return 0 if within else 1


if __name__ == "__main__":
    sys.exit(main())
