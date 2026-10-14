#!/usr/bin/env python3
"""The N64 recompiler end to end on the repository's own test ROM.

1. tests/fixtures/make_n64_rom.py writes the ROM;
2. n64_run (the interpreter) must pass it (r30 = -1);
3. n64recomp builds a project three times - from the boot image alone, with
   a profile of the ROM (which adds the overlays it DMAs in), and from the
   trace the boot build records while it runs (--learn-out, what a game's
   window does) - and each build must pass natively, report a native
   share, and run clean in --differential lockstep with the interpreter.

The generated projects link the runtime libraries of this build
(N64RT_PREBUILT_LIBS), so only the generated code is compiled. Fail closed.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n64recomp", required=True)
    ap.add_argument("--n64run", required=True)
    ap.add_argument("--libdir", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args()
    work = Path(args.work)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    rom = work / "fixture.z64"
    r = run([sys.executable, ROOT / "tests" / "fixtures" / "make_n64_rom.py", "--output", rom])
    if r.returncode:
        print(r.stdout, r.stderr)
        return 1
    verdict = ["--frames", "30", "--until-gpr", "r30=-1"]
    r = run([args.n64run, rom, *verdict])
    print(r.stdout)
    if r.returncode or "PASS" not in r.stdout:
        print("FAIL: the interpreter does not pass the fixture")
        return 1

    lib = "{}.lib" if os.name == "nt" else "lib{}.a"
    libs = ";".join(str(Path(args.libdir) / lib.format(n)) for n in ("n64runtime", "n64rsp", "n64rdp"))
    exe = ".exe" if os.name == "nt" else ""
    learned = work / "learned.trace"
    variants = (("boot", []), ("profiled", ["--profile", work / "profile.txt", "--profile-frames", "30"]),
                ("learned", ["--trace", learned]))
    native_share = {}
    for variant, extra in variants:
        out = work / variant
        if variant == "learned":
            # What a game's window does while it is played: the boot build
            # notes the code its interpreter ran. A second session must
            # keep what the first recorded, once.
            counts = []
            for session in (1, 2):
                r = run([work / "boot" / "build" / ("fixture" + exe), "--frames", "30", "--learn-out", learned])
                if r.returncode or not learned.exists():
                    print(r.stdout, r.stderr)
                    print(f"FAIL: --learn-out, session {session}")
                    return 1
                lines = learned.read_text().splitlines()
                counts.append((sum(l.startswith("E ") for l in lines), sum(l.startswith("P ") for l in lines),
                               sum(l.startswith("F ") for l in lines)))
            print(f"learned trace: entries, pages, image pages = {counts[0]} then {counts[1]}")
            if not counts[0][0] or not counts[0][1] or not counts[0][2] or counts[0] != counts[1]:
                print("FAIL: the play trace is empty, or a second session changed what the first recorded")
                return 1
        r = run([args.n64recomp, rom, "-o", out, "--name", "fixture", *extra])
        print(r.stdout, r.stderr)
        if r.returncode:
            print(f"FAIL: n64recomp ({variant})")
            return 1
        r = run(["cmake", "-G", "Ninja", "-S", out, "-B", out / "build", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DN64RT_PREBUILT_LIBS={libs}", f"-DN64RT_PREBUILT_INCLUDE={ROOT / 'runtimes' / 'runtime-n64' / 'include'}"])
        if r.returncode:
            print(r.stdout[-2000:], r.stderr[-2000:])
            print(f"FAIL: configure ({variant})")
            return 1
        r = run(["ninja", "-C", out / "build"])
        if r.returncode:
            print(r.stdout[-4000:])
            print(f"FAIL: build ({variant})")
            return 1
        game = out / "build" / ("fixture" + exe)
        r = run([game, *verdict, "--coverage"])
        print(r.stdout)
        m = re.search(r"native=([0-9.]+)%", r.stdout)
        if r.returncode or "PASS" not in r.stdout or not m:
            print(f"FAIL: the recompiled fixture ({variant}) does not pass")
            return 1
        native_share[variant] = float(m.group(1))
        if variant == "profiled" and float(m.group(1)) < 99.0:
            print(f"FAIL: with a profile, only {m.group(1)}% ran natively")
            return 1
        if variant == "learned" and native_share["learned"] <= native_share["boot"]:
            print(f"FAIL: a build from the play trace ran {m.group(1)}% natively, no more than the boot build")
            return 1
        # Every entry the profile saw (branch targets, exception vectors,
        # where ERET went) must be compiled.
        if variant == "profiled" and "interpreter entries: 0 distinct" not in r.stdout:
            print("FAIL: with a profile, code still entered the interpreter")
            return 1
        r = run([game, "--frames", "30", "--differential"])
        print(r.stdout[-3000:])
        if r.returncode or "no divergence" not in r.stdout:
            print(f"FAIL: --differential ({variant})")
            return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
