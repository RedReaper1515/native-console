#!/usr/bin/env python3
"""A game played by a program (`--control`), end to end.

Builds the game of `test_run_record.py`, whose memory shows what buttons it
saw and when: its main loop looks at the action buttons without pause and
keeps what it last saw, the joypad interrupt counts itself, and the frame
interrupt counts frames. `tools/gb_control.py` plays it:

- the game stops at the start of a frame, runs as many frames as it is told
  and stops again, and what it is told is held is what the game sees: every
  press reaches it as a press, with its interrupt, and every release;
- watched memory, memory asked for and the ROM read the same, and reading
  changes nothing;
- played with `--record-run`, the run is in the record press for press,
  `run_record.py verify` replays it with no program there, and the replay
  ends where the played game ended;
- while a run is recorded the game refuses a byte written and a state
  loaded, and writes a state when asked; while one is replayed it shows its
  memory frame by frame, sees the record's buttons and not the program's,
  and closes where the record ends;
- with no run, a state written and loaded again puts the game back where it
  was, and it goes on from there as it did the first time; a byte written
  into RAM is there, and one aimed at the ROM is refused;
- the game closes at its frame limit and says so, closes when it is told
  to, and exits with an error when the program goes away, when nobody
  listens on the port, and with a loop that has no place to stop in
  (`--benchmark`).
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from gb_control import ControlError, Game, Refused, held  # noqa: E402
from test_lift_evidence import build  # noqa: E402
from test_run_record import FRAMES_SEEN, MIX, PAD, PRESSES, SAME, make_rom  # noqa: E402

CONTROL = ROOT / "tools" / "gb_control.py"
RUN_RECORD = ROOT / "tools" / "run_record.py"
HRAM = 0xFF00
# What is held, and for how many frames. The game looks at A, B, Select and
# Start; a direction is held with them once and must change nothing it sees.
PLAN = [("", 10), ("A", 3), ("", 5), ("B", 4), ("", 2), ("AS", 6), ("", 7), ("T", 1), ("", 12),
        ("AL", 2), ("", 9)]
PRESSED = [buttons for buttons, _ in PLAN if buttons]


def play(game: Game, plan=PLAN) -> list[tuple[int, int, int]]:
    """Play the plan; what the game had seen at each stop: the buttons, the
    presses counted, the frames counted."""
    seen = []
    for buttons, frames in plan:
        assert game.step(buttons, frames), "the game closed while it was being played"
        seen.append((game.byte(HRAM + PAD), game.byte(HRAM + PRESSES), game.byte(HRAM + FRAMES_SEEN)))
    return seen


def run(command: list, expect: int = 0) -> str:
    done = subprocess.run([str(word) for word in command], capture_output=True, text=True, timeout=300)
    printed = done.stdout + done.stderr
    assert done.returncode == expect, f"exit {done.returncode}, not {expect}, from {command!r}\n{printed[-4000:]}"
    return printed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()

    scratch = tempfile.TemporaryDirectory(prefix="gbcontrol-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom_path = root / "control.gb"
        rom = make_rom()
        rom_path.write_bytes(rom)
        game_exe = build(args.gbrecomp, rom_path, ["--no-scan", "--pointer-scan", "off"],
                         root / "game", args.cmake_arg)
        saves = root / "saves"
        saves.mkdir()
        watch = [(0xFF80, 0x10), (0xC000, 0x20)]

        # Played and recorded.
        run_dir, played_dump = root / "the run", root / "played.json"
        with Game([game_exe, "--headless", "--record-run", run_dir, "--dump-state", played_dump],
                  watch=watch, log=root / "played.log") as game:
            assert game.frame == 0 and game.recording and not game.replaying, (game.frame, game.recording)
            assert game.read(0x134, 10) == b"RUN RECORD" == bytes(rom[0x134:0x13E]), "the ROM does not read as it is"
            seen = play(game)
            assert game.frame == sum(frames for _, frames in PLAN), game.frame
            for (buttons, frames), (pad, _, _) in zip(PLAN, seen):
                assert pad == 0x0F & ~held(buttons), f"with {buttons or 'nothing'} held the game saw {pad:02X}"
            assert [presses for _, presses, _ in seen][-1] == len(PRESSED), seen
            counted = [frames for _, _, frames in seen]
            for (_, frames), before, after in zip(PLAN[2:], counted[1:], counted[2:]):
                assert (after - before) & 0xFF == frames, f"{frames} frames asked for, {after - before} counted"
            # Memory watched, memory asked for, and asking twice.
            assert game.bytes_at(0xFF80, 0x10) == game.read(0xFF80, 0x10) == game.read(0xFF80, 0x10)
            assert game.byte(0xC005) == game.read(0xC005)[0] and len(game.read(0x0000, 0x10000)) == 0x10000
            try:
                game.read(0xFFF0, 0x20)
                raise AssertionError("a read past $FFFF was answered")
            except Refused as refused:
                assert "past $FFFF" in str(refused)
            # What would not be in the record is refused, and nothing changes.
            before = game.read(0xFF80, 0x7F)
            for what, word in ((lambda: game.write(0xFF8A, 0x5A), "no byte is written"),
                               (lambda: game.load_state(root / "none.state"), "no state is loaded")):
                try:
                    what()
                    raise AssertionError(f"{word}: not refused while a run is recorded")
                except Refused as refused:
                    assert word in str(refused) and "recorded" in str(refused), str(refused)
            assert game.read(0xFF80, 0x7F) == before
            game.save_state(root / "recorded.state")
            assert (root / "recorded.state").stat().st_size > 0x2000
            assert game.close() == 0, (root / "played.log").read_text(errors="replace")[-2000:]
        played = json.loads(played_dump.read_text(encoding="utf-8"))

        # The record has every press, and replays with nobody playing.
        segment = run_dir / "segment-0001.run"
        inputs = [line.split()[2:] for line in segment.read_text(encoding="ascii").splitlines()
                  if line.startswith("i ")]
        letters = {"A": "A", "B": "B", "AS": "AS", "T": "T", "AL": "LA"}
        assert [words[0] for words in inputs[0::2]] == [letters[buttons] for buttons in PRESSED], inputs
        assert all(words == ["-"] for words in inputs[1::2]) and len(inputs) == 2 * len(PRESSED), inputs
        assert all(words[1:] == ["!"] for words in inputs[0::2]), "a press did not ask for the joypad interrupt"
        verified = run([sys.executable, RUN_RECORD, "verify", run_dir, "--game", game_exe])
        assert "the run replays" in verified and "ended where the recording did" in verified, verified

        # Replayed with a program watching: the record's buttons, not the program's.
        replay_dump = root / "replayed.json"
        with Game([game_exe, "--headless", "--replay-run", segment, "--save-dir", saves,
                   "--dump-state", replay_dump], watch=watch, log=root / "replayed.log") as game:
            assert game.replaying and not game.recording
            watched, frames = [], 0
            for (_, count), expected in zip(PLAN, seen):
                # Start held throughout, which would save and change the game's memory if it were seen.
                if not game.step("S", count):
                    break
                frames += count
                watched.append((game.byte(HRAM + PAD), game.byte(HRAM + PRESSES), game.byte(HRAM + FRAMES_SEEN)))
            assert watched == seen[:len(watched)] and len(watched) >= len(seen) - 1, (watched, seen)
            try:
                if not game.closed:
                    game.write(0xFF8A, 0x5A)
                    raise AssertionError("a byte was written while a run is replayed")
            except Refused as refused:
                assert "replayed" in str(refused), str(refused)
            while not game.closed and game.step("S", 5):
                pass
            assert game.close() == 0, (root / "replayed.log").read_text(errors="replace")[-2000:]
        result = re.search(r"^\[RUN\] result (.*)$", (root / "replayed.log").read_text(errors="replace"), re.M)
        assert result and "end=matched" in result.group(1) and "late=0" in result.group(1), result
        replayed = json.loads(replay_dump.read_text(encoding="utf-8"))
        assert all(replayed[key] == played[key] for key in SAME), "the replay ends elsewhere than the played game"

        # With no run: a state written and loaded, a byte written.
        state = root / "there.state"
        with Game([game_exe, "--headless", "--save-dir", saves, "--rtc-unix-time", 1000000],
                  watch=watch, log=root / "free.log") as game:
            assert not game.recording and not game.replaying
            play(game, PLAN[:4])
            game.save_state(state)
            there = game.memory
            first = play(game, PLAN[4:8])
            onward = game.memory
            assert onward != there
            game.load_state(state)
            assert game.memory == there, "the state loaded is not the state written"
            second = play(game, PLAN[4:8])
            assert second == first and game.memory == onward, "from the state the game went another way"
            game.write(0xFF8A, b"\x5A\xA5")
            assert game.bytes_at(0xFF8A, 2) == b"\x5A\xA5" == game.read(0xFF8A, 2)
            game.write(0xC010, 0x77)
            assert game.byte(0xC010) == 0x77
            for address in (0x0150, 0xFF40):
                try:
                    game.write(address, 0)
                    raise AssertionError(f"a byte was written at {address:04X}")
                except Refused as refused:
                    assert "RAM only" in str(refused), str(refused)
            assert game.close() == 0

        # The game's own frame limit closes it, and the program is told.
        with Game([game_exe, "--headless", "--save-dir", saves, "--limit-frames", 50], log=root / "limit.log") as game:
            assert game.step("", 20) and game.frame == 20
            assert not game.step("A", 100) and game.closed and game.frame == 50, (game.closed, game.frame)
            assert game.exit_code == 0
            try:
                game.step()
                raise AssertionError("a closed game ran on")
            except ControlError:
                pass

        # The program gone, the game closes with an error and leaves no window open.
        game = Game([game_exe, "--headless", "--save-dir", saves], log=root / "gone.log")
        assert game.step("", 5)
        game._connection.close()
        assert game.process.wait(timeout=30) == 1, "the game did not close when its program went away"
        game._drop()
        assert "the program playing the game is gone" in (root / "gone.log").read_text(errors="replace")

        # Nobody listening, and a loop with no place to stop in.
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        free_port = listener.getsockname()[1]
        listener.close()
        told = run([game_exe, "--headless", "--save-dir", saves, "--control", free_port, "--limit-frames", 5], expect=1)
        assert "nothing is listening" in told, told
        try:
            Game([game_exe, "--benchmark", "--save-dir", saves, "--limit-frames", 5], log=root / "benchmark.log")
            raise AssertionError("--control was taken with --benchmark")
        except ControlError as refused:
            assert "before it connected" in str(refused), str(refused)
        assert "cannot be played by a program (--control) with --benchmark" in \
            (root / "benchmark.log").read_text(errors="replace")
        assert not [path for path in game_exe.parent.iterdir() if path.suffix in (".sav", ".rtc")], \
            "a refused game wrote a save beside itself"

        # The tool as a command.
        printed = run([sys.executable, CONTROL, game_exe, "--frames", 40, "--hold", "A", "--read", "FF86:1",
                       "--", "--headless", "--save-dir", saves])
        assert "frame 40," in printed and "FF86: 0e" in printed, printed
    finally:
        if scratch:
            scratch.cleanup()
    print("a game played by a program: played, recorded, replayed, put back, closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
