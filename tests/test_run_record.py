#!/usr/bin/env python3
"""A played run recorded and played again, end to end.

Builds a small game on a cartridge with memory, a battery and a clock, made
so that everything a run depends on shows in its memory:

- its main loop looks at the buttons without pause and, when they change,
  mixes how many times it has looked into a byte: a button that goes down
  one cycle late gives another byte;
- the joypad interrupt counts itself;
- holding Start writes that byte to the cartridge's memory, and the game
  starts from the byte it finds there: a second sitting depends on the first;
- every frame it reads the seconds of the cartridge's clock;
- every frame it reads back the volume it gave the sound hardware.

Two sittings are recorded with `--record-run`, each played by a script
(`--input`). Then:

- `run_record.py info` has to say the second began with the first one's save;
- `run_record.py verify` has to replay both to every checkpoint and to the
  end, and so does a replay with the loop cut another way and a replay in
  the interpreter (`--data-use`), with every input given on its own cycle;
- the clock the second sitting began with, hours ahead of the first, has to
  come from the record and not from the time of day;
- a replay writes no save;
- a recording that was never closed, cut off half way through a line,
  replays to its last whole line;
- a button moved by 4096 cycles, a start save with one byte changed and the
  record of another ROM each have to be told apart from the run;
- `verify --states` writes save states, and `sweep` replays the run in
  pieces from them and ends where the replay in one piece does;
- a run a script played for a few hundred frames, a press every ten, placed
  by cycle, by frame and by period, has every press in its record, replays
  with no script, and ends where the script on its own ends;
- a machine with a part left out (`--no-audio`, `--benchmark`), which the
  game can tell from a whole one, neither records a run nor replays one.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from test_lift_evidence import build
from test_lift_runtime_e2e import NINTENDO_LOGO, assemble, bump, ldh_a, ldh_to

ROOT = Path(__file__).resolve().parent.parent
RUN_RECORD = ROOT / "tools" / "run_record.py"

LOOKS, LOOKS_HIGH, MIX, PRESSES, FRAMES_SEEN, SECONDS, PAD, SOUND = range(0x80, 0x88)
VOLUME = 0x35
FIRST_FRAMES, SECOND_FRAMES, SCRIPTED_FRAMES = 300, 200, 400
FIRST_INPUT = ("c500000:A:300000,c1500000:B:123456,c2600000:AB:400000,c5000000:S:200000,"
               "c7000000:R:100000,c9000000:A:100000,c12000000:T:700000,c15000000:S:300000")
SECOND_INPUT = "c400000:B:200000,c3000000:A:100000,c6000000:S:500000,c9000000:AT:250000"
SAME = ["a", "f", "b", "c", "d", "e", "h", "l", "sp", "pc", "total_cycles", "rtc",
        "hram_ff80_fffe", "wram_bank_0_c000_cfff", "eram_a000_a0ff"]


def scripted_run() -> tuple[str, list[str]]:
    """A press every ten frames, held for four: the `--input` that says so and
    the buttons of each press. A, A, A, Start by cycle, the same by frame,
    then by period, where each fourth press is A and Start together."""
    frame = 70224  # cycles
    presses = [(30 + 10 * index, "S" if index % 4 == 3 else "A") for index in range(24)]
    entries = [f"c{at * frame}:{buttons}:{4 * frame}" for at, buttons in presses[:12]]
    entries += [f"{at}:{buttons}:4" for at, buttons in presses[12:]]
    entries.append(f"p{270 * frame}-{380 * frame}/{10 * frame}:A:{4 * frame}")
    entries.append(f"p{300 * frame}-{380 * frame}/{40 * frame}:S:{4 * frame}")
    held = [buttons for _, buttons in presses] + ["AS" if index % 4 == 3 else "A" for index in range(12)]
    return ",".join(entries), held


def program() -> list:
    return [
        ("label", "Start"),
        [0xF3, 0x31, 0xFF, 0xDF],                # di / ld sp,$DFFF
        [0xAF, 0x21, 0x80, 0xFF, 0x06, 0x10],    # xor a / ld hl,$FF80 / ld b,16
        ("label", "Clear"),
        [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli],a / dec b / jr nz
        # The cartridge's memory on, and the byte the last sitting left in it.
        [0x3E, 0x0A, 0xEA, 0x00, 0x00],          # ld a,$0A / ld [$0000],a
        [0xAF, 0xEA, 0x00, 0x40],                # xor a / ld [$4000],a
        [0xFA, 0x00, 0xA0], ldh_to(MIX),         # ld a,[$A000]
        [0x3E, 0x0F], ldh_to(PAD),               # nothing held
        [0x3E, 0x80], ldh_to(0x26),              # sound on
        [0x3E, VOLUME], ldh_to(0x24),            # a volume to read back
        [0x3E, 0x91], ldh_to(0x40),              # LCD on
        [0x3E, 0x11], ldh_to(0xFF),              # VBlank and joypad interrupts
        [0xAF], ldh_to(0x0F),
        [0x3E, 0x10], ldh_to(0x00),              # the action buttons
        [0xFB],                                  # ei
        ("label", "Main"),
        ldh_a(0x00), [0xE6, 0x0F, 0x47],         # and $0F / ld b,a
        ldh_a(PAD), [0xB8, 0x28], ("rel", "Same"),   # cp b / jr z
        [0x78], ldh_to(PAD),                     # ld a,b
        ldh_a(LOOKS), [0x4F],                    # ld c,a
        ldh_a(MIX), [0x07, 0xA9, 0xA8], ldh_to(MIX),  # rlca / xor c / xor b
        ("label", "Same"),
        ldh_a(LOOKS), [0x3C], ldh_to(LOOKS),     # inc a
        [0x20], ("rel", "Main"),                 # jr nz
        bump(LOOKS_HIGH),
        [0x18], ("rel", "Main"),                 # jr
        ("label", "VBlank"),
        [0xF5], bump(FRAMES_SEEN),               # push af
        ldh_a(PAD), [0xCB, 0x5F, 0x20], ("rel", "NoSave"),  # bit 3,a (Start) / jr nz
        [0xAF, 0xEA, 0x00, 0x40],                # xor a / ld [$4000],a
        ldh_a(MIX), [0xEA, 0x00, 0xA0],          # ld [$A000],a
        ("label", "NoSave"),
        # The clock's seconds: its register, latched, read.
        [0x3E, 0x08, 0xEA, 0x00, 0x40],          # ld a,$08 / ld [$4000],a
        [0xAF, 0xEA, 0x00, 0x60],                # xor a / ld [$6000],a
        [0x3C, 0xEA, 0x00, 0x60],                # inc a / ld [$6000],a
        [0xFA, 0x00, 0xA0], ldh_to(SECONDS),
        # The volume, as the sound hardware has it: $FF where there is none.
        ldh_a(0x24), ldh_to(SOUND),
        [0xF1, 0xD9],                            # pop af / reti
        ("label", "Joypad"),
        [0xF5], bump(PRESSES), [0xF1, 0xD9],
    ]


def make_rom() -> bytearray:
    code, labels = assemble(0x150, program())
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13E] = b"RUN RECORD"
    rom[0x147] = 0x10                            # MBC3, clock, memory, battery
    rom[0x149] = 0x02                            # 8 KiB of memory
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in (0x48, 0x50, 0x58):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x60:0x63] = bytes([0xC3, labels["Joypad"] & 0xFF, labels["Joypad"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom


def run(command: list, expect: int = 0, timeout: int = 300) -> str:
    """Everything the command printed; its exit code has to be `expect`."""
    done = subprocess.run([str(word) for word in command], capture_output=True, text=True,
                          timeout=timeout)
    printed = done.stdout + done.stderr
    assert done.returncode == expect, f"exit {done.returncode}, not {expect}, from {command!r}\n{printed[-4000:]}"
    return printed


def result(printed: str) -> dict[str, str]:
    found = re.search(r"^\[RUN\] result (.*)$", printed, re.M)
    assert found, printed[-2000:]
    return dict(word.split("=", 1) for word in found.group(1).split())


def checkpoints(frames: int) -> int:
    """One as the game starts and one every sixty frames while it runs on."""
    return (frames - 1) // 60 + 1


def lines(path: Path) -> list[str]:
    return path.read_text(encoding="ascii").splitlines()


def copy_run(run_dir: Path, target: Path) -> Path:
    shutil.copytree(run_dir, target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()

    scratch = tempfile.TemporaryDirectory(prefix="gbrun-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom_path = root / "run.gb"
        rom_path.write_bytes(make_rom())
        game = build(args.gbrecomp, rom_path, ["--no-scan", "--pointer-scan", "off"],
                     root / "game", args.cmake_arg)

        def no_save_written() -> bool:
            """Nothing in the folder replays are pointed at, nothing beside the game."""
            return not list(saves.iterdir()) and not [
                path for path in game.parent.iterdir() if path.suffix in (".sav", ".rtc")]

        saves = root / "saves"
        saves.mkdir()
        run_dir = root / "the run"  # a folder with a space in its name, as a player's may have
        tool = [sys.executable, RUN_RECORD]

        # Two sittings. The second starts a day and some hours after the
        # first was closed, by the clock the game is told.
        run([game, "--headless", "--record-run", run_dir, "--input", FIRST_INPUT,
             "--limit-frames", FIRST_FRAMES])
        first, first_save = run_dir / "segment-0001.run", run_dir / "segment-0001.sav"
        assert first.exists() and first_save.read_bytes() == bytes(0x2000), "the first sitting began with a save"
        header = lines(first)[:lines(first).index("begin")]
        assert header[0] == "native-console-run 1" and "console gb" in header, header
        assert any(re.fullmatch(r"rom [0-9a-f]{64} 32768 RUN RECORD", line) for line in header), header
        assert any(re.fullmatch(r"save segment-0001\.sav 8192 [0-9a-f]{64} blank", line) for line in header), header
        assert any(line.startswith("rtc 0 0 0 0 0 ") for line in header), header
        body = lines(first)[len(header) + 1:]
        assert body[0].startswith("k 0 0 "), body[0]
        assert sum(line.startswith("k ") for line in body) == checkpoints(FIRST_FRAMES), body
        assert sum(line.startswith("i ") for line in body) == 16, body
        assert any(line.endswith(" !") for line in body), "no press asked for the joypad interrupt"
        assert re.fullmatch(rf"e \d+ {FIRST_FRAMES} [0-9a-f]{{16}} [0-9a-f]{{64}}", body[-1]), body[-1]

        run([game, "--headless", "--record-run", run_dir, "--input", SECOND_INPUT,
             "--limit-frames", SECOND_FRAMES, "--rtc-unix-time", int(time.time()) + 100000])
        second = run_dir / "segment-0002.run"
        saved = (run_dir / "segment-0002.sav").read_bytes()
        assert saved[0] != 0 and saved[1:] == bytes(0x1FFF), "holding Start did not save"
        clock = next(line for line in lines(second) if line.startswith("rtc ")).split()
        assert int(clock[4]) == 1 and int(clock[3]) == 3, f"the second sitting's clock is not a day ahead: {clock}"

        info = run([*tool, "info", run_dir])
        assert "a blank save" in info and "segment 1's save" in info, info
        assert "2 segment(s)" in info and "RUN RECORD" in info, info

        # Played again: by the tool, with the loop cut another way, and in
        # the interpreter. Never with the time of day the recording had.
        verified = run([*tool, "verify", run_dir, "--game", game])
        assert verified.count("ended where the recording did") == 2 and "the run replays" in verified, verified
        ends = {}
        for segment, frames in ((first, FIRST_FRAMES), (second, SECOND_FRAMES)):
            for name, flags in (("plain", []), ("cut", ["--no-smooth-lcd-transitions"]),
                                ("interpreted", ["--data-use", root / "use.json"])):
                dump = root / f"{segment.stem}-{name}.json"
                played = result(run([game, "--headless", "--replay-run", segment, "--save-dir", saves,
                                     "--dump-state", dump, *flags]))
                assert played["end"] == "matched" and played["late"] == "0", (name, played)
                assert played["matched"] == played["checkpoints"] == str(checkpoints(frames)), (name, played)
                state = json.loads(dump.read_text(encoding="utf-8"))
                ends.setdefault(segment, state)
                assert all(state[key] == ends[segment][key] for key in SAME), name
        state = ends[second]
        assert state["hram_ff80_fffe"][PRESSES - 0x80] > 0, "the joypad interrupt never ran"
        assert state["eram_a000_a0ff"][0] != saved[0], "the second sitting's Start did not save"
        assert no_save_written(), "a replay wrote a save"

        # A recording nobody closed, cut off in the middle of a line.
        cut = copy_run(run_dir, root / "cut")
        text = (cut / "segment-0001.run").read_text(encoding="ascii")
        (cut / "segment-0001.run").write_text(text[:text.rindex("\ne ") - 7], encoding="ascii", newline="")
        played = result(run([game, "--headless", "--replay-run", cut / "segment-0001.run", "--save-dir", saves]))
        assert played["end"] == "unclosed" and played["matched"] == played["checkpoints"], played
        assert "not closed" in run([*tool, "verify", cut, "--game", game, "--segments", "1"])

        # What is not the run is told from it.
        moved = copy_run(run_dir, root / "moved")
        changed = lines(moved / "segment-0001.run")
        at = next(index for index, line in enumerate(changed) if line.startswith("i ") and " A" in line)
        words = changed[at].split()
        changed[at] = " ".join([words[0], str(int(words[1]) + 4096), *words[2:]])
        (moved / "segment-0001.run").write_text("\n".join(changed) + "\n", encoding="ascii", newline="")
        told = run([*tool, "verify", moved, "--game", game, "--segments", "1"], expect=1)
        assert "the replay left the record in frame" in told and "1 segment(s) did not replay" in told, told

        other_save = copy_run(run_dir, root / "other-save")
        data = bytearray((other_save / "segment-0002.sav").read_bytes())
        data[0x100] ^= 1
        (other_save / "segment-0002.sav").write_bytes(data)
        told = run([game, "--headless", "--replay-run", other_save / "segment-0002.run", "--save-dir", saves], expect=1)
        assert "is not the save the segment began with" in told, told

        other_rom = copy_run(run_dir, root / "other-rom")
        changed = lines(other_rom / "segment-0001.run")
        at = next(index for index, line in enumerate(changed) if line.startswith("rom "))
        words = changed[at].split(" ", 2)
        changed[at] = " ".join([words[0], "0" * 64, words[2]])
        (other_rom / "segment-0001.run").write_text("\n".join(changed) + "\n", encoding="ascii", newline="")
        told = run([game, "--headless", "--replay-run", other_rom / "segment-0001.run", "--save-dir", saves], expect=1)
        assert "the record is of another ROM" in told, told

        told = run([game, "--headless", "--record-run", root / "both", "--replay-run", first], expect=1)
        assert "--record-run with --replay-run" in told, told
        assert not (root / "both").exists()
        assert no_save_written(), "a refused replay wrote a save"

        # States along the way, and the run replayed in pieces from them.
        states, pieces = root / "states", root / "pieces"
        verified = run([*tool, "verify", run_dir, "--game", game, "--states", states, "--state-frames", "120"])
        assert "the run replays" in verified, verified
        names = sorted(path.name for path in states.iterdir())
        assert names == [f"segment-0001-f{frame:09d}.state" for frame in (0, 120, 240)] + \
                        [f"segment-0002-f{frame:09d}.state" for frame in (0, 120)], names
        swept = run([*tool, "sweep", run_dir, "--game", game, "--states", states, "--out", pieces,
                     "--", "--dump-state", "{out}.json"])
        assert "5 of 5 pieces replayed" in swept, swept
        for segment, last in ((first, 240), (second, 120)):
            state = json.loads((pieces / f"{segment.stem}-f{last:09d}.json").read_text(encoding="utf-8"))
            assert all(state[key] == ends[segment][key] for key in SAME), f"{segment.stem} in pieces ends elsewhere"
        middle = json.loads((pieces / "segment-0001-f000000120.json").read_text(encoding="utf-8"))
        assert middle["total_cycles"] < ends[first]["total_cycles"], "a piece ran past the next state"

        # A run a script played, a press every ten frames. What a script
        # presses is buttons like a player's: every press is in the record,
        # the record replays with no script, and it ends where the script on
        # its own ends.
        script, held = scripted_run()

        def on_its_own(name: str, *flags: str) -> dict:
            """Where the script takes the game with no run recorded or replayed."""
            folder = root / name
            folder.mkdir()
            run([game, "--headless", "--input", script, "--limit-frames", SCRIPTED_FRAMES,
                 "--save-dir", folder, "--dump-state", folder / "state.json", *flags])
            return json.loads((folder / "state.json").read_text(encoding="utf-8"))

        scripted_dir = root / "scripted"
        run([game, "--headless", "--record-run", scripted_dir, "--input", script,
             "--limit-frames", SCRIPTED_FRAMES])
        scripted = scripted_dir / "segment-0001.run"
        inputs = [line.split()[2:] for line in lines(scripted) if line.startswith("i ")]
        assert inputs == [words for buttons in held for words in ([buttons, "!"], ["-"])], inputs
        verified = run([*tool, "verify", scripted_dir, "--game", game,
                        "--states", root / "scripted-states", "--state-frames", "120"])
        assert f"{len(inputs)} of {len(inputs)} inputs" in verified and "the run replays" in verified, verified
        dump = root / "scripted.json"
        played = result(run([game, "--headless", "--replay-run", scripted, "--save-dir", saves,
                             "--dump-state", dump]))
        assert played["end"] == "matched" and played["late"] == "0", played
        state = json.loads(dump.read_text(encoding="utf-8"))
        alone = on_its_own("alone")
        assert all(state[key] == alone[key] for key in SAME), "the record of a scripted run is not what the script does"
        assert state["hram_ff80_fffe"][PRESSES - 0x80] == len(held), "a scripted press did not reach the game"

        # A machine with a part left out is another machine, and the game can
        # tell: with no sound hardware the volume it set reads back as $FF.
        # A run recorded on one would not replay on a whole one, or the other
        # way about, so neither is to be had.
        assert alone["hram_ff80_fffe"][SOUND - 0x80] == VOLUME, "the game does not read its volume back"
        silent = on_its_own("silent", "--no-audio")
        assert silent["hram_ff80_fffe"][SOUND - 0x80] == 0xFF, "the game cannot tell that --no-audio left the sound hardware out"
        for reduced in ("--no-audio", "--benchmark"):
            for flags in (["--record-run", root / "reduced", "--input", script],
                          ["--replay-run", scripted, "--save-dir", saves]):
                told = run([game, "--headless", reduced, *flags, "--limit-frames", 60], expect=1)
                assert f"cannot be recorded or replayed with {reduced}" in told, told
        assert not (root / "reduced").exists(), "a refused recording made its folder"
        assert no_save_written(), "a refused replay wrote a save"
    finally:
        if scratch:
            scratch.cleanup()
    print("recorded runs: recorded, replayed, told apart and swept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
