#!/usr/bin/env python3
"""Compare a gba_run frame (PPM) with a reference screenshot (PNG or PPM).

    python tools/gba/compare_frame.py ours.ppm expected.png [--diff diff.png]

Colours are compared in the GBA's 5-bit-per-channel space, so references
captured with a different 5->8 bit expansion still match. Exit status 0
means identical. Standard library only (non-interlaced 8-bit PNGs).
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ppm_to_png import read_ppm, write_png  # noqa: E402


def read_png(path: Path) -> tuple[int, int, list[tuple[int, int, int]]]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    idat = b""
    palette = b""
    width = height = depth = ctype = interlace = 0
    while pos < len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            palette = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
    if depth != 8 or interlace:
        raise ValueError(f"{path}: only 8-bit non-interlaced PNGs are supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray()
    prev = bytearray(stride)
    i = 0
    for _ in range(height):
        ftype = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        out += line
        prev = line
    pixels = []
    for p in range(width * height):
        px = out[p * channels:(p + 1) * channels]
        if ctype == 3:
            idx = px[0] * 3
            pixels.append((palette[idx], palette[idx + 1], palette[idx + 2]))
        elif ctype in (0, 4):
            pixels.append((px[0], px[0], px[0]))
        else:
            pixels.append((px[0], px[1], px[2]))
    return width, height, pixels


def load(path: Path):
    if path.suffix.lower() == ".png":
        return read_png(path)
    width, height, rgb = read_ppm(path)
    return width, height, [tuple(rgb[i:i + 3]) for i in range(0, len(rgb), 3)]


def main() -> int:
    args = sys.argv[1:]
    diff_path = None
    if "--diff" in args:
        i = args.index("--diff")
        diff_path = Path(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    w1, h1, a = load(Path(args[0]))
    w2, h2, b = load(Path(args[1]))
    if (w1, h1) != (w2, h2):
        print(f"size mismatch: {w1}x{h1} vs {w2}x{h2}")
        return 1
    diff = bytearray()
    bad = 0
    for pa, pb in zip(a, b):
        same = all((x >> 3) == (y >> 3) for x, y in zip(pa, pb))
        bad += not same
        diff += bytes(pa) if same else b"\xff\x00\xff"
    print(f"{bad} of {w1 * h1} pixels differ")
    if diff_path:
        write_png(diff_path, w1, h1, bytes(diff), 2)
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
