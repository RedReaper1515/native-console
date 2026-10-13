#!/usr/bin/env python3
"""Download the Nintendo 64 test suites at pinned commits.

Everything lands in roms/n64/<suite>/ (gitignored) together with a
MANIFEST.json that records the source repository, commit, license and a
SHA-256 for every file. Re-running is a no-op for suites whose manifest entry
already matches the pin.

    python tools/fetch_n64_tests.py                 # every suite
    python tools/fetch_n64_tests.py --suite krom
    python tools/fetch_n64_tests.py --list

Suites are fetched with git (a shallow, blob-filtered clone of the pinned
commit with a sparse checkout of the directories named), so only the files a
suite needs are downloaded. Suites without a license are for local testing
only: never commit them, never copy them into the repository, never
redistribute them.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO_ROOT / "roms" / "n64"

# name -> spec: a GitHub repository, a commit, the directories to check out,
# and the files to keep from them (globs on the path inside the repository).
SUITES = {
    "krom": {
        "repo": "PeterLemon/N64",
        "commit": "7085543e4a19d8c539fc9e0a4d2869e788b4ed4b",
        "license": "Unlicense (public domain)",
        "dirs": ["CPUTest", "CP1", "RSPTest", "RDP", "RDPTest", "Interrupt", "RDRAMTest"],
        "keep": ["*.N64", "*.png", "*.asm"],
        "note": "Peter Lemon's bare-metal tests: CPU, FPU, RSP and RDP, each with a reference screenshot. "
                "The ROMs carry Nintendo's 6102 boot code; local use only.",
    },
    "n64-systemtest": {
        "repo": "lemmy-64/n64-systemtest",
        "commit": "196f5421173220eb2f63a7a99c64795dc0ea0698",
        "license": "MIT",
        "dirs": ["src"],
        "keep": ["*"],
        "note": "Source only (a Rust program; building it needs a nightly toolchain, see docs/N64-PLAN.md). "
                "Its verdict is printed through the ISViewer port.",
    },
    "n64-tests": {
        "repo": "Dillonb/n64-tests",
        "commit": "bffe242e2fd7f177cc60136aa6e854c7c5eb32ec",
        "license": "none (local use only, never commit or redistribute)",
        "dirs": ["src"],
        "keep": ["*"],
        "note": "Source only (bass assembler). Each test reports in r30: -1 passes, n is the failing case.",
    },
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(*args: str, cwd: Path) -> str:
    result = subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True)
    return result.stdout.strip()


def fetch(spec: dict, target: Path) -> dict:
    url = f"https://github.com/{spec['repo']}.git"
    with tempfile.TemporaryDirectory(prefix="n64fetch-") as tmp:
        work = Path(tmp)
        git("init", "-q", cwd=work)
        git("remote", "add", "origin", url, cwd=work)
        git("config", "core.sparseCheckout", "true", cwd=work)
        (work / ".git" / "info" / "sparse-checkout").write_text("".join(f"/{d}/\n" for d in spec["dirs"]))
        print(f"  {url}@{spec['commit'][:12]}", flush=True)
        git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", spec["commit"], cwd=work)
        git("checkout", "-q", "FETCH_HEAD", cwd=work)
        head = git("rev-parse", "HEAD", cwd=work)
        if head != spec["commit"]:
            raise RuntimeError(f"checked out {head}, expected {spec['commit']}")
        hashes = {}
        for d in spec["dirs"]:
            for path in sorted((work / d).rglob("*")):
                if not path.is_file():
                    continue
                rel = path.relative_to(work).as_posix()
                if not any(fnmatch.fnmatch(path.name, k) for k in spec["keep"]):
                    continue
                out = target / rel
                out.parent.mkdir(parents=True, exist_ok=True)
                data = path.read_bytes()
                out.write_bytes(data)
                hashes[rel] = sha256(data)
    return hashes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--suite", action="append", choices=sorted(SUITES))
    parser.add_argument("--force", action="store_true", help="re-download even if the pin matches")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        for name, spec in SUITES.items():
            print(f"{name:15} {spec['repo']}@{spec['commit'][:12]}  [{spec['license']}]  {spec['note']}")
        return 0

    args.dest.mkdir(parents=True, exist_ok=True)
    manifest_path = args.dest / "MANIFEST.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}

    for name in args.suite or list(SUITES):
        spec = SUITES[name]
        target = args.dest / name
        previous = manifest.get(name)
        if not args.force and previous and previous.get("pin") == spec["commit"] and target.is_dir():
            print(f"{name}: up to date ({spec['commit'][:12]})")
            continue
        print(f"{name}: fetching {spec['repo']}@{spec['commit'][:12]}")
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        try:
            hashes = fetch(spec, target)
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
            "pin": spec["commit"],
            "license": spec["license"],
            "files": hashes,
        }
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        print(f"{name}: {len(hashes)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
