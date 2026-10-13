#!/usr/bin/env python3
"""Having the game unpack streams nobody played as far as: `gb_data.py unpack` is this.

A packed picture gets into a data tree when the game's own routine unpacks it
while a harvest is kept (gb_unpacked.py), so the tree holds what somebody
played as far as. The routine is there for all of them, though: called with
another stream's bank and address, it unpacks that one. This has the game do
so, once for each stream of a list, and writes a harvest for each, which
`gb_data.py unpacked` takes like any other.

The call is made by the game itself (`--unpacked-call`, gbrt_unpacked.h): it
loads a save state, sets the registers and the bytes of RAM the stream's bank
and address are taken from, enters the function, and stops as the routine
returns, when what it made has been kept. Nothing depends on where the
function would have gone back to, and nothing about the save state's file is
known here.

What a game needs for that is declared with its unpacker, in the tree's
`unpackers.json`. With nothing more declared the routine itself is entered,
with the stream's bank and address put where its `stream` says they are
found. `call` says otherwise, each part of it only where it is needed:

    "call": {"function": {"bank": 0, "address": 14059},
             "stream": {"bank": {"register": "a"}, "address": {"register": "de"}},
             "set": [{"ram8": 53418, "value": 0}],
             "variants": [{"name": "mirrored", "set": [{"ram8": 53418, "value": 1}]}],
             "interrupts": false,
             "cycles": 8000000}

- `function`: another function to enter, which leads to the routine;
- `stream`: where that function takes the stream's bank and address;
- `set`: what else to set first, in order: {"register": "b"}, {"ram8":
  ADDRESS}, {"ram16": ADDRESS} or {"cartridge": ADDRESS} (a write to the
  cartridge, which is how a bank is mapped), each with a "value": a number,
  or {"stream": "bank"} or {"stream": "address"} for the stream's own;
- `variants`: the same stream unpacked once more for each, with more set: a
  routine that makes a mirrored picture when a byte of RAM says so;
- `interrupts`: on or off while the routine runs (as the state has them,
  unless said);
- `cycles`: how long to wait for the routine to return.

Where the streams are, three ways:

- a list written by hand, a `BANK:ADDRESS` in hexadecimal to a line;
- `tables`, declared beside `call`: tables of pointers in the ROM,
      {"bank": 14, "address": 23572, "entries": 47, "entry_size": 5, "pointer": 0, "stream_bank": 19}
  `pointer` is where in an entry the address is (two bytes, low first), and
  `stream_bank` the bank the streams are in: a number, or {"entry": OFFSET}
  for a byte of the entry. Left out, it is the table's own bank;
- walking on. Packed streams mostly lie one after another, so where one ends
  the next probably starts, and where one ends is known once it has been
  unpacked with a record kept of what was read (`--data-use`). From every
  stream that is known (the tree's, the list's, the tables') the game is
  given the byte after its last as a stream, and so on.

What walking finds is a guess, and an unpacker makes something of nearly any
bytes. A guess is dropped, and its chain ends, where the routine does not
return, where what it made cannot be drawn the way the declaration says,
where it read on past the start of a stream that is known or out of its
bank, where only padding is left, and where a record of play in the tree
shows the bytes read by an instruction that reads no known stream. A chain
that arrives exactly at the start of another known stream is borne out, and
its harvests are written with the rest. The harvests of a chain that does
not are written apart, in `guessed`, to be looked at before they are taken.
"""
from __future__ import annotations

import bisect
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import shutil
import subprocess

import gb_discover
import gb_unpacked

REPORT_SCHEMA = "gbrecomp.unpack-report"
USE_SCHEMA = "gbrt.data-use"
BANK = 0x4000
FRAME = 70224                   # cycles in a frame
CYCLES = 41943040               # what the game waits for a routine unless told: ten seconds of its time
REGISTERS_8 = "abcdehl"
REGISTERS_16 = ("bc", "de", "hl")
SET_PLACES = ("register", "ram8", "ram16", "cartridge")
CALL_FIELDS = {"function", "stream", "set", "variants", "interrupts", "cycles"}
TABLE_FIELDS = {"bank", "address", "entries", "entry_size", "pointer", "stream_bank"}
PADDING = 16                    # as many equal bytes where a stream would start are padding, not a stream
GUESSED = "guessed"             # the folder for harvests of chains that were not borne out
CALLED = re.compile(r"\[UNPACKED\] call of [0-9A-F]+:[0-9A-F]+: (.*)")


class UnpackError(Exception):
    pass


def where(stream: tuple[int, int]) -> str:
    return f"{stream[0]:02X}:{stream[1]:04X}"


def times(count: int, one: str, many: str | None = None) -> str:
    return f"{count:,} {one if count == 1 else many or one + 's'}"


def in_rom(bank, address) -> bool:
    return type(bank) is int and type(address) is int and 0 <= address < 0x8000 and 0 <= bank < 0x200 and \
        (address < BANK) == (bank == 0)


# ---------------------------------------------------------------------------
# The declaration: `call` and `tables` of an unpacker


def check_sets(sets, what: str) -> None:
    if not isinstance(sets, list):
        raise UnpackError(f"{what} is a list of things to set")
    for item in sets:
        places = [key for key in item if key in SET_PLACES] if isinstance(item, dict) else []
        if len(places) != 1 or set(item) != {places[0], "value"}:
            raise UnpackError(f"{what} has {json.dumps(item)}: a thing to set is one of {{\"register\": \"b\"}}, "
                              f"{{\"ram8\": ADDRESS}}, {{\"ram16\": ADDRESS}} and {{\"cartridge\": ADDRESS}}, with a "
                              f"\"value\"")
        place, target, value = places[0], item[places[0]], item["value"]
        wide = place == "ram16" or (place == "register" and target in REGISTERS_16)
        if place == "register":
            good = target in gb_unpacked.REGISTERS
        else:
            low, high = {"ram8": (0x8000, 0xFFFE), "ram16": (0x8000, 0xFFFD), "cartridge": (0, 0x7FFF)}[place]
            good = type(target) is int and low <= target <= high
        if not good:
            raise UnpackError(f"{what} has {json.dumps(item)}: that is no register, and no address of its kind")
        if isinstance(value, dict):
            if value not in ({"stream": "bank"}, {"stream": "address"}):
                raise UnpackError(f"{what} has {json.dumps(item)}: a value is a number, or {{\"stream\": \"bank\"}} "
                                  f"or {{\"stream\": \"address\"}}")
            if value["stream"] == "address" and not wide:
                raise UnpackError(f"{what} has {json.dumps(item)}: an address does not fit in one byte")
        elif type(value) is not int or not 0 <= value <= (0xFFFF if wide else 0xFF):
            raise UnpackError(f"{what} has {json.dumps(item)}: its value does not fit")


def check_declared(unpacker: dict, path) -> None:
    """The parts of an unpacker's declaration that are for `unpack`:
    gb_unpacked.load_declaration has checked the rest."""
    name = unpacker["name"]
    call = unpacker.get("call", {})
    if not isinstance(call, dict) or set(call) - CALL_FIELDS:
        odd = sorted(set(call) - CALL_FIELDS)[0] if isinstance(call, dict) else "call"
        raise UnpackError(f"{path}: 'call' of {name} has no such field as '{odd}'")
    if "function" in call:
        function = call["function"]
        if not isinstance(function, dict) or set(function) != {"bank", "address"} or \
                not in_rom(function["bank"], function["address"]):
            raise UnpackError(f"{path}: 'function' in 'call' of {name} is the bank and address of a function in "
                              f"the ROM")
    if "stream" in call:
        stream = call["stream"]
        if not isinstance(stream, dict) or set(stream) != {"bank", "address"}:
            raise UnpackError(f"{path}: 'stream' in 'call' of {name} is where the function takes the stream's bank "
                              f"and its address")
        try:
            gb_unpacked.expression(stream["bank"], f"the stream's bank in 'call' of {name}")
            gb_unpacked.expression(stream["address"], f"the stream's address in 'call' of {name}")
        except gb_unpacked.UnpackedError as error:
            raise UnpackError(f"{path}: {error}") from error
    try:
        check_sets(call.get("set", []), f"'set' in 'call' of {name}")
        variants = call.get("variants", [])
        if not isinstance(variants, list):
            raise UnpackError(f"'variants' in 'call' of {name} is a list")
        names = set()
        for variant in variants:
            if not isinstance(variant, dict) or set(variant) != {"name", "set"} or \
                    not isinstance(variant["name"], str) or not gb_unpacked.NAME.fullmatch(variant["name"]) or \
                    variant["name"] in names:
                raise UnpackError(f"a variant in 'call' of {name} is a name of its own (letters, digits, _ and -) "
                                  f"and a 'set'")
            names.add(variant["name"])
            check_sets(variant["set"], f"'set' of the variant {variant['name']} of {name}")
    except UnpackError as error:
        raise UnpackError(f"{path}: {error}") from error
    if type(call.get("interrupts", True)) is not bool:
        raise UnpackError(f"{path}: 'interrupts' in 'call' of {name} is true or false")
    if type(call.get("cycles", 1)) is not int or not 1 <= call.get("cycles", 1) <= 1 << 40:
        raise UnpackError(f"{path}: 'cycles' in 'call' of {name} is how many cycles to wait for the routine")
    tables = unpacker.get("tables", [])
    if not isinstance(tables, list):
        raise UnpackError(f"{path}: 'tables' of {name} is a list of tables of pointers")
    for table in tables:
        if not isinstance(table, dict) or set(table) - TABLE_FIELDS or \
                not {"bank", "address", "entries"} <= set(table):
            raise UnpackError(f"{path}: a table of {name} is 'bank', 'address' and 'entries', and if need be "
                              f"'entry_size', 'pointer' and 'stream_bank'")
        size, pointer, bank = table.get("entry_size", 2), table.get("pointer", 0), table.get("stream_bank", 0)
        if not in_rom(table["bank"], table["address"]) or type(table["entries"]) is not int or \
                type(size) is not int or type(pointer) is not int or table["entries"] < 1 or \
                not 0 <= pointer <= size - 2:
            raise UnpackError(f"{path}: a table of {name} is not in the ROM, or has no entries, or its pointer does "
                              f"not lie in an entry")
        if isinstance(bank, dict):
            if set(bank) != {"entry"} or type(bank["entry"]) is not int or not 0 <= bank["entry"] < size:
                raise UnpackError(f"{path}: 'stream_bank' of a table of {name} is a bank, or {{\"entry\": OFFSET}} "
                                  f"for a byte of the entry")
        elif type(bank) is not int or not 0 <= bank < 0x200:
            raise UnpackError(f"{path}: 'stream_bank' of a table of {name} is a bank, or {{\"entry\": OFFSET}} for "
                              f"a byte of the entry")


def variants_of(unpacker: dict) -> list[dict | None]:
    """The ways a stream is unpacked: as it is, and once more for each variant declared."""
    return [None] + list(unpacker.get("call", {}).get("variants", ()))


def call_argument(unpacker: dict, stream: tuple[int, int], variant: dict | None = None) -> str:
    """What the game is given with --unpacked-call to have this stream unpacked."""
    call = unpacker.get("call", {})
    name = unpacker["name"]
    function = call.get("function", unpacker["function"])
    takes = call.get("stream", unpacker["stream"])
    sets = list(call.get("set", ())) + list(variant["set"] if variant else ())
    items = [f"{function['bank']:02X}:{function['address']:04X}"]

    def put(kind: int, target: int, value: int, wide: bool, what: str) -> None:
        if kind == 1 and not wide:
            items.append(f"{REGISTERS_8[target]}={value:02X}")
        elif kind == 2:
            items.append(f"{REGISTERS_16[target]}={value:04X}")
        elif kind == 3 and not wide:
            items.append(f"{target:04X}={value:02X}")
        elif kind == 4:
            items.append(f"{target:04X}={value & 0xFF:02X}")
            items.append(f"{target + 1:04X}={value >> 8:02X}")
        else:
            raise UnpackError(f"{what} of {name} is taken from one byte, and {value:X} does not fit in one")

    for part, value in zip(("bank", "address"), stream):
        kind, target = gb_unpacked.expression(takes[part], f"the stream's {part}")
        what = f"the stream's {part}"
        if kind == 0:
            if target != value:
                raise UnpackError(f"{what} of {name} is declared as {target:X}, so {where(stream)} cannot be given "
                                  f"to it: say in 'call' where a function takes it")
        elif kind == 5:
            if not any("cartridge" in item and item["value"] == {"stream": "bank"} for item in sets):
                raise UnpackError(f"{name} takes its stream from the bank that is mapped: say in 'call' how this "
                                  f"game maps one, as a 'set' of {{\"cartridge\": ADDRESS, \"value\": {{\"stream\": "
                                  f"\"bank\"}}}}")
        else:
            put(kind, target, value, part == "address" or value > 0xFF, what)
    for item in sets:
        place = next(key for key in item if key in SET_PLACES)
        value = item["value"]
        if isinstance(value, dict):
            value = stream[0] if value["stream"] == "bank" else stream[1]
        what = f"a 'set' in 'call'"
        if place == "register":
            put(*gb_unpacked.REGISTERS[item[place]], value, value > 0xFF, what)
        elif place == "ram16":
            put(4, item[place], value, True, what)
        else:
            put(3, item[place], value, value > 0xFF, what)
    if "interrupts" in call:
        items.append(f"ime={int(call['interrupts'])}")
    return ",".join(items)


# ---------------------------------------------------------------------------
# Where the streams are


def listed_streams(path: Path, rom_size: int) -> list[tuple[int, int]]:
    """A list written by hand: a BANK:ADDRESS in hexadecimal to a line, and
    anything after a # or a ; is a comment."""
    try:
        text = Path(path).read_text(encoding="utf-8-sig")
    except (OSError, ValueError) as error:
        raise UnpackError(f"cannot read the list {path}: {error}") from error
    streams = []
    for number, line in enumerate(text.splitlines(), 1):
        word = re.split(r"[#;]", line, maxsplit=1)[0].strip()
        if not word:
            continue
        found = re.fullmatch(r"([0-9A-Fa-f]{1,3}):([0-9A-Fa-f]{1,4})", word)
        stream = (int(found.group(1), 16), int(found.group(2), 16)) if found else (0, -1)
        offset = gb_unpacked.stream_offset(*stream) if in_rom(*stream) else None
        if offset is None or offset >= rom_size:
            raise UnpackError(f"{path}, line {number}: '{word}' is not the BANK:ADDRESS of a byte of the ROM, in "
                              f"hexadecimal")
        streams.append(stream)
    return streams


def tabled_streams(rom: bytes, table: dict) -> tuple[list[tuple[int, int]], int]:
    """The streams a table of pointers names, and how many of its entries point at nothing in the ROM."""
    size, pointer = table.get("entry_size", 2), table.get("pointer", 0)
    start = gb_unpacked.stream_offset(table["bank"], table["address"])
    if start + table["entries"] * size > len(rom) or \
            table["address"] + table["entries"] * size > (BANK if table["bank"] == 0 else 2 * BANK):
        raise UnpackError(f"the table at {where((table['bank'], table['address']))} does not lie in its bank with "
                          f"{table['entries']} entries of {size} bytes")
    streams, nowhere = [], 0
    for index in range(table["entries"]):
        entry = rom[start + index * size:start + (index + 1) * size]
        address = entry[pointer] | (entry[pointer + 1] << 8)
        bank = table.get("stream_bank", table["bank"])
        if isinstance(bank, dict):
            bank = entry[bank["entry"]]
        if address < BANK:
            bank = 0
        offset = gb_unpacked.stream_offset(bank, address) if in_rom(bank, address) else None
        if offset is None or offset >= len(rom):
            nowhere += 1
        else:
            streams.append((bank, address))
    return streams, nowhere


# ---------------------------------------------------------------------------
# The game, called


def made_with_sound(state: Path) -> bool:
    """Whether a save state has the sound hardware in it. One made by a game run with
    --no-audio has not, and a game loads a state only when it is run as the one that made it
    was: with the sound hardware, or without."""
    try:
        with Path(state).open("rb") as file:
            fields = gb_discover.HEADER.unpack(file.read(gb_discover.HEADER.size))
    except (OSError, ValueError):       # struct.error is a ValueError: a file too short to be a state
        return False
    return fields[0] == gb_discover.SAVESTATE_MAGIC and fields[-1] != 0


class Game:
    """A built game, a save state to call it in, and one unpacker of the tree."""

    def __init__(self, executable: Path, state: Path | None, unpacker: dict, manifest: dict, work: Path) -> None:
        self.executable, self.state, self.unpacker, self.work = Path(executable), state, unpacker, work
        self.cycles = unpacker.get("call", {}).get("cycles", CYCLES)
        # Without the sound hardware, as it always was, unless the state has it.
        self.sound = state is not None and made_with_sound(state)
        for folder in ("saves", "harvests", "records"):
            shutil.rmtree(work / folder, ignore_errors=True)
            (work / folder).mkdir(parents=True)
        # A pack that only watches, and only this routine: the game stops at the first return it sees.
        self.pack = work / "watch.gbup"
        self.pack.write_bytes(gb_unpacked.pack_bytes(manifest["rom"]["size"], manifest["rom"]["sha256"],
                                                     [unpacker], []))

    def unpack(self, stream: tuple[int, int], variant: dict | None = None, record: bool = False) -> dict:
        """Have the game unpack one stream. What comes back says how it went:
        `harvest` (the file) and `sha256` where the routine made something of
        it, `why` where it did not, and `fatal` where the game cannot be
        called at all."""
        tag = f"{stream[0]:02X}_{stream[1]:04X}" + (f".{variant['name']}" if variant else "")
        saves, harvest, use = self.work / "saves" / tag, self.work / "harvests" / f"{tag}.json", \
            self.work / "records" / f"{tag}.json"
        saves.mkdir(exist_ok=True)
        command = [str(self.executable), "--headless", *([] if self.sound else ["--no-audio"]), "--save-dir",
                   str(saves), "--limit-frames",
                   str(self.cycles // FRAME + 2), "--unpacked", str(self.pack), "--unpacked-harvest", str(harvest),
                   "--unpacked-call", call_argument(self.unpacker, stream, variant), "--unpacked-call-cycles",
                   str(self.cycles)]
        if self.state is not None:
            command += ["--load-state-file", str(self.state)]
        if record:
            command += ["--data-use", str(use)]
        result = {"variant": variant["name"] if variant else ""}
        try:
            done = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=120)
        except subprocess.TimeoutExpired:
            return {**result, "why": "the game did not stop"}
        except OSError as error:
            return {**result, "why": f"the game could not be started: {error}", "fatal": True}
        finally:
            shutil.rmtree(saves, ignore_errors=True)
        said = (done.stdout + done.stderr).splitlines()
        called = next((found.group(1) for found in map(CALLED.search, said) if found), None)
        if called is None:
            last = next((line for line in reversed(said) if line.strip()), "it said nothing")
            return {**result, "fatal": True,
                    "why": (f"the game stopped: {last.strip()}" if done.returncode else
                            "the game made no call: it was generated before --unpacked-call was there, and has to "
                            "be generated and built again")}
        found = re.search(r"returned after (\d+) cycles", called)
        if not found:
            return {**result, "why": called}
        result["cycles"] = int(found.group(1))
        try:
            made = json.loads(harvest.read_text(encoding="utf-8"))["made"]
        except (OSError, ValueError, KeyError) as error:
            return {**result, "why": f"the game wrote no harvest that can be read: {error}"}
        mine = [thing for thing in made if (thing["bank"], thing["address"]) == stream]
        if len(mine) != 1:
            other = ", ".join(sorted({where((thing["bank"], thing["address"])) for thing in made}))
            return {**result, "why": f"the routine was given {other} and not {where(stream)}: 'call' does not put "
                                     f"the stream where it is taken from" if other else
                                     "the routine returned and what it made was not kept: it does not lie in RAM "
                                     "where 'made' says"}
        if record:
            try:
                result["read"] = read_runs(use)
            except (OSError, ValueError, KeyError, TypeError) as error:
                return {**result, "why": f"the game wrote no record of what it read that can be read ({error}): "
                                         f"walking on needs a game that has --data-use"}
        result.update(harvest=harvest, sha256=mine[0]["sha256"], made=bytes.fromhex(mine[0]["bytes"]))
        return result


def read_runs(path: Path) -> list[tuple[int, int, tuple[int, int]]]:
    """The ROM bytes a record says were read, as (first offset, one past the
    last, the instruction that read them first), in order."""
    record = json.loads(Path(path).read_text(encoding="utf-8"))
    if record.get("schema") != USE_SCHEMA or record.get("version") != 1:
        raise ValueError(f"not a version 1 {USE_SCHEMA} record")
    return sorted((entry["offset"], entry["offset"] + entry["size"], tuple(entry["read_at"]))
                  for entry in record["ranges"])


def read_from(runs: list[tuple[int, int, tuple[int, int]]], offset: int) -> tuple[int, set]:
    """One past the last byte of the run of bytes read from `offset` on, and
    the instructions that read them. The offset itself if that byte was not read."""
    end, readers = offset, set()
    for first, last, reader in runs:
        if first <= end < last:
            end = last
            readers.add(reader)
    return end, readers


# ---------------------------------------------------------------------------
# Walking on


class Walk:
    """Streams guessed at from where known ones end."""

    def __init__(self, game: Game, rom: bytes, manifest: dict, known: dict, results: dict, workers: int,
                 limit: int) -> None:
        self.game, self.rom, self.known, self.workers, self.limit = game, rom, known, workers, limit
        self.regions = manifest["regions"]
        self.region_starts = [region["offset"] for region in self.regions]
        self.known_offsets = sorted(gb_unpacked.stream_offset(*stream) for stream in known)
        # What a record of play says of the ROM's bytes: who read each first.
        self.played = sorted((region["offset"] + run["offset"], region["offset"] + run["offset"] + run["size"],
                              tuple(run["read_at"]))
                             for region in self.regions for run in region.get("seen", ()))
        self.results = dict(results)        # every stream that was unpacked, known or guessed
        self.after: dict[tuple, tuple] = {}  # a guess -> the stream whose end it starts at
        self.next: dict[tuple, tuple] = {}   # a stream -> the stream that starts at its end
        self.ends: list[dict] = []           # where a chain went no further, and why
        # The instructions that read the known streams, in play and when called.
        self.readers: set = set()
        for stream, result in results.items():
            if "end" in result:
                self.readers |= result["readers"] | self.played_readers(gb_unpacked.stream_offset(*stream),
                                                                        result["end"])

    def played_readers(self, first: int, end: int) -> set:
        return {reader for start, stop, reader in self.played if start < end and first < stop}

    def in_code(self, first: int, end: int) -> int:
        """How many of those bytes lie in files of code."""
        count = 0
        index = max(bisect.bisect_right(self.region_starts, first) - 1, 0)
        while index < len(self.regions) and self.regions[index]["offset"] < end:
            region = self.regions[index]
            if region["kind"] != "data":
                count += min(end, region["offset"] + region["size"]) - max(first, region["offset"])
            index += 1
        return count

    def follows(self, stream: tuple[int, int]) -> tuple[tuple[int, int] | None, str]:
        """The stream that would start where this one ends, or why there is none to try."""
        end = self.results[stream]["end"]
        bank_end = (end // BANK + 1) * BANK
        if end % BANK == 0 or end >= len(self.rom):
            return None, "its bank ends there"
        rest = self.rom[end:bank_end]
        if rest == rest[:1] * len(rest):
            return None, f"only padding is left in its bank ({len(rest)} bytes of {rest[0]:02X})"
        if rest[:PADDING] == rest[:1] * PADDING:
            return None, f"padding follows it ({PADDING} bytes of {rest[0]:02X} or more)"
        bank = end // BANK
        return (bank, end if bank == 0 else BANK + end % BANK), ""

    def refuse(self, guess: tuple[int, int], result: dict) -> str:
        """Why what the game made of a guess is not taken for a stream, or nothing."""
        if "why" in result:
            return result["why"]
        first = gb_unpacked.stream_offset(*guess)
        end, readers = read_from(result["read"], first)
        if end == first:
            return "the routine did not read its first byte"
        if (end - 1) // BANK != first // BANK:
            return "the routine read on out of its bank"
        over = bisect.bisect_right(self.known_offsets, first)
        if over < len(self.known_offsets) and self.known_offsets[over] < end:
            offset = self.known_offsets[over]
            bank = offset // BANK
            return f"the routine read on past the start of a known stream, " \
                   f"{where((bank, offset if bank == 0 else BANK + offset % BANK))}"
        try:
            gb_unpacked.resolve_shape(self.game.unpacker, self.rom, *guess)
        except gb_unpacked.UnpackedError as error:
            return f"what was made cannot be drawn the way it is declared ({str(error).split(': ', 1)[-1]})"
        others = self.played_readers(first, end) - self.readers
        if others:
            return f"a record of play has those bytes read at {where(min(others))}, which reads no known stream"
        result.update(end=end, readers=readers)
        return ""

    def run(self) -> None:
        tried: set = set()
        frontier = []

        def on_from(stream: tuple[int, int]) -> None:
            guess, why = self.follows(stream)
            if guess is None:
                self.ends.append({"after": stream, "why": why})
            elif guess in self.known or guess in self.results:
                self.next[stream] = guess
            elif guess in tried:
                self.ends.append({"after": stream, "at": guess, "why": "that was tried, and dropped"})
            else:
                tried.add(guess)
                frontier.append((guess, stream))

        for stream in sorted(self.results):
            if "end" in self.results[stream]:
                on_from(stream)
        guesses = 0
        while frontier:
            batch, frontier = frontier[:max(self.limit - guesses, 0)], []
            if not batch:
                self.ends.append({"why": f"the walk was stopped after {times(guesses, 'guess', 'guesses')}"})
                break
            guesses += len(batch)
            with ThreadPoolExecutor(self.workers) as pool:
                made = list(pool.map(lambda job: self.game.unpack(job[0], record=True), batch))
            for (guess, stream), result in zip(batch, made):
                why = self.refuse(guess, result)
                if why:
                    self.ends.append({"after": stream, "at": guess, "why": why})
                    continue
                self.results[guess] = result
                self.after[guess] = stream
                self.next[stream] = guess
                on_from(guess)

    def meets(self, guess: tuple[int, int]) -> tuple[int, int] | None:
        """The known stream a guess's chain arrives at, if it does."""
        stream = guess
        while stream in self.after:
            stream = self.next.get(stream)
        return stream if stream in self.known else None


# ---------------------------------------------------------------------------
# The command


def run(args, manifest: dict, rom: bytes, unpackers: list[dict], draw) -> int:
    """`gb_data.py unpack`. `draw` makes a PNG of what a routine made, the way `unpacked` draws it."""
    by_name = {unpacker["name"]: unpacker for unpacker in unpackers}
    if args.unpacker is None and len(unpackers) > 1:
        raise UnpackError(f"{gb_unpacked.DECLARATION} declares {len(unpackers)} unpackers: say which with "
                          f"--unpacker ({', '.join(by_name)})")
    if args.unpacker is not None and args.unpacker not in by_name:
        raise UnpackError(f"{gb_unpacked.DECLARATION} declares no unpacker '{args.unpacker}'")
    unpacker = by_name[args.unpacker] if args.unpacker is not None else unpackers[0]
    name = unpacker["name"]
    if not (args.streams or args.tables or args.walk):
        raise UnpackError("say where the streams are: --streams FILE (a list), --tables (the tables declared), or "
                          "--walk (on from where the known ones end)")
    for path, what in ((args.game, "game"), (args.state, "save state")):
        if path is not None and not Path(path).is_file():
            raise UnpackError(f"there is no {what} at {path}")

    # Which streams, and how each is known. The first way a stream is known is the one it is listed by.
    known: dict[tuple[int, int], str] = {}
    for path in args.streams:
        for stream in listed_streams(path, len(rom)):
            known.setdefault(stream, "list")
    nowhere = 0
    if args.tables:
        if not unpacker.get("tables"):
            raise UnpackError(f"{gb_unpacked.DECLARATION} declares no 'tables' for {name}")
        for table in unpacker["tables"]:
            streams, missed = tabled_streams(rom, table)
            nowhere += missed
            for stream in streams:
                known.setdefault(stream, "table")
    if args.walk:
        for thing in manifest.get("unpacked", ()):
            if thing["unpacker"] == name:
                known.setdefault((thing["bank"], thing["address"]), "tree")
    if not known:
        raise UnpackError("there is no stream to start from: the tree has nothing unpacked by "
                          f"{name}, and no list or table gave one")
    call_argument(unpacker, next(iter(known)))      # a declaration that cannot be called says so before anything runs

    out = Path(args.output)
    work = out / "run"
    try:
        out.mkdir(parents=True, exist_ok=True)
        game = Game(args.game, args.state, unpacker, manifest, work)
    except OSError as error:
        raise UnpackError(f"cannot write to {out}: {error}") from error
    workers = max(1, min(args.workers, 8))
    variants = variants_of(unpacker)
    function = unpacker.get("call", {}).get("function", unpacker["function"])
    print(f"{name}: calling {where((function['bank'], function['address']))} in "
          f"{args.state if args.state is not None else 'the game as it is switched on'}, once for each stream"
          + (f" and each of {times(len(variants) - 1, 'variant')}" if len(variants) > 1 else ""))

    # The first by itself: a game that cannot be called at all says so once.
    order = sorted(known)
    first = game.unpack(order[0], record=args.walk)
    if first.get("fatal"):
        raise UnpackError(f"{where(order[0])}: {first['why']}")
    with ThreadPoolExecutor(workers) as pool:
        rest = list(pool.map(lambda stream: game.unpack(stream, record=args.walk), order[1:]))
    results = dict(zip(order, [first] + rest))
    walk = None
    if args.walk:
        for stream, result in results.items():
            if "read" in result:
                end, readers = read_from(result["read"], gb_unpacked.stream_offset(*stream))
                if end > gb_unpacked.stream_offset(*stream):
                    result.update(end=end, readers=readers)
        walk = Walk(game, rom, manifest, known, {stream: result for stream, result in results.items()
                                                 if "why" not in result}, workers, args.walk_limit)
        walk.run()
        results.update(walk.results)

    # The other ways of unpacking each, where the declaration has variants.
    jobs = [(stream, variant) for stream in sorted(results) if "why" not in results[stream]
            for variant in variants[1:]]
    with ThreadPoolExecutor(workers) as pool:
        more = list(pool.map(lambda job: game.unpack(*job), jobs))
    by_stream: dict[tuple, list[dict]] = {stream: [result] for stream, result in results.items()}
    for (stream, _), result in zip(jobs, more):
        by_stream[stream].append(result)

    # The harvests, where `unpacked` finds them: those of a chain that was not borne out apart.
    report = {"schema": REPORT_SCHEMA, "version": 1, "unpacker": name, "game": str(args.game),
              "state": str(args.state) if args.state is not None else None, "streams": [], "ends": []}
    counts = {"list": 0, "table": 0, "tree": 0, "met": 0, "open": 0}
    failed, harvests, guessed, open_in_code, open_bytes, all_code = [], 0, 0, 0, 0, []
    for stream in sorted(by_stream):
        found = known.get(stream, "walk")
        entry: dict = {"bank": stream[0], "address": stream[1], "found": found, "harvests": []}
        folder = out
        base = by_stream[stream][0]
        if "end" in base:
            entry["read"] = base["end"] - gb_unpacked.stream_offset(*stream)
        if found == "walk":
            met = walk.meets(stream)
            entry["after"] = list(walk.after[stream])
            entry["in_code"] = walk.in_code(gb_unpacked.stream_offset(*stream), base["end"])
            if met is not None:
                entry["meets"] = list(met)
                counts["met"] += 1
            else:
                folder = out / GUESSED
                folder.mkdir(exist_ok=True)
                counts["open"] += 1
                open_in_code += entry["in_code"]
                open_bytes += entry["read"]
                if entry["in_code"] == entry["read"]:
                    all_code.append(where(stream))
                # A picture of it beside its harvest: a guess is to be looked at.
                shape = gb_unpacked.resolve_shape(unpacker, rom, *stream)
                if shape is not None:
                    picture = folder / f"{stream[0]:02X}_{stream[1]:04X}.png"
                    picture.write_bytes(draw(base["made"], gb_unpacked.made_sizes(unpacker), shape))
                    entry["picture"] = picture.relative_to(out).as_posix()
        elif "why" not in base:
            counts[found] += 1
        for result in by_stream[stream]:
            label = where(stream) + (f" ({result['variant']})" if result["variant"] else "")
            if "why" in result:
                entry.setdefault("problems", []).append(f"{label}: {result['why']}")
                failed.append(f"{label}: {result['why']}")
                continue
            target = folder / result["harvest"].name
            shutil.copyfile(result["harvest"], target)
            (out / GUESSED / target.name if folder == out else out / target.name).unlink(missing_ok=True)
            entry["harvests"].append(target.relative_to(out).as_posix())
            entry.setdefault("sha256", []).append(result["sha256"])
            harvests += 1
            guessed += folder != out
        report["streams"].append(entry)
    for end in walk.ends if walk else ():
        report["ends"].append({key: (list(value) if isinstance(value, tuple) else value)
                               for key, value in end.items()})
    shutil.rmtree(work / "saves", ignore_errors=True)
    shutil.rmtree(work / "harvests", ignore_errors=True)
    if not args.keep_records:
        shutil.rmtree(work / "records", ignore_errors=True)
    (work / "report.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8", newline="\n")

    asked = {source: sum(1 for value in known.values() if value == source) for source in ("list", "table", "tree")}
    said = [f"{asked[source]:,} {words}" for source, words in
            (("list", "from a list"), ("table", "from the tables"), ("tree", "the tree has")) if asked[source]]
    done = counts["list"] + counts["table"] + counts["tree"]
    print(f"  {times(len(known), 'stream')} known ({', '.join(said)}): {done:,} unpacked")
    if nowhere:
        print(f"  {times(nowhere, 'entry', 'entries')} of the tables {'points' if nowhere == 1 else 'point'} at "
              f"nothing in the ROM")
    if walk is not None:
        walked = counts["met"] + counts["open"]
        print(f"  walking on from where they end: {times(walked, 'more stream')} guessed at")
        if counts["met"]:
            print(f"    {counts['met']:,} in chains that arrive exactly at the start of another known stream, which "
                  f"bears them out: with the rest")
        if counts["open"]:
            print(f"    {counts['open']:,} in chains that do not: guesses, in {out / GUESSED}, to be looked at before "
                  f"they are taken ({open_in_code:,} of their {open_bytes:,} bytes lie in files of code)")
        if all_code:
            print(f"    {len(all_code):,} of those lie wholly in files of code, where the analysis found instructions "
                  f"and a stream seldom lies whole: {', '.join(all_code[:8])}"
                  + (f" and {len(all_code) - 8} more" if len(all_code) > 8 else ""))
        reasons: dict[str, int] = {}
        for end in walk.ends:
            why = re.sub(r"\b[0-9A-F]{2,3}:[0-9A-F]{4}\b", "BB:AAAA", end["why"])
            why = re.sub(r"\(.*\)", "", why).strip()
            reasons[why] = reasons.get(why, 0) + 1
        for why, count in sorted(reasons.items(), key=lambda item: (-item[1], item[0])):
            print(f"    {times(count, 'chain')} went no further: {why}")
    print(f"  {times(harvests - guessed, 'harvest')} in {out}, for `unpacked` to take"
          + (f", and {guessed:,} of guesses in {out / GUESSED}" if guessed else "")
          + f". What was done to each stream is in {work / 'report.json'}")
    for line in failed[:12]:
        print(f"  not unpacked: {line}")
    if len(failed) > 12:
        print(f"  and {len(failed) - 12} more not unpacked")
    return 1 if failed else 0
