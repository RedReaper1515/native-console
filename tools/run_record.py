#!/usr/bin/env python3
"""Recorded runs: what is in one, whether it plays again, and running a game's
recorders over it.

A run is a folder a game wrote with `--record-run DIR` while somebody played:
one `segment-NNNN.run` for each sitting, beside the cartridge memory the
sitting began with (`segment-NNNN.sav`). The format is in
runtime/include/gbrt_run.h and RUN_RECORDS.md.

    run_record.py info RUN
        What the run holds: its segments, how long each is, whether each was
        closed, and what save each began with.

    run_record.py verify RUN --game GAME.exe [--states DIR] [-- ARGS...]
        Play every segment again with nobody at the controls and say whether
        each arrives where the recording did. With --states, also write a
        save state every --state-frames frames, for `sweep`. ARGS are added
        to the game's command line: a run played with a pack of unpacked
        data that replaces something (`info` says so) replays only with it,

            run_record.py verify RUN --game game.exe -- --unpacked game.gbup

    run_record.py sweep RUN --game GAME.exe --states DIR --out DIR -- ARGS...
        Play the run again in pieces, one from each state, side by side, with
        ARGS added to the game's command line. `{out}` in ARGS becomes a path
        of the piece's own under --out:

            run_record.py sweep RUN --game game.exe --states S --out U \\
                -- --data-use {out}.json

The game has to be built from the ROM the run was played on; it says so if it
is not.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SEGMENT_NAME = re.compile(r"segment-(\d{4})\.run$")
STATE_NAME = re.compile(r"segment-(\d{4})-f(\d{9})\.state$")
RESULT_LINE = re.compile(r"^\[RUN\] result (.*)$", re.M)
LEFT_LINE = re.compile(r"^\[RUN\] the replay left the record.*$", re.M)


class Segment:
    """One sitting, as its file has it."""

    def __init__(self, path: Path):
        self.path = path
        self.number = int(SEGMENT_NAME.search(path.name).group(1))
        self.header: dict[str, list[str]] = {}
        self.inputs = 0
        self.checkpoints = 0
        self.last_cycle = 0
        self.last_frame = 0
        self.end: list[str] | None = None
        self.cut_off = False
        self._read()

    def _read(self) -> None:
        with open(self.path, "r", encoding="ascii", errors="replace", newline="") as file:
            first = file.readline().split()
            if first[:1] != ["native-console-run"]:
                raise ValueError(f"{self.path} is not a run record")
            self.header["native-console-run"] = first[1:]
            for line in file:
                words = line.split()
                if words == ["begin"]:
                    break
                if words:
                    self.header[words[0]] = words[1:]
            else:
                raise ValueError(f"{self.path}: the header has no end")
            for line in file:
                if not line.endswith("\n"):
                    self.cut_off = True  # the recording stopped half way through a line
                    break
                words = line.split()
                if not words:
                    continue
                if words[0] == "i":
                    self.inputs += 1
                    self.last_cycle = int(words[1])
                elif words[0] == "k":
                    self.checkpoints += 1
                    self.last_frame = int(words[1])
                    self.last_cycle = int(words[2])
                elif words[0] == "e":
                    self.end = words[1:]
                    self.last_cycle = int(words[1])
                    self.last_frame = int(words[2])

    @property
    def clock(self) -> int:
        return int(self.header.get("clock", ["4194304"])[0])

    @property
    def seconds(self) -> float:
        return self.last_cycle / self.clock

    @property
    def rom(self) -> str:
        return self.header["rom"][0]

    @property
    def title(self) -> str:
        return " ".join(self.header["rom"][2:])

    @property
    def model(self) -> str:
        return self.header["model"][0]

    @property
    def start_save(self) -> str | None:
        save = self.header.get("save", ["-"])
        return None if save[0] == "-" else save[2]

    @property
    def start_save_blank(self) -> bool:
        return self.header.get("save", ["-"])[-1] == "blank"

    @property
    def replacements(self) -> tuple[int, str]:
        """How many replacements the pack of unpacked data had that the
        sitting was played with, and what tells them from any others."""
        words = self.header.get("unpacked", ["0", "-"])
        return int(words[0]), words[1] if len(words) > 1 else "-"

    @property
    def end_save(self) -> str | None:
        if not self.end or len(self.end) < 4 or self.end[3] == "-":
            return None
        return self.end[3]


def read_run(run: Path, only: set[int] | None = None) -> list[Segment]:
    if run.is_file():
        paths = [run]
    else:
        paths = sorted(p for p in run.iterdir() if SEGMENT_NAME.search(p.name))
    if not paths:
        raise SystemExit(f"{run} holds no segment-NNNN.run")
    segments = [Segment(p) for p in paths]
    if only:
        segments = [s for s in segments if s.number in only]
        if not segments:
            raise SystemExit("none of the segments asked for is in the run")
    return segments


def clock_text(seconds: float) -> str:
    whole = int(seconds)
    return f"{whole // 3600}:{whole // 60 % 60:02d}:{whole % 60:02d}"


def begins_with(segment: Segment, earlier: list[Segment]) -> str:
    if segment.start_save is None:
        return "no cartridge memory"
    # A sitting that was not closed wrote no save: the next one begins with
    # what the last closed one left.
    unclosed = []
    for previous in reversed(earlier):
        if previous.end_save == segment.start_save:
            note = f" ({', '.join(unclosed)} not closed)" if unclosed else ""
            return f"segment {previous.number}'s save{note}"
        if previous.end is not None:
            break
        unclosed.insert(0, f"segment {previous.number}")
    if segment.start_save_blank:
        return "a blank save"
    return "a save from outside the run"


def command_info(args: argparse.Namespace) -> int:
    segments = read_run(Path(args.run))
    first = segments[0]
    print(f"{args.run}: {first.title or '(no title)'}, {first.header['console'][0]} "
          f"{first.model}, ROM sha256 {first.rom}")
    print(f"{'segment':>7}  {'started':<16}  {'length':>9}  {'inputs':>9}  "
          f"{'checkpoints':>11}  {'closed':<6}  begins with")
    earlier: list[Segment] = []
    for segment in segments:
        started = int(segment.header.get("started", ["0"])[0])
        when = (datetime.datetime.fromtimestamp(started).strftime("%Y-%m-%d %H:%M")
                if started else "-")
        closed = "yes" if segment.end else ("cut" if segment.cut_off else "no")
        print(f"{segment.number:>7}  {when:<16}  {clock_text(segment.seconds):>9}  "
              f"{segment.inputs:>9,}  {segment.checkpoints:>11,}  {closed:<6}  "
              f"{begins_with(segment, earlier)}")
        if segment.rom != first.rom:
            print(f"         segment {segment.number} is of another ROM: {segment.rom}")
        count, identity = segment.replacements
        if count:
            print(f"         played with a pack of unpacked data, and replays only with it "
                  f"(--unpacked): {count} replacement{'' if count == 1 else 's'}, {identity}")
        earlier.append(segment)
    print(f"{len(segments)} segment(s), {clock_text(sum(s.seconds for s in segments))} "
          f"played, {sum(s.inputs for s in segments):,} inputs")
    return 0


def model_args(segment: Segment) -> list[str]:
    return ["--model", "dmg" if segment.model == "dmg" else "cgb"]


def run_game(command: list[str]) -> tuple[int, str]:
    """The game's exit code and everything it printed."""
    with tempfile.TemporaryDirectory(prefix="run-record-") as scratch:
        # A replay writes no save; the folder keeps the game from so much as
        # looking at one beside its executable.
        done = subprocess.run(command + ["--save-dir", scratch], capture_output=True,
                              text=True, errors="replace")
    return done.returncode, done.stdout + done.stderr


def result_of(output: str) -> dict[str, str]:
    found = RESULT_LINE.search(output)
    if not found:
        return {}
    return dict(word.split("=", 1) for word in found.group(1).split() if "=" in word)


def last_lines(output: str, count: int = 6) -> str:
    return "\n".join("    " + line for line in output.strip().splitlines()[-count:])


def command_verify(args: argparse.Namespace) -> int:
    game = str(Path(args.game).resolve())
    segments = read_run(Path(args.run), parse_numbers(args.segments))
    if args.states:
        Path(args.states).mkdir(parents=True, exist_ok=True)

    def verify(segment: Segment) -> tuple[Segment, int, str]:
        command = [game, "--headless", "--replay-run", str(segment.path.resolve())]
        command += model_args(segment)
        if args.states:
            command += ["--run-states", str(Path(args.states).resolve()),
                        "--run-state-frames", str(args.state_frames)]
        code, output = run_game(command + args.game_args)
        return segment, code, output

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for segment, code, output in pool.map(verify, segments):
            result = result_of(output)
            end = result.get("end", "")
            good = code == 0 and end in ("matched", "unclosed")
            if not good:
                failed += 1
            if not result:
                print(f"segment {segment.number}: the game did not replay it "
                      f"(exit code {code})\n{last_lines(output)}")
                continue
            line = (f"segment {segment.number}: {result['matched']} of "
                    f"{segment.checkpoints:,} checkpoints reached, "
                    f"{result['inputs']} of {segment.inputs:,} inputs, "
                    f"{clock_text(int(result['cycles']) / segment.clock)} played")
            if end == "matched":
                line += ", ended where the recording did"
            elif end == "unclosed":
                line += ", to the last line of a recording that was not closed"
            else:
                left = LEFT_LINE.search(output)
                line += "\n    " + (left.group(0) if left else f"end={end}, exit code {code}")
            if int(result.get("late", "0")):
                line += (f"\n    {result['late']} input(s) could not be given on their "
                         f"cycle (up to {result['max_late']} cycles late)")
            print(line)
    print("the run replays" if not failed else f"{failed} segment(s) did not replay")
    return 1 if failed else 0


def parse_numbers(text: str | None) -> set[int] | None:
    if not text:
        return None
    return {int(word) for word in text.split(",") if word.strip()}


def command_sweep(args: argparse.Namespace) -> int:
    game = str(Path(args.game).resolve())
    segments = {s.number: s for s in read_run(Path(args.run), parse_numbers(args.segments))}
    states: dict[int, list[tuple[int, Path]]] = {}
    for path in sorted(Path(args.states).iterdir()):
        found = STATE_NAME.search(path.name)
        if found and int(found.group(1)) in segments:
            states.setdefault(int(found.group(1)), []).append((int(found.group(2)), path))
    if not states:
        raise SystemExit(f"{args.states} holds no state of this run; "
                         "write them with: verify --states")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    pieces = []
    for number, found in sorted(states.items()):
        found.sort()
        for index, (frame, path) in enumerate(found):
            # To the next state; the last piece runs to the segment's end.
            frames = found[index + 1][0] - frame if index + 1 < len(found) else 0
            pieces.append((segments[number], frame, path, frames))

    def sweep(piece: tuple[Segment, int, Path, int]) -> tuple[str, int, str]:
        segment, frame, state, frames = piece
        name = f"segment-{segment.number:04d}-f{frame:09d}"
        target = str((out / name).resolve())
        command = [game, "--headless", "--replay-run", str(segment.path.resolve()),
                   "--load-state-file", str(state.resolve())]
        command += model_args(segment)
        if frames:
            command += ["--limit-frames", str(frames)]
        command += [word.replace("{out}", target) for word in args.game_args]
        code, output = run_game(command)
        return name, code, output

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for name, code, output in pool.map(sweep, pieces):
            result = result_of(output)
            if code != 0 or not result or result.get("end") == "diverged":
                failed += 1
                left = LEFT_LINE.search(output)
                print(f"{name}: " + (left.group(0) if left
                                     else f"exit code {code}\n{last_lines(output)}"))
    print(f"{len(pieces) - failed} of {len(pieces)} pieces replayed, into {out}")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    jobs = max(1, (os.cpu_count() or 2) // 2)

    info = commands.add_parser("info", help="what a run holds")
    info.add_argument("run", help="the run's folder, or one segment-NNNN.run")
    info.set_defaults(function=command_info)

    verify = commands.add_parser("verify", help="play a run again and check it")
    verify.add_argument("run")
    verify.add_argument("--game", required=True, help="the game's executable")
    verify.add_argument("--segments", help="only these, e.g. 1,3")
    verify.add_argument("--jobs", type=int, default=jobs)
    verify.add_argument("--states", help="write save states into this folder")
    verify.add_argument("--state-frames", type=int, default=3600,
                        help="a state every this many frames (3600 is a minute)")
    verify.set_defaults(function=command_verify)

    sweep = commands.add_parser("sweep", help="replay a run in pieces, with extra options")
    sweep.add_argument("run")
    sweep.add_argument("--game", required=True)
    sweep.add_argument("--states", required=True, help="the folder verify --states wrote")
    sweep.add_argument("--out", required=True, help="where each piece's output goes")
    sweep.add_argument("--segments")
    sweep.add_argument("--jobs", type=int, default=jobs)
    sweep.set_defaults(function=command_sweep)

    # What follows a lone -- is for the game, whatever it looks like.
    argv = sys.argv[1:]
    game_args: list[str] = []
    if "--" in argv:
        game_args = argv[argv.index("--") + 1:]
        argv = argv[:argv.index("--")]
    args = parser.parse_args(argv)
    args.game_args = game_args
    return args.function(args)


if __name__ == "__main__":
    sys.exit(main())
