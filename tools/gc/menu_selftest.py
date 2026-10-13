#!/usr/bin/env python3
"""Overlay menu gate: open and close the GameCube frontend's menu from the
keyboard (Escape) and from a controller alone (Guide, L3), check the game
pauses while it is open, check the window's close button ends the run, and
check a setting changed in the menu survives a restart (gc_sdl.cpp,
--menu-selftest). Before that, a run with --fullscreen on the new settings
file, which must not write Fullscreen as the saved setting. The settings
file starts with lines for a setting and a control this build does not
know, as a game built from a newer runtime would leave them in the file
they share, and they must still be there at the end.

Runs gc_play on `gc_spin`, whose only job is to keep running so there is
something to pause - every other program in tests/gc/programs reports its
verdicts and exits, and a machine whose guest has finished stops advancing
its field counter, which would fail "the game runs again" for a reason that
has nothing to do with the menu.

SDL's dummy video and audio drivers, a scratch preferences file and a scratch
save directory: the player's own settings, saves and memory card are never
touched. Fails closed - a run that says nothing fails.

    python tools/gc/menu_selftest.py --play build/bin/gc_play --out DIR
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GUEST = REPO_ROOT / "build" / "gc-programs" / "gc_spin.elf"

# Lines a game built from a newer runtime might have left in the shared
# settings file: a setting and a control this build has no name for. Each
# must still be there, as written, after this build has saved its own.
OTHER_BUILD_LINES = [
    "display.not_yet_invented = D:\\Some Folder\\a = b.pack",
    "keyboard.not_yet_invented.0 = key:4",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--play", type=Path, required=True, help="gc_play executable")
    parser.add_argument("--out", type=Path, required=True, help="scratch directory (emptied first)")
    parser.add_argument("--guest", type=Path, default=DEFAULT_GUEST,
                        help="the program to run behind the menu (tools/gc/build_gc_programs.py)")
    args = parser.parse_args()

    if not args.guest.exists():
        # Built by tools/gc/build_gc_programs.py, which needs devkitPPC.
        # Saying so and passing is right for a checkout that has not built
        # them; failing would stop anyone without devkitPPC running the suite.
        print(f"menu_selftest: {args.guest} not built; nothing to check")
        return 0

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    # The settings file is shared with games built from other versions of the
    # runtime. It starts with what one of those might have written.
    (args.out / "prefs.ini").write_text("# seeded by menu_selftest.py\n" + "\n".join(OTHER_BUILD_LINES) + "\n")

    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    common = [str(args.guest), "--prefs", str(args.out / "prefs.ini"), "--save-dir", str(args.out / "save")]
    ok = True
    # --fullscreen is one run's. On a settings file that has never held a
    # choice it must leave none: the size change that follows making the
    # window fullscreen used to be written as if the player had chosen it.
    try:
        run = subprocess.run([str(args.play), *common, "--fullscreen", "--mute", "--limit-frames", "5"],
                             capture_output=True, text=True, env=env, timeout=180)
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
                                 timeout=180)
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
    lost = [line for line in OTHER_BUILD_LINES if line not in prefs.splitlines()]
    if lost:
        print("settings of another build were not kept: " + "; ".join(lost))
        ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
