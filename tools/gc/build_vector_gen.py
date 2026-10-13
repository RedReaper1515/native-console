#!/usr/bin/env python3
"""Build the vector generator into a GameCube ELF.

The generator (tests/gc/vectors/gen/) runs on the guest, so it needs
devkitPPC. Nothing this project ships depends on devkitPPC: the vectors it
produces are checked in, and CI replays those.

    python tools/gc/build_vector_gen.py
    python tools/gc/build_vector_gen.py --out build/gc-vectors/gc_vector_gen.elf

Without DEVKITPRO set this exits non-zero and says so, rather than quietly
building nothing.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE_DIR = REPO_ROOT / "tests" / "gc" / "vectors" / "gen"
DEFAULT_OUT = REPO_ROOT / "build" / "gc-vectors" / "gc_vector_gen.elf"


def fail(message: str) -> int:
    print(f"build_vector_gen: {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--source", type=Path, default=SOURCE_DIR)
    args = parser.parse_args()

    devkitpro = os.environ.get("DEVKITPRO")
    devkitppc = os.environ.get("DEVKITPPC") or (devkitpro and str(Path(devkitpro, "devkitPPC")))
    if not devkitpro or not devkitppc:
        return fail(
            "DEVKITPRO must be set (and DEVKITPPC, if it is not $DEVKITPRO/devkitPPC). "
            "The generator is a GameCube program; see https://devkitpro.org/wiki/Getting_Started"
        )

    gcc = Path(devkitppc, "bin", "powerpc-eabi-gcc")
    if os.name == "nt":
        gcc = gcc.with_suffix(".exe")
    if not gcc.exists():
        return fail(f"{gcc} does not exist")

    libogc = Path(devkitpro, "libogc")
    if not (libogc / "include" / "gccore.h").exists():
        return fail(f"libogc is missing from {libogc}")

    sources = [args.source / "gc_vector_gen.c", args.source / "gc_run_case.S"]
    for source in sources:
        if not source.exists():
            return fail(f"{source} does not exist")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(gcc), "-g", "-O2", "-mogc", "-mcpu=750", "-meabi", "-mhard-float",
        "-Wall", "-Wextra",
        f"-I{args.source}", f"-I{libogc / 'include'}",
        *[str(s) for s in sources],
        f"-L{libogc / 'lib' / 'cube'}", "-logc", "-lm",
        "-o", str(args.out),
    ]
    print(" ".join(command), flush=True)
    result = subprocess.run(command)
    if result.returncode != 0:
        return fail(f"the compiler exited with {result.returncode}")
    print(f"build_vector_gen: {args.out} ({args.out.stat().st_size / 1e6:.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
