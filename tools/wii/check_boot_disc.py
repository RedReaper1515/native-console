#!/usr/bin/env python3
"""A Wii disc boots, interpreted and recompiled.

The whole path a game takes, on a disc built here (tools/wii/make_boot_disc.py):

  1. the disc is written as WBFS, ISO and CISO, and each boots under gc_run
     through its encrypted partition and its apploader, with every verdict
     of the program on it a pass;
  2. gcrecomp is given the disc, with the common key beside it, and writes a
     project that carries the disc's own title key;
  3. the built program is run with the disc beside it as ID.gcdata and no
     key anywhere - not beside the disc, not in the environment - and has to
     give the same verdicts, and agree with the interpreter under
     --differential.

Needs a compiler for step 2 (run it from the build environment, as the other
recompile checks are) and the parts tools/wii/build_wii_programs.py leaves.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
VERDICT = re.compile(r"^Test (\d+) (passed|failed) \((\d+) subtests(?:, (\d+) failures)?\)")
PROGRAM = "wii_ditest"
GAME = "wii_boot_disc"


def verdicts(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines() if VERDICT.match(line.strip())]


def all_pass(lines: list[str]) -> bool:
    return bool(lines) and all(" passed " in line for line in lines)


def run(command: list[str], env: dict | None = None, cwd: Path | None = None, timeout: int = 900):
    return subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=env, cwd=cwd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gc-run", type=Path, default=REPO_ROOT / "build" / "bin" / "gc_run")
    parser.add_argument("--gcrecomp", type=Path, default=REPO_ROOT / "build" / "bin" / "gcrecomp")
    parser.add_argument("--tool", type=Path, default=REPO_ROOT / "build" / "bin" / "wii_make_test_disc")
    parser.add_argument("--programs", type=Path, default=REPO_ROOT / "build" / "wii-programs")
    parser.add_argument("--work", type=Path, default=REPO_ROOT / "build" / "wii-boot-disc")
    parser.add_argument("--frames", type=int, default=400)
    args = parser.parse_args()

    def exe(path: Path) -> Path:
        return path.with_suffix(".exe") if not path.exists() and path.with_suffix(".exe").exists() else path

    gc_run, gcrecomp, tool = exe(args.gc_run), exe(args.gcrecomp), exe(args.tool)
    elf = args.programs / f"{PROGRAM}.elf"
    for needed in (gc_run, gcrecomp, tool, elf):
        if not needed.exists():
            print(f"check_boot_disc: {needed} does not exist", file=sys.stderr)
            return 1

    # No key from outside: what is found has to be what this put there.
    env = {k: v for k, v in os.environ.items() if k not in ("WII_COMMON_KEY", "WII_KEY_FILE")}
    shutil.rmtree(args.work, ignore_errors=True)
    discs = args.work / "discs"
    discs.mkdir(parents=True)
    failed = 0

    # 1. Every container boots, interpreted.
    reference: list[str] = []
    for container in ("wbfs", "iso", "ciso"):
        disc = discs / f"{PROGRAM}.{container}"
        made = run([sys.executable, str(REPO_ROOT / "tools" / "wii" / "make_boot_disc.py"),
                    "--program", PROGRAM, "--programs", str(args.programs), "--tool", str(tool),
                    "--container", container, "--out", str(disc)], env=env)
        if made.returncode != 0:
            print(f"check_boot_disc: the {container} disc was not made\n{made.stdout}{made.stderr}")
            return 1
        started = time.monotonic()
        ran = run([str(gc_run), str(disc), "--symbols", str(elf), "--frames", str(args.frames), "--quiet"],
                  env=env)
        lines = verdicts(ran.stdout)
        ok = all_pass(lines)
        print(f"{container:<5} interpreted: {len(lines)} tests, {'all pass' if ok else 'NOT all passing'} "
              f"({time.monotonic() - started:.1f} s)")
        if not ok:
            failed += 1
            print(ran.stdout[-2000:] + ran.stderr[-2000:])
        if container == "wbfs":
            reference = lines
        elif lines != reference:
            failed += 1
            print(f"      the {container} disc's verdicts differ from the WBFS disc's")

    # 2. Recompile from the disc, whose key is beside it.
    project = args.work / "project"
    started = time.monotonic()
    made = run([str(gcrecomp), str(discs / f"{PROGRAM}.wbfs"), "-o", str(project), "--name", GAME], env=env)
    if made.returncode != 0 or ", Wii," not in made.stdout:
        print(f"check_boot_disc: gcrecomp did not take the disc as a Wii game\n{made.stdout}{made.stderr}")
        return 1
    configured = run(["cmake", "-G", "Ninja", "-B", "build", "-DCMAKE_BUILD_TYPE=Release",
                      "-DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON"], env=env, cwd=project)
    built = run(["cmake", "--build", "build"], env=env, cwd=project) if configured.returncode == 0 else configured
    if built.returncode != 0:
        print("check_boot_disc: the generated project would not build")
        print("\n".join((built.stdout + built.stderr).splitlines()[-25:]))
        return 1
    print(f"recompiled from the disc and built ({time.monotonic() - started:.1f} s)")

    # 3. The game with its disc beside it and no key anywhere.
    game = project / "build" / (GAME + (".exe" if sys.platform == "win32" else ""))
    shutil.copyfile(discs / f"{PROGRAM}.wbfs", project / "build" / f"{GAME}.gcdata")
    started = time.monotonic()
    ran = run([str(game), "--symbols", str(elf), "--frames", str(args.frames)], env=env)
    lines = verdicts(ran.stdout)
    ok = all_pass(lines) and lines == reference
    print(f"recompiled, with only the key it carries: {len(lines)} tests, "
          f"{'all pass, as interpreted' if ok else 'NOT as interpreted'} ({time.monotonic() - started:.1f} s)")
    if not ok:
        failed += 1
        print(ran.stdout[-2000:] + ran.stderr[-2000:])

    started = time.monotonic()
    ran = run([str(game), "--symbols", str(elf), "--frames", "1", "--differential"], env=env)
    agree = "agree at the end of every one of" in ran.stderr
    print(f"differential over the boot and the tests: {'agrees' if agree else 'DIVERGED'} "
          f"({time.monotonic() - started:.1f} s)")
    if not agree:
        failed += 1
        print(ran.stderr[-3000:])

    if failed:
        print(f"\ncheck_boot_disc: {failed} step(s) failed")
        return 1
    print("\ncheck_boot_disc: the disc boots in every container, interpreted and recompiled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
