#!/usr/bin/env python3
"""The same program, built for each console, draws the same picture.

A Wii's graphics chip contains the GameCube's unchanged, and this backend
relies on that: one command processor, one rasteriser, one OpenGL path. The
check that it holds is to build the GameCube drawing programs
(tests/gc/programs) for both consoles and compare the frame each leaves on
screen, byte for byte. On the Wii machine the same commands go through MEM2's
bounds, the 729 MHz clocks and libogc's Wii start-up (IOS and all) to get
there, so a difference names something the Wii's half got wrong.

    python tools/wii/compare_consoles.py --gc-run build/bin/gc_run

Needs both program sets (tools/gc/build_gc_programs.py and
tools/wii/build_wii_programs.py). With neither built it says so and exits
zero, as the other program tests do; with one missing it fails.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
# The programs that leave a picture worth comparing.
PROGRAMS = ["gc_drawtest", "gc_tevtest", "gc_textest", "gc_fogtest", "gc_blendtest"]


def frame_hash(gc_run: Path, program: Path, out: Path, extra: list[str], frames: int) -> str | None:
    out.unlink(missing_ok=True)
    command = [str(gc_run), str(program), "--frames", str(frames), "--quiet",
               "--dump-frame", str(out)] + extra
    try:
        subprocess.run(command, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return None
    if not out.exists() or out.stat().st_size == 0:
        return None
    return hashlib.sha256(out.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gc-run", type=Path, default=REPO_ROOT / "build" / "bin" / "gc_run")
    parser.add_argument("--gamecube", type=Path, default=REPO_ROOT / "build" / "gc-programs")
    parser.add_argument("--wii", type=Path, default=REPO_ROOT / "build" / "wii-programs")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--gl", action="store_true", help="draw both with the OpenGL path")
    args = parser.parse_args()

    gc_run = args.gc_run
    if not gc_run.exists() and gc_run.with_suffix(".exe").exists():
        gc_run = gc_run.with_suffix(".exe")
    if not gc_run.exists():
        print(f"compare_consoles: {gc_run} does not exist; build the gc_run target", file=sys.stderr)
        return 1

    have_gc = any((args.gamecube / f"{name}.elf").exists() for name in PROGRAMS)
    have_wii = any((args.wii / f"{name}.elf").exists() for name in PROGRAMS)
    if not have_gc and not have_wii:
        print("compare_consoles: no programs built for either console (they need devkitPPC); "
              "nothing compared")
        return 0

    extra = ["--gl"] if args.gl else []
    failed = 0
    with tempfile.TemporaryDirectory(prefix="ncwii") as scratch:
        for name in PROGRAMS:
            gamecube = args.gamecube / f"{name}.elf"
            wii = args.wii / f"{name}.elf"
            if not gamecube.exists() or not wii.exists():
                print(f"{name:<16} MISSING for {'the GameCube' if not gamecube.exists() else 'the Wii'}")
                failed += 1
                continue
            a = frame_hash(gc_run, gamecube, Path(scratch) / f"{name}_gc.ppm", extra, args.frames)
            b = frame_hash(gc_run, wii, Path(scratch) / f"{name}_wii.ppm", extra + ["--wii"], args.frames)
            if a is None or b is None:
                print(f"{name:<16} NO FRAME from {'the GameCube' if a is None else 'the Wii'} run")
                failed += 1
            elif a != b:
                print(f"{name:<16} DIFFERENT: GameCube {a[:16]}, Wii {b[:16]}")
                failed += 1
            else:
                print(f"{name:<16} the same frame on both ({a[:16]})")
    if failed:
        print(f"\ncompare_consoles: {failed} of {len(PROGRAMS)} programs do not match")
        return 1
    print(f"\ncompare_consoles: {len(PROGRAMS)} programs draw the same frame on both consoles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
