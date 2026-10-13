#!/usr/bin/env python3
"""Reference runs of polyrastertest (Jaklyy, v1.0.2-b) on DeSmuME, and what a pass looks like.

    ..\\tools\\nds-venv\\Scripts\\python.exe tools/nds/oracle_oracle_make_polyrastertest.py

Writes roms/nds/oracle/polyrastertest/tests.json and pictures in frames/
(256x384, the top screen above the bottom one):

    first-failure   600 frames with nothing pressed: where DeSmuME stops
    complete        1,500 frames with X held from frame 100 to 1199: the end screen

The ROM draws each of its 77 polygons, captures the 3D picture and compares it
with data recorded on hardware. A test that matches is passed over; one that
does not stops the run on a picture of the differences until A or X is
pressed. X skims on while held, so one script serves any runner: X held to
frame 1199 carries it past every failure, and after that the end screen is
still. It says "Testing complete!" on the first text row of the bottom screen
and "Tests Passed: N/77" on the third.

DeSmuME's own rasteriser passes 1 of the 77, so the picture of a pass cannot
be taken from it whole. The ROM prints with one 8x8 font, so the pass row,
"Tests Passed: 77/77", is put together here from the characters of DeSmuME's
"Tests Passed: 1/77", and the script checks that the same way of putting a row
together reproduces the row DeSmuME did show.
"""

from __future__ import annotations

import hashlib
import sys

from oracle_make_wrestlers import CONVENTIONS, HERE, SUITES, Picture, oracle, sha256_file, write_json

ROM = SUITES / "polyrastertest-rom" / "polyrastertestv1.0.2-b" / "polyrastertest.nds"
OUT = HERE / "polyrastertest"
KEYS, FRAMES = ["100:X:1100"], 1500
BLACK, WHITE = bytes((0, 0, 0)), bytes((31, 31, 31))


def cells(picture: Picture, row: int, count: int) -> list[bytes]:
    return [picture.rect("bottom", 8 * column, 8 * row, 8, 8) for column in range(count)]


def row_pixels(glyphs: list[bytes]) -> bytes:
    """A text row's pixels, in row order, from its 8x8 characters."""
    return b"".join(glyph[line * 24:(line + 1) * 24] for line in range(8) for glyph in glyphs)


def as_reference(what: str, glyphs: list[bytes]) -> dict:
    data = row_pixels(glyphs)
    width = 8 * len(glyphs)
    letters = {BLACK: ".", WHITE: "#"}
    rows = ["".join(letters[data[(line * width + x) * 3:(line * width + x) * 3 + 3]] for x in range(width))
            for line in range(8)]
    return {"what": what, "width": width, "height": 8, "palette": {".": [0, 0, 0], "#": [31, 31, 31]},
            "pixels": rows, "sha256": hashlib.sha256(data).hexdigest()}


def main() -> int:
    stopped, error = oracle(ROM, [], 600, OUT / "frames" / "first-failure")
    if stopped is None:
        raise SystemExit(f"polyrastertest without input: {error}")
    end, error = oracle(ROM, KEYS, FRAMES, OUT / "frames" / "complete")
    if end is None:
        raise SystemExit(f"polyrastertest: {error}")

    complete = cells(end, 0, 17)      # "Testing complete!"
    score = cells(end, 2, 20)         # "Tests Passed: 1/77" and two blank cells
    one, slash, seven, blank = score[14], score[15], score[16], score[19]
    if score[17] != seven or score[18] != blank or blank != BLACK * 64:
        raise SystemExit("polyrastertest: the end screen is not the 'Tests Passed: 1/77' this script expects")
    if row_pixels(score[:14] + [one, slash, seven, seven, blank, blank]) != end.rect("bottom", 0, 16, 160, 8):
        raise SystemExit("polyrastertest: a row put together from characters differs from the one shown")
    passed = score[:14] + [seven, seven, slash, seven, seven, blank]

    references = {
        "complete": as_reference('"Testing complete!", the first text row of the bottom screen', complete),
        "all_passed": as_reference('"Tests Passed: 77/77" and a blank cell, the third text row, put together '
                                   "from the characters DeSmuME drew", passed),
        "desmume_score": as_reference('what DeSmuME shows there: "Tests Passed: 1/77"', score),
    }
    top_colours = sorted(tuple(colour) for colour in stopped.colours("top", 0, 0, 256, 192))
    document = {
        "rom": "roms/nds/polyrastertest-rom/polyrastertestv1.0.2-b/polyrastertest.nds",
        "rom_sha256": sha256_file(ROM),
        "oracle": "DeSmuME 0.9.12 through py-desmume 0.0.9, tools/nds/oracle_desmume.py",
        "conventions": CONVENTIONS,
        "screen": "Text is on the bottom screen, 8x8 characters, white (5-bit 31,31,31) on black. The top screen "
                  "shows the polygon being tested, or after a failure and at the end the comparison picture: "
                  "green a matching pixel, red a missing one, pink an overdrawn one, blue a wrong colour.",
        "pass": "The run has finished when the first text row is references.complete, and every test passed "
                "when the third is references.all_passed. Anything else on the third row is the count of "
                "tests that passed, out of the number run so far.",
        "fail": "Without X a run stops at its first failing test: the first text row is then \"Tests Passed: "
                "K/M\", M the number run so far, and there is no \"Testing complete!\".",
        "references": references,
        "runs": [{
            "id": "all-77", "title": "all 77 polygons, skimming past failures", "keys": KEYS, "frames": FRAMES,
            "checks": [
                {"name": "ran to the end", "screen": "bottom", "rect": [0, 0, 136, 8], "reference": "complete",
                 "sha256": references["complete"]["sha256"]},
                {"name": "77 of 77 passed", "screen": "bottom", "rect": [0, 16, 160, 8], "reference": "all_passed",
                 "sha256": references["all_passed"]["sha256"]},
            ],
            "oracle": {"passes": False, "shows": "Testing complete! Tests Passed: 1/77",
                       "note": "DeSmuME is not the reference for the 3D rasteriser; the hardware data in the "
                               "ROM is. Its first failure is test 1 (frames/first-failure.png), where the top "
                               "screen's colours are " + str(top_colours),
                       "top_sha256": end.sha256("top"), "bottom_sha256": end.sha256("bottom"),
                       "picture": "frames/complete.png", "picture_without_input": "frames/first-failure.png"},
        }],
    }
    write_json(OUT / "tests.json", document)
    print("polyrastertest: DeSmuME shows", document["runs"][0]["oracle"]["shows"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
