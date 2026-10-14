#!/usr/bin/env python3
"""Recompile the SNES test ROMs, build each one, and check it two ways.

For every ROM of run_snes_tests.py's catalogue, and the checklist ROM
(tools/snes/checklist): run snesrecomp, build the generated project against
the prebuilt runtime from the root build, then run the executable

  1. with the ROM's own pass criterion, which must give the same verdict as
     the interpreter (the recompiled program must not do better or worse),
  2. in --differential lockstep against the interpreter.

The checklist ROM must also pass every one of its tests natively. Fail
closed on generation or build errors.

    python tools/snes/recomp_suite.py [--filter SUBSTR] [--jobs N] [--build DIR]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_snes_tests import DATA, EXE, REPO, discover, run_one  # noqa: E402

VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
LIB = ("{}.lib" if os.name == "nt" else "lib{}.a")


def tool_env() -> dict:
    """MSVC, CMake and Ninja on PATH on Windows; the inherited environment elsewhere."""
    if os.name != "nt" or shutil.which("cl"):
        return dict(os.environ)
    out = subprocess.run(f'"{VCVARS}" >nul && set', shell=True, capture_output=True, text=True).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    tools = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft"
    path_key = next((k for k in env if k.upper() == "PATH"), "PATH")
    env[path_key] = rf"{tools}\CMake\CMake\bin;{tools}\CMake\Ninja;" + env.get(path_key, "")
    return env


def build_one(rom: Path, name: str, out_root: Path, env: dict, build: Path) -> tuple[Path | None, str]:
    out = out_root / name
    gen = subprocess.run([str(build / "bin" / f"snesrecomp{EXE}"), str(rom), "-o", str(out), "--name", name],
                         capture_output=True, text=True, env=env)
    if gen.returncode:
        return None, "snesrecomp failed: " + (gen.stderr or gen.stdout).strip()[:300]
    libs = ";".join(str(build / "lib" / LIB.format(n)) for n in ("snesruntime", "snesapu", "snesppu"))
    path = next((v for k, v in env.items() if k.upper() == "PATH"), None)
    cmake = shutil.which("cmake", path=path) or "cmake"
    ninja = shutil.which("ninja", path=path) or "ninja"
    cfg = subprocess.run([cmake, "-G", "Ninja", "-S", str(out), "-B", str(out / "build"), "-DCMAKE_BUILD_TYPE=Release",
                          f"-DSNESRT_PREBUILT_LIBS={libs}", f"-DSNESRT_PREBUILT_INCLUDE={REPO / 'runtimes' / 'runtime-snes' / 'include'}"],
                         capture_output=True, text=True, env=env)
    if cfg.returncode:
        return None, "configure failed: " + (cfg.stdout[-400:] + cfg.stderr[-400:])
    bld = subprocess.run([ninja, "-C", str(out / "build")], capture_output=True, text=True, env=env)
    if bld.returncode:
        return None, "build failed: " + bld.stdout[-800:]
    return out / "build" / f"{name}{EXE}", ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--filter")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--diff-frames", type=int, default=600)
    parser.add_argument("--out", type=Path, default=REPO / "output" / "snes_suite")
    parser.add_argument("--build", type=Path, default=REPO / "build", help="root build directory")
    parser.add_argument("--data", type=Path, default=DATA)
    parser.add_argument("--no-checklist", action="store_true")
    args = parser.parse_args()
    build = args.build.resolve()
    out_root = args.out.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    env = tool_env()
    interp = build / "bin" / f"snes_run{EXE}"

    jobs = []
    for rom, run_args in discover(args.data, args.filter):
        rel = rom.relative_to(args.data).as_posix()
        # Short, identifier-safe names: MSVC and CMake's compiler checks fail
        # on paths beyond 260 characters, and the generated tree nests deep.
        stem = "".join(ch if ch.isalnum() else "_" for ch in rom.stem)[:20]
        name = f"t_{stem}_{hashlib.sha1(rel.encode()).hexdigest()[:6]}"
        jobs.append((rom, run_args, name, rel))
    checklist_json = None
    if not args.no_checklist and (not args.filter or args.filter in "checklist"):
        rom = out_root / "checklist.sfc"
        checklist_json = out_root / "checklist.json"
        make = subprocess.run([sys.executable, str(HERE / "checklist" / "make_checklist_rom.py"), "-o", str(rom),
                               "--json", str(checklist_json)], capture_output=True, text=True)
        if make.returncode:
            print("checklist ROM assembly failed: " + make.stderr)
            return 1
        jobs.append((rom, ["--frames", "1500", "--auto-input"], "checklist", "checklist.sfc"))
    if not jobs:
        print("no test ROMs selected (fail closed)")
        return 1

    def check(job):
        rom, run_args, name, rel = job
        exe, err = build_one(rom, name, out_root, env, build)
        if not exe:
            return rel, False, False, False, err
        if name == "checklist":
            state_i, state_n = out_root / "checklist_interp.state", out_root / "checklist_native.state"
            subprocess.run([str(interp), str(rom), *run_args, "--quiet", "--dump-state", str(state_i)], capture_output=True)
            subprocess.run([str(exe), *run_args, "--quiet", "--dump-state", str(state_n)], capture_output=True)
            verdict = [sys.executable, str(HERE / "checklist_results.py")]
            interp_ok = subprocess.run([*verdict, str(state_i), str(checklist_json)], capture_output=True).returncode == 0
            native = subprocess.run([*verdict, str(state_n), str(checklist_json)], capture_output=True, text=True)
            native_ok, detail = native.returncode == 0, native.stdout.strip().splitlines()[-1:]
            detail = detail[0] if detail and not native_ok else ""
        else:
            _, interp_ok, _ = run_one(interp, args.data, rom, run_args)
            _, native_ok, detail = run_one(exe, args.data, rom, run_args)
        frames = str(min(args.diff_frames, 1500))
        diff_args = ["--auto-input"] if name == "checklist" else []
        diff = subprocess.run([str(exe), "--differential", "--frames", frames, *diff_args], capture_output=True,
                              text=True, timeout=1800)
        diff_ok = diff.returncode == 0
        note = detail if native_ok != interp_ok or (name == "checklist" and not native_ok) else ""
        if not diff_ok:
            note += " " + " | ".join(line.strip() for line in diff.stdout.splitlines()[:3])
        native_line = next((line for line in diff.stdout.splitlines() if line.startswith("differential:")), "")
        return rel, interp_ok, native_ok, diff_ok, (note.strip() or native_line)

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(check, jobs))

    agree = diff_pass = 0
    failed = False
    for rel, interp_ok, native_ok, diff_ok, note in results:
        same = interp_ok == native_ok
        if rel == "checklist.sfc":
            same = same and native_ok
        agree += same
        diff_pass += diff_ok
        failed |= not (same and diff_ok)
        flag = "ok  " if same and diff_ok else "FAIL"
        print(f"{flag} {rel}  interp={'pass' if interp_ok else 'fail'} native={'pass' if native_ok else 'fail'} "
              f"diff={'ok' if diff_ok else 'DIVERGED'}{'  ' + note if note else ''}")
    total = len(results)
    print(f"\nresult parity {agree}/{total}, differential clean {diff_pass}/{total}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
