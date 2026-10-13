#!/usr/bin/env python3
"""Build our own GameCube test programs into runnable ELFs.

dolphin-emu/hwtests is pure CPU: it says nothing about the memory map, the
clocks or the interrupts underneath. These programs (tests/gc/programs/) are
the other half of the P2 gate, and unlike hwtests they are ours - MIT, in
this repository, free to read and to change.

    python tools/gc/build_gc_programs.py
    python tools/gc/build_gc_programs.py --program gc_memtest

They print their verdicts in the same format hwtests does, so
tools/gc/run_gc_tests.py runs both against one baseline.

Building them needs devkitPPC, which nothing this project ships depends on.
Without DEVKITPRO set this exits non-zero and says what is missing, rather
than quietly building nothing.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE_DIR = REPO_ROOT / "tests" / "gc" / "programs"
DEFAULT_OUT = REPO_ROOT / "build" / "gc-programs"


def fail(message: str) -> int:
    print(f"build_gc_programs: {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=SOURCE_DIR)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--program", action="append", help="build only this one (repeatable)")
    args = parser.parse_args()

    devkitpro = os.environ.get("DEVKITPRO")
    devkitppc = os.environ.get("DEVKITPPC") or (devkitpro and str(Path(devkitpro, "devkitPPC")))
    if not devkitpro or not devkitppc:
        return fail(
            "DEVKITPRO must be set (and DEVKITPPC, if it is not $DEVKITPRO/devkitPPC). "
            "These are GameCube programs; see https://devkitpro.org/wiki/Getting_Started"
        )

    gcc = Path(devkitppc, "bin", "powerpc-eabi-gcc")
    if os.name == "nt":
        gcc = gcc.with_suffix(".exe")
    if not gcc.exists():
        return fail(f"{gcc} does not exist")

    libogc = Path(devkitpro, "libogc")
    if not (libogc / "include" / "gccore.h").exists():
        return fail(f"libogc is missing from {libogc}")

    sources = sorted(args.source.glob("gc_*.c"))
    if args.program:
        wanted = {name if name.endswith(".c") else name + ".c" for name in args.program}
        sources = [s for s in sources if s.name in wanted]
        if len(sources) != len(wanted):
            missing = wanted - {s.name for s in sources}
            return fail(f"no such program in {args.source}: {', '.join(sorted(missing))}")
    if not sources:
        return fail(f"no programs in {args.source}")

    args.out.mkdir(parents=True, exist_ok=True)
    for source in sources:
        elf = args.out / (source.stem + ".elf")
        command = [
            str(gcc), "-g", "-O2", "-mogc", "-mcpu=750", "-meabi", "-mhard-float",
            "-DGEKKO", "-Wall", "-Wextra",
            f"-I{args.source}", f"-I{libogc / 'include'}",
            str(source),
            f"-L{libogc / 'lib' / 'cube'}", "-logc", "-lm",
            "-o", str(elf),
        ]
        print(" ".join(command), flush=True)
        result = subprocess.run(command)
        if result.returncode != 0:
            return fail(f"{source.name}: the compiler exited with {result.returncode}")
        print(f"  -> {elf} ({elf.stat().st_size / 1e6:.2f} MB)")

    # One of the programs reads a disc, so the image it reads is built here
    # too rather than left for the caller to remember.
    disc = subprocess.run(
        [sys.executable, str(REPO_ROOT / "tools" / "gc" / "make_test_disc.py"),
         "--out", str(args.out / "test.gcm")])
    if disc.returncode != 0:
        return fail("make_test_disc.py failed")

    print(f"build_gc_programs: {len(sources)} program{'' if len(sources) == 1 else 's'} in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
