#!/usr/bin/env python3
"""Run the vector generator on an oracle and write the vector file.

PowerPC has no per-instruction test suite to download, so this project makes
its own: tests/gc/vectors/gen/ is a GameCube program that executes one
instruction at a time with a known register state and prints what changed.
This script runs it under Dolphin - used as a tool, never linked or copied -
and turns the log into tests/gc/vectors/gekko.vectors.

    python tools/gc/capture_vectors.py --dolphin "D:/Tools/Dolphin/Dolphin-x64/Dolphin.exe"

Dolphin needs three things to be usable this way, and this script sets all
three in a Dolphin user directory of its own, so it never touches a real
Dolphin installation:

  - OSReport HLE, which is how the guest's output reaches the host. Dolphin
    loads the ELF's symbols by itself, but the hook is flagged debug-only,
    so DebugModeEnabled has to be on as well (the interpreter core enables
    it anyway).
  - the interpreter CPU core, so that what is measured is the architecture
    and not the JIT's choice of what to leave in undefined register halves.
  - the OSREPORT_HLE log channel written to a file.
  - the null video backend and no audio, so the run is headless.

The vectors are evidence of what Dolphin does, which is a second
implementation, not hardware (AGENTS.md, evidence layers). The hardware
layer is hwtests/cputest. Anywhere the two disagree, the manual decides and
docs/GC-PLAN.md records it.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_ELF = REPO_ROOT / "build" / "gc-vectors" / "gc_vector_gen.elf"
DEFAULT_OUT = REPO_ROOT / "tests" / "gc" / "vectors" / "gekko.vectors"

DOLPHIN_INI = """[Core]
# The interpreter, not the JIT. The JIT is free to leave anything it likes
# in the half of a register an instruction does not define - its SSE
# double-precision arithmetic leaves frA's ps1 in the destination's ps1, for
# one - and those are artefacts of the recompiler, not of the CPU. The
# interpreter is Dolphin's reference implementation and is what a vector
# should be measured against.
CPUCore = 0
SkipIPL = True
GFXBackend = Null
EmulationSpeed = 0.000000
DSPHLE = True
EnableCheats = False
[Interface]
# OSReport's HLE hook is flagged debug-only: without this it is not
# installed and the guest's output never reaches the host.
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

LINE = re.compile(r"\[OSREPORT_HLE\]:\s*[0-9a-f]+->[0-9a-f]+\|\s*(V\|.*)$")


def fail(message: str) -> int:
    print(f"capture_vectors: {message}", file=sys.stderr)
    return 1


def run_oracle(dolphin: Path, elf: Path, user_dir: Path, timeout: int) -> list[str]:
    # A portable.txt next to the executable silently overrides --user, which
    # would send the log somewhere this script never looks at - and would
    # also mean writing into someone's real Dolphin configuration.
    if (dolphin.parent / "portable.txt").exists():
        raise RuntimeError(
            f"{dolphin.parent / 'portable.txt'} exists; it overrides --user. Remove it, or "
            "point --dolphin at a copy that has none."
        )
    config = user_dir / "Config"
    config.mkdir(parents=True, exist_ok=True)
    (user_dir / "Logs").mkdir(parents=True, exist_ok=True)
    (config / "Dolphin.ini").write_text(DOLPHIN_INI, encoding="ascii")
    (config / "Logger.ini").write_text(LOGGER_INI, encoding="ascii")
    log = user_dir / "Logs" / "dolphin.log"
    if log.exists():
        log.unlink()

    command = [str(dolphin), "-b", "-u", str(user_dir), "-e", str(elf)]
    print(" ".join(command), flush=True)
    result = subprocess.run(command, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"Dolphin exited with {result.returncode}")
    if not log.exists():
        raise RuntimeError(f"no log at {log}; OSREPORT_HLE logging did not start")

    lines = []
    for raw in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE.search(raw)
        if match:
            lines.append(match.group(1)[2:].strip())
    return lines


def convert(lines: list[str]) -> tuple[str, int]:
    """Turn the guest's output into the vector file, checking it is whole."""
    if not lines or not lines[0].startswith("SCRATCH "):
        raise RuntimeError("the run did not start with a SCRATCH line")
    scratch = lines[0].split()[1]
    if not lines[1].startswith("CASES "):
        raise RuntimeError("the run did not report a case count")
    expected = int(lines[1].split()[1]) * int(lines[1].split()[3])

    out = [
        "# Gekko instruction vectors.",
        "# Generated by tools/gc/capture_vectors.py from tests/gc/vectors/gen/,",
        "# measured on Dolphin. Evidence layer: a second implementation, not",
        "# hardware (docs/GC-PLAN.md). Do not edit by hand.",
        f"scratch {scratch}",
    ]
    cases = 0
    pending = None
    tags = {"A": "gpr", "B": "fpr0", "C": "fpr1", "D": "mem"}

    for line in lines[2:]:
        if line.startswith("END "):
            if int(line.split()[1]) != expected:
                raise RuntimeError(f"the run ended at {line.split()[1]} of {expected} cases")
            pending = None
            break
        if line.startswith("K "):
            parts = line.split()
            if len(parts) != 5:
                raise RuntimeError(f"bad case line: {line}")
            cases += 1
            pending = parts[1]
            out.append(f"case {parts[1]} {parts[2]} {parts[3]} {parts[4]}")
            continue
        parts = line.split()
        if len(parts) < 4 or parts[0] not in tags or parts[2] not in ("I", "O"):
            raise RuntimeError(f"bad state line: {line}")
        if pending is None:
            raise RuntimeError(f"a state line arrived before its case: {line}")
        direction = "in" if parts[2] == "I" else "out"
        out.append(f"{direction}.{tags[parts[0]]} " + " ".join(parts[3:]))
    else:
        raise RuntimeError("the run did not finish; no END line")

    if cases != expected:
        raise RuntimeError(f"{cases} cases captured, {expected} expected")
    return "\n".join(out) + "\n", cases


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dolphin", type=Path, default=os.environ.get("DOLPHIN"))
    parser.add_argument("--elf", type=Path, default=DEFAULT_ELF)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--user-dir", type=Path, default=REPO_ROOT / "build" / "gc-dolphin")
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args()

    if not args.dolphin:
        return fail("pass --dolphin PATH or set DOLPHIN; Dolphin is the oracle this reads")
    dolphin = Path(args.dolphin)
    if not dolphin.exists():
        return fail(f"{dolphin} does not exist")
    if not args.elf.exists():
        return fail(f"{args.elf} does not exist; build it with tools/gc/build_vector_gen.py")

    try:
        lines = run_oracle(dolphin, args.elf, args.user_dir, args.timeout)
        text, cases = convert(lines)
    except subprocess.TimeoutExpired:
        return fail(f"Dolphin did not finish within {args.timeout}s")
    except RuntimeError as error:
        return fail(str(error))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="ascii", newline="\n")
    print(f"capture_vectors: {cases} cases -> {args.out} ({args.out.stat().st_size / 1e6:.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
