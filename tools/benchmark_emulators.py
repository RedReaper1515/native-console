#!/usr/bin/env python3
"""Benchmark a recompiled binary against PyBoy and optional emulator commands."""

from __future__ import annotations

import argparse
import json
import math
import os
import shlex
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import psutil
from capture_pyboy_frames import BUTTON_MAP, active_buttons, parse_input_script
from pyboy import PyBoy


DMG_FPS = 4194304.0 / 70224.0


@dataclass
class TrialResult:
    wall_seconds: float
    peak_rss_bytes: int
    exit_code: int


@dataclass
class RunnerSummary:
    name: str
    kind: str
    runs: int
    mean_seconds: float
    stddev_seconds: float
    mean_fps: float
    mean_realtime: float
    max_peak_rss_bytes: int
    speedup_vs_baseline: float


@dataclass
class RunnerSpec:
    name: str
    kind: str
    argv: list[str]
    env: dict[str, str]


class SafeFormatDict(dict):
    def __missing__(self, key: str) -> str:
        return "{" + key + "}"


def infer_recompiled_project_dir(binary_path: Path) -> Path | None:
    binary_path = binary_path.resolve()
    candidates = [
        binary_path.parent.parent,
        binary_path.parent,
    ]
    for candidate in candidates:
        if (candidate / "CMakeLists.txt").exists():
            return candidate
    return None


def run_logged_command(argv: list[str]) -> None:
    print(f"[build] {' '.join(shlex.quote(part) for part in argv)}", flush=True)
    subprocess.run(argv, check=True)


def prepare_recompiled_binary(args: argparse.Namespace) -> Path:
    requested_binary = Path(args.recompiled_binary).resolve()

    if args.no_recompiled_autobuild:
        if not requested_binary.exists():
            raise SystemExit(f"Recompiled binary not found: {requested_binary}")
        return requested_binary

    project_dir = Path(args.recompiled_project).resolve() if args.recompiled_project else infer_recompiled_project_dir(requested_binary)
    if project_dir is None:
        if not requested_binary.exists():
            raise SystemExit(
                "Could not infer the generated project directory for auto-build. "
                "Pass --recompiled-project or --no-recompiled-autobuild."
            )
        print(f"[build] Using existing recompiled binary without auto-build: {requested_binary}", flush=True)
        return requested_binary

    build_dir = (
        Path(args.recompiled_build_dir).resolve()
        if args.recompiled_build_dir
        else (project_dir / f"build_bench_o{args.recompiled_opt_level}").resolve()
    )
    binary_path = build_dir / requested_binary.name

    cmake_argv = [
        "cmake",
        "-G",
        "Ninja",
        "-S",
        str(project_dir),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DGBRECOMP_GENERATED_OPT_LEVEL={args.recompiled_opt_level}",
        "-DGBRECOMP_ENABLE_IPO=ON",
    ]
    ninja_argv = ["ninja", "-C", str(build_dir)]

    print(
        f"[build] Preparing optimized recompiled benchmark build "
        f"(project={project_dir}, build={build_dir}, generated_opt=-O{args.recompiled_opt_level})"
        ,
        flush=True,
    )
    run_logged_command(cmake_argv)
    run_logged_command(ninja_argv)

    if not binary_path.exists():
        raise SystemExit(f"Expected built benchmark binary was not found: {binary_path}")
    return binary_path


def load_input_script(args: argparse.Namespace) -> str:
    if args.input_file:
        return Path(args.input_file).read_text().strip()
    return (args.input or "").strip()


def total_rss_bytes(proc: psutil.Process) -> int:
    total = 0
    processes = [proc]
    try:
        processes.extend(proc.children(recursive=True))
    except (psutil.Error, ProcessLookupError):
        pass

    for item in processes:
        try:
            total += item.memory_info().rss
        except (psutil.Error, ProcessLookupError):
            continue
    return total


def tail_text(path: Path, max_lines: int = 20) -> str:
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(lines[-max_lines:])


def run_and_measure(argv: list[str], sample_interval: float, env_overrides: dict[str, str] | None = None) -> TrialResult:
    env = os.environ.copy()
    if env_overrides:
        env.update(env_overrides)
    log_file = tempfile.NamedTemporaryFile(prefix="gbrecomp_bench_", suffix=".log", delete=False)
    log_path = Path(log_file.name)
    log_file.close()

    try:
        with log_path.open("wb") as handle:
            start = time.perf_counter()
            process = subprocess.Popen(argv, stdout=handle, stderr=subprocess.STDOUT, env=env)
            ps_process = psutil.Process(process.pid)
            peak_rss = 0

            while process.poll() is None:
                peak_rss = max(peak_rss, total_rss_bytes(ps_process))
                time.sleep(sample_interval)

            peak_rss = max(peak_rss, total_rss_bytes(ps_process))
            return_code = process.wait()
            elapsed = time.perf_counter() - start

        if return_code != 0:
            tail = tail_text(log_path)
            raise RuntimeError(
                f"Command failed with exit code {return_code}: {' '.join(shlex.quote(part) for part in argv)}"
                + (f"\nLast output:\n{tail}" if tail else "")
            )

        return TrialResult(
            wall_seconds=elapsed,
            peak_rss_bytes=peak_rss,
            exit_code=return_code,
        )
    finally:
        try:
            log_path.unlink()
        except OSError:
            pass


def build_runner_specs(args: argparse.Namespace, input_script: str, binary_path: Path) -> list[RunnerSpec]:
    rom_path = Path(args.rom).resolve()
    specs: list[RunnerSpec] = []

    recompiled_argv = [
        str(binary_path),
        "--benchmark",
        "--limit-frames",
        str(args.frames),
    ]
    if input_script:
        recompiled_argv.extend(["--input", input_script])
    if args.recompiled_arg:
        recompiled_argv.extend(args.recompiled_arg)
    specs.append(
        RunnerSpec(
            name="Recompiled",
            kind="recompiled",
            argv=recompiled_argv,
            env={
                "GBRECOMP_BENCHMARK": "1",
                "SDL_VIDEODRIVER": "dummy",
                "SDL_AUDIODRIVER": "dummy",
            },
        )
    )

    if not args.no_pyboy:
        pyboy_argv = [
            sys.executable,
            __file__,
            "__pyboy_worker",
            str(rom_path),
            "--frames",
            str(args.frames),
        ]
        if args.input_file:
            pyboy_argv.extend(["--input-file", str(Path(args.input_file).resolve())])
        elif input_script:
            pyboy_argv.extend(["--input", input_script])
        specs.append(RunnerSpec(name="PyBoy", kind="emulator", argv=pyboy_argv, env={}))

    placeholders = SafeFormatDict(
        rom=str(rom_path),
        frames=str(args.frames),
        input=input_script,
        input_script=input_script,
        input_file=str(Path(args.input_file).resolve()) if args.input_file else "",
        recompiled_binary=str(binary_path),
    )
    for raw in args.emulator_cmd:
        if "=" not in raw:
            raise SystemExit(f"Invalid --emulator-cmd value '{raw}'. Expected NAME=COMMAND.")
        name, template = raw.split("=", 1)
        command = template.format_map(placeholders)
        specs.append(
            RunnerSpec(
                name=name.strip(),
                kind="emulator",
                argv=shlex.split(command),
                env={},
            )
        )

    return specs


def summarize_runner(
    spec: RunnerSpec,
    trials: Iterable[TrialResult],
    frames: int,
    baseline_fps: float,
) -> RunnerSummary:
    trial_list = list(trials)
    wall_values = [trial.wall_seconds for trial in trial_list]
    fps_values = [frames / value for value in wall_values]
    mean_seconds = statistics.mean(wall_values)
    stddev_seconds = statistics.stdev(wall_values) if len(wall_values) > 1 else 0.0
    mean_fps = statistics.mean(fps_values)
    max_peak_rss = max((trial.peak_rss_bytes for trial in trial_list), default=0)
    return RunnerSummary(
        name=spec.name,
        kind=spec.kind,
        runs=len(trial_list),
        mean_seconds=mean_seconds,
        stddev_seconds=stddev_seconds,
        mean_fps=mean_fps,
        mean_realtime=mean_fps / DMG_FPS,
        max_peak_rss_bytes=max_peak_rss,
        speedup_vs_baseline=(mean_fps / baseline_fps) if baseline_fps > 0 else math.nan,
    )


def format_seconds(summary: RunnerSummary) -> str:
    if summary.runs <= 1:
        return f"{summary.mean_seconds:.3f}"
    return f"{summary.mean_seconds:.3f} +/- {summary.stddev_seconds:.3f}"


def print_table(summaries: list[RunnerSummary], baseline_name: str, frames: int) -> None:
    print()
    print(f"Frames: {frames} | Baseline: {baseline_name}")
    print()
    print(
        f"{'Runner':<18} {'Kind':<11} {'Wall(s)':>16} {'Guest FPS':>12} "
        f"{'x Realtime':>12} {'Peak RSS MB':>12} {'Speedup':>10}"
    )
    print("-" * 98)
    for summary in summaries:
        print(
            f"{summary.name:<18} {summary.kind:<11} {format_seconds(summary):>16} "
            f"{summary.mean_fps:>12.1f} {summary.mean_realtime:>12.2f} "
            f"{summary.max_peak_rss_bytes / (1024.0 * 1024.0):>12.1f} "
            f"{summary.speedup_vs_baseline:>10.2f}x"
        )


def run_pyboy_worker(args: argparse.Namespace) -> int:
    input_script = load_input_script(args)
    entries = parse_input_script(input_script)
    pressed: set[str] = set()

    with PyBoy(args.rom, window="null") as pyboy:
        pyboy.set_emulation_speed(0)

        for completed_frames in range(args.frames):
            desired = active_buttons(entries, completed_frames)

            for button in sorted(desired - pressed):
                pyboy.button_press(BUTTON_MAP[button])
            for button in sorted(pressed - desired):
                pyboy.button_release(BUTTON_MAP[button])
            pressed = desired

            if not pyboy.tick():
                return 2

        for button in sorted(pressed):
            pyboy.button_release(BUTTON_MAP[button])

    return 0


def parse_args() -> argparse.Namespace:
    if len(sys.argv) > 1 and sys.argv[1] == "__pyboy_worker":
        parser = argparse.ArgumentParser()
        parser.add_argument("mode")
        parser.add_argument("rom")
        parser.add_argument("--frames", type=int, required=True)
        parser.add_argument("--input")
        parser.add_argument("--input-file")
        return parser.parse_args()

    parser = argparse.ArgumentParser(
        description="Benchmark a recompiled binary against PyBoy and optional emulator commands."
    )
    parser.add_argument("rom", help="Path to the source ROM")
    parser.add_argument("--recompiled-binary", required=True, help="Path to the generated executable to benchmark")
    parser.add_argument(
        "--recompiled-project",
        help="Path to the generated project root. If omitted, the script tries to infer it from --recompiled-binary.",
    )
    parser.add_argument(
        "--recompiled-build-dir",
        help="Build directory to use for the optimized benchmark build. Defaults to <project>/build_bench_o<level>.",
    )
    parser.add_argument(
        "--recompiled-opt-level",
        type=int,
        default=3,
        help="Optimization level for the auto-built recompiled benchmark binary (default: 3).",
    )
    parser.add_argument(
        "--no-recompiled-autobuild",
        action="store_true",
        help="Use the binary at --recompiled-binary directly instead of creating a dedicated optimized benchmark build.",
    )
    parser.add_argument("--frames", type=int, default=600, help="Number of guest frames to run per trial")
    parser.add_argument("--input", help="Inline input script in frame:buttons:duration format")
    parser.add_argument("--input-file", help="Path to an input script file")
    parser.add_argument("--repeat", type=int, default=3, help="Measured trials per runner")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup runs per runner before measurement")
    parser.add_argument("--sample-ms", type=float, default=10.0, help="RSS sampling interval in milliseconds")
    parser.add_argument(
        "--recompiled-arg",
        action="append",
        default=[],
        help="Extra arg appended to the recompiled binary. Repeat as needed.",
    )
    parser.add_argument(
        "--emulator-cmd",
        action="append",
        default=[],
        help="Extra emulator command as NAME=COMMAND. Placeholders: {rom} {frames} {input} {input_file}",
    )
    parser.add_argument("--no-pyboy", action="store_true", help="Skip the built-in PyBoy comparison")
    parser.add_argument("--json-out", help="Optional path to write raw JSON results")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if getattr(args, "mode", None) == "__pyboy_worker":
        return run_pyboy_worker(args)

    if args.frames <= 0:
        raise SystemExit("--frames must be greater than zero")
    if args.repeat <= 0:
        raise SystemExit("--repeat must be greater than zero")
    if args.warmup < 0:
        raise SystemExit("--warmup must be zero or greater")
    if args.recompiled_opt_level < 0:
        raise SystemExit("--recompiled-opt-level must be zero or greater")

    input_script = load_input_script(args)
    recompiled_binary = prepare_recompiled_binary(args)
    specs = build_runner_specs(args, input_script, recompiled_binary)
    if len(specs) < 2:
        raise SystemExit("At least one emulator comparison is required. Remove --no-pyboy or add --emulator-cmd.")

    sample_interval = max(args.sample_ms / 1000.0, 0.001)
    trial_results: dict[str, list[TrialResult]] = {spec.name: [] for spec in specs}

    print(f"Benchmarking {args.frames} frames across {len(specs)} runners...", flush=True)
    for spec in specs:
        print(f"  {spec.name}: {' '.join(shlex.quote(part) for part in spec.argv)}", flush=True)

    for spec in specs:
        for warmup_index in range(args.warmup):
            print(f"[warmup] {spec.name} {warmup_index + 1}/{args.warmup}", flush=True)
            run_and_measure(spec.argv, sample_interval, spec.env)

        for run_index in range(args.repeat):
            print(f"[trial] {spec.name} {run_index + 1}/{args.repeat}", flush=True)
            trial_results[spec.name].append(run_and_measure(spec.argv, sample_interval, spec.env))

    baseline_spec = next((spec for spec in specs if spec.kind == "emulator"), specs[0])
    baseline_trials = trial_results[baseline_spec.name]
    baseline_fps = statistics.mean(args.frames / trial.wall_seconds for trial in baseline_trials)

    summaries = [
        summarize_runner(spec, trial_results[spec.name], args.frames, baseline_fps)
        for spec in specs
    ]
    print_table(summaries, baseline_spec.name, args.frames)

    if args.json_out:
        payload = {
            "frames": args.frames,
            "baseline": baseline_spec.name,
            "summaries": [asdict(item) for item in summaries],
            "trials": {name: [asdict(trial) for trial in items] for name, items in trial_results.items()},
        }
        Path(args.json_out).write_text(json.dumps(payload, indent=2))
        print()
        print(f"Wrote JSON results to {args.json_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
