#!/usr/bin/env python3
"""Compare frame dump directories from the runtime and PyBoy."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


FRAME_RE = re.compile(r"(\d+)")


def collect_frames(directory: Path) -> dict[int, Path]:
    frames: dict[int, Path] = {}
    for path in sorted(directory.iterdir()):
        if not path.is_file():
            continue
        match = FRAME_RE.search(path.stem)
        if not match:
            continue
        frames[int(match.group(1))] = path
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two frame-dump directories.")
    parser.add_argument("runtime_dir", help="Directory containing runtime PPM/PNG dumps")
    parser.add_argument("reference_dir", help="Directory containing PyBoy PNG dumps")
    parser.add_argument("--limit", type=int, default=20, help="How many worst frames to print")
    args = parser.parse_args()

    runtime_frames = collect_frames(Path(args.runtime_dir))
    reference_frames = collect_frames(Path(args.reference_dir))
    shared = sorted(set(runtime_frames) & set(reference_frames))
    if not shared:
        raise SystemExit("No overlapping frame numbers found")

    results = []
    for frame in shared:
        runtime_image = Image.open(runtime_frames[frame]).convert("RGBA")
        reference_image = Image.open(reference_frames[frame]).convert("RGBA")
        if runtime_image.size != reference_image.size:
            raise SystemExit(f"Frame {frame} size mismatch: {runtime_image.size} vs {reference_image.size}")

        diff = ImageChops.difference(runtime_image, reference_image)
        stat = ImageStat.Stat(diff)
        mean_abs_diff = sum(stat.mean) / len(stat.mean)
        changed_pixels = sum(1 for px in diff.getdata() if px != (0, 0, 0, 0))
        results.append((mean_abs_diff, changed_pixels, frame))

    results.sort(reverse=True)
    print(f"Compared {len(shared)} shared frames")
    for mean_abs_diff, changed_pixels, frame in results[: args.limit]:
        print(
            f"frame={frame:05d} mean_abs_diff={mean_abs_diff:.3f} changed_pixels={changed_pixels}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
