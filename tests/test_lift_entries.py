#!/usr/bin/env python3
"""The states a game entered its functions in, end to end.

Makes a small ROM with a function that returns only when a register points
at a record its caller made: no state made up from nothing gets through it,
and neither does a moment of the game's memory with made-up registers. Then:

- the generated game, run with `--entry-states FILE --entry-states-of LIST`,
  has to end in the same state as without, and as the same game recorded
  with `--data-use`, and the file has to hold what the game was: the
  registers, the return address and the memory of each arrival kept, the
  arrivals the rule keeps (the first with each caller, then the second, the
  fourth, the eighth...), and nothing of a function never arrived at;
- with no list, the functions are where calls and interrupts arrived, and a
  function only jumped to is not among them;
- `gb_lift.py check --entries` has to get the function through, exactly as
  the game ran it, where made-up states and an image do not, and say so; a
  function entered from a helper that it calls itself has to be run to its
  own return and not thrown away because that address runs deeper down; an
  edit to the lifted C has to be caught from a state as the game had it; and
  a function no state was kept of is not tried;
- `gb_lift.py entries` has to write the list from the export and count what
  a file covers;
- `gb_lift.py build` has to take a file in a source folder's `entries/` and
  give the function a verdict it could not get before;
- a lifted game asked for the states runs its recompiled code.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from test_lift_runtime_e2e import GB_LIFT, NINTENDO_LOGO, assemble, bump, ldh_a, ldh_to, play, run

FRAMES = 180
VBLANK_FLAG, LOOPS, RESULT, OUTER, INNER, THROUGH, SUM, NEVER, LATER = range(0x80, 0x89)
RECORD = 0xC140

PROGRAM = [
    ("label", "Start"),
    [0xF3],                                  # di
    [0x31, 0xFF, 0xDF],                      # ld sp, $DFFF
    [0xAF], ldh_to(0x0F),                    # IF = 0
    [0x21, 0x80, 0xFF, 0x06, 0x20],          # ld hl, $FF80 / ld b, 32
    ("label", "Clear"),
    [0x22, 0x05, 0x20], ("rel", "Clear"),    # ld [hli], a / dec b / jr nz
    [0x21, RECORD & 0xFF, RECORD >> 8],      # the record: A5 5A 3C
    [0x3E, 0xA5, 0x22, 0x3E, 0x5A, 0x22, 0x3E, 0x3C, 0x77],
    [0x3E, 0x01], ldh_to(0xFF),              # IE = VBlank
    [0xFB],                                  # ei
    ("label", "MainLoop"),
    [0xCD], ("abs", "WaitFrame"),
    [0x21, RECORD & 0xFF, RECORD >> 8],      # ld hl, RECORD
    [0xCD], ("abs", "NeedsRecord"),
    ("label", "AfterRecord"),
    [0xCD], ("abs", "Outer"),                # called by name,
    [0x21], ("abs", "Outer"),                # and through the helper Outer calls itself
    [0xCD], ("abs", "Through"),
    [0x3E, 0x01, 0xCD], ("abs", "Twice"),
    ("label", "AfterFirst"),
    [0x3E, 0x02, 0xCD], ("abs", "Twice"),
    ("label", "AfterSecond"),
    [0xCD], ("abs", "Later"),
    ldh_a(NEVER), [0xB7],                    # or a
    [0xC4], ("abs", "NeverCalled"),          # call nz: never
    bump(LOOPS),
    [0x18], ("rel", "MainLoop"),

    ("label", "VBlank"),
    [0xF5],                                  # push af
    [0x3E, 0x01], ldh_to(VBLANK_FLAG),
    [0xF1, 0xD9],                            # pop af / reti

    ("label", "WaitFrame"),
    [0xAF], ldh_to(VBLANK_FLAG),
    ("label", "WaitFrameLoop"),
    [0x76],                                  # halt
    ldh_a(VBLANK_FLAG), [0xB7, 0x28], ("rel", "WaitFrameLoop"),
    [0xC9],

    # Returns only when HL points at A5 5A, the record its caller made, and
    # waits for ever otherwise.
    ("label", "NeedsRecord"),
    [0x7E, 0xFE, 0xA5, 0x20], ("rel", "NeedsRecord"),   # ld a, [hl] / cp $A5 / jr nz
    [0x23, 0x7E, 0xFE, 0x5A],                # inc hl / ld a, [hl] / cp $5A
    ("label", "NeedsRecordSpin"),
    [0x20], ("rel", "NeedsRecordSpin"),      # jr nz
    [0x23, 0x7E], ldh_to(RESULT),            # inc hl / ld a, [hl]
    [0xC9],

    ("label", "Outer"),
    bump(OUTER),
    [0x21], ("abs", "Inner"),
    [0xCD], ("abs", "Through"),
    [0xC9],
    ("label", "Inner"),
    bump(INNER),
    [0xC9],
    ("label", "Through"),
    [0xCD], ("abs", "JumpHL"),
    ("label", "ThroughBack"),
    bump(THROUGH),
    [0xC9],
    ("label", "JumpHL"),
    [0xE9],                                  # jp hl

    ("label", "Twice"),
    [0x47], ldh_a(SUM), [0x80], ldh_to(SUM), # ld b, a / ldh a, [SUM] / add b
    [0xC9],

    ("label", "NeverCalled"),
    bump(NEVER),
    [0xC9],

    # Its second half runs only once the main loop has gone round eight times.
    ("label", "Later"),
    ldh_a(LOOPS), [0xFE, 0x08, 0xD8],        # cp 8 / ret c
    bump(LATER),
    [0xC9],
]

LIFTED = ("WaitFrame", "NeedsRecord", "Outer", "Twice", "NeverCalled", "Later")


def make_rom() -> tuple[bytearray, dict[str, int]]:
    code, labels = assemble(0x150, PROGRAM)
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x13F] = b"ENTRY GAME "
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    for vector in range(0x00, 0x40, 8):
        rom[vector] = 0xC9
    for vector in range(0x48, 0x68, 8):
        rom[vector] = 0xD9
    rom[0x40:0x43] = bytes([0xC3, labels["VBlank"] & 0xFF, labels["VBlank"] >> 8])
    rom[0x150:0x150 + len(code)] = code
    return rom, labels


def read_states(path: Path) -> dict:
    """A file of entry states, whole: the format is at the top of
    runtime/src/gbrt_entry_states.c."""
    data = path.read_bytes()
    assert data[:8] == b"GBLIFTEN", data[:8]
    (version, page_size, eram_size, pages_per_state, state_count, page_count, function_count, listed,
     per, zero) = struct.unpack_from("<10I", data, 8)
    assert (version, page_size, zero) == (1, 256, 0)
    at = 48
    functions = {}
    for _ in range(function_count):
        bank, address, arrivals, kept = struct.unpack_from("<HHII", data, at)
        assert (bank, address) not in functions and list(functions) == sorted(functions)
        functions[(bank, address)] = (arrivals, kept)
        at += 12
    states = []
    for _ in range(state_count):
        (bank, address, rom_bank, a, f, b, c, d, e, h, l, sp, returns_to, ime, ram_bank, ram_enabled,
         wram_bank, vram_bank, _zero, arrival, cycle, _end) = struct.unpack_from("<HHH8BHH6BIQI", data, at)
        states.append({"bank": bank, "address": address, "rom_bank": rom_bank, "a": a, "f": f, "b": b, "c": c,
                       "d": d, "e": e, "h": h, "l": l, "sp": sp, "returns_to": returns_to, "ime": ime,
                       "ram_enabled": ram_enabled, "arrival": arrival, "cycle": cycle})
        at += 40
    pages_at = at + state_count * pages_per_state * 4
    assert len(data) == pages_at + page_count * 256, "the file is not the size its head says"
    eram_pages = (eram_size + 255) // 256
    for state in states:
        ids = struct.unpack_from(f"<{pages_per_state}I", data, at)
        at += pages_per_state * 4
        body = b"".join(data[pages_at + page * 256:pages_at + page * 256 + 256] for page in ids)
        memory = body[eram_pages * 256:]
        state["wram"] = memory[:0x8000]
        state["vram"] = memory[0x8000:0xC000]
        tail = memory[0xC000:]
        state["hram"] = tail[0xA0:0xA0 + 0x7F]
        state["io"] = tail[0xA0 + 0x7F:0xA0 + 0x7F + 0x80]
        state["ie"] = tail[0xA0 + 0x7F + 0x80]
    order = [(state["bank"], state["address"], state["arrival"]) for state in states]
    assert order == sorted(order), "the states are not by function and arrival"
    return {"functions": functions, "states": states, "listed": listed, "per": per, "pages": page_count,
            "pages_per_state": pages_per_state}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--keep", type=Path, help="work here and keep the files")
    args = parser.parse_args()

    scratch = tempfile.TemporaryDirectory(prefix="gblift-entries-") if not args.keep else None
    root = Path(scratch.name) if scratch else args.keep
    root.mkdir(parents=True, exist_ok=True)
    try:
        rom, labels = make_rom()
        rom_path = root / "entry_game.gb"
        rom_path.write_bytes(rom)
        export_path = root / "export.json"
        analysis = ["--no-scan", "--pointer-scan", "off"]
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift-export", str(export_path)])
        export = json.loads(export_path.read_text(encoding="utf-8"))
        ids = {function["address"]: function["id"] for function in export["functions"]}
        for name in LIFTED + ("Through",):
            assert labels[name] in ids, f"the export has no function at {name}"
        assert labels["Inner"] not in ids, "Inner is reached by a jump through a register only"

        lifted = root / "lifted"
        arguments = []
        for name in LIFTED:
            arguments += ["--function", f"00:{labels[name]:04x}"]
        run([sys.executable, str(GB_LIFT), "baseline", str(export_path), *arguments, "-o", str(lifted)])
        package = root / "package"
        run([sys.executable, str(GB_LIFT), "package", str(export_path), "--source", str(lifted),
             "-o", str(package)])
        project = root / "game"
        run([str(args.gbrecomp), str(rom_path), *analysis, "--lift", str(package / "lift.json"),
             "-o", str(project), "--output-prefix", "entry_game"])
        build = project / "build"
        run(["cmake", "-G", "Ninja", "-S", str(project), "-B", str(build),
             "-DCMAKE_BUILD_TYPE=Release", *args.cmake_arg])
        run(["ninja", "-C", str(build)], timeout=900)
        executable = next(path for path in (build / "entry_game.exe", build / "entry_game") if path.exists())

        # ---- The list, from the export ------------------------------------
        listed = root / "functions.txt"
        printed = run([sys.executable, str(GB_LIFT), "entries", str(export_path), "-o", str(listed)]).stdout
        wanted = [line for line in listed.read_text(encoding="utf-8").splitlines() if not line.startswith("#")]
        assert len(wanted) == len(export["functions"]) and f"{len(wanted)} functions" in printed, printed
        assert f"00:{labels['NeedsRecord']:04X}" in wanted and wanted == sorted(wanted), wanted

        # ---- Recording changes nothing -------------------------------------
        def played(name: str, *flags: str) -> dict:
            return play(executable, root, name, "--no-lift", *flags, frames=FRAMES)[0]

        plain = played("plain")
        hram = plain["hram_ff80_ff90"]
        loops = hram[LOOPS - 0x80]
        assert loops > 100 and hram[RESULT - 0x80] == 0x3C and hram[NEVER - 0x80] == 0, hram
        assert hram[OUTER - 0x80] == hram[INNER - 0x80] == (2 * loops) % 256, hram
        states_path = root / "game.entries"
        recorded = played("recorded", "--entry-states", str(states_path), "--entry-states-of", str(listed))
        data_use = played("data-use", "--data-use", str(root / "use.json"))
        both = played("both", "--entry-states", str(root / "both.entries"), "--entry-states-of", str(listed),
                      "--data-use", str(root / "use-both.json"))
        for name, other in (("--entry-states", recorded), ("--data-use", data_use), ("both", both)):
            differ = [key for key in plain if key != "dispatch_fallbacks" and plain[key] != other.get(key)]
            assert not differ, f"the game ends differently with {name}: {differ}"
        assert not [key for key in data_use if data_use[key] != recorded[key]], \
            "two games run in the interpreter end differently"
        assert states_path.read_bytes() == (root / "both.entries").read_bytes(), \
            "the states differ with a second recorder beside"
        assert json.loads((root / "use.json").read_text(encoding="utf-8")) == \
            json.loads((root / "use-both.json").read_text(encoding="utf-8")), \
            "the record of what the data is used for differs with the states kept beside it"

        # A lifted game asked for the states runs its recompiled code.
        _, log = play(executable, root, "lifted-recorded", "--report-lift", "--entry-states",
                      str(root / "lifted.entries"), "--entry-states-of", str(listed), frames=FRAMES)
        counters = dict(re.findall(r"(\w+)=(\d+)", next(line for line in log.splitlines() if "[LIFT]" in line)))
        assert int(counters["calls"]) == 0, counters
        assert states_path.read_bytes() == (root / "lifted.entries").read_bytes()

        # ---- What the file holds ---------------------------------------------
        kept = read_states(states_path)
        assert kept["listed"] == 1 and kept["per"] == 8
        assert set(kept["functions"]) == {(0, int(line[3:], 16)) for line in wanted}

        def of(name: str) -> list[dict]:
            return [state for state in kept["states"] if state["address"] == labels[name]]

        assert kept["functions"][(0, labels["NeverCalled"])] == (0, 0) and not of("NeverCalled")
        # One caller: its first arrival, then the second, fourth, eighth and
        # sixteenth, which is half of the eight a function may have.
        needs = of("NeedsRecord")
        assert [state["arrival"] for state in needs] == [1, 2, 4, 8, 16], needs
        # (The run stops in the wait at the top of the loop after the last one counted.)
        assert kept["functions"][(0, labels["NeedsRecord"])] == (loops, 5), kept["functions"]
        for state in needs:
            assert (state["h"], state["l"]) == (RECORD >> 8, RECORD & 0xFF) and state["a"] == 1, state
            assert state["sp"] == 0xDFFD and state["returns_to"] == labels["AfterRecord"], state
            assert state["ime"] == 1 and state["rom_bank"] == 1, state
            # Its memory is that moment's: the record, and the loops so far.
            assert state["wram"][RECORD - 0xC000:RECORD - 0xC000 + 3] == bytes([0xA5, 0x5A, 0x3C])
            assert state["hram"][LOOPS - 0x80] == state["arrival"] - 1, (state["arrival"], state["hram"][:8])
            assert state["wram"][0x1FFD:0x1FFF] == struct.pack("<H", labels["AfterRecord"])
            assert state["ie"] == 1
        assert [state["cycle"] for state in needs] == sorted(state["cycle"] for state in needs)
        # Two callers: the first arrival with each, then by the count.
        twice = of("Twice")
        assert [state["arrival"] for state in twice] == [1, 2, 4, 8, 16, 32], twice
        assert [state["returns_to"] for state in twice[:2]] == [labels["AfterFirst"], labels["AfterSecond"]]
        assert [state["a"] for state in twice[:2]] == [1, 2]
        # Outer comes from the main loop and from the helper.
        assert {state["returns_to"] for state in of("Outer")} > {labels["ThroughBack"]}, of("Outer")
        # Memory is stored a page once however many states have it.
        assert kept["pages"] < len(kept["states"]) * 8 < len(kept["states"]) * kept["pages_per_state"], kept["pages"]

        # No more of a function than it is told.
        played("two", "--entry-states", str(root / "two.entries"), "--entry-states-of", str(listed),
               "--entry-states-per", "2")
        two = read_states(root / "two.entries")
        assert two["per"] == 2 and max(count for _, count in two["functions"].values()) == 2
        assert [state["arrival"] for state in two["states"] if state["address"] == labels["Twice"]] == [1, 2]

        # With no list: where calls and interrupts arrived. Inner and the
        # handler behind the vector's jump are only jumped to.
        played("calls", "--entry-states", str(root / "calls.entries"))
        calls = read_states(root / "calls.entries")
        assert calls["listed"] == 0
        assert set(calls["functions"]) == {(0, 0x40)} | {
            (0, labels[name]) for name in ("WaitFrame", "NeedsRecord", "Outer", "Through", "JumpHL", "Twice", "Later")}, \
            sorted(calls["functions"])
        assert [state["arrival"] for state in calls["states"] if state["address"] == labels["NeedsRecord"]] == \
            [1, 2, 4, 8, 16]

        # What cannot be done is said.
        for flags, message in ((["--entry-states-of", str(listed)], "--entry-states-of needs --entry-states"),
                               (["--entry-states", str(root / "x.entries"), "--entry-states-per", "0"],
                                "--entry-states-per takes")):
            refused = subprocess.run([str(executable), "--headless", "--no-audio", "--limit-frames", "5",
                                      "--save-dir", str(root / "saves-plain"), *flags],
                                     capture_output=True, text=True, timeout=120)
            assert refused.returncode != 0 and message in refused.stderr, (flags, refused.stderr[-400:])
        bad = root / "bad.txt"
        bad.write_text("00:0150\n7F:4000\n", encoding="utf-8")
        refused = subprocess.run([str(executable), "--headless", "--no-audio", "--limit-frames", "5",
                                  "--save-dir", str(root / "saves-plain"), "--entry-states",
                                  str(root / "x.entries"), "--entry-states-of", str(bad)],
                                 capture_output=True, text=True, timeout=120)
        assert refused.returncode != 0 and "line 2" in refused.stderr, refused.stderr[-400:]

        # ---- The checker ------------------------------------------------------
        needs_ref = f"00:{labels['NeedsRecord']:04x}"
        check = [sys.executable, str(GB_LIFT), "check", str(export_path), "--rom", str(rom_path),
                 "--source", str(lifted), "--trials", "100"]

        def checked(name: str, *more: str, expect: int = 0) -> tuple[dict, str]:
            report = root / f"{name}.json"
            result = subprocess.run([*check, *more, "--work", str(root / f"work-{name}"), "--json", str(report)],
                                    capture_output=True, text=True, timeout=600)
            assert result.returncode == expect, (name, result.returncode, result.stdout[-800:], result.stderr[-800:])
            return json.loads(report.read_text(encoding="utf-8")), result.stdout

        # Nothing made up gets NeedsRecord through, and a moment of the game's
        # memory does not either: the register is still made up.
        blind, _ = checked("blind", "--function", needs_ref, expect=3)
        assert blind["results"][0]["matched"] == 0, blind["results"][0]
        state_file = root / "moment.state"
        play(executable, root, "moment", "--no-lift", "--save-state-file", str(state_file), frames=100)
        image = root / "moment.img"
        run([sys.executable, str(GB_LIFT), "image", str(state_file), "-o", str(image)])
        from_image, _ = checked("image", "--function", needs_ref, "--image", str(image), "--images-only", expect=3)
        assert from_image["results"][0]["matched"] == 0, from_image["results"][0]
        # The states it was entered in do.
        informed, printed = checked("entries", "--function", needs_ref, "--entries", str(states_path))
        entry = informed["results"][0]
        assert entry["verdict"] == "match" and entry["passed_from"] == ["game.entries"], entry
        assert entry["entry_states"] == {"kept": 5, "matched": 5, "mismatched": 0, "thrown_away": 0}, entry
        assert entry["coverage"] == 1.0 and informed["entries"] is True, entry
        assert "only from game.entries" in printed, printed
        assert "1 of the functions have states a game entered them in; 1 of those did as the original" in printed

        # Every lifted function, from those states and nothing else. Outer was
        # entered from the helper it calls itself: its return address runs
        # deeper down too, and that is not a return gone wrong.
        everything, _ = checked("all", "--entries", str(states_path), "--images-only", expect=0)
        by_id = {entry["function"]: entry for entry in everything["results"]}
        outer = by_id[ids[labels["Outer"]]]
        assert outer["verdict"] == "match" and outer["entry_states"]["thrown_away"] == 0, outer
        assert outer["entry_states"]["matched"] == outer["entry_states"]["kept"] >= 2, outer
        for name in ("WaitFrame", "NeedsRecord", "Twice"):
            assert by_id[ids[labels[name]]]["verdict"] == "match", by_id[ids[labels[name]]]
        # Each state is run with its own memory: the second half of Later
        # is reached from the last of its five and from no other, and with
        # one state asked for there are no rounds that change anything.
        assert [state["arrival"] for state in of("Later")] == [1, 2, 4, 8, 16], of("Later")
        later, _ = checked("later", "--function", f"00:{labels['Later']:04x}", "--entries", str(states_path),
                           "--images-only", "--trials", "1")
        entry = later["results"][0]
        assert entry["trials"] == 5 and entry["entry_states"]["matched"] == 5, entry
        assert entry["coverage"] == 1.0, entry
        # No state was kept of NeverCalled: it is not tried.
        never = by_id[ids[labels["NeverCalled"]]]
        assert never["verdict"] == "inconclusive" and never["trials"] == 0, never
        assert never["entry_states"] == {"kept": 0, "matched": 0, "mismatched": 0, "thrown_away": 0}, never

        # An edit is caught from a state as the game had it.
        edited = root / "edited"
        shutil.copytree(lifted, edited)
        source = edited / "lifted_bank_00.c"
        text = source.read_text(encoding="utf-8")
        store = f"lift_write8(ctx, 0x{0xFF00 + RESULT:04X}, ctx->a);"
        assert text.count(store) == 1, text
        source.write_text(text.replace(store, store.replace(f"{0xFF00 + RESULT:04X}", "FF8F")), encoding="utf-8")
        report = root / "edited.json"
        caught = subprocess.run([sys.executable, str(GB_LIFT), "check", str(export_path), "--rom", str(rom_path),
                                 "--source", str(edited), "--trials", "100", "--function", needs_ref,
                                 "--entries", str(states_path), "--images-only",
                                 "--work", str(root / "work-edited"), "--json", str(report)],
                                capture_output=True, text=True, timeout=600)
        assert caught.returncode == 1, (caught.returncode, caught.stdout[-800:], caught.stderr[-800:])
        entry = json.loads(report.read_text(encoding="utf-8"))["results"][0]
        first = entry["mismatches"][0]
        assert entry["verdict"] == "mismatch" and entry["entry_states"]["mismatched"] == 5, entry
        assert first["arrival"] == 1 and first["as_recorded"] is True and first["states"] == "game.entries", first
        assert (first["entry"]["h"], first["entry"]["l"]) == (RECORD >> 8, RECORD & 0xFF), first
        assert "from game.entries, arrival 1 as the game had it" in caught.stdout, caught.stdout

        # ---- What a file covers -----------------------------------------------
        counts_path = root / "covered.json"
        printed = run([sys.executable, str(GB_LIFT), "entries", str(export_path), "--states", str(states_path),
                       "--report", str(root / "blind.json"), "--json", str(counts_path)]).stdout
        counts = json.loads(counts_path.read_text(encoding="utf-8"))
        assert ids[labels["NeverCalled"]] in counts["never"] and ids[labels["NeedsRecord"]] not in counts["never"]
        assert counts["arrivals"][ids[labels["NeedsRecord"]]] == loops, counts["arrivals"]
        assert counts["functions"] == len(export["functions"]) == counts["arrived_at"] + len(counts["never"])
        assert "inconclusive: 1 functions, 1 arrived at, 0 never" in printed, printed
        # Only what a report does not call a match.
        run([sys.executable, str(GB_LIFT), "entries", str(export_path), "-o", str(root / "open.txt"),
             "--report", str(root / "entries.json")])
        still = [line for line in (root / "open.txt").read_text(encoding="utf-8").splitlines()
                 if not line.startswith("#")]
        assert f"00:{labels['NeedsRecord']:04X}" not in still and len(still) == len(wanted) - 1, still

        # ---- In a source folder -------------------------------------------------
        folder = root / "source"
        run([sys.executable, str(GB_LIFT), "source", str(export_path), "-o", str(folder), "--rom", str(rom_path)])
        run([sys.executable, str(GB_LIFT), "build", str(folder), "--check-only", "--trials", "100"])
        record = json.loads((folder / "lift-source.json").read_text(encoding="utf-8"))
        assert record["functions"][ids[labels["NeedsRecord"]]]["verdict"] == "inconclusive", \
            record["functions"][ids[labels["NeedsRecord"]]]
        (folder / "entries").mkdir()
        shutil.copy(states_path, folder / "entries" / "played.entries")
        run([sys.executable, str(GB_LIFT), "build", str(folder), "--check-only", "--trials", "100"])
        record = json.loads((folder / "lift-source.json").read_text(encoding="utf-8"))
        assert record["functions"][ids[labels["NeedsRecord"]]]["verdict"] == "match", \
            record["functions"][ids[labels["NeedsRecord"]]]
        assert "played.entries" in record["images"], record["images"]
    finally:
        if scratch:
            scratch.cleanup()
    print("entry states end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
