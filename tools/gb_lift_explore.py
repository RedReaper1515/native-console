#!/usr/bin/env python3
"""A generated game exploring itself, for the evidence a played session gives.

`gb_lift.py edges --evidence` takes what a run of the game recorded: the
jumps its code does not spell out and the addresses calls arrived at. What a
run records is what was played, and nobody has the hours to play every corner
of every game. This plays it instead, many copies at once, without knowing
anything about the game.

It is a search by selection. Each run starts from a moment an earlier run
reached (a save state) and presses buttons at random from there. Its last
moment is kept, and can be a parent in turn, when the run found something no
run had: code arrived at for the first time, a jump going somewhere new, a
screen no run has ended on, or a value in a byte of the game's memory that
has only ever held a few. The screen is what gives a walk across a room a
direction: no new code runs on the way, and every step shows a different
background. The byte is what a door or a finished conversation shows in: the
number of the room, a flag set.

Parents are picked by how lately they were found and how seldom they have
been tried, more often when what they found was code. A screen that only
differs in its text is as new as one from another room, and a game shows a
great many of those, so tries are also counted by view, which is what a
screen looks like from a distance (the commonest tile in each twelfth of
it), and only the latest few moments of a view are kept. Picking a view
first and then a moment of it was tried, and was ten times slower through
a game's opening, where getting on means getting through text that all
looks the same from a distance.

A list of checkpoints, when there is one, is a walkthrough for it: conditions
on the game's memory in the order a player meets them. A moment is as far
along as the last checkpoint whose conditions hold in it, so one that has
walked back, or lost a fight and woken up two towns ago, counts as where it
is. Most runs then start from the moments furthest down the list, and the
first moment to pass each checkpoint is kept apart, to start from another
day. Past a checkpoint every screen is new again: the way back through a
room, with what was fetched, is ground the search has not covered.

Furthest is not always right. The furthest moments can be up the wrong
staircase, or in a blind corner a checkpoint's conditions happen to hold
in, or be a handful of states caught half way through a door, with the new
room's number and the old room's position. So of the runs that start from
the list, two in three start from the furthest checkpoint, two in three of
the rest from the one before it, and so on back: a bad checkpoint costs a
share of the runs, and the search goes on from behind it. (It used to wait
for the furthest moments to lead nowhere before going back. Counting what
was new from a distance, it gave up on a bridge it was half way across,
which looks the same all the way along; counting anything kept at all, it
never gave up on a corner where every run found one more page of a menu.)

Everything it uses is an option the generated game already has:
`--load-state-file`, `--save-state-file`, `--input`, `--evidence` and
`--dump-state`. What it writes is one merged evidence file, in the game's own
format, and the moments it kept.
"""
from __future__ import annotations

import concurrent.futures
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import gb_discover

SCHEMA = "gbrecomp.lift-explore"
GOALS_SCHEMA = "gbrecomp.lift-goals"
EVIDENCE_SCHEMA = "gbrt.evidence"
FRAME_CYCLES = 70224
RUN_FRAMES = (240, 480, 480, 960)   # a run from a kept moment: 4 to 16 seconds of the game
FIRST_FRAMES = 1500                 # a run from power-on: long enough to be past the logos
GIVE_UP_AFTER = 8                   # tries of a moment that found no code and led to nothing
CANDIDATES = 12                     # moments looked at for each pick of a parent
FEW_VALUES = 64                     # a byte of memory that has held more than this says nothing new
PER_VIEW = 400                      # plain moments kept of one view from a distance: a cave is one view
KEEP = 20000                        # moments kept in all: a quarter of a megabyte each
STEP_BACK = 1 / 3                   # of the runs from a checkpoint's moments, the share that goes to the one before
WRAM_BYTES = 0x8000


class ExploreError(Exception):
    pass


def input_script(rng: random.Random, base_cycles: int, frames: int) -> str:
    """Buttons at random for one run, in the game's `--input` form. Each run
    leans one way, mostly walking or mostly pressing A: a menu wants one and
    a corridor the other, and a run that does both gets through neither."""
    events = []
    walking = rng.choice((0.25, 0.45, 0.7))
    at = 4
    while at < frames - 8:
        roll = rng.random()
        if roll < walking:
            button, hold = rng.choice("UDLR"), rng.randint(6, 48)
        elif roll < walking + 0.60 * (1 - walking):
            button, hold = "A", rng.randint(3, 6)
        elif roll < walking + 0.75 * (1 - walking):
            button, hold = "B", rng.randint(3, 6)
        elif roll < walking + 0.82 * (1 - walking):
            button, hold = "S", rng.randint(3, 6)
        elif roll < walking + 0.84 * (1 - walking):
            button, hold = "T", rng.randint(3, 6)
        else:
            button, hold = None, rng.randint(4, 20)
        if button:
            events.append(f"c{base_cycles + at * FRAME_CYCLES}:{button}:{hold * FRAME_CYCLES}")
        at += hold + rng.randint(2, 6)
    return ",".join(events)


def look_at(state: bytes) -> tuple[str, str, bytes]:
    """What a save state shows and holds: a name for the screen (the
    background tiles in view, and the window's where it is shown), a name
    for the same seen from a distance (the commonest tile of each block of
    five by six), and the game's own memory, work RAM of every bank and then
    high RAM. Tile numbers, not pixels, so that an animated tile and a moving
    sprite do not make a screen new."""
    try:
        view = gb_discover.Savestate(state)
    except gb_discover.SavestateError as error:
        raise ExploreError(f"the game wrote a save state this cannot read: {error}") from error
    data = view.data
    vram = data[view.vram_off:view.vram_off + 0x2000]
    lcdc, scy, scx, wy, wx = (view.io(0xFF40), view.io(0xFF42), view.io(0xFF43), view.io(0xFF4A),
                              view.io(0xFF4B))
    tiles = bytearray()
    far = bytearray()
    if lcdc & 0x80:
        base = 0x1C00 if lcdc & 0x08 else 0x1800
        rows = []
        for row in range(18):
            line = base + ((scy // 8 + row) & 31) * 32
            rows.append(bytes(vram[line + ((scx // 8 + column) & 31)] for column in range(20)))
        tiles += b"".join(rows)
        for top in range(0, 18, 6):
            for left in range(0, 20, 5):
                block = b"".join(row[left:left + 5] for row in rows[top:top + 6])
                far.append(max(set(block), key=lambda tile: (block.count(tile), -tile)))
        if lcdc & 0x20 and wy < 144 and wx < 167:
            window = 0x1C00 if lcdc & 0x40 else 0x1800
            tiles += bytes([wy // 8, wx // 8]) + vram[window:window + 32 * 4]
            far += bytes([wy // 48, wx // 40])
    memory = bytes(data[view.wram_off:view.vram_off]) + bytes(data[view.hram_off:view.io_off])
    return (hashlib.blake2b(bytes(tiles), digest_size=8).hexdigest(),
            hashlib.blake2b(bytes(far), digest_size=6).hexdigest(), memory)


def transfer_key(transfer: dict) -> tuple:
    source = transfer.get("from") or {}
    return (transfer["kind"], transfer["bank"], transfer["at"], transfer["target_bank"], transfer["target"],
            transfer["calls"], transfer["returns_to"], source.get("bank"), source.get("site"),
            source.get("entered_bank"), source.get("entered"))


# ---- checkpoints ---------------------------------------------------------------

class Goals:
    """Checkpoints in the order a player meets them: each a name and
    conditions on bytes of the game's memory that all have to hold.

        {"schema": "gbrecomp.lift-goals", "version": 1, "goals": [
            {"name": "the second room", "when": [{"address": "D35E", "equals": 12}]},
            {"name": "the first badge", "when": [{"address": "D356", "bit": 0}]}]}

    An address is in work RAM (C000 to DFFF; `"bank"` for the upper half on
    a Game Boy Color, 1 if not given) or high RAM (FF80 to FFFE). A
    condition is `equals`, `not`, `at_least`, `at_most`, or `bit` (set)."""

    def __init__(self, path: Path):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise ExploreError(f"cannot read the checkpoints in {path}: {error}") from error
        if document.get("schema") != GOALS_SCHEMA or document.get("version") != 1:
            raise ExploreError(f"{path} is not a version 1 {GOALS_SCHEMA} file")
        self.names: list[str] = []
        self.tests: list[list[tuple[int, str, int]]] = []
        for goal in document["goals"]:
            tests = []
            for condition in goal["when"]:
                address = condition["address"]
                address = int(address, 16) if isinstance(address, str) else address
                if 0xC000 <= address < 0xD000:
                    offset = address - 0xC000
                elif 0xD000 <= address < 0xE000:
                    offset = condition.get("bank", 1) * 0x1000 + address - 0xD000
                elif 0xFF80 <= address < 0xFFFF:
                    offset = WRAM_BYTES + address - 0xFF80
                else:
                    raise ExploreError(f"checkpoint '{goal['name']}': {address:04X} is not in work RAM or high RAM")
                kinds = [kind for kind in ("equals", "not", "at_least", "at_most", "bit") if kind in condition]
                if len(kinds) != 1:
                    raise ExploreError(f"checkpoint '{goal['name']}': a condition is one of equals, not, "
                                       f"at_least, at_most and bit")
                tests.append((offset, kinds[0], int(condition[kinds[0]])))
            self.names.append(goal["name"])
            self.tests.append(tests)

    def passed(self, memory: bytes) -> int:
        """How far down the list this memory is: the number of the last
        checkpoint whose conditions hold, counting from 1; 0 for none."""
        for number in range(len(self.tests), 0, -1):
            for offset, kind, value in self.tests[number - 1]:
                held = memory[offset]
                if not ((kind == "equals" and held == value) or (kind == "not" and held != value) or
                        (kind == "at_least" and held >= value) or (kind == "at_most" and held <= value) or
                        (kind == "bit" and held >> value & 1)):
                    break
            else:
                return number
        return 0


# ---- one run -------------------------------------------------------------------

def play(exe: Path, folder: Path, parent_state: str | None, base_cycles: int, frames: int,
         seed: int | None, hold: dict[int, int] | None = None) -> dict:
    """One run of the game, with buttons pressed unless `seed` is None.
    Returns what it recorded and where it ended, or {"failed": why}. `hold`
    is bytes of the game's memory to set before it starts."""
    folder.mkdir(parents=True, exist_ok=True)
    if parent_state and hold:
        try:
            held = gb_discover.Savestate.load(parent_state)
            for address, value in hold.items():
                held.write(address, value)
            held.save(folder / "start.state")
        except (OSError, gb_discover.SavestateError) as error:
            return {"failed": f"its starting state could not be written: {error}", "folder": folder}
        parent_state = str(folder / "start.state")
    saves = folder / "saves"
    saves.mkdir(exist_ok=True)
    command = [str(exe), "--headless", "--no-audio", "--limit-frames", str(frames), "--save-dir", str(saves),
               "--evidence", str(folder / "evidence.json"), "--save-state-file", str(folder / "end.state"),
               "--dump-state", str(folder / "end.json")]
    if seed is not None:
        command += ["--input", input_script(random.Random(seed), base_cycles, frames)]
    if parent_state:
        command += ["--load-state-file", parent_state]
    # The machine is somebody's desk: let what they are doing go first.
    flags = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=120 + frames // 30,
                                creationflags=flags)
    except subprocess.TimeoutExpired:
        return {"failed": "it did not finish", "folder": folder}
    if result.returncode:
        return {"failed": (result.stderr or result.stdout)[-400:] or f"exit {result.returncode}", "folder": folder}
    try:
        evidence = json.loads((folder / "evidence.json").read_text(encoding="utf-8"))
        # The count that never wraps: it is the one `--input` is timed by.
        # The other starts again every seventeen minutes of the game, and a
        # line of moments is soon longer than that.
        cycles = json.loads((folder / "end.json").read_text(encoding="utf-8"))["total_cycles"]
        screen, view, memory = look_at((folder / "end.state").read_bytes())
    except (OSError, ValueError, KeyError) as error:
        return {"failed": f"its files could not be read: {error}", "folder": folder}
    if evidence.get("schema") != EVIDENCE_SCHEMA or evidence.get("version") != 1:
        return {"failed": "it wrote evidence of a kind this does not know", "folder": folder}
    return {"folder": folder, "evidence": evidence, "cycles": cycles, "screen": screen, "view": view,
            "memory": memory, "frames": frames}


# ---- the search ----------------------------------------------------------------

class Exploration:
    """What has been found so far, and the moments worth going on from."""

    def __init__(self, work: Path, seed: int, goals: Goals | None = None, watch_screens: bool = True,
                 watch_memory: bool = True, keep: int = KEEP):
        self.work = work
        self.room = max(keep, 100)
        self.rng = random.Random(seed)
        self.goals = goals
        self.watch_screens = watch_screens
        self.watch_memory = watch_memory
        self.rom: dict | None = None
        self.entered: set[tuple[int, int]] = set()
        self.transfers: dict[tuple, dict] = {}
        self.jumps: set[tuple] = set()
        self.screens: set[str] = set()
        self.held: list[set[int] | None] = []   # for each byte of memory, the values it has held, while few
        self.moments: list[dict] = []
        self.tried: dict[str, int] = {}     # view from a distance -> runs started from a moment of it
        self.views: dict[str, list[dict]] = {}  # view from a distance -> its moments, oldest first
        self.levels: dict[int, list[str]] = {}  # checkpoints passed -> the views there, in the order first seen
        self.given: list[str] = []          # hashes of the states somebody handed in
        self.passed: dict[str, str] = {}    # checkpoint number -> the state of the first moment past it
        self.running: dict[int, int] = {}   # moment -> runs from it that are not back yet
        self.front_runs = 0                 # runs from the furthest moments since the last report...
        self.fell_back: dict[int, int] = {}  # ...and where the ones that ended further back did
        self.runs = 0
        self.frames = 0
        self.dropped = 0
        self.failed = 0
        self.last_failure = ""
        self.gave_up = 0
        self.next_id = 1

    # ---- keeping it between sessions ----------------------------------------

    def load(self) -> bool:
        index = self.work / "explore.json"
        if not index.is_file():
            return False
        try:
            document = json.loads(index.read_text(encoding="utf-8"))
            evidence = json.loads((self.work / "evidence.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise ExploreError(f"cannot read the exploration in {self.work}: {error}") from error
        if document.get("schema") != SCHEMA or document.get("version") != 1:
            raise ExploreError(f"{index} is not a version 1 {SCHEMA} file")
        self.rom = evidence["rom"]
        self.entered = {tuple(pair) for pair in evidence["entered"]}
        self.transfers = {transfer_key(transfer): transfer for transfer in evidence["transfers"]}
        self.jumps = {key[:5] for key in self.transfers}
        self.screens = set(document["screens"])
        self.held = [set(values) if values is not None else None for values in document["held"]]
        self.moments = [moment for moment in document["moments"]
                        if moment["state"] is None or (self.work / moment["state"]).is_file()]
        for moment in self.moments:
            self.file(moment)
        self.tried = document["tried"]
        self.given = document["given"]
        self.passed = document["passed"]
        self.runs, self.frames = document["runs"], document["frames"]
        self.dropped, self.gave_up = evidence.get("dropped", 0), document["gave_up"]
        self.next_id = 1 + max((moment["id"] for moment in self.moments), default=0)
        return True

    def save(self) -> None:
        if self.rom is None:
            return
        transfers = sorted(self.transfers.values(), key=lambda t: (
            t["bank"], t["at"], t["target_bank"], t["target"], t["kind"], t["returns_to"],
            json.dumps(t.get("from"), sort_keys=True)))
        evidence = {"schema": EVIDENCE_SCHEMA, "version": 1, "rom": self.rom, "frames": self.frames,
                    "dropped": self.dropped, "transfers": transfers,
                    "entered": [list(pair) for pair in sorted(self.entered)]}
        document = {"schema": SCHEMA, "version": 1, "runs": self.runs, "frames": self.frames,
                    "gave_up": self.gave_up, "given": self.given, "passed": self.passed,
                    "screens": sorted(self.screens), "tried": self.tried,
                    "held": [sorted(values) if values is not None else None for values in self.held],
                    "moments": self.moments}
        for name, content in (("evidence.json", evidence), ("explore.json", document)):
            partial = self.work / (name + ".part")
            partial.write_text(json.dumps(content), encoding="utf-8")
            os.replace(partial, self.work / name)

    # ---- parents --------------------------------------------------------------

    def furthest(self) -> int:
        return max(self.levels, default=0)

    def falling_back(self) -> tuple[int, int] | None:
        """Since this was last asked: if more than a third of the runs from
        the furthest moments ended at one earlier checkpoint, that
        checkpoint and the share, in percent. It is what a room missing
        from the list looks like: everything that walks into it counts as
        back where the last flag was set."""
        runs, fell = self.front_runs, self.fell_back
        self.front_runs, self.fell_back = 0, {}
        if runs < 50 or not fell:
            return None
        where = max(fell, key=fell.get)
        return (where, fell[where] * 100 // runs) if fell[where] * 3 > runs else None

    def file(self, moment: dict) -> list[dict]:
        """Put a moment with the others of its view. Returns them."""
        alike = self.views.get(moment["view"])
        if alike is None:
            alike = self.views[moment["view"]] = []
            self.levels.setdefault(moment["passed"], []).append(moment["view"])
        alike.append(moment)
        return alike

    def keep(self, state: Path | None, result: dict | None, found: int, changed: int,
             parent: dict | None) -> dict:
        passed = result["passed"] if result is not None else 0
        moment = {"id": self.next_id, "state": None, "cycles": result["cycles"] if result else 0,
                  "view": f"{passed}:{result['view']}" if result else "", "found": found, "changed": changed,
                  "passed": passed, "chosen": 0, "kept": 0,
                  "generation": (parent["generation"] + 1) if parent else 0}
        self.next_id += 1
        if state is not None:
            target = self.work / "states" / f"{moment['id']:07d}.state"
            target.parent.mkdir(exist_ok=True)
            shutil.move(str(state), str(target))
            moment["state"] = f"states/{target.name}"
            for number in range(1, passed + 1):
                if str(number) in self.passed:
                    continue
                # The first moment past a checkpoint, kept apart from the
                # search. One that is past a later checkpoint is past the
                # earlier ones too, whether or not a run ever ended on them.
                name = re.sub(r"[^A-Za-z0-9]+", "-", self.goals.names[number - 1]).strip("-").lower()
                kept = self.work / "checkpoints" / f"{number:03d}-{name}.state"
                kept.parent.mkdir(exist_ok=True)
                shutil.copyfile(target, kept)
                self.passed[str(number)] = f"checkpoints/{kept.name}"
        self.moments.append(moment)
        alike = self.file(moment)
        if len(alike) > PER_VIEW:
            # The moments of one view are much alike: the latest few are
            # enough, and the ones that found code.
            for older in alike:
                if (older["state"] and not older["found"] and not older["changed"] and
                        not self.running.get(older["id"])):
                    self.forget(older)
                    break
        if len(self.moments) > self.room:
            self.make_room()
        return moment

    def make_room(self) -> None:
        """Too many moments are kept: let a tenth go. The ones least far along
        first, among those the ones that found no code, and the oldest of
        them. What they found is in the record all the same."""
        furthest = self.furthest()
        spare = [moment for moment in self.moments
                 if moment["state"] and moment["passed"] < furthest and not self.running.get(moment["id"])]
        if len(spare) < self.room // 10:
            spare = [moment for moment in self.moments
                     if moment["state"] and not self.running.get(moment["id"])][:-self.room // 10]
        spare.sort(key=lambda moment: (moment["passed"], bool(moment["found"]), moment["id"]))
        for moment in spare[:self.room // 10]:
            self.forget(moment)

    def forget(self, moment: dict) -> None:
        self.moments.remove(moment)
        alike = self.views[moment["view"]]
        alike.remove(moment)
        if not alike:
            del self.views[moment["view"]]
            there = self.levels[moment["passed"]]
            there.remove(moment["view"])
            if not there:
                del self.levels[moment["passed"]]
        (self.work / moment["state"]).unlink(missing_ok=True)
        self.running.pop(moment["id"], None)

    def choose(self) -> dict:
        """A parent for the next run. With checkpoints, three candidates in
        four are from the moments of one checkpoint: the furthest two times
        in three, the one before it two times in three of the rest, and so
        on back down the list. Half the candidates come from the latest
        fifth of their pool: the later a moment was found, the further along
        it is likely to be. Among them, code found counts for more than a
        screen, and every try already made, of the moment and of any moment
        that looks like it, counts against it."""
        levels = sorted(self.levels, reverse=True) if self.goals else []
        sorted_already: dict[int, list[dict]] = {}

        def of_a_checkpoint() -> list[dict]:
            index = 0
            while index + 1 < len(levels) and self.rng.random() < STEP_BACK:
                index += 1
            level = levels[index]
            if level not in sorted_already:
                sorted_already[level] = sorted((moment for view in self.levels[level] for moment in self.views[view]),
                                               key=lambda moment: moment["id"])
            return sorted_already[level]

        candidates = []
        for pick in range(CANDIDATES):
            pool = of_a_checkpoint() if levels and pick % 4 < 3 else self.moments
            count = len(pool)
            recent = max(1, count // 5)
            candidates.append(pool[self.rng.randrange(count - recent, count) if pick % 8 < 4
                                   else self.rng.randrange(count)])
        weights = [(1.0 + 4.0 * min(moment["found"], 10) + moment["changed"]) /
                   ((1 + moment["chosen"]) * (1 + self.tried.get(moment["view"], 0))) ** 0.5
                   for moment in candidates]
        moment = self.rng.choices(candidates, weights)[0]
        moment["chosen"] += 1
        self.tried[moment["view"]] = self.tried.get(moment["view"], 0) + 1
        self.running[moment["id"]] = self.running.get(moment["id"], 0) + 1
        return moment

    def back(self, parent: dict) -> None:
        """A run from `parent` has returned. One that found no code itself and
        has led nowhere in several tries is not worth its place."""
        if parent["id"] not in self.running:
            return
        self.running[parent["id"]] -= 1
        if (parent["state"] is None or parent["found"] or parent["changed"] or parent["kept"] or
                parent["chosen"] < GIVE_UP_AFTER or self.running[parent["id"]] or parent not in self.moments):
            return
        self.forget(parent)
        self.gave_up += 1

    # ---- what a run found -------------------------------------------------------

    def new_in_memory(self, memory: bytes) -> int:
        """How many bytes of memory hold a value they never held, among those
        that have held few. A counter or a random number runs through its
        values at once and is left out from then on; a room's number does not."""
        if not self.held:
            self.held = [set() for _ in memory]
        fresh = 0
        held = self.held
        for address, value in enumerate(memory):
            values = held[address]
            if values is None or value in values:
                continue
            if len(values) >= FEW_VALUES:
                held[address] = None
                continue
            values.add(value)
            fresh += 1
        return fresh

    def absorb(self, result: dict) -> tuple[int, bool, int]:
        """Add a run's record to what is known. Returns how much of it was
        new code (arrivals and jumps), whether its last screen was new, and
        how many bytes of memory held something new."""
        evidence = result["evidence"]
        if self.rom is None:
            self.rom = evidence["rom"]
        elif evidence["rom"] != self.rom:
            raise ExploreError("a run recorded another ROM than the exploration in this folder is of")
        self.frames += result["frames"]
        self.dropped += evidence.get("dropped", 0)
        arrived = {tuple(pair) for pair in evidence["entered"]}
        new_code = len(arrived - self.entered)
        self.entered |= arrived
        for transfer in evidence["transfers"]:
            key = transfer_key(transfer)
            if key in self.transfers:
                self.transfers[key]["count"] += transfer["count"]
            else:
                self.transfers[key] = transfer
            if key[:5] not in self.jumps:
                self.jumps.add(key[:5])
                new_code += 1
        # How far along it is, and its screen as one seen from there.
        result["passed"] = self.goals.passed(result["memory"]) if self.goals else 0
        screen = f"{result['passed']}:{result['screen']}"
        new_screen = self.watch_screens and screen not in self.screens
        self.screens.add(screen)
        first = not self.held
        new_memory = self.new_in_memory(result["memory"]) if self.watch_memory else 0
        # The first run fills every byte in: that is not news.
        return new_code, new_screen, 0 if first else new_memory

    def take(self, result: dict, parent: dict) -> None:
        self.runs += 1
        if "failed" in result:
            self.failed += 1
            self.last_failure = result["failed"]
        else:
            new_code, new_screen, new_memory = self.absorb(result)
            further = result["passed"] > parent["passed"]
            if self.goals and parent["passed"] == self.furthest():
                self.front_runs += 1
                if result["passed"] < parent["passed"]:
                    self.fell_back[result["passed"]] = self.fell_back.get(result["passed"], 0) + 1
            somewhere = bool(new_code or new_screen or new_memory or further)
            if somewhere:
                self.keep(result["folder"] / "end.state", result, new_code, min(new_memory, 4), parent)
                parent["kept"] += 1
        self.back(parent)
        shutil.rmtree(result["folder"], ignore_errors=True)

    def give(self, exe: Path, state: Path, number: int) -> bool:
        """A moment somebody else made: a state saved while playing. One frame
        run from it says what time it is there and that the game takes it."""
        if not state.is_file():
            raise ExploreError(f"no save state at {state}")
        digest = hashlib.sha256(state.read_bytes()).hexdigest()
        if digest in self.given:
            return False
        result = play(exe, self.work / "runs" / f"given-{number}", str(state.resolve()), 0, 1, None)
        if "failed" in result:
            raise ExploreError(f"the game could not start from {state}: {result['failed']}")
        new_code = self.absorb(result)[0]
        self.keep(result["folder"] / "end.state", result, max(1, new_code), 0, None)
        self.given.append(digest)
        shutil.rmtree(result["folder"], ignore_errors=True)
        return True


def explore(exe: Path, work: Path, minutes: float | None, runs: int | None, workers: int, seed: int,
            states: list[Path], fresh: bool, report, goals: Goals | None = None, watch_screens: bool = True,
            watch_memory: bool = True, keep: int = KEEP, power_on: bool = True,
            hold: dict[int, int] | None = None) -> Exploration:
    """Explore until `minutes` have passed or `runs` more runs were made,
    going on from what an earlier session left in `work` unless `fresh`."""
    exe = exe.resolve()
    if not exe.is_file():
        raise ExploreError(f"no game at {exe}")
    work = work.resolve()
    if fresh:
        for name in ("explore.json", "evidence.json"):
            (work / name).unlink(missing_ok=True)
        for folder in ("states", "checkpoints"):
            shutil.rmtree(work / folder, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(work / "runs", ignore_errors=True)
    if sys.platform == "win32":
        # A copy that crashes must not wait for somebody to dismiss a dialog.
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)

    exploration = Exploration(work, seed, goals, watch_screens, watch_memory, keep)
    if not exploration.load() and power_on:
        exploration.keep(None, None, 1, 0, None)   # power-on
    for number, state in enumerate(states):
        exploration.give(exe, state, number)
    if not exploration.moments:
        raise ExploreError("nowhere to start from: no save state was given, and power-on was left out")

    started = time.monotonic()
    deadline = started + minutes * 60 if minutes is not None else None
    launched = 0
    next_report = started + 30

    def more() -> bool:
        if deadline is not None and time.monotonic() >= deadline:
            return False
        return runs is None or launched < runs

    try:
        with concurrent.futures.ThreadPoolExecutor(workers) as pool:
            pending: dict = {}
            while more() or pending:
                while len(pending) < workers and more():
                    parent = exploration.choose()
                    launched += 1
                    frames = exploration.rng.choice(RUN_FRAMES) if parent["state"] else FIRST_FRAMES
                    state = str(work / parent["state"]) if parent["state"] else None
                    future = pool.submit(play, exe, work / "runs" / f"{launched:08d}", state, parent["cycles"],
                                         frames, exploration.rng.randrange(1 << 30), hold)
                    pending[future] = parent
                done, _ = concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)
                for future in done:
                    exploration.take(future.result(), pending.pop(future))
                if exploration.failed >= 20 and exploration.failed == exploration.runs:
                    raise ExploreError("every run of the game failed: " + exploration.last_failure)
                if time.monotonic() >= next_report:
                    next_report += 30
                    report(exploration, time.monotonic() - started)
                    exploration.save()
    finally:
        exploration.save()
        shutil.rmtree(work / "runs", ignore_errors=True)
    return exploration
