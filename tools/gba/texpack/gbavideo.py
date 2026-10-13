"""Reads video snapshots recorded with --record-textures (gba_texture_capture)
and rebuilds GBA text BG layers and regular sprites from them."""
import hashlib
import struct
from dataclasses import dataclass, field

import numpy as np

IO, PAL, VRAM, OAM = 0x400, 0x400, 0x18000, 0x400
RECORD = 8 + IO + PAL + VRAM + OAM

OBJ_SIZES = {  # (shape, size) -> (w, h) in pixels
    (0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (0, 3): (64, 64),
    (1, 0): (16, 8), (1, 1): (32, 8), (1, 2): (32, 16), (1, 3): (64, 32),
    (2, 0): (8, 16), (2, 1): (8, 32), (2, 2): (16, 32), (2, 3): (32, 64),
}


@dataclass
class Snapshot:
    frame: int
    io: bytes
    pal: bytes
    vram: bytes
    oam: bytes

    def reg(self, off):
        return self.io[off] | self.io[off + 1] << 8

    def colors(self):
        """All 512 palette entries as RGBA uint8 (alpha 255)."""
        raw = np.frombuffer(self.pal, dtype="<u2")
        r, g, b = raw & 31, (raw >> 5) & 31, (raw >> 10) & 31
        out = np.empty((512, 4), np.uint8)
        out[:, 0], out[:, 1], out[:, 2] = (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)
        out[:, 3] = 255
        return out


def read_snapshots(path, every=1):
    with open(path, "rb") as f:
        index = 0
        while True:
            rec = f.read(RECORD)
            if len(rec) < RECORD:
                return
            if index % every == 0:
                assert rec[:4] == b"VSNP"
                frame = struct.unpack_from("<I", rec, 4)[0]
                p = 8
                io = rec[p:p + IO]; p += IO
                pal = rec[p:p + PAL]; p += PAL
                vram = rec[p:p + VRAM]; p += VRAM
                oam = rec[p:p + OAM]
                yield Snapshot(frame, io, pal, vram, oam)
            index += 1


def tile_pixels(data, bpp8):
    """8x8 array of colour indexes from 32 (4bpp) or 64 (8bpp) bytes."""
    if bpp8:
        return np.frombuffer(data, np.uint8).reshape(8, 8)
    b = np.frombuffer(data, np.uint8)
    return np.stack([b & 15, b >> 4], axis=1).reshape(8, 8)


def tile_key(data):
    return hashlib.sha256(data).hexdigest()[:16]


def palette_key(colors16):
    return hashlib.sha256(colors16.tobytes()).hexdigest()[:12]


@dataclass
class TileUse:
    """One tile drawn at one place: layer image coordinates and how."""
    key: str
    data: bytes
    bpp8: bool
    palette: int  # 4bpp palette number, or -1 for 8bpp
    hflip: bool
    vflip: bool
    x: int
    y: int


@dataclass
class LayerImage:
    kind: str  # "bg" or "obj"
    rgba: np.ndarray
    indexes: np.ndarray  # colour index per pixel (0 = transparent)
    uses: list = field(default_factory=list)
    x: int = 0  # obj: screen position; bg: which BG
    y: int = 0
    priority: int = 0


def text_layers(s):
    """Text-mode BG layers enabled in DISPCNT, each as its whole virtual map."""
    dispcnt = s.reg(0)
    mode = dispcnt & 7
    if mode > 2:
        return []
    text_bgs = {0: (0, 1, 2, 3), 1: (0, 1), 2: ()}[mode]
    colors = s.colors()
    layers = []
    for bg in text_bgs:
        if not dispcnt & (0x100 << bg):
            continue
        cnt = s.reg(8 + 2 * bg)
        char_base = ((cnt >> 2) & 3) * 0x4000
        bpp8 = bool(cnt & 0x80)
        screen_base = ((cnt >> 8) & 31) * 0x800
        size = (cnt >> 14) & 3
        cols = 64 if size in (1, 3) else 32
        rows = 64 if size in (2, 3) else 32
        tile_bytes = 64 if bpp8 else 32
        idx = np.zeros((rows * 8, cols * 8), np.uint8)
        pal_of = np.zeros((rows * 8, cols * 8), np.uint16)
        uses = []
        for ty in range(rows):
            for tx in range(cols):
                o = screen_base + 2 * (ty * cols + tx)
                if o + 2 > 0x10000:
                    continue
                entry = s.vram[o] | s.vram[o + 1] << 8
                tile = entry & 0x3FF
                t0 = char_base + tile * tile_bytes
                data = s.vram[t0:t0 + tile_bytes]
                if len(data) < tile_bytes or t0 + tile_bytes > 0x10000:
                    continue
                px = tile_pixels(data, bpp8)
                hf, vf = bool(entry & 0x400), bool(entry & 0x800)
                if hf:
                    px = px[:, ::-1]
                if vf:
                    px = px[::-1, :]
                pal = -1 if bpp8 else entry >> 12
                idx[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = px
                pal_of[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = 0 if bpp8 else pal * 16
                if px.any():
                    uses.append(TileUse(tile_key(data), data, bpp8, pal, hf, vf, tx * 8, ty * 8))
        rgba = colors[(pal_of + idx) % 512].copy()
        rgba[idx == 0, 3] = 0
        layers.append(LayerImage("bg", rgba, idx, uses, bg, 0, cnt & 3))
    return layers


def sprites(s):
    """Visible regular (non-affine) OAM objects, one image each."""
    dispcnt = s.reg(0)
    if not dispcnt & 0x1000:
        return []
    colors = s.colors()
    out = []
    for i in range(128):
        a0, a1, a2 = struct.unpack_from("<HHH", s.oam, i * 8)
        affine = bool(a0 & 0x100)
        if not affine and a0 & 0x200:
            continue  # disabled
        if affine:
            continue  # rotated/scaled objects: art is still captured by the tile uses of other frames
        mode = (a0 >> 10) & 3
        if mode == 3:
            continue
        w, h = OBJ_SIZES.get(((a0 >> 14) & 3, (a1 >> 14) & 3), (0, 0))
        if not w:
            continue
        bpp8 = bool(a0 & 0x2000)
        tile_bytes = 64 if bpp8 else 32
        tile0 = a2 & 0x3FF
        pal = (a2 >> 12) & 15
        hf, vf = bool(a1 & 0x1000), bool(a1 & 0x2000)
        idx = np.zeros((h, w), np.uint8)
        uses = []
        n = 0
        for ty in range(h // 8):
            for tx in range(w // 8):
                t0 = 0x10000 + (tile0 + n) * tile_bytes
                n += 1
                data = s.vram[t0:t0 + tile_bytes]
                if len(data) < tile_bytes:
                    continue
                idx[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = tile_pixels(data, bpp8)
                if any(data):
                    # Position in the flipped image the object is shown as.
                    x = w - 8 - tx * 8 if hf else tx * 8
                    y = h - 8 - ty * 8 if vf else ty * 8
                    uses.append(TileUse(tile_key(data), data, bpp8, -1 if bpp8 else pal, hf, vf, x, y))
        if hf:
            idx = idx[:, ::-1]
        if vf:
            idx = idx[::-1, :]
        base = 256 if bpp8 else 256 + pal * 16
        rgba = colors[(base + idx.astype(np.uint16)) % 512].copy()
        rgba[idx == 0, 3] = 0
        if idx.any():
            x, y = a1 & 0x1FF, a0 & 0xFF
            out.append(LayerImage("obj", rgba, idx, uses, x - 512 if x >= 240 else x, y - 256 if y >= 160 else y,
                                  (a2 >> 10) & 3))
    return out


def screen(s, layers):
    """Rough 240x160 view: BG layers by priority, scrolled, plus sprites (for previews only)."""
    dispcnt = s.reg(0)
    canvas = np.zeros((160, 240, 4), np.uint8)
    canvas[:, :] = s.colors()[0]
    mode = dispcnt & 7
    text_bgs = {0: (0, 1, 2, 3), 1: (0, 1), 2: ()}.get(mode, ())
    enabled = [bg for bg in text_bgs if dispcnt & (0x100 << bg)]
    order = sorted(zip(enabled, layers), key=lambda p: (-(s.reg(8 + 2 * p[0]) & 3), -p[0]))
    for bg, layer in order:
        hofs, vofs = s.reg(0x10 + 4 * bg) & 0x1FF, s.reg(0x12 + 4 * bg) & 0x1FF
        lh, lw = layer.rgba.shape[:2]
        ys = (np.arange(160) + vofs) % lh
        xs = (np.arange(240) + hofs) % lw
        view = layer.rgba[ys][:, xs]
        m = view[:, :, 3] > 0
        canvas[m] = view[m]
    return canvas
