#!/usr/bin/env python3
"""Overlay menu gate: open and close the GBA frontend's menu from the
keyboard (Escape) and from a controller alone (Guide, L3), check the game
pauses while it is open, check a setting changed in it survives a restart,
and check --mute does not reach the saved Mute setting, nor --fullscreen the
saved Fullscreen (gba_sdl.cpp, --menu-selftest).

Runs gba_play on a minimal cartridge written here (a header and a branch to
itself, so no fetched data is needed) with SDL's dummy video and audio
drivers, a scratch preferences file and a scratch save directory: the
player's own settings and saves are never touched. Fails closed.

    python tools/gba/menu_selftest.py --play build/bin/gba_play --out DIR
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def make_rom() -> bytes:
    """A 192-byte header whose entry branches to 0x080000C0, which branches
    to itself. The built-in BIOS starts it; frames keep coming."""
    rom = bytearray(0x100)
    struct.pack_into("<I", rom, 0x00, 0xEA00002E)  # b 0x080000C0
    rom[0xA0:0xAC] = b"MENUSELFTEST"
    rom[0xAC:0xB0] = b"ZMST"
    rom[0xB0:0xB2] = b"01"
    rom[0xB2] = 0x96
    rom[0xBD] = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF  # header checksum
    struct.pack_into("<I", rom, 0xC0, 0xEAFFFFFE)  # b .
    return bytes(rom)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--play", type=Path, required=True, help="gba_play executable")
    parser.add_argument("--out", type=Path, required=True, help="scratch directory (emptied first)")
    args = parser.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    rom = args.out / "menu_selftest.gba"
    rom.write_bytes(make_rom())
    save_dir = args.out / "save"
    save_dir.mkdir()

    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    prefs = args.out / "prefs.ini"
    common = [str(rom), "--prefs", str(prefs), "--save-dir", str(save_dir)]
    ok = True
    # --fullscreen is one run's. On a settings file that has never held a
    # choice it must leave none: the size change that follows making the
    # window fullscreen used to be written as if the player had chosen it.
    try:
        run = subprocess.run([str(args.play), *common, "--fullscreen", "--mute", "--limit-frames", "5"],
                             capture_output=True, text=True, env=env, timeout=120)
    except subprocess.TimeoutExpired:
        print("--fullscreen: timeout")
        return 1
    if run.returncode != 0:
        print(f"--fullscreen: exit {run.returncode} {run.stderr.strip()[:300]}")
        ok = False
    if "display.fullscreen = 1" in (prefs.read_text() if prefs.exists() else ""):
        print("--fullscreen leaked into the saved preferences")
        ok = False
    for phase in (["--menu-selftest", "--mute"], ["--menu-selftest-verify"]):
        try:
            run = subprocess.run([str(args.play), *common, *phase], capture_output=True, text=True, env=env,
                                 timeout=120)
        except subprocess.TimeoutExpired:
            print(f"{phase[0]}: timeout")
            return 1
        print(run.stdout.strip())
        if run.returncode != 0 or "menu selftest: PASS" not in run.stdout:
            print(f"{phase[0]}: exit {run.returncode} {run.stderr.strip()[:300]}")
            ok = False
    # The first run saved its settings (the volume) while --mute was given.
    text = prefs.read_text() if prefs.exists() else ""
    if "audio.muted = 0" not in text:
        print("--mute leaked into the saved preferences")
        ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
