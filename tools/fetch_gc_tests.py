#!/usr/bin/env python3
"""Download the GameCube test suites and ISA data at pinned commits.

Everything lands in roms/gc/<suite>/ (gitignored) together with a
MANIFEST.json that records the source repository, commit (or release tag),
license and a SHA-256 for every file. Re-running is a no-op for suites whose
manifest entry already matches the pin.

    python tools/fetch_gc_tests.py                 # every suite
    python tools/fetch_gc_tests.py --suite hwtests
    python tools/fetch_gc_tests.py --list

Suites without a license, and GPL suites, are for local use only: never
commit them, never copy them into the repository, never redistribute them.
GPL suites are built and run as tools (see docs/GC-PLAN.md, "What we
reuse"); their code is not read for implementation.

hwtests needs devkitPPC to build. This script only downloads the source;
tools/gc/build_hwtests.py builds it when DEVKITPRO is set.
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
DEFAULT_DEST = REPO_ROOT / "roms" / "gc"

# name -> spec. Kinds:
#   files:   raw files at a pinned commit
#   tarball: the repository at a pinned commit, optionally filtered by glob
#   release: a release asset (zip) pinned by tag and SHA-256, extracted
SUITES = {
    "hwtests": {
        "repo": "dolphin-emu/hwtests",
        "commit": "f28077b139eec18967f60db6ce1e15b182dfeac0",
        "license": "GPL-2.0 (local use only: built and run as a tool, never copied or redistributed)",
        "exclude": ["*/Externals/*"],
        "note": "The hardware-verified suite. cputest: cr, fctiw, fctiwz, fprf, frsp, load, mtspr, "
                "nan, ni, pairedmove, reciprocal, rlw, srawix. gxtest: tev, rasterization, clipping, "
                "lighting, copyfilter, intensity. Verdicts come out through a weak OSReport symbol.",
    },
    "hwtests/Externals/fmt": {
        "repo": "fmtlib/fmt",
        "commit": "e69e5f977d458f2650bb346dadf2ad30c5320281",
        "license": "MIT",
        "note": "hwtests keeps fmt as a submodule and its build will not configure without it. "
                "A source tarball carries no submodules, so it is fetched as its own suite into "
                "the directory hwtests expects.",
    },
    "ppcisa": {
        "repo": "encounter/powerpc-rs",
        "commit": "4643ee0fd3275e3a89dee709185c37968acca83b",
        "license": "MIT OR Apache-2.0",
        "files": ["isa.yaml", "LICENSE-MIT", "LICENSE-APACHE", "README.md"],
        "note": "Machine-readable PowerPC ISA definition including the 750CXe paired-single "
                "extension. Generates our decode classifier and disassembler tables.",
    },
    "gcexamples": {
        "repo": "devkitPro/gamecube-examples",
        "commit": "1fc38100b1e8d06c743904f1b9d8f95a022ee7a1",
        "license": "none (local use only, never commit or redistribute)",
        "note": "libogc demo programs: VI modes, PAD, DVD, CARD, AX and GX bring-up. Small, "
                "well-understood programs to bring the machine model up against before hwtests.",
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
            print(f"{name:12} {spec['repo']}@{pin(spec)[:16]}  [{spec['license']}]  {spec['note']}")
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
