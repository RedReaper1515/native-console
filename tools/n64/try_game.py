#!/usr/bin/env python3
"""Try a Nintendo 64 game end to end and write a report.

    python tools/n64/try_game.py "C:/Users/me/Downloads/Game (USA).zip" [--frames 1800]

The ROM (.z64, .n64, .v64, or a .zip holding one) is copied into
output/n64-try/<name>/, never into roms/ or git. Then, with --save-dir set to
a scratch folder so no player's save is touched:

1. the interpreter (build/bin/n64_run) runs the game headless for --frames
   frames, and again with --hle-boot (IPL3 skipped), saving the last frame
   of each and timing the runs;
2. n64recomp profiles the game and compiles it into a project, which is
   configured and built with this checkout's SDL2 (from build/CMakeCache.txt);
3. the native build runs the same frames with --coverage (the share of
   instructions that ran as native code) and then a shorter --differential
   run against the interpreter.

Frames are written as .bmp next to the report, logs/n64-try-<name>.txt. Every
step's command and output is in the report; paste it back as it is.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ".exe" if os.name == "nt" else ""
ROM_EXTS = (".z64", ".n64", ".v64")


def ppm_to_bmp(ppm: Path, bmp: Path) -> bool:
    data = ppm.read_bytes()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not m:
        return False
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    row = (w * 3 + 3) & ~3
    out = bytearray(b"BM" + (54 + row * h).to_bytes(4, "little") + bytes(4) + (54).to_bytes(4, "little"))
    out += (40).to_bytes(4, "little") + w.to_bytes(4, "little") + h.to_bytes(4, "little")
    out += (1).to_bytes(2, "little") + (24).to_bytes(2, "little") + bytes(4) + (row * h).to_bytes(4, "little")
    out += bytes(16)
    for y in range(h - 1, -1, -1):
        line = px[y * w * 3:(y + 1) * w * 3]
        rgb = bytearray(line)
        rgb[0::3], rgb[2::3] = line[2::3], line[0::3]
        out += rgb + bytes(row - w * 3)
    bmp.write_bytes(bytes(out))
    return True


class Report:
    def __init__(self, path: Path):
        self.path = path
        self.lines = []

    def say(self, text=""):
        print(text, flush=True)
        self.lines.append(text)
        self.path.write_text("\n".join(self.lines) + "\n", encoding="utf-8")

    def run(self, title, cmd, timeout, cwd=None):
        self.say(f"== {title}")
        self.say("$ " + " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd))
        start = time.perf_counter()
        try:
            r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, cwd=cwd)
            out, code = (r.stdout + r.stderr), r.returncode
        except subprocess.TimeoutExpired as e:
            out = ((e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or ""))
            out += f"\n(timed out after {timeout} s)"
            code = -1
        secs = time.perf_counter() - start
        tail = out.strip().splitlines()[-40:]
        for line in tail:
            self.say("   " + line)
        self.say(f"-> exit {code}, {secs:.1f} s")
        self.say()
        return code, out, secs


def extract_rom(src: Path, dest_dir: Path) -> Path:
    dest_dir.mkdir(parents=True, exist_ok=True)
    if src.suffix.lower() == ".zip":
        with zipfile.ZipFile(src) as z:
            names = [n for n in z.namelist() if n.lower().endswith(ROM_EXTS)]
            if not names:
                raise SystemExit(f"{src}: no .z64/.n64/.v64 inside")
            target = dest_dir / Path(names[0]).name
            with z.open(names[0]) as f, open(target, "wb") as o:
                shutil.copyfileobj(f, o)
            return target
    if src.suffix.lower() not in ROM_EXTS:
        raise SystemExit(f"{src}: expected a .z64, .n64, .v64 or .zip")
    target = dest_dir / src.name
    shutil.copyfile(src, target)
    return target


def sdl2_dir(build: Path):
    cache = build / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("SDL2_DIR:"):
            value = line.split("=", 1)[1]
            return value if value and not value.endswith("NOTFOUND") else None
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rom")
    ap.add_argument("--build", default=str(ROOT / "build"))
    ap.add_argument("--frames", type=int, default=1800, help="frames per headless run (60 a second)")
    ap.add_argument("--diff-frames", type=int, default=300)
    ap.add_argument("--profile-frames", type=int, default=3000)
    ap.add_argument("--keys", action="append", default=[], help="passed to every run, e.g. 600:START")
    ap.add_argument("--timeout", type=int, default=3600)
    args = ap.parse_args()

    build = Path(args.build)
    tools = {n: build / "bin" / f"{n}{EXE}" for n in ("n64_run", "n64recomp")}
    for name, path in tools.items():
        if not path.exists():
            raise SystemExit(f"{path} not found: build this checkout first (ninja -C {build})")

    src = Path(args.rom)
    stem = re.sub(r"[^A-Za-z0-9]+", "_", src.stem).strip("_").lower()[:40] or "game"
    work = ROOT / "output" / "n64-try" / stem
    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    shots = logs / f"n64-try-{stem}"
    shots.mkdir(exist_ok=True)
    saves = work / "saves"
    if work.exists():
        shutil.rmtree(work)
    saves.mkdir(parents=True)
    rep = Report(logs / f"n64-try-{stem}.txt")
    rom = extract_rom(src, work / "rom")
    keys = [a for k in args.keys for a in ("--keys", k)]
    git = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    rep.say(f"N64 try: {src.name}")
    rep.say(f"checkout {git.stdout.strip()}, {sys.platform}, frames {args.frames}")
    rep.say()

    # 1. Interpreter, with the cartridge's IPL3 and with it skipped.
    for label, extra in (("ipl3", []), ("hle-boot", ["--hle-boot"])):
        ppm = work / f"interp-{label}.ppm"
        code, out, secs = rep.run(f"interpreter, {label}",
                                  [tools["n64_run"], rom, "--frames", args.frames, "--dump-frame", ppm,
                                   "--save-dir", saves, "--isviewer", *extra, *keys], args.timeout)
        if ppm.exists() and ppm_to_bmp(ppm, shots / f"interp-{label}.bmp"):
            rep.say(f"frame: {shots / f'interp-{label}.bmp'}")
        if code == 0 and secs > 0:
            rep.say(f"speed: {args.frames / secs:.1f} frames/s ({args.frames / secs / 60 * 100:.0f}% of full speed)")
        rep.say()

    # 2. Recompile with a profile, configure, build.
    project = work / "project"
    code, _, _ = rep.run("n64recomp", [tools["n64recomp"], rom, "-o", project, "--name", stem,
                                       "--profile", work / "profile" / "boot.txt",
                                       "--profile-frames", args.profile_frames], args.timeout)
    if code:
        rep.say("STOP: n64recomp failed")
        return 1
    cfg = ["cmake", "-G", "Ninja", "-S", project, "-B", project / "build", "-DCMAKE_BUILD_TYPE=Release"]
    sdl = sdl2_dir(build)
    if sdl:
        cfg.append(f"-DSDL2_DIR={sdl}")
    code, _, _ = rep.run("configure", cfg, args.timeout)
    if code:
        rep.say("STOP: configure failed")
        return 1
    code, _, secs = rep.run("build", ["ninja", "-C", project / "build"], args.timeout * 2)
    if code:
        rep.say("STOP: build failed")
        return 1
    game = project / "build" / f"{stem}{EXE}"

    # 3. The native build: coverage, then lockstep with the interpreter.
    ppm = work / "native.ppm"
    code, out, secs = rep.run("native, --coverage",
                              [game, "--frames", args.frames, "--dump-frame", ppm, "--coverage",
                               "--save-dir", saves, *keys], args.timeout)
    if ppm.exists() and ppm_to_bmp(ppm, shots / "native.bmp"):
        rep.say(f"frame: {shots / 'native.bmp'}")
    if code == 0 and secs > 0:
        rep.say(f"speed: {args.frames / secs:.1f} frames/s headless")
    a, b = work / "interp-ipl3.ppm", ppm
    if a.exists() and b.exists():
        rep.say("last frame, native vs interpreter: " + ("identical" if a.read_bytes() == b.read_bytes() else "DIFFERENT"))
    rep.say()
    rep.run("native, --differential", [game, "--frames", args.diff_frames, "--differential",
                                       "--save-dir", saves, *keys], args.timeout)
    rep.say(f"windowed: {game} --save-dir {saves}")
    rep.say(f"report: {rep.path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
