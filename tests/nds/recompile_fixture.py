#!/usr/bin/env python3
"""Run ndsrecomp on the repository's own DS ROMs and check what it writes.

    python tests/nds/recompile_fixture.py --ndsrecomp build/bin/ndsrecomp.exe --work DIR

Two ROMs from tests/fixtures/make_nds_save_rom.py, each profiled and
recompiled in one command (ROM --profile TRACE --profile-frames 30 -o DIR):

  save      The trace holds what both CPUs ran, with opcodes: the ARM9's idle
            branch and the ARM7's. The project carries everything it needs
            to build on its own: the generated code, a copy of the ROM, the
            runtime snapshot with the frontend and Dear ImGui.
  relocate  The ARM7 program copies itself to work RAM and runs there. The
            recompiler has to recognise the copy from the trace and compile
            it at the address it ran at.

Nothing is compiled here; tests/test_release_bundle.py builds and runs a
project, and tools/nds/recomp_suite.py the test ROMs.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

TESTS = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("make_nds_save_rom", TESTS / "fixtures/make_nds_save_rom.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    if not ok:
        failures.append(what)
        print(f"FAIL {what}")


def run(command: list[object]) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], text=True, errors="replace",
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)


def trace_lines(path: Path) -> dict[tuple[int, int, str], str]:
    """(CPU, address, a|t) -> opcode, for every line of a trace."""
    lines = {}
    for line in path.read_text().splitlines():
        cpu, address, mode, _count, opcode = line.split()
        lines[(int(cpu), int(address, 16), mode)] = opcode
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ndsrecomp", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    shutil.rmtree(args.work, ignore_errors=True)
    args.work.mkdir(parents=True)

    version = run([args.ndsrecomp, "--version-json"])
    check(version.returncode == 0, "--version-json exits 0")
    identity = json.loads(version.stdout) if version.returncode == 0 else {}
    check(identity.get("schema") == "ndsrecomp.version", "--version-json names its schema")
    check({"profile", "recompile", "traces"} <= set(identity.get("features", [])), "--version-json lists its features")

    # The save fixture.
    rom = args.work / "save.nds"
    rom.write_bytes(fixture.build_rom())
    project, trace = args.work / "save", args.work / "profile" / "save.txt"
    result = run([args.ndsrecomp, rom, "--profile", trace, "--profile-frames", 30, "-o", project, "--name", "fx_save"])
    check(result.returncode == 0, f"save: ndsrecomp exits 0\n{result.stdout}")
    if result.returncode == 0:
        check("profiled 30 frames" in result.stdout, "save: the profile is reported")
        lines = trace_lines(trace)
        check(lines.get((9, fixture.IDLE_ADDRESS, "a")) == "EAFFFFFE", "save: the trace has the ARM9's idle branch")
        check(lines.get((7, fixture.ARM7_ADDRESS, "a")) == "EAFFFFFE", "save: the trace has the ARM7's branch")
        check((9, fixture.ARM9_ADDRESS, "a") in lines, "save: the trace has the ARM9's first instruction")
        decls = (project / "generated/rc_decls.h").read_text()
        check(f"rc_main_{fixture.ARM9_ADDRESS:08X}_a(" in decls, "save: the ARM9 program is compiled")
        check(f"rc_arm7_{fixture.ARM7_ADDRESS:08X}_a(" in decls, "save: the ARM7 program is compiled")
        check("_at_" not in decls, "save: nothing is taken for a copy")
        for needed in ("CMakeLists.txt", "main.c", "generated/rc_table.c", "generated/rc_rom.c",
                       "runtime-nds/include/nds.h", "runtime-nds/src/nds.c", "runtime-nds/platform/nds_sdl.cpp",
                       "runtime-nds/cmake/NativeConsoleMacBundle.cmake", "runtime-nds/vendor/imgui/imgui.h"):
            check((project / needed).is_file(), f"save: the project has {needed}")
        check((project / "rom/fx_save.nds").read_bytes() == rom.read_bytes(), "save: the project has the ROM")
        check("nds_sdl_main" in (project / "main.c").read_text(), "save: main.c opens the window")

    # A name that is not a C identifier is refused before anything is written.
    refused = run([args.ndsrecomp, rom, "-o", args.work / "bad", "--name", "not a name"])
    check(refused.returncode != 0 and not (args.work / "bad").exists(), "a bad --name is refused")

    # The ARM7 program that runs from work RAM.
    rom = args.work / "relocate.nds"
    rom.write_bytes(fixture.build_rom(relocate_arm7=True))
    project, trace = args.work / "relocate", args.work / "profile" / "relocate.txt"
    result = run([args.ndsrecomp, rom, "--profile", trace, "--profile-frames", 30, "-o", project, "--name", "fx_reloc"])
    check(result.returncode == 0, f"relocate: ndsrecomp exits 0\n{result.stdout}")
    if result.returncode == 0:
        lines = trace_lines(trace)
        ran = [key for key in lines if key[0] == 7 and key[1] >= fixture.RELOCATED_ADDRESS]
        check(len(ran) == fixture.RELOCATED_INSTRUCTIONS + 1, f"relocate: the trace has the copy ({len(ran)} instructions)")
        copy = f"arm7_at_{fixture.RELOCATED_ADDRESS:08x}"
        check(f"ran in a copy of arm7 at 0x{fixture.RELOCATED_ADDRESS:08x}" in result.stdout,
              "relocate: the copy is reported")
        decls = (project / "generated/rc_decls.h").read_text()
        check(f"rc_{copy}_{fixture.RELOCATED_ADDRESS:08X}_a(" in decls, "relocate: the copy is compiled where it ran")
        # Without the trace there is nothing to find it by.
        plain = run([args.ndsrecomp, rom, "-o", args.work / "relocate-static", "--name", "fx_static"])
        check(plain.returncode == 0 and "_at_" not in (args.work / "relocate-static/generated/rc_decls.h").read_text(),
              "relocate: no copy without a trace")

    print(f"{'FAILED' if failures else 'passed'}: {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
