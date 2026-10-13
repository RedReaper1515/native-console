#!/usr/bin/env python3
"""Capture a reference frame from Dolphin's software renderer.

P3's gate (docs/GC-PLAN.md) is that the frames gc_run copies out match
Dolphin's software renderer within a tolerance. This runs a GameCube ELF or
DOL under Dolphin - used as a tool, never linked or copied - and writes one
of its frames as a binary PPM, in the same shape `gc_run --dump-frame`
writes, so tools/gc/compare_frame.py can put the two side by side.

    python tools/gc/capture_reference.py --dolphin "D:/Tools/Dolphin/Dolphin-x64/Dolphin.exe" \
        --elf build/gc-programs/gc_drawtest.elf --out logs/gc_drawtest-dolphin.ppm

    build-gc/bin/gc_run.exe build/gc-programs/gc_drawtest.elf --frames 600 \
        --dump-frame logs/gc_drawtest-ours.ppm
    python tools/gc/compare_frame.py logs/gc_drawtest-ours.ppm logs/gc_drawtest-dolphin.ppm

A frame from Dolphin is evidence of what a second implementation draws, not
of what the hardware draws (AGENTS.md, evidence layers). The hardware layer
for the graphics pipeline is dolphin-emu/hwtests' gxtest programs, through
tools/gc/run_gc_tests.py. Where the two disagree, the hardware test wins.

How Dolphin is made to produce frames, since none of it is a command-line
option and the settings only work together:

  - `[Movie] DumpFrames` in Dolphin.ini is the master switch. Without it
    nothing is dumped however the rest is configured - that was measured,
    not assumed.
  - `[Settings] DumpFramesAsImages` in GFX.ini makes the dumper write one
    PNG a frame, `Dump/Frames/framedump_N.png` under the user directory,
    numbered from 1. Without it the frames go to FFmpeg and come out as a
    single video file, which is no use here.
  - `[Settings] FrameDumpsResolutionType = 2` is "Raw Internal Resolution":
    the display buffer at the size the guest set it, 640x480 for the
    ordinary NTSC mode, rather than whatever size the host window happens
    to be. It is the same buffer gc_run decodes.
  - `[Enhancements] DisableCopyFilter = False` in GFX.ini, which is *not*
    Dolphin's default: it ships with the copy filter off because the
    deflicker weights a game asks for make the picture blurry. Off, the
    reference disagrees with us on exactly the rows either side of a
    horizontal edge.
  - `[Core] GFXBackend = Software Renderer`, because the software renderer
    is the reference the plan names. Frame dumping works through the same
    presenter for every backend, so this is a free choice, not a
    constraint.
  - the interpreter CPU core, for the reason capture_vectors.py gives: the
    JIT is free to leave anything it likes in the half of a register an
    instruction does not define, and those halves reach the transform unit
    as vertex coordinates. It costs about three times the wall clock of the
    JIT on a frame-bound program, which is worth paying; --jit is there for
    when it is not.

**The gap worth knowing about.** Dolphin dumps the frames the video
interface presents, so a program that never waits for a vsync presents
nothing and this produces nothing - and several of the test programs are
exactly that shape. tests/gc/programs/gc_drawtest.c, for instance, sets the
video interface up, copies one frame out and returns, all inside a fraction
of one frame's time; gxtest never touches the video interface at all. Those
programs' framebuffers can still be checked - they check themselves, which
is what the verdicts in run_gc_tests.py are - but there is no Dolphin frame
to compare against until the program presents one. When that happens this
says so and exits non-zero rather than leaving a stale or empty file behind.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_USER_DIR = REPO_ROOT / "build" / "gc-dolphin-frames"
DEFAULT_OUT_DIR = REPO_ROOT / "logs"

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
FRAME_NAME = re.compile(r"^framedump_(\d+)\.png$")

# The interpreter is the default; --jit rewrites this one line.
DOLPHIN_INI = """[Core]
CPUCore = {cpu_core}
SkipIPL = True
# The software renderer is the reference docs/GC-PLAN.md names for P3.
GFXBackend = Software Renderer
EmulationSpeed = 0.000000
DSPHLE = True
EnableCheats = False
[Interface]
ConfirmStop = False
UsePanicHandlers = False
OnScreenDisplayMessages = False
[Display]
RenderToMain = True
Fullscreen = False
[DSP]
Backend = No Audio
[Movie]
# The master switch. GFX.ini decides what shape the dump takes; without
# this one there is no dump at all.
DumpFrames = True
# Nothing in a batch run can answer a dialog, so never raise one.
DumpFramesSilent = True
[Analytics]
PermissionAsked = True
Enabled = False
"""

GFX_INI = """[Settings]
# One PNG a frame, rather than one video file through FFmpeg.
DumpFramesAsImages = True
# 2 is "Raw Internal Resolution": the guest's display buffer at the size
# the guest set it, which is the buffer gc_run --dump-frame decodes.
FrameDumpsResolutionType = 2
[Enhancements]
# The vertical copy filter is a real part of the copy, and the reference
# switches it off by default because it blurs the picture and most people
# would rather it did not. We are not most people: libogc hands the copy
# an interlaced mode's deflicker weights {8,8,10,12,10,8,8}, which group
# to sixteen, thirty-two, sixteen and soften every horizontal edge across
# two rows. Leave this at the default and the reference has hard edges we
# correctly do not, and the comparison reports our accuracy as its error.
DisableCopyFilter = False
"""


def fail(message: str) -> int:
    print(f"capture_reference: {message}", file=sys.stderr)
    return 1


def read_png(path: Path) -> tuple[int, int, bytes]:
    """(width, height, RGB raster) from Dolphin's frame dump.

    Dolphin writes 8-bit non-interlaced PNGs, truecolour with or without an
    alpha channel; anything else is a surprise worth stopping on rather
    than a case to guess at.
    """
    data = path.read_bytes()
    if data[:8] != PNG_SIGNATURE:
        raise RuntimeError(f"{path} is not a PNG")

    pos = 8
    header: tuple[int, ...] | None = None
    compressed: list[bytes] = []
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length:
            raise RuntimeError(f"{path}: a {kind.decode('latin1')} chunk is truncated")
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed.append(body)
        elif kind == b"IEND":
            break
    if header is None:
        raise RuntimeError(f"{path}: no IHDR")

    width, height, depth, colour, compression, filter_method, interlace = header
    if depth != 8 or colour not in (2, 6) or compression or filter_method or interlace:
        raise RuntimeError(
            f"{path}: this reads 8-bit non-interlaced truecolour PNGs, and that is "
            f"depth {depth}, colour type {colour}, interlace {interlace}")
    channels = 3 if colour == 2 else 4
    try:
        raw = zlib.decompress(b"".join(compressed))
    except zlib.error as error:
        raise RuntimeError(f"{path}: the image data will not decompress ({error})")

    stride = width * channels
    raster = bytearray(width * height * 3)
    previous = bytearray(stride)
    at = 0
    for y in range(height):
        if at + 1 + stride > len(raw):
            raise RuntimeError(f"{path}: the image data stops at row {y} of {height}")
        method = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        # The five PNG filters, all of which predict a byte from its
        # neighbours; every one of them can appear in a single image, which
        # is why this is per row rather than per file.
        if method == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif method == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif method == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif method == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                up_left = previous[i - channels] if i >= channels else 0
                up = previous[i]
                estimate = left + up - up_left
                da, db, dc = (abs(estimate - left), abs(estimate - up), abs(estimate - up_left))
                predicted = left if da <= db and da <= dc else (up if db <= dc else up_left)
                line[i] = (line[i] + predicted) & 0xFF
        elif method:
            raise RuntimeError(f"{path}: filter type {method} on row {y}")
        previous = line
        out = y * width * 3
        if channels == 3:
            raster[out:out + stride] = line
        else:
            for x in range(width):  # the alpha channel says nothing about a frame
                raster[out + x * 3:out + x * 3 + 3] = line[x * 4:x * 4 + 3]
    return width, height, bytes(raster)


def write_ppm(path: Path, width: int, height: int, raster: bytes) -> None:
    """The same header gc_efb_write_frame() writes, byte for byte."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as out:
        out.write(b"P6\n%d %d\n255\n" % (width, height))
        out.write(raster)


def dumped_frames(directory: Path) -> list[tuple[int, Path]]:
    """Every framedump_N.png, in frame order rather than in name order."""
    frames = []
    for entry in directory.glob("framedump_*.png"):
        match = FRAME_NAME.match(entry.name)
        if match:
            frames.append((int(match.group(1)), entry))
    return sorted(frames)


def run_dolphin(dolphin: Path, program: Path, user_dir: Path, timeout: int,
                jit: bool) -> tuple[Path, bool]:
    """Run the program and return (the frame directory, whether it timed out)."""
    # A portable.txt next to the executable silently overrides --user, which
    # would put the frames somewhere this never looks - and would also mean
    # writing into someone's real Dolphin configuration.
    if (dolphin.parent / "portable.txt").exists():
        raise RuntimeError(
            f"{dolphin.parent / 'portable.txt'} exists; it overrides --user. Remove it, or "
            "point --dolphin at a copy that has none.")

    config = user_dir / "Config"
    config.mkdir(parents=True, exist_ok=True)
    (config / "Dolphin.ini").write_text(DOLPHIN_INI.format(cpu_core=1 if jit else 0),
                                        encoding="ascii")
    (config / "GFX.ini").write_text(GFX_INI, encoding="ascii")

    frames = user_dir / "Dump" / "Frames"
    frames.mkdir(parents=True, exist_ok=True)
    # A frame left over from a previous run would be indistinguishable from
    # one this run produced, and "the last frame" would silently be the
    # wrong program's.
    for _, stale in dumped_frames(frames):
        stale.unlink()

    command = [str(dolphin), "-b", "-u", str(user_dir), "-e", str(program)]
    print(" ".join(command), flush=True)
    try:
        result = subprocess.run(command, timeout=timeout)
    except subprocess.TimeoutExpired:
        # Expected, and not a failure: a program that draws frames usually
        # loops forever, and every PNG already on disk is a whole frame.
        return frames, True
    if result.returncode != 0:
        raise RuntimeError(f"Dolphin exited with {result.returncode}")
    return frames, False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dolphin", type=Path, default=os.environ.get("DOLPHIN"))
    parser.add_argument("--elf", type=Path, required=True,
                        help="the GameCube ELF or DOL to run")
    parser.add_argument("--out", type=Path,
                        help="where to write the frame (default: logs/<name>-dolphin.ppm)")
    parser.add_argument("--frame", type=int,
                        help="which dumped frame to keep, numbered from 1 "
                             "(default: the last, by which the program has settled)")
    parser.add_argument("--user-dir", type=Path, default=DEFAULT_USER_DIR,
                        help="Dolphin user directory to build and use (never a real one)")
    parser.add_argument("--timeout", type=int, default=120,
                        help="seconds to let the program run (default 120); a program that "
                             "loops forever is expected to reach this")
    parser.add_argument("--jit", action="store_true",
                        help="use Dolphin's JIT instead of its interpreter: faster, but the "
                             "JIT's undefined register halves reach the transform unit")
    args = parser.parse_args()

    if not args.dolphin:
        return fail("pass --dolphin PATH or set DOLPHIN; Dolphin is the reference this reads")
    dolphin = Path(args.dolphin)
    if not dolphin.exists():
        return fail(f"{dolphin} does not exist")
    if not args.elf.exists():
        return fail(f"{args.elf} does not exist")

    try:
        frames_dir, timed_out = run_dolphin(dolphin, args.elf, args.user_dir, args.timeout,
                                            args.jit)
    except RuntimeError as error:
        return fail(str(error))

    frames = dumped_frames(frames_dir)
    if not frames:
        return fail(
            f"{args.elf.name} presented no frames, so Dolphin dumped none. Dolphin dumps what "
            "the video interface presents: a program that copies a frame out and returns "
            "without waiting for a vsync - gc_drawtest is one - never presents anything, and "
            "gxtest never touches the video interface at all. There is no reference frame to "
            "capture from a program of that shape.")

    if args.frame is None:
        candidates = list(reversed(frames))
    else:
        candidates = [entry for entry in frames if entry[0] == args.frame]
        if not candidates:
            return fail(f"frame {args.frame} was not dumped; this run produced "
                        f"{frames[0][0]} to {frames[-1][0]}")

    number, source = candidates[0]
    try:
        width, height, raster = read_png(source)
    except (OSError, RuntimeError) as error:
        # Killing Dolphin at the timeout can catch it halfway through the
        # PNG it was writing, so when no particular frame was asked for the
        # one before it will do. Only one step back: a second broken frame
        # is not a race, it is something worth looking at.
        if not (timed_out and args.frame is None and len(candidates) > 1):
            return fail(str(error))
        print(f"capture_reference: frame {number} is incomplete, which is what the timeout "
              f"looks like from here; using frame {candidates[1][0]} instead")
        number, source = candidates[1]
        try:
            width, height, raster = read_png(source)
        except (OSError, RuntimeError) as second:
            return fail(str(second))

    out = args.out or DEFAULT_OUT_DIR / f"{args.elf.stem}-dolphin.ppm"
    try:
        write_ppm(out, width, height, raster)
    except OSError as error:
        return fail(f"{out}: {error.strerror or error}")

    note = f" (stopped at the {args.timeout}s timeout)" if timed_out else ""
    print(f"capture_reference: frame {number} of {len(frames)}{note}, "
          f"{width}x{height} -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
