#!/usr/bin/env python3
"""A game's own save and load, end to end, headless.

The card test (tests/gc/gc_card_test.c) holds the card to the retail save
library's rules; this holds a real game to them. It runs a generated game
twice against one fresh memory card, each run from boot in its own process,
with --card:

  1. SAVE_SCRIPT drives the game's menus to save. The run must leave a card
     that mounts (header, directory and allocation-table checksums, free
     counts) with at least one file on it - optionally one whose game code is
     --expect-code.
  2. LOAD_SCRIPT drives a fresh boot to load that save. The card must come
     out of it byte for byte as it went in (a load writes nothing), and the
     picture at --expect-field must differ from the picture the same script
     gives against a blank card - which is the difference between the game
     finding the save and not finding it. Both frames are kept so they can be
     looked at.

Nothing in it knows about any game; the scripts and the field do. Scripts for
Shrek 2 (USA) are in tools/gc/scripts/. Save dirs and cards go in --work,
never beside the game, so no real card is touched.

  check_card_saves.py --game build/shrek/G3RE52.exe --disc "Shrek 2 (USA).ciso"
      --save-script tools/gc/scripts/shrek2_save.txt --save-frames 7400
      --load-script tools/gc/scripts/shrek2_load.txt --load-frames 3600
      --expect-field 3480 --expect-code G3RE --work WORKDIR
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import sys
import time
from pathlib import Path

SECTOR = 0x2000


def checksum(data: bytes) -> tuple[int, int]:
    a = b = 0
    for i in range(0, len(data) - 1, 2):
        word = (data[i] << 8) | data[i + 1]
        a = (a + word) & 0xFFFF
        b = (b + (word ^ 0xFFFF)) & 0xFFFF
    return (0 if a == 0xFFFF else a), (0 if b == 0xFFFF else b)


def card_problems(image: bytes) -> tuple[list[str], list[str]]:
    """What is wrong with a card image, and the game codes of its files."""
    problems: list[str] = []
    if len(image) not in (0x80000 << k for k in range(6)):
        return [f"{len(image)} bytes is not a card size"], []
    blocks = len(image) // SECTOR
    header = image[:SECTOR]
    if checksum(header[:0x1FC]) != struct.unpack(">HH", header[0x1FC:0x200]):
        problems.append("header checksum")
    directories = []
    for n in (1, 2):
        d = image[n * SECTOR:(n + 1) * SECTOR]
        if checksum(d[:0x1FFC]) != struct.unpack(">HH", d[0x1FFC:0x2000]):
            problems.append(f"directory copy {n} checksum")
        directories.append((struct.unpack(">h", d[0x1FFA:0x1FFC])[0], d))
    for n in (3, 4):
        f = image[n * SECTOR:(n + 1) * SECTOR]
        if checksum(f[4:]) != struct.unpack(">HH", f[:4]):
            problems.append(f"allocation table copy {n - 2} checksum")
        free = struct.unpack(">H", f[6:8])[0]
        counted = sum(1 for i in range(5, blocks) if f[2 * i:2 * i + 2] == b"\0\0")
        if free != counted:
            problems.append(f"allocation table copy {n - 2} says {free} free, counts {counted}")
    newest = max(directories, key=lambda x: x[0])[1]
    codes = [newest[e * 64:e * 64 + 6].decode("latin-1") for e in range(127) if newest[e * 64] != 0xFF]
    return problems, codes


def run(game: Path, disc: Path, card: Path, script: Path, frames: int, frame_path: Path, field: int) -> float:
    started = time.monotonic()
    command = [str(game), "--disc", str(disc), "--card", str(card), "--input", str(script),
               "--frames", str(frames)]
    if field:
        command += ["--dump-frame", str(frame_path), "--dump-frame-every", str(field)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=3600)
    if result.returncode != 0:
        raise SystemExit(f"check_card_saves: {' '.join(command)} exited {result.returncode}\n{result.stderr[-2000:]}")
    return time.monotonic() - started


def frame_difference(a: Path, b: Path) -> float:
    """Mean absolute difference of two PPM frames, 0-255."""
    def pixels(path: Path) -> bytes:
        data = path.read_bytes()
        at, tokens = 0, 0
        while tokens < 4:  # magic, width, height, maximum; then exactly one whitespace byte
            while data[at:at + 1].isspace():
                at += 1
            while not data[at:at + 1].isspace():
                at += 1
            tokens += 1
        return data[at + 1:]
    x, y = pixels(a), pixels(b)
    if len(x) != len(y):
        return 255.0
    step = 7  # a sample is plenty to tell two screens apart
    total = sum(abs(x[i] - y[i]) for i in range(0, len(x), step))
    return total / (len(x) / step)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--save-script", type=Path, required=True)
    parser.add_argument("--save-frames", type=int, required=True)
    parser.add_argument("--load-script", type=Path, required=True)
    parser.add_argument("--load-frames", type=int, required=True)
    parser.add_argument("--expect-field", type=int, required=True)
    parser.add_argument("--expect-code", default="")
    parser.add_argument("--min-difference", type=float, default=4.0)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()

    if args.load_frames <= args.expect_field:
        parser.error("--load-frames has to run past --expect-field")
    args.work.mkdir(parents=True, exist_ok=True)
    card = args.work / "saved.card"
    blank = args.work / "blank.card"
    for path in (card, blank, card.with_name(card.name + ".tmp")):
        path.unlink(missing_ok=True)

    seconds = run(args.game, args.disc, card, args.save_script, args.save_frames, args.work / "save.ppm", 0)
    image = card.read_bytes()
    problems, codes = card_problems(image)
    print(f"check_card_saves: save run {seconds:.0f} s; card {len(image)} bytes, files {codes or 'none'}")
    if problems:
        print("check_card_saves: FAIL the saved card does not mount: " + ", ".join(problems))
        return 1
    if not codes:
        print("check_card_saves: FAIL the save run left no file on the card")
        return 1
    if args.expect_code and not any(code.startswith(args.expect_code) for code in codes):
        print(f"check_card_saves: FAIL no file with game code {args.expect_code}")
        return 1

    before = hashlib.sha256(image).hexdigest()
    seconds = run(args.game, args.disc, card, args.load_script, args.load_frames, args.work / "load.ppm",
                  args.expect_field)
    print(f"check_card_saves: load run {seconds:.0f} s")
    if hashlib.sha256(card.read_bytes()).hexdigest() != before:
        print("check_card_saves: FAIL the load run changed the card")
        return 1
    seconds = run(args.game, args.disc, blank, args.load_script, args.load_frames, args.work / "blank.ppm",
                  args.expect_field)
    print(f"check_card_saves: control run against a blank card {seconds:.0f} s")

    loaded = args.work / f"load-{args.expect_field:06d}.ppm"
    control = args.work / f"blank-{args.expect_field:06d}.ppm"
    if not loaded.exists() or not control.exists():
        print(f"check_card_saves: FAIL no frame at field {args.expect_field}")
        return 1
    difference = frame_difference(loaded, control)
    print(f"check_card_saves: field {args.expect_field} with the save vs without: mean difference "
          f"{difference:.1f} ({loaded.name}, {control.name})")
    if difference < args.min_difference:
        print("check_card_saves: FAIL the load run looks the same with the save as without it")
        return 1
    print("check_card_saves: PASS the game saved to the card and loaded it back after a restart")
    return 0


if __name__ == "__main__":
    sys.exit(main())
