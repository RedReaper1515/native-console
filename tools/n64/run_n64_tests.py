#!/usr/bin/env python3
"""Run the Nintendo 64 test ROMs and hold them to a baseline.

Each ROM runs headless (n64_run, or a recompiled executable through
--runner) for a fixed number of frames; its verdict is read the way the
suite reports it:

  krom (Peter Lemon's tests): the screen. Every test prints a header row
  whose rightmost red label is "Test Result"; each test case below prints
  PASS in green or FAIL in red in that column. A ROM passes when its column
  holds at least one PASS and no FAIL. ROMs that draw something else (the
  timing tests' values, the comparison matrices, demos) are "unscored".

Fail closed: a missing ROM, a runner error, a timeout, a missing frame or a
baseline ROM that does not pass is a failure; so is an empty selection.

    python3 tools/n64/run_n64_tests.py --runner build/bin/n64_run
    python3 tools/n64/run_n64_tests.py --runner build/bin/n64_run --write-baseline
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATA = ROOT / "roms" / "n64"
DEFAULT_BASELINE = ROOT / "tests" / "n64" / "rom_baseline.txt"
KROM_DIRS = ["krom/CPUTest", "krom/CP1", "krom/RSPTest"]


def read_ppm(path):
    data = Path(path).read_bytes()
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6":
        raise ValueError("not a P6 PPM")
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def krom_verdict(path):
    w, h, px = read_ppm(path)

    def at(x, y):
        i = 3 * (y * w + x)
        return px[i], px[i + 1], px[i + 2]

    def red(p):
        return p[0] > 200 and p[1] < 64 and p[2] < 64

    def green(p):
        return p[1] > 100 and p[0] < 64 and p[2] < 64

    xs = [x for x in range(w) for y in range(8, 16) if red(at(x, y))]
    if not xs:
        return "unscored", 0, 0
    right = max(xs)
    left = right
    # Walk left over the header label (gaps between letters are under 9 pixels).
    while left > 0 and any(red(at(left - k, y)) for k in range(1, 9) if left - k >= 0 for y in range(8, 16)):
        left -= 1
    passes = fails = 0
    for band in range(24, h - 8, 8):
        g = r = 0
        for y in range(band, band + 8):
            for x in range(left, min(w, right + 1)):
                p = at(x, y)
                if green(p):
                    g += 1
                elif red(p):
                    r += 1
        if r:
            fails += 1
        elif g:
            passes += 1
    if fails:
        return "fail", passes, fails
    return ("pass" if passes else "unscored"), passes, fails


def run_one(runner, rom, frames, timeout, tmp):
    out = Path(tmp) / (rom.stem + ".ppm")
    try:
        proc = subprocess.run([runner, str(rom), "--frames", str(frames), "--dump-frame", str(out), "--quiet"],
                              capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "timeout", 0, 0
    if proc.returncode != 0 or not out.exists():
        return "error", 0, 0
    return krom_verdict(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--runner", required=True)
    ap.add_argument("--data", type=Path, default=DEFAULT_DATA)
    ap.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--filter", default="")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()

    roms = []
    for d in KROM_DIRS:
        base = args.data / d
        if base.is_dir():
            roms += sorted(p for p in base.rglob("*.N64"))
    roms = [r for r in roms if args.filter in str(r)]
    if not roms:
        print("FAIL: no test ROMs selected (run tools/fetch_n64_tests.py --suite krom)")
        return 1

    results = {}
    with tempfile.TemporaryDirectory(prefix="n64t-") as tmp:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futures = {pool.submit(run_one, args.runner, r, args.frames, args.timeout, tmp): r for r in roms}
            for f in concurrent.futures.as_completed(futures):
                results[futures[f].relative_to(args.data).as_posix()] = f.result()

    counts = {}
    for name in sorted(results):
        verdict, passes, fails = results[name]
        counts[verdict] = counts.get(verdict, 0) + 1
        print(f"{verdict:9} {name}  ({passes} passed, {fails} failed)")
    print(" ".join(f"{k}={v}" for k, v in sorted(counts.items())))

    if args.write_baseline:
        passing = sorted(n for n, r in results.items() if r[0] == "pass")
        args.baseline.write_text("# Test ROMs that pass (tools/n64/run_n64_tests.py --write-baseline).\n"
                                 + "".join(p + "\n" for p in passing))
        print(f"wrote {len(passing)} ROMs to {args.baseline}")
        return 0

    if not args.baseline.exists():
        print(f"FAIL: no baseline at {args.baseline}")
        return 1
    expected = [l.strip() for l in args.baseline.read_text().splitlines() if l.strip() and not l.startswith("#")]
    expected = [e for e in expected if args.filter in e]
    missing = [e for e in expected if e not in results]
    regressed = [e for e in expected if e in results and results[e][0] != "pass"]
    for e in missing:
        print(f"FAIL: baseline ROM {e} was not run (missing?)")
    for e in regressed:
        print(f"FAIL: baseline ROM {e} no longer passes ({results[e][0]})")
    if missing or regressed or not expected:
        return 1
    print(f"PASS: all {len(expected)} baseline ROMs pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
