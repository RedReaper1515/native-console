#!/usr/bin/env python3
"""Download the NES test suites at pinned commits.

Everything lands in roms/nes/<suite>/ (gitignored) together with a
MANIFEST.json that records the source repository, commit (or release tag),
license and a SHA-256 for every file. Re-running is a no-op for suites whose
manifest entry already matches the pin.

    python tools/fetch_nes_tests.py                 # every suite
    python tools/fetch_nes_tests.py --suite sst6502
    python tools/fetch_nes_tests.py --list

Suites without a license are for local testing only: never commit them,
never copy them into the repository, never redistribute them.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import fnmatch
import hashlib
import io
import json
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO_ROOT / "roms" / "nes"

HEX = [f"{i:02x}" for i in range(256)]

# name -> spec. Kinds:
#   files:   raw files at a pinned commit ("strip" drops a leading directory)
#   tarball: the repository at a pinned commit, optionally filtered by glob
#   release: a release asset (zip) pinned by tag and SHA-256, extracted
SUITES = {
    "sst6502": {
        "repo": "SingleStepTests/65x02",
        "commit": "2f6980a2d95757486c7bee24355c360e40e2a224",
        "license": "MIT",
        "strip": "nes6502/",
        "files": [f"nes6502/v1/{op}.json" for op in HEX] + ["nes6502/README.md", "LICENSE"],
        "note": "Per-instruction NES 6502 vectors with bus activity, 10,000 per opcode, unofficial opcodes included (~1.1 GB).",
    },
    "nes-test-roms": {
        "repo": "christopherpow/nes-test-roms",
        "commit": "95d8f621ae55cee0d09b91519a8989ae0e64753b",
        "license": "none (mixed authors; local use only, never commit or redistribute)",
        "include": [f"/{suite}/*" for suite in (
            "instr_test-v5", "nes_instr_test", "instr_misc", "instr_timing", "cpu_timing_test6",
            "branch_timing_tests", "blargg_nes_cpu_test5", "cpu_interrupts_v2", "cpu_reset", "cpu_dummy_reads",
            "cpu_dummy_writes", "cpu_exec_space", "ppu_vbl_nmi", "vbl_nmi_timing", "ppu_open_bus",
            "ppu_read_buffer", "blargg_ppu_tests_2005.09.15b", "oam_read", "oam_stress",
            "sprite_hit_tests_2005.10.05", "sprite_overflow_tests", "apu_test", "apu_reset", "apu_mixer",
            "blargg_apu_2005.07.30", "dmc_tests", "sprdma_and_dmc_dma", "dmc_dma_during_read4", "read_joy3",
            "mmc3_test_2", "mmc3_irq_tests", "MMC1_A12", "nes15-1.0.0",
        )] + ["/other/nestest.*"],
        "note": "blargg's CPU, PPU, APU and mapper suites, kevtris's nestest, nes15 (BSD-2-Clause, with source).",
    },
    "accuracycoin": {
        "repo": "100thCoin/AccuracyCoin",
        "commit": "9fc47e06e2277fea87d6fdaa3ecd9a6653ebf9a8",
        "license": "MIT",
        "files": ["AccuracyCoin.nes", "LICENSE", "README.md"],
        "note": "146 accuracy tests in one NROM image; a result byte per test at $0400-$0497.",
    },
}


def http_get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "native-console-fetch"})
    with urllib.request.urlopen(request, timeout=600) as response:
        return response.read()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch_files(spec: dict, target: Path) -> dict:
    def one(rel: str) -> tuple[str, str]:
        url = f"https://raw.githubusercontent.com/{spec['repo']}/{spec['commit']}/{rel}"
        strip = spec.get("strip", "")
        if strip and rel.startswith(strip):
            rel = rel[len(strip):]
        out = target / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        data = http_get(url)
        out.write_bytes(data)
        return rel, sha256(data)

    hashes = {}
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for n, (rel, digest) in enumerate(pool.map(one, spec["files"]), 1):
            hashes[rel] = digest
            if n % 32 == 0 or n == len(spec["files"]):
                print(f"  {n}/{len(spec['files'])} {rel}", flush=True)
    return hashes


def selected(path: str, spec: dict) -> bool:
    probe = "/" + path
    if any(fnmatch.fnmatch(probe, pattern) for pattern in spec.get("exclude", [])):
        return False
    include = spec.get("include")
    return not include or any(fnmatch.fnmatch(probe, pattern) for pattern in include)


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
            if not selected(rel.as_posix(), spec):
                continue
            out = target / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            blob = archive.extractfile(member).read()
            out.write_bytes(blob)
            hashes[rel.as_posix()] = sha256(blob)
    return hashes


def fetch_release(spec: dict, target: Path) -> dict:
    print(f"  {spec['url']}", flush=True)
    data = http_get(spec["url"])
    digest = sha256(data)
    if digest != spec["sha256"]:
        raise RuntimeError(f"release asset sha256 {digest} does not match the pin {spec['sha256']}")
    hashes = {}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for info in archive.infolist():
            parts = Path(info.filename).parts
            if info.is_dir() or not parts or ".." in parts or Path(info.filename).is_absolute():
                continue
            rel = Path(*parts)
            out = target / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            blob = archive.read(info)
            out.write_bytes(blob)
            hashes[rel.as_posix()] = sha256(blob)
    return hashes


def pin(spec: dict) -> str:
    return spec.get("commit") or f"{spec['tag']}@{spec['sha256']}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--suite", action="append", choices=sorted(SUITES))
    parser.add_argument("--force", action="store_true", help="re-download even if the pin matches")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        for name, spec in SUITES.items():
            print(f"{name:14} {spec['repo']}@{pin(spec)[:16]}  [{spec['license']}]  {spec['note']}")
        return 0

    args.dest.mkdir(parents=True, exist_ok=True)
    manifest_path = args.dest / "MANIFEST.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}

    for name in args.suite or list(SUITES):
        spec = SUITES[name]
        target = args.dest / name
        previous = manifest.get(name)
        if not args.force and previous and previous.get("pin") == pin(spec) and target.is_dir():
            print(f"{name}: up to date ({pin(spec)[:16]})")
            continue
        print(f"{name}: fetching {spec['repo']}@{pin(spec)[:16]}")
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        try:
            if "files" in spec:
                hashes = fetch_files(spec, target)
            elif "url" in spec:
                hashes = fetch_release(spec, target)
            else:
                hashes = fetch_tarball(spec, target)
            if not hashes:
                raise RuntimeError("no files matched")
        except Exception as error:  # fail closed: never leave a half-written suite looking valid
            shutil.rmtree(target, ignore_errors=True)
            manifest.pop(name, None)
            manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
            print(f"{name}: FAILED: {error}", file=sys.stderr)
            return 1
        manifest[name] = {
            "repo": spec["repo"],
            "pin": pin(spec),
            "license": spec["license"],
            "files": hashes,
        }
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        print(f"{name}: {len(hashes)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
