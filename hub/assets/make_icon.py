"""Draws the Native Console Hub icon (a generic game controller) and writes
hub.ico (16-256 px) and hub.png (256 px). Requires Pillow.

    python hub/assets/make_icon.py
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [16, 24, 32, 48, 64, 128, 256]

BG = (24, 27, 34, 255)          # dark tile
BG_EDGE = (58, 82, 130, 255)    # hub accent (selection blue)
BODY = (79, 140, 255, 255)      # controller body
BODY_DARK = (52, 96, 190, 255)
FACE = (16, 18, 24, 255)        # d-pad / grips
BTN_A = (61, 220, 151, 255)     # hub "ready" green
BTN_B = (255, 96, 96, 255)
BTN_X = (79, 180, 255, 255)
BTN_Y = (255, 205, 60, 255)


def render(size):
    s = 8  # supersample
    W = size * s
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = W / 256.0  # design units: 256 grid

    # Background tile with rounded corners and a subtle accent border.
    d.rounded_rectangle([0, 0, W - 1, W - 1], radius=int(52 * u), fill=BG_EDGE)
    d.rounded_rectangle([int(8 * u), int(8 * u), W - 1 - int(8 * u), W - 1 - int(8 * u)], radius=int(46 * u), fill=BG)

    # Controller body: a wide rounded bar plus two grips.
    body_top, body_bot = 92 * u, 164 * u
    d.rounded_rectangle([28 * u, body_top, 228 * u, body_bot], radius=int(36 * u), fill=BODY)
    d.ellipse([28 * u, 108 * u, 108 * u, 196 * u], fill=BODY)   # left grip
    d.ellipse([148 * u, 108 * u, 228 * u, 196 * u], fill=BODY)  # right grip
    # Shading line along the bottom edge of the body.
    d.rounded_rectangle([44 * u, 150 * u, 212 * u, 166 * u], radius=int(8 * u), fill=BODY_DARK)

    # D-pad on the left.
    cx, cy, arm, thick = 78 * u, 128 * u, 26 * u, 11 * u
    d.rounded_rectangle([cx - arm, cy - thick, cx + arm, cy + thick], radius=int(4 * u), fill=FACE)
    d.rounded_rectangle([cx - thick, cy - arm, cx + thick, cy + arm], radius=int(4 * u), fill=FACE)

    # Four face buttons on the right in a diamond.
    bx, by, r, off = 178 * u, 128 * u, 10 * u, 22 * u
    for (dx, dy, col) in ((0, -off, BTN_Y), (-off, 0, BTN_X), (off, 0, BTN_B), (0, off, BTN_A)):
        d.ellipse([bx + dx - r, by + dy - r, bx + dx + r, by + dy + r], fill=col)

    # Start / select pills in the middle.
    for x0 in (114 * u, 130 * u):
        d.rounded_rectangle([x0, 122 * u, x0 + 12 * u, 134 * u], radius=int(6 * u), fill=FACE)

    return img.resize((size, size), Image.LANCZOS)


def main():
    frames = {size: render(size) for size in SIZES}
    frames[256].save(os.path.join(HERE, "hub.png"))
    # Pillow writes a multi-size ICO from the largest frame; pass the exact sizes.
    frames[256].save(os.path.join(HERE, "hub.ico"), format="ICO",
                     sizes=[(sz, sz) for sz in SIZES],
                     append_images=[frames[sz] for sz in SIZES if sz != 256])
    print("wrote hub.ico and hub.png")


if __name__ == "__main__":
    main()
