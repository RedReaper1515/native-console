#!/usr/bin/env python3
"""Behavior tests for tools/gb_discover.py against synthetic savestates and a fake game."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fake_executable import write_fake_executable  # noqa: E402

SPEC = importlib.util.spec_from_file_location("gb_discover", ROOT / "tools" / "gb_discover.py")
assert SPEC and SPEC.loader
GD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GD
SPEC.loader.exec_module(GD)

PPU_SIZE = 12
CORE_SIZE = 20


def make_state(eram: int = 0x2000) -> bytes:
    header = GD.HEADER.pack(GD.SAVESTATE_MAGIC, 9, 0x1234, 0x8000, eram, GD.WRAM_SIZE,
                            GD.VRAM_SIZE, GD.OAM_SIZE, GD.HRAM_SIZE, GD.IO_SIZE, PPU_SIZE, 0)
    return (header + bytes(CORE_SIZE) + bytes(eram) + bytes(GD.WRAM_SIZE) + bytes(GD.VRAM_SIZE)
            + bytes(GD.OAM_SIZE) + bytes(GD.HRAM_SIZE) + bytes(GD.IO_SIZE) + bytes(PPU_SIZE))


# A stand-in for a generated game. It honours the runtime flags the tool uses:
# the player at FF98/FF99 walks with R/L/U (a wall stops D), a counter at C101
# rises for any button, and an entity whose X/Y live in C200/C210 drifts right
# and down every frame while its state byte C280 is nonzero.
FAKE_GAME = r'''
import sys, pathlib
sys.path.insert(0, {tools!r})
import gb_discover as gd
args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1] if name in args else None
save_dir = opt("--save-dir")
if not save_dir or not pathlib.Path(save_dir).is_dir():
    sys.exit(3)
state = gd.Savestate.load(opt("--load-state-file"))
frames = int(opt("--limit-frames"))
for token in filter(None, (opt("--input") or "").split(",")):
    start, buttons, duration = token.split(":")
    held = max(0, min(int(duration), frames - int(start)))
    for button, addr, sign in (("R", 0xFF98, 1), ("L", 0xFF98, -1), ("U", 0xFF99, -1)):
        if button in buttons:
            state.write(addr, state.read(addr) + sign * held)
    state.write(0xC101, state.read(0xC101) + held)
if state.read(0xC280):
    state.write(0xC200, state.read(0xC200) + frames // 4)
    state.write(0xC210, state.read(0xC210) + frames // 8)
oam = [(state.read(0xFF99), state.read(0xFF98), 0x10, 0)]
if state.read(0xC280):
    oam.append((state.read(0xC210) + 1, state.read(0xC200) + 1, 0x20, 0))
for i in range(40):
    y, x, t, f = oam[i] if i < len(oam) else (0, 0, 0, 0)
    for j, v in enumerate((y, x, t, f)):
        state.write(0xFE00 + i * 4 + j, v)
(pathlib.Path(save_dir) / "game.sav").write_bytes(b"written by game")
state.save(opt("--save-state-file"))
shots = opt("--dump-frames")
if shots:
    for n in shots.split(","):
        pathlib.Path(opt("--screenshot-prefix") + "_%05d.ppm" % int(n)).write_bytes(
            b"P6\n2 1\n255\n" + bytes([1, 2, 3, 4, 5, 6]))
'''


class SavestateTests(unittest.TestCase):
    def test_maps_wram_banks_hram_oam_and_io(self) -> None:
        state = GD.Savestate(make_state())
        state.write(0xC123, 0x11)
        state.write(0xD456, 0x22)
        state.write(0xFF90, 0x33)
        self.assertEqual(0x11, state.data[state.wram_off + 0x123])
        self.assertEqual(0x22, state.data[state.wram_off + 0x1000 + 0x456])  # DMG: bank 1
        self.assertEqual(0x33, state.data[state.hram_off + 0x10])
        state.write(0xFF70, 3)
        self.assertEqual(0, state.read(0xD456))  # CGB bank 3 is a different page
        state.write(0xD456, 0x44)
        self.assertEqual(0x44, state.data[state.wram_off + 0x3000 + 0x456])
        with self.assertRaises(GD.SavestateError):
            state.read(0x4000)

    def test_rejects_foreign_or_resized_files(self) -> None:
        with self.assertRaises(GD.SavestateError):
            GD.Savestate(b"\0" * 200)
        bad = bytearray(make_state())
        struct.pack_into("<I", bad, 24, 0x1000)  # wram_size field
        with self.assertRaises(GD.SavestateError):
            GD.Savestate(bad)

    def test_round_trip_through_a_file(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "a.state"
            path.write_bytes(make_state())
            out = GD.poked_state(path, {0xDB5A: 7}, Path(raw) / "b.state")
            self.assertEqual(7, GD.Savestate.load(out).read(0xDB5A))
            self.assertEqual(0, GD.Savestate.load(path).read(0xDB5A))


class TileTests(unittest.TestCase):
    def test_exports_tall_sprite_with_hash_and_rom_location(self) -> None:
        state = GD.Savestate(make_state())
        state.write(0xFF40, 0x04)  # 8x16 sprites
        state.write(0xFF48, 0b11100100)
        top = bytes(range(1, 17))
        bottom = bytes(range(17, 33))
        state.data[state.vram_off + 0x05 * 16:state.vram_off + 0x07 * 16] = top + bottom
        for j, v in enumerate((40, 30, 0x05, 0x20)):  # x-flipped
            state.write(0xFE00 + j, v)
        rom = bytearray(0x10000)
        rom[0x9010:0x9020] = top  # bank 2, CPU address 5010
        with tempfile.TemporaryDirectory() as raw:
            out = Path(raw) / "tiles"
            manifest = GD.export_tiles(state, out, bytes(rom))
            self.assertEqual(1, len(manifest))
            entry = manifest[0]
            self.assertEqual(["05", "06"], entry["tiles"])
            self.assertTrue(entry["x_flip"])
            self.assertEqual(["02:5010"], entry["rom_locations"]["05"])
            self.assertEqual([], entry["rom_locations"]["06"])
            self.assertEqual([hashlib.sha256(top).hexdigest(), hashlib.sha256(bottom).hexdigest()],
                             entry["tile_sha256"])
            png = (out / entry["png"]).read_bytes()
            self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
            self.assertEqual((32, 64), struct.unpack(">II", png[16:24]))
            self.assertEqual(manifest, json.loads((out / "tiles.json").read_text()))

    def test_blank_tiles_have_no_rom_location(self) -> None:
        self.assertEqual([], GD.rom_locations(bytes(0x8000), bytes(16)))

    def test_decode_tile_combines_bitplanes(self) -> None:
        indices = GD.decode_tile(bytes([0b10000001, 0b10000000] + [0] * 14))
        self.assertEqual([3, 0, 0, 0, 0, 0, 0, 1], indices[:8])


class ExperimentTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        # POSIX runs the stand-in through its shebang; Windows uses a .cmd wrapper.
        source = f"#!{sys.executable}\n" + FAKE_GAME.format(tools=str(ROOT / "tools"))
        self.exe = write_fake_executable(base / "game", source)
        state = GD.Savestate(make_state())
        state.write(0xFF98, 80)
        state.write(0xFF99, 90)
        state.write(0xC200, 10)
        state.write(0xC210, 20)
        state.write(0xC280, 5)
        self.load = base / "start.state"
        state.save(self.load)
        self.save = base / "player.sav"
        self.save.write_bytes(b"precious")
        self.runner = GD.Runner(self.exe, base / "work", self.save)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_runs_are_isolated_from_the_players_save(self) -> None:
        result = self.runner.run("iso", 10, "", self.load, shots=(5,))
        self.assertEqual(b"precious", self.save.read_bytes())
        self.assertEqual(b"written by game", (result.directory / "game.sav").read_bytes())
        self.assertTrue(result.screenshot(5).exists())

    def test_input_scan_separates_positions_from_counters(self) -> None:
        found = GD.input_scan(self.runner, self.load, 100, 20, [0, 5, 10], "R", "L")
        self.assertEqual([0xFF98], [a for a, _, _ in found["strict"]])
        blocked = GD.input_scan(self.runner, self.load, 100, 20, [0, 5, 10], "D", "U")
        self.assertEqual([], blocked["strict"])
        self.assertEqual([0xFF99], [a for a, _, _ in blocked["one_sided"]])

    def test_sprite_track_finds_entity_table(self) -> None:
        found = GD.sprite_track(self.runner, self.load, [8, 16, 24, 32], ignore_near=(0xFF98, 0xFF99))
        # The fake game draws the entity at OAM X = C200 + 1, i.e. screen X = C200 - 7.
        self.assertIn((0xC200, 7), found["x"])
        self.assertIn((0xC210, 15), found["y"])

    def test_table_sweep_reports_the_state_byte(self) -> None:
        rows = GD.table_sweep(self.runner, self.load, 0xC200, 0xC300, 0x10, 0, 8,
                              ignore_near=(0xFF98, 0xFF99))
        effects = {addr: effect for addr, _, effect in rows}
        self.assertEqual("sprites gone", effects[0xC280])
        self.assertEqual("sprites changed", effects[0xC200])

    def test_watch_and_cli_poke(self) -> None:
        rows = GD.watch(self.runner, self.load, [30, 40], "20:R:100", [0xFF98])
        self.assertEqual([(30, [90]), (40, [100])], rows)
        code = GD.main(["poke", "--exe", str(self.exe), "--work", str(self.runner.work),
                        "--load", str(self.load), "--set", "FF98=3", "--frames", "4",
                        "--watch", "FF98", "--shots", "2"])
        self.assertEqual(0, code)
        self.assertTrue((self.runner.work / "poke" / "poked_00002.png").exists())


if __name__ == "__main__":
    unittest.main()
