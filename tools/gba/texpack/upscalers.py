"""Upscalers for GBA texture packs, and a 4x scene compositor for previews.

Every upscaler takes an RGBA uint8 array (h, w, 4) and returns (h*4, w*4, 4).

  nearest  plain pixel repetition
  mmpx     MMPX 2x twice, from the runtime (runtime-gba/src/gba_upscale.c)
           built as a DLL/shared library: set GBA_MMPX_LIB
  xbrz     xBRZ 4x (GPL-3, not part of this repository): build xbrz.cpp from
           https://sourceforge.net/projects/xbrz/ with xbrz_c.cpp into a
           shared library and set GBA_XBRZ_LIB
  ai       an ESRGAN-family model through spandrel + PyTorch (CUDA): set
           GBA_UPSCALE_MODEL to the model file

See README.md for build commands.
"""
from __future__ import annotations

import ctypes
import os

import numpy as np

SCALE = 4


def nearest(rgba):
    return rgba.repeat(SCALE, 0).repeat(SCALE, 1)


def to_argb32(rgba):
    r, g, b, a = (rgba[..., i].astype(np.uint32) for i in range(4))
    return np.ascontiguousarray((a << 24) | (r << 16) | (g << 8) | b)


def from_argb32(px):
    return np.stack([(px >> 16) & 255, (px >> 8) & 255, px & 255, px >> 24], -1).astype(np.uint8)


def fill_transparent(rgba):
    """RGB with transparent pixels grown over from their opaque neighbours."""
    alpha = rgba[..., 3] > 0
    if alpha.all() or not alpha.any():
        return rgba[..., :3].copy()
    rgb = rgba[..., :3].astype(np.float32)
    known = alpha.copy()
    rgb[~known] = 0
    while not known.all():
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
            cnt += k
        grow = ~known & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        known |= grow
    return rgb.round().astype(np.uint8)


_libs = {}


def _lib(env, symbol, argtypes):
    if env not in _libs:
        path = os.environ.get(env)
        if not path:
            raise RuntimeError(f"set {env} to the shared library (see tools/gba/texpack/README.md)")
        lib = ctypes.CDLL(path)
        getattr(lib, symbol).argtypes = argtypes
        _libs[env] = lib
    return getattr(_libs[env], symbol)


def mmpx(rgba):
    fn = _lib("GBA_MMPX_LIB", "gba_mmpx_scale2x", [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p])
    src = to_argb32(rgba)
    for _ in range(2):
        h, w = src.shape
        dst = np.empty((h * 2, w * 2), np.uint32)
        fn(src.ctypes.data, w, h, dst.ctypes.data)
        src = dst
    return from_argb32(src)


def _xbrz_raw(rgba):
    fn = _lib("GBA_XBRZ_LIB", "xbrz_scale_argb", [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int])
    src = to_argb32(rgba)
    h, w = src.shape
    dst = np.empty((h * SCALE, w * SCALE), np.uint32)
    fn(SCALE, src.ctypes.data, dst.ctypes.data, w, h)
    return from_argb32(dst)


def xbrz(rgba):
    """Colour and outline are upscaled separately. The colour pass sees
    transparent pixels filled with their opaque neighbours' colour, so edges
    do not blend toward an unrelated colour; the outline comes from the
    transparency mask upscaled as a black and white image."""
    opaque = rgba[..., 3] > 0
    filled = rgba.copy()
    filled[..., :3] = fill_transparent(rgba)
    filled[..., 3] = 255
    out = _xbrz_raw(filled)
    if opaque.all():
        return out
    mask = np.zeros_like(rgba)
    mask[..., :3] = np.where(opaque, 255, 0)[..., None]
    mask[..., 3] = 255
    out[..., 3] = _xbrz_raw(mask)[..., 0]
    return out


_model = None


def _model_run(rgb):
    import torch
    t = torch.from_numpy(np.ascontiguousarray(rgb)).permute(2, 0, 1).float().div(255).unsqueeze(0).cuda()
    with torch.no_grad():
        return _model(t).clamp(0, 1).squeeze(0).permute(1, 2, 0).mul(255).round().byte().cpu().numpy()


def ai(rgba):
    """ESRGAN-family models see RGB only: transparent pixels take their
    neighbours' colour, and the transparency mask goes through the model as
    its own grey image, cut at half."""
    global _model
    if _model is None:
        from spandrel import ModelLoader
        path = os.environ.get("GBA_UPSCALE_MODEL")
        if not path:
            raise RuntimeError("set GBA_UPSCALE_MODEL to the model file")
        _model = ModelLoader().load_from_file(path).cuda().eval()
    out = np.empty((rgba.shape[0] * SCALE, rgba.shape[1] * SCALE, 4), np.uint8)
    out[..., :3] = _model_run(fill_transparent(rgba))
    opaque = rgba[..., 3] > 0
    if opaque.all():
        out[..., 3] = 255
    else:
        mask = np.repeat(opaque[..., None].astype(np.uint8) * 255, 3, axis=2)
        out[..., 3] = np.where(_model_run(mask)[..., 0] >= 128, 255, 0)
    return out


METHODS = {"nearest": nearest, "mmpx": mmpx, "xbrz": xbrz, "ai": ai}


def over(dst, src):
    """Alpha-blends src onto the opaque dst in place."""
    a = src[..., 3:4].astype(np.uint16)
    dst[..., :3] = (src[..., :3] * a + dst[..., :3] * (255 - a) + 127) // 255


def compose(s, upscale, layers=None, objs=None):
    """A snapshot (gbavideo.Snapshot) at SCALE x: each text BG and sprite
    upscaled on its own, then drawn by priority, sprites over BGs of equal
    priority. Colour effects and windows are not applied (previews only)."""
    import gbavideo as gv
    S = SCALE
    canvas = np.zeros((160 * S, 240 * S, 4), np.uint8)
    canvas[:, :] = s.colors()[0]
    layers = gv.text_layers(s) if layers is None else layers
    objs = gv.sprites(s) if objs is None else objs
    cache = {}

    def up(img):
        key = img.tobytes() + repr(img.shape).encode()
        if key not in cache:
            cache[key] = upscale(img)
        return cache[key]

    for prio in (3, 2, 1, 0):
        for layer in sorted([l for l in layers if l.priority == prio], key=lambda l: -l.x):
            bg = layer.x
            hofs, vofs = s.reg(0x10 + 4 * bg) & 0x1FF, s.reg(0x12 + 4 * bg) & 0x1FF
            big = up(layer.rgba)
            lh, lw = big.shape[:2]
            ys = (np.arange(160 * S) + vofs * S) % lh
            xs = (np.arange(240 * S) + hofs * S) % lw
            over(canvas, big[ys][:, xs])
        for o in reversed([o for o in objs if o.priority == prio]):
            img = up(o.rgba)
            h, w = img.shape[:2]
            x, y = o.x * S, o.y * S
            x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, 240 * S), min(y + h, 160 * S)
            if x0 < x1 and y0 < y1:
                over(canvas[y0:y1, x0:x1], img[y0 - y:y1 - y, x0 - x:x1 - x])
    return canvas
