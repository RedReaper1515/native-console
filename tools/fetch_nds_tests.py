#!/usr/bin/env python3
"""Download the Nintendo DS test suites and reference data at pinned commits.

Everything lands in roms/nds/<suite>/ (gitignored) together with a
MANIFEST.json that records the source repository, commit, license and a
SHA-256 for every file. Re-running is a no-op for suites whose manifest entry
already matches the pin.

    python tools/fetch_nds_tests.py                 # every suite
    python tools/fetch_nds_tests.py --suite rockwrestler
    python tools/fetch_nds_tests.py --list

The ARM7 is an ARM7TDMI, so its per-instruction vectors are the ones
tools/fetch_gba_tests.py already downloads into roms/gba/sst/ (900 MB). They
are not fetched a second time.
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
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO_ROOT / "roms" / "nds"

NO_LICENSE = "none (local use only, never commit or redistribute)"

# name -> repository, commit, license and how to fetch:
#   "files"    exact paths, fetched one by one
#   "prefixes" the repository tarball, keeping only paths under these prefixes
#   "release"  one release asset (a zip) pinned by SHA-256, unpacked in place
#   none       the whole repository tarball
SUITES = {
    "armwrestler": {
        "repo": "Atem2069/armwrestler-fixed",
        "commit": "de92073daac1838c0f01339f285e312b4865a4c6",
        "license": NO_LICENSE,
        "note": "ARM/Thumb/ARMv5TE instruction test ROM for both CPUs; armwrestler.nds is prebuilt.",
    },
    "rockwrestler": {
        "repo": "RockPolish/rockwrestler",
        "commit": "b0067a06cddb9bbc9c46ecf0c60a2d572594e581",
        "license": NO_LICENSE,
        "note": "ARMv5 additions, interworking, IPCSYNC/IPCFIFO, DIV/SQRT, WRAMCNT, VRAMCNT, TCM; rockwrestler.nds is prebuilt.",
    },
    "arm7wrestler": {
        "repo": "Arisotura/arm7wrestler",
        "commit": "05137604887ee1d184ec5c26899782151dca270f",
        "license": NO_LICENSE,
        "note": "armwrestler run on the ARM7. Source only; build with devkitARM.",
    },
    "polyrastertest": {
        "repo": "Jaklyy/polyrastertest",
        "commit": "6fd29def45f55870978ea83bc073d2fd6a4f6412",
        "license": "MIT",
        "note": "3D rasteriser tests with reference data recorded on hardware (source and data).",
    },
    "polyrastertest-rom": {
        "repo": "Jaklyy/polyrastertest",
        "commit": "6fd29def45f55870978ea83bc073d2fd6a4f6412",
        "license": "MIT",
        "release": {
            "tag": "v1.0.2-b",
            "asset": "polyrastertestv1.0.2-b.zip",
            "sha256": "56f6a80d526808dd7afeb888e3cf5408f1c4e5beb03fe0d92be79a5583c6a6ba",
        },
        "note": "The prebuilt polyrastertest ROM from the v1.0.2-b release.",
    },
    "nds-interp": {
        "repo": "StrikerX3/nds-interp",
        "commit": "e3acde4c35ac53e628a2548238870a287409733e",
        "license": "MIT",
        "note": "Edge-slope and interpolation captures from hardware (7z archives), their generator and a reference slope.h.",
    },
    "striker-tests": {
        "repo": "StrikerX3/nds-tests",
        "commit": "8624e60256474a48423d71b61b7657f8c400dc3a",
        "license": NO_LICENSE,
        "note": "2D window tests (basic, HBlank, mid-frame). Source only; build with devkitARM.",
    },
    "blocksds-tests": {
        "repo": "blocksds/sdk",
        "commit": "b6da002e457ba4fc2b4a32baf84cc5cbdf2f2bb1",
        "license": "per file: CC0-1.0, MIT or Zlib (see licenses/)",
        "prefixes": ["tests/", "licenses/"],
        "note": "BlocksDS hardware tests (IPC, IRQ, MPU, TCM, SWI, DIV/SQRT, cache, textures). Source only; build in the skylyrac/blocksds Docker image.",
    },
    "freebios": {
        "repo": "melonDS-emu/melonDS",
        "commit": "906e9ebb27da8c6a715cd7abab4abfe8a8d29427",
        "license": "BSD-2-Clause (the freebios/ directory only; the rest of melonDS is GPL-3.0 and is not fetched)",
        "prefixes": ["freebios/"],
        "note": "DraStic replacement ARM7/ARM9 BIOS source by Gilead Kutnick. tools/nds/build_freebios.py assembles it into the BIOS images the runtime boots with.",
    },
    "skyemu": {
        "repo": "skylersaleh/SkyEmu",
        "commit": "01516d6798e3652b583e6a366085bb51c43b528d",
        "license": "MIT",
        "files": ["src/nds_rom_database.h", "LICENSE"],
        "note": "SkyEmu's DS ROM database: which save chip each game's card has. tools/nds/gen_save_db.py converts it into runtime-nds/src/nds_save_db.c.",
    },
    "ph-decomp": {
        "repo": "zeldaret/ph",
        "commit": "29e3578e388b2ff350388e2cdfe45497c4ebdae2",
        "license": "CC0-1.0",
        "prefixes": ["config/usa/", "config/eur/", "docs/", "LICENSE", "ph_usa.sha1", "ph_eur.sha1"],
        "note": "Phantom Hourglass decompilation config: function symbols, relocations and module hashes for the main binary, ITCM, DTCM and all 62 overlays.",
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
    prefixes = spec.get("prefixes")
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
            if prefixes and not any(rel.as_posix().startswith(p) for p in prefixes):
                continue
            out = target / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            blob = archive.extractfile(member).read()
            out.write_bytes(blob)
            hashes[rel.as_posix()] = sha256(blob)
    if not hashes:
        raise RuntimeError("the archive had none of the expected paths")
    return hashes


def fetch_release(spec: dict, target: Path) -> dict:
    release = spec["release"]
    url = (f"https://github.com/{spec['repo']}/releases/download/"
           f"{release['tag']}/{release['asset']}")
    print(f"  {url}", flush=True)
    data = http_get(url)
    digest = sha256(data)
    # A release asset can be replaced under the same tag, so it is pinned by
    # hash rather than by the commit.
    if digest != release["sha256"]:
        raise RuntimeError(f"{release['asset']}: SHA-256 {digest}, expected {release['sha256']}")
    hashes = {}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            rel = Path(info.filename)
            if rel.is_absolute() or ".." in rel.parts:
                continue
            out = target / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            blob = archive.read(info)
            out.write_bytes(blob)
            hashes[rel.as_posix()] = sha256(blob)
    if not hashes:
        raise RuntimeError(f"{release['asset']} is empty")
    return hashes


def pin_of(spec: dict) -> str:
    release = spec.get("release")
    return f"{spec['commit']}+{release['sha256']}" if release else spec["commit"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--suite", action="append", choices=sorted(SUITES))
    parser.add_argument("--force", action="store_true", help="re-download even if the pin matches")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        for name, spec in SUITES.items():
            print(f"{name:19} {spec['repo']}@{spec['commit'][:10]}  [{spec['license']}]  {spec['note']}")
        return 0

    args.dest.mkdir(parents=True, exist_ok=True)
    manifest_path = args.dest / "MANIFEST.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}

    for name in args.suite or list(SUITES):
        spec = SUITES[name]
        target = args.dest / name
        previous = manifest.get(name)
        if (not args.force and previous and previous.get("pin") == pin_of(spec)
                and target.is_dir()):
            print(f"{name}: up to date ({spec['commit'][:10]})")
            continue
        print(f"{name}: fetching {spec['repo']}@{spec['commit'][:10]}")
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        try:
            if "release" in spec:
                hashes = fetch_release(spec, target)
            elif "files" in spec:
                hashes = fetch_files(spec, target)
            else:
                hashes = fetch_tarball(spec, target)
        except Exception as error:  # fail closed: never leave a half-written suite looking valid
            shutil.rmtree(target, ignore_errors=True)
            manifest.pop(name, None)
            manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
            print(f"{name}: FAILED: {error}", file=sys.stderr)
            return 1
        manifest[name] = {
            "repo": spec["repo"],
            "commit": spec["commit"],
            "pin": pin_of(spec),
            "license": spec["license"],
            "files": hashes,
        }
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        print(f"{name}: {len(hashes)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
