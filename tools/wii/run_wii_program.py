#!/usr/bin/env python3
"""Run one Wii test program on gc_run and judge its verdicts.

The programs in tests/wii/programs print a line per test in the format
tests/gc/programs/gc_test_report.h defines:

    Test 3 passed (128 subtests)
    Test 4 failed (96 subtests, 2 failures)

This runs one of them and exits zero only if it printed exactly the number
of verdicts expected and every one says "passed". Anything else is a
failure: a test that failed, a program that stopped early, one that printed
nothing at all, or one that was never built.

    python tools/wii/run_wii_program.py --gc-run build/bin/gc_run.exe \\
        --program build/wii-programs/wii_padtest.elf --tests 5 \\
        -- --wii --pad-test --frames 1200

Everything after the bare "--" is passed to gc_run.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

VERDICT = re.compile(r"^Test (\d+) (passed|failed) \((.*)\)\s*$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gc-run", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--tests", type=int, required=True, help="how many verdicts the program prints")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("runner_args", nargs="*", help="passed to gc_run (after --)")
    args = parser.parse_args()

    if not args.program.exists():
        print(f"run_wii_program: {args.program} has not been built (tools/wii/build_wii_programs.py)")
        return 1

    command = [str(args.gc_run), str(args.program), *args.runner_args]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print(f"run_wii_program: no result after {args.timeout} s: {' '.join(command)}")
        return 1

    output = result.stdout + result.stderr
    passed = failed = 0
    for text in output.splitlines():
        match = VERDICT.match(text.strip())
        if not match:
            continue
        if match.group(2) == "passed":
            passed += 1
        else:
            failed += 1

    print(output)
    if result.returncode != 0:
        print(f"run_wii_program: gc_run exited with {result.returncode}")
        return 1
    if failed or passed != args.tests:
        print(f"run_wii_program: {passed} passed and {failed} failed; {args.tests} were expected to pass")
        return 1
    print(f"run_wii_program: all {passed} tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
