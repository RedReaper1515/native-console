#!/usr/bin/env python3
"""Run the GBA test ROMs that report through R12 (jsmolka and Alyosha).

These ROMs run their checks, leave the number of the first failing check in
R12 (0 = all passed) and spin on "idle: b idle". Each ROM is selected when
its .asm source uses m_test_eval. Runs are fail-closed: a timeout, a crash,
or an empty selection counts as failure.

    python tools/gba/run_gba_tests.py [--runner build/bin/gba_run.exe]
                                      [--filter SUBSTR] [--frames N] [--jobs N]
                                      [--baseline FILE] [--write-baseline FILE]

With --baseline the run passes when every ROM listed in the file passes (a
regression gate); ROMs that newly pass are reported so the baseline can be
extended with --write-baseline.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DATA = REPO / "roms" / "gba"
SUITES = ("jsmolka", "alyosha")


def discover(data: Path, filter_text: str | None) -> list[Path]:
    roms = []
    for suite in SUITES:
        for asm in sorted((data / suite).rglob("*.asm")):
            if "lib" in asm.parts:
                continue
            rom = asm.with_suffix(".gba")
            if not rom.exists() or "m_test_eval" not in asm.read_text(errors="replace"):
                continue
            rel = rom.relative_to(data).as_posix()
            if filter_text and filter_text not in rel:
                continue
            roms.append(rom)
    return roms


def run_one(runner: Path, data: Path, rom: Path, frames: int) -> tuple[str, bool, str]:
    rel = rom.relative_to(data).as_posix()
    try:
        proc = subprocess.run(
            [str(runner), str(rom), "--until-idle", "--expect", "r12=0", "--frames", str(frames)],
            capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return rel, False, "timeout"
    out = proc.stdout
    if proc.returncode == 0 and "PASS" in out:
        return rel, True, ""
    m = re.search(r"r12=([0-9A-F]{8}), expected", out)
    if m:
        return rel, False, f"r12={int(m.group(1), 16)}"
    if "not idle" in out:
        return rel, False, "never idle"
    return rel, False, f"exit {proc.returncode}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", type=Path, default=REPO / "build" / "bin" / "gba_run.exe")
    parser.add_argument("--data", type=Path, default=DATA)
    parser.add_argument("--filter")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--write-baseline", type=Path)
    args = parser.parse_args()

    if not args.runner.exists():
        print(f"runner not found: {args.runner}")
        return 1
    roms = discover(args.data, args.filter)
    if not roms:
        print("no test ROMs selected (fail closed); run tools/fetch_gba_tests.py")
        return 1

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda r: run_one(args.runner, args.data, r, args.frames), roms))

    passed = 0
    for rel, ok, detail in results:
        passed += ok
        print(f"{'ok  ' if ok else 'FAIL'} {rel}{'  ' + detail if detail else ''}")
    print(f"\n{passed}/{len(results)} passed")

    if args.write_baseline:
        args.write_baseline.write_text("".join(f"{rel}\n" for rel, ok, _ in sorted(results) if ok))
        print(f"baseline written: {args.write_baseline}")

    if args.baseline:
        expected = {line.strip() for line in args.baseline.read_text().splitlines()
                    if line.strip() and not line.startswith("#")}
        if not expected:
            print("empty baseline (fail closed)")
            return 1
        status = {rel: ok for rel, ok, _ in results}
        missing = sorted(expected - status.keys())
        regressed = sorted(rel for rel in expected & status.keys() if not status[rel])
        improved = sorted(rel for rel, ok in status.items() if ok and rel not in expected)
        for rel in missing:
            print(f"MISSING from run: {rel}")
        for rel in regressed:
            print(f"REGRESSED: {rel}")
        for rel in improved:
            print(f"newly passing (extend the baseline): {rel}")
        return 0 if not missing and not regressed else 1

    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
