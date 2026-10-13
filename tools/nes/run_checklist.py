#!/usr/bin/env python3
"""Build the NES checklist ROM and run it interpreted, recompiled and in lockstep.

1. Assembles the ROM (tools/nes/checklist/make_checklist_rom.py) into --out.
2. Runs it in the interpreter (--runner, nes_run) with the scripted input
   the ROM needs and reads the per-test results from --dump-state.
3. With --recompiler (nesrecomp): generates a project, builds it with CMake
   and Ninja against the prebuilt runtime of --build, runs the executable
   natively (same results expected), and again with --differential against
   the interpreter.

Prints one line per test with both verdicts, and the share of instructions
that ran as native code. Fails unless every stage that was asked for ran,
the ROM finished, every test passed everywhere and the lockstep run was
clean (fail closed).

    python tools/nes/run_checklist.py --runner build/bin/nes_run --out DIR
    python tools/nes/run_checklist.py --runner build/bin/nes_run --recompiler build/bin/nesrecomp \
        --build build --out DIR [--trace]

--trace feeds the entry points the interpreter run saw (--trace-out) to
nesrecomp --trace, as a profile-guided build would; without it the
recompiler finds code by static analysis alone.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from checklist_results import Results  # noqa: E402

VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
EXE = ".exe" if os.name == "nt" else ""
LIB = "{}.lib" if os.name == "nt" else "lib{}.a"
RUNTIME_LIBS = ("nesruntime", "nesapu", "nesppu")
NAME = "checklist"


def tool_env() -> dict:
    """MSVC, CMake and Ninja on PATH on Windows; the inherited environment elsewhere."""
    if os.name != "nt" or shutil.which("cl"):
        return dict(os.environ)
    out = subprocess.run(f'"{VCVARS}" >nul && set', shell=True, capture_output=True, text=True).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    if not env:
        return dict(os.environ)
    tools = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft"
    path_key = next((k for k in env if k.upper() == "PATH"), "PATH")
    env[path_key] = rf"{tools}\CMake\CMake\bin;{tools}\CMake\Ninja;" + env.get(path_key, "")
    return env


class Failed(Exception):
    pass


def run(cmd: list, what: str, timeout: int = 900, ok: tuple[int, ...] = (0,), **kwargs) -> subprocess.CompletedProcess:
    started = time.monotonic()
    try:
        proc = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, **kwargs)
    except FileNotFoundError as error:
        raise Failed(f"{what}: {error}")
    except subprocess.TimeoutExpired:
        raise Failed(f"{what}: no result after {timeout} s")
    if proc.returncode not in ok:
        tail = (proc.stdout[-1200:] + proc.stderr[-600:]).strip()
        raise Failed(f"{what} failed (exit {proc.returncode}):\n{tail}")
    print(f"  {what}: {time.monotonic() - started:.1f} s", flush=True)
    return proc


def run_rom(exe: Path, rom: Path, state: Path, meta: Path, args: list[str], what: str,
            extra: list[str] | None = None) -> tuple[Results, str]:
    """One headless run to the ROM's verdict. Returns its results and the runner's output."""
    if state.exists():
        state.unlink()  # a run that writes no state must not inherit an old verdict
    # Exit 1 is the runner's own "a test failed"; the table says which.
    proc = run([exe, rom, *args, "--dump-state", state, *(extra or [])], what, ok=(0, 1))
    if not state.exists():
        raise Failed(f"{what}: no state dump\n{proc.stdout[-600:]}{proc.stderr[-300:]}")
    try:
        return Results(state, meta), proc.stdout
    except (ValueError, KeyError) as error:
        raise Failed(f"{what}: {error}")


def build_recompiled(recompiler: Path, rom: Path, out: Path, build: Path, env: dict, trace: Path | None) -> Path:
    project = out / "generated"
    if project.exists():
        shutil.rmtree(project)  # this script made it; a stale tree must not pass for a fresh one
    cmd = [recompiler, rom, "-o", project, "--name", NAME]
    if trace:
        cmd += ["--trace", trace]
    run(cmd, "nesrecomp", env=env)
    libs = [build / "lib" / LIB.format(n) for n in RUNTIME_LIBS]
    missing = [str(p) for p in libs if not p.exists()]
    if missing:
        raise Failed("the prebuilt runtime is missing (build nes_run in --build first): " + ", ".join(missing))
    path = next((v for k, v in env.items() if k.upper() == "PATH"), None)
    cmake = shutil.which("cmake", path=path) or "cmake"
    ninja = shutil.which("ninja", path=path) or "ninja"
    run([cmake, "-G", "Ninja", "-S", project, "-B", project / "build", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON",  # headless: the prebuilt runtime has no frontend
         "-DNESRT_PREBUILT_LIBS=" + ";".join(str(p) for p in libs),
         f"-DNESRT_PREBUILT_INCLUDE={REPO / 'runtime-nes' / 'include'}"], "configure", env=env)
    run([ninja, "-C", project / "build"], "build", timeout=1800, env=env)
    exe = project / "build" / f"{NAME}{EXE}"
    if not exe.exists():
        raise Failed(f"the build did not produce {exe}")
    return exe


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runner", type=Path, required=True, help="nes_run")
    parser.add_argument("--out", type=Path, required=True, help="work directory")
    parser.add_argument("--recompiler", type=Path, help="nesrecomp; adds the recompiled and lockstep runs")
    parser.add_argument("--build", type=Path, help="root build directory with lib/ (default: the runner's)")
    parser.add_argument("--frames", help="frame limit (default: the ROM's own)")
    parser.add_argument("--diff-frames", default="300", help="frames of the --differential run")
    parser.add_argument("--trace", action="store_true", help="give nesrecomp the interpreter run's entry points")
    args = parser.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runner = args.runner.resolve()
    rom, meta = out / f"{NAME}.nes", out / f"{NAME}.json"
    interp: Results | None = None
    native: Results | None = None
    native_share = diff_line = ""
    diff_ok = None
    try:
        for stale in (rom, meta):
            if stale.exists():
                stale.unlink()
        run([sys.executable, HERE / "checklist" / "make_checklist_rom.py", "-o", rom, "--json", meta], "assemble")
        info = json.loads(meta.read_text())
        keys = info["keys"]
        run_args = ["--blargg", "--frames", args.frames or str(info["frames"]), *keys]

        trace = out / f"{NAME}.trace" if args.trace else None
        if trace and trace.exists():
            trace.unlink()
        interp, _ = run_rom(runner, rom, out / f"{NAME}_interp.state", meta, run_args, "interpreter",
                            ["--trace-out", trace] if trace else None)

        if args.recompiler:
            if trace and not trace.exists():
                raise Failed("the interpreter run wrote no trace")
            env = tool_env()
            build = (args.build or runner.parent.parent).resolve()
            exe = build_recompiled(args.recompiler.resolve(), rom, out, build, env, trace)
            native, stdout = run_rom(exe, rom, out / f"{NAME}_native.state", meta, run_args, "recompiled")
            m = re.search(r"^native=.*$", stdout, re.MULTILINE)
            if not m:
                raise Failed("the recompiled run did not report a native share (did the program install?)\n" +
                             stdout[-400:])
            native_share = m.group(0)
            diff = run([exe, rom, "--differential", "--frames", args.diff_frames, *keys], "differential",
                       timeout=1800, ok=(0, 1))
            diff_ok = diff.returncode == 0 and "no divergence" in diff.stdout
            lines = [line.strip() for line in diff.stdout.splitlines() if line.strip()]
            diff_line = next((line for line in lines if line.startswith("differential:")), " | ".join(lines[:3]))
    except Failed as error:
        print(f"FAILED: {error}")
        return 1

    print()
    print(f"{'#':>2} {'interpreter':11s} " + (f"{'recompiled':11s} " if native else "") + "where  test")
    for i, (t, verdict) in enumerate(interp.rows):
        second = f"{native.rows[i][1]:11s} " if native else ""
        print(f"{t['number']:2d} {verdict:11s} {second}{t['slot']:5s}  {t['description']}")
    print()
    print(f"interpreter: {interp.summary()}")
    ok = interp.ok
    if native:
        print(f"recompiled:  {native.summary()}")
        print(f"             {native_share}")
        print(f"lockstep:    {diff_line or 'no output'}")
        same = [a[1] for a in interp.rows] == [b[1] for b in native.rows]
        if not same:
            print("the recompiled program's verdicts differ from the interpreter's")
        ok = ok and native.ok and same and bool(diff_ok)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
