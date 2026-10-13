#!/usr/bin/env python3
"""A loop that waits for a line of the screen, recompiled and interpreted.

    ldh a, [rLY]
    cp l
    jr nz, .wait

is how a game waits for the line it changes the picture at (Pokémon Red's
title screen and credits scroll with it). Generated code once passed such a
loop in one step and left it where its sums said the line began, which was
one turn of the loop later than the instructions themselves leave it
whenever the line began between the start of a turn and the turn's read of
LY. The interpreter runs the instructions, so the two parted by 28 cycles,
and a run recorded one way left its record the other way.

The game here waits for two lines a frame with that loop, after a pause that
is another length every frame, so that the line begins at every place in the
loop's turn. Between the second wait and the end of the frame it counts in a
loop of its own, so that where a wait ended shows in where the frame ends.
Then:

- the loop has to be code the recompiler made, or nothing is tested;
- after every number of frames from 8 to 23 the recompiled game and the
  interpreted one (`--data-use`) have to be the same machine, to the cycle;
- a run recorded recompiled has to replay recompiled and interpreted, to
  every checkpoint and to its end.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile

from test_lift_evidence import build, same_machine
from test_lift_runtime_e2e import NINTENDO_LOGO, assemble, bump, ldh_a, ldh_to
from test_run_record import checkpoints, result, run

FRAMES_SEEN, WAITS, MIX, COUNT, SEEN = range(0x80, 0x85)
FIRST_LINE, SECOND_LINE = 40, 101
FRAME_COUNTS = range(8, 24)
RECORDED_FRAMES = 150


def program() -> list:
    return [
        ("label", "Start"),
        [0xF3, 0x31, 0xFF, 0xDF],                # di / ld sp,$DFFF
        [0xAF, 0x21, 0x80, 0xFF, 0x06, 0x10],    # xor a / ld hl,$FF80 / ld b,16
        ("label", "Clear"),
        [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli],a / dec b / jr nz
        [0x3E, 0x91], ldh_to(0x40),              # LCD on
        [0x3E, 0x01], ldh_to(0xFF),              # the VBlank interrupt
        [0xAF], ldh_to(0x0F),
        [0xFB],                                  # ei
        ("label", "Main"),
        # A pause of another length each frame: 16 cycles a turn, and the
        # wait's turn is 28, so seven frames in a row begin a wait at seven
        # places in its turn.
        ldh_a(FRAMES_SEEN), [0xE6, 0x07, 0x3C, 0x4F],   # and 7 / inc a / ld c,a
        ("label", "Pause"),
        [0x0D, 0x20], ("rel", "Pause"),          # dec c / jr nz
        [0x2E, FIRST_LINE, 0xCD], ("abs", "WaitLine"),   # ld l,n / call
        [0x2E, SECOND_LINE, 0xCD], ("abs", "WaitLine"),
        # Count until the frame is over: where the second wait ended is
        # where this loop is when the frame ends.
        ("label", "Spin"),
        bump(COUNT),
        ldh_a(SEEN), [0xA7, 0x28], ("rel", "Spin"),     # and a / jr z
        [0xAF], ldh_to(SEEN),
        [0x18], ("rel", "Main"),                 # jr
        ("label", "WaitLine"),
        ldh_a(0x44), [0xBD, 0x20], ("rel", "WaitLine"),  # ldh a,[rLY] / cp l / jr nz
        ldh_a(0x04), [0x47],                     # the divider as the wait ends / ld b,a
        ldh_a(MIX), [0x07, 0xA8], ldh_to(MIX),   # rlca / xor b
        bump(WAITS),
        [0xC9],                                  # ret
        ("label", "VBlank"),
        [0xF5], bump(FRAMES_SEEN),               # push af
        [0x3E, 0x01], ldh_to(SEEN),
        [0xF1, 0xD9],                            # pop af / reti
    ]


def make_rom() -> bytearray:
    code, labels = assemble(0x150, program())
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13B] = b"LY WAIT"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in (0x48, 0x50, 0x58, 0x60):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()

    scratch = tempfile.TemporaryDirectory(prefix="gblywait-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom_path = root / "lywait.gb"
        rom_path.write_bytes(make_rom())
        game = build(args.gbrecomp, rom_path, ["--no-scan", "--pointer-scan", "off"],
                     root / "game", args.cmake_arg)
        saves = root / "saves"
        saves.mkdir()

        def played(frames: int, name: str, *flags) -> dict:
            dump = root / f"{name}-{frames}.json"
            run([game, "--headless", "--limit-frames", frames, "--save-dir", saves,
                 "--dump-state", dump, *flags])
            return json.loads(dump.read_text(encoding="utf-8"))

        apart = []
        for frames in FRAME_COUNTS:
            recompiled = played(frames, "recompiled")
            interpreted = played(frames, "interpreted", "--data-use", root / "use.json")
            assert recompiled["dispatch_fallbacks"] == 0, "the game's code was not all recompiled"
            hram = recompiled["hram_ff80_fffe"]
            assert hram[WAITS - 0x80] >= 2 * (frames - 2), f"the game did not wait for its lines: {hram[:5]}"
            assert hram[COUNT - 0x80] != 0, "the game did not count after its waits"
            differ = same_machine(recompiled, interpreted)
            if differ:
                apart.append(f"after {frames} frames: " + ", ".join(
                    f"{key} {recompiled[key]} recompiled and {interpreted[key]} interpreted"
                    if not isinstance(recompiled[key], (list, dict)) else key for key in differ))
        assert not apart, "the recompiled game and the interpreted one are not the same machine\n" + "\n".join(apart)

        # A run recorded recompiled, replayed both ways.
        run_dir = root / "run"
        run([game, "--headless", "--record-run", run_dir, "--input", "c900000:A:300000,c5000000:S:200000",
             "--limit-frames", RECORDED_FRAMES])
        for name, flags in (("recompiled", []), ("interpreted", ["--data-use", root / "use.json"])):
            replayed = result(run([game, "--headless", "--replay-run", run_dir / "segment-0001.run",
                                   "--save-dir", saves, *flags]))
            assert replayed["end"] == "matched" and replayed["late"] == "0", (name, replayed)
            assert replayed["matched"] == replayed["checkpoints"] == str(checkpoints(RECORDED_FRAMES)), (name, replayed)
    finally:
        if scratch:
            scratch.cleanup()
    print("a wait for a line: the same recompiled and interpreted, and a run of it replays both ways")
    return 0


if __name__ == "__main__":
    sys.exit(main())
