#!/usr/bin/env python3
"""Signatures for a game's functions, end to end.

Assembly does not say what a function takes and what it gives back. This
makes a small game whose functions each pose one of the questions, has
`gb_lift.py signatures` answer them, and holds the answers to what the code
does:

- a register that is only saved and restored is kept, not taken;
- a register left over from the work is neither given nor kept;
- a flag left over that the next call adds in is a result: the carry out of
  one `Step` goes into the next, with nothing between them that changes it,
  and through `Twice` out of the `Add` it ends with;
- a function with a way in whose users nobody can list gives everything it
  changes, until the game has been played with `--lift-callers`: then it
  gives what is read where it was seen to return to, its line says so, and
  one that was never reached in play is left as it was;
- `build` holds every function to its line: a line that leaves out a
  register the function reads stops the build;
- the checker cannot tell whether a caller reads what a line calls left
  over, and the game can: run with `--lift-signatures`, a game whose lines
  are right ends as the original does, and one whose line calls the chained
  carry left over does not;
- a line somebody changed is kept when the tool runs again, and
  `--restore` brings the tool's back;
- a flag or a register that decides nothing but how long a function takes
  is taken: `Pace` is a jump to the instruction after it, `Pause` passes two
  `nop` where B is not zero, and both end the same either way. Held to a
  line without it, a function takes another time than the original, and
  that is a line that does not hold: `signatures` watches the function
  again, as it does for one that came out differently;
- a run is not recorded with `--lift-signatures`.

Before any of that, `read_after_return` is asked about a made-up export in
which every way a function can be reached through a helper stands once: a
helper the export kept whole, one it cut in two at the address it pushes,
a caller only a played game saw, and a place nothing accounts for.
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
FRAMES = 300
PRESET = "x64-release" if os.name == "nt" else "release"

# Results live in HRAM so that `--dump-state` shows them (hram_ff80_ff90).
VBLANK_FLAG, LOOPS, SUM, TWICE, ACC, LOOSE, WAITS = range(0x80, 0x87)
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
    [0x21], ("abs", "Loose"),                # ld hl, Loose / ld hl, Rare: their addresses are
    [0x21], ("abs", "Rare"),                 # loaded, so who jumps to them cannot be listed.
    [0x21, 0x00, 0x00],                      # Nobody does.
    [0x3E, 0x01, 0xE0, 0xFF],                # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    [0x21, BUFFER & 0xFF, BUFFER >> 8],      # ld hl, BUFFER
    [0x01, 0x08, 0x00],                      # ld bc, 8
    [0xF0, LOOPS],                           # ldh a, [LOOPS]
    [0xCD], ("abs", "Fill"),
    [0xF0, LOOPS, 0x06, 0x03],               # ldh a, [LOOPS] / ld b, 3
    [0xCD], ("abs", "Add"),
    [0xE0, SUM],
    [0xF0, LOOPS],
    [0xCD], ("abs", "Twice"),
    [0xE0, TWICE],
    [0xCD], ("abs", "Step"),                 # the carry of Twice's sum goes in...
    [0xCD], ("abs", "Step"),                 # ...and the carry of the first into the second
    [0xCD], ("abs", "Pace"),                 # ...and decides how long this takes, and nothing else
    [0xCD], ("abs", "Loose"),
    [0xE0, LOOSE],
    [0xCD], ("abs", "WaitFrame"),
    [0xAF, 0xC4], ("abs", "Rare"),           # xor a / call nz, Rare: a call that is never made
    [0xF0, LOOPS, 0x47],                     # ldh a, [LOOPS] / ld b, a
    [0xCD], ("abs", "Pause"),
    bump(LOOPS),
    [0x18], ("rel", "MainLoop"),

    ("label", "VBlank"),
    [0xF5, 0x3E, 0x01, 0xE0, VBLANK_FLAG, 0xF1, 0xD9],  # push af / flag = 1 / pop af / reti

    # Fills BC bytes from HL with A. DE is saved and restored, and never read.
    ("label", "Fill"),
    [0xD5, 0x57],                            # push de / ld d, a
    ("label", "FillLoop"),
    [0x7A, 0x22, 0x0B, 0x78, 0xB1, 0x20], ("rel", "FillLoop"),  # ld a,d / ld [hli],a / dec bc / ld a,b / or c / jr nz
    [0xD1, 0xC9],                            # pop de / ret

    ("label", "Add"),
    [0x80, 0xC9],                            # add a, b / ret

    # A + A. B is left holding what A was.
    ("label", "Twice"),
    [0x47, 0xCD], ("abs", "Add"),            # ld b, a / call Add
    [0xC9],

    # ACC += $90 + the carry flag as it comes; leaves the carry of that.
    ("label", "Step"),
    [0xF0, ACC, 0xCE, 0x90, 0xE0, ACC],      # ldh a, [ACC] / adc a, $90 / ldh [ACC], a
    [0xC9],

    # Gives 9 in A, and leaves it in B as well, which nothing reads.
    ("label", "Loose"),
    [0x3E, 0x09, 0x47, 0xC9],                # ld a, 9 / ld b, a / ret

    # The same, and never reached.
    ("label", "Rare"),
    [0x3E, 0x07, 0x47, 0xC9],                # ld a, 7 / ld b, a / ret

    # A jump to the instruction after it: a cycle longer with the carry set,
    # and the same in everything that comes out.
    ("label", "Pace"),
    [0x38, 0x00],                            # jr c, +0
    [0xC9],

    # A cycle longer where B is not zero. AF is saved and restored.
    ("label", "Pause"),
    [0xF5, 0x04, 0x05, 0x28], ("rel", "PauseDone"),  # push af / inc b / dec b / jr z, PauseDone
    [0x00, 0x00],                            # nop / nop
    ("label", "PauseDone"),
    [0xF1, 0xC9],                            # pop af / ret

    ("label", "WaitFrame"),
    [0xAF, 0xE0, VBLANK_FLAG],
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    [0xF0, VBLANK_FLAG, 0xA7, 0x28], ("rel", "WaitFrameLoop"),
    bump(WAITS),
    [0xC9],
]

# What the tool has to find, and why (the module's docstring says it in words).
EXPECTED = {
    "Fill": "takes a bc hl  gives -  keeps de",
    "Add": "takes a b  gives a fc  keeps bc de hl",
    "Twice": "takes a  gives a fc  keeps c de hl",
    "Step": "takes fc  gives fc  keeps bc de hl",
    "Loose": "takes -  gives a b  keeps c de hl flags",
    "Rare": "takes -  gives a b  keeps c de hl flags",
    "WaitFrame": "takes -  gives -  keeps bc de hl",
    "Pace": "takes fc  gives -  keeps a bc de hl flags",
    "Pause": "takes b  gives -  keeps a bc de hl flags",
}
# The two that read something for their time alone, and what: a bit an item,
# in the order of `ITEMS` in tools/gb_lift_signatures.py.
TIME_ONLY = {"Pace": 1 << 10, "Pause": 1 << 1}


def what_is_read_after() -> None:
    """`read_after_return` on an export made up for it."""
    sys.path.insert(0, str(ROOT / "tools"))
    import gb_lift_signatures as signatures

    A, D, E = 0x001, 0x008, 0x010
    EVERYTHING = signatures.EVERYTHING

    def identity(address: int) -> str:
        return f"gbfn:v1:0000:{address:04x}"

    def function(address: int, blocks: list, calls=(), entered_by=(), needs=(), always=()) -> dict:
        made = []
        for start, code, more in blocks:
            instructions, at = [], start
            for data in code:
                instructions.append({"address": at, "bytes": bytes(data).hex()})
                at += len(data)
            block = {"address": start, "instructions": instructions, "successors": [], "leaves_to": [],
                     "returns": False, "unknown_exit": False}
            block.update(more)
            made.append(block)
        return {"id": identity(address), "bank": 0, "address": address, "entered_by": list(entered_by),
                "registers": {"needs": list(needs), "always_writes": list(always)},
                "flags": {"needs": [], "always_writes": []},
                "calls": [dict(call, site=site, function=identity(to)) for site, to, call in calls],
                "jumps_out": [], "seen_jumps": [], "blocks": made}

    def call(to: int) -> list[int]:
        return [0xCD, to & 0xFF, to >> 8]

    RET, LD_A_D, LD_A_E, PUSH_BC, POP_BC, JP_HL = [0xC9], [0x7A], [0x7B], [0xC5], [0xC1], [0xE9]
    sets_a_d_e = [[0x3E, 0x01], [0x57], [0x5F], RET]  # ld a, 1 / ld d, a / ld e, a / ret

    def target(address: int, entered_by=("far_call",)) -> dict:
        return function(address, [(address, sets_a_d_e, {"returns": True})], entered_by=entered_by,
                        always=("a", "d", "e"))

    def caller(address: int, to: int, then: list[int], reaches: int | None = None) -> dict:
        """call TO / THEN / ret, the call going on to REACHES where TO is a helper."""
        more = {"reaches": {"bank": 0, "address": reaches, "function": identity(reaches)}} if reaches else {}
        return function(address, [(address, [call(to), then, RET], {"returns": True})], calls=[(address, to, more)])

    export = {"functions": [
        # A helper kept whole: it pushes $2005, jumps, and comes back there.
        caller(0x1000, 0x2000, LD_A_D, reaches=0x3000),
        function(0x2000, [(0x2000, [[0x01, 0x05, 0x20], PUSH_BC, JP_HL], {"jump_returns_to": [0x2005]}),
                          (0x2005, [POP_BC, RET], {"returns": True})], needs=("h", "l"), always=("b", "c")),
        target(0x3000),
        # A helper cut in two at the address it pushes: the rest is a function of its own.
        caller(0x1100, 0x2100, LD_A_E, reaches=0x3100),
        function(0x2100, [(0x2100, [[0x01, 0x05, 0x21], PUSH_BC, JP_HL], {})], needs=("h", "l"), always=("b", "c")),
        function(0x2105, [(0x2105, [POP_BC, RET], {"returns": True})], entered_by=("seen",), always=("b", "c")),
        target(0x3100),
        # A way in nobody can list, and one caller the export has.
        caller(0x1200, 0x3200, LD_A_D),
        target(0x3200, entered_by=("data_pointer",)),
        # Every way in listed, and none of them there: only a played game saw the caller.
        target(0x3300, entered_by=()),
        caller(0x1300, 0x3400, LD_A_E),
        target(0x3400, entered_by=()),
    ]}
    # The call at $1300 really went to $3300, by a way the export does not know.
    after, as_played = signatures.read_after_return(export)
    assert after[identity(0x3000)] == D, "a function called through a whole helper: what the helper's caller reads"
    assert after[identity(0x3100)] == E, "a function called through a helper cut in two"
    assert after[identity(0x3200)] == EVERYTHING and not as_played, "an unlisted way in, and no play"
    assert after[identity(0x3300)] == 0

    played = {identity(0x3200): {(0, 0x1203, None, 0, 0)}, identity(0x3300): {(0, 0x1303, None, 0, 0)}}
    after, as_played = signatures.read_after_return(export, played)
    assert after[identity(0x3200)] == D and as_played == {identity(0x3200)}, "what is read where it was seen to return"
    assert after[identity(0x3300)] == E, "a caller only the play saw is counted for a listed function too"
    assert after[identity(0x3000)] == D and after[identity(0x3100)] == E

    # Seen coming back at the rest of the cut helper, with nothing to say
    # which caller of the helper it was: what any of them reads.
    played = {identity(0x3200): {(0, 0x2105, None, 0, 0)}}
    after, as_played = signatures.read_after_return(export, played)
    assert after[identity(0x3200)] == D | E and identity(0x3200) in as_played, signatures.words(after[identity(0x3200)])

    # A place nothing accounts for: the middle of something.
    played = {identity(0x3200): {(0, 0x1203, None, 0, 0), (0, 0x1001, None, 0, 0)},
              identity(0x3300): {(0, 0x7777, None, 0, 0)}}
    after, as_played = signatures.read_after_return(export, played)
    assert after[identity(0x3200)] == EVERYTHING and after[identity(0x3300)] == EVERYTHING and not as_played
    assert A  # the bit is named for the reader


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble(0x150, PROGRAM)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13D] = b"LIFT SIGN"
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


def play(executable: Path, root: Path, name: str, *flags) -> tuple[list[int], str]:
    """Run the game headless for FRAMES frames; its results and what it said."""
    saves = root / f"saves-{name}"
    saves.mkdir(exist_ok=True)
    dump = root / f"{name}.json"
    result = run([executable, "--headless", "--no-audio", "--limit-frames", str(FRAMES),
                  "--save-dir", saves, "--dump-state", dump, *flags], timeout=180)
    return json.loads(dump.read_text(encoding="utf-8"))["hram_ff80_ff90"], result.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()
    cmake_args = [f"--cmake-arg={item}" for item in args.cmake_arg]

    scratch = tempfile.TemporaryDirectory(prefix="gblift-sign-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        what_is_read_after()
        rom, labels = make_rom()
        rom_path = root / "lift_signatures.gb"
        rom_path.write_bytes(rom)
        at = {name: f"00:{address:04X}" for name, address in labels.items()}

        def build(*more: str, expect: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
            return tool("build", folder, "--gbrecomp", args.gbrecomp, "--trials", "200", *cmake_args, *more,
                        expect=expect)

        def lines() -> dict[str, str]:
            """signatures.txt as {function: its line without the function and the name}."""
            found = {}
            for line in (folder / "signatures.txt").read_text(encoding="utf-8").splitlines():
                if line and not line.startswith("#"):
                    found[line.split()[0]] = " ".join(line.split(";")[0].split()[1:])
            return found

        def as_written(text: str) -> str:
            return " ".join(text.split())

        def times() -> dict[str, dict]:
            """Every function checked again as it stands, held to its line where
            it has one: {function: in how many states it took the original's
            time, another, or none that was counted}."""
            build("--check-only", "--recheck")
            report = json.loads((folder / ".lift" / "check.json").read_text(encoding="utf-8"))
            return {f"00:{int(entry['function'][-4:], 16):04X}": entry["cycles"] for entry in report["results"]}

        def set_line(name: str, text: str) -> None:
            path = folder / "signatures.txt"
            was = path.read_text(encoding="utf-8").splitlines()
            now = [f"{at[name]}  {text}  ; {name}" if line.startswith(at[name]) else line for line in was]
            assert now != was, f"no line for {name}"
            path.write_text("\n".join(now) + "\n", encoding="utf-8", newline="\n")

        plain = ["--no-scan", "--pointer-scan", "off"]
        export = root / "export.json"
        run([args.gbrecomp, rom_path, *plain, "--lift-export", export])
        symbols = root / "names.sym"
        symbols.write_text("".join(f"{at[name].lower()} {name}\n" for name in EXPECTED), encoding="utf-8")
        folder = root / "source"
        tool("source", export, "-o", folder, "--symbols", symbols, "--rom", rom_path, "--", *plain)
        executable = folder / "build" / PRESET / "game" / ("game.exe" if os.name == "nt" else "game")

        # ---- The game as it is: what the rest is held to.
        build()
        original, _ = play(executable, root, "original")
        loops = original[LOOPS - 0x80]
        assert original[WAITS - 0x80] == loops and original[LOOSE - 0x80] == 9 and \
            original[SUM - 0x80] == (loops + 3) % 256 and original[TWICE - 0x80] == loops * 2 % 256, original
        # The carry is chained: with none ever coming in, ACC would be this.
        assert original[ACC - 0x80] != (loops + 1) * 2 * 0x90 % 256 or loops == 0, original

        # ---- The signatures, worked out.
        said = tool("signatures", folder, "--trials", "200").stdout
        found = lines()
        for name, wanted in EXPECTED.items():
            assert found.get(at[name]) == as_written(wanted), (name, found.get(at[name]), said)
        assert at["Start"] not in found and at["MainLoop"] not in found, "a function that never returns has a line"
        masks = folder / ".lift" / "signatures.masks"
        assert masks.is_file() and "--lift-signatures" in said, said
        held = len(found)
        assert f"{held} of " in said and f"{held} of them match the ROM's code held to it" in said, said

        # Every function is held to its line by `build`, which has nothing
        # left to check: `signatures` did.
        built = build().stdout
        assert "checking" not in built and f"{held} of them are held to a signature" in built, built

        # ---- What decides only how long a function takes is taken (the
        # lines above say so), and held to their lines all of them take the
        # original's time in every state.
        took = times()
        assert all(took[at[name]]["equal"] and not took[at[name]]["different"] for name in EXPECTED), took
        assert not any(entry["different"] for entry in took.values()), took

        # A line that leaves the flag out is a match still, and `build` takes
        # it: the time is something the checker reports, and the record keeps.
        import gb_lift_source  # tools/ is on the path since what_is_read_after()
        signatures_path, record_path = folder / "signatures.txt", folder / "lift-source.json"
        pace = f"gbfn:v1:0000:{labels['Pace']:04x}"
        full = next(line for line in signatures_path.read_text(encoding="utf-8").splitlines()
                    if line.startswith(at["Pace"]))
        short = full.replace("takes fc", "takes -")
        assert short != full, full
        signatures_path.write_text(signatures_path.read_text(encoding="utf-8").replace(full, short),
                                   encoding="utf-8", newline="\n")
        assert ", 0 differ" in build().stdout
        assert times()[at["Pace"]]["different"] > 0
        record = json.loads(record_path.read_text(encoding="utf-8"))
        assert record["functions"][pace]["clock"] == "differs", record["functions"][pace]

        # A first watch that did not see it, as one from 160 states does not
        # see a branch one state in a thousand takes: the report says the two
        # went through and were not read. `Pause` gets the line without B,
        # matches held to it in another time than the original, and is
        # watched again. `Pace` has that line already, as the tool's own and
        # from before the record kept the time: nothing about it has changed,
        # and it is held again all the same.
        record["signatures"][pace] = gb_lift_source.digest(short)
        for entry in record["functions"].values():
            entry.pop("clock", None)
        record_path.write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8", newline="\n")
        watched = json.loads((folder / ".lift" / "observed.json").read_text(encoding="utf-8"))
        for entry in watched["results"]:
            for name, item in TIME_ONLY.items():
                if entry["function"] == f"gbfn:v1:0000:{labels[name]:04x}":
                    assert entry["observed"]["reads"] & item, (name, entry["observed"])
                    entry["observed"]["reads"] &= ~item
                    entry["observed"]["through"] |= item
        unseen = root / "unseen.json"
        unseen.write_text(json.dumps(watched), encoding="utf-8")
        said = tool("signatures", folder, "--trials", "200", "--observed", unseen).stdout
        assert "2 did not hold as first watched, and were watched again from every state" in said, said
        assert "somebody changed" not in said, said
        for name, wanted in EXPECTED.items():
            assert lines()[at[name]] == as_written(wanted), (name, lines()[at[name]], said)
        record = json.loads(record_path.read_text(encoding="utf-8"))
        assert all(record["functions"][f"gbfn:v1:0000:{labels[name]:04x}"].get("clock") == "same"
                   for name in EXPECTED), record["functions"]
        assert not any(entry["different"] for entry in times().values())

        # ---- The game held to them ends as the original does.
        signed, log = play(executable, root, "signed", "--lift-signatures", masks)
        assert f"[LIFT] signatures: {held} of " in log, log
        assert signed == original, (original, signed)

        # ---- Who calls what, from the game played. Noting it changes
        # nothing. Loose was seen to return to one place, where A is read and
        # B is not: it gives A now, and its line says where that comes from.
        # Rare was never reached, so nothing is known of who else calls it.
        noted = folder / "callers" / "played.callers"
        noted.parent.mkdir()
        watched, log = play(executable, root, "noted", "--lift-callers", noted)
        assert watched == original and "[LIFT] callers:" in log, log
        assert any(line.startswith(f"0000:{labels['Loose']:04X} ") for line in
                   noted.read_text(encoding="utf-8").splitlines()), noted.read_text(encoding="utf-8")
        said = tool("signatures", folder, "--trials", "200").stdout
        assert re.search(r"\d+ more have a way in nobody can list", said), said
        text = (folder / "signatures.txt").read_text(encoding="utf-8")
        loose = next(line for line in text.splitlines() if line.startswith(at["Loose"]))
        rare = next(line for line in text.splitlines() if line.startswith(at["Rare"]))
        assert lines()[at["Loose"]] == "takes - gives a keeps c de hl flags" and loose.endswith("; Loose [as played]"), loose
        assert lines()[at["Rare"]] == as_written(EXPECTED["Rare"]) and rare.endswith("; Rare"), rare
        for name in ("Fill", "Add", "Twice", "Step", "WaitFrame", "Pace", "Pause"):
            assert lines()[at[name]] == as_written(EXPECTED[name]), (name, lines()[at[name]])
        build()
        assert play(executable, root, "narrowed", "--lift-signatures", masks)[0] == original

        # ---- A line that calls the chained carry left over. The checker has
        # no way to know better: it compares what a function leaves, not who
        # reads it. The game does: the second Step now adds in a carry that
        # is nobody's.
        set_line("Step", "takes fc  gives -  keeps bc de hl")
        built = build().stdout
        assert "checking 1 of" in built and ", 0 differ" in built, built
        wrong, _ = play(executable, root, "wrong", "--lift-signatures", masks)
        assert wrong[ACC - 0x80] != original[ACC - 0x80], "the game did not show the carry was a result"
        assert [x for i, x in enumerate(wrong) if i != ACC - 0x80] == \
            [x for i, x in enumerate(original) if i != ACC - 0x80], (original, wrong)
        # Not held to anything, the same game is the original still.
        assert play(executable, root, "unheld")[0] == original

        # A line somebody changed is theirs: the tool run again keeps it.
        said = tool("signatures", folder, "--trials", "200").stdout
        assert "1 lines somebody changed were kept" in said, said
        assert lines()[at["Step"]] == "takes fc gives - keeps bc de hl"
        said = tool("signatures", folder, "--trials", "200", "--restore", at["Step"]).stdout
        assert lines()[at["Step"]] == as_written(EXPECTED["Step"]) and "somebody changed" not in said, said
        build()
        assert play(executable, root, "restored", "--lift-signatures", masks)[0] == original

        # ---- A line that leaves out a register the function reads: the
        # checker gives it another value on the way in, and the build stops.
        set_line("Add", "takes a  gives a fc  keeps bc de hl")
        refused = build(expect=(1,))
        assert re.search(r"mismatch +gbfn:v1:0000:%04x +Add" % labels["Add"], refused.stdout), refused.stdout
        assert at["Add"] in refused.stderr and "Nothing was built" in refused.stderr, refused.stderr
        # And one that says it keeps what it changes.
        set_line("Add", "takes a b  gives fc  keeps a bc de hl")
        refused = build(expect=(1,))
        assert re.search(r"mismatch +gbfn:v1:0000:%04x +Add" % labels["Add"], refused.stdout), refused.stdout
        set_line("Add", "takes a b  gives a  keeps a")
        assert "both given and kept" in build(expect=(2,)).stderr
        # A line that can be read, however wrong, is one the tool can replace.
        set_line("Add", "takes a b  gives fc  keeps a bc de hl")
        tool("signatures", folder, "--trials", "200", "--restore", at["Add"])
        assert lines()[at["Add"]] == as_written(EXPECTED["Add"])
        assert ", 0 differ" in build().stdout

        # ---- A run recorded is of the registers too.
        refused = run([executable, "--headless", "--limit-frames", "5", "--record-run", root / "run",
                       "--lift-signatures", masks], expect=(1,)).stderr
        assert "--lift-signatures" in refused, refused
    finally:
        if scratch:
            scratch.cleanup()

    print("signatures were worked out, held against every function, and shown by the game where the checker cannot see")
    return 0


if __name__ == "__main__":
    sys.exit(main())
