#!/usr/bin/env python3
"""Prove a relocated Native Console release works for every console it ships.

Checks the manifest against the files, then for each console recompiles a
test ROM with the shipped recompiler, builds the project it writes (which
must carry its own runtime snapshot) and runs it headless with a verdict:

  gb   tests/fixtures/make_mapper_rom.py (ROM only): one benchmark frame
  gba  jsmolka arm.gba (tools/fetch_gba_tests.py --suite jsmolka): r12=0
  snes tests/fixtures/make_snes_sram_rom.py: its battery save holds A5 5A
  nes  tests/fixtures/make_nes_sram_rom.py: its battery save holds A5 5A
  nds  tests/fixtures/make_nds_save_rom.py: its save chip holds A5 5A C3 3C
  gc   tests/fixtures/make_gc_dol.py: ends at its idle branch, clean under
       --differential

Tests an extracted archive (--distribution-dir), or a build's bin folder
(--bin-dir), which is first packaged with tools/create_distribution.py.
Only repository-owned or MIT test ROMs are used.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Callable

TESTS = Path(__file__).resolve().parent
REPOSITORY = TESTS.parent
sys.path.insert(0, str(REPOSITORY / "tools"))
import create_distribution as distribution  # noqa: E402

EXE_SUFFIX = distribution.EXE_SUFFIX
CONSOLES = {console.family: console for console in distribution.CONSOLES}


def run(
    command: list[object],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
    timeout: int = 300,
) -> str:
    result = subprocess.run(
        [str(part) for part in command],
        cwd=cwd,
        env=env,
        text=True,
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        raise RuntimeError(
            f"command exited with {result.returncode}: "
            f"{' '.join(str(part) for part in command)}"
        )
    return result.stdout


@dataclass
class Smoke:
    root: Path  # scratch folder for ROMs and generated projects
    cmake_args: list[str]
    env: dict[str, str]  # SDL dummy drivers, the release on PATH
    gba_rom: Path | None

    def build(self, project: Path, name: str, *cmake_args: str) -> Path:
        build = project / "build"
        run(
            ["cmake", "-G", "Ninja", "-S", project, "-B", build, *cmake_args, *self.cmake_args],
            cwd=self.root,
        )
        run(["ninja", "-C", build], cwd=self.root, timeout=1800)
        executable = build / (name + EXE_SUFFIX)
        if executable.is_file():
            return executable
        matches = list(build.rglob(name + EXE_SUFFIX))
        if len(matches) != 1:
            raise RuntimeError(f"could not identify the {name} executable: {matches}")
        return matches[0]


def require_snapshot(runtime: Path, *files: str) -> None:
    missing = [name for name in files if not (runtime / name).is_file()]
    if missing:
        raise RuntimeError(f"generated project lacks its runtime snapshot: {runtime} {missing}")
    if (runtime / "build").exists():
        raise RuntimeError(f"generated runtime snapshot contains build artifacts: {runtime}")


def smoke_gb(smoke: Smoke, recompiler: Path) -> str:
    rom = smoke.root / "release_smoke.gb"
    run(
        [sys.executable, TESTS / "fixtures/make_mapper_rom.py", "--mapper", "rom-only", "--output", rom],
        cwd=smoke.root,
    )
    project = smoke.root / "gb"
    run([recompiler, rom, "--no-scan", "-o", project], cwd=smoke.root)
    require_snapshot(project / "runtime", "include/gbrt.h")
    executable = smoke.build(project, "release_smoke", "-DGBRECOMP_ENABLE_STRIP=OFF")
    run(
        [executable, "--benchmark", "--limit-frames", "1"],
        cwd=smoke.root,
        env={**smoke.env, "GBRECOMP_BENCHMARK": "1"},
    )
    return "mapper fixture ran one benchmark frame"


def smoke_gba(smoke: Smoke, recompiler: Path) -> str:
    if smoke.gba_rom is None:
        raise RuntimeError(
            "the release ships gbarecomp: pass --gba-rom roms/gba/jsmolka/arm/arm.gba "
            "(tools/fetch_gba_tests.py --suite jsmolka) or --skip gba"
        )
    project = smoke.root / "gba"
    run([recompiler, smoke.gba_rom, "-o", project, "--name", "release_smoke_gba"], cwd=smoke.root)
    require_snapshot(project / "runtime-gba", "include/gba_recomp.h", "vendor/imgui/imgui.h")
    executable = smoke.build(project, "release_smoke_gba")
    # jsmolka's ROMs leave 0 in r12 when every test passed.
    output = run(
        [executable, "--until-idle", "--expect", "r12=0", "--frames", "600"],
        cwd=smoke.root,
        env=smoke.env,
    )
    if "PASS" not in output:
        raise RuntimeError(f"{smoke.gba_rom.name} did not pass:\n{output}")
    return f"{smoke.gba_rom.name} passed (r12=0)"


def smoke_snes(smoke: Smoke, recompiler: Path) -> str:
    rom = smoke.root / "release_smoke.sfc"
    run([sys.executable, TESTS / "fixtures/make_snes_sram_rom.py", "--output", rom], cwd=smoke.root)
    project = smoke.root / "snes"
    run([recompiler, rom, "-o", project, "--name", "release_smoke_snes"], cwd=smoke.root)
    require_snapshot(project / "runtime-snes", "include/snes_recomp.h", "vendor/imgui/imgui.h")
    executable = smoke.build(project, "release_smoke_snes")
    # The fixture writes A5 5A to battery SRAM; the frontend saves it on exit.
    saves = smoke.root / "snes-saves"
    run(
        [executable, "--limit-frames", "120", "--mute", "--save-dir", saves],
        cwd=smoke.root,
        env=smoke.env,
    )
    save = saves / "release_smoke_snes.sav"
    if not save.is_file() or save.read_bytes()[:2] != b"\xA5\x5A":
        found = sorted(path.name for path in saves.glob("*")) if saves.is_dir() else []
        raise RuntimeError(f"no save holding A5 5A at {save} (found {found})")
    return "SRAM fixture saved A5 5A"


def smoke_nes(smoke: Smoke, recompiler: Path) -> str:
    rom = smoke.root / "release_smoke.nes"
    run([sys.executable, TESTS / "fixtures/make_nes_sram_rom.py", "--output", rom], cwd=smoke.root)
    project = smoke.root / "nes"
    run([recompiler, rom, "-o", project, "--name", "release_smoke_nes"], cwd=smoke.root)
    require_snapshot(project / "runtime-nes", "include/nes_recomp.h", "vendor/imgui/imgui.h")
    executable = smoke.build(project, "release_smoke_nes")
    # The fixture writes A5 5A to battery RAM; the frontend saves it on exit.
    saves = smoke.root / "nes-saves"
    run(
        [executable, "--limit-frames", "120", "--mute", "--save-dir", saves],
        cwd=smoke.root,
        env=smoke.env,
    )
    save = saves / "release_smoke_nes.sav"
    if not save.is_file() or save.read_bytes()[:2] != b"\xA5\x5A":
        found = sorted(path.name for path in saves.glob("*")) if saves.is_dir() else []
        raise RuntimeError(f"no save holding A5 5A at {save} (found {found})")
    return "battery RAM fixture saved A5 5A"


def smoke_nds(smoke: Smoke, recompiler: Path) -> str:
    rom = smoke.root / "release_smoke.nds"
    run([sys.executable, TESTS / "fixtures/make_nds_save_rom.py", "--output", rom], cwd=smoke.root)
    project = smoke.root / "nds"
    run([recompiler, rom, "-o", project, "--name", "release_smoke_nds"], cwd=smoke.root)
    require_snapshot(project / "runtime-nds", "include/nds_recomp.h", "vendor/imgui/imgui.h")
    executable = smoke.build(project, "release_smoke_nds")
    # The fixture writes A5 5A C3 3C to the card's save chip, which the
    # runtime has to recognise from the write; the frontend saves it on
    # exit. --prefs keeps the run away from the player's settings and the
    # console memory kept with them.
    saves = smoke.root / "nds-saves"
    run(
        [executable, "--limit-frames", "120", "--mute", "--save-dir", saves, "--prefs", saves / "prefs.ini"],
        cwd=smoke.root,
        env=smoke.env,
    )
    save = saves / "release_smoke_nds.sav"
    if not save.is_file() or save.read_bytes()[:4] != b"\xA5\x5A\xC3\x3C":
        found = sorted(path.name for path in saves.glob("*")) if saves.is_dir() else []
        raise RuntimeError(f"no save holding A5 5A C3 3C at {save} (found {found})")
    return "save chip fixture saved A5 5A C3 3C"


def smoke_gc(smoke: Smoke, recompiler: Path) -> str:
    program = smoke.root / "release_smoke.dol"
    run([sys.executable, TESTS / "fixtures/make_gc_dol.py", "--output", program], cwd=smoke.root)
    project = smoke.root / "gc"
    run([recompiler, program, "-o", project, "--name", "release_smoke_gc"], cwd=smoke.root)
    # Dear ImGui comes from the Game Boy runtime beside runtime-gc.
    require_snapshot(project / "runtime-gc", "include/gc_runner.h", "vendor/imgui/imgui.h")
    executable = smoke.build(project, "release_smoke_gc")
    output = run(
        [executable, "--frames", "30", "--differential", "--coverage"],
        cwd=smoke.root,
        env=smoke.env,
    )
    if "pc 8000311c" not in output or "machines agree" not in output:
        raise RuntimeError(f"the GameCube smoke program did not finish cleanly:\n{output}")
    return "DOL fixture reached its idle branch, clean under --differential"


# One recipe per console in tools/create_distribution.py CONSOLES.
SMOKES: dict[str, Callable[[Smoke, Path], str]] = {
    "gb": smoke_gb,
    "gba": smoke_gba,
    "snes": smoke_snes,
    "nes": smoke_nes,
    "nds": smoke_nds,
    "gc": smoke_gc,
}


def verify_manifest(installed: Path) -> dict:
    manifest = json.loads((installed / distribution.MANIFEST_NAME).read_text(encoding="utf-8"))
    if (
        manifest.get("schema") != distribution.MANIFEST_SCHEMA
        or manifest.get("version") != distribution.MANIFEST_VERSION
    ):
        raise RuntimeError(f"unexpected manifest identity: {manifest.get('schema')} v{manifest.get('version')}")
    if list(manifest["consoles"]) != sorted(CONSOLES):
        raise RuntimeError(f"release ships {sorted(manifest['consoles'])}, expected {sorted(CONSOLES)}")

    listed = {entry["path"]: entry for entry in manifest["files"]}
    present = {
        path.relative_to(installed).as_posix()
        for path in installed.rglob("*")
        if path.is_file()
    } - {distribution.MANIFEST_NAME}
    if listed.keys() != present:
        raise RuntimeError(
            "manifest and files differ: "
            f"missing {sorted(listed.keys() - present)}, unlisted {sorted(present - listed.keys())}"
        )
    for relative, entry in listed.items():
        path = installed / relative
        if path.stat().st_size != entry["size"] or distribution.sha256(path) != entry["sha256"]:
            raise RuntimeError(f"file does not match the manifest: {relative}")

    for family, entry in manifest["consoles"].items():
        console = CONSOLES[family]
        runtime = installed / entry["runtime"]["path"]
        missing = [name for name in console.required if not (runtime / name).is_file()]
        if missing:
            raise RuntimeError(f"{family} runtime is missing {missing}")
        if distribution.tree_sha256(runtime) != entry["runtime"]["tree_sha256"]:
            raise RuntimeError(f"{family} runtime does not match its tree hash")
        if entry["cli"]["identity"].get("schema") != f"{console.recompiler}.version":
            raise RuntimeError(f"{family} recompiler identity is not {console.recompiler}")
    if not (installed / "LICENSE").is_file():
        raise RuntimeError("release lacks LICENSE")
    return manifest


def package(bin_dir: Path, output: Path, extra_files: list[str]) -> None:
    command: list[object] = [
        sys.executable,
        REPOSITORY / "tools/create_distribution.py",
        "--bin-dir",
        bin_dir,
        "--license",
        REPOSITORY / "LICENSE",
        "--output",
        output,
    ]
    hub = bin_dir / (distribution.HUB_NAME + EXE_SUFFIX)
    if hub.is_file():
        command += ["--hub", hub]
    command += [f"--extra-file={specification}" for specification in extra_files]
    run(command, cwd=REPOSITORY)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--distribution-dir", type=Path, help="an extracted release archive")
    source.add_argument("--bin-dir", type=Path, help="a build's bin folder, packaged first")
    parser.add_argument(
        "--extra-file",
        action="append",
        default=[],
        metavar="SOURCE=DESTINATION",
        help="with --bin-dir: passed to create_distribution.py",
    )
    parser.add_argument("--gba-rom", type=Path, help="jsmolka arm.gba")
    parser.add_argument(
        "--skip",
        action="append",
        default=[],
        choices=sorted(SMOKES),
        help="do not build this console's project (its files are still checked)",
    )
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()

    missing_recipes = sorted(set(CONSOLES) - set(SMOKES))
    if missing_recipes:
        raise RuntimeError(f"no smoke recipe for {missing_recipes}: add one to SMOKES")

    with tempfile.TemporaryDirectory(prefix="native-console-release-smoke-") as tmp:
        root = Path(tmp)
        installed = root / "installed"
        if args.distribution_dir is not None:
            # A copy, so nothing can depend on where the archive was extracted.
            shutil.copytree(args.distribution_dir, installed)
        else:
            package(args.bin_dir.resolve(), installed, args.extra_file)
        manifest = verify_manifest(installed)

        executables = [entry["cli"]["path"] for entry in manifest["consoles"].values()]
        if "hub" in manifest:
            executables.append(manifest["hub"]["path"])
        for name in executables:
            path = installed / name
            path.chmod(path.stat().st_mode | 0o111)

        environment = os.environ.copy()
        environment.update({"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
        environment["PATH"] = str(installed) + os.pathsep + environment.get("PATH", "")
        smoke = Smoke(
            root=root,
            cmake_args=args.cmake_arg,
            env=environment,
            gba_rom=args.gba_rom.resolve() if args.gba_rom else None,
        )
        for family, entry in manifest["consoles"].items():
            if family in args.skip:
                print(f"skip {family}: files checked, no project built")
                continue
            detail = SMOKES[family](smoke, installed / entry["cli"]["path"])
            print(f"ok   {family}: {detail}")

        if "hub" in manifest:
            hub = installed / manifest["hub"]["path"]
            output = run([hub, "--help"], cwd=root, env=environment, timeout=60)
            if "Native Console Hub" not in output:
                raise RuntimeError(f"hub --help printed no banner:\n{output}")
            print("ok   hub: --help")

    print("relocated release generated, built, and ran a project for every console")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
