#!/usr/bin/env python3
"""Lifted C as a source folder: made from an export, kept, edited and rebuilt.

`gb_lift.py baseline` writes whole files, so running it again loses every
edit. This is the folder a person works in instead (LIFTING.md, "Working in
the source"):

  src/lifted_bank_NN.c  the structured C, one file a bank; yours once written
  src/lifted_names.h    names for fixed addresses; the tool's, written again
  src/lifted_data.h     names for what operands lead to in the ROM (with
                        --data-tree); the tool's, written again
  src/lifted_moved.h    where those names lead in the image the game is to run:
                        `gb_data.py build --moved-header` writes it
  src/**                any other C of yours, which the functions call: a file
                        with no lifted function in it, in src/ or a folder under it
  names.sym             names of your own, `BB:AAAA Name` a line
  changes.txt           the functions you changed on purpose, `BB:AAAA why`
  scratch.txt           RAM that source written by hand need not leave as the
                        ROM's code does, `AAAA why` or `AAAA-BBBB why`
  lift-source.json      what the tool wrote, so that it can tell what you changed
  CMakeLists.txt        the game with src/*.c as its lifted functions
  CMakePresets.json     how Visual Studio and `cmake --preset` configure it
  include/gblift.h      what the source includes, until the game is generated
  game/                 the project gbrecomp generates; not yours
  unplaced/             changed functions an update found no place for
  .lift/                the export, the package, the checker's work

The unit is one function: everything from the line after the function before
it ends, down to its own closing brace. So a comment written above a function
is that function's, and a function with a comment added is a changed one.
A function is known by its `GBLIFT_FUNCTION(bank, address, name)`, which is
what the compiler and the checker go by; the `gbfn:` line above it is a
comment like any other.

Called from gb_lift.py, which hands itself over as `lift`: this module does
not import it, since run as a script it is `__main__` and a second copy
would have errors of its own that nothing catches.
"""

from __future__ import annotations

import argparse
import bisect
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

RECORD = "lift-source.json"
RECORD_SCHEMA = "gbrecomp.lift-source"
SOURCES = "src"
CHANGES = "changes.txt"
SCRATCH = "scratch.txt"
NAMES = "names.sym"
UNPLACED = "unplaced"
TOOL = ".lift"
GAME = "game"
PREFIX = "game"  # the generated project's target and executable
STUB = "lifted_in_src.c"


# --------------------------------------------------------------------------
# A source file as functions
# --------------------------------------------------------------------------

NOT_CODE = re.compile(r"""/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.S)
BRACE = re.compile(r"[{}]")
OPENING = re.compile(r"\s*\{")
INCLUDE = re.compile(r"\s*#\s*include\b")


def code_only(text: str) -> str:
    """`text` with comments, strings and character constants blanked out:
    the same length and the same lines, so that offsets carry over."""
    return NOT_CODE.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)


def tidy(lines: list[str]) -> str:
    """Lines as one text, without what an editor changes on its own: spaces
    at the ends of lines and blank lines at either end."""
    lines = [line.rstrip() for line in lines]
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:24]


def function_id(bank: int, address: int) -> str:
    return f"gbfn:v1:{bank:04x}:{address:04x}"


def reference(function: str) -> str:
    """gbfn:v1:0003:7850 as 03:7850."""
    bank, address = function.split(":")[2:4]
    return f"{int(bank, 16):02X}:{address.upper()}"


class Unit:
    """One lifted function with what is written above it."""

    def __init__(self, bank: int, address: int, name: str, text: str) -> None:
        self.bank, self.address, self.name, self.text = bank, address, name, text
        self.id = function_id(bank, address)


def parse_source(text: str, where: str, lift) -> tuple[str, list[Unit], str]:
    """A lifted source file as (what is before its functions, the functions,
    what is after the last one).

    The first part ends with the last `#include` before the first function.
    Braces, and the declarations themselves, are looked for in code only: one
    in a comment or a string is text. `text` has "\\n" for its line ends."""
    code = code_only(text)
    lines = text.split("\n")
    starts = [0]
    for line in lines[:-1]:
        starts.append(starts[-1] + len(line) + 1)

    found: list[tuple[int, int, str, int, int]] = []
    position = 0
    for match in lift.DECLARATION.finditer(code):
        if match.start() < position:
            continue
        opening = OPENING.match(code, match.end())
        if not opening:
            continue  # declared, not defined
        name = match.group(3)
        depth, end = 0, None
        for brace in BRACE.finditer(code, opening.end() - 1):
            depth += 1 if brace.group() == "{" else -1
            if depth == 0:
                end = brace.start()
                break
        if end is None:
            raise lift.LiftError(f"{where}: cannot find where {name} ends: its braces do not pair up")
        first = bisect.bisect_right(starts, match.start()) - 1
        last = bisect.bisect_right(starts, end) - 1
        if found and first <= found[-1][4]:
            raise lift.LiftError(f"{where}: {name} starts on the line {found[-1][2]} ends on")
        found.append((int(match.group(1), 0), int(match.group(2), 0), name, first, last))
        position = end
    if not found:
        return tidy(lines), [], ""

    code_lines = code.split("\n")
    begin = 0
    for number in range(found[0][3]):
        if INCLUDE.match(code_lines[number]):
            begin = number + 1
    preamble = tidy(lines[:begin])
    units = []
    for bank, address, name, _, last in found:
        units.append(Unit(bank, address, name, tidy(lines[begin:last + 1])))
        begin = last + 1
    return preamble, units, tidy(lines[begin:])


def assemble(preamble: str, units: list[str], tail: str) -> str:
    """The file `parse_source` reads back as these parts: a blank line
    between any two of them."""
    parts = [preamble] if preamble else []
    parts += units
    if tail:
        parts.append(tail)
    return "\n\n".join(parts) + "\n"


# --------------------------------------------------------------------------
# Comments somebody added to a function the tool wrote
# --------------------------------------------------------------------------

# The comment every statement of the structured style ends with: the address
# of the instruction it stands for. A comment somebody adds is carried by it.
INSTRUCTION = re.compile(r"/\* ([0-9A-F]{4}): ")
# A line that is nothing but a comment and is the tool's own: the line that
# says which function this is, and an instruction that left no statement.
TOOL_LINE = re.compile(r"\s*/\* (?:gbfn:v1:[0-9a-f]{4}:[0-9a-f]{4} |[0-9A-F]{4}: ).*\*/")
# The tool's line about what a function takes, which a person's own comment
# may well begin like.
SIGNATURE_LINE = re.compile(r"/\* takes .*\*/")


def comments_added(text: str, written: str) -> tuple[list[str], list[tuple[int, list[str]]]] | None:
    """A function's text as the lines the tool wrote and the lines added to
    them, where all that was added is whole lines of comment and blank lines.

    Returns (the tool's lines, [(which of them a run of added lines stands
    above, the run)]), or None where the text differs from the tool's in any
    other way: a statement touched, a comment put at the end of one, a line
    of the tool's changed or taken out. The answer is not a guess: the
    tool's lines have to be, by their hash, the text the record says it wrote."""
    lines = text.split("\n")
    no_code = [not line.strip() for line in code_only(text).split("\n")]
    signatures = [number for number, line in enumerate(lines)
                  if no_code[number] and SIGNATURE_LINE.fullmatch(line.strip())]
    # Whichever line that begins "takes" is the tool's, if any is.
    for signature in signatures + [None]:
        base: list[str] = []
        runs: list[tuple[int, list[str]]] = []
        for number, line in enumerate(lines):
            if not no_code[number] or number == signature or TOOL_LINE.fullmatch(line):
                base.append(line)
            elif runs and runs[-1][0] == len(base):
                runs[-1][1].append(line)
            else:
                runs.append((len(base), [line]))
        if digest("\n".join(base)) == written:
            return base, runs
    return None


def carry_comments(base: list[str], runs: list[tuple[int, list[str]]], fresh: str,
                   lift) -> tuple[str | None, str]:
    """The text the tool writes for a function now, with the lines somebody
    added to what it wrote before (`comments_added`) put back in.

    What stood above the function stands above it again. A run inside it
    stood above the statement of some instruction, and is put above the
    statement that instruction has now; if it stood higher, above the `do {`
    or the label that statement was under, it goes above as many such lines
    as are there now. Returns (the text, ""), or (None, the address) where
    an instruction a run was written about is no longer in the function, or
    is there twice: the function is then left as it is."""
    new = fresh.split("\n")

    def declared(lines: list[str]) -> int:
        return next(number for number, line in enumerate(lines) if lift.DECLARATION.search(line))

    def indent(line: str) -> int:
        return len(line) - len(line.lstrip(" "))

    def depth(lines: list[str], number: int) -> int:
        """How far in the line stands; for a label, which is written further
        out than what it labels, how far in the line after it does."""
        while re.fullmatch(r"\s*L_[0-9A-F]{4}:;", lines[number]) and number + 1 < len(lines):
            number += 1
        return indent(lines[number])

    old_at, new_at = declared(base), declared(new)
    where: dict[str, int | None] = {}
    for number, line in enumerate(new):
        for address in INSTRUCTION.findall(line):
            where[address] = None if address in where else number
    placed: list[tuple[int, list[str]]] = []
    for above, run in runs:
        if above <= old_at:
            # Above the function: over the tool's own lines, among them, or
            # right over the declaration, as it was.
            position = new_at if above == old_at else min(above, new_at)
        else:
            anchor = next((number for number in range(above, len(base)) if INSTRUCTION.search(base[number])), None)
            if anchor is None:
                # Below the last statement that has an instruction to it.
                closing = 0
                while closing < len(new) - new_at - 1 and not INSTRUCTION.search(new[len(new) - 1 - closing]):
                    closing += 1
                position = len(new) - min(len(base) - above, closing)
            else:
                address = INSTRUCTION.findall(base[anchor])[0]
                found = where.get(address)
                if found is None:
                    return None, address
                position = found
                while found - position < anchor - above and position - 1 > new_at and \
                        not INSTRUCTION.search(new[position - 1]):
                    position -= 1
        shift = depth(new, position) - depth(base, above)
        placed.append((position, [line if not line.strip() else
                                  " " * shift + line if shift >= 0 else
                                  line[min(-shift, indent(line)):] for line in run]))
    out = list(new)
    for position, run in reversed(sorted(placed, key=lambda item: item[0])):
        out[position:position] = run
    text = "\n".join(out)
    # Nothing added may have gone missing on the way: take the result apart
    # again and count.
    again = comments_added(text, digest(fresh))
    if again is None or sorted(line.strip() for _, run in again[1] for line in run) != \
            sorted(line.strip() for _, run in runs for line in run):
        return None, "?"
    return text, ""


# --------------------------------------------------------------------------
# Names
# --------------------------------------------------------------------------

SYM_LINE = re.compile(r"([0-9A-Fa-f]{1,4}):([0-9A-Fa-f]{4})\s+(\S+)")

NAMES_HEADING = """\
; Names of your own, one a line: BB:AAAA Name (hexadecimal, as an RGBDS .sym
; file has them). A function at that address is called by the name, and so is
; a RAM address (bank 00). `gb_lift.py source` reads this file every time it
; is run, and writes the names into the functions nobody has changed.
"""


def read_names(text: str) -> dict[tuple[int, int], list[str]]:
    """(bank, address) -> names, from the text of a .sym file."""
    names: dict[tuple[int, int], list[str]] = {}
    for line in text.splitlines():
        match = SYM_LINE.fullmatch(line.split(";", 1)[0].strip())
        if match:
            names.setdefault((int(match.group(1), 16), int(match.group(2), 16)), []).append(match.group(3))
    return names


def apply_names(export: dict, names: dict[tuple[int, int], list[str]], lift) -> int:
    """Give the export the names of a symbol file, as `gbrecomp --symbols
    FILE --symbol-policy names-only` would have: on functions, and on RAM.
    Returns how many functions took one."""
    def usable(candidates: list[str]) -> str | None:
        return next((name for name in sorted(candidates) if lift.c_identifier(name)), None)

    named = 0
    for function in export["functions"]:
        name = usable(names.get((function["bank"], function["address"]), []))
        if name:
            function["symbol"] = name
            named += 1
    for entry in export["memory"]:
        if entry["address"] >= 0x8000:
            name = usable(names.get((0, entry["address"]), []))
            if name:
                entry["name"] = name
    return named


DEFINE = re.compile(r"#define[ \t]+([A-Za-z_]\w*)[ \t]+(\S.*)")


DATA_HEADER = "lifted_data.h"
MOVED_HEADER = "lifted_moved.h"
OPERANDS = "operands.json"              # in .lift/: every operand the source names
NAMED_OPERANDS = "named-operands.json"  # in .lift/: those of them the game really has by name
NAMED_SCHEMA = "gbrecomp.lift-named-operands"
MOVED_EMPTY = """\
/* Where the names of the game's source lead in the image the game is to run:
 * `gb_data.py build --moved-header` writes this file when it moves something
 * the source names. Nothing here: everything is where the ROM has it. */
"""


def named_operands(export: dict, tree: Path, names: dict[tuple[int, int], list[str]], taken: set[str],
                   lift) -> tuple[dict[tuple[int, int], str], dict[str, int], list[dict]]:
    """The operands of the game's instructions that lead to something in the
    ROM, as a data tree's records found them (`gb_data.py pointers` writes
    them into its manifest as the `pointers` of its `things`), each with a
    name for what it leads to.

    Returns {(bank, address of the instruction): the name}, {the name: the
    address it stands for in the ROM as it is}, and one entry an operand for
    the folder's record of them: the ROM offset of the operand's low byte, the
    name, the function the instruction is in, and `size`, how far what the
    name stands for runs: to the next name the names file has in that bank,
    which is where the next thing starts for whoever wrote the names (0 where
    the file has no name behind it). A table is read by counting from its
    start, and no record of a game says how long it is.

    A name is the label the names file has at that very place, where that is
    a name C can have and nothing else has taken it, and `rom_BB_AAAA`
    otherwise. An operand the records saw leading into two banks has no one
    thing to be named after and is left a number."""
    try:
        manifest = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise lift.LiftError(f"cannot read the data tree's manifest in {tree}: {error}") from error
    if manifest.get("rom", {}).get("sha256", export["rom"]["sha256"]) != export["rom"]["sha256"] or \
            manifest.get("rom", {}).get("size") != export["rom"]["size"]:
        raise lift.LiftError(f"{tree} is the data of another ROM than this export is of")
    if "things" not in manifest:
        raise lift.LiftError(f"{tree} does not say what the game's pointers lead to: record the game with "
                             "--data-use and --data-use-pointers, and run `gb_data.py pointers` on the records")
    inside: dict[tuple[int, int], str] = {}
    for function in export["functions"]:
        for block in function["blocks"]:
            for instruction in block["instructions"]:
                inside[(function["bank"], instruction["address"])] = function["id"]
    leads: dict[int, set[tuple[int, int]]] = {}  # an operand's offset -> the (bank, address) it was seen to lead to
    for thing in manifest["things"]:
        for pointer in thing["pointers"]:
            if pointer.get("operand") and pointer["high"] == pointer["low"] + 1:
                leads.setdefault(pointer["low"], set()).add((thing["bank"], pointer["value"]))
    at: dict[tuple[int, int], str] = {}
    values: dict[str, int] = {}
    entries: list[dict] = []
    name_of: dict[tuple[int, int], str] = {}
    used = set(taken)
    labelled: dict[int, list[int]] = {}  # a bank -> the addresses that have a name, in order
    for bank, address in sorted(names):
        if address < 0x8000:
            labelled.setdefault(bank, []).append(address)
    for low, places in sorted(leads.items()):
        if len(places) != 1 or low == 0:
            continue
        (bank, value), = places
        offset = low - 1  # the instruction: its opcode is the byte before its operand
        site = (offset // 0x4000, offset if offset < 0x4000 else 0x4000 + offset % 0x4000)
        function = inside.get(site)
        if function is None:
            continue  # not an instruction of any function the export has
        place = (bank if value >= 0x4000 else 0, value)
        if place not in name_of:
            label = next((label for label in (lift.c_identifier(name) for name in names.get(place, ()))
                          if label and label not in used), None)
            name_of[place] = label or f"rom_{place[0]:02x}_{place[1]:04x}"
            used.add(name_of[place])
        at[site] = name_of[place]
        values[name_of[place]] = value
        behind = labelled.get(place[0], [])
        following = bisect.bisect_right(behind, value)
        entries.append({"offset": low, "name": name_of[place], "function": function,
                        "size": behind[following] - value if following < len(behind) else 0})
    return at, values, entries


def spelled_addresses(export: dict) -> list[dict]:
    """Every instruction of the export whose operand is two bytes that could
    be an address in the ROM, and is not where it jumps or calls to: the ROM
    offset of the operand's low byte, the bank the instruction is in, and
    what the two bytes say. What such an operand leads to cannot move unless
    the source names the operand, whatever a record saw of it."""
    found = []
    for function in export["functions"]:
        for block in function["blocks"]:
            for instruction in block["instructions"]:
                data = bytes.fromhex(instruction["bytes"])
                if len(data) == 3 and data[0] in (0x01, 0x11, 0x21, 0x31, 0x08, 0xEA, 0xFA):
                    value = data[1] | data[2] << 8
                    address = instruction["address"]
                    if value < 0x8000 and address < 0x8000:
                        offset = address if function["bank"] == 0 or address < 0x4000 \
                            else function["bank"] * 0x4000 + address - 0x4000
                        found.append({"offset": offset + 1, "bank": function["bank"] if address >= 0x4000 else 0,
                                      "value": value})
    return found


def data_header(export: dict, values: dict[str, int]) -> str:
    """lifted_data.h: the names the source has for what its operands lead to
    in the ROM. Each is where the ROM has it, unless lifted_moved.h has said
    where it is in the image the game is to run; the checker compares the
    source with the ROM as it is, and does not read that."""
    lines = [f"/* Names for what the operands of {export['rom']['title'] or 'a ROM'}'s instructions lead to",
             " * in the ROM: `ld hl, $43DE` is `ctx->hl = BaseStats;`. Written by tools/gb_lift.py",
             " * source --data-tree, from what a played game was seen to read through each",
             " * operand, and written again every time it runs. A name is the address the ROM",
             f" * has the thing at, unless {MOVED_HEADER} says where it is in the image the game",
             " * runs: `gb_data.py build --moved-header` writes that when it moves the thing. */",
             "#ifndef LIFTED_DATA_H", "#define LIFTED_DATA_H", "",
             "#ifndef GBLIFT_CHECKING", f'#include "{MOVED_HEADER}"', "#endif", ""]
    for name, value in sorted(values.items(), key=lambda item: (item[1], item[0])):
        lines += [f"#ifndef {name}", f"#define {name} 0x{value:04X}", "#endif"]
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def names_header(export: dict, names: dict[int, str], earlier: str) -> str:
    """lifted_names.h: the names the fresh source uses, then every name an
    earlier header had that this one would drop, so that a function kept from
    then still compiles."""
    lines = [f"/* Names for fixed addresses of {export['rom']['title'] or 'a ROM'}: the hardware",
             " * registers, and RAM where the export or names.sym has a name. Written by",
             " * tools/gb_lift.py source, and written again every time it runs: a name of",
             " * your own goes into names.sym. */",
             "#ifndef LIFTED_NAMES_H", "#define LIFTED_NAMES_H", ""]
    for address, name in sorted(names.items()):
        lines.append(f"#define {name} 0x{address:04X}")
    taken = set(names.values()) | {"LIFTED_NAMES_H"}
    carried = []
    for line in earlier.splitlines():
        match = DEFINE.fullmatch(line.strip())
        if match and match.group(1) not in taken:
            taken.add(match.group(1))
            carried.append(f"#define {match.group(1)} {match.group(2).rstrip()}")
    if carried:
        lines += ["", "/* Names an earlier header had and this export does not give: source",
                  " * written then may use them. */"] + carried
    lines += ["", "/* And the names for what operands lead to in the ROM. */", f'#include "{DATA_HEADER}"',
              "", "#endif", ""]
    return "\n".join(lines)


# --------------------------------------------------------------------------
# What the tool writes
# --------------------------------------------------------------------------

def shape(function: dict) -> str:
    """A hash of the instructions a function is made of. While it stays the
    same, C written for the function is C for the same code."""
    instructions = sorted((i["address"], i["bytes"]) for b in function["blocks"] for i in b["instructions"])
    return digest(json.dumps([function["bank"], instructions]))


def fresh_sources(export: dict, time_mode: str, lift,
                  operands: dict[tuple[int, int], str] | None = None
                  ) -> tuple[dict[str, str], dict[str, str], dict[int, str], int]:
    """The structured C of every function of the export that has one, as
    `baseline --style structured` writes it: {id: text}, {id: why not}, the
    names for addresses, and how many functions go by a name. `operands`
    are the instructions whose operand goes by a name (`named_operands`)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gb_lift_codegen

    index = lift.function_index(export)
    c_names = lift.function_names(export)
    names = lift.memory_names(export, set(c_names.values()))
    fresh: dict[str, str] = {}
    skipped: dict[str, str] = {}
    named = 0
    for function in export["functions"]:
        try:
            for hazard, reason in lift.UNLIFTABLE.items():
                if hazard in function["hazards"]:
                    raise lift.Unliftable(reason)
            mine = {address: name for (bank, address), name in (operands or {}).items()
                    if bank == function["bank"]}
            source = gb_lift_codegen.structured_source(function, index, names, c_names[function["id"]], time_mode,
                                                       mine)
        except (lift.Unliftable, gb_lift_codegen.Unliftable) as reason:
            skipped[function["id"]] = str(reason)
            continue
        fresh[function["id"]] = tidy(source.split("\n"))
        named += c_names[function["id"]] != lift.default_c_name(function)
    return fresh, skipped, names, named


def bank_file(bank: int) -> str:
    return f"lifted_bank_{bank:02x}.c"


def bank_preamble(export: dict, bank: int, time_mode: str, lift) -> str:
    return (f"/* Bank {bank:02X} of {export['rom']['title'] or 'a ROM'} "
            f"(sha256 {export['rom']['sha256'][:16]}...), lifted to C.\n"
            " * Written by tools/gb_lift.py source, and yours from then on: run again, it\n"
            " * keeps every function you changed, comments included, and writes the ones\n"
            f" * you did not change afresh (LIFTING.md, \"Working in the source\"). Time: {time_mode}. */\n"
            "#include \"gblift.h\"\n"
            f"#include \"{lift.NAMES_HEADER}\"")


CMAKELISTS = """\
# The game, with this folder's src/*.c as its lifted functions.
# Written once by tools/gb_lift.py source; yours to change after that.
#
# Open the folder in Visual Studio (File > Open > Folder), or from a
# developer command prompt:
#   cmake --preset x64-release
#   cmake --build --preset x64-release
# `python tools/gb_lift.py build <this folder>` checks what you changed
# against the ROM first, generates game/ when it has to, and then does that.
cmake_minimum_required(VERSION 3.21)
project(lifted_game C CXX)

# The lifted functions are compiled where they are: what you edit is what runs.
# So is any other C under src/, which is code of yours that they call.
file(GLOB_RECURSE LIFTED_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
file(GLOB_RECURSE LIFTED_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game/CMakeLists.txt")
    # game/ is the project gbrecomp generated: the runtime, the recompiled
    # code, and the hooks that enter a lifted function where the game
    # reaches its first instruction. It holds no copy of src/.
    add_subdirectory(game)
    target_sources(game PRIVATE ${LIFTED_SOURCES} ${LIFTED_HEADERS})
    target_include_directories(game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
else()
    # No game yet (`gb_lift.py build` makes it): compile the functions on
    # their own, so that the folder opens and an editor finds what they include.
    add_library(lifted OBJECT ${LIFTED_SOURCES} ${LIFTED_HEADERS})
    target_include_directories(lifted PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                              "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
"""

CHANGES_HEADING = """\
# The functions changed on purpose, one a line: BB:AAAA (hexadecimal), then
# what the change is. `gb_lift.py build` stops when a function no longer does
# what the ROM's code does, unless it is listed here: then that is the mod,
# and it is built into the game.
"""

SCRATCH_HEADING = """\
# RAM that is a register in all but name, one stretch a line: AAAA or
# AAAA-BBBB (hexadecimal), then what it is. A helper parks A in such a byte
# for a moment; a far call keeps its caller's registers in six of them.
# Nothing reads one before writing it. A function you wrote yourself does not
# have to leave in them what the ROM's code leaves: `gb_lift.py build` does
# not compare these bytes for a function whose code somebody changed, nor for
# one held to a line of signatures.txt. It compares them for every other.
"""

STUB_SOURCE = """\
/* The lifted functions of this game are not in this folder. They are the
 * src/ files of the source folder this project is in, which that folder's
 * CMakeLists.txt adds to the game: edit those, and build from there
 * (LIFTING.md, "Working in the source"). */
typedef int lifted_functions_are_in_src;
"""


def presets(sdl2_dir: str | None) -> str:
    """CMakePresets.json, after the one Visual Studio writes for a new CMake
    project: Ninja, and on Windows the 64-bit MSVC tools."""
    cache = {"CMAKE_EXPORT_COMPILE_COMMANDS": "ON"}
    if sdl2_dir:
        cache["SDL2_DIR"] = sdl2_dir.replace("\\", "/")
    windows = {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows"}
    elsewhere = {"type": "notEquals", "lhs": "${hostSystemName}", "rhs": "Windows"}
    configure = [
        {"name": "base", "hidden": True, "generator": "Ninja",
         "binaryDir": "${sourceDir}/build/${presetName}", "cacheVariables": cache},
        {"name": "windows", "hidden": True, "inherits": "base", "condition": windows,
         "architecture": {"value": "x64", "strategy": "external"},
         "toolset": {"value": "host=x64", "strategy": "external"},
         "cacheVariables": {"CMAKE_C_COMPILER": "cl.exe", "CMAKE_CXX_COMPILER": "cl.exe"}},
        {"name": "x64-release", "displayName": "x64 Release", "inherits": "windows",
         "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"}},
        {"name": "x64-debug", "displayName": "x64 Debug", "inherits": "windows",
         "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"}},
        {"name": "release", "displayName": "Release", "inherits": "base", "condition": elsewhere,
         "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"}},
        {"name": "debug", "displayName": "Debug", "inherits": "base", "condition": elsewhere,
         "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"}},
    ]
    build = [{"name": entry["name"], "configurePreset": entry["name"]}
             for entry in configure if not entry.get("hidden")]
    return json.dumps({"version": 3, "configurePresets": configure, "buildPresets": build}, indent=2) + "\n"


def default_preset() -> str:
    return "x64-release" if os.name == "nt" else "release"


# --------------------------------------------------------------------------
# The record
# --------------------------------------------------------------------------

def load_record(folder: Path, lift) -> dict:
    path = folder / RECORD
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise lift.LiftError(f"{folder} is not a source folder `gb_lift.py source` made: "
                             f"cannot read {RECORD} ({error})") from error
    if record.get("schema") != RECORD_SCHEMA or record.get("version") != 1:
        raise lift.LiftError(f"{path} is not a version 1 {RECORD_SCHEMA} file")
    return record


def write_text(path: Path, text: str) -> bool:
    """Write `text` unless the file already is it; says whether it wrote.
    A file that is not touched is not compiled again."""
    try:
        if path.read_text(encoding="utf-8") == text:
            return False
    except (OSError, ValueError):
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    scratch = path.with_name(path.name + ".new")
    scratch.write_text(text, encoding="utf-8", newline="\n")
    os.replace(scratch, path)
    return True


def save_record(folder: Path, record: dict) -> None:
    record["functions"] = dict(sorted(record["functions"].items()))
    write_text(folder / RECORD, json.dumps(record, indent=1) + "\n")


def read_folder(folder: Path, lift) -> tuple[dict[str, tuple[str, list[Unit], str]], dict[str, tuple[str, Unit]]]:
    """Every src/*.c of a folder taken apart, and its functions by ID."""
    files: dict[str, tuple[str, list[Unit], str]] = {}
    units: dict[str, tuple[str, Unit]] = {}
    for path in sorted((folder / SOURCES).glob("*.c")):
        try:
            text = path.read_text(encoding="utf-8")  # whatever line ends an editor left: read as "\n"
        except (OSError, ValueError) as error:
            raise lift.LiftError(f"cannot read {path}: {error}") from error
        files[path.name] = parse_source(text, f"{SOURCES}/{path.name}", lift)
        for unit in files[path.name][1]:
            if unit.id in units:
                raise lift.LiftError(f"{reference(unit.id)} is lifted twice: in {SOURCES}/{units[unit.id][0]} "
                                     f"and {SOURCES}/{path.name}")
            units[unit.id] = (path.name, unit)
    return files, units


def support_code(folder: Path, files: dict[str, tuple[str, list[Unit], str]], lift) -> tuple[list[Path], str]:
    """The C a folder has beside its lifted functions, and one hash of it.

    Source written by hand is ordinary functions with arguments, kept
    wherever in src/ they belong, and a lifted function that has become a few
    lines handing its registers to one of them (LIFTING.md, "Functions
    written by hand"). Such code is every `.c` file of src/ that has no
    lifted function in it, and every `.c` file of a folder under src/, which
    may have none: a function of the ROM's has its place in a bank's file,
    where an update of the folder looks for it. The hash takes in the headers
    too, since a variable given another address there is another function."""
    sources = folder / SOURCES
    support = [sources / name for name, (_, units, _) in sorted(files.items()) if not units]
    for path in sorted(sources.rglob("*.c")):
        if path.parent == sources:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, ValueError) as error:
            raise lift.LiftError(f"cannot read {path}: {error}") from error
        declared = lift.DECLARATION.search(code_only(text))
        if declared:
            raise lift.LiftError(f"{path.relative_to(folder).as_posix()} declares {declared.group(3)} as a function "
                                 f"of the ROM's: those are kept in the files of {SOURCES}/ itself, and what is in a "
                                 "folder under it is the code they call")
        support.append(path)
    hashed = hashlib.sha256()
    headers = [path for path in sorted(sources.rglob("*.h")) if path.name != lift.NAMES_HEADER]
    for path in support + headers:
        hashed.update(path.relative_to(sources).as_posix().encode("utf-8") + b"\0")
        hashed.update(tidy(path.read_text(encoding="utf-8", errors="replace").split("\n")).encode("utf-8") + b"\0")
    return support, hashed.hexdigest()[:24] if support or headers else ""


def read_scratch(folder: Path, lift) -> list[str]:
    """scratch.txt: the stretches of RAM not compared for a function somebody
    changed, as the checker takes them."""
    path = folder / SCRATCH
    if not path.is_file():
        return []
    stretches = []
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        where = line.split(None, 1)[0].upper()
        found = re.fullmatch(r"([0-9A-F]{4})(?:-([0-9A-F]{4}))?", where)
        if not found or int(found.group(1), 16) < 0x8000 or \
                (found.group(2) and int(found.group(2), 16) < int(found.group(1), 16)):
            raise lift.LiftError(f"{path}:{number}: a line starts with an address in RAM as AAAA, or a stretch "
                                 f"of it as AAAA-BBBB, not {where!r}")
        stretches.append(where)
    return stretches


def read_changes(folder: Path, lift) -> dict[str, str]:
    """changes.txt: the functions meant to differ from the ROM, with why."""
    declared: dict[str, str] = {}
    path = folder / CHANGES
    if not path.is_file():
        return declared
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        where, why = (line.split(None, 1) + [""])[:2]
        if not re.fullmatch(r"[0-9A-Fa-f]{1,4}:[0-9A-Fa-f]{4}", where):
            raise lift.LiftError(f"{path}:{number}: a line starts with a function as BB:AAAA, not {where!r}")
        declared[lift.parse_function_ref(where)] = why.strip()
    return declared


# --------------------------------------------------------------------------
# source: make the folder, or bring it up to date with an export
# --------------------------------------------------------------------------

def existing_file_arguments(arguments: list[str]) -> list[str]:
    """Analysis options as they are remembered: an argument that names a file
    by its full path, since `build` is run from anywhere."""
    out = []
    for argument in arguments:
        path = Path(argument)
        out.append(str(path.resolve()) if not argument.startswith("-") and path.is_file() else argument)
    return out


def command_source(args: argparse.Namespace, lift) -> int:
    started = time.monotonic()
    export_path = Path(args.export).resolve()
    export = lift.load_export(export_path)
    folder = Path(args.output).resolve()
    made = (folder / RECORD).is_file()
    if made:
        record = load_record(folder, lift)
        if record["rom"]["sha256"] != export["rom"]["sha256"]:
            raise lift.LiftError(f"{folder} is the source of another ROM than this export is of")
    else:
        if folder.exists() and any(folder.iterdir()):
            raise lift.LiftError(f"{folder} is not empty and has no {RECORD}: name a new folder")
        record = {"schema": RECORD_SCHEMA, "version": 1, "functions": {}, "files": {}}
    was = record["functions"]
    time_mode = args.time or record.get("time") or "exact"
    analysis = args.analysis  # None: as it was told before

    # Names: the folder's own file, which a symbol file brought is added to
    # where the folder has no name of its own.
    names_path = folder / NAMES
    names_text = names_path.read_text(encoding="utf-8", errors="replace") if names_path.is_file() else NAMES_HEADING
    brought = 0
    if args.symbols:
        try:
            symbols = Path(args.symbols).read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            raise lift.LiftError(f"cannot read the symbol file {args.symbols}: {error}") from error
        have = read_names(names_text)
        lines = []
        for (bank, address), names in sorted(read_names(symbols).items()):
            if (bank, address) not in have:
                lines += [f"{bank:02x}:{address:04x} {name}" for name in names]
                brought += len(names)
        if lines:
            names_text = names_text.rstrip("\n") + "\n" + "\n".join(lines) + "\n"
    apply_names(export, read_names(names_text), lift)

    # What the operands of the game's instructions lead to in the ROM, from a
    # data tree whose records saw it: those operands go by a name.
    tree = args.data_tree or record.get("data_tree")
    operand_at: dict[tuple[int, int], str] = {}
    operand_values: dict[str, int] = {}
    operand_entries: list[dict] = []
    if tree:
        taken = set(lift.function_names(export).values())
        taken |= set(lift.memory_names(export, taken).values())
        operand_at, operand_values, operand_entries = named_operands(export, Path(tree).resolve(),
                                                                     read_names(names_text), taken, lift)

    fresh, skipped, memory, named = fresh_sources(export, time_mode, lift, operand_at)
    index = lift.function_index(export)
    shapes = {function_id_: shape(index[function_id_]) for function_id_ in fresh}
    restore = {lift.parse_function_ref(item) for item in args.restore}
    for function_id_ in restore:
        if function_id_ not in fresh:
            raise lift.LiftError(f"{reference(function_id_)} cannot be restored: the tool has no version of it")

    files, units = read_folder(folder, lift) if made else ({}, {})
    kept: list[str] = []          # a person changed their code, and they still stand for the same code
    reshaped: list[str] = []      # changed, and the code is no longer what they were written from
    commented: list[str] = []     # only comments added: as they are, the tool having nothing new for them
    carried: list[str] = []       # only comments added: written afresh, with the comments put back
    stranded: list[tuple[str, str]] = []  # only comments added, and one is about an instruction now gone
    unplaced: list[tuple[str, str, Unit]] = []  # changed, and no longer a function of the export
    refreshed: list[str] = []     # untouched, written again and came out differently
    gone: list[str] = []          # untouched, and no longer a function
    restored: list[str] = []
    same = 0
    out: dict[str, tuple[str, list[tuple[str, str]], str]] = {}
    banks: dict[str, int] = {}    # the files that are a bank's, made by the tool
    for name, (preamble, file_units, tail) in files.items():
        tracked = record["files"].get(name, {})
        if "bank" in tracked:
            banks[name] = tracked["bank"]
            if tracked.get("preamble") == digest(preamble):
                # The bank's own opening lines, which nobody has changed.
                preamble = bank_preamble(export, tracked["bank"], time_mode, lift)
        placed: list[tuple[str, str]] = []
        for unit in file_units:
            before = was.get(unit.id, {})
            if unit.id in restore:
                placed.append((unit.id, fresh[unit.id]))
                restored.append(unit.id)
            elif before.get("written") == digest(unit.text):
                if unit.id not in fresh:
                    gone.append(unit.id)
                    continue
                placed.append((unit.id, fresh[unit.id]))
                if fresh[unit.id] == unit.text:
                    same += 1
                else:
                    refreshed.append(unit.id)
            elif unit.id not in index:
                unplaced.append((unit.id, name, unit))
            else:
                # Somebody changed it. Where all that was added is comments,
                # it is still the tool's to write afresh, and the comments go
                # with it; where code was touched, it is kept exactly.
                added = comments_added(unit.text, before["written"]) \
                    if before.get("written") and unit.id in fresh else None
                if added is not None and digest(fresh[unit.id]) == before["written"]:
                    placed.append((unit.id, unit.text))
                    commented.append(unit.id)
                    continue
                if added is not None:
                    text, lost = carry_comments(*added, fresh[unit.id], lift)
                    if text is not None:
                        placed.append((unit.id, text))
                        carried.append(unit.id)
                        continue
                    stranded.append((unit.id, lost))
                placed.append((unit.id, unit.text))
                if added is not None:
                    pass
                elif before.get("shape") and unit.id in shapes and shapes[unit.id] != before["shape"]:
                    reshaped.append(unit.id)
                else:
                    kept.append(unit.id)
        out[name] = (preamble, placed, tail)

    # Functions the folder does not have: new ones go in, in address order;
    # one the tool wrote before and a person took out stays out.
    new: list[str] = []
    removed: list[str] = []
    for function_id_ in fresh:
        if function_id_ in units:
            continue
        if was.get(function_id_, {}).get("written") and function_id_ not in restore:
            removed.append(function_id_)
            continue
        function = index[function_id_]
        name = bank_file(function["bank"])
        if name not in out:
            out[name] = (bank_preamble(export, function["bank"], time_mode, lift), [], "")
            banks[name] = function["bank"]
        placed = out[name][1]
        where = next((position for position, (other, _) in enumerate(placed)
                      if other in index and (index[other]["bank"], index[other]["address"]) >
                      (function["bank"], function["address"])), len(placed))
        placed.insert(where, (function_id_, fresh[function_id_]))
        (restored if function_id_ in restore else new).append(function_id_)

    # Read back what is about to be written: every function a person changed
    # has to be in it as it was. Nothing is written before this holds.
    texts: dict[str, str] = {}
    for name, (preamble, placed, tail) in out.items():
        if not placed and not tail and name in banks and \
                preamble == bank_preamble(export, banks[name], time_mode, lift):
            texts[name] = ""  # a bank file of the tool's with nothing left in it
            continue
        texts[name] = assemble(preamble, [text for _, text in placed], tail)
        _, back, _ = parse_source(texts[name], f"{SOURCES}/{name}", lift)
        if [(unit.id, unit.text) for unit in back] != placed:
            raise lift.LiftError(f"{SOURCES}/{name} would not read back as it was put together: "
                                 "nothing was written. This is a fault in the tool")
    for function_id_ in kept + reshaped + commented + [item for item, _ in stranded]:
        name, unit = units[function_id_]
        if (function_id_, unit.text) not in out[name][1]:
            raise lift.LiftError(f"{reference(function_id_)} would have been lost: nothing was written. "
                                 "This is a fault in the tool")

    # From here on the folder changes. What src/ was is kept until next time.
    sources = folder / SOURCES
    if made:
        backup = folder / TOOL / "before-update"
        if backup.exists():
            shutil.rmtree(backup)
        shutil.copytree(sources, backup)
    sources.mkdir(parents=True, exist_ok=True)
    write_text(names_path, names_text)
    if unplaced:
        for function_id_, name, unit in unplaced:
            target = folder / UNPLACED / name
            earlier = target.read_text(encoding="utf-8") if target.is_file() else (
                "/* Functions somebody changed that an update of the folder found no place for.\n"
                " * Nothing here is compiled. */\n")
            write_text(target, earlier + f"\n/* {reference(function_id_)} was in {SOURCES}/{name}: it is no longer "
                       "a function of the export. */\n" + unit.text + "\n")
    lines = 0
    for name, text in texts.items():
        if text:
            write_text(sources / name, text)
            lines += text.count("\n")
        else:
            (sources / name).unlink(missing_ok=True)

    header_path = sources / lift.NAMES_HEADER
    earlier = header_path.read_text(encoding="utf-8") if header_path.is_file() else ""
    header_changed = bool(earlier) and record["files"].get(lift.NAMES_HEADER, {}).get("written") != digest(earlier)
    if header_changed:
        write_text(folder / UNPLACED / lift.NAMES_HEADER, earlier)
    header = names_header(export, memory, earlier)
    write_text(header_path, header)
    write_text(sources / DATA_HEADER, data_header(export, operand_values))
    if not (sources / MOVED_HEADER).exists():
        write_text(sources / MOVED_HEADER, MOVED_EMPTY)
    write_text(folder / TOOL / OPERANDS, json.dumps({"operands": operand_entries}, indent=1) + "\n")

    # What an editor and a build need, each written once and then left alone.
    for name, text in ((CHANGES, CHANGES_HEADING), (SCRATCH, SCRATCH_HEADING), ("CMakeLists.txt", CMAKELISTS),
                       ("CMakePresets.json", presets(args.sdl2_dir or os.environ.get("SDL2_DIR")))):
        if not (folder / name).exists():
            write_text(folder / name, text)
    write_text(folder / "include" / "gblift.h", (lift.LIFT_INCLUDE / "gblift.h").read_text(encoding="utf-8"))
    stored = folder / TOOL / "export.json"
    stored.parent.mkdir(parents=True, exist_ok=True)
    if stored != export_path:
        shutil.copyfile(export_path, stored)

    # The record: what the tool has for every function now, whether or not
    # that is what is in the file, and what the checker last said of a text.
    functions: dict[str, dict] = {}
    left_behind = {function_id_ for function_id_, _ in stranded}
    for name, (_, placed, _) in out.items():
        for function_id_, text in placed:
            before = was.get(function_id_, {})
            entry = {"file": name}
            if function_id_ in left_behind:
                # Its comments had nowhere to go, so it is still the text the
                # tool wrote before with comments added, and is recorded so:
                # it is tried again next time, and checked again meanwhile.
                entry["written"] = before["written"]
                if before.get("shape"):
                    entry["shape"] = before["shape"]
                functions[function_id_] = entry
                continue
            if function_id_ in fresh:
                entry["written"] = digest(fresh[function_id_])
                entry["shape"] = shapes[function_id_]
            if before.get("checked") == digest(text) and before.get("shape") == entry.get("shape"):
                entry["checked"], entry["verdict"] = before["checked"], before["verdict"]
                if before.get("signature"):
                    entry["signature"] = before["signature"]  # the line it was held to when checked
                if before.get("clock"):
                    entry["clock"] = before["clock"]
            functions[function_id_] = entry
    for function_id_ in removed:
        functions[function_id_] = {"written": digest(fresh[function_id_]), "shape": shapes[function_id_],
                                   "removed": True}
    record["rom"] = {"title": export["rom"]["title"], "sha256": export["rom"]["sha256"],
                     "size": export["rom"]["size"]}
    record["style"], record["time"] = "structured", time_mode
    if args.rom:
        record["rom_path"] = str(Path(args.rom).resolve())
    if tree:
        record["data_tree"] = str(Path(tree).resolve())
    if analysis is not None:
        record["analysis"] = existing_file_arguments(analysis)
    record.setdefault("analysis", [])
    record["functions"] = functions
    record["files"] = {name: {"bank": bank, "preamble": digest(bank_preamble(export, bank, time_mode, lift))}
                       for name, bank in sorted(banks.items()) if texts.get(name)}
    record["files"][lift.NAMES_HEADER] = {"written": digest(header)}
    record["last_update"] = {
        "kept": sorted(kept), "shape_changed": sorted(reshaped),
        "commented": sorted(commented + carried), "carried": sorted(carried),
        "comment_stranded": sorted(function_id_ for function_id_, _ in stranded),
        "not_placed": sorted(function_id_ for function_id_, _, _ in unplaced),
        "removed_by_hand": sorted(removed), "restored": sorted(restored),
        "refreshed": len(refreshed), "unchanged": same, "new": len(new), "gone": len(gone)}
    save_record(folder, record)

    took = time.monotonic() - started
    count = sum(len(placed) for _, placed, _ in out.values())
    print(f"{'updated' if made else 'wrote'} {folder}: {count} functions, {lines} lines in "
          f"{sum(1 for text in texts.values() if text)} files; {named} of the export's functions go by a name"
          + (f" ({brought} names brought into {NAMES})" if args.symbols else "") + f"  ({took:.1f} s)")
    if made:
        print(f"  kept      {len(kept) + len(reshaped) + len(stranded)} functions somebody changed")
        print(f"  carried   {len(commented) + len(carried)} functions somebody only commented, {len(carried)} of them "
              "written afresh with their comments put back")
        print(f"  refreshed {len(refreshed) + same} functions nobody touched, {len(refreshed)} of them written differently")
        print(f"  new       {len(new)} functions the folder did not have")
        if gone:
            print(f"  gone      {len(gone)} functions nobody touched that are no longer functions of the export")
        if restored:
            print(f"  restored  {len(restored)}: " + ", ".join(reference(item) for item in sorted(restored)))
        if removed:
            print(f"  left out  {len(removed)} that somebody took out of the folder (--restore BB:AAAA brings one back): "
                  + ", ".join(reference(item) for item in sorted(removed)[:8]) + (" ..." if len(removed) > 8 else ""))
        if reshaped or unplaced or stranded or header_changed:
            print(f"  could not reconcile {len(reshaped) + len(unplaced) + len(stranded) + header_changed}:")
            for function_id_, lost in sorted(stranded):
                name, unit = units[function_id_]
                print(f"    {reference(function_id_)}  {unit.name}  kept as it is in {SOURCES}/{name}: it has only "
                      "comments added, but " +
                      (f"one of them is about the instruction at {lost}, which the function no longer has, or has twice"
                       if lost != "?" else "they could not be put back into what the tool writes for it now"))
            for function_id_ in sorted(reshaped):
                name, unit = units[function_id_]
                print(f"    {reference(function_id_)}  {unit.name}  kept as it is in {SOURCES}/{name}, but the code it "
                      f"stands for is not the code it was written from ({index[function_id_]['instruction_count']} "
                      "instructions now): `build` checks it again")
            for function_id_, name, unit in sorted(unplaced, key=lambda item: item[0]):
                print(f"    {reference(function_id_)}  {unit.name}  is no longer a function of the export: taken out "
                      f"of {SOURCES}/{name}, its text is in {UNPLACED}/{name}")
            if header_changed:
                print(f"    {SOURCES}/{lift.NAMES_HEADER} was changed by hand and is written by the tool: what it was "
                      f"is in {UNPLACED}/{lift.NAMES_HEADER}, its names are kept, and a name of your own goes "
                      f"into {NAMES}")
        print(f"  what {SOURCES}/ was before this is in {TOOL}/before-update/")
    if skipped:
        reasons: dict[str, int] = {}
        for reason in skipped.values():
            key = reason if reason in lift.UNLIFTABLE.values() else reason.split(" at ")[0]
            reasons[key] = reasons.get(key, 0) + 1
        print(f"  no C for {len(skipped)} functions: " +
              ", ".join(f"{count} because {reason}" for reason, count in sorted(reasons.items(), key=lambda i: -i[1])))
    print("  the game is generated with: " + (" ".join(record["analysis"]) or "no analysis options") +
          ("" if analysis is not None else "  (as told before; give the export's own after `--` if they are others)"
           if made else "  (give the options the export was made with after `--`)"))
    if not made:
        print(f"build the game with: python {Path(lift.__file__).name} build {folder}"
              + ("" if record.get("rom_path") else " --rom ROM"))
    return 0


# --------------------------------------------------------------------------
# build: from the folder to a game
# --------------------------------------------------------------------------

def run(command: list, cwd: Path, what: str, lift, timeout: int = 3600) -> float:
    started = time.monotonic()
    try:
        result = subprocess.run([str(part) for part in command], cwd=cwd, capture_output=True, text=True,
                                errors="replace", timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise lift.LiftError(f"{what} did not run: {error}") from error
    if result.returncode:
        raise lift.LiftError(f"{what} failed (exit {result.returncode}):\n" +
                             (result.stdout + result.stderr).strip()[-6000:])
    return time.monotonic() - started


def clock(entry: dict) -> str:
    """What a check found of a function's time, in a word for the record:
    `differs` from the original's in some state that agreed, `same` in every
    one that had a time to compare, or `not counted` where none had."""
    timing = entry["cycles"]
    if timing["different"]:
        return "differs"
    return "same" if timing["equal"] else "not counted"


def command_build(args: argparse.Namespace, lift) -> int:
    folder = Path(args.folder).resolve()
    record = load_record(folder, lift)
    export_path = folder / TOOL / "export.json"
    export = lift.load_export(export_path)
    index = lift.function_index(export)
    files, units = read_folder(folder, lift)
    if not units:
        raise lift.LiftError(f"{folder / SOURCES} has no lifted function")
    c_names: dict[str, str] = {}
    for function_id_, (name, unit) in units.items():
        if function_id_ not in index:
            raise lift.LiftError(f"{SOURCES}/{name}: {unit.name} claims {reference(function_id_)}, which is not a "
                                 "function of the export the folder was last made from: run `source` again with "
                                 "the export it belongs to, or take it out")
        if unit.name in c_names:
            raise lift.LiftError(f"two functions are called {unit.name}: {reference(c_names[unit.name])} and "
                                 f"{reference(function_id_)}")
        c_names[unit.name] = function_id_
    declared = read_changes(folder, lift)

    rom_path = args.rom or record.get("rom_path")
    if not rom_path or not Path(rom_path).is_file():
        raise lift.LiftError("no ROM: give --rom" if not rom_path else f"no ROM at {rom_path}: give --rom")
    rom = Path(rom_path).resolve()
    if hashlib.sha256(rom.read_bytes()).hexdigest() != record["rom"]["sha256"]:
        raise lift.LiftError(f"{rom} is not the ROM this folder is the source of")
    if record.get("rom_path") != str(rom):
        record["rom_path"] = str(rom)

    # Check what changed: every function whose text is not the text the
    # checker last gave a verdict on. The first time, that is all of them.
    functions = record["functions"]
    images = [Path(image).resolve() for image in args.image]
    images += sorted(path.resolve() for path in (folder / "images").glob("*.img"))
    # And the states a game entered its functions in, which count as images
    # do: a new file has every function nothing got through checked again.
    entry_files = [Path(entries).resolve() for entries in args.entries]
    entry_files += sorted(path.resolve() for path in (folder / "entries").glob("*.entries"))
    more_images = sorted({image.name for image in images + entry_files} - set(record.get("images", [])))
    liftable = {function_id_ for function_id_ in units
                if not any(hazard in index[function_id_]["hazards"] for hazard in lift.UNLIFTABLE)}
    # Code the functions call. When any of it changes, every function
    # somebody changed is checked again: the tool's own call none of it.
    def code_changed(function_id_: str) -> bool:
        """Somebody changed its code: it is not the tool's text, nor that with
        comments added."""
        written = functions.get(function_id_, {}).get("written")
        text = units[function_id_][1].text
        return written != digest(text) and (not written or comments_added(text, written) is None)

    support, support_hash = support_code(folder, files, lift)
    scratch = read_scratch(folder, lift)
    if scratch:
        support_hash = hashlib.sha256((support_hash + " " + " ".join(scratch)).encode("utf-8")).hexdigest()[:24]
    support_changed = support_hash != record.get("support", "")
    # What each function takes, gives and keeps (signatures.txt): a function
    # is checked held to its line, and again when the line changes.
    import gb_lift_signatures  # here: that module imports this one
    signed = gb_lift_signatures.read(folder, lift)

    def held_to(function_id_: str) -> str:
        return digest(signed[function_id_][1]) if function_id_ in signed else ""

    to_check = []
    for function_id_ in sorted(liftable):
        entry = functions.setdefault(function_id_, {"file": units[function_id_][0]})
        text_hash = digest(units[function_id_][1].text)
        if args.recheck or entry.get("checked") != text_hash or \
                (more_images and entry.get("verdict") == "inconclusive") or \
                (support_changed and code_changed(function_id_)) or \
                entry.get("signature", "") != held_to(function_id_):
            to_check.append(function_id_)
    check_seconds = 0.0
    if to_check:
        report_path = folder / TOOL / "check.json"
        report_path.unlink(missing_ok=True)
        command = ["check", str(export_path), "--rom", str(rom), "--work", str(folder / TOOL / "check"),
                   "--json", str(report_path), "--trials", str(args.trials), "--seed", str(args.seed)]
        # The files the functions to check are in. A function somebody wrote
        # as C may call C that stands in any other of them (converted
        # functions call each other by name), so with one of those to check
        # every file is compiled in.
        with_them = {units[function_id_][0] for function_id_ in to_check}
        if any(code_changed(function_id_) for function_id_ in to_check):
            with_them = set(files)
        for name in sorted(with_them):
            command += ["--source", str(folder / SOURCES / name)]
        if len(to_check) < len(liftable):
            for function_id_ in to_check:
                command += ["--function", function_id_]
        for image in images:
            command += ["--image", str(image)]
        for entries in entry_files:
            command += ["--entries", str(entries)]
        for path in support:
            command += ["--support", str(path)]
        # The RAM scratch.txt lists is not compared for a function whose
        # code somebody changed, and for such a one only.
        terms: dict[str, dict] = {}
        for function_id_ in to_check:
            term: dict = {}
            # What scratch.txt lists is not compared for a function whose code
            # somebody changed, nor for one held to a signature: a register it
            # does not take has another value on the way in, and a helper
            # that parks its registers in scratch parks that.
            if scratch and (code_changed(function_id_) or function_id_ in signed):
                term["ignore"] = scratch
            if function_id_ in signed:
                term.update(gb_lift_signatures.contract_entry(signed[function_id_][0]))
            if term:
                terms[reference(function_id_)] = term
        if terms:
            contract_path = folder / TOOL / "contract.json"
            write_text(contract_path, json.dumps({"functions": terms}, indent=1) + "\n")
            command += ["--contract", str(contract_path)]
        command += ["--include", str(folder / SOURCES)]
        if args.cc:
            command += ["--cc", args.cc]
        if args.jobs:
            command += ["--jobs", str(args.jobs)]
        print(f"checking {len(to_check)} of {len(units)} functions against the ROM")
        sys.stdout.flush()
        started = time.monotonic()
        check = lift.build_parser().parse_args(command)
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            check.run(check)
        check_seconds = time.monotonic() - started
        # The checker lists every function it could not run, which for a whole
        # game is thousands of lines: those go to a file, and what differs and
        # the totals are shown.
        write_text(folder / TOOL / "check.log", said.getvalue())
        shown = False
        for line in said.getvalue().splitlines():
            if not line.startswith(" "):
                shown = not line.startswith("inconclusive")
            if shown:
                print(line)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        for entry in report["results"]:
            functions[entry["function"]]["checked"] = digest(units[entry["function"]][1].text)
            functions[entry["function"]]["verdict"] = entry["verdict"]
            # Whether it took the original's time, which is no part of the
            # verdict: `signatures` reads it, since a function that takes
            # another time held to its line has a line that leaves out what
            # its time depends on.
            functions[entry["function"]]["clock"] = clock(entry)
            if held_to(entry["function"]):
                functions[entry["function"]]["signature"] = held_to(entry["function"])
            else:
                functions[entry["function"]].pop("signature", None)
        record["images"] = sorted(set(record.get("images", [])) |
                                  {image.name for image in images + entry_files})
        save_record(folder, record)
    if support_changed:
        # Only now: a check that did not finish leaves them to be checked again.
        record["support"] = support_hash
        save_record(folder, record)

    if signed:
        gb_lift_signatures.write_masks(folder, signed)  # what the game takes with --lift-signatures

    def changed(function_id_: str) -> bool:
        return functions[function_id_].get("written") != digest(units[function_id_][1].text)

    def described(function_id_: str) -> str:
        name, unit = units[function_id_]
        return f"{reference(function_id_)}  {unit.name}  ({SOURCES}/{name})"

    verdict = {function_id_: functions[function_id_].get("verdict", "inconclusive") for function_id_ in liftable}
    undeclared = sorted(function_id_ for function_id_ in liftable
                        if verdict[function_id_] == "mismatch" and function_id_ not in declared)
    counts = {word: sum(1 for value in verdict.values() if value == word)
              for word in ("match", "mismatch", "inconclusive")}
    touched = sorted(function_id_ for function_id_ in units if changed(function_id_))
    print(f"{len(units)} functions: {counts['match']} match the ROM's code, {counts['mismatch']} differ, "
          f"{counts['inconclusive']} the checker could not run; {len(touched)} are not as the tool wrote them")
    if signed:
        print(f"{sum(1 for function_id_ in liftable if function_id_ in signed)} of them are held to a signature "
              f"({gb_lift_signatures.FILE})")
    bound = sorted(function_id_ for function_id_ in liftable
                   if verdict[function_id_] == "match" or function_id_ in declared)
    for function_id_ in sorted(declared):
        if function_id_ not in units:
            print(f"  {CHANGES} names {reference(function_id_)}, which the folder has no function for")
        elif function_id_ not in liftable:
            print(f"  {CHANGES} names {described(function_id_)}, which cannot be built in: "
                  "its code runs from RAM, or in a bank mapped low")
        else:
            said = {"mismatch": "differs from the ROM's code, as declared",
                    "match": "is declared changed, and the checker saw no difference",
                    "inconclusive": "is declared changed; the checker could not run it, so it is built in on "
                                    "your word"}[verdict[function_id_]]
            print(f"  {described(function_id_)} {said}" + (f": {declared[function_id_]}" if declared[function_id_] else ""))
    if undeclared:
        sys.stdout.flush()
        print(f"{len(undeclared)} of them no longer do what the ROM's code does, with nothing to say that is meant:",
              file=sys.stderr)
        for function_id_ in undeclared:
            print(f"  {described(function_id_)}", file=sys.stderr)
        print(f"Where that is the point, add a line to {folder / CHANGES}:\n"
              f"  {reference(undeclared[0])}  what the change is\n"
              f"Otherwise mend it, or get the tool's version back with `source ... --restore "
              f"{reference(undeclared[0])}`. Nothing was built.", file=sys.stderr)
        return 1
    unbuilt = [function_id_ for function_id_ in touched if function_id_ not in bound]
    if unbuilt:
        print(f"{len(unbuilt)} of the functions somebody changed are not in the game, because the checker could "
              f"not run them: the game runs the ROM's code there. A line in {CHANGES} builds one in:")
        for function_id_ in unbuilt[:12]:
            print(f"  {described(function_id_)}")
    if args.check_only:
        return 0
    if not bound:
        raise lift.LiftError("no function matched the ROM's code: there is nothing to build in")

    # The operands the game has by name: of a function that is in the game as
    # the tool wrote it, comments aside. Code somebody changed may have the
    # number written out, and a function the game runs the ROM's code for has
    # the ROM's operand. `gb_data.py pointers --named` takes this, and then
    # lets what such operands lead to move.
    operands_path = folder / TOOL / OPERANDS
    if operands_path.is_file():
        in_game = set(bound)
        entries = [{"offset": entry["offset"], "name": entry["name"], "size": entry.get("size", 0)}
                   for entry in json.loads(operands_path.read_text(encoding="utf-8"))["operands"]
                   if entry["function"] in in_game and not code_changed(entry["function"])]
        by_name = {entry["offset"] for entry in entries}
        write_text(folder / TOOL / NAMED_OPERANDS, json.dumps({
            "schema": NAMED_SCHEMA, "version": 1,
            "rom": {"sha1": hashlib.sha1(rom.read_bytes()).hexdigest()},
            "operands": entries,
            # And every other operand that spells an address in the ROM: what
            # one of those leads to stays where it is.
            "unnamed": [entry for entry in spelled_addresses(export) if entry["offset"] not in by_name]},
            indent=1) + "\n")

    # Gather. The package names the functions; the source it holds is a
    # stub, because the game compiles src/ where it is.
    package = folder / TOOL / "package"
    manifest = json.dumps({
        "schema": lift.PACKAGE_SCHEMA, "version": 1,
        "rom": {"sha256": record["rom"]["sha256"], "size": record["rom"]["size"]},
        "sources": [STUB],
        "functions": [{"id": function_id_, "symbol": units[function_id_][1].name} for function_id_ in bound],
    }, indent=1) + "\n"
    write_text(package / STUB, STUB_SOURCE)
    write_text(package / "lift.json", manifest)

    # Generate, when what gbrecomp is given is not what the game was made from.
    gbrecomp = Path(args.gbrecomp) if args.gbrecomp else \
        lift.REPO / "build" / "bin" / ("gbrecomp.exe" if os.name == "nt" else "gbrecomp")
    if not gbrecomp.is_file():
        raise lift.LiftError(f"no gbrecomp at {gbrecomp}: build the repository, or give --gbrecomp")
    game = folder / GAME
    stamp_path = folder / TOOL / "generated.json"
    tool = gbrecomp.stat()
    stamp = json.dumps({"manifest": digest(manifest), "analysis": record.get("analysis", []),
                        "gbrecomp": [str(gbrecomp.resolve()), tool.st_size, tool.st_mtime_ns]}, indent=1) + "\n"
    generated = None
    if args.regenerate or not (game / "CMakeLists.txt").is_file() or not stamp_path.is_file() or \
            stamp_path.read_text(encoding="utf-8") != stamp:
        first = not (game / "CMakeLists.txt").is_file()
        stamp_path.unlink(missing_ok=True)
        generated = run([gbrecomp, rom, *record.get("analysis", []), "--lift", package / "lift.json",
                         "-o", game, "--output-prefix", PREFIX], folder, "gbrecomp", lift)
        write_text(stamp_path, stamp)
        if first:
            # The folder's CMakeLists.txt looks for game/ when it is configured:
            # a build folder configured before has to be configured again.
            os.utime(folder / "CMakeLists.txt")
    if args.no_compile:
        print(f"{len(bound)} functions gathered" +
              (f"; generated {game} in {generated:.1f} s" if generated is not None else ""))
        return 0

    preset = args.preset or default_preset()
    build_dir = folder / "build" / preset
    configured = None
    if generated is not None or args.cmake_arg or not (build_dir / "CMakeCache.txt").is_file():
        configured = run(["cmake", "--preset", preset, *args.cmake_arg], folder, "cmake", lift)
    built = run(["cmake", "--build", "--preset", preset], folder, "the build", lift, timeout=7200)
    executable = next((path for path in (build_dir / GAME / f"{PREFIX}.exe", build_dir / GAME / PREFIX)
                       if path.is_file()), None)
    if executable is None:
        raise lift.LiftError(f"the build made no {PREFIX} in {build_dir / GAME}")
    steps = [f"checked {len(to_check)} in {check_seconds:.1f} s"] if to_check else ["nothing to check"]
    if generated is not None:
        steps.append(f"generated in {generated:.1f} s")
    if configured is not None:
        steps.append(f"configured in {configured:.1f} s")
    steps.append(f"built in {built:.1f} s")
    print(f"{len(bound)} lifted functions in the game ({', '.join(steps)}): {executable}")
    return 0
