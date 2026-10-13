#!/usr/bin/env python3
"""Lift export, mechanical lift and checker, end to end.

Builds a ROM that has one small function for every SM83 instruction, plus a
few of the things hand-written assembly does with the stack. Then:

- `gbrecomp --lift-export` has to describe those functions correctly;
- `gb_lift.py baseline` has to turn every one into C that the checker finds
  equivalent to the ROM, with the same cycle count, having reached every
  instruction;
- a wrong edit to that C has to be reported as a mismatch, also where the
  code edited is what a function goes on with after a call it made by
  pushing a return address and jumping;
- a hand-written, readable replacement has to be accepted.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_LIFT = ROOT / "tools" / "gb_lift.py"

UNDEFINED = {0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD}
CALLEE = 0x0F00          # inc c / ret
DOUBLE_RETURN = 0x0F10   # pop hl / ret: returns to its caller's caller
COMPARE = 0x0F20         # cp 5 / ret: only uses_carry calls it
HAND_CALLEE = 0x0F30     # inc c / ret: reached by `push de / jp hl` only
HAND_COMPARE = 0x0F38    # cp 5 / ret: the same
WAITING = 0xFFD6         # the byte wait_frame sets, for an interrupt handler to clear
FRAMES = 0xC410          # what main_loop counts
FIRST_FUNCTION = 0x1000
SPACING = 16
RET = 0xC9


def body_for(op: int, base: int) -> bytes | None:
    """A function that executes `op` once and returns."""
    if op in UNDEFINED or op == 0xCB:
        return None
    low, high = (base + 5) & 0xFF, (base + 5) >> 8
    special = {
        0x10: [0x10, 0x00, RET],
        0x18: [0x18, 0x02, 0x3E, 0x01, 0x06, 0x02, RET],
        0x31: [0x21, 0x00, 0x00, 0x39, 0x31, 0x34, 0xC2, 0xF9, RET],  # save SP in HL around it
        0x33: [0x33, 0x3B, RET],
        0x3B: [0x3B, 0x33, RET],
        0x08: [0x08, 0x00, 0xC4, RET],
        0xC3: [0xC3, low, high, 0x3E, 0x01, 0x06, 0x02, RET],
        0xC9: [RET],
        0xD9: [0xD9],
        0xCD: [0xCD, CALLEE & 0xFF, CALLEE >> 8, 0x06, 0x02, RET],
        0xE0: [0xE0, 0x90, RET],
        0xF0: [0xF0, 0x90, RET],
        0xE2: [0x0E, 0x90, 0xE2, RET],
        0xF2: [0x0E, 0x90, 0xF2, RET],
        0xE8: [0xE8, 0x05, 0xE8, 0xFB, RET],
        0xE9: [0x21, (base + 4) & 0xFF, (base + 4) >> 8, 0xE9, 0x1C, RET],
        0xEA: [0xEA, 0x00, 0xC4, RET],
        0xFA: [0xFA, 0x00, 0xC4, RET],
        0xF8: [0xF8, 0x7F, RET],
        0xF9: [0x21, 0x00, 0x00, 0x39, 0xF9, RET],
    }
    if op in special:
        return bytes(special[op])
    if op in (0x20, 0x28, 0x30, 0x38):
        return bytes([op, 0x02, 0x3E, 0x01, 0x06, 0x02, RET])
    if op in (0xC2, 0xCA, 0xD2, 0xDA):
        return bytes([op, low, high, 0x3E, 0x01, 0x06, 0x02, RET])
    if op in (0xC4, 0xCC, 0xD4, 0xDC):
        return bytes([op, CALLEE & 0xFF, CALLEE >> 8, 0x06, 0x02, RET])
    if op in (0xC0, 0xC8, 0xD0, 0xD8):
        return bytes([op, 0x3E, 0x01, RET])
    if op in (0xC5, 0xD5, 0xE5, 0xF5):
        return bytes([op, 0xC1, RET])            # push, then pop bc
    if op in (0xC1, 0xD1, 0xE1, 0xF1):
        return bytes([0xC5, op, RET])            # push bc, then pop
    if op < 0x40 and (op & 0xC7) == 0x06:
        return bytes([op, 0x5A, RET])            # ld r, n
    if op < 0x40 and (op & 0xCF) == 0x01:
        return bytes([op, 0x34, 0xC2, RET])      # ld rr, nn
    if op >= 0xC0 and (op & 0xC7) == 0xC6:
        return bytes([op, 0x5A, RET])            # alu a, n
    return bytes([op, RET])


def make_rom() -> tuple[bytearray, dict[str, int]]:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13F] = b"LIFT CHECK "
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector:vector + 2] = bytes([0x14, RET])      # inc d / ret
    for vector in range(0x40, 0x68, 8):
        rom[vector] = 0xD9                               # reti
    rom[CALLEE:CALLEE + 2] = bytes([0x0C, RET])
    rom[DOUBLE_RETURN:DOUBLE_RETURN + 2] = bytes([0xE1, RET])
    rom[COMPARE:COMPARE + 3] = bytes([0xFE, 0x05, RET])
    rom[HAND_CALLEE:HAND_CALLEE + 2] = bytes([0x0C, RET])
    rom[HAND_COMPARE:HAND_COMPARE + 3] = bytes([0xFE, 0x05, RET])

    functions: dict[str, int] = {}
    address = FIRST_FUNCTION

    def place(name: str, code: bytes) -> None:
        nonlocal address
        assert len(code) <= SPACING
        rom[address:address + len(code)] = code
        functions[name] = address
        address += SPACING

    for op in range(256):
        code = body_for(op, address)
        if code is not None:
            place(f"op_{op:02x}", code)
    for cb in range(256):
        place(f"cb_{cb:02x}", bytes([0xCB, cb, RET]))
    place("count_down", bytes([0x06, 0x05, 0x05, 0x20, 0xFD, RET]))   # ld b,5 / dec b / jr nz / ret
    # The same loop followed by XOR A: nothing reads the flags DEC leaves.
    place("count_clear", bytes([0x06, 0x05, 0x05, 0x20, 0xFD, 0xAF, RET]))
    place("double_return", bytes([0xCD, DOUBLE_RETURN & 0xFF, DOUBLE_RETURN >> 8, 0x3E, 0x01, RET]))
    # Tests of a flag, and where the flag came from.
    place("test_later", bytes([0xFE, 0x05, 0x47, 0x28, 0x01, 0x04, RET]))       # cp 5 / ld b,a / jr z / inc b
    place("return_if", bytes([0xFE, 0x05, 0xC8, 0x3C, RET]))                    # cp 5 / ret z / inc a
    place("carry_kept", bytes([0x90, 0x79, 0x38, 0x01, 0x1C, 0xAF, RET]))       # sub b / ld a,c / jr c / inc e / xor a
    place("bit_memory", bytes([0xCB, 0x7E, 0x28, 0x01, 0x1C, 0xAF, RET]))       # bit 7,[hl] / jr z / inc e / xor a
    place("flag_from_call", bytes([0xCD, CALLEE & 0xFF, CALLEE >> 8, 0xC8, 0x1C, RET]))
    place("uses_carry", bytes([0xCD, COMPARE & 0xFF, COMPARE >> 8, 0x38, 0x01, 0x1C, 0xAF, RET]))
    add_b = functions["op_80"]
    place("tail_known", bytes([0x05, 0xC3, add_b & 0xFF, add_b >> 8]))          # dec b / jp (add a,b / ret)
    here = address
    place("push_ret_jump", bytes([0x21, (here + 5) & 0xFF, (here + 5) >> 8, 0xE5, RET, 0x1C, RET]))
    # A call made by hand: the address to come back to is pushed, then JP HL.
    # ld hl,HAND_CALLEE / ld de,back / push de / jp hl / back: inc e / ret
    here = address
    place("call_by_hand", bytes([0x21, HAND_CALLEE & 0xFF, HAND_CALLEE >> 8, 0x11, (here + 8) & 0xFF,
                                 (here + 8) >> 8, 0xD5, 0xE9, 0x1C, RET]))
    # The same with a compare before the jump and a test where it comes
    # back: the flag tested there is the one the callee returned.
    # ld hl,HAND_COMPARE / ld de,back / cp 3 / push de / jp hl / back: jr z,+1 / inc e / ret
    here = address
    place("flag_from_hand_call", bytes([0x21, HAND_COMPARE & 0xFF, HAND_COMPARE >> 8, 0x11,
                                        (here + 10) & 0xFF, (here + 10) >> 8, 0xFE, 0x03, 0xD5, 0xE9,
                                        0x28, 0x01, 0x1C, RET]))
    # Waiting for an interrupt: set a byte and HALT until a handler has
    # cleared it. ld a,1 / ldh [WAITING],a / halt / ldh a,[WAITING] / and a / jr nz / ret
    place("wait_frame", bytes([0x3E, 0x01, 0xE0, WAITING & 0xFF, 0x76, 0xF0, WAITING & 0xFF, 0xA7, 0x20, 0xFA, RET]))
    wait = functions["wait_frame"]
    place("wait_then_count", bytes([0xCD, wait & 0xFF, wait >> 8, 0x1C, RET]))    # call wait_frame / inc e / ret
    # A main loop: it waits a frame, counts it, and never returns.
    # call wait_frame / ld a,[FRAMES] / inc a / ld [FRAMES],a / jr main_loop
    place("main_loop", bytes([0xCD, wait & 0xFF, wait >> 8, 0xFA, FRAMES & 0xFF, FRAMES >> 8, 0x3C,
                              0xEA, FRAMES & 0xFF, FRAMES >> 8, 0x18, 0xF4]))

    # A routine the function ends by jumping to: a stretch that does not
    # follow on from it, reached by JP only. cp 3 / jp z,Apart / inc e / ret,
    # and Apart twelve bytes on: inc c / ret.
    here = address
    place("jumps_apart", bytes([0xFE, 0x03, 0xCA, (here + 12) & 0xFF, (here + 12) >> 8, 0x1C, RET]))
    rom[here + 12:here + 14] = bytes([0x0C, RET])
    # The same stretch reached by JR stays part of the function...
    here = address
    place("jumps_near", bytes([0xFE, 0x03, 0x28, 0x08, 0x1C, RET]))
    rom[here + 12:here + 14] = bytes([0x0C, RET])
    # ...and so does one a JP reaches that follows on from the function.
    here = address
    place("jumps_on", bytes([0xFE, 0x03, 0xCA, (here + 7) & 0xFF, (here + 7) >> 8, 0x1C, RET, 0x0C, RET]))
    # Jumped to with something pushed, which the routine pops:
    # push bc / cp 3 / jp z,Apart / pop bc / ret, and Apart: pop bc / inc c / ret.
    here = address
    place("pushes_then_jumps", bytes([0xC5, 0xFE, 0x03, 0xCA, (here + 12) & 0xFF, (here + 12) >> 8, 0xC1, RET]))
    rom[here + 12:here + 15] = bytes([0xC1, 0x0C, RET])

    cursor = 0x150
    for target in functions.values():
        rom[cursor:cursor + 3] = bytes([0xCD, target & 0xFF, target >> 8])
        cursor += 3
    rom[cursor:cursor + 2] = bytes([0x18, 0xFE])
    assert cursor + 2 <= CALLEE and address <= 0x8000
    return rom, functions


def run(command: list[str], expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout}\n{result.stderr}")
    return result


def gb_lift(*arguments: str, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, str(GB_LIFT), *arguments], expect)


def function_id(address: int) -> str:
    return f"gbfn:v1:0000:{address:04x}"


def check_export(export: dict, functions: dict[str, int]) -> None:
    assert export["schema"] == "gbrecomp.lift-export" and export["version"] == 1
    assert export["rom"]["size"] == 0x8000 and len(export["rom"]["sha256"]) == 64
    by_id = {function["id"]: function for function in export["functions"]}
    missing = [name for name, address in functions.items() if function_id(address) not in by_id]
    assert not missing, f"functions the export does not have: {missing[:8]}"
    for name, address in functions.items():
        assert by_id[function_id(address)]["kind"] == "call", name

    add_b = by_id[function_id(functions["op_80"])]
    assert add_b["registers"]["reads"] == ["a", "b"], add_b["registers"]
    assert add_b["registers"]["writes"] == ["a"], add_b["registers"]
    assert add_b["flags"]["always_writes"] == ["z", "n", "h", "c"], add_b["flags"]
    assert add_b["blocks"][0]["instructions"][0]["asm"] == "add a, b"

    xor_a = by_id[function_id(functions["op_af"])]
    assert xor_a["registers"]["reads"] == [], "xor a does not depend on a"

    load = by_id[function_id(functions["op_fa"])]
    assert load["registers"]["reads"] == [] and load["registers"]["writes"] == ["a"]
    assert [(m["address"], m["access"], m["space"]) for m in load["memory"]] == [(0xC400, "r", "wram")]
    assert load["blocks"][0]["instructions"][0]["asm"] == "ld a, [$C400]"

    store = by_id[function_id(functions["op_70"])]
    assert store["registers"]["reads"] == ["b", "h", "l"], store["registers"]

    high = by_id[function_id(functions["op_f0"])]
    assert high["memory"][0]["address"] == 0xFF90 and high["memory"][0]["space"] == "hram"

    # A callee's effect reaches its caller: `call CALLEE` reads and writes c.
    caller = by_id[function_id(functions["op_cd"])]
    assert "c" in caller["registers"]["reads"] and "c" in caller["registers"]["writes"]
    assert caller["calls"][0]["function"] == function_id(CALLEE)
    assert function_id(functions["op_cd"]) in by_id[function_id(CALLEE)]["callers"]

    loop = by_id[function_id(functions["count_down"])]
    assert len(loop["blocks"]) >= 2 and any(block["successors"] for block in loop["blocks"])
    assert loop["registers"]["always_writes"] == ["b"], loop["registers"]

    # A return that does not go back to the caller: nothing can be said about
    # what follows it, and so about whoever calls such a function.
    for name in ("op_c5", "op_c1", "count_down", "tail_known"):
        assert by_id[function_id(functions[name])]["stack_balanced"], name
    popper = by_id[function_id(DOUBLE_RETURN)]
    assert not popper["stack_balanced"] and popper["summary_incomplete"], popper
    assert popper["registers"]["reads"] == [] and popper["flags"]["reads"] == []
    # Everything but hl, which its POP has overwritten by then.
    assert popper["registers"]["needs"] == ["a", "b", "c", "d", "e"], popper["registers"]
    assert popper["flags"]["needs"] == ["z", "n", "h", "c"], popper["flags"]
    assert by_id[function_id(functions["double_return"])]["summary_incomplete"]
    assert not by_id[function_id(functions["push_ret_jump"])]["stack_balanced"]
    # Where the summary is complete, what a function needs is what it reads.
    assert add_b["registers"]["needs"] == ["a", "b"] and add_b["flags"]["needs"] == []

    # `push de / jp hl` with a constant in de is a call: the function goes
    # on where the code it jumps to returns, and that is not a function of
    # its own. Its RET finds the stack as its CALL left it.
    for name, back in (("call_by_hand", 8), ("flag_from_hand_call", 10)):
        by_hand = by_id[function_id(functions[name])]
        jump = next(block for block in by_hand["blocks"] if block["instructions"][-1]["asm"] == "jp hl")
        assert jump["jump_returns_to"] == [functions[name] + back], (name, jump)
        assert jump["jump_returns_certain"] is True and functions[name] + back in jump["successors"], jump
        assert function_id(functions[name] + back) not in by_id, name
        assert by_hand["stack_balanced"] and by_hand["returns"], (name, by_hand["returns"])
    assert not any("jump_returns_to" in block
                   for block in by_id[function_id(functions["op_e9"])]["blocks"]), "a plain jp hl"

    # A stretch apart from its function that the function reaches only by JP
    # is a function of its own, entered by that jump with whatever was pushed.
    for name in ("jumps_apart", "pushes_then_jumps"):
        host = by_id[function_id(functions[name])]
        apart = functions[name] + 12
        tail = by_id[function_id(apart)]
        assert tail["kind"] == "detached_tail" and tail["callers"] == [host["id"]], (name, tail["kind"])
        assert "detached_tail" not in tail["entered_by"], tail["entered_by"]
        assert apart not in [block["address"] for block in host["blocks"]], name
        assert [(jump["target"], jump["function"], jump["kind"]) for jump in host["jumps_out"]] == \
            [(apart, tail["id"], "jump")], host["jumps_out"]
        assert host["stack_balanced"] and tail["stack_balanced"], name
    # Reached by JR, or following on from the function, it is still part of it.
    for name, offset in (("jumps_near", 12), ("jumps_on", 7)):
        host = by_id[function_id(functions[name])]
        assert function_id(functions[name] + offset) not in by_id, name
        assert functions[name] + offset in [block["address"] for block in host["blocks"]], name

    # COMPARE has one caller, which tests carry after it and then clears
    # every flag: carry is all of them a caller reads.
    compare = by_id[function_id(COMPARE)]
    assert compare["kind"] == "call" and compare["entered_by"] == [], compare["entered_by"]
    assert compare["flags"]["read_after"] == ["c"], compare["flags"]
    # The entry point and the reset vectors are reached without a call.
    assert by_id["gbfn:v1:0000:0100"]["entered_by"] == ["entry"]
    assert by_id["gbfn:v1:0000:0100"]["flags"]["read_after"] == ["z", "n", "h", "c"]

    assert "halt" in by_id[function_id(functions["op_76"])]["hazards"]
    assert "computed_jump" in by_id[function_id(functions["op_e9"])]["hazards"]
    assert "sp_manipulation" in by_id[function_id(functions["op_31"])]["hazards"]

    header = [region for region in export["data_regions"] if region["guess"] == "header"]
    assert header and header[0]["address"] == 0x0104, export["data_regions"][:4]
    named = {entry["address"]: entry for entry in export["memory"]}
    assert function_id(functions["op_fa"]) in named[0xC400]["readers"]
    assert function_id(functions["op_ea"]) in named[0xC400]["writers"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="gblift-") as scratch:
        root = Path(scratch)
        rom, functions = make_rom()
        rom_path = root / "lift_check.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        run([str(args.gbrecomp), str(rom_path), "--no-scan", "--pointer-scan", "off",
             "--lift-export", str(export_path)])
        export = json.loads(export_path.read_text(encoding="utf-8"))
        check_export(export, functions)

        assert "add a, b" in gb_lift("function", str(export_path), f"00:{functions['op_80']:04x}").stdout
        assert f"a call: what it jumps to returns to .L{functions['call_by_hand'] + 8:04X}" in             gb_lift("function", str(export_path), f"00:{functions['call_by_hand']:04x}").stdout
        assert "functions" in gb_lift("summary", str(export_path)).stdout

        # Every function, lifted mechanically, has to match the ROM.
        lifted = root / "lifted"
        gb_lift("baseline", str(export_path), "--all", "-o", str(lifted))
        report_path = root / "report.json"
        gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(lifted),
                "--trials", "300", "--work", str(root / "work"), "--json", str(report_path))
        report = json.loads(report_path.read_text(encoding="utf-8"))
        results = {entry["function"]: entry for entry in report["results"]}
        assert report["summary"]["mismatch"] == 0, report["summary"]
        problems = []
        for name, address in functions.items():
            entry = results[function_id(address)]
            # The callee of double_return returns past it: only its CALL runs.
            reached = entry["instructions"] if name != "double_return" else 1
            if (entry["verdict"] != "match" or entry["instructions_executed"] != reached or
                    entry["cycles"]["different"] or entry["cycles"]["not_counted"]):
                problems.append((name, entry["verdict"], entry["coverage"], entry["cycles"],
                                 entry["skipped_because"]))
        assert not problems, f"{len(problems)} functions did not match fully: {problems[:6]}"
        # No handler runs in the checker, so a wait for one ends only because
        # a HALT changes the byte the code keeps looking at: every state
        # these two got through, they got through that way.
        for name in ("wait_frame", "wait_then_count"):
            entry = results[function_id(functions[name])]
            assert entry["matched"] >= 100 and entry["waited"] == entry["matched"], (name, entry)
            assert entry["stopped_waiting"] == 0, (name, entry)
        assert results[function_id(functions["op_76"])]["waited"] == 0, "a lone HALT waits for nothing"
        # The loop that never returns is compared at a wait, once most of a
        # state's budget is gone, and a few dozen such states are enough.
        entry = results[function_id(functions["main_loop"])]
        assert 0 < entry["matched"] == entry["stopped_waiting"] == entry["trials"] < 100, entry

        # The structured style has to match too, with the time kept either way.
        for time in ("block", "exact"):
            structured = root / f"structured-{time}"
            gb_lift("baseline", str(export_path), "--all", "--style", "structured", "--time", time,
                    "-o", str(structured))
            assert (structured / "lifted_names.h").is_file()
            structured_report = root / f"structured-{time}.json"
            gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(structured),
                    "--trials", "300", "--work", str(root / f"work-structured-{time}"),
                    "--json", str(structured_report))
            report = json.loads(structured_report.read_text(encoding="utf-8"))
            assert report["summary"]["mismatch"] == 0, (time, report["summary"])
            results = {entry["function"]: entry for entry in report["results"]}
            problems = [(name, results[function_id(address)]["verdict"], results[function_id(address)]["cycles"])
                        for name, address in functions.items()
                        if results[function_id(address)]["verdict"] != "match"
                        or results[function_id(address)]["cycles"]["different"]
                        or results[function_id(address)]["cycles"]["not_counted"]]
            assert not problems, f"structured ({time}): {len(problems)} functions did not match: {problems[:6]}"
        text = (root / "structured-block" / "lifted_bank_00.c").read_text(encoding="utf-8")

        def body_of(name: str) -> str:
            start = text.index(f"0x{functions[name]:04X}, ")
            return text[start:text.index("\n}\n", start)]

        # A loop becomes a loop, and its test reads what DEC left in b. The
        # flags a RET hands back are still computed; where nothing reads
        # them, they are not.
        assert "do {" in body_of("count_down") and "goto" not in body_of("count_down")
        assert "ctx->b != 0" in body_of("count_down"), body_of("count_down")
        assert "lift_dec8" in body_of("count_down"), body_of("count_down")
        assert "ctx->b != 0" in body_of("count_clear"), body_of("count_clear")
        assert "lift_dec8" not in body_of("count_clear"), body_of("count_clear")

        # A test is written from the instruction that set the flag: when
        # that is not the one before it, and when it is a return.
        assert "ctx->a == 0x05" in body_of("test_later"), body_of("test_later")
        assert "if (ctx->a == 0x05) {" in body_of("return_if"), body_of("return_if")
        for name in ("count_down", "count_clear", "test_later", "return_if"):
            assert "ctx->f_" not in body_of(name), body_of(name)
        # Where the operands are gone by the time of the test, or were in
        # memory, the answer is kept in a local and the flags are not.
        assert "bool carry;" in body_of("carry_kept"), body_of("carry_kept")
        assert "carry = ctx->a < ctx->b;" in body_of("carry_kept"), body_of("carry_kept")
        assert "lift_sub8" not in body_of("carry_kept"), body_of("carry_kept")
        assert "zero = !(lift_read8(ctx, ctx->hl) & 0x80);" in body_of("bit_memory"), body_of("bit_memory")
        assert "lift_bit" not in body_of("bit_memory"), body_of("bit_memory")
        # A flag a callee returned is the flag.
        assert "if (ctx->f_z) {" in body_of("flag_from_call"), body_of("flag_from_call")
        assert "ctx->f_c" in body_of("uses_carry"), body_of("uses_carry")
        # A call made by hand is a call, and the function goes on after it.
        # A flag tested there is the callee's, not what was compared before.
        for name in ("call_by_hand", "flag_from_hand_call"):
            assert "lift_call_pushed(ctx, ctx->hl" in body_of(name), body_of(name)
            assert "lift_jump" not in body_of(name) and "goto" not in body_of(name), body_of(name)
        assert "ctx->f_z" in body_of("flag_from_hand_call"), body_of("flag_from_hand_call")
        assert "0x03" not in body_of("flag_from_hand_call").split("lift_call_pushed")[1]
        # Leaving into a function that overwrites the flags: none are owed.
        assert "ctx->b--;" in body_of("tail_known"), body_of("tail_known")
        assert "the callers found read flag c */" in text, "COMPARE's note"

        # Scoring against a disassembly: a symbol file naming three of the
        # functions, and source saying two start code and one starts data.
        sym = root / "fixture.sym"
        sym.write_text(f"00:{functions['op_80']:04x} AddB\n00:{functions['op_af']:04x} ClearA\n"
                       f"00:{functions['count_down']:04x} CountDown\n00:{functions['count_down'] + 2:04x} CountDown.loop\n"
                       "00:7000 SomeTable\n", encoding="utf-8")
        tree = root / "disassembly"
        tree.mkdir()
        (tree / "main.asm").write_text(
            "AddB::\n\tadd b\n\tret\nClearA:\n\txor a\n\tret\nCountDown::\n\tld b, 5\n.loop\n\tdec b\n"
            "\tjr nz, .loop\n\tret\nSomeTable:\n\tdb 1, 2, 3\n", encoding="utf-8")
        score_path = root / "score.json"
        gb_lift("score", str(export_path), "--sym", str(sym), "--source", str(tree), "--json", str(score_path))
        score = json.loads(score_path.read_text(encoding="utf-8"))
        assert score["their_functions"] == {"total": 3, "is_our_function": 3,
                                            "decoded_inside_another": 0, "not_decoded": 0}, score
        assert score["our_functions"]["call"]["function_label"] == 3, score["our_functions"]
        assert score["label_kinds"] == {"code": 4, "data": 1, "unknown": 0}, score["label_kinds"]

        # A wrong edit is caught: ADD turned into ADC.
        source = lifted / "lifted_bank_00.c"
        text = source.read_text(encoding="utf-8")
        marker = f"/* {functions['op_80']:04X}: add a, b */"
        line = next(line for line in text.splitlines() if marker in line)
        assert "lift_add8" in line
        wrong = root / "wrong"
        wrong.mkdir()
        (wrong / "lifted_bank_00.c").write_text(text.replace(line, line.replace("lift_add8", "lift_adc8")),
                                                encoding="utf-8")
        wrong_report = root / "wrong.json"
        gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(wrong),
                "--function", f"00:{functions['op_80']:04x}", "--trials", "300",
                "--work", str(root / "work-wrong"), "--json", str(wrong_report), expect=(1,))
        entry = json.loads(wrong_report.read_text(encoding="utf-8"))["results"][0]
        assert entry["verdict"] == "mismatch" and entry["mismatches"], entry
        assert any(difference.startswith(("a:", "flag"))
                   for difference in entry["mismatches"][0]["differences"])

        # And so is one in what a function does after a call it made by
        # hand: that code is the function's own, and its C is what is run.
        marker = f"/* {functions['call_by_hand'] + 8:04X}: inc e */"
        line = next(line for line in text.splitlines() if marker in line)
        assert "lift_inc8" in line, line
        wrong = root / "wrong-back"
        wrong.mkdir()
        (wrong / "lifted_bank_00.c").write_text(text.replace(line, line.replace("lift_inc8", "lift_dec8")),
                                                encoding="utf-8")
        wrong_report = root / "wrong-back.json"
        gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(wrong),
                "--function", f"00:{functions['call_by_hand']:04x}", "--trials", "300",
                "--work", str(root / "work-wrong-back"), "--json", str(wrong_report), expect=(1,))
        entry = json.loads(wrong_report.read_text(encoding="utf-8"))["results"][0]
        assert entry["verdict"] == "mismatch" and entry["mismatches"], entry
        assert any(difference.startswith("e:") for difference in entry["mismatches"][0]["differences"]),             entry["mismatches"][0]

        # Readable source that keeps the behaviour is accepted. It leaves the
        # guest's time uncounted, which the report says rather than rejects.
        readable = root / "readable"
        readable.mkdir()
        (readable / "count_down.c").write_text(f'''#include "gblift.h"

/* Counts b down from 5; what is left is b = 0 and the flags of the last DEC. */
GBLIFT_FUNCTION(0x00, 0x{functions['count_down']:04X}, count_down) {{
    ctx->b = 0;
    ctx->f_z = 1;
    ctx->f_n = 1;
    ctx->f_h = 0;
    lift_ret(ctx);
}}
''', encoding="utf-8")
        readable_report = root / "readable.json"
        gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(readable),
                "--trials", "300", "--work", str(root / "work-readable"), "--json", str(readable_report))
        entry = json.loads(readable_report.read_text(encoding="utf-8"))["results"][0]
        assert entry["verdict"] == "match" and entry["coverage"] == 1.0, entry
        assert entry["cycles"]["not_counted"] == entry["matched"], entry["cycles"]

        # The same for a wait: written as a loop it is accepted, and one that
        # halts once and takes that for the interrupt it wanted is not.
        waits = {
            "loop": ("do {\n        lift_halt(ctx);\n    } "
                     f"while (lift_read8(ctx, 0x{WAITING:04X}) != 0);", "match"),
            "once": ("lift_halt(ctx);", "mismatch"),
        }
        for label, (body, verdict) in waits.items():
            folder = root / f"wait-{label}"
            folder.mkdir()
            (folder / "wait_frame.c").write_text(f'''#include "gblift.h"

/* Waits for the next interrupt handler to clear the flag. */
GBLIFT_FUNCTION(0x00, 0x{functions['wait_frame']:04X}, wait_frame) {{
    lift_write8(ctx, 0x{WAITING:04X}, 1);
    {body}
    ctx->a = 0;
    ctx->f_z = 1;
    ctx->f_n = 0;
    ctx->f_h = 1;
    ctx->f_c = 0;
    lift_ret(ctx);
}}
''', encoding="utf-8")
            wait_report = root / f"wait-{label}.json"
            gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(folder),
                    "--trials", "300", "--work", str(root / f"work-wait-{label}"), "--json", str(wait_report),
                    expect=(0,) if verdict == "match" else (1,))
            entry = json.loads(wait_report.read_text(encoding="utf-8"))["results"][0]
            assert entry["verdict"] == verdict, (label, entry)
            if verdict == "match":
                assert entry["waited"] == entry["matched"] >= 100, (label, entry)

        # And for the loop that never returns. Where the two are compared is
        # in the wait, so the registers are not looked at: memory, the stack
        # and the hardware are. One that counts two a frame is caught there.
        for label, step, verdict in (("counts", 1, "match"), ("miscounts", 2, "mismatch")):
            folder = root / f"loop-{label}"
            folder.mkdir()
            (folder / "main_loop.c").write_text(f'''#include "gblift.h"

/* Counts frames for ever. */
GBLIFT_FUNCTION(0x00, 0x{functions['main_loop']:04X}, main_loop) {{
    for (;;) {{
        if (lift_call(ctx, 0x{functions['wait_frame']:04X}, 0x{functions['main_loop'] + 3:04X})) return;
        lift_write8(ctx, 0x{FRAMES:04X}, (uint8_t)(lift_read8(ctx, 0x{FRAMES:04X}) + {step}));
    }}
}}
''', encoding="utf-8")
            loop_report = root / f"loop-{label}.json"
            gb_lift("check", str(export_path), "--rom", str(rom_path), "--source", str(folder),
                    "--trials", "300", "--work", str(root / f"work-loop-{label}"), "--json", str(loop_report),
                    expect=(0,) if verdict == "match" else (1,))
            entry = json.loads(loop_report.read_text(encoding="utf-8"))["results"][0]
            assert entry["verdict"] == verdict, (label, entry)
            if verdict == "match":
                assert entry["stopped_waiting"] == entry["matched"] > 0, (label, entry)
            else:
                assert any(f"{FRAMES:04X}" in difference
                           for difference in entry["mismatches"][0]["differences"]), entry["mismatches"]

    print(f"lift export, mechanical lift and checker agree on {len(functions)} functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
