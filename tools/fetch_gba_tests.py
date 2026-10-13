#!/usr/bin/env python3
"""Download the Game Boy Advance test suites at pinned commits.

Everything lands in roms/gba/<suite>/ (gitignored) together with a
MANIFEST.json that records the source repository, commit, license and a
SHA-256 for every file. Re-running is a no-op for suites whose manifest entry
already matches the pinned commit.

    python tools/fetch_gba_tests.py                 # every suite
    python tools/fetch_gba_tests.py --suite sst     # just one
    python tools/fetch_gba_tests.py --list
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO_ROOT / "roms" / "gba"

SST_FILES = [
    "arm_b_bl", "arm_bx", "arm_cdp", "arm_data_proc_immediate",
    "arm_data_proc_immediate_shift", "arm_data_proc_register_shift",
    "arm_ldm_stm", "arm_ldr_str_immediate_offset",
    "arm_ldr_str_register_offset", "arm_ldrh_strh", "arm_ldrsb_ldrsh",
    "arm_mcr_mrc", "arm_mrs", "arm_msr_imm", "arm_msr_reg", "arm_mul_mla",
    "arm_mull_mlal", "arm_stc_ldc", "arm_swi", "arm_swp",
    "thumb_add_cmp_mov_hi", "thumb_add_sp_or_pc", "thumb_add_sub",
    "thumb_add_sub_sp", "thumb_b", "thumb_bcc", "thumb_bl_blx_prefix",
    "thumb_bl_suffix", "thumb_bx", "thumb_data_proc", "thumb_ldm_stm",
    "thumb_ldr_pc_rel", "thumb_ldr_str_imm_offset",
    "thumb_ldr_str_reg_offset", "thumb_ldr_str_sp_rel",
    "thumb_ldrb_strb_imm_offset", "thumb_ldrh_strh_imm_offset",
    "thumb_ldrh_strh_reg_offset", "thumb_ldrsb_strb_reg_offset",
    "thumb_ldrsh_ldrsb_reg_offset", "thumb_lsl_lsr_asr",
    "thumb_mov_cmp_add_sub", "thumb_push_pop", "thumb_swi",
    "thumb_undefined_bcc",
]

# name -> (repository, commit, license, how to fetch)
SUITES = {
    "sst": {
        "repo": "SingleStepTests/ARM7TDMI",
        "commit": "e3097d88d428752736b949d94c094d5da50f0db6",
        "license": "MIT",
        "files": [f"v1/{name}.json.bin" for name in SST_FILES] + ["LICENSE"],
        "note": "Per-instruction ARM7TDMI vectors generated from NanoBoyAdvance (~900 MB).",
    },
    "jsmolka": {
        "repo": "jsmolka/gba-tests",
        "commit": "a7113b67e63f83a9b321696ddd7042ccfad6c881",
        "license": "MIT",
        "note": "arm, thumb, memory, bios, ppu, save, nes, unsafe ROMs.",
    },
    "alyosha": {
        "repo": "alyosha-tas/gba-tests",
        "commit": "66f5f1d60cd7030b600606886c76f5c821ecaf9d",
        "license": "MIT",
        "note": "Fork of jsmolka with timing, prefetch, DMA, IRQ, LDM, FIFO and serial ROMs.",
    },
    "nba": {
        "repo": "nba-emu/hw-test",
        "commit": "3932a0e54dbb5d94b84798d2d5e462e080b95599",
        "license": "BSD-3-Clause",
        "note": "NanoBoyAdvance hardware edge-case tests.",
    },
    "mgba-suite": {
        "repo": "mgba-emu/suite",
        "commit": "e6942030d25ffe3ba76c72b73a86da073ec857cc",
        "license": "MIT",
        "note": "Source only; build with its docker-build.sh (devkitARM).",
    },
    "cult-bios": {
        "repo": "Cult-of-GBA/BIOS",
        "commit": "a30e9a96df083628b650724b7d4d7112b4070b98",
        "license": "MIT",
        "note": "Replacement BIOS; bios.bin is prebuilt.",
    },
    "armwrestler": {
        "repo": "destoer/armwrestler-gba-fixed",
        "commit": "802e55ad61b421f4bbbc6f74f96aa4e5df4d630f",
        "license": "none (local use only, never commit or redistribute)",
        "note": "Classic ARM/Thumb instruction test ROM.",
    },
}


def http_get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "native-console-fetch"})
    with urllib.request.urlopen(request, timeout=300) as response:
        return response.read()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch_files(spec: dict, target: Path) -> dict:
    hashes = {}
    for rel in spec["files"]:
        url = f"https://raw.githubusercontent.com/{spec['repo']}/{spec['commit']}/{rel}"
        out = target / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        print(f"  {rel}", flush=True)
        data = http_get(url)
        out.write_bytes(data)
        hashes[rel] = sha256(data)
    return hashes


def fetch_tarball(spec: dict, target: Path) -> dict:
    url = f"https://codeload.github.com/{spec['repo']}/tar.gz/{spec['commit']}"
    print(f"  {url}", flush=True)
    data = http_get(url)
    hashes = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            # Strip the "<repo>-<commit>/" prefix GitHub adds.
            parts = Path(member.name).parts[1:]
            if not parts or ".." in parts:
                continue
            rel = Path(*parts)
            out = target / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            blob = archive.extractfile(member).read()
            out.write_bytes(blob)
            hashes[rel.as_posix()] = sha256(blob)
    return hashes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--suite", action="append", choices=sorted(SUITES))
    parser.add_argument("--force", action="store_true", help="re-download even if pinned commit matches")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        for name, spec in SUITES.items():
            print(f"{name:12} {spec['repo']}@{spec['commit'][:10]}  [{spec['license']}]  {spec['note']}")
        return 0

    args.dest.mkdir(parents=True, exist_ok=True)
    manifest_path = args.dest / "MANIFEST.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}

    for name in args.suite or list(SUITES):
        spec = SUITES[name]
        target = args.dest / name
        previous = manifest.get(name)
        if (not args.force and previous and previous.get("commit") == spec["commit"]
                and target.is_dir()):
            print(f"{name}: up to date ({spec['commit'][:10]})")
            continue
        print(f"{name}: fetching {spec['repo']}@{spec['commit'][:10]}")
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        try:
            hashes = fetch_files(spec, target) if "files" in spec else fetch_tarball(spec, target)
        except Exception as error:  # fail closed: never leave a half-written suite looking valid
            shutil.rmtree(target, ignore_errors=True)
            manifest.pop(name, None)
            manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
            print(f"{name}: FAILED: {error}", file=sys.stderr)
            return 1
        manifest[name] = {
            "repo": spec["repo"],
            "commit": spec["commit"],
            "license": spec["license"],
            "files": hashes,
        }
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        print(f"{name}: {len(hashes)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
