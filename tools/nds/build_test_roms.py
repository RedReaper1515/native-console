#!/usr/bin/env python3
"""Build the Nintendo DS test ROMs that are fetched as source only.

tools/fetch_nds_tests.py leaves three suites in roms/nds/ with no ROM in
them. This script builds them in the BlocksDS Docker image into

    roms/nds/built/<suite>/<name>.nds        the ROMs
    roms/nds/built/<suite>/elf/              their ELF files, for symbols
    roms/nds/built/sdk-arm7/                 the SDK's stock ARM7 programs
                                             that most of the ROMs carry
    roms/nds/built/generated/                input this script generates (the
                                             file tree of one test)
    roms/nds/built/logs/<suite>/<name>.log   one build log per ROM
    roms/nds/built/MANIFEST.json             per ROM: suite, source commit,
                                             license, SHA-256, image ID

    python tools/nds/build_test_roms.py                    # whatever is stale
    python tools/nds/build_test_roms.py --suite arm7wrestler
    python tools/nds/build_test_roms.py --rom system_swi_calls --force
    python tools/nds/build_test_roms.py --list

The suites:

    blocksds-tests  the BlocksDS SDK's hardware tests, one ROM per
                    tests/<group>/<test>/, named <group>_<test>. Each has its
                    own Makefile for the SDK and is built with it unchanged.
    arm7wrestler    armwrestler running on the ARM7. Its Makefiles are for
                    devkitARM; it is built with the SDK's two-CPU template.
    striker-tests   three 2D window tests, also written for devkitARM, built
                    with the SDK's single-CPU template.

Docker must be running with the image already pulled; nothing is pulled or
installed. Each fetched suite is mounted read-only and copied to a scratch
directory inside the container, where everything is built, so roms/nds/<suite>
is never written to. What the devkitARM suites need (wrapper Makefiles and two
small glue files) is written into that scratch copy; see WRAPPERS below.

A ROM is up to date when its file still has the SHA-256 the manifest records
and nothing it was built from has changed: its sources, the suite's commit,
its recipe, or the image. --force rebuilds regardless.

The run fails closed. A suite whose files differ from roms/nds/MANIFEST.json
(changed, missing, or with files the fetch did not put there) is not built.
A ROM that fails to build is removed from the output and the manifest, its log
is kept, and the exit code is 1. ROMs listed in NOT_BUILDABLE cannot be built
with the pinned image; they are named with the reason, skipped unless asked
for with --rom, and are not errors.

No-license suites (arm7wrestler, striker-tests) are for local runs only.
Everything this script writes is under roms/, which is gitignored.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_ROMS = REPO_ROOT / "roms" / "nds"
DEFAULT_IMAGE = "skylyrac/blocksds:slim-latest"

# Part of every ROM's fingerprint: raise it to force everything to rebuild
# after a change here that the recipes' own text does not show.
RECIPE_VERSION = 1

# ROMs the pinned image (BlocksDS v1.24.0) cannot build, with the reason.
NOT_BUILDABLE = {
    "blocksds-tests/maxmod_nitrofs_with_header": (
        "needs a BlocksDS release newer than v1.24.0: the test was added to the SDK after it and "
        "calls mmGetModuleNameById, mmGetSampleNameById, mmGetModuleIdByName and mmGetSampleIdByName, "
        "which that release's Maxmod does not have"),
}

# --- What the devkitARM suites need to build with the BlocksDS SDK -----------
#
# Both suites were written for devkitARM, whose start-up code enters main()
# with "bx" from ARM code. libnds in BlocksDS calls main() from Thumb code on
# the ARM9, and the linker can only turn that call into an interworking one if
# main is marked as a function. The assembly programs here define main as a
# bare ARM label, so their ARM9 side is linked with --wrap=main and the entry
# below, which is marked, jumps to the real one.
GLUE_MAIN_ENTRY = """\
@ Written by tools/nds/build_test_roms.py; not part of the fetched source.
@ An ARM entry the linker can interwork to, for a main that is a bare label.
    .arm
    .global __wrap_main
    .type __wrap_main, %function
__wrap_main:
    b __real_main
"""

# arm7wrestler's ARM7 program contains ARMv5TE instructions on purpose (it
# checks that the ARM7 rejects them), so it has to be assembled as ARMv5TE.
# The object must still be marked ARMv4T, or the linker would feel free to use
# BLX anywhere in the ARM7 binary.
GLUE_ARM7_V4T = """\
@ Written by tools/nds/build_test_roms.py; not part of the fetched source.
    .eabi_attribute Tag_CPU_name, "ARM7TDMI"
    .eabi_attribute Tag_CPU_arch, 2
"""

WRAPPERS = {
    "arm7wrestler": {
        "Makefile.blocksds": """\
# Written by tools/nds/build_test_roms.py; not part of the fetched source.
BLOCKSDS	?= /opt/blocksds/core
NAME		:= arm7wrestler
GAME_TITLE	:= ARM7Wrestler
GAME_SUBTITLE	:= Arisotura/arm7wrestler
include $(BLOCKSDS)/sys/default_makefiles/rom_arm9arm7/Makefile
""",
        # The sources have no .arm directive: devkitARM assembled them as ARM.
        "Makefile.arm9": """\
# Written by tools/nds/build_test_roms.py; not part of the fetched source.
BLOCKSDS	?= /opt/blocksds/core
SOURCEDIRS	:= arm9/source nc_glue/arm9
ASFLAGS		:= -marm
LDFLAGS		:= -Wl,--wrap=main
include $(BLOCKSDS)/sys/default_makefiles/rom_arm9arm7/Makefile.arm9
""",
        # -I is for the font: the source includes "../data/font8x8.pat",
        # relative to the build directory devkitARM's Makefile ran in.
        "Makefile.arm7": """\
# Written by tools/nds/build_test_roms.py; not part of the fetched source.
BLOCKSDS	?= /opt/blocksds/core
INCLUDEDIRS	:= arm7/source
ASFLAGS		:= -marm -Wa,-march=armv5te -include nc_glue/arm7_v4t.inc
include $(BLOCKSDS)/sys/default_makefiles/rom_arm9arm7/Makefile.arm7
""",
        "nc_glue/arm9/main_entry.s": GLUE_MAIN_ENTRY,
        "nc_glue/arm7_v4t.inc": GLUE_ARM7_V4T,
    },
    # The devkitARM Makefile compiled these as ARM code at -O3; the template's
    # own flags (Thumb, -O2) come first on the command line and these win.
    "striker-c++": {
        "Makefile.blocksds": """\
# Written by tools/nds/build_test_roms.py; not part of the fetched source.
BLOCKSDS	?= /opt/blocksds/core
NAME		:= @NAME@
GAME_TITLE	:= Window Tests
GAME_SUBTITLE	:= StrikerX3/nds-tests @NAME@
SOURCEDIRS	:= source
CFLAGS		:= -marm -O3
CXXFLAGS	:= -marm -O3
include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
""",
    },
    "striker-asm": {
        "Makefile.blocksds": """\
# Written by tools/nds/build_test_roms.py; not part of the fetched source.
BLOCKSDS	?= /opt/blocksds/core
NAME		:= @NAME@
GAME_TITLE	:= Window Tests
GAME_SUBTITLE	:= StrikerX3/nds-tests @NAME@
SOURCEDIRS	:= source nc_glue
ASFLAGS		:= -marm
LDFLAGS		:= -Wl,--wrap=main
include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
""",
        "nc_glue/main_entry.s": GLUE_MAIN_ENTRY,
    },
}

# filesystem/stress_test fills its NitroFS with gen_filesystem.py, which needs
# python3 (the image has none) and draws from os.urandom, so no two builds of
# it are alike. The same tree is generated here instead, from this seed: 20
# directories nested at random, and 100 files of 1 to 256 KiB named after the
# MD5 of their contents, which is what the test checks.
STRESS_SEED = b"native-console nds stress_test 1"
STRESS_FOLDERS = 20
STRESS_FILES = 100


class BuildError(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --- Docker -----------------------------------------------------------------


def docker_env() -> dict:
    # A Python started from Git Bash is a native program, so its arguments to
    # docker are not rewritten. These two cover an MSYS Python, which would
    # turn "/src" into a Windows path.
    env = dict(os.environ)
    env["MSYS_NO_PATHCONV"] = "1"
    env["MSYS2_ARG_CONV_EXCL"] = "*"
    return env


def image_identity(image: str) -> dict:
    try:
        result = subprocess.run(["docker", "image", "inspect", "--format", "{{.Id}}|{{json .RepoDigests}}", image],
                                capture_output=True, text=True, timeout=120, env=docker_env())
    except FileNotFoundError:
        raise BuildError("docker was not found on PATH") from None
    except subprocess.TimeoutExpired:
        raise BuildError("docker did not answer") from None
    if result.returncode != 0:
        raise BuildError(f"Docker image {image} is not available ({result.stderr.strip()}). "
                         f"Start Docker and run: docker pull {image}")
    image_id, _, digests = result.stdout.strip().partition("|")
    return {"name": image, "id": image_id, "repo_digests": json.loads(digests or "[]") or []}


def bind(source: Path, target: str, readonly: bool) -> list:
    # --mount takes a Windows path as it is, spaces included; -v would need
    # the drive colon told apart from its own separators.
    fields = ["type=bind", f"source={source.resolve()}", f"target={target}"]
    if readonly:
        fields.append("readonly")
    quoted = ['"' + field.replace('"', '""') + '"' if ("," in field or '"' in field) else field
              for field in fields]
    return ["--mount", ",".join(quoted)]


# --- The ROMs ---------------------------------------------------------------


class Rom:
    def __init__(self, suite: str, name: str, source: str, steps: list, wrappers: dict | None = None,
                 generated: str | None = None, note: str | None = None):
        self.suite = suite          # directory under roms/nds/
        self.name = name            # the ROM is built/<suite>/<name>.nds
        self.source = source        # its directory inside the suite, POSIX style
        self.steps = steps          # shell commands, run in that directory
        self.wrappers = wrappers or {}  # files written there first
        self.generated = generated  # a generated input tree to copy in, or None
        self.note = note

    @property
    def key(self) -> str:
        return f"{self.suite}/{self.name}"


def discover(roms_dir: Path) -> list:
    """Every ROM this script knows how to build, in build order."""
    roms = []
    tests = roms_dir / "blocksds-tests" / "tests"
    if tests.is_dir():
        for makefile in sorted(tests.glob("*/*/Makefile")):
            group, test = makefile.parent.parent.name, makefile.parent.name
            rom = Rom("blocksds-tests", f"{group}_{test}", f"tests/{group}/{test}",
                      ["make -j$(nproc) VERBOSE=1"])
            if (group, test) == ("filesystem", "stress_test"):
                rom.generated = "nitrofs"
                rom.note = ("NitroFS contents generated by this script from a fixed seed, "
                            "in place of gen_filesystem.py (needs python3, and is random)")
            roms.append(rom)

    if (roms_dir / "arm7wrestler").is_dir():
        roms.append(Rom(
            "arm7wrestler", "arm7wrestler", ".",
            ["make -f Makefile.blocksds -j$(nproc) VERBOSE=1",
             # The check the glue is there for: the ARM7 binary is an ARMv4T one.
             "arm-none-eabi-readelf -A build/arm7.elf | grep -q 'Tag_CPU_arch: v4T$'",
             "arm-none-eabi-nm build/arm9.elf | grep -q ' T __wrap_main$'"],
            WRAPPERS["arm7wrestler"],
            note="built with the BlocksDS two-CPU template (crt0 and libnds on both CPUs) in place of devkitARM's"))

    window = roms_dir / "striker-tests" / "window"
    if window.is_dir():
        for directory in sorted(path for path in window.iterdir() if path.is_dir()):
            is_asm = any(directory.glob("source/*.s"))
            wrappers = {path: text.replace("@NAME@", directory.name)
                        for path, text in WRAPPERS["striker-asm" if is_asm else "striker-c++"].items()}
            steps = ["make -f Makefile.blocksds -j$(nproc) VERBOSE=1"]
            if is_asm:
                steps.append(f"arm-none-eabi-nm build/{directory.name}.elf | grep -q ' T __wrap_main$'")
            roms.append(Rom("striker-tests", directory.name, f"window/{directory.name}", steps, wrappers,
                            note="built with the BlocksDS single-CPU template in place of devkitARM's"))
    return roms


def source_files(roms_dir: Path, rom: Rom, entry: dict) -> dict:
    """The ROM's source files and their hashes, checked against what the fetch
    recorded: nothing changed, nothing missing, nothing added."""
    prefix = "" if rom.source == "." else rom.source + "/"
    expected = {rel: digest for rel, digest in entry["files"].items() if rel.startswith(prefix)}
    if not expected:
        raise BuildError(f"{rom.key}: roms/nds/MANIFEST.json lists no files under {rom.suite}/{rom.source}")
    root = roms_dir / rom.suite
    base = root if rom.source == "." else root / rom.source
    present = {path.relative_to(root).as_posix() for path in base.rglob("*") if path.is_file()}
    problems = [f"not from the fetch: {rel}" for rel in sorted(present - set(expected))]
    problems += [f"missing: {rel}" for rel in sorted(set(expected) - present)]
    for rel in sorted(set(expected) & present):
        if sha256((root / rel).read_bytes()) != expected[rel]:
            problems.append(f"changed: {rel}")
    if problems:
        shown = "\n  ".join(problems[:8]) + ("\n  ..." if len(problems) > 8 else "")
        raise BuildError(f"{rom.key}: roms/nds/{rom.suite} is not as fetched; run "
                         f"tools/fetch_nds_tests.py --suite {rom.suite} --force\n  {shown}")
    return expected


def shell_block(rom: Rom) -> str:
    """The part of the container script that builds one ROM. Its text is part
    of the ROM's fingerprint."""
    lines = [f"cd /build/{rom.suite}/{rom.source}"]
    for path, text in rom.wrappers.items():
        directory = os.path.dirname(path)
        if directory:
            lines.append(f"mkdir -p {directory}")
        lines.append(f"cat > {path} <<'NC_EOF'\n{text}NC_EOF")
    if rom.generated:
        lines.append(f"cp -a /gen/{rom.key}/{rom.generated} ./{rom.generated}")
    lines += [f"timeout 900 {step}" if step.startswith("make ") else step for step in rom.steps]
    lines += [
        # Exactly one ROM must come out, whatever the Makefile calls it.
        'set -- *.nds; [ "$#" -eq 1 ] && [ -f "$1" ]',
        f'echo "@@built $1"',
        f'cp "$1" /out/{rom.key}.nds',
        f"if [ -f build/arm9.elf ]; then cp build/arm9.elf /out/{rom.suite}/elf/{rom.name}.arm9.elf; "
        f"cp build/arm7.elf /out/{rom.suite}/elf/{rom.name}.arm7.elf; "
        f'else cp "build/${{1%.nds}}.elf" /out/{rom.suite}/elf/{rom.name}.arm9.elf; fi',
    ]
    return "\n".join(lines)


def container_script(roms: list, suites: list) -> str:
    parts = [
        "set -u",
        "export PATH=/opt/wonderful/toolchain/gcc-arm-none-eabi/bin:$PATH",
        'echo "@@gcc=$(arm-none-eabi-gcc --version | head -n 1)"',
        'echo "@@binutils=$(arm-none-eabi-as --version | head -n 1)"',
        'echo "@@blocksds=$(cat "$BLOCKSDS/version.txt" 2>/dev/null | head -n 1)"',
        "mkdir -p /build /out/sdk-arm7",
        # The stock ARM7 programs, for their symbols.
        'cp "$BLOCKSDS"/sys/arm7/main_core/*.elf /out/sdk-arm7/ || exit 3',
    ]
    for suite in suites:
        parts.append(f"cp -a /src/{suite} /build/{suite} || exit 3")
        parts.append(f"mkdir -p /out/{suite}/elf /out/logs/{suite}")
    for rom in roms:
        parts.append(
            f"rm -f /out/{rom.key}.nds /out/{rom.suite}/elf/{rom.name}.arm9.elf "
            f"/out/{rom.suite}/elf/{rom.name}.arm7.elf\n"
            f"(\nset -e\necho \"Building {rom.key} in $NC_IMAGE\"\n{shell_block(rom)}\n) "
            f"> /out/logs/{rom.key}.log 2>&1\n"
            f'echo "@@rom {rom.key} $?"')
    parts.append('echo "@@done"')
    return "\n".join(parts) + "\n"


# --- Generated input --------------------------------------------------------


def seeded_bytes(label: str, size: int) -> bytes:
    """SHA-256 in counter mode: the same bytes on every host and Python."""
    out = bytearray()
    counter = 0
    prefix = STRESS_SEED + label.encode() + b":"
    while len(out) < size:
        out += hashlib.sha256(prefix + counter.to_bytes(8, "little")).digest()
        counter += 1
    return bytes(out[:size])


def seeded_int(label: str, low: int, high: int) -> int:
    return low + int.from_bytes(seeded_bytes(label, 8), "little") % (high - low + 1)


def generate_stress_nitrofs(target: Path) -> None:
    """The tree filesystem/stress_test/gen_filesystem.py makes, from a seed."""
    marker = target.parent / "generated.json"
    stamp = {"seed": STRESS_SEED.decode(), "folders": STRESS_FOLDERS, "files": STRESS_FILES}
    if marker.exists() and json.loads(marker.read_text()) == stamp and target.is_dir():
        return
    if target.exists():
        shutil.rmtree(target)
    marker.unlink(missing_ok=True)
    target.mkdir(parents=True)
    folders = [target]
    for number in range(STRESS_FOLDERS):
        parent = folders[seeded_int(f"folder {number}", 0, len(folders) - 1)]
        folders.append(parent / f"dir{number}")
        folders[-1].mkdir()
    for number in range(STRESS_FILES):
        parent = folders[seeded_int(f"place {number}", 0, STRESS_FOLDERS - 1)]
        data = seeded_bytes(f"file {number}", seeded_int(f"size {number}", 1, 256) * 1024)
        (parent / hashlib.md5(data).hexdigest()).write_bytes(data)
    marker.write_text(json.dumps(stamp, indent=2, sort_keys=True) + "\n", newline="\n")


# --- ROM header -------------------------------------------------------------


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def rom_header(data: bytes) -> dict:
    """The header fields a runner wants, after checking that this is a ROM:
    the header's own CRC, and both programs lying inside the file."""
    if len(data) < 0x200:
        raise BuildError("shorter than a ROM header")

    def word(offset: int) -> int:
        return int.from_bytes(data[offset:offset + 4], "little")

    if crc16(data[:0x15E]) != int.from_bytes(data[0x15E:0x160], "little"):
        raise BuildError("the header CRC is wrong")
    header = {"title": data[0:12].rstrip(b"\0").decode("ascii", "replace")}
    for cpu, offset in (("arm9", 0x20), ("arm7", 0x30)):
        rom_offset, entry, load, size = (word(offset + 4 * index) for index in range(4))
        if size == 0 or rom_offset + size > len(data):
            raise BuildError(f"the {cpu} program does not lie inside the file")
        header[cpu] = {"rom_offset": rom_offset, "entry": f"0x{entry:08X}", "load": f"0x{load:08X}", "size": size}
    header["nitrofs_files"] = word(0x4C) // 8  # FAT size: 8 bytes per file
    return header


# --- Main -------------------------------------------------------------------


def fingerprint(rom: Rom, entry: dict, files: dict, image: dict) -> str:
    recipe = {
        "version": RECIPE_VERSION, "pin": entry.get("pin", entry["commit"]), "image": image["id"],
        "files": sorted(files.items()), "script": shell_block(rom),
        "generated": [STRESS_SEED.decode(), STRESS_FOLDERS, STRESS_FILES] if rom.generated else None,
    }
    return sha256(json.dumps(recipe, sort_keys=True).encode())


def is_current(out_dir: Path, rom: Rom, built: dict, print_of: str) -> bool:
    record = built.get(rom.key)
    path = out_dir / f"{rom.key}.nds"
    return bool(record and record.get("fingerprint") == print_of and path.is_file()
                and sha256(path.read_bytes()) == record.get("sha256"))


def write_manifest(path: Path, manifest: dict) -> None:
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--roms", type=Path, default=DEFAULT_ROMS,
                        help="where tools/fetch_nds_tests.py put the suites (default: roms/nds)")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help=f"Docker image (default: {DEFAULT_IMAGE})")
    parser.add_argument("--suite", action="append", choices=["blocksds-tests", "arm7wrestler", "striker-tests"])
    parser.add_argument("--rom", action="append", metavar="NAME",
                        help="build only this ROM (a name from --list); may be repeated")
    parser.add_argument("--force", action="store_true", help="rebuild even what is up to date")
    parser.add_argument("--list", action="store_true", help="show every ROM and its state, build nothing")
    args = parser.parse_args()

    roms_dir = args.roms.resolve()
    out_dir = roms_dir / "built"
    manifest_path = out_dir / "MANIFEST.json"
    try:
        fetch_path = roms_dir / "MANIFEST.json"
        if not fetch_path.exists():
            raise BuildError(f"{fetch_path} is missing; run tools/fetch_nds_tests.py")
        fetched = json.loads(fetch_path.read_text())
        manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
        built = manifest.setdefault("roms", {})

        known = discover(roms_dir)
        selected = [rom for rom in known if not args.suite or rom.suite in args.suite]
        if args.rom:
            unknown = sorted(set(args.rom) - {rom.name for rom in selected})
            if unknown:
                raise BuildError(f"no such ROM: {', '.join(unknown)} (see --list)")
            selected = [rom for rom in selected if rom.name in args.rom]
        if not selected:
            raise BuildError("nothing to build: the suites are not in roms/nds; run tools/fetch_nds_tests.py")
        for rom in selected:
            if rom.suite not in fetched:
                raise BuildError(f"{rom.suite} is not in roms/nds/MANIFEST.json; run tools/fetch_nds_tests.py")

        try:
            image = image_identity(args.image)
        except BuildError as error:
            if not args.list:
                raise
            # Listing works without Docker; it just cannot tell stale from current.
            print(f"note: {error}\n", file=sys.stderr)
            image = None
        prints, skipped, stale, current = {}, [], [], []
        for rom in selected:
            if rom.key in NOT_BUILDABLE and not (args.rom and rom.name in args.rom):
                skipped.append(rom)
                continue
            if image is None:
                continue
            files = source_files(roms_dir, rom, fetched[rom.suite])
            prints[rom.key] = fingerprint(rom, fetched[rom.suite], files, image)
            (current if is_current(out_dir, rom, built, prints[rom.key]) and not args.force else stale).append(rom)

        if args.list:
            for rom in selected:
                state = ("not buildable" if rom in skipped else "up to date" if rom in current
                         else "built?" if image is None and rom.key in built
                         else "stale" if rom.key in built else "not built")
                print(f"{rom.suite:15} {rom.name:38} {state:13} {rom.suite}/{rom.source}")
            for rom in skipped:
                print(f"\n{rom.key} is not buildable with {args.image}: {NOT_BUILDABLE[rom.key]}")
            return 0

        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "logs").mkdir(exist_ok=True)
        (out_dir / "generated").mkdir(exist_ok=True)
        manifest["not_built"] = {rom.key: NOT_BUILDABLE[rom.key] for rom in known if rom.key in NOT_BUILDABLE
                                 and rom.key not in built}
        for rom in skipped:
            print(f"{rom.key}: not buildable with this image: {NOT_BUILDABLE[rom.key]}")
        if not stale:
            write_manifest(manifest_path, manifest)
            print(f"{len(current)} ROMs up to date")
            return 0

        for rom in stale:
            if rom.generated:
                generate_stress_nitrofs(out_dir / "generated" / rom.suite / rom.name / rom.generated)
        suites = sorted({rom.suite for rom in stale})
        script = container_script(stale, suites)
        command = ["docker", "run", "--rm", "-i", "--network", "none", "-e", f"NC_IMAGE={image['name']} ({image['id']})"]
        for suite in suites:
            command += bind(roms_dir / suite, f"/src/{suite}", True)
        command += bind(out_dir / "generated", "/gen", True) + bind(out_dir, "/out", False)
        # The script is stored before it runs, so that nothing it starts can
        # read the rest of it from stdin.
        command += [image["name"], "bash", "-c", "cat > /tmp/build.sh && exec bash /tmp/build.sh"]

        print(f"building {len(stale)} ROMs in {image['name']} ({image['id'][:19]}); {len(current)} up to date",
              flush=True)
        started = time.monotonic()
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, env=docker_env())
        process.stdin.write(script.encode("utf-8"))
        process.stdin.close()
        results, facts, finished, output = {}, {}, False, []
        for raw in process.stdout:
            line = raw.decode("utf-8", errors="replace").rstrip()
            output.append(line)
            match = re.fullmatch(r"@@rom (\S+) (\d+)", line)
            if match:
                results[match.group(1)] = int(match.group(2))
                print(f"  {'built ' if match.group(2) == '0' else 'FAILED'} {match.group(1)}", flush=True)
            elif line.startswith("@@") and "=" in line:
                name, _, value = line[2:].partition("=")
                facts[name] = value
            finished = finished or line == "@@done"
        status = process.wait()
        (out_dir / "logs" / "driver.log").write_text("\n".join(output) + "\n", encoding="utf-8", newline="\n")

        manifest["image"] = image
        manifest["toolchain"] = facts
        manifest["note"] = ("Every ROM also contains code of the BlocksDS SDK it was built with (crt0, libnds, "
                            "picolibc, and a stock ARM7 program unless arm7 is 'own'); see the SDK's licenses.")
        failures = []
        for rom in stale:
            path = out_dir / f"{rom.key}.nds"
            log_rel = f"logs/{rom.key}.log"
            log_path = out_dir / log_rel
            log_text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
            try:
                if results.get(rom.key) != 0:
                    raise BuildError("the build failed" if rom.key in results else "the build did not run")
                data = path.read_bytes()
                header = rom_header(data)
            except (BuildError, OSError) as error:
                path.unlink(missing_ok=True)
                built.pop(rom.key, None)
                reason = next((line.strip() for line in log_text.splitlines()
                               if re.search(r"\berror\b|\*\*\*|No such file|not found", line, re.I)), str(error))
                failures.append((rom, reason, log_path))
                continue
            entry = fetched[rom.suite]
            stock = re.search(r"-7 \S*/main_core/(\S+\.elf)", log_text)
            base = roms_dir / rom.suite / rom.source
            spdx = sorted({match.group(1) for source in base.rglob("*")
                           if source.is_file() and source.suffix in (".c", ".cpp", ".h", ".s", ".py", "")
                           and "test" not in source.relative_to(base).parts[:1]
                           for match in re.finditer(r"SPDX-License-Identifier:\s*([^\s*]+)",
                                                    source.read_text(encoding="utf-8", errors="replace")[:2000])})
            built_as = re.search(r"^@@built (.+)$", log_text, re.M)
            built[rom.key] = {
                "suite": rom.suite, "name": rom.name, "rom": f"{rom.key}.nds",
                "source": f"{rom.suite}/{rom.source}".removesuffix("/."),
                "repo": entry["repo"], "commit": entry["commit"], "license": entry["license"],
                "sha256": sha256(data), "size": len(data), "image_id": image["id"],
                "fingerprint": prints[rom.key], "log": log_rel, "header": header,
                "built_as": built_as.group(1) if built_as else None,
                "arm7": f"sdk-arm7/{stock.group(1)}" if stock else "own",
                "elf": sorted(f"{rom.suite}/elf/{item.name}" for item in (out_dir / rom.suite / "elf").glob(f"{rom.name}.arm?.elf")),
            }
            if spdx:
                built[rom.key]["source_spdx"] = spdx
            if rom.note:
                built[rom.key]["note"] = rom.note
        manifest["not_built"] = {key: reason for key, reason in manifest["not_built"].items() if key not in built}
        write_manifest(manifest_path, manifest)

        elapsed = time.monotonic() - started
        print(f"{len(stale) - len(failures)} built, {len(failures)} failed, {len(current)} up to date, "
              f"{len(skipped)} not buildable, in {elapsed:.1f} s")
        for rom, reason, log_path in failures:
            print(f"FAILED {rom.key}: {reason}\n       log: {log_path}", file=sys.stderr)
        if status != 0 or not finished:
            raise BuildError(f"the container did not run to its end (exit {status}); "
                             f"log: {out_dir / 'logs' / 'driver.log'}")
        return 1 if failures else 0
    except BuildError as error:
        print(f"FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
