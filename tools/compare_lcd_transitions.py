#!/usr/bin/env python3
"""Compare LCD-off slowdown spans between a runtime log and a PyBoy replay."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from pyboy import PyBoy

DMG_FRAME_CYCLES = 70224


BUTTON_MAP = {
    "U": "up",
    "D": "down",
    "L": "left",
    "R": "right",
    "A": "a",
    "B": "b",
    "S": "start",
    "T": "select",
}

FRAME_RE = re.compile(
    r"\[FRAME\] #(?P<frame>\d+).*?cycles=(?P<cycles>\d+).*?"
    r"lcd_off_cycles=(?P<lcd_off_cycles>\d+)\s+"
    r"lcd_transitions=(?P<lcd_transitions>\d+)\s+"
    r"lcd_spans=(?P<lcd_spans>\d+)\s+"
    r"last_lcd_off_span=(?P<last_lcd_off_span>\d+)"
)

LCD_ON_RE = re.compile(
    r"\[LCD\] ON cyc=(?P<cycle>\d+)\s+frame_cycles=(?P<frame_cycles>\d+).*?"
    r"span_cycles=(?P<span_cycles>\d+)\s+span_frame_cycles=(?P<span_frame_cycles>\d+)\s+"
    r"frame_lcd_off_cycles=(?P<frame_lcd_off_cycles>\d+)\s+span_index=(?P<span_index>\d+)"
)


@dataclass
class InputEntry:
    anchor: str
    start: int
    buttons: str
    duration: int


@dataclass
class RuntimeFrame:
    frame: int
    cycles: int
    lcd_off_cycles: int
    lcd_transitions: int
    lcd_spans: int
    last_lcd_off_span: int


@dataclass
class RuntimeSpan:
    span_index: int
    cycle: int
    frame_cycles: int
    span_cycles: int
    span_frame_cycles: int
    frame_lcd_off_cycles: int
    frame: int | None = None


@dataclass
class PyBoySpan:
    start_frame: int
    end_frame: int
    start_cycles: int
    end_cycles: int
    observed_frames: int
    observed_cycles: int
    open_span: bool


def parse_input_script(text: str) -> list[InputEntry]:
    entries: list[InputEntry] = []
    for raw_token in text.strip().split(","):
        token = raw_token.strip()
        if not token:
            continue
        parts = token.split(":")
        if len(parts) != 3:
            raise ValueError(f"Invalid input token: {token}")
        start_text = parts[0].strip()
        anchor = "frame"
        if start_text[:1].lower() == "c":
            anchor = "cycle"
            start_text = start_text[1:]
        elif start_text[:1].lower() == "f":
            start_text = start_text[1:]
        buttons = parts[1].strip().upper()
        duration = int(parts[2])
        if duration <= 0:
            continue
        entries.append(InputEntry(anchor, int(start_text), buttons, duration))
    return entries


def read_input_script(args: argparse.Namespace) -> str:
    if args.input_script:
        return args.input_script.strip()
    if args.input_file:
        return Path(args.input_file).read_text(encoding="utf-8").strip()
    return ""


def buttons_for_frame(entries: list[InputEntry], frame: int) -> set[str]:
    active: set[str] = set()
    frame_start_cycles = frame * DMG_FRAME_CYCLES
    frame_end_cycles = frame_start_cycles + DMG_FRAME_CYCLES
    for entry in entries:
        if entry.anchor == "cycle":
            entry_end = entry.start + entry.duration
            if entry.start < frame_end_cycles and entry_end > frame_start_cycles:
                active.update(entry.buttons)
            continue
        if entry.start <= frame < (entry.start + entry.duration):
            active.update(entry.buttons)
    return active


def apply_button_state(pyboy: PyBoy, previous: set[str], current: set[str]) -> None:
    for button in sorted(previous - current):
        pyboy.button_release(BUTTON_MAP[button])
    for button in sorted(current - previous):
        pyboy.button_press(BUTTON_MAP[button])


def collect_pyboy_spans(rom_path: Path, entries: list[InputEntry], end_frame: int) -> list[PyBoySpan]:
    spans: list[PyBoySpan] = []
    previous_buttons: set[str] = set()
    off_start_frame: int | None = None
    off_start_cycles = 0

    with PyBoy(str(rom_path), window="null") as pyboy:
        pyboy.set_emulation_speed(0)

        while pyboy.frame_count < end_frame:
            active = buttons_for_frame(entries, pyboy.frame_count)
            apply_button_state(pyboy, previous_buttons, active)
            previous_buttons = active

            if not pyboy.tick(1, False, False):
                break

            completed_frame = pyboy.frame_count
            cycles = pyboy._cycles()
            lcd_enabled = bool(pyboy.memory[0xFF40] & 0x80)

            if not lcd_enabled:
                if off_start_frame is None:
                    off_start_frame = completed_frame
                    off_start_cycles = cycles
            elif off_start_frame is not None:
                spans.append(
                    PyBoySpan(
                        start_frame=off_start_frame,
                        end_frame=completed_frame,
                        start_cycles=off_start_cycles,
                        end_cycles=cycles,
                        observed_frames=completed_frame - off_start_frame,
                        observed_cycles=cycles - off_start_cycles,
                        open_span=False,
                    )
                )
                off_start_frame = None

        apply_button_state(pyboy, previous_buttons, set())

        if off_start_frame is not None:
            cycles = pyboy._cycles()
            spans.append(
                PyBoySpan(
                    start_frame=off_start_frame,
                    end_frame=pyboy.frame_count,
                    start_cycles=off_start_cycles,
                    end_cycles=cycles,
                    observed_frames=pyboy.frame_count - off_start_frame,
                    observed_cycles=cycles - off_start_cycles,
                    open_span=True,
                )
            )

        pyboy.stop(save=False)

    return spans


def parse_runtime_log(path: Path) -> tuple[list[RuntimeFrame], list[RuntimeSpan]]:
    frames: list[RuntimeFrame] = []
    spans: list[RuntimeSpan] = []
    pending_spans: list[RuntimeSpan] = []

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        frame_match = FRAME_RE.search(line)
        if frame_match:
            frame = RuntimeFrame(
                frame=int(frame_match.group("frame")),
                cycles=int(frame_match.group("cycles")),
                lcd_off_cycles=int(frame_match.group("lcd_off_cycles")),
                lcd_transitions=int(frame_match.group("lcd_transitions")),
                lcd_spans=int(frame_match.group("lcd_spans")),
                last_lcd_off_span=int(frame_match.group("last_lcd_off_span")),
            )
            frames.append(frame)
            if pending_spans:
                for pending in pending_spans:
                    pending.frame = frame.frame
                    spans.append(pending)
                pending_spans.clear()
            continue

        lcd_on_match = LCD_ON_RE.search(line)
        if lcd_on_match:
            pending_spans.append(
                RuntimeSpan(
                    span_index=int(lcd_on_match.group("span_index")),
                    cycle=int(lcd_on_match.group("cycle")),
                    frame_cycles=int(lcd_on_match.group("frame_cycles")),
                    span_cycles=int(lcd_on_match.group("span_cycles")),
                    span_frame_cycles=int(lcd_on_match.group("span_frame_cycles")),
                    frame_lcd_off_cycles=int(lcd_on_match.group("frame_lcd_off_cycles")),
                )
            )

    spans.extend(pending_spans)
    return frames, spans


def filter_runtime_frames(frames: list[RuntimeFrame], start_frame: int, end_frame: int) -> list[RuntimeFrame]:
    return [frame for frame in frames if start_frame <= frame.frame <= end_frame and frame.lcd_off_cycles > 0]


def filter_runtime_spans(spans: list[RuntimeSpan], start_frame: int, end_frame: int) -> list[RuntimeSpan]:
    return [span for span in spans if span.frame is None or start_frame <= span.frame <= end_frame]


def filter_pyboy_spans(spans: list[PyBoySpan], start_frame: int, end_frame: int) -> list[PyBoySpan]:
    filtered: list[PyBoySpan] = []
    for span in spans:
        if span.end_frame < start_frame:
            continue
        if span.start_frame > end_frame:
            continue
        filtered.append(span)
    return filtered


def print_summary(runtime_frames: list[RuntimeFrame], runtime_spans: list[RuntimeSpan], pyboy_spans: list[PyBoySpan]) -> None:
    print("Runtime frames with LCD-off work:")
    if not runtime_frames:
        print("  none")
    else:
        for frame in runtime_frames[:12]:
            print(
                f"  frame #{frame.frame}: cycles={frame.cycles} "
                f"lcd_off_cycles={frame.lcd_off_cycles} lcd_transitions={frame.lcd_transitions} "
                f"lcd_spans={frame.lcd_spans} last_lcd_off_span={frame.last_lcd_off_span}"
            )

    print("\nRuntime exact LCD-off spans:")
    if not runtime_spans:
        print("  none (rerun with --log-lcd-transitions)")
    else:
        for span in runtime_spans[:12]:
            frame_text = "?" if span.frame is None else str(span.frame)
            print(
                f"  span #{span.span_index}: frame={frame_text} span_cycles={span.span_cycles} "
                f"span_frame_cycles={span.span_frame_cycles} cycle={span.cycle}"
            )

    print("\nPyBoy frame-boundary LCD-off spans:")
    if not pyboy_spans:
        print("  none")
    else:
        for idx, span in enumerate(pyboy_spans[:12], start=1):
            suffix = " open" if span.open_span else ""
            print(
                f"  span #{idx}: frames={span.start_frame}->{span.end_frame} "
                f"observed_frames={span.observed_frames} observed_cycles={span.observed_cycles}{suffix}"
            )

    print("\nOrder-aligned comparison:")
    max_count = max(len(runtime_spans), len(pyboy_spans))
    if max_count == 0:
        print("  nothing to compare")
        return
    for index in range(max_count):
        runtime_text = "missing"
        pyboy_text = "missing"
        if index < len(runtime_spans):
            runtime_span = runtime_spans[index]
            runtime_text = (
                f"runtime span#{runtime_span.span_index} frame={runtime_span.frame if runtime_span.frame is not None else '?'} "
                f"cycles={runtime_span.span_cycles}"
            )
        if index < len(pyboy_spans):
            pyboy_span = pyboy_spans[index]
            pyboy_text = (
                f"pyboy frames={pyboy_span.start_frame}->{pyboy_span.end_frame} "
                f"cycles={pyboy_span.observed_cycles}"
            )
        print(f"  {index + 1:>2}: {runtime_text} | {pyboy_text}")


def derive_end_frame(args: argparse.Namespace, runtime_frames: list[RuntimeFrame], entries: list[InputEntry]) -> int:
    if args.end_frame is not None:
        return args.end_frame
    runtime_max = max((frame.frame for frame in runtime_frames), default=0)
    script_max = 0
    for entry in entries:
        if entry.anchor == "cycle":
            entry_end = entry.start + entry.duration
            script_max = max(script_max, (entry_end + DMG_FRAME_CYCLES - 1) // DMG_FRAME_CYCLES)
        else:
            script_max = max(script_max, entry.start + entry.duration)
    derived = max(runtime_max, script_max + 60, 60)
    return derived


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare LCD-off spans in a runtime log against a PyBoy replay of the same input script."
    )
    parser.add_argument("rom", help="Path to the ROM file")
    parser.add_argument("--runtime-log", required=True, help="Runtime log captured with --debug-performance")
    parser.add_argument("--input-file", help="Recorded input file in frame:buttons:duration or c<cycle>:buttons:duration format")
    parser.add_argument("--input-script", help="Inline input script in frame:buttons:duration or c<cycle>:buttons:duration format")
    parser.add_argument("--start-frame", type=int, default=0, help="First frame to include in the comparison")
    parser.add_argument("--end-frame", type=int, help="Last frame to include in the comparison")
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    rom_path = Path(args.rom)
    runtime_log_path = Path(args.runtime_log)
    if not rom_path.exists():
        parser.error(f"ROM not found: {rom_path}")
    if not runtime_log_path.exists():
        parser.error(f"Runtime log not found: {runtime_log_path}")

    input_script = read_input_script(args)
    entries = parse_input_script(input_script) if input_script else []
    runtime_frames, runtime_spans = parse_runtime_log(runtime_log_path)
    if not runtime_frames and not runtime_spans:
        print(
            "Runtime log does not contain LCD slowdown fields. Rerun the game with "
            "--debug-performance or --log-lcd-transitions.",
            file=sys.stderr,
        )
        return 1

    end_frame = derive_end_frame(args, runtime_frames, entries)
    pyboy_spans = collect_pyboy_spans(rom_path, entries, end_frame)

    filtered_runtime_frames = filter_runtime_frames(runtime_frames, args.start_frame, end_frame)
    filtered_runtime_spans = filter_runtime_spans(runtime_spans, args.start_frame, end_frame)
    filtered_pyboy_spans = filter_pyboy_spans(pyboy_spans, args.start_frame, end_frame)

    print(
        f"Comparing frames {args.start_frame}..{end_frame} "
        f"(PyBoy spans are sampled at frame boundaries, runtime spans are exact transition logs).\n"
    )
    print_summary(filtered_runtime_frames, filtered_runtime_spans, filtered_pyboy_spans)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
