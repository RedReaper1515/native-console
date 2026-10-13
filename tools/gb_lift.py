#!/usr/bin/env python3
"""Lift Game Boy functions to C and check the C against the original.

Works from the export `gbrecomp ROM --lift-export FILE` writes (LIFTING.md):

  summary   what the export holds
  function  one function's dossier: disassembly, callers, callees, memory
  baseline  write a first, mechanical C version of functions
  check     build lifted C with the checker and compare it with the ROM
  package   gather lifted C into a package `gbrecomp --lift` builds into the game
  image     turn a save state into a memory image for `check --image`
  entries   list the functions for a game's `--entry-states-of`, and say what
            the states a game kept (`--entry-states`, `check --entries`) cover
  executed  sort the export's functions by what a played game did with their
            bytes: executed them (`--executed`), read them as data, or neither
  score     mark the export against a disassembly of the same ROM
  source    make a source folder of the structured C, or bring one up to date
            with an export, keeping every function somebody changed
  build     from a source folder to a game: check what changed, generate, build

A function that `check` reports as matching did the same as the original on
every machine state tried; the report says how much of the original those
states reached.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LIFT_INCLUDE = REPO / "runtime" / "include"  # gblift.h ships with the runtime
CHECKER_DIR = REPO / "lift" / "checker"
CHECKER_SOURCES = ("lift_machine.c", "lift_cpu.c", "lift_check.c")
EXPORT_SCHEMA = "gbrecomp.lift-export"

# Why a function gets no mechanical version, by hazard.
UNLIFTABLE = {
    "ram_code": "its code runs from RAM",
    "lower_window_bank": "it is in a bank mapped at 0x0000 (MBC1 mode 1)",
}


class LiftError(Exception):
    pass


# --------------------------------------------------------------------------
# The export
# --------------------------------------------------------------------------

def load_export(path: Path) -> dict:
    try:
        export = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise LiftError(f"cannot read export {path}: {error}") from error
    if export.get("schema") != EXPORT_SCHEMA or export.get("version") != 1:
        raise LiftError(f"{path} is not a version 1 {EXPORT_SCHEMA} file")
    return export


def function_index(export: dict) -> dict[str, dict]:
    return {function["id"]: function for function in export["functions"]}


def parse_function_ref(text: str) -> str:
    """Accept gbfn:v1:0002:4497, 02:4497 or 4497 (bank 0) and return the ID."""
    text = text.strip()
    if text.startswith("gbfn:v1:"):
        return text.lower()
    match = re.fullmatch(r"(?:([0-9A-Fa-f]{1,4}):)?\$?([0-9A-Fa-f]{1,4})", text)
    if not match:
        raise LiftError(f"not a function reference: {text!r} (want BB:AAAA in hexadecimal)")
    bank = int(match.group(1) or "0", 16)
    return f"gbfn:v1:{bank:04x}:{int(match.group(2), 16):04x}"


def rom_offset(bank: int, address: int) -> int:
    return bank * 0x4000 + (address & 0x3FFF)


def default_c_name(function: dict) -> str:
    return f"lifted_{function['bank']:02x}_{function['address']:04x}"


def display_name(function: dict) -> str:
    return function.get("symbol") or function["name"]


# --------------------------------------------------------------------------
# summary / function
# --------------------------------------------------------------------------

def command_summary(args: argparse.Namespace) -> int:
    export = load_export(args.export)
    rom = export["rom"]
    functions = export["functions"]
    print(f"{rom['title'] or '(untitled)'}  {rom['size']} bytes  {rom['mapper']}  "
          f"sha256 {rom['sha256'][:16]}...")
    print(f"analysis: {export['analysis_profile']}")
    stats = export["stats"]
    print(f"{stats['functions']} functions, {stats['instructions']} instructions, "
          f"{stats['code_bytes']} bytes of code, {stats['other_bytes']} bytes not decoded")

    kinds: dict[str, int] = {}
    hazards: dict[str, int] = {}
    clean = 0
    for function in functions:
        kinds[function["kind"]] = kinds.get(function["kind"], 0) + 1
        if not function["hazards"]:
            clean += 1
        for hazard in function["hazards"]:
            hazards[hazard] = hazards.get(hazard, 0) + 1
    print("functions by how they were found:")
    for kind, count in sorted(kinds.items(), key=lambda item: -item[1]):
        print(f"  {kind:<14} {count}")
    if functions and "ran" in functions[0]:
        print(f"entered in the runs the evidence is from: "
              f"{sum(1 for function in functions if function['ran'])}")
    print(f"functions with no hazard: {clean}")
    print("hazards:")
    for hazard, count in sorted(hazards.items(), key=lambda item: -item[1]):
        print(f"  {hazard:<20} {count}")

    guesses: dict[str, list[int]] = {}
    for region in export["data_regions"]:
        entry = guesses.setdefault(region["guess"], [0, 0])
        entry[0] += 1
        entry[1] += region["size"]
    print("bytes not decoded, by what they look like:")
    for guess, (count, size) in sorted(guesses.items(), key=lambda item: -item[1][1]):
        print(f"  {guess:<14} {size:>8} bytes in {count} regions")
    print(f"fixed memory addresses the code names: {len(export['memory'])}")
    return 0


def format_dossier(export: dict, function: dict) -> str:
    index = function_index(export)

    def name_of(function_id: str | None) -> str:
        if not function_id:
            return "?"
        other = index.get(function_id)
        return f"{display_name(other)} ({function_id})" if other else function_id

    def reaches_note(site: dict) -> str:
        """Where the helper a site calls takes it, when `gb_lift.py edges` found out."""
        note = ""
        seen = site.get("reaches_seen")
        if seen:
            note = "; seen in a run going on to " + ", ".join(name_of(entry["function"]) for entry in seen[:4])
            if len(seen) > 4:
                note += f" and {len(seen) - 4} more"
        table = site.get("reaches_table")
        if table:
            return (f", which goes on to one of the {len(table['entries'])} functions in the table at "
                    f"{table['bank']:02X}:{table['address']:04X}{note}")
        reaches = site.get("reaches")
        if not reaches:
            return note
        return (f", which goes on to {reaches['bank']:02X}:{reaches['address']:04X} "
                f"{name_of(reaches['function'])}{note}")

    lines = []
    lines.append(f"# {display_name(function)}  {function['id']}")
    lines.append(f"bank {function['bank']:02X}, address {function['address']:04X}, "
                 f"{function['instruction_count']} instructions, {function['byte_count']} bytes, "
                 f"found as: {function['kind']}")
    if function["hazards"]:
        lines.append("hazards: " + ", ".join(function["hazards"]))
    if "declared_table" in function:
        table = function["declared_table"]
        lines.append(f"entry {table['index']} of {table['name']}, a table at "
                     f"{table['bank']:02X}:{table['address']:04X} somebody read the code for")
    if "ran" in function:
        lines.append("entered in the runs the evidence is from" if function["ran"]
                     else "not entered in the runs the evidence is from")
    registers = function["registers"]
    flags = function["flags"]
    note = "  (incomplete: an unknown callee or exit is involved)" if function["summary_incomplete"] else ""
    lines.append("reads before writing: " + (", ".join(registers["reads"] + [f"flag {f}" for f in flags["reads"]]) or "nothing") + note)
    lines.append("may write: " + (", ".join(registers["writes"] + [f"flag {f}" for f in flags["writes"]]) or "nothing"))
    lines.append("always writes: " + (", ".join(registers["always_writes"] + [f"flag {f}" for f in flags["always_writes"]]) or "nothing"))
    if "read_after" in registers:  # an export from before these has none
        def listed(key: str) -> str:
            return ", ".join(registers[key] + [f"flag {f}" for f in flags[key]]) or "nothing"
        if function["summary_incomplete"]:
            lines.append("needs on entry, counting what it cannot be followed into: " + listed("needs"))
        if not function["stack_balanced"]:
            lines.append("does not always return to its caller (the stack is not balanced at a RET)")
        if function["entered_by"]:
            lines.append("may be entered other than by the calls listed: " + ", ".join(function["entered_by"]))
        elif function["stack_balanced"]:
            lines.append("read after it returns, by the callers listed: " + listed("read_after"))
    if function["pushes"] or function["pops"]:
        lines.append(f"pushes {', '.join(function['pushes']) or '-'}; pops {', '.join(function['pops']) or '-'}")

    if function["callers"]:
        lines.append("")
        lines.append("called from:")
        for caller in function["callers"][:24]:
            lines.append(f"  {name_of(caller)}")
        if len(function["callers"]) > 24:
            lines.append(f"  ... and {len(function['callers']) - 24} more")
    if function["calls"]:
        lines.append("")
        lines.append("calls:")
        for call in function["calls"]:
            bank = "??" if call["target_bank"] is None else f"{call['target_bank']:02X}"
            condition = " (conditional)" if call["conditional"] else ""
            lines.append(f"  at {call['site']:04X}: {call['kind']} {bank}:{call['target']:04X} "
                         f"{name_of(call['function']) if call['function'] else '(no known function)'}{condition}"
                         f"{reaches_note(call)}")
    if function.get("seen_jumps"):
        lines.append("")
        lines.append("jumps the code does not spell out, as a run of the game made them:")
        for jump in function["seen_jumps"]:
            lines.append(f"  at {jump['site']:04X}: to " +
                         ", ".join(name_of(entry["function"]) for entry in jump["targets"]))
    if function["jumps_out"]:
        lines.append("")
        lines.append("leaves to:")
        for jump in function["jumps_out"]:
            bank = "??" if jump["target_bank"] is None else f"{jump['target_bank']:02X}"
            lines.append(f"  at {jump['site']:04X}: {jump['kind']} {bank}:{jump['target']:04X} "
                         f"{name_of(jump['function']) if jump['function'] else '(no known function)'}"
                         f"{reaches_note(jump)}")
    if function["memory"]:
        lines.append("")
        lines.append("fixed memory addresses:")
        for access in function["memory"]:
            mode = {"r": "reads", "w": "writes", "rw": "reads and writes", "p": "takes the address of"}.get(
                access["access"], access["access"])
            name = f"  {access['name']}" if access.get("name") else ""
            lines.append(f"  {access['address']:04X} ({access['space']}) {mode}{name}")
    if function["bank_writes"]:
        lines.append("")
        lines.append("cartridge mapper writes:")
        for write in function["bank_writes"]:
            value = "?" if write["value"] is None else f"{write['value']:02X}"
            lines.append(f"  at {write['site']:04X}: {write['address']:04X} = {value}")
    if function["rom_references"]:
        lines.append("")
        lines.append("ROM addresses loaded into a register pair:")
        for reference in function["rom_references"]:
            bank = "??" if reference["bank"] is None else f"{reference['bank']:02X}"
            lines.append(f"  at {reference['site']:04X}: {bank}:{reference['address']:04X} ({reference['points_at']})")

    lines.append("")
    lines.append("disassembly:")
    # A label only where something jumps to; blocks that simply follow on
    # from the one before are listed without a break.
    starts = {block["address"] for block in function["blocks"]}
    labels = {function["address"]}
    for block in function["blocks"]:
        for instruction in block["instructions"]:
            if instruction.get("target") in starts and not instruction["asm"].startswith(("call", "rst")):
                labels.add(instruction["target"])
        labels.update(block.get("jump_returns_to", ()))
    previous_end = None
    for block in sorted(function["blocks"], key=lambda item: item["address"]):
        if block["address"] in labels or block["address"] != previous_end:
            lines.append(f".L{block['address']:04X}:")
        for instruction in block["instructions"]:
            lines.append(f"    {instruction['address']:04X}  {instruction['bytes']:<6}  {instruction['asm']}")
        notes = []
        if block["leaves_to"]:
            notes.append("continues in " + ", ".join(name_of(target) for target in block["leaves_to"]))
        back = block.get("jump_returns_to")
        if back:
            where = " or ".join(f".L{address:04X}" for address in back)
            if block.get("jump_returns_certain"):
                notes.append(f"a call: what it jumps to returns to {where}, pushed above")
            else:
                notes.append(f"a call when the address pushed is {where}: seen to come back there")
        elif block["unknown_exit"]:
            notes.append("leaves to code that was not decoded")
        for note in notes:
            lines.append(f"          ; {note}")
        previous_end = block["end"]
    return "\n".join(lines) + "\n"


def command_function(args: argparse.Namespace) -> int:
    export = load_export(args.export)
    index = function_index(export)
    function_id = parse_function_ref(args.function)
    if function_id not in index:
        raise LiftError(f"{function_id} is not a function in this export")
    sys.stdout.write(format_dossier(export, index[function_id]))
    return 0


# --------------------------------------------------------------------------
# baseline: a mechanical C version, one statement group per instruction
# --------------------------------------------------------------------------

REG8 = ("b", "c", "d", "e", "h", "l", None, "a")
REG16 = ("bc", "de", "hl", "sp")
CONDITIONS = ("!ctx->f_z", "ctx->f_z", "!ctx->f_c", "ctx->f_c")
ALU = ("add8", "adc8", "sub8", "sbc8", "and8", "xor8", "or8", "cp8")
SHIFTS = ("rlc", "rrc", "rl", "rr", "sla", "sra", "swap", "srl")


def read8(index: int) -> str:
    return "lift_read8(ctx, ctx->hl)" if index == 6 else f"ctx->{REG8[index]}"


def write8(index: int, value: str) -> str:
    if index == 6:
        return f"lift_write8(ctx, ctx->hl, {value});"
    return f"ctx->{REG8[index]} = {value};"


class Unliftable(Exception):
    pass


# Instructions with a single memory access, which the hardware makes in their
# last machine cycle. The runtime samples the bus one T-cycle before the
# instruction ends, and a mechanical lift does the same, so that a read of a
# register the hardware is changing (LY, DIV) sees what the original saw.
LATE_ACCESS = frozenset(
    [0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77, 0x36,
     0x0A, 0x1A, 0x02, 0x12, 0xFA, 0xEA, 0xF0, 0xE0, 0xF2, 0xE2, 0x22, 0x2A, 0x32, 0x3A,
     0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE])
# HALT and STOP take their own four cycles inside lift_halt and lift_stop.
SELF_TIMED = frozenset([0x76, 0x10])


def translate(instruction: dict) -> tuple[list[str], str | None]:
    """C statements for one instruction that does not transfer control.

    Returns (statements, None), or ([], kind) for a control transfer the
    caller has to place: jump, jump_cc, call, call_cc, ret, ret_cc, reti,
    rst, jp_hl.
    """
    data = bytes.fromhex(instruction["bytes"])
    op = data[0]
    imm8 = data[1] if len(data) > 1 else 0
    imm16 = data[1] | (data[2] << 8) if len(data) > 2 else 0

    if op == 0xCB:
        cb = data[1]
        reg, bit, group = cb & 7, (cb >> 3) & 7, cb >> 6
        if group == 0:
            return [write8(reg, f"lift_{SHIFTS[bit]}(ctx, {read8(reg)})")], None
        if group == 1:
            return [f"lift_bit(ctx, {bit}, {read8(reg)});"], None
        if group == 2:
            return [write8(reg, f"(uint8_t)({read8(reg)} & 0x{0xFF ^ (1 << bit):02X}u)")], None
        return [write8(reg, f"(uint8_t)({read8(reg)} | 0x{1 << bit:02X}u)")], None

    if 0x40 <= op <= 0x7F:
        if op == 0x76:
            return ["lift_halt(ctx);"], None
        dst, src = (op >> 3) & 7, op & 7
        if dst == src:
            return [], None
        return [write8(dst, read8(src))], None
    if 0x80 <= op <= 0xBF:
        return [f"lift_{ALU[(op >> 3) & 7]}(ctx, {read8(op & 7)});"], None

    if op < 0x40:
        reg, pair = (op >> 3) & 7, (op >> 4) & 3
        low = op & 0x0F
        if low == 0x1:
            return [f"ctx->{REG16[pair]} = 0x{imm16:04X};"], None
        if low == 0x3:
            return [f"ctx->{REG16[pair]}++;"], None
        if low == 0xB:
            return [f"ctx->{REG16[pair]}--;"], None
        if low == 0x9:
            return [f"lift_add_hl(ctx, ctx->{REG16[pair]});"], None
        if (op & 7) == 4:
            return [write8(reg, f"lift_inc8(ctx, {read8(reg)})")], None
        if (op & 7) == 5:
            return [write8(reg, f"lift_dec8(ctx, {read8(reg)})")], None
        if (op & 7) == 6:
            return [write8(reg, f"0x{imm8:02X}")], None
        simple = {
            0x00: [],
            0x02: ["lift_write8(ctx, ctx->bc, ctx->a);"],
            0x12: ["lift_write8(ctx, ctx->de, ctx->a);"],
            0x0A: ["ctx->a = lift_read8(ctx, ctx->bc);"],
            0x1A: ["ctx->a = lift_read8(ctx, ctx->de);"],
            0x22: ["lift_write8(ctx, ctx->hl, ctx->a);", "ctx->hl++;"],
            0x32: ["lift_write8(ctx, ctx->hl, ctx->a);", "ctx->hl--;"],
            0x2A: ["ctx->a = lift_read8(ctx, ctx->hl);", "ctx->hl++;"],
            0x3A: ["ctx->a = lift_read8(ctx, ctx->hl);", "ctx->hl--;"],
            0x07: ["lift_rlca(ctx);"],
            0x0F: ["lift_rrca(ctx);"],
            0x17: ["lift_rla(ctx);"],
            0x1F: ["lift_rra(ctx);"],
            0x08: [f"lift_write16(ctx, 0x{imm16:04X}, ctx->sp);"],
            0x10: ["lift_stop(ctx);"],
            0x27: ["lift_daa(ctx);"],
            0x2F: ["ctx->a = (uint8_t)~ctx->a;", "ctx->f_n = 1;", "ctx->f_h = 1;"],
            0x37: ["ctx->f_n = 0;", "ctx->f_h = 0;", "ctx->f_c = 1;"],
            0x3F: ["ctx->f_n = 0;", "ctx->f_h = 0;", "ctx->f_c = (uint8_t)!ctx->f_c;"],
        }
        if op in simple:
            return simple[op], None
        if op == 0x18:
            return [], "jump"
        return [], "jump_cc"  # 0x20, 0x28, 0x30, 0x38

    pair = (op >> 4) & 3
    if op in (0xC0, 0xC8, 0xD0, 0xD8):
        return [], "ret_cc"
    if op in (0xC2, 0xCA, 0xD2, 0xDA):
        return [], "jump_cc"
    if op in (0xC4, 0xCC, 0xD4, 0xDC):
        return [], "call_cc"
    if op in (0xC1, 0xD1, 0xE1):
        return [f"ctx->{REG16[pair]} = lift_pop16(ctx);"], None
    if op == 0xF1:
        return ["lift_pop_af(ctx);"], None
    if op in (0xC5, 0xD5, 0xE5):
        return [f"lift_push16(ctx, ctx->{REG16[pair]});"], None
    if op == 0xF5:
        return ["lift_push_af(ctx);"], None
    if (op & 0xC7) == 0xC6:
        return [f"lift_{ALU[(op >> 3) & 7]}(ctx, 0x{imm8:02X});"], None
    if (op & 0xC7) == 0xC7:
        return [], "rst"
    table = {
        0xC3: ([], "jump"),
        0xC9: ([], "ret"),
        0xD9: ([], "reti"),
        0xCD: ([], "call"),
        0xE9: ([], "jp_hl"),
        0xE0: ([f"lift_write8(ctx, 0x{0xFF00 + imm8:04X}, ctx->a);"], None),
        0xF0: ([f"ctx->a = lift_read8(ctx, 0x{0xFF00 + imm8:04X});"], None),
        0xE2: (["lift_write8(ctx, (uint16_t)(0xFF00u + ctx->c), ctx->a);"], None),
        0xF2: (["ctx->a = lift_read8(ctx, (uint16_t)(0xFF00u + ctx->c));"], None),
        0xEA: ([f"lift_write8(ctx, 0x{imm16:04X}, ctx->a);"], None),
        0xFA: ([f"ctx->a = lift_read8(ctx, 0x{imm16:04X});"], None),
        0xE8: ([f"ctx->sp = lift_sp_plus(ctx, 0x{imm8:02X});"], None),
        0xF8: ([f"ctx->hl = lift_sp_plus(ctx, 0x{imm8:02X});"], None),
        0xF9: (["ctx->sp = ctx->hl;"], None),
        0xF3: (["lift_di(ctx);"], None),
        0xFB: (["lift_ei(ctx);"], None),
    }
    if op in table:
        return table[op]
    raise Unliftable(f"undefined opcode {op:02X} at {instruction['address']:04X}")


def baseline_source(function: dict, c_name: str | None = None) -> str:
    """A mechanical C version of one function. Raises Unliftable."""
    for hazard, reason in UNLIFTABLE.items():
        if hazard in function["hazards"]:
            raise Unliftable(reason)
    blocks = sorted(function["blocks"], key=lambda block: block["address"])
    if not blocks:
        raise Unliftable("it has no decoded blocks")
    starts = {block["address"] for block in blocks}
    # Jumps and fall-through the export lists as leaving the function do,
    # whatever address they go to: from the home bank, an address in the
    # switchable area is in whichever bank is mapped, not in the bank this
    # function has a block of.
    jumps_away = {jump["site"] for jump in function["jumps_out"] if jump["kind"] == "jump"}
    falls_away = {jump["site"] for jump in function["jumps_out"] if jump["kind"] == "fallthrough"}
    entry = function["address"]
    used_labels: set[int] = set()
    body: list[str] = []

    def goto(target: int) -> str:
        used_labels.add(target)
        return f"goto L_{target:04X};"

    def leave(target: str) -> list[str]:
        return [f"lift_jump(ctx, {target});", "return;"]

    if blocks[0]["address"] != entry:
        body.append("    " + goto(entry))

    for position, block in enumerate(blocks):
        body.append(f"@L_{block['address']:04X}")
        ended = False
        for instruction in block["instructions"]:
            address = instruction["address"]
            data = bytes.fromhex(instruction["bytes"])
            following = (address + len(data)) & 0xFFFF
            cycles = instruction["cycles"]
            taken = instruction.get("cycles_taken", cycles)
            statements, control = translate(instruction)
            comment = f"/* {address:04X}: {instruction['asm']} */"
            lines: list[str] = []
            # Where the original would be once this instruction is done. An
            # interrupt taken at the lift_tick() returns there.
            at_next = f"ctx->pc = 0x{following:04X};"
            if control is None:
                if data[0] in SELF_TIMED:
                    lines = [at_next] + statements
                elif data[0] in LATE_ACCESS and cycles > 1:
                    lines = ([f"lift_advance(ctx, {cycles - 1});"] + statements +
                             [at_next, "lift_tick(ctx, 1);"])
                else:
                    lines = statements + [at_next, f"lift_tick(ctx, {cycles});"]
            else:
                op = data[0]
                condition = CONDITIONS[(op >> 3) & 3]
                target = instruction.get("target", 0)

                # A jump inside the function is an instruction boundary like
                # any other. One that leaves it, a call and a return hand the
                # machine on with their time already passed and nothing
                # taken: what is pending is for the code that runs next,
                # exactly as after the instruction they stand for.
                def go(destination: int, time: int) -> list[str]:
                    if destination in starts and instruction["address"] not in jumps_away:
                        return [f"ctx->pc = 0x{destination:04X};", f"lift_tick(ctx, {time});",
                                goto(destination)]
                    return [f"lift_advance(ctx, {time});"] + leave(f"0x{destination:04X}")

                def call(destination: int, time: int) -> str:
                    return (f"lift_advance(ctx, {time}); "
                            f"if (lift_call(ctx, 0x{destination:04X}, 0x{following:04X})) return;")

                if control == "jump":
                    lines = go(target, cycles)
                    ended = True
                elif control == "jump_cc":
                    lines = [f"if ({condition}) {{ {' '.join(go(target, taken))} }}",
                             at_next, f"lift_tick(ctx, {cycles});"]
                elif control == "call":
                    lines = [call(target, cycles)]
                elif control == "call_cc":
                    lines = [f"if ({condition}) {{ {call(target, taken)} }} "
                             f"else {{ {at_next} lift_tick(ctx, {cycles}); }}"]
                elif control == "rst":
                    lines = [call(op & 0x38, cycles)]
                elif control == "ret":
                    lines = [f"lift_advance(ctx, {cycles});", "lift_ret(ctx);", "return;"]
                    ended = True
                elif control == "reti":
                    lines = [f"lift_advance(ctx, {cycles});", "lift_reti(ctx);", "return;"]
                    ended = True
                elif control == "ret_cc":
                    lines = [f"if ({condition}) {{ lift_advance(ctx, {taken}); lift_ret(ctx); return; }}",
                             at_next, f"lift_tick(ctx, {cycles});"]
                elif control == "jp_hl":
                    # With an address the function pushed itself on top of
                    # the stack, the jump is a call that comes back there.
                    back = [point for point in block.get("jump_returns_to", ()) if point in starts]
                    came_back = "if (lift_call_pushed(ctx, ctx->hl, 0x{0:04X})) return; {1}"
                    lines = [f"lift_advance(ctx, {cycles});"]
                    if len(back) == 1 and block.get("jump_returns_certain"):
                        follows = position + 1 < len(blocks) and blocks[position + 1]["address"] == back[0]
                        lines.append(came_back.format(back[0], "" if follows else goto(back[0])).rstrip())
                    elif back:
                        lines.append("switch (lift_read16(ctx, ctx->sp)) {")
                        lines += ["case 0x{0:04X}: ".format(point) + came_back.format(point, goto(point))
                                  for point in back]
                        lines += ["default: lift_jump(ctx, ctx->hl); return;", "}"]
                    else:
                        lines += leave("ctx->hl")
                    ended = True
            first = True
            for line in lines:
                body.append(f"    {line}" + (f"  {comment}" if first else ""))
                first = False
            if not lines:
                body.append(f"    {comment}")
            if ended:
                break
        if not ended:
            # Control runs off the end of the block.
            end = block["end"] & 0xFFFF
            next_start = blocks[position + 1]["address"] if position + 1 < len(blocks) else None
            if end in starts and block["instructions"][-1]["address"] not in falls_away:
                if end != next_start:
                    body.append("    " + goto(end))
            else:
                for line in leave(f"0x{end:04X}"):
                    body.append(f"    {line}")

    name = c_name or default_c_name(function)
    out = [f"/* {function['id']}  {display_name(function)}"
           f"  ({function['instruction_count']} instructions) */",
           f"GBLIFT_FUNCTION(0x{function['bank']:02X}, 0x{function['address']:04X}, {name}) {{"]
    for line in body:
        if line.startswith("@L_"):
            label = int(line[3:], 16)
            if label in used_labels:
                out.append(f"{line[1:]}:")
        else:
            out.append(line)
    out.append("}")
    return "\n".join(out) + "\n"


def select_functions(export: dict, references: list[str], all_functions: bool,
                     bank: int | None, clean_only: bool) -> list[dict]:
    index = function_index(export)
    if references:
        chosen = []
        for reference in references:
            function_id = parse_function_ref(reference)
            if function_id not in index:
                raise LiftError(f"{function_id} is not a function in this export")
            chosen.append(index[function_id])
        return chosen
    if not all_functions and bank is None:
        raise LiftError("name functions with --function, or use --all or --bank")
    chosen = [f for f in export["functions"] if bank is None or f["bank"] == bank]
    if clean_only:
        chosen = [f for f in chosen if not f["hazards"]]
    return chosen


# Names a lifted function or a memory name must not take: C's own, and ones
# the C library, the runtime or SDL may define in the same program.
RESERVED_NAMES = frozenset("""
auto break case char const continue default do double else enum extern float for goto if inline int
long register restrict return short signed sizeof static struct switch typedef union unsigned void
volatile while main exit abort abs rand srand time clock memcpy memset memmove memcmp strlen strcpy
strcmp printf puts putchar getchar malloc calloc realloc free signal raise remove rename system read
write open close sleep index select connect accept bind listen send recv socket min max near far
small interface environ errno stdin stdout stderr ctx div ldiv fabs floor ceil sqrt pow log exp sin
cos tan round trunc getenv atoi atol strtol qsort bsearch assert setjmp longjmp wait kill pipe link
unlink stat chdir mkdir rmdir access dup fork execv yield pause alarm Sleep Beep
NULL TRUE FALSE EOF BUFSIZ FILE SEEK_SET SEEK_CUR SEEK_END INT_MAX INT_MIN CHAR_BIT RAND_MAX
WINAPI CALLBACK IN OUT CONST VOID BOOL BYTE WORD DWORD ERROR bool true false
""".split())


def c_identifier(name: str) -> str | None:
    """`name` as a C identifier, or None if it cannot be one."""
    if not name or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        return None
    # Locals the structured style declares.
    if re.fullmatch(r"value|(zero|carry)\d*", name):
        return None
    # The second group are the generated code's own prefixes.
    if name in RESERVED_NAMES or name.startswith(
            ("lift_", "gb", "GB", "SDL", "Im", "__", "func_", "body_", "loc_", "pc_", "sym_", "lifted_")):
        return None
    return name


def function_names(export: dict) -> dict[str, str]:
    """A C name for every function: its symbol where it has a usable one."""
    names: dict[str, str] = {}
    taken: set[str] = set()
    for function in export["functions"]:
        name = c_identifier(function.get("symbol", "")) or default_c_name(function)
        if name in taken:
            name = f"{name}_{function['bank']:02x}_{function['address']:04x}"
        taken.add(name)
        names[function["id"]] = name
    return names


def memory_names(export: dict, taken: set[str]) -> dict[int, str]:
    """Names for fixed addresses: hardware registers, and RAM from symbols."""
    import gb_lift_codegen

    names = dict(gb_lift_codegen.HARDWARE_NAMES)
    used = set(names.values()) | taken
    for entry in export["memory"]:
        address = entry["address"]
        if address in names or address < 0x8000:
            continue
        name = c_identifier(entry.get("name", ""))
        if name and name not in used:
            names[address] = name
            used.add(name)
    return names


NAMES_HEADER = "lifted_names.h"


def write_names_header(path: Path, export: dict, names: dict[int, str]) -> None:
    lines = [f"/* Names for fixed addresses of {export['rom']['title'] or 'a ROM'}: the hardware",
             " * registers, and RAM where the export had symbols. Written by tools/gb_lift.py. */",
             "#ifndef LIFTED_NAMES_H", "#define LIFTED_NAMES_H", ""]
    for address, name in sorted(names.items()):
        lines.append(f"#define {name} 0x{address:04X}")
    lines += ["", "#endif", ""]
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def command_baseline(args: argparse.Namespace) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gb_lift_codegen

    export = load_export(args.export)
    functions = select_functions(export, args.function, args.all, args.bank, args.clean_only)
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    index = function_index(export)
    c_names = function_names(export)
    structured = args.style == "structured"
    names = memory_names(export, set(c_names.values())) if structured else {}

    by_bank: dict[int, list[str]] = {}
    skipped: list[tuple[str, str]] = []
    written = 0
    for function in functions:
        try:
            for hazard, reason in UNLIFTABLE.items():
                if hazard in function["hazards"]:
                    raise Unliftable(reason)
            if structured:
                source = gb_lift_codegen.structured_source(
                    function, index, names, c_names[function["id"]], args.time)
            else:
                source = baseline_source(function, c_names[function["id"]])
        except (Unliftable, gb_lift_codegen.Unliftable) as reason:
            skipped.append((function["id"], str(reason)))
            continue
        by_bank.setdefault(function["bank"], []).append(source)
        written += 1

    if structured:
        write_names_header(out_dir / NAMES_HEADER, export, names)
    files = []
    for bank, sources in sorted(by_bank.items()):
        path = out_dir / f"lifted_bank_{bank:02x}.c"
        if structured:
            header = (f"/* Bank {bank:02X} of {export['rom']['title'] or 'a ROM'} "
                      f"(sha256 {export['rom']['sha256'][:16]}...), lifted.\n"
                      " * Written by tools/gb_lift.py baseline --style structured: registers are\n"
                      " * still ctx->a and friends, but flags nothing reads are gone and jumps\n"
                      f" * are if, do and for where the code is laid out that way. Time: {args.time}. */\n"
                      "#include \"gblift.h\"\n"
                      f"#include \"{NAMES_HEADER}\"\n\n")
        else:
            header = (f"/* Mechanical lift of bank {bank:02X} of {export['rom']['title'] or 'a ROM'} "
                      f"(sha256 {export['rom']['sha256'][:16]}...).\n"
                      " * Written by tools/gb_lift.py baseline: one statement group per instruction.\n"
                      " * It is the starting point for readable source, and what the checker\n"
                      " * accepts as equivalent before anyone edits it. */\n"
                      "#include \"gblift.h\"\n\n")
        path.write_text(header + "\n".join(sources), encoding="utf-8", newline="\n")
        files.append(path)
    print(f"wrote {written} functions to {len(files)} files in {out_dir}")
    if skipped:
        reasons: dict[str, int] = {}
        for _, reason in skipped:
            key = reason.split(" at ")[0]
            reasons[key] = reasons.get(key, 0) + 1
        print(f"skipped {len(skipped)}:")
        for reason, count in sorted(reasons.items(), key=lambda item: -item[1]):
            print(f"  {count} because {reason}")
    return 0


# --------------------------------------------------------------------------
# image: a save state as the checker's memory image
# --------------------------------------------------------------------------

def command_image(args: argparse.Namespace) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gb_discover  # the save-state reader lives there

    data = Path(args.savestate).read_bytes()
    state = gb_discover.Savestate(bytearray(data))
    # The header is a C struct holding a 64-bit field, so it is padded to a
    # multiple of eight bytes; the registers follow it.
    header_size = (gb_discover.HEADER.size + 7) & ~7
    af, bc, de, hl, sp, pc = struct.unpack_from("<6H", data, header_size)
    fields = gb_discover.HEADER.unpack_from(data, 0)
    eram_size, wram_size, vram_size, oam_size, hram_size, io_size = fields[4:10]
    eram_off = state.wram_off - eram_size
    wram = data[state.wram_off:state.wram_off + wram_size]
    vram_off = state.wram_off + wram_size
    vram = data[vram_off:vram_off + vram_size]
    oam_off = vram_off + vram_size
    oam = data[oam_off:oam_off + oam_size]
    hram_off = oam_off + oam_size
    hram = data[hram_off:hram_off + hram_size]
    io_off = hram_off + hram_size
    io = data[io_off:io_off + io_size]
    if (wram_size, vram_size, oam_size, hram_size, io_size) != (0x8000, 0x4000, 0xA0, 0x7F, 0x81):
        raise LiftError("this save state does not have the section sizes the image format expects")

    image = bytearray(b"GBLIFTIM")
    image += struct.pack("<I", 1)
    image += struct.pack("<6H", af, bc, de, hl, sp, pc)
    image += struct.pack("<HBBBBBB", args.rom_bank, 0, state.wram_bank, state.vram_bank, 0, 0, 0)
    image += struct.pack("<I", eram_size)
    image += data[eram_off:eram_off + eram_size]
    image += wram + vram + oam + hram + io[:0x80] + io[0x80:0x81]
    Path(args.output).write_bytes(image)
    print(f"wrote {args.output}: sp {sp:04X}, {eram_size} bytes of cartridge RAM")
    return 0


# --------------------------------------------------------------------------
# entries: the states a played game entered its functions in
# --------------------------------------------------------------------------

ENTRY_STATES_MAGIC = b"GBLIFTEN"


def read_entry_states(path: Path) -> dict:
    """The head of a file a game wrote with --entry-states: its numbers, and
    for each function how often control arrived there and how many states
    were kept (runtime/src/gbrt_entry_states.c has the format)."""
    with open(path, "rb") as file:
        header = file.read(48)
        if len(header) != 48 or header[:8] != ENTRY_STATES_MAGIC or struct.unpack_from("<I", header, 8)[0] != 1:
            raise LiftError(f"{path} is not a version 1 file of entry states")
        (page_size, eram_size, pages_per_state, state_count, page_count, function_count, listed,
         per) = struct.unpack_from("<8I", header, 12)
        table = file.read(function_count * 12)
        if len(table) != function_count * 12:
            raise LiftError(f"{path} ends before its list of functions does")
    functions = {}
    for bank, address, arrivals, kept in struct.iter_unpack("<HHII", table):
        functions[f"gbfn:v1:{bank:04x}:{address:04x}"] = (arrivals, kept)
    return {"path": path, "page_size": page_size, "eram_size": eram_size, "pages_per_state": pages_per_state,
            "states": state_count, "pages": page_count, "listed": bool(listed), "per": per,
            "functions": functions}


def command_entries(args: argparse.Namespace) -> int:
    export = load_export(args.export)
    index = function_index(export)
    verdicts: dict[str, str] = {}
    if args.report:
        report = json.loads(Path(args.report).read_text(encoding="utf-8"))
        verdicts = {entry["function"]: entry["verdict"] for entry in report.get("results", [])}

    if args.output:
        # The list a game is told with --entry-states-of: where each function
        # begins. With a report, only the ones it does not call a match.
        # Not what cannot be lifted (code in RAM, a bank in the lower window):
        # that is no place in the ROM as a game has it mapped.
        wanted = [function for function in export["functions"]
                  if not any(hazard in function["hazards"] for hazard in UNLIFTABLE)
                  and (not args.report or verdicts.get(function["id"]) != "match")]
        lines = [f"# {len(wanted)} functions of {Path(args.export).name}"
                 + (f" that {Path(args.report).name} does not call a match" if args.report else ""),
                 "# for a game's --entry-states-of (tools/gb_lift.py entries)"]
        lines += [f"{function['bank']:02X}:{function['address']:04X}"
                  for function in sorted(wanted, key=lambda function: (function["bank"], function["address"]))]
        Path(args.output).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        print(f"wrote {args.output}: {len(wanted)} functions")
    if not args.states:
        if not args.output:
            raise LiftError("give -o LIST to write the list of functions, or --states FILE to read what a game kept")
        return 0

    # What the games kept, against the export: a function counts by its most
    # arrivals in any one file.
    arrivals: dict[str, int] = {}
    kept: dict[str, int] = {}
    total_states = 0
    for path in args.states:
        states = read_entry_states(Path(path))
        total_states += states["states"]
        for function_id, (arrived, held) in states["functions"].items():
            arrivals[function_id] = max(arrivals.get(function_id, 0), arrived)
            kept[function_id] = kept.get(function_id, 0) + held
    ours = {function_id for function_id in arrivals if function_id in index}
    arrived_at = {function_id for function_id in ours if arrivals[function_id]}
    elsewhere = sum(1 for function_id, count in arrivals.items() if count and function_id not in index)
    never = sorted(set(index) - arrived_at)
    print(f"{len(args.states)} file{'s' if len(args.states) != 1 else ''} of entry states: {total_states} states")
    print(f"of the export's {len(index)} functions, control arrived at the start of {len(arrived_at)} "
          f"({sum(1 for function_id in arrived_at if kept[function_id])} with a state kept); "
          f"{len(never)} were never arrived at")
    if elsewhere:
        print(f"{elsewhere} places arrived at are no function of the export")
    by_verdict: dict[str, list[int]] = {}
    if verdicts:
        for function_id in index:
            counts = by_verdict.setdefault(verdicts.get(function_id, "not checked"), [0, 0])
            counts[0] += 1
            counts[1] += function_id in arrived_at
        for verdict in sorted(by_verdict):
            total, reached = by_verdict[verdict]
            print(f"  {verdict}: {total} functions, {reached} arrived at, {total - reached} never")
    by_kind: dict[str, list[int]] = {}
    for function_id, function in index.items():
        counts = by_kind.setdefault(function["kind"], [0, 0])
        counts[0] += 1
        counts[1] += function_id in arrived_at
    print("by how the export came by the function:")
    for kind in sorted(by_kind, key=lambda kind: -by_kind[kind][0]):
        total, reached = by_kind[kind]
        print(f"  {kind}: {total} functions, {reached} arrived at, {total - reached} never")
    if args.json:
        Path(args.json).write_text(json.dumps({
            "schema": "gbrecomp.lift-entries", "version": 1,
            "files": [str(path) for path in args.states], "states": total_states,
            "functions": len(index), "arrived_at": len(arrived_at),
            "never": never,
            "arrivals": {function_id: arrivals[function_id] for function_id in sorted(arrived_at)},
            "kept": {function_id: kept[function_id] for function_id in sorted(arrived_at)},
            "by_kind": {kind: {"functions": total, "arrived_at": reached}
                        for kind, (total, reached) in by_kind.items()},
            "by_verdict": {verdict: {"functions": total, "arrived_at": reached}
                           for verdict, (total, reached) in by_verdict.items()},
        }, indent=1) + "\n", encoding="utf-8", newline="\n")
    return 0


# --------------------------------------------------------------------------
# executed: what a played game did with the bytes the export calls functions
# --------------------------------------------------------------------------

EXECUTED_MAGIC = b"GBEXECUT"
# How a function stands to what was played, from the surest sign that it is
# code to none.
EXECUTED_CLASSES = ("ran", "ran_inside", "ran_otherwise", "read", "neither")
EXECUTED_SAYS = {
    "ran": "its first instruction was executed",
    "ran_inside": "instructions of it were executed, its first never: entered further in",
    "ran_otherwise": "bytes of it were executed, as other instructions than the export decoded",
    "read": "none of it was executed, and the game read bytes of it as data",
    "neither": "the game neither executed nor read any of it",
}


def read_executed(path: Path) -> tuple[bytearray, int]:
    """The map a game wrote with --executed: a byte for each byte of the ROM
    (bit 0: an executed instruction began there; bit 1: a later byte of one),
    and how many instructions were run. runtime/include/gbrt_executed.h has
    the format."""
    data = path.read_bytes()
    if len(data) < 28 or data[:8] != EXECUTED_MAGIC or struct.unpack_from("<I", data, 8)[0] != 1:
        raise LiftError(f"{path} is not a version 1 map of executed bytes")
    size = struct.unpack_from("<I", data, 12)[0]
    if len(data) != 28 + size:
        raise LiftError(f"{path} is not the size its head says")
    return bytearray(data[28:]), struct.unpack_from("<Q", data, 20)[0]


def read_data_reads(path: Path, size: int) -> bytearray:
    """For each byte of the ROM, whether a `--data-use` record has the game
    reading it as data: 1 read, 3 read and used for something."""
    record = json.loads(path.read_text(encoding="utf-8"))
    if record.get("schema") != "gbrt.data-use":
        raise LiftError(f"{path} is not a record of what the data is used for (--data-use)")
    if record["rom"]["size"] != size:
        raise LiftError(f"{path} was recorded from another ROM")
    read = bytearray(size)
    for entry in record["ranges"]:
        flag = 3 if entry["as_it_is"] or entry["worked_out"] else 1
        for offset in range(entry["offset"], min(entry["offset"] + entry["size"], size)):
            read[offset] |= flag
    return read


def command_executed(args: argparse.Namespace) -> int:
    export = load_export(args.export)
    size = export["rom"]["size"]
    ran = bytearray(size)
    instructions = 0
    for name in args.map:
        one, count = read_executed(Path(name))
        if len(one) != size:
            raise LiftError(f"{name} was recorded from another ROM")
        instructions += count
        ran = bytearray(a | b for a, b in zip(ran, one))
    read = bytearray(size)
    for name in args.data_use:
        one = read_data_reads(Path(name), size)
        read = bytearray(a | b for a, b in zip(read, one))

    verdicts: dict[str, str] = {}
    if args.verdicts:
        given = json.loads(Path(args.verdicts).read_text(encoding="utf-8"))
        if "results" in given:   # a `check --json` report
            verdicts = {entry["function"]: entry["verdict"] for entry in given["results"]}
        else:                    # a source folder's lift-source.json
            verdicts = {function_id: entry.get("verdict", "not checked")
                        for function_id, entry in given.get("functions", {}).items()}

    decoded = bytearray(size)   # a byte the export has in an instruction
    functions = []
    for function in export["functions"]:
        spans = [(rom_offset(function["bank"], instruction["address"]), len(instruction["bytes"]) // 2)
                 for block in function["blocks"] for instruction in block["instructions"]
                 if instruction["address"] < 0x8000]
        spans = [(offset, length) for offset, length in spans if offset + length <= size]
        if not spans:
            continue
        own = sorted({offset + i for offset, length in spans for i in range(length)})
        for offset in own:
            decoded[offset] = 1
        first = rom_offset(function["bank"], function["address"])
        began = sum(1 for offset, _ in spans if ran[offset] & 1)
        bytes_ran = sum(1 for offset in own if ran[offset])
        bytes_read = sum(1 for offset in own if read[offset] and not ran[offset])
        if first < size and ran[first] & 1:
            kind = "ran"
        elif began:
            kind = "ran_inside"
        elif bytes_ran:
            kind = "ran_otherwise"
        elif bytes_read:
            kind = "read"
        else:
            kind = "neither"
        functions.append({"function": function["id"], "class": kind, "kind": function["kind"],
                          "bytes": len(own), "instructions": len(spans), "instructions_run": began,
                          "bytes_run": bytes_ran, "bytes_read": bytes_read})

    def table(title: str, key) -> dict[str, dict[str, int]]:
        rows: dict[str, dict[str, int]] = {}
        for entry in functions:
            row = rows.setdefault(key(entry), dict.fromkeys(EXECUTED_CLASSES, 0))
            row[entry["class"]] += 1
        print(f"\n{title:<16}" + "".join(f"{word:>15}" for word in EXECUTED_CLASSES) + f"{'all':>8}")
        for name in sorted(rows, key=lambda name: -sum(rows[name].values())):
            print(f"{name:<16}" + "".join(f"{rows[name][word]:>15}" for word in EXECUTED_CLASSES)
                  + f"{sum(rows[name].values()):>8}")
        return rows

    counts = dict.fromkeys(EXECUTED_CLASSES, 0)
    for entry in functions:
        counts[entry["class"]] += 1
    print(f"{len(args.map)} map{'s' if len(args.map) != 1 else ''} of executed bytes "
          f"({instructions} instructions run from the ROM), {len(args.data_use)} "
          f"record{'s' if len(args.data_use) != 1 else ''} of what was read as data")
    print(f"the export's {len(functions)} functions, by what the game did with them:")
    for word in EXECUTED_CLASSES:
        print(f"  {counts[word]:>6}  {word:<14} {EXECUTED_SAYS[word]}")
    if not args.data_use:
        print("  (no record of what was read was given: `read` cannot be told from `neither`)")
    by_verdict = table("verdict", lambda entry: verdicts.get(entry["function"], "not checked")) if verdicts else {}
    by_kind = table("how it was found", lambda entry: entry["kind"])

    # The same by the byte, and what ran that the export has no instruction for.
    both = sum(1 for offset in range(size) if decoded[offset] and ran[offset] and read[offset])
    bytes_ = {
        "decoded": sum(decoded),
        "decoded_ran": sum(1 for offset in range(size) if decoded[offset] and ran[offset]),
        "decoded_read_only": sum(1 for offset in range(size) if decoded[offset] and read[offset] and not ran[offset]),
        "decoded_neither": sum(1 for offset in range(size) if decoded[offset] and not ran[offset] and not read[offset]),
        "ran_and_read": both,
        "ran_not_decoded": sum(1 for offset in range(size) if ran[offset] and not decoded[offset]),
        "ran": sum(1 for offset in range(size) if ran[offset]),
        "read": sum(1 for offset in range(size) if read[offset]),
    }
    print(f"\nby the byte: of {bytes_['decoded']} bytes the export has in instructions, "
          f"{bytes_['decoded_ran']} were executed, {bytes_['decoded_read_only']} were read as data and "
          f"never executed, {bytes_['decoded_neither']} neither")
    print(f"{bytes_['ran_and_read']} bytes were both executed and read as data (code a game copies, or "
          f"sums up)")
    print(f"{bytes_['ran_not_decoded']} executed bytes are in no instruction of the export: code it does "
          f"not have")
    if args.json:
        Path(args.json).write_text(json.dumps({
            "schema": "gbrecomp.lift-executed", "version": 1,
            "maps": [str(name) for name in args.map], "data_use": [str(name) for name in args.data_use],
            "instructions": instructions, "classes": counts, "bytes": bytes_,
            "by_verdict": by_verdict, "by_kind": by_kind, "functions": functions,
        }, indent=1) + "\n", encoding="utf-8", newline="\n")
    return 0

# --------------------------------------------------------------------------
# check
# --------------------------------------------------------------------------

def merge_check_runs(runs: list[tuple[str, dict]]) -> dict:
    """One report from the runs of a check, each from another kind of state.

    A function that differs in any run is a mismatch; one that passes in any
    and differs in none is a match, and `passed_from` says in which. The
    counts are added up and what was executed is taken together."""
    report = dict(runs[0][1])
    merged: dict[str, dict] = {}
    for label, run in runs:
        for entry in run["results"]:
            for mismatch in entry["mismatches"]:
                mismatch["states"] = label
            passed = [label] if entry["verdict"] == "match" else []
            into = merged.get(entry["function"])
            if into is None:
                merged[entry["function"]] = dict(entry, passed_from=passed)
                continue
            into["passed_from"] += passed
            for key in ("trials", "matched", "mismatched", "skipped", "ran_code_in_ram", "waited",
                        "stopped_waiting"):
                into[key] += entry[key]
            if "entry_states" in entry:
                states = into.setdefault("entry_states", {"kept": 0, "matched": 0, "mismatched": 0,
                                                          "thrown_away": 0})
                for key in states:
                    states[key] += entry["entry_states"][key]
            for reason, count in entry["skipped_because"].items():
                into["skipped_because"][reason] = into["skipped_because"].get(reason, 0) + count
            for key in ("equal", "different", "not_counted", "not_compared"):
                into["cycles"][key] += entry["cycles"][key]
            if "observed" in entry:
                seen = into.setdefault("observed", {"states": 0, "reads": 0, "changed": 0, "through": 0})
                seen["states"] += entry["observed"]["states"]
                for key in ("reads", "changed", "through"):
                    seen[key] |= entry["observed"][key]
            into["mismatches"] = (into["mismatches"] + entry["mismatches"])[:3]
            into["executed"] = sorted(set(into["executed"]) | set(entry["executed"]))
            into["memory_read"] = sorted(set(into["memory_read"]) | set(entry["memory_read"]))[:256]
            # By the runs' own verdicts: a run can pass states and still give
            # none (more of them thrown away as `used_its_stack_as_data`).
            into["verdict"] = ("mismatch" if into["mismatched"] else
                               "match" if into["passed_from"] else "inconclusive")
    report["results"] = list(merged.values())
    report["states"] = [label for label, _ in runs]
    report["image"] = any(run.get("image") for _, run in runs)
    report["entries"] = any(run.get("entries") for _, run in runs)
    return report


# A register or a flag as a bit: gblift.h's LIFT_REG_ and LIFT_FLAG_, which
# are the checker's IGNORE_ bits.
SIGNATURE_BITS = {"a": 0x001, "b": 0x002, "c": 0x004, "d": 0x008, "e": 0x010, "h": 0x020, "l": 0x040,
                  "fz": 0x080, "fn": 0x100, "fh": 0x200, "fc": 0x400,
                  "bc": 0x006, "de": 0x018, "hl": 0x060, "flags": 0x780}


def signature_bits(names: list[str], where: str) -> int:
    bits = 0
    for name in names:
        if name not in SIGNATURE_BITS:
            raise LiftError(f"{where}: {name!r} is no register or flag (a b c d e h l bc de hl fz fn fh fc flags)")
        bits |= SIGNATURE_BITS[name]
    return bits


DECLARATION = re.compile(
    r"GBLIFT_FUNCTION\(\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*([A-Za-z_]\w*)\s*\)")


def find_compiler(explicit: str | None) -> str:
    candidates = [explicit, os.environ.get("CC"), "cc", "gcc", "clang", "cl"]
    for candidate in candidates:
        if not candidate:
            continue
        found = shutil.which(candidate)
        if found:
            return found
    raise LiftError("no C compiler found: put cc, gcc, clang or cl on PATH, or pass --cc")


def is_msvc(compiler: str) -> bool:
    return Path(compiler).stem.lower() == "cl"


def scan_sources(sources: list[Path]) -> list[tuple[int, int, str, Path]]:
    found = []
    seen: dict[tuple[int, int], Path] = {}
    for source in sources:
        try:
            text = source.read_text(encoding="utf-8")
        except OSError as error:
            raise LiftError(f"cannot read {source}: {error}") from error
        for match in DECLARATION.finditer(text):
            bank, address = int(match.group(1), 0), int(match.group(2), 0)
            key = (bank, address)
            if key in seen:
                raise LiftError(f"{bank:02X}:{address:04X} is lifted twice: in {seen[key]} and {source}")
            seen[key] = source
            found.append((bank, address, match.group(3), source))
    return found


def support_wrappers(support: list[Path], work: Path) -> list[Path]:
    """One file in `work` for each file of support code, which includes it.

    Support code is kept in folders, and two folders may each have a
    `copy.c`: compiled where they are, the two would be one object file to
    the compiler. What a wrapper includes still finds its own headers beside
    itself."""
    for stale in work.glob("lift_support_*.c"):
        stale.unlink()
    wrappers = []
    for number, path in enumerate(support):
        wrapper = work / f"lift_support_{number:04d}.c"
        spelled = str(path).replace("\\", "/")
        wrapper.write_text(f'/* Written by tools/gb_lift.py check. */\n#include "{spelled}"\n',
                           encoding="utf-8", newline="\n")
        wrappers.append(wrapper)
    return wrappers


def build_checker(compiler: str, sources: list[Path], table: Path, work: Path,
                  optimize: bool, support: list[Path] | None = None,
                  include: list[Path] | None = None) -> Path:
    exe = work / ("lift_check.exe" if os.name == "nt" else "lift_check")
    inputs = [CHECKER_DIR / name for name in CHECKER_SOURCES] + [table] + sources
    inputs += support_wrappers(support or [], work)
    folders = [LIFT_INCLUDE, CHECKER_DIR] + list(include or [])
    if is_msvc(compiler):
        # 4201: the nameless structs the register pairs are made of.
        # GBLIFT_CHECKING: the source is compared with the ROM as it is, so a name of the
        # source for something in the ROM leads where the ROM has it (lifted_data.h).
        command = [compiler, "/nologo", "/std:c11", "/W3", "/wd4201", "/wd4102", "/D_CRT_SECURE_NO_WARNINGS", "/MP",
                   "/DGBLIFT_CHECKING", "/O2" if optimize else "/Od"] + [f"/I{folder}" for folder in folders]
        command += [str(path) for path in inputs]
        command += [f"/Fe:{exe}", "/link", "/STACK:8388608"]
    else:
        command = [compiler, "-std=c11", "-O2" if optimize else "-O0", "-w", "-DGBLIFT_CHECKING"]
        for folder in folders:
            command += ["-I", str(folder)]
        command += [str(path) for path in inputs]
        command += ["-o", str(exe)]
    result = subprocess.run(command, cwd=work, capture_output=True, text=True)
    if result.returncode != 0:
        output = (result.stdout + result.stderr).strip()
        raise LiftError("the lifted source does not compile:\n" + output[-6000:])
    return exe


def command_check(args: argparse.Namespace) -> int:
    export = load_export(args.export)
    index = function_index(export)
    rom = Path(args.rom).resolve()
    if not rom.is_file():
        raise LiftError(f"no ROM at {rom}")

    sources: list[Path] = []
    for item in args.source:
        path = Path(item)
        if path.is_dir():
            sources += sorted(path.glob("*.c"))
        else:
            sources.append(path)
    sources = [path.resolve() for path in sources]
    if not sources:
        raise LiftError("no lifted source given")
    declared = scan_sources(sources)
    if not declared:
        raise LiftError("no GBLIFT_FUNCTION(bank, address, name) found in the source given")
    # Code the functions call that is no function of the ROM's by itself: it
    # is compiled in and nothing in it is looked for.
    support = [Path(item).resolve() for item in args.support]
    for path in support:
        if not path.is_file():
            raise LiftError(f"no support file at {path}")
    for bank, address, name, source in scan_sources(support):
        raise LiftError(f"{source} is given as support code and declares {name} ({bank:02X}:{address:04X}): "
                        "a file with a GBLIFT_FUNCTION is --source")
    include = [Path(item).resolve() for item in args.include]

    wanted = {parse_function_ref(reference) for reference in args.function} if args.function else None
    ignores: dict[str, str] = {}
    held: dict[str, tuple[int, int, int]] = {}
    if args.contract:
        contract = json.loads(Path(args.contract).read_text(encoding="utf-8"))
        for function_id, entry in contract.get("functions", {}).items():
            ignores[parse_function_ref(function_id)] = ",".join(entry.get("ignore", []))
            if "takes" in entry:
                # A signature the function is held to (gb_lift_signatures.py).
                held[parse_function_ref(function_id)] = tuple(
                    signature_bits(entry.get(key, []), f"{function_id}: {key}") for key in ("takes", "gives", "keeps"))

    rows = []
    for bank, address, name, source in declared:
        function_id = f"gbfn:v1:{bank:04x}:{address:04x}"
        if wanted is not None and function_id not in wanted:
            continue
        function = index.get(function_id)
        if function is None:
            raise LiftError(f"{source.name}: {name} claims {bank:02X}:{address:04X}, "
                            "which is not a function in the export")
        for hazard, reason in UNLIFTABLE.items():
            if hazard in function["hazards"]:
                raise LiftError(f"{source.name}: {name} ({bank:02X}:{address:04X}) cannot be checked: {reason}")
        offsets = [rom_offset(bank, i["address"]) for b in function["blocks"] for i in b["instructions"]
                   if i["address"] < 0x8000]
        rows.append((bank, address, name, function, min(offsets, default=0), max(offsets, default=0),
                     ignores.get(function_id, ""), held.get(function_id)))
    if not rows:
        raise LiftError("none of the functions asked for is in the source given")

    work = (Path(args.work) if args.work else REPO / "output" / "lift-check").resolve()
    work.mkdir(parents=True, exist_ok=True)
    table = work / "lift_candidates.c"
    lines = ["/* Written by tools/gb_lift.py check. */", '#include "lift_check.h"', ""]
    for _, _, name, *_ in rows:
        lines.append(f"void {name}(GBContext* ctx);")
    lines.append("")
    lines.append("const LiftCandidate lift_candidates[] = {")
    for bank, address, name, function, low, high, ignore, signature in rows:
        label = display_name(function).replace("\\", "\\\\").replace('"', '\\"')
        # Something is known to call or enter it, so it is code and worth
        # more states: not so for what only a jump or nothing at all reaches.
        called = int(function["kind"] not in ("unreferenced", "shared_tail", "detached_tail"))
        signed = "1, 0x{:03X}, 0x{:03X}, 0x{:03X}".format(*signature) if signature else "0, 0, 0, 0"
        lines.append(f'    {{0x{bank:04X}, 0x{address:04X}, {name}, "{label}", {low}u, {high}u, "{ignore}", {called}, '
                     f'{signed}}},')
    lines.append("};")
    lines.append(f"const unsigned lift_candidate_count = {len(rows)}u;")
    table.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    compiler = find_compiler(args.cc)
    started = time.monotonic()
    exe = build_checker(compiler, sources, table, work, not args.no_optimize, support, include)
    built = time.monotonic() - started

    command = [str(exe), "--rom", str(rom), "--trials", str(args.trials), "--seed", str(args.seed),
               "--max-steps", str(args.max_steps)]
    if args.ignore:
        command += ["--ignore", args.ignore]
    if args.strict_stack:
        command.append("--strict-stack")
    if args.observe:
        command += ["--observe", "--observe-states", str(args.observe_states)]

    # One worker per processor, each taking every Nth function.
    jobs = args.jobs if args.jobs else (os.cpu_count() or 1)
    jobs = max(1, min(jobs, (len(rows) + 15) // 16))
    started = time.monotonic()
    deadline = started + args.timeout

    def run_states(extra: list[str]) -> dict:
        """Every function from one kind of state: made-up memory, an image, or
        the states a game entered its functions in."""
        workers = []
        for shard in range(jobs):
            raw = work / f"lift_check_raw_{shard}.json"
            raw.unlink(missing_ok=True)
            log = open(work / f"lift_check_{shard}.log", "w", encoding="utf-8")
            process = subprocess.Popen(command + extra + ["--shard", f"{shard}/{jobs}", "--json", str(raw)],
                                       cwd=work, stdout=log, stderr=subprocess.STDOUT)
            workers.append((process, raw, log))
        part_report = None
        failure = None
        for process, raw, log in workers:
            try:
                code = process.wait(timeout=max(1.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                for other, _, _ in workers:
                    other.kill()
                failure = (f"the checker did not finish in {args.timeout} s: a lifted function "
                           "probably loops without touching memory")
                code = None
            log.close()
            if failure:
                continue
            if code not in (0, 1, 3):
                failure = "the checker failed:\n" + Path(log.name).read_text(encoding="utf-8")[-4000:]
                continue
            part = json.loads(raw.read_text(encoding="utf-8"))
            if part_report is None:
                part_report = part
            else:
                part_report["results"] += part["results"]
        if failure:
            raise LiftError(failure)
        return part_report

    # Made-up memory first, then each image of the game's own: a function
    # passes where any of them gets it through, and fails where any differs.
    images = [Path(image).resolve() for image in args.image]
    entry_files = [Path(entries).resolve() for entries in args.entries]
    for path in images + entry_files:
        if not path.is_file():
            raise LiftError(f"no file at {path}")
    runs: list[tuple[str, dict]] = []
    if not (args.images_only and (images or entry_files)):
        runs.append(("made-up", run_states([])))
    for image in images:
        runs.append((image.name, run_states(["--image", str(image)])))
    # And the states a game really entered each function in: a function is
    # run from those exactly as the game ran it, and from nothing made up.
    for entries in entry_files:
        runs.append((entries.name, run_states(["--entries", str(entries)])))
    report = merge_check_runs(runs)
    ran = time.monotonic() - started
    report["results"].sort(key=lambda entry: entry["function"])

    counts = {"match": 0, "mismatch": 0, "inconclusive": 0}
    by_id = {f"gbfn:v1:{bank:04x}:{address:04x}": (function, name)
             for bank, address, name, function, *_ in rows}
    for entry in report["results"]:
        function, c_name = by_id[entry["function"]]
        own = {rom_offset(function["bank"], i["address"])
               for b in function["blocks"] for i in b["instructions"] if i["address"] < 0x8000}
        executed = own & set(entry["executed"])
        entry["c_name"] = c_name
        entry["instructions"] = len(own)
        entry["instructions_executed"] = len(executed)
        entry["coverage"] = round(len(executed) / len(own), 4) if own else 0.0
        del entry["executed"]
        counts[entry["verdict"]] += 1
    report["rom_sha256"] = export["rom"]["sha256"]
    report["summary"] = counts
    report["build_seconds"] = round(built, 2)
    report["run_seconds"] = round(ran, 2)
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8", newline="\n")

    shown = 0
    for entry in report["results"]:
        verdict = entry["verdict"]
        if verdict == "match" and not args.verbose and len(report["results"]) > 20:
            continue
        timing = entry["cycles"]
        if verdict != "match":
            clock = ""
        elif timing["different"]:
            clock = "  time differs"
        elif timing["not_counted"] == entry["matched"]:
            clock = "  time not counted"
        elif not timing["equal"]:
            # Every state it got through was one in which the original used what it had
            # pushed as data, and the source keeps its own stack: the time says nothing.
            clock = "  time not compared"
        else:
            clock = "  same time"
        # States it got through only because a HALT changed what it polled,
        # and states it never returned from, compared at a wait instead.
        notes = []
        if entry.get("waited"):
            notes.append(f"{entry['waited']} through a wait")
        if entry.get("stopped_waiting"):
            notes.append(f"{entry['stopped_waiting']} compared at a wait, not returned")
        # Passed only from the game's own memory: nothing made up got through.
        if len(report["states"]) > 1 and entry["passed_from"] and "made-up" not in entry["passed_from"]:
            notes.append("only from " + ", ".join(entry["passed_from"]))
        waits = f" ({', '.join(notes)})" if notes else ""
        print(f"{verdict:<12} {entry['function']}  {entry['c_name']}  "
              f"{entry['matched']}/{entry['trials']} states{waits}, "
              f"{entry['coverage'] * 100:.0f}% of its instructions reached{clock}")
        if entry["skipped"]:
            reasons = ", ".join(f"{count} {reason}" for reason, count in entry["skipped_because"].items())
            print(f"             {entry['skipped']} states not usable: the original gave {reasons}")
        for mismatch in entry["mismatches"][:1 if not args.verbose else 3]:
            regs = mismatch["entry"]
            source = "" if mismatch["states"] == "made-up" else f" from {mismatch['states']}"
            if "arrival" in mismatch:
                source += (f", arrival {mismatch['arrival']}" +
                           (" as the game had it" if mismatch["as_recorded"] else " with what it reads changed"))
            print(f"             state {mismatch['trial']}{source}: a={regs['a']:02X} bc={regs['b']:02X}{regs['c']:02X} "
                  f"de={regs['d']:02X}{regs['e']:02X} hl={regs['h']:02X}{regs['l']:02X} sp={regs['sp']:04X} "
                  f"z={regs['fz']} n={regs['fn']} h={regs['fh']} c={regs['fc']} bank={regs['rom_bank']:02X}")
            for difference in mismatch["differences"]:
                print(f"               {difference}")
        shown += 1
    kinds = report["states"]
    each = f"{args.trials} states each" if len(kinds) == 1 else \
        f"{args.trials} states each from " + ", ".join("made-up memory" if kind == "made-up" else kind
                                                       for kind in kinds)
    print(f"{counts['match']} match, {counts['mismatch']} mismatch, {counts['inconclusive']} inconclusive "
          f"of {len(report['results'])} functions; {each} "
          f"(built in {built:.1f} s, ran in {ran:.1f} s)")
    if len(kinds) > 1:
        only = sum(1 for entry in report["results"]
                   if entry["verdict"] == "match" and "made-up" not in entry["passed_from"])
        print(f"{only} of the matches pass only from the game's own memory")
    if report.get("entries"):
        with_states = [entry for entry in report["results"] if entry.get("entry_states", {}).get("kept")]
        as_played = sum(1 for entry in with_states
                        if entry["entry_states"]["matched"] and not entry["entry_states"]["mismatched"])
        print(f"{len(with_states)} of the functions have states a game entered them in; "
              f"{as_played} of those did as the original from such a state exactly as the game had it")
    waiting = sum(1 for entry in report["results"] if entry["verdict"] == "match" and entry.get("waited"))
    if waiting:
        print(f"{waiting} of the matches wait for an interrupt in some state: compared with a HALT "
              "changing what they poll, not with a handler run")
    endless = sum(1 for entry in report["results"]
                  if entry["verdict"] == "match" and entry.get("stopped_waiting") == entry["matched"])
    if endless:
        print(f"{endless} of the matches returned from no state: compared where they wait, on memory, "
              "the stack and the hardware, not on registers")
    if counts["mismatch"]:
        return 1
    if counts["inconclusive"] and not counts["match"]:
        return 3
    return 0


# --------------------------------------------------------------------------
# score: the export against a disassembly
# --------------------------------------------------------------------------

def command_score(args: argparse.Namespace) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gb_lift_score

    export = load_export(args.export)
    if "symbols=yes" in export.get("analysis_profile", ""):
        print("note: this export was made with --symbols, so the analysis already had the "
              "disassembly's labels; score an export made without them", file=sys.stderr)
    result = gb_lift_score.score(export, args.sym, args.source)
    sys.stdout.write(gb_lift_score.format_score(result))
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8", newline="\n")
    return 0


# --------------------------------------------------------------------------
# coverage: what a run of the game ran, lifted and not
# --------------------------------------------------------------------------

def command_explore(args: argparse.Namespace) -> int:
    """Have a generated game play itself, and keep what it recorded.

    Many short runs at once, each from a moment an earlier one reached, with
    buttons pressed at random; a run that found something new leaves its last
    moment for others to go on from (tools/gb_lift_explore.py). What comes
    out is one evidence file, the same as a played session writes."""
    import gb_lift_explore as explorer

    if args.minutes is None and args.runs is None:
        args.minutes = 10.0
    workers = args.workers or max(1, (os.cpu_count() or 2) - 2)
    starts: set[tuple[int, int]] | None = None
    if args.export:
        import gb_lift_edges as far

        export = load_export(args.export)
        starts = {(function["bank"], function["address"]) for function in export["functions"]}
        # Where a function's own JP HL comes back is that function's.
        starts |= far.jump_returns(export)

    def numbers(exploration) -> str:
        text = (f"{len(exploration.entered):,} places arrived at, {len(exploration.jumps):,} jumps, "
                f"{len(exploration.screens):,} screens")
        if starts is not None:
            outside = sum(1 for place in exploration.entered if place[1] < 0x8000 and place not in starts)
            text += f"; {outside:,} arrivals are not the start of a function in the export"
        return text

    def how_far(exploration) -> str:
        if not exploration.goals:
            return ""
        furthest = exploration.furthest()
        name = exploration.goals.names[furthest - 1] if furthest else "none"
        return f"; checkpoint {furthest} of {len(exploration.goals.names)} passed ({name})"

    def report(exploration, seconds: float) -> None:
        deepest = max((moment["generation"] for moment in exploration.moments), default=0)
        print(f"{seconds / 60:6.1f} min  {exploration.runs:,} runs, {exploration.frames / 3600:,.0f} minutes of the "
              f"game: {numbers(exploration)}; {len(exploration.moments):,} moments kept, "
              f"{deepest} generations deep{how_far(exploration)}", flush=True)
        back = exploration.falling_back()
        if back:
            name = exploration.goals.names[back[0] - 1] if back[0] else "before the first"
            print(f"            {back[1]}% of the runs from there end back at checkpoint {back[0]} ({name}): "
                  f"if that is not where they are, a room on the way is missing from the list", flush=True)

    work = Path(args.output)
    hold: dict[int, int] = {}
    for item in args.hold:
        try:
            address, value = (int(part, 16) for part in item.split("="))
        except ValueError:
            raise LiftError(f"--hold takes ADDRESS=VALUE in hexadecimal, not '{item}'") from None
        if not (0xC000 <= address < 0xE000 or 0xFF80 <= address < 0xFFFF) or not 0 <= value <= 0xFF:
            raise LiftError(f"--hold {item}: a byte of work RAM or high RAM, and a value it can hold")
        hold[address] = value
    try:
        goals = explorer.Goals(Path(args.goals)) if args.goals else None
        exploration = explorer.explore(Path(args.game), work, args.minutes, args.runs, workers, args.seed,
                                       [Path(state) for state in args.state], args.fresh, report, goals,
                                       not args.no_screens, not args.no_memory, args.keep,
                                       not args.states_only, hold)
    except explorer.ExploreError as error:
        raise LiftError(str(error)) from error
    except KeyboardInterrupt:
        print(f"stopped; what was found so far is in {work}", file=sys.stderr)
        return 130
    print(f"{exploration.runs:,} runs, {exploration.frames / 216000:,.1f} hours of the game: "
          f"{numbers(exploration)}{how_far(exploration)}")
    for number, state in sorted(exploration.passed.items(), key=lambda item: int(item[0])):
        print(f"  past checkpoint {number}, {exploration.goals.names[int(number) - 1] if exploration.goals else ''}: "
              f"{work / state}")
    if exploration.failed:
        print(f"  {exploration.failed:,} runs failed; the last said: {exploration.last_failure.strip()[-200:]}")
    if exploration.dropped:
        print(f"  {exploration.dropped:,} jumps did not fit in a run's record and were left out")
    print(f"wrote {work / 'evidence.json'}: pass it to `gb_lift.py edges` with --evidence; "
          f"run this again with the same -o to go on from here")
    return 0


def command_coverage(args: argparse.Namespace) -> int:
    """Count a `--trace-entries` file by where each dispatch landed.

    The game logs every address its dispatcher was sent to. One that is the
    start of a lifted function ran that function's C. One inside a lifted
    function ran the recompiled code there, which an edit to the C does not
    change. The rest is in functions that are not lifted."""
    export = load_export(args.export)
    package = json.loads(Path(args.package).read_text(encoding="utf-8"))
    if package.get("schema") != PACKAGE_SCHEMA:
        raise LiftError(f"{args.package} is not a {PACKAGE_SCHEMA} file")
    if package["rom"]["sha256"] != export["rom"]["sha256"]:
        raise LiftError("the package and the export are of different ROMs")
    lifted = {entry["id"] for entry in package["functions"]}
    reasons: dict[str, dict] = {}
    if args.report:
        report = json.loads(Path(args.report).read_text(encoding="utf-8"))
        reasons = {entry["function"]: entry for entry in report["results"]}

    owner: dict[tuple[int, int], dict] = {}
    for function in export["functions"]:
        for block in function["blocks"]:
            for instruction in block["instructions"]:
                address = instruction["address"]
                owner.setdefault((function["bank"] if address >= 0x4000 else 0, address), function)

    counts: dict[tuple[int, int], int] = {}
    for path in args.trace:
        with open(path, encoding="ascii", errors="replace") as trace:
            for line in trace:
                bank, _, address = line.strip().partition(":")
                try:
                    where = (int(bank), int(address, 16))
                except ValueError:
                    continue
                if where[1] < 0x4000:
                    where = (0, where[1])
                counts[where] = counts.get(where, 0) + 1
    total = sum(counts.values())
    if not total:
        raise LiftError("the trace has no dispatches in it: run the game with --trace-entries FILE")

    starts = 0
    inside: dict[str, dict[int, int]] = {}
    plain: dict[str, int] = {}
    nowhere = 0
    for where, count in counts.items():
        function = owner.get(where)
        if function is None:
            nowhere += count
        elif function["id"] not in lifted:
            plain[function["id"]] = plain.get(function["id"], 0) + count
        elif where[1] == function["address"]:
            starts += count
        else:
            inside.setdefault(function["id"], {})[where[1]] = count

    index = function_index(export)

    def share(count: int) -> str:
        return f"{count:>10,} ({100 * count / total:4.1f}%)"

    inside_total = sum(sum(spots.values()) for spots in inside.values())
    print(f"{total:,} dispatches at {len(counts):,} addresses")
    print(f"{share(starts)} at the start of a lifted function: its C ran")
    print(f"{share(inside_total)} inside a lifted function, {len(inside)} of them: the recompiled code ran "
          "there, and an edit to the C is not what the game did")
    for function_id, spots in sorted(inside.items(), key=lambda item: -sum(item[1].values()))[:args.top]:
        function = index[function_id]
        where = ", ".join(f"{address:04X} x{count}" for address, count in
                          sorted(spots.items(), key=lambda item: -item[1])[:3])
        print(f"{'':>19}{sum(spots.values()):>9,}  {display_name(function)} "
              f"({function['bank']:02X}:{function['address']:04X}) at {where}")
    print(f"{share(sum(plain.values()))} in {len(plain)} functions that are not lifted")
    for function_id, count in sorted(plain.items(), key=lambda item: -item[1])[:args.top]:
        function = index[function_id]
        entry = reasons.get(function_id)
        why = ""
        if entry and entry["verdict"] != "match":
            because = ", ".join(f"{number} {reason}" for reason, number in entry["skipped_because"].items())
            why = f"  check: {entry['verdict']}" + (f" ({because})" if because else "")
        elif entry is None and reasons:
            why = "  not checked"
        print(f"{'':>19}{count:>9,}  {display_name(function)} "
              f"({function['bank']:02X}:{function['address']:04X}) [{function['kind']}]{why}")
    print(f"{share(nowhere)} in no function of the export: code in RAM, or not decoded")
    if args.json:
        document = {
            "schema": "gbrecomp.lift-coverage", "version": 1, "dispatches": total,
            "lifted_starts": starts,
            "inside_lifted": [{"function": function_id, "at": [{"address": address, "count": count}
                                                               for address, count in sorted(spots.items())]}
                              for function_id, spots in sorted(inside.items())],
            "not_lifted": [{"function": function_id, "count": count} for function_id, count in sorted(plain.items())],
            "in_no_function": nowhere,
        }
        Path(args.json).write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8", newline="\n")
    return 0


# --------------------------------------------------------------------------
# package: lifted source for `gbrecomp --lift`
# --------------------------------------------------------------------------

PACKAGE_SCHEMA = "gbrecomp.lift-package"


def command_edges(args: argparse.Namespace) -> int:
    import gb_lift_edges as far

    export = load_export(args.export)
    index = function_index(export)
    rom = Path(args.rom).resolve()
    if not rom.is_file():
        raise LiftError(f"no ROM at {rom}")
    sites = far.find_sites(export)
    work = Path(args.work or (REPO / "output" / "lift-edges")).resolve()
    work.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    try:
        probe = far.build_probe(find_compiler(args.cc), CHECKER_DIR, LIFT_INCLUDE, work)
        def run(requests: list) -> list[str]:
            return far.run_probes(probe, rom, requests, args.trials, args.max_steps)

        outcomes = run([(site, site["known"]) for site in sites])
        unprompted = run([(site, {}) for site in sites])
        bases = far.table_bases(sites, outcomes, run)
    except far.EdgesError as error:
        raise LiftError(str(error)) from error
    rom_bytes = rom.read_bytes()
    try:
        declared = far.load_tables(args.tables, rom_bytes)
    except far.EdgesError as error:
        raise LiftError(str(error)) from error
    lengths = {(table["bank"], table["address"]): table["entries"] for table in declared if table["plain"]}
    unread: list = []
    edges, counts = far.resolve(export, sites, outcomes, unprompted, bases, rom_bytes, lengths, unread)
    # A declared table a helper is seen to call through is listed on the
    # calls that go through it, like one the ROM marks the end of.
    used = {(edge["table"]["bank"], edge["table"]["address"]) for edge in edges if "table" in edge}
    apart = [table for table in declared if (table["bank"], table["address"]) not in used]
    named = sorted({(bank, address) for table in apart for _, bank, address in table["functions"]})
    entered: set = set()
    seen_counts = None
    if args.evidence:
        try:
            transfers, entered = far.load_evidence(args.evidence, rom_bytes)
        except far.EdgesError as error:
            raise LiftError(str(error)) from error
        seen, seen_counts = far.evidence_edges(export, transfers, edges)
        edges += seen
    groups = far.classify_targets(export, edges, named)

    helpers = {site["helper"] for site in sites}
    print(f"{len(sites)} calls and jumps, with a constant in some register, to {len(helpers)} functions "
          f"the export cannot follow ({time.monotonic() - started:.1f} s)")
    targets = sum(len(group) for group in groups.values())
    single = [edge for edge in edges if "table" not in edge and not edge.get("seen")]
    print(f"  {len(single)} go to one place whatever else is in the machine")
    tables = {(edge["table"]["bank"], edge["table"]["address"]) for edge in edges if "table" in edge}
    table_sites = {(edge["from"], edge["site"]) for edge in edges if "table" in edge}
    if tables:
        entries = {(edge["table"]["bank"], edge["table"]["address"], edge["table"]["index"])
                   for edge in edges if "table" in edge}
        print(f"  {len(table_sites)} go through a table that ends where code begins"
              f"{', or is as long as somebody says' if counts.get('table as long as somebody says') else ''}: "
              f"{len(tables)} tables, {len(entries)} entries")
    starts = {(function["bank"], function["address"]) for function in export["functions"]}
    if declared:
        print(f"  {len(declared)} tables somebody read the code for name {len(named)} places"
              + (f" ({len(declared) - len(apart)} of them a call above goes through)" if len(apart) < len(declared)
                 else ""))
        for table in sorted(apart, key=lambda table: -len(table["functions"]))[:12]:
            places = {(bank, address) for _, bank, address in table["functions"]}
            here = len(places & starts)
            print(f"    {table['where']}: {len(places)} places"
                  + (f", {here} of them functions in this export" if places else ""))
            if len(places) >= 8 and here * 8 < len(places):
                # Real tables name a good many functions something else found too.
                print("      hardly any of them known: is it where, and the shape, it is said to be?")
    if seen_counts:
        ran = sum(1 for function in export["functions"] if (function["bank"], function["address"]) in entered)
        print(f"  the game was seen to make {seen_counts['seen']} jumps its code does not spell out: "
              f"{seen_counts['agree']} as found above, {seen_counts['added']} more")
        if seen_counts["disagree"]:
            print(f"    {seen_counts['disagree']} of them from a call found above to go to one place, "
                  "to another place: look at those")
        if seen_counts["elsewhere"]:
            print(f"    {seen_counts['elsewhere']} went to RAM or to nowhere in the ROM, and are left out")
        if seen_counts["came back"]:
            print(f"    {seen_counts['came back']} were a RET to where a function's own JP HL comes back: "
                  "the call it makes by hand returning, and left out")
        print(f"    {ran} of this export's {len(export['functions'])} functions were entered in those runs")
    # Where a call or an interrupt arrived is the start of a function. One
    # that is not a function here is in the middle of one, or not decoded.
    # Not where a function's own JP HL comes back: its callee arrives there
    # every time it returns, and the export keeps the place the function's.
    came_back = far.jump_returns(export)
    arrived = sorted(where for where in entered if where not in starts and where not in came_back)
    if arrived:
        shown = ", ".join(f"{bank:02X}:{address:04X}" for bank, address in arrived[:6])
        what = ("1 place a call arrived at is" if len(arrived) == 1
                else f"{len(arrived)} places a call arrived at are")
        print(f"    {what} not the start of a function in this export "
              f"({shown}{', ...' if len(arrived) > 6 else ''}): with these edges, the next export has "
              f"{'it' if len(arrived) == 1 else 'them'}")
    print(f"  {targets} different places in all")
    by_helper: dict[str, set] = {}
    for edge in edges:
        if edge.get("helper") and not edge.get("seen"):
            by_helper.setdefault(edge["helper"], set()).add((edge["bank"], edge["address"]))
    for helper, reached in sorted(by_helper.items(), key=lambda item: -len(item[1]))[:8]:
        calls = len({(edge["from"], edge["site"]) for edge in edges
                     if edge.get("helper") == helper and not edge.get("seen")})
        print(f"    through {display_name(index[helper])} ({helper[-9:].upper()}): "
              f"{calls} calls to {len(reached)} places")
    for kind, count in sorted(counts.items(), key=lambda item: -item[1]):
        if kind not in ("to", "table"):
            print(f"  {count} {OUTCOME_WORDS.get(kind, kind)}")
    print(f"  of the places reached: {len(groups['function'])} are functions in this export, "
          f"{len(groups['inside'])} are inside another function, {len(groups['undecoded'])} were not decoded")
    print(f"  reached from the entry point by what the export lists: {far.reachable(export)} "
          f"of {len(export['functions'])} functions")
    if args.unread:
        # A tables file to finish: where each unread table is, and a call that goes through it. How
        # many entries each has is for whoever reads that call to say.
        skeleton = {"schema": far.TABLES_SCHEMA, "version": 1, "tables": [
            {"bank": table["bank"], "address": table["address"], "entries": 0,
             "why": f"called through from {display_name(index[table['from']])} at {table['site']:04X}"}
            for table in sorted(unread, key=lambda table: (table["bank"], table["address"]))]}
        args.unread.parent.mkdir(parents=True, exist_ok=True)
        args.unread.write_text(json.dumps(skeleton, indent=1) + "\n", encoding="utf-8")
        print(f"  wrote the {len(unread)} tables not read to {args.unread}: say how many entries each has "
              f"and hand it back with --tables")
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(far.edges_document(export, edges, entered, apart), indent=1) + "\n",
                          encoding="utf-8")
        # What the export already has: found before and handed back to it.
        have = set()
        for function in export["functions"]:
            for site in function["calls"] + function["jumps_out"]:
                if "reaches" in site:
                    have.add((site["site"], site["reaches"]["bank"], site["reaches"]["address"]))
                for entry in site.get("reaches_table", {}).get("entries", ()):
                    have.add((site["site"], entry["bank"], entry["address"]))
                for entry in site.get("reaches_seen", ()):
                    have.add((site["site"], entry["bank"], entry["address"]))
            for jump in function.get("seen_jumps", ()):
                for entry in jump["targets"]:
                    have.add((jump["site"], entry["bank"], entry["address"]))
        # A jump seen in code the export does not have has nowhere to be
        # listed; it is in the export once its target is a function.
        entries = {(function["bank"], function["address"]) for function in export["functions"]}
        for edge in edges:
            if edge["from"] is None and (edge["bank"], edge["address"]) in entries:
                have.add((edge["site"], edge["bank"], edge["address"]))
        new = sum(1 for edge in edges if (edge["site"], edge["bank"], edge["address"]) not in have)
        new += len(arrived)
        # A function a declared table names is in the export once it is one of its functions.
        new += sum(1 for place in named if place not in starts)
        again = (f"; {new} of the calls are not in this export, so export again with it and run this again"
                 if new else "; this export has every one of them")
        print(f"wrote {output}: pass it to gbrecomp with --lift-edges{again}")
    return 0


OUTCOME_WORDS = {
    "returns": "come back without leaving for anywhere the code does not name",
    "varies": "go to different places from different states: the constants do not decide it",
    "timeout": "do not get anywhere in the instructions allowed",
    "invalid_opcode": "run into a byte that is no instruction",
    "to RAM": "go to code in RAM",
    "table whose end the ROM does not mark": "go through a table that does not end where code is known to "
                                             "begin: not read",
    "table as long as somebody says": "go through a table whose end the ROM does not mark, and somebody "
                                      "says how long it is",
    "table that does not start at a constant of the call": "go through a table whose start is not a constant "
                                                           "of the call",
    "to a bank the ROM does not have": "go to a bank the ROM does not have",
    "same without the constants": "go to one place with or without the constants: the function called "
                                  "decides, and is found where it does",
    "log_overflow": "write to the hardware without end",
    "another_bank_mapped": "come back to banked code with another bank mapped",
    "bank_switched_under_code": "map another bank over the code that is running",
    "bad_return": "return with the stack out of place",
}


def command_package(args: argparse.Namespace) -> int:
    export = load_export(args.export)
    index = function_index(export)

    sources: list[Path] = []
    for item in args.source:
        item_path = Path(item)
        sources += sorted(item_path.glob("*.c")) + sorted(item_path.glob("*.h")) if item_path.is_dir() else [item_path]
    sources = [source.resolve() for source in sources]
    names = [source.name for source in sources]
    if len(set(names)) != len(names):
        raise LiftError("two source files have the same name; a package holds them in one folder")
    declared = scan_sources([source for source in sources if source.suffix == ".c"])
    if not declared:
        raise LiftError("no GBLIFT_FUNCTION(bank, address, name) found in the source given")

    verdicts: dict[str, str] | None = None
    if args.report:
        verdicts = {}
        for report_path in args.report:
            report = json.loads(Path(report_path).read_text(encoding="utf-8"))
            if report.get("rom_sha256") != export["rom"]["sha256"]:
                raise LiftError(f"{report_path} is a check of another ROM")
            for entry in report["results"]:
                previous = verdicts.get(entry["function"])
                if entry["verdict"] == "mismatch" or previous == "mismatch":
                    verdicts[entry["function"]] = "mismatch"
                elif entry["verdict"] == "match" or previous == "match":
                    verdicts[entry["function"]] = "match"
                else:
                    verdicts[entry["function"]] = "inconclusive"
    excluded_hazards = set(args.exclude_hazard)
    only = {parse_function_ref(reference) for reference in args.function} if args.function else None

    chosen = []
    left_out: dict[str, int] = {}

    def leave_out(reason: str) -> None:
        left_out[reason] = left_out.get(reason, 0) + 1

    for bank, address, name, source in declared:
        function_id = f"gbfn:v1:{bank:04x}:{address:04x}"
        function = index.get(function_id)
        if function is None:
            raise LiftError(f"{source.name}: {name} claims {bank:02X}:{address:04X}, "
                            "which is not a function in the export")
        if only is not None and function_id not in only:
            leave_out("not asked for")
        elif any(hazard in function["hazards"] for hazard in UNLIFTABLE):
            leave_out("cannot be lifted")
        elif excluded_hazards & set(function["hazards"]):
            leave_out("has an excluded hazard")
        elif verdicts is not None and verdicts.get(function_id) != "match":
            leave_out(f"check verdict: {verdicts.get(function_id, 'not checked')}")
        else:
            chosen.append((function_id, name))
    if not chosen:
        raise LiftError("no function is left to package")

    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    for source in sources:
        target = out_dir / source.name
        if target.resolve() != source:
            shutil.copyfile(source, target)
    manifest = {
        "schema": PACKAGE_SCHEMA,
        "version": 1,
        "rom": {"sha256": export["rom"]["sha256"], "size": export["rom"]["size"]},
        "sources": sorted(names),
        "functions": [{"id": function_id, "symbol": name} for function_id, name in sorted(chosen)],
    }
    manifest_path = out_dir / "lift.json"
    manifest_path.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"packaged {len(chosen)} functions from {len(sources)} files: {manifest_path}")
    for reason, count in sorted(left_out.items(), key=lambda item: -item[1]):
        print(f"  left out {count}: {reason}")
    print(f"build it into the game with: gbrecomp ROM --lift {manifest_path} -o PROJECT")
    return 0


# --------------------------------------------------------------------------
# source / build: the structured C as a folder somebody works in
# --------------------------------------------------------------------------

def command_source(args: argparse.Namespace) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gb_lift_source

    return gb_lift_source.command_source(args, sys.modules[__name__])


def command_signatures(args: argparse.Namespace) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gb_lift_signatures

    return gb_lift_signatures.command_signatures(args, sys.modules[__name__])


def command_convert(args: argparse.Namespace) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gb_lift_convert

    return gb_lift_convert.command_convert(args, sys.modules[__name__])


def command_build(args: argparse.Namespace) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gb_lift_source

    return gb_lift_source.command_build(args, sys.modules[__name__])


# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    summary = commands.add_parser("summary", help="what an export holds")
    summary.add_argument("export", type=Path)
    summary.set_defaults(run=command_summary)

    function = commands.add_parser("function", help="one function's dossier")
    function.add_argument("export", type=Path)
    function.add_argument("function", help="BB:AAAA in hexadecimal, or a gbfn:v1 ID")
    function.set_defaults(run=command_function)

    baseline = commands.add_parser("baseline", help="write a mechanical C version of functions")
    baseline.add_argument("export", type=Path)
    baseline.add_argument("-o", "--output", required=True, help="directory for lifted_bank_NN.c")
    baseline.add_argument("--function", action="append", default=[], help="BB:AAAA; repeatable")
    baseline.add_argument("--bank", type=lambda text: int(text, 16), help="every function of this bank (hexadecimal)")
    baseline.add_argument("--all", action="store_true", help="every function")
    baseline.add_argument("--clean-only", action="store_true", help="with --all or --bank: only functions with no hazard")
    baseline.add_argument("--style", choices=("mechanical", "structured"), default="mechanical",
                          help="mechanical: one statement group per instruction and a goto per jump, "
                               "cycle-exact (default). structured: dead flags dropped, compares folded "
                               "into conditions, if/do/for where the layout allows, names from symbols")
    baseline.add_argument("--time", choices=("block", "exact"), default="block",
                          help="structured style: pass time once per straight run of statements "
                               "(block, default) or after every instruction (exact)")
    baseline.set_defaults(run=command_baseline)

    package = commands.add_parser(
        "package", help="gather lifted C into a package `gbrecomp --lift` builds into the game")
    package.add_argument("export", type=Path)
    package.add_argument("--source", action="append", required=True,
                         help="a lifted .c file, or a directory of them; repeatable")
    package.add_argument("-o", "--output", required=True, help="directory for the package")
    package.add_argument("--report", action="append", default=[],
                         help="a `check --json` report: package only functions that matched; repeatable, "
                              "a function matches if it does in any and mismatches in none")
    package.add_argument("--function", action="append", default=[], help="package only BB:AAAA; repeatable")
    package.add_argument("--exclude-hazard", action="append", default=[],
                         help="leave out functions with this hazard (see LIFTING.md); repeatable")
    package.set_defaults(run=command_package)

    image = commands.add_parser("image", help="turn a save state into a memory image")
    image.add_argument("savestate", type=Path)
    image.add_argument("-o", "--output", required=True)
    image.add_argument("--rom-bank", type=lambda text: int(text, 16), default=0,
                       help="ROM bank to map for bank-0 functions (hexadecimal; default: vary it)")
    image.set_defaults(run=command_image)

    check = commands.add_parser("check", help="compare lifted C with the ROM")
    check.add_argument("export", type=Path)
    check.add_argument("--rom", required=True)
    check.add_argument("--source", action="append", required=True,
                       help="a lifted .c file, or a directory of them; repeatable")
    check.add_argument("--support", action="append", default=[],
                       help="a .c file the lifted functions call into that declares none itself; repeatable")
    check.add_argument("--include", action="append", default=[],
                       help="a directory the source's own #include lines are also looked up in; repeatable")
    check.add_argument("--function", action="append", default=[], help="check only BB:AAAA; repeatable")
    check.add_argument("--trials", type=int, default=500, help="machine states per function (default 500)")
    check.add_argument("--seed", type=int, default=1)
    check.add_argument("--max-steps", type=int, default=200000,
                       help="instructions the original may run in one state (default 200000)")
    check.add_argument("--image", action="append", default=[],
                       help="also start states from this image of the game's memory (see `image`); repeatable")
    check.add_argument("--entries", action="append", default=[],
                       help="also start states from the states a game entered its functions in (a file a "
                            "game wrote with --entry-states); repeatable")
    check.add_argument("--images-only", action="store_true",
                       help="with --image or --entries: leave out the states made up from nothing")
    check.add_argument("--ignore", help="registers and flags not compared, for every function: "
                                        "a b c d e h l fz fn fh fc ime, comma separated")
    check.add_argument("--contract", help='JSON: {"functions": {"BB:AAAA": {"ignore": ["fh", ...]}}}; with '
                                          '"takes", "gives" and "keeps" a function is held to that signature')
    check.add_argument("--observe", action="store_true",
                       help="also report, for each function, the registers and flags it reads and the ones it "
                            "changes (`observed` in the report)")
    check.add_argument("--observe-states", type=int, default=160,
                       help="with --observe: from this many states of each kind at most (default 160; 0: all)")
    check.add_argument("--strict-stack", action="store_true",
                       help="also compare the bytes below the stack pointer")
    check.add_argument("--work", help="build directory (default output/lift-check)")
    check.add_argument("--json", help="write the full report here")
    check.add_argument("--cc", help="C compiler (default: $CC, then cc, gcc, clang, cl)")
    check.add_argument("--no-optimize", action="store_true", help="compile without optimization (faster build)")
    check.add_argument("--timeout", type=int, default=1800, help="seconds the checker may run (default 1800)")
    check.add_argument("-j", "--jobs", type=int, default=0, help="worker processes (default: one per processor)")
    check.add_argument("-v", "--verbose", action="store_true")
    check.set_defaults(run=command_check)

    entries = commands.add_parser(
        "entries", help="list the functions for a game's --entry-states-of; say what the states a game kept cover")
    entries.add_argument("export", type=Path)
    entries.add_argument("-o", "--output", help="write the list of functions here, one BANK:ADDRESS a line")
    entries.add_argument("--report", help="a `check --json` report: with -o, list only the functions it does not "
                                          "call a match; with --states, count by its verdicts")
    entries.add_argument("--states", action="append", default=[], metavar="FILE",
                         help="a file a game wrote with --entry-states; repeatable")
    entries.add_argument("--json", help="with --states: write the counts and the functions never arrived at here")
    entries.set_defaults(run=command_entries)

    executed = commands.add_parser(
        "executed", help="sort the functions by what a played game did with their bytes: executed, read, neither")
    executed.add_argument("export", type=Path)
    executed.add_argument("--map", action="append", required=True, metavar="FILE",
                          help="a map a game wrote with --executed; repeatable, and what any of them has counts")
    executed.add_argument("--data-use", action="append", default=[], metavar="RECORD",
                          help="a record a game wrote with --data-use, for what was read as data; repeatable")
    executed.add_argument("--verdicts", metavar="FILE",
                          help="a `check --json` report or a source folder's lift-source.json: count by verdict too")
    executed.add_argument("--json", help="write the counts and every function's class here")
    executed.set_defaults(run=command_executed)

    edges = commands.add_parser(
        "edges", help="find calls made through helpers that take their target in registers")
    edges.add_argument("export", type=Path)
    edges.add_argument("--rom", required=True)
    edges.add_argument("-o", "--output", help="write the calls found here, for `gbrecomp --lift-edges`")
    edges.add_argument("--trials", type=int, default=6,
                       help="states each call is tried from; all must agree (default 6)")
    edges.add_argument("--max-steps", type=int, default=20000,
                       help="instructions a helper may run before it leaves (default 20000)")
    edges.add_argument("--evidence", action="append", default=[], type=Path,
                       help="what a run of the game made with `--evidence FILE` recorded; repeatable")
    edges.add_argument("--tables", action="append", default=[], type=Path,
                       help="tables of functions somebody has read the code for (a gbrecomp.lift-tables file): "
                            "where each is, how long, and where in an entry the address sits; repeatable")
    edges.add_argument("--unread", type=Path,
                       help="write the tables a helper calls through whose end the ROM does not mark, as a "
                            "tables file to finish: 'entries' is for whoever reads the call to fill in")
    edges.add_argument("--work", help="build directory (default output/lift-edges)")
    edges.add_argument("--cc", help="C compiler (default: $CC, then cc, gcc, clang, cl)")
    edges.set_defaults(run=command_edges)

    score = commands.add_parser("score", help="mark the export against a disassembly of the same ROM")
    score.add_argument("export", type=Path)
    score.add_argument("--sym", required=True, type=Path, help="the disassembly's RGBDS .sym file")
    score.add_argument("--source", type=Path,
                       help="the disassembly's source tree, to tell code labels from data labels")
    score.add_argument("--json", help="write the numbers here")
    score.set_defaults(run=command_score)

    explore = commands.add_parser(
        "explore", help="have a generated game play itself, for the evidence a played session would give")
    explore.add_argument("game", type=Path, help="the generated game's executable")
    explore.add_argument("-o", "--output", required=True, type=Path,
                         help="folder for evidence.json and the moments kept; an exploration already there is gone on with")
    explore.add_argument("--minutes", type=float, help="how long to explore (default 10, unless --runs is given)")
    explore.add_argument("--runs", type=int, help="stop after this many runs")
    explore.add_argument("--workers", type=int, help="copies of the game at once (default: all processors but two)")
    explore.add_argument("--state", action="append", default=[], type=Path,
                         help="a save state to start from as well as power-on, such as one made while playing; repeatable")
    explore.add_argument("--goals", type=Path,
                         help="checkpoints in the order a player meets them, as conditions on the game's memory: "
                              "most runs start from the moments furthest along, and the first moment past each is "
                              "kept in checkpoints/")
    explore.add_argument("--states-only", action="store_true",
                         help="start from the states given and not from power-on as well")
    explore.add_argument("--hold", action="append", default=[], metavar="ADDRESS=VALUE",
                         help="a byte of the game's memory to set at the start of every run, in hexadecimal: "
                              "something that must not run out while the search is looking elsewhere; repeatable")
    explore.add_argument("--export", type=Path,
                         help="the export, to say how many arrivals are not yet the start of a function")
    explore.add_argument("--keep", type=int, default=20000,
                         help="moments to keep at most, a quarter of a megabyte each (default 20000)")
    explore.add_argument("--fresh", action="store_true", help="forget an exploration already in the folder")
    explore.add_argument("--seed", type=int, default=1, help="for the random choices (default 1)")
    explore.add_argument("--no-screens", action="store_true",
                         help="do not count a screen no run has ended on as something found")
    explore.add_argument("--no-memory", action="store_true",
                         help="do not count a new value in a byte of the game's memory as something found")
    explore.set_defaults(run=command_explore)

    coverage = commands.add_parser(
        "coverage", help="what a run of the game ran: lifted C, recompiled code inside a lifted function, the rest")
    coverage.add_argument("export", type=Path)
    coverage.add_argument("--trace", action="append", required=True, type=Path,
                          help="what the game wrote with `--trace-entries FILE`; repeatable")
    coverage.add_argument("--package", required=True, type=Path,
                          help="the lift.json built into that game: what is lifted")
    coverage.add_argument("--report", type=Path, help="a `check --json` report, to say why a function is not lifted")
    coverage.add_argument("--top", type=int, default=12, help="functions to list under each heading (default 12)")
    coverage.add_argument("--json", help="write the counts here")
    coverage.set_defaults(run=command_coverage)

    source = commands.add_parser(
        "source", help="make a source folder of the structured C, or bring one up to date with an export",
        epilog="After `--` come the analysis options the export was made with (--pointer-scan, --lift-edges, "
               "--annotations, ...): `build` generates the game with them.")
    source.add_argument("export", type=Path)
    source.add_argument("-o", "--output", required=True,
                        help="the folder: a new one, or one this command made, which is then brought up to date")
    source.add_argument("--symbols", type=Path,
                        help="an RGBDS .sym file whose names go into the folder's names.sym, where that has none")
    source.add_argument("--time", choices=("exact", "block"),
                        help="how the source keeps time (see `baseline`); exact for a new folder, so that the game "
                             "built from it ends as the original does")
    source.add_argument("--rom", help="the ROM, for `build` to find later")
    source.add_argument("--data-tree", metavar="DIR",
                        help="a data tree of the same ROM on which `gb_data.py pointers` has been run: the "
                             "operands its records saw leading to something in the ROM go by a name in the "
                             "source, and what they lead to can then be moved (remembered for later runs)")
    source.add_argument("--restore", action="append", default=[], metavar="BB:AAAA",
                        help="write this function afresh though somebody changed it or took it out; repeatable")
    source.add_argument("--sdl2-dir", help="SDL2's CMake folder, for a new folder's CMakePresets.json "
                                           "(default: the SDL2_DIR environment variable)")
    source.set_defaults(run=command_source, analysis=None)

    build = commands.add_parser(
        "build", help="from a source folder to a game: check what changed against the ROM, generate, build")
    build.add_argument("folder", type=Path, help="a folder `source` made")
    build.add_argument("--rom", help="the ROM (default: the one `source` or an earlier `build` was given)")
    build.add_argument("--gbrecomp", help="the recompiler (default: build/bin/gbrecomp of this repository)")
    build.add_argument("--entries", action="append", default=[],
                       help="also check from the states a game entered its functions in (a file a game wrote "
                            "with --entry-states), as every *.entries in the folder's entries/ is; repeatable")
    build.add_argument("--image", action="append", default=[],
                       help="also check from this image of the game's memory (see `image`), as every *.img in "
                            "the folder's images/ is; repeatable")
    build.add_argument("--trials", type=int, default=500, help="machine states per function (default 500)")
    build.add_argument("--seed", type=int, default=1)
    build.add_argument("--recheck", action="store_true", help="check every function, not only what changed")
    build.add_argument("--check-only", action="store_true", help="stop after the check")
    build.add_argument("--regenerate", action="store_true", help="have gbrecomp generate the game again")
    build.add_argument("--no-compile", action="store_true", help="stop once the game is generated")
    build.add_argument("--preset", help="the preset of CMakePresets.json to build "
                                        "(default: x64-release on Windows, release elsewhere)")
    build.add_argument("--cmake-arg", action="append", default=[], help="one more argument for cmake; repeatable")
    build.add_argument("--cc", help="C compiler for the checker (default: $CC, then cc, gcc, clang, cl)")
    build.add_argument("-j", "--jobs", type=int, default=0, help="checker processes (default: one per processor)")
    build.set_defaults(run=command_build)

    signatures = commands.add_parser(
        "signatures", help="work out what each function of a source folder takes, gives and keeps, and hold "
                           "every one to it")
    signatures.add_argument("folder", type=Path, help="a folder `source` made")
    signatures.add_argument("--rom", help="the ROM (default: the one `source` or an earlier `build` was given)")
    signatures.add_argument("--trials", type=int, default=500, help="machine states per function (default 500)")
    signatures.add_argument("--seed", type=int, default=1)
    signatures.add_argument("--observed", help="a report of `check --observe` to work from, in place of running one")
    signatures.add_argument("--callers", action="append", default=[], metavar="PATH",
                            help="a file a game run with --lift-callers wrote, or a folder of them, besides "
                                 "what is in the folder's callers/; repeatable")
    signatures.add_argument("--restore", action="append", default=[], metavar="BB:AAAA",
                            help="write the tool's line for this function again, changed or taken out; repeatable")
    signatures.add_argument("-j", "--jobs", type=int, default=0, help="checker processes (default: one per processor)")
    signatures.set_defaults(run=command_signatures)

    convert = commands.add_parser(
        "convert", help="write the functions of a source folder as C functions with arguments, from what "
                        "signatures.txt says each takes, gives and keeps, and hold every one to the ROM's code")
    convert.add_argument("folder", type=Path, help="a folder `source` made, built, with a signatures.txt")
    convert.add_argument("-o", "--output", help="where to write (default: .lift/convert in the folder)")
    convert.add_argument("--rom", help="the ROM (default: the one `source` or an earlier `build` was given)")
    convert.add_argument("--function", action="append", default=[], help="convert only BB:AAAA; repeatable")
    convert.add_argument("--trials", type=int, default=500, help="machine states per function (default 500)")
    convert.add_argument("--seed", type=int, default=1)
    convert.add_argument("--no-check", action="store_true", help="write the C and do not run the checker on it")
    convert.add_argument("--write", action="store_true",
                         help="put the converted functions that match into the folder's src/, each where its "
                              "lifted function stands: they are then functions somebody changed, which `source` "
                              "keeps and `build` checks and builds into the game. Keep a copy of the folder: "
                              "the converted functions call each other, so they do not come out one at a time")
    convert.add_argument("--machine-stack", action="store_true",
                         help="keep every function's pushes and calls on the machine's stack, and write none as C "
                              "that keeps what it saves in locals and is called from C")
    convert.add_argument("--cc", help="C compiler for the checker (default: $CC, then cc, gcc, clang, cl)")
    convert.add_argument("-j", "--jobs", type=int, default=0, help="checker processes (default: one per processor)")
    convert.set_defaults(run=command_convert)
    return parser


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    # `source EXPORT -o FOLDER -- OPTIONS`: what follows `--` is gbrecomp's.
    rest: list[str] | None = None
    if "--" in argv:
        argv, rest = argv[:argv.index("--")], argv[argv.index("--") + 1:]
    parser = build_parser()
    args = parser.parse_args(argv)
    if rest is not None:
        if not hasattr(args, "analysis"):
            parser.error("only `source` takes options after `--`")
        args.analysis = rest
    try:
        return args.run(args)
    except LiftError as error:
        print(f"gb_lift: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
