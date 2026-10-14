#!/usr/bin/env python3
"""Turn Link's Awakening overworld rooms into MagicaVoxel (.vox) dioramas.

Each input is a runtime savestate taken while a room is on screen. The room's
10x8 grid of object IDs (WRAM D711) says what every 16x16 cell is; an object
table (la_voxel_objects.json) gives each object ID a shape class; the pixels
come from the BG tiles and OAM sprites in VRAM, so the art is exactly what the
game drew.

    python tools/la_voxel.py capture --exe game.exe --save game.sav --rooms 92,93,A2 --out states
    python tools/la_voxel.py room --state room_92.state --out room_92.vox
    python tools/la_voxel.py scene --state a.state --state b.state --out village.vox
    python tools/la_voxel.py objects --state a.state --out objects.png
    python tools/la_voxel.py profile --out links_awakening.voxel

`profile` writes the same object table as a runtime voxel profile, which a
generated executable loads with --voxel-profile to draw the game in 3D
(see docs/VOXEL_VIEW.md).

Shape classes:
  ground   flat, GROUND voxels thick
  relief   ground whose darker pixels rise `raise` voxels (grass tufts, flowers)
  sunken   one voxel thick (water, holes)
  dome     rises to `height`, rounded over `radius` pixels from the class edge
  box      solid block `height` voxels tall
  cutout   (box, dome) lightest pixels touching the cell edge stay ground
  prop     each object in the art becomes a box: art rows below `top_rows` wrap
           its sides, rows above are stretched over its top (or over a slab
           `canopy` voxels thick); `span` cells per side ([class, x, y] objects)
  facade   the bottom `rows` cell rows of each block stand up as a front wall;
           the cells above become the roof, slid forward onto the wall's top
Sprites stand up as one-voxel-thick cutouts on the ground.

Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from collections import Counter, deque
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gb_discover import Runner, Savestate, decode_tile, write_png  # noqa: E402

ROOM_OBJECTS = 0xD711  # row r, column c at ROOM_OBJECTS + r * 16 + c
HMAP_ROOM = 0xFFF6
HPLAYER_X, HPLAYER_Y = 0xFF98, 0xFF99
COLS, ROWS, CELL = 10, 8, 16
WIDTH, HEIGHT = COLS * CELL, ROWS * CELL  # the playfield above the HUD
DEPTH = 64  # every model shares one Z size so scene models line up
GROUND = 3

PALETTES = {
    "dmg": (0xE0F8D0, 0x88C070, 0x346856, 0x081820),
    "gray": (0xF0F0F0, 0xA8A8A8, 0x585858, 0x181818),
    "pocket": (0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F),
}
SAVE_SPAWN_ROOM = 0x466  # battery save offset of file 1's spawn room (WRAM DB61)
CONTINUE_INPUT = "300:S:5,500:S:5,700:A:5"  # title -> file select -> continue file 1
DEFAULT_OBJECTS = Path(__file__).resolve().parent / "la_voxel_objects.json"


# ---------------------------------------------------------------------------
# Reading a room out of a savestate


class Room:
    def __init__(self, state: Savestate, palette: tuple[int, ...]):
        self.state = state
        self.id = state.read(HMAP_ROOM)
        self.objects = [[state.read(ROOM_OBJECTS + r * 16 + c) for c in range(COLS)]
                        for r in range(ROWS)]
        self.palette = palette
        self.shades = self._render_bg()  # shade 0-3 per playfield pixel

    def _tile_pixels(self, index: int, cache: dict[int, list[int]]) -> list[int]:
        if index not in cache:
            lcdc = self.state.io(0xFF40)
            if lcdc & 0x10:
                addr = 0x8000 + index * 16
            else:
                addr = 0x9000 + (index - 256 if index >= 128 else index) * 16
            cache[index] = decode_tile(bytes(self.state.read(addr + i) for i in range(16)))
        return cache[index]

    def _render_bg(self) -> list[list[int]]:
        st = self.state
        lcdc, scx, scy, bgp = st.io(0xFF40), st.io(0xFF43), st.io(0xFF42), st.io(0xFF47)
        tilemap = 0x9C00 if lcdc & 0x08 else 0x9800
        cache: dict[int, list[int]] = {}
        out = []
        for y in range(HEIGHT):
            row = []
            ty = (y + scy) & 0xFF
            for x in range(WIDTH):
                tx = (x + scx) & 0xFF
                tile = st.read(tilemap + (ty >> 3) * 32 + (tx >> 3))
                color = self._tile_pixels(tile, cache)[(ty & 7) * 8 + (tx & 7)]
                row.append((bgp >> (color * 2)) & 3)
            out.append(row)
        return out

    def is_player(self, sprite) -> bool:
        """Link's two halves sit at (X - 8, Y - 16) and his shadow 8 rows lower."""
        px, py = self.state.read(HPLAYER_X), self.state.read(HPLAYER_Y)
        return px - 9 <= sprite.x <= px and py - 16 <= sprite.y <= py - 8

    def sprites(self, player: bool = True):
        """(x, y, shade rows) for each on-screen OAM sprite, color 0 dropped."""
        st = self.state
        tall = st.tall_sprites
        for sprite in st.sprites():
            if not player and self.is_player(sprite):
                continue
            height = 16 if tall else 8
            pixels = decode_tile(st.tile_data(sprite.tile))
            if tall:
                pixels += decode_tile(st.tile_data((sprite.tile + 1) & 0xFF))
            obp = st.io(0xFF49 if sprite.dmg_palette else 0xFF48)
            rows = []
            for py in range(height):
                sy = height - 1 - py if sprite.y_flip else py
                row = []
                for px in range(8):
                    sx = 7 - px if sprite.x_flip else px
                    color = pixels[sy * 8 + sx]
                    row.append(None if color == 0 else (obp >> (color * 2)) & 3)
                rows.append(row)
            yield sprite.x, sprite.y, rows


# ---------------------------------------------------------------------------
# Building voxels


def distance_to_edge(mask: list[list[bool]]) -> list[list[int]]:
    """Pixel steps (8-connected) from each masked pixel to the nearest unmasked one.

    The screen border does not count as an edge, so a forest that continues
    into the next room stays flat-topped at the seam.
    """
    far = WIDTH + HEIGHT
    dist = [[far if mask[y][x] else 0 for x in range(WIDTH)] for y in range(HEIGHT)]
    queue = deque((x, y) for y in range(HEIGHT) for x in range(WIDTH) if not mask[y][x])
    while queue:
        x, y = queue.popleft()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < WIDTH and 0 <= ny < HEIGHT and dist[ny][nx] > dist[y][x] + 1:
                    dist[ny][nx] = dist[y][x] + 1
                    queue.append((nx, ny))
    return dist


def build_room(room: Room, table: dict, player: bool = True) -> tuple[dict[tuple[int, int, int], int], Counter]:
    """Voxels keyed by (x, y, z) in .vox axes (x right, y away from viewer, z up) -> shade."""
    classes = table["classes"]
    unknown: Counter = Counter()
    cell_class = []
    for r in range(ROWS):
        row = []
        for c in range(COLS):
            name = object_class(table, room.objects[r][c])
            if name is None:
                unknown[room.objects[r][c]] += 1
                name = "ground"
            row.append(name)
        cell_class.append(row)

    def cls(x: int, y: int) -> str:
        return cell_class[y // CELL][x // CELL]

    voxels: dict[tuple[int, int, int], int] = {}

    def column(x: int, sy: int, top: int, shade: int) -> None:
        for z in range(min(top, DEPTH)):
            voxels[(x, HEIGHT - 1 - sy, z)] = shade

    cutouts: dict[tuple[int, int], set[tuple[int, int]]] = {}

    def background(r: int, c: int) -> set[tuple[int, int]]:
        """Lightest-shade pixels connected to the cell's edge: ground showing around an object."""
        if (r, c) not in cutouts:
            x0, y0 = c * CELL, r * CELL
            light = {(x, y) for y in range(y0, y0 + CELL) for x in range(x0, x0 + CELL)
                     if room.shades[y][x] == 0}
            queue = deque(p for p in light if p[0] in (x0, x0 + CELL - 1) or p[1] in (y0, y0 + CELL - 1))
            found = set(queue)
            while queue:
                x, y = queue.popleft()
                for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if n in light and n not in found:
                        found.add(n)
                        queue.append(n)
            cutouts[(r, c)] = found
        return cutouts[(r, c)]

    distances = {}
    for name, spec in classes.items():
        if spec["type"] == "dome":
            mask = [[cls(x, y) == name for x in range(WIDTH)] for y in range(HEIGHT)]
            distances[name] = distance_to_edge(mask)

    # Facade blocks are vertical runs of one class within a cell column.
    facade_bottom: dict[tuple[int, int], int] = {}  # cell -> last cell row of its run
    for r in range(ROWS):
        for c in range(COLS):
            if classes.get(cell_class[r][c], {}).get("type") != "facade":
                continue
            bottom = r
            while bottom + 1 < ROWS and cell_class[bottom + 1][c] == cell_class[r][c]:
                bottom += 1
            facade_bottom[(r, c)] = bottom

    for sy in range(HEIGHT):
        for x in range(WIDTH):
            name = cls(x, sy)
            spec = classes.get(name, {"type": "ground"})
            shade = room.shades[sy][x]
            kind = spec["type"]
            if kind == "ground":
                column(x, sy, GROUND, shade)
            elif kind == "relief":
                column(x, sy, GROUND + (spec.get("raise", 1) if shade >= spec.get("min_shade", 2) else 0), shade)
            elif kind == "sunken":
                column(x, sy, 1, shade)
            elif (spec.get("cutout") or kind == "prop") and (x, sy) in background(sy // CELL, x // CELL):
                column(x, sy, GROUND, shade)
            elif kind == "box":
                column(x, sy, GROUND + spec["height"], shade)
            elif kind == "prop":
                # The runtime wraps the art around a box; the preview is a plain block.
                column(x, sy, GROUND + spec.get("span", 1) * CELL - spec.get("top_rows", 0)
                       + spec.get("canopy", 0), shade)
            elif kind == "dome":
                d = min(distances[name][sy][x], spec["radius"]) / spec["radius"]
                height = spec["height"] * (1 - (1 - d) ** 2) ** 0.5
                if shade == 3:
                    height -= spec.get("groove", 0)
                column(x, sy, GROUND + max(1, round(height)), shade)
            elif kind == "facade":
                # Rows scan top to bottom, so the wall written last wins over the roof.
                r, c = sy // CELL, x // CELL
                bottom_row = facade_bottom[(r, c)]
                first = r
                while first > 0 and cell_class[first - 1][c] == name:
                    first -= 1
                wall_rows = min(spec.get("rows", 1), bottom_row - first + 1)
                wall_top_row = bottom_row - wall_rows + 1
                wall_px = wall_rows * CELL
                block_end = (bottom_row + 1) * CELL  # screen y just below the block
                if sy < first * CELL + wall_px:
                    column(x, sy, GROUND, 0)  # footprint the slid roof uncovers
                if r >= wall_top_row:
                    z = GROUND + (block_end - 1 - sy)
                    for thick in range(2):
                        voxels[(x, HEIGHT - block_end + thick, min(z, DEPTH - 1))] = shade
                    for z in range(GROUND):
                        voxels[(x, HEIGHT - block_end, z)] = shade
                else:
                    column(x, sy + wall_px, GROUND + wall_px, shade)
            else:
                raise ValueError(f"unknown shape type {kind!r} for class {name!r}")

    for sx, sy, rows in room.sprites(player):
        bottom = sy + len(rows) - 1
        if bottom >= HEIGHT or sx + 8 <= 0 or sx >= WIDTH:
            continue
        plane = HEIGHT - 1 - max(0, bottom)
        for py, row in enumerate(rows):
            for px, shade in enumerate(row):
                x = sx + px
                if shade is None or not 0 <= x < WIDTH:
                    continue
                z = GROUND + (len(rows) - 1 - py)
                if z < DEPTH:
                    voxels[(x, plane, z)] = shade
    return voxels, unknown


# ---------------------------------------------------------------------------
# .vox writing


def chunk(kind: bytes, content: bytes = b"", children: bytes = b"") -> bytes:
    return kind + struct.pack("<ii", len(content), len(children)) + content + children


def vox_string(text: str) -> bytes:
    raw = text.encode()
    return struct.pack("<i", len(raw)) + raw


def vox_dict(items: dict[str, str]) -> bytes:
    return struct.pack("<i", len(items)) + b"".join(vox_string(k) + vox_string(v) for k, v in items.items())


def write_vox(path: Path, models: list[tuple[tuple[int, int], dict]], palette: tuple[int, ...]) -> int:
    """models: ((offset x, offset y) in voxels, voxels). Returns the voxel count."""
    body = b""
    total = 0
    for _, voxels in models:
        body += chunk(b"SIZE", struct.pack("<iii", WIDTH, HEIGHT, DEPTH))
        data = bytearray(struct.pack("<i", len(voxels)))
        for (x, y, z), shade in voxels.items():
            data += bytes((x, y, z, shade + 1))
        body += chunk(b"XYZI", bytes(data))
        total += len(voxels)

    # Scene graph: root transform -> group -> (transform -> shape) per model.
    count = len(models)
    body += chunk(b"nTRN", struct.pack("<i", 0) + vox_dict({}) + struct.pack("<iiii", 1, -1, -1, 1) + vox_dict({}))
    body += chunk(b"nGRP", struct.pack("<i", 1) + vox_dict({}) + struct.pack("<i", count)
                  + b"".join(struct.pack("<i", 2 + 2 * i) for i in range(count)))
    for i, ((ox, oy), _) in enumerate(models):
        translate = f"{ox} {oy} {DEPTH // 2}"
        body += chunk(b"nTRN", struct.pack("<i", 2 + 2 * i) + vox_dict({})
                      + struct.pack("<iiii", 3 + 2 * i, -1, 0, 1) + vox_dict({"_t": translate}))
        body += chunk(b"nSHP", struct.pack("<i", 3 + 2 * i) + vox_dict({})
                      + struct.pack("<i", 1) + struct.pack("<i", i) + vox_dict({}))

    rgba = bytearray()
    for i in range(256):
        rgb = palette[i] if i < len(palette) else 0
        rgba += bytes(((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255))
    body += chunk(b"RGBA", bytes(rgba))

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"VOX " + struct.pack("<i", 150) + chunk(b"MAIN", b"", body))
    return total


def write_preview(path: Path, models: list[tuple[tuple[int, int], dict]], palette: tuple[int, ...],
                  scale: int = 2) -> None:
    """Oblique view from the front and above: tops in palette color, fronts darker."""
    placed = []
    for (ox, oy), voxels in models:
        placed += [(x + ox, y + oy, z, shade) for (x, y, z), shade in voxels.items()]
    min_x = min(v[0] for v in placed)
    min_y = min(v[1] for v in placed)
    width = (max(v[0] for v in placed) - min_x + 1) * scale
    depth = max(v[1] for v in placed) - min_y + 1
    height = (depth + DEPTH + 1) * scale
    image = bytearray(b"\x20\x20\x28\xff" * width * height)

    def color(shade: int, factor: float) -> bytes:
        rgb = palette[shade]
        return bytes((int(((rgb >> 16) & 255) * factor), int(((rgb >> 8) & 255) * factor),
                      int((rgb & 255) * factor), 255))

    tops = [[color(s, 0.8 + 0.2 * min(z, 30) / 30) for z in range(DEPTH)] for s in range(4)]
    fronts = [color(s, 0.6) for s in range(4)]
    placed.sort(key=lambda v: (-v[1], v[2]))
    for x, y, z, shade in placed:
        px = (x - min_x) * scale
        py = (depth - 1 - (y - min_y) + DEPTH - z) * scale
        for face, row0 in ((tops[shade][z], py), (fronts[shade], py + scale)):
            for row in range(row0, min(row0 + scale, height)):
                start = (row * width + px) * 4
                image[start:start + scale * 4] = face * scale
    write_png(path, width, height, bytes(image))


# ---------------------------------------------------------------------------
# Commands


def load_table(path: Path) -> dict:
    table = json.loads(path.read_text())
    table["classes"].setdefault("ground", {"type": "ground"})
    return table


def load_rooms(paths: list[Path], palette: tuple[int, ...]) -> list[Room]:
    return [Room(Savestate.load(p), palette) for p in paths]


def report_unknown(room: Room, unknown: Counter) -> None:
    if unknown:
        ids = ", ".join(f"{k:02X}x{v}" for k, v in sorted(unknown.items()))
        print(f"  room {room.id:02X}: unclassified objects drawn as ground: {ids}")


def cmd_capture(args: argparse.Namespace) -> None:
    """Rewrite the save's spawn room, continue file 1, and keep the savestate."""
    save = args.save.read_bytes()
    args.out.mkdir(parents=True, exist_ok=True)
    for text in args.rooms.split(","):
        room = int(text, 16)
        name = f"room_{room:02X}"
        patched = args.out / "saves" / name / args.save.name
        patched.parent.mkdir(parents=True, exist_ok=True)
        data = bytearray(save)
        data[SAVE_SPAWN_ROOM] = room
        patched.write_bytes(data)
        run = Runner(args.exe, args.out / "runs", save_file=patched).run(name, args.frames, CONTINUE_INPUT)
        state = run.state
        if state.read(HMAP_ROOM) != room:
            raise SystemExit(f"{name}: the game is in room {state.read(HMAP_ROOM):02X}; "
                             "is file 1 an overworld save?")
        out = args.out / f"{name}.state"
        out.write_bytes(run.state_path.read_bytes())
        print(f"{name} -> {out}")


def cmd_room(args: argparse.Namespace) -> None:
    table = load_table(args.objects)
    palette = PALETTES[args.palette]
    for path in args.state:
        room = Room(Savestate.load(path), palette)
        voxels, unknown = build_room(room, table)
        out = args.out if len(args.state) == 1 else args.out / f"room_{room.id:02X}.vox"
        count = write_vox(out, [((0, 0), voxels)], palette)
        if args.preview:
            write_preview(out.with_suffix(".png"), [((0, 0), voxels)], palette)
        print(f"room {room.id:02X}: {count} voxels -> {out}")
        report_unknown(room, unknown)


def cmd_scene(args: argparse.Namespace) -> None:
    """Lay rooms out by their overworld position (room ID = row nibble, column nibble)."""
    table = load_table(args.objects)
    palette = PALETTES[args.palette]
    rooms = load_rooms(args.state, palette)
    min_col = min(r.id & 15 for r in rooms)
    min_row = min(r.id >> 4 for r in rooms)
    models = []
    for room in rooms:
        voxels, unknown = build_room(room, table, player=room.id == args.link_room)
        col, row = (room.id & 15) - min_col, (room.id >> 4) - min_row
        models.append(((col * WIDTH, -row * HEIGHT), voxels))
        report_unknown(room, unknown)
    count = write_vox(args.out, models, palette)
    if args.preview:
        write_preview(args.out.with_suffix(".png"), models, palette)
    print(f"{len(rooms)} rooms: {count} voxels -> {args.out}")


INDOOR_FLAG = 0xDBA5  # 0 on the overworld, 1 indoors and in dungeons
GAMEPLAY_TYPE = 0xDB95  # 00 intro/title, 02 file select, 0B playing
GAMEPLAY_WORLD = 0x0B
PROFILE_KEYS = ("height", "radius", "groove", "raise", "min_shade", "rows", "cutout", "span", "top_rows", "canopy")


def object_class(table: dict, obj: int) -> str | None:
    """An object's class name. Table values are a class, or [class, x, y] for a
    prop object that is cell (x, y) of its art."""
    value = table["objects"].get(f"{obj:02X}")
    return value[0] if isinstance(value, list) else value


# The addresses above were found on this ROM; other revisions are unverified.
LA_ROMS = {"ed42628d5cd8c73a6f8c6a8965f19674876a4fa04e616834e26b81b35963ad87":
           "Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev 2)"}


def profile_text(table: dict, roms: list[str]) -> str:
    """The runtime voxel profile for Link's Awakening's overworld."""
    lines = [
        "voxel-profile 1",
        "# Generated by tools/la_voxel.py profile from tools/la_voxel_objects.json.",
        "title Link's Awakening overworld",
        *(f"rom {sha}" for sha in roms),
        f"playfield {WIDTH} {HEIGHT}",
        f"grid {ROOM_OBJECTS:04X} {COLS} {ROWS} 16",
        f"room {HMAP_ROOM:04X}",
        f"require {GAMEPLAY_TYPE:04X} {GAMEPLAY_WORLD:02X}",
        f"require {INDOOR_FLAG:04X} 00",
        f"ground {GROUND}",
        "camera 55 2.0",
    ]
    for name, spec in table["classes"].items():
        params = []
        for key in PROFILE_KEYS:
            if key in spec:
                value = spec[key]
                params.append(f"{key}={int(value) if not isinstance(value, bool) else int(value)}")
        lines.append(" ".join(["class", name, spec["type"], *params]))
    for obj, value in sorted(table["objects"].items(), key=lambda item: int(item[0], 16)):
        if isinstance(value, list):
            lines.append(f"object {obj} {value[0]} {value[1]} {value[2]}")
        else:
            lines.append(f"object {obj} {value}")
    return "\n".join(lines) + "\n"


def cmd_profile(args: argparse.Namespace) -> None:
    table = load_table(args.objects)
    roms = list(LA_ROMS)
    for rom in args.rom or []:
        sha = hashlib.sha256(rom.read_bytes()).hexdigest()
        if sha not in roms:
            roms.append(sha)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(profile_text(table, roms), encoding="ascii", newline="\n")
    print(f"{len(table['objects'])} objects, {len(table['classes'])} classes -> {args.out}")


def cmd_objects(args: argparse.Namespace) -> None:
    """A 16x16 sheet of every object ID seen: the cell at row R, column C is object RC."""
    palette = PALETTES[args.palette]
    table = load_table(args.objects)
    art: dict[int, list[list[int]]] = {}
    seen: dict[int, set[int]] = {}
    for room in load_rooms(args.state, palette):
        for r in range(ROWS):
            for c in range(COLS):
                obj = room.objects[r][c]
                seen.setdefault(obj, set()).add(room.id)
                art.setdefault(obj, [row[c * CELL:(c + 1) * CELL] for row in room.shades[r * CELL:(r + 1) * CELL]])
    step = CELL + 2
    size = 16 * step
    pixels = bytearray(b"\x40\x00\x40\xff" * size * size)
    for obj, cell in art.items():
        ox, oy = (obj & 15) * step + 1, (obj >> 4) * step + 1
        for y in range(CELL):
            for x in range(CELL):
                rgb = palette[cell[y][x]]
                i = ((oy + y) * size + ox + x) * 4
                pixels[i:i + 4] = bytes(((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255))
    scale = args.scale
    big = bytearray()
    for y in range(size * scale):
        row = pixels[(y // scale) * size * 4:((y // scale) + 1) * size * 4]
        big += b"".join(row[x * 4:x * 4 + 4] * scale for x in range(size))
    write_png(args.out, size * scale, size * scale, bytes(big))
    for obj in sorted(seen):
        name = object_class(table, obj) or "-"
        print(f"{obj:02X} {name:10} rooms {' '.join(f'{r:02X}' for r in sorted(seen[obj]))}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("capture", help="savestates of overworld rooms, from a save with file 1 outdoors")
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--save", type=Path, required=True, help="battery save; only read")
    p.add_argument("--rooms", required=True, help="hex room IDs, e.g. 92,93,A2")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--frames", type=int, default=1400)
    p.set_defaults(func=cmd_capture)
    p = sub.add_parser("profile", help="runtime voxel profile for --voxel-profile")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--objects", type=Path, default=DEFAULT_OBJECTS)
    p.add_argument("--rom", type=Path, action="append",
                   help="also allow this ROM (its SHA-256 is added); Rev 2 is always listed")
    p.set_defaults(func=cmd_profile)
    for name, func, help_text in (("room", cmd_room, "one .vox per savestate"),
                                  ("scene", cmd_scene, "all rooms in one .vox, placed by room ID"),
                                  ("objects", cmd_objects, "PNG sheet of object IDs for classifying")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--state", type=Path, action="append", required=True)
        p.add_argument("--out", type=Path, required=True)
        p.add_argument("--objects", type=Path, default=DEFAULT_OBJECTS)
        p.add_argument("--palette", choices=sorted(PALETTES), default="dmg")
        if name != "objects":
            p.add_argument("--preview", action="store_true", help="also write an oblique PNG next to the .vox")
        if name == "scene":
            p.add_argument("--link-room", type=lambda t: int(t, 16), default=-1,
                           help="the one room (hex ID) that keeps Link; others drop him")
        if name == "objects":
            p.add_argument("--scale", type=int, default=3)
        p.set_defaults(func=func)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
