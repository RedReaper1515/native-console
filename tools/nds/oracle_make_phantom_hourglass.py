#!/usr/bin/env python3
"""Reference runs of Phantom Hourglass (USA) on DeSmuME: pictures, hashes, registers, memory.

    ..\\tools\\nds-venv\\Scripts\\python.exe tools/nds/oracle_oracle_make_phantom_hourglass.py ROM [--run noinput|scripted]... [--verify]

ROM is the user's own dump (SHA-1 4c8f52dd719918bbcd46e73a8bae8628139c1b85). It
is read where it is and never copied. Two runs are made into
roms/nds/oracle/phantom-hourglass/<run>/:

    noinput    3,600 frames from boot with nothing pressed
    scripted   14,400 frames with the touches in touch-script.args, which go
               from the intro through the title screen, the creation of the
               save, file selection, name entry and the mode menu into the
               prologue and the first scene on the ship

Each run leaves:

    every-frame.txt        "frame N top=.. bottom=.." and "video N .." for every frame
    frames/NNNNNN.png      both screens (256x384, top above bottom) every 60 frames
    frames/frames.txt      the "frame" and "video" lines of those
    frames/NNNNNN.ppm      the same picture at each checkpoint, as nds_run --dump-frame writes it
    ram-NNNNNN.bin         main RAM (4 MiB) at each checkpoint
    dtcm-NNNNNN.bin        the DTCM (16 KiB at 0x027E0000)
    itcm-NNNNNN.bin        the ITCM (32 KiB)
    stdout.txt             everything oracle_desmume.py printed, registers included

and phantom-hourglass/timeline.json gathers the script, the checkpoints (what
each shows, both CPUs' registers, the files and their SHA-256) and what the
per-frame log says about when things change. A capture numbered N is the
state after N frames, what nds_run --frames N ends with.

--verify makes every run twice and fails unless the second gives the same
output, pictures and memory as the first.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# The oracle's data: gitignored, written by the oracle_make_*.py scripts.
HERE = ROOT / "roms" / "nds" / "oracle"
ORACLE = ROOT / "tools" / "nds" / "oracle_desmume.py"
OUT = HERE / "phantom-hourglass"
ROM_SHA1 = "4c8f52dd719918bbcd46e73a8bae8628139c1b85"
DTCM, ITCM = "0x027E0000:0x4000", "0x01FF8000:0x8000"

# The stylus script: frame, x, y on the bottom screen, frames held, what it is for.
TOUCHES = [
    (400, 128, 96, 6, "anywhere during the intro: skips to the title screen"),
    (700, 128, 96, 6, 'title screen ("Touch the screen!"): with a blank save the game now creates its file'),
    (1300, 128, 96, 6, 'dismisses "A file has been created.": File Selection appears'),
    (1500, 128, 66, 6, 'file 1 ("New Game!"): name entry, the firmware nickname filled in'),
    (1700, 192, 176, 6, 'OK on the keyboard: "Is this name OK?"'),
    (1900, 205, 112, 6, 'Yes: "Do you hold the stylus in your right or left hand?"'),
    (2100, 205, 112, 6, 'Right: "You use your right hand?"'),
    (2300, 205, 112, 6, "Yes: back to File Selection, file 1 now exists"),
    (2500, 128, 66, 6, "file 1: Copy / Erase / Start"),
    (2700, 207, 169, 6, 'Start: "Choose a mode to play."'),
    (2900, 128, 80, 6, "Adventure: the prologue starts (it plays by itself for about 10,000 frames)"),
]

RUNS = {
    "noinput": {
        "frames": 3600,
        "touches": [],
        "checkpoints": {
            0: "boot: the state DeSmuME sets up, before the first instruction",
            60: "Nintendo logo (top) and the ESRB notice (bottom); 2D only",
            300: "intro: the sea, 3D on both screens",
            900: "intro: gulls over the sea",
            1800: "intro: the ship",
            3000: 'title screen, "Touch the screen!" fading in and out',
            3600: "title screen",
        },
    },
    "scripted": {
        "frames": 14400,
        "touches": TOUCHES,
        "checkpoints": {
            60: "Nintendo logo and the ESRB notice",
            300: "intro: the sea",
            600: "title screen, reached by the touch at 400",
            900: '"Creating a file... Do not turn the power off."',
            1290: '"A file has been created."',
            1450: "File Selection: two New Game! entries",
            1650: "Enter a name: keyboard, the firmware nickname in the field",
            1800: '"Is this name OK?" Yes / No',
            2050: '"Do you hold the stylus in your right or left hand?" Right / Left',
            2250: '"You use your right hand?" Yes / No',
            2450: "File Selection: file 1 with the name and three hearts",
            2650: "file 1 chosen: Copy, Erase, Start",
            2850: '"Choose a mode to play." Adventure / Battle',
            3600: "prologue, first picture (the pirates' ship); the top screen is between two lines of text",
            7200: 'prologue: "Her lineage traced back to an ancient, ruined kingdom."',
            14400: 'first scene on the ship, 3D: Niko, "So what did you think of my amazing paper cutouts?"',
        },
    },
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run_oracle(rom: Path, options: list[str], stdout: Path) -> str:
    done = subprocess.run([sys.executable, str(ORACLE), str(rom), *options], capture_output=True, text=True)
    stdout.write_text(done.stdout, encoding="ascii", newline="\n")
    stdout.with_name("stderr.txt").write_text(done.stderr, encoding="utf-8", newline="\n")
    if done.returncode:
        raise SystemExit(f"oracle_desmume.py failed ({done.returncode}): {done.stderr.strip().splitlines()[-1:]}")
    return done.stdout


def fields(line: str) -> dict[str, str]:
    return dict(item.split("=", 1) for item in line.split() if "=" in item)


def make_run(rom: Path, name: str, out: Path, reuse: bool = False) -> dict:
    spec = RUNS[name]
    frames, checkpoints = spec["frames"], spec["checkpoints"]
    inputs = [f"{frame}:{x},{y}:{held}" for frame, x, y, held, _ in spec["touches"]]
    touch = [arg for item in inputs for arg in ("--touch", item)]
    if reuse:
        every, text = (out / "every-frame.txt").read_text(), (out / "stdout.txt").read_text()
    else:
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        # Every frame's hashes and video registers.
        every = run_oracle(rom, ["--frames", str(frames), *touch, "--every", "1", "--hash-frames", "--video-regs",
                                 "--first-nonblank", "--quiet"], out / "every-frame.txt")
        # Pictures every 60 frames, and everything at the checkpoints.
        at = [arg for frame in checkpoints for arg in ("--at", str(frame))]
        text = run_oracle(rom, ["--frames", str(frames), *touch, "--every", "60", *at,
                                "--dump-frames", str(out / "frames"), "--stacked", "--png", "--hash-frames",
                                "--video-regs", "--regs", "--dump-ram", str(out / "ram-{frame:06d}.bin"),
                                "--dump-mem", f"{DTCM}:{out / 'dtcm-{frame:06d}.bin'}",
                                "--dump-mem", f"{ITCM}:{out / 'itcm-{frame:06d}.bin'}", "--quiet"],
                          out / "stdout.txt")
        for picture in (out / "frames").glob("*.ppm"):
            if int(picture.stem) not in checkpoints:
                picture.unlink()

    captures: dict[int, dict] = {}
    for line in every.splitlines():
        kind, number = line.split()[:2]
        if kind in ("frame", "video"):
            captures.setdefault(int(number), {}).update(fields(line))
    first = fields(next(line for line in every.splitlines() if line.startswith("first_nonblank")))

    registers: dict[int, dict] = {}
    for line in text.splitlines():
        parts = line.split()
        if parts[0] == "regs":
            registers.setdefault(int(parts[1]), {})[parts[2]] = fields(line)
    points = []
    for frame, shows in checkpoints.items():
        files = {"picture": f"frames/{frame:06d}.ppm", "picture_png": f"frames/{frame:06d}.png",
                 "ram": f"ram-{frame:06d}.bin", "dtcm": f"dtcm-{frame:06d}.bin", "itcm": f"itcm-{frame:06d}.bin"}
        missing = [path for path in files.values() if not (out / path).is_file()]
        if missing or frame not in registers:
            raise SystemExit(f"{name}: checkpoint {frame} is incomplete ({missing})")
        state = captures[frame] if frame else {}
        points.append({"frame": frame, "shows": shows,
                       "top_sha256": state.get("top"), "bottom_sha256": state.get("bottom"),
                       "video": {key: value for key, value in state.items() if key not in ("top", "bottom")},
                       "arm9": registers[frame]["arm9"], "arm7": registers[frame]["arm7"],
                       "files": files, "sha256": {key: sha256(out / path) for key, path in files.items()
                                                  if key != "picture_png"}})

    # What the per-frame log says about when things change.
    def first_frame(test) -> int | None:
        return next((frame for frame in sorted(captures) if test(captures[frame])), None)

    # Display mode changes. While 3D is shown on both screens the game flips POWCNT1's display-swap bit
    # and exchanges VRAMCNT_C and VRAMCNT_D every frame (one screen shows the 3D picture, the other a
    # capture of the previous one), so those two are reported as ranges instead.
    changes, swapping = [], []
    previous = swap = None
    for frame in sorted(captures):
        video = captures[frame]
        key = (video["dispcnt_a"], video["dispcnt_b"], video["disp3dcnt"])
        if key != previous:
            changes.append({"frame": frame, "dispcnt_a": key[0], "dispcnt_b": key[1], "disp3dcnt": key[2],
                            "vramcnt": video["vramcnt"], "powcnt1": video["powcnt1"]})
            previous = key
        bit = int(video["powcnt1"], 16) & 0x8000
        if swap is not None and bit != swap:
            if swapping and swapping[-1][1] == frame - 1:
                swapping[-1][1] = frame
            else:
                swapping.append([frame, frame])
        swap = bit
    observed = {
        "first_frame_not_one_colour": {screen: None if value == "none" else int(value)
                                       for screen, value in first.items()},
        "first_frame_with_polygons": first_frame(lambda v: int(v["polygons"]) > 0),
        "first_frame_with_3d_on_bg0": first_frame(lambda v: int(v["dispcnt_a"], 16) & 0x108 == 0x108),
        "display_mode_changes": changes,
        "display_swap_flips_every_frame": [span for span in swapping if span[1] - span[0] >= 8],
    }
    return {"frames": frames, "touches": [{"frame": f, "x": x, "y": y, "held": held, "does": does,
                                           "option": f"--touch {f}:{x},{y}:{held}"}
                                          for f, x, y, held, does in spec["touches"]],
            "observed": observed, "checkpoints": points,
            "pictures_every_60_frames": "frames/NNNNNN.png", "every_frame": "every-frame.txt"}


def comparable(out: Path) -> dict[str, str]:
    """Everything a second run must reproduce."""
    return {str(path.relative_to(out)): sha256(path) for path in sorted(out.rglob("*"))
            if path.is_file() and path.name != "stderr.txt"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rom", type=Path)
    parser.add_argument("--run", action="append", choices=sorted(RUNS))
    parser.add_argument("--verify", action="store_true", help="make each run twice and compare")
    parser.add_argument("--reuse", action="store_true",
                        help="do not run the emulator: rebuild timeline.json from the runs already there")
    args = parser.parse_args()
    if not args.rom.is_file():
        print(f"{args.rom}: no such ROM", file=sys.stderr)
        return 2
    digest = hashlib.sha1(args.rom.read_bytes()).hexdigest()
    if digest != ROM_SHA1:
        print(f"{args.rom}: SHA-1 {digest}, expected {ROM_SHA1} (Phantom Hourglass, USA)", file=sys.stderr)
        return 2

    OUT.mkdir(parents=True, exist_ok=True)
    script = ["# Phantom Hourglass (USA): from boot to the first scene on the ship.",
              "# oracle_desmume.py ROM --frames 14400 @touch-script.args ...; nds_run takes the same --touch options."]
    script += [f"--touch {f}:{x},{y}:{held}".ljust(26) + f"# {does}" for f, x, y, held, does in TOUCHES]
    (OUT / "touch-script.args").write_text("\n".join(script) + "\n", encoding="ascii", newline="\n")

    timeline_path = OUT / "timeline.json"
    timeline = json.loads(timeline_path.read_text()) if timeline_path.is_file() else {}
    timeline.update({
        "rom": {"name": args.rom.name, "sha1": ROM_SHA1},
        "oracle": "DeSmuME 0.9.12 through py-desmume 0.0.9, tools/nds/oracle_desmume.py; "
                  "clock from 2026-01-01 12:00:00, blank save, DeSmuME's firmware settings (nickname DeSmuME)",
        "conventions": {
            "frame": "a capture numbered N is the state after N frames: nds_run --frames N; inputs are held "
                     "while frames F to F+held-1 (counted from 0) run",
            "sha256": "of a screen's 256x192 pixels, three bytes a pixel: the 5-bit red, green and blue values",
            "picture": "256x384, the top screen above the bottom one",
            "registers": "r15 is DeSmuME's fetch address; pc is the instruction that runs next",
        },
    })
    timeline.setdefault("runs", {})
    status = 0
    for name in args.run or sorted(RUNS):
        out = OUT / name
        second = timeline["runs"].get(name, {}).get("second_run") if args.reuse else None
        timeline["runs"][name] = make_run(args.rom, name, out, args.reuse)
        if second:
            timeline["runs"][name]["second_run"] = second
        print(f"{name}: {RUNS[name]['frames']} frames, {len(RUNS[name]['checkpoints'])} checkpoints")
        if args.verify and not args.reuse:
            first = comparable(out)
            again = HERE / "work" / f"verify-{name}"
            make_run(args.rom, name, again)
            second = comparable(again)
            differing = sorted(key for key in first.keys() | second.keys() if first.get(key) != second.get(key))
            shutil.rmtree(again)
            timeline["runs"][name]["second_run"] = (f"identical: {len(first)} files" if not differing
                                                    else f"DIFFERENT: {differing[:10]}")
            print(f"{name}: second run {timeline['runs'][name]['second_run']}")
            status |= bool(differing)
    timeline_path.write_text(json.dumps(timeline, indent=1) + "\n", encoding="ascii", newline="\n")
    return status


if __name__ == "__main__":
    sys.exit(main())
