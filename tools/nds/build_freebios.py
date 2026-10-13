#!/usr/bin/env python3
"""Assemble the replacement Nintendo DS BIOS and embed it in the runtime.

The DS runtime boots games with the DraStic BIOS ("FreeBIOS") by Gilead
Kutnick, BSD-2-Clause, instead of Nintendo's. tools/fetch_nds_tests.py puts
its source, as melonDS distributes it, in roms/nds/freebios/freebios/. This
script assembles the two DS images from it in the BlocksDS Docker image, the
way melonDS's own freebios/Makefile.bios does (one source, -DPLATFORM_NTR and
-DTARGET_ARM9 or -DTARGET_ARM7), and writes:

    runtime-nds/include/nds_bios_data.h    the two arrays' declarations
    runtime-nds/src/nds_bios_data.c        the images, with the license notice
    roms/nds/built/freebios/               (gitignored) for reading and tools:
        bios_ntr_arm9.bin, bios_ntr_arm7.bin   the bytes as assembled
        bios9.bin, bios7.bin                   the same, zero-padded to the
                                               size of the BIOS ROMs
        bios_ntr_arm9.elf, bios_ntr_arm7.elf   as linked by Makefile.bios, at 0
        bios_ntr_arm9.at_ffff0000.elf          the ARM9 ELF moved to where the
                                               ARM9 BIOS is mapped
        *.lst                                  disassembly and symbol table
        *.sym                                  "address name", in address order
        *.map, MANIFEST.json
    roms/nds/built/logs/freebios.log       the build log

    python tools/nds/build_freebios.py              # build and generate
    python tools/nds/build_freebios.py --compare    # also compare with the
                                                    # copies in melonDS, SkyEmu

Docker must be running with the image already pulled; nothing is pulled or
installed. The fetched source is mounted read-only and assembled in a scratch
copy inside the container.

The run fails closed. The fetched source must match roms/nds/MANIFEST.json,
and the assembled images must hash to the values pinned below, which are the
images melonDS embeds (src/FreeBIOS_Data.h at the same commit). The two
generated files are written only after every check has passed, and only if
their contents would change.

--compare downloads melonDS's and SkyEmu's embedded copies into
build-agents/nds-builds/ref/ and compares them with what was built, byte for
byte. Where a copy differs, it assembles that copy's own source with the same
toolchain to tell a difference in the source from a difference in the tools.
The report is printed and kept in roms/nds/built/freebios/COMPARE.txt.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_ROMS = REPO_ROOT / "roms" / "nds"
HEADER_PATH = REPO_ROOT / "runtime-nds" / "include" / "nds_bios_data.h"
SOURCE_PATH = REPO_ROOT / "runtime-nds" / "src" / "nds_bios_data.c"
REF_DIR = REPO_ROOT / "build-agents" / "nds-builds" / "ref"

DEFAULT_IMAGE = "skylyrac/blocksds:slim-latest"
TOOLCHAIN_BIN = "/opt/wonderful/toolchain/gcc-arm-none-eabi/bin"

SUITE = "freebios"
SOURCE_FILES = [
    "freebios/Makefile",
    "freebios/Makefile.bios",
    "freebios/drastic_bios_readme.txt",
    "freebios/src/bios_common.s",
]

# The two BIOS ROMs: name, where the CPU sees it, and its size.
IMAGES = {
    "arm9": {"base": 0xFFFF0000, "size": 4096, "array": "nds_bios_arm9", "padded": "bios9.bin"},
    "arm7": {"base": 0x00000000, "size": 16384, "array": "nds_bios_arm7", "padded": "bios7.bin"},
}

# What the source and the assembled images hashed to when the images were
# compared with the ones melonDS embeds and found identical (--compare). A
# source that hashes differently is a different revision of the BIOS: run
# --compare against it, then update these.
PINNED = {
    "commit": "906e9ebb27da8c6a715cd7abab4abfe8a8d29427",
    "source_sha256": "2957553c276fd5c0b497698a82c0846d3a0c60a87fa1af7fdae6d44d59e73374",
    "arm9": {"size": 1876, "sha256": "61eb625657544706fbc8d1eabd788cf120edcb63072f452fa282a2410ed705ba"},
    "arm7": {"size": 8240, "sha256": "ded421807b0780eb89cbacf03b06dd0f5a0a4fe6a937ae5a9a58ee3b2eaba3f3"},
}

# Other embedded copies of this BIOS, for --compare. Each is an older revision
# of the source, kept as two padded binaries next to the source they were
# assembled from. melonDS 1.1 is the release docs/NDS-PLAN.md uses as oracle.
OLDER_COPIES = [
    {
        "name": "melonDS 1.1",
        "prefix": "melonDS-1.1",
        "repo": "melonDS-emu/melonDS",
        "commit": "b86390e4428bf38ce4c1ce0e9ca446d6d25955e8",
        "dir": "freebios",
    },
    {
        "name": "SkyEmu",
        "prefix": "SkyEmu",
        "repo": "skylersaleh/SkyEmu",
        "commit": "01516d6798e3652b583e6a366085bb51c43b528d",
        "dir": "src/freebios",
    },
]


class BuildError(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_if_changed(path: Path, data: bytes) -> bool:
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


# --- Docker -----------------------------------------------------------------


def docker_env() -> dict:
    # A Python started from Git Bash is a native program, so its arguments to
    # docker are not rewritten. These two cover an MSYS Python, which would
    # turn "/src" into a Windows path.
    env = dict(os.environ)
    env["MSYS_NO_PATHCONV"] = "1"
    env["MSYS2_ARG_CONV_EXCL"] = "*"
    return env


def docker(args: list, **kwargs) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(["docker", *args], env=docker_env(), **kwargs)
    except FileNotFoundError:
        raise BuildError("docker was not found on PATH") from None
    except subprocess.TimeoutExpired:
        raise BuildError("docker did not finish in time") from None


def image_identity(image: str) -> dict:
    result = docker(["image", "inspect", "--format", "{{.Id}}|{{json .RepoDigests}}", image],
                    capture_output=True, text=True, timeout=120)
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


def run_in_container(image: str, mounts: list, script: str, log_path: Path) -> str:
    """Run a bash script in a fresh container, with no network. The script goes
    in on stdin, so no shell on the host ever sees it. Returns its output,
    which is also appended to the log."""
    command = ["run", "--rm", "-i", "--network", "none"]
    for mount in mounts:
        command += bind(*mount)
    command += [image, "bash", "-s"]
    result = docker(command, input=script.encode("utf-8"), stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, timeout=900)
    output = result.stdout.decode("utf-8", errors="replace")
    with log_path.open("a", encoding="utf-8", newline="\n") as log:
        log.write(output)
    if result.returncode != 0:
        tail = "\n".join(output.splitlines()[-25:])
        raise BuildError(f"the build in the container failed (exit {result.returncode}); "
                         f"log: {log_path}\n{tail}")
    return output


# --- Build ------------------------------------------------------------------

BUILD_SCRIPT = r"""
set -eu
export PATH=@TOOLCHAIN@:$PATH
echo "@@gcc=$(arm-none-eabi-gcc --version | head -n 1)"
echo "@@binutils=$(arm-none-eabi-as --version | head -n 1)"

rm -rf /build && mkdir -p /build && cp -a /src/. /build/ && cd /build

for target in arm9 arm7; do
    name=bios_ntr_$target
    # Makefile.bios's default goal also wants a C header from xxd, which this
    # image does not have; the .bin is all that is needed.
    make -f Makefile.bios VERBOSE=1 TARGET=$target PLATFORM=ntr OUTPUT=$name.bin $name.bin
    cp $name.bin build/$name.elf build/$name.map /out/
done

# The ARM9 BIOS is linked at 0, like the ARM7 one, and is position independent
# apart from its SWI table, whose entries the source offsets by 0xFFFF0000.
# Move a copy of the ELF there so the listing shows the real addresses.
arm-none-eabi-objcopy --change-addresses 0xFFFF0000 /out/bios_ntr_arm9.elf /out/bios_ntr_arm9.at_ffff0000.elf
arm-none-eabi-objcopy -O binary /out/bios_ntr_arm9.at_ffff0000.elf /tmp/moved.bin
cmp /tmp/moved.bin /out/bios_ntr_arm9.bin

listing() {
    echo "$2"
    echo
    arm-none-eabi-objdump -d "$1"
    echo
    echo "SYMBOL TABLE, in address order (arm-none-eabi-nm -n)"
    echo
    arm-none-eabi-nm -n "$1"
}
listing /out/bios_ntr_arm9.at_ffff0000.elf \
    "FreeBIOS for the ARM9 (NTR), 4 KiB BIOS ROM at 0xFFFF0000. Generated by tools/nds/build_freebios.py." \
    > /out/bios_ntr_arm9.lst
listing /out/bios_ntr_arm7.elf \
    "FreeBIOS for the ARM7 (NTR), 16 KiB BIOS ROM at 0x00000000. Generated by tools/nds/build_freebios.py." \
    > /out/bios_ntr_arm7.lst

arm-none-eabi-nm -n /out/bios_ntr_arm9.at_ffff0000.elf > /out/bios_ntr_arm9.nm
arm-none-eabi-nm -n /out/bios_ntr_arm7.elf > /out/bios_ntr_arm7.nm
echo "@@done"
"""

# Everything the build leaves in roms/nds/built/freebios/. Removed first, so a
# failed run cannot leave an older build looking current.
BUILT_FILES = [
    "bios_ntr_arm9.bin", "bios_ntr_arm7.bin", "bios9.bin", "bios7.bin",
    "bios_ntr_arm9.elf", "bios_ntr_arm7.elf", "bios_ntr_arm9.at_ffff0000.elf",
    "bios_ntr_arm9.map", "bios_ntr_arm7.map", "bios_ntr_arm9.lst", "bios_ntr_arm7.lst",
    "bios_ntr_arm9.nm", "bios_ntr_arm7.nm", "bios_ntr_arm9.sym", "bios_ntr_arm7.sym",
    "MANIFEST.json",
]


def check_sources(roms: Path) -> dict:
    manifest_path = roms / "MANIFEST.json"
    if not manifest_path.exists():
        raise BuildError(f"{manifest_path} is missing; run tools/fetch_nds_tests.py --suite {SUITE}")
    entry = json.loads(manifest_path.read_text()).get(SUITE)
    if not entry:
        raise BuildError(f"{SUITE} is not in {manifest_path}; run tools/fetch_nds_tests.py --suite {SUITE}")
    for rel in SOURCE_FILES:
        path = roms / SUITE / rel
        if not path.is_file():
            raise BuildError(f"{path} is missing; run tools/fetch_nds_tests.py --suite {SUITE} --force")
        if sha256(path.read_bytes()) != entry["files"].get(rel):
            raise BuildError(f"{path} does not match roms/nds/MANIFEST.json; "
                             f"run tools/fetch_nds_tests.py --suite {SUITE} --force")
    return entry


def symbols_from_nm(text: str) -> list:
    """(address, name) for the labels of the source. The linker's own symbols
    (_stack, __bss_start__ and the like) and the mapping symbols are left out."""
    symbols = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tT" and not parts[2].startswith("$"):
            symbols.append((int(parts[0], 16), parts[2]))
    return symbols


def build(roms: Path, image: dict, allow_unverified: bool) -> dict:
    entry = check_sources(roms)
    source_dir = roms / SUITE / "freebios"
    out_dir = roms / "built" / "freebios"
    log_path = roms / "built" / "logs" / "freebios.log"
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    for name in BUILT_FILES:
        (out_dir / name).unlink(missing_ok=True)
    log_path.write_text(f"FreeBIOS {entry['repo']}@{entry['commit']} in {image['name']} ({image['id']})\n",
                        encoding="utf-8", newline="\n")

    script = BUILD_SCRIPT.replace("@TOOLCHAIN@", TOOLCHAIN_BIN)
    output = run_in_container(image["name"], [(source_dir, "/src", True), (out_dir, "/out", False)],
                              script, log_path)
    facts = dict(re.findall(r"^@@(\w+)=(.*)$", output, re.M))
    if "@@done" not in output or "gcc" not in facts:
        raise BuildError(f"the build did not run to its end; log: {log_path}")

    source_hash = sha256((source_dir / "src" / "bios_common.s").read_bytes())
    same_source = source_hash == PINNED["source_sha256"]
    result = {"repo": entry["repo"], "commit": entry["commit"], "license": "BSD-2-Clause",
              "source_sha256": source_hash, "image": image, "toolchain": facts, "images": {}}
    problems = []
    for cpu, spec in IMAGES.items():
        raw = (out_dir / f"bios_ntr_{cpu}.bin").read_bytes()
        if not raw or len(raw) > spec["size"] or len(raw) % 4:
            raise BuildError(f"the {cpu} image is {len(raw)} bytes; the BIOS ROM is {spec['size']}")
        if same_source and (len(raw) != PINNED[cpu]["size"] or sha256(raw) != PINNED[cpu]["sha256"]):
            problems.append(f"the {cpu} image is {len(raw)} bytes, SHA-256 {sha256(raw)}; the verified one "
                            f"is {PINNED[cpu]['size']} bytes, SHA-256 {PINNED[cpu]['sha256']}")
        padded = raw + bytes(spec["size"] - len(raw))
        (out_dir / spec["padded"]).write_bytes(padded)

        symbols = symbols_from_nm((out_dir / f"bios_ntr_{cpu}.nm").read_text())
        if not symbols:
            raise BuildError(f"no symbols came out of the {cpu} ELF")
        (out_dir / f"bios_ntr_{cpu}.nm").unlink()
        (out_dir / f"bios_ntr_{cpu}.sym").write_text(
            "".join(f"{address:08x} {name}\n" for address, name in symbols), newline="\n")
        result["images"][cpu] = {
            "base": f"0x{spec['base']:08X}", "rom_size": spec["size"], "assembled_size": len(raw),
            "sha256": sha256(raw), "padded_sha256": sha256(padded), "raw": raw,
        }

    if not same_source:
        problems.append(f"bios_common.s (SHA-256 {source_hash}, commit {entry['commit']}) is not the "
                        f"revision whose images were verified (commit {PINNED['commit']}); run --compare "
                        f"and update PINNED in {Path(__file__).name}")
    if problems and not allow_unverified:
        raise BuildError("the images are not the verified ones, so nothing was generated "
                         "(--allow-unverified overrides):\n  " + "\n  ".join(problems))
    result["verified"] = not problems
    for problem in problems:
        print(f"warning: {problem}", file=sys.stderr)
    return result


# --- Generated C ------------------------------------------------------------


def license_notice(roms: Path) -> list:
    """The notice at the top of drastic_bios_readme.txt, up to its "-- Info --"
    section, line for line."""
    lines = (roms / SUITE / "freebios" / "drastic_bios_readme.txt").read_text(encoding="utf-8").splitlines()
    notice = []
    for line in lines:
        if line.strip() == "-- Info --":
            break
        notice.append(line.rstrip())
    while notice and not notice[-1]:
        notice.pop()
    text = "\n".join(notice)
    if "Copyright (c) 2013, Gilead Kutnick" not in text or "POSSIBILITY OF SUCH DAMAGE." not in text \
            or "*/" in text:
        raise BuildError("drastic_bios_readme.txt does not hold the notice this script expects")
    return notice


def comment(lines: list) -> str:
    return "/*\n" + "".join((" * " + line).rstrip() + "\n" for line in lines) + " */\n"


def generate_header(result: dict) -> str:
    arm9, arm7 = result["images"]["arm9"], result["images"]["arm7"]
    intro = [
        "The replacement BIOS images the DS boots with: the DraStic BIOS",
        '("FreeBIOS") by Gilead Kutnick, BSD-2-Clause. Generated by',
        "tools/nds/build_freebios.py together with src/nds_bios_data.c, which",
        "carries the license notice and says where the source is. Do not edit.",
        "",
        "Each array is a whole BIOS ROM as its CPU sees it: 4 KiB for the ARM9",
        "at 0xFFFF0000 and 16 KiB for the ARM7 at 0. The assembled code is",
        "shorter (the two CODE_SIZE values); the rest of each ROM is zero.",
    ]
    return (
        comment(intro)
        + "#ifndef NDS_BIOS_DATA_H\n#define NDS_BIOS_DATA_H\n\n#include <stdint.h>\n\n"
        + "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        + f"#define NDS_BIOS_ARM9_SIZE {arm9['rom_size']}\n"
        + f"#define NDS_BIOS_ARM7_SIZE {arm7['rom_size']}\n"
        + f"#define NDS_BIOS_ARM9_CODE_SIZE {arm9['assembled_size']}\n"
        + f"#define NDS_BIOS_ARM7_CODE_SIZE {arm7['assembled_size']}\n\n"
        + "extern const uint8_t nds_bios_arm9[NDS_BIOS_ARM9_SIZE];\n"
        + "extern const uint8_t nds_bios_arm7[NDS_BIOS_ARM7_SIZE];\n\n"
        + "#ifdef __cplusplus\n}\n#endif\n\n#endif /* NDS_BIOS_DATA_H */\n"
    )


def c_array(name: str, size_macro: str, raw: bytes) -> str:
    rows = []
    for offset in range(0, len(raw), 12):
        rows.append("    " + ", ".join(f"0x{byte:02x}" for byte in raw[offset:offset + 12]) + ",\n")
    return f"const uint8_t {name}[{size_macro}] = {{\n" + "".join(rows) + "};\n"


def generate_source(result: dict, notice: list) -> str:
    arm9, arm7 = result["images"]["arm9"], result["images"]["arm7"]
    intro = [
        "Generated by tools/nds/build_freebios.py. Do not edit.",
        "",
        'The replacement Nintendo DS BIOS images: the DraStic BIOS ("FreeBIOS")',
        "by Gilead Kutnick, BSD-2-Clause, assembled without change from",
        "",
        f"    https://github.com/{result['repo']}",
        f"    commit {result['commit']}",
        "    freebios/src/bios_common.s",
        f"    (SHA-256 {result['source_sha256'][:32]}",
        f"             {result['source_sha256'][32:]})",
        "",
        "as that repository's freebios/Makefile.bios builds it for the DS:",
        "-DPLATFORM_NTR with -DTARGET_ARM9 (-mcpu=arm946e-s+nofp) or",
        "-DTARGET_ARM7 (-mcpu=arm7tdmi), linked at 0. Assembled with",
        f"{result['toolchain']['gcc']} and",
        f"{result['toolchain']['binutils']}.",
        "",
        f"nds_bios_arm9: {arm9['assembled_size']} bytes of code in a {arm9['rom_size']}-byte ROM at {arm9['base']}",
        f"    SHA-256 {arm9['sha256'][:32]}",
        f"            {arm9['sha256'][32:]}",
        f"nds_bios_arm7: {arm7['assembled_size']} bytes of code in a {arm7['rom_size']}-byte ROM at {arm7['base']}",
        f"    SHA-256 {arm7['sha256'][:32]}",
        f"            {arm7['sha256'][32:]}",
        "",
        "The hashes are of the assembled bytes. Each ROM is zero from the end",
        "of its code to its size, which is what the short initialisers below",
        "leave.",
        "",
        *(["The assembled bytes are identical to the images melonDS embeds",
           "(src/FreeBIOS_Data.h at the same commit)."] if result["verified"] else
          ["These images are NOT the ones that were checked against the copy",
           "melonDS embeds (generated with --allow-unverified)."]),
        "",
        "The license of the BIOS, from freebios/drastic_bios_readme.txt:",
        "",
        *notice,
    ]
    return (
        comment(intro)
        + '#include "nds_bios_data.h"\n\n'
        + c_array("nds_bios_arm9", "NDS_BIOS_ARM9_SIZE", arm9["raw"])
        + "\n"
        + c_array("nds_bios_arm7", "NDS_BIOS_ARM7_SIZE", arm7["raw"])
    )


# --- Comparison with the copies other emulators embed -----------------------


def reference_name(prefix: str, commit: str, path: str) -> str:
    # The commit is in the name, so a changed pin never reuses an old download.
    return f"{prefix}-{commit[:10]}-{Path(path).name}"


def fetch_reference(repo: str, commit: str, path: str, prefix: str) -> Path:
    out = REF_DIR / reference_name(prefix, commit, path)
    if not out.exists():
        url = f"https://raw.githubusercontent.com/{repo}/{commit}/{path}"
        print(f"  fetching {url}", flush=True)
        request = urllib.request.Request(url, headers={"User-Agent": "native-console-fetch"})
        with urllib.request.urlopen(request, timeout=300) as response:
            data = response.read()
        REF_DIR.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
    return out


def parse_c_arrays(text: str) -> dict:
    arrays = {}
    for match in re.finditer(r"unsigned char (\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
        arrays[match.group(1)] = bytes(int(token, 16) for token in re.findall(r"0x[0-9a-fA-F]{1,2}", match.group(2)))
    return arrays


def describe_difference(ours: bytes, theirs: bytes) -> str:
    if len(ours) != len(theirs):
        return f"sizes differ ({len(ours)} against {len(theirs)})"
    differing = [index for index in range(len(ours)) if ours[index] != theirs[index]]
    return (f"{len(differing)} of {len(ours)} bytes differ, from offset 0x{differing[0]:X} "
            f"to 0x{differing[-1]:X}")


# The older copies' Makefile, with one change: it asks for -march=armv5 for
# the ARM9, which GCC no longer has; armv5t assembles the same ARM code.
REBUILD_SCRIPT = r"""
set -eu
export PATH=@TOOLCHAIN@:$PATH
mkdir -p /build && cd /build && cp /ref/@SOURCE@ bios_common.S
arm-none-eabi-gcc bios_common.S -DBIOS_ARM7 -march=armv4 -c -o arm7.o
arm-none-eabi-gcc bios_common.S -DBIOS_ARM9 -march=armv5t -c -o arm9.o
arm-none-eabi-objcopy -O binary arm7.o /out/rebuilt-arm7.tmp
arm-none-eabi-objcopy -O binary arm9.o /out/rebuilt-arm9.tmp
echo "@@done"
"""


def ntr_lines(source: str) -> list:
    """The source as the DS build sees it, as far as a line comparison needs:
    the TWL-only blocks removed and the older names of the two CPU macros
    brought to the current ones. Only "#ifdef PLATFORM_TWL", "#if ... &&
    defined(PLATFORM_TWL)", "#else" and "#endif" are understood, which is all
    the source uses around them."""
    lines, stack = [], []
    for line in source.replace("\r\n", "\n").split("\n"):
        stripped = line.strip()
        if stripped.startswith(("#ifdef", "#if ", "#ifndef")):
            stack.append("twl" if "PLATFORM_TWL" in stripped else "other")
            if stack[-1] == "twl":
                continue
        elif stripped.startswith("#else") and stack and stack[-1] in ("twl", "ntr"):
            stack[-1] = "ntr" if stack[-1] == "twl" else "twl"
            continue
        elif stripped.startswith("#endif") and stack:
            if stack.pop() in ("twl", "ntr"):
                continue
        if "twl" in stack:
            continue
        lines.append(line.rstrip().replace("BIOS_ARM9", "TARGET_ARM9").replace("BIOS_ARM7", "TARGET_ARM7"))
    return lines


def compare(roms: Path, result: dict, image: dict) -> bool:
    out_dir = roms / "built" / "freebios"
    log_path = roms / "built" / "logs" / "freebios.log"
    report = []
    ok = True

    def say(line: str = "") -> None:
        report.append(line)
        print(line, flush=True)

    say(f"FreeBIOS built from {result['repo']}@{result['commit']} in {image['name']}")
    say(f"  {result['toolchain']['gcc']}; {result['toolchain']['binutils']}")
    for cpu in IMAGES:
        info = result["images"][cpu]
        say(f"  {cpu}: {info['assembled_size']} bytes, SHA-256 {info['sha256']}")
    say()

    # melonDS at the commit the source came from: the copy that has to match.
    data_h = fetch_reference(result["repo"], result["commit"], "src/FreeBIOS_Data.h", "melonDS")
    arrays = parse_c_arrays(data_h.read_text())
    say(f"melonDS src/FreeBIOS_Data.h at {result['commit']} (SHA-256 {sha256(data_h.read_bytes())})")
    for cpu in IMAGES:
        theirs = arrays.get(f"bios_ntr_{cpu}")
        ours = result["images"][cpu]["raw"]
        if theirs is None:
            say(f"  bios_ntr_{cpu}: not in the file")
            ok = False
        elif theirs == ours:
            say(f"  bios_ntr_{cpu}: {len(theirs)} bytes, IDENTICAL to the built image")
        else:
            say(f"  bios_ntr_{cpu}: DIFFERENT from the built image: {describe_difference(ours, theirs)}")
            ok = False
    say()

    # Older revisions. They are expected to differ; what has to hold is that
    # the difference is in the source and not in the toolchain.
    our_source = ntr_lines((roms / SUITE / "freebios" / "src" / "bios_common.s").read_text())
    for copy in OLDER_COPIES:
        prefix = copy["prefix"]
        source = fetch_reference(copy["repo"], copy["commit"], f"{copy['dir']}/bios_common.S", prefix)
        binaries = {cpu: fetch_reference(copy["repo"], copy["commit"],
                                         f"{copy['dir']}/drastic_bios_{cpu}.bin", prefix).read_bytes()
                    for cpu in IMAGES}
        say(f"{copy['name']}: {copy['repo']}@{copy['commit']} {copy['dir']}/")
        script = REBUILD_SCRIPT.replace("@TOOLCHAIN@", TOOLCHAIN_BIN).replace("@SOURCE@", source.name)
        for cpu in IMAGES:
            (out_dir / f"rebuilt-{cpu}.tmp").unlink(missing_ok=True)
        run_in_container(image["name"], [(REF_DIR, "/ref", True), (out_dir, "/out", False)], script, log_path)
        for cpu, spec in IMAGES.items():
            theirs = binaries[cpu]
            ours = result["images"][cpu]["raw"] + bytes(spec["size"] - result["images"][cpu]["assembled_size"])
            rebuilt_path = out_dir / f"rebuilt-{cpu}.tmp"
            rebuilt = rebuilt_path.read_bytes()
            rebuilt_path.unlink()
            say(f"  drastic_bios_{cpu}.bin: {len(theirs)} bytes, SHA-256 {sha256(theirs)}")
            if theirs == ours:
                say("    IDENTICAL to the built image (padded)")
            else:
                say(f"    differs from the built image (padded): {describe_difference(ours, theirs)}")
            if rebuilt == theirs:
                say("    its own source, assembled with this toolchain, gives it back byte for byte")
            else:
                say(f"    its own source, assembled with this toolchain, does NOT give it back: "
                    f"{describe_difference(rebuilt, theirs)}")
                ok = False
        changes = [line for line in difflib.unified_diff(ntr_lines(source.read_text()), our_source,
                                                         "theirs", "ours", n=0, lineterm="")
                   if line[:1] in "+-" and line[:3] not in ("+++", "---") and line[1:].strip()]
        say(f"  source lines that differ in the DS (NTR) build, theirs (-) against ours (+): {len(changes)}")
        for line in changes:
            say(f"    {line}")
        say()

    say("Result: " + ("the built images are the ones melonDS embeds; every other difference is explained "
                      "by the source revision." if ok else "UNEXPLAINED DIFFERENCES, see above."))
    (out_dir / "COMPARE.txt").write_text("\n".join(report) + "\n", encoding="utf-8", newline="\n")
    return ok


# --- Main -------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--roms", type=Path, default=DEFAULT_ROMS,
                        help="where tools/fetch_nds_tests.py put the suites (default: roms/nds)")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help=f"Docker image (default: {DEFAULT_IMAGE})")
    parser.add_argument("--compare", action="store_true",
                        help="compare with the copies melonDS and SkyEmu embed (needs the network)")
    parser.add_argument("--allow-unverified", action="store_true",
                        help="generate even if the images are not the pinned, verified ones")
    args = parser.parse_args()

    try:
        image = image_identity(args.image)
        print(f"freebios: assembling in {image['name']} ({image['id'][:19]})", flush=True)
        result = build(args.roms, image, args.allow_unverified)
        compared = compare(args.roms, result, image) if args.compare else None
        if compared is False and not args.allow_unverified:
            raise BuildError("the comparison found differences it could not explain, so nothing was generated")

        notice = license_notice(args.roms)
        manifest = {key: value for key, value in result.items() if key != "images"}
        manifest["images"] = {cpu: {key: value for key, value in info.items() if key != "raw"}
                              for cpu, info in result["images"].items()}
        manifest["compared"] = compared
        (args.roms / "built" / "freebios" / "MANIFEST.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", newline="\n")

        for path, text in ((HEADER_PATH, generate_header(result)), (SOURCE_PATH, generate_source(result, notice))):
            changed = write_if_changed(path, text.encode("utf-8"))
            print(f"freebios: {path.relative_to(REPO_ROOT).as_posix()} {'written' if changed else 'unchanged'}")
    except BuildError as error:
        print(f"freebios: FAILED: {error}", file=sys.stderr)
        return 1

    for cpu, info in result["images"].items():
        print(f"freebios: {cpu} {info['assembled_size']} bytes of code in a {info['rom_size']}-byte ROM "
              f"at {info['base']}, SHA-256 {info['sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
