#!/usr/bin/env python3
"""Overlay menu gate: open and close the SNES frontend's menu from the
keyboard (Escape) and from a controller alone (Guide, L3), check the game
pauses while it is open, and check a setting changed in it survives a
restart (snes_sdl.cpp, --menu-selftest). Before that, a run with
--fullscreen on the new settings file, which must not write Fullscreen as
the saved setting.

Runs snes_play on the checklist ROM (assembled here, so no fetched data is
needed) with SDL's dummy video and audio drivers, a scratch preferences file
and a scratch save directory: the player's own settings and saves are never
touched. Fails closed.

    python tools/snes/menu_selftest.py --play build/bin/snes_play --out DIR
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--play", type=Path, required=True, help="snes_play executable")
    parser.add_argument("--out", type=Path, required=True, help="scratch directory (emptied first)")
    args = parser.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    rom = args.out / "checklist.sfc"
    make = subprocess.run([sys.executable, str(HERE / "checklist" / "make_checklist_rom.py"), "-o", str(rom)],
                          capture_output=True, text=True)
    if make.returncode or not rom.exists():
        print("checklist ROM assembly failed: " + make.stderr)
        return 1

    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    common = [str(rom), "--prefs", str(args.out / "prefs.ini"), "--save-dir", str(args.out / "save")]
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
                                 timeout=120)
        except subprocess.TimeoutExpired:
            print(f"{phase[0]}: timeout")
            return 1
        print(run.stdout.strip())
        if run.returncode != 0 or "menu selftest: PASS" not in run.stdout:
            print(f"{phase[0]}: exit {run.returncode} {run.stderr.strip()[:300]}")
            ok = False
    prefs = (args.out / "prefs.ini").read_text() if (args.out / "prefs.ini").exists() else ""
    if "audio.muted = 0" not in prefs:
        print("--mute leaked into the saved preferences")
        ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
