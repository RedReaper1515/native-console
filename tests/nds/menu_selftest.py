#!/usr/bin/env python3
"""Overlay menu gate for the DS frontend (runtimes/runtime-nds/platform/nds_sdl.cpp,
--menu-selftest): the menu opens and closes from the keyboard and from a
controller alone and pauses the game; bindings are changed through it, with
the layout, stylus and lid bindings among them, are saved, and are put back
as they were found; the stylus, microphone and lid reach the game; the save
file is written; and the settings, the save and the console's firmware file
(nickname, language) survive a restart. Before that, a run with --fullscreen
must not write Fullscreen as the saved setting.

Runs nds_play on a ROM made here (two programs that loop, so no fetched data
is needed) with SDL's dummy video and audio drivers, a scratch settings file
and a scratch save directory: the player's own settings and saves are never
touched. The settings file starts with bindings that differ from the
defaults, one of them on a key the test binds to something else, so "put
back as found" is checked against more than an empty file; the save file
starts blank, at a size that says which chip the card has. Fails closed.

    python tests/nds/menu_selftest.py --play build/bin/nds_play --out DIR
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# SDL scancodes and controller buttons the seeded bindings use.
SCANCODE_F9 = 66       # the key the self-test binds to "next layout"
SCANCODE_N = 17
BUTTON_PADDLE1 = 16

SAVE_SIZE = 0x2000         # an 8 KiB chip
FIRMWARE_SIZE = 0x40000    # the DS's firmware flash

# Bindings the player is taken to have made before the test runs. The
# screenshot key is the one the test takes for the layout shortcut.
SEEDED_BINDINGS = [
    f"keyboard.screenshot.1 = key:{SCANCODE_F9}",
    f"keyboard.blow.1 = key:{SCANCODE_N}",
    f"controller.fast_forward.0 = button:{BUTTON_PADDLE1}",
]


def make_rom() -> bytes:
    """A card with a header and two programs that do nothing but loop."""
    rom = bytearray(0x10000)
    rom[0x00:0x0C] = b"MENUSELFTEST"
    rom[0x0C:0x10] = b"####"  # no such game: the save file's size says what the chip is
    struct.pack_into("<4I", rom, 0x20, 0x4000, 0x02000000, 0x02000000, 4)  # ARM9: offset, entry, load, size
    struct.pack_into("<4I", rom, 0x30, 0x8000, 0x02380000, 0x02380000, 4)  # ARM7
    struct.pack_into("<I", rom, 0x60, 0x00586000)  # ROMCTRL for a mask ROM
    struct.pack_into("<I", rom, 0x80, len(rom))
    struct.pack_into("<I", rom, 0x4000, 0xEAFFFFFE)  # b .
    struct.pack_into("<I", rom, 0x8000, 0xEAFFFFFE)
    return bytes(rom)


def binding_lines(text: str) -> list[str]:
    return sorted(line.strip() for line in text.splitlines() if line.startswith(("keyboard.", "controller.")))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--play", type=Path, required=True, help="nds_play executable")
    parser.add_argument("--out", type=Path, required=True, help="scratch directory (emptied first)")
    args = parser.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    rom = args.out / "menu_selftest.nds"
    rom.write_bytes(make_rom())
    prefs = args.out / "prefs.ini"
    prefs.write_text("# seeded by menu_selftest.py\n" + "\n".join(SEEDED_BINDINGS) + "\n")
    # A blank save. Its size tells the machine what chip the card has (the
    # ROM's game code is in no table), so there is a save to write to.
    save = args.out / "save" / "menu_selftest.sav"
    save.parent.mkdir()
    save.write_bytes(b"\xFF" * SAVE_SIZE)
    firmware = args.out / "nds_firmware.bin"  # the frontend keeps it beside the settings

    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    common = [str(rom), "--prefs", str(prefs), "--save-dir", str(args.out / "save")]
    ok = True
    # --fullscreen is one run's. On a settings file that has never held a
    # choice it must leave none: the size change that follows making the
    # window fullscreen used to be written as if the player had chosen it.
    try:
        run = subprocess.run([str(args.play), *common, "--fullscreen", "--mute", "--limit-frames", "5"],
                             capture_output=True, text=True, env=env, timeout=300)
    except subprocess.TimeoutExpired:
        print("--fullscreen: timeout")
        return 1
    if run.returncode != 0:
        print(f"--fullscreen: exit {run.returncode} {run.stderr.strip()[:600]}")
        ok = False
    if "display.fullscreen = 1" in (prefs.read_text() if prefs.exists() else ""):
        print("--fullscreen leaked into the saved settings")
        ok = False
    for phase in (["--menu-selftest", "--mute"], ["--menu-selftest-verify"]):
        try:
            run = subprocess.run([str(args.play), *common, *phase], capture_output=True, text=True, env=env,
                                 timeout=300)
        except subprocess.TimeoutExpired:
            print(f"{phase[0]}: timeout")
            return 1
        print(run.stdout.strip())
        if run.returncode != 0 or "menu selftest: PASS" not in run.stdout or "FAIL" in run.stdout:
            print(f"{phase[0]}: exit {run.returncode} {run.stderr.strip()[:600]}")
            ok = False

    text = prefs.read_text() if prefs.exists() else ""
    if "audio.muted = 0" not in text:
        print("--mute leaked into the saved settings")
        ok = False
    if binding_lines(text) != sorted(SEEDED_BINDINGS):
        print("the bindings were not put back as they were found:")
        print("  found  " + "; ".join(sorted(SEEDED_BINDINGS)))
        print("  left   " + "; ".join(binding_lines(text)))
        ok = False
    saved = save.read_bytes() if save.exists() else b""
    if len(saved) != SAVE_SIZE or saved == b"\xFF" * SAVE_SIZE:
        print(f"{save} was not written with the chip's {SAVE_SIZE} bytes ({len(saved)} bytes, "
              f"{'blank' if saved else 'missing'})")
        ok = False
    if not firmware.exists() or firmware.stat().st_size != FIRMWARE_SIZE:
        print(f"no {FIRMWARE_SIZE}-byte firmware image at {firmware}")
        ok = False
    if any(args.out.glob("**/*.tmp")):
        print("a temporary file was left behind")
        ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
