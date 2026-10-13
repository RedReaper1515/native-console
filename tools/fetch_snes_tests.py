#!/usr/bin/env python3
"""Download the SNES test suites at pinned commits.

Everything lands in roms/snes/<suite>/ (gitignored) together with a
MANIFEST.json that records the source repository, commit (or release tag),
license and a SHA-256 for every file. Re-running is a no-op for suites whose
manifest entry already matches the pin.

    python tools/fetch_snes_tests.py                 # every suite
    python tools/fetch_snes_tests.py --suite sst65816
    python tools/fetch_snes_tests.py --list

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
DEFAULT_DEST = REPO_ROOT / "roms" / "snes"

HEX = [f"{i:02x}" for i in range(256)]

# name -> spec. Kinds:
#   files:   raw files at a pinned commit
#   tarball: the repository at a pinned commit, optionally filtered by glob
#   release: a release asset (zip) pinned by tag and SHA-256, extracted
SUITES = {
    "sst65816": {
        "repo": "SingleStepTests/65816",
        "commit": "db6b10401729d5f20f2181dde5d3d7b037093a4a",
        "license": "none (local use only, never commit or redistribute)",
        "files": [f"v1/{op}.{mode}.json" for op in HEX for mode in ("e", "n")] + ["README.md"],
        "note": "Per-instruction 65816 vectors with bus activity, 10,000 native + 10,000 emulation per opcode (~2.9 GB).",
    },
    "sstspc700": {
        "repo": "SingleStepTests/spc700",
        "commit": "67d15f492b2740964abd4efd1229e0ec9c342228",
        "license": "MIT",
        "files": [f"v1/{op}.json" for op in HEX] + ["LICENSE"],
        "note": "Per-instruction SPC700 vectors with bus activity (~84 MB).",
    },
    "gilyon": {
        "repo": "gilyon/snes-tests",
        "tag": "v1.4",
        "url": "https://github.com/gilyon/snes-tests/releases/download/v1.4/snes-tests.zip",
        "sha256": "c3a39e15a9f3789fe143e54dfa035c988204576eafacf97df8ff3546a917c5e9",
        "license": "MIT",
        "note": "cputest-full/basic (every 65C816 opcode and addressing mode, wrapping, decimal mode) and spctest.",
    },
    "undisbeliever": {
        "repo": "undisbeliever/snes-test-roms",
        "tag": "v20210217",
        "url": "https://github.com/undisbeliever/snes-test-roms/releases/download/v20210217/snes-test-roms-20210217.zip",
        "sha256": "00945927bee494317663accaeb21d854b6cdfef674813e6c119877a8eeacb1c4",
        "license": "Zlib",
        "note": "HDMA, INIDISP, VMAIN, OBJ and DMA behaviour ROMs.",
    },
    "absindx": {
        "repo": "absindx/SNES-TestRoms",
        "tag": "20241020",
        "url": "https://github.com/absindx/SNES-TestRoms/releases/download/20241020/SnesTestRoms.zip",
        "sha256": "0dbd46f44025485cd9d80ed0a77be51dd5b8ea7f3e41414a4d62af59bcf6c808",
        "license": "BSL-1.0",
        "note": "SA-1 RAM protection and reboot tests, SA-1 register and open-bus dumps.",
    },
    "peterlemon": {
        "repo": "PeterLemon/SNES",
        "commit": "350b394e86ec5d62f600b5cbf64cdce3721bb6ef",
        "license": "none (local use only, never commit or redistribute)",
        "include": ["*/CPUTest/*.sfc", "*/CPUTest/*.png", "*/CPUTest/*.spc", "*/PPU/*.sfc", "*/PPU/*.png",
                    "*/SPC700/*.sfc", "*/SPC700/*.png", "*/BANK/*.sfc", "*/BANK/*.png", "*/INPUT/*.sfc"],
        "note": "krom's CPU/SPC700 instruction tests (PASS/FAIL on screen, reference PNGs) and PPU demos.",
    },
    "higan": {
        "repo": "akatsuki105/snes-test-roms",
        "commit": "8223a0807f2b467a9923bbe455917c51d10aa26e",
        "license": "none (mixed authors; local use only, never commit or redistribute)",
        "exclude": ["*/PeterLemon/*"],
        "note": "Mirror of gitlab.com/higan/snes-test-roms: blargg SPC, KungFuFurby NMI/IRQ/HDMA, "
                "Sour timing, jonasquinn/anomie collection, undisbeliever INIDISP, 93143 HDMA.",
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
