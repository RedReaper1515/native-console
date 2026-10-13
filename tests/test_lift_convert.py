#!/usr/bin/env python3
"""Lifted functions written as C with arguments, end to end.

`gb_lift.py convert` takes what `signatures.txt` says each function of a
source folder takes, gives and keeps, and writes the function as C that has
those for its arguments and its result, with every call it makes written
the same way. This makes a small game whose functions each ask one thing of
it, holds what it writes to the ROM's code with the checker, and then plays
the game built from it against the game built from the lifted source:

- `Add` adds two registers: it comes out as `uint8_t Add(GBContext* ctx,
  uint8_t a, uint8_t b)`, and its binding hands the two in and the sum back;
- `Sum3` calls `Add` twice: both are C calls of it, with nothing under them;
- `Saver` pushes a pair, works with both halves and pops it: the pair is an
  argument, since it is read, and nothing is kept for it, since its caller's
  is its caller's;
- `Mover` pushes one pair and pops it into another: that is kept in a local;
- `Tail` ends by jumping into `Add`, which comes back: a call and a return;
- `TailMaybe` ends by jumping into a function that sometimes takes a return
  address off and goes back further: that stays a jump, and the game built
  with it returns past `Outer` as the original does;
- `Pushy` calls, with a pair pushed, something that may not leave the stack
  as it found it: it stays on the machine's stack;
- `CallsDrop` calls a function that drops its return address: the converted
  function is left at once, as the assembly is;
- `Through` jumps through a register: it is left as it is, and the report
  says why;
- `Linger` reads a byte the stack grows over and takes longer when it is not
  zero: from where the checker puts the stack, that byte is half of the pair
  the function has just pushed, which C that pushes nothing never writes
  there. The two end alike and take another time, and that is not the
  C's time gone wrong: such a state is counted as `not_compared`, where the
  lifted source, which pushes what the original pushes, has the same time
  in every state;
- with `Add`'s line changed by hand to give nothing, the C gives the sum
  all the same, because `Sum3` reads it: what C gives is what its callers
  may read, whatever a line was seen to bear out;
- a function the checker has not found to match is not converted, and
  nothing calls it with arguments;
- `--write` puts the converted functions into the folder, `build` checks
  them there and builds the game, and the game ends as the game built from
  the lifted source does: the same count of cycles, and the same sum of the
  timer as the frame interrupt found it, which comes out otherwise if the
  interrupt lands an instruction late at a call or a return.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_LIFT = ROOT / "tools" / "gb_lift.py"
FRAMES = 300
PRESET = "x64-release" if os.name == "nt" else "release"

# Results live in HRAM so that `--dump-state` shows them (hram_ff80_ff90).
(VBLANK_FLAG, LOOPS, R_ADD, R_SUM, R_SAVE, R_TAIL, R_DROP, R_THROUGH, WAITS, R_MOVE, R_PUSHY, R_OUTER, R_BUSY,
 R_TIMER, R_FALL, R_AFTER) = range(0x80, 0x90)


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


# Where a function's first push puts its low byte when the checker has made up the state it is
# entered in: the stack pointer is $DFF0 there unless the state says otherwise, with the caller's
# return address under it (make_state in lift/checker/lift_check.c).
STACK_BYTE = 0xDFEC


def bump(register: int) -> list[int]:
    """ldh a,[r] / inc a / ldh [r],a"""
    return [0xF0, register, 0x3C, 0xE0, register]


PROGRAM = [
    ("label", "Start"),
    [0xF3],                                  # di
    [0x31, 0xFF, 0xDF],                      # ld sp, $DFFF
    [0xAF, 0xE0, 0x0F],                      # IF = 0
    [0x21, 0x80, 0xFF, 0x06, 0x20],          # ld hl, $FF80 / ld b, 32
    ("label", "Clear"),
    [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli], a / dec b / jr nz
    [0x3E, 0x05, 0xE0, 0x07],                # TAC = 5: the timer counts, one every 16 cycles
    [0x3E, 0x01, 0xE0, 0xFF],                # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    [0x3E, 0x05, 0x06, 0x07],                # ld a, 5 / ld b, 7
    [0xCD], ("abs", "Add"), [0xE0, R_ADD],
    [0x3E, 0x01, 0x06, 0x02, 0x0E, 0x03],    # ld a, 1 / ld b, 2 / ld c, 3
    [0xCD], ("abs", "Sum3"), [0xE0, R_SUM],
    [0x01, 0x34, 0x12],                      # ld bc, $1234
    [0xCD], ("abs", "Saver"),
    [0x80, 0xE0, R_SAVE],                    # add a, b / ldh [R_SAVE], a: the sum, and b as it was
    [0x3E, 0x09, 0x06, 0x01],                # ld a, 9 / ld b, 1
    [0xCD], ("abs", "Tail"), [0xE0, R_TAIL],
    [0x01, 0x78, 0x56],                      # ld bc, $5678
    [0xCD], ("abs", "Mover"), [0xE0, R_MOVE],
    [0x01, 0x02, 0x01],                      # ld bc, $0102
    [0xA7],                                  # and a: no flag from before is read after this
    [0xCD], ("abs", "Pushy"),
    [0x80, 0xE0, R_PUSHY],                   # add a, b
    [0xA7],                                  # and a: no flag from before is read after this
    [0xCD], ("abs", "CallsDrop"), [0xE0, R_DROP],
    [0x3E, 0x01],                            # ld a, 1
    [0xCD], ("abs", "MaybeDrop"),            # called too, and so a function of its own: it comes back, this time
    [0x3E, 0xAA, 0xE0, R_OUTER],             # R_OUTER = $AA, until Outer has been
    [0xCD], ("abs", "Outer"), [0xE0, R_OUTER],
    [0x21], ("abs", "Target"),               # ld hl, Target
    [0xA7],                                  # and a
    [0xCD], ("abs", "Through"), [0xE0, R_THROUGH],
    [0x3E, 0x01, 0x06, 0x5A],                # ld a, 1 / ld b, $5A
    [0xCD], ("abs", "FallsInto"), [0xE0, R_FALL],
    [0x06, 0x21],                            # ld b, $21
    [0xCD], ("abs", "After"), [0xE0, R_AFTER],
    [0xCD], ("abs", "Linger"),
    # More than a frame of calls and returns, so that the frame interrupt
    # lands among them, somewhere else every time.
    [0x1E, 0x02],                            # ld e, 2
    ("label", "Busy"),
    [0x16, 0x00],                            # ld d, 0
    ("label", "BusyLoop"),
    [0x7A, 0x42, 0x4A],                      # ld a, d / ld b, d / ld c, d
    [0xCD], ("abs", "Sum3"), [0xE0, R_BUSY],
    [0x15, 0x20], ("rel", "BusyLoop"),       # dec d / jr nz
    [0x1D, 0x20], ("rel", "Busy"),           # dec e / jr nz
    [0xCD], ("abs", "WaitFrame"),
    bump(LOOPS),
    [0xC3], ("abs", "MainLoop"),

    # The frame interrupt adds the timer, as it finds it, to a sum: a handler
    # that runs a few cycles late finds another number there.
    ("label", "VBlank"),
    [0xF5, 0xE5],                            # push af / push hl
    [0xF0, 0x05, 0x21, R_TIMER, 0xFF, 0x86, 0x77],  # ldh a, [TIMA] / ld hl, R_TIMER / add a, [hl] / ld [hl], a
    [0x3E, 0x01, 0xE0, VBLANK_FLAG],
    [0xE1, 0xF1, 0xD9],                      # pop hl / pop af / reti

    ("label", "Add"),
    [0x80, 0xC9],                            # add a, b / ret

    ("label", "Sum3"),
    [0xCD], ("abs", "Add"),
    [0x41],                                  # ld b, c
    [0xCD], ("abs", "Add"),
    [0xC9],

    # The sum of the two halves, with both used for the work and put back.
    ("label", "Saver"),
    [0xC5, 0x78, 0x81, 0x47, 0x4F, 0xC1, 0xC9],  # push bc / ld a, b / add a, c / ld b, a / ld c, a / pop bc / ret

    # One pair into another by way of the stack.
    ("label", "Mover"),
    [0xC5, 0x06, 0x00, 0xD1, 0x7A, 0xC9],    # push bc / ld b, 0 / pop de / ld a, d / ret

    ("label", "Tail"),
    [0x3C, 0xC3], ("abs", "Add"),            # inc a / jp Add

    # Calls, with a pair pushed, something that does not always come back.
    ("label", "Pushy"),
    [0xC5],                                  # push bc
    [0xCD], ("abs", "CallsDrop"),
    [0xC1, 0xC9],                            # pop bc / ret

    # Drop takes its return address off and goes back to whoever called
    # CallsDrop: the `ld a, $20` is never reached.
    ("label", "CallsDrop"),
    [0x3E, 0x10],                            # ld a, $10
    [0xCD], ("abs", "Drop"),
    [0x3E, 0x20, 0xC9],                      # ld a, $20 / ret
    ("label", "Drop"),
    [0x3C, 0xE1, 0xC9],                      # inc a / pop hl / ret

    # MaybeDrop comes back when A is not zero, and goes back one further when
    # it is. TailMaybe jumps to it, and Outer calls that with $FF: the `ld a,
    # $77` is never reached, and Outer's caller gets 0.
    ("label", "Outer"),
    [0x3E, 0xFF],                            # ld a, $FF
    [0xCD], ("abs", "TailMaybe"),
    [0x3E, 0x77, 0xC9],                      # ld a, $77 / ret
    ("label", "TailMaybe"),
    [0x3C, 0xC3], ("abs", "MaybeDrop"),      # inc a / jp MaybeDrop
    ("label", "MaybeDrop"),
    [0xB7, 0xC0, 0xE1, 0xC9],                # or a / ret nz / pop hl / ret

    ("label", "Through"),
    [0xE9],                                  # jp hl
    ("label", "Target"),
    [0x3E, 0x33, 0xC9],                      # ld a, $33 / ret

    # A conditional return that is also the last instruction before the next
    # function: FallsInto leaves by it, or runs on into After, which reads B.
    ("label", "FallsInto"),
    [0xB7, 0xC8],                            # or a / ret z
    ("label", "After"),
    [0x78, 0xC9],                            # ld a, b / ret

    # A little longer when the byte at STACK_BYTE is not zero, and nothing else of it shows. The
    # game's stack never gets that far down. The checker's does: it enters a function with the
    # stack pointer two above that byte in most states, and there the pair pushed here lies on it.
    ("label", "Linger"),
    [0xE5],                                  # push hl
    [0x21, STACK_BYTE & 0xFF, STACK_BYTE >> 8, 0x7E],  # ld hl, STACK_BYTE / ld a, [hl]
    [0xE1],                                  # pop hl
    [0xA7, 0x28, 0x02, 0x00, 0x00],          # and a / jr z, +2 / nop / nop
    [0xAF, 0xC9],                            # xor a / ret

    ("label", "WaitFrame"),
    [0xAF, 0xE0, VBLANK_FLAG],
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    [0xF0, VBLANK_FLAG, 0xA7, 0x28], ("rel", "WaitFrameLoop"),
    bump(WAITS),
    [0xC9],
]
FUNCTIONS = ("Add", "Sum3", "Saver", "Mover", "Tail", "Pushy", "CallsDrop", "Drop", "Outer", "TailMaybe",
             "MaybeDrop", "Through", "Target", "FallsInto", "After", "Linger", "WaitFrame")


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble(0x150, PROGRAM)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13E] = b"LIFT CONVT"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


def run(command: list, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=900)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-4000:]}\n"
                             f"{result.stderr[-4000:]}")
    return result


def time_of(report: Path, name: str) -> dict:
    """What a report of the checker says of the function called `name`, binding or not."""
    for entry in json.loads(report.read_text(encoding="utf-8"))["results"]:
        if entry["c_name"] in (name, f"{name}_binding"):
            return entry
    raise AssertionError(f"{report} has no {name}")


def body_of(text: str, name: str) -> str:
    """The converted function called `name`, without its binding."""
    start = text.index(f" {name}(GBContext* ctx")
    return text[start:text.index("GBLIFT_FUNCTION(", start)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()
    cmake_args = [f"--cmake-arg={item}" for item in args.cmake_arg]

    scratch = tempfile.TemporaryDirectory(prefix="gblift-convert-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    for made in ("source", "converted", "one", "stack"):  # a kept folder from a run before: its lines are of that game
        shutil.rmtree(root / made, ignore_errors=True)
    try:
        rom, labels = make_rom()
        rom_path = root / "lift_convert.gb"
        rom_path.write_bytes(rom)
        at = {name: f"00:{address:04X}" for name, address in labels.items()}

        def lift(*arguments, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
            return run([sys.executable, GB_LIFT, *arguments], expect)

        def build(expect: tuple[int, ...] = (0,)) -> str:
            return lift("build", folder, "--gbrecomp", args.gbrecomp, "--trials", "200", *cmake_args,
                        expect=expect).stdout

        def play(name: str) -> dict:
            saves = root / f"saves-{name}"
            saves.mkdir(exist_ok=True)
            dump = root / f"{name}.json"
            executable = folder / "build" / PRESET / "game" / ("game.exe" if os.name == "nt" else "game")
            run([executable, "--headless", "--no-audio", "--limit-frames", str(FRAMES), "--save-dir", saves,
                 "--dump-state", dump])
            return json.loads(dump.read_text(encoding="utf-8"))

        plain = ["--no-scan", "--pointer-scan", "off"]
        export = root / "export.json"
        run([args.gbrecomp, rom_path, *plain, "--lift-export", export])
        symbols = root / "names.sym"
        symbols.write_text("".join(f"{at[name].lower()} {name}\n" for name in FUNCTIONS), encoding="utf-8")
        folder = root / "source"
        lift("source", export, "-o", folder, "--symbols", symbols, "--rom", rom_path, "--", *plain)
        build()
        # The lifted source pushes what the original pushes: where Linger reads its own pushed
        # pair, the two read the same byte, and every state is held to the original's time.
        timed = time_of(folder / ".lift" / "check.json", "Linger")
        assert timed["skipped"] == 0 and timed["cycles"]["equal"] == timed["matched"] > 100, timed
        lift("signatures", folder, "--trials", "200")
        build()
        lifted = play("lifted")
        results = lifted["hram_ff80_ff90"]
        assert [results[register - 0x80] for register in (R_ADD, R_SUM, R_TAIL, R_MOVE, R_PUSHY, R_DROP, R_OUTER,
                                                          R_THROUGH, R_FALL, R_AFTER)] == \
            [12, 6, 11, 0x56, 0x12, 0x11, 0x00, 0x33, 0x5A, 0x21], results
        assert results[LOOPS - 0x80] > 20 and results[R_TIMER - 0x80], results
        signed = {}
        for line in (folder / "signatures.txt").read_text(encoding="utf-8").splitlines():
            if line and not line.startswith("#"):
                signed[line.split(";")[1].strip()] = " ".join(line.split(";")[0].split()[1:])
        assert signed["Add"] == "takes a b gives a keeps bc de hl", signed
        assert "keeps bc" in signed["Saver"] and "takes bc" in signed["Saver"], signed
        assert "TailMaybe" in signed and "Outer" in signed, signed

        # ---- Converted, and held to the ROM's code.
        out = root / "converted"
        said = lift("convert", folder, "-o", out, "--trials", "200").stdout
        report = json.loads((out / "convert.json").read_text(encoding="utf-8"))
        text = (out / "converted_bank_00.c").read_text(encoding="utf-8")
        by_name = {name: f"gbfn:v1:0000:{labels[name]:04x}" for name in FUNCTIONS}
        for name in ("Add", "Sum3", "Saver", "Mover", "Tail", "Pushy", "CallsDrop", "Outer", "TailMaybe"):
            assert report["verdicts"].get(by_name[name]) == "match", (name, said)
        assert "0 differ" in said and report["summary"]["mismatch"] == 0, said
        # As C, Linger pushes nothing and reads what the made-up state had in that byte: the same
        # result after another time, in a state that says nothing of its time. Every other state
        # does, and there the time is the original's.
        assert report["shapes"].get(by_name["Linger"]) == "as C that C can call", report["shapes"]
        linger = body_of(text, "Linger")
        assert "push hl" in linger and "lift_push16" not in linger, linger
        timed = time_of(out / "check.json", "Linger")
        clock = timed["cycles"]
        assert timed["verdict"] == "match" and clock["different"] == 0, timed
        assert clock["not_compared"] > 10 and clock["equal"] > 100 and \
            clock["equal"] + clock["not_compared"] == timed["matched"], timed
        assert by_name["Through"] in report["left_out"]["it jumps through a register"], report["left_out"]
        shapes = {name: report["shapes"].get(by_name[name]) for name in FUNCTIONS}
        assert all(shapes[name] == "as C that C can call" for name in ("Add", "Sum3", "Saver", "Mover", "Tail")), shapes
        assert shapes["CallsDrop"] == shapes["Outer"] == shapes["TailMaybe"] == "as C, for the machine to call", shapes
        assert shapes["Pushy"] == "on the machine's stack" and "with something pushed" in \
            report["not_as_c"][by_name["Pushy"]], (shapes, report["not_as_c"])

        # What a function is passed is what its code may read, whatever its line was seen to bear
        # out. FallsInto runs on into After past a conditional return, and After reads B: asked
        # with lines that say FallsInto takes A and no more, the converter still finds B read.
        sys.path.insert(0, str(ROOT / "tools"))
        import gb_lift
        import gb_lift_codegen
        import gb_lift_convert
        exported = gb_lift.load_export(folder / ".lift" / "export.json")
        index = gb_lift.function_index(exported)
        a, b = gb_lift_codegen.BIT["a"], gb_lift_codegen.BIT["b"]
        narrow = {by_name["FallsInto"]: (a, a, 0), by_name["After"]: (b, a, 0)}
        for as_c in (True, False):
            asked = gb_lift_convert.Converter(index[by_name["FallsInto"]], index, {}, {}, narrow, as_c, set(),
                                              gb_lift_convert.well_nested(exported, index))
            assert asked.instructions[0].live_in & b, f"B is not found read (as C: {as_c})"
        falls = body_of(text, "FallsInto")
        assert re.search(r"r\.a = After\(ctx, r\.b\); return r\.a;", falls) and shapes["FallsInto"] == \
            "as C that C can call", falls

        add = body_of(text, "Add")
        assert text.count("uint8_t Add(GBContext* ctx, uint8_t a, uint8_t b) {") == 1 and \
            "LiftRegs r = {0};" in add and "lift_tick(ctx, 16); return r.a;" in add, add
        # The binding: it says the function keeps its own stack, hands the two in and the sum back,
        # puts back what the function keeps, and calls the flags nobody's.
        binding = text[text.index("GBLIFT_FUNCTION(0x00, 0x%04X, Add_binding)" % labels["Add"]):]
        binding = binding[:binding.index("\n}\n")]
        assert "lift_own_stack(ctx);" in binding and "ctx->a = Add(ctx, in.a, in.b);" in binding and \
            "ctx->bc = in.bc; ctx->de = in.de; ctx->hl = in.hl;" in binding and \
            "lift_undefined(ctx, 0x780u);" in binding and "lift_ret(ctx);" in binding, binding
        # From C to C: the time of the call passed where an interrupt can be taken, and nothing pushed.
        sum3 = body_of(text, "Sum3")
        assert len(re.findall(r"lift_tick\(ctx, 24\); r\.a = Add\(ctx, r\.a, r\.b\);", sum3)) == 2 and \
            "lift_call" not in sum3 and "_call(" not in sum3, sum3
        saver = body_of(text, "Saver")
        assert saver.startswith(" Saver(GBContext* ctx, uint16_t bc)") and "push" in saver and \
            "lift_push16" not in saver and "s0" not in saver, saver
        mover = body_of(text, "Mover")
        assert "uint16_t s0;" in mover and "s0 = r.bc;" in mover and "r.de = s0;" in mover and \
            "lift_push16" not in mover and "lift_pop16" not in mover, mover
        tail = body_of(text, "Tail")
        assert re.search(r"lift_tick\(ctx, 16\); r\.a = Add\(ctx, r\.a, r\.b\); return r\.a;", tail), tail
        # Into something that may go back further: a jump, with every register, and nothing after it.
        tail_maybe = body_of(text, "TailMaybe")
        assert "lift_jump_all(ctx, &r, 0x%04X" % labels["MaybeDrop"] in tail_maybe and "return" not in tail_maybe, \
            tail_maybe
        assert "lift_call_all(ctx, &r, 0x%04X" % labels["Drop"] in body_of(text, "CallsDrop")
        pushy = body_of(text, "Pushy")
        assert "lift_push16(ctx, r.bc);" in pushy and "liftr_from(&r, ctx);" in pushy and \
            "liftr_to(ctx, &r); return" in pushy and "CallsDrop_call(ctx, &r, " in pushy, pushy
        header = (out / "converted.h").read_text(encoding="utf-8")
        assert "liftr_add8(LiftRegs* ctx, uint8_t value)" in header and \
            "CallsDrop_call(GBContext* ctx, LiftRegs* r, uint16_t return_address" in header and \
            "TailMaybe_call(GBContext* ctx, LiftRegs* r, uint16_t return_address" in header, header[-4000:]
        assert not (folder / "src" / "converted").exists(), "written into the folder without --write"

        # ---- One function asked for, and every function kept on the machine's stack.
        said = lift("convert", folder, "-o", root / "one", "--function", at["Saver"], "--trials", "200").stdout
        assert "1 of 1 functions written" in said and "1 match" in said, said
        said = lift("convert", folder, "-o", root / "stack", "--machine-stack", "--trials", "200").stdout
        stacked = json.loads((root / "stack" / "convert.json").read_text(encoding="utf-8"))
        assert set(stacked["shapes"].values()) == {"on the machine's stack"} and \
            stacked["summary"]["mismatch"] == 0 and \
            "lift_own_stack" not in (root / "stack" / "converted_bank_00.c").read_text(encoding="utf-8"), said

        # ---- A callee whose line gives less than a caller reads: the C gives it all the same.
        path = folder / "signatures.txt"
        lines = path.read_text(encoding="utf-8")
        mine = next(line for line in lines.splitlines() if line.endswith("; Add"))
        path.write_text(lines.replace(mine, f"{at['Add']}  takes a b  gives -  keeps bc de hl  ; Add"),
                        encoding="utf-8")
        build()
        said = lift("convert", folder, "-o", out, "--trials", "200").stdout
        text = (out / "converted_bank_00.c").read_text(encoding="utf-8")
        report = json.loads((out / "convert.json").read_text(encoding="utf-8"))
        assert "uint8_t Add(GBContext* ctx, uint8_t a, uint8_t b) {" in text and \
            report["given"].get(by_name["Add"]) == "a", report["given"]
        sum3 = body_of(text, "Sum3")
        assert len(re.findall(r"r\.a = Add\(ctx, r\.a, r\.b\);", sum3)) == 2 and "lift_call_all" not in sum3, sum3
        assert report["verdicts"][by_name["Sum3"]] == "match" and report["summary"]["mismatch"] == 0, said

        # ---- A function that does not match is not converted, and not called with arguments.
        path.write_text(lines.replace(mine, f"{at['Add']}  takes a  gives a  keeps bc de hl  ; Add"),
                        encoding="utf-8")
        build(expect=(0, 1))
        said = lift("convert", folder, "-o", out, "--trials", "200").stdout
        report = json.loads((out / "convert.json").read_text(encoding="utf-8"))
        assert by_name["Add"] not in report["converted"] and \
            any(by_name["Add"] in ids for why, ids in report["left_out"].items() if "match" in why), report
        assert report["summary"]["mismatch"] == 0, said
        assert "Add(ctx" not in body_of((out / "converted_bank_00.c").read_text(encoding="utf-8"), "Sum3")

        # ---- Into the folder, built, and played against the game built from the lifted source.
        path.write_text(lines, encoding="utf-8")
        build()
        said = lift("convert", folder, "-o", out, "--trials", "200", "--write").stdout
        assert "put into" in said, said
        source = (folder / "src" / "lifted_bank_00.c").read_text(encoding="utf-8")
        assert "GBLIFT_FUNCTION(0x00, 0x%04X, Add_binding)" % labels["Add"] in source and \
            '#include "converted/converted.h"' in source and (folder / "src" / "converted" / "converted.h").is_file()
        said = build()
        assert ", 0 differ" in said, said
        timed = time_of(folder / ".lift" / "check.json", "Linger")
        assert timed["cycles"]["different"] == 0 and timed["cycles"]["not_compared"] > 10, timed
        converted = play("converted")
        assert converted["hram_ff80_ff90"] == lifted["hram_ff80_ff90"], (lifted["hram_ff80_ff90"],
                                                                         converted["hram_ff80_ff90"])
        assert converted["total_cycles"] == lifted["total_cycles"], (lifted["total_cycles"],
                                                                     converted["total_cycles"])
    finally:
        if scratch:
            scratch.cleanup()

    print("lifted functions were written as C with arguments, and the game built from them plays the same")
    return 0


if __name__ == "__main__":
    sys.exit(main())
