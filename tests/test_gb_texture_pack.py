#!/usr/bin/env python3
"""Behavior tests for tools/gb_texture_pack.py."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("gb_texture_pack", ROOT / "tools" / "gb_texture_pack.py")
assert SPEC and SPEC.loader
TP = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TP
SPEC.loader.exec_module(TP)
GD = TP.gd

ROM = bytes((i * 7) & 0xFF for i in range(0x8000))


def make_state() -> "GD.Savestate":
    header = GD.HEADER.pack(GD.SAVESTATE_MAGIC, 9, 1, len(ROM), 0, GD.WRAM_SIZE, GD.VRAM_SIZE,
                            GD.OAM_SIZE, GD.HRAM_SIZE, GD.IO_SIZE, 0, 0)
    return GD.Savestate(header + bytes(20) + bytes(GD.WRAM_SIZE + GD.VRAM_SIZE + GD.OAM_SIZE
                                                   + GD.HRAM_SIZE + GD.IO_SIZE))


def palette_png(path: Path) -> None:
    """2x1 indexed PNG: opaque red, half-transparent green."""
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 1, 8, 3, 0, 0, 0))
                     + chunk(b"PLTE", bytes((255, 0, 0, 0, 255, 0)))
                     + chunk(b"tRNS", bytes((255, 128)))
                     + chunk(b"IDAT", zlib.compress(b"\x00\x00\x01"))
                     + chunk(b"IEND", b""))


class PngTests(unittest.TestCase):
    def test_round_trips_rgba_with_filters(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "a.png"
            pixels = bytes((x * 13 + y * 7) & 0xFF for y in range(5) for x in range(6 * 4))
            GD.write_png(path, 6, 5, pixels)
            self.assertEqual((6, 5, pixels), TP.read_png(path))

    def test_decodes_indexed_color_with_transparency(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "p.png"
            palette_png(path)
            self.assertEqual((2, 1, bytes((255, 0, 0, 255, 0, 255, 0, 128))), TP.read_png(path))


class PackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.rom = self.dir / "game.gb"
        self.rom.write_bytes(ROM)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def manifest(self, tiles: list[dict], scale: int = 2) -> Path:
        path = self.dir / "pack" / "pack.json"
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps({
            "schema": "gbrecomp.texture-pack", "version": 1,
            "pack_id": "org.example.test", "license": "CC0-1.0",
            "rom": {"sha256": hashlib.sha256(ROM).hexdigest(), "size": len(ROM)},
            "scale": scale, "tiles": tiles,
        }))
        return path

    def image(self, name: str, side: int, value: int) -> str:
        path = self.dir / "pack" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        GD.write_png(path, side, side, bytes((value, 0, 0, 255)) * side * side)
        return name

    def test_build_writes_the_documented_layout_sorted(self) -> None:
        high, low = "f" * 64, "0" * 64
        manifest = self.manifest([
            {"tile_sha256": high, "palette": "any", "image": self.image("a.png", 16, 1)},
            {"tile_sha256": low, "palette": {"dmg": 0xE4}, "image": self.image("b.png", 16, 2)},
            {"tile_sha256": low, "palette": "any", "image": self.image("c.png", 16, 3)},
        ])
        out = self.dir / "pack.gbtp"
        result = TP.build_pack(manifest, out, self.rom)
        data = out.read_bytes()
        self.assertEqual(3, result["entries"])
        self.assertEqual(184 + 3 * 36 + 3 * 16 * 16 * 4, len(data))
        magic, version = struct.unpack_from("<II", data)
        self.assertEqual((0x50544247, 1), (magic, version))
        self.assertEqual(hashlib.sha256(ROM).digest(), data[8:40])
        self.assertEqual((len(ROM), 2, 3, 0), struct.unpack_from("<IIII", data, 40))
        self.assertEqual(b"org.example.test", data[56:120].rstrip(b"\0"))
        keys = [data[184 + i * 36:184 + i * 36 + 36] for i in range(3)]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(bytes.fromhex(low) + bytes((0, 0, 0, 0)), keys[0])       # any sorts first
        self.assertEqual(bytes.fromhex(low) + bytes((1, 0xE4, 0, 0)), keys[1])
        images = 184 + 3 * 36
        self.assertEqual([3, 2, 1], [data[images + i * 1024] for i in range(3)])
        summary = TP.inspect_pack(out)
        self.assertEqual({"dmg": 0xE4}, summary["entries"][1]["palette"])

    def test_build_rejects_bad_input(self) -> None:
        good = {"tile_sha256": "a" * 64, "palette": "any"}
        cases = {
            "expected 16x16": [dict(good, image=self.image("small.png", 8, 1))],
            "inside the pack directory": [dict(good, image="../game.gb")],
            "duplicate": [dict(good, image=self.image("x.png", 16, 1)), dict(good, image="x.png")],
            "palette must be": [dict(good, palette={"cgb": 9}, image=self.image("y.png", 16, 1))],
            "64 lowercase hex": [dict(good, tile_sha256="A" * 64, image=self.image("z.png", 16, 1))],
        }
        for message, tiles in cases.items():
            with self.subTest(message):
                with self.assertRaisesRegex(TP.PackError, message):
                    TP.build_pack(self.manifest(tiles), self.dir / "bad.gbtp")
        with self.assertRaisesRegex(TP.PackError, "does not match"):
            TP.build_pack(self.manifest([dict(good, image=self.image("w.png", 16, 1))]),
                          self.dir / "bad.gbtp", self.dir / "pack" / "w.png")

    def test_template_exports_visible_background_and_sprite_tiles(self) -> None:
        state = make_state()
        tile_bg = bytes(range(1, 17))
        tile_obj = bytes(range(17, 33))
        state.data[state.vram_off + 5 * 16:state.vram_off + 6 * 16] = tile_bg
        state.data[state.vram_off + 9 * 16:state.vram_off + 10 * 16] = tile_obj
        state.data[state.vram_off + 0x1800:state.vram_off + 0x1800 + 32 * 32] = bytes((5,)) * 1024
        for j, value in enumerate((16 + 40, 8 + 40, 9, 0)):
            state.write(0xFE00 + j, value)
        state.write(0xFF40, 0x93)
        state.write(0xFF47, 0xE4)
        state.write(0xFF48, 0xD2)
        state_path = self.dir / "screen.state"
        state.save(state_path)

        out = self.dir / "src"
        result = TP.write_template([state_path], self.rom, out, 3, "org.example.t", "CC0-1.0", True)
        self.assertEqual(2, result["added"])
        manifest = json.loads((out / "pack.json").read_text())
        by_hash = {t["tile_sha256"]: t for t in manifest["tiles"]}
        self.assertEqual({"dmg": 0xE4}, by_hash[hashlib.sha256(tile_bg).hexdigest()]["palette"])
        self.assertEqual({"dmg": 0xD2}, by_hash[hashlib.sha256(tile_obj).hexdigest()]["palette"])
        width, height, _ = TP.read_png(out / by_hash[hashlib.sha256(tile_obj).hexdigest()]["image"])
        self.assertEqual((24, 24), (width, height))

        again = TP.write_template([state_path], self.rom, out, 3, "org.example.t", "CC0-1.0", True)
        self.assertEqual(0, again["added"])
        self.assertEqual(2, again["total"])
        TP.build_pack(out / "pack.json", self.dir / "built.gbtp", self.rom)

    def test_cli_reports_errors_without_a_traceback(self) -> None:
        self.assertEqual(1, TP.main(["build", str(self.dir / "missing.json"), "--out", str(self.dir / "o")]))


if __name__ == "__main__":
    unittest.main()
