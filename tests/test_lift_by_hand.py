#!/usr/bin/env python3
"""Functions of a game written by hand as ordinary C, end to end.

A lifted function is the original's instructions with registers. The step
after it is the function written as C would have it: arguments, results, the
game's variables by name, kept in files of its own. What is left in the bank's
file is a binding, a few lines that hand the registers over. This makes a
small game, has `gb_lift.py source` make its folder, writes five of its
functions that way under `src/native/`, and holds `gb_lift.py build` to what
LIFTING.md says of such code:

- the C a binding calls is compiled into the checker and into the game from
  where it is, in `src/` or a folder under it, and its own headers are found;
- a variable is the byte at its address (`lift_ram`), and code that is not
  written by hand is still called through the machine (`lift_call`);
- a register the C has no value for is said to be undefined
  (`lift_undefined`) and is not compared;
- a byte of RAM that is a register in all but name is compared until
  `scratch.txt` lists it, and then is not, for a function somebody changed;
- where the original reads what it pushed through a pointer, and C that
  pushes nothing comes out differently, the state is thrown away and not
  called a mismatch: a function that is so in every state gets no verdict,
  and `build` says it is not in the game;
- an edit to the C a binding calls has the binding checked again, though its
  own text is what it was, and a wrong edit stops the build;
- a file under `src/native/` cannot declare a function of the ROM's;
- the game built from all this ends as the original does;
- `source` run again keeps every binding and leaves the C alone.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

ROOT = Path(__file__).resolve().parent.parent
GB_LIFT = ROOT / "tools" / "gb_lift.py"
FRAMES = 120
PRESET = "x64-release" if os.name == "nt" else "release"

# Results live in HRAM so that `--dump-state` shows them (hram_ff80_ff90).
VBLANK_FLAG, LOOPS, TOTAL, PARKED, PEEKED, LEFT, WAITS = range(0x80, 0x87)
BUFFER = 0xC100


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
    [0x3E, 0x01, 0xE0, 0xFF],                # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    [0x21, BUFFER & 0xFF, BUFFER >> 8],      # ld hl, BUFFER
    [0x01, 0x08, 0x00],                      # ld bc, 8
    [0xF0, LOOPS],                           # ldh a, [LOOPS]
    [0xCD], ("abs", "Fill"),
    [0xCD], ("abs", "Sum"),
    [0x3E, 0x05, 0xCD], ("abs", "Park"),     # ld a, 5
    [0x0E, 0x07, 0xCD], ("abs", "Peek"),     # ld c, 7
    [0xE0, PEEKED],
    [0xCD], ("abs", "WaitFrame"),
    bump(LOOPS),
    [0x18], ("rel", "MainLoop"),

    ("label", "VBlank"),
    [0xF5, 0x3E, 0x01, 0xE0, VBLANK_FLAG, 0xF1, 0xD9],  # push af / flag = 1 / pop af / reti

    # Fills BC bytes from HL with A. It keeps DE on the stack meanwhile.
    ("label", "Fill"),
    [0xD5, 0x57],                            # push de / ld d, a
    ("label", "FillLoop"),
    [0x7A, 0x22, 0x0B, 0x78, 0xB1, 0x20], ("rel", "FillLoop"),  # ld a,d / ld [hli],a / dec bc / ld a,b / or c / jr nz
    [0xD1, 0xC9],                            # pop de / ret

    # Stays as the tool wrote it: the one the C calls through the machine.
    ("label", "Double"),
    [0x87, 0xC9],                            # add a, a / ret

    # TOTAL += Double(LOOPS). B is left holding the doubled number.
    ("label", "Sum"),
    [0xF0, LOOPS, 0xCD], ("abs", "Double"),
    ("label", "SumBack"),
    [0x47, 0xF0, TOTAL, 0x80, 0xE0, TOTAL],  # ld b, a / ldh a, [TOTAL] / add a, b / ldh [TOTAL], a
    [0xC9],

    # LEFT = A + 1, with A parked in a byte of RAM on the way: a register in
    # all but name.
    ("label", "Park"),
    [0xE0, PARKED, 0x3C, 0xE0, LEFT, 0xF0, PARKED],  # ldh [PARKED], a / inc a / ldh [LEFT], a / ldh a, [PARKED]
    [0xC9],

    # A = C, read back out of what it pushed: the stack as data.
    ("label", "Peek"),
    [0xC5, 0xF8, 0x00, 0x7E, 0xC1],          # push bc / ld hl, sp+0 / ld a, [hl] / pop bc
    [0xC9],

    ("label", "WaitFrame"),
    [0xAF, 0xE0, VBLANK_FLAG],
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    [0xF0, VBLANK_FLAG, 0xA7, 0x28], ("rel", "WaitFrameLoop"),
    bump(WAITS),
    [0xC9],
]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble(0x150, PROGRAM)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"LIFT HAND"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


# The machine the C runs on, in src/ itself: a file with no function of the
# ROM's in it is code the functions call, there as well as in a folder.
MACHINE_C = '''/* The machine the game's own C runs on. */
#include "native/game.h"

GBContext* gb;
'''

GAME_H = '''/* The game, as C. */
#ifndef NATIVE_GAME_H
#define NATIVE_GAME_H

#include "gblift.h"

extern GBContext* gb;

void Fill(uint16_t to, uint16_t count, uint8_t value);
uint8_t Sum(void);
void Park(uint8_t value);
void WaitFrame(void);

#endif
'''


def game_c(labels: dict[str, int], fill_value: str = "value") -> str:
    return f'''/* The game, as C: its variables by name, its functions with arguments. */
#include "game.h"
#include "variables.h"

/* Fills `count` bytes from `to` on. A count of 0 fills 65,536. */
void Fill(uint16_t to, uint16_t count, uint8_t value) {{
    do {{
        lift_write8(gb, to++, {fill_value});
    }} while (--count);
}}

/* Adds twice the number of turns to the total, and gives the total.
 * Doubling is code of the ROM's that nobody has written here. */
uint8_t Sum(void) {{
    gb->a = hLoops;
    lift_call(gb, 0x{labels["Double"]:04X}, 0x{labels["SumBack"]:04X});
    hTotal = (uint8_t)(hTotal + gb->a);
    return hTotal;
}}

/* The original parks `value` in a byte of RAM on the way. This does not. */
void Park(uint8_t value) {{
    hLeft = (uint8_t)(value + 1u);
}}

void WaitFrame(void) {{
    hVBlankFlag = 0;
    do {{
        lift_halt(gb);
    }} while (!hVBlankFlag);
    hWaits++;
}}
'''


VARIABLES_H = f'''/* The game's variables: each is the byte at its address. */
#ifndef NATIVE_VARIABLES_H
#define NATIVE_VARIABLES_H

#define RAM(address) (*lift_ram(gb, (address)))
#define hVBlankFlag RAM(0xFF{VBLANK_FLAG:02X})
#define hLoops RAM(0xFF{LOOPS:02X})
#define hTotal RAM(0xFF{TOTAL:02X})
#define hLeft RAM(0xFF{LEFT:02X})
#define hWaits RAM(0xFF{WAITS:02X})

#endif
'''


def bindings(labels: dict[str, int]) -> dict[str, str]:
    """What takes the place of each function's lifted text in the bank's file."""
    def declared(name: str) -> str:
        return f"GBLIFT_FUNCTION(0x00, 0x{labels[name]:04X}, {name}_binding)"

    return {
        "Fill": f'''/* Written by hand: Fill. Out: HL past the area, BC = 0, A = 0, the flags of the last `or`. */
{declared("Fill")} {{
    const uint16_t count = ctx->bc;
    gb = ctx;
    Fill(ctx->hl, count, ctx->a);
    ctx->hl = (uint16_t)(ctx->hl + count);
    ctx->bc = 0;
    ctx->a = 0;
    ctx->f_z = 1; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0;
    lift_ret(ctx);
}}''',
        "Sum": f'''/* Written by hand: Sum. Out: A = the total. B and the flags are left over. */
{declared("Sum")} {{
    gb = ctx;
    ctx->a = Sum();
    lift_undefined(ctx, LIFT_REG_B | LIFT_FLAGS);
    lift_ret(ctx);
}}''',
        "Park": f'''/* Written by hand: Park. Out: A as it came, the flags of the `inc a`. */
{declared("Park")} {{
    gb = ctx;
    Park(ctx->a);
    ctx->f_z = (uint8_t)(ctx->a == 0xFF); ctx->f_n = 0; ctx->f_h = (uint8_t)((ctx->a & 0x0F) == 0x0F);
    lift_ret(ctx);
}}''',
        "Peek": f'''/* Written by hand, and looking where the original looks: but nothing was pushed there. */
{declared("Peek")} {{
    ctx->hl = (uint16_t)(ctx->sp - 2u);
    ctx->a = lift_read8(ctx, ctx->hl);
    ctx->f_z = 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0;
    lift_ret(ctx);
}}''',
        "WaitFrame": f'''/* Written by hand: WaitFrame. Out: A = the waits so far, the flags of the `inc a`. */
{declared("WaitFrame")} {{
    gb = ctx;
    WaitFrame();
    ctx->a = *lift_ram(ctx, 0xFF{WAITS:02X});
    ctx->f_z = (uint8_t)(ctx->a == 0); ctx->f_n = 0; ctx->f_h = (uint8_t)((ctx->a & 0x0F) == 0); ctx->f_c = 0;
    lift_ret(ctx);
}}''',
    }


def run(command: list, cwd: Path | None = None, timeout: int = 900,
        expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    result = subprocess.run([str(part) for part in command], cwd=cwd, capture_output=True, text=True,
                            timeout=timeout)
    if result.returncode not in expect:
        raise AssertionError(f"exit {result.returncode} from {command!r}\n{result.stdout[-4000:]}\n"
                             f"{result.stderr[-4000:]}")
    return result


def tool(*arguments, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
    return run([sys.executable, GB_LIFT, *arguments], expect=expect)


def play(executable: Path, root: Path, name: str) -> dict:
    """Run the game headless for FRAMES frames; its state at the end."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    run([executable, "--headless", "--no-audio", "--limit-frames", str(FRAMES),
         "--save-dir", saves, "--dump-state", dump], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8"))


def function_text(text: str, address: int) -> str:
    """One function of a lifted file: from its gbfn line to its closing brace."""
    start = text.index(f"/* gbfn:v1:0000:{address:04x}")
    return text[start:text.index("\n}\n", start) + 2]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()
    cmake_args = [f"--cmake-arg={item}" for item in args.cmake_arg]

    scratch = tempfile.TemporaryDirectory(prefix="gblift-hand-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom, labels = make_rom()
        rom_path = root / "lift_by_hand.gb"
        rom_path.write_bytes(rom)
        at = {name: f"00:{address:04X}" for name, address in labels.items()}
        by_hand = ("Fill", "Sum", "Park", "Peek", "WaitFrame")

        def build(*more: str, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
            return tool("build", folder, "--gbrecomp", args.gbrecomp, "--trials", "200", *cmake_args, *more,
                        expect=expect)

        plain = ["--no-scan", "--pointer-scan", "off"]
        export = root / "export.json"
        run([args.gbrecomp, rom_path, *plain, "--lift-export", export])
        symbols = root / "names.sym"
        symbols.write_text("".join(f"{at[name].lower()} {name}\n" for name in (*by_hand, "Double")), encoding="utf-8")
        folder = root / "source"
        tool("source", export, "-o", folder, "--symbols", symbols, "--rom", rom_path, "--", *plain)
        assert (folder / "scratch.txt").is_file(), "the folder has no scratch.txt"
        bank = folder / "src" / "lifted_bank_00.c"
        executable = folder / "build" / PRESET / "game" / ("game.exe" if os.name == "nt" else "game")

        # ---- The game as the tool wrote it: what the rest is held to.
        built = build().stdout
        assert "0 are not as the tool wrote them" in built, built
        original = play(executable, root, "original")
        hram = original["hram_ff80_ff90"]
        loops = hram[LOOPS - 0x80]
        assert loops >= FRAMES - 4 and hram[WAITS - 0x80] == loops and hram[PEEKED - 0x80] == 7 and \
            hram[LEFT - 0x80] == 6 and hram[PARKED - 0x80] == 5, hram
        assert original["wram_bank_0_c000_cfff"][0x100:0x108] == [loops] * 8, "the buffer is not filled"

        # ---- Five functions written by hand: C under src/, bindings where
        # the lifted functions stood.
        native = folder / "src" / "native"
        native.mkdir()
        (folder / "src" / "machine.c").write_text(MACHINE_C, encoding="utf-8", newline="\n")
        (native / "game.h").write_text(GAME_H, encoding="utf-8", newline="\n")
        (native / "variables.h").write_text(VARIABLES_H, encoding="utf-8", newline="\n")
        (native / "game.c").write_text(game_c(labels), encoding="utf-8", newline="\n")
        text = bank.read_text(encoding="utf-8")
        assert '#include "lifted_names.h"\n' in text, text[:400]
        text = text.replace('#include "lifted_names.h"\n', '#include "lifted_names.h"\n#include "native/game.h"\n', 1)
        for name, binding in bindings(labels).items():
            lifted = function_text(text, labels[name])
            first_line = lifted.split("\n", 1)[0]
            text = text.replace(lifted, first_line + "\n" + binding)
        bank.write_text(text, encoding="utf-8", newline="\n")

        # Park does not park. The byte it leaves alone is compared, and the
        # build stops, until scratch.txt says it is a register in all but name.
        refused = build(expect=(1,))
        assert "5 are not as the tool wrote them" in refused.stdout, refused.stdout
        assert re.search(r"mismatch +gbfn:v1:0000:%04x +Park_binding" % labels["Park"], refused.stdout), refused.stdout
        assert f"hram FF{PARKED:02X}" in refused.stdout, refused.stdout
        assert at["Park"] in refused.stderr and "Nothing was built" in refused.stderr, refused.stderr
        for name in ("Fill", "Sum", "WaitFrame"):
            assert at[name] not in refused.stderr, refused.stderr
            assert re.search(rf"match +gbfn:v1:0000:{labels[name]:04x} +{name}_binding", refused.stdout), refused.stdout

        scratch_file = folder / "scratch.txt"
        heading = scratch_file.read_text(encoding="utf-8")
        scratch_file.write_text(heading + "somewhere  not an address\n", encoding="utf-8")
        assert "scratch.txt:" in build(expect=(2,)).stderr
        scratch_file.write_text(heading + f"FF{PARKED:02X}  where Park keeps A while it works\n", encoding="utf-8")
        built = build().stdout
        # The list changed, so every function somebody changed is checked
        # again, and no other.
        assert "checking 5 of" in built, built
        assert re.search(r"match +gbfn:v1:0000:%04x +Park_binding" % labels["Park"], built), built
        assert ", 0 differ" in built, built
        contract = json.loads((folder / ".lift" / "contract.json").read_text(encoding="utf-8"))["functions"]
        assert sorted(contract) == sorted(at[name] for name in by_hand), contract
        assert all(entry["ignore"] == [f"FF{PARKED:02X}"] for entry in contract.values()), contract

        # Peek's original reads what it pushed, and its C finds something
        # else there in every state: none of them is a mismatch, the checker
        # has no verdict, and the game runs the ROM's code there. That is said.
        log = (folder / ".lift" / "check.log").read_text(encoding="utf-8")
        peek = log[log.index("Peek_binding"):]
        assert log[:log.index("Peek_binding")].rstrip().endswith(f"gbfn:v1:0000:{labels['Peek']:04x}") and \
            "used_its_stack_as_data" in peek.split("\n", 2)[1], log
        assert "1 of the functions somebody changed are not in the game" in built and \
            any(line.strip().startswith(at["Peek"]) for line in built.splitlines()), built

        # ---- The game with the C in it ends as the original does: the
        # variables, the buffer, the turns. PARKED is the byte nobody parks in
        # now. (The registers are not compared: Sum leaves B undefined, and a
        # run stops inside WaitFrame, whose C keeps nothing in them.)
        game = play(executable, root, "by-hand")
        expected = list(hram)
        expected[PARKED - 0x80] = 0
        assert game["hram_ff80_ff90"] == expected, (hram, game["hram_ff80_ff90"])
        assert game["wram_bank_0_c000_cfff"] == original["wram_bank_0_c000_cfff"], "work RAM differs"

        # ---- An edit to the C a binding calls. The binding's own text is
        # what it was; it is checked again all the same, and a wrong edit
        # stops the build with the game as it was.
        before = executable.stat().st_mtime_ns
        (native / "game.c").write_text(game_c(labels, "(uint8_t)(value ^ 1u)"), encoding="utf-8", newline="\n")
        refused = build(expect=(1,))
        assert "checking 5 of" in refused.stdout, refused.stdout
        assert re.search(r"mismatch +gbfn:v1:0000:%04x +Fill_binding" % labels["Fill"], refused.stdout), refused.stdout
        assert at["Fill"] in refused.stderr and at["Sum"] not in refused.stderr, refused.stderr
        assert executable.stat().st_mtime_ns == before, "a refused build built the game"
        (native / "game.c").write_text(game_c(labels), encoding="utf-8", newline="\n")
        built = build().stdout
        assert "checking 5 of" in built and ", 0 differ" in built, built
        # Nothing changed: nothing is checked.
        assert "checking" not in build().stdout

        # ---- A function of the ROM's has its place in a bank's file.
        stray = native / "stray.c"
        stray.write_text('#include "gblift.h"\nGBLIFT_FUNCTION(0x00, 0x%04X, Double_elsewhere) { lift_ret(ctx); }\n'
                         % labels["Double"], encoding="utf-8", newline="\n")
        refused = build(expect=(2,)).stderr
        assert "src/native/stray.c declares Double_elsewhere" in refused, refused
        stray.unlink()

        # ---- The folder brought up to date: the bindings are kept as code
        # somebody changed, and the C they call is not the tool's to touch.
        kept = bank.read_text(encoding="utf-8")
        mine = {path: path.read_bytes() for path in (folder / "src" / "machine.c", native / "game.c",
                                                      native / "game.h", native / "variables.h")}
        again = tool("source", export, "-o", folder).stdout
        assert "kept      5 functions somebody changed" in again, again
        assert bank.read_text(encoding="utf-8") == kept
        assert all(path.read_bytes() == data for path, data in mine.items()), "the C under src/ was touched"
        assert "checking" not in build().stdout
        assert play(executable, root, "again")["hram_ff80_ff90"] == expected
    finally:
        if scratch:
            scratch.cleanup()

    print("five functions were written by hand as C; the checker held each to the ROM, and the game ends the same")
    return 0


if __name__ == "__main__":
    sys.exit(main())
