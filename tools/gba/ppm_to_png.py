#!/usr/bin/env python3
"""Convert binary PPM (P6) frames from gba_run --dump-frame to PNG.

    python tools/gba/ppm_to_png.py frame.ppm [more.ppm ...] [--scale N]

Uses only the standard library. Each output is written next to its input.
"""

import struct
import sys
import zlib
from pathlib import Path


def read_ppm(path: Path):
    data = path.read_bytes()
    parts = []
    pos = 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    if parts[0] != b"P6" or int(parts[3]) != 255:
        raise ValueError(f"{path}: not an 8-bit P6 PPM")
    return int(parts[1]), int(parts[2]), data[pos + 1:]


def write_png(path: Path, width: int, height: int, rgb: bytes, scale: int) -> None:
    rows = []
    for y in range(height):
        row = rgb[y * width * 3:(y + 1) * width * 3]
        if scale > 1:
            row = b"".join(row[x * 3:x * 3 + 3] * scale for x in range(width))
        rows.extend([b"\x00" + row] * scale)

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width * scale, height * scale, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def main() -> int:
    args = sys.argv[1:]
    scale = 1
    if "--scale" in args:
        i = args.index("--scale")
        scale = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 2
    for name in args:
        src = Path(name)
        width, height, rgb = read_ppm(src)
        dst = src.with_suffix(".png")
        write_png(dst, width, height, rgb, scale)
        print(dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
