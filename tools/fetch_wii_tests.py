#!/usr/bin/env python3
"""Download the Wii reference sources at pinned commits.

Everything lands in roms/wii/<suite>/ (gitignored) together with a
MANIFEST.json that records the source repository, commit, license and a
SHA-256 for every file. Re-running is a no-op for suites whose manifest entry
already matches the pin.

    python tools/fetch_wii_tests.py                 # every suite
    python tools/fetch_wii_tests.py --suite libogc
    python tools/fetch_wii_tests.py --list

What the Wii shares with the GameCube - dolphin-emu/hwtests (its iostest
directory is the Wii's), the PowerPC ISA table - is fetched by
tools/fetch_gc_tests.py into roms/gc/ and is not repeated here.

Nothing here is copied into the repository. libogc is read for the client
side of every IOS request and its formats; the test programs under
tests/wii/programs are built against the copy devkitPro installs, not this
one. Suites without a license are for local use only.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_gc_tests as fetch  # noqa: E402  (the machinery is the GameCube script's)

SUITES = {
    "libogc": {
        "repo": "devkitPro/libogc",
        "commit": "c70bdf2fc36bf6c6c12d946ae3ad70d604366d1b",
        "license": "zlib (wiiuse/ is GPL-3 and is left out; lwbt/ is BSD)",
        "exclude": ["*/wiiuse/*"],
        "note": "v3.1.0, the release the installed devkitPro libraries are. The client side of IOS: "
                "ipc.c, isfs.c, es.c, stm.c, conf.c, usb.c, libdi, and lwbt (the homebrew Bluetooth "
                "stack). What a program that uses a Wii's hardware looks like.",
    },
    "ogws-bte": {
        "repo": "doldecomp/ogws",
        "commit": "27fa94a593096eed7442efc4e70981fd52025b9c",
        "license": "Apache-2.0 (Broadcom's Bluetooth stack, as the repository's NOTICE says; "
                   "the rest of the repository is not fetched)",
        "include": ["*/src/revolution/BTE/*", "*/include/revolution/BTE/*", "/NOTICE", "/README.md"],
        "note": "The Bluetooth stack a retail game links: which HCI commands it sends at start-up, "
                "what it does when a remote connects. Read as a reference for the emulated controller.",
    },
    "achurch": {
        "repo": "achurch.org/cpu-tests",
        "url": "https://achurch.org/cpu-tests/ppc750cl.s",
        "name": "ppc750cl.s",
        "tag": "2019-01-11",
        "sha256": "9d15ef92ca0470a99bac72661f74a6f12e035bcd96a85819c761b5852ad097be",
        "license": "public domain (\"No copyright is claimed on this file\")",
        "note": "Andrew Church's test of every PowerPC 750CL instruction, paired singles included, "
                "validated on the real processor. tools/wii/build_wii_programs.py assembles it into "
                "wii_cputest.",
    },
    "wii-examples": {
        "repo": "devkitPro/wii-examples",
        "commit": "ef17c1fcc3dfde90880a5281a114feff931411d6",
        "license": "none (local use only, never commit or redistribute)",
        "note": "libogc demo programs for the Wii: remotes, the flash filesystem, GX. Smoke tests "
                "with no verdict of their own.",
    },
}

def fetch_plain(spec: dict, target: Path) -> dict:
    """One file at a URL, held to its SHA-256: a suite that is not a repository."""
    print(f"  {spec['url']}", flush=True)
    data = fetch.http_get(spec["url"])
    digest = fetch.sha256(data)
    if digest != spec["sha256"]:
        raise RuntimeError(f"{spec['url']} has sha256 {digest}, not the pinned {spec['sha256']}")
    (target / spec["name"]).write_bytes(data)
    return {spec["name"]: digest}


if __name__ == "__main__":
    zipped = fetch.fetch_release
    fetch.fetch_release = lambda spec, target: fetch_plain(spec, target) if "name" in spec else zipped(spec, target)
    fetch.SUITES = SUITES
    fetch.DEFAULT_DEST = fetch.REPO_ROOT / "roms" / "wii"
    fetch.__doc__ = __doc__
    # argparse reads the defaults when main() builds its parser, which is now.
    sys.exit(fetch.main())
