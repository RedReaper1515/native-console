#!/usr/bin/env python3
"""Compare a PPM frame dump against a reference screenshot.

The dump is a binary PPM (P6, any size, maxval up to 65535). The reference
is a PNG (8-bit RGB / RGBA / grey / grey+alpha, or 1/2/4/8-bit palette or
grey, 16-bit channels reduced to their high byte; no interlace), decoded
with the standard library only (zlib + struct). Some reference files in
the krom test set are BMPs with a .png name; uncompressed 24/32-bit BMPs
are accepted too.

Both images can be cropped (--crop / --crop-ref, X,Y,W,H) and reduced 2x
per axis (--downscale / --downscale-ref: "2" for both axes, or "2x1",
"1x2"). With --auto-scale (the default) an axis that is exactly twice as
large in one image as in the other is reduced to match: a 512x448
interlaced/hires dump against a 256x224 reference, or a 256x224 dump
against a 512x448 reference (the reference is then sampled).

Pixels differ when any channel differs by more than --tolerance (default
0). The script prints the count and percentage of differing pixels and the
first few coordinates, optionally writes a diff image (--diff-out, PPM),
and exits 0 when the count is within --max-diff (a pixel count, or a
percentage such as "0.5%"; default 0), 1 when it is not, 2 on errors
(unreadable file, size mismatch).

Examples:
  compare_frame.py frame.ppm roms/snes/peterlemon/PPU/Rings/Rings.png
  compare_frame.py frame.ppm ref.png --crop 0,0,256,224 --max-diff 0.1%
"""

import argparse
import struct
import sys
import zlib


class ImageError(Exception):
    pass


class Image:
    """RGB image: width, height and a flat list of (r, g, b) tuples."""

    def __init__(self, width, height, pixels):
        if len(pixels) != width * height:
            raise ImageError("pixel count %d does not match %dx%d" % (len(pixels), width, height))
        self.width = width
        self.height = height
        self.pixels = pixels

    def at(self, x, y):
        return self.pixels[y * self.width + x]


# --------------------------------------------------------------------- PPM


def _ppm_tokens(data, count, pos):
    """Read `count` whitespace-separated header tokens, skipping comments."""
    tokens = []
    n = len(data)
    while len(tokens) < count:
        while pos < n and data[pos:pos + 1].isspace():
            pos += 1
        if pos < n and data[pos:pos + 1] == b"#":
            while pos < n and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < n and not data[pos:pos + 1].isspace() and data[pos:pos + 1] != b"#":
            pos += 1
        if start == pos:
            raise ImageError("truncated PPM header")
        tokens.append(data[start:pos])
    return tokens, pos


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise ImageError("%s: not a binary PPM (P6)" % path)
    tokens, pos = _ppm_tokens(data, 3, 2)
    try:
        width, height, maxval = (int(t) for t in tokens)
    except ValueError:
        raise ImageError("%s: bad PPM header" % path)
    if width <= 0 or height <= 0 or not 0 < maxval < 65536:
        raise ImageError("%s: bad PPM dimensions or maxval" % path)
    pos += 1  # single whitespace byte after maxval
    bpc = 1 if maxval < 256 else 2
    need = width * height * 3 * bpc
    raster = data[pos:pos + need]
    if len(raster) != need:
        raise ImageError("%s: truncated PPM raster (%d of %d bytes)" % (path, len(raster), need))
    pixels = []
    if bpc == 1:
        if maxval == 255:
            for i in range(0, need, 3):
                pixels.append((raster[i], raster[i + 1], raster[i + 2]))
        else:
            for i in range(0, need, 3):
                pixels.append(tuple(raster[i + k] * 255 // maxval for k in range(3)))
    else:
        for i in range(0, need, 6):
            pixels.append(tuple(((raster[i + 2 * k] << 8) | raster[i + 2 * k + 1]) * 255 // maxval
                                for k in range(3)))
    return Image(width, height, pixels)


def write_ppm(path, image):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (image.width, image.height))
        f.write(bytes(c for px in image.pixels for c in px))


# --------------------------------------------------------------------- PNG

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter(raw, width, height, bpp_bits):
    """Undo PNG scanline filters; returns a list of bytearrays (one per row)."""
    stride = (width * bpp_bits + 7) // 8
    bpp = max(1, bpp_bits // 8)
    rows = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        if pos + 1 + stride > len(raw):
            raise ImageError("truncated PNG image data")
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 0:
            pass
        elif ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                upleft = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
        else:
            raise ImageError("bad PNG filter type %d" % ftype)
        rows.append(line)
        prev = line
    return rows


def _samples(row, width, count, depth):
    """Unpack `width * count` samples of `depth` bits from a row."""
    n = width * count
    if depth == 8:
        return list(row[:n])
    if depth == 16:
        return [row[2 * i] for i in range(n)]  # high byte
    out = []
    per_byte = 8 // depth
    mask = (1 << depth) - 1
    for i in range(n):
        byte = row[i // per_byte]
        shift = 8 - depth * (i % per_byte + 1)
        out.append((byte >> shift) & mask)
    return out


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != PNG_SIGNATURE:
        raise ImageError("%s: not a PNG" % path)
    pos = 8
    ihdr = None
    palette = None
    idat = []
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length:
            raise ImageError("%s: truncated %r chunk" % (path, ctype))
        pos += 12 + length
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body) - 2, 3)]
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
    if ihdr is None:
        raise ImageError("%s: no IHDR" % path)
    width, height, depth, color_type, compression, filt, interlace = ihdr
    if compression != 0 or filt != 0:
        raise ImageError("%s: unsupported PNG compression/filter method" % path)
    if interlace != 0:
        raise ImageError("%s: interlaced PNGs are not supported" % path)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ImageError("%s: bad PNG color type %d" % (path, color_type))
    allowed = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}[color_type]
    if depth not in allowed:
        raise ImageError("%s: unsupported bit depth %d for color type %d" % (path, depth, color_type))
    if color_type == 3 and not palette:
        raise ImageError("%s: palette PNG without PLTE" % path)
    try:
        raw = zlib.decompress(b"".join(idat))
    except zlib.error as e:
        raise ImageError("%s: bad PNG data (%s)" % (path, e))
    rows = _unfilter(raw, width, height, depth * channels)

    pixels = []
    for row in rows:
        s = _samples(row, width, channels, depth)
        if color_type == 2:
            pixels.extend(zip(s[0::3], s[1::3], s[2::3]))
        elif color_type == 6:
            pixels.extend(zip(s[0::4], s[1::4], s[2::4]))  # alpha ignored
        elif color_type == 3:
            for i in s:
                if i >= len(palette):
                    raise ImageError("%s: palette index %d out of range" % (path, i))
                pixels.append(palette[i])
        else:  # grey, grey + alpha
            step = channels
            scale = 255 // ((1 << depth) - 1) if depth < 8 else 1
            for g in s[0::step]:
                v = g * scale
                pixels.append((v, v, v))
    return Image(width, height, pixels)


# --------------------------------------------------------------------- BMP


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM" or len(data) < 54:
        raise ImageError("%s: not a BMP" % path)
    offset = struct.unpack("<I", data[10:14])[0]
    header_size = struct.unpack("<I", data[14:18])[0]
    if header_size < 40:
        raise ImageError("%s: unsupported BMP header" % path)
    width, height, planes, bits, compression = struct.unpack("<iiHHI", data[18:34])
    if bits not in (24, 32) or compression not in (0, 3):
        raise ImageError("%s: only uncompressed 24/32-bit BMPs are supported" % path)
    bottom_up = height > 0
    height = abs(height)
    bpp = bits // 8
    stride = (width * bpp + 3) & ~3
    pixels = [None] * (width * height)
    for y in range(height):
        src = offset + (height - 1 - y if bottom_up else y) * stride
        row = data[src:src + width * bpp]
        if len(row) != width * bpp:
            raise ImageError("%s: truncated BMP" % path)
        for x in range(width):
            b, g, r = row[x * bpp:x * bpp + 3]
            pixels[y * width + x] = (r, g, b)
    return Image(width, height, pixels)


def read_image(path):
    with open(path, "rb") as f:
        magic = f.read(8)
    if magic.startswith(PNG_SIGNATURE):
        return read_png(path)
    if magic.startswith(b"BM"):
        return read_bmp(path)
    if magic.startswith(b"P6"):
        return read_ppm(path)
    raise ImageError("%s: unknown image format" % path)


# --------------------------------------------------------------- transforms


def crop(image, box):
    x0, y0, w, h = box
    if x0 < 0 or y0 < 0 or w <= 0 or h <= 0 or x0 + w > image.width or y0 + h > image.height:
        raise ImageError("crop %d,%d,%d,%d outside a %dx%d image" % (x0, y0, w, h, image.width, image.height))
    pixels = []
    for y in range(y0, y0 + h):
        start = y * image.width + x0
        pixels.extend(image.pixels[start:start + w])
    return Image(w, h, pixels)


def downscale(image, fx, fy, mode):
    if fx == 1 and fy == 1:
        return image
    if image.width % fx or image.height % fy:
        raise ImageError("cannot reduce %dx%d by %dx%d" % (image.width, image.height, fx, fy))
    w, h = image.width // fx, image.height // fy
    pixels = []
    for y in range(h):
        for x in range(w):
            if mode == "nearest":
                pixels.append(image.at(x * fx, y * fy))
            else:
                acc = [0, 0, 0]
                for dy in range(fy):
                    for dx in range(fx):
                        px = image.at(x * fx + dx, y * fy + dy)
                        for k in range(3):
                            acc[k] += px[k]
                n = fx * fy
                pixels.append(tuple((a + n // 2) // n for a in acc))
    return Image(w, h, pixels)


def parse_box(text):
    try:
        parts = [int(v) for v in text.split(",")]
    except ValueError:
        parts = []
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("expected X,Y,W,H")
    return tuple(parts)


def parse_factor(text):
    try:
        if "x" in text:
            fx, fy = (int(v) for v in text.split("x"))
        else:
            fx = fy = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("expected N or NxM")
    if fx not in (1, 2) or fy not in (1, 2):
        raise argparse.ArgumentTypeError("factors must be 1 or 2")
    return fx, fy


def parse_max_diff(text):
    try:
        if text.endswith("%"):
            return ("percent", float(text[:-1]))
        return ("count", int(text))
    except ValueError:
        raise argparse.ArgumentTypeError("expected a pixel count or a percentage like 0.5%")


# ------------------------------------------------------------------- main


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dump", help="frame dump (binary PPM, P6)")
    ap.add_argument("reference", help="reference image (PNG, or BMP)")
    ap.add_argument("--crop", type=parse_box, help="crop the dump to X,Y,W,H before scaling")
    ap.add_argument("--crop-ref", type=parse_box, help="crop the reference to X,Y,W,H before scaling")
    ap.add_argument("--downscale", type=parse_factor, default=(1, 1), help="reduce the dump by 2, 2x1 or 1x2")
    ap.add_argument("--downscale-ref", type=parse_factor, default=(1, 1), help="reduce the reference likewise")
    ap.add_argument("--no-auto-scale", dest="auto_scale", action="store_false",
                    help="do not match axes that differ by exactly 2x")
    ap.add_argument("--scale-mode", choices=("nearest", "average"), default="nearest",
                    help="how 2x reduction samples (default: nearest, the top-left pixel)")
    ap.add_argument("--tolerance", type=int, default=0, help="per-channel difference allowed (default 0)")
    ap.add_argument("--max-diff", type=parse_max_diff, default=("count", 0),
                    help="differing pixels allowed: a count or a percentage like 0.5%% (default 0)")
    ap.add_argument("--diff-out", help="write a diff image (PPM): differing pixels red, others dimmed")
    ap.add_argument("--show", type=int, default=8, help="list up to this many differing pixels (default 8)")
    args = ap.parse_args(argv)

    try:
        dump = read_ppm(args.dump)
        ref = read_image(args.reference)
        if args.crop:
            dump = crop(dump, args.crop)
        if args.crop_ref:
            ref = crop(ref, args.crop_ref)
        dump = downscale(dump, args.downscale[0], args.downscale[1], args.scale_mode)
        ref = downscale(ref, args.downscale_ref[0], args.downscale_ref[1], args.scale_mode)
        if args.auto_scale:
            dfx = 2 if dump.width == 2 * ref.width else 1
            dfy = 2 if dump.height == 2 * ref.height else 1
            rfx = 2 if ref.width == 2 * dump.width else 1
            rfy = 2 if ref.height == 2 * dump.height else 1
            dump = downscale(dump, dfx, dfy, args.scale_mode)
            ref = downscale(ref, rfx, rfy, args.scale_mode)
        if (dump.width, dump.height) != (ref.width, ref.height):
            raise ImageError("size mismatch: dump %dx%d, reference %dx%d"
                             % (dump.width, dump.height, ref.width, ref.height))
    except (OSError, ImageError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2

    tol = args.tolerance
    diffs = []
    for i, (a, b) in enumerate(zip(dump.pixels, ref.pixels)):
        if abs(a[0] - b[0]) > tol or abs(a[1] - b[1]) > tol or abs(a[2] - b[2]) > tol:
            diffs.append(i)
    total = dump.width * dump.height
    count = len(diffs)
    percent = 100.0 * count / total
    print("%dx%d: %d of %d pixels differ (%.4f%%)" % (dump.width, dump.height, count, total, percent))
    for i in diffs[:args.show]:
        x, y = i % dump.width, i // dump.width
        print("  (%d,%d) dump %02X%02X%02X ref %02X%02X%02X" % ((x, y) + dump.pixels[i] + ref.pixels[i]))

    if args.diff_out:
        bad = set(diffs)
        pixels = [(255, 0, 0) if i in bad else tuple(c // 4 for c in px) for i, px in enumerate(ref.pixels)]
        try:
            write_ppm(args.diff_out, Image(ref.width, ref.height, pixels))
        except OSError as e:
            print("error: %s" % e, file=sys.stderr)
            return 2

    kind, limit = args.max_diff
    ok = count <= limit if kind == "count" else percent <= limit
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
