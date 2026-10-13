#!/usr/bin/env python3
"""Run a DS runner through the oracle's test data and read the verdicts.

    python tools/nds/oracle_check.py RUNNER [--suite NAME]... [--only ID]... [--keep DIR]
                                          [-- RUNNER OPTIONS...]

RUNNER is nds_run (or anything taking the same --keys, --frames and
--dump-frame). A suite is a directory of roms/nds/oracle/ with a tests.json: armwrestler,
rockwrestler and polyrastertest (all of them when none is named). For every
run in a suite this starts the runner from boot with the run's keys, has it
write the picture after the run's number of frames, and compares each check's
rectangle with the SHA-256 the file gives for a pass. Options after -- are
passed to the runner, for example -- --bios9 FILE --bios7 FILE.

A check passes only when its rectangle matches. A runner that fails to start,
times out or writes no picture fails every check of that run, and the exit
status is non-zero unless every check of every run passed. Standard library
only; the oracle_make_*.py scripts next to this one write the data.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# The DS test data (gitignored): fetched ROMs, and the oracle's data written
# by the oracle_make_*.py scripts. --data DIR names another copy of it, as
# CMake's NDS_TEST_DATA_DIR does.
DATA = ROOT / "roms" / "nds"


def oracle_dir(data: Path) -> Path:
    return data / "oracle"


def rom_file(data: Path, name: str) -> Path:
    """A ROM as a tests.json names it ("roms/nds/..."), in this copy of the data."""
    prefix = "roms/nds/"
    return data / name[len(prefix):] if name.startswith(prefix) else ROOT / name


HERE = oracle_dir(DATA)
TO_5_BITS = bytes(value >> 3 for value in range(256))
W, H = 256, 192


def read_picture(path: Path) -> bytes | None:
    """The 256x384 picture's pixels in 5-bit channels, or None if it is not one."""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    fields, pos = [], 0
    try:
        while len(fields) < 4:
            while data[pos:pos + 1].isspace():
                pos += 1
            end = pos
            while not data[end:end + 1].isspace():
                end += 1
            fields.append(data[pos:end])
            pos = end
    except IndexError:
        return None
    pixels = data[pos + 1:]
    if fields != [b"P6", b"%d" % W, b"%d" % (2 * H), b"255"] or len(pixels) != W * 2 * H * 3:
        return None
    return pixels.translate(TO_5_BITS)


def rect_sha256(pixels: bytes, screen: str, rect: list[int]) -> str:
    x, y, w, h = rect
    y += H if screen == "bottom" else 0
    rows = b"".join(pixels[((y + row) * W + x) * 3:((y + row) * W + x + w) * 3] for row in range(h))
    return hashlib.sha256(rows).hexdigest()


def main() -> int:
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        split = argv.index("--")
        argv, extra = argv[:split], argv[split + 1:]
    where = argparse.ArgumentParser(add_help=False)
    where.add_argument("--data", type=Path, default=DATA, metavar="DIR",
                       help="the DS test data, if not this checkout's roms/nds")
    here = oracle_dir(where.parse_known_args(argv)[0].data)
    suites = sorted(path.parent.name for path in here.glob("*/tests.json"))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], parents=[where])
    parser.add_argument("runner", type=Path)
    parser.add_argument("--suite", action="append", choices=suites)
    parser.add_argument("--only", action="append", default=[], metavar="ID", help="run only these run ids")
    parser.add_argument("--keep", type=Path, metavar="DIR", help="keep the runner's pictures here")
    parser.add_argument("--timeout", type=float, default=300.0, help="seconds allowed per run")
    args = parser.parse_args(argv)
    if not args.runner.is_file():
        print(f"{args.runner}: no such runner", file=sys.stderr)
        return 2
    if not suites:
        print(f"{here}: no tests.json here (run the tools/nds/oracle_make_*.py scripts)", file=sys.stderr)
        return 2

    scratch = tempfile.TemporaryDirectory(prefix="nds-oracle-tests-")
    pictures = args.keep or Path(scratch.name)
    pictures.mkdir(parents=True, exist_ok=True)
    checks_run = checks_passed = runs_selected = 0
    for suite in args.suite or suites:
        data = json.loads((here / suite / "tests.json").read_text())
        for run in data["runs"]:
            if args.only and run["id"] not in args.only:
                continue
            if not run["checks"]:
                continue  # an information screen, nothing to pass or fail
            runs_selected += 1
            picture = pictures / f"{suite}-{run['id']}.ppm"
            picture.unlink(missing_ok=True)
            command = [str(args.runner), str(rom_file(args.data, data["rom"])), "--frames", str(run["frames"]),
                       "--dump-frame", str(picture), "--quiet", *extra]
            for key in run["keys"]:
                command += ["--keys", key]
            problem = ""
            try:
                done = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
                if done.returncode:
                    problem = f"runner exit status {done.returncode}"
            except subprocess.TimeoutExpired:
                problem = f"runner still going after {args.timeout:.0f} s"
            except OSError as error:
                problem = f"runner did not start: {error}"
            pixels = None if problem else read_picture(picture)
            if not problem and pixels is None:
                problem = "no 256x384 picture written"
            failed = []
            for check in run["checks"]:
                checks_run += 1
                if pixels is not None and rect_sha256(pixels, check["screen"], check["rect"]) == check["sha256"]:
                    checks_passed += 1
                else:
                    failed.append(check["name"])
            verdict = "ok  " if not failed else "FAIL"
            detail = problem or (", ".join(failed) if failed else f"{len(run['checks'])} checks")
            print(f"{verdict} {suite} {run['id']}: {detail}", flush=True)
    scratch.cleanup()
    print(f"{checks_passed} of {checks_run} checks passed in {runs_selected} runs")
    if not checks_run:
        print("nothing was selected", file=sys.stderr)
        return 1
    return 0 if checks_passed == checks_run else 1


if __name__ == "__main__":
    sys.exit(main())
