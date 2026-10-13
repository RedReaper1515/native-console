#!/usr/bin/env python3
"""The memory card survives the machine being turned off.

`gc_cardtest` checks that a file written through the save library can be read
back, and it checks it after unmounting and remounting, so the bytes have to
have reached the card rather than the library's work area. What it cannot
check from inside is whether the card reached the *disk* - the image the next
run will open. That is this script.

It runs the same program twice against one image. The first run finds an
empty card and says so; the second run must find the file the first one wrote
and every byte of it. Both halves are required: a machine that never writes
the image back passes the first run and fails the second, and a machine that
reports a file it cannot read fails the second differently. A run that prints
neither answer fails too, which is the case that matters most - the two lines
are how the program says which world it is in, and their absence means it did
not get that far.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

FRESH = "there was no file from an earlier run"
INTACT = "the file from an earlier run is intact"


def fail(message: str) -> int:
    print(f"gc_card_restart: {message}", file=sys.stderr)
    return 1


def run(gc_run: Path, program: Path, card: Path) -> tuple[str, int]:
    result = subprocess.run(
        [str(gc_run), str(program), "--frames", "120", "--card", str(card)],
        capture_output=True,
        text=True,
        timeout=300,
    )
    return result.stdout + result.stderr, result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gc-run", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    args = parser.parse_args()

    if not args.program.exists():
        # Built by tools/gc/build_gc_programs.py, which needs devkitPPC. Saying
        # so and passing is right for a checkout that has not built them;
        # failing here would mean nobody without devkitPPC could run the
        # suite at all.
        print(f"gc_card_restart: {args.program} not built; nothing to check")
        return 0

    with tempfile.TemporaryDirectory() as directory:
        card = Path(directory) / "card.raw"

        first, status = run(args.gc_run, args.program, card)
        if status != 0:
            return fail(f"the first run exited {status}\n{first}")
        if FRESH not in first:
            return fail(f"the first run did not report an empty card\n{first}")
        if "failed" in first:
            return fail(f"the first run had failures\n{first}")
        if not card.exists():
            return fail("the first run left no image behind")
        size = card.stat().st_size
        if size != 512 * 1024:
            return fail(f"the image is {size} bytes, wanted 524288")

        second, status = run(args.gc_run, args.program, card)
        if status != 0:
            return fail(f"the second run exited {status}\n{second}")
        if INTACT not in second:
            return fail(f"the second run did not find the first run's file\n{second}")
        if "failed" in second:
            return fail(f"the second run had failures\n{second}")

    print("gc_card_restart: the card survived being put down and picked up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
