#!/usr/bin/env python3
"""Overlay menu gate: open and close the N64 frontend's menu from the
keyboard (Escape, F10) and from a controller alone (Guide, L3), check the
game pauses while it is open, rebind a key and check the bindings are saved
back as they were found, and check a setting changed in it survives a
restart (n64_sdl.cpp, --menu-selftest).

Runs n64_play on the fixture ROM (tests/fixtures/make_n64_rom.py, so no
fetched data is needed) with SDL's dummy video and audio drivers, a scratch
preferences file and a scratch save directory: the player's own settings
and saves are never touched. The first run passes --mute, which must not
reach the saved Mute setting; before it, a run with --fullscreen on the new
settings file must not write Fullscreen as the saved setting. Fails closed.

    python tests/n64/menu_selftest.py --play build/bin/n64_play \
        --rom-maker tests/fixtures/make_n64_rom.py --out DIR
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--play", type=Path, required=True, help="n64_play executable")
    parser.add_argument("--rom-maker", type=Path, required=True, help="tests/fixtures/make_n64_rom.py")
    parser.add_argument("--out", type=Path, required=True, help="scratch directory (emptied first)")
    args = parser.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    rom = args.out / "fixture.z64"
    make = subprocess.run([sys.executable, str(args.rom_maker), "--output", str(rom)], capture_output=True, text=True)
    if make.returncode or not rom.exists():
        print("fixture ROM generation failed: " + make.stderr)
        return 1

    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    save_dir = args.out / "save"
    common = [str(rom), "--prefs", str(args.out / "prefs.ini"), "--save-dir", str(save_dir)]
    ok = True
    # --fullscreen is one run's. On a settings file that has never held a
    # choice it must leave none: the size change that follows making the
    # window fullscreen used to be written as if the player had chosen it.
    try:
        run = subprocess.run([str(args.play), *common, "--fullscreen", "--mute", "--limit-frames", "5"],
                             capture_output=True, text=True, env=env, timeout=240)
    except subprocess.TimeoutExpired:
        print("--fullscreen: timeout")
        return 1
    prefs = (args.out / "prefs.ini").read_text() if (args.out / "prefs.ini").exists() else ""
    if run.returncode != 0:
        print(f"--fullscreen: exit {run.returncode} {run.stderr.strip()[:300]}")
        ok = False
    if "display.fullscreen = 1" in prefs:
        print("--fullscreen leaked into the saved preferences")
        ok = False
    for phase in (["--menu-selftest", "--mute"], ["--menu-selftest-verify"]):
        try:
            run = subprocess.run([str(args.play), *common, *phase], capture_output=True, text=True, env=env,
                                 timeout=240)
        except subprocess.TimeoutExpired:
            print(f"{phase[0]}: timeout")
            return 1
        print(run.stdout.strip())
        if run.returncode != 0 or "menu selftest: PASS" not in run.stdout or "FAIL" in run.stdout:
            print(f"{phase[0]}: exit {run.returncode} {run.stderr.strip()[:300]}")
            ok = False
    prefs = (args.out / "prefs.ini").read_text() if (args.out / "prefs.ini").exists() else ""
    if "audio.muted = 0" not in prefs:
        print("--mute leaked into the saved preferences")
        ok = False
    if "keyboard." in prefs or "controller." in prefs:
        print("the preferences file kept a binding the self-test should have put back:\n" + prefs)
        ok = False
    if not (save_dir / "fixture.sav").exists():
        print("no cartridge save was written to --save-dir")
        ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
