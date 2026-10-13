#!/usr/bin/env python3
"""Run AccuracyCoin headless and print its result table.

AccuracyCoin (100thCoin, MIT) is one NROM image with a menu of accuracy
tests. START on the first page runs all of them and draws a result table;
each test also leaves a result byte in page 4 of work RAM:

    $00          not run
    $01          passed
    n << 2 | 1   passed with behaviour n (a test with several correct answers)
    n << 2 | 2   failed with error code n (the README lists them per test)
    $FF          skipped

The names and result addresses are read from the ROM's own menu tables
(pages of `"name", $FF, result address, test address`), so the table always
describes the image that was run. Tests whose result lives in page 3 only
draw something and are left out, as the ROM's own table does.

    python tools/nes/accuracycoin.py [--runner build/bin/nes_run] [--rom FILE]
                                     [--frames N] [--baseline FILE] [--failures]

Fail closed: the run must reach the ROM's result screen ("running all
tests" flag cleared after having been set, every counted test with a result
byte), and with --baseline every test listed there must pass. The exit
status is 0 only if every test passed, or with --baseline if none of the
listed ones failed.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ROM = REPO / "roms" / "nes" / "accuracycoin" / "AccuracyCoin.nes"
EXE = ".exe" if os.name == "nt" else ""
FRAMES = 6000  # the run takes about 4,200
# START once the menu is up; the cursor starts on the page index.
KEYS = ["--keys", "150:START:6"]
RUNNING_ALL_TESTS = 0x35  # zero page: 1 while the run is in progress
TALLY_TESTS, TALLY_PASSED = 0x37, 0x38  # the ROM's own counts, made while it draws the table


@dataclass(frozen=True)
class Test:
    page: int
    suite: str
    name: str
    address: int


def parse_rom(path: Path) -> list[Test]:
    data = path.read_bytes()
    if data[:4] != b"NES\x1a" or data[4] != 2:
        raise ValueError(f"{path}: not the 32 KiB NROM image AccuracyCoin is")
    prg = data[16:16 + 0x8000]

    def text(offset: int) -> tuple[str, int]:
        end = prg.index(b"\xff", offset)
        return prg[offset:end].decode("ascii"), end + 1

    # The page table is a list of pointers that ends where the first page begins.
    first = prg.find(b"CPU Behavior\xff")
    if first < 0:
        raise ValueError(f"{path}: menu tables not found")
    target = (0x8000 + first).to_bytes(2, "little")
    table = prg.rfind(target, 0, first)
    if table < 0 or (first - table) % 2:
        raise ValueError(f"{path}: page table not found")
    tests: list[Test] = []
    for page, at in enumerate(range(table, first, 2)):
        offset = int.from_bytes(prg[at:at + 2], "little") - 0x8000
        suite, offset = text(offset)
        while prg[offset] != 0xFF:
            name, offset = text(offset)
            address = int.from_bytes(prg[offset:offset + 2], "little")
            offset += 4
            if address >> 8 == 4:
                tests.append(Test(page + 1, suite, " ".join(name.split()), address))
    return tests


def read_ram(state: Path) -> bytes:
    data = state.read_bytes()
    at = data.index(b"RAM ")
    size = int.from_bytes(data[at + 4:at + 8], "little")
    return data[at + 8:at + 8 + size]


def describe(value: int) -> tuple[str, str]:
    """(verdict, detail) of a result byte."""
    if value == 0:
        return "not run", ""
    if value == 0xFF:
        return "skipped", ""
    if value & 1:
        return "pass", f"behaviour {value >> 2}" if value >> 2 else ""
    return "FAIL", f"error {value >> 2:X}"


def run(runner: Path, rom: Path, frames: int, timeout: int, extra: list[str] | None = None) -> tuple[list[tuple[Test, int]], str]:
    """Results per test, and why the run does not count ('' if it does)."""
    tests = parse_rom(rom)
    fd, name = tempfile.mkstemp(prefix="accuracycoin_", suffix=".state")
    os.close(fd)
    state = Path(name)
    state.unlink()
    try:
        proc = subprocess.run([str(runner), str(rom), "--frames", str(frames), *KEYS, "--quiet", "--dump-state", str(state),
                               *(extra or [])], capture_output=True, text=True, timeout=timeout, errors="replace")
        if proc.returncode or not state.exists():
            return [], f"runner exit {proc.returncode}: {proc.stderr.strip()[:200]}"
        ram = read_ram(state)
    finally:
        if state.exists():
            state.unlink()
    results = [(test, ram[test.address]) for test in tests]
    problem = ""
    unrun = [test.name for test, value in results if value == 0]
    if ram[RUNNING_ALL_TESTS]:
        problem = "still running after its frames: " + (unrun[0] if unrun else "?")
    elif unrun:
        problem = f"{len(unrun)} tests have no result (first: {unrun[0]})"
    elif ram[TALLY_TESTS] != len(tests) & 0xFF:
        problem = f"the ROM counted {ram[TALLY_TESTS]} tests, its tables list {len(tests)}"
    return results, problem


def verdict(runner: Path, rom: Path, frames: int, timeout: int) -> tuple[bool, str, str]:
    """For run_nes_tests.py: (everything passed, frames, detail)."""
    try:
        results, problem = run(runner, rom, frames, timeout)
    except (ValueError, OSError) as error:
        return False, "", str(error)
    if problem:
        return False, str(frames), problem
    failed = [test.name for test, value in results if describe(value)[0] != "pass"]
    # The checksum of all result bytes lets recomp_suite.py see that a recompiled run gave the same table.
    table = zlib.crc32(bytes(value for _, value in results))
    detail = f"{len(results) - len(failed)}/{len(results)} tests pass (table {table:08X})"
    return not failed, str(frames), detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runner", type=Path, default=REPO / "build" / "bin" / f"nes_run{EXE}")
    parser.add_argument("--rom", type=Path, default=ROM)
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--baseline", type=Path, help="tests that must pass, one name per line")
    parser.add_argument("--write-baseline", type=Path, help="write the tests that passed to this file")
    parser.add_argument("--failures", action="store_true", help="print only the tests that did not pass")
    args, extra = parser.parse_known_args()

    if not args.rom.exists():
        print(f"ROM not found: {args.rom} (fail closed); run tools/fetch_nes_tests.py")
        return 1
    try:
        results, problem = run(args.runner, args.rom, args.frames, args.timeout, extra)
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAILED: {error}")
        return 1
    if not results:
        print(f"FAILED: {problem}")
        return 1

    page = None
    passed = 0
    for test, value in results:
        word, detail = describe(value)
        passed += word == "pass"
        if args.failures and word == "pass":
            continue
        if test.page != page:
            page = test.page
            print(f"\nPage {page}: {test.suite}")
        print(f"  {word:8s} {test.name:28s} ${test.address:04X}={value:02X}{'  ' + detail if detail else ''}")
    print(f"\n{passed}/{len(results)} tests pass")
    if problem:
        print(f"FAILED: {problem}")
        return 1
    if args.write_baseline:
        header = ("# AccuracyCoin tests that pass in the interpreter (tools/nes/accuracycoin.py),\n"
                  "# as page:name.\n")
        args.write_baseline.write_text(header + "".join(f"{t.page}:{t.name}\n" for t, v in results if v & 1 and v != 0xFF),
                                       newline="\n")
    if args.baseline:
        expected = [line.strip() for line in args.baseline.read_text().splitlines()
                    if line.strip() and not line.startswith("#")]
        if not expected:
            print("baseline is empty (fail closed)")
            return 1
        state = {f"{t.page}:{t.name}": describe(v)[0] == "pass" for t, v in results}
        bad = [name for name in expected if not state.get(name, False)]
        for name in bad:
            print(f"baseline test {'no longer passes' if name in state else 'not found'}: {name}")
        if bad:
            return 1
        print(f"baseline: all {len(expected)} tests pass")
        return 0
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
