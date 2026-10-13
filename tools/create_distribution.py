#!/usr/bin/env python3
"""Create a versioned, provenance-complete Native Console distribution.

For every console in CONSOLES the distribution holds its recompiler (from
--bin-dir) and its runtime folder (from --source-root); the hub is added at
the root when --hub is given, so its auto-detection finds the recompilers
next to it. native-console-release.json records every file's hash, each
recompiler's --version-json identity and each runtime's tree hash.

A Windows release also carries SDL2's development files (--sdl2-dev, the
unpacked SDL2-devel-*-VC package) in SDL2/: every game is compiled on the
user's computer against SDL2, and the hub looks for SDL2/cmake beside
itself, so that nothing has to be downloaded and pointed at by hand.

Adding a console to the release is one CONSOLES row here plus its smoke
recipe in tests/test_release_bundle.py.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


MANIFEST_NAME = "native-console-release.json"
MANIFEST_SCHEMA = "native-console.release"
MANIFEST_VERSION = 2
HUB_NAME = "native_console_hub"
EXE_SUFFIX = ".exe" if os.name == "nt" else ""


@dataclass(frozen=True)
class Console:
    family: str  # the hub's family id
    recompiler: str  # executable name without extension
    runtime: str  # runtime folder, the same relative path in the checkout and the archive
    required: tuple[str, ...]  # runtime files a generated project cannot build without


CONSOLES = (
    Console(
        "gb",
        "gbrecomp",
        "runtime",
        (
            "CMakeLists.txt",
            "include/gbrt.h",
            "include/gbrt_presentation.h",
            "src/gbrt.c",
            "src/gbrt_presentation.c",
            "vendor/imgui/imgui.cpp",
            "vendor/imgui/LICENSE.txt",
        ),
    ),
    # Generated GBA, SNES, NES, DS and GameCube projects take Dear ImGui from
    # runtime/vendor/imgui, which the Game Boy row ships.
    Console(
        "gba",
        "gbarecomp",
        "runtime-gba",
        (
            "include/gba_recomp.h",
            "include/gba.h",
            "src/gba.c",
            "platform/gba_sdl.cpp",
            "cmake/NativeConsoleMacBundle.cmake",
        ),
    ),
    Console(
        "snes",
        "snesrecomp",
        "runtime-snes",
        (
            "include/snes_recomp.h",
            "include/snes.h",
            "src/snes.c",
            "platform/snes_sdl.cpp",
            "cmake/NativeConsoleMacBundle.cmake",
        ),
    ),
    Console(
        "nes",
        "nesrecomp",
        "runtime-nes",
        (
            "include/nes_recomp.h",
            "include/nes.h",
            "src/nes.c",
            "platform/nes_sdl.cpp",
            "cmake/NativeConsoleMacBundle.cmake",
        ),
    ),
    Console(
        "nds",
        "ndsrecomp",
        "runtime-nds",
        (
            "include/nds_recomp.h",
            "include/nds.h",
            "src/nds.c",
            "platform/nds_sdl.cpp",
            "cmake/NativeConsoleMacBundle.cmake",
        ),
    ),
    Console(
        "gc",
        "gcrecomp",
        "runtime-gc",
        (
            "CMakeLists.txt",
            "include/gc.h",
            "include/gc_runner.h",
            "src/gc.c",
            "src/gc_runner.c",
            "platform/gc_sdl.cpp",
        ),
    ),
    Console(
        "n64",
        "n64recomp",
        "runtime-n64",
        (
            "include/n64_recomp.h",
            "include/n64.h",
            "src/n64.c",
            "src/n64_runner.c",
            "platform/n64_sdl.cpp",
            "cmake/NativeConsoleMacBundle.cmake",
        ),
    ),
)

# SDL2's development files as a release carries them: the layout of the
# SDL2-devel-*-VC package, which its own sdl2-config.cmake expects, with only
# what a generated project needs to configure, compile, link and run.
SDL2_DEV_FOLDER = "SDL2"
SDL2_DEV_FILES = (
    "cmake/sdl2-config.cmake",
    "cmake/sdl2-config-version.cmake",
    "lib/x64/SDL2.dll",
    "lib/x64/SDL2.lib",
    "lib/x64/SDL2main.lib",
)
SDL2_DEV_LICENSES = ("LICENSE.txt", "COPYING.txt")


class DistributionError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalized_text_bytes(path: Path) -> bytes:
    content = path.read_bytes()
    if b"\0" not in content:
        content = content.replace(b"\r\n", b"\n")
    return content


def tree_sha256(root: Path, *, clean_source: bool = False) -> str:
    digest = hashlib.sha256()
    paths = (candidate for candidate in root.rglob("*") if candidate.is_file())
    for path in sorted(paths, key=lambda candidate: candidate.relative_to(root).as_posix()):
        relative = path.relative_to(root)
        if ".DS_Store" in relative.parts or "__pycache__" in relative.parts:
            continue
        if clean_source and any(
            part == "build" or part.startswith("build_")
            for part in relative.parts
        ):
            continue
        if path.suffix == ".pyc":
            continue
        content = normalized_text_bytes(path)
        encoded = relative.as_posix().encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "big"))
        digest.update(encoded)
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(hashlib.sha256(content).digest())
    return digest.hexdigest()


def normalize_runtime_text_files(root: Path) -> None:
    for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
        content = path.read_bytes()
        normalized = content.replace(b"\r\n", b"\n") if b"\0" not in content else content
        if normalized != content:
            path.write_bytes(normalized)


def require_file(path: Path, label: str, *, executable: bool = False) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise DistributionError(f"missing {label}: {resolved}")
    if resolved.is_symlink():
        raise DistributionError(f"{label} may not be a symlink: {resolved}")
    if executable and not os.access(resolved, os.X_OK):
        raise DistributionError(f"{label} is not executable: {resolved}")
    return resolved


def require_directory(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_dir() or path.is_symlink():
        raise DistributionError(f"invalid {label}: {resolved}")
    return resolved


def ignore_runtime(_directory: str, names: list[str]) -> set[str]:
    ignored: set[str] = set()
    for name in names:
        if (
            name == ".DS_Store"
            or name == "__pycache__"
            or name == "build"
            or name.startswith("build_")
            or name.endswith(".pyc")
        ):
            ignored.add(name)
    return ignored


def load_identity(executable: Path, tool: str) -> dict[str, Any]:
    completed = subprocess.run(
        [str(executable), "--version-json"],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise DistributionError(
            f"{tool} --version-json exited {completed.returncode}: "
            f"{completed.stderr.strip()}"
        )
    try:
        value = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise DistributionError(f"{tool} returned malformed version JSON") from error
    expected_keys = {"schema", "schema_version", "version", "abis", "features"}
    if (
        not isinstance(value, dict)
        or set(value) != expected_keys
        or value.get("schema") != f"{tool}.version"
        or value.get("schema_version") != 1
        or not isinstance(value.get("version"), str)
        or not isinstance(value.get("abis"), dict)
        or not isinstance(value.get("features"), list)
    ):
        raise DistributionError(f"{tool} returned unsupported version identity")
    return value


def copy_sdl2_dev(package: Path, output: Path) -> str:
    """Copies the part of an unpacked SDL2-devel-*-VC package a generated
    project builds against into output/SDL2, and returns its cmake folder."""
    package = require_directory(package, "SDL2 development package")
    destination = output / SDL2_DEV_FOLDER
    if destination.exists():
        raise DistributionError(f"two things would be written to {destination}")
    headers = sorted(path for path in (package / "include").glob("*.h") if path.is_file())
    if not headers:
        raise DistributionError(f"no SDL2 headers in {package / 'include'}")
    license_path = next(
        (package / name for name in SDL2_DEV_LICENSES if (package / name).is_file()), None
    )
    if license_path is None:
        raise DistributionError(f"no SDL2 license in {package}")
    files = [(require_file(package / relative, "SDL2 development file"), relative) for relative in SDL2_DEV_FILES]
    files += [(header, f"include/{header.name}") for header in headers]
    files.append((license_path, "LICENSE.txt"))
    for source, relative in files:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    return f"{SDL2_DEV_FOLDER}/cmake"


def inventory(root: Path) -> list[dict[str, object]]:
    files = []
    for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
        relative = path.relative_to(root).as_posix()
        if relative == MANIFEST_NAME:
            continue
        if path.is_symlink():
            raise DistributionError(f"distribution contains a symlink: {relative}")
        files.append(
            {
                "path": relative,
                "size": path.stat().st_size,
                "sha256": sha256(path),
            }
        )
    return files


def copy_executable(source: Path, destination: Path) -> None:
    if destination.exists():
        raise DistributionError(f"two files would be written to {destination}")
    shutil.copy2(source, destination)
    destination.chmod(destination.stat().st_mode | 0o111)


def main() -> int:
    repository = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--bin-dir",
        type=Path,
        required=True,
        help="folder holding every console's recompiler (a build's bin/)",
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=repository,
        help="checkout holding the runtime folders (default: this repository)",
    )
    parser.add_argument("--hub", type=Path, help="hub executable to ship at the root")
    parser.add_argument("--license", type=Path, required=True)
    parser.add_argument("--readme", type=Path)
    parser.add_argument(
        "--extra-file",
        action="append",
        default=[],
        metavar="SOURCE=DESTINATION",
        help="copy an additional platform file into a safe relative path",
    )
    parser.add_argument(
        "--sdl2-dev",
        type=Path,
        metavar="DIR",
        help="an unpacked SDL2-devel-*-VC package: its cmake, include and lib/x64 "
        "are shipped in SDL2/, where the hub finds them (Windows)",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", dest="platform_name")
    parser.add_argument("--architecture")
    args = parser.parse_args()

    bin_dir = require_directory(args.bin_dir, "recompiler folder")
    source_root = require_directory(args.source_root, "source root")
    sources = []
    for console in CONSOLES:
        recompiler = require_file(
            bin_dir / (console.recompiler + EXE_SUFFIX),
            f"{console.family} recompiler",
            executable=True,
        )
        runtime = require_directory(
            source_root / console.runtime, f"{console.family} runtime source"
        )
        for relative in console.required:
            require_file(runtime / relative, f"{console.family} runtime file")
        identity = load_identity(recompiler, console.recompiler)
        sources.append((console, recompiler, runtime, identity))
    versions = sorted({identity["version"] for *_, identity in sources})
    if len(versions) != 1:
        raise DistributionError(f"recompilers disagree on the version: {versions}")
    hub = require_file(args.hub, "hub", executable=True) if args.hub else None
    license_path = require_file(args.license, "license")
    readme = require_file(args.readme, "readme") if args.readme else None

    output = args.output.expanduser().resolve()
    if output.exists():
        raise DistributionError(f"output already exists: {output}")
    output.mkdir(parents=True)

    consoles: dict[str, Any] = {}
    for console, recompiler, runtime, identity in sources:
        copied_recompiler = output / (console.recompiler + EXE_SUFFIX)
        copy_executable(recompiler, copied_recompiler)
        copied_runtime = output / console.runtime
        shutil.copytree(runtime, copied_runtime, ignore=ignore_runtime)
        normalize_runtime_text_files(copied_runtime)
        runtime_hash = tree_sha256(copied_runtime)
        if tree_sha256(runtime, clean_source=True) != runtime_hash:
            raise DistributionError(
                f"copied {console.runtime} differs from its source"
            )
        consoles[console.family] = {
            "cli": {
                "path": copied_recompiler.name,
                "sha256": sha256(copied_recompiler),
                "identity": identity,
            },
            "runtime": {
                "path": console.runtime,
                "tree_sha256": runtime_hash,
            },
        }

    hub_entry = None
    if hub is not None:
        copied_hub = output / (HUB_NAME + EXE_SUFFIX)
        copy_executable(hub, copied_hub)
        hub_entry = {"path": copied_hub.name, "sha256": sha256(copied_hub)}

    shutil.copy2(license_path, output / "LICENSE")
    if readme is not None:
        shutil.copy2(readme, output / "README.md")
    for specification in args.extra_file:
        if "=" not in specification:
            raise DistributionError("--extra-file requires SOURCE=DESTINATION")
        source_raw, destination_raw = specification.split("=", 1)
        source = require_file(Path(source_raw), "extra file")
        destination_relative = Path(destination_raw)
        if (
            destination_relative.is_absolute()
            or ".." in destination_relative.parts
            or not destination_relative.parts
        ):
            raise DistributionError("extra-file destination is unsafe")
        destination = output / destination_relative
        if destination.exists():
            raise DistributionError(f"extra-file destination exists: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    sdl2_cmake = copy_sdl2_dev(args.sdl2_dev, output) if args.sdl2_dev else None

    payload: dict[str, Any] = {
        "schema": MANIFEST_SCHEMA,
        "version": MANIFEST_VERSION,
        "release": {
            "tool_version": versions[0],
            "platform": args.platform_name or sys.platform,
            "architecture": args.architecture or platform.machine(),
        },
        "consoles": consoles,
        "files": inventory(output),
    }
    if hub_entry is not None:
        payload["hub"] = hub_entry
    if sdl2_cmake is not None:
        payload["sdl2"] = {"cmake_dir": sdl2_cmake}
    manifest = output / MANIFEST_NAME
    manifest.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"ok  distribution: {output}")
    print(f"    version={versions[0]}")
    for family, entry in consoles.items():
        print(
            f"    {family}: {entry['cli']['path']} + {entry['runtime']['path']}/ "
            f"tree_sha256={entry['runtime']['tree_sha256']}"
        )
    if hub_entry is not None:
        print(f"    hub: {hub_entry['path']}")
    if sdl2_cmake is not None:
        print(f"    sdl2: {sdl2_cmake}")
    print(f"    manifest_sha256={sha256(manifest)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (DistributionError, OSError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
