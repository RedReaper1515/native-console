#!/usr/bin/env python3
"""Build and inspect texture packs for generated Game Boy Advance games.

    build     validate pack.json and its PNGs and write a .gbatp file
    inspect   print what a .gbatp file contains

Run the game with `--texture-pack pack.gbatp`. pack.json comes from
tools/gba/texpack (capture, upscale, cut into tiles) or by hand. See
TEXTURE_PACKS.md.

Uses only the standard library; Pillow, when installed, decodes the PNGs
faster.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gb_texture_pack import PackError, read_png  # noqa: E402

MAGIC = 0x50544147  # 'GATP'
VERSION = 1
HEADER = struct.Struct("<II32sIIII64s64sII")
ENTRY = struct.Struct("<32s32sB3xII")
MAX_SCALE = 8
MAX_ENTRIES = 65536
HEX64 = re.compile(r"^[0-9a-f]{64}$")
IDENT = re.compile(r"^[A-Za-z0-9._+()\- ]{1,63}$")

try:
    from PIL import Image
except ImportError:  # the standard-library decoder is used instead
    Image = None


def load_image(path: Path) -> tuple[int, int, bytes]:
    if Image is not None:
        with Image.open(path) as im:
            rgba = im.convert("RGBA")
            return rgba.width, rgba.height, rgba.tobytes()
    return read_png(path)


def build_pack(manifest_path: Path, out: Path, rom_path: Path | None = None) -> dict:
    manifest = json.loads(Path(manifest_path).read_text())
    base = Path(manifest_path).resolve().parent
    if manifest.get("schema") != "native-console.gba-texture-pack" or manifest.get("version") != 1:
        raise PackError("pack.json: expected schema native-console.gba-texture-pack, version 1")
    scale = manifest.get("scale")
    if not isinstance(scale, int) or not 1 <= scale <= MAX_SCALE:
        raise PackError(f"pack.json: scale must be 1-{MAX_SCALE}")
    pack_id = manifest.get("pack_id", "gba-texture-pack")
    license_id = manifest.get("license", "NOASSERTION")
    for label, value in (("pack_id", pack_id), ("license", license_id)):
        if not IDENT.match(value):
            raise PackError(f"pack.json: {label} must be a short identifier")
    tiles = manifest.get("tiles", [])
    if not 1 <= len(tiles) <= MAX_ENTRIES:
        raise PackError(f"pack.json: 1-{MAX_ENTRIES} tiles required")

    side = 8 * scale
    palettes = bytearray()
    palette_offsets: dict[bytes, int] = {}
    rows = []
    for i, t in enumerate(tiles):
        tile, pal = t.get("tile_sha256", ""), t.get("palette_sha256", "")
        if not HEX64.match(tile) or not HEX64.match(pal):
            raise PackError(f"tiles[{i}]: tile_sha256 and palette_sha256 must be 64 lowercase hex digits")
        bpp = t.get("bpp")
        if bpp not in (4, 8):
            raise PackError(f"tiles[{i}]: bpp must be 4 or 8")
        pal_bytes = bytes.fromhex(t.get("palette", ""))
        if len(pal_bytes) != (512 if bpp == 8 else 32) or hashlib.sha256(pal_bytes).hexdigest() != pal:
            raise PackError(f"tiles[{i}]: palette bytes do not match palette_sha256")
        image = (base / t.get("image", "")).resolve()
        if base not in image.parents:
            raise PackError(f"tiles[{i}]: image must be inside the pack directory")
        if not image.is_file():
            raise PackError(f"tiles[{i}]: missing image {t.get('image')}")
        w, h, rgba = load_image(image)
        if (w, h) != (side, side):
            raise PackError(f"tiles[{i}]: {t['image']} is {w}x{h}, expected {side}x{side}")
        if pal_bytes not in palette_offsets:
            palette_offsets[pal_bytes] = len(palettes)
            palettes += pal_bytes
        rows.append((bytes.fromhex(tile) + bytes.fromhex(pal), bpp, palette_offsets[pal_bytes], rgba))

    rows.sort(key=lambda r: r[0])
    for a, b in zip(rows, rows[1:]):
        if a[0] == b[0]:
            raise PackError(f"duplicate tile and palette {a[0][:32].hex()} / {a[0][32:].hex()}")

    rom_sha, rom_size = bytes(32), 0
    if rom_path is not None:
        rom = Path(rom_path).read_bytes()
        rom_sha, rom_size = hashlib.sha256(rom).digest(), len(rom)

    with open(out, "wb") as f:
        f.write(HEADER.pack(MAGIC, VERSION, rom_sha, rom_size, scale, len(rows), len(palettes),
                            pack_id.encode(), license_id.encode(), 0, 0))
        for key, bpp, offset, _ in rows:
            f.write(ENTRY.pack(key[:32], key[32:], bpp, offset, 0))
        f.write(palettes)
        for *_, rgba in rows:
            f.write(rgba)
    return {"entries": len(rows), "scale": scale, "palettes": len(palette_offsets), "bytes": Path(out).stat().st_size}


def inspect_pack(path: Path) -> dict:
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise PackError("too short")
    magic, version, rom_sha, rom_size, scale, count, pal_size, pack_id, license_id, _, _ = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise PackError("not a GBA texture pack")
    return {
        "version": version,
        "rom_sha256": rom_sha.hex() if any(rom_sha) else None,
        "rom_size": rom_size,
        "scale": scale,
        "entries": count,
        "palette_bytes": pal_size,
        "pack_id": pack_id.split(b"\0")[0].decode(),
        "license": license_id.split(b"\0")[0].decode(),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build")
    b.add_argument("manifest", type=Path)
    b.add_argument("--out", type=Path, required=True)
    b.add_argument("--rom", type=Path, help="tie the pack to this exact ROM")
    i = sub.add_parser("inspect")
    i.add_argument("pack", type=Path)
    args = ap.parse_args(argv)
    try:
        if args.command == "build":
            print(json.dumps(build_pack(args.manifest, args.out, args.rom)))
        else:
            print(json.dumps(inspect_pack(args.pack), indent=2))
    except PackError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
