#!/usr/bin/env python3
"""Recompile the NES test ROMs, build each one, and check it two ways (the P4 gate).

For every ROM of run_nes_tests.py's catalogue:

  1. the interpreter (nes_run) reads the ROM's verdict, and with --trace
     also writes the entry points it saw;
  2. nesrecomp generates a project, which is built with CMake and Ninja
     against the prebuilt runtime of the root build (headless, no SDL);
  3. the executable reads the verdict the same way: it must be the
     interpreter's verdict, pass or fail, and a failure must be the same
     failure (recompiled code must not do better or worse);
  4. the executable runs in --differential lockstep with the interpreter
     for as many frames as the ROM needed: CPU registers and the cycle
     counter after every native slice, memory and the picture every frame.

A table shows both verdicts, the lockstep result and the share of
instructions that ran as native code. Fail closed: a generation or build
error, a missing native share (the program did not install), a timeout or
an empty selection fails.

    python tools/nes/recomp_suite.py --build build [--filter SUBSTR]... [--subset]
                                     [--jobs N] [--no-trace] [--out DIR]

--subset runs the short representative list the CTest nes_recomp_suite uses.
--no-trace leaves discovery to static analysis alone (the default feeds the
interpreter run's entry points to nesrecomp --trace, as the hub's profiling
step does for a game).

Differential mode is not a hardware oracle: both sides share the runtime's
devices. It shows that generated code executes the program exactly as the
interpreter does; the verdicts are the independent evidence.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_checklist import LIB, RUNTIME_LIBS, tool_env  # noqa: E402
from run_nes_tests import DATA, EXE, REPO, Rom, command, discover, run_one  # noqa: E402

# One ROM per verdict method and cartridge board, short ones: the CTest.
SUBSET = (
    "other/nestest.nes",                                # NROM, trace
    "instr_test-v5/rom_singles/11-stack.nes",           # NROM with PRG RAM, blargg
    "instr_misc/instr_misc.nes",                        # MMC1 multi-test
    "cpu_interrupts_v2/rom_singles/1-cli_latency.nes",  # IRQ timing
    "ppu_vbl_nmi/rom_singles/05-nmi_timing.nes",        # NMI timing
    "cpu_reset/registers.nes",                          # the reset button
    "branch_timing_tests/1.Branch_Basics.nes",          # $F8
    "blargg_apu_2005.07.30/08.irq_timing.nes",          # $F0
    "cpu_dummy_reads/cpu_dummy_reads.nes",              # CNROM, text
    "mmc3_test_2/rom_singles/1-clocking.nes",           # MMC3
    "read_joy3/test_buttons.nes",                       # scripted input
    "nes15-1.0.0/nes15-NTSC.nes",                       # a game
)

# Options of a verdict run that the lockstep run needs too (same input, same start).
CARRIED = ("--keys", "--pc")
CARRIED_FLAGS = ("--auto-input",)


@dataclass
class Row:
    rel: str
    interp: bool | None = None
    native: bool | None = None
    diff: bool | None = None
    share: str = ""
    note: str = ""
    build_seconds: float = 0.0
    seconds: float = 0.0

    @property
    def ok(self) -> bool:
        return self.interp is not None and self.interp == self.native and bool(self.diff)


def run(cmd: list, timeout: int, env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, env=env,
                          errors="replace")


def carried(args: tuple[str, ...]) -> list[str]:
    out, i = [], 0
    while i < len(args):
        if args[i] in CARRIED and i + 1 < len(args):
            out += [args[i], args[i + 1]]
            i += 2
            continue
        if args[i] in CARRIED_FLAGS:
            out.append(args[i])
        i += 1
    return out


def verdict_rom(rom: Rom) -> Rom:
    """nestest's trace needs the interpreter; recompiled, it is judged by its result bytes
    once the CPU has run off the end of the test and stopped."""
    if rom.method != "nestest":
        return rom
    return Rom(rom.rel, "ram", rom.frames,
               ("--pc", "C000", "--until-idle", "--expect-ram", "02=0", "--expect-ram", "03=0"))


def check(rom: Rom, opts, env: dict, tools: tuple[str, str]) -> Row:
    row = Row(rom.rel)
    started = time.monotonic()
    data: Path = opts.data
    path = data / rom.rel
    # Short names: MSVC and CMake's compiler checks fail beyond 260 characters.
    name = "t" + hashlib.sha1(rom.rel.encode()).hexdigest()[:7]
    project = opts.out / name
    try:
        if not path.exists():
            row.note = "ROM not found"
            return row
        interp = run_one(opts.runner, data, rom, opts.timeout)
        row.interp = interp.ok
        judged = verdict_rom(rom)
        if judged is not rom and row.interp:
            row.interp = run_one(opts.runner, data, judged, opts.timeout).ok

        if project.exists():
            shutil.rmtree(project)  # a stale tree must not pass for a fresh one
        project.mkdir(parents=True)
        trace = None
        if not opts.no_trace:
            trace = project / "trace.txt"
            traced = run([*command(opts.runner, data, judged), "--trace-out", trace, "--quiet"], opts.timeout)
            if traced.returncode not in (0, 1) or not trace.exists():
                row.note = f"no trace from the interpreter (exit {traced.returncode})"
                return row
        build_started = time.monotonic()
        gen = run([opts.recompiler, path, "-o", project / "p", "--name", name, *(["--trace", trace] if trace else [])],
                  opts.timeout, env)
        if gen.returncode:
            row.note = "nesrecomp failed: " + (gen.stderr or gen.stdout).strip()[-300:]
            return row
        cmake, ninja = tools
        cfg = run([cmake, "-G", "Ninja", "-S", project / "p", "-B", project / "b", "-DCMAKE_BUILD_TYPE=Release",
                   "-DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON",  # headless: the prebuilt runtime has no frontend
                   "-DNESRT_PREBUILT_LIBS=" + ";".join(str(p) for p in opts.libs),
                   f"-DNESRT_PREBUILT_INCLUDE={REPO / 'runtime-nes' / 'include'}"], opts.timeout, env)
        if cfg.returncode:
            row.note = "configure failed: " + (cfg.stdout[-300:] + cfg.stderr[-300:]).strip()
            return row
        bld = run([ninja, "-C", project / "b"], 1800, env)
        exe = project / "b" / f"{name}{EXE}"
        if bld.returncode or not exe.exists():
            row.note = "build failed: " + bld.stdout[-600:].strip()
            return row
        row.build_seconds = time.monotonic() - build_started

        native = run_one(exe, data, judged, opts.timeout)
        row.native = native.ok
        # The share of native instructions, and proof that the program installed.
        shown = run(command(exe, data, judged), opts.timeout)
        share = re.search(r"^native=([0-9.]+)%", shown.stdout, re.MULTILINE)
        if not share:
            row.native = None
            row.note = "no native share reported (the program did not install)"
            return row
        row.share = share.group(1)
        if row.native != row.interp:
            row.note = f"verdicts differ: recompiled {native.detail or 'passes'}"
        elif native.detail != interp.detail and rom.method != "nestest":
            # Both fail, but not the same way (another result code, another AccuracyCoin table).
            row.native = None
            row.note = f"verdicts differ: interpreted {interp.detail}; recompiled {native.detail}"

        frames = int(interp.frames) + 30 if interp.frames.isdigit() else rom.frames
        frames = max(60, min(frames, rom.frames))
        diff = run([exe, path, "--differential", "--frames", frames, *carried(judged.args)], 1800)
        row.diff = diff.returncode == 0 and "no divergence" in diff.stdout
        if not row.diff:
            lines = [line.strip() for line in diff.stdout.splitlines() if line.strip()]
            row.note = (row.note + " " + " | ".join(lines[:3] or [f"exit {diff.returncode}"])).strip()
    except subprocess.TimeoutExpired as error:
        row.note = f"timeout: {Path(str(error.cmd[0])).name}"
    except OSError as error:
        row.note = str(error)
    finally:
        row.seconds = time.monotonic() - started
        if row.ok and not opts.keep:
            shutil.rmtree(project, ignore_errors=True)
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, default=REPO / "build", help="root build directory (bin/ and lib/)")
    parser.add_argument("--runner", type=Path, help="nes_run (default: the build's)")
    parser.add_argument("--recompiler", type=Path, help="nesrecomp (default: the build's)")
    parser.add_argument("--data", type=Path, default=DATA)
    parser.add_argument("--out", type=Path, default=REPO / "output" / "nes_suite", help="work directory")
    parser.add_argument("--filter", action="append", help="only ROMs whose path contains this (repeatable)")
    parser.add_argument("--subset", action="store_true", help="the representative ROMs of the CTest")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument("--timeout", type=int, default=600, help="seconds per step")
    parser.add_argument("--no-trace", action="store_true", help="static analysis alone")
    parser.add_argument("--keep", action="store_true", help="keep the generated projects of passing ROMs")
    opts = parser.parse_args()

    build = opts.build.resolve()
    opts.runner = (opts.runner or build / "bin" / f"nes_run{EXE}").resolve()
    opts.recompiler = (opts.recompiler or build / "bin" / f"nesrecomp{EXE}").resolve()
    opts.out = opts.out.resolve()
    opts.data = opts.data.resolve()
    opts.libs = [build / "lib" / LIB.format(n) for n in RUNTIME_LIBS]
    missing = [str(p) for p in (opts.runner, opts.recompiler, *opts.libs) if not p.exists()]
    if missing:
        print("missing (build nes_run and nesrecomp first; fail closed): " + ", ".join(missing))
        return 1

    roms = discover(opts.data, None)
    if opts.subset:
        roms = [rom for rom in roms if rom.rel.endswith(SUBSET)]
        if len(roms) != len(SUBSET):
            print("the subset names ROMs that are not in the catalogue (fail closed)")
            return 1
    if opts.filter:
        roms = [rom for rom in roms if any(f in rom.rel for f in opts.filter)]
    roms = [rom for rom in roms if rom.method != "coin" or not opts.subset]
    if not roms:
        print("no test ROMs selected (fail closed)")
        return 1
    opts.out.mkdir(parents=True, exist_ok=True)

    env = tool_env()
    path = next((v for k, v in env.items() if k.upper() == "PATH"), None)
    tools = (shutil.which("cmake", path=path) or "cmake", shutil.which("ninja", path=path) or "ninja")

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max(1, opts.jobs)) as pool:
        rows = list(pool.map(lambda rom: check(rom, opts, env, tools), roms))
    elapsed = time.monotonic() - started

    def word(value: bool | None) -> str:
        return "-" if value is None else "pass" if value else "fail"

    width = max(len(r.rel) for r in rows)
    print(f"{'':4s} {'interp':6s} {'recomp':6s} {'lockstep':8s} {'native':>7s}  ROM")
    for r in rows:
        lock = "-" if r.diff is None else "clean" if r.diff else "DIVERGED"
        print(f"{'ok' if r.ok else 'FAIL':4s} {word(r.interp):6s} {word(r.native):6s} {lock:8s} "
              f"{(r.share + '%') if r.share else '-':>7s}  {r.rel:{width}s}{'  ' + r.note if r.note else ''}")
    total = len(rows)
    parity = sum(r.interp is not None and r.interp == r.native for r in rows)
    clean = sum(bool(r.diff) for r in rows)
    shares = sorted(float(r.share) for r in rows if r.share)
    print(f"\nverdict parity {parity}/{total}, lockstep clean {clean}/{total}", end="")
    if shares:
        print(f", native share median {shares[len(shares) // 2]:.1f}% (min {shares[0]:.1f}%)", end="")
    print(f"; {elapsed:.0f} s ({sum(r.build_seconds for r in rows):.0f} s of generating and building, summed over jobs)")
    return 0 if all(r.ok for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
