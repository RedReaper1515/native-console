"""Run guest programs through the reference rasteriser and through OpenGL, and hold the two to each other.

The reference rasteriser in runtime-gc/src/gc_gx_raster.c is what the
hardware tests are held to, so it is what "correct" means here. The OpenGL
path is a fast path and is right exactly insofar as it agrees with the
reference. This runs a program both ways and compares two things:

  * the frame, **byte for byte** - both runs decode the same display buffer
    with the same code, so any difference at all is a difference in what
    was drawn, and a tolerance would only hide one;
  * the verdict lines the program itself printed, which for a program built
    on tests/gc/programs/gc_test_report.h (or hwtests' own reporting) is a
    per-subtest pass or fail.

The second matters more than it looks. A program that peeks at the
framebuffer and checks exact texel values is a far sharper instrument than
a picture diff - a stage that is off by one in the middle of a flat region
moves no visible pixel but fails a peek - and every one of our guest
programs is built that way.

The OpenGL run is repeated once per shader mode (GC_GL_SHADERS=uber,
specialised, hybrid), because the three are meant to be interchangeable and
a gate that ran only one would not show it.

A run that crashes, cannot start (a missing SDL2.dll exits before main), or
exits differently from the reference is a disagreement: an earlier version
of this script compared two empty outputs and reported agreement.

    python tools/gc/compare_gl.py build/gc-programs/gc_textest.elf
    python tools/gc/compare_gl.py --gc-run build-gpu/bin/gc_run.exe -j 8
    python tools/gc/compare_gl.py --fast

--fast runs the OpenGL path's fast way of drawing instead (gc_gl.h, "The
fast path"), where the card rasterises and blends. That path is not meant
to agree byte for byte, so it is held to a tolerance, and the tolerance is
part of what a run reports: how many pixels differ at all, how many by more
than eight levels in some channel, and the mean difference over every
channel. A frame passes when the last two are within --fast-over and
--fast-mean. Verdict lines are still compared, because a program that
peeks at exact values says more than a picture does - except for the
programs in FAST_VERDICTS_DIFFER, which measure the one thing the fast path
gives up, and whose verdict counts are printed and not held.

--fast-scale N draws at N times the console's size as well (2 to 4). The
frame the reference is compared with is still the console's size: it is
what the path brings down for a copy, which is what reaches the display.
The larger picture a window shows instead (--dump-picture) is checked too:
it has to be exactly N times the frame each way and, averaged back down,
within --picture-over and --picture-mean of the reference's frame. Those
are looser than the frame's own limits, because the frame has been through
the display buffer's shared chroma and the picture has not.

Passing no program runs every ELF in build/gc-programs and
build/hwtests/gxtest. Exit status is 0 when every run agrees.
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
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DIRECTORIES = [REPO_ROOT / "build" / "gc-programs", REPO_ROOT / "build" / "hwtests" / "gxtest"]
MODES = ("specialised", "uber", "hybrid")

# Programs that sweep far more than a run needs to cover, and how many fields
# to give them - the same prefixes tools/gc/run_gc_tests.py samples, because
# the cases are independent and a prefix is a real check. gc_spin never ends.
FRAMES = {
    "gxtest_intensity": 60,
    "gxtest_copyfilter": 120,
    "gc_spin": 120,
}

# Programs that print their verdict only when the whole sweep is done, which
# is minutes of interpretation a run. Left out unless --slow; the gate runs
# them separately (docs/GC-PLAN.md).
SLOW = {"gxtest_lighting", "gxtest_tev"}

# Programs whose verdicts the fast path is not held to, and why.
FAST_VERDICTS_DIFFER = {
    "gxtest_rasterization": "it probes the sample point to a ten-thousandth of a pixel, and a card places "
                            "vertices on a grid of 1/256",
}

# ... and the ones it is not held to at a scale (--fast-scale above 1).
FAST_SCALED_VERDICTS_DIFFER = {
    "gc_textest": "its mipmap test draws a texture a set number of the console's pixels across and expects the "
                  "level of detail for that size; at a scale the level follows the size actually drawn",
}

# Programs with a frame and no larger picture of it, and why.
NO_PICTURE = {
    "gc_spin": "it writes the display buffer itself; the card never copies a picture into it",
    "gc_timetest": "it draws nothing and copies nothing; its frame is the display buffer as it started",
}

# A verdict line from gc_test_report.h, and the summary lines around it; and
# hwtests' own "Test N passed" / "FAIL" lines.
VERDICT = re.compile(r"^(?:Test \d+ (?:passed|failed).*|\s*FAIL .*|\d+ tests? reported.*)$")
STATS = re.compile(r"^gl: stats (.*)$")


def run(gc_run: Path, program: Path, frame: Path, mode: str | None, frames: int,
        timeout: int, fast: bool = False, scale: int = 1, picture: Path | None = None,
        anisotropy: int = 1) -> dict:
    command = [str(gc_run), str(program), "--frames", str(frames), "--dump-frame", str(frame)]
    env = dict(os.environ)
    # The choice is the flag's alone: a variable left set must not turn an
    # exact run into a fast one.
    env.pop("GC_GL_FAST", None)
    env.pop("GC_GL_SCALE", None)
    env.pop("GC_GL_ANISOTROPY", None)
    env.pop("GC_GL_CARD_FILTER", None)
    if mode:
        command.append("--gl-fast" if fast else "--gl")
        if fast and anisotropy > 1:
            command += ["--gl-anisotropy", str(anisotropy)]
        if fast and scale > 1:
            command += ["--gl-scale", str(scale)]
            if picture is not None:
                command += ["--dump-picture", str(picture)]
        env["GC_GL_SHADERS"] = mode
        # Each run compiles from scratch: the disk cache is measured elsewhere
        # and must not let one mode's programs stand in for another's.
        env["GC_GL_CACHE"] = "off"
    started = time.perf_counter()
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return {"problem": f"timed out after {timeout}s"}
    result = {
        "problem": "",
        "code": done.returncode,
        "verdicts": [l.rstrip() for l in done.stdout.splitlines() if VERDICT.match(l.rstrip())],
        "stats": [m.group(1) for m in (STATS.match(l) for l in done.stderr.splitlines()) if m],
        "seconds": time.perf_counter() - started,
        "stderr": done.stderr,
    }
    if done.returncode < 0 or done.returncode > 255:
        result["problem"] = f"crashed or could not start (exit {done.returncode:#x})"
    return result


def frames_equal(a: Path, b: Path) -> tuple[bool, str]:
    if a.exists() != b.exists():
        return False, "only one of the two runs produced a frame"
    if not a.exists():
        return True, "no frame"
    x, y = a.read_bytes(), b.read_bytes()
    if x == y:
        return True, "identical"
    if len(x) != len(y):
        return False, f"frames of different sizes ({len(x)} and {len(y)} bytes)"
    different = sum(1 for i in range(len(x)) if x[i] != y[i])
    return False, f"{different} bytes differ"


def read_ppm(path: Path) -> tuple[int, int, bytes] | None:
    """A binary PPM's width, height and pixels, or None when it is not one."""
    data = path.read_bytes()
    fields: list[bytes] = []
    at = 0
    while len(fields) < 4 and at < len(data):
        while at < len(data) and data[at:at + 1].isspace():
            at += 1
        if data[at:at + 1] == b"#":
            while at < len(data) and data[at:at + 1] != b"\n":
                at += 1
            continue
        end = at
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    if len(fields) < 4 or fields[0] != b"P6" or fields[3] != b"255":
        return None
    width, height = int(fields[1]), int(fields[2])
    pixels = data[at + 1:]
    if len(pixels) != width * height * 3:
        return None
    return width, height, pixels


def frames_close(a: Path, b: Path, over_limit: float, mean_limit: float) -> tuple[bool, str]:
    """The fast path's tolerance: see the module's description."""
    if a.exists() != b.exists():
        return False, "only one of the two runs produced a frame"
    if not a.exists():
        return True, "no frame"
    x, y = read_ppm(a), read_ppm(b)
    if x is None or y is None:
        return False, "a frame is not a binary PPM"
    if x[:2] != y[:2]:
        return False, f"frames of different sizes ({x[0]}x{x[1]} and {y[0]}x{y[1]})"
    if x[2] == y[2]:
        return True, "identical"
    count = x[0] * x[1]
    p, q = x[2], y[2]
    differ = over = total = 0
    for i in range(count):
        r = abs(p[3 * i] - q[3 * i])
        g = abs(p[3 * i + 1] - q[3 * i + 1])
        b_ = abs(p[3 * i + 2] - q[3 * i + 2])
        if r or g or b_:
            differ += 1
            total += r + g + b_
            if r > 8 or g > 8 or b_ > 8:
                over += 1
    over_share = 100.0 * over / count
    mean = total / (count * 3.0)
    how = (f"{100.0 * differ / count:.2f}% of pixels differ, {over_share:.3f}% by more than 8, "
           f"mean {mean:.3f}")
    return over_share <= over_limit and mean <= mean_limit, how


def picture_close(frame: Path, picture: Path, scale: int, over_limit: float,
                  mean_limit: float) -> tuple[bool, str]:
    """The larger picture against a frame of the console's size: see the module's description."""
    if not frame.exists():
        return (not picture.exists()), "no frame" if not picture.exists() else "a picture and no frame"
    if not picture.exists():
        return False, "no picture was written"
    small, large = read_ppm(frame), read_ppm(picture)
    if small is None or large is None:
        return False, "not a binary PPM"
    width, height, p = small
    if (large[0], large[1]) != (width * scale, height * scale):
        return False, f"the picture is {large[0]}x{large[1]}, not {scale} times {width}x{height}"
    q = large[2]
    row_bytes = width * scale * 3
    area = scale * scale
    over = total = 0
    for y in range(height):
        rows = [q[(y * scale + k) * row_bytes:(y * scale + k + 1) * row_bytes] for k in range(scale)]
        base = y * width * 3
        for x in range(width):
            worst = 0
            for channel in range(3):
                first = x * scale * 3 + channel
                mean = sum(sum(row[first:first + scale * 3:3]) for row in rows) / area
                d = abs(mean - p[base + x * 3 + channel])
                total += d
                if d > worst:
                    worst = d
            if worst > 8:
                over += 1
    count = width * height
    over_share = 100.0 * over / count
    mean = total / (count * 3.0)
    return (over_share <= over_limit and mean <= mean_limit,
            f"picture {large[0]}x{large[1]}, brought down: {over_share:.3f}% by more than 8, mean {mean:.3f}")


def compare_one(gc_run: Path, program: Path, modes: list[str], frames: int, timeout: int,
                keep: Path | None, fast: bool = False, over_limit: float = 0.0,
                mean_limit: float = 0.0, scale: int = 1, picture_over: float = 0.0,
                picture_mean: float = 0.0, anisotropy: int = 1) -> tuple[str, dict[str, bool], list[str]]:
    lines: list[str] = []
    verdict: dict[str, bool] = {}
    with tempfile.TemporaryDirectory() as scratch:
        here = Path(keep) if keep else Path(scratch)
        here.mkdir(parents=True, exist_ok=True)
        reference = here / f"{program.stem}.reference.ppm"
        reference.unlink(missing_ok=True)
        ref = run(gc_run, program, reference, None, frames, timeout)
        if ref["problem"]:
            lines.append(f"compare_gl: {program.name}: the reference run {ref['problem']}")
            return program.name, {m: False for m in modes}, lines
        for mode in modes:
            accelerated = here / f"{program.stem}.{'fast-' if fast else ''}{mode}.ppm"
            accelerated.unlink(missing_ok=True)
            picture = (here / f"{program.stem}.fast-{mode}.picture.ppm"
                       if fast and scale > 1 and program.stem not in NO_PICTURE else None)
            if picture is not None:
                picture.unlink(missing_ok=True)
            gl = run(gc_run, program, accelerated, mode, frames, timeout, fast, scale,
                     picture if reference.exists() else None, anisotropy)
            agreed = True
            if gl["problem"]:
                lines.append(f"compare_gl: {program.name} [{mode}]: the OpenGL run {gl['problem']}")
                verdict[mode] = False
                continue
            if gl["code"] != ref["code"]:
                agreed = False
                lines.append(f"compare_gl: {program.name} [{mode}]: exit {gl['code']}, reference {ref['code']}")
            excused = None
            if fast:
                excused = FAST_VERDICTS_DIFFER.get(program.stem)
                if excused is None and scale > 1:
                    excused = FAST_SCALED_VERDICTS_DIFFER.get(program.stem)
            if ref["verdicts"] != gl["verdicts"] and excused:
                lines.append(f"compare_gl: {program.name} [{mode}]: verdicts differ and are not held "
                             f"({len(ref['verdicts'])} lines from the reference, {len(gl['verdicts'])} from "
                             f"the fast path): {excused}")
            elif ref["verdicts"] != gl["verdicts"]:
                agreed = False
                lines.append(f"compare_gl: {program.name} [{mode}]: the two runs report different verdicts")
                for line in ref["verdicts"]:
                    if line not in gl["verdicts"]:
                        lines.append(f"    only the reference says: {line}")
                for line in gl["verdicts"]:
                    if line not in ref["verdicts"]:
                        lines.append(f"    only OpenGL says:        {line}")
            if fast:
                same, how = frames_close(reference, accelerated, over_limit, mean_limit)
            else:
                same, how = frames_equal(reference, accelerated)
            if not same:
                agreed = False
                lines.append(f"compare_gl: {program.name} [{mode}]: the frames differ"
                             f"{' by more than the tolerance' if fast else ''}: {how}")
            if picture is not None and reference.exists():
                right, what = picture_close(reference, picture, scale, picture_over, picture_mean)
                if not right:
                    agreed = False
                    lines.append(f"compare_gl: {program.name} [{mode}]: the larger picture is wrong: {what}")
                how += "; " + what
            stats = "; ".join(gl["stats"])
            lines.append(f"compare_gl: {program.name} [{mode}]: {'agree' if agreed else 'DISAGREE'}"
                         f" (frame {how}, {len(gl['verdicts'])} verdict lines,"
                         f" reference {ref['seconds']:.1f}s, gl {gl['seconds']:.1f}s) {stats}")
            verdict[mode] = agreed
    return program.name, verdict, lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("program", nargs="*", type=Path)
    parser.add_argument("--gc-run", type=Path, default=REPO_ROOT / "build-gc" / "bin" / "gc_run")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--modes", default=",".join(MODES),
                        help="comma-separated shader modes to run the OpenGL path in")
    parser.add_argument("-j", "--jobs", type=int, default=4)
    parser.add_argument("--keep", type=Path, help="where to leave the frames for inspection")
    parser.add_argument("--slow", action="store_true", help="also run " + ", ".join(sorted(SLOW)))
    parser.add_argument("--fast", action="store_true",
                        help="run the fast path (--gl-fast) and hold it to a tolerance, not to the byte")
    parser.add_argument("--fast-over", type=float, default=0.5,
                        help="fast path: most pixels, in percent, that may differ by more than 8 levels")
    parser.add_argument("--fast-mean", type=float, default=1.0,
                        help="fast path: largest mean difference over every channel, in levels")
    parser.add_argument("--fast-scale", type=int, default=1,
                        help="fast path: draw at this many times the console's size (--gl-scale)")
    parser.add_argument("--fast-anisotropy", type=int, default=1,
                        help="fast path: filter slanted textures with this many samples (--gl-anisotropy)")
    parser.add_argument("--picture-over", type=float, default=2.0,
                        help="at a scale: most pixels of the larger picture, brought down, more than 8 levels out")
    parser.add_argument("--picture-mean", type=float, default=1.5,
                        help="at a scale: largest mean difference of the larger picture, brought down")
    args = parser.parse_args()

    gc_run = args.gc_run
    if not gc_run.exists() and gc_run.with_suffix(".exe").exists():
        gc_run = gc_run.with_suffix(".exe")
    if not gc_run.exists():
        print(f"compare_gl: {gc_run} does not exist; build the gc_run target", file=sys.stderr)
        return 1

    programs = args.program or [p for d in DEFAULT_DIRECTORIES for p in sorted(d.glob("*.elf"))
                                if args.slow or p.stem not in SLOW]
    if not programs:
        print("compare_gl: no programs to run", file=sys.stderr)
        return 1
    # A program that is not there fails to start the same way on both paths,
    # which would read as agreement.
    missing = [p for p in programs if not p.is_file()]
    if missing:
        for p in missing:
            print(f"compare_gl: {p} is not a file", file=sys.stderr)
        return 1
    modes = [m for m in args.modes.split(",") if m]

    started = time.perf_counter()
    counts = {m: 0 for m in modes}
    ok = True
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [pool.submit(compare_one, gc_run, p, modes, FRAMES.get(p.stem, args.frames),
                               args.timeout, args.keep, args.fast, args.fast_over, args.fast_mean,
                               args.fast_scale, args.picture_over, args.picture_mean, args.fast_anisotropy)
                   for p in programs]
        for future in futures:
            _, verdict, lines = future.result()
            for line in lines:
                print(line)
            for mode in modes:
                if verdict.get(mode):
                    counts[mode] += 1
                else:
                    ok = False
    for mode in modes:
        print(f"compare_gl: {'fast, ' if args.fast else ''}{mode}: {counts[mode]} of {len(programs)} programs "
              f"{'are within the tolerance' if args.fast else 'agree'}")
    passed = "every program is within the tolerance" if args.fast else "every program agrees"
    print(f"compare_gl: {passed if ok else 'the two paths disagree'}"
          f" ({time.perf_counter() - started:.1f}s)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
