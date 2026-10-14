#!/usr/bin/env python3
"""Recompile the R12 test ROMs, build each one, and check it two ways.

For every ROM selected by run_gba_tests.py (jsmolka, Alyosha): run
gbarecomp, build the generated project against the prebuilt runtime from
the root build, then run the executable

  1. with the ROM's own pass criterion (--until-idle --expect r12=0), and
  2. in --differential mode against the interpreter.

A ROM passes criterion 1 when its interpreter result is also a pass (the
recompiled program must not do better or worse than the interpreter), and
criterion 2 when there is no divergence. Fail closed on build errors.

    python tools/gba/recomp_suite.py [--filter SUBSTR] [--jobs N] [--frames N]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_gba_tests import DATA, REPO, discover, run_one  # noqa: E402

VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"


def tool_env() -> dict:
    """Environment with MSVC, CMake and Ninja on PATH (Windows); the
    inherited environment elsewhere."""
    if os.name != "nt" or shutil.which("cl"):
        return dict(os.environ)
    out = subprocess.run(f'"{VCVARS}" >nul && set', shell=True, capture_output=True, text=True).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    tools = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft"
    path_key = next((k for k in env if k.upper() == "PATH"), "PATH")  # Windows spells it "Path"
    env[path_key] = rf"{tools}\CMake\CMake\bin;{tools}\CMake\Ninja;" + env.get(path_key, "")
    return env


def build_one(rom: Path, data: Path, out_root: Path, env: dict, BUILD: Path) -> tuple[Path | None, str]:
    rel = rom.relative_to(data).with_suffix("")
    name = "t_" + "_".join(rel.parts).replace("-", "_").replace(".", "_")
    out = out_root / name
    exe_name = name + (".exe" if os.name == "nt" else "")
    gen = subprocess.run([str(BUILD / "bin" / ("gbarecomp.exe" if os.name == "nt" else "gbarecomp")), str(rom),
                          "-o", str(out), "--name", name], capture_output=True, text=True, env=env)
    if gen.returncode:
        return None, "gbarecomp failed: " + gen.stderr.strip()
    lib = BUILD / "lib" / ("gbaruntime.lib" if os.name == "nt" else "libgbaruntime.a")
    path = next((v for k, v in env.items() if k.upper() == "PATH"), None)
    cmake = shutil.which("cmake", path=path) or "cmake"
    ninja = shutil.which("ninja", path=path) or "ninja"
    cfg = subprocess.run([cmake, "-G", "Ninja", "-S", str(out), "-B", str(out / "build"),
                          f"-DGBART_PREBUILT_LIB={lib}", f"-DGBART_PREBUILT_INCLUDE={REPO / 'runtimes' / 'runtime-gba' / 'include'}"],
                         capture_output=True, text=True, env=env)
    if cfg.returncode:
        return None, "configure failed: " + cfg.stdout[-400:] + cfg.stderr[-400:]
    bld = subprocess.run([ninja, "-C", str(out / "build")], capture_output=True, text=True, env=env)
    if bld.returncode:
        return None, "build failed: " + bld.stdout[-800:]
    return out / "build" / exe_name, ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--filter")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--out", type=Path, default=REPO / "output" / "gba_suite")
    parser.add_argument("--build", type=Path, default=REPO / "build", help="root build directory (gbarecomp, gba_run, gbaruntime)")
    parser.add_argument("--data", type=Path, default=DATA)
    args = parser.parse_args()
    BUILD = args.build.resolve()  # generated projects are configured elsewhere

    roms = discover(args.data, args.filter)
    if not roms:
        print("no test ROMs selected (fail closed)")
        return 1
    env = tool_env()
    args.out.mkdir(parents=True, exist_ok=True)
    interp = BUILD / "bin" / ("gba_run.exe" if os.name == "nt" else "gba_run")

    def check(rom: Path):
        rel = rom.relative_to(args.data).as_posix()
        exe, err = build_one(rom, args.data, args.out, env, BUILD)
        if not exe:
            return rel, False, False, False, err
        _, interp_ok, _ = run_one(interp, args.data, rom, args.frames)
        _, native_ok, detail = run_one(exe, args.data, rom, args.frames)
        diff = subprocess.run([str(exe), str(rom), "--differential", "--frames", str(min(args.frames, 300))],
                              capture_output=True, text=True, timeout=600)
        diff_ok = diff.returncode == 0
        note = detail if native_ok != interp_ok else ""
        if not diff_ok:
            note += " " + " | ".join(l.strip() for l in diff.stdout.splitlines()[:3])
        return rel, interp_ok, native_ok, diff_ok, note.strip()

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(check, roms))

    agree = diff_pass = 0
    for rel, interp_ok, native_ok, diff_ok, note in results:
        same = interp_ok == native_ok
        agree += same
        diff_pass += diff_ok
        flag = "ok  " if same and diff_ok else "FAIL"
        print(f"{flag} {rel}  interp={'pass' if interp_ok else 'fail'} native={'pass' if native_ok else 'fail'} "
              f"diff={'ok' if diff_ok else 'DIVERGED'}{'  ' + note if note else ''}")
    total = len(results)
    print(f"\nresult parity {agree}/{total}, differential clean {diff_pass}/{total}")
    return 0 if agree == total and diff_pass == total else 1


if __name__ == "__main__":
    sys.exit(main())
