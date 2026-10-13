#!/usr/bin/env python3
"""Run the NES test ROMs and read each one's verdict the way its suite reports it.

Every ROM fetched by tools/fetch_nes_tests.py is in the catalogue below with
its verdict method and a frame budget. A ROM is run headless by nes_run (the
interpreter) or by a recompiled executable with the options that make the
runner decide:

  blargg     the $6000 protocol of blargg's 2010 and later ROMs: DE B0 61 at
             $6001, $80 while running, $81 = press reset, 0 = passed
  f8, f0     blargg's 2005 ROMs: a result byte in zero page (1 = passed) and
             the result on screen. The byte holds the number of the subtest
             in progress while the ROM runs and is 0 at power-on, so it only
             counts once the ROM has stopped in its end loop, and the screen
             must agree.
  text       the verdict is printed and nothing else: the ROM must stop and
             the screen must say so
  crc        prints a checksum of what it measured; the source lists the
             checksums a console can give
  nestest    kevtris's nestest from $C000 against its reference trace
             (registers and cycle count of all 8,991 instructions)
  ends       no verdict (the result is heard): the ROM must reach its end
  smoke      no verdict (a game, or a test read off the screen): it must
             still be running after its frames and show a picture
  coin       AccuracyCoin's result table: every one of its tests must pass
             (accuracycoin.py prints the table)

Fail closed everywhere: a missing ROM, a ROM in the data directory that is
not in the catalogue, a timeout, a missing signature, a ROM that does not
stop, or an empty selection is a failure.

    python tools/nes/run_nes_tests.py [--runner build/bin/nes_run] [--filter SUBSTR]
                                      [--baseline FILE] [--jobs N] [--list]

With --baseline, every ROM listed there must pass (a listed ROM that is
missing, times out or fails is an error); the other ROMs are reported only.
Without it the exit status says whether everything selected passed.
--write-baseline FILE writes the ROMs that passed.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DATA = REPO / "roms" / "nes"
EXE = ".exe" if os.name == "nt" else ""
TESTS = "nes-test-roms"

IDLE = ["--until-idle"]
F8 = [*IDLE, "--expect-ram", "F8=1", "--expect-text", "PASSED"]
F0 = [*IDLE, "--expect-ram", "F0=1", "--expect-text", "$01"]
PASSED = [*IDLE, "--expect-text", "Passed", "--fail-text", "Failed"]


@dataclass(frozen=True)
class Rom:
    rel: str  # path under the data directory
    method: str
    frames: int
    args: tuple[str, ...] = ()
    note: str = ""


def suite(folder: str, method: str, frames: int, names: str | list[str], args: list[str] | None = None,
          note: str = "", base: str = TESTS) -> list[Rom]:
    if isinstance(names, str):
        names = names.split()
    return [Rom(f"{base}/{folder}/{name}.nes", method, frames, tuple(args or ()), note) for name in names]


def crc(folder: str, name: str, frames: int, *sums: str) -> list[Rom]:
    args = [*IDLE]
    for value in sums:
        args += ["--expect-text-any", value]
    return suite(folder, "crc", frames, [name], args)


# Frame budgets are about twice what the interpreter needs (minimum 300).
CATALOGUE: list[Rom] = [
    # --- CPU -------------------------------------------------------------
    *suite("other", "nestest", 60, "nestest"),
    *suite("instr_test-v5", "blargg", 4800, "all_instrs"),
    *suite("instr_test-v5", "blargg", 3600, "official_only"),
    *suite("instr_test-v5/rom_singles", "blargg", 900,
           "01-basics 02-implied 03-immediate 04-zero_page 05-zp_xy 06-absolute 07-abs_xy 08-ind_x 09-ind_y "
           "10-branches 11-stack 12-jmp_jsr 13-rts 14-rti 15-brk 16-special"),
    *suite("nes_instr_test/rom_singles", "blargg", 600,
           "01-implied 02-immediate 03-zero_page 04-zp_xy 05-absolute 06-abs_xy 07-ind_x 08-ind_y 09-branches "
           "10-stack 11-special"),
    *suite("blargg_nes_cpu_test5", "text", 2400, "cpu official",
           [*IDLE, "--expect-text", "All tests complete", "--fail-text", "Failed"]),
    *suite("instr_misc", "blargg", 600, "instr_misc"),
    *suite("instr_misc/rom_singles", "blargg", 300, "01-abs_x_wrap 02-branch_wrap 03-dummy_reads 04-dummy_reads_apu"),
    *suite("instr_timing", "blargg", 3000, "instr_timing"),
    *suite("instr_timing/rom_singles", "blargg", 2400, "1-instr_timing 2-branch_timing"),
    *suite("cpu_timing_test6", "text", 1500, "cpu_timing_test", [*IDLE, "--expect-text", "PASSED"]),
    *suite("branch_timing_tests", "f8", 300, "1.Branch_Basics 2.Backward_Branch 3.Forward_Branch", F8),
    *suite("cpu_dummy_reads", "text", 300, "cpu_dummy_reads", PASSED),
    *suite("cpu_dummy_writes", "blargg", 900, "cpu_dummy_writes_oam cpu_dummy_writes_ppumem"),
    *suite("cpu_exec_space", "blargg", 900, "test_cpu_exec_space_apu test_cpu_exec_space_ppuio"),
    *suite("cpu_reset", "blargg", 600, "ram_after_reset registers"),
    *suite("cpu_interrupts_v2", "blargg", 1800, "cpu_interrupts"),
    *suite("cpu_interrupts_v2/rom_singles", "blargg", 900,
           "1-cli_latency 2-nmi_and_brk 3-nmi_and_irq 4-irq_and_dma 5-branch_delays_irq"),
    # --- PPU -------------------------------------------------------------
    *suite("ppu_vbl_nmi", "blargg", 3600, "ppu_vbl_nmi"),
    *suite("ppu_vbl_nmi/rom_singles", "blargg", 900,
           "01-vbl_basics 02-vbl_set_time 03-vbl_clear_time 04-nmi_control 05-nmi_timing 06-suppression "
           "07-nmi_on_timing 08-nmi_off_timing 09-even_odd_frames 10-even_odd_timing"),
    *suite("vbl_nmi_timing", "f8", 600,
           "1.frame_basics 2.vbl_timing 3.even_odd_frames 4.vbl_clear_timing 5.nmi_suppression 6.nmi_disable "
           "7.nmi_timing", F8),
    *suite("blargg_ppu_tests_2005.09.15b", "f0", 300, "palette_ram sprite_ram vbl_clear_time vram_access", F0),
    *suite("blargg_ppu_tests_2005.09.15b", "f0", 300, "power_up_palette", F0,
           note="compares the power-on palette with the one of blargg's own console"),
    *suite("ppu_open_bus", "blargg", 600, "ppu_open_bus"),
    *suite("ppu_read_buffer", "blargg", 3000, "test_ppu_read_buffer"),
    *suite("oam_read", "blargg", 300, "oam_read"),
    *suite("oam_stress", "blargg", 3600, "oam_stress"),
    *suite("sprite_hit_tests_2005.10.05", "f8", 300,
           "01.basics 02.alignment 03.corners 04.flip 05.left_clip 06.right_edge 07.screen_bottom "
           "08.double_height 09.timing_basics 10.timing_order 11.edge_timing", F8),
    *suite("sprite_overflow_tests", "f8", 300, "1.Basics 2.Details 3.Timing 4.Obscure 5.Emulator", F8),
    # --- APU -------------------------------------------------------------
    *suite("apu_test", "blargg", 1200, "apu_test"),
    *suite("apu_test/rom_singles", "blargg", 600,
           "1-len_ctr 2-len_table 3-irq_flag 4-jitter 5-len_timing 6-irq_flag_timing 7-dmc_basics 8-dmc_rates"),
    *suite("apu_reset", "blargg", 600,
           "4015_cleared 4017_timing 4017_written irq_flag_cleared len_ctrs_enabled works_immediately"),
    *suite("blargg_apu_2005.07.30", "f0", 300,
           "01.len_ctr 02.len_table 03.irq_flag 04.clock_jitter 05.len_timing_mode0 06.len_timing_mode1 "
           "07.irq_flag_timing 08.irq_timing 09.reset_timing 10.len_halt_timing 11.len_reload_timing", F0),
    *suite("apu_mixer", "blargg", 3000, "dmc noise square triangle",
           note="finishes; the mixing itself is judged by ear"),
    *suite("dmc_tests", "ends", 300, "buffer_retained latency status status_irq", IDLE,
           note="no verdict and no source: clicks to be heard"),
    # --- DMA, controllers ------------------------------------------------
    *suite("sprdma_and_dmc_dma", "blargg", 600, "sprdma_and_dmc_dma sprdma_and_dmc_dma_512"),
    *crc("dmc_dma_during_read4", "dma_2007_read", 300, "159A7A8F", "5E3DF9C4"),
    *crc("dmc_dma_during_read4", "double_2007_read", 300, "85CFD627", "F018C287", "440EF923", "E52F41A5"),
    *suite("dmc_dma_during_read4", "text", 300, "dma_2007_write dma_4016_read read_write_2007", PASSED),
    *suite("read_joy3", "text", 600, "thorough_test", PASSED),
    *suite("read_joy3", "text", 900, "test_buttons",
           [*PASSED, "--keys", "150:A:10", "--keys", "190:B:10", "--keys", "230:SELECT:10", "--keys", "270:START:10",
            "--keys", "310:UP:10", "--keys", "350:DOWN:10", "--keys", "390:LEFT:10", "--keys", "430:RIGHT:10"]),
    *suite("read_joy3", "text", 600, "count_errors", [*IDLE, "--expect-text", "Conflicts: "],
           note="counts DMC conflicts with the controller reads; any count is a pass"),
    *suite("read_joy3", "text", 600, "count_errors_fast", [*IDLE, "--expect-text", "Errors: "],
           note="counts controller reads a DMC fetch corrupted; any count is a pass"),
    # --- Cartridges ------------------------------------------------------
    *suite("mmc3_test_2/rom_singles", "blargg", 600,
           "1-clocking 2-details 3-A12_clocking 4-scanline_timing 5-MMC3 6-MMC3_alt"),
    *suite("mmc3_irq_tests", "f8", 300,
           "1.Clocking 2.Details 3.A12_clocking 4.Scanline_timing 5.MMC3_rev_A 6.MMC3_rev_B", F8),
    *suite("MMC1_A12", "smoke", 600, "mmc1_a12", ["--smoke"],
           note="interactive: the result is read off the screen"),
    # --- Whole programs --------------------------------------------------
    *suite("nes15-1.0.0", "smoke", 600, "nes15-NTSC nes15-PAL", ["--smoke", "--auto-input"]),
    # --- AccuracyCoin: 146 tests in one ROM; passes only when all of them do
    # (tools/nes/accuracycoin.py prints the table and has its own baseline).
    Rom("accuracycoin/AccuracyCoin.nes", "coin", 6000, ("--keys", "150:START:6")),
]

# Expected CRCs are the ones the sources of dmc_dma_during_read4 list
# (a console gives one of several depending on how its CPU and PPU clocks
# lined up at power-on).


def discover(data: Path, flt: str | None) -> list[Rom]:
    return [rom for rom in CATALOGUE if not flt or flt in rom.rel]


def uncatalogued(data: Path) -> list[str]:
    known = {rom.rel for rom in CATALOGUE}
    found = []
    for folder in (TESTS, "accuracycoin"):
        root = data / folder
        if root.is_dir():
            found += [p.relative_to(data).as_posix() for p in sorted(root.rglob("*.nes"))]
    return [rel for rel in found if rel not in known]


@dataclass
class Result:
    rel: str
    method: str
    ok: bool
    frames: str = ""
    detail: str = ""
    seconds: float = 0.0


def summarize(stdout: str) -> tuple[str, str]:
    """Frames run and the most useful line of a failed run."""
    frames = re.search(r"frames=(\d+)", stdout)
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    fails = [line for line in lines if line.startswith("FAIL")]
    detail = "; ".join(line[6:] for line in fails)
    # blargg's own message for a failed ROM is the line before "Failed".
    block = re.search(r"\[blargg\]\n(.*?)\[/blargg\]", stdout, re.DOTALL)
    if fails and block:
        text = [line.strip() for line in block.group(1).splitlines() if line.strip()]
        text = [line for line in text if line not in ("Failed",) and not re.fullmatch(r"[0-9A-F]{8}", line)]
        if text:
            detail += f" ({text[-1][:70]})"
    return (frames.group(1) if frames else ""), detail


def run_nestest(runner: Path, data: Path, rom: Rom, timeout: int) -> tuple[bool, str, str]:
    path = data / rom.rel
    reference = path.with_suffix(".log")
    if not reference.exists():
        return False, "", "no reference trace"
    fd, log = tempfile.mkstemp(prefix="nestest_", suffix=".log")
    os.close(fd)
    try:
        proc = subprocess.run([str(runner), str(path), "--pc", "C000", "--frames", str(rom.frames), "--cpu-log", log,
                               "--interpreter"], capture_output=True, text=True, timeout=timeout)
        if proc.returncode:
            return False, "", f"runner exit {proc.returncode}"
        cmp = subprocess.run([sys.executable, str(HERE / "nestest_compare.py"), log, str(reference)],
                             capture_output=True, text=True, timeout=timeout)
        frames, _ = summarize(proc.stdout)
        return cmp.returncode == 0, frames, "" if cmp.returncode == 0 else cmp.stdout.strip().splitlines()[0][:100]
    finally:
        os.unlink(log)


def command(runner: Path, data: Path, rom: Rom) -> list[str]:
    """The headless run that reads a ROM's verdict (the same for nes_run and a recompiled executable)."""
    if rom.method == "blargg":
        return [str(runner), str(data / rom.rel), "--frames", str(rom.frames), "--blargg"]
    return [str(runner), str(data / rom.rel), "--frames", str(rom.frames), *rom.args]


def run_one(runner: Path, data: Path, rom: Rom, timeout: int = 600) -> Result:
    started = time.monotonic()
    result = Result(rom.rel, rom.method, False)
    try:
        if not (data / rom.rel).exists():
            result.detail = "ROM not found"
        elif rom.method == "nestest":
            result.ok, result.frames, result.detail = run_nestest(runner, data, rom, timeout)
        elif rom.method == "coin":
            import accuracycoin  # noqa: PLC0415 (only when that ROM is selected)
            result.ok, result.frames, result.detail = accuracycoin.verdict(runner, data / rom.rel, rom.frames, timeout)
        else:
            proc = subprocess.run(command(runner, data, rom), capture_output=True, text=True, timeout=timeout,
                                  errors="replace")
            result.frames, result.detail = summarize(proc.stdout)
            result.ok = proc.returncode == 0 and "PASS" in proc.stdout.splitlines()
            if proc.returncode not in (0, 1):
                result.detail = f"exit {proc.returncode}: {proc.stderr.strip()[:200]}"
            elif not result.ok and not result.detail:
                result.detail = "no verdict"
    except subprocess.TimeoutExpired:
        result.detail = f"timeout after {timeout} s"
    except OSError as error:
        result.detail = str(error)
    result.seconds = time.monotonic() - started
    return result


def read_baseline(path: Path) -> list[str]:
    return [line.strip() for line in path.read_text().splitlines() if line.strip() and not line.startswith("#")]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runner", type=Path, default=REPO / "build" / "bin" / f"nes_run{EXE}")
    parser.add_argument("--data", type=Path, default=DATA, help="directory filled by tools/fetch_nes_tests.py")
    parser.add_argument("--filter", help="only ROMs whose path contains this")
    parser.add_argument("--baseline", type=Path, help="ROMs that must pass, one path per line")
    parser.add_argument("--write-baseline", type=Path, help="write the ROMs that passed to this file")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--timeout", type=int, default=600, help="seconds per ROM")
    parser.add_argument("--list", action="store_true", help="print the catalogue and exit")
    args = parser.parse_args()

    roms = discover(args.data, args.filter)
    if args.list:
        for rom in roms:
            print(f"{rom.method:8s} {rom.frames:5d}  {rom.rel}{'  # ' + rom.note if rom.note else ''}")
        print(f"\n{len(roms)} ROMs")
        return 0 if roms else 1
    if not roms:
        print("no test ROMs selected (fail closed); run tools/fetch_nes_tests.py")
        return 1
    if not args.runner.exists():
        print(f"runner not found: {args.runner} (fail closed)")
        return 1

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        results = list(pool.map(lambda rom: run_one(args.runner, args.data, rom, args.timeout), roms))
    elapsed = time.monotonic() - started

    width = max(len(r.rel) for r in results)
    print(f"{'':4s} {'method':8s} {'frames':>6s}  ROM")
    for r in results:
        print(f"{'pass' if r.ok else 'FAIL'} {r.method:8s} {r.frames:>6s}  {r.rel:{width}s}"
              f"{'  ' + r.detail if r.detail and not r.ok else ''}")
    passed = {r.rel for r in results if r.ok}
    by_method: dict[str, list[int]] = {}
    for r in results:
        counts = by_method.setdefault(r.method, [0, 0])
        counts[0] += r.ok
        counts[1] += 1
    print(f"\n{len(passed)}/{len(results)} passed in {elapsed:.1f} s  (" +
          ", ".join(f"{name} {ok}/{total}" for name, (ok, total) in by_method.items()) + ")")

    failed = False
    stray = uncatalogued(args.data)
    if stray and not args.filter:
        for rel in stray:
            print(f"not in the catalogue: {rel}")
        failed = True
    if args.write_baseline:
        header = ("# NES test ROMs that pass in the interpreter (tools/nes/run_nes_tests.py).\n"
                  "# The CTest nes_test_roms_baseline fails if any of them stops passing.\n")
        args.write_baseline.write_text(header + "".join(f"{r.rel}\n" for r in results if r.ok), newline="\n")
        print(f"wrote {len(passed)} ROMs to {args.write_baseline}")

    if args.baseline:
        expected = read_baseline(args.baseline)
        if args.filter:
            expected = [rel for rel in expected if args.filter in rel]
        if not expected:
            print("baseline is empty (fail closed)")
            return 1
        known = {r.rel for r in results}
        missing = [rel for rel in expected if rel not in known]
        regressed = [rel for rel in expected if rel in known and rel not in passed]
        for rel in missing:
            print(f"baseline ROM is not in the catalogue: {rel}")
        for rel in regressed:
            print(f"baseline ROM no longer passes: {rel}")
        new = [r.rel for r in results if r.ok and r.rel not in expected]
        for rel in new:
            print(f"passes but is not in the baseline: {rel}")
        if missing or regressed or failed:
            return 1
        print(f"baseline: all {len(expected)} ROMs pass")
        return 0
    return 0 if len(passed) == len(results) and not failed else 1


if __name__ == "__main__":
    sys.exit(main())
