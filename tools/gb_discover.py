#!/usr/bin/env python3
"""Find what a Game Boy game's RAM and graphics mean by experimenting on its generated executable.

Every experiment starts from a runtime savestate, runs the generated executable
headless with scripted input, and reads RAM, OAM, and VRAM back out of the
savestate it writes on exit. Guesses are then confirmed by editing the
savestate and watching what the game does. See docs/DISCOVERY.md.

The tool never touches the executable's own save: every run gets a fresh
--save-dir under the work directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

# GBSavestateFileHeader in runtime/src/gbrt.c.
HEADER = struct.Struct("<IIQIIIIIIIII")
SAVESTATE_MAGIC = 0x56534247  # 'GBSV'
WRAM_SIZE = 0x1000 * 8
VRAM_SIZE = 0x2000 * 2
OAM_SIZE = 0xA0
HRAM_SIZE = 0x7F
IO_SIZE = 0x81

DMG_SHADES = (0xE0F8D0, 0x88C070, 0x346856, 0x081820)


def parse_addr(text: str) -> int:
    text = text.strip().lower().removeprefix("0x").removeprefix("$")
    value = int(text, 16)
    if not 0 <= value <= 0xFFFF:
        raise argparse.ArgumentTypeError(f"address out of range: {text}")
    return value


def parse_range(text: str) -> tuple[int, int]:
    """'C200:C600' -> (0xC200, 0xC600), end exclusive."""
    lo, _, hi = text.partition(":")
    if not hi:
        raise argparse.ArgumentTypeError(f"range must be START:END, got {text}")
    start, end = parse_addr(lo), parse_addr(hi)
    if end <= start:
        raise argparse.ArgumentTypeError(f"empty range {text}")
    return start, end


def parse_frames(text: str) -> list[int]:
    """'30' -> [30]; '10,20' -> [10, 20]; '10:120:6' -> range(10, 120, 6)."""
    if ":" in text:
        parts = [int(p) for p in text.split(":")]
        return list(range(*parts))
    return [int(p) for p in text.split(",")]


def parse_set(text: str) -> tuple[int, int]:
    addr, _, value = text.partition("=")
    if not value:
        raise argparse.ArgumentTypeError(f"--set needs ADDR=VALUE, got {text}")
    number = int(value, 0)
    if not 0 <= number <= 0xFF:
        raise argparse.ArgumentTypeError(f"byte value out of range: {value}")
    return parse_addr(addr), number


# ---------------------------------------------------------------------------
# Savestate access


class SavestateError(ValueError):
    pass


@dataclass
class Sprite:
    index: int
    x: int  # screen coordinates (OAM X - 8, OAM Y - 16)
    y: int
    tile: int
    flags: int

    @property
    def x_flip(self) -> bool:
        return bool(self.flags & 0x20)

    @property
    def y_flip(self) -> bool:
        return bool(self.flags & 0x40)

    @property
    def dmg_palette(self) -> int:
        return (self.flags >> 4) & 1

    @property
    def cgb_bank(self) -> int:
        return (self.flags >> 3) & 1


class Savestate:
    """Addressable view of a runtime savestate file.

    Sections sit at the end of the file in the order eram, wram, vram, oam,
    hram, io, ppu, apu, so they are located from the end and the variable-size
    core state in front of them never has to be decoded.
    """

    def __init__(self, data: bytes | bytearray):
        if len(data) < HEADER.size:
            raise SavestateError("file is shorter than a savestate header")
        fields = HEADER.unpack_from(data, 0)
        (magic, self.version, self.rom_hash, self.rom_size, eram, wram, vram,
         oam, hram, io, ppu, apu) = fields
        if magic != SAVESTATE_MAGIC:
            raise SavestateError("not a gbrecomp savestate (bad magic)")
        expected = {"wram": (wram, WRAM_SIZE), "vram": (vram, VRAM_SIZE),
                    "oam": (oam, OAM_SIZE), "hram": (hram, HRAM_SIZE), "io": (io, IO_SIZE)}
        for name, (got, want) in expected.items():
            if got != want:
                raise SavestateError(f"unexpected {name} size {got} (expected {want})")
        tail = wram + vram + oam + hram + io + ppu + apu
        self.wram_off = len(data) - tail
        if self.wram_off < HEADER.size + eram:
            raise SavestateError("savestate sections do not fit in the file")
        self.vram_off = self.wram_off + wram
        self.oam_off = self.vram_off + vram
        self.hram_off = self.oam_off + oam
        self.io_off = self.hram_off + hram
        self.data = bytearray(data)

    @classmethod
    def load(cls, path: Path | str) -> "Savestate":
        return cls(Path(path).read_bytes())

    def save(self, path: Path | str) -> None:
        Path(path).write_bytes(self.data)

    def io(self, addr: int) -> int:
        return self.data[self.io_off + addr - 0xFF00]

    @property
    def wram_bank(self) -> int:
        return (self.io(0xFF70) & 7) or 1

    @property
    def vram_bank(self) -> int:
        return self.io(0xFF4F) & 1

    def _offset(self, addr: int) -> int:
        if 0x8000 <= addr < 0xA000:
            return self.vram_off + self.vram_bank * 0x2000 + addr - 0x8000
        if 0xC000 <= addr < 0xD000:
            return self.wram_off + addr - 0xC000
        if 0xD000 <= addr < 0xE000:
            return self.wram_off + self.wram_bank * 0x1000 + addr - 0xD000
        if 0xFE00 <= addr < 0xFEA0:
            return self.oam_off + addr - 0xFE00
        if 0xFF00 <= addr < 0xFF80:
            return self.io_off + addr - 0xFF00
        if 0xFF80 <= addr < 0xFFFF:
            return self.hram_off + addr - 0xFF80
        raise SavestateError(f"address {addr:04X} is not VRAM, WRAM, OAM, I/O, or HRAM")

    def read(self, addr: int) -> int:
        return self.data[self._offset(addr)]

    def write(self, addr: int, value: int) -> None:
        self.data[self._offset(addr)] = value & 0xFF

    def sprites(self) -> list[Sprite]:
        """OAM entries that are on screen."""
        out = []
        for i in range(40):
            y, x, tile, flags = self.data[self.oam_off + i * 4:self.oam_off + i * 4 + 4]
            if 0 < y < 160 and 0 < x < 168:
                out.append(Sprite(i, x - 8, y - 16, tile, flags))
        return out

    @property
    def tall_sprites(self) -> bool:
        return bool(self.io(0xFF40) & 0x04)

    def tile_data(self, tile: int, bank: int = 0) -> bytes:
        start = self.vram_off + bank * 0x2000 + tile * 16
        return bytes(self.data[start:start + 16])


def search_space(state: Savestate, ranges: list[tuple[int, int]] | None) -> list[int]:
    if not ranges:
        return list(range(0xC000, 0xE000)) + list(range(0xFF80, 0xFFFF))
    return [a for lo, hi in ranges for a in range(lo, hi)]


# ---------------------------------------------------------------------------
# Images (stdlib only)


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = Path(path).read_bytes()
    tokens: list[bytes] = []
    pos = 0
    while len(tokens) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos)
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        tokens.append(data[pos:end])
        pos = end
    if tokens[0] != b"P6":
        raise ValueError(f"{path} is not a binary PPM")
    width, height = int(tokens[1]), int(tokens[2])
    pixels = data[pos + 1:pos + 1 + width * height * 3]
    return width, height, pixels


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    rows = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 9))
        + chunk(b"IEND", b""))


def ppm_to_png(ppm: Path, png: Path, scale: int = 2) -> None:
    width, height, rgb = read_ppm(ppm)
    out = bytearray()
    for y in range(height * scale):
        row = bytearray()
        for x in range(width):
            i = ((y // scale) * width + x) * 3
            row += (rgb[i:i + 3] + b"\xff") * scale
        out += row
    write_png(png, width * scale, height * scale, bytes(out))


def decode_tile(raw: bytes) -> list[int]:
    """16 bytes of 2bpp tile data -> 64 color indices, row by row."""
    out = []
    for row in range(8):
        lo, hi = raw[row * 2], raw[row * 2 + 1]
        for bit in range(7, -1, -1):
            out.append(((hi >> bit) & 1) << 1 | ((lo >> bit) & 1))
    return out


# ---------------------------------------------------------------------------
# Running the generated executable


@dataclass
class RunResult:
    directory: Path
    state_path: Path

    @property
    def state(self) -> Savestate:
        return Savestate.load(self.state_path)

    def screenshot(self, frame: int) -> Path:
        return self.directory / f"shot_{frame:05d}.ppm"


class Runner:
    """Launches a generated executable headless, isolated from the player's save."""

    def __init__(self, exe: Path, work: Path, save_file: Path | None = None, timeout: int = 600):
        self.exe = Path(exe)
        self.work = Path(work)
        self.save_file = Path(save_file) if save_file else None
        self.timeout = timeout
        self.work.mkdir(parents=True, exist_ok=True)

    def run(self, name: str, frames: int, inputs: str = "", load: Path | None = None,
            shots: tuple[int, ...] = ()) -> RunResult:
        directory = self.work / name
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)
        if self.save_file:
            shutil.copy2(self.save_file, directory / self.save_file.name)
        state_path = directory / "end.state"
        command = [str(self.exe), "--headless", "--no-audio",
                   "--save-dir", str(directory),
                   "--limit-frames", str(frames),
                   "--save-state-file", str(state_path)]
        if load:
            command += ["--load-state-file", str(load)]
        if inputs:
            command += ["--input", inputs]
        if shots:
            command += ["--dump-frames", ",".join(map(str, shots)),
                        "--screenshot-prefix", str(directory / "shot")]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        result = subprocess.run(command, cwd=directory, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=self.timeout)
        if result.returncode != 0 or not state_path.exists():
            raise RuntimeError(f"run '{name}' failed (exit {result.returncode}):\n{result.stdout[-2000:]}")
        return RunResult(directory, state_path)


def poked_state(source: Path, writes: dict[int, int], dest: Path) -> Path:
    state = Savestate.load(source)
    for addr, value in writes.items():
        state.write(addr, value)
    dest.parent.mkdir(parents=True, exist_ok=True)
    state.save(dest)
    return dest


# ---------------------------------------------------------------------------
# Experiments


def monotonic(values: list[int], sign: int, strict: bool) -> bool:
    """Strict: moves in direction `sign` on every step. Otherwise: never moves against it."""
    steps = [(b - a) * sign for a, b in zip(values, values[1:])]
    if strict:
        return all(s > 0 for s in steps)
    return all(s >= 0 for s in steps)


def input_scan(runner: Runner, load: Path, frames: int, start: int, steps: list[int],
               positive: str, negative: str, ranges=None) -> dict[str, list[tuple[int, list[int], list[int]]]]:
    """Bytes that rise while `positive` is held longer and fall while `negative` is.

    `strict` matches move on every step in both directions. `one_sided`
    matches move strictly one way and never the wrong way the other, which is
    what a position looks like when a wall stops the player.
    """
    series: dict[str, list[Savestate]] = {}
    for button in (positive, negative):
        series[button] = []
        for held in steps:
            inputs = f"{start}:{button}:{held}" if held else ""
            series[button].append(runner.run(f"scan_{button}{held}", frames, inputs, load).state)
    addrs = search_space(series[positive][0], ranges)
    strict, one_sided = [], []
    for addr in addrs:
        up = [s.read(addr) for s in series[positive]]
        down = [s.read(addr) for s in series[negative]]
        if monotonic(up, 1, True) and monotonic(down, -1, True):
            strict.append((addr, up, down))
        elif (monotonic(up, 1, True) and monotonic(down, -1, False)) or \
             (monotonic(up, 1, False) and monotonic(down, -1, True)):
            one_sided.append((addr, up, down))
    return {"strict": strict, "one_sided": one_sided}


def sprites_of_interest(state: Savestate, region=None, ignore_near=None, radius: int = 24) -> list[Sprite]:
    sprites = state.sprites()
    if region:
        x0, y0, x1, y1 = region
        sprites = [s for s in sprites if x0 <= s.x < x1 and y0 <= s.y < y1]
    if ignore_near:
        # ignore_near names RAM holding a position in raw OAM units (screen + 8, + 16).
        px = state.read(ignore_near[0]) - 8
        py = state.read(ignore_near[1]) - 16
        sprites = [s for s in sprites if abs(s.x - px) > radius or abs(s.y - py) > radius]
    return sprites


def sprite_track(runner: Runner, load: Path, frames: list[int], region=None, ignore_near=None,
                 lag: int = 3, ranges=None) -> dict[str, object]:
    """Bytes that equal a sprite group's top-left screen X or Y plus a constant."""
    samples = []
    for frame in frames:
        state = runner.run("track", frame, "", load).state
        group = sprites_of_interest(state, region, ignore_near)
        if group:
            samples.append((frame, state, min(s.x for s in group), min(s.y for s in group)))
    if not samples:
        return {"samples": [], "x": [], "y": [], "note": "no matching sprites in any frame"}
    # Keep frames showing the same number of sprites as the first one, so a
    # second object walking on screen does not move the group's corner.
    first = len(sprites_of_interest(samples[0][1], region, ignore_near))
    samples = [s for s in samples if len(sprites_of_interest(s[1], region, ignore_near)) == first]
    result: dict[str, object] = {"samples": [(f, x, y) for f, _, x, y in samples]}
    still = []
    for axis, key in ((2, "x"), (3, "y")):
        hits = []
        if len({s[axis] for s in samples}) >= 3:
            for addr in search_space(samples[0][1], ranges):
                diffs = [(s[1].read(addr) - s[axis] + 128) & 0xFF for s in samples]
                if max(diffs) - min(diffs) <= lag:
                    hits.append((addr, min(diffs) - 128))
        else:
            still.append(key)
        result[key] = hits
    if still:
        result["note"] = (f"the sprites barely moved in {' or '.join(still)}; "
                          "choose frames where they travel further")
    return result


def table_sweep(runner: Runner, load: Path, start: int, end: int, step: int, value: int,
                frames: int, region=None, ignore_near=None) -> list[tuple[int, int, str]]:
    """Write `value` to each nonzero byte at start, start+step, ... and classify the effect."""
    base = Savestate.load(load)
    reference = runner.run("sweep_ref", frames, "", load).state
    ref_sprites = [(s.x, s.y, s.tile) for s in sprites_of_interest(reference, region, ignore_near)]
    rows = []
    for addr in range(start, end, step):
        original = base.read(addr)
        if original == value:
            continue
        poked = poked_state(load, {addr: value}, runner.work / "sweep.state")
        after = runner.run("sweep_try", frames, "", poked).state
        got = [(s.x, s.y, s.tile) for s in sprites_of_interest(after, region, ignore_near)]
        if ref_sprites and not got:
            effect = "sprites gone"
        elif got != ref_sprites:
            effect = "sprites changed"
        else:
            effect = ""
        rows.append((addr, original, effect))
    return rows


def watch(runner: Runner, load: Path, frames: list[int], inputs: str, addrs: list[int]) -> list[tuple[int, list[int]]]:
    rows = []
    for frame in frames:
        state = runner.run("watch", frame, inputs, load).state
        rows.append((frame, [state.read(a) for a in addrs]))
    return rows


def rom_locations(rom: bytes, needle: bytes, limit: int = 8) -> list[str]:
    if len(set(needle)) <= 1:
        return []  # blank or solid tiles match everywhere
    found = []
    pos = rom.find(needle)
    while pos != -1 and len(found) < limit:
        bank = pos // 0x4000
        cpu = pos % 0x4000 + (0x4000 if bank else 0)
        found.append(f"{bank:02X}:{cpu:04X}")
        pos = rom.find(needle, pos + 1)
    return found


def export_tiles(state: Savestate, out: Path, rom: bytes | None, region=None,
                 near: tuple[int, int] | None = None, radius: int = 24, scale: int = 4) -> list[dict]:
    """Write each selected sprite's tile as a PNG and describe where its pixels come from."""
    out.mkdir(parents=True, exist_ok=True)
    sprites = state.sprites()
    if region:
        x0, y0, x1, y1 = region
        sprites = [s for s in sprites if x0 <= s.x < x1 and y0 <= s.y < y1]
    if near:
        px, py = state.read(near[0]) - 8, state.read(near[1]) - 16
        sprites = [s for s in sprites if abs(s.x - px) <= radius and abs(s.y - py) <= radius]
    palettes = (state.io(0xFF48), state.io(0xFF49))
    height = 16 if state.tall_sprites else 8
    manifest = []
    for sprite in sprites:
        tiles = [sprite.tile] + ([(sprite.tile + 1) & 0xFF] if height == 16 else [])
        raw = b"".join(state.tile_data(t, sprite.cgb_bank) for t in tiles)
        indices = [i for t in tiles for i in decode_tile(state.tile_data(t, sprite.cgb_bank))]
        palette = palettes[sprite.dmg_palette]
        pixels = bytearray()
        for y in range(height * scale):
            for x in range(8 * scale):
                tx, ty = x // scale, y // scale
                if sprite.x_flip:
                    tx = 7 - tx
                if sprite.y_flip:
                    ty = height - 1 - ty
                color = indices[ty * 8 + tx]
                if color == 0:
                    pixels += b"\x00\x00\x00\x00"
                else:
                    rgb = DMG_SHADES[(palette >> (color * 2)) & 3]
                    pixels += bytes(((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255))
        name = f"oam{sprite.index:02d}_tile{sprite.tile:02X}.png"
        write_png(out / name, 8 * scale, height * scale, bytes(pixels))
        entry = {
            "oam_index": sprite.index, "screen_x": sprite.x, "screen_y": sprite.y,
            "tiles": [f"{t:02X}" for t in tiles], "vram_bank": sprite.cgb_bank,
            "x_flip": sprite.x_flip, "y_flip": sprite.y_flip,
            "dmg_palette": sprite.dmg_palette, "png": name,
            # Texture packs key replacements by these (docs/TEXTURE_PACKS.md).
            "tile_sha256": [hashlib.sha256(state.tile_data(t, sprite.cgb_bank)).hexdigest() for t in tiles],
        }
        if rom is not None:
            entry["rom_locations"] = {f"{t:02X}": rom_locations(rom, state.tile_data(t, sprite.cgb_bank))
                                      for t in tiles}
        manifest.append(entry)
    (out / "tiles.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


# ---------------------------------------------------------------------------
# Command line


def hexdump(state: Savestate, start: int, end: int) -> str:
    lines = []
    for row in range(start - start % 16, end, 16):
        cells = []
        for addr in range(row, row + 16):
            cells.append(f"{state.read(addr):02X}" if start <= addr < end else "  ")
        lines.append(f"{row:04X}  {' '.join(cells)}")
    return "\n".join(lines)


def region_arg(text: str) -> tuple[int, int, int, int]:
    parts = [int(p) for p in text.split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("region is X0,Y0,X1,Y1 in screen pixels")
    return tuple(parts)  # type: ignore[return-value]


def pair_arg(text: str) -> tuple[int, int]:
    parts = [parse_addr(p) for p in text.split(",")]
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("expected XADDR,YADDR")
    return parts[0], parts[1]


def add_runner_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--exe", type=Path, required=True, help="generated game executable")
    parser.add_argument("--work", type=Path, required=True, help="scratch directory for runs")
    parser.add_argument("--save-file", type=Path, help="battery save copied into each run (never modified)")
    parser.add_argument("--load", type=Path, required=True, help="savestate every run starts from")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("run", help="run once and keep the resulting savestate and screenshots")
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--save-file", type=Path)
    p.add_argument("--load", type=Path)
    p.add_argument("--frames", type=int, required=True, help="frames to run (counted from --load)")
    p.add_argument("--input", default="", help="runtime input script, e.g. 20:R:40,80:A:4")
    p.add_argument("--shots", type=parse_frames, default=[], help="frames to screenshot (saved as PNG)")
    p.add_argument("--name", default="run")
    p.add_argument("--out-state", type=Path, help="copy the final savestate here")

    p = sub.add_parser("peek", help="hex dump part of a savestate")
    p.add_argument("--state", type=Path, required=True)
    p.add_argument("--range", type=parse_range, required=True, dest="span")

    p = sub.add_parser("diff", help="bytes that differ between two savestates")
    p.add_argument("before", type=Path)
    p.add_argument("after", type=Path)
    p.add_argument("--range", type=parse_range, action="append", dest="ranges")

    p = sub.add_parser("input-scan", help="bytes that follow how long a button is held (positions)")
    add_runner_args(p)
    p.add_argument("--frames", type=int, default=100)
    p.add_argument("--start", type=int, default=20, help="frame the button goes down")
    p.add_argument("--steps", type=parse_frames, default=[0, 6, 12, 18, 24], help="hold durations")
    p.add_argument("--axis", action="append", required=True, metavar="POS:NEG",
                   help="button pair such as R:L or D:U; repeatable")
    p.add_argument("--range", type=parse_range, action="append", dest="ranges")

    p = sub.add_parser("sprite-track", help="bytes that follow a moving sprite's screen position")
    add_runner_args(p)
    p.add_argument("--frames", type=parse_frames, default=list(range(10, 120, 6)))
    p.add_argument("--region", type=region_arg, help="only sprites in X0,Y0,X1,Y1")
    p.add_argument("--ignore-near", type=pair_arg, metavar="XADDR,YADDR",
                   help="skip sprites near the position stored at these addresses (the player)")
    p.add_argument("--lag", type=int, default=3, help="pixels of OAM lag tolerated")
    p.add_argument("--range", type=parse_range, action="append", dest="ranges")

    p = sub.add_parser("poke", help="edit bytes in the savestate, run, and report what happened")
    add_runner_args(p)
    p.add_argument("--set", type=parse_set, action="append", required=True, metavar="ADDR=VALUE")
    p.add_argument("--frames", type=int, default=60)
    p.add_argument("--input", default="")
    p.add_argument("--watch", type=parse_addr, action="append", default=[])
    p.add_argument("--shots", type=parse_frames, default=[])

    p = sub.add_parser("table-sweep", help="write a value into each table slot and see which one removes a sprite")
    add_runner_args(p)
    p.add_argument("--range", type=parse_range, required=True, dest="span")
    p.add_argument("--step", type=int, default=16, help="table length; 16 tests slot 0 of each table")
    p.add_argument("--value", type=lambda t: int(t, 0), default=0)
    p.add_argument("--frames", type=int, default=30)
    p.add_argument("--region", type=region_arg)
    p.add_argument("--ignore-near", type=pair_arg, metavar="XADDR,YADDR")

    p = sub.add_parser("watch", help="values of addresses over time under a fixed input script")
    add_runner_args(p)
    p.add_argument("--frames", type=parse_frames, required=True)
    p.add_argument("--input", default="")
    p.add_argument("--addr", type=parse_addr, action="append", required=True)

    p = sub.add_parser("tiles", help="export sprite tiles as PNGs with SHA-256 keys and ROM locations")
    p.add_argument("--state", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--rom", type=Path, help="find where each tile's bytes are stored in the ROM")
    p.add_argument("--region", type=region_arg)
    p.add_argument("--near", type=pair_arg, metavar="XADDR,YADDR",
                   help="only sprites near the position stored at these addresses")
    p.add_argument("--radius", type=int, default=24)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    cmd = args.command

    if cmd == "peek":
        print(hexdump(Savestate.load(args.state), *args.span))
        return 0

    if cmd == "diff":
        a, b = Savestate.load(args.before), Savestate.load(args.after)
        for addr in search_space(a, args.ranges):
            if a.read(addr) != b.read(addr):
                print(f"{addr:04X}: {a.read(addr):3d} -> {b.read(addr):3d}")
        return 0

    if cmd == "tiles":
        rom = args.rom.read_bytes() if args.rom else None
        manifest = export_tiles(Savestate.load(args.state), args.out, rom, args.region, args.near, args.radius)
        for entry in manifest:
            where = entry.get("rom_locations", {})
            print(f"OAM {entry['oam_index']:2d} at ({entry['screen_x']},{entry['screen_y']}) "
                  f"tiles {','.join(entry['tiles'])} sha256 {','.join(h[:12] for h in entry['tile_sha256'])} rom {where}")
        print(f"{len(manifest)} sprites written to {args.out}")
        return 0

    runner = Runner(args.exe, args.work, args.save_file)

    if cmd == "run":
        result = runner.run(args.name, args.frames, args.input, args.load, tuple(args.shots))
        for frame in args.shots:
            ppm_to_png(result.screenshot(frame), result.directory / f"shot_{frame:05d}.png")
        if args.out_state:
            shutil.copy2(result.state_path, args.out_state)
        print(f"state: {args.out_state or result.state_path}")
        return 0

    if cmd == "input-scan":
        for axis in args.axis:
            positive, _, negative = axis.partition(":")
            found = input_scan(runner, args.load, args.frames, args.start, args.steps,
                               positive, negative, args.ranges)
            print(f"{positive} raises / {negative} lowers:")
            for addr, up, down in found["strict"]:
                print(f"  {addr:04X}  {positive}:{up}  {negative}:{down}")
            if found["one_sided"]:
                print("  one direction blocked (a wall?):")
                for addr, up, down in found["one_sided"]:
                    print(f"  {addr:04X}  {positive}:{up}  {negative}:{down}")
        return 0

    if cmd == "sprite-track":
        found = sprite_track(runner, args.load, args.frames, args.region, args.ignore_near,
                             args.lag, args.ranges)
        for frame, x, y in found["samples"]:
            print(f"frame {frame:4d}: sprite group at ({x},{y})")
        if found.get("note"):
            print(f"note: {found['note']}")
        for key in ("x", "y"):
            print(f"{key.upper()} candidates:")
            for addr, offset in found.get(key, []):
                print(f"  {addr:04X} (value = screen {key} {offset:+d})")
        return 0

    if cmd == "poke":
        writes = dict(args.set)
        reference = runner.run("poke_ref", args.frames, args.input, args.load, tuple(args.shots))
        poked = poked_state(args.load, writes, args.work / "poke.state")
        result = runner.run("poke", args.frames, args.input, poked, tuple(args.shots))
        for frame in args.shots:
            ppm_to_png(reference.screenshot(frame), result.directory / f"reference_{frame:05d}.png")
            ppm_to_png(result.screenshot(frame), result.directory / f"poked_{frame:05d}.png")
        ref_state, state = reference.state, result.state
        for addr in args.watch:
            print(f"{addr:04X}: reference {ref_state.read(addr)}  poked {state.read(addr)}")
        before = [(s.x, s.y, s.tile) for s in ref_state.sprites()]
        after = [(s.x, s.y, s.tile) for s in state.sprites()]
        print(f"sprites: reference {len(before)}, poked {len(after)}"
              + ("" if before == after else " (positions or tiles differ)"))
        print(f"output: {result.directory}")
        return 0

    if cmd == "table-sweep":
        rows = table_sweep(runner, args.load, *args.span, args.step, args.value, args.frames,
                           args.region, args.ignore_near)
        for addr, original, effect in rows:
            print(f"  {addr:04X} was {original:3d}  {effect}")
        return 0

    if cmd == "watch":
        print("frame  " + "  ".join(f"{a:04X}" for a in args.addr))
        for frame, values in watch(runner, args.load, args.frames, args.input, args.addr):
            print(f"{frame:5d}  " + "  ".join(f"{v:4d}" for v in values))
        return 0

    return 2


if __name__ == "__main__":
    sys.exit(main())
