#!/usr/bin/env python3
"""Run the SNES test ROMs and read each one's verdict.

Every catalogued ROM is run headless by snes_run (the interpreter) or by a
recompiled executable, with the options that make the runner decide its
verdict the way that suite reports it:

  gilyon cputest/spctest  "Success" on screen once the ROM idles
  krom CPU/SPC700 tests   pages of PASS/FAIL: "FAIL" must never appear
  blargg SPC tests        text sent through the APU ports: "Passed", never "Failed"
  absindx SA-1 tests      the ROM's TestFinished byte in WRAM is 1
  krom PPU demos          the frame at a fixed point compared with krom's
                          screenshot (tools/snes/compare_frame.py)

    python tools/snes/run_snes_tests.py [--runner build/bin/snes_run]
                                        [--filter SUBSTR] [--baseline FILE]

With --baseline, every ROM listed there must pass (fail closed: a listed ROM
that is missing, times out or fails is an error); other ROMs are reported.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DATA = REPO / "roms" / "snes"
EXE = ".exe" if os.name == "nt" else ""

# (glob relative to the data directory, runner arguments). Order is the report order.
CATALOGUE: list[tuple[str, list[str]]] = [
    ("gilyon/cputest/cputest-full.sfc", ["--frames", "3000", "--until-idle", "--expect-text", "Success"]),
    ("gilyon/cputest/cputest-basic.sfc", ["--frames", "3000", "--until-idle", "--expect-text", "Success"]),
    ("gilyon/spctest/spctest.sfc", ["--frames", "3000", "--until-idle", "--expect-text", "Success"]),
    ("peterlemon/CPUTest/CPU/*/*.sfc", ["--frames", "600", "--fail-text", "FAIL", "--expect-text", "PASS"]),
    ("peterlemon/CPUTest/SPC700/*/*.sfc", ["--frames", "2400", "--fail-text", "FAIL", "--expect-text", "PASS"]),
    ("higan/blargg-spc-6/spc_*.sfc",
     ["--frames", "20000", "--until-idle", "--apu-console", "--fail-text", "Failed", "--expect-text", "Passed"]),
    # absindx SA-1 tests: TestFinished ($00:0000) is 1 on pass, $FF on failure.
    ("absindx/SnesTestRoms/SA1RebootTest/*.sfc", ["--frames", "600", "--expect-wram", "7E0000=1"]),
    ("absindx/SnesTestRoms/SA1RamProtectionTest/*.sfc", ["--frames", "600", "--expect-wram", "7E0000=1"]),
    # Frame tests: "@frame" marks a ROM whose frame 150 is compared with the
    # screenshot beside it (same name, .png).
    ("peterlemon/PPU/**/*.sfc", ["@frame", "--frames", "150"]),
]


def discover(data: Path, flt: str | None) -> list[tuple[Path, list[str]]]:
    out = []
    for pattern, args in CATALOGUE:
        for rom in sorted(data.glob(pattern)):
            rel = rom.relative_to(data).as_posix()
            if flt and flt not in rel:
                continue
            out.append((rom, args))
    return out


def run_frame(runner: Path, data: Path, rom: Path, args: list[str], timeout: int) -> tuple[str, bool, str]:
    rel = rom.relative_to(data).as_posix()
    reference = rom.with_suffix(".png")
    if not reference.exists():
        return rel, False, "no reference screenshot"
    dump = Path(tempfile.gettempdir()) / f"snes_frame_{os.getpid()}_{abs(hash(rel))}.ppm"
    try:
        run = subprocess.run([str(runner), str(rom), "--quiet", *args, "--dump-frame", str(dump)], capture_output=True,
                             text=True, timeout=timeout)
        if run.returncode not in (0, 1) or not dump.exists():
            return rel, False, f"runner exit {run.returncode}"
        cmp = subprocess.run([sys.executable, str(Path(__file__).with_name("compare_frame.py")), str(dump), str(reference)],
                             capture_output=True, text=True)
        detail = next((line for line in cmp.stdout.splitlines() if "differ" in line or "error" in line), "")
        return rel, cmp.returncode == 0, detail.strip()
    except subprocess.TimeoutExpired:
        return rel, False, "timeout"
    finally:
        if dump.exists():
            dump.unlink()


def run_one(runner: Path, data: Path, rom: Path, args: list[str], timeout: int = 900) -> tuple[str, bool, str]:
    if args and args[0] == "@frame":
        return run_frame(runner, data, rom, args[1:], timeout)
    rel = rom.relative_to(data).as_posix()
    try:
        proc = subprocess.run([str(runner), str(rom), "--quiet", *args], capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return rel, False, "timeout"
    ok = proc.returncode == 0 and "PASS" in proc.stdout.splitlines()
    detail = next((line for line in proc.stdout.splitlines() if line.startswith("FAIL")), "")
    if proc.returncode not in (0, 1):
        detail = f"exit {proc.returncode}: {proc.stderr.strip()[:200]}"
    return rel, ok, detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runner", type=Path, default=REPO / "build" / "bin" / f"snes_run{EXE}")
    parser.add_argument("--data", type=Path, default=DATA)
    parser.add_argument("--filter")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    roms = discover(args.data, args.filter)
    if not roms:
        print("no test ROMs found (fail closed); run tools/fetch_snes_tests.py")
        return 1
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda r: run_one(args.runner, args.data, r[0], r[1]), roms))

    passed = {rel for rel, ok, _ in results if ok}
    for rel, ok, detail in results:
        print(f"{'pass' if ok else 'FAIL'} {rel}{'  ' + detail if detail and not ok else ''}")
    print(f"\n{len(passed)}/{len(results)} passed")

    if args.baseline:
        expected = [line.strip() for line in args.baseline.read_text().splitlines()
                    if line.strip() and not line.startswith("#")]
        if args.filter:
            expected = [rel for rel in expected if args.filter in rel]
        if not expected:
            print("baseline is empty (fail closed)")
            return 1
        known = {rel for rel, _, _ in results}
        missing = [rel for rel in expected if rel not in known]
        regressed = [rel for rel in expected if rel in known and rel not in passed]
        for rel in missing:
            print(f"baseline ROM not found: {rel}")
        for rel in regressed:
            print(f"baseline ROM no longer passes: {rel}")
        if missing or regressed:
            return 1
        print(f"baseline: all {len(expected)} ROMs pass")
        return 0
    return 0 if len(passed) == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
