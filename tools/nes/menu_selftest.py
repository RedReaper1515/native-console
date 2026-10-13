#!/usr/bin/env python3
"""Overlay menu gate: open and close the NES frontend's menu from the
keyboard (Escape, F10) and from a controller alone (Guide, L3), check the
game pauses while it is open, check a setting changed in it survives a
restart, and check a rebinding reaches the preferences file and is put back
as it was found (nes_sdl.cpp, --menu-selftest).

Then the frontend's files: --mute must not reach the saved Mute setting (nor
--fullscreen the saved Fullscreen, which a run before the menu's checks), the
battery save is the cartridge's PRG RAM written to --save-dir and read back
by the next run, screenshots follow the Overscan setting (256x224 with the
hidden lines left out, 256x240 with them), and the CRT filter draws both.

Runs nes_play on ROMs made here (a 16 KiB NROM image that only loops, and
one with a battery that counts its runs), so no fetched data is needed, with
SDL's dummy video and audio drivers, a scratch preferences file and a scratch
save directory: the player's own settings and saves are never touched. Fails
closed.

    python tools/nes/menu_selftest.py --play build/bin/nes_play --out DIR
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

PRG_SIZE = 0x4000  # one 16 KiB bank, at $8000 and again at $C000
CHR_SIZE = 0x2000


def make_rom(code: bytes, battery: bool = False) -> bytes:
    """An iNES NROM image running `code` from $C000; every vector points there."""
    header = bytes([0x4E, 0x45, 0x53, 0x1A, 1, 1, 0x02 if battery else 0x00, 0x00]) + bytes(8)
    prg = bytearray(PRG_SIZE)
    prg[: len(code)] = code
    prg[0x3FFA:0x4000] = bytes([0x00, 0xC0] * 3)  # NMI, reset, IRQ
    return header + bytes(prg) + bytes(CHR_SIZE)


# loop: JMP loop
LOOP_ROM = make_rom(bytes([0x4C, 0x00, 0xC0]))
# LDA $6000 / CLC / ADC #1 / STA $6000 / loop: JMP loop
COUNT_ROM = make_rom(bytes([0xAD, 0x00, 0x60, 0x18, 0x69, 0x01, 0x8D, 0x00, 0x60, 0x4C, 0x09, 0xC0]), battery=True)


def bmp_size(path: Path) -> tuple[int, int]:
    """Width and height of a BMP file, (0, 0) when it is not one."""
    try:
        data = path.read_bytes()
    except OSError:
        return (0, 0)
    if len(data) < 26 or data[:2] != b"BM":
        return (0, 0)
    width, height = struct.unpack_from("<ii", data, 18)
    return (width, abs(height))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--play", type=Path, required=True, help="nes_play executable")
    parser.add_argument("--out", type=Path, required=True, help="scratch directory (emptied first)")
    args = parser.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    rom = args.out / "loop.nes"
    rom.write_bytes(LOOP_ROM)
    counter = args.out / "counter.nes"
    counter.write_bytes(COUNT_ROM)
    prefs_path = args.out / "prefs.ini"
    save_dir = args.out / "save"

    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    files = ["--prefs", str(prefs_path), "--save-dir", str(save_dir)]
    ok = True

    def play(what: str, *arguments: str) -> subprocess.CompletedProcess | None:
        nonlocal ok
        try:
            run = subprocess.run([str(args.play), *arguments, *files], capture_output=True, text=True, env=env,
                                 timeout=120)
        except subprocess.TimeoutExpired:
            print(f"{what}: timeout")
            ok = False
            return None
        if run.stdout.strip():
            print(run.stdout.strip())
        if run.returncode != 0:
            print(f"{what}: exit {run.returncode} {run.stderr.strip()[:300]}")
            ok = False
        return run

    def check(passed: bool, what: str) -> None:
        nonlocal ok
        print(f"frontend: {'ok  ' if passed else 'FAIL'} {what}")
        if not passed:
            ok = False

    # --fullscreen is one run's. On a settings file that has never held a
    # choice it must leave none: the size change that follows making the
    # window fullscreen used to be written as if the player had chosen it.
    if play("--fullscreen", str(rom), "--fullscreen", "--mute", "--limit-frames", "5") is None:
        return 1
    check("display.fullscreen = 1" not in (prefs_path.read_text() if prefs_path.exists() else ""),
          "--fullscreen never reaches the saved Fullscreen setting")

    # The menu, driven by synthetic input; then a restart.
    for phase in (["--menu-selftest", "--mute"], ["--menu-selftest-verify"]):
        run = play(phase[0], str(rom), *phase)
        if run is None:
            return 1
        if "menu selftest: PASS" not in run.stdout:
            print(f"{phase[0]}: no PASS line")
            ok = False
    prefs = prefs_path.read_text() if prefs_path.exists() else ""
    if "audio.muted = 0" not in prefs:
        print("--mute leaked into the saved preferences")
        ok = False
    # The preferences file did not exist before, so "as found" is no binding
    # lines at all: bindings are written only where they differ from defaults.
    if "keyboard." in prefs or "controller." in prefs:
        print("the rebinding test left a binding in the preferences")
        ok = False

    # Battery save: PRG RAM written on exit and loaded by the next run.
    save = save_dir / "counter.sav"
    counts = []
    for attempt in (1, 2):
        if play(f"battery run {attempt}", str(counter), "--limit-frames", "5", "--mute") is None:
            return 1
        data = save.read_bytes() if save.exists() else b""
        check(len(data) == 0x2000, f"run {attempt} wrote the battery save as 8 KiB of PRG RAM ({len(data)} bytes)")
        counts.append(data[0] if data else -1)
    check(counts[0] >= 0 and counts[1] == (counts[0] + 1) & 0xFF,
          f"the second run started from the first run's save (counter {counts[0]} then {counts[1]})")
    check(not (save_dir / "loop.sav").exists(), "a cartridge without a battery writes no save")

    # Screenshots are the picture as shown: without the overscan lines by
    # default, with them once the setting is off.
    shot = args.out / "shot"
    if play("screenshot", str(rom), "--limit-frames", "3", "--mute", "--screenshot-prefix", str(shot)) is None:
        return 1
    check(bmp_size(shot.with_suffix(".bmp")) == (256, 224), "Overscan on (the default): the screenshot is 256x224")
    prefs_path.write_text(prefs_path.read_text().replace("display.overscan = 1", "display.overscan = 0"))
    display = args.out / "display.bmp"
    if play("screenshot with overscan lines", str(rom), "--limit-frames", "3", "--mute", "--screenshot-prefix",
            str(shot), "--display-shot", str(display), "--display-size", "1280x960") is None:
        return 1
    check(bmp_size(shot.with_suffix(".bmp")) == (256, 240), "Overscan off: the screenshot is 256x240")
    check(bmp_size(display) == (1280, 960), "--display-shot writes the display at --display-size")
    # The CRT filter draws both shapes of the picture.
    for overscan in (0, 1):
        text = prefs_path.read_text().replace("display.filter = 0", "display.filter = 2")
        prefs_path.write_text(text.replace(f"display.overscan = {1 - overscan}", f"display.overscan = {overscan}"))
        display.unlink(missing_ok=True)
        run = play("CRT display shot", str(rom), "--limit-frames", "3", "--mute", "--display-shot", str(display),
                   "--display-size", "1280x960")
        if run is None:
            return 1
        check(bmp_size(display) == (1280, 960) and "CRT)" in run.stdout,
              f"the CRT filter draws the display with Overscan {'on' if overscan else 'off'}")
    check("audio.muted = 0" in prefs_path.read_text(), "--mute never reaches the saved Mute setting")

    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
