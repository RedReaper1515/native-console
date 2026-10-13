#!/usr/bin/env python3
"""Lifted source built into a generated game, end to end.

Makes a small ROM whose main loop calls subroutines that between them do
what makes lifted code hard to run inside the real machine: call other
subroutines (lifted or not), wait for the next frame with HALT, be
interrupted, return past their caller, jump with `push hl / ret`, and call
through a register by pushing a return address and jumping.

The ROM is lifted mechanically, packaged, and generated with `--lift`. The
one executable then runs twice, with the lifted code and with `--no-lift`,
and the machine has to end in the same state both ways, cycle for cycle.
Finally one lifted function is edited in the generated project and rebuilt:
the game has to change accordingly.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_LIFT = ROOT / "tools" / "gb_lift.py"
CMAKE_ARGS: list[str] = []
FRAMES = 240


def assemble(origin: int, items: list) -> tuple[bytes, dict[str, int]]:
    """Bytes, ("label", name), ("abs", name) and ("rel", name) items to code."""
    labels: dict[str, int] = {}
    for emit in (False, True):
        address = origin
        out = bytearray()
        for item in items:
            if isinstance(item, (bytes, bytearray, list)):
                data = bytes(item)
            elif item[0] == "label":
                labels[item[1]] = address
                continue
            elif item[0] == "abs":
                target = labels.get(item[1], 0)
                data = bytes([target & 0xFF, target >> 8])
            else:
                target = labels.get(item[1], address + 1)
                offset = target - (address + 1)
                if emit and not -128 <= offset <= 127:
                    raise AssertionError(f"relative jump to {item[1]} out of range")
                data = bytes([offset & 0xFF])
            out += data
            address += len(data)
    return bytes(out), labels


# Results live in HRAM so that `--dump-state` shows them (hram_ff80_ff90).
VBLANK_FLAG, LOOPS, INTERRUPTS, WAITS, SUM, MIX, ROTATED, HELPED, COPIED = range(0x80, 0x89)
WAIT_CALLS, WAIT_DONE, DOUBLE, PUSHED, TAILS, SHARED_HEAD, SHARED_TAIL = range(0x89, 0x90)
BACK, HAND = 0x90, 0x91  # past what `hram_ff80_ff90` shows: read from `hram_ff80_fffe`
SETUP = 0xC120  # two bytes the start-up writes and NeedsSetup waits for


def ldh_a(register: int) -> list[int]:
    return [0xF0, register]


def ldh_to(register: int) -> list[int]:
    return [0xE0, register]


def bump(register: int) -> list[int]:
    """ldh a,[r] / inc a / ldh [r],a"""
    return [0xF0, register, 0x3C, 0xE0, register]


PROGRAM = [
    ("label", "Start"),
    [0xF3],                                  # di
    [0x31, 0xFF, 0xDF],                      # ld sp, $DFFF
    [0xAF], ldh_to(0x0F),                    # IF = 0
    [0x21, 0x80, 0xFF, 0x06, 0x20],          # ld hl, $FF80 / ld b, 32
    ("label", "Clear"),
    [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli], a / dec b / jr nz
    [0x3E, 0xA5, 0xEA, SETUP & 0xFF, SETUP >> 8],              # two bytes only
    [0x3E, 0x5A, 0xEA, (SETUP + 1) & 0xFF, (SETUP + 1) >> 8],  # this start-up writes
    [0x3E, 0x01], ldh_to(0xFF),              # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    [0xCD], ("abs", "NeedsSetup"),
    [0xCD], ("abs", "Leaf"),
    [0xCD], ("abs", "Caller"),
    [0xCD], ("abs", "WaitFrame"),
    [0xCD], ("abs", "CallsWait"),
    [0xCD], ("abs", "DoubleReturn"),
    [0xCD], ("abs", "PushRet"),
    [0xCD], ("abs", "TailJump"),
    [0xCD], ("abs", "Shared"),
    [0xCD], ("abs", "IntoMiddle"),
    [0xCD], ("abs", "ByHand"),
    bump(LOOPS),
    [0x18], ("rel", "MainLoop"),

    ("label", "VBlank"),
    [0xF5],                                  # push af
    [0x3E, 0x01], ldh_to(VBLANK_FLAG),
    bump(INTERRUPTS),
    [0xF1, 0xD9],                            # pop af / reti

    # Arithmetic and a loop long enough to be interrupted now and then.
    ("label", "Leaf"),
    ldh_a(SUM), [0xC6, 0x03], ldh_to(SUM),   # SUM += 3
    [0x47], ldh_a(MIX), [0xA8], ldh_to(MIX), # MIX ^= SUM
    [0x06, 0x10],                            # ld b, 16
    ("label", "LeafLoop"),
    [0x07, 0x05, 0x20], ("rel", "LeafLoop"), # rlca / dec b / jr nz
    ldh_to(ROTATED),
    [0xC9],

    ("label", "Add7"),
    [0xC6, 0x07, 0xC9],

    # Calls a plain subroutine and a lifted one.
    ("label", "Caller"),
    ldh_a(ROTATED),
    [0xCD], ("abs", "Add7"),
    ldh_to(HELPED),
    [0xCD], ("abs", "Leaf"),
    ldh_a(SUM), ldh_to(COPIED),
    [0xC9],

    # Waits for the interrupt handler to say a frame has passed.
    ("label", "WaitFrame"),
    [0xAF], ldh_to(VBLANK_FLAG),
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    ldh_a(VBLANK_FLAG), [0xA7, 0x28], ("rel", "WaitFrameLoop"),
    bump(WAITS),
    [0xC9],

    # The same wait again, kept out of the lifted set by the test.
    ("label", "WaitPlain"),
    [0xAF], ldh_to(VBLANK_FLAG),
    ("label", "WaitPlainLoop"),
    [0x76],
    ldh_a(VBLANK_FLAG), [0xA7, 0x28], ("rel", "WaitPlainLoop"),
    [0xC9],

    # Frames end inside both of its callees.
    ("label", "CallsWait"),
    bump(WAIT_CALLS),
    [0xCD], ("abs", "WaitFrame"),
    [0xCD], ("abs", "WaitPlain"),
    bump(WAIT_DONE),
    [0xC9],

    # The callee drops a return address: control goes to the caller's caller.
    ("label", "DropFrame"),
    [0xE1],                                  # pop hl
    bump(DOUBLE),
    [0xC9],
    ("label", "DoubleReturn"),
    [0xCD], ("abs", "DropFrame"),
    [0x3E, 0xEE], ldh_to(DOUBLE),            # never reached
    [0xC9],

    # `push hl / ret` as a jump.
    ("label", "PushRet"),
    [0x21], ("abs", "PushRetTarget"),
    [0xE5, 0xC9],
    ("label", "PushRetTarget"),
    bump(PUSHED),
    [0xC9],

    ("label", "TailJump"),
    bump(TAILS),
    [0xC3], ("abs", "Leaf"),

    # A function entered in its middle, by a jump nothing spells out.
    ("label", "IntoMiddle"),
    [0x21], ("abs", "SharedTail"),
    [0xE5, 0xC9],
    ("label", "Shared"),
    bump(SHARED_HEAD),
    ("label", "SharedTail"),
    bump(SHARED_TAIL),
    [0xC9],

    # A call made by hand: the address to come back to is pushed, then
    # JP HL. The function goes on from there.
    ("label", "ByHand"),
    [0x21], ("abs", "HandTarget"),
    [0x11], ("abs", "ByHandBack"), [0xD5, 0xE9],  # ld de, .back / push de / jp hl
    ("label", "ByHandBack"),
    bump(BACK),
    [0xC9],
    ("label", "HandTarget"),
    bump(HAND),
    [0xC9],

    # Returns only once the start-up has run: it spins, with no HALT, until
    # both bytes are there. No memory made up from nothing gets through it.
    ("label", "NeedsSetup"),
    [0xFA, SETUP & 0xFF, SETUP >> 8, 0xFE, 0xA5, 0x20], ("rel", "NeedsSetup"),
    [0xFA, (SETUP + 1) & 0xFF, (SETUP + 1) >> 8, 0xFE, 0x5A, 0x20], ("rel", "NeedsSetup"),
    [0xC9],
]

LIFTED = ("Leaf", "Caller", "WaitFrame", "CallsWait", "DoubleReturn", "PushRet", "TailJump",
          "DropFrame", "IntoMiddle", "Shared", "NeedsSetup", "ByHand", "HandTarget")


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble(0x150, PROGRAM)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13E] = b"LIFT GAME "
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


def run(command: list[str], cwd: Path | None = None, timeout: int = 600) -> subprocess.CompletedProcess:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
    return result


def play(executable: Path, root: Path, name: str, *flags: str, frames: int = FRAMES) -> tuple[dict, str]:
    """Run the game headless for `frames` frames; the state dump and stderr."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    result = run([str(executable), "--headless", "--no-audio", "--limit-frames", str(frames),
                  "--save-dir", str(saves), "--dump-state", str(dump), *flags], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8")), result.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()
    CMAKE_ARGS.extend(args.cmake_arg)

    scratch = tempfile.TemporaryDirectory(prefix="gblift-game-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom, labels = make_rom()
        rom_path = root / "lift_game.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        analysis = ["--no-scan", "--pointer-scan", "off"]
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        export = json.loads(export_path.read_text(encoding="utf-8"))
        ids = {function["address"]: function["id"] for function in export["functions"]}
        for name in LIFTED:
            assert labels[name] in ids, f"the export has no function at {name}"
        # Where ByHand's callee returns is ByHand's own code, not a function.
        assert labels["ByHandBack"] not in ids, "the return point of a call made by hand is a function"

        # Lift every function the test names, plus the interrupt handler.
        lifted = root / "lifted"
        wanted = [f"00:{labels[name]:04x}" for name in LIFTED] + ["00:0040"]
        arguments = []
        for reference in wanted:
            arguments += ["--function", reference]
        run([sys.executable, str(GB_LIFT), "baseline", str(export_path), *arguments, "-o", str(lifted)])
        package = root / "package"
        run([sys.executable, str(GB_LIFT), "package", str(export_path), "--source", str(lifted),
             "-o", str(package)])

        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift", str(package / "lift.json"),
             "-o", str(project), "--output-prefix", "lift_game"])
        build = project / "build"
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(build),
             "-DCMAKE_BUILD_TYPE=Release", *CMAKE_ARGS])
        run(["ninja", "-C", str(build)], timeout=900)
        executable = next(path for path in (build / "lift_game.exe", build / "lift_game") if path.exists())

        original, _ = play(executable, root, "original", "--no-lift", "--report-lift")
        with_lift, log = play(executable, root, "lifted", "--report-lift")

        counters = dict(re.findall(r"(\w+)=(\d+)", next(line for line in log.splitlines() if "[LIFT]" in line)))
        assert int(counters["functions"]) == len(wanted), counters
        assert int(counters["calls"]) > 500, counters
        assert int(counters["guest_calls"]) > 500, counters
        assert int(counters["frame_waits"]) > FRAMES // 2, counters

        hram = original["hram_ff80_ff90"]
        assert hram[LOOPS - 0x80] > 20 and hram[WAITS - 0x80] > 20, hram
        assert hram[DOUBLE - 0x80] not in (0, 0xEE) and hram[PUSHED - 0x80] > 0, hram
        high = original["hram_ff80_fffe"]
        assert high[BACK - 0x80] == high[HAND - 0x80] > 20, high[:0x20]
        # A mechanical lift keeps ctx->pc where the original would be, so even
        # the address an interrupt pushes and the PC the run ends on agree.
        differing = sorted(key for key in original if original[key] != with_lift.get(key))
        assert not differing, (
            f"lifted and original runs differ in {differing}: "
            + ", ".join(f"{key}: {str(original[key])[:80]} vs {str(with_lift.get(key))[:80]}"
                        for key in differing[:4]))

        # Asked for evidence (LIFTING.md), the same game runs its recompiled
        # bodies: lifted source does not pass through the recorder. It ends
        # in the same state, and the `push hl / ret` is in the record.
        evidence_path = root / "evidence.json"
        recording, log = play(executable, root, "recording", "--report-lift", "--evidence", str(evidence_path))
        counters = dict(re.findall(r"(\w+)=(\d+)", next(line for line in log.splitlines() if "[LIFT]" in line)))
        assert int(counters["calls"]) == 0, counters
        assert not [key for key in original if original[key] != recording.get(key)]
        evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
        assert any(transfer["kind"] == "return" and transfer["target"] == labels["PushRetTarget"]
                   for transfer in evidence["transfers"]), evidence["transfers"]

        # What ran. `coverage` counts the dispatches the game logs by where
        # they landed. IntoMiddle jumps into the second half of Shared, which
        # is lifted: the C of Shared does not run for that, its recompiled
        # code does, and an edit to the C would not show there. That is the
        # middle count. The main loop is not lifted, and what `push hl / ret`
        # reaches in PushRet is code the export never decoded. ByHand's
        # callee returns into the middle of ByHand, and that is not in the
        # count: the lifted C made the call and went on itself.
        trace = root / "trace.txt"
        play(executable, root, "traced", "--trace-entries", str(trace))
        coverage_path = root / "coverage.json"
        printed = run([sys.executable, str(GB_LIFT), "coverage", str(export_path), "--trace", str(trace),
                       "--package", str(package / "lift.json"), "--json", str(coverage_path)]).stdout
        coverage = json.loads(coverage_path.read_text(encoding="utf-8"))
        shared_tail = hram[SHARED_TAIL - 0x80] - hram[SHARED_HEAD - 0x80]
        assert hram[SHARED_HEAD - 0x80] > 20 and shared_tail > 20, hram
        assert coverage["lifted_starts"] > 500, coverage
        assert coverage["inside_lifted"] == [
            {"function": ids[labels["Shared"]], "at": [{"address": labels["SharedTail"], "count": shared_tail}]},
        ], (coverage["inside_lifted"], printed)
        assert "gbfn:v1:0000:0100" in {entry["function"] for entry in coverage["not_lifted"]}, coverage
        assert coverage["in_no_function"] >= hram[PUSHED - 0x80] > 20, coverage
        assert "inside a lifted function, 1 of them" in printed, printed

        # The structured style with exact time is held to the same standard.
        # (Flags nothing reads are not computed in it, so F is left out.)
        structured = root / "structured"
        run([sys.executable, str(GB_LIFT), "baseline", str(export_path), *arguments,
             "--style", "structured", "--time", "exact", "-o", str(structured)])
        structured_package = root / "structured-package"
        run([sys.executable, str(GB_LIFT), "package", str(export_path), "--source", str(structured),
             "-o", str(structured_package)])
        structured_project = root / "game-structured"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift", str(structured_package / "lift.json"),
             "-o", str(structured_project), "--output-prefix", "lift_game"])
        structured_build = structured_project / "build"
        run(["cmake", "-G", "Ninja", "-S", str(structured_project), "-B", str(structured_build),
             "-DCMAKE_BUILD_TYPE=Release", *CMAKE_ARGS])
        run(["ninja", "-C", str(structured_build)], timeout=900)
        structured_exe = next(path for path in (structured_build / "lift_game.exe",
                                                structured_build / "lift_game") if path.exists())
        with_structured, _ = play(structured_exe, root, "structured")
        # The interrupt handler pushes AF, dead flags and all, and pops it
        # again: what differs is left below the stack pointer.
        assert with_structured["sp"] == original["sp"]
        top = original["sp"] - 0xD000
        reference = dict(original)
        for state in (reference, with_structured):
            state["wram_bank_1_d000_dfff"] = (state["wram_bank_1_d000_dfff"][:top - 32] +
                                              state["wram_bank_1_d000_dfff"][top:])
        differing = sorted(key for key in reference
                           if key != "f" and reference[key] != with_structured.get(key))
        assert not differing, f"structured lift and original runs differ in {differing}"

        # A save state made while lifted functions are half way through (the
        # run always stops inside one of the waits) holds a machine the
        # recompiled code can carry on from, and the lifted code as well.
        first, rest = 100, FRAMES - 100
        state_file = root / "half.state"
        play(executable, root, "half", "--save-state-file", str(state_file), frames=first)
        for name, flags in (("resumed-lifted", []), ("resumed-original", ["--no-lift"])):
            resumed, _ = play(executable, root, name, "--load-state-file", str(state_file), *flags,
                              frames=rest)
            differing = sorted(key for key in original if original[key] != resumed.get(key))
            assert not differing, f"{name}: a state saved inside lifted code resumed differently: {differing}"

        # The checker, from the game's own memory. NeedsSetup returns from no
        # state made up from nothing; from an image made of that save state
        # it does, and the report says that is where it passed.
        image = root / "half.img"
        run([sys.executable, str(GB_LIFT), "image", str(state_file), "-o", str(image)])
        check = [sys.executable, str(GB_LIFT), "check", str(export_path), "--rom", str(rom_path),
                 "--source", str(lifted), "--function", f"00:{labels['NeedsSetup']:04x}", "--trials", "100"]
        blind = subprocess.run([*check, "--work", str(root / "work-blind"), "--json", str(root / "blind.json")],
                               capture_output=True, text=True, timeout=600)
        assert blind.returncode == 3, (blind.returncode, blind.stdout[-600:], blind.stderr[-600:])
        assert json.loads((root / "blind.json").read_text(encoding="utf-8"))["results"][0]["matched"] == 0
        informed = run([*check, "--image", str(image), "--work", str(root / "work-image"),
                        "--json", str(root / "informed.json")])
        entry = json.loads((root / "informed.json").read_text(encoding="utf-8"))["results"][0]
        assert entry["verdict"] == "match" and entry["passed_from"] == ["half.img"], entry
        assert "only from half.img" in informed.stdout, informed.stdout

        # Mod the game: Leaf adds 5 where it added 3. Only that file rebuilds.
        # And what ByHand does once its callee is back counts down, not up:
        # that code is reached by a return to an address ByHand pushed.
        source = project / "lift" / "lifted_bank_00.c"
        text = source.read_text(encoding="utf-8")
        assert text.count("lift_add8(ctx, 0x03);") == 1
        text = text.replace("lift_add8(ctx, 0x03);", "lift_add8(ctx, 0x05);")
        marker = f"/* {labels['ByHandBack'] + 2:04X}: inc a */"
        counted = next(line for line in text.splitlines() if marker in line)
        assert "lift_inc8" in counted and f"lift_call_pushed(ctx, ctx->hl, 0x{labels['ByHandBack']:04X})" in text
        text = text.replace(counted, counted.replace("lift_inc8", "lift_dec8"))
        source.write_text(text, encoding="utf-8")
        run(["ninja", "-C", str(build)], timeout=900)
        modded, _ = play(executable, root, "modded")
        loops = modded["hram_ff80_ff90"][LOOPS - 0x80]
        calls = original["hram_ff80_ff90"][SUM - 0x80] * pow(3, -1, 256) % 256
        expected = calls * 5 % 256
        assert modded["hram_ff80_ff90"][SUM - 0x80] == expected, (modded["hram_ff80_ff90"], expected)
        went_back = modded["hram_ff80_fffe"]
        assert went_back[HAND - 0x80] == high[HAND - 0x80], went_back[:0x20]
        assert went_back[BACK - 0x80] == -high[BACK - 0x80] % 256, (went_back[:0x20], high[:0x20])
        assert loops == hram[LOOPS - 0x80], "the mod changed how many frames the game took"
        still, _ = play(executable, root, "modded-off", "--no-lift")
        assert still["hram_ff80_ff90"] == hram, "--no-lift has to run the original code"
    finally:
        if scratch:
            scratch.cleanup()

    print(f"lifted and original runs agree after {FRAMES} frames; an edited function changed the game")
    return 0


if __name__ == "__main__":
    sys.exit(main())
