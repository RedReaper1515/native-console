#!/usr/bin/env python3
"""Lifted C as a source folder somebody works in, end to end.

Makes a small game, has `gb_lift.py source` make its source folder from an
export, and then does to the folder what a modder does:

- opens it: the folder alone has to configure and compile, and running
  `source` again has to change nothing;
- builds it untouched: the game `gb_lift.py build` makes has to end as the
  original does, and has to compile the folder's own files, not a copy;
- comments functions and tidies another: all have to be kept, and all
  still have to match the ROM's code;
- changes one on purpose: `build` has to refuse it, building nothing, until
  `changes.txt` says it is meant, and the game then has to end differently in
  just the way the change says;
- changes one the checker cannot run: `build` has to say it is not in the
  game, and build it in once `changes.txt` names it;
- edits again and builds without the tool, as an editor would: the game
  has to run the edit;
- updates the folder from another export of the same ROM, in which one
  function is gone, two are more code than they were and one is less: a
  function whose code somebody changed has to be kept exactly; one somebody
  only commented has to be written afresh, with a name the folder's
  `names.sym` now has, and its comments put back where they stood; one whose
  comment is about an instruction it no longer has has to be left as it is
  and named; what nobody touched written afresh; the function with no place
  put aside with its text; a function taken out by hand left out until it is
  asked back; and the game built from the result still has to carry the
  change;
- switches the folder to `--time block` and back: the commented functions
  have to follow with their comments, the changed ones stay as they are,
  and the way back has to give the very file it started from.
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
VBLANK_FLAG, LOOPS, COUNT, SCALED, SCORE, WHOLE, MIDDLE, EXTRA, WAITS, READY = range(0x80, 0x8A)
FRONT, REAR, LONG, TAIL, SPUN = range(0x8A, 0x8F)
SETUP = 0xC120  # two bytes the start-up writes and NeedsSetup waits for


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
    [0x3E, 0xA5, 0xEA, SETUP & 0xFF, SETUP >> 8],              # two bytes only
    [0x3E, 0x5A, 0xEA, (SETUP + 1) & 0xFF, (SETUP + 1) >> 8],  # this start-up writes
    [0x3E, 0x01, 0xE0, 0xFF],                # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    [0xCD], ("abs", "NeedsSetup"),
    [0xCD], ("abs", "Count"),
    [0xCD], ("abs", "Scale"),
    [0xCD], ("abs", "Bonus"),
    [0xCD], ("abs", "Whole"),
    [0xCD], ("abs", "Front"),
    [0xCD], ("abs", "Long"),
    [0xCD], ("abs", "Spin"),
    [0xCD], ("abs", "Extra"),
    [0xCD], ("abs", "WaitFrame"),
    bump(LOOPS),
    [0x18], ("rel", "MainLoop"),

    ("label", "VBlank"),                     # the one that is renamed where it stands
    [0xF5, 0x3E, 0x01, 0xE0, VBLANK_FLAG, 0xF1, 0xD9],  # push af / flag = 1 / pop af / reti

    ("label", "Count"),                      # the one that gets a comment
    bump(COUNT),
    [0xC9],

    ("label", "Scale"),                      # the one that is tidied
    [0xF0, COUNT, 0x47],                     # ldh a, [COUNT] / ld b, a
    [0xF0, SCALED, 0xA8, 0xE0, SCALED],      # SCALED ^= b
    [0xC9],

    ("label", "Bonus"),                      # the one that is changed on purpose
    [0xF0, SCORE, 0xC6, 0x03, 0xE0, SCORE],  # SCORE += 3
    [0xC9],

    # Three pairs that are two functions in one export and one in the other:
    # the first of each runs on into the second, which is a function only
    # where an annotation says so. Middle and Rear are functions in the first
    # export, so Whole and Front are more code in the second; Tail is one in
    # the second, so Long is less code there.
    ("label", "Whole"),                      # commented and tidied
    bump(WHOLE),
    ("label", "Middle"),
    bump(MIDDLE),
    [0xC9],

    ("label", "Front"),                      # only commented
    bump(FRONT),
    ("label", "Rear"),
    bump(REAR),
    [0xC9],

    ("label", "Long"),                       # only commented, the tail too
    bump(LONG),
    ("label", "Tail"),
    bump(TAIL),
    [0xC9],

    # The one with a comment everywhere a comment can stand.
    ("label", "Spin"),
    [0x06, 0x03],                            # ld b, 3
    ("label", "SpinLoop"),
    [0x05, 0x20], ("rel", "SpinLoop"),       # dec b / jr nz
    [0x78, 0xE0, SPUN],                      # ld a, b / ldh [SPUN], a
    [0xC9],

    ("label", "Extra"),                      # the one that is taken out by hand
    bump(EXTRA),
    [0xC9],

    ("label", "WaitFrame"),                  # the one nobody touches
    [0xAF, 0xE0, VBLANK_FLAG],
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    [0xF0, VBLANK_FLAG, 0xA7, 0x28], ("rel", "WaitFrameLoop"),
    bump(WAITS),
    [0xC9],

    # The one the checker cannot run: it spins, with no HALT, until both
    # bytes of the start-up are there, and no memory made up from nothing
    # has them.
    ("label", "NeedsSetup"),
    [0xFA, SETUP & 0xFF, SETUP >> 8, 0xFE, 0xA5, 0x20], ("rel", "NeedsSetup"),
    [0xFA, (SETUP + 1) & 0xFF, (SETUP + 1) >> 8, 0xFE, 0x5A, 0x20], ("rel", "NeedsSetup"),
    bump(READY),
    [0xC9],
]


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble(0x150, PROGRAM)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13F] = b"LIFT SOURCE"
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


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


def play(executable: Path, root: Path, name: str, *flags: str) -> tuple[dict, str]:
    """Run the game headless for FRAMES frames; the state dump and stderr."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    result = run([executable, "--headless", "--no-audio", "--limit-frames", str(FRAMES),
                  "--save-dir", saves, "--dump-state", dump, *flags], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8")), result.stderr


def lifted_count(log: str) -> int:
    """How many lifted functions the game says it has (`--report-lift`)."""
    return int(re.search(r"functions=(\d+)", next(line for line in log.splitlines() if "[LIFT]" in line)).group(1))


def function_text(text: str, address: int) -> str:
    """One function of a lifted file: from its gbfn line to its closing brace."""
    start = text.index(f"/* gbfn:v1:0000:{address:04x}")
    return text[start:text.index("\n}\n", start) + 3]


def with_lines_above(function: str, marker: str, added: list[str]) -> str:
    """`function` with `added` put above its first line that has `marker`,
    as far in as that line is."""
    lines = function.split("\n")
    at = next(number for number, line in enumerate(lines) if marker in line)
    pad = lines[at][:len(lines[at]) - len(lines[at].lstrip())]
    lines[at:at] = [pad + line if line else "" for line in added]
    return "\n".join(lines)


def same_but(original: dict, other: dict, allowed: set[int]) -> list[str]:
    """What differs between two state dumps, apart from the flags (the
    structured style computes none that nothing reads), the bytes an
    interrupt left below the stack pointer, and the HRAM bytes in `allowed`."""
    top = original["sp"] - 0xD000
    differing = []
    for key in original:
        a, b = original[key], other.get(key)
        if key == "f" or a == b:
            continue
        if key == "wram_bank_1_d000_dfff" and a[:top - 32] + a[top:] == b[:top - 32] + b[top:]:
            continue
        if key.startswith("hram_") and all(x == y or 0x80 + i in allowed for i, (x, y) in enumerate(zip(a, b))):
            continue
        differing.append(key)
    return differing


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()
    cmake_args = [f"--cmake-arg={item}" for item in args.cmake_arg]

    scratch = tempfile.TemporaryDirectory(prefix="gblift-source-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom, labels = make_rom()
        rom_path = root / "lift_source.gb"
        rom_path.write_bytes(rom)
        at = {name: f"00:{address:04X}" for name, address in labels.items()}
        ident = {name: f"gbfn:v1:0000:{address:04x}" for name, address in labels.items()}

        def build(*more: str, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
            return tool("build", folder, "--gbrecomp", args.gbrecomp, "--trials", "200", *cmake_args, *more,
                        expect=expect)

        def declared(name: str, called: str = "") -> str:
            return f"GBLIFT_FUNCTION(0x00, 0x{labels[name]:04X}, {called or f'lifted_00_{labels[name]:04x}'})"

        # Two exports of the one ROM. In the first, annotations make functions
        # of the second halves of Whole and Front; in the second, of the
        # second half of Long.
        told, told_second = root / "told.annotations", root / "second.annotations"
        told.write_text(f"function {at['Middle']} Middle\nfunction {at['Rear']} Rear\n", encoding="utf-8")
        told_second.write_text(f"function {at['Tail']} Tail\n", encoding="utf-8")
        plain = ["--no-scan", "--pointer-scan", "off"]
        first = [*plain, "--annotations", str(told)]
        second = [*plain, "--annotations", str(told_second)]
        export_first, export_second = root / "first.json", root / "second.json"
        run([args.gbrecomp, rom_path, *first, "--lift-export", export_first])
        run([args.gbrecomp, rom_path, *second, "--lift-export", export_second])
        ids = {function["address"] for function in json.loads(export_first.read_text(encoding="utf-8"))["functions"]}
        ids_second = {function["address"]
                      for function in json.loads(export_second.read_text(encoding="utf-8"))["functions"]}
        assert {labels["Middle"], labels["Rear"]} <= ids and labels["Tail"] not in ids, "the first export is not as meant"
        assert not {labels["Middle"], labels["Rear"]} & ids_second and labels["Tail"] in ids_second, \
            "the two exports do not differ"

        # ---- The folder, from an export and a symbol file somebody brought.
        # The file names two functions and an address, and has another name
        # for the function the export itself names: the file's is used.
        symbols = root / "brought.sym"
        symbols.write_text(f"; a symbol file\n{at['Count'].lower()} Count\n{at['Bonus'].lower()} Bonus\n"
                           f"{at['Middle'].lower()} SecondHalf\n00:ff82 hCount\n", encoding="utf-8")
        folder = root / "source"
        made = tool("source", export_first, "-o", folder, "--symbols", symbols, "--rom", rom_path,
                    "--", *first).stdout
        bank = folder / "src" / "lifted_bank_00.c"
        for name in ("lift-source.json", "changes.txt", "names.sym", "CMakeLists.txt", "CMakePresets.json",
                     "include/gblift.h", "src/lifted_names.h", ".lift/export.json"):
            assert (folder / name).is_file(), f"the folder has no {name}"
        pristine = bank.read_text(encoding="utf-8")
        record = json.loads((folder / "lift-source.json").read_text(encoding="utf-8"))
        wanted = ("Count", "Scale", "Bonus", "Whole", "Middle", "Front", "Rear", "Long", "Spin", "Extra",
                  "WaitFrame", "NeedsSetup", "VBlank")
        for name in wanted:
            assert ident[name] in record["functions"], f"no record of {name}"
            assert f"GBLIFT_FUNCTION(0x00, 0x{labels[name]:04X}, " in pristine, f"{name} is not in the source"
        assert declared("Count", "Count") in pristine, "the brought name is not used"
        assert "hCount" in function_text(pristine, labels["Count"]), "the brought name of an address is not used"
        assert declared("Middle", "SecondHalf") in pristine, "the export's name was used where the folder has its own"
        assert record["analysis"][:3] == plain and Path(record["analysis"][4]) == told.resolve(), record["analysis"]
        assert "4 of the export's functions go by a name" in made, made
        total = len(record["functions"])
        assert f"{total} functions" in made and total > len(wanted), made

        # A folder that is not one of these is not written into.
        stray = root / "stray"
        stray.mkdir()
        (stray / "notes.txt").write_text("mine\n", encoding="utf-8")
        refused = tool("source", export_first, "-o", stray, expect=(2,)).stderr
        assert "is not empty" in refused and not (stray / "src").exists(), refused
        # A run that fails writes nothing: no folder is left half made.
        never = root / "never"
        failed = tool("source", export_first, "-o", never, "--symbols", symbols, "--restore", "00:7FF0",
                      expect=(2,)).stderr
        assert "cannot be restored" in failed and not never.exists(), failed

        # Made again, nothing changes: no function is written differently and
        # no file is touched.
        stamp = bank.stat().st_mtime_ns
        again = tool("source", export_first, "-o", folder).stdout
        assert "kept      0 functions" in again and "0 of them written differently" in again, again
        assert bank.read_text(encoding="utf-8") == pristine and bank.stat().st_mtime_ns == stamp

        # Opened in an editor before any game is built: the folder alone has
        # to configure and compile, with what the sources include found.
        run(["cmake", "--preset", PRESET, *args.cmake_arg], cwd=folder)
        run(["cmake", "--build", "--preset", PRESET], cwd=folder)
        assert not (folder / "game").exists()

        # ---- The game from the untouched folder ends as the original does.
        # NeedsSetup is the one function the checker cannot run, so it is the
        # one function that is not built in.
        built = build().stdout
        assert "generated in" in built and "0 are not as the tool wrote them" in built, built
        assert f"{total - 1} match the ROM's code, 0 differ, 1 the checker could not run" in built, built
        # The checker's line for it is in .lift/check.log, not on the screen.
        assert not any(line.startswith("inconclusive") for line in built.splitlines()), built
        assert f"inconclusive {ident['NeedsSetup']}" in (folder / ".lift" / "check.log").read_text(encoding="utf-8")
        build_dir = folder / "build" / PRESET / "game"
        executable = next(path for path in (build_dir / "game.exe", build_dir / "game") if path.exists())
        # The game compiles the folder's files where they are: its project
        # holds no copy of them.
        assert sorted(path.name for path in (folder / "game" / "lift").glob("*.c")) == ["lifted_in_src.c"]
        original, _ = play(executable, root, "original", "--no-lift")
        untouched, log = play(executable, root, "untouched", "--report-lift")
        assert lifted_count(log) == total - 1 and int(re.search(r" calls=(\d+)", log).group(1)) > 300, log
        hram = original["hram_ff80_ff90"]
        loops = hram[COUNT - 0x80]  # turns of the main loop begun
        assert loops > 50 and hram[SCORE - 0x80] == loops * 3 % 256 and hram[EXTRA - 0x80] == loops, hram
        assert hram[READY - 0x80] == loops and hram[TAIL - 0x80] == loops and hram[SPUN - 0x80] == 0, hram
        assert not same_but(original, untouched, set()), same_but(original, untouched, set())

        # ---- A modder's work.
        text = pristine

        def edit(name: str, change) -> None:
            nonlocal text
            before = function_text(text, labels[name] if isinstance(name, str) else name)
            after = change(before)
            assert after != before and text.count(before) == 1, name
            text = text.replace(before, after)

        # Count: a comment above the function, over the tool's own lines, and
        # one inside it.
        edit("Count", lambda body: "/* Counts the turns of the main loop. */\n" +
             body.replace(") {\n", ") {\n    /* one more turn */\n", 1))
        # Scale: its body tidied without changing what it does, with a blank
        # line in it.
        scale = function_text(text, labels["Scale"])
        loaded = next(line for line in scale.splitlines() if "ld b, a" in line)
        assert loaded.startswith("    ctx->b = ctx->a;"), loaded
        tidy = ("    const uint8_t turns = ctx->a;  /* tidied: \"}\" in a string is text */\n"
                "\n"
                "    ctx->b = turns;" + loaded[len("    ctx->b = ctx->a;"):])
        edit("Scale", lambda body: body.replace(loaded, tidy))
        # Whole: a comment, and a statement written another way.
        counted = next(line for line in function_text(text, labels["Whole"]).splitlines() if ": inc a */" in line)
        other_way = counted.replace("ctx->a++;", "ctx->a += 1;").replace(
            "ctx->a = lift_inc8(ctx, ctx->a);", "ctx->a = lift_inc8(ctx, (uint8_t)(ctx->a + 0));")
        assert other_way != counted, counted
        edit("Whole", lambda body: "/* The first half. */\n" + body.replace(counted, other_way))
        # Middle and NeedsSetup: a comment above.
        edit("Middle", lambda body: "/* The second half. */\n" + body)
        edit("NeedsSetup", lambda body: "/* Waits for the start-up. */\n" + body)
        # Front: only comments, one above the declaration and one inside.
        edit("Front", lambda body: with_lines_above(
            with_lines_above(body, "GBLIFT_FUNCTION(", ["/* The front half. */"]),
            ": inc a */", ["/* one more at the front */"]))
        # Long: only comments, one on each half. The second is about an
        # instruction that is another function's in the second export.
        long_lines = [line for line in function_text(text, labels["Long"]).splitlines() if ": inc a */" in line]
        assert len(long_lines) == 2, long_lines
        edit("Long", lambda body: with_lines_above(
            with_lines_above(body, long_lines[0], ["/* the long part */"]), long_lines[1], ["/* the tail part */"]))
        # Spin: only comments, everywhere one can stand. The block above is
        # where a describing pass puts it, under the tool's two lines.
        spin_above = ["/* Spins three times and says how it ended.", " * In: nothing. Out: SPUN is 0. */"]
        assert "    do {\n" in function_text(text, labels["Spin"]), function_text(text, labels["Spin"])

        def spin_comments(body: str) -> str:
            for marker, added in (("GBLIFT_FUNCTION(", spin_above), (": ld b, $03 */", ["/* Three turns. */"]),
                                  ("    do {", ["/* Count them down. */"]), (": dec b */", ["/* one turn */"]),
                                  (": ld a, b */", ["", "/* Say how it ended. */"])):
                body = with_lines_above(body, marker, added)
            assert body.endswith("\n}\n")
            return body[:-2] + "    /* and back */\n}\n"

        edit("Spin", spin_comments)
        # Four of the vectors, each a `ret`. A comment put at the end of a
        # statement's line, a comment of one's own that looks like the
        # tool's, and a line of the tool's taken out are changes to what the
        # tool wrote, not comments added; a line that begins as the tool's
        # line about what a function takes is a comment like any other.
        edit(0x0008, lambda body: body.replace("/* 0008: ret */", "/* 0008: ret */ /* mine */"))
        edit(0x0010, lambda body: with_lines_above(body, "/* 0010: ret */", ["/* 0010: mine */"]))
        edit(0x0018, lambda body: body.replace("/* takes nothing */\n", ""))
        edit(0x0020, lambda body: with_lines_above(body, "GBLIFT_FUNCTION(", ["/* takes nothing from anyone */"]))
        commented = text
        bank.write_text(commented, encoding="utf-8")
        touched = 12
        built = build().stdout
        assert f"checking {touched} of {total} functions" in built and \
            f"{touched} are not as the tool wrote them" in built, built
        assert f"{total - 1} match the ROM's code, 0 differ" in built and "generated in" not in built, built
        # The one the checker cannot run is not in the game, changed or not:
        # that is said, since an edit to it would not show.
        assert "1 of the functions somebody changed are not in the game" in built and \
            any(line.strip().startswith(at["NeedsSetup"]) for line in built.splitlines()), built
        tidied, log = play(executable, root, "tidied", "--report-lift")
        assert lifted_count(log) == total - 1, log
        assert not same_but(original, tidied, set()), same_but(original, tidied, set())

        # ---- A function changed on purpose: three a turn becomes five. The
        # checker calls it a mismatch, and `build` stops there, with the game
        # as it was, until changes.txt says the change is meant.
        bonus = function_text(commented, labels["Bonus"])
        adds = next(line for line in bonus.splitlines() if "add a, $03" in line)
        assert adds.count("0x03") == 1, adds
        modded = commented.replace(bonus, bonus.replace(adds, adds.replace("0x03", "0x05")))
        bank.write_text(modded, encoding="utf-8")
        before = executable.stat().st_mtime_ns
        refused = build(expect=(1,))
        assert f"checking 1 of {total} functions" in refused.stdout, refused.stdout
        assert "mismatch" in refused.stdout and "original 03, lifted 05" in refused.stdout, refused.stdout
        assert at["Bonus"] in refused.stderr and "changes.txt" in refused.stderr and \
            "Nothing was built" in refused.stderr, refused.stderr
        assert at["Count"] not in refused.stderr and at["Scale"] not in refused.stderr, refused.stderr
        assert executable.stat().st_mtime_ns == before, "a refused build built the game"
        # Asked again, it is refused again, without the checker having to run.
        refused = build(expect=(1,))
        assert "checking" not in refused.stdout and at["Bonus"] in refused.stderr, refused.stdout

        changes = folder / "changes.txt"
        heading = changes.read_text(encoding="utf-8")
        changes.write_text(heading + "not a function\n", encoding="utf-8")
        bad = build(expect=(2,)).stderr
        assert "changes.txt:5" in bad, bad
        # Declaring another function does not let this one through.
        changes.write_text(heading + f"{at['Extra']}  not this one\n", encoding="utf-8")
        refused = build(expect=(1,))
        assert at["Bonus"] in refused.stderr and "saw no difference" in refused.stdout, refused.stdout
        # Declared, it is the mod: built in, with the game not generated again.
        # A line for a function the folder does not have is pointed out.
        changes.write_text(heading + f"{at['Bonus']}  five a turn, not three\n00:7FF0  nothing there\n",
                           encoding="utf-8")
        built = build().stdout
        assert "differs from the ROM's code, as declared: five a turn, not three" in built, built
        assert "names 00:7FF0, which the folder has no function for" in built, built
        assert f"{total - 2} match the ROM's code, 1 differ" in built and "generated in" not in built, built
        changed, _ = play(executable, root, "changed")
        assert changed["hram_ff80_ff90"][SCORE - 0x80] == loops * 5 % 256, changed["hram_ff80_ff90"]
        assert not same_but(original, changed, {SCORE}), same_but(original, changed, {SCORE})
        off, _ = play(executable, root, "changed-off", "--no-lift")
        assert off["hram_ff80_ff90"] == hram, "--no-lift has to run the original code"

        # The function the checker cannot run, declared: built in on the
        # modder's word, which takes generating the game again.
        changes.write_text(heading + f"{at['Bonus']}  five a turn, not three\n"
                           f"{at['NeedsSetup']}\tcommented, and wanted in the game\n", encoding="utf-8")
        built = build().stdout
        assert "built in on your word: commented, and wanted in the game" in built and "generated in" in built, built
        assert "not in the game" not in built, built
        worded, log = play(executable, root, "worded", "--report-lift")
        assert lifted_count(log) == total, log
        assert not same_but(original, worded, {SCORE}), same_but(original, worded, {SCORE})

        # ---- Edited again and built as an editor builds it, without the
        # tool: what runs is the file that was edited.
        bank.write_text(modded.replace(adds.replace("0x03", "0x05"), adds.replace("0x03", "0x07")), encoding="utf-8")
        run(["cmake", "--build", "--preset", PRESET], cwd=folder)
        seven, _ = play(executable, root, "seven")
        assert seven["hram_ff80_ff90"][SCORE - 0x80] == loops * 7 % 256, seven["hram_ff80_ff90"]

        # ---- The folder brought up to date with the other export. Extra is
        # taken out by hand first, the interrupt's handler is given a name
        # where it stands, and names.sym gets names of its own: for a
        # function nobody touched, for one somebody tidied, and for an
        # address a function somebody only commented writes to.
        current = bank.read_text(encoding="utf-8")
        current = current.replace(function_text(current, labels["Extra"]), "")
        assert current.count(declared("VBlank")) == 1
        current = current.replace(declared("VBlank"), declared("VBlank", "OnVBlank"))
        # Saved as an editor may save it: other line ends, and spaces left at
        # the end of a line of a function nobody otherwise touched.
        halts = next(line for line in current.splitlines() if "lift_halt(ctx);" in line)
        current = current.replace(halts, halts + "   ")
        bank.write_text(current.replace("\n", "\r\n"), encoding="utf-8", newline="")
        with open(folder / "names.sym", "a", encoding="utf-8") as names:
            names.write(f"{at['WaitFrame']} WaitForFrame\n{at['Scale']} Rescale\n00:ff8e hSpun\n")
        (folder / "src" / "lifted_names.h").write_text(
            (folder / "src" / "lifted_names.h").read_text(encoding="utf-8").replace(
                "#endif", "#define hMine 0xFF8F\n#endif"), encoding="utf-8")
        long_before = function_text(current, labels["Long"])
        updated = tool("source", export_second, "-o", folder, "--", *second).stdout
        text = bank.read_text(encoding="utf-8")
        assert "\r" not in text

        # Where somebody changed code, the function is kept exactly: a tidied
        # body with its blank line, a change, a name given where the function
        # stands, a comment at the end of a statement, a comment that looks
        # like the tool's, a line of the tool's taken out. None of them gets
        # the name the folder now has, or anything else.
        assert tidy in text and adds.replace("0x03", "0x07") in text
        assert declared("Scale") in text, "a function somebody changed was renamed"
        assert declared("VBlank", "OnVBlank") in text
        for vector in (0x0008, 0x0010, 0x0018):
            assert function_text(text, vector) == function_text(commented, vector), f"{vector:04X} was not kept"
        # Where somebody only added comments, the function is the tool's to
        # write afresh, and the comments are put back where they stood. Spin
        # now writes to an address that has a name; every comment is where it
        # was: under the tool's two lines above, over a statement, over the
        # loop and inside it, after a blank line, and at the end.
        spin = function_text(text, labels["Spin"])
        assert "lift_write8(ctx, hSpun, ctx->a)" in spin and "0xFF8E" not in spin.replace("[$FF8E]", ""), spin
        assert re.search(r"/\* takes [^\n]*\*/\n" + re.escape("\n".join(spin_above)) + r"\nGBLIFT_FUNCTION\(", spin), spin
        for placed in ("    /* Three turns. */\n    ctx->b = 0x03;",
                       "    /* Count them down. */\n    do {\n        /* one turn */\n        ctx->b",
                       ";  /* %04X: jr nz, $%04X */\n\n    /* Say how it ended. */\n    ctx->a = ctx->b;" % (
                           labels["SpinLoop"] + 1, labels["SpinLoop"]),
                       ": ret */\n    /* and back */\n}\n"):
            assert placed in spin, (placed, spin)
        # Count is as it was, word for word: the tool has nothing new for it.
        assert function_text(text, labels["Count"]) == function_text(commented, labels["Count"])
        assert f"/* Counts the turns of the main loop. */\n/* {ident['Count']}" in text and "/* one more turn */" in text
        assert "/* Waits for the start-up. */" in text
        vector = function_text(text, 0x0020)
        assert vector == function_text(commented, 0x0020) and "/* takes nothing from anyone */" in vector, vector
        # Front is more code than it was, and its comments went with it into
        # the new text: the one inside stands over the same instruction.
        front = function_text(text, labels["Front"])
        assert "0xFF8B" in front and "0xFF8B" not in function_text(commented, labels["Front"]), front
        assert "/* The front half. */\nGBLIFT_FUNCTION(" in front, front
        assert re.search(r"    /\* one more at the front \*/\n[^\n]*/\* %04X: inc a \*/" % (labels["Front"] + 2), front), front
        # Long is less code than it was, and one of its comments is about an
        # instruction it no longer has: it is left exactly as it is, and named.
        assert function_text(text, labels["Long"]) == long_before, "a function with a comment that has no place was rewritten"
        assert "/* the tail part */" in text and "/* the long part */" in text
        stranded = next(line for line in updated.splitlines() if line.strip().startswith(at["Long"]))
        assert "kept as it is" in stranded and f"the instruction at {labels['Tail'] + 2:04X}" in stranded, updated
        # Written afresh, with the name the folder now has for it.
        assert declared("WaitFrame", "WaitForFrame") in text
        # Scale, Bonus, the handler, three vectors, Whole and Long are kept;
        # Count, NeedsSetup, Front, Spin and a vector were only commented,
        # Front and Spin of them written afresh; Middle has no place, Rear is
        # gone, Tail is new, Extra is out. The rest nobody touched.
        assert "kept      8 functions somebody changed" in updated, updated
        assert "carried   5 functions somebody only commented, 2 of them written afresh" in updated, updated
        assert "new       1 functions" in updated and "gone      1 functions" in updated, updated
        differently = int(re.search(rf"refreshed {total - 16} functions nobody touched, (\d+) of them written "
                                    "differently", updated).group(1))
        assert 1 <= differently < total - 16, updated
        # Not reconciled: Whole is more code than it was written from and is
        # kept as it is; Middle is no function any more, and its text is put
        # aside, comment and all.
        assert "could not reconcile 4" in updated, updated
        whole = next(line for line in updated.splitlines() if line.strip().startswith(at["Whole"]))
        assert "kept as it is" in whole and "not the code it was written from" in whole, updated
        middle = next(line for line in updated.splitlines() if line.strip().startswith(at["Middle"]))
        assert "no longer a function" in middle and "unplaced/lifted_bank_00.c" in middle, updated
        assert "/* The first half. */" in text and "/* The second half. */" not in text
        assert f"0x{labels['Middle']:04X}, SecondHalf)" not in text
        aside = (folder / "unplaced" / "lifted_bank_00.c").read_text(encoding="utf-8")
        assert "/* The second half. */" in aside and f"0x{labels['Middle']:04X}, SecondHalf)" in aside, aside
        # A header of the tool's that somebody wrote in: put aside, its name kept.
        assert "lifted_names.h was changed by hand" in updated, updated
        assert "#define hMine 0xFF8F" in (folder / "src" / "lifted_names.h").read_text(encoding="utf-8")
        assert "#define hMine 0xFF8F" in (folder / "unplaced" / "lifted_names.h").read_text(encoding="utf-8")
        # Taken out by hand: left out, and said.
        assert "left out  1 that somebody took out" in updated and at["Extra"] in updated, updated
        assert f"0x{labels['Extra']:04X}, " not in text
        # What the folder was is kept until the next update.
        assert (folder / ".lift" / "before-update" / "lifted_bank_00.c").read_bytes() == \
            current.replace("\n", "\r\n").encode("utf-8")
        record = json.loads((folder / "lift-source.json").read_text(encoding="utf-8"))
        assert record["analysis"][:3] == plain and Path(record["analysis"][4]) == told_second.resolve()
        last = record["last_update"]
        assert last["shape_changed"] == [ident["Whole"]] and last["not_placed"] == [ident["Middle"]] and \
            last["removed_by_hand"] == [ident["Extra"]] and last["comment_stranded"] == [ident["Long"]] and \
            last["carried"] == sorted([ident["Front"], ident["Spin"]]), last

        # Updated again with nothing changed in between, it stays as it is.
        settled = tool("source", export_second, "-o", folder).stdout
        assert "kept      8 functions" in settled and "could not reconcile 1:" in settled, settled
        assert "carried   5 functions somebody only commented, 0 of them written afresh" in settled, settled
        assert "0 of them written differently" in settled, settled
        assert "left out  1" in settled and bank.read_text(encoding="utf-8") == text
        # Asked back, the function taken out is there again, and so is the
        # tool's version of the one that was tidied, under its new name.
        restored = tool("source", export_second, "-o", folder, "--restore", at["Extra"],
                        "--restore", at["Scale"]).stdout
        text = bank.read_text(encoding="utf-8")
        assert "restored  2" in restored and f"0x{labels['Extra']:04X}, " in text, restored
        assert declared("Scale", "Rescale") in text and "uint8_t turns" not in text
        assert text.index(f"0x{labels['Spin']:04X}, ") < text.index(f"0x{labels['Extra']:04X}, ") < \
            text.index(f"0x{labels['WaitFrame']:04X}, "), "a function put back is not where it belongs"

        # The game from the updated folder: generated again, since the
        # analysis is another; what was written afresh, comments or no,
        # checked again, and still a match; the change still in it.
        built = build().stdout
        assert "generated in" in built, built
        assert f"{total - 3} match the ROM's code, 1 differ, 1 the checker could not run" in built, built
        checked = int(re.search(rf"checking (\d+) of {total - 1} functions", built).group(1))
        looked_at = {entry["function"] for entry in json.loads(
            (folder / ".lift" / "check.json").read_text(encoding="utf-8"))["results"]}
        assert checked == len(looked_at) < total - 1, built
        # Whole and Long are other code now, Extra and Scale are the tool's
        # again, WaitFrame was written afresh and so were Front and Spin with
        # their comments, Bonus was edited after the checker last saw it and
        # the handler renamed; Count is as it was when it was last checked.
        for name, again in (("Whole", True), ("Long", True), ("Extra", True), ("Scale", True), ("WaitFrame", True),
                            ("Front", True), ("Spin", True), ("Bonus", True), ("VBlank", True), ("Tail", True),
                            ("Count", False), ("NeedsSetup", False)):
            assert (ident[name] in looked_at) == again, (name, sorted(looked_at))
        record = json.loads((folder / "lift-source.json").read_text(encoding="utf-8"))
        for name in ("Whole", "Long", "Front", "Spin"):
            assert record["functions"][ident[name]]["verdict"] == "match", name
        final, log = play(executable, root, "final", "--report-lift")
        assert lifted_count(log) == total - 1, log
        assert final["hram_ff80_ff90"][SCORE - 0x80] == loops * 7 % 256, final["hram_ff80_ff90"]
        assert not same_but(original, final, {SCORE}), same_but(original, final, {SCORE})

        # ---- The folder switched to the other way of keeping time. The
        # functions somebody only commented follow, comments and all; the
        # ones whose code somebody changed stay as they are.
        exact = bank.read_text(encoding="utf-8")
        switched = tool("source", export_second, "-o", folder, "--time", "block").stdout
        block = bank.read_text(encoding="utf-8")
        assert json.loads((folder / "lift-source.json").read_text(encoding="utf-8"))["time"] == "block"
        written_afresh = int(re.search(r"carried   5 functions somebody only commented, (\d+) of them written afresh",
                                       switched).group(1))
        assert written_afresh >= 4 and "kept      7 functions somebody changed" in switched, switched
        spin = function_text(block, labels["Spin"])
        assert "lift_step" not in next(line for line in spin.splitlines() if ": ld b, $03 */" in line), spin
        for placed in ("\n".join(spin_above) + "\nGBLIFT_FUNCTION(",
                       "    /* Three turns. */\n    ctx->b = 0x03;",
                       "    /* Count them down. */\n    do {\n        /* one turn */\n        ctx->b",
                       "\n\n    /* Say how it ended. */\n    ctx->a = ctx->b;",
                       ": ret */\n    /* and back */\n}\n"):
            assert placed in spin, (placed, spin)
        count = function_text(block, labels["Count"])
        assert "    /* one more turn */\n" in count and "lift_step(ctx, 0x%04X" % (labels["Count"] + 2) not in count, count
        assert f"/* Counts the turns of the main loop. */\n/* {ident['Count']}" in block
        for name in ("Bonus", "Whole", "Long"):
            assert function_text(block, labels[name]) == function_text(exact, labels[name]), name
        built = build().stdout
        assert f"{total - 3} match the ROM's code, 1 differ, 1 the checker could not run" in built, built
        assert "generated in" not in built and ident["Spin"] in (folder / ".lift" / "check.json").read_text(
            encoding="utf-8"), built
        timed, log = play(executable, root, "block", "--report-lift")
        results = timed["hram_ff80_ff90"]
        assert lifted_count(log) == total - 1 and results[COUNT - 0x80] > 50, log
        assert results[SCORE - 0x80] == results[COUNT - 0x80] * 7 % 256 and results[SPUN - 0x80] == 0, results
        # And back: the very file it was.
        tool("source", export_second, "-o", folder, "--time", "exact")
        assert bank.read_text(encoding="utf-8") == exact, "there and back is not where it started"
        text = exact

        # What no export of this game brings about, on text made up here: the
        # tool comes to write a loop as a label and a jump. A comment that
        # stood over a statement in the loop goes over that statement, as far
        # in as it now is; one that stood over the loop goes over the label.
        # And where the instruction a comment is about is written twice, the
        # comments are not put anywhere: the function is to be left alone.
        sys.path.insert(0, str(ROOT / "tools"))
        import gb_lift
        import gb_lift_source
        was = ["/* gbfn:v1:0000:1000  made_up  (3 instructions) */",
               "GBLIFT_FUNCTION(0x00, 0x1000, made_up) {",
               "    do {",
               "        ctx->a++;  /* 1000: inc a */",
               "    } while (lift_branch(ctx, ctx->a != 0, 0x1000, 12, 0x1003, 8));  /* 1001: jr nz, $1000 */",
               "    lift_ret(ctx); return;  /* 1003: ret */",
               "}"]
        now = ["/* gbfn:v1:0000:1000  made_up  (3 instructions) */",
               "/* takes a */",
               "GBLIFT_FUNCTION(0x00, 0x1000, made_up) {",
               "L_1000:;",
               "    ctx->a++;  /* 1000: inc a */",
               "    if (ctx->a != 0) goto L_1000;  /* 1001: jr nz, $1000 */",
               "    lift_ret(ctx); return;  /* 1003: ret */",
               "}"]
        mine = was[:1] + ["/* Above. */"] + was[1:2] + ["    /* Over the loop. */"] + was[2:3] + \
            ["        /* In the loop. */"] + was[3:5] + ["", "    /* Over the end. */"] + was[5:]
        taken = gb_lift_source.comments_added("\n".join(mine), gb_lift_source.digest("\n".join(was)))
        assert taken is not None and taken[0] == was and [above for above, _ in taken[1]] == [1, 2, 3, 5], taken
        put, lost = gb_lift_source.carry_comments(*taken, "\n".join(now), gb_lift)
        assert lost == "" and put == "\n".join(
            now[:2] + ["/* Above. */"] + now[2:3] + ["    /* Over the loop. */"] + now[3:4] +
            ["    /* In the loop. */"] + now[4:6] + ["", "    /* Over the end. */"] + now[6:]), put
        # With no loop and no label, the two land on the one statement, and
        # stay in the order they were written in.
        flat = now[:3] + now[4:5] + now[6:]
        put, lost = gb_lift_source.carry_comments(*taken, "\n".join(flat), gb_lift)
        assert lost == "" and put == "\n".join(
            flat[:2] + ["/* Above. */"] + flat[2:3] + ["    /* Over the loop. */", "    /* In the loop. */"] +
            flat[3:4] + ["", "    /* Over the end. */"] + flat[4:]), put
        twice = now[:5] + ["    ctx->a++;  /* 1000: inc a */"] + now[5:]
        assert gb_lift_source.carry_comments(*taken, "\n".join(twice), gb_lift) == (None, "1000")
        # A function's text is only "comments added" against the text it was
        # written as: against any other, it is somebody's code.
        assert gb_lift_source.comments_added("\n".join(mine), gb_lift_source.digest("\n".join(now))) is None

        # A function that is in the folder twice, and a folder of another
        # ROM, are refused.
        doubled = text + "\n" + function_text(text, labels["Count"])
        bank.write_text(doubled, encoding="utf-8")
        twice = build("--check-only", expect=(2,)).stderr
        assert "is lifted twice" in twice, twice
        twice = tool("source", export_second, "-o", folder, expect=(2,)).stderr
        assert "is lifted twice" in twice and bank.read_text(encoding="utf-8") == doubled, twice
        bank.write_text(text, encoding="utf-8")
        other = bytearray(rom)
        other[0x134:0x13F] = b"ANOTHER ROM"
        other[0x14D] = (-sum(other[0x134:0x14D]) - 25) & 0xFF
        other_path = root / "other.gb"
        other_path.write_bytes(other)
        export_other = root / "other.json"
        run([args.gbrecomp, other_path, *plain, "--lift-export", export_other])
        assert "another ROM" in tool("source", export_other, "-o", folder, expect=(2,)).stderr
        assert "not the ROM" in build("--rom", other_path, "--check-only", expect=(2,)).stderr
        assert bank.read_text(encoding="utf-8") == text
    finally:
        if scratch:
            scratch.cleanup()

    print("a source folder was made, edited, refused, declared, updated and built; the game carries the change")
    return 0


if __name__ == "__main__":
    sys.exit(main())
