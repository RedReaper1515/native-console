"""Extracts GBA tile graphics: unpacks the ROM, pairs tiles with palettes from
recorded video snapshots (--record-textures), and writes browsable sheets,
context images, and a tile database. For exploring a game's art; packs are
built from snapshots with build_pack.py.

  python extract.py ROM OUTDIR SNAPSHOTS.bin [...]
"""
import collections
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

import gbavideo as gv

SHEET_COLS = 16


# ---------------------------------------------------------------- ROM

def lz77(data, pos):
    if data[pos] != 0x10:
        return None
    size = data[pos + 1] | data[pos + 2] << 8 | data[pos + 3] << 16
    if size < 32 or size > 0x40000:
        return None
    dst = bytearray()
    p, end = pos + 4, len(data)
    while len(dst) < size:
        if p >= end:
            return None
        flags = data[p]
        p += 1
        for bit in range(8):
            if len(dst) >= size:
                break
            if flags & (0x80 >> bit):
                if p + 1 >= end:
                    return None
                b0, b1 = data[p], data[p + 1]
                p += 2
                length, disp = (b0 >> 4) + 3, ((b0 & 0xF) << 8 | b1) + 1
                if disp > len(dst):
                    return None
                for _ in range(length):
                    dst.append(dst[-disp])
            else:
                if p >= end:
                    return None
                dst.append(data[p])
                p += 1
    if len(dst) != size:
        return None
    return bytes(dst), p - pos


def scan_lz77(rom):
    blocks, pos = [], 0
    while pos < len(rom) - 4:
        r = lz77(rom, pos)
        if r and r[1] < len(r[0]):
            blocks.append((pos, r[0]))
            pos += (r[1] + 3) & ~3
        else:
            pos += 4
    return blocks


# ---------------------------------------------------------------- runtime

class TileDB:
    def __init__(self):
        self.tiles = {}  # key -> dict

    def add(self, use, colors, where):
        t = self.tiles.get(use.key)
        if t is None:
            t = self.tiles[use.key] = {"data": use.data, "bpp8": use.bpp8, "palettes": collections.Counter(),
                                       "colors": {}, "layers": set()}
        if use.bpp8:
            pal = colors[0:256] if where == "bg" else colors[256:512]
        else:
            base = (0 if where == "bg" else 256) + use.palette * 16
            pal = colors[base:base + 16]
        pk = gv.palette_key(pal)
        t["palettes"][pk] += 1
        t["colors"].setdefault(pk, pal)
        t["layers"].add(where)


def layer_signature(s, bg):
    cnt = s.reg(8 + 2 * bg)
    char_base = ((cnt >> 2) & 3) * 0x4000
    screen_base = ((cnt >> 8) & 31) * 0x800
    h = hashlib.sha1()
    h.update(cnt.to_bytes(2, "little"))
    h.update(s.vram[screen_base:screen_base + 0x2000])
    h.update(s.vram[char_base:char_base + 0x8000])
    h.update(s.pal[:0x200])
    return h.digest()


def runtime_pass(snapshot_files, out, db):
    ctx = out / "context"
    (ctx / "bg").mkdir(parents=True, exist_ok=True)
    (ctx / "obj").mkdir(parents=True, exist_ok=True)
    seen_layers, seen_sprites, seen_snap = set(), set(), set()
    stats = collections.Counter()
    for path in snapshot_files:
        for s in gv.read_snapshots(path):
            stats["snapshots"] += 1
            sig = hashlib.sha1(s.io[:0x60] + s.pal + s.vram + s.oam).digest()
            if sig in seen_snap:
                continue
            seen_snap.add(sig)
            colors = s.colors()
            dispcnt = s.reg(0)
            mode = dispcnt & 7
            text_bgs = {0: (0, 1, 2, 3), 1: (0, 1), 2: ()}.get(mode, ())
            new_bgs = [bg for bg in text_bgs if dispcnt & (0x100 << bg) and layer_signature(s, bg) not in seen_layers]
            if new_bgs:
                for bg, layer in zip([b for b in text_bgs if dispcnt & (0x100 << b)], gv.text_layers(s)):
                    lsig = layer_signature(s, bg)
                    if lsig in seen_layers:
                        continue
                    seen_layers.add(lsig)
                    for use in layer.uses:
                        db.add(use, colors, "bg")
                    if layer.uses:
                        name = f"f{s.frame:06d}_bg{bg}_{lsig.hex()[:8]}"
                        save_context(ctx / "bg", name, layer)
                        stats["bg layers"] += 1
            for sprite in gv.sprites(s):
                ssig = hashlib.sha1(sprite.rgba.tobytes() + bytes(sprite.rgba.shape[:2])).digest()
                if ssig in seen_sprites:
                    continue
                seen_sprites.add(ssig)
                for use in sprite.uses:
                    db.add(use, colors, "obj")
                save_context(ctx / "obj", f"f{s.frame:06d}_{ssig.hex()[:8]}", sprite)
                stats["sprites"] += 1
    return stats


def save_context(directory, name, layer):
    """The layer image (cropped to its drawn area) plus where each tile sits."""
    ys, xs = np.nonzero(layer.indexes)
    if not len(ys):
        return
    y0, x0 = int(ys.min() // 8) * 8, int(xs.min() // 8) * 8
    y1, x1 = int(-(-(ys.max() + 1) // 8)) * 8, int(-(-(xs.max() + 1) // 8)) * 8
    Image.fromarray(layer.rgba[y0:y1, x0:x1]).save(directory / f"{name}.png")
    uses = [{"key": u.key, "palette": u.palette, "hflip": u.hflip, "vflip": u.vflip, "x": u.x - x0, "y": u.y - y0}
            for u in layer.uses if y0 <= u.y < y1 and x0 <= u.x < x1]
    (directory / f"{name}.json").write_text(json.dumps(uses))


# ---------------------------------------------------------------- sheets

GREY = np.array([[v, v, v, 255] for v in np.linspace(0, 255, 16).astype(np.uint8)], np.uint8)


def sheet(tile_datas, db, bpp8=False):
    """16 columns of tiles. A tile the game drew uses its most used palette;
    an unseen one borrows the palette of the nearest drawn tile within 64
    tiles (neighbouring frames of one sprite or tileset usually share one),
    else grey. Transparent pixels are alpha 0 once a palette is known."""
    n = len(tile_datas)
    rows = -(-n // SHEET_COLS)
    img = np.zeros((rows * 8, SHEET_COLS * 8, 4), np.uint8)
    known = [None] * n
    for i, data in enumerate(tile_datas):
        t = db.tiles.get(gv.tile_key(data))
        if t and t["palettes"]:
            known[i] = t["colors"][t["palettes"].most_common(1)[0][0]]
    seen_at = [i for i in range(n) if known[i] is not None]
    coloured = len(seen_at)
    j = 0
    for i, data in enumerate(tile_datas):
        px = gv.tile_pixels(data, bpp8)
        pal = known[i]
        if pal is None and seen_at:
            while j + 1 < len(seen_at) and abs(seen_at[j + 1] - i) <= abs(seen_at[j] - i):
                j += 1
            if abs(seen_at[j] - i) <= 64:
                pal = known[seen_at[j]]
        if pal is None:
            block = (GREY if not bpp8 else np.repeat(GREY, 16, axis=0))[px].copy()
        else:
            block = pal[px].copy()
            block[px == 0, 3] = 0
        y, x = (i // SHEET_COLS) * 8, (i % SHEET_COLS) * 8
        img[y:y + 8, x:x + 8] = block
    return img, coloured


def art_score(data):
    """How much a 4bpp tile looks like pixel art: the share of neighbouring
    pixels with the same colour index (drawn tiles ~0.4-0.9, other data lower)."""
    px = gv.tile_pixels(data, False)
    return ((px[:, 1:] == px[:, :-1]).mean() + (px[1:, :] == px[:-1, :]).mean()) / 2


def window_score(rom, pos):
    """Median art score of the 16 tiles from pos."""
    return float(np.median([art_score(rom[pos + i * 32:pos + i * 32 + 32]) for i in range(16)
                            if pos + i * 32 + 32 <= len(rom)] or [0]))


def main():
    rom_path, out = Path(sys.argv[1]), Path(sys.argv[2])
    snapshot_files = sys.argv[3:]
    rom = rom_path.read_bytes()
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()

    db = TileDB()
    stats = runtime_pass(snapshot_files, out, db)
    print(f"runtime: {dict(stats)}, {len(db.tiles)} distinct tiles seen ({time.time() - t0:.0f} s)")

    blocks = scan_lz77(rom)
    lz_index = {}
    for off, data in blocks:
        if len(data) % 32 == 0:
            for i in range(0, len(data), 32):
                lz_index.setdefault(data[i:i + 32], (off, i // 32))
    print(f"ROM: {len(blocks)} LZ77 streams ({time.time() - t0:.0f} s)")

    # Where each seen tile lives in the ROM. Near-blank tiles (few distinct
    # bytes) match almost anywhere, so they are not searched for raw.
    raw_hits = []
    source = {}
    for key, t in db.tiles.items():
        data = t["data"]
        if data in lz_index:
            off, i = lz_index[data]
            source[key] = f"lz77:{off:07X}:{i}"
            continue
        p = rom.find(data) if len(set(data)) >= 4 else -1
        while p != -1 and p % 4:
            p = rom.find(data, p + 1)
        if p != -1:
            source[key] = f"raw:{p:07X}"
            raw_hits.append(p)
        else:
            source[key] = "runtime"

    sheets = out / "sheets"
    for sub in ("lz77", "lz77-likely", "raw"):
        (sheets / sub).mkdir(parents=True, exist_ok=True)
    summary = collections.Counter()
    distinct_tiles, distinct_coloured = set(), set()

    def record(tiles):
        for data in tiles:
            k = gv.tile_key(data)
            distinct_tiles.add(k)
            if k in db.tiles:
                distinct_coloured.add(k)

    # Compressed blocks: confirmed when the game drew one of their tiles,
    # "likely" when unseen but they look like pixel art; the rest are other data.
    for off, data in blocks:
        if len(data) % 32:
            continue
        tiles = [data[i:i + 32] for i in range(0, len(data), 32)]
        img, coloured = sheet(tiles, db)
        if coloured:
            sub = "lz77"
        elif np.median([art_score(x) for x in tiles]) >= 0.45:
            sub = "lz77-likely"
        else:
            summary["lz77 skipped (not graphics)"] += 1
            continue
        Image.fromarray(img).save(sheets / sub / f"{off:07X}_{len(tiles)}tiles.png")
        summary[sub] += 1
        record(tiles)

    # Uncompressed graphics: grow from each tile the game drew, a tile at a
    # time, while the surrounding 16 tiles still look like pixel art.
    regions = []
    for hit in sorted(raw_hits):
        if regions and regions[-1][0] <= hit < regions[-1][1] and (hit - regions[-1][0]) % 32 == 0:
            continue
        a = b = hit
        while a - 32 >= 0 and hit - a < 0x10000 and window_score(rom, a - 32) >= 0.4:
            a -= 32
        while b + 32 <= len(rom) and b - hit < 0x10000 and window_score(rom, b) >= 0.4:
            b += 32
        b = max(b, hit + 32)
        if regions and a <= regions[-1][1] and (a - regions[-1][0]) % 32 == 0:
            regions[-1] = (regions[-1][0], max(regions[-1][1], b))
        else:
            regions.append((a, b))
    for a, b in regions:
        tiles = [rom[i:i + 32] for i in range(a, b, 32)]
        img, coloured = sheet(tiles, db)
        Image.fromarray(img).save(sheets / "raw" / f"{a:07X}_{len(tiles)}tiles.png")
        summary["raw"] += 1
        record(tiles)

    by_source = collections.Counter(v.split(":")[0] for v in source.values())
    db_json = {
        key: {
            "source": source[key],
            "bpp8": t["bpp8"],
            "layers": sorted(t["layers"]),
            "palettes": [{"key": pk, "uses": n, "colors": t["colors"][pk][:, :3].tolist()}
                         for pk, n in t["palettes"].most_common()],
            "data": t["data"].hex(),
        }
        for key, t in db.tiles.items()
    }
    (out / "tiles.json").write_text(json.dumps(db_json))
    report = {
        "runtime": dict(stats),
        "tiles_seen": len(db.tiles),
        "tiles_seen_by_source": dict(by_source),
        "sheets": summary,
        "distinct_tiles_in_sheets": len(distinct_tiles),
        "of_which_seen_in_game_with_colours": len(distinct_coloured),
        "seconds": round(time.time() - t0),
    }
    (out / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
