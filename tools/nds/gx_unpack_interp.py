#!/usr/bin/env python3
"""Unpack the nds-interp hardware captures for tests/nds/test_nds_gx_raster.c.

StrikerX3/nds-interp (MIT) ships `nds-interp/data/data.7z`: four files, TL.bin,
TR.bin, BL.bin and BR.bin, each a dump of every line a DS draws from one
corner of the screen to every point (0..256, 0..192). tools/fetch_nds_tests.py
fetches the archive into roms/nds/nds-interp/; this script extracts it.

    python tools/nds/gx_unpack_interp.py [--archive FILE] [--out DIR]

The default output directory is roms/nds/nds-interp-data (not committed, like
everything under roms/). The test program takes that directory as
`--interp DIR`. Running the script again when the files are already there and
have the expected sizes does nothing.

Needs py7zr (pip install py7zr).
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# File name, and its size: a 7-byte header, then for each of the 257 x 193 end
# points a 2-byte tag and 3 bytes for every scanline the previous line could
# touch, then one more block without a tag for the last line (see the
# generator, nds-interp/data/src/source/main.cpp).
EXPECTED = {
    "TL.bin": 14_532_332,
    "TR.bin": 14_532_332,
    "BL.bin": 14_385_644,
    "BR.bin": 14_385_644,
}


def present(out: Path) -> bool:
    return all((out / name).is_file() and (out / name).stat().st_size == size for name, size in EXPECTED.items())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--archive", type=Path,
                        default=ROOT / "roms/nds/nds-interp/nds-interp/data/data.7z")
    parser.add_argument("--out", type=Path, default=ROOT / "roms/nds/nds-interp-data")
    args = parser.parse_args()

    if present(args.out):
        print(f"{args.out}: already unpacked")
        return 0
    if not args.archive.is_file():
        print(f"{args.archive}: not found; run tools/fetch_nds_tests.py first", file=sys.stderr)
        return 1
    try:
        import py7zr
    except ImportError:
        print("py7zr is not installed: pip install py7zr", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    with py7zr.SevenZipFile(args.archive, "r") as archive:
        names = archive.getnames()
        archive.extractall(path=args.out)
    for name in sorted(names):
        path = args.out / name
        if path.is_file():
            print(f"{path}: {path.stat().st_size} bytes")
    if not present(args.out):
        print(f"{args.out}: the archive did not contain the four .bin files", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
