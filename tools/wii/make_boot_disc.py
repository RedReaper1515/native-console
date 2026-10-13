#!/usr/bin/env python3
"""Build a Wii disc that boots one of our test programs.

No retail disc is in this repository, and a Wii's whole way of starting a
game - an encrypted partition, the disc's own apploader, a DOL - has to be
tested all the same. This builds a disc that goes through every step of it:

  * tests/wii/programs/apploader/wii_apploader.c is the apploader;
  * a test program from tests/wii/programs is the game;
  * build/bin/wii_make_test_disc puts both on the synthetic disc
    (tests/wii/wii_test_disc.h), encrypted with the keys made up for it.

    python tools/wii/make_boot_disc.py --program wii_ditest

writes build/wii-programs/wii_ditest.wbfs and, beside it,
wii-common-key.bin holding the made-up common key - which is where the disc
reader looks, so `gc_run build/wii-programs/wii_ditest.wbfs` boots it.

Needs what tools/wii/build_wii_programs.py leaves (the apploader's code and
entry point, and the program as a DOL) and the wii_make_test_disc target;
nothing here needs devkitPPC.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
# The made-up key the test disc is built around (tests/wii/wii_test_disc.h).
TEST_COMMON_KEY = b"NC-test-common-k"


def fail(message: str) -> int:
    print(f"make_boot_disc: {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", default="wii_ditest", help="the test program that is the game")
    parser.add_argument("--programs", type=Path, default=REPO_ROOT / "build" / "wii-programs")
    parser.add_argument("--tool", type=Path, default=REPO_ROOT / "build" / "bin" / "wii_make_test_disc")
    parser.add_argument("--container", choices=["iso", "ciso", "wbfs"], default="wbfs")
    parser.add_argument("--out", type=Path, help="the disc (default: beside the program)")
    args = parser.parse_args()

    tool = args.tool
    if not tool.exists() and tool.with_suffix(".exe").exists():
        tool = tool.with_suffix(".exe")
    if not tool.exists():
        return fail(f"{tool} does not exist; build the wii_make_test_disc target")

    apploader = args.programs / "apploader.bin"
    entry = args.programs / "apploader.entry"
    dol = args.programs / f"{args.program}.dol"
    for needed in (apploader, entry, dol):
        if not needed.exists():
            return fail(f"{needed} does not exist; run tools/wii/build_wii_programs.py")

    out = args.out or args.programs / f"{args.program}.{args.container}"
    out.parent.mkdir(parents=True, exist_ok=True)
    command = [str(tool), str(out), "--apploader", str(apploader), entry.read_text().strip(),
               "--dol", str(dol)]
    if args.container == "ciso":
        command += ["--ciso", "21"]
    elif args.container == "wbfs":
        command += ["--wbfs", "21"]
    made = subprocess.run(command, capture_output=True, text=True)
    if made.returncode != 0:
        return fail(f"the disc was not written\n{made.stdout}{made.stderr}")

    (out.parent / "wii-common-key.bin").write_bytes(TEST_COMMON_KEY)
    print(f"make_boot_disc: {out} ({out.stat().st_size / 1e6:.1f} MB): apploader "
          f"{apploader.stat().st_size} bytes at {entry.read_text().strip()}, DOL {dol.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
