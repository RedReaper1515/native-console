#!/usr/bin/env python3
"""A generated game exploring itself, end to end.

Makes a small game with a room to cross: holding Right moves the view one
pixel every fourth frame, and at the far side, 200 pixels on, the A button
jumps through an address kept in RAM to a function no instruction names. The
analysis does not find that function, and buttons pressed at random from
power-on do not get there: a run is over long before a random walk has gone
that far. On the way no new code runs at all, so "found new code" gives a
search nothing to go by.

`gb_lift.py explore` has to get there all the same, by keeping the moments
that show a screen no run has ended on and going on from them, and has to:

- write one evidence file that holds the jump and where it arrived;
- leave no save beside the game's executable;
- go on from where it stopped when run again in the same folder;
- take a save state somebody hands it as a place to start, and a list of
  checkpoints, and keep the first moment past each;
- still press buttons at the right time from a state made late in a long
  session, when the game's 32-bit cycle count has wrapped;
- hold a byte of the game's memory at a value for every run when told to;
- give `edges --evidence` what makes the function a function in the export.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile

from test_lift_evidence import build
from test_lift_runtime_e2e import GB_LIFT, NINTENDO_LOGO, assemble, bump, ldh_a, ldh_to, run

VBLANK_FLAG, X, LOOPS, FOUND, TICK = range(0x80, 0x85)
POINTER = 0xC000
FAR_SIDE = 200


def program(secret: int) -> list:
    return [
        ("label", "Start"),
        [0xF3],                                  # di
        [0x31, 0xFF, 0xDF],                      # ld sp, $DFFF
        [0xAF], ldh_to(0x40), ldh_to(0x0F),      # LCD off, IF = 0
        [0x21, 0x80, 0xFF, 0x06, 0x10],          # ld hl, $FF80 / ld b, 16
        ("label", "Clear"),
        [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli], a / dec b / jr nz
        # The background: every column its own tile, so that each eight
        # pixels of the way across show something else.
        [0x21, 0x00, 0x98, 0x06, 0x20],          # ld hl, $9800 / ld b, 32
        ("label", "FillRow"),
        [0xAF],                                  # xor a
        ("label", "FillCell"),
        [0x22, 0x3C, 0xFE, 0x20, 0x20], ("rel", "FillCell"),  # ld [hli], a / inc a / cp 32 / jr nz
        [0x05, 0x20], ("rel", "FillRow"),        # dec b / jr nz
        # The address of the function at the far side, a byte at a time so
        # that no instruction holds it.
        [0x21, POINTER & 0xFF, POINTER >> 8],
        [0x3E, secret & 0xFF, 0x22, 0x3E, secret >> 8, 0x77],
        [0x3E, 0x91], ldh_to(0x40),              # LCD on, background on
        [0x3E, 0x01], ldh_to(0xFF),              # IE = VBlank
        [0xFB],                                  # ei
        ("label", "MainLoop"),
        [0xCD], ("abs", "WaitFrame"),
        [0xCD], ("abs", "ReadPad"),
        [0xCD], ("abs", "Move"),
        [0xCD], ("abs", "Act"),
        bump(LOOPS),
        [0x18], ("rel", "MainLoop"),

        ("label", "VBlank"),
        [0xF5, 0x3E, 0x01], ldh_to(VBLANK_FLAG), [0xF1, 0xD9],

        ("label", "WaitFrame"),
        [0xAF], ldh_to(VBLANK_FLAG),
        ("label", "WaitFrameLoop"),
        [0x76],                                  # halt
        ldh_a(VBLANK_FLAG), [0xA7, 0x28], ("rel", "WaitFrameLoop"),
        [0xC9],

        # b = directions held (bit 0 right, bit 1 left), c = buttons (bit 0 A).
        ("label", "ReadPad"),
        [0x3E, 0x20], ldh_to(0x00), ldh_a(0x00), ldh_a(0x00), [0x2F, 0xE6, 0x0F, 0x47],
        [0x3E, 0x10], ldh_to(0x00), ldh_a(0x00), ldh_a(0x00), [0x2F, 0xE6, 0x0F, 0x4F],
        [0x3E, 0x30], ldh_to(0x00),
        [0xC9],

        # One pixel every fourth frame, and the view follows.
        ("label", "Move"),
        ldh_a(TICK), [0x3C], ldh_to(TICK), [0xE6, 0x03, 0xC0],   # inc / and 3 / ret nz
        ldh_a(X),
        [0xCB, 0x40, 0x28], ("rel", "NoRight"),  # bit 0, b / jr z
        [0xFE, 0xF0, 0x30], ("rel", "NoRight"),  # cp 240 / jr nc
        [0x3C],                                  # inc a
        ("label", "NoRight"),
        [0xCB, 0x48, 0x28], ("rel", "NoLeft"),   # bit 1, b / jr z
        [0xA7, 0x28], ("rel", "NoLeft"),         # and a / jr z
        [0x3D],                                  # dec a
        ("label", "NoLeft"),
        ldh_to(X), ldh_to(0x43),                 # and SCX
        [0xC9],

        # At the far side, A goes where the address in RAM says.
        ("label", "Act"),
        ldh_a(X), [0xFE, FAR_SIDE, 0xD8],        # cp FAR_SIDE / ret c
        [0xCB, 0x41, 0xC8],                      # bit 0, c / ret z
        [0x21, POINTER & 0xFF, POINTER >> 8, 0x2A, 0x66, 0x6F],
        ("label", "ActJump"),
        [0xE9],                                  # jp hl
        ("label", "Secret"),
        bump(FOUND), [0xC9],
    ]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    _, labels = assemble(0x150, program(0))
    code, labels = assemble(0x150, program(labels["Secret"]))
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13E] = b"LIFT ROOM "
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]

    scratch = tempfile.TemporaryDirectory(prefix="gblift-explore-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom, labels = make_rom()
        rom_path = root / "room.gb"
        rom_path.write_bytes(rom)
        secret = [0, labels["Secret"]]
        export_path = root / "export-0.json"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        functions = {function["address"] for function in
                     json.loads(export_path.read_text(encoding="utf-8"))["functions"]}
        assert labels["Secret"] not in functions, "the function at the far side was found without being run"

        executable = build(args.gbrecomp, rom_path, analysis, root / "game", args.cmake_arg)
        explore = [sys.executable, str(GB_LIFT), "explore", str(executable), "--workers", "4"]

        # Looking only at code, nothing on the way across is worth keeping:
        # every run is from power-on, and none gets there.
        blind = root / "blind"
        run([*explore, "-o", str(blind), "--runs", "60", "--no-screens", "--no-memory"], timeout=600)
        evidence = json.loads((blind / "evidence.json").read_text(encoding="utf-8"))
        assert secret not in evidence["entered"], "buttons at random from power-on crossed the room"
        index = json.loads((blind / "explore.json").read_text(encoding="utf-8"))
        assert index["runs"] == 60 and len(index["moments"]) <= 3, index["moments"]

        # Keeping the screens, it crosses.
        found = root / "found"
        printed = run([*explore, "-o", str(found), "--runs", "700", "--export", str(export_path)],
                      timeout=900).stdout
        evidence = json.loads((found / "evidence.json").read_text(encoding="utf-8"))
        assert evidence["schema"] == "gbrt.evidence" and evidence["version"] == 1
        assert evidence["rom"]["size"] == len(rom) and evidence["frames"] > 100000, evidence["frames"]
        assert secret in evidence["entered"], (len(evidence["entered"]), printed[-600:])
        jumps = {(t["kind"], t["at"], t["target"]) for t in evidence["transfers"]}
        assert jumps == {("jump", labels["ActJump"], labels["Secret"])}, jumps
        assert "1 arrivals are not the start of a function in the export" in printed, printed[-600:]
        index = json.loads((found / "explore.json").read_text(encoding="utf-8"))
        assert index["schema"] == "gbrecomp.lift-explore" and index["runs"] == 700
        moments = index["moments"]
        assert len(moments) > 10 and max(moment["generation"] for moment in moments) >= 4, len(moments)
        for moment in moments:
            assert moment["state"] is None or (found / moment["state"]).is_file(), moment
        assert not (found / "runs").exists(), "the runs' own folders were left behind"
        # No copy of the game kept its save beside the executable.
        assert not list(executable.parent.glob("*.sav")), list(executable.parent.glob("*.sav"))

        # Run again, it goes on: the runs add up and nothing found is lost.
        run([*explore, "-o", str(found), "--runs", "24"], timeout=600)
        again = json.loads((found / "explore.json").read_text(encoding="utf-8"))
        assert again["runs"] == 724, again["runs"]
        more = json.loads((found / "evidence.json").read_text(encoding="utf-8"))
        assert secret in more["entered"] and more["frames"] > evidence["frames"]

        # A moment somebody saved is a place to start, and a list of
        # checkpoints says how far along a moment is. From the last moment
        # kept, which is past half way, the first checkpoint is passed at
        # once and the second, the far side's function having run, within a
        # few runs; the first moment past each is kept apart.
        last = max((moment for moment in moments if moment["state"]), key=lambda moment: moment["id"])
        goals = root / "goals.json"
        goals.write_text(json.dumps({"schema": "gbrecomp.lift-goals", "version": 1, "goals": [
            {"name": "Half way", "when": [{"address": f"{0xFF00 + X:04X}", "at_least": 100}]},
            {"name": "The far side's function", "when": [{"address": f"{0xFF00 + FOUND:04X}", "at_least": 1}]},
        ]}), encoding="utf-8")
        given = root / "given"
        printed = run([*explore, "-o", str(given), "--runs", "120", "--state", str(found / last["state"]),
                       "--goals", str(goals)], timeout=600).stdout
        index = json.loads((given / "explore.json").read_text(encoding="utf-8"))
        assert len(index["given"]) == 1 and any(moment["id"] == 2 for moment in index["moments"]), index["given"]
        assert index["passed"] == {"1": "checkpoints/001-half-way.state",
                                   "2": "checkpoints/002-the-far-side-s-function.state"}, (index["passed"], printed)
        for state in index["passed"].values():
            assert (given / state).is_file(), state
        assert "checkpoint 2 of 2 passed (The far side's function)" in printed, printed[-600:]

        # A state from late in a long session: the game's 32-bit cycle count
        # has wrapped by then (it does every seventeen minutes), and buttons
        # are timed by the count that does not. Started from such a state
        # only, the search still has to be able to walk: a third of the room
        # in a hundred and fifty runs.
        late = root / "late.state"
        saves = root / "saves-late"
        saves.mkdir(exist_ok=True)
        dump = root / "late.json"
        run([str(executable), "--headless", "--no-audio", "--limit-frames", "62000", "--save-dir", str(saves),
             "--save-state-file", str(late), "--dump-state", str(dump)], timeout=600)
        counts = json.loads(dump.read_text(encoding="utf-8"))
        assert counts["total_cycles"] > 1 << 32 > counts["cycles"], (counts["cycles"], counts["total_cycles"])
        walk = root / "walk.json"
        walk.write_text(json.dumps({"schema": "gbrecomp.lift-goals", "version": 1, "goals": [
            {"name": "A third of the way", "when": [{"address": f"{0xFF00 + X:04X}", "at_least": 66}]}]}),
            encoding="utf-8")
        printed = run([*explore, "-o", str(root / "from-late"), "--runs", "150", "--state", str(late),
                       "--states-only", "--goals", str(walk)], timeout=600).stdout
        assert "checkpoint 1 of 1 passed" in printed, printed[-600:]

        # A byte held at a value for every run: with the position held at
        # the far side, the same late state finds the far side's function in
        # a handful of runs, which without that it is nowhere near.
        far = root / "far.json"
        far.write_text(json.dumps({"schema": "gbrecomp.lift-goals", "version": 1, "goals": [
            {"name": "The far side's function", "when": [{"address": f"{0xFF00 + FOUND:04X}", "at_least": 1}]}]}),
            encoding="utf-8")
        printed = run([*explore, "-o", str(root / "held"), "--runs", "40", "--state", str(late), "--states-only",
                       "--goals", str(far), "--hold", f"{0xFF00 + X:04X}={FAR_SIDE:02X}"], timeout=600).stdout
        assert "checkpoint 1 of 1 passed" in printed, printed[-600:]

        # And the evidence is the kind a played session writes: the function
        # is a function in the next export.
        edges_path = root / "edges.json"
        run([sys.executable, str(GB_LIFT), "edges", str(export_path), "--rom", str(rom_path),
             "--evidence", str(found / "evidence.json"), "-o", str(edges_path),
             "--work", str(root / "work-edges")])
        second = root / "export-1.json"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-edges", str(edges_path),
             "--lift-export", str(second)])
        by_address = {function["address"]: function
                      for function in json.loads(second.read_text(encoding="utf-8"))["functions"]}
        assert by_address[labels["Secret"]]["kind"] == "seen", by_address.get(labels["Secret"])
    finally:
        if scratch:
            scratch.cleanup()

    print("a game exploring itself, end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
