#!/usr/bin/env python3
"""Signatures for the functions of a source folder: what each one takes, what
it gives back and what it leaves as it came.

Assembly has no signatures. A function reads whatever registers it reads and
leaves something in every one of them, and which of that is an argument, a
result, a register saved and restored, or what happened to be left over, is
nowhere written down. This works it out, a function at a time, into one file
of the folder a person can read and correct:

    00:36E0  takes a bc hl  gives hl  keeps de  ; FillMemory

- `takes`: the registers and flags it reads. Found by the checker from the
  original itself (`check --observe`): one is read if another value on the
  way in changes anything else that comes out, or how long the function
  takes.
- `keeps`: the ones that come out as they went in, from every state tried.
- `gives`: of the rest, the ones something reads after the function has
  returned: a caller, or what a function it jumps into returns to. Found
  from the export, by following what is read after every call there is
  (`read_after_return`). Where the function has a way in that the export
  cannot list the callers of, that is everything it changes: unless the
  game has been played with `--lift-callers`, which notes where each
  function returned to. Then such a function gives what is read after the
  places it was seen to return to, and its line says `[as played]`.
- Whatever is in none of the three is left over: undefined.

A signature is then held against the function (LIFTING.md, "Signatures"):
by the checker, which gives what is not taken another value, puts back what
is kept and does not compare what is left over; and by the game run with
`--lift-signatures`, which puts a value that is nobody's into what is left
over, so that a run compared with the original shows a register that was a
result after all.

Called from gb_lift.py, which hands itself over as `lift`, as
gb_lift_source.py is.
"""

from __future__ import annotations

import contextlib
import io
import json
import re
import sys
from pathlib import Path

import gb_lift_codegen as codegen
import gb_lift_source as source

FILE = "signatures.txt"
MASKS = "signatures.masks"  # in the folder's .lift/: what the game reads
ITEMS = ("a", "b", "c", "d", "e", "h", "l", "fz", "fn", "fh", "fc")
EVERYTHING = (1 << len(ITEMS)) - 1
ENOUGH_STATES = 8
NO_LINE = "none"  # in the record: no line the tool could write held for this function

HEADING = """\
# What each function takes, gives and keeps: one a line, as
#     BB:AAAA  takes a bc hl  gives hl  keeps de  ; Name
# Registers are a b c d e h l (bc, de, hl for both halves), flags fz fn fh fc
# (flags for all four), - for none. What a function neither gives nor keeps
# it leaves undefined. `gb_lift.py signatures` writes these lines and writes
# one again when it finds out more, unless somebody has changed it: a line
# you correct is yours, and a function you take the line of out has none
# until you ask for it back (--restore BB:AAAA). `gb_lift.py build` holds
# every function to its line.
"""


# --------------------------------------------------------------------------
# The file
# --------------------------------------------------------------------------

def words(bits: int) -> str:
    """Bits as the file spells them: pairs and `flags` where all are there."""
    out = []
    if bits & 0x001:
        out.append("a")
    for pair, high, low in (("bc", 0x002, 0x004), ("de", 0x008, 0x010), ("hl", 0x020, 0x040)):
        if bits & high and bits & low:
            out.append(pair)
        elif bits & high:
            out.append(pair[0])
        elif bits & low:
            out.append(pair[1])
    if bits & 0x780 == 0x780:
        out.append("flags")
    else:
        out += [name for name, bit in (("fz", 0x080), ("fn", 0x100), ("fh", 0x200), ("fc", 0x400)) if bits & bit]
    return " ".join(out) or "-"


def names_of(bits: int) -> list[str]:
    return [name for number, name in enumerate(ITEMS) if bits >> number & 1]


class Signature:
    def __init__(self, takes: int, gives: int, keeps: int) -> None:
        self.takes, self.gives, self.keeps = takes, gives, keeps

    def line(self, function_id: str, name: str) -> str:
        return (f"{source.reference(function_id)}  takes {words(self.takes)}  gives {words(self.gives)}  "
                f"keeps {words(self.keeps)}" + (f"  ; {name}" if name else ""))

    def same(self, other: "Signature") -> bool:
        return (self.takes, self.gives, self.keeps) == (other.takes, other.gives, other.keeps)


def read(folder: Path, lift) -> dict[str, tuple[Signature, str]]:
    """signatures.txt as {function: (signature, the line as it stands)}."""
    path = folder / FILE
    found: dict[str, tuple[Signature, str]] = {}
    if not path.is_file():
        return found
    for number, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        line = raw.split("#", 1)[0].split(";", 1)[0].split()
        if not line:
            continue
        where = f"{path}:{number}"
        if not re.fullmatch(r"[0-9A-Fa-f]{1,4}:[0-9A-Fa-f]{4}", line[0]):
            raise lift.LiftError(f"{where}: a line starts with a function as BB:AAAA, not {line[0]!r}")
        function_id = lift.parse_function_ref(line[0])
        parts = {"takes": 0, "gives": 0, "keeps": 0}
        key = None
        for word in line[1:]:
            if word in parts:
                key = word
            elif key is None:
                raise lift.LiftError(f"{where}: {word!r} comes before any of takes, gives, keeps")
            elif word != "-":
                parts[key] |= lift.signature_bits([word], where)
        if parts["gives"] & parts["keeps"]:
            raise lift.LiftError(f"{where}: {words(parts['gives'] & parts['keeps'])} is both given and kept")
        if function_id in found:
            raise lift.LiftError(f"{where}: {line[0]} has a line already")
        found[function_id] = (Signature(parts["takes"], parts["gives"], parts["keeps"]), source.tidy([raw]))
    return found


def contract_entry(signature: Signature) -> dict:
    return {"takes": names_of(signature.takes), "gives": names_of(signature.gives),
            "keeps": names_of(signature.keeps)}


def write_masks(folder: Path, signatures: dict[str, tuple[Signature, str]]) -> Path:
    """The file a game run with --lift-signatures reads."""
    lines = ["# Written by tools/gb_lift.py from signatures.txt: BBBB:AAAA TAKES GIVES KEEPS, hexadecimal."]
    for function_id, (signature, _) in sorted(signatures.items()):
        bank, address = function_id.split(":")[2:4]
        lines.append(f"{bank}:{address} {signature.takes:03x} {signature.gives:03x} {signature.keeps:03x}")
    path = folder / source.TOOL / MASKS
    source.write_text(path, "\n".join(lines) + "\n")
    return path


# --------------------------------------------------------------------------
# What is read after a function returns
# --------------------------------------------------------------------------

# gb_lift_codegen's bits for registers and flags, as this module's.
_FROM_CODEGEN = [(codegen.BIT[name], 1 << number) for number, name in enumerate(ITEMS[:7])] + \
                [(codegen.FZ, 0x080), (codegen.FN, 0x100), (codegen.FH, 0x200), (codegen.FC, 0x400)]


def _bits(mask: int) -> int:
    return sum(mine for theirs, mine in _FROM_CODEGEN if mask & theirs)


class _Function:
    """One function of the export, ready for the question asked of it over
    and over: with this read after it returns, what is read after each of
    its calls?"""

    def __init__(self, function: dict) -> None:
        self.id = function["id"]
        self.bank, self.address = function["bank"], function["address"]
        self.pushes: list[int] = []  # addresses it loads into a pair and pushes: where a jump is to come back
        self.jumps_through_register = False
        self.needs = _bits(codegen.summary_bits(function, "needs")) if "needs" in function["registers"] else EVERYTHING
        self.always_writes = _bits(codegen.summary_bits(function, "always_writes")) \
            if "always_writes" in function["registers"] else 0
        self.sites = {site["site"]: site for site in function["calls"]}
        self.blocks = []
        self.usable = True
        loaded: tuple[int, int] | None = None  # (pair, address) of the `ld rr, nn` just before
        for block in function["blocks"]:
            instructions = []
            for raw in block["instructions"]:
                data = bytes.fromhex(raw["bytes"])
                try:
                    kind = codegen.control_kind(data)
                except codegen.Unliftable:
                    self.usable = False
                    kind = "plain"
                reads, writes = codegen.effects(data)
                instructions.append((raw["address"], kind, _bits(reads), _bits(writes)))
                if data[0] in (0xC5, 0xD5, 0xE5) and loaded and loaded[0] == (data[0] >> 4) & 3:
                    self.pushes.append(loaded[1])
                loaded = ((data[0] >> 4) & 3, data[1] | data[2] << 8) if data[0] in (0x01, 0x11, 0x21) else None
                if kind == "jp_hl":
                    self.jumps_through_register = True
            self.blocks.append((block["address"], instructions, list(block["successors"]),
                                list(block.get("jump_returns_to", ())), list(block["leaves_to"]),
                                bool(block["returns"]), bool(block["unknown_exit"])))
        # Where what the function jumps to through a register comes back to.
        self.comes_back = sorted({address for block in self.blocks for address in block[3]})

    def flow(self, after_return: int, summaries: dict[str, "_Function"]) -> tuple[dict[int, int], dict[int, int]]:
        """({call site: what is read after the call}, {block: what is read
        when it is entered}), with `after_return` read once it has returned."""
        if not self.usable:
            return {site: EVERYTHING for site in self.sites}, {block[0]: EVERYTHING for block in self.blocks}
        entered = {block[0]: 0 for block in self.blocks}
        after_site: dict[int, int] = {}
        changed = True
        while changed:
            changed = False
            for address, instructions, successors, comes_back, leaves_to, returns, unknown in reversed(self.blocks):
                live = 0
                for successor in successors + comes_back:
                    live |= entered.get(successor, EVERYTHING)
                if returns:
                    live |= after_return
                if unknown:
                    live |= EVERYTHING
                for into in leaves_to:
                    callee = summaries.get(into)
                    live |= callee.needs | (after_return & ~callee.always_writes) if callee else EVERYTHING
                for at, kind, reads, writes in reversed(instructions):
                    if kind in ("call", "call_cc", "rst"):
                        after_site[at] = live
                        site = self.sites.get(at)
                        callee = summaries.get(site["function"]) if site and site.get("function") else None
                        through = callee.needs | (live & ~callee.always_writes) if callee else EVERYTHING
                        live = through | reads | (live if kind == "call_cc" else 0)
                    elif kind == "jp_hl":
                        live = EVERYTHING
                    else:
                        live = (live & ~writes) | reads
                if live != entered[address]:
                    entered[address] = live
                    changed = True
        return after_site, entered


# Ways into a function that leave no list of who comes that way.
LISTED_WAYS_IN = frozenset(("far_call", "table_call"))

CALLERS = "callers"  # in the folder: files a game run with --lift-callers wrote


def read_callers(paths: list[Path], lift) -> dict[str, set[tuple]]:
    """Files a game run with --lift-callers wrote, taken together:
    {function: {(bank, address it returned to, the function running around it
    or None, bank, address that one was to return to)}}."""
    seen: dict[str, set[tuple]] = {}

    def place(text: str) -> tuple[int, int]:
        bank, address = text.split(":")
        return int(bank, 16), int(address, 16)

    for path in paths:
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as error:
            raise lift.LiftError(f"cannot read {path}: {error}") from error
        for number, line in enumerate(lines, 1):
            words_ = line.split("#", 1)[0].split()
            if not words_:
                continue
            try:
                callee, site = place(words_[0]), place(words_[1])
                within = None if words_[2] == "-" else place(words_[2])
                within_site = (0, 0) if words_[3] == "-" else place(words_[3])
                int(words_[4])
            except (ValueError, IndexError):
                raise lift.LiftError(f"{path}:{number}: not a line a game writes with --lift-callers") from None
            seen.setdefault(f"gbfn:v1:{callee[0]:04x}:{callee[1]:04x}", set()).add(
                (site[0], site[1], f"gbfn:v1:{within[0]:04x}:{within[1]:04x}" if within else None, *within_site))
    return seen


def read_after_return(export: dict, played: dict[str, set[tuple]] | None = None) -> tuple[dict[str, int], set[str]]:
    """For every function, the registers and flags something reads after it
    has returned: what its callers read after their calls, what is read
    after the helper a far call goes through has finished, and what is read
    after any function that jumps or runs on into it. Where it has a way in
    nobody can list the users of, that is EVERYTHING, unless `played` has
    where a game saw it return to (`read_callers`): then it is what is read
    after those places as well, and the function is in the second result,
    the ones known as played. A place nothing here can account for (the
    middle of a function an interrupt came in) makes it EVERYTHING again,
    and a caller a run saw that the export does not list is counted for a
    function with every way in listed, too."""
    functions = {function["id"]: _Function(function) for function in export["functions"]}
    index = {function["id"]: function for function in export["functions"]}

    # Where a call returns to, and where what a helper jumped to comes back.
    returns_at: dict[tuple[int, int], tuple[str, int]] = {}
    comes_back_at: dict[tuple[int, int], str] = {}
    for function in export["functions"]:
        for block in function["blocks"]:
            for raw in block["instructions"]:
                data = bytes.fromhex(raw["bytes"])
                try:
                    kind = codegen.control_kind(data)
                except codegen.Unliftable:
                    continue
                if kind in ("call", "call_cc", "rst"):
                    back = (raw["address"] + len(data)) & 0xFFFF
                    returns_at[(function["bank"] if back >= 0x4000 else 0, back)] = (function["id"], raw["address"])
            for address in block.get("jump_returns_to", ()):
                comes_back_at[(function["bank"] if address >= 0x4000 else 0, address)] = function["id"]

    # A helper that pushes an address and jumps through a register comes
    # back at that address. Where the export keeps the helper whole, its
    # blocks say so (`jump_returns_to`). Where it has made a function of the
    # address (a run saw something return there), that function is the rest
    # of the helper: its continuation.
    at = {(function["bank"], function["address"]): function["id"] for function in export["functions"]}
    continues: dict[str, list[str]] = {}
    continuation_of: dict[tuple[int, int], str] = {}
    for function in functions.values():
        if function.comes_back or not function.jumps_through_register:
            continue
        for address in function.pushes:
            rest = at.get((function.bank if address >= 0x4000 else 0, address))
            if rest and rest != function.id:
                continues.setdefault(function.id, []).append(rest)
                continuation_of[(functions[rest].bank, address)] = function.id

    # What a played game adds: for the function a place is in, whose return
    # there is to be counted and how.
    hosted: dict[str, list[tuple]] = {}
    unaccounted: set[str] = set()
    for callee, places in (played or {}).items():
        if callee not in functions:
            continue
        for bank, pc, within, within_bank, within_pc in places:
            if (bank, pc) in returns_at:
                host, call = returns_at[(bank, pc)]
                hosted.setdefault(host, []).append((callee, "call", call, None))
            elif (bank, pc) in comes_back_at:
                helper = comes_back_at[(bank, pc)]
                if within == helper and (within_bank, within_pc) in returns_at:
                    # Called through a helper: read after it is what the
                    # helper's own caller reads, once the helper is done.
                    host, call = returns_at[(within_bank, within_pc)]
                    hosted.setdefault(host, []).append((callee, "through", call, helper))
                else:
                    hosted.setdefault(helper, []).append((callee, "block", pc, None))
            elif (bank, pc) in continuation_of:
                # Back at the rest of a helper: read after it is what is read
                # after the helper, whoever called that.
                hosted.setdefault(continuation_of[(bank, pc)], []).append((callee, "rest", pc, None))
            else:
                unaccounted.add(callee)

    after = {}
    as_played: set[str] = set()
    for function in export["functions"]:
        function_id = function["id"]
        unlisted = [way for way in function.get("entered_by", ()) if way not in LISTED_WAYS_IN]
        if function_id in unaccounted:
            after[function_id] = EVERYTHING
        elif unlisted and played and function_id in played:
            after[function_id] = 0
            as_played.add(function_id)
        else:
            after[function_id] = EVERYTHING if unlisted else 0

    through_cache: dict[tuple[str, int], int] = {}

    def through(helper_id: str | None, read_after_helper: int) -> int:
        """What is read once the function a helper went on to has come back
        to the helper, given what is read after the helper itself."""
        helper = functions.get(helper_id) if helper_id else None
        if helper is None or not (helper.comes_back or helper.id in continues):
            return EVERYTHING
        key = (helper.id, read_after_helper)
        if key not in through_cache:
            through_cache[key] = 0
            if helper.comes_back:
                _, entered = helper.flow(read_after_helper, functions)
                for address in helper.comes_back:
                    through_cache[key] |= entered.get(address, EVERYTHING)
            for rest in continues.get(helper.id, ()):
                # The rest of the helper returns where the helper would have.
                _, entered = functions[rest].flow(read_after_helper, functions)
                through_cache[key] |= entered.get(functions[rest].address, EVERYTHING)
        return through_cache[key]

    def targets(site: dict) -> list[str]:
        found = []
        if "reaches" in site:
            found.append(site["reaches"]["function"])
        if "reaches_table" in site:
            found += [entry["function"] for entry in site["reaches_table"]["entries"]]
        if "reaches_seen" in site:
            found += [entry["function"] for entry in site["reaches_seen"]]
        return [target for target in found if target]

    pending = list(functions)
    queued = set(pending)
    while pending:
        function_id = pending.pop()
        queued.discard(function_id)
        function = functions[function_id]
        after_site, entered = function.flow(after[function_id], functions)

        def add(target: str | None, bits: int) -> None:
            if target in after and bits & ~after[target]:
                after[target] |= bits
                if target not in queued:
                    queued.add(target)
                    pending.append(target)

        for site in index[function_id]["calls"]:
            read = after_site.get(site["site"], EVERYTHING)
            add(site.get("function"), read)
            for target in targets(site):
                add(target, through(site.get("function"), read))
        for jump in index[function_id].get("jumps_out", ()):
            # It returns to this function's caller, whatever it goes through.
            add(jump.get("function"), after[function_id])
            for target in targets(jump):
                add(target, through(jump.get("function"), after[function_id]))
        for jump in index[function_id].get("seen_jumps", ()):
            for entry in jump.get("targets", ()):
                if entry.get("function") not in as_played:
                    add(entry.get("function"), EVERYTHING)
        for callee, kind, where, helper in hosted.get(function_id, ()):
            if kind == "call":
                add(callee, after_site.get(where, EVERYTHING))
            elif kind == "through":
                add(callee, through(helper, after_site.get(where, EVERYTHING)))
            elif kind == "rest":
                add(callee, through(function_id, after[function_id]))
            else:
                add(callee, entered.get(where, EVERYTHING))
    return after, as_played


# --------------------------------------------------------------------------
# signatures: the command
# --------------------------------------------------------------------------

def _quietly(lift, arguments: list[str]) -> tuple[int, str]:
    """Run another command of gb_lift.py; its exit code and what it printed."""
    args = lift.build_parser().parse_args(arguments)
    said = io.StringIO()
    with contextlib.redirect_stdout(said), contextlib.redirect_stderr(said):
        try:
            code = args.run(args)
        except lift.LiftError as error:
            return 2, said.getvalue() + f"\n{error}\n"
    return code, said.getvalue()


def command_signatures(args, lift) -> int:
    folder = Path(args.folder).resolve()
    record = source.load_record(folder, lift)
    export = lift.load_export(folder / source.TOOL / "export.json")
    index = lift.function_index(export)
    files, units = source.read_folder(folder, lift)
    rom = args.rom or record.get("rom_path")
    if not rom or not Path(rom).is_file():
        raise lift.LiftError("no ROM: give --rom" if not rom else f"no ROM at {rom}: give --rom")
    liftable = sorted(function_id for function_id in units if function_id in index and
                      not any(hazard in index[function_id]["hazards"] for hazard in lift.UNLIFTABLE))
    restore = {lift.parse_function_ref(item) for item in args.restore}

    # ---- What each function reads and what it changes: from the original.
    def observe(which: list[str], states: int, name: str) -> dict[str, dict]:
        """The checker's report for these functions, each watched from
        `states` states of a kind at most (0: from every one)."""
        report_path = folder / source.TOOL / name
        report_path.unlink(missing_ok=True)
        support, _ = source.support_code(folder, files, lift)
        command = ["check", str(folder / source.TOOL / "export.json"), "--rom", str(rom),
                   "--work", str(folder / source.TOOL / "check"), "--json", str(report_path),
                   "--trials", str(args.trials), "--seed", str(args.seed), "--observe",
                   "--observe-states", str(states), "--include", str(folder / source.SOURCES)]
        for file_name in sorted({units[function_id][0] for function_id in which}):
            command += ["--source", str(folder / source.SOURCES / file_name)]
        if len(which) < len(liftable):
            for function_id in which:
                command += ["--function", function_id]
        for path in support:
            command += ["--support", str(path)]
        for image in sorted(path.resolve() for path in (folder / "images").glob("*.img")):
            command += ["--image", str(image)]
        for entries in sorted(path.resolve() for path in (folder / "entries").glob("*.entries")):
            command += ["--entries", str(entries)]
        scratch = source.read_scratch(folder, lift)
        if scratch:
            # What a byte of scratch holds is no result: a register that only
            # ever reaches one is not read.
            command += ["--ignore", ",".join(scratch)]
        if args.jobs:
            command += ["--jobs", str(args.jobs)]
        code, said = _quietly(lift, command)
        source.write_text(folder / source.TOOL / (name + ".log"), said)
        if not report_path.is_file():
            raise lift.LiftError(f"the checker gave no report:\n{said[-2000:]}")
        return {entry["function"]: entry for entry in json.loads(report_path.read_text(encoding="utf-8"))["results"]}

    if args.observed:
        observed = {entry["function"]: entry
                    for entry in json.loads(Path(args.observed).read_text(encoding="utf-8"))["results"]}
    else:
        print(f"asking the original what {len(liftable)} functions read and change")
        sys.stdout.flush()
        observed = observe(liftable, 160, "observed.json")

    # ---- What is read after each returns: from the export, and from where
    # a played game saw each return to.
    caller_files = sorted((folder / CALLERS).glob("*")) if (folder / CALLERS).is_dir() else []
    for item in args.callers:
        path = Path(item)
        caller_files += sorted(path.glob("*")) if path.is_dir() else [path]
    played = read_callers([path for path in caller_files if path.is_file()], lift)
    after, as_played = read_after_return(export, played)

    def needed(function_id: str) -> int:
        """What the export says it may read: never too little."""
        function = index[function_id]
        return _bits(codegen.summary_bits(function, "needs")) if "needs" in function["registers"] else EVERYTHING

    def proposed(function_id: str, cautious: bool = False) -> Signature | None:
        entry = observed.get(function_id)
        if not entry or entry["verdict"] != "match" or "observed" not in entry or \
                entry["observed"]["states"] < ENOUGH_STATES:
            return None
        seen = entry["observed"]
        keeps = EVERYTHING & ~seen["changed"]
        gives = seen["changed"] & after.get(function_id, EVERYTHING)
        # What it gives and, some of the time, leaves as it came: that time
        # the caller has back what it handed in, so it is taken as well.
        takes = seen["reads"] | (gives & seen["through"])
        # `cautious`: a register is read on a path too rare to have been
        # watched. Then it takes whatever the export says it may read, which
        # counts a register that is only saved as read.
        return Signature(takes | (needed(function_id) if cautious else 0), gives, keeps)

    def fallback(function_id: str) -> Signature:
        """What the export alone says, which errs on the side of taking and
        giving: for a function whose observed line did not hold."""
        function = index[function_id]
        needs = needed(function_id)
        writes = _bits(codegen.summary_bits(function, "writes"))
        always = _bits(codegen.summary_bits(function, "always_writes")) if "always_writes" in function["registers"] else 0
        seen = observed.get(function_id, {}).get("observed", {"reads": 0, "changed": 0})
        gives = writes | seen["changed"]
        # Whatever it does not write every time may come out as it went in.
        return Signature(needs | seen["reads"] | (gives & ~always), gives, EVERYTHING & ~gives)

    def name_of(function_id: str) -> str:
        name = units[function_id][1].name if function_id in units else ""
        # What it gives is as good as the play that saw who calls it.
        return name + (" [as played]" if function_id in as_played else "")

    # ---- The file: the tool's lines written again, a person's kept.
    have = read(folder, lift)
    wrote = record.get("signatures", {})
    lines: dict[str, str] = {}
    mine: dict[str, str] = {}
    kept_by_hand = taken_out = 0
    none_held: list[str] = []
    for function_id in liftable:
        new = proposed(function_id)
        if function_id in have and wrote.get(function_id) != source.digest(have[function_id][1]) and \
                function_id not in restore:
            lines[function_id] = have[function_id][1]  # somebody's own
            kept_by_hand += 1
        elif function_id not in have and function_id in wrote and function_id not in restore:
            # Somebody took the line out, or no line the tool could write held.
            if wrote[function_id] == NO_LINE:
                none_held.append(function_id)
            else:
                taken_out += 1
            mine[function_id] = wrote[function_id]
        elif new is not None:
            lines[function_id] = new.line(function_id, name_of(function_id))
            mine[function_id] = source.digest(lines[function_id])
    for function_id, (_, line) in have.items():
        if function_id not in liftable:
            lines[function_id] = line  # a function the folder does not have now: not this tool's to drop

    def write() -> None:
        ordered = sorted(lines.items(), key=lambda item: item[0])
        source.write_text(folder / FILE, HEADING + "\n".join(line for _, line in ordered) + ("\n" if ordered else ""))
        record["signatures"] = dict(sorted(mine.items()))
        source.save_record(folder, record)

    # A function whose record does not say what time it took held to its
    # line was checked before the record kept that: it is held again.
    for function_id in mine:
        entry = record["functions"].get(function_id, {})
        if function_id in lines and "signature" in entry and "clock" not in entry:
            del entry["signature"]
    write()

    def slower(function_id: str) -> bool:
        """Held to its line it took another time than the original, and not
        held it took the original's: the line leaves out something that
        decides only how long the function takes, in a state the watching
        did not have."""
        took = record["functions"].get(function_id, {}).get("clock")
        return took == "differs" and not observed.get(function_id, {}).get("cycles", {}).get("different")

    # ---- Hold every function to its line. One that does not hold, where
    # the function matched before, is tried on the next rung down. Not to
    # hold is to come out differently, or to take another time where the
    # function took the original's before it had a line:
    #   1. watched again, from every state the check uses;
    #   2. taking, besides what it was seen to read, what the export says it
    #      may read;
    #   3. the line the export alone gives, which errs on the side of taking
    #      and giving;
    #   4. no line.
    before = {function_id: observed.get(function_id, {}).get("verdict") for function_id in liftable}
    rung: dict[str, int] = {}
    for _ in range(6):
        code, said = _quietly(lift, ["build", str(folder), "--check-only", "--trials", str(args.trials),
                                     "--seed", str(args.seed)] + (["--rom", str(rom)] if args.rom else []) +
                              (["--jobs", str(args.jobs)] if args.jobs else []))
        source.write_text(folder / source.TOOL / "signatures.log", said)
        if code == 2:
            raise lift.LiftError(f"the check of the signatures did not run:\n{said[-3000:]}")
        record = source.load_record(folder, lift)
        failing = [function_id for function_id in liftable
                   if function_id in mine and function_id in lines and before[function_id] == "match" and
                   (record["functions"].get(function_id, {}).get("verdict") != "match" or slower(function_id))]
        if not failing:
            break
        again = [function_id for function_id in failing if function_id not in rung]
        if again:
            observed.update(observe(again, 0, "observed-again.json"))
        for function_id in failing:
            rung[function_id] = rung.get(function_id, 0) + 1
            new = (proposed(function_id) if rung[function_id] == 1 else
                   proposed(function_id, cautious=True) if rung[function_id] == 2 else
                   fallback(function_id) if rung[function_id] == 3 else None)
            if new is None:
                rung[function_id] = 4
                lines.pop(function_id, None)
            else:
                lines[function_id] = new.line(function_id, name_of(function_id))
            mine[function_id] = source.digest(lines[function_id]) if function_id in lines else NO_LINE
        write()
    else:
        raise lift.LiftError("functions still fail under their signatures after six rounds: see "
                             f"{folder / source.TOOL / 'signatures.log'}")
    cautious = sorted(function_id for function_id, at in rung.items() if at == 2)
    widened = sorted(function_id for function_id, at in rung.items() if at == 3)
    dropped = sorted(function_id for function_id, at in rung.items() if at == 4)

    signatures = read(folder, lift)
    masks = write_masks(folder, signatures)
    held = [function_id for function_id in liftable if function_id in signatures]
    record = source.load_record(folder, lift)
    matching = sum(1 for function_id in held if record["functions"].get(function_id, {}).get("verdict") == "match")
    listed = sum(1 for function_id in held if after.get(function_id, EVERYTHING) != EVERYTHING and
                 function_id not in as_played)
    by_play = sum(1 for function_id in held if function_id in as_played)
    left_over = sum(1 for function_id in held
                    if EVERYTHING & ~(signatures[function_id][0].gives | signatures[function_id][0].keeps))
    print(f"{len(held)} of {len(liftable)} functions have a signature in {folder / FILE}; "
          f"{matching} of them match the ROM's code held to it")
    print(f"  {listed} have every way in listed, so what they give is what is read after them")
    if played:
        print(f"  {by_play} more have a way in nobody can list, and give what is read where a played game saw "
              f"them return to ({len(caller_files)} files of callers): their lines say [as played]")
    print(f"  the other {len(held) - listed - by_play} give everything they change")
    print(f"  {left_over} leave a register or a flag undefined")
    if rung:
        print(f"  {len(rung)} did not hold as first watched, and were watched again from every state")
    if cautious:
        print(f"  {len(cautious)} of those take what the export says they may read as well: a register is read on "
              "a path too rare to be watched")
    if widened:
        print(f"  {len(widened)} did not hold as watched and have the line the export alone gives: "
              + ", ".join(source.reference(item) for item in sorted(widened)[:8]) + (" ..." if len(widened) > 8 else ""))
    dropped += none_held
    if dropped:
        print(f"  {len(dropped)} hold to no line the tool can write and have none: "
              + ", ".join(source.reference(item) for item in sorted(dropped)[:8]) + (" ..." if len(dropped) > 8 else ""))
    if kept_by_hand:
        print(f"  {kept_by_hand} lines somebody changed were kept as they are")
    if taken_out:
        print(f"  {taken_out} functions somebody took the line of out have none (--restore BB:AAAA)")
    print(f"the game takes them with: --lift-signatures \"{masks}\"")
    return 0
