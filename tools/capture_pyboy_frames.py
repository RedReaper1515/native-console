#!/usr/bin/env python3
"""Replay a recorded input script in PyBoy and dump selected frames."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

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


@dataclass(frozen=True)
class InputEntry:
    anchor: str
    start: int
    buttons: frozenset[str]
    duration: int


def parse_input_script(script: str) -> list[InputEntry]:
    entries: list[InputEntry] = []
    script = script.strip()
    if not script:
        return entries

    for raw_entry in script.split(","):
        raw_entry = raw_entry.strip()
        if not raw_entry:
            continue
        start_text, buttons_text, duration_text = raw_entry.split(":")
        anchor = "frame"
        if start_text[:1].lower() == "c":
            anchor = "cycle"
            start_text = start_text[1:]
        elif start_text[:1].lower() == "f":
            start_text = start_text[1:]
        entries.append(
            InputEntry(
                anchor=anchor,
                start=int(start_text),
                buttons=frozenset(buttons_text),
                duration=int(duration_text),
            )
        )
    return entries


def parse_frame_spec(spec: str) -> list[int]:
    frames: set[int] = set()
    for chunk in spec.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        if "-" in chunk:
            parts = chunk.split("-")
            if len(parts) not in (2, 3):
                raise ValueError(f"Invalid frame range: {chunk}")
            start = int(parts[0])
            end = int(parts[1])
            step = int(parts[2]) if len(parts) == 3 else 1
            frames.update(range(start, end + 1, step))
        else:
            frames.add(int(chunk))
    return sorted(frames)


def active_buttons(entries: Iterable[InputEntry], completed_frames: int) -> set[str]:
    active: set[str] = set()
    frame_start_cycles = completed_frames * DMG_FRAME_CYCLES
    frame_end_cycles = frame_start_cycles + DMG_FRAME_CYCLES
    for entry in entries:
        if entry.anchor == "cycle":
            entry_end = entry.start + entry.duration
            if entry.start < frame_end_cycles and entry_end > frame_start_cycles:
                active.update(entry.buttons)
            continue
        if entry.start <= completed_frames < (entry.start + entry.duration):
            active.update(entry.buttons)
    return active


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Replay a recorded input script in PyBoy and dump specific frames."
    )
    parser.add_argument("rom", help="Path to the ROM file")
    parser.add_argument(
        "--input-file",
        required=True,
        help="Recorded input script file in frame:buttons:duration or c<cycle>:buttons:duration format",
    )
    parser.add_argument(
        "--frames",
        required=True,
        help="Frame list/ranges, for example '420-520' or '420-520-4,1500-1620-4'",
    )
    parser.add_argument("-o", "--output-dir", required=True, help="Directory for dumped PNG frames")
    parser.add_argument("--speed", type=int, default=0, help="PyBoy emulation speed (0 = unlimited)")
    args = parser.parse_args()

    rom_path = Path(args.rom)
    input_path = Path(args.input_file)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    script = input_path.read_text().strip()
    entries = parse_input_script(script)
    requested_frames = parse_frame_spec(args.frames)
    if not requested_frames:
        raise SystemExit("No frames requested")

    max_frame = max(requested_frames)
    requested_set = set(requested_frames)

    pressed: set[str] = set()
    with PyBoy(str(rom_path), window="null") as pyboy:
        pyboy.set_emulation_speed(args.speed)

        for completed_frames in range(max_frame):
            desired = active_buttons(entries, completed_frames)

            for button in sorted(desired - pressed):
                pyboy.button_press(BUTTON_MAP[button])
            for button in sorted(pressed - desired):
                pyboy.button_release(BUTTON_MAP[button])
            pressed = desired

            if not pyboy.tick():
                break

            frame_number = completed_frames + 1
            if frame_number in requested_set:
                output_path = output_dir / f"frame_{frame_number:05d}.png"
                pyboy.screen.image.save(output_path)
                print(f"saved {output_path}")

        for button in sorted(pressed):
            pyboy.button_release(BUTTON_MAP[button])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
