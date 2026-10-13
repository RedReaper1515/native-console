#!/usr/bin/env python3
"""Evidence from running the game, end to end.

Makes a small ROM whose main loop reaches code three ways nothing in the ROM
spells out: through addresses it keeps in RAM, with `push hl / ret`, and by
pushing a return address by hand before a `jp hl`, once with the address in
the code and once with both it and the target out of a table. The two
functions behind the RAM addresses are never named by any instruction, so
the analysis does not find them, nor the two the table names. Then:

- the generated game, run with `--evidence FILE`, has to end in the same
  state as without it, and the file has to hold exactly those jumps, each
  with the CALL that entered the function it happened in, and no ordinary
  call, return or interrupt; recorded from a save state made with a call in
  progress, it has to hold the same: a function the state was made inside
  that pushes where to come back after that and jumps has made a call, known
  by its target returning there, code that is in no call and pushes a
  register for its target to pop has jumped, and a state made between the
  push and the jump leaves a jump and nothing else; and the game ends in the
  same state recorded or not, from a state too;
- a function that pushes where to come back and jumps has to stay one
  function in an export given that address as a place a run arrived at, by
  a recording started inside it that was made before the recorder could tell
  such a call, or by edges written from one;
- `gb_lift.py edges --evidence` has to turn them into edges, and the export
  made with them has to have the two functions, of the kind `seen`, with
  their caller, the call that reaches them, and which functions ran;
- a game generated with those edges has to compile the two functions, where
  the first one had to interpret them, and end in the same state;
- the function that calls through the table has to go on, in its lifted C,
  from wherever its callee was seen to come back: the checker has to find
  that C equivalent and catch an edit to it, and a game built with it has to
  end in the same state without one dispatch into the middle of it;
- evidence recorded from another ROM has to be refused.

That a lifted game asked for evidence records from its recompiled bodies is
checked in test_lift_runtime_e2e.py, which has a lifted game.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from test_lift_runtime_e2e import GB_LIFT, NINTENDO_LOGO, assemble, bump, ldh_a, ldh_to, run

FRAMES = 180
VBLANK_FLAG, LOOPS, COUNT_A, COUNT_B, PUSHED, HAND, BACK = range(0x80, 0x87)
TURN, AGAIN, FIRST, SECOND = range(0x87, 0x8B)
WAITED, WAIT_BACK = 0x8B, 0x8C
LATE, LATE_BACK, TOP = 0x8D, 0x8E, 0x8F
POINTERS = 0xC000

PROGRAM = [
    ("label", "Start"),
    [0xF3],                                  # di
    [0x31, 0xFF, 0xDF],                      # ld sp, $DFFF
    [0xCD], ("abs", "ByHandLateBack"),       # a function in its own right: see ByHandLate
    [0xAF], ldh_to(0x0F),                    # IF = 0
    [0x21, 0x80, 0xFF, 0x06, 0x10],          # ld hl, $FF80 / ld b, 16
    ("label", "Clear"),
    [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli], a / dec b / jr nz
    # Two addresses, a byte at a time so that no instruction holds either,
    # into RAM.
    [0x21, POINTERS & 0xFF, POINTERS >> 8],
    [0x3E], ("low", "HandlerA"), [0x22], [0x3E], ("high", "HandlerA"), [0x22],
    [0x3E], ("low", "HandlerB"), [0x22], [0x3E], ("high", "HandlerB"), [0x22],
    [0x3E, 0x01], ldh_to(0xFF),              # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    ("label", "CallDispatch"),
    [0xCD], ("abs", "Dispatch"),
    ("label", "CallPushRet"),
    [0xCD], ("abs", "PushRet"),
    ("label", "CallByHand"),
    [0xCD], ("abs", "ByHand"),
    ("label", "CallByTable"),
    [0xCD], ("abs", "ByTable"),
    ("label", "CallByHandWait"),
    [0xCD], ("abs", "ByHandWait"),
    ("label", "CallByHandLate"),
    [0xCD], ("abs", "ByHandLate"),
    bump(LOOPS),
    # This loop is in no call. It pushes a register for the code it jumps to
    # to pop, which is a jump and not a call made by hand, though something
    # was pushed.
    [0xF5],                                  # push af
    [0x21], ("abs", "TopTarget"),
    ("label", "TopJump"),
    [0xE9],                                  # jp hl
    ("label", "TopTarget"),
    [0xF1],                                  # pop af
    bump(TOP),
    [0xC3], ("abs", "MainLoop"),             # jp MainLoop

    ("label", "VBlank"),
    [0xF5, 0x3E, 0x01], ldh_to(VBLANK_FLAG), [0xF1, 0xD9],

    ("label", "WaitFrame"),
    [0xAF], ldh_to(VBLANK_FLAG),
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    ldh_a(VBLANK_FLAG), [0xA7, 0x28], ("rel", "WaitFrameLoop"),
    [0xC9],

    # Jumps to one of the two addresses in RAM, by turns.
    ("label", "Dispatch"),
    ldh_a(LOOPS), [0xE6, 0x01, 0x87],        # and 1 / add a
    [0x6F, 0x26, POINTERS >> 8],             # ld l, a / ld h, $C0
    [0x2A, 0x66, 0x6F],                      # ld a, [hli] / ld h, [hl] / ld l, a
    ("label", "DispatchJump"),
    [0xE9],                                  # jp hl
    ("label", "HandlerA"),
    bump(COUNT_A), [0xC9],
    ("label", "HandlerB"),
    bump(COUNT_B), [0xC9],

    # `push hl / ret` as a jump.
    ("label", "PushRet"),
    [0x21], ("abs", "PushRetTarget"), [0xE5],
    ("label", "PushRetJump"),
    [0xC9],
    ("label", "PushRetTarget"),
    bump(PUSHED), [0xC9],

    # A call made by hand: somewhere to come back to is pushed, then JP HL.
    ("label", "ByHand"),
    [0x21], ("abs", "HandTarget"),
    [0x11], ("abs", "ByHandBack"), [0xD5],   # ld de, .back / push de
    ("label", "ByHandJump"),
    [0xE9],
    ("label", "ByHandBack"),
    bump(BACK), [0xC9],
    ("label", "HandTarget"),
    bump(HAND), [0xC9],

    # The same, having waited for a frame first, as a helper that switches
    # bank does after calling something to look its target up. A run ends
    # in that wait, so a save state is made with this function entered and
    # its push not yet made.
    ("label", "ByHandWait"),
    [0xCD], ("abs", "WaitFrame"),
    [0x21], ("abs", "WaitTarget"),
    [0x01], ("abs", "ByHandWaitBack"), [0xC5],   # ld bc, .back / push bc
    ("label", "ByHandWaitJump"),
    [0xE9],
    ("label", "ByHandWaitBack"),
    bump(WAIT_BACK), [0xC9],
    ("label", "WaitTarget"),
    bump(WAITED),
    ("label", "WaitTargetReturn"),
    [0xC9],

    # And pushing first, then waiting: a save state made in that wait is
    # made between the push and the jump. The analysis does not look for an
    # address pushed that far before the jump, so where this one comes back
    # is something the code calls as well, once, before it counts anything.
    ("label", "ByHandLate"),
    [0x21], ("abs", "LateTarget"),
    [0x01], ("abs", "ByHandLateBack"), [0xC5],   # ld bc, .back / push bc
    [0xCD], ("abs", "WaitFrame"),
    ("label", "ByHandLateJump"),
    [0xE9],
    ("label", "ByHandLateBack"),
    bump(LATE_BACK), [0xC9],
    ("label", "LateTarget"),
    bump(LATE), [0xC9],

    # Four calls made by hand, to two functions by turns, each with its own
    # place to come back to; both addresses are read from a table. Neither
    # place is the target of a jump: the code runs into them from above.
    ("label", "ByTable"),
    [0xAF], ldh_to(TURN),
    ("label", "TableAgain"),                 # the first comes back here
    bump(AGAIN),
    ("label", "TableOn"),                    # and the second here
    ldh_a(TURN), [0xFE, 0x04, 0xD0],         # cp 4 / ret nc
    [0xE6, 0x01, 0x87, 0x87],                # and 1 / add a / add a
    [0x6F, 0x26, 0x00],                      # ld l, a / ld h, 0
    [0x11], ("abs", "Table"), [0x19],        # ld de, Table / add hl, de
    [0x2A, 0x5F, 0x2A, 0x57],                # ld a, [hli] / ld e, a / ld a, [hli] / ld d, a
    [0x2A, 0x66, 0x6F],                      # ld a, [hli] / ld h, [hl] / ld l, a
    [0xD5],                                  # push de
    ("label", "ByTableJump"),
    [0xE9],                                  # jp hl
    ("label", "TableFirst"),
    bump(TURN), bump(FIRST), [0xC9],
    ("label", "TableSecond"),
    bump(TURN), bump(SECOND), [0xC9],
    ("label", "Table"),
    ("abs", "TableAgain"), ("abs", "TableFirst"), ("abs", "TableOn"), ("abs", "TableSecond"),
]


def assemble_program() -> tuple[bytes, dict[str, int]]:
    """`assemble`, plus ("low", name) and ("high", name) for half an address."""
    labels: dict[str, int] = {}
    for _ in range(2):
        items = []
        for item in PROGRAM:
            if isinstance(item, tuple) and item[0] in ("low", "high"):
                address = labels.get(item[1], 0)
                items.append([address & 0xFF if item[0] == "low" else address >> 8])
            else:
                items.append(item)
        code, labels = assemble(0x150, items)
    return code, labels


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble_program()
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13E] = b"LIFT SEEN "
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


def function_id(address: int) -> str:
    return f"gbfn:v1:0000:{address:04x}"


def build(gbrecomp: Path, rom_path: Path, analysis: list[str], project: Path, cmake_args: list[str]) -> Path:
    run([str(gbrecomp), str(rom_path), *analysis, "-o", str(project), "--output-prefix", "seen_game"])
    run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(project / "build"),
         "-DCMAKE_BUILD_TYPE=Release", *cmake_args])
    run(["ninja", "-C", str(project / "build")], timeout=900)
    return next(path for path in (project / "build" / "seen_game.exe", project / "build" / "seen_game")
                if path.exists())


def play(executable: Path, root: Path, name: str, *flags: str, frames: int = FRAMES) -> tuple[dict, str]:
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    result = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(frames),
                  "--save-dir", str(saves), "--dump-state", str(dump), "--report-interpreter-hotspots",
                  *flags], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8")), result.stdout + result.stderr


def interpreted(log: str) -> int:
    """How many times the game had to interpret code it had not compiled."""
    found = re.search(r"\[INTERP\] Summary: fallbacks=(\d+)", log)
    return int(found.group(1)) if found else 0


def records(evidence: dict) -> list:
    """The transfers of a recording, to compare with what is expected."""
    return sorted((((t["kind"], t["at"], t["target"]), (t["calls"], t["returns_to"], t["from"]))
                   for t in evidence["transfers"]), key=repr)


def whole(by_id: dict, labels: dict[str, int]) -> None:
    """ByHandWait is one function: it goes on, in its own blocks, from the
    address it pushes, and nothing else starts there."""
    function = by_id[function_id(labels["ByHandWait"])]
    jump = next(block for block in function["blocks"] if block["instructions"][-1]["asm"] == "jp hl")
    assert jump.get("jump_returns_to") == [labels["ByHandWaitBack"]] and jump["jump_returns_certain"], jump
    assert labels["ByHandWaitBack"] in {block["address"] for block in function["blocks"]}, function["blocks"]
    assert function_id(labels["ByHandWaitBack"]) not in by_id, "the function was cut where its jump comes back"
    assert function["stack_balanced"] and function["returns"], function


def same_machine(a: dict, b: dict) -> list[str]:
    """Keys in which two state dumps differ, apart from how often the interpreter was used."""
    return [key for key in a if key != "dispatch_fallbacks" and a[key] != b.get(key)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()
    analysis = ["--no-scan", "--pointer-scan", "off"]

    scratch = tempfile.TemporaryDirectory(prefix="gblift-seen-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom, labels = make_rom()
        rom_path = root / "seen.gb"
        rom_path.write_bytes(rom)

        first_export = root / "export-0.json"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(first_export)])
        by_id = {function["id"]: function
                 for function in json.loads(first_export.read_text(encoding="utf-8"))["functions"]}
        for name in ("HandlerA", "HandlerB", "TableFirst", "TableSecond"):
            assert function_id(labels[name]) not in by_id, f"{name} was found without evidence"
        # Where the call ByHand makes comes back is in the code, so that is
        # known from the start, and it is ByHand's own. Where ByTable's come
        # back is not.
        by_hand = by_id[function_id(labels["ByHand"])]
        jump = next(block for block in by_hand["blocks"] if block["instructions"][-1]["asm"] == "jp hl")
        assert jump["jump_returns_to"] == [labels["ByHandBack"]] and jump["jump_returns_certain"], jump
        assert function_id(labels["ByHandBack"]) not in by_id
        whole(by_id, labels)
        assert not any("jump_returns_to" in block
                       for block in by_id[function_id(labels["ByTable"])]["blocks"])

        # Play it, recording.
        executable = build(args.gbrecomp, rom_path, analysis, root / "game-0", args.cmake_arg)
        plain, plain_log = play(executable, root, "plain")
        evidence_path = root / "evidence.json"
        recorded, log = play(executable, root, "recorded", "--evidence", str(evidence_path))
        assert "[EVIDENCE]" in log, log
        assert not same_machine(plain, recorded), same_machine(plain, recorded)
        hram = plain["hram_ff80_ff90"]
        assert hram[LOOPS - 0x80] > 40 and hram[COUNT_A - 0x80] > 20 and hram[COUNT_B - 0x80] > 20, hram
        assert hram[PUSHED - 0x80] == hram[HAND - 0x80] == hram[BACK - 0x80] > 40, hram
        assert hram[TURN - 0x80] == 4 and hram[FIRST - 0x80] == hram[SECOND - 0x80] > 80, hram
        assert hram[AGAIN - 0x80] == hram[FIRST - 0x80] * 3 // 2 % 256, hram
        assert hram[WAITED - 0x80] == hram[WAIT_BACK - 0x80] > 40, hram
        assert hram[LATE - 0x80] == hram[LATE_BACK - 0x80] == hram[TOP - 0x80] > 40, hram
        assert interpreted(plain_log) > 100, "the code only those jumps reach was compiled without being told"

        evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
        assert evidence["schema"] == "gbrt.evidence" and evidence["version"] == 1
        assert evidence["rom"]["size"] == len(rom) and evidence["dropped"] == 0, evidence["rom"]
        after_dispatch = labels["CallDispatch"] + 3
        after_push_ret = labels["CallPushRet"] + 3

        def from_call(site: str, entered: str) -> dict:
            return {"bank": 0, "site": labels[site], "entered_bank": 0, "entered": labels[entered]}

        expected = {
            ("jump", labels["DispatchJump"], labels["HandlerA"]):
                (False, after_dispatch, from_call("CallDispatch", "Dispatch")),
            ("jump", labels["DispatchJump"], labels["HandlerB"]):
                (False, after_dispatch, from_call("CallDispatch", "Dispatch")),
            ("return", labels["PushRetJump"], labels["PushRetTarget"]):
                (False, after_push_ret, from_call("CallPushRet", "PushRet")),
            ("jump", labels["ByHandJump"], labels["HandTarget"]):
                (True, labels["ByHandBack"], from_call("CallByHand", "ByHand")),
            ("jump", labels["ByTableJump"], labels["TableFirst"]):
                (True, labels["TableAgain"], from_call("CallByTable", "ByTable")),
            ("jump", labels["ByTableJump"], labels["TableSecond"]):
                (True, labels["TableOn"], from_call("CallByTable", "ByTable")),
            ("jump", labels["ByHandWaitJump"], labels["WaitTarget"]):
                (True, labels["ByHandWaitBack"], from_call("CallByHandWait", "ByHandWait")),
            ("jump", labels["ByHandLateJump"], labels["LateTarget"]):
                (True, labels["ByHandLateBack"], from_call("CallByHandLate", "ByHandLate")),
            # The loop is in no call: what it pushed is not looked at.
            ("jump", labels["TopJump"], labels["TopTarget"]): (False, 0, None),
        }
        assert records(evidence) == sorted(expected.items(), key=repr), records(evidence)
        for transfer in evidence["transfers"]:
            assert transfer["bank"] == 0 and transfer["target_bank"] == 0 and transfer["count"] > 20, transfer
        entered = {tuple(pair) for pair in evidence["entered"]}
        for name in ("Dispatch", "HandlerA", "HandlerB", "PushRet", "PushRetTarget", "ByHand", "HandTarget",
                     "WaitFrame", "ByTable", "TableFirst", "TableSecond", "ByHandWait", "WaitTarget",
                     "ByHandLate", "LateTarget", "TopTarget"):
            assert (0, labels[name]) in entered, name
        assert (0, 0x0100) in entered and (0, 0x0040) in entered, sorted(entered)
        for name in ("ByHandBack", "TableAgain", "TableOn", "ByHandWaitBack"):
            assert (0, labels[name]) not in entered, "a return is not an arrival"

        # Recordings that start from a save state. A state is made at the
        # end of a run, which is inside a wait for a frame: a call is in
        # progress that the recording never sees made. Its return is a
        # return, not a jump, and where it goes back to is not an arrival.
        #
        # The loop waits twice a turn, so of two runs one frame apart one
        # ends in each wait.
        states = {}
        for frames in (FRAMES, FRAMES + 1):
            state = root / f"state-{frames}.state"
            ended, _ = play(executable, root, f"state-{frames}", "--save-state-file", str(state), frames=frames)
            counted = ended["hram_ff80_ff90"]
            ahead = (counted[WAITED - 0x80] - counted[LATE - 0x80]) % 256
            assert ahead in (0, 1), counted
            states["late" if ahead else "wait"] = state
        assert set(states) == {"wait", "late"}, states

        def resumed_from(name: str) -> tuple[dict, set]:
            """A recording started from a state, which has to leave the game
            as it is, and what it holds apart from what is `expected`."""
            path = root / f"evidence-{name}.json"
            unrecorded, _ = play(executable, root, f"{name}-plain", "--load-state-file", str(states[name]))
            recorded, _ = play(executable, root, f"{name}-recorded", "--load-state-file", str(states[name]),
                               "--evidence", str(path))
            assert not same_machine(unrecorded, recorded), (name, same_machine(unrecorded, recorded))
            document = json.loads(path.read_text(encoding="utf-8"))
            assert document["dropped"] == 0
            more = [item for item in records(document) if item not in expected.items()]
            assert len(records(document)) == len(expected) + len(more), records(document)
            for (kind, at, target), _ in more:
                assert [t["count"] for t in document["transfers"]
                        if (t["kind"], t["at"], t["target"], t["from"]) == (kind, at, target, None)] == [1]
            return dict(more), {tuple(pair) for pair in document["entered"]} - entered

        # In the wait ByHandWait makes, ByHandWait is in progress too, and
        # nothing says how much was on the stack when it was entered. What
        # it pushes after that is below everything the state had, and its
        # target returns to it: the call it makes by hand, with no CALL to
        # put it on. No RET is taken for a jump, and where the target went
        # back to is not an arrival.
        #
        # The loop's own jump has something pushed below everything the
        # state had as well, once the calls from before the state are back.
        # Its target pops that and does not return to it: still the jump.
        more, arrived = resumed_from("wait")
        assert more == {("jump", labels["ByHandWaitJump"], labels["WaitTarget"]):
                        (True, labels["ByHandWaitBack"], None)}, more
        assert not arrived, sorted(arrived)

        # In the wait ByHandLate makes, the address was pushed before the
        # state was made, and that cannot be told: it is on the stack like
        # anything else the state had. The JP HL is a jump with nothing to
        # come back to. The target's RET goes for one of the returns the
        # recording did not see the call of.
        more, arrived = resumed_from("late")
        assert more == {("jump", labels["ByHandLateJump"], labels["LateTarget"]): (False, 0, None)}, more
        assert not arrived, sorted(arrived)
        resumed_paths = [root / "evidence-wait.json", root / "evidence-late.json"]

        # What a recorder that could not tell such a call wrote for the
        # first of those, and there are such files: the JP HL as a jump with
        # nothing to come back to, the target's RET, once, as a jump to an
        # address no CALL pushed, and where the RET went as arrived at.
        def unseen(kind: str, at: str, target: str) -> dict:
            return {"kind": kind, "bank": 0, "at": labels[at], "target_bank": 0, "target": labels[target],
                    "calls": False, "returns_to": 0, "from": None, "count": 1}

        older_path = root / "evidence-older.json"
        older_path.write_text(json.dumps(dict(
            evidence,
            transfers=evidence["transfers"] + [unseen("jump", "ByHandWaitJump", "WaitTarget"),
                                               unseen("return", "WaitTargetReturn", "ByHandWaitBack")],
            entered=evidence["entered"] + [[0, labels["ByHandWaitBack"]]])), encoding="utf-8")

        # Into edges, and into the export.
        edges_path = root / "edges.json"
        found = run([sys.executable, str(GB_LIFT), "edges", str(first_export), "--rom", str(rom_path),
                     "--evidence", str(evidence_path), "-o", str(edges_path),
                     "--work", str(root / "work-edges")])
        assert "the game was seen to make 9 jumps its code does not spell out" in found.stdout, found.stdout
        edges = json.loads(edges_path.read_text(encoding="utf-8"))
        seen = {(edge["site"], edge["kind"], edge["address"]) for edge in edges["edges"] if edge.get("seen")}
        assert {(labels["CallDispatch"], "call", labels["HandlerA"]),
                (labels["CallDispatch"], "call", labels["HandlerB"])} <= seen, sorted(seen)
        assert [0, labels["HandlerA"]] in edges["entered"]

        second_export = root / "export-1.json"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-edges", str(edges_path),
             "--lift-export", str(second_export)])
        export = json.loads(second_export.read_text(encoding="utf-8"))
        by_id = {function["id"]: function for function in export["functions"]}
        main_id = next(function["id"] for function in export["functions"]
                       if any(call["site"] == labels["CallDispatch"] for call in function["calls"]))
        for name in ("HandlerA", "HandlerB"):
            handler = by_id[function_id(labels[name])]
            assert handler["kind"] == "seen", (name, handler["kind"])
            assert "seen" in handler["entered_by"], handler["entered_by"]
            assert handler["callers"] == [main_id], handler["callers"]
            assert handler["ran"] is True
        call = next(call for call in by_id[main_id]["calls"] if call["site"] == labels["CallDispatch"])
        assert [entry["address"] for entry in call["reaches_seen"]] == \
            sorted([labels["HandlerA"], labels["HandlerB"]]), call
        assert by_id[function_id(labels["Dispatch"])]["ran"] is True
        again = run([sys.executable, str(GB_LIFT), "edges", str(second_export), "--rom", str(rom_path),
                     "--evidence", str(evidence_path), "-o", str(root / "again.json"),
                     "--work", str(root / "work-edges")])
        assert "this export has every one of them" in again.stdout, again.stdout
        dossier = run([sys.executable, str(GB_LIFT), "function", str(second_export),
                       main_id]).stdout
        assert "seen in a run going on to" in dossier, dossier
        structured = root / "structured"
        run([sys.executable, str(GB_LIFT), "baseline", str(second_export), "--all", "--style", "structured",
             "-o", str(structured)])
        text = "".join(path.read_text(encoding="utf-8") for path in structured.glob("*.c"))
        assert "seen going on to" in text, "the structured source does not say where the call was seen to go"

        # Where ByTable's calls come back, now that a run has shown it: two
        # places in the middle of what was one block, which is cut there.
        # They stay ByTable's own code, and its lifted C goes on from them,
        # having looked at the address on the stack.
        by_table = by_id[function_id(labels["ByTable"])]
        jump = next(block for block in by_table["blocks"] if block["instructions"][-1]["asm"] == "jp hl")
        back = [labels["TableAgain"], labels["TableOn"]]
        assert jump["jump_returns_to"] == back and jump["jump_returns_certain"] is False, jump
        assert set(back) <= {block["address"] for block in by_table["blocks"]}, by_table["blocks"]
        assert all(function_id(address) not in by_id for address in back)
        assert by_table["stack_balanced"], by_table
        for name in ("TableFirst", "TableSecond"):
            assert by_id[function_id(labels[name])]["kind"] == "seen", name
        assert "switch (lift_read16(ctx, ctx->sp)) {" in text, "the structured source does not make the call"
        dossier = run([sys.executable, str(GB_LIFT), "function", str(second_export),
                       function_id(labels["ByTable"])]).stdout
        assert "seen to come back there" in dossier, dossier

        # Where a CALL arrived is the start of a function, even where no edge
        # says who called. The real case is a call into a bank the code
        # around it does not show; this ROM has one bank to switch to, so
        # the record is given one more arrival by hand, in the middle of
        # WaitFrame.
        middle = labels["WaitFrameLoop"]
        assert function_id(middle) not in by_id
        more = dict(evidence, entered=evidence["entered"] + [[0, middle]])
        more_path = root / "evidence-more.json"
        more_path.write_text(json.dumps(more), encoding="utf-8")
        more_edges = root / "edges-more.json"
        found = run([sys.executable, str(GB_LIFT), "edges", str(second_export), "--rom", str(rom_path),
                     "--evidence", str(more_path), "-o", str(more_edges), "--work", str(root / "work-edges")])
        assert "1 place a call arrived at is not the start of a function" in found.stdout, found.stdout
        assert "export again" in found.stdout, found.stdout
        third_export = root / "export-2.json"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-edges", str(more_edges),
             "--lift-export", str(third_export)])
        arrived = {function["id"]: function
                   for function in json.loads(third_export.read_text(encoding="utf-8"))["functions"]}
        assert arrived[function_id(middle)]["kind"] == "seen", arrived[function_id(middle)]["kind"]
        assert "seen" in arrived[function_id(middle)]["entered_by"] and arrived[function_id(middle)]["ran"]
        found = run([sys.executable, str(GB_LIFT), "edges", str(third_export), "--rom", str(rom_path),
                     "--evidence", str(more_path), "-o", str(root / "again-more.json"),
                     "--work", str(root / "work-edges")])
        assert "this export has every one of them" in found.stdout, found.stdout

        def exported(edges: dict, name: str) -> dict:
            edges_file, export_file = root / f"edges-{name}.json", root / f"export-{name}.json"
            edges_file.write_text(json.dumps(edges), encoding="utf-8")
            run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-edges", str(edges_file),
                 "--lift-export", str(export_file)])
            return {function["id"]: function
                    for function in json.loads(export_file.read_text(encoding="utf-8"))["functions"]}

        # The recordings started from the two states: no RET that was a call
        # coming back, and nowhere new arrived at. Their two jumps have no
        # CALL to be put on and go on the jumps themselves, to functions the
        # export has, and ByHandWait stays whole.
        from_states = [part for path in resumed_paths for part in ("--evidence", str(path))]
        found = run([sys.executable, str(GB_LIFT), "edges", str(second_export), "--rom", str(rom_path),
                     "--evidence", str(evidence_path), *from_states,
                     "-o", str(root / "edges-states.json"), "--work", str(root / "work-edges")])
        assert "comes back" not in found.stdout, found.stdout
        assert "0 are inside another function, 0 were not decoded" in found.stdout, found.stdout
        from_both = json.loads((root / "edges-states.json").read_text(encoding="utf-8"))
        assert from_both["entered"] == edges["entered"]
        on_jumps = {(edge["site"], edge["address"], edge["returns_to"]) for edge in from_both["edges"]
                    if edge["kind"] == "computed" and edge["site"] != labels["TopJump"]}
        assert on_jumps == {(labels["ByHandWaitJump"], labels["WaitTarget"], labels["ByHandWaitBack"]),
                            (labels["ByHandLateJump"], labels["LateTarget"], 0)}, on_jumps
        whole(exported(from_both, "states"), labels)
        found = run([sys.executable, str(GB_LIFT), "edges", str(root / "export-states.json"), "--rom", str(rom_path),
                     "--evidence", str(evidence_path), *from_states,
                     "-o", str(root / "again-states.json"), "--work", str(root / "work-edges")])
        assert "this export has every one of them" in found.stdout, found.stdout

        # A place a run arrived at is not the start of a function where it
        # is where a function's own JP HL comes back: there the function
        # goes on. The older recording started inside ByHandWait has the
        # address ByHandWait pushes as arrived at, and a RET that went to
        # it. `edges` leaves the RET out, and the export keeps ByHandWait
        # whole.
        inside_edges = root / "edges-inside.json"
        found = run([sys.executable, str(GB_LIFT), "edges", str(second_export), "--rom", str(rom_path),
                     "--evidence", str(evidence_path), "--evidence", str(older_path),
                     "-o", str(inside_edges), "--work", str(root / "work-edges")])
        assert "1 were a RET to where a function's own JP HL comes back" in found.stdout, found.stdout
        inside = json.loads(inside_edges.read_text(encoding="utf-8"))
        assert [0, labels["ByHandWaitBack"]] in inside["entered"]
        assert not any(edge["address"] == labels["ByHandWaitBack"] for edge in inside["edges"]), inside["edges"]

        whole(exported(inside, "inside"), labels)
        # And that is every edge there is to find: nothing is left to ask for
        # at an address that will never be a function.
        found = run([sys.executable, str(GB_LIFT), "edges", str(root / "export-inside.json"), "--rom", str(rom_path),
                     "--evidence", str(evidence_path), "--evidence", str(older_path),
                     "-o", str(root / "again-inside.json"), "--work", str(root / "work-edges")])
        assert "this export has every one of them" in found.stdout, found.stdout

        # Edges written before that was so have the RET as a jump seen to go
        # there. The export knows it for what it is by the instruction it
        # left at. The same of a place a JP HL was only seen to come back
        # to, which a recording started inside ByTable would have: the RET
        # that ends TableFirst, going to TableAgain.
        def returned(at: int, to: int) -> dict:
            inside_of = next(function["id"] for function in by_id.values()
                             if any(instruction["address"] == at
                                    for block in function["blocks"] for instruction in block["instructions"]))
            return {"from": inside_of, "site": at, "kind": "computed", "bank": 0, "address": to,
                    "returns_to": 0, "left_at": at, "seen": True}

        first_ends = labels["TableSecond"] - 1
        assert rom[first_ends] == 0xC9
        older = exported(dict(inside,
                              edges=inside["edges"] + [returned(labels["WaitTargetReturn"], labels["ByHandWaitBack"]),
                                                       returned(first_ends, labels["TableAgain"])],
                              entered=inside["entered"] + [[0, labels["TableAgain"]]]), "older")
        whole(older, labels)
        by_table = older[function_id(labels["ByTable"])]
        jump = next(block for block in by_table["blocks"] if block["instructions"][-1]["asm"] == "jp hl")
        assert jump["jump_returns_to"] == back and jump["jump_returns_certain"] is False, jump
        assert all(function_id(address) not in older for address in back), "ByTable was cut where its jump comes back"
        lifted_whole = root / "lifted-whole"
        run([sys.executable, str(GB_LIFT), "baseline", str(root / "export-older.json"),
             "--function", f"00:{labels['ByHandWait']:04x}", "-o", str(lifted_whole)])
        text = (lifted_whole / "lifted_bank_00.c").read_text(encoding="utf-8")
        assert f"lift_call_pushed(ctx, ctx->hl, 0x{labels['ByHandWaitBack']:04X})" in text, text
        run([sys.executable, str(GB_LIFT), "check", str(root / "export-older.json"), "--rom", str(rom_path),
             "--function", f"00:{labels['ByHandWait']:04x}", "--source", str(lifted_whole),
             "--work", str(root / "work-whole"), "--json", str(root / "check-whole.json")])
        entry = json.loads((root / "check-whole.json").read_text(encoding="utf-8"))["results"][0]
        assert entry["verdict"] == "match", entry

        # A JP HL seen to go there is another matter: something jumps into
        # the function, and what it jumps to is a function, as anywhere.
        jumped = {"from": function_id(labels["Dispatch"]), "site": labels["DispatchJump"], "kind": "computed",
                  "bank": 0, "address": labels["ByHandWaitBack"], "returns_to": 0,
                  "left_at": labels["DispatchJump"], "seen": True}
        cut = exported(dict(inside, edges=inside["edges"] + [jumped]), "jumped")
        assert cut[function_id(labels["ByHandWaitBack"])]["kind"] == "seen", cut[function_id(labels["ByHandWaitBack"])]
        assert not any("jump_returns_to" in block
                       for block in cut[function_id(labels["ByHandWait"])]["blocks"])

        # The same game, told: the handlers are compiled, and nothing else changes.
        told = build(args.gbrecomp, rom_path, [*analysis, "--lift-edges", str(edges_path)], root / "game-1",
                     args.cmake_arg)
        with_edges, told_log = play(told, root, "told")
        assert not same_machine(plain, with_edges), same_machine(plain, with_edges)
        assert interpreted(told_log) == 0, told_log[-600:]

        # ByTable, lifted. The checker runs its C against the ROM from made-up
        # memory, where the turn counter is one that reaches the jump often
        # enough, and has to catch an edit to what follows the first place
        # its callee comes back to.
        lifted = root / "lifted"
        wanted = []
        for name in ("ByTable", "TableFirst", "TableSecond", "ByHand", "HandTarget"):
            wanted += ["--function", f"00:{labels[name]:04x}"]
        run([sys.executable, str(GB_LIFT), "baseline", str(second_export), *wanted, "-o", str(lifted)])
        source = lifted / "lifted_bank_00.c"
        text = source.read_text(encoding="utf-8")
        for address in back:
            assert f"case 0x{address:04X}: if (lift_call_pushed(ctx, ctx->hl, 0x{address:04X})) return;" in text
        check = [sys.executable, str(GB_LIFT), "check", str(second_export), "--rom", str(rom_path),
                 "--function", f"00:{labels['ByTable']:04x}", "--trials", "400"]
        run([*check, "--source", str(lifted), "--work", str(root / "work-check"),
             "--json", str(root / "check.json")])
        entry = json.loads((root / "check.json").read_text(encoding="utf-8"))["results"][0]
        assert entry["verdict"] == "match" and entry["coverage"] == 1.0, entry
        marker = f"/* {labels['TableAgain'] + 2:04X}: inc a */"
        counted = next(line for line in text.splitlines() if marker in line)
        wrong = root / "wrong"
        wrong.mkdir(exist_ok=True)
        (wrong / "lifted_bank_00.c").write_text(text.replace(counted, counted.replace("lift_inc8", "lift_dec8")),
                                                encoding="utf-8")
        caught = subprocess.run(
            [*check, "--source", str(wrong), "--work", str(root / "work-wrong"), "--json", str(root / "wrong.json")],
            capture_output=True, text=True, timeout=600)
        assert caught.returncode == 1, (caught.returncode, caught.stdout[-600:], caught.stderr[-600:])
        entry = json.loads((root / "wrong.json").read_text(encoding="utf-8"))["results"][0]
        assert entry["verdict"] == "mismatch", entry

        # And in the game: the same state at the end, and no dispatch into
        # the middle of a lifted function. Without the places to come back
        # to, every return of a callee would be one.
        package = root / "package"
        run([sys.executable, str(GB_LIFT), "package", str(second_export), "--source", str(lifted),
             "-o", str(package)])
        with_c = build(args.gbrecomp, rom_path,
                       [*analysis, "--lift-edges", str(edges_path), "--lift", str(package / "lift.json")],
                       root / "game-2", args.cmake_arg)
        trace = root / "trace.txt"
        lifted_state, lifted_log = play(with_c, root, "lifted", "--report-lift", "--trace-entries", str(trace))
        assert not same_machine(plain, lifted_state), same_machine(plain, lifted_state)
        counters = dict(re.findall(r"(\w+)=(\d+)", next(line for line in lifted_log.splitlines()
                                                       if "[LIFT]" in line)))
        assert int(counters["functions"]) == 5 and int(counters["calls"]) > 400, counters
        coverage_path = root / "coverage.json"
        run([sys.executable, str(GB_LIFT), "coverage", str(second_export), "--trace", str(trace),
             "--package", str(package / "lift.json"), "--json", str(coverage_path)])
        coverage = json.loads(coverage_path.read_text(encoding="utf-8"))
        assert coverage["inside_lifted"] == [] and coverage["lifted_starts"] > 400, coverage

        # Evidence from another ROM is refused.
        other = bytearray(rom)
        other[0x14E] ^= 0xFF
        other_path = root / "other.gb"
        other_path.write_bytes(other)
        refused = subprocess.run(
            [sys.executable, str(GB_LIFT), "edges", str(first_export), "--rom", str(other_path),
             "--evidence", str(evidence_path), "--work", str(root / "work-edges")],
            capture_output=True, text=True, timeout=300)
        assert refused.returncode != 0 and "another ROM" in refused.stderr, refused.stderr
    finally:
        if scratch:
            scratch.cleanup()

    print("lift evidence end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
