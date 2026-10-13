#!/usr/bin/env python3
"""From a Game Boy ROM to a folder to mod it in, in one command.

    gb_workspace.py ROM DIR [what else there is] [-- OPTIONS for gbrecomp]

Every step of DATA_FILES.md is a command of its own (`gbrecomp`, the game,
`gb_data.py`, `run_record.py`), and a tree with everything in it takes a dozen
of them in the right order, with the same options said again each time. This
runs them. It prints each command before it runs it, so that the steps can be
learnt from it and done by hand, and it leaves in DIR:

    README.txt        what is there, what was found, what was not and why,
                      and the two commands to build and to play an edit
    tree/             the ROM's data as files to edit (the data tree)
    game/             the game, generated and built: game/build/<prefix>
    export.json       the analysis the tree was split from
    game.annotations  what the analysis was told is data
    records/          what the game did with its data, a record a piece of play
    harvests/, called/, watch.gbup    what its unpacking routine made
    game.gb, game.gbup                the image and the pack the tree builds to
    old/tree-N        the tree as it was before it was split again
    workspace.json    what DIR was made from; workspace.log, every command's output

What there is besides the ROM, each of which turns its steps on:

    --states FOLDER      save states of the game: play is recorded from each
    --run FOLDER         a recorded run (RUN_RECORDS.md): it is replayed in pieces
    --sym FILE           a symbol file: `names --sym` calls the files by it
    --charmap FILE       what letter each byte of text is: `text` writes the strings
    --unpackers FILE     which routine unpacks: a harvest over the same play,
                         `unpacked`, and `unpack` over the tables it declares
    --annotations FILE   lines of one's own for the analysis
    -- OPTIONS           what gbrecomp is given: the game and the export are
                         always made with the same ones

With no play given it records the game from switching on, twice, once with
nothing pressed and once with buttons pressed at random. That is always
recorded; states and a run add to it.

The steps, and why there are two passes. The game is generated and built,
and its ROM described (`--lift-export`) and split into a tree. The game is
then played with `--data-use --data-use-tiles`, and `uses` writes what it did
into the tree. `annotations` writes what it showed to be data, and the game
and the export are made again, told that: sprites and tile sets that the
analysis had decoded stretches of as instructions are whole files then. The
tree of the new export is split with `--keep`, which takes over everything
the old tree holds, and the old one is kept under old/. After that come
`names`, so that every file is made under the name it keeps, and the views:
`tiles` (the pictures the game was seen to draw), `maps`, `text` or the sheet
of `glyphs`, and `unpacked`; then `names` again for what was unpacked and for
the list, and `build`, which has to make the original ROM of a tree nobody
has edited.

It can be run again on the same DIR, with the same command or with more:
what is not said again is as it was. A step is done again only if what it
was made from has changed, so a run with nothing new does nothing, and one
that stopped goes on from the step that failed. The tree may have been
edited in between. No step writes over an edit: a tree is split again only
with `--keep`, `text` leaves a file of text that was edited as it is, and
`tiles`, `maps` and `unpacked` do so by themselves.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
TOOLS = Path(__file__).resolve().parent
GB_DATA = TOOLS / "gb_data.py"
RUN_RECORD = TOOLS / "run_record.py"
sys.path.insert(0, str(TOOLS))
import gb_data_text  # noqa: E402  (which instructions read text, as `text` finds them)
import gb_data_unpack  # noqa: E402  (whether a save state has the sound hardware in it)
import gb_lift_explore  # noqa: E402  (buttons at random, as the explorer presses them)

SCHEMA = "gbrecomp.workspace"
FRAMES = 900            # recorded from each save state: fifteen seconds
BOOT_FRAMES = 1800      # and from switching on: past the logos of most games
MOST_FRAMES = 12000     # more than this and the buttons no longer fit on a command line
RUN_STATE_FRAMES = 3600  # a recorded run is replayed in pieces of a minute
ECHO = 12               # lines of a tool's own report that are printed; the log has them all
# What gbrecomp is given by this tool, or takes only where it generates and not where it describes.
OWN_OPTIONS = {"-o": "the folder is DIR/game", "--output": "the folder is DIR/game",
               "--lift-export": "the export is DIR/export.json", "--output-prefix": "say --prefix NAME",
               "--lift": "lifted source goes into a game generated by hand (LIFTING.md)",
               "--native-patch": "it is taken only where a game is generated, and the export has to be made "
                                 "with the same options",
               "--android": "this builds the desktop game"}
ANNOTATION = re.compile(r"^\s*(data|function|label)\s+([0-9A-Fa-f]+):([0-9A-Fa-f]+)(?:\s+(\S+))?")
USES = ("tiles", "tile_map", "oam", "sound", "palette", "bank", "jump", "pointer")  # as gb_data.py has them


class WorkspaceError(Exception):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as file:
        for chunk in iter(lambda: file.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def digest_of(value) -> str:
    """One number for anything a step is made from."""
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode("utf-8")).hexdigest()


def tree_digest(folder: Path, suffixes: tuple[str, ...]) -> str:
    """One number for the files of a folder that have one of these endings."""
    found = sorted(path for path in Path(folder).rglob("*") if path.is_file() and
                   (path.suffix in suffixes or path.name in suffixes))
    return digest_of([[path.relative_to(folder).as_posix(), sha256_file(path)] for path in found])


def shown(command: list, whole: bool = False) -> str:
    """A command as it would be typed here: Python by its name, and a path under the folder
    this was started in from there. `whole` keeps every path whole, for a command that is
    written down to be typed somewhere else."""
    here = Path.cwd()
    words = []
    for word in map(str, command):
        if word == sys.executable:
            word = "python" if os.name == "nt" else "python3"
        elif not whole and os.path.isabs(word):
            try:
                word = str(Path(word).relative_to(here))
            except ValueError:
                pass
        words.append(word)
    return subprocess.list2cmdline(words) if os.name == "nt" else shlex.join(words)


def counted(count: int, one: str, many: str | None = None) -> str:
    return f"{count:,} {one if count == 1 else (many or one + 's')}"


def annotation_lines(text: str) -> list[str]:
    """What an annotations file says to the analysis: its lines without comments. The file
    `annotations` writes begins by counting the records, which says nothing to gbrecomp."""
    lines = []
    for line in text.splitlines():
        body = line.split(";", 1)[0].strip()
        if body and not body.startswith("#") and not body.startswith("//"):
            lines.append(" ".join(body.split()))
    return lines


def told(seen: str, own: str, own_name: str) -> str:
    """One annotations file of what the records show and of the lines somebody wrote.
    gbrecomp takes one file and refuses two lines that share a byte, so what the records
    show is cut around every line of one's own: those are the ones somebody vouches for."""
    taken: dict[int, list[tuple[int, int]]] = {}
    for line in own.splitlines():
        found = ANNOTATION.match(line.split(";", 1)[0])
        if not found:
            continue
        size = 1
        if found.group(1) == "data":
            if not (found.group(4) or "").isdigit():
                continue                # gbrecomp says what is wrong with the line
            size = int(found.group(4))
        address = int(found.group(3), 16)
        taken.setdefault(int(found.group(2), 16), []).append((address, address + size))
    out = []
    for line in seen.splitlines():
        body, _, comment = line.partition(";")
        found = ANNOTATION.match(body)
        if not found or found.group(1) != "data" or not (found.group(4) or "").isdigit():
            out.append(line)
            continue
        bank, start = int(found.group(2), 16), int(found.group(3), 16)
        left = [(start, start + int(found.group(4)))]
        for first, last in sorted(taken.get(bank, ())):
            left = [part for low, high in left for part in ((low, min(high, first)), (max(low, last), high))
                    if part[0] < part[1]]
        if left == [(start, start + int(found.group(4)))]:
            out.append(line)
        else:
            out += [f"data {bank:02X}:{low:04X} {high - low} ;{comment}" for low, high in left]
    if own.strip():
        out += ["", f"# The lines of {own_name}, which what the records show is cut around:", *own.splitlines()]
    return "\n".join(out) + "\n"


class Workspace:
    def __init__(self, rom: Path, directory: Path, given: dict, options: list[str] | None):
        self.rom = rom.resolve()
        self.dir = directory.resolve()
        if not self.rom.is_file():
            raise WorkspaceError(f"there is no ROM at {rom}")
        self.rom_sha = sha256_file(self.rom)
        self.state_path = self.dir / "workspace.json"
        self.state = self.load_state()
        self.inputs = self.remembered(given, options)
        self.gbrecomp = Path(self.inputs["gbrecomp"])
        self.tree = self.dir / "tree"
        self.project = self.dir / "game"
        self.export = self.dir / "export.json"
        self.records = self.dir / "records"
        self.harvests = self.dir / "harvests"
        self.called = self.dir / "called"
        self.run_states = self.dir / "run-states"
        self.scratch = self.dir / "scratch"
        self.seen_annotations = self.dir / "seen.annotations"
        self.annotations = self.dir / "game.annotations"
        self.pack = self.dir / "watch.gbup"
        self.image = self.dir / "game.gb"
        self.log_path = self.dir / "workspace.log"
        self.lock = threading.Lock()
        self.ran: list[str] = []
        self.held: list[str] = []
        self.notes: list[str] = []          # what was not found, and why: the README's second half
        self.runtime_sha = self.runtime_digest()
        self.gbrecomp_sha = sha256_file(self.gbrecomp) if self.gbrecomp.is_file() else ""

    # ---- what the folder was made from -------------------------------------------------------

    def load_state(self) -> dict:
        fresh = {"schema": SCHEMA, "version": 1, "rom": {"sha256": self.rom_sha}, "inputs": {}, "done": {},
                 "pieces": {}, "harvested": {}, "cycles": {}, "copied": {}, "found": {}}
        if not self.state_path.exists():
            if self.dir.exists() and any(self.dir.iterdir()):
                raise WorkspaceError(f"{self.dir} is not empty and is no workspace of this tool (it has no "
                                     f"workspace.json): name a folder to make, or an empty one")
            return fresh
        try:
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise WorkspaceError(f"cannot read {self.state_path}: {error}") from error
        if not isinstance(state, dict) or state.get("schema") != SCHEMA or state.get("version") != 1:
            raise WorkspaceError(f"{self.state_path} is not a version 1 {SCHEMA} file")
        if state["rom"]["sha256"] != self.rom_sha:
            raise WorkspaceError(f"{self.dir} was made from another ROM (SHA-256 {state['rom']['sha256'][:16]}..., "
                                 f"and {self.rom.name} is {self.rom_sha[:16]}...): a folder is of one ROM")
        for key, value in fresh.items():
            state.setdefault(key, value)
        return state

    def save_state(self) -> None:
        self.dir.mkdir(parents=True, exist_ok=True)
        new = self.state_path.with_suffix(".json.new")
        new.write_text(json.dumps(self.state, indent=1, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
        new.replace(self.state_path)

    def remembered(self, given: dict, options: list[str] | None) -> dict:
        """What was given now, and for everything else what was given before."""
        before = self.state["inputs"]
        inputs = {"states": None, "run": None, "sym": [], "charmap": None, "unpackers": None, "annotations": None,
                  "unpack_state": None, "frames": FRAMES, "boot_frames": BOOT_FRAMES, "rounds": 1, "prefix": "game",
                  "cmake_args": [], "options": [], "seen_names": True,
                  "gbrecomp": str(ROOT / "build" / "bin" / ("gbrecomp.exe" if os.name == "nt" else "gbrecomp"))}
        inputs.update({key: value for key, value in before.items() if key in inputs})
        if options is not None:
            inputs["options"], own = self.options_given(options)
            if own:
                given = {**given, "annotations": own}
        for key, value in given.items():
            if value is None or value == []:
                continue
            if key in ("states", "run", "charmap", "unpackers", "annotations", "unpack_state", "gbrecomp"):
                value = str(Path(value).resolve())
            elif key == "sym":
                value = [str(Path(path).resolve()) for path in value]
            inputs[key] = value
        for key, kind in (("states", "folder"), ("run", "folder"), ("charmap", "file"), ("unpackers", "file"),
                          ("annotations", "file"), ("unpack_state", "file")):
            path = inputs[key]
            if path and not (Path(path).is_dir() if kind == "folder" else Path(path).is_file()):
                raise WorkspaceError(f"there is no {kind} at {path}" + ("" if given.get(key) else
                                     f" (--{key.replace('_', '-')}, as it was given before)"))
        for path in inputs["sym"]:
            if not Path(path).is_file():
                raise WorkspaceError(f"there is no symbol file at {path}")
        if inputs["states"] and not list(Path(inputs["states"]).glob("*.state")):
            raise WorkspaceError(f"{inputs['states']} holds no save state (*.state)")
        if inputs["run"] and not list(Path(inputs["run"]).glob("segment-*.run")):
            raise WorkspaceError(f"{inputs['run']} holds no segment-NNNN.run: it is not a recorded run")
        for key in ("frames", "boot_frames"):
            if not 1 <= inputs[key] <= MOST_FRAMES:
                raise WorkspaceError(f"--{key.replace('_', '-')} is 1 to {MOST_FRAMES:,}: for longer play, record "
                                     f"a run (RUN_RECORDS.md) and give it with --run")
        if not 1 <= inputs["rounds"] <= 16:
            raise WorkspaceError("--rounds is 1 to 16")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", inputs["prefix"]):
            raise WorkspaceError("--prefix is a name a C identifier can have")
        return inputs

    def options_given(self, options: list[str]) -> tuple[list[str], str | None]:
        """gbrecomp's options as they will be given every time: a file named in them by its
        whole path, and an annotations file taken out, since it goes in with what the records
        show."""
        kept: list[str] = []
        own = None
        index = 0
        while index < len(options):
            word = options[index]
            index += 1
            name = word.split("=", 1)[0]
            if name == "--annotations":
                if index >= len(options):
                    raise WorkspaceError("--annotations needs a file")
                own = options[index]
                index += 1
            elif name in OWN_OPTIONS or name.startswith("--android"):
                raise WorkspaceError(f"{name} is not for gbrecomp here: "
                                     f"{OWN_OPTIONS.get(name, OWN_OPTIONS['--android'])}")
            else:
                kept.append(str(Path(word).resolve()) if not word.startswith("-") and Path(word).exists() else word)
        return kept, own

    def runtime_digest(self) -> str:
        """The runtime a generated game is built from: its sources are copied into the project."""
        options = self.inputs["options"]
        folder = Path(options[options.index("--runtime-dir") + 1]) if "--runtime-dir" in options[:-1] else \
            Path(os.environ.get("GBRECOMP_RUNTIME_DIR") or ROOT / "runtime")
        return tree_digest(folder, (".c", ".cpp", ".h", "CMakeLists.txt")) if folder.is_dir() else ""

    # ---- saying and running ------------------------------------------------------------------

    def log(self, text: str) -> None:
        with self.lock:
            with self.log_path.open("a", encoding="utf-8", newline="\n") as file:
                file.write(text if text.endswith("\n") else text + "\n")

    def say(self, text: str = "") -> None:
        print(text, flush=True)
        self.log(text)

    def run(self, command: list, accept: tuple[int, ...] = (0,), echo: bool = False, quiet: bool = False,
            timeout: float | None = None) -> subprocess.CompletedProcess:
        """Run one command, printed first, with everything it said in the log."""
        command = [str(word) for word in command]
        if quiet:
            self.log(f"$ {shown(command)}")
        else:
            self.say(f"$ {shown(command)}")
        started = time.monotonic()
        environment = {**os.environ, "PYTHONIOENCODING": "utf-8:backslashreplace"}
        try:
            done = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                  timeout=timeout, env=environment)
        except subprocess.TimeoutExpired as error:
            self.log(f"  did not finish in {timeout:.0f} seconds")
            raise WorkspaceError(f"{Path(command[0]).name} did not finish in {timeout:.0f} seconds") from error
        except OSError as error:
            raise WorkspaceError(f"cannot run {command[0]}: {error}") from error
        took = time.monotonic() - started
        said = (done.stdout + done.stderr).rstrip()
        self.log("\n".join("  " + line for line in said.splitlines()) + f"\n  exit code {done.returncode}, "
                 f"{took:.1f} s")
        if done.returncode not in accept:
            tail = "\n".join("  " + line for line in said.splitlines()[-15:])
            raise WorkspaceError(f"{Path(command[0]).name if command[0] != sys.executable else Path(command[1]).name}"
                                 f" stopped with exit code {done.returncode}:\n{tail}")
        if echo and not quiet:
            lines = done.stdout.rstrip().splitlines()
            for line in lines[:ECHO]:
                print("  " + line)
            if len(lines) > ECHO:
                print(f"  ... and {len(lines) - ECHO} more lines, in {self.log_path.name}")
        if took >= 5 and not quiet:
            print(f"  ({took:.0f} s)", flush=True)
        return done

    def tool(self, *arguments, accept: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess:
        return self.run([sys.executable, GB_DATA, *arguments], accept=accept, echo=True)

    def step(self, name: str, made_from: dict, outputs: list[Path], what: str, action) -> bool:
        """Do a step unless it is done: made from the same things, with what it made still there."""
        before = self.state["done"].get(name)
        gone = [path for path in outputs if not path.exists()]
        if before == made_from and not gone:
            self.held.append(name)
            return False
        if before is None:
            why = ""
        elif before != made_from:
            changed = sorted(key for key in set(before) | set(made_from) if before.get(key) != made_from.get(key))
            why = f" (again: {', '.join(changed)} changed)"
        else:
            why = f" (again: {gone[0].name} is gone)"
        self.say(f"\n# {what}{why}")
        self.state["done"].pop(name, None)
        self.save_state()
        action()
        self.state["done"][name] = made_from
        self.save_state()
        self.ran.append(name)
        return True

    # ---- the game and the tree, made with the same options -----------------------------------

    def told_text(self) -> str | None:
        """What game.annotations has to say: what the records showed, and the lines somebody
        gave. None while there is neither."""
        own = Path(self.inputs["annotations"]) if self.inputs["annotations"] else None
        if own is None and not self.seen_annotations.exists():
            return None
        seen = self.seen_annotations.read_text(encoding="utf-8") if self.seen_annotations.exists() else ""
        return told(seen, own.read_text(encoding="utf-8") if own else "", str(own))

    def told_digest(self) -> str | None:
        text = self.told_text()
        return digest_of(annotation_lines(text)) if text is not None else None

    def write_told(self) -> None:
        text = self.told_text()
        if text is not None and (not self.annotations.exists() or
                                 self.annotations.read_text(encoding="utf-8") != text):
            self.annotations.write_text(text, encoding="utf-8", newline="\n")

    def analysis(self) -> list[str]:
        """What gbrecomp is given to decide what is code: for the game and for the export alike."""
        self.write_told()
        return [str(self.rom), *self.inputs["options"],
                *(["--annotations", str(self.annotations)] if self.told_text() is not None else [])]

    def game_inputs(self) -> tuple[dict, dict, dict]:
        base = {"rom": self.rom_sha, "gbrecomp": self.gbrecomp_sha, "annotations": self.told_digest(),
                "options": [sha256_file(Path(word)) if Path(word).is_file() and not word.startswith("-") else word
                            for word in self.inputs["options"]]}
        generate = {**base, "runtime": self.runtime_sha, "prefix": self.inputs["prefix"]}
        return base, generate, {"generated": digest_of(generate), "cmake_args": self.inputs["cmake_args"]}

    def executable(self) -> Path:
        build = self.project / "build"
        name = self.inputs["prefix"]
        return next((path for path in (build / f"{name}.exe", build / name) if path.is_file()), build / name)

    def game_is_built(self) -> bool:
        _, generate, build = self.game_inputs()
        return self.state["done"].get("generate") == generate and self.state["done"].get("build") == build and \
            self.executable().is_file()

    def game_and_tree(self) -> None:
        base, generate, build = self.game_inputs()
        self.step("generate", generate, [self.project / "CMakeLists.txt"],
                  "The game as C" + (", told what the records show to be data" if self.seen_annotations.exists()
                                     else ""),
                  lambda: self.run([self.gbrecomp, *self.analysis(), "-o", self.project, "--output-prefix",
                                    self.inputs["prefix"]]))

        def compile_game() -> None:
            self.run(["cmake", "-G", "Ninja", "-S", self.project, "-B", self.project / "build",
                      "-DCMAKE_BUILD_TYPE=Release", *self.inputs["cmake_args"]])
            self.run(["ninja", "-C", self.project / "build"])
            if not self.executable().is_file():
                raise WorkspaceError(f"the build left no {self.executable()}")

        # No output is named: a build that was cut short has no stamp, and the game is looked for after it.
        if not self.executable().is_file():
            self.state["done"].pop("build", None)
        self.step("build", build, [], "Built", compile_game)
        self.step("export", base, [self.export],
                  "The ROM described, with the options the game was generated with",
                  lambda: self.run([self.gbrecomp, *self.analysis(), "--lift-export", self.export]))
        if self.step("split", {"export": sha256_file(self.export)}, [self.tree / "manifest.json"],
                     "Apart: a file for each region of the ROM" if not (self.tree / "manifest.json").exists() else
                     "Apart again, since the export is another: a new tree, with what the old one holds",
                     self.split):
            # Whatever was made in the tree before is to be made in this one: every later step again.
            for name in [name for name in self.state["done"] if name not in ("generate", "build", "export", "split",
                                                                               "annotations")]:
                del self.state["done"][name]
            self.save_state()

    def split(self) -> None:
        if not (self.tree / "manifest.json").exists():
            if self.tree.exists() and any(self.tree.iterdir()):
                raise WorkspaceError(f"{self.tree} is there and is no data tree: move it away")
            self.tool("split", self.export, "--rom", self.rom, "-o", self.tree)
            return
        # The tree may have been worked in. A new export's tree is always another folder, which
        # takes over what the old one holds; the old one is then kept, and the new one takes its place.
        fresh = self.dir / "tree.next"
        if fresh.exists():
            shutil.rmtree(fresh)        # a split that was cut short: this tool's own, and made again now
        self.tool("split", self.export, "--rom", self.rom, "-o", fresh, "--keep", self.tree,
                  *(["--lose-edits-in-code"] if self.lose_edits_in_code else []))
        old = self.dir / "old"
        old.mkdir(exist_ok=True)
        number = 1 + max([int(path.name[5:]) for path in old.glob("tree-*") if path.name[5:].isdigit()], default=0)
        kept = old / f"tree-{number}"
        try:
            self.tree.rename(kept)
        except OSError as error:
            raise WorkspaceError(f"cannot move {self.tree} to {kept} ({error}): close what has its files open, "
                                 f"and run this again") from error
        try:
            fresh.rename(self.tree)
        except OSError as error:
            kept.rename(self.tree)
            raise WorkspaceError(f"cannot move {fresh} to {self.tree} ({error}): the tree is as it was; run this "
                                 f"again") from error
        self.say(f"  the tree as it was is kept in {kept}")

    # ---- play --------------------------------------------------------------------------------

    def pieces(self) -> list[dict]:
        """The runs of the game to record: from switching on, and from each save state."""
        boot = self.inputs["boot_frames"]
        found = [{"name": "boot", "state": None, "frames": boot, "seed": None},
                 {"name": "boot-buttons", "state": None, "frames": boot, "seed": "boot"}]
        if self.inputs["states"]:
            for path in sorted(Path(self.inputs["states"]).glob("*.state")):
                for turn in range(1, self.inputs["rounds"] + 1):
                    found.append({"name": f"state-{path.stem}" + (f"-r{turn}" if turn > 1 else ""), "state": path,
                                  "frames": self.inputs["frames"],
                                  "seed": path.name + (f"#{turn}" if turn > 1 else "")})
        for piece in found:
            piece["sha"] = sha256_file(piece["state"]) if piece["state"] else None
            # A state made by a game run with --no-audio loads only in a game run that way too.
            piece["quiet"] = bool(piece["state"]) and not gb_data_unpack.made_with_sound(piece["state"])
            piece["made_from"] = {"rom": self.rom_sha, "runtime": self.runtime_sha, "frames": piece["frames"],
                                  "seed": piece["seed"], "state": piece["sha"]}
        return found

    def segments(self) -> list[dict]:
        """The sittings of a recorded run, each replayed in pieces from states of its own."""
        if not self.inputs["run"]:
            return []
        found = []
        for path in sorted(Path(self.inputs["run"]).glob("segment-*.run")):
            save = path.with_suffix(".sav")
            found.append({"name": path.stem, "number": int(path.stem.split("-")[1]),
                          "made_from": {"rom": self.rom_sha, "runtime": self.runtime_sha,
                                        "run": sha256_file(path), "save": sha256_file(save) if save.exists() else None,
                                        "state_frames": RUN_STATE_FRAMES}})
        return found

    def to_record(self) -> tuple[list[dict], list[dict]]:
        def stale(piece: dict, made: str) -> bool:
            before = self.state["pieces"].get(piece["name"])
            if before is None or before["made_from"] != piece["made_from"]:
                return True
            if before.get("failed"):
                return self.retry
            return made == "record" and not (self.records / f"{piece['name']}.json").exists()

        return [piece for piece in self.pieces() if stale(piece, "record")], \
            [segment for segment in self.segments() if stale(segment, "run")]

    def play_command(self, piece: dict, more: list) -> list[str] | str:
        """The game's command line for a piece of play, or why there is none."""
        saves = self.scratch / "saves" / piece["name"]
        saves.mkdir(parents=True, exist_ok=True)
        command = [str(self.executable()), "--headless", "--save-dir", str(saves)]
        if piece["state"]:
            command += ["--load-state-file", str(piece["state"])]
        if piece["quiet"]:
            command.append("--no-audio")
        if piece["seed"] is not None:
            # Buttons go by the machine's count of cycles, and a save state is somewhere along it.
            cycles = 0
            if piece["state"]:
                cycles = self.state["cycles"].get(piece["sha"])
                if cycles is None:
                    at = self.scratch / "at" / f"{piece['name']}.json"
                    at.parent.mkdir(parents=True, exist_ok=True)
                    self.log(f"$ {shown([*command, '--limit-frames', '1', '--dump-state', at])}")
                    try:
                        first = subprocess.run([*command, "--limit-frames", "1", "--dump-state", str(at)],
                                               capture_output=True, text=True, errors="replace",
                                               timeout=self.timeout)
                        cycles = json.loads(at.read_text(encoding="utf-8"))["total_cycles"]
                    except subprocess.TimeoutExpired:
                        return f"did not finish one frame in {self.timeout:.0f} seconds"
                    except (OSError, ValueError, KeyError):
                        return "the game did not load the state: " + \
                            ((first.stdout + first.stderr).strip().splitlines() or ["it said nothing"])[-1]
                    with self.lock:
                        self.state["cycles"][piece["sha"]] = cycles
            script = gb_lift_explore.input_script(random.Random(piece["seed"]), cycles, piece["frames"])
            if script:
                command += ["--input", script]
        return [*command, "--limit-frames", str(piece["frames"]), *map(str, more)]

    def play(self, pieces: list[dict], more, output) -> dict[str, str | None]:
        """Run the game once for each piece, several at a time: None where it wrote its file,
        and otherwise why it did not."""
        # One of them is printed before any is run: the last that can be, which has the most to
        # it. The log has them all.
        printed = None
        for piece in pieces[:-4:-1]:
            command = self.play_command(piece, more(piece))
            if isinstance(command, str):
                continue
            printed = piece["name"]
            brief = list(command)
            if "--input" in brief and len(brief[brief.index("--input") + 1]) > 120:
                at = brief.index("--input") + 1
                brief[at] = ",".join(brief[at].split(",")[:3]) + ",..."
                self.log(f"$ {shown(command)}")
                self.say(f"$ {shown(brief)}\n  (and so on for {piece['frames']:,} frames: {self.log_path.name} "
                         f"has every press)")
            else:
                self.say(f"$ {shown(command)}")
            if len(pieces) > 1:
                self.say(f"  and {len(pieces) - 1} more like it, {min(self.workers, len(pieces))} at a time")
            break

        def one(piece: dict) -> tuple[str, str | None]:
            target = output(piece)
            command = self.play_command(piece, more(piece))
            if isinstance(command, str):
                self.log(f"{piece['name']}: {command}")
                return piece["name"], command
            if piece["name"] != printed:
                self.log(f"$ {shown(command)}")
            if target.exists():
                target.unlink()
            try:
                done = subprocess.run(command, capture_output=True, text=True, errors="replace",
                                      timeout=self.timeout)
            except subprocess.TimeoutExpired:
                return piece["name"], (f"did not finish {piece['frames']} frames in {self.timeout:.0f} seconds (a "
                                       f"game whose screen is off never ends a frame)")
            except OSError as error:
                return piece["name"], f"cannot run the game: {error}"
            said = (done.stdout + done.stderr).strip().splitlines()
            if done.returncode or not target.exists():
                self.log(f"{piece['name']}: exit code {done.returncode}\n"
                         + "\n".join("  " + line for line in said[-8:]))
                return piece["name"], f"the game stopped with exit code {done.returncode}: " + \
                    (said[-1] if said else "it said nothing")
            return piece["name"], None

        with concurrent.futures.ThreadPoolExecutor(self.workers) as pool:
            return dict(pool.map(one, pieces))

    def sweep(self, segments: list[dict], out: Path, *more) -> None:
        """A recorded run played again in pieces, each with these options (run_record.py)."""
        numbers = ",".join(str(segment["number"]) for segment in segments)
        done = self.run([sys.executable, RUN_RECORD, "sweep", self.inputs["run"], "--game", self.executable(),
                         "--states", self.run_states, "--out", out, "--segments", numbers, "--jobs", self.workers,
                         "--", *more], accept=(0, 1), echo=True)
        if done.returncode:
            self.state["found"]["run"] = "Some pieces of the recorded run did not replay: workspace.log has " \
                                         "which. What the others showed is in the tree."

    def record(self) -> None:
        pieces, segments = self.to_record()
        if not pieces and not segments:
            self.held.append("record")
            return
        self.records.mkdir(parents=True, exist_ok=True)
        if pieces:
            self.say(f"\n# Played, with what the game does with its data recorded: {self.play_said(pieces)}")
            results = self.play(pieces, lambda piece: ["--data-use", self.records / f"{piece['name']}.json",
                                                       "--data-use-tiles"],
                                lambda piece: self.records / f"{piece['name']}.json")
            for piece in pieces:
                self.state["pieces"][piece["name"]] = {"made_from": piece["made_from"],
                                                       "failed": results[piece["name"]]}
            self.save_state()
            failed = [name for name, why in results.items() if why]
            self.say(f"  {counted(len(pieces) - len(failed), 'record')} in {self.records}"
                     + (f"; {len(failed)} not made: {failed[0]}: {results[failed[0]]}" if failed else ""))
        if segments:
            self.say(f"\n# The recorded run played again, with save states along the way, and then in pieces "
                     f"from them with the recorder on ({counted(len(segments), 'sitting')})")
            numbers = ",".join(str(segment["number"]) for segment in segments)
            self.state["found"].pop("run", None)
            verified = self.run([sys.executable, RUN_RECORD, "verify", self.inputs["run"], "--game",
                                 self.executable(), "--segments", numbers, "--states", self.run_states,
                                 "--state-frames", RUN_STATE_FRAMES, "--jobs", self.workers], accept=(0, 1),
                                echo=True)
            self.sweep(segments, self.records, "--data-use", "{out}.json", "--data-use-tiles")
            if verified.returncode:
                self.state["found"]["run"] = "The recorded run did not replay to its end with this game: " \
                                             "workspace.log has where it left the record. It was recorded as " \
                                             "far as it went."
            for segment in segments:
                self.state["pieces"][segment["name"]] = {"made_from": segment["made_from"], "failed": None}
            self.save_state()
        self.ran.append("record")
        if not list(self.records.glob("*.json")):
            raise WorkspaceError("no piece of play left a record: workspace.log has what the game said")

    def play_said(self, pieces: list[dict]) -> str:
        boots = sum(1 for piece in pieces if not piece["state"])
        states = len(pieces) - boots
        parts = []
        if boots:
            parts.append(f"{counted(boots, 'run')} of {self.inputs['boot_frames']:,} frames from switching on")
        if states:
            parts.append(f"{counted(states, 'run')} of {self.inputs['frames']:,} frames from save states, buttons "
                         f"pressed at random")
        return " and ".join(parts)

    def records_digest(self) -> str:
        return tree_digest(self.records, (".json",))

    # ---- the views ---------------------------------------------------------------------------

    def manifest(self) -> dict:
        try:
            return json.loads((self.tree / "manifest.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise WorkspaceError(f"cannot read {self.tree / 'manifest.json'}: {error}") from error

    def bring(self, key: str, target: Path, empty=lambda path: False) -> None:
        """A file somebody gave, into the tree: while the tree has none, an empty one, or the
        one that was put there. One that was changed in the tree since is the tree's own."""
        if not self.inputs[key]:
            return
        source = Path(self.inputs[key])
        new = sha256_file(source)
        there = sha256_file(target) if target.exists() else None
        if there == new:
            self.state["copied"][key] = new
        elif there is None or empty(target) or there == self.state["copied"].get(key):
            shutil.copyfile(source, target)
            self.state["copied"][key] = new
            self.say(f"\n# {source} into the tree, as {target.name}")
        else:
            self.say(f"\n# {target} is not {source} and not what was copied from it before: it was written in "
                     f"the tree, and stays")
        self.save_state()

    @staticmethod
    def no_letters(charmap: Path) -> bool:
        try:
            return not any(json.loads(charmap.read_text(encoding="utf-8")).get("letters", {}).values())
        except (OSError, ValueError, AttributeError):
            return False                # `text` says what is wrong with it

    def glyph_state(self, readers: set) -> Path | None:
        """A save state from the end of the piece of play that read the most text: the
        letters are most likely in video memory there."""
        best, most = None, 0
        for path in sorted(self.records.glob("*.json")):
            try:
                record = json.loads(path.read_text(encoding="utf-8"))
                read = sum(entry["size"] for entry in record["ranges"] if tuple(entry["read_at"][:2]) in readers)
            except (OSError, ValueError, KeyError, TypeError):
                continue
            if read > most:
                best, most = path.stem, read
        if best is None:
            return None
        if best.startswith("segment-"):
            states = sorted(path for path in self.run_states.glob(f"{best[:12]}-f*.state"))
            names = [path.stem for path in states]
            if best not in names:
                return None
            return states[min(names.index(best) + 1, len(states) - 1)]
        piece = next((piece for piece in self.pieces() if piece["name"] == best), None)
        if piece is None:
            return None                 # a record of play that is no longer given
        state = self.dir / "glyphs.state"
        failed = self.play([piece], lambda piece: ["--save-state-file", state], lambda piece: state)[best]
        if failed:
            self.say(f"  no save state for the sheet: {failed}")
            return None
        return state

    def views(self, uses: str) -> None:
        manifest = self.manifest()
        if any("tile_runs" in region for region in manifest["regions"]):
            self.step("tiles", {"uses": uses}, [], "A picture of every run of tiles the game was seen to draw",
                      lambda: self.tool("tiles", self.tree))
        else:
            self.notes.append("No picture of tiles: no byte of the ROM reached the tile area of video memory as "
                              "it is in any record. A game that unpacks all its pictures has none to find this "
                              "way (see the packed data, below); otherwise more play shows more.")
        self.step("maps", {"uses": uses}, [], "The tile maps the game was seen to show, as tile numbers",
                  lambda: self.tool("maps", self.tree))
        charmap = self.tree / "charmap.json"
        self.bring("charmap", charmap, self.no_letters)
        readers = set(gb_data_text.text_readers(manifest))
        if not readers:
            self.notes.append("No text: no instruction was seen to read bytes one after another and send most of "
                              "them to a tile map as they are, which is what a routine that prints does. Play "
                              "in which the game prints something shows one.")
        elif charmap.exists() and not self.no_letters(charmap):
            self.step("text", {"uses": uses, "charmap": sha256_file(charmap), "read": True}, [],
                      "The strings the game was seen to print, and the ones like them in what it was not seen "
                      "to use, as text",
                      lambda: self.tool("text", self.tree, "--keep-edited", "--read"))
        else:
            def glyphs() -> None:
                state = self.glyph_state(readers)
                if state is None:
                    raise WorkspaceError("there is no save state to draw the game's letters from")
                self.tool("glyphs", self.tree, state)

            self.step("glyphs", {"uses": uses}, [self.tree / "glyphs.png"],
                      "No charmap says what letter a byte is: the game's letters as a sheet, to write one from",
                      glyphs)
            self.notes.append("No text yet: nothing says what letter a byte is. tree/glyphs.png shows the tile "
                              "each byte draws, with the bytes the strings use dark. Write the letters into "
                              "tree/charmap.json (DATA_FILES.md, Text as text) or give a charmap with --charmap, "
                              "and run this again: `text` then writes the strings.")

    def unpacked(self) -> None:
        declaration = self.tree / "unpackers.json"
        self.bring("unpackers", declaration)
        if not declaration.exists():
            self.notes.append("Nothing of what the game unpacks: no unpackers.json says which routine unpacks. "
                              "DATA_FILES.md, Finding the routine that unpacks, has how one is proposed from a "
                              "recording; give the declaration with --unpackers, or put it in the tree, and run "
                              "this again.")
            return
        try:
            declared = json.loads(declaration.read_text(encoding="utf-8"))["unpackers"]
            tabled = [unpacker["name"] for unpacker in declared if unpacker.get("tables")]
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise WorkspaceError(f"cannot read {declaration}: {error}") from error
        said = sha256_file(declaration)
        self.step("watch-pack", {"declaration": said}, [self.pack],
                  "A pack that says which routine to watch, and replaces nothing",
                  lambda: self.tool("watch-pack", self.tree, "-o", self.pack))
        self.harvest(said)
        folders = [self.harvests] if list(self.harvests.glob("*.json")) else []
        for name in tabled:
            out = self.called / name
            state = self.inputs["unpack_state"] or \
                (str(sorted(Path(self.inputs["states"]).glob("*.state"))[0]) if self.inputs["states"] else None)

            def unpack(name: str = name, out: Path = out, state: str | None = state) -> None:
                done = self.tool("unpack", self.tree, "--game", self.executable(), "--tables", "--unpacker", name,
                                 "-o", out, "--workers", self.workers, *(["--state", state] if state else []),
                                 accept=(0, 1))
                if done.returncode:
                    self.say("  some streams of the tables were not unpacked; the others are taken")

            self.step(f"unpack {name}", {"declaration": said, "rom": self.rom_sha, "runtime": self.runtime_sha,
                                         "state": sha256_file(Path(state)) if state else None},
                      [out / "run" / "report.json"],
                      f"The streams of the tables {name} declares, unpacked by the game with nobody playing", unpack)
            if list(out.glob("*.json")):
                folders.append(out)
        if not folders:
            self.notes.append("Nothing of what the game unpacks: its routine was not entered in any piece of "
                              "play, and the declaration has no tables of streams.")
            return
        self.step("unpacked", {"declaration": said, "tree": self.state["done"]["split"],
                               "harvests": [tree_digest(folder, (".json",)) for folder in folders]}, [],
                  "What the routine made, into the tree as files to edit",
                  lambda: self.tool("unpacked", self.tree, *folders))

    def harvest(self, declaration: str) -> None:
        """The same play again, with the pack: the game keeps what its routine makes."""
        def stale(piece: dict) -> bool:
            before = self.state["harvested"].get(piece["name"])
            return before is None or before["made_from"] != [piece["made_from"], declaration] or \
                (self.retry if before.get("failed") else
                 "number" not in piece and not (self.harvests / f"{piece['name']}.json").exists())

        pieces = [piece for piece in self.pieces() if stale(piece)]
        segments = [segment for segment in self.segments() if stale(segment)]
        if not pieces and not segments:
            self.held.append("harvest")
            return
        self.harvests.mkdir(parents=True, exist_ok=True)
        self.say("\n# The same play again with the pack: what the routine made of each stream it was given")
        if pieces:
            results = self.play(pieces, lambda piece: ["--unpacked", self.pack, "--unpacked-harvest",
                                                       self.harvests / f"{piece['name']}.json"],
                                lambda piece: self.harvests / f"{piece['name']}.json")
            for piece in pieces:
                self.state["harvested"][piece["name"]] = {"made_from": [piece["made_from"], declaration],
                                                          "failed": results[piece["name"]]}
            failed = [name for name, why in results.items() if why]
            self.say(f"  {counted(len(pieces) - len(failed), 'harvest')} in {self.harvests}"
                     + (f"; {len(failed)} not made: {failed[0]}: {results[failed[0]]}" if failed else ""))
        if segments:
            self.sweep(segments, self.harvests, "--unpacked", self.pack, "--unpacked-harvest", "{out}.json")
            for segment in segments:
                self.state["harvested"][segment["name"]] = {"made_from": [segment["made_from"], declaration],
                                                            "failed": None}
        self.save_state()
        self.ran.append("harvest")

    # ---- the whole of it ---------------------------------------------------------------------

    def environment(self) -> None:
        """Whether a game can be built here, said before anything is made."""
        if not self.gbrecomp.is_file():
            raise WorkspaceError(f"gbrecomp is not at {self.gbrecomp}: build it first (cmake -G Ninja -B build . "
                                 f"and ninja -C build), or say where it is with --gbrecomp")
        missing = [f"{name} is not on PATH" for name in ("cmake", "ninja") if not shutil.which(name)]
        if not os.environ.get("CC") and not any(shutil.which(name) for name in ("cl", "cc", "gcc", "clang")):
            missing.append("no C compiler is on PATH (cl, cc, gcc or clang)"
                           + (": on Windows, run this from a Developer Command Prompt, or after vcvars64.bat"
                              if os.name == "nt" else ""))
        if missing:
            raise WorkspaceError("a game cannot be built here: " + "; ".join(missing))

    def make(self, workers: int, timeout: float, retry: bool, lose_edits_in_code: bool) -> None:
        self.workers, self.timeout, self.retry, self.lose_edits_in_code = workers, timeout, retry, \
            lose_edits_in_code
        # A game is built if there is none that is made from what there is now, and may be built
        # again if anything is played: what play shows goes back to the analysis.
        pieces, segments = self.to_record()
        if not self.game_is_built() or pieces or segments:
            self.environment()
        self.dir.mkdir(parents=True, exist_ok=True)
        self.state["inputs"] = self.inputs
        self.save_state()
        self.log(f"\n=== {datetime.datetime.now().isoformat(timespec='seconds')}  {shown(sys.argv)}")
        self.game_and_tree()
        self.record()
        records = self.records_digest()
        self.step("uses", {"tree": self.state["done"]["split"], "records": records}, [],
                  "What the game did with each region, into the tree",
                  lambda: self.tool("uses", self.tree, self.records))
        self.step("annotations", {"records": records}, [self.seen_annotations],
                  "What the game showed to be data, for the analysis to be told",
                  lambda: self.tool("annotations", self.tree, self.records, "-o", self.seen_annotations))
        # The second pass: the game and the export again, told, and the tree split again.
        self.game_and_tree()
        uses = {"tree": self.state["done"]["split"], "records": records}
        self.step("uses", uses, [], "What the game did with each region, into the new tree",
                  lambda: self.tool("uses", self.tree, self.records))
        # Names before the views, so that each picture and file of text is made under the name
        # it keeps, and once more at the end, for what was unpacked since and for the list.
        names = ["names", self.tree, *[word for path in self.inputs["sym"] for word in ("--sym", path)],
                 "--seen" if self.inputs["seen_names"] else "--no-seen"]
        named = {"uses": digest_of(uses), "sym": [sha256_file(Path(path)) for path in self.inputs["sym"]],
                 "seen": self.inputs["seen_names"]}
        self.step("names", named, [],
                  "The files called by name: " + ("the names of the symbol file" if self.inputs["sym"] else
                                                  "no symbol file, so by what the game used each as"
                                                  if self.inputs["seen_names"] else
                                                  "no symbol file, and the addresses are to stay"),
                  lambda: self.tool(*names))
        self.views(digest_of(named))
        self.unpacked()
        views = digest_of({name: made for name, made in self.state["done"].items() if name not in ("list", "image")})
        self.step("list", {"views": views}, [self.dir / "names.txt"],
                  "Names for what was made since, and a list of everything the tree has",
                  lambda: self.tool(*names, "--list", self.dir / "names.txt"))
        self.step("image", {"views": views}, [self.image], "Together again", self.build_image)
        self.write_readme()

    def build_image(self) -> None:
        self.tool("build", self.tree, "-o", self.image)
        rom, image = self.rom.read_bytes(), self.image.read_bytes()
        differ = sum(1 for one, other in zip(rom, image) if one != other) + abs(len(rom) - len(image))
        self.state["found"]["image_differs"] = differ
        if differ:
            self.say(f"  {differ:,} bytes of it are not the ROM's: the tree has been edited")

    # ---- what was found ----------------------------------------------------------------------

    def summary(self) -> list[str]:
        manifest = self.manifest()
        regions = manifest["regions"]
        lines = []
        for kind, note in (("data", ""), ("code", ", which cannot be edited: the game's C was compiled from it")):
            of = [region for region in regions if region["kind"] == kind]
            lines.append(f"{counted(len(of), 'file')} of {kind}, {sum(region['size'] for region in of):,} "
                         f"bytes{note}")
        records = len(list(self.records.glob("*.json")))
        read = sum(seen["size"] for region in regions for seen in region.get("seen", ()))
        lines.append(f"{counted(records, 'record')} of the game played: it read {read:,} bytes of its data in them")
        if self.annotations.exists():
            data = [line.split() for line in annotation_lines(self.annotations.read_text(encoding="utf-8"))
                    if line.startswith("data ")]
            lines.append(f"{sum(int(words[2]) for words in data if len(words) > 2 and words[2].isdigit()):,} bytes "
                         f"in {counted(len(data), 'run')} that the analysis was told are data (game.annotations)")
        views = [view for region in regions for view in region.get("views", ())]
        tiles = [view for view in views if view["kind"] == "tiles"]
        if tiles:
            lines.append(f"{counted(len(tiles), 'picture')} of tiles, {sum(view['tiles'] for view in tiles):,} "
                         f"tiles: tree/tiles.png shows them all, and tree/tiles.txt lists them")
        maps = [view for view in views if view["kind"] == "map"]
        if maps:
            lines.append(f"{counted(len(maps), 'file')} of tile maps (.map.txt), "
                         f"{counted(sum(len(view['grids']) for view in maps), 'grid')}, each with a picture")
        text = [view for view in views if view["kind"] == "text"]
        if text:
            read = sum(len(view.get("read", ())) for view in text)
            lines.append(f"{counted(sum(len(view['strings']) for view in text), 'string')} in "
                         f"{counted(len(text), 'file')} of text (.text.txt)"
                         + (f": {read:,} of them nobody saw printed, and were found by reading the data for "
                            f"what the others are made of" if read else ""))
        things = manifest.get("unpacked", ())
        if things:
            streams = {(thing["unpacker"], thing["bank"], thing["address"]) for thing in things}
            played = self.streams_in([self.harvests])
            tabled = self.streams_in([path for path in self.called.glob("*") if path.is_dir()]) - played \
                if self.called.exists() else set()
            lines.append(f"{counted(len(things), 'thing')} the game unpacks, of {counted(len(streams), 'stream')}, "
                         f"under tree/unpacked: {len(played):,} of the streams were unpacked in play"
                         + (f", and {len(tabled):,} more are from the tables declared" if self.called.exists()
                            else ""))
        # A file that is called by a name no longer begins with its address; one called by what the
        # game used it as still does, and ends in the use.
        data = [region for region in regions if region["kind"] == "data"]
        by_name = sum(1 for region in data
                      if not Path(region["file"]).name.startswith(f"{region['address']:04X}_"))
        by_use = sum(1 for region in data if re.fullmatch(
            rf"{region['address']:04X}_({'|'.join(USES)})\.bin", Path(region["file"]).name))
        if by_name or by_use:
            said = [f"{by_name:,} by a name" if by_name else "",
                    f"{by_use:,} by what the game used most of it as (4146_tiles.bin)" if by_use else ""]
            lines.append(f"of the files of data, {' and '.join(part for part in said if part)}: names.txt lists "
                         f"everything the tree has, and tree/names.sym is where a name is given")
        differ = self.state["found"].get("image_differs")
        if differ is not None:
            lines.append("the tree builds to the original ROM, byte for byte" if not differ else
                         f"the tree is edited: the image it builds differs from the ROM in {differ:,} bytes")
        return lines

    @staticmethod
    def streams_in(folders: list[Path]) -> set[tuple[int, int]]:
        found = set()
        for folder in folders:
            for path in folder.glob("*.json"):
                try:
                    found.update((thing["bank"], thing["address"])
                                 for thing in json.loads(path.read_text(encoding="utf-8"))["made"])
                except (OSError, ValueError, KeyError, TypeError):
                    pass
        return found

    def write_readme(self) -> None:
        """README.txt: what is in the folder, what was found, what was not and why, and the
        commands that build and play an edit. Written again only when it would say something
        else."""
        manifest = self.manifest()
        game = self.executable()
        pack = self.image.with_suffix(".gbup")
        python = "python" if os.name == "nt" else "python3"
        failed = {name: piece["failed"] for name, piece in sorted(self.state["pieces"].items()) if piece["failed"]}
        notes = list(self.notes)
        if not self.inputs["states"] and not self.inputs["run"]:
            notes.insert(0, f"Nobody's play was given, so the records are of the game switched on and left for "
                            f"{self.inputs['boot_frames']:,} frames, once with nothing pressed and once with "
                            f"buttons pressed at random. That shows logos and a title screen and seldom more. "
                            f"Save states from further in (--states FOLDER) or a recorded run (--run FOLDER, "
                            f"RUN_RECORDS.md) show what the game does there, and everything else here is found "
                            f"from what the records show.")
        if self.state["found"].get("run"):
            notes.append(self.state["found"]["run"])
        quiet = sum(1 for piece in self.pieces() if piece["quiet"] and piece["name"] not in failed)
        if quiet:
            notes.append(f"{counted(quiet, 'run')} from save states had the game without its sound hardware "
                         f"(--no-audio): the states were made that way, and a state loads only in a game run as "
                         f"the one that made it. The sound registers read FF there, which a game can tell.")
        if failed:
            first = next(iter(failed))
            notes.append(f"{counted(len(failed), 'piece')} of play left no record ({first}: {failed[first]}"
                         + (f", and {len(failed) - 1} more: workspace.json has each" if len(failed) > 1 else "")
                         + "). --retry tries them again.")
        if not self.inputs["sym"]:
            notes.append("No names from a symbol file: --sym FILE brings in the labels of one, and a line in "
                         "tree/names.sym names anything.")
        notes.append("Data the game was not seen to use in any record is as the analysis guessed it: a sprite it "
                     "did not load has no picture, and may lie partly in files of code. Record more and run this "
                     "again.")
        notes.append("Not made here: free space for strings to grow into (`pointers`, `free`), and lifted "
                     "source. DATA_FILES.md and LIFTING.md have both.")
        here = [("tree/", "the ROM's data as files: edit these (DATA_FILES.md says what each kind is)"),
                ("game/", f"the game, generated and built: {game.relative_to(self.dir).as_posix()}"),
                ("export.json", "the analysis the tree was split from, made with the game's options"),
                ("game.annotations", "what the analysis was told is data"),
                ("records/", "what the game did with its data, a record for each piece of play"),
                ("harvests/", "what its unpacking routine made in each piece of play"),
                ("called/", "what it made of the streams of the tables declared, with nobody playing"),
                ("run-states/", "save states along the recorded run"),
                ("game.gb", "the image the tree builds to" + (", and game.gbup, the pack" if pack.exists() else "")),
                ("names.txt", "everything the tree has, with its address, its name and its file"),
                ("old/", "the tree as it was each time it was split again"),
                ("workspace.log", "every command that was run, with what it said"),
                ("workspace.json", "what this folder was made from: the tool's own")]
        lines = [f"{manifest['rom'].get('title') or self.rom.name}: a folder to mod it in",
                 f"made by tools/gb_workspace.py from {self.rom}", "", "WHAT IS HERE", ""]
        lines += [f"  {name:17s} {what}" for name, what in here if (self.dir / name).exists()]
        lines += ["", "WHAT WAS FOUND", ""]
        for line in self.summary():
            lines += self.wrapped(line, " ")
        lines += ["", "WHAT WAS NOT, AND WHY", ""]
        for note in notes:
            lines += self.wrapped(note)
        play = [game, "--rom-image", self.image] + (["--unpacked", pack] if (self.tree / "unpackers.json").exists()
                                                    else [])
        lines += ["", "AFTER AN EDIT", "",
                  "  what is edited:",
                  f"    {shown([python, GB_DATA, 'status', self.tree], whole=True)}",
                  "  build the tree:",
                  f"    {shown([python, GB_DATA, 'build', self.tree, '-o', self.image], whole=True)}",
                  "  start the game with it:",
                  f"    {shown(play, whole=True)}",
                  "",
                  "  (In PowerShell a command that begins with a quoted path takes & in front.)",
                  "", "TO ADD TO IT", "",
                  "  More play, a charmap, a symbol file or an unpackers declaration, and this again. What is",
                  "  not said again is as it was, nothing that is done is done again, and no edit is lost:",
                  f"    {shown([python, Path(__file__).resolve(), self.rom, self.dir], whole=True)}", ""]
        text = "\n".join(lines)
        readme = self.dir / "README.txt"
        if not readme.exists() or readme.read_text(encoding="utf-8") != text:
            readme.write_text(text, encoding="utf-8", newline="\n")

    @staticmethod
    def wrapped(text: str, first: str = "  -", width: int = 96) -> list[str]:
        """A paragraph of the README, broken into lines."""
        lines, line = [], first
        for word in text.split():
            if len(line) + 1 + len(word) > width and line != first:
                lines.append(line)
                line = "   "
            line += " " + word
        return lines + [line]


def main() -> int:
    arguments = sys.argv[1:]
    options = None
    if "--" in arguments:
        cut = arguments.index("--")
        arguments, options = arguments[:cut], arguments[cut + 1:]
    parser = argparse.ArgumentParser(
        description="From a Game Boy ROM to a folder to mod it in: the game generated and built, its data as a "
                    "tree of files with everything the tools find, and a README that says what is there.",
        epilog="After --, what gbrecomp is given for the game and for the export alike (--pointer-scan strict, "
               "--lift-edges FILE, ...). Run again on the same folder, what is not said again is as it was.")
    parser.add_argument("rom", type=Path, help="the ROM")
    parser.add_argument("directory", type=Path, help="the folder to make, or one this made before")
    parser.add_argument("--states", type=Path, metavar="FOLDER", help="save states of the game to record play from")
    parser.add_argument("--run", type=Path, metavar="FOLDER", help="a recorded run (--record-run) to replay")
    parser.add_argument("--sym", action="append", type=Path, metavar="FILE",
                        help="a symbol file to name the tree's files from; repeatable")
    parser.add_argument("--charmap", type=Path, metavar="FILE", help="what letter each byte of text is")
    parser.add_argument("--unpackers", type=Path, metavar="FILE", help="which routine of the game unpacks")
    parser.add_argument("--annotations", type=Path, metavar="FILE",
                        help="lines of one's own for the analysis, which go in with what the records show")
    parser.add_argument("--unpack-state", type=Path, metavar="FILE",
                        help="the save state the game unpacks the streams of declared tables in (the first of "
                             "--states unless given, and the game as it is switched on without)")
    parser.add_argument("--frames", type=int, help=f"frames recorded from each save state ({FRAMES})")
    parser.add_argument("--boot-frames", type=int, help=f"frames recorded from switching on ({BOOT_FRAMES})")
    parser.add_argument("--rounds", type=int, help="runs from each save state, each with other buttons (1)")
    parser.add_argument("--prefix", help="what the game's program is called (game)")
    parser.add_argument("--seen-names", action=argparse.BooleanOptionalAction, default=None,
                        help="call a data file with no name by what the game used it as (on unless turned off)")
    parser.add_argument("--gbrecomp", type=Path, help="the recompiler (build/bin/gbrecomp of this checkout)")
    parser.add_argument("--cmake-arg", action="append", dest="cmake_args", metavar="ARG",
                        help="given to cmake when the game is configured (-DSDL2_DIR=...); repeatable")
    parser.add_argument("--workers", type=int, default=4, help="copies of the game to run at once (4, at most 8)")
    parser.add_argument("--timeout", type=float, default=600, help="seconds one run of the game may take (600)")
    parser.add_argument("--retry", action="store_true", help="try again the pieces of play that left no record")
    parser.add_argument("--lose-edits-in-code", action="store_true",
                        help="when the tree is split again: go on without edits to bytes that are code now")
    args = parser.parse_args(arguments)
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="backslashreplace")
    given = {key: getattr(args, key) for key in ("states", "run", "sym", "charmap", "unpackers", "annotations",
                                                 "unpack_state", "frames", "boot_frames", "rounds", "prefix",
                                                 "seen_names", "gbrecomp", "cmake_args")}
    started = time.monotonic()
    workspace = None
    try:
        workspace = Workspace(args.rom, args.directory, given, options)
        workspace.make(max(1, min(8, args.workers)), args.timeout, args.retry, args.lose_edits_in_code)
    except (WorkspaceError, KeyboardInterrupt) as error:
        print(f"gb_workspace: {error if isinstance(error, WorkspaceError) else 'stopped'}", file=sys.stderr)
        if workspace is not None and workspace.log_path.exists():
            workspace.log(f"gb_workspace: {error if isinstance(error, WorkspaceError) else 'stopped'}")
            print(f"gb_workspace: what was done so far holds: the same command again goes on from here. "
                  f"{workspace.log_path} has every command and what it said", file=sys.stderr)
        return 1
    took = time.monotonic() - started
    if workspace.ran:
        if workspace.held:
            workspace.say(f"\nDone before, and as it was: {', '.join(dict.fromkeys(workspace.held))}")
        workspace.say(f"\n{workspace.dir}: done in {took:.0f} s. {workspace.dir / 'README.txt'} says what is "
                      f"there, what was found, and how to build and play an edit.")
    else:
        workspace.say(f"{workspace.dir}: nothing to do. Everything in it is made from what was given, and "
                      f"{workspace.dir / 'README.txt'} says what is there.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
