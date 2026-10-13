#!/usr/bin/env python3
"""Run the Nintendo DS test ROMs that report a verdict, and read each verdict.

    python tools/nds/run_nds_tests.py [--nds-run EXE] [--filter TEXT]... [--jobs N]
                                      [--baseline FILE | --no-baseline] [--update-baseline] [--strict]
                                      [--with NAME]... [--ci-frames] [--no-rom]
                                      [--tests FILE] [--roms DIR] [--json FILE] [--keep DIR]
                                      [--timeout SECONDS] [--list] [-- RUNNER OPTIONS...]

The ROMs are the ones tools/nds/build_test_roms.py builds into roms/nds/built
(BlocksDS's hardware tests, arm7wrestler, the window tests). What each one must
show is in tests/nds/blocksds_tests.json, which also says where every
expectation comes from and lists the ROMs that cannot report a verdict on a
console with only the cartridge in it. An entry there is one run from reset:
so many frames with a script of keys, then its checks. A libnds program's
verdict is text in a tilemap, so the runner is asked for that memory
(--dump-mem) rather than for a picture; verdicts in the debug output are read
from the runner's "[arm9] ..." lines, and pictures are compared by the SHA-256
of a rectangle, as tools/nds/oracle_check.py does (its code is used).

The runner is build/bin/nds_run unless --nds-run (or --runner, the same
option) names another build's, or any program taking the same options;
options after -- are passed to it (-- --bios9 FILE --bios7 FILE). --no-rom
leaves the ROM's path out, for a recompiled game, which carries its ROM:
select that game's entries with --filter.

The baseline (tests/nds/blocksds_baseline.json) records which entries pass
and, for those that do not, what the failure was. The exit status is 0 when
every entry the baseline says passes still passes and nothing fails that the
baseline does not know. An entry that newly passes is reported and does not
change the status (as tools/gba/run_gba_tests.py does it); --strict makes
that, and a known failure that now fails differently, a failure too, so that
the baseline has to be brought up to date. --update-baseline rewrites the
selected entries' records and keeps the notes of those that still fail.
--no-baseline ignores the file: the status is then 0 only if everything
passes.

Runs fail closed. No runner, no test file, an empty selection, a ROM that is
not on disk, a built ROM the test file does not mention, a runner that does
not start, exits with an error, runs past --timeout or leaves out a dump: all
of these make the exit status non-zero whatever the baseline says, and
--update-baseline then writes nothing.

--ci-frames runs each entry for the number of frames the test's own CI waits
(on melonDS) where the entry gives one, instead of this machine's allowance:
it shows which verdicts arrive late here. --with NAME runs the entries that
need something the machine lacks (--list names them). --list prints the
entries and the ROMs that are not judged, and runs nothing. --keep DIR keeps
each run's dumps. A JSON record of the run is written to --json FILE
(logs/nds_test_roms.json by default).

Standard library only.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from oracle_check import H, W, read_picture, rect_sha256  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
TESTS = REPO / "tests" / "nds" / "blocksds_tests.json"
BASELINE = REPO / "tests" / "nds" / "blocksds_baseline.json"
ROMS = REPO / "roms" / "nds" / "built"
RESULTS = REPO / "logs" / "nds_test_roms.json"
EXE = ".exe" if os.name == "nt" else ""

COLUMNS, ROWS = 32, 24
CONSOLE_BYTES = COLUMNS * ROWS * 2
UNREADABLE = "�"  # a tile that is not a character of the console font
CHECK_TYPES = ("console", "log", "picture", "rects", "colour_rows")


class TestsError(Exception):
    """The test file is unusable; nothing can be judged with it."""


# ---------------------------------------------------------------- the test file

def _need(condition: bool, where: str, what: str) -> None:
    if not condition:
        raise TestsError(f"{where}: {what}")


def _colour(value, where: str) -> None:
    _need(isinstance(value, list) and len(value) == 3 and all(isinstance(c, int) and 0 <= c < 32 for c in value),
          where, "a colour is three 5-bit values")


def _rect(value, where: str) -> None:
    ok = isinstance(value, list) and len(value) == 4 and all(isinstance(v, int) for v in value)
    _need(ok and value[0] >= 0 and value[1] >= 0 and value[2] > 0 and value[3] > 0 and
          value[0] + value[2] <= W and value[1] + value[3] <= H, where, "a rect is [x, y, width, height] on one screen")


def _rows(value, where: str) -> None:
    _need(isinstance(value, dict) and all(k.isdigit() and int(k) < ROWS and isinstance(v, str) for k, v in value.items()),
          where, f"rows are numbered 0-{ROWS - 1} and hold strings")


def _validate_check(check, consoles: dict, where: str) -> None:
    _need(isinstance(check, dict) and check.get("type") in CHECK_TYPES, where, f"a check's type is one of {CHECK_TYPES}")
    kind = check["type"]
    if kind == "console":
        _need(check.get("console") in consoles, where, f"no console named {check.get('console')!r}")
        _rows(check.get("rows", {}), where)
        _rows(check.get("rows_match", {}), where)
        _need(not set(check.get("rows", {})) & set(check.get("rows_match", {})), where, "a row is in rows and rows_match")
        for pattern in list(check.get("rows_match", {}).values()) + list(check.get("stream", [])):
            try:
                re.compile(pattern)
            except re.error as error:
                raise TestsError(f"{where}: bad regular expression {pattern!r}: {error}") from None
        _need(all(isinstance(text, str) and text for text in check.get("absent", [])), where, "absent holds strings")
        _need(any(check.get(key) for key in ("rows", "rows_match", "stream", "absent", "blank_elsewhere")),
              where, "a console check that checks nothing")
    elif kind == "log":
        _need(check.get("cpu") in (9, 7), where, "a log check's cpu is 9 or 7")
        _need(bool(check.get("lines") or check.get("absent")), where, "a log check that checks nothing")
    elif kind == "picture":
        _need(check.get("screen") in ("top", "bottom"), where, "screen is top or bottom")
        _rect(check.get("rect"), where)
        _need(isinstance(check.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", check["sha256"]) is not None,
              where, "sha256 is 64 hexadecimal digits")
    elif kind == "rects":
        _need(check.get("screen") in ("top", "bottom"), where, "screen is top or bottom")
        _colour(check.get("background"), where)
        _need(isinstance(check.get("rects"), list) and check["rects"], where, "rects is a list")
        for item in check["rects"]:
            _rect(item.get("rect"), where)
            _colour(item.get("colour"), where)
    else:
        _need(check.get("screen") in ("top", "bottom"), where, "screen is top or bottom")
        for key in ("pass_colour", "fail_colour", "done_colour"):
            _colour(check.get(key), where)
        ints = all(isinstance(check.get(key), int) for key in ("first_y", "pitch", "rows"))
        _need(ints and check["pitch"] > 0 and check["rows"] > 0 and check["first_y"] >= 0 and
              check["first_y"] + check["pitch"] * check["rows"] <= H, where, "the rows do not fit the screen")


def load_tests(path: Path) -> dict:
    """The test file, checked: a malformed one must not pass anything."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise TestsError(f"{path}: {error.strerror or error}") from None
    except ValueError as error:
        raise TestsError(f"{path}: not JSON: {error}") from None
    _need(isinstance(document, dict) and isinstance(document.get("entries"), list) and document["entries"],
          str(path), "no entries")
    consoles = document.get("consoles", {})
    for name, console in consoles.items():
        try:
            console["at"] = int(console["address"], 16)
        except (KeyError, TypeError, ValueError):
            raise TestsError(f"{path}: console {name!r} has no hexadecimal address") from None
    seen = set()
    for entry in document["entries"]:
        where = f"{path.name}: entry {entry.get('id')!r}" if isinstance(entry, dict) else f"{path.name}: an entry"
        _need(isinstance(entry, dict) and isinstance(entry.get("id"), str) and entry["id"] not in seen, where,
              "an entry needs an id of its own")
        seen.add(entry["id"])
        _need(isinstance(entry.get("rom"), str) and bool(entry["rom"]), where, "no rom")
        _need(isinstance(entry.get("frames"), int) and entry["frames"] > 0, where, "frames is a positive number")
        _need(isinstance(entry.get("checks"), list) and bool(entry["checks"]), where, "no checks")
        for check in entry["checks"]:
            _validate_check(check, consoles, where)
        every = entry.get("keys_every")
        if every is not None:
            _need(isinstance(every, dict) and isinstance(every.get("buttons"), str) and
                  all(isinstance(every.get(key), int) and every[key] > 0 for key in ("period", "hold")) and
                  isinstance(every.get("first"), int) and every["first"] >= 0, where,
                  "keys_every needs buttons, first, period and hold")
        need = entry.get("requires")
        _need(need is None or need in document.get("requirements", {}), where, f"requires {need!r}, which is not explained")
    for item in document.get("not_judged", []):
        _need(isinstance(item, dict) and isinstance(item.get("rom"), str) and isinstance(item.get("why"), str),
              str(path), "a not_judged item needs rom and why")
    return document


def unlisted_roms(document: dict, roms: Path) -> list[str]:
    """Built ROMs (per the builder's manifest) that the test file neither tests nor explains."""
    try:
        manifest = json.loads((roms / "MANIFEST.json").read_text(encoding="utf-8"))
        built = {item["rom"] for item in manifest["roms"].values()}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return []
    known = {entry["rom"] for entry in document["entries"]} | {item["rom"] for item in document.get("not_judged", [])}
    return sorted(built - known)


def select_entries(document: dict, filters: list[str], enabled: set[str]) -> tuple[list[dict], list[dict]]:
    """The entries to run and the ones skipped for a requirement, in file order."""
    run, skipped = [], []
    for entry in document["entries"]:
        if filters and not any(text in entry["id"] for text in filters):
            continue
        need = entry.get("requires")
        if need and need not in enabled and "all" not in enabled:
            skipped.append(entry)
        else:
            run.append(entry)
    return run, skipped


def entry_frames(entry: dict, ci_frames: bool = False) -> int:
    return entry.get("ci_frames", entry["frames"]) if ci_frames else entry["frames"]


def input_options(entry: dict, frames: int) -> list[str]:
    """The entry's script as runner options."""
    options = []
    for key in entry.get("keys", []):
        options += ["--keys", key]
    every = entry.get("keys_every")
    if every:
        for frame in range(every["first"], frames, every["period"]):
            options += ["--keys", f"{frame}:{every['buttons']}:{every['hold']}"]
    for touch in entry.get("touch", []):
        options += ["--touch", touch]
    for lid in entry.get("lid", []):
        options += ["--lid", lid]
    return options


# ---------------------------------------------------------------- reading verdicts

def console_rows(data: bytes) -> list[str]:
    """The 24 rows of a libnds console's tilemap, without trailing spaces."""
    rows = []
    for row in range(ROWS):
        text = []
        for column in range(COLUMNS):
            at = (row * COLUMNS + column) * 2
            code = ((data[at] | (data[at + 1] << 8)) & 0x3FF) + 32
            text.append(chr(code) if code < 127 else UNREADABLE)
        rows.append("".join(text).rstrip(" "))
    return rows


def debug_lines(output: str) -> dict[int, list[str]]:
    """The runner's "[arm9] ..." and "[arm7] ..." lines, per CPU."""
    lines: dict[int, list[str]] = {9: [], 7: []}
    for line in output.splitlines():
        for cpu in (9, 7):
            prefix = f"[arm{cpu}] "
            if line.startswith(prefix):
                lines[cpu].append(line[len(prefix):].rstrip("\r"))
    return lines


def _check_console(check: dict, rows: list[str], captures: dict) -> list[str]:
    name = check["console"]
    problems = []
    exact, patterns = check.get("rows", {}), check.get("rows_match", {})
    for key in sorted(exact, key=int):
        if rows[int(key)] != exact[key]:
            problems.append(f"{name} row {key}: {rows[int(key)]!r}, expected {exact[key]!r}")
    for key in sorted(patterns, key=int):
        if not re.fullmatch(patterns[key], rows[int(key)]):
            problems.append(f"{name} row {key}: {rows[int(key)]!r} does not match {patterns[key]!r}")
    if check.get("blank_elsewhere"):
        for index, row in enumerate(rows):
            if row and str(index) not in exact and str(index) not in patterns:
                problems.append(f"{name} row {index}: {row!r}, expected a blank row")
    stream = "".join(row.ljust(COLUMNS) for row in rows)
    for pattern in check.get("stream", []):
        match = re.search(pattern, stream)
        if not match:
            problems.append(f"{name}: nothing matches {pattern!r}")
            continue
        for group, value in match.groupdict().items():
            if value is None:
                continue
            if group in captures and captures[group][0] != value:
                problems.append(f"{name}: '{group}' is {value!r} but {captures[group][1]} has {captures[group][0]!r}")
            captures.setdefault(group, (value, name))
    for text in check.get("absent", []):
        if text in stream:
            problems.append(f"{name}: shows {text!r}")
    return problems


def _check_log(check: dict, lines: dict[int, list[str]]) -> list[str]:
    cpu = check["cpu"]
    problems = [f"debug output: no line {line!r} from the ARM{cpu}" for line in check.get("lines", [])
                if line not in lines[cpu]]
    problems += [f"debug output: the ARM{cpu} printed {line!r}" for line in check.get("absent", [])
                 if line in lines[cpu]]
    return problems


def _pixel(pixels: bytes, screen: str, x: int, y: int) -> tuple[int, int, int]:
    at = ((y + (H if screen == "bottom" else 0)) * W + x) * 3
    return pixels[at], pixels[at + 1], pixels[at + 2]


def _check_picture(check: dict, pixels: bytes) -> list[str]:
    got = rect_sha256(pixels, check["screen"], check["rect"])
    if got == check["sha256"]:
        return []
    name = check.get("name") or f"{check['screen']} screen {check['rect']}"
    against = f" from {check['reference']}" if check.get("reference") else ""
    return [f"{name}: not the reference picture{against} (this one's SHA-256 starts {got[:12]})"]


def _check_rects(check: dict, pixels: bytes) -> list[str]:
    screen = check["screen"]
    want = [tuple(check["background"])] * (W * H)
    for item in check["rects"]:
        x, y, w, h = item["rect"]
        for row in range(y, y + h):
            want[row * W + x:row * W + x + w] = [tuple(item["colour"])] * w
    wrong = [(index % W, index // W) for index in range(W * H) if _pixel(pixels, screen, index % W, index // W) != want[index]]
    if not wrong:
        return []
    x, y = wrong[0]
    return [f"{screen} screen: {len(wrong)} pixels are not the expected rectangles, the first at ({x}, {y}): "
            f"{list(_pixel(pixels, screen, x, y))}, expected {list(want[y * W + x])}"]


def _check_colour_rows(check: dict, pixels: bytes) -> list[str]:
    screen, first, pitch, count = check["screen"], check["first_y"], check["pitch"], check["rows"]
    good, bad, done = tuple(check["pass_colour"]), tuple(check["fail_colour"]), tuple(check["done_colour"])
    passed, failed, finished = set(), set(), False
    for y in range(H):
        band = (y - first) // pitch if y >= first else -1
        for x in range(W):
            colour = _pixel(pixels, screen, x, y)
            if colour == good:
                passed.add(band)
            elif colour == bad:
                failed.add(band)
            elif colour == done:
                finished = True
    problems = []
    if failed:
        problems.append(f"{screen} screen: the fail colour is in row{'s' if len(failed) > 1 else ''} "
                        f"{', '.join(str(band) for band in sorted(failed))}")
    missing = [band for band in range(count) if band not in passed]
    if missing:
        problems.append(f"{screen} screen: {count - len(missing)} of {count} rows show the pass colour "
                        f"(not row{'s' if len(missing) > 1 else ''} {', '.join(str(band) for band in missing)})")
    extra = sorted(band for band in passed if band < 0 or band >= count)
    if extra:
        problems.append(f"{screen} screen: the pass colour is also outside the {count} rows")
    if not finished:
        problems.append(f"{screen} screen: the page did not finish (its legend's colour is nowhere)")
    return problems


def run_entry(runner: Path, rom: Path | None, entry: dict, document: dict, *, frames: int | None = None,
              extra: tuple[str, ...] | list[str] = (), keep: Path | None = None, timeout: float = 300.0) -> dict:
    """Run one entry and read its verdict.

    rom is None for a runner that carries its ROM. extra options go to the
    runner after the entry's own. Returns a record: status is "pass", "fail"
    (the checks did not hold) or "error" (there is nothing to judge: the
    runner did not run to the end or left a dump out), failure says why in one
    line, problems lists every check that did not hold, and output is the
    runner's standard output.
    """
    frames = frames or entry["frames"]
    record = {"id": entry["id"], "rom": entry["rom"], "frames": frames, "status": "error", "failure": "",
              "problems": [], "output": "", "seconds": 0.0}
    consoles = sorted({check["console"] for check in entry["checks"] if check["type"] == "console"})
    wants_picture = any(check["type"] in ("picture", "rects", "colour_rows") for check in entry["checks"])

    scratch = None
    if keep:
        folder = keep / re.sub(r"[^A-Za-z0-9_.-]", "_", entry["id"])
        folder.mkdir(parents=True, exist_ok=True)
    else:
        scratch = tempfile.TemporaryDirectory(prefix="nds-tests-")
        folder = Path(scratch.name)
    try:
        command = [str(runner)] + ([str(rom)] if rom else []) + ["--frames", str(frames)] + input_options(entry, frames)
        dumps = {}
        for name in consoles:
            dumps[name] = folder / f"{name}.bin"
            dumps[name].unlink(missing_ok=True)
            command += ["--dump-mem", f"{document['consoles'][name]['at']:08X}:{CONSOLE_BYTES:X}:{dumps[name]}"]
        picture = folder / "frame.ppm"
        if wants_picture:
            picture.unlink(missing_ok=True)
            command += ["--dump-frame", str(picture)]
        command += ["--quiet", *extra]
        record["command"] = command

        started = time.monotonic()
        try:
            done = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                  timeout=timeout)
        except subprocess.TimeoutExpired:
            record["failure"] = f"the runner was still going after {timeout:.0f} s"
            return record
        except OSError as error:
            record["failure"] = f"the runner did not start: {error.strerror or error}"
            return record
        finally:
            record["seconds"] = round(time.monotonic() - started, 2)
        record["output"] = done.stdout
        if done.returncode:
            said = (done.stderr.strip().splitlines() or [""])[-1]
            record["failure"] = f"runner exit status {done.returncode}" + (f": {said}" if said else "")
            return record

        texts = {}
        for name, path in dumps.items():
            try:
                data = path.read_bytes()
            except OSError:
                data = b""
            if len(data) != CONSOLE_BYTES:
                record["failure"] = f"the runner wrote no dump of the {name} console"
                return record
            texts[name] = console_rows(data)
        pixels = read_picture(picture) if wants_picture else None
        if wants_picture and pixels is None:
            record["failure"] = "the runner wrote no 256x384 picture"
            return record

        problems: list[str] = []
        captures: dict = {}
        lines = debug_lines(done.stdout)
        for check in entry["checks"]:
            kind = check["type"]
            if kind == "console":
                problems += _check_console(check, texts[check["console"]], captures)
            elif kind == "log":
                problems += _check_log(check, lines)
            elif kind == "picture":
                problems += _check_picture(check, pixels)
            elif kind == "rects":
                problems += _check_rects(check, pixels)
            else:
                problems += _check_colour_rows(check, pixels)
        record["problems"] = problems
        record["status"] = "fail" if problems else "pass"
        if problems:
            more = f" (and {len(problems) - 1} more)" if len(problems) > 1 else ""
            record["failure"] = problems[0] + more
        if keep:
            (folder / "consoles.txt").write_text(
                "".join(f"[{name}]\n" + "".join(f"{index:2} {row}\n" for index, row in enumerate(rows))
                        for name, rows in texts.items()), encoding="utf-8")
        return record
    finally:
        if scratch:
            scratch.cleanup()


# ---------------------------------------------------------------- the baseline

def load_baseline(path: Path) -> dict:
    """{id: record}; a baseline that cannot be read is an error, not an empty one."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        entries = document["entries"]
        if not isinstance(entries, dict) or not all(record.get("status") in ("pass", "fail") for record in entries.values()):
            raise ValueError("entries must map ids to records with status pass or fail")
    except OSError as error:
        raise TestsError(f"{path}: {error.strerror or error}") from None
    except (ValueError, KeyError, TypeError, AttributeError) as error:
        raise TestsError(f"{path}: not a baseline: {error}") from None
    return entries


BASELINE_COMMENT = [
    "What tools/nds/run_nds_tests.py finds today, entry by entry of blocksds_tests.json: which pass, and for the",
    "others the failure as the runner words it. 'owner' and 'note' are written by hand (the phase of",
    "docs/NDS-PLAN.md that owns the failure, and what is known about it); --update-baseline keeps them for",
    "entries that still fail. Entries that need something the machine lacks ('requires') are not here.",
]


def write_baseline(path: Path, document: dict, results: list[dict], old: dict, whole: bool) -> None:
    """Records for the entries just run; the others' are kept unless the run was of everything."""
    new = {}
    for record in results:
        if record["status"] == "pass":
            new[record["id"]] = {"status": "pass"}
        else:
            kept = {key: old[record["id"]][key] for key in ("owner", "note")
                    if old.get(record["id"], {}).get("status") == "fail" and key in old[record["id"]]}
            new[record["id"]] = {"status": "fail", "failure": record["failure"], **kept}
    order = [entry["id"] for entry in document["entries"]]
    merged = {}
    for ident in order:
        if ident in new:
            merged[ident] = new[ident]
        elif ident in old and not whole:
            merged[ident] = old[ident]
    lines = ["{", '  "comment": [']
    lines += [f"    {json.dumps(line)}{',' if index + 1 < len(BASELINE_COMMENT) else ''}"
              for index, line in enumerate(BASELINE_COMMENT)]
    lines += ["  ],", '  "entries": {']
    items = list(merged.items())
    for index, (ident, record) in enumerate(items):
        comma = "," if index + 1 < len(items) else ""
        if record["status"] == "pass":
            lines.append(f"    {json.dumps(ident)}: {json.dumps(record)}{comma}")
        else:
            lines.append(f"    {json.dumps(ident)}: {{")
            fields = list(record.items())
            for position, (key, value) in enumerate(fields):
                lines.append(f"      {json.dumps(key)}: {json.dumps(value, ensure_ascii=False)}"
                             f"{',' if position + 1 < len(fields) else ''}")
            lines.append(f"    }}{comma}")
    lines += ["  }", "}"]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def compare_with_baseline(results: list[dict], baseline: dict, selected: set[str], filtered: bool,
                          document: dict) -> tuple[list[str], bool, bool]:
    """Lines to print, whether the run is worse than the baseline, whether the baseline is out of date."""
    lines, worse, stale = [], False, False
    status = {record["id"]: record for record in results}
    for ident, record in status.items():
        known = baseline.get(ident)
        if record["status"] == "error":
            continue  # reported, and fatal, on its own
        if known is None:
            stale = True
            if record["status"] == "pass":
                lines.append(f"newly passing (not in the baseline; update it): {ident}")
            else:
                worse = True
                lines.append(f"FAILS and is not in the baseline: {ident}")
        elif known["status"] == "pass" and record["status"] == "fail":
            worse = True
            lines.append(f"REGRESSED: {ident}: {record['failure']}")
        elif known["status"] == "fail" and record["status"] == "pass":
            stale = True
            lines.append(f"newly passing (update the baseline): {ident}")
        elif known["status"] == "fail" and known.get("failure") != record["failure"]:
            stale = True
            lines.append(f"fails differently than the baseline records: {ident}: {record['failure']}")
    # An entry the baseline knows that this run should have covered and did not.
    in_file = {entry["id"] for entry in document["entries"]}
    for ident in baseline:
        if ident in status:
            continue
        if ident not in in_file:
            if not filtered:
                worse = True
                lines.append(f"MISSING: {ident} is in the baseline but not in the test file")
        elif ident in selected or not filtered:
            # Selected by the filter (or everything was) yet not run: skipped for a requirement.
            worse = True
            lines.append(f"MISSING from the run: {ident}")
    return lines, worse, stale


# ---------------------------------------------------------------- the program

def print_list(document: dict, enabled: set[str]) -> None:
    for entry in document["entries"]:
        script = " ".join(input_options(entry, entry["frames"])[:8])
        if len(input_options(entry, entry["frames"])) > 8:
            script += " ..."
        need = f"  [needs {entry['requires']}]" if entry.get("requires") else ""
        print(f"{entry['id']}{need}\n    {entry['rom']}, {entry['frames']} frames{', ' + script if script else ''}\n"
              f"    {entry.get('about', '')}")
    needs = sorted({entry["requires"] for entry in document["entries"] if entry.get("requires")})
    if needs:
        print("\nrequirements (run those entries with --with NAME):")
        for need in needs:
            print(f"    {need}: {document['requirements'][need]}")
    if document.get("not_judged"):
        print("\nnot judged:")
        for item in document["not_judged"]:
            print(f"    {item['rom']}: {item['why']}")


def main() -> int:
    argv = sys.argv[1:]
    extra: list[str] = []
    if "--" in argv:
        split = argv.index("--")
        argv, extra = argv[:split], argv[split + 1:]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     epilog="Options after -- are passed to the runner.")
    parser.add_argument("--runner", "--nds-run", dest="runner", type=Path,
                        default=REPO / "build" / "bin" / f"nds_run{EXE}",
                        help="the runner (default: build/bin/nds_run)")
    parser.add_argument("--filter", action="append", default=[], metavar="TEXT",
                        help="run only entries whose id contains TEXT (repeatable)")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--tests", type=Path, default=TESTS)
    parser.add_argument("--roms", type=Path, default=ROMS, help="directory the entries' rom paths are under")
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--no-baseline", action="store_true", help="ignore the baseline: everything must pass")
    parser.add_argument("--update-baseline", action="store_true", help="write what this run found to the baseline")
    parser.add_argument("--strict", action="store_true",
                        help="also fail when the baseline is out of date (an entry newly passes or fails differently)")
    parser.add_argument("--with", dest="enabled", action="append", default=[], metavar="NAME",
                        help="also run the entries that require NAME ('all' for every one)")
    parser.add_argument("--ci-frames", action="store_true",
                        help="run each entry for the frames its own CI waits, where the entry gives them")
    parser.add_argument("--no-rom", action="store_true", help="the runner carries its ROM: pass no ROM path")
    parser.add_argument("--json", type=Path, default=RESULTS, help="where to write the run's record")
    parser.add_argument("--keep", type=Path, metavar="DIR", help="keep each run's dumps here")
    parser.add_argument("--timeout", type=float, default=300.0, help="seconds allowed per run")
    parser.add_argument("--list", action="store_true", help="list the entries and the ROMs not judged; run nothing")
    args = parser.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")

    try:
        document = load_tests(args.tests)
    except TestsError as error:
        print(f"{error} (fail closed)")
        return 1
    enabled = set(args.enabled)
    if args.list:
        print_list(document, enabled)
        return 0
    if not args.runner.is_file():
        print(f"runner not found: {args.runner}")
        return 1
    entries, skipped = select_entries(document, args.filter, enabled)
    if not entries:
        print("no tests selected (fail closed)" + (f"; {len(skipped)} need --with" if skipped else ""))
        return 1

    setup_errors = []
    if not args.no_rom:
        for rom in unlisted_roms(document, args.roms):
            setup_errors.append(f"{rom} was built but {args.tests.name} neither tests it nor lists it as not judged")
    baseline = None
    if not args.no_baseline and not (args.update_baseline and not args.baseline.exists()):
        try:
            baseline = load_baseline(args.baseline)
        except TestsError as error:
            setup_errors.append(str(error))

    def run(entry: dict) -> dict:
        rom = None if args.no_rom else args.roms / entry["rom"]
        if rom is not None and not rom.is_file():
            return {"id": entry["id"], "rom": entry["rom"], "frames": entry_frames(entry, args.ci_frames),
                    "status": "error", "failure": f"ROM not found: {rom} (run tools/nds/build_test_roms.py)",
                    "problems": [], "output": "", "seconds": 0.0}
        return run_entry(args.runner, rom, entry, document, frames=entry_frames(entry, args.ci_frames), extra=extra,
                         keep=args.keep, timeout=args.timeout)

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        results = list(pool.map(run, entries))
    elapsed = time.monotonic() - started

    passed = sum(record["status"] == "pass" for record in results)
    failed = sum(record["status"] == "fail" for record in results)
    errors = sum(record["status"] == "error" for record in results)
    for record in results:
        flag = {"pass": "ok   ", "fail": "FAIL ", "error": "ERROR"}[record["status"]]
        known = baseline.get(record["id"], {}) if baseline else {}
        why = ""
        if record["status"] == "fail" and known.get("status") == "fail":
            why = f"  [known{': ' + known['owner'] if known.get('owner') else ''}]"
        print(f"{flag} {record['id']}{'  ' + record['failure'] if record['failure'] else ''}{why}")
    print(f"\n{passed}/{len(results)} passed, {failed} failed, {errors} could not be judged, in {elapsed:.1f} s"
          + (" (frames as the tests' own CI waits)" if args.ci_frames else ""))
    if skipped:
        needs = sorted({entry["requires"] for entry in skipped})
        print(f"{len(skipped)} entries skipped, needing {', '.join(needs)} (--with NAME runs them; --list explains)")
    not_judged = document.get("not_judged", [])
    if not_judged and not args.filter:
        print(f"{len(not_judged)} built ROMs cannot report a verdict here (--list says why)")
    for problem in setup_errors:
        print(f"ERROR {problem}")

    record_out = {
        "runner": str(args.runner), "runner_options": extra, "tests": str(args.tests),
        "frames": "ci" if args.ci_frames else "entry", "seconds": round(elapsed, 1),
        "summary": {"run": len(results), "passed": passed, "failed": failed, "errors": errors,
                    "skipped": len(skipped), "not_judged": len(not_judged)},
        "entries": [{key: record[key] for key in ("id", "rom", "frames", "status", "failure", "problems", "seconds")}
                    for record in results],
        "skipped": [{"id": entry["id"], "requires": entry["requires"]} for entry in skipped],
        "not_judged": not_judged,
        "errors": setup_errors,
    }
    try:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(record_out, indent=1) + "\n", encoding="utf-8")
    except OSError as error:
        print(f"ERROR cannot write {args.json}: {error.strerror or error}")
        setup_errors.append("no record written")

    broken = bool(errors or setup_errors)
    if args.update_baseline:
        if broken:
            print("the baseline was not written: the run above is incomplete")
            return 1
        old = baseline or {}
        write_baseline(args.baseline, document, results, old, whole=not args.filter and not skipped)
        print(f"baseline written: {args.baseline}")
        return 0
    if baseline is None:
        return 0 if passed == len(results) and not broken else 1
    selected = {entry["id"] for entry in entries}
    if args.filter:
        # Entries the filter selects but a requirement kept out are not expected in the baseline.
        selected |= {entry["id"] for entry in skipped}
    lines, worse, stale = compare_with_baseline(results, baseline, selected, bool(args.filter), document)
    for line in lines:
        print(line)
    if broken or worse or (args.strict and stale):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
