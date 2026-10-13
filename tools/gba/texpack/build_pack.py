"""Builds a GBA texture-pack folder from recorded video snapshots.

Snapshots come from a game run with --record-textures FILE (the SDL game or
the headless runner; gba_texture_capture in gba_texture_pack.h).

Every background layer (the whole map) and every sprite the game drew is
upscaled as one image, so each tile is upscaled next to its real neighbours.
The upscaled image is then cut back into tiles: one candidate per place the
tile was drawn, unflipped. Each (tile, palette) pair keeps the candidate that
occurs most often, or else the one closest to all the others.

  python build_pack.py METHOD OUTDIR SNAPSHOTS.bin [...] [--pack-id ID] [--license SPDX]
                       [--preview FILESTEM:FRAME ...]

METHOD: nearest, mmpx, xbrz or ai (upscalers.py). Then build the .gbatp with
tools/gba_texture_pack.py build OUTDIR/pack.json --out PACK.gbatp --rom ROM.

Pack layout:
  pack.json  {"schema": "native-console.gba-texture-pack", "version": 1, "scale": 4,
              "tiles": [{"tile_sha256", "bpp", "palette_sha256", "palette", "image"}]}
  tiles/<tile>_<palette>.png   (8*scale x 8*scale RGBA)

Palettes: each tile keeps the palette it was drawn with most often. Another
palette that is a per-channel linear change of it (a fade to black or white,
a flash) is not stored: the runtime recolours the image with that fit, as
preview does. Palettes that are not such a change (a recoloured enemy) get
their own image. Entries carry the palette bytes (hex) so the fit can be made.

tile_sha256 is the SHA-256 of the tile's VRAM bytes (32 for 4bpp, 64 for
8bpp); palette_sha256 is the SHA-256 of its palette RAM bytes (16 or 256
BGR555 colours: the BG or OBJ palette in use).
"""
import argparse
import collections
import hashlib
import json
import random
import time
from pathlib import Path

import numpy as np
from PIL import Image

import gbavideo as gv
import upscalers as up

S = up.SCALE
MAX_CANDIDATES = 24
METHODS = up.METHODS


def palette_bytes(s, use, where):
    base = 0 if where == "bg" else 0x200
    if use.bpp8:
        return s.pal[base:base + 0x200]
    return s.pal[base + use.palette * 32:base + use.palette * 32 + 32]


def unflip(block, use):
    if use.hflip:
        block = block[:, ::-1]
    if use.vflip:
        block = block[::-1, :]
    return block


class Harvest:
    def __init__(self):
        self.candidates = collections.defaultdict(list)  # (tile, palette) -> [blocks]
        self.seen = collections.Counter()
        self.bpp = {}
        self.palettes = {}  # palette sha -> bytes
        self.rng = random.Random(1)

    def add(self, key, bpp, block):
        n = self.seen[key]
        self.seen[key] += 1
        self.bpp[key] = bpp
        c = self.candidates[key]
        if len(c) < MAX_CANDIDATES:
            c.append(block.copy())
        else:  # reservoir sampling keeps a fair sample of every place it was drawn
            j = self.rng.randrange(n + 1)
            if j < MAX_CANDIDATES:
                c[j] = block.copy()

    def choose(self, key):
        c = self.candidates[key]
        counts = collections.Counter(b.tobytes() for b in c)
        best, n = counts.most_common(1)[0]
        if n > 1 or len(c) == 1:
            return next(b for b in c if b.tobytes() == best)
        stack = np.stack(c).astype(np.int32)
        dist = np.abs(stack[:, None] - stack[None, :]).sum(axis=(2, 3, 4))
        return c[int(dist.sum(1).argmin())]


def harvest_image(h, s, layer, big, where):
    for use in layer.uses:
        block = big[use.y * S:(use.y + 8) * S, use.x * S:(use.x + 8) * S]
        if block.shape[:2] != (8 * S, 8 * S):
            continue
        tile = hashlib.sha256(use.data).hexdigest()
        pal = hashlib.sha256(palette_bytes(s, use, where)).hexdigest()
        h.palettes[pal] = palette_bytes(s, use, where)
        h.add((tile, pal), 8 if use.bpp8 else 4, unflip(block, use))


def run(method, snapshot_files, h):
    fn = METHODS[method]
    seen_layers, seen_sprites, seen_snap = set(), set(), set()
    stats = collections.Counter()
    for path in snapshot_files:
        for s in gv.read_snapshots(path):
            sig = hashlib.sha1(s.io[:0x60] + s.pal + s.vram + s.oam).digest()
            if sig in seen_snap:
                continue
            seen_snap.add(sig)
            for layer in gv.text_layers(s):
                lsig = hashlib.sha1(layer.rgba.tobytes()).digest()
                if lsig in seen_layers or not layer.uses:
                    continue
                seen_layers.add(lsig)
                harvest_image(h, s, layer, fn(layer.rgba), "bg")
                stats["bg layers"] += 1
            for sprite in gv.sprites(s):
                ssig = hashlib.sha1(sprite.rgba.tobytes() + repr(sprite.rgba.shape).encode()).digest()
                if ssig in seen_sprites:
                    continue
                seen_sprites.add(ssig)
                harvest_image(h, s, sprite, fn(sprite.rgba), "obj")
                stats["sprites"] += 1
    return stats


def colours(pal_bytes):
    """BGR555 bytes -> (n, 3) float array of 5-bit channel values."""
    raw = np.frombuffer(pal_bytes, "<u2").astype(np.int32)
    return np.stack([raw & 31, (raw >> 5) & 31, (raw >> 10) & 31], 1).astype(np.float64)


def channel_fit(src_pal, dst_pal):
    """Per-channel dst = a * src + b over colours 1..n-1, and the worst error (5-bit units)."""
    src, dst = colours(src_pal)[1:], colours(dst_pal)[1:]
    coef, worst = [], 0.0
    for c in range(3):
        x, y = src[:, c], dst[:, c]
        if np.ptp(x) == 0:
            a, b = 0.0, float(y.mean())
        else:
            a, b = np.polyfit(x, y, 1)
        worst = max(worst, float(np.abs(a * x + b - y).max()))
        coef.append((float(a), float(b)))
    return coef, worst


def recolour(img, coef):
    """Applies a 5-bit channel fit to 8-bit RGBA art."""
    out = img.astype(np.float64)
    for c, (a, b) in enumerate(coef):
        out[..., c] = a * out[..., c] + b * 255 / 31
    out[..., :3] = out[..., :3].clip(0, 255)
    return out.round().astype(np.uint8)


def write_pack(h, out, method, pack_id="gba-texture-pack", license_id="NOASSERTION"):
    tiles_dir = out / "tiles"
    tiles_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    by_tile = collections.defaultdict(list)
    for tile, pal in h.candidates:
        by_tile[tile].append(pal)
    keep = []
    folded = 0
    for tile, pals in by_tile.items():
        pals.sort(key=lambda p: -h.seen[(tile, p)])
        kept = []
        for pal in pals:
            if any(channel_fit(h.palettes[k], h.palettes[pal])[1] <= 1.0 for k in kept):
                folded += 1
                continue
            kept.append(pal)
            keep.append((tile, pal))
    print(f"palettes: {len(keep)} entries kept, {folded} fade/flash variants folded into them")
    for (tile, pal) in sorted(keep):
        name = f"{tile[:16]}_{pal[:12]}.png"
        Image.fromarray(np.ascontiguousarray(h.choose((tile, pal)))).save(tiles_dir / name)
        entries.append({"tile_sha256": tile, "bpp": h.bpp[(tile, pal)], "palette_sha256": pal,
                        "palette": h.palettes[pal].hex(), "image": f"tiles/{name}"})
    manifest = {"schema": "native-console.gba-texture-pack", "version": 1, "scale": S,
                "pack_id": pack_id, "license": license_id, "method": method, "tiles": entries}
    (out / "pack.json").write_text(json.dumps(manifest, indent=1))
    return entries


# ---------------------------------------------------------------- preview

def load_pack(out):
    manifest = json.loads((out / "pack.json").read_text())
    images = {}
    for e in manifest["tiles"]:
        img = np.asarray(Image.open(out / e["image"]).convert("RGBA"))
        images[(e["tile_sha256"], e["palette_sha256"])] = img
        images.setdefault(e["tile_sha256"], []).append((bytes.fromhex(e["palette"]), img))
    return images


def pack_image(s, layer, where, images, stats):
    """The layer rebuilt at S x from pack tiles only (original tile, scaled, where missing)."""
    h, w = layer.rgba.shape[:2]
    big = np.zeros((h * S, w * S, 4), np.uint8)
    for use in layer.uses:
        tile = hashlib.sha256(use.data).hexdigest()
        pal = hashlib.sha256(palette_bytes(s, use, where)).hexdigest()
        img = images.get((tile, pal))
        if img is None and tile in images:
            current = palette_bytes(s, use, where)
            fits = [(channel_fit(p, current), art) for p, art in images[tile]]
            (coef, worst), art = min(fits, key=lambda f: f[0][1])
            if worst <= 1.0:
                img = recolour(art, coef)
                stats["recoloured"] += 1
        if img is None:
            stats["missing"] += 1
            img = unflip(up.nearest(layer.rgba[use.y:use.y + 8, use.x:use.x + 8]), use)
        elif (tile, pal) in images:
            stats["replaced"] += 1
        if use.hflip:
            img = img[:, ::-1]
        if use.vflip:
            img = img[::-1, :]
        big[use.y * S:(use.y + 8) * S, use.x * S:(use.x + 8) * S] = img
    return big


def preview(out, snapshot_files, wanted):
    images = load_pack(out)
    stats = collections.Counter()
    want = {}
    for w in wanted:
        run_name, frame = w.split(":")
        want.setdefault(run_name, set()).add(int(frame))
    for path in snapshot_files:
        frames = want.get(Path(path).stem, set())
        for s in gv.read_snapshots(path):
            if s.frame not in frames:
                continue
            cache = {}

            def key(rgba):
                return rgba.tobytes() + repr(rgba.shape).encode()

            def fn(rgba):
                return cache[key(rgba)]

            layers, objs = gv.text_layers(s), gv.sprites(s)
            for l in layers:
                cache[key(l.rgba)] = pack_image(s, l, "bg", images, stats)
            for o in objs:
                cache[key(o.rgba)] = pack_image(s, o, "obj", images, stats)
            img = up.compose(s, fn)
            Image.fromarray(img).save(out / f"preview_{Path(path).stem}_{s.frame}.png")
    print(f"preview: {dict(stats)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("method", choices=sorted(METHODS))
    ap.add_argument("out", type=Path)
    ap.add_argument("snapshots", nargs="+")
    ap.add_argument("--preview", nargs="*", default=[])
    ap.add_argument("--preview-only", action="store_true")
    ap.add_argument("--pack-id", default="gba-texture-pack")
    ap.add_argument("--license", default="NOASSERTION")
    args = ap.parse_args()
    t0 = time.time()
    if not args.preview_only:
        h = Harvest()
        stats = run(args.method, args.snapshots, h)
        entries = write_pack(h, args.out, args.method, args.pack_id, args.license)
        multi = sum(1 for k in h.candidates if len(h.candidates[k]) > 1)
        print(f"{args.method}: {dict(stats)}, {len(entries)} tiles ({multi} drawn in more than one place), "
              f"{time.time() - t0:.0f} s")
    if args.preview:
        preview(args.out, args.snapshots, args.preview)


if __name__ == "__main__":
    main()
