#!/usr/bin/env python3
"""Build our own Wii test programs into runnable ELFs.

These (tests/wii/programs/) are to the Wii what tests/gc/programs are to the
GameCube: programs of ours that exercise the machine around the processor -
MEM2, Hollywood's registers, IOS and its devices, the remotes - and print
their verdicts in the format tools/gc/run_gc_tests.py reads.

    python tools/wii/build_wii_programs.py
    python tools/wii/build_wii_programs.py --program wii_hello

Building them needs devkitPPC and libogc's Wii libraries. Without DEVKITPRO
set this exits non-zero and says what is missing, rather than quietly
building nothing.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SOURCE_DIR = REPO_ROOT / "tests" / "wii" / "programs"
# The verdict header is the GameCube programs': one format, one runner.
REPORT_DIR = REPO_ROOT / "tests" / "gc" / "programs"
DEFAULT_OUT = REPO_ROOT / "build" / "wii-programs"

# The apploader of the bootable test disc (tools/wii/make_boot_disc.py), and
# where a console puts one.
APPLOADER = SOURCE_DIR / "apploader" / "wii_apploader.c"
APPLOADER_ADDRESS = 0x81200000
# The programs a boot disc is made of, which it wants as DOLs.
DISC_PROGRAMS = ["wii_ditest"]

# Programs that include a file tools/fetch_wii_tests.py fetches, and the file.
FETCHED = {"wii_cputest": Path("achurch") / "ppc750cl.s"}

# Libraries a program asks for by naming them in a comment:
#   /* wii-libs: wiiuse bte */
LIBS_MARKER = "wii-libs:"


def fail(message: str) -> int:
    print(f"build_wii_programs: {message}", file=sys.stderr)
    return 1


def extra_libs(source: Path) -> list[str]:
    for text in source.read_text(encoding="utf-8").splitlines():
        if LIBS_MARKER in text:
            names = text.split(LIBS_MARKER, 1)[1].replace("*/", " ").split()
            return [f"-l{name}" for name in names]
    return []


def build_disc_parts(devkitpro: Path, devkitppc: Path, out: Path) -> str | None:
    """The apploader (apploader.bin, apploader.entry) and each disc program's DOL."""
    suffix = ".exe" if os.name == "nt" else ""
    binaries = devkitppc / "bin"
    gcc = binaries / f"powerpc-eabi-gcc{suffix}"
    objcopy = binaries / f"powerpc-eabi-objcopy{suffix}"
    nm = binaries / f"powerpc-eabi-nm{suffix}"
    elf2dol = devkitpro / "tools" / "bin" / f"elf2dol{suffix}"
    for tool in (objcopy, nm, elf2dol):
        if not tool.exists():
            return f"{tool} does not exist"

    def run(command: list[str]) -> subprocess.CompletedProcess:
        return subprocess.run(command, capture_output=True, text=True)

    # No library, no small-data areas (nothing has set the registers they
    # hang off), linked where the console puts one, and with its sections
    # packed together rather than a page apart.
    elf = out / "apploader.elf"
    built = run([str(gcc), "-Os", "-ffreestanding", "-nostdlib", "-mcpu=750", "-meabi",
                 "-msdata=none", "-fno-toplevel-reorder", "-Wall", "-Wextra",
                 f"-Wl,-Ttext,0x{APPLOADER_ADDRESS:08x}", "-Wl,-e,app_entry", "-Wl,--build-id=none",
                 "-Wl,-N", str(APPLOADER), "-o", str(elf)])
    if built.returncode != 0:
        return f"the apploader would not compile\n{built.stdout}{built.stderr}"
    copied = run([str(objcopy), "-O", "binary", "-j", ".text", "-j", ".rodata", "-j", ".rodata.str1.4",
                  "-j", ".data", str(elf), str(out / "apploader.bin")])
    if copied.returncode != 0:
        return f"objcopy failed\n{copied.stderr}"
    symbols = run([str(nm), str(elf)])
    entry = re.search(r"^([0-9a-f]{8}) T app_entry$", symbols.stdout, re.MULTILINE)
    if not entry:
        return "the apploader has no app_entry"
    # Data the image needs and objcopy left out would only show as a crash
    # on boot, so it is refused here.
    bss = re.findall(r"^[0-9a-f]{8} [bB] (\w+)$", symbols.stdout, re.MULTILINE)
    if bss:
        return f"the apploader has uninitialised data, which a disc would not hold: {', '.join(bss)}"
    (out / "apploader.entry").write_text(entry.group(1) + "\n")
    print(f"  -> {out / 'apploader.bin'} ({(out / 'apploader.bin').stat().st_size} bytes, entry {entry.group(1)})")

    for name in DISC_PROGRAMS:
        program = out / f"{name}.elf"
        if not program.exists():
            continue
        converted = run([str(elf2dol), str(program), str(out / f"{name}.dol")])
        if converted.returncode != 0:
            return f"elf2dol failed on {name}\n{converted.stdout}{converted.stderr}"
        print(f"  -> {out / (name + '.dol')}")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=SOURCE_DIR)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--program", action="append", help="build only this one (repeatable)")
    parser.add_argument("--data", type=Path, default=REPO_ROOT / "roms" / "wii",
                        help="where tools/fetch_wii_tests.py put its suites")
    parser.add_argument("--no-gamecube", action="store_true",
                        help="leave out tests/gc/programs, which are built for the Wii as well")
    args = parser.parse_args()

    devkitpro = os.environ.get("DEVKITPRO")
    devkitppc = os.environ.get("DEVKITPPC") or (devkitpro and str(Path(devkitpro, "devkitPPC")))
    if not devkitpro or not devkitppc:
        return fail(
            "DEVKITPRO must be set (and DEVKITPPC, if it is not $DEVKITPRO/devkitPPC). "
            "These are Wii programs; see https://devkitpro.org/wiki/Getting_Started"
        )

    gcc = Path(devkitppc, "bin", "powerpc-eabi-gcc")
    if os.name == "nt":
        gcc = gcc.with_suffix(".exe")
    if not gcc.exists():
        return fail(f"{gcc} does not exist")

    libogc = Path(devkitpro, "libogc")
    if not (libogc / "lib" / "wii" / "libogc.a").exists():
        return fail(f"libogc's Wii libraries are missing from {libogc}")

    # The Wii's own programs, and the GameCube's built for a Wii: the two
    # consoles share everything those exercise, so each must give the same
    # verdicts - and the drawing ones the same picture - on either.
    sources = sorted(args.source.glob("wii_*.c"))
    if not args.no_gamecube:
        sources += sorted(REPORT_DIR.glob("gc_*.c"))
    if args.program:
        wanted = {name if name.endswith(".c") else name + ".c" for name in args.program}
        sources = [s for s in sources if s.name in wanted]
        if len(sources) != len(wanted):
            missing = wanted - {s.name for s in sources}
            return fail(f"no such program in {args.source}: {', '.join(sorted(missing))}")
    if not sources:
        return fail(f"no programs in {args.source}")

    args.out.mkdir(parents=True, exist_ok=True)
    for source in sources:
        elf = args.out / (source.stem + ".elf")
        include = []
        if source.stem in FETCHED:
            # A program that is mostly a fetched file is skipped, with a
            # note, when the file has not been fetched.
            needed = args.data / FETCHED[source.stem]
            if not needed.exists():
                print(f"  {source.name}: skipped, {needed} is not there "
                      f"(tools/fetch_wii_tests.py --suite {FETCHED[source.stem].parts[0]})")
                elf.unlink(missing_ok=True)
                continue
            include = [f"-Wa,-I{needed.parent}"]
        command = [
            str(gcc), "-g", "-O2", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float",
            "-DGEKKO", "-DHW_RVL", "-Wall", "-Wextra",
            f"-I{args.source}", f"-I{args.source.parent}", f"-I{REPORT_DIR}", f"-I{libogc / 'include'}",
            str(source),
            *include, f"-L{libogc / 'lib' / 'wii'}", *extra_libs(source), "-logc", "-lm",
            "-o", str(elf),
        ]
        print(" ".join(command), flush=True)
        result = subprocess.run(command)
        if result.returncode != 0:
            return fail(f"{source.name}: the compiler exited with {result.returncode}")
        print(f"  -> {elf} ({elf.stat().st_size / 1e6:.2f} MB)")

    # What a disc that boots is made of: the apploader's code and its entry
    # point, and the programs that go on one as DOLs. Built here so that
    # making the disc - which a test does - needs no devkitPPC.
    if not args.program:
        problem = build_disc_parts(Path(devkitpro), Path(devkitppc), args.out)
        if problem:
            return fail(problem)

    # One of the GameCube programs reads a disc; the image is the same one.
    disc = subprocess.run(
        [sys.executable, str(REPO_ROOT / "tools" / "gc" / "make_test_disc.py"),
         "--out", str(args.out / "test.gcm")], stdout=subprocess.DEVNULL)
    if disc.returncode != 0:
        return fail("make_test_disc.py failed")

    print(f"build_wii_programs: {len(sources)} program{'' if len(sources) == 1 else 's'} in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
