#!/usr/bin/env python3
"""Recompile the DS test ROMs, build each one, and check it against the interpreter.

    python tools/nds/recomp_suite.py [--filter TEXT]... [--jobs N] [--work DIR] [--build DIR]
                                     [--ndsrecomp EXE] [--nds-run EXE] [--profile] [--profile-frames N]
                                     [--prebuilt-runtime BUILD_DIR] [--build-type TYPE]
                                     [--oracle-suite NAME]... [--no-oracle] [--also-unjudged]
                                     [--with NAME]... [--tests FILE] [--roms DIR]
                                     [--json FILE] [--timeout SECONDS] [--list]

The test ROMs are every entry of tests/nds/blocksds_tests.json (see
run_nds_tests.py, whose verdict reader this uses) and the runs of the wrestler
ROMs in roms/nds/oracle (armwrestler and rockwrestler, written by
tools/nds/oracle_make_wrestlers.py and read as tools/nds/oracle_check.py reads
them). For each ROM: ndsrecomp ROM -o WORK/NAME --name NAME, then CMake and
Ninja on the project it wrote. Then, for each of the ROM's runs:

  1. the verdict, read from the recompiled program and from the interpreter:
     the two must be the same, word for word when it is a failure;
  2. the final state (--state-hash) of the two runs must be the same;
  3. --differential over the run's own script must report no divergence.

The native share of each CPU's instructions (--coverage) is recorded and
printed per run.

--profile uses the recompiler's profile flow instead of the plain one:
ndsrecomp ROM --profile WORK/NAME/profile.trace --profile-frames N -o ...,
which first plays the ROM with the interpreter and compiles what the trace
shows as well (code a program copies elsewhere before running it, for one).
N is --profile-frames, or else the longest of the ROM's runs. It needs an
ndsrecomp that has --profile.

The recompiler is --ndsrecomp, or BUILD/bin/ndsrecomp with --build naming the
root build directory (build). The interpreter is the built executable itself
run with --interpreter, so both sides are one build of the runtime; --nds-run
EXE (or --interpreter, the same option) compares with another build's nds_run
instead, given the ROM's file. Neither comparison is a hardware check: see
run_nds_tests.py and its baseline for that.

A project is built with the runtime snapshot ndsrecomp copies into it, which
is what a user's build does. --prebuilt-runtime BUILD_DIR links
BUILD_DIR/lib/ndsruntime instead (a library file may be named directly): much
less to compile, but it tests that library, so the run refuses one older than
the runtime's headers. Projects are configured as --build-type (Release): a
bare "cmake -G Ninja" of a generated project under MSVC is a Debug build.

--work (build/nds-recomp-suite) holds one folder per ROM, removed and written
afresh on every run so that no stale generated file is compiled. --build is
the root build directory, where bin/ndsrecomp is looked for. --jobs ROMs are
handled at a time. --also-unjudged adds the built ROMs that have no verdict
(not_judged in the test file): 300 frames each, for the state and the
differential comparison. --oracle-suite names the oracle suites to include
(armwrestler and rockwrestler unless given; polyrastertest is the third);
--no-oracle leaves them out. --filter, --with, --tests and --roms are
run_nds_tests.py's.

Fail closed: an empty selection, a ROM that is not there, a recompiler,
configure or build error, a run that does not finish, a missing verdict,
state hash or differential result all fail the ROM's runs, and the exit status
is 0 only if every run met all three conditions.

On Windows the MSVC environment is taken from vcvars64.bat when cl is not
already on PATH (set VCVARS64 to its path if Build Tools are not in the usual
place), and CMake and Ninja are looked for in the Build Tools as well as on
PATH. Standard library only.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from oracle_check import DATA, oracle_dir, rom_file  # noqa: E402
from run_nds_tests import (EXE, REPO, ROMS, TESTS, TestsError, input_options, load_tests, run_entry,  # noqa: E402
                           select_entries)

RESULTS = REPO / "logs" / "nds_recomp_suite.json"
RUNTIME = REPO / "runtimes" / "runtime-nds"
# The oracle suites that are homebrew test ROMs. Named, never globbed: the
# oracle folder also holds data made from a commercial game.
ORACLE_SUITES = ("armwrestler", "rockwrestler", "polyrastertest")
ORACLE_DEFAULT = ("armwrestler", "rockwrestler")
MARKER = ".nds-recomp-suite"
UNJUDGED_FRAMES = 300

VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
VS_TOOLS = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft"


class SetupError(Exception):
    """Something the suite needs is not there."""


# ---------------------------------------------------------------- toolchain

def tool_env() -> dict:
    """Environment with a C compiler, CMake and Ninja reachable (as tools/gba/recomp_suite.py does it)."""
    if os.name != "nt":
        return dict(os.environ)
    env = dict(os.environ)
    if not shutil.which("cl"):
        vcvars = os.environ.get("VCVARS64", VCVARS)
        if not Path(vcvars).is_file():
            raise SetupError("no C compiler: cl is not on PATH and vcvars64.bat is not at\n  " + vcvars +
                             "\nRun this from a Developer Command Prompt, or set VCVARS64 to the path of vcvars64.bat.")
        out = subprocess.run(f'"{vcvars}" >nul && set', shell=True, capture_output=True, text=True).stdout
        env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
        if not any(key.upper() == "INCLUDE" for key in env):
            raise SetupError(f"{vcvars} did not set up a compiler environment")
    path_key = next((key for key in env if key.upper() == "PATH"), "PATH")  # Windows spells it "Path"
    env[path_key] = env.get(path_key, "") + rf";{VS_TOOLS}\CMake\CMake\bin;{VS_TOOLS}\CMake\Ninja"
    return env


def find_tool(name: str, env: dict) -> str:
    path = next((value for key, value in env.items() if key.upper() == "PATH"), None)
    found = shutil.which(name, path=path)
    if not found:
        raise SetupError(f"{name} was not found on PATH" +
                         (" or in the Build Tools' CMake folder" if os.name == "nt" else ""))
    return found


def prebuilt_library(given: Path) -> Path:
    """The runtime library --prebuilt-runtime names, refused if the runtime's headers are newer."""
    if given.is_dir():
        names = ("ndsruntime.lib", "libndsruntime.a")
        library = next((given / "lib" / name for name in names if (given / "lib" / name).is_file()), None)
        if library is None:
            raise SetupError(f"no lib/ndsruntime library in {given}")
    elif given.is_file():
        library = given
    else:
        raise SetupError(f"--prebuilt-runtime {given}: no such directory or file")
    built = library.stat().st_mtime
    newer = [path for path in sorted((RUNTIME / "include").iterdir()) if path.stat().st_mtime > built]
    if newer:
        raise SetupError(f"{library} is older than {newer[0]}" + (f" and {len(newer) - 1} more headers" if len(newer) > 1 else "") +
                         ":\ngenerated code would be compiled against headers the library was not built from. "
                         "Rebuild it, or leave --prebuilt-runtime out.")
    stale = [path for path in sorted((RUNTIME / "src").iterdir()) if path.stat().st_mtime > built]
    if stale:
        print(f"note: {library.name} is older than {stale[0].name}" + (f" and {len(stale) - 1} more sources" if len(stale) > 1 else "") +
              ": this run tests the library as built, not the sources as they are now")
    return library


# ---------------------------------------------------------------- what to run

def oracle_entries(suite: str, data_dir: Path) -> list[dict]:
    """The runs of an oracle suite as entries (see tools/nds/oracle_check.py for the data)."""
    path = oracle_dir(data_dir) / suite / "tests.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError:
        raise SetupError(f"{path} not found; run tools/nds/oracle_make_wrestlers.py (or --no-oracle)") from None
    rom = rom_file(data_dir, data["rom"])
    entries = []
    for run in data["runs"]:
        if not run["checks"]:
            continue  # an information screen, nothing to pass or fail
        entries.append({
            "id": f"{suite}/{run['id']}", "rom": data["rom"], "rom_path": rom, "rom_sha256": data.get("rom_sha256"),
            "frames": run["frames"], "keys": list(run["keys"]),
            "checks": [{"type": "picture", "name": check["name"], "screen": check["screen"], "rect": check["rect"],
                        "sha256": check["sha256"]} for check in run["checks"]],
        })
    return entries


def project_name(rom: Path) -> str:
    """A C identifier from the ROM's file name."""
    name = re.sub(r"[^A-Za-z0-9_]", "_", rom.stem)
    return name if not name[0].isdigit() else "rom_" + name


# ---------------------------------------------------------------- building

def remove_project(folder: Path) -> None:
    if not folder.exists():
        return
    if not (folder / MARKER).is_file():
        raise SetupError(f"{folder} exists and was not made by this script; remove it or choose another --work")
    shutil.rmtree(folder)


def build_rom(rom: Path, name: str, options: argparse.Namespace, tools: dict,
              profile_frames: int) -> tuple[Path | None, str, dict]:
    """Recompile and build one ROM. Returns the executable (or None and why not) and the seconds each step took."""
    folder = options.work / name
    seconds = {}
    try:
        remove_project(folder)
    except (SetupError, OSError) as error:
        return None, f"cannot clear {folder}: {error}", seconds
    folder.mkdir(parents=True)
    (folder / MARKER).write_text("made by tools/nds/recomp_suite.py; removed and rewritten on every run\n")

    def step(label: str, command: list[str]) -> str:
        started = time.monotonic()
        try:
            done = subprocess.run(command, capture_output=True, text=True, errors="replace", env=tools["env"],
                                  timeout=options.timeout)
        except subprocess.TimeoutExpired:
            return f"{label} still going after {options.timeout:.0f} s"
        except OSError as error:
            return f"{label} did not start: {error.strerror or error}"
        finally:
            seconds[label] = round(time.monotonic() - started, 2)
        if done.returncode:
            said = [line.strip() for line in (done.stdout + "\n" + done.stderr).splitlines() if line.strip()]
            if any(line.startswith("usage:") for line in said):
                return f"{label} did not accept its options" + (
                    " (--profile needs an ndsrecomp that has it)" if options.profile and label == "ndsrecomp" else "")
            errors = [line for line in said if "error" in line.lower()] or said[-3:]
            return f"{label} failed: " + " | ".join(errors[:3])
        return ""

    build = folder / "build"
    configure = [tools["cmake"], "-G", "Ninja", "-S", str(folder), "-B", str(build),
                 f"-DCMAKE_BUILD_TYPE={options.build_type}", f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}"]
    if tools["library"]:
        configure += [f"-DNDSRT_PREBUILT_LIB={tools['library'].as_posix()}",
                      f"-DNDSRT_PREBUILT_INCLUDE={(RUNTIME / 'include').as_posix()}"]
    recompile = [str(tools["ndsrecomp"]), str(rom)]
    if options.profile:
        recompile += ["--profile", str(folder / "profile.trace"), "--profile-frames", str(profile_frames)]
    recompile += ["-o", str(folder), "--name", name, "--runtime", str(RUNTIME)]
    for label, command in (
            ("ndsrecomp", recompile),
            ("configure", configure),
            ("build", [tools["ninja"], "-C", str(build), "-j", str(tools["ninja_jobs"])])):
        problem = step(label, command)
        if problem:
            return None, problem, seconds
    executable = build / f"{name}{EXE}"
    if not executable.is_file():
        return None, f"the build wrote no {executable.name}", seconds
    return executable, "", seconds


# ---------------------------------------------------------------- checking

def state_of(record: dict) -> str | None:
    match = re.search(r"^state ([0-9A-F]{16})\s*$", record["output"], re.M)
    return match.group(1) if match else None


def native_share(output: str) -> dict | None:
    match = re.search(r"^native: arm9 ([\d.]+)% \((\d+) of (\d+) instructions interpreted\), "
                      r"arm7 ([\d.]+)% \((\d+) of (\d+)\)", output, re.M)
    if not match:
        return None
    return {"arm9": float(match.group(1)), "arm9_interpreted": int(match.group(2)), "arm9_instructions": int(match.group(3)),
            "arm7": float(match.group(4)), "arm7_interpreted": int(match.group(5)), "arm7_instructions": int(match.group(6))}


def check_entry(executable: Path, entry: dict, document: dict, rom: Path, options: argparse.Namespace) -> dict:
    """The three comparisons for one run of a built ROM."""
    result = {"id": entry["id"], "rom": entry["rom"], "frames": entry["frames"], "ok": False, "notes": []}
    if options.interpreter:
        interpreted = run_entry(options.interpreter, rom, entry, document, extra=["--state-hash"], timeout=options.timeout)
    else:
        interpreted = run_entry(executable, None, entry, document, extra=["--interpreter", "--state-hash"],
                                timeout=options.timeout)
    native = run_entry(executable, None, entry, document, extra=["--coverage", "--state-hash"], timeout=options.timeout)
    for label, record in (("interpreted", interpreted), ("native", native)):
        result[label] = record["status"]
        result[label + "_failure"] = record["failure"]
        result[label + "_seconds"] = record["seconds"]
    judged = bool(entry["checks"])
    result["judged"] = judged
    result["same_verdict"] = ("error" not in (interpreted["status"], native["status"]) and
                              interpreted["status"] == native["status"] and interpreted["failure"] == native["failure"])
    if not result["same_verdict"]:
        for label, record in (("interpreted", interpreted), ("native", native)):
            if record["failure"]:
                result["notes"].append(f"{label}: {record['failure']}")

    states = state_of(interpreted), state_of(native)
    result["state"] = {"interpreted": states[0], "native": states[1]}
    result["same_state"] = bool(states[0]) and states[0] == states[1]
    if not result["same_state"] and "error" not in (interpreted["status"], native["status"]):
        result["notes"].append(f"final state {states[1] or 'missing'} native, {states[0] or 'missing'} interpreted")
    result["native_share"] = native_share(native["output"])
    if result["native_share"] is None and native["status"] != "error":
        result["notes"].append("no --coverage line")

    command = [str(executable), "--differential", "--frames", str(entry["frames"])] + input_options(entry, entry["frames"])
    started = time.monotonic()
    try:
        done = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=options.timeout)
        lines = [line.strip() for line in done.stdout.splitlines() if line.strip()]
        result["differential_clean"] = done.returncode == 0 and bool(lines) and lines[-1] == "PASS"
        if not result["differential_clean"]:
            diverged = next((line for line in lines if line.startswith("DIVERGED")), None)
            said = diverged or (done.stderr.strip().splitlines() or lines or [f"exit status {done.returncode}"])[-1]
            result["notes"].append(f"differential: {said}")
            result["differential_output"] = done.stdout[-4000:]
    except subprocess.TimeoutExpired:
        result["differential_clean"] = False
        result["notes"].append(f"differential still going after {options.timeout:.0f} s")
    except OSError as error:
        result["differential_clean"] = False
        result["notes"].append(f"differential did not start: {error.strerror or error}")
    result["differential_seconds"] = round(time.monotonic() - started, 2)
    result["ok"] = (result["same_verdict"] and result["same_state"] and result["differential_clean"] and
                    result["native_share"] is not None)
    return result


def failed_entry(entry: dict, why: str) -> dict:
    return {"id": entry["id"], "rom": entry["rom"], "frames": entry["frames"], "ok": False, "notes": [why],
            "judged": bool(entry["checks"]), "interpreted": "error", "native": "error", "same_verdict": False,
            "same_state": False, "differential_clean": False, "native_share": None}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def handle_rom(rom: Path, entries: list[dict], document: dict, options: argparse.Namespace, tools: dict) -> dict:
    name = project_name(rom)
    record = {"rom": entries[0]["rom"], "name": name, "seconds": {}, "problem": ""}
    if not rom.is_file():
        record["problem"] = f"ROM not found: {rom}"
    else:
        wanted = entries[0].get("rom_sha256")
        if wanted and sha256_file(rom) != wanted:
            record["problem"] = f"{rom} is not the ROM the oracle's data was made from"
    executable = None
    if not record["problem"]:
        profile_frames = options.profile_frames or max(entry["frames"] for entry in entries)
        executable, record["problem"], record["seconds"] = build_rom(rom, name, options, tools, profile_frames)
    if not executable:
        record["results"] = [failed_entry(entry, record["problem"]) for entry in entries]
        return record
    started = time.monotonic()
    record["results"] = [check_entry(executable, entry, document, rom, options) for entry in entries]
    record["seconds"]["runs"] = round(time.monotonic() - started, 2)
    return record


# ---------------------------------------------------------------- the program

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--filter", action="append", default=[], metavar="TEXT",
                        help="only runs whose id contains TEXT (repeatable)")
    parser.add_argument("--jobs", type=int, default=max(1, min(6, (os.cpu_count() or 2) // 2)), help="ROMs at a time")
    parser.add_argument("--work", type=Path, default=REPO / "build" / "nds-recomp-suite")
    parser.add_argument("--build", type=Path, default=REPO / "build", help="root build directory (bin/ndsrecomp)")
    parser.add_argument("--ndsrecomp", type=Path, help="the recompiler (default: BUILD/bin/ndsrecomp)")
    parser.add_argument("--nds-run", "--interpreter", dest="interpreter", type=Path, metavar="EXE",
                        help="an nds_run to compare with, given the ROM's file "
                             "(default: the built program's own --interpreter)")
    parser.add_argument("--profile", action="store_true",
                        help="recompile with ndsrecomp --profile: play the ROM first and compile what the trace shows")
    parser.add_argument("--profile-frames", type=int, default=0, metavar="N",
                        help="frames to profile (default: the longest of the ROM's runs)")
    parser.add_argument("--prebuilt-runtime", type=Path, metavar="BUILD_DIR",
                        help="link BUILD_DIR/lib/ndsruntime instead of compiling each project's snapshot")
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--oracle-suite", action="append", choices=ORACLE_SUITES, metavar="NAME",
                        help=f"oracle suites to include, of {', '.join(ORACLE_SUITES)} (default: {', '.join(ORACLE_DEFAULT)})")
    parser.add_argument("--no-oracle", action="store_true", help="leave the oracle suites out")
    parser.add_argument("--also-unjudged", action="store_true",
                        help=f"add the ROMs with no verdict: {UNJUDGED_FRAMES} frames, state and differential only")
    parser.add_argument("--with", dest="enabled", action="append", default=[], metavar="NAME")
    parser.add_argument("--tests", type=Path, default=TESTS)
    parser.add_argument("--roms", type=Path, default=ROMS)
    parser.add_argument("--data", type=Path, default=DATA, metavar="DIR",
                        help="the DS test data the oracle suites are in, if not this checkout's roms/nds")
    parser.add_argument("--json", type=Path, default=RESULTS)
    parser.add_argument("--timeout", type=float, default=900.0, help="seconds allowed per step and per run")
    parser.add_argument("--list", action="store_true", help="list the ROMs and their runs; do nothing")
    options = parser.parse_args()
    options.work = options.work.resolve()
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")

    try:
        document = load_tests(options.tests)
        entries, skipped = select_entries(document, options.filter, set(options.enabled))
        for entry in entries:
            entry["rom_path"] = options.roms / entry["rom"]
        if not options.no_oracle:
            for suite in options.oracle_suite or ORACLE_DEFAULT:
                entries += [entry for entry in oracle_entries(suite, options.data)
                            if not options.filter or any(text in entry["id"] for text in options.filter)]
        if options.also_unjudged:
            for item in document.get("not_judged", []):
                ident = Path(item["rom"]).stem + "/no-verdict"
                if not options.filter or any(text in ident for text in options.filter):
                    entries.append({"id": ident, "rom": item["rom"], "rom_path": options.roms / item["rom"],
                                    "frames": UNJUDGED_FRAMES, "checks": []})
    except (TestsError, SetupError) as error:
        print(f"{error} (fail closed)")
        return 1
    if not entries:
        print("no test ROMs selected (fail closed)")
        return 1

    by_rom: dict[Path, list[dict]] = {}
    for entry in entries:
        by_rom.setdefault(entry["rom_path"], []).append(entry)
    names = {}
    for rom in by_rom:
        other = names.setdefault(project_name(rom), rom)
        if other != rom:
            print(f"{rom} and {other} would share the project name {project_name(rom)} (fail closed)")
            return 1
    if options.list:
        for rom, runs in by_rom.items():
            print(f"{project_name(rom)}: {rom}")
            for entry in runs:
                print(f"    {entry['id']}, {entry['frames']} frames")
        return 0

    try:
        env = tool_env()
        tools = {
            "env": env, "cmake": find_tool("cmake", env), "ninja": find_tool("ninja", env),
            "ndsrecomp": options.ndsrecomp or options.build.resolve() / "bin" / f"ndsrecomp{EXE}",
            "library": prebuilt_library(options.prebuilt_runtime.resolve()) if options.prebuilt_runtime else None,
            "ninja_jobs": max(2, (os.cpu_count() or 2) // max(1, min(options.jobs, len(by_rom)))),
        }
        if not Path(tools["ndsrecomp"]).is_file():
            raise SetupError(f"ndsrecomp not found: {tools['ndsrecomp']}")
        if options.interpreter and not options.interpreter.is_file():
            raise SetupError(f"interpreter not found: {options.interpreter}")
        options.work.mkdir(parents=True, exist_ok=True)
    except (SetupError, OSError) as error:
        print(f"{error} (fail closed)")
        return 1

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max(1, options.jobs)) as pool:
        records = list(pool.map(lambda item: handle_rom(item[0], item[1], document, options, tools), by_rom.items()))
    elapsed = time.monotonic() - started

    results = [result for record in records for result in record["results"]]
    for result in results:
        share = result["native_share"]
        verdict = "none" if not result["judged"] else result["interpreted"]
        native = "none" if not result["judged"] else result["native"]
        line = (f"{'ok  ' if result['ok'] else 'FAIL'} {result['id']}  interpreted={verdict} native={native}"
                f"{'' if result['same_verdict'] else ' (DIFFERENT)'}"
                f" state={'same' if result['same_state'] else 'DIFFERENT'}"
                f" differential={'clean' if result['differential_clean'] else 'DIVERGED'}")
        if share:
            line += f"  native arm9 {share['arm9']:.2f}% arm7 {share['arm7']:.2f}%"
        print(line)
        if not result["ok"]:
            for note in result["notes"]:
                print(f"       {note}")

    total = len(results)
    same_verdict = sum(result["same_verdict"] for result in results)
    same_state = sum(result["same_state"] for result in results)
    clean = sum(result["differential_clean"] for result in results)
    built = sum(not record["problem"] for record in records)
    print(f"\n{total} runs of {len(records)} ROMs: verdict as interpreted {same_verdict}/{total}, "
          f"final state as interpreted {same_state}/{total}, differential clean {clean}/{total}")
    step_seconds = {label: sum(record["seconds"].get(label, 0.0) for record in records)
                    for label in ("ndsrecomp", "configure", "build", "runs")}
    runtime = "prebuilt runtime" if tools["library"] else "each with its runtime snapshot"
    if options.profile:
        runtime += ", profiled first"
    print(f"{built}/{len(records)} ROMs recompiled and built ({options.build_type}, {runtime}); "
          f"{elapsed:.0f} s in all with {options.jobs} at a time; summed over ROMs: "
          + ", ".join(f"{label} {value:.0f} s" for label, value in step_seconds.items()))
    shares = [result["native_share"] for result in results if result["native_share"]]
    if shares:
        for cpu in ("arm9", "arm7"):
            values = [share[cpu] for share in shares]
            print(f"native share of {cpu.upper()} instructions: lowest {min(values):.2f}%, median "
                  f"{statistics.median(values):.2f}%, highest {max(values):.2f}%")
    for record in records:
        if record["problem"]:
            print(f"ERROR {record['name']}: {record['problem']}")
    if skipped:
        print(f"{len(skipped)} entries skipped, needing {', '.join(sorted({entry['requires'] for entry in skipped}))} (--with NAME)")

    try:
        options.json.parent.mkdir(parents=True, exist_ok=True)
        options.json.write_text(json.dumps({
            "ndsrecomp": str(tools["ndsrecomp"]), "build_type": options.build_type, "profile": options.profile,
            "runtime": str(tools["library"]) if tools["library"] else "snapshot",
            "interpreter": str(options.interpreter) if options.interpreter else "the built program's --interpreter",
            "seconds": round(elapsed, 1),
            "summary": {"runs": total, "roms": len(records), "built": built, "same_verdict": same_verdict,
                        "same_state": same_state, "differential_clean": clean},
            "roms": [{key: record[key] for key in ("rom", "name", "seconds", "problem")} for record in records],
            "runs": [{key: value for key, value in result.items() if key != "differential_output"} for result in results],
        }, indent=1) + "\n", encoding="utf-8")
    except OSError as error:
        print(f"ERROR cannot write {options.json}: {error.strerror or error}")
        return 1
    return 0 if all(result["ok"] for result in results) and built == len(records) else 1


if __name__ == "__main__":
    sys.exit(main())
