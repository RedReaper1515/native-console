#!/usr/bin/env python3
"""Run the GameCube test programs and compare against a baseline.

Two suites, one runner, because they print their verdicts the same way.
dolphin-emu/hwtests is the hardware layer of this backend's evidence
(AGENTS.md): it was written against a real console, so where it and our own
vectors disagree it wins. tests/gc/programs is ours, and covers what a pure
CPU suite cannot see - the memory map, the clocks, the interrupts.

This runs each program on gc_run, reads the verdicts out of the OSReport
output, and checks them against tests/gc/hwtest_baseline.txt.

    python tools/gc/run_gc_tests.py --gc-run build/bin/gc_run.exe
    python tools/gc/run_gc_tests.py --update      # after a real improvement

A program that gets worse fails the run. A program that gets *better* also
fails it, with a different message: the baseline is a record of where the
implementation stands, and leaving it stale hides the next regression.

The programs come from tools/gc/build_hwtests.py and
tools/gc/build_gc_programs.py, both of which need devkitPPC. Nothing that
ships depends on them; when neither is built, this says so and exits zero
so that a checkout without devkitPPC is not a failure.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_TESTS = [REPO_ROOT / "build" / "hwtests", REPO_ROOT / "build" / "gc-programs"]
DEFAULT_BASELINE = REPO_ROOT / "tests" / "gc" / "hwtest_baseline.txt"

# Programs that are not run unless asked for, and why.
#
# cputest_fctiw sweeps 771 million cases: hours of interpretation, not a
# failure. The four gxtest programs take about a minute each, and until
# there is a rasteriser every one of them fails almost entirely - so
# recording their numbers would buy four minutes of "still not drawing".
# They come off this list one at a time as P3 gives them something to
# protect. The other three gxtest programs are in the baseline: bitfield
# passes outright in 80 ms, and clipping and rasterization are quick enough
# (0.2 and 3 seconds) that their failure counts are worth watching fall.
SLOW = {
    "cputest_fctiw",
    # 65,536 cases, one copy each, and it prints its verdict only at the end -
    # so it cannot be sampled the way the others are, and the whole of it is
    # about eight minutes. It passes all 65,536 (recorded in docs/GC-PLAN.md);
    # run it with --slow after touching lighting.
    "gxtest_lighting",
    "gxtest_tev",
}

# Programs in the same directory that are not tests of the machine, and so
# have no verdicts to record. gc_spin exists for the frontend's menu
# self-test, which needs a guest that is still running to be paused; it
# deliberately never finishes and never reports anything.
NOT_TESTS = {
    "gc_spin",
    # The Wii's bootable test disc: its apploader, and the program that only
    # has anything to check when it is booted from that disc
    # (tools/wii/check_boot_disc.py does that).
    "apploader",
    "wii_ditest",
}

# Programs that sweep far more than one run can cover, and how many fields
# to give them. Any prefix of the sweep is a real check - the cases are
# independent - so sampling one is worth more than skipping the lot.
FRAMES = {
    # 2,048 blocks of 262,144 assertions. Sixty fields is about sixty
    # blocks, fifteen million assertions, and six seconds.
    "gxtest_intensity": 60,
    # Thousands of independent copy cases, each reporting its own verdict as
    # it goes. The full run is two minutes and passes every one of 6,539,264
    # subtests; a hundred and twenty fields is a real prefix of that for a
    # fraction of the time.
    "gxtest_copyfilter": 120,
}

# What a program needs on gc_run's command line besides the usual.
PROGRAM_ARGS: dict[str, list[str]] = {
    # A remote held in a known, awkward position (gc_run.c).
    "wii_padtest": ["--pad-test"],
}

VERDICT = re.compile(r"^Test (\d+) (passed|failed) \((\d+) subtests(?:, (\d+) failures)?\)")


def run_one(gc_run: Path, program: Path, frames: int, timeout: int,
            disc: Path | None, card: Path | None, extra: list[str]) -> tuple[int, int, str]:
    """(failing subtests, total subtests, note) for one program."""
    command = [str(gc_run), str(program), "--frames", str(frames), "--quiet"] + extra
    # A disc in the drive all the time: one program reads it, and the rest
    # never look at the drive.
    if disc and disc.exists():
        command += ["--disc", str(disc)]
    # A memory card in the first slot, formatted fresh for every run. One
    # program writes to it; leaving an image behind would make its verdict
    # depend on what the last run did, which is the one thing a baseline
    # cannot have.
    if card is not None:
        card.unlink(missing_ok=True)
        command += ["--card", str(card)]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return (-1, 0, "timed out")

    subtests = failures = 0
    verdicts = 0
    for line in result.stdout.splitlines():
        match = VERDICT.match(line.strip())
        if not match:
            continue
        verdicts += 1
        subtests += int(match.group(3))
        failures += int(match.group(4) or 0)
    if not verdicts:
        return (-1, 0, "no verdict: the program did not finish")
    return (failures, subtests, f"{verdicts} tests")


def read_baseline(path: Path) -> dict[str, int]:
    baseline: dict[str, int] = {}
    if not path.exists():
        return baseline
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, _, failures = line.partition(" ")
        baseline[name] = int(failures)
    return baseline


def write_baseline(path: Path, results: dict[str, int]) -> None:
    lines = [
        "# Failing subtests per test program, from tools/gc/run_gc_tests.py.",
        "# Zero means the program passes outright. cputest_* and gxtest_* are",
        "# dolphin-emu/hwtests; the rest are ours, from tests/gc/programs.",
        "# A number here is a debt, not a target: see docs/GC-PLAN.md for what",
        "# each one is waiting on.",
    ]
    # A baseline that already says what it is keeps saying it: the comment
    # at its head is the file's, and only the numbers are rewritten.
    if path.exists():
        kept = []
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.startswith("#"):
                break
            kept.append(line)
        if kept:
            lines = kept
    lines += [f"{name} {failures}" for name, failures in sorted(results.items())]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gc-run", type=Path, default=REPO_ROOT / "build" / "bin" / "gc_run")
    parser.add_argument("--tests", type=Path, action="append",
                        help="a directory of test ELFs; repeatable (default: both build trees)")
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--disc", type=Path, default=DEFAULT_TESTS[1] / "test.gcm",
                        help="a disc image to put in the drive (tools/gc/make_test_disc.py)")
    parser.add_argument("--card", type=Path, default=DEFAULT_TESTS[1] / "card.raw",
                        help="where to keep the memory card image; deleted before each program")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--slow", action="store_true", help="include the very long sweeps")
    parser.add_argument("--update", action="store_true", help="rewrite the baseline")
    parser.add_argument("--runner-arg", action="append", default=[],
                        help="an extra argument for gc_run, such as --runner-arg=--wii "
                             "for the Wii's programs (build/wii-programs)")
    args = parser.parse_args()

    gc_run = args.gc_run
    if not gc_run.exists() and gc_run.with_suffix(".exe").exists():
        gc_run = gc_run.with_suffix(".exe")
    if not gc_run.exists():
        print(f"run_gc_tests: {gc_run} does not exist; build the gc_run target", file=sys.stderr)
        return 1

    trees = args.tests or DEFAULT_TESTS
    programs = sorted(elf for tree in trees for elf in tree.rglob("*.elf"))
    if not programs:
        print("run_gc_tests: no test programs in "
              + ", ".join(str(tree) for tree in trees)
              + "; build them with tools/gc/build_hwtests.py and "
                "tools/gc/build_gc_programs.py (both need devkitPPC)")
        return 0

    baseline = read_baseline(args.baseline)
    results: dict[str, int] = {}
    regressions: list[str] = []
    improvements: list[str] = []

    for program in programs:
        name = program.stem
        if name in NOT_TESTS:
            continue
        if name in SLOW and not args.slow:
            print(f"{name:<24} skipped (see SLOW in this script; pass --slow to include it)")
            continue
        failures, subtests, note = run_one(gc_run, program, FRAMES.get(name, args.frames),
                                           args.timeout, args.disc, args.card,
                                           args.runner_arg + PROGRAM_ARGS.get(name, []))
        results[name] = failures
        expected = baseline.get(name)
        state = "passes" if failures == 0 else (note if failures < 0 else
                                               f"{failures} of {subtests} subtests fail")
        if expected is None:
            print(f"{name:<24} {state}  (not in the baseline)")
            if not args.update:
                regressions.append(f"{name} is not in the baseline")
        elif failures > expected or (failures < 0 <= expected):
            print(f"{name:<24} {state}  WORSE than the baseline ({expected})")
            regressions.append(f"{name}: {expected} -> {failures}")
        elif failures < expected:
            print(f"{name:<24} {state}  better than the baseline ({expected})")
            improvements.append(f"{name}: {expected} -> {failures}")
        else:
            print(f"{name:<24} {state}")

    if args.update:
        write_baseline(args.baseline, results)
        print(f"run_gc_tests: baseline written with {len(results)} programs")
        return 0

    if regressions:
        print("\nregressed:")
        for line in regressions:
            print(f"  {line}")
        return 1
    if improvements:
        print("\nbetter than the baseline - rerun with --update to record it:")
        for line in improvements:
            print(f"  {line}")
        return 1
    print(f"\nrun_gc_tests: {len(results)} programs match the baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main())
