#!/usr/bin/env python3
"""Reference runs of armwrestler and rockwrestler on DeSmuME, as data a test runner can use.

    ..\\tools\\nds-venv\\Scripts\\python.exe tools/nds/oracle_oracle_make_wrestlers.py [armwrestler] [rockwrestler]

For each ROM this writes roms/nds/oracle/<rom>/tests.json and one picture per
run in <rom>/frames/ (ID.png and ID.ppm, 256x384: the top screen above the
bottom one, as nds_run --dump-frame writes it), and tour.args, one input script
that visits every run in a single session (an @FILE for oracle_desmume.py; the
same --keys work with nds_run).

tests.json holds a list of runs. A run starts from boot:

    nds_run ROM --keys K1 --keys K2 ... --frames FRAMES --dump-frame out.ppm

and has checks, each a rectangle of one screen and the SHA-256 that rectangle
has on a pass. The file's "conventions" says how the hash is made (three bytes
a pixel, the 5-bit red, green and blue values, so it does not depend on how a
runner widens 5 bits to 8). "references" holds the pixels behind each hash,
and each run's "oracle" says what DeSmuME itself showed.

Every key is held 4 frames with at least 8 released before the next, the first
at frame 30, and a result is captured 60 frames after the key that starts it:
all of it far more than either ROM needs, so that a runner whose boot takes a
few frames more or fewer than DeSmuME's reads the same screens.

The fonts come from the fetched suites (tools/fetch_nds_tests.py) and are only
used to say in words what the oracle showed.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# The oracle's data: gitignored, written by the oracle_make_*.py scripts.
HERE = ROOT / "roms" / "nds" / "oracle"
ORACLE = ROOT / "tools" / "nds" / "oracle_desmume.py"
SUITES = ROOT / "roms" / "nds"

FIRST, HOLD, STEP, RESULT = 30, 4, 12, 60
TO_5_BITS = bytes(value >> 3 for value in range(256))
W, H = 256, 192

CONVENTIONS = {
    "command": "nds_run ROM --keys K... --frames FRAMES --dump-frame out.ppm",
    "keys": "FRAME:BUTTONS:N holds BUTTONS while frames FRAME to FRAME+N-1, counted from 0, are emulated",
    "frames": "the number of frames to emulate; the picture after them is the one checked",
    "picture": "256x384: the top screen is rows 0-191, the bottom screen rows 192-383",
    "rect": "[x, y, width, height] in pixels on the screen named",
    "sha256": "SHA-256 of the rectangle's pixels in row order, three bytes a pixel: the red, green and blue "
              "5-bit values (each 8-bit channel shifted right by 3)",
    "pixels": "one character per pixel; the palette gives a character's [red, green, blue] 5-bit values",
}


# ---------------------------------------------------------------- pictures

def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P6" or fields[3] != b"255":
        raise SystemExit(f"{path}: not an 8-bit P6 PPM")
    return int(fields[1]), int(fields[2]), data[pos + 1:]


class Picture:
    """Both screens in 5-bit channels."""

    def __init__(self, path: Path):
        width, height, rgb = read_ppm(path)
        if (width, height) != (W, 2 * H):
            raise SystemExit(f"{path}: {width}x{height}, expected {W}x{2 * H}")
        self.rgb = rgb.translate(TO_5_BITS)

    def rect(self, screen: str, x: int, y: int, w: int, h: int) -> bytes:
        y += H if screen == "bottom" else 0
        return b"".join(self.rgb[((y + row) * W + x) * 3:((y + row) * W + x + w) * 3] for row in range(h))

    def sha256(self, screen: str, x: int = 0, y: int = 0, w: int = W, h: int = H) -> str:
        return hashlib.sha256(self.rect(screen, x, y, w, h)).hexdigest()

    def colours(self, screen: str, x: int, y: int, w: int, h: int) -> set[bytes]:
        data = self.rect(screen, x, y, w, h)
        return {data[i:i + 3] for i in range(0, len(data), 3)}


def reference(picture: Picture, screen: str, rect: list[int], letters: dict[bytes, str], what: str) -> dict:
    x, y, w, h = rect
    data = picture.rect(screen, x, y, w, h)
    rows = ["".join(letters[data[(r * w + c) * 3:(r * w + c) * 3 + 3]] for c in range(w)) for r in range(h)]
    return {"what": what, "width": w, "height": h,
            "palette": {letter: list(colour) for colour, letter in letters.items()},
            "pixels": rows, "sha256": hashlib.sha256(data).hexdigest()}


# ---------------------------------------------------------------- running

def oracle(rom: Path, keys: list[str], frames: int, picture_stem: Path) -> tuple[Picture | None, str]:
    """Run DeSmuME; keep ID.png and ID.ppm. Returns the picture, or None and why not."""
    with tempfile.TemporaryDirectory(prefix="nds-wrestler-") as scratch:
        command = [sys.executable, str(ORACLE), str(rom), "--frames", str(frames), "--at", str(frames),
                   "--dump-frames", scratch, "--stacked", "--png", "--quiet"]
        for key in keys:
            command += ["--keys", key]
        done = subprocess.run(command, capture_output=True, text=True)
        if done.returncode:
            return None, done.stderr.strip().splitlines()[-1] if done.stderr.strip() else "failed"
        picture_stem.parent.mkdir(parents=True, exist_ok=True)
        for suffix in (".ppm", ".png"):
            shutil.move(str(Path(scratch) / f"{frames:06d}{suffix}"), str(picture_stem.with_suffix(suffix)))
    return Picture(picture_stem.with_suffix(".ppm")), ""


def presses(buttons: list[str]) -> tuple[list[str], int]:
    """--keys for a sequence of presses and the frame the last one starts on."""
    keys = [f"{FIRST + STEP * i}:{button}:{HOLD}" for i, button in enumerate(buttons)]
    return keys, FIRST + STEP * (len(buttons) - 1)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, document: dict) -> None:
    """Indented, with lists of numbers (rectangles, colours) kept on one line."""
    text = json.dumps(document, indent=1)
    text = re.sub(r"\[\s*(-?\d+(?:,\s*-?\d+)*)\s*\]",
                  lambda found: "[" + ", ".join(re.findall(r"-?\d+", found.group(1))) + "]", text)
    path.write_text(text + "\n", encoding="ascii", newline="\n")


def write_tour(path: Path, title: str, rom: str, visits: list[tuple[str, list[str]]], settle: int) -> None:
    """One session through every run: (label, buttons that lead there from the previous one)."""
    lines = [f"# {title}", f"# oracle_desmume.py {rom} @{path.name} --dump-frames DIR --stacked --png",
             "# The same --keys work with nds_run; each --at is the count for its --frames."]
    frame, captures = FIRST, []
    for label, buttons in visits:
        for button in buttons:
            lines.append(f"--keys {frame}:{button}:{HOLD}".ljust(24) + f"# towards {label}")
            last, frame = frame, frame + STEP
        frame = last + settle
        captures.append(frame)
        lines.append(f"--at {frame}".ljust(24) + f"# {label}")
        frame += STEP
    lines.insert(3, f"--frames {captures[-1]}")
    path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")


# ---------------------------------------------------------------- armwrestler

BLACK, WHITE, GREEN, RED = bytes((0, 0, 0)), bytes((31, 31, 31)), bytes((0, 24, 0)), bytes((24, 0, 0))

# (id, menu item, page title, buttons from boot)
ARM_PAGES = [
    ("arm-alu-1", "ARM ALU", "ALU TESTS PART 1", ["START"]),
    ("arm-alu-2", "ARM ALU, second page", "ALU PT 2 / MISC", ["START", "START"]),
    ("arm-load-1", "ARM LDR/STR", "LOAD TESTS PART 1", ["DOWN", "START"]),
    ("arm-load-2", "ARM LDR/STR, second page", "LOAD TESTS PART 2", ["DOWN", "START", "START"]),
    ("arm-ldm-stm", "ARM LDM/STM", "LDM/STM TESTS 1", ["DOWN", "DOWN", "START"]),
    ("thumb-alu", "THUMB ALU", "ALU TEST", ["DOWN"] * 3 + ["START"]),
    ("thumb-load", "THUMB LDR/STR", "LDR/STR TEST 1", ["DOWN"] * 4 + ["START"]),
    ("thumb-ldm-stm", "THUMB LDM/STM", "LDM/STM TEST", ["DOWN"] * 5 + ["START"]),
    ("arm-v5te", "ARM V5TE", "ARM V5TE TESTS", ["DOWN"] * 6 + ["START"]),
]
# What the ROM's strings look like in its font.
ARM_GLYPHS = {"&": "+", "'": "-", "\\": "#", ")": "!", "j": "*", "g": ",", "`": ">", "^": "d", "_": "n"}


def arm_text(font: bytes, picture: Picture, y: int) -> list[tuple[str, bytes | None]]:
    """The 32 character cells of the top screen's text row at y: (character, colour)."""
    glyphs = {}
    for index in range(len(font) // 64):
        glyphs.setdefault(bytes(1 if b else 0 for b in font[index * 64:(index + 1) * 64]), index)
    cells = []
    for x in range(0, W, 8):
        data = picture.rect("top", x, y, 8, 8)
        pixels = [data[i:i + 3] for i in range(0, len(data), 3)]
        lit = [p for p in pixels if p != BLACK]
        if not lit:
            cells.append((" ", None))
            continue
        index = glyphs.get(bytes(0 if p == BLACK else 1 for p in pixels))
        char = "?" if index is None else " " if index == 0 else chr(index + 37)
        cells.append((ARM_GLYPHS.get(char, char), lit[0]))
    return cells


def make_armwrestler() -> None:
    out = HERE / "armwrestler"
    rom = SUITES / "armwrestler" / "armwrestler.nds"
    font = (SUITES / "armwrestler" / "font8x8.pat").read_bytes()
    runs, ok = [], None
    for ident, item, title, buttons in ARM_PAGES:
        keys, last = presses(buttons)
        frames = last + RESULT
        picture, error = oracle(rom, keys, frames, out / "frames" / ident)
        if picture is None:
            raise SystemExit(f"armwrestler {ident}: {error}")
        checks, shows = [], []
        for y in range(16, 176, 8):
            cells = arm_text(font, picture, y)
            verdict = [i for i, (_, colour) in enumerate(cells) if colour in (GREEN, RED)]
            if not verdict:
                continue
            name = "".join(c for c, colour in cells[:verdict[0]] if colour in (WHITE, None)).strip()
            result = "".join(c for c, _ in cells[verdict[0]:]).strip()
            rect = [verdict[0] * 8, y, 16, 8]
            if result == "OK" and ok is None:
                ok = reference(picture, "top", rect, {BLACK: ".", GREEN: "#"},
                               'armwrestler\'s green "OK" on black')
            checks.append({"name": " ".join(name.split()), "screen": "top", "rect": rect,
                           "reference": "ok", "oracle": result})
            shows.append(f"{' '.join(name.split())} {result}")
        if not checks:
            # A page without rows: assert that, so a runner does not pass it by drawing nothing at all.
            rows = [0, 16, W, 128]
            if picture.colours("top", *rows) != {BLACK}:
                raise SystemExit(f"armwrestler {ident}: no verdict rows but the rows area is not black")
            checks.append({"name": "no result rows", "screen": "top", "rect": rows, "reference": None,
                           "sha256": picture.sha256("top", *rows),
                           "oracle": "black: this page draws only its title and the footer"})
        runs.append({"id": ident, "menu_item": item, "title": title, "keys": keys, "frames": frames,
                     "checks": checks,
                     "oracle": {"passes": all(c["oracle"] == "OK" or c["reference"] is None for c in checks),
                                "shows": shows, "top_sha256": picture.sha256("top"),
                                "bottom_sha256": picture.sha256("bottom"), "picture": f"frames/{ident}.png"}})
        print(f"armwrestler {ident}: {len(shows)} rows, " + ("all OK" if runs[-1]["oracle"]["passes"] else "NOT all OK"))
    for run in runs:
        for check in run["checks"]:
            if check["reference"] == "ok":
                check["sha256"] = ok["sha256"]
    document = {
        "rom": "roms/nds/armwrestler/armwrestler.nds", "rom_sha256": sha256_file(rom),
        "oracle": "DeSmuME 0.9.12 through py-desmume 0.0.9, tools/nds/oracle_desmume.py",
        "conventions": CONVENTIONS,
        "menu": "UP and DOWN move the cursor (7 items, it stops at the ends), START runs the item. On a page "
                "START goes to the next page (ARM pages 1-5 follow each other, then the menu; the three "
                "THUMB pages follow each other, then the menu) and SELECT returns to the menu.",
        "screen": "Everything is on the top screen, 8x8 characters. A page has its title at y=1, one row "
                  "per test from y=16 down in steps of 8, the test's name in white from x=16 and its verdict "
                  "at the rectangle given (x=72, 80 or 104 by page).",
        "pass": 'A row passes when its rectangle is the green "OK" (references.ok).',
        "fail": 'A failed row has "BAD" in red (5-bit 24,0,0) starting at the same x, followed by what was '
                "wrong (C N V Z Q for flags, Rd, Rn and a value, MEM and a value), so the rectangle differs.",
        "references": {"ok": ok},
        "runs": runs,
    }
    write_json(out / "tests.json", document)
    write_tour(out / "tour.args", "armwrestler: all nine pages in one session.", document["rom"], [
        ("ALU TESTS PART 1", ["START"]), ("ALU PT 2 / MISC", ["START"]), ("LOAD TESTS PART 1", ["START"]),
        ("LOAD TESTS PART 2", ["START"]), ("LDM/STM TESTS 1", ["START"]),
        ("THUMB ALU TEST", ["START", "DOWN", "DOWN", "DOWN", "START"]), ("THUMB LDR/STR TEST 1", ["START"]),
        ("THUMB LDM/STM TEST", ["START"]), ("ARM V5TE TESTS", ["START", "DOWN", "DOWN", "DOWN", "START"]),
    ], RESULT)


# ---------------------------------------------------------------- rockwrestler

GREY, DARK = bytes((21, 21, 21)), bytes((10, 10, 10))
ROCK_GROUPS = [
    ("ARMv4", ["CONDITION CODES"]),
    ("ARMv5", ["CLZ", "QADD, QSUB", "QDADD, QDSUB", "SMULxy", "SMLAxy", "SMULWy", "SMLAWy", "SMLALxy", "BLX",
               "LDR r15, POP {r15}, LDM {r15}", "LDM / STM"]),
    ("IPC", ["IPCSYNC", "IPCFIFO", "IPCFIFO IRQ"]),
    ("DS MATH", ["SQRT 32", "SQRT 64", "DIV 32/32", "DIV 64/32", "DIV 64/64"]),
    ("MEMORY", ["WRAM CNT", "VRAM CNT", "TCM"]),
    ("INITIAL STATE", ["IPC/IRQ/CPSR", "CP15"]),
]


def rock_text(font: bytes, picture: Picture) -> list[str]:
    """The top screen as 24 lines of 32 characters; a cell never drawn is a space."""
    tiles = {}
    for index in range(len(font) // 64):
        tiles.setdefault(bytes(font[index * 64:(index + 1) * 64]), index)
    index_of = {bytes((0, 0, 0)): 0, DARK: 1, GREY: 2, WHITE: 3}
    lines = []
    for ty in range(24):
        line = ""
        for tx in range(32):
            data = picture.rect("top", tx * 8, ty * 8, 8, 8)
            pixels = [data[i:i + 3] for i in range(0, len(data), 3)]
            if len(set(pixels)) == 1:
                line += " "  # the clear colour, a blank tile, or the ARM7 light
                continue
            tile = tiles.get(bytes(index_of.get(p, 255) for p in pixels))
            line += "?" if tile is None else "0123456789ABCDEF"[tile] if tile < 16 else ">" if tile == 0x16 \
                else " " if tile < 32 else chr(tile)
        lines.append(line.rstrip())
    return lines


def slug(text: str) -> str:
    return "-".join("".join(c.lower() if c.isalnum() else " " for c in text).split())


def make_rockwrestler() -> None:
    out = HERE / "rockwrestler"
    rom = SUITES / "rockwrestler" / "rockwrestler.nds"
    font = (SUITES / "rockwrestler" / "src9" / "framework" / "font.bin").read_bytes()
    verdict = [0, 0, 16, 8]
    runs, ok, ok_screen, visits = [], None, None, []
    for g, (group, tests) in enumerate(ROCK_GROUPS):
        for t, test in enumerate(tests):
            ident = f"{slug(group)}-{slug(test)}"
            keys, last = presses(["DOWN"] * g + ["A"] + ["DOWN"] * t + ["A"])
            frames = last + RESULT
            info = group == "INITIAL STATE"
            picture, error = oracle(rom, keys, frames, out / "frames" / ident)
            run = {"id": ident, "group": group, "name": test, "kind": "info" if info else "test",
                   "keys": keys, "frames": frames, "checks": []}
            if picture is None:
                run["oracle"] = {"passes": False, "shows": None, "error": error}
            else:
                lines = rock_text(font, picture)
                shown = [line for line in lines if line.strip()] if info else lines[0].strip()
                run["oracle"] = {"passes": None if info else shown == "OK", "shows": shown,
                                 "top_sha256": picture.sha256("top"), "bottom_sha256": picture.sha256("bottom"),
                                 "picture": f"frames/{ident}.png"}
                if shown == "OK" and ok is None:
                    ok = reference(picture, "top", verdict, {bytes((0, 0, 0)): "#", DARK: "+", GREY: "-", WHITE: "."},
                                   'rockwrestler\'s "OK" at the top left of a screen cleared to grey')
                    ok_screen = picture.sha256("top")
                if shown == "OK" and picture.sha256("top") != ok_screen:
                    raise SystemExit(f"rockwrestler {ident}: OK, but the screen differs from the first OK screen")
            if not info:
                run["checks"].append({"name": test, "screen": "top", "rect": verdict, "reference": "ok"})
            runs.append(run)
            print(f"rockwrestler {ident}: {run['oracle'].get('error') or run['oracle']['shows']}")
            # The tour: B leaves a result (and then the group), DOWN moves on, A opens or runs.
            if t:
                route = ["B", "DOWN", "A"]
            elif g:
                route = ["B", "B", "DOWN", "A", "A"]
            else:
                route = ["A", "A"]
            visits.append((f"{group} / {test}", route))
    for run in runs:
        for check in run["checks"]:
            check["sha256"] = ok["sha256"]
    document = {
        "rom": "roms/nds/rockwrestler/rockwrestler.nds", "rom_sha256": sha256_file(rom),
        "oracle": "DeSmuME 0.9.12 through py-desmume 0.0.9, tools/nds/oracle_desmume.py",
        "conventions": CONVENTIONS,
        "menu": "UP and DOWN move the cursor (it wraps), A opens a group or runs a test, B goes back. Each run "
                "here goes from boot: DOWN to the group, A, DOWN to the test, A.",
        "screen": "Everything is on the top screen, 32x24 characters of 8x8 pixels. A test clears the screen "
                  "to grey (5-bit 21,21,21) and, when it returns, draws OK at the top left and waits for B.",
        "pass": 'A test passes when the rectangle is "OK" (references.ok). On a pass the whole top screen is '
                "that and grey: references.ok_screen_sha256.",
        "fail": '"FAIL xxx", "TIMEOUT xxx" or "TIMEOUT R/W" in the same place, xxx the hexadecimal number of '
                "the check that failed (see the test's source in roms/nds/rockwrestler/src9/tests); a test "
                "that never returns leaves the screen grey. The INITIAL STATE entries are not tests: they "
                "print registers and have no checks.",
        "references": {"ok": ok, "ok_screen_sha256": ok_screen},
        "runs": runs,
    }
    write_json(out / "tests.json", document)
    write_tour(out / "tour.args", "rockwrestler: every entry of every group in one session "
               "(DeSmuME itself dies in DS MATH / DIV 64/32, so this one is for nds_run).",
               document["rom"], visits, RESULT)


def main() -> int:
    wanted = sys.argv[1:] or ["armwrestler", "rockwrestler"]
    makers = {"armwrestler": make_armwrestler, "rockwrestler": make_rockwrestler}
    for name in wanted:
        if name not in makers:
            print(__doc__)
            return 2
    for name in wanted:
        makers[name]()
    return 0


if __name__ == "__main__":
    sys.exit(main())
