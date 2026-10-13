"""Every GameCube check, as fast as the machine allows, with the time each took.

    python tools/gc/run_checks.py --quick
    python tools/gc/run_checks.py --full --disc "Shrek 2 (USA).ciso" --frames-ref DIR

The stages, and what each one catches:

  build          the whole tree (keep-going, so an unrelated target cannot
                 stop the GameCube ones) and then the GameCube targets, which
                 must build
  ctest          the GameCube and hub CTest suites, including the 27-program
                 hardware baseline; --full adds the release packaging test
  recompiled     run_recompiled_tests.py: every test program recompiled and
                 checked against the interpreter (--full adds the slow one)
  game-build     the commercial disc through gcrecomp and a compile, when
                 --disc is given
  frames         one headless run of the game dumping a frame every 100
                 fields, compared with a reference (--frames-ref) at fields
                 600, 1200, 2000, 2500, 3000 and 3700
  audio          the sound the frames run played, through audio_check.py:
                 no clipping runs or clicks, tonal music at a sane level where
                 the game plays music, digital silence where it plays none
  fast-frames    the same run three more times on the OpenGL path's fast way
                 of drawing (--gl-fast), at one, two and three times the
                 console's size, each with an empty program cache; its frames
                 at those fields are held to the frames run's own, the
                 software renderer's, within a tolerance for each size, since
                 that path is close and not exact
  differential   --full only: the recompiled game against the interpreter at
                 every field end, with scripted input through New Game into
                 gameplay, plus its native share and the sound task's command
                 lists (they must keep arriving after New Game)
  skip           --full only: the windowed frontend (dummy video driver,
                 software) skipping the storybook and the opening cutscene
                 (scripts/shrek2_skip.txt) with the graphics thread on and
                 off; every present must show the single-threaded run's
                 picture for the field its buffer is from, at most a field
                 late, and both machines must end in the same state

ctest, recompiled and game-build are independent and run at the same time;
frames and differential start as soon as the game is built, and audio and
fast-frames follow frames. The wall time is the longest chain rather than
the sum.

fast-frames needs a graphics card with OpenGL 4.1. Without one it fails, and
does not fall back to comparing the software renderer with itself.

Nothing here uses a hosted CI service: everything runs on this machine.

On Windows it needs the MSVC environment. If `cl` is not on PATH the build
steps are run through vcvars64.bat, found with vswhere or at --vcvars.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_gl import frames_close  # noqa: E402  (the fast path's measure of two frames)

FRAME_FIELDS = (600, 1200, 2000, 2500, 3000, 3700)
# What a frame from the fast path is held to against the software renderer's
# frame of the same field, by the scale it was drawn at: the most pixels, in
# percent, more than 8 levels out in some channel, and the largest mean
# difference over every channel, in levels.
#
# At the console's size the two differ by a level or two over most of a
# blended picture (that path neither dithers nor rounds to six bits) and by
# more at a few pixels of an edge. At twice the size the frame is the mean
# of four, so every edge and letter is smoothed and several percent of a menu
# is out. At three times it is the middle sample of each block of nine, which
# lands where the 1x sample does - but a picture that reaches the screen
# through a copy kept as a texture (this game's menu) is drawn from a copy
# three times as fine, and its edges and letters move as they do at 2x.
# Shrek 2's worst over these fields: 0.17 % and 1.39 at 1x, 4.23 % and 2.05
# at 2x, 4.40 % and 1.98 at 3x (docs/GC-PLAN.md, "The fast path" and "A copy
# as large as the picture").
FAST_FRAMES_LIMITS = {1: (0.5, 2.0), 2: (6.0, 3.0), 3: (6.0, 3.0)}
# What Shrek 2 plays over the frames run (no controller, 61.7 s): the title
# music, the logos, the load before the menu (digital silence) and the menu's
# music. audio_check.py says what each kind is held to; docs/GC-PLAN.md
# ("Clean sound") has the measurements behind it, and a build that decodes
# its samples from main memory fails eleven of its checks.
AUDIO_SECTIONS = ("music:2:15", "music:23:30", "silence:40.5:44", "music:46:61")
EXE = ".exe" if sys.platform == "win32" else ""

print_lock = threading.Lock()


def say(message: str) -> None:
    with print_lock:
        print(message, flush=True)


class Stage:
    def __init__(self, name: str):
        self.name = name
        self.ok = False
        self.skipped = ""
        self.seconds = 0.0
        self.lines: list[str] = []

    def note(self, line: str) -> None:
        self.lines.append(line)


# ------------------------------------------------------------------ environment


def find_vcvars(explicit: str | None) -> str | None:
    if sys.platform != "win32" or shutil.which("cl"):
        return None
    if explicit:
        return explicit
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                               capture_output=True, text=True).stdout.strip()
        if found:
            candidate = Path(found) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.exists():
                return str(candidate)
    for edition in ("18", "2022"):
        for kind in ("BuildTools", "Community", "Professional", "Enterprise"):
            candidate = Path(r"C:\Program Files (x86)\Microsoft Visual Studio") / edition / kind / \
                "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.exists():
                return str(candidate)
            candidate = Path(r"C:\Program Files\Microsoft Visual Studio") / edition / kind / \
                "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.exists():
                return str(candidate)
    return None


def compiler_env(vcvars: str | None) -> dict[str, str]:
    """The environment vcvars64.bat produces, captured once."""
    env = dict(os.environ)
    if vcvars:
        dump = subprocess.run(f'call "{vcvars}" >nul && set', shell=True, capture_output=True, text=True)
        for line in dump.stdout.splitlines():
            if "=" in line:
                key, _, value = line.partition("=")
                env[key] = value
        # Build Tools ships its own CMake and Ninja, but vcvars64 does not
        # always put them on PATH.
        ide = Path(vcvars).parents[3] / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake"
        for tool, folder in (("cmake", ide / "CMake" / "bin"), ("ninja", ide / "Ninja")):
            if not shutil.which(tool, path=env.get("PATH")) and folder.exists():
                env["PATH"] = f"{folder}{os.pathsep}{env.get('PATH', '')}"
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    return env


def run(command: list[str], log: Path, env: dict[str, str], cwd: Path | None = None,
        timeout: int = 7200) -> int:
    # Windows looks a bare program name up on this process's PATH, not the
    # one being handed to the child, so resolve it against the child's.
    found = shutil.which(command[0], path=env.get("PATH"))
    if found:
        command = [found] + command[1:]
    with log.open("w", encoding="utf-8", errors="replace") as out:
        try:
            return subprocess.run(command, stdout=out, stderr=subprocess.STDOUT, env=env,
                                  cwd=cwd, timeout=timeout).returncode
        except OSError as error:
            out.write(f"\nrun_checks: could not start {command[0]}: {error}\n")
            return -1
        except subprocess.TimeoutExpired:
            out.write(f"\nrun_checks: timed out after {timeout} s\n")
            return -1


def tail(log: Path, lines: int = 12) -> list[str]:
    try:
        return log.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:]
    except OSError:
        return []


# ------------------------------------------------------------------ stages


def stage_build(args, env, scratch: Path) -> Stage:
    s = Stage("build")
    configure = ["cmake", "-G", "Ninja", "-S", str(REPO_ROOT), "-B", str(args.build),
                 "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTS=ON", "-DGBRECOMP_ENABLE_NATIVE_PATCHES=ON",
                 "-DBUILD_HUB=ON"]
    if args.sdl2_dir:
        configure.append(f"-DSDL2_DIR={args.sdl2_dir}")
    if run(configure, scratch / "build-configure.log", env) != 0:
        s.note("cmake would not configure; see build-configure.log")
        s.lines += tail(scratch / "build-configure.log")
        return s
    run(["ninja", "-C", str(args.build), "-k", "0"], scratch / "build-all.log", env)
    failed = [line for line in tail(scratch / "build-all.log", 100000) if line.startswith("FAILED:")]
    for line in failed[:10]:
        s.note(f"(not a GameCube target, reported only) {line}")
    targets = ["gcrecomp", "gc_run", "gc_play", "gc_cpu_test", "gc_gx_test", "gc_ax_test"]
    if run(["ninja", "-C", str(args.build)] + targets, scratch / "build-gc.log", env) != 0:
        s.note("the GameCube targets do not build; see build-gc.log")
        s.lines += tail(scratch / "build-gc.log")
        return s
    s.ok = True
    return s


def stage_ctest(args, env, scratch: Path) -> Stage:
    s = Stage("ctest")
    pattern = "^gc_|hub|release" if args.full else "^gc_|hub"
    code = run(["ctest", "--test-dir", str(args.build), "-R", pattern, "--output-on-failure",
                "-j", str(max(2, (os.cpu_count() or 8) // 4))], scratch / "ctest.log", env)
    for line in tail(scratch / "ctest.log", 100000):
        if "tests passed" in line or "***Failed" in line or "Not Run" in line or "Timeout" in line:
            s.note(line.strip())
    s.ok = code == 0
    return s


def stage_recompiled(args, env, scratch: Path) -> Stage:
    s = Stage("recompiled")
    command = [sys.executable, str(REPO_ROOT / "tools" / "gc" / "run_recompiled_tests.py"),
               "--gcrecomp", str(args.build / "bin" / f"gcrecomp{EXE}"), "--work", str(scratch / "recompiled")]
    if args.full:
        command.append("--slow")
    code = run(command, scratch / "recompiled.log", env)
    for line in tail(scratch / "recompiled.log", 100000):
        if "DISAGREE" in line or line.startswith("run_recompiled_tests:"):
            s.note(line.strip())
    s.ok = code == 0
    return s


def game_exe(scratch: Path) -> Path:
    return scratch / "game" / "build" / f"G3RE52{EXE}"


def stage_game_build(args, env, scratch: Path) -> Stage:
    s = Stage("game-build")
    project = scratch / "game"
    if project.exists():
        shutil.rmtree(project, ignore_errors=True)
    if run([str(args.build / "bin" / f"gcrecomp{EXE}"), str(args.disc), "-o", str(project), "--name", "G3RE52"],
           scratch / "game-generate.log", env) != 0:
        s.lines += tail(scratch / "game-generate.log")
        return s
    configure = ["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
                 "-DCMAKE_BUILD_TYPE=Release"]
    if args.sdl2_dir:
        configure.append(f"-DSDL2_DIR={args.sdl2_dir}")
    if run(configure, scratch / "game-configure.log", env) != 0:
        s.lines += tail(scratch / "game-configure.log")
        return s
    started = time.monotonic()
    if run(["ninja", "-C", str(project / "build")], scratch / "game-compile.log", env) != 0:
        s.lines += tail(scratch / "game-compile.log")
        return s
    s.note(f"compiled in {time.monotonic() - started:.0f} s")
    s.ok = game_exe(scratch).exists()
    return s


def reference_frame(ref: Path, field: int) -> Path | None:
    for name in (f"f-{field:06d}.ppm", f"f{field}.ppm"):
        if (ref / name).exists():
            return ref / name
    return None


def stage_frames(args, env, scratch: Path) -> Stage:
    s = Stage("frames")
    out = scratch / "frames"
    if out.exists():
        shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    last = max(FRAME_FIELDS)
    # The same run records the sound for the audio stage: it costs nothing
    # extra, and the frames are the same with or without it.
    code = run([str(game_exe(scratch)), "--disc", str(args.disc), "--frames", str(last),
                "--dump-frame", str(out / "f.ppm"), "--dump-frame-every", "100",
                "--dump-audio", str(out / "audio32k.raw")],
               scratch / "frames.log", env)
    if code != 0:
        s.note(f"the run exited {code}")
        s.lines += tail(scratch / "frames.log")
        return s
    ok = True
    for field in FRAME_FIELDS:
        got = out / f"f-{field:06d}.ppm"
        if not got.exists():
            s.note(f"field {field}: no frame written")
            ok = False
            continue
        digest = hashlib.sha1(got.read_bytes()).hexdigest()[:12]
        if args.save_frames_ref:
            args.save_frames_ref.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(got, args.save_frames_ref / got.name)
        if not args.frames_ref:
            s.note(f"field {field}: {digest} (no reference given)")
            continue
        ref = reference_frame(args.frames_ref, field)
        if ref is None:
            s.note(f"field {field}: {digest}, no reference frame")
            ok = False
        elif ref.read_bytes() == got.read_bytes():
            s.note(f"field {field}: identical")
        else:
            s.note(f"field {field}: DIFFERS from {ref}")
            ok = False
    s.ok = ok
    return s


def stage_audio(args, env, scratch: Path) -> Stage:
    s = Stage("audio")
    capture = scratch / "frames" / "audio32k.raw"
    if not capture.exists():
        s.note("the frames run wrote no sound")
        return s
    try:
        import numpy  # noqa: F401  (audio_check.py needs it)
    except ImportError:
        s.skipped = "numpy is not installed"
        s.note("skipped: audio_check.py needs numpy")
        s.ok = True
        return s
    command = [sys.executable, str(REPO_ROOT / "tools" / "gc" / "audio_check.py"), str(capture)]
    for section in AUDIO_SECTIONS:
        command += ["--section", section]
    code = run(command, scratch / "audio.log", env)
    for line in tail(scratch / "audio.log", 100000):
        if "FAILED" in line or "every check passed" in line or "clicks" in line:
            s.note(line.replace("audio_check: ", "").strip())
    s.ok = code == 0
    return s


def stage_fast_frames(args, env, scratch: Path) -> Stage:
    s = Stage("fast-frames")
    reference = scratch / "frames"
    last = max(FRAME_FIELDS)
    ok = True
    for scale, (over, mean) in FAST_FRAMES_LIMITS.items():
        label = f"fast-frames-{scale}x"
        out = scratch / label
        # An empty program cache each time, so every program is compiled in
        # the run: a cache left by an earlier build could hide one that no
        # longer compiles.
        for stale in (out, scratch / f"{label}-cache"):
            if stale.exists():
                shutil.rmtree(stale, ignore_errors=True)
        out.mkdir(parents=True)
        (scratch / f"{label}-cache").mkdir(parents=True)
        command = [str(game_exe(scratch)), "--disc", str(args.disc), "--frames", str(last),
                   "--gl-fast", "--gl-cache", str(scratch / f"{label}-cache"),
                   "--dump-frame", str(out / "f.ppm"), "--dump-frame-every", "100"]
        if scale > 1:
            command += ["--gl-scale", str(scale)]
        # The flags decide, not a variable left set in the shell. And the
        # card is reached through a real video driver (a hidden window), not
        # the dummy one the other stages are given.
        clean = {k: v for k, v in env.items()
                 if not k.upper().startswith("GC_GL_") and k.upper() != "SDL_VIDEODRIVER"}
        code = run(command, scratch / f"{label}.log", clean)
        if code != 0:
            s.note(f"{scale}x: the run exited {code}")
            s.lines += tail(scratch / f"{label}.log")
            ok = False
            continue
        if not any("gl: ready" in line and "fast path" in line for line in tail(scratch / f"{label}.log", 100000)):
            s.note(f"{scale}x: the run does not say it drew on the fast path")
            ok = False
            continue
        for field in FRAME_FIELDS:
            name = f"f-{field:06d}.ppm"
            if not (reference / name).exists() or not (out / name).exists():
                s.note(f"{scale}x, field {field}: no frame to compare")
                ok = False
                continue
            close, how = frames_close(reference / name, out / name, over, mean)
            s.note(f"{scale}x, field {field}: {how}{'' if close else ' - OUT OF TOLERANCE'}")
            ok = ok and close
    s.ok = ok
    return s


SKIP_FIELDS = 2600
SKIP_LATE_MAX = 1  # PRESENTS_KEPT_MAX in gc_sdl.cpp


def read_shown_log(path: Path) -> dict[int, tuple[str, int, int]]:
    """gc_sdl --shown-log: field -> (picture hash, field its buffer is from, kept)."""
    rows = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 4:
            rows[int(parts[0])] = (parts[1], int(parts[2]), int(parts[3]))
    return rows


def stage_skip(args, env, scratch: Path) -> Stage:
    """The windowed frontend (dummy video driver, software renderer) skipping
    the storybook and the opening cutscene, with the graphics thread and
    without it. Every present must show the picture the single-threaded run
    showed for the field its buffer came from, at most SKIP_LATE_MAX fields
    back, and the machines must end in the same state."""
    s = Stage("skip")
    script = REPO_ROOT / "tools" / "gc" / "scripts" / "shrek2_skip.txt"
    results = {}
    for mode in ("off", "on"):
        # One save directory path for both: the memory card's identity, and so
        # the state hash, follows its path.
        save = scratch / "skip-save"
        if save.exists():
            shutil.rmtree(save, ignore_errors=True)
        save.mkdir(parents=True)
        log = scratch / f"skip-shown-{mode}.txt"
        run_env = dict(env)
        run_env["GC_GX_THREAD"] = "0" if mode == "off" else "1"
        code = run([str(game_exe(scratch)), str(args.disc), "--renderer", "software", "--play-input", str(script),
                    "--mute", "--unthrottled", "--limit-frames", str(SKIP_FIELDS), "--save-dir", str(save),
                    "--prefs", str(save / "prefs.ini"), "--shown-log", str(log), "--audio-stats"],
                   scratch / f"skip-{mode}.log", run_env)
        if code != 0 or not log.exists():
            s.note(f"thread {mode}: the run exited {code}")
            s.lines += tail(scratch / f"skip-{mode}.log")
            return s
        state = [line.strip() for line in tail(scratch / f"skip-{mode}.log", 100000) if line.startswith("state:")]
        present = [line.strip() for line in tail(scratch / f"skip-{mode}.log", 100000)
                   if line.startswith("present:")]
        results[mode] = (read_shown_log(log), state[-1] if state else "", present[-1] if present else "")
        s.note(f"thread {mode}: {results[mode][2]}")
    ref, got = results["off"][0], results["on"][0]
    wrong, late_max = [], 0
    for field, (picture, source, _kept) in sorted(got.items()):
        late_max = max(late_max, field - source)
        if field - source > SKIP_LATE_MAX or source not in ref or ref[source][0] != picture:
            wrong.append(field)
    same_state = results["off"][1] and results["off"][1] == results["on"][1]
    s.note(f"{len(got)} presents compared, most fields late {late_max}, {len(wrong)} showing the wrong picture"
           + (f" (first at field {wrong[0]})" if wrong else ""))
    s.note(f"machine state {'the same' if same_state else 'DIFFERS'}: {results['on'][1]}")
    s.ok = len(got) >= SKIP_FIELDS - 1 and not wrong and bool(same_state)
    return s


def stage_differential(args, env, scratch: Path) -> Stage:
    s = Stage("differential")
    ax_log = scratch / "differential-ax.log"
    command = [str(game_exe(scratch)), "--disc", str(args.disc), "--frames", str(args.differential_fields),
               "--auto-input", "--differential", "--coverage", "--audio-log", str(ax_log)]
    if args.load_state:
        command += ["--load-state", str(args.load_state)]
    code = run(command, scratch / "differential.log", env, timeout=14400)
    agreed = False
    for line in tail(scratch / "differential.log", 100000):
        if "differential:" in line or "DIVERGED" in line or "native share" in line:
            s.note(line.strip())
            agreed = agreed or "agree at the end of every" in line
    # The sound task has to still be running after New Game (field ~1440):
    # command lists must keep arriving, or the audio engine loops one buffer.
    lists_ok = True
    try:
        rows = [line.split() for line in ax_log.read_text().splitlines() if not line.startswith("#")]
        by_field = {int(r[0]): int(r[1]) for r in rows if len(r) >= 2}
        checkpoints = [f for f in (1500, args.differential_fields) if f in by_field]
        if len(checkpoints) == 2 and checkpoints[1] > checkpoints[0]:
            before, after = by_field[checkpoints[0]], by_field[checkpoints[1]]
            s.note(f"sound command lists: {before} at field {checkpoints[0]}, {after} at field {checkpoints[1]}")
            lists_ok = after > before
    except (OSError, ValueError):
        s.note("no sound log")
    s.ok = code == 0 and agreed and lists_ok
    return s


# ------------------------------------------------------------------ main


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    tier = parser.add_mutually_exclusive_group()
    tier.add_argument("--quick", action="store_true", help="everything except the differential (default)")
    tier.add_argument("--full", action="store_true", help="add the slow test programs, the release test and "
                                                          "the gameplay differential")
    parser.add_argument("--build", type=Path, default=REPO_ROOT / "build")
    parser.add_argument("--no-build", action="store_true", help="use the build tree as it is")
    parser.add_argument("--disc", type=Path, help="a GameCube disc image for game-build, frames and differential")
    parser.add_argument("--frames-ref", type=Path, help="reference frames (f-NNNNNN.ppm or fNNN.ppm)")
    parser.add_argument("--save-frames-ref", type=Path, help="copy this run's frames here as a new reference")
    parser.add_argument("--differential-fields", type=int, default=2400)
    parser.add_argument("--load-state", type=Path, help="start the differential from a saved state")
    parser.add_argument("--sdl2-dir", default=os.environ.get("SDL2_DIR"))
    parser.add_argument("--scratch", type=Path, default=REPO_ROOT / "build" / "run-checks")
    parser.add_argument("--vcvars", help="vcvars64.bat, when cl is not on PATH")
    args = parser.parse_args()
    args.build = args.build.resolve()
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    if args.disc:
        args.disc = args.disc.resolve()
        if not args.disc.exists():
            print(f"run_checks: {args.disc} does not exist", file=sys.stderr)
            return 2

    # The test programs and scripts come from this checkout, so the build tree
    # has to be this checkout's too - another one's binaries disagree with them.
    cache = args.build / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:"):
                source = Path(line.partition("=")[2]).resolve()
                if source != REPO_ROOT:
                    print(f"run_checks: {args.build} was configured from {source}, not {REPO_ROOT}; "
                          f"run the copy of this script in that checkout", file=sys.stderr)
                    return 2

    env = compiler_env(find_vcvars(args.vcvars))
    started = time.monotonic()
    stages: list[Stage] = []
    stage_times: dict[str, float] = {}

    def timed(fn, *extra) -> Stage:
        t = time.monotonic()
        say(f"run_checks: {fn.__name__[6:]} started at +{t - started:.0f} s")
        try:
            stage = fn(args, env, scratch, *extra)
        except Exception as error:  # a broken stage is a failure, not a crash of the others
            stage = Stage(fn.__name__[6:].replace("_", "-"))
            stage.note(f"the stage itself failed: {error!r}")
        stage.seconds = time.monotonic() - t
        say(f"run_checks: {stage.name} {'ok' if stage.ok else ('skipped' if stage.skipped else 'FAILED')} "
            f"in {stage.seconds:.0f} s")
        return stage

    if not args.no_build:
        build = timed(stage_build)
        stages.append(build)
        if not build.ok:
            return report(stages, started)

    with ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(timed, stage_ctest), pool.submit(timed, stage_recompiled)]
        if args.disc:
            def game_chain() -> list[Stage]:
                chain = [timed(stage_game_build)]
                if not chain[0].ok:
                    return chain
                def frames_then_audio() -> list[Stage]:
                    frames = timed(stage_frames)
                    return [frames, timed(stage_audio), timed(stage_fast_frames)]
                follow = [pool.submit(frames_then_audio)]
                if args.full:
                    follow.append(pool.submit(timed, stage_differential))
                    follow.append(pool.submit(timed, stage_skip))
                results: list[Stage] = []
                for f in follow:
                    got = f.result()
                    results.extend(got if isinstance(got, list) else [got])
                return chain + results
            futures.append(pool.submit(game_chain))
        for future in futures:
            result = future.result()
            stages.extend(result if isinstance(result, list) else [result])
    return report(stages, started)


def report(stages: list[Stage], started: float) -> int:
    wall = time.monotonic() - started
    print("\nrun_checks: stage          result   seconds")
    for s in stages:
        print(f"run_checks: {s.name:<14} {'ok' if s.ok else 'FAILED':<8} {s.seconds:7.0f}")
        for line in s.lines:
            print(f"run_checks:     {line}")
    total = sum(s.seconds for s in stages)
    ok = all(s.ok for s in stages)
    print(f"run_checks: {total:.0f} s of stage time in {wall:.0f} s wall - "
          f"{'everything passed' if ok else 'SOMETHING FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
