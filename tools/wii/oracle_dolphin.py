#!/usr/bin/env python3
"""Run a Wii test program under Dolphin and under gc_run, and put their verdicts side by side.

The Wii test programs (tests/wii/programs) check themselves, and what they
check against is what this project believes a Wii does. Running the same
ELF on an unrelated implementation is the cheapest way to find a belief that
is wrong: where the two disagree, one of them is, and the lines say which
subtest to look at.

    python tools/wii/oracle_dolphin.py --dolphin "D:/Tools/Dolphin/Dolphin-x64/Dolphin.exe" \
        build/wii-programs/wii_fstest.elf

Dolphin is run as a separate program (GPL: a tool here, never linked or
copied) in a user directory of its own under the system's temporary
directory, so nobody's real Dolphin settings or saves are touched. Its
answer is evidence of what a second implementation does, not of what the
hardware does (AGENTS.md, evidence layers).

Not every difference is ours to fix. Dolphin makes its own console: its
flash memory's settings, its registered remotes and the title it reports for
a bare ELF are its choices, as ours are ours, and a subtest that reads one
of those will differ. The filesystem's rules, IOS's error numbers and the
machine's globals should not.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

DOLPHIN_INI = """[Core]
CPUCore = 0
SkipIPL = True
GFXBackend = Null
EmulationSpeed = 0.000000
DSPHLE = True
EnableCheats = False
[Interface]
DebugModeEnabled = True
ConfirmStop = False
UsePanicHandlers = False
OnScreenDisplayMessages = False
[Display]
RenderToMain = True
Fullscreen = False
[DSP]
Backend = No Audio
[Analytics]
PermissionAsked = True
Enabled = False
"""

LOGGER_INI = """[Options]
Verbosity = 4
WriteToFile = True
WriteToConsole = False
WriteToWindow = False
[Logs]
OSREPORT_HLE = True
"""

REPORT = re.compile(r"\[OSREPORT_HLE\]:\s*[0-9a-f]+->[0-9a-f]+\|\s?(.*)$")
VERDICT = re.compile(r"^(--- Test \d+: .*|Test \d+ (passed|failed) .*|\s*FAIL .*|\d+ tests reported)$")


def verdicts(lines: list[str]) -> list[str]:
    return [line.rstrip() for line in lines if VERDICT.match(line.rstrip())]


def run_dolphin(dolphin: Path, elf: Path, timeout: int) -> list[str]:
    if (dolphin.parent / "portable.txt").exists():
        raise RuntimeError(f"{dolphin.parent / 'portable.txt'} exists and overrides the user directory")
    user = Path(tempfile.mkdtemp(prefix="ncwii-dolphin-"))
    try:
        (user / "Config").mkdir(parents=True)
        (user / "Logs").mkdir(parents=True)
        (user / "Config" / "Dolphin.ini").write_text(DOLPHIN_INI, encoding="ascii")
        (user / "Config" / "Logger.ini").write_text(LOGGER_INI, encoding="ascii")
        try:
            subprocess.run([str(dolphin), "-b", "-u", str(user), "-e", str(elf)], timeout=timeout)
        except subprocess.TimeoutExpired:
            print(f"oracle_dolphin: Dolphin was still running after {timeout} s and was stopped")
        log = user / "Logs" / "dolphin.log"
        if not log.exists():
            raise RuntimeError("Dolphin wrote no log; OSReport logging did not start")
        out = []
        for raw in log.read_text(encoding="utf-8", errors="replace").splitlines():
            match = REPORT.search(raw)
            if match:
                out.append(match.group(1))
        return out
    finally:
        shutil.rmtree(user, ignore_errors=True)


def run_ours(gc_run: Path, elf: Path, frames: int, timeout: int) -> list[str]:
    ran = subprocess.run([str(gc_run), str(elf), "--wii", "--frames", str(frames), "--quiet"],
                         capture_output=True, text=True, timeout=timeout)
    return ran.stdout.splitlines()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf", type=Path, nargs="+")
    parser.add_argument("--dolphin", type=Path, default=os.environ.get("DOLPHIN"))
    parser.add_argument("--gc-run", type=Path, default=REPO_ROOT / "build" / "bin" / "gc_run")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if not args.dolphin or not Path(args.dolphin).exists():
        print("oracle_dolphin: pass --dolphin PATH or set DOLPHIN", file=sys.stderr)
        return 1
    gc_run = args.gc_run
    if not gc_run.exists() and gc_run.with_suffix(".exe").exists():
        gc_run = gc_run.with_suffix(".exe")

    differing = 0
    for elf in args.elf:
        ours = verdicts(run_ours(gc_run, elf, args.frames, args.timeout))
        theirs = verdicts(run_dolphin(Path(args.dolphin), elf, args.timeout))
        print(f"== {elf.name}: {len(ours)} verdict lines here, {len(theirs)} under Dolphin")
        if not theirs:
            print("   Dolphin reported nothing: the program did not reach its first test there")
            differing += 1
            continue
        if ours == theirs:
            print("   the same, line for line")
            continue
        differing += 1
        only_here = [line for line in ours if line not in theirs]
        only_there = [line for line in theirs if line not in ours]
        for line in only_here:
            print(f"   here only:    {line}")
        for line in only_there:
            print(f"   Dolphin only: {line}")
    return 1 if differing else 0


if __name__ == "__main__":
    sys.exit(main())
