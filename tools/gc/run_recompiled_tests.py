"""Recompile every guest test program, build it, and hold it to the interpreter.

This is P4's gate. For each program it runs `gcrecomp`, builds the project
that comes out, and then runs that program twice - once through the
recompiled blocks and once with `--interpret`, which is the same executable
driving the same machine through the interpreter instead. Three things have
to match:

  * the verdict lines the program printed, which is what it was written to
    report;
  * the instruction count, because a recompiled block that ran one
    instruction too many or too few would otherwise pass unnoticed;
  * the final program counter.

Comparing a recompiled run against a *separate* interpreter would leave
room for the two to differ by the harness. Comparing one executable against
itself leaves room only for the thing being tested.

    python tools/gc/run_recompiled_tests.py
    python tools/gc/run_recompiled_tests.py --only gc_memtest

Programs are checked side by side (`--parallel`), each one's recompiled and
interpreted runs at the same time, and every stage is timed so the slow one
is visible. Nothing a program is compared on depends on wall-clock time: the
machine counts instructions, not seconds.

Needs a compiler; on Windows that means running under a vcvars shell, the
same as the rest of the build.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PROGRAMS = REPO_ROOT / "build" / "gc-programs"
VERDICT = re.compile(r"^(?:Test \d+ (?:passed|failed)|\s*FAIL .*|\d+ tests reported)$")
SUMMARY = re.compile(r"(\d+) fields, (\d+) instructions, pc ([0-9a-f]+)")

# A program that needs a disc, and the disc it needs.
DISC = {"gc_disctest": PROGRAMS / "test.gcm"}

# ELFs in a program directory that are not programs to run: the Wii boot
# disc's apploader, which is called by a loader rather than started.
NOT_PROGRAMS = {"apploader"}

# Programs that take too long to be worth building into the default run.
# Nothing here is skipped for being awkward - only for being slow.
SLOW = {"gc_timetest"}

# Programs that save, which run with a memory card in the slot (the runner's
# --card): a new card each, one for each run, so the two start from the same
# blank card and neither sees the other's writes. Without one they would only
# report that there is no card.
CARD = {"gc_cardtest"}


def run(command: list[str], cwd: Path | None = None, timeout: int = 1800):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def verdicts(text: str) -> list[str]:
    return [line.rstrip() for line in text.splitlines() if VERDICT.match(line.rstrip())]


class Result:
    """What one program's check found, reported in one piece when it is done,
    so programs checked side by side do not interleave their lines."""

    def __init__(self, name: str):
        self.name = name
        self.ok = False
        self.lines: list[str] = []
        self.times: dict[str, float] = {}
        self.shape = "?"

    def timed(self, stage: str, started: float) -> None:
        self.times[stage] = time.monotonic() - started


def check_one(program: Path, work: Path, gcrecomp: Path, jobs: int, keep: bool,
              recompiler_args: list[str]) -> Result:
    name = program.stem
    result = Result(name)
    out = work / name
    if out.exists() and not keep:
        shutil.rmtree(out, ignore_errors=True)

    started = time.monotonic()
    made = run([str(gcrecomp), str(program), "-o", str(out), "--name", name] + recompiler_args)
    result.timed("recompile", started)
    if made.returncode != 0:
        result.lines.append(f"gcrecomp failed\n{made.stdout}{made.stderr}")
        return result
    blocks = re.search(r"(\d+) blocks, (\d+) instructions", made.stdout)
    if blocks:
        result.shape = f"{blocks.group(1)} blocks"

    started = time.monotonic()
    # Headless on purpose: with SDL2 found (SDL2_DIR in the environment) the
    # program becomes the windowed frontend, which runs until it is closed
    # and does not take the runner's --interpret.
    configured = run(["cmake", "-G", "Ninja", "-B", "build", "-DCMAKE_BUILD_TYPE=Release",
                      "-DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON"],
                     cwd=out)
    result.timed("configure", started)
    if configured.returncode != 0:
        result.lines.append(f"cmake would not configure\n{configured.stdout}{configured.stderr}")
        return result
    started = time.monotonic()
    built = run(["cmake", "--build", "build", "-j", str(jobs)], cwd=out)
    result.timed("compile", started)
    if built.returncode != 0:
        result.lines.append("the generated project would not build")
        result.lines += ["    " + line for line in (built.stdout + built.stderr).splitlines()[-20:]]
        return result

    executable = out / "build" / (name + (".exe" if sys.platform == "win32" else ""))
    if not executable.exists():
        result.lines.append(f"{executable} was not produced")
        return result

    arguments = []
    disc = DISC.get(name)
    if disc is not None:
        # The disc beside the programs being checked, whichever set that is.
        disc = program.parent / disc.name
    if disc and disc.exists():
        arguments += ["--disc", str(disc)]

    cards = {"recompiled": [], "interpreted": []}
    if name in CARD:
        for which in cards:
            card = out / f"{which}.card"
            card.unlink(missing_ok=True)
            cards[which] = ["--card", str(card)]

    # The two runs share nothing but the executable, so they go at once.
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=2) as pool:
        recompiled_future = pool.submit(run, [str(executable)] + arguments + cards["recompiled"])
        interpreted_future = pool.submit(run, [str(executable)] + arguments + cards["interpreted"] + ["--interpret"])
        recompiled = recompiled_future.result()
        interpreted = interpreted_future.result()
    result.timed("run both", started)

    ok = True
    if verdicts(recompiled.stdout) != verdicts(interpreted.stdout):
        ok = False
        result.lines.append("the two runs report different verdicts")
        for line in verdicts(recompiled.stdout):
            if line not in verdicts(interpreted.stdout):
                result.lines.append(f"    only recompiled: {line}")
        for line in verdicts(interpreted.stdout):
            if line not in verdicts(recompiled.stdout):
                result.lines.append(f"    only interpreted: {line}")

    left = SUMMARY.search(recompiled.stderr)
    right = SUMMARY.search(interpreted.stderr)
    if not left or not right:
        ok = False
        result.lines.append("a run did not report how far it got")
    elif left.groups() != right.groups():
        ok = False
        result.lines.append(f"recompiled ran {left.group(2)} instructions to pc {left.group(3)}, "
                            f"interpreted {right.group(2)} to pc {right.group(3)}")

    result.ok = ok
    return result


def report(result: Result) -> None:
    stages = ", ".join(f"{stage} {seconds:.1f} s" for stage, seconds in result.times.items())
    print(f"  {result.name}: {'agree' if result.ok else 'DISAGREE'} ({result.shape}; {stages})",
          flush=True)
    for line in result.lines:
        print(f"      {line}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gcrecomp", type=Path,
                        default=REPO_ROOT / "build-gc" / "bin" / "gcrecomp")
    parser.add_argument("--work", type=Path, default=REPO_ROOT / "build" / "gc-recompiled")
    parser.add_argument("--programs", type=Path, default=PROGRAMS,
                        help="the directory of guest programs (build/wii-programs for the Wii's)")
    parser.add_argument("--recompiler-arg", action="append", default=[],
                        help="an extra argument for gcrecomp, such as --recompiler-arg=--wii")
    parser.add_argument("--only", action="append", help="just these programs, by name")
    parser.add_argument("--slow", action="store_true", help="include the long ones")
    parser.add_argument("--keep", action="store_true", help="reuse the generated projects")
    parser.add_argument("-j", type=int, default=0,
                        help="compile jobs for each generated project "
                             "(default: the processor's threads shared between the programs)")
    parser.add_argument("--parallel", type=int, default=0,
                        help="programs checked at once (default: all of them)")
    args = parser.parse_args()

    gcrecomp = args.gcrecomp
    if not gcrecomp.exists() and gcrecomp.with_suffix(".exe").exists():
        gcrecomp = gcrecomp.with_suffix(".exe")
    if not gcrecomp.exists():
        print(f"run_recompiled_tests: {gcrecomp} does not exist; build the gcrecomp target",
              file=sys.stderr)
        return 1

    programs = [p for p in sorted(args.programs.glob("*.elf")) if p.stem not in NOT_PROGRAMS]
    if args.only:
        programs = [p for p in programs if p.stem in set(args.only)]
    elif not args.slow:
        programs = [p for p in programs if p.stem not in SLOW]
    if not programs:
        print("run_recompiled_tests: no programs to recompile", file=sys.stderr)
        return 1

    args.work.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    parallel = args.parallel if args.parallel > 0 else len(programs)
    # The processor's threads shared out, a little oversubscribed because a
    # compile spends part of its time reading headers - but not one full job
    # count per program, which on a big machine would start hundreds of
    # compilers at once and run out of memory before it ran out of cores.
    jobs = args.j if args.j > 0 else max(2, -(-(os.cpu_count() or 8) * 3 // (2 * parallel)))
    results: list[Result] = []
    with ThreadPoolExecutor(max_workers=parallel) as pool:
        futures = [pool.submit(check_one, program, args.work, gcrecomp, jobs, args.keep,
                               args.recompiler_arg)
                   for program in programs]
        for future in futures:
            result = future.result()
            report(result)
            results.append(result)

    ok = all(result.ok for result in results)
    # Where the time went, summed over programs, so the next thing to make
    # faster is the first line rather than a guess.
    totals: dict[str, float] = {}
    for result in results:
        for stage, seconds in result.times.items():
            totals[stage] = totals.get(stage, 0.0) + seconds
    wall = time.monotonic() - started
    print("run_recompiled_tests: " + ", ".join(
        f"{stage} {seconds:.0f} s" for stage, seconds in sorted(totals.items(), key=lambda kv: -kv[1]))
        + f" summed over {len(results)} programs; {wall:.0f} s wall, {parallel} at once, "
          f"{jobs} compile jobs each")
    print("run_recompiled_tests: " +
          ("every program agrees with the interpreter"
           if ok else "the recompiled and interpreted runs differ"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
