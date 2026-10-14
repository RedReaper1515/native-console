#!/usr/bin/env python3
"""Recompile the N64 test ROMs, build each one, and check it two ways.

For every ROM run_n64_tests.py runs (or a --filter of them): run n64recomp,
build the generated project against the runtime libraries of a root build,
then run the executable

  1. with the suite's own verdict, which must be the interpreter's verdict
     (the recompiled program must do no better and no worse), and
  2. in --differential lockstep against the interpreter.

Fail closed on generation, build or run errors.

    python3 tools/n64/recomp_suite.py --build build [--filter SUBSTR] [--jobs N]
"""
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
from run_n64_tests import DEFAULT_DATA, KROM_DIRS, krom_verdict  # noqa: E402

ROOT = HERE.parents[1]
EXE = ".exe" if os.name == "nt" else ""
LIB = "{}.lib" if os.name == "nt" else "lib{}.a"


def check(rom: Path, out_root: Path, build: Path, frames: int, diff_frames: int):
    name = "t" + hashlib.sha1(str(rom).encode()).hexdigest()[:10]
    out = out_root / name
    shutil.rmtree(out, ignore_errors=True)
    r = subprocess.run([str(build / "bin" / f"n64recomp{EXE}"), str(rom), "-o", str(out), "--name", name],
                       capture_output=True, text=True)
    if r.returncode:
        return "error", "n64recomp: " + (r.stderr or r.stdout)[-300:]
    libs = ";".join(str(build / "lib" / LIB.format(n)) for n in ("n64runtime", "n64rsp", "n64rdp"))
    r = subprocess.run(["cmake", "-G", "Ninja", "-S", str(out), "-B", str(out / "build"), "-DCMAKE_BUILD_TYPE=Release",
                        f"-DN64RT_PREBUILT_LIBS={libs}", f"-DN64RT_PREBUILT_INCLUDE={ROOT / 'runtimes' / 'runtime-n64' / 'include'}"],
                       capture_output=True, text=True)
    if r.returncode:
        return "error", "configure: " + r.stdout[-300:] + r.stderr[-300:]
    r = subprocess.run(["ninja", "-C", str(out / "build")], capture_output=True, text=True)
    if r.returncode:
        return "error", "build: " + r.stdout[-600:]
    game = out / "build" / f"{name}{EXE}"
    frame_rc = out / "rc.ppm"
    frame_in = out / "in.ppm"
    r1 = subprocess.run([str(game), "--frames", str(frames), "--dump-frame", str(frame_rc), "--quiet"],
                        capture_output=True, text=True, timeout=600)
    r2 = subprocess.run([str(build / "bin" / f"n64_run{EXE}"), str(rom), "--frames", str(frames), "--dump-frame",
                         str(frame_in), "--quiet"], capture_output=True, text=True, timeout=600)
    if r1.returncode or r2.returncode or not frame_rc.exists() or not frame_in.exists():
        return "error", "run failed"
    v_rc, v_in = krom_verdict(frame_rc)[0], krom_verdict(frame_in)[0]
    if v_rc != v_in:
        return "mismatch", f"recompiled {v_rc}, interpreted {v_in}"
    same_frame = frame_rc.read_bytes() == frame_in.read_bytes()
    r = subprocess.run([str(game), "--frames", str(diff_frames), "--differential"], capture_output=True, text=True,
                       timeout=1200)
    if r.returncode or "no divergence" not in r.stdout:
        return "diverged", r.stdout[-1200:]
    native = r.stdout.split("(")[1].split(")")[0] if "(" in r.stdout else "?"
    shutil.rmtree(out, ignore_errors=True)
    return "ok", f"{v_rc}, frame {'identical' if same_frame else 'DIFFERENT'}, {native}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--data", type=Path, default=DEFAULT_DATA)
    ap.add_argument("--out", type=Path, default=ROOT / "output" / "n64_suite")
    ap.add_argument("--filter", default="")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--diff-frames", type=int, default=60)
    ap.add_argument("-j", "--jobs", type=int, default=4)
    args = ap.parse_args()
    build = args.build.resolve()
    roms = []
    for d in KROM_DIRS:
        base = args.data / d
        if base.is_dir():
            roms += sorted(base.rglob("*.N64"))
    roms = [r for r in roms if args.filter in str(r)]
    if not roms:
        print("FAIL: no test ROMs selected")
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(check, r, args.out, build, args.frames, args.diff_frames): r for r in roms}
        for f in concurrent.futures.as_completed(futures):
            rom = futures[f]
            try:
                status, detail = f.result()
            except Exception as e:  # fail closed
                status, detail = "error", str(e)
            print(f"{status:9} {rom.relative_to(args.data)}  {detail}", flush=True)
            if status != "ok" or "DIFFERENT" in detail:
                bad += 1
    print(f"{len(roms) - bad} of {len(roms)} ROMs: same verdict and frame recompiled as interpreted, differential clean")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
