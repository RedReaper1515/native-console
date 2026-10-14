#!/usr/bin/env python3
"""Author texture packs for generated Game Boy games.

    template  export the tiles on screen in savestates as editable PNGs plus pack.json
    build     validate pack.json and its PNGs and write a .gbtp file
    inspect   print what a .gbtp file contains

Run the game with `--texture-pack pack.gbtp`. See docs/TEXTURE_PACKS.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gb_discover as gd  # noqa: E402

SCHEMA = "gbrecomp.texture-pack"
MAGIC = 0x50544247
VERSION = 1
HEADER_SIZE = 184
ENTRY_SIZE = 36
MAX_SCALE = 8
MAX_ENTRIES = 4096
PALETTE_ANY, PALETTE_DMG, PALETTE_CGB = 0, 1, 2
PACK_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$")
LICENSE = re.compile(r"^[A-Za-z0-9.+() -]{1,63}$")


class PackError(ValueError):
    pass


# ---------------------------------------------------------------------------
# PNG (stdlib only)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path: Path) -> tuple[int, int, bytes]:
    """Decode an 8-bit, non-interlaced PNG to (width, height, RGBA bytes)."""
    data = Path(path).read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise PackError(f"{path}: not a PNG")
    pos = 8
    width = height = color_type = 0
    palette = b""
    transparency = b""
    idat = bytearray()
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace != 0 or color_type not in (0, 2, 3, 4, 6):
                raise PackError(f"{path}: only 8-bit non-interlaced PNGs are supported")
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            transparency = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif kind == 4:
                line[i] = (line[i] + _paeth(left, up, up_left)) & 0xFF
            elif kind != 0:
                raise PackError(f"{path}: bad PNG filter {kind}")
        rows.append(bytes(line))
        previous = line
    out = bytearray()
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if color_type == 6:
                out += px
            elif color_type == 2:
                out += px + b"\xff"
            elif color_type == 0:
                out += bytes((px[0], px[0], px[0], 255))
            elif color_type == 4:
                out += bytes((px[0], px[0], px[0], px[1]))
            else:
                index = px[0]
                rgb = palette[index * 3:index * 3 + 3]
                alpha = transparency[index] if index < len(transparency) else 255
                out += rgb + bytes((alpha,))
    return width, height, bytes(out)


# ---------------------------------------------------------------------------
# Manifest


def tile_hash(tile: bytes) -> str:
    return hashlib.sha256(tile).hexdigest()


def palette_key(value) -> tuple[int, int]:
    if value in (None, "any"):
        return PALETTE_ANY, 0
    if isinstance(value, dict) and len(value) == 1:
        (kind, number), = value.items()
        if kind == "dmg" and isinstance(number, int) and 0 <= number <= 255:
            return PALETTE_DMG, number
        if kind == "cgb" and isinstance(number, int) and 0 <= number <= 7:
            return PALETTE_CGB, number
    raise PackError(f"palette must be \"any\", {{\"dmg\": 0-255}} or {{\"cgb\": 0-7}}, got {value!r}")


def palette_label(kind: int, number: int):
    return "any" if kind == PALETTE_ANY else {("dmg" if kind == PALETTE_DMG else "cgb"): number}


def load_manifest(path: Path) -> dict:
    manifest = json.loads(Path(path).read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA or manifest.get("version") != VERSION:
        raise PackError(f"{path}: expected schema {SCHEMA} version {VERSION}")
    pack_id = manifest.get("pack_id", "")
    if not isinstance(pack_id, str) or not PACK_ID.match(pack_id):
        raise PackError("pack_id must be 1-63 characters of letters, digits, '.', '_' or '-'")
    license_id = manifest.get("license", "")
    if not isinstance(license_id, str) or not LICENSE.match(license_id):
        raise PackError("license must be an SPDX expression of at most 63 characters")
    rom = manifest.get("rom", {})
    if not re.fullmatch(r"[0-9a-f]{64}", str(rom.get("sha256", ""))) or not isinstance(rom.get("size"), int):
        raise PackError("rom needs a lowercase sha256 and an integer size")
    scale = manifest.get("scale")
    if not isinstance(scale, int) or not 1 <= scale <= MAX_SCALE:
        raise PackError(f"scale must be 1-{MAX_SCALE}")
    tiles = manifest.get("tiles")
    if not isinstance(tiles, list) or not 1 <= len(tiles) <= MAX_ENTRIES:
        raise PackError(f"tiles must list 1-{MAX_ENTRIES} entries")
    return manifest


def build_pack(manifest_path: Path, out: Path, rom_path: Path | None = None) -> dict:
    manifest_path = Path(manifest_path)
    manifest = load_manifest(manifest_path)
    base = manifest_path.resolve().parent
    scale = manifest["scale"]
    side = 8 * scale
    rom_sha = manifest["rom"]["sha256"]
    if rom_path is not None:
        rom = Path(rom_path).read_bytes()
        if hashlib.sha256(rom).hexdigest() != rom_sha or len(rom) != manifest["rom"]["size"]:
            raise PackError("the ROM given with --rom does not match the manifest")

    records = {}
    for index, tile in enumerate(manifest["tiles"]):
        digest = str(tile.get("tile_sha256", ""))
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise PackError(f"tiles[{index}]: tile_sha256 must be 64 lowercase hex digits")
        kind, number = palette_key(tile.get("palette", "any"))
        image = (base / str(tile.get("image", ""))).resolve()
        if base not in image.parents:
            raise PackError(f"tiles[{index}]: image must stay inside the pack directory")
        if not image.is_file():
            raise PackError(f"tiles[{index}]: missing image {tile.get('image')}")
        width, height, rgba = read_png(image)
        if (width, height) != (side, side):
            raise PackError(f"tiles[{index}]: {tile['image']} is {width}x{height}, expected {side}x{side}")
        key = bytes.fromhex(digest) + bytes((kind, number))
        if key in records:
            raise PackError(f"tiles[{index}]: duplicate tile and palette")
        records[key] = rgba

    ordered = sorted(records)
    header = struct.pack("<II32sIIII64s64s", MAGIC, VERSION, bytes.fromhex(rom_sha),
                         manifest["rom"]["size"], scale, len(ordered), 0,
                         manifest["pack_id"].encode(), manifest["license"].encode())
    assert len(header) == HEADER_SIZE
    body = b"".join(key + b"\0\0" for key in ordered) + b"".join(records[key] for key in ordered)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + body)
    return {"entries": len(ordered), "scale": scale, "bytes": HEADER_SIZE + len(body),
            "sha256": hashlib.sha256(header + body).hexdigest()}


def inspect_pack(path: Path) -> dict:
    data = Path(path).read_bytes()
    if len(data) < HEADER_SIZE:
        raise PackError("file is shorter than a pack header")
    magic, version, rom_sha, rom_size, scale, count, _, pack_id, license_id = \
        struct.unpack_from("<II32sIIII64s64s", data)
    if magic != MAGIC:
        raise PackError("not a texture pack")
    entries = []
    for i in range(count):
        entry = data[HEADER_SIZE + i * ENTRY_SIZE:HEADER_SIZE + (i + 1) * ENTRY_SIZE]
        entries.append({"tile_sha256": entry[:32].hex(), "palette": palette_label(entry[32], entry[33])})
    return {"version": version, "rom_sha256": rom_sha.hex(), "rom_size": rom_size, "scale": scale,
            "pack_id": pack_id.split(b"\0")[0].decode(), "license": license_id.split(b"\0")[0].decode(),
            "entries": entries}


# ---------------------------------------------------------------------------
# Templates


def screen_tiles(state: gd.Savestate) -> set[tuple[int, int, int, bool]]:
    """(bank, slot, palette, is_cgb_palette) for tiles visible with the end-of-frame registers.

    Savestates carry no model flag, so palettes are recorded as DMG register values.
    """
    lcdc = state.io(0xFF40)
    found: set[tuple[int, int, int, bool]] = set()
    if not lcdc & 0x80:
        return found
    unsigned = bool(lcdc & 0x10)

    def map_slot(map_base: int, source_x: int, source_y: int) -> int:
        index = state.data[state.vram_off + map_base - 0x8000 + (source_y >> 3) * 32 + (source_x >> 3)]
        # 8800 addressing: indices 0-127 live at 9000-97FF (slots 256-383).
        return index if unsigned or index >= 128 else index + 256

    if lcdc & 0x01:
        bgp = state.io(0xFF47)
        scx, scy = state.io(0xFF43), state.io(0xFF42)
        wx, wy = state.io(0xFF4B), state.io(0xFF4A)
        bg_map = 0x9C00 if lcdc & 0x08 else 0x9800
        win_map = 0x9C00 if lcdc & 0x40 else 0x9800
        window = bool(lcdc & 0x20) and wx <= 166 and wy < 144
        for y in range(144):
            for x in range(160):
                if window and y >= wy and x >= wx - 7:
                    slot = map_slot(win_map, x - (wx - 7), y - wy)
                else:
                    slot = map_slot(bg_map, (x + scx) & 0xFF, (y + scy) & 0xFF)
                found.add((0, slot, bgp, False))
    if lcdc & 0x02:
        tall = bool(lcdc & 0x04)
        for sprite in state.sprites():
            palette = state.io(0xFF49) if sprite.dmg_palette else state.io(0xFF48)
            base = sprite.tile & 0xFE if tall else sprite.tile
            for half in range(2 if tall else 1):
                found.add((sprite.cgb_bank, base + half, palette, False))
    return found


def write_template(states: list[Path], rom_path: Path, out: Path, scale: int, pack_id: str,
                   license_id: str, per_palette: bool) -> dict:
    rom = Path(rom_path).read_bytes()
    out = Path(out)
    (out / "tiles").mkdir(parents=True, exist_ok=True)
    manifest_path = out / "pack.json"
    tiles: dict[tuple[str, int, int], dict] = {}
    if manifest_path.exists():
        existing = load_manifest(manifest_path)
        for tile in existing["tiles"]:
            kind, number = palette_key(tile.get("palette", "any"))
            tiles[(tile["tile_sha256"], kind, number)] = tile
    added = 0
    for state_path in states:
        state = gd.Savestate.load(state_path)
        for bank, slot, palette, cgb in sorted(screen_tiles(state)):
            raw = state.tile_data(slot, bank)
            if len(set(raw)) <= 1:
                continue  # blank and solid tiles are left to the original renderer
            digest = tile_hash(raw)
            kind, number = ((PALETTE_CGB if cgb else PALETTE_DMG), palette) if per_palette else (PALETTE_ANY, 0)
            if (digest, kind, number) in tiles:
                continue
            name = f"tiles/{digest[:16]}" + (f"_{'cgb' if cgb else 'dmg'}{number:02x}" if per_palette else "") + ".png"
            indices = gd.decode_tile(raw)
            shades = palette if not cgb else 0xE4
            pixels = bytearray()
            for y in range(8 * scale):
                for x in range(8 * scale):
                    color = indices[(y // scale) * 8 + x // scale]
                    rgb = gd.DMG_SHADES[(shades >> (color * 2)) & 3]
                    pixels += bytes(((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255))
            gd.write_png(out / name, 8 * scale, 8 * scale, bytes(pixels))
            tiles[(digest, kind, number)] = {"tile_sha256": digest,
                                             "palette": palette_label(kind, number),
                                             "image": name}
            added += 1
    manifest = {
        "schema": SCHEMA,
        "version": VERSION,
        "pack_id": pack_id,
        "license": license_id,
        "rom": {"sha256": hashlib.sha256(rom).hexdigest(), "size": len(rom)},
        "scale": scale,
        "tiles": sorted(tiles.values(), key=lambda t: (t["tile_sha256"], json.dumps(t["palette"]))),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return {"added": added, "total": len(tiles), "manifest": str(manifest_path)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("template", help="export on-screen tiles as editable PNGs and pack.json")
    p.add_argument("--state", type=Path, action="append", required=True,
                   help="savestate to take tiles from; repeat to cover more screens")
    p.add_argument("--rom", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True, help="pack source directory (merged if it exists)")
    p.add_argument("--scale", type=int, default=4)
    p.add_argument("--pack-id", default="org.example.texture-pack")
    p.add_argument("--license", default="CC-BY-4.0", dest="license_id")
    p.add_argument("--per-palette", action="store_true",
                   help="one entry per tile and palette instead of one per tile")

    p = sub.add_parser("build", help="write a .gbtp file from pack.json")
    p.add_argument("manifest", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--rom", type=Path, help="also check the manifest against this ROM")

    p = sub.add_parser("inspect", help="describe a .gbtp file")
    p.add_argument("pack", type=Path)

    args = parser.parse_args(argv)
    try:
        if args.command == "template":
            if not 1 <= args.scale <= MAX_SCALE:
                raise PackError(f"--scale must be 1-{MAX_SCALE}")
            result = write_template(args.state, args.rom, args.out, args.scale, args.pack_id,
                                    args.license_id, args.per_palette)
            print(f"{result['added']} new tiles, {result['total']} total, manifest {result['manifest']}")
        elif args.command == "build":
            result = build_pack(args.manifest, args.out, args.rom)
            print(f"wrote {args.out}: {result['entries']} entries, scale {result['scale']}, "
                  f"{result['bytes']} bytes, sha256 {result['sha256']}")
        else:
            print(json.dumps(inspect_pack(args.pack), indent=2))
    except (PackError, OSError, json.JSONDecodeError, gd.SavestateError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
