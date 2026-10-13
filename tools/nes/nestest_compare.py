#!/usr/bin/env python3
"""Compare a nes_run --cpu-log trace of nestest with the reference log.

kevtris's nestest.nes, started at $C000 ("automation" mode), runs every
official and most unofficial opcodes without needing a PPU; nestest.log is
the trace of a known-good run. This compares, instruction by instruction,
the program counter, A, X, Y, P, the stack pointer and the CPU cycle count.

    nes_run nestest.nes --pc C000 --frames 60 --cpu-log out.log
    python tools/nes/nestest_compare.py out.log roms/nes/nes-test-roms/other/nestest.log

Fails closed: a short trace, a missing file or any differing line is an
error.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REFERENCE = re.compile(
    r"^([0-9A-F]{4}) .*A:([0-9A-F]{2}) X:([0-9A-F]{2}) Y:([0-9A-F]{2}) P:([0-9A-F]{2}) SP:([0-9A-F]{2}).*CYC:(\d+)")
OURS = re.compile(r"^([0-9A-F]{4}) A:([0-9A-F]{2}) X:([0-9A-F]{2}) Y:([0-9A-F]{2}) P:([0-9A-F]{2}) SP:([0-9A-F]{2}) CYC:(\d+)")


def parse(path: Path, pattern: re.Pattern) -> list[tuple]:
    rows = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        match = pattern.match(line)
        if not match:
            raise SystemExit(f"{path}:{number}: cannot parse: {line!r}")
        rows.append(match.groups())
    return rows


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    ours = parse(Path(sys.argv[1]), OURS)
    reference = parse(Path(sys.argv[2]), REFERENCE)
    if not reference:
        print("FAIL: the reference log is empty")
        return 1
    if len(ours) < len(reference):
        print(f"FAIL: the trace has {len(ours)} instructions, the reference {len(reference)}")
        return 1
    names = ("PC", "A", "X", "Y", "P", "SP", "CYC")
    for index, (got, want) in enumerate(zip(ours, reference)):
        if got != want:
            print(f"FAIL: instruction {index + 1} differs")
            print("  got  " + " ".join(f"{n}:{v}" for n, v in zip(names, got)))
            print("  want " + " ".join(f"{n}:{v}" for n, v in zip(names, want)))
            return 1
    print(f"PASS: {len(reference)} instructions match (registers and cycle counts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
